-- Attempt 18: coroutine suspended across ticks, resumed with bad state
local co
co = coroutine.create(function()
  while true do
    rs.spawn_human(0, 0)
    coroutine.yield()
  end
end)
rs.on_tick(function()
  local ok, e = coroutine.resume(co)
  if not ok then print("coro died:", e) end
end)
print("coroutine tick registered")
