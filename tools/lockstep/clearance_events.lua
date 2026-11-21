-- Passive development observer: record the entire active object list at a
-- source-bound REDREG WAVE write. This records transitions, not inferred clears.
local machine=manager.machine
local cpu=machine.devices[":maincpu"]
local space=cpu.spaces["program"]
local screen=machine.screens[":screen"]
local frames=assert(tonumber(os.getenv("STV_LS_FRAMES")))
local out=assert(io.open(assert(os.getenv("STV_CLEARANCE_EVENTS_OUT")),"wb"))
local done,count=false,0
local function word(a) return space:read_u16(a) & 0xffff end
local function long(a) return word(a)|(word(a+16)<<16) end
local function reg(n) return cpu.state[n].value & 0xffffffff end
out:write('{"schema":"stv.mame.exit_objects.v1","phase":"write_tap_before_store","pc_semantics":"raw_cpu_state_not_instruction_start","clearance_proof":false}\n')
stv_clearance_tap=space:install_write_tap(0x010eb630,0x010eb63f,"stv_clearance_objects",function(offset,data,mask)
    if done or reg("PC")~=0xffec95a0 or word(0x010865a0)~=16 then return end
    local objects,seen={},{}
    local ptr=long(0x01000040)
    local reason="complete"
    while ptr~=0 do
        if ptr<0x010003a0 or ptr>=0x01033160 or (ptr-0x010003a0)%0x1f0~=0 then reason="outside_pool";break end
        if seen[ptr] then reason="cycle";break end
        if #objects>=420 then reason="extent";break end
        seen[ptr]=true
        objects[#objects+1]=string.format('{"address":%u,"next":%u,"oid":%u}',ptr,long(ptr),word(ptr+0x150))
        ptr=long(ptr)
    end
    count=count+1
    out:write(string.format('{"kind":"exit_objects","frame":%u,"pc_raw":%u,"circuit":%u,"old_wave":%u,"new_wave":%u,"mask":%u,"a13_raw":%u,"list_boundary":"%s","boundary_pointer":%u,"objects":[%s]}\n',
        screen:frame_number(),reg("PC"),word(0x010eb610),word(0x010eb630),data&0xffff,mask&0xffff,reg("A13"),reason,ptr,table.concat(objects,",")))
    out:flush()
end)
emu.register_frame_done(function()
    local f=screen:frame_number()
    if not done and f>=frames then
        out:write(string.format('{"kind":"end","frame":%u,"events":%u}\n',f,count))
        out:close();done=true
    end
end,"stv_clearance_events")
dofile(assert(os.getenv("STV_ROUTE_EVENTS_OBSERVER")))
