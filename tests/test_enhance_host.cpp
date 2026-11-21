// Native coupled player smoke: real presentation path, no window/audio device.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <fstream>
#include <chrono>
#define wWinMain unused_enhance_host_entry
#include "../src/host/win_main.cpp"
#undef wWinMain

static bool check_retail_retirements(const Machine& loaded){
    // Run the real list routines for each family, including delayed deaths
    // which neutralize OID before returning the object to the pool.
    constexpr u16 ids[]={0x8300,0x8400,0x8600,0x0800,0x8900,0x8902,0x8903,0x8904,
        0x8a01,0x8a30,0x0b00,0x0e00,0x9000,0x9100,0x9200,0x9801,0x9802,0x9803,
        0x9804,0x9805,0x9806,0x9807,0x9808,0x9900,0x9901,0x9902,0x9903,0x9904,
        0x9905,0x9906,0x9907,0x9908,0x9909,0x990a,0x990b};
    for(bool bulk:{false,true}){
        Machine m=loaded;m.reset();m.external_timers=true;m.next_event_cycle=~0ull;
        stv::host::EnemyRemainsTracker ledger;
        m.on_object=[&](ObjectEvent event,u32 node,u32 other){ledger.observe(m.ram,event,node,m.c.cyc,{}, {},other);};
        m.ww(0x010865a0,16);
        auto run=[&](u32 entry){
            constexpr u32 returned=0xffec7270;
            m.c.sp=0x010fffe0;m.c.st=0x830;push(m,m.c,returned);
            for(unsigned n=0;n<8000 && entry!=returned && !m.stop;++n)entry=l8_run(m,entry,m.c.cyc+1);
            return entry==returned && !m.stop;
        };
        for(unsigned i=0;i<std::size(ids);++i){
            auto node=object_pool_base+i*object_stride;
            m.ww(node+0xb0,0x8002);m.ww(node+0xf0,20);m.ww(node+0x100,24);
            m.ww(node+0x70,u16(100+i*4));m.ww(node+0x90,120);m.ww(node+0x150,ids[i]);
            m.c.a[0]=node;if(!run(0xffe03030))return false; // INSOBJ
            if(i%2)m.ww(node+0x150,0);
        }
        if(bulk){
            m.c.a[0]=m.c.a[1]=0;if(!run(0xffe034b0))return false; // KILOBJ, match all
        }else for(unsigned i=0;i<std::size(ids);++i){
            m.c.a[0]=object_pool_base+i*object_stride;if(!run(0xffe031d0))return false; // DELOBJ
        }
        auto bodies=ledger.take(0);
        if(bodies.size()!=std::size(ids) || rd_dword(m,0x01000040)!=0)return false;
        for(auto oid:ids)if(std::count_if(bodies.begin(),bodies.end(),[&](const auto& b){return b.oid==oid;})!=1)return false;
    }
    std::printf("Retail single and group removal: %zu identities each\n",std::size(ids));
    return true;
}
int main(int argc,char** argv) {
    if(argc!=4&&argc!=5)return 2;
    // Title mode: power on with released controls and capture the widescreen
    // title page. Its centre is the live ROM image and its wings derive only
    // from that frame (no artwork ships).
    if(argc==5 && std::string(argv[4])=="title"){
        std::filesystem::create_directories(argv[3]);g_rom_path=std::filesystem::path(argv[3])/"retail-l8.dat";
        std::string error;
        if(!stv::gsp::import_rom_set(argv[1],g_rom_path,error))return 3;
        g_enhanced=true;g_script1.clear();g_script2.clear();
        if(!reset_coupled_game())return 3;
        for(unsigned frame=0;frame<6000;++frame){
            g_pcm.clear();run_one_frame();if(!g_frame_done||g_m.stop||!g_snd.fault.empty())return 4;
            if(game_scene_state(g_m)!=0x30 || scene_background(g_m)!=0xffea56d0u)continue;
            build_software_frame(g_m);
            if(g_frame_width!=547)return 25;
            // Skip the fade-in: capture once most of the ROM image is drawn.
            if(std::count_if(g_pixels.begin(),g_pixels.end(),[](u32 c){return (c&0xffffff)!=0;})<547*VIS_H/2)continue;
            unsigned lit=0,centre=0;
            for(int y=0;y<VIS_H;++y){
                for(int x:{0,30,60,480,510,546})lit+=(g_pixels[std::size_t(y)*547+x]&0xffffff)!=0;
                for(int x=0;x<410;++x)centre+=g_pixels[std::size_t(y)*547+68+x]==g_m.pens[g_m.frame[std::size_t(y+VIS_MINY)*g_m.vram_w+VIS_MINX+x]&0x0fff];
            }
            std::ofstream out(std::filesystem::path(argv[3])/"title-widescreen.ppm",std::ios::binary);
            out<<"P6\n547 "<<VIS_H<<"\n255\n";
            for(auto rgb:g_pixels){out.put(char(rgb>>16));out.put(char(rgb>>8));out.put(char(rgb));}
            std::printf("Widescreen title at frame %u: centre %u/%d ROM pixels, %u/%d side pixels lit (must be 0)\n",
                        frame,centre,410*VIS_H,lit,6*VIS_H);
            return out && centre==410u*VIS_H && lit==0 ? 0 : 26;
        }
        return 27;
    }
    bool resident=argc==5;
    if(resident)g_wnd=CreateWindowExW(0,L"STATIC",L"Headless resident effects",WS_OVERLAPPEDWINDOW,0,0,640,480,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    std::filesystem::create_directories(argv[3]);g_rom_path=std::filesystem::path(argv[3])/"retail-l8.dat";
    std::string error;
    if(!stv::gsp::import_rom_set(argv[1],g_rom_path,error))return 3;
    g_enhance_effects.use_gpu_simulation(resident);
    g_enhanced=true;
    if(!stv::net::load_scripted_inputs(argv[2],4000,g_script1,g_script2)||!reset_coupled_game())return 3;
    if(resident && std::string(argv[4])=="dxr"){
        if(!g_resident_effects.prepare_dxr()){std::fprintf(stderr,"%s\n",g_resident_effects.dxr_error().c_str());return 18;}
        g_settings.run_ahead=1;g_settings.smooth_motion=true;
        std::vector<double> samples[2];
        // Alternate the RT setting across this actual entrance pan. Include
        // the coupled tick, prediction, two 1080p redraws and GPU completion.
        for(unsigned i=0;i<2050;++i){
            bool on=(i%4==0 || i%4==3);g_settings.hardware_rt=on;
            auto start=std::chrono::steady_clock::now();
            g_pcm.clear();run_one_frame();
            if(!g_frame_done || g_m.stop || !g_snd.fault.empty())return 19;
            if(game_scene_state(g_m)!=0x18)continue;
            present_frame(local_video_frame());
            if(!g_resident_ready)return 20;
            for(float phase:{.25f,.75f}){
                if(!g_resident_effects.draw(nullptr,1920,1080,0,0,1920,1080,g_settings,false,false,
                    g_camera.offset(g_camera.dx,phase),g_camera.offset(g_camera.dy,phase)) || !g_resident_effects.present())return 21;
            }
            if(!g_resident_effects.finish() || !g_resident_effects.dxr_error().empty())return 22;
            if(i>=1740 && i<=1860)samples[on].push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
        }
        for(unsigned on=0;on<=1;++on){
            auto& t=samples[on];if(t.size()<40)return 23;std::sort(t.begin(),t.end());
            std::printf("Native entrance DXR %s samples=%zu median_ms=%.3f p95_ms=%.3f max_ms=%.3f\n",
                on?"on":"off",t.size(),t[t.size()/2],t[t.size()*95/100],t.back());
        }
        DestroyWindow(g_wnd);return 0;
    }
    if(resident && !check_retail_retirements(g_m))return 13;
    g_settings.enhance=true;g_settings.ray_tracing=true;g_settings.voxels=true;
    g_settings.shockwaves=g_settings.tracers=g_settings.brass=resident;
    g_settings.persistent_gore=resident;
    unsigned first_death=0,bursts=0,enemy_bodies=0,body_types[32]{};
    auto write=[&](const char* name,const std::vector<u32>& pixels) {
        std::ofstream out(std::filesystem::path(argv[3])/name,std::ios::binary);
        out<<"P6\n"<<g_frame_width<<" "<<VIS_H<<"\n255\n";
        for(auto rgb:pixels){out.put(char(rgb>>16));out.put(char(rgb>>8));out.put(char(rgb));}
        return bool(out);
    };
    std::ofstream ram_capture(std::filesystem::path(argv[3])/"effect-ram.bin",std::ios::binary);
    size_t changed=0,max_changed=0,voxel_frames=0,max_voxels=0;double max_ms=0,total_ms=0;
    size_t protectedPixels=0;
    std::vector<u8> predicted_state;
    std::vector<u16> predicted_pixels;
    unsigned prediction_due=0, predictions_checked=0;
    double prediction_ms[4]{};
    unsigned predictions[4]{};
    for(unsigned frame=0;frame<4000;++frame) {
        g_pcm.clear();run_one_frame();if(!g_frame_done||g_m.stop||!g_snd.fault.empty())return 4;
        if (!resident && prediction_due && frame == prediction_due) {
            std::vector<u8> actual;
            g_m.save_state(actual, 0, false);
            if (actual != predicted_state || predicted_pixels != g_m.presentation_frame) {
                std::fprintf(stderr, "run-ahead differs from held-input replay at frame %u\n", frame);
                return 15;
            }
            ++predictions_checked;
            prediction_due = 0;
        }
        if (!resident && frame % 47 == 0 && frame > 0) {
            const unsigned count = 1 + (frame / 47) % 3;
            const auto first = g_m.frame_number;
            bool held = first + count < g_script1.size();
            for (unsigned i=1; held && i<=count; ++i)
                held = g_script1[first+i] == g_script1[first] && g_script2[first+i] == g_script2[first];
            if (held) {
                g_settings.run_ahead = count;
                const auto start = std::chrono::steady_clock::now();
                const auto& future = local_video_frame();
                prediction_ms[count] += std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
                ++predictions[count];
                if (&future == &g_m || future.frame_number != first + count) return 16;
                // The fork reuses its immutable copy only for an unchanged revision.
                if (!g_m.static_revision || future.static_revision != g_m.static_revision ||
                    future.gfx != g_m.gfx || future.prog != g_m.prog || future.enhanced_data != g_m.enhanced_data ||
                    future.snd_u4 != g_m.snd_u4 || future.snd_u19 != g_m.snd_u19 || future.snd_u20 != g_m.snd_u20 ||
                    g_m.gfx.empty() || g_m.prog.empty())
                    return 24;
                future.save_state(predicted_state, 0, false);
                predicted_pixels = future.presentation_frame;
                prediction_due = frame + count;
                g_settings.run_ahead = 0;
            }
        }
        if(frame>=1800 && frame%60==0)ram_capture.write(reinterpret_cast<const char*>(g_m.ram.data()),std::streamsize(g_m.ram.size()*2));
        std::vector<u8> before,after;g_m.save_state(before,g_sched->main_pc(),false);
        auto readbacks=g_resident_effects.readback_bytes();
        auto start=std::chrono::steady_clock::now();present_frame(g_m);
        if(resident)build_software_frame(g_m); // independent reference image for diagnostic readback
        const auto clean_pixels=g_pixels;
        if(!resident)g_enhance_effects.compose(g_pixels,g_frame_width,VIS_H,g_settings);
        if(resident && (!g_resident_ready || g_resident_effects.readback_bytes()!=readbacks || !g_resident_effects.capture(g_pixels)))return 11;
        if(resident){
            if(g_frame_width==547 && g_enhance_effects.packet().in_arena &&
               stv::gsp::scene_view_width(g_m)==547){
                for(int y=25;y<VIS_H-4;++y)for(int x=4;x<g_frame_width-4;++x){
                    const auto pos=(y+VIS_MINY)*g_m.vram_w+x+VIS_MINX;
                    if(!(g_m.presentation_frame[pos]&0x1000))continue;
                    ++protectedPixels;
                    if(g_pixels[y*g_frame_width+x]!=clean_pixels[y*g_frame_width+x])return 14;
                }
            }
            for(const auto& body:g_enhance_effects.packet().remains){
                ++enemy_bodies;++body_types[(body.oid>>8)&31];
            }
            if(!g_enhance_effects.packet().remains.empty() && !write("enemy-remains.ppm",g_pixels))return 12;
            for(const auto& spawn:g_enhance_effects.packet().spawns){
                if(spawn.kind!=float(stv::host::ArenaEffect::PlayerBurst))continue;
                ++bursts;if(!first_death)first_death=frame;
                std::printf("player %d death burst at frame %u, ground (%.1f, %.1f)\n",1+int(spawn.power),frame,spawn.x,spawn.y);
            }
            if(first_death && frame==first_death+12 && !write("player-death-flight.ppm",g_pixels))return 12;
            if(first_death && frame==first_death+110 && !write("player-death-floor.ppm",g_pixels))return 12;
        }
        auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();max_ms=std::max(ms,max_ms);total_ms+=ms;
        g_m.save_state(after,g_sched->main_pc(),false);if(before!=after)return 5;
        size_t differences=0;for(size_t i=0;i<g_pixels.size();++i)differences+=g_pixels[i]!=clean_pixels[i];
        changed+=differences>0;
        auto voxels=g_enhance_effects.voxel_count();voxel_frames+=voxels>0;
        if(voxels && voxels>=max_voxels){
            max_voxels=voxels;if(!write("voxels.ppm",g_pixels)||!write("voxels-original.ppm",clean_pixels))return 9;
            auto isolated=g_settings;isolated.particles=isolated.blood=isolated.debris=isolated.smoke=isolated.fire=isolated.lighting=false;
            auto voxel_pixels=clean_pixels;g_enhance_effects.compose(voxel_pixels,g_frame_width,VIS_H,isolated);
            if(!write("voxels-only.ppm",voxel_pixels))return 10;
        }
        if(differences>max_changed){max_changed=differences;if(!write("enhanced.ppm",g_pixels)||!write("original.ppm",clean_pixels))return 6;}
        // Turning everything off must reproduce every original pixel immediately.
        auto opt=g_settings;opt.enhance=false;auto clean=clean_pixels;g_enhance_effects.compose(clean,g_frame_width,VIS_H,opt);
        if(clean!=clean_pixels)return 7;
    }
    std::ofstream report(std::filesystem::path(argv[3])/"report.json");
    report<<"{\"frames\":4000,\"changed_frames\":"<<changed<<",\"voxel_frames\":"<<voxel_frames<<",\"max_voxels\":"<<max_voxels<<",\"max_changed_pixels\":"<<max_changed<<",\"main_snapshot_unchanged\":true,\"capture_mean_ms\":"<<total_ms/4000<<",\"capture_max_ms\":"<<max_ms<<"}\n";
    std::printf("4000 native frames; %zu changed; %zu peak pixels; state unchanged; capture mean %.3f ms max %.3f ms\n",changed,max_changed,total_ms/4000,max_ms);
    std::printf("voxel frames=%zu peak cubes=%zu\n",voxel_frames,max_voxels);
    if(resident)DestroyWindow(g_wnd);
    if(resident){
        std::printf("Enemy bodies deposited: %u\n",enemy_bodies);
        for(int type=0;type<32;++type)if(body_types[type])std::printf("body type %02x: %u\n",type,body_types[type]);
    }
    if (!resident) {
        for (int i=1; i<=3; ++i) {
            if (predictions[i] < 8) return 17;
            std::printf("Run-ahead %d: %u checks, mean %.3f ms\n", i, predictions[i], prediction_ms[i]/predictions[i]);
        }
        if (predictions_checked < 30) return 17;
    }
    return changed && (resident?(bursts>0 && enemy_bodies>0 && protectedPixels>0):voxel_frames>0) && report ? 0:8;
}
