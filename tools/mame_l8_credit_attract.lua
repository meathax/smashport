-- Development-only MAME L8 witness: coin/start after POST and into attract.
local machine = manager.machine
local screen = machine.screens[":screen"]
local cpu = machine.devices[":maincpu"]
local space = cpu.spaces["program"]
local port = machine.ioport.ports[":IN1"]
local coin = port:field(0x0001)
local start1 = port:field(0x0004)
local writes = 0
l8_attract_coin_taps = {}
l8_attract_coin_taps[1] = space:install_write_tap(0x01400000, 0x0140ffff, "l8_attract_coin_cmos", function(offset, data, mask)
    local frame = screen:frame_number()
    if frame >= 2500 and frame <= 4250 then
        if writes < 100 then
            print(string.format("L8_CMOS_WRITE sample=%d frame=%d time=%.9f address=%08x data=%08x mask=%08x pc=%08x", writes, frame, machine.time:as_double(), offset, data, mask, cpu.state["PC"].value))
        end
        writes = writes + 1
    end
end)
emu.register_frame_done(function()
    local frame = screen:frame_number()
    if frame == 2600 then coin:set_value(1); print("L8_ACTION coin_press frame=2600")
    elseif frame == 2850 then coin:clear_value(); print("L8_ACTION coin_release frame=2850")
    elseif frame == 3150 then start1:set_value(1); print("L8_ACTION start_press frame=3150")
    elseif frame == 3170 then start1:clear_value(); print("L8_ACTION start_release frame=3170")
    end
    if frame == 2500 or frame == 2590 or frame == 2870 or frame == 3140 or frame == 3220 or frame == 3500 or frame == 3900 or frame == 4200 then
        local name = string.format("l8-credit-attract-%d.png", frame)
        local err = screen:snapshot(name)
        print(string.format("L8_SNAPSHOT frame=%d name=%s error=%s", frame, name, tostring(err)))
    end
    if frame == 4250 then print(string.format("L8_SUMMARY cmos_writes=%d", writes)) end
end, "l8_credit_attract")
print("L8_CREDIT_ATTRACT_INIT")
