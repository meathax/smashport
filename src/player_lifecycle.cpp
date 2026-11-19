#include "stv/player_lifecycle.hpp"
namespace stv {
Status source_short_player_hit(const NativeSprite& legs,const NativeSprite& killer,u16 id,bool& hit) {
    hit=false;
    if(!legs.art||!killer.art)return Status::fail("SHORT_PLYR requires original images");
    EditorImage p,k;auto s=legs.art->image(legs.image,p);if(!s)return s;
    s=killer.art->image(killer.image,k);if(!s)return s;
    // SETF 16,1: signed positions/dimensions, then 32-bit ADD/SUB.
    i32 ymin=signed16(u16(legs.position.y>>16)),height=signed16(u16(p.height));
    if(id==0x8500) { // SHORT2, laser: Y-only recheck after ColLists' full box.
        height-=5;
    } else {
        ymin+=3;
        if(id==0x8400){ymin-=5;height-=8;}
        else if(id==0x1700){ymin+=12;height-=21;}
        else height-=13;
    }
    const i32 ky=signed16(u16(killer.position.y>>16));
    if(ky>ymin+height||ky+signed16(u16(k.height))<ymin)return {};
    if(id==0x8500){hit=true;return {};}
    const i32 xmin=signed16(u16(legs.position.x>>16))+4;
    const i32 xmax=xmin+signed16(u16(p.stride))-4;
    const i32 kx=signed16(u16(killer.position.x>>16));
    hit=!(kx>xmax||kx+signed16(u16(k.stride))<xmin);
    return {};
}
PlayerHitEffects source_player_hit(PlayerLife& p,u16 id,bool hit,bool halt,bool attract,unsigned circuit,unsigned wave) {
    PlayerHitEffects e;
    if(!hit||halt||attract||!p.number||(p.immunity&0x1ff)||p.dead)return e;
    // KILL_PLYR does not decrement PLIVES: DOV does, after the death sequence.
    p.dead=id;++p.deaths;e.accepted=true;
    e.clear_early_hulk_difficulty=circuit==0&&wave<3;
    if(p.reserves>=9)p.reserves=9;
    if(!p.reserves){p.countdown=1;e.request_buyin=true;}
    return e;
}
void source_torso_immunity_step(PlayerLife& p) {
    const u16 low=u16(p.immunity&0x1ff);
    if(low)p.immunity=u16(low-1); // low==0 skips the store, retaining high bits.
}
PlayerDeathKind source_player_death_kind(u16 id,bool face_on) {
    if(!id)return PlayerDeathKind::none;
    if(face_on||id==0x1700||id==0x0b00||id==0x0e00)return PlayerDeathKind::chunks;
    id=u16(id&0xfff0);
    if(id==0x9800||id==0x9900||id==0x9600||id==0x8900)return PlayerDeathKind::chunks;
    if(id==0x8500||id==0x8400||id==0x8a40)return PlayerDeathKind::shock;
    return PlayerDeathKind::spin;
}
Status source_player_respawn(PlayerLife& p) {
    if(!p.dead)return Status::fail("DOV requires a dead player");
    if(p.countdown||!p.reserves)return Status::fail("DOV blocked at original buy-in service boundary");
    --p.reserves;++p.respawns;p.immunity=140;p.dead=0;return {};
}
PlayerBounds source_player_bounds(unsigned type,u32 x,u32 y) {
    // WAVES.ASM BNDTBL (normal, helper/rings, blades).
    if(type==1)return {x+u32(0x25+15)*65536,x+u32(0x162-14)*65536,y+u32(0x2c+13)*65536,y+u32(0xf3-4)*65536};
    if(type==2)return {x+u32(0x25+35)*65536,x+u32(0x162-32)*65536,y+u32(0x2c+35)*65536,y+u32(0xf3-20)*65536};
    return {x+0x250000,x+0x1620000,y+0x2c0000,y+0xef0000};
}
const SourceMovement* find_source_movement(const char* label) {
    for(unsigned i=0;i<source_movement_count;++i)if(name_equal(label,source_movements[i].label))return &source_movements[i];
    return nullptr;
}
}
