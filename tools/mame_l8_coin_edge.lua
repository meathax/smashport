-- Development-only MAME 0.289 L8 active-low coin-input witness.
-- Uses MAME's input-field override, not memory mutation or an emulated CPU in the port.
local machine = manager.machine
local screen = machine.screens[":screen"]
local cpu = machine.devices[":maincpu"]
local space = cpu.spaces["program"]
local coin = machine.ioport.ports[":IN1"]:field(0x0001)
local count = 0
l8_coin_edge_taps = {}
l8_coin_edge_taps[1] = space:install_read_tap(0x01c00010, 0x01c0001f, "l8_coin_edge", function(offset, data, mask)
    local frame = screen:frame_number()
    if frame >= 1048 and frame <= 1057 and count < 160 then
        print(string.format("L8_COIN_READ sample=%d frame=%d emutime=%.9f data=%08x mask=%08x pc=%08x", count, frame, machine.time:as_double(), data, mask, cpu.state["PC"].value))
        count = count + 1
    end
end)
emu.register_frame_done(function()
    local frame = screen:frame_number()
    if frame == 1048 then
        coin:set_value(1)
        print(string.format("L8_COIN_EVENT action=press frame=%d emutime=%.9f", frame, machine.time:as_double()))
    elseif frame == 1052 then
        coin:clear_value()
        print(string.format("L8_COIN_EVENT action=release frame=%d emutime=%.9f", frame, machine.time:as_double()))
    end
end, "l8_coin_edge")
print("L8_COIN_INIT port=IN1 mask=0001 press_frame=1048 release_frame=1052")
