// adv_runner2.cpp — v2 adversarial runner: multi-tick, timing, leak measurement.
// Usage: adv_runner2 <script.lua> [ticks]
// Prints RESULT, per-tick ms, TICK_ERRORs. Wrap with `timeout` for hangs.
#include "lua_bindings.h"
#include "rs_game.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <exception>
#include <chrono>

struct RSWorld { int dummy; };

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: adv_runner2 <script.lua> [ticks]\n"); return 2; }
    int ticks = argc > 2 ? atoi(argv[2]) : 1;
    FILE* f = fopen(argv[1], "rb");
    if (!f) { printf("RESULT: NOFILE\n"); return 2; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char* buf = new char[n + 1];
    size_t got = fread(buf, 1, n, f); buf[got] = 0; fclose(f);

    RSWorld w;
    RS_LuaState* st = nullptr;
    try { st = new RS_LuaState(&w); }
    catch (const std::exception& e) {
        printf("RESULT: STATE_CREATE_FAIL: %s\n", e.what());
        delete[] buf; return 0;
    }
    auto t0 = std::chrono::steady_clock::now();
    bool ok = st->runString(buf, "adv2");
    auto t1 = std::chrono::steady_clock::now();
    double loadMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    if (ok) printf("RESULT: OK load_ms=%.1f\n", loadMs);
    else printf("RESULT: LUA_ERROR: %s load_ms=%.1f\n", st->lastError(), loadMs);

    double worst = 0;
    for (int i = 0; i < ticks; i++) {
        auto a = std::chrono::steady_clock::now();
        st->tick();
        auto b = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(b - a).count();
        if (ms > worst) worst = ms;
        const char* te = st->lastError();
        if (te && te[0] && i < 3) printf("TICK%d_ERROR: %s\n", i, te);
    }
    printf("TICKS: n=%d worst_ms=%.1f\n", ticks, worst);
    delete st;
    delete[] buf;
    return 0;
}
