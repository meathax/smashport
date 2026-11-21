-- Development-only MAME 0.289 L8 input/interrupt phase witness.
-- MAME drops Lua memory taps if their handles are collected.
local machine = manager.machine
local screen = machine.screens[":screen"]
local cpu = machine.devices[":maincpu"]
local space = cpu.spaces["program"]
local start_time, end_time = 19.20, 19.32
local inputs, interrupts = 0, 0
l8_input_phase_taps = {}
for port = 0, 3 do
    local first = 0x01c00000 + port * 0x10
    l8_input_phase_taps[#l8_input_phase_taps + 1] = space:install_read_tap(first, first + 0x0f, "l8_input_phase_" .. port, function(offset, data, mask)
        local now = machine.time:as_double()
        if now >= start_time and now < end_time and inputs < 96 then
            print(string.format("L8_INPUT sample=%d port=%d frame=%d emutime=%.9f address=%08x data=%08x mask=%08x pc=%08x", inputs, port, screen:frame_number(), now, offset, data, mask, cpu.state["PC"].value))
            inputs = inputs + 1
        end
    end)
end
l8_input_phase_taps[#l8_input_phase_taps + 1] = space:install_write_tap(0xc00000a0, 0xc00000af, "l8_input_phase_dpyint", function(offset, data, mask)
    local now = machine.time:as_double()
    if now >= start_time and now < end_time and interrupts < 24 then
        print(string.format("L8_INTERRUPT sample=%d frame=%d emutime=%.9f data=%08x pc=%08x", interrupts, screen:frame_number(), now, data, cpu.state["PC"].value))
        interrupts = interrupts + 1
    end
end)
print(string.format("L8_INPUT_INIT start=%.2f end=%.2f", start_time, end_time))
