#pragma once
#include "stv/gsp/core.hpp"
#include <algorithm>
#include <iterator>

namespace stv::gsp {
// Selected L8 LISTWHAT firing animations, including recoil and alternate ANI pivots.
// Coordinates name an occupied texel of the unflipped image, before clipping.
struct MuzzleCel { u32 image; int x,y; };
inline constexpr MuzzleCel muzzle_cels[] = {
    {0xfffa9dc0, 0, 17},
    {0xfffa9e20, 0, 0},
    {0xfffa9e80, 13, 0},
    {0xfffa9ee0, 0, 15},
    {0xfffa9f40, 0, 0},
    {0xfffa9fa0, 12, 0},
    {0xfffaa000, 1, 12},
    {0xfffaa060, 4, 4},
    {0xfffaa0c0, 7, 6},
    {0xfffaa120, 0, 13},
    {0xfffaa180, 5, 0},
    {0xfffaa1e0, 14, 0},
    {0xfffaa240, 2, 11},
    {0xfffaa2a0, 1, 2},
    {0xfffaa300, 11, 0},
    {0xfffaa360, 12, 0},
    {0xfffaa3c0, 2, 11},
    {0xfffaa420, 1, 2},
    {0xfffaa480, 10, 0},
    {0xfffaa4e0, 12, 0},
    {0xfffaa720, 6, 21},
    {0xfffaa780, 0, 0},
    {0xfffaa7e0, 6, 0},
    {0xfffaa840, 0, 4},
    {0xfffaa8a0, 0, 5},
    {0xfffaa900, 0, 4},
    {0xfffaa960, 0, 5},
    {0xfffaa9c0, 0, 17},
    {0xfffaaa20, 0, 16},
    {0xfffaaa80, 0, 17},
    {0xfffaaae0, 0, 16},
    {0xfffaab40, 4, 24},
    {0xfffaaba0, 4, 20},
    {0xfffaac00, 4, 24},
    {0xfffaac60, 4, 20},
    {0xfffaacc0, 0, 6},
    {0xfffaad20, 0, 6},
    {0xfffaad80, 0, 6},
    {0xfffaade0, 0, 6},
    {0xfffaae40, 1, 12},
    {0xfffaaea0, 0, 6},
    {0xfffaaf00, 1, 12},
    {0xfffaaf60, 0, 6},
    {0xfffaafc0, 1, 12},
    {0xfffab020, 0, 2},
    {0xfffab080, 1, 12},
    {0xfffab0e0, 0, 2},
    {0xfffab140, 0, 1},
    {0xfffab1a0, 0, 1},
    {0xfffab200, 0, 1},
    {0xfffab260, 0, 1},
    {0xfffab3c0, 6, 18},
    {0xfffab420, 0, 0},
    {0xfffab480, 6, 0},
};
inline const MuzzleCel* muzzle_cel(u32 image) {
    auto it=std::lower_bound(std::begin(muzzle_cels),std::end(muzzle_cels),image,
        [](const MuzzleCel& cel,u32 address){return cel.image<address;});
    return it!=std::end(muzzle_cels) && it->image==image ? it : nullptr;
}
}
