#pragma once
#include "frontend.hpp"
#include "source_lab.hpp"
#include "coin_service.hpp"
#include "switch_scan.hpp"
#include "wave_flow.hpp"
#include "source_room.hpp"

namespace stv {

// The profile is data, not a second game implementation.  The reference
// geometry is the measured MAME L8 visible area; the enhanced width is the
// deterministic 16:9 projection from the driver's assumed 4:3 display model.
// The latter remains explicitly unverified until room bounds and pixel aspect
// have been measured from the executed reference.
enum class GameProfileId : u32 { source_fixture = 0x53524658u, reference_l8 = 0x4c385245u, enhanced_l8_16_9 = 0x4c38454eu };

struct GameGeometry {
    u16 source_width{410}, source_height{256};
    u16 playfield_width{400}, playfield_height{256};
    u16 left_extension{}, right_extension{};
    bool geometry_verified{};
};

struct BalanceVersion {
    u32 id{};
    u16 max_players{2};
    u16 population_numerator[2]{1, 1};
    u16 population_denominator[2]{1, 1};
    u16 release_numerator[2]{1, 1};
    u16 release_denominator[2]{1, 1};
};

struct GameProfile {
    GameProfileId id{GameProfileId::source_fixture};
    u32 build_id{0x00050014u};
    u32 asset_profile{};
    u32 frame_rate_numerator{8'000'000};
    u32 frame_rate_denominator{506u * 289u};
    GameGeometry geometry{};
    BalanceVersion balance{};
    bool process_graph_complete{};

    static GameProfile source_fixture();
    static GameProfile reference_l8();
    static GameProfile enhanced_l8_16_9();
    Status validate() const;
    Sha256 fingerprint() const;
};

enum class SessionPhase : u8 { boot, attract, credit, playing, paused, finished, boundary };

struct SessionPlayer {
    bool present{}, playing{};
    u8 lives{2};
    u32 score{};
};

// Stable metadata used by replay files and by the online compatibility gate.
// It contains fingerprints, never host addresses or pointers.
struct ReplayHeader {
    u32 magic{0x53545652u}; // STVR
    u16 version{2};
    u16 header_bytes{0};
    u32 build_id{};
    u32 profile_id{};
    u64 seed{};
    Sha256 asset_fingerprint{};
    Sha256 rules_fingerprint{};
};

// Control-plane identity exchanged before an online session starts.  The
// payload is deliberately independent of ENet so an eventual LAN/direct-IP
// transport cannot bypass deterministic compatibility checks.
struct SessionHello {
    static constexpr u32 magic = 0x53545648u; // STVH
    static constexpr u16 version = 2;
    static constexpr usize wire_bytes = 96;
    u32 build_id{};
    u32 profile_id{};
    u32 balance_id{};
    u16 max_players{2};
    u64 seed{};
    Sha256 asset_fingerprint{};
    Sha256 rules_fingerprint{};
};

// A snapshot is a versioned byte view. For the current source-component
// profile, mutable simulation is restored by replaying the same InputFrame
// timeline from a deterministic reset and then checking the canonical digest.
// The format is deliberately pointer-free and is the common implementation
// used by replay and rollback callers.
struct GameSnapshot {
    static constexpr u32 magic = 0x53545653u; // STVS
    static constexpr u16 version = 4;
    Bytes bytes{};
    u32 input_count{};
    Sha256 state_fingerprint{};
};

enum class TransportChannel : u8 { control, input, checkpoint };
enum class TransportState : u8 { offline, connecting, connected, closed };

using TransportOpen = Status(*)(void*, bool host, u16 port);
using TransportSend = Status(*)(void*, TransportChannel, Bytes, bool reliable);
using TransportPoll = Status(*)(void*, Buffer, usize& received, TransportChannel& channel);
using TransportClose = Status(*)(void*);

// ENet is supplied by the platform transport milestone. Keeping this seam
// independent of ENet makes the deterministic simulation testable offline and
// prevents a networking library from becoming part of gameplay state.
struct SessionTransport {
    void* context{};
    TransportState state{TransportState::offline};
    TransportOpen open{};
    TransportSend send{};
    TransportPoll poll{};
    TransportClose close{};
    bool valid() const { return open && send && poll && close; }
};

class GameSession {
public:
    static constexpr unsigned max_players = 2;
    static constexpr unsigned max_snapshot_inputs = 32768;

    GameProfile profile{};
    SessionPhase phase{SessionPhase::boot};
    SessionPlayer players[max_players]{};
    UiPersistent operator_state{};
    u64 boundary{};

    Status load(const SourcePack&, const GameProfile&);
    // Transfer a validated operator state at a session boundary. During play,
    // GameSession is the sole owner of credits, adjustments and audits.
    Status import_operator_state(const UiPersistent&);
    Status reset(u32 seed=0x13579bdfu, unsigned active_players=2, unsigned fixture_hulks=16,
                 bool lifecycle=false, const UiPersistent* session_settings=nullptr);
    // Loads an original WAVE_INFO/W2_INFO/W3_INFO table into the session
    // controller.  This is data ownership only: actor services still have to
    // be supplied through WaveOverlord::step before a retail room can run.
    Status prepare_room(unsigned circuit=0, unsigned wave=1);
    Status advance(const InputFrame&);
    // Replace one player's contiguous input suffix and deterministically
    // replay from the session baseline. `replayed_states` receives canonical
    // digests for state boundaries first_boundary+1 through the current one.
    // The caller supplies snapshot-sized recovery storage so a failed replay
    // restores the pre-correction state atomically.
    Status replace_player_inputs(u64 first_boundary, unsigned player_slot,
                                 const PlayerInput* inputs, usize count,
                                 Buffer recovery_storage, Sha256* replayed_states,
                                 usize replayed_state_capacity, usize& replayed_state_count);
    Status validate() const;
    Sha256 state_digest() const;

    usize snapshot_size() const;
    Status save_snapshot(Buffer, GameSnapshot&) const;
    Status restore_snapshot(GameSnapshot);
    Status make_replay_header(ReplayHeader&) const;
    Status make_session_hello(SessionHello&) const;
    Status validate_session_hello(const SessionHello&) const;

    // Executed-reference entry into a prepared retail room: runs the native
    // WAVES CREATE OVERLORD process graph with RAND/HCOUNT/FROMDR/PLYROBJS
    // supplied explicitly from pinned L8 evidence. This is a differential
    // harness, not play: advance() and snapshots stay fail-closed for retail
    // profiles, and every untranslated service stops the room by name.
    Status start_reference_room(const SourceRoomStart&);
    Status step_reference_room();
    const SourceRoom& reference_room() const { return retail_room; }

    SourceLab& fixture() { return arena; }
    const SourceLab& fixture() const { return arena; }
    WaveOverlord& room_flow() { return room; }
    const WaveOverlord& room_flow() const { return room; }
    const SourceWidgetStream& widget_input() const { return widget; }
    const SourceCoinService& coin_timing() const { return coin_service; }
    const SourceCoinValidation& coin_process() const { return coin_validation; }
    const SourceSlamProcess& slam_process() const { return slam; }

private:
    SourceLab arena{};
    const SourcePack* source_pack{};
    u32 seed{};
    UiPersistent initial_operator_state{};
    unsigned active_players{}, fixture_hulks{};
    bool lifecycle{}, loaded{};
    WaveOverlord room{};
    bool room_loaded{};
    SourceRoom retail_room{};
    SourceWidgetStream widget{};
    SourceCoinService coin_service{};
    SourceCoinValidation coin_validation{};
    SourceSlamProcess slam{};
    bool accepted_coin_pressed[max_players]{}, start_pressed[max_players]{};
    u64 last_timestamp{};
    InputFrame history[max_snapshot_inputs]{};
    u32 history_count{};

    Status advance_impl(const InputFrame&, bool record);
    void clear_session_state();
    void write_frame(u8*&, const InputFrame&) const;
    static bool read_frame(const u8*&, const u8*, InputFrame&);
};

Status encode_replay_header(const ReplayHeader&, Buffer);
Status decode_replay_header(Bytes, ReplayHeader&);
Status encode_session_hello(const SessionHello&, Buffer);
Status decode_session_hello(Bytes, SessionHello&);

}
