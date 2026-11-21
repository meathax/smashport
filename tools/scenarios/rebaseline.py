#!/usr/bin/env python3
"""Renew C2 proofs after C1. --prepare-only never launches a native replay.

Exit 0: preparation succeeded (C1 can still be pending); 2: failed/missing
proof; 3: measured proofs complete, but C2 target closure remains OPEN.
All paths/commands/hashes/results are recorded in a fresh atomic report.json.
"""
import argparse
import collections
import copy
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import threading
import time
import wave
import zlib

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/lockstep"))
import lsfile
import sndfile

CASES = (
    ("1p", 1040000, "full_game_1150k.inputs", "fullgame"),
    ("2p", 440000, "two_player_full_game_440k.inputs", "twoplayer"),
    ("attract", 394000, "attract_soak.inputs", "attractsoak"),
    ("operator_walk", 14720, "operator_walk.inputs", "v_operator_walk"),
    ("operator_freeplay", 13000, "operator_freeplay.inputs", "v_operator_freeplay"),
    ("operator_cmos_reset", 14506, "operator_cmos_reset.inputs", "v_operator_cmos_reset"),
    ("framebuffers", 39100, "bot_s1_40k.inputs", "snapcheck"),
)
SNAPS = (1000, 2000, 2700, 3200, 4200, 5200, 8000, 12000, 18000, 24000, 30000, 39000)
C1_CASES = {name: (frames, f"tests/scenarios/{inputs}") for name, frames, inputs, _ in CASES[:3]}
D10 = ("tools/gsp2cpp", "tools/m6809_aot", "third_party/ymfm", "src/game", "src/gsp", "src/snd",
       "include/stv/game", "include/stv/gsp", "include/stv/snd", "include/stv/reference_timing.hpp", "generated",
       "tools/lockstep/native_main.cpp", "tools/lockstep/bot.hpp", "tools/lockstep/content_observer.hpp",
       "tools/lockstep/scene_observer.hpp", "data/enhanced_patchpoints.json", "data/enhanced_data_roots.json", "data/enhanced_scene_writes.json", "data/enhanced_balance.json")
FRAME_NUM, FRAME_DEN, RATE = 506 * 289, 8000000, 48000
MAME_SHA = "af6966108d9b52c22465c6d50f4e5d50cc371b50f2d27dc443935f287aad37a3"


class ProofError(ValueError):
    pass


class Cancelled(ProofError):
    pass


def require(condition, message):
    if not condition:
        raise ProofError(message)


def cmake_value(build_directory, name):
    cache = Path(build_directory).resolve() / "CMakeCache.txt"
    if not cache.is_file():
        return None
    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith(name + ":"):
            return line.split("=", 1)[1].strip()
    return None


def generated_root(build_directory=None):
    if build_directory is not None:
        selected = cmake_value(build_directory, "STV_PLAYER_INPUTS")
        if selected:
            return Path(selected).resolve()
    selected = os.environ.get("STV_PLAYER_INPUTS")
    return Path(selected).resolve() if selected else ROOT / "generated/private"


def selected_rom(build_directory, root=ROOT):
    selected = Path(build_directory).resolve() / "selected-l8.zip"
    return selected if selected.is_file() else Path(root) / "original/user-supplied-roms/smashtv.zip"


def executable_build_directory(native):
    directory = Path(native).resolve().parent
    for candidate in (directory, directory.parent):
        if (candidate / "CMakeCache.txt").is_file():
            return candidate
    return directory


def now():
    return datetime.now(timezone.utc).isoformat()


def sha(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def pin(path):
    path = Path(path).resolve()
    require(path.is_file(), f"missing file: {path}")
    return {"path": str(path), "bytes": path.stat().st_size, "sha256": sha(path)}


def atomic_json(path, data):
    path = Path(path)
    with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", newline="\n", dir=path.parent,
                                     prefix=path.name + ".", suffix=".tmp", delete=False) as output:
        temp = Path(output.name)
        json.dump(data, output, indent=2)
        output.write("\n")
        output.flush()
        os.fsync(output.fileno())
    try:
        deadline = time.monotonic() + 2
        while True:
            try:
                os.replace(temp, path)
                break
            except PermissionError:
                # Windows readers can temporarily deny rename/delete sharing.
                if time.monotonic() >= deadline:
                    raise
                time.sleep(.01)
    finally:
        temp.unlink(missing_ok=True)


def child_environment():
    env = {key: value for key, value in os.environ.items() if not key.upper().startswith("STV_")}
    env.update(STV_LS_COMPACT="1", STV_LS_SUMMARY="1", STV_LS_DUMP_EVERY="60")
    return env


def main_endpoint(path, final):
    """Prove compact/summary cadence and every intended frame, independently of diff."""
    current, summaries, seen = 0, 0, False
    try:
        for kind, frame, payload in lsfile.records(path):
            require(frame == current and current <= final, f"{path}: unexpected frame {frame}; expected {current}")
            if kind in (1, 2, 3):
                require(not seen and frame % 60 == 0 and len(payload) == 6, f"{path}: invalid compact region/cadence at {frame}")
            elif kind == 9:
                require(not seen and len(payload) == 8, f"{path}: missing/duplicate/malformed summary at {frame}")
                seen = True
                summaries += 1
            elif kind == 8:
                require(seen and not payload, f"{path}: frame {frame} lacks one complete summary/end")
                current += 1
                seen = False
            else:
                raise ProofError(f"{path}: unexpected kind {kind} in summary mode")
    except SystemExit as error:
        raise ProofError(str(error)) from error
    require(current == final + 1 and summaries == final + 1 and not seen,
            f"{path}: intended endpoint 0..{final} incomplete; got {current} frames/{summaries} summaries")
    return {"first_frame": 0, "last_frame": final, "frames": current, "summaries": summaries, "dump_every": 60}


def sound_endpoint(path, final, expected=None):
    summaries, last, last_kind, previous_count = 0, None, None, 0
    for kind, frame, data in sndfile.records(path):
        require(0 <= frame <= final, f"{path}: sound record outside intended 0..{final}")
        last_kind = kind
        if kind == 9:
            require(frame == summaries, f"{path}: noncontiguous sound summary {frame}; expected {summaries}")
            require(data["count"] >= previous_count, f"{path}: cumulative access count decreased")
            previous_count = data["count"]
            last = data
            summaries += 1
    require(summaries == final + 1 and last_kind == 9, f"{path}: intended sound endpoint 0..{final} incomplete")
    require(last is not None and (expected is None or (last["count"], last["hash"]) == expected),
            f"{path}: final sound count/hash differs")
    return {"first_frame": 0, "frame": final, "summaries": summaries, "count": last["count"], "hash": f"{last['hash']:08x}"}


def wav_complete(path, final=39000):
    with wave.open(str(path), "rb") as source:
        channels, width, rate, frames = source.getnchannels(), source.getsampwidth(), source.getframerate(), source.getnframes()
        require((channels, width, rate) == (1, 2, RATE), f"{path}: expected mono PCM16/{RATE} Hz")
        size = 0
        while chunk := source.readframes(65536):
            size += len(chunk)
    require(size == frames * channels * width, f"{path}: truncated/partial PCM payload")
    minimum = final * FRAME_NUM * RATE // FRAME_DEN
    require(frames >= minimum, f"{path}: incomplete command windows through frame {final}; need {minimum} samples, got {frames}")
    return {"rate": rate, "samples": frames, "required_samples": minimum, "last_command_window_frame": final}


def fuzz_complete(vectors, results):
    def row(text, count):
        values = text.split()
        require(len(values) == count and all(re.fullmatch(r"[0-9a-fA-F]{1,8}", value) for value in values), "malformed fuzz fixture row")
        return [int(value, 16) for value in values]
    vlines, rlines = Path(vectors).read_text(encoding="utf-8").splitlines(), Path(results).read_text(encoding="utf-8").splitlines()
    require(rlines and rlines[0].startswith("IO "), "fuzz fixture lacks IO header")
    io = row(rlines[0][3:], 32)
    require(all(value <= 0xffff for value in io), "fuzz IO register exceeds 16 bits")
    require(len(vlines) == 840 and len(rlines) == 841, "fuzz proof requires exactly 840 vectors and 840 results")
    instances = collections.Counter(row(line, 33)[0] for line in vlines)
    for line in rlines[1:]:
        row(line, 34)
    require(len(instances) == 21 and set(instances.values()) == {40}, "fuzz fixture requires 21 instruction instances with 40 vectors each")
    return {"vectors": 840, "results": 840, "instances": 21}


def command_report(data):
    rows, summary = data.get("rows", []), data.get("summary", {})
    require([row.get("cmd") for row in rows] == list(range(256)), "command proof requires all 256 ordered command windows")
    require(summary.get("commands") == 256 and summary.get("failed") == [], "command proof has failures/missing commands")
    require(all(row.get("status") in ("silent", "pass") for row in rows), "command proof has incomplete/failed windows")
    require(summary.get("audible") == sum(row["status"] == "pass" for row in rows)
            and summary.get("silent") == sum(row["status"] == "silent" for row in rows), "command summary counts disagree")
    return summary


def snapshot_report(data):
    rows = data.get("results", [])
    require(data.get("frames") == 12 and data.get("identical") == 12, "snapshot proof requires 12 identical frames")
    require([row.get("frame") for row in rows] == list(SNAPS), "snapshot proof has missing/duplicate/unexpected frames")
    require(all(row.get("size") == [410, 256] and row.get("differing_pixels") == 0 for row in rows), "snapshot dimensions/pixels differ")
    return data


def snapshot_fixture(path):
    data = Path(path).read_bytes()
    require(data[:8] == b"\x89PNG\r\n\x1a\n", f"{path}: invalid PNG header")
    offset, compressed, ended = 8, bytearray(), False
    while offset < len(data):
        require(offset + 12 <= len(data), f"{path}: truncated PNG chunk")
        length, = struct.unpack_from(">I", data, offset)
        kind, end = data[offset + 4:offset + 8], offset + 12 + length
        require(end <= len(data), f"{path}: truncated PNG payload")
        payload = data[offset + 8:end - 4]
        require(zlib.crc32(kind + payload) == struct.unpack_from(">I", data, end - 4)[0], f"{path}: invalid PNG CRC")
        if offset == 8:
            require(kind == b"IHDR" and len(payload) == 13
                    and struct.unpack(">IIBBBBB", payload) == (410, 256, 8, 2, 0, 0, 0), f"{path}: expected RGB8 410x256 snapshot")
        if kind == b"IDAT":
            compressed.extend(payload)
        if kind == b"IEND":
            require(not payload and end == len(data), f"{path}: invalid PNG end")
            ended = True
        offset = end
    require(ended and compressed, f"{path}: incomplete PNG")
    require(len(zlib.decompress(compressed)) == 256 * (1 + 410 * 3), f"{path}: incomplete PNG rows")
    return {"frame_size": [410, 256]}


def fuzz_result(text):
    require(re.search(r"(?m)^PASS: 840 vectors, 0 mismatches\s*$", text) is not None,
            "fuzz proof did not compare all 840 vectors")
    return {"vectors": 840, "mismatches": 0}


def translation_rows(path):
    rows = []
    for line in Path(path).read_text(encoding="utf-8").splitlines()[1:]:
        head, source, chunk, status = line.rsplit(",", 3)
        address, instruction = head.split(",", 1)
        rows.append({"address": address, "instruction": instruction, "source": source, "chunk": chunk, "status": status})
    require(rows and len({row["address"] for row in rows}) == len(rows), "empty/duplicate translation index")
    return rows


def indirect_classifications(index, codemap, covs=()):
    rows = translation_rows(index)
    hit = bytearray(len(rows))
    for path in covs:
        data = Path(path).read_bytes()
        require(len(data) == len(rows) and all(value in (0, 1) for value in data), f"{path}: invalid fresh coverage")
        for i, value in enumerate(data):
            hit[i] |= value
    sites = json.loads(Path(codemap).read_text(encoding="utf-8"))["indirect_sites"]
    require(len(sites) == len(set(sites)) == 147, "expected exactly 147 unique static indirect sites")
    by_address = {row["address"]: (i, row) for i, row in enumerate(rows)}
    require(all(address in by_address and by_address[address][1]["status"] == "translated" for address in sites),
            "indirect sites must identify accepted translated instruction rows")
    out = []
    for address in sites:
        i, row = by_address.get(address, (None, {}))
        out.append({"address": address, "emitted": row.get("status") == "translated", "source": row.get("source", ""),
                    "form": row.get("instruction", ""), "fresh_coverage": "NOT_MEASURED" if not covs else "HIT" if i is not None and hit[i] else "NOT_HIT",
                    "target_closure": "OPEN", "target_set_evidence": []})
    return {"sites": out, "instructions": len(rows), "fresh_runs": len(covs), "target_closure": "OPEN",
            "note": "An emitted or executed source site does not prove its indirect target set. No source-backed target closure supplied."}


def git(root, *args):
    return subprocess.check_output(["git", "-C", str(root), *args], text=True, encoding="utf-8").strip()


def check_d10_sources(root, base):
    finished = git(root, "rev-parse", "HEAD")
    require(not git(root, "log", "--format=%H", f"{base}..{finished}", "--", *D10), "D10 source-touching commit invalidates pinned proof")
    require(not git(root, "diff", "--name-only", base, finished, "--", *D10), "D10 sources changed after pinned commit")
    require(not git(root, "status", "--porcelain", "--untracked-files=all", "--", *D10), "D10 sources have uncommitted changes")
    return finished


def c1_dependency(root, path, recorder, rom, *, renew_native=False):
    require(Path(path).is_file(), f"missing C1 proof: {path}")
    doc = json.loads(Path(path).read_text(encoding="utf-8"))
    jobs = doc.get("jobs", [])
    require(len({job.get("name") for job in jobs}) == len(jobs), "duplicate C1 jobs")
    by_name = {job.get("name"): job for job in jobs}
    pending = [name for name in C1_CASES if by_name.get(name, {}).get("state") != "PASS_COMPLETE_STRICT"]
    if renew_native:
        require(re.fullmatch(r"[0-9a-f]{64}", doc.get("recorder_sha256", "")) is not None,
                "historical C1 recorder provenance missing")
    else:
        require(doc.get("recorder_sha256") == recorder["sha256"], "C1 recorder provenance differs")
    require(doc.get("rom_zip_sha256") == rom["sha256"] and doc.get("mame_sha256") == MAME_SHA, "C1 ROM/MAME provenance differs")
    base = doc.get("verified_at", "")
    require(re.fullmatch(r"[0-9a-f]{40}", base) is not None, "C1 source commit provenance missing")
    if renew_native:
        # Only the complete MAME references are reusable across D10. Old native
        # agreement never becomes current proof through this explicit mode.
        require(git(root, "cat-file", "-t", base) == "commit", "C1 provenance must name an existing commit")
    else:
        check_d10_sources(root, base)
    checked = []
    for name, (frames, inputs) in C1_CASES.items():
        job = by_name.get(name)
        if job is None or name in pending:
            continue
        require(job.get("frames") == frames and job.get("input") == inputs and job.get("input_sha256") == sha(root / inputs), f"C1 {name}: input/frame provenance differs")
        proof = job.get("strict_diff", {})
        diff_command = job.get("strict_diff_command", "").replace("\\", "/")
        require(job.get("exit_code") == 0 and proof.get("exit_code") == 0
                and "NO_DIVERGENCE (both streams ended)" in proof.get("stdout", "")
                and "--allow-prefix" not in diff_command and "sndfile.py diff" in diff_command,
                f"C1 {name}: strict terminal proof missing")
        ends = proof.get("endpoints", [])
        require(len(ends) == 2 and {end.get("role") for end in ends} == {"reference_stream", "native_stream"}, f"C1 {name}: complete endpoints missing")
        require(job["reference_stream"] != job["native_stream"], f"C1 {name}: oracle/native stream paths must differ")
        for end in ends:
            require(end.get("path") == job.get(end["role"]), f"C1 {name}: endpoint path differs")
            stream = root / end["path"]
            measured = sound_endpoint(stream, frames)
            require(all(end.get(key) == value for key, value in measured.items() if key != "first_frame")
                    and end.get("sha256") == sha(stream), f"C1 {name}: endpoint/hash evidence differs")
        require(ends[0]["count"] == ends[1]["count"] and ends[0]["hash"] == ends[1]["hash"], f"C1 {name}: final count/hash differs")
        checked.append(name)
    return {"state": "PENDING" if pending else "REFERENCE_READY_NATIVE_RENEWAL_REQUIRED" if renew_native else "READY",
            "pending": pending, "checked": checked, "verified_at": base, "evidence": pin(path),
            "recorder_matches": not renew_native, "d10_sources_match": not renew_native,
            "native_gate_proof": not renew_native and not pending}


def strict_sound_result(text, reference, native, frames):
    expected_text = f"NO_DIVERGENCE (both streams ended): {frames + 1} frame summaries, {frames + 1} conclusive, last frame {frames}"
    require(text.strip() == expected_text, "strict C1 renewal lacks complete conclusive sound comparison")
    ends = []
    for role, path in (("reference_stream", reference), ("native_stream", native)):
        ends.append({"role": role, **pin(path), **sound_endpoint(path, frames)})
    require(ends[0]["count"] == ends[1]["count"] and ends[0]["hash"] == ends[1]["hash"], "renewed sound endpoints differ")
    return {"stdout": text.strip(), "endpoints": ends}


def renewed_c1_proof(report, old_path, recorder, rom):
    snapshot = report.snapshot()
    prior = json.loads(Path(old_path).read_text(encoding="utf-8"))
    steps = {step["name"]: step for step in snapshot["steps"]}
    jobs = []
    for name, (frames, inputs) in C1_CASES.items():
        native = steps.get(name + "-native", {})
        diff = steps.get(name + "-sound-diff", {})
        require(native.get("state") == diff.get("state") == "PASS" and
                native.get("exit_code") == diff.get("exit_code") == 0,
                "all current native recordings and strict sound comparisons must finish before C1 renewal")
        proof = diff.get("proof", {})
        ends = proof.get("endpoints", [])
        require(len(ends) == 2 and {end.get("role") for end in ends} == {"reference_stream", "native_stream"},
                "renewed sound proof lacks both endpoints")
        jobs.append({"name": name, "state": "PASS_COMPLETE_STRICT", "frames": frames, "input": inputs,
                     "input_sha256": sha(ROOT / inputs), "exit_code": 0, "command": native["command"],
                     **{end["role"]: end["path"] for end in ends},
                     "strict_diff_command": " ".join(diff["command"]),
                     "strict_diff": {"exit_code": 0, "stdout": proof["stdout"], "endpoints": ends,
                                     "log": diff["log"], "log_sha256": diff["log_sha256"]}})
    return {"verified_at": snapshot["verified_at"], "result": "PASS_3_OF_3_COMPLETE_STRICT_SOUND_STREAMS",
            "scope": "Fresh current-commit native recordings; complete pinned MAME references reused. Main-state comparisons use these same fresh processes.",
            "recorder_exe": recorder["path"], "recorder_sha256": recorder["sha256"],
            "rom_zip_sha256": rom["sha256"], "mame_sha256": prior["mame_sha256"], "jobs": jobs,
            "prior_reference_manifest": pin(old_path)}


class Report:
    def __init__(self, directory, data):
        self.directory, self.path, self.data = directory, directory / "report.json", data
        self.lock = threading.RLock()
        self.cancelled = threading.Event()
        self.children = {}
        self.save()

    def save(self):
        with self.lock:
            self.data["updated_at_utc"] = now()
            self.data["owned_live_pids"] = [pid for pid, (process, _) in self.children.items() if process.poll() is None]
            atomic_json(self.path, self.data)

    def update(self, record, **values):
        with self.lock:
            record.update(values)
            self.save()

    def snapshot(self):
        with self.lock:
            return copy.deepcopy(self.data)

    def cancel_children(self, reason):
        with self.lock:
            self.cancelled.set()
            self.data.setdefault("cancellation_reason", reason)
            for process, step in self.children.values():
                if process.poll() is None:
                    step["cancel_requested"] = True
                    try:
                        process.terminate()
                    except OSError:
                        if process.poll() is None:
                            process.kill()
            self.save()

    def run(self, name, argv, directory, verify=None):
        log = directory / (name + ".log")
        step = {"name": name, "command": list(map(str, argv)), "cwd": str(ROOT), "environment": {key: value for key, value in child_environment().items() if key.startswith("STV_")},
                "state": "RUNNING", "started_at_utc": now(), "log": str(log)}
        with self.lock:
            self.data["steps"].append(step)
            self.save()
        process = None
        try:
            with log.open("w", encoding="utf-8") as output:
                # Registration and cancellation share the lock: a child cannot
                # appear between the cancellation check and owned-PID tracking.
                with self.lock:
                    if self.cancelled.is_set():
                        raise Cancelled("cancelled before child launch")
                    process = subprocess.Popen(step["command"], cwd=ROOT, env=child_environment(), stdout=output, stderr=subprocess.STDOUT)
                    self.children[process.pid] = (process, step)
                    self.update(step, pid=process.pid)
                code = process.wait()
                self.update(step, exit_code=code)
            with log.open("rb") as source:
                source.seek(max(0, log.stat().st_size - 32768))
                tail = source.read().decode("utf-8", errors="replace")
            self.update(step, log_sha256=sha(log), output_tail=tail)
            if step.get("cancel_requested"):
                raise Cancelled("owned child terminated after another case failed/interrupted")
            require(code == 0, f"{name}: process failed ({code}); see {log}")
            if verify:
                self.update(step, proof=verify(tail))
            self.update(step, state="PASS")
        except BaseException as error:
            if not isinstance(error, Cancelled):
                self.cancel_children(str(error) or type(error).__name__)
            if process is not None and process.poll() is None:
                try:
                    code = process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    code = process.wait(timeout=5)
                self.update(step, exit_code=code)
            self.update(step, state="CANCELLED" if isinstance(error, Cancelled) else "FAILED", error=str(error) or type(error).__name__)
            raise
        finally:
            with self.lock:
                if process is not None and step.get("exit_code") is not None:
                    self.children.pop(process.pid, None)
                self.update(step, completed_at_utc=now())


def run_main_group(report, cases, workers, run_case):
    require(1 <= workers <= 3, "main workers must be between 1 and 3")
    group = {"workers": workers, "state": "RUNNING", "cases": [{"name": case[0], "state": "QUEUED"} for case in cases]}
    with report.lock:
        report.data["main_group"] = group
        report.save()

    def work(case, entry):
        try:
            if report.cancelled.is_set():
                raise Cancelled("cancelled before case launch")
            report.update(entry, state="RUNNING", started_at_utc=now())
            result = run_case(case)
            report.update(entry, state="PASS", completed_at_utc=now())
            return result
        except BaseException as error:
            report.update(entry, state="CANCELLED" if isinstance(error, Cancelled) else "FAILED", error=str(error) or type(error).__name__, completed_at_utc=now())
            if not isinstance(error, Cancelled):
                report.cancel_children(str(error) or type(error).__name__)
            raise

    pool = ThreadPoolExecutor(max_workers=workers)
    futures = {}
    results = {}
    try:
        for i, (case, entry) in enumerate(zip(cases, group["cases"])):
            futures[pool.submit(work, case, entry)] = i
        for future in as_completed(futures):
            results[futures[future]] = future.result()
    except BaseException as error:
        report.cancel_children(str(error) or type(error).__name__)
        for future, i in futures.items():
            if future.cancel():
                report.update(group["cases"][i], state="CANCELLED", completed_at_utc=now(), error="cancelled before case launch")
        pool.shutdown(wait=True, cancel_futures=True)
        for entry in group["cases"]:
            if entry["state"] == "QUEUED":
                report.update(entry, state="CANCELLED", completed_at_utc=now(), error="cancelled before case launch")
        report.update(group, state="FAILED", completed_at_utc=now())
        if isinstance(error, Cancelled):
            raise ProofError(report.data["cancellation_reason"]) from error
        raise
    else:
        pool.shutdown(wait=True)
        report.update(group, state="PASS", completed_at_utc=now())
        return [results[i] for i in range(len(cases))]


def executable(build, name, configuration):
    candidates = (build / name, build / configuration / name)
    return next((path for path in candidates if path.is_file()), candidates[0])


def command_capture(native, rom, directory):
    # The harness enables co-simulation only when its --snd stream is open.
    return [native, rom, directory / "native.bin", 39000, "--native-hcount",
            "--snd", directory / "native.snd", "--wav", directory / "native.wav",
            "--snd-commands", "600,150,0,255"]


def content_samples(path, final):
    """Validate raw sampled observations; incomplete POST lists stay explicit."""
    metadata = {"schema": "stv.content.samples.v1", "cadence": 60, "phase": "before_main_region_dump",
                "process_pdata198": "raw_unbound_field"}
    expected, object_boundaries, process_boundaries, ingame = 0, 0, 0, 0
    def integer(value, low, high):
        return type(value) is int and low <= value <= high
    def nodes(rows, complete, process):
        require(type(rows) is list and type(complete) is bool, "content list/complete flag malformed")
        addresses, boundary = set(), False
        fields = ("pid", "wake", "saved_sp", "pdata198_raw", "saved_a8", "saved_a9", "saved_a10", "saved_a11") if process else ("oid", "image", "x", "y")
        for i, row in enumerate(rows):
            require(type(row) is dict, "content node malformed")
            if "boundary_address" in row:
                require(set(row) == {"boundary_address", "reason"} and not complete and i == len(rows)-1
                        and integer(row["boundary_address"], 1, 0xffffffff)
                        and row["reason"] == "invalid_or_cyclic_ram_node", "content boundary malformed")
                boundary = True
                continue
            require(set(row) == {"address", *fields}, "content node fields malformed")
            address = row["address"]
            extent = 0x1a8 if process else 0x160
            require(integer(address, 0x01000000, 0x01100000-extent) and address % 16 == 0 and address not in addresses,
                    "content RAM node address invalid/duplicate")
            addresses.add(address)
            for name in fields:
                low, high = (-32768, 32767) if name in ("x", "y") else (0, 0xffff if name in ("pid", "pdata198_raw", "oid") else 0xffffffff)
                require(integer(row[name], low, high), "content node value invalid")
        require(boundary == (not complete), "content incomplete list lacks a named boundary")
        return boundary
    with Path(path).open(encoding="utf-8") as stream:
        header = json.loads(stream.readline())
        require(header == metadata and type(header.get("cadence")) is int, "content metadata/phase differs")
        for line in stream:
            row = json.loads(line)
            require(type(row) is dict and set(row) == {"frame", "gamstate", "circuit", "wave", "world_x", "world_y", "p1_lives", "p2_lives",
                    "objects", "object_list_complete", "processes", "process_list_complete"}, "content sample fields malformed")
            require(type(row["frame"]) is int and row["frame"] == expected and expected <= final, "content sample cadence/endpoint differs")
            expected += 60
            for name in ("gamstate", "circuit", "wave", "p1_lives", "p2_lives"):
                require(integer(row[name], 0, 0xffff), "content raw word invalid")
            require(all(integer(row[name], -32768, 32767) for name in ("world_x", "world_y")), "content signed coordinates invalid")
            object_boundaries += nodes(row["objects"], row["object_list_complete"], False)
            process_boundaries += nodes(row["processes"], row["process_list_complete"], True)
            ingame += row["gamstate"] == 0x10
    require(expected == (final//60+1)*60, "content samples incomplete")
    return {"samples": expected//60, "last_sample_frame": expected-60, "object_boundary_samples": object_boundaries,
            "process_boundary_samples": process_boundaries, "in_game_samples": ingame, "content_gate_proof": False}


def resume_recordings(root, path, native, required_files, c1_evidence):
    """Reuse completed captures only; rerun all comparisons in the new report."""
    path = Path(path).resolve()
    require(path.is_relative_to((root / "build").resolve()), "resume report must be within project build/")
    data = json.loads(path.read_text(encoding="utf-8"))
    require(data.get("result") in ("FAILED", "PROOFS_COMPLETE_TARGET_CLOSURE_OPEN")
            and not data.get("owned_live_pids"), "resume needs a terminal report without owned live jobs")
    base = data.get("verified_at", "")
    require(re.fullmatch(r"[0-9a-f]{40}", base) is not None, "resume source commit missing")
    check_d10_sources(root, base)
    group = data.get("main_group", {})
    require(group.get("state") == "PASS"
            and [(c.get("name"), c.get("state")) for c in group.get("cases", [])]
            == [(case[0], "PASS") for case in CASES], "resume requires every completed main native case")
    # These reviewed orchestration files may have been corrected since capture.
    # Comparators, observers, runtime, generated data and oracle/input pins may not.
    mutable = {str((root / name).resolve()) for name in
               ("tools/scenarios/rebaseline.py", "tools/scenarios/rebaseline.ps1", "tests/test_rebaseline.py")}
    required = {str(Path(p).resolve()) for p in required_files} - mutable - {str(Path(c1_evidence).resolve())}
    pins = data.get("pins", {})
    require(required <= set(pins), "resume prerequisite pin missing")
    for name, recorded in pins.items():
        if name in mutable:
            continue
        require(str(Path(recorded["path"]).resolve()) == name, "resume pin path disagrees")
        require(pin(name) == recorded, f"resume prerequisite changed: {name}")
    steps = data.get("steps", [])
    output = {}
    artifacts = {str(path): pin(path)}
    for name, frames, inputs, _ in CASES:
        found = [s for s in steps if s.get("name") == name + "-native"]
        require(len(found) == 1 and found[0].get("state") == "PASS" and found[0].get("exit_code") == 0,
                f"{name}: resume native step incomplete/duplicate")
        step = found[0]
        proof = step.get("proof", {})
        paths = {}
        for field in ("stream", "sound_stream", "coverage", "content_stream"):
            recorded = proof.get(field, {})
            require(recorded.get("path"), f"{name}: resume {field} pin missing")
            file = Path(recorded["path"]).resolve()
            require(file.is_relative_to(path.parent), f"{name}: resume artifact outside original output")
            require(pin(file) == recorded, f"{name}: resume {field} changed")
            paths[field] = file
            artifacts[str(file)] = recorded
        require(main_endpoint(paths["stream"], frames) == proof.get("endpoint"), f"{name}: resume endpoint disagrees")
        require(content_samples(paths["content_stream"], frames) == proof.get("content_samples"), f"{name}: resume content samples disagree")
        expected = [native, selected_rom(executable_build_directory(native), root), paths["stream"], frames,
                    "--native-hcount", "--inputs", root / "tests/scenarios" / inputs,
                    "--snd", paths["sound_stream"], "--coverage", paths["coverage"], "--content", paths["content_stream"]]
        if name == "framebuffers":
            expected += ["--snap", ",".join(map(str, SNAPS)), "--snap-dir", paths["stream"].parent]
            for frame in SNAPS:
                file = paths["stream"].parent / f"native_{frame}.png"
                snapshot_fixture(file)
                artifacts[str(file)] = pin(file)
        require(step.get("command") == list(map(str, expected)), f"{name}: resume native command differs")
        require(step.get("environment") == {k: v for k, v in child_environment().items() if k.startswith("STV_")},
                f"{name}: resume native environment differs")
        log = Path(step.get("log", "")).resolve()
        require(log.is_relative_to(path.parent) and sha(log) == step.get("log_sha256"), f"{name}: resume native log changed")
        artifacts[str(log)] = pin(log)
        output[name] = copy.deepcopy(step)
    return output, artifacts


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prepare-only", "--preflight", action="store_true")
    parser.add_argument("--build-directory", default="build/msvc")
    parser.add_argument("--configuration", choices=("Debug", "Release"), default="Release")
    parser.add_argument("--workers", type=int, choices=(1, 2, 3), default=3, help="independent main-case workers (default: 3)")
    parser.add_argument("--out-dir")
    parser.add_argument("--resume-report", help="reuse validated completed native captures from a terminal C2 report; rerun comparisons and remaining proofs")
    parser.add_argument("--c1-evidence", default="evidence/c1-sound-full-runs-2026-10-01.json")
    parser.add_argument("--renew-c1-native", action="store_true",
                        help="reuse completed pinned MAME sound references; renew C1 sound with the fresh C2 native recordings")
    args = parser.parse_args(argv)
    report = None
    try:
        commit = git(ROOT, "rev-parse", "HEAD")
        output = (ROOT / (args.out_dir or f"build/c2-{datetime.now(timezone.utc):%Y%m%dT%H%M%S}-{commit[:8]}")).resolve()
        require(output.is_relative_to((ROOT / "build").resolve()), "outputs must stay within project build/")
        output.mkdir(parents=True, exist_ok=False)
        report = Report(output, {"verified_at": commit, "mode": "PREPARE_ONLY" if args.prepare_only else "FULL",
                                "result": "PREPARING", "gate": "OPEN", "main_workers": args.workers, "steps": [], "pins": {}, "started_at_utc": now()})
        report.run("runner-tests", [sys.executable, ROOT / "tests/test_rebaseline.py"], output)
        build = (ROOT / args.build_directory).resolve()
        native, fuzz = executable(build, "stv_l8_lockstep.exe", args.configuration), executable(build, "stv_op_fuzz.exe", args.configuration)
        generated = generated_root(build)
        rom = selected_rom(build)
        index = generated / "l8/l8_translation_index.csv"
        codemap = generated / "l8_codemap.json"
        files = [native, fuzz, rom, index, codemap, ROOT / args.c1_evidence]
        files += [ROOT / "tests/scenarios" / case[2] for case in CASES]
        files += [ROOT / "build" / case[3] / "ls.bin" for case in CASES]
        files += [ROOT / "build/snapcheck/snap" / f"mame_{frame}.png" for frame in SNAPS]
        files += [ROOT / path for path in ("build/opfuzz/vectors.txt", "build/opfuzz/mame.txt", "build/snd-20k/snd.bin", "build/goldens/mame.wav")]
        files += [path for path in generated.rglob("*") if path.is_file()]
        files += [ROOT / "tools/lockstep" / path for path in ("lsfile.py", "sndfile.py", "snapcmp.py", "wavcmp.py", "snd_goldens.py", "coverage.py", "record.lua", "snd_record.lua", "snd_commands.lua", "opfuzz.lua", "opfuzz_vectors.py", "native_main.cpp", "bot.hpp", "content_observer.hpp")]
        files += [ROOT / path for path in ("tools/scenarios/rebaseline.py", "tools/scenarios/rebaseline.ps1", "tests/test_rebaseline.py", "tests/test_op_fuzz.cpp")]
        report.data["pins"] = {str(path.resolve()): pin(path) for path in sorted(set(files))}
        report.data["working_tree"] = git(ROOT, "status", "--short")
        report.data["dependency"] = c1_dependency(ROOT, ROOT / args.c1_evidence, pin(native), pin(rom), renew_native=args.renew_c1_native)
        references = {job["name"]: ROOT / job["reference_stream"] for job in json.loads((ROOT / args.c1_evidence).read_text(encoding="utf-8"))["jobs"]}
        if args.renew_c1_native:
            for name in C1_CASES:
                require(name in references, "missing reusable C1 reference")
                report.data["pins"][str(references[name].resolve())] = pin(references[name])
        measured = {}
        for name, frames, inputs, oracle in CASES:
            measured[name] = main_endpoint(ROOT / "build" / oracle / "ls.bin", frames)
        measured["fuzz"] = fuzz_complete(ROOT / "build/opfuzz/vectors.txt", ROOT / "build/opfuzz/mame.txt")
        measured["sound_20k"] = sound_endpoint(ROOT / "build/snd-20k/snd.bin", 20000, (32828328, 0xff493ea0))
        measured["command_wav"] = wav_complete(ROOT / "build/goldens/mame.wav")
        measured["snapshots"] = [snapshot_fixture(ROOT / "build/snapcheck/snap" / f"mame_{frame}.png") for frame in SNAPS]
        report.data["oracle_preflight"] = measured
        reusable = {}
        if args.resume_report:
            reusable, artifacts = resume_recordings(ROOT, ROOT / args.resume_report, native, files, ROOT / args.c1_evidence)
            report.data["pins"].update(artifacts)
            report.data["resumed_from"] = pin(ROOT / args.resume_report)
        report.data["indirect"] = indirect_classifications(index, codemap)
        report.data["planned_cases"] = [{"name": name, "frames": frames, "input": inputs, "oracle": oracle} for name, frames, inputs, oracle in CASES]
        report.save()
        if args.prepare_only:
            report.data["finished_at_commit"] = check_d10_sources(ROOT, commit)
            report.data["result"] = "PREPARED_C1_PENDING" if report.data["dependency"]["pending"] else "PREPARED_NOT_RUN"
            report.save()
            print(json.dumps({"report": str(report.path), "result": report.data["result"], "gate": "OPEN", "C1": report.data["dependency"]["state"]}))
            return 0
        require(report.data["dependency"]["state"] == ("REFERENCE_READY_NATIVE_RENEWAL_REQUIRED" if args.renew_c1_native else "READY"),
                "C1 dependency pending: " + ", ".join(report.data["dependency"]["pending"]))
        require(not git(ROOT, "status", "--porcelain", "--", "tools/scenarios/rebaseline.py", "tools/scenarios/rebaseline.ps1", "tools/lockstep", "tests/test_rebaseline.py", "tests/test_op_fuzz.cpp"),
                "commit reviewed runner/validator changes before generating C2 proof")
        check_d10_sources(ROOT, commit)
        report.data["result"] = "RUNNING"
        report.save()
        def main_case(case):
            name, frames, inputs, oracle = case
            directory = output / name
            directory.mkdir(exist_ok=False)
            stream, sound, cov = directory / "native.bin", directory / "native.snd", directory / "native.cov"
            content = directory / "content.jsonl"
            cmd = [native, rom, stream, frames, "--native-hcount", "--inputs", ROOT / "tests/scenarios" / inputs,
                   "--snd", sound, "--coverage", cov, "--content", content]
            if name == "framebuffers":
                cmd += ["--snap", ",".join(map(str, SNAPS)), "--snap-dir", directory]
            if name in reusable:
                step = copy.deepcopy(reusable[name])
                step.pop("pid", None)
                step.update(execution="REUSED_PINNED_NATIVE_CAPTURE", reuse_verified_at_utc=now())
                stream, sound, cov = (Path(step["proof"][key]["path"]) for key in ("stream", "sound_stream", "coverage"))
                with report.lock:
                    report.data["steps"].append(step)
                    report.save()
            else:
                report.run(name + "-native", cmd, directory,
                           lambda _, p=stream, s=sound, c=cov, o=content, n=frames: {"endpoint": main_endpoint(p, n), "stream": pin(p), "sound_stream": pin(s), "coverage": pin(c),
                               "content_stream": pin(o), "content_samples": content_samples(o, n)})
            report.run(name + "-diff", [sys.executable, ROOT / "tools/lockstep/lsfile.py", "diff", ROOT / "build" / oracle / "ls.bin", stream], directory)
            if args.renew_c1_native and name in C1_CASES:
                reference = references[name]
                report.run(name + "-sound-diff", [sys.executable, ROOT / "tools/lockstep/sndfile.py", "diff", reference, sound], directory,
                           lambda text, r=reference, s=sound, n=frames: strict_sound_result(text, r, s, n))
            data = cov.read_bytes()
            require(len(data) == report.data["indirect"]["instructions"] and all(value in (0, 1) for value in data), f"{cov}: invalid fresh coverage")
            if name == "framebuffers":
                report.run("snapshots", [sys.executable, ROOT / "tools/lockstep/snapcmp.py", ROOT / "build/snapcheck/snap", stream.parent, *SNAPS], directory,
                           lambda text: snapshot_report(json.loads(text.splitlines()[-1])))
            return cov
        fresh_cov = run_main_group(report, CASES, args.workers, main_case)
        if args.renew_c1_native:
            check_d10_sources(ROOT, commit)
            renewed = output / "renewed-c1-sound.json"
            atomic_json(renewed, renewed_c1_proof(report, ROOT / args.c1_evidence, pin(native), pin(rom)))
            report.data["renewed_c1"] = c1_dependency(ROOT, renewed, pin(native), pin(rom))
            require(report.data["renewed_c1"]["state"] == "READY", "fresh C1 sound renewal incomplete")
        report.data["indirect"] = indirect_classifications(index, codemap, fresh_cov)
        report.save()
        directory = output / "fuzz"
        directory.mkdir(exist_ok=False)
        report.run("fuzz", [fuzz, rom, ROOT / "build/opfuzz/vectors.txt", ROOT / "build/opfuzz/mame.txt"], directory,
                   fuzz_result)
        directory = output / "sound_20k"
        directory.mkdir(exist_ok=False)
        sound = directory / "native.snd"
        report.run("sound20k-native", [native, rom, directory / "native.bin", 20000, "--native-hcount", "--native-inputs", "--snd", sound, "--expect-snd", "32828328:ff493ea0"], directory,
                   lambda _: {"endpoint": sound_endpoint(sound, 20000, (32828328, 0xff493ea0)), "stream": pin(sound)})
        report.run("sound20k-diff", [sys.executable, ROOT / "tools/lockstep/sndfile.py", "diff", ROOT / "build/snd-20k/snd.bin", sound], directory)
        directory = output / "commands"
        directory.mkdir(exist_ok=False)
        wav, golden = directory / "native.wav", directory / "goldens.json"
        report.run("commands-native", command_capture(native, rom, directory), directory,
                   lambda _: {"endpoint": wav_complete(wav), "wav": pin(wav)})
        report.run("command-goldens", [sys.executable, ROOT / "tools/lockstep/snd_goldens.py", ROOT / "build/goldens/mame.wav", wav, "--start", 600, "--period", 150, "--from", 0, "--to", 255, "--json", golden], directory,
                   lambda _: {"summary": command_report(json.loads(golden.read_text(encoding="utf-8"))), "report": pin(golden)})
        report.run("coverage", [sys.executable, ROOT / "tools/lockstep/coverage.py", index, *fresh_cov], output)
        report.data["finished_at_commit"] = check_d10_sources(ROOT, commit)
        for recorded in report.data["pins"].values():
            require(sha(recorded["path"]) == recorded["sha256"], f"pinned prerequisite changed during run: {recorded['path']}")
        report.data["result"] = "PROOFS_COMPLETE_TARGET_CLOSURE_OPEN"
        report.save()
        print(json.dumps({"report": str(report.path), "result": report.data["result"], "gate": "OPEN"}))
        return 3
    except (OSError, ValueError, KeyError, EOFError, wave.Error, zlib.error, subprocess.SubprocessError, KeyboardInterrupt) as error:
        if report:
            report.data.update(result="FAILED", error=str(error), gate="OPEN")
            report.save()
        print(f"C2 preparation/proof failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
