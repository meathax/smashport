#include "stv/host/net_fault.hpp"
#include <cstdio>
#include <limits>

using Fault = stv::host::NetFaultInjection;
int main(int argc, char** argv) {
    unsigned checks = 0, failures = 0;
    auto check = [&](bool value, const char* name) {
        ++checks; if (!value) { ++failures; std::fprintf(stderr, "FAIL: %s\n", name); }
    };
    Fault fault;
    check(fault.configure(0, 30) && !fault.enabled, "zero disables injection");
    check(!fault.configure(900, 0), "zero hash period rejected");
    check(!fault.configure(std::numeric_limits<unsigned>::max(), 30), "boundary overflow rejected");
    check(fault.configure(899, 30) && fault.effective_frame == 900, "round up to next exchanged frame");
    check(fault.configure(900, 30) && fault.effective_frame == 900, "exact exchange boundary retained");
    check(fault.configure(901, 30) && fault.effective_frame == 930, "one past boundary advances");

    std::uint16_t word = 0xffff;
    auto hash = [&] { return (2166136261u ^ word) * 16777619u; };
    check(!fault.after_step(901, 0, false, word, hash) && word == 0xffff && !fault.attempts,
          "unexchanged frame never mutated");
    check(!fault.after_step(931, 0, false, word, hash) && !fault.attempts,
          "no later frame mutated");
    check(fault.after_step(930, 0, false, word, hash) && word == 0x7fff &&
          fault.attempts == 1 && fault.changed_words == 1 && fault.changed_hashes == 1,
          "previously set high bit really changes at selected frame");
    word = 0xffff; // the rollback engine restores the before-frame state.
    check(fault.after_step(930, 0, false, word, hash) && fault.attempts == 2 &&
          fault.events[0].before_hash == fault.events[1].before_hash &&
          fault.events[0].after_hash == fault.events[1].after_hash,
          "resimulation repeats from restored state and is observed");
    check(!fault.after_step(930, 1, false, word, hash) && fault.attempts == 2,
          "no reinjection after completed resync");
    check(!fault.after_step(930, 0, true, word, hash) && fault.attempts == 2,
          "no injection during resync");

    bool every_word_changed = true;
    for (unsigned before = 0; before <= 0xffff; ++before) {
        Fault one; one.configure(30, 30);
        std::uint16_t value = std::uint16_t(before);
        auto one_hash = [&] { return (2166136261u ^ value) * 16777619u; };
        one.after_step(30, 0, false, value, one_hash);
        every_word_changed &= value != before && one.changed_words == 1 && one.changed_hashes == 1;
    }
    check(every_word_changed, "all 65536 original words produce a changed word and FNV hash");
    Fault broken; broken.configure(30, 30);
    std::uint16_t value = 0;
    broken.after_step(30, 0, false, value, [] { return 123u; });
    check(broken.attempts == 1 && broken.changed_words == 1 && broken.changed_hashes == 0,
          "unchanged observed hash remains explicit failed injection evidence");
    if (argc == 2) {
        FILE* file = std::fopen(argv[1], "w");
        check(file != nullptr, "open actual JSON fixture");
        if (file) { fault.write_json(file); std::fputs("\n", file); std::fclose(file); }
    }
    std::printf("net fault injection: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
