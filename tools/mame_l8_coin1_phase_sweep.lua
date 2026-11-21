-- Bounded MAME-only one-frame coin1 pulses distributed across frame phases.
-- This observes ordinary input/RAM/CMOS execution; it never changes emulated RAM.
local machine = manager.machine
local screen = machine.screens[":screen"]
local space = machine.devices[":maincpu"].spaces["program"]
local cpu = machine.devices[":maincpu"]
local coin1 = machine.ioport.ports[":IN1"]:field(0x0001)

local tests = {}
for trial = 0, 15 do
    -- 41-frame spacing advances the absolute frame phase by 9 modulo 16.
    -- Across 16 trials this covers each frame-number phase once.
    local press = 2600 + trial * 41
    tests[#tests + 1] = { trial = trial, press = press, release = press + 1 }
end

local c_flags = 0x010ee680
local audit_units = 0x01400090
local credits = 0x014010e0
local last_flag
local flag_writes, audit_writes, credit_writes = 0, 0, 0

local function test_for_frame(frame)
    for _, test in ipairs(tests) do
        if frame >= test.press - 2 and frame <= test.release + 10 then return test end
    end
end

coin1_phase_flag_tap = space:install_write_tap(c_flags, c_flags + 15, "l8_coin1_phase_flag", function(offset, data, mask)
    local frame = screen:frame_number()
    local test = test_for_frame(frame)
    if not test then return end
    local value = data % 256
    if value ~= last_flag then
        print(string.format("L8_COIN_PHASE_FLAG trial=%02d frame=%d offset=%d old=%s value=%d mask=%08x pc=%08x",
            test.trial, frame, frame - test.press, last_flag == nil and "?" or tostring(last_flag),
            value, mask, cpu.state["PC"].value))
        last_flag = value
        flag_writes = flag_writes + 1
    end
end)

coin1_phase_audit_tap = space:install_write_tap(audit_units, audit_units + 15, "l8_coin1_phase_audit", function(offset, data, mask)
    local frame = screen:frame_number()
    local test = test_for_frame(frame)
    if not test then return end
    if offset == audit_units then
        print(string.format("L8_COIN_PHASE_AUDIT trial=%02d frame=%d units=%d data=%08x pc=%08x",
            test.trial, frame, data, data, cpu.state["PC"].value))
        audit_writes = audit_writes + 1
    end
end)

coin1_phase_credit_tap = space:install_write_tap(credits, credits + 15, "l8_coin1_phase_credit", function(offset, data, mask)
    local frame = screen:frame_number()
    local test = test_for_frame(frame)
    if not test then return end
    print(string.format("L8_COIN_PHASE_CREDIT trial=%02d frame=%d credits=%d data=%08x pc=%08x",
        test.trial, frame, data, data, cpu.state["PC"].value))
    credit_writes = credit_writes + 1
end)

emu.register_frame_done(function()
    local frame = screen:frame_number()
    for _, test in ipairs(tests) do
        if frame == test.press then
            coin1:set_value(1)
            print(string.format("L8_COIN_PHASE_ACTION trial=%02d action=press frame=%d phase=%d",
                test.trial, frame, frame % 16))
        elseif frame == test.release then
            coin1:clear_value()
            print(string.format("L8_COIN_PHASE_ACTION trial=%02d action=release frame=%d phase=%d",
                test.trial, frame, frame % 16))
        end
    end
    if frame == tests[#tests].release + 11 then
        print(string.format("L8_COIN_PHASE_SUMMARY end=%d flag_writes=%d audit_writes=%d credit_writes=%d",
            frame, flag_writes, audit_writes, credit_writes))
        machine:exit()
    end
end, "l8_coin1_phase_sweep")

print("L8_COIN_PHASE_SWEEP_INIT fresh_nvram_required=1 chute=coin1 pulse=1_frame trials=16")
