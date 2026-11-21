-- Development-only MAME 0.289 sound-board recorder (PLAN.md P6).
-- Never loaded by the shipping executable. Observes only; inputs follow the
-- same default schedule as record.lua (or STV_LS_INPUTS).
--
-- Environment:
--   STV_SND_OUT    output path                     (default snd.bin)
--   STV_LS_FRAMES  frames to record, then exit     (default 4000)
--   STV_SND_FROM / STV_SND_TO  frame window for full access records (default none)
--   STV_LS_INPUTS  input schedule, as record.lua
--
-- Stream (little endian): "STVSND01" then records u8 type, payload:
--   1 sound CPU read   / 2 sound CPU write:  u32 frame, u32 sec, u64 attosec, u16 addr, u8 data, u16 pc
--   3 main sound write / 4 main control write: u32 frame, u32 sec, u64 attosec, u16 data
--   9 frame summary: u32 frame, u32 access count, u32 fnv1a over (kind, sec, attosec>>20, addr, data)
-- Times are MAME local times (the executing CPU's), exact to the attosecond.
local machine = manager.machine
local screen = machine.screens[":screen"]
local main = machine.devices[":maincpu"]
local snd = machine.devices[":cvsd:cpu"]
local mspace = main.spaces["program"]
local sspace = snd.spaces["program"]
local out = assert(io.open(os.getenv("STV_SND_OUT") or "snd.bin", "wb"))
local frames = tonumber(os.getenv("STV_LS_FRAMES") or "4000")
local from = tonumber(os.getenv("STV_SND_FROM") or "-1")
local to = tonumber(os.getenv("STV_SND_TO") or "-2")
out:write("STVSND01")

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

local function now_as()
    local t = machine.time
    return t.seconds, t.attoseconds
end

local function s_now() return machine.time.seconds end
local function a_now() return machine.time.attoseconds end
local count, hash = 0, 2166136261
local function mix(v)
    for i = 0, 3 do
        hash = ((hash ~ ((v >> (8 * i)) & 0xff)) * 16777619) & 0xffffffff
    end
end

local function access(kind, offset, data)
    local f = screen:frame_number()
    local s, a = now_as()
    count = count + 1
    mix(kind); mix(s); mix(a >> 20); mix(offset); mix(data & 0xff)
    if f >= from and f <= to then
        out:write(string.pack("<I1I4I4I8I2I1I2", kind, f, s, a, offset, data & 0xff, snd.state["PC"].value & 0xffff))
    end
end

taps = {}
taps[1] = sspace:install_read_tap(0x2000, 0x7fff, "snd_io_r", function(o, d, m) access(1, o, d); return d end)
taps[2] = sspace:install_write_tap(0x2000, 0x7fff, "snd_io_w", function(o, d, m) access(2, o, d) end)
taps[3] = sspace:install_write_tap(0x9cf6, 0x9d21, "snd_prot_w", function(o, d, m) access(2, o, d) end)
taps[4] = mspace:install_write_tap(0x01e00000, 0x01e0001f, "main_snd_w", function(o, d, m)
    out:write(string.pack("<I1I4I4I8I2", 3, screen:frame_number(), s_now(), a_now(), d & 0xffff))
end)
taps[5] = mspace:install_write_tap(0x01f00000, 0x01f0001f, "main_ctl_w", function(o, d, m)
    out:write(string.pack("<I1I4I4I8I2", 4, screen:frame_number(), s_now(), a_now(), d & 0xffff))
end)

emu.register_frame_done(function()
    local f = screen:frame_number()
    -- Memory-tap callbacks allocate Lua observer objects at sound-access rate.
    -- Reclaim them at a frame boundary: long captures otherwise exhaust host
    -- memory. This changes no emulated inputs, times or stream records.
    if f % 64 == 0 then collectgarbage("collect") end
    out:write(string.pack("<I1I4I4I4", 9, f, count, hash))
    local ev = schedule[f]
    if ev then
        for _, e in ipairs(ev) do
            local field = machine.ioport.ports[e.tag]:field(e.mask)
            if e.value ~= 0 then field:set_value(1) else field:clear_value() end
        end
    end
    if f >= frames then
        out:close()
        machine:exit()
    end
end)
