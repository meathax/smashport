#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "stv/host/menu_draw_win.hpp"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

int main(int argc,char** argv) {
    int checks=0,failures=0;
    auto check=[&](bool ok){++checks;if(!ok)++failures;};
    const std::filesystem::path out=argc>1?argv[1]:"menu-render";
    std::filesystem::create_directories(out);
    for(auto [w,h]:{std::pair{320,240},{533,256},{640,480},{1280,720},{1920,1080},
                    {2000,1123},{3840,2160}}) {
        const auto layout=stv::host::arcade_title_layout(w,h);
        const int aw=layout.artwork.right-layout.artwork.left;
        const int ah=layout.artwork.bottom-layout.artwork.top;
        check(aw>0&&ah>0);
        check(layout.artwork.left>=layout.artwork_pane.left&&layout.artwork.top>=layout.artwork_pane.top);
        check(layout.artwork.right<=layout.artwork_pane.right&&layout.artwork.bottom<=layout.artwork_pane.bottom);
        check(std::abs((layout.artwork.left+layout.artwork.right)-
                       (layout.artwork_pane.left+layout.artwork_pane.right))<=1);
        check(std::abs((layout.artwork.top+layout.artwork.bottom)-
                       (layout.artwork_pane.top+layout.artwork_pane.bottom))<=1);
        check(std::abs(aw*256-ah*410)<=410);
        check(layout.artwork_pane.top==layout.controls.top &&
              layout.artwork_pane.bottom==layout.controls.bottom);
        check(layout.artwork_pane.right<=layout.controls.left);
        BITMAPINFO bi{};bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth=w;bi.bmiHeader.biHeight=-h;bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;
        void* pixels=nullptr;HDC dc=CreateCompatibleDC(nullptr);
        HBITMAP bitmap=CreateDIBSection(dc,&bi,DIB_RGB_COLORS,&pixels,nullptr,0);
        if(!dc||!bitmap||!pixels)return 2;
        auto old=SelectObject(dc,bitmap);
        std::vector<std::string> lines{"KEYBOARD CONTROLS", "ESC: KEEP KEY / BACKSPACE: DEFAULTS"};
        for(int i=0;i<20;++i)lines.push_back(std::string(i==19?"> ":"  ")+"PLAYER ACTION "+std::to_string(i+1)+" - SPACE");
        const auto audit=stv::host::draw_pc_menu(dc,w,h,lines);
        check(audit.selection_visible);check(audit.panel.left>=0&&audit.panel.top>=0);
        check(audit.panel.right<=w&&audit.panel.bottom<=h);
        check(audit.visible_lines<=audit.total_lines);
        const auto title_fallback=stv::host::draw_pc_menu(dc,w,h,
            {"SMASH T.V.","[ MAIN MENU ]","> GAME  - < CUSTOM WIDESCREEN 16:9 >",
             "START","MULTIPLAYER","OPTIONS","EXIT","ARROWS / D-PAD   ENTER / A TO SELECT"},
            nullptr,&layout.controls);
        check(EqualRect(&title_fallback.panel,&layout.controls));
        check(title_fallback.panel.left==layout.controls.left);
        check(title_fallback.panel.left>layout.artwork_pane.right);
        for (const auto& help : {"OPEN A ROOM ON YOUR LOCAL NETWORK",
                                 "LEFT / RIGHT ADJUSTS / SETTINGS SAVE AUTOMATICALLY"}) {
            auto menu=stv::host::draw_pc_menu(dc,w,h,{"SMASH T.V.","MULTIPLAYER","> HOST LAN GAME",help});
            check(EqualRect(&menu.panel,&audit.panel) && menu.selection_visible && menu.visible_lines==4);
        }
        stv::host::draw_pc_menu(dc,w,h,lines);
        GdiFlush();
        BITMAPFILEHEADER file{};file.bfType=0x4d42;file.bfOffBits=sizeof(file)+sizeof(BITMAPINFOHEADER);
        file.bfSize=file.bfOffBits+w*h*4;
        std::ofstream image(out/(std::to_string(w)+"x"+std::to_string(h)+".bmp"),std::ios::binary);
        image.write(reinterpret_cast<char*>(&file),sizeof(file));image.write(reinterpret_cast<char*>(&bi.bmiHeader),sizeof(BITMAPINFOHEADER));
        image.write(static_cast<char*>(pixels),std::streamsize(w)*h*4);check(image.good());
        SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);
    }
    std::printf("Menu renderer: %d checks, %d failures; actual GDI offscreen captures written\n",checks,failures);
    return failures?1:0;
}
