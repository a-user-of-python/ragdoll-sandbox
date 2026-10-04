-- Attempt 11: bad args to rs functions (crash test)
local tests = {
  function() rs.spawn_human() end,                    -- missing args
  function() rs.spawn_human(nil, nil) end,             -- nil args
  function() rs.spawn_human("a", "b") end,            -- strings (coercible? "a" is not)
  function() rs.spawn_human("10", "20") end,          -- numeric strings (should work)
  function() rs.spawn_human(1e308, 1e308) end,        -- huge
  function() rs.spawn_human(0/0, 0/0) end,            -- NaN
  function() rs.spawn_human(math.huge, -math.huge) end,
  function() rs.explode(0, 0, -5, -100) end,           -- negative radius/power
  function() rs.fire(0, 0, 0, 99999) end,              -- invalid weapon id
  function() rs.fire(0, 0, 0, -1) end,
  function() rs.despawn(4294967295) end,               -- max uint32
  function() rs.despawn(-1) end,                       -- negative (wraps to huge)
  function() rs.apply_impulse(12345, 1e30, 1e30) end,  -- nonexistent entity, huge impulse
  function() rs.on_tick("not a function") end,
  function() rs.on_tick(nil) end,
}
for i, fn in ipairs(tests) do
  local ok, e = pcall(fn)
  print(string.format("test %2d: %s %s", i, ok and "OK" or "LUA_ERROR", ok and "" or tostring(e):sub(1,60)))
end
print("all bad-arg tests done (no crash = good)")
