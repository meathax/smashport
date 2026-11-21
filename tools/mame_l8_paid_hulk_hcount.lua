-- Development-only MAME 0.289 trace for L8 coin/start, mapped credit and HCOUNT.
-- The script does not claim a first-room launch; its observations decide that.
-- This reference script is never loaded by the native Windows executable.
local machine = manager.machine
local screen = machine.screens[":screen"]
local cpu = machine.devices[":maincpu"]
local space = cpu.spaces["program"]
local port = machine.ioport.ports[":IN1"]
local coin = port:field(0x0001)
local start1 = port:field(0x0004)
local reads = 0
local credit_writes = 0

l8_paid_hulk_hcount_taps = {}
l8_paid_hulk_hcount_taps[2] = space:install_write_tap(0x014010e0, 0x014010ef, "l8_mapped_credit", function(offset, data, mask)
    local frame = screen:frame_number()
    if frame >= 2500 and frame <= 3800 then
        print(string.format("L8_CREDIT_WRITE sample=%d frame=%d emutime=%.9f address=%08x value=%08x mask=%08x pc=%08x",
            credit_writes, frame, machine.time:as_double(), offset, data, mask, cpu.state["PC"].value))
        credit_writes = credit_writes + 1
    end
end)
l8_paid_hulk_hcount_taps[1] = space:install_read_tap(0xc00001c0, 0xc00001cf, "l8_paid_hulk_hcount", function(offset, data, mask)
    local frame = screen:frame_number()
    if frame >= 3000 and frame <= 4300 then
        if frame >= 3525 and frame <= 3550 and reads < 2048 then
            print(string.format("L8_RAND_HCOUNT read=%d frame=%d emutime=%.9f address=%08x value=%08x mask=%08x pc=%08x",
                reads, frame, machine.time:as_double(), offset, data, mask, cpu.state["PC"].value))
        end
        reads = reads + 1
    end
end)

emu.register_frame_done(function()
    local frame = screen:frame_number()
    if frame == 2600 then
        coin:set_value(1)
        print("L8_ACTION coin_press frame=2600")
    elseif frame == 2604 then
        coin:clear_value()
        print("L8_ACTION coin_release frame=2604")
    elseif frame == 3150 then
        start1:set_value(1)
        print("L8_ACTION start_press frame=3150")
    elseif frame == 3170 then
        start1:clear_value()
        print("L8_ACTION start_release frame=3170")
    end
    if frame == 2500 or frame == 2600 or frame == 2606 or frame == 2700 or frame == 3140 or frame == 3150 or frame == 3500 or frame == 3900 or frame == 4200 then
        print(string.format("L8_CHECKPOINT frame=%d emutime=%.9f", frame, machine.time:as_double()))
    end
    if frame == 2500 or frame == 2600 or frame == 2606 or frame == 2700 or frame == 3140 or frame == 3200 or frame == 3525 or frame == 3533 or frame == 3534 or frame == 3550 or frame == 4200 then
        local name = string.format("l8-paid-hulk-hcount-%d.png", frame)
        local err = screen:snapshot(name)
        print(string.format("L8_SNAPSHOT frame=%d name=%s error=%s", frame, name, tostring(err)))
    end
    if frame == 4300 then
        print(string.format("L8_SUMMARY coin_hcount_reads=%d mapped_credit_writes=%d", reads, credit_writes))
    end
end, "l8_paid_hulk_hcount")

print("L8_COIN_HULK_HCOUNT_INIT credit=014010e0 hcount=c00001c0 coin=2600..2604 start=3150..3170")
