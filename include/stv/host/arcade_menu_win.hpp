#pragma once
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace stv::host {

// L8 RD7FONT and RD15FONT, in TEXT.ASM ASCII order. Decode from the player's
// imported graphics using the same padded sprite stride as the arcade DMA.
class ArcadeMenuFont {
    struct Glyph {
        int width = 0, height = 0;
        std::vector<unsigned char> mask;
    };
    std::array<std::array<Glyph,93>,2> banks_{};
public:
    template<class Machine, class ReadLong>
    bool bind(Machine& machine, ReadLong read_long) {
        const unsigned tables[] = {0xffe64630u, 0xffe651d0u};
        for (int bank=0; bank<2; ++bank) {
            for (unsigned i=0; i<93; ++i) {
                auto& glyph = banks_[bank][i];
                const auto descriptor = read_long(tables[bank] + 32*i);
                if (descriptor < 0xffe00000u || descriptor > 0xffffff80u) return false;
                glyph.width = machine.rw(descriptor);
                glyph.height = machine.rw(descriptor+16);
                if (glyph.width>32 || glyph.height>32) return false;
                const auto source = read_long(descriptor+64);
                if (source<0x02000000u) return false;
                const auto offset = (source-0x02000000u)>>3;
                const int stride = (glyph.width+3)&~3;
                if (offset+stride*glyph.height>machine.gfx.size()) return false;
                glyph.mask.resize(glyph.width*glyph.height);
                for (int y=0; y<glyph.height; ++y)
                    for (int x=0; x<glyph.width; ++x)
                        glyph.mask[y*glyph.width+x]=machine.gfx[offset+y*stride+x]!=0;
            }
        }
        return true;
    }
    void draw(HDC dc, RECT rect, std::string text, bool large, COLORREF ink, COLORREF paper,
              UINT align=DT_LEFT) const {
        if(text.empty() || rect.right<=rect.left || rect.bottom<=rect.top) return;
        const auto& bank=banks_[large];
        int width=0, height=large?16:9;
        for(char& c:text) {
            if(c>='a' && c<='z') c=char(c-'a'+'A');
            if(c<32 || c>125) c='?';
            width+=c==' '?5:bank[c-33].width+1;
            if(c!=' ') height=std::max(height,bank[c-33].height);
        }
        auto dib=[](COLORREF c){return (unsigned(GetRValue(c))<<16)|(unsigned(GetGValue(c))<<8)|GetBValue(c);};
        std::vector<unsigned> pixels(width*height,dib(paper));
        int cursor=0;
        for(char c:text) {
            if(c==' ') {cursor+=5;continue;}
            const auto& g=bank[c-33];
            for(int y=0;y<g.height;++y) for(int x=0;x<g.width;++x)
                if(g.mask[y*g.width+x]) pixels[(height-g.height+y)*width+cursor+x]=dib(ink);
            cursor+=g.width+1;
        }
        const double scale=std::min(double(rect.right-rect.left)/width,double(rect.bottom-rect.top)/height);
        const int w=std::max(1,int(width*scale)), h=std::max(1,int(height*scale));
        const int x=align==DT_CENTER?(rect.left+rect.right-w)/2:align==DT_RIGHT?rect.right-w:rect.left;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth=width; bi.bmiHeader.biHeight=-height;
        bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;
        SetStretchBltMode(dc,COLORONCOLOR);
        StretchDIBits(dc,x,(rect.top+rect.bottom-h)/2,w,h,0,0,width,height,pixels.data(),&bi,DIB_RGB_COLORS,SRCCOPY);
    }
};

inline const char* game_mode_label(bool enhanced) {
    return enhanced ? "CUSTOM WIDESCREEN 16:9" : "ORIGINAL ARCADE 4:3";
}

struct ArcadeTitleLayout {
    RECT artwork_pane{};
    RECT artwork{};
    RECT controls{};
};

inline RECT aspect_fit_rect(RECT bounds, int source_width, int source_height) {
    const int width = std::max(0, int(bounds.right - bounds.left));
    const int height = std::max(0, int(bounds.bottom - bounds.top));
    if (source_width <= 0 || source_height <= 0 || width == 0 || height == 0) return {};
    const int fitted_width = std::min(width, int(std::int64_t(height) * source_width / source_height));
    const int fitted_height = std::min(height, int(std::int64_t(width) * source_height / source_width));
    return {bounds.left + (width - fitted_width) / 2,
            bounds.top + (height - fitted_height) / 2,
            bounds.left + (width + fitted_width) / 2,
            bounds.top + (height + fitted_height) / 2};
}

inline ArcadeTitleLayout arcade_title_layout(int client_width, int client_height) {
    ArcadeTitleLayout layout;
    const int margin = std::max(8, client_width / 40);
    const int gutter = std::max(12, client_width / 50);
    const int controls_left = client_width * 48 / 100;
    const int controls_width = std::max(1, client_width - margin - controls_left);
    const int pane_height = std::max(1, std::min(client_height * 78 / 100, controls_width * 9 / 8));
    const int pane_top = (client_height - pane_height) / 2;
    const int artwork_right = std::max(margin, controls_left - gutter);
    layout.controls = {controls_left, pane_top, controls_left + controls_width, pane_top + pane_height};
    layout.artwork_pane = {margin, pane_top, artwork_right, pane_top + pane_height};
    RECT artwork_bounds = layout.artwork_pane;
    const int padding = std::max(4, std::min(12, pane_height / 40));
    InflateRect(&artwork_bounds, -padding, -padding);
    layout.artwork = aspect_fit_rect(artwork_bounds, 410, 256);
    return layout;
}

inline void arcade_title_controls(HDC dc,int cw,int ch,const ArcadeMenuFont& font,int selection,bool enhanced) {
    const auto layout=arcade_title_layout(cw,ch);
    const int x=layout.controls.left,w=layout.controls.right-layout.controls.left;
    const int h=layout.controls.bottom-layout.controls.top,y=layout.controls.top;
    const int unit=std::max(1,h/130);
    auto fill=[&](int l,int t,int r,int b,COLORREF c) {
        RECT box{l,t,r,b};SetDCBrushColor(dc,c);FillRect(dc,&box,static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    };
    const COLORREF steel=RGB(84,159,158), dark=RGB(9,21,30), red=RGB(127,26,28), gold=RGB(255,212,78);
    fill(x+3*unit,y+3*unit,x+w+3*unit,y+h+3*unit,RGB(4,8,12));
    fill(x,y,x+w,y+h,steel);fill(x+unit,y+unit,x+w-unit,y+h-unit,dark);
    const int pad=w/18, head=h/7;
    fill(x+unit,y+unit,x+w-unit,y+head,red);
    font.draw(dc,{x+pad,y+head/5,x+w-pad,y+head*4/5},"MAIN MENU",true,gold,red,DT_CENTER);
    const int top=y+head+h/25, mode_h=h/7;
    if(selection==0)fill(x+unit,top,x+w-unit,top+mode_h,red);
    font.draw(dc,{x+pad,top+unit,x+w-pad,top+mode_h/3},enhanced ? "CUSTOM MODE" : "CLASSIC ARCADE EXPERIENCE",false,steel,selection==0?red:dark);
    font.draw(dc,{x+pad,top+mode_h/2,x+w-pad,top+mode_h-unit},
              std::string("< ") + game_mode_label(enhanced) + " >",true,gold,selection==0?red:dark);
    const char* actions[]={"START","MULTIPLAYER","OPTIONS","GLOBAL LEADERBOARDS","EXIT"};
    const int first=top+mode_h+h/32,row=h*2/21;
    for(int i=0;i<5;++i) {
        const int t=first+i*row;
        const bool on=selection==i+1;
        if(on){fill(x+unit,t,x+w-unit,t+row-1,red);fill(x+unit,t,x+4*unit,t+row-1,gold);}
        font.draw(dc,{x+pad,t+row/5,x+w-pad,t+row*4/5},actions[i],true,on?gold:RGB(221,235,233),on?red:dark);
    }
    font.draw(dc,{x+pad,y+h-h/10,x+w-pad,y+h-h/25},
              selection==0?"LEFT / RIGHT   CHANGE":"ENTER / A   SELECT",false,steel,dark,DT_CENTER);
}

// GPU live-arena main menu. RGB(255,0,255) is a transparent overlay key and
// RGB(255,0,128) a translucent dark glass key (resident_display.hlsl overlayPS).
constexpr COLORREF kOverlayClear=RGB(255,0,255), kOverlayGlass=RGB(255,0,128);
inline RECT arcade_scene_panel(int cw,int ch) {
    const int margin=std::max(12,cw/28);
    // A small card at the bottom centre: the booth behind it is the show.
    const int w=std::clamp(cw*20/100,std::min(cw-2*margin,260),std::max(260,cw-2*margin));
    const int bottom=ch*95/100, h=std::max(std::min(ch*85/100,240),ch*46/100);
    const int left=(cw-w)/2;
    return {left,bottom-h,left+w,bottom};
}
inline void arcade_scene_controls(HDC dc,int cw,int ch,const ArcadeMenuFont& font,int selection,bool enhanced,
                                  unsigned long long ms) {
    const RECT panel=arcade_scene_panel(cw,ch);
    const int x=panel.left,y=panel.top,w=panel.right-panel.left,h=panel.bottom-panel.top;
    const int unit=std::max(1,h/220);
    auto fill=[&](int l,int t,int r,int b,COLORREF c) {
        RECT box{l,t,r,b};SetDCBrushColor(dc,c);FillRect(dc,&box,static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    };
    const COLORREF steel=RGB(84,159,158), red=RGB(127,26,28), gold=RGB(255,212,78), glass=kOverlayGlass;
    fill(x,y,x+w,y+h,glass);
    fill(x,y,x+w,y+unit,steel);fill(x,y+h-unit,x+w,y+h,steel);
    fill(x,y,x+unit,y+h,steel);fill(x+w-unit,y,x+w,y+h,steel);
    const int pad=w/14, head=h*17/100;
    fill(x+unit,y+unit,x+w-unit,y+head,red);
    fill(x+unit,y+head,x+w-unit,y+head+2*unit,gold);
    font.draw(dc,{x+pad,y+head/6,x+w-pad,y+head*3/5},"SMASH T.V.",true,gold,red,DT_CENTER);
    font.draw(dc,{x+pad,y+head*13/20,x+w-pad,y+head*9/10},"LIVE FROM THE ARENA",false,RGB(221,235,233),red,DT_CENTER);
    const int top=y+head+h/20, mode_h=h*13/100;
    if(selection==0){fill(x+unit,top,x+w-unit,top+mode_h,red);fill(x+unit,top,x+5*unit,top+mode_h,gold);}
    const COLORREF mpaper=selection==0?red:glass;
    font.draw(dc,{x+pad,top+unit*2,x+w-pad,top+mode_h*2/5},enhanced?"CUSTOM MODE":"CLASSIC ARCADE EXPERIENCE",false,steel,mpaper);
    font.draw(dc,{x+pad,top+mode_h/2,x+w-pad,top+mode_h-unit*2},
              std::string("< ")+game_mode_label(enhanced)+" >",true,gold,mpaper);
    const char* actions[]={"START","MULTIPLAYER","OPTIONS","GLOBAL LEADERBOARDS","EXIT"};
    const int first=top+mode_h+h/28,row=h*9/100;
    for(int i=0;i<5;++i) {
        const int t=first+i*row;
        const bool on=selection==i+1;
        if(on){fill(x+unit,t,x+w-unit,t+row-unit,red);fill(x+unit,t,x+5*unit,t+row-unit,gold);}
        font.draw(dc,{x+pad,t+row/5,x+w-pad,t+row*4/5},actions[i],true,on?gold:RGB(221,235,233),on?red:glass);
    }
    font.draw(dc,{x+pad,y+h-h/11,x+w-pad,y+h-h/28},
              selection==0?"LEFT / RIGHT   CHANGE":"ENTER / A   SELECT",false,steel,glass,DT_CENTER);
    // broadcast bug: blinking ON AIR light, top right
    const int bh=std::max(14,ch/22),bw=bh*4,bx=cw-std::max(12,cw/28)-bw,by=ch*8/100;
    fill(bx,by,bx+bw,by+bh,red);
    if((ms/600)%2==0)fill(bx+bh/4,by+bh/3,bx+bh/4+bh/3,by+bh/3+bh/3,RGB(255,236,200));
    font.draw(dc,{bx+bh*3/4,by+bh/5,bx+bw-bh/5,by+bh*4/5},"ON AIR",true,gold,red,DT_CENTER);
    font.draw(dc,{bx-bw/2,by+bh+unit*3,bx+bw,by+bh+bh*3/5},"ARENA CAM 1",false,RGB(221,235,233),glass,DT_RIGHT);
}
}
