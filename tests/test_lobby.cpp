// Behavioral tests for the lobby adaptation. No game ROM, UI or live network is needed.
#include "stv/net/lobby.hpp"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <utility>

using namespace stv::net;
namespace {
int checks = 0;
void check(bool ok, const char* message) {
    ++checks;
    if (!ok) throw std::runtime_error(message);
}
LobbyIdentity identity(unsigned profile = 1) {
    LobbyIdentity f; f.build.fill(0x42); f.program = 0x12345678; f.profile = profile;
    f.vram_width = profile ? 1024 : 512; f.visible_extra = profile ? 147 : 0;
    return f;
}
LobbyConfig config(std::uint64_t id, const char* name = "PLAYER") {
    return {id, name, identity(), 27016};
}
unsigned kind(const LobbyPacket& p) { return p.bytes.size() > 5 ? p.bytes[5] : 0; }
void put64(std::vector<std::uint8_t>& b, std::size_t at, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) b.at(at + i) = std::uint8_t(value >> (8 * i));
}
struct Node {
    LobbyAddress address;
    Lobby lobby;
    Node(std::uint64_t id, std::uint16_t port, const char* name)
        : address{0x7f000001, port}, lobby(config(id, name)) {}
};
struct Envelope { Node* from; LobbyPacket packet; };
struct Network {
    Node host{100, 41001, "HOST"}, guest{200, 41002, "guest"}, third{300, 41003, "THIRD"};
    std::vector<Node*> nodes{&host, &guest, &third};
    std::function<bool(const Envelope&)> drop;
    unsigned duplicates = 1;
    void flush(std::uint64_t now) {
        for (unsigned round = 0; round < 24; ++round) {
            std::vector<Envelope> ready;
            for (auto* node : nodes)
                for (auto& packet : node->lobby.take_outgoing()) ready.push_back({node, std::move(packet)});
            if (ready.empty()) return;
            for (const auto& e : ready) {
                if (drop && drop(e)) continue;
                const bool discovery = e.packet.to.port == 27016;
                for (auto* to : nodes) {
                    if (to == e.from) continue;
                    if (!discovery && to->address != e.packet.to) continue;
                    for (unsigned copy = 0; copy < duplicates; ++copy)
                        to->lobby.receive(e.from->address, e.packet.bytes, discovery, now);
                }
            }
        }
        throw std::runtime_error("message exchange failed to settle");
    }
    void tick(std::uint64_t now) {
        for (auto* node : nodes) node->lobby.tick(now);
        flush(now);
    }
    LobbyRoom discover() {
        check(guest.lobby.search(0), "search begins");
        check(host.lobby.host(31001, 0), "host begins");
        flush(0);
        check(guest.lobby.rooms().size() == 1, "duplicate discovery packets retain one room");
        return guest.lobby.rooms().front();
    }
    void joined() {
        discover();
        // Pass an actual cache reference: join must copy it before clearing the browser.
        check(guest.lobby.join(guest.lobby.rooms().front(), 31002, 0), "join cache reference");
        flush(0);
        check(host.lobby.members().size() == 2 && guest.lobby.members().size() == 2, "two-member roster");
        check(guest.lobby.state() == LobbyState::Joined && host.lobby.can_start(), "joined and startable");
    }
};
LobbyPacket packet_of(std::vector<LobbyPacket> packets, unsigned wanted) {
    const auto it = std::find_if(packets.begin(), packets.end(), [&](const auto& p) { return kind(p) == wanted; });
    check(it != packets.end(), "expected protocol packet exists");
    return *it;
}
void naming_and_configuration() {
    check(Lobby::clean_name("some-user-name") == "SOME-USER-", "name bounded to ten source characters");
    check(Lobby::clean_name("  ab_9?! ") == "AB 9", "name normalization and trim");
    check(Lobby::clean_name("") == "PLAYER" && Lobby::clean_name("!?!") == "PLAYER", "blank name defaults");
    check(Lobby::clean_name(std::string("AB\0EVIL", 7)) == "AB", "embedded null terminates name");
    check(Lobby::ip_text(0xc0a80102) == "192.168.1.2", "IPv4 canonical text");
    Lobby invalid(config(0)); check(!invalid.host(30001, 0), "zero process identity rejected");
    Lobby a(config(1)); check(!a.host(0, 0), "unreserved game port rejected");
    check(a.search(0) && a.set_name("new"), "rename while browsing");
    check(a.local_name() == "NEW", "new normalized name");
    check(a.host(30001, 1) && !a.set_name("other"), "room name immutable until leave");
    a.leave(2); check(a.set_name("other"), "rename after leave");
}
void malformed_discovery_and_join() {
    Node host(100, 41001, "HOST"), browser(200, 41002, "CLIENT");
    host.lobby.host(31001, 0);
    auto beacon = packet_of(host.lobby.take_outgoing(), 1);
    for (std::size_t size = 0; size < beacon.bytes.size(); ++size) {
        Lobby candidate(config(200)); candidate.search(0);
        candidate.receive(host.address, {beacon.bytes.begin(), beacon.bytes.begin() + size}, true, 0);
        check(candidate.rooms().empty(), "every truncated beacon rejected");
    }
    const auto rejected = [&](std::vector<std::uint8_t> bytes, bool via_discovery = true) {
        Lobby candidate(config(200)); candidate.search(0);
        candidate.receive(host.address, bytes, via_discovery, 0);
        check(candidate.rooms().empty(), "malformed beacon rejected without room mutation");
    };
    auto b = beacon.bytes; b.push_back(0); rejected(b);
    b = beacon.bytes; b[0] ^= 1; rejected(b);
    b = beacon.bytes; b[4] = 2; rejected(b);
    b = beacon.bytes; b[6] = 1; rejected(b);
    b = beacon.bytes; put64(b, 8, 0); rejected(b);
    b = beacon.bytes; put64(b, 16, 0); rejected(b);
    b = beacon.bytes; put64(b, 24, 1); rejected(b);
    b = beacon.bytes; b[32] = 0xff; rejected(b);
    b = beacon.bytes; b[33] = 0; rejected(b);
    b = beacon.bytes; b[42] = 3; rejected(b);
    b = beacon.bytes; b[43] = 2; rejected(b);
    b = beacon.bytes; b[44] = b[45] = 0; rejected(b);
    rejected(beacon.bytes, false);
    rejected(std::vector<std::uint8_t>(Lobby::kMaximumPacket + 1, 0));
    browser.lobby.search(0); browser.lobby.receive(host.address, beacon.bytes, true, 0);
    check(browser.lobby.rooms().size() == 1, "valid beacon accepted after malformed candidates");
    browser.lobby.receive({0xc0a80102, host.address.port}, beacon.bytes, true, 0);
    check(browser.lobby.rooms().size() == 1, "LAN and loopback sources for the same host room deduplicate");
    check(browser.lobby.rooms()[0].address == host.address, "alternate route cannot invalidate an on-screen room selection");
    browser.lobby.receive(host.address, beacon.bytes, true, 0);
    browser.lobby.join(browser.lobby.rooms().front(), 31002, 0);
    const auto join = packet_of(browser.lobby.take_outgoing(), 2);
    for (std::size_t size = 0; size < join.bytes.size(); ++size) {
        host.lobby.receive(browser.address, {join.bytes.begin(), join.bytes.begin() + size}, false, 0);
        check(host.lobby.members().size() == 1 && !host.lobby.can_start(), "truncated JOIN cannot claim a slot");
    }
    host.lobby.receive(browser.address, join.bytes, false, 0);
    check(host.lobby.can_start(), "valid JOIN admits one member");
}
void room_cache_capacity_and_expiry() {
    Lobby browser(config(900)); browser.search(0);
    std::vector<LobbyRoom> snapshots;
    for (unsigned i = 0; i < 11; ++i) {
        Lobby host(config(100 + i, "HOST")); host.host(std::uint16_t(32000 + i), 0);
        const auto beacon = packet_of(host.take_outgoing(), 1);
        browser.receive({0x7f000001, std::uint16_t(42000 + i)}, beacon.bytes, true, 0);
    }
    check(browser.rooms().size() == Lobby::kMaximumRooms, "room cache bounded");
    const auto chosen = browser.rooms().at(4);
    check(chosen.address.port == 42004 && chosen.host_id == 104, "selected room carries stable identity");
    browser.tick(Lobby::kRoomExpiryMs - 1); check(browser.rooms().size() == 8, "room alive before deadline");
    browser.tick(Lobby::kRoomExpiryMs); check(browser.rooms().empty(), "all stale rooms expire exactly at deadline");
    browser.tick(0); check(browser.rooms().empty(), "backward clock ignored");
    check(chosen.address.port == 42004, "external selection retains its own endpoint after expiry");
    Lobby other(config(800)); other.search(0);
    Lobby incompatible({701, "OTHER", identity(0), 27016}); incompatible.host(32100, 0);
    auto beacon = packet_of(incompatible.take_outgoing(), 1);
    other.receive({0x7f000001, 42701}, beacon.bytes, true, 0);
    check(other.rooms().size() == 1 && other.rooms()[0].compatible, "other profile is selectable");
    check(!other.join(other.rooms()[0], 32101, 0), "guest must switch to the host profile before joining");
    other.set_search_identity(identity(0));
    check(other.join(other.rooms()[0], 32101, 0), "locally reproduced host profile can join");
}
void full_room_and_endpoint_binding() {
    Network n; const auto stale = n.discover();
    n.guest.lobby.join(n.guest.lobby.rooms().front(), 31002, 0); n.flush(0);
    check(n.third.lobby.join(stale, 31003, 0), "stale vacancy can send JOIN");
    n.flush(0);
    check(n.third.lobby.state() == LobbyState::Idle && n.third.lobby.problem() == "Room is full",
          "host authoritatively rejects third participant");
    check(n.host.lobby.members().size() == 2, "full-room rejection preserves roster");
    n.guest.lobby.tick(250);
    auto original = packet_of(n.guest.lobby.take_outgoing(), 2);
    n.host.lobby.receive({0x7f000002, 49999}, original.bytes, false, 250);
    auto forged = original.bytes; forged[42] ^= 4;
    n.host.lobby.receive(n.guest.address, forged, false, 250);
    check(n.host.lobby.start(250), "host start survives endpoint substitution attempts");
    n.flush(250);
    const auto handoff = n.host.lobby.take_handoff();
    check(handoff && handoff->peer_ip == n.guest.address.ip && handoff->peer_game_port == 31002,
          "handoff uses reserved selected game endpoint");
}
void reliable_start_under_independent_loss() {
    for (unsigned lost_kind : {6u, 7u, 8u, 9u}) {
        Network n; n.joined(); unsigned dropped = 0;
        n.drop = [&](const Envelope& e) { if (kind(e.packet) == lost_kind && dropped < 4) { ++dropped; return true; } return false; };
        n.duplicates = 2;
        check(n.host.lobby.start(0), "host starts under control-packet loss");
        n.flush(0);
        for (std::uint64_t now = 100; now <= 1800; now += 100) n.tick(now);
        check(dropped == 4, "requested start-message loss exercised");
        check(n.host.lobby.state() == LobbyState::Handshaking && n.guest.lobby.state() == LobbyState::Handshaking,
              "both peers reach handoff despite independent start/ready/commit/ack loss");
        const auto h = n.host.lobby.take_handoff(), g = n.guest.lobby.take_handoff();
        check(h && g && h->host && !g->host && h->session_token == g->session_token, "same start token and fixed player roles");
        check(h->peer_game_port == 31002 && g->peer_game_port == 31001 &&
              h->local_game_port == 31001 && g->local_game_port == 31002, "ports crossed exactly");
        check(h->peer_name == "GUEST" && g->peer_name == "HOST", "peer names available at handoff");
        check(!n.host.lobby.take_handoff() && !n.guest.lobby.take_handoff(), "handoff never delivered twice");
        check(!n.host.lobby.established(h->session_token + 1, 1800), "wrong established token rejected");
        check(n.host.lobby.established(h->session_token, 1800) &&
              n.guest.lobby.established(g->session_token, 1800), "game owner acknowledges established session");
        n.tick(2000);
        check(n.host.lobby.state() == LobbyState::Playing && n.guest.lobby.state() == LobbyState::Playing,
              "lobby keepalive continues independently of gameplay inputs");
    }
}
void lost_list_before_host_start() {
    Network n; n.discover(); bool discarded = false;
    n.drop = [&](const Envelope& e) { if (kind(e.packet) == 3 && !discarded) { discarded = true; return true; } return false; };
    n.guest.lobby.join(n.guest.lobby.rooms().front(), 31002, 0); n.flush(0);
    check(n.guest.lobby.state() == LobbyState::Joining && n.host.lobby.can_start(), "first membership list actually lost");
    check(n.host.lobby.start(0), "host can press start before membership-list delivery");
    n.flush(0);
    for (unsigned t = 100; t <= 1200; t += 100) n.tick(t);
    check(n.host.lobby.take_handoff().has_value() && n.guest.lobby.take_handoff().has_value(),
          "JOIN retry repairs lost list before reliable start");
}
void reorder_truncation_and_exactly_once() {
    Network n; n.joined(); n.host.lobby.start(0);
    auto start = packet_of(n.host.lobby.take_outgoing(), 6);
    // COMMIT arriving before START/READY must not release either game.
    auto commit = start.bytes; commit[5] = 8;
    n.guest.lobby.receive(n.host.address, commit, false, 0);
    check(!n.guest.lobby.take_handoff(), "early/reordered COMMIT rejected");
    for (std::size_t size = 0; size < start.bytes.size(); ++size) {
        n.guest.lobby.receive(n.host.address, {start.bytes.begin(), start.bytes.begin() + size}, false, 0);
        check(n.guest.lobby.state() == LobbyState::Joined, "truncated START cannot transition");
    }
    n.guest.lobby.receive(n.host.address, start.bytes, false, 0);
    auto ready = packet_of(n.guest.lobby.take_outgoing(), 7);
    auto wrong = ready.bytes; put64(wrong, 32, n.host.lobby.session_token() + 1);
    n.host.lobby.receive(n.guest.address, wrong, false, 0);
    check(!n.host.lobby.take_handoff(), "wrong READY token rejected");
    n.host.lobby.receive({0x7f000001, 49000}, ready.bytes, false, 0);
    check(!n.host.lobby.take_handoff(), "wrong READY source endpoint rejected");
    n.host.lobby.receive(n.guest.address, ready.bytes, false, 0);
    const auto h = n.host.lobby.take_handoff();
    check(h.has_value(), "host handoff only after valid READY");
    auto accepted_commit = packet_of(n.host.lobby.take_outgoing(), 8);
    n.guest.lobby.receive(n.host.address, accepted_commit.bytes, false, 0);
    check(n.guest.lobby.take_handoff().has_value(), "join handoff only after valid COMMIT");
    n.guest.lobby.take_outgoing(); // lose first ACK
    n.host.lobby.receive(n.guest.address, ready.bytes, false, 0);
    n.flush(0);
    check(!n.host.lobby.take_handoff() && !n.guest.lobby.take_handoff(), "duplicates acknowledge without another reset");
    n.host.lobby.tick(200); auto remaining = n.host.lobby.take_outgoing();
    check(std::none_of(remaining.begin(), remaining.end(), [](const auto& p) { return kind(p) == 8; }),
          "recovered ACK stops COMMIT retransmission");
}
void leave_timeout_and_rejoin() {
    Network n; n.joined();
    const auto old_room = n.host.lobby.room_id();
    n.guest.lobby.leave(1); n.flush(1);
    check(n.guest.lobby.state() == LobbyState::Idle && n.host.lobby.members().size() == 1 &&
          n.host.lobby.state() == LobbyState::Hosting, "guest leave removes only guest before game");
    n.guest.lobby.search(2); n.host.lobby.tick(501); n.flush(501);
    check(n.guest.lobby.rooms().size() == 1, "host advertises vacancy after leave");
    n.guest.lobby.join(n.guest.lobby.rooms()[0], 31002, 501); n.flush(501);
    check(n.host.lobby.can_start(), "rejoin succeeds");
    n.host.lobby.leave(502); n.flush(502);
    check(n.guest.lobby.state() == LobbyState::Idle && n.guest.lobby.problem() == "Host left", "host leave reported");
    n.host.lobby.host(31001, 503);
    check(n.host.lobby.room_id() != old_room, "new hosting lifetime has a distinct room id");
    Network timeout; timeout.joined();
    timeout.guest.lobby.tick(Lobby::kPeerExpiryMs);
    check(timeout.guest.lobby.state() == LobbyState::Idle && !timeout.guest.lobby.problem().empty(),
          "peer timeout uses elapsed milliseconds");
    Network lost; lost.joined(); lost.host.lobby.start(0); lost.flush(0);
    lost.host.lobby.take_handoff(); lost.guest.lobby.take_handoff();
    lost.guest.lobby.leave(1); lost.flush(1);
    check(lost.host.lobby.state() == LobbyState::Idle && !lost.host.lobby.take_handoff(),
          "lost peer after handoff closes lobby without creating another game");
    Lobby joining(config(400)); LobbyRoom absent;
    absent.room_id = 99; absent.host_id = 98; absent.address = {0x7f000001, 49999};
    absent.name = "ABSENT"; absent.game_port = 31000; absent.players = 1; absent.identity = identity();
    joining.join(absent, 31002, 0); joining.tick(Lobby::kJoinTimeoutMs);
    check(joining.state() == LobbyState::Idle && joining.problem() == "No answer from host", "join has bounded deadline");
}
void delayed_leave_from_prior_membership() {
    Network n; n.joined();
    n.guest.lobby.leave(1);
    const auto old_leave = packet_of(n.guest.lobby.take_outgoing(), 4);
    n.host.lobby.receive(n.guest.address, old_leave.bytes, false, 1);
    n.guest.lobby.search(2); n.flush(2);
    check(n.guest.lobby.rooms().size() == 1, "vacant room rediscovered for new membership");
    n.guest.lobby.join(n.guest.lobby.rooms()[0], 31002, 2); n.flush(2);
    check(n.host.lobby.can_start(), "new membership accepted");
    n.host.lobby.receive(n.guest.address, old_leave.bytes, false, 3);
    check(n.host.lobby.can_start() && n.host.lobby.members().size() == 2,
          "delayed old LEAVE cannot evict a new membership with the same endpoint");
    check(n.host.lobby.start(3), "new membership can start after delayed old LEAVE");
    n.flush(3);
    check(n.host.lobby.take_handoff().has_value() && n.guest.lobby.take_handoff().has_value(),
          "membership binding preserves reliable handoff");
}
void public_admission_and_ping() {
    auto hc=config(100,"HOST"), gc=config(200,"GUEST"), hc2=config(300,"FAST");
    hc.internet=gc.internet=hc2.internet=true;
    Lobby host(hc), guest(gc), fast(hc2);
    host.set_password("arcade"); host.host(1,0); fast.host(1,0); guest.search(0);
    LobbyAddress a{101,1}, b{102,1}, g{103,1};
    const auto beacon=packet_of(host.take_outgoing(),1).bytes;
    guest.receive(a,beacon,true,0);
    guest.receive(b,packet_of(fast.take_outgoing(),1).bytes,true,0);
    guest.measured_ping(a,80); guest.measured_ping(b,15);
    check(guest.rooms().size()==2 && guest.rooms()[0].address==b && guest.rooms()[1].locked,"measured ping order and lock flag");
    auto locked=guest.rooms()[1];
    guest.set_password("wrong"); guest.join(locked,1,1);
    host.receive(g,packet_of(guest.take_outgoing(),2).bytes,false,1);
    guest.receive(a,packet_of(host.take_outgoing(),5).bytes,false,1);
    check(!host.can_start() && guest.problem()=="Incorrect room password","wrong password leaves seat available");
    guest.set_password("arcade"); guest.join(locked,1,2);
    host.receive(g,packet_of(guest.take_outgoing(),2).bytes,false,2);
    guest.receive(a,packet_of(host.take_outgoing(),3).bytes,false,2);
    check(host.can_start() && guest.state()==LobbyState::Joined,"valid protected admission");
}
void lost_leave_and_start_deadline() {
    Network n; n.joined();
    n.guest.lobby.leave(1); n.guest.lobby.take_outgoing(); // lose every LEAVE copy
    n.guest.lobby.search(2);
    // A stale vacancy models the UI selection that began before membership loss.
    LobbyRoom room; room.room_id = n.host.lobby.room_id(); room.host_id = 100;
    room.address = n.host.address; room.name = "HOST"; room.game_port = 31001;
    room.players = 1; room.identity = identity();
    check(n.guest.lobby.join(room, 31002, 2), "new attempt begins despite lost LEAVE");
    n.flush(2);
    for (std::uint64_t t = 250; t <= 4000; t += 250) n.tick(t);
    check(n.guest.lobby.state() == LobbyState::Joined && n.host.lobby.can_start(),
          "old membership expires and repeated new JOIN recovers within join deadline");
    n.drop = [](const Envelope& e) { return kind(e.packet) == 6; };
    check(n.host.lobby.start(4000), "start begins with all START packets lost"); n.flush(4000);
    for (std::uint64_t t = 4250; t < 14000; t += 250) n.tick(t);
    check(n.host.lobby.state() == LobbyState::Starting, "valid roster replies keep peer alive until start deadline");
    n.tick(14000);
    check(n.host.lobby.state() == LobbyState::Idle &&
          n.host.lobby.problem() == "Game start was not acknowledged" &&
          !n.host.lobby.take_handoff(), "unacknowledged start has an absolute bounded deadline");
}
}  // namespace
int main() {
    try {
        public_admission_and_ping(); naming_and_configuration(); malformed_discovery_and_join(); room_cache_capacity_and_expiry();
        full_room_and_endpoint_binding(); reliable_start_under_independent_loss(); lost_list_before_host_start();
        reorder_truncation_and_exactly_once(); leave_timeout_and_rejoin(); delayed_leave_from_prior_membership();
        lost_leave_and_start_deadline();
        std::cout << "PASS lobby: " << checks << " behavioral checks; memory transport, no gameplay mutation\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL lobby after " << checks << " checks: " << e.what() << "\n";
        return 1;
    }
}

