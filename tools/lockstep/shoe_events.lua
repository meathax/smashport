-- Passive observer of the source-bound SHOEON timer store. No memory writes.
local cpu=manager.machine.devices[":maincpu"]
local space=cpu.spaces["program"]
local screen=manager.machine.screens[":screen"]
local frames=assert(tonumber(os.getenv("STV_LS_FRAMES")))
local out=assert(io.open(assert(os.getenv("STV_SHOE_EVENTS_OUT")),"wb"))
local count,done=0,false
-- CPU addresses are bits; PNUM is eight bits into a 16-bit memory word.
local function word(a)
    local base=a&0xfffffff0
    return ((space:read_u16(base)|(space:read_u16(base+16)<<16))>>(a&15))&0xffff
end
local function long(a)return word(a)|(word(a+16)<<16) end
local function reg(n)return cpu.state[n].value&0xffffffff end
local function list(head,first,stride,limit)
    local found,seen={},{};local p=long(head)
    while p~=0 do
        if p<first or p>=first+stride*limit or (p-first)%stride~=0 or seen[p] then return found,false end
        seen[p]=true;found[p]=true;p=long(p)
    end
    return found,true
end
out:write('{"schema":"stv.mame.shoe_assignments.v1","phase":"write_tap_before_store","pc_semantics":"raw_cpu_state_not_instruction_start"}\n')
stv_shoe_taps={}
for slot=0,239 do
    local process=0x01086790+slot*0x5a0
    local address=process+0x180
    stv_shoe_taps[slot+1]=space:install_write_tap(address,address+15,"stv_shoe_"..slot,function(offset,data,mask)
        local pc=reg("PC")
        if done or pc<0xffe30230 or pc>0xffe30380 or word(0x010865a0)~=16 then return end
        local active,complete=list(0x01000000,0x01086790,0x5a0,240)
        local objects,objects_complete=list(0x01000040,0x010003a0,0x1f0,420)
        local player,torso=0,0
        for n=1,2 do
            local candidate=long(0x010de2b0+(n-1)*32)
            if active[candidate] and long(candidate+0x2c0)==process then player=n;torso=candidate;break end
        end
        local icon=reg("A8")
        local valid=player~=0 and active[process] and complete
        local pid,number,backlink,torso_object,legs_object=0,0,0,0,0
        if valid then
            pid=word(torso+0x20);number=word(torso+0x1d8)&255
            backlink=long(process+0x160);torso_object=long(torso+0x100);legs_object=long(process+0x100)
        end
        local old=word(offset);local merged=((old&~mask)|(data&mask))&0xffff
        count=count+1
        out:write(string.format('{"kind":"shoe_assignment","frame":%u,"pc_raw":%u,"process":%u,"address":%u,"data":%u,"mask":%u,"old_timer":%u,"merged_timer":%u,"a0_raw":%u,"icon":%u,"icon_oid":%u,"icon_active":%s,"object_list_complete":%s,"process_list_complete":%s,"live_owner_links":%s,"player":%u,"torso":%u,"torso_pid":%u,"legs_pid":%u,"player_number":%u,"legs_backlink":%u,"torso_object":%u,"legs_object":%u,"circuit":%u,"wave":%u}\n',
            screen:frame_number(),pc,process,offset,data&0xffff,mask&0xffff,old,merged,reg("A0"),icon,
            objects[icon] and word(icon+0x150) or 0,tostring(objects[icon]==true),tostring(objects_complete),
            tostring(complete),tostring(valid==true),player,torso,pid,active[process] and word(process+0x20) or 0,
            number,backlink,torso_object,legs_object,word(0x010eb610),word(0x010eb630)))
        out:flush()
    end)
end
emu.register_frame_done(function()
    local f=screen:frame_number()
    if not done and f>=frames then out:write(string.format('{"kind":"end","frame":%u,"events":%u}\n',f,count));out:close();done=true end
end,"stv_shoe_events")
dofile(assert(os.getenv("STV_SHOE_BASE_OBSERVER")))
