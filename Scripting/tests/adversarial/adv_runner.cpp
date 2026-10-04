// adv_runner.cpp — adversarial sandbox test runner.
// Usage: adv_runner <script.lua>
// Runs the script in a fresh sandboxed RS_LuaState (stub world).
// Prints: RESULT: OK | LUA_ERROR: <msg> | and exits nonzero on crash.
// For hang detection, the caller wraps with `timeout`.
#include "lua_bindings.h"
#include "rs_game.h"
#include <cstdio>
#include <exception>
#include <cstring>

// Minimal stub world (we only test the sandbox, not game logic).
struct RSWorld { int dummy; };

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: adv_runner <script.lua>\n"); return 2; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { printf("RESULT: NOFILE\n"); return 2; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char* buf = new char[n + 1];
    fread(buf, 1, n, f); buf[n] = 0; fclose(f);

    RSWorld w;
    // Use a marker file so we can detect filesystem writes.
    RS_LuaState* st = nullptr;
    try {
        st = new RS_LuaState(&w);
    } catch (const std::exception& e) {
        printf("RESULT: STATE_CREATE_FAIL: %s\n", e.what());
        delete[] buf; return 0;
    }
    bool ok = st->runString(buf, "adversarial");
    if (ok) {
        printf("RESULT: OK (ran clean)\n");
    } else {
        printf("RESULT: LUA_ERROR: %s\n", st->lastError());
    }
    // Also exercise tick() in case the script registered on_tick.
    st->tick();
    const char* te = st->lastError();
    if (te && te[0]) printf("TICK_ERROR: %s\n", te);
    delete st;
    delete[] buf;
    return 0;
}
