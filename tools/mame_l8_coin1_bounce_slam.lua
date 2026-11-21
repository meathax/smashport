-- Bounded MAME-only L8 coin1 bounce and tilt/slam interaction probe.
-- Inputs are changed through MAME's input fields; emulated RAM is untouched.
local machine = manager.machine
local screen = machine.screens[":screen"]
local space = machine.devices[":maincpu"].spaces["program"]
local cpu = machine.devices[":maincpu"]
local in1 = machine.ioport.ports[":IN1"]
local coin1, slam = in1:field(0x0001), in1:field(0x0008)

local actions = {
    { frame = 2600, input = coin1, down = true, label = "baseline_coin1_press" },
    { frame = 2601, input = coin1, down = false, label = "baseline_coin1_release" },
    { frame = 2603, input = coin1, down = true, label = "bounce_coin1_press" },
    { frame = 2604, input = coin1, down = false, label = "bounce_coin1_release" },
    { frame = 2700, input = coin1, down = true, label = "slam_coin1_press" },
    { frame = 2701, input = coin1, down = false, label = "slam_coin1_release" },
    { frame = 2702, input = slam, down = true, label = "slam_press" },
    { frame = 2710, input = coin1, down = true, label = "during_slam_coin1_press" },
    { frame = 2711, input = coin1, down = false, label = "during_slam_coin1_release" },
    { frame = 2718, input = slam, down = false, label = "slam_release" },
    { frame = 2800, input = coin1, down = true, label = "recovery_coin1_press" },
    { frame = 2801, input = coin1, down = false, label = "recovery_coin1_release" },
}

local c_flags = 0x010ee680
local audit_units, credits = 0x01400090, 0x014010e0
local current_flag
local function in_window(frame)
    return (frame >= 2598 and frame <= 2620) or
           (frame >= 2698 and frame <= 2730) or
           (frame >= 2798 and frame <= 2820)
end

coin1_bounce_slam_flag_tap = space:install_write_tap(c_flags, c_flags + 15, "l8_coin1_bounce_slam_flag", function(offset, data, mask)
    local frame = screen:frame_number()
    if not in_window(frame) or offset ~= c_flags then return end
    local value = data % 256
    if value ~= current_flag then
        print(string.format("L8_COIN_BOUNCE_FLAG frame=%d value=%d old=%s mask=%08x pc=%08x",
            frame, value, current_flag == nil and "?" or tostring(current_flag), mask, cpu.state["PC"].value))
        current_flag = value
    end
end)
coin1_bounce_slam_audit_tap = space:install_write_tap(audit_units, audit_units + 15, "l8_coin1_bounce_slam_audit", function(offset, data, mask)
    local frame = screen:frame_number()
    if in_window(frame) and offset == audit_units then
        print(string.format("L8_COIN_BOUNCE_AUDIT frame=%d total=%d pc=%08x", frame, data, cpu.state["PC"].value))
    end
end)
coin1_bounce_slam_credit_tap = space:install_write_tap(credits, credits + 15, "l8_coin1_bounce_slam_credit", function(offset, data, mask)
    local frame = screen:frame_number()
    if in_window(frame) then
        print(string.format("L8_COIN_BOUNCE_CREDIT frame=%d total=%d pc=%08x", frame, data, cpu.state["PC"].value))
    end
end)
coin1_bounce_slam_input_tap = space:install_read_tap(0x01c00010, 0x01c0001f, "l8_coin1_bounce_slam_inputs", function(offset, data, mask)
    local frame = screen:frame_number()
    if frame >= 2698 and frame <= 2720 then
        print(string.format("L8_COIN_BOUNCE_INPUT frame=%d address=%08x data=%08x pc=%08x",
            frame, offset, data, cpu.state["PC"].value))
    end
end)

emu.register_frame_done(function()
    local frame = screen:frame_number()
    for _, action in ipairs(actions) do
        if frame == action.frame then
            if action.down then action.input:set_value(1) else action.input:clear_value() end
            print(string.format("L8_COIN_BOUNCE_ACTION label=%s action=%s frame=%d",
                action.label, action.down and "press" or "release", frame))
        end
    end
    if frame == 2820 then
        print("L8_COIN_BOUNCE_SUMMARY end=2820")
        machine:exit()
    end
end, "l8_coin1_bounce_slam")

print("L8_COIN_BOUNCE_INIT fresh_nvram_required=1 baseline_bounce_slam_and_recovery=1")
