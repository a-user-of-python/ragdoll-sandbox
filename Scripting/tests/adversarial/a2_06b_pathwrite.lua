-- a2_06b: package.path overwritten -> require arbitrary Lua file outside mod dir.
package.path = "/tmp/?.lua"
local ok, m = pcall(require, "evilmod")
print("require evilmod:", ok, m and m.secret)
