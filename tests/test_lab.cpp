#include "stv/source_lab.hpp"
#include "../src/source_states.hpp"
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
using namespace stv;
static unsigned groups=0;static u64 checks=0;
#define CHECK(x) do{++checks;if(!(x))throw std::runtime_error(std::string(__FILE__)+":"+std::to_string(__LINE__)+": " #x);}while(false)
template<class F>void group(const char* name,F fn){fn();++groups;std::cout<<"PASS "<<name<<'\n';}
static u32 bcd(unsigned n){u32 v=0;for(unsigned s=0;s<32;s+=4){v|=(n%10)<<s;n/=10;}return v;}
static std::vector<u8> fixture(const char* p){std::ifstream f(p,std::ios::binary|std::ios::ate);if(!f)throw std::runtime_error("Source pack missing");auto n=f.tellg();if(n<0||n>64*1024*1024)throw std::runtime_error("Bad fixture size");std::vector<u8>b(static_cast<usize>(n));f.seekg(0);f.read(reinterpret_cast<char*>(b.data()),std::streamsize(b.size()));if(!f)throw std::runtime_error("Fixture read failed");return b;}
int main(int argc,char**argv){try{if(argc!=2)throw std::runtime_error("Pass source.stva");auto bytes=fixture(argv[1]);auto pack=std::make_unique<SourcePack>();CHECK(pack->open({bytes.data(),bytes.size()}));
 group("SCORE BCDADD decimal oracle and eight-digit wrap",[]{for(unsigned i=0;i<10000;++i){unsigned a=(i*7919u)%100000000,b=(i*523u)%100000000;CHECK(source_bcd_add(bcd(a),bcd(b))==bcd((a+b)%100000000));}CHECK(source_bcd_add(0x99999999,1)==0);});
 group("SCORE SCRADD2 co-op doubling, attract suppression, one extra request",[]{SourceScore s{0,0x1500000};auto e=source_score_add(s,0x500,3,false);CHECK(s.packed_bcd==0x1000&&!e.extra_life_requested);e=source_score_add(s,0x10000,1,true);CHECK(s.packed_bcd==0x1000);s={0x1499900,0x1500000};e=source_score_add(s,0x500,1,false);CHECK(s.packed_bcd==0x1500400&&e.extra_life_requested&&s.next_extra==0x3000000);s={0,0x1500000};e=source_score_add(s,0x9000000,1,false);CHECK(e.extra_life_requested&&s.next_extra==0x3000000);});
 group("NDSP1 literal DVEL includes objects with commented-out NOMOV bit",[]{auto objects=std::make_unique<NativeObjects>();objects->initialize();ObjectHandle o;CHECK(objects->allocate(o));objects->cells[o].sprite.flags=0x2000;objects->cells[o].sprite.position={0,0,0x12000,0u-0x4000};CHECK(objects->insert(o));objects->integrate();CHECK(objects->cells[o].sprite.position.x==0x12000&&objects->cells[o].sprite.position.y==0u-0x4000);});
 group("MPROC overload signed word source shift",[]{auto p=std::make_unique<NativeProcesses>();p->initialize();p->overload=0x8000;auto step=[](NativeProcesses&,ProcessHandle,void*){return ProcessAction::suicide();};CHECK(p->dispatch(step,nullptr,1));CHECK(p->overload==0xc001);});
 group("Source lab input cadence, PLYRPRCS bounds, paired anchors, gun 4+4 tick firing cycle",[&]{auto lab=std::make_unique<SourceLab>();CHECK(lab->load(*pack));CHECK(lab->reset(3,1,0));LabInput in;in.fire[0]=8;for(unsigned i=0;i<8;++i){CHECK(lab->tick(in));CHECK(lab->stats.shots==1);CHECK(lab->validate());}CHECK(lab->tick(in));CHECK(lab->stats.shots==2);CHECK(lab->players[0].legs!=no_object&&lab->players[1].legs==no_object);CHECK(lab->stats.sound_writes>0);u32 value=0;const auto& player=lab->processes.cells[lab->players[0].torso_process];CHECK(read_field({player.data,sizeof player.data},{320},32,false,value)&&value==0x2c0000);CHECK(read_field({player.data,sizeof player.data},{352},32,false,value)&&value==0xef0000);CHECK(read_field({player.data,sizeof player.data},{384},32,false,value)&&value==0x250000);CHECK(read_field({player.data,sizeof player.data},{416},32,false,value)&&value==0x1620000);auto& leg=lab->objects.cells[lab->players[0].legs].sprite;u32 x,y;CHECK(sprite_anchor(leg,x,y));CHECK(x==140u<<16&&y==165u<<16);});
 group("Two-player independent source velocities and retained source palettes",[&]{auto lab=std::make_unique<SourceLab>();CHECK(lab->load(*pack));CHECK(lab->reset(7,2,0));LabInput in;in.move[0]=8;in.move[1]=4;CHECK(lab->tick(in));auto p1=lab->players[0].legs,p2=lab->players[1].legs;CHECK(lab->objects.cells[p1].sprite.position.vx==0x16000);CHECK(lab->objects.cells[p2].sprite.position.vx==0u-0x16000);CHECK(lab->objects.cells[p1].sprite.palette!=lab->objects.cells[p2].sprite.palette);CHECK(lab->tick(in));u32 x1,y1,x2,y2;CHECK(sprite_anchor(lab->objects.cells[p1].sprite,x1,y1));CHECK(sprite_anchor(lab->objects.cells[p2].sprite,x2,y2));CHECK(x1==(140u<<16)+0x16000&&x2==(260u<<16)-0x16000);});
 group("ANI retains OPAL across image change unless FRANIM requests palette",[&]{EditorImages bank;CHECK(bank.open(pack->find("BMB.IMG")->bytes));NativeSprite sprite{&bank,0,-1,2,{0,0,0,0}};EditorImage old;CHECK(bank.image(0,old));for(unsigned i=1;i<bank.count;++i){CHECK(set_animation_image(sprite,i,2));CHECK(sprite.palette==int((old.selector&255)-3));}});
 group("Editor duplicate names require metadata equivalence or pinned linked-table binding",[&]{EditorImages bank;CHECK(bank.open(pack->find("PLYRS2.IMG")->bytes));CHECK(bank.find("P1SG9T2A")==106);CHECK(bank.find("P1SG9T3A")==-2);unsigned index;CHECK(resolve_animation_image(bank,"P1SG9T3A",index));CHECK(index==158);EditorImage image;CHECK(bank.image(index,image));CHECK(image.anchor_x==15&&image.anchor_y==-25);std::vector<u8> changed(bank.data.data,bank.data.data+bank.data.size);changed[10]^=1;EditorImages altered;CHECK(altered.open({changed.data(),changed.size()}));CHECK(!resolve_animation_image(altered,"P1SG9T3A",index));});
 group("Integrated native pair callbacks, pool reuse and deterministic reset",[&]{auto a=std::make_unique<SourceLab>(),b=std::make_unique<SourceLab>();CHECK(a->load(*pack));CHECK(b->load(*pack));constexpr u8 compass[8]={1,9,8,10,2,6,4,5};for(unsigned i=0;i<4000;++i){if(i==2000){CHECK(a->reset(123,2,32));CHECK(b->reset(123,2,32));}LabInput in;in.fire[0]=compass[(i/35)%8];in.fire[1]=compass[(i/43+4)%8];in.move[0]=u8(i%400<200?8:4);in.move[1]=u8(i%400<200?4:8);CHECK(a->tick(in));CHECK(b->tick(in));CHECK(a->validate());CHECK(b->validate());if(i%37==0){auto da=a->state_digest(),db=b->state_digest();CHECK(equal(da.bytes,db.bytes,32));}}CHECK(a->stats.shots>400&&a->stats.hulk_kills>10);CHECK(a->objects.count<100);});
 group("L8 Hulk T4 and WAY resumes preserve scheduler sleeps and observed facing turns",[&]{
    auto lab=std::make_unique<SourceLab>();CHECK(lab->load(*pack));CHECK(lab->reset(0x9137,1,1));
    for(unsigned i=0;i<3;++i)CHECK(lab->tick({}));
    ProcessHandle h=no_process;
    for(ProcessHandle p=lab->processes.active_head;p!=no_process;p=lab->processes.cells[p].next)
        if(lab->processes.cells[p].id==source_hulk_process_id&&lab->contexts[p].object!=no_object){h=p;break;}
    CHECK(h!=no_process);auto& c=lab->contexts[h];auto& process=lab->processes.cells[h];CHECK(c.object!=no_object);
    auto read=[&](SourceHulkField field){u32 value=0;CHECK(source_hulk_pdata_read(process,field,value));return u16(value);};
    auto write=[&](SourceHulkField field,u32 value){CHECK(source_hulk_pdata_write(process,field,value));};
    auto& actor=lab->objects.cells[c.object].sprite;actor.position.x=100u<<16;actor.position.y=80u<<16;actor.position.vx=actor.position.vy=0;
    EditorImage image;CHECK(actor.art->image(actor.image,image));
    const i32 anchor=(actor.flags&16)?i32(image.width)-image.anchor_x-1:image.anchor_x;
    const i32 actor_x=100+anchor;CHECK(actor_x>0&&actor_x<300);
    auto& target=lab->objects.cells[lab->players[0].torso].sprite;
    target.position.x=u32(actor_x)<<16;target.position.y=220u<<16;target.position.vx=target.position.vy=0;
    write(SourceHulkField::level,0);write(SourceHulkField::age,100);write(SourceHulkField::keep_age,100);
    write(SourceHulkField::direction,4);write(SourceHulkField::seek_direction,4);
    write(SourceHulkField::control_flags,0);write(SourceHulkField::distance,1);write(SourceHulkField::oscillation,0);
    process.continuation=source_hulk_resume_lup_top;process.time=1;
    CHECK(lab->tick({}));
    CHECK(read(SourceHulkField::direction)==4&&read(SourceHulkField::seek_direction)==5);
    CHECK((read(SourceHulkField::control_flags)&1)&&read(SourceHulkField::oscillation)==2);
    CHECK(process.continuation==source_hulk_resume_lup_top&&process.time==3);
    CHECK(lab->tick({}));CHECK(process.continuation==source_hulk_resume_lup_top&&process.time==2);
    CHECK(lab->tick({}));CHECK(process.continuation==source_hulk_resume_lup_top&&process.time==1);
    CHECK(lab->tick({}));CHECK(read(SourceHulkField::direction)==5&&read(SourceHulkField::seek_direction)==5);
    CHECK(process.continuation==source_hulk_resume_lup_top&&process.time==3);
    CHECK(lab->tick({}));CHECK(process.continuation==source_hulk_resume_lup_top&&process.time==2);
    CHECK(lab->tick({}));CHECK(process.continuation==source_hulk_resume_lup_top&&process.time==1);
    CHECK(lab->tick({}));CHECK(read(SourceHulkField::oscillation)==0&&(read(SourceHulkField::control_flags)&1)==0);
    CHECK(process.continuation==source_hulk_resume_lup_top&&process.time==1);
    write(SourceHulkField::distance,20);const u32 prior_cursor=process.saved[1];
    CHECK(lab->tick({}));CHECK(process.saved[1]!=prior_cursor);
    CHECK(process.continuation==source_hulk_resume_lup_top&&process.time>0);

    // The second paired L8 actor turns one octant every observed three ticks.
    write(SourceHulkField::control_flags,1);write(SourceHulkField::oscillation,2);
    write(SourceHulkField::direction,4);write(SourceHulkField::seek_direction,7);
    write(SourceHulkField::age,100);write(SourceHulkField::distance,20);
    constexpr u16 observed_faces[3]={5,6,7};
    for(unsigned i=0;i<3;++i){if(i==0){process.continuation=source_hulk_resume_lup_top;process.time=1;}else CHECK(process.time==1);CHECK(lab->tick({}));if(read(SourceHulkField::direction)!=observed_faces[i])throw std::runtime_error("Hulk facing expected "+std::to_string(observed_faces[i])+" got "+std::to_string(read(SourceHulkField::direction)));CHECK(process.continuation==source_hulk_resume_lup_top&&process.time==3);if(i<2){CHECK(lab->tick({}));CHECK(lab->tick({}));CHECK(process.time==1);}}
    CHECK(lab->validate());
 });
 group("Source lab rejects unsupported fixture and surfaces instead of using fallback",[&]{auto a=std::make_unique<SourceLab>();CHECK(!a->reset());CHECK(a->load(*pack));CHECK(!a->reset(1,3,1));CHECK(!a->reset(1,1,65));u32 pixel;CHECK(!a->draw({&pixel,1,1,1,1}));CHECK(a->reset(1,1,0));LabInput conflict;conflict.fire[0]=3;CHECK(!a->tick(conflict));CHECK(!a->tick({}));CHECK(a->reset(1,1,0));CHECK(a->tick({}));});
 std::cout<<"RESULT "<<groups<<" groups; "<<checks<<" checks. Native source-component tests; NOT arcade equivalence.\n";return 0;
 }catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
