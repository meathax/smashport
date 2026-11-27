// Pickup toast queue + pixel-font rasteriser. Glyphs: Press Start 2P (SIL OFL 1.1),
// baked by tools/fonts/bake_pixel_font.py into pixel_font_data.hpp.
#include "stv/host/variant_toast.hpp"

#include <cstddef>
#include "stv/gsp/variants.hpp"
#include "stv/host/pixel_font_data.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace stv::host {
namespace {
float ease(float t) { t = std::clamp(t, 0.f, 1.f); return 1.f - (1.f - t) * (1.f - t) * (1.f - t); }
constexpr std::uint32_t kText = 0xF4F6FA, kDim = 0xA3ABBD, kCard = 0x0B1220;

void fill(std::vector<std::uint32_t>& px, const ToastLayout& l, int x, int y, int w, int h, std::uint32_t c) {
    for (int j = std::max(0, y); j < std::min(l.height, y + h); ++j)
        for (int i = std::max(0, x); i < std::min(l.width, x + w); ++i)
            px[std::size_t(j) * std::size_t(l.width) + std::size_t(i)] = c;
}
void text(std::vector<std::uint32_t>& px, const ToastLayout& l, int x, int y, const char* s, int scale, std::uint32_t c) {
    for (; *s; ++s, x += 8 * scale) {
        const unsigned char ch = static_cast<unsigned char>(*s);
        const auto* glyph = kPixelFont8x8[(ch >= 32 && ch < 127 ? ch : '?') - 32];
        for (int row = 0; row < 8; ++row)
            for (int col = 0; col < 8; ++col)
                if (glyph[row] & (0x80 >> col)) fill(px, l, x + col * scale, y + row * scale, scale, scale, c);
    }
}
}  // namespace

void VariantToastQueue::push(int player, std::uint8_t variant) {
    const auto& info = stv::gsp::variant_info(static_cast<stv::gsp::Variant>(variant));
    VariantToast t;
    t.player = player ? 1 : 0;
    t.variant = variant;
    t.name = info.name;
    t.blurb = info.blurb;
    t.tint = info.tint;
    items_.push_back(t);
    // Each player stacks independently on its own side: evict that player's oldest.
    for (;;) {
        int count = 0;
        for (const auto& it : items_) count += it.player == t.player;
        if (count <= kMaxVisible) break;
        items_.erase(std::find_if(items_.begin(), items_.end(),
                                  [&](const VariantToast& it) { return it.player == t.player; }));
    }
}

void VariantToastQueue::advance(double dt_ms) {
    if (dt_ms < 0) dt_ms = 0;
    const int n = int(items_.size());
    for (int i = 0; i < n; ++i) {
        auto& t = items_[std::size_t(i)];
        t.age_ms += dt_ms;
        int newer = 0;   // newer cards on the same side push this one down
        for (int k = i + 1; k < n; ++k) newer += items_[std::size_t(k)].player == t.player;
        const double target = double(newer);
        const double step = dt_ms / kShiftMs;
        t.slot = t.slot < target ? std::min(target, t.slot + step) : std::max(target, t.slot - step);
    }
    items_.erase(std::remove_if(items_.begin(), items_.end(),
                                [](const VariantToast& t) { return t.age_ms >= kLifeMs; }), items_.end());
}

std::vector<VariantToastView> VariantToastQueue::views() const {
    std::vector<VariantToastView> out;
    for (auto it = items_.rbegin(); it != items_.rend(); ++it) {
        float shown = 1.f;
        if (it->age_ms < kSlideInMs) shown = ease(float(it->age_ms / kSlideInMs));
        else if (it->age_ms > kSlideInMs + kHoldMs) shown = 1.f - ease(float((it->age_ms - kSlideInMs - kHoldMs) / kSlideOutMs));
        out.push_back({&*it, float(it->slot), shown});
    }
    return out;
}

int pixel_text_width(const char* s, int scale) { return int(std::strlen(s)) * 8 * scale; }

int toast_unit(int, int game_height) { return std::max(1, game_height / 360); }

int toast_icon(int unit) { return 13 * unit; }

// Lives-box (TV) edges in native game pixels, measured from the retail HUD art in a
// 16:9 arena frame: the P1 box covers x 15..63, the P2 box x 482..530, both y 9..54.
// Both track the lives digit X (WAVES.ASM:3415/3394: P1 34 wide / 30 classic, P2
// 502 / 361), so the box edge is that digit X +30 (P1) or -20 (P2).
ToastHudEdge toast_hud_edge(int player, bool wide) {
    if (player) return {(wide ? 502 : 361) - 20, 9};
    return {(wide ? 34 : 30) + 30, 9};
}

ToastPlacement toast_place(int player, bool wide, int frame_w, int frame_h, int x0, int y0, int w, int h,
                           int unit, int card_width) {
    const ToastHudEdge e = toast_hud_edge(player, wide);
    const int gap = 2 * std::max(1, unit);
    const int centre = x0 + w / 2;
    ToastPlacement p;
    p.y = y0 + int((std::int64_t(e.top) * h + frame_h / 2) / frame_h);   // flush with the box top
    if (player) {
        // Floor: the card's right edge never reaches the P2 box's first column.
        const int right = x0 + int((std::int64_t(e.x) * w) / frame_w) - gap;
        p.max_width = std::max(0, right - (centre + gap));
        p.x = right - card_width;
    } else {
        // Ceil: the card's left edge always clears the P1 box's last column.
        const int left = x0 + int((std::int64_t(e.x) * w + frame_w - 1) / frame_w) + gap;
        p.max_width = std::max(0, (centre - gap) - left);
        p.x = left;
    }
    return p;
}

ToastLayout toast_layout(const VariantToast& t, int unit, int max_width) {
    ToastLayout l;
    for (l.unit = std::max(1, unit);; --l.unit) {
        // One integer step above the base unit (title 4x / blurb 3x at 1080p).
        l.title_scale = l.unit + 1;
        l.blurb_scale = l.unit;
        l.pad = 3 * l.unit;
        l.gap = 2 * l.unit;
        const int icon = toast_icon(l.unit);
        const int tag = pixel_text_width("P1", l.title_scale);
        const int title_row = pixel_text_width(t.name, l.title_scale) + 4 * l.unit + tag;
        const int blurb_row = pixel_text_width(t.blurb, l.blurb_scale);
        l.width = l.pad + icon + l.pad + std::max(title_row, blurb_row) + l.pad;
        l.height = 2 * l.pad + 8 * l.title_scale + (*t.blurb ? l.gap + 8 * l.blurb_scale : 0);
        if (l.width <= max_width || l.unit == 1) break;
    }
    return l;
}

void render_toast(const VariantToast& t, const ToastLayout& l, bool keyed, std::vector<std::uint32_t>& px) {
    const std::uint32_t body = keyed ? kToastKeyGlass : kCard;
    px.assign(std::size_t(l.width) * std::size_t(l.height), body);
    // Pixel-art rounded corners: cut 2 units off the first row, 1 off the second.
    const int u = l.unit;
    const std::uint32_t cut = keyed ? kToastKeyClear : kCard;
    for (int row = 0; row < 2 * u; ++row) {
        const int w = row < u ? 2 * u : u;
        fill(px, l, 0, row, w, 1, cut);
        fill(px, l, l.width - w, row, w, 1, cut);
        fill(px, l, 0, l.height - 1 - row, w, 1, cut);
        fill(px, l, l.width - w, l.height - 1 - row, w, 1, cut);
    }
    // Opaque edge so the card reads on any background.
    const std::uint32_t edge = 0x2B3550;
    fill(px, l, 2 * u, 0, l.width - 4 * u, u, edge);
    fill(px, l, 2 * u, l.height - u, l.width - 4 * u, u, edge);
    fill(px, l, 0, 2 * u, u, l.height - 4 * u, edge);
    fill(px, l, l.width - u, 2 * u, u, l.height - 4 * u, edge);
    // Tinted icon square with a highlight pixel block.
    const int icon = toast_icon(u), ix = l.pad, iy = (l.height - icon) / 2;
    fill(px, l, ix, iy, icon, icon, 0x05080F);
    fill(px, l, ix + u, iy + u, icon - 2 * u, icon - 2 * u, t.tint);
    fill(px, l, ix + 2 * u, iy + 2 * u, 2 * u, 2 * u, 0xFFFFFF);
    const int tx = l.pad + icon + l.pad;
    const char* tag = t.player ? "P2" : "P1";
    text(px, l, tx, l.pad, t.name, l.title_scale, kText);
    text(px, l, l.width - l.pad - pixel_text_width(tag, l.title_scale), l.pad, tag, l.title_scale,
         t.player ? kToastP2 : kToastP1);
    if (*t.blurb) text(px, l, tx, l.pad + 8 * l.title_scale + l.gap, t.blurb, l.blurb_scale, kDim);
}

}  // namespace stv::host
