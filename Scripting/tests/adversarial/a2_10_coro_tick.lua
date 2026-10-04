-- a2_10: coroutine resumed across ticks, each resume burns ~50ms.
-- Hook must fire inside the coroutine on every resume (deadline per tick).
local co = coroutine.create(function()
    while true do
        local t0 = os.clock()
        while os.clock() - t0 < 0.05 do end
        coroutine.yield()
    end
end)
rs.on_tick(function()
    local ok, err = coroutine.resume(co)
    if not ok then print("co died: " .. tostring(err)) end
end)
print("co armed")
