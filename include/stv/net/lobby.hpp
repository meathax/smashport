#pragma once
// Two-player LAN rooms and reliable handoff to Smash TV's existing game transport.
// Discovery, bounded serialization and naming adapted from Sebastian Toyos's
// arcade-lan a7a15483521d59fec74177d26f6bd438ac8dcaa4 (BSD-3-Clause).
// Copyright (c) 2026, Sebastian Toyos. See third_party/arcade-lan-lobby/LICENSE.
// No gameplay input, rollback, CMOS, game snapshot or audio state belongs here.
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace stv::net {

struct LobbyAddress {
    std::uint32_t ip = 0;       // IPv4 a.b.c.d as a<<24 | b<<16 | c<<8 | d
    std::uint16_t port = 0;
    bool operator==(const LobbyAddress& o) const { return ip == o.ip && port == o.port; }
    bool operator!=(const LobbyAddress& o) const { return !(*this == o); }
};

struct LobbyIdentity {
    std::array<std::uint8_t, 32> build{};
    std::uint32_t program = 0, profile = 0, vram_width = 0, visible_extra = 0;
    bool operator==(const LobbyIdentity& o) const {
        return build == o.build && program == o.program && profile == o.profile &&
               vram_width == o.vram_width && visible_extra == o.visible_extra;
    }
    bool operator!=(const LobbyIdentity& o) const { return !(*this == o); }
};

struct LobbyConfig {
    std::uint64_t node_id = 0;  // nonzero random identity for this process, supplied by the host frontend
    std::string name = "PLAYER";
    LobbyIdentity identity{};
    std::uint16_t discovery_port = 27016;
    bool internet = false;
};

enum class LobbyState { Idle, Searching, Joining, Hosting, Joined, Starting, Handshaking, Playing };

struct LobbyRoom {
    std::uint64_t room_id = 0;
    std::uint64_t host_id = 0;
    LobbyAddress address{};    // control endpoint; distinct from the reserved game endpoint
    std::string name;
    std::uint16_t game_port = 0;
    unsigned players = 0;
    bool busy = false, compatible = false, locked = false;
    int ping_ms = -1;
    LobbyIdentity identity{};
    std::uint64_t last_seen_ms = 0;
};

struct LobbyMember {
    std::uint64_t id = 0;
    std::string name;
    bool host = false, own = false, heard = false;
};

struct LobbyHandoff {
    bool host = false;
    std::uint32_t peer_ip = 0;
    std::uint16_t peer_game_port = 0, local_game_port = 0;
    std::uint64_t room_id = 0, session_token = 0;
    std::string peer_name;
    LobbyAddress control;
};

struct LobbyPacket {
    LobbyAddress to{};
    std::vector<std::uint8_t> bytes;
};

// Pure state machine. Call tick/receive with a monotonic millisecond clock.
// An older timestamp is ignored. Outgoing datagrams are bounded and owned by the
// caller after take_outgoing(). Tests can route them without opening any sockets.
class Lobby {
public:
    static constexpr std::size_t kNameLength = 10;
    static constexpr std::size_t kMaximumRooms = 8;
    static constexpr std::size_t kMaximumPacket = 160;
    static constexpr std::uint64_t kRoomExpiryMs = 3000;
    static constexpr std::uint64_t kPeerExpiryMs = 3000;
    static constexpr std::uint64_t kJoinTimeoutMs = 5000;
    static constexpr std::uint64_t kStartTimeoutMs = 10000;

    explicit Lobby(LobbyConfig config);
    static std::string clean_name(std::string_view name);
    static std::string ip_text(std::uint32_t ip);

    void set_search_identity(LobbyIdentity identity) {
        if (state_ == LobbyState::Searching) config_.identity = identity;
    }
    void set_password(std::string password) { password_ = std::move(password); }
    void measured_ping(LobbyAddress address, int milliseconds);
    void reply_to_probe(LobbyAddress to);
    bool set_name(std::string_view name);  // only outside a room
    bool host(std::uint16_t local_game_port, std::uint64_t now_ms);
    bool search(std::uint64_t now_ms);
    bool join(const LobbyRoom& room, std::uint16_t local_game_port, std::uint64_t now_ms);
    bool start(std::uint64_t now_ms);      // host, with exactly one compatible member
    void leave(std::uint64_t now_ms);      // repeated leave packets, then clean local state
    void tick(std::uint64_t now_ms);
    void receive(LobbyAddress from, const std::vector<std::uint8_t>& bytes,
                 bool via_discovery, std::uint64_t now_ms);

    LobbyState state() const { return state_; }
    bool hosting() const { return host_; }
    bool can_start() const;
    const std::string& local_name() const { return config_.name; }
    const std::vector<LobbyRoom>& rooms() const { return rooms_; }
    const std::string& peer_name() const { return peer_name_; }
    std::vector<LobbyMember> members() const;
    const std::string& problem() const { return problem_; }
    void clear_problem() { problem_.clear(); }
    std::uint16_t discovery_port() const { return config_.discovery_port; }
    std::uint64_t room_id() const { return room_id_; }
    std::uint64_t session_token() const { return token_; }

    std::vector<LobbyPacket> take_outgoing();
    std::optional<LobbyHandoff> take_handoff();  // exactly once for an accepted start token
    bool established(std::uint64_t session_token, std::uint64_t now_ms);
    // Keep ticking/polling during the blocking game handshake and while playing.
    // established() acknowledges the game handshake; it never changes game state.
    // A timeout/leave is reported through problem()/state(), never neutral input.

private:
    enum class StartPhase { None, WaitReady, WaitCommit, WaitAck, Complete };
    bool clock(std::uint64_t now);
    void clear_room();
    void announce();
    void send_join();
    void send_list();
    void send_keepalive();
    void send_start(unsigned kind);
    void send_leave();
    void fail_peer(const char* why);
    void offer_handoff();
    void queue(LobbyAddress to, std::vector<std::uint8_t> bytes);

    LobbyConfig config_;
    LobbyState state_ = LobbyState::Idle;
    StartPhase start_phase_ = StartPhase::None;
    bool host_ = false, handoff_offered_ = false, clock_set_ = false;
    std::uint64_t now_ = 0, state_since_ = 0, peer_heard_ = 0;
    std::uint64_t last_beacon_ = 0, last_keepalive_ = 0, last_start_ = 0;
    std::uint64_t room_id_ = 0, peer_id_ = 0, token_ = 0, next_token_ = 1, next_room_ = 0;
    std::uint64_t membership_ = 0, next_membership_ = 1;
    std::uint16_t local_game_port_ = 0, peer_game_port_ = 0;
    LobbyAddress peer_{};
    std::string peer_name_, problem_, password_;
    std::vector<LobbyRoom> rooms_;
    std::vector<LobbyPacket> outgoing_;
    std::optional<LobbyHandoff> handoff_;
};

// The socket adapter owns only lobby sockets. It does not create, bind, poll,
// close or adopt any UdpTransport game socket. open() reserves an ephemeral
// control endpoint; poll() opens the shared beacon listener only while searching.
// All I/O is nonblocking and bounded per poll; no worker thread is created.
class LobbyUdp {
public:
    LobbyUdp();
    ~LobbyUdp();
    LobbyUdp(const LobbyUdp&) = delete;
    LobbyUdp& operator=(const LobbyUdp&) = delete;
    bool open(std::uint16_t discovery_port, std::string& error, bool public_room = false);
    bool target(const std::string& hostname, std::uint16_t port, std::string& error);
    bool poll(Lobby& lobby, std::uint64_t now_ms, std::string& error);
    void close();
    std::uint16_t local_port() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace stv::net

