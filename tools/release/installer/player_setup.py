"""Build and launch the native player using only this installation and the selected ROM."""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import uuid

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / "tools/player"))
from prepare import prepare, digest


def compile_workers():
    """Parallel compile without exhausting memory: each large translated
    chunk can need over a gigabyte, so stay at six units on any PC.
    (The bundled Python has no ctypes; do not probe memory with it.)"""
    return max(1, min(os.cpu_count() or 1, 6))


def compile_player(rom, work):
    cache = work / "selected-l8.zip"
    generated = prepare(rom, cache, work / "generated")
    compiler = ROOT / "llvm/bin/clang++.exe"
    env = dict(os.environ, PATH=str(compiler.parent) + ";" + os.environ["SystemRoot"] + r"\System32")
    flags = [str(compiler), "--target=x86_64-w64-windows-gnu", "-std=c++20", "-O2",
             "-DNDEBUG", "-I" + str(ROOT / "include"), "-I" + str(generated / "l8")]
    sources = [*sorted((generated / "l8").glob("l8_*.cpp")), generated / "snd/snd_code.cpp",
               ROOT / "src/gsp/enhanced.cpp", ROOT / "src/net/handshake.cpp"]
    identity = hashlib.sha256((ROOT / "manifest.json").read_bytes() +
                              (generated / "outputs.json").read_bytes()).digest()
    header = work / "stv/net/build_identity.hpp"
    header.parent.mkdir(parents=True)
    header.write_text("#pragma once\n#include <array>\n#include <cstdint>\nnamespace stv::net { "
                      "inline constexpr std::array<std::uint8_t,32> kBuildIdentity = {" +
                      ",".join(map(str, identity)) + "}; }\n", encoding="ascii")
    flags.append("-I" + str(work))

    def compile_unit(source):
        obj = work / (source.stem + ".o")
        subprocess.run([*flags, "-w", "-c", source, "-o", obj], check=True, env=env)
        return obj

    with concurrent.futures.ThreadPoolExecutor(max_workers=compile_workers()) as pool:
        objects = list(pool.map(compile_unit, sources))
    exe = work / "SmashTV.exe"
    subprocess.run([compiler, "--target=x86_64-w64-windows-gnu", "-static", "-municode", "-mwindows",
                    *objects, *sorted((ROOT / "host").glob("*.o")), ROOT / "host/eos.lib",
                    *["-l" + n for n in "ksguid ws2_32 d3d11 d3d12 dxgi d3dcompiler kernel32 user32 gdi32 winspool shell32 ole32 oleaut32 uuid comdlg32 advapi32".split()],
                    "-o", exe], check=True, env=env)
    for item in (ROOT / "runtime").iterdir():
        if item.is_dir():
            shutil.copytree(item, work / item.name)
        else:
            shutil.copyfile(item, work / item.name)
    return exe, cache


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--rom", type=Path)
    parser.add_argument("--state", required=True, type=Path)
    parser.add_argument("--prepare-only", action="store_true")
    args = parser.parse_args()
    state = args.state.resolve()
    state.mkdir(parents=True, exist_ok=True)
    marker = state / "player.json"
    identity = digest(ROOT / "manifest.json")
    if marker.is_file() and not args.rom:
        saved = json.loads(marker.read_bytes())
        folder = state / str(uuid.UUID(saved["generation"]))
        exe, rom = folder / "SmashTV.exe", folder / "selected-l8.zip"
        if saved["kit"] != identity or digest(exe) != saved["exe"] or digest(rom) != saved["rom"]:
            sys.exit(2)
    elif args.rom:
        generation = str(uuid.uuid4())
        with tempfile.TemporaryDirectory(dir=state) as td:
            temp = Path(td)
            target = temp / generation
            target.mkdir()
            exe, rom = compile_player(args.rom.resolve(), target)
            for name in ("generated", "stv"):
                shutil.rmtree(target / name)
            for obj in target.glob("*.o"):
                obj.unlink()
            record = dict(kit=identity, generation=generation, exe=digest(exe), rom=digest(rom))
            (temp / "player.json").write_text(json.dumps(record), encoding="utf8")
            target.rename(state / generation)
            os.replace(temp / "player.json", marker)
        # Each setup writes a complete player, imported ROM and runtime. Remove
        # generations the marker no longer names. A still-running older player
        # keeps its files; a later setup removes them.
        for child in state.iterdir():
            try:
                stale = child.is_dir() and str(uuid.UUID(child.name)) == child.name and child.name != generation
            except ValueError:
                stale = False
            if stale:
                shutil.rmtree(child, ignore_errors=True)
        exe, rom = state / generation / exe.name, state / generation / rom.name
    else:
        sys.exit(2)
    if not args.prepare_only:
        subprocess.Popen([exe], cwd=exe.parent, env=dict(os.environ, STV_HOST_ROM_ZIP=str(rom)))


if __name__ == "__main__":
    main()
