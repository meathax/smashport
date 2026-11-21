// Development-only scenario bot for the native lockstep harness (PLAN P5).
// C++ port of tools/scenarios/bot.lua, run inside stv_l8_lockstep (--bot FILE)
// because the native build is lockstep-identical to MAME and far faster. It plays
// player 1 with legal inputs only (coin, start, 8-way move and fire), deciding
// from RAM at each frame boundary, and writes every input change in the --inputs
// format. MAME then replays the file (record.lua STV_LS_INPUTS) as the oracle.
//
// Additions over bot.lua: boss rooms (a multi-part enemy type) are fought from a
// ring around the boss centroid while strafing, and player mines/obstacles
// (class 0, type 0x17) are avoided.
//
// L8 RAM (bit addresses): GAMSTATE 0x010865A0, WAVE 0x010EB630, OBJLST 0x01000040;
// object: OLINK +0x00, OXPOS +0x70, OYPOS +0x90, OID +0x150 (class 0x8000 enemy,
// 0x4000 player side; type in bits 8..12: 0x01 player, 0x0C pickup, 0x17 mine).
#pragma once

#include "stv/gsp/machine.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>

namespace stv::bot {

struct Event { int port; unsigned mask; int value; };

class Bot {
public:
    explicit Bot(int seed = 1, int player = 1)
        : seed_(seed), player_(player), legacy_(std::getenv("STV_BOT_LEGACY") != nullptr), debug_(std::getenv("STV_BOT_DEBUG") != nullptr) {}

    std::vector<Event> step(uint32_t f, stv::gsp::Machine& m) {
        std::vector<Event> out;
        const int gs = m.rw(kGamState);
        if (gs != last_state_) frame_in_state_ = 0, last_state_ = gs; else ++frame_in_state_;
        credit_phase_ = (credit_phase_ + 1) % 240;
        int coin = credit_phase_ < 6, start = credit_phase_ >= 26 && credit_phase_ < 36;
        if (gs == kInGame && frame_in_state_ < 600) coin = start = 0;
        want(out, 1, player_ == 1 ? 0x0001 : 0x0002, coin);
        want(out, 1, player_ == 1 ? 0x0004 : 0x0020, start);

        int mv = 0, fire = 0;
        if (gs == kInGame) play(f, m, mv, fire);
        else finish_pressure(f);
        if (log_every_ && f % log_every_ == 0)
            std::printf("BOT%d f=%u state=%02x wave=%d quiet=%d mv=%x fire=%x p=%d,%d boss=%d\n", player_, f, gs, m.rw(kWave),
                        quiet_, mv, fire, dbg_px_, dbg_py_, boss_seen_);
        const unsigned sh = player_ == 1 ? 0 : 8;   // P2 stick and fire are IN0 bits 8..15
        for (int b = 0; b < 4; ++b) {
            want(out, 0, 1u << (b + sh), (mv >> b) & 1);
            want(out, 0, 1u << (b + 4 + sh), (fire >> b) & 1);
        }
        return out;
    }
    // An unfinished visit is explicit and never promoted to a cleared-room sample.
    void finish_pressure(uint32_t f) {
        flush_pressure(f, "WAVEPART");
        stat_lives_ = stat_wave_ = -1;
    }
    int log_every_ = 3000;
    // Development route coverage can hold both players to the same legal exit.
    // The default retains the existing timed retry policy and pressure inputs.
    int route_slot_ = -1;
    // taking over from a scripted prefix: re-send every button state on the next step
    void resync() {
        for (auto& [k, v] : held_) v = -1;
        const unsigned sh = player_ == 1 ? 0 : 8;
        for (int b = 0; b < 8; ++b) held_[int(1u << (b + sh))] = -1;
        if (player_ == 1) held_[0x10001] = held_[0x10004] = -1; else held_[0x10002] = held_[0x10020] = -1;
    }

private:
    static constexpr uint32_t kGamState = 0x010865A0u, kObjLst = 0x01000040u, kWave = 0x010EB630u;
    static constexpr uint32_t kCircuit = 0x010EB610u, kWorldTl = 0x01000280u;   // CIRCUIT (0-based), WORLDTL [Y,X]
    static constexpr uint32_t kCirc2Tbl = 0xFFECCE80u;   // ENDWAVE CIRC2TBL: per-wave exit boxes, 384 bits per wave
    static constexpr uint32_t kP1Lives = 0x01086360u, kP2Lives = 0x01086520u;   // P1DATA/P2DATA + PLIVES (0x140)
    static constexpr int kInGame = 0x10;
    struct Obj { int x, y; unsigned oid; };
    struct Probe { int i = 1, still = 0, lx = 0, ly = 0, lox = 0, hix = 0, loy = 0, hiy = 0, t0 = 0; };

    static int s16(unsigned v) { return int(int16_t(uint16_t(v))); }
    static int fdiv(int a, int b) { int q = a / b; if ((a % b != 0) && ((a < 0) != (b < 0))) --q; return q; }
    static int fmod_(int a, int b) { return a - fdiv(a, b) * b; }

    void want(std::vector<Event>& out, int port, unsigned mask, int v) {
        int& h = held_[port * 0x10000 + int(mask)];
        if (h != v) { h = v; out.push_back({port, mask, v}); }
    }
    static int dir_bits(int dx, int dy, int dead) {
        int b = 0;
        const int ax = std::abs(dx), ay = std::abs(dy);
        if (dy < -dead && ay * 2 >= ax) b |= 1;
        if (dy > dead && ay * 2 >= ax) b |= 2;
        if (dx < -dead && ax * 2 >= ay) b |= 4;
        if (dx > dead && ax * 2 >= ay) b |= 8;
        return b;
    }
    std::vector<Obj> objects(stv::gsp::Machine& m) {
        std::vector<Obj> list;
        auto rl = [&](uint32_t a) { return uint32_t(m.rw(a)) | (uint32_t(m.rw(a + 16)) << 16); };
        uint32_t a = rl(kObjLst);
        for (int n = 0; a && n < 600; ++n, a = rl(a)) list.push_back({s16(m.rw(a + 0x70)), s16(m.rw(a + 0x90)), m.rw(a + 0x150)});
        return list;
    }

    void flush_pressure(uint32_t f, const char* tag) {
        if (stat_frames_ > 0)
            std::printf("%s%d f=%u circuit=%u wave=%d frames=%u deaths=%u avg_enemies=%.1f peak=%u\n", tag, player_, f,
                        stat_circuit_, stat_wave_, stat_frames_, stat_deaths_, double(stat_enemy_sum_) / stat_frames_, stat_peak_);
        stat_frames_ = stat_deaths_ = stat_peak_ = 0;
        stat_enemy_sum_ = 0;
    }

    void play(uint32_t f, stv::gsp::Machine& m, int& mv, int& fire) {
        const std::vector<Obj> objs = objects(m);
        bool have = false;
        int px = 0, py = 0;
        for (const Obj& o : objs)
            if ((o.oid & 0xC000) == 0x4000 && ((o.oid >> 8) & 0x1F) == 0x01 &&
                (legacy_ || (int(o.oid & 0xFF) == player_ && o.x != 0))) {   // own player; skip parked copies at x 0
                px = o.x, py = o.y, have = true;
                break;
            }
        const int wave = m.rw(kWave);
        // Observed life-counter decreases within a (circuit,wave) visit.
        // A decrease sampled on a boundary has ambiguous room ownership: retain
        // it separately rather than charging either room's pressure measurement.
        const unsigned circuit = m.rw(kCircuit);
        const int lives = m.rw(player_ == 1 ? kP1Lives : kP2Lives);
        const bool boundary = stat_wave_ >= 0 && (wave != stat_wave_ || circuit != stat_circuit_);
        if (boundary) {
            flush_pressure(f, "WAVESTAT");
            if (stat_lives_ >= 0 && lives < stat_lives_)
                std::printf("WAVEBOUND%d f=%u from_circuit=%u from_wave=%d to_circuit=%u to_wave=%d life_decrease=%d\n",
                            player_, f, stat_circuit_, stat_wave_, circuit, wave, stat_lives_ - lives);
        } else if (stat_lives_ >= 0 && lives < stat_lives_) {
            stat_deaths_ += unsigned(stat_lives_ - lives);
        }
        stat_lives_ = lives;
        stat_circuit_ = circuit;
        stat_wave_ = wave;
        if (wave != last_wave_) {
            if (last_wave_ >= 0) need_center_ = true;
            last_wave_ = wave;
            walls_.clear();
        }
        if (have && need_center_) {
            need_center_ = false;
            const int ex[4][2] = {{0, -half_h_}, {half_w_, 0}, {0, half_h_}, {-half_w_, 0}};   // entry door is opposite
            cx_ = px + ex[last_door_ - 1][0], cy_ = py + ex[last_door_ - 1][1];
            probe_ = nullptr_probe();
            quiet_ = 0;
        }
        if (!have) return;
        // Use movement feedback before a safety override: fleeing a mine directly
        // into a wall must not suppress the pathfinder forever.
        blocked_frames_ = (px == last_px_ && py == last_py_ && last_mv_)
            ? std::min(blocked_frames_ + 1, 120) : 0;
        dbg_px_ = px, dbg_py_ = py;

        // enemies by type: a type with many parts is a boss
        std::map<int, std::pair<int, std::pair<long, long>>> types;
        const Obj* best = nullptr;
        long long bd = 1LL << 40;
        int threat = 0;
        const Obj* junk = nullptr;
        long long jd = 1LL << 40;
        bool pick = false;
        int pkx = 0, pky = 0;
        long long pd = 1LL << 40;
        const Obj* mine_near = nullptr;
        long long md = 1LL << 40;
        for (const Obj& o : objs) {
            const long long dx = o.x - px, dy = o.y - py, d = dx * dx + dy * dy;
            const int type = (o.oid >> 8) & 0x1F;
            if (o.oid & 0x8000) {
                // parked objects far outside the world (e.g. 8A30 at -9277,-14589 after a
                // wave ends) are not opponents
                if (!legacy_ && (std::abs(o.x - px) > 400 || std::abs(o.y - py) > 400)) continue;
                if (d < bd) best = &o, bd = d;
                if (d < 60 * 60) ++threat;
                auto& t = types[type];
                ++t.first, t.second.first += o.x, t.second.second += o.y;
            } else if ((o.oid & 0xC000) == 0 && type == 0x17) {
                if (d < jd) junk = &o, jd = d;
                if (d < md) mine_near = &o, md = d;
            } else if (type == 0x0C && (!pick || d < pd)) {
                pick = true, pkx = o.x, pky = o.y, pd = d;
            }
        }
        // a boss is one enemy type with many parts packed together (grunt waves spread out)
        int boss_n = 0;
        long bx = 0, by = 0;
        for (const auto& [type, t] : types) {
            if (t.first < 6 || t.first <= boss_n) continue;
            const long cx = t.second.first / t.first, cy = t.second.second / t.first;
            bool packed = true;
            for (const Obj& o : objs)
                if ((o.oid & 0x8000) && int((o.oid >> 8) & 0x1F) == type && (std::abs(o.x - cx) > 70 || std::abs(o.y - cy) > 70))
                    packed = false;
            if (packed) boss_n = t.first, bx = cx, by = cy;
        }
        {
            unsigned near = 0;
            for (const auto& [type, t] : types) near += unsigned(t.first);
            ++stat_frames_, stat_enemy_sum_ += near, stat_peak_ = std::max(stat_peak_, near);
        }
        if (legacy_) boss_n = 0, mine_near = nullptr;
        if (boss_n) boss_seen_ = 1;

        if (best) {
            quiet_ = 0;
            if (boss_n) {
                fire = dir_bits(int(bx - px), int(by - py), 3);
                const long dx = bx - px, dy = by - py, d2 = dx * dx + dy * dy;
                // strafe around the boss; reverse on a timer or when stuck against something
                if (px == last_px_ && py == last_py_) { if (++stuck_ > 20) strafe_ = -strafe_, stuck_ = 0; } else stuck_ = 0;
                if (f % 420 == 0) strafe_ = -strafe_;
                if (d2 < 90L * 90) mv = dir_bits(int(-dx), int(-dy), 0);
                else if (d2 > 140L * 140) mv = dir_bits(int(dx), int(dy), 0);
                else mv = dir_bits(int(-dy * strafe_), int(dx * strafe_), 0);
                // projectiles and other enemies close by: back away from the nearest
                if (bd < 45 * 45 && types[(best->oid >> 8) & 0x1F].first < 6) mv = dir_bits(px - best->x, py - best->y, 0);
            } else {
                fire = dir_bits(best->x - px, best->y - py, 3);
                if (bd < 70 * 70) mv = dir_bits(px - best->x, py - best->y, 0);                  // back away
                else if (pick && pd < 120 * 120 && threat == 0) mv = dir_bits(pkx - px, pky - py, 4);   // nearby prize
                else mv = steer(px, py, cx_, cy_, 24, objs);                                     // keep central
            }
            if (mv == 0) mv = (fdiv(int(f), 30) % 2 == 0) ? 4 : 8;
            if (mine_near && md < 12 * 12) {
                const int away=dir_bits(px - mine_near->x, py - mine_near->y, 0);
                const int selected=blocked_frames_<12?away:mv;
                if(debug_&&f%600==0)std::printf("BOTMINE%d f=%u p=%d,%d mine=%d,%d distance2=%lld planned=%x away=%x selected=%x blocked=%d\n",player_,f,px,py,mine_near->x,mine_near->y,md,mv,away,selected,blocked_frames_);
                mv=selected;
            }
            // A retreat can point straight into a wall while an enemy stays
            // nearby. Use movement feedback to return to the floor.
            if (!legacy_ && blocked_frames_ >= 12)
                mv = steer(px, py, cx_, cy_, 24, objs);
        } else {
            ++quiet_;
            if (junk) fire = dir_bits(junk->x - px, junk->y - py, 3);
            if (quiet_ == 1 || quiet_ % 3000 == 0) probe_ = Probe{1, 0, px, py};
            if (probe_.i <= 4) {
                const int dirs[4] = {4, 8, 1, 2};
                mv = dirs[probe_.i - 1];
                if (px == probe_.lx && py == probe_.ly) ++probe_.still; else probe_.still = 0;
                probe_.lx = px, probe_.ly = py;
                if (probe_.still >= 12 || quiet_ > probe_.i * 400) {
                    if (probe_.i == 1) probe_.lox = px; else if (probe_.i == 2) probe_.hix = px;
                    else if (probe_.i == 3) probe_.loy = py; else probe_.hiy = py;
                    ++probe_.i, probe_.still = 0;
                    if (probe_.i == 5) {
                        cx_ = fdiv(probe_.lox + probe_.hix, 2), cy_ = fdiv(probe_.loy + probe_.hiy, 2);
                        half_w_ = fdiv(probe_.hix - probe_.lox, 2), half_h_ = fdiv(probe_.hiy - probe_.loy, 2);
                        if (half_w_ < 90) cx_ = probe_.lox + 109, half_w_ = 109;
                        if (half_h_ < 60) cy_ = probe_.loy + 80, half_h_ = 80;
                        probe_.t0 = quiet_;
                    }
                }
            } else {
                const int k = fmod_(fdiv(quiet_ - probe_.t0, 360) + seed_, 4) + 1;
                last_door_ = k;
                const int ofs[4][2] = {{0, -177}, {206, 0}, {0, 183}, {-214, 0}};
                const int tx = cx_ + ofs[k - 1][0] * 2, ty = cy_ + ofs[k - 1][1] * 2;
                if (k == 1 || k == 3) mv = std::abs(px - cx_) > 3 ? dir_bits(cx_ - px, 0, 1) : dir_bits(0, ty - py, 0);
                else mv = std::abs(py - cy_) > 3 ? dir_bits(0, cy_ - py, 1) : dir_bits(tx - px, 0, 0);
            }
            // the game's own exit ("magic") boxes for this circuit and wave: walk into one,
            // trying the next every 1,200 frames and sidestepping when stuck
            if (!legacy_ && quiet_ > 60) {
                int boxes[3][4], nb = 0;
                const int circuit = m.rw(kCircuit), wv = m.rw(kWave);
                if (circuit >= 0 && circuit < 3 && wv >= 1 && wv <= (circuit == 0 ? 11 : 22)) {
                    auto rl = [&](uint32_t a) { return uint32_t(m.rw(a)) | (uint32_t(m.rw(a + 16)) << 16); };
                    // Native enhancement tables have independent geometry. Their
                    // nested pointers already refer to the native data owner.
                    const uint32_t root = m.enhanced ? stv::gsp::enhanced_data_address(kCirc2Tbl) : kCirc2Tbl;
                    const uint32_t base = rl(root + uint32_t(circuit) * 32) + uint32_t(wv - 1) * 384;
                    for (int k = 0; k < 3; ++k) {
                        const uint32_t ul = rl(base + uint32_t(k) * 32), lr = rl(base + 96 + uint32_t(k) * 32);
                        if (ul == 0 || lr == 0) continue;
                        boxes[nb][0] = s16(ul & 0xffff), boxes[nb][1] = s16(ul >> 16), boxes[nb][2] = s16(lr & 0xffff),
                        boxes[nb][3] = s16(lr >> 16), ++nb;
                    }
                }
                if (nb) {
                    const int* b = boxes[fmod_(route_slot_ >= 0 ? route_slot_ : fdiv(quiet_, 1200) + seed_, nb)];
                    const uint32_t wtl = uint32_t(m.rw(kWorldTl)) | (uint32_t(m.rw(kWorldTl + 16)) << 16);
                    const int tx = s16(wtl & 0xffff) + (b[0] + b[2]) / 2, ty = s16(wtl >> 16) + (b[1] + b[3]) / 2;
                    const int sx = (b[0] + b[2]) / 2, sy = (b[1] + b[3]) / 2;   // screen-relative: which wall
                    if (debug_ && f % 600 == 0)
                        std::printf("BOTDBG%d f=%u box %d..%d,%d..%d target %d,%d player %d,%d walls %zu stuck %d\n", player_, f,
                                    b[0], b[2], b[1], b[3], tx, ty, px, py, walls_.size(), stuck_);
                    last_door_ = sy < 70 ? 1 : sy > 190 ? 3 : sx < 120 ? 4 : 2;
                    if (px == last_px_ && py == last_py_ && last_mv_) ++stuck_; else if (stuck_ < 40) stuck_ = 0;
                    if (stuck_ == 40) {
                        // A full remembered map can fence off an open co-op
                        // exit indefinitely. Forget it on the next blocked
                        // attempt; current mines remain in the object list.
                        if (walls_.size() >= 64) {
                            if(debug_)std::printf("BOTWALLRESET%d f=%u wave=%d count=%zu p=%d,%d\n",player_,f,wave,walls_.size(),px,py);
                            walls_.clear();
                        }
                        // blocked by scenery the object list does not show (an arena centre
                        // piece, a wall): remember the spot ahead so the path goes round it
                        const int ax = (last_mv_ & 8 ? 12 : 0) - (last_mv_ & 4 ? 12 : 0);
                        const int ay = (last_mv_ & 2 ? 12 : 0) - (last_mv_ & 1 ? 12 : 0);
                        walls_.push_back({px + ax, py + ay});
                        stuck_ = 0;
                    }
                    if (stuck_ >= 40) {   // blocked: slide along the wall for a while
                        mv = (last_door_ == 1 || last_door_ == 3) ? (seed_ & 1 ? 4 : 8) : (seed_ & 1 ? 1 : 2);
                        if (++stuck_ > 100) stuck_ = 0;
                    } else {
                        mv = steer(px, py, tx, ty, 1, objs);
                    }
                }
            }
            if (pick && pd < 150 * 150 && quiet_ < 240) mv = dir_bits(pkx - px, pky - py, 4);
            if (mine_near && md < 12 * 12) {
                const int away=dir_bits(px - mine_near->x, py - mine_near->y, 0);
                const int selected=blocked_frames_<12?away:mv;
                if(debug_&&f%600==0)std::printf("BOTMINE%d f=%u p=%d,%d mine=%d,%d distance2=%lld planned=%x away=%x selected=%x blocked=%d\n",player_,f,px,py,mine_near->x,mine_near->y,md,mv,away,selected,blocked_frames_);
                mv=selected;
            }
        }
        last_px_ = px, last_py_ = py, last_mv_ = mv;
    }
    // head for (tx,ty) but sidestep a mine (class 0, type 0x17) lying ahead in the path
    // Head for (tx,ty) around mines (class 0, type 0x17): A* on a 6 px grid over the
    // box spanning player and target plus a margin, mines blocking 15 px around them.
    int steer(int px, int py, int tx, int ty, int dead, const std::vector<Obj>& objs) const {
        const int dx = tx - px, dy = ty - py;
        if (legacy_ || (std::abs(dx) <= dead && std::abs(dy) <= dead)) return dir_bits(dx, dy, dead);
        std::vector<std::pair<int, int>> mines;
        for (const Obj& o : objs)
            if ((o.oid & 0xC000) == 0 && ((o.oid >> 8) & 0x1F) == 0x17) mines.push_back({o.x, o.y});
        mines.insert(mines.end(), walls_.begin(), walls_.end());
        if (mines.empty()) return dir_bits(dx, dy, dead);
        constexpr int C = 6, M = 60;
        const int x0 = std::min(px, tx) - M, y0 = std::min(py, ty) - M;
        const int W = (std::abs(dx) + 2 * M) / C + 1, H = (std::abs(dy) + 2 * M) / C + 1;
        auto cell = [&](int x, int y) { return ((y - y0) / C) * W + (x - x0) / C; };
        std::vector<char> blocked(std::size_t(W) * H, 0);
        for (int cy = 0; cy < H; ++cy)
            for (int cx = 0; cx < W; ++cx) {
                const int wx = x0 + cx * C + C / 2, wy = y0 + cy * C + C / 2;
                for (const auto& [mx, my] : mines)
                    if (std::abs(wx - mx) < 15 && std::abs(wy - my) < 15) { blocked[std::size_t(cy) * W + cx] = 1; break; }
            }
        const int s = cell(px, py), g = cell(tx, ty);
        if (s == g) return dir_bits(dx, dy, dead);
        blocked[std::size_t(s)] = 0;
        blocked[std::size_t(g)] = 0;
        std::vector<int> from(std::size_t(W) * H, -1), cost(std::size_t(W) * H, 1 << 30);
        std::vector<std::pair<int, int>> open{{0, s}};   // (priority, cell), small heap
        cost[std::size_t(s)] = 0;
        auto h = [&](int c) { return (std::abs(c % W - g % W) + std::abs(c / W - g / W)) * 10; };
        while (!open.empty()) {
            std::pop_heap(open.begin(), open.end(), std::greater<>());
            const int c = open.back().second;
            open.pop_back();
            if (c == g) break;
            for (int ny = -1; ny <= 1; ++ny)
                for (int nx = -1; nx <= 1; ++nx) {
                    if (!nx && !ny) continue;
                    const int cx = c % W + nx, cy = c / W + ny;
                    if (cx < 0 || cy < 0 || cx >= W || cy >= H) continue;
                    const int n = cy * W + cx;
                    if (blocked[std::size_t(n)]) continue;
                    const int nc = cost[std::size_t(c)] + (nx && ny ? 14 : 10);
                    if (nc < cost[std::size_t(n)]) {
                        cost[std::size_t(n)] = nc, from[std::size_t(n)] = c;
                        open.push_back({nc + h(n), n});
                        std::push_heap(open.begin(), open.end(), std::greater<>());
                    }
                }
        }
        if (from[std::size_t(g)] < 0) return dir_bits(dx, dy, dead);   // no way round: go straight
        // walk towards the path cell three steps ahead of the player
        std::vector<int> path;
        for (int c = g; c != s && c >= 0; c = from[std::size_t(c)]) path.push_back(c);
        const int wp = path[path.size() >= 3 ? path.size() - 3 : 0];
        const int wx = x0 + (wp % W) * C + C / 2, wy = y0 + (wp / W) * C + C / 2;
        return dir_bits(wx - px, wy - py, 1);
    }
    static Probe nullptr_probe() { Probe p; p.i = 99; return p; }

    int blocked_frames_ = 0;
    int seed_;
    int player_;    // 1 or 2
    bool legacy_;   // STV_BOT_LEGACY: exactly bot.lua (no boss mode or mine avoidance)
    bool debug_;    // STV_BOT_DEBUG: exit-seeking trace every 600 frames
    std::map<int, int> held_;
    int last_state_ = -1, frame_in_state_ = 0, credit_phase_ = 0;
    int last_wave_ = -1, last_door_ = 2;
    bool need_center_ = false;
    int cx_ = 614, cy_ = 457, half_w_ = 109, half_h_ = 80;
    int quiet_ = 0;
    Probe probe_ = nullptr_probe();
    int dbg_px_ = -1, dbg_py_ = -1, boss_seen_ = 0;
    int last_px_ = 0, last_py_ = 0, stuck_ = 0, strafe_ = 1, last_mv_ = 0;
    std::vector<std::pair<int, int>> walls_;   // learned blocked spots this wave
    int stat_lives_ = -1, stat_wave_ = -1;
    unsigned stat_frames_ = 0, stat_deaths_ = 0, stat_peak_ = 0, stat_circuit_ = 0;
    unsigned long long stat_enemy_sum_ = 0;
};

}  // namespace stv::bot
