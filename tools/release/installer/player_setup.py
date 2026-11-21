"""Build and launch the native player using only this installation and the selected ROM."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / "tools/player"))
from prepare import prepare, digest


def compile_workers():
    """Parallel compile without exhausting memory: each large translated
    chunk can need over a gigabyte, so stay at six units on any PC.
    (The bundled Python has no ctypes; do not probe memory with it.)"""
    return max(1, min(os.cpu_count() or 1, 6))


def new_uuid():
    raw = bytearray(os.urandom(16))
    raw[6] = (raw[6] & 15) | 0x40
    raw[8] = (raw[8] & 63) | 0x80
    text = raw.hex()
    return "-".join((text[:8], text[8:12], text[12:16], text[16:20], text[20:]))


def is_uuid(text):
    if len(text) != 36 or any(text[i] != "-" for i in (8, 13, 18, 23)):
        return False
    try:
        int(text.replace("-", ""), 16)
    except ValueError:
        return False
    return True


def parallel_map(function, items, workers):
    """Run the small bundled Python without requiring concurrent.futures."""
    results = [None] * len(items)
    next_index = 0
    lock = threading.Lock()
    failure = []

    def worker():
        nonlocal next_index
        while True:
            with lock:
                if next_index >= len(items) or failure:
                    return
                index = next_index
                next_index += 1
            try:
                results[index] = function(items[index])
            except BaseException as error:
                with lock:
                    failure.append(error)
                return

    threads = [threading.Thread(target=worker) for _ in range(min(workers, len(items)))]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    if failure:
        raise failure[0]
    return results


def compile_player(rom, work):
    print("progress 0.10 Validating the selected ROM", flush=True)
    cache = work / "selected-l8.zip"
    generated = prepare(rom, cache, work / "generated")
    print("progress 0.35 Translating the main and sound programs", flush=True)
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

    objects = parallel_map(compile_unit, sources, compile_workers())
    print("progress 0.85 Linking the private native player", flush=True)
    exe = work / "SmashTV.exe"
    host_libraries = [ROOT / "host" / name for name in ("eos.lib", "sdl.lib")
                      if (ROOT / "host" / name).is_file()]
    subprocess.run([compiler, "--target=x86_64-w64-windows-gnu", "-municode", "-mwindows",
                    *objects, *sorted((ROOT / "host").glob("*.o")), *host_libraries,
                    *["-l" + n for n in "ksguid ws2_32 d3d11 d3d12 dxgi d3dcompiler kernel32 user32 gdi32 winspool shell32 ole32 oleaut32 uuid comdlg32 advapi32".split()],
                    "-o", exe], check=True, env=env)
    for item in (ROOT / "runtime").iterdir():
        if item.is_dir():
            shutil.copytree(item, work / item.name)
        else:
            shutil.copyfile(item, work / item.name)
    for dll in ("libc++.dll", "libunwind.dll", "libwinpthread-1.dll"):
        source = compiler.parent / dll
        if source.is_file():
            shutil.copyfile(source, work / dll)
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
        if not is_uuid(saved["generation"]):
            sys.exit(2)
        folder = state / saved["generation"]
        exe, rom = folder / "SmashTV.exe", folder / "selected-l8.zip"
        if saved["kit"] != identity or digest(exe) != saved["exe"] or digest(rom) != saved["rom"]:
            sys.exit(2)
    elif args.rom:
        print("progress 0.05 Preparing the private native player", flush=True)
        generation = new_uuid()
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
            stale = child.is_dir() and is_uuid(child.name) and child.name != generation
            if stale:
                shutil.rmtree(child, ignore_errors=True)
        exe, rom = state / generation / exe.name, state / generation / rom.name
    else:
        sys.exit(2)
    if not args.prepare_only:
        subprocess.Popen([exe], cwd=exe.parent, env=dict(os.environ, STV_HOST_ROM_ZIP=str(rom)))


if __name__ == "__main__":
    main()
