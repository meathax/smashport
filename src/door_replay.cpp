#include "stv/source_lab.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
using namespace stv;namespace fs=std::filesystem;
static void ok(Status s){if(!s)throw std::runtime_error(std::string(s.message)+" @"+std::to_string(s.offset));}
static std::string digest(Sha256 s){char b[65];hex(s.bytes,32,b);return b;}
static void ppm(const fs::path& path,const std::vector<u32>& p){std::ofstream f(path,std::ios::binary);f<<"P6\n400 260\n255\n";for(u32 c:p){char rgb[]={char(c>>16),char(c>>8),char(c)};f.write(rgb,3);}if(!f)throw std::runtime_error("Cannot write PPM");}
int main(int argc,char** argv){try{
 if(argc!=3)throw std::runtime_error("Usage: stv_door_replay source.stva output-directory");
 std::ifstream f(argv[1],std::ios::binary);if(!f)throw std::runtime_error("Source pack missing");std::vector<u8> bytes((std::istreambuf_iterator<char>(f)),{});auto pack=std::make_unique<SourcePack>();ok(pack->open({bytes.data(),bytes.size()}));
 auto a=std::make_unique<SourceLab>(),b=std::make_unique<SourceLab>();ok(a->load(*pack));ok(b->load(*pack));ok(a->reset(0x13579bdf,2,8,true));ok(b->reset(0x13579bdf,2,8,true));fs::path out=argv[2];fs::create_directories(out);std::vector<u32> pixels(400*260),other(pixels.size());
 ok(a->draw({pixels.data(),400,260,400,pixels.size()}));ppm(out/"linked-arena-before-processes.ppm",pixels);
 for(auto& p:pixels)p=editor_color(u16(6*32*32+6*32+6));ok(a->arena.draw_background({pixels.data(),400,260,400,pixels.size()}));ppm(out/"linked-static-background.ppm",pixels);
 std::ofstream csv(out/"door-state-trace.csv");csv<<"tick,halt,left_timer,right_timer,top_timer,left_view,right_view,top_view,requests,processes,frames,completions,state_sha256\n";
 for(unsigned t=0;t<800;++t){if(t==0||t==200||t==400){for(unsigned g=1;g<=3;++g){ok(a->open_source_gate(g,t==200?0xffffffffu:12));ok(b->open_source_gate(g,t==200?0xffffffffu:12));}}if(t==300){for(unsigned g=1;g<=3;++g){ok(a->open_source_gate(g,8));ok(b->open_source_gate(g,8));}}a->halted=b->halted=t>=30&&t<60;ok(a->tick({}));ok(b->tick({}));ok(a->validate());ok(b->validate());auto da=a->state_digest(),db=b->state_digest();if(!equal(da.bytes,db.bytes,32))throw std::runtime_error("Door replay state divergence at "+std::to_string(t));ok(a->draw({pixels.data(),400,260,400,pixels.size()}));ok(b->draw({other.data(),400,260,400,other.size()}));if(pixels!=other)throw std::runtime_error("Door replay framebuffer divergence at "+std::to_string(t));csv<<t<<','<<a->halted;for(unsigned i=0;i<3;++i)csv<<','<<unsigned(a->doors.timer[i]);for(unsigned i=0;i<3;++i)csv<<','<<unsigned(a->doors.view[i]);csv<<','<<a->stats.gate_requests<<','<<a->stats.door_processes<<','<<a->stats.door_frames<<','<<a->stats.door_completions<<','<<digest(da)<<'\n';if(t==0||t==3||t==6||t==9||t==30||t==90||t==140||t==220||t==380||t==799)ppm(out/("gate-"+std::to_string(t)+".ppm"),pixels);}
 std::ofstream summary(out/"summary.json");summary<<"{\n  \"instances\": 2, \"ticks_per_instance\": 800, \"framebuffers_compared\": 800,\n  \"gate_requests\": "<<a->stats.gate_requests<<", \"gate_processes\": "<<a->stats.door_processes<<", \"gate_frames\": "<<a->stats.door_frames<<", \"gate_completions\": "<<a->stats.door_completions<<",\n  \"final_state_sha256\": \""<<digest(a->state_digest())<<"\",\n  \"scope\": \"Authored gate/HALT fixture; native/native comparison, NOT original arcade progression or hardware timing\"\n}\n";
 if(!csv||!summary)throw std::runtime_error("Door evidence write failed");std::cout<<"PASS two-instance 800-tick door replay; frames="<<a->stats.door_frames<<" completions="<<a->stats.door_completions<<'\n';return 0;
 }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
