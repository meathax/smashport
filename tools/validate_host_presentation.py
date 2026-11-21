#!/usr/bin/env python3
"""Compare actual Windows presentation throughput without changing game timing.

Each visible host gets its own LOCALAPPDATA/TEMP and the same supplied ROM and
script. This is a bounded host-invariance test, not a ROM/oracle, physical display,
listening, network or release-soak certification. Existing output is preserved.
"""
from __future__ import annotations
import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import struct
import zlib
import sys
import time
from datetime import datetime, timezone


def stamp():
    return datetime.now(timezone.utc).isoformat()


def pin(path):
    path = Path(path).resolve(strict=True)
    return {"path": str(path), "bytes": path.stat().st_size,
            "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}


def write_json(path, value):
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


def require(value, message):
    if not value:
        raise RuntimeError(message)


def check_host(report, frames, mode):
    for key in ("frames", "frame_number", "samples", "presentation_frames", "presentation_failures",
                "frame_captures", "simulation_rate_numerator", "simulation_rate_denominator",
                "simulation_clock_rebases", "main_cycles", "sound_cycles", "mixer_ym_samples",
                "audio_errors", "audio_underruns", "audio_output_samples", "audio_rate", "main_faults",
                "compositor_waits", "compositor_wait_errors", "audio_output_resyncs",
                "audio_dropped_source_samples", "audio_dropped_device_frames",
                "audio_pending_limit_samples", "audio_max_pending_samples"):
        require(type(report.get(key)) is int and report[key] >= 0, f"Invalid host integer {key}")
    for key in ("timed_out", "audio_device", "audio_initial_device", "presentation_window_visible"):
        require(type(report.get(key)) is bool, f"Invalid host boolean {key}")
    for key in ("seconds", "simulation_seconds", "audio_output_ratio", "audio_min_queued_ms", "presentation_fps"):
        require(type(report.get(key)) in (float, int) and math.isfinite(report[key]), f"Invalid host number {key}")
    require(report["frames"] == frames and report["frame_number"] == frames - 1 and not report["timed_out"],
            "Host did not complete the exact requested simulation endpoint")
    require(report["sound_fault"] == "", "Translated sound fault")
    require(report["audio_initial_device"] and report["audio_device"] and report["audio_rate"] > 0 and
            report["audio_errors"] == 0 and report["audio_underruns"] == 0 and
            report["audio_min_queued_ms"] >= 0 and report["samples"] > 0 and report["audio_output_samples"] > 0 and
            report["audio_error_stage"] == "" and report["audio_error_code"] == "00000000",
            "Audio was absent, failed, or underrunning")
    require(0.994999999 <= report["audio_output_ratio"] <= 1.005000001, "Output drift matching exceeded its bound")
    require(report["audio_output_resyncs"] == report["audio_dropped_source_samples"] ==
            report["audio_dropped_device_frames"] == 0, "Normal presentation capture needed audio backlog recovery")
    require(report["audio_pending_limit_samples"] >= 2 and
            report["audio_max_pending_samples"] <= report["audio_pending_limit_samples"],
            "Device software output exceeded its queue bound")
    require(report["compositor_wait_errors"] == 0 and
            (report["compositor_waits"] > 0 if mode == "compositor_synced" else report["compositor_waits"] == 0),
            "Optional compositor wait failed or leaked into unlocked rendering")
    require(report["simulation_rate_numerator"] == 8_000_000 and report["simulation_rate_denominator"] == 146_234 and
            report["simulation_clock_rebases"] == 0, "The game clock was changed or rebased")
    expected_seconds = frames * 146_234 / 8_000_000
    require(abs(report["simulation_seconds"] - expected_seconds) <= 1e-6, "Wrong internal simulation duration")
    require(abs(report["seconds"] - expected_seconds) <= max(.025, expected_seconds * .003),
            "Wall-clock game speed differs from the exact arcade rate")
    require(report["presentation_schema"] == 2 and report["presentation_mode"] == mode,
            "Wrong presentation mode/report schema")
    require(report["presentation_window_visible"] and report["presentation_frames"] >= 2 and
            report["presentation_failures"] == 0, "Actual visible GDI presentations are missing or failed")
    histogram = report["present_ms_hist"]
    require(type(histogram) is list and len(histogram) == 41 and
            all(type(n) is int and n >= 0 for n in histogram) and
            sum(histogram) == report["presentation_frames"] - 1, "Presentation count and histogram disagree")
    require(0 < report["frame_captures"] <= frames, "Invalid completed-frame capture count")
    rect = report["content_rect"]
    require(len(rect) == 4 and all(type(n) is int and n >= 0 for n in rect), "Invalid content rectangle")
    require(abs(rect[0] + rect[2] - report["presentation_client_width"]) <= 1 and
            abs(rect[1] + rect[3] - report["presentation_client_height"]) <= 1,
            "The completed scene is not centered")
    require(report["observation_hash_algorithm"] == "fnv1a64_little_endian", "Unknown observation hash")
    for key in ("raw_pcm_hash", "final_ram_hash", "final_framebuffer_hash"):
        require(type(report.get(key)) is str and len(report[key]) == 16 and
                all(c in "0123456789abcdef" for c in report[key]), f"Missing observation hash {key}")


def capture_window(pid, path):
    """Read pixels from this runner's visible SmashTV client only."""
    user = ctypes.WinDLL("user32", use_last_error=True)
    gdi = ctypes.WinDLL("gdi32", use_last_error=True)
    user.SetProcessDpiAwarenessContext.argtypes = [ctypes.c_void_p]
    user.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user.EnumWindows.argtypes = [callback_type, wintypes.LPARAM]
    user.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user.IsWindowVisible.argtypes = [wintypes.HWND]
    user.SetForegroundWindow.argtypes = [wintypes.HWND]
    user.GetClientRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
    user.GetDC.argtypes = [wintypes.HWND]
    user.GetDC.restype = ctypes.c_void_p
    user.ReleaseDC.argtypes = [wintypes.HWND, ctypes.c_void_p]
    handles = []
    @callback_type
    def visit(hwnd, unused):
        found = wintypes.DWORD()
        user.GetWindowThreadProcessId(hwnd, ctypes.byref(found))
        if found.value == pid and user.IsWindowVisible(hwnd):
            handles.append(hwnd)
            return False
        return True
    user.EnumWindows(visit, 0)
    require(handles, "No owned visible window to capture")
    hwnd = handles[0]
    foreground_requested = bool(user.SetForegroundWindow(hwnd))
    rect = wintypes.RECT()
    require(user.GetClientRect(hwnd, ctypes.byref(rect)), "Cannot read owned client rectangle")
    width, height = rect.right, rect.bottom
    require(0 < width <= 16384 and 0 < height <= 16384 and width * height <= 32 * 1024 * 1024,
            "Invalid screenshot dimensions")
    gdi.CreateCompatibleDC.argtypes = [ctypes.c_void_p]
    gdi.CreateCompatibleDC.restype = ctypes.c_void_p
    gdi.CreateCompatibleBitmap.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
    gdi.CreateCompatibleBitmap.restype = ctypes.c_void_p
    gdi.SelectObject.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    gdi.SelectObject.restype = ctypes.c_void_p
    gdi.DeleteObject.argtypes = [ctypes.c_void_p]
    gdi.DeleteDC.argtypes = [ctypes.c_void_p]
    gdi.BitBlt.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                          ctypes.c_void_p, ctypes.c_int, ctypes.c_int, wintypes.DWORD]
    class Header(ctypes.Structure):
        _fields_ = [("size", wintypes.DWORD), ("width", wintypes.LONG), ("height", wintypes.LONG),
                    ("planes", wintypes.WORD), ("bits", wintypes.WORD), ("compression", wintypes.DWORD),
                    ("image_bytes", wintypes.DWORD), ("xppm", wintypes.LONG), ("yppm", wintypes.LONG),
                    ("used", wintypes.DWORD), ("important", wintypes.DWORD)]
    class Info(ctypes.Structure):
        _fields_ = [("header", Header), ("colors", wintypes.DWORD * 3)]
    gdi.GetDIBits.argtypes = [ctypes.c_void_p, ctypes.c_void_p, wintypes.UINT, wintypes.UINT,
                            ctypes.c_void_p, ctypes.POINTER(Info), wintypes.UINT]
    dc = memory = bitmap = previous = None
    try:
        dc = user.GetDC(hwnd)
        require(dc, "Cannot acquire owned window DC")
        memory = gdi.CreateCompatibleDC(dc)
        bitmap = gdi.CreateCompatibleBitmap(dc, width, height)
        require(memory and bitmap, "Cannot allocate screenshot surface")
        previous = gdi.SelectObject(memory, bitmap)
        require(gdi.BitBlt(memory, 0, 0, width, height, dc, 0, 0, 0x40CC0020), "Client pixel copy failed")
        gdi.SelectObject(memory, previous)
        previous = None
        info = Info()
        info.header = Header(ctypes.sizeof(Header), width, -height, 1, 32, 0, width * height * 4, 0, 0, 0, 0)
        pixels = ctypes.create_string_buffer(width * height * 4)
        require(gdi.GetDIBits(dc, bitmap, 0, height, pixels, ctypes.byref(info), 0) == height,
                "Screenshot rows are incomplete")
        raw = pixels.raw
        rgba = bytearray(raw)
        rgba[0::4], rgba[2::4] = raw[2::4], raw[0::4]
        rgba[3::4] = b"\xff" * (width * height)
        def chunk(kind, data):
            return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)
        rows = b"".join(b"\0" + rgba[y * width * 4:(y + 1) * width * 4] for y in range(height))
        png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
               chunk(b"IDAT", zlib.compress(rows, 1)) + chunk(b"IEND", b""))
        path.write_bytes(png)
        return {"file": pin(path), "width": width, "height": height, "pid": pid,
                "foreground_requested": foreground_requested, "captured_at": stamp()}
    finally:
        if previous and memory:
            gdi.SelectObject(memory, previous)
        if bitmap:
            gdi.DeleteObject(bitmap)
        if memory:
            gdi.DeleteDC(memory)
        if dc:
            user.ReleaseDC(hwnd, dc)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--rom", type=Path, required=True)
    parser.add_argument("--inputs", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--frames", type=int, default=2400)
    parser.add_argument("--profile", choices=("reference", "enhanced"), default="enhanced")
    parser.add_argument("--cases", nargs="+", choices=("nearest-window", "crt-fullscreen", "vsync-window"),
                        default=("nearest-window", "crt-fullscreen", "vsync-window"))
    parser.add_argument("--capture-frames", help="Comma-separated approximate frame targets for wall-timed client captures")
    args = parser.parse_args()
    require(os.name == "nt", "Actual host validation requires Windows")
    require(len(set(args.cases)) == len(args.cases), "Duplicate capture case")
    capture_frames = sorted(int(value) for value in args.capture_frames.split(",")) if args.capture_frames else None
    if capture_frames:
        require(len(set(capture_frames)) == len(capture_frames) and
                all(0 < frame < args.frames for frame in capture_frames), "Invalid screenshot frame targets")
    require(300 <= args.frames <= 100000, "Use a bounded capture of 300 to 100000 frames")
    require(not args.output.exists(), "Preserve existing output directory")
    pins = {name: pin(getattr(args, name)) for name in ("exe", "rom", "inputs")}
    args.output.mkdir(parents=True)
    binary_folder = args.output / "bin"
    binary_folder.mkdir()
    copied_exe = binary_folder / args.exe.name
    shutil.copy2(args.exe, copied_exe)
    # The player loads its runtime DLLs (EOS, librashader, dxcompiler) and
    # shader presets from its own folder.
    for sibling in args.exe.parent.iterdir():
        if sibling.suffix.lower() == ".dll" or sibling.name == "eos-client.ini":
            shutil.copy2(sibling, binary_folder / sibling.name)
        elif sibling.is_dir() and sibling.name == "shaders":
            shutil.copytree(sibling, binary_folder / sibling.name)
    tested_runner = args.output / "tested-runner.py"
    shutil.copy2(__file__, tested_runner)
    source_exe = pins["exe"]
    pins["exe"] = pin(copied_exe)
    require(pins["exe"]["sha256"] == source_exe["sha256"], "Executable snapshot changed")
    receipt = {"schema": "stv.host-presentation-invariance.v1", "result": "RUNNING",
               "started_at": stamp(), "finished_at": None, "owner_pid": os.getpid(), "live_child": None,
               "frames": args.frames, "profile": args.profile, "pins": pins,
               "build_exe_at_snapshot": source_exe, "runner": pin(tested_runner),
               "selected_cases": list(args.cases), "capture_frame_targets": capture_frames,
               "cases": [], "comparison": None, "release_gate_proof": False,
               "limitations": [
                   "GDI submissions are measured; physical monitor scanout is not.",
                   "Original animation advances at the arcade rate; no sprite interpolation is claimed.",
                   "Main-memory diagnostics are compared and retained, not waived or certified correct.",
                   "This is not MAME equivalence, full content, physical listening, network or soak proof."
               ]}
    report_file = args.output / "report.json"
    write_json(report_file, receipt)
    configs = [
        ("nearest-window", 0, 0, 0, 2),
        ("crt-fullscreen", 3, 1, 0, 0),
        ("vsync-window", 0, 0, 1, 2),
    ]
    child = None
    try:
        for name, pixel_filter, fullscreen, vsync, scale in configs:
            if name not in args.cases:
                continue
            folder = args.output / name
            data = folder / "appdata" / "SmashTV"
            temporary = folder / "temp"
            data.mkdir(parents=True)
            temporary.mkdir()
            shutil.copy2(args.rom, data / "smashtv.zip")
            require(pin(data / "smashtv.zip")["sha256"] == pins["rom"]["sha256"], "Private ROM copy changed")
            settings = (f"version=1\nwindow_scale={scale}\nscaling=1\nfilter={pixel_filter}\n"
                        f"fullscreen={fullscreen}\nvsync={vsync}\nfps=1\nvolume=100\nmuted=0\n"
                        "swap_pads=0\nname=TIMING\n")
            settings_path = data / "pc-settings.cfg"
            settings_path.write_text(settings, encoding="ascii")
            host_path = folder / "host.json"
            require(len(str(host_path.resolve())) < 260 and len(pins["inputs"]["path"]) < 260,
                    "Path exceeds the current host automation contract")
            env = {k: v for k, v in os.environ.items() if not k.upper().startswith("STV_")}
            env.update({"STV_HOST_ROM_ZIP": str((data / "smashtv.zip").resolve()), "LOCALAPPDATA": str(data.parent.resolve()), "TEMP": str(temporary.resolve()),
                        "TMP": str(temporary.resolve()), "STV_HOST_FRAMES": str(args.frames),
                        "STV_HOST_REPORT": str(host_path.resolve()), "STV_HOST_NOPAUSE": "1",
                        "STV_HOST_INPUTS": pins["inputs"]["path"], "STV_PROFILE": args.profile})
            case = {"name": name, "settings": settings, "settings_pin": pin(settings_path),
                    "started_at": stamp(), "pid": None, "exit_code": None, "result": "RUNNING", "screenshots": []}
            receipt["cases"].append(case)
            start = time.monotonic()
            print(f"RUN {name}: {args.frames} fixed-rate frames", flush=True)
            with (folder / "stdout.txt").open("wb") as out, (folder / "stderr.txt").open("wb") as err:
                child = subprocess.Popen([pins["exe"]["path"]], cwd=str(Path(pins["exe"]["path"]).parent),
                                         env=env, stdout=out, stderr=err)
                case["pid"] = child.pid
                receipt["live_child"] = child.pid
                write_json(report_file, receipt)
                deadline = max(60.0, 30 + args.frames / 50.0)
                targets = capture_frames or [round(args.frames * fraction) for fraction in (.1, .8)]
                captures = [(frame * 146234 / 8000000, frame) for frame in targets]
                while child.poll() is None and time.monotonic() - start < deadline:
                    if captures and time.monotonic() - start >= captures[0][0]:
                        _, target_frame = captures.pop(0)
                        try:
                            screenshot = capture_window(child.pid, folder / f"frame-{target_frame:06d}.png")
                            screenshot.update({"approximate_frame_target": target_frame,
                                               "wall_seconds_from_launch": time.monotonic() - start,
                                               "frame_target_is_exact": False})
                            case["screenshots"].append(screenshot)
                        except Exception as error:
                            case["screenshots"].append({"capture_error": str(error), "captured_at": stamp(),
                                                        "approximate_frame_target": target_frame})
                        write_json(report_file, receipt)
                    time.sleep(.1)
                require(child.poll() is not None, "Owned host exceeded its external deadline")
                case["exit_code"] = child.returncode
                require(child.returncode == 0, f"Owned host exited with {child.returncode}")
            child = None
            receipt["live_child"] = None
            require(host_path.is_file(), "Host did not write its observation report")
            host = json.loads(host_path.read_text(encoding="utf-8"))
            case.update({"wall_seconds": time.monotonic() - start, "host": host,
                         "host_pin": pin(host_path), "finished_at": stamp()})
            check_host(host, args.frames, "compositor_synced" if vsync else "uncapped")
            require(settings_path.read_text(encoding="ascii") == settings, "Scripted capture changed its preferences")
            case["result"] = "PASS"
            write_json(report_file, receipt)
            print(f"PASS {name}: {host['presentation_fps']:.1f} render FPS; "
                  f"{host['simulation_hz']:.6f} game Hz; {host['audio_underruns']} underruns", flush=True)
        fields = ("frames", "frame_number", "profile", "samples", "audio_rate", "main_cycles", "sound_cycles",
                  "mixer_ym_samples", "raw_pcm_hash", "final_ram_hash", "final_framebuffer_hash",
                  "main_faults", "main_fault_details")
        baseline = receipt["cases"][0]["host"]
        for case in receipt["cases"][1:]:
            for key in fields:
                require(case["host"][key] == baseline[key], f"{case['name']}: simulation/audio differs at {key}")
        for case in receipt["cases"]:
            if case["name"] == "nearest-window":
                require(case["host"]["presentation_frames"] > 2 * args.frames,
                        "Nearest-window run did not demonstrate presentation faster than twice the simulation rate")
        for name, original in pins.items():
            require(pin(original["path"]) == original, f"{name} changed during capture")
        receipt["comparison"] = {"compared_modes": len(receipt["cases"]), "identical_fields": list(fields) if len(receipt["cases"]) > 1 else [], "main_faults_observed": baseline["main_faults"],
                                 "same_mixer_rate": baseline["audio_rate"], "raw_pcm_hash": baseline["raw_pcm_hash"],
                                 "final_ram_hash": baseline["final_ram_hash"],
                                 "final_framebuffer_hash": baseline["final_framebuffer_hash"]}
        receipt["result"] = ("PASS_HOST_PRESENTATION_INVARIANCE" if len(receipt["cases"]) > 1
                             else "PASS_HOST_PRESENTATION_CAPTURE")
    except BaseException as error:
        receipt["result"] = "FAIL"
        receipt["error"] = str(error)
        if receipt["cases"] and receipt["cases"][-1]["result"] == "RUNNING":
            receipt["cases"][-1]["result"] = "FAIL"
            receipt["cases"][-1]["error"] = str(error)
    finally:
        if child is not None and child.poll() is None:
            child.terminate()
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait(timeout=5)
        if child is not None:
            receipt["owned_child_exit_code"] = child.returncode
        receipt["live_child"] = None
        receipt["finished_at"] = stamp()
        write_json(report_file, receipt)
    print(json.dumps({"result": receipt["result"], "report": str(report_file.resolve()),
                      "error": receipt.get("error")}), flush=True)
    return 0 if receipt["result"] in ("PASS_HOST_PRESENTATION_INVARIANCE", "PASS_HOST_PRESENTATION_CAPTURE") else 1


if __name__ == "__main__":
    sys.exit(main())
