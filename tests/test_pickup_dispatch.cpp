// Execute the retail ICPL instructions and actual ROM table. Static provenance:
// tools/scenarios/pickup_dispatch_contract.py and shoe_effect_contract.py.
// Selection, full shoe calls and weapon pickup calls are tested. Spawned
// weapon-meter/ring processes have separate execution and lifetime obligations.
#include "stv/gsp/ops.hpp"
#include <cstdio>
#include <array>
#include <utility>
#include <vector>
using namespace stv::gsp;
int main(int argc,char** argv) {
    if(argc!=2)return 2;
    Machine loaded;std::string error;
    if(!loaded.load_roms(argv[1],error)){std::fprintf(stderr,"%s\n",error.c_str());return 2;}
    Machine enhanced_loaded=loaded;
    enhanced_loaded.enhanced=true;
    enhanced_loaded.set_wide(1024,137);
    if(!apply_enhanced(enhanced_loaded,137)){std::fprintf(stderr,"Enhanced fixture preparation failed\n");return 2;}
    constexpr u32 targets[]={0xffed25d0,0xffed3ba0,0xffe31a80,0xffe31cc0,
        0xffe27880,0xffe31700,0xffe2fd40,0xffe314c0,0xffe2ff60,0xffe30230,
        0xffe30450,0xffe2d920,0xffe2f9e0,0xffe2fa10,0xffe2cff0,0xffe2cda0};
    constexpr u32 item=0x010003a0,player=item+0x1f0,stack=0x010fffe0,returned=0xffec7270;
    unsigned checks=0,failures=0;
    auto check=[&](bool ok,const char* message){++checks;if(!ok){++failures;std::fprintf(stderr,"%s\n",message);}};
    for(bool wide:{false,true})for(bool signed_field:{false,true})
        for(unsigned player_sub=0;player_sub<4;++player_sub)
            for(unsigned subtype=0;subtype<16;++subtype) {
                Machine m=wide?enhanced_loaded:loaded;m.external_timers=true;
                m.c.sp=stack;m.c.a[1]=item;m.c.a[2]=player;m.c.b[0]=0xdeadbeef;
                if(signed_field)m.c.st|=0x20u;
                m.ww(item+0x150,u16(0x0700|subtype));
                m.ww(player+0x150,u16(0x4000|player_sub));
                push(m,m.c,returned);
                u32 pc=0xffe5de80;
                for(unsigned n=0;n<40 && pc!=returned && !m.stop;++n)
                    pc=l8_run(m,pc,m.c.cyc+1);
                check(pc==returned,"Pickup selector did not return");
                check(!m.stop && m.faults.empty(),"Pickup selector faulted");
                check(m.c.b[0]==(player_sub==3?0:targets[subtype]),"Wrong pickup handler/ring rejection");
                check(m.c.a[1]==item && m.c.a[2]==player,"Pickup selector clobbered input owners");
                check(m.c.sp==stack,"Pickup selector did not restore stack");
            }
    // Shoe assignment and expiry use linked owner records, with an unrelated
    // player's records as sentinels. The prefix ends before deletion/sound.
    constexpr u32 torsos[]={0x01087000,0x01088000},legs[]={0x01089000,0x0108a000};
    constexpr u32 leg_objects[]={0x01001000,0x01002000},torso_objects[]={0x01003000,0x01004000};
    for(bool wide:{false,true})for(unsigned owner=0;owner<2;++owner)
        for(unsigned circuit=0;circuit<3;++circuit)for(unsigned direction=0;direction<10;++direction) {
            Machine m=wide?enhanced_loaded:loaded;m.external_timers=true;m.c.sp=stack;
            for(unsigned n=0;n<2;++n) {
                wlong(m,leg_objects[n]+0x160,torsos[n]);
                wlong(m,torsos[n]+0x2c0,legs[n]);
                wlong(m,legs[n]+0x100,torso_objects[n]);
                m.ww(legs[n]+0x180,u16(n==owner?65535:1234));
            }
            m.ww(0x010eb610,u16(circuit));m.ww(0x010eeab0,0xa500);
            m.c.a[0]=leg_objects[owner];
            u32 pc=0xffe30230;
            for(unsigned n=0;n<20 && pc!=0xffe30380 && !m.stop;++n)pc=l8_run(m,pc,m.c.cyc+1);
            const u16 duration=circuit==2?500:300;
            check(pc==0xffe30380 && !m.stop && m.faults.empty(),"Shoe assignment prefix failed");
            check(m.rw(legs[owner]+0x180)==duration,"Shoe timer not replaced for correct owner/circuit");
            check(m.rw(legs[1-owner]+0x180)==1234,"Shoe assignment changed other player");
            check(m.rw(0x010eeab0)==u16(0xa500|(duration&255)),"Shoe flag byte or adjacent byte differs");
            // Begin a separate valid movement call; the assignment's later
            // cleanup and register restore are outside this scoped test.
            m.c.sp=stack;m.c.a[13]=legs[owner];m.c.a[8]=leg_objects[owner];m.c.a[6]=direction*32;
            for(unsigned tick=0;tick<=duration;++tick) {
                const bool fast=tick<duration;
                const u32 vx=rlong(m,(fast?0xffe1fb20u:0xffe1f8a0u)+direction*32);
                const u32 vy=rlong(m,(fast?0xffe1fc60u:0xffe1f9e0u)+direction*32);
                push(m,m.c,returned);pc=0xffe1fda0;
                for(unsigned n=0;n<40 && pc!=returned && !m.stop;++n)pc=l8_run(m,pc,m.c.cyc+1);
                check(pc==returned && !m.stop && m.faults.empty(),"Shoe movement call failed");
                check(m.rw(legs[owner]+0x180)==(fast?duration-tick-1:0),"Shoe decrement/zero boundary differs");
                check(m.rw(legs[1-owner]+0x180)==1234,"Shoe movement changed other player timer");
                check(rlong(m,leg_objects[owner]+0x20)==vx && rlong(m,leg_objects[owner]+0x40)==vy,
                      "Shoe leg velocity/table transition differs");
                check(rlong(m,torso_objects[owner]+0x20)==vx && rlong(m,torso_objects[owner]+0x40)==vy,
                      "Shoe torso velocity/table transition differs");
                check(m.c.sp==stack,"Shoe movement did not restore stack");
            }
        }
    // Execute the complete SHOEON, including real DELOBJ, ONESND/SNDLD and
    // sound-port writes. Valid linked-list fixtures cover sole/head/middle/tail
    // deletion, empty/nonempty free lists and the sound admission boundaries.
    constexpr u32 nodes[]={0x010003a0,0x01000590,0x01000780};
    constexpr u32 free_node=0x01000970,player_image=0x01000b60;
    constexpr u32 live_torsos[]={0x01086790,0x01086d30};
    constexpr u32 live_legs[]={0x010872d0,0x01087870};
    constexpr u32 sound_table=0xffe633f0,sound_base=0x010eae00;
    constexpr u16 priorities[]={0,0xf0ed,0xf0ee,0xf4ee,0xf0ef,0xf8ed,0xf4ed};
    unsigned full_cases=0;
    for(bool wide:{false,true})for(unsigned owner=0;owner<2;++owner)
        for(unsigned circuit=0;circuit<3;++circuit)for(unsigned position=0;position<4;++position)
            for(bool free_present:{false,true})for(u16 priority:priorities)for(bool suppressed:{false,true}) {
                ++full_cases;
                Machine m=wide?enhanced_loaded:loaded;m.external_timers=true;m.c.sp=stack;
                // ROBO.ASM startup sets FW0=16/FE0=1 and FW1=32/FE1=1.
                // Sound priority words rely on the signed field-0 load.
                m.c.st=0x830;
                const unsigned count=position==0?1:3,selected=position==0?0:position-1;
                const u32 icon=nodes[selected];
                for(unsigned n=0;n<count;++n) {
                    wlong(m,nodes[n],n+1<count?nodes[n+1]:0);
                    m.ww(nodes[n]+0xb0,u16(0x8123+n));
                }
                wlong(m,0x01000040,nodes[0]);wlong(m,0x01000060,free_present?free_node:0);
                wlong(m,free_node,0);m.ww(free_node+0xb0,0x4321);
                wlong(m,player_image+0x160,live_torsos[owner]);
                for(unsigned n=0;n<2;++n) {
                    wlong(m,live_torsos[n]+0x2c0,live_legs[n]);
                    m.ww(live_legs[n]+0x180,u16(n==owner?217:1234));
                }
                m.ww(0x010eb610,u16(circuit));m.ww(0x010eeab0,0xa500);
                m.ww(0x010865f0,suppressed?1:0);
                std::array<u16,32> channels{};
                for(unsigned n=0;n<channels.size();++n) {
                    channels[n]=u16(0x2100+n);m.ww(sound_base+n*16,channels[n]);
                }
                channels[0]=priority;m.ww(sound_base,priority);
                std::vector<std::pair<u32,u16>> sound_writes;
                m.on_sound_write=[&](u32 address,u16 value){sound_writes.emplace_back(address,value);};
                for(unsigned n=0;n<15;++n){m.c.a[n]=0xa0000000+n;m.c.b[n]=0xb0000000+n;}
                m.c.a[0]=player_image;m.c.a[8]=icon;
                const auto saved_a=std::to_array(m.c.a),saved_b=std::to_array(m.c.b);
                wlong(m,stack,0x13579bdf);wlong(m,stack-0x800,0x2468ace0);
                push(m,m.c,returned);u32 pc=0xffe30230;
                for(unsigned n=0;n<600 && pc!=returned && !m.stop;++n)pc=l8_run(m,pc,m.c.cyc+1);
                check(pc==returned && !m.stop && m.faults.empty(),"Complete SHOEON failed to return");
                check(m.c.sp==stack && rlong(m,stack)==0x13579bdf && rlong(m,stack-0x800)==0x2468ace0,
                      "Complete SHOEON stack/canaries differ");
                for(unsigned n=1;n<15;++n)check(m.c.a[n]==saved_a[n],"Complete SHOEON clobbered saved A register");
                for(unsigned n=0;n<15;++n)check(m.c.b[n]==saved_b[n],"Complete SHOEON clobbered B register");
                check(m.c.a[0]==sound_table,"Complete SHOEON final sound pointer differs");
                const u16 duration=circuit==2?500:300;
                check(m.rw(live_legs[owner]+0x180)==duration && m.rw(live_legs[1-owner]+0x180)==1234,
                      "Complete SHOEON changed wrong owner timer");
                check(m.rw(0x010eeab0)==u16(0xa500|(duration&255)),"Complete SHOEON icon flag differs");
                u32 link=rlong(m,0x01000040);
                for(unsigned n=0;n<count;++n)if(n!=selected) {
                    check(link==nodes[n],"Icon removal changed surviving object order");link=rlong(m,nodes[n]);
                    check(m.rw(nodes[n]+0xb0)==u16(0x8123+n),"Icon removal changed surviving flags");
                }
                check(link==0,"Icon removal left a tail/cycle");
                check(rlong(m,0x01000060)==icon && rlong(m,icon)==(free_present?free_node:0),
                      "Deleted icon was not returned to free-list head");
                check(m.rw(icon+0xb0)==u16((0x8123+selected)&0x7fff),"Deleted icon in-use bit/other flags differ");
                check(rlong(m,free_node)==0 && m.rw(free_node+0xb0)==0x4321,"Existing free object changed");
                const bool accepted=!suppressed && !(priority&0x800) &&
                    ((priority&255)<0xee || ((priority&255)==0xee && !(priority&0x400)));
                if(accepted) {
                    check(sound_writes==std::vector<std::pair<u32,u16>>{{0x01e00000,0xfd36},{0x01e00000,0xff36}},
                          "Shoe sound command/strobe sequence differs");
                    channels[0]=0xf4ee;channels[1]=channels[2]=0x34;
                    channels[3]=u16(sound_table);channels[4]=u16(sound_table>>16);
                    channels[5]=u16(sound_table+48);channels[6]=u16((sound_table+48)>>16);
                } else check(sound_writes.empty(),"Suppressed/priority-rejected shoe sound was emitted");
                for(unsigned n=0;n<channels.size();++n)
                    check(m.rw(sound_base+n*16)==channels[n],"Sound channel state or adjacent channel changed");
            }
    constexpr u32 weapon_entries[]={0xffe31a80,0xffe31cc0,0xffe31700,0xffe314c0};
    unsigned weapon_cases=0;
    for(bool wide:{false,true})for(unsigned owner=0;owner<2;++owner)
        for(unsigned mode=1;mode<=4;++mode)for(unsigned previous=0;previous<5;++previous)
            for(bool face:{false,true})for(bool suppressed:{false,true}) {
                ++weapon_cases;
                Machine m=wide?enhanced_loaded:loaded;m.external_timers=true;m.c.st=0x830;
                auto call=[&](u32 entry) {
                    m.c.sp=stack;push(m,m.c,returned);u32 pc=entry;
                    for(unsigned n=0;n<20000 && pc!=returned && !m.stop;++n)pc=l8_run(m,pc,m.c.cyc+1);
                    check(pc==returned && !m.stop && m.faults.empty(),"Weapon pickup/setup failed bounded return");
                    check(m.c.sp==stack,"Weapon pickup/setup stack imbalance");
                };
                call(0xffe0a7d0); // Actual PINIT and GETPRC establish valid owners/free pool.
                u32 owners[2]{};
                for(unsigned n=0;n<2;++n) {
                    m.c.a[13]=0x01000000;m.c.a[1]=247+n;m.c.a[7]=returned;
                    call(0xffe0aba0);owners[n]=m.c.a[0];
                    wb(m,owners[n]+0x1d8,n+1);
                    wlong(m,owners[n]+0x190,0xa500005a);wf(m,owners[n]+0x198,n==owner?previous:4,0);
                }
                wlong(m,player_image+0x160,owners[owner]);
                wlong(m,0x01000040,item);wlong(m,item,0);m.ww(item+0xb0,0x8123);
                wlong(m,0x01000060,free_node);wlong(m,free_node,0);
                m.ww(0x010eeab0,0xa500);m.ww(0x010de380,0xb600);m.ww(0x010f31f0,u16(0xc700|face));
                m.ww(0x010865f0,suppressed?1:0);
                for(unsigned n=0;n<32;++n)m.ww(sound_base+n*16,0);
                std::vector<std::pair<u32,u16>> sound_writes;
                m.on_sound_write=[&](u32 address,u16 value){sound_writes.emplace_back(address,value);};
                for(unsigned n=0;n<15;++n){m.c.a[n]=0xa0000000+n;m.c.b[n]=0xb0000000+n;}
                m.c.a[0]=player_image;m.c.a[8]=item;m.c.a[13]=owners[owner];
                const auto saved_a=std::to_array(m.c.a),saved_b=std::to_array(m.c.b);
                call(weapon_entries[mode-1]);
                const bool ring=face && (mode==2 || mode==3);
                check(rf(m,owners[owner]+0x198,0)==mode && rf(m,owners[1-owner]+0x198,0)==4,
                      "Weapon pickup selected wrong mode/player");
                for(u32 p:owners)check(rb(m,p+0x190)==0x5a && rb(m,p+0x1a8)==0xa5,
                                      "Unaligned weapon field overwrote adjacent bytes");
                check(m.rw(0x010eeab0)==u16(0xa500|mode) && m.rw(0x010de380)==0xb601,
                      "Weapon pickup icon/fill flag or adjacent byte differs");
                check(rlong(m,0x01000040)==0 && rlong(m,0x01000060)==item &&
                      rlong(m,item)==free_node && m.rw(item+0xb0)==0x123,"Weapon icon cleanup differs");
                check(m.c.a[0]==1,"Weapon pickup return A0 differs");
                for(unsigned n=1;n<15;++n)check(m.c.a[n]==(n==9 && ring?owner+1:saved_a[n]),
                                               "Weapon pickup register return contract differs");
                for(unsigned n=0;n<15;++n)check(m.c.b[n]==saved_b[n],"Weapon pickup clobbered B register");
                unsigned fills=0,rings=0,used=0,free_count=0;std::array<bool,240> seen{};
                for(unsigned list=0;list<2;++list) {
                    u32 p=rlong(m,0x01000000+list*32);
                    while(p) {
                        const bool valid=p>=0x01086790 && p<0x010dad90 && (p-0x01086790)%0x5a0==0;
                        check(valid,"Weapon child list points outside process pool");if(!valid)break;
                        const unsigned index=(p-0x01086790)/0x5a0;
                        check(!seen[index],"Weapon child list has duplicate/reused owner");if(seen[index])break;
                        seen[index]=true;
                        if(list)++free_count;
                        else {
                            ++used;
                            if(p!=owners[0] && p!=owners[1]) {
                                const u32 wake=rlong(m,p+0xe0);
                                check(m.rw(p+0x30)==1 && rlong(m,p+0x40)==p+0x5a0,"Weapon child scheduler initialization differs");
                                if(wake==0xffe27fd0) {
                                    ++fills;check(m.rw(p+0x20)==0 && rlong(m,p+0x80)==owners[owner],"FILLUP has wrong PID/owner");
                                } else if(wake==0xffe10af0) {
                                    ++rings;check(m.rw(p+0x20)==273 && rlong(m,p+0xa0)==owner+1 &&
                                        rlong(m,p+0x80)==(mode==2?260:270),"Boss ring player/duration differs");
                                } else check(false,"Unexpected weapon child entry");
                            }
                        }
                        p=rlong(m,p);
                    }
                }
                check(fills==1 && rings==unsigned(ring) && used==3+unsigned(ring) && used+free_count==240,
                      "Weapon child count or process pool partition differs");
                check(sound_writes==(suppressed?std::vector<std::pair<u32,u16>>{}:
                    std::vector<std::pair<u32,u16>>{{0x01e00000,0xfd91},{0x01e00000,0xff91}}),
                    "Weapon pickup ICON sound/suppression differs");
            }
    // Drive the real refill process through PRCD1/PRCD2, FRANIM, PRCSLP and
    // SUCIDE. Each iteration is one isolated scheduler visit, not wall time.
    constexpr u16 shot_tables[2][5]={{100,20,20,8,17},{100,9,12,5,8}};
    constexpr u32 meter_objects[]={0x01001000,0x01001200};
    constexpr u32 meter_images[]={0xfffbab20,0xfffba820,0xfffba8a0,0xfffba920,
                                  0xfffba9a0,0xfffbaa20,0xfffbaaa0,0xfffbab20};
    unsigned meter_cases=0;
    for(bool wide:{false,true})for(unsigned owner=0;owner<2;++owner)
        for(unsigned mode=1;mode<=4;++mode)for(unsigned dip=0;dip<2;++dip) {
            ++meter_cases;
            constexpr u16 wide_charges[2][5]={{100,25,25,10,21},{100,11,15,6,10}};
            const u16 charges=wide?wide_charges[dip][mode]:shot_tables[dip][mode];
            Machine m=wide?enhanced_loaded:loaded;m.external_timers=true;m.c.st=0x830;
            m.ww(0x010865a0,0x10);
            m.ports[3]=u16(dip?0xffff:0xbfff);
            auto call=[&](u32 entry) {
                m.c.sp=stack;push(m,m.c,returned);u32 pc=entry;
                for(unsigned n=0;n<20000 && pc!=returned && !m.stop;++n)pc=l8_run(m,pc,m.c.cyc+1);
                check(pc==returned && !m.stop && m.faults.empty(),"Meter routine/setup failed bounded return");
                check(m.c.sp==stack,"Meter routine/setup stack imbalance");
            };
            call(0xffe0a7d0);u32 owners[2]{};
            for(unsigned n=0;n<2;++n) {
                m.c.a[13]=0x01000000;m.c.a[1]=247+n;m.c.a[7]=returned;
                call(0xffe0aba0);owners[n]=m.c.a[0];
                wb(m,owners[n]+0x1d8,n+1);wf(m,owners[n]+0x198,n==owner?mode:2,0);
                m.ww(owners[n]+0x220,n==owner?777:999);
                wlong(m,0x010ee0a0+n*32,meter_objects[n]);
                wlong(m,meter_objects[n]+0x130,meter_images[0]);
                wb(m,0x010de418+n*8,n==owner?7:4);
            }
            m.ww(0x010de380,0xb601);
            m.c.a[13]=owners[owner];m.c.a[1]=0;m.c.a[7]=0xffe27fd0;m.c.a[10]=owners[owner];
            call(0xffe0aba0);const u32 filling=m.c.a[0];
            unsigned visits=0;bool finished=false;
            while(visits<100 && !finished && !m.stop) {
                ++visits;m.c.sp=stack;m.c.a[13]=owners[owner];u32 pc=0xffe0a500;
                // Enter once before testing the same dispatcher boundary.
                pc=l8_run(m,pc,m.c.cyc+1);
                for(unsigned n=0;n<20000 && pc!=0xffe0a500 && !m.stop;++n)pc=l8_run(m,pc,m.c.cyc+1);
                check(pc==0xffe0a500 && !m.stop && m.faults.empty(),"Refill process failed scheduler boundary");
                check(m.c.sp==stack,"Refill process leaked native return stack");
                finished=rlong(m,0x01000020)==filling;
                check(rb(m,0x010de418+(1-owner)*8)==4 && m.rw(owners[1-owner]+0x220)==999,
                      "Refill changed other player's meter/counter");
            }
            check(finished && visits==37,"Refill animation/sleep lifetime differs");
            check(rlong(m,owners[owner])!=filling && m.rw(0x010de380)==0xb600,
                  "Refill did not unlink itself or clear only fill flag");
            check(rb(m,0x010de418+owner*8)==1 && m.rw(owners[owner]+0x220)==charges,
                  "Refill meter stage or retail DIP-selected counter differs");
            check(rlong(m,meter_objects[owner]+0x130)==meter_images[1],"Refill final meter image differs");
            // FFLG suppresses depletion for either player while refill is active.
            m.c.a[10]=owners[owner];m.c.a[8]=item;m.ww(0x010de380,0xb601);
            call(0xffe28390);
            check(rb(m,0x010de418+owner*8)==1 && rf(m,owners[owner]+0x198,0)==mode,
                  "Active refill failed to suppress metering");
            m.ww(0x010de380,0xb600);m.ww(0x010865f0,1); // Sound admission has separate coverage.
            for(unsigned stage=2;stage<=7;++stage) {
                m.c.a[10]=owners[owner];m.c.a[8]=item;call(0xffe28390);
                check(rb(m,0x010de418+owner*8)==stage && rb(m,0x010de418+(1-owner)*8)==4,
                      "Meter depletion changed wrong stage/player");
                check(rlong(m,meter_objects[owner]+0x130)==meter_images[stage],"Depletion meter image differs");
                check(rf(m,owners[owner]+0x198,0)==(stage==7?0:mode),"Weapon did not expire exactly at meter stage7");
                check(m.rw(owners[owner]+0x220)==charges && m.rw(owners[1-owner]+0x220)==999,
                      "Depletion counter reset or other player counter differs");
            }
            m.c.a[10]=owners[owner];call(0xffe28390);
            check(rb(m,0x010de418+owner*8)==7 && rf(m,owners[owner]+0x198,0)==0,"Empty meter failed saturation");
        }
    // An already-created ring at RTP: exercise its real timer/follow loop and
    // destruction, including spawned blink ownership. Palette/blink children
    // are inspected but not scheduled here; entry-time allocation is separate.
    unsigned ring_cases=0;
    for(bool wide:{false,true})for(unsigned owner=0;owner<2;++owner)
        for(unsigned duration:{260u,270u})for(unsigned pauses:{0u,3u})for(unsigned protection=0;protection<3;++protection) {
            ++ring_cases;
            Machine m=wide?enhanced_loaded:loaded;m.external_timers=true;m.c.st=0x830;
            auto call=[&](u32 entry) {
                m.c.sp=stack;push(m,m.c,returned);u32 pc=entry;
                for(unsigned n=0;n<20000 && pc!=returned && !m.stop;++n)pc=l8_run(m,pc,m.c.cyc+1);
                check(pc==returned && !m.stop && m.faults.empty() && m.c.sp==stack,"Ring fixture process setup failed");
            };
            auto create=[&](u32 parent,u16 pid,u32 wake) {
                m.c.a[13]=parent;m.c.a[1]=pid;m.c.a[7]=wake;call(0xffe0aba0);return m.c.a[0];
            };
            call(0xffe0a7d0);u32 owners[2]{},colors[2]{};
            for(unsigned n=0;n<2;++n) {
                owners[n]=create(0x01000000,u16(247+n),returned);
                wlong(m,0x010de2b0+n*32,owners[n]);wb(m,owners[n]+0x1d8,n+1);
                wb(m,owners[n]+0x230,1);wb(m,owners[n]+0x238,1);m.ww(owners[n]+0x160,77);
                wlong(m,nodes[n]+0x160,owners[n]);wlong(m,nodes[n]+0x130,0xfffae3a0);
                wlong(m,nodes[n]+0x60,(100+n*40)<<16);wlong(m,nodes[n]+0x80,120<<16);
                m.ww(nodes[n]+0xb0,0x8002);
                colors[n]=create(0x01000000,u16(259+n),returned);
            }
            const u32 other_blink=create(0x01000000,u16(244+(1-owner)),returned);
            const u32 ring_object=nodes[2];
            m.c.a[9]=ring_object;m.c.a[10]=owner*32;m.c.a[11]=nodes[owner];
            const u32 ring=create(owners[owner],273,0xffe10f80);
            m.ww(ring+0x170,u16(duration));wb(m,ring+0x138,owner+1);
            wlong(m,ring_object+0x160,ring);wlong(m,ring_object+0x130,0xfffae3a0);
            m.ww(ring_object+0xb0,0x8002);
            wlong(m,0x01000040,nodes[0]);wlong(m,nodes[0],ring_object);wlong(m,ring_object,nodes[1]);wlong(m,nodes[1],0);
            wlong(m,0x01000060,free_node);wlong(m,free_node,0);
            if(protection==1)wlong(m,owners[owner]+0x2e0,owners[1-owner]);
            if(protection==2)wb(m,owners[owner]+0x230,2);
            auto active=[&](u32 wanted) {
                u32 p=rlong(m,0x01000000);
                for(unsigned n=0;p && n<240;++n) {if(p==wanted)return true;p=rlong(m,p);}
                return false;
            };
            unsigned visits=0;bool finished=false;
            while(visits<duration+pauses+1 && !finished && !m.stop) {
                const bool paused=visits<pauses;++visits;m.ww(0x010eb7f0,paused?1:0);
                m.c.sp=stack;m.c.a[13]=owners[owner];u32 pc=0xffe0a500;
                pc=l8_run(m,pc,m.c.cyc+1);
                for(unsigned n=0;n<20000 && pc!=0xffe0a500 && !m.stop;++n)pc=l8_run(m,pc,m.c.cyc+1);
                check(pc==0xffe0a500 && !m.stop && m.faults.empty() && m.c.sp==stack,"Ring loop failed scheduler/stack boundary");
                const unsigned remaining=duration-(visits>pauses?visits-pauses:0);
                check(m.rw(ring+0x170)==remaining,"Ring timer pause/decrement differs");
                check(active(colors[1-owner]) && active(other_blink) && active(owners[0]) && active(owners[1]),
                      "Ring processing removed other owner/process");
                check(m.rw(owners[1-owner]+0x160)==77 && rb(m,owners[1-owner]+0x230)==1 && rb(m,owners[1-owner]+0x238)==1,
                      "Ring processing changed other player's protection");
                check(active(colors[owner])==(remaining>100),"Ring color cycle did not end at100 remaining");
                unsigned blinks=0;u32 p=rlong(m,0x01000000);
                for(unsigned n=0;p && n<240;++n,p=rlong(m,p))if(m.rw(p+0x20)==244+owner) {
                    ++blinks;check(rlong(m,p+0xe0)==0xffe11b10 && rlong(m,p+0xa0)==ring_object,
                                  "Blink child wake/object owner differs");
                }
                check(blinks==unsigned(remaining>0 && remaining<=100),"Ring blink allocation/removal boundary differs");
                finished=!active(ring);
                check(finished==(remaining==0),"Ring process lifetime differs");
                if(!finished && !paused)check(m.rw(owners[owner]+0x160)==40,"Ring did not refresh owner protection delay");
            }
            check(finished && visits==duration+pauses && rlong(m,0x01000020)==ring,"Ring failed exact expiry/free return");
            check(rlong(m,0x01000040)==nodes[0] && rlong(m,nodes[0])==nodes[1] && rlong(m,nodes[1])==0,
                  "Ring object removal changed player objects");
            check(rlong(m,0x01000060)==ring_object && rlong(m,ring_object)==free_node && m.rw(ring_object+0xb0)==2,
                  "Ring object not released or flags changed");
            check(rb(m,owners[owner]+0x230)==(protection==0?0:protection==1?1:2) &&
                  rb(m,owners[owner]+0x238)==(protection==0?0:1),"Ring expiry removed another protection boundary");
        }
    // Full RINGS entry and its palette/blink children, with real object/palette
    // allocators and PALTRANS. Only unrelated synthetic owner processes are
    // skipped during the linked-list scan; newly inserted children run in order.
    unsigned ring_entry_cases=0;
    for(bool wide:{false,true})for(unsigned owner=0;owner<2;++owner)for(unsigned duration:{260u,270u}) {
        ++ring_entry_cases;
        Machine m=wide?enhanced_loaded:loaded;m.external_timers=true;m.c.st=0x830;
        auto call=[&](u32 entry) {
            m.c.sp=stack;push(m,m.c,returned);u32 pc=entry;
            for(unsigned n=0;n<30000 && pc!=returned && !m.stop;++n)pc=l8_run(m,pc,m.c.cyc+1);
            check(pc==returned && !m.stop && m.faults.empty() && m.c.sp==stack,"Full ring setup/palette transfer failed");
        };
        call(0xffe0a7d0);call(0xffe02750);call(0xffe09000);
        u32 owners[2]{},objects[2]{};
        for(unsigned n=0;n<2;++n) {
            m.c.a[13]=0x01000000;m.c.a[1]=247+n;m.c.a[7]=returned;call(0xffe0aba0);owners[n]=m.c.a[0];
            wlong(m,0x010de2b0+n*32,owners[n]);wb(m,owners[n]+0x1d8,n+1);
            m.c.a[13]=owners[n];m.c.a[0]=(100+n*40)<<16;m.c.a[1]=120<<16;
            m.c.a[2]=0xfffae3a0;m.c.a[3]=100;m.c.a[4]=0x8002;m.c.a[5]=0x4101+n;m.c.a[6]=m.c.a[7]=0;
            call(0xffe04170);objects[n]=m.c.a[8];
            wlong(m,0x010dd780+n*32,objects[n]);wlong(m,owners[n]+0x100,objects[n]);
            if(n==1){m.c.a[0]=0xfffc6570;call(0xffe09260);m.ww(objects[n]+0x110,u16(m.c.a[0]));}
        }
        call(0xffe09b70);
        m.c.a[13]=owners[owner];m.c.a[1]=273;m.c.a[7]=0xffe10af0;m.c.a[9]=owner+1;m.c.a[10]=duration;
        call(0xffe0aba0);const u32 ring=m.c.a[0];
        u32 ring_object=0;unsigned elapsed=0,palette_changes=0,blink_off=0,blink_on=0;
        for(;elapsed<duration && !m.stop;++elapsed) {
            m.c.sp=stack;m.c.a[13]=0x01000000;
            for(unsigned visits=0;visits<240;++visits) {
                const u32 next=rlong(m,m.c.a[13]);if(!next)break;
                const u16 pid=m.rw(next+0x20);
                if(pid==247 || pid==248){m.c.a[13]=next;continue;}
                check(pid==273 || pid==259+owner || pid==244+owner,"Full ring scan found unexpected child PID");
                u32 pc=0xffe0a500;pc=l8_run(m,pc,m.c.cyc+1);
                for(unsigned n=0;n<30000 && pc!=0xffe0a500 && !m.stop;++n)pc=l8_run(m,pc,m.c.cyc+1);
                check(pc==0xffe0a500 && !m.stop && m.faults.empty() && m.c.sp==stack,"Full ring child failed scheduler boundary");
            }
            if(elapsed==0) {
                ring_object=rlong(m,ring+0xa0); // Saved A9 after actual creation and first sleep.
                check(ring_object>=0x010003a0 && ring_object<0x01033160 && (ring_object-0x010003a0)%0x1f0==0,
                      "Full ring creation returned non-object address");
                check(m.rw(ring_object+0x150)==0x4103 && rlong(m,ring_object+0x160)==ring,
                      "Created ring OID/process link differs");
                check(rb(m,owners[owner]+0x230)==1 && rb(m,owners[1-owner]+0x230)==0,
                      "Ring creation changed wrong player's boundary");
                check(m.rw(ring_object+0x110)==m.rw(objects[owner]+0x110),"Ring palette belongs to wrong player");
            }
            const auto palette_before=m.palette;call(0xffe09b70);
            if(m.palette!=palette_before)++palette_changes;
            const unsigned remaining=duration-elapsed-1;
            check(m.rw(ring+0x170)==remaining,"Full ring timer differs");
            if(remaining>0 && remaining<=100) {
                const u16 display=m.rw(ring_object+0xb0)&15;
                check(display==(((100-remaining)/5)%2?2:0),"Blink five-visit off/on cadence differs");
                if(display==0)++blink_off;else ++blink_on;
            }
            check(m.rw(objects[0]+0x150)==0x4101 && m.rw(objects[1]+0x150)==0x4102 &&
                  rlong(m,objects[0]+0x160)==owners[0] && rlong(m,objects[1]+0x160)==owners[1],
                  "Ring children changed player object identity");
        }
        check(elapsed==duration && palette_changes>2 && blink_off==50 && blink_on==50,
              "Full ring palette cycle/blinking/expiry not observed");
        check(rlong(m,0x01000020)==ring && rlong(m,0x01000060)==ring_object && !(m.rw(ring_object+0xb0)&0x8000),
              "Full ring process/object failed release");
        unsigned count=0;
        for(u32 p=rlong(m,0x01000000);p && count<240;p=rlong(m,p),++count)
            check(p==owners[0] || p==owners[1],"Full ring left a palette/blink child alive");
        check(count==2 && rb(m,owners[owner]+0x230)==0 && rb(m,owners[owner]+0x238)==0,
              "Full ring did not restore owner/list state");
    }
    // RINGON uses the configured duration to create a new process. It does not
    // mutate an existing ring's timer. This is a caller-conditional contract,
    // not a claim that overlapping pickups are reachable in ordinary play.
    unsigned ring_pickups=0;
    for(bool wide:{false,true})for(unsigned owner=0;owner<2;++owner)for(unsigned difficulty=0;difficulty<11;++difficulty)
        for(unsigned existing=0;existing<3;++existing)for(bool suppressed:{false,true}) {
            ++ring_pickups;
            Machine m=wide?enhanced_loaded:loaded;m.external_timers=true;m.c.st=0x830;
            auto call=[&](u32 entry) {
                m.c.sp=stack;push(m,m.c,returned);u32 pc=entry;
                for(unsigned n=0;n<20000 && pc!=returned && !m.stop;++n)pc=l8_run(m,pc,m.c.cyc+1);
                check(pc==returned && !m.stop && m.faults.empty() && m.c.sp==stack,"Ring pickup/setup failed bounded return");
            };
            auto create=[&](u16 pid,u32 wake) {
                m.c.a[13]=0x01000000;m.c.a[1]=pid;m.c.a[7]=wake;call(0xffe0aba0);return m.c.a[0];
            };
            call(0xffe0a7d0);u32 owners[2]{};
            for(unsigned n=0;n<2;++n){owners[n]=create(u16(247+n),returned);wb(m,owners[n]+0x1d8,n+1);}
            u32 old_ring=0;std::array<u16,90> old_record{};
            if(existing) {
                old_ring=create(273,0xffe10f80);wb(m,old_ring+0x138,existing==1?owner+1:2-owner);m.ww(old_ring+0x170,17);
                for(unsigned n=0;n<old_record.size();++n)old_record[n]=m.rw(old_ring+n*16);
            }
            wlong(m,player_image+0x160,owners[owner]);wlong(m,0x01000040,item);wlong(m,item,0);
            m.ww(item+0xb0,0x8123);wlong(m,0x01000060,free_node);wlong(m,free_node,0);
            const u16 duration=m.rw(0xffe10670+difficulty*16);
            m.ww(0x010de2f0,duration);m.ww(0x010eeab0,0xa500);m.ww(0x010865f0,suppressed?1:0);
            std::vector<std::pair<u32,u16>> sound_writes;
            m.on_sound_write=[&](u32 address,u16 value){sound_writes.emplace_back(address,value);};
            for(unsigned n=0;n<15;++n){m.c.a[n]=0xa0000000+n;m.c.b[n]=0xb0000000+n;}
            m.c.a[0]=player_image;m.c.a[8]=item;m.c.a[13]=owners[owner];
            const auto saved_a=std::to_array(m.c.a),saved_b=std::to_array(m.c.b);
            call(0xffe2fd40);
            unsigned fresh=0,count=0;
            for(u32 p=rlong(m,0x01000000);p && count<240;p=rlong(m,p),++count) {
                if(p==owners[0] || p==owners[1] || p==old_ring)continue;
                ++fresh;
                check(m.rw(p+0x20)==273 && m.rw(p+0x30)==1 && rlong(m,p+0xe0)==0xffe10af0,
                      "Ring pickup spawned wrong process/entry");
                check(rlong(m,p+0xa0)==owner+1 && rlong(m,p+0x80)==duration,
                      "Ring pickup duration/player parameters differ");
            }
            check(fresh==1 && count==3+unsigned(existing!=0),"Ring pickup altered existing process population");
            if(old_ring)for(unsigned n=0;n<old_record.size();++n)
                check(m.rw(old_ring+n*16)==old_record[n],"Ring pickup changed an existing ring record");
            check(rlong(m,0x01000040)==0 && rlong(m,0x01000060)==item && rlong(m,item)==free_node &&
                  m.rw(item+0xb0)==0x123 && m.rw(0x010eeab0)==0xa501,"Ring pickup icon cleanup/flag differs");
            check(m.c.a[0]==0xffe62970,"Ring pickup final sound descriptor differs");
            for(unsigned n=1;n<15;++n)check(m.c.a[n]==(n==10?duration:saved_a[n]),"Ring pickup register contract differs");
            for(unsigned n=0;n<15;++n)check(m.c.b[n]==saved_b[n],"Ring pickup changed B register");
            check(sound_writes==(suppressed?std::vector<std::pair<u32,u16>>{}:
                std::vector<std::pair<u32,u16>>{{0x01e00000,0xfd92},{0x01e00000,0xff92}}),
                "Ring pickup ICSND/suppression differs");
        }
    // Run the complete sector/list/box scanner and indirect pickup dispatch.
    // Synthetic rectangles deliberately disable pixel scanning; natural sprite
    // masks and full-game collision reachability remain separate obligations.
    constexpr unsigned pickup_subtypes[]={2,3,5,7,9,6};
    constexpr unsigned pickup_modes[]={1,2,3,4,0,0};
    unsigned collision_cases=0;
    for(bool wide:{false,true})for(unsigned order=0;order<2;++order)
        for(unsigned pickup=0;pickup<6;++pickup)for(unsigned layout=0;layout<8;++layout) {
            ++collision_cases;
            Machine m=wide?enhanced_loaded:loaded;m.external_timers=true;m.c.st=0x830;
            auto call=[&](u32 entry) {
                m.c.sp=stack;push(m,m.c,returned);u32 pc=entry;
                for(unsigned n=0;n<100000 && pc!=returned && !m.stop;++n)pc=l8_run(m,pc,m.c.cyc+1);
                check(pc==returned && !m.stop && m.faults.empty() && m.c.sp==stack,"Collision scanner/setup failed bounded return");
            };
            auto create=[&](u16 pid) {
                m.c.a[13]=0x01000000;m.c.a[1]=pid;m.c.a[7]=returned;call(0xffe0aba0);return m.c.a[0];
            };
            call(0xffe0a7d0);u32 owners[2]{},legs[2]{};
            for(unsigned n=0;n<2;++n) {
                owners[n]=create(u16(247+n));legs[n]=create(u16(247+n));
                wb(m,owners[n]+0x1d8,n+1);wlong(m,owners[n]+0x2c0,legs[n]);
                wlong(m,nodes[n]+0x160,owners[n]);
                m.ww(nodes[n]+0x150,u16(layout==7 && n==0?0x4103:0x4101+n));
                m.ww(nodes[n]+0xb0,0x8002);m.ww(nodes[n]+0xf0,16);m.ww(nodes[n]+0x100,16);
                const unsigned x=layout==1?(n==1?100:200):layout==2?100:layout==3?200+n*40:n==0?100:200;
                wlong(m,nodes[n]+0x60,x<<16);wlong(m,nodes[n]+0x80,100<<16);
            }
            const u32 scanner=create(0),icon=nodes[2];
            m.c.a[13]=scanner;
            m.ww(icon+0x150,u16(0x0c00|pickup_subtypes[pickup]));
            m.ww(icon+0xb0,layout==6?0x8802:0x8002);m.ww(icon+0xf0,16);m.ww(icon+0x100,16);
            const unsigned icon_x=layout==4?116:layout==5?117:100;
            wlong(m,icon+0x60,icon_x<<16);wlong(m,icon+0x80,100<<16);
            wlong(m,0x01000040,nodes[order]);wlong(m,nodes[order],nodes[1-order]);
            wlong(m,nodes[1-order],icon);wlong(m,icon,0);wlong(m,0x01000060,free_node);wlong(m,free_node,0);
            m.ww(0x010865f0,1);m.ww(0x010de2f0,300);
            const bool collected=layout<=2 || layout==4;
            const unsigned owner=layout==1?1:layout==2?order:0;
            for(unsigned scan=0;scan<2;++scan) {
                m.c.a[13]=scanner;call(0xffe54770);
                for(unsigned n=0;n<2;++n) {
                    check(rf(m,owners[n]+0x198,0)==(collected && n==owner?pickup_modes[pickup]:0),
                          "Collision pickup assigned weapon to wrong player");
                    check(m.rw(legs[n]+0x180)==(collected && n==owner && pickup==4?300:0),
                          "Collision pickup assigned shoes to wrong player");
                }
                unsigned children=0,count=0;
                for(u32 p=rlong(m,0x01000000);p && count<240;p=rlong(m,p),++count) {
                    if(p==scanner || p==owners[0] || p==owners[1] || p==legs[0] || p==legs[1])continue;
                    ++children;
                    if(pickup==5)check(m.rw(p+0x20)==273 && rlong(m,p+0xe0)==0xffe10af0 &&
                        rlong(m,p+0xa0)==owner+1 && rlong(m,p+0x80)==300,"Collision ring child has wrong owner/entry");
                    else check(m.rw(p+0x20)==0 && rlong(m,p+0xe0)==0xffe27fd0 &&
                        rlong(m,p+0x80)==owners[owner],"Collision weapon refill has wrong owner/entry");
                }
                const unsigned expected=unsigned(collected && pickup!=4);
                check(children==expected && count==5+expected,"Collision scanner duplicated pickup or changed owner processes");
                check(rlong(m,0x01000060)==(collected?icon:free_node) &&
                      bool(m.rw(icon+0xb0)&0x8000)==!collected,"Collision item consumption/rejection differs");
                check(rlong(m,0x01000040)==nodes[order] && rlong(m,nodes[order])==nodes[1-order] &&
                      rlong(m,nodes[1-order])==(collected?0:icon),"Collision scanner corrupted surviving object links");
            }
        }
    // EXTRA awards immediately, but its icon survives as a non-collidable flying
    // object until KOFFC removes it. Exercise both display boundary and cleanup.
    unsigned extra_cases=0;
    for(bool wide:{false,true})for(unsigned owner=0;owner<2;++owner)
        for(unsigned lives:{0u,8u,9u,10u})for(unsigned total:{0u,254u,255u})
            for(bool rackup:{false,true})for(bool arrived:{false,true}) {
                ++extra_cases;
                Machine m=wide?enhanced_loaded:loaded;m.external_timers=true;m.c.st=0x830;
                auto call=[&](u32 entry) {
                    m.c.sp=stack;push(m,m.c,returned);u32 pc=entry;
                    for(unsigned n=0;n<20000 && pc!=returned && !m.stop;++n)pc=l8_run(m,pc,m.c.cyc+1);
                    check(pc==returned && !m.stop && m.faults.empty() && m.c.sp==stack,"Extra life/setup failed bounded return");
                };
                call(0xffe0a7d0);
                m.c.a[13]=0x01000000;m.c.a[1]=247+owner;m.c.a[7]=returned;call(0xffe0aba0);
                const u32 parent=m.c.a[0];wb(m,parent+0x1d8,owner+1);
                constexpr u32 life_fields[]={0x01086360,0x01086520};
                constexpr u32 cash_fields[]={0x010ee568,0x010ee590};
                constexpr u32 displays[]={0x01005000,0x01005200};
                for(unsigned n=0;n<2;++n) {
                    m.ww(life_fields[n],u16(n==owner?lives:6));
                    wf(m,cash_fields[n],n?17:11,0);
                    wlong(m,0x010ee0e0+n*32,displays[n]);
                    wlong(m,displays[n]+0x130,0xfffba320);m.ww(displays[n]+0xb0,0x8002);
                }
                wlong(m,player_image+0x160,parent);wlong(m,0x01000040,item);wlong(m,item,0);
                wlong(m,0x01000060,free_node);wlong(m,free_node,0);
                m.ww(item+0xb0,0x8002);m.ww(item+0x150,0x0c04);
                wlong(m,item+0x60,100u<<16);wlong(m,item+0x80,(arrived?34u:100u)<<16);
                wlong(m,0x010ee080,0);m.ww(0x010f2ba0,rackup?1:0);
                m.ww(0x010eea30,u16(0xb600|total));m.ww(0x010eeab0,0xa500);
                m.c.a[0]=player_image;m.c.a[8]=item;m.c.a[13]=parent;call(0xffe27880);
                check(m.rw(life_fields[owner])==lives+1 && m.rw(life_fields[1-owner])==6,
                      "Extra life awarded to wrong player or incorrectly capped");
                check(m.rw(0x010eea30)==u16(0xb600|((total+1)&255)) && m.rw(0x010eeab0)==0xa501,
                      "Extra life byte counters changed adjacent fields");
                for(unsigned n=0;n<2;++n) {
                    const u32 image=n==owner && lives<9?0xfffba320+(lives+1)*128:0xfffba320;
                    check(rlong(m,displays[n]+0x130)==image,"Extra life display boundary/owner differs");
                    check(rf(m,cash_fields[n],0)==(n?17u:11u)+(n==owner && !rackup?2:0),
                          "Extra life flight counter changed wrong owner or rackup state");
                }
                check(m.rw(item+0x150)==0 && m.rw(item+0xb0)==0x8802 && m.rw(item+0xa0)==200 &&
                      rlong(m,0x01000040)==item && rlong(m,0x01000060)==free_node,
                      "Extra life icon failed non-collidable flight conversion");
                u32 flight=0,sound=0;unsigned count=0;
                for(u32 p=rlong(m,0x01000000);p && count<240;p=rlong(m,p),++count) {
                    if(p==parent)continue;
                    if(rlong(m,p+0xe0)==0xffe2d580)flight=p;
                    if(rlong(m,p+0xe0)==0xffe27dc0)sound=p;
                }
                check(count==3 && flight && sound && m.rw(flight+0x20)==263 &&
                      rlong(m,flight+0xc0)==item && rlong(m,flight+0xa0)==owner+1,
                      "Extra life spawned wrong flight/sound process or owner");
                unsigned visits=0;
                for(;visits<182;++visits) {
                    u32 previous=0x01000000;unsigned links=0;
                    while(rlong(m,previous) && rlong(m,previous)!=flight && links++<240)previous=rlong(m,previous);
                    if(rlong(m,previous)!=flight)break;
                    m.c.sp=stack;m.c.a[13]=previous;u32 pc=l8_run(m,0xffe0a500,m.c.cyc+1);
                    for(unsigned n=0;n<20000 && pc!=0xffe0a500 && !m.stop;++n)pc=l8_run(m,pc,m.c.cyc+1);
                    check(pc==0xffe0a500 && !m.stop && m.faults.empty() && m.c.sp==stack,
                          "Extra life flight failed scheduler boundary");
                }
                check(visits==(arrived?11u:181u) && rlong(m,0x01000040)==0 &&
                      rlong(m,0x01000060)==item && !(m.rw(item+0xb0)&0x8000),
                      "Extra life flight failed arrival/timeout cleanup");
                check(m.rw(life_fields[owner])==lives+1 && m.rw(life_fields[1-owner])==6,
                      "Extra life cleanup awarded twice or changed other player");
            }
    std::printf("%u extra-life award/flight cases\n",extra_cases);
    // Cash, keys and floor items share flight presentation, but only keys/items
    // score here; floor items also retire their original process in KOFFC2.
    unsigned reward_cases=0;
    constexpr u32 reward_entries[]={0xffe2cff0,0xffe2ff60,0xffe2cda0};
    auto packed_bcd=[](unsigned value) {
        u32 result=0;for(unsigned shift=0;shift<32;shift+=4){result|=(value%10)<<shift;value/=10;}return result;
    };
    for(bool wide:{false,true})for(unsigned owner=0;owner<2;++owner)for(unsigned kind=0;kind<3;++kind)
        for(bool coop:{false,true})for(unsigned initial:{0u,485000u,498750u})
            for(bool attract:{false,true})for(bool rackup:{false,true})for(bool arrived:{false,true}) {
                ++reward_cases;
                Machine m=wide?enhanced_loaded:loaded;m.external_timers=true;m.c.st=0x830;
                auto call=[&](u32 entry) {
                    m.c.sp=stack;push(m,m.c,returned);u32 pc=entry;
                    for(unsigned n=0;n<30000 && pc!=returned && !m.stop;++n)pc=l8_run(m,pc,m.c.cyc+1);
                    check(pc==returned && !m.stop && m.faults.empty() && m.c.sp==stack,"Reward/setup failed bounded return");
                };
                call(0xffe0a7d0);
                auto create=[&](unsigned pid) {
                    m.c.a[13]=0x01000000;m.c.a[1]=pid;m.c.a[7]=returned;call(0xffe0aba0);return m.c.a[0];
                };
                const u32 parent=create(247+owner),icon_process=create(254);
                wb(m,parent+0x1d8,owner+1);
                constexpr u32 data[]={0x01086220,0x010863e0};
                constexpr u32 cash[]={0x010ee568,0x010ee590};
                for(unsigned n=0;n<2;++n) {
                    wlong(m,data[n]+0x40,packed_bcd(n==owner?initial:123456));
                    wlong(m,data[n]+0x120,0x00500000);m.ww(data[n]+0x140,9);
                    wf(m,cash[n],n?17:11,0);
                }
                wlong(m,player_image+0x160,parent);wlong(m,item+0x160,icon_process);
                wlong(m,0x01000040,item);wlong(m,item,0);wlong(m,0x01000060,free_node);wlong(m,free_node,0);
                m.ww(item+0xb0,0x8002);m.ww(item+0x150,u16(0x0c00+(kind==0?14:kind==1?8:15)));
                wlong(m,item+0x60,100u<<16);wlong(m,item+0x80,(arrived?34u:100u)<<16);
                wlong(m,0x010ee080,0);m.ww(0x010f2ba0,rackup?1:0);
                m.ww(0x010865a0,attract?0x30:0x10);m.ww(0x010ee600,coop?3:owner+1);
                m.ww(0x010865f0,1);m.ww(0x010eeab0,0xa500);
                m.c.a[0]=player_image;m.c.a[8]=item;m.c.a[13]=parent;call(reward_entries[kind]);
                const unsigned score=initial+(attract?0:(kind==1?15000:kind==2?1250:0)*(coop?2:1));
                const bool extended=!attract && kind!=0 && score>=500000;
                for(unsigned n=0;n<2;++n) {
                    check(rlong(m,data[n]+0x40)==packed_bcd(n==owner?score:123456),"Reward score amount/doubling/owner differs");
                    check(m.rw(data[n]+0x140)==9+unsigned(n==owner && extended),"Reward score extension changed wrong lives");
                    check(rlong(m,data[n]+0x120)==(n==owner && extended?0x02000000u:0x00500000u),
                          "Reward score extension threshold differs");
                    check(rf(m,cash[n],0)==(n?17u:11u)+(n==owner && !rackup?2:0),"Reward flight counter differs");
                }
                check(m.rw(0x010eeab0)==(kind==1?0xa501:0xa500),"Reward changed wrong icon flag byte");
                check(m.rw(item+0x150)==0 && m.rw(item+0xb0)==0x8802 && m.rw(item+0xa0)==200,
                      "Reward icon failed non-collidable flight conversion");
                u32 flight=0;unsigned count=0;
                for(u32 p=rlong(m,0x01000000);p && count<240;p=rlong(m,p),++count)
                    if(rlong(m,p+0xe0)==(kind==2?0xffe2d720u:0xffe2d580u))flight=p;
                check(count==3+unsigned(extended) && flight && m.rw(flight+0x20)==263 &&
                      rlong(m,flight+0xc0)==item && rlong(m,flight+0xa0)==owner+1,
                      "Reward spawned wrong flight/extension process or owner");
                unsigned visits=0;
                for(;visits<182;++visits) {
                    u32 previous=0x01000000;unsigned links=0;
                    while(rlong(m,previous) && rlong(m,previous)!=flight && links++<240)previous=rlong(m,previous);
                    if(rlong(m,previous)!=flight)break;
                    m.c.sp=stack;m.c.a[13]=previous;u32 pc=l8_run(m,0xffe0a500,m.c.cyc+1);
                    for(unsigned n=0;n<30000 && pc!=0xffe0a500 && !m.stop;++n)pc=l8_run(m,pc,m.c.cyc+1);
                    check(pc==0xffe0a500 && !m.stop && m.faults.empty() && m.c.sp==stack,"Reward flight failed scheduler boundary");
                }
                bool owner_alive=false,icon_alive=false;count=0;
                for(u32 p=rlong(m,0x01000000);p && count<240;p=rlong(m,p),++count) {
                    owner_alive|=p==parent;icon_alive|=p==icon_process;
                }
                check(owner_alive && icon_alive==(kind!=2) && count==2-unsigned(kind==2)+unsigned(extended),
                      "Reward flight killed wrong process or retained original floor-item process");
                check(visits==(arrived?11u:181u) && rlong(m,0x01000040)==0 &&
                      rlong(m,0x01000060)==item && !(m.rw(item+0xb0)&0x8000),"Reward flight cleanup differs");
                check(rlong(m,data[owner]+0x40)==packed_bcd(score) && m.rw(data[owner]+0x140)==9+unsigned(extended),
                      "Reward flight awarded score or life twice");
            }
    std::printf("%u cash/key/item reward and cleanup cases\n",reward_cases);
    unsigned pow_cases=0;
    for(bool wide:{false,true})for(unsigned owner=0;owner<2;++owner)for(unsigned kind=0;kind<2;++kind) {
        ++pow_cases;
        Machine m=wide?enhanced_loaded:loaded;m.external_timers=true;m.c.st=0x830;
        auto call=[&](u32 entry) {
            m.c.sp=stack;push(m,m.c,returned);u32 pc=entry;
            for(unsigned n=0;n<50000 && pc!=returned && !m.stop;++n)pc=l8_run(m,pc,m.c.cyc+1);
            check(pc==returned && !m.stop && m.faults.empty() && m.c.sp==stack,"POW/setup failed bounded return");
        };
        call(0xffe0a7d0);call(0xffe02750);call(0xffe09000);
        u32 owners[2]{},objects[2]{};
        for(unsigned n=0;n<2;++n) {
            m.c.a[13]=0x01000000;m.c.a[1]=247+n;m.c.a[7]=returned;call(0xffe0aba0);owners[n]=m.c.a[0];
            wb(m,owners[n]+0x1d8,n+1);wlong(m,0x010de2b0+n*32,owners[n]);
            m.c.a[13]=owners[n];m.c.a[0]=(100+n*40)<<16;m.c.a[1]=120<<16;
            m.c.a[2]=0xfffae3a0;m.c.a[3]=100;m.c.a[4]=0x8002;m.c.a[5]=0x4101+n;m.c.a[6]=m.c.a[7]=0;
            call(0xffe04170);objects[n]=m.c.a[8];wlong(m,owners[n]+0x100,objects[n]);
        }
        m.c.a[13]=owners[owner];m.c.a[0]=100<<16;m.c.a[1]=120<<16;m.c.a[2]=0xfffae3a0;
        m.c.a[3]=100;m.c.a[4]=0x8002;m.c.a[5]=0x0c00+kind;m.c.a[6]=m.c.a[7]=0;
        call(0xffe04170);const u32 icon=m.c.a[8];
        m.ww(0x010865f0,1);m.ww(0x010865a0,16);
        m.c.a[0]=objects[owner];m.c.a[8]=icon;m.c.a[13]=owners[owner];call(kind?0xffed3ba0:0xffed25d0);
        check(rlong(m,0x01000060)==icon && !(m.rw(icon+0xb0)&0x8000),"POW pickup failed icon consumption");
        auto visit=[&]() {
            m.c.sp=stack;m.c.a[13]=0x01000000;
            for(unsigned n=0;n<240;++n) {
                const u32 next=rlong(m,m.c.a[13]);if(!next)break;
                if(next==owners[0] || next==owners[1]){m.c.a[13]=next;continue;}
                u32 pc=l8_run(m,0xffe0a500,m.c.cyc+1);
                for(unsigned j=0;j<50000 && pc!=0xffe0a500 && !m.stop;++j)pc=l8_run(m,pc,m.c.cyc+1);
                if(pc!=0xffe0a500 || m.stop || !m.faults.empty() || m.c.sp!=stack) {
                    std::fprintf(stderr,"POW boundary diagnostic wide=%u owner=%u kind=%u process=%08x pc=%08x sp=%08x stop=%u faults=%zu\n",
                        unsigned(wide),owner,kind,next,pc,m.c.sp,unsigned(m.stop),m.faults.size());
                    return false;
                }
                check(pc==0xffe0a500 && !m.stop && m.faults.empty() && m.c.sp==stack,"POW child failed scheduler boundary");
            }
            return true;
        };
        bool spawned=true;for(unsigned n=0;n<60 && spawned;++n)spawned=visit();
        check(spawned,"POW spawn/animation failed scheduler boundary");if(!spawned)continue;
        std::vector<std::pair<u32,u32>> effects;
        unsigned count=0;
        for(u32 p=rlong(m,0x01000000);p && count<240;p=rlong(m,p),++count) {
            if(p==owners[0] || p==owners[1])continue;
            const u32 object=rlong(m,p+0xc0);effects.emplace_back(p,object);
            check(m.rw(p+0x20)==(kind?231:230) && rlong(m,object+0x160)==p && (m.rw(object+0xb0)&0x8000),
                  "POW effect object/process linkage differs");
            if(kind)check(rlong(m,p+0x180)==objects[owner] && rlong(m,owners[owner]+0x2e0)==object,
                          "Helper linked to wrong player");
            else check(rb(m,p+0x138)==owner+1 && rf(m,p+0x1a8,1)==owners[owner] && rb(m,p+0x1a0)==0,
                       "Blade linked to wrong player or starts with hits");
        }
        check(effects.size()==(kind?1:5) && count==2+effects.size(),"POW failed exact effect population/spawner retirement");
        check(rb(m,owners[owner]+0x230)==(kind?1:2) && rb(m,owners[1-owner]+0x230)==0 &&
              rlong(m,owners[1-owner]+0x2e0)==0,"POW boundary changed wrong player");
        for(const auto& [process,object]:effects) {
            const u32 prior_wake=rlong(m,process+0xe0);
            const unsigned detach_hits=kind?1u:(wide?7u:5u);
            for(unsigned hit=1;hit<=detach_hits;++hit) {
                m.c.a[13]=owners[owner];m.c.a[8]=object;call(kind?0xffed4ec0:0xffed3890);
                if(!kind)check(rb(m,process+0x1a0)==hit &&
                    rlong(m,process+0xe0)==(hit<detach_hits?prior_wake:0xffed2e40),"Blade detached at the wrong hit count");
            }
            if(kind)check(m.rw(object+0x150)==0 && rlong(m,owners[owner]+0x2e0)==0 &&
                rlong(m,process+0xe0)==0xffed42f0,"Helper hit failed pointer/dispatch reset");
            // Isolate the off-screen retirement path; movement to this position
            // and a natural collision trace remain separate coverage obligations.
            wlong(m,object+0x60,1000u<<16);wlong(m,object+0x80,1000u<<16);
        }
        for(unsigned n=0;n<3;++n)check(visit(),"POW cleanup failed scheduler boundary");
        count=0;
        for(u32 p=rlong(m,0x01000000);p && count<240;p=rlong(m,p),++count)
            check(p==owners[0] || p==owners[1],"POW left child process after off-screen cleanup");
        check(count==2 && rb(m,owners[owner]+0x230)==0 && rb(m,owners[owner]+0x238)==0 &&
              rb(m,owners[owner]+0x218)==0,"POW did not restore normal owner boundary/flags");
        unsigned object_count=0;
        for(u32 p=rlong(m,0x01000040);p && object_count<420;p=rlong(m,p),++object_count)
            check(p==objects[0] || p==objects[1],"POW left effect object after cleanup");
        check(object_count==2,"POW cleanup removed player object");
    }
    std::printf("%u POW spawn/hit/cleanup lifecycles\n",pow_cases);
    unsigned bomb_cases=0;
    for(bool wide:{false,true})for(unsigned owner=0;owner<2;++owner)for(bool coop:{false,true})
        for(unsigned copies:{0u,1u,2u})for(bool shake:{false,true}) {
            ++bomb_cases;
            Machine m=wide?enhanced_loaded:loaded;m.external_timers=true;m.c.st=0x830;
            auto call=[&](u32 entry) {
                m.c.sp=stack;push(m,m.c,returned);u32 pc=entry;
                for(unsigned n=0;n<50000 && pc!=returned && !m.stop;++n)pc=l8_run(m,pc,m.c.cyc+1);
                check(pc==returned && !m.stop && m.faults.empty() && m.c.sp==stack,"Bomb/setup failed bounded return");
            };
            call(0xffe0a7d0);call(0xffe02750);call(0xffe09000);
            auto process=[&](unsigned pid) {
                m.c.a[13]=0x01000000;m.c.a[1]=pid;m.c.a[7]=returned;call(0xffe0aba0);return m.c.a[0];
            };
            auto object=[&](u32 proc,u16 oid) {
                m.c.a[13]=proc;m.c.a[0]=100<<16;m.c.a[1]=120<<16;m.c.a[2]=0xfffae3a0;
                m.c.a[3]=100;m.c.a[4]=0x8002;m.c.a[5]=oid;m.c.a[6]=m.c.a[7]=0;call(0xffe04170);return m.c.a[8];
            };
            u32 owners[2]{},players[2]{};
            for(unsigned n=0;n<2;++n) {
                owners[n]=process(247+n);wb(m,owners[n]+0x1d8,n+1);players[n]=object(owners[n],u16(0x4101+n));
                wlong(m,owners[n]+0x100,players[n]);
                const u32 data=n?0x010863e0:0x01086220;
                wlong(m,data+0x40,packed_bcd(1230));wlong(m,data+0x120,0x00500000);
            }
            const u32 neutral_process=process(255),neutral=object(neutral_process,1);
            wb(m,neutral_process+0x100,1); // Live tank-driver owner for torso unlinking.
            for(unsigned n=0;n<copies;++n)for(u16 oid:{u16(0x8600),u16(0x9000),u16(0x8300),u16(0x9500),
                u16(0x0e00),u16(0x8e00),u16(0x9100),u16(0x8400),u16(0x8902),u16(0x8903),u16(0x8904),u16(0x9200)}) {
                const u32 p=process(254);object(p,oid);
                if(oid==0x9100)wlong(m,p+0x100,neutral_process);
                if(oid==0x8400 && copies==2) {
                    const u32 laser=process(254),laser_object=object(laser,0x8500);
                    wf(m,p+0x110,laser,1);wlong(m,laser+0xc0,laser_object);
                }
            }
            constexpr u32 population[]={0x010de5d8,0x010eeb20,0x010f4550,0x010eead0,0x010f3178,0x010f45b8};
            for(unsigned n=0;n<6;++n)wf(m,population[n],copies*(n==3?2:n==5?3:1),0);
            const u32 icon=object(owners[owner],0x0c0a);
            m.ww(0x010865f0,1);m.ww(0x010865a0,16);m.ww(0x010ee600,coop?3:owner+1);
            wb(m,0x010eea48,shake?0:1);
            m.ww(0x010eeab0,0xa500);m.c.a[13]=owners[owner];m.c.a[0]=players[owner];m.c.a[8]=icon;
            call(0xffe30450);
            check(rlong(m,0x01000060)==icon && !(m.rw(icon+0xb0)&0x8000) && m.rw(0x010eeab0)==0xa501,
                  "Bomb pickup failed icon/flag transaction");
            unsigned completed=0,sound_end=0;
            for(unsigned tick=1;tick<=360;++tick) {
                m.c.sp=stack;m.c.a[13]=0x01000000;bool valid=true;
                for(unsigned n=0;n<240;++n) {
                    const u32 next=rlong(m,m.c.a[13]);if(!next)break;
                    const unsigned pid=m.rw(next+0x20);
                    if(next==owners[0] || next==owners[1] || next==neutral_process || pid==254){m.c.a[13]=next;continue;}
                    u32 pc=l8_run(m,0xffe0a500,m.c.cyc+1);
                    for(unsigned j=0;j<50000 && pc!=0xffe0a500 && !m.stop;++j)pc=l8_run(m,pc,m.c.cyc+1);
                    if(pc!=0xffe0a500 || m.stop || !m.faults.empty() || m.c.sp!=stack) {
                        std::fprintf(stderr,"Bomb boundary wide=%u owner=%u copies=%u tick=%u pc=%08x stop=%u\n",
                            unsigned(wide),owner,copies,tick,pc,unsigned(m.stop));valid=false;break;
                    }
                }
                check(valid,"Bomb child failed scheduler boundary");if(!valid)break;
                unsigned count=0;bool sound_alive=false;
                for(u32 p=rlong(m,0x01000000);p && count<240;p=rlong(m,p),++count) {
                    const u32 wake=rlong(m,p+0xe0);
                    sound_alive|=wake>=0xffe30630 && wake<0xffe30980;
                }
                if(!sound_alive && !sound_end)sound_end=tick;
                if(count==3){completed=tick;break;}
            }
            check(completed>0 && sound_end==33,"Bomb/sound/animation processes failed completion timing");
            check(rb(m,0x010eea48)==(shake?0:1),"Bomb shake failed retirement or changed existing shake flag");
            unsigned count=0;
            for(u32 p=rlong(m,0x01000000);p && count<240;p=rlong(m,p),++count)
                check(p==owners[0] || p==owners[1] || p==neutral_process,"Bomb retained killed enemy or effect process");
            check(count==3,"Bomb removed unrelated owner process");
            count=0;
            for(u32 p=rlong(m,0x01000040);p && count<420;p=rlong(m,p),++count)
                check(p==players[0] || p==players[1] || p==neutral,"Bomb retained enemy/projectile/effect object");
            check(count==3 && m.rw(neutral+0x150)==1,"Bomb removed unrelated object");
            check(rb(m,neutral_process+0x100)==(copies?0:1),"Bomb torso cleanup failed driver flag unlink");
            for(u32 address:population)check(rf(m,address,0)==0,"Bomb failed enemy population cleanup");
            for(unsigned n=0;n<2;++n) {
                const unsigned expected=1230+(n==owner?copies*(500+2750+1050+2500+3*2000+1000)*(coop?2:1):0);
                check(rlong(m,(n?0x010863e0:0x01086220)+0x40)==packed_bcd(expected),"Bomb points or scoring player differs");
            }
        }
    std::printf("%u smart-bomb mixed-enemy lifecycles\n",bomb_cases);
    unsigned prize_cases=0;
    constexpr u32 prize_entries[]={0xffe2d920,0xffe2f9e0,0xffe2fa10};
    for(bool wide:{false,true})for(unsigned owner=0;owner<2;++owner)for(unsigned circuit=0;circuit<3;++circuit)
        for(unsigned kind=0;kind<3;++kind)for(bool coop:{false,true})for(unsigned interruption=0;interruption<4;++interruption) {
            ++prize_cases;
            Machine m=wide?enhanced_loaded:loaded;m.external_timers=true;m.c.st=0x830;
            auto call=[&](u32 entry) {
                m.c.sp=stack;push(m,m.c,returned);u32 pc=entry;
                for(unsigned n=0;n<50000 && pc!=returned && !m.stop;++n)pc=l8_run(m,pc,m.c.cyc+1);
                check(pc==returned && !m.stop && m.faults.empty() && m.c.sp==stack,"Prize/setup failed bounded return");
            };
            call(0xffe0a7d0);call(0xffe02750);call(0xffe09000);
            auto object=[&](u32 proc,u16 oid,unsigned y) {
                m.c.a[13]=proc;m.c.a[0]=100<<16;m.c.a[1]=y<<16;m.c.a[2]=0xfffae3a0;
                m.c.a[3]=100;m.c.a[4]=0x8002;m.c.a[5]=oid;m.c.a[6]=m.c.a[7]=0;call(0xffe04170);return m.c.a[8];
            };
            u32 owners[2]{},players[2]{},hud[2][3]{};
            constexpr u32 hud_fields[]={0x010eb580,0x010ee0e0,0x010ee0a0};
            for(unsigned n=0;n<2;++n) {
                m.c.a[13]=0x01000000;m.c.a[1]=247+n;m.c.a[7]=returned;call(0xffe0aba0);owners[n]=m.c.a[0];
                wb(m,owners[n]+0x1d8,n+1);players[n]=object(owners[n],u16(0x4101+n),120);
                for(unsigned h=0;h<3;++h){hud[n][h]=object(owners[n],0,20);wlong(m,hud_fields[h]+n*32,hud[n][h]);}
                wlong(m,(n?0x010863e0:0x01086220)+0x40,packed_bcd(1230));
                wlong(m,(n?0x010863e0:0x01086220)+0x120,0x00500000);
                for(unsigned k=0;k<3;++k)wb(m,(n?0x010ee578:0x010ee550)+k*8,k==2?250:10+k*10);
            }
            const u32 icon=object(owners[owner],u16(0x0c0b+kind),34);
            m.ww(0x010865f0,1);m.ww(0x010865a0,16);m.ww(0x010ee600,coop?3:owner+1);m.ww(0x010eb610,u16(circuit));
            wf(m,0x010de3b8,3,0);wb(m,0x010ee810,interruption==1?1:0);
            wb(m,0x010de388+owner*8,interruption==2?1:0);wb(m,0x010eb008,0);
            call(0xffe09b70);m.c.a[13]=owners[owner];m.c.a[0]=players[owner];m.c.a[8]=icon;call(prize_entries[kind]);
            for(unsigned n=0;n<2;++n) {
                check(rlong(m,(n?0x010863e0:0x01086220)+0x40)==packed_bcd(1230+(n==owner?5010*(coop?2:1):0)),
                      "Prize score or scoring owner differs");
                for(unsigned k=0;k<3;++k)check(rb(m,(n?0x010ee578:0x010ee550)+k*8)==
                    ((k==2?250:10+k*10)+(n==owner && k==kind?10:0))%256,"Prize counter subtype/owner/wrap differs");
            }
            check(rb(m,0x010eea40)==1 && m.rw(icon+0x150)==0 && m.rw(icon+0xb0)==0x8802,
                  "Prize collection failed flag/non-collidable conversion");
            unsigned completed=0,palette_changes=0;bool displayed=false;
            for(unsigned tick=1;tick<=300;++tick) {
                if(interruption==3 && tick==6)wb(m,0x010ee810,1);
                m.c.sp=stack;m.c.a[13]=0x01000000;bool valid=true;
                for(unsigned n=0;n<240;++n) {
                    const u32 next=rlong(m,m.c.a[13]);if(!next)break;
                    if(next==owners[0] || next==owners[1]){m.c.a[13]=next;continue;}
                    u32 pc=l8_run(m,0xffe0a500,m.c.cyc+1);
                    for(unsigned j=0;j<50000 && pc!=0xffe0a500 && !m.stop;++j)pc=l8_run(m,pc,m.c.cyc+1);
                    if(pc!=0xffe0a500 || m.stop || !m.faults.empty() || m.c.sp!=stack) {
                        std::fprintf(stderr,"Prize boundary wide=%u owner=%u circuit=%u kind=%u exit=%u tick=%u pc=%08x\n",
                            unsigned(wide),owner,circuit,kind,interruption,tick,pc);valid=false;break;
                    }
                }
                check(valid,"Prize child failed scheduler boundary");if(!valid)break;
                if(tick==5 && (interruption==0 || interruption==3)) {
                    displayed=true;
                    for(unsigned n=0;n<2;++n)for(unsigned h=0;h<3;++h)
                        check((m.rw(hud[n][h]+0xb0)&15)==(n==owner?0:2),"Prize hid wrong player's HUD");
                    check(rf(m,0x010de3b8,0)==(3u&~(1u<<owner)),"Prize hid wrong score display");
                    const u32 art=rlong(m,rlong(m,0xffe2fce0+circuit*32)+kind*32);
                    const u32 text=rlong(m,rlong(m,0xffe2fb60+circuit*32)+kind*32);
                    check(rlong(m,icon+0x130)==art,"Prize circuit/type art selection differs");
                    bool text_found=false;unsigned count=0;
                    for(u32 p=rlong(m,0x01000040);p && count<420;p=rlong(m,p),++count)text_found|=rlong(m,p+0x130)==text;
                    check(text_found,"Prize circuit/type text missing");
                }
                const auto before=m.palette;call(0xffe09b70);palette_changes+=m.palette!=before;
                unsigned count=0;for(u32 p=rlong(m,0x01000000);p && count<240;p=rlong(m,p),++count){}
                if(count==2){completed=tick;break;}
            }
            check(completed>0 && displayed==(interruption==0 || interruption==3),"Prize lifecycle/display did not complete");
            if(interruption==0)check(completed>=125 && palette_changes>1,"Prize normal display duration/palette animation absent");
            for(unsigned n=0;n<2;++n)for(unsigned h=0;h<3;++h)check((m.rw(hud[n][h]+0xb0)&15)==2,"Prize failed HUD restoration");
            check(rf(m,0x010de3b8,0)==3 && rb(m,0x010eb008)==0 && rb(m,0x010ee810)==0,
                  "Prize failed score/palette/exit-flag restoration");
            unsigned count=0;
            for(u32 p=rlong(m,0x01000040);p && count<420;p=rlong(m,p),++count) {
                bool retained=p==players[0] || p==players[1];
                for(unsigned n=0;n<2;++n)for(unsigned h=0;h<3;++h)retained|=p==hud[n][h];
                check(retained,"Prize left temporary art/text object");
            }
            check(count==8,"Prize removed a permanent player/HUD object");
        }
    std::printf("%u prize reward/display/cleanup lifecycles\n",prize_cases);
    // Execute retail PIXSCAN, comparing carry against an independent screen-space
    // mask intersection. Synthetic headers/pixels exercise the real graphics bus;
    // they do not establish natural sprite or caller reachability.
    unsigned pixel_cases=0;
    constexpr int offsets[][2]={{0,0},{1,0},{-1,0},{0,1},{0,-1},{4,0},{0,3}};
    for(bool wide:{false,true})for(unsigned width:{3u,4u})
        for(unsigned flip0=0;flip0<4;++flip0)for(unsigned flip1=0;flip1<4;++flip1)
            for(unsigned pattern=0;pattern<4;++pattern)for(const auto& offset:offsets) {
                ++pixel_cases;
                Machine m=wide?enhanced_loaded:loaded;m.external_timers=true;m.c.st=0x830;
                constexpr u32 headers[]={0x01070000,0x01070100};
                constexpr u32 objects[]={item,player};
                const unsigned flips[]={flip0,flip1};
                std::array<std::array<u8,12>,2> masks{};
                if(pattern==1){masks[0][0]=1;masks[1][0]=63;}
                if(pattern==2){masks[0][0]=1;masks[1][10]=63;}
                if(pattern==3){masks[0].fill(1);masks[1].fill(63);}
                for(unsigned n=0;n<2;++n) {
                    wlong(m,objects[n]+0x130,headers[n]);
                    wlong(m,headers[n],(3u<<16)|width);
                    wlong(m,headers[n]+0x40,0x02000000+n*0x100);
                    for(unsigned p=0;p<12;++p)m.gfx[n*32+p]=masks[n][p];
                    m.ww(objects[n]+0x70,u16(100+(n?offset[0]:0)));
                    m.ww(objects[n]+0x90,u16(100+(n?offset[1]:0)));
                    // Test either object's flag and both flags across patterns.
                    const bool pixel_flag=pattern==3 || n==(pattern&1);
                    m.ww(objects[n]+0xb0,u16(0x8002|(flips[n]<<4)|(pixel_flag?0x400:0)));
                }
                bool expected=false;
                auto opaque=[&](unsigned n,int x,int y) {
                    x-=100+(n?offset[0]:0);y-=100+(n?offset[1]:0);
                    if(x<0 || x>=int(width) || y<0 || y>=3)return false;
                    // Retail horizontal addressing reflects the padded row stride.
                    if(flips[n]&1)x=3-x;
                    if(flips[n]&2)y=2-y;
                    return masks[n][y*4+x]!=0;
                };
                for(int y=98;y<107;++y)for(int x=98;x<109;++x)
                    expected|=opaque(0,x,y)&&opaque(1,x,y);
                for(unsigned n=0;n<15;++n){m.c.a[n]=0xa0000000+n;m.c.b[n]=0xb0000000+n;}
                m.c.a[1]=item;m.c.a[2]=player;m.c.b[13]=0;
                const auto saved_a=std::to_array(m.c.a),saved_b=std::to_array(m.c.b);
                m.c.sp=stack;push(m,m.c,returned);u32 pc=0xffe5f700;
                for(unsigned n=0;n<5000 && pc!=returned && !m.stop;++n)pc=l8_run(m,pc,m.c.cyc+1);
                check(pc==returned && !m.stop && m.faults.empty() && m.c.sp==stack,"Pixel scan failed bounded return");
                check(bool(m.c.st&ST_C)==expected,"Pixel scan differs from screen-space mask intersection");
                for(unsigned n=0;n<14;++n)check(m.c.a[n]==saved_a[n],"Pixel scan changed saved A register");
                for(unsigned n=0;n<15;++n)check(m.c.b[n]==saved_b[n],"Pixel scan changed B register");
            }
    // STRT_WPN executes the real allocator, preserving the firing player's
    // identity in PA11. These are creation-boundary contracts; projectile
    // execution, collision and retirement are separate lifetime obligations.
    unsigned firing_cases=0;
    constexpr u32 projectile_entries[]={0xffe371d0,0xffe36d80,0xffe36370,0xffe34e20,0xffe34ae0};
    constexpr u32 helper_entries[]={0xffe370c0,0xffe36cd0,0xffe362e0,0,0xffe34610};
    for(bool wide:{false,true})for(unsigned owner=0;owner<2;++owner)
        for(unsigned mode=0;mode<5;++mode)for(bool helper:{false,true}) {
            ++firing_cases;
            Machine m=wide?enhanced_loaded:loaded;m.external_timers=true;m.c.st=0x830;
            auto call=[&](u32 entry) {
                m.c.sp=stack;push(m,m.c,returned);u32 pc=entry;
                for(unsigned n=0;n<20000 && pc!=returned && !m.stop;++n)pc=l8_run(m,pc,m.c.cyc+1);
                check(pc==returned && !m.stop && m.faults.empty() && m.c.sp==stack,"Firing/setup failed bounded return");
            };
            call(0xffe0a7d0);
            u32 owners[2]{};
            for(unsigned n=0;n<2;++n) {
                m.c.a[13]=0x01000000;m.c.a[1]=247+n;m.c.a[7]=returned;call(0xffe0aba0);
                owners[n]=m.c.a[0];wb(m,owners[n]+0x1d8,n+1);
                wf(m,owners[n]+0x198,n==owner?mode:4,0);
                m.ww(owners[n]+0x220,n==owner?100:999);
                wlong(m,owners[n]+0x2e0,n==owner && helper?free_node:0);
            }
            m.ww(0x010865f0,1); // Sound admission is tested separately.
            const unsigned primary_count=mode==1?3:1;
            const unsigned helper_count=helper && mode!=3?primary_count:0;
            const unsigned spent=mode==0?0:1+unsigned(helper && (mode==1 || mode==4));
            for(unsigned shot=1;shot<=3;++shot) {
                m.c.a[13]=owners[owner];m.c.a[8]=player_image;
                m.c.a[10]=0x13579bdf;m.c.a[11]=0xdeadbeef;call(0xffe337a0);
                check(m.c.a[13]==owners[owner],"Firing changed executing player process");
                check(m.rw(owners[owner]+0x220)==100-shot*spent && m.rw(owners[1-owner]+0x220)==999,
                      "Firing ammo cost/helper policy or other player counter differs");
                check(rf(m,owners[owner]+0x198,0)==mode && rf(m,owners[1-owner]+0x198,0)==4,
                      "Firing changed weapon selector before depletion");
                unsigned primary=0,secondary=0,links=0,spread[2][3]{};
                for(u32 p=rlong(m,0x01000000);p && links<240;p=rlong(m,p),++links) {
                    if(p==owners[0] || p==owners[1])continue;
                    const u32 wake=rlong(m,p+0xe0);
                    const bool extra=wake==helper_entries[mode];
                    check(wake==projectile_entries[mode] || (helper_count && extra),"Firing spawned unexpected projectile entry");
                    check(m.rw(p+0x20)==286 && m.rw(p+0x30)==1,"Firing projectile PID/initial delay differs");
                    check(rlong(m,p+0x60)==owner+1 && rlong(m,p+0xc0)==player_image,
                          "Firing projectile inherited wrong player or image");
                    if(extra)++secondary;else ++primary;
                    if(mode==1) {
                        const u32 variant=rlong(m,p+0x80);
                        check(variant<3,"Spray variant outside three-shot domain");
                        if(variant<3)++spread[extra?1:0][variant];
                    }
                }
                check(links==2+shot*(primary_count+helper_count) && primary==shot*primary_count && secondary==shot*helper_count,
                      "Firing process count or primary/helper multiplicity differs");
                if(mode==1)for(unsigned n=0;n<3;++n)
                    check(spread[0][n]==shot && spread[1][n]==(helper?shot:0),"Spray variants not created exactly once per firing");
            }
        }
    std::printf("%u firing creation cases (three shots each)\n",firing_cases);
    // Execute projectile entries and actual object allocation. Pods/grenades
    // additionally run natural expiry and grenade shadow/arming transitions.
    // Boundary positions are injected; display-driven flight is not simulated.
    unsigned bullet_cases=0,wall_cases=0,bounce_cases=0;
    constexpr unsigned converted[]={0,1,5,6,3,2,4,7,8,6};
    for(bool wide:{false,true})for(unsigned owner=0;owner<2;++owner)
        for(unsigned mode=0;mode<5;++mode)for(bool helper:{false,true})
            for(unsigned direction:{1u,2u,4u,5u,6u,7u,8u,9u}) {
                ++bullet_cases;
                Machine m=wide?enhanced_loaded:loaded;m.external_timers=true;m.c.st=0x830;
                auto call=[&](u32 entry) {
                    m.c.sp=stack;push(m,m.c,returned);u32 pc=entry;
                    for(unsigned n=0;n<50000 && pc!=returned && !m.stop;++n)pc=l8_run(m,pc,m.c.cyc+1);
                    check(pc==returned && !m.stop && m.faults.empty() && m.c.sp==stack,"Bullet/setup failed bounded return");
                };
                call(0xffe0a7d0);call(0xffe02750);call(0xffe09000);
                u32 owners[2]{},images[2]{};
                for(unsigned n=0;n<2;++n) {
                    m.c.a[13]=0x01000000;m.c.a[1]=247+n;m.c.a[7]=returned;call(0xffe0aba0);
                    owners[n]=m.c.a[0];wb(m,owners[n]+0x1d8,n+1);
                    m.c.a[13]=owners[n];m.c.a[0]=(100+n*40)<<16;m.c.a[1]=120<<16;
                    m.c.a[2]=0xfffae3a0;m.c.a[3]=100;m.c.a[4]=0x8002;m.c.a[5]=0x4101+n;m.c.a[6]=m.c.a[7]=0;
                    call(0xffe04170);images[n]=m.c.a[8];
                    wlong(m,0x010dd780+n*32,images[n]);wlong(m,0x010de2b0+n*32,owners[n]);
                    wf(m,owners[n]+0x198,n==owner?mode:4,0);m.ww(owners[n]+0x220,n==owner?100:999);
                    wlong(m,owners[n]+0x2e0,n==owner && helper?images[n]:0);
                }
                m.ww(0x010865f0,1);m.ww(0x010865a0,16);m.ww(0x010de480,direction);
                wlong(m,0x010ee9f0,180<<16);wlong(m,0x010eea10,140<<16);
                auto visit=[&]() {
                    m.c.sp=stack;m.c.a[13]=0x01000000;
                    for(unsigned n=0;n<240;++n) {
                        const u32 next=rlong(m,m.c.a[13]);if(!next)break;
                        if(next==owners[0] || next==owners[1]){m.c.a[13]=next;continue;}
                        u32 pc=l8_run(m,0xffe0a500,m.c.cyc+1);
                        for(unsigned j=0;j<50000 && pc!=0xffe0a500 && !m.stop;++j)pc=l8_run(m,pc,m.c.cyc+1);
                        if(pc!=0xffe0a500 || m.stop || !m.faults.empty() || m.c.sp!=stack) {
                            std::fprintf(stderr,"Bullet diagnostic wide=%u owner=%u mode=%u helper=%u dir=%u pc=%08x\n",
                                unsigned(wide),owner,mode,unsigned(helper),direction,pc);return false;
                        }
                    }
                    return true;
                };
                const unsigned base_cycles=(mode==2 || mode==3)?3u:2u;
                const unsigned gate_cycles=direction==1 && !helper && mode!=3?12u:0u;
                const unsigned bounce_cycles=mode==3?3u:0u;
                for(unsigned cycle=0;cycle<base_cycles+gate_cycles+bounce_cycles;++cycle) {
                    const bool gate_case=mode!=3 && cycle>=base_cycles,bounce_case=mode==3 && cycle>=base_cycles;
                    const unsigned gate=(gate_case || bounce_case)?cycle-base_cycles:0,wall=1+(bounce_case?gate:gate/4);
                    const bool door_open=(gate/2)%2,inside=gate%2;
                    // Each firing case supplies a fresh input direction; a
                    // prior deflection legitimately changes the shared B_DIR.
                    m.ww(0x010de480,direction);
                    // Compare a real right-facing shot against the same shot with
                    // a clockwise stick offset. Release input before child wake.
                    const bool analog_case=cycle==0;
                    constexpr unsigned fire_bits[]={0,1,2,0,8,9,10,4,5,6};
                    if(analog_case)m.aim_inputs[owner]=u16((60<<10)|(fire_bits[direction]<<4));
                    if(wide && analog_case) {
                        m.c.a[0]=images[owner];m.c.a[9]=0x00160014;
                        m.c.a[10]=(96u<<16)|96;m.c.a[11]=0x02123400;m.c.a[12]=0x8002;
                        const auto dims=m.c.a[9],pos=m.c.a[10],page=m.c.a[11],flags=m.c.a[12];
                        m.queue_aimed_sprite();
                        check(m.c.a[9]==dims && m.c.a[10]==pos && m.c.a[11]==page && m.c.a[12]==flags,
                              "Analog aim changed retail torso drawing");
                    }
                    m.c.a[13]=owners[owner];m.c.a[8]=images[owner];call(0xffe337a0);
                    m.aim_inputs.fill(0);
                    const bool entered=visit();check(entered,"Bullet entry failed scheduler boundary");if(!entered)break;
                    std::vector<std::pair<u32,u32>> bullets;std::vector<u32> shadows;unsigned count=0;
                    for(u32 p=rlong(m,0x01000000);p && count<240;p=rlong(m,p),++count) {
                        if(p==owners[0] || p==owners[1])continue;
                        const u32 object=rlong(m,p+0xc0);bullets.emplace_back(p,object);
                        check(rb(m,p+0x138)==owner+1 && m.rw(p+0x100)==converted[direction],"Bullet owner/direction not materialized");
                        check(rlong(m,object+0x160)==p && (m.rw(object+0xb0)&0x8400)==((mode==2 || mode==3)?0x8000:0x8400) &&
                              m.rw(object+0x150)==(mode==3?0:0x4200+mode+1),"Bullet object identity/linkage differs");
                        if(analog_case) {
                            const auto aim=m.shot_aim[(p-0x01086790)/0x5a0];
                            check(bool(aim)==wide,"Analog shot escaped profile or lost its owner");
                            if(direction==4 && (mode==0 || mode==4)) {
                                const auto vx=s32(rlong(m,object+0x20)),vy=s32(rlong(m,object+0x40));
                                check(vx>0 && (wide ? vy>vx/3 && vy<vx/2 : vy==0),
                                      "Allocated projectile missed the firing bearing");
                            }
                        }
                        if(mode!=3)check(rlong(m,object+0x20)!=0 || rlong(m,object+0x40)!=0,"Bullet starts without velocity");
                        if(mode==3) {
                            const u32 shadow=rlong(m,p+0xa0);shadows.push_back(shadow);
                            check(shadow!=object && (m.rw(shadow+0xb0)&0x8000) && m.rw(shadow+0x150)==0 &&
                                  rlong(m,shadow+0x160)==p,"Grenade shadow identity/linkage differs");
                            check(rb(m,p+0x120)==1,"Grenade first toss step did not advance");
                        }
                    }
                    const unsigned expected=(mode==1?3:1)*(helper && mode!=3?2:1);
                    check(bullets.size()==expected && count==expected+2,"Bullet allocation count differs");
                    // Retail MV_BULL deflects normal bullets through GET_CNVRT,
                    // which rewrites MYPLYR from the caller's current A11.
                    // This field alone cannot prove original firing identity
                    // across collisions, including when the new value is 1/2.
                    if(mode==0 && cycle==0)for(const auto& [process,object]:bullets)
                        for(u32 collision_a11:{1u,2u,9u,0x12eu}) {
                            m.c.a[8]=object;m.c.a[13]=owners[owner];m.c.a[11]=collision_a11;
                            call(0xffe32040);
                            check(rb(m,process+0x138)==(collision_a11&255),"Deflection owner rewrite differs from retail");
                            check(m.c.a[13]==owners[owner] && m.c.a[8]==object &&
                                  rlong(m,object+0x160)==process && m.rw(object+0x150)==0x4201,
                                  "Deflection changed projectile linkage or caller process");
                            const u16 converted_direction=m.rw(process+0x100);
                            check(converted_direction>=1 && converted_direction<=8 &&
                                  (rlong(m,object+0x20)!=0 || rlong(m,object+0x40)!=0),
                                  "Deflection failed to select moving direction");
                        }
                    if(gate_case) {
                        ++wall_cases;
                        // Actual retail DRINF is a ROM table of KP_LS/RS/TS
                        // byte-state addresses, not an invented RAM read.
                        const u32 flag=rlong(m,0xffe3b1f0+wall*32);
                        check(flag>=0x01000000 && flag<0x01100000,"DRINF gate state is not RAM");
                        wb(m,flag,door_open?1:0);
                    }
                    if(mode==3 && cycle==0 && !bullets.empty()) {
                        // Collision deletion is legal only after the grenade
                        // arms; before then the neutral OID excludes it.
                        for(unsigned n=0;n<40 && rb(m,bullets[0].first+0x120)<16;++n)
                            check(visit(),"Grenade arming failed scheduler boundary");
                        check(rb(m,bullets[0].first+0x120)==16 && m.rw(bullets[0].second+0x150)==0x4204,
                              "Grenade did not arm on toss step15");
                    }
                    if(bounce_case && !bullets.empty()) {
                        ++bounce_cases;
                        const auto [process,object]=bullets[0];
                        wlong(m,object+0x60,(wall==1?0u:wall==2?1000u:200u)<<16);
                        wlong(m,object+0x80,(wall==3?0u:120u)<<16);
                        for(unsigned n=0;n<4 && rb(m,process+0x120)<2;++n)
                            check(visit(),"Grenade wall contact failed scheduler boundary");
                        const bool slide_x=wall!=3 && converted[direction]!=1 && converted[direction]!=5;
                        const bool slide_y=wall==3 && converted[direction]!=3 && converted[direction]!=7;
                        check(rb(m,process+0x120)==2 && rb(m,process+0x104)==unsigned(slide_x) &&
                              rb(m,process+0x130)==unsigned(slide_y),"Grenade wall slide axis/direction exception differs");
                    }
                    for(const auto& [process,object]:bullets) {
                        if(cycle==0){m.c.a[13]=owners[owner];m.c.a[8]=object;call(0xffe322d0);}
                        else if(gate_case) {
                            const unsigned x=wall==1?0:wall==2?1000:inside?(wide?273:201):100;
                            const unsigned y=wall==3?0:inside?140:80;
                            wlong(m,object+0x60,x<<16);wlong(m,object+0x80,y<<16);
                        } else if(bounce_case || cycle==2 || (mode!=2 && mode!=3))wlong(m,object+0x80,1000u<<16);
                    }
                    if(cycle!=0) {
                        bool expired=false,transition=false;
                        std::vector<bool> passages(bullets.size()),impacts(bullets.size());
                        for(unsigned n=0;n<160;++n) {
                            bool active=false;
                            for(unsigned index=0;index<bullets.size();++index) {
                                const auto [process,object]=bullets[index];
                                if(!(m.rw(object+0xb0)&0x8000))continue;
                                active=true;
                                transition|=m.rw(object+0x150)==(mode==2?0x4206:0x4204);
                                if(gate_case) {
                                    const u32 passage=mode==2?0xffe369d0:mode==4?0xffe34940:0xffe375f0;
                                    passages[index]=passages[index] || rlong(m,process+0xe0)==passage;
                                    if(m.rw(object+0x150)==0) {
                                        impacts[index]=true;
                                        check(rlong(m,object+0x20)==0 && rlong(m,object+0x40)==0,
                                              "Wall impact failed to stop projectile velocity");
                                    }
                                }
                            }
                            if(!active){expired=true;break;}
                            check(visit(),"Projectile expiry failed scheduler boundary");
                        }
                        check(expired,"Projectile failed bounded retirement");
                        if(cycle==1 && (mode==2 || mode==3))check(transition,"Natural projectile expiry skipped explosion/arming state");
                        if(gate_case)for(unsigned index=0;index<bullets.size();++index) {
                            const bool through=door_open && inside;
                            if(passages[index]!=through || impacts[index]==through)
                                std::fprintf(stderr,"Gate diagnostic wide=%u owner=%u mode=%u wall=%u open=%u inside=%u pass=%u impact=%u\n",
                                    unsigned(wide),owner,mode,wall,unsigned(door_open),unsigned(inside),unsigned(passages[index]),unsigned(impacts[index]));
                            check(passages[index]==through && impacts[index]!=through,
                                  "Projectile wall/door decision differs from gate state and aperture");
                        }
                    }
                    count=0;
                    for(u32 p=rlong(m,0x01000000);p && count<240;p=rlong(m,p),++count)
                        check(p==owners[0] || p==owners[1],"Bullet cleanup retained projectile process");
                    check(count==2,"Bullet cleanup removed owner process");count=0;
                    for(u32 p=rlong(m,0x01000040);p && count<420;p=rlong(m,p),++count)
                        check(p==images[0] || p==images[1],"Bullet cleanup retained projectile object");
                    check(count==2 && m.rw(owners[1-owner]+0x220)==999,"Bullet cleanup removed player image or changed other ammo");
                    for(const auto& [process,object]:bullets)
                        check(!(m.rw(object+0xb0)&0x8000),"Bullet cleanup failed to free object slot");
                    for(u32 shadow:shadows)check(!(m.rw(shadow+0xb0)&0x8000),"Grenade cleanup retained shadow object");
                }
            }
    std::printf("%u projectile entry/cleanup cases (pods/grenades three cycles, others two)\n",bullet_cases);
    std::printf("%u projectile wall/door cases\n",wall_cases);
    std::printf("%u grenade wall-slide cases\n",bounce_cases);
    // Complete YESWALL call: world-relative fixed-point aperture, including
    // fractional edge values, negative origins and the boss's expanded hole.
    unsigned aperture_cases=0;
    constexpr std::pair<int,int> origins[]={{0,0},{-70,-30},{1000,500}};
    for(bool wide:{false,true})for(bool boss:{false,true})for(unsigned wall=1;wall<=3;++wall)
        for(const auto& [world_x,world_y]:origins) {
            const u32 low=(wall==3?(wide?237u:165u)-(boss?45u:0u):111u)<<16;
            const u32 high=(wall==3?(wide?309u:237u)+(boss?45u:0u):169u)<<16;
            for(u32 coordinate:{low-1,low,low+1,high-1,high,high+1}) {
                ++aperture_cases;
                Machine m=wide?enhanced_loaded:loaded;m.external_timers=true;m.c.st=0x830;
                m.ww(0x010865a0,16);wb(m,0x010f2da8,boss?1:0);
                wlong(m,0x01000280,u32(u16(world_x))|(u32(u16(world_y))<<16));
                wlong(m,item+0x60,u32(world_x)*65536u+(wall==3?coordinate:200u<<16));
                wlong(m,item+0x80,u32(world_y)*65536u+(wall==3?120u<<16:coordinate));
                m.c.a[0]=wall;m.c.a[8]=item;m.c.sp=stack;push(m,m.c,returned);
                u32 pc=0xffe37790;
                for(unsigned n=0;n<100 && pc!=returned && !m.stop;++n)pc=l8_run(m,pc,m.c.cyc+1);
                check(pc==returned && !m.stop && m.faults.empty() && m.c.sp==stack && m.c.a[8]==item,
                      "Aperture call failed return/owner preservation");
                check(bool(m.c.st&ST_C)==(coordinate<low || coordinate>=high),
                      "Aperture world origin/fractional edge/boss expansion differs");
            }
        }
    std::printf("%u full aperture/world-origin calls\n",aperture_cases);
    std::printf("%u checks, %u failures; 256 selections,120 shoe movement,%u shoe calls,%u weapon calls,%u meter,%u ring expiry,%u full ring,%u ring pickups,%u collision transactions,%u pixel masks across both profiles\n",checks,failures,full_cases,weapon_cases,meter_cases,ring_cases,ring_entry_cases,ring_pickups,collision_cases,pixel_cases);
    return failures?1:0;
}
