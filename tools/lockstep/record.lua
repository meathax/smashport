-- Development-only MAME 0.289 Y-unit lockstep recorder (PLAN.md P0/P3).
-- Never loaded by the shipping executable. Observes only: no RAM/register
-- writes. Inputs are driven solely from the declared schedule file.
--
-- Environment:
--   STV_LS_OUT     output stream path            (default lockstep.bin)
--   STV_LS_FRAMES  frames to record, then exit    (default 20000)
--   STV_LS_INPUTS  schedule file: "frame port mask value" per line
--                  (default: coin1 2600..2604, start1 3150..3170)
--   STV_LS_BOT     scenario bot (tools/scenarios/bot.lua) instead of a schedule;
--                  its input changes are written to STV_LS_INPUTS_OUT
--   STV_LS_DUMP_EVERY N: RAM/palette/CMOS chunk records only on frames divisible by N
--   STV_LS_SNAPS   frames for screen snapshots (mame_F.png in the snapshot directory)
--   STV_LS_SUMMARY kinds 4..7 folded into one hash record (type 9) per frame
--   STV_LS_COMPACT chunk records carry a 32-bit hash (u16 index, u32 hash) instead
--                  of the 256 bytes, for long scenarios
--
-- Stream (little endian): "STVLS001" then records
--   u8 type, u32 frame, u16 len, payload[len]
-- types: 1 RAM chunk (u16 index, 256 bytes of mainram 0x01000000..)
--        2 palette chunk (u16 index, 256 bytes)   3 CMOS chunk (u16 index, 256 bytes)
--        4 DMA register write (u32 bitaddr, u16 data, u16 mask, u32 pc)
--        5 sound write (u32 bitaddr, u16 data, u16 mask, u32 pc)
--        6 HCOUNT read (u16 value, u32 pc)
--        7 input read (u32 bitaddr, u16 value, u32 pc)
--        8 frame end (u32 emutime attoseconds>>32 is not portable; u32 frame only)
-- Chunks are emitted only when they differ from the previous frame (all on
-- the first recorded frame). A frame's taps precede its chunks and marker.
local machine = manager.machine
local screen = machine.screens[":screen"]
local cpu = machine.devices[":maincpu"]
local space = cpu.spaces["program"]
local out_path = os.getenv("STV_LS_OUT") or "lockstep.bin"
local frames = tonumber(os.getenv("STV_LS_FRAMES") or "20000")
local out = assert(io.open(out_path, "wb"))
out:write("STVLS001")

-- STV_LS_SUMMARY: DMA/sound/HCOUNT/input records (kinds 4..7) are folded into one
-- type 9 record per frame (u32 FNV-1a over kind + the PC-free bytes lsfile.py
-- compares, u32 record count), for very long scenarios.
local summary = os.getenv("STV_LS_SUMMARY") ~= nil
local KEEP = {[4] = 8, [5] = 8, [6] = 2, [7] = 6}
local sum_h, sum_n = 2166136261, 0
local function rec(kind, frame, payload)
    if summary and KEEP[kind] then
        local h = sum_h ~ kind
        h = (h * 16777619) & 0xffffffff
        for i = 1, KEEP[kind] do
            h = ((h ~ payload:byte(i)) * 16777619) & 0xffffffff
        end
        sum_h, sum_n = h, sum_n + 1
        return
    end
    if summary and kind == 8 then
        out:write(string.pack("<I1I4I2I4I4", 9, frame, 8, sum_h, sum_n))
        sum_h, sum_n = 2166136261, 0
    end
    out:write(string.pack("<I1I4I2", kind, frame, #payload), payload)
end

local schedule = {}
local function add_event(frame, tag, mask, value)
    schedule[frame] = schedule[frame] or {}
    table.insert(schedule[frame], {tag = tag, mask = mask, value = value})
end
local inputs = os.getenv("STV_LS_INPUTS")
if inputs then
    for line in io.lines(inputs) do
        local f, tag, mask, value = line:match("^%s*(%d+)%s+(%S+)%s+(%x+)%s+(%d+)")
        if f then add_event(tonumber(f), tag, tonumber(mask, 16), tonumber(value)) end
    end
else
    add_event(2600, ":IN1", 0x0001, 1); add_event(2604, ":IN1", 0x0001, 0)
    add_event(3150, ":IN1", 0x0004, 1); add_event(3170, ":IN1", 0x0004, 0)
end

-- STV_LS_BOT: Lua file returning function(frame, space, machine) -> list of
-- {tag=, mask=, value=} input changes for this frame (applied like the schedule).
-- Its decisions are written to STV_LS_INPUTS_OUT for native replay (--inputs).
local bot, bot_out
if os.getenv("STV_LS_BOT") then
    bot = dofile(os.getenv("STV_LS_BOT"))
    bot_out = assert(io.open(os.getenv("STV_LS_INPUTS_OUT") or "bot_inputs.txt", "w"))
end

local function pc() return cpu.state["PC"].value end
local function frame() return screen:frame_number() end

taps = {}
-- The game addresses the DMA block through its 0x01A80000 mirror.
local function dma_tap(offset, data, mask)
    rec(4, frame(), string.pack("<I4I2I2I4", offset, data & 0xffff, mask & 0xffff, pc()))
end
taps[1] = space:install_write_tap(0x01a00000, 0x01a0009f, "ls_dma", dma_tap)
taps[6] = space:install_write_tap(0x01a80000, 0x01a8009f, "ls_dma_mirror", dma_tap)
taps[2] = space:install_write_tap(0x01e00000, 0x01e0001f, "ls_sound", function(offset, data, mask)
    rec(5, frame(), string.pack("<I4I2I2I4", offset, data & 0xffff, mask & 0xffff, pc()))
end)
taps[3] = space:install_write_tap(0x01f00000, 0x01f0001f, "ls_control", function(offset, data, mask)
    rec(5, frame(), string.pack("<I4I2I2I4", offset, data & 0xffff, mask & 0xffff, pc()))
end)
taps[4] = space:install_read_tap(0xc00001c0, 0xc00001cf, "ls_hcount", function(offset, data, mask)
    rec(6, frame(), string.pack("<I2I4", data & 0xffff, pc()))
    return data
end)
taps[5] = space:install_read_tap(0x01c00000, 0x01c0007f, "ls_input", function(offset, data, mask)
    rec(7, frame(), string.pack("<I4I2I4", offset, data & 0xffff, pc()))
    return data
end)

-- Region dumps: bit addresses, 16-bit words. 256-byte chunks = 0x800 bits.
local regions = {
    {kind = 1, base = 0x01000000, bytes = 0x20000, prev = {}},
    {kind = 2, base = 0x01800000, bytes = 0x4000, prev = {}},
    {kind = 3, base = 0x01400000, bytes = 0x2000, prev = {}},
}
local compact = os.getenv("STV_LS_COMPACT") ~= nil
local dump_every = tonumber(os.getenv("STV_LS_DUMP_EVERY") or "1")   -- region records every Nth frame
local function dump(f)
    for _, r in ipairs(regions) do
        local data = space:read_range(r.base, r.base + r.bytes * 8 - 1, 16, 16)
        assert(#data == r.bytes, "read_range size")
        for i = 0, r.bytes // 256 - 1 do
            local c = data:sub(i * 256 + 1, i * 256 + 256)
            if r.prev[i] ~= c then
                r.prev[i] = c
                if compact then
                    -- STV_LS_COMPACT: 32-bit hash of the chunk instead of its contents
                    local h = 2166136261
                    for k = 1, 256, 4 do
                        h = ((h * 16777619) ~ string.unpack("<I4", c, k)) & 0xffffffff
                    end
                    rec(r.kind, f, string.pack("<I2I4", i, h))
                else
                    rec(r.kind, f, string.pack("<I2", i) .. c)
                end
            end
        end
    end
end

-- STV_LS_SNAPS "f1,f2,...": screen snapshots (snapshot directory, mame_F.png) at those
-- frames, compared with stv_l8_lockstep --snap output by tools/lockstep/snapcmp.py
local snap_frames = {}
for s in (os.getenv("STV_LS_SNAPS") or ""):gmatch("%d+") do snap_frames[tonumber(s)] = true end

local done = false
emu.register_frame_done(function()
    if done then return end
    -- Lua RAM inspection invokes MAME read taps too. Event observers must
    -- distinguish these host reads from execution at the suspended CPU PC.
    stv_lockstep_host_read = true
    local f = frame()
    local ev = schedule[f]
    if bot then
        -- scenario bot: decides from RAM; its decisions are logged as a replayable schedule
        ev = bot(f, space, machine) or {}
        for _, e in ipairs(ev) do
            bot_out:write(string.format("%d %s %x %d\n", f, e.tag, e.mask, e.value))
        end
        bot_out:flush()
    end
    if ev then
        for _, e in ipairs(ev) do
            local fld = machine.ioport.ports[e.tag]:field(e.mask)
            if e.value ~= 0 then fld:set_value(1) else fld:clear_value() end
        end
    end
    if f % dump_every == 0 then dump(f) end
    if snap_frames[f] then screen:snapshot(string.format("mame_%d.png", f)) end
    stv_lockstep_host_read = false
    rec(8, f, "")
    if f >= frames then
        done = true
        out:close()
        print(string.format("STV_LS_DONE frames=%d out=%s", f, out_path))
        machine:exit()
    end
end, "stv_lockstep")
print(string.format("STV_LS_INIT out=%s frames=%d", out_path, frames))
