// lua_bindings.cpp — Ragdoll Sandbox Lua modding layer.
//
// Original glue code binding the embedded PUC Lua interpreter to the
// Game module's C API (rs_game.h). No Apple dependencies.
//
// What this file does:
//   * RS_LuaState: owns a sandboxed lua_State bound to one RSWorld.
//   * Installs the `rs` table (spawn/despawn/weapons/tools bindings).
//   * Sandboxes the interpreter (see sandboxLibraries()).
//   * Routes Lua print() to a log callback.
//   * Implements RS_RunLuaFile / RS_GetLuaError / RS_LuaTick.
//
// The Game module must NOT define RS_RunLuaFile, RS_GetLuaError, RS_LuaTick.

#include "lua_bindings.h"
#include "rs_game.h"

// PUC Lua headers do not carry extern "C" guards (see lua.hpp); wrap them.
extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// RS_ApplyImpulse is required by the Lua API (DESIGN.md lists
// rs.apply_impulse(e,ix,iy)) but is absent from the rs_game.h contract
// snapshot. Declared here so this module compiles whether or not
// Game/rs_game.h has caught up yet. A duplicate compatible declaration is
// legal C++, so this stays correct if the Game module adds it later.
// The Game module MUST implement:
//   void RS_ApplyImpulse(RSWorld* w, uint32_t e, float ix, float iy);
extern "C" void RS_ApplyImpulse(RSWorld* w, uint32_t e, float ix, float iy);
// ---------------------------------------------------------------------------

namespace {

// World -> live Lua state registry (single-threaded game loop).
std::unordered_map<RSWorld*, RS_LuaState*> g_states;

// Load-time error kept when a state is discarded after a failed runFile.
std::string g_lastLoadError;

} // namespace

struct RS_LuaStateImpl {
    lua_State*     L        = nullptr;
    RSWorld*       world    = nullptr;
    RS_LogCallback logCb   = nullptr;
    void*          logUser  = nullptr;
    // (H3) every rs.on_tick callback, in registration order. Each captures
    // the mod dir active at registration so the io.open path lock is judged
    // per-mod even in multi-mod (R3).
    struct TickRef { int ref = LUA_NOREF; std::string modDir; };
    std::vector<TickRef> tickRefs;
    static constexpr size_t kMaxTickRefs = 4096; // (R2) bound C++-heap growth
    std::string    lastError;

    // --- DoS hardening (H1/H2) ---
    // Memory cap bookkeeping for the custom allocator.
    struct LuaAllocState {
        size_t used = 0;
        static constexpr size_t kCap = 64u * 1024u * 1024u; // 64MB per mod state
    } alloc;
    // Wall-clock deadline for the instruction-count hook. Set around each
    // protected call (mod load ~5s, on_tick ~100ms); max() when idle.
    std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::time_point::max();
    // Mod directory for the io.open path lock (set by setModDir).
    std::string modDir;
    // Directory of the mod whose tick callback is currently running
    // (per-callback, for multi-mod io.open checks).
    std::string activeModDir;
};

// Budgets for the hook (H1).
static constexpr auto kLoadBudget = std::chrono::seconds(5);
static constexpr auto kTickBudget = std::chrono::milliseconds(100);
// (H-C) per-callback slice of the tick budget: stops one greedy mod from
// pinning the frame loop just under the shared cap.
static constexpr auto kCallbackBudget = std::chrono::milliseconds(16);
// Hook granularity: check the clock every N Lua instructions. Cheap enough
// to be always on (one clock read per 10k instructions).
static constexpr int kHookCount = 10000;

static RS_LuaStateImpl* getImpl(lua_State* L) {
    return static_cast<RS_LuaStateImpl*>(lua_touserdata(L, lua_upvalueindex(1)));
}

// ---------------------------------------------------------------------------
// DoS hardening (H1/H2): memory-capped allocator, wall-clock instruction
// hook, unprotected-error panic. All original code.
// ---------------------------------------------------------------------------

// (H2) Custom allocator capping the Lua state at 64MB. Returning NULL makes
// Lua raise a catchable "not enough memory" error instead of jetsamming.
static void* rs_lua_alloc(void* ud, void* ptr, size_t osize, size_t nsize) {
    RS_LuaStateImpl::LuaAllocState* a =
        static_cast<RS_LuaStateImpl::LuaAllocState*>(ud);
    if (nsize == 0) {
        a->used = (osize >= a->used) ? 0 : a->used - osize;
        std::free(ptr);
        return nullptr;
    }
    const size_t freed = (osize <= a->used) ? osize : a->used;
    const size_t base  = a->used - freed;
    if (base >= RS_LuaStateImpl::LuaAllocState::kCap ||
        nsize > RS_LuaStateImpl::LuaAllocState::kCap - base) {
        return nullptr; // over cap: Lua raises LUA_ERRMEM ("not enough memory")
    }
    void* np = ptr ? std::realloc(ptr, nsize) : std::malloc(nsize);
    if (!np) return nullptr;
    a->used = base + nsize;
    return np;
}

// Unprotected errors (outside pcall) have nowhere to go; mirror lauxlib's
// panic. In practice unreachable: every entry point uses pcall, and the
// capped allocator turns OOM into catchable LUA_ERRMEM.
static int rs_panic(lua_State* L) {
    const char* msg = lua_tostring(L, -1);
    std::fprintf(stderr, "PANIC: unprotected Lua error: %s\n", msg ? msg : "?");
    std::abort();
    return 0;
}

// (H1) Count hook: every kHookCount instructions, check the wall clock
// against the current deadline. luaL_error from a hook is a normal Lua
// error and is caught by the surrounding pcall.
static void rs_count_hook(lua_State* L, lua_Debug* /*ar*/) {
    RS_LuaStateImpl* im =
        *static_cast<RS_LuaStateImpl**>(lua_getextraspace(L));
    if (std::chrono::steady_clock::now() > im->deadline) {
        luaL_error(L, "mod timed out (instruction budget exceeded)");
    }
}

// PUC Lua is compiled as C: a C++ exception unwinding through its call
// frames is undefined behavior (typically a hard crash with no diagnostic).
// Every binding below converts C++ exceptions into Lua errors instead.
// (m3: exceptions must never cross the Lua boundary.)
#define RS_LUA_TRY try
#define RS_LUA_CATCH(name) \
    catch (const std::exception& e) { return luaL_error(L, "rs." name ": %s", e.what()); } \
    catch (...) { return luaL_error(L, "rs." name ": internal error"); }

// ---------------------------------------------------------------------------
// `rs.*` bindings. Upvalue 1 is always the Impl* (light userdata).
// ---------------------------------------------------------------------------

static int l_spawn_human(lua_State* L) {
    RS_LUA_TRY {
        RS_LuaStateImpl* im = getImpl(L);
        float x = (float)luaL_checknumber(L, 1);
        float y = (float)luaL_checknumber(L, 2);
        lua_pushinteger(L, (lua_Integer)RS_SpawnHuman(im->world, x, y));
        return 1;
    }
    RS_LUA_CATCH("spawn_human");
}

static int l_spawn_crate(lua_State* L) {
    RS_LUA_TRY {
        RS_LuaStateImpl* im = getImpl(L);
        float x = (float)luaL_checknumber(L, 1);
        float y = (float)luaL_checknumber(L, 2);
        float s = (float)luaL_checknumber(L, 3);
        lua_pushinteger(L, (lua_Integer)RS_SpawnCrate(im->world, x, y, s));
        return 1;
    }
    RS_LUA_CATCH("spawn_crate");
}

static int l_spawn_barrel(lua_State* L) {
    RS_LUA_TRY {
        RS_LuaStateImpl* im = getImpl(L);
        float x = (float)luaL_checknumber(L, 1);
        float y = (float)luaL_checknumber(L, 2);
        lua_pushinteger(L, (lua_Integer)RS_SpawnBarrel(im->world, x, y));
        return 1;
    }
    RS_LUA_CATCH("spawn_barrel");
}

static int l_despawn(lua_State* L) {
    RS_LUA_TRY {
        RS_LuaStateImpl* im = getImpl(L);
        uint32_t e = (uint32_t)luaL_checkinteger(L, 1);
        RS_Despawn(im->world, e);
        return 0;
    }
    RS_LUA_CATCH("despawn");
}

static int l_explode(lua_State* L) {
    RS_LUA_TRY {
        RS_LuaStateImpl* im = getImpl(L);
        float x = (float)luaL_checknumber(L, 1);
        float y = (float)luaL_checknumber(L, 2);
        float r = (float)luaL_checknumber(L, 3);
        float p = (float)luaL_checknumber(L, 4);
        RS_Explode(im->world, x, y, r, p);
        return 0;
    }
    RS_LUA_CATCH("explode");
}

static int l_fire(lua_State* L) {
    RS_LUA_TRY {
        RS_LuaStateImpl* im = getImpl(L);
        float x     = (float)luaL_checknumber(L, 1);
        float y     = (float)luaL_checknumber(L, 2);
        float angle = (float)luaL_checknumber(L, 3);
        int   weapon= (int)luaL_checkinteger(L, 4);
        RS_FireHitscan(im->world, x, y, angle, weapon);
        return 0;
    }
    RS_LUA_CATCH("fire");
}

static int l_grenade(lua_State* L) {
    RS_LUA_TRY {
        RS_LuaStateImpl* im = getImpl(L);
        float x  = (float)luaL_checknumber(L, 1);
        float y  = (float)luaL_checknumber(L, 2);
        float vx = (float)luaL_checknumber(L, 3);
        float vy = (float)luaL_checknumber(L, 4);
        RS_ThrowGrenade(im->world, x, y, vx, vy);
        return 0;
    }
    RS_LUA_CATCH("grenade");
}

static int l_apply_impulse(lua_State* L) {
    RS_LUA_TRY {
        RS_LuaStateImpl* im = getImpl(L);
        uint32_t e  = (uint32_t)luaL_checkinteger(L, 1);
        float ix    = (float)luaL_checknumber(L, 2);
        float iy    = (float)luaL_checknumber(L, 3);
        RS_ApplyImpulse(im->world, e, ix, iy);
        return 0;
    }
    RS_LUA_CATCH("apply_impulse");
}

static int l_on_tick(lua_State* L) {
    RS_LUA_TRY {
        RS_LuaStateImpl* im = getImpl(L);
        luaL_checktype(L, 1, LUA_TFUNCTION);
        // (H3) multi-mod: APPEND, never replace. Every registered callback
        // fires each tick, in registration order.
        // (R2) cap registrations: the vector lives on the C++ heap, outside
        // the Lua 64MB cap.
        if (im->tickRefs.size() >= RS_LuaStateImpl::kMaxTickRefs)
            return luaL_error(L, "on_tick: too many callbacks (max 4096)");
        lua_pushvalue(L, 1);
        RS_LuaStateImpl::TickRef tr;
        tr.ref = luaL_ref(L, LUA_REGISTRYINDEX);
        tr.modDir = im->modDir; // (R3) per-mod dir for io.open checks
        im->tickRefs.push_back(std::move(tr));
        lua_pushboolean(L, 1);
        return 1;
    }
    RS_LUA_CATCH("on_tick");
}

static const luaL_Reg kRsLib[] = {
    {"spawn_human",   l_spawn_human},
    {"spawn_crate",   l_spawn_crate},
    {"spawn_barrel",  l_spawn_barrel},
    {"despawn",       l_despawn},
    {"explode",       l_explode},
    {"fire",         l_fire},
    {"grenade",       l_grenade},
    {"apply_impulse", l_apply_impulse},
    {"on_tick",       l_on_tick},
    {nullptr, nullptr},
};

// ---------------------------------------------------------------------------
// print() -> log callback
// ---------------------------------------------------------------------------

static int l_print(lua_State* L) {
    RS_LUA_TRY {
        RS_LuaStateImpl* im = getImpl(L);
        // NOTE: n must be captured BEFORE luaL_buffinit, which pushes a
        // light-userdata placeholder onto the stack.
        int n = lua_gettop(L);
        luaL_Buffer b;
        luaL_buffinit(L, &b);
        for (int i = 1; i <= n; ++i) {
            if (i > 1) luaL_addstring(&b, "\t");
            luaL_tolstring(L, i, nullptr); // pushes string repr
            luaL_addvalue(&b);             // pops it into the buffer
        }
        luaL_pushresult(&b);
        size_t len = 0;
        const char* msg = lua_tolstring(L, -1, &len);
        // (H6) cap print() output: a mod logging megabytes per tick is a DoS
        // on the log sink and the Settings error UI.
        std::string out(msg ? msg : "", msg ? len : 0);
        static constexpr size_t kPrintCap = 4096;
        if (out.size() > kPrintCap) {
            out.resize(kPrintCap);
            out += "...[truncated]";
        }
        if (im->logCb) im->logCb(out.c_str(), im->logUser);
        else fprintf(stderr, "[lua] %s\n", out.c_str());
        return 0;
    }
    RS_LUA_CATCH("print");
}

// ---------------------------------------------------------------------------
// Sandbox: read-only io, neutered os, no C module loading, no debug lib.
// ---------------------------------------------------------------------------

// io.open wrapper: only read modes allowed.
// (2026-10-04) io.open path lock: mods may only READ inside their own
// directory. The mode check alone still allowed exfiltrating any readable
// app-container file via print(). Lexical normalization; ".." past root or
// outside dir is denied. Symlinks can't be planted by a mod (no write/exec).
static std::string rs_norm_path(const std::string& p) {
    std::vector<std::string> parts;
    size_t i = 0;
    while (i <= p.size()) {
        size_t j = p.find('/', i);
        if (j == std::string::npos) j = p.size();
        std::string c = p.substr(i, j - i);
        if (c.empty() || c == ".") { /* skip */ }
        else if (c == "..") {
            if (parts.empty()) return ""; // escapes root: invalid
            parts.pop_back();
        } else parts.push_back(c);
        i = j + 1;
    }
    std::string n;
    for (auto& c : parts) { n += "/"; n += c; }
    return n.empty() ? "/" : n;
}

// (R3) package.path lock: the key is VIRTUALIZED via __index/__newindex.
// __newindex alone can't block reassignment of an existing key, so the real
// "path" key is removed; __index serves it from the registry, __newindex
// rejects writes. require() reads via lua_getfield, which respects __index.
static int l_package_index_path(lua_State* L) {
    const char* k = luaL_checkstring(L, 2);
    if (std::strcmp(k, "path") == 0) {
        lua_getfield(L, LUA_REGISTRYINDEX, "rs_package_path");
        return 1;
    }
    lua_rawget(L, 1);
    return 1;
}

static int l_package_newindex(lua_State* L) {
    const char* k = luaL_checkstring(L, 2);
    if (std::strcmp(k, "path") == 0)
        return luaL_error(L, "package.path is locked by the sandbox");
    lua_rawset(L, 1);
    return 0;
}

// (H-A) __gc hang guard: PUC Lua disables hooks while running finalizers
// (allowhook=0 in lgc.c), so an infinite loop in __gc hangs forever with no
// timeout possible. Block __gc at the only mod-reachable path: setmetatable.
// (debug.setmetatable is already nil.)
// __newindex guard for user metatables: blocks adding __gc post-hoc.
static int l_mt_newindex_guard(lua_State* L) {
    const char* k = luaL_checkstring(L, 2);
    if (std::strcmp(k, "__gc") == 0)
        return luaL_error(L, "sandbox: __gc blocked (hang risk)");
    lua_rawset(L, 1);
    return 0;
}

static int l_setmetatable_guard(lua_State* L) {
    // Args: (obj, mt). Nil mt is fine (removes metatable).
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        // Reject __gc (rawget: don't trigger a hostile __index).
        lua_pushstring(L, "__gc");
        lua_rawget(L, 2);
        bool has_gc = !lua_isnil(L, -1);
        lua_pop(L, 1);
        if (has_gc)
            return luaL_error(L, "setmetatable: __gc blocked by sandbox (hang risk)");
        // Block held-reference post-hoc (mt.__gc = bomb): if mt has no
        // metatable yet, give it one whose __newindex rejects "__gc".
        // (If it already has one, we leave it — the direct path above is
        // the realistic attack; rawset bypass is an accepted residual.)
        if (lua_getmetatable(L, 2) == 0) {
            lua_newtable(L);                        // guard_mt
            lua_pushcfunction(L, l_mt_newindex_guard);
            lua_setfield(L, -2, "__newindex");
            lua_setmetatable(L, 2);                 // setmetatable(mt, guard)
        } else {
            lua_pop(L, 1);                          // drop existing metatable
        }
    }
    // Call the original (stashed in the registry; the global is this wrapper).
    lua_getfield(L, LUA_REGISTRYINDEX, "rs_setmetatable_orig");
    lua_pushvalue(L, 1);
    lua_pushvalue(L, 2);
    lua_call(L, 2, 1);
    return 1;
}

static bool rs_path_within_dir(const std::string& dir, const char* p) {
    if (!p || !*p) return false;
    std::string path(p);
    if (path.find('\0') != std::string::npos) return false;
    std::string full = (path[0] == '/') ? path : dir + "/" + path;
    std::string n = rs_norm_path(full);
    std::string d = rs_norm_path(dir);
    if (n.empty() || d.empty()) return false;
    return n == d || (n.size() > d.size() && n.compare(0, d.size(), d) == 0 &&
                     n[d.size()] == '/');
}

static int l_io_open_ro(lua_State* L) {
    RS_LUA_TRY {
        const char* mode = luaL_optstring(L, 2, "r");
        for (const char* p = mode; *p; ++p) {
            if (*p == 'w' || *p == 'a' || *p == '+')
                return luaL_error(L, "io.open: write access denied by sandbox");
        }
        // Path lock: read-only inside the mod's own directory.
        // (R3) during tick, use the calling mod's dir (multi-mod).
        RS_LuaStateImpl* im =
            *static_cast<RS_LuaStateImpl**>(lua_getextraspace(L));
        const char* path = luaL_checkstring(L, 1);
        const std::string& dir =
            !im->activeModDir.empty() ? im->activeModDir : im->modDir;
        if (dir.empty() || !rs_path_within_dir(dir, path))
            return luaL_error(L, "io.open: path outside mod directory");
        lua_getfield(L, LUA_REGISTRYINDEX, "rs_io_open_orig");
        lua_pushvalue(L, 1);
        lua_pushvalue(L, 2);
        lua_call(L, 2, LUA_MULTRET);
        lua_remove(L, 1);
        lua_remove(L, 1);
        return lua_gettop(L);
    }
    RS_LUA_CATCH("io.open");
}

static void sandboxLibraries(lua_State* L) {
    // --- base: file-loading chunks can read arbitrary files (H4) ---
    lua_pushnil(L); lua_setglobal(L, "dofile");
    lua_pushnil(L); lua_setglobal(L, "loadfile");
    // NOTE: `load` (string chunks) stays: no file access involved.

    // --- os: strip process and filesystem mutation ---
    lua_getglobal(L, "os");
    if (lua_istable(L, -1)) {
        const char* drop[] = {"execute", "exit", "remove", "rename",
                              // os.getenv/os.tmpname leak host filesystem paths
                              // (info disclosure); mods have no legit need.
                              "getenv", "tmpname", nullptr};
        for (int i = 0; drop[i]; ++i) {
            lua_pushnil(L);
            lua_setfield(L, -2, drop[i]);
        }
    }
    lua_pop(L, 1);

    // --- io: read-only ---
    lua_getglobal(L, "io");
    if (lua_istable(L, -1)) {
        lua_pushnil(L); lua_setfield(L, -2, "write");
        lua_pushnil(L); lua_setfield(L, -2, "popen");
        lua_pushnil(L); lua_setfield(L, -2, "output");
        // (M2) io.tmpfile() returns a READ/WRITE handle — nil it.
        lua_pushnil(L); lua_setfield(L, -2, "tmpfile");
        // (m4) the io.stdout/io.stderr handles keep their own write method,
        // bypassing the nil'd io.write. Remove the handles entirely; mods
        // use print(), which routes to the log callback.
        lua_pushnil(L); lua_setfield(L, -2, "stdout");
        lua_pushnil(L); lua_setfield(L, -2, "stderr");
        lua_pushnil(L); lua_setfield(L, -2, "stdin");
        lua_getfield(L, -1, "open");                    // stash original
        lua_setfield(L, LUA_REGISTRYINDEX, "rs_io_open_orig");
        lua_pushcfunction(L, l_io_open_ro);             // install wrapper
        lua_setfield(L, -2, "open");
    }
    lua_pop(L, 1);

    // --- package: no C modules (native-code escape) ---
    lua_getglobal(L, "package");
    if (lua_istable(L, -1)) {
        lua_pushstring(L, "");
        lua_setfield(L, -2, "cpath");
        // (C2) package.loadlib bypasses the searchers entirely and loads
        // native code directly — nil it like os.execute.
        lua_pushnil(L);
        lua_setfield(L, -2, "loadlib");
        lua_getfield(L, -1, "searchers");
        if (lua_istable(L, -1)) {
            // Keep only the preload (1) and Lua-file (2) searchers;
            // drop the C (3) and all-in-one (4) loaders.
            lua_newtable(L);                    // package, searchers, new
            lua_geti(L, -2, 1); lua_seti(L, -2, 1);
            lua_geti(L, -2, 2); lua_seti(L, -2, 2);
            lua_setfield(L, -3, "searchers");    // package.searchers = new
            lua_pop(L, 1); // drop old searchers table -> [package]
        } else {
            lua_pop(L, 1); // searchers (not a table) -> [package]
        }
        // (R3) lock package.path: virtualize it via __index/__newindex so a
        // mod chunk (runs after sandboxing) can't reassign it to widen
        // require(). The real key is removed; __index serves the registry
        // copy, __newindex rejects "path" writes. C code uses the registry.
        lua_pushstring(L, "path");
        lua_pushnil(L);
        lua_rawset(L, -3);                        // package.path = nil (raw)
        lua_pushstring(L, "");
        lua_setfield(L, LUA_REGISTRYINDEX, "rs_package_path");
        lua_newtable(L);                          // package, mt
        lua_pushcfunction(L, l_package_index_path);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, l_package_newindex);
        lua_setfield(L, -2, "__newindex");
        lua_setmetatable(L, -2);                  // setmetatable(package, mt)
        lua_pop(L, 1); // package
    } else {
        lua_pop(L, 1); // package (not a table)
    }

    // (H5) default-deny require(): the registry path is "" until
    // RS_RunLuaFile -> setModDir sets it to the mod's own directory.
    // (Path virtualized via __index; see the package lock above.)

    // (H-A) __gc hang guard: stash the real setmetatable and install the
    // wrapper that blocks __gc (hooks can't stop finalizer loops).
    lua_getglobal(L, "setmetatable");
    if (lua_iscfunction(L, -1)) {
        lua_setfield(L, LUA_REGISTRYINDEX, "rs_setmetatable_orig");
        lua_pushcfunction(L, l_setmetatable_guard);
        lua_setglobal(L, "setmetatable");
    } else {
        lua_pop(L, 1);
    }

    // NOTE: the `debug` library is intentionally not opened: debug.setupvalue /
    // debug.upvaluejoin could rewrite the Impl* upvalues captured by the `rs`
    // closures. Tracebacks are therefore unavailable; errors carry messages.
}

static void openSafeLibraries(lua_State* L) {
    static const luaL_Reg libs[] = {
        {"_G",            luaopen_base},
        {LUA_COLIBNAME,   luaopen_coroutine},
        {LUA_TABLIBNAME,  luaopen_table},
        {LUA_STRLIBNAME,  luaopen_string},
        {LUA_MATHLIBNAME, luaopen_math},
        {LUA_UTF8LIBNAME, luaopen_utf8},
        {LUA_OSLIBNAME,   luaopen_os},
        {LUA_IOLIBNAME,   luaopen_io},
        {LUA_LOADLIBNAME, luaopen_package},
        {nullptr, nullptr}
    };
    for (const luaL_Reg* l = libs; l->name; ++l) {
        luaL_requiref(L, l->name, l->func, 1);
        lua_pop(L, 1);
    }
}

// ---------------------------------------------------------------------------
// RS_LuaState
// ---------------------------------------------------------------------------

RS_LuaState::RS_LuaState(RSWorld* world, RS_LogCallback logCb, void* logUser)
    : impl_(nullptr), world_(world) {
    // impl_ is held in a unique_ptr until the state is fully built, so a
    // throw (e.g. luaL_newstate failing) cannot leak it.
    std::unique_ptr<RS_LuaStateImpl> impl(new RS_LuaStateImpl());
    impl->world   = world;
    impl->logCb   = logCb;
    impl->logUser = logUser;
    // (m3) lua_newstate returns NULL on allocation failure — never proceed
    // with a null state. Throw; callers (RS_RunLuaFile) convert to an error.
    // (H2) custom allocator caps the state at 64MB.
    impl->L = lua_newstate(rs_lua_alloc, &impl->alloc);
    if (!impl->L) throw std::runtime_error("lua_newstate failed");
    lua_atpanic(impl->L, rs_panic);

    // (H1) stash Impl* where the count hook can reach it, then arm the
    // always-on instruction hook. The deadline starts at max (no budget
    // enforced until a protected call sets one).
    *static_cast<RS_LuaStateImpl**>(lua_getextraspace(impl->L)) = impl.get();
    lua_sethook(impl->L, rs_count_hook, LUA_MASKCOUNT, kHookCount);

    openSafeLibraries(impl->L);
    sandboxLibraries(impl->L);

    // print -> log (upvalue 1 = Impl*)
    lua_pushlightuserdata(impl->L, impl.get());
    lua_pushcclosure(impl->L, l_print, 1);
    lua_setglobal(impl->L, "print");

    // rs table (upvalue 1 = Impl* for every function)
    lua_newtable(impl->L);
    lua_pushlightuserdata(impl->L, impl.get());
    luaL_setfuncs(impl->L, kRsLib, 1);
    lua_setglobal(impl->L, "rs");

    impl_ = impl.release();
    g_states[world] = this;
}

RS_LuaState::~RS_LuaState() {
    auto it = g_states.find(world_);
    if (it != g_states.end() && it->second == this) g_states.erase(it);
    for (auto& tr : impl_->tickRefs)
        luaL_unref(impl_->L, LUA_REGISTRYINDEX, tr.ref);
    lua_close(impl_->L);
    delete impl_;
}

RS_LuaState* RS_LuaState::forWorld(RSWorld* w) {
    auto it = g_states.find(w);
    return it != g_states.end() ? it->second : nullptr;
}

void RS_LuaState::setLogCallback(RS_LogCallback cb, void* user) {
    impl_->logCb   = cb;
    impl_->logUser = user;
}

bool RS_LuaState::runFile(const char* path) {
    lua_State* L = impl_->L;
    impl_->lastError.clear();
    // (H1) mod load gets the load budget; the count hook enforces it.
    impl_->deadline = std::chrono::steady_clock::now() + kLoadBudget;
    try {
        if (luaL_loadfile(L, path) != LUA_OK) {
            impl_->lastError = lua_tostring(L, -1) ? lua_tostring(L, -1) : "load error";
            lua_pop(L, 1);
            impl_->deadline = std::chrono::steady_clock::time_point::max();
            return false;
        }
        if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            impl_->lastError = lua_tostring(L, -1) ? lua_tostring(L, -1) : "run error";
            lua_pop(L, 1);
            impl_->deadline = std::chrono::steady_clock::time_point::max();
            return false;
        }
        impl_->deadline = std::chrono::steady_clock::time_point::max();
        return true;
    } catch (const std::exception& e) {
        impl_->lastError = e.what();
        impl_->deadline = std::chrono::steady_clock::time_point::max();
        return false;
    } catch (...) {
        impl_->lastError = "runFile: internal error";
        impl_->deadline = std::chrono::steady_clock::time_point::max();
        return false;
    }
}

bool RS_LuaState::runString(const char* code, const char* chunkName) {
    lua_State* L = impl_->L;
    impl_->lastError.clear();
    impl_->deadline = std::chrono::steady_clock::now() + kLoadBudget;
    try {
        if (luaL_loadbuffer(L, code, strlen(code),
                            chunkName ? chunkName : "mod") != LUA_OK) {
            impl_->lastError = lua_tostring(L, -1) ? lua_tostring(L, -1) : "load error";
            lua_pop(L, 1);
            impl_->deadline = std::chrono::steady_clock::time_point::max();
            return false;
        }
        if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            impl_->lastError = lua_tostring(L, -1) ? lua_tostring(L, -1) : "run error";
            lua_pop(L, 1);
            impl_->deadline = std::chrono::steady_clock::time_point::max();
            return false;
        }
        impl_->deadline = std::chrono::steady_clock::time_point::max();
        return true;
    } catch (const std::exception& e) {
        impl_->lastError = e.what();
        impl_->deadline = std::chrono::steady_clock::time_point::max();
        return false;
    } catch (...) {
        impl_->lastError = "runString: internal error";
        impl_->deadline = std::chrono::steady_clock::time_point::max();
        return false;
    }
}

// (H5) lock require() to the mod's own directory.
void RS_LuaState::setModDir(const char* dir) {
    lua_State* L = impl_->L;
    std::string d = dir ? dir : "";
    impl_->modDir = d; // also feeds the io.open path lock (empty = deny all)
    std::string path = d.empty() ? "" : d + "/?.lua";
    // (R3) path lives in the registry now (virtualized via __index).
    lua_pushlstring(L, path.c_str(), path.size());
    lua_setfield(L, LUA_REGISTRYINDEX, "rs_package_path");
}

bool RS_LuaState::tick() {
    if (impl_->tickRefs.empty()) return true;
    lua_State* L = impl_->L;
    // (m2) clear any previous error: a recovered mod must not report a
    // phantom error from an older failing tick.
    impl_->lastError.clear();
    // (H1) every tick gets the tick budget, shared across all callbacks.
    impl_->deadline = std::chrono::steady_clock::now() + kTickBudget;
    bool ok = true;
    std::string firstErr;
    // (H3) run EVERY registered callback; one mod's error doesn't silence
    // the others. Stash the first error for RS_GetLuaError.
    // (R1) index with a cached count: a callback may call rs.on_tick
    // (push_back can reallocate), so range-for iterators would dangle.
    // Callbacks registered mid-tick run starting next tick.
    size_t n = impl_->tickRefs.size();
    for (size_t i = 0; i < n; ++i) {
        int ref = impl_->tickRefs[i].ref;
        impl_->activeModDir = impl_->tickRefs[i].modDir; // (R3) per-mod io.open
        // (H-C) per-callback budget: a greedy mod burning just under the
        // shared 100ms every tick would pin the game at ~9fps and starve
        // other mods. Each callback gets 16ms on top of the shared cap.
        auto cbDeadline = std::chrono::steady_clock::now() + kCallbackBudget;
        // Use the earlier of the shared tick deadline and the per-callback one.
        auto savedDeadline = impl_->deadline;
        if (cbDeadline < savedDeadline) impl_->deadline = cbDeadline;
        try {
            lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
            if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
                const char* msg = lua_tostring(L, -1);
                if (firstErr.empty()) firstErr = msg ? msg : "tick error";
                lua_pop(L, 1);
                ok = false;
            }
        } catch (const std::exception& e) {
            if (firstErr.empty()) firstErr = e.what();
            ok = false;
        } catch (...) {
            if (firstErr.empty()) firstErr = "tick: internal error";
            ok = false;
        }
        impl_->deadline = savedDeadline; // restore shared tick deadline
    }
    impl_->deadline = std::chrono::steady_clock::time_point::max();
    if (!ok) {
        impl_->lastError = firstErr;
        if (impl_->logCb) impl_->logCb(impl_->lastError.c_str(), impl_->logUser);
        else fprintf(stderr, "[lua] on_tick error: %s\n", impl_->lastError.c_str());
    }
    return ok;
}

const char* RS_LuaState::lastError() const {
    return impl_->lastError.c_str();
}

// ---------------------------------------------------------------------------
// C entry points (declared by Game/rs_game.h)
// ---------------------------------------------------------------------------

static std::string parentDirOf(const char* path) {
    std::string p(path ? path : "");
    size_t i = p.find_last_of("/\\");
    if (i == std::string::npos) return ".";
    if (i == 0) return "/";
    return p.substr(0, i);
}

extern "C" int RS_RunLuaFile(RSWorld* w, const char* path) {
    if (!w || !path) {
        g_lastLoadError = "RS_RunLuaFile: null world or path";
        return -1;
    }
    try {
        // (H3) multi-mod: load INTO the existing shared state (created on
        // demand). Each mod's rs.on_tick appends; nothing is wiped.
        RS_LuaState* st = RS_LuaState::forWorld(w);
        if (!st) st = new RS_LuaState(w);
        // (H5) lock require() to this mod file's directory.
        st->setModDir(parentDirOf(path).c_str());
        if (!st->runFile(path)) {
            g_lastLoadError = st->lastError();
            // Keep the state: other mods may be loaded and healthy; only
            // report this mod's failure. (RS_LuaReset drops everything.)
            return -1;
        }
        g_lastLoadError.clear();
        return 0;
    } catch (const std::exception& e) {
        g_lastLoadError = e.what();
        return -1;
    } catch (...) {
        g_lastLoadError = "RS_RunLuaFile: internal error";
        return -1;
    }
}

// (H3) Drop the entire Lua state: all mods, all on_tick callbacks, all
// globals. The next RS_RunLuaFile starts completely fresh. Used when the
// enabled-mod set changes (reset + reload enabled ones).
extern "C" void RS_LuaReset(RSWorld* w) {
    if (!w) return;
    delete RS_LuaState::forWorld(w); // dtor erases the g_states entry
}

extern "C" const char* RS_GetLuaError(RSWorld* w) {
    if (RS_LuaState* st = RS_LuaState::forWorld(w)) {
        const char* e = st->lastError();
        if (e && e[0]) return e;
    }
    return g_lastLoadError.empty() ? nullptr : g_lastLoadError.c_str();
}

// (C1) Called by Game's RS_DestroyWorld / RS_ClearWorld. Deletes the Lua
// state bound to w and erases the registry entry so g_states never dangles.
// A later world allocated at the same address gets a fresh state, and
// RS_RunLuaFile(w) after this starts clean (never resurrects the old one).
extern "C" void RS_ScriptingWorldDestroyed(RSWorld* w) {
    RS_LuaReset(w);
}

void RS_LuaTick(RSWorld* w) {
    try {
        if (RS_LuaState* st = RS_LuaState::forWorld(w)) {
            st->tick(); // errors are stashed; never propagate into the game loop
        }
    } catch (...) {
        // tick() already guards internally; this is a last-resort net so a
        // C++ exception can never cross into the game's step loop.
    }
}
