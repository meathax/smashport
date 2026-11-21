"""Build a per-user offline Windows installer from a fresh, explicitly selected payload."""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
HOST = """base zip gsp/machine gsp/gfx gsp/run gsp/state snd/board snd/audio snd/voices game/cosched
net/rollback net/udp net/lobby host/pc_menu host/pc_settings host/video_filter
host/enhance host/gpu_effects host/ray_scene host/room_lighting host/gpu_resident host/dxr_lighting host/gpu_filter
host/win_main host/wasapi host/audio_fx""".split()
GENERATOR = """tools/player/prepare.py tools/player/l8_locations.json tools/gsp2cpp/compare.py
tools/gsp2cpp/codemap.py tools/gsp2cpp/decode.py tools/gsp2cpp/retail_tables.py
tools/gsp2cpp/emit.py tools/gsp2cpp/isa_table.json
tools/m6809_aot/codemap6809.py tools/m6809_aot/decode6809.py tools/m6809_aot/emit6809.py
docs/reference-rom-profile.json src/gsp/enhanced.cpp src/net/handshake.cpp""".split()
GRAPHICS = ROOT / "third_party/graphics"


def sha(path):
    with path.open("rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()


def copy(src, dst):
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(src, dst)


def write(path, content):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf8")


def project_metadata(stage):
    def sanitize(row):
        if isinstance(row, list):
            return [sanitize(v) for v in row]
        if not isinstance(row, dict):
            return row
        out = {}
        for k, v in row.items():
            if k in ("original", "source", "purpose", "note", "role", "description"):
                continue
            if k in ("instruction", "operands"):
                out[k + "_sha256"] = hashlib.sha256(json.dumps(v, separators=(",", ":")).encode()).hexdigest()
            elif k == "budget" and v == row.get("original", [None, None])[1]:
                out[k] = None
            else:
                out[k] = sanitize(v)
        return out
    for stem in ("stage_balance", "data_roots", "patchpoints", "balance", "scene_writes"):
        name = Path("data") / ("enhanced_" + stem + ".json")
        write(stage / name, json.dumps(sanitize(json.loads((ROOT / name).read_bytes()))))


def host_objects(stage, temp, tc, eos, dxc, sdl):
    cpp = tc / "bin/clang++.exe"
    gen = temp / "shaders"
    gen.mkdir()
    write(gen / "resident_effects_shader.hpp",
          (ROOT / "src/host/resident_effects_shader.hpp.in").read_text().replace(
          "@STV_EFFECTS_HLSL@", (ROOT / "src/host/resident_effects.hlsl").read_text()))
    subprocess.run([dxc, "-T", "cs_6_5", "-E", "main", "-O3", "-Fh", gen / "dxr_shader.hpp",
                    "-Vn", "stv_dxr_shader", ROOT / "src/host/dxr_lighting.hlsl"], check=True)
    includes = [ROOT / "include", ROOT / "third_party/ymfm", GRAPHICS / "runtime/post",
                GRAPHICS / "third_party/librashader/include", gen]
    achievements = ROOT / "third_party/recomp_achievements"
    includes.append(achievements / "include")
    if sdl:
        includes.append(sdl / "include")
    if eos:
        includes.append(eos / "SDK/Include")
    # The MinGW Windows headers stop before D3D11.4 even though current Windows
    # DXR hosts use ID3D11Device5 fencing. Prefer MinGW's compatible base
    # declarations, then fall through to the installed SDK for newer headers.
    sdk_after = []
    resolved_dxc = dxc.resolve()
    if (resolved_dxc.name.lower() == "dxc.exe" and
            len(resolved_dxc.parents) >= 4 and
            resolved_dxc.parents[0].name.lower() in ("x64", "x86", "arm64")):
        sdk_root = resolved_dxc.parents[3] / "Include" / resolved_dxc.parents[1].name
        sdk_after = [sdk_root / name for name in ("um", "shared") if (sdk_root / name).is_dir()]
    flags = ["-std=c++20", "-O2", "-DNDEBUG", "-DNOMINMAX", "-DWIN32_LEAN_AND_MEAN", "-DUNICODE",
             "-D_UNICODE", "-DWINVER=0x0a00", "-D_WIN32_WINNT=0x0a00",
             "-DSTV_HAS_EOS=" + ("1" if eos else "0"), "-DSTV_PLAYER_RELEASE=1",
             "-DSTV_RECOMP_ACHIEVEMENTS"]
    if sdl:
        flags.append("-DSTV_SDL_PADS")
    owners = HOST + (["net/epic_rooms"] if eos else [])
    sources = [ROOT / "src" / (n + ".cpp") for n in owners] + [
        ROOT / "third_party/ymfm/ymfm_opm.cpp", GRAPHICS / "runtime/post/post_chain.cpp"]
    (stage / "host").mkdir()
    def compile_one(src):
        subprocess.run([cpp, *flags, *["-I" + str(p) for p in includes],
                        *["-idirafter" + str(p) for p in sdk_after], "-c", src,
                        "-o", stage / "host" / (src.stem + ".o")], check=True)
    with concurrent.futures.ThreadPoolExecutor(4) as pool:
        list(pool.map(compile_one, sources))
    rc = achievements / "third_party/rcheevos"
    cjson = achievements / "third_party/cjson"
    c_sources = [achievements / "src/rl_achievements.c", cjson / "cJSON.c",
                 *sorted((rc / "src/runtime").glob("*.c")), *sorted((rc / "src/util").glob("*.c"))]
    for source in c_sources:
        subprocess.run([cpp, "-x", "c", "-std=gnu99",
                        "-O2", "-DRC_DISABLE_LUA", "-I", achievements / "include",
                        "-I", rc / "include", "-I", rc / "src", "-I", cjson,
                        "-c", source, "-o", stage / "host" / ("ach_" + source.stem + ".o")], check=True)
    copy(achievements / "LICENSE", stage / "runtime/licenses/RecompAchievements.txt")
    copy(rc / "LICENSE", stage / "runtime/licenses/rcheevos.txt")
    copy(cjson / "LICENSE", stage / "runtime/licenses/cJSON.txt")
    if sdl:
        copy(sdl / "lib/libSDL3.dll.a", stage / "host/sdl.lib")
        copy(sdl / "bin/SDL3.dll", stage / "runtime/SDL3.dll")
        sdl_license = sdl / "LICENSE.txt"
        if not sdl_license.is_file():
            sdl_license = sdl.parent / "LICENSE.txt"
        copy(sdl_license, stage / "runtime/licenses/SDL3.txt")
    if eos:
        copy(eos / "SDK/Lib/EOSSDK-Win64-Shipping.lib", stage / "host/eos.lib")


def toolchain(stage, tc):
    llvm = stage / "llvm"
    # The .cfg files select libc++, compiler-rt, libunwind and lld for the
    # MinGW target. Without them clang searches for GNU libstdc++ headers and
    # the end user's first setup cannot compile the translated game.
    for name in ("clang++.exe", "clang-22.exe", "ld.lld.exe", "libLLVM-22.dll", "libclang-cpp.dll",
                 "libc++.dll", "libunwind.dll",
                 "x86_64-w64-windows-gnu.cfg", "mingw32-common.cfg"):
        copy(tc / "bin" / name, llvm / "bin" / name)
    pthread = tc / "bin/libwinpthread-1.dll"
    if pthread.is_file():
        copy(pthread, llvm / "bin/libwinpthread-1.dll")
    for name in ("include", "lib/clang/22/include", "lib/clang/22/lib/windows",
                 "x86_64-w64-mingw32/lib", "x86_64-w64-mingw32/share/mingw32"):
        shutil.copytree(tc / name, llvm / name)
    copy(tc / "LICENSE.TXT", llvm / "LICENSE.TXT")
    py = tc / "python"
    for name in ("python.exe", "python312.dll", "python3.dll", "vcruntime140.dll",
                 "vcruntime140_1.dll", "LICENSE.txt"):
        source = py / name
        if not source.is_file() and name in ("python3.dll", "vcruntime140_1.dll"):
            continue
        copy(source, stage / "python" / name)
    for name in ("libcrypto-3-x64.dll", "_hashlib.pyd", "_bz2.pyd", "_lzma.pyd", "select.pyd", "_socket.pyd"):
        source = py / "DLLs" / name
        if source.is_file():
            copy(source, stage / "DLLs" / name)
    for src in (py / "Lib").rglob("*.py"):
        rel = src.relative_to(py)
        if not set(rel.parts) & {"site-packages", "__pycache__", "test", "tests", "idlelib", "tkinter", "ensurepip", "venv"}:
            copy(src, stage / rel)


def runtime(stage, eos, config):
    rt = stage / "runtime"
    licenses = {"librashader-MPL.txt":"third_party/librashader/LICENSE.md",
                "librashader-GPL.txt":"third_party/librashader/LICENSE-GPL.md",
                "blurbusters-MIT.txt":"runtime/post/blurbusters/LICENSE",
                "PINS_crt.md":"third_party/PINS_crt.md", "PROVENANCE.json":"PROVENANCE.json",
                "LICENSE-LLVM.txt":"third_party/librashader/dxc/LICENSE-LLVM.txt",
                "LICENSE-MS.txt":"third_party/librashader/dxc/LICENSE-MS.txt",
                "LICENCE-MIT.txt":"third_party/librashader/dxc/LICENCE-MIT.txt",
                "librashader-d3d11-PROVENANCE.json":"librashader-d3d11/PROVENANCE.json"}
    binaries = {"librashader_d3d11.dll":"librashader-d3d11/librashader_d3d11.dll",
                "librashader.dll":"third_party/librashader/bin/librashader.dll",
                "dxcompiler.dll":"third_party/librashader/dxc/dxcompiler.dll"}
    for row in json.loads((ROOT / "tools/release/graphics-files.json").read_bytes())["files"]:
        n = row["path"]
        src = GRAPHICS / (licenses[Path(n).name] if n.startswith("licenses/") else binaries.get(n,n))
        if sha(src) != row["sha256"]:
            raise ValueError("Graphics inventory mismatch: " + n)
        copy(src, rt / n)
    if eos:
        keys = dict(line.split("=", 1) for line in config.read_text().splitlines() if "=" in line)
        required = {"ProductId", "SandboxId", "DeploymentId", "ClientId", "ClientSecret"}
        if set(keys) != required or not all(keys.values()):
            raise ValueError("Expected only the five EOS game-client settings")
        copy(config, rt / "eos-client.ini")
        copy(eos / "SDK/Bin/EOSSDK-Win64-Shipping.dll", rt / "EOSSDK-Win64-Shipping.dll")
        copy(eos / "ThirdPartyNotices/ThirdPartySoftwareNotice.txt", rt / "licenses/EOS.txt")
    for lib in ("ymfm", "tmnt-port", "arcade-lan-lobby"):
        copy(ROOT / "third_party" / lib / "LICENSE", rt / "licenses" / (lib + ".txt"))
    copy(ROOT / "THIRD_PARTY_NOTICES.md", rt / "licenses/THIRD_PARTY_NOTICES.md")


def check_host_sources(eos):
    """Every player host/net source must be precompiled into the kit; a new file
    left out of HOST would only fail at the end user's first-run link."""
    compiled = set(HOST) | {"net/handshake"}  # handshake is compiled during setup
    if eos:
        compiled.add("net/epic_rooms")
    present = {p.relative_to(ROOT / "src").with_suffix("").as_posix()
               for d in ("host", "net") for p in (ROOT / "src" / d).glob("*.cpp")}
    if not eos:
        present.discard("net/epic_rooms")
    # The Windows kit always uses win_main. SDL can supply pad input there.
    present.discard("host/sdl_main")
    missing = sorted(present - compiled)
    if missing:
        raise ValueError("Installer HOST list is missing player sources: " + ", ".join(missing))


def assemble(args):
    if bool(args.eos) != bool(args.config):
        raise ValueError("EOS SDK and client config must be supplied together")
    check_host_sources(args.eos is not None)
    out = args.output.resolve()
    if out.is_relative_to(ROOT):
        raise ValueError("Installer output must be outside the private checkout")
    out.parent.mkdir(parents=True, exist_ok=True)
    csc = Path(os.environ["WINDIR"]) / "Microsoft.NET/Framework64/v4.0.30319/csc.exe"
    def build_cs(dest, options):
        # csc.exe reads forward slashes in a source path as option switches and
        # then looks for the bare file name in the working directory. Run it
        # from the installer folder and pass Windows-separated paths.
        win = lambda path: str(path).replace("/", "\\")
        options = [o.split(":", 1)[0] + ":" + win(o.split(":", 1)[1]) if o.startswith("/resource:") else o
                   for o in options]
        subprocess.run([csc, "/nologo", "/optimize+", "/platform:x64", "/r:System.Windows.Forms.dll",
                        "/r:System.Drawing.dll", "/r:Microsoft.CSharp.dll", "/r:System.Core.dll",
                        "/r:System.IO.Compression.dll", "/r:System.IO.Compression.FileSystem.dll",
                        "/win32icon:" + win(ROOT / "src/host/resources/smash.ico"),
                        "/out:" + win(dest), *options, "PlayerSetup.cs"], check=True, cwd=HERE)
    with tempfile.TemporaryDirectory(dir=out.parent) as scratch:
        temp = Path(scratch)
        stage = temp / "kit"
        stage.mkdir()
        for name in GENERATOR:
            copy(ROOT / name, stage / name)
        for src in [*(ROOT / "include/stv/gsp").glob("*.hpp"), *(ROOT / "include/stv/snd").glob("*.hpp"),
                    ROOT / "include/stv/net/rollback.hpp", ROOT / "include/stv/net/handshake.hpp", ROOT / "include/stv/base.hpp", ROOT / "include/stv/zip.hpp",
                    ROOT / "include/stv/reference_timing.hpp"]:
            copy(src, stage / src.relative_to(ROOT))
        project_metadata(stage)
        runtime(stage,args.eos,args.config)
        host_objects(stage,temp,args.llvm,args.eos,args.dxc,args.sdl)
        toolchain(stage,args.llvm)
        copy(HERE / "player_setup.py", stage / "player_setup.py")
        files = sorted(p for p in stage.rglob("*") if p.is_file())
        manifest = {p.relative_to(stage).as_posix():sha(p) for p in files}
        write(stage / "manifest.json", json.dumps(manifest, sort_keys=True))
        with zipfile.ZipFile(temp / "kit.zip", "w", zipfile.ZIP_DEFLATED, compresslevel=3) as z:
            for p in [*files,stage / "manifest.json"]:
                z.write(p,p.relative_to(stage).as_posix())
        # One distributed file: SmashTV.exe installs, prompts for the ROM, builds and launches.
        build_cs(temp / "SmashTV.exe", ["/target:winexe", "/resource:" + str(temp / "kit.zip") + ",player.zip"])
        os.replace(temp / "SmashTV.exe", out)
        print(out, sha(out), flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for opt in ("output", "llvm", "dxc"):
        parser.add_argument("--" + opt, type=Path, required=True)
    for opt in ("eos", "config", "sdl"):
        parser.add_argument("--" + opt, type=Path)
    assemble(parser.parse_args())


