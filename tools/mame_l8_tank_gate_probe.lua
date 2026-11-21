-- Development-only L8 first-room/tank table probe. No runtime port dependency.
-- A candidate ROM signature is not a resolved DRINF symbol until accessed.
local machine = manager.machine
local screen = machine.screens[":screen"]
local cpu = machine.devices[":maincpu"]
local space = cpu.spaces["program"]
local port = machine.ioport.ports[":IN1"]
local coin = port:field(0x0001)
local start1 = port:field(0x0004)
local reads = 0
l8_tank_probe_taps = {}
l8_tank_probe_taps[1] = space:install_read_tap(0xffe3b1f0, 0xffe3b28f, "l8_drinf_candidate", function(offset, data, mask)
    if reads < 48 then
        print(string.format("L8_TABLE_READ sample=%d frame=%d emutime=%.9f address=%08x data=%08x mask=%08x pc=%08x", reads, screen:frame_number(), machine.time:as_double(), offset, data, mask, cpu.state["PC"].value))
        reads = reads + 1
    end
end)
emu.register_frame_done(function()
    local frame = screen:frame_number()
    if frame == 1050 then
        coin:set_value(1)
        print("L8_ACTION coin_press frame=1050")
    elseif frame == 1060 then
        coin:clear_value()
        print("L8_ACTION coin_release frame=1060")
    elseif frame == 1100 then
        start1:set_value(1)
        print("L8_ACTION start_press frame=1100")
    elseif frame == 1110 then
        start1:clear_value()
        print("L8_ACTION start_release frame=1110")
    elseif frame == 1150 or frame == 1700 or frame == 2500 or frame == 3200 then
        local name = string.format("l8-tank-%d.png", frame)
        local err = screen:snapshot(name)
        print(string.format("L8_SNAPSHOT frame=%d name=%s error=%s", frame, name, tostring(err)))
    end
end, "l8_tank_probe")
print("L8_TANK_PROBE candidate_start=ffe3b1f0 candidate_end=ffe3b28f")
