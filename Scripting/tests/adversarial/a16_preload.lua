-- Attempt 16: package.preload injection
package.preload["sneaky"] = function()
  print("PRELOAD RAN")
  return {evil = true}
end
local ok, m = pcall(require, "sneaky")
print("require via preload:", ok and "ran (lua-only, no escape)" or ("blocked: "..tostring(m)))
-- can preload return something dangerous? only lua values possible
