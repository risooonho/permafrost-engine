/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2017-2020 Eduard Permyakov 
 *
 *  Permafrost Engine is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Permafrost Engine is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 * 
 *  Linking this software statically or dynamically with other modules is making 
 *  a combined work based on this software. Thus, the terms and conditions of 
 *  the GNU General Public License cover the whole combination. 
 *  
 *  As a special exception, the copyright holders of Permafrost Engine give 
 *  you permission to link Permafrost Engine with independent modules to produce 
 *  an executable, regardless of the license terms of these independent 
 *  modules, and to copy and distribute the resulting executable under 
 *  terms of your choice, provided that you also meet, for each linked 
 *  independent module, the terms and conditions of the license of that 
 *  module. An independent module is a module which is not derived from 
 *  or based on Permafrost Engine. If you modify Permafrost Engine, you may 
 *  extend this exception to your version of Permafrost Engine, but you are not 
 *  obliged to do so. If you do not wish to do so, delete this exception 
 *  statement from your version.
 *
 */

#include "public/game.h"
#include "gamestate.h"
#include "selection.h"
#include "timer_events.h"
#include "movement.h"
#include "game_private.h"
#include "combat.h" 
#include "clearpath.h"
#include "position.h"
#include "../render/public/render.h"
#include "../render/public/render_ctrl.h"
#include "../anim/public/anim.h"
#include "../map/public/map.h"
#include "../entity.h"
#include "../camera.h"
#include "../cam_control.h"
#include "../asset_load.h"
#include "../event.h"
#include "../config.h"
#include "../collision.h"
#include "../settings.h"
#include "../main.h"
#include "../ui.h"

#include <assert.h> 


#define CAM_HEIGHT          175.0f
#define CAM_TILT_UP_DEGREES 25.0f
#define CAM_SPEED           0.20f

#define ACTIVE_CAM          (s_gs.cameras[s_gs.active_cam_idx])
#define ARR_SIZE(a)         (sizeof(a)/sizeof(a[0]))

VEC_IMPL(extern, obb, struct obb)
__KHASH_IMPL(entity, extern, khint32_t, struct entity*, 1, kh_int_hash_func, kh_int_hash_equal)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static struct gamestate s_gs;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static vec2_t g_default_minimap_pos(void)
{
    struct sval res = (struct sval){ 
        .type = ST_TYPE_VEC2, 
        .as_vec2 = (vec2_t){1920, 1080}
    };
    const float PAD = 10.0f;
    int size = 256;
    
    return (vec2_t) {
        (size + 2*MINIMAP_BORDER_WIDTH) / cos(M_PI/4.0f)/2 + PAD,
        res.as_vec2.y - (size + 2*MINIMAP_BORDER_WIDTH) / cos(M_PI/4.0f)/2 - PAD,
    };
}

static void g_reset_camera(struct camera *cam)
{
    Camera_SetPitchAndYaw(cam, -(90.0f - CAM_TILT_UP_DEGREES), 90.0f + 45.0f);
    Camera_SetPos(cam, (vec3_t){ 0.0f, CAM_HEIGHT, 0.0f }); 
}

static void g_reset(void)
{
    G_Sel_Clear();

    uint32_t key;
    struct entity *curr;
    (void)key;

    kh_foreach(s_gs.active, key, curr, {
        G_SafeFree(curr);
    });

    kh_clear(entity, s_gs.active);
    kh_clear(entity, s_gs.dynamic);
    vec_pentity_reset(&s_gs.visible);
    vec_pentity_reset(&s_gs.light_visible);
    vec_obb_reset(&s_gs.visible_obbs);

    if(s_gs.map) {

        M_Raycast_Uninstall();
        M_FreeMinimap(s_gs.map);
        AL_MapFree(s_gs.map);
        G_Move_Shutdown();
        G_Combat_Shutdown();
        G_ClearPath_Shutdown();
        G_Pos_Shutdown();

        s_gs.map = NULL;
        free((void*)s_gs.prev_tick_map);
        s_gs.prev_tick_map = NULL;
    }

    for(int i = 0; i < NUM_CAMERAS; i++) {
        g_reset_camera(s_gs.cameras[i]);
    }
    G_ActivateCamera(0, CAM_MODE_RTS);

    s_gs.num_factions = 0;
}

static bool g_init_cameras(void) 
{
    for(int i = 0; i < NUM_CAMERAS; i++) {
    
        s_gs.cameras[i] = Camera_New();
        if(!s_gs.cameras[i]) {
            return false;
        }

        Camera_SetSpeed(s_gs.cameras[i], CAM_SPEED);
        Camera_SetSens (s_gs.cameras[i], 0.05f);
        g_reset_camera(s_gs.cameras[i]);
    }
    return true;
}

static void g_init_map(void)
{
    M_CenterAtOrigin(s_gs.map);
    M_RestrictRTSCamToMap(s_gs.map, ACTIVE_CAM);
    M_Raycast_Install(s_gs.map, ACTIVE_CAM);
    M_InitMinimap(s_gs.map, g_default_minimap_pos());
    G_Move_Init(s_gs.map);
    G_Combat_Init();
    G_ClearPath_Init(s_gs.map);
    G_Pos_Init(s_gs.map);
    N_FC_ClearAll();
    N_FC_ClearStats();
}

static void g_shadow_pass(const struct camera *cam, const struct map *map, 
                          vec_rstat_t stat_ents, vec_ranim_t anim_ents)
{
    vec3_t pos = Camera_GetPos(cam);
    vec3_t dir = Camera_GetDir(cam);

    R_PushCmd((struct rcmd){ 
        .func = R_GL_DepthPassBegin, 
        .nargs = 3,
        .args = { 
            R_PushArg(&s_gs.light_pos, sizeof(s_gs.light_pos)),
            R_PushArg(&pos, sizeof(pos)),
            R_PushArg(&dir, sizeof(dir)),
        },
    });

    if(map) {
        M_RenderVisibleMap(map, cam, true, RENDER_PASS_DEPTH);
    }

    for(int i = 0; i < vec_size(&stat_ents); i++) {
    
        struct ent_stat_rstate *curr = &vec_AT(&stat_ents, i);
        R_PushCmd((struct rcmd){
            .func = R_GL_RenderDepthMap,
            .nargs = 2,
            .args = {
                curr->render_private,
                R_PushArg(&curr->model, sizeof(curr->model)),
            },
        });
    }

    for(int i = 0; i < vec_size(&anim_ents); i++) {
    
        struct ent_anim_rstate *curr = &vec_AT(&anim_ents, i);

        mat4x4_t model, normal;
        PFM_Mat4x4_Inverse(&curr->model, &model);
        PFM_Mat4x4_Transpose(&model, &normal);

        R_PushCmd((struct rcmd){
            .func = R_GL_SetAnimUniforms,
            .nargs = 4,
            .args = {
                (void*)curr->inv_bind_pose, 
                R_PushArg(curr->curr_pose, sizeof(curr->curr_pose)),
                R_PushArg(&normal, sizeof(normal)),
                R_PushArg(&curr->njoints, sizeof(curr->njoints)),
            },
        });

        R_PushCmd((struct rcmd){
            .func = R_GL_RenderDepthMap,
            .nargs = 2,
            .args = {
                curr->render_private,
                R_PushArg(&curr->model, sizeof(curr->model)),
            },
        });
    }

    R_PushCmd((struct rcmd){ R_GL_DepthPassEnd, 0 });
}

static void g_draw_pass(const struct camera *cam, const struct map *map, 
                        bool shadows, vec_rstat_t stat_ents, vec_ranim_t anim_ents)
{
    if(map) {
        M_RenderVisibleMap(map, cam, shadows, RENDER_PASS_REGULAR);
    }

    for(int i = 0; i < vec_size(&stat_ents); i++) {
    
        struct ent_stat_rstate *curr = &vec_AT(&stat_ents, i);
        R_PushCmd((struct rcmd){
            .func = R_GL_Draw,
            .nargs = 2,
            .args = {
                curr->render_private,
                R_PushArg(&curr->model, sizeof(curr->model)),
            },
        });
    }

    for(int i = 0; i < vec_size(&anim_ents); i++) {
    
        struct ent_anim_rstate *curr = &vec_AT(&anim_ents, i);

        mat4x4_t model, normal;
        PFM_Mat4x4_Inverse(&curr->model, &model);
        PFM_Mat4x4_Transpose(&model, &normal);

        R_PushCmd((struct rcmd){
            .func = R_GL_SetAnimUniforms,
            .nargs = 4,
            .args = {
                (void*)curr->inv_bind_pose, 
                R_PushArg(curr->curr_pose, sizeof(curr->curr_pose)),
                R_PushArg(&normal, sizeof(normal)),
                R_PushArg(&curr->njoints, sizeof(curr->njoints)),
            },
        });

        R_PushCmd((struct rcmd){
            .func = R_GL_Draw,
            .nargs = 2,
            .args = {
                curr->render_private,
                R_PushArg(&curr->model, sizeof(curr->model)),
            },
        });
    }
}

static void g_render_healthbars(void)
{
    size_t max_ents = vec_size(&s_gs.visible);
    size_t num_combat_visible = 0;

    GLfloat ent_health_pc[max_ents];
    vec3_t ent_top_pos_ws[max_ents];

    for(int i = 0; i < max_ents; i++) {
    
        struct entity *curr = vec_AT(&s_gs.visible, i);

        if(!(curr->flags & ENTITY_FLAG_COMBATABLE))
            continue;

        int max_health = curr->max_hp;
        int curr_health = G_Combat_GetCurrentHP(curr);

        ent_top_pos_ws[num_combat_visible] = Entity_TopCenterPointWS(curr);
        ent_health_pc[num_combat_visible] = ((GLfloat)curr_health)/max_health;

        num_combat_visible++;
    }

    R_PushCmd((struct rcmd){
        .func = R_GL_DrawHealthbars,
        .nargs = 4,
        .args = {
            R_PushArg(&num_combat_visible, sizeof(num_combat_visible)),
            R_PushArg(ent_health_pc, sizeof(ent_health_pc)),
            R_PushArg(ent_top_pos_ws, sizeof(ent_top_pos_ws)),
            R_PushArg(ACTIVE_CAM, g_sizeof_camera),
        },
    });
}

static void g_make_draw_list(vec_pentity_t ents, vec_rstat_t *out_stat, vec_ranim_t *out_anim)
{
    for(int i = 0; i < vec_size(&ents); i++) {

        const struct entity *curr = vec_AT(&ents, i);

        mat4x4_t model;
        Entity_ModelMatrix(curr, &model);

        if(curr->flags & ENTITY_FLAG_ANIMATED) {
        
            struct ent_anim_rstate rstate = (struct ent_anim_rstate){curr->render_private, model};
            A_GetRenderState(curr, &rstate.njoints, rstate.curr_pose, &rstate.inv_bind_pose);
            vec_ranim_push(out_anim, rstate);
        }else{
        
            struct ent_stat_rstate rstate = (struct ent_stat_rstate){curr->render_private, model};
            vec_rstat_push(out_stat, rstate);
        }
    }
}

static void g_create_render_input(struct render_input *out)
{
    struct sval shadows_setting;
    ss_e status = Settings_Get("pf.video.shadows_enabled", &shadows_setting);
    assert(status == SS_OKAY);

    out->cam = ACTIVE_CAM;
    out->map = s_gs.map;
    out->shadows = shadows_setting.as_bool;

    vec_rstat_init(&out->cam_vis_stat);
    vec_ranim_init(&out->cam_vis_anim);

    vec_rstat_init(&out->light_vis_stat);
    vec_ranim_init(&out->light_vis_anim);

    g_make_draw_list(s_gs.visible, &out->cam_vis_stat, &out->cam_vis_anim);
    g_make_draw_list(s_gs.light_visible, &out->light_vis_stat, &out->light_vis_anim);

    assert(vec_size(&out->cam_vis_stat) + vec_size(&out->cam_vis_anim) == vec_size(&s_gs.visible));
    assert(vec_size(&out->light_vis_stat) + vec_size(&out->light_vis_anim) == vec_size(&s_gs.light_visible));
}

static void g_destroy_render_input(struct render_input *rinput)
{
    vec_rstat_destroy(&rinput->cam_vis_stat);
    vec_ranim_destroy(&rinput->cam_vis_anim);

    vec_rstat_destroy(&rinput->light_vis_stat);
    vec_ranim_destroy(&rinput->light_vis_anim);
}

static void *g_push_render_input(struct render_input in)
{
    struct render_input *ret = R_PushArg(&in, sizeof(in));

    ret->cam = R_PushArg(in.cam, g_sizeof_camera);

    if(in.cam_vis_stat.size) {
        ret->cam_vis_stat.array = R_PushArg(in.cam_vis_stat.array, in.cam_vis_stat.size * sizeof(struct ent_stat_rstate));
    }
    if(in.cam_vis_anim.size) {
        ret->cam_vis_anim.array = R_PushArg(in.cam_vis_anim.array, in.cam_vis_anim.size * sizeof(struct ent_anim_rstate));
    }

    if(in.light_vis_stat.size) {
        ret->light_vis_stat.array = R_PushArg(in.light_vis_stat.array, in.light_vis_stat.size * sizeof(struct ent_stat_rstate));
    }
    if(in.light_vis_anim.size) {
        ret->light_vis_anim.array = R_PushArg(in.light_vis_anim.array, in.light_vis_anim.size * sizeof(struct ent_anim_rstate));
    }

    return ret;
}

static bool bool_val_validate(const struct sval *new_val)
{
    return (new_val->type == ST_TYPE_BOOL);
}

static bool faction_id_validate(const struct sval *new_val)
{
    if(new_val->type != ST_TYPE_INT)
        return false;
    if(new_val->as_int < 0)
        return false;
    return true;
}

static void shadows_en_commit(const struct sval *new_val)
{
    bool on = new_val->as_bool;
    if(s_gs.map) {
        M_SetShadowsEnabled(s_gs.map, on);
    }

    if(!s_gs.active)
        return;

    uint32_t key;
    struct entity *curr;
    (void)key;

    kh_foreach(s_gs.active, key, curr, {

        R_PushCmd((struct rcmd){
            .func = R_GL_SetShadowsEnabled,
            .nargs = 2,
            .args = {
                curr->render_private,
                R_PushArg(&on, sizeof(on)),
            },
        });
    });
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool G_Init(void)
{
    ASSERT_IN_MAIN_THREAD();

    vec_pentity_init(&s_gs.visible);
    vec_pentity_init(&s_gs.light_visible);
    vec_obb_init(&s_gs.visible_obbs);
    vec_pentity_init(&s_gs.deleted);

    s_gs.active = kh_init(entity);
    if(!s_gs.active)
        goto fail_active;

    s_gs.dynamic = kh_init(entity);
    if(!s_gs.dynamic)
        goto fail_dynamic;

    if(!g_init_cameras())
        goto fail_cams; 

    if(!R_InitWS(&s_gs.ws[0]))
        goto fail_ws;

    if(!R_InitWS(&s_gs.ws[1])) {
        R_DestroyWS(&s_gs.ws[0]);
        goto fail_ws;
    }

    g_reset();
    G_Sel_Init();
    G_Sel_Enable();
    G_Timer_Init();
    R_PushCmd((struct rcmd){ R_GL_WaterInit, 0 });

    ss_e status;
    (void)status;

    status = Settings_Create((struct setting){
        .name = "pf.game.healthbar_mode",
        .val = (struct sval) {
            .type = ST_TYPE_BOOL,
            .as_bool = true
        },
        .prio = 0,
        .validate = bool_val_validate,
        .commit = NULL,
    });
    assert(status == SS_OKAY);

    status = Settings_Create((struct setting){
        .name = "pf.video.shadows_enabled",
        .val = (struct sval) {
            .type = ST_TYPE_BOOL,
            .as_bool = true 
        },
        .prio = 0,
        .validate = bool_val_validate,
        .commit = shadows_en_commit,
    });
    assert(status == SS_OKAY);

    status = Settings_Create((struct setting){
        .name = "pf.debug.show_navigation_cost_base",
        .val = (struct sval) {
            .type = ST_TYPE_BOOL,
            .as_bool = false
        },
        .prio = 0,
        .validate = bool_val_validate,
        .commit = NULL,
    });
    assert(status == SS_OKAY);

    status = Settings_Create((struct setting){
        .name = "pf.debug.show_last_cmd_flow_field",
        .val = (struct sval) {
            .type = ST_TYPE_BOOL,
            .as_bool = false
        },
        .prio = 0,
        .validate = bool_val_validate,
        .commit = NULL,
    });
    assert(status == SS_OKAY);

    status = Settings_Create((struct setting){
        .name = "pf.debug.show_first_sel_movestate",
        .val = (struct sval) {
            .type = ST_TYPE_BOOL,
            .as_bool = false
        },
        .prio = 0,
        .validate = bool_val_validate,
        .commit = NULL,
    });
    assert(status == SS_OKAY);

    status = Settings_Create((struct setting){
        .name = "pf.debug.show_first_sel_combined_hrvo",
        .val = (struct sval) {
            .type = ST_TYPE_BOOL,
            .as_bool = false 
        },
        .prio = 0,
        .validate = bool_val_validate,
        .commit = NULL,
    });

    status = Settings_Create((struct setting){
        .name = "pf.debug.show_enemy_seek_fields",
        .val = (struct sval) {
            .type = ST_TYPE_BOOL,
            .as_bool = false 
        },
        .prio = 0,
        .validate = bool_val_validate,
        .commit = NULL,
    });

    status = Settings_Create((struct setting){
        .name = "pf.debug.enemy_seek_fields_faction_id",
        .val = (struct sval) {
            .type = ST_TYPE_INT,
            .as_int = 0
        },
        .prio = 0,
        .validate = faction_id_validate,
        .commit = NULL,
    });

    status = Settings_Create((struct setting){
        .name = "pf.debug.show_navigation_blockers",
        .val = (struct sval) {
            .type = ST_TYPE_BOOL,
            .as_bool = false 
        },
        .prio = 0,
        .validate = bool_val_validate,
        .commit = NULL,
    });

    status = Settings_Create((struct setting){
        .name = "pf.debug.show_navigation_portals",
        .val = (struct sval) {
            .type = ST_TYPE_BOOL,
            .as_bool = false 
        },
        .prio = 0,
        .validate = bool_val_validate,
        .commit = NULL,
    });

    status = Settings_Create((struct setting){
        .name = "pf.debug.show_chunk_boundaries",
        .val = (struct sval) {
            .type = ST_TYPE_BOOL,
            .as_bool = false 
        },
        .prio = 0,
        .validate = bool_val_validate,
        .commit = NULL,
    });

    s_gs.prev_tick_map = NULL;
    s_gs.curr_ws_idx = 0;
    s_gs.light_pos = (vec3_t){120.0f, 150.0f, 120.0f};
    s_gs.ss = G_RUNNING;

    return true;

fail_ws:
    for(int i = 0; i < NUM_CAMERAS; i++)
        Camera_Free(s_gs.cameras[i]);
fail_cams:
    kh_destroy(entity, s_gs.dynamic);
fail_dynamic:
    kh_destroy(entity, s_gs.active);
fail_active:
    return false;
}

bool G_NewGameWithMapString(const char *mapstr)
{
    ASSERT_IN_MAIN_THREAD();

    g_reset();

    size_t copysize = AL_MapShallowCopySizeStr(mapstr);
    s_gs.prev_tick_map = malloc(copysize);
    if(!s_gs.prev_tick_map)
        return false;

    s_gs.map = AL_MapFromPFMapString(mapstr);
    if(!s_gs.map) {
        free((void*)s_gs.prev_tick_map);
        return false;
    }

    g_init_map();
    E_Global_Notify(EVENT_NEW_GAME, NULL, ES_ENGINE);

    return true;
}

bool G_NewGameWithMap(const char *dir, const char *pfmap)
{
    ASSERT_IN_MAIN_THREAD();

    g_reset();

    size_t copysize = AL_MapShallowCopySize(dir, pfmap);
    s_gs.prev_tick_map = malloc(copysize);
    if(!s_gs.prev_tick_map)
        return false;

    s_gs.map = AL_MapFromPFMap(dir, pfmap);
    if(!s_gs.map) {
        free((void*)s_gs.prev_tick_map);
        return false;
    }

    g_init_map();
    E_Global_Notify(EVENT_NEW_GAME, NULL, ES_ENGINE);

    return true;
}

void G_GetMinimapPos(float *out_x, float *out_y)
{
    ASSERT_IN_MAIN_THREAD();

    assert(s_gs.map);
    vec2_t center_pos;
    M_GetMinimapPos(s_gs.map, &center_pos);
    *out_x = center_pos.x;
    *out_y = center_pos.y;
}

void G_SetMinimapPos(float x, float y)
{
    ASSERT_IN_MAIN_THREAD();

    assert(s_gs.map);
    M_SetMinimapPos(s_gs.map, (vec2_t){x, y});
}

int G_GetMinimapSize(void)
{
    ASSERT_IN_MAIN_THREAD();

    assert(s_gs.map);
    return M_GetMinimapSize(s_gs.map);
}

void G_SetMinimapSize(int size)
{
    ASSERT_IN_MAIN_THREAD();

    assert(s_gs.map);
    M_SetMinimapSize(s_gs.map, size);
}

void G_SetMinimapResizeMask(int mask)
{
    ASSERT_IN_MAIN_THREAD();

    assert(s_gs.map);
    M_SetMinimapResizeMask(s_gs.map, mask);
}

bool G_MouseOverMinimap(void)
{
    ASSERT_IN_MAIN_THREAD();

    if(!s_gs.map)
        return false;
    return M_MouseOverMinimap(s_gs.map);
}

bool G_MapHeightAtPoint(vec2_t xz, float *out_height)
{
    ASSERT_IN_MAIN_THREAD();

    if(!s_gs.map)
        return false;

    if(!M_PointInsideMap(s_gs.map, xz))
        return false;

    *out_height = M_HeightAtPoint(s_gs.map, xz);
    return true;
}

bool G_PointInsideMap(vec2_t xz)
{
    ASSERT_IN_MAIN_THREAD();

    if(!s_gs.map)
        return false;
    return M_PointInsideMap(s_gs.map, xz);
}

void G_BakeNavDataForScene(void)
{
    ASSERT_IN_MAIN_THREAD();

    uint32_t key;
    struct entity *curr;
    (void)key;

    kh_foreach(s_gs.active, key, curr, {

        if(((ENTITY_FLAG_COLLISION | ENTITY_FLAG_STATIC) & curr->flags) 
         != (ENTITY_FLAG_COLLISION | ENTITY_FLAG_STATIC))
            continue;

        struct obb obb;
        Entity_CurrentOBB(curr, &obb);
        M_NavCutoutStaticObject(s_gs.map, &obb);
    });

    M_NavUpdatePortals(s_gs.map);
    M_NavUpdateIslandsField(s_gs.map);
}

bool G_UpdateMinimapChunk(int chunk_r, int chunk_c)
{
    ASSERT_IN_MAIN_THREAD();

    assert(s_gs.map);
    return M_UpdateMinimapChunk(s_gs.map, chunk_r, chunk_c);
}

void G_MoveActiveCamera(vec2_t xz_ground_pos)
{
    ASSERT_IN_MAIN_THREAD();

    vec3_t old_pos = Camera_GetPos(ACTIVE_CAM);
    float offset_mag = cos(DEG_TO_RAD(Camera_GetPitch(ACTIVE_CAM))) * Camera_GetHeight(ACTIVE_CAM);

    /* We position the camera such that the camera ray intersects the ground plane (Y=0)
     * at the specified xz position. */
    vec3_t new_pos = (vec3_t) {
        xz_ground_pos.x - cos(DEG_TO_RAD(Camera_GetYaw(ACTIVE_CAM))) * offset_mag,
        old_pos.y,
        xz_ground_pos.z + sin(DEG_TO_RAD(Camera_GetYaw(ACTIVE_CAM))) * offset_mag 
    };

    Camera_SetPos(ACTIVE_CAM, new_pos);
}

void G_Shutdown(void)
{
    ASSERT_IN_MAIN_THREAD();

    g_reset();

    R_DestroyWS(&s_gs.ws[0]);
    R_DestroyWS(&s_gs.ws[1]);

    R_PushCmd((struct rcmd){ R_GL_WaterShutdown, 0 });
    G_Timer_Shutdown();
    G_Sel_Shutdown();

    for(int i = 0; i < NUM_CAMERAS; i++)
        Camera_Free(s_gs.cameras[i]);

    kh_destroy(entity, s_gs.active);
    kh_destroy(entity, s_gs.dynamic);
    vec_pentity_destroy(&s_gs.light_visible);
    vec_pentity_destroy(&s_gs.visible);
    vec_obb_destroy(&s_gs.visible_obbs);
    vec_pentity_destroy(&s_gs.deleted);
}

void G_Update(void)
{
    ASSERT_IN_MAIN_THREAD();

    if(s_gs.map) {
        M_Update(s_gs.map);
    }

    vec_pentity_reset(&s_gs.visible);
    vec_pentity_reset(&s_gs.light_visible);
    vec_obb_reset(&s_gs.visible_obbs);

    vec3_t pos = Camera_GetPos(ACTIVE_CAM);
    vec3_t dir = Camera_GetDir(ACTIVE_CAM);

    struct frustum cam_frust;
    Camera_MakeFrustum(ACTIVE_CAM, &cam_frust);

    struct frustum light_frust;
    R_LightFrustum(s_gs.light_pos, pos, dir, &light_frust);

    uint32_t key;
    struct entity *curr;
    (void)key;

    kh_foreach(s_gs.active, key, curr, {

        if(s_gs.ss == G_RUNNING && curr->flags & ENTITY_FLAG_ANIMATED)
            A_Update(curr);

        if(!(curr->flags & ENTITY_FLAG_COLLISION))
            continue;

        if(curr->flags & ENTITY_FLAG_INVISIBLE)
            continue;

        struct obb obb;
        Entity_CurrentOBB(curr, &obb);

        /* Build the set of currently visible entities. Note that there may be some 
         * false positives due to using the fast frustum cull. 
         */
        if(C_FrustumOBBIntersectionFast(&cam_frust, &obb) != VOLUME_INTERSEC_OUTSIDE) {

            vec_pentity_push(&s_gs.visible, curr);
            vec_obb_push(&s_gs.visible_obbs, obb);
        }

        if(C_FrustumOBBIntersectionFast(&light_frust, &obb) != VOLUME_INTERSEC_OUTSIDE) {

            vec_pentity_push(&s_gs.light_visible, curr);
        }
    });

    /* Next, update the set of currently selected entities. */
    G_Sel_Update(ACTIVE_CAM, &s_gs.visible, &s_gs.visible_obbs);
}

void G_Render(void)
{
    ASSERT_IN_MAIN_THREAD();
    ss_e status;
    (void)status;

    R_PushCmd((struct rcmd){ R_GL_BeginFrame, 0 });

    struct render_input in;
    g_create_render_input(&in);
    G_RenderMapAndEntities(in);

    struct sval refract_setting;
    status = Settings_Get("pf.video.water_refraction", &refract_setting);
    assert(status == SS_OKAY);

    struct sval reflect_setting;
    status = Settings_Get("pf.video.water_reflection", &reflect_setting);
    assert(status == SS_OKAY);

    if(s_gs.map) {
        R_PushCmd((struct rcmd){
            .func = R_GL_DrawWater,
            .nargs = 3,
            .args = { 
                g_push_render_input(in),
                R_PushArg(&refract_setting.as_bool, sizeof(int)),
                R_PushArg(&reflect_setting.as_bool, sizeof(int)),
            },
        });
    }
    g_destroy_render_input(&in);

    enum selection_type sel_type;
    const vec_pentity_t *selected = G_Sel_Get(&sel_type);
    for(int i = 0; i < vec_size(selected); i++) {

        struct entity *curr = vec_AT(selected, i);
        vec2_t curr_pos = G_Pos_GetXZ(curr->uid);
        const float width = 0.4f;

        R_PushCmd((struct rcmd){
            .func = R_GL_DrawSelectionCircle,
            .nargs = 5,
            .args = {
                R_PushArg(&curr_pos, sizeof(curr_pos)),
                R_PushArg(&curr->selection_radius, sizeof(curr->selection_radius)),
                R_PushArg(&width, sizeof(width)),
                R_PushArg(&g_seltype_color_map[sel_type], sizeof(g_seltype_color_map[0])),
                (void*)s_gs.prev_tick_map,
            },
        });
    }

    E_Global_NotifyImmediate(EVENT_RENDER_3D, NULL, ES_ENGINE);
    R_PushCmd((struct rcmd) { R_GL_SetScreenspaceDrawMode, 0 });
    E_Global_NotifyImmediate(EVENT_RENDER_UI, NULL, ES_ENGINE);

    struct sval hb_setting;
    status = Settings_Get("pf.game.healthbar_mode", &hb_setting);
    assert(status == SS_OKAY);

    if(hb_setting.as_bool) {
        g_render_healthbars();
    }

    if(s_gs.map) {
        M_RenderMinimap(s_gs.map, ACTIVE_CAM);
    }
}

void G_RenderMapAndEntities(struct render_input in)
{
    if(in.shadows) {
        g_shadow_pass(in.cam, in.map, in.light_vis_stat, in.light_vis_anim);
    }
    g_draw_pass(in.cam, in.map, in.shadows, in.cam_vis_stat, in.cam_vis_anim);
}

bool G_AddEntity(struct entity *ent, vec3_t pos)
{
    ASSERT_IN_MAIN_THREAD();

    int ret;
    khiter_t k;

    k = kh_put(entity, s_gs.active, ent->uid, &ret);
    if(ret == -1 || ret == 0)
        return false;
    kh_value(s_gs.active, k) = ent;

    if(ent->flags & ENTITY_FLAG_COMBATABLE)
        G_Combat_AddEntity(ent, COMBAT_STANCE_AGGRESSIVE);

    G_Pos_Set(ent->uid, pos);
    if(ent->flags & ENTITY_FLAG_STATIC)
        return true;

    k = kh_put(entity, s_gs.dynamic, ent->uid, &ret);
    assert(ret != -1 && ret != 0);
    kh_value(s_gs.dynamic, k) = ent;

    G_Move_AddEntity(ent);
    return true;
}

bool G_RemoveEntity(struct entity *ent)
{
    ASSERT_IN_MAIN_THREAD();

    khiter_t k = kh_get(entity, s_gs.active, ent->uid);
    if(k == kh_end(s_gs.active))
        return false;
    kh_del(entity, s_gs.active, k);

    if(ent->flags & ENTITY_FLAG_SELECTABLE)
        G_Sel_Remove(ent);

    if(!(ent->flags & ENTITY_FLAG_STATIC)) {
        k = kh_get(entity, s_gs.dynamic, ent->uid);
        assert(k != kh_end(s_gs.dynamic));
        kh_del(entity, s_gs.dynamic, k);
    }

    G_Move_RemoveEntity(ent);
    G_Combat_RemoveEntity(ent);
    G_Pos_Delete(ent->uid);
    return true;
}

void G_StopEntity(const struct entity *ent)
{
    ASSERT_IN_MAIN_THREAD();

    if(ent->flags & ENTITY_FLAG_COMBATABLE) {
        G_Combat_StopAttack(ent);
        G_Combat_SetStance(ent, COMBAT_STANCE_AGGRESSIVE);
    }
    G_Move_Stop(ent);
}

void G_SafeFree(struct entity *ent)
{
    ASSERT_IN_MAIN_THREAD();
    vec_pentity_push(&s_gs.deleted, ent);
}

bool G_AddFaction(const char *name, vec3_t color)
{
    ASSERT_IN_MAIN_THREAD();

    if(s_gs.num_factions == MAX_FACTIONS)
        return false;
    if(strlen(name) >= sizeof(s_gs.factions[0].name))
        return false;

    int new_fac_id = s_gs.num_factions;
    strcpy(s_gs.factions[new_fac_id].name, name);
    s_gs.factions[new_fac_id].color = color;
    s_gs.factions[new_fac_id].controllable = true;
    s_gs.num_factions++;

    /* By default, a new faction is mutually at peace with 
     * every other faction. */
    for(int i = 0; i < new_fac_id; i++) {
        s_gs.diplomacy_table[i][new_fac_id] = DIPLOMACY_STATE_PEACE;
        s_gs.diplomacy_table[new_fac_id][i] = DIPLOMACY_STATE_PEACE;
    }

    return true;
}

bool G_RemoveFaction(int faction_id)
{
    ASSERT_IN_MAIN_THREAD();

    if(s_gs.num_factions < 2) 
        return false;
    if(faction_id < 0 || faction_id >= s_gs.num_factions)
        return false;

    /* Remove all entities belonging to the faction. There is no problem
     * with deleting an entry while iterating - the table bin simply gets 
     * marked as 'deleted'. 
     * Also, patch the faction_ids (which are used to index 's_gs.factions' 
     * to account for the shift in entries in this array. */
    for(khiter_t k = kh_begin(s_gs.active); k != kh_end(s_gs.active); k++) {

        if(!kh_exist(s_gs.active, k))
            continue;

        struct entity *curr = kh_value(s_gs.active, k);
        if(curr->faction_id == faction_id)
            G_RemoveEntity(curr);
        else if(curr->faction_id > faction_id) 
            --curr->faction_id;
    }

    /* Reflect the faction_id changes in the diplomacy table */
    memmove(&s_gs.diplomacy_table[faction_id], &s_gs.diplomacy_table[faction_id + 1],
        sizeof(s_gs.diplomacy_table[0]) * (s_gs.num_factions - faction_id - 1));
    for(int i = 0; i < s_gs.num_factions-1; i++) {

        memmove(&s_gs.diplomacy_table[i][faction_id], &s_gs.diplomacy_table[i][faction_id + 1],
            sizeof(enum diplomacy_state) * (s_gs.num_factions - faction_id - 1));
    }

    memmove(s_gs.factions + faction_id, s_gs.factions + faction_id + 1, 
        sizeof(struct faction) * (s_gs.num_factions - faction_id - 1));
    --s_gs.num_factions;

    return true;
}

bool G_UpdateFaction(int faction_id, const char *name, vec3_t color, bool control)
{
    ASSERT_IN_MAIN_THREAD();

    if(faction_id >= s_gs.num_factions)
        return false;
    if(strlen(name) >= sizeof(s_gs.factions[0].name))
        return false;

    strcpy(s_gs.factions[faction_id].name, name);
    s_gs.factions[faction_id].color = color;
    s_gs.factions[faction_id].controllable = control;
    return true;
}

int G_GetFactions(char out_names[][MAX_FAC_NAME_LEN], vec3_t *out_colors, bool *out_ctrl)
{
    ASSERT_IN_MAIN_THREAD();

    for(int i = 0; i < s_gs.num_factions; i++) {
    
        if(out_names) {
            strncpy(out_names[i], s_gs.factions[i].name, MAX_FAC_NAME_LEN);
            out_names[i][MAX_FAC_NAME_LEN-1] = '\0';
        }
        if(out_colors) {
            out_colors[i] = s_gs.factions[i].color;
        }
        if(out_ctrl) {
            out_ctrl[i] = s_gs.factions[i].controllable;
        }
    }
    return s_gs.num_factions;
}

bool G_SetDiplomacyState(int fac_id_a, int fac_id_b, enum diplomacy_state ds)
{
    ASSERT_IN_MAIN_THREAD();

    if(fac_id_a < 0 || fac_id_a >= s_gs.num_factions)
        return false;
    if(fac_id_b < 0 || fac_id_b >= s_gs.num_factions)
        return false;
    if(fac_id_a == fac_id_b)
        return false;

    s_gs.diplomacy_table[fac_id_a][fac_id_b] = ds;
    s_gs.diplomacy_table[fac_id_b][fac_id_a] = ds;
    return true;
}

bool G_GetDiplomacyState(int fac_id_a, int fac_id_b, enum diplomacy_state *out)
{
    ASSERT_IN_MAIN_THREAD();

    if(fac_id_a < 0 || fac_id_a >= s_gs.num_factions)
        return false;
    if(fac_id_b < 0 || fac_id_b >= s_gs.num_factions)
        return false;
    if(fac_id_a == fac_id_b)
        return false;

    *out = s_gs.diplomacy_table[fac_id_a][fac_id_b];
    return true;
}

bool G_ActivateCamera(int idx, enum cam_mode mode)
{
    ASSERT_IN_MAIN_THREAD();

    if( !(idx >= 0 && idx < NUM_CAMERAS) )
        return false;

    s_gs.active_cam_idx = idx;

    switch(mode) {
    case CAM_MODE_RTS:  CamControl_RTS_Install(s_gs.cameras[idx]); break;
    case CAM_MODE_FPS:  CamControl_FPS_Install(s_gs.cameras[idx]); break;
    default: assert(0);
    }

    return true;
}

vec3_t G_ActiveCamPos(void)
{
    ASSERT_IN_MAIN_THREAD();

    return Camera_GetPos(ACTIVE_CAM);
}

const struct camera *G_GetActiveCamera(void)
{
    ASSERT_IN_MAIN_THREAD();

    return ACTIVE_CAM;
}

vec3_t G_ActiveCamDir(void)
{
    ASSERT_IN_MAIN_THREAD();

    mat4x4_t lookat;
    Camera_MakeViewMat(ACTIVE_CAM, &lookat);
    vec3_t ret = (vec3_t){-lookat.cols[0][2], -lookat.cols[1][2], -lookat.cols[2][2]};
    PFM_Vec3_Normal(&ret, &ret);
    return ret;
}

bool G_UpdateTile(const struct tile_desc *desc, const struct tile *tile)
{
    ASSERT_IN_MAIN_THREAD();

    return M_AL_UpdateTile(s_gs.map, desc, tile);
}

const khash_t(entity) *G_GetDynamicEntsSet(void)
{
    ASSERT_IN_MAIN_THREAD();

    return s_gs.dynamic;
}

const khash_t(entity) *G_GetAllEntsSet(void)
{
    ASSERT_IN_MAIN_THREAD();

    return s_gs.active;
}

void G_SetSimState(enum simstate ss)
{
    ASSERT_IN_MAIN_THREAD();

    if(ss == s_gs.ss)
        return;

    uint32_t curr_tick = SDL_GetTicks();
    if(ss == G_RUNNING) {
    
        uint32_t key;
        struct entity *curr;
        (void)key;

        kh_foreach(s_gs.active, key, curr, {
           
            if(!(curr->flags & ENTITY_FLAG_ANIMATED))
                continue;
            A_AddTimeDelta(curr, curr_tick - s_gs.ss_change_tick);
        });
    }

    E_Global_Notify(EVENT_GAME_SIMSTATE_CHANGED, (void*)ss, ES_ENGINE);
    s_gs.ss_change_tick = curr_tick;
    s_gs.ss = ss;
}

void G_SetLightPos(vec3_t pos)
{
    ASSERT_IN_MAIN_THREAD();

    s_gs.light_pos = pos;
    R_PushCmd((struct rcmd){
        .func = R_GL_SetLightPos,
        .nargs = 1,
        .args = { R_PushArg(&pos, sizeof(pos)) },
    });
}

enum simstate G_GetSimState(void)
{
    ASSERT_IN_MAIN_THREAD();

    return s_gs.ss;
}

void G_Zombiefy(struct entity *ent)
{
    ASSERT_IN_MAIN_THREAD();

    if(ent->flags & ENTITY_FLAG_SELECTABLE)
        G_Sel_Remove(ent);

    if(!(ent->flags & ENTITY_FLAG_STATIC)) {
        khiter_t k = kh_get(entity, s_gs.dynamic, ent->uid);
        assert(k != kh_end(s_gs.dynamic));
        kh_del(entity, s_gs.dynamic, k);
    }

    G_Move_RemoveEntity(ent);
    G_Combat_RemoveEntity(ent);

    ent->flags &= ~ENTITY_FLAG_SELECTABLE;
    ent->flags &= ~ENTITY_FLAG_COLLISION;
    ent->flags &= ~ENTITY_FLAG_COMBATABLE;
    ent->flags &= ~ENTITY_FLAG_ANIMATED;

    ent->flags |= ENTITY_FLAG_INVISIBLE;
    ent->flags |= ENTITY_FLAG_STATIC;
    ent->flags |= ENTITY_FLAG_ZOMBIE;
}

struct render_workspace *G_GetSimWS(void)
{
    ASSERT_IN_MAIN_THREAD();

    return &s_gs.ws[s_gs.curr_ws_idx];
}

struct render_workspace *G_GetRenderWS(void)
{
    ASSERT_IN_RENDER_THREAD();;

    return &s_gs.ws[(s_gs.curr_ws_idx + 1) % 2];
}

void G_SwapBuffers(void)
{
    ASSERT_IN_MAIN_THREAD();

    int sim_idx = s_gs.curr_ws_idx;
    int render_idx = (sim_idx + 1) % 2;

    if(s_gs.map)
        M_AL_ShallowCopy((struct map*)s_gs.prev_tick_map, s_gs.map);

    for(int i = 0; i < vec_size(&s_gs.deleted); i++) {

        struct entity *curr = vec_AT(&s_gs.deleted, i);
        AL_EntityFree(curr);
    }
    vec_pentity_reset(&s_gs.deleted);

    assert(queue_size(s_gs.ws[render_idx].commands) == 0);
    R_ClearWS(&s_gs.ws[render_idx]);
    s_gs.curr_ws_idx = render_idx;
}

const struct map *G_GetPrevTickMap(void)
{
    ASSERT_IN_MAIN_THREAD();

    return s_gs.prev_tick_map;
}

