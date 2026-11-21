// PLAN C6: compatibility must be checked in both directions before CMOS/play.
#include "stv/net/handshake.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <type_traits>

using namespace stv::net;

namespace {
int failures = 0, checks = 0;
void check(bool ok, const char* name) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL %s\n", name); }
}
struct Wire : Transport {
    std::vector<std::vector<u8>> received, sent;
    void send(const std::vector<u8>& d) override { sent.push_back(d); }
    std::vector<std::vector<u8>> poll() override {
        auto data = std::move(received); received.clear(); return data;
    }
};
template<class T> void put(std::vector<u8>& d, const T& v) {
    auto p = reinterpret_cast<const u8*>(&v); d.insert(d.end(), p, p + sizeof v);
}
std::vector<u8> header(char type, char version =
#ifdef STV_PREP_BASELINE
    '1'
#else
    '2'
#endif
) {
    return {u8(type), 'S', 'T', 'V', 'N', 'E', 'T', u8(version)};
}
std::vector<u8> fp_packet(char type, const Fingerprint& fp, char version =
#ifdef STV_PREP_BASELINE
    '1'
#else
    '2'
#endif
) {
    auto d = header(type, version); put(d, fp.build); put(d, fp.program); put(d, fp.profile);
#ifndef STV_PREP_BASELINE
    put(d, fp.vram_width); put(d, fp.visible_extra);
#endif
    return d;
}
std::vector<u8> chunk(u16 index = 0) {
    auto d = header('C'); put(d, index);
    for (int i = 0; i < 512; ++i) put(d, u16(0xbeef));
    return d;
}
template<class F> F different_build(F fp) {
    if constexpr (std::is_integral_v<decltype(fp.build)>) fp.build ^= 0x80000000;
    else fp.build[31] ^= 0x80;
    return fp;
}
bool sends(const Wire& w, char type) {
    return std::any_of(w.sent.begin(), w.sent.end(), [=](const auto& d) { return !d.empty() && d[0] == type; });
}
void deliver(Wire& from, Wire& to, bool& lose_welcome, bool reverse) {
    if (reverse) std::reverse(from.sent.begin(), from.sent.end());
    for (const auto& d : from.sent) {
        if ((d[0] == 'W' || d[0] == 'V') && lose_welcome) { lose_welcome = false; continue; }
        to.received.push_back(d);
    }
    from.sent.clear();
}
void reject_host(const Fingerprint& mine, const Fingerprint& theirs, const char* label) {
    std::vector<u16> cmos(1537, 0xaaaa); const auto original = cmos;
    Wire w; Handshake hs(true, mine, cmos); w.received.push_back(fp_packet('H', theirs));
    check(hs.step(w) == Handshake::State::Failed && sends(w, 'X') && !sends(w, 'C') &&
          cmos == original && hs.chunks_done() == 0 && !hs.error().empty(), label);
}
void reject_client(const Fingerprint& mine, const Fingerprint& theirs, const char* label) {
    std::vector<u16> cmos(1537, 0xaaaa); const auto original = cmos;
    Wire w; Handshake hs(false, mine, cmos);
    w.received = {fp_packet('W', theirs), chunk(), header('G'), {'I'}};
    check(hs.step(w) == Handshake::State::Failed && cmos == original && hs.chunks_done() == 0 &&
          !sends(w, 'K') && !sends(w, 'R') && !hs.error().empty(), label);
}
void transfer(const Fingerprint& fp, bool loss_and_reorder, bool room = false) {
    std::vector<u16> source(1537), destination(room ? 512 : source.size(), 0xaaaa);
    for (std::size_t i = 0; i < source.size(); ++i) source[i] = u16(i * 37);
    Wire a, b;
#ifdef STV_PREP_BASELINE
    Handshake host(true, fp, source), client(false, fp, destination);
#else
    Handshake host(true, fp, source, room), client(false, fp, destination, room);
#endif
    bool drop_a = loss_and_reorder, drop_b = false;
    for (int tick = 0; tick < 500 && (host.state() != Handshake::State::Ready || client.state() != Handshake::State::Ready); ++tick) {
        host.step(a); deliver(a, b, drop_a, loss_and_reorder);
        client.step(b); deliver(b, a, drop_b, loss_and_reorder);
    }
    check(host.state() == Handshake::State::Ready && client.state() == Handshake::State::Ready &&
          destination == source && host.chunks_done() == 4 && client.chunks_done() == 4,
          loss_and_reorder ? "matching peers retry lost welcome and reordered chunks" : "matching peers transfer full and partial CMOS chunks");
}
Fingerprint sample_fp() {
    auto machine = std::make_unique<stv::gsp::Machine>();
    machine->prog = {0x1234, 0xabcd, 0};
    return fingerprint(*machine);
}
void unit_tests() {
    const auto fp = sample_fp();
#ifndef STV_PREP_BASELINE
    {
        auto hello = fp_packet('H', fp);
        check(Handshake::is_client_hello(hello), "complete hello can select direct peer");
        hello.pop_back();
        check(!Handshake::is_client_hello(hello) && !Handshake::is_client_hello({}) &&
              !Handshake::is_client_hello(fp_packet('W', fp)), "unrelated packets cannot claim waiting host");
        std::vector<u16> cmos(512); Wire wire; Handshake host(true, fp, cmos);
        wire.received = {fp_packet('H', fp)};
        host.step(wire);
        check(sends(wire, 'W'), "welcome on first compatible hello");
    }
#endif
    transfer(fp, false); transfer(fp, true);
#ifndef STV_PREP_BASELINE
    transfer(fp, true, true);  // guest learns the room history size despite lost and reordered datagrams
#endif
    reject_host(fp, different_build(fp), "host rejects different compiled build before CMOS");
    reject_client(fp, different_build(fp), "client rejects different compiled build before CMOS");
    auto other = fp; other.program ^= 0x80;
    reject_host(fp, other, "host rejects different ROM/program");
    reject_client(fp, other, "client rejects different ROM/program");
    other = fp; other.profile ^= 1;
    reject_host(fp, other, "host rejects different profile");
    reject_client(fp, other, "client rejects different profile");
#ifndef STV_PREP_BASELINE
    other = fp; ++other.vram_width;
    reject_host(fp, other, "host rejects different runtime geometry");
    reject_client(fp, other, "client rejects different runtime geometry");
#endif
    {
        std::vector<u16> cmos(512, 0xaaaa); const auto original = cmos;
        Wire w; Handshake client(false, fp, cmos);
        w.received = {chunk(), header('G'), {'I'}};
        check(client.step(w) == Handshake::State::Waiting && cmos == original && client.chunks_done() == 0 &&
              !sends(w, 'K') && !sends(w, 'R'), "client ignores data/go/rollback before host identity");
    }
    {
        std::vector<u16> cmos; Wire w; Handshake client(false, fp, cmos);
        w.received = {header('G'), {'I'}};
        check(client.step(w) == Handshake::State::Waiting, "empty CMOS does not bypass host identity");
    }
    {
        std::vector<u16> cmos(512, 0xaaaa); Wire w; Handshake host(true, fp, cmos);
        auto ack = header('K'); put(ack, u16(0)); w.received = {ack, header('R')};
        check(host.step(w) == Handshake::State::Waiting && host.chunks_done() == 0 && !sends(w, 'G'),
              "host ignores acknowledgements/go before compatible hello");
    }
    {
        std::vector<u16> cmos(512, 0xaaaa); const auto original = cmos;
        Wire w; Handshake client(false, fp, cmos); auto truncated = fp_packet('W', fp); truncated.pop_back();
        w.received = {truncated, chunk(), header('G')};
        check(client.step(w) == Handshake::State::Waiting && cmos == original && client.chunks_done() == 0,
              "truncated welcome cannot authorize CMOS");
    }
    {
        std::vector<u16> cmos(512, 0xaaaa); const auto original = cmos;
        Wire w; Handshake client(false, fp, cmos);
        w.received = {fp_packet('W', fp, '1'), chunk(), header('G')};
        check(client.step(w) == Handshake::State::Waiting && cmos == original && client.chunks_done() == 0,
              "previous wire protocol cannot authorize CMOS");
    }
    {
        std::vector<u16> cmos(512, 0xaaaa); Wire w; Handshake client(false, fp, cmos);
        w.received = {fp_packet('W', fp), chunk(), {'I'}};
        check(client.step(w) == Handshake::State::Ready && cmos[0] == 0xbeef && client.chunks_done() == 1,
              "compatible identity and completed CMOS allow rollback-packet go fallback");
    }
    {
        auto m = std::make_unique<stv::gsp::Machine>(); m->prog = {1, 2}; const auto a = fingerprint(*m);
        m->prog[0] ^= 1; const auto b = fingerprint(*m);
        check(a.build == b.build && a.profile == b.profile && a.program != b.program, "ROM/program identity is separate from implementation");
        m->enhanced = true; const auto c = fingerprint(*m);
        check(b.build == c.build && b.program == c.program && b.profile != c.profile, "profile identity is separate from implementation");
        ++m->vram_w; const auto d = fingerprint(*m);
        check(c.build == d.build && c.program == d.program && c.profile == d.profile && !(c == d),
              "runtime geometry is separate from implementation");
    }
    std::printf("%d checks, %d failures\n", checks, failures);
}
std::vector<u8> read_packet(const char* path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
} // namespace

int main(int argc, char** argv) {
    const auto fp = sample_fp();
    if (argc == 2 && std::strcmp(argv[1], "--identity") == 0) {
        const auto* bytes = reinterpret_cast<const u8*>(&fp.build);
        for (std::size_t i = 0; i < sizeof fp.build; ++i) std::printf("%02x", bytes[i]);
        std::printf(" %08x %u\n", fp.program, fp.profile); return 0;
    }
    if (argc == 3 && (std::strcmp(argv[1], "--emit-hello") == 0 || std::strcmp(argv[1], "--emit-welcome") == 0)) {
        auto data = fp_packet(argv[1][7] == 'h' ? 'H' : 'W', fp);
        std::ofstream out(argv[2], std::ios::binary); out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
        return out ? 0 : 2;
    }
    if (argc == 3 && (std::strcmp(argv[1], "--reject-hello") == 0 || std::strcmp(argv[1], "--reject-welcome") == 0)) {
        const bool host = argv[1][9] == 'h';
        std::vector<u16> cmos(512, 0xaaaa); const auto original = cmos; Wire w; Handshake hs(host, fp, cmos);
        w.received = {read_packet(argv[2]), chunk(), header('G'), {'I'}};
        check(hs.step(w) == Handshake::State::Failed && cmos == original && hs.chunks_done() == 0 &&
              !sends(w, 'C') && !sends(w, 'K') && !sends(w, 'G'), "separately compiled peer rejected before CMOS/play");
        std::printf("%s\n", hs.error().c_str()); return failures ? 1 : 0;
    }
    unit_tests(); return failures ? 1 : 0;
}
