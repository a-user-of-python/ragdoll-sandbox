-- a2_07: print cap — 1MB string must be truncated, no blowup.
local big = string.rep("x", 1024 * 1024)
local t0 = os.clock()
print(big)
print("print done in " .. string.format("%.2f", os.clock() - t0) .. "s")
