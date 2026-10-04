# Adversarial Sandbox Test Battery

Tests the Ragdoll Sandbox Lua sandbox (`Scripting/lua_bindings.cpp`) against
escape attempts. Every script here was EXECUTED against the real sandbox
via `adv_runner` — no theoretical results.

## Running

```bash
# Build the runner (needs lua + rs_lua libs + stubs):
g++ -std=c++17 -IScripting -IThirdParty/lua/src -IScripting/tests/stubs \
  Scripting/tests/adversarial/adv_runner.cpp /tmp/stubs.o \
  Scripting/build/librs_lua.a Scripting/build/liblua.a \
  -o /tmp/adv_runner

# Run one (always wrap with timeout for hang detection):
timeout 5 /tmp/adv_runner Scripting/tests/adversarial/a01_loadlib.lua
# exit 124 = TIMEOUT (hang — bad)
```

## Scripts

| File | Attack | Expected |
|------|--------|----------|
| a01_loadlib.lua | package.loadlib native load | BLOCKED (nil) |
| a02_io.lua | io.tmpfile/open/popen/stdout writes | BLOCKED |
| a03_os.lua | os.execute/exit/remove/rename/getenv | exec/exit/remove/rename nil; getenv/tmpname work (info leak, minor) |
| a04_debug.lua | debug library | BLOCKED (nil) |
| a05_byteload.lua | string.dump C funcs, load bytecode | C dump blocked; Lua bytecode OK (not an escape) |
| a06_searchers.lua | custom package.searcher | Runs, but Lua-only (no native escape) |
| a07_gc_coro.lua | collectgarbage, coroutines | Harmless |
| a08_rs_corrupt.lua | nil/replace rs table entries | Self-harm only, no crash |
| a09_load.lua | load("os.execute(...)"), loadfile | Blocked at runtime (os.execute nil) |
| a10_hang.lua | `while true do end` | **HANGS (DoS)** |
| a11_badargs.lua | nil/NaN/huge/wrong-type args to rs.* | Clean Lua errors, no crash |
| a12_env.lua | _G/_ENV/string metatable | No escape path |
| a13_tick_hang.lua | infinite loop in on_tick | **HANGS every frame (DoS)** |
| a14_recursion.lua | tail-call loop | Hangs (tail calls don't overflow; test artifact) |
| a14b_recursion2.lua | non-tail recursion | Clean "stack overflow" error |
| a15_membomb.lua | unbounded allocation | Slow, OOM risk (DoS) |
| a16_preload.lua | package.preload injection | Lua-only, no escape |
| a17_redos.lua | pattern backtracking | No catastrophic backtracking in Lua patterns |
| a18_coroutine_tick.lua | coroutine across ticks | Works, harmless |

## Open vulnerabilities (2026-10-04)

1. **DoS via infinite loop** (critical): no instruction limit or watchdog.
   Mitigation options: `lua_sethook` with instruction-count hook →
   `luaL_error`; or run tick in a thread with timeout. Needs design decision.
2. **DoS via memory exhaustion** (major): no allocation limit.
   Mitigation: custom `lua_Alloc` with a byte cap.
3. **os.getenv/os.tmpname info disclosure** (minor): reveals paths.
   Mitigation: nil them like os.execute.
