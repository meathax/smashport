-- Bounded MAME-only probe for L8 coin chute input width and source flag routing.
-- It drives controls and observes RAM/CMOS writes; it never changes emulated RAM.
local machine = manager.machine
local screen = machine.screens[":screen"]
local space = machine.devices[":maincpu"].spaces["program"]
local in1 = machine.ioport.ports[":IN1"]

local tests = {
    { label = "coin1-1f", press = 2600, release = 2601, mask = 0x0001 },
    { label = "coin1-4f", press = 2700, release = 2704, mask = 0x0001 },
    { label = "coin1-8f", press = 2800, release = 2808, mask = 0x0001 },
    { label = "coin1-20f", press = 2900, release = 2920, mask = 0x0001 },
    { label = "coin2-4f", press = 3000, release = 3004, mask = 0x0002 },
    { label = "coin3-4f", press = 3100, release = 3104, mask = 0x0200 },
    { label = "bill1-4f", press = 3200, release = 3204, mask = 0x0080 },
}
for _, test in ipairs(tests) do test.field = in1:field(test.mask) end

local c_flags = 0x010ee680
local c_flags_offsets = { [c_flags] = "C_FLAGS+0", [c_flags + 8] = "C_FLAGS+8",
    [c_flags + 16] = "C_FLAGS+16", [c_flags + 24] = "C_FLAGS+24" }
local last_flag_bytes = {}
local cpu = machine.devices[":maincpu"]
local flag_writes = 0
local cmos_writes = 0

local function lane_byte(value, lane)
    return math.floor(value / (256 ^ lane)) % 256
end

local function in_test_window(frame)
    for _, test in ipairs(tests) do
        if frame >= test.press - 2 and frame <= test.release + 8 then return true end
    end
    return false
end

coin_flag_tap = space:install_write_tap(c_flags, c_flags + 31, "l8_coin_flag_sweep", function(offset, data, mask)
    local frame = screen:frame_number()
    if not in_test_window(frame) then return end
    for lane = 0, 3 do
        local address = offset + lane * 8
        local source_offset = c_flags_offsets[address]
        if source_offset then
            local value = lane_byte(data, lane)
            local key = string.format("%08x", address)
            if last_flag_bytes[key] ~= value then
                print(string.format("L8_COIN_FLAG frame=%d source=%s address=%08x old=%s value=%d mask=%08x pc=%08x",
                    frame, source_offset, address, last_flag_bytes[key] == nil and "?" or tostring(last_flag_bytes[key]),
                    value, mask, cpu.state["PC"].value))
                last_flag_bytes[key] = value
                flag_writes = flag_writes + 1
            end
        end
    end
end)

coin_cmos_tap = space:install_write_tap(0x01400000, 0x014010ff, "l8_coin_cmos_sweep", function(offset, data, mask)
    local frame = screen:frame_number()
    if not in_test_window(frame) then return end
    local pc = cpu.state["PC"].value
    print(string.format("L8_COIN_CMOS frame=%d address=%08x data=%08x mask=%08x pc=%08x",
        frame, offset, data, mask, pc))
    cmos_writes = cmos_writes + 1
end)

coin_input_read_tap = space:install_read_tap(0x01c00010, 0x01c0001f, "l8_coin_widget_reads", function(offset, data, mask)
    local frame = screen:frame_number()
    if in_test_window(frame) then
        print(string.format("L8_COIN_INPUT frame=%d address=%08x data=%08x mask=%08x pc=%08x",
            frame, offset, data, mask, cpu.state["PC"].value))
    end
end)

emu.register_frame_done(function()
    local frame = screen:frame_number()
    for _, test in ipairs(tests) do
        if frame == test.press then
            test.field:set_value(1)
            print(string.format("L8_COIN_ACTION label=%s action=press frame=%d width=%d mask=%04x",
                test.label, frame, test.release - test.press, test.mask))
        elseif frame == test.release then
            test.field:clear_value()
            print(string.format("L8_COIN_ACTION label=%s action=release frame=%d mask=%04x",
                test.label, frame, test.mask))
        end
    end
    if frame == 3300 then
        print(string.format("L8_COIN_SUMMARY end=3300 flag_writes=%d cmos_writes=%d", flag_writes, cmos_writes))
        machine:exit()
    end
end, "l8_coin_chute_sweep")

print("L8_COIN_SWEEP_INIT fresh_nvram_required=1 windows=coin1_1_4_8_20f_coin2_coin3_bill1_4f")
