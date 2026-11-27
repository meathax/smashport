// Weapon/power-up variants (enhanced_l8_16_9 with variant_mode != 0).
// Every entry point is a retail hook bound by tools/gsp2cpp/emit.py; the
// translated program keeps running its own processes and objects. Variant
// children are ordinary retail weapon processes (WPN2/WPN3) created through a
// native copy of MPROC GETPRC, then steered at their own creation points.
// State lives in Machine::variants (serialized) and in guest RAM; every roll
// derives from guest state. Design and hook PCs: docs/WEAPON_VARIANTS.md.
#include "stv/gsp/machine.hpp"
#include "stv/gsp/ops.hpp"
#include "stv/gsp/scene.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace stv::gsp {

namespace {
// ---- L8 RAM / ROM addresses (bit addresses), verified against the translation index
constexpr u32 kActive = 0x01000000, kFree = 0x01000020;
constexpr u32 kProcPool = 0x01086790, kProcStride = 0x5a0;
constexpr u32 kPlyrPrcs = 0x010de2b0, kPlyrObjs = 0x010dd780;
constexpr u32 kWorldTL = 0x01000280;         // [Y:X] packed
constexpr u32 kCircuit = 0x010eb610, kBossOn = 0x010de430, kGameState = 0x010865a0;
constexpr u32 kLasrX = 0x010ee9f0, kBDir = 0x010de480, kShkOn = 0x010eea48;
constexpr u32 kP1Data = 0x01086220;
// process fields
constexpr u32 PROCID = 0x20, PTIME = 0x30, PSPTR = 0x40, PA11 = 0x60, PA10 = 0x80, PA9 = 0xa0, PA8 = 0xc0, PWAKE = 0xe0;
constexpr u32 BLTDIR = 0x100, AGE = 0x110, MYPLYR = 0x138, HITS = 0x1a0, WPN_TYP = 0x198, PNUM = 0x1d8,
              SHOTCNT = 0x220, LEG_PRC = 0x2c0, LSRPTR = 0x2e0, TSO_PRC = 0x160, SHOECNT = 0x180;
// object fields
constexpr u32 OXVEL = 0x20, OYVEL = 0x40, OXVAL = 0x60, OXPOS = 0x70, OYVAL = 0x80, OYPOS = 0x90, OFLAGS = 0xb0,
              OSIZEX = 0xf0, OSIZEY = 0x100, OID = 0x150, OPLINK = 0x160;
// retail entry points
constexpr u32 WPN1 = 0xffe371d0, WPN1A = 0xffe370c0, WPN2 = 0xffe36d80, WPN2A = 0xffe36cd0, WPN3 = 0xffe36370,
              WPN3A = 0xffe362e0, WPN4 = 0xffe34e20, WPN5 = 0xffe34ae0, WPN5A = 0xffe34610, CHAINSAW = 0xffed29b0,
              RINGS = 0xffe10af0, LASER = 0xffed3d00, LASERDIE = 0xffed42f0, SBOMB = 0xffe30980, SBSND = 0xffe30630,
              CASH_LPQ = 0xffed88b0, SHAKE_CALL = 0xfff06540;
constexpr u32 KILL_BULL = 0xffe322d0, KILL_CBALL = 0xffee8210, KILL_PLYR = 0xffe19220;
constexpr u16 BULPID = 0x11e, LSRPID = 0xe7;

enum : u32 {
    H_TICK = 0xffe0a440, H_GETPRC = 0xffe0ac50, H_ICON = 0xffed6990,
    H_POW1ON = 0xffed25d0, H_POW2ON = 0xffed3ba0, H_SPRAYON = 0xffe31a80, H_PODON = 0xffe31cc0, H_EXTRA = 0xffe27880,
    H_GRENON = 0xffe31700, H_RINGON = 0xffe2fd40, H_NSTPON = 0xffe314c0, H_SHOEON = 0xffe30230, H_SMRTON = 0xffe30450,
    H_EXTRA_DO = 0xffe27920, H_FILL = 0xffe28150, H_FIRE = 0xffe337a0, H_SPRAY_LOOP = 0xffe33c10,
    H_GREN_LOOP = 0xffe34320, H_ROCKET_LOOP = 0xffe34450, H_NEED_END = 0xffe36c60, H_DOIT = 0xffe373d0,
    H_LPTP = 0xffe37400, H_LP_WALL = 0xffe374a0, H_POD_DOME = 0xffe36510, H_POD_OBJ = 0xffe36540,
    H_POD_BOOM = 0xffe365d0, H_GREN_OBJ = 0xffe35160, H_GREN_BOOM = 0xffe35c10, H_MISS = 0xffe346f0,
    H_MISS_LOOP = 0xffe34720, H_MISS_WALL = 0xffe347c0, H_COLL = 0xffe557c0, H_RBTEXP = 0xfff2bb40,
    H_KILLPLYR = 0xffe19560, H_RINGS = 0xffe10af0, H_SHOECNT = 0xffe30360, H_STUFFVEL = 0xffe1ff50,
    H_SAWS = 0xffed2890, H_ORBIT1 = 0xffed2b90, H_ORBIT2 = 0xffed2c20, H_CHAINHIT = 0xffed3920,
    H_ENCIRCLE = 0xffed2cf0, H_HELPER = 0xffed4090, H_SBVICTIM = 1,
};
// redirect targets
constexpr u32 R_RETS = 0xffe33980, R_SPRAY_CREATE = 0xffe33b90, R_GREN_CREATE = 0xffe342a0, R_ROCKET_CREATE = 0xffe343d0,
              R_REGEXPNO = 0xffe37c30, R_C1 = 0xffe37660, R_GAMOK = 0xffe1a080, R_EXTRA_SKIP = 0xffe27940;

using V = Variant;
inline u8 vid(V v) { return u8(v); }

int proc_slot(u32 p) {
    const u32 o = p - kProcPool;
    return o < 240 * kProcStride && o % kProcStride == 0 ? int(o / kProcStride) : -1;
}
int obj_slot(u32 o) {
    const u32 d = o - object_pool_base;
    return d < object_slots * object_stride && d % object_stride == 0 ? int(d / object_stride) : -1;
}
u64 mix64(u64 x) {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}
bool boss_type(unsigned type) { return type == 0x0a || type == 0x18 || type == 0x19; } // cobra, mutoid, flatface
// Compass 1..8 clockwise from up (SHOTS BULXV/BULYV order), x100 unit vectors.
constexpr int kDirX[8] = {0, 71, 100, 71, 0, -71, -100, -71};
constexpr int kDirY[8] = {-100, -71, 0, 71, 100, 71, 0, -71};
constexpr u16 kConvert[9] = {1, 5, 6, 3, 2, 4, 7, 8, 6};   // SHOTS CONVERT: B_DIR -> BLTDIR
// Q15 rotations for the Wall of Lead fan (A10 3..8) and twin seeker splay.
struct Rot { s32 c, s; };
constexpr Rot kFan[6] = {{32138, 6393}, {32138, -6393}, {27246, 18205}, {27246, -18205}, {23170, 23170}, {23170, -23170}};
constexpr Rot kSplay[2] = {{32522, 3993}, {32522, -3993}};   // +-7 degrees
constexpr Rot kSteer[2] = {{32748, 1143}, {32748, -1143}};   // +-2 degrees per frame
constexpr Rot kShard[3] = {{28378, 16384}, {32767, 0}, {28378, -16384}};   // +-30 degrees
void rotate(s32& x, s32& y, Rot r) {
    const s64 nx = (s64(x) * r.c - s64(y) * r.s) / 32768, ny = (s64(x) * r.s + s64(y) * r.c) / 32768;
    x = s32(nx); y = s32(ny);
}
} // namespace

Variant variant_roll(unsigned sub, std::uint8_t mode, bool boss, std::uint64_t h) {
    if (mode == 0) return V::None;
    Variant a, b;
    variant_pair(sub, a, b);
    // Each variant owns its own band, so every variant of a tier rolls at the
    // same rate whatever its pickup's partner is (a pickup whose partner is
    // very rare, or absent outside a boss room, does not double the other).
    // Balanced (Rare): normal 1/16 each (1/8 per two-normal pickup), very rare
    // 1/50 each (1/25 per very-rare pair). Frequent: 1/8 and 1/20 each.
    const unsigned rare_each = mode == 2 ? 50 : 20, normal_each = mode == 2 ? 125 : 62;
    const unsigned u = unsigned(h % 1000);
    unsigned base = 0;
    for (Variant v : {a, b}) {
        if (v == V::None || (v == V::QuakeShell && !boss)) continue;
        const unsigned band = variant_info(v).very_rare ? rare_each : normal_each;
        if (u >= base && u < base + band) return v;
        base += band;
    }
    return V::None;
}

void variant_pair(unsigned sub, Variant& a, Variant& b) {
    switch (sub) {
    case 0: a = V::BuzzStorm; b = V::ThrownBlades; return;            // blades
    case 1: a = V::HotLead; b = V::RicochetRounds; return;            // helper pickup: default-gun variants
    case 2: a = V::WallOfLead; b = V::ShrapnelSpread; return;         // spray
    case 3: a = V::ClusterGrenade; b = V::NapalmGrenade; return;      // pod bombs
    case 4: a = V::Clone; b = V::LastStand; return;                   // extra man
    case 5: a = V::MortarBarrage; b = V::QuakeShell; return;          // grenade lobber
    case 6: a = V::Reflector; b = V::ShockwaveShield; return;         // force field
    case 7: a = V::TunnelRocket; b = V::TwinSeekers; return;          // non-stop rockets
    case 9: a = V::Afterburner; b = V::BulldozerBoots; return;        // shoes
    case 10: a = V::DoubleTap; b = V::GoldRushBomb; return;           // smart bomb
    default: a = b = V::None; return;
    }
}

namespace {
struct Ctx {
    Machine& m;
    Cpu& c;
    VariantState& s;
    // 16-bit fields may sit on byte boundaries (WPN_TYP is PDATA+152).
    u16 w(u32 a) { return u16(rfield(m, a, 16)); }
    u32 l(u32 a) { return rlong(m, a); }
    void ww(u32 a, u16 v) { wfield(m, a, v, 16); }
    void wl(u32 a, u32 v) { wlong(m, a, v); }
    // player 0/1 from a torso/legs process, or -1
    int player_of_proc(u32 p) {
        if (proc_slot(p) < 0) return -1;
        for (int k = 0; k < 2; ++k) if (l(kPlyrPrcs + k * 32) == p) return k;
        const u32 torso = l(p + TSO_PRC);
        for (int k = 0; k < 2; ++k) if (proc_slot(torso) >= 0 && l(kPlyrPrcs + k * 32) == torso) return k;
        return -1;
    }
    int player_of_obj(u32 o) {
        if (obj_slot(o) < 0) return -1;
        const u16 id = w(o + OID);
        if (id == 0x4101 || id == 0x4102) return id - 0x4101;
        return player_of_proc(l(o + OPLINK));
    }
    u32 torso_proc(int p) { return l(kPlyrPrcs + p * 32); }
    u32 torso_obj(int p) { const u32 o = l(kPlyrObjs + p * 32); return obj_slot(o) >= 0 ? o : 0; }
    Variant weapon(int p, unsigned type) {
        if (p < 0 || p > 1) return V::None;
        auto& pl = s.player[p];
        const u32 tp = torso_proc(p);
        if (!pl.weapon || pl.weapon_type != type || proc_slot(tp) < 0 || w(tp + WPN_TYP) != type) return V::None;
        return Variant(pl.weapon);
    }
    s32 cx(u32 o) { return s16(w(o + OXPOS)) + s32(w(o + OSIZEX)) / 2; }
    s32 cy(u32 o) { return s16(w(o + OYPOS)) + s32(w(o + OSIZEY)) / 2; }
    bool queue(VariantSpawnKind kind, Variant v, int owner, s32 x, s32 y, u16 age = 1, s32 vx = 0, s32 vy = 0) {
        if (s.spawn_count >= s.spawn.size()) return false;
        s.spawn[s.spawn_count++] = {u8(kind), vid(v), u8(owner + 1), 0, age, 0, x, y, vx, vy};
        return true;
    }
    template <class F> void each_active(F&& f) {
        u32 p = l(kActive);
        for (int guard = 0; p && guard < 240; ++guard) {
            if (proc_slot(p) < 0) return;
            f(p, proc_slot(p));
            p = l(p);
        }
    }
    // Free retail processes; variant extras keep a reserve so retail CREATEs never starve.
    int free_procs(int cap = 64) {
        int n = 0;
        for (u32 p = l(kFree); p && n < cap && proc_slot(p) >= 0; p = l(p)) ++n;
        return n;
    }
    static constexpr int kReserve = 24;
    int live_children(VariantChild kind, int owner) {
        int n = 0;
        each_active([&](u32, int k) { if (s.proc_kind[k] == u8(kind) && s.proc_owner[k] == owner + 1) ++n; });
        return n;
    }
    void clear_proc(int k) {
        s.proc_var[k] = s.proc_kind[k] = s.proc_owner[k] = s.proc_flags[k] = 0;
        s.proc_aux[k] = s.proc_last[k] = 0;
        s.proc_x[k] = s.proc_y[k] = s.proc_vx[k] = s.proc_vy[k] = 0;
    }
    void notify(int p, Variant v) { if (v != V::None && m.on_variant_pickup) m.on_variant_pickup(p, vid(v)); }
};
} // namespace

bool Machine::variant_state_empty() const {
    static const VariantState empty{};
    return std::memcmp(&variants, &empty, sizeof empty) == 0;
}

void Machine::variant_object_inserted(u32 node) {
    const int k = obj_slot(node);
    if (k >= 0) variants.icon[std::size_t(k)] = 0;
}

u32 Machine::variant_spawn(u32 prev, u16 pid, u32 wake, u32 a8, u32 a9, u32 a10, u32 a11) {
    // MPROC GETPRC/XFERPRC0, natively: unlink FREE, link after prev, stuff wake/regs/stack.
    const u32 p = rlong(*this, kFree);
    const int k = proc_slot(p);
    if (k < 0 || (prev != kActive && proc_slot(prev) < 0)) return 0;
    if (Ctx{*this, c, variants}.free_procs(Ctx::kReserve) < Ctx::kReserve) return 0;
    wlong(*this, kFree, rlong(*this, p));
    wlong(*this, p, rlong(*this, prev));
    wlong(*this, prev, p);
    wlong(*this, p + PWAKE, wake); wlong(*this, p + PA8, a8); wlong(*this, p + PA9, a9);
    wlong(*this, p + PA10, a10); wlong(*this, p + PA11, a11); wlong(*this, p + PSPTR, p + kProcStride);
    ww(p + PTIME, 1); ww(p + PROCID, pid);
    shot_aim[std::size_t(k)] = 0;
    shot_muzzles[std::size_t(k)] = 1;   // independent emitter: no muzzle snap, no analog re-aim
    Ctx{*this, c, variants}.clear_proc(k);
    return p;
}

u32 Machine::variant_draw_tint(u32 node) const {
    const int k = obj_slot(node);
    if (k < 0) return 0;
    auto& self = const_cast<Machine&>(*this);
    const u16 id = self.rw(node + OID);
    if ((id & 0x1f00) == 0x0c00) {
        const u8 v = variants.icon[std::size_t(k)];
        if (!v) return 0;
        const u32 t = variant_info(Variant(v)).tint;
        return ((frame_number >> 3) & 1) ? (t | 0x1000000u) : t;   // flashing pickup
    }
    const int pk = proc_slot(rlong(self, node + OPLINK));
    if (pk < 0 || !variants.proc_var[std::size_t(pk)]) return 0;
    return variant_info(Variant(variants.proc_var[std::size_t(pk)])).tint;
}

u16 Machine::variant_pen(u16 word, u32 tint) {
    if (variant_pen_cache.size() != 4096) variant_pen_cache.assign(4096, {});
    const u32 h = (u32(word) * 0x9e3779b1u) ^ (tint * 0x85ebca6bu);
    for (u32 probe = 0; probe < 4; ++probe) {
        const auto& e = variant_pen_cache[(h + probe) & 4095];
        if (e.epoch == palette_epoch && e.key == (u32(word) | (tint & 0x1000000u)) && e.tint == (tint & 0xffffff)) return e.value;
    }
    const u16 pen = u16(((word & 0xc000) >> 8) | (word & 0x0f3f));
    const u32 rgb = pens[pen & 0x0fff];
    const int r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255;
    const int lum = (r * 77 + g * 150 + b * 29) >> 8;
    int t[3] = {int((tint >> 16) & 255), int((tint >> 8) & 255), int(tint & 255)};
    for (int& v : t) {
        v = std::min(255, v * (64 + lum * 3 / 2) / 256);
        if (tint & 0x1000000u) v = (v + 255 + lum) / 3;   // flash: toward white
    }
    u16 best = pen;
    long best_d = 0x7fffffff;
    for (u16 q = 1; q < 4096; ++q) {
        const u32 c2 = pens[q];
        const long dr = long((c2 >> 16) & 255) - t[0], dg = long((c2 >> 8) & 255) - t[1], db = long(c2 & 255) - t[2];
        const long d = 2 * dr * dr + 4 * dg * dg + 3 * db * db;
        if (d < best_d) { best_d = d; best = q; }
    }
    const u16 out = u16((best & 0x0f3f) | ((best & 0xc0) << 8));
    auto& e = variant_pen_cache[h & 4095];
    e = {palette_epoch, u32(word) | (tint & 0x1000000u), tint & 0xffffff, out};
    return out;
}

u32 Machine::variant_hook(u32 pc) {
    Ctx x{*this, c, variants};
    auto& s = variants;
    switch (pc) {
    // ------------------------------------------------------------ per dispatch pass
    case H_TICK: {
        if (x.w(kGameState) == 0x30) {   // attract mode after a game: nothing carries into the next credit
            s.player = {};
            s.spawn_count = 0;
            s.stun_timer = 0;
            return 0;
        }
        for (int p = 0; p < 2; ++p) {
            auto& pl = s.player[p];
            if (pl.gun_timer && !--pl.gun_timer) pl.gun = 0;
            if (pl.throw_cooldown) --pl.throw_cooldown;
            if (pl.grace) --pl.grace;
            if (pl.gold_timer) --pl.gold_timer;
            if (pl.double_tap_timer && !--pl.double_tap_timer) {
                const u32 obj = pl.double_tap_obj;
                if (obj_slot(obj) >= 0 && (x.w(obj + OID) == 0x4101 + p)) x.queue(VariantSpawnKind::SmartBomb, V::DoubleTap, p, 0, 0);
            }
            if (pl.ring_timer && !--pl.ring_timer && pl.ring == vid(V::ShockwaveShield)) {
                // Shockwave: a blast ring around the player plus one on each enemy within ~1/4 screen.
                if (const u32 o = x.torso_obj(p)) {
                    const s32 px = x.cx(o), py = x.cy(o);
                    for (int d = 0; d < 8; ++d) x.queue(VariantSpawnKind::Blast, V::ShockwaveShield, p, px + kDirX[d] * 44 / 100, py + kDirY[d] * 44 / 100);
                    for (int k = 0; k < int(object_slots); ++k) {
                        const u32 e = object_pool_base + u32(k) * object_stride;
                        const u16 id = x.w(e + OID);
                        if (!(x.w(e + OFLAGS) & 0x8000) || (id & 0xe000) != 0x8000 || ((id >> 8) & 0x1f) == 0x15) continue;
                        const s32 dx = x.cx(e) - px, dy = x.cy(e) - py;
                        if (dx * dx + dy * dy <= 100 * 100) x.queue(VariantSpawnKind::Blast, V::ShockwaveShield, p, x.cx(e), x.cy(e));
                    }
                }
                pl.ring = 0;
            }
            if (pl.clone_timer && !--pl.clone_timer) {
                // Clone time is up: the helper leaves as after a hit (POW2 LSHIT).
                x.each_active([&](u32 proc, int k) {
                    if (s.proc_var[k] != vid(V::Clone) || x.w(proc + PROCID) != LSRPID) return;
                    const u32 tp = x.torso_proc(p);
                    if (proc_slot(tp) >= 0) { const u32 hob = x.l(tp + LSRPTR); if (obj_slot(hob) >= 0) x.ww(hob + OID, 0); x.wl(tp + LSRPTR, 0); }
                    x.wl(proc + PWAKE, LASERDIE);
                    s.proc_var[k] = 0;
                });
                pl.clone = 0;
            }
            // Afterburner: a short flame trail while the shoes run.
            if (pl.shoe == vid(V::Afterburner)) {
                const u32 tp = x.torso_proc(p);
                const u32 legs = proc_slot(tp) >= 0 ? x.l(tp + LEG_PRC) : 0;
                if (proc_slot(legs) >= 0 && x.w(legs + SHOECNT)) {
                    if (++pl.trail_timer >= 8) {
                        pl.trail_timer = 0;
                        const u32 o = x.torso_obj(p);
                        if (o && x.live_children(VariantChild::Trail, p) < 4)
                            x.queue(VariantSpawnKind::Trail, V::Afterburner, p, x.cx(o) - 4, x.cy(o) + 18, 14);
                    }
                } else pl.shoe = 0;
            } else if (pl.shoe == vid(V::BulldozerBoots)) {
                const u32 tp = x.torso_proc(p);
                const u32 legs = proc_slot(tp) >= 0 ? x.l(tp + LEG_PRC) : 0;
                if (proc_slot(legs) < 0 || !x.w(legs + SHOECNT)) pl.shoe = 0;
            }
        }
        if (s.stun_timer) {
            --s.stun_timer;
            for (int k = 0; k < int(object_slots); ++k) {
                const u32 e = object_pool_base + u32(k) * object_stride;
                const u16 id = x.w(e + OID);
                if (!(x.w(e + OFLAGS) & 0x8000) || (id & 0xe000) != 0x8000) continue;
                const unsigned type = (id >> 8) & 0x1f;
                if (type == 0x15 || type == 0x16) continue;   // shots keep flying
                x.wl(e + OXVEL, 0); x.wl(e + OYVEL, 0);
            }
        }
        // Create queued children (linked at the head of the active list).
        const u16 n = s.spawn_count;
        s.spawn_count = 0;
        for (u16 i = 0; i < n; ++i) {
            const VariantSpawn q = s.spawn[i];
            const int owner = q.owner - 1;
            const u32 torso = owner >= 0 && owner < 2 ? x.torso_obj(owner) : 0;
            const u32 a8 = torso ? torso : object_pool_base;
            u32 proc = 0;
            VariantChild kind = VariantChild::None;
            switch (VariantSpawnKind(q.kind)) {
            case VariantSpawnKind::Blast: kind = VariantChild::Blast; proc = variant_spawn(kActive, BULPID, WPN3, a8, 0, 0, u32(q.owner)); break;
            case VariantSpawnKind::Patch: kind = VariantChild::Patch; proc = variant_spawn(kActive, BULPID, WPN3, a8, 0, 0, u32(q.owner)); break;
            case VariantSpawnKind::Trail: kind = VariantChild::Trail; proc = variant_spawn(kActive, BULPID, WPN3, a8, 0, 0, u32(q.owner)); break;
            case VariantSpawnKind::Fragment: kind = VariantChild::Fragment; proc = variant_spawn(kActive, BULPID, WPN2, a8, 0, 0, u32(q.owner)); break;
            case VariantSpawnKind::Shake:
                if (!rb(*this, kShkOn)) variant_spawn(kActive, 0, SHAKE_CALL, 0, 0, u32(q.age), 0);
                break;
            case VariantSpawnKind::SmartBomb:
                if (torso) { variant_spawn(kActive, 0, SBSND, 0, 0, 0, 0); variant_spawn(kActive, 0, SBOMB, 0, torso, 0, 0); }
                break;
            case VariantSpawnKind::Cash: {
                const u32 tl = rlong(*this, kWorldTL);
                const s32 sx = q.x - s16(tl), sy = q.y - s16(tl >> 16);
                variant_spawn(kActive, 0, CASH_LPQ, 0, 1, u32(std::clamp(sx - 37, 40, 300)), u32(std::clamp(sy - 25, 50, 190)));
                break;
            }
            case VariantSpawnKind::CloneHelper:
                if (torso && !rlong(*this, kLasrX)) {
                    proc = variant_spawn(kActive, LSRPID, LASER, 0, torso, 0, 0);
                    if (const int k = proc_slot(proc); k >= 0) shot_muzzles[std::size_t(k)] = 0;
                }
                break;
            default: break;
            }
            if (const int k = proc_slot(proc); k >= 0) {
                s.proc_var[k] = q.variant; s.proc_kind[k] = u8(kind); s.proc_owner[k] = q.owner;
                s.proc_aux[k] = q.age; s.proc_x[k] = q.x; s.proc_y[k] = q.y; s.proc_vx[k] = q.vx; s.proc_vy[k] = q.vy;
            }
        }
        return 0;
    }
    // ------------------------------------------------------------ process creation (GETPRC)
    case H_GETPRC: {
        const int k = proc_slot(c.a[0]);
        if (k < 0) return 0;
        x.clear_proc(k);
        const u32 wake = c.a[7];
        const int owner = int(c.a[11] & 0xff) - 1;
        auto tag = [&](Variant v, int p) {
            if (v == V::None) return;
            s.proc_var[k] = vid(v); s.proc_owner[k] = u8(p + 1);
        };
        switch (wake) {
        case WPN1: case WPN1A:
            if (owner >= 0 && owner < 2 && s.player[owner].gun_timer) tag(Variant(s.player[owner].gun), owner);
            break;
        case WPN2: case WPN2A: {
            const Variant v = x.weapon(owner, 1);
            tag(v, owner);
            if (wake == WPN2 && v == V::WallOfLead && c.a[10] >= 3 && c.a[10] <= 8) { s.proc_aux[k] = u16(c.a[10]); c.a[10] = 0; }
            break;
        }
        case WPN3: case WPN3A: tag(x.weapon(owner, 2), owner); break;
        case WPN4: tag(x.weapon(owner, 3), owner); if (owner >= 0 && owner < 2) s.proc_aux[k] = s.player[owner].fire_loop; break;
        case WPN5: case WPN5A: tag(x.weapon(owner, 4), owner); if (owner >= 0 && owner < 2 && wake == WPN5) s.proc_aux[k] = s.player[owner].fire_loop; break;
        case CHAINSAW: {
            const int p = int(c.a[8] & 0xff) - 1;
            if (p >= 0 && p < 2 && s.player[p].blade) {
                tag(Variant(s.player[p].blade), p);
                if (s.player[p].blade == vid(V::ThrownBlades)) s.proc_kind[k] = u8(VariantChild::Thrown);
            }
            break;
        }
        case RINGS: {
            const int p = int(c.a[9] & 0xff) - 1;
            if (p >= 0 && p < 2 && s.player[p].ring_pending) {
                s.player[p].ring_pending = 0;
                tag(Variant(s.player[p].ring), p);
            }
            break;
        }
        default: break;
        }
        return 0;
    }
    // ------------------------------------------------------------ icon spawn: roll
    case H_ICON: {
        const u32 node = c.a[0];
        const int k = obj_slot(node);
        if (k < 0) return 0;
        s.icon[k] = 0;
        const u16 id = x.w(node + OID);
        if ((id & 0x1f00) != 0x0c00 || x.w(kGameState) == 0x30) return 0;   // no variants in the attract demo
        const u64 h = mix64(frame_number * 0x100000001b3ull ^ (u64(node) << 20) ^ (u64(x.w(node + OXPOS)) << 40) ^
                            (u64(x.w(node + OYPOS)) << 52) ^ c.cyc);
        s.icon[k] = vid(variant_roll(id & 0x3f, variant_mode, rb(*this, kBossOn) != 0, h));
        return 0;
    }
    // ------------------------------------------------------------ pickups (COLL2 ICONTBL)
    case H_POW1ON: case H_POW2ON: case H_SPRAYON: case H_PODON: case H_EXTRA: case H_GRENON:
    case H_RINGON: case H_NSTPON: case H_SHOEON: case H_SMRTON: {
        const int k = obj_slot(c.a[8]);
        Variant v = V::None;
        if (k >= 0) { v = Variant(s.icon[k]); s.icon[k] = 0; }
        const int p = x.player_of_obj(c.a[0]);
        if (p < 0) return 0;
        auto& pl = s.player[p];
        auto weapon = [&](unsigned type) { pl.weapon = vid(v); pl.weapon_type = u8(type); pl.fire_loop = 0; };
        switch (pc) {
        case H_POW1ON: pl.blade = vid(v); break;
        case H_POW2ON: if (v != V::None) { pl.gun = vid(v); pl.gun_timer = v == V::HotLead ? 480 : 600; } break;
        case H_SPRAYON: weapon(1); break;
        case H_PODON: weapon(2); break;
        case H_GRENON: weapon(3); break;
        case H_NSTPON: weapon(4); break;
        case H_RINGON: pl.ring = vid(v); pl.ring_pending = v != V::None; break;
        case H_SHOEON: pl.shoe = vid(v); break;
        case H_EXTRA: pl.extra_pending = vid(v); break;
        case H_SMRTON:
            if (v == V::DoubleTap && pl.double_tap_circuit != x.w(kCircuit) + 1) {
                pl.double_tap_timer = 120; pl.double_tap_obj = c.a[0]; pl.double_tap_circuit = u8(x.w(kCircuit) + 1);
            } else if (v == V::GoldRushBomb) { pl.gold_timer = 90; pl.gold_drops = 0; }
            break;
        default: break;
        }
        x.notify(p, v);
        return 0;
    }
    case H_EXTRA_DO: {   // SCORE EXTRA: CALLR DO_EXTRA, A4 = PNUM
        const int p = int(c.a[4] & 0xff) - 1;
        if (p < 0 || p > 1 || !s.player[p].extra_pending) return 0;
        auto& pl = s.player[p];
        const Variant v = Variant(pl.extra_pending);
        pl.extra_pending = 0;
        if (v == V::LastStand) pl.last_stand = 1;
        else if (v == V::Clone) {
            pl.clone = 1; pl.clone_timer = 600;
            const u32 hob = proc_slot(x.torso_proc(p)) >= 0 ? x.l(x.torso_proc(p) + LSRPTR) : 0;
            if (obj_slot(hob) >= 0) {   // a helper is already out: it becomes the clone
                const int hk = proc_slot(x.l(hob + OPLINK));
                if (hk >= 0) { s.proc_var[hk] = vid(V::Clone); s.proc_owner[hk] = u8(p + 1); }
            } else {
                x.queue(VariantSpawnKind::CloneHelper, V::Clone, p, 0, 0);
            }
            return 0;   // the Clone comes with the retail extra life
        }
        return R_EXTRA_SKIP;   // LastStand replaces the extra life
    }
    case H_FILL: {   // FILLUP after GETCNT: A10 = player process
        const int p = x.player_of_proc(c.a[10]);
        if (p < 0) return 0;
        const unsigned type = x.w(c.a[10] + WPN_TYP);
        const Variant v = x.weapon(p, type);
        int pct = 100;
        switch (v) {
        case V::WallOfLead: case V::NapalmGrenade: case V::TwinSeekers: pct = 40; break;
        case V::ShrapnelSpread: pct = 50; break;
        case V::ClusterGrenade: pct = 35; break;
        case V::TunnelRocket: pct = 50; break;
        case V::MortarBarrage: pct = 25; break;
        case V::QuakeShell: x.ww(c.a[10] + SHOTCNT, 5); return 0;
        default: return 0;
        }
        const u16 shots = x.w(c.a[10] + SHOTCNT);
        x.ww(c.a[10] + SHOTCNT, u16(std::max(1, shots * pct / 100)));
        return 0;
    }
    // ------------------------------------------------------------ firing (STRT_WPN)
    case H_FIRE: {
        const int p = x.player_of_proc(c.a[13]);
        if (p < 0) return 0;
        auto& pl = s.player[p];
        if (pl.blade == vid(V::ThrownBlades) && !pl.throw_cooldown) {
            u32 pick = 0;
            int pick_k = -1, index = 0, first_k = -1;
            u32 first = 0;
            x.each_active([&](u32 proc, int k) {
                if (s.proc_kind[k] != u8(VariantChild::Thrown) || s.proc_owner[k] != p + 1 || (s.proc_flags[k] & 4)) return;
                if (first_k < 0) { first = proc; first_k = k; }
                if (index++ == pl.throw_rr % 3 && pick_k < 0) { pick = proc; pick_k = k; }
            });
            if (pick_k < 0) { pick = first; pick_k = first_k; }
            if (pick_k >= 0) {
                (void)pick;
                const u16 bdir = x.w(kBDir);
                const unsigned dir = bdir >= 1 && bdir <= 9 ? kConvert[bdir - 1] : 1;
                s.proc_flags[pick_k] |= 4; s.proc_aux[pick_k] = 0;
                s.proc_vx[pick_k] = kDirX[(dir - 1) & 7]; s.proc_vy[pick_k] = kDirY[(dir - 1) & 7];
                pl.throw_rr = u8(pl.throw_rr + 1); pl.throw_cooldown = 12;
            }
        }
        if (x.weapon(p, 1) == V::WallOfLead && (++pl.fire_count & 3) == 0) return R_RETS;   // slightly slower fire
        return 0;
    }
    case H_SPRAY_LOOP: {
        const int p = x.player_of_proc(c.a[13]);
        if (p < 0 || x.weapon(p, 1) != V::WallOfLead) return 0;
        auto& pl = s.player[p];
        if (pl.fire_loop < 6 && x.free_procs(x.kReserve) >= x.kReserve) { c.a[10] = 3u + pl.fire_loop; ++pl.fire_loop; return R_SPRAY_CREATE; }
        pl.fire_loop = 0;
        return 0;
    }
    case H_ROCKET_LOOP: {
        const int p = x.player_of_proc(c.a[13]);
        if (p < 0 || x.weapon(p, 4) != V::TwinSeekers) return 0;
        auto& pl = s.player[p];
        if (!pl.fire_loop && x.free_procs(x.kReserve) >= x.kReserve) { pl.fire_loop = 1; return R_ROCKET_CREATE; }
        pl.fire_loop = 0;
        return 0;
    }
    case H_GREN_LOOP: {
        const int p = x.player_of_proc(c.a[13]);
        if (p < 0 || x.weapon(p, 3) != V::MortarBarrage) return 0;
        auto& pl = s.player[p];
        if (pl.fire_loop < 2 && x.free_procs(x.kReserve) >= x.kReserve) { ++pl.fire_loop; return R_GREN_CREATE; }
        pl.fire_loop = 0;
        return 0;
    }
    // ------------------------------------------------------------ spray / default bullets
    case H_NEED_END: {
        const int k = proc_slot(c.a[13]);
        if (k < 0 || s.proc_var[k] != vid(V::WallOfLead) || s.proc_aux[k] < 3 || s.proc_aux[k] > 8) return 0;
        s32 vx = s32(c.a[6]), vy = s32(c.a[7]);
        rotate(vx, vy, kFan[s.proc_aux[k] - 3]);
        c.a[6] = u32(vx); c.a[7] = u32(vy);
        return 0;
    }
    case H_DOIT: {
        const int k = proc_slot(c.a[13]);
        if (k < 0 || s.proc_kind[k] != u8(VariantChild::Fragment)) return 0;
        c.a[0] = u32(s.proc_x[k]) << 16; c.a[1] = u32(s.proc_y[k]) << 16;
        c.a[6] = u32(s.proc_vx[k]); c.a[7] = u32(s.proc_vy[k]);
        return 0;
    }
    case H_LPTP: {
        const int k = proc_slot(c.a[13]);
        if (k < 0 || s.proc_kind[k] != u8(VariantChild::Fragment)) return 0;
        if (s.proc_aux[k] && --s.proc_aux[k]) return 0;
        c.a[10] = 1;   // REGEXPNO selects BULL_EXP_LIST by wall
        return R_REGEXPNO;
    }
    case H_LP_WALL: {   // SCRTSTG reported wall A0 (1 left, 2 right, 3 up, 4 down)
        const int k = proc_slot(c.a[13]);
        if (k < 0 || s.proc_var[k] != vid(V::RicochetRounds) || (s.proc_flags[k] & 1)) return 0;
        const u32 o = c.a[8];
        s32 vx = s32(x.l(o + OXVEL)), vy = s32(x.l(o + OYVEL));
        if (c.a[0] == 1 || c.a[0] == 2) vx = -vx; else vy = -vy;
        x.wl(o + OXVEL, u32(vx)); x.wl(o + OYVEL, u32(vy));
        x.wl(o + OXVAL, x.l(o + OXVAL) + u32(vx) * 2); x.wl(o + OYVAL, x.l(o + OYVAL) + u32(vy) * 2);
        s.proc_flags[k] |= 1;
        return R_C1;
    }
    // ------------------------------------------------------------ pod bombs
    case H_POD_DOME: {
        const int k = proc_slot(c.a[13]);
        if (k < 0) return 0;
        const auto kind = VariantChild(s.proc_kind[k]);
        if (kind != VariantChild::Blast && kind != VariantChild::Patch && kind != VariantChild::Trail) return 0;
        c.a[0] = u32(s.proc_x[k] - 8) << 16; c.a[1] = u32(s.proc_y[k] - 8) << 16;
        c.a[6] = c.a[7] = 0;
        x.ww(c.a[13] + AGE, std::max<u16>(1, s.proc_aux[k]));
        return 0;
    }
    case H_POD_OBJ: {
        const int k = proc_slot(c.a[13]);
        if (k >= 0 && s.proc_kind[k] == u8(VariantChild::Patch) && obj_slot(c.a[8]) >= 0) x.ww(c.a[8] + OID, 0x4205);  // pierce
        return 0;
    }
    case H_POD_BOOM: {
        const int k = proc_slot(c.a[13]);
        if (k < 0 || s.proc_kind[k] || obj_slot(c.a[8]) < 0) return 0;
        const int p = s.proc_owner[k] - 1;
        const s32 px = x.cx(c.a[8]), py = x.cy(c.a[8]);
        if (s.proc_var[k] == vid(V::ClusterGrenade)) {
            for (int d = 0; d < 8; d += 2) x.queue(VariantSpawnKind::Blast, V::ClusterGrenade, p, px + kDirX[d] / 5, py + kDirY[d] / 5);
        } else if (s.proc_var[k] == vid(V::NapalmGrenade) && x.live_children(VariantChild::Patch, p) < 3) {
            x.queue(VariantSpawnKind::Patch, V::NapalmGrenade, p, px, py, 180);
        }
        return 0;
    }
    // ------------------------------------------------------------ grenade lobber
    case H_GREN_OBJ: {
        const int k = proc_slot(c.a[13]);
        if (k < 0 || s.proc_var[k] != vid(V::MortarBarrage) || !s.proc_aux[k]) return 0;
        const unsigned dir = x.w(c.a[13] + BLTDIR);
        if (dir < 1 || dir > 8) return 0;
        const s32 d = 30 * s.proc_aux[k];
        c.a[0] += u32(kDirX[dir - 1] * d / 100) << 16;
        c.a[1] += u32(kDirY[dir - 1] * d / 100) << 16;
        wlong(*this, c.sp, c.a[0]);   // WPN4 pops the saved X for the ground shadow
        return 0;
    }
    case H_GREN_BOOM: {
        const int k = proc_slot(c.a[13]);
        if (k < 0 || obj_slot(c.a[8]) < 0) return 0;
        const int p = s.proc_owner[k] - 1;
        const s32 px = x.cx(c.a[8]), py = x.cy(c.a[8]) + 6;
        if (s.proc_var[k] == vid(V::MortarBarrage)) {
            x.queue(VariantSpawnKind::Blast, V::MortarBarrage, p, px, py);
            for (int d = 0; d < 8; d += 2) x.queue(VariantSpawnKind::Blast, V::MortarBarrage, p, px + kDirX[d] / 5, py + kDirY[d] / 5);
        } else if (s.proc_var[k] == vid(V::QuakeShell)) {
            x.queue(VariantSpawnKind::Shake, V::QuakeShell, p, 0, 0, 30);
            for (int d = 0; d < 8; ++d) x.queue(VariantSpawnKind::Blast, V::QuakeShell, p, px + kDirX[d] / 4, py + kDirY[d] / 4);
            s.stun_timer = 60;
        }
        return 0;
    }
    // ------------------------------------------------------------ rockets
    case H_MISS: {
        const int k = proc_slot(c.a[13]);
        if (k < 0) return 0;
        s32 vx = s32(c.a[6]), vy = s32(c.a[7]);
        if (s.proc_var[k] == vid(V::TunnelRocket)) { vx = vx * 3 / 2; vy = vy * 3 / 2; }
        else if (s.proc_var[k] == vid(V::TwinSeekers)) rotate(vx, vy, kSplay[s.proc_aux[k] & 1]);
        else return 0;
        c.a[6] = u32(vx); c.a[7] = u32(vy);
        return 0;
    }
    case H_MISS_LOOP: {
        const int k = proc_slot(c.a[13]);
        if (k < 0 || s.proc_var[k] != vid(V::TwinSeekers) || obj_slot(c.a[8]) < 0) return 0;
        const u32 o = c.a[8];
        const s32 ox = x.cx(o), oy = x.cy(o);
        s64 best = 180 * 180;
        s32 tx = 0, ty = 0;
        bool found = false;
        for (int e = 0; e < int(object_slots); ++e) {
            const u32 n = object_pool_base + u32(e) * object_stride;
            const u16 id = x.w(n + OID);
            if ((id & 0xe000) != 0x8000 || !(x.w(n + OFLAGS) & 0x8000) || (x.w(n + OFLAGS) & 0x0800)) continue;
            const unsigned type = (id >> 8) & 0x1f;
            if (type == 0x15 || type == 0x16 || type == 0x05) continue;
            const s32 dx = x.cx(n) - ox, dy = x.cy(n) - oy;
            const s64 d = s64(dx) * dx + s64(dy) * dy;
            if (d < best) { best = d; tx = dx; ty = dy; found = true; }
        }
        if (!found) return 0;
        s32 vx = s32(x.l(o + OXVEL)), vy = s32(x.l(o + OYVEL));
        const s64 cross = s64(vx >> 8) * ty - s64(vy >> 8) * tx;
        rotate(vx, vy, kSteer[cross < 0 ? 1 : 0]);
        x.wl(o + OXVEL, u32(vx)); x.wl(o + OYVEL, u32(vy));
        return 0;
    }
    case H_MISS_WALL: {
        const int k = proc_slot(c.a[13]);
        if (k < 0 || s.proc_var[k] != vid(V::TunnelRocket) || (s.proc_flags[k] & 2) || obj_slot(c.a[8]) < 0) return 0;
        s.proc_flags[k] |= 2;
        const u32 o = c.a[8];
        const s32 vx = s32(x.l(o + OXVEL)) >> 16, vy = s32(x.l(o + OYVEL)) >> 16;
        const s32 px = x.cx(o) - vx * 3, py = x.cy(o) - vy * 3;
        const int p = s.proc_owner[k] - 1;
        x.queue(VariantSpawnKind::Blast, V::TunnelRocket, p, px, py);
        x.queue(VariantSpawnKind::Blast, V::TunnelRocket, p, px - vy * 3, py + vx * 3);
        x.queue(VariantSpawnKind::Blast, V::TunnelRocket, p, px + vy * 3, py - vx * 3);
        return 0;
    }
    // ------------------------------------------------------------ collisions (COLL2 ColLists)
    case H_COLL: {
        u32 b1 = c.b[1], b2 = c.b[2];
        auto apply = [&](u32 self, u32 other, u32& bself, u32& bother) {
            const int ks = obj_slot(self), ko = obj_slot(other);
            if (ks < 0 || ko < 0) return;
            const u16 sid = x.w(self + OID), so = x.w(other + OID);
            const bool enemy = (so & 0xe000) == 0x8000;
            const unsigned otype = (so >> 8) & 0x1f;
            if ((sid & 0xff00) == 0x4200 && enemy) {
                const int k = proc_slot(x.l(self + OPLINK));
                if (k < 0) return;
                const Variant v = Variant(s.proc_var[k]);
                const auto kind = VariantChild(s.proc_kind[k]);
                const bool boss = boss_type(otype);
                const int p = s.proc_owner[k] - 1;
                if (v == V::HotLead && bself == KILL_BULL && !boss) {
                    if (s.proc_last[k] != ko + 1) { s.proc_last[k] = u16(ko + 1); ++s.proc_aux[k]; }
                    if (s.proc_aux[k] <= 2) bself = 0;   // pierce two enemies
                }
                if (v == V::RicochetRounds && (s.proc_flags[k] & 1) && boss && p >= 0 && (++s.player[p].damage_count & 1)) bother = 0;
                if (v == V::ShrapnelSpread && kind == VariantChild::None && !(s.proc_flags[k] & 2) && bself == KILL_BULL && !boss) {
                    s.proc_flags[k] |= 2;
                    for (const Rot& r : kShard) {
                        s32 vx = s32(x.l(self + OXVEL)) * 4 / 5, vy = s32(x.l(self + OYVEL)) * 4 / 5;
                        rotate(vx, vy, r);
                        x.queue(VariantSpawnKind::Fragment, V::ShrapnelSpread, p, x.cx(self), x.cy(self), 10, vx, vy);
                    }
                }
                if (kind == VariantChild::Fragment && boss) bother = 0;   // fragments do not hurt bosses
                if (v == V::TwinSeekers && boss && p >= 0) { const unsigned n = ++s.player[p].damage_count % 5; if (n == 1 || n == 3) bother = 0; }
                return;
            }
            // Reflector: an enemy shot that touches the ring flies back as a player shot.
            if ((sid & 0xe000) == 0x8000 && ((sid >> 8) & 0x1f) == 0x15 && bself == KILL_CBALL && (so & 0xff00) == 0x4100) {
                const int rk = proc_slot(x.l(other + OPLINK));
                if (rk < 0 || s.proc_var[rk] != vid(V::Reflector)) return;
                bself = 0;
                x.wl(self + OXVEL, 0u - x.l(self + OXVEL)); x.wl(self + OYVEL, 0u - x.l(self + OYVEL));
                x.ww(self + OID, 0x4201);
                if (const int ck = proc_slot(x.l(self + OPLINK)); ck >= 0) {
                    s.proc_var[ck] = vid(V::Reflector); s.proc_owner[ck] = s.proc_owner[rk];
                    shot_aim[std::size_t(ck)] = 0;   // drawn as an ordinary retail rifle shot
                }
                return;
            }
            // Bulldozer boots: grunts and hulks the player runs into are flattened.
            if ((sid == 0x4101 || sid == 0x4102) && bself == KILL_PLYR && enemy && (otype == 0x03 || otype == 0x06)) {
                const int p = sid - 0x4101;
                if (s.player[p].shoe != vid(V::BulldozerBoots)) return;
                const u32 tp = x.torso_proc(p);
                const u32 legs = proc_slot(tp) >= 0 ? x.l(tp + LEG_PRC) : 0;
                if (proc_slot(legs) < 0 || !x.w(legs + SHOECNT)) return;
                bself = 0; bother = 0;
                const u16 mark = u16(frame_number | 1);
                if (!s.obj_mark[ko] || u16(mark - s.obj_mark[ko]) > 12) { s.obj_mark[ko] = mark; x.queue(VariantSpawnKind::Blast, V::BulldozerBoots, p, x.cx(other), x.cy(other)); }
            }
        };
        apply(c.a[1], c.a[2], b1, b2);
        apply(c.a[2], c.a[1], b2, b1);
        c.b[1] = b1; c.b[2] = b2;
        return 0;
    }
    case H_RBTEXP: {   // ROBOTS RBT_EXP: A10 = killer process, A9 = killer OID
        const int k = proc_slot(c.a[10]);
        if (k >= 0 && s.proc_var[k] == vid(V::HotLead)) c.a[9] = 0;   // explode (burn) instead of shattering
        return 0;
    }
    case H_KILLPLYR: {   // KILL_PLYR "YES, KILL THIS PLYR": A7 = player process, A8 = player object
        const int p = x.player_of_proc(c.a[7]);
        if (p < 0) return 0;
        if (s.player[p].grace) return R_GAMOK;   // the free smart bomb is on its way
        if (!s.player[p].last_stand) return 0;
        s.player[p].last_stand = 0;
        s.player[p].grace = 45;
        if (x.torso_obj(p)) x.queue(VariantSpawnKind::SmartBomb, V::LastStand, p, 0, 0);
        return R_GAMOK;
    }
    case H_RINGS: {   // RINGS entry: A10 = time, A9 = player
        const int k = proc_slot(c.a[13]);
        const int p = int(c.a[9] & 0xff) - 1;
        if (k < 0 || p < 0 || p > 1 || !s.proc_var[k]) return 0;
        if (s.proc_var[k] == vid(V::Reflector)) c.a[10] = c.a[10] * 3 / 5;
        s.player[p].ring_timer = u16(std::min<u32>(c.a[10], 0xffff));
        return 0;
    }
    case H_SHOECNT: {   // SHOEON: A0 = legs process, A1 = shoe time
        for (int p = 0; p < 2; ++p) {
            const u32 tp = x.torso_proc(p);
            if (proc_slot(tp) < 0 || x.l(tp + LEG_PRC) != c.a[0]) continue;
            if (s.player[p].shoe == vid(V::Afterburner)) c.a[1] = c.a[1] * 3 / 5;
            else if (s.player[p].shoe == vid(V::BulldozerBoots)) c.a[1] = 420;
        }
        return 0;
    }
    case H_STUFFVEL: {   // STUFF_VEL: A13 = legs process, A0/A1 = velocity
        const int p = x.player_of_proc(c.a[13]);
        if (p < 0 || s.player[p].shoe != vid(V::Afterburner) || !x.w(c.a[13] + SHOECNT)) return 0;
        c.a[0] = u32(s32(c.a[0]) * 5 / 4); c.a[1] = u32(s32(c.a[1]) * 5 / 4);
        return 0;
    }
    // ------------------------------------------------------------ blades
    case H_SAWS: {   // DOSAWS: A8 = player number; A11 = saw count (set by the retail MOVK)
        const int p = int(c.a[8] & 0xff) - 1;
        if (p < 0 || p > 1) return 0;
        if (s.player[p].blade == vid(V::BuzzStorm)) c.a[11] = 6;
        else if (s.player[p].blade == vid(V::ThrownBlades)) c.a[11] = 3;
        return 0;
    }
    case H_ORBIT1: case H_ORBIT2: {   // after the retail orbit multiplier load (A0)
        const int k = proc_slot(c.a[13]);
        if (k >= 0 && s.proc_var[k] == vid(V::BuzzStorm)) c.a[0] += 3;
        return 0;
    }
    case H_CHAINHIT: {   // CHAINHIT: A1 = hits, before CMPI MAXHITS
        const int k = proc_slot(c.a[13]);
        if (k < 0) return 0;
        if (s.proc_var[k] == vid(V::BuzzStorm)) c.a[1] *= 2;                         // half duration
        else if (s.proc_var[k] == vid(V::ThrownBlades)) c.a[1] = c.a[1] >= 10 ? 5u : 0u;   // breaks after 10 kills
        return 0;
    }
    case H_ENCIRCLE: {   // after ENCIRCLE stored the saw position: thrown saws fly out and back
        const int k = proc_slot(c.a[13]);
        const u32 o = c.a[8];
        if (k < 0 || s.proc_kind[k] != u8(VariantChild::Thrown) || !(s.proc_flags[k] & 4) || obj_slot(o) < 0) return 0;
        const int p = s.proc_owner[k] - 1;
        const u32 t = p >= 0 ? x.torso_obj(p) : 0;
        if (!t) { s.proc_flags[k] &= ~4; return 0; }
        const s32 bx = s32(x.l(t + OXVAL)), by = s32(x.l(t + OYVAL));
        if (s.proc_aux[k] == 0) { s.proc_x[k] = s32(x.l(o + OXVAL)) - bx; s.proc_y[k] = s32(x.l(o + OYVAL)) - by; }
        const s32 a = ++s.proc_aux[k];
        constexpr s32 kFrames = 40;
        const s32 dist = a >= kFrames ? 0 : 120 * 4 * a * (kFrames - a) / (kFrames * kFrames);   // parabola, out and back
        x.wl(o + OXVAL, u32(bx + s.proc_x[k] + ((s.proc_vx[k] * dist / 100) << 16)));
        x.wl(o + OYVAL, u32(by + s.proc_y[k] + ((s.proc_vy[k] * dist / 100) << 16)));
        if (a >= kFrames) { s.proc_flags[k] &= ~4; s.proc_aux[k] = 0; }
        return 0;
    }
    case H_HELPER: {   // POW2 ENCIRCLE end: the Clone mirrors the player across the arena
        const int k = proc_slot(c.a[13]);
        if (k < 0 || s.proc_var[k] != vid(V::Clone) || obj_slot(c.a[8]) < 0) return 0;
        const int p = s.proc_owner[k] - 1;
        const u32 t = p >= 0 ? x.torso_obj(p) : 0;
        if (!t) return 0;
        const s32 left = enhanced_scene(*this) ? 0x24 : 0x20, right = enhanced_scene(*this) ? 0x200 : 0x173;
        const s32 cxw = s16(rlong(*this, kWorldTL)) + (left + right) / 2;
        const s32 mx = 2 * cxw - x.cx(t) - s32(x.w(c.a[8] + OSIZEX)) / 2;
        x.wl(c.a[8] + OXVAL, u32(mx) << 16);
        x.wl(c.a[8] + OYVAL, x.l(t + OYVAL));
        return 0;
    }
    case H_SBVICTIM: {   // SBOMB victim: A8 = victim, A9 = P1DATA/P2DATA of the bomber
        const int p = c.a[9] == kP1Data ? 0 : 1;
        auto& pl = s.player[p];
        if (!pl.gold_timer || pl.gold_drops >= 8 || obj_slot(c.a[8]) < 0) return 0;
        ++pl.gold_drops;
        x.queue(VariantSpawnKind::Cash, V::GoldRushBomb, p, x.cx(c.a[8]), x.cy(c.a[8]));
        return 0;
    }
    default: return 0;
    }
}

} // namespace stv::gsp
