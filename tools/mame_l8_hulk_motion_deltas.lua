local machine = manager.machine
local screen = machine.screens[":screen"]
local cpu = machine.devices[":maincpu"]
local space = cpu.spaces["program"]
local processes = { 0x0108b0b0, 0x0108c190, 0x0108ab10, 0x0108ccd0 }
local tracked = {}
local armed = false
local sleep_saved = {}
local wake_seen = {}
local mv_seen = {}
local restored = {}
local wake_words = {}
local object_for = {}
local object_writes = {}
local context_seen = {}
for _, process in ipairs(processes) do
    tracked[process] = true
    sleep_saved[process] = false
    wake_seen[process] = false
    mv_seen[process] = false
    restored[process] = {}
    wake_words[process] = {}
    object_writes[process] = {}
    context_seen[process] = {}
end

local function emit(stage, process, address, data, mask, pc, extra)
    print(string.format(
        "L8_HULK_WAKE_TAP stage=%s frame=%d emutime=%.9f process=%08x address=%08x data=%08x mask=%08x pc=%08x a7=%08x a8=%08x a12=%08x%s",
        stage, screen:frame_number(), machine.time:as_double(), process, address,
        data, mask, pc, cpu.state["A7"].value, cpu.state["A8"].value,
        cpu.state["A12"].value, extra or ""))
end

l8_hulk_sleep_resume_taps = {}
l8_hulk_sleep_resume_taps[#l8_hulk_sleep_resume_taps + 1] =
    space:install_write_tap(0x01000000, 0x010fffff, "l8_hulk_sleep_resume_writes",
        function(address, data, mask)
            if not armed then return end
            local process = cpu.state["A13"].value
            if not tracked[process] then return end
            local pc = cpu.state["PC"].value
            if not sleep_saved[process] and (pc == 0xffe0a4f0 or pc == 0xffe0a500) and
               address >= process and address < process + 0x5a0 and
               not context_seen[process][address] then
                context_seen[process][address] = true
                local stage = pc == 0xffe0a4f0 and "PRCSLP_CONTEXT_WRITE" or "PRCSLP_SLEEP_VALUE_WRITE"
                object_for[process] = cpu.state["A8"].value
                emit(stage, process, address, data, mask, pc,
                     " a13=" .. string.format("%08x", process) ..
                     " actor_object=" .. string.format("%08x", object_for[process]))
                if pc == 0xffe0a500 and address == process + 0x30 then
                    sleep_saved[process] = true
                end
            elseif sleep_saved[process] and not wake_seen[process] and pc == 0xffe0a570 and
                   address == process + 0x30 then
                emit("PRCDSP_PTIME_DECREMENT", process, address, data, mask, pc)
            end

            local object = object_for[process]
            if wake_seen[process] and mv_seen[process] and object and
               address >= object + 0x20 and address <= object + 0x9f then
                local offset = address - object
                local limit = offset >= 0x60 and offset <= 0x90 and 4 or 1
                local count = object_writes[process][address] or 0
                if count < limit then
                    object_writes[process][address] = count + 1
                    local field = offset < 0x40 and "OXVEL" or
                                  offset < 0x60 and "OYVEL" or
                                  offset < 0x80 and "OXVAL" or "OYVAL"
                    emit("POST_MV_FIELD_WRITE", process, address, data, mask, pc,
                         " actor_object=" .. string.format("%08x", object) ..
                         " object_offset=" .. string.format("%02x", offset) ..
                         " field=" .. field .. " occurrence=" .. tostring(count + 1))
                end
            end
        end)

for _, process in ipairs(processes) do
    l8_hulk_sleep_resume_taps[#l8_hulk_sleep_resume_taps + 1] =
        space:install_read_tap(process + 0x40, process + 0xff,
            string.format("l8_hulk_restore_%08x", process),
            function(address, data, mask)
                if not armed then return end
                local pc = cpu.state["PC"].value
                if sleep_saved[process] and not wake_seen[process] and pc == 0xffe0a5d0 and
                   not restored[process][address] then
                    restored[process][address] = true
                    emit("PRCDSP_REGISTER_RESTORE_READ", process, address, data, mask, pc)
                    if address == process + 0xe0 then wake_words[process].low = data & 0xffff end
                    if address == process + 0xf0 then wake_words[process].high = data & 0xffff end
                    if wake_words[process].low == 0xd6a0 and wake_words[process].high == 0xffe3 then
                        wake_seen[process] = true
                        emit("FIRST_RESUMED_WAKE_PC", process, process + 0xe0,
                             0xffe3d6a0, 0xffffffff, pc, " resumed_pc=ffe3d6a0")
                    end
                end
            end)

    l8_hulk_sleep_resume_taps[#l8_hulk_sleep_resume_taps + 1] =
        space:install_read_tap(process + 0x100, process + 0x10f,
            string.format("l8_hulk_first_mv_%08x", process),
            function(address, data, mask)
                if armed and wake_seen[process] and not mv_seen[process] and
                   cpu.state["PC"].value == 0xffe3d730 then
                    mv_seen[process] = true
                    emit("FIRST_RESUMED_MV_ENTRY", process, address, data, mask,
                         cpu.state["PC"].value,
                         " resumed_pc=ffe3d6a0 actor_object=" ..
                         string.format("%08x", object_for[process] or 0))
                end
            end)
end

local port = machine.ioport.ports[":IN1"]
local coin = port:field(0x0001)
local start1 = port:field(0x0004)
local done_reported = false
emu.register_frame_done(function()
    local frame = screen:frame_number()
    if not armed and frame >= 3528 then
        armed = true
        print(string.format("L8_HULK_WAKE_ARMED frame=%d", frame))
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
    elseif frame >= 3950 and not done_reported then
        done_reported = true
        print(string.format("L8_HULK_MOTION_DONE frame=%d", frame))
    end
end, "l8_hulk_sleep_resume")

print("L8_HULK_MOTION_PROBE target=paid_PRCSLP_to_first_MV_and_position_deltas L8=retail_inputs_only")

