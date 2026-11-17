// LAN discovery/rooms adapted from arcade-lan, copyright (c) 2026 Sebastian Toyos.
// BSD-3-Clause: third_party/arcade-lan-lobby/LICENSE.
// Upstream a7a15483521d59fec74177d26f6bd438ac8dcaa4; see PROVENANCE.json.
// The explicit two-player START/READY/COMMIT/ACK protocol is an adaptation for
// handing off to Smash's existing Handshake, not the donor's lockstep transport.
#include "stv/net/lobby.hpp"

#include <algorithm>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace stv::net {
namespace {
using Byte = std::uint8_t;
using Clock = std::uint64_t;
constexpr Clock kBeaconMs = 500, kKeepaliveMs = 250, kStartRetryMs = 100, kSilentMs = 1000;
constexpr unsigned kBeacon = 1, kJoin = 2, kList = 3, kLeave = 4, kRefused = 5,
                   kStart = 6, kReady = 7, kCommit = 8, kAck = 9, kKeepalive = 10;
constexpr unsigned kFull = 1, kBusy = 2, kDifferent = 3, kPassword = 4;
constexpr std::uint32_t kBroadcast = 0xffffffffu, kLoopback = 0x7f000001u;
constexpr std::size_t kQueueLimit = 128;

struct Writer {
    std::vector<Byte> bytes;
    void u8(unsigned v) { bytes.push_back(Byte(v)); }
    void u16(std::uint16_t v) { u8(v); u8(v >> 8); }
    void u32(std::uint32_t v) { u16(std::uint16_t(v)); u16(std::uint16_t(v >> 16)); }
    void u64(std::uint64_t v) { u32(std::uint32_t(v)); u32(std::uint32_t(v >> 32)); }
    void name(std::string_view value) {
        for (std::size_t i = 0; i < Lobby::kNameLength; ++i) u8(i < value.size() ? Byte(value[i]) : 0);
    }
    void identity(const LobbyIdentity& f) {
        bytes.insert(bytes.end(), f.build.begin(), f.build.end());
        u32(f.program); u32(f.profile); u32(f.vram_width); u32(f.visible_extra);
    }
};
struct Reader {
    const std::vector<Byte>& bytes;
    std::size_t at = 0;
    bool valid = true;
    unsigned u8() {
        if (at >= bytes.size()) { valid = false; return 0; }
        return bytes[at++];
    }
    std::uint16_t u16() { const auto lo = u8(); return std::uint16_t(lo | (u8() << 8)); }
    std::uint32_t u32() { const auto lo = u16(); return std::uint32_t(lo) | (std::uint32_t(u16()) << 16); }
    std::uint64_t u64() { const auto lo = u32(); return std::uint64_t(lo) | (std::uint64_t(u32()) << 32); }
    std::string name() {
        std::string out;
        bool ended = false;
        for (std::size_t i = 0; i < Lobby::kNameLength; ++i) {
            const auto c = u8();
            if (!c) { ended = true; continue; }
            if (ended || !((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                           c == ' ' || c == '.' || c == '-')) valid = false;
            out.push_back(char(c));
        }
        return out;
    }
    LobbyIdentity identity() {
        LobbyIdentity f;
        for (auto& b : f.build) b = Byte(u8());
        f.program = u32(); f.profile = u32(); f.vram_width = u32(); f.visible_extra = u32();
        return f;
    }
    bool done() const { return valid && at == bytes.size(); }
};
Writer header(unsigned kind, std::uint64_t sender, std::uint64_t room, std::uint64_t membership = 0) {
    Writer w;
    for (const auto c : {'S', 'T', 'V', 'L'}) w.u8(unsigned(c));
    w.u8(1); w.u8(kind); w.u16(0); w.u64(sender); w.u64(room); w.u64(membership);
    return w;
}
bool valid_address(LobbyAddress a) { return a.ip != 0 && a.ip != kBroadcast && a.port != 0; }
bool room_state(LobbyState state) {
    return state == LobbyState::Hosting || state == LobbyState::Joined || state == LobbyState::Starting ||
           state == LobbyState::Handshaking || state == LobbyState::Playing;
}
}  // namespace

Lobby::Lobby(LobbyConfig config) : config_(std::move(config)) {
    config_.name = clean_name(config_.name);
    next_room_ = config_.node_id;
    if (!config_.node_id || !config_.discovery_port) problem_ = "Invalid lobby identity or discovery port";
}
std::string Lobby::clean_name(std::string_view from) {
    std::string out;
    for (std::size_t i = 0; i < from.size() && i < kNameLength; ++i) {
        unsigned char c = static_cast<unsigned char>(from[i]);
        if (!c) break;
        if (c >= 'a' && c <= 'z') c = static_cast<unsigned char>(c - 'a' + 'A');
        out += ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-') ? char(c) : ' ';
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    const auto first = out.find_first_not_of(' ');
    if (first != std::string::npos) out.erase(0, first);
    return out.empty() ? "PLAYER" : out;
}
std::string Lobby::ip_text(std::uint32_t ip) {
    return std::to_string(ip >> 24) + "." + std::to_string((ip >> 16) & 255) + "." +
           std::to_string((ip >> 8) & 255) + "." + std::to_string(ip & 255);
}
bool Lobby::clock(std::uint64_t now) {
    if (clock_set_ && now < now_) return false;
    now_ = now; clock_set_ = true;
    return true;
}
bool Lobby::set_name(std::string_view name) {
    if (state_ != LobbyState::Idle && state_ != LobbyState::Searching) return false;
    config_.name = clean_name(name);
    return true;
}
void Lobby::queue(LobbyAddress to, std::vector<Byte> bytes) {
    if (bytes.size() <= kMaximumPacket && outgoing_.size() < kQueueLimit && to.ip && to.port)
        outgoing_.push_back({to, std::move(bytes)});
}
void Lobby::clear_room() {
    state_ = LobbyState::Idle; start_phase_ = StartPhase::None; host_ = false;
    handoff_offered_ = false; handoff_.reset();
    room_id_ = peer_id_ = token_ = membership_ = 0; local_game_port_ = peer_game_port_ = 0;
    peer_ = {}; peer_name_.clear();
    state_since_ = peer_heard_ = last_beacon_ = last_keepalive_ = last_start_ = now_;
}
void Lobby::send_leave() {
    if (!peer_.port || !room_id_) return;
    for (unsigned copy = 0; copy < 3; ++copy)
        queue(peer_, header(kLeave, config_.node_id, room_id_, membership_).bytes);
}
void Lobby::leave(std::uint64_t now) {
    if (!clock(now)) return;
    outgoing_.clear();
    send_leave();
    clear_room();
    rooms_.clear();
}
bool Lobby::host(std::uint16_t port, std::uint64_t now) {
    if (!clock(now) || !config_.node_id || !config_.discovery_port || !port) return false;
    leave(now); problem_.clear();
    host_ = true; state_ = LobbyState::Hosting; local_game_port_ = port;
    do { ++next_room_; } while (!next_room_);
    room_id_ = next_room_;
    announce();
    return true;
}
bool Lobby::search(std::uint64_t now) {
    if (!clock(now) || !config_.node_id || !config_.discovery_port) return false;
    leave(now); problem_.clear(); state_ = LobbyState::Searching;
    return true;
}
bool Lobby::join(const LobbyRoom& room, std::uint16_t port, std::uint64_t now) {
    if (!clock(now) || !config_.node_id || !config_.discovery_port || !port || !room.room_id ||
        !room.host_id || room.host_id == config_.node_id || !valid_address(room.address) ||
        !room.game_port || !room.players || room.players > 2) return false;
    if (room.busy) { problem_ = "Room is already starting or playing"; return false; }
    if (room.players >= 2) { problem_ = "Room is full"; return false; }
    if (room.identity != config_.identity) { problem_ = "Different build or playfield profile"; return false; }
    const LobbyRoom chosen = room; // caller may pass a reference into rooms_, which leave() clears
    leave(now); problem_.clear();
    state_ = LobbyState::Joining; room_id_ = chosen.room_id; peer_ = chosen.address;
    peer_name_ = clean_name(chosen.name); peer_game_port_ = chosen.game_port; local_game_port_ = port;
    peer_id_ = chosen.host_id;
    membership_ = next_membership_++;
    if (!membership_) membership_ = next_membership_++;
    send_join();
    return true;
}
void Lobby::announce() {
    if (!host_ || !room_state(state_)) return;
    reply_to_probe({kBroadcast, config_.discovery_port});
    reply_to_probe({kLoopback, config_.discovery_port});
    last_beacon_ = now_;
}
void Lobby::reply_to_probe(LobbyAddress to) {
    if (!host_ || !room_state(state_)) return;
    auto w = header(kBeacon, config_.node_id, room_id_);
    w.name(config_.name); w.u8(peer_id_ ? 2u : 1u); w.u8(state_ != LobbyState::Hosting);
    w.u16(local_game_port_); w.identity(config_.identity);
    if (config_.internet && !password_.empty()) w.u8(1);
    queue(to, std::move(w.bytes));
}
void Lobby::send_join() {
    auto w = header(kJoin, config_.node_id, room_id_, membership_);
    w.name(config_.name); w.u16(local_game_port_); w.identity(config_.identity);
    if (config_.internet) w.bytes.insert(w.bytes.end(), password_.begin(), password_.end());
    queue(peer_, std::move(w.bytes)); last_keepalive_ = now_;
}
void Lobby::send_list() {
    if (!host_ || !peer_id_) return;
    auto w = header(kList, config_.node_id, room_id_, membership_);
    w.name(config_.name); w.name(peer_name_); w.u64(peer_id_);
    w.u16(local_game_port_); w.u16(peer_game_port_); w.identity(config_.identity);
    queue(peer_, std::move(w.bytes));
}
void Lobby::send_keepalive() {
    if (!peer_id_) return;
    auto w = header(kKeepalive, config_.node_id, room_id_, membership_); w.u64(token_);
    queue(peer_, std::move(w.bytes)); last_keepalive_ = now_;
}
bool Lobby::can_start() const {
    return host_ && state_ == LobbyState::Hosting && peer_id_ && peer_game_port_ &&
           now_ - peer_heard_ <= kPeerExpiryMs;
}
bool Lobby::start(std::uint64_t now) {
    if (!clock(now) || !can_start()) return false;
    problem_.clear(); token_ = next_token_++;
    if (!token_) token_ = next_token_++;
    state_ = LobbyState::Starting; state_since_ = now_; start_phase_ = StartPhase::WaitReady;
    handoff_.reset(); handoff_offered_ = false;
    send_start(kStart); announce();
    return true;
}
void Lobby::send_start(unsigned kind) {
    auto w = header(kind, config_.node_id, room_id_, membership_);
    w.u64(token_); w.u64(host_ ? peer_id_ : config_.node_id);
    w.u16(host_ ? local_game_port_ : peer_game_port_);
    w.u16(host_ ? peer_game_port_ : local_game_port_);
    queue(peer_, std::move(w.bytes)); last_start_ = now_;
}
void Lobby::offer_handoff() {
    if (handoff_offered_) return;
    handoff_offered_ = true;
    handoff_ = LobbyHandoff{host_, peer_.ip, peer_game_port_, local_game_port_, room_id_, token_, peer_name_, peer_};
}
std::optional<LobbyHandoff> Lobby::take_handoff() {
    auto result = std::move(handoff_); handoff_.reset(); return result;
}
bool Lobby::established(std::uint64_t token, std::uint64_t now) {
    if (!clock(now) || !token || token != token_ || !handoff_offered_ ||
        (state_ != LobbyState::Handshaking && state_ != LobbyState::Playing)) return false;
    state_ = LobbyState::Playing;
    // The real game handshake also establishes that the peer accepted COMMIT.
    // Continue answering duplicate control messages, but no gameplay lives here.
    start_phase_ = StartPhase::Complete;
    announce();
    return true;
}
std::vector<LobbyPacket> Lobby::take_outgoing() {
    std::vector<LobbyPacket> out; out.swap(outgoing_); return out;
}
std::vector<LobbyMember> Lobby::members() const {
    if (!room_state(state_) && state_ != LobbyState::Joining) return {};
    LobbyMember own{config_.node_id, config_.name, host_, true, true};
    LobbyMember other{peer_id_, peer_name_, !host_, false, now_ - peer_heard_ <= kSilentMs};
    if (host_) return peer_id_ ? std::vector<LobbyMember>{own, other} : std::vector<LobbyMember>{own};
    return peer_id_ ? std::vector<LobbyMember>{other, own} : std::vector<LobbyMember>{own};
}
void Lobby::fail_peer(const char* why) {
    problem_ = why;
    if (host_ && state_ == LobbyState::Hosting) {
        peer_id_ = 0; peer_ = {}; peer_name_.clear(); peer_game_port_ = 0;
        token_ = membership_ = 0; handoff_.reset(); handoff_offered_ = false; start_phase_ = StartPhase::None;
        announce();
    } else {
        send_leave(); clear_room();
    }
}
void Lobby::tick(std::uint64_t now) {
    if (!clock(now)) return;
    if (state_ == LobbyState::Searching) {
        rooms_.erase(std::remove_if(rooms_.begin(), rooms_.end(), [this](const auto& r) {
            return now_ - r.last_seen_ms >= kRoomExpiryMs;
        }), rooms_.end());
        return;
    }
    if (state_ == LobbyState::Joining) {
        if (now_ - state_since_ >= kJoinTimeoutMs) { problem_ = "No answer from host"; clear_room(); return; }
        if (now_ - last_keepalive_ >= kKeepaliveMs) send_join();
        return;
    }
    if (!room_state(state_)) return;
    // Internet paths can briefly stall during Wi-Fi changes or relay recovery.
    // Explicit leave packets still release the seat on the next poll.
    const auto peer_timeout = config_.internet && state_ == LobbyState::Playing ? 10000 : kPeerExpiryMs;
    if (peer_id_ && now_ - peer_heard_ >= peer_timeout) {
        fail_peer(host_ ? "Player 2 left or stopped responding" : "Host left or stopped responding");
        return;
    }
    if (start_phase_ != StartPhase::None && start_phase_ != StartPhase::Complete &&
        now_ - state_since_ >= kStartTimeoutMs) {
        fail_peer("Game start was not acknowledged"); return;
    }
    if (host_ && now_ - last_beacon_ >= kBeaconMs) announce();
    if (now_ - last_keepalive_ >= kKeepaliveMs) {
        if (state_ == LobbyState::Joined) send_join();  // also recovers a lost membership list
        else send_keepalive();
    }
    if (now_ - last_start_ >= kStartRetryMs) {
        if (start_phase_ == StartPhase::WaitReady) send_start(kStart);
        else if (start_phase_ == StartPhase::WaitCommit) send_start(kReady);
        else if (start_phase_ == StartPhase::WaitAck) send_start(kCommit);
    }
}
void Lobby::receive(LobbyAddress from, const std::vector<Byte>& bytes, bool discovery, std::uint64_t now) {
    if (!clock(now) || !valid_address(from) || bytes.size() < 32 || bytes.size() > kMaximumPacket) return;
    Reader r{bytes};
    for (const auto c : {'S', 'T', 'V', 'L'}) if (r.u8() != unsigned(c)) return;
    if (r.u8() != 1) return;
    const auto kind = r.u8();
    if (r.u16() != 0) return;
    const auto sender = r.u64(), room = r.u64(), membership = r.u64();
    if (!sender || sender == config_.node_id || !room) return;
    if (kind == kBeacon) {
        const auto name = r.name(); const auto count = r.u8(), busy = r.u8();
        const auto game_port = r.u16(); const auto identity = r.identity();
        const bool locked = config_.internet && r.at < bytes.size() && r.u8() == 1;
        if (!discovery || membership || state_ != LobbyState::Searching || !r.done() || name.empty() ||
            count < 1 || count > 2 || busy > 1 || !game_port) return;
        auto found = std::find_if(rooms_.begin(), rooms_.end(), [&](const auto& value) {
            // A local host is heard through both its LAN and loopback addresses.
            // One process/room identity is one browser row, even across interfaces.
            return value.room_id == room && value.host_id == sender;
        });
        // Keep an on-screen selection stable while its route is still heard.
        // Alternate NIC/loopback copies neither replace nor refresh that route;
        // after normal expiry a newly heard route can create the room again.
        if (found != rooms_.end() && found->address != from) return;
        LobbyRoom value;
        value.room_id = room; value.host_id = sender; value.address = from; value.name = name;
        value.locked = locked;
        if (found != rooms_.end()) value.ping_ms = found->ping_ms;
        value.game_port = game_port; value.players = count; value.busy = busy != 0;
        value.identity = identity; value.compatible = identity == config_.identity || (identity.build == config_.identity.build && identity.profile <= 1 && identity.profile != config_.identity.profile); value.last_seen_ms = now_;
        if (found != rooms_.end()) *found = std::move(value);
        else if (rooms_.size() < (config_.internet ? 200u : kMaximumRooms)) rooms_.push_back(std::move(value));
        return;
    }
    if (discovery || !membership || room != room_id_) return;
    if (kind == kJoin && host_ && room_state(state_)) {
        const auto name = r.name(); const auto game_port = r.u16(); const auto identity = r.identity();
        std::string supplied;
        if (config_.internet && r.valid && bytes.size() - r.at <= 32) {
            supplied.assign(bytes.begin() + r.at, bytes.end());
            r.at = bytes.size();
        }
        if (!r.done() || name.empty() || !game_port) return;
        unsigned reason = 0;
        if (config_.internet && supplied != password_) reason = kPassword;
        else if (identity != config_.identity) reason = kDifferent;
        else if (sender == peer_id_) {
            // Never silently replace the selected control or reserved game endpoint.
            if (membership != membership_ || from != peer_ || game_port != peer_game_port_ || name != peer_name_) return;
            peer_heard_ = now_;
            send_list(); // a lost LIST must recover even if the host already pressed Start
            if (start_phase_ == StartPhase::WaitReady) send_start(kStart);
            else if (start_phase_ == StartPhase::WaitAck) send_start(kCommit);
            return;
        } else if (state_ != LobbyState::Hosting) reason = kBusy;
        else if (peer_id_) reason = kFull;
        if (reason) {
            auto w = header(kRefused, config_.node_id, room_id_, membership); w.u8(reason);
            queue(from, std::move(w.bytes)); return;
        }
        peer_id_ = sender; peer_ = from; peer_name_ = name; peer_game_port_ = game_port; peer_heard_ = now_;
        membership_ = membership;
        send_list(); announce(); return;
    }
    // Bind leave/list/start to this exact join attempt. A delayed packet from a
    // prior membership must not evict or release a newly joined game.
    if (membership != membership_ || from != peer_ || (peer_id_ && sender != peer_id_)) return;
    if (kind == kList && !host_ && (state_ == LobbyState::Joining || state_ == LobbyState::Joined)) {
        const auto name = r.name(), guest_name = r.name(); const auto guest = r.u64();
        const auto host_port = r.u16(), guest_port = r.u16(); const auto identity = r.identity();
        if (!r.done() || name.empty() || guest_name != config_.name || guest != config_.node_id ||
            identity != config_.identity || host_port != peer_game_port_ || guest_port != local_game_port_) return;
        peer_id_ = sender; peer_name_ = name; peer_heard_ = now_; state_ = LobbyState::Joined;
        return;
    }
    if (kind == kRefused && !host_ && state_ == LobbyState::Joining) {
        const auto reason = r.u8(); if (!r.done() || reason < kFull || reason > kPassword) return;
        problem_ = reason == kPassword ? "Incorrect room password" : reason == kFull ? "Room is full" : reason == kBusy ? "Room is already playing" :
                   "Different build or playfield profile";
        clear_room(); return;
    }
    if (kind == kLeave) {
        if (!r.done() || (!room_state(state_) && state_ != LobbyState::Joining)) return;
        fail_peer(host_ ? "Player 2 left" : "Host left"); return;
    }
    if (kind == kKeepalive) {
        const auto token = r.u64();
        if (!r.done() || !room_state(state_) || !peer_id_ || token != token_) return;
        peer_heard_ = now_; return;
    }
    if (kind < kStart || kind > kAck || !room_state(state_)) return;
    const auto token = r.u64(), guest = r.u64(); const auto host_port = r.u16(), guest_port = r.u16();
    if (!r.done() || !token || guest != (host_ ? peer_id_ : config_.node_id) ||
        host_port != (host_ ? local_game_port_ : peer_game_port_) ||
        guest_port != (host_ ? peer_game_port_ : local_game_port_)) return;
    if (kind == kStart && !host_) {
        if (state_ == LobbyState::Joined && token > token_) {
            token_ = token; start_phase_ = StartPhase::WaitCommit; state_ = LobbyState::Starting; state_since_ = now_;
            handoff_.reset(); handoff_offered_ = false;
        } else if (token != token_ || start_phase_ == StartPhase::None) return;
        peer_heard_ = now_; send_start(kReady); return;
    }
    if (token != token_) return;
    if (kind == kReady && host_ &&
        (start_phase_ == StartPhase::WaitReady || start_phase_ == StartPhase::WaitAck)) {
        peer_heard_ = now_; start_phase_ = StartPhase::WaitAck;
        if (state_ == LobbyState::Starting) state_ = LobbyState::Handshaking;
        offer_handoff(); send_start(kCommit); return;
    }
    if (kind == kCommit && !host_ &&
        (start_phase_ == StartPhase::WaitCommit || start_phase_ == StartPhase::Complete)) {
        peer_heard_ = now_; start_phase_ = StartPhase::Complete;
        if (state_ == LobbyState::Starting) state_ = LobbyState::Handshaking;
        offer_handoff(); send_start(kAck); return;
    }
    if (kind == kAck && host_ && start_phase_ == StartPhase::WaitAck) {
        peer_heard_ = now_; start_phase_ = StartPhase::Complete;
    }
}

void Lobby::measured_ping(LobbyAddress address, int milliseconds) {
    for (auto& room : rooms_) if (room.address == address) {
        room.ping_ms = room.ping_ms < 0 ? milliseconds : (room.ping_ms * 3 + milliseconds) / 4;
        break;
    }
    std::stable_sort(rooms_.begin(), rooms_.end(), [](const auto& a, const auto& b) {
        if ((a.ping_ms < 0) != (b.ping_ms < 0)) return b.ping_ms < 0;
        return a.ping_ms < b.ping_ms;
    });
}

// Socket boundary adapted from arcade-lan/src/lan_udp.c.
namespace {
#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket kNoSocket = INVALID_SOCKET;
void close_socket(Socket s) { if (s != kNoSocket) closesocket(s); }
std::string socket_error() { return std::to_string(WSAGetLastError()); }
#else
using Socket = int;
constexpr Socket kNoSocket = -1;
void close_socket(Socket s) { if (s != kNoSocket) ::close(s); }
std::string socket_error() { return std::to_string(errno); }
#endif
Socket open_socket(std::uint16_t port, bool shared, std::string& error) {
    Socket s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kNoSocket) { error = "Cannot open lobby socket: " + socket_error(); return s; }
    const int on = 1;
    bool ok = setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof(on)) == 0;
    if (ok && shared) {
        ok = setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on), sizeof(on)) == 0;
#if defined(SO_REUSEPORT) && !defined(_WIN32)
        if (ok) ok = setsockopt(s, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on)) == 0;
#endif
    }
    sockaddr_in address{}; address.sin_family = AF_INET;
    address.sin_port = htons(port); address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (ok) ok = ::bind(s, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
#ifdef _WIN32
    u_long nonblocking = 1;
    if (ok) ok = ioctlsocket(s, FIONBIO, &nonblocking) == 0;
#else
    if (ok) {
        const int flags = fcntl(s, F_GETFL, 0);
        ok = flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
    }
#endif
    if (!ok) { error = "Cannot configure lobby socket: " + socket_error(); close_socket(s); return kNoSocket; }
    return s;
}
}  // namespace
struct LobbyUdp::Impl {
    LobbyAddress target{};
    std::uint64_t next_probe = 0;
    bool public_room = false;
    Socket control = kNoSocket, beacon = kNoSocket;
    std::uint16_t discovery_port = 0;
    bool initialized = false;
};
LobbyUdp::LobbyUdp() : impl_(std::make_unique<Impl>()) {}
LobbyUdp::~LobbyUdp() { close(); }
void LobbyUdp::close() {
    close_socket(impl_->beacon); close_socket(impl_->control);
    impl_->beacon = impl_->control = kNoSocket;
#ifdef _WIN32
    if (impl_->initialized) WSACleanup();
#endif
    impl_->initialized = false; impl_->discovery_port = 0;
}
bool LobbyUdp::open(std::uint16_t port, std::string& error, bool public_room) {
    close(); error.clear();
    if (!port) { error = "Lobby discovery port must be nonzero"; return false; }
#ifdef _WIN32
    WSADATA data{};
    const int started = WSAStartup(MAKEWORD(2, 2), &data);
    if (started) { error = "Winsock is unavailable: " + std::to_string(started); return false; }
#endif
    impl_->initialized = true; impl_->discovery_port = port;
    impl_->target = {}; impl_->next_probe = 0; impl_->public_room = public_room;
    impl_->control = open_socket(public_room ? port : 0, false, error);
    if (impl_->control == kNoSocket) { close(); return false; }
    return true;
}
bool LobbyUdp::target(const std::string& hostname, std::uint16_t port, std::string& error) {
    addrinfo hints{}, *resolved = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(hostname.c_str(), nullptr, &hints, &resolved) || !resolved) {
        error = "Cannot find host " + hostname;
        return false;
    }
    impl_->target = {ntohl(reinterpret_cast<sockaddr_in*>(resolved->ai_addr)->sin_addr.s_addr), port};
    freeaddrinfo(resolved);
    return true;
}
std::uint16_t LobbyUdp::local_port() const {
    if (impl_->control == kNoSocket) return 0;
    sockaddr_in address{};
#ifdef _WIN32
    int size = sizeof(address);
#else
    socklen_t size = sizeof(address);
#endif
    if (getsockname(impl_->control, reinterpret_cast<sockaddr*>(&address), &size)) return 0;
    return ntohs(address.sin_port);
}
bool LobbyUdp::poll(Lobby& lobby, std::uint64_t now, std::string& error) {
    error.clear();
    if (impl_->control == kNoSocket || impl_->discovery_port != lobby.discovery_port()) {
        error = "Lobby socket is not open for this discovery port"; return false;
    }
    constexpr char probe[] = "STVROOM1";
    if (lobby.state() == LobbyState::Searching && impl_->target.port && now >= impl_->next_probe) {
        sockaddr_in dest{};
        dest.sin_family = AF_INET; dest.sin_addr.s_addr = htonl(impl_->target.ip); dest.sin_port = htons(impl_->target.port);
        sendto(impl_->control, probe, 8, 0, reinterpret_cast<sockaddr*>(&dest), sizeof dest);
        impl_->next_probe = now + 500;
    }
    if (lobby.state() == LobbyState::Searching && !impl_->target.port && impl_->beacon == kNoSocket) {
        impl_->beacon = open_socket(impl_->discovery_port, true, error);
        if (impl_->beacon == kNoSocket) return false;
    } else if (lobby.state() != LobbyState::Searching && impl_->beacon != kNoSocket) {
        close_socket(impl_->beacon); impl_->beacon = kNoSocket;
    }
    for (unsigned channel = 0; channel < 2; ++channel) {
        const Socket s = channel ? impl_->beacon : impl_->control;
        if (s == kNoSocket) continue;
        for (unsigned count = 0; count < 64; ++count) {
            std::array<char, Lobby::kMaximumPacket + 1> bytes{};
            sockaddr_in from{};
#ifdef _WIN32
            int size = sizeof(from);
#else
            socklen_t size = sizeof(from);
#endif
            const int got = int(recvfrom(s, bytes.data(), int(bytes.size()), 0,
                                        reinterpret_cast<sockaddr*>(&from), &size));
            if (got > 0) {
                std::vector<Byte> data(bytes.begin(), bytes.begin() + got);
                const LobbyAddress address{ntohl(from.sin_addr.s_addr), ntohs(from.sin_port)};
                if (impl_->public_room && got == 8 && std::equal(data.begin(), data.end(), probe)) {
                    lobby.reply_to_probe(address);
                    continue;
                }
                const bool directed = impl_->target.port && lobby.state() == LobbyState::Searching;
                if (directed && address != impl_->target) continue;
                lobby.receive(address, data, channel != 0 || directed, now);
                continue;
            }
            if (got == 0) continue; // a zero-length datagram is malformed, not an end of stream
#ifdef _WIN32
            const int code = WSAGetLastError();
            if (code == WSAECONNRESET || code == WSAEMSGSIZE) continue;
            if (code == WSAEWOULDBLOCK) break;
#else
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
#endif
            error = "Lobby receive failed: " + socket_error(); return false;
        }
    }
    lobby.tick(now);
    bool ok = true;
    for (const auto& packet : lobby.take_outgoing()) {
        if (impl_->public_room && packet.bytes.size() > 5 && packet.bytes[5] == kBeacon &&
            (packet.to.ip == kBroadcast || packet.to == LobbyAddress{kLoopback, impl_->discovery_port})) continue;
        sockaddr_in to{}; to.sin_family = AF_INET; to.sin_port = htons(packet.to.port);
        to.sin_addr.s_addr = htonl(packet.to.ip);
        const int sent = int(sendto(impl_->control, reinterpret_cast<const char*>(packet.bytes.data()),
                                   int(packet.bytes.size()), 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to)));
        if (sent != int(packet.bytes.size())) {
            ok = false; if (error.empty()) error = "Lobby send failed: " + socket_error();
        }
    }
    return ok;
}
}  // namespace stv::net

