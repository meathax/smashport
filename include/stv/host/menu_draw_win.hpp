// Shared production GDI menu renderer; usable with an offscreen DC for layout checks.
#pragma once
#include <windows.h>
#include "arcade_menu_win.hpp"
#include <algorithm>
#include <string>
#include <vector>
#include <utility>
namespace stv::host {
struct MenuRenderAudit { RECT panel{}; int total_lines=0,visible_lines=0; bool selection_visible=false; };
inline MenuRenderAudit draw_pc_menu(HDC dc,int cw,int ch,std::vector<std::string> lines,const ArcadeMenuFont* arcade=nullptr) {
    MenuRenderAudit audit;
    if(cw<96 || ch<96 || lines.empty()) return audit;
    SetBkMode(dc,TRANSPARENT);
    // Keep the panel inside the client and the selected row visible when
    // a binding list needs a viewport. Preserve crisp arcade lettering.
    for (auto& line : lines)
        for (char& c : line) if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    const int bw = std::min(cw - 24, std::max(520, std::min(cw * 9 / 10, ch)));
    const int bh = std::min(ch - 24, std::max(260, ch * 3 / 5));
    const int lh = std::max(9, (bh - 36) / 16);
    const int font_size = std::max(8, lh * 3 / 4);
    HFONT font = CreateFontW(-font_size, std::max(4, font_size * 3 / 5), 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY,
                             FIXED_PITCH | FF_MODERN, L"Terminal");
    const HGDIOBJ previous_font = SelectObject(dc, font);
    const int capacity = std::max(3, (bh - 36) / lh);
    audit.total_lines = int(lines.size());
    if (int(lines.size()) > capacity) {
        int selected = 1;
        for (int i=1; i<int(lines.size()); ++i)
            if (lines[i].rfind("> ",0)==0) { selected=i; break; }
        const int body = capacity-2;
        const int first = std::clamp(selected-body/2,1,int(lines.size())-body);
        std::vector<std::string> visible{lines.front()};
        visible.insert(visible.end(),lines.begin()+first,lines.begin()+first+body);
        visible.push_back("ROWS "+std::to_string(first)+"-"+std::to_string(first+body-1)+
                          " OF "+std::to_string(lines.size()-1));
        lines=std::move(visible);
    }
    audit.visible_lines=int(lines.size());
    const int bx = std::max(12, (cw - bw) / 2), by = std::max(12, (ch - bh) / 2);
    RECT box{bx, by, bx + bw, by + bh};
    audit.panel=box;
    auto fill = [&](RECT area, COLORREF color) {
        HBRUSH brush = CreateSolidBrush(color); FillRect(dc, &area, brush); DeleteObject(brush);
    };
    fill(box, RGB(114, 22, 26));
    RECT inner{bx + 5, by + 5, bx + bw - 5, by + bh - 5};
    fill(inner, RGB(238, 174, 55));
    InflateRect(&inner, -2, -2); fill(inner, RGB(15, 18, 27));
    // Steel rivets and a red broadcast marquee echo the arcade HUD panels.
    for (int sx : {bx + 10, bx + bw - 14}) for (int sy : {by + 10, by + bh - 14})
        fill({sx, sy, sx + 4, sy + 4}, RGB(246, 215, 138));
    fill({bx + 18, by + 13, bx + bw - 18, by + 17 + lh}, RGB(119, 23, 30));
    const int saved_dc = SaveDC(dc);
    IntersectClipRect(dc, bx + 4, by + 4, bx + bw - 4, by + bh - 4);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const auto& line = lines[i];
        const int y = by + 18 + int(i) * lh;
        const bool selected = line.rfind("> ", 0) == 0;
        if(selected) audit.selection_visible = y >= by+4 && y+lh <= by+bh-4;
        const bool section = line.rfind("-- ", 0) == 0 || line.rfind("[ ", 0) == 0;
        if (selected) {
            fill({bx + 18, y - 2, bx + bw - 18, y + lh - 2}, RGB(120, 30, 32));
            fill({bx + 18, y - 2, bx + 22, y + lh - 2}, RGB(255, 224, 90));
        }
        const bool disabled = line.find("AVAILABLE OFFLINE") != std::string::npos || line.find("NO ONLINE GAME") != std::string::npos || line.find("UNAVAILABLE") != std::string::npos;
        SetTextColor(dc, disabled ? RGB(130, 137, 149) : selected || i == 0 ? RGB(255, 224, 90) :
                         section ? RGB(106, 204, 221) : RGB(224, 226, 217));
        RECT text{bx + 28, y, bx + bw - 28, y + lh};
        if (arcade) {
            const auto color=GetTextColor(dc);
            const auto paper=selected?RGB(120,30,32):i==0?RGB(119,23,30):RGB(15,18,27);
            text.bottom-=std::max(2,lh/5);
            auto label=selected?line.substr(2):line;
            while (!label.empty() && label.front()==' ') label.erase(0,1);
            arcade->draw(dc,text,label,i==0,color,paper,i==0||section?DT_CENTER:DT_LEFT);
            continue;
        }
        // Fit the complete fallback label, including a long help message.
        SIZE extent{};
        GetTextExtentPoint32A(dc,line.c_str(),int(line.size()),&extent);
        const int state=SaveDC(dc);
        const int width=text.right-text.left;
        if (extent.cx>width) {
            SetMapMode(dc,MM_ANISOTROPIC);
            SetWindowExtEx(dc,extent.cx,1,nullptr);
            SetViewportExtEx(dc,width,1,nullptr);
            SetViewportOrgEx(dc,text.left,0,nullptr);
            text.left=0;
            text.right=extent.cx;
        }
        DrawTextA(dc,line.c_str(),int(line.size()),&text,
                  (i==0 || section ? DT_CENTER : DT_LEFT) | DT_SINGLELINE | DT_NOPREFIX);
        RestoreDC(dc,state);
    }
    RestoreDC(dc, saved_dc);
    SelectObject(dc, previous_font);
    DeleteObject(font);
    return audit;
}
}
