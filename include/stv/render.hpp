#pragma once
#include "base.hpp"
namespace stv {
// Platform raster service, NOT a claimed translation of NDSP1/DMA yet.
struct Surface {u32* pixels{};unsigned width{},height{},stride{};usize pixel_capacity{};};
struct IndexedImage {Bytes pixels{};unsigned width{},height{},stride{};};
struct Blit {i32 x{},y{};bool flip_x{},flip_y{},transparent_zero=true;};
Status draw_indexed(Surface target,IndexedImage image,const u32* palette,unsigned palette_count,Blit op);
}
