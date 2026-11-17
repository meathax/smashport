#pragma once
#include "base.hpp"
namespace stv {
// Executed MAME 0.289 L8 screen witness and source GSP.INC display targets.
// This fixes host frame pacing only. CPU instruction phases, interrupt service
// latency, input sampling and HCOUNT-dependent RNG still need integration.
struct L8ReferenceTiming {
    static constexpr u64 pixel_clock_hz=8'000'000;
    static constexpr u64 horizontal_total=506;
    static constexpr u64 vertical_total=289;
    static constexpr u64 pixels_per_frame=horizontal_total*vertical_total;
    static constexpr u16 half_screen_interrupt=0x009a;
    static constexpr u16 end_screen_interrupt=0x011a;
};
}
