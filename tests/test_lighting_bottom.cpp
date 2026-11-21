// Ray-traced room lighting must not blacken the front wall. The arena's front
// wall face starts at the floor edge; the lip proxy box is solid there, so a
// ground receiver inside it was fully shadowed and the wall rendered as a black
// bar. Real presented frames, lit and unlit from the same machine state.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#define wWinMain unused_lighting_bottom_entry
#include "../src/host/win_main.cpp"
#undef wWinMain

namespace {
bool capture_luma(std::vector<float>& rows, int& width, int& height) {
    if (!g_resident_ready || !g_resident_effects.capture(g_pixels)) return false;
    width = int(g_frame_width);
    height = int(g_pixels.size() / g_frame_width);
    rows.assign(size_t(height), 0.f);
    const int x0 = width * 22 / 100, x1 = width * 77 / 100;   // clear of the side walls and wings
    for (int y = 0; y < height; ++y) {
        double sum = 0;
        for (int x = x0; x < x1; ++x) {
            const u32 p = g_pixels[size_t(y) * size_t(width) + size_t(x)];
            sum += (((p >> 16) & 255) + ((p >> 8) & 255) + (p & 255)) / 3.0;
        }
        rows[size_t(y)] = float(sum / (x1 - x0));
    }
    return true;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) return 2;
    g_wnd = CreateWindowExW(0, L"STATIC", L"Headless resident effects", WS_OVERLAPPEDWINDOW, 0, 0, 640, 480, nullptr, nullptr,
                            GetModuleHandleW(nullptr), nullptr);
    std::filesystem::create_directories(argv[3]);
    g_rom_path = std::filesystem::path(argv[3]) / "retail-l8.dat";
    std::string error;
    if (!stv::gsp::import_rom_set(argv[1], g_rom_path, error)) return 3;
    g_enhance_effects.use_gpu_simulation(true);
    g_enhanced = true;
    constexpr unsigned kFrame = 4000;
    if (!stv::net::load_scripted_inputs(argv[2], kFrame + 1, g_script1, g_script2) || !reset_coupled_game()) return 3;
    g_settings.enhance = true;
    g_settings.ray_tracing = true;
    g_settings.voxels = true;
    g_settings.shockwaves = g_settings.tracers = g_settings.brass = true;
    if (!g_resident_effects.prepare_dxr()) {
        std::fprintf(stderr, "SKIP: DXR unavailable: %s\n", g_resident_effects.dxr_error().c_str());
        return 77;
    }
    for (unsigned f = 0; f <= kFrame; ++f) {
        g_pcm.clear();
        run_one_frame();
        if (!g_frame_done || g_m.stop || !g_snd.fault.empty()) return 4;
        g_settings.hardware_rt = true;
        present_frame(g_m);
        if (f < kFrame) continue;
        std::vector<float> lit, plain;
        int w = 0, h = 0, w2 = 0, h2 = 0;
        if (!capture_luma(lit, w, h)) { std::puts("FAIL: no lit capture"); return 5; }
        g_settings.hardware_rt = false;
        present_frame(g_m);
        if (!capture_luma(plain, w2, h2) || w2 != w || h2 != h) { std::puts("FAIL: no unlit capture"); return 6; }
        // The front wall begins where the art's shading strip appears below the floor.
        int wall = -1;
        for (int y = h * 9 / 10; y < h; ++y)
            if (plain[size_t(y)] > 70.f) { wall = y; break; }
        if (wall < 0) { std::puts("FAIL: front wall not found in the unlit frame"); return 7; }
        bool ok = true;
        float worst = 9.f;
        for (int y = wall; y < wall + 7 && y < h; ++y) {
            const float ratio = lit[size_t(y)] / std::max(plain[size_t(y)], 1.f);
            worst = std::min(worst, ratio);
            std::printf("row %d art %.0f lit %.0f ratio %.2f\n", y, plain[size_t(y)], lit[size_t(y)], ratio);
            if (ratio < 0.75f) ok = false;
        }
        std::printf("front wall rows %d..%d, worst lit/art ratio %.2f (must be >= 0.75)\n", wall, wall + 6, worst);
        std::puts(ok ? "PASS: front wall is lit, not black" : "FAIL: front wall is shadowed black");
        DestroyWindow(g_wnd);
        return ok ? 0 : 1;
    }
    return 8;
}
