-- Development-only MAME breakpoint probe for live OHULK placement arguments.
-- The original CPU is the authority here; this script only observes.
local machine = manager.machine
local screen = machine.screens[":screen"]
local cpu = machine.devices[":maincpu"]
local cpu_debug = cpu.debug
local debugger = machine.debugger

if cpu_debug == nil or debugger == nil then
    print("L8_HULK_PLACEMENT_ERROR debugger_unavailable")
    return
end

-- These are the four repeatable HULKPID process records established by the
-- prior fresh-run ownership trace. Stop before STRTOBJ consumes A0/A1.
local processes = { 0x0108b0b0, 0x0108c190, 0x0108ab10, 0x0108ccd0 }
local condition = table.concat({
    string.format("a13 == 0x%08x", processes[1]),
    string.format("a13 == 0x%08x", processes[2]),
    string.format("a13 == 0x%08x", processes[3]),
    string.format("a13 == 0x%08x", processes[4]),
}, " || ")
local action = 'printf "L8_HULK_PLACEMENT process=%08X pc=%08X a0=%08X a1=%08X a11=%08X a13=%08X\\n",a13,pc,a0,a1,a11,a13 ; g'
local breakpoint = cpu_debug:bpset(0xffe03f80, condition, action)
print(string.format("L8_HULK_PLACEMENT_BP index=%s condition=%s", tostring(breakpoint), condition))
local stack_action = 'printf "L8_HULK_HISTORY_BEGIN process=%08X pc=%08X a0=%08X a1=%08X\\n",a13,pc,a0,a1 ; history ,120 ; printf "L8_HULK_HISTORY_END process=%08X pc=%08X\\n",a13,pc ; g'
local stack_breakpoint = cpu_debug:bpset(0xffe03f60, condition, stack_action)
print(string.format("L8_HULK_STACK_BP index=%s condition=%s", tostring(stack_breakpoint), condition))

local debug_cursor = #debugger.consolelog
local function emit_debugger_lines()
    local consolelog = debugger.consolelog
    local inside_history = false
    for index = debug_cursor + 1, #consolelog do
        local line = consolelog[index]
        if line and string.find(line, "L8_HULK_HISTORY_BEGIN", 1, true) then
            inside_history = true
        end
        if line and (inside_history or string.find(line, "L8_HULK_", 1, true)) then
            print("L8_DEBUGGER " .. line)
        end
        if line and string.find(line, "L8_HULK_HISTORY_END", 1, true) then
            inside_history = false
        end
    end
    debug_cursor = #consolelog
end
local port = machine.ioport.ports[":IN1"]
local coin = port:field(0x0001)
local start1 = port:field(0x0004)
emu.register_frame_done(function()
    emit_debugger_lines()
    local frame = screen:frame_number()
    if frame == 2600 then coin:set_value(1); print("L8_ACTION coin1_press frame=2600")
    elseif frame == 2604 then coin:clear_value(); print("L8_ACTION coin1_release frame=2604")
    elseif frame == 3150 then start1:set_value(1); print("L8_ACTION start1_press frame=3150")
    elseif frame == 3170 then start1:clear_value(); print("L8_ACTION start1_release frame=3170")
    elseif frame == 4300 then
        print("L8_HULK_PLACEMENT_DONE frame=4300")
    end
end, "l8_hulk_placement_probe")

print("L8_HULK_PLACEMENT_PROBE target=STRTOBJ-entry pc=ffe03f80 process_count=4")
