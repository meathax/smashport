#pragma once
// Little binary stream for exact coupled-machine snapshots (online join).
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

namespace stv::snd {
struct StateOut {
    std::vector<uint8_t>& b;
    void raw(const void* p, std::size_t n) { auto q = static_cast<const uint8_t*>(p); b.insert(b.end(), q, q + n); }
    template <class T> void pod(const T& v) { static_assert(std::is_trivially_copyable_v<T>); raw(&v, sizeof v); }
    template <class T> void vec(const std::vector<T>& v) { pod(uint64_t(v.size())); if (!v.empty()) raw(v.data(), v.size() * sizeof(T)); }
};
struct StateIn {
    const uint8_t* p; const uint8_t* end; bool ok = true;
    void raw(void* d, std::size_t n) { if (!ok || std::size_t(end - p) < n) { ok = false; return; } std::memcpy(d, p, n); p += n; }
    template <class T> void pod(T& v) { static_assert(std::is_trivially_copyable_v<T>); raw(&v, sizeof v); }
    template <class T> void vec(std::vector<T>& v, uint64_t limit) {
        uint64_t n = 0; pod(n);
        if (!ok || n > limit || n * sizeof(T) > std::size_t(end - p)) { ok = false; return; }
        v.resize(std::size_t(n)); if (n) raw(v.data(), std::size_t(n) * sizeof(T));
    }
};
}
