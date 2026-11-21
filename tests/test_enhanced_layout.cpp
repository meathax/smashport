// Layout regression using the actual prepared native data, not a second remap.
// Optional PPMs audit each unique original/widened background module. They are
// static composition evidence; actor animation is verified by runtime fixtures.
#include "stv/gsp/ops.hpp"
#include "stv/gsp/scene.hpp"
#ifdef _WIN32
#include "stv/host/gpu_resident.hpp"
#endif

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace stv::gsp;
namespace {
int failures=0;
void check(bool value,const char* why,u32 address=0) {
    if (!value) { std::fprintf(stderr,"%08x: %s\n",address,why); ++failures; }
}
u16 word(const Machine& m,u32 a) {
    const bool native=a>=ENHANCED_DATA_BASE && a<ENHANCED_DATA_END;
    const auto& v=native ? m.enhanced_data : m.prog;
    const std::size_t i=(a-(native ? ENHANCED_DATA_BASE : 0xff800000u))>>4;
    if (i>=v.size()) { check(false,"data address outside loaded image",a); return 0; }
    return v[i];
}
u32 lng(const Machine& m,u32 a) { return u32(word(m,a))|(u32(word(m,a+16))<<16); }
struct Block {
    u16 flags,x,y,header,w,h;
    u32 image,palette;
    int order;
};
std::vector<Block> blocks(const Machine& m,u32 module) {
    const u32 b=lng(m,module+48),h=lng(m,module+80),p=lng(m,module+112);
    const unsigned n=word(m,module+32);
    check(n>0 && n<2048,"invalid module block count",module);
    std::vector<Block> out;
    if (n>=2048) return out;
    for (unsigned i=0;i<n;++i) {
        const u32 a=b+i*64;
        const u16 f=word(m,a),hi=word(m,a+48);
        const u32 hdr=h+(hi&0xfffu)*32;
        out.push_back({f,word(m,a+16),word(m,a+32),hi,word(m,hdr),word(m,hdr+16),
                       lng(m,hdr+32),lng(m,p+((f&15u)|((hi>>12)<<4))*32),int(i)});
    }
    return out;
}
unsigned pixel(const Machine& m,const Block& b,unsigned x,unsigned y) {
    if (b.flags&16) x=b.w-1-x;
    if (b.flags&32) y=b.h-1-y;
    const std::size_t off=((b.image-0x02000000u)>>3)+y*((b.w+3u)&~3u)+x;
    if (off>=m.gfx.size()) { check(false,"graphics read beyond data",b.image); return 0; }
    return m.gfx[off];
}
bool same_art(const Block& a,const Block& b) {
    return a.image==b.image && a.flags==b.flags && a.w==b.w && a.h==b.h &&
           ((a.header>>12)==(b.header>>12));
}
int exact_count(const std::vector<Block>& bs,const Block& art,int x,int y) {
    return int(std::count_if(bs.begin(),bs.end(),[&](const Block& b) {
        return same_art(b,art) && b.x==x && b.y==y;
    }));
}
void protected_art(u32 module,const std::vector<Block>& ref,const std::vector<Block>& wide) {
    const bool studio=module==0xfff6c840u;
    if (module==0xfff81ef0u) {
        const auto interior=std::find_if(ref.begin(),ref.end(),[](const Block& b) {
            return b.w==100 && b.h==34 && b.y==113 && (b.flags&15)==1;
        });
        check(interior!=ref.end(),"blue interior palette fixture missing",module);
        if (interior!=ref.end()) for (const Block& b:wide) if ((b.flags>>8)==63)
            check(b.palette==interior->palette,"interior filler reused the grey bottom trim palette",module);
    }
    for (const Block& b:ref) {
        if (!studio && b.w==64 && b.h==37 && (b.y==41 || b.y==42))
            check(exact_count(wide,b,b.x+68,b.y)==1,
                  "upper floor/door steps must stay whole; their edge is not floor filler",module);
        if (studio) {
            // Both TV logos, two contestant podiums, all cameras/operators, MC,
            // and animated audience/door pieces remain original whole objects.
            const bool protected_piece=b.header==0x18 || b.header==0x1a ||
                b.header==0x22 || b.header==0x2c || b.header==0x2e ||
                b.header==0x32 || b.header==0x38 || b.header==0x3a;
            if (protected_piece)
                check(exact_count(wide,b,b.x+68,b.y)==1,
                      "studio unique assembly duplicated, split or displaced",module);
        } else {
            const unsigned z=b.flags>>8;
            const bool hud=(z>=0x73 && z<=0x77) || (b.w==112 && b.h==33 && (b.y==45 || b.y==46));
            const bool centre_door=z==0x78 || z==0x79;
            if (hud || centre_door) {
                const int dx=centre_door ? 68 : (b.x<196 ? 0 : 137);
                check(exact_count(wide,b,b.x+dx,b.y)==1,
                      "HUD/door assembly not preserved once at its anchor",module);
            }
        }
    }
}
void studio_panels(const Machine& reference,const Machine& wide,
                   const std::vector<Block>& ref,const std::vector<Block>& out) {
    int panels=0;
    for (const Block& b:ref) if (b.header==0x000e) {
        ++panels;
        const bool left=b.x<196;
        const int extra=left ? 68 : 69,cut=left ? 20 : 132;
        const auto found=std::find_if(out.begin(),out.end(),[&](const Block& n) {
            return n.x==b.x+(left ? 0 : 68) && n.y==b.y &&
                   n.w==b.w+extra && n.h==b.h &&
                   (n.flags&~16u)==(b.flags&~16u);
        });
        check(found!=out.end(),"complete studio TV/podium panel absent",0xfff6c840u);
        if (found==out.end()) continue;
        bool equal=true;
        for (unsigned y=0;y<b.h;++y)
            for (unsigned x=0;x<b.w;++x)
                equal=equal && pixel(reference,b,x,y)==pixel(wide,*found,x+(x>=unsigned(cut) ? extra : 0),y);
        check(equal,"TV/podium original columns altered or repeated inside assembly",0xfff6c840u);
    }
    check(panels==2,"retail studio must contain exactly two TV/podium panels",0xfff6c840u);
}
void ppm(const Machine& m,u32 module,const std::filesystem::path& file) {
    const int width=word(m,module),height=word(m,module+16);
    auto bs=blocks(m,module);
    std::stable_sort(bs.begin(),bs.end(),[](const Block& a,const Block& b) {
        if ((a.flags>>8)!=(b.flags>>8)) return (a.flags>>8)<(b.flags>>8);
        if (a.x!=b.x) return a.x>b.x;
        return a.order>b.order;
    });
    std::vector<unsigned char> rgb(std::size_t(width)*height*3,49);
    for (const Block& b:bs) {
        const unsigned colours=word(m,b.palette);
        for (unsigned y=0;y<b.h && int(b.y+y)<height;++y)
            for (unsigned x=0;x<b.w && int(b.x+x)<width;++x) {
                const unsigned v=pixel(m,b,x,y);
                if (!v && (b.flags&0x40)) continue;
                check(v<colours,"pixel exceeds source palette",module);
                if (v>=colours) continue;
                const u16 colour=word(m,b.palette+16+v*16);
                const std::size_t dest=(std::size_t(b.y+y)*width+b.x+x)*3;
                for (unsigned channel=0;channel<3;++channel) {
                    const unsigned c=(colour>>(10-channel*5))&31;
                    rgb[dest+channel]=static_cast<unsigned char>((c<<3)|(c>>2));
                }
            }
    }
    std::ofstream f(file,std::ios::binary);
    f<<"P6\n"<<width<<" "<<height<<"\n255\n";
    f.write(reinterpret_cast<const char*>(rgb.data()),std::streamsize(rgb.size()));
    check(bool(f),"could not write composition evidence",module);
}
// Exercise the translated background allocator and both display queues at each
// playable camera origin. Seed freed slots with real overhead/text IDs so any
// tile that borrows the previous occupant's identity trips the mask check.
void room_floor_masks(Machine& m) {
    if(m.enhanced)m.set_wide(1024,137);
    m.enable_sprite_coverage();
    m.external_timers=true;
    const auto address=[&](u32 a){return m.enhanced?enhanced_data_address(a):a;};
    unsigned rooms=0,draws=0;
#ifdef _WIN32
    stv::host::ResidentEffects gpu;
    stv::host::PcSettings settings;
    settings.enhance=settings.persistent_gore=true;
    const int width=m.enhanced?547:410;
#endif
    auto run=[&](u32 pc){
        constexpr u32 done=0xffe03660;
        m.c.sp=0x010fffe0;
        push(m,m.c,done);
        for(unsigned n=0;n<300000 && pc!=done && !m.stop;++n)
            pc=l8_run(m,pc,m.c.cyc+1);
        return pc==done && !m.stop && m.faults.empty();
    };
    for(unsigned c=0;c<3;++c)for(unsigned w=1;w<=(c==0?10:c==1?18:22);++w){
        if(c && w==10)continue;
        std::fill(m.ram.begin(),m.ram.end(),0);
        m.c=Cpu{};
        m.ww(L8_GAME_STATE,0x10);
        const u32 camera=lng(m,lng(m,address(0xffe8c7f0)+32*c)+32*w);
        wlong(m,L8_BACKGROUND_MODULES,lng(m,address(0xffe72650)+32*c));
        wlong(m,0x01000280,camera);
        const u32 window=m.enhanced?0x00fc0218:0x00fc018b;
        wlong(m,0x010002c0,window);
        wlong(m,0x01000060,object_pool_base);
        for(unsigned i=0;i<object_slots;++i){
            const u32 node=object_pool_base+i*object_stride;
            wlong(m,node,i+1<object_slots?node+object_stride:0);
            m.ww(node+0x150,i%2?2:0x070f);
        }
        if(!run(0xffe0c0f0)){check(false,"retail background update failed",camera);return;}
        m.c.b[4]=camera;
        m.c.b[1]=128u<<16;
        m.c.a[13]=window;
        m.c.a[14]=0;
        constexpr u32 queue=0x01033160, size=420*0xa0;
        m.c.a[4]=queue+size;
        m.c.a[5]=queue+3*size;
        m.ww(0x01085200,1); // no queued one-shot DMA to merge
        std::fill(m.display_queue_coverage.begin(),m.display_queue_coverage.end(),1);
        if(!run(0xffe01320)){check(false,"retail display list failed",camera);return;}
        const auto count=m.c.b[2]+m.c.b[3];
        check(count>0 && count<840,"empty or oversized room display list",camera);
        std::fill(m.vram.begin(),m.vram.end(),0);
        for(auto end:{420u,1260u}){
            const auto length=end==420?m.c.b[2]:m.c.b[3];
            if(length<=420)check(std::all_of(m.display_queue_coverage.begin()+end-length,
                m.display_queue_coverage.begin()+end,[](u8 tag){return tag==0;}),
                "room art masks floor effects",camera);
            // DMAINT consumes five longwords from each record, starting
            // with control/stride. Issue the command after the other registers.
            for(unsigned i=1;i<=length && i<=420;++i){
                const u32 record=queue+(end-i)*0xa0;
                m.c.b[14]=record+0xa0;
                for(unsigned reg=1;reg<=9;++reg)
                    m.ww(0x01a00000+16*reg,m.rw(record+16*reg));
                m.ww(0x01a00000,m.rw(record));
            }
        }
#ifdef _WIN32
        std::vector<u16> indexed(width*256);
        for(int y=0;y<256;++y)for(int x=0;x<width;++x){
            const auto at=y*m.vram_w+x;
            const u16 raw=m.vram[at];
            indexed[y*width+x]=((raw&0xc000)>>8)|(raw&0x0f3f)|(m.sprite_coverage[at]<<12);
        }
        stv::host::EffectPacket packet{};
        packet.in_arena=true;packet.room=(c<<16)|w;packet.serial=1000*rooms+1;
        packet.frame=packet.serial;packet.camera_x=s16(camera);packet.camera_y=s16(camera>>16);
        const int left=m.enhanced?33:29,right=m.enhanced?513:372;
        packet.geometry.floor={float(left),46.f,float(right-left),195.f};
        for(int y=48;y<241;y+=12)for(int x=left+2;x<right;x+=12)
            packet.remains.push_back({float(packet.camera_x+x),float(packet.camera_y+y),1.f,
                0x8600,unsigned(x*257+y),packet.room});
        // Room art is drawn without the retail palette fade. Deposits only show on drawn
        // (non-black) surfaces, so give every pen a lit stand-in colour.
        std::vector<u32> pens(4096);
        for(unsigned i=0;i<pens.size();++i)pens[i]=0x404040u+(i&63)*0x010101u;
        std::vector<u32> first,later;
        auto read=[&](std::vector<u32>& out){
            return gpu.update_arcade(indexed,width,width,256,pens,false,false,packet,settings,true)
                && gpu.capture(out);
        };
        if(!read(first)){check(false,gpu.error().c_str(),camera);return;}
        packet.remains.clear();++packet.serial;packet.frame+=600;
        if(!read(later)){check(false,gpu.error().c_str(),camera);return;}
        for(int y=46;y<241;y+=16)for(int x=left;x<right;x+=16){
            bool marked=false;
            for(int j=y;j<std::min(y+16,241);++j)for(int k=x;k<std::min(x+16,right);++k){
                const int at=j*width+k;
                const auto base=pens[indexed[at]&4095];
                // Wetness changes the tint over time. Check the footprint.
                marked|=first[at]!=base && later[at]!=base;
            }
            check(marked,"unstained floor region in GPU readback",camera);
        }
        // Inject smoke over the entire room, including a ground collision
        // fixture. A smoke-only readback must reach each sampled floor patch.
        settings.persistent_gore=false;settings.fluid_smoke=true;
        packet.geometry.left=float(left);packet.geometry.right=float(right);
        packet.geometry.top=46;packet.geometry.bottom=241;
        packet.geometry.solids={packet.geometry.floor};
        for(int y=52;y<241;y+=24){
            packet.spawns.clear();
            for(int x=left+5;x<right;x+=24)
                packet.spawns.push_back({float(x),float(y),15,1,1,0,2,0});
            ++packet.serial;++packet.frame;
            if(!gpu.update_arcade(indexed,width,width,256,m.pens,false,false,packet,settings,true)){
                check(false,gpu.error().c_str(),camera);return;
            }
        }
        if(!gpu.capture(later)){check(false,gpu.error().c_str(),camera);return;}
        for(int y=52;y<241;y+=24)for(int x=left+5;x<right;x+=24){
            const auto at=y*width+x;
            check(later[at]!=m.pens[indexed[at]&4095],"smoke missing over floor fixture",camera);
        }
        settings.fluid_smoke=false;settings.persistent_gore=true;
#endif
        draws+=count;++rooms;
    }
    check(rooms==48,"room floor mask inventory incomplete");
    std::printf("%s floor masks: %u rooms, %u background DMA draws\n",m.enhanced?"Wide":"Classic",rooms,draws);
}
void geometry_instructions() {
    // TURNON's X subtraction precedes DOIMG/ADJSTWTL. The overhead assembly
    // retains the approved viewport-left anchor: subtract89 in both modes.
    for (unsigned mode=0;mode<3;++mode) {
        for (u32 x:{90u,117u,137u,312u,446u,569u,1008u}) {
            Machine m;
            m.enhanced=mode!=0;
            m.ww(L8_GAME_STATE,mode==2 ? 0x30 : 0x18);
            m.external_timers=true;
            m.c.a[10]=x;
            m.c.a[11]=0x12345678u;
            const u32 next=l8_run(m,0xffe6c910u,1);
            check(m.c.a[10]==x-89u,"approved overhead scaffold left anchor regressed");
            check(m.c.a[11]==0x12345678u,"overhead scaffold Y changed");
            check(next==0xffe6c930u && m.c.cyc==2 && !m.stop && m.faults.empty(),
                  "overhead scaffold instruction timing/continuation changed");
        }
    }
    struct Site { u32 pc,ref,wide; unsigned reg,length; };
    constexpr Site sites[]={
        {0xffe20250u,0x01300000u,0x01740000u,0,48},
        {0xffe202c0u,0x00670000u,0x00ab0000u,0,48},
        {0xffe20640u,0x00d80000u,0x011c0000u,0,48},
        {0xffe206b0u,0x00d00000u,0x01140000u,0,48},
        {0xffe200f0u,0x00080000u,0x000c0000u,0,48},
        {0xffe20190u,0x01900000u,0x021d0000u,0,48},
        {0xffe204e0u,0x00080000u,0x000c0000u,0,48},
        {0xffe20580u,0x01900000u,0x021d0000u,0,48},
        // Original studio camera hold plus the part of the extra actor travel
        // not supplied by the longer camera scroll. Only INIT_GAME uses it.
        {0xffe77940u,0x000000c8u,0x00000100u,0,32},
        {0xffe8e7a0u,0x01000190u,0x01000219u,8,48},
        {0xffe8e7d0u,0x008000c8u,0x0080010cu,10,48},
        {0xffe7b0e0u,0x00220000u,0x00260000u,1,48},
        {0xffe7b130u,0x010f0000u,0x019c0000u,1,48},
        {0xffe791e0u,0x008000c4u,0x0080010cu,9,48},
        {0xffe90ec0u,0x008000c8u,0x00800110u,10,48},
        {0xffe90ef0u,0x008000c8u,0x00800110u,11,48},
        {0xffe90fc0u,0x008000c8u,0x00800110u,1,48},
    };
    for (const Site& s:sites) {
        u64 cycles=0;
        for (unsigned mode=0;mode<3;++mode) {
            Machine m;
            m.enhanced=mode!=0;
            m.ww(L8_GAME_STATE,mode==2 ? 0x30 : 0x18);
            m.external_timers=true;
            const u32 next=l8_run(m,s.pc,1);
            const u32 expected=mode==1 ? s.wide : s.ref;
            check(m.c.a[s.reg]==expected,"geometry immediate did not match scene policy",s.pc);
            check(next==s.pc+s.length && m.faults.empty() && !m.stop,
                  "actor instruction boundary/fault changed",s.pc);
            if (!mode) cycles=m.c.cyc;
            else check(m.c.cyc==cycles,"actor instruction cycle cost changed",s.pc);
        }
    }
}
} // namespace
int main(int argc,char** argv) {
    if (argc<2 || argc>3) {
        std::fprintf(stderr,"usage: stv_enhanced_layout_tests smashtv.zip [static-module-output]\n"); return 2;
    }
    Machine reference;
    std::string error;
    if (!reference.load_roms(argv[1],error)) { std::fprintf(stderr,"%s\n",error.c_str()); return 2; }
    Machine wide=reference;
    wide.enhanced=true;
    wide.ww(L8_GAME_STATE,0x18);
    if (!apply_enhanced(wide,137)) {
        for (const auto& f:wide.faults) std::fprintf(stderr,"%08x: %s\n",f.addr,f.what.c_str());
        return 1;
    }
    check(reference.prog==wide.prog,"preparation changed imported program ROM");
    // Traverse the retail room stream independently, including its unused slots.
    // Only ordinary budgets may change; no timing, formation or hazard changes.
    unsigned reviewed=0,changed=0;
    for(unsigned c=0;c<3;++c) {
        u32 a=lng(reference,0xffe13790u+c*32);
        check(lng(wide,enhanced_data_address(0xffe13790u+c*32))==enhanced_data_address(a),"population nested root not relocated");
        for(unsigned w=1;w<=(c==0?10:c==1?18:22);++w) {
            const bool unused=c&&w==10,boss=(c==0&&w==10)||(c==1&&w==7)||(c==2&&(w==7||w==21));
            if(!unused)++reviewed;
            for(unsigned guard=0;word(reference,a)&&guard<16;++guard,a+=96) {
                const unsigned type=word(reference,a),budget=word(reference,a+16);
                unsigned expected=budget;
                if(!unused&&!boss&&!(c==2&&w==22)) {
                    const unsigned percent=135+10*c+5*((w-1)/6);
                    if((type==6||type==10||type==2)&&budget>=32)
                        expected=budget*(percent-(budget>=600||type==2?15:0))/100;
                }
                changed+=expected!=budget;
                const unsigned rate=word(reference,a+48);
                const unsigned release=expected==budget||!rate ? rate :
                    std::max<u64>(1,u64(rate)*budget*21/(u64(expected)*20));
                for(unsigned f=0;f<6;++f) {
                    const unsigned field= f==1 ? expected : f==3 ? release : word(reference,a+f*16);
                    check(word(wide,enhanced_data_address(a+f*16))==field,
                          "stage field, budget or release pacing differs",a+f*16);
                }
            }
            check(word(wide,enhanced_data_address(a))==0,"population terminator changed",a);a+=16;
        }
    }
    check(reviewed==48&&changed==40,"incomplete stage balance inventory");
    {Machine m=reference;room_floor_masks(m);}
    {Machine m=wide;room_floor_masks(m);}
    for(unsigned i=0;i<27;++i) {
        const u32 a=0xffe85200u+i*8;const unsigned bit=a&8u;
        check(u8(word(wide,enhanced_data_address(a&~15u))>>bit)==u8(word(reference,a&~15u)>>bit)+72,"bonus prize pile not aligned with boards",a);
    }
    for(bool enabled:{false,true})for(u32 background:{u32(0),u32(0xffea5670u)}) {
        Machine m;m.enhanced=enabled;m.external_timers=true;m.ww(L8_GAME_STATE,0x18);
        m.ww(L8_BACKGROUND_MODULES,u16(background));m.ww(L8_BACKGROUND_MODULES+16,u16(background>>16));
        check(l8_run(m,0xffe88e80u,1)==0xffe88ea0u&&m.c.a[11]==(enabled&&!background?110u:38u),"bonus portrait origin or fixed initials changed");
    }

    for(bool enabled:{false,true})for(u16 state:{u16(0),u16(0x10),u16(0x18),u16(0x30),u16(0x50)}) {
        Machine m=wide;m.enhanced=enabled;m.ww(L8_GAME_STATE,state);m.external_timers=true;m.c.a[0]=64;
        const u64 cycle=m.c.cyc;const auto next=l8_run(m,0xffe18440u,cycle+1);
        const bool live=enabled&&(state==0x10||state==0x18||state==0x50);
        check(next==0xffe18470u&&m.c.cyc-cycle==3&&m.c.a[0]==64+(live?enhanced_data_address(0xffe13790u):0xffe13790u),"population root scene/timing isolation");
    }
    {Machine bad=reference;bad.enhanced=true;bad.prog[(0xffe13800u-0xff800000u)>>4]^=1;
     check(!apply_enhanced(bad,137)&&bad.stop,"changed retail population accepted");}

    for (bool enabled : {false, true}) {
        Machine m = wide;
        m.enhanced = enabled;
        m.ww(L8_GAME_STATE, 0x10);
        m.external_timers = true;
        const u64 cycle = m.c.cyc;
        const u32 next = l8_run(m, 0xffe7c5d0u, cycle + 1);
        const u32 table = enabled ? enhanced_data_address(0xffe7c550u) : 0xffe7c550u;
        check(next == 0xffe7c600u && m.c.cyc == cycle + 3 && m.c.a[0] == table,
              "STOPME pit root or timing differs");
        const u32 limits = lng(m, table + 64);
        check((limits >> 16) == (enabled ? 154u : 150u) &&
              (limits & 65535) == (enabled ? 376u : 235u), "two-pit passage edges");
    }

    check(lng(reference,0xffe1f920u)==90112u,"Classic cardinal speed changed");
    check(lng(wide,enhanced_data_address(0xffe1f920u))==112640u,"Widescreen cardinal speed differs");
    check(lng(wide,enhanced_data_address(0xffe1f940u))==80000u,"Widescreen diagonal speed differs");
    check(lng(wide,enhanced_data_address(0xffe1f980u))==u32(-112640),"Widescreen negative speed differs");
    check(lng(wide,enhanced_data_address(0xffe1fba0u))==158720u,"Widescreen shoe speed differs");
    check(wide.gfx.size()>reference.gfx.size() &&
          std::equal(reference.gfx.begin(),reference.gfx.end(),wide.gfx.begin()),
          "original graphics changed or native strip data absent");
    check(std::all_of(wide.gfx.begin()+reference.gfx.size(),wide.gfx.end(),
                      [](u8 value){return value<64;}),"materialized strips exceed six-bit graphics");
    geometry_instructions();
    std::map<u32,u32> modules;
    for (unsigned circuit=0;circuit<3;++circuit) {
        u32 a=lng(reference,0xffe72650u+circuit*32);
        u32 b=lng(wide,enhanced_data_address(0xffe72650u+circuit*32));
        for (unsigned n=0;n<200 && lng(reference,a)!=0xffffffffu;++n,a+=64,b+=64) {
            const u32 original=lng(reference,a),replacement=lng(wide,b);
            const auto [i,added]=modules.emplace(original,replacement);
            check(added || i->second==replacement,"shared module relocated inconsistently",original);
            check(word(reference,a+48)==word(wide,b+48),"module world Y changed",original);
        }
    }
    const bool dump=argc==3;
    std::ofstream manifest;
    if (dump) {
        std::filesystem::create_directories(argv[2]);
        manifest.open(std::filesystem::path(argv[2])/"modules.json");
        manifest<<"{\"kind\":\"static_module_composition_not_runtime\",\"modules\":[\n";
    }
    unsigned widened=0,first=0;
    for (const auto& [original,replacement]:modules) {
        const unsigned ow=word(reference,original),nw=word(wide,replacement);
        const auto ref=blocks(reference,original),out=blocks(wide,replacement);
        const bool expands=ow>=390 && ow<=392;
        widened+=expands;
        check(nw==ow+(expands ? 137 : 0),"module width does not match scene geometry",original);
        check(word(reference,original+16)==word(wide,replacement+16),"module height changed",original);
        if (expands) {
            protected_art(original,ref,out);
            const u32 family=lng(reference,original+80);
            if (family==0xfff82400u || family==0xfff8c050u)
                for (const Block& b:ref)
                    if (b.y==0 && (b.w==68 || b.w==72) && b.h>=46 && b.h<=54 &&
                        ((b.x>=89 && b.x<=90) || (b.x>=230 && b.x<=235)))
                        check(exact_count(out,b,b.x+68,b.y)==1,
                              "unique stone face/plaque was sliced or duplicated",original);
            if (family==0xfff70370u)
                for (const Block& b:ref) if (b.w==96 && b.h==34 &&
                                            (b.header==0xbc || b.header==0xbe)) {
                    const int extra=b.x<196 ? 68 : 69;
                    const int x=b.x+(b.x<196 ? 0 : 68);
                    check(std::count_if(out.begin(),out.end(),[&](const Block& n) {
                        return n.x==x && n.y==b.y && n.w==b.w+extra && n.h==b.h &&
                               (n.flags&~16u)==(b.flags&~16u);
                    })==1,"bridge deck no longer reaches its centred medallion",original);
                }
            if (original==0xfff6c840u) studio_panels(reference,wide,ref,out);
        }
        int previous=-1;
        for (const Block& b:out) {
            check(b.x>=previous,"background X search order broken",original); previous=b.x;
            check(b.w>0 && b.w<=250 && b.h>0 && b.h<=512,
                  "block violates original BAKGND widest-block bound",original);
            check(b.image>=0x02000000u && !(b.image&7) &&
                  ((b.image-0x02000000u)>>3)+std::size_t((b.w+3u)&~3u)*b.h<=wide.gfx.size(),
                  "header points outside prepared graphics",original);
        }
        if (dump) {
            char prefix[16]; std::snprintf(prefix,sizeof(prefix),"%08x",original);
            const auto dir=std::filesystem::path(argv[2]);
            ppm(reference,original,dir/(std::string(prefix)+"-reference.ppm"));
            ppm(wide,replacement,dir/(std::string(prefix)+"-enhanced.ppm"));
            manifest<<(first++ ? ",\n" : "")<<"{\"original\":\""<<prefix<<"\",\"width\":"
                    <<ow<<",\"enhanced_width\":"<<nw<<",\"height\":"<<word(reference,original+16)
                    <<",\"original_blocks\":"<<ref.size()<<",\"enhanced_blocks\":"<<out.size()<<"}";
        }
    }
    if (dump) manifest<<"\n]}\n";
    check(modules.size()==31,"unexpected unique L8 background template count");
    check(widened==27,"unexpected widened arena/studio template count");
    // Widescreen raises the authored active-population caps while retaining
    // singleton/sentinel values and a signed-safe byte after difficulty.
    check((word(wide,enhanced_data_address(0xffe12d80u))>>8)==18,
          "widescreen concurrent Hulk population is not signed-safe");
    check((word(reference,0xffe12d80u)>>8)==12,
          "classic concurrent Hulk population changed");
    check((word(wide,enhanced_data_address(0xffe12d80u))&255)==2,
          "opening minimum population changed");
    // Non-singleton regular, heavy and ranged caps stay below the sign bit
    // after the +6 retail difficulty modifier.
    for (const auto [base,original] : {
            std::pair<u32,unsigned>{0xffe13018u,8}, {0xffe12f58u,8},
            {0xffe13178u,8}, {0xffe130d8u,20}}) {
        const auto byte=[&](const Machine& m,u32 a) {
            return unsigned((word(m,a&~15u)>>(a&8u))&255u);
        };
        check(byte(wide,enhanced_data_address(base+8))==original*2,
              "widescreen active-population cap crosses the signed-byte limit");
        check(byte(reference,base+8)==original,"classic regular crowd cap changed");
        check(byte(wide,enhanced_data_address(base))==2,"minimum crowd cap changed");
    }
    for (const auto [base,word_index] : {
            std::pair<u32,unsigned>{0xffe12d80u,1}, {0xffe12e40u,1},
            {0xffe12e98u,1}, {0xffe132c8u,1}}) {
        const auto byte=[&](const Machine& m,u32 a) {
            return unsigned((word(m,a&~15u)>>(a&8u))&255u);
        };
        check(byte(wide,enhanced_data_address(base+word_index*8u))==(3*byte(reference,base+word_index*8u)+1)/2,
              "specialist/ranged cap was not raised safely");
    }
    check(word(wide,enhanced_data_address(0xffe137f0u+48))==4 &&
          word(wide,enhanced_data_address(0xffe13a50u+48))==6,
          "boosted populations do not receive bounded faster release pacing");
    check(word(wide,enhanced_data_address(0xffe13a50u+16))==452,
          "stage population budget changed unexpectedly");
    // Exact endpoint durations, turn sequences and imported graphic pointers are
    // independently checked here; movement velocities are not touched.
    check(word(wide,enhanced_data_address(0xffe97910u))==123 &&
          word(wide,enhanced_data_address(0xffe979e0u))==123,
          "studio logo world anchor does not compensate for the shifted camera");
    check(word(wide,enhanced_data_address(0xffea45c0u))==430 &&
          word(wide,enhanced_data_address(0xffea46d0u))==430,
          "studio corridor duration does not cover the full 137-pixel studio-to-arena distance");
    check(word(wide,enhanced_data_address(0xffea5460u))==274 &&
          word(wide,enhanced_data_address(0xffea4d70u))==274,
          "lateral doorway script does not cover the added half-room distance");
    for (u32 base:{0xffea5450u,0xffea4d60u})
        for (unsigned i=0;i<5;++i) if (i!=1)
            check(word(reference,base+i*16)==word(wide,enhanced_data_address(base+i*16)),
                  "lateral script direction, neutral stop or terminator changed",base+i*16);
    for (u32 base:{0xffea54a0u,0xffea4db0u})
        for (unsigned i=0;i<5;++i)
            check(word(reference,base+i*16)==word(wide,enhanced_data_address(base+i*16)),
                  "vertical doorway script changed",base+i*16);
    for (u32 base:{0xffea44f0u,0xffea4600u})
        for (unsigned i=0;i<17;++i) if (i!=13)
            check(word(reference,base+i*16)==word(wide,enhanced_data_address(base+i*16)),
                  "original studio turn/control script changed",base);
    if (failures) { std::fprintf(stderr,"%d layout failure(s)\n",failures); return 1; }
    std::printf("%zu unique modules (%u widened): protected HUD/studio assemblies, graphic bounds, original data and compiled scene-gated geometry pass\n",
                modules.size(),widened);
    return 0;
}
