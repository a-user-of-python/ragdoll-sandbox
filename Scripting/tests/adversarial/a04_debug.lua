-- Attempt 4: debug library
print("debug:", type(debug))
if debug then
  print("debug.getregistry:", type(debug.getregistry))
  local ok, reg = pcall(debug.getregistry)
  print("getregistry:", ok and "ESCAPED: got registry" or ("blocked: "..tostring(reg)))
  print("debug.getupvalue:", type(debug.getupvalue))
  print("debug.setupvalue:", type(debug.setupvalue))
else
  print("debug is nil (blocked)")
end
