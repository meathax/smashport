#pragma once
// Online co-op session setup (PLAN P10, D9): compatibility check and CMOS
// transfer before both peers start from power-on.
//
//  client -> host   'H' fingerprint            (repeated until welcomed)
//  host   -> client 'W' fingerprint | 'X' reason (incompatible)
//  host   -> client 'C' chunk index, 1 KB of the host's CMOS (resent until acked)
//  client -> host   'K' chunk index
//  client -> host   'R' (all chunks held)       host -> client 'G' (go), both start
// Both machines then run from reset with the same CMOS, so they stay identical.
#include "stv/net/rollback.hpp"

#include <array>
#include <string>
#include <vector>

namespace stv::net {

struct Fingerprint {
    std::array<u8, 32> build{}; // SHA-256 of compiled implementation inputs/settings
    u32 program = 0;   // imported L8 program image (after profile patches)
    u32 profile = 0;   // 0 reference_l8, 1 enhanced_l8_16_9
    u32 vram_width = 0;
    u32 visible_extra = 0;
    bool operator==(const Fingerprint& o) const {
        return build == o.build && program == o.program && profile == o.profile &&
               vram_width == o.vram_width && visible_extra == o.visible_extra;
    }
};

Fingerprint fingerprint(const stv::gsp::Machine& m);

class Handshake {
public:
    enum class State { Waiting, Transferring, Ready, Failed };
    // host: cmos is the image to send; client: cmos receives the host's image
    Handshake(bool host, Fingerprint mine, std::vector<u16>& cmos, bool room_transfer = false);
    // call once per frame; sends and receives on t
    State step(Transport& t);
    static bool is_client_hello(const std::vector<u8>& packet);
    State state() const { return state_; }
    const std::string& error() const { return error_; }
    int chunks_done() const;

private:
    void welcome(Transport& t) const;
    bool host_, room_transfer_;
    Fingerprint mine_;
    std::vector<u16>& cmos_;
    State state_ = State::Waiting;
    std::string error_;
    std::vector<u8> have_;   // chunk received (client) / acked (host)
    u64 tick_ = 0;
    bool go_ = false;
    bool peer_accepted_ = false; // client validated the host fingerprint
};

}  // namespace stv::net
