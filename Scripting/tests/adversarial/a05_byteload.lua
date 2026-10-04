-- Attempt 5: bytecode smuggling
print("string.dump:", type(string.dump))
print("load:", type(load))
-- try dumping an rs C function
local ok, d = pcall(string.dump, rs.spawn_human)
print("dump rs.spawn_human:", ok and "ESCAPED: dumped C func" or ("blocked: "..tostring(d)))
-- try dumping a lua function and reloading
local f = function(x) return x + 1 end
local ok2, d2 = pcall(string.dump, f)
print("dump lua func:", ok2 and "ok" or ("blocked: "..tostring(d2)))
if ok2 then
  local ok3, lf = pcall(load, d2)
  print("load bytecode:", ok3 and ("ok, call: "..tostring(lf(41))) or ("blocked: "..tostring(lf)))
end
-- try load with env manipulation
local ok4, lf4 = pcall(load, "return os.execute('id')", "evil", "t", {})
print("load with empty env:", ok4 and "loaded" or ("blocked: "..tostring(lf4)))
