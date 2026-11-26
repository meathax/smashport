#include "stv/host/enhance.hpp"
#include "stv/host/gpu_effects.hpp"
#include "stv/gsp/scene.hpp"
#include <algorithm>
#include <cmath>
namespace stv::host {
namespace {
// L8 image descriptors: symbols_l8.json unique relocation-masked matches;
// source HULKS BLOOD/BLDOTS and BOOM2 animation tables. No death inference.
EffectKind classify(uint32_t image, uint16_t oid) {
    if ((image >= 0xfffa9160u && image <= 0xfffa9860u && (image-0xfffa9160u)%0x80u==0))
        return EffectKind::Blood;
    if (image >= 0xfffbf1a0u && image <= 0xfffbf5a0u && (image-0xfffbf1a0u)%0x80u==0)
        return EffectKind::Explosion;
    // First fragments created by HULKS.ADDPCS2 and ADPCS4 (orb death).
    if(!oid && (image==0xfffa8d60u || image==0xfffd2dd0u || image==0xfffd1650u))return EffectKind::FleshBreak;
    if(!oid && (image==0xfffb5ca0u || image==0xfffb44a0u || image==0xfffd30d0u))return EffectKind::MetalBreak;
    if (oid==0x8500 || (oid==0x9500 && image==0xfffb92a0u)) return EffectKind::EnemyShot;
    if (oid >= 0x4201 && oid <= 0x4206) return EffectKind::Projectile;
    return EffectKind::None;
}
// L8 ROBO.EQU body types. Chasers, wall men and tank bases can be neutral.
// Cobra flames, cannon shells, mines, lasers, prizes and scenery have no body.
bool enemy_body(uint16_t oid) {
    if(oid==0x0800 || oid==0x0b00 || oid==0x0e00)return true;
    if((oid&0xe000)!=0x8000)return false;
    switch((oid>>8)&31){
    case 10:return (oid&255)<0x40;
    case 3: case 4: case 6: case 8: case 9: case 11: case 14:
    case 16: case 17: case 18: case 24: case 25:return true;
    default:return false;
    }
}
unsigned body_flags(uint16_t oid){
    const unsigned sub=oid&255;
    switch((oid>>8)&31){
    case 3: case 4: case 5: case 8: case 9: case 11: case 14: case 17:
    case 21: case 22: case 23: case 26:return MachinePart;
    case 10: // Cobra's segmented shell, and the bleeding head/neck assembly (COBRA.ASM).
        return BossPart|(sub==0x30?0u:MachinePart);
    case 24: // MUTOID.ONLIST: tank halves 1/2, torso 3/4, arms 5/6, head 7, helper 8.
        return BossPart|((sub>=3 && sub<=7)?0u:MachinePart);
    case 25: // RBINIT / SKINIT: robot face panels, glass and photon emitters.
        return BossPart|MachinePart;
    default:return 0;
    }
}
uint32_t blend(uint32_t dst, uint32_t src, float alpha, bool add=false) {
    alpha=std::clamp(alpha,0.f,1.f); uint32_t out=0;
    for (unsigned shift : {0u,8u,16u}) {
        float d=float((dst>>shift)&255), s=float((src>>shift)&255);
        auto c=uint32_t(std::clamp(add ? d+s*alpha : d+(s-d)*alpha,0.f,255.f));
        out |= c<<shift;
    }
    return out;
}
}
void EnemyRemainsTracker::observe(std::span<const uint16_t> ram,gsp::ObjectEvent event,uint32_t node,uint64_t cycle,
                                 std::span<const uint8_t> gfx,std::span<const uint32_t> pens,uint32_t partner){
    const auto offset=node-gsp::object_pool_base;
    if(offset>=gsp::object_slots*gsp::object_stride || offset%gsp::object_stride || ram.size()!=65536)return;
    auto& slot=slots_[offset/gsp::object_stride];
    if(event==gsp::ObjectEvent::Insert)slot={true,false,{}};
    auto word=[&](uint32_t a){return ram[(a-0x01000000u)>>4];};
    const uint32_t room=uint32_t(word(0x010eb610))<<16 | word(0x010eb630);
    if(!gsp::live_arena_state(word(0x010865a0),uint32_t(word(0x010dd4e0))|uint32_t(word(0x010dd4f0))<<16) ||
       word(0x010865a0)==0x18){slot={};return;}
    if(slot.contactExpires && cycle>slot.contactExpires){
        slot.blasted=false;slot.blastDx=slot.blastDy=0;slot.contactExpires=0;
    }
    auto centre=[&](uint32_t obj,unsigned pos,unsigned size){
        return float(int16_t(word(obj+pos)))+.5f*word(obj+size);
    };
    if(event==gsp::ObjectEvent::MachineWound){
        const auto oid=word(node+0x150);
        const bool eye=oid==0x9904 || oid==0x9905;
        if(!eye && oid!=0x9805 && oid!=0x9806)return;
        const float w=word(node+0xf0),h=word(node+0x100);
        if(w<1 || h<1 || w>160 || h>160)return;
        const bool left=oid==0x9805 || oid==0x9904;
        float x=centre(node,0x70,0xf0)+(eye?0.f:(left?.32f:-.32f)*w);
        float y=float(int16_t(word(node+0x90)))+h*(eye?.5f:.28f);
        const unsigned pal=word(node+0x110);
        impacts_.push_back({x,y,left?-1.f:1.f,.24f,oid,BrokenJoint,room,(pal&0xf00)|((pal>>8)&0xc0)});
        return;
    }
    if(event==gsp::ObjectEvent::SmartBombStart){
        const auto player=word(node+0x150);
        if(player==0x4101 || player==0x4102)
            smartBombs_[player-0x4101]={centre(node,0x70,0xf0),centre(node,0x90,0x100),room,true};
        return;
    }
    if(event==gsp::ObjectEvent::SmartBombVictim){
        slot.blasted=true;
        slot.contactExpires=0;
        slot.blastDx=slot.blastDy=0;
        // SBOMB carries the scoring player's P1DATA/P2DATA in A9.
        if(partner==0x01086220 || partner==0x010863e0){
            const auto& src=smartBombs_[partner==0x010863e0];
            if(src.valid && src.room==room){
                slot.blastDx=centre(node,0x70,0xf0)-src.x;
                slot.blastDy=centre(node,0x90,0x100)-src.y;
            }
        }
        return;
    }
    if(event==gsp::ObjectEvent::Blast){slot.blasted=true;slot.contactExpires=0;return;}
    if(event==gsp::ObjectEvent::Contact){
        auto other=partner-gsp::object_pool_base;
        if(!slot.inserted || other>=gsp::object_slots*gsp::object_stride || other%gsp::object_stride)return;
        const auto weapon=word(partner+0x150);
        if((weapon>=0x4201 && weapon<=0x4206) || weapon==0x9500 || weapon==0x9600 || weapon==0x4103 || weapon==0x4104){
            slot.blasted=weapon==0x4204 || weapon==0x4206 || weapon==0x9600;
            // An accepted hit can leave a boss alive. Only carry it into a
            // contemporaneous death/retag, unless the retail blast hook confirms it.
            slot.contactExpires=slot.blasted?cycle+185000:0;
            slot.blastDx=slot.blasted?centre(node,0x70,0xf0)-centre(partner,0x70,0xf0):0;
            slot.blastDy=slot.blasted?centre(node,0x90,0x100)-centre(partner,0x90,0x100):0;
        }
        if(weapon>=0x4201 && weapon<=0x4206){
            const auto oid=word(node+0x150);
            if(enemy_body(oid)){
                auto longword=[&](uint32_t a){return uint32_t(word(a))|(uint32_t(word(a+16))<<16);};
                const float x=float(int16_t(word(node+0x70))),y=float(int16_t(word(node+0x90)));
                const float w=float(word(node+0xf0)),h=float(word(node+0x100));
                const float sx=float(int16_t(word(partner+0x70))),sy=float(int16_t(word(partner+0x90)));
                const float px=std::clamp(sx,x,x+w-1),py=std::clamp(sy,y,y+h-1);
                unsigned material=body_flags(oid);
                // A fleshy boss component can also carry armor or a weapon.
                // Require skin-coloured ink at the impact before adding blood.
                if((material&BossPart) && !(material&MachinePart) && !gfx.empty() && pens.size()==4096){
                    const unsigned flags=word(node+0xb0),pal=word(node+0x110);
                    uint32_t sag=longword(node+0xd0);
                    if(flags&16)sag-=uint32_t(w-1)*8;
                    const int64_t first=(sag>=0x02000000u?sag-0x02000000u:sag)>>3;
                    const int pitch=(int16_t(word(node+0xc0))+((flags&16)?-int(w):int(w))+3)&~3;
                    const unsigned bank=(pal&0xf00)|((pal>>8)&0xc0);
                    bool skin=false;
                    for(int j=-1;j<=1;++j)for(int i=-1;i<=1;++i){
                        int tx=int(px-x)+i,ty=int(py-y)+j;
                        if(tx<0 || ty<0 || tx>=int(w) || ty>=int(h))continue;
                        if(flags&16)tx=int(w)-1-tx;
                        const int64_t at=first+int64_t(pitch)*ty+tx;
                        if(at<0 || uint64_t(at)>=gfx.size() || !gfx[size_t(at)])continue;
                        const auto rgb=pens[bank|(gfx[size_t(at)]&63)];
                        const unsigned r=(rgb>>16)&255,g=(rgb>>8)&255,b=rgb&255;
                        skin|=r>g+15 && g>=40 && b>=20 && g*100>=r*35 && b*100>=r*20 && b*100>=g*60;
                    }
                    if(!skin)material|=MachinePart;
                }
                impacts_.push_back({px,py,
                    float(int32_t(longword(partner+0x20)))/65536.f,
                    float(int32_t(longword(partner+0x40)))/65536.f,oid,material,room,0,weapon});
            }
        }
        return;
    }
    if(!slot.inserted && event==gsp::ObjectEvent::Retag)return;
    const auto oid=word(node+0x150), w=word(node+0xf0), h=word(node+0x100);
    if(event==gsp::ObjectEvent::Retag && slot.blasted && enemy_body(oid) && !enemy_body(uint16_t(partner)))
        slot.contactExpires=0; // retain the killing hit through the death animation
    if(enemy_body(oid) && w && h && w<=512 && h<=512){
        const float x=float(int16_t(word(node+0x70)))+w*.5f;
        const float y=float(int16_t(word(node+0x90)))+h*.85f;
        const auto flagsForPart=body_flags(oid);
        slot.body={x,y,std::clamp(std::sqrt(float(w)*h)/18.f,.75f,2.8f),oid,
                   uint32_t(cycle)^(node*2654435761u),room};
        slot.body.flags=flagsForPart;
        // NDSP1.GSAGOF prepares a flip-adjusted bit address and DMA row offset.
        // Sample the actor's actual upper and lower sprite colours while its palette is live.
        const auto flags=word(node+0xb0),pal=word(node+0x110);
        uint32_t sag=word(node+0xd0)|(uint32_t(word(node+0xe0))<<16);
        const bool flipped=(flags&0x10)!=0;
        if(flipped)sag-=(w-1)*8;
        const int64_t first=(sag>=0x02000000u?sag-0x02000000u:sag)>>3;
        const int pitch=(int16_t(word(node+0xc0))+(flipped?-int(w):int(w))+3)&~3;
        const unsigned bank=(pal&0xf00)|((pal>>8)&0xc0);
        if(!gfx.empty() && pens.size()==4096){
            for(unsigned half=0;half<2;++half){
                std::array<unsigned,64> weight{};
                unsigned best=0;
                for(int j=0;j<5;++j)for(int i=0;i<7;++i){
                    const int64_t address=first+int64_t(pitch)*((int(half)*10+2*j+1)*h/20)+(2*i+1)*w/14;
                    if(address<0 || uint64_t(address)>=gfx.size())continue;
                    unsigned pen=gfx[size_t(address)]&63;
                    if(!pen)continue;
                    auto rgb=pens[bank|pen];
                    unsigned light=std::max({(rgb>>16)&255,(rgb>>8)&255,rgb&255});
                    weight[pen]+=light>50?4:1;
                    if(weight[pen]>weight[best])best=pen;
                }
                if(best)(half?slot.body.lower:slot.body.upper)=pens[bank|best];
            }
        }
    }
    if(event==gsp::ObjectEvent::Retire){
        const uint32_t image=word(node+0x130)|(uint32_t(word(node+0x140))<<16);
        if(slot.blasted || classify(image,oid)==EffectKind::Explosion)slot.body.flags|=BlastDeath;
        slot.body.blastDx=slot.blastDx;slot.body.blastDy=slot.blastDy;
        if(slot.body.oid && slot.body.room==room)retired_.push_back(slot.body);
        slot={};
    }
}
std::vector<EnemyRemains> EnemyRemainsTracker::take(uint32_t room){
    std::vector<EnemyRemains> result;
    result.swap(retired_);
    std::erase_if(result,[&](const EnemyRemains& body){return body.room!=room;});
    return result;
}
std::vector<EnemyImpact> EnemyRemainsTracker::take_impacts(uint32_t room){
    std::vector<EnemyImpact> result;result.swap(impacts_);
    std::erase_if(result,[&](const auto& hit){return hit.room!=room;});return result;
}
EffectScene observe_effect_scene(std::span<const uint16_t> ram, uint64_t frame, int width, int height, bool observe_objects) {
    EffectScene s; s.frame=frame; s.width=width; s.height=height;
    if (ram.size()!=65536 || width<32 || width>1024 || height<32 || height>512) return s;
    auto word=[&](uint32_t a) { return ram[(a-0x01000000u)/16]; };
    auto longword=[&](uint32_t a) { return uint32_t(word(a)) | uint32_t(word(a+16))<<16; };
    const auto gameState=word(0x010865a0u);
    s.roomIntro=gameState==0x18;
    // Game over (50h) keeps the live room and its actors on screen.
    if (!gsp::live_arena_state(gameState,longword(0x010dd4e0u))) return s;
    s.room=uint32_t(word(0x010eb610u))<<16 | word(0x010eb630u);
    s.camera_x=int16_t(word(0x01000280u)); s.camera_y=int16_t(word(0x01000290u));
    s.in_arena=true;
    if(!observe_objects)return s;
    auto byte=[&](uint32_t a){return (word(a)>>(a&8))&255;};
    for(unsigned player=0;player<s.players.size();++player){
        const auto proc=longword(0x010de2b0+player*32);
        if((proc&15) || proc<0x01000000 || proc>0x010ffde0 ||
           word(proc+0x20)!=247+player || byte(proc+0x1d8)!=player+1)continue;
        // ROBO.KILL_PLYR sets the unaligned 16-bit DEAD field on the torso process.
        s.players[player].process=proc;
        s.players[player].dead=(byte(proc+0x208) | byte(proc+0x210))!=0;
    }
    auto& geo=s.geometry;
    // WAVES.DO_BND stores four signed 16.16 limits in the active torso process.
    for(unsigned owner=0;owner<2;owner++){
        auto proc=longword(0x010de2b0+owner*32);
        if((proc&15) || proc<0x01000000 || proc>0x010ffd00 || byte(proc+0x1d8)!=owner+1)continue;
        auto bound=[&](unsigned off){return float(int32_t(longword(proc+off)))/65536.f;};
        geo.left=bound(0x280)-s.camera_x;geo.right=bound(0x2a0)-s.camera_x;
        geo.top=bound(0x240)-s.camera_y;geo.bottom=bound(0x260)-s.camera_y;
        if(geo.right>geo.left+32 && geo.bottom>geo.top+32)break;
        geo={};
    }
    // L8 SHOTS.YESWALL and DRINF. Left, right and top gates each own one byte.
    geo.openGates=(byte(0x010ee9b8)!=0) | ((byte(0x010ee9c0)!=0)<<1) | ((byte(0x010ee9c8)!=0)<<2);
    float margin=byte(0x010f2da8)?45.f:0.f;
    geo.gateLeft=(width==547?237.f:165.f)-margin;geo.gateRight=geo.gateLeft+72+2*margin;
    auto origin=longword(0x010ee080); // WAVES.SET_WAVE / WAVEYX
    float ox=int16_t(origin)-s.camera_x,oy=int16_t(origin>>16)-s.camera_y;
    // The normal room envelope stays fixed through death, respawn and temporary
    // power-up movement limits. Keep deposits below the rear wall and above the front lip.
    const float left=width==547?33.f:29.f,right=width==547?513.f:372.f;
    geo.floor={ox+left,oy+46,right-left,195};
    // NDSP1.BAKLST (OFIXED, after OBJLST/OFREE/ENDOFREE) links the live
    // ADDBLOCK room tiles. Recessed art is identified by its L8 image header,
    // so masks follow the tiles through room scrolls and mirrored copies.
    {
        struct RecessTile { uint32_t image; uint16_t w,h,x,y,rw,rh; };
        // Sub-rectangles in unflipped image coordinates, authored from the
        // 48-room widescreen audit.
        static constexpr RecessTile tiles[]={
            // Railed stair alcoves and the window bay (circuit 2). Circuit-1 stairwells
            // are walkways and keep their gore.
            {0xff00f880u,124,69,0,0,124,69},{0xff01d980u,96,43,0,0,95,43},{0xff01d9c0u,96,43,0,4,95,39},{0xff01de40u,12,85,0,0,12,81},
            {0xff01f5a0u,124,69,0,0,124,69},{0xff01ff60u,124,69,0,0,124,69},{0xff00f7c0u,132,8,8,0,124,8},
            {0xff01f4e0u,132,8,8,0,124,8},{0xff01ffa0u,132,8,9,0,123,8}};
        constexpr uint32_t base=0x010003a0u,stride=0x1f0u;
        std::array<bool,420> visited{};
        // High-priority bay tiles are inserted on OBJLST; ADDBLOCK still stores
        // their BAKBITS pointer in OPLINK, which separates them from sprites.
        for(const uint32_t head:{0x010000a0u,0x01000040u})
        for(uint32_t tile=longword(head);tile;tile=longword(tile)){
            if(tile<base || (tile-base)%stride || (tile-base)/stride>=visited.size() || visited[(tile-base)/stride])break;
            visited[(tile-base)/stride]=true;
            if(longword(tile+0x160)-0x010dadd0u>=10000u)continue;
            const uint32_t image=longword(tile+0x130);
            const unsigned w=word(tile+0xf0),h=word(tile+0x100),flags=word(tile+0xb0);
            for(const auto& r:tiles){
                if(r.image!=image || r.w!=w || r.h!=h)continue;
                // OFLAGS bits 4/5 are the DMA horizontal/vertical flips set by ADDBLOCK.
                const float rx=float(int16_t(word(tile+0x70))-s.camera_x)+float((flags&0x10)?w-r.x-r.rw:r.x);
                const float ry=float(int16_t(word(tile+0x90))-s.camera_y)+float((flags&0x20)?h-r.y-r.rh:r.y);
                if(rx<width && ry<height && rx+r.rw>0 && ry+r.rh>0 && geo.recesses.size()<32)
                    geo.recesses.push_back({rx,ry,float(r.rw),float(r.rh)});
            }
        }
    }
    s.studio.mountX=ox+width*.5f-40;s.studio.mountY=oy+31;
    s.studio.aimX=ox+width*.5f;s.studio.aimY=oy+128;
    s.studio.boss=s.room==10 || s.room==0x10007 || s.room==0x20007 || s.room==0x20015;
    std::array<bool,420> seen{};
    auto node=longword(0x01000040u);
    float prizeX=0,prizeY=0;bool prize=false;
    float playerFeet[2]{-10000,-10000};
    while(node) {
        constexpr uint32_t base=0x010003a0u, stride=0x1f0u;
        if(node<base || (node-base)%stride || (node-base)/stride>=seen.size()) return s;
        auto index=(node-base)/stride;
        if(seen[index] || !(word(node+0xb0)&0x8000)) return s;
        seen[index]=true;
        auto image=longword(node+0x130); auto kind=classify(image,word(node+0x150));
        auto weapon=word(node+0x150);
        const auto process=longword(node+0x160);
        const bool bulletProcess=process>=0x01086790 && (process-0x01086790)%0x5a0==0 &&
            (process-0x01086790)/0x5a0<240 && word(process+0x20)==286;
        const bool grenade=!weapon && bulletProcess && longword(process+0xc0)==node;
        if(grenade){kind=EffectKind::Projectile;weapon=0x4204;}
        if(kind!=EffectKind::None) {
            // NDSP1 DCLIP subtracts packed WORLDTL; object coordinates are signed.
            float x=float(int16_t(word(node+0x70))-s.camera_x);
            float y=float(int16_t(word(node+0x90))-s.camera_y);
            if(x>=0 && x<width && y>=24 && y<height-4) {
                EffectObject object{node,image,kind,x,y,float(int32_t(longword(node+0x20)))/65536.f,float(int32_t(longword(node+0x40)))/65536.f,weapon};
                if(grenade){
                    constexpr int dx[]={0,0,1,1,1,0,-1,-1,-1},dy[]={0,-1,-1,0,1,1,1,0,-1};
                    const unsigned dir=word(process+0x100);
                    if(dir<=8){object.vx=float(dx[dir]);object.vy=float(dy[dir]);}
                }
                object.w=float(word(node+0xf0));object.h=float(word(node+0x100));
                s.objects.push_back(object);
            }
        }
        // DISP OSIZE supplies a 2D footprint. Proxy height is an artistic choice.
        unsigned type=(word(node+0x150)>>8)&0x3f;
        bool actor=type==1||type==3||type==4||type==6||type==8||type==9||type==10||
                   type==13||type==14||type==16||type==17||type==18||
                   type==21||type==24||type==25;
        float w=float(word(node+0xf0)),h=float(word(node+0x100));
        float x=float(int16_t(word(node+0x70))-s.camera_x),y=float(int16_t(word(node+0x90))-s.camera_y);
        if(actor && s.blockers.size()<64 && w>=2 && w<=96 && h>=2 && h<=96 && x>-96 && x<width && y>0 && y<height) {
            float top=std::clamp(h*.7f,3.f,32.f);
            s.blockers.push_back({{x+w*.2f,y+h-w*.25f,0},{x+w*.8f,y+h,top},0,false});
        }
        // ROBO.EQU OID class/type; material is presentation-only.
        auto oid=word(node+0x150);
        unsigned player=oid==0x4101?1:oid==0x4102?2:0;
        if(player && w>0 && h>0 && w<=160 && h<=160){
            auto& pose=s.players[player-1];
            if(pose.process && longword(node+0x160)==pose.process){
                pose.visible=true;pose.x=x+w*.5f;pose.y=y+h;pose.height=h*.55f;
            }
        }
        // ROBO.EQU TYPTEXT includes life digits, timers, score and message windows.
        // The two portrait/lives backplates are foreground arena art (OID 6).
        if(type==0x1a && w>0 && h>0 && x+w>0 && x<width && y+h>0 && y<height)
            geo.solids.push_back({x,y,w,h});
        bool lifePanel=oid==6 && y==0 && w==52 && h==56;
        if(type==12 && x>=0 && x<width && y>30 && y<height-12){prize=true;prizeX=x+w*.5f;prizeY=y+h*.5f;}
        if(oid==1){ // ROBO.SUBARW, the retail exit arrow
            s.studio.exit=true;s.studio.doorX=x+w*.5f;s.studio.doorY=y+h*.5f;
        }
        if(player==1 || (player==2 && playerFeet[0]<0)){s.studio.aimX=x+w*.5f;s.studio.aimY=y+h;}
        bool ui=(oid&0x1f00)==0x0700 || oid==1 || lifePanel;
        const bool tvMonitor=oid==0x070f || oid==0x0710; // TVMNTR map and announcer parts
        const bool overhead=oid==2 || oid==3; // SCROLL_WAVE changes SUBUP to SUBUP2 while moving it
        // SCORE.ASM assigns these subtypes to score, lives, join text and countdowns.
        const bool scoreText=oid==0x0702 || oid==0x0703 || oid==0x0704 ||
                             (oid>=0x070b && oid<=0x070e);
        const auto hudPal=word(node+0x110);
        const int hudBank=(hudPal&0xf00)|((hudPal>>8)&0xc0);
        // Retail L8 SHAD / SHAD2 use pen 1 and share MUTT with the tank.
        // The image owner matters: the OID and palette alone include wreckage.
        // GSAGOF rounds OSIZEX up to a four-pixel DMA row (13 -> 16, 59 -> 60).
        if(oid==4 && ((image==0xfffe0120u && w==16 && h==71) ||
                      (image==0xfffe01a0u && w==60 && h==6)))
            s.tankShadows.push_back({int(x),int(y),int(w),int(h),uint16_t(hudBank|1)});
        if(lifePanel && x<width && x+w>0){
            // The 52x56 backplate is drawn over the room wall: its outer five
            // columns on the screen edge and six top rows are rim wall art that
            // must light like the neighbouring wall. Mark the inset and the score
            // bezel as room surfaces so they receive light and airborne effects.
            const bool left=x+w*.5f<width*.5f;
            s.hud.push_back({{left?x+5:x,y+6,w-5,h-6},hudBank,true,true});
            s.hud.push_back({{left?x+25:x-77,46,106,33},hudBank,true,true});
        }else if((scoreText || overhead || (ui && (tvMonitor || y+h<=56 || oid==1))) &&
                 w>0 && h>0 && x<width && x+w>0 && y<height && y+h>0){
            s.hud.push_back({{x,y,w,h},hudBank,false,scoreText});
        }
        // Only physical objects occlude light. Neutral animation frames include
        // blood, muzzle sprites and explosions, whose bounds are not bodies.
        bool physical=actor || type==11 || type==12 || type==23 || type==26;
        if(physical && !ui && oid!=6 && w>0 && h>0 && w<=160 && h<=160 && x+w>0 && x<width && y+h>25 && y<height){
            unsigned pal=word(node+0x110);
            float bank=float((pal&0xf00)|((pal>>8)&0xc0));
            const unsigned material=body_flags(oid);
            const unsigned response=type==26 ? 4 : player ? 1 : (type==12 || type==13) ? 3 :
                                    type==21 ? 2 :
                                    (material&MachinePart) ? 2 : (material&BossPart) ? 5 : 1;
            s.casters.push_back({x,y,w,h,y+h,bank,float(player),float(response)});
            if(player)playerFeet[player-1]=std::max(playerFeet[player-1],y+h);
        }
        if (((oid&0xc000)==0x8000 || enemy_body(oid)) && w>=2 && w<=160 && h>=2 && h<=160)
            {
                const unsigned palette=word(node+0x110);
                s.targets.push_back({node,x,y,w,h,(body_flags(oid)&MachinePart)!=0,oid,int((palette&0xf00)|((palette>>8)&0xc0))});
                if (body_flags(oid)&BossPart) {
                    ++s.studio.bossParts;
                    s.studio.bossX += x+w*.5f;
                    s.studio.bossY += y+h*.5f;
                }
            }
        node=longword(node);
    }
    if (s.studio.bossParts) {
        s.studio.bossX /= float(s.studio.bossParts);
        s.studio.bossY /= float(s.studio.bossParts);
    }
    if(s.studio.exit){s.studio.aimX=prize?prizeX:s.studio.doorX;s.studio.aimY=prize?prizeY:s.studio.doorY;}
    for(auto& caster:s.casters)if(caster.player)caster.foot=playerFeet[int(caster.player)-1];
    s.valid=true; return s;
}
size_t EnhanceEffects::voxel_count() const { return size_t(std::count_if(particles_.begin(),particles_.end(),[](const auto& p){return p.kind==Kind::Voxel;})); }
void EnhanceEffects::reset_weapon_fx() { packet_.weaponFx={}; }
void EnhanceEffects::reset() {
    bossSmokeDeath_=false;
    packet_.remains.clear();packet_.spawns.clear();packet_.reset=true;packet_.dt=0;
    packet_.boxes.clear();packet_.casters.clear();packet_.hud.clear();packet_.lamps={};packet_.cinema={};
    reset_weapon_fx();++packet_.serial;particles_.clear();contacts_.clear();
    gpu_error_.clear();previous_={};primed_=false;weaponFxObjectContinuity_=false;
    random_=0x51a7b10du;
}
float EnhanceEffects::random() { random_^=random_<<13; random_^=random_>>17; random_^=random_<<5; return float(random_&65535)/65535.f; }
bool EnhanceEffects::enabled(Kind k,const PcSettings& s) const {
    switch(k) { case Kind::Spark:return s.particles; case Kind::Blood:return s.blood;
    case Kind::Debris:return s.debris; case Kind::Smoke:return s.smoke; case Kind::Fire:return s.fire; case Kind::Voxel:return s.voxels; } return false;
}
void EnhanceEffects::emit(Kind kind,float x,float y,int count,bool inert) {
    if(kind==Kind::Blood) count *= 2;
    if(resident_) {if(packet_.spawns.size()<256)packet_.spawns.push_back({x,y,float(kind),float(count),0,0,float(inert)});return;}
    if(kind==Kind::Voxel) count=std::min(count,64-int(std::count_if(particles_.begin(),particles_.end(),[](const auto& p){return p.kind==Kind::Voxel;})));
    for(int i=0;i<count && particles_.size()<Capacity;++i) {
        float a=random()*6.2831853f, speed=12+random()*48;
        Particle p{kind,x,y,2,std::cos(a)*speed,std::sin(a)*speed,25+random()*60,0,0,1,0};
        switch(kind) {
        case Kind::Spark:p.z=0;p.life=.12f+random()*.18f;p.size=.28f;p.color=0xffd78a;break;
        case Kind::Blood:p.life=4+random()*3;p.color=0xc01722;p.size=.3f+random()*.35f;break;
        case Kind::Voxel:p.life=4+random()*3;p.color=random()>.5f?0x8e1020:0xd33542;p.size=.28f+random()*.42f;break;
        case Kind::Debris:p.life=3+random()*3;p.color=random()>.5f?0xa30e20:0xe04751;p.size=.3f+random()*.45f;break;
        case Kind::Smoke:p.life=2.8f+random()*1.8f;p.color=0x77716b;p.size=3+random()*2;p.vx*=.28f;p.vy*=.28f;p.vz=7;break;
        case Kind::Fire:p.life=1.3f+random()*1.3f;p.color=0xff7620;p.size=1.3f+random()*1.4f;p.vx*=.12f;p.vy*=.12f;p.vz=14;break;
        }
        if(inert)p.color=random()>.5f?0x6b655c:0x393b40;
        p.total=p.life;particles_.push_back(p);
    }
}
void EnhanceEffects::advance_weapon_fx(const EffectScene& scene,float dt,const PcSettings& settings) {
    auto& fx=packet_.weaponFx;
    auto& director=fx.director;
    const float elapsed=std::clamp(dt,0.f,.25f);
    director.camera_trauma=std::max(0.f,director.camera_trauma-elapsed*1.15f);
    director.zoom_punch=std::max(0.f,director.zoom_punch-elapsed*.12f);
    director.hit_flash=std::clamp(director.hit_flash*std::exp(-elapsed*11.f),0.f,1.f);
    director.vignette=std::clamp(director.vignette*std::exp(-elapsed*3.2f),0.f,1.f);
    director.chromatic_split=std::clamp(director.chromatic_split*std::exp(-elapsed*5.f),0.f,1.f);
    director.tier=director.camera_trauma>=.62f?ImpactTier::Large:
        director.camera_trauma>=.27f?ImpactTier::Medium:
        director.camera_trauma>0?ImpactTier::Small:ImpactTier::None;
    if(!settings.combat_feedback){
        reset_weapon_fx();
        return;
    }
    (void)scene;
}
void EnhanceEffects::queue_weapon_fx(WeaponFxKind kind,WeaponFamily family,
                                     float x,float y,float dx,float dy,float strength) {
    auto& fx=packet_.weaponFx;
    const WeaponFxEvent event{x,y,dx,dy,std::clamp(strength,0.f,2.f),family,kind};
    if(fx.event_count<fx.events.size()){fx.events[fx.event_count++]=event;return;}
    if(kind==WeaponFxKind::Wake)return;
    for(unsigned i=0;i<fx.event_count;++i){
        if(fx.events[i].kind!=WeaponFxKind::Wake)continue;
        fx.events[i]=event;
        return;
    }
}
bool EnhanceEffects::has_weapon_fx(WeaponFxKind kind,WeaponFamily family,float x,float y) const {
    const auto& fx=packet_.weaponFx;
    for(size_t i=0;i<fx.event_count;++i){
        const auto& event=fx.events[i];
        if(event.kind==kind && event.family==family &&
           std::abs(event.x-x)<=2.f && std::abs(event.y-y)<=2.f)return true;
    }
    return false;
}
void EnhanceEffects::impact(ImpactTier tier, bool camera_motion) {
    auto& director=packet_.weaponFx.director;
    float trauma=.18f,zoom=.003f,flash=.16f,vignette=.10f,split=.08f;
    if(tier==ImpactTier::Medium){trauma=.45f;zoom=.007f;flash=.34f;vignette=.22f;split=.26f;}
    if(tier==ImpactTier::Large){trauma=.82f;zoom=.014f;flash=.72f;vignette=.44f;split=.62f;}
    if(camera_motion){
        director.camera_trauma=std::max(director.camera_trauma,trauma);
        director.zoom_punch=std::max(director.zoom_punch,zoom);
    }
    director.hit_flash=std::max(director.hit_flash,flash);
    director.vignette=std::max(director.vignette,vignette);
    director.chromatic_split=std::max(director.chromatic_split,split);
    director.tier=static_cast<uint8_t>(tier)>static_cast<uint8_t>(director.tier)?tier:director.tier;
}
void EnhanceEffects::advance(const EffectScene& scene,const PcSettings& settings,float audioPulse) {
    packet_.remains=scene.remains;
    packet_.in_arena=scene.in_arena || scene.valid;
    packet_.roomIntro=scene.roomIntro;packet_.room=scene.room;packet_.frame=scene.frame;
    packet_.camera_x=scene.camera_x;packet_.camera_y=scene.camera_y;
    // Room age is cinema timing, independent of the effect simulation: it must
    // keep counting when Enhancements is off (reset() runs every frame then).
    // It starts with the room (including its intro: the boss-intro cue fades
    // over the first 42 frames). A backward frame is a new power-on.
    if(!roomClock_ || scene.room!=roomClockRoom_ || scene.frame<roomClockFrame_){
        roomStart_=scene.frame;stepDistance_.fill(0);nextFoot_.fill(0);
    }
    roomClock_=true;roomClockRoom_=scene.room;roomClockFrame_=scene.frame;
    packet_.roomAge=scene.frame-roomStart_;packet_.studio=scene.studio;
    packet_.foreground=scene.targets;
    packet_.lightSources.clear();
    for(const auto& obj:scene.objects)
        if((obj.kind==EffectKind::Projectile || obj.kind==EffectKind::EnemyShot) &&
           obj.w>0 && obj.h>0 && obj.w<=64 && obj.h<=64)
            packet_.lightSources.push_back({obj.x,obj.y,obj.w,obj.h});
    packet_.tankShadows=scene.tankShadows;
    packet_.lamps=scene.lamps;packet_.hud=scene.hud;packet_.geometry=scene.geometry;
    packet_.cinema=roomCinema_.observe({
        scene.frame,scene.room,unsigned(std::min<std::uint64_t>(packet_.roomAge,1000000)),
        scene.studio.bossParts,scene.studio.bossX,scene.studio.bossY,
        scene.in_arena,scene.roomIntro,scene.studio.boss,
        std::any_of(scene.remains.begin(),scene.remains.end(),[](const auto& body){return (body.flags&BossPart)!=0;})
    },unsigned(std::max(settings.room_cinema,0)),audioPulse);
    const auto cinema=packet_.cinema;
    if((settings.hardware_rt || settings.persistent_gore || settings.fluid_smoke ||
        settings.weapon_impacts || settings.contact_shadows) && scene.valid)
        packet_.casters=scene.casters;
    else
        packet_.casters.clear();
    packet_.dxrLights.clear();
    if(settings.hardware_rt){
        // Boss-room hostile shots carry encounter-colored spill. This is a room
        // treatment, not a claim that every hostile shot belongs to the boss.
        const bool bossRoom=scene.room==10 || scene.room==0x10007 ||
            scene.room==0x20007 || scene.room==0x20015;
        const auto tint=boss_attack_tint(scene.room);
        for(const auto& obj:scene.objects){
            const bool blast=obj.kind==EffectKind::Explosion;
            const bool shot=bossRoom && scene.in_arena && obj.kind==EffectKind::EnemyShot;
            if(!blast && !shot)continue;
            // Smooth, frame-deterministic breathing; no accumulated light energy
            // or render-rate-dependent flashes on stationary laser objects.
            const float flick=.9f+.1f*std::sin(float(scene.frame%4096)*.12f+float(obj.slot%31));
            // Explosion light follows the fireball: a short yellow-white flash
            // cooling through orange to ember red, dimming as it ages.
            static std::array<std::uint64_t,256> blastStart{},blastSeen{};
            float age=0;
            if(blast){
                const unsigned k=obj.slot&255;
                if(blastSeen[k]==0 || scene.frame<blastSeen[k] || scene.frame-blastSeen[k]>2)
                    blastStart[k]=scene.frame;
                blastSeen[k]=scene.frame;
                age=float(std::min<std::uint64_t>(scene.frame-blastStart[k],60));
            }
            const float t1=std::min(age/8.f,1.f),t2=std::clamp((age-8.f)/22.f,0.f,1.f);
            const std::array<float,3> hot{1,.86f,.55f},mid{1,.48f,.13f},ember{.85f,.2f,.05f};
            std::array<float,3> fire{};
            for(int c=0;c<3;++c)fire[c]=hot[c]+(mid[c]-hot[c])*t1+(ember[c]-mid[c])*t2;
            const auto color=bossRoom&&!blast?tint:fire;
            const float decay=1.f-.6f*std::min(age/36.f,1.f);
            const RoomLamp lamp{obj.x,obj.y,blast?26.f:14.f,blast?140.f:72.f,
                color[0],color[1],color[2],(blast?1.8f*decay:.9f)*flick};
            if(packet_.dxrLights.size()<MaxGameplayLights){
                packet_.dxrLights.push_back(lamp);
                continue;
            }
            const auto weakest=std::min_element(packet_.dxrLights.begin(),packet_.dxrLights.end(),
                [](const auto& a,const auto& b){return a.power*a.radius<b.power*b.radius;});
            if(weakest!=packet_.dxrLights.end() && lamp.power*lamp.radius>weakest->power*weakest->radius)
                *weakest=lamp;
        }
        if(bossRoom){
            // A barrage must read as colored pools, not twelve overlapping
            // full-power explosions bleaching the floor and persistent debris.
            float power=0;
            for(const auto& lamp:packet_.dxrLights)power+=lamp.power;
            const float exposure=std::min(1.f,5.f/std::max(power,.001f));
            for(auto& lamp:packet_.dxrLights)lamp.power*=exposure;
        }
    }
    packet_.spawns.clear();packet_.dt=0;packet_.reset=false;++packet_.serial;
    packet_.weaponFx.event_count=0;
    packet_.boxes=scene.blockers;
    if(!settings.enhance || !scene.valid || scene.roomIntro) {
        reset();
        packet_.cinema=cinema;
        if((settings.hardware_rt || settings.contact_shadows) && scene.valid)
            packet_.casters=scene.casters;
        packet_.hud=scene.hud;
        if(settings.enhance && scene.in_arena)packet_.remains=scene.remains;
        return;
    }
    std::array<EffectSpawn,2> deaths{};
    unsigned deathCount=0;
    if(primed_ && scene.room==previous_.room && scene.frame>previous_.frame &&
       scene.frame-previous_.frame<=8 && (settings.blood || settings.persistent_gore)){
        for(unsigned player=0;player<scene.players.size();++player){
            const auto& now=scene.players[player];const auto& old=previous_.players[player];
            if(!now.process || now.process!=old.process || old.dead || !now.dead)continue;
            // Prefer the last living torso. The death animation can already have removed it.
            if(!old.visible && !now.visible)continue;
            const auto& pose=old.visible?old:now;
            float x=pose.x+(old.visible?previous_.camera_x-scene.camera_x:0);
            float y=pose.y+(old.visible?previous_.camera_y-scene.camera_y:0);
            deaths[deathCount++]={x,y,float(ArenaEffect::PlayerBurst),256,pose.height,float(player)};
        }
    }
    auto emitDeaths=[&]{
        if(deathCount && settings.combat_feedback)impact(ImpactTier::Large,true);
        if(resident_ && settings.destruction)for(const auto& hit:scene.impacts){
            if(!(hit.flags&BrokenJoint) || packet_.spawns.size()>=250)continue;
            const bool eye=hit.oid==0x9904 || hit.oid==0x9905;
            packet_.spawns.push_back({hit.x-scene.camera_x,hit.y-scene.camera_y,
                float(ArenaEffect::MachineWound),1,hit.dx,hit.dy,float(eye),float(hit.paletteBank)});
        }
        if(settings.explosive_debris && resident_)std::erase_if(packet_.remains,[&](const EnemyRemains& body){
            const float x=body.x-scene.camera_x,y=body.y-scene.camera_y;
            if(!(body.flags&BlastDeath) || packet_.spawns.size()>=254 ||
               x<0 || x>=scene.width || y<24 || y>=scene.height)return false;
            const float distance=std::hypot(body.blastDx,body.blastDy);
            const float force=.40f+.60f/(1.f+distance/52.f);
            const float inv=distance>2.f?1.f/distance:0.f;
            const bool metal=body.flags&MachinePart;
            packet_.spawns.push_back({x,y,float(ArenaEffect::EnemyBlast),
                std::round((metal?29.f:55.f)*force*std::clamp(body.scale,.85f,1.6f)),
                body.blastDx*inv,body.blastDy*inv,float(metal),force});
            return true; // the moving pieces own their deposits as they land
        });
        for(unsigned i=0;i<deathCount;++i){
            const auto& burst=deaths[i];
            if(resident_)packet_.spawns.push_back(burst);
            else if(settings.blood)emit(Kind::Blood,burst.x,burst.y-burst.radius,128);
        }
    };
    if(!primed_ || scene.frame<previous_.frame || scene.frame-previous_.frame>8 || scene.room!=previous_.room ||
       scene.width!=previous_.width || scene.height!=previous_.height || scene.camera_x!=previous_.camera_x || scene.camera_y!=previous_.camera_y) {
        const bool continuous_room=primed_ && scene.frame>previous_.frame &&
            scene.frame-previous_.frame<=8 && scene.room==previous_.room &&
            scene.width==previous_.width && scene.height==previous_.height;
        const float elapsed=continuous_room?float(scene.frame-previous_.frame)*.01827925f:0;
        WeaponFxPacket retainedFx{};
        if(continuous_room)retainedFx=packet_.weaponFx;
        reset();packet_.cinema=cinema;packet_.hud=scene.hud;packet_.lamps=scene.lamps;
        if(continuous_room)packet_.weaponFx=retainedFx;
        if(settings.hardware_rt || settings.persistent_gore || settings.fluid_smoke ||
           settings.weapon_impacts || settings.contact_shadows)
            packet_.casters=scene.casters;
        previous_=scene;
        // Retain scene identity, not emitter continuity: an explosion already
        // visible when scrolling stops must still seed its cosmetic burst.
        previous_.objects.clear();packet_.remains=scene.remains;primed_=true;
        advance_weapon_fx(scene,elapsed,settings);emitDeaths();return;
    }
    // Repaints and pauses never advance cosmetic time; disabled categories vanish immediately.
    std::erase_if(particles_,[&](const Particle& p){return !enabled(p.kind,settings);});
    if(scene.frame==previous_.frame) return;
    emitDeaths();
    const float dt=float(scene.frame-previous_.frame)*.01827925f; // 506*289 / 8 MHz
    packet_.dt=dt;
    advance_weapon_fx(scene,dt,settings);
    for(auto& p:particles_) {
        p.life-=dt; p.x+=p.vx*dt;p.y+=p.vy*dt;p.z+=p.vz*dt;
        if(p.kind==Kind::Smoke) {p.size+=dt*1.5f;p.vx*=std::exp(-dt*2);p.vy*=std::exp(-dt*2);}
        else if(p.kind==Kind::Fire) {p.size+=dt*.4f;p.vx+=std::sin(p.life*17+p.y)*dt*5;}
        else {p.vz-=180*dt; if(p.z<0) {p.z=0;p.vz=-p.vz*.28f;p.vx*=.6f;p.vy*=.6f;
            if(p.kind==Kind::Blood || std::abs(p.vz)<3) {p.vz=0;p.vx=0;p.vy=0;}}}
        if(p.x<4 || p.x>scene.width-5) {p.x=std::clamp(p.x,4.f,float(scene.width-5));p.vx*=-.45f;}
        if(p.y<25 || p.y>scene.height-5) {p.y=std::clamp(p.y,25.f,float(scene.height-5));p.vy*=-.45f;}
    }
    std::erase_if(particles_,[](const Particle& p){return p.life<=0;});
    auto spawn_arena = [&](ArenaEffect kind,float x,float y,float dx=0,float dy=0) {
        if(resident_ && packet_.spawns.size()<256)
            packet_.spawns.push_back({x,y,float(kind),1,dx,dy});
    };
    auto command=[&](unsigned kind,float x,float y,float count,float dx,float dy,float variant,float size){
        if(resident_ && packet_.spawns.size()<256)packet_.spawns.push_back({x,y,float(kind),count,dx,dy,variant,size});
    };
    // Reuse the fixed quarter-resolution flow field. Eight injections and 24
    // core particles per observation bound even a simultaneous boss barrage.
    // Key moments and explosions take priority over attack wisps.
    unsigned smokeEvents=0,smokeCores=0;
    const unsigned smokeStyle=scene.room==0x10007?6:scene.room==0x20007?7:scene.room==0x20015?8:5;
    auto smokePuff=[&](float x,float y,float dx,float dy,float strength,unsigned cores){
        if(!settings.fluid_smoke || smokeEvents>=8)return;
        ++smokeEvents;
        command(15,x,y,1,dx,dy,float(smokeStyle),strength);
        const unsigned count=std::min(cores,24-smokeCores);
        if(count)command(16,x,y,float(count),dx,dy,strength>=1?1.f:0.f,0);
        smokeCores+=count;
    };
    if(cinema.death && !bossSmokeDeath_){
        for(float side:{-1.f,0.f,1.f})smokePuff(cinema.deathX+side*18,cinema.deathY,side*.35f,-.3f,1.65f,6);
    }
    bossSmokeDeath_=cinema.death;
    for(const auto& hit:scene.impacts)if((hit.flags&(BossPart|BrokenJoint))==(BossPart|BrokenJoint))
        smokePuff(hit.x-scene.camera_x,hit.y-scene.camera_y,0,-.3f,1.5f,6);
    if(settings.fluid_smoke)for(const auto& obj:scene.objects)if(obj.kind==EffectKind::Explosion){
        if(smokeEvents>=8)break;
        const bool fresh=std::none_of(previous_.objects.begin(),previous_.objects.end(),[&](const auto& old){
            return old.slot==obj.slot && old.kind==obj.kind && old.weapon==obj.weapon &&
                std::abs(old.x-obj.x)<32 && std::abs(old.y-obj.y)<32;
        });
        if(fresh)smokePuff(obj.x,obj.y,0,-.15f,scene.studio.boss?1.25f:1.05f,6);
    }
    for(const auto& hit:scene.impacts){
        if(hit.flags&BrokenJoint){
            if(settings.combat_feedback)impact(ImpactTier::Large,true);
            continue;
        }
        const float x=hit.x-scene.camera_x,y=hit.y-scene.camera_y;
        const bool metal=hit.flags&MachinePart,boss=hit.flags&BossPart;
        WeaponFamily family{};
        if(settings.combat_feedback && settings.weapon_signatures && projectile_weapon_family(hit.weapon,family) &&
           !has_weapon_fx(WeaponFxKind::Impact,family,x,y))
            queue_weapon_fx(WeaponFxKind::Impact,family,x,y,hit.dx,hit.dy,boss?1.4f:metal?1.15f:1.f);
        if(settings.combat_feedback)
            impact(boss||hit.weapon==0x4204||hit.weapon==0x4206?ImpactTier::Medium:ImpactTier::Small);
        if(metal){
            if(settings.particles)emit(Kind::Spark,x,y,boss?18:7);
            if(settings.destruction)command(13,x,y,boss?4:2,hit.dx,hit.dy,1,0);
            if(settings.blood)command(1,x,y,boss?40:14,hit.dx,hit.dy,1,0); // oil/grease spray
        }else{
            if(settings.blood)command(1,x,y,boss?72:24,hit.dx,hit.dy,0,0);
            if(settings.persistent_gore)command(11,x,y,1,hit.dx,hit.dy,0,boss?5:2);
        }
    }
    for(const auto& body:scene.remains){
        float x=body.x-scene.camera_x,y=body.y-scene.camera_y;
        if(x<0 || x>=scene.width || y<24 || y>=scene.height)continue;
        bool metal=body.flags&MachinePart,boss=body.flags&BossPart,blast=body.flags&BlastDeath;
        if(blast && settings.explosive_debris && resident_)continue;
        if(!metal && resident_ && settings.explosive_debris && settings.persistent_gore && packet_.spawns.size()<250){
            // Every organic kill bursts: torn meat and bone fan out along the
            // killing shot (the nearest observed hit), with an exit-wound spray.
            float dx=0,dy=0,best=28.f*28.f;
            for(const auto& hit:scene.impacts){
                const float hx=hit.x-scene.camera_x-x,hy=hit.y-scene.camera_y-y,d2=hx*hx+hy*hy,len=std::hypot(hit.dx,hit.dy);
                if(d2<best && len>.01f){best=d2;dx=hit.dx/len;dy=hit.dy/len;}
            }
            const float scale=std::clamp(body.scale,.85f,1.6f);
            packet_.spawns.push_back({x,y-3,float(ArenaEffect::EnemyBlast),std::round((boss?32.f:16.f)*scale),dx,dy,0,.52f});
            if(settings.blood)command(1,x,y-4,boss?48:30,dx*4,dy*4,0,0);
        }
        if(metal){
            if(settings.particles)emit(Kind::Spark,x,y-4,boss?64:blast?40:18);
            if(settings.blood)command(1,x,y-4,boss?64:blast?32:12,0,0,1,0);
        }else if(settings.blood)emit(Kind::Blood,x,y-4,boss?96:blast?36:7);
        if(settings.destruction)command(13,x,y-4,boss?22:blast?12:4,0,0,metal?1:0,0);
    }
    if(settings.persistent_gore){
        // Sample each moving contact along its path, even on skipped host frames.
        auto trace=[&](unsigned lane,float x,float y,float dx,float dy){
            const float distance=std::hypot(dx,dy);
            if(distance<.1f || distance>20)return;
            const bool tread=lane>=2;
            const float spacing=tread?3.f:5.f;
            auto& travel=stepDistance_[lane];
            for(float t=spacing-travel;t<=distance;t+=spacing){
                float side=tread?0.f:((++nextFoot_[lane]&1)?1.5f:-1.5f);
                command(11,x+((t-distance)*dx-side*dy)/distance,
                        y+((t-distance)*dy+side*dx)/distance,1,
                        dx/distance,dy/distance,2,float(lane+1));
            }
            travel=std::fmod(travel+distance,spacing);
        };
        for(unsigned player=1;player<=2;player++){
            auto foot=[&](const EffectScene& sc)->const ShadowCaster*{
                for(auto& c:sc.casters)if(c.player==player && c.y+c.h==c.foot)return &c;
                return nullptr;
            };
            auto now=foot(scene),old=foot(previous_);if(!now||!old)continue;
            float x=now->x+now->w*.5f;
            trace(player-1,x,now->foot,x-(old->x+old->w*.5f),now->foot-old->foot);
        }
        for(const auto& tank:scene.targets){
            if(tank.oid!=0x9801 && tank.oid!=0x9802)continue;
            const auto prior=std::find_if(previous_.targets.begin(),previous_.targets.end(),
                [&](const auto& old){return old.slot==tank.slot && old.oid==tank.oid;});
            if(prior==previous_.targets.end())continue;
            float x=tank.x+tank.w*.5f,y=tank.y+tank.h-1;
            trace(2+tank.oid-0x9801,x,y,x-(prior->x+prior->w*.5f),y-(prior->y+prior->h-1));
        }
    }
    for(const auto& flash:scene.visible_muzzles){
        // Player brass leaves the gun on the displayed firing cel, a few pixels
        // behind the scanned tip. One casing per volley, like the flash.
        if(settings.brass && flash.weapon>=0x4201 && flash.weapon<=0x4206)
            command(unsigned(ArenaEffect::Brass),float(flash.x)-flash.dx*3.f,float(flash.y)-flash.dy*3.f,1,
                    flash.dx,flash.dy,float(flash.weapon-0x4201u),0);
        if(!settings.muzzle_flash)continue;
        if(resident_ && packet_.spawns.size()<256)
            packet_.spawns.push_back({float(flash.x),float(flash.y),float(ArenaEffect::Muzzle),1,flash.dx,flash.dy,1,
                                      flash.weapon>=0x4201 && flash.weapon<=0x4206 ? float(flash.weapon-0x4200u) : 0.f});
        WeaponFamily family{};
        if(settings.combat_feedback && settings.weapon_signatures && projectile_weapon_family(flash.weapon,family))
            queue_weapon_fx(WeaponFxKind::Muzzle,family,float(flash.x),float(flash.y),flash.dx,flash.dy,1);
    }
    size_t emitters=0;
    unsigned smokeTrails=0;
    for(const auto& obj:scene.objects) {
        if(++emitters>96 && obj.kind!=EffectKind::Projectile) continue;
        const auto prior=std::find_if(previous_.objects.begin(),previous_.objects.end(),[&](const auto& old){
            return old.slot==obj.slot && old.kind==obj.kind && old.weapon==obj.weapon && std::abs(old.x-obj.x)<32 && std::abs(old.y-obj.y)<32;});
        bool newly_visible=prior==previous_.objects.end();
        bool launched=newly_visible || (prior->vx==0 && prior->vy==0 && (obj.vx!=0 || obj.vy!=0));
        // Brass ejects from the gun for every shot, including the standard 0x4201
        // whose retail sprite owns the rest of its flight. Start a few pixels behind
        // the measured firing tip (the ejection port), not at the bullet sprite.
        if(newly_visible && settings.brass && !obj.raster_muzzle &&
           (obj.kind==EffectKind::Projectile || obj.kind==EffectKind::EnemyShot)) {
            const unsigned casing=obj.kind==EffectKind::EnemyShot ? (obj.weapon==0x8500 ? 6u : 7u) :
                (obj.weapon>=0x4201 && obj.weapon<=0x4206 ? obj.weapon-0x4201u : 0u);
            const float speed=std::hypot(obj.vx,obj.vy);
            float ex=obj.barrel?obj.muzzle_x:obj.x-obj.vx, ey=obj.barrel?obj.muzzle_y:obj.y-obj.vy;
            if(speed>.1f){ex-=obj.vx/speed*3.f;ey-=obj.vy/speed*3.f;}
            command(unsigned(ArenaEffect::Brass),ex,ey,1,obj.vx,obj.vy,float(casing),0);
        }
        if(obj.kind==EffectKind::Projectile && obj.weapon!=0x4206){
            const float speed=std::hypot(obj.vx,obj.vy);
            if(!obj.raster_muzzle && launched && settings.muzzle_flash && speed>.1f)
                command(unsigned(ArenaEffect::Muzzle),obj.barrel?obj.muzzle_x:obj.x-obj.vx,
                        obj.barrel?obj.muzzle_y:obj.y-obj.vy,1,obj.vx/speed,obj.vy/speed,0,float(obj.weapon-0x4200u));
            if(obj.weapon==0x4201)continue; // its retail sprite owns the rest of its flight
        }
        if(!newly_visible && scene.studio.boss && obj.kind==EffectKind::EnemyShot &&
           scene.frame/10!=previous_.frame/10 && smokeTrails<2){
            const float speed=std::hypot(obj.vx,obj.vy);
            if(speed>.1f){
                smokePuff(obj.x-obj.vx,obj.y-obj.vy,-obj.vx/speed*.2f,-obj.vy/speed*.2f,.35f,1);
                ++smokeTrails;
            }
        }
        WeaponFamily family{};
        if(settings.combat_feedback && settings.weapon_signatures &&
           obj.kind==EffectKind::Projectile && projectile_weapon_family(obj.weapon,family)){
            const float speed=std::hypot(obj.vx,obj.vy);
            if(!obj.raster_muzzle && weaponFxObjectContinuity_ && launched && speed>.1f){
                const float x=obj.barrel?obj.muzzle_x:obj.x-obj.vx;
                const float y=obj.barrel?obj.muzzle_y:obj.y-obj.vy;
                queue_weapon_fx(WeaponFxKind::Muzzle,family,x,y,obj.vx/speed,obj.vy/speed,1);
            }
            if(!newly_visible){
                const float oldSpeed=std::hypot(prior->vx,prior->vy);
                if(speed>.1f && oldSpeed>.1f){
                    const float dot=(obj.vx*prior->vx+obj.vy*prior->vy)/(speed*oldSpeed);
                    if(dot<-.35f){
                        queue_weapon_fx(WeaponFxKind::Ricochet,family,obj.x,obj.y,obj.vx/speed,obj.vy/speed,1);
                        if(settings.combat_feedback)impact(ImpactTier::Small);
                    }
                }
            }
        }
        if(launched && (settings.muzzle_flash || settings.weapon_impacts || settings.fluid_smoke) && (obj.kind==EffectKind::EnemyShot ||
           (obj.kind==EffectKind::Projectile && obj.weapon!=0x4204 && obj.weapon!=0x4206))){
            float dx=obj.vx,dy=obj.vy;
            float x=obj.barrel ? obj.muzzle_x : obj.x-dx, y=obj.barrel ? obj.muzzle_y : obj.y-dy;
            // The orb creates a stationary one-pixel HLINE/VLINE and grows it
            // outward. Find its emitter to distinguish left/right or up/down.
            if(dx==0 && dy==0 && obj.weapon==0x8500 &&
               (obj.image==0xfffb55a0u || obj.image==0xfffb5620u)){
                float nearest=32*32;
                for(const auto& t:scene.targets){
                    if((t.oid&0x1f00)!=0x0400)continue;
                    float ox=x-(t.x+t.w*.5f),oy=y-(t.y+t.h*.5f);
                    float dist=ox*ox+oy*oy;
                    if(dist>=nearest)continue;
                    nearest=dist;
                    dx=obj.image==0xfffb55a0u?ox:0;
                    dy=obj.image==0xfffb5620u?oy:0;
                }
            }
            float speed=std::hypot(dx,dy);
            if(speed>.1f){
                if(settings.muzzle_flash && obj.kind==EffectKind::EnemyShot)spawn_arena(ArenaEffect::Muzzle,x,y,dx/speed,dy/speed);
                if(settings.fluid_smoke){
                    if(scene.studio.boss && obj.kind==EffectKind::EnemyShot)
                        smokePuff(x,y,dx/speed,dy/speed,.55f,2);
                    else{
                        const unsigned count=std::min(3u,24-smokeCores);
                        if(count)command(16,x,y,float(count),dx/speed,dy/speed,0,0);
                        smokeCores+=count;
                    }
                }
                bool energy=obj.kind==EffectKind::EnemyShot || obj.weapon==0x4205;
                if(settings.weapon_impacts || settings.fluid_smoke)command(15,x,y,1,dx/speed,dy/speed,energy?1.f:0.f,0);
            }
        }
        if(newly_visible && (obj.kind==EffectKind::FleshBreak || obj.kind==EffectKind::MetalBreak)){
            // A neutral fragment image alone cannot assign flesh to a boss component.
            // Typed contact/retirement above owns all additional boss tissue.
            if(scene.studio.boss && obj.kind==EffectKind::FleshBreak)continue;
            bool metal=obj.kind==EffectKind::MetalBreak;
            if(settings.fluid_smoke && !settings.destruction)command(15,obj.x,obj.y,1,obj.vx,obj.vy,4,0);
            if(settings.destruction && scene.studio.boss && settings.shockwaves)spawn_arena(ArenaEffect::Shockwave,obj.x,obj.y);
            if(settings.destruction)command(13,obj.x,obj.y,metal?6:8,obj.vx,obj.vy,metal?1:0,0);
            if(settings.persistent_gore)command(11,obj.x,obj.y,1,obj.vx,obj.vy,metal?1:0,metal?5:9);
        }
        // Player bullets already have their retail sprite, including at analog bearings.
        if(obj.kind==EffectKind::Projectile) {
            if(newly_visible && settings.particles) emit(Kind::Spark,obj.x,obj.y,1);
        } else if(obj.kind==EffectKind::Blood && !scene.studio.boss) {
            if(newly_visible && settings.persistent_gore)command(11,obj.x,obj.y,1,obj.vx,obj.vy,0,4);
            if(newly_visible && settings.blood) emit(Kind::Blood,obj.x,obj.y,24);
            if(newly_visible && settings.debris) emit(Kind::Debris,obj.x,obj.y,5);
        } else if(obj.kind==EffectKind::Explosion) {
            if(newly_visible && settings.combat_feedback)impact(ImpactTier::Large);
            // The pressure ring and scorch belong to Weapon impacts; Dynamic smoke adds its plume separately.
            if(newly_visible && settings.weapon_impacts)command(15,obj.x,obj.y,1,0,0,2,0);
            if(newly_visible && settings.shockwaves) spawn_arena(ArenaEffect::Shockwave,obj.x,obj.y);
            if(newly_visible && settings.voxels) emit(Kind::Voxel,obj.x,obj.y,8,true);
            if(newly_visible && settings.debris) emit(Kind::Debris,obj.x,obj.y,6,true);
            if(newly_visible && settings.particles) emit(Kind::Spark,obj.x,obj.y,12);
            // Stable per-object patterns share the existing Fire/Smoke controls.
            // Cosmetic origins only: never replace the retail explosion/damage.
            const unsigned variant=((obj.slot>>4)^unsigned(obj.x*17)^unsigned(obj.y*31))&3u;
            if(settings.fire) {
                switch(variant) {
                case 0: // ragged three-lobe fireball
                    emit(Kind::Fire,obj.x-3,obj.y+1,1);emit(Kind::Fire,obj.x+3,obj.y,1);emit(Kind::Fire,obj.x,obj.y-3,1);break;
                case 1: // rising narrow plume
                    emit(Kind::Fire,obj.x-1,obj.y,1);emit(Kind::Fire,obj.x+1,obj.y-4,1);emit(Kind::Fire,obj.x,obj.y-8,1);break;
                case 2: // lateral blast fan
                    emit(Kind::Fire,obj.x-7,obj.y+1,1);emit(Kind::Fire,obj.x,obj.y-2,1);emit(Kind::Fire,obj.x+7,obj.y+1,1);break;
                default: // staggered diagonal burst
                    emit(Kind::Fire,obj.x-5,obj.y-5,1);emit(Kind::Fire,obj.x,obj.y,1);emit(Kind::Fire,obj.x+5,obj.y+4,1);break;
                }
            }
            if(settings.smoke && newly_visible) {
                const float dx=variant==2?6.f:variant==3?4.f:2.f;
                const float dy=variant==1?-6.f:variant==3?-4.f:-1.f;
                emit(Kind::Smoke,obj.x-dx,obj.y+dy,3);
                emit(Kind::Smoke,obj.x,obj.y-2,3);
                emit(Kind::Smoke,obj.x+dx,obj.y-dy,3);
            }

        }
    }
    // Swept sprite-contact cosmetics, not arcade damage/death evidence. A segment
    // entering a target emits once; continuing overlap does not become a trail.
    std::vector<uint64_t> contacts;
    for(const auto& old:previous_.objects) if(old.kind==EffectKind::Projectile) {
        auto now=std::find_if(scene.objects.begin(),scene.objects.end(),[&](const auto& o){return o.slot==old.slot&&o.kind==old.kind;});
        float dx=now==scene.objects.end()?0:now->x-old.x,dy=now==scene.objects.end()?0:now->y-old.y;
        if(std::abs(dx)>32||std::abs(dy)>32)continue;
        bool hit=false;
        for(const auto& target:previous_.targets) {
            RayBox bounds{{target.x-2,target.y-2,-1},{target.x+target.w+2,target.y+target.h+2,1}};
            auto inside=[&](float x,float y){return x>=bounds.lo.x&&x<=bounds.hi.x&&y>=bounds.lo.y&&y<=bounds.hi.y;};
            RayHit contact;
            bool entered=!inside(old.x,old.y)&&intersect_box(bounds,{old.x,old.y,0},{dx,dy,0},0,1,contact);
            const uint64_t key=(uint64_t(old.slot)<<32)|target.slot;
            if(now!=scene.objects.end() && inside(now->x,now->y))contacts.push_back(key);
            if(!entered && !(now==scene.objects.end()&&inside(old.x,old.y)))continue;
            if(std::find(contacts_.begin(),contacts_.end(),key)!=contacts_.end()){hit=true;break;}
            float x=old.x+dx*(entered?contact.t:0),y=old.y+dy*(entered?contact.t:0);
            WeaponFamily family{};
            if(settings.combat_feedback && settings.weapon_signatures && projectile_weapon_family(old.weapon,family) &&
               !has_weapon_fx(WeaponFxKind::Impact,family,x,y))
                queue_weapon_fx(WeaponFxKind::Impact,family,x,y,old.vx,old.vy,target.metal?1.2f:1.f);
            if(settings.combat_feedback)impact(target.metal?ImpactTier::Medium:ImpactTier::Small);
            if(settings.persistent_gore)command(11,x,y,1,old.vx,old.vy,target.metal?1:0,target.metal?3:6);
            if(settings.weapon_impacts || settings.fluid_smoke)command(15,x,y,1,old.vx,old.vy,old.weapon==0x4204?2:old.weapon==0x4205?1:3,1);
            if(target.metal) {
                if(settings.particles)emit(Kind::Spark,x,y,scene.studio.boss?24:5);
                if(settings.blood && scene.impacts.empty())command(1,x,y,scene.studio.boss?48:18,old.vx,old.vy,1,0);
            } else {
                if(settings.blood && scene.impacts.empty()){
                    if(resident_)command(1,x,y,scene.studio.boss?104:52,old.vx,old.vy,0,0);
                    else emit(Kind::Blood,x,y,scene.studio.boss?52:26);
                }
                if(settings.debris)emit(Kind::Debris,x,y,5);
                if(settings.voxels)emit(Kind::Voxel,x,y,3);
            }
            hit=true;break;
        }
        if(hit || now!=scene.objects.end())continue;
        float x=old.x,y=old.y;
        const auto& floor=scene.geometry.floor;
        const float front=floor.y+floor.h;
        const bool nearFront=floor.h>0 && old.vy>0 && old.y+old.vy>=front;
        bool frontContact=false;
        if(nearFront){
            const float contactX=old.x+(front-old.y)*(old.vx/old.vy);
            if(contactX>=floor.x && contactX<floor.x+floor.w){
                x=contactX;
                y=front;
                frontContact=true;
            }
        }
        if(frontContact || old.x<12 || old.x>scene.width-13 || old.y<33 ||
           (floor.h<=0 && old.y>scene.height-13)) {
            WeaponFamily family{};
            if(settings.combat_feedback && settings.weapon_signatures && projectile_weapon_family(old.weapon,family)){
                const float speed=std::hypot(old.vx,old.vy);
                if(speed>.1f)
                    queue_weapon_fx(WeaponFxKind::Ricochet,family,x,y,old.vx/speed,old.vy/speed,.8f);
            }
            if(settings.combat_feedback)impact(ImpactTier::Small);
            if(settings.particles)emit(Kind::Spark,x,y,4);
        }
    }
    contacts_=std::move(contacts);
    // Append wakes after combat events so crowded rooms prioritize fresh shots.
    if(settings.fluid_smoke){
        for(const auto& target:scene.targets){
            auto old=std::find_if(previous_.targets.begin(),previous_.targets.end(),
                [&](const EffectTarget& t){return t.slot==target.slot;});
            if(old==previous_.targets.end())continue;
            float dx=target.x-old->x,dy=target.y-old->y;
            if(dx*dx+dy*dy>.04f && dx*dx+dy*dy<144.f)
                command(17,target.x+target.w*.5f,target.y+target.h*.7f,1,dx,dy,0,0);
        }
        for(const auto& c:scene.casters)if(c.player && c.foot==c.y+c.h){
            for(const auto& last:previous_.casters)if(last.player==c.player && last.foot==last.y+last.h){
                float dx=c.x-last.x,dy=c.foot-last.foot;
                if(dx*dx+dy*dy>.04f && dx*dx+dy*dy<144.f)command(17,c.x+c.w*.5f,c.foot-6,1,dx,dy,0,0);
                break;
            }
        }
    }
    previous_=scene;
    weaponFxObjectContinuity_=true;
}
void EnhanceEffects::compose(std::span<uint32_t> pixels,int width,int height,const PcSettings& s) const {
    if(!s.enhance || !primed_ || width!=previous_.width || height!=previous_.height || pixels.size()!=size_t(width)*height) return;
    std::vector<GpuParticle> draw;
    for(const auto& p:particles_) if(enabled(p.kind,s)) {
        const float fadeTime=(p.kind==Kind::Smoke || p.kind==Kind::Fire)?.8f:.4f;
        draw.push_back({p.x,p.y-p.z,p.size,float(p.kind),float((p.color>>16)&255)/255.f,
            float((p.color>>8)&255)/255.f,float(p.color&255)/255.f,std::min(1.f,p.life/fadeTime),p.life/p.total,p.z,p.y,0});
    }
    render_gpu_effects(pixels,width,height,draw,{}, {},false,gpu_error_);
}
}
