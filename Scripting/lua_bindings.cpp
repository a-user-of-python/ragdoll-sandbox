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

#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

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
    int            tickRef  = LUA_NOREF;
    std::string    lastError;
};

static RS_LuaStateImpl* getImpl(lua_State* L) {
    return static_cast<RS_LuaStateImpl*>(lua_touserdata(L, lua_upvalueindex(1)));
}

// ---------------------------------------------------------------------------
// `rs.*` bindings. Upvalue 1 is always the Impl* (light userdata).
// ---------------------------------------------------------------------------

static int l_spawn_human(lua_State* L) {
    RS_LuaStateImpl* im = getImpl(L);
    float x = (float)luaL_checknumber(L, 1);
    float y = (float)luaL_checknumber(L, 2);
    lua_pushinteger(L, (lua_Integer)RS_SpawnHuman(im->world, x, y));
    return 1;
}

static int l_spawn_crate(lua_State* L) {
    RS_LuaStateImpl* im = getImpl(L);
    float x = (float)luaL_checknumber(L, 1);
    float y = (float)luaL_checknumber(L, 2);
    float s = (float)luaL_checknumber(L, 3);
    lua_pushinteger(L, (lua_Integer)RS_SpawnCrate(im->world, x, y, s));
    return 1;
}

static int l_spawn_barrel(lua_State* L) {
    RS_LuaStateImpl* im = getImpl(L);
    float x = (float)luaL_checknumber(L, 1);
    float y = (float)luaL_checknumber(L, 2);
    lua_pushinteger(L, (lua_Integer)RS_SpawnBarrel(im->world, x, y));
    return 1;
}

static int l_despawn(lua_State* L) {
    RS_LuaStateImpl* im = getImpl(L);
    uint32_t e = (uint32_t)luaL_checkinteger(L, 1);
    RS_Despawn(im->world, e);
    return 0;
}

static int l_explode(lua_State* L) {
    RS_LuaStateImpl* im = getImpl(L);
    float x = (float)luaL_checknumber(L, 1);
    float y = (float)luaL_checknumber(L, 2);
    float r = (float)luaL_checknumber(L, 3);
    float p = (float)luaL_checknumber(L, 4);
    RS_Explode(im->world, x, y, r, p);
    return 0;
}

static int l_fire(lua_State* L) {
    RS_LuaStateImpl* im = getImpl(L);
    float x     = (float)luaL_checknumber(L, 1);
    float y     = (float)luaL_checknumber(L, 2);
    float angle = (float)luaL_checknumber(L, 3);
    int   weapon= (int)luaL_checkinteger(L, 4);
    RS_FireHitscan(im->world, x, y, angle, weapon);
    return 0;
}

static int l_grenade(lua_State* L) {
    RS_LuaStateImpl* im = getImpl(L);
    float x  = (float)luaL_checknumber(L, 1);
    float y  = (float)luaL_checknumber(L, 2);
    float vx = (float)luaL_checknumber(L, 3);
    float vy = (float)luaL_checknumber(L, 4);
    RS_ThrowGrenade(im->world, x, y, vx, vy);
    return 0;
}

static int l_apply_impulse(lua_State* L) {
    RS_LuaStateImpl* im = getImpl(L);
    uint32_t e  = (uint32_t)luaL_checkinteger(L, 1);
    float ix    = (float)luaL_checknumber(L, 2);
    float iy    = (float)luaL_checknumber(L, 3);
    RS_ApplyImpulse(im->world, e, ix, iy);
    return 0;
}

static int l_on_tick(lua_State* L) {
    RS_LuaStateImpl* im = getImpl(L);
    luaL_checktype(L, 1, LUA_TFUNCTION);
    if (im->tickRef != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, im->tickRef);
        im->tickRef = LUA_NOREF;
    }
    lua_pushvalue(L, 1);
    im->tickRef = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_pushboolean(L, 1);
    return 1;
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
    const char* msg = lua_tostring(L, -1);
    if (im->logCb) im->logCb(msg ? msg : "", im->logUser);
    else fprintf(stderr, "[lua] %s\n", msg ? msg : "");
    return 0;
}

// ---------------------------------------------------------------------------
// Sandbox: read-only io, neutered os, no C module loading, no debug lib.
// ---------------------------------------------------------------------------

// io.open wrapper: only read modes allowed.
static int l_io_open_ro(lua_State* L) {
    const char* mode = luaL_optstring(L, 2, "r");
    for (const char* p = mode; *p; ++p) {
        if (*p == 'w' || *p == 'a' || *p == '+')
            return luaL_error(L, "io.open: write access denied by sandbox");
    }
    lua_getfield(L, LUA_REGISTRYINDEX, "rs_io_open_orig");
    lua_pushvalue(L, 1);
    lua_pushvalue(L, 2);
    lua_call(L, 2, LUA_MULTRET);
    int nres = lua_gettop(L) - 2; // drop our own (fname, mode) args
    lua_remove(L, 1);
    lua_remove(L, 1);
    (void)nres;
    return lua_gettop(L);
}

static void sandboxLibraries(lua_State* L) {
    // --- os: strip process and filesystem mutation ---
    lua_getglobal(L, "os");
    if (lua_istable(L, -1)) {
        const char* drop[] = {"execute", "exit", "remove", "rename", nullptr};
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
        lua_getfield(L, -1, "searchers");
        if (lua_istable(L, -1)) {
            // Keep only the preload (1) and Lua-file (2) searchers;
            // drop the C (3) and all-in-one (4) loaders.
            lua_newtable(L);                    // package, searchers, new
            lua_geti(L, -2, 1); lua_seti(L, -2, 1);
            lua_geti(L, -2, 2); lua_seti(L, -2, 2);
            lua_setfield(L, -3, "searchers");    // package.searchers = new
        }
        lua_pop(L, 1); // searchers
    }
    lua_pop(L, 1); // package

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
    : impl_(new RS_LuaStateImpl()), world_(world) {
    impl_->world   = world;
    impl_->logCb   = logCb;
    impl_->logUser = logUser;
    impl_->L       = luaL_newstate();

    openSafeLibraries(impl_->L);
    sandboxLibraries(impl_->L);

    // print -> log (upvalue 1 = Impl*)
    lua_pushlightuserdata(impl_->L, impl_);
    lua_pushcclosure(impl_->L, l_print, 1);
    lua_setglobal(impl_->L, "print");

    // rs table (upvalue 1 = Impl* for every function)
    lua_newtable(impl_->L);
    lua_pushlightuserdata(impl_->L, impl_);
    luaL_setfuncs(impl_->L, kRsLib, 1);
    lua_setglobal(impl_->L, "rs");

    g_states[world] = this;
}

RS_LuaState::~RS_LuaState() {
    auto it = g_states.find(world_);
    if (it != g_states.end() && it->second == this) g_states.erase(it);
    if (impl_->tickRef != LUA_NOREF)
        luaL_unref(impl_->L, LUA_REGISTRYINDEX, impl_->tickRef);
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
    if (luaL_loadfile(L, path) != LUA_OK) {
        impl_->lastError = lua_tostring(L, -1) ? lua_tostring(L, -1) : "load error";
        lua_pop(L, 1);
        return false;
    }
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        impl_->lastError = lua_tostring(L, -1) ? lua_tostring(L, -1) : "run error";
        lua_pop(L, 1);
        return false;
    }
    return true;
}

bool RS_LuaState::runString(const char* code, const char* chunkName) {
    lua_State* L = impl_->L;
    impl_->lastError.clear();
    if (luaL_loadbuffer(L, code, strlen(code),
                        chunkName ? chunkName : "mod") != LUA_OK) {
        impl_->lastError = lua_tostring(L, -1) ? lua_tostring(L, -1) : "load error";
        lua_pop(L, 1);
        return false;
    }
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        impl_->lastError = lua_tostring(L, -1) ? lua_tostring(L, -1) : "run error";
        lua_pop(L, 1);
        return false;
    }
    return true;
}

bool RS_LuaState::tick() {
    if (impl_->tickRef == LUA_NOREF) return true;
    lua_State* L = impl_->L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, impl_->tickRef);
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        const char* msg = lua_tostring(L, -1);
        impl_->lastError = msg ? msg : "tick error";
        lua_pop(L, 1);
        if (impl_->logCb) impl_->logCb(impl_->lastError.c_str(), impl_->logUser);
        else fprintf(stderr, "[lua] on_tick error: %s\n", impl_->lastError.c_str());
        return false;
    }
    return true;
}

const char* RS_LuaState::lastError() const {
    return impl_->lastError.c_str();
}

// ---------------------------------------------------------------------------
// C entry points (declared by Game/rs_game.h)
// ---------------------------------------------------------------------------

extern "C" int RS_RunLuaFile(RSWorld* w, const char* path) {
    if (!w || !path) {
        g_lastLoadError = "RS_RunLuaFile: null world or path";
        return -1;
    }
    delete RS_LuaState::forWorld(w); // clean reload: drop previous mod state
    RS_LuaState* st = new RS_LuaState(w);
    if (!st->runFile(path)) {
        g_lastLoadError = st->lastError();
        delete st; // never leave a half-loaded mod behind
        return -1;
    }
    g_lastLoadError.clear();
    return 0;
}

extern "C" const char* RS_GetLuaError(RSWorld* w) {
    if (RS_LuaState* st = RS_LuaState::forWorld(w)) {
        const char* e = st->lastError();
        if (e && e[0]) return e;
    }
    return g_lastLoadError.empty() ? nullptr : g_lastLoadError.c_str();
}

void RS_LuaTick(RSWorld* w) {
    if (RS_LuaState* st = RS_LuaState::forWorld(w)) {
        st->tick(); // errors are stashed; never propagate into the game loop
    }
}
