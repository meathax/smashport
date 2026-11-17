#pragma once
#include <filesystem>
#include <string>

namespace stv::gsp {
struct Machine;
// The import contains the fourteen verified chips in board socket order.
bool import_rom_set(const std::filesystem::path& selected, const std::filesystem::path& temporary,
                    std::string& error);
bool load_imported_roms(Machine& machine, const std::filesystem::path& path, std::string& error);
}
