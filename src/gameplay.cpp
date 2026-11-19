#include "stv/gameplay.hpp"
namespace stv {
u32 SourceRng::random(u16 count){u32 old=seed;bool overflow=((old^(old<<1))&0x80000000u)!=0;u32 shifted=old<<1;if(!overflow)shifted|=2;seed=shifted;return rotl(shifted+(old>>31),count&31);}
u32 SourceRng::randu(u32 limit,u16 count){return u32((u64(random(count))*limit)>>32)+1;}
u32 SourceRng::range(u32 lower,u32 upper,u16 count){return randu(upper-lower,count)+lower;}
// HULKS.ASM:GV draws YVAL from [21h,C1h] first, then X from [2Ah,157h].
SourceSpawnBounds l8_source_hulk_spawn_bounds(){return {0x002a0000u,0x01570000u,0x00210000u,0x00c10000u};}
Status source_red_carpet(u16 door,SourceRectangle& out){
    constexpr u16 left_x[5]={0,0x98,0,0x88,0x88};
    constexpr u16 right_x[5]={0xf8,0x200,0xf8,0xf8,0xf8};
    constexpr u16 top_y[5]={0x48,0x48,0x48,0x44,0};
    constexpr u16 bottom_y[5]={0xb8,0xb8,0xb8,0xff,0xc8};
    if(door>=5)return Status::fail("GET_VALID_XY FROMDR outside RED_CARPET tables",door);
    out={u32(left_x[door])<<16,u32(right_x[door])<<16,u32(top_y[door])<<16,u32(bottom_y[door])<<16};
    return {};
}
Status source_get_valid_xy_circle(const SourceCirclePlacementInput& input,SourceSpawnReservations& reserved,SourceCirclePlacement& out){
    // GET_VALID_XY: HULK, not INAMODE, CTBL wave, regular HULKTYP, nonzero TMP.
    // The source indexes each .LONG as 32 bits (TMP after DEC), then MOVX/MOVY
    // reconstruct the packed Y:X pair. Coordinates below are the source
    // TABLEX/TABLEX1/TABLEX3 literals, already evaluated by the assembler.
    SourceCirclePlacement result{};
    if(!input.hulk_actor||input.attract_mode){out=result;return {};}
    if(input.wave<1||input.wave>10)
        return Status::fail("GET_VALID_XY CTBL wave outside source table",input.wave);
    if(input.wave<3||input.wave>8||input.hulk_type!=0||input.tmp==0){out=result;return {};}
    if(input.tmp>16)return Status::fail("GET_VALID_XY CTBL TMP outside 16-point table",input.tmp);
    if(reserved.count==0||reserved.count>SourceSpawnReservations::capacity)
        return Status::fail("GET_VALID_XY OEXIT KEEPXY live count is invalid",reserved.count);
    if(reserved.count==SourceSpawnReservations::capacity)
        return Status::fail("GET_VALID_XY OEXIT KEEPXY has no room",reserved.count);

    using Point=u16[2];
    static constexpr Point table_x[16]={
        {0x50,0x28},{0x50,0x48},{0x50,0x68},{0x50,0x88},
        {0x42,0xa8},{0x3c,0xc8},{0x48,0xe8},{0x62,0xfc},
        {0x98,0x28},{0x98,0x48},{0x98,0x68},{0x98,0x88},
        {0xa6,0xa8},{0xac,0xc8},{0xa0,0xe8},{0x86,0xfc}
    };
    static constexpr Point table_x1[16]={
        {0x20,0xa4},{0x35,0xa4},{0x4a,0xa4},{0x5f,0x9c},
        {0x78,0x92},{0x95,0x92},{0xaa,0xa0},{0xb7,0xb7},
        {0x20,0xee},{0x35,0xee},{0x4a,0xee},{0x5f,0xf6},
        {0x78,0x100},{0x95,0x100},{0xaa,0xf8},{0xb7,0xe1}
    };
    static constexpr Point table_x3[16]={
        {0xd0,0xa4},{0xbb,0xa4},{0xa6,0xa4},{0x91,0x9c},
        {0x78,0x92},{0x5b,0x92},{0x46,0xa0},{0x39,0xb7},
        {0xd0,0xee},{0xbb,0xee},{0xa6,0xee},{0x91,0xf6},
        {0x78,0x100},{0x5b,0x100},{0x46,0xf8},{0x39,0xe1}
    };
    // CTBL1 is indexed by wave-1. Only waves 3..8 reach this selection.
    const Point* table=table_x;
    if(input.wave==3||input.wave==8)table=table_x3;
    else if(input.wave==6)table=table_x1;

    const u16 index=u16(input.tmp-1); // DEC TMP, then SLL 5 for a .LONG entry.
    const u16 x=table[index][0],y=table[index][1];
    const u32 packed_xy=(u32(y)<<16)|x;
    result.selected=true;
    result.tmp_after=index;
    result.point_index=u8(index);
    result.pixel_x=x;result.pixel_y=y;
    result.a0_x_16_16=u32(x)<<16;
    result.a1_packed_xy=packed_xy;
    result.a3_keepxy=packed_xy;
    reserved.points[reserved.count++]={u32(x)<<16,packed_xy}; // OEXIT stores A3 and advances KEEPXY.
    out=result;
    return {};
}
Status source_apply_ohulk_waveyx(u32 base_x,u32 base_a1,bool attract_mode,u32 wave_yx,SourceOhulkCoordinates& out){
    // OHULK adds the packed WAVEYX X half to A0 and its Y half to A1.
    // GET_VALID_XY also writes a packed A3 reservation, but that is not either
    // coordinate register (and using it here loses random-placement fractions).
    SourceOhulkCoordinates result{base_x,base_a1};
    if(!attract_mode){
        result.a0_x_16_16+=(wave_yx&0xffffu)<<16;
        result.a1_value+=wave_yx&0xffff0000u;
    }
    out=result;
    return {};
}
namespace {
bool within_distance(u32 a,u32 b,u32 distance){return a>=b?a-b<=distance:b-a<=distance;}
bool inside(const SourceRectangle& r,u32 x,u32 y){return x>=r.x_min&&x<=r.x_max&&y>=r.y_min&&y<=r.y_max;}
}
Status source_get_valid_xy_random(SourceRng& rng,SourceHCountRead read_hcount,void* context,
                                  const SourceSpawnBounds& bounds,u16 from_door,
                                  SourceSpawnReservations& reserved,SourceSpawnResult& result,
                                  unsigned attempt_limit,SourceSpawnRejections* rejections){
    if(!read_hcount)return Status::fail("GET_VALID_XY needs an explicit HCOUNT reader");
    if(bounds.x_min>=bounds.x_max)return Status::fail("GET_VALID_XY invalid X range");
    if(bounds.y_min>=bounds.y_max)return Status::fail("GET_VALID_XY invalid Y range");
    if(attempt_limit==0)return Status::fail("GET_VALID_XY native retry cap is zero");
    if(reserved.count==0||reserved.count>SourceSpawnReservations::capacity)
        return Status::fail("GET_VALID_XY RAMXY live count is invalid",reserved.count);
    if(reserved.count==SourceSpawnReservations::capacity)
        return Status::fail("GET_VALID_XY RAMXY has no room before its terminator",reserved.count);
    SourceRectangle carpet;
    auto carpet_status=source_red_carpet(from_door,carpet);
    if(!carpet_status)return carpet_status;
    for(unsigned attempt=1;attempt<=attempt_limit;++attempt){
        SourceSpawnPoint candidate;
        // Source order: the first RANGRAND is stored to YVAL, the second is A0/X.
        candidate.y=rng.range(bounds.y_min,bounds.y_max,read_hcount(context));
        candidate.x=rng.range(bounds.x_min,bounds.x_max,read_hcount(context));
        auto reject=[&]{if(rejections){if(rejections->x&&rejections->count<rejections->capacity)rejections->x[rejections->count]=candidate.x;++rejections->count;}};
        if(inside(carpet,candidate.x,candidate.y)){reject();continue;}
        bool overlaps=false;
        for(unsigned i=0;i<reserved.count;++i){
            const auto& old=reserved.points[i];
            if(within_distance(candidate.y,old.y,0x001c0000u)&&
               within_distance(candidate.x,old.x,0x001a0000u)){overlaps=true;break;}
        }
        if(overlaps){reject();continue;}
        // OEXIT stores A3=[integer Y:integer X]. TCK compares the full 16.16
        // candidate Y against that whole packed long, and X against X<<16.
        reserved.points[reserved.count++]={candidate.x&0xffff0000u,
                                           (candidate.y&0xffff0000u)|(candidate.x>>16)};
        result={candidate,attempt};
        return {};
    }
    return Status::fail("GET_VALID_XY random branch exhausted native retry cap",attempt_limit);
}
Status source_get_valid_xy(const SourceGetValidXYRequest& request,SourceRng& rng,
                           SourceHCountRead read_hcount,void* hcount_context,
                           SourceSpawnReservations& reserved,SourceGetValidXYResult& out){
    SourceCirclePlacement circle{};
    auto status=source_get_valid_xy_circle(
        {request.hulk_actor,request.attract_mode,request.wave,request.hulk_type,request.tmp},
        reserved,circle);
    if(!status)return status;

    SourceGetValidXYResult result{};
    result.tmp_after=request.tmp;
    if(circle.selected){
        // MOVX/SLL produce A0 as 16.16 X. The table .LONG remains packed Y:X
        // in A1; A3 separately holds the RAMXY reservation written by OEXIT.
        result.branch=SourceSpawnBranch::circle;
        result.coordinates={circle.a0_x_16_16,circle.a1_packed_xy};
        result.circle=circle;
        result.tmp_after=circle.tmp_after;
    }else{
        status=source_get_valid_xy_random(rng,read_hcount,hcount_context,
                                          request.bounds,request.from_door,
                                          reserved,result.random,
                                          request.attempt_limit,request.rejections);
        if(!status)return status;
        result.branch=SourceSpawnBranch::random;
        result.coordinates={result.random.point.x,result.random.point.y};
    }
    out=result;
    return {};
}
u16 player_direction(u8 bits,const SourcePosition& p,const PlayerBounds& b,bool intro){bits&=15;if(!bits)return 0;u16 d=0;bool skip_left=false,skip_up=false;if(bits&8){if(intro||signed32(p.x)<signed32(b.right))d=4;else skip_left=true;}if(!skip_left&&(bits&4)){if(intro||signed32(p.x)>signed32(b.left))d=u16(d+7);}if(bits&2){if(intro||signed32(p.y)<signed32(b.bottom))d=u16(d+2);else skip_up=true;}if(!skip_up&&(bits&1)){if(intro||signed32(p.y)>signed32(b.top))d=u16(d+1);}return d>9?9:d;}
Status player_velocity(u16 dir,u16& shoes,SourcePosition& l,SourcePosition& t){if(dir>9)return Status::fail("Unknown original player direction",dir);u32 cardinal=0x16000,diagonal=0xfa00;if(shoes){--shoes;cardinal=0x1f000;diagonal=0x18000;}u32 vx[10]={0,0,0,0,cardinal,diagonal,diagonal,0-cardinal,0-diagonal,0-diagonal};u32 vy[10]={0,0-cardinal,cardinal,0,0,0-diagonal,diagonal,0,0-diagonal,diagonal};l.vx=t.vx=vx[dir];l.vy=t.vy=vy[dir];return {};}
u16 firing_direction(u8 b){u16 d=0;if(b&1)d+=1;if(b&2)d+=2;if(b&4)d+=7;if(b&8)d+=4;return d>9?9:d;}
Status regular_bullet_setup(u16 d,const SourcePosition& torso,BulletSetup& out){if(d==0||d==3||d>9)return Status::fail("Direction has no regular-bullet image",d);constexpr u8 convert[9]={1,5,6,3,2,4,7,8,6};constexpr u32 dx[8]={0,0x72000,0x72000,0x72000,0,0u-0x72000,0u-0x72000,0u-0x72000};constexpr u32 dy[8]={0u-0x72000,0u-0x72000,0,0x72000,0x72000,0x72000,0,0u-0x72000};constexpr u32 ox[10]={0,0x60000,0x50000,0,0x100000,0xa0000,0xe0000,0xfffffffb,0x40000,0x60000};constexpr u32 oy[10]={0,0,0x120000,0,0x50000,0x40000,0xc0000,0x50000,0x30000,0xb0000};constexpr u16 flips[10]={0,0,0x20,0,0x10,0x10,0x30,0,0,0x20};const char* images[10]={nullptr,"BULLETUP","BULLETUP",nullptr,"BULLETSIDE","BULLET45","BULLET45","BULLETSIDE","BULLET45","BULLET45"};u8 compass=convert[d-1];BulletSetup b;b.position={torso.x+ox[d],torso.y+oy[d],dx[compass-1],dy[compass-1]};b.age=60;b.id=0x4201;b.flags=u16(2|0x400|flips[d]);b.compass_direction=compass;b.image=images[d];out=b;return {};}
Status hulk_standing_frame(u16 hulk_type,u16 direction,HulkStandingFrame& out){
    if(direction<1||direction>8)return Status::fail("HULKS GET_STAND DIR outside SPINFRMS",direction);
    static constexpr const char* regular[8]={"GB12W2A","GB10W3A","GB9W4A","GB8W3A","GB6W5A","GB8W3A","GB9W4A","GB10W3A"};
    static constexpr const char* type2[8]={"SNG12A","SNG10A","SNG9A","SNG8A","SNG6A","SNG8A","SNG9A","SNG10A"};
    static constexpr u16 flags[8]={2,0x12,0x12,0x12,2,2,2,2};
    const unsigned index=direction-1;
    out={hulk_type==2?type2[index]:regular[index],flags[index]};
    return {};
}
Status hulk_speeds(u16 level,HulkSpeeds& out){if(signed16(level)<0)return Status::fail("Negative HULKS DO_DIFF level would not terminate");u32 h=0x8000+u32(level)*0x2000,v=h-0x1000;out={h,v,(h/10)*8,(v/10)*8};return {};}
u16 turn_hulk(u16 current,u16 target){i32 diff=i32(target)-i32(current),absdiff=diff<0?-diff:diff;if(absdiff>=5)diff=-diff;if(diff<0)return current<=1?8:u16(current-1);return current==8?1:u16(current+1);}
u16 hulk_direction_to(i32 ox,i32 oy,i32 tx,i32 ty){i64 dx=i64(tx)-ox,dy=i64(ty)-oy;u64 ax=u64(dx<0?-dx:dx),ay=u64(dy<0?-dy:dy);u16 d=0;if(ax){u64 slope=(ay<<16)/ax;if(slope<=0x269fb){++d;if(slope<=0x69fb)++d;}}if(dx<0){if(dy<0)d=d?u16(8-d):0;else d=u16(d+4);}else if(dy>=0)d=u16(4-d);return d;}
Status hulk_velocity(u16 dir,const HulkSpeeds& s,SourcePosition& p){if(dir<1||dir>8)return Status::fail("Unknown HULKS INTO direction",dir);u32 x[8]={0,s.diagonal_x,s.horizontal,s.diagonal_x,0,0-s.diagonal_x,0-s.horizontal,0-s.diagonal_x};u32 y[8]={0-s.vertical,0-s.diagonal_y,0,s.diagonal_y,s.vertical,s.diagonal_y,0,0-s.diagonal_y};p.vx=x[dir-1];p.vy=y[dir-1];return {};}
void advance_position(SourcePosition& p){p.x+=p.vx;p.y+=p.vy;}
}

namespace stv {
u32 source_bcd_add(u32 score,u32 amount) {
    u32 carry=0;
    for(unsigned i=0;i<8;++i) {
        u32 digit=(amount&15)+(score&15)+carry;
        carry=digit>=10;
        if(carry)digit+=6;
        score=(score>>4)+(digit<<28);
        amount>>=4;
    }
    return score;
}
ScoreEffects source_score_add(SourceScore& score,u32 amount,u16 status,bool attract) {
    ScoreEffects effects;
    if(attract)return effects;
    score.packed_bcd=source_bcd_add(score.packed_bcd,amount);
    if(status==3)score.packed_bcd=source_bcd_add(score.packed_bcd,amount);
    if(score.packed_bcd>=score.next_extra) {
        score.next_extra=source_bcd_add(score.next_extra,0x1500000);
        effects.extra_life_requested=true; // exactly once per SCRADD2 call
    }
    return effects;
}
}
