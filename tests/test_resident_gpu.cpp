#include "stv/host/camera_motion.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "stv/host/gpu_resident.hpp"
#include <algorithm>
#include "../src/host/title_background.hpp"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <thread>
#include <set>
using namespace stv::host;
PcSettings isolated_settings() {
 PcSettings s;
 s.room_cinema=s.atmosphere=s.sprite_rim=s.contact_shadows=s.wet_materials=0;
 s.heat_distortion=s.emissive_spill=s.cinematic_grade=s.combat_feedback=0;
 return s;
}
// Report where an access violation happens (module + offset) before dying.
static LONG WINAPI crash_report(EXCEPTION_POINTERS* e){
 void* at=e->ExceptionRecord->ExceptionAddress;HMODULE mod{};wchar_t name[MAX_PATH]=L"?";
 GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCWSTR)at,&mod);
 if(mod)GetModuleFileNameW(mod,name,MAX_PATH);
 std::fprintf(stderr,"CRASH code=%08lx at %ls+0x%llx thread=%lu\n",e->ExceptionRecord->ExceptionCode,name,
  (unsigned long long)((char*)at-(char*)mod),GetCurrentThreadId());
 void* frames[24];USHORT n=CaptureStackBackTrace(0,24,frames,nullptr);
 for(USHORT i=0;i<n;i++){HMODULE m{};wchar_t mn[MAX_PATH]=L"?";GetModuleHandleExW(6,(LPCWSTR)frames[i],&m);if(m)GetModuleFileNameW(m,mn,MAX_PATH);
  std::fprintf(stderr,"  #%u %ls+0x%llx\n",i,mn,(unsigned long long)((char*)frames[i]-(char*)m));}
 return EXCEPTION_CONTINUE_SEARCH;}

#define REQUIRE(x) if(!(x)){std::fprintf(stderr,"FAIL line %d: %s | %s\n",__LINE__,#x,gpu.error().c_str());return 1;}
int main(int argc,char** argv){
 std::setvbuf(stdout,nullptr,_IONBF,0); // a crash must not lose the progress already printed
 AddVectoredExceptionHandler(1,[](EXCEPTION_POINTERS* e)->LONG{return e->ExceptionRecord->ExceptionCode==EXCEPTION_ACCESS_VIOLATION?crash_report(e):EXCEPTION_CONTINUE_SEARCH;});
 bool benchmark=argc>1;PcSettings s=isolated_settings();s.enhance=s.ray_tracing=s.voxels=true;
 const bool sharp4k=benchmark && std::string(argv[1])=="sharp4k";
 const bool wide_smoke_workload=benchmark && std::string(argv[1])=="wide-boss-smoke";
 const bool boss_smoke_workload=benchmark && (std::string(argv[1])=="boss-smoke" || wide_smoke_workload);
 const int nativeWidth=wide_smoke_workload?547:530;
 const bool integrated_workload=benchmark && (std::string(argv[1])=="visual-fx" || boss_smoke_workload);
 const bool arena_workload=benchmark &&
     (sharp4k || integrated_workload || std::string(argv[1])=="arena-on" || std::string(argv[1])=="arena-off");
 if(integrated_workload)enable_all_visual_effects(s);
 if(boss_smoke_workload)s.hardware_rt=true;
 if(arena_workload&&!integrated_workload)
     s.shockwaves=s.tracers=s.brass=sharp4k || std::string(argv[1])=="arena-on";
 if(sharp4k){s.persistent_gore=s.materials=s.destruction=s.weapon_impacts=s.fluid_smoke=true;s.filter=4;s.glow=s.sharpen=s.curvature=1;}
 const int width=sharp4k?3840:1920,height=sharp4k?2160:1080;
 EffectScene scene{1,1,nativeWidth,256,0,0,true,{}};
 if(sharp4k)scene.in_arena=true;
 if(sharp4k)scene.studio={60,40,250,155,480,120,true,false};
 if(sharp4k)scene.lamps={RoomLamp{105,33,20,194,1,.79f,.46f,1.12f},RoomLamp{505,191,15,148,.43f,.81f,1,.78f},RoomLamp{265,32,22,184,1,.79f,.46f,.78f}};
 for(int i=0;i<24;i++){float x=30.f+(i%8)*62,y=55.f+(i/8)*66;scene.casters.push_back({x,y,16,28,y+28,0,float(i==0)});scene.blockers.push_back({{x,y,0},{x+12,y+10,20},0,false});}
 std::vector<uint32_t> source(nativeWidth*256,0x405060),pixels;
 ResidentEffects gpu;
 // Differential capture fixture for stable GPU tile ordering, overflow, clipped
 // trails, partial edge tiles, resize, and clearing after a populated frame.
 if(benchmark && std::string(argv[1])=="combat-bins"){
  PcSettings opt=isolated_settings();opt.enhance=true;
  opt.combat_feedback=opt.weapon_signatures=opt.temporal_trails=3;
  // Captured from the prior CPU-bin renderer, Release RTX 4090, 21 Nov 2026.
  constexpr uint64_t expected[]={0xcd268b380278940bull,0x9ecbdc5ff435716dull,0x15a6dd1c3c0e9dbdull,0x60573040bfec4b25ull,
   0x062297a5d3aec9efull,0xa56b67e97e69744dull,0x3557f8d41a4db36eull,0xd8fd0aacb34dbf25ull,
   0x79fbd77a5afce78bull,0x280b014cde9c6349ull,0xd72d1759f2da2f87ull,0xe7d18d11b9e5ab25ull};
  unsigned fixture=0;
  for(int w:{410,547,530})for(int variant=0;variant<4;++variant){
   EffectPacket packet;packet.serial=100+w*4+variant;packet.in_arena=true;
   packet.weaponFx.event_count=variant==3?0:96;
   for(unsigned i=0;i<packet.weaponFx.event_count;++i){
    auto& e=packet.weaponFx.events[i];
    e.x=variant==0?80.f:float((i*53)%(w+40))-20.25f;
    e.y=variant==0?80.f:float((i*37)%296)-20.75f;
    e.dx=-17;e.dy=11;e.strength=.5f;e.family=WeaponFamily(i%6);e.kind=WeaponFxKind(i%4);
   }
   packet.weaponFx.history_count=variant==2?6:0;
   for(unsigned f=0;f<packet.weaponFx.history_count;++f){
    auto& h=packet.weaponFx.history[f];h.sample_count=48;
    for(unsigned i=0;i<48;++i)h.samples[i]={i,float((i*31)%(w+30))-15.f-f*9,float((i*43)%276)-10.f+f*7,0,0,WeaponFamily(i%6)};
   }
   std::vector<uint32_t> background(w*256,0x405060);
   REQUIRE(gpu.update(background,w,256,packet,opt));REQUIRE(gpu.capture(pixels));
   uint64_t hash=14695981039346656037ull;
   for(auto pixel:pixels){hash^=pixel;hash*=1099511628211ull;}
   std::printf("combat width=%d variant=%d hash=%016llx\n",w,variant,(unsigned long long)hash);
   REQUIRE(hash==expected[fixture++]);
   if(variant==3){REQUIRE(pixels==background);}else{REQUIRE(pixels!=background);}
   auto frozen=pixels;REQUIRE(gpu.recompose(packet,opt));REQUIRE(gpu.capture(pixels));REQUIRE(pixels==frozen);
  }
  return 0;
 }
 auto loadPpm=[&](const std::filesystem::path& path,int& w,int& h)->std::vector<uint32_t>{
  std::ifstream input(path,std::ios::binary);
  std::string magic;int maximum=0;if(!(input>>magic>>w>>h>>maximum)||magic!="P6"||maximum!=255)throw std::runtime_error("Invalid P6 capture");
  input.get();
  std::vector<unsigned char> bytes(size_t(w)*h*3);
  if(!input.read(reinterpret_cast<char*>(bytes.data()),std::streamsize(bytes.size())))throw std::runtime_error("Truncated P6 capture");
  std::vector<uint32_t> image(size_t(w)*h);
  for(size_t i=0;i<image.size();++i)image[i]=uint32_t(bytes[i*3])<<16|uint32_t(bytes[i*3+1])<<8|bytes[i*3+2];
  return image;
 };
 auto savePpm=[&](const std::filesystem::path& path,int w,int h,const std::vector<uint32_t>& image){
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path,std::ios::binary);output<<"P6\n"<<w<<' '<<h<<"\n255\n";
  for(auto rgb:image){unsigned char bytes[3]={static_cast<unsigned char>(rgb>>16),
                                              static_cast<unsigned char>(rgb>>8),
                                              static_cast<unsigned char>(rgb)};
   output.write(reinterpret_cast<const char*>(bytes),3);}
  if(!output)throw std::runtime_error("Write P6 capture");
 };
 if(argc==4 && std::string(argv[1])=="room-captures"){
  const std::filesystem::path root=argv[2],out=argv[3];
  std::map<uint32_t,std::pair<unsigned,std::filesystem::path>> selected;
  for(const auto& entry:std::filesystem::recursive_directory_iterator(root)){
   if(!entry.is_regular_file() || entry.path().extension()!=".ppm")continue;
   unsigned circuit=0,wave=0,frame=0;
   if(std::sscanf(entry.path().filename().string().c_str(),"effects-%u-%u-%u.ppm",&circuit,&wave,&frame)!=3)continue;
   const uint32_t room=(circuit<<16)|wave;
   auto found=selected.find(room);
   if(found==selected.end() || frame<found->second.first)selected[room]={frame,entry.path()};
  }
  unsigned required=0;
  const unsigned waves[]={10u,18u,22u};
  for(unsigned c=0;c<3;++c)for(unsigned w=1;w<=waves[c];++w)
   if(c==0 || w!=10)++required;
  REQUIRE(selected.size()==required);
  REQUIRE(gpu.prepare_dxr());
  PcSettings opt=isolated_settings();opt.hardware_rt=true;
  std::filesystem::create_directories(out);
  std::ofstream csv(out/"room-lighting.csv");
  csv<<"room,source_frame,path,width,height,changed_pixels\n";
  for(const auto& [room,sourceCapture]:selected){
   int w=0,h=0;auto capture=loadPpm(sourceCapture.second,w,h);
   REQUIRE(w==547 && h==256);
   EffectPacket packet;packet.in_arena=true;packet.room=room;packet.frame=sourceCapture.first;
   packet.geometry.floor={33,46,480,195};
   REQUIRE(gpu.update(capture,w,h,packet,opt) && gpu.capture(pixels));
   unsigned changed=0;for(size_t i=0;i<pixels.size();++i)changed+=pixels[i]!=capture[i];
   REQUIRE(changed>1000);
   const std::string stem="room-"+std::to_string(room>>16)+"-"+std::to_string(room&65535);
   savePpm(out/(stem+".ppm"),w,h,pixels);
   csv<<room<<','<<sourceCapture.first<<','<<sourceCapture.second.generic_string()<<','<<w<<','<<h<<','<<changed<<'\n';
  }
  REQUIRE(bool(csv));
  std::printf("Relit %zu retained retail room captures with 53-profile DXR into %s\n",selected.size(),out.string().c_str());
  return 0;
 }
 // A HUD arrow must not claim same-palette floor pixels in its search rectangle.
 {
  stv::host::ResidentEffects gpu;std::vector<uint32_t> pixels;
  PcSettings opt=isolated_settings();opt.enhance=true;opt.atmosphere=3;
  EffectPacket packet{};packet.in_arena=true;packet.room=10;packet.frame=1;packet.serial=1;
  packet.geometry.floor={29,46,343,195};
  std::vector<uint16_t> indexed(410*256,0x81);
  std::vector<uint32_t> pens(4096,0x606060);pens[0x82]=0xffffff;
  indexed[140*410+205]=0x1082;
  REQUIRE(gpu.update_arcade(indexed,410,410,256,pens,false,false,packet,opt,true));
  REQUIRE(gpu.capture(pixels));const auto withoutHud=pixels;
  packet.hud.push_back({{200,130,10,20},128});
  REQUIRE(gpu.update_arcade(indexed,410,410,256,pens,false,false,packet,opt,true));
  REQUIRE(gpu.capture(pixels));
  REQUIRE(pixels==withoutHud);
 }
 // Visual check on a real frame: boss-room BMP (any 24-bit capture), blood
 // bursts against the rear wall/hole and a player walking out of the mess.
 if(argc==4 && std::string(argv[1])=="boss-room"){
  FILE* f=std::fopen(argv[2],"rb");if(!f)return 2;
  std::vector<unsigned char> bmp(4<<20);bmp.resize(std::fread(bmp.data(),1,bmp.size(),f));std::fclose(f);
  auto le=[&](size_t at){return int(bmp[at]|bmp[at+1]<<8|bmp[at+2]<<16|bmp[at+3]<<24);};
  const int w=le(18),h=le(22),off=le(10),stride=(w*3+3)&~3;
  if(h!=256||w<300||w>1024)return 2;
  std::vector<uint32_t> room(size_t(w)*h);
  for(int y=0;y<h;++y)for(int x=0;x<w;++x){const auto* p=&bmp[off+size_t(h-1-y)*stride+x*3];room[size_t(y)*w+x]=p[2]<<16|p[1]<<8|p[0];}
  PcSettings opt=isolated_settings();opt.enhance=opt.persistent_gore=opt.blood=opt.debris=opt.destruction=true;
  EffectPacket p{};p.in_arena=true;p.room=5;p.frame=1;p.serial=1;
  p.geometry.floor={40,8,float(w-80),234}; // reaches into the torn rear wall
  auto frame=[&](){++p.frame;++p.serial;p.dt=.01827925f;return gpu.update(room,w,256,p,opt) && gpu.capture(pixels);};
  for(int k=0;k<6;++k)p.spawns.push_back({w*.25f+k*w*.1f,60,18,256,14,float(1+k%2)});
  for(int k=0;k<14;++k)p.spawns.push_back({w*.22f+k*22.f,k%2?22.f:66.f,11,1,0,-1,0,12});
  REQUIRE(frame());p.spawns.clear();
  for(int i=0;i<160;++i)REQUIRE(frame());
  // Walk from the pool towards the lower right: alternate feet 5px apart.
  float x=w*.45f,y=72;
  for(int i=0;i<60;++i){
   p.spawns.clear();x+=3.f;y+=4.f;const float side=(i&1)?1.5f:-1.5f;
   p.spawns.push_back({x-side*.8f,y+side*.6f,11,1,.6f,.8f,2,1});
   REQUIRE(frame());
  }
  for(int i=0;i<30;++i){p.spawns.clear();REQUIRE(frame());}
  std::string out=argv[3];
  for(auto [name,img]:{std::pair{"boss-room-source.ppm",&room},std::pair{"boss-room-blood.ppm",&pixels}}){
   FILE* o=std::fopen((out+"/"+name).c_str(),"wb");if(!o)return 3;
   std::fprintf(o,"P6\n%d 256\n255\n",w);
   for(auto c:*img){unsigned char rgb[3]={(unsigned char)(c>>16),(unsigned char)(c>>8),(unsigned char)c};std::fwrite(rgb,1,3,o);}
   std::fclose(o);
  }
  std::puts("boss-room visual written");return 0;
 }
 if(benchmark && (std::string(argv[1])=="dxr" || std::string(argv[1])=="dxr-seams")){
  if(!gpu.prepare_dxr()){
   const auto& why=gpu.dxr_error();
   std::fprintf(stderr,"DXR unavailable: %s\n",why.c_str());
   return why=="DXR 1.1 required" || why=="Hardware adapter required" ||
          why.starts_with("D3D12 hardware device") ? 77 : 2;
  }
  PcSettings opt=isolated_settings();opt.hardware_rt=true;
  EffectPacket packet;packet.in_arena=true;packet.roomIntro=true;packet.geometry.floor={24,50,476,185};
  auto render=[&](){++packet.serial;return gpu.update(source,nativeWidth,256,packet,opt) && gpu.capture(pixels);};
  REQUIRE(render());auto entrance=pixels;REQUIRE(entrance!=source);
  packet.casters.push_back({90,114,18,35,149,0,1});
  REQUIRE(render());REQUIRE(pixels!=entrance);
  // Every authored selector is lit; each room cue gives a deterministic unique
  // rig picture, not merely one image per broad mood family.
  packet.roomIntro=false;
  std::vector<std::vector<uint32_t>> moods;
  std::vector<uint32_t> rooms;
  const unsigned waveCount[]={11u,19u,23u};
  for(unsigned c=0;c<3;++c)
   for(unsigned wave=1;wave<=waveCount[c];++wave)rooms.push_back((c<<16)|wave);
  REQUIRE(rooms.size()==53 && rooms.size()==room_lighting_profile_count());
  for(uint32_t room:rooms){
   packet.room=room;packet.frame=600;REQUIRE(render());REQUIRE(pixels!=source);
   for(const auto& other:moods)REQUIRE(pixels!=other);
   auto first=pixels;REQUIRE(render());REQUIRE(pixels==first);
   moods.push_back(first);
  }
  packet.room=10;REQUIRE(render());auto mutoid=pixels;REQUIRE(mutoid!=source);
  packet.foreground.push_back({0,130,86,50,52,true,0x9801});
  REQUIRE(render());auto boss=pixels;REQUIRE(boss!=mutoid);
  packet.dxrLights={{150,145,18,95,1,.73f,.31f,3}};REQUIRE(render());REQUIRE(pixels!=boss);
  packet.frame=777;REQUIRE(render());REQUIRE(pixels!=boss); // animated rig
  packet.geometry.floor.w=0;REQUIRE(render());REQUIRE(pixels==source);
  packet.geometry.floor.w=476;packet.in_arena=false;REQUIRE(render());REQUIRE(pixels==source);
  packet.in_arena=true;opt.hardware_rt=false;REQUIRE(render());REQUIRE(pixels==source);
  // Changing a boss's sprite partition must not create lighting seams.
  for(int layout:{0,1,2}){
   // Classic, full 547-pixel gameplay, and a 410-pixel page centered in 547.
   const int inputWidth=layout==1?547:410,outputWidth=layout?547:410;
   const int rightSide=layout==1?300:0,wingShift=layout==2?68:0;
   auto inputAt=[&](int x,int y){return y*inputWidth+x+rightSide;};
   auto outputAt=[&](int x,int y){return y*outputWidth+x+rightSide+wingShift;};
   opt.hardware_rt=true;
   ResidentEffects assembled;REQUIRE(assembled.prepare_dxr());
   std::vector<uint16_t> ink(inputWidth*256,0x81);
   std::vector<uint32_t> colors(4096,0x606060);
   for(int y=80;y<144;++y)for(int x=100;x<164;++x)ink[inputAt(x,y)]=0x1081;
   for(unsigned room:{1u,10u,0x10007u,0x20007u,0x20015u}){
    EffectPacket pose;pose.in_arena=true;pose.room=room;pose.frame=500;pose.geometry.floor=layout==1?HudRect{33,46,480,195}:HudRect{29,46,343,195};
    const uint16_t oid=room==0x10007?0x9900:room==0x20007?0x8a00:room==1?0x8000:0x9801;
    auto part=[&](float x,float y,float w,float h){
     x+=float(rightSide);
     pose.foreground.push_back({0,x,y,w,h,true,oid,128});
     pose.casters.push_back({x,y,w,h,y+h,128,0,2});
    };
    auto draw=[&](){++pose.serial;return assembled.update_arcade(ink,inputWidth,inputWidth,256,colors,layout!=0,false,pose,opt,true) && assembled.dxr_frame()!=nullptr && assembled.capture(pixels);};
    part(100,80,64,64);REQUIRE(draw());const auto continuous=pixels;
    if(layout==2)for(int y=0;y<256;++y)for(int x:{0,67,478,546})
     REQUIRE(pixels[y*outputWidth+x]==0); // no lighting leaks into centered-page wings
    // HUD bounds must never cut through foreground art. Unknown indexed
    // ownership is not permission to protect every pixel in a rectangle.
    for(int bank:{-1,256}){
     pose.hud={{{96.f+rightSide,76,76,34},bank}};REQUIRE(draw());
     for(int y=84;y<140;++y)for(int x=104;x<160;++x)REQUIRE(pixels[outputAt(x,y)]==continuous[outputAt(x,y)]);
    }
    pose.hud={{{96.f+rightSide,76,76,34},256,true}};REQUIRE(draw());
    for(int y=84;y<140;++y)for(int x=104;x<160;++x)REQUIRE(pixels[outputAt(x,y)]==continuous[outputAt(x,y)]);
    pose.hud.clear();
    // Genuine displayed HUD ink still stays ungraded when a boss crosses it.
    ink[inputAt(110,90)]=0x1101;pose.hud={{{96.f+rightSide,76,76,34},256}};
    REQUIRE(draw());REQUIRE(pixels[outputAt(110,90)]==colors[257]);
    ink[inputAt(110,90)]=0x101;pose.hud[0].backdrop=true;
    REQUIRE(draw());REQUIRE(pixels[outputAt(110,90)]==colors[257]);
    ink[inputAt(110,90)]=0x1081;pose.hud.clear();
    for(bool horizontal:{false,true}){
     pose.foreground.clear();pose.casters.clear();
     if(horizontal){part(100,80,64,36);part(100,108,64,36);}
     else{part(100,80,36,64);part(128,80,36,64);}
     REQUIRE(draw());
     for(int y=84;y<140;++y)for(int x=104;x<160;++x)REQUIRE(pixels[outputAt(x,y)]==continuous[outputAt(x,y)]);
    }
    // Displayed pixels can span a gap between current sprite-part rectangles.
    // The assembled surface must not drop into fallback lighting in that strip.
    pose.foreground.clear();pose.casters.clear();
    part(100,80,28,64);part(136,80,28,64);REQUIRE(draw());
    for(int y=84;y<140;++y)for(int x=104;x<160;++x)REQUIRE(pixels[outputAt(x,y)]==continuous[outputAt(x,y)]);
    // An overlapping object whose palette is absent from the displayed pixels
    // must not steal a strip of the boss's lighting or cast a phantom shadow.
    pose.casters.push_back({120.f+rightSide,72,12,80,152,256,0,1});
    REQUIRE(draw());
    for(int y=84;y<140;++y)for(int x=104;x<160;++x)REQUIRE(pixels[outputAt(x,y)]==continuous[outputAt(x,y)]);
    if(room==1 || room==0x20007){
     // No per-piece material change or missing observer rectangle may mark
     // a straight line on displayed ordinary actors or segmented Cobra art.
     pose.casters[0].material=4;pose.casters[1].material=5;REQUIRE(draw());
     for(int y=84;y<140;++y)for(int x=104;x<160;++x)REQUIRE(pixels[outputAt(x,y)]==continuous[outputAt(x,y)]);
     pose.casters.clear();pose.foreground.clear();
     // Removing Cobra targets also removes its follow lights, so test absent
     // rectangles independently in the ordinary room with an unchanged rig.
     if(room==1){REQUIRE(draw());
      for(int y=84;y<140;++y)for(int x=104;x<160;++x)REQUIRE(pixels[outputAt(x,y)]==continuous[outputAt(x,y)]);}
     // A flat sprite crossing the rear wall's projected y=22 edge must retain
     // a smooth light field instead of receiving a hard proxy-wall shadow.
     std::fill(ink.begin(),ink.end(),0x81);
     for(int y=0;y<64;++y)for(int x=100;x<164;++x)ink[inputAt(x,y)]=0x1081;
     part(100,0,64,64);REQUIRE(draw());
     for(int y=18;y<27;++y)for(int x=104;x<160;++x)for(int shift:{0,8,16})
      REQUIRE(std::abs(int((pixels[outputAt(x,y)]>>shift)&255)-int((pixels[outputAt(x,y+1)]>>shift)&255))<20);
     std::fill(ink.begin(),ink.end(),0x81);
     for(int y=80;y<144;++y)for(int x=100;x<164;++x)ink[inputAt(x,y)]=0x1081;
    }
   }
  }
  if(std::string(argv[1])=="dxr-seams"){std::puts("PASS: Classic, widescreen right-side and centered-page sprite/HUD/wall seam checks pass");return 0;}
  // Cost of a heavy frame: Pleasure Dome rig, 60 actors and 12 combat lights,
  // submitted and fenced per frame (the player never waits on this fence).
  packet.room=0x20016;packet.foreground.clear();packet.casters.clear();
  for(int i=0;i<60;i++){float x=40.f+(i%12)*38,y=70.f+(i/12)*32;packet.casters.push_back({x,y,16,28,y+28,float(64*(i%8)),0,float(1+(i&1))});}
  packet.dxrLights.clear();for(int i=0;i<12;i++)packet.dxrLights.push_back({60.f+i*36,90.f+(i%3)*50,14,90,1,.7f,.3f,1.4f});
  bool timedOk=true;
  auto timed=[&](bool rt){opt.hardware_rt=rt;std::vector<double> ms;
   for(int f=0;f<240;f++){packet.frame=1000+f;++packet.serial;auto t0=std::chrono::steady_clock::now();
    timedOk=timedOk && gpu.update(source,nativeWidth,256,packet,opt) && gpu.finish();
    ms.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t0).count());}
   std::sort(ms.begin(),ms.end());return std::pair{ms[ms.size()/2],ms[ms.size()*95/100]};};
  auto base=timed(false),rt=timed(true);REQUIRE(timedOk);
  std::printf("DXR heavy frame median %.3f ms p95 %.3f ms (raster-only median %.3f ms p95 %.3f ms)\n",rt.first,rt.second,base.first,base.second);
  REQUIRE(rt.second<8.0);
  packet.casters.clear();
  for(int i=0;i<420;++i){
   const float x=24.f+(i%28)*18.f,y=50.f+(i/28)*9.f;
   packet.casters.push_back({x,y,10,12,y+12,float(64*(i%8)),float(i%2),float(i%5+1)});
  }
  packet.serial++;REQUIRE(gpu.update(source,nativeWidth,256,packet,opt) && gpu.finish());
  REQUIRE(gpu.dxr_error().empty());
  std::printf("DXR 1.1 GPU readbacks verified entrance, actor occlusion, 53 authored room cues, bosses, combat light, animation and bypass on %s\n",gpu.adapter().c_str());
  return 0;
 }
 for(int trial=0;trial<(boss_smoke_workload?4:benchmark?3:1);trial++){
  if(boss_smoke_workload){
   const unsigned rooms[]={10,0x10007,0x20007,0x20015};scene.room=rooms[trial];
   scene.in_arena=scene.studio.boss=true;scene.geometry.floor=wide_smoke_workload?HudRect{33,46,480,195}:HudRect{29,46,472,195};
   scene.targets={{1,230,70,64,104,true,uint16_t(trial==1?0x9900:trial==2?0x8a00:0x9801),0}};
   std::printf("boss smoke stress room=%u, 8 explosions/12 frames plus 12 hostile projectiles\n",scene.room);
  }
  EnhanceEffects fx;fx.use_gpu_simulation(true);fx.advance(scene,s);REQUIRE(gpu.update(source,nativeWidth,256,fx.packet(),s));
  if(boss_smoke_workload)REQUIRE(gpu.dxr_frame()!=nullptr);
  double advance=0,submit=0,complete=0;std::vector<double> times;
  for(int frame=0;frame<(benchmark?420:100);frame++){
   scene.frame++;scene.objects.clear();
   if(frame%12==0)for(int i=0;i<8;i++)scene.objects.push_back({uint32_t(100+frame*8+i),0,EffectKind::Explosion,float(30+i*62),float(75+(frame%3)*40)});
   if(sharp4k && frame%7==0)scene.objects.push_back({uint32_t(90000+frame),0,frame%2?EffectKind::MetalBreak:EffectKind::FleshBreak,240,140});
   if(arena_workload)for(int i=0;i<12;i++){
    int age=(frame+i*3)%16;
    scene.objects.push_back({uint32_t(100000+i*1000+(frame+i*3)/16),0,boss_smoke_workload?EffectKind::EnemyShot:EffectKind::Projectile,float(32+i*35+age*3),float(70+(i%4)*32),3,0,0x4201});
   }
   auto a=std::chrono::steady_clock::now();fx.advance(scene,s);auto b=std::chrono::steady_clock::now();
   REQUIRE(gpu.update(source,nativeWidth,256,fx.packet(),s));REQUIRE(gpu.draw(nullptr,width,height,0,0,width,height,s));REQUIRE(gpu.present());auto c=std::chrono::steady_clock::now();REQUIRE(gpu.finish());auto d=std::chrono::steady_clock::now();
   auto ms=[](auto x,auto y){return std::chrono::duration<double,std::milli>(y-x).count();};
   if(frame>=60){advance+=ms(a,b);submit+=ms(b,c);complete+=ms(c,d);times.push_back(ms(a,d));}
  }
  REQUIRE(gpu.readback_bytes()==0);std::sort(times.begin(),times.end());auto n=double(times.size());
  std::printf("trial=%d advance_ms=%.6f submit_ms=%.6f gpu_completion_ms=%.6f total_ms=%.6f p95_ms=%.6f max_ms=%.6f readback_bytes=%llu adapter=%s\n",trial,advance/n,submit/n,complete/n,(advance+submit+complete)/n,times[size_t(times.size()*.95)],times.back(),(unsigned long long)gpu.readback_bytes(),gpu.adapter().c_str());
  if(benchmark)continue;
  REQUIRE(gpu.capture(pixels));REQUIRE(pixels!=source);auto paused=pixels;
  REQUIRE(gpu.update(source,nativeWidth,256,fx.packet(),s));REQUIRE(gpu.capture(pixels));REQUIRE(pixels==paused);
  s.enhance=false;s.room_cinema=0;fx.advance(scene,s);
  REQUIRE(gpu.update(source,nativeWidth,256,fx.packet(),s));REQUIRE(gpu.capture(pixels));REQUIRE(pixels==source);
  s.enhance=true;fx.reset();REQUIRE(gpu.update(source,nativeWidth,256,fx.packet(),s));REQUIRE(gpu.capture(pixels));REQUIRE(pixels==source);
  // Compare the new indexed path against independently expanded RGB, including
  // source pitch, palette masking, exact title dimming and both wing seams.
  for(int mode=0;mode<4;mode++){
   const int w=mode==3?547:410, h=256, stride=w+29;
   std::vector<uint16_t> indexed(stride*h);
   std::vector<uint32_t> pens(4096),expected(w*h);
   for(size_t i=0;i<pens.size();i++)pens[i]=(i*0x1327u)&0xffffff;
   for(int y=0;y<h;y++)for(int x=0;x<w;x++){
    indexed[y*stride+x]=uint16_t(0xf000u|((x*13+y*7)&4095));
    expected[y*w+x]=pens[indexed[y*stride+x]&4095];
   }
   int expanded=w;extend_title(expected,expanded,mode!=0,mode==2);
   PcSettings clean=s;clean.enhance=false;
   REQUIRE(gpu.update_arcade(indexed,stride,w,h,pens,mode!=0,mode==2,fx.packet(),clean));
   REQUIRE(gpu.capture(pixels));REQUIRE(pixels==expected);
   REQUIRE(gpu.recompose(fx.packet(),clean));REQUIRE(gpu.capture(pixels));REQUIRE(pixels==expected);
  }
  {
   PcSettings opt=isolated_settings();opt.enhance=true;
   opt.combat_feedback=opt.hit_flash=3;
   EffectPacket flash;flash.serial=180;flash.in_arena=true;
   flash.weaponFx.director.hit_flash=1;
   std::vector<uint16_t> indexed(410*256,0);
   std::vector<uint32_t> pens(4096,0x101010);pens[1]=0x202020;
   indexed[100*410+100]=0x1001;
   REQUIRE(gpu.update_arcade(indexed,410,410,256,pens,false,false,flash,opt,true));
   REQUIRE(gpu.draw(nullptr,410,256,0,0,410,256,opt,false,false,0,0));
   REQUIRE(gpu.capture(pixels,true));
   REQUIRE((pixels[100*410+100]&0xffffff)==0x202020);
   REQUIRE((pixels[20*410+20]&0xffffff)!=0x101010);
  }
  // Generate each effect independently through the player event path.
  for(int effect: {0,2}){
   PcSettings opt=isolated_settings();opt.enhance=true;
   opt.particles=opt.blood=opt.debris=opt.smoke=opt.fire=opt.lighting=false;
   opt.shockwaves=effect==0;opt.tracers=effect==1;opt.brass=effect==2;
   EnhanceEffects fx;fx.use_gpu_simulation(true);
   EffectScene sample{1,1,530,256,0,0,true,{}};
   fx.advance(sample,opt);REQUIRE(gpu.update(source,nativeWidth,256,fx.packet(),opt));
   sample.frame++;sample.objects={{123,0,effect==0?EffectKind::Explosion:effect==1?EffectKind::EnemyShot:EffectKind::Projectile,260,100}};
   fx.advance(sample,opt);REQUIRE(gpu.update(source,nativeWidth,256,fx.packet(),opt));
   REQUIRE(fx.packet().spawns.size()==1);REQUIRE(fx.packet().spawns[0].kind==float(7+effect));
   sample.frame++;
   fx.advance(sample,opt);REQUIRE(gpu.update(source,nativeWidth,256,fx.packet(),opt));
   REQUIRE(gpu.capture(pixels));
   std::printf("isolated kind=%d changed=%zu mask=%u commands=%zu\n",7+effect,size_t(std::count_if(pixels.begin(),pixels.end(),[](auto c){return c!=0x405060;})),fx.packet().mask,fx.packet().spawns.size());
   REQUIRE(pixels!=source);
   for(size_t i=0;i<pixels.size();i++)if(pixels[i]!=source[i]){
    REQUIRE(i%530>220 && i%530<300 && i/530>60 && i/530<140);
   }
   auto frozen=pixels;
   REQUIRE(gpu.recompose(fx.packet(),opt));REQUIRE(gpu.capture(pixels));REQUIRE(pixels==frozen);
   opt.shockwaves=opt.tracers=opt.brass=false;
   REQUIRE(gpu.recompose(fx.packet(),opt));REQUIRE(gpu.capture(pixels));REQUIRE(pixels==source);
  }
  // A legacy afterglow setting must still leave each player bullet as one
  // arcade sprite, never a secondary projectile or wake.
  for(bool analog:{false,true}){
   PcSettings opt=isolated_settings();opt.enhance=opt.tracers=true;opt.particles=false;
   EnhanceEffects fx;fx.use_gpu_simulation(true);
   EffectScene room{1,1,530,256,0,0,true,{}};
   fx.advance(room,opt);
   room.objects={{123,0,EffectKind::Projectile,200,120,0,0,0x4201,analog}};
   ++room.frame;fx.advance(room,opt);
   room.objects.front().x+=6;room.objects.front().y-=4;
   ++room.frame;fx.advance(room,opt);
   REQUIRE(fx.packet().spawns.empty());
  }
  // Legacy enemy-shot afterglow is retired too: movement emits no extra sprite.
  {
   PcSettings opt=isolated_settings();opt.enhance=opt.tracers=true;
   opt.particles=opt.blood=opt.debris=opt.smoke=opt.fire=false;
   EnhanceEffects fx;fx.use_gpu_simulation(true);
   EffectScene room{1,1,530,256,0,0,true,{}};
   fx.advance(room,opt);
   room.objects={{124,0,EffectKind::EnemyShot,210,120,4,0,0x8500}};
   ++room.frame;fx.advance(room,opt);
   room.objects.front().x+=4;++room.frame;fx.advance(room,opt);
   REQUIRE(fx.packet().spawns.empty());
  }
  // Both players die on the same frame. Their deposits survive camera motion,
  // a long pause in presentation, and new living poses after respawn.
  {
   PcSettings opt=isolated_settings();opt.enhance=opt.blood=opt.persistent_gore=true;
   EnhanceEffects fx;fx.use_gpu_simulation(true);
   EffectScene room{1,42,530,256,0,0,true,{}};
   room.players={DeathPose{1,false,true,125,150,12},DeathPose{2,false,true,375,150,12}};
   auto advance=[&](){fx.advance(room,opt);return gpu.update(source,nativeWidth,256,fx.packet(),opt);};
   REQUIRE(advance());++room.frame;
   for(auto& p:room.players){p.dead=true;p.visible=false;}
   REQUIRE(advance());REQUIRE(fx.packet().spawns.size()==2);
   for(int f=0;f<100;++f){
    if(f==10)room.camera_x=12;
    ++room.frame;REQUIRE(advance());REQUIRE(fx.packet().spawns.empty());
   }
   for(auto& p:room.players){p.dead=false;p.visible=true;}
   room.frame+=2000;REQUIRE(advance());REQUIRE(gpu.capture(pixels));
   size_t left=0,right=0;
   for(size_t i=0;i<pixels.size();++i)if(pixels[i]!=source[i]){
    if(i%530<265)++left;else ++right;
   }
   REQUIRE(left>500 && right>500);
   auto settled=pixels;
   room.camera_x+=8;++room.frame;REQUIRE(advance());REQUIRE(gpu.capture(pixels));
   for(int y=30;y<250;++y)for(int x=16;x<500;++x)REQUIRE(pixels[y*530+x]==settled[y*530+x+8]);
   ++room.room;++room.frame;REQUIRE(advance());REQUIRE(gpu.capture(pixels));REQUIRE(pixels==source);
  }
  // The eye discharges once; the arm jet loses reach and leaves oil.
  {
   PcSettings opt=isolated_settings();opt.enhance=opt.destruction=opt.persistent_gore=true;
   EffectPacket p{};p.in_arena=true;p.room=78;p.serial=300;p.dt=.01827925f;
   p.spawns={{100,110,20,1,1,.24f,0,0},{250,110,20,1,-1,.24f,1,0}};
   auto draw=[&](){++p.frame;++p.serial;return gpu.update(source,nativeWidth,256,p,opt) && gpu.capture(pixels);};
   auto changed=[&](int left,int right){
    unsigned n=0;for(int y=96;y<=130;++y)for(int x=left;x<=right;++x)n+=pixels[y*530+x]!=source[y*530+x];return n;
   };
   REQUIRE(draw());REQUIRE(changed(101,125)>12 && changed(237,263)>5);
   p.spawns.clear();
   for(int i=0;i<8;++i){REQUIRE(draw());}
   REQUIRE(changed(237,263)==0 && changed(101,125)>8);
   for(int i=0;i<38;++i){REQUIRE(draw());}
   REQUIRE(changed(101,125)>8);
   opt.persistent_gore=false;REQUIRE(draw());REQUIRE(changed(101,125)==0);
   std::vector<uint16_t> indexed(530*256,0x1081);
   std::vector<uint32_t> pens(4096,source.front());
   for(int y=100;y<120;y++)for(int x=250;x<265;x++)indexed[y*530+x]=0x1101;
   p.spawns={{250,110,20,1,1,0,1,128}};++p.serial;
   REQUIRE(gpu.update_arcade(indexed,530,530,256,pens,false,false,p,opt,true));
   REQUIRE(gpu.capture(pixels));REQUIRE(changed(239,249)>0 && changed(250,264)==0);
  }
  // Only contacts crossing fresh source blood carry pigment. Prints cannot reload it.
  {
   PcSettings opt=isolated_settings();opt.enhance=opt.persistent_gore=true;
   EffectPacket p{};p.in_arena=true;p.room=77;p.frame=1;p.serial=331;
   p.spawns={{90,130,11,1,-1,0,0,6}};
   auto draw=[&](){++p.frame;++p.serial;return gpu.update(source,nativeWidth,256,p,opt) && gpu.capture(pixels);};
   REQUIRE(draw());
   auto contact=[&](float x,float y,float lane){p.spawns.push_back({x,y,11,1,1,0,2,lane});};
   p.spawns.clear();contact(90,130,1);contact(90,130,3);REQUIRE(draw());
   auto ink=[&](int x,int y){int n=0;for(int j=-5;j<=5;++j)for(int i=-4;i<=4;++i)
       n+=pixels[(y+j)*530+x+i]!=source[(y+j)*530+x+i];return n;};
   // Boots carry 24 fading prints and treads 16; untouched contacts stay clean.
   int early=0,late=0;
   for(int i=0;i<28;++i){
    int x=130+i*12;p.spawns.clear();contact(float(x),100,1);contact(float(x),130,3);contact(float(x),150,4);
    REQUIRE(draw());REQUIRE((ink(x,100)>0)==(i<24));REQUIRE((ink(x,130)>0)==(i<16));REQUIRE(ink(x,150)==0);
    if(i<4)early+=ink(x,100);else if(i>=20 && i<24)late+=ink(x,100);
   }
   REQUIRE(late>0 && late<early);   // the track dries out gradually
   // Dried blood and existing tracks never reload a sole (probe beyond the trail).
   p.frame+=600;p.spawns.clear();contact(90,130,4);contact(490,150,4);contact(130,100,2);contact(490,100,2);
   REQUIRE(draw());REQUIRE(ink(490,150)==0 && ink(490,100)==0);
  }
  // Blood never paints void: a wall hole or pit is near-black in the ROM frame.
  {
   PcSettings opt=isolated_settings();opt.enhance=opt.persistent_gore=true;
   std::vector<uint32_t> holed=source;
   for(int y=150;y<230;++y)for(int x=300;x<380;++x)holed[y*530+x]=0x050505;
   EffectPacket p{};p.in_arena=true;p.room=78;p.frame=1;p.serial=441;
   for(int k=0;k<6;++k)p.spawns.push_back({300.f,160.f+k*10,11,1,1,0,0,10});
   REQUIRE(gpu.update(holed,530,256,p,opt));REQUIRE(gpu.capture(pixels));
   unsigned lit=0,dark=0;
   for(int y=150;y<230;++y){
    for(int x=280;x<300;++x)lit+=pixels[y*530+x]!=holed[y*530+x];
    for(int x=308;x<380;++x)dark+=pixels[y*530+x]!=holed[y*530+x];
   }
   std::printf("void gate: lit=%u dark=%u\n",lit,dark);REQUIRE(lit>40 && dark==0);
  }
  // A room clear can exceed the transient command buffer. The last victim
  // must leave small particle deposits too, including one outside the initial view.
  {
   PcSettings opt=isolated_settings();opt.enhance=opt.persistent_gore=true;
   EffectPacket packet{};packet.in_arena=true;packet.room=93;packet.serial=911;
   for(unsigned i=0;i<256;++i)packet.remains.push_back({80,140,1,0x8600,i,93});
   packet.remains.push_back({400,150,1.3f,0x8600,654,93});
   packet.remains.push_back({800,150,1.3f,0x8300,321,93,0,0,MachinePart});
   REQUIRE(gpu.update(source,nativeWidth,256,packet,opt));REQUIRE(gpu.capture(pixels));
   auto covered=[&](int centre){
    unsigned changed=0;for(int y=100;y<200;++y)for(int x=centre-48;x<centre+48;++x)
     changed+=pixels[y*530+x]!=source[y*530+x];return changed;
   };
   REQUIRE(covered(400)>40);
   packet.remains.clear();packet.frame+=5000;++packet.serial;packet.camera_x=350;
   REQUIRE(gpu.update(source,nativeWidth,256,packet,opt));REQUIRE(gpu.capture(pixels));
   REQUIRE(covered(50)>40 && covered(450)>40);
   ++packet.room;++packet.frame;++packet.serial;
   REQUIRE(gpu.update(source,nativeWidth,256,packet,opt));REQUIRE(gpu.capture(pixels));REQUIRE(pixels==source);
   // Ground fixtures accept deposits while raised walls and the HUD stay clear.
   packet.geometry.floor={100,72,330,132};packet.geometry.solids={{230,90,36,105}};
   packet.hud={{104,73,77,26}};packet.camera_x=0;
   packet.remains={{94,71,1.6f,0x9803,11,packet.room,0,0,BossPart|BlastDeath},
                   {435,211,1.6f,0x9801,22,packet.room,0,0,BossPart|MachinePart|BlastDeath},
                   {242,120,1.6f,0x9900,33,packet.room,0,0,BossPart|MachinePart},
                   {431,67,1.6f,0x8600,44,packet.room}};
   ++packet.frame;++packet.serial;
   REQUIRE(gpu.update(source,nativeWidth,256,packet,opt));REQUIRE(gpu.capture(pixels));
   unsigned floorInk=0,fixtureInk=0,edgeInk=0;
   for(int y=0;y<256;++y)for(int x=0;x<530;++x){
    if(pixels[y*530+x]==source[y*530+x])continue;
    REQUIRE(x>=100 && x<430 && y>=72 && y<204);
    if(x>=230 && x<266 && y>=90 && y<195)++fixtureInk;
    if(x==100 || x==429 || y==72 || y==203)++edgeInk;
    REQUIRE(!(x>=104 && x<181 && y>=73 && y<99));++floorInk;
   }
   REQUIRE(floorInk>100 && fixtureInk>20 && edgeInk>0);
   // The selector leads the pan. Deposits remain anchored until gameplay resumes.
   auto outgoing=pixels;
   packet.remains.clear();++packet.room;++packet.serial;
   REQUIRE(gpu.update(source,nativeWidth,256,packet,opt));REQUIRE(gpu.capture(pixels));REQUIRE(pixels==outgoing);
   packet.roomIntro=true;packet.camera_x=40;packet.geometry.floor.x+=490;
   packet.hud[0].x-=40;++packet.serial;
   REQUIRE(gpu.update(source,nativeWidth,256,packet,opt));REQUIRE(gpu.capture(pixels));
   for(int y=30;y<240;++y)for(int x=10;x<450;++x)REQUIRE(pixels[y*530+x]==outgoing[y*530+x+40]);
   packet.roomIntro=false;packet.camera_x=530;packet.geometry.floor.x=100;++packet.serial;
   REQUIRE(gpu.update(source,nativeWidth,256,packet,opt));REQUIRE(gpu.capture(pixels));REQUIRE(pixels==source);
   opt.blood=true;packet.spawns={{101,130,1,64,0,0,-2,0}};++packet.serial;
   REQUIRE(gpu.update(source,nativeWidth,256,packet,opt));packet.spawns.clear();
   for(int f=0;f<120;++f){++packet.frame;++packet.serial;REQUIRE(gpu.update(source,nativeWidth,256,packet,opt));}
   REQUIRE(gpu.capture(pixels));
   unsigned wallInk=0;
   for(int y=80;y<170;++y)for(int x=85;x<100;++x)wallInk+=pixels[y*530+x]!=source[y*530+x];
   REQUIRE(wallInk>5);
  }
  // Blood spray and machine particles settle independently of death deposits.
  {
   PcSettings opt=isolated_settings();opt.enhance=opt.explosive_debris=opt.persistent_gore=true;
   EffectPacket p{};p.room=600;p.in_arena=true;p.frame=1;p.serial=790000;
   p.geometry.floor={80,60,360,166};p.dt=.01827925f;p.reset=true;
   REQUIRE(gpu.update(source,nativeWidth,256,p,opt));p.reset=false;
   // Flesh travels left toward a wall. Steel flies right. A centred
   // detonation at the bottom has no preferred horizontal direction.
   p.spawns={{105,130,19,80,-1,0,0,1},{325,130,19,60,1,0,1,1},{210,208,19,80,0,0,0,1}};
   auto step=[&](){++p.frame;++p.serial;return gpu.update(source,nativeWidth,256,p,opt);};
   REQUIRE(step());p.spawns.clear();
   for(int f=0;f<8;++f)REQUIRE(step());
   REQUIRE(gpu.capture(pixels));
   std::array<unsigned,6> counts{};
   for(int y=45;y<210;++y)for(int x=70;x<430;++x){
    if(pixels[y*530+x]==source[y*530+x])continue;
    if(x<150 && y<146)++counts[x>=105];
    if(x>280 && y<146)++counts[2+(x>=325)];
    if(x>=160 && x<265 && y>155)++counts[4+(x>=210)];
   }
   REQUIRE(counts[0]>2*counts[1] && counts[3]>2*counts[2] && counts[4]>20 && counts[5]>20);
   for(int f=0;f<130;++f)REQUIRE(step());
   REQUIRE(gpu.capture(pixels));
   unsigned wall=0,shrapnel=0;
   for(int y=65;y<190;++y)for(int x=60;x<440;++x){
    auto c=pixels[y*530+x];if(c==source[y*530+x])continue;
    if(x<80)++wall;
    if(x>345){REQUIRE(int(c>>16)-int(c&255)<32);++shrapnel;}
   }
   REQUIRE(wall>5 && shrapnel>30);
  }
  // Ordinary particles retain their own material and settling behaviour.
  {
   PcSettings opt=isolated_settings();opt.enhance=opt.persistent_gore=opt.blood=opt.destruction=true;
   EffectPacket p{};p.in_arena=true;p.room=601;p.serial=800000;p.frame=1;p.reset=true;
   REQUIRE(gpu.update(source,nativeWidth,256,p,opt));
   p.reset=false;++p.frame;++p.serial;
   p.spawns={{120,150,1,64,2,0},{380,150,13,32,0,0,1,0}};
   REQUIRE(gpu.update(source,nativeWidth,256,p,opt));p.spawns.clear();p.dt=.01827925f;
   for(int f=0;f<150;++f){++p.frame;++p.serial;REQUIRE(gpu.update(source,nativeWidth,256,p,opt));}
   REQUIRE(gpu.capture(pixels));auto landed=pixels;
   unsigned blood=0,metal=0;
   for(size_t i=0;i<pixels.size();++i)if(pixels[i]!=source[i]){
    if(i%530<265)++blood;else ++metal;
   }
   REQUIRE(blood>50 && metal>25);
   p.frame+=1000;++p.serial;p.reset=true;
   REQUIRE(gpu.update(source,nativeWidth,256,p,opt));REQUIRE(gpu.capture(pixels));
   // Wet blood darkens as it dries. Every deposited pixel must remain occupied.
   for(size_t i=0;i<landed.size();++i)if(landed[i]!=source[i])REQUIRE(pixels[i]!=source[i]);
   ++p.room;++p.frame;++p.serial;
   REQUIRE(gpu.update(source,nativeWidth,256,p,opt));REQUIRE(gpu.capture(pixels));REQUIRE(pixels==source);
  }
  // Follow one real ejection through flight, settling, pool churn and room exit.
  {
   PcSettings opt=isolated_settings();opt.enhance=opt.brass=true;
   opt.particles=opt.blood=opt.debris=opt.smoke=opt.fire=opt.lighting=false;
   EnhanceEffects fx;fx.use_gpu_simulation(true);
   EffectScene room{1,11,530,256,0,0,true,{}};
   auto advance=[&](){fx.advance(room,opt);return gpu.update(source,nativeWidth,256,fx.packet(),opt);};
   REQUIRE(advance());room.frame++;
   for(unsigned family=0;family<8;family++){
    auto kind=family<6?EffectKind::Projectile:EffectKind::EnemyShot;
    auto weapon=uint16_t(family<6?0x4201+family:family==6?0x8500:0x9500);
    room.objects.push_back({77+family,0,kind,float(48+family%4*50),float(80+family/4*85),2,0,weapon});
   }
   REQUIRE(advance());REQUIRE(gpu.capture(pixels));auto airborne=pixels;
   room.objects.clear();
   for(int i=0;i<140;i++){room.frame++;REQUIRE(advance());}
   REQUIRE(gpu.capture(pixels));auto landed=pixels;
   REQUIRE(landed!=source && landed!=airborne);
   for(int family=0;family<8;family++){
    bool found=false;
    for(int y=56+family/4*85;y<116+family/4*85;y++)
     for(int x=23+family%4*50;x<73+family%4*50;x++)found|=landed[y*530+x]!=source[y*530+x];
    REQUIRE(found);
   }
   // More subsequent ejections than there are live brass slots, well away from the first.
   for(int i=0;i<152;i++){
    room.frame++;room.objects={{uint32_t(1000+i),0,EffectKind::Projectile,430,130}};
    REQUIRE(advance());
   }
   REQUIRE(gpu.capture(pixels));
   for(size_t i=0;i<pixels.size();i++)if(i%530<250)REQUIRE(pixels[i]==landed[i]);
   opt.brass=false;REQUIRE(gpu.recompose(fx.packet(),opt));REQUIRE(gpu.capture(pixels));REQUIRE(pixels==source);
   opt.brass=true;room.camera_x+=12;room.frame++;REQUIRE(advance());
   REQUIRE(gpu.capture(pixels));
   for(size_t i=0;i<pixels.size();i++)if(landed[i]!=source[i])REQUIRE(pixels[i-12]==landed[i]);
   room.room++;room.frame++;REQUIRE(advance());REQUIRE(gpu.capture(pixels));REQUIRE(pixels==source);
  }
  // Fire into a room wall, an open side gate, then a solid inside the aperture.
  {
   PcSettings opt=isolated_settings();opt.enhance=opt.brass=true;opt.lighting=false;
   auto pastWall=[&](unsigned gates,bool obstacle){
    EffectPacket p;p.in_arena=true;p.room=501+gates+unsigned(obstacle);p.frame=1;
    p.serial=10000+p.room*200;p.reset=true;
    p.geometry={80,45,151,236,165,237,gates};
    if(obstacle)p.geometry.solids={{150,100,5,80}};
    if(!gpu.update(source,nativeWidth,256,p,opt))return -1;
    ++p.serial;++p.frame;p.reset=false;p.spawns={{148,142,9,96}};
    if(!gpu.update(source,nativeWidth,256,p,opt))return -1;
    p.spawns.clear();p.dt=1.f/54.706f;
    for(int n=0;n<120;n++){++p.serial;++p.frame;if(!gpu.update(source,nativeWidth,256,p,opt))return -1;}
    if(!gpu.capture(pixels))return -1;
    int beyond=0;for(int y=80;y<200;y++)for(int x=154;x<210;x++)beyond+=pixels[y*530+x]!=source[y*530+x];
    return beyond;
   };
   REQUIRE(pastWall(0,false)==0);REQUIRE(pastWall(2,false)>0);REQUIRE(pastWall(2,true)==0);
   EffectPacket p;p.serial=900000;p.in_arena=true;p.room=13;p.reset=true;p.frame=1;
   p.spawns={{220,140,10,1,1,0}};
   REQUIRE(gpu.update(source,nativeWidth,256,p,opt));REQUIRE(gpu.capture(pixels));
   REQUIRE(pixels[140*530+222]!=source[140*530+222] && pixels[140*530+217]==source[140*530+217]);
   opt.muzzle_flash=false;p.serial++;REQUIRE(gpu.update(source,nativeWidth,256,p,opt));REQUIRE(gpu.capture(pixels));REQUIRE(pixels==source);
  }
  // Legacy settings and stale lamp inputs must never alter the arcade image.
  EffectPacket retired;retired.serial=890001;retired.in_arena=true;retired.reset=true;
  retired.lamps[0]={125,78,16,140,1,.8f,.4f,1};retired.casters={{160,90,16,24,114,0,1}};
  retired.spawns={{150,92,6,1,100,1}};
  s.lighting=s.room_lights=s.materials=s.ray_tracing=true;
  REQUIRE(gpu.update(source,nativeWidth,256,retired,s));REQUIRE(gpu.capture(pixels));REQUIRE(pixels==source);
  // Isolate the persistent blast stamp from airborne particles at both walls.
  {
   PcSettings opt=isolated_settings();opt.enhance=opt.weapon_impacts=true;opt.fluid_smoke=false;
   EffectPacket p{};p.serial=990000;p.room=70;p.in_arena=true;p.reset=true;
   p.geometry.floor={40,50,450,180};
   p.spawns={{40,140,15,0,0,0,2,0},{489,140,15,0,0,0,2,0}};
   REQUIRE(gpu.update(source,nativeWidth,256,p,opt));REQUIRE(gpu.capture(pixels));
   unsigned left=0,right=0;
   for(int y=0;y<256;++y)for(int x=0;x<530;++x){
    if(pixels[y*530+x]==source[y*530+x])continue;
    REQUIRE(x>=40 && x<490 && y>=50 && y<230);
    if(x==40)++left;
    if(x==489)++right;
   }
   REQUIRE(left && right);
  }
  // The flow field advances once per arcade frame, survives paused redraws,
  // and is emptied when a new room starts.
  {
   PcSettings opt=isolated_settings();opt.enhance=opt.fluid_smoke=true;opt.lighting=false;
   EffectPacket p;p.serial=1000000;p.room=71;p.in_arena=true;p.reset=true;p.dt=.018f;
   p.geometry={20,52,505,235,215,290,0};p.spawns={{220,150,15,1,1,0,2,0}};
   REQUIRE(gpu.update(source,nativeWidth,256,p,opt));REQUIRE(gpu.capture(pixels));REQUIRE(pixels!=source);
   auto first=pixels;REQUIRE(gpu.recompose(p,opt));REQUIRE(gpu.capture(pixels));REQUIRE(pixels==first);
   p.reset=false;p.spawns.clear();
   for(int i=0;i<10;i++){++p.frame;++p.serial;REQUIRE(gpu.update(source,nativeWidth,256,p,opt));}
   REQUIRE(gpu.capture(pixels));REQUIRE(pixels!=source && pixels!=first);
   p.reset=true;++p.serial;++p.room;REQUIRE(gpu.update(source,nativeWidth,256,p,opt));REQUIRE(gpu.capture(pixels));REQUIRE(pixels==source);
  }
  // A single burst survives its former maximum lifetime, then fades completely.
  for(int kind : {3,4,16}){
   PcSettings opt=isolated_settings();opt.enhance=opt.smoke=opt.fire=true;opt.fluid_smoke=kind==16;
   EffectPacket p;p.serial=2000000+kind*1000;p.in_arena=true;p.room=kind;p.reset=true;
   p.spawns={{250,160,float(kind),12}};
   REQUIRE(gpu.update(source,nativeWidth,256,p,opt));
   p.reset=false;p.spawns.clear();p.dt=.018f;
   int checkpoint=kind==3?130:kind==4?75:50;
   int end=kind==3?257:kind==4?146:98;
   for(int i=1;i<=end;i++){
    ++p.frame;++p.serial;REQUIRE(gpu.update(source,nativeWidth,256,p,opt));
    if(i==checkpoint){REQUIRE(gpu.capture(pixels));REQUIRE(pixels!=source);}
   }
   REQUIRE(gpu.capture(pixels));REQUIRE(pixels==source);
  }
  // Track a moving wall on the actual scaled output for one second at each
  // display rate. The source advances two arcade pixels per 54.7 Hz tick.
  {
   PcSettings opt=isolated_settings();
   EffectPacket packet;
   std::vector<uint32_t> wall(256*32), display;
   for(int hz : {60,120,144}){
    CameraMotion motion;
    EffectScene pan;pan.in_arena=true;pan.width=256;
    int lastTick=-1;
    for(int n=0;n<hz;++n){
     double t=double(n)*8000000.0/(146234.0*hz);
     int tick=int(t);
     if(tick!=lastTick){
      for(int y=0;y<32;++y)for(int x=0;x<256;++x)wall[y*256+x]=x>=200-2*std::min(tick,40)?0xffffff:0;
      pan.frame=tick;pan.camera_x=2*std::min(tick,40);motion.observe(pan);
      packet.frame=tick; ++packet.serial;
      REQUIRE(gpu.update(wall,256,32,packet,opt));lastTick=tick;
     }
     REQUIRE(gpu.draw(nullptr,1024,32,0,0,1024,32,opt,false,false,motion.offset(motion.dx,float(t-tick)),0));
     REQUIRE(gpu.capture(display,true));
     int edge=0;
     while(edge<1024 && !(display[16*1024+edge]&255))++edge;
     REQUIRE(std::abs(edge-(800-8*std::clamp(t-1.0,0.0,40.0)))<=0.51);
    }
   }
  }
  // Hidden-window lifecycle smoke: resize, overlay, every filter, real flip Present.
  HWND wnd=CreateWindowExW(0,L"STATIC",L"Smash resident GPU test",WS_OVERLAPPEDWINDOW,0,0,640,480,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);REQUIRE(wnd);
  auto before=gpu.readback_bytes();
  REQUIRE(gpu.draw(wnd,640,480,0,0,640,480,s,true,true));auto dc=(HDC)gpu.overlay_dc();TextOutA(dc,10,10,"GPU MENU",8);gpu.present();REQUIRE(gpu.error().empty());
  REQUIRE(gpu.draw(wnd,800,600,20,20,760,560,s));gpu.present();REQUIRE(gpu.error().empty());
  for(int mode=0;mode<8;mode++){s.filter=mode;REQUIRE(gpu.draw(wnd,800,600,20,20,760,560,s));gpu.present();REQUIRE(gpu.error().empty());if(mode>=5){for(int i=0;i<10000&&!gpu.filter_ready(mode);i++){std::this_thread::sleep_for(std::chrono::milliseconds(10));REQUIRE(gpu.draw(wnd,800,600,20,20,760,560,s));gpu.present();REQUIRE(gpu.error().empty());}REQUIRE(gpu.filter_ready(mode));}}
  REQUIRE(gpu.readback_bytes()==before);REQUIRE(gpu.finish());DestroyWindow(wnd);
 }
 return 0;
}
