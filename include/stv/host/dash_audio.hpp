#pragma once
#include <array>
#include <cstddef>

namespace stv::host {
class DashAudio {
    std::array<double,2> position_{-1.,-1.};
    std::array<bool,2> launching_{};
public:
    // Observe the local or confirmed machine once per delivered game frame.
    bool observe(bool first, bool second);
    void mix(float* samples, std::size_t frames, unsigned channels, unsigned rate);
};
}
