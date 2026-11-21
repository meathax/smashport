-- Development-only raw MAME route-write observer. Never loaded by the player.
-- Wrap the unchanged main recorder so every event capture also has a complete
-- comparable main stream. No RAM/register/input writes are introduced here.
local machine = manager.machine
local screen = machine.screens[":screen"]
local cpu = machine.devices[":maincpu"]
local space = cpu.spaces["program"]
local frames = assert(tonumber(os.getenv("STV_LS_FRAMES")))
local base = assert(os.getenv("STV_ROUTE_BASE_RECORDER"))
local output = assert(io.open(assert(os.getenv("STV_ROUTE_EVENTS_OUT")), "wb"))
local count, done = 0, false
local function reg(name) return cpu.state[name].value & 0xffffffff end
output:write('{"schema":"stv.mame.route_writes.v1","phase":"write_tap_before_store","pc_semantics":"raw_cpu_state_not_instruction_start","clearance_proof":false}\n')

-- WAVE is word-aligned. Do not read the unaligned FROMDR field with read_u16.
stv_route_write_tap = space:install_write_tap(0x010eb630, 0x010eb63f, "stv_route_wave", function(offset, data, mask)
    if done then return end
    local old = space:read_u16(0x010eb630)
    local bits = mask & 0xffff
    local merged = ((old & (~bits)) | (data & bits)) & 0xffff
    count = count + 1
    output:write(string.format('{"kind":"wave_write","frame":%u,"address":%u,"data":%u,"mask":%u,"old_wave":%u,"merged_wave":%u,"pc_raw":%u,"a0_raw":%u,"a1_raw":%u,"a8_raw":%u,"a9_raw":%u,"a13_raw":%u,"a14_raw":%u,"circuit":%u,"gamstate":%u,"p1_lives":%u,"p2_lives":%u}\n',
        screen:frame_number(), offset, data & 0xffff, bits, old, merged, reg("PC"),
        reg("A0"), reg("A1"), reg("A8"), reg("A9"), reg("A13"), reg("A14"),
        space:read_u16(0x010eb610), space:read_u16(0x010865a0),
        space:read_u16(0x01086360), space:read_u16(0x01086520)))
    output:flush()
end)

-- Register before the main recorder, which requests exit at the same endpoint.
emu.register_frame_done(function()
    local frame = screen:frame_number()
    if not done and frame >= frames then
        output:write(string.format('{"kind":"end","frame":%u,"events":%u}\n',frame,count))
        output:close()
        done = true
    end
end, "stv_route_events")
dofile(base)
