#pragma once
#include "zip.hpp"
namespace stv {
constexpr unsigned reference_rom_count=14;
constexpr usize rom_scratch_size=0x20000;
constexpr usize package_header_size=4096;
constexpr usize main_region_size=0x100000, graphics_region_size=0x800000, sound_region_size=0x90000;
constexpr usize asset_package_size=package_header_size+main_region_size+graphics_region_size+sound_region_size;
constexpr u32 retail_l8_reference_id=0x00010008; // Reference data, NOT source-compatibility certification.
struct RomSpec {
 const char* name; u32 size,crc; const char* sha1_text;
 u32 region,offset,stride,reload; // reload==0 means no reload; regions: main=0,gfx=1,sound=2
};
extern const RomSpec l8_reference_roms[reference_rom_count];
struct ImportReport {
 int entry[reference_rom_count]{}; Sha256 content[reference_rom_count]{};
 Sha256 archive{}; unsigned found{},ignored{}; u32 missing_mask{};
};
// Profiles are passed explicitly for isolated synthetic tests. Shipping frontends use ONLY l8_reference_roms.
Status scan_roms(const ZipArchive& zip,const RomSpec (&spec)[reference_rom_count],Buffer scratch,ImportReport& out);
Status make_package(const ZipArchive& zip,const RomSpec (&spec)[reference_rom_count],u32 profile,
                    const ImportReport& report,Buffer scratch,Buffer output);
Status verify_package(Bytes package,const RomSpec (&spec)[reference_rom_count],u32 expected_profile,Buffer scratch);
struct TextWriter {
 char* data{};usize capacity{},size{};bool overflow{};
 void text(const char* value);void number(u64 value);void quoted(const char* value);void digest(const u8* value,usize count);
};
Status write_manifest(const ZipArchive& zip,const RomSpec (&spec)[reference_rom_count],u32 profile,
                      const ImportReport& report,Bytes package,TextWriter& output);
}
