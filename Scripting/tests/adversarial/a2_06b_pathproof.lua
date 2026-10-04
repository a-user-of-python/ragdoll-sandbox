-- a2_06b: prove package.path overwrite enables require outside the mod dir.
-- Target: an existing benign test file in the adversarial dir (no new files).
if not _DONE66 then
  _DONE66 = true
  package.path = "/home/hatch/workspace/ragdoll-sandbox/Scripting/tests/adversarial/?.lua"
  local ok, e = pcall(require, "a2_06b_helper")
  print("outside-moddir require:", ok, (e and e.name) or e)
end
