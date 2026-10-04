# Ragdoll Sandbox — Lua Modding API

Mods are plain Lua 5.4 scripts placed in the app's `Documents/Mods/` folder
(the app lists them in Settings → Mods, where they can be enabled/disabled
and reloaded). Each enabled mod runs once at load, and may register a
per-step callback with `rs.on_tick`.

All game interaction goes through the global `rs` table. World coordinates
are in points: +x right, +y **up**.

## Functions

### `rs.spawn_human(x, y) -> id`
Spawns a human ragdoll at `(x, y)`. Returns the entity id (> 0).

```lua
local bob = rs.spawn_human(400, 300)
```

### `rs.spawn_crate(x, y, size) -> id`
Spawns a breakable wooden crate of the given size (width/height in points).

```lua
local c = rs.spawn_crate(500, 100, 40)
```

### `rs.spawn_barrel(x, y) -> id`
Spawns an explosive barrel. Damaging it (or fire) detonates it.

```lua
local b = rs.spawn_barrel(600, 100)
```

### `rs.despawn(id)`
Removes an entity. Invalid ids are ignored.

```lua
rs.despawn(bob)
```

### `rs.explode(x, y, radius, power)`
Detonation: radial impulse + damage + fire and smoke particles.

```lua
rs.explode(400, 300, 150, 900)
```

### `rs.fire(x, y, angle, weapon)`
Fires a hitscan weapon from `(x, y)` along `angle` (radians, 0 = +x).
`weapon`: `0` pistol, `1` rifle, `2` shotgun, `3` sniper, `4` smg.

```lua
rs.fire(100, 200, 0, 1)  -- rifle shot to the right
```

### `rs.grenade(x, y, vx, vy)`
Throws a grenade from `(x, y)` with velocity `(vx, vy)`. Explodes on fuse.

```lua
rs.grenade(300, 400, 250, 300)
```

### `rs.apply_impulse(id, ix, iy)`
Applies an impulse `(ix, iy)` to an entity (e.g. fling a ragdoll).

```lua
rs.apply_impulse(bob, 0, 5000)  -- launch upward
```

### `rs.on_tick(fn) -> true`
Registers `fn` to run every physics step. Re-registering replaces the
previous callback. Errors are caught: the message is available in
Settings → Mods and via `RS_GetLuaError`; a failing tick never crashes
the game.

```lua
local t = 0
rs.on_tick(function()
  t = t + 1
  if t % 60 == 0 then print("a second passed") end
end)
```

## The `print` function

`print(...)` works as usual but routes to the app's mod log
(Settings → Mods shows recent output) instead of stdout.

## Sandbox rules

Mods run in a restricted Lua environment:

- `os.execute`, `os.exit`, `os.remove`, `os.rename` are **removed**.
- `io` is **read-only**: `io.write`, `io.popen`, `io.output` removed;
  `io.open` rejects write/append modes.
- `package.cpath` is emptied and the C module loaders are removed —
  mods cannot load native code. Pure-Lua `require` still works.
- The `debug` library is not loaded.

`math`, `string`, `table`, `coroutine`, `utf8`, `os.clock`, `os.time`,
`os.date`, `dofile`, `load`/`loadfile` are available.

## Example: meteor shower

```lua
-- meteor_shower.lua — rains exploding barrels from the sky.
-- See sample_mods/meteor_shower.lua for the full working mod.

local timer = 0

rs.on_tick(function()
  timer = timer + 1
  if timer % 45 == 0 then            -- every 0.75s at 60Hz
    local x = math.random(100, 900)
    local b = rs.spawn_barrel(x, 700)
    rs.apply_impulse(b, math.random(-200, 200), -300)
  end
end)

print("meteor shower armed")
```

Tips:
- Spawn ids let you track entities across ticks; store them in locals.
- Keep per-tick work small — the callback runs inside the physics step.
- Use `print` liberally while developing; check the mod log for errors.
