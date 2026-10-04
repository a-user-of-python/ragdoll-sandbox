-- a2_02b: __gc with FINITE but long loop — does the hook fire at all in __gc?
local t = setmetatable({}, {__gc = function()
    local s = 0
    for i = 1, 200000000 do s = s + i end
end})
t = nil
print("before gc")
collectgarbage("collect")
print("after gc")
