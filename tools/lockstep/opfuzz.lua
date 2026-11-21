-- PLAN P5 reduced fuzz, MAME 0.289 side (development only). Run with
--   mame smashtv -debug -debugger none -video none -sound none -nothrottle
--        -autoboot_script tools/lockstep/opfuzz.lua
-- STV_FUZZ_VECTORS: vector file (tools/lockstep/opfuzz_vectors.py)
-- STV_FUZZ_OUT:     results, one line per vector:
--                   "pc_after st_after sp_after a0..a14 b0..b14 ram_fnv"
-- Main RAM holds a fixed pattern before every vector; each vector sets the CPU
-- state, single-steps one instruction under the debugger and records the result.
local machine = manager.machine
local cpu = machine.devices[":maincpu"]
local space = cpu.spaces["program"]
local dbg = machine.debugger
local RAM, WORDS = 0x01000000, 0x10000
local function pattern(i) return ((i * 0x9E37) ~ ((i >> 3) * 0x85EB)) & 0xffff end
local function fill()
    for i = 0, WORDS - 1 do space:write_u16(RAM + i * 16, pattern(i)) end
end
local function ram_fnv()
    local data = space:read_range(RAM, RAM + WORDS * 16 - 1, 16, 16)
    local h = 2166136261
    for i = 1, #data, 2 do
        h = ((h ~ string.unpack("<I2", data, i)) * 16777619) & 0xffffffff
    end
    return h
end
local vectors = {}
for line in io.lines(os.getenv("STV_FUZZ_VECTORS")) do
    local t = {}
    for w in line:gmatch("%x+") do t[#t + 1] = tonumber(w, 16) end
    if #t == 33 then vectors[#vectors + 1] = t end
end
local out = assert(io.open(os.getenv("STV_FUZZ_OUT") or "opfuzz.out", "w"))
local pattern_hash
local n, idx, phase, dirty = 0, 1, 0, true
local finished = false
local function load(v)
    if dirty then fill() end
    cpu.state["ST"].value = v[2]
    cpu.state["SP"].value = v[3]
    for r = 0, 14 do cpu.state["A" .. r].value = v[4 + r] end
    for r = 0, 14 do cpu.state["B" .. r].value = v[19 + r] end
    cpu.state["PC"].value = v[1]
end
emu.register_periodic(function()
    if finished then return end
    n = n + 1
    if n < 200 then return end
    if phase == 0 then              -- enter the debugger
        cpu.debug:step(1)
        phase = 1
        return
    end
    if dbg.execution_state ~= "stop" then return end
    if phase == 1 then              -- first stop: I/O registers (for XY ops), pattern RAM, first vector
        local io = {}
        for i = 0, 31 do io[#io + 1] = string.format("%04x", space:read_u16(0xC0000000 + i * 16)) end
        out:write("IO ", table.concat(io, " "), "\n")
        fill()
        pattern_hash = ram_fnv()
        dirty = false
        load(vectors[idx])
        cpu.debug:step(1)
        phase = 2
        return
    end
    -- phase 2: the vector's instruction has executed
    local r = {string.format("%08x %08x %08x", cpu.state["PC"].value, cpu.state["ST"].value, cpu.state["SP"].value)}
    for k = 0, 14 do r[#r + 1] = string.format("%08x", cpu.state["A" .. k].value) end
    for k = 0, 14 do r[#r + 1] = string.format("%08x", cpu.state["B" .. k].value) end
    local h = ram_fnv()
    r[#r + 1] = string.format("%08x", h)
    out:write(table.concat(r, " "), "\n")
    dirty = h ~= pattern_hash
    idx = idx + 1
    if idx > #vectors then
        out:close()
        finished = true
        print(string.format("STV_FUZZ_DONE vectors=%d", #vectors))
        machine:exit()
        return
    end
    load(vectors[idx])
    cpu.debug:step(1)
end)
