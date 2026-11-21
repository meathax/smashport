#include "stv/host/recomp_achievements.hpp"

#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace {
std::vector<std::string> events;
std::vector<std::pair<uint32_t, int32_t>> boards;
}

extern "C" {
int rl_ach_init(const char*, rl_ach_read_fn, void*) { return 1; }
void rl_ach_shutdown() {}
void rl_ach_frame() {}
void rl_ach_event(const char* name) { events.emplace_back(name); }
void rl_ach_lboard(uint32_t id, int32_t value) { boards.emplace_back(id, value); }
void rl_ach_presence(const char*) {}
void rl_ach_casual(const char*) {}
}

int main() {
    using namespace stv::gsp;
    Machine machine;
    stv::host::RecompAchievements achievements;
    achievements.start(false);
    const auto sample = [&](u16 scene, u32 background, bool online = false) {
        machine.ram[(L8_GAME_STATE - 0x01000000u) >> 4] = scene;
        const auto index = (L8_BACKGROUND_MODULES - 0x01000000u) >> 4;
        machine.ram[index] = u16(background);
        machine.ram[index + 1] = u16(background >> 16);
        ++machine.frame_number;
        achievements.frame(machine, online);
    };
    constexpr u32 grat = 0xffea54f0u;
    sample(0x30, grat);
    sample(0x10, grat);
    sample(0x18, 0, true);
    sample(0x10, grat, true);
    if (!events.empty() || !boards.empty()) return EXIT_FAILURE;
    sample(0x18, 0);
    sample(0x10, grat);
    sample(0x10, grat);
    if (events != std::vector<std::string>{"start_game", "complete_game"} ||
        boards != std::vector<std::pair<uint32_t, int32_t>>{{1, 2}}) return EXIT_FAILURE;
    sample(0x30, 0);
    machine.enhanced = true;
    sample(0x18, 0);
    machine.frame_number = 0;
    sample(0x10, grat);
    if (events.size() != 3 || boards.size() != 1) return EXIT_FAILURE;
    sample(0x18, 0);
    sample(0x10, 0);
    sample(0x10, grat);
    if (events.size() != 5 || boards.size() != 2 || boards.back() != std::pair<uint32_t, int32_t>{2, 3})
        return EXIT_FAILURE;
}
