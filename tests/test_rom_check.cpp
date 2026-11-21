// ROM set check (PLAN P8): renamed files are found by CRC, and every missing,
// truncated or corrupted file is named in one error message.
//   stv_rom_check_tests smashtv.zip work_dir
#include "stv/gsp/machine.hpp"
#include "stv/zip.hpp"
#include "stv/gsp/rom_import.hpp"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
using namespace stv;
using stv::gsp::Machine;
namespace {
struct File { std::string name; std::vector<u8> data; };
int failures = 0;
void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++failures;
}
void add16(std::vector<u8>& z, u32 x) { z.push_back(u8(x)); z.push_back(u8(x >> 8)); }
void add32(std::vector<u8>& z, u32 x) { add16(z, x & 0xFFFF); add16(z, x >> 16); }
// stored (uncompressed) ZIP writer, independent of the reader under test
std::vector<u8> write_zip(const std::vector<File>& files) {
    std::vector<u8> z, cd;
    for (const auto& f : files) {
        const u32 crc = crc32({f.data.data(), f.data.size()}), at = u32(z.size());
        add32(z, 0x04034b50); add16(z, 20); add16(z, 0); add16(z, 0); add16(z, 0); add16(z, 0);
        add32(z, crc); add32(z, u32(f.data.size())); add32(z, u32(f.data.size()));
        add16(z, u32(f.name.size())); add16(z, 0);
        z.insert(z.end(), f.name.begin(), f.name.end());
        z.insert(z.end(), f.data.begin(), f.data.end());
        add32(cd, 0x02014b50); add16(cd, 20); add16(cd, 20); add16(cd, 0); add16(cd, 0); add16(cd, 0); add16(cd, 0);
        add32(cd, crc); add32(cd, u32(f.data.size())); add32(cd, u32(f.data.size()));
        add16(cd, u32(f.name.size())); add16(cd, 0); add16(cd, 0); add16(cd, 0); add16(cd, 0); add32(cd, 0); add32(cd, at);
        cd.insert(cd.end(), f.name.begin(), f.name.end());
    }
    const u32 start = u32(z.size());
    z.insert(z.end(), cd.begin(), cd.end());
    add32(z, 0x06054b50); add16(z, 0); add16(z, 0); add16(z, u32(files.size())); add16(z, u32(files.size()));
    add32(z, u32(cd.size())); add32(z, start); add16(z, 0);
    return z;
}
std::string load(const std::filesystem::path& dir, const std::vector<File>& files, Machine& m, bool& ok) {
    const auto path = dir / "rom_check.zip";
    const auto z = write_zip(files);
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(z.data()), std::streamsize(z.size()));
    std::string err;
    ok = m.load_roms(path.string(), err);
    return err;
}
bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }
}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: %s smashtv.zip work_dir\n", argv[0]); return 2; }
    std::ifstream in(argv[1], std::ios::binary);
    std::vector<u8> bytes((std::istreambuf_iterator<char>(in)), {});
    static ZipArchive zip;
    if (!zip.open({bytes.data(), bytes.size()})) { std::fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
    std::vector<File> orig;
    for (unsigned i = 0; i < zip.count; ++i) {
        if (zip.entries[i].directory) continue;
        File f{zip.entries[i].name, std::vector<u8>(zip.entries[i].size)};
        if (!zip.extract(i, {f.data.data(), f.data.size()})) { std::fprintf(stderr, "extract failed\n"); return 2; }
        orig.push_back(std::move(f));
    }
    const std::filesystem::path dir = argv[2];
    std::filesystem::create_directories(dir);
    auto ref = std::make_unique<Machine>();
    auto m = std::make_unique<Machine>();
    bool ok = false;
    std::string err = load(dir, orig, *ref, ok);
    check(ok && err.empty(), "original set (re-zipped, stored) loads");

    // every file renamed and in reverse order: found by CRC, same program, graphics and sound
    std::vector<File> renamed(orig.rbegin(), orig.rend());
    for (std::size_t i = 0; i < renamed.size(); ++i) renamed[i].name = "renamed_" + std::to_string(i) + ".bin";
    err = load(dir, renamed, *m, ok);
    check(ok && m->prog == ref->prog && m->gfx == ref->gfx && m->snd_u4 == ref->snd_u4 && m->snd_u19 == ref->snd_u19 &&
              m->snd_u20 == ref->snd_u20, "renamed files are matched by CRC and decode identically");

    const auto picked = dir / "selected.zip";
    const auto parent = dir / "smashtv.zip";
    const auto cache = dir / "retail-l8.dat";
    auto save = [&](const auto& path, const std::vector<File>& members) {
        auto z = write_zip(members);
        std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(z.data()), z.size());
    };
    auto populated = [&] {
        return stv::gsp::load_imported_roms(*m, cache, err) && m->prog == ref->prog &&
               m->gfx == ref->gfx && m->snd_u4 == ref->snd_u4 &&
               m->snd_u19 == ref->snd_u19 && m->snd_u20 == ref->snd_u20;
    };
    save(picked, orig);
    check(stv::gsp::import_rom_set(picked, cache, err) && populated(), "non-merged ZIP imports into embedded-player cache");
    for (auto& f : renamed) f.name = "merged/" + f.name;
    auto merged = renamed;
    merged.push_back({"la8_smash_tv_game_rom_u105.u105", {42}});
    save(picked, merged);
    check(stv::gsp::import_rom_set(picked, cache, err) && populated(), "merged nested chips beat conflicting filenames");
    save(picked, {renamed.begin(), renamed.begin() + 2});
    save(parent, {renamed.begin() + 2, renamed.end()});
    check(stv::gsp::import_rom_set(picked, cache, err), "split set resolves sibling chips");
    std::filesystem::remove(parent);
    check(!stv::gsp::import_rom_set(picked, cache, err), "incomplete split set rejected");
    std::filesystem::remove(picked);
    check(populated(), "restart without source ZIPs reproduces all game data");
    std::ofstream(cache, std::ios::binary | std::ios::app).put(0);
    check(!stv::gsp::load_imported_roms(*m, cache, err), "damaged saved import rejected");
    std::filesystem::remove(cache);

    // one missing, one corrupted, one truncated: all three named, nothing else
    std::vector<File> bad;
    for (const auto& f : orig) {
        if (f.name == "la8_smash_tv_game_rom_u89.u89") continue;
        bad.push_back(f);
        if (f.name == "la1_smash_tv_game_rom_u111.u111") bad.back().data[0x1234] ^= 0x40;
        if (f.name == "sl2_smash_tv_sound_rom_u4.u4") bad.back().data.resize(0x8000);
    }
    err = load(dir, bad, *m, ok);
    std::size_t lines = 0;
    for (char c : err) lines += c == '\n';
    check(!ok && has(err, "missing: la8_smash_tv_game_rom_u89.u89") &&
              has(err, "wrong CRC: la1_smash_tv_game_rom_u111.u111") &&
              has(err, "wrong size: sl2_smash_tv_sound_rom_u4.u4 (32768 bytes, expected 65536)") && lines == 4,
          "missing, corrupted and truncated files are each reported (" + std::to_string(lines) + " lines)");

    err = load(dir, {}, *m, ok);
    lines = 0;
    for (char c : err) lines += c == '\n';
    check(!ok && !err.empty() && (lines == 0 || lines == 15), "empty ZIP is rejected (" + err.substr(0, err.find('\n')) + ")");
    std::filesystem::remove(dir / "rom_check.zip");
    return failures ? 1 : 0;
}
