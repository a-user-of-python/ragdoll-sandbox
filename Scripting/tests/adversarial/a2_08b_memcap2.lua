-- a2_08b: hold 40MB, exceed cap via pcall, verify state survives and recovers.
local hold = string.rep("a", 40 * 1024 * 1024)
print("holding 40MB ok")
local ok, err = pcall(string.rep, "b", 70 * 1024 * 1024)
print("70MB attempt:", ok, err)
print("alive1:", 1 + 1)
hold = nil
collectgarbage("collect")
local s = string.rep("c", 10 * 1024 * 1024)
print("10MB after gc ok, len=" .. #s)
-- string interning pressure: many unique strings
local t = {}
local ok2, err2 = pcall(function()
    for i = 1, 3000000 do t[i] = "str" .. i end
end)
print("3M strings:", ok2, err2 and err2:sub(1, 40))
print("alive2:", 2 + 2)
