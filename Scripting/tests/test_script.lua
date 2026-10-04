-- test_script.lua — exercises every rs.* binding with known arguments.
-- Run by tests/test_lua_bindings.cpp; the C++ side verifies the stub log.

-- 1. spawning (return values checked on the C++ side via stub ids)
h = rs.spawn_human(400, 300)
c = rs.spawn_crate(500, 100, 40)
b = rs.spawn_barrel(600, 100)

-- 2. weapons / effects
rs.explode(400, 300, 150, 900)
rs.fire(100, 200, 0.5, 1)
rs.grenade(300, 400, 250, 300)

-- 3. impulse + despawn
rs.apply_impulse(h, 10, 5000)
rs.despawn(c)

-- 4. print routing (C++ captures via log callback)
print("hello", "mods", 42)

-- 5. per-step callback
ticks = 0
rs.on_tick(function()
  ticks = ticks + 1
  if ticks == 3 then
    rs.spawn_crate(700, 700, 25)
  end
end)

-- 6. sandbox assertions (fail loudly if the sandbox is broken)
assert(os.execute == nil, "os.execute must be removed")
assert(os.exit == nil, "os.exit must be removed")
assert(os.remove == nil, "os.remove must be removed")
assert(io.write == nil, "io.write must be removed")
assert(io.popen == nil, "io.popen must be removed")
assert(package.cpath == "", "package.cpath must be empty")
assert(debug == nil, "debug library must not be loaded")

-- io.open must reject write modes
local ok, err = pcall(function() io.open("/tmp/x", "w") end)
assert(not ok, "io.open write mode must be denied")
