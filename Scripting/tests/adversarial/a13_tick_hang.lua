-- Attempt 13: infinite loop inside on_tick (hangs EVERY frame via RS_Step)
rs.on_tick(function()
  while true do end
end)
print("on_tick registered, tick will hang")
