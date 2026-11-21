-- Development-only executed L8 DRINF index-4 and following pointer read.
local machine = manager.machine
local screen = machine.screens[":screen"]
local cpu = machine.devices[":maincpu"]
local space = cpu.spaces["program"]
local port = machine.ioport.ports[":IN1"]
local coin = port:field(0x0001)
local start1 = port:field(0x0004)
local index_reads, derefs = 0, 0
l8_drinf_deref_taps = {}
l8_drinf_deref_taps[1] = space:install_read_tap(0xffe3b270, 0xffe3b28f, "l8_drinf_index4", function(offset, data, mask)
    if index_reads < 32 then
        print(string.format("L8_INDEX4 sample=%d frame=%d emutime=%.9f address=%08x data=%08x mask=%08x pc=%08x", index_reads, screen:frame_number(), machine.time:as_double(), offset, data, mask, cpu.state["PC"].value))
        index_reads = index_reads + 1
    end
end)
l8_drinf_deref_taps[2] = space:install_read_tap(0x18915600, 0x1891563f, "l8_drinf_target", function(offset, data, mask)
    if derefs < 32 then
        print(string.format("L8_DEREF sample=%d frame=%d emutime=%.9f address=%08x data=%08x mask=%08x pc=%08x", derefs, screen:frame_number(), machine.time:as_double(), offset, data, mask, cpu.state["PC"].value))
        derefs = derefs + 1
    end
end)
emu.register_frame_done(function()
    local frame = screen:frame_number()
    if frame == 1050 then coin:set_value(1)
    elseif frame == 1060 then coin:clear_value()
    elseif frame == 1100 then start1:set_value(1)
    elseif frame == 1110 then start1:clear_value() end
end, "l8_drinf_deref")
print("L8_DRINF_DEREF index=ffe3b270 target=18915610")
