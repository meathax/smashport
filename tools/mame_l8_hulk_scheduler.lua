-- Development-only MAME trace of the paid first-room OHULK process lifecycle.
-- The reference CPU remains authoritative; this observer never writes RAM.
local machine = manager.machine
local screen = machine.screens[":screen"]
local cpu = machine.devices[":maincpu"]
local space = cpu.spaces["program"]
local cpu_debug = cpu.debug
local debugger = machine.debugger

if cpu_debug == nil or debugger == nil then
    print("L8_HULK_SCHED_ERROR debugger_unavailable")
    return
end

local hulk_id = 0x00ec
local ohulk_entry = 0xffe3b8e0
local initial_processes = { 0x0108b0b0, 0x0108c190, 0x0108ab10, 0x0108ccd0 }
local tracked_processes = {}
for _, process in ipairs(initial_processes) do tracked_processes[process] = true end
local create_condition = string.format("a1 == 0x%08x && a7 == 0x%08x", hulk_id, ohulk_entry)

-- Memory taps supply instruction-time evidence for mutations that several
-- debugger breakpoints would otherwise collapse into one frame-done callback.
local write_stages = {
    -- MAME reports the architectural PC after the tapped store has issued.
    [0xffe0ac40] = "FREE_HEAD_POP",
    [0xffe0ac50] = "NEW_PROCESS_LINK",
    [0xffe0ac60] = "CREATOR_ACTIVE_LINK",
    [0xffe0ae10] = "PROCESS_IMAGE",
    [0xffe0ae30] = "PTIME_INITIALIZED",
    [0xffe0ae40] = "PROCID_INITIALIZED",
    [0xffe0a570] = "PTIME_DISPATCH_DECREMENT",
}
l8_hulk_scheduler_taps = {}
l8_hulk_scheduler_taps[1] = space:install_write_tap(0x01000000, 0x010fffff, "l8_hulk_scheduler_writes", function(address, data, mask)
    if screen:frame_number() ~= 3533 then return end
    local pc = cpu.state["PC"].value
    local stage = write_stages[pc]
    if stage == "PTIME_DISPATCH_DECREMENT" and not tracked_processes[cpu.state["A13"].value] then
        return
    end
    if stage then
        print(string.format("L8_HULK_TAP stage=%s frame=%d emutime=%.9f address=%08x data=%08x mask=%08x pc=%08x a0=%08x a1=%08x a2=%08x a7=%08x a13=%08x",
            stage, screen:frame_number(), machine.time:as_double(), address, data, mask, pc,
            cpu.state["A0"].value, cpu.state["A1"].value, cpu.state["A2"].value,
            cpu.state["A7"].value, cpu.state["A13"].value))
    end
end)

local dispatched = {}
local wake_words_read = {}
for _, process in ipairs(initial_processes) do
    wake_words_read[process] = 0
    l8_hulk_scheduler_taps[#l8_hulk_scheduler_taps + 1] = space:install_read_tap(
        process + 0xe0, process + 0xef, string.format("l8_hulk_wake_pc_%08x", process),
        function(address, data, mask)
            if screen:frame_number() == 3533 and cpu.state["A13"].value == process and
               cpu.state["PC"].value == 0xffe0a5d0 and wake_words_read[process] < 2 then
                wake_words_read[process] = wake_words_read[process] + 1
                print(string.format("L8_HULK_TAP stage=WAKE_PC_READ frame=%d emutime=%.9f process=%08x address=%08x data=%08x mask=%08x pc=%08x a7=%08x a13=%08x",
                    screen:frame_number(), machine.time:as_double(), process, address, data, mask,
                    cpu.state["PC"].value, cpu.state["A7"].value, cpu.state["A13"].value))
            end
        end)
    l8_hulk_scheduler_taps[#l8_hulk_scheduler_taps + 1] = space:install_read_tap(
        process + 0x130, process + 0x13f, string.format("l8_hulk_dispatch_%08x", process),
        function(address, data, mask)
            if not dispatched[process] and screen:frame_number() >= 3533 and
               cpu.state["A13"].value == process then
                dispatched[process] = true
                print(string.format("L8_HULK_TAP stage=FIRST_OHULK_CFLAGS_READ frame=%d emutime=%.9f process=%08x address=%08x data=%08x mask=%08x pc=%08x a7=%08x a13=%08x",
                    screen:frame_number(), machine.time:as_double(), process, address, data, mask,
                    cpu.state["PC"].value, cpu.state["A7"].value, cpu.state["A13"].value))
            end
        end)
end

local create_stages = {
    { "GETPRC_ENTRY",       0xffe0aba0 },
    { "FREE_HEAD_READ",     0xffe0abf0 },
    { "FREE_NEXT_READ",     0xffe0ac10 },
    { "FREE_HEAD_POPPED",   0xffe0ac40 },
    { "NEW_LINK_SET",       0xffe0ac50 },
    { "ACTIVE_LINK_SET",    0xffe0ac90 },
    { "PROCESS_IMAGE_WRITE",0xffe0adf0 },
    { "WAKE_WORD_READY",    0xffe0ae20 },
    { "WAKE_WORD_WRITTEN",  0xffe0ae30 },
    { "ID_WORD_WRITTEN",    0xffe0ae40 },
    { "GETPRC_RETURN",      0xffe0ae60 },
}

for _, item in ipairs(create_stages) do
    local stage, pc = item[1], item[2]
    local action = string.format(
        'printf "L8_HULK_SCHED stage=%s pc=%%08X id=%%08X creator=%%08X a0=%%08X a1=%%08X a2=%%08X a7=%%08X a13=%%08X\\n",pc,a1,a13,a0,a1,a2,a7,a13 ; g',
        stage)
    local index = cpu_debug:bpset(pc, create_condition, action)
    print(string.format("L8_HULK_SCHED_BP stage=%s pc=%08x index=%s", stage, pc, tostring(index)))
end

for index, process in ipairs(initial_processes) do
    local condition = string.format("a13 == 0x%08x", process)
    local action
    if index == 1 then
        action = 'printf "L8_HULK_SCHED_HISTORY_BEGIN stage=OHULK_FIRST_DISPATCH pc=%08X process=%08X a7=%08X\\n",pc,a13,a7 ; history ,220 ; printf "L8_HULK_SCHED_HISTORY_END process=%08X pc=%08X\\n",a13,pc ; g'
    else
        action = 'printf "L8_HULK_SCHED_DISPATCH stage=OHULK_FIRST_DISPATCH pc=%08X process=%08X a7=%08X\\n",pc,a13,a7 ; g'
    end
    local dispatch_bp = cpu_debug:bpset(ohulk_entry, condition, action)
    print(string.format("L8_HULK_SCHED_BP stage=OHULK_FIRST_DISPATCH process=%08x pc=%08x index=%s",
        process, ohulk_entry, tostring(dispatch_bp)))
end

local debug_cursor = #debugger.consolelog
local function emit_debugger_lines()
    local consolelog = debugger.consolelog
    local inside_history = false
    local frame = screen:frame_number()
    local emutime = machine.time:as_double()
    for index = debug_cursor + 1, #consolelog do
        local line = consolelog[index]
        if line and string.find(line, "L8_HULK_SCHED_HISTORY_BEGIN", 1, true) then
            inside_history = true
        end
        if line and (inside_history or string.find(line, "L8_HULK_SCHED", 1, true)) then
            print(string.format("L8_DEBUGGER frame=%d emutime=%.9f %s", frame, emutime, line))
        end
        if line and string.find(line, "L8_HULK_SCHED_HISTORY_END", 1, true) then
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
    if frame == 2600 then
        coin:set_value(1)
        print("L8_ACTION coin1_press frame=2600")
    elseif frame == 2604 then
        coin:clear_value()
        print("L8_ACTION coin1_release frame=2604")
    elseif frame == 3150 then
        start1:set_value(1)
        print("L8_ACTION start1_press frame=3150")
    elseif frame == 3170 then
        start1:clear_value()
        print("L8_ACTION start1_release frame=3170")
    elseif frame == 4200 then
        print("L8_HULK_SCHED_FRAME frame=4200")
    elseif frame == 4300 then
        emit_debugger_lines()
        print("L8_HULK_SCHED_DONE frame=4300")
    end
end, "l8_hulk_scheduler")

print("L8_HULK_SCHED_PROBE target=CREATE_GETPRC_first_dispatch L8=retail_inputs_only")
