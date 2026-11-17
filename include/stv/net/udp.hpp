#pragma once
// UDP datagram transport for online co-op (PLAN P10). Windows sockets, non-blocking.
// Rollback traffic is unreliable by design (inputs are resent until acked); the
// session handshake and CMOS transfer use the same socket with their own acks.
#include "stv/net/rollback.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace stv::net {

class UdpTransport : public Transport {
public:
    UdpTransport();
    ~UdpTransport() override;
    UdpTransport(const UdpTransport&) = delete;
    UdpTransport& operator=(const UdpTransport&) = delete;

    bool bind(uint16_t port, std::string& error);          // host: listen on port (0 = any)
    bool set_peer(const std::string& host, uint16_t port, std::string& error);
    void expect_ip(uint32_t ip) { expected_ip_ = ip; }
    bool has_peer() const { return peer_set_; }
    // until a peer is set, the first sender becomes the peer (the host learns the client)
    void send(const std::vector<u8>& datagram) override;
    std::vector<std::vector<u8>> poll() override;
    uint16_t local_port() const;
    std::string peer_name() const;

private:
    uintptr_t sock_ = ~uintptr_t(0);
    uint32_t expected_ip_ = 0;
    bool peer_set_ = false;
    unsigned char peer_[16] = {};   // sockaddr_in
};

// Broadcast a discovery probe on the local network; returns the first host that
// is waiting for a player ("a.b.c.d"), or "" after timeout_ms.
std::string discover_lan_host(uint16_t port, int timeout_ms);

}  // namespace stv::net
