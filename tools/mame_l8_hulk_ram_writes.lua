-- Development-only MAME 0.289 probe for first paid-room main-RAM writes.
-- This is a reference trace helper and is never loaded by SmashTV.exe.
local machine = manager.machine
local screen = machine.screens[":screen"]
local cpu = machine.devices[":maincpu"]
local space = cpu.spaces["program"]
local port = machine.ioport.ports[":IN1"]
local coin = port:field(0x0001)
local start1 = port:field(0x0004)
local writes = {}
local write_count = 0
local unique_count = 0
local overflow = 0
local hcount_reads = 0

l8_paid_hulk_ram_taps = {}
l8_paid_hulk_ram_taps[1] = space:install_write_tap(0x01000000, 0x010fffff, "l8_main_ram_window", function(offset, data, mask)
    local frame = screen:frame_number()
    if frame == 3533 or frame == 3534 then
        write_count = write_count + 1
        local row = writes[offset]
        if not row then
            if unique_count < 8192 then
                row = {count = 0, first = data, last = data, first_frame = frame, last_frame = frame,
                    first_pc = cpu.state["PC"].value, last_pc = cpu.state["PC"].value, mask = mask, small_values = {}}
                writes[offset] = row
                unique_count = unique_count + 1
            else
                overflow = overflow + 1
                return
            end
        end
        row.count = row.count + 1
        row.last = data
        row.last_frame = frame
        row.last_pc = cpu.state["PC"].value
        if data <= 16 and #row.small_values < 64 then
            table.insert(row.small_values, string.format("%08x@%08x", data, cpu.state["PC"].value))
        end
    end
end)
l8_paid_hulk_ram_taps[2] = space:install_read_tap(0xc00001c0, 0xc00001cf, "l8_hcount_window", function(offset)
    if screen:frame_number() == 3533 then hcount_reads = hcount_reads + 1 end
end)

emu.register_frame_done(function()
    local frame = screen:frame_number()
    if frame == 2600 then coin:set_value(1); print("L8_ACTION coin1_press frame=2600")
    elseif frame == 2604 then coin:clear_value(); print("L8_ACTION coin1_release frame=2604")
    elseif frame == 3150 then start1:set_value(1); print("L8_ACTION start1_press frame=3150")
    elseif frame == 3170 then start1:clear_value(); print("L8_ACTION start1_release frame=3170")
    end
    if frame == 3533 or frame == 3534 or frame == 4200 then
        local name = string.format("l8-paid-hulk-ram-%d.png", frame)
        local err = screen:snapshot(name)
        print(string.format("L8_SNAPSHOT frame=%d name=%s error=%s", frame, name, tostring(err)))
    end
    if frame == 3550 then
        local addresses = {}
        for address in pairs(writes) do table.insert(addresses, address) end
        table.sort(addresses)
        local candidates = {}
        for _, address in ipairs(addresses) do
            local row = writes[address]
            local previous, streak, longest = nil, 0, 0
            for _, sample in ipairs(row.small_values) do
                local value = tonumber(string.sub(sample, 1, 8), 16)
                if previous ~= nil and value == previous + 1 then
                    streak = streak + 1
                elseif previous ~= nil and value == previous then
                    -- Repeated source stores do not advance the candidate counter.
                else
                    streak = 1
                end
                if streak > longest then longest = streak end
                previous = value
            end
            if longest >= 4 then table.insert(candidates, {address = address, row = row, longest = longest}) end
        end
        print(string.format("L8_RAM_SUMMARY frame_window=3533..3534 writes=%d unique=%d monotone_candidates=%d hcount_frame3533=%d overflow=%d",
            write_count, #addresses, #candidates, hcount_reads, overflow))
        for _, candidate in ipairs(candidates) do
            local row = candidate.row
            print(string.format("L8_RAM_COUNTER address=%08x count=%d first=%08x last=%08x mask=%08x longest_increment_run=%d first_frame=%d last_frame=%d first_pc=%08x last_pc=%08x values=%s",
                candidate.address, row.count, row.first, row.last, row.mask, candidate.longest, row.first_frame,
                row.last_frame, row.first_pc, row.last_pc, table.concat(row.small_values, ",")))
        end
    end
end, "l8_paid_hulk_ram_writes")

print("L8_HULK_RAM_INIT main_ram=01000000..010fffff capture_frames=3533..3534")
