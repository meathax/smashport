// Extended sound channels and the output chain, on the real coupled machine.
//  1. Observe-only: the SNDLD probes and the extra sound boards never change the
//     game (RAM) or the retail board PCM, frame for frame.
//  2. Sounds the retail loader discards (gunfire while the announcer speaks)
//     are played on extra boards, without faults.
//  3. The stereo output stays finite and below full scale.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <chrono>
#define wWinMain unused_extended_sound_entry
#include "../src/host/win_main.cpp"
#undef wWinMain

namespace {
// Optional listening comparison: STV_SOUND_WAV=<dir> writes retail.wav (the
// original mono board) and enhanced.wav (extra boards + output chain).
struct Wav {
    std::vector<int16_t> data; int channels = 1;
    void add(float v) { data.push_back(int16_t(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f))); }
    void write(const std::filesystem::path& file, uint32_t rate) const {
        FILE* f = _wfopen(file.c_str(), L"wb"); if (!f) return;
        const uint32_t bytes = uint32_t(data.size() * 2), byte_rate = rate * channels * 2;
        const uint16_t align = uint16_t(channels * 2), bits = 16, pcm = 1, ch = uint16_t(channels);
        const uint32_t riff = 36 + bytes, fmt = 16;
        std::fwrite("RIFF", 1, 4, f); std::fwrite(&riff, 4, 1, f); std::fwrite("WAVEfmt ", 1, 8, f);
        std::fwrite(&fmt, 4, 1, f); std::fwrite(&pcm, 2, 1, f); std::fwrite(&ch, 2, 1, f); std::fwrite(&rate, 4, 1, f);
        std::fwrite(&byte_rate, 4, 1, f); std::fwrite(&align, 2, 1, f); std::fwrite(&bits, 2, 1, f);
        std::fwrite("data", 1, 4, f); std::fwrite(&bytes, 4, 1, f); std::fwrite(data.data(), 2, data.size(), f);
        std::fclose(f);
    }
} g_retail_wav, g_enhanced_wav;
bool g_wav = std::getenv("STV_SOUND_WAV") != nullptr;

struct Run {
    std::vector<u32> ram, pcm;
    uint64_t dropped = 0, steals = 0;
    int max_voices = 0;
    double peak = 0, energy = 0, seconds = 0;
    bool finite = true;
    std::string fault;
    std::vector<double> level;   // gameplay: AC level (std-dev, DC removed) of the mono mix per 60 frames
};

u32 input_at(unsigned i) {
    if (i > 2600 && i < 2620) return 0x100u;                         // coin
    if (i > 3150 && i < 3170) return 0x200u;                         // start
    if (i < 3300) return 0;
    // Hold fire in a rotating direction while walking: continuous gunfire.
    static constexpr u32 kFire[4] = {0x10u, 0x20u, 0x40u, 0x80u};
    static constexpr u32 kMove[4] = {0x01u, 0x04u, 0x02u, 0x08u};
    return kFire[(i / 200) % 4] | kMove[(i / 75) % 4];
}

std::vector<u16> g_start_cmos;   // both runs begin from the same CMOS (a game changes its audits)

Run play(bool extended, unsigned frames) {
    Run r;
    g_settings.extended_sound = extended;
    g_settings.audio_hifi = extended;
    g_settings.audio_wide = extended;
    g_settings.reverb = extended ? 1 : 0;
    g_voices.stop();
    g_fx_configured = false;
    if (g_start_cmos.empty()) g_start_cmos = g_m.cmos;
    else g_m.cmos = g_start_cmos;
    if (!reset_coupled_game()) { r.fault = "reset"; return r; }
    double spent = 0;
    double w_sum = 0, w_sq = 0;
    std::size_t w_n = 0;
    for (unsigned i = 0; i < frames; ++i) {
        const u32 in = input_at(i);
        g_pcm.clear();
        run_one_frame();
        stv::net::apply_inputs(g_m, in, 0);
        if (g_m.stop) { r.fault = "machine stopped"; break; }
        u32 h = 2166136261u;
        for (u16 w : g_m.ram) h = (h ^ w) * 16777619u;
        r.ram.push_back(h);
        u32 a = 2166136261u;
        for (float f : g_pcm) { u32 b; std::memcpy(&b, &f, 4); a = (a ^ b) * 16777619u; }
        r.pcm.push_back(a);
        const auto t0 = std::chrono::steady_clock::now();
        deliver_audio(false);
        const double d = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (i >= 3300) spent += d;   // gameplay; the player skips the power-on test without output
        if (i >= 3300) {
            if (!extended) for (float v : g_pcm) w_sum += v, w_sq += double(v) * v, ++w_n;
            else for (std::size_t k = 0; k + 1 < g_stereo.size(); k += 2) {
                const double v = 0.5 * (g_stereo[k] + g_stereo[k + 1]);
                w_sum += v; w_sq += v * v; ++w_n;
            }
            if ((i - 3300) % 60 == 59) {
                const double mean = w_n ? w_sum / w_n : 0;
                r.level.push_back(w_n ? std::sqrt(std::max(0.0, w_sq / w_n - mean * mean)) : 0);
                w_sum = w_sq = 0; w_n = 0;
            }
        }
        if (g_wav && i >= 3300) {
            if (!extended) for (float v : g_pcm) g_retail_wav.add(v);
            else { g_enhanced_wav.channels = 2; for (float v : g_stereo) g_enhanced_wav.add(v); }
        }
        if (extended) {
            if (!g_voices.fault().empty()) { r.fault = g_voices.fault(); break; }
            r.max_voices = std::max(r.max_voices, g_voices.active_voices());
            for (float v : g_stereo) {
                if (!std::isfinite(v)) r.finite = false;
                r.peak = std::max(r.peak, double(std::fabs(v)));
                if (i >= 3300) r.energy += double(v) * v;
            }
        }
    }
    r.dropped = g_voices.dropped_played();
    r.steals = g_voices.steals();
    std::printf("boards rendered per frame %.2f, voice starts %llu, bank starts %llu\n", double(g_voices.boards_rendered()) / frames, (unsigned long long)g_voices.allocations(), (unsigned long long)g_voice_starts);
    r.seconds = spent;
    return r;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    std::filesystem::create_directories(argv[2]);
    g_rom_path = std::filesystem::path(argv[2]) / "retail-l8.dat";
    std::string error;
    if (!stv::gsp::import_rom_set(argv[1], g_rom_path, error)) { std::fprintf(stderr, "FAIL import: %s\n", error.c_str()); return 3; }
    g_enhanced = true;
    const unsigned frames = 9000;
    const Run base = play(false, frames);
    const Run ext = play(true, frames);
    bool ok = base.fault.empty() && ext.fault.empty();
    if (!ok) std::fprintf(stderr, "FAIL fault: base='%s' extended='%s'\n", base.fault.c_str(), ext.fault.c_str());
    unsigned ram_bad = 0, pcm_bad = 0;
    for (size_t i = 0; i < std::min(base.ram.size(), ext.ram.size()); ++i) {
        ram_bad += base.ram[i] != ext.ram[i];
        pcm_bad += base.pcm[i] != ext.pcm[i];
    }
    std::printf("frames=%u ram_differences=%u retail_pcm_differences=%u\n", frames, ram_bad, pcm_bad);
    std::printf("dropped_sounds_played=%llu max_extra_voices=%d steals=%llu peak=%.3f finite=%d\n",
                (unsigned long long)ext.dropped, ext.max_voices, (unsigned long long)ext.steals, ext.peak, int(ext.finite));
    std::printf("gameplay output cost per frame: retail %.3f ms, extended+fx %.3f ms\n",
                1000.0 * base.seconds / (frames - 3300), 1000.0 * ext.seconds / (frames - 3300));
    {   // cost split: output chain alone, and one board advanced for a frame
        stv::host::AudioFx fx; fx.configure(48000, {true, true, 2});
        std::vector<float> buf(800 * 2, 0.1f);
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 600; ++i) fx.process(buf.data(), 800);
        const double fx_ms = 1000.0 * std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / 600;
        stv::snd::VoiceBank one;
        one.start(g_snd, *g_mixer, g_sched->base(), 1);
        auto at = g_sched->base();
        std::vector<float> out;
        t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 300; ++i) { at = at + stv::snd::Atto{0, stv::snd::Atto::kPerSec / 60}; one.render(at, 800, out); }
        const double board_ms = 1000.0 * std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / 300;
        std::printf("output chain %.3f ms/frame; music board %.3f ms/frame\n", fx_ms, board_ms);
    }
    if (g_wav) {
        const std::filesystem::path dir = std::getenv("STV_SOUND_WAV");
        std::filesystem::create_directories(dir);
        g_retail_wav.write(dir / "retail.wav", g_mixer->out_rate());
        g_enhanced_wav.write(dir / "enhanced.wav", g_mixer->out_rate());
    }
    // Sustained sound (music, ambient loops) the retail board plays must still be audible in
    // the extended mix: a voice that is cut while its sequence is still current drops these.
    unsigned silent_seconds = 0, audible_seconds = 0;
    for (size_t i = 0; i < std::min(base.level.size(), ext.level.size()); ++i) {
        if (base.level[i] < 0.03) continue;
        ++audible_seconds;
        if (ext.level[i] < 0.5 * base.level[i]) ++silent_seconds;
    }
    std::printf("retail-audible seconds=%u, lost or halved in extended mix=%u\n", audible_seconds, silent_seconds);
    ok = ok && audible_seconds > 20 && silent_seconds <= 1;
    ok = ok && base.ram.size() == frames && ext.ram.size() == frames && ram_bad == 0 && pcm_bad == 0;
    ok = ok && ext.dropped > 0 && ext.max_voices >= 2 && ext.finite && ext.peak <= 1.0 && ext.energy > 0;
    std::puts(ok ? "PASS: extended sound is observe-only, plays discarded sounds and stays clip-free"
                 : "FAIL: extended sound");
    return ok ? 0 : 1;
}
