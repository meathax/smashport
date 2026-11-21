-- Passive retail pickup-handler observations. Never changes memory/registers.
local cpu=manager.machine.devices[":maincpu"]
local space=cpu.spaces["program"]
local screen=manager.machine.screens[":screen"]
local frames=assert(tonumber(os.getenv("STV_LS_FRAMES")))
local out=assert(io.open(assert(os.getenv("STV_PICKUP_EVENTS_OUT")),"wb"))
local count,done,busy=0,false,false
local function word(a)
    local b=a&0xfffffff0
    return ((space:read_u16(b)|(space:read_u16(b+16)<<16))>>(a&15))&0xffff
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
local function emit(subtype,player_object,phase)
    local active,complete=list(0x01000000,0x01086790,0x5a0,240)
    local objects,objects_complete=list(0x01000040,0x010003a0,0x1f0,420)
    local torso=objects[player_object] and long(player_object+0x160) or 0
    local player=0
    for n=1,2 do if torso~=0 and long(0x010de2b0+(n-1)*32)==torso then player=n end end
    local legs=active[torso] and long(torso+0x2c0) or 0
    local icon=reg("A8")
    count=count+1
    out:write(string.format('{"kind":"pickup_handler","phase":"%s","frame":%u,"pc_raw":%u,"subtype":%u,"icon":%u,"icon_oid":%u,"icon_active":%s,"player_object":%u,"player_oid":%u,"player_object_active":%s,"player":%u,"torso":%u,"legs":%u,"torso_active":%s,"legs_active":%s,"torso_pid":%u,"legs_pid":%u,"player_number":%u,"legs_backlink":%u,"torso_object":%u,"legs_object":%u,"process_list_complete":%s,"object_list_complete":%s,"circuit":%u,"wave":%u}\n',
        phase,screen:frame_number(),reg("PC"),subtype,icon,objects[icon] and word(icon+0x150) or 0,tostring(objects[icon]==true),
        player_object,objects[player_object] and word(player_object+0x150) or 0,tostring(objects[player_object]==true),player,torso,legs,
        tostring(active[torso]==true),tostring(active[legs]==true),active[torso] and word(torso+0x20) or 0,
        active[legs] and word(legs+0x20) or 0,active[torso] and word(torso+0x1d8)&255 or 0,
        active[legs] and long(legs+0x160) or 0,active[torso] and long(torso+0x100) or 0,active[legs] and long(legs+0x100) or 0,
        tostring(complete),tostring(objects_complete),word(0x010eb610),word(0x010eb630)))
    out:flush()
end
-- Raw PC points past each observed memory instruction, not at its start.
local direct={ [0xffed2620]=0,[0xffe31ac0]=2,[0xffe31d00]=3,[0xffe278c0]=4,
    [0xffe31740]=5,[0xffe2fd80]=6,[0xffe31500]=7,[0xffe2ffa0]=8,[0xffe30270]=9,[0xffe2ce30]=15 }
out:write('{"schema":"stv.mame.pickup_handlers.v1","pc_semantics":"raw_cpu_state_not_instruction_start"}\n')
stv_pickup_taps={}
for slot=0,419 do
    local object=0x010003a0+slot*0x1f0
    stv_pickup_taps[#stv_pickup_taps+1]=space:install_read_tap(object+0x160,object+0x16f,"stv_pickup_owner_"..slot,function(offset,data,mask)
        if busy or done or stv_lockstep_host_read then return end
        local pc=reg("PC");local subtype=direct[pc]
        if pc==0xffe2d970 then subtype=11+reg("A10") end
        if pc==0xffe2d110 then subtype=14 end
        if subtype==nil or reg("A0")~=object then return end
        busy=true
        -- SEND is shared with TITM, whose earlier handler read is already logged.
        if word(0x010865a0)==16 and (pc~=0xffe2d110 or long(reg("SP"))==0xffe2d040) then
            emit(subtype,object,"owner_read")
        end
        busy=false
    end)
end
stv_pickup_taps[#stv_pickup_taps+1]=space:install_write_tap(0x010eeab0,0x010eeabf,"stv_pickup_bomb",function(offset,data,mask)
    if busy or done or reg("PC")~=0xffe304c0 then return end
    busy=true;if word(0x010865a0)==16 then emit(10,reg("A9"),"bomb_flag_store") end;busy=false
end)
-- POW2 calls DELOBJ before reading its player owner. Observe the first list
-- traversal while the icon is still active; five saved registers precede RETS.
stv_pickup_taps[#stv_pickup_taps+1]=space:install_read_tap(0x01000040,0x0100004f,"stv_pickup_helper",function(offset,data,mask)
    if busy or done or stv_lockstep_host_read or reg("PC")~=0xffe03240 then return end
    busy=true
    if word(0x010865a0)==16 and long(reg("SP")+5*32)==0xffed3bf0 then emit(1,reg("A9"),"helper_delete_entry") end
    busy=false
end)
emu.register_frame_done(function()
    local f=screen:frame_number()
    if not done and f>=frames then out:write(string.format('{"kind":"end","frame":%u,"events":%u}\n',f,count));out:close();done=true end
end,"stv_pickup_events")
dofile(assert(os.getenv("STV_PICKUP_BASE_OBSERVER")))
