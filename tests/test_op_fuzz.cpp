// PLAN P5 reduced fuzz, native side (development only): the L8 instruction forms no
// scenario executes, one instruction per vector, compared with MAME 0.289 single-
// stepping the same instance from the same state (tools/lockstep/opfuzz.lua).
//
//   stv_op_fuzz <smashtv.zip> <vectors.txt> <mame_results.txt>
// Main RAM holds the pattern opfuzz.lua writes; the I/O registers that XY/pixel
// ops read (CONTROL, CONVSP, CONVDP, PSIZE, PMASK) are taken from MAME's dump.
#include "stv/gsp/ops.hpp"

#include <charconv>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace stv::gsp;

namespace {
u16 pattern(u32 i) { return u16(((i * 0x9E37u) ^ ((i >> 3) * 0x85EBu)) & 0xffff); }
u32 ram_fnv(const Machine& m) {
    u32 h = 2166136261u;
    for (u16 w : m.ram) h = (h ^ w) * 16777619u;
    return h;
}
bool hexline(const std::string& s, std::vector<u32>& v) {
    v.clear();
    std::istringstream in(s);
    std::string w;
    while (in >> w) {
        u32 value{};
        const auto parsed = std::from_chars(w.data(), w.data() + w.size(), value, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != w.data() + w.size()) return false;
        v.push_back(value);
    }
    return true;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: %s smashtv.zip vectors.txt mame_results.txt\n", argv[0]); return 2; }
    static Machine m;
    std::string err;
    if (!m.load_roms(argv[1], err)) { std::fprintf(stderr, "ROM import failed: %s\n", err.c_str()); return 2; }
    m.reset();
    std::ifstream vf(argv[2]), rf(argv[3]);
    if (!vf || !rf) { std::fprintf(stderr, "cannot open vectors or results file\n"); return 2; }
    std::string line;
    std::getline(rf, line);
    if (line.rfind("IO ", 0) != 0) { std::fprintf(stderr, "results file lacks the IO header\n"); return 2; }
    std::vector<u32> io;
    if (!hexline(line.substr(3), io) || io.size() != 32) {
        std::fprintf(stderr, "results IO header must contain 32 hexadecimal registers\n"); return 2;
    }
    for (u32 value : io) if (value > 0xffff) {
        std::fprintf(stderr, "results IO register exceeds 16 bits\n"); return 2;
    }
    for (int idx : {CONTROL, CONVSP, CONVDP, PSIZE, PMASK}) m.ww(0xC0000000u + u32(idx) * 16, u16(io[std::size_t(idx)]));
    std::map<u32, std::pair<int, int>> per_pc;   // pc -> (vectors, mismatches)
    int total = 0, bad = 0;
    std::string vl, rl;
    while (true) {
        const bool has_vector = bool(std::getline(vf, vl));
        const bool has_result = bool(std::getline(rf, rl));
        if (has_vector != has_result) {
            std::fprintf(stderr, "vector/result length mismatch after %d vectors\n", total); return 2;
        }
        if (!has_vector) break;
        std::vector<u32> v, r;
        if (!hexline(vl, v) || v.size() != 33 || !hexline(rl, r) || r.size() != 34) {
            std::fprintf(stderr, "malformed vector/result row %d\n", total + 1); return 2;
        }
        for (u32 i = 0; i < m.ram.size(); ++i) m.ram[i] = pattern(i);
        m.c.st = v[1];
        m.c.sp = v[2];
        for (int k = 0; k < 15; ++k) m.c.a[k] = v[3 + std::size_t(k)], m.c.b[k] = v[18 + std::size_t(k)];
        m.faults.clear();
        const u32 pc = l8_run(m, v[0], m.c.cyc + 1);
        std::vector<u32> got{pc, m.c.st, m.c.sp};
        for (int k = 0; k < 15; ++k) got.push_back(m.c.a[k]);
        for (int k = 0; k < 15; ++k) got.push_back(m.c.b[k]);
        got.push_back(ram_fnv(m));
        auto& pp = per_pc[v[0]];
        ++pp.first, ++total;
        if (got != r) {
            ++pp.second, ++bad;
            if (pp.second <= 2) {
                static const char* names[34] = {"PC", "ST", "SP", "A0", "A1", "A2", "A3", "A4", "A5", "A6", "A7", "A8",
                                                "A9", "A10", "A11", "A12", "A13", "A14", "B0", "B1", "B2", "B3", "B4",
                                                "B5", "B6", "B7", "B8", "B9", "B10", "B11", "B12", "B13", "B14", "RAM"};
                std::printf("MISMATCH at %08x:", v[0]);
                for (int k = 0; k < 34; ++k)
                    if (got[std::size_t(k)] != r[std::size_t(k)]) std::printf(" %s native %08x mame %08x", names[k], got[std::size_t(k)], r[std::size_t(k)]);
                std::printf("%s\n", m.faults.empty() ? "" : (" (fault: " + m.faults.front().what + ")").c_str());
            }
        }
    }
    if (!vf.eof() || !rf.eof()) { std::fprintf(stderr, "vector/result read failed\n"); return 2; }
    if (!total) { std::fprintf(stderr, "empty comparison: no vectors\n"); return 2; }
    for (const auto& [pc, c] : per_pc) std::printf("%08x: %d vectors, %d mismatches\n", pc, c.first, c.second);
    std::printf("%s: %d vectors, %d mismatches\n", bad ? "FAIL" : "PASS", total, bad);
    return bad ? 1 : 0;
}
