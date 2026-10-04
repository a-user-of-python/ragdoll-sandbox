// lua_bindings.h — Ragdoll Sandbox Lua modding layer.
//
// C++ wrapper around an embedded PUC Lua state, plus the C entry points the
// game/app use. All original glue code. No Apple dependencies.
//
// Ownership note: this module defines RS_RunLuaFile, RS_GetLuaError,
// RS_LuaReset and RS_LuaTick (the first two are also declared by the
// Game/rs_game.h contract). The Game module must NOT define them; it only
// calls RS_LuaTick from its step loop. The Game module MUST call
// RS_ScriptingWorldDestroyed from RS_DestroyWorld and RS_ClearWorld so bound
// states are deleted with their worlds.
//
// Multi-mod: one lua_State per world, shared by all enabled mods.
// RS_RunLuaFile loads INTO the existing state (creating it on demand);
// each mod's rs.on_tick appends to a list and every callback fires each tick.
// RS_LuaReset drops the whole state (used when the enabled-mod set changes).
//
// DoS hardening: an instruction-count hook enforces wall-clock budgets
// (mod load ~5s, on_tick ~100ms per tick) and a custom allocator caps each
// state at 64MB. Both raise catchable Lua errors, never crashes.
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
    // false and lastError() holds the message. Loads INTO the existing
    // state so multiple mods coexist (see RS_RunLuaFile).
    bool runFile(const char* path);
    bool runString(const char* code, const char* chunkName = "mod");

    // Invoke every rs.on_tick callback (in registration order) in protected
    // mode. On error returns false and lastError() holds the FIRST error;
    // remaining callbacks still run. Errors never throw into the game.
    bool tick();

    // Last error message (empty string if none). Valid until next call.
    const char* lastError() const;

    // Change the log sink after construction.
    void setLogCallback(RS_LogCallback cb, void* user);

    // Restrict require() to the given directory (H5). Called by
    // RS_RunLuaFile with the mod file's parent dir.
    void setModDir(const char* dir);

    // Find the state bound to a world (nullptr if none).
    static RS_LuaState* forWorld(RSWorld* w);

private:
    RS_LuaStateImpl* impl_;
    RSWorld* world_;
};

// Called by the game loop once per RS_Step. Looks up the RS_LuaState for w
// (no-op when none) and runs its on_tick callbacks. Errors are stashed and
// retrievable via RS_GetLuaError; a failing tick does NOT throw into the game.
void RS_LuaTick(RSWorld* w);

// Drop the entire Lua state bound to w (all mods, all on_tick callbacks).
// Used when the enabled-mod set changes: call RS_LuaReset, then RS_RunLuaFile
// for each enabled mod. Safe on null w and on worlds with no state.
extern "C" void RS_LuaReset(RSWorld* w);

// Called by the Game module from RS_DestroyWorld / RS_ClearWorld. Deletes the
// RS_LuaState bound to w (if any) and erases the registry entry, so the map
// never keeps a dangling pointer to a freed world. Safe on null w and on
// worlds with no state. After this call, RS_RunLuaFile(w) starts completely
// fresh (it never resurrects the old state).
extern "C" void RS_ScriptingWorldDestroyed(RSWorld* w);

#endif // RS_LUA_BINDINGS_H
