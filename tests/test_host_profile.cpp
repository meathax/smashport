#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <filesystem>
#include <cstdio>
#define wWinMain unused_host_entry
// The matrix compiles both host policies in either build configuration.
// Compile the real menu with that same policy instead of inheriting the
// enclosing cache\'s shared frontend-library policy.
#include "../src/host/pc_menu.cpp"
#include "../src/host/win_main.cpp"
#undef wWinMain
static int checks=0,failures=0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; std::printf("FAIL line %d: %s\n",__LINE__,#x); } } while(false)
int main(int argc,char** argv) {
    if(argc!=2) return 2;
    constexpr bool player=STV_PLAYER_RELEASE!=0;
    g_data_dir=std::filesystem::path(argv[1]).wstring();CreateDirectoryW(g_data_dir.c_str(),nullptr);
    SetEnvironmentVariableW(L"STV_PROFILE",nullptr);DeleteFileW(profile_path().c_str());
    CHECK(load_profile()); // Fresh installs default to widescreen in both host policies.
    for(const auto* setting : {"reference\n","enhanced\n"}) {
        FILE* f=nullptr;_wfopen_s(&f,profile_path().c_str(),L"w");CHECK(f!=nullptr);
        if(f) { std::fputs(setting,f);std::fclose(f); }
        const bool requested=setting[0]=='e';
        CHECK(load_profile()==requested);
        SetEnvironmentVariableW(L"STV_PROFILE",L"reference");CHECK(!load_profile());
        SetEnvironmentVariableW(L"STV_PROFILE",L"enhanced");CHECK(load_profile());
        SetEnvironmentVariableW(L"STV_PROFILE",nullptr);
    }
    SetEnvironmentVariableW(L"STV_PROFILE",L"reference");g_enhanced=load_profile();
    CHECK(!g_enhanced);
    CHECK((cmos_path().find(L"16x9")!=std::wstring::npos)==false);
    // Inspect the real host context and the real Video page; row counts are not
    // policy. A shipping host never offers profile selection, even when its test
    // links the development frontend library in a combined developer build.
    using namespace stv::host;
    const auto context=menu_context();
    CHECK(!context.development_profile_chooser);
    PcMenu menu; menu.open(false);
    CHECK(menu.rows(context).size()==3);
    menu.handle(PcMenuAction::Down,context);
    CHECK(menu.selected_item()==PcMenuItem::Options);
    menu.handle(PcMenuAction::Accept,context);
    CHECK(menu.page()==PcMenuPage::Options);
    for(int n=0;n<16 && menu.selected_item()!=PcMenuItem::Video;++n)
        menu.handle(PcMenuAction::Down,context);
    CHECK(menu.selected_item()==PcMenuItem::Video);
    menu.handle(PcMenuAction::Accept,context);
    CHECK(menu.page()==PcMenuPage::Video);
    bool has_profile=false,profile_enabled=false;
    for(const auto& row:menu.rows(context)) if(row.item==PcMenuItem::PlayfieldProfile) {
        has_profile=true; profile_enabled=row.enabled;
    }
    CHECK(!has_profile && !profile_enabled);
    g_net.on=true;
    const auto online_context=menu_context();
    CHECK(online_context.online);
    has_profile=profile_enabled=false;
    for(const auto& row:menu.rows(online_context)) if(row.item==PcMenuItem::PlayfieldProfile) {
        has_profile=true; profile_enabled=row.enabled;
    }
    CHECK(!has_profile && !profile_enabled);
    g_net.on=false;
    g_launch_mode=0; CHECK(!load_profile());
    g_enhanced=false; const auto classic_cmos=cmos_path();
    g_launch_mode=1; CHECK(load_profile());
    g_enhanced=true; CHECK(cmos_path()!=classic_cmos);
    g_launch_mode=-1;
    g_mode_menu=g_menu=g_paused=true; g_mode_selection=1;
    handle_menu_key(nullptr,VK_UP); CHECK(g_mode_selection==0);
    handle_menu_key(nullptr,VK_ESCAPE); CHECK(g_mode_menu && g_paused);
    g_mode_menu=false;
    std::printf("profile=%s: %d checks, %d failures\n",player ? "dual-mode-player" : "development",checks,failures);
    return failures ? 1 : 0;
}
