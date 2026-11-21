-- Development-only MAME L8 witness: paid P1 start then coin/start P2 in room.
local machine = manager.machine
local screen = machine.screens[":screen"]
local cpu = machine.devices[":maincpu"]
local port = machine.ioport.ports[":IN1"]
local coin1 = port:field(0x0001)
local coin2 = port:field(0x0002)
local start1 = port:field(0x0004)
local start2 = port:field(0x0020)
l8_join_taps = {}
l8_join_taps[1] = cpu.spaces["program"]:install_write_tap(0x014010e0, 0x014010ef, "l8_join_credit", function(offset, data, mask)
    local frame = screen:frame_number()
    if frame >= 2590 and frame <= 4850 then
        print(string.format("L8_CREDIT_WRITE frame=%d time=%.9f address=%08x data=%08x mask=%08x pc=%08x", frame, machine.time:as_double(), offset, data, mask, cpu.state["PC"].value))
    end
end)
emu.register_frame_done(function()
    local frame = screen:frame_number()
    if frame == 2600 then coin1:set_value(1); print("L8_ACTION coin1_press frame=2600")
    elseif frame == 2604 then coin1:clear_value(); print("L8_ACTION coin1_release frame=2604")
    elseif frame == 3150 then start1:set_value(1); print("L8_ACTION start1_press frame=3150")
    elseif frame == 3170 then start1:clear_value(); print("L8_ACTION start1_release frame=3170")
    elseif frame == 4200 then coin2:set_value(1); print("L8_ACTION coin2_press frame=4200")
    elseif frame == 4204 then coin2:clear_value(); print("L8_ACTION coin2_release frame=4204")
    elseif frame == 4500 then start2:set_value(1); print("L8_ACTION start2_press frame=4500")
    elseif frame == 4520 then start2:clear_value(); print("L8_ACTION start2_release frame=4520")
    end
    if frame == 4190 or frame == 4250 or frame == 4490 or frame == 4700 or frame == 4850 then
        local name = string.format("l8-join-%d.png", frame)
        local err = screen:snapshot(name)
        print(string.format("L8_SNAPSHOT frame=%d error=%s", frame, tostring(err)))
    end
end, "l8_second_player_join")
