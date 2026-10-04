-- Attempt 2: io escapes
print("tmpfile:", type(io.tmpfile))
local ok1, e1 = pcall(io.tmpfile)
print("tmpfile():", ok1 and "ESCAPED: got handle" or ("blocked: "..tostring(e1)))
if ok1 then
  local okw, ew = pcall(function() return e1:write("pwned") end)
  print("tmpfile write:", okw and "ESCAPED: wrote" or ("blocked: "..tostring(ew)))
end
local ok2, e2 = pcall(io.open, "/tmp/adv_pwned.txt", "w")
print("io.open w:", ok2 and "ESCAPED: opened for write" or ("blocked: "..tostring(e2)))
local ok3, e3 = pcall(io.popen, "id")
print("io.popen:", ok3 and "ESCAPED" or ("blocked: "..tostring(e3)))
print("io.stdout:", type(io.stdout))
local ok4, e4 = pcall(function() io.stdout:write("x") end)
print("io.stdout:write:", ok4 and "ESCAPED" or ("blocked: "..tostring(e4)))
local ok5, e5 = pcall(function() io.stderr:write("x") end)
print("io.stderr:write:", ok5 and "ESCAPED" or ("blocked: "..tostring(e5)))
local ok6, f6 = pcall(io.open, "/tmp/adv_runner.cpp", "r")
print("io.open r (should work):", ok6 and "OK readable" or ("blocked: "..tostring(f6)))
