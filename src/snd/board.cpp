// Smash TV sound board runtime. Device behaviour follows MAME 0.289:
// williamssound.cpp (williams_cvsd_sound_device), 6821pia.cpp, ymfm_mame.h /
// ymopm.cpp (YM2151 through ymfm) and the scheduler rules described in ops.hpp.
#include "stv/snd/board.hpp"
#include "stv/snd/state_stream.hpp"

#include "ymfm_opm.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace stv::snd {

namespace {
constexpr uint32_t kYmClock = 3579545;
// Development trace: STV_SND_DBG=<from>,<to> (sound CPU cycles) prints timer activity.
uint64_t g_dbg_from = ~0ull, g_dbg_to = 0;
bool dbg_on(uint64_t cyc) {
    static bool init = false;
    if (!init) {
        init = true;
        if (const char* e = std::getenv("STV_SND_DBG")) std::sscanf(e, "%llu,%llu", (unsigned long long*)&g_dbg_from, (unsigned long long*)&g_dbg_to);
    }
    return cyc >= g_dbg_from && cyc <= g_dbg_to;
}
double tcyc(const Atto& t) { return double(t.sec) * 2e6 + double(t.as) / 5e11; }
}

Atto from_ticks(uint64_t ticks, uint32_t hz) {
    const int64_t apt = Atto::kPerSec / int64_t(hz);
    if (ticks < hz) return Atto{0, int64_t(ticks) * apt};
    const uint32_t rem = uint32_t(ticks % hz);
    return Atto{int64_t(ticks / hz), int64_t(rem) * apt};
}

// ---------------------------------------------------------------------------
// YM2151 interface (MAME ym_generic_device semantics)
// ---------------------------------------------------------------------------
struct Board::YmIntf : public ymfm::ymfm_interface {
    Board& b;
    explicit YmIntf(Board& bd) : b(bd) {}
    void ymfm_sync_mode_write(uint8_t data) override { b.sync_now(kYmMode, data); }
    void ymfm_sync_check_interrupts() override {
        if (b.in_cpu) b.sync_now(kYmCheckIrq);
        else m_engine->engine_check_interrupts();
    }
    void ymfm_set_timer(uint32_t tnum, int32_t clocks) override {
        // emu_timer::adjust / enable(false): the timer leaves its old list position
        b.q.erase(std::remove_if(b.q.begin(), b.q.end(),
                                 [&](const Event& e) { return e.kind == kYmTimer && e.arg0 == int(tnum); }),
                  b.q.end());
        if (clocks >= 0) b.schedule(b.now() + from_ticks(uint64_t(clocks), kYmClock), kYmTimer, int(tnum));
    }
    void ymfm_update_irq(bool asserted) override { b.pia_ca1_w(!asserted); }
    void ymfm_set_busy_end(uint32_t clocks) override { b.ym_busy_end = b.now() + from_ticks(clocks, kYmClock); }
    bool ymfm_is_busy() override { return b.now() < b.ym_busy_end; }
    void timer_expired(uint32_t tnum) { m_engine->engine_timer_expired(tnum); }
    void mode_write(uint8_t data) { m_engine->engine_mode_write(data); }
    void check_interrupts() { m_engine->engine_check_interrupts(); }
};

Board::Board() : region(0x90000, 0) {
    cpu.bd = this;
    ymi = std::make_unique<YmIntf>(*this);
    ym = std::make_unique<ymfm::ym2151>(*ymi);
    tables.resize(7);
    for (int p = 0; p < 7; ++p) {
        tables[p].assign(p == 6 ? 0x800 : 0x8000, nullptr);
        const PageTable& pt = k_pages[p];
        for (size_t i = 0; i < pt.count; ++i) {
            const Entry& e = pt.entries[i];
            if (p == 6) {
                if (e.pc < 0x800) tables[p][e.pc] = e.fn;
            } else if (e.pc >= 0x8000) {
                tables[p][e.pc - 0x8000] = e.fn;
            }
        }
    }
}
Board::~Board() = default;

// The prediction board has its own YM interface, CPU bus and timer queue.
// Its translated dispatch tables are identical. PCM goes only to the live mixer.
void Board::copy_prediction_state(const Board& source) {
    cpu = source.cpu;
    cpu.bd = this;
    ram = source.ram;
    region = source.region;
    bank = source.bank;
    bank_base = source.bank_base;
    page_index = source.page_index;
    pia = source.pia;
    dac = source.dac;
    halted = source.halted;
    vector_pending = source.vector_pending;
    fault = source.fault;
    stop = source.stop;
    aborted = source.aborted;
    cvsd = source.cvsd;
    ym_samples = source.ym_samples;
    q = source.q;
    seq = source.seq;
    in_cpu = source.in_cpu;
    cb_time = source.cb_time;
    reset_hit = source.reset_hit;
    ym_reset_state = source.ym_reset_state;
    ym_busy_end = source.ym_busy_end;
    std::vector<uint8_t> registers;
    ymfm::ymfm_saved_state save(registers, true), restore(registers, false);
    source.ym->save_restore(save);
    ym->save_restore(restore);
}

namespace {
constexpr std::size_t kProtectFirst = 0x10000 + (0x9cf6 - 0x8000), kProtectSize = 0x9d21 - 0x9cf6 + 1;
}
void Board::save_state(std::vector<uint8_t>& out) const {
    StateOut o{out};
    Cpu c = cpu; c.bd = nullptr; o.pod(c);
    o.pod(ram); o.raw(region.data() + kProtectFirst, kProtectSize);
    o.pod(bank); o.pod(bank_base); o.pod(page_index); o.pod(pia); o.pod(dac);
    o.pod(halted); o.pod(vector_pending); o.pod(stop); o.pod(aborted); o.pod(cvsd); o.pod(ym_samples);
    o.vec(q); o.pod(seq); o.pod(in_cpu); o.pod(cb_time); o.pod(reset_hit); o.pod(ym_reset_state); o.pod(ym_busy_end);
    o.pod(uint8_t(fault.empty() ? 0 : 1));
    std::vector<uint8_t> registers;
    ymfm::ymfm_saved_state save(registers, true);
    ym->save_restore(save);
    o.vec(registers);
}
bool Board::load_state(const uint8_t*& p, const uint8_t* end) {
    StateIn i{p, end};
    Cpu c; i.pod(c);
    std::array<uint8_t, 0x800> r{}; i.pod(r);
    uint8_t protect[kProtectSize]; i.raw(protect, kProtectSize);
    uint8_t bk; uint32_t bb; int pi; Pia6821 pa; uint8_t d; bool h, vp, st, ab; Hc55516 cv; uint64_t ys;
    i.pod(bk); i.pod(bb); i.pod(pi); i.pod(pa); i.pod(d); i.pod(h); i.pod(vp); i.pod(st); i.pod(ab); i.pod(cv); i.pod(ys);
    std::vector<Event> events; i.vec(events, 4096);
    uint64_t sq; bool ic; Atto cb; bool rh; uint8_t yr; Atto busy; uint8_t faulted;
    i.pod(sq); i.pod(ic); i.pod(cb); i.pod(rh); i.pod(yr); i.pod(busy); i.pod(faulted);
    std::vector<uint8_t> registers; i.vec(registers, 1u << 20);
    if (!i.ok || faulted || pi < -1 || pi >= int(tables.size()) || bb > region.size()) return false;
    cpu = c; cpu.bd = this; ram = r; std::memcpy(region.data() + kProtectFirst, protect, kProtectSize);
    bank = bk; bank_base = bb; page_index = pi; pia = pa; dac = d; halted = h; vector_pending = vp;
    stop = st; aborted = ab; cvsd = cv; ym_samples = ys; q = std::move(events); seq = sq; in_cpu = ic;
    cb_time = cb; reset_hit = rh; ym_reset_state = yr; ym_busy_end = busy; fault.clear();
    ymfm::ymfm_saved_state restore(registers, false);
    ym->save_restore(restore);
    p = i.p;
    return true;
}

bool Board::load_roms(const std::vector<uint8_t>& u4, const std::vector<uint8_t>& u19,
                      const std::vector<uint8_t>& u20, std::string& error) {
    if (u4.size() != 0x10000 || u19.size() != 0x10000 || u20.size() != 0x10000) {
        error = "sound ROMs must be 64 KiB each (U4, U19, U20)";
        return false;
    }
    std::fill(region.begin(), region.end(), 0);
    std::copy(u4.begin(), u4.end(), region.begin() + 0x10000);
    std::copy(u4.begin(), u4.end(), region.begin() + 0x20000);
    std::copy(u19.begin(), u19.end(), region.begin() + 0x30000);
    std::copy(u19.begin(), u19.end(), region.begin() + 0x40000);
    std::copy(u20.begin(), u20.end(), region.begin() + 0x50000);
    std::copy(u20.begin(), u20.end(), region.begin() + 0x60000);
    return true;
}

void Board::power_on() {
    cpu = Cpu{};
    cpu.bd = this;
    ram.fill(0);
    pia = Pia6821{};
    dac = 0;
    set_bank(0);
    halted = false;
    vector_pending = true;
    fault.clear();
    q.clear();
    cpu.due = 0;
    seq = 0;
    ym_reset_state = 1;
    ym_busy_end = Atto{};
    aborted = false;
    stop = false;
    in_cpu = false;
    cb_time = Atto{};
    ym->reset();                                  // ymfm_device_base::device_reset
    ym_samples = 0;
    cvsd = Hc55516{};
    // pia6821_device::device_reset: port A out 0xff, CA2 high (YM reset line released)
    if (mixer) mixer->dac(Atto{}, 0xff);
    ym_reset_w(1);
    // m6809 device_reset
    cpu.cc |= 0x50;
}

void Board::set_bank(uint8_t v) {
    bank = v & 0x0f;
    const int chip = bank & 3;
    bank_base = 0x10000 + 0x8000 * ((bank >> 2) & 3) + 0x20000 * chip;
    if (chip == 3) page_index = -1;
    else page_index = chip * 2 + ((bank >> 2) & 1);
}

Atto Board::now() const { return in_cpu ? cyc_time(cpu.cyc) : cb_time; }

bool Board::schedule(const Atto& t, Kind k, int a0, int a1) {
    const Event e{t, seq++, k, a0, a1};
    auto it = std::upper_bound(q.begin(), q.end(), e, [](const Event& x, const Event& y) {
        return x.t < y.t || (x.t == y.t && x.seq < y.seq);
    });
    const bool head = it == q.begin() && (!host || t < host->next_foreign_timer());
    if (dbg_on(cpu.cyc))
        std::printf("  [%llu] schedule kind=%d a=%d,%d t=%.3f head=%d in_cpu=%d foreign=%.3f due=%llu\n", (unsigned long long)cpu.cyc, int(k), a0, a1,
                    tcyc(t), int(head), int(in_cpu), host ? tcyc(host->next_foreign_timer()) : -1.0, (unsigned long long)cpu.due);
    q.insert(it, e);
    return head;
}

Atto Board::next_event_time() const {
    return q.empty() ? Atto{INT64_MAX, 0} : q.front().t;
}

// MAME synchronize(): a timer at the current time. Created while the sound CPU
// executes, it aborts the CPU's timeslice when it becomes the list head; the CPU
// then stops at its next suspend check.
void Board::sync_now(Kind k, int a0, int a1) {
    if (schedule(now(), k, a0, a1) && in_cpu) {
        aborted = true;
        cpu.due = cpu.cyc;
    }
}

void Board::fire_until(const Atto& base) {
    const bool saved = in_cpu;
    in_cpu = false;
    while (!q.empty() && !(base < q.front().t)) {
        const Event e = q.front();
        q.erase(q.begin());
        cb_time = e.t;
        if (dbg_on(cpu.cyc))
            std::printf("  [%llu] fire kind=%d a=%d,%d t=%.3f base=%.3f\n", (unsigned long long)cpu.cyc, int(e.kind), e.arg0,
                        e.arg1, tcyc(e.t), tcyc(base));
        switch (e.kind) {
        case kSyncWrite:                                  // williams_cvsd_sound_device::sync_write
            pia.in_b = uint8_t(e.arg0 & 0xff);
            pia.in_b_pushed = true;
            pia_cb1_w((e.arg0 >> 8) & 1);
            pia_cb2_w((e.arg0 >> 9) & 1);
            break;
        case kInputLine:
            apply_input(Line(e.arg0), e.arg1);
            break;
        case kYmMode:
            ymi->mode_write(uint8_t(e.arg0));
            break;
        case kYmCheckIrq:
            ymi->check_interrupts();
            break;
        case kYmTimer:
            ymi->timer_expired(uint32_t(e.arg0));
            break;
        }
    }
    in_cpu = saved;
}

bool Board::apply_due() {
    reset_hit = false;
    host->slice_end();
    const bool r = reset_hit;
    reset_hit = false;
    return r;
}

bool Cpu::service() { return bd->apply_due(); }

// device_execute_interface::set_input_line: queued and applied through a
// synchronize() timer at the current time.
void Board::set_input_line(Line l, int state) { sync_now(kInputLine, l, state); }

void Board::apply_input(Line l, int state) {
    switch (l) {
    case kLineNmi: {
        const bool assert_ = state != 0;
        cpu.nmi_asserted = cpu.nmi_asserted || (assert_ && !cpu.nmi_line && cpu.lds);
        cpu.nmi_line = assert_;
        break;
    }
    case kLineFirq: cpu.firq_line = state != 0; break;
    case kLineIrq: cpu.irq_line = state != 0; break;
    case kLineReset:
        // device_input::empty_event_queue: asserting only suspends the CPU (mid-
        // instruction); releasing it resets the device and resumes it at its
        // whole-cycle local time.
        if (state) {
            if (!halted) reset_hit = true;
            halted = true;
        } else if (halted) {
            cpu.nmi_asserted = false;                     // m6809_base_device::device_reset
            cpu.lds = false;
            cpu.dp = 0;
            cpu.cc |= 0x50;
            vector_pending = true;
            halted = false;
        }
        break;
    }
}

// cvsd_sound_w: reset_write(BIT(~data, 8)); write((data & 0xff) | ((data & 0x200) >> 1)).
// The main CPU is the executing device, so every synchronize() timer is stamped
// with its local time; only the first can become the list head.
bool Board::main_sound_write(const Atto& t_write, uint16_t data) {
    const bool saved = in_cpu;
    in_cpu = false;
    bool head = false;
    if (!(data & 0x100)) {
        set_bank(0);                                              // immediate: the sound CPU runs later in the slice
        head |= schedule(t_write, kInputLine, kLineFirq, 0);       // device_reset clears the lines
        head |= schedule(t_write, kInputLine, kLineIrq, 0);
        head |= schedule(t_write, kInputLine, kLineNmi, 0);
        head |= schedule(t_write, kInputLine, kLineReset, 1);
    } else {
        head |= schedule(t_write, kInputLine, kLineReset, 0);
    }
    head |= schedule(t_write, kSyncWrite, (data & 0xff) | ((data & 0x200) >> 1));
    in_cpu = saved;
    return head;
}

// ---------------------------------------------------------------------------
// Bus
// ---------------------------------------------------------------------------
uint8_t Cpu::rd(uint16_t addr) {
    ++cyc;
    return bd->read(addr);
}

void Cpu::wr(uint16_t addr, uint8_t v) {
    ++cyc;
    bd->write(addr, v);
}

uint8_t Board::read(uint16_t a) {
    if (a < 0x2000) return ram[a & 0x7ff];
    if (a >= 0x8000) return region[bank_base + (a - 0x8000)];
    in_cpu = true;
    uint8_t v = 0;
    if (a < 0x4000) {
        ym_update(now());
        v = ym->read(a & 1);                                          // reads are not gated by reset
    } else if (a < 0x6000) {
        v = pia_read(a & 3);
    }
    in_cpu = false;
    if (on_access) on_access(Access{1, cyc_time(cpu.cyc), a, v, cpu.pc});
    return v;
}

void Board::write(uint16_t a, uint8_t v) {
    if (a < 0x2000) { ram[a & 0x7ff] = v; return; }
    if (a >= 0x8000) {
        if (a >= 0x9cf6 && a <= 0x9d21) {                              // cvsd_protection_w: into bank 0
            region[0x10000 + (a - 0x8000)] = v;
            if (on_access) on_access(Access{2, cyc_time(cpu.cyc), a, v, cpu.pc});
        }
        return;
    }
    if (on_access) on_access(Access{2, cyc_time(cpu.cyc), a, v, cpu.pc});
    in_cpu = true;
    if (a < 0x4000) {
        if (ym_reset_state != 0) {
            ym_update(now());
            ym->write(a & 1, v);
        }
    } else if (a < 0x6000) {
        pia_write(a & 3, v);
    } else if (a < 0x6800) {                                          // cvsd_digit_clock_clear_w
        cvsd.clock_w(false);
        cvsd.digit_w(v & 1);
        if (mixer) mixer->cvsd(now(), cvsd.next_sample);
    } else if (a < 0x7000) {                                          // cvsd_clock_set_w
        cvsd.clock_w(true);
        if (mixer) mixer->cvsd(now(), cvsd.next_sample);
    } else if (a >= 0x7800) {
        set_bank(v);
    }
    in_cpu = false;
}

// ---------------------------------------------------------------------------
// PIA 6821 (MAME pia6821_device)
// ---------------------------------------------------------------------------
namespace {
bool irq1_enabled(uint8_t c) { return c & 0x01; }
bool c1_low_to_high(uint8_t c) { return c & 0x02; }
bool output_selected(uint8_t c) { return c & 0x04; }
bool irq2_enabled(uint8_t c) { return c & 0x08; }
bool strobe_e_reset(uint8_t c) { return c & 0x08; }
bool strobe_c1_reset(uint8_t c) { return !(c & 0x08); }
bool c2_set(uint8_t c) { return c & 0x08; }
bool c2_low_to_high(uint8_t c) { return c & 0x10; }
bool c2_set_mode(uint8_t c) { return c & 0x10; }
bool c2_strobe_mode(uint8_t c) { return !(c & 0x10); }
bool c2_output(uint8_t c) { return c & 0x20; }
bool c2_input(uint8_t c) { return !(c & 0x20); }
}  // namespace

void Board::pia_update_interrupts() {
    int s = (pia.irq_a1 && irq1_enabled(pia.ctl_a)) || (pia.irq_a2 && irq2_enabled(pia.ctl_a));
    if (s != pia.irq_a_state) {
        pia.irq_a_state = s;
        set_input_line(kLineFirq, s);
    }
    s = (pia.irq_b1 && irq1_enabled(pia.ctl_b)) || (pia.irq_b2 && irq2_enabled(pia.ctl_b));
    if (s != pia.irq_b_state) {
        pia.irq_b_state = s;
        set_input_line(kLineNmi, s);
    }
}

void Board::pia_set_out_ca2(bool v) {
    pia.out_ca2 = v;
    ym_reset_w(v ? 1 : 0);
}

void Board::pia_set_out_cb2(bool v) {
    const bool z = !c2_output(pia.ctl_b);
    if (v != pia.out_cb2 || z != pia.last_out_cb2_z) {
        pia.out_cb2 = v;
        pia.last_out_cb2_z = z;
    }
}

void Board::pia_send_out_a() {
    uint8_t v;
    if (pia.ddr_a == 0xff) v = pia.out_a;
    else {
        const uint8_t in = uint8_t((~pia.ddr_a & 0xff) | (pia.ddr_a & pia.out_a));  // port A inputs float high
        v = uint8_t((pia.out_a & pia.ddr_a) | (in & ~pia.ddr_a));
    }
    dac = v;
    if (mixer) mixer->dac(now(), v);
}

uint8_t Board::pia_read(int off) {
    switch (off & 3) {
    case 0:
        if (output_selected(pia.ctl_a)) {
            const uint8_t in = 0xff;                           // no port A input handler: 0xff
            const uint8_t ret = uint8_t((~pia.ddr_a & in) | (pia.ddr_a & pia.out_a));
            pia.irq_a1 = pia.irq_a2 = false;
            pia_update_interrupts();
            if (c2_output(pia.ctl_a) && c2_strobe_mode(pia.ctl_a)) {
                if (pia.out_ca2) pia_set_out_ca2(false);
                if (strobe_e_reset(pia.ctl_a)) pia_set_out_ca2(true);
            }
            return ret;
        }
        return pia.ddr_a;
    case 1: {
        uint8_t ret = pia.ctl_a;
        if (pia.irq_a1) ret |= 0x80;
        if (pia.irq_a2 && c2_input(pia.ctl_a)) ret |= 0x40;
        return ret;
    }
    case 2:
        if (output_selected(pia.ctl_b)) {
            uint8_t ret;
            if (pia.ddr_b == 0xff) ret = pia.out_b;
            else {
                const uint8_t in = pia.in_b_pushed ? pia.in_b : 0x00;
                ret = uint8_t((pia.out_b & pia.ddr_b) | (in & ~pia.ddr_b));
            }
            if (pia.irq_b1 && c2_strobe_mode(pia.ctl_b) && strobe_c1_reset(pia.ctl_b)) pia_set_out_cb2(true);
            pia.irq_b1 = pia.irq_b2 = false;
            pia_update_interrupts();
            return ret;
        }
        return pia.ddr_b;
    default: {
        uint8_t ret = pia.ctl_b;
        if (pia.irq_b1) ret |= 0x80;
        if (pia.irq_b2 && c2_input(pia.ctl_b)) ret |= 0x40;
        return ret;
    }
    }
}

void Board::pia_write(int off, uint8_t v) {
    switch (off & 3) {
    case 0:
        if (output_selected(pia.ctl_a)) {
            pia.out_a = v;
            pia_send_out_a();
        } else if (pia.ddr_a != v) {
            pia.ddr_a = v;
            pia_send_out_a();
        }
        break;
    case 1: {
        v &= 0x3f;
        const bool was_output = c2_output(pia.ctl_a);
        pia.ctl_a = v;
        if (c2_output(pia.ctl_a)) {
            if (c2_set_mode(pia.ctl_a)) {
                const bool set = c2_set(pia.ctl_a);
                if (!was_output || pia.out_ca2 != set) pia_set_out_ca2(set);
            } else if (!was_output || !pia.out_ca2) {
                pia_set_out_ca2(true);
            }
        } else if (was_output) {
            ym_reset_w(1);
        }
        pia_update_interrupts();
        break;
    }
    case 2:
        if (output_selected(pia.ctl_b)) {
            pia.out_b = v;                                    // talkback latch (not read by the Y-unit)
            if (c2_strobe_mode(pia.ctl_b)) {
                pia_set_out_cb2(false);
                if (strobe_e_reset(pia.ctl_b)) pia_set_out_cb2(true);
            }
        } else if (pia.ddr_b != v) {
            pia.ddr_b = v;
        }
        break;
    default:
        v &= 0x3f;
        pia.ctl_b = v;
        pia_set_out_cb2(c2_set_mode(pia.ctl_b) ? c2_set(pia.ctl_b) : true);
        pia_update_interrupts();
        break;
    }
}

void Board::pia_ca1_w(bool state) {
    if (pia.in_ca1 != state && ((state && c1_low_to_high(pia.ctl_a)) || (!state && !c1_low_to_high(pia.ctl_a)))) {
        pia.irq_a1 = true;
        pia_update_interrupts();
    }
    pia.in_ca1 = state;
}

void Board::pia_cb1_w(bool state) {
    if (pia.in_cb1 != state && ((state && c1_low_to_high(pia.ctl_b)) || (!state && !c1_low_to_high(pia.ctl_b)))) {
        pia.irq_b1 = true;
        pia_update_interrupts();
    }
    pia.in_cb1 = state;
}

void Board::pia_cb2_w(bool state) {
    if (c2_input(pia.ctl_b) && pia.in_cb2 != state &&
        ((state && c2_low_to_high(pia.ctl_b)) || (!state && !c2_low_to_high(pia.ctl_b)))) {
        pia.irq_b2 = true;
        pia_update_interrupts();
    }
    pia.in_cb2 = state;
}

// sound_stream::update(): generate samples 0..floor(t * rate) (none at time zero)
void Board::ym_update(const Atto& t) {
    if (t.sec == 0 && t.as == 0) return;
    const uint64_t last = sample_index(t, kYmRate);
    ymfm::ym2151::output_data o;
    while (ym_samples <= last) {
        ym->generate(&o, 1);
        if (mixer) mixer->ym_sample(o.data[0], o.data[1]);
        ++ym_samples;
    }
}

// ym2151_device::reset_w (active low)
void Board::ym_reset_w(int state) {
    if (state != ym_reset_state) {
        ym_update(now());
        ym_reset_state = uint8_t(state);
        if (state != 0) ym->reset();
    }
}

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------
#define SYB if (cpu.cyc >= cpu.due && apply_due()) return false

uint16_t Board::pending_interrupt() const {
    if (cpu.nmi_asserted) return 0xfffc;
    if (!(cpu.cc & 0x40) && cpu.firq_line) return 0xfff6;
    if (!(cpu.cc & 0x10) && cpu.irq_line) return 0xfff8;
    return 0;
}

// INTERRUPT_VECTOR microcode. As in m6809make.py, the suspend check (SYB)
// follows each '@' step.
bool Board::step_vector(uint16_t ea) {
    cpu.cyc += 1; SYB;
    const uint8_t h = cpu.rd(ea); SYB;
    const uint8_t l = cpu.rd(uint16_t(ea + 1)); SYB;
    cpu.pc = uint16_t((h << 8) | l);
    cpu.cyc += 1; SYB;
    return true;
}

// PUSH_REGISTERS onto S
bool Board::push_all(uint8_t m) {
    Cpu& c = cpu;
    if (m & 0x80) { --c.s; c.wr(c.s, uint8_t(c.pc)); SYB; --c.s; c.wr(c.s, uint8_t(c.pc >> 8)); SYB; }
    if (m & 0x40) { --c.s; c.wr(c.s, uint8_t(c.u)); SYB; --c.s; c.wr(c.s, uint8_t(c.u >> 8)); SYB; }
    if (m & 0x20) { --c.s; c.wr(c.s, uint8_t(c.y)); SYB; --c.s; c.wr(c.s, uint8_t(c.y >> 8)); SYB; }
    if (m & 0x10) { --c.s; c.wr(c.s, uint8_t(c.x)); SYB; --c.s; c.wr(c.s, uint8_t(c.x >> 8)); SYB; }
    if (m & 0x08) { --c.s; c.wr(c.s, c.dp); SYB; }
    if (m & 0x04) { --c.s; c.wr(c.s, c.b); SYB; }
    if (m & 0x02) { --c.s; c.wr(c.s, c.a); SYB; }
    if (m & 0x01) { --c.s; c.wr(c.s, c.cc); SYB; }
    return true;
}

// NMI / FIRQ / IRQ entry (base6x09.lst)
void Board::take_interrupt(uint16_t v) {
    Cpu& c = cpu;
    if (dbg_on(c.cyc)) std::printf("  [%llu] take interrupt %04x pc=%04x\n", (unsigned long long)c.cyc, v, c.pc);
    if (v == 0xfffc) c.nmi_asserted = false;
    c.cyc += 1;                                     // @dummy_read_opcode_arg(0)
    if (c.cyc >= c.due && apply_due()) return;
    c.cyc += 1;                                     // @dummy_read_opcode_arg(0)
    if (c.cyc >= c.due && apply_due()) return;
    c.cyc += 1;                                     // @dummy_vma(1)
    if (c.cyc >= c.due && apply_due()) return;
    if (v == 0xfff6) { c.cc &= ~0x80; if (!push_all(0x81)) return; c.cc |= 0x50; }
    else if (v == 0xfff8) { c.cc |= 0x80; if (!push_all(0xff)) return; c.cc |= 0x10; }
    else { c.cc |= 0x80; if (!push_all(0xff)) return; c.cc |= 0x50; }
    step_vector(v);
}

bool Cpu::sync_wait() {
    Board& b = *bd;
    for (;;) {
        if (cyc >= due && b.apply_due()) return true;
        if (nmi_asserted || firq_line || irq_line) return false;
        if (due == ~0ull) { b.fault = "SYNC with no pending event"; return true; }
        cyc = std::max(cyc, due);                  // @eat_remaining, then the suspend check
    }
}

bool Cpu::cwai_wait() {
    Board& b = *bd;
    uint16_t v;
    for (;;) {
        if (cyc >= due && b.apply_due()) return true;
        if ((v = b.pending_interrupt()) != 0) break;
        if (due == ~0ull) { b.fault = "CWAI with no pending event"; return true; }
        cyc = std::max(cyc, due);
    }
    if (nmi_asserted) nmi_asserted = false;
    cc |= uint8_t(0x10 | (v != 0xfff8 ? 0x40 : 0));
    return !b.step_vector(v);
}

Fn Board::lookup(uint16_t pc) {
    if (pc < 0x2000) {
        const uint16_t a = pc & 0x7ff;
        const PageTable& pt = k_pages[6];
        Fn f = tables[6][a];
        if (!f) return nullptr;
        for (size_t i = 0; i < pt.count; ++i) {
            if (pt.entries[i].pc == a) {
                const Entry& e = pt.entries[i];
                for (int k = 0; k < e.nchk; ++k)
                    if (ram[(a + k) & 0x7ff] != e.chk[k]) return nullptr;
                break;
            }
        }
        return f;
    }
    if (pc < 0x8000 || page_index < 0) return nullptr;
    return tables[page_index][pc - 0x8000];
}

void Board::run() {
    while (!stop && fault.empty()) {
        if (cpu.cyc >= cpu.due) {
            apply_due();
            continue;
        }
        if (halted) {                                     // suspended with eatcycles: time still advances
            cpu.cyc = cpu.due;
            continue;
        }
        if (vector_pending) {
            vector_pending = false;
            step_vector(0xfffe);
            continue;
        }
        if (const uint16_t v = pending_interrupt()) {
            take_interrupt(v);
            continue;
        }
        Fn f = lookup(cpu.pc);
        if (!f) {
            char buf[96];
            std::snprintf(buf, sizeof buf, "sound: no translation for pc %04x (bank %x, page %s)", cpu.pc, bank,
                          page_index >= 0 ? k_pages[page_index].name : "none");
            fault = buf;
            break;
        }
        f(cpu);
    }
}

}  // namespace stv::snd
