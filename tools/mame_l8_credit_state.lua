-- Development-only MAME 0.289 L8 mapped-CMOS witness for coin/start.
-- Y-unit CMOS is an nvram_device-owned pointer, not a Lua memory share.
local machine = manager.machine
local screen = machine.screens[":screen"]
local cpu = machine.devices[":maincpu"]
local space = cpu.spaces["program"]
local port = machine.ioport.ports[":IN1"]
local coin = port:field(0x0001)
local start1 = port:field(0x0004)
local writes = 0
l8_credit_state_taps = {}
l8_credit_state_taps[1] = space:install_write_tap(0x01400000, 0x0140ffff, "l8_credit_cmos", function(offset, data, mask)
    local frame = screen:frame_number()
    if frame >= 1049 and frame <= 1140 then
        if writes < 160 then
            print(string.format("L8_CMOS_WRITE sample=%d frame=%d emutime=%.9f address=%08x data=%08x mask=%08x pc=%08x", writes, frame, machine.time:as_double(), offset, data, mask, cpu.state["PC"].value))
        end
        writes = writes + 1
    end
end)
emu.register_frame_done(function()
    local frame = screen:frame_number()
    if frame == 1050 then coin:set_value(1); print("L8_CREDIT_ACTION coin_press frame=1050")
    elseif frame == 1060 then coin:clear_value(); print("L8_CREDIT_ACTION coin_release frame=1060")
    elseif frame == 1100 then start1:set_value(1); print("L8_CREDIT_ACTION start_press frame=1100")
    elseif frame == 1110 then start1:clear_value(); print("L8_CREDIT_ACTION start_release frame=1110")
    elseif frame == 1140 then print(string.format("L8_CREDIT_SUMMARY writes=%d", writes)) end
end, "l8_credit_state")
print("L8_CREDIT_INIT cmos_start=01400000 cmos_end=0140ffff")
