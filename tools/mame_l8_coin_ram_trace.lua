-- Development-only observer for the retail L8 coin timer path.
-- It watches main RAM writes after one controlled post-POST coin pulse.
-- It never changes emulated memory and exits after the bounded capture.
local machine = manager.machine
local screen = machine.screens[":screen"]
local cpu = machine.devices[":maincpu"]
local space = cpu.spaces["program"]
local coin = machine.ioport.ports[":IN1"]:field(0x0001)
local candidates = {}
local candidate_count = 0
local write_count = 0
local watched = {}
local watched_events = 0

local function lane_byte(value, lane)
    return math.floor(value / (256 ^ lane)) % 256
end

local function record_write(offset, data, mask)
    local frame = screen:frame_number()
    if frame < 2600 or frame > 2610 then return end
    write_count = write_count + 1
    local pc = cpu.state["PC"].value
    for lane = 0, 3 do
        local value = lane_byte(data, lane)
        local key = string.format("%08x:%d", offset, lane)
        local state = candidates[key]
        -- Ignore the high process-stack area. A fresh byte value 30 written
        -- just after the coin edge is the candidate timer-byte initial value.
        if value == 30 and not state and frame >= 2601 and frame <= 2603 and offset < 0x010ff000 and candidate_count < 64 then
            state = { offset = offset, lane = lane, value = value, frame = frame, pc = pc }
            candidates[key] = state
            candidate_count = candidate_count + 1
            print(string.format("L8_COIN_TIMER_CANDIDATE frame=%d address=%08x lane=%d value=%d data=%08x mask=%08x pc=%08x",
                frame, offset, lane, value, data, mask, pc))
            -- TMS34010 addresses are bit-addressed; the source offsets 0, 8,
            -- 16 and 24 identify adjacent bytes in the packed coin word.
            for delta = -32, 32, 8 do
                for watched_lane = 0, 1 do
                    local watch_offset = offset + delta
                    local watch_key = string.format("%08x:%d", watch_offset, watched_lane)
                    if not watched[watch_key] then
                        watched[watch_key] = { offset = watch_offset, lane = watched_lane, value = nil }
                    end
                end
            end
        end
        local watch = watched[key]
        if watch and watched_events < 2000 and (watch.value == nil or watch.value ~= value) then
            print(string.format("L8_COIN_TIMER_WORD_WRITE frame=%d address=%08x lane=%d old=%s new=%d data=%08x mask=%08x pc=%08x",
                frame, offset, lane, watch.value == nil and "?" or tostring(watch.value), value, data, mask, pc))
            watch.value = value
            watched_events = watched_events + 1
        end
    end
end

l8_coin_ram_tap = space:install_write_tap(0x01000000, 0x010fffff, "l8_coin_mainram", record_write)
l8_coin_cmos_tap = space:install_write_tap(0x01400000, 0x0140ffff, "l8_coin_cmos", function(offset, data, mask)
    local frame = screen:frame_number()
    if frame >= 2600 and frame <= 2610 then
        local kind = (offset >= 0x014010e0 and offset <= 0x014010ef) and "credit" or "other"
        print(string.format("L8_COIN_CMOS_WRITE kind=%s frame=%d address=%08x data=%08x mask=%08x pc=%08x",
            kind,
            frame, offset, data, mask, cpu.state["PC"].value))
    end
end)

emu.register_frame_done(function()
    local frame = screen:frame_number()
    if frame == 2600 then
        coin:set_value(1)
        print("L8_COIN_RAM_ACTION press frame=2600")
    elseif frame == 2604 then
        coin:clear_value()
        print("L8_COIN_RAM_ACTION release frame=2604")
    elseif frame == 2615 then
        print(string.format("L8_COIN_RAM_SUMMARY frames=2600..2610 mainram_writes=%d candidates=%d watched_events=%d",
            write_count, candidate_count, watched_events))
        machine:exit()
    end
end, "l8_coin_ram_trace")

print("L8_COIN_RAM_INIT range=01000000-010fffff observer=write-only")
