// PLAN P8: presentation scaling, including 4K displays with correct integer scaling.
#include "stv/host/scale.hpp"

#include <cstdio>
#include <initializer_list>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

using namespace stv::host;

static int fails = 0;

#ifdef _WIN32
// Present a pseudo-random source through the host's GDI path (StretchDIBits,
// COLORONCOLOR) at 3840x2160 and check every source pixel is an exact sx x sy block.
static void gdi_block_check(int vw, int vh, ScaleMode mode) {
    const int cw = 3840, ch = 2160;
    const Scale sc = compute_scale(cw, ch, vw, vh, mode);
    std::vector<unsigned> src(std::size_t(vw) * vh);
    unsigned seed = 12345;
    for (unsigned& p : src) { seed = seed * 1103515245u + 12345u; p = (seed >> 8) & 0xffffff; }
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = cw;
    bi.bmiHeader.biHeight = -ch;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(dc, bmp);
    SetStretchBltMode(dc, COLORONCOLOR);
    bi.bmiHeader.biWidth = vw;
    bi.bmiHeader.biHeight = -vh;
    const int x0 = (cw - sc.w) / 2, y0 = (ch - sc.h) / 2;
    StretchDIBits(dc, x0, y0, sc.w, sc.h, 0, 0, vw, vh, src.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
    GdiFlush();
    const unsigned* out = static_cast<const unsigned*>(bits);
    long bad = 0;
    for (int y = 0; y < sc.h; ++y)
        for (int x = 0; x < sc.w; ++x)
            if ((out[std::size_t(y0 + y) * cw + x0 + x] & 0xffffff) != src[std::size_t(y / sc.sy) * vw + x / sc.sx]) ++bad;
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
    std::printf("%s GDI 4K block check %dx%d x%d y%d: %ld mismatched pixels\n", bad ? "FAIL" : "PASS", vw, vh, sc.sx, sc.sy, bad);
    if (bad) ++fails;
}
#endif
static void expect(const char* what, Scale s, int w, int h, int sx, int sy) {
    const bool ok = s.w == w && s.h == h && s.sx == sx && s.sy == sy;
    std::printf("%s %s: %dx%d (x%d y%d)\n", ok ? "PASS" : "FAIL", what, s.w, s.h, s.sx, s.sy);
    if (!ok) ++fails;
}

int main() {
    // 4K UHD, enhanced 547x256 and reference 410x256
    expect("4K exact 16:9", compute_scale(3840, 2160, 547, 256, ScaleMode::IntegerExact), 2735, 1536, 5, 6);
    expect("4K fill 16:9", compute_scale(3840, 2160, 547, 256, ScaleMode::IntegerFill), 3829, 2048, 7, 8);
    expect("4K exact 4:3", compute_scale(3840, 2160, 410, 256, ScaleMode::IntegerExact), 2050, 1536, 5, 6);
    expect("4K fill 4:3", compute_scale(3840, 2160, 410, 256, ScaleMode::IntegerFill), 2870, 2048, 7, 8);
    // 1440p and 1080p
    expect("1440p fill 16:9", compute_scale(2560, 1440, 547, 256, ScaleMode::IntegerFill), 2188, 1280, 4, 5);
    expect("1080p exact falls back to smooth", compute_scale(1920, 1080, 547, 256, ScaleMode::IntegerExact), 1920, 1078, 0, 0);
    // 1440p and up keep the aspect within 5% of 5:6 pixels; every result fits
    for (int ch : {1440, 2160, 2880})
        for (int vw : {410, 547}) {
            const int cw = ch * 16 / 9;
            const Scale s = compute_scale(cw, ch, vw, 256, ScaleMode::IntegerFill);
            const double err = (double(s.sx) / s.sy) / kPixelAspect - 1.0;
            if (s.w > cw || s.h > ch || err > 0.0501 || err < -0.0501) {
                std::printf("FAIL fit %dx%d vw %d\n", cw, ch, vw);
                ++fails;
            }
        }
    for (int ch : {720, 768, 900, 1080, 1200}) {
        const Scale s = compute_scale(ch * 16 / 9, ch, 547, 256, ScaleMode::IntegerFill);
        const double err = s.sx ? (double(s.sx) / s.sy) / kPixelAspect - 1.0 : 0.0;
        if (s.w > ch * 16 / 9 || s.h > ch || err > 0.05 + 1e-12 || err < -0.05 - 1e-12) {
            std::printf("FAIL small %d\n", ch);
            ++fails;
        }
    }
    // Every supported aspect/size must use a compliant integer pair or the
    // smooth fallback. Check the rendered aspect too, allowing one rounded pixel.
    for (int vh : {256})
        for (int vw : {410, 547})
            for (int ch : {480, 600, 720, 768, 900, 1080, 1200, 1440, 2160, 2880, 4320})
                for (int width : {4, 16, 21}) {
                    const int cw = ch * width / (width == 4 ? 3 : 9);
                    for (ScaleMode mode : {ScaleMode::IntegerExact, ScaleMode::IntegerFill}) {
                        const Scale s = compute_scale(cw, ch, vw, vh, mode);
                        const double desired = double(vw) * kPixelAspect / vh;
                        const double error = std::fabs(double(s.w) / s.h / desired - 1.0);
                        const double tolerance = s.sx ? 0.05 + 1e-12 : 1.0 / s.h;
                        if (s.w <= 0 || s.h <= 0 || s.w > cw || s.h > ch || error > tolerance ||
                            (s.sx && (s.w != s.sx * vw || s.h != s.sy * vh))) {
                            std::printf("FAIL aspect/fit %dx%d source %dx%d mode %d\n", cw, ch, vw, vh, int(mode));
                            ++fails;
                        }
                    }
                }
#ifdef _WIN32
    gdi_block_check(547, 256, ScaleMode::IntegerExact);
    gdi_block_check(547, 256, ScaleMode::IntegerFill);
    gdi_block_check(410, 256, ScaleMode::IntegerFill);
#endif
    std::printf(fails ? "FAILED\n" : "all scaling checks passed\n");
    return fails ? 1 : 0;
}
