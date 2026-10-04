// rs_game_stubs.cpp — TEST STUB IMPLEMENTATIONS (host test only).
//
// Records every RS_* call into a global CallLog so the host test can verify
// argument marshaling from Lua. Returns deterministic fake entity ids.
// NOT linked into the real app; the real Game module provides these.

#include "rs_game.h"

#include <string>
#include <vector>

// Concrete dummy world for tests.
struct RSWorld {
    int tag = 0x5253;
};

struct Call {
    std::string name;
    // numeric args, in order
    double a0 = 0, a1 = 0, a2 = 0, a3 = 0;
};

static std::vector<Call> g_calls;
static uint32_t g_nextId = 100;

extern "C" {

// --- test helpers (not part of any contract) ---
void RS_TestReset(void) { g_calls.clear(); g_nextId = 100; }
int  RS_TestCallCount(void) { return (int)g_calls.size(); }
const char* RS_TestCallName(int i) { return g_calls[i].name.c_str(); }
double RS_TestCallArg(int i, int a) {
    const Call& c = g_calls[i];
    return a == 0 ? c.a0 : a == 1 ? c.a1 : a == 2 ? c.a2 : c.a3;
}
RSWorld* RS_TestMakeWorld(void) { return new RSWorld(); }
void RS_TestFreeWorld(RSWorld* w) { delete w; }

static void rec(const char* n, double a0 = 0, double a1 = 0,
                double a2 = 0, double a3 = 0) {
    g_calls.push_back(Call{n, a0, a1, a2, a3});
}

uint32_t RS_SpawnHuman(RSWorld*, float x, float y) {
    rec("RS_SpawnHuman", x, y); return g_nextId++;
}
uint32_t RS_SpawnCrate(RSWorld*, float x, float y, float s) {
    rec("RS_SpawnCrate", x, y, s); return g_nextId++;
}
uint32_t RS_SpawnBarrel(RSWorld*, float x, float y) {
    rec("RS_SpawnBarrel", x, y); return g_nextId++;
}
void RS_Despawn(RSWorld*, uint32_t e) { rec("RS_Despawn", (double)e); }

void RS_FireHitscan(RSWorld*, float x, float y, float angle, int weapon) {
    rec("RS_FireHitscan", x, y, angle, (double)weapon);
}
void RS_ThrowGrenade(RSWorld*, float x, float y, float vx, float vy) {
    rec("RS_ThrowGrenade", x, y, vx, vy);
}
void RS_Explode(RSWorld*, float x, float y, float radius, float power) {
    rec("RS_Explode", x, y, radius, power);
}
void RS_ApplyImpulse(RSWorld*, uint32_t e, float ix, float iy) {
    rec("RS_ApplyImpulse", (double)e, ix, iy);
}

} // extern "C"
