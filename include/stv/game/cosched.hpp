// Y-unit machine scheduler: MAME 0.289 device_scheduler::timeslice() for the
// translated main program and the translated sound program (PLAN P6/P7).
//
// Each timeslice ends at the earliest pending timer anywhere in the machine.
// The main CPU runs first; a main write that creates the head timer aborts its
// slice after the current instruction and lowers the target to its local time.
// The sound CPU then runs to the target; a synchronize() it makes that becomes
// the head aborts it at its next suspend check. Timers due at the new base time
// then fire in expiry order. This decides exactly where each main->sound write,
// YM2151 timer and interrupt line change lands in the sound program.
#pragma once

#include "stv/gsp/ops.hpp"
#include "stv/snd/board.hpp"

#include <functional>
#include <vector>

namespace stv::game {

using snd::Atto;
using gsp::u16;
using gsp::u32;
using gsp::u64;

class CoScheduler : public snd::SliceHost {
public:
    CoScheduler(gsp::Machine& m, snd::Board& s);

    // Copy the stopped live scheduler after binding fresh prediction boards.
    void copy_prediction_timing(const CoScheduler& source);
    // Exact timing state for an online join (same fields as copy_prediction_timing).
    void save_state(std::vector<uint8_t>& out) const;
    bool load_state(const uint8_t*& p, const uint8_t* end);

    // Begin at power-on (both CPUs at time zero); `main_pc` is the main reset vector.
    void start(u32 main_pc);
    // Execute timeslices until request_stop() (or a fault). The stop takes effect at
    // the sound CPU's next instruction boundary, so timing is unaffected and
    // resume() continues exactly where run() returned.
    void run();
    void resume();
    void request_stop() { stop_ = true; s_.stop = true; }

    // Call from Machine::on_sound_write for 0x01E00000 writes (main CPU context).
    void main_sound_write(u16 data);

    // After each slice end, with the new base time (all timers up to it have fired).
    std::function<void(const Atto&)> on_base;

    Atto base() const { return base_; }
    Atto target() const { return target_; }   // read-only bounded resync diagnostics
    u32 main_pc() const { return main_pc_; }
    void set_main_pc(u32 pc) { main_pc_ = pc; }   // after loading a main-machine snapshot (online resync)

    static Atto from_ps(u64 ps) { return Atto{int64_t(ps / 1000000000000ull), int64_t(ps % 1000000000000ull) * 1000000}; }
    static Atto main_time(u64 cyc) { return Atto{int64_t(cyc / 5000000), int64_t(cyc % 5000000) * 200000000000LL}; }

    // snd::SliceHost
    void slice_end() override;
    Atto next_foreign_timer() const override;

private:
    void abort_if_head(const Atto& t, bool dma_pending_counts);
    void start_slice();
    void end_slice_now();
    Atto next_main_timer() const;

    gsp::Machine& m_;
    snd::Board& s_;
    Atto base_{};
    Atto target_{};
    u32 main_pc_{};
    u64 main_sync_ps_{~0ull};        // first queued main IRQ-line synchronization
    bool main_abort_{};
    bool stop_{};
    std::vector<u64> fired_ps_;   // main timers the native core already fired but MAME's base has not reached
};

}  // namespace stv::game
