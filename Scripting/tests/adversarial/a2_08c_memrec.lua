-- a2_08c: OOM recovery — fill near cap, catch OOM, prove state still works.
local t = {}
local ok, err = pcall(function()
    for i = 1, 200000 do t[i] = string.rep("x", 200) end  -- ~40MB+table
end)
print("fill:", ok, err and err:sub(1, 30))
print("alive1:", 40 + 2)
t = nil
collectgarbage("collect")
local s = string.rep("z", 5 * 1024 * 1024)
print("5MB after gc ok:", #s == 5 * 1024 * 1024)
-- interning pressure
local u = {}
local ok2, err2 = pcall(function()
    for i = 1, 1000000 do u["k" .. i] = i end
end)
print("1M unique strings:", ok2, err2 and err2:sub(1, 30))
print("alive2:", 21 * 2)
