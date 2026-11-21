#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <xinput.h>
#include <filesystem>
#include <cstdio>
#include <array>
#include <fstream>
#include <iterator>
static bool test_focus=true;
static HWND test_window=reinterpret_cast<HWND>(std::uintptr_t(1));
static ULONGLONG test_clock=100;
static std::array<XINPUT_STATE,2> test_states{};
static std::array<bool,2> test_alive{true,true};
static std::array<SHORT,256> test_keys{};
HWND test_foreground() { return test_focus ? test_window : nullptr; }
SHORT test_key(int vk) { return vk>=0 && vk<256 ? test_keys[vk] : 0; }
ULONGLONG test_millis() { return test_clock; }
DWORD WINAPI test_xinput(DWORD n,XINPUT_STATE* state) {
    if(n>=2 || !test_alive[n]) return ERROR_DEVICE_NOT_CONNECTED;
    *state=test_states[n]; return ERROR_SUCCESS;
}
#define GetForegroundWindow test_foreground
#define GetAsyncKeyState test_key
#define GetTickCount64 test_millis
#define wWinMain unused_host_entry
#include "../src/host/win_main.cpp"
#undef wWinMain
#undef GetTickCount64
#undef GetAsyncKeyState
#undef GetForegroundWindow

static int checks=0,failures=0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; std::printf("FAIL line %d: %s\n",__LINE__,#x); } } while(false)
using namespace stv::host;
void poll(WORD p1=0,WORD p2=0,int elapsed=20) {
    test_states[0].Gamepad.wButtons=p1; test_states[1].Gamepad.wButtons=p2;
    test_clock+=elapsed; poll_pad_controls();
}
void neutral() { test_states={}; poll(); }

// Drive the same host key handler the actual keyboard/pad adapter uses. Page
// structure may grow without tests writing private menu indexes or selection.
bool select_pc_item(PcMenuItem wanted) {
    sync_menu_input();
    if (!g_menu || !g_pc_menu.is_open()) return false;
    auto seek_visible = [&](PcMenuItem item) {
        const auto rows = g_pc_menu.rows(menu_context());
        bool present = false;
        for (const auto& row : rows) if (row.item == item && row.enabled) present = true;
        if (!present) return false;
        for (std::size_t step = 0; step <= rows.size(); ++step) {
            if (g_pc_menu.selected_item() == item) return true;
            handle_menu_key(g_wnd, VK_DOWN);
        }
        return false;
    };
    if (seek_visible(wanted)) return true;
    while (g_pc_menu.page() != PcMenuPage::Main) handle_menu_key(g_wnd, VK_ESCAPE);
    if (seek_visible(wanted)) return true;
    if(!seek_visible(PcMenuItem::Options))return false;
    handle_menu_key(g_wnd,VK_RETURN);
    if(seek_visible(wanted))return true;
    PcMenuItem page = PcMenuItem::None;
    switch (wanted) {
    case PcMenuItem::WindowScale: case PcMenuItem::Fullscreen:
    case PcMenuItem::Scaling: case PcMenuItem::PixelFilter:
    case PcMenuItem::Vsync: case PcMenuItem::ShowFps: case PcMenuItem::PlayfieldProfile:
        page = PcMenuItem::Video; break;
    case PcMenuItem::MasterVolume: case PcMenuItem::Mute: case PcMenuItem::SoundTest:
        page = PcMenuItem::Audio; break;
    case PcMenuItem::KeyboardBindings: case PcMenuItem::PadBindings:
    case PcMenuItem::SwapPads: case PcMenuItem::StickDeadzone: case PcMenuItem::ResetControls:
        page = PcMenuItem::Controls; break;
    case PcMenuItem::PlayerName: case PcMenuItem::DirectHost: case PcMenuItem::HostGame: case PcMenuItem::FindRooms:
    case PcMenuItem::DirectJoin: case PcMenuItem::Disconnect:
        page = PcMenuItem::Multiplayer; break;
    default: break;
    }
    if (page == PcMenuItem::None || !seek_visible(page)) return false;
    handle_menu_key(g_wnd, VK_RETURN);
    return seek_visible(wanted);
}

void analog_aim_tests() {
    g_wnd=test_window; g_xinput=test_xinput; g_menu=false; g_paused=false;
    g_enhanced=true; neutral(); poll();
    auto machine=std::make_unique<Machine>();
    machine->enhanced=true;
    machine->ww(L8_GAME_STATE,0x10);
    constexpr u32 child=0x01086790;
    for (int degrees=0; degrees<360; ++degrees) {
        const double a=degrees*3.14159265358979323846/180;
        test_states[0].Gamepad.sThumbRX=SHORT(std::lround(30000*std::sin(a)));
        test_states[0].Gamepad.sThumbRY=SHORT(std::lround(30000*std::cos(a)));
        poll();
        const auto input=local_player_input();
        CHECK((input>>10)!=0);
        stv::net::apply_inputs(*machine,0,input); // remote player two
        machine->c.a[11]=2; machine->c.a[0]=child;
        machine->latch_shot_aim();
        stv::net::apply_inputs(*machine,0,0); // released before child wakes
        machine->c.a[13]=child; machine->c.a[0]=1;
        machine->bind_shot_compass();
        machine->c.a[5]=0x4201;
        machine->aim_launch(true);
        const double x=s32(machine->c.a[6])*kPixelAspect, y=-double(s32(machine->c.a[7]));
        CHECK((x*std::sin(a)+y*std::cos(a))/std::hypot(x,y)>0.99995);
    }
    neutral();
    test_states[0].Gamepad.sThumbRX=11000; poll(); CHECK(!(local_player_input()>>10));
    test_states[0].Gamepad.sThumbRX=13000; poll(); CHECK(local_player_input()>>10);
    test_states[0].Gamepad.sThumbRX=11000; poll(); CHECK(local_player_input()>>10);
    test_states[0].Gamepad.sThumbRX=9000; poll(); CHECK(!(local_player_input()>>10));
    neutral();
    for (int angle=0;angle<360;++angle) {
        const double a=angle*3.14159265358979323846/180;
        test_states[0].Gamepad.sThumbLX=SHORT(std::lround(30000*std::sin(a)));
        test_states[0].Gamepad.sThumbLY=SHORT(std::lround(30000*std::cos(a)));
        poll();
        const auto input=local_player_input();
        stv::net::apply_inputs(*machine,0,input);
        const auto motion=machine->move_inputs[1];
        const double x=std::int8_t(motion&255)*kPixelAspect,y=-double(std::int8_t(motion>>8));
        CHECK((x*std::sin(a)+y*std::cos(a))/std::hypot(x,y)>0.9999);
    }
    test_states[0].Gamepad.sThumbLX=0;
    int previous=-1;
    for (int radius : {11000,16000,24000,32767}) {
        test_states[0].Gamepad.sThumbLY=SHORT(radius); poll();
        int speed=-std::int8_t(local_player_input()>>24);
        CHECK(speed>previous && (radius!=11000 || speed==0));
        previous=speed;
    }
    g_enhanced=false;
    test_states[0].Gamepad.sThumbRX=32000; poll(); CHECK(!(local_player_input()>>10));
    neutral(); g_enhanced=true;
}

void classic_twin_stick_tests() {
    g_enhanced = false;
    g_deadzone = 12000;
    g_pad_bindings.reset();
    const auto previous_keys = test_keys;
    std::copy(std::begin(kDefaultKeys),std::end(kDefaultKeys),g_keys);
    neutral();
    // Order follows the cabinet contacts U,D,L,R (active low on IN0).
    struct Direction { int x, y; u16 contacts; };
    constexpr Direction sticks[] = {{0,0,0},{0,1,1},{1,1,9},{1,0,8},{1,-1,10},
                                    {0,-1,2},{-1,-1,6},{-1,0,4},{-1,1,5}};
    auto remote = std::make_unique<Machine>();
    for (int radius : {13000, 29000}) {
        for (auto move : sticks) for (auto fire : sticks) {
            auto axis = [radius](int n, Direction d) {
                return SHORT(n * (d.x && d.y ? int(radius / std::sqrt(2.0)) : radius));
            };
            auto& p1 = test_states[0].Gamepad;
            auto& p2 = test_states[1].Gamepad;
            p1.sThumbLX=p2.sThumbRX=axis(move.x,move); p1.sThumbLY=p2.sThumbRY=axis(move.y,move);
            p1.sThumbRX=p2.sThumbLX=axis(fire.x,fire); p1.sThumbRY=p2.sThumbLY=axis(fire.y,fire);
            poll();
            const u16 first=move.contacts | (fire.contacts<<4);
            const u16 second=fire.contacts | (move.contacts<<4);
            sample_inputs();
            CHECK(g_m.ports[0]==u16(~(first | (second<<8))) && g_m.ports[1]==0xffff);
            const auto packet=local_player_input();
            CHECK(packet==first);
            stv::net::apply_inputs(*remote,packet | 0xfffffc00u,second | 0xfffffc00u);
            CHECK(remote->ports[0]==g_m.ports[0] && remote->ports[1]==g_m.ports[1]);
            CHECK((remote->aim_inputs==std::array<u16,2>{} && remote->move_inputs==std::array<u16,2>{}));
        }
    }
    neutral();
    CHECK(stick_bits(8000,8000)==0 && stick_bits(12000,0)==0);
    CHECK(stick_bits(30000,12200)==8 && stick_bits(30000,12600)==9);
    test_keys['W']=test_keys['S']=test_keys[VK_LEFT]=test_keys[VK_RIGHT]=SHORT(0x8000);
    sample_inputs(); CHECK(g_m.ports[0]==0xffff);
    test_keys=previous_keys;
    poll(XINPUT_GAMEPAD_A); sample_inputs(); CHECK(g_m.ports[1]==u16(~4) && g_m.ports[0]==0xffff);
    neutral(); g_enhanced=true;
}

void core_tests() {
    PadBindings bind;
    CHECK(bind.buttons[0]==PadUp && bind.buttons[8]==PadUp);
    CHECK(bind.buttons[16]==PadBack && bind.buttons[19]==PadA);
    CHECK(bind.assign(4,PadUp)==0 && bind.buttons[0]==0 && bind.buttons[4]==PadUp);
    CHECK(bind.buttons[8]==PadUp); // logical player isolation
    CHECK(bind.assign(4,PadA)==18 && bind.assign(5,PadA)==4);
    CHECK(bind.buttons[4]==0 && bind.buttons[5]==PadA);
    CHECK(bind.assign(5,0)==-1 && bind.buttons[5]==0);
    const auto before=bind.buttons;
    for(auto bad : {PadStart,PadLB,PadRB,PadClear,PadA|PadB,std::uint32_t(0x80000000)}) {
        CHECK(bind.assign(2,bad)==-2 && bind.buttons==before);
    }
    CHECK(bind.assign(-1,PadA)==-2 && bind.assign(20,PadA)==-2);
    CHECK(bind.assign(4,PadLT)==-1 && bind.assign(12,PadRT)==-1);
    PadSnapshot none{}, input{true,PadLT,0,0};
    CHECK(!bind.held(4,none) && bind.held(4,input) && !bind.held(12,input));
    bind.reset(); CHECK(bind.buttons[4]==0 && bind.buttons[0]==PadUp);
    PadRouter r;
    CHECK(r.update(0,{true,PadA,0,0},true,0)==PadEvent::None);
    CHECK(!r.game(0).connected); // held on connect must release
    CHECK(r.update(0,{true,0,0,0},true,20)==PadEvent::None && r.game(0).connected);
    CHECK(r.update(0,{true,PadA,0,0},true,40)==PadEvent::None && r.fresh(0)==PadA);
    r.set_overlay(true);
    CHECK(r.update(0,{true,PadA,0,0},true,60)==PadEvent::None);
    r.update(0,{true,0,0,0},true,80);
    CHECK(r.update(0,{true,PadA,0,0},true,100)==PadEvent::Confirm);
    CHECK(r.update(0,{true,PadA,0,0},true,1000)==PadEvent::None); // no repeated activation
    r.update(0,{true,0,0,0},true,1020);
    CHECK(r.update(0,{true,PadDown,0,0},true,1040)==PadEvent::Down);
    CHECK(r.update(0,{true,PadDown,0,0},true,1389)==PadEvent::None);
    CHECK(r.update(0,{true,PadDown,0,0},true,1390)==PadEvent::Down);
    CHECK(r.update(0,{true,PadDown,0,0},true,1509)==PadEvent::None);
    CHECK(r.update(0,{true,PadDown,0,0},true,1510)==PadEvent::Down);
    CHECK(r.update(0,{true,PadUp|PadDown,0,0},true,1530)==PadEvent::None);
    CHECK(r.update(0,{true,0,4,0},true,1550)==PadEvent::Left);
    CHECK(r.update(0,{true,PadL3|PadR3,0,0},true,1570)==PadEvent::Open);
    CHECK(r.update(0,{true,PadL3|PadR3,0,0},true,1590)==PadEvent::None);
    r.set_overlay(false);
    CHECK(!r.game(0).connected);
    r.update(0,{true,0,1,0},true,1610); CHECK(!r.game(0).connected);
    r.update(0,{true,0,0,0},true,1630); CHECK(r.game(0).connected);
    r.update(0,{true,PadStart,0,0},false,1650); CHECK(!r.game(0).connected);
    CHECK(r.update(0,{true,PadStart,0,0},true,1670)==PadEvent::None && !r.game(0).connected);
    r.update(0,{true,0,0,0},true,1690); CHECK(r.game(0).connected);
    r.update(0,{false,0,0,0},true,1710); CHECK(!r.game(0).connected);
    r.update(0,{true,PadStart,0,0},true,1730); CHECK(!r.game(0).connected);
    r.update(0,{true,0,0,0},true,1750); CHECK(r.game(0).connected);
}
void host_tests(const wchar_t* directory) {
    g_wnd=test_window; g_data_dir=directory; CreateDirectoryW(directory,nullptr);
    DeleteFileW(keys_path().c_str()); // exact isolated fixture only
    load_keys(); CHECK(g_keys[P1_UP]=='W' && g_keys[COIN2]=='6' && g_deadzone==12000);
    g_keys[P1_UP]='T'; g_pad_bindings.assign(P1_FUP,PadA); g_deadzone=16000; save_keys();
    g_keys[P1_UP]='Z'; g_pad_bindings.reset(); g_deadzone=8000; load_keys();
    CHECK(g_keys[P1_UP]=='T' && g_pad_bindings.buttons[P1_FUP]==PadA && g_deadzone==16000);
    FILE* f=nullptr; _wfopen_s(&f,keys_path().c_str(),L"w"); CHECK(f!=nullptr);
    if(f) { std::fprintf(f,"0 87\n16 53\npad 4 768\npad 30 4096\npad -1 4096\ndeadzone 0\n"); std::fclose(f); }
    load_keys(); CHECK(g_keys[P1_UP]=='W' && g_keys[COIN1]=='5');
    CHECK(g_pad_bindings.buttons[P1_FUP]==0 && g_deadzone==12000);
    g_xinput=test_xinput; g_menu=false; g_paused=false;
    g_m.ram[(0x010865a0u-0x01000000u)>>4]=0x30;
    wndproc(g_wnd,WM_KEYDOWN,VK_ESCAPE,0); CHECK(g_menu);
    wndproc(g_wnd,WM_KEYDOWN,VK_ESCAPE,0); CHECK(!g_menu);
    wndproc(g_wnd,WM_KEYDOWN,VK_F1,0); CHECK(g_menu && g_pc_menu.is_open());
    wndproc(g_wnd,WM_KEYDOWN,VK_F1,LPARAM(1)<<30); CHECK(g_menu);
    wndproc(g_wnd,WM_KEYDOWN,VK_F1,0); CHECK(!g_menu);

    poll(); poll();
    u16 in0=0,in1=0;
    poll(XINPUT_GAMEPAD_DPAD_UP|XINPUT_GAMEPAD_BACK,XINPUT_GAMEPAD_DPAD_RIGHT|XINPUT_GAMEPAD_A);
    read_controls(in0,in1); CHECK(in0==0x0801 && in1==0x0021);
    neutral();
    test_states[0].Gamepad.sThumbLX=20000; test_states[0].Gamepad.sThumbRY=20000; poll();
    read_controls(in0,in1); CHECK(in0==0x18 && in1==4);
    neutral();
    g_swap_pads=true;
    poll(0,XINPUT_GAMEPAD_DPAD_LEFT|XINPUT_GAMEPAD_BACK); read_controls(in0,in1);
    CHECK(in0==4 && in1==1); g_swap_pads=false; neutral();
    test_focus=false; poll(XINPUT_GAMEPAD_START); read_controls(in0,in1); CHECK(in0==0 && in1==0);
    test_focus=true; poll(XINPUT_GAMEPAD_START); read_controls(in0,in1); CHECK(in1==0);
    neutral(); poll(XINPUT_GAMEPAD_START); read_controls(in0,in1); CHECK(g_menu && in0==0 && in1==0);
    neutral(); poll(XINPUT_GAMEPAD_START); CHECK(!g_menu); neutral();
    test_alive[0]=false; poll(); read_controls(in0,in1); CHECK(in1==0);
    test_alive[0]=true;
    for(int n=0;n<61;++n) poll(XINPUT_GAMEPAD_START);
    read_controls(in0,in1); CHECK(in1==0); // held reconnect must release
    neutral(); poll(XINPUT_GAMEPAD_START); read_controls(in0,in1); CHECK(g_menu && in0==0 && in1==0);
    neutral(); poll(XINPUT_GAMEPAD_START); CHECK(!g_menu); neutral();
    poll(XINPUT_GAMEPAD_LEFT_SHOULDER|XINPUT_GAMEPAD_RIGHT_SHOULDER); CHECK(!g_menu); neutral();
    poll(XINPUT_GAMEPAD_LEFT_THUMB); CHECK(!g_menu);
    g_paused=true; poll(XINPUT_GAMEPAD_LEFT_THUMB|XINPUT_GAMEPAD_RIGHT_THUMB);
    CHECK(g_menu && g_paused && g_pc_menu.selected_item()==PcMenuItem::Resume);
    poll(XINPUT_GAMEPAD_LEFT_THUMB|XINPUT_GAMEPAD_RIGHT_THUMB);
    CHECK(g_menu && g_pc_menu.selected_item()==PcMenuItem::Resume); neutral();
    poll(XINPUT_GAMEPAD_DPAD_UP); CHECK(g_pc_menu.selected_item()==PcMenuItem::MainMenu);
    neutral(); poll(XINPUT_GAMEPAD_DPAD_DOWN); CHECK(g_pc_menu.selected_item()==PcMenuItem::Resume); neutral();
    CHECK(select_pc_item(PcMenuItem::PadBindings)); poll(XINPUT_GAMEPAD_A); CHECK(g_pad_controls && !g_pad_capture);
    poll(XINPUT_GAMEPAD_A); CHECK(!g_pad_capture); neutral();
    poll(XINPUT_GAMEPAD_DPAD_RIGHT); CHECK(g_pad_player==1); neutral();
    poll(XINPUT_GAMEPAD_DPAD_LEFT); CHECK(g_pad_player==0); neutral();
    g_pad_selection=4; poll(XINPUT_GAMEPAD_A); CHECK(g_pad_capture);
    poll(XINPUT_GAMEPAD_A); CHECK(g_pad_capture && g_pad_bindings.buttons[P1_FUP]==0);
    neutral(); poll(0,XINPUT_GAMEPAD_Y); CHECK(g_pad_capture && g_pad_bindings.buttons[P1_FUP]==0);
    neutral(); poll(XINPUT_GAMEPAD_A); CHECK(!g_pad_capture && g_pad_bindings.buttons[P1_FUP]==PadA);
    neutral();
    g_pad_selection=5; poll(XINPUT_GAMEPAD_A); neutral(); poll(XINPUT_GAMEPAD_A);
    CHECK(!g_pad_capture && g_pad_bindings.buttons[P1_FDOWN]==PadA && g_pad_bindings.buttons[P1_FUP]==0);
    neutral();
    poll(XINPUT_GAMEPAD_A); neutral(); poll(XINPUT_GAMEPAD_RIGHT_SHOULDER);
    CHECK(!g_pad_capture && g_pad_bindings.buttons[P1_FDOWN]==0);
    neutral(); poll(XINPUT_GAMEPAD_A); CHECK(g_pad_capture); neutral();
    poll(XINPUT_GAMEPAD_LEFT_THUMB|XINPUT_GAMEPAD_RIGHT_THUMB);
    CHECK(g_pad_controls && !g_pad_capture && g_menu); neutral();
    g_pad_selection=4; poll(XINPUT_GAMEPAD_A); neutral();
    test_states[0].Gamepad.bLeftTrigger=31; poll();
    CHECK(!g_pad_capture && g_pad_bindings.buttons[P1_FUP]==PadLT); neutral();
    poll(XINPUT_GAMEPAD_X); CHECK(g_pad_bindings.buttons[P1_FUP]==0 && g_pad_bindings.buttons[P1_UP]==PadUp); neutral();
    g_pad_bindings.assign(P1_FUP,PadA);
    poll(XINPUT_GAMEPAD_B); CHECK(!g_pad_controls && g_menu); neutral();
    CHECK(select_pc_item(PcMenuItem::Resume)); poll(XINPUT_GAMEPAD_A);
    CHECK(!g_menu && !g_paused); read_controls(in0,in1); CHECK(in0==0 && in1==0);
    poll(XINPUT_GAMEPAD_A); read_controls(in0,in1); CHECK(in0==0);
    neutral(); poll(XINPUT_GAMEPAD_A); read_controls(in0,in1); CHECK(in0==0x10); neutral();
    g_menu=true; g_pad_router.set_overlay(true); neutral();
    CHECK(select_pc_item(PcMenuItem::LocalCoop)); handle_menu_key(g_wnd,VK_RETURN); CHECK(!g_menu && !g_paused);
    g_menu=true; g_pad_router.set_overlay(true); neutral();
    CHECK(select_pc_item(PcMenuItem::StickDeadzone)); handle_menu_key(g_wnd,VK_RETURN); CHECK(g_deadzone==16000);
    const int dz=g_deadzone; wndproc(g_wnd,WM_KEYDOWN,VK_RETURN,LPARAM(1)<<30); CHECK(g_deadzone==dz);
    g_pad_bindings.reset(); load_keys(); CHECK(g_deadzone==16000 && g_pad_bindings.buttons[P1_FUP]==PadA);
    g_menu=false; g_pad_router.set_overlay(false); neutral();
    poll(XINPUT_GAMEPAD_A,XINPUT_GAMEPAD_LEFT_THUMB|XINPUT_GAMEPAD_RIGHT_THUMB);
    CHECK(g_menu); // the other pad's chord takes priority over a same-poll button
    neutral(); CHECK(select_pc_item(PcMenuItem::KeyboardBindings)); poll(XINPUT_GAMEPAD_A); CHECK(g_remap==0);
    const auto old_key=g_keys[0]; neutral(); poll(XINPUT_GAMEPAD_A); CHECK(g_keys[0]==old_key && g_remap==0);
    neutral(); poll(XINPUT_GAMEPAD_B); CHECK(g_remap==-1 && g_menu); neutral();
    g_paused=true; CHECK(select_pc_item(PcMenuItem::OperatorMenu)); poll(XINPUT_GAMEPAD_A);
    CHECK(!g_menu && !g_paused && g_test_frames==30); neutral();
    read_controls(in0,in1); CHECK(in1==0x10 && g_test_frames==29);
    g_test_frames=1; read_controls(in0,in1); CHECK(in1==0x10 && g_test_frames==0);
    read_controls(in0,in1); CHECK(in1==0);
    g_deadzone=12000; CHECK(stick_bits(12000,0)==0 && stick_bits(12001,0)==8 && stick_bits(-12001,0)==4);
    g_deadzone=16000; CHECK(stick_bits(15000,0)==0 && stick_bits(16001,0)==8);
    g_keys[P1_FUP]=VK_RETURN;
    g_menu=true; sync_menu_input(); CHECK(select_pc_item(PcMenuItem::Resume));
    test_keys[VK_RETURN]=SHORT(0x8000);
    wndproc(g_wnd,WM_KEYDOWN,VK_RETURN,0); read_controls(in0,in1);
    CHECK(!g_menu && in0==0 && g_keyboard_blocked);
    read_controls(in0,in1); CHECK(in0==0);
    test_keys[VK_RETURN]=0; read_controls(in0,in1); CHECK(!g_keyboard_blocked);
    test_keys[VK_RETURN]=SHORT(0x8000); read_controls(in0,in1); CHECK(in0==0x10);
    test_keys[VK_RETURN]=0;
    g_paused=false; wndproc(g_wnd,WM_KEYDOWN,'P',LPARAM(1)<<30); CHECK(!g_paused);
    g_m.ram[(0x010865a0u-0x01000000u)>>4]=0x10;
    g_m.ram[(0x01086360u-0x01000000u)>>4]=3;
    g_fire_start.reset();neutral();poll(XINPUT_GAMEPAD_A);read_controls(in0,in1);CHECK(in1==0);
    g_m.ram[(0x01086360u-0x01000000u)>>4]=0;
    read_controls(in0,in1);CHECK(in1==0);
    neutral();read_controls(in0,in1);poll(XINPUT_GAMEPAD_A);read_controls(in0,in1);CHECK(in1==4);
    neutral();read_controls(in0,in1);
    const auto saved=g_pad_bindings;g_pad_bindings.reset();
    g_m.ram[(0x010865a0u-0x01000000u)>>4]=0x40;
    poll(XINPUT_GAMEPAD_A);read_controls(in0,in1);CHECK(in1==4); // operator confirm stays reachable
    neutral();read_controls(in0,in1);g_pad_bindings=saved;
}

void host_connect_cancel_tests() {
    // The online handshake closes PcMenu but must retain controller navigation
    // ownership. Back cancels once, and that same held button cannot fire after
    // the connection closes: a neutral state is required before gameplay.
    const auto saved_bindings=g_pad_bindings;
    g_m.ram[(0x010865a0u-0x01000000u)>>4]=0x10;
    g_m.ram[(0x01086360u-0x01000000u)>>4]=3;
    g_pad_bindings.assign(P1_FUP,PadB);
    g_menu=false; g_paused=false; g_connecting=true; g_cancel_connect=false;
    sync_menu_input(); neutral();
    u16 in0=0,in1=0;
    poll(XINPUT_GAMEPAD_B);
    CHECK(g_cancel_connect);
    read_controls(in0,in1); CHECK(in0==0 && in1==0);
    g_cancel_connect=false;
    poll(XINPUT_GAMEPAD_B); CHECK(!g_cancel_connect);
    g_connecting=false; sync_menu_input();
    poll(XINPUT_GAMEPAD_B); read_controls(in0,in1);
    CHECK(in0==0 && in1==0);
    neutral(); poll(XINPUT_GAMEPAD_B); read_controls(in0,in1);
    CHECK(in0==0x10 && in1==0);
    neutral(); g_pad_bindings=saved_bindings; g_cancel_connect=false;
}

std::string read_fixture_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), {});
}

void host_preferences_tests() {
    // Use the same isolated directory the existing host fixture supplied.
    // No endpoint/window/ROM/socket is opened; actual host event handlers apply
    // preferences and write only this fixture's two settings files.
    g_menu=false; g_paused=false; g_name_edit=false; g_join_edit=false;
    g_pad_controls=false; g_pad_capture=false; g_remap=-1;
    g_confirm=ConfirmAction::None; g_confirm_yes=false;
    sync_menu_input(); neutral();
    DeleteFileW(preferences_path().c_str());
    load_preferences();
    CHECK(g_settings==PcSettings{});
    CHECK(g_audio.volume()==100 && !g_audio.muted());
    CHECK(g_scale_mode==ScaleMode::IntegerFill && !g_swap_pads);
    const std::string keys_before=read_fixture_text(keys_path());
    const std::vector<float> pcm_before{0.25f,-0.5f,1.25f};
    const std::vector<u32> pixels_before{0x00112233,0x00445566,0x00778899};
    g_pcm=pcm_before; g_pixels=pixels_before;

    g_menu=true; sync_menu_input(); neutral();
    CHECK(select_pc_item(PcMenuItem::MasterVolume));
    poll(XINPUT_GAMEPAD_DPAD_LEFT);
    CHECK(g_settings.volume==95 && g_audio.volume()==95); neutral();
    for(int n=0;n<25;++n) handle_menu_key(g_wnd,VK_LEFT);
    CHECK(g_settings.volume==0 && g_audio.volume()==0);
    for(int n=0;n<25;++n) handle_menu_key(g_wnd,VK_RIGHT);
    CHECK(g_settings.volume==100 && g_audio.volume()==100);
    handle_menu_key(g_wnd,VK_RETURN);
    CHECK(g_settings.volume==100);  // clamped even on another explicit activation
    handle_menu_key(g_wnd,VK_LEFT); handle_menu_key(g_wnd,VK_LEFT);
    CHECK(g_settings.volume==90 && g_audio.volume()==90);
    CHECK(select_pc_item(PcMenuItem::Mute));
    poll(XINPUT_GAMEPAD_A); CHECK(g_settings.muted && g_audio.muted());
    poll(XINPUT_GAMEPAD_A); CHECK(g_settings.muted && g_audio.muted()); // held is one action
    neutral();
    CHECK(select_pc_item(PcMenuItem::PixelFilter));
    handle_menu_key(g_wnd,VK_LEFT); CHECK(g_settings.filter==7);
    handle_menu_key(g_wnd,VK_RIGHT); CHECK(g_settings.filter==0);
    handle_menu_key(g_wnd,VK_RETURN); CHECK(g_settings.filter==1);
    CHECK(select_pc_item(PcMenuItem::ShowFps));
    poll(XINPUT_GAMEPAD_A); CHECK(g_settings.show_fps); neutral();
    CHECK(select_pc_item(PcMenuItem::SwapPads));
    poll(XINPUT_GAMEPAD_A); CHECK(g_swap_pads && g_settings.swap_pads); neutral();
    CHECK(g_pcm==pcm_before && g_pixels==pixels_before); // settings act at presentation/output only
    CHECK(read_fixture_text(keys_path())==keys_before);  // PC prefs do not rewrite bindings

    const PcSettings expected=g_settings;
    g_settings=PcSettings{}; g_swap_pads=false; g_scale_mode=ScaleMode::IntegerExact;
    g_audio.set_volume(1); g_audio.set_muted(false);
    load_preferences();
    CHECK(g_settings==expected);
    CHECK(g_audio.volume()==90 && g_audio.muted() && g_swap_pads);
    CHECK(g_scale_mode==ScaleMode::IntegerFill);
    CHECK(g_pcm==pcm_before && g_pixels==pixels_before);

    // A scripted capture must not overwrite a person's PC preference file.
    const std::string saved=read_fixture_text(preferences_path());
    const DWORD env_size=GetEnvironmentVariableW(L"STV_HOST_INPUTS",nullptr,0);
    std::wstring old_env(env_size,L'\0');
    if(env_size) {
        const DWORD got=GetEnvironmentVariableW(L"STV_HOST_INPUTS",old_env.data(),env_size);
        old_env.resize(got);
    }
    CHECK(SetEnvironmentVariableW(L"STV_HOST_INPUTS",L"fixture-only-no-replay")!=0);
    g_settings.volume=5;
    save_preferences();
    CHECK(read_fixture_text(preferences_path())==saved);
    CHECK(SetEnvironmentVariableW(L"STV_HOST_INPUTS",env_size?old_env.c_str():nullptr)!=0);
    load_preferences(); CHECK(g_settings==expected && g_audio.volume()==90);

    // Corrupt saved values must reset the real output controls as well as the model.
    {
        std::ofstream invalid(preferences_path(),std::ios::binary|std::ios::trunc);
        invalid<<"version=1\nvolume=101\n";
    }
    g_menu_status.clear(); load_preferences();
    CHECK(g_settings==PcSettings{} && g_audio.volume()==100 && !g_audio.muted());
    CHECK(!g_swap_pads && g_scale_mode==ScaleMode::IntegerFill && !g_menu_status.empty());
    CHECK(read_fixture_text(keys_path())==keys_before);
    g_settings=expected; g_swap_pads=expected.swap_pads;
    g_scale_mode=static_cast<ScaleMode>(expected.scaling);
    save_preferences(); load_preferences();

    // Controller-only name editing goes through the same pad router as gameplay.
    CHECK(select_pc_item(PcMenuItem::PlayerName));
    poll(XINPUT_GAMEPAD_A);
    CHECK(g_name_edit && g_name_draft=="PLAYER" && g_name_cursor==0);
    poll(XINPUT_GAMEPAD_A); CHECK(g_name_edit); neutral();
    poll(XINPUT_GAMEPAD_DPAD_UP); CHECK(g_name_draft=="QLAYER"); neutral();
    poll(XINPUT_GAMEPAD_DPAD_RIGHT); CHECK(g_name_cursor==1); neutral();
    poll(XINPUT_GAMEPAD_DPAD_DOWN); CHECK(g_name_draft=="QKAYER"); neutral();
    poll(XINPUT_GAMEPAD_X); CHECK(g_name_draft=="KAYER" && g_name_cursor==0); neutral();
    u16 in0=0,in1=0;
    read_controls(in0,in1); CHECK(in0==0 && in1==0);
    poll(XINPUT_GAMEPAD_A);
    CHECK(!g_name_edit && g_menu && g_settings.player_name=="KAYER");
    poll(XINPUT_GAMEPAD_A); CHECK(!g_name_edit); neutral();
    g_settings.player_name="RESET"; load_preferences();
    CHECK(g_settings.player_name=="KAYER");

    const std::string name_saved=read_fixture_text(preferences_path());
    CHECK(select_pc_item(PcMenuItem::PlayerName));
    poll(XINPUT_GAMEPAD_A); neutral();
    poll(XINPUT_GAMEPAD_DPAD_UP); CHECK(g_name_edit && g_name_draft=="LAYER"); neutral();
    poll(XINPUT_GAMEPAD_B);
    CHECK(!g_name_edit && g_menu && g_settings.player_name=="KAYER"); neutral();
    CHECK(read_fixture_text(preferences_path())==name_saved);

    // Real WM_CHAR handling folds lowercase and ignores unsupported characters.
    CHECK(select_pc_item(PcMenuItem::PlayerName));
    handle_menu_key(g_wnd,VK_RETURN);
    while(!g_name_draft.empty()) handle_menu_key(g_wnd,VK_BACK);
    for(wchar_t c : std::wstring(L"jo! d")) wndproc(g_wnd,WM_CHAR,WPARAM(c),0);
    CHECK(g_name_draft=="JO D" && g_name_cursor==4);
    handle_menu_key(g_wnd,VK_RETURN);
    CHECK(!g_name_edit && g_settings.player_name=="JO D");
    g_settings.player_name="RESET"; load_preferences(); CHECK(g_settings.player_name=="JO D");
    CHECK(g_pcm==pcm_before && g_pixels==pixels_before);

    g_password_prompt=PasswordPrompt::Join; g_password_draft="ab"; g_password_cursor=0;
    handle_menu_key(g_wnd,VK_BACK); CHECK(g_password_draft=="ab" && g_password_cursor==0);
    handle_menu_key(g_wnd,VK_RIGHT); handle_menu_key(g_wnd,VK_BACK);
    CHECK(g_password_draft=="b" && g_password_cursor==0);
    handle_menu_key(g_wnd,VK_ESCAPE); CHECK(g_password_draft.empty());

    // Defaults update both the existing binding owner and the PC preference owner.
    CHECK(select_pc_item(PcMenuItem::ResetControls));
    handle_menu_key(g_wnd,VK_RETURN);
    CHECK(g_deadzone==12000 && !g_swap_pads && !g_settings.swap_pads);
    for(int i=0;i<ACTION_COUNT;++i) CHECK(g_keys[i]==kDefaultKeys[i]);
    CHECK(g_pad_bindings.buttons[P1_UP]==PadUp && g_pad_bindings.buttons[P1_FUP]==0);
    load_keys(); load_preferences();
    CHECK(g_deadzone==12000 && !g_swap_pads && g_settings.player_name=="JO D");
    CHECK(g_settings.volume==90 && g_audio.volume()==90 && g_audio.muted());
    g_menu=false; sync_menu_input(); neutral(); g_pcm.clear(); g_pixels.clear();
}

void start_fire_policy_tests() {
    FireStartRouter fire;
    CHECK(!fire.update(0,true,false));
    CHECK(!fire.update(0,true,true)); // holding fire through death cannot buy a continue
    CHECK(!fire.update(0,false,true));
    CHECK(fire.update(0,true,true));
    CHECK(fire.update(0,true,true)); // level remains stable between polling and simulation
    CHECK(!fire.update(0,true,false));
    CHECK(!fire.update(0,true,true)); // prior start press cannot rearm after a later death
    CHECK(fire.update(1,true,true)); // independent second-player admission
    fire.reset();CHECK(fire.update(0,true,true));
    PadRouter pad;pad.update(0,{true,0,0,0},true,0);pad.update(0,{true,0,0,0},true,10);
    CHECK(pad.update(0,{true,PadStart,0,0},true,20)==PadEvent::Open);
    CHECK(pad.update(0,{true,PadStart,0,0},true,40)==PadEvent::None);
    CHECK(!PadBindings::valid(PadStart));
}

int main(int argc,char** argv) {
    CHECK(valid_join_address(L"192.168.1.24"));
    CHECK(valid_join_address(L"my-host.example:65535"));
    CHECK(!valid_join_address(L":27015") && !valid_join_address(L"host:0"));
    CHECK(!valid_join_address(L"host:65536") && !valid_join_address(L"host:"));
    CHECK(!valid_join_address(L"host:12x") && !valid_join_address(L"host --host"));
    start_fire_policy_tests();
    if(argc!=2) return 2;
    analog_aim_tests(); classic_twin_stick_tests(); core_tests(); const auto dir=std::filesystem::path(argv[1]).wstring(); host_tests(dir.c_str()); host_connect_cancel_tests(); host_preferences_tests();
    std::printf("%d checks, %d failures\n",checks,failures);
    return failures ? 1 : 0;
}
