#include "stv/animation.hpp"
namespace stv {
const SourceAnimation* find_animation(const char* m,const char* l){for(unsigned i=0;i<source_animation_count;++i)if(name_equal(m,source_animations[i].module)&&name_equal(l,source_animations[i].label))return &source_animations[i];return nullptr;}
Status encode_source_animation_cursor(const SourceAnimation& animation,u16 frame,u32& token){
 unsigned id=0;
 for(unsigned i=0;i<source_animation_count;++i)if(&source_animations[i]==&animation){id=i+1;break;}
 if(!id||id>0x0fffu)return Status::fail("Source animation is outside the stable native catalog");
 if(frame>animation.count)return Status::fail("Source animation cursor exceeds its frame list",frame);
 token=0xa0000000u|(u32(id)<<16)|frame;return {};
}
Status decode_source_animation_cursor(u32 token,const SourceAnimation*& animation,u16& frame){
 if((token&0xf0000000u)!=0xa0000000u)return Status::fail("A9 is not a stable native source-animation token",token);
 const unsigned id=(token>>16)&0x0fffu;
 if(!id||id>source_animation_count)return Status::fail("A9 source-animation ID is outside the generated catalog",id);
 const auto& selected=source_animations[id-1];const u16 cursor=u16(token);
 if(cursor>selected.count)return Status::fail("A9 source-animation cursor exceeds its frame list",cursor);
 animation=&selected;frame=cursor;return {};
}
namespace {void offset(const EditorImage& e,u16 f,u32& x,u32& y){i32 ax=e.anchor_x,ay=e.anchor_y;if(f&16)ax=i32(e.stride)-1-ax;if(f&32)ay=i32(e.height)-1-ay;x=u32(ax)<<16;y=u32(ay)<<16;}}
Status sprite_image(const NativeSprite& s,EditorImage& out){if(s.raw_image){out=*s.raw_image;return {};}if(!s.art)return Status::fail("Sprite has no image header");return s.art->image(s.image,out);}
Status sprite_palette(const NativeSprite& s,EditorPalette& out){if(s.palette_view){out=*s.palette_view;return {};}if(!s.art)return Status::fail("Sprite has no palette owner");EditorImage e;auto status=sprite_image(s,e);if(!status)return status;return s.art->palette(s.palette<0?unsigned((e.selector&255)-3):unsigned(s.palette),out);}
Status sprite_anchor(const NativeSprite& s,u32& x,u32& y){if(!s.art)return Status::fail("Sprite has no source image bank");EditorImage e;auto status=sprite_image(s,e);if(!status)return status;u32 ax,ay;offset(e,s.flags,ax,ay);x=s.position.x+ax;y=s.position.y+ay;return {};}
Status place_sprite_anchor(NativeSprite& s,u32 x,u32 y){if(!s.art)return Status::fail("Sprite has no source image bank");EditorImage e;auto status=sprite_image(s,e);if(!status)return status;u32 ax,ay;offset(e,s.flags,ax,ay);s.position.x=x-ax;s.position.y=y-ay;return {};}
Status set_animation_image(NativeSprite& s,unsigned image,u16 flags){u32 x,y;auto status=sprite_anchor(s,x,y);if(!status)return status;EditorImage e;status=s.art->image(image,e);if(!status)return status;u32 ax,ay;offset(e,flags,ax,ay);if(s.palette<0){EditorImage old;status=sprite_image(s,old);if(!status)return status;s.palette=int((old.selector&255)-3);}s.raw_image=nullptr;s.image=image;s.flags=flags;s.position.x=x-ax;s.position.y=y-ay;return {};}
Status resolve_animation_image(const EditorImages& bank,const char* name,unsigned& index){
 int found=bank.find(name);
 if(found>=0){index=unsigned(found);return {};}
 if(found==-1)return Status::fail("Animation image missing from selected original bank");
 // PLYRS2.IMG has P1SG9T3A twice with different anchors. BACKUP/IMGTBL.ASM
 // lines 267-269 explicitly give width 21,height 16,anchor (15,-25): directory
 // entry 158, not entry 107 (14,-24). This is a documented lab binding to the
 // exact uploaded bank, NOT a blanket assertion that BACKUP matches retail.
 static constexpr u8 bank_sha[32]={0x69,0x69,0xc5,0xac,0xa7,0x9a,0x8a,0x65,0x58,0xed,0xb2,0x66,0x2b,0xf2,0xd2,0xd6,0xac,0xa7,0x6f,0x20,0x8a,0x0f,0x23,0x44,0x23,0xd8,0x7f,0x1a,0x45,0xd5,0xa8,0xe1};
 if(name_equal(name,"P1SG9T3A")){
  auto digest=sha256(bank.data);
  if(equal(digest.bytes,bank_sha,32)){index=158;return {};}
 }
 return Status::fail("Ambiguous original image name; no verified binding for this bank");
}
Status apply_source_frame(NativeSprite& s,const SourceFrame& f){
    return apply_source_frame(s,f,nullptr);
}
Status apply_source_frame(NativeSprite& s,const SourceFrame& f,
                          const EditorImages* secondary_bank){
    if(!s.art)return Status::fail("Missing frame bank");
    if(f.control&~0x60ffu)return Status::fail("Unsupported animation control bits",f.control);

    const EditorImages* target_bank=s.art;
    unsigned image=0;
    auto resolution=resolve_animation_image(*target_bank,f.image,image);
    if(!resolution){
        // A second bank is a source-selected fallback, never a way to hide an
        // ambiguous image in the sprite's owning bank.
        if(!secondary_bank||secondary_bank==s.art||s.art->find(f.image)!=-1)
            return resolution;
        const int alternate=secondary_bank->find(f.image);
        if(alternate==-2)return Status::fail("Animation image is ambiguous in secondary source bank");
        if(alternate<0)return resolution;
        target_bank=secondary_bank;image=unsigned(alternate);
    }

    u32 anchor_x=0,anchor_y=0;
    auto status=sprite_anchor(s,anchor_x,anchor_y);if(!status)return status;
    u16 flags=s.flags;
    if(f.control&0x4000u)flags=u16((flags&~0x30u)|f.flip_bits);
    int palette=s.palette;
    if(palette<0){
        EditorImage original;status=sprite_image(s,original);if(!status)return status;
        palette=int((original.selector&255)-3);
    }

    if(f.control&0x2000u){
        bool found=false;
        for(unsigned i=0;i<target_bank->palette_count;++i){
            EditorPalette candidate;status=target_bank->palette(i,candidate);if(!status)return status;
            if(!name_equal(candidate.name,f.palette))continue;
            if(found){
                EditorPalette prior;status=target_bank->palette(unsigned(palette),prior);if(!status)return status;
                if(candidate.count!=prior.count||candidate.words.size!=prior.words.size||
                   !equal(candidate.words.data,prior.words.data,candidate.words.size))
                    return Status::fail("Ambiguous animation palette name");
            }else{palette=int(i);found=true;}
        }
        if(!found)return Status::fail("Animation palette not present in selected bank");
    }else if(target_bank!=s.art&&!s.palette_view){
        // Palette indices are local to an IMG bank. Preserve the actual colors
        // across a bank change and fail explicitly if no exact palette exists.
        EditorPalette prior;status=sprite_palette(s,prior);if(!status)return status;
        bool found=false;
        for(unsigned i=0;i<target_bank->palette_count;++i){
            EditorPalette candidate;status=target_bank->palette(i,candidate);if(!status)return status;
            if(candidate.count==prior.count&&candidate.words.size==prior.words.size&&
               equal(candidate.words.data,prior.words.data,prior.words.size)){
                palette=int(i);found=true;break;
            }
        }
        if(!found)return Status::fail("No color-identical palette exists in secondary source bank");
    }

    EditorImage selected;status=target_bank->image(image,selected);if(!status)return status;
    u32 offset_x=0,offset_y=0;offset(selected,flags,offset_x,offset_y);
    NativeSprite next=s;next.art=target_bank;next.image=image;next.flags=flags;
    next.position.x=anchor_x-offset_x;next.position.y=anchor_y-offset_y;
    next.palette=palette;next.raw_image=nullptr;
    if(f.control&0x2000u)next.palette_view=nullptr;
    s=next;
    return {};
}
Status draw_sprite(const NativeSprite& s,Surface out){if((s.flags&0x100)||!(s.flags&15))return {};EditorImage e;auto result=sprite_image(s,e);if(!result)return result;EditorPalette p;result=sprite_palette(s,p);if(!result)return result;u32 colors[256]{};for(unsigned i=0;i<p.count;++i)colors[i]=editor_color(le16(p.words.data+i*2));return draw_indexed(out,{e.pixels,e.stride,e.height,e.stride},colors,p.count,{signed16(u16(s.position.x>>16)),signed16(u16(s.position.y>>16)),bool(s.flags&16),bool(s.flags&32),bool((s.flags&3)!=3)});}
u8 screen_test(const NativeSprite& s,i32 left,i32 top,i32 right,i32 bottom){EditorImage e;if(!s.art||!sprite_image(s,e))return 255;i32 x=signed16(u16(s.position.x>>16)),y=signed16(u16(s.position.y>>16));if(y>=bottom)return 4;if(x>=right)return 2;if(y+i32(e.height)<=top)return 3;if(x+i32(e.stride)<=left)return 1;return 0;}
}
