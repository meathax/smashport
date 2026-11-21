// Retail firing points. Boot the real game to play, force each weapon mode and
// fire in all eight directions. Record where the retail program spawns each
// player projectile (OXVAL/OYVAL plus its animation anchor at INSOBJ) and
// compare the host's muzzle effect origin with that spawn point.
#define main muzzle_fixture_unused_main
#include "test_room_entrances.cpp"
#undef main
#include "stv/host/enhance.hpp"
#include "stv/host/firing_point.hpp"
#include <cstdlib>
#include <map>
#include <set>
#include <vector>

namespace {
Machine boot(const char* rom,bool wide,u32& pc){
    Machine m;std::string err;require(m.load_roms(rom,err),"muzzle fixture ROM load");
    if(wide){m.enhanced=true;m.set_wide(1024,137);require(apply_enhanced(m,137),"muzzle fixture wide data");}
    m.reset();pc=m.reset_vector();
    m.on_vblank=[&]{const auto f=m.frame_number;
        if(f==2600||f==2640)m.ports[1]&=u16(~1);
        if(f==2612||f==2652)m.ports[1]|=1;
        if(f==3150)m.ports[1]&=u16(~0x04);if(f==3170)m.ports[1]|=0x04;
        protect(m);if(f>=4300){m.run_until_cycle=m.c.cyc;m.recompute_next_event();}};
    while(m.frame_number<4300&&!m.stop)pc=l8_run(m,pc,m.c.cyc+1000000);
    require(!m.stop&&m.rw(L8_GAME_STATE)==0x10,"muzzle fixture boot failed");m.on_vblank={};return m;
}
struct Spawn{u32 node;double ax,ay;int px,py;u16 oid;};
}
int main(int argc,char**argv){try{
    if(argc<2)return 2;
    const char* dump=argc>2?argv[2]:nullptr;
    constexpr int fire[8]={1,9,8,10,2,6,4,5}; // up, up-right, right, ... (FIRE nibble)
    unsigned cases=0,worst_estimate=0,bad=0;double worst=0;
    for(bool wide:{false,true}){
        u32 pc0=0;const Machine booted=boot(argv[1],wide,pc0);
        for(unsigned mode=0;mode<=4;++mode)for(int d=0;d<8;++d){
            Machine m=booted;u32 pc=pc0;
            const u32 torso=rlong(m,players);require(torso!=0,"P1 torso process missing");
            // PDATA weapon mode is an unaligned 16-bit field; keep its meter full.
            wfield(m,torso+0x198,mode,16);m.ww(torso+0x220,999);
            // Hide enemy objects (display list only) so the gun is unobstructed.
            auto hide_enemies=[&]{
                u32 link=0x01000040;
                for(u32 n=rlong(m,link),guard=0;n && guard<420;++guard){
                    const u32 next=rlong(m,n);const u16 oid=m.rw(n+0x150);
                    if((oid&0xc000)==0x8000 || oid==0x0800 || oid==0x0b00 || oid==0x0e00)wlong(m,link,next);else link=n;
                    n=next;
                }
            };
            std::vector<Spawn> spawns;
            m.on_object=[&](ObjectEvent e,u32 node,u32){
                if(e!=ObjectEvent::Insert)return;
                const u16 oid=m.rw(node+0x150);
                if(std::getenv("STV_MUZZLE_ALL") && mode==3 && d==2)std::printf("insert oid=%04x image=%08x\n",oid,rlong(m,node+0x130));
                if((oid&0xff00)!=0x4200)return;
                const auto a=anchor(m,node);
                spawns.push_back({node,a[0],a[1],s16(m.rw(node+0x70)),s16(m.rw(node+0x90)),oid});
            };
            // Hold fire with no movement until the first shot is two frames old.
            m.ports[0]=u16(~(fire[d]<<4));
            unsigned start=unsigned(m.frame_number);
            Spawn first{};bool have=false;std::array<double,2> observed{};
            for(unsigned f=1;f<=180 && !have;++f){
                const std::size_t before=spawns.size();
                if(!std::getenv("STV_MUZZLE_SHOW_ENEMIES"))hide_enemies();
                step_to(m,pc,start+f);
                for(std::size_t i=before;i<spawns.size();++i)
                    if(rlong(m,spawns[i].node+0x160)==torso || true){first=spawns[i];have=true;break;}
                if(have){
                    // What the presentation sees at the first completed frame:
                    // top-left OXPOS/OYPOS minus one frame of velocity.
                    const double vx=s32(rlong(m,first.node+0x20))/65536.0,vy=s32(rlong(m,first.node+0x40))/65536.0;
                    observed={s16(m.rw(first.node+0x70))-vx,s16(m.rw(first.node+0x90))-vy};
                    const double ex=observed[0]-first.ax,ey=observed[1]-first.ay,err=std::hypot(ex,ey);
                    // The machine's own record must equal the INSOBJ anchor.
                    const auto& rec=m.spawn_anchor[(first.node-object_pool_base)/object_stride];
                    require(rec.valid && rec.x==s16(std::floor(first.ax)) && rec.y==s16(std::floor(first.ay)),"spawn anchor record differs");
                    const int camx=s16(m.rw(world)),camy=s16(m.rw(world+16));
                    stv::host::EffectObject shot{first.node,rlong(m,first.node+0x130),stv::host::EffectKind::Projectile,
                        float(s16(m.rw(first.node+0x70))-camx),float(s16(m.rw(first.node+0x90))-camy),float(vx),float(vy),first.oid};
                    float fx=0,fy=0;require(stv::host::firing_point(m,shot,camx,camy,fx,fy),"no firing point");
                    // The point must be the first transparent pixel past the owner's ink.
                    const double len=std::hypot(vx,vy),ux=vx/len,uy=vy/len;
                    bool inside=false,before=false;
                    for(u32 n=rlong(m,0x01000040),g=0;n&&g<420;n=rlong(m,n),++g){
                        if((m.rw(n+0x150)&0xff00)!=0x4100)continue;
                        inside|=stv::host::sprite_ink(m,n,int(std::floor(fx+camx)),int(std::floor(fy+camy)));
                        before|=stv::host::sprite_ink(m,n,int(std::floor(fx+camx-ux)),int(std::floor(fy+camy-uy)));
                    }
                    std::printf("   firing_point=%.2f,%.2f past_anchor=%.1f tip_ink_before=%d tip_ink=%d\n",fx,fy,
                        std::hypot(fx-(rec.x-camx),fy-(rec.y-camy)),int(before),int(inside));
                    if(inside){std::printf("FIRING POINT INSIDE INK\n");++bad;}
                    if(err>worst){worst=err;worst_estimate=cases;}
                    std::printf("%s mode=%u dir=%d oid=%04x spawn_anchor=%.2f,%.2f spawn_pos=%d,%d legacy_estimate=%.2f,%.2f error=%.2f v=%.2f,%.2f\n",
                        wide?"wide":"classic",mode,d,first.oid,first.ax-s16(m.rw(world)),first.ay-s16(m.rw(world+16)),
                        first.px-s16(m.rw(world)),first.py-s16(m.rw(world+16)),
                        observed[0]-s16(m.rw(world)),observed[1]-s16(m.rw(world+16)),err,vx,vy);
                    if(dump){
                        // Draw only player (41xx) objects from ROM graphics at
                        // the spawn frame state, around the spawn anchor.
                        const int camx=s16(m.rw(world)),camy=s16(m.rw(world+16));
                        const int cx=int(std::floor(first.ax))-camx,cy=int(std::floor(first.ay))-camy;
                        std::vector<u32> img(48*48,0x202020);
                        for(u32 n=rlong(m,0x01000040),guard=0;n && guard<420;n=rlong(m,n),++guard){
                            const u16 oid=m.rw(n+0x150);if((oid&0xff00)!=0x4100)continue;
                            const int w=m.rw(n+0xf0),h=m.rw(n+0x100);const u16 flags=m.rw(n+0xb0),pal=m.rw(n+0x110);
                            if(w<=0||h<=0||w>200||h>200)continue;
                            u32 sag=rlong(m,n+0xd0);if(flags&16)sag-=u32(w-1)*8;
                            const long long base=(long long)((sag>=0x02000000u?sag-0x02000000u:sag)>>3);
                            const int pitch=(s16(m.rw(n+0xc0))+((flags&16)?-w:w)+3)&~3;
                            const unsigned bank=(pal&0xf00)|((pal>>8)&0xc0);
                            const int ox=s16(m.rw(n+0x70))-camx,oy=s16(m.rw(n+0x90))-camy;
                            for(int ty=0;ty<h;++ty)for(int tx=0;tx<w;++tx){
                                const int sx=(flags&16)?w-1-tx:tx,sy=(flags&32)?h-1-ty:ty;
                                const long long at=base+(long long)pitch*sy+sx;
                                if(at<0||at>=(long long)m.gfx.size())continue;
                                const u8 pen=m.gfx[size_t(at)]&63;if(!pen)continue;
                                const int x=ox+tx-(cx-24),y=oy+ty-(cy-24);
                                if(x>=0&&x<48&&y>=0&&y<48)img[y*48+x]=m.pens[bank|pen];
                            }
                        }
                        {const int px=int(std::floor(fx))-(cx-24),py=int(std::floor(fy))-(cy-24);
                         if(px>=0&&px<48&&py>=0&&py<48)img[py*48+px]=0xff00ff;}
                        char name[512];std::snprintf(name,sizeof name,"%s/%s-m%u-d%d.ppm",dump,wide?"wide":"classic",mode,d);
                        if(FILE* o=std::fopen(name,"wb")){
                            std::fprintf(o,"P6\n48 48\n255\n");
                            for(u32 c:img){const unsigned char rgb[3]={u8(c>>16),u8(c>>8),u8(c)};std::fwrite(rgb,1,3,o);}
                            std::fclose(o);
                        }
                    }
                }
            }
            if(!have){std::printf("%s mode=%u dir=%d no projectile spawned in 180 frames\n",wide?"wide":"classic",mode,d);++cases;continue;}
            ++cases;
        }
    }
    std::printf("MUZZLE cases=%u worst_legacy_error=%.2f (case %u) firing_points_inside_ink=%u\n",cases,worst,worst_estimate,bad);
    return bad?1:0;
}catch(const std::exception&e){std::fprintf(stderr,"FAIL muzzle points: %s\n",e.what());return 1;}}
