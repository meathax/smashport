#pragma once
#include "base.hpp"
namespace stv {
constexpr usize max_archive_size=64*1024*1024;
constexpr usize max_entry_size=16*1024*1024;
constexpr unsigned max_zip_entries=1024;
struct ZipEntry {
 char name[256]{}; u32 crc{},compressed_size{},size{},local_offset{},data_offset{};
 u16 method{},flags{}; bool directory{};
};
struct ZipArchive {
 Bytes bytes{}; ZipEntry entries[max_zip_entries]{}; unsigned count{};
 Status open(Bytes input);
 Status extract(unsigned index,Buffer output) const;
};
Status inflate_raw(Bytes compressed,Buffer output,usize& written);
}
