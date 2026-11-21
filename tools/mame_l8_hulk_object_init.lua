-- MAME-only write trace for the four repeatable first-room Hulk object slots.
-- The slot addresses come from the paired HULK_CNT/process-context witness.
-- This observer does not modify guest memory or provide native game data.
local machine = manager.machine
local screen = machine.screens[":screen"]
local cpu = machine.devices[":maincpu"]
local space = cpu.spaces["program"]
local port = machine.ioport.ports[":IN1"]
local coin = port:field(0x0001)
local start1 = port:field(0x0004)
local hulks = {0x0101f1b0, 0x0101f3a0, 0x0101f590, 0x0101f780}
local writes = 0

local function tap_field(actor_index, field, offset, start, finish)
    local name = string.format("l8_hulk_%d_%s", actor_index, field)
    l8_hulk_object_taps[#l8_hulk_object_taps + 1] = space:install_write_tap(start, finish, name, function(address, data, mask)
        local frame = screen:frame_number()
        if frame >= 3000 and frame <= 3600 then
            print(string.format("L8_HULK_OBJECT_WRITE sample=%d frame=%d emutime=%.9f actor=%d field=%s address=%08x data=%08x mask=%08x pc=%08x a8=%08x a13=%08x",
                writes, frame, machine.time:as_double(), actor_index, field, address, data, mask,
                cpu.state["PC"].value, cpu.state["A8"].value, cpu.state["A13"].value))
            writes = writes + 1
        end
    end)
end

l8_hulk_object_taps = {}
for i, object in ipairs(hulks) do
    tap_field(i, "x", object + 0x60, object + 0x60, object + 0x7f)
    tap_field(i, "y", object + 0x80, object + 0x80, object + 0x9f)
    tap_field(i, "oplink", object + 0x160, object + 0x160, object + 0x17f)
end
l8_hulk_object_taps[#l8_hulk_object_taps + 1] = space:install_write_tap(0x010de5d0, 0x010de5df, "l8_hulk_count", function(offset, data, mask)
    local frame = screen:frame_number()
    if frame >= 3000 and frame <= 3600 then
        print(string.format("L8_HULK_COUNT frame=%d emutime=%.9f address=%08x value=%08x mask=%08x pc=%08x a8=%08x a13=%08x",
            frame, machine.time:as_double(), offset, data, mask, cpu.state["PC"].value, cpu.state["A8"].value, cpu.state["A13"].value))
    end
end)

emu.register_frame_done(function()
    local frame = screen:frame_number()
    if frame == 2600 then coin:set_value(1); print("L8_ACTION coin1_press frame=2600")
    elseif frame == 2604 then coin:clear_value(); print("L8_ACTION coin1_release frame=2604")
    elseif frame == 3150 then start1:set_value(1); print("L8_ACTION start1_press frame=3150")
    elseif frame == 3170 then start1:clear_value(); print("L8_ACTION start1_release frame=3170")
    elseif frame == 4200 then
        local err = screen:snapshot("l8-paid-hulk-object-init-4200.png")
        print(string.format("L8_SNAPSHOT frame=4200 error=%s", tostring(err)))
    elseif frame == 4300 then
        print(string.format("L8_SUMMARY object_writes=%d", writes))
    end
end, "l8_hulk_object_init")

print("L8_HULK_OBJECT_INIT slot_addresses=0101f1b0,0101f3a0,0101f590,0101f780 source=prior repeatable HULKPID OPLINK trace")
