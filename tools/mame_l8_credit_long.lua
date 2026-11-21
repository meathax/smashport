-- Development-only L8 witness: sustained coin pulse, delayed start and visible state.
-- The source coin debounce can outlast a short input pulse; no native runtime use.
local machine = manager.machine
local screen = machine.screens[":screen"]
local cpu = machine.devices[":maincpu"]
local space = cpu.spaces["program"]
local port = machine.ioport.ports[":IN1"]
local coin = port:field(0x0001)
local start1 = port:field(0x0004)
local writes = 0
l8_credit_long_taps = {}
l8_credit_long_taps[1] = space:install_write_tap(0x01400000, 0x0140ffff, "l8_credit_long_cmos", function(offset, data, mask)
    local frame = screen:frame_number()
    if frame >= 1050 and frame <= 1700 then
        if writes < 100 then
            print(string.format("L8_CMOS_WRITE sample=%d frame=%d time=%.9f address=%08x data=%08x mask=%08x pc=%08x", writes, frame, machine.time:as_double(), offset, data, mask, cpu.state["PC"].value))
        end
        writes = writes + 1
    end
end)
emu.register_frame_done(function()
    local frame = screen:frame_number()
    if frame == 1050 then coin:set_value(1); print("L8_ACTION coin_press frame=1050")
    elseif frame == 1390 then coin:clear_value(); print("L8_ACTION coin_release frame=1390")
    elseif frame == 1450 then start1:set_value(1); print("L8_ACTION start_press frame=1450")
    elseif frame == 1470 then start1:clear_value(); print("L8_ACTION start_release frame=1470")
    end
    if frame == 1040 or frame == 1395 or frame == 1440 or frame == 1490 or frame == 1680 then
        local name = string.format("l8-credit-long-%d.png", frame)
        local err = screen:snapshot(name)
        print(string.format("L8_SNAPSHOT frame=%d name=%s error=%s", frame, name, tostring(err)))
    end
    if frame == 1700 then print(string.format("L8_SUMMARY cmos_writes=%d", writes)) end
end, "l8_credit_long")
print("L8_CREDIT_LONG_INIT")
