// Online co-op rollback (PLAN P10, D9). See include/stv/net/rollback.hpp.
#include "stv/net/rollback.hpp"

#include "stv/gsp/ops.hpp"

#include <algorithm>
#include <stdexcept>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace stv::net {

using stv::gsp::Machine;

void apply_inputs(Machine& m, PlayerInput p1, PlayerInput p2) {
    const u16 in0 = u16((p1 & 0xff) | ((p2 & 0xff) << 8));
    u16 in1 = 0;
    if (p1 & 0x100) in1 |= 0x0001;   // coin 1
    if (p2 & 0x100) in1 |= 0x0002;   // coin 2
    if (p1 & 0x200) in1 |= 0x0004;   // start 1
    if (p2 & 0x200) in1 |= 0x0020;   // start 2
    if (m.enhanced) {
        m.move_inputs = {u16(p1>>16),u16(p2>>16)};
        m.aim_inputs = {u16(p1 & 0xfcf0),u16(p2 & 0xfcf0)};
    } else {
        m.move_inputs = {};
        m.aim_inputs = {};
    }
    m.ports[0] = u16(~in0);
    m.ports[1] = u16(~in1);
}

bool load_scripted_inputs(const std::string& path, u32 frames, std::vector<PlayerInput>& p1, std::vector<PlayerInput>& p2) {
    std::ifstream in(path);
    if (!in) return false;
    struct Ev { u32 f; int port; u16 mask; int v; };
    std::vector<Ev> ev;
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream s(line);
        u32 f;
        std::string tag, mask;
        int v;
        if (!(s >> f >> tag >> mask >> v)) continue;
        ev.push_back({f, tag == ":IN0" ? 0 : 1, u16(std::stoul(mask, nullptr, 16)), v});
    }
    p1.clear(), p2.clear();
    u16 in0 = 0, in1 = 0;   // active high
    std::size_t k = 0;
    for (u32 f = 0; f < frames; ++f) {
        for (; k < ev.size() && ev[k].f == f; ++k) {
            u16& p = ev[k].port ? in1 : in0;
            p = ev[k].v ? u16(p | ev[k].mask) : u16(p & ~ev[k].mask);
        }
        p1.push_back(PlayerInput((in0 & 0xff) | (in1 & 0x1 ? 0x100 : 0) | (in1 & 0x4 ? 0x200 : 0)));
        p2.push_back(PlayerInput((in0 >> 8) | (in1 & 0x2 ? 0x100 : 0) | (in1 & 0x20 ? 0x200 : 0)));
    }
    return true;
}

FrameRunner::FrameRunner(Machine& m) : m_(m) {
    m_.on_vblank = [this] {
        apply_inputs(m_, next_p1_, next_p2_);
        at_vblank_ = true;
        m_.run_until_cycle = m_.c.cyc;   // l8_run returns at the next instruction boundary
        m_.recompute_next_event();
    };
    m_.on_sound_write = [this](u32, u16 d) { sound_.push_back({m_.now_ps(), d}); };
}

void FrameRunner::start() {
    m_.reset();
    pc_ = m_.reset_vector();
    frame_ = 0;
}

void FrameRunner::step(PlayerInput p1, PlayerInput p2) {
    next_p1_ = p1, next_p2_ = p2;
    at_vblank_ = false;
    sound_.clear();
    while (!at_vblank_ && !m_.stop) pc_ = stv::gsp::l8_run(m_, pc_, m_.c.cyc + 1000000);
    ++frame_;
}

void FrameRunner::save(std::vector<u8>& out) const { m_.save_state(out, pc_, false); }

bool FrameRunner::load(const std::vector<u8>& in, u32 frame) {
    // Network/rollback snapshots never replace verified local program or graphics.
    if (in.size() < 13 || in[12] != 0 || !m_.load_state(in, pc_)) return false;
    frame_ = frame;
    return true;
}

u32 FrameRunner::ram_hash() const {
    u32 h = 2166136261u;
    for (u16 w : m_.ram) h = (h ^ w) * 16777619u;
    return h;
}

LoopbackLink::LoopbackLink(Conditions c, u32 seed) : c_(c), rng_(seed) {
    a_.link = this, a_.side = 0;
    b_.link = this, b_.side = 1;
}

void LoopbackLink::End::send(const std::vector<u8>& d) {
    LoopbackLink& l = *link;
    if (std::uniform_real_distribution<double>(0, 1)(l.rng_) < l.c_.loss) return;
    if (l.now_ >= l.c_.outage_from && l.now_ < l.c_.outage_to) return;   // link down
    const int jitter = l.c_.jitter_ticks ? std::uniform_int_distribution<int>(-l.c_.jitter_ticks, l.c_.jitter_ticks)(l.rng_) : 0;
    const u64 due = l.now_ + u64(std::max(0, l.c_.latency_ticks + jitter));
    l.q_[1 - side].push_back({due, d});   // jitter can reorder: poll() takes whatever is due
}

std::vector<std::vector<u8>> LoopbackLink::End::poll() {
    std::vector<std::vector<u8>> out;
    auto& q = link->q_[side];
    for (auto it = q.begin(); it != q.end();) {
        if (it->due <= link->now_) {
            out.push_back(std::move(it->data));
            it = q.erase(it);
        } else {
            ++it;
        }
    }
    return out;
}

namespace {
constexpr int kRing = RollbackSession::kWindow + 4;
template <class T> void put(std::vector<u8>& b, T v) { const u8* p = reinterpret_cast<const u8*>(&v); b.insert(b.end(), p, p + sizeof v); }
template <class T> bool get(const std::vector<u8>& b, std::size_t& at, T& v) {
    if (at + sizeof v > b.size()) return false;
    std::memcpy(&v, b.data() + at, sizeof v);
    at += sizeof v;
    return true;
}
}  // namespace

RollbackSession::RollbackSession(FrameRunner& runner, Transport& link, int local_player)
    : runner_(runner), link_(link), local_(local_player), states_(kRing) {}

void RollbackSession::grow(std::size_t n) {
    if (local_in_.size() >= n) return;
    local_in_.resize(n), remote_in_.resize(n), used_remote_.resize(n), remote_known_.resize(n), hashes_.resize(n);
}

PlayerInput RollbackSession::input_for(int player, u32 f) const {
    if (player == local_) return local_in_[f];
    return used_remote_[f];
}

bool RollbackSession::advance(PlayerInput local) {
    return advance_frame(local, false);
}

int RollbackSession::frame_advantage() const {
    if (!peer_lead_known_) return 0;
    const int local_lead = int(runner_.frame()) - int(confirmed_);
    return (local_lead - peer_lead_) / 2;
}

bool RollbackSession::tick(PlayerInput local) {
    // Ahead of the peer: hold one tick so the two clocks converge.
    const bool wait = frame_advantage() >= kSyncLead && pumps_ >= last_sync_wait_ + kSyncSpacing &&
                      !paused_local_ && !paused_remote_ && !resyncing();
    if (wait) last_sync_wait_ = pumps_, ++stats_.time_sync_waits;
    const bool advanced = !wait && advance_frame(local, true);
    if (!advanced) send_state(runner_.frame()); // keep acknowledgements flowing while held
    receive_state();
    return advanced;
}

bool RollbackSession::advance_frame(PlayerInput local, bool publish_input) {
    const u32 f = runner_.frame();
    if (paused_local_ || paused_remote_ || resyncing()) return false;   // paused or resyncing: both hold
    if (f >= confirmed_ + kWindow) {
        ++stats_.stalls;
        return false;
    }
    grow(f + 1);
    local_in_[f] = local;
    // The input is final as soon as it is sampled. Put it on the wire before
    // taking the rollback snapshot, running the CPU, and hashing this frame.
    if (publish_input) send_state(f + 1);
    // predict the remote input: the latest one known
    PlayerInput guess = 0;
    for (u32 k = f + 1; k-- > 0;)
        if (remote_known_[k]) { guess = remote_in_[k]; break; }
    used_remote_[f] = remote_known_[f] ? remote_in_[f] : guess;
    runner_.save(states_[f % kRing]);
    runner_.step(input_for(1, f), input_for(2, f));
    if (after_step) after_step(f);
    hashes_[f] = runner_.ram_hash();
    return true;
}

bool RollbackSession::resimulate_from(u32 g) {
    const u32 end = runner_.frame();
    if (!runner_.load(states_[g % kRing], g)) return false;
    ++stats_.rollbacks;
    for (u32 k = g; k < end; ++k) {
        PlayerInput guess = 0;
        for (u32 j = k + 1; j-- > 0;)
            if (remote_known_[j]) { guess = remote_in_[j]; break; }
        used_remote_[k] = remote_known_[k] ? remote_in_[k] : guess;
        if (k != g) runner_.save(states_[k % kRing]);
        runner_.step(input_for(1, k), input_for(2, k));
        if (after_step) after_step(k);
        hashes_[k] = runner_.ram_hash();
        ++stats_.resimulated;
    }
    return true;
}

void RollbackSession::send_state(u32 input_end) {
    std::vector<u8> d;
    const u32 f = runner_.frame();
    // from the first input the peer lacks (so it catches up after an outage), 64 at most
    const u32 first = std::min(remote_ack_, input_end);
    const u16 n = u16(std::min<u32>(input_end - first, 64));
    put(d, u8('I'));
    put(d, first);
    put(d, n);
    for (u32 k = first; k < first + n; ++k) put(d, local_in_[k]);
    put(d, confirmed_);   // how much of the remote input stream we hold, contiguous
    // the last few confirmed-frame hashes on the exchange grid
    std::vector<u32> hs;
    for (u32 g = confirmed_; g-- > 0 && hs.size() < 4;)
        if (g % kHashEvery == 0) hs.push_back(g);
    put(d, u8(hs.size()));
    for (u32 g : hs) put(d, g), put(d, hashes_[g]);
    put(d, u8(paused_local_ ? 1 : 0));   // coordinated pause
    // resync: holding, this side's frame, epoch (host: current, client: last loaded),
    // client: snapshot being received and bytes held in order
    put(d, u8(resyncing() ? 1 : 0));
    put(d, f);
    put(d, epoch_);
    put(d, rx_epoch_);
    put(d, rx_have_);
    link_.send(d);
}

namespace {
constexpr u32 kChunk = 1024;          // snapshot bytes per datagram
constexpr u32 kChunksPerPump = 32;    // sent per pump, from the send cursor
constexpr u32 kSendWindow = 256 * 1024;   // unacknowledged bytes in flight
}  // namespace

// A hash mismatch on either side starts a resync; the host is authoritative.
void RollbackSession::desync_seen(u32 f) {
    ++stats_.desyncs;
    if (on_desync) on_desync(f, hashes_[f], 0);
    if (local_ == 1) {
        if (!tx_pending_ && !tx_active_) tx_pending_ = true, ++epoch_;
    } else {
        resync_wait_ = true;
    }
}

// Host: once both sides hold and the host has confirmed every frame either side has
// simulated, send the snapshot before confirmed frame B.
void RollbackSession::host_resync_step() {
    if (tx_pending_ && !tx_active_) {
        if (!peer_holding_ || confirmed_ != std::min(runner_.frame(), peer_frame_) || remote_ack_ > confirmed_) return;
        tx_base_ = confirmed_;
        if (tx_base_ == runner_.frame()) runner_.save(tx_blob_);
        else tx_blob_ = states_[tx_base_ % kRing];
        tx_active_ = true, tx_cursor_ = 0, peer_have_ = 0;
    }
    if (!tx_active_) return;
    if (peer_epoch_ == epoch_) {   // the client has loaded it
        tx_active_ = tx_pending_ = false;
        resync_floor_ = tx_base_;
        remote_hashes_.clear();
        ++stats_.resyncs;
        return;
    }
    if (peer_rx_epoch_ == epoch_) peer_have_ = peer_rx_have_;
    else peer_have_ = 0;
    const u32 total = u32(tx_blob_.size()), limit = std::min(total, peer_have_ + kSendWindow);
    if (tx_cursor_ < peer_have_ || tx_cursor_ >= limit) tx_cursor_ = peer_have_;   // go back to the first byte missing
    for (u32 i = 0; i < kChunksPerPump && tx_cursor_ < limit; ++i) {
        const u32 len = std::min(kChunk, total - tx_cursor_);
        std::vector<u8> d;
        put(d, u8('S'));
        put(d, epoch_);
        put(d, tx_base_);
        put(d, total);
        put(d, tx_cursor_);
        put(d, u16(len));
        d.insert(d.end(), tx_blob_.begin() + tx_cursor_, tx_blob_.begin() + tx_cursor_ + len);
        link_.send(d);
        tx_cursor_ += len;
    }
}

// Client: the whole snapshot is here; load it at B and resimulate to the current frame.
void RollbackSession::client_apply_resync() {
    const u32 b = rx_base_;
    states_[b % kRing].swap(rx_blob_);
    if (!resimulate_from(b)) {
        states_[b % kRing].swap(rx_blob_);
        rx_epoch_ = 0; rx_base_ = kNone; rx_have_ = 0;
        rx_blob_.clear(); rx_chunks_.clear();
        return;
    }
    rx_blob_ = states_[b % kRing];
    resync_floor_ = b;
    epoch_ = rx_epoch_;
    resync_wait_ = false;
    ++stats_.resyncs;
    if (on_resync) on_resync(b, rx_blob_);
}

void RollbackSession::pump() {
    send_state(runner_.frame());
    receive_state();
}

void RollbackSession::receive_state() {
    ++pumps_;
    u32 redo = ~0u;
    for (const auto& d : link_.poll()) {
        last_rx_ = pumps_;
        std::size_t at = 0;
        u8 type = 0;
        if (!get(d, at, type)) continue;
        if (type == 'S') {   // snapshot chunk from the host (client side)
            u32 ep = 0, base = 0, total = 0, off = 0;
            u16 len = 0;
            if (local_ == 1 || !get(d, at, ep) || !get(d, at, base) || !get(d, at, total) || !get(d, at, off) ||
                !get(d, at, len) || at + len != d.size() || ep <= epoch_ || ep < rx_epoch_ ||
                !total || total > (64u << 20) || off >= total || off % kChunk ||
                len != std::min(kChunk, total - off))
                continue;
            // The host snapshots the minimum confirmed peer frame. Never skip
            // local simulation or replay inputs older than the retained window.
            if (base > runner_.frame() || runner_.frame() - base > kWindow || base < resync_floor_)
                continue;
            if (ep != rx_epoch_) {
                rx_epoch_ = ep, rx_base_ = base, rx_blob_.assign(total, 0), rx_have_ = 0;
                rx_chunks_.assign((total + kChunk - 1) / kChunk, 0);
            } else if (base != rx_base_ || total != rx_blob_.size()) {
                continue;   // a snapshot's extent and frame cannot change within its epoch
            }
            resync_wait_ = true;
            if (!rx_chunks_[off / kChunk]) {
                std::memcpy(rx_blob_.data() + off, d.data() + at, len);
                rx_chunks_[off / kChunk] = 1;
            }
            // Acknowledge only the contiguous prefix, while retaining later UDP
            // chunks so jitter does not force a complete send-window replay.
            while (rx_have_ < total && rx_chunks_[rx_have_ / kChunk])
                rx_have_ += std::min(kChunk, total - rx_have_);
            continue;
        }
        u32 first = 0, ack = 0;
        u16 n = 0;
        if (type != 'I' || !get(d, at, first) || !get(d, at, n)) continue;
        // A peer stalls kWindow frames past the inputs it holds from this side, so
        // a genuine datagram never names a frame far beyond the local one. A corrupt
        // or hostile first frame must not index or grow the per-frame histories.
        if (u64(first) + n > u64(runner_.frame()) + u64(kWindow) * 4) continue;
        std::vector<PlayerInput> in(n);
        bool ok = true;
        for (auto& v : in) ok = ok && get(d, at, v);
        if (!ok || !get(d, at, ack) || ack > runner_.frame()) continue;
        remote_ack_ = std::max(remote_ack_, ack);
        for (u32 k = 0; k < n; ++k) {
            const u32 g = first + k;
            grow(g + 1);
            if (remote_known_[g]) continue;
            remote_known_[g] = 1, remote_in_[g] = in[k];
            // frames below the resync floor came from the host's state: nothing to redo
            if (g < runner_.frame() && g >= resync_floor_ && used_remote_[g] != in[k]) redo = std::min(redo, g);
        }
        u8 nh = 0;
        std::vector<std::pair<u32, u32>> hs;
        if (get(d, at, nh))
            for (u8 i = 0; i < nh; ++i) {
                u32 g = 0, h = 0;
                if (get(d, at, g) && get(d, at, h)) hs.push_back({g, h});
            }
        u8 pause = 0, hold = 0;
        if (get(d, at, pause)) paused_remote_ = pause != 0;
        u32 pf = 0, ep = 0, rxe = 0, rxh = 0;
        if (get(d, at, hold) && get(d, at, pf) && get(d, at, ep) && get(d, at, rxe) && get(d, at, rxh)) {
            peer_holding_ = hold != 0, peer_frame_ = pf, peer_epoch_ = ep;
            // The peer's lead over this side's inputs it holds (ack).
            peer_lead_ = int(pf) - int(ack), peer_lead_known_ = true;
            if (local_ == 1) {
                peer_rx_epoch_ = rxe, peer_rx_have_ = rxh;
                // the client saw a desync (it holds without a resync under way)
                if (hold && !tx_pending_ && !tx_active_ && ep == epoch_) tx_pending_ = true, ++epoch_;
                // its hashes count only once it runs on the host's latest snapshot
                if (ep != epoch_) hs.clear();
            } else if (hold && ep > epoch_) {
                resync_wait_ = true;   // the host started a resync
            }
        }
        if (!resyncing()) remote_hashes_.insert(remote_hashes_.end(), hs.begin(), hs.end());
    }
    if (redo != ~0u && !resimulate_from(redo))
        throw std::runtime_error("rollback: invalid local snapshot");
    // confirmed: both inputs final for every frame below
    u32 c = confirmed_;
    while (c < runner_.frame() && c < remote_known_.size() && remote_known_[c]) {
        if (on_confirmed) on_confirmed(c, hashes_[c]);
        ++c;
    }
    confirmed_ = c;
    if (local_ == 1) host_resync_step();
    else if (resync_wait_ && rx_epoch_ > epoch_ && rx_have_ == rx_blob_.size()) client_apply_resync();
    if (resyncing()) return;
    // compare exchanged hashes for frames confirmed on both sides
    for (auto it = remote_hashes_.begin(); it != remote_hashes_.end();) {
        if (it->first < resync_floor_) { it = remote_hashes_.erase(it); continue; }
        if (it->first < confirmed_) {
            if (checked_.size() <= it->first) checked_.resize(it->first + 1);
            if (checked_[it->first]) { it = remote_hashes_.erase(it); continue; }
            checked_[it->first] = 1;
            ++stats_.hashes_checked;
            const bool bad = hashes_[it->first] != it->second;
            const u32 g = it->first;
            it = remote_hashes_.erase(it);
            if (bad) {
                desync_seen(g);
                break;
            }
        } else {
            ++it;
        }
    }
}

}  // namespace stv::net
