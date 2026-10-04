-- a2_01: infinite loop inside a coroutine.
-- Lua 5.4 hooks are per-coroutine; lua_sethook on the main thread may NOT
-- cover this coroutine -> potential permanent hang.
local co = coroutine.create(function()
    while true do end
end)
local ok, err = coroutine.resume(co)
print("resume returned:", ok, err)
