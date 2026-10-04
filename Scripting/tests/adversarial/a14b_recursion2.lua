-- Non-tail recursion (should hit Lua stack limit cleanly)
local function f(n) return 1 + f(n+1) end
local ok, e = pcall(f, 1)
print("non-tail recursion:", ok and "NO ERROR?!" or ("clean error: "..tostring(e):sub(1,60)))
