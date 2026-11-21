-- Passive before/after observations at the retail collision dispatch boundary.
local cpu=manager.machine.devices[":maincpu"]
local space=cpu.spaces["program"]
local screen=manager.machine.screens[":screen"]
local out=assert(io.open(assert(os.getenv("STV_COLLISION_EVENTS_OUT")),"wb"))
local frames=assert(tonumber(os.getenv("STV_LS_FRAMES")))
local count,pairs_count,overlaps,done,busy=0,0,0,false,false
local pending={}
local function reg(n)return cpu.state[n].value&0xffffffff end
local function word(a)
    local b=a&0xfffffff0
    return ((space:read_u16(b)|(space:read_u16(b+16)<<16))>>(a&15))&0xffff
end
local function long(a)return word(a)|(word(a+16)<<16) end
local function list(head,first,stride,limit)
    local found={};local p=long(head)
    while p~=0 do
        if p<first or p>=first+stride*limit or (p-first)%stride~=0 or found[p] then return found,false end
        found[p]=true;p=long(p)
    end
    return found,true
end
local function write(row)
    local fields={}
    for k,v in pairs(row) do fields[#fields+1]='"'..k..'":'..(type(v)=="string" and '"'..v..'"' or tostring(v)) end
    out:write('{'..table.concat(fields,',')..'}\n');out:flush();count=count+1
end
local function sample(context,phase)
    local objects,oc=list(0x01000040,0x010003a0,0x1f0,420)
    local processes,pc=list(0x01000000,0x01086790,0x5a0,240)
    local row={kind="collision_dispatch",phase=phase,pair=context.id,frame=screen:frame_number(),
        pc_raw=reg("PC"),sp=reg("SP"),object_list_complete=oc,process_list_complete=pc,
        callback1=reg("B1"),callback2=reg("B2"),object1=context.one,object2=context.two}
    for n,obj in ipairs({context.one,context.two}) do
        row["active"..n]=objects[obj]==true
        row["oid"..n]=objects[obj] and word(obj+0x150) or 0
        row["flags"..n]=objects[obj] and word(obj+0xb0) or 0
        local process=objects[obj] and long(obj+0x160) or 0
        row["process"..n]=process;row["process_active"..n]=processes[process]==true
        row["pid"..n]=processes[process] and word(process+0x20) or 0
        row["myplayer"..n]=processes[process] and word(process+0x138)&255 or 0
    end
    for player=1,2 do
        local torso=long(0x010de2b0+(player-1)*32)
        local legs=processes[torso] and long(torso+0x2c0) or 0
        local image=processes[torso] and long(torso+0x100) or 0
        row["owner"..player]=processes[torso]==true and processes[legs]==true and torso~=legs
            and word(torso+0x20)==246+player and word(legs+0x20)==246+player
            and word(torso+0x1d8)&255==player and long(legs+0x160)==torso
            and long(legs+0x100)==image and objects[image]==true and word(image+0x150)==0x4100+player
    end
    write(row)
end
out:write('{"schema":"stv.mame.collision_dispatch.v1","scope":"projectile_enemy_pairs"}\n')
stv_collision_tap=space:install_read_tap(0x01000060,0x0100006f,"stv_collision_dispatch",function(offset,data,mask)
    if busy or done or stv_lockstep_host_read then return end
    local pc=reg("PC");if pc~=0xffe557c0 and pc~=0xffe55890 then return end
    busy=true;local sp=reg("SP")
    if pc==0xffe557c0 and word(0x010865a0)==16 then
        local one,two=reg("A1"),reg("A2")
        local objects,complete=list(0x01000040,0x010003a0,0x1f0,420)
        if complete and objects[one] and objects[two] then
            local a,b=word(one+0x150),word(two+0x150)
            if (a>=0x4201 and a<=0x4206 and b&0xc000==0x8000) or
               (b>=0x4201 and b<=0x4206 and a&0xc000==0x8000) then
                if pending[sp] then
                    overlaps=overlaps+1
                    local old=pending[sp]
                    print(string.format("STV_COLLISION_OVERLAP frame=%u prior_frame=%u pair=%u sp=%08x old_objects=%08x,%08x new_objects=%08x,%08x old_callbacks=%08x,%08x new_callbacks=%08x,%08x a11=%08x",
                        screen:frame_number(),old.frame,old.id,sp,old.one,old.two,one,two,
                        old.callback1,old.callback2,reg("B1"),reg("B2"),reg("A11")))
                else
                    pairs_count=pairs_count+1
                    local context={id=pairs_count,one=one,two=two,frame=screen:frame_number(),
                        callback1=reg("B1"),callback2=reg("B2")};pending[sp]=context;sample(context,"before")
                end
            end
        end
    elseif pc==0xffe55890 and pending[sp] then
        sample(pending[sp],"after");pending[sp]=nil
    end
    busy=false
end)
emu.register_frame_done(function()
    local f=screen:frame_number()
    if not done and f>=frames then
        local unpaired=0;for _ in pairs(pending) do unpaired=unpaired+1 end
        out:write(string.format('{"kind":"end","frame":%u,"events":%u,"pairs":%u,"overlaps":%u,"unpaired":%u}\n',f,count,pairs_count,overlaps,unpaired))
        out:close();done=true
    end
end,"stv_collision_events")
dofile(assert(os.getenv("STV_COLLISION_BASE_OBSERVER")))
