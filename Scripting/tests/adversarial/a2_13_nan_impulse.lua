-- a2_13: NaN impulse via Lua into the real game (N3 zombie test).
-- If the N3 sanitizer works, no NaN positions after stepping.
local e = rs.spawn_human(0, 100)
print("spawned", e)
rs.apply_impulse(e, 0/0, 0/0)
rs.apply_impulse(e, 1e308 * 10, -(1e308 * 10))
print("nan impulses applied")
