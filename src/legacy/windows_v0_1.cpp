#include "stv/win32_min.hpp"
#include "stv/assets.hpp"
#include "stv/game_rules.hpp"
#include "stv/sound_commands.hpp"
#include "stv/input.hpp"
using namespace stv;
using namespace win;
namespace {
constexpr DWORD mem_commit_reserve=0x3000,mem_release=0x8000,page_rw=4;
constexpr UINT wm_destroy=2,wm_paint=15,wm_command=0x111,wm_timer=0x113,wm_keydown=0x100,wm_dropfiles=0x233;
constexpr UINT import_button=101,checks_button=102,sound_button=103,difficulty_button=104;
HWND main_window{};HINSTANCE instance{};HBRUSH background{};HANDLE xinput_library{};XINPUT_GET_STATE xinput_get_state{};
char status_text[16384]="No ROMs imported. This executable is a development checkpoint, NOT A PLAYABLE GAME.";
char checks_text[1024]="Native smoke checks have not been run in this Windows process.";
char trace_text[2048]="No sound-command fixture run. There is no audible game audio implementation.";
char rules_text[2048]{};wchar_t display_text[32768]{},error_text[32768]{};
unsigned difficulty_index=4;DigitalStick sticks[2][2];StickSettings stick_settings;PlayerInput inputs[2]{};bool connected[2]{};
ZipArchive zip_archive; // Large parser storage is static, never on the Windows stack.
HANDLE invalid_handle(){return reinterpret_cast<HANDLE>(~usize(0));}
struct Allocation {
 u8* data{};usize size{};
 bool allocate(usize n){data=static_cast<u8*>(VirtualAlloc(nullptr,n? n:1,mem_commit_reserve,page_rw));size=n;return data!=nullptr;}
 ~Allocation(){if(data)VirtualFree(data,0,mem_release);}
 Allocation()=default;Allocation(const Allocation&)=delete;Allocation& operator=(const Allocation&)=delete;
 Buffer buffer(){return {data,size};}Bytes bytes()const{return {data,size};}
};
usize wlength(LPCWSTR s){usize n=0;while(s[n])++n;return n;}
void widen(const char* src,wchar_t* dst,usize cap){usize i=0;for(;src[i]&&i+1<cap;++i)dst[i]=wchar_t(u8(src[i]));if(cap)dst[i]=0;}
void refresh(){InvalidateRect(main_window,nullptr,0);}
void error(Status s){TextWriter text{status_text,sizeof status_text};text.text("ERROR: ");text.text(s.message?s.message:"Unspecified failure");text.text("\nOffset/index/error code: ");text.number(s.offset);widen(status_text,error_text,sizeof error_text/sizeof *error_text);MessageBoxW(main_window,error_text,L"Smash TV checkpoint - operation did not complete",0x10);refresh();}
Status read_file(LPCWSTR path,Allocation& out){HANDLE file=CreateFileW(path,0x80000000,1,nullptr,3,0x80,nullptr);if(file==invalid_handle())return Status::fail("Cannot open selected file",GetLastError());i64 size=0;if(!GetFileSizeEx(file,&size)||size<0||u64(size)>max_archive_size){CloseHandle(file);return Status::fail("Selected input exceeds 64 MiB or its size could not be read");}if(!out.allocate(usize(size))){CloseHandle(file);return Status::fail("Could not allocate input buffer");}usize total=0;while(total<out.size){DWORD got=0;if(!ReadFile(file,out.data+total,DWORD(out.size-total),&got,nullptr)||!got){CloseHandle(file);return Status::fail("Read failed before end of selected file",GetLastError());}total+=got;}CloseHandle(file);return {};}
Status write_new_file(LPCWSTR path,Bytes data){HANDLE file=CreateFileW(path,0x40000000,0,nullptr,1,0x80,nullptr);if(file==invalid_handle())return Status::fail("Cannot create output; existing files are never overwritten",GetLastError());usize total=0;while(total<data.size){DWORD written=0;if(!WriteFile(file,data.data+total,DWORD(data.size-total),&written,nullptr)||!written){DWORD code=GetLastError();CloseHandle(file);DeleteFileW(path);return Status::fail("Output write failed; partial new file removed",code);}total+=written;}if(!FlushFileBuffers(file)){DWORD code=GetLastError();CloseHandle(file);DeleteFileW(path);return Status::fail("Output flush failed; partial new file removed",code);}CloseHandle(file);return {};}
void show_missing(const ImportReport& r){TextWriter t{status_text,sizeof status_text};t.text("Import rejected: no compatible complete reference set was found.\nThis importer accepts MAME smashtv L8 reference data only. Source compatibility is UNVERIFIED.\n\n");for(unsigned i=0;i<reference_rom_count;++i)if(r.missing_mask&(1u<<i)){t.text(l8_reference_roms[i].name);t.text("\n  bytes: ");t.number(l8_reference_roms[i].size);t.text("  SHA1: ");t.text(l8_reference_roms[i].sha1_text);t.text("\n");}widen(status_text,error_text,sizeof error_text/sizeof *error_text);MessageBoxW(main_window,error_text,L"Missing or incompatible reference ROMs",0x30);refresh();}
void import_zip(LPCWSTR path){
 Allocation input;auto s=read_file(path,input);if(!s){error(s);return;}s=zip_archive.open(input.bytes());if(!s){error(s);return;}
 Allocation scratch,package,manifest;if(!scratch.allocate(rom_scratch_size)||!package.allocate(asset_package_size)||!manifest.allocate(32768)){error(Status::fail("Could not allocate importer working buffers"));return;}
 ImportReport report;s=scan_roms(zip_archive,l8_reference_roms,scratch.buffer(),report);if(!s){if(s.message&&text_equal(s.message,"Required reference ROMs are missing or have incompatible hashes"))show_missing(report);else error(s);return;}
 s=make_package(zip_archive,l8_reference_roms,retail_l8_reference_id,report,scratch.buffer(),package.buffer());if(!s){error(s);return;}
 s=verify_package(package.bytes(),l8_reference_roms,retail_l8_reference_id,scratch.buffer());if(!s){error(s);return;}
 TextWriter writer{reinterpret_cast<char*>(manifest.data),manifest.size};s=write_manifest(zip_archive,l8_reference_roms,retail_l8_reference_id,report,package.bytes(),writer);if(!s){error(s);return;}
 static wchar_t output[2048];static wchar_t manifest_path[2048];const wchar_t default_name[]=L"reference-l8.stvdata";copy(output,default_name,sizeof default_name);
 OPENFILENAMEW dialog{};dialog.lStructSize=sizeof dialog;dialog.hwndOwner=main_window;dialog.lpstrFile=output;dialog.nMaxFile=2048;dialog.lpstrFilter=L"Native reference data (*.stvdata)\0*.stvdata\0\0";dialog.lpstrDefExt=L"stvdata";dialog.lpstrTitle=L"Save NEW reference package (not playable game data yet)";dialog.Flags=0x00000808;
 if(!GetSaveFileNameW(&dialog))return;
 usize len=wlength(output);const wchar_t suffix[]=L".manifest.json";if(len+sizeof(suffix)/sizeof(*suffix)>2048){error(Status::fail("Output path is too long for manifest suffix"));return;}
 copy(manifest_path,output,len*sizeof(wchar_t));copy(manifest_path+len,suffix,sizeof suffix);
 s=write_new_file(output,package.bytes());if(!s){error(s);return;}
 s=write_new_file(manifest_path,{manifest.data,writer.size});if(!s){DeleteFileW(output);error(s);return;}
 TextWriter text{status_text,sizeof status_text};text.text("SUCCESS: all 14 reference inputs matched size, CRC32 and SHA1.\nSaved a verified STVDATA-1 package plus provenance manifest.\nPackage SHA256: ");text.digest(sha256(package.bytes()).bytes,32);text.text("\nThis is reference data only. Source/retail compatibility is unverified; gameplay is still absent.");refresh();
}
void select_zip(){static wchar_t path[2048]{};path[0]=0;OPENFILENAMEW dialog{};dialog.lStructSize=sizeof dialog;dialog.hwndOwner=main_window;dialog.lpstrFile=path;dialog.nMaxFile=2048;dialog.lpstrFilter=L"User-supplied ROM ZIP (*.zip)\0*.zip\0All files (*.*)\0*.*\0\0";dialog.lpstrTitle=L"Select a lawfully supplied Smash TV reference ROM ZIP";dialog.Flags=0x00001808;if(GetOpenFileNameW(&dialog))import_zip(path);}
void compute_rules(){Difficulty d;SpawnCaps caps;PaletteCycleRequest effect;auto s=set_difficulty(difficulty_index,d);if(s)s=set_maximums(2,1,1,d,caps,effect);TextWriter t{rules_text,sizeof rules_text};t.text("Source-derived native fixture: SETDIFF + SETMAX, circuit=2, wave=1, player status=1\n");if(!s){t.text(s.message);return;}t.text("Difficulty ");t.number(difficulty_index);t.text(" | Hulk cap ");t.number(caps.max_hulk);t.text(" | Robot cap ");t.number(caps.max_robot);t.text(" | Tank cap ");t.number(caps.max_tank);t.text(" | Gal cap ");t.number(caps.max_gal);t.text("\nThese are isolated original rule translations, NOT a running room or progression test.");}
void run_checks(){unsigned passed=0,total=0;auto check=[&](bool ok){++total;if(ok)++passed;};
 const u8 abc[]={'a','b','c'};char hash[65];hex(sha256({abc,3}).bytes,32,hash);check(text_equal(hash,"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));check(add32(~0u,1).flags.c&&add32(~0u,1).value==0);
 u8 memory[8]{};u32 field=0;check(bool(write_field({memory,8},{7},32,0xdeadbeef)));check(bool(read_field({memory,8},{7},32,false,field))&&field==0xdeadbeef);check(!read_field({memory,8},{63},32,false,field));
 Difficulty d;check(bool(set_difficulty(4,d))&&d.cobra_kill==3400);SpawnCaps caps;PaletteCycleRequest e;check(bool(set_maximums(2,1,1,d,caps,e))&&caps.max_hulk==10&&caps.max_tank==3);HulkAge h{1,2,100};check(advance_hulk_age(h)&&h.level==3&&h.age==100);
 u8 compressed[]={1,3,0,0xfc,0xff,'a','b','c'},plain[3]{};usize n=0;check(bool(inflate_raw({compressed,8},{plain,3},n))&&n==3&&equal(plain,abc,3));
 SoundCommands sound;SoundWrite writes[16];SoundTrace trace{writes,16};bool accepted;check(bool(sound.load(sound_shot,4,9,trace,accepted))&&accepted&&trace.count==2&&writes[0].bus_value==0xfdc4);for(unsigned i=0;i<5;++i)check(bool(sound.process_tick(trace)));check(sound.channels[0].priority==0&&trace.count==2);
 TextWriter t{checks_text,sizeof checks_text};t.text(passed==total?"PASS: ":"FAIL: ");t.number(passed);t.text("/");t.number(total);t.text(" native smoke checks executed in THIS process.\nThis is not MAME differential, original-asset, audio, controller or full-game validation.");refresh();}
void run_sound_fixture(){SoundCommands sound;SoundWrite writes[128];SoundTrace trace{writes,128};bool accepted;auto s=sound.load(sound_shot,4,9,trace,accepted);for(unsigned i=0;i<5&&s;++i)s=sound.process_tick(trace);if(s)s=sound.load(sound_rifle,4,0,trace,accepted);if(!s){error(s);return;}TextWriter t{trace_text,sizeof trace_text};t.text("Native SOUNDS command fixture, no audio synthesis:\n");for(usize i=0;i<trace.count;++i){t.text("tick=");t.number(writes[i].tick);t.text(" channel=");t.number(writes[i].channel);t.text(" bus decimal=");t.number(writes[i].bus_value);t.text(i%2?"\n":"   ");}t.text("CPU pulse widths, sound-board execution, music and speech remain unimplemented.");refresh();}
bool key(int code){return (GetAsyncKeyState(code)&0x8000)!=0;}
void sample_input(){bool focused=GetForegroundWindow()==main_window;for(unsigned player=0;player<2;++player){inputs[player]={};XINPUT_STATE state{};connected[player]=xinput_get_state&&xinput_get_state(player,&state)==0;if(connected[player]&&focused){auto& g=state.Gamepad;auto inv=[](short value){return short(value==-32768?32767:-value);};inputs[player].movement=sticks[player][0].sample(g.sThumbLX,inv(g.sThumbLY),stick_settings);inputs[player].shooting=sticks[player][1].sample(g.sThumbRX,inv(g.sThumbRY),stick_settings);inputs[player].start=(g.wButtons&0x10)!=0;inputs[player].coin=(g.wButtons&0x20)!=0;}else{sticks[player][0].reset();sticks[player][1].reset();}}
 if(focused){u8 move=digital_directions(key('W'),key('S'),key('A'),key('D')),shoot=digital_directions(key(0x26),key(0x28),key(0x25),key(0x27));if(move)inputs[0].movement=move;if(shoot)inputs[0].shooting=shoot;}refresh();}
void paint(){PAINTSTRUCT ps{};HDC dc=BeginPaint(main_window,&ps);RECT client{};GetClientRect(main_window,&client);FillRect(dc,&client,background);SetBkMode(dc,1);SetTextColor(dc,0x00eeeeee);
 static char assembled[32768];TextWriter text{assembled,sizeof assembled};text.text("SMASH TV | NATIVE DEVELOPMENT CHECKPOINT 0.1\nNOT PLAYABLE - no original graphics, full gameplay or audible game audio\n\n");text.text("This PE executable contains native C++ routines, data import and diagnostics.\nIt does not launch or embed MAME, another emulator, Python or a game CPU interpreter.\n\n");text.text(status_text);text.text("\n\n");text.text(checks_text);text.text("\n\n");text.text(rules_text);text.text("\n\n");text.text(trace_text);text.text("\n\nINPUT DIAGNOSTIC (not game controls yet; stick XInput slots 0 and 1)\n");for(unsigned p=0;p<2;++p){text.text("P");text.number(p+1);text.text(connected[p]?" controller CONNECTED":" controller not connected");text.text(" | movement bits ");text.number(inputs[p].movement);text.text(" | shooting bits ");text.number(inputs[p].shooting);text.text(" | start ");text.number(inputs[p].start);text.text("\n");}text.text("Direction bits: up=1, down=2, left=4, right=8. Keyboard P1: WASD + arrows.\nInactive windows clear diagnostic input. ESC exits. Import also accepts one dragged ZIP.\nThe 50 ms UI refresh timer is NOT a selected arcade simulation rate.\n\nSource commit, source/retail match, original asset decode, process scheduling and sound-driver\ntranslation are not complete. See PORT_STATUS.md for exact blockers and next actions.");
 widen(assembled,display_text,sizeof display_text/sizeof *display_text);RECT rect{24,85,client.right-24,client.bottom-18};DrawTextW(dc,display_text,-1,&rect,0x10);EndPaint(main_window,&ps);}
LRESULT __stdcall window_proc(HWND window,UINT message,WPARAM w,LPARAM l){
 if(message==wm_paint){paint();return 0;}
 if(message==wm_command){switch(unsigned(w&0xffff)){case import_button:select_zip();break;case checks_button:run_checks();break;case sound_button:run_sound_fixture();break;case difficulty_button:difficulty_index=(difficulty_index+1)%10;compute_rules();refresh();break;}return 0;}
 if(message==wm_timer){sample_input();return 0;}
 if(message==wm_keydown&&w==0x1b){DestroyWindow(window);return 0;}
 if(message==wm_dropfiles){auto drop=reinterpret_cast<HDROP>(w);static wchar_t path[2048];UINT files=DragQueryFileW(drop,0xffffffff,nullptr,0);UINT len=files==1?DragQueryFileW(drop,0,nullptr,0):0;if(files==1&&len<2048&&DragQueryFileW(drop,0,path,2048)){DragFinish(drop);import_zip(path);}else{DragFinish(drop);error(Status::fail("Drag exactly one ZIP with a supported path length"));}return 0;}
 if(message==wm_destroy){KillTimer(window,1);PostQuitMessage(0);return 0;}
 return DefWindowProcW(window,message,w,l);
}
void button(const wchar_t* title,UINT id,int x,int width){CreateWindowExW(0,L"BUTTON",title,0x50010000,x,22,width,38,main_window,reinterpret_cast<HMENU>(usize(id)),instance,nullptr);}
int application(){instance=GetModuleHandleW(nullptr);background=CreateSolidBrush(0x00222222);WNDCLASSEXW cls{};cls.cbSize=sizeof cls;cls.lpfnWndProc=window_proc;cls.hInstance=instance;cls.hCursor=LoadCursorW(nullptr,reinterpret_cast<LPCWSTR>(usize(32512)));cls.hbrBackground=background;cls.lpszClassName=L"SmashTVNativeCheckpoint";
 if(!RegisterClassExW(&cls))return 1;
 main_window=CreateWindowExW(0,cls.lpszClassName,L"Smash TV - native development checkpoint - NOT PLAYABLE",0x00cf0000,0x100,0x60,1100,900,nullptr,nullptr,instance,nullptr);if(!main_window)return 2;
 xinput_library=LoadLibraryExW(L"xinput1_4.dll",nullptr,0x00000800);if(xinput_library){auto procedure=GetProcAddress(xinput_library,"XInputGetState");static_assert(sizeof procedure==sizeof xinput_get_state);copy(&xinput_get_state,&procedure,sizeof procedure);}
 button(L"Import reference ZIP",import_button,24,225);button(L"Run native smoke checks",checks_button,264,240);button(L"Trace sound commands",sound_button,519,235);button(L"Next difficulty",difficulty_button,769,190);
 compute_rules();DragAcceptFiles(main_window,1);ShowWindow(main_window,5);UpdateWindow(main_window);SetTimer(main_window,1,50,nullptr);
 MSG msg{};BOOL result;while((result=GetMessageW(&msg,nullptr,0,0))>0){TranslateMessage(&msg);DispatchMessageW(&msg);}if(xinput_library)FreeLibrary(xinput_library);if(background)DeleteObject(background);return result<0?3:int(msg.wParam);
}
}
#if defined(STV_FREESTANDING)
// Implement compiler-emitted memory helpers without a redistributable C runtime.
extern "C" void* memcpy(void* dst,const void* src,usize n){volatile u8* d=static_cast<u8*>(dst);const volatile u8* s=static_cast<const u8*>(src);for(usize i=0;i<n;++i)d[i]=s[i];return dst;}
extern "C" void* memset(void* dst,int value,usize n){volatile u8* d=static_cast<u8*>(dst);for(usize i=0;i<n;++i)d[i]=u8(value);return dst;}
extern "C" void* memmove(void* dst,const void* src,usize n){volatile u8* d=static_cast<u8*>(dst);const volatile u8* s=static_cast<const u8*>(src);if(reinterpret_cast<usize>(dst)<reinterpret_cast<usize>(src)){for(usize i=0;i<n;++i)d[i]=s[i];}else{for(usize i=n;i>0;--i)d[i-1]=s[i-1];}return dst;}
extern "C" void mainCRTStartup(){ExitProcess(unsigned(application()));}
#else
int __stdcall wWinMain(HINSTANCE,HINSTANCE,wchar_t*,int){return application();}
#endif
