#pragma once
#include "source_assets.hpp"
#include "gameplay.hpp"
namespace stv {
struct SourceFrame {const char* image;u16 control;u16 flip_bits;const char* palette;};
struct SourceAnimation {const char* module;const char* label;u32 line;const SourceFrame* frames;u16 count;};
extern const SourceAnimation source_animations[];extern const unsigned source_animation_count;
const SourceAnimation* find_animation(const char* module,const char* label);
// Stable, pointer-free handle for an immutable source script and FRANIM cursor.
// Token values are native IDs, never source virtual addresses or host pointers.
Status encode_source_animation_cursor(const SourceAnimation&,u16 frame,u32& token);
Status decode_source_animation_cursor(u32 token,const SourceAnimation*& animation,u16& frame);
struct NativeSprite {
 const EditorImages* art{};unsigned image{};int palette{-1};u16 flags{2};SourcePosition position{};
 // Independent original image-header / palette ownership for promoted BG objects.
 const EditorImage* raw_image{};const EditorPalette* palette_view{};
};
// ANI/GANIOF: preserve the animation anchor, including both flips. Positions
// remain wrapping 16.16 bit patterns; no host signed-overflow dependency.
// Explicit pinned binding for the one conflicting editor-image name.
Status resolve_animation_image(const EditorImages&,const char* name,unsigned& index);
Status set_animation_image(NativeSprite&,unsigned image,u16 flags);
Status apply_source_frame(NativeSprite&,const SourceFrame&);
// Some L8 FRANIM scripts reference frames split across original IMG banks.
// Resolve the primary sprite bank first, and consult this optional secondary
// bank only when the image is absent from the primary bank.
Status apply_source_frame(NativeSprite&,const SourceFrame&,const EditorImages* secondary_bank);
Status sprite_image(const NativeSprite&,EditorImage&);
Status sprite_palette(const NativeSprite&,EditorPalette&);
Status sprite_anchor(const NativeSprite&,u32& x,u32& y);
Status place_sprite_anchor(NativeSprite&,u32 x,u32 y);
Status draw_sprite(const NativeSprite&,Surface);
u8 screen_test(const NativeSprite&,i32 left,i32 top,i32 right,i32 bottom);
}
