-- a2_09: pcall/timeout interaction — timed-out mod must leave state usable.
local ok, err = pcall(function() while true do end end)
print("hang pcall:", ok, err)
print("alive after timeout:", 7 * 6)
rs.on_tick(function() print("tick still works") end)
