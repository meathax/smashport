#include "../src/host/title_background.hpp"
#include <cstdio>
#include <fstream>
// Widescreen native pages show the original 4:3 ROM image with black sides:
// the release contains no title or wing artwork and synthesises none.
int main(int argc, char** argv) {
    std::vector<std::uint32_t> source(410u * 256u);
    for (unsigned i = 0; i < source.size(); ++i) source[i] = i * 7919u;
    for (int repeat = 0; repeat < 2; ++repeat) for (bool wide : {false, true}) for (bool title : {false, true}) {
        auto pixels = source; int width = 410;
        if (stv::host::extend_title(pixels, width, wide, title) != wide) return 2;
        if (!wide) { if (width != 410 || pixels != source) return 3; continue; }
        if (width != 547 || pixels.size() != 547u * 256u) return 4;
        for (unsigned y = 0; y < 256; ++y) for (unsigned x = 0; x < 547; ++x) {
            const auto expected = x >= 68 && x < 478 ? source[y * 410 + x - 68] : 0u;
            if (pixels[y * 547 + x] != expected) return 5;
        }
        const auto completed = pixels;
        if (stv::host::extend_title(pixels, width, true, title) || pixels != completed) return 6;
        if (title && argc == 2) {
            std::ofstream f(argv[1], std::ios::binary); f << "P6\n547 256\n255\n";
            for (auto pixel : pixels) for (int shift : {16, 8, 0}) f.put(char(pixel >> shift));
            if (!f) return 7;
        }
    }
    std::puts("PASS: native pages keep the original 4:3 ROM image with black sides; no gameplay repadding");
}
