-- Determine whether the L8 fourth-chute XCOIN route runs with a live P2.
-- Fresh-NVRAM trace: paid P1 start, P2 paid join/start, then coin3 pulse.
local machine = manager.machine
local screen = machine.screens[":screen"]
local cpu = machine.devices[":maincpu"]
local space = cpu.spaces["program"]
local port = machine.ioport.ports[":IN1"]
local coin1, coin2 = port:field(0x0001), port:field(0x0002)
local start1, start2 = port:field(0x0004), port:field(0x0020)
local coin3 = port:field(0x0200)
local c_flags = 0x010ee680
local c_flag_names = { [c_flags] = "C_FLAGS+0", [c_flags + 8] = "C_FLAGS+8",
    [c_flags + 16] = "C_FLAGS+16", [c_flags + 24] = "C_FLAGS+24" }
local last_flags = {}
local flag_writes, cmos_writes = 0, 0

l8_coin3_active_p2_taps = {}
l8_coin3_active_p2_taps[#l8_coin3_active_p2_taps + 1] = space:install_write_tap(
    c_flags, c_flags + 31, "l8_coin3_p2_flags", function(offset, data, mask)
        local frame = screen:frame_number()
        if frame < 2590 or frame > 4900 then return end
        for lane = 0, 3 do
            local address = offset + lane * 8
            if c_flag_names[address] then
                local value = math.floor(data / (256 ^ lane)) % 256
                local key = string.format("%08x", address)
                if last_flags[key] ~= value then
                    print(string.format("L8_COIN_FLAG frame=%d source=%s address=%08x old=%s value=%d pc=%08x",
                        frame, c_flag_names[address], address,
                        last_flags[key] == nil and "?" or tostring(last_flags[key]), value, cpu.state["PC"].value))
                    last_flags[key] = value
                    flag_writes = flag_writes + 1
                end
            end
        end
    end)
l8_coin3_active_p2_taps[#l8_coin3_active_p2_taps + 1] = space:install_write_tap(
    0x01400060, 0x0140017f, "l8_coin3_p2_audits", function(offset, data, mask)
        local frame = screen:frame_number()
        if frame >= 2590 and frame <= 4900 then
            print(string.format("L8_COIN_CMOS frame=%d address=%08x data=%08x mask=%08x pc=%08x",
                frame, offset, data, mask, cpu.state["PC"].value))
            cmos_writes = cmos_writes + 1
        end
    end)
l8_coin3_active_p2_taps[#l8_coin3_active_p2_taps + 1] = space:install_write_tap(
    0x014010e0, 0x014010ef, "l8_coin3_p2_credit", function(offset, data, mask)
        local frame = screen:frame_number()
        if frame >= 2590 and frame <= 4900 then
            print(string.format("L8_CREDIT frame=%d address=%08x data=%08x pc=%08x",
                frame, offset, data, cpu.state["PC"].value))
        end
    end)
l8_coin3_active_p2_taps[#l8_coin3_active_p2_taps + 1] = space:install_read_tap(
    0x01c00010, 0x01c0001f, "l8_coin3_p2_widget", function(offset, data, mask)
        local frame = screen:frame_number()
        if frame >= 4798 and frame <= 4810 then
            print(string.format("L8_COIN3_INPUT frame=%d data=%08x pc=%08x",
                frame, data, cpu.state["PC"].value))
        end
    end)
l8_coin3_active_p2_taps[#l8_coin3_active_p2_taps + 1] = space:install_read_tap(
    0x01404140, 0x0140417f, "l8_coin3_adjustment", function(offset, data, mask)
        local frame = screen:frame_number()
        if frame >= 4798 and frame <= 4810 then
            print(string.format("L8_XCOIN_ADJUSTMENT frame=%d address=%08x data=%08x mask=%08x pc=%08x",
                frame, offset, data, mask, cpu.state["PC"].value))
        end
    end)

emu.register_frame_done(function()
    local frame = screen:frame_number()
    if frame == 2600 then coin1:set_value(1); print("L8_ACTION coin1_press frame=2600")
    elseif frame == 2604 then coin1:clear_value(); print("L8_ACTION coin1_release frame=2604")
    elseif frame == 3150 then start1:set_value(1); print("L8_ACTION start1_press frame=3150")
    elseif frame == 3170 then start1:clear_value(); print("L8_ACTION start1_release frame=3170")
    elseif frame == 4200 then coin2:set_value(1); print("L8_ACTION coin2_press frame=4200")
    elseif frame == 4204 then coin2:clear_value(); print("L8_ACTION coin2_release frame=4204")
    elseif frame == 4500 then start2:set_value(1); print("L8_ACTION start2_press frame=4500")
    elseif frame == 4520 then start2:clear_value(); print("L8_ACTION start2_release frame=4520")
    elseif frame == 4800 then coin3:set_value(1); print("L8_ACTION coin3_press frame=4800 mask=0200")
    elseif frame == 4804 then coin3:clear_value(); print("L8_ACTION coin3_release frame=4804 mask=0200")
    end
    if frame == 4900 then
        print(string.format("L8_COIN3_P2_SUMMARY end=4900 flag_writes=%d cmos_writes=%d", flag_writes, cmos_writes))
        machine:exit()
    end
end, "l8_coin3_active_p2")

print("L8_COIN3_P2_INIT fresh_nvram_required=1 coin3=IN1:0200 p2_start_frame=4500 test_frame=4800")
