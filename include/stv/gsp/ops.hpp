#pragma once
// Helpers used by generated L8 translation units (tools/gsp2cpp/emit.py).
#include "stv/gsp/machine.hpp"

namespace stv::gsp {

using ChunkFn = u32 (*)(Machine&, u32 pc);

// Generated: pc -> chunk function lookup for the L8 program (0 = not code).
ChunkFn l8_chunk_for(u32 pc);

inline u32 rf(Machine& m, u32 a, int f) { return rfield_ext(m, m.c, a, f); }
inline void wf(Machine& m, u32 a, u32 v, int f) { wfield(m, a, v, fw(m.c, f)); }
inline u32 rb(Machine& m, u32 a) { return rfield(m, a, 8); }
inline void wb(Machine& m, u32 a, u32 v) { wfield(m, a, v, 8); }

// TMS34010 graphics instructions (src/gsp/gfx.cpp)
void gsp_cvxyl(Machine& m, u32& rd, u32 rs);
void gsp_pixt_ri(Machine& m, u32 rs, u32 rd);
void gsp_pixt_rixy(Machine& m, u32 rs, u32 rdxy);
void gsp_pixt_ir(Machine& m, u32 rs, u32& rd);
void gsp_pixt_ii(Machine& m, u32 rs, u32 rd);
void gsp_pixt_ixyr(Machine& m, u32 rsxy, u32& rd);
void gsp_pixt_ixyixy(Machine& m, u32 rsxy, u32 rdxy);
void gsp_drav(Machine& m, u32 rs, u32& rd);
u32 gsp_fill(Machine& m, bool linear, u32 self, u32 next);
u32 gsp_pixblt_b(Machine& m, bool linear, u32 self, u32 next);

// Run translated code until the cycle budget is reached or a fault occurs.
// Returns the PC to resume from.
u32 l8_run(Machine& m, u32 pc, u64 until_cycle);
// Executed-instruction flags, one per row of l8_translation_index.csv (generated).
extern u8 l8_cov[];
extern const u32 l8_cov_count;

} // namespace stv::gsp
