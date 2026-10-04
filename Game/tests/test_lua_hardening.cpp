// test_lua_hardening.cpp — sandbox DoS hardening tests (H1-H6).
//
// Uses the REAL game (RS_CreateWorld) + real rs_lua bindings.
// Ownership rule: RS_LuaState is heap-allocated; the world owns it.
// RS_DestroyWorld / RS_LuaReset delete it. Never stack-allocate a state
// and then destroy its world (double-free).
//   H1: infinite loop in on_tick times out (~100ms) instead of hanging.
//   H2: 70MB allocation hits the 64MB cap with "not enough memory"; state survives.
//   H3: two mods coexist, both on_ticks fire; RS_LuaReset + reload works.
//   H4: dofile/loadfile are nil.
//   H5: require() is locked to the mod dir (absolute-path require fails).
//   H6: print() output truncated at 4KB.

#include "lua_bindings.h"
#include "rs_game.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

static int g_failures = 0;
static std::string g_log;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        ++g_failures; \
    } \
} while (0)

static void logCb(const char* msg, void*) { g_log += msg; g_log += "\n"; }

static void writeFile(const char* path, const char* content) {
    FILE* f = fopen(path, "w");
    if (f) { fputs(content, f); fclose(f); }
}

static double nowSec() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

int main() {
    // ---- H1: infinite loop times out, doesn't hang ----
    {
        RSWorld* w = RS_CreateWorld();
        RS_LuaState* st = new RS_LuaState(w, logCb, nullptr);
        CHECK(st->runString("rs.on_tick(function() while true do end end)", "hang"));
        double t0 = nowSec();
        RS_LuaTick(w); // must return in ~100ms, not hang
        double dt = nowSec() - t0;
        CHECK(dt < 5.0);
        const char* e = RS_GetLuaError(w);
        CHECK(e != nullptr && std::string(e).find("timed out") != std::string::npos);
        if (e) printf("  H1 timeout msg: %s (%.2fs)\n", e, dt);
        // state still usable after a timeout
        CHECK(st->runString("rs.on_tick(function() end)", "recover"));
        RS_DestroyWorld(w); // deletes st via RS_ScriptingWorldDestroyed
    }

    // ---- H2: memory cap ----
    {
        RSWorld* w = RS_CreateWorld();
        RS_LuaState* st = new RS_LuaState(w, logCb, nullptr);
        bool ok = st->runString("string.rep('x', 70000000)", "mem bomb");
        CHECK(!ok);
        const char* e = st->lastError();
        CHECK(e != nullptr && std::string(e).find("not enough memory") != std::string::npos);
        if (e) printf("  H2 oom msg: %s\n", e);
        // state survives OOM and still runs code
        CHECK(st->runString("x = 40 + 2", "after oom"));
        RS_DestroyWorld(w);
    }

    // ---- H3: two mods coexist; disable + reload ----
    writeFile("/tmp/hard_a.lua",
              "rs.on_tick(function() rs.spawn_human(0, 500) end)");
    writeFile("/tmp/hard_b.lua",
              "rs.on_tick(function() rs.spawn_crate(100, 500, 20) end)");
    {
        RSWorld* w = RS_CreateWorld();
        CHECK(RS_RunLuaFile(w, "/tmp/hard_a.lua") == 0);
        CHECK(RS_RunLuaFile(w, "/tmp/hard_b.lua") == 0); // must NOT wipe A
        int before = RS_GetEntityCount(w);
        RS_LuaTick(w);
        int after = RS_GetEntityCount(w);
        CHECK(after - before == 2); // human + crate, one per mod
        printf("  H3 multi-mod: %d -> %d entities\n", before, after);

        // disable A: reset + reload only B
        RS_LuaReset(w);
        CHECK(RS_LuaState::forWorld(w) == nullptr);
        CHECK(RS_RunLuaFile(w, "/tmp/hard_b.lua") == 0);
        before = RS_GetEntityCount(w);
        RS_LuaTick(w);
        after = RS_GetEntityCount(w);
        CHECK(after - before == 1); // only B's crate
        printf("  H3 reset+reload: %d -> %d entities\n", before, after);
        RS_DestroyWorld(w);
    }

    // ---- H4/H5: dofile/loadfile nil, require locked ----
    {
        RSWorld* w = RS_CreateWorld();
        RS_LuaState* st = new RS_LuaState(w, logCb, nullptr);
        CHECK(st->runString("return dofile == nil and loadfile == nil", "h4"));
        // absolute-path require must fail (path locked to mod dir)
        bool ok = st->runString("require('/etc/hostname')", "h5");
        CHECK(!ok);
        RS_DestroyWorld(w);
    }

    // ---- H6: print truncation ----
    {
        RSWorld* w = RS_CreateWorld();
        g_log.clear();
        RS_LuaState* st = new RS_LuaState(w, logCb, nullptr);
        CHECK(st->runString("print(string.rep('y', 100000))", "h6"));
        CHECK(g_log.size() < 5000); // 4096 + "[truncated]" + newline
        CHECK(g_log.find("[truncated]") != std::string::npos);
        printf("  H6 print bytes: %zu\n", g_log.size());
        RS_DestroyWorld(w);
    }

    // ---- os.getenv / os.tmpname nil (info disclosure) ----
    {
        RSWorld* w = RS_CreateWorld();
        RS_LuaState* st = new RS_LuaState(w, logCb, nullptr);
        CHECK(st->runString("return os.getenv == nil and os.tmpname == nil", "osleak"));
        CHECK(st->runString("return package.loadlib == nil and io.tmpfile == nil", "sandbox"));
        RS_DestroyWorld(w);
    }

    if (g_failures == 0) printf("ALL LUA HARDENING TESTS PASSED\n");
    else printf("%d FAILURES\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
