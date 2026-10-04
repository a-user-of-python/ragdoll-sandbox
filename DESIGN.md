# Ragdoll Sandbox — Design Contract

Original 2D physics sandbox for iPad (Apple Silicon M1–M5), inspired by the
genre (People Playground) but 100% original code, art, and assets. Nothing is
copied from any existing game.

Target: 60fps on iPad Air M3. Sideloadable unsigned IPA built by GitHub Actions.

## Module map

```
ragdoll-sandbox/
  Engine/        C++17 2D rigid-body physics. NO Apple deps, NO game logic.
                 CMake + host unit tests. Owner: physics agent.
  Game/          C++17 game logic: humans, props, weapons, damage, particles,
                 tools. Uses Engine. Exposes C API (rs_game.h). Owner: game agent.
  Renderer/      Swift + Metal 2D batch renderer. Consumes C API structs.
                 Owner: renderer agent.
  ThirdParty/lua Vendored PUC Lua 5.4.x source (all original MIT code).
  Scripting/     C++ Lua bindings -> C API. Owner: lua agent.
  App/           SwiftUI iOS app: game view, toolbar, spawn menu, settings
                 (graphics + JIT/memory status), StikJIT adapter, entitlements.
                 Owner: app agent.
  .github/workflows/build-ipa.yml  Unsigned IPA via xcodebuild. Owner: app agent.
```

## The contract: `Game/rs_game.h` (C API)

This header is the law. Game implements it. Renderer, Scripting, App consume it.
It must be pure C (no C++), includable from Swift (via bridging header) and C++.

```c
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
int  RS_RunLuaFile(RSWorld* w, const char* path); /* 0=ok, else error */
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

#ifdef __cplusplus
}
#endif
#endif
```

Coordinate system: world units = points. +x right, +y UP. Camera handled by App.

## Engine requirements (physics agent)

- C++17, header+src under `Engine/`, namespace `rs2d`.
- Rigid bodies: circle, box, convex polygon. Density, friction, restitution.
- Sequential-impulse solver, configurable iterations (default 8 vel / 3 pos).
- Broadphase: spatial hash or sweep-and-prune. Must handle 500 bodies at 60Hz on M3.
- Joints: revolute (with angle limits), distance, weld. Needed for ragdolls.
- Raycast against bodies (for hitscan weapons). AABB query.
- No malloc in the step loop (pools). Deterministic given same dt.
- `Engine/tests/` with a test runner; CMakeLists.txt. All tests must pass on host.
- C++ API must be clean enough for the game agent to build ragdolls. Document it in `Engine/API.md`.

## Game requirements (game agent)

- `Game/` C++17, uses Engine. Implements `rs_game.h` fully.
- Human: ragdoll (head, torso, pelvis, 2x upper arm, 2x forearm, 2x thigh, 2x shin)
  with revolute joints + angle limits. Per-limb health; damage -> blood particles;
  limb destroyed at 0 hp (detach); all vital destroyed -> death (tint=1).
- Props: crate (breakable), barrel (explodes on damage/fire), ball, plank.
- Weapons as in the C API. Hitscan: raycast, spread, damage falloff, tracer particles.
  Grenade: timed fuse -> explode. Rocket: projectile -> explode on impact.
  Explosion: radial impulse + damage + fire particles + smoke.
  Fire: damages over time, spreads to flammables (crates, humans), tint=2.
- Particles: pooled, capped by RS_SetParticleBudget.
- Tools: grab (mouse joint), freeze, heal, delete.
- Fixed timestep accumulator inside RS_Step (1/60 substeps, max 4).
- No Apple deps. Buildable on host for tests.

## Renderer requirements (renderer agent)

- NATIVE Metal only — Metal 3 as baseline (iPadOS 16+), Metal 4 API where
  available (guard with @available / respondsToSelector, never crash on
  Metal 3-only devices). No MoltenVK, no translation layers, no cross-platform
  abstraction over the GPU path.
- Swift + Metal, `Renderer/` with `Renderer.swift`, `Shaders.metal`.
- Immediate-mode API for the App:
  `submit(items: [RSRenderItem], particles: [RSParticle], viewMatrix: simd_float3x3)`.
- Batches boxes/circles/segments into few draw calls. Per-vertex color.
- Circles via instanced quads + SDF in shader (crisp at any zoom).
- Particles as instanced quads with soft falloff.
- Blood: dark red; fire: additive orange/yellow; smoke: alpha grey.
- Camera: App supplies view matrix (pan/zoom). Renderer is dumb.
- Use Metal 4 features opportunistically where they help (e.g. argument-table
  tiers, ML-tensor-free paths only — no ML dependency): fence-free triple
  buffering, memoryless render targets where applicable. Keep a clean
  Metal 3 fallback path.
- Must hit 60fps on M3 with 2000 items + 4000 particles. No UIKit in render loop.

## Scripting requirements (lua agent)

- Vendor PUC Lua 5.4.x under `ThirdParty/lua/` (real upstream source).
- `Scripting/lua_bindings.cpp`: binds the Lua API from the contract to rs_game.h.
- `Scripting/LUA_API.md`: documents every function with a mod example.
- Mods load from the app's Documents/Mods/*.lua (App handles the folder).
- `rs.on_tick(fn)`: fn called every RS_Step. Errors captured -> RS_GetLuaError.
- Sandboxed: no `os.execute`, no `io` except read. Keep `print` -> log.

## App requirements (app agent)

- Xcode project `RagdollSandbox.xcodeproj`, iOS 17+, iPad only, landscape.
- SwiftUI: main game view (Metal), left toolbar (tools), right spawn palette
  (humans, props, weapons), top bar (pause, clear, settings gear).
- Touch: one finger = active tool (grab/shoot), two-finger drag = pan,
  pinch = zoom. Tool-dependent.
- SettingsView:
  * Graphics: render scale (50/75/100%), particle budget (slider),
    physics substeps (1/2/4), blood on/off, FPS counter on/off.
  * System: device model, JIT status (StikJIT), memory status
    (increased-memory-limit entitlement + available MB), physics step ms.
  * Mods: list Documents/Mods/*.lua, enable/disable, reload button.
- StikJIT: adapt the enablement pattern (see Madeira's StikJITHelper) to request
  JIT at launch; report status. No private App Store APIs needed beyond what
  sideloading permits.
- Entitlements: `com.apple.developer.kernel.increased-memory-limit` = true.
- `.github/workflows/build-ipa.yml`: macos runner, xcodebuild archive with
  CODE_SIGNING_ALLOWED=NO, package Payload/*.app into RagdollSandbox-unsigned.ipa,
  upload as artifact. Must be manual (workflow_dispatch) or on tags.
- Bundle id: `com.ragdollsandbox.app`. Display name: "Ragdoll Sandbox".

## Legal

All code, art, names, and text are original. No assets, code, or files from
People Playground or any other game. "Inspired by the genre" only.
