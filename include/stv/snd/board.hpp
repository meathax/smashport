// Smash TV sound board: bus, PIA 6821, YM2151 (ymfm), MC1408 DAC latch,
// HC55516 CVSD pins, ROM banking and the MAME scheduler semantics that couple
// them to the translated 6809 program. PLAN P6.
#pragma once

#include "stv/snd/audio.hpp"
#include "stv/snd/ops.hpp"

#include <array>

namespace ymfm { class ym2151; }

namespace stv::snd {

struct Pia6821 {
    uint8_t in_a{0xff}, in_b{0}, out_a{}, out_b{}, ddr_a{}, ddr_b{}, ctl_a{}, ctl_b{};
    bool in_ca1{true}, in_ca2{true}, in_cb1{false}, in_cb2{false};
    bool out_ca2{}, out_cb2{}, last_out_cb2_z{};
    bool irq_a1{}, irq_a2{}, irq_b1{}, irq_b2{};
    int irq_a_state{}, irq_b_state{};
    bool in_b_pushed{};
};

// One board-level observation for the lockstep log: sound CPU I/O accesses.
struct Access {
    uint8_t kind;      // 1 read, 2 write
    Atto t;
    uint16_t addr;
    uint8_t data;
    uint16_t pc;
};

// The machine-level scheduler (MAME device_scheduler::timeslice) that owns the
// timeslices. The sound CPU calls slice_end() when it reaches the current slice
// target (Cpu::due) or aborts its slice; the host fires due timers and starts the
// next slice, running the main CPU first as MAME does.
struct SliceHost {
    virtual ~SliceHost() = default;
    virtual void slice_end() = 0;
    virtual Atto next_foreign_timer() const = 0;   // earliest pending timer outside the sound board
};

struct Board {
    Board();
    ~Board();

    // ROM region laid out as MAME's "cvsd:cpu" (0x90000 bytes, U4/U19/U20 plus reloads).
    bool load_roms(const std::vector<uint8_t>& u4, const std::vector<uint8_t>& u19, const std::vector<uint8_t>& u20,
                   std::string& error);
    void power_on();
    void copy_prediction_state(const Board& source);
    // Exact state for an online join: everything copy_prediction_state copies,
    // except the immutable ROM image (only its protection RAM window).
    void save_state(std::vector<uint8_t>& out) const;
    bool load_state(const uint8_t*& p, const uint8_t* end);

    // Main CPU side (midyunit_cvsd_state::cvsd_sound_w, word access at 0x01E00000),
    // called while the main CPU executes, at its local time t_write. Returns true
    // when the new synchronize timer became the head of the timer list, in which
    // case MAME aborts the main CPU's timeslice after the current instruction.
    bool main_sound_write(const Atto& t_write, uint16_t data);

    // Run the translated sound program; the host drives timeslices until `stop`.
    void run();
    bool stop{};

    // Timer list access for the host.
    Atto next_event_time() const;      // earliest pending sound-board timer, or never
    void fire_until(const Atto& base); // execute_timers(): fire every timer expiring at or before base
    SliceHost* host{};
    bool aborted{};                    // the sound CPU's timeslice was aborted (a synchronize became the head)

    Cpu cpu;
    std::vector<uint8_t> region;       // 0x90000
    std::array<uint8_t, 0x800> ram{};
    uint8_t bank{};
    uint32_t bank_base{0x10000};
    int page_index{0};                 // translated page for bank_base, -1 none
    Pia6821 pia;
    uint8_t dac{};

    bool halted{};
    bool vector_pending{true};         // next step is the reset vector fetch
    std::string fault;

    std::function<void(const Access&)> on_access;
    // Audio: optional mixer fed with the chip streams; the YM2151 stream is always
    // generated (MAME update_streams() before each access) so its state matches.
    Mixer* mixer{};
    Hc55516 cvsd;
    uint64_t ym_samples{};
    void ym_update(const Atto& t);                   // sound_stream::update() to time t

    // bus
    uint8_t read(uint16_t a);
    void write(uint16_t a, uint8_t v);

    // scheduler
    enum Kind : uint8_t { kSyncWrite, kInputLine, kYmMode, kYmCheckIrq, kYmTimer };
    struct Event {
        Atto t;
        uint64_t seq;
        Kind kind;
        int arg0;
        int arg1;
    };
    bool schedule(const Atto& t, Kind k, int a0 = 0, int a1 = 0);   // true if it became the list head
    void sync_now(Kind k, int a0 = 0, int a1 = 0);   // MAME synchronize() at the current time
    bool apply_due();                                // the sound CPU reached a suspend check with cyc >= due
    Atto now() const;

    std::vector<Event> q;              // pending timers, ordered by (time, creation)
    uint64_t seq{};
    bool in_cpu{};                     // a CPU access is in progress (scheduler.currently_executing())
    Atto cb_time;                      // time of the event being applied
    bool reset_hit{};

    // interrupt lines into the 6809 (device_input::empty_event_queue)
    enum Line { kLineIrq = 0, kLineFirq = 1, kLineNmi = 2, kLineReset = 3 };
    void set_input_line(Line l, int state);
    void apply_input(Line l, int state);

    // PIA
    uint8_t pia_read(int off);
    void pia_write(int off, uint8_t v);
    void pia_update_interrupts();
    void pia_ca1_w(bool state);
    void pia_cb1_w(bool state);
    void pia_cb2_w(bool state);
    void pia_set_out_ca2(bool v);
    void pia_set_out_cb2(bool v);
    void pia_send_out_a();

    // YM2151
    struct YmIntf;
    std::unique_ptr<YmIntf> ymi;
    std::unique_ptr<ymfm::ym2151> ym;
    uint8_t ym_reset_state{1};
    Atto ym_busy_end;
    void ym_reset_w(int state);

    void take_interrupt(uint16_t vector);
    uint16_t pending_interrupt() const;
    bool step_vector(uint16_t ea);
    bool push_all(uint8_t mask);
    Fn lookup(uint16_t pc);
    void set_bank(uint8_t v);
    std::vector<std::vector<Fn>> tables;    // 7 pages x 0x8000 (ram: 0x800)
};

}  // namespace stv::snd
