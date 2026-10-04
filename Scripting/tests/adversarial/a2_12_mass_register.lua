-- a2_12: mass on_tick registration — 200k callbacks at load.
-- Memory cap should trip OR tick must stay bounded.
for i = 1, 200000 do
    rs.on_tick(function() end)
end
print("registered 200k")
