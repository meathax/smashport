# Pins for the post-processing chain (CRT shaders, HDR)

Downloaded 2026-11-11 under the user's approval of that date, only from the
official sources named below. `third_party/librashader/` and
`third_party/slang-shaders/` are ignored by git (size); restore them from
these pins.

## librashader (shader runtime)

| Item | Value |
|---|---|
| Upstream | https://github.com/SnowflakePowered/librashader |
| Version | `librashader-v0.12.0` (tag object `5537e838fc4680f5a9b2c0f4b9b669edea6e981b`, commit `87e8a97b50516d997defeaa168173dcd185d4022`), released 2026-07-04 |
| What | **Built from source** on 2026-11-11 with only the Direct3D 12 runtime and a static C runtime |
| Licence | MPL-2.0 (or GPL-3.0 at the distributor's choice): `LICENSE.md`, `LICENSE-GPL.md`. `librashader_ld.h` and the C API definitions are MIT. |
| ABI / API | C ABI 2, API 5 (checked at load; `gdl_post_test --selftest` prints it) |

Build recipe (rustc 1.97.1, cargo 1.97.1, host `x86_64-pc-windows-msvc`; 164
crates from crates.io as locked by the repository's `Cargo.lock`, SHA-256
`dcec8cd616d9572ff347127fd4026f23ee2a8d881ec02027e733675e106a2661`; 2 min 18 s):

```bash
git clone --depth 1 --branch librashader-v0.12.0 https://github.com/SnowflakePowered/librashader lrs_git
cd lrs_git
RUSTFLAGS="-C target-feature=+crt-static" CARGO_TARGET_DIR=../lrs_target \
  cargo build -p librashader-capi --profile optimized --no-default-features --features runtime-d3d12
cp ../lrs_target/optimized/librashader_capi.dll <project>/third_party/librashader/bin/librashader.dll
```

No source change. The official release build differs only in features
(all runtimes) and the dynamic C runtime; the file is renamed the same way.
The build is not bit-reproducible (not attempted); the hash below is of the
file produced here.

| File (under `third_party/librashader/`) | SHA-256 |
|---|---|
| `bin/librashader.dll` (14,295,040 bytes, source build) | `8e5912fc7432ff39e935c61674823f37b96a824bb6c727f1319cc31fc561a74a` |
| `include/librashader.h` | `a20da26f079e7e15e4736368fa553bb94dd4cc6039078d0af81cd5427bae5393` |
| `include/librashader_ld.h` | `bcffcbc854afb287c9f935c1a0e3b569f5e6775ef85914bd7f7025ad1f6bde33` |
| `include/README.md` | `f2b103e6d0dbff9ea3cebe848f3b10c099215231a3d5edc99fa1fa2b9bba13a3` |
| `LICENSE.md` | `69c15395f33bc9ce8e1d8b6cef42b7e49cdec4c6f5233d4b9cfc4bfa335f97f9` |
| `LICENSE-GPL.md` | `831e92808eb553250066261a1387a9323cbf300d4559a41f2c71fd3712bdb44a` |

The headers and licence files come from the repository at the same tag.

Dependencies of the source-built DLL, from its import table (`llvm-objdump -p`):
`kernel32.dll`, `ntdll.dll`, `bcryptprimitives.dll`,
`api-ms-win-core-synch-l1-2-0.dll`, `oleaut32.dll`, `combase.dll`,
`shell32.dll`: Windows components only. **No `D3DX9_43.dll`, no Visual C++
redistributable, no `d3dcompiler_47.dll`.** At run time it loads
`dxcompiler.dll` by name (the only such string in the binary). `dxil.dll` is
not needed: with an empty shader cache and no `dxil.dll` beside it,
`crt-lottes.slangp` built and produced the same output hash
(`03de75af68037d16`) as with it.

For comparison, the official prebuilt
`librashader-x86_64-windows-v0.12.0-optimized.zip` (SHA-256
`521fe0f364bfa705883f9e99fb1733a3a24f0f403bcc5591b6b78f6cff289183`, DLL
`1890f647c7fbe52d4cc591526db24367caca284996855c4565c6003c7e46f8cc`), used in
the first session, imports `D3DX9_43.dll`, `d3dcompiler_47.dll`,
`MSVCP140.dll`, `VCRUNTIME140.dll` and `VCRUNTIME140_1.dll`. It is no longer
in the tree.

### What a release must ship for the post chain

| File | From | Licence file to ship with it |
|---|---|---|
| `librashader.dll` | `third_party/librashader/bin/` | `LICENSE.md` (MPL-2.0); offer the source: the tag above, unmodified |
| `dxcompiler.dll` | `third_party/librashader/dxc/` | `LICENSE-LLVM.txt`, `LICENSE-MS.txt`, `LICENCE-MIT.txt` |
| `presets/` and `shaders/` | output of `tools/post/make_shader_package.py` (9.8 MB) | inside it: `shaders/SOURCES.md`, `crt/shaders/crt-royale/LICENSE.TXT` (GPL-2.0), Mega Bezel `README.md`; GPL-3.0 text is `LICENSE-GPL.md` of librashader |

Both DLLs go next to the executable (or into the folder given to the
`PostChain` constructor). Not shipped: `dxil.dll`, the 129 MB slang-shaders
tree, anything of Sony Megatron. Without `librashader.dll` or
`dxcompiler.dll` the module still runs every native stage (scaling, beam
simulator, adjustments, curvature, HDR encode) and `set_preset()` returns
"shader presets are unavailable: librashader.dll not found or not loadable
(Windows error 126)" (checked with `--runtime-dir` pointing at an empty
folder). The executable itself needs `d3d12.dll`, `dxgi.dll` and
`d3dcompiler_47.dll`, all part of Windows 10/11.

## DirectX Shader Compiler (run-time prerequisite of the librashader D3D12 runtime)

| Item | Value |
|---|---|
| Upstream | https://github.com/microsoft/DirectXShaderCompiler |
| Version | release `v1.9.2609`, asset `dxc_2026_09_29.zip` |
| Asset SHA-256 | `ad31b1fc8443175d204f77a611fdb3ef2ec42759bdc2f1167368de24a4a7e7f1` |
| Licence | University of Illinois / NCSA (LLVM) and MIT: `LICENSE-LLVM.txt`, `LICENSE-MS.txt`, `LICENCE-MIT.txt` |

Kept (from `bin/x64/` of the asset) in `third_party/librashader/dxc/` (`dxil.dll` is kept but not needed and not copied by the build):

| File | SHA-256 |
|---|---|
| `dxcompiler.dll` | `0689e2fee0a1c9df8662a59680469f24e2a3ad7ce6bbbcc9284f7a83172bfc59` |
| `dxil.dll` | `64be2368211d257a038a7d4ea6bb3e24c878eb8d588c7b8414fccf759b69c2ac` |
| `LICENSE-LLVM.txt` | `729615317e28dd03907e46f0fc3b5e88f7853cee61d1a1471d2749335516b46f` |
| `LICENSE-MS.txt` | `734f72f239fe7b07b4c7203f294c1a7ce27095687278bab7e56d630d7c672963` |
| `LICENCE-MIT.txt` | `903df5512f7d02609fed0c780a9b704f5a3eeb6e4d84ebe42a29845c81899a3c` |

## slang-shaders (preset collection, data only)

| Item | Value |
|---|---|
| Upstream | https://github.com/libretro/slang-shaders |
| Commit | `2345ee91602a7404d3a1a6a5df6108d7305fdad4` (2026-11-08, branch `master`) |
| Size | 129 MB working tree including a 50 MB shallow `.git`; 5,258 files |
| Licence | Per shader (see `docs/post_chain.md`); most CRT presets are GPL. The repository has no top-level licence file. |

Restore:

```bash
mkdir third_party/slang-shaders && cd third_party/slang-shaders
git init -q && git remote add origin https://github.com/libretro/slang-shaders
git fetch --depth 1 origin 2345ee91602a7404d3a1a6a5df6108d7305fdad4 && git checkout FETCH_HEAD
```

No local changes (`git status` clean). The project presets in `presets/`
refer to it with `#reference "../third_party/slang-shaders/..."`.

## Project presets

| File | SHA-256 |
|---|---|
| `presets/arcade_medium_res.slangp` | `8cbe4157740588412d74e5651d987e6c8c3cc0e6123e2032bc0587d8c3360152` |
| `presets/arcade_medium_res_1080p.slangp` | `0fcfbc88a3e17fed6f93ffa3ceb8e32418e4a7579ee10eb09306c72a4351226d` |
| `presets/arcade_medium_res_hdr.slangp` (CRT-Guest-Advanced; no longer Megatron) | `34e965838efd1de7d657c25c09e2764e6e9e31512da9ced9f943c02adce48002` |
