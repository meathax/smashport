// Global online leaderboards for the Windows player: retail score capture,
// navigation state and the GDI browser screen. Network I/O lives in the
// shared recomp client (third_party/recomp_leaderboard).
#pragma once
#include <windows.h>
#include "rl_leaderboard.hpp"
#include "stv/host/arcade_menu_win.hpp"
#include "stv/host/arcade_scores.hpp"
#include "stv/host/pad.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <set>
#include <string>
#include <vector>

namespace stv::host {

// ---------------------------------------------------------------- score capture
// The retail table reader lives in arcade_scores.hpp (shared with the replay verifier).

// Reports entries the retail game has just inserted. The first observation of
// a profile is a baseline. Both players' initials entries can time out on the
// same frame, so up to two new entries at once are real; more than that (table
// reset, CMOS load or profile/online image swap) is re-baselined instead of
// being reported, so factory/default tables are never uploaded.
class ArcadeScoreWatch {
    static constexpr std::size_t kMaxPlayers = 2;
    std::set<ArcadeScore> previous_, seen_;
    bool primed_ = false;
public:
    void reset() { previous_.clear(); seen_.clear(); primed_ = false; }
    std::vector<ArcadeScore> update(const std::vector<std::uint16_t>& cmos) {
        auto now = arcade_score_records(cmos);
        std::vector<ArcadeScore> fresh;
        if (now == previous_) return fresh;
        std::set<std::array<unsigned char, 8>> distinct;
        for (const auto& s : now)
            if (!previous_.count(s) && !seen_.count(s)) distinct.insert(s.raw);
        if (primed_ && distinct.size() <= kMaxPlayers)
            for (const auto& raw : distinct)
                for (const auto& s : now)
                    if (s.raw == raw) { fresh.push_back(s); break; }
        for (const auto& s : now) seen_.insert(s);
        previous_ = std::move(now);
        primed_ = true;
        return fresh;
    }
};

// ---------------------------------------------------------------- navigation
inline constexpr const char* kLeaderboardBoards[2] = {"arcade", "wide2"};
// "wide2" holds Widescreen ruleset 2 (kWidescreenRuleset). The retired "wide" board
// keeps v1.0.0-v1.0.4 scores, which were set under different Widescreen rules.
inline constexpr const char* kLeaderboardBoardLabels[2] = {"ARCADE  4:3", "WIDESCREEN  16:9"};
inline constexpr const char* kLeaderboardPeriods[3] = {"all", "week", "day"};
inline constexpr const char* kLeaderboardPeriodLabels[3] = {"ALL TIME", "LAST 7 DAYS", "LAST 24 HOURS"};

enum class LeaderboardInput { Up, Down, Left, Right, PageUp, PageDown, Home, End,
                              PrevPeriod, NextPeriod, FindMe, Refresh, Back };

struct LeaderboardNav {
    int board = 0, period = 0;
    int selected = 0, top = 0;
    int total = 0;     // rows available on the server (-1 = unknown)
    int visible = 10;  // rows the last draw could show
    bool find_me = false;
    bool refresh = false;

    void reset_view() { selected = top = 0; find_me = false; }
    void clamp() {
        const int count = std::max(0, total);
        selected = count ? std::clamp(selected, 0, count - 1) : 0;
        const int view = std::max(1, visible);
        if (selected < top) top = selected;
        if (selected >= top + view) top = selected - view + 1;
        top = std::clamp(top, 0, std::max(0, count - view));
    }
    // Returns false when the screen should close.
    bool apply(LeaderboardInput in) {
        const int view = std::max(1, visible);
        switch (in) {
        case LeaderboardInput::Up: --selected; break;
        case LeaderboardInput::Down: ++selected; break;
        case LeaderboardInput::PageUp: selected -= view; top -= view; break;
        case LeaderboardInput::PageDown: selected += view; top += view; break;
        case LeaderboardInput::Home: selected = top = 0; break;
        case LeaderboardInput::End: selected = std::max(0, total - 1); break;
        case LeaderboardInput::Left: case LeaderboardInput::Right:
            board ^= 1; reset_view(); break;
        case LeaderboardInput::PrevPeriod: period = (period + 2) % 3; reset_view(); break;
        case LeaderboardInput::NextPeriod: period = (period + 1) % 3; reset_view(); break;
        case LeaderboardInput::FindMe: find_me = true; break;
        case LeaderboardInput::Refresh: refresh = true; break;
        case LeaderboardInput::Back: return false;
        }
        clamp();
        return true;
    }
};

inline bool leaderboard_key(WPARAM vk, LeaderboardInput& out) {
    switch (vk) {
    case VK_UP: out = LeaderboardInput::Up; return true;
    case VK_DOWN: out = LeaderboardInput::Down; return true;
    case VK_LEFT: out = LeaderboardInput::Left; return true;
    case VK_RIGHT: out = LeaderboardInput::Right; return true;
    case VK_PRIOR: out = LeaderboardInput::PageUp; return true;
    case VK_NEXT: out = LeaderboardInput::PageDown; return true;
    case VK_HOME: out = LeaderboardInput::Home; return true;
    case VK_END: out = LeaderboardInput::End; return true;
    case 'Q': out = LeaderboardInput::PrevPeriod; return true;
    case 'E': case VK_TAB: case VK_RETURN: out = LeaderboardInput::NextPeriod; return true;
    case 'F': case 'Y': out = LeaderboardInput::FindMe; return true;
    case 'R': case VK_F5: out = LeaderboardInput::Refresh; return true;
    case VK_ESCAPE: case VK_BACK: out = LeaderboardInput::Back; return true;
    default: return false;
    }
}

// Pad buttons the shared menu router does not translate (A/B/X and the
// directions arrive as menu keys). LB/RB period, LT/RT page, Y find me.
inline int leaderboard_pad(std::uint32_t fresh, LeaderboardInput* out) {
    int n = 0;
    if (fresh & PadLB) out[n++] = LeaderboardInput::PrevPeriod;
    if (fresh & PadRB) out[n++] = LeaderboardInput::NextPeriod;
    if (fresh & PadLT) out[n++] = LeaderboardInput::PageUp;
    if (fresh & PadRT) out[n++] = LeaderboardInput::PageDown;
    if (fresh & PadY) out[n++] = LeaderboardInput::FindMe;
    return n;
}

inline std::string leaderboard_score_text(std::uint64_t score) {
    std::string digits = std::to_string(score), out;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i && (digits.size() - i) % 3 == 0) out.push_back(',');
        out.push_back(digits[i]);
    }
    return out;
}

inline std::string leaderboard_age_text(std::int64_t then, std::int64_t now) {
    const std::int64_t age = std::max<std::int64_t>(0, now - then);
    if (age < 60) return "JUST NOW";
    if (age < 3600) return std::to_string(age / 60) + "M AGO";
    if (age < 86400) return std::to_string(age / 3600) + "H AGO";
    if (age < 7 * 86400) return std::to_string(age / 86400) + "D AGO";
    std::time_t t = std::time_t(then);
    std::tm tm{};
    if (gmtime_s(&tm, &t)) return "";
    static const char* months[] = {"JAN","FEB","MAR","APR","MAY","JUN","JUL","AUG","SEP","OCT","NOV","DEC"};
    char text[24];
    std::snprintf(text, sizeof text, "%02d %s %04d", tm.tm_mday, months[tm.tm_mon % 12], tm.tm_year + 1900);
    return text;
}

// ---------------------------------------------------------------- screen
class GlobalLeaderboardScreen {
public:
    LeaderboardNav nav;
    bool is_open() const { return open_; }
    void open(int board) {
        open_ = true;
        nav = {};
        nav.board = board & 1;
        nav.total = -1;
        first_request_ = true;
    }
    void close() { open_ = false; }
    bool input(LeaderboardInput in) {
        if (!nav.apply(in)) { open_ = false; return false; }
        if (in == LeaderboardInput::Left || in == LeaderboardInput::Right ||
            in == LeaderboardInput::PrevPeriod || in == LeaderboardInput::NextPeriod) {
            nav.total = -1;
            first_request_ = true;
        }
        return true;
    }

    // Issue only the requests the current view needs (client throttles repeats).
    void sync(rl::lb::Client& client, std::int64_t now) {
        if (!open_ || !client.configured()) return;
        const std::string board = kLeaderboardBoards[nav.board], period = kLeaderboardPeriods[nav.period];
        const int size = client.page_size();
        const bool force = nav.refresh;
        nav.refresh = false;
        if (first_request_ || force) {
            client.request_rank(board, period, force);
            first_request_ = false;
        }
        rl::lb::MyRank me;
        if (nav.find_me && client.rank(board, period, me)) {
            nav.find_me = false;
            if (me.found) {
                nav.total = std::max(nav.total, me.total);
                nav.selected = me.rank - 1;
                nav.top = nav.selected - nav.visible / 2;
                nav.clamp();
            }
        }
        const int first = (nav.top / size) * size;
        const int last = ((nav.top + nav.visible + 8) / size) * size;
        for (int offset = first; offset <= last; offset += size) {
            if (nav.total >= 0 && offset >= std::max(1, nav.total)) break;
            client.request(board, period, offset, force);
        }
        rl::lb::Page head;
        if (client.page(board, period, 0, head)) {
            nav.total = head.total;
            nav.clamp();
        }
        (void)now;
    }

    void draw(HDC dc, int cw, int ch, const ArcadeMenuFont* font, const rl::lb::Client& client,
              std::int64_t now) {
        if (cw < 160 || ch < 120) return;
        const std::string board = kLeaderboardBoards[nav.board], period = kLeaderboardPeriods[nav.period];
        const COLORREF navy = RGB(12, 16, 26), panel = RGB(17, 23, 36), line = RGB(36, 48, 66),
                       red = RGB(127, 26, 28), gold = RGB(255, 212, 78), steel = RGB(106, 204, 221),
                       white = RGB(226, 232, 230), dim = RGB(122, 134, 150), mine = RGB(18, 64, 74),
                       silver = RGB(205, 214, 222), bronze = RGB(214, 140, 76);
        auto fill = [&](int l, int t, int r, int b, COLORREF c) {
            RECT box{l, t, r, b};
            SetDCBrushColor(dc, c);
            FillRect(dc, &box, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        };
        HFONT gdi = nullptr;
        int gdi_height = 0;
        auto text = [&](RECT r, const std::string& s, bool large, COLORREF ink, COLORREF paper, UINT align) {
            if (s.empty() || r.right <= r.left || r.bottom <= r.top) return;
            if (font) { font->draw(dc, r, s, large, ink, paper, align); return; }
            const int h = std::max(8, int(r.bottom - r.top));
            if (!gdi || gdi_height != h) {
                if (gdi) DeleteObject(gdi);
                gdi = CreateFontW(-h, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                  CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"Terminal");
                gdi_height = h;
            }
            const HGDIOBJ old = SelectObject(dc, gdi);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, ink);
            DrawTextA(dc, s.c_str(), int(s.size()), &r, align | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
            SelectObject(dc, old);
        };

        fill(0, 0, cw, ch, navy);
        const int margin = std::max(10, cw / 48);
        const int pw = std::min(cw - 2 * margin, std::max(320, ch * 3 / 2));
        const int ph = ch - 2 * margin;
        const int px = (cw - pw) / 2, py = margin;
        const int unit = std::max(1, ph / 260);
        fill(px + 3 * unit, py + 3 * unit, px + pw + 3 * unit, py + ph + 3 * unit, RGB(4, 6, 10));
        fill(px, py, px + pw, py + ph, red);
        fill(px + 2 * unit, py + 2 * unit, px + pw - 2 * unit, py + ph - 2 * unit, gold);
        fill(px + 3 * unit, py + 3 * unit, px + pw - 3 * unit, py + ph - 3 * unit, panel);
        const int in_l = px + 3 * unit, in_r = px + pw - 3 * unit;
        const int pad = std::max(6, pw / 40);

        // Title bar.
        const int head_h = ph * 10 / 100;
        int y = py + 3 * unit;
        fill(in_l, y, in_r, y + head_h, red);
        text({in_l + pad, y + head_h / 6, in_r - pad, y + head_h * 5 / 6}, "GLOBAL LEADERBOARDS", true, gold, red, DT_CENTER);
        y += head_h + ph / 60;

        // Board tabs.
        const int tab_h = ph * 7 / 100;
        const int tab_w = (in_r - in_l - 2 * pad - pad / 2) / 2;
        for (int i = 0; i < 2; ++i) {
            const int l = in_l + pad + i * (tab_w + pad / 2);
            const bool on = i == nav.board;
            fill(l, y, l + tab_w, y + tab_h, on ? gold : line);
            fill(l + unit, y + unit, l + tab_w - unit, y + tab_h - (on ? 0 : unit), on ? red : panel);
            text({l + pad / 2, y + tab_h / 5, l + tab_w - pad / 2, y + tab_h * 4 / 5}, kLeaderboardBoardLabels[i],
                 true, on ? gold : dim, on ? red : panel, DT_CENTER);
        }
        y += tab_h;
        fill(in_l + pad, y, in_r - pad, y + unit, gold);
        y += ph / 70;

        // Period chips.
        const int chip_h = ph * 5 / 100;
        const int chip_w = (in_r - in_l - 2 * pad) / 3;
        for (int i = 0; i < 3; ++i) {
            const int l = in_l + pad + i * chip_w;
            const bool on = i == nav.period;
            if (on) { fill(l + pad / 3, y, l + chip_w - pad / 3, y + chip_h, steel);
                      fill(l + pad / 3 + unit, y + unit, l + chip_w - pad / 3 - unit, y + chip_h - unit, mine); }
            text({l + pad / 2, y + chip_h / 5, l + chip_w - pad / 2, y + chip_h * 4 / 5}, kLeaderboardPeriodLabels[i],
                 false, on ? white : dim, on ? mine : panel, DT_CENTER);
        }
        y += chip_h + ph / 70;

        // Your standing.
        const int card_h = ph * 6 / 100;
        rl::lb::MyRank me;
        const bool have_me = client.rank(board, period, me);
        fill(in_l + pad, y, in_r - pad, y + card_h, line);
        fill(in_l + pad + unit, y + unit, in_r - pad - unit, y + card_h - unit, RGB(22, 30, 46));
        std::string standing;
        if (!client.configured()) standing = "ONLINE RANKINGS ARE NOT SET UP IN THIS BUILD";
        else if (!have_me) standing = client.offline() ? "YOUR RANK   OFFLINE" : "YOUR RANK   ...";
        else if (!me.found) standing = "YOUR RANK   NO SCORE YET  -  ENTER YOUR INITIALS TO RANK";
        else standing = "YOUR RANK   #" + std::to_string(me.rank) + " OF " + std::to_string(me.total) +
                        "     " + leaderboard_score_text(me.score) + "   " + me.name;
        text({in_l + 2 * pad, y + card_h / 4, in_r - 2 * pad, y + card_h * 3 / 4}, standing, false,
             have_me && me.found ? gold : white, RGB(22, 30, 46), DT_LEFT);
        y += card_h + ph / 70;

        // Table.
        const int footer_h = ph * 11 / 100;
        const int col_h = ph * 45 / 1000;
        const int table_l = in_l + pad, table_r = in_r - pad - std::max(5, 3 * unit) - unit * 2;
        const int tw = table_r - table_l;
        const int c_rank = table_l + tw * 2 / 100, c_name = table_l + tw * 18 / 100,
                  c_score_r = table_l + tw * 72 / 100, c_date = table_l + tw * 76 / 100;
        fill(table_l, y, table_r, y + col_h, RGB(28, 38, 56));
        auto col = [&](int l, int r, const char* s, UINT align) {
            text({l, y + col_h / 5, r, y + col_h * 4 / 5}, s, false, steel, RGB(28, 38, 56), align);
        };
        col(c_rank, c_name, "RANK", DT_LEFT);
        col(c_name, c_score_r - tw * 22 / 100, "PLAYER", DT_LEFT);
        col(c_score_r - tw * 26 / 100, c_score_r, "SCORE", DT_RIGHT);
        col(c_date, table_r - pad / 2, "WHEN", DT_LEFT);
        y += col_h;
        const int rows_top = y, rows_bottom = py + ph - 3 * unit - footer_h;
        const int row_h = std::max(12, ph / 24);
        nav.visible = std::max(1, (rows_bottom - rows_top) / row_h);
        nav.clamp();

        const int size = client.page_size();
        const std::string me_pid = client.pid();
        std::int64_t fetched = 0;
        rl::lb::Page cached;
        int cached_offset = -1;
        bool any_loading = false;
        for (int i = 0; i < nav.visible; ++i) {
            const int index = nav.top + i;
            if (nav.total >= 0 && index >= nav.total) break;
            const int offset = (index / size) * size;
            if (offset != cached_offset) {
                std::int64_t when = 0;
                if (!client.page(board, period, offset, cached, &when)) cached.rows.clear();
                else if (!offset || !fetched) fetched = when;
                cached_offset = offset;
                any_loading = any_loading || client.loading(board, period, offset);
            }
            const int r_top = rows_top + i * row_h;
            const bool selected = index == nav.selected;
            const rl::lb::Row* row = nullptr;
            const int local = index - offset;
            if (local >= 0 && local < int(cached.rows.size())) row = &cached.rows[local];
            const bool own = row && row->pid == me_pid;
            COLORREF bg = selected ? red : own ? mine : (i & 1) ? RGB(20, 27, 41) : panel;
            fill(table_l, r_top, table_r, r_top + row_h - unit, bg);
            if (selected) fill(table_l, r_top, table_l + 3 * unit, r_top + row_h - unit, gold);
            const int t = r_top + row_h / 6, b = r_top + row_h * 5 / 6 - unit;
            if (!row) {
                text({c_name, t, c_score_r, b}, any_loading ? "LOADING ..." : "- - -", false, dim, bg, DT_LEFT);
                continue;
            }
            const COLORREF medal = row->rank == 1 ? gold : row->rank == 2 ? silver : row->rank == 3 ? bronze : white;
            if (row->rank <= 3) fill(c_rank - 1, t + unit, c_rank + 2 * unit, b - unit, medal);
            text({c_rank + 7 * unit, t, c_name - pad / 2, b}, std::to_string(row->rank), row->rank <= 3,
                 row->rank <= 3 ? medal : selected ? gold : white, bg, DT_LEFT);
            text({c_name, t, c_score_r - tw * 28 / 100, b}, row->name, true, selected ? gold : own ? steel : white, bg, DT_LEFT);
            if (own) text({c_score_r - tw * 27 / 100, t + row_h / 10, c_score_r - tw * 21 / 100, b - row_h / 10},
                          "YOU", false, steel, bg, DT_LEFT);
            if (row->verified) {   // the server re-simulated this run's replay and the score matched
                const int name_r = c_score_r - tw * 28 / 100;
                text({c_name + (name_r - c_name) * 52 / 100, t + row_h / 10, name_r, b - row_h / 10},
                     "VERIFIED", false, RGB(96, 208, 128), bg, DT_RIGHT);
            }
            text({c_score_r - tw * 22 / 100, t, c_score_r, b}, leaderboard_score_text(row->score), true,
                 selected ? gold : white, bg, DT_RIGHT);
            text({c_date, t, table_r - pad / 2, b}, leaderboard_age_text(row->ts, now), false, dim, bg, DT_LEFT);
        }
        if (nav.total == 0) {
            const int mid = (rows_top + rows_bottom) / 2;
            text({table_l, mid - row_h, table_r, mid + row_h}, client.configured() ? "NO SCORES YET  -  BE THE FIRST!"
                                                                                   : "LEADERBOARD SERVER NOT CONFIGURED",
                 true, gold, panel, DT_CENTER);
        } else if (nav.total < 0 && !any_loading && client.offline()) {
            const int mid = (rows_top + rows_bottom) / 2;
            text({table_l, mid - row_h, table_r, mid + row_h}, "OFFLINE  -  NO SAVED RANKINGS YET", true, gold, panel, DT_CENTER);
        }

        // Scrollbar.
        if (nav.total > nav.visible) {
            const int sl = table_r + unit * 2, sr = sl + std::max(5, 3 * unit);
            const int track = rows_bottom - rows_top;
            fill(sl, rows_top, sr, rows_bottom, line);
            const int thumb = std::max(row_h, track * nav.visible / nav.total);
            const int pos = rows_top + (track - thumb) * nav.top / std::max(1, nav.total - nav.visible);
            fill(sl, pos, sr, pos + thumb, steel);
        }

        // Footer: status and controls.
        const int fy = rows_bottom + ph / 80;
        const int fl = (footer_h - ph / 80) / 2;
        std::string status;
        COLORREF status_ink = steel;
        if (!client.configured()) { status = "OFFLINE BUILD"; status_ink = dim; }
        else if (any_loading) status = "UPDATING ...";
        else if (client.offline()) {
            status = fetched ? "OFFLINE  -  SHOWING RANKINGS FROM " + leaderboard_age_text(fetched, now) : "OFFLINE";
            status_ink = bronze;
        } else if (!client.last_error().empty()) { status = client.last_error(); status_ink = bronze; }
        else if (fetched) status = "ONLINE  -  UPDATED " + leaderboard_age_text(fetched, now);
        if (nav.total > 0) status += "      " + std::to_string(nav.selected + 1) + " / " + std::to_string(nav.total);
        if (const auto waiting = client.pending())
            status += "      " + std::to_string(waiting) + (waiting == 1 ? " SCORE" : " SCORES") + " WAITING TO UPLOAD";
        text({in_l + pad, fy + fl / 8, in_r - pad, fy + fl * 3 / 4}, status, false, status_ink, panel, DT_LEFT);
        text({in_l + pad, fy + fl + fl / 8, in_r - pad, fy + fl + fl * 5 / 8},
             "< > MODE    LB/RB PERIOD    LT/RT PAGE    Y FIND ME    X REFRESH    B BACK",
             false, dim, panel, DT_CENTER);
        if (gdi) DeleteObject(gdi);
    }

private:
    bool open_ = false;
    bool first_request_ = true;
};

}  // namespace stv::host
