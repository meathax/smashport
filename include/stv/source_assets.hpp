#pragma once
#include "base.hpp"
#include "render.hpp"
namespace stv {
// Views borrow their input buffer. All byte offsets are file offsets, never host pointers.
struct SourceEntry {char name[48]{};Bytes bytes{};};
struct SourcePack {
 static constexpr unsigned max_entries=128;
 SourceEntry entries[max_entries]{};unsigned count{};
 Bytes raw{};Sha256 verified_fingerprint{};
 Status open(Bytes data);
 // Hash the entire validated import pack, including names and file bytes.
 // Recheck borrowed storage before advertising it to replay/online peers.
 Status fingerprint(Sha256& out)const;
 const SourceEntry* find(const char* name) const;
};
struct EditorPalette {char name[16]{};u16 count{};Bytes words{};};
struct EditorImage {char name[17]{};i32 anchor_x{},anchor_y{};u16 width{},height{},selector{};u32 stride{};Bytes pixels{};};
struct EditorImages {
 Bytes data{};u16 count{},palette_count{};u32 directory{};
 Status open(Bytes input);
 Status image(unsigned index,EditorImage& out)const;
 Status palette(unsigned index,EditorPalette& out)const;
 // -1 absent; -2 same-name records have differing metadata or pixels.
 int find(const char* name)const;
};
// BDD/BDB data remains separated by original editor file: equal palette names do
// not imply equal colors. The fourth BDD block field is preserved, not guessed.
struct EditorBlock {u32 id{},width{},height{},editor_field{};Bytes pixels{};};
struct EditorPlacement {u16 flags{};i32 x{},y{};u32 block{};u16 palette{};bool palette_valid{true};};
struct EditorModule {char name[32]{};i32 left{},right{},top{},bottom{};};
struct EditorBackground {
 static constexpr unsigned max_blocks=512,max_palettes=32,max_modules=32,max_placements=2048;
 EditorBlock blocks[max_blocks]{};EditorPalette palettes[max_palettes]{};
 EditorModule modules[max_modules]{};EditorPlacement placements[max_placements]{};
 unsigned block_count{},palette_count{},module_count{},placement_count{},invalid_palette_placements{};
 Status open(Bytes bdd,Bytes bdb);
 int find_module(const char* name)const;
 Status draw_module(unsigned index,Surface out)const;
};
u32 editor_color(u16 word); // xRRRRRGGGGGBBBBB -> opaque 0x00RRGGBB
Status draw_editor_image(const EditorImages&,unsigned image_index,Surface,i32 x,i32 y,bool flip_x=false,bool flip_y=false,int palette_override=-1);
bool name_equal(const char*,const char*);
}
