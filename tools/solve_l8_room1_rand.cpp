// Development tool: solve UTIL.ASM:RAND at L8 paid room-1 entry from executed
// evidence, using ONLY the native room owner as the forward model.
//
// Inputs (all pinned MAME 0.289 L8 evidence, see include/stv/l8_reference.hpp):
//   * the 35 HCOUNT reads of frame 3533, in bus order;
//   * the first Hulk's GET_VALID_XY Y return (first RANGRAND of the room).
// Every RAND/offset that can produce that first Y is enumerated by inverting
// RANDU/RANDOM, then the complete native OVERLORD -> ONSCRN_HULKS -> 4x OHULK
// graph is run and compared against all four captured placements, attempt
// counts, rejected X values and HAGE words. Exit 0 only if exactly one RAND
// equivalence class (RAND bit 0 is unobservable) and one offset survive.
#include "stv/session.hpp"
#include "stv/l8_reference.hpp"
#include <cstdio>
#include <fstream>
#include <memory>
#include <vector>
using namespace stv;

namespace {
bool matches(const SourceRoom& room){
    if(room.hulk_starts!=4||room.rejections.count!=9)return false;
    for(unsigned i=0;i<9;++i)if(room.rejected_x[i]!=l8_paid_room1_rejected_x[i])return false;
    unsigned seen=0;
    for(ProcessHandle h=room.processes.active_head;h!=no_process;h=room.processes.cells[h].next){
        const auto& p=room.processes.cells[h];
        if(p.id!=source_hulk_process_id)continue;
        if(seen>=4)return false;
        const auto& object=room.objects.cells[p.saved[0]];
        u32 x=0,y=0;if(!sprite_anchor(object.sprite,x,y))return false;
        const auto& want=l8_paid_room1_hulks[seen];
        if(x!=want.x+0x01d80000u||y!=want.y+0x01520000u)return false;
        u32 hage=0;if(!source_hulk_pdata_read(p,SourceHulkField::age,hage)||hage!=want.hage)return false;
        ++seen;
    }
    return seen==4;
}
}

int main(int argc,char** argv){
    if(argc!=2){std::fprintf(stderr,"usage: stv_solve_l8_room1_rand data/source.stva\n");return 2;}
    std::ifstream file(argv[1],std::ios::binary);
    std::vector<u8> bytes((std::istreambuf_iterator<char>(file)),{});
    auto pack=std::make_unique<SourcePack>();
    if(!pack->open({bytes.data(),bytes.size()})){std::fprintf(stderr,"source pack failed\n");return 2;}
    auto session=std::make_unique<GameSession>();
    if(!session->load(*pack,GameProfile::reference_l8())||!session->prepare_room(0,1)){std::fprintf(stderr,"room selection failed\n");return 2;}
    const WaveOverlord selected=session->room_flow();
    auto room=std::make_unique<SourceRoom>();
    constexpr u32 reads=sizeof l8_paid_room1_hcount_frame3533/sizeof l8_paid_room1_hcount_frame3533[0];
    // First GV draw: RANGRAND(21h,C1h) -> YVAL = RANDU(A0h<<16) + 21h<<16.
    const u32 range=0x00c10000u-0x00210000u;
    const u32 randu=l8_paid_room1_hulks[0].y-0x00210000u; // RANDU result, 1-based
    unsigned candidates=0,solutions=0;u32 solved=0,solved_offset=0,rand_after=0;u32 classes=0;
    for(u32 offset=0;offset<reads;++offset){
        const u64 low=(u64(randu-1)<<32)/range,high=(u64(randu)<<32)/range+1;
        for(u64 value=low;value<=high&&value<=0xffffffffull;++value){
            if(u32((value*range)>>32)!=randu-1)continue;
            const u32 k=l8_paid_room1_hcount_frame3533[offset]&31u;
            const u32 r=u32(value),rotated=k?(r>>k)|(r<<(32-k)):r;
            for(u32 bit0=0;bit0<2;++bit0){
                const u32 seed=((rotated>>1)&~1u)|bit0|((rotated&1u)<<31);
                SourceRng probe{seed};if(probe.random(l8_paid_room1_hcount_frame3533[offset])!=r)continue;
                ++candidates;
                SourceRoomStart start{};start.rand=seed;
                start.hcount=l8_paid_room1_hcount_frame3533+offset;start.hcount_count=reads-offset;
                start.from_door=0;start.player_present[0]=true;start.player_x[0]=662u<<16;start.player_y[0]=463u<<16;
                if(!room->start(selected,session->fixture().hulk_art,session->fixture().hulk_secondary_art,start))continue;
                bool ok=true;for(unsigned t=0;t<4&&ok;++t)ok=bool(room->tick());
                if(!ok||!matches(*room))continue;
                ++solutions;
                if(!classes||(seed>>1)!=(solved>>1)||offset!=solved_offset)++classes;
                solved=seed&~1u;solved_offset=offset;rand_after=room->rng.seed;
                std::printf("SOLUTION rand=%08x offset=%u reads_consumed=%u rand_after=%08x\n",
                            seed,offset,room->hcount.position,rand_after);
            }
        }
    }
    std::printf("{\"candidates\":%u,\"solutions\":%u,\"equivalence_classes\":%u,\"rand\":\"%08x\",\"hcount_offset\":%u,\"rand_after\":\"%08x\"}\n",
                candidates,solutions,classes,solved,solved_offset,rand_after);
    if(classes!=1||solved!=l8_paid_room1_rand_solved||solved_offset!=0||rand_after!=l8_paid_room1_rand_after_hulks){
        std::fprintf(stderr,"FAIL: RAND solution is not unique or differs from l8_reference.hpp\n");return 1;
    }
    std::printf("PASS: unique RAND class; solved, not measured\n");
    return 0;
}
