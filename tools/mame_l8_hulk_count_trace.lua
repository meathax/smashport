-- Development-only MAME 0.289 reference trace for HULK_CNT in a paid first room.
-- This observer does not modify guest memory or enter the shipped executable.
local machine = manager.machine
local screen = machine.screens[":screen"]
local cpu = machine.devices[":maincpu"]
local space = cpu.spaces["program"]
local port = machine.ioport.ports[":IN1"]
local coin = port:field(0x0001)
local start1 = port:field(0x0004)
local hcount_reads = 0
local hulk_count_writes = 0

l8_hulk_count_taps = {}
l8_hulk_count_taps[1] = space:install_write_tap(0x010de5d0, 0x010de5df, "l8_hulk_count", function(offset, data, mask)
    local frame = screen:frame_number()
    if frame >= 3000 and frame <= 4300 then
        print(string.format("L8_HULK_CNT_WRITE sample=%d frame=%d emutime=%.9f address=%08x value=%08x mask=%08x pc=%08x",
            hulk_count_writes, frame, machine.time:as_double(), offset, data, mask, cpu.state["PC"].value))
        hulk_count_writes = hulk_count_writes + 1
    end
end)
l8_hulk_count_taps[2] = space:install_read_tap(0xc00001c0, 0xc00001cf, "l8_hulk_count_hcount", function()
    if screen:frame_number() == 3533 then hcount_reads = hcount_reads + 1 end
end)

emu.register_frame_done(function()
    local frame = screen:frame_number()
    if frame == 2600 then coin:set_value(1); print("L8_ACTION coin1_press frame=2600")
    elseif frame == 2604 then coin:clear_value(); print("L8_ACTION coin1_release frame=2604")
    elseif frame == 3150 then start1:set_value(1); print("L8_ACTION start1_press frame=3150")
    elseif frame == 3170 then start1:clear_value(); print("L8_ACTION start1_release frame=3170")
    end
    if frame == 3533 or frame == 3534 or frame == 3550 or frame == 3600 or frame == 3800 or frame == 4200 then
        local name = string.format("l8-paid-hulk-count-%d.png", frame)
        local err = screen:snapshot(name)
        print(string.format("L8_SNAPSHOT frame=%d name=%s error=%s", frame, name, tostring(err)))
    end
    if frame == 4300 then
        print(string.format("L8_SUMMARY hulk_count_writes=%d hcount_reads_frame3533=%d", hulk_count_writes, hcount_reads))
    end
end, "l8_hulk_count_trace")

print("L8_HULK_CNT_INIT address=010de5d8 source=HULKS.SETUP_HULK")
