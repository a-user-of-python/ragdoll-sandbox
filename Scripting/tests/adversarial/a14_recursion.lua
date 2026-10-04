-- Attempt 14: deep recursion (Lua stack overflow handling)
local function f(n) return f(n+1) end
local ok, e = pcall(f, 1)
print("deep recursion:", ok and "no error?!" or ("clean error: "..tostring(e):sub(1,50)))
