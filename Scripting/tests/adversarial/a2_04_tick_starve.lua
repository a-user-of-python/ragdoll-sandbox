-- a2_04: tick starvation. Mod A burns ~95ms every tick; does mod B's on_tick run?
local b_ran = 0
rs.on_tick(function()
    local t0 = os.clock()
    while os.clock() - t0 < 0.095 do end  -- burn 95ms
end)
rs.on_tick(function() b_ran = b_ran + 1 end)
print("registered A and B")
-- runner will tick 5x; afterwards we can't read b_ran from C, so stash it:
rs.on_tick(function() print("B_RAN=" .. b_ran) end)
