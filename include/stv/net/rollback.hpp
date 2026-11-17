#pragma once
// Online co-op, PLAN P10 (D9): rollback on D3 snapshots. Only the main machine
// is rolled back: the translated main program does not read the sound board, so
// the sound board is driven from confirmed frames only (their latch writes, at
// their times), which also makes "audio only from confirmed frames" hold.
//
// A frame is the MAME frame_done cycle: the machine runs to the next vblank, the
// frame's input ports are applied there (as record.lua / stv_l8_lockstep do), and
// the machine stops at the next instruction boundary. Both peers apply both
// players' inputs of frame f at vblank f, so their machines stay identical.
#include "stv/gsp/machine.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <random>
#include <string>
#include <vector>

namespace stv::net {

using stv::gsp::u8;
using stv::gsp::u16;
using stv::gsp::u32;
using stv::gsp::u64;

// One player's controls for a frame: stick (bits 0..3 up/down/left/right),
// fire (bits 4..7), coin (bit 8), start (bit 9). Active high.
// Bits 10..15 encode fine aim; bits 16..23 and 24..31 carry signed movement X/Y.
using PlayerInput = u32;

// Port values for a frame from both players (Y-unit IN0/IN1, active low).
void apply_inputs(stv::gsp::Machine& m, PlayerInput p1, PlayerInput p2);

// Per-frame controls of each player from a record.lua-style input file
// ("frame :IN0|:IN1 hexmask 0|1" lines), for frames [0, frames).
bool load_scripted_inputs(const std::string& path, u32 frames, std::vector<PlayerInput>& p1, std::vector<PlayerInput>& p2);

// Steps a Machine one frame at a time (main program only).
class FrameRunner {
public:
    explicit FrameRunner(stv::gsp::Machine& m);
    void start();                                  // from reset
    void begin_session() { frame_ = 0; } // current machine becomes the new connection epoch
    void step(PlayerInput p1, PlayerInput p2);     // run to the next vblank, apply the inputs there, stop
    u32 pc() const { return pc_; }
    u32 frame() const { return frame_; }           // frames stepped so far
    void save(std::vector<u8>& out) const;         // snapshot without the program image
    bool load(const std::vector<u8>& in, u32 frame);   // frame: frames stepped at that snapshot
    u32 ram_hash() const;                          // FNV-1a of main RAM (desync checks)
    // latch writes of the last step (bit address, data) for the sound board
    const std::vector<std::pair<u64, u16>>& sound_writes() const { return sound_; }

private:
    stv::gsp::Machine& m_;
    u32 pc_ = 0, frame_ = 0;
    bool at_vblank_ = false;
    PlayerInput next_p1_ = 0, next_p2_ = 0;
    std::vector<std::pair<u64, u16>> sound_;
};

// Unreliable datagram link. send() may lose, delay or reorder; poll() returns
// the datagrams that have arrived.
struct Transport {
    virtual ~Transport() = default;
    virtual void send(const std::vector<u8>& datagram) = 0;
    virtual std::vector<std::vector<u8>> poll() = 0;
};

// In-process pair of transports with simulated latency, jitter and loss, measured
// in "ticks" (one tick per frame advanced by the test driver).
class LoopbackLink {
public:
    struct Conditions { int latency_ticks = 0; int jitter_ticks = 0; double loss = 0.0; u64 outage_from = 0, outage_to = 0; };
    LoopbackLink(Conditions c, u32 seed);
    Transport& end(int i) { return i ? static_cast<Transport&>(b_) : a_; }
    void tick() { ++now_; }

private:
    struct End : Transport {
        LoopbackLink* link;
        int side;
        void send(const std::vector<u8>& d) override;
        std::vector<std::vector<u8>> poll() override;
    };
    struct InFlight { u64 due; std::vector<u8> data; };
    Conditions c_;
    std::mt19937 rng_;
    u64 now_ = 0;
    std::deque<InFlight> q_[2];   // q_[i]: datagrams on their way to end i
    End a_, b_;
};

// Rollback session for one peer. local_player is 1 or 2.
class RollbackSession {
public:
    struct Stats { u64 rollbacks = 0, resimulated = 0, stalls = 0, desyncs = 0, hashes_checked = 0, resyncs = 0, time_sync_waits = 0; };
    static constexpr int kWindow = 20;     // frames the local side may run ahead of confirmed input
    static constexpr int kHashEvery = 30;  // confirmed-frame RAM hash exchange period
    // Time sync: when this side runs this many frames further ahead of the
    // other than the other runs ahead of it, tick() holds one frame (at most
    // once per kSyncSpacing ticks) so both clocks meet in the middle and
    // rollbacks stay as short as the link allows.
    static constexpr int kSyncLead = 2, kSyncSpacing = 6;
    int frame_advantage() const;            // (local lead - peer lead) / 2, in frames

    RollbackSession(FrameRunner& runner, Transport& link, int local_player);
    // Advance one frame with this frame's local input, if within the window.
    // Returns false (a stall) when the remote side is too far behind.
    bool advance(PlayerInput local);
    bool tick(PlayerInput local); // publish sampled input before simulation and service the link
    void pump();                            // send and receive; roll back on corrected input
    u32 frame() const { return runner_.frame(); }
    u32 confirmed() const { return confirmed_; }   // every frame below this has both inputs final
    // final inputs of a confirmed frame (f < confirmed())
    void confirmed_inputs(u32 f, PlayerInput& p1, PlayerInput& p2) const {
        p1 = local_ == 1 ? local_in_[f] : remote_in_[f];
        p2 = local_ == 2 ? local_in_[f] : remote_in_[f];
    }
    // Checkpoint resync after a desync: player 1 (the host) is authoritative. Both sides
    // hold; once the host has confirmed every frame the client has, it sends its
    // snapshot at its confirmed frame B (1 KB chunks, resent from the client's
    // acknowledged offset) and the client loads it and resimulates from B.
    bool resyncing() const { return resync_wait_ || tx_pending_ || tx_active_; }
    // coordinated pause: while either side is paused, neither advances
    void set_paused(bool p) { paused_local_ = p; }
    bool paused_by_peer() const { return paused_remote_; }
    u64 last_receive_tick() const { return last_rx_; }   // pump() count at the last datagram from the peer
    u64 pumps() const { return pumps_; }
    const Stats& stats() const { return stats_; }
    // called once per frame as it becomes confirmed (frame index, RAM hash after it)
    std::function<void(u32, u32)> on_confirmed;
    std::function<void(u32, u32, u32)> on_desync;   // frame, local hash, remote hash
    std::function<void(u32)> after_step;            // test hook: after frame f is (re)simulated
    // client: the host's state before frame B was loaded (B, snapshot without program)
    std::function<void(u32, const std::vector<u8>&)> on_resync;

private:
    PlayerInput input_for(int player, u32 f) const;
    void grow(std::size_t n);
    bool resimulate_from(u32 f);
    bool advance_frame(PlayerInput local, bool publish_input);
    void send_state(u32 input_end);
    void receive_state();
    void desync_seen(u32 f);
    void host_resync_step();
    void client_apply_resync();

    FrameRunner& runner_;
    Transport& link_;
    int local_;
    std::vector<PlayerInput> local_in_, remote_in_, used_remote_;
    std::vector<u8> remote_known_;
    std::vector<std::vector<u8>> states_;   // states_[f]: snapshot before frame f (ring of kWindow+2)
    std::vector<u32> hashes_;               // RAM hash after frame f, for confirmed frames
    std::vector<std::pair<u32, u32>> remote_hashes_;
    std::vector<u8> checked_;               // confirmed frames whose hash was compared
    u32 confirmed_ = 0, remote_ack_ = 0;
    u64 pumps_ = 0, last_rx_ = 0;
    bool paused_local_ = false, paused_remote_ = false;
    // checkpoint resync; each resync has an epoch number chosen by the host
    static constexpr u32 kNone = ~0u;
    u32 resync_floor_ = 0;                   // frames below this come from the host's state
    bool resync_wait_ = false;               // client: holding until the host's state is loaded
    bool tx_pending_ = false, tx_active_ = false;   // host: resync wanted / snapshot being sent
    u32 epoch_ = 0;                          // host: current epoch; client: last epoch loaded
    u32 tx_base_ = kNone, tx_cursor_ = 0, peer_have_ = 0;
    std::vector<u8> tx_blob_;
    bool peer_holding_ = false, peer_lead_known_ = false;
    int peer_lead_ = 0;
    u64 last_sync_wait_ = 0;
    u32 peer_frame_ = 0, peer_epoch_ = 0, peer_rx_epoch_ = 0, peer_rx_have_ = 0;
    u32 rx_epoch_ = 0, rx_base_ = kNone, rx_have_ = 0;
    std::vector<u8> rx_blob_;
    std::vector<u8> rx_chunks_;             // complete out-of-order snapshot chunks
    Stats stats_;
};

}  // namespace stv::net
