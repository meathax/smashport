// enhanced_l8_16_9 data patch layer (PLAN D7, P9). Applied to the imported L8
// native data image when the enhanced profile is selected; imported program ROM
// remains immutable. Explicit compiled gameplay table roots select this image.
//
// The game's world is one map per circuit: GAMEMODS -> per-circuit module lists
// ([module ptr][X][Y], 0xFFFFFFFF-terminated) of shared room templates, rooms
// 472 px apart; the camera origin per wave comes from CRCTTAB, the movement walls
// from BNDTBL (offsets from the camera). To widen every arena by kWiden columns:
//   - each 391-px arena template gets a relocated block table: blocks right of the
//     centre features move right by kWiden and the plain floor strip there is tiled
//     into the gap (original blocks only, no new art);
//   - module X positions and camera X origins move by kWiden per room column;
//   - the camera is centred on the wider arena and the right wall moves out.
// Addresses are L8 (resolved from the translated code's source references).
#include "stv/gsp/scene.hpp"
#include "stv/gsp/ops.hpp"
#include "stv/gsp/det_math.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <map>
#include <set>
#include <vector>

namespace stv::gsp {

namespace {
#include "enhanced_stage_balance.inc"

constexpr u32 kGameMods = 0xFFE72650u;   // GAMEMODS: .long CIRCUIT1,CIRCUIT2,CIRCUIT3
constexpr u32 kCrctTab = 0xFFE8C7F0u;    // CRCTTAB: .long CRCT1YX,CRCT2YX,CRCT3YX
constexpr u32 kBndTbl = 0xFFE8B8D0u;     // BNDTBL: .long NORMBND,POW2BND,POW1BND
constexpr u32 kInitialFormations = 0xFFE417C0u; // HULKS CTBL1: eleven typed data pointers
constexpr u32 kFreeBase = 0xFF801000u;   // unused maindata space for relocated tables
constexpr int kPitch = 472;              // room pitch in world pixels
constexpr int kArenaW = 391;             // arena template width (390..392 in practice)
constexpr int kRackupW = 393;            // bonus rack-up screen template width

struct Rom {
    std::vector<u16>& data;
    u16& w(u32 a) { return data[(a - 0xFF800000u) >> 4]; }
    u32 l(u32 a) { return u32(w(a)) | (u32(w(a + 16)) << 16); }
    void sl(u32 a, u32 v) { w(a) = u16(v); w(a + 16) = u16(v >> 16); }
};

int column(int x, int x0) { return int(std::lround(double(x - x0) / kPitch)); }

// Geometric remap of a screen-relative X (offset from WORLDTL; the reference arena
// spans 5..395): points follow the floor they lie on. Template x < 149 stays with
// the left wall, 149..241 moves with the centre (+widen/2), 242.. moves with the
// right wall (+widen); everything also moves by the camera centring shift.
int arena_x(int x, int widen) {
    const int m = x - 5, shift = (410 + widen - (kArenaW + widen)) / 2 - 5;
    return x + (m < 149 ? 0 : m < 242 ? widen / 2 : widen) + shift;
}

// Screen-relative X tables consumed by gameplay code (source table, L8 address from
// the consuming MOVI). Each is checked against its reference values first; a
// mismatch is a named fault and the table is left alone.
enum class Field { FixHi, XyLo, Word };   // 16.16 X in the high word / [Y,X] X low word / .WORD X
struct XTable {
    const char* src;
    u32 addr;
    Field f;
    std::vector<int> ref;
};
const std::vector<XTable>& x_tables() {
    static const std::vector<XTable> t = {
        {"CHASER.ASM:58 SX", 0xFFEC0710u, Field::FixHi, STV_ROM_X_FFEC0710},
        {"FATMAN.ASM:869 SX", 0xFFEEEA10u, Field::FixHi, STV_ROM_X_FFEEEA10},
        {"GAL.ASM:813 SX", 0xFFF364A0u, Field::FixHi, STV_ROM_X_FFF364A0},
        {"LZRORB.ASM:478 SX", 0xFFF171A0u, Field::FixHi, STV_ROM_X_FFF171A0},
        {"ROBOTS.ASM:389 GXL", 0xFFF2CE10u, Field::FixHi, STV_ROM_X_FFF2CE10},
        {"ROBOTS.ASM:390 GXR", 0xFFF2CE90u, Field::FixHi, STV_ROM_X_FFF2CE90},
        {"ROBOTS.ASM:1057 SX", 0xFFF313E0u, Field::FixHi, STV_ROM_X_FFF313E0},
        {"HULKS.ASM:1104 SX", 0xFFE40AB0u, Field::FixHi, STV_ROM_X_FFE40AB0},
        // GHULK entry FFE3BEB0 is selected by the accepted CREATE at FFE3B520.
        // Its source-matched prefix consumes four signed 16.16 values per door.
        {"HULKS.ASM:228 GXL (GHULK)", 0xFFE3BBD0u, Field::FixHi, STV_ROM_X_FFE3BBD0},
        {"HULKS.ASM:229 GXR (GHULK)", 0xFFE3BC50u, Field::FixHi, STV_ROM_X_FFE3BC50},
        {"SNAKE.ASM:263 GXL", 0xFFF46350u, Field::FixHi, STV_ROM_X_FFF46350},
        {"SNAKE.ASM:264 GXR", 0xFFF463B0u, Field::FixHi, STV_ROM_X_FFF463B0},
        {"TANK.ASM:651 SX", 0xFFEE02C0u, Field::FixHi, STV_ROM_X_FFEE02C0},
        {"FATMAN.ASM:604 CRNRYX", 0xFFEEC900u, Field::XyLo, STV_ROM_X_FFEEC900},
        {"TANK.ASM:778 UPLFT", 0xFFEE0D00u, Field::XyLo, STV_ROM_X_FFEE0D00},
        {"TANK.ASM:779 DNRGT", 0xFFEE0D80u, Field::XyLo, STV_ROM_X_FFEE0D80},
        {"ICONS.ASM:288 PX", 0xFFED7320u, Field::Word, STV_ROM_X_FFED7320},
        {"ICONS.ASM:289 PX2", 0xFFED7360u, Field::Word, STV_ROM_X_FFED7360},
        {"ENDWAVE.ASM:869 EXX (EXIT sign per door)", 0xFFEC8930u, Field::FixHi, STV_ROM_X_FFEC8930},
        // RED_CARPETX packs two X bounds (right low/left high), not [Y,X].
        // FROMDR indexes five longs; its separate RED_CARPETY table is unchanged.
        // Patch each 16-bit word after verifying the complete retail table.
        {"HULKS.ASM:3156 RED_CARPETX", 0xFFE4CFC0u, Field::Word, STV_ROM_X_FFE4CFC0},
    };
    return t;
}
constexpr u32 kCircTbl = 0xFFEC30B0u;    // ENDWAVE CIRCTBL: per-circuit exit-arrow X/Y lists
constexpr u32 kCirc2Tbl = 0xFFECCE80u;   // ENDWAVE CIRC2TBL: per-wave exit ("magic") boxes
constexpr int kCircuitWaves[3] = {11, 22, 22};   // records per table (circuit 1 wave 11 exits to the bonus; circuits 2 and 3 share 22)
constexpr u32 kMsgX = 0xFFE2CCA0u;       // SCORE MSGX: P1 coin/start message X (P2 adds 0EBH)
}  // namespace

namespace {
int firing_slot(u32 process) {
    constexpr u32 pool=0x01086790, stride=0x5a0;
    const u32 offset=process-pool;
    return offset < 240*stride && offset%stride==0 ? int(offset/stride) : -1;
}

struct Bearing {
    double x,y,cos_delta,sin_delta;
};

int aim_sector(u16 shot) {
    constexpr int sector_by_bits[]={0,0,4,0,6,7,5,0,2,1,3,0,0,0,0,0};
    return sector_by_bits[(shot>>4)&15];
}

constexpr double eighth=3.14159265358979323846/4;
Bearing bearing(u16 shot) {
    const int sector=aim_sector(shot);
    const double angle=(sector+(int(shot>>10)-32)/62.0)*eighth;
    const double delta=angle-((shot&15)-1)*eighth;
    return {det::sin(angle),-det::cos(angle),det::cos(delta),det::sin(delta)};
}
}

namespace {
// Virtual DMA pages are reserved for angled projectiles.
constexpr int tile=48, fine_steps=63, base_poses=5*8*fine_steps;
struct Point { int x,y; };

Point sprite_pivot(Machine& m,u32 image,int stride,int height,u16 flags) {
    const u32 offset=rlong(m,image+0x20);
    int x=s16(offset),y=s16(offset>>16);
    if (s32(offset)>=0x02000000) x=y=0; // retail GANIOF fallback
    if (flags&0x10) x=stride-1-x;
    if (flags&0x20) y=height-1-y;
    return {x,y+38}; // waist relative to the shared retail animation origin
}

Point object_pivot(Machine& m,u32 obj) {
    return sprite_pivot(m,rlong(m,obj+0x130),m.rw(obj+0xf0),m.rw(obj+0x100),m.rw(obj+0xb0));
}

}

void Machine::boost_enemy_motion() {
    if(!enhanced_scene(*this) || game_scene_state(*this)!=0x10)return;
    const u32 node=c.a[0],offset=node-object_pool_base;
    if(offset>=object_slots*object_stride || offset%object_stride)return;
    const u16 oid=rw(node+0x150);
    const unsigned type=(oid>>8)&31;
    const bool neutral=oid==0x0800 || oid==0x0b00 || oid==0x0e00;
    if((oid&0xe000)!=0x8000 && !neutral)return;
    // Scale the final motion once. Formation offsets, difficulty rolls and
    // raw acceleration remain distinct, and velocity RAM never compounds.
    switch(type){
    case 3: case 4: case 6: case 8: case 9: case 11: case 14: case 16: case 18:
        c.a[12]=u32(s32(s64(s32(c.a[12]))*9/8));
        c.a[11]=u32(s32(s64(s32(c.a[11]))*9/8));
        break;
    case 17:{
        const u32 proc=rlong(*this,node+0x160);
        const u32 parent=firing_slot(proc)>=0?rlong(*this,proc+0x100):0;
        const bool mutoid=firing_slot(parent)>=0 && rw(parent+0x20)==214;
        c.a[12]=u32(s32(s64(s32(c.a[12]))*(mutoid?5:9)/(mutoid?4:8)));
        c.a[11]=u32(s32(s64(s32(c.a[11]))*(mutoid?5:9)/(mutoid?4:8)));
        break;
    }
    case 24:{
        // All moving Mutoid body components share the same motion multiplier.
        // Its separately spawned helper uses the ordinary enemy rate.
        const bool helper=(oid&255)==8;
        c.a[12]=u32(s32(s64(s32(c.a[12]))*(helper?9:5)/(helper?8:4)));
        c.a[11]=u32(s32(s64(s32(c.a[11]))*(helper?9:5)/(helper?8:4)));
        break;
    }
    default:break; // boss components have their own acceleration policy
    }
}

void Machine::mutoid_wall_action() {
    // GET_ST has chosen the next action in A0 and A8 is the tank half whose Y
    // the retail MDN/MUP handlers test against WORLDTLY+[40,170]. Retail picks
    // down/up/lunge without looking at that wall, so the tank can be sent into
    // it for a frame or two (or a whole timer when a jerk clears its velocity).
    // Turn such a choice around while it is still inside the legal interval.
    if(!enhanced_scene(*this) || game_scene_state(*this)!=0x10)return;
    const u32 tank=c.a[8];
    const u32 offset=tank-object_pool_base;
    if(offset>=object_slots*object_stride || offset%object_stride)return;
    const u16 oid=rw(tank+0x150);
    if(oid!=0x9801 && oid!=0x9802)return;
    const s64 y=s64(s32(rlong(*this,tank+0x80)))-s64(s32(rlong(*this,0x01000260u)));
    constexpr s64 top=40ll<<16, bottom=170ll<<16, margin=8ll<<16;
    switch(c.a[0]){
    case 1: case 13: if(y>=bottom-margin)c.a[0]=2;break;   // down / lunge down
    case 2: case 12: if(y<=top+margin)c.a[0]=1;break;      // up / lunge up
    default:break;
    }
}

void Machine::analog_walk_facing() {
    const unsigned direction=c.a[3];
    if((!move_inputs[0] && !move_inputs[1]) || game_scene_state(*this)!=0x10 ||
       (direction!=5 && direction!=6 && direction!=8 && direction!=9))return;
    const u32 proc=c.a[7];
    if(firing_slot(proc)<0)return;
    const unsigned n=rb(*this,proc+0x1d8)-1u;
    if(n>=2 || !move_inputs[n] || rlong(*this,0x010de2b0+n*32)!=proc)return;
    const int dx=static_cast<std::int8_t>(move_inputs[n]&255);
    const int dy=static_cast<std::int8_t>(move_inputs[n]>>8);
    if(std::abs(dx)*1000 < std::abs(dy)*414)c.a[3]=dy<0 ? 1 : 2;
    if(std::abs(dy)*1000 < std::abs(dx)*414)c.a[3]=dx<0 ? 7 : 4;
}

void Machine::analog_velocity() {
    if ((!move_inputs[0] && !move_inputs[1]) || game_scene_state(*this)!=0x10)return;
    const u32 proc=rlong(*this,c.a[13]+0x160); // legs' TSO_PRC
    if(firing_slot(proc)<0)return;
    const unsigned n=rb(*this,proc+0x1d8)-1u;
    if(n>=2 || !move_inputs[n] || rlong(*this,0x010de2b0+n*32)!=proc)return;
    const u32 ytable=c.a[2]-c.a[6]; // selected by retail shoe timer
    const int dx=static_cast<std::int8_t>(move_inputs[n]&255);
    const int dy=static_cast<std::int8_t>(move_inputs[n]>>8);
    const auto component=[&](int axis,u32 speed,u32 pos,u32 low,u32 high) {
        const s32 coord=s32(rlong(*this,c.a[8]+pos));
        const s32 delta=s32(s64(axis)*speed/127);
        if(delta>0)return std::clamp<s64>(s64(s32(rlong(*this,proc+high)))-coord,0,delta);
        return std::clamp<s64>(s64(s32(rlong(*this,proc+low)))-coord,delta,0);
    };
    c.a[0]=u32(component(dx,rlong(*this,ytable-0x140+4*32),0x60,0x280,0x2a0));
    c.a[1]=u32(component(dy,rlong(*this,ytable+2*32),0x80,0x240,0x260));
}

void Machine::queue_aimed_sprite() {
    const u32 obj=c.a[0];
    if(obj<0x010003a0 || obj>=0x010003a0+420*0x1f0 || (obj-0x010003a0)%0x1f0)return;
    const unsigned oid=rw(obj+0x150);
    if(oid==0x4201 || oid==0x4205) {
        const int slot=firing_slot(rlong(*this,obj+0x160));
        if(slot<0 || !(shot_aim[slot]>>10))return;
        const auto vx=s32(rlong(*this,obj+0x20)), vy=s32(rlong(*this,obj+0x40));
        if((vx==0 && vy==0) || (c.a[12]&15)!=2)return;
        const auto pivot=object_pivot(*this,obj);
        constexpr int directions=8*62, size=24;
        const int step=(int(std::lround(det::atan2(double(vy),double(vx))*62/eighth))+directions)%directions;
        const int key=(oid==0x4205 ? directions : 0)+step;
        const int x=s16(c.a[10])+pivot.x-size/2, y=s16(c.a[10]>>16)+pivot.y-38-size/2;
        c.a[10]=u16(x)|(u32(u16(y))<<16);
        c.a[9]=size|(size<<16);
        c.a[11]=0x02000000u+8*(aim_graphics_base+base_poses*tile*tile+key*size*size);
        c.a[12]&=0xffcf;
        return;
    }
}

void Machine::draw_aimed_sprite(u32 offset,int width,int height,int x,int y,int stride,u16 palette_bank) {
    u32 relative=offset-aim_graphics_base;
    constexpr int size=24, directions=8*62;
    constexpr u32 first_bullet=base_poses*tile*tile, bullet_bytes=2*directions*size*size;
    if(relative<first_bullet || relative>=first_bullet+bullet_bytes)return;
    relative-=first_bullet;
    const unsigned key=relative/(size*size);
    const u32 img=key<directions ? 0xfffbe520 : 0xfffbe6a0;
    const double angle=(int(key%directions)-directions/2)*eighth/62;
    const auto anchor=rlong(*this,img+32);
    const auto pixels=(rlong(*this,img+64)-0x02000000u)>>3;
    const int artWidth=rw(img), artHeight=rw(img+16), artStride=(artWidth+3)&~3;
    const Point pivot{s16(anchor),s16(anchor>>16)};
    if(stride!=size)return;
    const int start=relative%(size*size);
    const auto sample=[&](int x,int y)->u8 {
        if(x<0 || x>=artWidth || y<0 || y>=artHeight)return 0;
        const auto index=std::size_t(pixels)+y*artStride+x;
        return index<gfx.size() ? gfx[index] : 0;
    };
    for(int row=0;row<height;++row)for(int col=0;col<width;++col) {
        const int dx=start%size+col-size/2, dy=start/size+row-size/2;
        const u8 pen=sample(pivot.x+int(std::lround(dx*det::cos(angle)+dy*det::sin(angle))),
                            pivot.y+int(std::lround(dy*det::cos(angle)-dx*det::sin(angle))));
        if(pen){
            const auto cell=std::size_t(y+row)*vram_w+x+col;
            vram[cell]=palette_bank|pen;
            if(!sprite_coverage.empty())sprite_coverage[cell]=1;
        }
    }
}

void Machine::latch_shot_aim() {
    const int slot=firing_slot(c.a[0]);
    if (slot<0) return;
    const unsigned owner=c.a[11]&255;
    const u16 input=owner>=1 && owner<=2 ? aim_inputs[owner-1] : 0;
    // Helpers keep their independent emitter. The low compass nibble is filled at wake.
    const u32 wake=c.a[7];
    const bool helper=wake==0xffe370c0 || wake==0xffe36cd0 || wake==0xffe362e0 || wake==0xffe34610;
    shot_aim[slot]=game_scene_state(*this)==0x10 && (input>>10) ? u16(input | (helper ? 0x200 : 0)) : 0;
}

void Machine::bind_shot_compass() {
    const int slot=firing_slot(c.a[13]);
    if (slot>=0 && shot_aim[slot]) {
        const unsigned compass=c.a[0];
        shot_aim[slot]=compass>=1 && compass<=8 ? u16((shot_aim[slot]&0xfff0)|compass) : 0;
    }
}

void Machine::aim_launch(bool direct) {
    const int slot=firing_slot(c.a[13]);
    if (slot<0 || !shot_aim[slot]) return;
    const auto b=bearing(shot_aim[slot]);
    const double vx=s32(c.a[6]), vy=s32(c.a[7]);
    if (direct) {
        // Use the cardinal shot speed at every angle.
        const double speed=c.a[5]==0x4205 ? 0x40000 : 0x72000;
        c.a[6]=u32(std::lround(speed*b.x));
        c.a[7]=u32(std::lround(speed*b.y));
    } else {
        // Rotate the actual weapon vector, including spray or random pod spread.
        c.a[6]=u32(std::lround(vx*b.cos_delta-vy*b.sin_delta));
        c.a[7]=u32(std::lround(vx*b.sin_delta+vy*b.cos_delta));
    }
}

void Machine::aim_grenade_step(bool vertical) {
    const int slot=firing_slot(c.a[13]);
    if (slot<0 || !shot_aim[slot]) return;
    // A10 has just advanced past the current shadow byte. Its X/Y pair
    // describes ground travel; the grenade's difference includes toss height.
    const u32 pair=c.a[10]-(vertical ? 16 : 8);
    const double x=s32(sext(rb(*this,pair),8)), y=s32(sext(rb(*this,pair+8),8));
    const auto b=bearing(shot_aim[slot]);
    const auto rotated=std::lround(vertical ? x*b.sin_delta+y*b.cos_delta : x*b.cos_delta-y*b.sin_delta);
    c.a[0]+=u32(rotated)-c.a[2];
    c.a[2]=u32(rotated);
}

bool apply_enhanced(Machine& m, int widen) {
    if (!m.enhanced_data.empty()) {
        m.fault("native enhancement data already prepared", ENHANCED_DATA_BASE);
        m.stop = true;
        return false;
    }
    if (m.prog.size() != 0x100000 / 2 || m.gfx.size() != 0x800000 || widen <= 0 || widen > 512) {
        m.fault("invalid native enhancement data configuration", ENHANCED_DATA_BASE);
        m.stop = true;
        return false;
    }
    // Build transactionally: rejected source tables cannot publish a partial
    // enhancement, and neither success nor failure modifies imported ROM bytes.
    std::vector<u16> working = m.prog;
    std::vector<u8> working_gfx = m.gfx;
    Rom r{working};
    u32 free = kFreeBase;
    std::map<u32, u32> relocated;   // template module -> new module record

    auto relocate_arena = [&](u32 mod) -> u32 {
        if (auto it = relocated.find(mod); it != relocated.end()) return it->second;
        const int xs = r.w(mod), ys = r.w(mod + 16), n = r.w(mod + 32);
        if (xs < kArenaW - 1 || xs > kArenaW + 1) {
            relocated[mod] = mod;
            return mod;
        }
        const u32 blk = r.l(mod + 48), hdrs = r.l(mod + 80);
        constexpr u32 studio = 0xFFF6C840u;
        constexpr u32 metal = 0xFFF5C7E0u;
        const bool known_headers = hdrs == metal || hdrs == 0xFFF62AB0u ||
            hdrs == 0xFFF70370u || hdrs == 0xFFF82400u ||
            hdrs == 0xFFF8C050u || hdrs == 0xFFF94DC0u;
        if (!known_headers || n <= 0 || n > 256 || widen <= 0 || widen > 256) {
            m.fault("enhanced background is not a bound L8 template", mod);
            m.stop = true;
            return 0;
        }
        const int wl = widen / 2, wr = widen - wl;
        struct B {
            u16 f, x, y, h;              // original map flags, position and header index
            int w, ht;
            u32 image;
        };
        std::vector<B> original, out;
        std::vector<std::array<int,4>> upper_floor_gaps; // x,y,width,height
        original.reserve(std::size_t(n));
        for (int k = 0; k < n; ++k) {
            const u32 a = blk + u32(k) * 64;
            const u16 h = r.w(a + 48);
            const u32 ha = hdrs + u32(h & 0x0fff) * 32;
            B b{r.w(a), r.w(a + 16), r.w(a + 32), h,
                int(r.w(ha)), int(r.w(ha + 16)), r.l(ha + 32)};
            const std::size_t stride = std::size_t((b.w + 3) & ~3);
            if (b.w <= 0 || b.w > 250 || b.ht <= 0 || b.ht > 512 ||
                b.image < 0x02000000u || (b.image & 7u) ||
                std::size_t((b.image - 0x02000000u) >> 3) + stride * std::size_t(b.ht) > 0x800000u) {
                m.fault("enhanced background image is not a bound L8 block", ha);
                m.stop = true;
                return 0;
            }
            original.push_back(b);
        }
        // The original stage/arena headers include complete TV, podium and HUD
        // assemblies. They are never filler tiles. Keep each unique block once;
        // grow only the certified wall/floor/trim runs below. All pixels in an
        // extension come from that original run, with the palette unchanged.
        auto pixel = [&](const B& b, int x, int y) -> u8 {
            if (b.f & 0x10) x = b.w - 1 - x;
            const std::size_t off = (b.image - 0x02000000u) >> 3;
            return working_gfx[off + std::size_t(y) * std::size_t((b.w + 3) & ~3) + std::size_t(x)];
        };
        auto materialize = [&](B b, int width, int height, const auto& source_pixel) {
            // BAKGND's visibility search assumes WIDEST_BLOCK=250. Split long
            // trim/floor runs into at most 240-pixel pieces, retaining priority.
            for (int left = 0; left < width; left += 240) {
                const int bw = std::min(240, width - left);
                const int stride = (bw + 3) & ~3;
                const std::size_t off = working_gfx.size();
                working_gfx.resize(off + std::size_t(stride) * std::size_t(height), 0);
                for (int y = 0; y < height; ++y)
                    for (int x = 0; x < bw; ++x)
                        working_gfx[off + std::size_t(y) * std::size_t(stride) + std::size_t(x)] =
                            source_pixel(left + x, y);
                B piece = b;
                piece.f = u16(piece.f & ~0x10u); // horizontal flip is baked into the copied rows
                piece.x = u16(int(b.x) + left);
                piece.w = bw;
                piece.ht = height;
                piece.image = 0x02000000u + u32(off * 8);
                out.push_back(piece);
            }
        };
        auto extend = [&](const B& b, int extra, int insert, int from, int length, int x) {
            B dest = b;
            dest.x = u16(x);
            materialize(dest, b.w + extra, b.ht, [&](int dx, int y) {
                const int sx = dx < insert ? dx : dx < insert + extra ?
                    from + (dx - insert) % length : dx - extra;
                return pixel(b, sx, y);
            });
        };
        auto move = [&](B b, int dx) {
            b.x = u16(int(b.x) + dx);
            out.push_back(b);
        };
        for (const B& b : original) {
            const int x = b.x, y = b.y, bw = b.w, bh = b.ht;
            if (mod == studio) {
                // One rigid centre set keeps the podiums, cameras, door and
                // scripted walking lane in the same relative coordinate space.
                if (b.h == 0x000e) {
                    const bool left = x < 196;
                    extend(b, left ? wl : wr, left ? 20 : bw - 20,
                           left ? 19 : bw - 20, 1, left ? x : x + wl);
                } else if (b.h == 0x0006) {
                    // The spectator banks are already mirrored repeating art.
                    // Overlap complete banks so transparent silhouettes join;
                    // clipping a bank at a column creates a visible vertical tear.
                    move(b, wl);
                    move(b, x < 100 ? 0 : widen);
                } else if (b.h == 0x0000 || b.h == 0x0002 || b.h == 0x000a ||
                           b.h == 0x000c || b.h == 0x0010 || b.h == 0x0012) {
                    move(b, x < 196 ? 0 : widen); // exterior walls and carpet end caps
                } else {
                    move(b, wl);
                }
                continue;
            }
            int shift = x < 132 ? 0 : x < 242 ? wl : widen;
            if (bw == 112 && bh == 33 && (y == 45 || y == 46))
                shift = x < 196 ? 0 : widen; // complete score bezels
            if (bw >= 150 && bh <= 17 && y >= 238) {
                const bool left = x < 100;
                extend(b, left ? wl : wr, left ? 120 : bw - 120,
                       left ? 88 : bw - 120, 32, left ? x : x + wl);
            } else if ((hdrs == 0xfff82400u || hdrs == 0xfff8c050u) &&
                       y == 0 && bw == 40 && bh == 46 && (x == 52 || x == 299)) {
                // Jungle/temple side walls: repeat the complete masonry/foliage
                // texture, not a four-column slice through a carved face.
                const bool left = x == 52;
                extend(b, left ? wl : wr, left ? 36 : 4, 4, 32, left ? x : x + wl);
            } else if ((hdrs == 0xfff82400u || hdrs == 0xfff8c050u) &&
                       y == 0 && (bw == 68 || bw == 72) && bh >= 46 && bh <= 54 &&
                       ((x >= 89 && x <= 90) || (x >= 230 && x <= 235))) {
                move(b, wl); // complete carved faces/plaques stay with the centre doorway
            } else if ((hdrs == 0xfff82400u || hdrs == 0xfff8c050u) &&
                       y == 0 && bw == 104 && bh == 49 && (x == 52 || x == 235)) {
                // Pillared temple variant contains its plain masonry and unique
                // pillar in one header. Insert only in its outer masonry run.
                const bool left = x == 52;
                extend(b, left ? wl : wr, left ? 32 : 72, left ? 0 : 72, 32,
                       left ? x : x + wl);
            } else if (hdrs == 0xfff70370u && bw == 96 && bh == 34 &&
                       (b.h == 0x00bcu || b.h == 0x00beu)) {
                // Cyber arena bridges retain the middle medallion and their end
                // caps. Extend only the repeating grate/flat deck between them.
                const bool left = x < 196, flip = (b.f & 0x10u) != 0;
                const bool grate = b.h == 0x00beu;
                const int cut = flip ? 32 : 64;
                const int from = grate ? (flip ? 16 : 64) : (flip ? 64 : 8);
                extend(b, left ? wl : wr, cut, from, grate ? 16 : 24,
                       left ? x : x + wl);
            } else if (y == 0 && bh >= 46 && bh <= 54 && x >= 50 && x < 154 && x + bw >= 154) {
                extend(b, wl, bw - 4, bw - 8, 4, x); // wall between left HUD and centre gate
            } else if (y == 0 && bh >= 46 && bh <= 54 && x >= 225 && x <= 240) {
                extend(b, wr, 4, 4, 4, x + wl); // corresponding right wall
            } else if (hdrs == metal && bw == 108 && bh == 17) {
                const bool left = x < 196;
                extend(b, left ? wl : wr, left ? 64 : 44, left ? 56 : 44, 8, left ? x : x + wl);
            } else if (hdrs != metal && bw == 64 && bh == 37 && (y == 41 || y == 42)) {
                // These headers include the central door's steps and their
                // shadow, not a repeatable horizontal floor strip. Preserve
                // them whole and fill only the newly exposed side gap using
                // the room's actual floor tile below the original wall art.
                const bool left = x < 180;
                move(b, wl);
                upper_floor_gaps.push_back({left ? x : x+wl+bw, y, left ? wl : wr, bh});
            } else if (hdrs != metal && bw == 100 && bh == 34 && x < 100) {
                extend(b, wl, 96, 64, 32, x);
            } else if (hdrs != metal && bw == 100 && bh == 34 && x >= 235 && x < 250) {
                extend(b, wr, 4, 4, 32, x + wl);
            } else if (hdrs != metal && bw == 48 && bh == 34 && (y == 78 || y == 79)) {
                if (x < 100) extend(b, wl, 44, 12, 32, x);
                else if (x >= 260 && x < 275) extend(b, wr, 4, 4, 32, x + wl);
                else move(b, wl);
            } else {
                move(b, shift);
            }
        }
        if (mod == studio) {
            auto tile = [&](std::size_t index, int x, int y, int width) {
                const B& src = original[index];
                for (int off = 0; off < width; off += src.w) {
                    B dest = src;
                    dest.x = u16(x + off);
                    dest.y = u16(y);
                    const int bw = std::min(src.w, width - off);
                    materialize(dest, bw, src.ht, [&](int xx, int yy) { return pixel(src, xx, yy); });
                }
            };
            // Bound by TVSETBMOD: plain ceiling trim, carpet run, then floor.
            // No TV/column/podium/camera header is eligible for this filling.
            if (original[10].h != 0x0008 || original[20].h != 0x0016 || original[8].h != 0x0004) {
                m.fault("enhanced studio strip identity mismatch", mod);
                m.stop = true;
                return 0;
            }
            tile(10, 2, 0, wl);       tile(10, 391 + wl, 0, wr);
            tile(20, 54, 116, wl);    tile(20, 338 + wl, 116, wr);
            tile(8, 1, 173, wl);      tile(8, 392 + wl, 173, wr);
        } else if (hdrs != metal) {
            // The bottom trim can reuse the floor image with another palette
            // (FFF81EF0 has grey trim and blue interior). Bind an interior row,
            // not whichever block happens to be first in the X-sorted table.
            auto it = original.end();
            for (auto candidate=original.begin(); candidate!=original.end(); ++candidate)
                if (candidate->w==100 && candidate->ht==34 && !(candidate->f&0x30u) &&
                    (it==original.end() || std::abs(int(candidate->y)+17-ys/2)<
                                           std::abs(int(it->y)+17-ys/2)))
                    it=candidate;
            if (it == original.end()) {
                m.fault("enhanced arena floor identity mismatch", mod);
                m.stop = true;
                return 0;
            }
            const B src = *it;
            auto modulo = [](int v, int divisor) { const int x = v % divisor; return x < 0 ? x + divisor : x; };
            auto tile_floor = [&](int x, int y, int width, int height) {
                B floor = src;
                floor.x = u16(x);
                floor.y = u16(y);
                floor.f = u16((src.f & 0x00cfu) | (63u << 8)); // below original floor/wall/props
                materialize(floor, width, height, [&](int dx, int dy) {
                    return pixel(src, modulo(dx+x-int(src.x),96), modulo(dy+y-int(src.y),34));
                });
            };
            tile_floor(24,79,xs+widen-48,ys-79);
            for (const auto& gap : upper_floor_gaps)
                tile_floor(gap[0],gap[1],gap[2],gap[3]);
        }
        std::stable_sort(out.begin(), out.end(), [](const B& p, const B& q) { return p.x < q.x; });
        if (out.size() >= 2048 || free + u32(out.size()) * 128 + 160 >= 0xFFE00000u ||
            working_gfx.size() > 0x1000000u) {
            m.fault("enhanced background storage limit", mod);
            m.stop = true;
            return 0;
        }
        const u32 nh = free;
        free += u32(out.size()) * 64;
        const u32 nb = free;
        free += u32(out.size()) * 64;
        for (std::size_t i = 0; i < out.size(); ++i) {
            const B& b = out[i];
            const u32 ha = nh + u32(i) * 64, ba = nb + u32(i) * 64;
            r.w(ha) = u16(b.w); r.w(ha + 16) = u16(b.ht); r.sl(ha + 32, b.image);
            r.w(ba) = b.f; r.w(ba + 16) = b.x; r.w(ba + 32) = b.y;
            r.w(ba + 48) = u16((b.h & 0xf000u) | u16(i * 2));
        }
        const u32 nm = free;
        r.w(nm) = u16(xs + widen); r.w(nm + 16) = u16(ys); r.w(nm + 32) = u16(out.size());
        r.sl(nm + 48, nb); r.sl(nm + 80, nh); r.sl(nm + 112, r.l(mod + 112));
        free += 160;
        relocated[mod] = nm;
        return nm;
    };

    // module lists: templates relocated, X spaced by the widened pitch
    std::vector<std::vector<std::pair<int, int>>> rooms(3);   // reference room positions per circuit
    for (int c = 0; c < 3; ++c) {
        u32 e = r.l(kGameMods + u32(c) * 32);
        for (int k = 0; k < 200 && r.l(e) != 0xFFFFFFFFu; ++k, e += 64) {
            const u32 mod = r.l(e);
            const u16 raw = r.w(e + 32);
            const int mw = r.w(mod), mh = r.w(mod + 16);
            if (raw != 0x8000 && raw != 0x8001) {
                const int x = int(int16_t(raw));
                if (mw >= kArenaW - 1 && mw <= kArenaW + 1) rooms[std::size_t(c)].push_back({x, int(int16_t(r.w(e + 48)))});
                // pieces kept at 4:3 width sit centred in their widened column: vertical
                // passages (400 wide, between rows of rooms, joining the top/bottom doors)
                // and the 393x259 bonus rack-up screens below the boss rooms
                const bool centred = (mw >= 395 && mw <= 405 && mh < 150) || mw == kRackupW;
                r.w(e + 32) = u16(x + widen * column(x, 477) + (centred ? widen / 2 : 0));
            }
            const u32 relocated_mod = relocate_arena(mod);
            if (!relocated_mod) return false;
            r.sl(e, relocated_mod);
        }
    }
    // camera origins per wave ([Y,X] longs): same spacing, centred on the wider arena.
    // Tables hold 12/20/24 entries; 0 marks an unused wave. An entry with no room there
    // is the bonus screen's text base (a column to the right of the boss room, whose
    // rack-up screen is centred under the boss room): it follows the boss room column.
    constexpr int kCrctEntries[3] = {12, 20, 24};
    const int shift = 5 - (410 + widen - (kArenaW + widen)) / 2;
    for (int c = 0; c < 3; ++c) {
        const u32 t = r.l(kCrctTab + u32(c) * 32);
        for (int k = 1; k < kCrctEntries[c]; ++k) {
            const u32 a = t + u32(k) * 32;
            const u32 v = r.l(a);
            const int x = int(v & 0xffff), y = int(v >> 16);
            if (v == 0) continue;
            bool room = false;
            for (const auto& [rx, ry] : rooms[std::size_t(c)]) room = room || (std::abs(rx - 5 - x) <= 8 && std::abs(ry - y) <= 8);
            // reference: camera = module X - 5 (arena at screen 5..395 of 410);
            // enhanced: arena centred in the 410+widen view
            const int nx = room ? x + widen * column(x, kPitch) + shift : x + widen * column(x - kPitch, kPitch) + widen / 2;
            r.sl(a, (v & 0xffff0000u) | u32(u16(nx)));
        }
    }
    // STOPME tests a packed [left,right) interval below the pit lip.
    // The left pit stays on the left wall and the right pit moves with the
    // right wall, including in the two-pit room with its bottom exit.
    constexpr u32 pit_limits = 0xffe7c550u;
    constexpr u32 retail_limits[] = STV_ROM_retail_limits;
    for (unsigned i = 0; i < 3; ++i) {
        const u32 address = pit_limits + i * 32;
        const u32 bounds = r.l(address);
        if (bounds != retail_limits[i]) {
            m.fault("pit limits differ from L8 reference", address);
            m.stop = true;
            return false;
        }
        const int left = int(bounds >> 16) - shift;
        const int right = int(bounds & 0xffffu) + widen - shift;
        r.sl(address, (u32(u16(left)) << 16) | u16(right));
    }

    // movement walls (offsets from the camera, value in the high word): the camera
    // moved left by the centring shift, and the right wall moves out by widen as well
    for (int k = 0; k < 3; ++k) {
        const u32 t = r.l(kBndTbl + u32(k) * 32);
        r.sl(t + 2 * 32, r.l(t + 2 * 32) + (u32(-shift) << 16));
        r.sl(t + 3 * 32, r.l(t + 3 * 32) + (u32(widen - shift) << 16));
    }

    // spawn, door and range tables (screen-relative X)
    // Pre-placed formations bypass the random placement path. Validate entire
    // packed records, including Y, before remapping their X with the floor.
    constexpr u32 formation_ptrs[] = STV_ROM_formation_ptrs;
    constexpr u32 formations[3][16] = STV_ROM_formations;
    auto reject_initial = [&](u32 address) {
        m.fault("initial placement table does not match L8 reference", address);
        m.stop = true;
        return false;
    };
    for (u32 i=0; i<11; ++i)
        if (r.l(kInitialFormations+i*32)!=formation_ptrs[i]) return reject_initial(kInitialFormations+i*32);
    for (u32 table=0; table<3; ++table)
        for (u32 i=0; i<16; ++i) {
            const u32 address=0xffe4d100u+table*0x200+i*32;
            const u32 value=formations[table][i];
            if (r.l(address)!=value) return reject_initial(address);
            r.sl(address,(value&0xffff0000u)|u16(arena_x(int(value&0xffff),widen)));
        }
    for (u32 i=0; i<2; ++i) {
        const u32 address=0xfff1ab80u+i*32;
        const int x=i==0?117:285;
        if (r.l(address)!=(u32(x)<<16)) return reject_initial(address);
        r.sl(address,u32(arena_x(x,widen))<<16);
    }
    auto get = [&](u32 a, Field f) -> int {
        return f == Field::FixHi ? int(int16_t(r.w(a + 16))) : int(int16_t(r.w(a)));
    };
    auto put = [&](u32 a, Field f, int x) { r.w(f == Field::FixHi ? a + 16 : a) = u16(x); };
    for (const XTable& t : x_tables()) {
        const u32 stride = t.f == Field::Word ? 16 : 32;
        bool ok = true;
        for (std::size_t k = 0; k < t.ref.size(); ++k) ok = ok && get(t.addr + u32(k) * stride, t.f) == t.ref[k];
        if (!ok) {
            m.fault("enhanced table does not match L8 reference", t.addr);
            m.stop = true;
            return false;
        }
        for (std::size_t k = 0; k < t.ref.size(); ++k) put(t.addr + u32(k) * stride, t.f, arena_x(t.ref[k], widen));
    }
    // The studio is an intact central set with new side wings. Its actors,
    // title overlay and crowd share that same translation. Validate complete
    // retail records before changing typed coordinates in the native data copy.
    constexpr u16 logo_smash[] = STV_ROM_logo_smash;
    constexpr u16 logo_tv[] = STV_ROM_logo_tv;
    constexpr u16 hands_x[] = STV_ROM_hands_x;
    constexpr u16 walk_p1[] = STV_ROM_walk_p1;
    constexpr u16 walk_p2[] = STV_ROM_walk_p2;
    constexpr u16 walk_left[] = STV_ROM_walk_left;
    constexpr u16 walk_right[] = STV_ROM_walk_right;
    constexpr u32 movement_scripts[] = STV_ROM_movement_scripts;
    for (std::size_t i=0; i<std::size(movement_scripts); ++i)
        if (r.l(0xffea3df0u+u32(i)*32) != movement_scripts[i]) {
            m.fault("movement script table does not match L8 reference",0xffea3df0u+u32(i)*32);
            m.stop=true;
            return false;
        }
    auto verify_words = [&](u32 address, const auto& expected) {
        for (std::size_t i=0; i<std::size(expected); ++i)
            if (r.w(address+u32(i)*16) != expected[i]) {
                m.fault("studio data does not match L8 reference",address+u32(i)*16);
                m.stop=true;
                return false;
            }
        return true;
    };
    if (!verify_words(0xffe97900u,logo_smash) || !verify_words(0xffe979d0u,logo_tv) ||
        !verify_words(0xffe70aa0u,hands_x) || !verify_words(0xffea44f0u,walk_p1) ||
        !verify_words(0xffea4600u,walk_p2) ||
        !verify_words(0xffea5450u,walk_left) || !verify_words(0xffea4d60u,walk_right)) return false;
    // SMASHON uses absolute world coordinates (no ADJSTWTL). PUTBGND moves
    // the studio camera left by widen, so its centred overlay shifts by -ceil(widen/2).
    r.w(0xffe97910u)=u16(192-(widen-widen/2));
    r.w(0xffe979e0u)=u16(192-(widen-widen/2));
    for (std::size_t i=0; i<std::size(hands_x); ++i)
        r.w(0xffe70aa0u+u32(i)*16)=u16(hands_x[i]+widen/2);
    // HNORMVEL is 0x16000 = 11/8 pixels per original game tick. The studio
    // start moves left by ceil(widen/2) in world coordinates, while the first
    // arena's centre moves right by floor(widen/2): travel grows by all widen
    // pixels. Extend only the final straight leg; keep every turn, Y coordinate
    // and velocity original. INIT_GAME's matching camera wait is source-bound
    // in enhanced_patchpoints.json, leaving other wave transitions unchanged.
    const int corridor_ticks=(widen*8+10)/11;
    r.w(0xffea45c0u)=u16(330+corridor_ticks);
    r.w(0xffea46d0u)=u16(330+corridor_ticks);

    // A lateral doorway already follows the widened room edge. Its destination
    // is half an added room width farther inside the next arena. Round that
    // distance to the nearest unchanged 11/8-pixel movement tick (50 for137px).
    // The longer native camera scroll supplies the corresponding entry time;
    // top/bottom scripts and all scripted velocities remain original.
    // At the player's 137-column extension the last movement update and the
    // script's neutral command can share a dispatcher scan. In the recorded
    // first-room exit, 50 extra sleeps yield only 66 extra pixels of travel.
    // One further sleep yields 70.125, closer to the 68.5-pixel target.
    const int lateral_ticks=(widen*4+5)/11 + (widen==137 ? 1 : 0);
    r.w(0xffea5460u)=u16(223+lateral_ticks);
    r.w(0xffea4d70u)=u16(223+lateral_ticks);

    // exit arrows: per circuit, zero-terminated X lists per wave (16.16), ended by an
    // empty list; the Y lists follow and are unchanged
    for (int c = 0; c < 3; ++c) {
        u32 a = r.l(kCircTbl + u32(c) * 32);
        for (int wave = 0; wave < 64 && r.l(a) != 0; ++wave) {
            for (; r.l(a) != 0; a += 32) put(a, Field::FixHi, arena_x(get(a, Field::FixHi), widen));
            a += 32;
        }
    }
    // exit boxes: per wave 4 rows x 3 [Y,X] longs (upper-left row, lower-right row,
    // destination, from-door); only the corner rows hold X, and 0 means no box
    std::set<u32> boxes_done;
    for (int c = 0; c < 3; ++c) {
        const u32 base = r.l(kCirc2Tbl + u32(c) * 32);
        for (int wave = 0; wave < kCircuitWaves[c]; ++wave)
            for (int k = 0; k < 6; ++k) {
                const u32 a = base + u32(wave) * 384 + u32(k) * 32;
                // circuits 2 and 3 share one table in L8: patch each record once
                if (r.l(a) != 0 && boxes_done.insert(a).second) put(a, Field::XyLo, arena_x(get(a, Field::XyLo), widen));
            }
    }
    // HUD coin/start messages: P1 base X moves with the left HUD (+shift); the P2
    // offset (0EBH, patch point msg_p2_offset) grows by widen
    for (int k = 0; k < 8; ++k) r.w(kMsgX + u32(k) * 16) = u16(r.w(kMsgX + u32(k) * 16) - shift);
    // Relocate only typed data-pointer slots. Numeric coordinates, sentinel
    // values, graphics addresses and compiled callback PCs are never rewritten.
    // The emitter redirects the named root consumers; all original ROM accesses,
    // including diagnostics invoked during live gameplay, still see real bytes.
    std::set<u32> pointer_slots;
    for (u32 i=0; i<11; ++i) pointer_slots.insert(kInitialFormations+i*32);
    // GMOVTAB is a table of input scripts, not compiled callback addresses.
    // REG selects TVMOV/TVMOV2 directly when FROMDR is zero. Only the two
    // horizontal room-entry scripts require the native longer interior walk.
    pointer_slots.insert(0xffea3df0u + 32); // LFTDOOR
    pointer_slots.insert(0xffea3df0u + 64); // RGTDOOR
    for (int c = 0; c < 3; ++c) {
        u32 e = r.l(kGameMods + u32(c) * 32);
        for (int k = 0; k < 200 && r.l(e) != 0xFFFFFFFFu; ++k, e += 64)
            pointer_slots.insert(e);
        for (const u32 root : {kGameMods, kCrctTab, kBndTbl, kCircTbl, kCirc2Tbl, 0xffe13790u})
            pointer_slots.insert(root + u32(c) * 32);
    }
    for (const auto& [original, replacement] : relocated) {
        if (original == replacement) continue;
        pointer_slots.insert(replacement + 48);    // block list
        pointer_slots.insert(replacement + 80);    // image dimensions
        pointer_slots.insert(replacement + 112);   // palettes
    }
    for (const u32 slot : pointer_slots) {
        const u32 pointer = r.l(slot);
        if (pointer < 0xFF800000u) {
            m.fault("native enhancement pointer is not L8 data", slot);
            m.stop = true;
            return false;
        }
        r.sl(slot, enhanced_data_address(pointer));
    }
    // RMAX/GMAX crowds +50%; ORB/HULK/FAT caps +25% (rounded down).
    // The wider arena needs crowd density as well as faster movement. Keep
    // ORB/HULK/FAT caps on the smaller increase rather than multiplying every
    // enemy family's cap by the same amount. Retail SETMAX owns the modifiers.
    // Only verified table extents are changed; signed modifiers and spawn totals
    // remain owned by the original room logic. Single tanks stay single.
    const std::pair<u32,std::vector<u8>> population[] = {
        {0xffe13018u, STV_ROM_POP_ffe13018}, // RMAX2
        {0xffe12e98u, STV_ROM_POP_ffe12e98}, // OMAX
        {0xffe13178u, STV_ROM_POP_ffe13178}, // GMAX2
        {0xffe132c8u, STV_ROM_POP_ffe132c8}, // HMAX2
        {0xffe12f58u, STV_ROM_POP_ffe12f58}, // RMAX
        {0xffe130d8u, STV_ROM_POP_ffe130d8}, // GMAX
        {0xffe12d80u, STV_ROM_POP_ffe12d80}, // HMAX
        {0xffe12e40u, STV_ROM_POP_ffe12e40}, // FMAX
    };
    for (const auto& [base, values] : population) {
        for (u32 i=0; i<values.size(); ++i) {
            const u32 address=base+i*8, aligned=address&~15u;
            const unsigned shift=(address&8u);
            const u8 original=u8(r.w(aligned)>>shift);
            if (original!=values[i]) {
                m.fault("widescreen population table mismatch",address);
                m.stop=true; return false;
            }
            const bool crowd = base==0xffe13018u || base==0xffe12f58u ||
                               base==0xffe13178u || base==0xffe130d8u;
            const u8 count=original<=2 ? original :
                u8(crowd ? original*7/4 : original*3/2);
            r.w(aligned)=u16((r.w(aligned)&~(0xffu<<shift))|(u16(count)<<shift));
        }
    }
    // Bonus foreground prize piles are screen-relative, unlike the text's
    // WAVEYX owner. Shift both player columns with the 393px rack-up module.
    constexpr u8 prize_x[] = STV_ROM_prize_x;
    for (unsigned i=0;i<std::size(prize_x);++i) {
        const u32 a=0xffe85200u+i*8, aligned=a&~15u;
        const unsigned bit=a&8u;
        if (u8(r.w(aligned)>>bit)!=prize_x[i]) {
            m.fault("widescreen bonus prize table mismatch",a);m.stop=true;return false;
        }
        const u16 x=u16(prize_x[i]+widen/2-shift);
        r.w(aligned)=u16((r.w(aligned)&~(0xffu<<bit))|(x<<bit));
    }
    // Every playable stage has an explicit reviewed population policy. Check
    // all six retail fields before modifying only ordinary spawn budgets.
    for (const auto& row : stage_population) {
        for (unsigned field=0; field<6; ++field) {
            if (r.w(row.address+field*16)!=row.original[field]) {
                m.fault("widescreen stage population mismatch",row.address+field*16);
                m.stop=true; return false;
            }
        }
        r.w(row.address+16)=row.budget;
    }
    // Widescreen movement: 25% faster, preserving directions and shoe advantage.
    {
        constexpr s32 expected[] = STV_ROM_expected_FFE1F8A0;
        for (u32 i=0; i<10; ++i) {
            const u32 address=0xffe1f8a0u+i*32;
            if (r.l(address)!=u32(expected[i])) {
                m.fault("widescreen player velocity table mismatch",address);
                m.stop=true; return false;
            }
            r.sl(address,u32(s32(s64(expected[i])*5/4)));
        }
    }
    {
        constexpr s32 expected[] = STV_ROM_expected_FFE1F9E0;
        for (u32 i=0; i<10; ++i) {
            const u32 address=0xffe1f9e0u+i*32;
            if (r.l(address)!=u32(expected[i])) {
                m.fault("widescreen player velocity table mismatch",address);
                m.stop=true; return false;
            }
            r.sl(address,u32(s32(s64(expected[i])*5/4)));
        }
    }
    {
        constexpr s32 expected[] = STV_ROM_expected_FFE1FB20;
        for (u32 i=0; i<10; ++i) {
            const u32 address=0xffe1fb20u+i*32;
            if (r.l(address)!=u32(expected[i])) {
                m.fault("widescreen player velocity table mismatch",address);
                m.stop=true; return false;
            }
            r.sl(address,u32(s32(s64(expected[i])*5/4)));
        }
    }
    {
        constexpr s32 expected[] = STV_ROM_expected_FFE1FC60;
        for (u32 i=0; i<10; ++i) {
            const u32 address=0xffe1fc60u+i*32;
            if (r.l(address)!=u32(expected[i])) {
                m.fault("widescreen player velocity table mismatch",address);
                m.stop=true; return false;
            }
            r.sl(address,u32(s32(s64(expected[i])*5/4)));
        }
    }
    m.enhanced_data = std::move(working);
    m.gfx = std::move(working_gfx);
    m.static_data_replaced();
    return true;
}

static bool enhanced_actor_owner(const Machine& m,u32 parent,u32 image,u16 pid) {
    // Bind both live lists, the actor kind, saved image and reverse owner link.
    // This scoped check does not close arbitrary same-slot/same-object reuse.
    constexpr u32 process_first=0x01086790u, process_stride=0x5a0u;
    constexpr u32 object_first=0x010003a0u, object_stride=0x1f0u;
    auto process=[](u32 p){return p>=process_first && p<process_first+240*process_stride &&
                                  (p-process_first)%process_stride==0;};
    auto object=[](u32 p){return p>=object_first && p<object_first+420*object_stride &&
                                 (p-object_first)%object_stride==0;};
    auto word=[&](u32 p){return m.ram[(p-0x01000000u)>>4];};
    auto value=[&](u32 p){return u32(word(p)) | (u32(word(p+16))<<16);};
    if(!process(parent) || !object(image) || word(parent+0x20)!=pid ||
       value(parent+0xc0)!=image || value(image+0x160)!=parent)return false;
    bool parent_active=false;
    for(u32 p=value(0x01000000u),n=0;p && n<240;++n){
        if(!process(p))return false;
        if(p==parent){parent_active=true;break;}
        p=value(p);
    }
    if(!parent_active)return false;
    for(u32 p=value(0x01000040u),n=0;p && n<420;++n){
        if(!object(p))return false;
        if(p==image)return true;
        p=value(p);
    }
    return false;
}

bool enhanced_tank_reaction_parent(const Machine& m) {
    return enhanced_actor_owner(m,m.c.a[9],m.c.a[8],239);
}

bool enhanced_mutoid_lineup_parent(const Machine& m) {
    const u32 parent=m.c.a[13];
    if(parent<0x01086790u || parent>=0x010dad90u || (parent-0x01086790u)%0x5a0u)return false;
    const u32 index=(parent+0x100u-0x01000000u)>>4;
    const u32 image=u32(m.ram[index]) | (u32(m.ram[index+1])<<16);
    return enhanced_actor_owner(m,parent,image,214);
}

}  // namespace stv::gsp
