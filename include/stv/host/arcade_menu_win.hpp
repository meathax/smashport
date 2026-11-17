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

inline void arcade_title_controls(HDC dc,int cw,int ch,const ArcadeMenuFont& font,int selection,bool enhanced) {
    // Keep the artwork's logo on the left and the contestant controls on the right.
    const int x=cw*48/100,w=cw-x-cw/32;
    const int h=std::min(ch*78/100,w*9/8),y=(ch-h)/2,unit=std::max(1,h/130);
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
    const char* actions[]={"START","MULTIPLAYER","OPTIONS","EXIT"};
    const int first=top+mode_h+h/32,row=h/9;
    for(int i=0;i<4;++i) {
        const int t=first+i*row;
        const bool on=selection==i+1;
        if(on){fill(x+unit,t,x+w-unit,t+row-1,red);fill(x+unit,t,x+4*unit,t+row-1,gold);}
        font.draw(dc,{x+pad,t+row/5,x+w-pad,t+row*4/5},actions[i],true,on?gold:RGB(221,235,233),on?red:dark);
    }
    font.draw(dc,{x+pad,y+h-h/10,x+w-pad,y+h-h/25},
              selection==0?"LEFT / RIGHT   CHANGE":"ENTER / A   SELECT",false,steel,dark,DT_CENTER);
}
}
