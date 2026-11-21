#!/usr/bin/env python3
"""Reproduce the observed freestanding Windows x64 diagnostic cross-build with Clang/LLD 17.
No downloads, source rewriting, Windows SDK, runtime CPU interpreter or bundled CRT.
This cross-build is NOT evidence of Windows execution. See PORT_STATUS.md.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import shutil
import subprocess
from pathlib import Path

IMPORTS = {
    "kernel32": "GetModuleHandleW ExitProcess VirtualAlloc VirtualFree GetLastError CreateFileW ReadFile WriteFile FlushFileBuffers CloseHandle GetFileSizeEx DeleteFileW MoveFileExW LoadLibraryExW GetProcAddress FreeLibrary QueryPerformanceCounter QueryPerformanceFrequency GetModuleFileNameW GetEnvironmentVariableW CreateDirectoryW GetPrivateProfileIntW WritePrivateProfileStringW",
    "user32": "RegisterClassExW CreateWindowExW ShowWindow UpdateWindow GetMessageW TranslateMessage DispatchMessageW DefWindowProcW PostQuitMessage DestroyWindow BeginPaint EndPaint GetClientRect DrawTextW FillRect InvalidateRect LoadCursorW MessageBoxW SetTimer KillTimer GetAsyncKeyState GetForegroundWindow GetWindowRect GetWindowLongPtrW SetWindowLongPtrW SetWindowPos MonitorFromWindow GetMonitorInfoW",
    "gdi32": "CreateSolidBrush DeleteObject SetTextColor SetBkMode SetStretchBltMode StretchDIBits",
    "comdlg32": "GetOpenFileNameW GetSaveFileNameW",
    "shell32": "DragAcceptFiles DragQueryFileW DragFinish",
}

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=Path("build/windows-cross"))
    parser.add_argument("--clang", default="clang++")
    parser.add_argument("--linker", default="lld-link")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    clang, linker = shutil.which(args.clang), shutil.which(args.linker)
    if not clang or not linker:
        raise SystemExit("Clang C++ and lld-link are required; no automatic installer or download is attempted.")
    log: list[str] = []
    def run(command: list[str]) -> None:
        log.append("COMMAND: " + " ".join(command))
        result = subprocess.run(command, capture_output=True, text=True, timeout=120)
        log.extend([result.stdout, result.stderr])
        (out / "build.log").write_text("\n".join(log))
        if result.returncode:
            raise SystemExit(result.stderr or result.stdout or f"Command failed: {command}")
    objects, libraries = [], []
    for name, exports in IMPORTS.items():
        definition = out / f"{name}.def"
        definition.write_text(f"LIBRARY {name}.dll\nEXPORTS\n" + "\n".join(exports.split()) + "\n")
        library = out / f"{name}.lib"
        run([linker, "/dll", "/noentry", "/machine:x64", f"/def:{definition}", f"/out:{out / ('unused_' + name + '.dll')}", f"/implib:{library}"])
        libraries.append(str(library))
    for source in ["base", "zip", "assets", "game_rules", "sound_commands", "input", "render", "source_assets", "process", "gameplay", "animation", "objects", "source_lab", "source_lifecycle", "player_lifecycle", "waves", "wave_flow", "tank_rules", "doors", "source_arena", "source_doors", "arena_tables", "source_animations", "source_flow", "frontend_state", "frontend_render", "frontend_audits", "frontend_tables", "presentation_tables", "windows"]:
        obj = out / f"{source}.obj"
        run([clang, "--target=x86_64-pc-windows-msvc", "-std=c++20", "-O2", "-ffreestanding", "-fno-exceptions", "-fno-rtti", "-fno-stack-protector", "-mno-stack-arg-probe", "-DSTV_FREESTANDING=1", "-DUNICODE", "-D_UNICODE", "-Wall", "-Wextra", "-Wpedantic", "-I", str(root / "include"), "-c", str(root / ("generated" if source in {"source_animations", "source_flow", "arena_tables", "frontend_tables", "presentation_tables"} else "src") / f"{source}.cpp"), "-o", str(obj)])
        objects.append(str(obj))
    exe = out / "SmashTV.exe"
    run([linker, "/machine:x64", "/subsystem:windows,6.02", "/entry:mainCRTStartup", "/nodefaultlib", "/timestamp:0", "/dynamicbase", "/highentropyva", "/nxcompat", "/stack:8388608,8388608", "/opt:ref", "/opt:icf", f"/out:{exe}", *objects, *libraries])
    version = subprocess.check_output([clang, "--version"], text=True).splitlines()[0]
    manifest = {"artifact": exe.name, "sha256": hashlib.sha256(exe.read_bytes()).hexdigest(), "size": exe.stat().st_size,
                "compiler": version, "linker": subprocess.check_output([linker, "--version"], text=True).strip(),
                "kind": "native source startup/operator checkpoint plus arena lab; COMPLETE GAME NOT IMPLEMENTED", "windows_execution": "UNVERIFIED",
                "stack_policy": "8 MiB reserved and committed; no compiler stack-probe runtime dependency", "crt": "none",
                "required_dlls": [name + ".dll" for name in ["kernel32","user32","gdi32"]], "optional_system_dll": "xinput1_4.dll"}
    (out / "build-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps(manifest, indent=2))
    return 0
if __name__ == "__main__":
    raise SystemExit(main())
