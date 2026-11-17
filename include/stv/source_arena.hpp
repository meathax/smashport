#pragma once
#include "source_assets.hpp"
namespace stv {
struct ArenaHeader {u16 width,height;u32 original_graphics_bit_address;};
struct ArenaPlacement {u16 flags;i16 x,y;u16 block,palette;};
extern const ArenaHeader source_arena_headers[];
extern const ArenaPlacement source_arena_placements[];
extern const unsigned source_arena_header_count,source_arena_placement_count;
extern const EditorPalette source_arena_palettes[2];
struct SourceArena {
    EditorImage images[64]{};
    Status bind(const EditorBackground&);
    Status draw_background(Surface)const;
};
u16 source_arena_flags(u16 map_flags);
u16 source_arena_foreground_z(u16 source_z);
}
