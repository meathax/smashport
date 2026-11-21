-- Development-only MAME 0.289 L8 observer: UTIL.ASM:RAND around the paid
-- first-room OHULK burst (frame 3533). It never writes RAM or registers.
--
-- Purpose: confirm the room-entry RAND value that the native differential
-- (tests/test_extended.cpp, "L8 paid first room ONSCRN_HULKS/OHULK ...")
-- solved from the captured HCOUNT stream: 0xCBF5010A or 0xCBF5010B (the
-- low bit is unobservable through RANDOM's outputs), with RAND=0x1F07E710
-- after the 34th Hulk draw.
--
-- RAND is the .BSS long at bit address 0x01085220 (L8 RANDOM at 0xFFE06B50
-- reads it; 0xFFE06BE0 stores it). HCOUNT is read at 0xC00001C0.
-- Run with the same fresh config/NVRAM, coin/start schedule as the prior
-- paid-start traces, e.g.:
--   mame smashtv -rompath <roms> -cfg_directory <fresh> -nvram_directory <fresh>
--        -window -nothrottle -seconds_to_run 80 -autoboot_script mame_l8_rand_room_entry.lua
local machine = manager.machine
local screen = machine.screens[":screen"]
local cpu = machine.devices[":maincpu"]
local space = cpu.spaces["program"]

local rand_address = 0x01085220
local hcount_address = 0xc00001c0
local first_frame, last_frame = 3532, 3534
local sequence = 0

local function in_window()
    local frame = screen:frame_number()
    return frame >= first_frame and frame <= last_frame
end

local function emit(kind, address, data, mask)
    sequence = sequence + 1
    print(string.format("L8_RAND_TAP seq=%d kind=%s frame=%d emutime=%.9f address=%08x data=%08x mask=%08x pc=%08x a13=%08x",
        sequence, kind, screen:frame_number(), machine.time:as_double(), address, data, mask,
        cpu.state["PC"].value, cpu.state["A13"].value))
end

l8_rand_taps = {}
l8_rand_taps[1] = space:install_read_tap(rand_address, rand_address + 0x1f, "l8_rand_read",
    function(address, data, mask) if in_window() then emit("RAND_READ", address, data, mask) end end)
l8_rand_taps[2] = space:install_write_tap(rand_address, rand_address + 0x1f, "l8_rand_write",
    function(address, data, mask) if in_window() then emit("RAND_WRITE", address, data, mask) end end)
l8_rand_taps[3] = space:install_read_tap(hcount_address, hcount_address + 0xf, "l8_rand_hcount",
    function(address, data, mask) if in_window() then emit("HCOUNT_READ", address, data, mask) end end)

local port = machine.ioport.ports[":IN1"]
local coin = port:field(0x0001)
local start1 = port:field(0x0004)
emu.register_frame_done(function()
    local frame = screen:frame_number()
    if frame == 2600 then coin:set_value(1); print("L8_ACTION coin1_press frame=2600")
    elseif frame == 2604 then coin:clear_value(); print("L8_ACTION coin1_release frame=2604")
    elseif frame == 3150 then start1:set_value(1); print("L8_ACTION start1_press frame=3150")
    elseif frame == 3170 then start1:clear_value(); print("L8_ACTION start1_release frame=3170")
    elseif frame == 3540 then
        print(string.format("L8_RAND_DONE frame=3540 events=%d", sequence))
        machine:exit()
    end
end, "l8_rand_room_entry")

print(string.format("L8_RAND_PROBE rand=%08x hcount=%08x window=%d..%d", rand_address, hcount_address, first_frame, last_frame))
