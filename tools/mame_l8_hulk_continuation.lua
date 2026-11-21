-- Development-only paid first-room trace of the first four retail OHULK
-- continuations. The probe observes L8 execution and never writes emulated RAM.
local machine = manager.machine
local screen = machine.screens[":screen"]
local cpu = machine.devices[":maincpu"]
local space = cpu.spaces["program"]
local cpu_debug = cpu.debug
local debugger = machine.debugger

if cpu_debug == nil or debugger == nil then
    print("L8_HULK_CONT_ERROR debugger_unavailable")
    return
end

local processes = { 0x0108b0b0, 0x0108c190, 0x0108ab10, 0x0108ccd0 }
local first_cflags = {}
local function condition_for(process)
    return string.format("a13 == 0x%08x", process)
end

-- The first instruction after PRCDSP restores each saved OHULK continuation.
l8_hulk_continuation_taps = {}
for _, process in ipairs(processes) do
    first_cflags[process] = false
    l8_hulk_continuation_taps[#l8_hulk_continuation_taps + 1] = space:install_read_tap(
        process + 0x130, process + 0x13f,
        string.format("l8_hulk_cflags_%08x", process),
        function(address, data, mask)
            if not first_cflags[process] and screen:frame_number() >= 3533 and
               cpu.state["A13"].value == process and cpu.state["PC"].value == 0xffe3b900 then
                first_cflags[process] = true
                print(string.format(
                    "L8_HULK_CONT_TAP stage=FIRST_CFLAGS_READ frame=%d emutime=%.9f process=%08x address=%08x data=%08x mask=%08x pc=%08x a7=%08x",
                    screen:frame_number(), machine.time:as_double(), process, address,
                    data, mask, cpu.state["PC"].value, cpu.state["A7"].value))
            end
        end)
end

local stages = {
    { "FIRST_OHULK_DISPATCH", 0xffe3b8e0 },
    { "GET_VALID_XY_ENTRY",   0xffe41920 },
    { "GET_VALID_XY_RETURN",  0xffe3b9a0 },
    { "IH1_ENTRY",             0xffe41630 },
    { "BEGINOBJ2_ENTRY",       0xffe04170 },
    { "BEGINOBJ2_RETURN",      0xffe3baf0 },
    { "SETUP_HULK_ENTRY",      0xffe3e670 },
    { "SETUP_HULK_RETURN",     0xffe3bb50 },
    { "DO_DIFF_ENTRY",         0xffe3ec30 },
    { "DO_DIFF_RETURN",        0xffe3bb70 },
    { "JUST_SK_ENTRY",         0xffe3ebe0 },
    { "JUST_SK_RETURN",        0xffe3bb90 },
    { "GET_STAND_ENTRY",       0xffe405d0 },
    { "GET_STAND_RETURN",      0xffe3bbb0 },
    { "LUP_TOP_FIRST_ENTRY",   0xffe3c760 },
    { "BUYINPID_EXISTS_BRANCH", 0xffe3d500 },
    { "SLEEP2_LOAD",           0xffe3d650 },
    { "SLEEP2_CALL",           0xffe3d670 },
    { "PRCSLP_ENTRY",          0xffe0a490 },
}

local debug_cursor = #debugger.consolelog
local breakpoints_armed = false
local function arm_breakpoints()
    if breakpoints_armed then return end
    breakpoints_armed = true
    print(string.format("L8_HULK_CONT_ARMED frame=%d", screen:frame_number()))
    for _, process in ipairs(processes) do
        for _, item in ipairs(stages) do
            local stage, pc = item[1], item[2]
            local action = string.format(
                'printf "L8_HULK_CONT stage=%s pc=%%08X process=%%08X a0=%%08X a1=%%08X a2=%%08X a7=%%08X a8=%%08X a11=%%08X a13=%%08X a14=%%08X\\n",pc,a13,a0,a1,a2,a7,a8,a11,a13,a14 ; g',
                stage)
            if stage == "PRCSLP_ENTRY" and process == processes[1] then
                action = string.format(
                    'printf "L8_HULK_CONT_HISTORY_BEGIN process=%%08X pc=%%08X\\n",a13,pc ; history ,100 ; printf "L8_HULK_CONT_HISTORY_END process=%%08X pc=%%08X\\n",a13,pc ; printf "L8_HULK_CONT stage=%s pc=%%08X process=%%08X sleep=%%08X a7=%%08X a8=%%08X a13=%%08X a14=%%08X\\n",pc,a13,a0,a7,a8,a13,a14 ; g',
                    stage)
            end
            local index = cpu_debug:bpset(pc, condition_for(process), action)
            print(string.format("L8_HULK_CONT_BP stage=%s process=%08x pc=%08x index=%s",
                stage, process, pc, tostring(index)))
        end
    end
end

local function emit_debugger_lines()
    local consolelog = debugger.consolelog
    local inside_history = false
    local frame = screen:frame_number()
    local emutime = machine.time:as_double()
    for index = debug_cursor + 1, #consolelog do
        local line = consolelog[index]
        if line and string.find(line, "L8_HULK_CONT_HISTORY_BEGIN", 1, true) then
            inside_history = true
        end
        if line and (inside_history or string.find(line, "L8_HULK_CONT", 1, true)) then
            print(string.format("L8_DEBUGGER frame=%d emutime=%.9f %s", frame, emutime, line))
        end
        if line and string.find(line, "L8_HULK_CONT_HISTORY_END", 1, true) then
            inside_history = false
        end
    end
    debug_cursor = #consolelog
end

local port = machine.ioport.ports[":IN1"]
local coin = port:field(0x0001)
local start1 = port:field(0x0004)
local done_reported = false
emu.register_frame_done(function()
    emit_debugger_lines()
    local frame = screen:frame_number()
    if not breakpoints_armed and frame >= 3528 then
        arm_breakpoints()
    end
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
    elseif frame >= 3535 and not done_reported then
        emit_debugger_lines()
        done_reported = true
        print(string.format("L8_HULK_CONT_DONE frame=%d", frame))
    end
end, "l8_hulk_continuation")

print("L8_HULK_CONT_PROBE target=first_OHULK_setup_to_PRCslp L8=retail_inputs_only")
