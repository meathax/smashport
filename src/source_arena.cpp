#include "stv/source_arena.hpp"
namespace stv {
u16 source_arena_flags(u16 map_flags) {
    // BAKGND ADDNEW, BSETPAL: bit 6 selects transparent-zero DMA mode 2;
    // map bits 4/5 become the normal horizontal/vertical flip bits.
    return u16(0x8000u|((map_flags&64)?2u:3u)|(map_flags&0x30));
}
u16 source_arena_foreground_z(u16 z) {
    // WAVES PUTOBJ: these exceptions precede ordinary z+128 promotion.
    if(z==118||z==119)return u16(z-100);
    if(z>=120&&z<=123)return u16(z+10);
    if(z==100||z==101)return u16(z+0x2b);
    if(z==102)return u16(z+111);
    return u16(z+128);
}
Status SourceArena::bind(const EditorBackground& b) {
    if(source_arena_header_count!=64||b.block_count!=64)return Status::fail("Linked L7 header count does not match supplied FINAL7 blocks");
    for(unsigned i=0;i<64;++i) {
        const auto& h=source_arena_headers[i];const auto& block=b.blocks[i];
        if(h.width!=block.width||h.height!=block.height||block.pixels.size!=usize(h.width)*h.height)
            return Status::fail("Linked L7 block dimensions do not match original BDD",i);
        auto& image=images[i];image={};image.name[0]='B';image.name[1]=char('0'+i/10);image.name[2]=char('0'+i%10);
        image.width=h.width;image.height=h.height;image.stride=h.width;image.pixels=block.pixels;
        // Background image headers have no animation origin offsets.
        for(usize n=0;n<image.pixels.size;++n)if(image.pixels.data[n]>=64)return Status::fail("Linked arena pixel exceeds 64-color palette",i);
    }
    for(unsigned i=0;i<source_arena_placement_count;++i) {
        const auto& p=source_arena_placements[i];
        if(p.block>=64||p.palette>=2)return Status::fail("Linked arena placement reference invalid",i);
    }
    return {};
}
Status SourceArena::draw_background(Surface out)const {
    u32 colors[2][64];
    for(unsigned p=0;p<2;++p)for(unsigned c=0;c<64;++c)colors[p][c]=editor_color(le16(source_arena_palettes[p].words.data+c*2));
    // Static blocks remain on the background list. WAVES GBOBJ promotes
    // z>=100 onto the foreground list; SourceLab inserts those native objects.
    for(unsigned z=0;z<100;++z)for(unsigned i=0;i<source_arena_placement_count;++i) {
        const auto& p=source_arena_placements[i];if((p.flags>>8)!=z)continue;
        const auto& b=images[p.block];auto s=draw_indexed(out,{b.pixels,b.width,b.height,b.stride},colors[p.palette],64,
            {p.x,p.y,bool(p.flags&16),bool(p.flags&32),bool(p.flags&64)});
        if(!s)return s;
    }
    return {};
}
}
