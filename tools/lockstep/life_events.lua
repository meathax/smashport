-- Development-only passive life-counter observer; no game-state writes.
local machine=manager.machine
local cpu=machine.devices[":maincpu"]
local space=cpu.spaces["program"]
local screen=machine.screens[":screen"]
local frames=assert(tonumber(os.getenv("STV_LS_FRAMES")))
local out=assert(io.open(assert(os.getenv("STV_LIFE_EVENTS_OUT")),"wb"))
local count,done=0,false
local function reg(name) return cpu.state[name].value & 0xffffffff end
local function word(a)
    local shift=a & 15
    local base=a & 0xfffffff0
    local value=space:read_u16(base)
    if shift~=0 then value=value | (space:read_u16(base+16)<<16) end
    return (value>>shift)&0xffff
end
out:write('{"schema":"stv.mame.life_writes.v1","phase":"write_tap_before_store","pc_semantics":"raw_cpu_state_not_instruction_start","damage_owner_proof":false}\n')
stv_life_taps={}
for player,address in ipairs({0x01086360,0x01086520}) do
    stv_life_taps[player]=space:install_write_tap(address,address+15,"stv_life_"..player,function(offset,data,mask)
        if done then return end
        local old=word(address)
        local merged=((old & (~mask)) | (data & mask)) & 0xffff
        local process=reg("A13")
        local valid=process>=0x01000000 and process+0x218<0x01100000
        local pnum,dead=-1,-1
        if valid then pnum=word(process+0x1d8)&0xff;dead=word(process+0x208) end
        count=count+1
        out:write(string.format('{"kind":"life_write","frame":%u,"player":%u,"address":%u,"data":%u,"mask":%u,"old":%u,"merged":%u,"pc_raw":%u,"a1_raw":%u,"a2_raw":%u,"a8_raw":%u,"a13_raw":%u,"process_player_raw":%d,"dead_raw":%d,"gamstate":%u,"circuit":%u,"wave":%u}\n',
            screen:frame_number(),player,offset,data&0xffff,mask&0xffff,old,merged,reg("PC"),reg("A1"),reg("A2"),reg("A8"),process,pnum,dead,
            word(0x010865a0),word(0x010eb610),word(0x010eb630)))
        out:flush()
    end)
end
emu.register_frame_done(function()
    local frame=screen:frame_number()
    if not done and frame>=frames then
        out:write(string.format('{"kind":"end","frame":%u,"events":%u}\n',frame,count))
        out:close();done=true
    end
end,"stv_life_events")
dofile(assert(os.getenv("STV_LIFE_BASE_RECORDER")))
