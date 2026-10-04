-- Attempt 1: native code loading via package.loadlib
local r = {}
r.loadlib_type = type(package.loadlib)
if package.loadlib then
  local ok, err = pcall(package.loadlib, "libSystem.dylib", "_system")
  r.loadlib_call = ok and "ESCAPED: loaded" or ("blocked: " .. tostring(err))
else
  r.loadlib_call = "nil (blocked)"
end
-- variant: via package table directly
r.cpath = package.cpath
-- variant: loadlib with different lib
if package.loadlib then
  local ok2, err2 = pcall(package.loadlib, "/usr/lib/libc.dylib", "system")
  r.loadlib2 = ok2 and "ESCAPED" or ("blocked: " .. tostring(err2))
end
for k,v in pairs(r) do print(k, v) end
