#include "stv/win32_min.hpp"
#include "stv/session.hpp"
#include "stv/assets.hpp"
#include "stv/frontend.hpp"
#include "stv/reference_timing.hpp"
using namespace stv;using namespace win;
namespace {
constexpr UINT destroy_msg=2,paint_msg=15,timer_msg=0x113,keydown_msg=0x100,activate_msg=0x1c,mousewheel_msg=0x20a;
constinit SourcePack pack{};
GameSession game_session{};
constinit UiFrontend frontend{};
constinit EditorImages browser_images{};
constinit EditorBackground browser_background{};
HWND window{};HINSTANCE instance{};HBRUSH black{};HANDLE xinput_dll{};XINPUT_GET_STATE xinput{};
u32 canvas[2048*2048]{};unsigned canvas_width=lab_width,canvas_height=lab_height;
unsigned mode=0,image_file=0,image_index=0,bg_file=0,bg_module=0,active_players=2;
unsigned image_entries[128]{},background_entries[128]{},image_files{},background_files{};
DigitalStick sticks[2][2]{};StickSettings settings{};ControllerAssignment controller_assignment{};TickClock frontend_clock{},lab_clock{};
bool connected[2]{},previous_start[2]{},paused=false,focused=true,fullscreen=false,integer_scale=true,ready=false;
bool pc_menu_open=false,pc_settings_open=false;unsigned pc_menu_selection=0,pc_settings_selection=0;u16 previous_menu_buttons{};
constexpr unsigned pc_menu_count=7;
constexpr unsigned pc_settings_count=6;
const char* pc_menu_items[pc_menu_count]={"RESUME","LOCAL CO-OP","HOST","JOIN","SETTINGS","OPERATOR MENU","QUIT"};
i64 previous_counter{};u64 pending_ticks{};RECT saved_rect{};i64 saved_style{};
char message[2048]="Loading supplied source assets...";wchar_t wide[8192]{},settings_path[2048]{};
KeyboardBindings keyboard_bindings{};
int remap=-1;
wchar_t operator_path[2048]{};bool operator_write_enabled=false,operator_write_failed=false;
u32 controller_buttons{};bool action_start[2]{},action_coin[2]{};bool host_back=false;u64 last_saved_mutation{};
const wchar_t* binding_names[20]={L"P1MoveUp",L"P1MoveDown",L"P1MoveLeft",L"P1MoveRight",L"P1FireUp",L"P1FireDown",L"P1FireLeft",L"P1FireRight",L"P1Start",L"P1Coin",L"P2MoveUp",L"P2MoveDown",L"P2MoveLeft",L"P2MoveRight",L"P2FireUp",L"P2FireDown",L"P2FireLeft",L"P2FireRight",L"P2Start",L"P2Coin"};
const char* binding_display[20]={"P1 move up","P1 move down","P1 move left","P1 move right","P1 fire up","P1 fire down","P1 fire left","P1 fire right","P1 start","P1 coin","P2 move up","P2 move down","P2 move left","P2 move right","P2 fire up","P2 fire down","P2 fire left","P2 fire right","P2 start","P2 coin"};
InputFrame session_input(const LabInput& input){InputFrame frame;frame.boundary=game_session.boundary;frame.timestamp=game_session.boundary;for(unsigned player=0;player<2;++player){frame.player[player].movement=input.move[player];frame.player[player].shooting=input.fire[player];frame.player[player].start=action_start[player];frame.player[player].coin=action_coin[player];}return frame;}
void widen(const char* text,wchar_t* out,usize cap){usize i=0;while(text[i]&&i+1<cap){out[i]=wchar_t(u8(text[i]));++i;}if(cap)out[i]=0;}
usize wlen(const wchar_t* p){usize n=0;while(p[n])++n;return n;}
bool append(wchar_t* to,usize cap,const wchar_t* value){usize a=wlen(to),b=wlen(value);if(a+b>=cap)return false;copy(to+a,value,(b+1)*sizeof(wchar_t));return true;}
void set_message(const char* text){TextWriter out{message,sizeof message};out.text(text);}
void report(Status s){TextWriter out{message,sizeof message};out.text("ERROR: ");out.text(s.message?s.message:"unknown failure");out.text(" (offset/index ");out.number(s.offset);out.text(")");paused=true;pending_ticks=0;InvalidateRect(window,nullptr,0);}
bool key(unsigned code){return (GetAsyncKeyState(int(code))&0x8000)!=0;}
void reset_clock(){QueryPerformanceCounter(&previous_counter);frontend_clock.phase=lab_clock.phase=0;pending_ticks=0;}
void decimal_w(unsigned value,wchar_t* out){char text[32];TextWriter writer{text,sizeof text};writer.number(value);widen(text,out,32);}
void save_settings(){if(!settings_path[0])return;wchar_t text[32];bool okay=true;
 for(unsigned i=0;i<20;++i){decimal_w(keyboard_bindings.keys[i/10][i%10],text);okay=WritePrivateProfileStringW(L"Keys",binding_names[i],text,settings_path)&&okay;}
 decimal_w(settings.engage,text);okay=WritePrivateProfileStringW(L"Input",L"Engage",text,settings_path)&&okay;
 decimal_w(settings.release,text);okay=WritePrivateProfileStringW(L"Input",L"Release",text,settings_path)&&okay;
 okay=WritePrivateProfileStringW(L"Video",L"IntegerScale",integer_scale?L"1":L"0",settings_path)&&okay;
 decimal_w(controller_assignment.xinput_slot[0],text);okay=WritePrivateProfileStringW(L"Controllers",L"P1Slot",text,settings_path)&&okay;
 decimal_w(controller_assignment.xinput_slot[1],text);okay=WritePrivateProfileStringW(L"Controllers",L"P2Slot",text,settings_path)&&okay;
 if(!okay)set_message("Settings could not be saved. This does not block source-lab operation.");
}
void load_settings(){DWORD n=GetEnvironmentVariableW(L"LOCALAPPDATA",settings_path,2048);if(!n||n>=1900){settings_path[0]=0;return;}
 if(!append(settings_path,2048,L"\\SmashTVNativeLab")){settings_path[0]=0;return;}
 if(!CreateDirectoryW(settings_path,nullptr)&&GetLastError()!=183){settings_path[0]=0;return;}
 if(!append(settings_path,2048,L"\\settings.ini")){settings_path[0]=0;return;}
 for(unsigned i=0;i<20;++i){UINT k=GetPrivateProfileIntW(L"Keys",binding_names[i],int(keyboard_bindings.keys[i/10][i%10]),settings_path);if(k>0&&k<256)keyboard_bindings.keys[i/10][i%10]=u16(k);}
 ControllerAssignment assignment;UINT p1=GetPrivateProfileIntW(L"Controllers",L"P1Slot",0,settings_path),p2=GetPrivateProfileIntW(L"Controllers",L"P2Slot",1,settings_path);
 if(p1<ControllerAssignment::device_limit&&p2<ControllerAssignment::device_limit&&assignment.assign(0,p1)&&assignment.assign(1,p2))controller_assignment=assignment;
 if(!keyboard_bindings.validate()){keyboard_bindings=KeyboardBindings{};set_message("Conflicting saved key bindings were rejected; default two-player mappings restored.");}
 StickSettings candidate=settings;candidate.engage=u16(GetPrivateProfileIntW(L"Input",L"Engage",9000,settings_path));candidate.release=u16(GetPrivateProfileIntW(L"Input",L"Release",7000,settings_path));if(validate_stick_settings(candidate))settings=candidate;
 integer_scale=GetPrivateProfileIntW(L"Video",L"IntegerScale",1,settings_path)!=0;
}
// This is a versioned native state file, not the original machine's CMOS layout.
// Existing damaged files are preserved before defaults may replace them.
Status start_frontend(){
 operator_write_enabled=false;operator_write_failed=false;operator_path[0]=0;
 UiPersistent restored;bool valid=false;
 DWORD n=GetEnvironmentVariableW(L"LOCALAPPDATA",operator_path,2048);
 if(n&&n<1900&&append(operator_path,2048,L"\\SmashTVNativeLab")){
  if((CreateDirectoryW(operator_path,nullptr)||GetLastError()==183)&&append(operator_path,2048,L"\\operator-state-v5.bin")){
   operator_write_enabled=true;
   HANDLE file=CreateFileW(operator_path,0x80000000,1,nullptr,3,0x80,nullptr);
   if(file!=reinterpret_cast<HANDLE>(~usize(0))){
    u8 data[ui_persistent_size];i64 size=0;DWORD got=0;
    bool read_ok=GetFileSizeEx(file,&size)&&size==i64(ui_persistent_size)&&ReadFile(file,data,DWORD(sizeof data),&got,nullptr)&&got==sizeof data;
    CloseHandle(file);
    valid=read_ok&&bool(decode_ui_persistent({data,sizeof data},restored));
    if(!valid){
     wchar_t quarantine[2048];copy(quarantine,operator_path,(wlen(operator_path)+1)*sizeof(wchar_t));
     bool preserved=false;
     for(unsigned i=0;i<100&&!preserved;++i){
      quarantine[wlen(operator_path)]=0;wchar_t number[16];decimal_w(i,number);
      if(!append(quarantine,2048,L".invalid.")||!append(quarantine,2048,number))break;
      preserved=MoveFileExW(operator_path,quarantine,8)!=0; // no replace of earlier evidence
     }
     if(!preserved){operator_write_enabled=false;set_message("Invalid native settings could not be preserved; writes disabled. Factory recovery remains visible.");}
     else set_message("Invalid native settings preserved as .invalid.N; source factory-recovery screen will run.");
    }
   }else if(GetLastError()!=2&&GetLastError()!=3){operator_write_enabled=false;set_message("Native settings could not be read; writes disabled to protect existing data.");}
  }
 }
 auto status=frontend.boot(valid?&restored:nullptr);if(!status)return status;
 status=game_session.import_operator_state(frontend.settings);if(!status)return status;
 last_saved_mutation=0;
 if(valid)set_message("Native operator settings restored. Original gameplay/credit entry and hardware POST remain incomplete.");
 else if(operator_write_enabled)set_message("Source FACCHECK default-recovery path. Native file integrity is checked; PCB POST is not implemented.");
 return {};
}
void save_operator(){
 if(!operator_write_enabled||operator_write_failed||!frontend.persisted_valid||!frontend.dirty)return;
 u8 data[ui_persistent_size];auto status=encode_ui_persistent(frontend.settings,{data,sizeof data});
 if(!status){report(status);operator_write_failed=true;return;}
 wchar_t temporary[2048];copy(temporary,operator_path,(wlen(operator_path)+1)*sizeof(wchar_t));
 if(!append(temporary,2048,L".tmp")){operator_write_failed=true;set_message("Native settings temporary path too long; not saved.");return;}
 HANDLE file=CreateFileW(temporary,0x40000000,0,nullptr,2,0x80,nullptr);bool okay=false;
 if(file!=reinterpret_cast<HANDLE>(~usize(0))){DWORD wrote=0;okay=WriteFile(file,data,DWORD(sizeof data),&wrote,nullptr)&&wrote==sizeof data;okay=FlushFileBuffers(file)&&okay;okay=CloseHandle(file)&&okay;}
 if(okay)okay=MoveFileExW(temporary,operator_path,9)!=0;
 if(!okay){DeleteFileW(temporary);operator_write_failed=true;set_message("Native operator state could not be saved. Previous file was not deliberately overwritten in place.");return;}
 frontend.dirty=false;last_saved_mutation=frontend.mutations;
}
void pc_menu_move(int delta){int next=int(pc_menu_selection)+delta;while(next<0)next+=int(pc_menu_count);while(next>=int(pc_menu_count))next-=int(pc_menu_count);pc_menu_selection=unsigned(next);}
void pc_settings_move(int delta){int next=int(pc_settings_selection)+delta;while(next<0)next+=int(pc_settings_count);while(next>=int(pc_settings_count))next-=int(pc_settings_count);pc_settings_selection=unsigned(next);}
void pc_settings_adjust(int delta){
 if(pc_settings_selection<2){unsigned player=pc_settings_selection;for(unsigned step=1;step<=ControllerAssignment::device_limit;++step){unsigned slot=(controller_assignment.xinput_slot[player]+(delta<0?ControllerAssignment::device_limit-step:step))%ControllerAssignment::device_limit;auto status=controller_assignment.assign(player,slot);if(status){sticks[player][0].reset();sticks[player][1].reset();save_settings();set_message("Controller assignment saved; the selected XInput slot is polled for reconnects.");return;}}return;}
 if(pc_settings_selection==2){int engage=int(settings.engage)+delta*500;if(engage<2500||engage>20000)return;settings.engage=u16(engage);settings.release=u16(engage-2000);save_settings();}
 else if(pc_settings_selection==3){integer_scale=!integer_scale;save_settings();}
}
void pc_settings_activate(){if(pc_settings_selection<4){pc_settings_adjust(1);return;}if(pc_settings_selection==4){remap=0;set_message("Rebind 20 keyboard actions; Enter keeps the current key and Escape cancels.");return;}pc_settings_open=false;set_message("Returned to the PC menu.");}
void pc_menu_execute();
void pc_menu_toggle(){pc_menu_open=!pc_menu_open;pc_settings_open=false;if(pc_menu_open){pc_menu_selection=0;paused=true;}else paused=false;reset_clock();InvalidateRect(window,nullptr,0);}
Status frontend_input(const LabInput& in,UiInput& result){
 InputFrame frame;frame.boundary=frontend.ticks*2;
 for(unsigned player=0;player<2;++player){frame.player[player].movement=in.move[player];frame.player[player].shooting=in.fire[player];}
 for(unsigned player=0;player<2;++player){frame.player[player].start=action_start[player]||((controller_buttons&(player?0x200000:0x40000))!=0);frame.player[player].coin=action_coin[player];}
 WidgetPorts ports;auto s=encode_widget(frame,ports);if(!s)return s;
 result={};result.closed=~ports.longword();
 if(focused&&remap<0&&key('9'))result.closed|=0x400000;
 result.back=host_back;host_back=false;return {};
}
void toggle_fullscreen(){if(!fullscreen){GetWindowRect(window,&saved_rect);saved_style=GetWindowLongPtrW(window,-16);MONITORINFO mi{};mi.cbSize=sizeof mi;auto monitor=MonitorFromWindow(window,2);if(!GetMonitorInfoW(monitor,&mi))return;SetWindowLongPtrW(window,-16,saved_style&~i64(0x00cf0000));SetWindowPos(window,nullptr,mi.rcMonitor.left,mi.rcMonitor.top,mi.rcMonitor.right-mi.rcMonitor.left,mi.rcMonitor.bottom-mi.rcMonitor.top,0x24);fullscreen=true;}else{SetWindowLongPtrW(window,-16,saved_style);SetWindowPos(window,nullptr,saved_rect.left,saved_rect.top,saved_rect.right-saved_rect.left,saved_rect.bottom-saved_rect.top,0x24);fullscreen=false;}reset_clock();}
Status redraw(){
 if(!ready)return {};
 if(mode==0){canvas_width=400;canvas_height=260;return frontend.draw({canvas,canvas_width,canvas_height,canvas_width,usize(canvas_width)*canvas_height});}
 if(mode==1){canvas_width=lab_width;canvas_height=lab_height;return game_session.fixture().draw({canvas,canvas_width,canvas_height,canvas_width,usize(canvas_width)*canvas_height});}
 if(mode==2){if(!image_files)return Status::fail("No original image banks");auto& e=pack.entries[image_entries[image_file%image_files]];auto s=browser_images.open(e.bytes);if(!s)return s;if(!browser_images.count)return Status::fail("Original image bank has no images");image_index%=browser_images.count;
    EditorImage image;s=browser_images.image(image_index,image);if(!s)return s;canvas_width=image.width+32;canvas_height=image.height+32;if(canvas_width<160)canvas_width=160;if(canvas_height<120)canvas_height=120;if(canvas_width>2048||canvas_height>2048)return Status::fail("Image too large for browser canvas");
    for(usize i=0;i<usize(canvas_width)*canvas_height;++i)canvas[i]=0x181818;
    return draw_editor_image(browser_images,image_index,{canvas,canvas_width,canvas_height,canvas_width,usize(canvas_width)*canvas_height},16,16);
 }
 if(!background_files)return Status::fail("No original background banks");auto& e=pack.entries[background_entries[bg_file%background_files]];char name[48];copy(name,e.name,sizeof name);usize len=length(name);if(len<4)return Status::fail("Bad BDD name");name[len-1]='B';const auto* layout=pack.find(name);if(!layout)return Status::fail("Background layout missing");auto s=browser_background.open(e.bytes,layout->bytes);if(!s)return s;if(!browser_background.module_count)return Status::fail("Original background bank has no named modules");bg_module%=browser_background.module_count;auto& m=browser_background.modules[bg_module];canvas_width=unsigned(m.right-m.left+1);canvas_height=unsigned(m.bottom-m.top+1);if(canvas_width>2048||canvas_height>2048)return Status::fail("Background too large for browser canvas");zero(canvas,usize(canvas_width)*canvas_height*sizeof(u32));return browser_background.draw_module(bg_module,{canvas,canvas_width,canvas_height,canvas_width,usize(canvas_width)*canvas_height});
}
LabInput sample(){LabInput in;controller_buttons=0;action_start[0]=action_start[1]=false;action_coin[0]=action_coin[1]=false;for(unsigned player=0;player<2;++player){XINPUT_STATE state{};connected[player]=xinput&&xinput(controller_assignment.xinput_slot[player],&state)==0;
 if(connected[player]&&focused){const auto& g=state.Gamepad;auto inverted=[](short v){return i16(v==-32768?32767:-v);};in.move[player]=sticks[player][0].sample(g.sThumbLX,inverted(g.sThumbLY),settings);in.fire[player]=sticks[player][1].sample(g.sThumbRX,inverted(g.sThumbRY),settings);bool start=(g.wButtons&0x10)!=0;action_start[player]=start;action_coin[player]=(g.wButtons&0x20)!=0;if((g.wButtons&(0x1000|0x10))&&mode==0)controller_buttons|=player?0x200000:0x40000;if(start&&!previous_start[player]&&remap<0&&mode==1&&!pc_menu_open){paused=!paused;reset_clock();}previous_start[player]=start;
    if(player==0){const u16 buttons=g.wButtons;if(pc_menu_open){if(pc_settings_open){if((buttons&0x0001)&&!(previous_menu_buttons&0x0001))pc_settings_move(-1);if((buttons&0x0002)&&!(previous_menu_buttons&0x0002))pc_settings_move(1);if((buttons&0x0004)&&!(previous_menu_buttons&0x0004))pc_settings_adjust(-1);if((buttons&0x0008)&&!(previous_menu_buttons&0x0008))pc_settings_adjust(1);if((buttons&0x1000)&&!(previous_menu_buttons&0x1000))pc_settings_activate();if((buttons&0x2000)&&!(previous_menu_buttons&0x2000)){pc_settings_open=false;set_message("Returned to the PC menu.");}}else{if((buttons&0x0001)&&!(previous_menu_buttons&0x0001))pc_menu_move(-1);if((buttons&0x0002)&&!(previous_menu_buttons&0x0002))pc_menu_move(1);if((buttons&0x1000)&&!(previous_menu_buttons&0x1000))pc_menu_execute();if((buttons&0x2000)&&!(previous_menu_buttons&0x2000))pc_menu_toggle();}}previous_menu_buttons=buttons;}
 }else{sticks[player][0].reset();sticks[player][1].reset();previous_start[player]=false;if(player==0)previous_menu_buttons=0;}
 if(focused&&remap<0){const auto* k=keyboard_bindings.keys[player];u8 m=digital_directions(key(k[0]),key(k[1]),key(k[2]),key(k[3])),f=digital_directions(key(k[4]),key(k[5]),key(k[6]),key(k[7]));if(m)in.move[player]=m;if(f)in.fire[player]=f;action_start[player]=action_start[player]||key(k[8]);action_coin[player]=action_coin[player]||key(k[9]);}
 }return in;}
void update(){i64 now;QueryPerformanceCounter(&now);u64 elapsed=now>=previous_counter?u64(now-previous_counter):0;previous_counter=now;auto input=sample();
 if(ready&&focused&&!paused&&!pc_menu_open&&mode<=1&&remap<0){u64 due=0;auto s=(mode==0?frontend_clock:lab_clock).advance(elapsed,due);if(!s){report(s);return;}pending_ticks+=due;unsigned batch=0;while(pending_ticks&&batch++<16){if(mode==0){UiInput ui;s=frontend_input(input,ui);if(s)s=frontend.tick(ui);if(s&&game_session.boundary==0)s=game_session.import_operator_state(frontend.settings);}else s=game_session.advance(session_input(input));if(!s){report(s);return;}--pending_ticks;}}
 else{frontend_clock.phase=lab_clock.phase=0;pending_ticks=0;}
 save_operator();auto s=redraw();if(!s)report(s);InvalidateRect(window,nullptr,0);
}
void paint(){PAINTSTRUCT ps{};HDC dc=BeginPaint(window,&ps);RECT rect{};GetClientRect(window,&rect);FillRect(dc,&rect,black);SetBkMode(dc,1);SetTextColor(dc,0x00eeeeee);
 char text[8192];TextWriter t{text,sizeof text};t.text("SMASH TV | NATIVE BOOT / OPERATOR MILESTONE 0.5 | FULL GAME INCOMPLETE\n");
 t.text("F7 source frontend  F8 operator menu | F1 arena lab  F2 sprites  F3 backgrounds | F9 scale  F10 PC menu  F11 fullscreen\n");
 t.text("F4: rebind 20 keyboard actions | Start 1/2, coin 5/6 | XInput Start/Back: start/coin | P: pause | R: reboot/reset\n");
 if(mode==0){
 t.text(paused||!focused?"PAUSED | ":"RUNNING | ");t.text(frontend.state_name());t.text(" | tick ");t.number(frontend.ticks);
 t.text(" | native settings ");t.text(operator_write_failed?"SAVE FAILED":!operator_write_enabled?"READ-ONLY":frontend.dirty?"pending":"saved");t.text("\n");
 t.text("Source REVISION 5.00. PCB tests NOT translated. L8 reference frame pace 54.70684 Hz; event phases/audio incomplete.\n");
 if(frontend.page==UiPage::Boundary||frontend.page==UiPage::BoardDiagram){t.text("BOUNDARY: ");t.text(frontend.boundary?frontend.boundary:"Unimplemented native service");}
 else t.text("Original menus, settings and first attract segments only. Full credit/start/demo/game flow is not connected.");
 }else if(mode==1){t.text(paused||!focused?"PAUSED | ":"RUNNING | ");t.text("Test clock: 60 synthetic ticks/sec (arcade timing UNVERIFIED). Hulk contact/death/respawn enabled; audio absent.\n");
 auto& lab=game_session.fixture();t.text("Tick ");t.number(lab.stats.ticks);t.text(" | shots ");t.number(lab.stats.shots);t.text(" | kills ");t.number(lab.stats.hulk_kills);t.text(" | hulks ");t.number(lab.stats.hulks);t.text(" | objects ");t.number(lab.objects.count);t.text(" | credits ");t.number(game_session.operator_state.credits);t.text(" | backlog ");t.number(pending_ticks);t.text("\n");
 for(unsigned i=0;i<2;++i){t.text(i?"  P2 ":"P1 ");t.text(connected[i]?"controller connected":"keyboard / controller absent");t.text(" | reserve lives ");t.number(lab.players[i].life.reserves);t.text(lab.players[i].life.countdown?" BUY-IN NOT PORTED":lab.players[i].life.dead?" DEAD":"");t.text(" | score ");u32 score=lab.players[i].score.packed_bcd;for(unsigned d=8;d>0;--d){char a[2]={char('0'+((score>>((d-1)*4))&15)),0};t.text(a);}}
 }else if(mode==2&&image_files){EditorImage im;browser_images.image(image_index,im);t.text(pack.entries[image_entries[image_file%image_files]].name);t.text(" | ");t.text(im.name);t.text(" | image ");t.number(image_index+1);t.text(" / ");t.number(browser_images.count);t.text(" | Tab changes bank, Left/Right changes image; wheel also browses.\n");t.text("Original palette/indexed pixels. Editor visualization, not a gameplay screenshot.");}
 else if(mode==3&&background_files){t.text(pack.entries[background_entries[bg_file%background_files]].name);t.text(" | ");t.text(browser_background.modules[bg_module].name);t.text(" | Tab changes bank, Left/Right changes editor module.\n");t.text("Original BDD/BDB editor reconstruction; compiled arcade-map fidelity is not verified.");}
 t.text("\n");if(remap>=0){t.text("REMAP: press key for ");t.text(binding_display[unsigned(remap)]);t.text(" (Esc cancels; Enter keeps current binding). ");}
 t.text(message);t.text(" | dead zone ");t.number(settings.engage);t.text("/");t.number(settings.release);
 widen(text,wide,8192);RECT label{14,10,rect.right-14,180};DrawTextW(dc,wide,-1,&label,0x10);
 if(ready&&rect.right>40&&rect.bottom>220){int avail_w=rect.right-28,avail_h=rect.bottom-205;int width=avail_w,height=int(i64(avail_w)*canvas_height/canvas_width);if(height>avail_h){height=avail_h;width=int(i64(height)*canvas_width/canvas_height);}if(integer_scale){int scale=avail_w/int(canvas_width);int sy=avail_h/int(canvas_height);if(sy<scale)scale=sy;if(scale>=1){width=int(canvas_width)*scale;height=int(canvas_height)*scale;}}
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=LONG(canvas_width);info.bmiHeader.biHeight=-LONG(canvas_height);info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
    SetStretchBltMode(dc,3);StretchDIBits(dc,(rect.right-width)/2,190+(avail_h-height)/2,width,height,0,0,int(canvas_width),int(canvas_height),canvas,&info,0,0x00cc0020);
 }
 if(pc_menu_open){
    RECT panel{rect.right/2-250,rect.bottom/2-220,rect.right/2+250,rect.bottom/2+220};FillRect(dc,&panel,black);SetTextColor(dc,0x00ffffff);
    RECT title=panel;title.left+=28;title.top+=20;title.right-=28;title.bottom=title.top+34;DrawTextW(dc,pc_settings_open?L"SETTINGS":L"SMASH TV",-1,&title,0x10);
    if(pc_settings_open){
      for(unsigned i=0;i<pc_settings_count;++i){char line[128];TextWriter line_writer{line,sizeof line};line_writer.text(i==pc_settings_selection?"> ":"  ");
        if(i<2){line_writer.text(i?"P2 controller: XInput slot ":"P1 controller: XInput slot ");line_writer.number(controller_assignment.xinput_slot[i]+1);line_writer.text(connected[i]?" (connected)":" (disconnected)");}
        else if(i==2){line_writer.text("Stick dead zone: ");line_writer.number(settings.engage);}
        else if(i==3){line_writer.text("Integer scaling: ");line_writer.text(integer_scale?"on":"off");}
        else if(i==4)line_writer.text("Rebind 20 keyboard actions");else line_writer.text("Back to PC menu");
        widen(line,wide,8192);RECT row=panel;row.left+=36;row.top+=68+int(i)*42;row.right-=24;row.bottom=row.top+30;SetTextColor(dc,i==pc_settings_selection?0x00ffff:0x00dddddd);DrawTextW(dc,wide,-1,&row,0x10);
      }
      SetTextColor(dc,0x00aaaaaa);RECT help=panel;help.left+=28;help.top=panel.bottom-38;help.right-=18;help.bottom=panel.bottom-12;DrawTextW(dc,L"Up/Down choose  Left/Right change  Enter select  F4 remap  Esc back",-1,&help,0x10);
    }else{
      for(unsigned i=0;i<pc_menu_count;++i){char line[96];TextWriter line_writer{line,sizeof line};line_writer.text(i==pc_menu_selection?"> ":"  ");line_writer.text(pc_menu_items[i]);widen(line,wide,8192);RECT row=panel;row.left+=44;row.top+=68+int(i)*42;row.right-=28;row.bottom=row.top+30;SetTextColor(dc,i==pc_menu_selection?0x00ffff:0x00dddddd);DrawTextW(dc,wide,-1,&row,0x10);}
      SetTextColor(dc,0x00aaaaaa);RECT help=panel;help.left+=28;help.top=panel.bottom-38;help.right-=28;help.bottom=panel.bottom-12;DrawTextW(dc,L"Arrow keys / D-pad: select    Enter / A: choose    Esc / B: close",-1,&help,0x10);
    }
 }
 EndPaint(window,&ps);
}
void reset_lab(const UiPersistent* session_settings=nullptr){auto s=game_session.reset(0x13579bdf,active_players,16,true,session_settings);if(!s)report(s);else set_message("GameSession source fixture and native doors; no room progression or audio. F5 tests gates. R resets, not an arcade continue.");reset_clock();}
void pc_menu_execute(){
 switch(pc_menu_selection){
 case 0: pc_menu_toggle(); break;
 case 1: active_players=2;mode=1;pc_menu_open=false;paused=false;reset_lab(&frontend.settings);set_message("Local co-op selected for the two-slot source fixture; retail credit/start flow remains untranslated.");break;
 case 2: set_message("Host unavailable: ENet transport and rollback are not connected; compatibility hello and packet formats are ready.");break;
 case 3: set_message("Join unavailable: ENet transport and rollback are not connected; compatibility hello and packet formats are ready.");break;
 case 4: pc_settings_open=true;pc_settings_selection=0;set_message("PC input/display settings. Controller assignments are limited to two unique XInput slots.");break;
 case 5: pc_menu_open=false;mode=0;paused=true;{auto s=frontend.enter_service();if(!s)report(s);}reset_clock();break;
 case 6: DestroyWindow(window);break;
 default: break;
 }
 InvalidateRect(window,nullptr,0);
}
void browse(int delta){if(mode==2&&image_files){unsigned count=browser_images.count;if(count)image_index=unsigned((int(image_index)+delta+int(count))%int(count));}else if(mode==3&&background_files){unsigned count=browser_background.module_count;if(count)bg_module=unsigned((int(bg_module)+delta+int(count))%int(count));}auto s=redraw();if(!s)report(s);}
LRESULT __stdcall procedure(HWND w,UINT msg,WPARAM keycode,LPARAM extra){
 if(msg==paint_msg){paint();return 0;}
 if(msg==timer_msg){update();return 0;}
 if(msg==activate_msg){focused=keycode!=0;if(!focused){paused=true;for(auto& row:sticks)for(auto& stick:row)stick.reset();}reset_clock();return 0;}
 if(msg==mousewheel_msg){int delta=signed16(u16(keycode>>16));if(mode>=2)browse(delta>0?-1:1);return 0;}
 if(msg==keydown_msg){unsigned keyvalue=unsigned(keycode);if(extra&(i64(1)<<30))return 0;
    if(remap>=0){if(keyvalue==0x1b){remap=-1;set_message("Key remapping cancelled; already chosen keys retained.");save_settings();}else{if(keyvalue!=13){bool reserved=(keyvalue>=0x70&&keyvalue<=0x7b)||keyvalue=='P'||keyvalue=='R'||keyvalue=='N'||keyvalue==9||keyvalue==0xbb||keyvalue==0xbd;if(reserved){set_message("Reserved lab control key; choose another binding.");InvalidateRect(w,nullptr,0);return 0;}auto bind_status=keyboard_bindings.assign(unsigned(remap)/10,InputAction(unsigned(remap)%10),u16(keyvalue));if(!bind_status){set_message(bind_status.message?bind_status.message:"Keyboard binding rejected.");InvalidateRect(w,nullptr,0);return 0;}}++remap;if(remap==20){remap=-1;save_settings();set_message("Twenty keyboard actions saved in LocalAppData\\SmashTVNativeLab\\settings.ini.");}}InvalidateRect(w,nullptr,0);return 0;}
      if(pc_menu_open){if(pc_settings_open){if(keyvalue==0x1b){pc_settings_open=false;set_message("Returned to the PC menu.");}else if(keyvalue==0x26||keyvalue=='W')pc_settings_move(-1);else if(keyvalue==0x28||keyvalue=='S')pc_settings_move(1);else if(keyvalue==0x25||keyvalue=='A')pc_settings_adjust(-1);else if(keyvalue==0x27||keyvalue=='D')pc_settings_adjust(1);else if(keyvalue==13||keyvalue==32)pc_settings_activate();else if(keyvalue==0x73){remap=0;set_message("Rebind 20 keyboard actions; Enter keeps the current key and Escape cancels.");}InvalidateRect(w,nullptr,0);return 0;}if(keyvalue==0x1b){pc_menu_toggle();return 0;}if(keyvalue==0x26||keyvalue=='W'){pc_menu_move(-1);InvalidateRect(w,nullptr,0);return 0;}if(keyvalue==0x28||keyvalue=='S'){pc_menu_move(1);InvalidateRect(w,nullptr,0);return 0;}if(keyvalue==13||keyvalue==32){pc_menu_execute();return 0;}return 0;}
    if(keyvalue==0x1b){pc_menu_toggle();return 0;}
    if(keyvalue>=0x70&&keyvalue<=0x72){mode=keyvalue-0x6f;reset_clock();}
    else if(keyvalue==0x73){remap=0;paused=true;reset_clock();}
    else if(keyvalue==0x74&&ready&&mode==1){for(unsigned g=1;g<=3;++g){auto gate_status=game_session.fixture().open_source_gate(g,12);if(!gate_status){report(gate_status);break;}}set_message("Diagnostic gate request only; original respawns also use this native door service.");}
    else if(keyvalue==0x75){active_players=active_players==1?2:1;reset_lab();}
    else if(keyvalue==0x76){mode=0;paused=false;reset_clock();}
    else if(keyvalue==0x77&&ready){mode=0;paused=false;auto st=frontend.enter_service();if(!st)report(st);reset_clock();}
    else if(keyvalue==8&&mode==0){host_back=true;}
    else if(keyvalue==0x78){integer_scale=!integer_scale;save_settings();}
    else if(keyvalue==0x7a)toggle_fullscreen();
    else if(keyvalue==0x79)pc_menu_toggle();
    else if(keyvalue=='P'){paused=!paused;reset_clock();}
    else if(keyvalue=='R'&&ready){if(mode==0){save_operator();auto st=start_frontend();if(!st)report(st);reset_clock();}else reset_lab();}
    else if(keyvalue=='N'&&ready&&mode<=1){paused=true;auto in=sample();Status s;if(mode==0){UiInput ui;s=frontend_input(in,ui);if(s)s=frontend.tick(ui);}else s=game_session.advance(session_input(in));if(!s)report(s);reset_clock();}
    else if(keyvalue==9){if(mode==2&&image_files){image_file=(image_file+1)%image_files;image_index=0;}if(mode==3&&background_files){bg_file=(bg_file+1)%background_files;bg_module=0;}}
    else if(mode>=2&&(keyvalue==0x25||keyvalue==0x27))browse(keyvalue==0x25?-1:1);
    else if(keyvalue==0xbb||keyvalue==0xbd){int next=int(settings.engage)+(keyvalue==0xbb?500:-500);if(next>=2500&&next<=20000){settings.engage=u16(next);settings.release=u16(next-2000);save_settings();}}
    auto s=redraw();if(!s)report(s);InvalidateRect(w,nullptr,0);return 0;
 }
 if(msg==destroy_msg){save_operator();KillTimer(w,1);PostQuitMessage(0);return 0;}
 return DefWindowProcW(w,msg,keycode,extra);
}
Status load_source(u8*& data){wchar_t path[2048];DWORD n=GetModuleFileNameW(nullptr,path,2048);if(!n||n>=2048)return Status::fail("Executable path unavailable");while(n&&path[n-1]!=L'\\'&&path[n-1]!=L'/')--n;path[n]=0;if(!append(path,2048,L"data\\source.stva"))return Status::fail("Data path too long");
 HANDLE f=CreateFileW(path,0x80000000,1,nullptr,3,0x80,nullptr);if(f==reinterpret_cast<HANDLE>(~usize(0)))return Status::fail("Missing data\\source.stva beside SmashTV.exe. Extract the entire Windows ZIP, not only its EXE",GetLastError());i64 size=0;
 if(!GetFileSizeEx(f,&size)||size<=0||size>64*1024*1024){CloseHandle(f);return Status::fail("Source pack size outside 1..64 MiB");}
 data=static_cast<u8*>(VirtualAlloc(nullptr,usize(size),0x3000,4));if(!data){CloseHandle(f);return Status::fail("Source pack allocation failed");}
 usize pos=0;while(pos<usize(size)){DWORD got=0;if(!ReadFile(f,data+pos,DWORD(usize(size)-pos),&got,nullptr)||!got){CloseHandle(f);return Status::fail("Source pack read failed");}pos+=got;}CloseHandle(f);
 auto s=pack.open({data,usize(size)});if(!s)return s;
 for(unsigned i=0;i<pack.count;++i){auto name=pack.entries[i].name;usize length_name=length(name);if(length_name>=4&&name_equal(name+length_name-4,".IMG"))image_entries[image_files++]=i;else if(length_name>=4&&name_equal(name+length_name-4,".BDD"))background_entries[background_files++]=i;}
 s=game_session.load(pack,GameProfile::source_fixture());if(!s)return s;return frontend.load(pack);
}
int application(){instance=GetModuleHandleW(nullptr);black=CreateSolidBrush(0x101010);WNDCLASSEXW wc{};wc.cbSize=sizeof wc;wc.lpfnWndProc=procedure;wc.hInstance=instance;wc.hbrBackground=black;wc.hCursor=LoadCursorW(nullptr,reinterpret_cast<LPCWSTR>(usize(32512)));wc.lpszClassName=L"SmashTVNativeSourceLab";
 if(!RegisterClassExW(&wc))return 1;window=CreateWindowExW(0,wc.lpszClassName,L"Smash TV - native source startup / operator 0.5 - full game incomplete",0x00cf0000,80,50,1080,940,nullptr,nullptr,instance,nullptr);if(!window)return 2;
 i64 frequency;if(!QueryPerformanceFrequency(&frequency)||frequency<=0)return 3;
 auto s=frontend_clock.configure(u64(frequency),L8ReferenceTiming::pixel_clock_hz,L8ReferenceTiming::pixels_per_frame);if(!s)return 4;
 s=lab_clock.configure(u64(frequency),60,1);if(!s)return 4;
 load_settings();u8* data=nullptr;s=load_source(data);if(s){ready=true;reset_lab();s=start_frontend();if(s)s=redraw();}if(!s)report(s);
 xinput_dll=LoadLibraryExW(L"xinput1_4.dll",nullptr,0x800);if(xinput_dll){auto ptr=GetProcAddress(xinput_dll,"XInputGetState");copy(&xinput,&ptr,sizeof ptr);}
 ShowWindow(window,5);UpdateWindow(window);reset_clock();SetTimer(window,1,8,nullptr);MSG msg{};BOOL result;
 while((result=GetMessageW(&msg,nullptr,0,0))>0){TranslateMessage(&msg);DispatchMessageW(&msg);}
 if(data)VirtualFree(data,0,0x8000);if(xinput_dll)FreeLibrary(xinput_dll);if(black)DeleteObject(black);return result<0?5:int(msg.wParam);
}
}
#if defined(STV_FREESTANDING)
extern "C" void* memcpy(void* dst,const void* src,usize n){auto d=static_cast<volatile u8*>(dst);auto s=static_cast<const volatile u8*>(src);for(usize i=0;i<n;++i)d[i]=s[i];return dst;}
extern "C" void* memset(void* dst,int v,usize n){auto d=static_cast<volatile u8*>(dst);for(usize i=0;i<n;++i)d[i]=u8(v);return dst;}
extern "C" void* memmove(void* dst,const void* src,usize n){auto d=static_cast<volatile u8*>(dst);auto s=static_cast<const volatile u8*>(src);if(reinterpret_cast<usize>(dst)<reinterpret_cast<usize>(src)){for(usize i=0;i<n;++i)d[i]=s[i];}else{while(n){--n;d[n]=s[n];}}return dst;}
extern "C" void mainCRTStartup(){ExitProcess(unsigned(application()));}
#else
int __stdcall wWinMain(HINSTANCE,HINSTANCE,wchar_t*,int){return application();}
#endif
