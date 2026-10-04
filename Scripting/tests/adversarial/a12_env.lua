-- Attempt 12: environment/global manipulation
print("_G:", type(_G))
print("_ENV:", type(_ENV))
-- try to get string metatable and modify
local mt = getmetatable("")
print("string metatable:", type(mt))
-- try to pollute _G
_G.evil_global = function() print("evil") end
print("set _G.evil_global: ok")
-- can we reach package via _G?
print("_G.package.loadlib:", _G.package and type(_G.package.loadlib) or "no package")
-- try rawget on registry via getmetatable on a function? (no debug)
local ok, e = pcall(function() return getmetatable(rs.spawn_human) end)
print("getmetatable(rs.spawn_human):", ok and type(e) or ("err: "..tostring(e)))
