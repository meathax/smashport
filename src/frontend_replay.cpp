#include "stv/frontend.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>
#include <string>
using namespace stv;namespace fs=std::filesystem;
static void okay(Status s){if(!s)throw std::runtime_error(std::string(s.message)+" @"+std::to_string(s.offset));}
static void capture(const fs::path&p,const std::vector<u32>&v){std::ofstream f(p,std::ios::binary);f<<"P6\n400 260\n255\n";for(u32 c:v){char b[3]={char(c>>16),char(c>>8),char(c)};f.write(b,3);}if(!f)throw std::runtime_error("Capture write failed");}
static u32 replay_crc(Bytes bytes){
 // Test-only lookup acceleration with the same polynomial as stv::crc32.
 static u32 table[256]{};static bool initialized=false;
 if(!initialized){for(unsigned i=0;i<256;++i){u32 c=i;for(unsigned b=0;b<8;++b)c=(c>>1)^((c&1)?0xedb88320u:0);table[i]=c;}initialized=true;}
 u32 c=~0u;for(usize i=0;i<bytes.size;++i)c=(c>>8)^table[(c^bytes.data[i])&255];return ~c;
}
static std::string hash(Bytes data){char h[65];auto s=sha256(data);hex(s.bytes,32,h);return h;}
int main(int argc,char**argv){try{
 if(argc<3||argc>5)throw std::runtime_error("Usage: stv_frontend_replay source.stva output [ticks] [render_stride=8]");
 unsigned count=argc>=4?unsigned(std::stoul(argv[3])):12000;if(count<1600||count>1000000)throw std::runtime_error("ticks must be in 1600..1000000");
 std::ifstream f(argv[1],std::ios::binary);if(!f)throw std::runtime_error("Source pack not readable");std::vector<u8>data((std::istreambuf_iterator<char>(f)),{});
 auto pack=std::make_unique<SourcePack>();okay(pack->open({data.data(),data.size()}));auto a=std::make_unique<UiFrontend>(),b=std::make_unique<UiFrontend>();okay(a->load(*pack));okay(b->load(*pack));okay(a->boot());okay(b->boot());
 fs::path out=argv[2];fs::create_directories(out);std::ofstream trace(out/"frontend-state.csv"),events(out/"frontend-events.csv"),inputs(out/"frontend-inputs.csv"),vectors(out/"audit-vectors.csv");
 trace<<"fixture_tick,native_tick,page,selection,depth,audit,difficulty,mutations,sound_requests,state_digest,frame_crc32\n";events<<"fixture_tick,native_tick,label,value\n";inputs<<"fixture_tick,closed,service,back,native_reload\n";
 vectors<<"a14,a15,a16,a17,a18,a19,player_average,elapsed_average\n";
 std::vector<u32>pixels(400*260),other(pixels.size());u32 random=0x20260922;u64 state_digest=0;unsigned reloads=0,captures=0;
 unsigned visits[unsigned(UiPage::Fault)+1]{};unsigned rendered=0;const unsigned stride=argc==5?unsigned(std::stoul(argv[4])):8;if(!stride||stride>256)throw std::runtime_error("render_stride must be 1..256");
 for(unsigned t=0;t<count;++t){
  UiInput input{};bool reload=t==3000||t==6000;
  if(reload){u8 bytes[640];okay(encode_ui_persistent(a->settings,{bytes,sizeof bytes}));UiPersistent p;okay(decode_ui_persistent({bytes,sizeof bytes},p));okay(a->boot(&p));okay(b->boot(&p));++reloads;}
  if(t>=1500&&t<2000){unsigned n=t-1500;
   input.service=n==0;
   if(n==10||n==20||n==30||n==70||n==80||n==90||n==100)input.closed=2;
   if(n==40||n==110||n==160)input.closed=0x40000;
   if(n==140)input.closed=1;
   if(n==180||n==200)input.back=true;
  }else if(t>=2000&&!(t>=3000&&t<4500)&&!(t>=6000&&t<7500)){
   const unsigned phase=t%360;
   input.service=phase==0;random=random*1664525u+1013904223u;
   if(phase%12==1){const u32 choices[6]={1,2,0x40000,0x200000,0x1000,0x2000};input.closed=choices[(random>>24)%6];}
   if(phase==300)input.back=true;
  }
  const u64 before=a->event_total;
  okay(a->tick(input));okay(b->tick(input));okay(a->validate());okay(b->validate());
  state_digest=a->digest();if(state_digest!=b->digest())throw std::runtime_error("State divergence at "+std::to_string(t));
  const bool capture_now=t==38||t==70||t==115||t==410||t==460||t==980||t==1520||t==1585||t==1645||t==3140||t==4500||t+1==count;
  u32 frame_crc=0;
  if(t%stride==0||capture_now){
   okay(a->draw({pixels.data(),400,260,400,pixels.size()}));okay(b->draw({other.data(),400,260,400,other.size()}));if(pixels!=other)throw std::runtime_error("Framebuffer divergence at "+std::to_string(t));
   frame_crc=replay_crc({reinterpret_cast<const u8*>(pixels.data()),pixels.size()*sizeof(u32)});++rendered;
  }
  ++visits[unsigned(a->page)];
  inputs<<t<<','<<input.closed<<','<<input.service<<','<<input.back<<','<<reload<<'\n';
  trace<<t<<','<<a->ticks<<','<<a->state_name()<<','<<a->selection<<','<<a->menu_depth<<','<<a->audit_page<<','<<a->settings.adjustments[13]<<','<<a->mutations<<','<<a->sound_requests<<','<<state_digest<<','<<frame_crc<<'\n';
  for(u64 e=before;e<a->event_total;++e){const auto&event=a->events[e%256];events<<t<<','<<event.tick<<','<<event.label<<','<<event.value<<'\n';}
  if(capture_now){capture(out/("frontend-"+std::to_string(t)+".ppm"),pixels);++captures;}
 }
 UiPersistent audit;audit.factory_all();char player[96],elapsed[96];
 for(unsigned i=0;i<4096;++i){for(unsigned j=14;j<=19;++j){random=random*1664525u+1013904223u;audit.game_audits[j]=i<2048?(random>>16):random;vectors<<audit.game_audits[j]<<',';}
  okay(ui_audit_text(audit,{"","DO_AVGT",0},player,sizeof player));okay(ui_audit_text(audit,{"","DO_ELAPSE",0},elapsed,sizeof elapsed));vectors<<'"'<<player<<"\",\""<<elapsed<<"\"\n";
 }
 u8 persistent[640];okay(encode_ui_persistent(a->settings,{persistent,sizeof persistent}));std::ofstream state(out/"native-operator-state.bin",std::ios::binary);state.write(reinterpret_cast<const char*>(persistent),sizeof persistent);
 std::ofstream summary(out/"summary.json");summary<<"{\n  \"instances\":2,\"ticks_per_instance\":"<<count<<",\"framebuffers_compared\":"<<rendered<<",\"render_stride\":"<<stride<<",\"captures\":"<<captures<<",\"explicit_native_reloads\":"<<reloads<<",\"audit_vectors\":4096,\n  \"final_state_digest\":\""<<state_digest<<"\",\"persistent_sha256\":\""<<hash({persistent,sizeof persistent})<<"\",\n  \"page_visits\":[";for(unsigned i=0;i<=unsigned(UiPage::Fault);++i){if(i)summary<<',';summary<<visits[i];}summary<<"],\n  \"scope\":\"Authored UI inputs, two native instances, explicit persistence reloads. NOT arcade/PCB differential, original credit flow, or Windows execution.\"\n}\n";
 if(!inputs||!events||!trace||!summary||!vectors||!state)throw std::runtime_error("Evidence write failed");
 std::cout<<"PASS frontend native/native replay: "<<count<<" states, "<<rendered<<" framebuffers, "<<reloads<<" native reloads, 4096 audit vectors\n";return 0;
 }catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
