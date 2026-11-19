#include "stv/source_room.hpp"

namespace stv {

Status source_onscreen_hulk_type(u16 circuit,u16 wave,u16 current_type,u16& out){
    // HULKS.ASM:ONSCRN_HULKS: circuits 0/1 keep the existing @HULKTYP word.
    if(circuit<2){out=current_type;return {};}
    static constexpr u8 htyp[25]={0,2,2,0,2,0,0,2,0,2,2,0,0,2,0,0,2,0,0,0,0,0,0,0,0};
    if(wave>=sizeof htyp)return Status::fail("ONSCRN_HULKS WAVE is outside the HTYP table",wave);
    out=htyp[wave];
    return {};
}

u16 source_hcount_stream_read(void* context){
    auto& stream=*static_cast<SourceHCountStream*>(context);
    if(!stream.values||stream.position>=stream.count){stream.exhausted=true;return 0;}
    return stream.values[stream.position++];
}

namespace {
constexpr unsigned wave_record_bytes=12;

ProcessAction room_step(NativeProcesses& processes,ProcessHandle handle,void* context){
    auto& room=*static_cast<SourceRoom*>(context);
    const u16 id=processes.cells[handle].id;
    if(id==source_lord_process_id)return room.lord_step(handle);
    if(id==source_hulk_process_id)return room.hulk_step(handle);
    return ProcessAction::fail("Room process has no translated native owner");
}

Status onscreen_initial(WaveOverlord& overlord,unsigned index,u16 count,void* context){
    if(index>=overlord.release.count)return Status::fail("OVERLORD ON1 record index is outside WAVE_RAM",index);
    const u16 type=overlord.release.records[index].type;
    // ROBO.ASM:TYP_TBL. Only type 6 (ONSCRN_HULKS) is translated here.
    if(type!=source_hulk_wave_type)return Status::fail("OVERLORD ON1 TYP_TBL handler is untranslated for this enemy type",type);
    return static_cast<SourceRoom*>(context)->onscreen_hulks(count);
}

Status release_spawn(WaveRelease& release,unsigned index,void*){
    // ROBO.ASM:TYP_TBL2 (SPAWN_HULKS, SPAWN_TNK, ...) is not yet owned here.
    const u16 type=index<release.count?release.records[index].type:0;
    return Status::fail("OVERLORD RLSE TYP_TBL2 spawn handler is untranslated for this enemy type",type);
}

struct Hasher {
    Sha256 state{};u8 buffer[512]{};usize used{};
    void flush(){
        u8 block[32+sizeof buffer];
        copy(block,state.bytes,32);copy(block+32,buffer,used);
        state=sha256({block,32+used});used=0;
    }
    void v(u64 value){if(used+8>sizeof buffer)flush();put64(buffer+used,value);used+=8;}
    Sha256 finish(){flush();return state;}
};
}

void SourceRoom::wave_ram_from_release(){
    zero(wave_ram,sizeof wave_ram);
    for(unsigned i=0;i<overlord.release.count&&i<8;++i){
        const auto& r=overlord.release.records[i];
        const u16 words[6]={r.type,r.remaining,r.level,r.rate,r.initial,r.counter};
        for(unsigned k=0;k<6;++k)put16(wave_ram+i*wave_record_bytes+k*2,words[k]);
    }
}

Status SourceRoom::release_from_wave_ram(){
    for(unsigned i=0;i<overlord.release.count&&i<8;++i){
        const u8* record=wave_ram+i*wave_record_bytes;
        auto& r=overlord.release.records[i];
        if(le16(record)!=r.type)return Status::fail("WAVE_RAM record type changed under a native actor",i);
        r.remaining=le16(record+2);r.level=le16(record+4);
        r.rate=le16(record+6);r.initial=le16(record+8);r.counter=le16(record+10);
    }
    return {};
}

void SourceRoom::reservations_from_world(SourceSpawnReservations& out) const {
    const auto& w=overlord.world;
    out=SourceSpawnReservations{};
    out.count=u8(w.keepxy);
    for(unsigned i=0;i<w.keepxy&&i<SourceSpawnReservations::capacity;++i)
        out.points[i]={(w.ramxy[i]&0xffffu)<<16,w.ramxy[i]};
}

Status SourceRoom::world_from_reservations(const SourceSpawnReservations& in){
    auto& w=overlord.world;
    if(in.count==0||in.count>=SourceSpawnReservations::capacity)
        return Status::fail("RAMXY/KEEPXY reservation count is outside MAX_ONSC",in.count);
    for(unsigned i=0;i<in.count;++i){
        if(in.points[i].x!=((in.points[i].y&0xffffu)<<16))
            return Status::fail("RAMXY entry is not an OEXIT-packed [Y:X] long",i);
        w.ramxy[i]=in.points[i].y;
    }
    for(unsigned i=in.count;i<SourceSpawnReservations::capacity;++i)w.ramxy[i]=0;
    w.keepxy=in.count;
    return {};
}

void SourceRoom::clear(){
    processes.initialize();objects.initialize();
    overlord=WaveOverlord{};rng=SourceRng{};hcount=SourceHCountStream{};
    tmp=hulk_type=from_door=0;zero(wave_ram,sizeof wave_ram);
    player_torsos[0]=player_torsos[1]=no_object;lord=no_process;
    regular_art=snake_art=nullptr;
    ticks=onscreen_hulk_calls=hulk_starts=0;boundary=nullptr;started=false;
    rejections=SourceSpawnRejections{};
}

Status SourceRoom::start(const WaveOverlord& selected,const EditorImages& regular,
                         const EditorImages& snake,const SourceRoomStart& input){
    if(selected.phase!=WaveFlowPhase::start||selected.wakes||!selected.release.count)
        return Status::fail("Room start requires an unstarted prepared WAVE_INFO selection");
    if(input.from_door>=5)return Status::fail("FROMDR is outside the RED_CARPET tables",input.from_door);
    if(!input.hcount||!input.hcount_count)
        return Status::fail("Room start requires an explicit HCOUNT source; no beam-timing model is connected");
    if(!input.player_present[0]&&!input.player_present[1])
        return Status::fail("Room start requires at least one PLYROBJS torso position");
    // Build in place: every rejectable input was checked above, and fresh
    // MPROC/object pools cannot fail the allocations below.
    started=false;boundary=nullptr;
    processes.initialize();
    objects.initialize();
    overlord=selected;
    rng.seed=input.rand;
    hcount={input.hcount,input.hcount_count,0,false};
    from_door=input.from_door;
    tmp=0;
    hulk_type=0; // Game-start @HULKTYP; ONSCRN_HULKS changes it only on circuits 2/3.
    zero(wave_ram,sizeof wave_ram);
    regular_art=&regular;
    snake_art=&snake;
    ticks=onscreen_hulk_calls=hulk_starts=0;
    rejections=SourceSpawnRejections{};
    lord=no_process;
    for(unsigned p=0;p<2;++p){
        player_torsos[p]=no_object;
        if(!input.player_present[p])continue;
        ObjectHandle torso;auto s=objects.allocate(torso);if(!s)return s;
        objects.cells[torso].sprite.position.x=input.player_x[p];
        objects.cells[torso].sprite.position.y=input.player_y[p];
        player_torsos[p]=torso;
    }
    // WAVES.ASM: CREATE LORDPID,OVERLORD.
    auto s=processes.create(source_lord_process_id,WaveOverlord::continuation,nullptr,lord);
    if(!s)return s;
    started=true;
    return validate();
}

Status SourceRoom::onscreen_hulks(u16 count){
    // HULKS.ASM:ONSCRN_HULKS / OH1. CREATE inserts after the current OVERLORD
    // process, so the last Hulk created is dispatched first in this same pass.
    const auto& w=overlord.world;
    u16 type=hulk_type;
    auto s=source_onscreen_hulk_type(w.circuit,w.wave,hulk_type,type);if(!s)return s;
    if(type!=0&&type!=2)return Status::fail("ONSCRN_HULKS HULKTYP art is untranslated",type);
    hulk_type=type;
    tmp=16;
    if(!count)return Status::fail("ONSCRN_HULKS DSJNE with a zero count is outside the source ON1 path");
    for(u16 i=0;i<count;++i){
        ProcessHandle created;
        s=processes.create(source_hulk_process_id,source_hulk_entry_ohulk,nullptr,created);
        if(!s)return s;
    }
    ++onscreen_hulk_calls;
    return {};
}

ProcessAction SourceRoom::lord_step(ProcessHandle handle){
    if(handle!=lord)return ProcessAction::fail("Only the room-created OVERLORD process may run LORDPID");
    WaveFlowHooks hooks{};
    hooks.initial=onscreen_initial;
    hooks.release=release_spawn;
    // SETMAX palette and O_ITEMS cash services stay null: OVERLORD names them.
    return overlord.step(hooks,this);
}

ProcessAction SourceRoom::hulk_step(ProcessHandle handle){
    auto& w=overlord.world;
    const EditorImages* primary=hulk_type==2?snake_art:regular_art;
    const EditorImages* secondary=hulk_type==2?regular_art:snake_art;
    const u16 player_x[2]={
        player_torsos[0]!=no_object?u16(objects.cells[player_torsos[0]].sprite.position.x>>16):u16(0),
        player_torsos[1]!=no_object?u16(objects.cells[player_torsos[1]].sprite.position.x>>16):u16(0)};
    const ProcessHandle player_processes[2]={no_process,no_process};
    ProcessAction action{};
    if(processes.cells[handle].continuation==source_hulk_entry_ohulk){
        wave_ram_from_release();
        HulkAccounting accounting{w.enemy_count[0],w.done[0]};
        SourceSpawnReservations reservations;reservations_from_world(reservations);
        SourceOhulkStartInput input{};
        input.actor.process=handle;
        input.actor.art=primary;
        input.actor.secondary_art=secondary;
        input.actor.hulk_type=hulk_type;
        input.actor.attract_mode=w.attract_mode;
        input.actor.wave_yx=w.wave_yx;
        input.actor.wave_records={wave_ram,sizeof wave_ram};
        input.actor.accounting=&accounting;
        input.actor.player_object_x=player_x;
        input.actor.player_torsos=player_torsos;
        input.actor.rng=&rng;
        input.actor.read_hcount=source_hcount_stream_read;
        input.actor.hcount_context=&hcount;
        input.actor.halt=w.halt;
        input.actor.hulk_down=w.done[0];
        input.actor.ehalt=w.ehalt;
        input.actor.player_processes=player_processes;
        input.placement.wave=w.wave;
        input.placement.tmp=tmp;
        input.placement.from_door=from_door;
        input.reservations=&reservations;
        rejections.x=rejected_x;rejections.capacity=sizeof rejected_x/sizeof rejected_x[0];
        input.placement.rejections=&rejections;
        SourceOhulkStartResult result{};
        auto s=source_ohulk_start(processes,objects,input,result);
        if(!s)return ProcessAction::fail(s.message);
        if(hcount.exhausted)return ProcessAction::fail("HCOUNT source exhausted during OHULK");
        tmp=result.placement.tmp_after;
        s=world_from_reservations(reservations);if(!s)return ProcessAction::fail(s.message);
        s=release_from_wave_ram();if(!s)return ProcessAction::fail(s.message);
        w.enemy_count[0]=accounting.count;
        w.done[0]=accounting.done;
        ++hulk_starts;
        if(result.actor.setup.create_dexit)
            return ProcessAction::fail("SETUP_HULK CREATE 0,DEXIT needs the room-owned DEXIT process");
        return result.actor.next_action;
    }
    u16 count=w.enemy_count[0];
    SourceHulkLupTopWorld world{};
    world.halt=w.halt;world.hulk_down=w.done[0];world.ehalt=w.ehalt;world.hulk_type=hulk_type;
    world.player_torsos=player_torsos;world.rng=&rng;
    world.read_hcount=source_hcount_stream_read;world.hcount_context=&hcount;
    world.player_processes=player_processes;world.hulk_count=&count;world.secondary_art=secondary;
    auto s=source_hulk_lup_top(processes,objects,handle,world,action);
    if(!s)return ProcessAction::fail(s.message);
    if(hcount.exhausted)return ProcessAction::fail("HCOUNT source exhausted during LUP_TOP");
    w.enemy_count[0]=count;
    return action;
}

Status SourceRoom::tick(){
    if(!started)return Status::fail("Room has not been started");
    if(boundary)return Status::fail(boundary);
    auto s=processes.dispatch(room_step,this);
    ++ticks;
    if(!s){boundary=s.message;return s;}
    return validate();
}

Status SourceRoom::validate() const {
    if(!started)return {};
    auto s=processes.validate();if(!s)return s;
    s=objects.validate();if(!s)return s;
    if(lord>=process_capacity)return Status::fail("Room OVERLORD handle is invalid");
    if(hcount.position>hcount.count)return Status::fail("HCOUNT stream position is past its end");
    // OVERLORD:AP seeds RAMXY[0]=[60,60] before any GET_VALID_XY can run.
    const bool seeded=overlord.phase!=WaveFlowPhase::start&&overlord.phase!=WaveFlowPhase::initialize;
    if(overlord.world.keepxy>=SourceSpawnReservations::capacity||(seeded&&overlord.world.keepxy==0))
        return Status::fail("RAMXY/KEEPXY count is invalid",overlord.world.keepxy);
    if(overlord.release.count>8)return Status::fail("WAVE_RAM has more than eight records");
    for(unsigned p=0;p<2;++p)if(player_torsos[p]!=no_object&&
        (player_torsos[p]>=object_capacity||!objects.cells[player_torsos[p]].allocated))
        return Status::fail("PLYROBJS stand-in handle is invalid",p);
    return {};
}

Sha256 SourceRoom::digest() const {
    Hasher h;
    h.v(0x524f4f4du);h.v(started);h.v(ticks);h.v(onscreen_hulk_calls);h.v(hulk_starts);
    h.v(rng.seed);h.v(hcount.count);h.v(hcount.position);h.v(hcount.exhausted);
    h.v(tmp);h.v(hulk_type);h.v(from_door);h.v(lord);h.v(boundary!=nullptr);
    for(auto t:player_torsos)h.v(t);
    h.v(processes.active_head);h.v(processes.free_head);h.v(processes.active_count);
    h.v(processes.accumulated_timer);h.v(processes.time_temp);h.v(processes.overload);h.v(processes.dispatches);
    for(const auto& p:processes.cells){
        h.v(p.active);if(!p.active)continue;
        h.v(p.next);h.v(p.id);h.v(p.time);h.v(p.continuation);h.v(p.saved_a12);h.v(p.depth);
        for(auto a:p.saved)h.v(a);
        for(unsigned i=0;i<sizeof p.data;i+=8){u64 word=0;for(unsigned k=0;k<8&&i+k<sizeof p.data;++k)word|=u64(p.data[i+k])<<(8*k);h.v(word);}
        for(unsigned i=0;i<p.depth&&i<16;++i)h.v(p.returns[i]);
    }
    h.v(objects.head);h.v(objects.free_head);h.v(objects.count);
    for(const auto& o:objects.cells){
        h.v(o.allocated);if(!o.allocated)continue;
        h.v(o.listed);h.v(o.next);h.v(o.process);h.v(o.id);h.v(o.z);h.v(o.clip);h.v(o.dma_xy);
        const auto& sp=o.sprite;
        h.v(sp.art==regular_art?1:sp.art==snake_art?2:sp.art?3:0);
        h.v(sp.image);h.v(u32(sp.palette));h.v(sp.flags);
        h.v(sp.position.x);h.v(sp.position.y);h.v(sp.position.vx);h.v(sp.position.vy);
        h.v(sp.raw_image!=nullptr);h.v(sp.palette_view!=nullptr);
    }
    const auto& w=overlord.world;
    h.v(u32(overlord.phase));h.v(overlord.wakes);h.v(overlord.initial_calls);h.v(overlord.cash_calls);h.v(overlord.initializations);
    h.v(w.circuit);h.v(w.wave);h.v(w.player_status);h.v(w.ehalt);h.v(w.halt);h.v(w.wave_yx);
    h.v(w.attract_mode);h.v(w.tv_process_present);h.v(w.keepxy);
    for(auto n:w.enemy_count)h.v(n);
    for(auto n:w.done)h.v(n);
    for(auto n:w.ramxy)h.v(n);
    for(auto n:w.gate_timer)h.v(n);
    h.v(w.econ);h.v(w.enon);h.v(w.pzon);h.v(w.shk_on);h.v(w.face_on);h.v(w.boss_on);h.v(w.toid_on);h.v(w.wntout);h.v(w.pctot);h.v(w.endon);
    h.v(w.maximums.max_hulk);h.v(w.maximums.max_tank);
    const auto& r=overlord.release;h.v(r.count);h.v(r.scans);h.v(r.requests);
    for(unsigned i=0;i<r.count&&i<8;++i){const auto& x=r.records[i];h.v(x.type);h.v(x.remaining);h.v(x.level);h.v(x.rate);h.v(x.initial);h.v(x.counter);}
    return h.finish();
}

}
