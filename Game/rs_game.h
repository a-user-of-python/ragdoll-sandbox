/* rs_game.h — C API contract for Ragdoll Sandbox.
 *
 * This header is the law. Game/ implements it. Renderer/, Scripting/ and
 * App/ consume it. Pure C: includable from Swift (bridging header) and C++.
 *
 * Coordinate system: world units = points. +x right, +y UP. Camera handled
 * by the App.
 *
 * All original code. No assets, code, or files from any existing game.
 */
#ifndef RS_GAME_H
#define RS_GAME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RSWorld RSWorld;   /* opaque */

/* --- lifecycle --- */
RSWorld* RS_CreateWorld(void);
void     RS_DestroyWorld(RSWorld* w);
void     RS_Step(RSWorld* w, float dt);          /* fixed-step internally */
void     RS_ClearWorld(RSWorld* w);              /* remove all entities */

/* --- spawning: returns entity id (>0), 0 on failure --- */
uint32_t RS_SpawnHuman (RSWorld* w, float x, float y);
uint32_t RS_SpawnCrate (RSWorld* w, float x, float y, float size);
uint32_t RS_SpawnBarrel(RSWorld* w, float x, float y);   /* explosive */
uint32_t RS_SpawnBall  (RSWorld* w, float x, float y, float r);
uint32_t RS_SpawnPlank (RSWorld* w, float x, float y, float pw, float ph);
void     RS_Despawn(RSWorld* w, uint32_t e);
void     RS_DespawnAllHumans(RSWorld* w);

/* --- physics impulse (for Lua rs.apply_impulse) --- */
void RS_ApplyImpulse(RSWorld* w, uint32_t e, float ix, float iy);

/* --- weapons --- */
/* weapon: 0=pistol 1=rifle 2=shotgun 3=sniper 4=smg */
void RS_FireHitscan(RSWorld* w, float x, float y, float angle, int weapon);
void RS_ThrowGrenade(RSWorld* w, float x, float y, float vx, float vy);
void RS_FireRocket  (RSWorld* w, float x, float y, float angle);
void RS_MeleeSwing  (RSWorld* w, float x, float y, float angle,
                     float range, float damage);
void RS_Explode(RSWorld* w, float x, float y, float radius, float power);
void RS_Ignite (RSWorld* w, float x, float y, float radius);  /* fire */

/* --- tools --- */
uint32_t RS_GrabBegin(RSWorld* w, float x, float y);   /* grab handle, 0=none */
void     RS_GrabMove (RSWorld* w, uint32_t grab, float x, float y);
void     RS_GrabEnd  (RSWorld* w, uint32_t grab);
void     RS_SetFrozen(RSWorld* w, uint32_t e, int frozen);
void     RS_HealHuman(RSWorld* w, uint32_t e);          /* revive/heal */

/* --- render data (called once per frame after RS_Step) --- */
typedef struct {
    float   x, y;        /* center, world units (1 unit = 1 point) */
    float   angle;       /* radians */
    float   w, h;        /* full width/height (diameter for circles) */
    float   r, g, b, a;
    uint8_t shape;       /* 0=box 1=circle 2=segment */
    uint8_t tint;        /* 0=normal 1=dead 2=burning */
    /* for shape==2 (segment): (x,y) is p0, (w,h) is p1, angle unused */
} RSRenderItem;
int RS_GetRenderItems(RSWorld* w, RSRenderItem* out, int maxItems);

typedef struct {
    float   x, y, vx, vy;
    float   life, maxLife, size;
    float   r, g, b, a;
    uint8_t type;        /* 0=blood 1=fire 2=smoke 3=spark 4=debris */
} RSParticle;
int  RS_GetParticles(RSWorld* w, RSParticle* out, int maxItems);
void RS_SetParticleBudget(RSWorld* w, int max);   /* graphics setting */

/* --- Lua modding --- */
int  RS_RunLuaFile(RSWorld* w, const char* path); /* 0=ok, else error.
   Loads INTO the world's shared Lua state (created on demand) so multiple
   mods coexist; each mod's rs.on_tick appends to the per-tick callback list. */
void RS_LuaReset(RSWorld* w); /* drop the whole Lua state (all mods). Call
   this, then RS_RunLuaFile per enabled mod, when the mod set changes. */
const char* RS_GetLuaError(RSWorld* w);
/* Lua API (documented in Scripting/LUA_API.md):
   rs.spawn_human(x,y) rs.spawn_crate(x,y,s) rs.spawn_barrel(x,y)
   rs.despawn(e) rs.explode(x,y,r,p) rs.fire(x,y,angle,weapon)
   rs.grenade(x,y,vx,vy) rs.apply_impulse(e,ix,iy)
   rs.on_tick(function)  -- called every step */

/* --- stats --- */
int   RS_GetEntityCount(RSWorld* w);
int   RS_GetBodyCount(RSWorld* w);
float RS_GetStepMs(RSWorld* w);   /* last step CPU ms, for settings HUD */

/* --- App-required extensions --- */
uint32_t RS_EntityAtPoint(RSWorld* w, float x, float y); /* topmost (most
    recently created) entity whose body contains the point; 0 = none.
    Used by Delete/Freeze/Heal. */
void RS_SetPhysicsSubsteps(RSWorld* w, int n); /* 1..4, default 2. Subdivides
    each 1/60s tick into n physics substeps (and scales solver iterations).
    Backing for the Settings > Graphics > Physics substeps control. */

#ifdef __cplusplus
}
#endif

#endif /* RS_GAME_H */
