// UDP transport (PLAN P10). See include/stv/net/udp.hpp.
#include "stv/net/udp.hpp"
#include "stv/net/handshake.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstring>

namespace stv::net {

namespace {
struct WsaInit {
    bool ok = false;
    WsaInit() { WSADATA d; ok = WSAStartup(MAKEWORD(2, 2), &d) == 0; }
    ~WsaInit() { if (ok) WSACleanup(); }
};
WsaInit& wsa() { static WsaInit w; return w; }
SOCKET S(uintptr_t s) { return static_cast<SOCKET>(s); }
}  // namespace

UdpTransport::UdpTransport() {
    if (!wsa().ok) return;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return;
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    sock_ = uintptr_t(s);
}

UdpTransport::~UdpTransport() {
    if (S(sock_) != INVALID_SOCKET) closesocket(S(sock_));
}

bool UdpTransport::bind(uint16_t port, std::string& error) {
    if (S(sock_) == INVALID_SOCKET) { error = "no socket (Winsock unavailable)"; return false; }
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(S(sock_), reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
        error = "cannot listen on UDP port " + std::to_string(port) + " (in use?)";
        return false;
    }
    return true;
}

bool UdpTransport::set_peer(const std::string& host, uint16_t port, std::string& error) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) {
        error = "cannot resolve " + host;
        return false;
    }
    std::memcpy(peer_, res->ai_addr, sizeof(sockaddr_in));
    freeaddrinfo(res);
    peer_set_ = true;
    return true;
}

void UdpTransport::send(const std::vector<u8>& d) {
    if (!peer_set_ || S(sock_) == INVALID_SOCKET) return;
    sendto(S(sock_), reinterpret_cast<const char*>(d.data()), int(d.size()), 0,
           reinterpret_cast<const sockaddr*>(peer_), sizeof(sockaddr_in));
}

std::vector<std::vector<u8>> UdpTransport::poll() {
    std::vector<std::vector<u8>> out;
    if (S(sock_) == INVALID_SOCKET) return out;
    char buf[2048];
    for (int received = 0; received < 256; ++received) {
        sockaddr_in from{};
        int len = sizeof from;
        const int n = recvfrom(S(sock_), buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &len);
        if (n == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEMSGSIZE) continue;
            break;
        }
        if (n == 0) continue;
        if (n == 8 && std::memcmp(buf, "STVDISC1", 8) == 0) {   // LAN discovery probe: answer, do not adopt
            if (!peer_set_) sendto(S(sock_), "STVHERE1", 8, 0, reinterpret_cast<const sockaddr*>(&from), sizeof from);
            continue;
        }
        if (!peer_set_) {
            if (expected_ip_ && ntohl(from.sin_addr.s_addr) != expected_ip_) continue;
            if (!Handshake::is_client_hello({buf, buf + n})) continue;
            std::memcpy(peer_, &from, sizeof from);
            peer_set_ = true;
        } else if (std::memcmp(&reinterpret_cast<sockaddr_in*>(peer_)->sin_addr, &from.sin_addr, sizeof from.sin_addr) != 0 ||
                   reinterpret_cast<sockaddr_in*>(peer_)->sin_port != from.sin_port) {
            continue;       // not our peer
        }
        out.emplace_back(buf, buf + n);
    }
    return out;
}

std::string discover_lan_host(uint16_t port, int timeout_ms) {
    if (!wsa().ok) return "";
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return "";
    BOOL on = TRUE;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof on);
    DWORD to = 100;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof to);
    sockaddr_in b{};
    b.sin_family = AF_INET;
    b.sin_port = htons(port);
    b.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    std::string found;
    for (int waited = 0; waited < timeout_ms && found.empty(); waited += 100) {
        if (waited % 500 == 0) sendto(s, "STVDISC1", 8, 0, reinterpret_cast<const sockaddr*>(&b), sizeof b);
        char buf[16];
        sockaddr_in from{};
        int len = sizeof from;
        const int n = recvfrom(s, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &len);
        if (n == 8 && std::memcmp(buf, "STVHERE1", 8) == 0) {
            char ip[64] = "";
            inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
            found = ip;
        }
    }
    closesocket(s);
    return found;
}

uint16_t UdpTransport::local_port() const {
    sockaddr_in a{};
    int len = sizeof a;
    if (S(sock_) == INVALID_SOCKET || getsockname(S(sock_), reinterpret_cast<sockaddr*>(&a), &len) != 0) return 0;
    return ntohs(a.sin_port);
}

std::string UdpTransport::peer_name() const {
    if (!peer_set_) return "";
    char ip[64] = "";
    const sockaddr_in* p = reinterpret_cast<const sockaddr_in*>(peer_);
    inet_ntop(AF_INET, &p->sin_addr, ip, sizeof ip);
    return std::string(ip) + ":" + std::to_string(ntohs(p->sin_port));
}

}  // namespace stv::net
