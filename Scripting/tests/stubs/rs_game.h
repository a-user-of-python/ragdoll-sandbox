// rs_game.h — TEST STUB HEADER (host test only).
//
// THIS IS NOT THE REAL Game/rs_game.h. It exists so Scripting can be
// compiled and tested on the host before the Game module is written.
// Declarations are copied from the DESIGN.md contract.
//
// When the real Game/rs_game.h exists, Scripting/CMakeLists.txt prefers it
// and this directory is ignored entirely.
//
// NOTE FOR THE GAME MODULE: the DESIGN.md Lua API requires
//   void RS_ApplyImpulse(RSWorld* w, uint32_t e, float ix, float iy);
// which is absent from the DESIGN.md rs_game.h snapshot. The real header
// MUST declare it and the Game module MUST implement it.

#ifndef RS_GAME_STUB_H
#define RS_GAME_STUB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RSWorld RSWorld;   // opaque; stubs use a dummy definition

uint32_t RS_SpawnHuman (RSWorld* w, float x, float y);
uint32_t RS_SpawnCrate (RSWorld* w, float x, float y, float size);
uint32_t RS_SpawnBarrel(RSWorld* w, float x, float y);
void     RS_Despawn(RSWorld* w, uint32_t e);

void RS_FireHitscan(RSWorld* w, float x, float y, float angle, int weapon);
void RS_ThrowGrenade(RSWorld* w, float x, float y, float vx, float vy);
void RS_Explode(RSWorld* w, float x, float y, float radius, float power);

// Required by the Lua API contract (see note above).
void RS_ApplyImpulse(RSWorld* w, uint32_t e, float ix, float iy);

int RS_RunLuaFile(RSWorld* w, const char* path);
const char* RS_GetLuaError(RSWorld* w);

#ifdef __cplusplus
}
#endif

#endif // RS_GAME_STUB_H
