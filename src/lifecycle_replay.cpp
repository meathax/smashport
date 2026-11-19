#include "stv/source_lab.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
using namespace stv;namespace fs=std::filesystem;
static void require(Status s){if(!s)throw std::runtime_error(std::string(s.message)+" ["+std::to_string(s.offset)+"]");}
static std::string hash(Sha256 d){char text[65];hex(d.bytes,32,text);return text;}
static void contact_fixture(SourceLab& lab){for(auto h=lab.objects.head;h!=no_object;h=lab.objects.cells[h].next)if(lab.objects.cells[h].id==0x8600){auto& s=lab.objects.cells[h].sprite;s.position=lab.objects.cells[lab.players[0].legs].sprite.position;s.position.vx=s.position.vy=0;lab.players[0].life.immunity=0;return;}throw std::runtime_error("Fixture enemy missing");}
static void capture(const fs::path& p,const std::vector<u32>& pixels){std::ofstream f(p,std::ios::binary);f<<"P6\n400 260\n255\n";for(auto pixel:pixels){char rgb[3]={char(pixel>>16),char(pixel>>8),char(pixel)};f.write(rgb,3);}if(!f)throw std::runtime_error("Capture write failed");}
int main(int argc,char** argv){try{
 if(argc<3||argc>4)throw std::runtime_error("Usage: stv_lifecycle_replay source.stva output-directory [ticks=12000]");
 unsigned ticks=argc==4?unsigned(std::stoul(argv[3])):12000;if(!ticks||ticks>1000000)throw std::runtime_error("Invalid tick count");
 std::ifstream f(argv[1],std::ios::binary|std::ios::ate);if(!f)throw std::runtime_error("Source pack not found");auto size=f.tellg();if(size<0||size>64000000)throw std::runtime_error("Invalid source pack size");std::vector<u8> bytes(static_cast<usize>(size));f.seekg(0);f.read(reinterpret_cast<char*>(bytes.data()),size);if(!f)throw std::runtime_error("Source pack read failed");
 auto pack=std::make_unique<SourcePack>();require(pack->open({bytes.data(),bytes.size()}));auto a=std::make_unique<SourceLab>(),b=std::make_unique<SourceLab>();require(a->load(*pack));require(b->load(*pack));
 fs::path out=argv[2];fs::create_directories(out);std::ofstream trace(out/"state-trace.csv"),inputs(out/"input-and-fixture-events.csv");
 trace<<"tick,p1x,p1y,p1dead,p1reserves,p1immunity,p1respawns,p1ring,p1torso_state,p2dead,p2reserves,hits,respawns,ring_kills,blood,bat_swings,shots,hulks,objects,processes,state_sha256\n";
 inputs<<"tick,reset,forced_contact,p1move,p1fire,p2move,p2fire\n";
 std::vector<u32> pixels(400*260),other(pixels.size());u64 shots=0,hits=0,respawns=0,ringkills=0,blood=0,bats=0;unsigned maxobjects=0,maxprocesses=0,resets=0;
 auto sum=[&]{shots+=a->stats.shots;hits+=a->stats.player_hits;respawns+=a->stats.respawns;ringkills+=a->stats.ring_kills;blood+=a->stats.blood_drops;bats+=a->stats.bat_swings;};
 for(unsigned t=0;t<ticks;++t){bool reset=t%1000==0,forced=t%1000==1;if(reset){if(t){sum();++resets;}require(a->reset(0x13579bdf+resets,2,32,true));require(b->reset(0x13579bdf+resets,2,32,true));}if(forced){contact_fixture(*a);contact_fixture(*b);}LabInput in;constexpr u8 compass[8]={1,9,8,10,2,6,4,5};if(t%1000>360){in.fire[0]=compass[(t/35)%8];in.fire[1]=compass[(t/43+4)%8];in.move[0]=u8(t%400<200?8:4);in.move[1]=u8(t%400<200?4:8);}require(a->tick(in));require(b->tick(in));require(a->validate());require(b->validate());
   if(a->objects.count>maxobjects)maxobjects=a->objects.count;if(a->processes.active_count>maxprocesses)maxprocesses=a->processes.active_count;
   inputs<<t<<','<<reset<<','<<forced<<','<<unsigned(in.move[0])<<','<<unsigned(in.fire[0])<<','<<unsigned(in.move[1])<<','<<unsigned(in.fire[1])<<'\n';
   if(t%5==0||t+1==ticks){auto da=a->state_digest(),db=b->state_digest();if(!equal(da.bytes,db.bytes,32))throw std::runtime_error("Native/native divergence at "+std::to_string(t));const auto& p=a->players[0];const auto& pos=a->objects.cells[p.legs].sprite.position;trace<<t<<','<<pos.x<<','<<pos.y<<','<<p.life.dead<<','<<p.life.reserves<<','<<p.life.immunity<<','<<p.life.respawns<<','<<p.ring<<','<<a->processes.cells[p.torso_process].continuation<<','<<a->players[1].life.dead<<','<<a->players[1].life.reserves<<','<<a->stats.player_hits<<','<<a->stats.respawns<<','<<a->stats.ring_kills<<','<<a->stats.blood_drops<<','<<a->stats.bat_swings<<','<<a->stats.shots<<','<<a->stats.hulks<<','<<a->objects.count<<','<<a->processes.active_count<<','<<hash(da)<<'\n';}
   if(t==0||t==8||t==40||t==137||t==220||t==300||t==600||t+1==ticks){require(a->draw({pixels.data(),400,260,400,pixels.size()}));require(b->draw({other.data(),400,260,400,other.size()}));if(pixels!=other)throw std::runtime_error("Native/native framebuffer divergence");capture(out/("frame-"+std::to_string(t)+".ppm"),pixels);}
 }
 sum();if(!trace||!inputs)throw std::runtime_error("Trace write failed");
 std::ofstream report(out/"summary.json");report<<"{\n  \"ticks_per_instance\": "<<ticks<<", \"instances\": 2, \"resets\": "<<resets<<",\n  \"shots_per_fixture_run\": "<<shots<<", \"player_hits\": "<<hits<<", \"respawns\": "<<respawns<<", \"ring_kills\": "<<ringkills<<", \"blood_effects\": "<<blood<<", \"bat_swings\": "<<bats<<",\n  \"max_objects\": "<<maxobjects<<", \"max_processes\": "<<maxprocesses<<",\n  \"final_state_sha256\": \""<<hash(a->state_digest())<<"\",\n  \"scope\": \"Two native instances; authored contact/reset/input fixtures; NOT arcade equivalence or full-game validation\"\n}\n";
 if(!report)throw std::runtime_error("Report write failed");std::cout<<"PASS native lifecycle replay: "<<ticks<<" ticks per instance; hits="<<hits<<" respawns="<<respawns<<" ring_kills="<<ringkills<<" blood="<<blood<<" bats="<<bats<<" shots="<<shots<<" max_objects="<<maxobjects<<" max_processes="<<maxprocesses<<" resets="<<resets<<"\n";return 0;
 }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
