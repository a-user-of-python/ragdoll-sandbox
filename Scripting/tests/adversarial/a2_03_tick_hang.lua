-- a2_03: infinite loop in on_tick (tick budget 100ms expected).
rs.on_tick(function() while true do end end)
print("registered")
