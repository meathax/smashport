# Source provenance and third-party notices

## Supplied historical Williams source and graphics

The original source includes: **COPYRIGHT (C) 1989 WILLIAMS ELECTRONICS GAMES, INC.** Original headers and files remain unchanged in `original/archive/smashtv-main`. The uploaded ZIP is identified by SHA-256 in `SOURCE_AUDIT.md`.

Native gameplay, process/object/animation routines and compiled data tables in this checkpoint are derived from supplied original game source. `data/source.stva`, diagnostic renders and generated animation tables contain or derive from supplied original graphics/game data. Public repository availability or possession of this upload is not treated as a redistribution licence. Exact copyright/licensing permissions have not been established. This is a private development handoff to the supplying user, not an authorised commercial release. No repository publication or public release was performed.

Historical repository attribution: https://github.com/historicalsource/smashtv . No exact Git commit is asserted from this archive.

## MAME 0.289 (reference behaviour, BSD-3-Clause)

MAME 0.289 is the reference model (PLAN D4). It is never bundled, linked or invoked by `SmashTV.exe`; it runs only in development tools (`tools/mame_run.ps1`, `tools/lockstep/*.lua`). Native code reproduces the behaviour of these MAME source files, which are licensed BSD-3-Clause:

- `src/devices/cpu/tms34010/*` (Alex Pasadyn, Zsolt Vasvari) and `src/mame/midway/midyunit*` (Alex Pasadyn, Zsolt Vasvari, Ernesto Corvi, Aaron Giles): main CPU cycle rules, Y-unit video, DMA and I/O.
- `src/devices/cpu/m6809/m6809.lst`, `base6x09.lst`, `m6809.cpp`, `m6809make.py` (Nathan Woods): sound CPU microcode order, cycles and suspend points, translated per instruction by `tools/m6809_aot/emit6809.py`.
- `src/devices/machine/6821pia.cpp` (Aaron Giles, Nathan Woods), `src/devices/sound/hc55516.cpp` (Aaron Giles, Jonathan Gevaryahu), `src/devices/sound/dac.cpp` (smf), `src/mame/shared/williamssound.cpp` and `src/devices/sound/ymfm_mame.h` (Aaron Giles), `src/emu/schedule.cpp`, `diexec.cpp` and `sound.cpp` (Aaron Giles): sound board devices and scheduler semantics in `src/snd/*` and `src/game/cosched.cpp`.

BSD-3-Clause terms: redistribution in source or binary form must retain the copyright notice, the licence conditions and the disclaimer; contributors' names may not be used to endorse derived products without permission. The software is provided "as is", without warranty.

## ymfm (vendored, BSD-3-Clause)

`third_party/ymfm` is an unmodified copy of ymfm from MAME 0.289 (`3rdparty/ymfm`), Copyright (c) 2021, Aaron Giles. Its licence is in `third_party/ymfm/LICENSE` and must accompany any binary that contains it (`SmashTV.exe` does). Provenance and file hashes: `third_party/ymfm/PROVENANCE.md`.

## Build and runtime dependencies

`SmashTV.exe` imports kernel32.dll, user32.dll, gdi32.dll, comdlg32.dll, ole32.dll, shell32.dll ws2_32.dll, d3d12.dll, dxgi.dll and d3dcompiler_47.dll, uses the Windows audio endpoint (WASAPI) through COM, and optionally loads xinput1_4.dll from the Windows system directory. These operating-system DLLs are not redistributed. MSVC, CMake, Ninja and Python are development tools, not player requirements. No SDL, emulator, interpreter or Python runtime is bundled.

ZIP/DEFLATE/hash helpers are checkpoint implementations; independent development tests use Python's standard library. Synthetic importer fixtures are not original game ROMs. Original source graphics remain segregated in the data directory and unchanged original tree. Any future distribution must address original-source, generated-data, graphics and sound rights separately.

## TMNT native port frontend (adapted, BSD-3-Clause)

The native Windows PC frontend adapts generic menu navigation, local preferences,
bounded player-name editing, presentation-filter ideas/constants and output-side
volume handling from [cheviman/tmnt-port](https://github.com/cheviman/tmnt-port) at
commit `401c3694e5f89fa5844c384cfd82512803202cf9`, Copyright (c) 2026, Sebastian
Toyos. Exact upstream files, hashes and adaptation scope are recorded in
`third_party/tmnt-port/PROVENANCE.json`. The complete unmodified BSD-3-Clause
license is retained in `third_party/tmnt-port/LICENSE` and accompanies the binary
as `licenses/tmnt-port-LICENSE.txt`.

The adaptation uses the existing Win32/XInput/WASAPI host and adds no SDL runtime.
It preserves Smash TV's two-player model, original arcade menus and fixed shared
16:9 player profile. TMNT game data, ROMs, assets, game-specific addresses and
gameplay networking were not imported.

## arcade-lan room discovery (adapted, BSD-3-Clause)

The separate two-player room discovery, bounded names, membership lifecycle and
reliable start-handoff module adapts
[cheviman/arcade-lan](https://github.com/cheviman/arcade-lan) at commit
`a7a15483521d59fec74177d26f6bd438ac8dcaa4`, the submodule pinned by the TMNT
commit above, Copyright (c) 2026, Sebastian Toyos. Exact upstream file identities
and the local adaptation are recorded in
`third_party/arcade-lan-lobby/PROVENANCE.json`. The complete unmodified
BSD-3-Clause license is retained in `third_party/arcade-lan-lobby/LICENSE` and
accompanies the binary as `licenses/arcade-lan-lobby-LICENSE.txt`.

This lobby controls discovery and connection handoff around Smash TV's existing
authoritative handshake, input transport and rollback ownership. It does not
replace the game protocol or import the donor's four-player gameplay model.


## Optional GPU picture processing (11 November 2026)

The private GDL DECOMP standalone post-processing donor is recorded byte-for-byte
in `third_party/graphics/PROVENANCE.json`. Its upstream dependencies are
librashader 0.12.0 (MPL-2.0; C headers MIT), DirectX Shader Compiler v1.9.2609
(LLVM/NCSA and MIT), CRT Guest Advanced and CRT Royale (GPL-2.0-or-later), and
CRT Lottes (public domain). The copied post-chain and the live D3D11 presenter
contain a native port of
[Blur Busters CRT Beam Simulator](https://github.com/blurbusters/crt-beam-simulator)
at commit `734786a6c48f954af11cb390e38a9e06107ffdd9`,
Copyright (c) 2024 Mark Rejhon (@BlurBusters) and Timothy Lottes
(@NOTimothyLottes), under the MIT licence. Exact hashes and the complete
licence are retained under `third_party/graphics/runtime/post/blurbusters/`.
The live player adds no window, game or frame-history code from that repository.
The same post chain contains an AMD CAS-inspired formula.
Full runtime, shader and donor notices and source/build pins are retained in
`third_party/graphics/` and copied to the staged `licenses/` and `shaders/` trees.
The shader subset is pinned to slang-shaders commit
`2345ee91602a7404d3a1a6a5df6108d7305fdad4`. No source ROM or gameplay code comes
from that donor. This remains a private build, not a redistribution approval.
