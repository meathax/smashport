#pragma once
#include "animation.hpp"
#include "hulk_process.hpp"
namespace stv {
using ObjectHandle=u16;constexpr ObjectHandle no_object=0xffff;constexpr unsigned object_capacity=420;
struct NativeObject {NativeSprite sprite{};ObjectHandle next{no_object};ProcessHandle process{no_process};u16 id{},z{};u16 clip{};u32 dma_xy{};bool allocated{},listed{};};
struct NativeObjects {
 NativeObject cells[object_capacity]{};ObjectHandle head{no_object},free_head{no_object};unsigned count{};
 void initialize();Status allocate(ObjectHandle&);Status insert(ObjectHandle);Status erase(ObjectHandle);Status validate()const;
 bool sort_pass(); // original YZSORT is one adjacent-swap pass, NOT a complete stable sort
 void integrate();Status draw(Surface)const;
};
// L8 HULKS.ASM:IH1 and NDSP1.ASM:BEGINOBJ2 for an on-screen Hulk. Handles
// stand in for source addresses; no virtual CPU or host pointer is persisted.
constexpr u16 source_hulk_object_id=0x8600; // CLSENMY | TYPHULK
constexpr u16 source_hulk_initial_flags=0x8012; // DMAWNZ | M_INUSE | M_FLIPH
constexpr u16 source_hulk_initial_z=159;
Status source_ohulk_beginobj2(NativeProcesses&,NativeObjects&,ProcessHandle,
                              const EditorImages&,u16 hulk_type,
                              const SourceOhulkCoordinates&,ObjectHandle&);
Status source_pixel_overlap(const NativeSprite&,const NativeSprite&,bool& hit);
}
