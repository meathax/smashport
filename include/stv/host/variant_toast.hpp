#pragma once
// Phone-style pickup notification for weapon variants. Presentation only: the
// queue is fed by Machine::on_variant_pickup and advanced on host time; it never
// touches machine state, replays or the game framebuffer. Platform neutral so
// the timing, stacking and raster are unit-testable without a GPU.
#include <cstddef>
#include <cstdint>
#include <vector>

namespace stv::host {

struct VariantToast {
    int player = 0;                // 0 = P1, 1 = P2
    std::uint8_t variant = 0;      // stv::gsp::Variant
    const char* name = "";
    const char* blurb = "";
    std::uint32_t tint = 0;        // 0xRRGGBB
    double age_ms = 0;
    double slot = 0;               // animated stack position, 0 = top
};

struct VariantToastView {
    const VariantToast* toast = nullptr;
    float slot = 0;                // fractional stack position
    float shown = 0;               // 0 fully off-screen .. 1 fully slid in
};

class VariantToastQueue {
public:
    static constexpr int kMaxVisible = 1;   // per player: a second card would reach the score bar
    static constexpr double kSlideInMs = 250, kHoldMs = 2500, kSlideOutMs = 300, kShiftMs = 160;
    static constexpr double kLifeMs = kSlideInMs + kHoldMs + kSlideOutMs;

    // Newest goes on top; a third toast evicts the oldest.
    void push(int player, std::uint8_t variant);
    // dt_ms of host time. Pass 0 while the PC menu is open (toasts wait there).
    void advance(double dt_ms);
    void clear() { items_.clear(); }
    bool active() const { return !items_.empty(); }
    std::vector<VariantToastView> views() const;   // newest first
    std::size_t size() const { return items_.size(); }
private:
    std::vector<VariantToast> items_;   // oldest .. newest
};

struct ToastLayout {
    int unit = 1, title_scale = 1, blurb_scale = 1;
    int width = 0, height = 0, pad = 0, gap = 0;
};
// unit = integer pixel scale of the 8px font, from the displayed game height.
int toast_unit(int game_width, int game_height);
int toast_icon(int unit);
// Card placement beside each player's lives box (P1 to its right, P2 to its left),
// top-aligned with the box. Native edge from the retail HUD; screen mapping uses the
// displayed game image (x0, y0, w, h) showing frame_w x frame_h native pixels.
struct ToastHudEdge { int x; int top; };   // P1: first column right of box; P2: box's first column
ToastHudEdge toast_hud_edge(int player, bool wide);
struct ToastPlacement { int x = 0, y = 0, max_width = 0; };
// Pass card_width 0 to obtain max_width, then again with the laid-out width for x.
ToastPlacement toast_place(int player, bool wide, int frame_w, int frame_h, int x0, int y0, int w, int h,
                           int unit, int card_width);
// Reduces the unit until the card fits in max_width.
ToastLayout toast_layout(const VariantToast&, int unit, int max_width);
// Fills layout.width*layout.height pixels (0x00RRGGBB). keyed=true uses the host
// overlay keys: 0xFF00FF transparent (rounded corners), 0xFF0080 translucent glass.
// keyed=false paints an opaque card (the software presenter has no overlay keys).
void render_toast(const VariantToast&, const ToastLayout&, bool keyed, std::vector<std::uint32_t>& pixels);
// Text width in pixels at an integer scale (8px advance per character).
int pixel_text_width(const char* text, int scale);

inline constexpr std::uint32_t kToastKeyClear = 0xFF00FF, kToastKeyGlass = 0xFF0080;
inline constexpr std::uint32_t kToastP1 = 0x4DA6FF, kToastP2 = 0xFF5050;

}  // namespace stv::host
