#pragma once
#include "base.hpp"

namespace stv {

// Executed MAME 0.289 L8 reference values for differential harnesses only.
// Nothing here is a production default: the shipping game must derive these
// from its own native boot/timing path before a retail room can be playable.

// All 35 HCOUNT (0xC00001C0) reads of paid-start frame 3533, in bus order,
// from two byte-identical fresh-NVRAM runs.
// Source: evidence/l8-paid-hulk-hcount-2026-09-23.json (reads 525..559).
inline constexpr u16 l8_paid_room1_hcount_frame3533[35]={
    0xf1,0x58,0x11,0x44,0x8a,0xee,0x8b,0xf0,0xa7,0x10,0xd7,0x3e,0xd0,0x06,0x3f,0xa3,0x40,0xa6,
    0x5d,0xc2,0x5f,0xc4,0x8d,0xf1,0x8e,0xf3,0x90,0xf4,0x93,0xc6,0x11,0x75,0x3f,0x72,0x2b};

// UTIL.ASM:RAND at the first OHULK draw. MEASURED: the first frame-3533
// RAND_READ in tools/mame_l8_rand_room_entry.lua (headless MAME 0.289 L8,
// evidence/p0-rand-measured-2026-09-24.json) is 0xCBF5010A, and the 34th
// RAND_WRITE of that frame stores 0x1F07E710. This equals the value that
// tools/solve_l8_room1_rand.cpp solved uniquely (up to bit 0) from the
// native OVERLORD/ONSCRN_HULKS/OHULK graph.
inline constexpr u32 l8_paid_room1_rand_measured=0xcbf5010au;
inline constexpr u32 l8_paid_room1_rand_solved=0xcbf5010au;
static_assert(l8_paid_room1_rand_measured==l8_paid_room1_rand_solved);
inline constexpr u32 l8_paid_room1_rand_after_hulks=0x1f07e710u;

// Captured GET_VALID_XY A0/A1 returns, in first-dispatch order (process
// records 0x0108B0B0, 0x0108C190, 0x0108AB10, 0x0108CCD0).
// Source: evidence/l8-hulk-continuation-2026-09-23.json.
struct L8HulkPlacementSample {u32 x{},y{};u16 attempts{};u16 hage{};};
inline constexpr L8HulkPlacementSample l8_paid_room1_hulks[4]={
    {0x005f213du,0x00239d7eu,1,0x012c},{0x008cc8eau,0x0034e483u,4,0x01a4},
    {0x014fb404u,0x002ae0f9u,7,0x005a},{0x006c9eacu,0x00bd1c8du,1,0x00dc}};
// GET_VALID_XY entry A0 on each retry = the rejected candidate X.
inline constexpr u32 l8_paid_room1_rejected_x[9]={
    0x003da3efu,0x0037439cu,0x005f0e6du,
    0x0033d21bu,0x0037435fu,0x007890d8u,0x005f0d7du,0x00c523a5u,0x00744d19u};

}
