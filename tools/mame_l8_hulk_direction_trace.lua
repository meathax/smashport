local machine = manager.machine
local screen = machine.screens[":screen"]
local cpu = machine.devices[":maincpu"]
local space = cpu.spaces["program"]
local processes = { 0x0108b0b0, 0x0108c190, 0x0108ab10, 0x0108ccd0 }
local objects = { 0x0101f1b0, 0x0101f3a0, 0x0101f590, 0x0101f780 }
-- This pointer is read from PLYROBJS by the first pass of the same clean L8 run.
local first_player_object = 0x01000970
local tracked, mv_seen, object_for, seek_seen, stop_frame = {}, {}, {}, {}, {}
for _, process in ipairs(processes) do
    tracked[process] = true
    mv_seen[process] = false
    seek_seen[process] = false
    stop_frame[process] = 0
end

local field_names = {
    [0x100] = "HAGE", [0x110] = "DIR", [0x120] = "DIS",
    [0x130] = "CFLAGS", [0x140] = "SK_DIR", [0x150] = "LEVEL",
    [0x200] = "SKPLYR"
}
local armed = false
local complete = false

local function emit(stage, process, address, data, mask, pc, extra)
    print(string.format(
        "L8_HULK_DIRECTION_TAP stage=%s frame=%d process=%08x address=%08x data=%08x mask=%08x pc=%08x a0=%08x a1=%08x a2=%08x a7=%08x a8=%08x a13=%08x%s",
        stage, screen:frame_number(), process or 0, address or 0, data or 0,
        mask or 0, pc or 0, cpu.state["A0"].value, cpu.state["A1"].value,
        cpu.state["A2"].value, cpu.state["A7"].value, cpu.state["A8"].value,
        cpu.state["A13"].value, extra or ""))
end

local function active(process)
    return mv_seen[process] and screen:frame_number() <= stop_frame[process]
end

local function on_read(address, data, mask)
    if not armed then return end
    local process = cpu.state["A13"].value
    local pc = cpu.state["PC"].value
    if tracked[process] then
        if not mv_seen[process] and address == process + 0x100 and pc == 0xffe3d730 then
            mv_seen[process] = true
            stop_frame[process] = screen:frame_number() + 32
            object_for[process] = cpu.state["A8"].value
            emit("FIRST_RESUMED_MV", process, address, data, mask, pc,
                 " actor_object=" .. string.format("%08x", object_for[process]))
        end
        if active(process) then
            local offset = address - process
            local field = field_names[offset]
            if field then
                emit("PDATA_READ_" .. field, process, address, data, mask, pc)
            end
            if address >= 0x010dd780 and address <= 0x010dd7b0 then
                emit("PLYROBJS_READ", process, address, data, mask, pc)
            end
            if address == first_player_object + 0x70 and pc == 0xffe3f880 then
                emit("TARGET_X_READ", process, address, data, mask, pc,
                     " target_object=" .. string.format("%08x", first_player_object))
            elseif address == first_player_object + 0x90 and pc == 0xffe3f8b0 then
                emit("TARGET_Y_READ", process, address, data, mask, pc,
                     " target_object=" .. string.format("%08x", first_player_object))
            end
            local object = object_for[process]
            if object and address == object + 0x70 then
                emit("HULK_X_INTEGER_READ", process, address, data, mask, pc,
                     " actor_object=" .. string.format("%08x", object))
            elseif object and address == object + 0x90 and pc == 0xffe40870 then
                emit("HULK_Y_INTEGER_READ", process, address, data, mask, pc,
                     " actor_object=" .. string.format("%08x", object))
            end
        end
    end
end

local function on_write(address, data, mask)
    if not armed then return end
    local process = cpu.state["A13"].value
    if not tracked[process] or not active(process) then return end
    local pc = cpu.state["PC"].value
    local offset = address - process
    local field = field_names[offset]
    if field then
        emit("PDATA_WRITE_" .. field, process, address, data, mask, pc)
        if field == "SK_DIR" and not seek_seen[process] then
            seek_seen[process] = true
            stop_frame[process] = screen:frame_number() + 12
            emit("FIRST_SEEK_RESULT", process, address, data, mask, pc)
        end
    end
    local object = object_for[process]
    if object and address >= object + 0x20 and address <= object + 0x50 then
        emit("HULK_VELOCITY_WRITE", process, address, data, mask, pc,
             " actor_object=" .. string.format("%08x", object) ..
             " object_offset=" .. string.format("%02x", address - object))
    end
end

l8_hulk_direction_taps = {}
for i, process in ipairs(processes) do
    l8_hulk_direction_taps[#l8_hulk_direction_taps + 1] =
        space:install_read_tap(process + 0x100, process + 0x21f,
            string.format("l8_hulk_direction_reads_%08x", process), on_read)
    l8_hulk_direction_taps[#l8_hulk_direction_taps + 1] =
        space:install_write_tap(process + 0x100, process + 0x21f,
            string.format("l8_hulk_direction_writes_%08x", process), on_write)
    l8_hulk_direction_taps[#l8_hulk_direction_taps + 1] =
        space:install_write_tap(objects[i] + 0x20, objects[i] + 0x5f,
            string.format("l8_hulk_velocity_writes_%08x", objects[i]), on_write)
    l8_hulk_direction_taps[#l8_hulk_direction_taps + 1] =
        space:install_read_tap(objects[i] + 0x60, objects[i] + 0x9f,
            string.format("l8_hulk_position_reads_%08x", objects[i]), on_read)
end
l8_hulk_direction_taps[#l8_hulk_direction_taps + 1] =
    space:install_read_tap(0x010dd780, 0x010dd7bf,
        "l8_hulk_player_object_pointer_reads", on_read)
l8_hulk_direction_taps[#l8_hulk_direction_taps + 1] =
    space:install_read_tap(first_player_object + 0x60, first_player_object + 0x9f,
        "l8_hulk_player_position_reads", on_read)

local port = machine.ioport.ports[":IN1"]
local coin = port:field(0x0001)
local start1 = port:field(0x0004)
emu.register_frame_done(function()
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
    end
    if not armed and frame >= 3880 then
        armed = true
        print(string.format("L8_HULK_DIRECTION_ARMED frame=%d", frame))
    end
    if armed and not complete and frame >= 4040 then
        complete = true
        print(string.format("L8_HULK_DIRECTION_DONE frame=%d", frame))
    end
end, "l8_hulk_direction_trace")

print("L8_HULK_DIRECTION_PROBE target=first_resumed_DOHULK_SETUP_DIR_seek_and_velocity L8=retail_inputs_only")
