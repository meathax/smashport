#pragma once
// Native Y-unit platform services for translated L8 code (PLAN D3/D5/D6).
// Memory map, DMA blitter, palette, CMOS, control latch, input ports, sound
// latch and the TMS34010 I/O registers are native C++. Timing follows the MAME
// 0.289 midyunit/tms34010/screen model in integer picoseconds so interrupt
// points and HCOUNT reads can be compared with reference recordings.
#include "stv/gsp/core.hpp"
#include "stv/gsp/object_lifecycle.hpp"
#include <array>
#include <functional>
#include <string>
#include <vector>

namespace stv::gsp {

// Native enhancement data has its own virtual address range. It is never
// substituted for physical program-ROM reads (including the original ROM test).
// Keeping the sign bit preserves the original table-pointer N/Z behavior.
constexpr u32 ENHANCED_DATA_BASE = 0xFF000000u;
constexpr u32 ENHANCED_DATA_END = 0xFF800000u;
constexpr u32 enhanced_data_address(u32 original) { return original - 0x00800000u; }

constexpr u64 PS_PER_CYCLE = 200000;           // TMS34010: 40 MHz / 8
constexpr u64 PS_PER_PIXEL = 125000;           // screen clock 8 MHz
constexpr int SCREEN_W = 506, SCREEN_H = 289;  // MAME set_raw(8MHz, 506, 90, 500, 289, 20, 276)
constexpr int VIS_MINX = 90, VIS_MAXX = 499, VIS_MINY = 20, VIS_MAXY = 275;
constexpr u64 PS_PER_LINE = PS_PER_PIXEL * SCREEN_W;
constexpr u64 PS_PER_FRAME = PS_PER_LINE * SCREEN_H;
constexpr u64 PS_VBLANK_PERIOD = PS_PER_LINE * (SCREEN_H - (VIS_MAXY - VIS_MINY + 1));

enum IoReg { HESYNC, HEBLNK, HSBLNK, HTOTAL, VESYNC, VEBLNK, VSBLNK, VTOTAL, DPYCTL, DPYSTRT, DPYINT, CONTROL,
             HSTDATA, HSTADRL, HSTADRH, HSTCTLL, HSTCTLH, INTENB, INTPEND, CONVSP, CONVDP, PSIZE, PMASK,
             DPYTAP = 27, HCOUNT_R = 28, VCOUNT_R = 29, DPYADR = 30, REFCNT = 31 };
enum : u16 { INT_X1 = 0x0002, INT_X2 = 0x0004, INT_HI = 0x0200, INT_DI = 0x0400, INT_WV = 0x0800 };
enum { DMA_COMMAND, DMA_ROWBYTES, DMA_OFFSETLO, DMA_OFFSETHI, DMA_XSTART, DMA_YSTART, DMA_WIDTH, DMA_HEIGHT,
       DMA_PALETTE, DMA_COLOR };

struct Fault {
    std::string what;
    u32 addr{};
    u64 cycle{};
};

// Differential-mode hooks: recorded reference values replace native sources.
struct Replay {
    virtual ~Replay() = default;
    virtual bool hcount(u16& v) = 0;
    virtual bool input(u32 bitaddr, u16& v) = 0;
};

struct Machine {
    Cpu c;
    // memories
    std::vector<u16> ram = std::vector<u16>(0x20000 / 2);       // 0x01000000
    std::vector<u16> palette = std::vector<u16>(0x4000 / 2);    // 0x01800000
    std::vector<u32> pens = std::vector<u32>(0x1000);           // RGB888 of palette & 0x0fff
    std::vector<u16> cmos = std::vector<u16>(0x4000);           // 4 pages of 0x1000 words
    std::vector<u16> vram = std::vector<u16>(0x80000 / 2);      // local_videoram
    std::vector<u8> gfx;                                        // decoded 6bpp gfx, base 0x02000000
    std::vector<u16> prog;                                      // immutable imported ROM: 0xFF800000..
    std::vector<u16> enhanced_data;                             // native tables: 0xFF000000..0xFF7FFFFF
    std::vector<u8> snd_u4, snd_u19, snd_u20;                   // sound board ROMs (PLAN P6)
    // Identifies the contents of prog, enhanced_data, gfx and the sound ROMs.
    // Every replacement takes a new process-unique value; copies share it, so
    // a host fork can keep its own copy of this immutable data. Zero: unknown.
    // Not machine state: snapshots neither carry nor compare it.
    u64 static_revision{};
    void static_data_replaced();
    // Presentation only (not machine state): the retail spawn anchor of each
    // foreground object slot at INSOBJ, OXVAL/OYVAL plus its animation anchor,
    // in world pixels. Muzzle effects start from the real firing point.
    struct SpawnAnchor { s16 x{}, y{}; bool valid{}; };
    std::array<SpawnAnchor, 420> spawn_anchor{};
    void object_inserted(u32 node);
    std::vector<u16> frame = std::vector<u16>(512 * SCREEN_H);  // pen indices per displayed line (vram_w wide)
    // Video line width. reference_l8: 512 (the Y-unit VRAM). enhanced_l8_16_9: a wider
    // line so DMA can draw past column 511, and vis_extra more displayed columns (PLAN D7).
    // Presentation-only coverage follows DMA writes and beam scanout. The game
    // and snapshots continue to see the original VRAM words and pen indices.
    std::vector<u8> sprite_coverage, display_queue_coverage;
    std::vector<u16> presentation_frame;
    void enable_sprite_coverage();
    u32 vram_w{512};
    u32 vis_extra{0};
    u32 frame_vis_extra{0};   // scene width latched at the first visible scanline
    void set_wide(u32 line_width, u32 extra_columns);
    std::size_t vidx(std::size_t linear) const { return vram_w == 512 ? linear : (linear >> 9) * vram_w + (linear & 511); }
    // Enhanced host input and immutable launch bearings by retail process pool slot.
    void analog_walk_facing();
    void analog_velocity();
    void boost_enemy_motion();
    std::array<u16,2> move_inputs{};
    std::array<u16,2> aim_inputs{};
    std::array<u16,240> shot_aim{};
    std::array<u32,240> shot_muzzles{}; // retained for existing enhanced snapshot compatibility
    void latch_shot_aim();
    void bind_shot_compass();
    void queue_aimed_sprite();
    static constexpr u32 aim_graphics_base = 0x02000000; // byte offset in virtual DMA graphics
    void draw_aimed_sprite(u32 offset, int width, int height, int x, int y, int stride, u16 palette);
    void aim_launch(bool direct);
    void aim_grenade_step(bool vertical);
    // devices
    std::array<u16, 32> io{};
    std::array<u16, 16> dma{};
    u32 cmos_page{};
    bool videobank_select{};
    bool autoerase_enable{};
    std::array<u16, 6> ports{0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff};
    u32 convsp{}, convdp{}, pixelshift{};
    int gfxcycles{};           // remaining cycles of an interrupted FILL/PIXBLT (ST P flag)
    // screen/timers (picoseconds)
    u64 vblank_start_ps{};
    u64 scan_next_ps{};
    int scan_param{};
    u64 dma_done_ps{~0ull};
    u64 vblank_next_ps{};
    u64 vblank_end_ps{~0ull};
    u64 next_event_cycle{};
    u64 frame_number{};        // MAME screen frame_number(): increments at vblank end
    bool stop{};               // set by fatal faults
    bool enhanced{};           // enhanced_l8_16_9 profile: patch points take their enhanced values (PLAN D7)
    // diagnostics / recording hooks
    std::vector<Fault> faults;
    u64 big_field_quirks{};
    Replay* replay{};
    std::function<void(u32, u16)> on_dma_write, on_sound_write, on_input_read;
    std::function<void()> on_boundary;   // called once at the next instruction boundary, then cleared
    std::function<void(u64)> on_timer_fired;   // a main-side timer (scanline, vblank, DMA) fired; its time in ps
    std::function<void(u64)> on_timer_armed;   // the DMA completion timer was armed for this time (ps)
    std::function<void(u64)> on_sync;          // a synchronize() at this time (ps) from main CPU context
    u64 run_until_cycle{~0ull};          // l8_run stops at the first instruction boundary at or after this
    std::function<void(u16)> on_hcount_read;
    std::function<void(ObjectEvent,u32,u32)> on_object; // subject and optional contact partner
    std::function<void()> on_vblank;       // MAME Lua frame_done point (vblank begin)

    u64 now_ps() const { return c.cyc * PS_PER_CYCLE; }
    u16 rw(u32 bitaddr);
    void ww(u32 bitaddr, u16 v);
    void note_big_field_quirk(u32 a, int size);
    void fault(const char* what, u32 addr);

    bool load_roms(const std::string& zip_path, std::string& error);
    // snapshot at an instruction boundary (src/gsp/state.cpp); pc is the next instruction
    void save_state(std::vector<u8>& out, u32 pc, bool with_prog = true) const;
    bool load_state(const std::vector<u8>& in, u32& pc);
    void reset();
    u32 reset_vector() const;
    // Fire all timer events due at the current cycle count, then apply MAME's
    // slice-start interrupt check; returns the PC to continue with.
    u32 boundary(u32 next);
    u32 check_interrupt(u32 next);   // MAME check_interrupt(); returns vector or 'next'
    void force_boundary() { next_event_cycle = 0; check_requested = true; }
    // Machine-scheduler mode (stv::game::CoScheduler): timers are fired only at
    // timeslice ends through fire_timers_through(), and interrupts are checked at
    // l8_run entry (execute_run) or when an I/O write requests it, as in MAME.
    bool external_timers{};
    bool check_requested{};
    void recompute_next_event();
    void fire_timers_through(u64 ps);   // fire every main-side timer expiring at or before ps

private:
    void fire_timer(u64 t);
    u16 io_read(u32 idx);
    void io_write(u32 idx, u16 v);
    void dma_write(u32 idx, u16 v);
    void dma_draw(u16 command, int width, int height, int xpos, int ypos, s32 rowbytes, u32 offset, u16 pal, u16 color);
    void scanline_callback(int vcount);
    void render_line(int scanline);
    void autoerase_line(int scanline);
    int vpos_at(u64 ps) const;
    int hpos_at(u64 ps) const;
    u64 time_until_pos(u64 now, int vpos, int hpos) const;
};

// enhanced_l8_16_9 data patch layer (src/gsp/enhanced.cpp): widen every arena by `widen` columns.
// A false result is an import/configuration failure; callers must not reset/run it.
[[nodiscard]] bool apply_enhanced(Machine& m, int widen);

// Delayed callbacks retain numeric process/object addresses. Require current
// actor ownership before applying their enhanced-profile writes.
bool enhanced_tank_reaction_parent(const Machine& m);
bool enhanced_mutoid_lineup_parent(const Machine& m);

} // namespace stv::gsp
