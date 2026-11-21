-- Scenario-scoped passive observer; addresses pinned by the prior life trace.
local cpu=manager.machine.devices[":maincpu"]
local space=cpu.spaces["program"]
local screen=manager.machine.screens[":screen"]
local out=assert(io.open(assert(os.getenv("STV_DAMAGE_EVENTS_OUT")),"wb"))
local frames=assert(tonumber(os.getenv("STV_LS_FRAMES")))
local count,done=0,false
local function reg(n)return cpu.state[n].value & 0xffffffff end
local function word(a)
    local b=a & 0xfffffff0
    return ((space:read_u16(b)|(space:read_u16(b+16)<<16))>>(a & 15))&0xffff
end
local function long(a)return word(a)|(word(a+16)<<16) end
local function signed(v)return v>=32768 and v-65536 or v end
out:write('{"schema":"stv.mame.damage_writes.v1","phase":"write_tap_before_store","scope":"two_prior_trace_process_slots_and_wave_ram","collision_geometry_proof":false}\n')
stv_damage_taps={}
for player,process in ipairs({0x01087e10,0x01089490}) do
    local field=process+0x208
    local base=field & 0xfffffff0
    stv_damage_taps[player]=space:install_write_tap(base,base+31,"stv_dead_"..player,function(offset,data,mask)
        if done or word(0x010865a0)~=16 then return end
        local lo,hi=space:read_u16(base),space:read_u16(base+16)
        local old=((lo|(hi<<16))>>8)&0xffff
        if offset==base then lo=((lo & ~mask)|(data & mask))&0xffff
        elseif offset==base+16 then hi=((hi & ~mask)|(data & mask))&0xffff
        else return end
        local merged=((lo|(hi<<16))>>8)&0xffff
        local killer=reg("A10");local object=reg("A8")
        local valid=killer>=0x01000000 and killer+0x170<0x01100000 and object>=0x01000000 and object+0x180<0x01100000
        local oid,x,y,oplink=-1,0,0,0
        if valid then oid=word(killer+0x150);x=signed(word(killer+0x70));y=signed(word(killer+0x90));oplink=long(object+0x160) end
        count=count+1
        out:write(string.format('{"kind":"dead_write","frame":%u,"player_slot":%u,"process":%u,"address":%u,"mask":%u,"data":%u,"old_dead":%u,"merged_dead":%u,"pc_raw":%u,"a0_raw":%u,"a7_raw":%u,"a8_raw":%u,"a10_raw":%u,"a11_raw":%u,"process_player_raw":%u,"object_oplink_raw":%u,"killer_oid_raw":%d,"killer_x_raw":%d,"killer_y_raw":%d}\n',
            screen:frame_number(),player,process,offset,mask&0xffff,data&0xffff,old,merged,reg("PC"),reg("A0"),reg("A7"),object,killer,reg("A11"),word(process+0x1d8)&0xff,oplink,oid,x,y))
        out:flush()
    end)
end
stv_damage_taps[3]=space:install_write_tap(0x010ddef0,0x010de2af,"stv_wave_difficulty",function(offset,data,mask)
    if done or reg("PC")~=0xffe19690 then return end
    local old=space:read_u16(offset)
    local merged=((old & ~mask)|(data & mask))&0xffff
    count=count+1
    out:write(string.format('{"kind":"difficulty_write","frame":%u,"address":%u,"old":%u,"merged":%u,"mask":%u,"pc_raw":%u,"a0_raw":%u,"a2_raw":%u,"a10_raw":%u,"entry_type_raw":%u,"circuit":%u,"wave":%u}\n',
        screen:frame_number(),offset,old,merged,mask&0xffff,reg("PC"),reg("A0"),reg("A2"),reg("A10"),word(reg("A0")),word(0x010eb610),word(0x010eb630)))
    out:flush()
end)
emu.register_frame_done(function()
    local f=screen:frame_number()
    if not done and f>=frames then out:write(string.format('{"kind":"end","frame":%u,"events":%u}\n',f,count));out:close();done=true end
end,"stv_damage_events")
dofile(assert(os.getenv("STV_DAMAGE_LIFE_OBSERVER")))
