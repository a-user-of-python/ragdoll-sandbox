-- a2_02: __gc metamethod with infinite loop, triggered via collectgarbage.
local t = setmetatable({}, {__gc = function() while true do end end})
t = nil
print("before gc")
collectgarbage("collect")
print("after gc - should not reach here without timeout")
