-- Attempt 17: catastrophic regex backtracking (CPU DoS)
local s = string.rep("a", 30)
local ok, e = pcall(string.match, s, "^(a+)+$")
print("redos 30 chars:", ok and "matched" or ("err: "..tostring(e):sub(1,40)))
