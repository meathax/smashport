-- Development only (PLAN P5/P6 divergence work): MAME 0.289 instruction trace of the
-- sound CPU over a frame window, with the same input replay as snd_record.lua.
-- Run under the debugger (tools/mame_run.ps1 -Extra '-debug','-debugger','none'):
--   STV_LS_INPUTS  input schedule            STV_TRACE_FROM / STV_TRACE_TO  frame window
--   STV_TRACE_OUT  trace file (MAME "trace" format: instructions and "(interrupted at ...)" lines;
--                  the tracelog action is ignored when issued from Lua, so no cycle counts:
--                  use snd_bp.lua for times)
local machine = manager.machine
local screen = machine.screens[":screen"]
local dbg = machine.debugger
local from = tonumber(os.getenv("STV_TRACE_FROM") or "0")
local to = tonumber(os.getenv("STV_TRACE_TO") or "0")
local outp = os.getenv("STV_TRACE_OUT") or "snd_trace.txt"

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

local started = false
emu.register_frame_done(function()
    if dbg.execution_state ~= "run" then dbg.execution_state = "run" end
    local f = screen:frame_number()
    local ev = schedule[f]
    if ev then
        for _, e in ipairs(ev) do
            local field = machine.ioport.ports[e.tag]:field(e.mask)
            if e.value ~= 0 then field:set_value(1) else field:clear_value() end
        end
    end
    if f == from and not started then
        started = true
        local act = os.getenv("STV_TRACE_ACTION") or 'tracelog "%d ",totalcycles'
        dbg:command('trace ' .. outp .. ',:cvsd:cpu,,{' .. act .. '}')
    end
    if f >= to then
        dbg:command('trace off,:cvsd:cpu')
        machine:exit()
    end
end)
