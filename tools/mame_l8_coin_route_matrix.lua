-- Bounded MAME-only simultaneous-chute, service-credit and diagnostic coin probe.
-- All stimuli use the documented L8 IN1 fields; emulated RAM is never poked.
local machine = manager.machine
local screen = machine.screens[":screen"]
local cpu = machine.devices[":maincpu"]
local space = cpu.spaces["program"]
local in1 = machine.ioport.ports[":IN1"]
local coin1, coin2 = in1:field(0x0001), in1:field(0x0002)
local test_switch, service_credit = in1:field(0x0010), in1:field(0x0040)

local actions = {
    {frame=2600, field=coin1, down=true, label="simultaneous_coin1_press"},
    {frame=2600, field=coin2, down=true, label="simultaneous_coin2_press"},
    {frame=2604, field=coin1, down=false, label="simultaneous_coin1_release"},
    {frame=2604, field=coin2, down=false, label="simultaneous_coin2_release"},
    {frame=2700, field=service_credit, down=true, label="service_credit_press"},
    {frame=2704, field=service_credit, down=false, label="service_credit_release"},
    {frame=2800, field=test_switch, down=true, label="diagnostic_test_press"},
    {frame=2804, field=test_switch, down=false, label="diagnostic_test_release"},
    {frame=2810, field=coin1, down=true, label="coin_during_diagnostic_press"},
    {frame=2811, field=coin1, down=false, label="coin_during_diagnostic_release"},
}

local c_flags = 0x010ee680
local last_flags = {}
local watched_cmos = {[0x01400090]="AUDLCOIN",[0x014000f0]="AUDRCOIN",
    [0x01400210]="AUDSERV",[0x01400c90]="AUDSTAT",[0x014010e0]="CREDITS"}
local last_cmos = {}
local function in_window(frame)
    return (frame >= 2598 and frame <= 2620) or
           (frame >= 2698 and frame <= 2720) or
           (frame >= 2798 and frame <= 2840)
end

coin_route_flags = space:install_write_tap(c_flags, c_flags + 31, "l8_coin_route_flags", function(offset, data, mask)
    local frame = screen:frame_number()
    if not in_window(frame) then return end
    for lane = 0, 3 do
        local address = offset + lane * 8
        if address == c_flags or address == c_flags + 8 then
            local value = math.floor(data / (256 ^ lane)) % 256
            local key = string.format("%08x", address)
            if last_flags[key] ~= value then
                print(string.format("L8_ROUTE_FLAG frame=%d name=%s value=%d old=%s pc=%08x",
                    frame, address == c_flags and "C_FLAGS+0" or "C_FLAGS+8", value,
                    last_flags[key] == nil and "?" or tostring(last_flags[key]), cpu.state["PC"].value))
                last_flags[key] = value
            end
        end
    end
end)

coin_route_cmos = space:install_write_tap(0x01400000, 0x014010ff, "l8_coin_route_cmos", function(offset, data, mask)
    local frame = screen:frame_number()
    if not in_window(frame) then return end
    local name = watched_cmos[offset]
    if name and last_cmos[name] ~= data then
        print(string.format("L8_ROUTE_CMOS frame=%d name=%s address=%08x value=%d pc=%08x",
            frame, name, offset, data, cpu.state["PC"].value))
        last_cmos[name] = data
    end
end)

coin_route_input = space:install_read_tap(0x01c00010, 0x01c0001f, "l8_coin_route_input", function(offset, data, mask)
    local frame = screen:frame_number()
    if in_window(frame) then
        print(string.format("L8_ROUTE_INPUT frame=%d data=%08x pc=%08x", frame, data, cpu.state["PC"].value))
    end
end)

emu.register_frame_done(function()
    local frame = screen:frame_number()
    for _, action in ipairs(actions) do
        if frame == action.frame then
            if action.down then action.field:set_value(1) else action.field:clear_value() end
            print(string.format("L8_ROUTE_ACTION label=%s action=%s frame=%d",
                action.label, action.down and "press" or "release", frame))
        end
    end
    if frame == 2840 then
        print("L8_ROUTE_SUMMARY end=2840")
        machine:exit()
    end
end, "l8_coin_route_matrix")

print("L8_ROUTE_MATRIX_INIT fresh_nvram_required=1 simultaneous_service_diagnostic=1")
