#include "stv/session_transport.hpp"

namespace stv {
namespace {

constexpr usize bundle_header_bytes = InputBundle::header_bytes;
constexpr usize bundle_sample_bytes = InputBundle::sample_bytes;

void write_sample(u8*& out, const InputSample& sample) {
    put64(out, sample.boundary); out += 8;
    put64(out, sample.timestamp); out += 8;
    *out++ = sample.input.movement;
    *out++ = sample.input.shooting;
    *out++ = u8(sample.input.start);
    *out++ = u8(sample.input.coin);
}

bool read_sample(const u8*& in, const u8* end, InputSample& sample) {
    if (!in || usize(end - in) < bundle_sample_bytes) return false;
    sample = {};
    sample.boundary = le64(in); in += 8;
    sample.timestamp = le64(in); in += 8;
    sample.input.movement = *in++;
    sample.input.shooting = *in++;
    sample.input.start = *in++ != 0;
    sample.input.coin = *in++ != 0;
    return true;
}

} // namespace

Status encode_input_bundle(const InputBundle& bundle, Buffer out) {
    const usize bytes = bundle_header_bytes + usize(bundle.count) * bundle_sample_bytes;
    if (!out.data || out.size < bytes) return Status::fail("Input bundle buffer is too small", bytes);
    if (bundle.count == 0 || bundle.count > InputBundle::max_samples) return Status::fail("Input bundle sample count is invalid");
    if (bundle.player_slot >= GameSession::max_players) return Status::fail("Input bundle player slot is invalid");
    for (unsigned i = 0; i < bundle.count; ++i) {
        if (bundle.samples[i].boundary != bundle.samples[0].boundary + i) return Status::fail("Input bundle boundaries are not contiguous", i);
        if (i && bundle.samples[i].timestamp < bundle.samples[i - 1].timestamp) return Status::fail("Input bundle timestamps moved backwards", i);
        if (bundle.samples[i].input.accepted_coin) return Status::fail("Fixture-accepted coin events cannot be sent as remote physical input", i);
    }
    u8* p = out.data;
    put32(p, InputBundle::magic); p += 4;
    put16(p, InputBundle::version); p += 2;
    put16(p, u16(bundle_header_bytes)); p += 2;
    put32(p, bundle.packet_sequence); p += 4;
    put64(p, bundle.confirmed_boundary); p += 8;
    *p++ = bundle.player_slot;
    *p++ = bundle.count;
    put16(p, 0); p += 2;
    for (unsigned i = 0; i < bundle.count; ++i) write_sample(p, bundle.samples[i]);
    return {};
}

Status decode_input_bundle(Bytes in, InputBundle& bundle) {
    if (!in.data || in.size < bundle_header_bytes) return Status::fail("Input bundle is truncated");
    if (le32(in.data) != InputBundle::magic || le16(in.data + 4) != InputBundle::version || le16(in.data + 6) != bundle_header_bytes) return Status::fail("Input bundle identity is invalid");
    InputBundle candidate;
    candidate.packet_sequence = le32(in.data + 8);
    candidate.confirmed_boundary = le64(in.data + 12);
    candidate.player_slot = in.data[20];
    candidate.count = in.data[21];
    if (candidate.count == 0 || candidate.count > InputBundle::max_samples) return Status::fail("Input bundle sample count is invalid");
    if (candidate.player_slot >= GameSession::max_players) return Status::fail("Input bundle player slot is invalid");
    const usize expected = bundle_header_bytes + usize(candidate.count) * bundle_sample_bytes;
    if (in.size != expected) return Status::fail("Input bundle length is invalid", expected);
    const u8* p = in.data + bundle_header_bytes;
    const u8* end = in.data + in.size;
    for (unsigned i = 0; i < candidate.count; ++i) {
        if (!read_sample(p, end, candidate.samples[i])) return Status::fail("Input bundle sample is truncated", i);
        if (candidate.samples[i].boundary != candidate.samples[0].boundary + i) return Status::fail("Input bundle boundaries are not contiguous", i);
        if (i && candidate.samples[i].timestamp < candidate.samples[i - 1].timestamp) return Status::fail("Input bundle timestamps moved backwards", i);
    }
    bundle = candidate;
    return {};
}

Status encode_checkpoint(const CheckpointMessage& checkpoint, Buffer out) {
    if (!out.data || out.size < CheckpointMessage::wire_bytes) return Status::fail("Checkpoint buffer is too small", CheckpointMessage::wire_bytes);
    u8* p = out.data;
    put32(p, CheckpointMessage::magic); p += 4;
    put16(p, CheckpointMessage::version); p += 2;
    put16(p, CheckpointMessage::wire_bytes); p += 2;
    put64(p, checkpoint.boundary); p += 8;
    copy(p, checkpoint.state_fingerprint.bytes, 32);
    return {};
}

Status decode_checkpoint(Bytes in, CheckpointMessage& checkpoint) {
    if (!in.data || in.size != CheckpointMessage::wire_bytes) return Status::fail("Checkpoint size is invalid");
    if (le32(in.data) != CheckpointMessage::magic || le16(in.data + 4) != CheckpointMessage::version || le16(in.data + 6) != CheckpointMessage::wire_bytes) return Status::fail("Checkpoint identity is invalid");
    CheckpointMessage candidate;
    candidate.boundary = le64(in.data + 8);
    copy(candidate.state_fingerprint.bytes, in.data + 16, 32);
    checkpoint = candidate;
    return {};
}

Status RollbackLedger::reset(u8 delay, u8 rollback_window, u64 confirmed) {
    if (delay > rollback_window || rollback_window == 0) return Status::fail("Rollback delay/window is invalid");
    input_delay = delay;
    window = rollback_window;
    latest_boundary = confirmed;
    confirmed_boundary = confirmed;
    newest_remote_boundary = confirmed;
    for (unsigned i = 0; i < tracked_samples; ++i) {
        sample_present[i] = false;
        sample_boundaries[i] = 0;
        remote_samples[i] = InputSample{};
    }
    return {};
}

Status RollbackLedger::observe_local_boundary(u64 boundary) {
    if (boundary == latest_boundary) return {};
    if (boundary != latest_boundary + 1) return Status::fail("Local rollback boundary is out of sequence", usize(latest_boundary));
    latest_boundary = boundary;
    return {};
}

Status RollbackLedger::accept_input_bundle(const InputBundle& bundle, RollbackResult& result) {
    if (bundle.count == 0 || bundle.count > InputBundle::max_samples) return Status::fail("Rollback input sample count is invalid");
    if (bundle.player_slot >= GameSession::max_players) return Status::fail("Rollback input player slot is invalid");
    for (unsigned i = 0; i < bundle.count; ++i) {
        const auto& sample = bundle.samples[i];
        if (sample.boundary != bundle.samples[0].boundary + i) return Status::fail("Rollback input boundaries are not contiguous", i);
        if (i && sample.timestamp < bundle.samples[i - 1].timestamp) return Status::fail("Rollback input timestamps moved backwards", i);
        if (sample.input.accepted_coin) return Status::fail("Fixture-accepted coin events cannot enter the rollback input ledger", i);
        if (sample.boundary > latest_boundary + u64(input_delay)) return Status::fail("Rollback input is beyond the configured delay", i);
        if (sample.boundary < latest_boundary && latest_boundary - sample.boundary > window)
            return Status::fail("Rollback input exceeds the configured correction window", i);
        const unsigned index = unsigned(sample.boundary % tracked_samples);
        const bool same = sample_present[index] && sample_boundaries[index] == sample.boundary &&
            remote_samples[index].timestamp == sample.timestamp &&
            remote_samples[index].input.movement == sample.input.movement &&
            remote_samples[index].input.shooting == sample.input.shooting &&
            remote_samples[index].input.start == sample.input.start &&
            remote_samples[index].input.coin == sample.input.coin;
        if (sample.boundary < confirmed_boundary && !same)
            return Status::fail("Changed remote input is older than the confirmed boundary", i);
    }
    result = RollbackResult::duplicate;
    for (unsigned i = 0; i < bundle.count; ++i) {
        const auto& sample = bundle.samples[i];
        const unsigned index = unsigned(sample.boundary % tracked_samples);
        const bool same = sample_present[index] && sample_boundaries[index] == sample.boundary &&
            remote_samples[index].timestamp == sample.timestamp &&
            remote_samples[index].input.movement == sample.input.movement &&
            remote_samples[index].input.shooting == sample.input.shooting &&
            remote_samples[index].input.start == sample.input.start &&
            remote_samples[index].input.coin == sample.input.coin;
        if (!same) {
            sample_present[index] = true;
            sample_boundaries[index] = sample.boundary;
            remote_samples[index] = sample;
            result = RollbackResult::accepted;
        }
        if (sample.boundary > newest_remote_boundary) newest_remote_boundary = sample.boundary;
    }
    return {};
}

Status RollbackLedger::remote_input_at_or_before(u64 boundary, PlayerInput& input, bool& exact) const {
    input = {};
    exact = false;
    bool found = false;
    u64 nearest = 0;
    for (unsigned i = 0; i < tracked_samples; ++i) {
        if (!sample_present[i] || sample_boundaries[i] > boundary) continue;
        if (!found || sample_boundaries[i] > nearest) {
            found = true;
            nearest = sample_boundaries[i];
            input = remote_samples[i].input;
        }
    }
    exact = found && nearest == boundary;
    input.accepted_coin = false;
    return {};
}

Status RollbackLedger::rollback_range(u64 late_boundary, u64& from, u64& to) const {
    if (late_boundary < confirmed_boundary) return Status::fail("Rollback request is older than the confirmed boundary", usize(confirmed_boundary));
    if (late_boundary > latest_boundary) return Status::fail("Rollback request is ahead of the local boundary", usize(latest_boundary));
    if (latest_boundary - late_boundary > window) return Status::fail("Rollback request exceeds the configured window", usize(window));
    from = late_boundary;
    to = latest_boundary;
    return {};
}

Status RollbackLedger::confirm_checkpoint(const CheckpointMessage& checkpoint, const Sha256& local_digest) {
    if (checkpoint.boundary > latest_boundary) return Status::fail("Checkpoint is ahead of the local boundary", usize(latest_boundary));
    if (!equal(checkpoint.state_fingerprint.bytes, local_digest.bytes, 32)) return Status::fail("Confirmed checkpoint fingerprint diverged", usize(checkpoint.boundary));
    if (checkpoint.boundary >= confirmed_boundary) confirmed_boundary = checkpoint.boundary;
    return {};
}

Status OnlineSession::open(GameSession& candidate, SessionTransport link, bool host_role, u16 port) {
    if (!link.valid()) return Status::fail("Online session transport callbacks are incomplete");
    SessionHello hello;
    auto s = candidate.make_session_hello(hello);
    if (!s) return s;
    s = encode_session_hello(hello, {hello_wire, sizeof hello_wire});
    if (!s) return s;
    game = &candidate;
    host = host_role;
    local_hello = hello;
    transport = link;
    phase = OnlinePhase::connecting;
    transport.state = TransportState::connecting;
    s = transport.open(transport.context, host, port);
    if (!s) { phase = OnlinePhase::offline; transport.state = TransportState::offline; game = nullptr; return s; }
    // The hello is reliable regardless of the eventual transport's packet
    // classes.  It is sent before any input or checkpoint is permitted.
    s = transport.send(transport.context, TransportChannel::control, {hello_wire, sizeof hello_wire}, true);
    if (!s) { close(); return s; }
    return {};
}

Status OnlineSession::digest_at(u64 boundary,Sha256& out)const{
    const unsigned index=unsigned(boundary%RollbackLedger::tracked_samples);
    if(!digest_present[index]||digest_boundary[index]!=boundary)
        return Status::fail("Canonical checkpoint state is unavailable at this boundary",usize(boundary));
    out=digest_history[index];return {};
}

Status OnlineSession::record_local_state(){
    if(phase!=OnlinePhase::ready||!game)return Status::fail("Online session is not ready to record local state");
    const u64 current=game->boundary;
    if(current!=rollback.latest_boundary&&current!=rollback.latest_boundary+1)
        return Status::fail("Online GameSession advanced without a recorded boundary",usize(rollback.latest_boundary));
    auto digest=game->state_digest();
    const unsigned index=unsigned(current%RollbackLedger::tracked_samples);
    if(current==rollback.latest_boundary&&digest_present[index]&&digest_boundary[index]==current){
        if(!equal(digest.bytes,digest_history[index].bytes,32))
            return Status::fail("Recorded local state changed at the same boundary",usize(current));
        return {};
    }
    auto s=rollback.observe_local_boundary(current);if(!s)return s;
    digest_boundary[index]=current;digest_history[index]=digest;digest_present[index]=true;
    return {};
}

Status OnlineSession::advance_frame(const InputFrame& local_frame, Buffer rollback_storage, bool& advanced) {
    advanced = false;
    if (phase != OnlinePhase::ready || !game) return Status::fail("Online session is not ready to advance");
    if (game->boundary != rollback.latest_boundary)
        return Status::fail("Online GameSession advanced outside the online coordinator", usize(rollback.latest_boundary));
    auto s = apply_remote_corrections(rollback_storage); if (!s) return s;
    const u64 boundary = game->boundary;
    if (local_frame.boundary != boundary) return Status::fail("Online local input boundary is out of sequence", usize(boundary));
    if (local_frame.slam) return Status::fail("Global cabinet slam is not represented by the online input protocol");
    const unsigned local_slot = host ? 0u : 1u;
    const unsigned remote_slot = 1u - local_slot;
    if (local_frame.player[0].accepted_coin || local_frame.player[1].accepted_coin)
        return Status::fail("Fixture-accepted coin events cannot enter an online session");

    const u64 horizon = rollback.newest_remote_boundary > ~u64(0) - rollback.input_delay
        ? ~u64(0) : rollback.newest_remote_boundary + rollback.input_delay;
    if (boundary > horizon) return {};

    InputFrame merged = local_frame;
    // The fixture's boundary is its common logical clock. Host QPC/device
    // timestamps are local and must not enter the deterministic game digest.
    // A retail profile must replace this with the measured L8 event-clock map.
    merged.timestamp = boundary;
    PlayerInput remote{};
    bool exact = false;
    s = rollback.remote_input_at_or_before(boundary, remote, exact); if (!s) return s;
    (void)exact;
    merged.player[remote_slot] = remote;

    GameSnapshot before;
    s = game->save_snapshot(rollback_storage, before); if (!s) return s;
    s = game->advance(merged);
    if (s) s = record_local_state();
    if (!s) {
        auto restored = game->restore_snapshot(before);
        return restored ? s : restored;
    }
    advanced = true;
    return {};
}

Status OnlineSession::apply_remote_corrections(Buffer rollback_storage) {
    if (!correction_pending) return {};
    if (phase != OnlinePhase::ready || !game) return Status::fail("Online correction arrived before the session was ready");
    if (game->boundary != rollback.latest_boundary)
        return Status::fail("Online correction cannot replay an unrecorded GameSession boundary", usize(rollback.latest_boundary));
    if (pending_correction_boundary >= game->boundary) {
        correction_pending = false;
        return {};
    }

    u64 from{}, to{};
    auto s = rollback.rollback_range(pending_correction_boundary, from, to); if (!s) return s;
    if (to != game->boundary) return Status::fail("Online correction range does not reach the current GameSession boundary", usize(to));
    const usize count = usize(to - from);
    if (!count || count > RollbackLedger::tracked_samples)
        return Status::fail("Online correction range exceeds retained input history", count);

    PlayerInput replacement[RollbackLedger::tracked_samples]{};
    Sha256 replayed[RollbackLedger::tracked_samples]{};
    const unsigned remote_slot = host ? 1u : 0u;
    for (usize i = 0; i < count; ++i) {
        bool exact = false;
        s = rollback.remote_input_at_or_before(from + i, replacement[i], exact); if (!s) return s;
        (void)exact;
    }
    usize replayed_count = 0;
    s = game->replace_player_inputs(from, remote_slot, replacement, count, rollback_storage,
                                    replayed, RollbackLedger::tracked_samples, replayed_count);
    if (!s) return s;
    if (replayed_count && replayed_count != count)
        return Status::fail("Online correction returned an incomplete checkpoint history", replayed_count);
    for (usize i = 0; i < replayed_count; ++i) {
        const u64 state_boundary = from + 1 + i;
        const unsigned index = unsigned(state_boundary % RollbackLedger::tracked_samples);
        digest_boundary[index] = state_boundary;
        digest_history[index] = replayed[i];
        digest_present[index] = true;
    }
    correction_pending = false;
    return {};
}

Status OnlineSession::accept_control(Bytes bytes) {
    SessionHello remote;
    auto s = decode_session_hello(bytes, remote);
    if (!s) return s;
    if (!game) return Status::fail("Online session has no GameSession");
    s = game->validate_session_hello(remote);
    if (!s) return s;
    s = rollback.reset(RollbackLedger::default_input_delay,RollbackLedger::default_window,game->boundary);
    if (!s) return s;
    correction_pending = false;
    for(unsigned i=0;i<RollbackLedger::tracked_samples;++i)digest_present[i]=false;
    phase = OnlinePhase::ready;
    transport.state = TransportState::connected;
    return record_local_state();
}

Status OnlineSession::accept_input(Bytes bytes) {
    if (phase != OnlinePhase::ready) return Status::fail("Online input arrived before compatibility hello");
    InputBundle bundle;
    auto s = decode_input_bundle(bytes, bundle);
    if (!s) return s;
    const unsigned expected_remote_slot = host ? 1u : 0u;
    if (bundle.player_slot != expected_remote_slot)
        return Status::fail("Online input bundle used the local player's slot", bundle.player_slot);
    s = rollback.accept_input_bundle(bundle, last_input_result);
    if (!s || last_input_result != RollbackResult::accepted) return s;
    for (unsigned i = 0; i < bundle.count; ++i) {
        const u64 sample_boundary = bundle.samples[i].boundary;
        if (sample_boundary < game->boundary &&
            (!correction_pending || sample_boundary < pending_correction_boundary)) {
            pending_correction_boundary = sample_boundary;
            correction_pending = true;
        }
    }
    return {};
}

Status OnlineSession::accept_checkpoint(Bytes bytes) {
    if (phase != OnlinePhase::ready) return Status::fail("Online checkpoint arrived before compatibility hello");
    if (!game) return Status::fail("Online session has no GameSession");
    CheckpointMessage checkpoint;
    auto s = decode_checkpoint(bytes, checkpoint);
    if (!s) return s;
    Sha256 local_digest;
    s=digest_at(checkpoint.boundary,local_digest);if(!s)return s;
    s = rollback.confirm_checkpoint(checkpoint, local_digest);
    if (!s) phase = OnlinePhase::desynced;
    return s;
}

Status OnlineSession::pump(Buffer scratch, usize& received, TransportChannel& channel) {
    received = 0;
    if (phase == OnlinePhase::offline || phase == OnlinePhase::closed) return Status::fail("Online session is not open");
    auto s = transport.poll(transport.context, scratch, received, channel);
    if (!s) return s;
    if (!received) return {};
    if (channel == TransportChannel::control) s = accept_control({scratch.data, received});
    else if (channel == TransportChannel::input) s = accept_input({scratch.data, received});
    else if (channel == TransportChannel::checkpoint) s = accept_checkpoint({scratch.data, received});
    else s = Status::fail("Online session received an unknown channel");
    if (!s && phase != OnlinePhase::desynced) phase = OnlinePhase::closed;
    return s;
}

Status OnlineSession::send_input(const InputBundle& bundle) {
    if (phase != OnlinePhase::ready) return Status::fail("Online input cannot be sent before compatibility hello");
    const unsigned expected_local_slot = host ? 0u : 1u;
    if (bundle.player_slot != expected_local_slot)
        return Status::fail("Online input bundle must use the local player's slot", bundle.player_slot);
    auto s = encode_input_bundle(bundle, {tx_wire, sizeof tx_wire});
    if (!s) return s;
    return transport.send(transport.context, TransportChannel::input, {tx_wire, InputBundle::header_bytes + usize(bundle.count) * InputBundle::sample_bytes}, false);
}

Status OnlineSession::make_checkpoint(u64 boundary, CheckpointMessage& checkpoint) const {
    if (phase != OnlinePhase::ready) return Status::fail("Online checkpoint is unavailable before compatibility hello");
    if (boundary > rollback.latest_boundary) return Status::fail("Online checkpoint boundary is ahead of the local session", usize(rollback.latest_boundary));
    CheckpointMessage candidate{};
    candidate.boundary = boundary;
    auto s = digest_at(boundary, candidate.state_fingerprint); if (!s) return s;
    checkpoint = candidate;
    return {};
}

Status OnlineSession::send_checkpoint(const CheckpointMessage& checkpoint) {
    if (phase != OnlinePhase::ready) return Status::fail("Online checkpoint cannot be sent before compatibility hello");
    Sha256 local_digest;
    auto s=digest_at(checkpoint.boundary,local_digest);if(!s)return s;
    if(!equal(checkpoint.state_fingerprint.bytes,local_digest.bytes,32))
        return Status::fail("Outgoing checkpoint does not match the recorded local state",usize(checkpoint.boundary));
    s = encode_checkpoint(checkpoint, {tx_wire, sizeof tx_wire});
    if (!s) return s;
    return transport.send(transport.context, TransportChannel::checkpoint, {tx_wire, CheckpointMessage::wire_bytes}, true);
}

Status OnlineSession::close() {
    Status result;
    if (transport.close && transport.state != TransportState::offline && transport.state != TransportState::closed)
        result = transport.close(transport.context);
    phase = OnlinePhase::closed;
    transport.state = TransportState::closed;
    game = nullptr;
    return result;
}

}
