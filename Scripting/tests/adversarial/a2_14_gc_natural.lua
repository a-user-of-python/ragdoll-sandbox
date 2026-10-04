-- a2_14: __gc bomb triggered by NATURAL GC during ticks (no explicit collectgarbage).
-- Plant the bomb, then allocate garbage each tick to force GC cycles.
local bomb = setmetatable({}, {__gc = function() while true do end end})
bomb = nil
local n = 0
rs.on_tick(function()
    n = n + 1
    local t = {}
    for i = 1, 5000 do t[i] = "garbage" .. i end  -- pressure GC
    if n % 20 == 0 then print("tick " .. n .. " survived") end
end)
print("bomb planted")
