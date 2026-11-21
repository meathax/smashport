// A pending main IRQ-clear synchronization must remain in the global timer
// ordering after an earlier sound timer shortens the main-aborted timeslice.
// Oracle: MAME 0.289 diexec.cpp device_input::set_state_synced keeps the first
// callback queued; schedule.cpp compares every new timer with that global head.
#include "stv/game/cosched.hpp"

#include <cstdio>

int main() {
    stv::gsp::Machine machine;
    stv::snd::Board sound;
    stv::game::CoScheduler scheduler(machine, sound);
    machine.scan_next_ps = 1000000000;
    machine.vblank_next_ps = 2000000000;
    machine.vblank_end_ps = ~stv::gsp::u64{0};
    machine.dma_done_ps = ~stv::gsp::u64{0};
    sound.q.clear();

    // Sound's earlier callback prevents the main synchronization from being
    // the global head. Its deadline must nevertheless survive that callback.
    const auto early = stv::game::CoScheduler::main_time(50);
    const auto pending = stv::game::CoScheduler::main_time(100);
    const auto later = stv::game::CoScheduler::main_time(150);
    sound.schedule(early, stv::snd::Board::kInputLine, stv::snd::Board::kLineReset, 0);
    machine.c.cyc = 100;
    machine.ww(0x01a80000, 0);
    sound.fire_until(early);
    if (!(scheduler.next_foreign_timer() == pending)) {
        std::fprintf(stderr, "pending DMA IRQ-clear callback disappeared after earlier sound event\n");
        return 1;
    }

    // Repeated changes on one CPU input line share its first queued callback
    // in MAME. They must not move the pending deadline to a later write.
    machine.c.cyc = 150;
    machine.ww(0x01a80000, 0);
    if (!(scheduler.next_foreign_timer() == pending)) {
        std::fprintf(stderr, "repeated main input-line write moved the pending callback\n");
        return 1;
    }
    if (sound.main_sound_write(later, 0xfdc4)) {
        std::fprintf(stderr, "later sound command incorrectly became global timer head\n");
        return 1;
    }
    std::puts("pending DMA synchronization retains global deadline and timer ordering");
    return 0;
}
