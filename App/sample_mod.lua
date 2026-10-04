-- sample_mod.lua — bundled example mod for Ragdoll Sandbox.
-- Copy of this file is placed in Documents/Mods on first launch.
-- Uses only the documented rs.* Lua API (see Scripting/LUA_API.md).
-- All original code.

-- Every 3 seconds, drop a barrel from the sky at a random x.
local timer = 0

rs.on_tick(function(dt)
    timer = timer + dt
    if timer >= 3.0 then
        timer = 0
        local x = (math.random() - 0.5) * 800
        local id = rs.spawn_barrel(x, 400)
        print("sample_mod: dropped barrel id=" .. tostring(id))
    end
end)

print("sample_mod loaded: barrels will rain every 3 seconds")
