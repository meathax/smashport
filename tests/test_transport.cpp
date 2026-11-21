#include "stv/session_transport.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
using namespace stv;

namespace {
std::vector<u8> read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("source pack open failed");
    auto size = file.tellg(); if (size < 0 || size > 64 * 1024 * 1024) throw std::runtime_error("source pack size invalid");
    std::vector<u8> bytes(static_cast<usize>(size)); file.seekg(0); file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
    if (!file) throw std::runtime_error("source pack read failed"); return bytes;
}
void require(Status status) { if (!status) throw std::runtime_error(std::string(status.message ? status.message : "status failure") + " at " + std::to_string(status.offset)); }
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

struct Pipe {
    u8 bytes[128]{};
    usize size{};
    TransportChannel channel{TransportChannel::control};
    bool reliable{};
    bool opened{};
    bool closed{};
};
struct Endpoint { Pipe* transmit{}; Pipe* receive{}; };
Status pipe_open(void* context, bool, u16) { auto& endpoint = *static_cast<Endpoint*>(context); endpoint.transmit->opened = true; endpoint.transmit->closed = false; endpoint.receive->opened = true; endpoint.receive->closed = false; return {}; }
Status pipe_send(void* context, TransportChannel channel, Bytes bytes, bool reliable) {
    auto& pipe = *static_cast<Endpoint*>(context)->transmit; if (!pipe.opened || bytes.size > sizeof pipe.bytes) return Status::fail("pipe send unavailable");
    copy(pipe.bytes, bytes.data, bytes.size); pipe.size = bytes.size; pipe.channel = channel; pipe.reliable = reliable; return {};
}
Status pipe_poll(void* context, Buffer out, usize& received, TransportChannel& channel) {
    auto& pipe = *static_cast<Endpoint*>(context)->receive; received = 0; if (!pipe.size) return {};
    if (!out.data || out.size < pipe.size) return Status::fail("pipe receive buffer is too small", pipe.size);
    copy(out.data, pipe.bytes, pipe.size); received = pipe.size; channel = pipe.channel; pipe.size = 0; return {};
}
Status pipe_close(void* context) { auto& endpoint = *static_cast<Endpoint*>(context); endpoint.transmit->closed = true; endpoint.receive->closed = true; return {}; }
SessionTransport link(Endpoint& endpoint) { return {&endpoint, TransportState::offline, pipe_open, pipe_send, pipe_poll, pipe_close}; }
}

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("Usage: stv_transport_tests data/source.stva");
        InputBundle source;
        source.packet_sequence = 19;
        source.confirmed_boundary = 40;
        source.player_slot = 1;
        source.count = 3;
        for (unsigned i = 0; i < source.count; ++i) {
            source.samples[i].boundary = 41 + i;
            source.samples[i].timestamp = 1000 + i * 2;
            source.samples[i].input.movement = u8(1u << i);
            source.samples[i].input.shooting = u8(8u >> i);
            source.samples[i].input.start = i == 0;
            source.samples[i].input.coin = i == 1;
        }
        std::vector<u8> bytes(InputBundle::max_wire_bytes);
        require(encode_input_bundle(source, {bytes.data(), bytes.size()}));
        InputBundle decoded;
        require(decode_input_bundle({bytes.data(), InputBundle::header_bytes + source.count * InputBundle::sample_bytes}, decoded));
        require(decoded.packet_sequence == source.packet_sequence && decoded.confirmed_boundary == source.confirmed_boundary && decoded.player_slot == 1 && decoded.count == 3, "input bundle header changed");
        require(decoded.samples[2].boundary == 43 && decoded.samples[2].input.shooting == 2 && decoded.samples[1].input.coin, "input bundle sample changed");
        require(!decoded.samples[1].input.accepted_coin, "wire input invented a fixture-only accepted-coin event");
        auto fixture_acceptance = source; fixture_acceptance.samples[0].input.accepted_coin = true;
        require(!encode_input_bundle(fixture_acceptance, {bytes.data(), bytes.size()}), "fixture accepted-coin event was serialized as remote physical input");
        auto broken = source;
        broken.samples[2].boundary++;
        require(!encode_input_bundle(broken, {bytes.data(), bytes.size()}), "non-contiguous input bundle was accepted");
        require(!decode_input_bundle({bytes.data(), InputBundle::header_bytes + source.count * InputBundle::sample_bytes - 1}, decoded), "truncated input bundle was accepted");

        CheckpointMessage checkpoint;
        checkpoint.boundary = 43;
        for (unsigned i = 0; i < 32; ++i) checkpoint.state_fingerprint.bytes[i] = u8(i * 3);
        std::vector<u8> checkpoint_bytes(CheckpointMessage::wire_bytes);
        require(encode_checkpoint(checkpoint, {checkpoint_bytes.data(), checkpoint_bytes.size()}));
        CheckpointMessage restored;
        require(decode_checkpoint({checkpoint_bytes.data(), checkpoint_bytes.size()}, restored));
        require(restored.boundary == checkpoint.boundary && equal(restored.state_fingerprint.bytes, checkpoint.state_fingerprint.bytes, 32), "checkpoint roundtrip changed canonical digest");
        checkpoint_bytes[0] ^= 1;
        require(!decode_checkpoint({checkpoint_bytes.data(), checkpoint_bytes.size()}, restored), "checkpoint with invalid magic was accepted");

        RollbackLedger ledger;
        require(ledger.reset());
        InputBundle injected_fixture_acceptance; injected_fixture_acceptance.player_slot = 1;
        injected_fixture_acceptance.count = 1; injected_fixture_acceptance.samples[0].boundary = 1;
        injected_fixture_acceptance.samples[0].timestamp = 1; injected_fixture_acceptance.samples[0].input.accepted_coin = true;
        RollbackResult injected_result{};
        require(!ledger.accept_input_bundle(injected_fixture_acceptance, injected_result), "fixture accepted-coin event entered the rollback input ledger");
        for (u64 boundary = 1; boundary <= 8; ++boundary) require(ledger.observe_local_boundary(boundary));
        InputBundle late;
        late.packet_sequence = 7;
        late.confirmed_boundary = 2;
        late.player_slot = 1;
        late.count = 3;
        for (unsigned i = 0; i < late.count; ++i) {
            late.samples[i].boundary = 6 + i;
            late.samples[i].timestamp = 60 + i;
            late.samples[i].input.movement = u8(1u << i);
        }
        RollbackResult rollback_result;
        require(ledger.accept_input_bundle(late, rollback_result) && rollback_result == RollbackResult::accepted, "first reordered input bundle was not accepted");
        require(ledger.accept_input_bundle(late, rollback_result) && rollback_result == RollbackResult::duplicate, "redundant input bundle was not treated as a duplicate");
        Sha256 digest{}; CheckpointMessage confirmed; confirmed.boundary = 4; confirmed.state_fingerprint = digest;
        require(ledger.confirm_checkpoint(confirmed, digest));
        u64 from{}, to{};
        require(ledger.rollback_range(6, from, to) && from == 6 && to == 8, "rollback window range changed");
        require(!ledger.rollback_range(0, from, to), "rollback older than confirmed state was accepted");
        auto future = late; future.samples[0].boundary = 11; future.samples[1].boundary = 12; future.samples[2].boundary = 13;
        require(!ledger.accept_input_bundle(future, rollback_result), "input beyond configured delay was accepted");
        CheckpointMessage good; good.boundary = 8; good.state_fingerprint = digest;
        require(ledger.confirm_checkpoint(good, digest));
        digest.bytes[0] = 1;
        require(!ledger.confirm_checkpoint(good, digest), "divergent checkpoint was accepted");
        RollbackLedger expired_rollback;
        require(expired_rollback.reset());
        for(u64 boundary=1;boundary<=20;++boundary)require(expired_rollback.observe_local_boundary(boundary));
        InputBundle too_late{};too_late.player_slot=1;too_late.count=1;
        too_late.samples[0].boundary=1;too_late.samples[0].timestamp=1;
        require(!expired_rollback.accept_input_bundle(too_late,rollback_result),
                "remote input older than the twelve-boundary correction window was accepted");

        auto source_bytes = read(argv[1]);
        auto pack_a = std::make_unique<SourcePack>(); auto pack_b = std::make_unique<SourcePack>();
        require(pack_a->open({source_bytes.data(), source_bytes.size()}));
        require(pack_b->open({source_bytes.data(), source_bytes.size()}));
        auto host_game = std::make_unique<GameSession>(); auto join_game = std::make_unique<GameSession>();
        require(host_game->load(*pack_a, GameProfile::source_fixture()));
        require(join_game->load(*pack_b, GameProfile::source_fixture()));
        require(host_game->reset(0x7788u, 2, 0, false));
        require(join_game->reset(0x7788u, 2, 0, false));
        Pipe host_to_join, join_to_host; Endpoint host_link{&host_to_join, &join_to_host}, join_link{&join_to_host, &host_to_join};
        OnlineSession host_session, join_session;
        require(host_session.open(*host_game, link(host_link), true, 1978));
        require(join_session.open(*join_game, link(join_link), false, 1978));
        u8 scratch[OnlineSession::tx_capacity]{}; usize received{}; TransportChannel channel{};
        require(join_session.pump({scratch, sizeof scratch}, received, channel));
        require(host_session.pump({scratch, sizeof scratch}, received, channel));
        require(host_session.phase == OnlinePhase::ready && join_session.phase == OnlinePhase::ready, "compatible online hello did not open both peers");
        InputBundle online_input = late; online_input.packet_sequence = 8; online_input.player_slot = 0;
        for (unsigned i = 0; i < online_input.count; ++i) online_input.samples[i].boundary = i;
        auto wrong_sender_slot=online_input;wrong_sender_slot.player_slot=1;
        require(!host_session.send_input(wrong_sender_slot),"host was allowed to send the joining player's input slot");
        require(host_session.send_input(online_input));
        require(join_session.pump({scratch, sizeof scratch}, received, channel));
        require(join_session.last_input_result == RollbackResult::accepted && !host_to_join.reliable, "online input did not use the unreliable path");
        CheckpointMessage online_checkpoint; online_checkpoint.boundary = 0; online_checkpoint.state_fingerprint = host_game->state_digest();
        require(host_session.send_checkpoint(online_checkpoint));
        require(join_session.pump({scratch, sizeof scratch}, received, channel));
        require(join_session.phase == OnlinePhase::ready && host_to_join.reliable, "compatible checkpoint did not remain ready/reliable");
        InputFrame first{};first.boundary=0;first.timestamp=1;
        first.player[0].shooting=1;
        require(host_game->advance(first));require(join_game->advance(first));
        require(host_session.record_local_state());require(join_session.record_local_state());
        CheckpointMessage delayed;delayed.boundary=1;delayed.state_fingerprint=host_game->state_digest();
        InputFrame second{};second.boundary=1;second.timestamp=2;
        second.player[0].movement=8;
        require(host_game->advance(second));require(join_game->advance(second));
        require(host_session.record_local_state());require(join_session.record_local_state());
        require(!equal(delayed.state_fingerprint.bytes,join_game->state_digest().bytes,32),
                "delayed checkpoint test did not advance beyond its recorded state");
        require(host_session.send_checkpoint(delayed));
        require(join_session.pump({scratch,sizeof scratch},received,channel));
        require(join_session.phase==OnlinePhase::ready&&join_session.rollback.confirmed_boundary==1,
                "delayed confirmed checkpoint was compared against the current game state");
        CheckpointMessage false_history;false_history.boundary=1;false_history.state_fingerprint=host_game->state_digest();
        require(!host_session.send_checkpoint(false_history),
                "outgoing checkpoint advertised the current state under an older boundary");
        require(encode_checkpoint(false_history,{checkpoint_bytes.data(),checkpoint_bytes.size()}));
        require(pipe_send(&host_link,TransportChannel::checkpoint,{checkpoint_bytes.data(),checkpoint_bytes.size()},true));
        require(!join_session.pump({scratch,sizeof scratch},received,channel)&&join_session.phase==OnlinePhase::desynced,
                "incoming checkpoint with a false historical hash did not pause on divergence");
        require(host_session.close() && join_session.close() && host_to_join.closed && join_to_host.closed, "online close did not close both adapters");

        auto correction_host_game = std::make_unique<GameSession>();
        auto correction_join_game = std::make_unique<GameSession>();
        require(correction_host_game->load(*pack_a, GameProfile::source_fixture()));
        require(correction_join_game->load(*pack_b, GameProfile::source_fixture()));
        require(correction_host_game->reset(0x9922u, 2, 0, false));
        require(correction_join_game->reset(0x9922u, 2, 0, false));
        Pipe correction_host_to_join, correction_join_to_host;
        Endpoint correction_host_link{&correction_host_to_join, &correction_join_to_host};
        Endpoint correction_join_link{&correction_join_to_host, &correction_host_to_join};
        OnlineSession correction_host, correction_join;
        require(correction_host.open(*correction_host_game, link(correction_host_link), true, 1978));
        require(correction_join.open(*correction_join_game, link(correction_join_link), false, 1978));
        require(correction_join.pump({scratch,sizeof scratch},received,channel));
        require(correction_host.pump({scratch,sizeof scratch},received,channel));
        require(correction_host.phase==OnlinePhase::ready&&correction_join.phase==OnlinePhase::ready,
                "rollback correction peers did not pass compatibility hello");

        PlayerInput host_local[4]{};
        host_local[0].movement=8; host_local[0].shooting=1;
        host_local[1].movement=1; host_local[1].shooting=2;
        host_local[2].shooting=4;
        host_local[3].movement=4;
        PlayerInput join_local[4]{};
        join_local[0].movement=2; join_local[0].shooting=8;
        join_local[1].movement=8; join_local[1].shooting=4;
        join_local[2].movement=1; join_local[2].shooting=2;
        join_local[3].movement=4; join_local[3].shooting=1;
        auto advance_local = [&](OnlineSession& online, GameSession& game, bool host_role, u64 boundary,
                                 const PlayerInput& input, bool expected_advance) {
            InputFrame local{}; local.boundary=boundary;
            local.timestamp=(host_role?1000:9000)+boundary;
            local.player[host_role?0:1]=input;
            std::vector<u8> recovery(game.snapshot_size()); bool did_advance=false;
            require(online.advance_frame(local,{recovery.data(),recovery.size()},did_advance));
            require(did_advance==expected_advance,"online input-delay horizon advanced or stalled at the wrong boundary");
        };
        for(u64 boundary=0;boundary<3;++boundary){
            advance_local(correction_host, *correction_host_game, true, boundary, host_local[boundary], true);
            advance_local(correction_join, *correction_join_game, false, boundary, join_local[boundary], true);
        }
        advance_local(correction_host, *correction_host_game, true, 3, host_local[3], false);
        advance_local(correction_join, *correction_join_game, false, 3, join_local[3], false);

        auto send_local_bundle = [&](OnlineSession& online, bool host_role, unsigned first, unsigned count, u32 sequence) {
            InputBundle bundle{}; bundle.packet_sequence=sequence; bundle.confirmed_boundary=0;
            bundle.player_slot=host_role?0:1; bundle.count=u8(count);
            for(unsigned i=0;i<count;++i){
                const unsigned boundary=first+i; bundle.samples[i].boundary=boundary;
                bundle.samples[i].timestamp=(host_role?1000:9000)+boundary;
                bundle.samples[i].input=host_role?host_local[boundary]:join_local[boundary];
            }
            require(online.send_input(bundle));
        };
        send_local_bundle(correction_host,true,0,3,20);
        require(correction_join.pump({scratch,sizeof scratch},received,channel));
        std::vector<u8> join_recovery(correction_join_game->snapshot_size());
        require(correction_join.apply_remote_corrections({join_recovery.data(),join_recovery.size()}));
        send_local_bundle(correction_join,false,0,3,21);
        require(correction_host.pump({scratch,sizeof scratch},received,channel));
        std::vector<u8> host_recovery(correction_host_game->snapshot_size());
        require(correction_host.apply_remote_corrections({host_recovery.data(),host_recovery.size()}));
        require(equal(correction_host_game->state_digest().bytes,correction_join_game->state_digest().bytes,32),
                "late remote samples did not replay both peers to the same state");
        require(correction_host.record_local_state()&&correction_join.record_local_state(),
                "rollback replay left the current checkpoint digest stale");
        CheckpointMessage corrected_midpoint;
        require(correction_host.make_checkpoint(1,corrected_midpoint)&&correction_host.send_checkpoint(corrected_midpoint),
                "corrected historical checkpoint could not be built and sent");
        require(correction_join.pump({scratch,sizeof scratch},received,channel)&&
                correction_join.rollback.confirmed_boundary==1,
                "peer digest history was not refreshed at a corrected intermediate boundary");
        require(!correction_host.make_checkpoint(4,corrected_midpoint),
                "future checkpoint was built before the state boundary existed");

        send_local_bundle(correction_host,true,3,1,22);
        require(correction_join.pump({scratch,sizeof scratch},received,channel));
        send_local_bundle(correction_join,false,3,1,23);
        require(correction_host.pump({scratch,sizeof scratch},received,channel));
        advance_local(correction_host,*correction_host_game,true,3,host_local[3],true);
        advance_local(correction_join,*correction_join_game,false,3,join_local[3],true);
        require(equal(correction_host_game->state_digest().bytes,correction_join_game->state_digest().bytes,32),
                "confirmed remote inputs did not produce identical online states");
        CheckpointMessage corrected_checkpoint; corrected_checkpoint.boundary=4;
        corrected_checkpoint.state_fingerprint=correction_host_game->state_digest();
        require(correction_host.send_checkpoint(corrected_checkpoint));
        require(correction_join.pump({scratch,sizeof scratch},received,channel));
        require(correction_join.rollback.confirmed_boundary==4,
                "checkpoint after rollback did not confirm the corrected state");

        InputBundle changed_confirmed{}; changed_confirmed.packet_sequence=24;
        changed_confirmed.confirmed_boundary=4; changed_confirmed.player_slot=0; changed_confirmed.count=1;
        changed_confirmed.samples[0].boundary=0; changed_confirmed.samples[0].timestamp=100;
        changed_confirmed.samples[0].input=host_local[0]; changed_confirmed.samples[0].input.movement^=1;
        require(correction_host.send_input(changed_confirmed));
        require(!correction_join.pump({scratch,sizeof scratch},received,channel),
                "changed input older than a confirmed checkpoint was accepted");
        require(correction_host.close()&&correction_join.close(),"rollback correction online sessions did not close");
        std::cout << "PASS: deterministic input redundancy and checkpoint wire formats\n";
        std::cout << "scope: adapter-neutral compatibility, delayed remote-input replay and checkpoint repair are tested; ENet sockets and full retail progression remain explicit boundaries\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
