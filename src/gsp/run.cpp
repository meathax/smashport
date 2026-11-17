// Dispatcher for statically translated L8 code (PLAN D2): maps a PC to the
// generated chunk that contains it and calls it. No instruction is decoded here.
#include "stv/gsp/ops.hpp"

namespace stv::gsp {

u32 l8_run(Machine& m, u32 pc, u64 until_cycle) {
    // The stop cycle takes part in next_event_cycle, so a chunk returns at the
    // first instruction boundary at or after it (MAME: execute until icount <= 0).
    m.run_until_cycle = until_cycle;
    m.recompute_next_event();
    // tms340x0_device::execute_run(): check interrupts, then execute instructions
    // while icount > 0 -- always at least one (do/while).
    if (m.c.cyc >= until_cycle || m.stop) {
        m.run_until_cycle = ~0ull;
        m.recompute_next_event();
        return pc;
    }
    pc = m.check_interrupt(pc);
    do {
        ChunkFn fn = l8_chunk_for(pc);
        if (!fn) {
            m.fault("no translated code at PC", pc);
            break;
        }
        pc = fn(m, pc);
    } while (m.c.cyc < m.run_until_cycle && !m.stop);
    m.run_until_cycle = ~0ull;
    m.recompute_next_event();
    return pc;
}

} // namespace stv::gsp
