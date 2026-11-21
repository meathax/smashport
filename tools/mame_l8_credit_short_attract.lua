-- Development-only MAME L8 coin pulse after POST. Tests short-pulse acceptance.
local machine = manager.machine
local screen = machine.screens[":screen"]
local cpu = machine.devices[":maincpu"]
local coin = machine.ioport.ports[":IN1"]:field(0x0001)
l8_short_coin_taps = {}
l8_short_coin_taps[1] = cpu.spaces["program"]:install_write_tap(0x014010e0, 0x014010ef, "l8_short_coin_credit", function(offset, data, mask)
    local frame = screen:frame_number()
    if frame >= 2590 and frame <= 2750 then
        print(string.format("L8_CREDIT_WRITE frame=%d time=%.9f address=%08x data=%08x mask=%08x pc=%08x", frame, machine.time:as_double(), offset, data, mask, cpu.state["PC"].value))
    end
end)
emu.register_frame_done(function()
    local frame = screen:frame_number()
    if frame == 2600 then coin:set_value(1); print("L8_ACTION coin_press frame=2600")
    elseif frame == 2604 then coin:clear_value(); print("L8_ACTION coin_release frame=2604")
    elseif frame == 2750 then
        local err = screen:snapshot("l8-credit-short-2750.png")
        print(string.format("L8_SNAPSHOT frame=2750 error=%s", tostring(err)))
    end
end, "l8_credit_short_attract")
