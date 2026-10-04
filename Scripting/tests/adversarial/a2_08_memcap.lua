-- a2_08: allocator — hold 60MB (under cap), then exceed cap; state must survive.
local hold = string.rep("a", 60 * 1024 * 1024)
print("holding 60MB ok, len=" .. #hold)
local ok, err = pcall(function()
    return string.rep("b", 70 * 1024 * 1024)
end)
print("70MB attempt:", ok, err)
print("state alive after OOM: " .. (1 + 1))
hold = nil
collectgarbage("collect")
local ok2 = pcall(function() return string.rep("c", 10 * 1024 * 1024) end)
print("10MB after gc:", ok2)
