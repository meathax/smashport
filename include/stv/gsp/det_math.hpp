// Deterministic sine, cosine and arc tangent for enhanced game-state math.
//
// Online co-op compares RAM hashes between two PCs. The C runtime's sin, cos
// and atan2 come from the system UCRT, whose results can differ in the last
// bit between Windows builds and CPU feature paths (FMA). These ports of the
// fdlibm/FreeBSD algorithms use only IEEE-754 +, -, * and /, which SSE2
// evaluates identically everywhere. Fused multiply-add contraction is disabled
// below; MSVC does not contract under its default /fp:precise.
//
// Domain: finite arguments of moderate magnitude (|x| < 2^19 * pi/2 for sin and cos).
#pragma once

#include <cstdint>
#include <cstring>

#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace stv::gsp::det {

namespace detail {
inline std::uint32_t high_word(double x) {
    std::uint64_t bits;
    std::memcpy(&bits, &x, sizeof bits);
    return std::uint32_t(bits >> 32);
}

// FreeBSD k_sin.c: sin on [-pi/4, pi/4]; y is the tail of x.
inline double kernel_sin(double x, double y, bool tail) {
    constexpr double S1 = -1.66666666666666324348e-01, S2 = 8.33333333332248946124e-03,
                     S3 = -1.98412698298579493134e-04, S4 = 2.75573137070700676789e-06,
                     S5 = -2.50507602534068634195e-08, S6 = 1.58969099521155010221e-10;
    const double z = x * x, w = z * z;
    const double r = S2 + z * (S3 + z * S4) + z * w * (S5 + z * S6);
    const double v = z * x;
    if (!tail) return x + v * (S1 + z * r);
    return x - ((z * (0.5 * y - v * r) - y) - v * S1);
}

// FreeBSD k_cos.c: cos on [-pi/4, pi/4]; y is the tail of x.
inline double kernel_cos(double x, double y) {
    constexpr double C1 = 4.16666666666666019037e-02, C2 = -1.38888888888741095749e-03,
                     C3 = 2.48015872894767294178e-05, C4 = -2.75573143513906633035e-07,
                     C5 = 2.08757232129817482790e-09, C6 = -1.13596475577881948265e-11;
    const double z = x * x, w = z * z;
    const double r = z * (C1 + z * (C2 + z * C3)) + w * w * (C4 + z * (C5 + z * C6));
    const double hz = 0.5 * z, one_minus = 1.0 - hz;
    return one_minus + (((1.0 - one_minus) - hz) + (z * r - x * y));
}

// Cody-Waite reduction by pi/2 (FreeBSD e_rem_pio2.c medium path, first round).
inline int reduce(double x, double& y0, double& y1) {
    constexpr double inv_pio2 = 6.36619772367581382433e-01, pio2_1 = 1.57079632673412561417e+00,
                     pio2_1t = 6.07710050650619224932e-11;
    const double scaled = x * inv_pio2;
    const int n = int(scaled + (scaled < 0 ? -0.5 : 0.5));
    const double fn = double(n);
    const double r = x - fn * pio2_1, w = fn * pio2_1t;
    y0 = r - w;
    y1 = (r - y0) - w;
    return n;
}
} // namespace detail

inline double sin(double x) {
    if ((detail::high_word(x) & 0x7fffffffu) <= 0x3fe921fbu) return detail::kernel_sin(x, 0.0, false);
    double y0, y1;
    switch (detail::reduce(x, y0, y1) & 3) {
    case 0: return detail::kernel_sin(y0, y1, true);
    case 1: return detail::kernel_cos(y0, y1);
    case 2: return -detail::kernel_sin(y0, y1, true);
    default: return -detail::kernel_cos(y0, y1);
    }
}

inline double cos(double x) {
    if ((detail::high_word(x) & 0x7fffffffu) <= 0x3fe921fbu) return detail::kernel_cos(x, 0.0);
    double y0, y1;
    switch (detail::reduce(x, y0, y1) & 3) {
    case 0: return detail::kernel_cos(y0, y1);
    case 1: return -detail::kernel_sin(y0, y1, true);
    case 2: return -detail::kernel_cos(y0, y1);
    default: return detail::kernel_sin(y0, y1, true);
    }
}

// FreeBSD s_atan.c.
inline double atan(double x) {
    constexpr double hi[] = {4.63647609000806093515e-01, 7.85398163397448278999e-01,
                             9.82793723247329054082e-01, 1.57079632679489655800e+00};
    constexpr double lo[] = {2.26987774529616870924e-17, 3.06161699786838301793e-17,
                             1.39033110312309984516e-17, 6.12323399573676603587e-17};
    constexpr double T[] = {3.33333333333329318027e-01, -1.99999999998764832476e-01,
                            1.42857142725034663711e-01, -1.11111104054623557880e-01,
                            9.09088713343650656196e-02, -7.69187620504482999495e-02,
                            6.66107313738753120669e-02, -5.83357013379057348645e-02,
                            4.97687799461593236017e-02, -3.65315727442169155270e-02,
                            1.62858201153657823623e-02};
    const std::uint32_t hx = detail::high_word(x), ix = hx & 0x7fffffffu;
    const bool negative = (hx >> 31) != 0;
    if (ix >= 0x44100000u) return negative ? -(hi[3] + lo[3]) : hi[3] + lo[3];   // |x| >= 2^66
    int id;
    if (ix < 0x3fdc0000u) {                       // |x| < 0.4375
        if (ix < 0x3e400000u) return x;           // |x| < 2^-27
        id = -1;
    } else {
        x = negative ? -x : x;
        if (ix < 0x3ff30000u) {                   // |x| < 1.1875
            if (ix < 0x3fe60000u) { id = 0; x = (2.0 * x - 1.0) / (2.0 + x); }
            else { id = 1; x = (x - 1.0) / (x + 1.0); }
        } else if (ix < 0x40038000u) { id = 2; x = (x - 1.5) / (1.0 + 1.5 * x); }   // |x| < 2.4375
        else { id = 3; x = -1.0 / x; }
    }
    const double z = x * x, w = z * z;
    const double s1 = z * (T[0] + w * (T[2] + w * (T[4] + w * (T[6] + w * (T[8] + w * T[10])))));
    const double s2 = w * (T[1] + w * (T[3] + w * (T[5] + w * (T[7] + w * T[9]))));
    if (id < 0) return x - x * (s1 + s2);
    const double result = hi[id] - ((x * (s1 + s2) - lo[id]) - x);
    return negative ? -result : result;
}

// FreeBSD e_atan2.c for finite arguments.
inline double atan2(double y, double x) {
    constexpr double pi = 3.1415926535897931160e+00, pi_o_2 = 1.5707963267948965580e+00,
                     pi_lo = 1.2246467991473531772e-16;
    const bool ny = (detail::high_word(y) >> 31) != 0, nx = (detail::high_word(x) >> 31) != 0;
    if (y == 0.0) return nx ? (ny ? -pi : pi) : y;
    if (x == 0.0) return ny ? -pi_o_2 : pi_o_2;
    const int ex = int((detail::high_word(x) >> 20) & 0x7ff), ey = int((detail::high_word(y) >> 20) & 0x7ff);
    double z;
    if (ey - ex > 60) z = pi_o_2 + 0.5 * pi_lo;
    else if (nx && ey - ex < -60) z = 0.0;
    else { const double ratio = y / x; z = atan(ratio < 0 ? -ratio : ratio); }
    if (!nx) return ny ? -z : z;
    return ny ? (z - pi_lo) - pi : pi - (z - pi_lo);
}

} // namespace stv::gsp::det

#if defined(__clang__)
#pragma clang fp contract(on)
#endif
