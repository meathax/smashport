#pragma once
#include "session.hpp"

namespace stv {

// The online layer carries only the remote player's deterministic samples.
// The host merges them with its local slot before calling GameSession::advance.
// Three samples are sent per packet (current plus two redundant samples) so a
// single lost datagram does not require a retransmission or a speculative
// gameplay decision.  This is a wire format, not an ENet implementation.
struct InputSample {
    u64 boundary{};
    u64 timestamp{};
    PlayerInput input{};
};

struct InputBundle {
    static constexpr u32 magic = 0x53545649u; // STVI
    static constexpr u16 version = 1;
    static constexpr u16 header_bytes = 24;
    static constexpr unsigned max_samples = 3;
    static constexpr usize sample_bytes = 20;
    static constexpr usize max_wire_bytes = header_bytes + max_samples * sample_bytes;

    u32 packet_sequence{};
    u64 confirmed_boundary{};
    u8 player_slot{};
    u8 count{};
    InputSample samples[max_samples]{};
};

// A checkpoint is the only state information that crosses the simulation
// boundary.  It is a canonical digest of a confirmed GameSnapshot, never a
// host pointer or an opaque engine object.
struct CheckpointMessage {
    static constexpr u32 magic = 0x53545643u; // STVC
    static constexpr u16 version = 1;
    static constexpr u16 wire_bytes = 48;
    u64 boundary{};
    Sha256 state_fingerprint{};
};

enum class RollbackResult : u8 { accepted, duplicate };

// Deterministic transport-side window bookkeeping.  It deliberately does not
// advance GameSession: the caller supplies local frame boundaries and later
// connects a confirmed GameSnapshot replay to the returned rollback range.
// Keeping this policy separate means packet reordering and divergence checks
// can be tested without claiming that the source fixture is retail gameplay.
struct RollbackLedger {
    static constexpr u8 default_input_delay = 2;
    static constexpr u8 default_window = 12;
    static constexpr unsigned tracked_samples = 256;

    u8 input_delay{default_input_delay};
    u8 window{default_window};
    u64 latest_boundary{};
    u64 confirmed_boundary{};
    u64 newest_remote_boundary{};

    Status reset(u8 delay=default_input_delay, u8 rollback_window=default_window, u64 confirmed=0);
    Status observe_local_boundary(u64 boundary);
    Status accept_input_bundle(const InputBundle&, RollbackResult&);
    Status rollback_range(u64 late_boundary, u64& from, u64& to) const;
    Status remote_input_at_or_before(u64 boundary, PlayerInput& input, bool& exact) const;
    Status confirm_checkpoint(const CheckpointMessage&, const Sha256& local_digest);

private:
    bool sample_present[tracked_samples]{};
    u64 sample_boundaries[tracked_samples]{};
    InputSample remote_samples[tracked_samples]{};
};

enum class OnlinePhase : u8 { offline, connecting, ready, desynced, closed };

// Adapter-neutral online session lifecycle.  A concrete ENet adapter supplies
// the four SessionTransport callbacks; this class owns the compatibility gate
// and message ordering so a future socket implementation cannot bypass them.
struct OnlineSession {
    static constexpr usize tx_capacity = SessionHello::wire_bytes > InputBundle::max_wire_bytes ? SessionHello::wire_bytes : InputBundle::max_wire_bytes;

    SessionTransport transport{};
    OnlinePhase phase{OnlinePhase::offline};
    GameSession* game{};
    bool host{};
    RollbackLedger rollback{};
    RollbackResult last_input_result{RollbackResult::duplicate};

    Status open(GameSession&, SessionTransport, bool host_role, u16 port);
    // Call after each deterministic GameSession::advance. Retain canonical
    // hashes by boundary so delayed checkpoints never compare with "now".
    Status record_local_state();
    // Merge the role's local slot with the latest remote sample, use the
    // shared fixture boundary as its logical timestamp, advance only while
    // inside the input-delay horizon, and record its canonical state. A false
    // `advanced` means the session is waiting for remote input.
    Status advance_frame(const InputFrame& local_frame, Buffer rollback_storage, bool& advanced);
    // Apply queued late samples by replaying the affected GameSession suffix.
    // Caller storage must hold game->snapshot_size() bytes.
    Status apply_remote_corrections(Buffer rollback_storage);
    Status pump(Buffer scratch, usize& received, TransportChannel& channel);
    Status send_input(const InputBundle&);
    Status make_checkpoint(u64 boundary, CheckpointMessage&) const;
    Status send_checkpoint(const CheckpointMessage&);
    Status close();

private:
    SessionHello local_hello{};
    u8 hello_wire[SessionHello::wire_bytes]{};
    u8 tx_wire[tx_capacity]{};
    u64 digest_boundary[RollbackLedger::tracked_samples]{};
    Sha256 digest_history[RollbackLedger::tracked_samples]{};
    bool digest_present[RollbackLedger::tracked_samples]{};
    u64 pending_correction_boundary{};
    bool correction_pending{};

    Status digest_at(u64 boundary,Sha256& out)const;
    Status accept_control(Bytes);
    Status accept_input(Bytes);
    Status accept_checkpoint(Bytes);
};

Status encode_input_bundle(const InputBundle&, Buffer);
Status decode_input_bundle(Bytes, InputBundle&);
Status encode_checkpoint(const CheckpointMessage&, Buffer);
Status decode_checkpoint(Bytes, CheckpointMessage&);

}
