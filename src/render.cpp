#include "stv/render.hpp"
namespace stv {
Status draw_indexed(Surface dst,IndexedImage src,const u32* pal,unsigned count,Blit op){
 if(!dst.pixels || !src.pixels.data || !pal || !count || count>256 || !dst.width || !dst.height || !src.width || !src.height || dst.stride<dst.width || src.stride<src.width)return Status::fail("Invalid raster dimensions or palette");
 if(dst.width>8192 || dst.height>8192 || src.width>8192 || src.height>8192 || src.stride>0x100000 || dst.stride>0x100000)return Status::fail("Raster dimensions exceed safe limits");
 if(u64(src.height-1)*src.stride+src.width>src.pixels.size)return Status::fail("Indexed image backing data is truncated");
 if(u64(dst.height-1)*dst.stride+dst.width>dst.pixel_capacity)return Status::fail("Destination backing data is truncated");
 // Validate BEFORE writing any destination pixel, so malformed input is transactional.
 for(unsigned y=0;y<src.height;++y)for(unsigned x=0;x<src.width;++x)if(src.pixels.data[usize(y)*src.stride+x]>=count)return Status::fail("Pixel index exceeds palette",usize(y)*src.stride+x);
 for(unsigned y=0;y<src.height;++y){i64 dy=i64(op.y)+y;if(dy<0 || dy>=dst.height)continue;unsigned sy=op.flip_y?src.height-1-y:y;
  for(unsigned x=0;x<src.width;++x){i64 dx=i64(op.x)+x;if(dx<0 || dx>=dst.width)continue;unsigned sx=op.flip_x?src.width-1-x:x;u8 index=src.pixels.data[usize(sy)*src.stride+sx];if(index || !op.transparent_zero)dst.pixels[usize(dy)*dst.stride+usize(dx)]=pal[index];}
 }return {};
}
}
