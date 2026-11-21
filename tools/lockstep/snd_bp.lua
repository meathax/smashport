-- Development only (PLAN P5/P6 divergence work): MAME 0.289 sound-CPU breakpoint log.
-- Replays STV_LS_INPUTS; during frames [STV_BP_FROM, STV_BP_TO] every hit of the
-- sound-CPU breakpoints STV_BP_PCS (hex, comma separated) is logged to STV_BP_OUT as
-- "frame pc seconds attoseconds cc". Run under -debug -debugger none.
local machine = manager.machine
local screen = machine.screens[":screen"]
local dbg = machine.debugger
local snd = machine.devices[":cvsd:cpu"]
local from = tonumber(os.getenv("STV_BP_FROM") or "0")
local to = tonumber(os.getenv("STV_BP_TO") or "0")
local pcs = os.getenv("STV_BP_PCS") or ""
local mpcs = os.getenv("STV_BP_MAIN_PCS") or ""   -- main CPU breakpoints (hex bit addresses)
local maincpu = machine.devices[":maincpu"]
local out = assert(io.open(os.getenv("STV_BP_OUT") or "snd_bp.txt", "w"))

local schedule = {}
local inputs = os.getenv("STV_LS_INPUTS")
if inputs then
    for line in io.lines(inputs) do
        local f, tag, mask, value = line:match("^%s*(%d+)%s+(%S+)%s+(%x+)%s+(%d+)")
        if f then
            f = tonumber(f)
            schedule[f] = schedule[f] or {}
            table.insert(schedule[f], {tag = tag, mask = tonumber(mask, 16), value = tonumber(value)})
        end
    end
end

local armed = false
emu.register_periodic(function()
    if dbg.execution_state == "stop" then
        local t = machine.time
        local crb = snd.spaces["program"]:read_u8(0x4003)   -- PIA CRB: bit 7 = IRQB1 flag (no read side effect)
        out:write(string.format("%d %04X %d %d %02X crb=%02X mainpc=%08X\n", screen:frame_number(), snd.state["PC"].value,
            t.seconds, t.attoseconds, snd.state["CC"].value, crb, machine.devices[":maincpu"].state["PC"].value))
        dbg.execution_state = "run"
    end
end)

emu.register_frame_done(function()
    if dbg.execution_state ~= "run" then return end
    local f = screen:frame_number()
    local ev = schedule[f]
    if ev then
        for _, e in ipairs(ev) do
            local field = machine.ioport.ports[e.tag]:field(e.mask)
            if e.value ~= 0 then field:set_value(1) else field:clear_value() end
        end
    end
    if f == from and not armed then
        armed = true
        for pc in pcs:gmatch("[^,]+") do snd.debug:bpset(tonumber(pc, 16), "1", "") end
        for pc in mpcs:gmatch("[^,]+") do maincpu.debug:bpset(tonumber(pc, 16), "1", "") end
    end
    if f > to then
        out:close()
        machine:exit()
    end
end)
