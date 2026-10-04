-- Attempt 9: load() sandbox escape
local ok, f = pcall(load, "os.execute('touch /tmp/adv_load_pwned')")
print("load os.execute:", ok and "loaded" or ("blocked: "..tostring(f)))
if ok then
  local ok2, e2 = pcall(f)
  print("run it:", ok2 and "RAN (check os.execute nil)" or ("blocked: "..tostring(e2)))
end
-- load with _ENV override
local ok3, f3 = pcall(load, "return _ENV", "t", "t", {x=1})
print("load with env:", ok3 and "loaded" or ("blocked: "..tostring(f3)))
-- dostring (should not exist in 5.4)
print("dostring:", type(dostring))
print("loadfile:", type(loadfile))
local ok4, e4 = pcall(loadfile, "/tmp/t.lua")
print("loadfile:", ok4 and "loaded" or ("blocked: "..tostring(e4)))
