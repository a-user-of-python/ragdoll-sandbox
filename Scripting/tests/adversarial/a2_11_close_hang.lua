-- a2_11: __close (to-be-closed) with infinite loop — hooks should fire here.
local obj = setmetatable({}, {__close = function() while true do end end})
local function f()
    local x <close> = obj
    print("in scope")
end
f()
print("after scope - should not reach without timeout")
