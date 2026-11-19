#include "stv/source_lab.hpp"
#include "source_states.hpp"
namespace stv {
namespace {
using namespace source_state;
ProcessAction failed(Status s){return ProcessAction::fail(s.message?s.message:"Player lifecycle continuation failed");}
int player_palette(const EditorImages& art,unsigned player) {
    for(unsigned i=0;i<art.palette_count;++i){EditorPalette p;if(art.palette(i,p)&&name_equal(p.name,player?"NUP2":"NUP1"))return int(i);}
    return -1;
}
void set_script_input(SourceLab& lab,unsigned player,u16 bits) {
    lab.players[player].scripted_bits=bits;
    lab.input.move[player]=u8(bits&15);lab.input.fire[player]=u8((bits>>4)&15);
}
}
Status SourceLab::collide_player(ObjectHandle enemy,ObjectHandle object) {
    auto& e=objects.cells[enemy];auto& leg=objects.cells[object];
    if(leg.process>=process_capacity)return Status::fail("Player collision has no native process");
    const unsigned player=contexts[leg.process].player;
    if(player>=player_count||object!=players[player].legs)return Status::fail("Untranslated player class in Hulk collision");
    // HUPL/BAT_PLYR is the enemy callback and precedes PLHU/KILL_PLYR.
    // Even a contact subsequently rejected by SHORT_PLYR can initiate a bat.
    if(processes.cells[e.process].id==source_hulk_process_id) {
        auto status=source_hulk_pdata_write(processes.cells[e.process],
                                            SourceHulkField::batting,player+1);
        if(!status)return status;
    }
    contexts[e.process].bat_player=u16(player+1);
    bool hit=false;auto s=source_short_player_hit(leg.sprite,e.sprite,e.id,hit);if(!s)return s;
    auto& p=players[player];auto effects=source_player_hit(p.life,e.id,hit,halted,false,circuit,wave);
    if(!effects.accepted)return {};
    ++stats.player_hits;
    if(effects.clear_early_hulk_difficulty)early_hulk_difficulty_cleared=true;
    auto score=source_score_add(p.score,0x10,player_count==2?3:1,false);
    if(score.extra_life_requested)++p.extra_requests;
    // CONTINUE's credit/display/countdown process is not implemented. CNTD=1
    // deliberately remains an observable pending service, never fake game-over.
    if(effects.request_buyin){s=sound_command("MCSPK9");if(!s)return s;}
    return {};
}
ProcessAction SourceLab::player_death_step(ProcessHandle ph) {
    auto& c=contexts[ph];auto& p=players[c.player];const auto state=processes.cells[ph].continuation;
    if(state==player_spin_wait) {
        LabContext drop;drop.player=c.player;drop.reference=p.torso;ProcessHandle child;
        auto s=spawn_process(0,blood_wait,drop,child);if(!s)return failed(s);
        s=spawn_process(0,blood_wait_extra,drop,child);if(!s)return failed(s);
        s=sound_command("UGH");if(!s)return failed(s);
        s=script(ph,"ROBO.ASM","TSPN");if(!s)return failed(s);
        return frame(ph,player_spin_frames);
    }
    if(state==player_spin_frames) {
        if(c.frame<c.animation->count)return frame(ph,player_spin_frames);
        objects.cells[p.torso].z=132;
        auto s=script(ph,"ROBO.ASM","DIELST");if(!s)return failed(s);
        return frame(ph,player_fall_frames);
    }
    if(state==player_fall_frames) {
        if(c.frame<c.animation->count)return frame(ph,player_fall_frames);
        return ProcessAction::wait(1,player_all_die);
    }
    if(state==player_all_die)return ProcessAction::wait(1,player_respawn_wait); // ALLDIE/SLP
    if(halted)return ProcessAction::wait(1,state);
    if(p.life.countdown) {
        if(p.life.countdown!=1)return ProcessAction::fail("Buy-in expiry needs untranslated KILLP/GAMEOVER services");
        if(state!=player_buyin_wait) {
            auto& torso=objects.cells[p.torso].sprite.position;torso.x=torso.y=0;
            // SWAPSK: only the connected regular-Hulk target indices exist here.
            if(player_count==2)for(auto h=processes.active_head;h!=no_process;h=processes.cells[h].next)
                if(contexts[h].object<object_capacity&&objects.cells[contexts[h].object].id==0x8600&&contexts[h].player==c.player)
                    contexts[h].player=1-c.player;
        }
        return ProcessAction::wait(1,player_buyin_wait);
    }
    auto s=begin_player_respawn(c.player);if(!s)return failed(s);
    // DOV falls directly into NDD, not TORSO_1: do not decrement DELYDET again.
    processes.cells[ph].continuation=torso_resume;
    return torso_step(ph);
}
Status SourceLab::begin_player_respawn(unsigned player) {
    auto& p=players[player];auto s=source_player_respawn(p.life);if(!s)return s;
    objects.cells[p.torso].z=159;
    LabContext entry;entry.player=player;ProcessHandle child;
    s=spawn_process(0,entry_init,entry,child);if(!s)return s;
    p.shoes=0;p.helper_bounds=false;
    s=sync_player_bounds(player);if(!s)return s;
    auto& legs=objects.cells[p.legs].sprite;auto& torso=objects.cells[p.torso].sprite;
    // DOV writes raw legs positions, not an animation anchor.
    legs.position.x=player?0x1800000:0x80000;legs.position.y=0x850000;
    u32 x,y;s=sprite_anchor(legs,x,y);if(!s)return s;
    torso.flags=u16((legs.flags&0xfff0)|0x802); // GANISAG + OBJON
    s=place_sprite_anchor(torso,x,y);if(!s)return s;
    torso.palette=player_palette(player_art,player);
    if(torso.palette<0)return Status::fail("DOV player palette is unresolved");
    ++stats.respawns;return {};
}
ProcessAction SourceLab::player_legs_death_step(ProcessHandle ph) {
    auto& c=contexts[ph];auto& p=players[c.player];const auto state=processes.cells[ph].continuation;
    if(state==legs_spin_wait) {
        auto s=script(ph,"ROBO.ASM","LSPN");if(!s)return failed(s);
        return frame(ph,legs_spin_frames);
    }
    if(state==legs_spin_frames) {
        if(c.frame<c.animation->count)return frame(ph,legs_spin_frames);
        objects.cells[p.legs].sprite.position.x=0; // PDTH/HG: raw X only
        return ProcessAction::wait(1,legs_dead_wait);
    }
    if(p.life.dead)return ProcessAction::wait(1,legs_dead_wait);
    auto s=script(ph,"ROBO.ASM",c.player?"PWK_LFT":"PWK_RGT");if(!s)return failed(s);
    processes.cells[ph].continuation=legs_top;
    return legs_step(ph);
}
ProcessAction SourceLab::lifecycle_effect_step(ProcessHandle ph) {
    auto& c=contexts[ph];auto& p=players[c.player];auto state=processes.cells[ph].continuation;
    if(state==cheer_wait)return ProcessAction::wait(25,cheer_emit);
    if(state==cheer_emit){auto s=sound_command("CROWD2");return s?ProcessAction::suicide():failed(s);}
    if(state==blood_wait_extra)return ProcessAction::wait(1,blood_wait);
    if(state==blood_wait)return ProcessAction::wait(30,blood_init);
    if(state==blood_init) {
        if(c.reference>=object_capacity||!objects.cells[c.reference].allocated)return ProcessAction::fail("TWOCHNKS lost its player reference");
        const u32 vx=rng.range(0u-0x2000,0x2000,horizontal_count());
        const u32 vy=rng.range(0u-0x2000,0x2000,horizontal_count());
        u32 x,y;auto s=sprite_anchor(objects.cells[c.reference].sprite,x,y);if(!s)return failed(s);
        s=new_sprite(ph,player_art,"BBS1",0x802,0,55,x-0x40000,y+0x2a0000,vx,vy);if(!s)return failed(s);
        s=script(ph,"HULKS.ASM","BLDOTS");if(!s)return failed(s);++stats.blood_drops;
        return frame(ph,blood_frames);
    }
    if(state==blood_frames) {
        if(c.frame<c.animation->count)return frame(ph,blood_frames);
        auto s=objects.erase(c.object);return s?ProcessAction::suicide():failed(s);
    }
    if(state==entry_init) {
        ++p.gate_requests;
        auto gate_status=open_source_gate(c.player+1,10);if(!gate_status)return failed(gate_status);
        p.shoes=0;p.entry_script=true;c.movement=find_source_movement(c.player?"RDRS":"LDRS");c.frame=0;
        if(!c.movement)return ProcessAction::fail("Original respawn movement table is missing");
        LabContext ring;ring.player=c.player;ring.age=190;ProcessHandle child;
        auto s=spawn_process(0,ring_init,ring,child);if(!s)return failed(s);p.ring_process=child;
        state=entry_loop;
    }
    if(state==entry_sleep) {
        c.age=u16(c.age-1);
        if(c.age)return ProcessAction::wait(1,entry_sleep); // ATTSLP itself ignores HALT
        ++c.frame;state=entry_loop;
    }
    if(state==entry_loop) {
        if(halted)return ProcessAction::wait(1,entry_loop);
        if(!c.movement||c.frame>c.movement->count)return ProcessAction::fail("SCRIPTPLAY data cursor is invalid");
        if(c.frame==c.movement->count){p.entry_script=false;return ProcessAction::suicide();}
        const auto& move=c.movement->moves[c.frame];set_script_input(*this,c.player,move.bits);c.age=move.ticks;
        return ProcessAction::wait(1,entry_sleep);
    }
    if(state==ring_init) {
        if(p.ring!=no_object)return ProcessAction::fail("Replacing live rings requires untranslated ring cancellation path");
        p.helper_bounds=true;auto bounds_status=sync_player_bounds(c.player);if(!bounds_status)return failed(bounds_status);
        ++p.palette_requests; // RNG_LIT/RNG_LIT2 not silently emulated.
        u32 x,y;auto s=sprite_anchor(objects.cells[p.torso].sprite,x,y);if(!s)return failed(s);
        s=new_sprite(ph,player_art,"SHLD1",2,0x4103,140,x,y,0,0,player_palette(player_art,c.player));if(!s)return failed(s);
        p.ring=c.object;processes.cells[ph].data[7]=u8(c.player+1);state=ring_loop;
    }
    if(state==ring_loop) {
        if(halted)return ProcessAction::wait(1,ring_loop);
        c.age=u16(c.age-1);
        if(!c.age) {
            auto s=objects.erase(c.object);if(!s)return failed(s);
            if(p.blink_process!=no_process){s=processes.kill(p.blink_process);if(!s)return failed(s);}
            p.ring=no_object;p.ring_process=p.blink_process=no_process;p.helper_bounds=false;
            s=sync_player_bounds(c.player);if(!s)return failed(s);
            return ProcessAction::suicide();
        }
        if(c.age==100) {
            LabContext blink;blink.player=c.player;blink.reference=p.ring;ProcessHandle child;
            auto s=spawn_process(0,ring_blink_init,blink,child);if(!s)return failed(s);p.blink_process=child;
        }
        p.life.immunity=40;
        u32 x,y;auto s=sprite_anchor(objects.cells[p.torso].sprite,x,y);if(!s)return failed(s);
        s=place_sprite_anchor(objects.cells[p.ring].sprite,x,y+0x300000);if(!s)return failed(s);
        return ProcessAction::wait(1,ring_loop);
    }
    if(state==ring_blink_init||state==ring_blink_on||state==ring_blink_off) {
        if(c.reference>=object_capacity||!objects.cells[c.reference].allocated)return ProcessAction::fail("BLINK references deleted ring");
        auto& ring=objects.cells[c.reference].sprite;
        if(state==ring_blink_init){auto s=image(c.reference,"FRNTBK",ring.flags);if(!s)return failed(s);}
        // UTIL OBJOFF/OBJON only alter the low DMA-mode nibble. Collisions remain.
        ring.flags=u16((ring.flags&0xfff0)|(state==ring_blink_on?2:0));
        return ProcessAction::wait(5,state==ring_blink_on?ring_blink_off:ring_blink_on);
    }
    return ProcessAction::fail("Unsupported lifecycle effect continuation");
}
void SourceLab::sync_source_fields(ProcessHandle ph) {
    auto& c=contexts[ph];auto& process=processes.cells[ph];auto* data=process.data;
    // Native Hulk lowering owns HULKS PDATA directly. Never mirror the retired
    // fixture context over that canonical process state after dispatch.
    if(process.id==source_hulk_process_id)return;
    // Addresses are NEVER synthesized from host pointers. The historical
    // reused-cell BLTDIR source value remains an explicit fidelity limitation.
    if(c.object<object_capacity&&objects.cells[c.object].id==0x4103) {
        data[7]=u8(c.player+1);put16(data+14,c.age);
    }
}
}
