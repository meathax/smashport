// Retail high-score table reader shared by the player (score capture) and the
// replay verifier. Platform neutral: no Windows or network dependencies.
#pragma once
#include <array>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace stv::host {

// ---------------------------------------------------------------- score capture
// Retail HSTD: TOD_TAB in CMOS page 0 and ALL_TAB in page 2 (LINK.EQU). Each
// entry is an 8-digit BCD score, three initials and a checksum byte (sum of
// all eight bytes == 0xFF), one byte per CMOS word. ADD_ENTRY writes a new
// entry, its initials and checksum within one game process call, so a frame
// boundary never exposes a partially written valid entry.
struct ArcadeScore {
    std::array<unsigned char, 8> raw{};
    std::uint64_t score = 0;
    std::string name;
    bool operator<(const ArcadeScore& o) const { return raw < o.raw; }
    bool operator==(const ArcadeScore& o) const { return raw == o.raw; }
};

inline bool arcade_score_entry(const std::array<unsigned char, 8>& raw, ArcadeScore& out) {
    unsigned sum = 0;
    for (auto b : raw) sum += b;
    if ((sum & 255) != 255) return false;
    std::uint64_t score = 0;
    for (int i = 0; i < 4; ++i) {
        if ((raw[i] >> 4) > 9 || (raw[i] & 15) > 9) return false;
        score = score * 100 + (raw[i] >> 4) * 10 + (raw[i] & 15);
    }
    if (!score) return false;
    std::string name;
    for (int i = 4; i < 7; ++i) {
        char c = char(raw[i]);
        if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
        name.push_back(c >= 33 && c <= 125 ? c : ' ');
    }
    while (!name.empty() && name.back() == ' ') name.pop_back();
    while (!name.empty() && name.front() == ' ') name.erase(0, 1);
    out.raw = raw;
    out.score = score;
    out.name = name.empty() ? "???" : name;
    return true;
}

inline std::set<ArcadeScore> arcade_score_records(const std::vector<std::uint16_t>& cmos) {
    std::set<ArcadeScore> out;
    for (std::size_t base : {std::size_t(0x0000), std::size_t(0x2000)}) {
        for (std::size_t i = 0; i < 16; ++i) {
            const std::size_t at = base + i * 8;
            if (at + 8 > cmos.size()) break;
            std::array<unsigned char, 8> raw{};
            for (int j = 0; j < 8; ++j) raw[j] = static_cast<unsigned char>(cmos[at + j]);
            ArcadeScore score;
            if (arcade_score_entry(raw, score)) out.insert(score);
        }
    }
    return out;
}

}  // namespace stv::host
