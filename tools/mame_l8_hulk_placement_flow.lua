-- Development-only live L8 placement trace. This observes the retail program;
-- it never changes emulated memory or supplies coordinate/RNG values.
local machine = manager.machine
local screen = machine.screens[":screen"]
local cpu = machine.devices[":maincpu"]
local cpu_debug = cpu.debug
local debugger = machine.debugger

if cpu_debug == nil or debugger == nil then
    print("L8_HULK_FLOW_ERROR debugger_unavailable")
    return
end

-- Stable HULKPID process records from the paired fresh-run ownership report.
local processes = { 0x0108b0b0, 0x0108c190, 0x0108ab10, 0x0108ccd0 }
local condition = table.concat({
    string.format("a13 == 0x%08x", processes[1]),
    string.format("a13 == 0x%08x", processes[2]),
    string.format("a13 == 0x%08x", processes[3]),
    string.format("a13 == 0x%08x", processes[4]),
}, " || ")

local breakpoints = {
    {
        name = "GET_VALID_XY_RETURN",
        pc = 0xffe3b9a0,
        action = 'printf "L8_HULK_FLOW stage=GET_VALID_XY_RETURN process=%08X pc=%08X x=%08X y=%08X a11=%08X\\n",a13,pc,a0,a1,a11 ; g',
    },
    {
        name = "WAVEYX_LOADED",
        pc = 0xffe3ba30,
        action = 'printf "L8_HULK_FLOW stage=WAVEYX_LOADED process=%08X pc=%08X x=%08X y=%08X waveyx=%08X\\n",a13,pc,a0,a1,a2 ; g',
    },
    {
        name = "WAVEYX_APPLIED",
        pc = 0xffe3baa0,
        action = 'printf "L8_HULK_FLOW stage=WAVEYX_APPLIED process=%08X pc=%08X x=%08X y=%08X\\n",a13,pc,a0,a1 ; g',
    },
    {
        name = "STROBJ_INPUT",
        pc = 0xffe03f60,
        action = 'printf "L8_HULK_FLOW stage=STRTOBJ_INPUT process=%08X pc=%08X x=%08X y=%08X\\n",a13,pc,a0,a1 ; g',
    },
    {
        name = "STROBJ_OBJECT_COORDS",
        pc = 0xffe043d0,
        action = 'printf "L8_HULK_FLOW stage=STRTOBJ_OBJECT_COORDS process=%08X pc=%08X x=%08X y=%08X x_anim_offset=%08X y_anim_offset=%08X\\n",a13,pc,a9,a10,a6,a7 ; g',
    },
}

for _, item in ipairs(breakpoints) do
    local index = cpu_debug:bpset(item.pc, condition, item.action)
    print(string.format("L8_HULK_FLOW_BP stage=%s pc=%08x index=%s", item.name, item.pc, tostring(index)))
end

local debug_cursor = #debugger.consolelog
emu.register_frame_done(function()
    local consolelog = debugger.consolelog
    for index = debug_cursor + 1, #consolelog do
        local line = consolelog[index]
        if line and string.find(line, "L8_HULK_FLOW", 1, true) then
            print("L8_DEBUGGER " .. line)
        end
    end
    debug_cursor = #consolelog

    local frame = screen:frame_number()
    local port = machine.ioport.ports[":IN1"]
    if frame == 2600 then
        port:field(0x0001):set_value(1)
        print("L8_ACTION coin1_press frame=2600")
    elseif frame == 2604 then
        port:field(0x0001):clear_value()
        print("L8_ACTION coin1_release frame=2604")
    elseif frame == 3150 then
        port:field(0x0004):set_value(1)
        print("L8_ACTION start1_press frame=3150")
    elseif frame == 3170 then
        port:field(0x0004):clear_value()
        print("L8_ACTION start1_release frame=3170")
    elseif frame == 4200 then
        local err = screen:snapshot("l8-hulk-placement-flow-4200.png")
        print(string.format("L8_HULK_FLOW_SNAPSHOT frame=4200 error=%s", tostring(err)))
    elseif frame == 4300 then
        print("L8_HULK_FLOW_DONE frame=4300")
    end
end, "l8_hulk_placement_flow")

print("L8_HULK_FLOW_PROBE target=GET_VALID_XY,WAVEYX,STRTOBJ L8=retail_inputs_only")
