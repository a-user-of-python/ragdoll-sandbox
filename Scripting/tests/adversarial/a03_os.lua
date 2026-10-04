-- Attempt 3: os escapes
for _, fn in ipairs({"execute","exit","remove","rename"}) do
  print("os."..fn..":", type(os[fn]))
end
local ok, e = pcall(os.execute, "touch /tmp/adv_os_pwned")
print("os.execute:", ok and "ESCAPED" or ("blocked: "..tostring(e)))
print("os.getenv:", type(os.getenv))
local okg, g = pcall(os.getenv, "HOME")
print("os.getenv('HOME'):", okg and ("returned: "..tostring(g)) or ("blocked: "..tostring(g)))
print("os.tmpname:", type(os.tmpname))
local okt, t = pcall(function() if os.tmpname then return os.tmpname() end end)
print("os.tmpname():", okt and ("returned: "..tostring(t)) or ("blocked: "..tostring(t)))
print("os.clock (should work):", type(os.clock))
print("os.time (should work):", type(os.time))
