-- Exploratory MAME-only trace of TMS34010 registers at L8 HULK_CNT writes.
-- This observer neither changes emulated memory nor supplies native gameplay.
local machine = manager.machine
local screen = machine.screens[":screen"]
local cpu = machine.devices[":maincpu"]
local space = cpu.spaces["program"]
local port = machine.ioport.ports[":IN1"]
local coin = port:field(0x0001)
local start1 = port:field(0x0004)
local count_writes = 0

local function reg_hex(name)
    local ok, value = pcall(function() return cpu.state[name].value end)
    if not ok then return "unavailable" end
    return string.format("%08x", value)
end

local function memory_hex(width, address)
    local ok, value = pcall(function() return space["read_u" .. width](space, address) end)
    if not ok then return "unavailable" end
    return string.format("%08x", value)
end

l8_hulk_count_context_taps = {}
l8_hulk_count_context_taps[1] = space:install_write_tap(0x010de5d0, 0x010de5df, "l8_hulk_count_context", function(offset, data, mask)
    local frame = screen:frame_number()
    if frame >= 3000 and frame <= 4300 then
        local process = cpu.state["A13"].value
        print(string.format("L8_HULK_CONTEXT sample=%d frame=%d emutime=%.9f address=%08x value=%08x mask=%08x pc=%08x a13=%s a12=%s a11=%s a8=%s a0=%s a1=%s",
            count_writes, frame, machine.time:as_double(), offset, data, mask, cpu.state["PC"].value,
            reg_hex("A13"), reg_hex("A12"), reg_hex("A11"), reg_hex("A8"), reg_hex("A0"), reg_hex("A1")))
        if count_writes < 5 then
            local object = cpu.state["A8"].value
            print(string.format("L8_HULK_OBJECT sample=%d procid=%s ptime=%s psptr=%s object=%s object_process_link=%s xpos=%s ypos=%s hage=%s dir=%s cflags=%s skdir=%s level=%s skplyr=%s",
                count_writes,
                memory_hex(16, process + 0x20), memory_hex(16, process + 0x30), memory_hex(32, process + 0x40),
                reg_hex("A8"), memory_hex(32, object + 0x160), memory_hex(16, object + 0x70),
                memory_hex(16, object + 0x90), memory_hex(16, process + 0x100), memory_hex(16, process + 0x110),
                memory_hex(16, process + 0x130), memory_hex(16, process + 0x140), memory_hex(16, process + 0x150),
                memory_hex(8, process + 0x200)))
        end
        count_writes = count_writes + 1
    end
end)

emu.register_frame_done(function()
    local frame = screen:frame_number()
    if frame == 2600 then coin:set_value(1); print("L8_ACTION coin1_press frame=2600")
    elseif frame == 2604 then coin:clear_value(); print("L8_ACTION coin1_release frame=2604")
    elseif frame == 3150 then start1:set_value(1); print("L8_ACTION start1_press frame=3150")
    elseif frame == 3170 then start1:clear_value(); print("L8_ACTION start1_release frame=3170")
    elseif frame == 4300 then print(string.format("L8_SUMMARY hulk_context_writes=%d", count_writes))
    end
end, "l8_hulk_count_context")

print(string.format("L8_HULK_CONTEXT_INIT address=010de5d8 source=HULKS.HULK_CNT space_shift=%d data_width=%d",
    space.shift, space.data_width))
