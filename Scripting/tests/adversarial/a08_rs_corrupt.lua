-- Attempt 8: corrupt the rs table
print("rs.spawn_human before:", type(rs.spawn_human))
rs.spawn_human = nil
print("rs.spawn_human after nil:", type(rs.spawn_human))
local ok, e = pcall(rs.spawn_human, 0, 0)
print("call nilled:", ok and "ESCAPED??" or ("blocked: "..tostring(e)))
rs.spawn_human = "not a function"
local ok2, e2 = pcall(rs.spawn_human, 0, 0)
print("call string:", ok2 and "ESCAPED??" or ("blocked: "..tostring(e2)))
-- try to replace with evil function
rs.explode = function() print("FAKE explode") end
rs.explode(0,0,0,0)
print("rs table replaced ok (no crash)")
-- restore sanity for tick
rs.on_tick(function() print("tick still works") end)
