#include "stv/source_lab.hpp"
#include "source_states.hpp"
namespace stv {
namespace {
using namespace source_state;
constexpr const char* walk[10]={"PWK_UP","PWK_UP","PWK_DN","PWK_UP","PWK_RGT","PWK_UPRGT","PWK_DNRGT","PWK_LFT","PWK_UPLFT","PWK_DNLFT"};
constexpr const char* reverse_walk[10]={"PWK_UP","PWK_DN","PWK_UP","PWK_UP","PWK_LFT","PWK_DNLFT","PWK_UPLFT","PWK_RGT","PWK_DNRGT","PWK_UPRGT"};
constexpr const char* stand[10]={"P1S12L1","P1S12L1","P1SH6L1","P1S12L1","P1SH9L1","P1S10L1","P1SH8L1","P1SH9L1","P1S10L1","P1SH8L1"};
constexpr const char* torso_walk[10]={"P1WG12T1","P1WG12T1","P1WG6T1","P1WG12T1","P1WG9T1","P1WG10T1","P1WG8T1","P1WG9T1","P1WG10T1","P1WG8T1"};
constexpr const char* shoot[10]={"SHT_UP","SHT_UP","SHT_DN","SHT_UP","SHT_RGT","SHT_UPRGT","SHT_DNRGT","SHT_LFT","SHT_UPLFT","SHT_DNLFT"};
constexpr const char* shoot2[10]={"SHT_UP","SHT_UP","SHT_DN","SHT_UP","SHT_RGT2","SHT_UPRGT","SHT_DNRGT2","SHT_LFT2","SHT_UPLFT","SHT_DNLFT"};
constexpr const char* shoot_legs[10]={"L_SHT_UP","L_SHT_UP","L_SHT_DN","L_SHT_UP","L_SHT_RGT","L_SHT_UPRGT","L_SHT_DNRGT","L_SHT_LFT","L_SHT_UPLFT","L_SHT_DNLFT"};
constexpr u16 flip[10]={0,0,0,0,16,16,16,0,0,0};
constexpr u16 reverse_bits[10]={0,0b1001000100,0b0100100010,0,0b1110000000,0b1010000100,0b0110000010,0b0001110000,0b0001010100,0b0000110010};
constexpr u16 age_table[12]={80,150,90,110,140,210,250,50,70,125,45,80};
constexpr const char* impacts[4]={"EXPL","EXPR","EXPT","EXPD"};
constexpr const char* deaths[8]={"VDIE","HDIER","HDIER","HDIER","VDIED","HDIE","HDIE","HDIE"};
constexpr u32 death_vx[8]={0,0x5c00,0x5c00,0x5c00,0,0u-0x5c00,0u-0x5c00,0u-0x5c00};
constexpr u32 death_vy[8]={0u-0x5000,0u-0x5000,0,0x5000,0x5000,0x5000,0,0u-0x5000};
int palette_named(const EditorImages& bank,const char* name) {
    for(unsigned i=0;i<bank.palette_count;++i) {EditorPalette p;if(bank.palette(i,p)&&name_equal(p.name,name))return int(i);}
    return -1;
}
ProcessAction failed(Status s){return ProcessAction::fail(s.message?s.message:"Native source continuation failed");}
}

Status SourceLab::load(const SourcePack& pack) {
    loaded=false;
    const auto* pa=pack.find("PLYRS2.IMG");const auto* ha=pack.find("FINGRNT.IMG");
    const auto* da=pack.find("NUARENA3.IMG");const auto* ba=pack.find("BMB.IMG");const auto* bd=pack.find("FINAL7.BDD");const auto* bl=pack.find("FINAL7.BDB");
    if(!pa||!ha||!ba||!bd||!bl||!da)return Status::fail("Source lab requires PLYRS2/FINGRNT/BMB IMG and FINAL7 BDD/BDB");
    auto s=player_art.open(pa->bytes);if(!s)return s;
    s=hulk_art.open(ha->bytes);if(!s)return s;
    const auto* secondary_hulk=pack.find("NSNAK.IMG");
    if(!secondary_hulk)return Status::fail("Source lab requires the original NSNAK secondary Hulk animation bank");
    s=hulk_secondary_art.open(secondary_hulk->bytes);if(!s)return s;
    s=bullet_art.open(ba->bytes);if(!s)return s;
    s=background.open(bd->bytes,bl->bytes);if(!s)return s;
    s=door_art.open(da->bytes);if(!s)return s;
    s=arena.bind(background);if(!s)return s;
    int module=background.find_module("ARENA");if(module<0)return Status::fail("FINAL7 editor ARENA module not found");
    background_module=unsigned(module);
    // WAVES.ASM AX4/DOFA, circuit 0: original RGB555 auto-erase color (6,6,6).
    for(auto& pixel:background_pixels)pixel=editor_color(u16(6*32*32+6*32+6));
    s=background.draw_module(background_module,{background_pixels,lab_width,lab_height,lab_width,lab_width*lab_height});
    if(!s)return s;
    loaded=true;return reset();
}
Status SourceLab::reset(u32 seed,unsigned active_players,unsigned fixture_hulks,bool with_lifecycle) {
    if(!loaded)return Status::fail("Load verified original source assets before resetting lab");
    if(active_players<1||active_players>2||fixture_hulks>64)return Status::fail("Unsupported source-lab fixture size");
    processes.initialize();objects.initialize();
    for(auto& c:contexts)c=LabContext{};
    for(auto& p:players)p=LabPlayer{};
    lifecycle_enabled=with_lifecycle;halted=false;circuit=0;wave=1;early_hulk_difficulty_cleared=false;
    for(unsigned i=0;i<2;++i)players[i].life.number=u8(i<active_players?i+1:0);
    input={};stats={};source_hulk_count=0;stats.initial_fixture_hulks=fixture_hulks;player_count=active_players;
    rng.seed=seed;random_calls=0;sound={};failure={};
    if(with_lifecycle){auto a=setup_source_arena();if(!a)return a;}
    else {doors={};for(auto& h:door_objects)h=no_object;for(auto& t:door_tasks)t=DoorTask{};
        for(auto& pixel:background_pixels)pixel=editor_color(u16(6*32*32+6*32+6));
        auto a=background.draw_module(background_module,{background_pixels,lab_width,lab_height,lab_width,lab_width*lab_height});if(!a)return a;}
    LabContext context;ProcessHandle p;
    auto s=spawn_process(0,boot,context,p);if(!s)return s;
    // No gameplay tick is consumed by initialization. The boot script itself is
    // explicitly authored test setup, then source native processes take over.
    return {};
}
Status SourceLab::join_fixture_player(unsigned player) {
    if(!loaded||!failure)return Status::fail("Source fixture is not ready for a player join");
    if(player!=1||player_count!=1||players[1].life.number)return Status::fail("Only an absent second fixture player can join");
    // DRAW_PLYR_TORSO creates DRAW_PLYR_LEGS on its first dispatch. Check both
    // slots before P_START charges a credit; never strand a paid join at pool
    // exhaustion. Placement remains the lab's documented authored setup.
    if(process_capacity-processes.active_count<2||object_capacity-objects.count<2)
        return Status::fail("Source fixture has no room for the second player's processes and objects");
    LabContext context;context.player=player;ProcessHandle child;
    auto s=spawn_process(u16(0x10+player),torso_init,context,child);if(!s)return s;
    players[player].life.number=u8(player+1);
    player_count=2;
    return {};
}
Status SourceLab::spawn_process(u16 id,u32 state,const LabContext& c,ProcessHandle& out) {
    auto s=processes.create(id,state,nullptr,out);if(s)contexts[out]=c;return s;
}
Status SourceLab::new_sprite(ProcessHandle ph,const EditorImages& bank,const char* name,u16 flags,u16 id,u16 z,u32 x,u32 y,u32 vx,u32 vy,int palette) {
    int img=bank.find(name);if(img<0)return Status::fail("Required original sprite not in selected bank");
    if(palette<0){EditorImage initial;auto result=bank.image(unsigned(img),initial);if(!result)return result;palette=int((initial.selector&255)-3);}
    ObjectHandle object;auto s=objects.allocate(object);if(!s)return s;
    auto& o=objects.cells[object];o.sprite={&bank,unsigned(img),palette,flags,{0,0,vx,vy}};
    o.process=ph;o.id=id;o.z=z;
    s=place_sprite_anchor(o.sprite,x,y);if(!s){objects.erase(object);return s;}
    s=objects.insert(object);if(!s){objects.erase(object);return s;}
    contexts[ph].object=object;return {};
}
Status SourceLab::image(ObjectHandle object,const char* name,u16 flags) {
    if(object>=object_capacity||!objects.cells[object].allocated)return Status::fail("ANI references inactive object");
    auto& sprite=objects.cells[object].sprite;int idx=sprite.art->find(name);
    if(idx<0)return Status::fail("ANI original image unresolved");
    return set_animation_image(sprite,unsigned(idx),flags);
}
Status SourceLab::script(ProcessHandle ph,const char* module,const char* label) {
    auto& c=contexts[ph];c.animation=find_animation(module,label);c.frame=0;
    if(!c.animation)return Status::fail("Original animation script is not lowered");
    return {};
}
ProcessAction SourceLab::frame(ProcessHandle ph,u32 next) {
    auto& c=contexts[ph];
    if(!c.animation||c.frame>=c.animation->count||c.object>=object_capacity)return ProcessAction::fail("Unresolved FRANIM frame");
    const auto& f=c.animation->frames[c.frame++];auto s=apply_source_frame(objects.cells[c.object].sprite,f);
    if(!s)return failed(s);
    return ProcessAction::wait(u16(f.control&255),next);
}
u16 SourceLab::horizontal_count() {
    // A reproducible TEST HCOUNT input, not a claim about original pixel clocks.
    return u16((stats.ticks*17+u64(random_calls++)*13)&0x1ff);
}
Status SourceLab::sound_command(const u16* words,usize count) {
    SoundWrite writes[32];SoundTrace trace{writes,32};bool accepted=false;
    auto s=sound.load(words,count,0,trace,accepted);if(!s)return s;
    for(usize i=0;i<trace.count;++i) {
        const auto& w=writes[i];u8 bytes[12];put64(bytes,w.tick);put16(bytes+8,w.bus_value);bytes[10]=w.channel;bytes[11]=w.kind;
        for(auto b:bytes)stats.sound_hash=(stats.sound_hash^b)*1099511628211ull;
    }
    stats.sound_writes+=trace.count;return {};
}
Status SourceLab::sound_command(const char* source_label) {
    const auto* table=find_source_sound(source_label);
    if(!table)return Status::fail("Unknown original SOUNDS.ASM command label");
    return sound_command(table->words,table->word_count);
}
Status SourceLab::player_initialize(ProcessHandle ph) {
    auto& c=contexts[ph];auto& p=players[c.player];p.torso_process=ph;
    int pal=palette_named(player_art,c.player?"NUP2":"NUP1");
    if(pal<0)return Status::fail("Selected source player palette is missing");
    // Test placement, not arcade new-game/entrance code.
    const u32 x=(c.player?260u:140u)<<16,y=165u<<16;
    auto s=new_sprite(ph,player_art,"P1WG9T1",u16(2|0x800|(c.player?0:16)),u16(0x4101+c.player),159,x,y,0,0,pal);if(!s)return s;
    p.torso=c.object;p.direction=u16(0x8000|(c.player?7:4));
    s=sync_player_bounds(c.player);if(!s)return s;
    LabContext legs;legs.player=c.player;ProcessHandle lp;
    s=spawn_process(u16(0x10+c.player),legs_top,legs,lp);if(!s)return s;p.legs_process=lp;
    s=new_sprite(lp,player_art,"P1SH9L1",u16(2|(c.player?0:16)),u16(0x4101+c.player),158,x,y,0,0,pal);if(!s)return s;
    p.legs=contexts[lp].object;
    // DRAW_PLYR_LEGS starts its script at PWK_RGT regardless of placement.
    return script(lp,"ROBO.ASM","PWK_RGT");
}
Status SourceLab::sync_player_bounds(unsigned player) {
    if(player>=2)return Status::fail("PLYRPRCS bounds slot is outside the two-player table",player);
    const ProcessHandle handle=players[player].torso_process;
    if(handle>=process_capacity||processes.cells[handle].id!=u16(0x10u+player))
        return Status::fail("PLYRPRCS bounds slot has no matching native torso process",player);
    const PlayerBounds active_bounds=lifecycle_enabled?
        source_player_bounds(players[player].helper_bounds?1:0):bounds;
    NativeProcess next=processes.cells[handle];
    auto status=write_field({next.data,sizeof next.data},{320},32,active_bounds.top);
    if(!status)return status;
    status=write_field({next.data,sizeof next.data},{352},32,active_bounds.bottom);
    if(!status)return status;
    status=write_field({next.data,sizeof next.data},{384},32,active_bounds.left);
    if(!status)return status;
    status=write_field({next.data,sizeof next.data},{416},32,active_bounds.right);
    if(!status)return status;
    processes.cells[handle]=next;
    return {};
}
ProcessAction SourceLab::legs_step(ProcessHandle ph) {
    auto& c=contexts[ph];auto& p=players[c.player];
    auto& legs=objects.cells[p.legs].sprite;auto& torso=objects.cells[p.torso].sprite;
    if(processes.cells[ph].continuation==legs_after_shoot)return ProcessAction::wait(1,legs_top); // SL_1 after FRANIM
    if(lifecycle_enabled&&p.life.dead) {
        // ROBO MOVE_TOP: ordinary Hulk death -> SLEEP 5 -> LSPN -> HG.
        return ProcessAction::wait(5,legs_spin_wait);
    }
    for(unsigned guard=0;guard<8;++guard) {
        const auto active_bounds=lifecycle_enabled?source_player_bounds(p.helper_bounds?1:0):bounds;
        const u16 direction=player_direction(input.move[c.player],legs.position,active_bounds,false);
        // UPDATE_PLYRXY sets carry only if the computed direction is nonzero.
        if(!direction) {
            legs.position.vx=legs.position.vy=torso.position.vx=torso.position.vy=0;
            p.have_reverse=true;
            if(p.shoot_flag) {
                p.direction=u16(0x8000|p.shoot_flag);
                auto s=script(ph,"ROBO.ASM",shoot_legs[p.shoot_flag]);if(!s)return failed(s);
                return frame(ph,legs_after_shoot);
            }
            p.direction|=0x8000;unsigned d=p.direction&31;
            if(d>=10)return ProcessAction::fail("Original standing direction outside table");
            auto s=image(p.legs,stand[d],u16(2|flip[d]));if(!s)return failed(s);
            return ProcessAction::wait(1,legs_top);
        }
        if(p.reverse) {
            p.have_reverse=true;p.direction=direction;
            if(p.reverse_direction!=direction) {
                p.reverse_direction=direction;auto s=script(ph,"ROBO.ASM",reverse_walk[direction]);if(!s)return failed(s);
            }
        } else {
            const auto old=p.direction&31;p.direction=direction;
            if(p.have_reverse||old!=direction) {
                p.have_reverse=false;auto s=script(ph,"ROBO.ASM",walk[direction]);if(!s)return failed(s);
            }
        }
        auto s=player_velocity(direction,p.shoes,legs.position,torso.position);if(!s)return failed(s);
        if(!c.animation)return ProcessAction::fail("Player walking script unresolved");
        if(c.frame==c.animation->count) {c.frame=0;continue;} // FRANIM end -> reset -> MOVE_TOP, no sleep
        return frame(ph,legs_top);
    }
    return ProcessAction::fail("Player animation continuation failed to yield");
}
ProcessAction SourceLab::torso_step(ProcessHandle ph) {
    auto& c=contexts[ph];auto& p=players[c.player];
    if(processes.cells[ph].continuation==torso_init) {auto s=player_initialize(ph);if(!s)return failed(s);}
    if(processes.cells[ph].continuation==torso_frames) {
        if(c.frame<c.animation->count)return frame(ph,torso_frames);
        p.shoot_flag=0;
    }
    p.reverse=false; // TORSO_1
    if(lifecycle_enabled&&processes.cells[ph].continuation!=torso_resume) {
        source_torso_immunity_step(p.life);
        if(p.life.dead) {
            if(source_player_death_kind(p.life.dead,false)!=PlayerDeathKind::spin)
                return ProcessAction::fail("Non-spin player death requires untranslated chunk/shock services");
            LabContext cheer;ProcessHandle child;
            auto s=spawn_process(0,cheer_wait,cheer,child);if(!s)return failed(s);
            auto& legs=objects.cells[p.legs].sprite;auto& torso=objects.cells[p.torso].sprite;
            legs.position.vx=legs.position.vy=torso.position.vx=torso.position.vy=0;
            return ProcessAction::wait(5,player_spin_wait);
        }
    }
    const u16 shot=firing_direction(input.fire[c.player]);
    if(!shot) {
        p.old_shoot=0;unsigned d=p.direction&31;
        if(d>=10)return ProcessAction::fail("Torso walking direction outside original table");
        auto& torso=objects.cells[p.torso].sprite;const auto& legs=objects.cells[p.legs].sprite;
        u32 x,y;auto s=sprite_anchor(legs,x,y);if(!s)return failed(s);
        s=place_sprite_anchor(torso,x,y);if(!s)return failed(s);
        if(p.direction&0x8000)torso.position.vx=torso.position.vy=0;
        s=image(p.torso,torso_walk[d],u16(2|0x800|flip[d]));if(!s)return failed(s);
        return ProcessAction::wait(1,torso_top);
    }
    unsigned movement=p.direction&15;
    if(movement>=10)return ProcessAction::fail("Torso reverse-direction index invalid");
    if(shot!=p.old_shoot)p.reverse_direction=0;
    p.old_shoot=shot;p.shoot_flag=shot;p.reverse=(reverse_bits[shot]&(1u<<movement))!=0;
    auto s=script(ph,"ROBO.ASM",movement<3?shoot2[shot]:shoot[shot]);if(!s)return failed(s);
    LabContext bullet;bullet.player=c.player;bullet.direction=shot;ProcessHandle bp;
    s=spawn_process(0x20,bullet_init,bullet,bp);if(!s)return failed(s);
    s=sound_command("SHOT");if(!s)return failed(s);
    // Bullet child executes after this process sleeps: its position is read from
    // the torso AFTER this initial ANI, exactly as CREATE then FRANIM in source.
    return frame(ph,torso_frames);
}
ProcessAction SourceLab::bullet_step(ProcessHandle ph) {
    auto& c=contexts[ph];const u32 state=processes.cells[ph].continuation;
    if(state==bullet_init) {
        BulletSetup b;auto s=regular_bullet_setup(c.direction,objects.cells[players[c.player].torso].sprite.position,b);if(!s)return failed(s);
        c.direction=b.compass_direction;c.age=b.age;
        s=new_sprite(ph,bullet_art,b.image,b.flags,b.id,159,b.position.x,b.position.y,b.position.vx,b.position.vy);if(!s)return failed(s);
        put16(processes.cells[ph].data,c.direction);
        processes.cells[ph].data[7]=u8(c.player+1);
        ++stats.shots;++stats.bullets;
    }
    if(state==bullet_frames) {
        if(c.frame<c.animation->count)return frame(ph,bullet_frames);
        auto s=objects.erase(c.object);if(!s)return failed(s);
        --stats.bullets;return ProcessAction::suicide();
    }
    auto& object=objects.cells[c.object];u8 wall=screen_test(object.sprite,0x20,0x21,0x173,0xfc);
    if(!wall)return ProcessAction::wait((stats.ticks&1)?1:2,bullet_loop);
    if(wall>4)return ProcessAction::fail("Bullet screen test failed");
    auto s=sound_command("TING");if(!s)return failed(s);++stats.wall_impacts;
    if(wall==4) {s=objects.erase(c.object);if(!s)return failed(s);--stats.bullets;return ProcessAction::suicide();}
    // This lab deliberately fixes all gates CLOSED. Open gate path is not called.
    object.sprite.position.vx=object.sprite.position.vy=0;object.sprite.flags|=0x800;object.id=0;
    s=script(ph,"SHOTS.ASM",impacts[wall-1]);if(!s)return failed(s);
    return frame(ph,bullet_frames);
}
ProcessAction SourceLab::hulk_step(ProcessHandle ph) {
    auto& c=contexts[ph];
    if(processes.cells[ph].continuation==hulk_init) {
        auto s=new_sprite(ph,hulk_art,"GB6W5A",18,0x8600,159,c.anchor_x,c.anchor_y);if(!s)return failed(s);
        auto& process=processes.cells[ph];
        process.saved[0]=c.object;
        const u16 age=u16(age_table[rng.range(0,10,horizontal_count())]*2);
        s=source_hulk_pdata_write(process,SourceHulkField::direction,3);if(!s)return failed(s);
        s=source_hulk_pdata_write(process,SourceHulkField::seek_direction,5);if(!s)return failed(s);
        s=source_hulk_pdata_write(process,SourceHulkField::control_flags,1);if(!s)return failed(s);
        s=source_hulk_pdata_write(process,SourceHulkField::age,age);if(!s)return failed(s);
        s=source_hulk_pdata_write(process,SourceHulkField::keep_age,age);if(!s)return failed(s);
        s=source_hulk_pdata_write(process,SourceHulkField::level,c.level);if(!s)return failed(s);
        s=source_hulk_pdata_write(process,SourceHulkField::oscillation,0);if(!s)return failed(s);
        s=source_hulk_pdata_write(process,SourceHulkField::batting,0);if(!s)return failed(s);
        ++stats.hulks;++source_hulk_count;c.player=(stats.hulks&1)%player_count;
        s=source_hulk_pdata_write(process,SourceHulkField::seek_player,u8(c.player));if(!s)return failed(s);
        s=source_hulk_do_diff(process);if(!s)return failed(s);
        s=source_hulk_get_stand(processes,objects,ph,0);if(!s)return failed(s);
        s=source_hulk_get_run(process,0);if(!s)return failed(s);
        const u16 player_torsos[2]={players[0].torso,players[1].torso};
        auto hcount=[](void* context)->u16{return static_cast<SourceLab*>(context)->horizontal_count();};
        SourceHulkJustSkResult target{};
        s=source_hulk_just_sk(processes,ph,objects,player_torsos,rng,hcount,this,target);if(!s)return failed(s);
        s=source_hulk_into(processes,ph,objects,target.target.direction);if(!s)return failed(s);
    }
    const u16 player_torsos[2]={players[0].torso,players[1].torso};
    const ProcessHandle player_processes[2]={players[0].torso_process,players[1].torso_process};
    auto hcount=[](void* context)->u16{return static_cast<SourceLab*>(context)->horizontal_count();};
    SourceHulkLupTopWorld world{};
    bool has_live_torso=false;
    for(unsigned player=0;player<2;++player)if(player_torsos[player]<object_capacity&&
        objects.cells[player_torsos[player]].allocated&&u16(objects.cells[player_torsos[player]].sprite.position.x>>16))
        has_live_torso=true;
    // The fixture has no retail ALLDIE/buy-in owner. Hold its Hulk kernels once
    // every PLYROBJS target is off-screen instead of inventing a target value.
    world.halt=u16(halted||!has_live_torso);
    world.ehalt=0; // The component fixture releases the arena-entry halt.
    world.hulk_type=0;
    world.player_torsos=player_torsos;
    world.rng=&rng;
    world.read_hcount=hcount;
    world.hcount_context=this;
    world.player_processes=player_processes;
    world.hulk_count=&source_hulk_count;
    world.secondary_art=&hulk_secondary_art;
    const auto continuation=processes.cells[ph].continuation;
    const bool bat_resume=continuation==source_hulk_resume_bat_first||
                          continuation==source_hulk_resume_bat_second;
    u32 batting_before=0;
    auto s=source_hulk_pdata_read(processes.cells[ph],SourceHulkField::batting,
                                  batting_before);
    if(!s)return failed(s);
    ProcessAction action{};
    s=source_hulk_lup_top(processes,objects,ph,world,action);
    if(!s)return failed(s);
    if(action.kind==ProcessAction::die){--stats.hulks;c.object=no_object;}
    u32 batting_after=0;
    s=source_hulk_pdata_read(processes.cells[ph],SourceHulkField::batting,
                             batting_after);
    if(!s)return failed(s);
    const bool bat_continuation=action.next==source_hulk_resume_bat_first||
                                action.next==source_hulk_resume_bat_second;
    if(bat_continuation) {
        const SourceAnimation* animation=nullptr;u16 cursor=0;
        s=decode_source_animation_cursor(processes.cells[ph].saved[1],animation,cursor);
        if(!s)return failed(s);
        c.animation=animation;c.frame=cursor;
        c.repeats=action.next==source_hulk_resume_bat_first?2:1;
        if(batting_before&&!bat_resume)++stats.bat_swings;
    } else if(bat_resume&&batting_after==0) {
        // Keep the fixture's diagnostic mirror aligned with BATDN's restored A9.
        const SourceAnimation* animation=nullptr;u16 cursor=0;
        s=decode_source_animation_cursor(processes.cells[ph].saved[1],animation,cursor);
        if(!s)return failed(s);
        c.animation=animation;c.frame=cursor;c.repeats=0;c.bat_player=0;
    }
    return action;
}
Status SourceLab::kill_regular(ObjectHandle enemy,ObjectHandle bullet) {
    auto& e=objects.cells[enemy];auto& b=objects.cells[bullet];
    const auto& ec=contexts[e.process];const auto& bc=contexts[b.process];
    LabContext death;death.player=bc.player;death.direction=ec.direction;
    if(processes.cells[e.process].id==source_hulk_process_id){u32 direction=0;auto result=source_hulk_pdata_read(processes.cells[e.process],SourceHulkField::direction,direction);if(!result)return result;death.direction=u16(direction);}
    death.palette=e.sprite.palette;
    if(death.palette<0) {EditorImage i;auto s=e.sprite.art->image(e.sprite.image,i);if(!s)return s;death.palette=int((i.selector&255)-3);}
    auto s=sprite_anchor(e.sprite,death.anchor_x,death.anchor_y);if(!s)return s;
    const bool ring=b.id==0x4103;
    unsigned dir=((ring?le16(processes.cells[b.process].data):bc.direction)-1)&7;death.vx=death_vx[dir];death.vy=death_vy[dir];
    ProcessHandle dp;s=spawn_process(0,death_init,death,dp);if(!s)return s;
    s=processes.kill(e.process);if(!s)return s;s=objects.erase(enemy);if(!s)return s;
    s=sound_command("EXP1");if(!s)return s;
    // Source ColLists calls both callbacks even when first victim is deleted.
    if(!ring) {s=processes.kill(b.process);if(!s)return s;s=objects.erase(bullet);if(!s)return s;--stats.bullets;}
    else ++stats.ring_kills;
    ++stats.hulk_kills;return {};
}
ProcessAction SourceLab::death_step(ProcessHandle ph) {
    auto& c=contexts[ph];
    if(processes.cells[ph].continuation==death_init) {
        auto effects=source_score_add(players[c.player].score,0x500,player_count==2?3:1,false);
        if(effects.extra_life_requested)++players[c.player].extra_requests;
        --stats.hulks;--source_hulk_count;++stats.death_effects;
        auto s=new_sprite(ph,hulk_art,"VHIT1",u16(2|0x800|32),0,199,c.anchor_x,c.anchor_y,c.vx,c.vy,c.palette);if(!s)return failed(s);
        if(c.direction<1||c.direction>8)return ProcessAction::fail("HULK_EXP direction outside source table");
        s=script(ph,"HULKS.ASM",deaths[c.direction-1]);if(!s)return failed(s);
    }
    if(c.frame<c.animation->count)return frame(ph,death_frames);
    auto s=objects.erase(c.object);if(!s)return failed(s);--stats.death_effects;return ProcessAction::suicide();
}
Status SourceLab::collide_regular() {
    // COLL2 ColScan0/CScnRng/ColLists: original EList/PList construction order.
    // Regular Hulks, bullets, player legs and rings are the implemented class pairs.
    i32 end=0,remaining=400;
    do {
        const i32 start=end;end+=128;remaining-=128;
        if(remaining<0){end-=remaining;remaining=0;}
        ObjectHandle enemies[object_capacity],player_list[object_capacity];unsigned ne=0,np=0;
        for(auto h=objects.head;h!=no_object;h=objects.cells[h].next) {
            const auto& o=objects.cells[h];if(o.sprite.flags&0x800)continue;
            EditorImage im;auto s=o.sprite.art->image(o.sprite.image,im);if(!s)return s;
            u32 x=u32(signed16(u16(o.sprite.position.x>>16)));
            if(x>u32(end)||x+im.stride<u32(start))continue;
            if(o.id==0x8600)enemies[ne++]=h;
            else if(o.id==0x4201||(lifecycle_enabled&&(o.id&0xff00)==0x4100))player_list[np++]=h;
        }
        for(unsigned ei=0;ei<ne;++ei) {
            auto eh=enemies[ei];if(!objects.cells[eh].allocated)continue;
            for(unsigned pi=0;pi<np;++pi) {
                auto ph=player_list[pi];if(!objects.cells[ph].allocated)continue;
                ++stats.pair_tests;bool hit=false;
                const auto& a=objects.cells[eh].sprite;const auto& b=objects.cells[ph].sprite;
                Status s;
                if((a.flags|b.flags)&0x400)s=source_pixel_overlap(a,b,hit);
                else {
                    EditorImage ai,bi;s=sprite_image(a,ai);if(!s)return s;
                    s=sprite_image(b,bi);if(!s)return s;
                    i32 ax=signed16(u16(a.position.x>>16)),ay=signed16(u16(a.position.y>>16));
                    i32 bx=signed16(u16(b.position.x>>16)),by=signed16(u16(b.position.y>>16));
                    hit=!(bx>ax+i32(ai.stride)||bx+i32(bi.stride)<ax||by>ay+i32(ai.height)||by+i32(bi.height)<ay);
                }
                if(!s)return s;
                if(!hit)continue;
                const u16 id=objects.cells[ph].id;
                if(id==0x4201||id==0x4103){s=kill_regular(eh,ph);if(!s)return s;break;}
                s=collide_player(eh,ph);if(!s)return s;
            }
        }
    }while(remaining>0);
    return {};
}
ProcessAction SourceLab::step(NativeProcesses& processes,ProcessHandle ph,void* context) {
    auto& lab=*static_cast<SourceLab*>(context);const auto state=processes.cells[ph].continuation;
    auto execute=[&]() -> ProcessAction {
    switch(state) {
    case boot: {
        LabContext c;ProcessHandle child;auto s=lab.spawn_process(0,collision_sleep,c,child);if(!s)return failed(s);
        // Authored fixture grid: never identified as WAVES/OHULK placement.
        for(unsigned i=0;i<lab.stats.initial_fixture_hulks;++i) {
            c={};c.anchor_x=(50+(i%8)*42u)<<16;c.anchor_y=(70+(i/8)*26u)<<16;c.level=0;
            s=lab.spawn_process(source_hulk_process_id,hulk_init,c,child);if(!s)return failed(s);
        }
        // Insert players last so that their objects exist before hulk seeks.
        for(unsigned i=0;i<lab.player_count;++i) {c={};c.player=i;s=lab.spawn_process(u16(0x10+i),torso_init,c,child);if(!s)return failed(s);}
        return ProcessAction::suicide();
    }
    case source_state::door_step:return lab.door_step(ph);
    case torso_init:case torso_top:case torso_resume:case torso_frames:return lab.torso_step(ph);
    case legs_top:case legs_after_shoot:return lab.legs_step(ph);
    case bullet_init:case bullet_loop:case bullet_frames:return lab.bullet_step(ph);
    case hulk_init:case hulk_loop:
    case hulk_resume_after_t4:case hulk_resume_after_way:case hulk_resume_after_retry:
    case hulk_resume_after_get_run:case hulk_resume_after_franim:
    case source_hulk_resume_lup_top:case source_hulk_resume_bat_first:
    case source_hulk_resume_bat_second:
        return lab.hulk_step(ph);
    case player_spin_wait:case player_spin_frames:case player_fall_frames:
    case player_all_die:case player_respawn_wait:case player_buyin_wait:
        return lab.player_death_step(ph);
    case legs_spin_wait:case legs_spin_frames:case legs_dead_wait:
        return lab.player_legs_death_step(ph);
    case cheer_wait:case cheer_emit:case blood_wait_extra:case blood_wait:
    case blood_init:case blood_frames:case entry_init:case entry_loop:case entry_sleep:
    case ring_init:case ring_loop:case ring_blink_init:case ring_blink_on:case ring_blink_off:
        return lab.lifecycle_effect_step(ph);
    case death_init:case death_frames:return lab.death_step(ph);
    case collision_sleep:return ProcessAction::wait(1,collision_scan);
    case collision_scan: {auto s=lab.collide_regular();return s?ProcessAction::wait(1,collision_scan):failed(s);}
    default:return ProcessAction::fail("Unsupported native source process continuation");
    }
    };
    const auto action=execute();
    // Commit scalar source fields before another continuation can free/reuse the
    // cell in this same dispatch. End-of-tick synchronization is too late.
    if(lab.lifecycle_enabled)lab.sync_source_fields(ph);
    return action;
}
Status SourceLab::tick(const LabInput& next) {
    if(!loaded)return Status::fail("Original source data not loaded");
    if(!failure)return failure;
    input=next;
    for(unsigned i=0;i<player_count;++i)if(players[i].entry_script){input.move[i]=u8(players[i].scripted_bits&15);input.fire[i]=u8((players[i].scripted_bits>>4)&15);}
    ++stats.ticks;
    // MAIN DIRQ0: input sample, DISPLAY velocity integration, then return to
    // process dispatcher. One synthetic display boundary per lab tick; exact
    // board interrupt/cycle ordering and HCOUNT are not claimed as validated.
    objects.integrate();objects.sort_pass();
    auto s=processes.dispatch(step,this);if(!s){failure=s;return s;}
    SoundWrite writes[32];SoundTrace trace{writes,32};s=sound.process_tick(trace);if(!s){failure=s;return s;}
    for(usize i=0;i<trace.count;++i) {
        u8 bytes[12];put64(bytes,writes[i].tick);put16(bytes+8,writes[i].bus_value);bytes[10]=writes[i].channel;bytes[11]=writes[i].kind;
        for(auto b:bytes)stats.sound_hash=(stats.sound_hash^b)*1099511628211ull;
    }
    stats.sound_writes+=trace.count;
    return {};
}
Status SourceLab::draw(Surface out)const {
    if(!out.pixels||out.width!=lab_width||out.height!=lab_height||out.stride<lab_width||out.pixel_capacity<usize(out.stride)*lab_height)return Status::fail("Source lab surface must be 400x260");
    for(unsigned y=0;y<lab_height;++y)copy(out.pixels+usize(y)*out.stride,background_pixels+usize(y)*lab_width,lab_width*sizeof(u32));
    return objects.draw(out);
}
Status SourceLab::validate()const {
    auto s=processes.validate();if(!s)return s;s=objects.validate();if(!s)return s;
    unsigned bullets=0,hulks=0,deaths_count=0;
    for(auto h=processes.active_head;h!=no_process;h=processes.cells[h].next) {
        auto state=processes.cells[h].continuation;const auto& c=contexts[h];
        if(state==bullet_loop||state==bullet_frames)++bullets;
        if((processes.cells[h].id==source_hulk_process_id&&c.object!=no_object)||state==hulk_loop||state==hulk_resume_after_t4||state==hulk_resume_after_way||
           state==hulk_resume_after_retry||state==hulk_resume_after_get_run||
           state==hulk_resume_after_franim||state==death_init)++hulks;
        if(state==death_frames)++deaths_count;
        if(c.object!=no_object&&(state!=death_init)&&(c.object>=object_capacity||!objects.cells[c.object].allocated||objects.cells[c.object].process!=h))return Status::fail("Process/object association mismatch",h);
    }
    if(stats.ticks&&(bullets!=stats.bullets||hulks!=stats.hulks||deaths_count!=stats.death_effects||
                     source_hulk_count!=u16(stats.hulks)))return Status::fail("Native lab actor accounting mismatch");
    return {};
}
Sha256 SourceLab::state_digest()const {
    // Fixed, versioned little-endian serializer; never hashes C++ padding or pointers.
    u8 data[262144];usize used=0;
    auto v=[&](u64 n){put64(data+used,n);used+=8;};
    v(9);v(lifecycle_enabled);v(halted);v(circuit);v(wave);v(early_hulk_difficulty_cleared);v(stats.ticks);v(player_count);v(rng.seed);v(random_calls);
    v(stats.shots);v(stats.wall_impacts);v(stats.hulk_kills);v(stats.hulks);v(source_hulk_count);v(stats.bullets);v(stats.death_effects);v(stats.sound_writes);v(stats.sound_hash);v(stats.pair_tests);
    v(stats.gate_requests);v(stats.door_processes);v(stats.door_frames);v(stats.door_completions);
    for(unsigned i=0;i<3;++i){v(doors.timer[i]);v(doors.view[i]);}for(auto h:door_objects)v(h);
    v(stats.player_hits);v(stats.respawns);v(stats.bat_swings);v(stats.blood_drops);v(stats.ring_kills);
    for(unsigned i=0;i<2;++i) {
        v(input.move[i]);v(input.fire[i]);const auto& p=players[i];
        v(p.legs);v(p.torso);v(p.direction);v(p.shoot_flag);v(p.old_shoot);v(p.reverse_direction);v(p.shoes);v(p.reverse);v(p.have_reverse);v(p.score.packed_bcd);v(p.score.next_extra);v(p.extra_requests);
        v(p.life.dead);v(p.life.immunity);v(p.life.reserves);v(p.life.number);v(p.life.countdown);v(p.life.deaths);v(p.life.respawns);
        v(p.ring);v(p.ring_process);v(p.blink_process);v(p.helper_bounds);v(p.entry_script);v(p.scripted_bits);v(p.gate_requests);v(p.palette_requests);
    }
    v(objects.head);v(objects.free_head);v(objects.count);
    for(const auto& o:objects.cells) {
        v(o.allocated);v(o.listed);v(o.next);v(o.process);v(o.id);v(o.z);v(o.clip);v(o.dma_xy);
        v(o.sprite.art==&player_art?1:o.sprite.art==&hulk_art?2:o.sprite.art==&bullet_art?3:o.sprite.art==&door_art?4:0);
        v(o.sprite.raw_image?u64(o.sprite.raw_image-arena.images)+1:0);
        v(o.sprite.palette_view?u64(o.sprite.palette_view-source_arena_palettes)+1:0);
        v(o.sprite.image);v(u32(o.sprite.palette));v(o.sprite.flags);v(o.sprite.position.x);v(o.sprite.position.y);v(o.sprite.position.vx);v(o.sprite.position.vy);
    }
    v(processes.active_head);v(processes.free_head);v(processes.active_count);v(processes.time_temp);v(processes.overload);v(processes.dispatches);
    for(unsigned i=0;i<process_capacity;++i) {
        const auto& p=processes.cells[i];const auto& c=contexts[i];
        v(door_tasks[i].gate);v(door_tasks[i].duration);v(u8(door_tasks[i].phase));
        v(p.next);v(p.active);v(p.id);v(p.time);v(p.continuation);v(p.depth);
        for(auto r:p.saved)v(r);v(p.saved_a12);
        if(lifecycle_enabled||p.id==source_hulk_process_id||p.id==0x10||p.id==0x11)
            for(unsigned n=0;n<sizeof(p.data);n+=4)v(le32(p.data+n));
        v(c.object);v(c.player);v(c.animation?u64(c.animation-source_animations)+1:0);v(c.frame);v(c.direction);v(c.age);v(c.level);
        v(c.anchor_x);v(c.anchor_y);v(c.vx);v(c.vy);v(u32(c.palette));
        v(c.reference);v(c.bat_player);v(c.repeats);v(c.movement?u64(c.movement-source_movements)+1:0);
    }
    v(sound.ticks);v(sound.suppressed);
    for(const auto& channel:sound.channels) {v(channel.priority);v(channel.period);v(channel.timer);v(channel.cursor);v(channel.start);v(channel.word_count);for(auto word:channel.words)v(word);}
    return sha256({data,used});
}
}
