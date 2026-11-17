#include "stv/game/cosched.hpp"
#include "stv/snd/state_stream.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace stv::game {

namespace {
constexpr Atto kNever{INT64_MAX, 0};
constexpr int64_t kStreamUpdateAs = 20000000000000000LL;   // sound_manager: attotime::from_hz(50)
constexpr int64_t kQuantumAs = 1000000000000000000LL / 60;  // default maximum quantum, from_hz(60)

Atto min_atto(const Atto& a, const Atto& b) { return b < a ? b : a; }
Atto max_atto(const Atto& a, const Atto& b) { return a < b ? b : a; }
u64 main_cycles_floor(const Atto& t) { return u64(t.sec) * 5000000ull + u64(t.as / 200000000000LL); }
Atto add_as(const Atto& t, int64_t as) {
    Atto r{t.sec, t.as + as};
    while (r.as >= Atto::kPerSec) { r.as -= Atto::kPerSec; ++r.sec; }
    return r;
}
// Development trace: STV_SCHED_DBG=<sound cycle> prints slices near that point.
u64 dbg_from() {
    static const u64 v = [] {
        const char* e = std::getenv("STV_SCHED_DBG");
        return e ? std::strtoull(e, nullptr, 10) : ~0ull;
    }();
    return v;
}
}  // namespace

CoScheduler::CoScheduler(gsp::Machine& m, snd::Board& s) : m_(m), s_(s) {
    s_.host = this;
    m_.external_timers = true;
    m_.on_timer_fired = [this](u64 ps) {
        if (base_ < from_ps(ps)) fired_ps_.push_back(ps);
    };
    // A DMA write arms the DMA-done timer from main CPU context; if it is the new
    // head of the timer list, MAME aborts the main CPU's timeslice.
    m_.on_timer_armed = [this](u64 ps) { abort_if_head(from_ps(ps), false); };
    // A synchronize() from main CPU context (DMA_COMMAND writes clear the main IRQ line
    // through set_input_line) queues a timer at the current time: it heads the list
    // unless an earlier timer is pending, and then aborts the main CPU's timeslice.
    m_.on_sync = [this](u64 ps) {
        // device_input::set_state_synced keeps one callback for a pending line
        // queue. Retain its deadline even if an earlier sound event prevents
        // this write from heading the list, or later shortens the timeslice.
        if (main_sync_ps_ != ~0ull) return;
        abort_if_head(from_ps(ps), true);
        main_sync_ps_ = ps;
    };
}

void CoScheduler::copy_prediction_timing(const CoScheduler& source) {
    base_ = source.base_;
    target_ = source.target_;
    main_pc_ = source.main_pc_;
    main_sync_ps_ = source.main_sync_ps_;
    main_abort_ = source.main_abort_;
    stop_ = source.stop_;
    fired_ps_ = source.fired_ps_;
}

void CoScheduler::save_state(std::vector<uint8_t>& out) const {
    snd::StateOut o{out};
    o.pod(base_); o.pod(target_); o.pod(main_pc_); o.pod(main_sync_ps_); o.pod(main_abort_); o.pod(stop_); o.vec(fired_ps_);
}
bool CoScheduler::load_state(const uint8_t*& p, const uint8_t* end) {
    snd::StateIn i{p, end};
    Atto b, t; u32 pc; u64 sync; bool abort, stop; std::vector<u64> fired;
    i.pod(b); i.pod(t); i.pod(pc); i.pod(sync); i.pod(abort); i.pod(stop); i.vec(fired, 4096);
    if (!i.ok) return false;
    base_ = b; target_ = t; main_pc_ = pc; main_sync_ps_ = sync; main_abort_ = abort; stop_ = stop; fired_ps_ = std::move(fired);
    p = i.p;
    return true;
}

void CoScheduler::abort_if_head(const Atto& t, bool dma_pending_counts) {
    Atto others = min_atto(s_.next_event_time(), add_as(base_, kQuantumAs));
    if (main_sync_ps_ != ~0ull) others = min_atto(others, from_ps(main_sync_ps_));
    if (dma_pending_counts && m_.dma_done_ps != ~0ull && base_ < from_ps(m_.dma_done_ps))
        others = min_atto(others, from_ps(m_.dma_done_ps));
    for (u64 f : fired_ps_) others = min_atto(others, from_ps(f));
    for (u64 v : {m_.scan_next_ps, m_.vblank_next_ps, m_.vblank_end_ps})
        if (base_ < from_ps(v)) others = min_atto(others, from_ps(v));
    const int64_t k = base_.sec * 50 + base_.as / kStreamUpdateAs;
    others = min_atto(others, Atto{(k + 1) / 50, ((k + 1) % 50) * kStreamUpdateAs});
    if (t < others) {
        main_abort_ = true;
        m_.run_until_cycle = 0;
        m_.recompute_next_event();
    }
}

Atto CoScheduler::next_main_timer() const {
    Atto t = kNever;
    if (main_sync_ps_ != ~0ull) t = from_ps(main_sync_ps_);
    for (u64 v : {m_.scan_next_ps, m_.vblank_next_ps, m_.vblank_end_ps, m_.dma_done_ps})
        if (v != ~0ull && base_ < from_ps(v)) t = min_atto(t, from_ps(v));
    for (u64 f : fired_ps_) t = min_atto(t, from_ps(f));
    const int64_t k = base_.sec * 50 + base_.as / kStreamUpdateAs;   // periodic 50 Hz stream update
    t = min_atto(t, Atto{(k + 1) / 50, ((k + 1) % 50) * kStreamUpdateAs});
    return t;
}

Atto CoScheduler::next_foreign_timer() const { return next_main_timer(); }

void CoScheduler::main_sound_write(u16 data) {
    if (s_.main_sound_write(main_time(m_.c.cyc), data)) {
        main_abort_ = true;                    // abort_timeslice(): stop after this instruction
        m_.run_until_cycle = 0;
        m_.recompute_next_event();
    }
}

void CoScheduler::start(u32 main_pc) {
    main_pc_ = main_pc;
    base_ = Atto{};
    fired_ps_.clear();
    main_sync_ps_ = ~0ull;
    stop_ = false;
    s_.stop = false;
    start_slice();
}

void CoScheduler::run() {
    s_.run();
}

void CoScheduler::resume() {
    stop_ = false;
    s_.stop = false;
    s_.run();
}

void CoScheduler::end_slice_now() {
    base_ = target_;
    // main-side timers due by the new base fire now, as in execute_timers()
    const u64 base_ps = u64(base_.sec) * 1000000000000ull + u64(base_.as / 1000000);
    m_.fire_timers_through(base_ps);
    if (main_sync_ps_ != ~0ull && main_sync_ps_ <= base_ps) main_sync_ps_ = ~0ull;
    fired_ps_.erase(std::remove_if(fired_ps_.begin(), fired_ps_.end(), [&](u64 f) { return !(base_ < from_ps(f)); }),
                    fired_ps_.end());
    s_.fire_until(base_);
    if (on_base) on_base(base_);
}

void CoScheduler::start_slice() {
    for (;;) {
        if (m_.stop) {                    // main CPU fault: nothing can advance any more
            s_.stop = true;
            s_.cpu.due = ~0ull;
            return;
        }
        Atto t = min_atto(min_atto(next_main_timer(), s_.next_event_time()), add_as(base_, kQuantumAs));
        if (dbg_from() != ~0ull) {
            const u64 from = dbg_from();
            const u64 bc = snd::cyc_floor(base_);
            if (bc >= from && bc <= from + 2000)
                std::printf("START base=%.3f scan=%.3f vbn=%.3f vbe=%.3f dma=%.3f fired=%zu t=%.3f main_cyc=%.3f\n",
                            double(base_.sec) * 2e6 + double(base_.as) / 5e11, m_.scan_next_ps / 500000.0,
                            m_.vblank_next_ps / 500000.0, m_.vblank_end_ps / 500000.0, m_.dma_done_ps / 500000.0,
                            fired_ps_.size(), double(t.sec) * 2e6 + double(t.as) / 5e11, double(m_.c.cyc) * 0.4);
        }
        // main CPU first
        const u64 mt = main_cycles_floor(t);
        if (m_.c.cyc < mt) {
            main_abort_ = false;
            main_pc_ = gsp::l8_run(m_, main_pc_, mt);
            if (main_abort_) {
                const Atto lm = main_time(m_.c.cyc);
                if (lm < t) t = max_atto(lm, base_);
                main_abort_ = false;
            }
        }
        target_ = t;
        const u64 st = snd::cyc_floor(t);
        if (dbg_from() != ~0ull) {
            const u64 from = dbg_from();
            if (st >= from && st <= from + 2000)
                std::printf("SLICE base=%.3f target=%.3f main_next=%.3f snd_next=%.3f main_cyc=%.3f snd_cyc=%llu\n",
                            double(base_.sec) * 2e6 + double(base_.as) / 5e11, double(t.sec) * 2e6 + double(t.as) / 5e11,
                            double(next_main_timer().sec) * 2e6 + double(next_main_timer().as) / 5e11,
                            double(s_.next_event_time().sec) * 2e6 + double(s_.next_event_time().as) / 5e11,
                            double(m_.c.cyc) * 0.4, (unsigned long long)s_.cpu.cyc);
        }
        if (s_.halted) {                  // suspended, eating cycles
            if (s_.cpu.cyc < st) s_.cpu.cyc = st;
            end_slice_now();
            continue;
        }
        if (s_.cpu.cyc >= st) {           // less than one cycle to run: the sound CPU sits this slice out
            end_slice_now();
            continue;
        }
        s_.cpu.due = st;
        return;
    }
}

void CoScheduler::slice_end() {
    if (s_.aborted) {
        s_.aborted = false;
        const Atto ls = snd::cyc_time(s_.cpu.cyc);
        if (ls < target_) target_ = max_atto(ls, base_);
    }
    end_slice_now();
    start_slice();
}

}  // namespace stv::game
