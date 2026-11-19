#include "stv/source_lab.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <chrono>
using namespace stv;
namespace fs=std::filesystem;
static void require(Status s){if(!s)throw std::runtime_error(std::string(s.message)+" at "+std::to_string(s.offset));}
static std::vector<u8> read(const fs::path& p){std::ifstream f(p,std::ios::binary|std::ios::ate);if(!f)throw std::runtime_error("Cannot open source pack");auto size=f.tellg();if(size<0||size>64*1024*1024)throw std::runtime_error("Source pack size invalid");std::vector<u8>b(static_cast<usize>(size));f.seekg(0);f.read(reinterpret_cast<char*>(b.data()),std::streamsize(b.size()));if(!f)throw std::runtime_error("Source pack read failed");return b;}
static std::string digest(Sha256 d){char h[65];hex(d.bytes,32,h);return h;}
static void capture(const fs::path& p,const std::vector<u32>& pixels){std::ofstream f(p,std::ios::binary);f<<"P6\n"<<lab_width<<" "<<lab_height<<"\n255\n";for(auto x:pixels){char c[3]={char(x>>16),char(x>>8),char(x)};f.write(c,3);}if(!f)throw std::runtime_error("Capture write failed");}
// Deliberately authored test input. This is not a recording of original arcade play.
static LabInput fixture(u64 tick){constexpr u8 directions[8]={1,9,8,10,2,6,4,5};LabInput in;in.fire[0]=directions[(tick/35)%8];in.fire[1]=directions[(tick/43+4)%8];constexpr u8 path[4]={8,2,4,1};in.move[0]=path[(tick/140)%4];in.move[1]=path[(tick/150+2)%4];return in;}
int main(int argc,char** argv){try{
 if(argc<3||argc>4)throw std::runtime_error("Usage: stv_replay source.stva output_directory [ticks=12000]");
 unsigned ticks=argc==4?unsigned(std::stoul(argv[3])):12000;if(!ticks||ticks>1000000)throw std::runtime_error("Tick count must be 1..1000000");
 auto bytes=read(argv[1]);auto pack=std::make_unique<SourcePack>();require(pack->open({bytes.data(),bytes.size()}));
 auto a=std::make_unique<SourceLab>(),b=std::make_unique<SourceLab>();require(a->load(*pack));require(b->load(*pack));
 fs::path output=argv[2];fs::create_directories(output);std::ofstream trace(output/"state-trace.csv"),inputs(output/"input.csv");
 trace<<"tick,p1x,p1y,p2x,p2y,p1score_bcd,p2score_bcd,shots,kills,hulks,bullets,processes,objects,state_sha256\n";
 inputs<<"tick,p1move,p1fire,p2move,p2fire\n";std::vector<u32> pixels(lab_width*lab_height),pixels2(pixels.size());
 u64 total_shots=0,total_kills=0,total_impacts=0;unsigned max_objects=0,max_processes=0,resets=0;
 const auto start=std::chrono::steady_clock::now();
 for(unsigned t=0;t<ticks;++t){
    if(t&&t%1500==0){total_shots+=a->stats.shots;total_kills+=a->stats.hulk_kills;total_impacts+=a->stats.wall_impacts;++resets;require(a->reset(0x13579bdf+resets,2,16));require(b->reset(0x13579bdf+resets,2,16));}
    auto in=fixture(t);require(a->tick(in));require(b->tick(in));require(a->validate());require(b->validate());
    if(a->objects.count>max_objects)max_objects=a->objects.count;if(a->processes.active_count>max_processes)max_processes=a->processes.active_count;
    inputs<<t<<','<<unsigned(in.move[0])<<','<<unsigned(in.fire[0])<<','<<unsigned(in.move[1])<<','<<unsigned(in.fire[1])<<'\n';
    if(t%20==0||t+1==ticks){auto da=a->state_digest(),db=b->state_digest();if(!equal(da.bytes,db.bytes,32))throw std::runtime_error("First native/native state divergence at tick "+std::to_string(t));
       const auto& p1=a->objects.cells[a->players[0].legs].sprite.position;const auto& p2=a->objects.cells[a->players[1].legs].sprite.position;
       trace<<t<<','<<p1.x<<','<<p1.y<<','<<p2.x<<','<<p2.y<<','<<a->players[0].score.packed_bcd<<','<<a->players[1].score.packed_bcd<<','<<a->stats.shots<<','<<a->stats.hulk_kills<<','<<a->stats.hulks<<','<<a->stats.bullets<<','<<a->processes.active_count<<','<<a->objects.count<<','<<digest(da)<<'\n';
    }
    if(t==0||t==60||t==120||t==240||t==600||t+1==ticks){require(a->draw({pixels.data(),lab_width,lab_height,lab_width,pixels.size()}));require(b->draw({pixels2.data(),lab_width,lab_height,lab_width,pixels2.size()}));if(pixels!=pixels2)throw std::runtime_error("Native/native pixel divergence");capture(output/("frame-"+std::to_string(t)+".ppm"),pixels);}
 }
 total_shots+=a->stats.shots;total_kills+=a->stats.hulk_kills;total_impacts+=a->stats.wall_impacts;
 double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
 std::cout<<"PASS: deterministic native/native source-component replay\n"<<"ticks_per_instance="<<ticks<<" instances=2 resets="<<resets<<" total_shots="<<total_shots<<" total_hulk_kills="<<total_kills<<" total_wall_impacts="<<total_impacts<<" max_objects="<<max_objects<<" max_processes="<<max_processes<<" elapsed_seconds="<<elapsed<<"\n";
 std::cout<<"final_state_sha256="<<digest(a->state_digest())<<"\nNOT an arcade differential or full-game test. Fixture clock/HCOUNT/spawns are authored; damage and sound synthesis absent.\n";
 if(!trace||!inputs)throw std::runtime_error("Trace write failed");return 0;
 }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
