-- meteor_shower.lua — sample mod for Ragdoll Sandbox.
--
-- Rains explosive barrels from the sky on a timer. Uses only the documented
-- `rs` API (see Scripting/LUA_API.md). Drop into Documents/Mods/ and enable
-- it in Settings -> Mods.

local SPAWN_MIN_X = 100
local SPAWN_MAX_X = 900
local SPAWN_Y     = 700
local EVERY_N_TICKS = 45   -- ~0.75s at a 60Hz step

local timer = 0
local spawned = 0

rs.on_tick(function()
  timer = timer + 1
  if timer % EVERY_N_TICKS == 0 then
    local x = math.random(SPAWN_MIN_X, SPAWN_MAX_X)
    local barrel = rs.spawn_barrel(x, SPAWN_Y)
    if barrel ~= 0 then
      -- Give it a random sideways shove and a downward push for drama.
      rs.apply_impulse(barrel, math.random(-250, 250), -400)
      spawned = spawned + 1
    end
    -- Every 10 barrels, drop a bonus explosion at a random spot.
    if spawned % 10 == 0 then
      rs.explode(math.random(SPAWN_MIN_X, SPAWN_MAX_X), 200, 120, 700)
    end
  end
end)

print("meteor shower armed: raining barrels every " .. EVERY_N_TICKS .. " ticks")
