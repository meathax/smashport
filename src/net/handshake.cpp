// Online co-op session setup (PLAN P10). See include/stv/net/handshake.hpp.
#include "stv/net/handshake.hpp"
#include "stv/net/build_identity.hpp"

#include <algorithm>
#include <cstring>

namespace stv::net {

namespace {
constexpr char kMagic[7] = {'S', 'T', 'V', 'N', 'E', 'T', '2'};
constexpr std::size_t kChunkWords = 512;   // 1 KB

u32 fnv(const u16* p, std::size_t n, u32 h = 2166136261u) {
    for (std::size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 16777619u;
    return h;
}

template <class T> void put(std::vector<u8>& b, T v) { const u8* p = reinterpret_cast<const u8*>(&v); b.insert(b.end(), p, p + sizeof v); }
template <class T> bool get(const std::vector<u8>& b, std::size_t& at, T& v) {
    if (at + sizeof v > b.size()) return false;
    std::memcpy(&v, b.data() + at, sizeof v);
    at += sizeof v;
    return true;
}
std::vector<u8> header(char type) {
    std::vector<u8> d{u8(type)};
    d.insert(d.end(), kMagic, kMagic + sizeof kMagic);
    return d;
}
bool check_header(const std::vector<u8>& d, std::size_t& at) {
    if (d.size() < 1 + sizeof kMagic || std::memcmp(d.data() + 1, kMagic, sizeof kMagic) != 0) return false;
    at = 1 + sizeof kMagic;
    return true;
}
void put_fp(std::vector<u8>& d, const Fingerprint& f) {
    d.insert(d.end(), f.build.begin(), f.build.end());
    put(d, f.program); put(d, f.profile); put(d, f.vram_width); put(d, f.visible_extra);
}
bool get_fp(const std::vector<u8>& d, std::size_t& at, Fingerprint& f) {
    if (d.size() - at < f.build.size()) return false;
    std::copy_n(d.begin() + std::ptrdiff_t(at), f.build.size(), f.build.begin());
    at += f.build.size();
    return get(d, at, f.program) && get(d, at, f.profile) &&
           get(d, at, f.vram_width) && get(d, at, f.visible_extra) && at == d.size();
}
const char* mismatch(const Fingerprint& mine, const Fingerprint& theirs) {
    if (theirs.profile != mine.profile) return "different playfield profile (4:3 / 16:9)";
    if (theirs.vram_width != mine.vram_width || theirs.visible_extra != mine.visible_extra)
        return "different playfield geometry";
    if (theirs.program != mine.program) return "different ROM set or program patches";
    return "different SmashTV.exe build";
}
}  // namespace

Fingerprint fingerprint(const stv::gsp::Machine& m) {
    Fingerprint f;
    f.build = kBuildIdentity;
    f.vram_width = m.vram_w;
    f.visible_extra = m.vis_extra;
    f.program = fnv(m.prog.data(), m.prog.size());
    f.profile = m.enhanced ? 1 : 0;
    return f;
}

Handshake::Handshake(bool host, Fingerprint mine, std::vector<u16>& cmos, bool room_transfer)
    : host_(host), room_transfer_(room_transfer), mine_(mine), cmos_(cmos), have_((cmos.size() + kChunkWords - 1) / kChunkWords, 0) {}

bool Handshake::is_client_hello(const std::vector<u8>& packet) {
    std::size_t at = 0;
    Fingerprint peer;
    return check_header(packet, at) && packet.front() == 'H' && get_fp(packet, at, peer);
}

void Handshake::welcome(Transport& transport) const {
    auto packet = header(room_transfer_ ? 'V' : 'W');
    if (room_transfer_) put(packet, u32(cmos_.size()));
    put_fp(packet, mine_);
    transport.send(packet);
}

int Handshake::chunks_done() const { return int(std::count(have_.begin(), have_.end(), 1)); }

Handshake::State Handshake::step(Transport& t) {
    ++tick_;
    if (state_ == State::Failed || state_ == State::Ready) return state_;
    std::size_t nchunks = have_.size();
    for (const auto& d : t.poll()) {
        std::size_t at = 0;
        // the host's first rollback packet also means go (all 'G' datagrams may be lost)
        if (!host_ && peer_accepted_ && !d.empty() && d[0] == 'I' && chunks_done() == int(nchunks)) { state_ = State::Ready; return state_; }
        if (!check_header(d, at)) continue;
        const char type = char(d[0]);
        if (host_) {
            if (type == 'H') {
                Fingerprint theirs;
                if (!get_fp(d, at, theirs)) continue;
                if (!(theirs == mine_)) {
                    auto x = header('X');
                    const char* why = mismatch(mine_, theirs);
                    x.insert(x.end(), why, why + std::strlen(why));
                    t.send(x);
                    state_ = State::Failed, error_ = std::string("peer is incompatible: ") + why;
                    return state_;
                }
                if (state_ == State::Waiting) {
                    state_ = State::Transferring;
                    welcome(t);
                }
            } else if (type == 'K' && state_ == State::Transferring) {
                u16 idx = 0;
                if (get(d, at, idx) && idx < nchunks) have_[idx] = 1;
            } else if (type == 'R' && state_ == State::Transferring && chunks_done() == int(nchunks)) {
                go_ = true;
            }
        } else {
            if (type == 'X') {
                state_ = State::Failed, error_ = "host refused: " + std::string(d.begin() + std::ptrdiff_t(at), d.end());
                return state_;
            }
            if (type == (room_transfer_ ? 'V' : 'W')) {
                u32 words = u32(cmos_.size());
                if (room_transfer_ && (!get(d, at, words) || words < cmos_.size() || words > 16 * 1024 * 1024 ||
                    (peer_accepted_ && words != cmos_.size()))) continue;
                Fingerprint theirs;
                if (!get_fp(d, at, theirs)) continue;
                if (!(theirs == mine_)) {
                    state_ = State::Failed, error_ = std::string("host is incompatible: ") + mismatch(mine_, theirs);
                    return state_;
                }
                if (!peer_accepted_ && words != cmos_.size()) {
                    cmos_.resize(words);
                    nchunks = (words + kChunkWords - 1) / kChunkWords;
                    have_.assign(nchunks, 0);
                }
                peer_accepted_ = true;
                if (state_ == State::Waiting) state_ = State::Transferring;
            }
            if (type == 'C' && peer_accepted_) {
                u16 idx = 0;
                if (!get(d, at, idx) || idx >= nchunks) continue;
                const std::size_t words = std::min(kChunkWords, cmos_.size() - idx * kChunkWords);
                if (d.size() - at < words * 2) continue;
                std::memcpy(&cmos_[idx * kChunkWords], d.data() + at, words * 2);
                have_[idx] = 1;
                auto k = header('K');
                put(k, idx);
                t.send(k);
                state_ = State::Transferring;
            }
            if (type == 'G' && peer_accepted_ && chunks_done() == int(nchunks)) { state_ = State::Ready; return state_; }
        }
    }
    if (host_) {
        if (state_ == State::Transferring) {
            // (re)send unacked chunks, a few per frame
            int sent = 0;
            for (std::size_t i = 0; i < nchunks && sent < 32; ++i) {
                if (have_[i] || (tick_ + i) % 6) continue;
                auto c = header('C');
                put(c, u16(i));
                const std::size_t words = std::min(kChunkWords, cmos_.size() - i * kChunkWords);
                const u8* p = reinterpret_cast<const u8*>(&cmos_[i * kChunkWords]);
                c.insert(c.end(), p, p + words * 2);
                t.send(c);
                ++sent;
            }
            if (tick_ % 10 == 0) welcome(t);
        }
        if (go_) {
            for (int i = 0; i < 3; ++i) t.send(header('G'));   // repeated: the client may miss one
            state_ = State::Ready;
        }
    } else {
        if (state_ == State::Waiting && tick_ % 10 == 1) { auto h = header('H'); put_fp(h, mine_); t.send(h); }
        if (peer_accepted_ && chunks_done() == int(nchunks) && tick_ % 5 == 0) t.send(header('R'));
    }
    return state_;
}

}  // namespace stv::net
