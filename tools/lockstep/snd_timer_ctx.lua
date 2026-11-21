-- Development-only MAME 0.289 callback/instruction context, without a debugger.
-- Trace only: this is not a sound hash recorder or a fidelity gate.
-- STV_LS_INPUTS has exactly the snd_record.lua replay format/default schedule.
-- STV_CTX_OUT          output text path (default snd_timer_ctx.txt)
-- STV_CTX_ARM_FRAME    install taps after this frame (default 63500)
-- STV_CTX_STOP_FRAME   exit after this frame (default 63502)
-- STV_CTX_CYCLE_FROM / STV_CTX_CYCLE_TO   sound-clock window (inclusive)
--                     defaults 2321538850 / 2321539010
-- STV_CTX_MAX_RECORDS  output bound (default 2000); overflow is reported.
local machine = manager.machine
local screen = machine.screens[":screen"]
local main = machine.devices[":maincpu"]
local snd = machine.devices[":cvsd:cpu"]
local mspace = main.spaces["program"]
local sspace = snd.spaces["program"]
local out = assert(io.open(os.getenv("STV_CTX_OUT") or "snd_timer_ctx.txt", "w"))
local arm_frame = tonumber(os.getenv("STV_CTX_ARM_FRAME") or "63500")
local stop_frame = tonumber(os.getenv("STV_CTX_STOP_FRAME") or "63502")
local cycle_from = tonumber(os.getenv("STV_CTX_CYCLE_FROM") or "2321538850")
local cycle_to = tonumber(os.getenv("STV_CTX_CYCLE_TO") or "2321539010")
local max_records = tonumber(os.getenv("STV_CTX_MAX_RECORDS") or "2000")
assert(arm_frame and stop_frame and arm_frame < stop_frame, "invalid context frame limits")
assert(cycle_from and cycle_to and cycle_from <= cycle_to, "invalid context cycle limits")
assert(max_records and max_records > 0, "invalid context output bound")
out:write("# Trace-only MAME memory-tap context; times are the executing CPU's local time.\n")
out:write("# event frame seconds attoseconds sound_cycle sound_cycle_remainder_as address data mask sound_pc sound_cc pia_crb main_pc\n")

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

local record_count, dropped = 0, 0
local armed, closed = false, false
local taps = {} -- retain every memory-tap handle until exit
local function log(event, offset, data, mask)
    if closed then return end
    local t = machine.time
    local cycle = t.seconds * 2000000 + t.attoseconds // 500000000000
    if cycle < cycle_from or cycle > cycle_to then return end
    if record_count >= max_records then dropped = dropped + 1; return end
    record_count = record_count + 1
    -- CRB has no read side effect. No read tap covers 0x4003, so this cannot
    -- recurse through our taps or clear the port-B interrupt flags.
    local crb = sspace:read_u8(0x4003)
    out:write(string.format("%s %d %d %d %d %d %08X %04X %04X %04X %02X %02X %08X\n",
        event, screen:frame_number(), t.seconds, t.attoseconds, cycle,
        t.attoseconds % 500000000000, offset, data, mask,
        snd.state["PC"].value & 0xffff, snd.state["CC"].value & 0xff,
        crb, main.state["PC"].value & 0xffffffff))
end
local function arm()
    armed = true
    for _, range in ipairs({{0x8065, 0x806c}, {0x80a6, 0x80a9}, {0xfffc, 0xfffd}}) do
        table.insert(taps, sspace:install_read_tap(range[1], range[2], "ctx_snd_r", function(o, d, m)
            log("SND_R", o, d, m); return d
        end))
    end
    table.insert(taps, sspace:install_write_tap(0x2000, 0x2001, "ctx_ym_w", function(o, d, m)
        log("YM_W", o, d, m)
    end))
    table.insert(taps, sspace:install_write_tap(0x7800, 0x7800, "ctx_bank_w", function(o, d, m)
        log("BANK_W", o, d, m)
    end))
    table.insert(taps, mspace:install_write_tap(0x01a80000, 0x01a8000f, "ctx_dma_w", function(o, d, m)
        log("DMA_W", o, d, m)
    end))
    table.insert(taps, mspace:install_write_tap(0x01e00000, 0x01e0001f, "ctx_main_snd_w", function(o, d, m)
        log("MAIN_SND_W", o, d, m)
    end))
end

emu.register_frame_done(function()
    if closed then return end
    local f = screen:frame_number()
    local ev = schedule[f]
    if ev then
        for _, e in ipairs(ev) do
            local field = machine.ioport.ports[e.tag]:field(e.mask)
            if e.value ~= 0 then field:set_value(1) else field:clear_value() end
        end
    end
    if f >= arm_frame and not armed then arm() end
    if f >= stop_frame then
        closed = true
        out:write(string.format("# end frame=%d records=%d dropped=%d trace_only=true\n", f, record_count, dropped))
        out:close()
        machine:exit()
    end
end)
