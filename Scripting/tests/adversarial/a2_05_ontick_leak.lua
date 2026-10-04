-- a2_05: on_tick self-re-registration leak.
-- Each tick adds one more callback; vector grows unboundedly.
rs.on_tick(function()
    rs.on_tick(function() end)
end)
print("leak armed")
