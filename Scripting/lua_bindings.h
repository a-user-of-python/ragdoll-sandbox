// lua_bindings.h — Ragdoll Sandbox Lua modding layer.
//
// C++ wrapper around an embedded PUC Lua state, plus the C entry points the
// game/app use. All original glue code. No Apple dependencies.
//
// Ownership note: this module defines RS_RunLuaFile, RS_GetLuaError and
// RS_LuaTick (declared by the Game/rs_game.h contract). The Game module must
// NOT define them; it only calls RS_LuaTick from its step loop.
//
// Threading: the game loop is single-threaded; this module is not thread-safe.

#ifndef RS_LUA_BINDINGS_H
#define RS_LUA_BINDINGS_H

#include <cstddef>

// Opaque world handle, owned by the Game module.
struct RSWorld;

// Log sink for Lua print() and mod diagnostics. May be nullptr (= stderr).
typedef void (*RS_LogCallback)(const char* message, void* userdata);

// Forward-declared implementation (defined in lua_bindings.cpp).
struct RS_LuaStateImpl;

// RAII wrapper around a sandboxed lua_State bound to one RSWorld.
class RS_LuaState {
public:
    // Creates the state, opens sanitized libs, installs the `rs` table.
    // Registers itself for RS_LuaTick lookup. Not copyable.
    explicit RS_LuaState(RSWorld* world,
                         RS_LogCallback logCb = nullptr,
                         void* logUser = nullptr);
    ~RS_LuaState();

    RS_LuaState(const RS_LuaState&) = delete;
    RS_LuaState& operator=(const RS_LuaState&) = delete;

    RSWorld* world() const { return world_; }

    // Run a mod file / chunk. Returns true on success; on failure returns
    // false and lastError() holds the message.
    bool runFile(const char* path);
    bool runString(const char* code, const char* chunkName = "mod");

    // Invoke the rs.on_tick callback (if any) in protected mode.
    // On error returns false and lastError() holds the message.
    bool tick();

    // Last error message (empty string if none). Valid until next call.
    const char* lastError() const;

    // Change the log sink after construction.
    void setLogCallback(RS_LogCallback cb, void* user);

    // Find the state bound to a world (nullptr if none).
    static RS_LuaState* forWorld(RSWorld* w);

private:
    RS_LuaStateImpl* impl_;
    RSWorld* world_;
};

// Called by the game loop once per RS_Step. Looks up the RS_LuaState for w
// (no-op when none) and runs its on_tick callback. Errors are stashed and
// retrievable via RS_GetLuaError; a failing tick does NOT throw into the game.
void RS_LuaTick(RSWorld* w);

#endif // RS_LUA_BINDINGS_H
