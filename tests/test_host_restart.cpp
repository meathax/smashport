// Actual in-process restart/disconnect owner regression. Only the host entry
// point is renamed; the reset functions, translated programs and sound are real.
// Coupled tests are headless; focused join tests use a message-only window and
// ephemeral owned loopback sockets. No endpoint/controller or visible GUI is opened.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>
#define wWinMain unused_restart_host_entry
#include "../src/host/win_main.cpp"
#undef wWinMain

namespace {
int restart_checks=0;
void require_restart(bool good,const char* message) {
    ++restart_checks;
    if(!good)throw std::runtime_error(message);
}
using MainEvent=std::array<u64,3>;
using IoEvent=std::array<u64,6>;
using Values=std::vector<u64>;
constexpr u64 kHashOffset=14695981039346656037ull;
u64 hash_data(const void*data,std::size_t bytes,u64 hash=kHashOffset) {
    const auto*p=static_cast<const u8*>(data);
    for(std::size_t i=0;i<bytes;++i){hash^=p[i];hash*=1099511628211ull;}
    return hash;
}
template<class T>u64 hash_vector(const std::vector<T>&v,u64 h=kHashOffset){return hash_data(v.data(),v.size()*sizeof(T),h);}
template<class C>void add_values(Values&v,const C& c){for(auto x:c)v.push_back(u64(x));}
void add_time(Values&v,stv::snd::Atto t){v.push_back(u64(t.sec));v.push_back(u64(t.as));}
Values sound_values(stv::snd::Board&s,stv::snd::Mixer&mixer,stv::game::CoScheduler&schedule) {
    auto&c=s.cpu;auto&p=s.pia;
    Values v{c.a,c.b,c.dp,c.cc,c.x,c.y,c.u,c.s,c.pc,c.cyc,c.due,
        u64(c.nmi_asserted),u64(c.nmi_line),u64(c.firq_line),u64(c.irq_line),u64(c.lds),
        s.bank,s.bank_base,u64(s.page_index),s.dac,u64(s.halted),u64(s.vector_pending),
        s.ym_samples,s.ym_reset_state,s.seq,u64(s.aborted),u64(s.reset_hit),u64(s.in_cpu),u64(s.stop),
        p.in_a,p.in_b,p.out_a,p.out_b,p.ddr_a,p.ddr_b,p.ctl_a,p.ctl_b,
        u64(p.in_ca1),u64(p.in_ca2),u64(p.in_cb1),u64(p.in_cb2),u64(p.out_ca2),u64(p.out_cb2),u64(p.last_out_cb2_z),
        u64(p.irq_a1),u64(p.irq_a2),u64(p.irq_b1),u64(p.irq_b2),u64(p.irq_a_state),u64(p.irq_b_state),u64(p.in_b_pushed),
        u64(s.cvsd.last_clock),u64(s.cvsd.buffered_bit),s.cvsd.shiftreg,u64(s.cvsd.sylfilter),u64(s.cvsd.intfilter),u64(s.cvsd.next_sample),
        mixer.ym_generated(),mixer.out_rate(),schedule.main_pc()};
    add_values(v,s.ram);add_time(v,s.cb_time);add_time(v,s.ym_busy_end);add_time(v,schedule.base());add_time(v,schedule.target());
    v.push_back(s.q.size());for(const auto&e:s.q){add_time(v,e.t);v.insert(v.end(),{e.seq,u64(e.kind),u64(e.arg0),u64(e.arg1)});}
    return v;
}
bool same_faults(const Machine&a,const Machine&b){
    if(a.faults.size()!=b.faults.size())return false;
    for(std::size_t i=0;i<a.faults.size();++i)
        if(a.faults[i].what!=b.faults[i].what||a.faults[i].addr!=b.faults[i].addr||a.faults[i].cycle!=b.faults[i].cycle)return false;
    return true;
}
struct Captured {
    std::vector<MainEvent> commands;
    std::vector<IoEvent> io;
    u64 command_count=0,control_count=0,io_count=0,pcm_count=0,io_hash=kHashOffset,pcm_hash=kHashOffset;
    void clear(){commands.clear();io.clear();}
    void finish(const std::vector<float>&pcm){
        for(const auto&c:commands){
            if(c[1]>=0x01e00000u&&c[1]<0x01e00020u)++command_count;
            if(c[1]>=0x01f00000u&&c[1]<0x01f00020u)++control_count;
        }
        io_count+=io.size();pcm_count+=pcm.size();io_hash=hash_vector(io,io_hash);pcm_hash=hash_vector(pcm,pcm_hash);
    }
};
void attach_capture(Machine&m,stv::snd::Board&s,Captured&capture){
    const auto bridge=m.on_sound_write;
    require_restart(bool(bridge),"new main sound bridge is absent");
    m.on_sound_write=[&m,&capture,bridge](u32 address,u16 value){
        capture.commands.push_back({m.c.cyc,address,value});
        bridge(address,value);
    };
    s.on_access=[&capture](const stv::snd::Access&a){
        capture.io.push_back({a.kind,u64(a.t.sec),u64(a.t.as),a.addr,a.data,a.pc});
    };
}
struct FreshOwner {
    Machine m;
    stv::snd::Board s;
    stv::snd::Mixer mixer{48000};
    std::unique_ptr<stv::game::CoScheduler> schedule;
    std::vector<float> pcm;
    bool started=false,done=false;
    FreshOwner(const std::string&rom,bool enhanced,const std::vector<u16>&cmos,
               const std::vector<stv::net::PlayerInput>&p1,const std::vector<stv::net::PlayerInput>&p2){
        std::string error;
        require_restart(m.load_roms(rom,error),"fresh baseline main ROM load");
        if(enhanced){m.enhanced=true;m.set_wide(1024,137);require_restart(stv::gsp::apply_enhanced(m,137),"fresh baseline enhanced data");}
        m.reset();m.cmos=cmos;
        require_restart(s.load_roms(m.snd_u4,m.snd_u19,m.snd_u20,error),"fresh baseline sound ROM load");
        s.mixer=&mixer;s.power_on();schedule=std::make_unique<stv::game::CoScheduler>(m,s);
        m.on_sound_write=[this](u32 address,u16 value){
            if(address>=0x01e00000u&&address<0x01e00020u)schedule->main_sound_write(value);
        };
        schedule->on_base=[this](const stv::snd::Atto&t){s.ym_update(t);mixer.render_until(t,pcm);};
        m.on_vblank=[this,&p1,&p2]{
            const auto frame=m.frame_number;
            stv::net::apply_inputs(m,frame<p1.size()?p1[std::size_t(frame)]:0,frame<p2.size()?p2[std::size_t(frame)]:0);
            done=true;schedule->request_stop();
        };
    }
    void step(){
        done=false;pcm.clear();
        if(!started){started=true;schedule->start(m.reset_vector());schedule->run();}
        else schedule->resume();
        require_restart(done&&!m.stop&&s.fault.empty(),"fresh owner frame stopped");
    }
    ~FreshOwner(){schedule.reset();s.host=nullptr;s.mixer=nullptr;}
};
void intro_startup(const std::string& rom) {
    const std::vector<stv::net::PlayerInput> released;
    for (bool enhanced:{false,true}) {
        Machine initial;std::string error;
        require_restart(initial.load_roms(rom,error),"intro ROM load");initial.reset();
        FreshOwner boot(rom,enhanced,initial.cmos,released,released);
        unsigned title_frames=0,frames=0;bool saw_scoreboard=false;
        while(!startup_intro_ready(boot.m,title_frames)&&frames<4000) {
            saw_scoreboard|=game_scene_state(boot.m)==0x30 && scene_background(boot.m)==0xffea5610u;
            boot.step();++frames;
        }
        require_restart(saw_scoreboard,"intro did not run original scoreboard sequence");
        require_restart(frames<4000&&scene_background(boot.m)==0xffea56d0u,"intro did not land on retail title");
        require_restart(title_frames==320,"intro render settling interval");
        require_restart(!enhanced_scene(boot.m),"title must retain native geometry in either mode");
        std::printf("PASS startup_intro mode=%s frames=%u background=%08x title_frames=%u\n",enhanced?"wide":"classic",frames,scene_background(boot.m),title_frames);
    }
}
struct EnvironmentClear {
    std::vector<std::pair<std::wstring,std::wstring>> old;
    explicit EnvironmentClear(std::initializer_list<const wchar_t*>names){
        for(const auto*name:names){
            const DWORD length=GetEnvironmentVariableW(name,nullptr,0);std::wstring value(length,L'\0');
            if(length){GetEnvironmentVariableW(name,value.data(),length);value.resize(length-1);}
            old.push_back({name,value});SetEnvironmentVariableW(name,nullptr);
        }
    }
    ~EnvironmentClear(){for(const auto&entry:old)SetEnvironmentVariableW(entry.first.c_str(),entry.second.empty()?nullptr:entry.second.c_str());}
};
std::vector<u8> read_bytes(const std::filesystem::path&file){
    std::ifstream in(file,std::ios::binary);
    require_restart(bool(in),"owned CMOS file missing");
    return std::vector<u8>(std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>());
}
void preserve_difference(const std::filesystem::path&dir,const std::vector<u8>&actual,const std::vector<u8>&expected){
    std::ofstream a(dir/"restart-actual-state.bin",std::ios::binary),b(dir/"restart-fresh-state.bin",std::ios::binary);
    a.write(reinterpret_cast<const char*>(actual.data()),std::streamsize(actual.size()));
    b.write(reinterpret_cast<const char*>(expected.data()),std::streamsize(expected.size()));
}
void scenario(const std::string&rom,bool enhanced,const std::filesystem::path&dir,
              const std::vector<stv::net::PlayerInput>&p1,const std::vector<stv::net::PlayerInput>&p2){
    g_mode_menu=false; // Each profile scenario begins in gameplay after the preceding guest returned to the title.
    g_net=NetPlay{};g_rom_path=dir/"retail-l8.dat";
    std::string import_error;
    require_restart(stv::gsp::import_rom_set(rom,g_rom_path,import_error),"import restart fixture");
    g_enhanced=enhanced;g_data_dir=dir.wstring();
    g_m.cmos.assign(0x4000,0);g_script1=p1;g_script2=p2;
    g_settings=stv::host::PcSettings{};g_settings.volume=37;g_settings.muted=true;g_settings.filter=3;
    g_settings.window_scale=3;g_settings.player_name="RESTART";
    const auto preferences=g_settings;
    g_audio.set_volume(37);g_audio.set_muted(true);
    require_restart(!g_audio.ok(),"test unexpectedly opened an output endpoint");
    require_restart(reset_coupled_game(),"initial actual host setup failed");
    for(u32 frame=0;frame<2300;++frame){
        g_pcm.clear();run_one_frame();
        require_restart(g_frame_done&&!g_m.stop&&g_snd.fault.empty(),"priming frame stopped");
    }
    require_restart(g_started&&g_m.frame_number==2299&&g_m.c.cyc>0&&g_snd.cpu.cyc>0&&!g_pcm.empty(),"priming did not exercise coupled owners");
    const auto retained_cmos=g_m.cmos;
    save_cmos();
    const auto disk_before=read_bytes(cmos_path());
    require_restart(disk_before.size()==retained_cmos.size()*sizeof(u16)&&
                    std::memcmp(disk_before.data(),retained_cmos.data(),disk_before.size())==0,"saved owned CMOS content");
    u64 stale_main=0,stale_sound=0;
    g_m.on_input_read=[&](u32,u16){++stale_main;};
    g_m.on_boundary=[&]{++stale_main;};
    g_snd.on_access=[&](const stv::snd::Access&){++stale_sound;};
    g_restart_requested=true;
    require_restart(service_frontend_requests(),"actual restart request not serviced");
    require_restart(!g_restart_requested&&!g_disconnect_requested&&!g_net.on&&!g_paused&&!g_menu,("restart frontend state: menu="+std::to_string(g_menu)+" paused="+std::to_string(g_paused)+" mode="+std::to_string(g_mode_menu)+" status="+g_menu_status).c_str());
    require_restart(!g_started&&!g_frame_done&&g_pcm.empty()&&g_m.c.cyc==0&&g_m.frame_number==0,"restart retained prior execution or PCM");
    require_restart(g_m.cmos==retained_cmos&&read_bytes(cmos_path())==disk_before,"restart changed retained CMOS");
    require_restart(!g_m.on_input_read&&!g_m.on_boundary&&!g_snd.on_access&&stale_main==0&&stale_sound==0,"stale observers survived reconstruction");
    require_restart(g_m.external_timers&&g_snd.host==g_sched.get()&&g_snd.mixer==g_mixer.get()&&
                    bool(g_m.on_sound_write)&&bool(g_m.on_vblank)&&bool(g_sched->on_base),"new owners/callbacks not connected");
    require_restart(g_audio.volume()==37&&g_audio.muted()&&g_settings==preferences,"restart changed output/preferences");
    FreshOwner fresh(rom,enhanced,retained_cmos,p1,p2);
    require_restart(g_m.prog==fresh.m.prog&&g_m.gfx==fresh.m.gfx,"fresh ROM/profile data differs");
    Captured actual,expected;attach_capture(g_m,g_snd,actual);attach_capture(fresh.m,fresh.s,expected);
    std::vector<u8> actual_state,expected_state;
    for(u32 frame=0;frame<1300;++frame){
        actual.clear();expected.clear();g_pcm.clear();run_one_frame();fresh.step();
        require_restart(g_frame_done&&!g_m.stop&&g_snd.fault.empty(),"restarted host frame stopped");
        // Compare the authoritative sound and game owners to an independent
        // fresh timeline while exercising all prediction depths every tick.
        g_settings.run_ahead = 1 + frame % 3;
        const auto& preview = local_video_frame();
        require_restart(&preview != &g_m && preview.frame_number == g_m.frame_number + g_settings.run_ahead,
                        "run-ahead failed to reach its requested lead");
        g_settings.run_ahead = preferences.run_ahead;
        g_m.save_state(actual_state,g_sched->main_pc(),false);
        fresh.m.save_state(expected_state,fresh.schedule->main_pc(),false);
        const bool pcm_equal=g_pcm.size()==fresh.pcm.size()&&
            (g_pcm.empty()||std::memcmp(g_pcm.data(),fresh.pcm.data(),g_pcm.size()*sizeof(float))==0);
        const bool state_equal=actual_state==expected_state&&g_m.run_until_cycle==fresh.m.run_until_cycle&&same_faults(g_m,fresh.m);
        const bool sound_equal=sound_values(g_snd,*g_mixer,*g_sched)==sound_values(fresh.s,fresh.mixer,*fresh.schedule);
        if(actual.commands!=expected.commands||actual.io!=expected.io||!pcm_equal||!state_equal||!sound_equal){
            std::printf("FIRST DIFFERENCE profile=%s frame=%u commands=%d io=%d pcm=%d main=%d sound=%d\n",
                enhanced?"enhanced":"reference",frame,int(actual.commands==expected.commands),int(actual.io==expected.io),
                int(pcm_equal),int(state_equal),int(sound_equal));
            preserve_difference(dir,actual_state,expected_state);
            require_restart(false,"restarted coupled continuation differs from fresh owner");
        }
        actual.finish(g_pcm);expected.finish(fresh.pcm);
    }
    require_restart(actual.command_count>0&&actual.io_count>1000&&actual.pcm_count>48000,"empty command/audio comparison");
    require_restart(actual.command_count==expected.command_count&&actual.io_count==expected.io_count&&
                    actual.pcm_count==expected.pcm_count&&actual.io_hash==expected.io_hash&&actual.pcm_hash==expected.pcm_hash,
                    "matched event receipt inconsistent");
    require_restart(stale_main==0&&stale_sound==0&&g_audio.volume()==37&&g_audio.muted()&&g_settings==preferences,"post-restart owner or preference corruption");
    std::printf("restart profile=%s frames=1300 commands=%llu controls=%llu io=%llu/%016llx pcm=%llu/%016llx exact_state=true\n",
        enhanced?"enhanced":"reference",(unsigned long long)actual.command_count,(unsigned long long)actual.control_count,
        (unsigned long long)actual.io_count,(unsigned long long)actual.io_hash,(unsigned long long)actual.pcm_count,(unsigned long long)actual.pcm_hash);
    g_m.on_sound_write={};g_snd.on_access={};fresh.m.on_sound_write={};fresh.s.on_access={};

    // A guest must leave host CMOS behind and restore its retained local owner.
    // This isolated owner case is followed by real handshake cases below.
    const auto local_cmos=retained_cmos;
    g_m.cmos.assign(local_cmos.size(),0xa55a);
    require_restart(g_m.cmos!=local_cmos,"guest CMOS negative control was not different");
    g_net.on=true;g_net.host=false;g_net.local_cmos=local_cmos;g_disconnect_requested=true;
    require_restart(service_frontend_requests(),"guest disconnect request not serviced");
    require_restart(!g_net.on&&!g_disconnect_requested&&!g_started&&g_m.cmos==local_cmos,"guest did not recover local CMOS");
    require_restart(read_bytes(cmos_path())==disk_before,"guest overwrote local CMOS with host data");
    require_restart(g_audio.volume()==37&&g_audio.muted()&&g_settings==preferences,"disconnect changed preferences");
    g_pcm.clear();run_one_frame();
    require_restart(g_frame_done&&!g_m.stop&&g_snd.fault.empty()&&!g_pcm.empty()&&g_snd.host==g_sched.get(),"guest reset owner not live");
    g_m.on_sound_write={};g_snd.on_access={};
}

enum class JoinCase { PartialCancel, PartialClose, Complete };
struct MessageOnlyWindow {
    HWND previous=g_wnd, window=nullptr;
    HINSTANCE instance=GetModuleHandleW(nullptr);
    const wchar_t* name=L"SmashTVGuestCmosRegression";
    MessageOnlyWindow(){
        WNDCLASSW cls{};cls.lpfnWndProc=wndproc;cls.hInstance=instance;cls.lpszClassName=name;
        require_restart(RegisterClassW(&cls)!=0,"cannot register owned message-only window");
        window=CreateWindowExW(0,name,L"",0,0,0,0,0,HWND_MESSAGE,nullptr,instance,nullptr);
        require_restart(window!=nullptr,"cannot create owned message-only window");
        g_wnd=window;
    }
    ~MessageOnlyWindow(){
        if(IsWindow(window))DestroyWindow(window);
        MSG msg{};while(PeekMessageW(&msg,nullptr,WM_QUIT,WM_QUIT,PM_REMOVE)){}
        g_wnd=previous;UnregisterClassW(name,instance);
    }
};
struct ChunkLimitedTransport : stv::net::Transport {
    stv::net::UdpTransport& udp;
    bool first_chunk_only;
    ChunkLimitedTransport(stv::net::UdpTransport&u,bool only):udp(u),first_chunk_only(only){}
    void send(const std::vector<u8>&d)override{
        // Real handshake framing: type+7-byte magic, then little-endian chunk index.
        if(first_chunk_only&&d.size()>=10&&d[0]=='C'&&(d[8]!=0||d[9]!=0))return;
        udp.send(d);
    }
    std::vector<std::vector<u8>>poll()override{return udp.poll();}
};
struct JoinResult { bool connected=false,quit_preserved=false;int chunks=0;u16 client_port=0; };
JoinResult actual_guest_connect(const std::string&rom,JoinCase test_case,const std::vector<u16>&host_image){
    require_restart(!g_net.on&&!g_net.local_cmos,"previous guest ownership leaked into a new attempt");
    require_restart(reset_coupled_game(),"cannot reset before guest handshake");
    MessageOnlyWindow window;
    stv::net::UdpTransport host;
    std::string error;
    require_restart(host.bind(0,error),"cannot bind owned ephemeral host socket");
    g_net=NetPlay{};g_net.on=true;g_net.host=false;g_net.peer="127.0.0.1";g_net.port=host.local_port();
    auto client=std::make_unique<stv::net::UdpTransport>();
    require_restart(client->bind(0,error),"cannot bind owned ephemeral client socket");
    const u16 client_port=client->local_port();
    require_restart(client->set_peer("127.0.0.1",host.local_port(),error)&&
                    host.set_peer("127.0.0.1",client_port,error),"cannot connect owned loopback sockets");
    g_net.udp=std::move(client);
    auto sent_cmos=host_image;
    stv::net::Handshake handshake(true,stv::net::fingerprint(g_m),sent_cmos);
    ChunkLimitedTransport transport(host,test_case!=JoinCase::Complete);
    bool deadline=false,posted=false;
    int chunks=0;
    std::jthread worker([&](std::stop_token stop){
        const auto begin=GetTickCount64();
        while(!stop.stop_requested()){
            handshake.step(transport);chunks=handshake.chunks_done();
            if(test_case!=JoinCase::Complete&&chunks==1){
                posted=PostMessageW(window.window,test_case==JoinCase::PartialClose?WM_CLOSE:WM_KEYDOWN,
                                    test_case==JoinCase::PartialClose?0:VK_ESCAPE,0)!=0;
                break;
            }
            if(GetTickCount64()-begin>5000){
                deadline=true;PostMessageW(window.window,WM_KEYDOWN,VK_ESCAPE,0);break;
            }
            Sleep(1);
        }
    });
    const bool connected=net_connect(g_rom_path);
    worker.request_stop();worker.join();
    require_restart(!deadline,"owned loopback handshake exceeded five-second deadline");
    require_restart(test_case==JoinCase::Complete||posted,"partial handshake cancellation message failed");
    require_restart(chunks==(test_case==JoinCase::Complete?32:1),"partial/full chunk count was not exercised");
    require_restart(connected==(test_case==JoinCase::Complete),"actual net_connect outcome differs");
    MSG quit{};const bool queued=PeekMessageW(&quit,nullptr,WM_QUIT,WM_QUIT,PM_REMOVE)!=0;
    require_restart(queued==(test_case==JoinCase::PartialClose),"WM_QUIT was lost or unexpectedly produced");
    if(queued)require_restart(quit.wParam==0,"reposted WM_QUIT exit code changed");
    std::printf("guest handshake=%s chunks=%d committed=%d quit_preserved=%d ports=%u/%u\n",
        test_case==JoinCase::Complete?"complete":test_case==JoinCase::PartialClose?"partial-close":"partial-cancel",
        chunks,int(connected),int(queued),host.local_port(),client_port);
    return {connected,queued,chunks,client_port};
}
bool image_on_disk(const std::vector<u16>&image){
    const auto bytes=read_bytes(cmos_path());
    return bytes.size()==image.size()*sizeof(u16)&&std::memcmp(bytes.data(),image.data(),bytes.size())==0;
}
void released_client_socket(u16 port){
    stv::net::UdpTransport check;std::string error;
    require_restart(check.bind(port,error),"closed guest session retained its owned UDP port");
}
void network_cmos_regressions(const std::string&rom,const std::filesystem::path&dir){
    // net_report writes to TEMP; keep every report inside this test's owned directory.
    EnvironmentClear temporary{L"TEMP",L"TMP"};
    const auto temp=std::filesystem::absolute(dir).wstring();
    require_restart(SetEnvironmentVariableW(L"TEMP",temp.c_str())&&SetEnvironmentVariableW(L"TMP",temp.c_str()),
                    "cannot isolate handshake report directory");
    close_lobby();g_net=NetPlay{};g_script1.clear();g_script2.clear();
    const std::vector<u16> older(0x4000,0x1111),latest(0x4000,0x2222),host(0x4000,0xa55a);
    auto seed=[&](const std::vector<u16>&local){
        g_net=NetPlay{};g_m.cmos=older;save_cmos();
        require_restart(image_on_disk(older),"could not seed older owned disk CMOS");
        g_m.cmos=local;
    };
    seed(latest);
    auto cancelled=actual_guest_connect(rom,JoinCase::PartialCancel,host);
    require_restart(g_m.cmos==latest&&g_net.local_cmos&&*g_net.local_cmos==latest,
                    "partial cancelled handshake changed local CMOS ownership");
    require_restart(image_on_disk(older),"cancelled handshake wrote disk without a save");
    g_net=NetPlay{};require_restart(reset_coupled_game(),"cancelled attempt fallback reset failed");
    require_restart(g_m.cmos==latest,"cancelled attempt reset selected partial host CMOS");
    released_client_socket(cancelled.client_port);

    seed(latest);
    auto closed=actual_guest_connect(rom,JoinCase::PartialClose,host);
    require_restart(g_m.cmos==latest&&g_net.local_cmos&&*g_net.local_cmos==latest,
                    "closing a partial join changed the live local CMOS");
    require_restart(image_on_disk(latest),"closing a partial join lost latest local CMOS or saved a host chunk");
    g_net=NetPlay{};released_client_socket(closed.client_port);

    seed(latest);
    auto joined=actual_guest_connect(rom,JoinCase::Complete,host);
    require_restart(g_m.cmos==host&&g_net.mr&&g_net.mr->cmos==host,
                    "successful handshake did not commit the agreed CMOS to both machines");
    require_restart(g_net.local_cmos&&*g_net.local_cmos==latest,"success overwrote the immutable local snapshot");
    save_cmos();
    require_restart(image_on_disk(latest),"guest save selected host CMOS or stale disk image");
    g_m.cmos[0]^=0xffff;
    g_disconnect_requested=true;
    require_restart(service_frontend_requests(),"successful guest disconnect was not serviced");
    require_restart(!g_net.on&&!g_net.local_cmos&&!g_started&&g_m.cmos==latest&&image_on_disk(latest),
                    "guest disconnect failed to restore/clear/save local ownership");
    released_client_socket(joined.client_port);

    // A second join captures a new local image; Restart shares the same restore path.
    auto newer=latest;newer[9]=0x3333;
    seed(newer);
    joined=actual_guest_connect(rom,JoinCase::Complete,host);
    require_restart(g_net.local_cmos&&*g_net.local_cmos==newer,"repeated join reused an earlier local snapshot");
    g_restart_requested=true;
    require_restart(service_frontend_requests()&&!g_net.on&&!g_net.local_cmos&&g_m.cmos==newer&&image_on_disk(newer),
                    "guest restart did not restore the current attempt's local image");
    released_client_socket(joined.client_port);

    // The existing scripted-input rule suppresses local persistence even for a guest snapshot.
    seed(latest);
    joined=actual_guest_connect(rom,JoinCase::Complete,host);
    require_restart(SetEnvironmentVariableW(L"STV_NET_INPUTS",L"owned-script-persistence-fixture")!=0,
                    "cannot set scripted persistence fixture");
    save_cmos();
    require_restart(image_on_disk(older),"scripted guest wrote its local snapshot to disk");
    g_disconnect_requested=true;
    require_restart(service_frontend_requests()&&g_m.cmos==latest&&!g_net.local_cmos&&image_on_disk(older),
                    "scripted disconnect lost local ownership or modified disk");
    SetEnvironmentVariableW(L"STV_NET_INPUTS",nullptr);
    released_client_socket(joined.client_port);
    std::puts("guest CMOS: partial cancel/close, full commit/disconnect, repeated join/restart, scripted persistence PASS");
}

void earned_cmos_roundtrip(const std::filesystem::path&input,const std::filesystem::path&dir,const std::string&rom){
    const auto bytes=read_bytes(input);
    require_restart(bytes.size()==g_m.cmos.size()*sizeof(u16),"earned CMOS image has wrong size");
    std::vector<u16> earned(g_m.cmos.size());
    std::memcpy(earned.data(),bytes.data(),bytes.size());
    g_net=NetPlay{};g_data_dir=dir.wstring();
    std::wstring previous_path;
    for(bool enhanced:{false,true}){
        g_enhanced=enhanced;
        require_restart(cmos_path()!=previous_path,"profiles share a CMOS filename");
        previous_path=cmos_path();
        g_m.cmos=earned;save_cmos();
        require_restart(image_on_disk(earned),"earned image changed during host save");
        g_m=Machine{};
        std::fill(g_m.cmos.begin(),g_m.cmos.end(),u16(0xa5a5));
        load_cmos();
        require_restart(g_m.cmos==earned,"fresh host owner did not reload earned image");
        const std::vector<stv::net::PlayerInput> no_inputs;
        FreshOwner boot(rom,enhanced,g_m.cmos,no_inputs,no_inputs);
        for(unsigned frame=0;frame<2700;++frame)boot.step();
        require_restart(std::equal(earned.begin()+0x2000,earned.begin()+0x2058,
            boot.m.cmos.begin()+0x2000),"retail cold boot changed earned high-score table");
        // A short file must not partially overwrite the live owner's image.
        {std::ofstream truncated(std::filesystem::path(cmos_path()),std::ios::binary|std::ios::trunc);
         truncated.write(reinterpret_cast<const char*>(bytes.data()),23);}
        const auto before=g_m.cmos;load_cmos();
        require_restart(g_m.cmos==before,"truncated CMOS partially replaced earned image");
        save_cmos();require_restart(image_on_disk(earned),"earned image was not restored after rejection");
    }
    std::printf("PASS earned_cmos_roundtrip checks=%d; both profiles, fresh owners,2700-frame cold boots,truncated-file rejection; no window/socket\n",restart_checks);
}
}
int main(int argc,char**argv){
    const bool image_mode=argc==5&&std::strcmp(argv[1],"--cmos-image")==0;
    if(!image_mode&&argc!=4)return 2;
    EnvironmentClear environment{L"STV_HOST_INPUTS",L"STV_NET_INPUTS",L"STV_FRONTEND_REPORT",
        L"STV_NET_FAULT",L"STV_NET_AUDIO_AUDIT",L"STV_NET_AUDIO_TRACE",L"STV_NET_CHECKPOINT_AUDIT",L"STV_NET_FRAMES"};
    const std::filesystem::path dir=std::filesystem::path(argv[3])/("owned-"+std::to_string(GetCurrentProcessId()));
    try{
        require_restart(!std::filesystem::exists(dir),"refusing existing owned test folder");
        require_restart(std::filesystem::create_directories(dir),"cannot create owned CMOS directory");
        if(image_mode){
            earned_cmos_roundtrip(argv[2],dir,argv[4]);
            // Preserve actual on-disk images as evidence for this optional mode.
            std::printf("CMOS artifacts=%s\n",dir.string().c_str());
            return 0;
        }
        // HWND_BOTTOM is a z-order sentinel, not a real window. Passing a nonnull
        // invalid window to InvalidateRect prevents the NULL/all-windows behavior.
        g_wnd=HWND_BOTTOM;
        std::vector<stv::net::PlayerInput>p1,p2;
        require_restart(stv::net::load_scripted_inputs(argv[2],2300,p1,p2),"input schedule load failed");
        intro_startup(argv[1]);
        scenario(argv[1],false,dir,p1,p2);
        scenario(argv[1],true,dir,p1,p2);
        network_cmos_regressions(argv[1],dir);
        g_net=NetPlay{};g_sched.reset();g_snd.host=nullptr;g_snd.mixer=nullptr;g_mixer.reset();
        g_script1.clear();g_script2.clear();
        std::filesystem::remove_all(dir);
        std::printf("PASS host_restart checks=%d; owned loopback handshake only, no physical endpoint or full network gate proof\n",restart_checks);
        return 0;
    }catch(const std::exception&e){
        g_m.on_sound_write={};g_snd.on_access={};g_net=NetPlay{};g_sched.reset();g_snd.host=nullptr;g_snd.mixer=nullptr;g_mixer.reset();
        std::fprintf(stderr,"FAIL host_restart after %d checks: %s; artifacts=%s\n",restart_checks,e.what(),dir.string().c_str());
        return 1;
    }
}
