-- Attempt 15: memory exhaustion
print("starting memory bomb...")
local t = {}
for i = 1, 10000000 do
  t[i] = string.rep("x", 100)
  if i % 1000000 == 0 then print("allocated", i) end
end
print("survived?")
