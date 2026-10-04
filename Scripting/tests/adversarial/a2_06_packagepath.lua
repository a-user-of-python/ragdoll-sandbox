-- a2_06: package.path lock — is it writable by the mod? traversal? absolute?
print("path0=" .. package.path)
package.path = "/tmp/?.lua;/etc/?.lua"
print("path1=" .. package.path)
-- try absolute require
local ok1, e1 = pcall(require, "hostname")
print("abs require:", ok1, e1)
-- try traversal via module name dots
local ok2, e2 = pcall(require, "....etc.hostname")
print("traversal require:", ok2, e2)
-- preload with function (should be lua-only, fine)
package.preload["evil"] = function() return {x=1} end
local m = require("evil")
print("preload:", m.x)
