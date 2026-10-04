-- Attempt 6: package.searchers manipulation
print("searchers count:", #package.searchers)
-- try to add a malicious searcher
table.insert(package.searchers, function(name)
  print("MALICIOUS SEARCHER CALLED for", name)
  return function() print("evil module loaded") end
end)
print("after insert, count:", #package.searchers)
local ok, m = pcall(require, "evil_mod_xyz")
print("require evil:", ok and "searcher ran" or ("blocked: "..tostring(m)))
-- try to restore and check C searcher is gone
print("searcher 3 type:", type(package.searchers[3]))
