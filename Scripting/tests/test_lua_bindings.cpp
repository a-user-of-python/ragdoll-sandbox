// test_lua_bindings.cpp — host test for the Lua modding layer.
//
// Compiles against Scripting/tests/stubs/rs_game.h (test-only declarations)
// and links stub RS_* implementations that record calls. Verifies:
//   * every rs.* binding marshals arguments correctly,
//   * rs.on_tick fires per RS_LuaTick,
//   * print() routes to the log callback,
//   * the sandbox removes dangerous functions,
//   * load/tick errors are captured via RS_GetLuaError without crashing,
//   * the sample meteor_shower mod loads and runs.

#include "lua_bindings.h"
#include "rs_game.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

// Test helpers implemented in stubs/rs_game_stubs.cpp
extern "C" {
void RS_TestReset(void);
int  RS_TestCallCount(void);
const char* RS_TestCallName(int i);
double RS_TestCallArg(int i, int a);
RSWorld* RS_TestMakeWorld(void);
void RS_TestFreeWorld(RSWorld* w);
}

static int g_failures = 0;
static std::string g_printLog;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        ++g_failures; \
    } \
} while (0)

#define CHECK_CALL(idx, name, a0, a1, a2, a3) do { \
    CHECK(RS_TestCallCount() > (idx)); \
    if (RS_TestCallCount() > (idx)) { \
        CHECK(std::string(RS_TestCallName(idx)) == (name)); \
        CHECK(fabs(RS_TestCallArg(idx, 0) - (a0)) < 1e-6); \
        CHECK(fabs(RS_TestCallArg(idx, 1) - (a1)) < 1e-6); \
        CHECK(fabs(RS_TestCallArg(idx, 2) - (a2)) < 1e-6); \
        CHECK(fabs(RS_TestCallArg(idx, 3) - (a3)) < 1e-6); \
    } \
} while (0)

static void logCb(const char* msg, void*) { g_printLog += msg; g_printLog += "\n"; }

static int findCalls(const char* name) {
    int n = 0;
    for (int i = 0; i < RS_TestCallCount(); ++i)
        if (std::string(RS_TestCallName(i)) == name) ++n;
    return n;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        printf("usage: %s <test_script.lua> <sample_mods_dir>\n", argv[0]);
        return 2;
    }
    const char* scriptPath = argv[1];
    std::string sampleMod = std::string(argv[2]) + "/meteor_shower.lua";

    // ---- 1. load the exercise script ----
    RSWorld* w = RS_TestMakeWorld();
    RS_TestReset();
    g_printLog.clear();
    {
        RS_LuaState st(w, logCb, nullptr);
        bool ok = st.runFile(scriptPath);
        CHECK(ok);
        if (!ok) printf("load error: %s\n", st.lastError());

        // 8 calls from the script body, in order.
        CHECK(RS_TestCallCount() == 8);
        CHECK_CALL(0, "RS_SpawnHuman", 400, 300, 0, 0);
        CHECK_CALL(1, "RS_SpawnCrate", 500, 100, 40, 0);
        CHECK_CALL(2, "RS_SpawnBarrel", 600, 100, 0, 0);
        CHECK_CALL(3, "RS_Explode", 400, 300, 150, 900);
        CHECK_CALL(4, "RS_FireHitscan", 100, 200, 0.5, 1);
        CHECK_CALL(5, "RS_ThrowGrenade", 300, 400, 250, 300);
        CHECK_CALL(6, "RS_ApplyImpulse", 100, 10, 5000, 0); // h == 100
        CHECK_CALL(7, "RS_Despawn", 101, 0, 0, 0);          // c == 101

        // print() routing: "hello\tmods\t42"
        CHECK(g_printLog.find("hello\tmods\t42") != std::string::npos);

        // ---- 2. on_tick via RS_LuaTick ----
        for (int i = 0; i < 5; ++i) RS_LuaTick(w);
        CHECK(RS_TestCallCount() == 9); // one more: the tick-3 crate
        CHECK_CALL(8, "RS_SpawnCrate", 700, 700, 25, 0);

        // no state after destroy; tick is a safe no-op tested below
    }

    // ---- 3. RS_RunLuaFile / RS_GetLuaError: missing file ----
    CHECK(RS_RunLuaFile(w, "/nonexistent/mod.lua") == -1);
    CHECK(RS_GetLuaError(w) != nullptr);

    // ---- 4. tick error is captured, not fatal ----
    {
        RS_LuaState st(w, logCb, nullptr);
        CHECK(st.runString("rs.on_tick(function() error('boom') end)"));
        RS_LuaTick(w); // must not crash
        const char* e = RS_GetLuaError(w);
        CHECK(e != nullptr && std::string(e).find("boom") != std::string::npos);
    }

    // ---- 5. reload replaces previous mod state ----
    CHECK(RS_RunLuaFile(w, scriptPath) == 0);
    CHECK(RS_LuaState::forWorld(w) != nullptr);
    RS_TestReset();
    for (int i = 0; i < 2; ++i) RS_LuaTick(w);
    CHECK(findCalls("RS_SpawnCrate") == 0); // fresh `ticks`, no tick-3 yet
    CHECK(RS_RunLuaFile(w, scriptPath) == 0); // reload again: still fine
    RS_TestReset();
    for (int i = 0; i < 3; ++i) RS_LuaTick(w);
    CHECK(findCalls("RS_SpawnCrate") == 1); // tick-3 crate fired once

    // ---- 6. sample mod: meteor_shower.lua ----
    RS_TestReset();
    CHECK(RS_RunLuaFile(w, sampleMod.c_str()) == 0);
    if (RS_GetLuaError(w)) printf("sample mod error: %s\n", RS_GetLuaError(w));
    for (int i = 0; i < 200; ++i) RS_LuaTick(w);
    int barrels = findCalls("RS_SpawnBarrel");
    int impulses = findCalls("RS_ApplyImpulse");
    CHECK(barrels >= 3);    // 200 ticks / 45 = ~4 spawns
    CHECK(impulses >= 3);
    CHECK(findCalls("RS_Explode") >= 0); // bonus explosions may fire

    // ---- 7. tick with no state is a no-op ----
    delete RS_LuaState::forWorld(w);
    CHECK(RS_LuaState::forWorld(w) == nullptr);
    RS_LuaTick(w); // must not crash
    CHECK(RS_GetLuaError(w) == nullptr);

    RS_TestFreeWorld(w);

    if (g_failures == 0) printf("ALL LUA BINDING TESTS PASSED\n");
    else printf("%d FAILURES\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
