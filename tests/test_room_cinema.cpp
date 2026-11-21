#include "stv/host/room_cinema.hpp"
#include "stv/host/pc_settings.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

namespace {
int failures = 0;

void check(bool condition, std::string_view message) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %.*s\n", int(message.size()), message.data());
    ++failures;
}

bool near(float a, float b, float tolerance = 0.0001f) {
    return std::fabs(a - b) <= tolerance;
}

void test_audio_peaks() {
    stv::host::AudioPeakObserver observer;
    std::array<float, 5> samples{0.10f, -0.75f, 0.90f, NAN, -0.25f};
    const auto original = samples;
    observer.observe(samples);
    check(std::memcmp(samples.data(), original.data(), sizeof(samples)) == 0,
          "PCM observation must not mutate samples");
    check(near(observer.peak(), 0.90f), "observer must report the largest finite magnitude");
    check(near(observer.pulse(), 0.90f), "attack must follow the raw board peak");
    check(observer.samples_seen() == samples.size(), "observer must count every supplied sample");
    observer.observe(std::array<float, 2>{0.05f, -0.10f});
    check(near(observer.peak(), 0.10f), "peak must describe the latest PCM block");
    check(observer.pulse() > observer.peak() && observer.pulse() < 0.90f,
          "release must decay without inventing a new peak");
    observer.reset();
    check(observer.peak() == 0 && observer.pulse() == 0 && observer.samples_seen() == 0,
          "reset must clear presentation-only state");
}

stv::host::RoomCinemaInput boss_input(std::uint64_t frame) {
    stv::host::RoomCinemaInput input;
    input.frame = frame;
    input.room = 10;
    input.roomAge = unsigned(frame);
    input.inArena = true;
    input.bossRoom = true;
    return input;
}

void test_boss_lifecycle() {
    stv::host::BossCinemaTracker tracker;
    auto intro = boss_input(1);
    intro.roomIntro = true;
    auto cue = tracker.observe(intro, 2, 0.2f);
    check(cue.quality == 2 && near(cue.bossIntro, 0.9762f, 0.01f),
          "boss intro must be graded from observed room-intro state");

    auto active = boss_input(40);
    active.bossParts = 3;
    active.bossX = 210;
    active.bossY = 92;
    cue = tracker.observe(active, 2, 0.3f);
    check(cue.activeBoss && cue.bossPhase > cue.bossIntro,
          "active boss presence must advance the presentation phase");

    auto partial = boss_input(72);
    partial.bossParts = 1;
    partial.bossRetired = true;
    partial.bossX = 208;
    partial.bossY = 90;
    cue = tracker.observe(partial, 2, 0.1f);
    check(!cue.death, "a component retirement while a boss part remains is not a final death");

    auto final = boss_input(80);
    final.bossParts = 0;
    final.bossRetired = true;
    cue = tracker.observe(final, 2, 0.1f);
    check(!cue.death, "the final-retire guard must not fire immediately");
    for (std::uint64_t frame = 81; frame <= 92; ++frame)
        cue = tracker.observe(boss_input(frame), 2, 0.1f);
    check(cue.death && cue.deathPower > 0, "the original retirement must become a protected death cue");

    stv::host::BossCinemaTracker cancel;
    active.frame = 100;
    active.roomAge = 100;
    cancel.observe(active, 3, 0);
    final.frame = 120;
    final.roomAge = 120;
    final.bossRetired = true;
    cancel.observe(final, 3, 0);
    auto reappeared = boss_input(124);
    reappeared.bossParts = 1;
    reappeared.bossX = 180;
    reappeared.bossY = 90;
    cue = cancel.observe(reappeared, 3, 0);
    check(!cue.death && cue.activeBoss, "a reappearing boss incarnation must cancel pending death");

    cue = tracker.observe(boss_input(92), 0, 1);
    check(cue.quality == 0 && cue.audioPulse == 0 && cue.bossPower == 0 && cue.deathPower == 0,
          "Off must suppress all room-cinema state and analysis output");

    auto title = boss_input(30);
    title.inArena = false;
    cue = tracker.observe(title, 2, 0.9f);
    check(cue.audioPulse == 0 && !cue.activeBoss && !cue.death,
          "non-arena screens must not retain audio or boss cinema");

    stv::host::BossCinemaTracker rewind;
    auto before = boss_input(200);
    before.bossParts = 2;
    before.bossX = 200;
    rewind.observe(before, 3, 0);
    auto after = boss_input(10);
    after.bossRetired = true;
    cue = rewind.observe(after, 3, 0);
    check(!cue.death && cue.bossPhase < 0.5f,
          "frame rewind must reset protected boss-death state");
}

void test_quality_and_tint() {
    check(std::string_view(stv::host::room_cinema_quality_name(0)) == "off",
          "quality zero must be labelled Off");
    check(std::string_view(stv::host::room_cinema_quality_name(3)) == "high",
          "quality three must be labelled High");
    check(std::string_view(stv::host::room_cinema_quality_name(99)) == "high",
          "invalid high values must remain bounded");
    const auto cold = stv::host::room_cinema_tint(10);
    const auto hot = stv::host::room_cinema_tint(0x20007);
    check(cold[2] > cold[0] && hot[0] > hot[2], "boss rooms must have distinct safe tints");
}

void test_settings_migration() {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / "smash-room-cinema-tests";
    std::error_code ec;
    fs::create_directories(root, ec);
    check(!ec, "test directory must be creatable");
    const auto current = root / "current.cfg";
    stv::host::PcSettings settings;
    settings.room_cinema = 3;
    std::string error;
    check(stv::host::save_pc_settings(current, settings, error), "version 2 settings must save");
    stv::host::PcSettings loaded;
    check(stv::host::load_pc_settings(current, loaded, error) == stv::host::SettingsLoad::Loaded &&
          loaded.room_cinema == 3, "quality must round-trip through persistence");

    const auto legacy = root / "legacy.cfg";
    {
        std::ofstream out(legacy, std::ios::binary | std::ios::trunc);
        out << "version=1\nname=PLAYER\n";
    }
    loaded = {};
    check(stv::host::load_pc_settings(legacy, loaded, error) == stv::host::SettingsLoad::Loaded &&
          loaded.room_cinema == 0, "version 1 profiles must migrate cinema to Off");

    const auto partial = root / "partial-v2.cfg";
    {
        std::ofstream out(partial, std::ios::binary | std::ios::trunc);
        out << "version=2\nname=PLAYER\n";
    }
    loaded = {};
    check(stv::host::load_pc_settings(partial, loaded, error) == stv::host::SettingsLoad::Invalid,
          "version 2 profiles must include the quality key");
    fs::remove(current, ec); fs::remove(legacy, ec); fs::remove(partial, ec);
}

int benchmark() {
    using Clock = std::chrono::steady_clock;
    stv::host::AudioPeakObserver observer;
    stv::host::BossCinemaTracker tracker;
    constexpr std::size_t iterations = 2'000'000;
    constexpr std::array<float, 8> samples{.1f,.2f,-.3f,.4f,.5f,-.6f,.7f,-.8f};
    auto input = boss_input(40);
    input.bossParts = 4;
    input.bossX = 200;
    input.bossY = 90;
    const auto start = Clock::now();
    for (std::size_t i = 0; i < iterations; ++i) {
        observer.observe(samples);
        input.frame = 40 + i;
        (void)tracker.observe(input, 3, observer.pulse());
    }
    const auto elapsed = std::chrono::duration<double, std::nano>(Clock::now() - start).count();
    std::printf("room-cinema host-state overhead: %.2f ns/frame (2,000,000 iterations, 8 PCM samples)\n",
                elapsed / iterations);
    return observer.samples_seen() == 0;
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--benchmark") return benchmark();
    test_audio_peaks();
    test_boss_lifecycle();
    test_quality_and_tint();
    test_settings_migration();
    return failures ? 1 : 0;
}
