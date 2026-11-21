#!/usr/bin/env python3
"""Bounded C2 runner regressions; no native replay, ROM import or builds."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import threading
import time
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch
import wave
import zlib

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("rebaseline", ROOT / "tools/scenarios/rebaseline.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


CONTENT_META = {"schema":"stv.content.samples.v1","cadence":60,"phase":"before_main_region_dump","process_pdata198":"raw_unbound_field"}
def content_row(frame):
    return dict(frame=frame,gamstate=16,circuit=0,wave=1,world_x=-370,world_y=338,p1_lives=3,p2_lives=2,
                objects=[],object_list_complete=True,processes=[],process_list_complete=True)


class RebaselineTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name)

    def main_stream(self, name, frames, omit_summary=None, omit_end=None, extra_region=None):
        out = bytearray(b"STVLS001")
        for frame in frames:
            if frame == extra_region:
                out.extend(struct.pack("<BIHHI", 1, frame, 6, 0, 123))
            if frame != omit_summary:
                out.extend(struct.pack("<BIHII", 9, frame, 8, 123, 0))
            if frame != omit_end:
                out.extend(struct.pack("<BIH", 8, frame, 0))
        path = self.path / name
        path.write_bytes(out)
        return path

    def sound_stream(self, name, frames):
        out = bytearray(b"STVSND01")
        for frame in frames:
            out.extend(struct.pack("<BIII", 9, frame, frame + 1, 123))
        path = self.path / name
        path.write_bytes(out)
        return path

    def test_identical_shortened_streams_are_not_intended_endpoint_proof(self):
        a, b = self.main_stream("a.bin", [0, 1]), self.main_stream("b.bin", [0, 1])
        # Existing strict diff establishes equality, so the separate endpoint
        # check must reject this otherwise plausible short proof.
        with contextlib.redirect_stdout(io.StringIO()):
            code = runner.lsfile.cmd_diff(SimpleNamespace(a=a, b=b, allow_prefix=False, window=0, symbols=None))
        self.assertEqual(code, 0)
        for path in (a, b):
            with self.assertRaisesRegex(runner.ProofError, "intended endpoint"):
                runner.main_endpoint(path, 2)

    def test_main_endpoint_complete_and_contiguous(self):
        data = runner.main_endpoint(self.main_stream("ok.bin", [0, 1, 2]), 2)
        self.assertEqual((data["frames"], data["summaries"], data["last_frame"]), (3, 3, 2))
        for frames in ([1, 2], [0, 2], [0, 1, 1], [0, 1, 2, 3]):
            with self.subTest(frames=frames), self.assertRaises(runner.ProofError):
                runner.main_endpoint(self.main_stream("bad.bin", frames), 2)

    def test_main_endpoint_requires_each_summary_and_end(self):
        for options in ({"omit_summary": 1}, {"omit_end": 2}, {"extra_region": 1}):
            with self.subTest(options=options), self.assertRaises(runner.ProofError):
                runner.main_endpoint(self.main_stream("bad.bin", [0, 1, 2], **options), 2)

    def test_sound_requires_final_contiguous_summary_and_count(self):
        path = self.sound_stream("ok.snd", [0, 1, 2])
        self.assertEqual(runner.sound_endpoint(path, 2, (3, 123))["summaries"], 3)
        for frames in ([0, 1], [0, 2], [1, 2]):
            with self.subTest(frames=frames), self.assertRaises(runner.ProofError):
                runner.sound_endpoint(self.sound_stream("bad.snd", frames), 2)
        with self.assertRaisesRegex(runner.ProofError, "count/hash"):
            runner.sound_endpoint(path, 2, (4, 123))

    def fuzz_fixture(self, count=840, results=840, header=None):
        vectors, oracle = self.path / "vectors.txt", self.path / "mame.txt"
        vectors.write_text("".join(f"{i // 40:08x} " + " ".join(["0"] * 32) + "\n" for i in range(count)))
        oracle.write_text((header if header is not None else "IO " + " ".join(["0"] * 32) + "\n")
                          + (" ".join(["0"] * 34) + "\n") * results)
        return vectors, oracle

    def test_fuzz_requires_complete_840_vector_fixture(self):
        self.assertEqual(runner.fuzz_complete(*self.fuzz_fixture())["vectors"], 840)
        for count, results in ((839, 840), (840, 839), (839, 839), (841, 841), (0, 0)):
            with self.subTest(count=count, results=results), self.assertRaisesRegex(runner.ProofError, "840"):
                runner.fuzz_complete(*self.fuzz_fixture(count, results))

    def test_fuzz_requires_io_and_complete_hex_rows(self):
        for header in ("", "IO 0\n", "IO " + " ".join(["10000"] * 32) + "\n"):
            with self.subTest(header=header), self.assertRaises(runner.ProofError):
                runner.fuzz_complete(*self.fuzz_fixture(header=header))
        vectors, oracle = self.fuzz_fixture()
        vectors.write_text(vectors.read_text(encoding="utf-8").replace("00000000", "00000000junk", 1), encoding="utf-8")
        with self.assertRaises(runner.ProofError):
            runner.fuzz_complete(vectors, oracle)

    def commands(self):
        return {"summary": {"commands": 256, "audible": 0, "silent": 256, "failed": []},
                "rows": [{"cmd": cmd, "status": "silent"} for cmd in range(256)]}

    def test_all_256_command_windows_required(self):
        self.assertEqual(runner.command_report(self.commands())["commands"], 256)
        data = self.commands()
        data["rows"].pop()
        with self.assertRaisesRegex(runner.ProofError, "256"):
            runner.command_report(data)
        data = self.commands()
        data["rows"][100]["cmd"] = 99
        with self.assertRaises(runner.ProofError):
            runner.command_report(data)
        data = self.commands()
        data["rows"][-1]["status"] = "missing"
        with self.assertRaises(runner.ProofError):
            runner.command_report(data)

    def test_wav_complete_payload_and_last_window_required(self):
        minimum = 10 * runner.FRAME_NUM * runner.RATE // runner.FRAME_DEN
        path = self.path / "audio.wav"
        for frames, rate, truncated in ((minimum, 48000, False), (minimum - 1, 48000, False),
                                         (minimum + 10, 44100, False), (minimum + 10, 48000, True)):
            with self.subTest(frames=frames, rate=rate, truncated=truncated):
                with wave.open(str(path), "wb") as out:
                    out.setnchannels(1)
                    out.setsampwidth(2)
                    out.setframerate(rate)
                    out.writeframes(b"\x00\x00" * frames)
                if truncated:
                    path.write_bytes(path.read_bytes()[:-2])
                if frames == minimum and rate == 48000 and not truncated:
                    self.assertEqual(runner.wav_complete(path, 10)["samples"], minimum)
                else:
                    with self.assertRaises(runner.ProofError):
                        runner.wav_complete(path, 10)

    def test_all_twelve_exact_snapshots_required(self):
        data = {"frames": 12, "identical": 12,
                "results": [{"frame": frame, "size": [410, 256], "differing_pixels": 0} for frame in runner.SNAPS]}
        runner.snapshot_report(data)
        data["results"].pop()
        with self.assertRaises(runner.ProofError):
            runner.snapshot_report(data)

    def test_snapshot_fixture_dimensions_and_missing_file(self):
        path = self.path / "mame.png"
        def png(width):
            def chunk(kind, payload):
                return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload))
            return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, 256, 8, 2, 0, 0, 0))
                    + chunk(b"IDAT", zlib.compress(b"\0" * (256 * (1 + width * 3)))) + chunk(b"IEND", b""))
        path.write_bytes(png(410))
        runner.snapshot_fixture(path)
        path.write_bytes(png(409))
        with self.assertRaises(runner.ProofError):
            runner.snapshot_fixture(path)
        path.write_bytes(png(410)[:-12])
        with self.assertRaises(runner.ProofError):
            runner.snapshot_fixture(path)
        with self.assertRaises(FileNotFoundError):
            runner.snapshot_fixture(self.path / "missing.png")

    def test_fuzz_success_requires_all_840_actual_comparisons(self):
        runner.fuzz_result("PASS: 840 vectors, 0 mismatches\n")
        for text in ("PASS: 0 vectors, 0 mismatches", "PASS: 839 vectors, 0 mismatches", "FAIL: 840 vectors, 1 mismatches"):
            with self.subTest(text=text), self.assertRaises(runner.ProofError):
                runner.fuzz_result(text)

    def test_missing_c1_proof_cannot_start_full_run(self):
        with self.assertRaisesRegex(runner.ProofError, "missing C1 proof"):
            runner.c1_dependency(self.path, self.path / "missing.json", {"sha256": "exe"}, {"sha256": "rom"})

    def test_pending_c1_states_are_not_ready_and_provenance_is_checked(self):
        path = self.path / "c1.json"
        data = {"jobs": [{"name": name, "state": "RUNNING"} for name in runner.C1_CASES],
                "recorder_sha256": "exe", "rom_zip_sha256": "rom", "mame_sha256": runner.MAME_SHA, "verified_at": "0" * 40}
        path.write_text(json.dumps(data))
        with patch.object(runner, "git", return_value=""):
            dependency = runner.c1_dependency(self.path, path, {"sha256": "exe"}, {"sha256": "rom"})
            self.assertEqual(dependency["state"], "PENDING")
            self.assertEqual(set(dependency["pending"]), {"1p", "2p", "attract"})
            with self.assertRaisesRegex(runner.ProofError, "recorder provenance"):
                runner.c1_dependency(self.path, path, {"sha256": "changed"}, {"sha256": "rom"})

    def test_pass_labels_without_complete_c1_evidence_are_rejected(self):
        path = self.path / "c1.json"
        for name, (_, inputs) in runner.C1_CASES.items():
            (self.path / inputs).parent.mkdir(parents=True, exist_ok=True)
            (self.path / inputs).write_bytes(b"")
        data = {"jobs": [{"name": name, "state": "PASS_COMPLETE_STRICT", "frames": frames, "input": inputs,
                          "input_sha256": runner.sha(self.path / inputs), "exit_code": 0,
                          "strict_diff_command": "python tools/lockstep/sndfile.py diff ref.snd native.snd",
                          "strict_diff": {"exit_code": 0, "stdout": "NO_DIVERGENCE (both streams ended)"}}
                         for name, (frames, inputs) in runner.C1_CASES.items()],
                "recorder_sha256": "exe", "rom_zip_sha256": "rom", "mame_sha256": runner.MAME_SHA, "verified_at": "0" * 40}
        path.write_text(json.dumps(data))
        with patch.object(runner, "git", return_value=""), self.assertRaisesRegex(runner.ProofError, "endpoints missing"):
            runner.c1_dependency(self.path, path, {"sha256": "exe"}, {"sha256": "rom"})

    def source_repo(self, name):
        root = self.path / name
        root.mkdir()
        def command(*args):
            return subprocess.check_output(["git", "-C", str(root), "-c", "commit.gpgsign=false", *args],
                                           text=True, encoding="utf-8", stderr=subprocess.STDOUT).strip()
        command("init", "--quiet")
        command("config", "user.name", "C2 regression")
        command("config", "user.email", "c2-test@example.invalid")
        for relative in ("tools/m6809_aot/translate.py", "third_party/ymfm/src/ymfm.cpp", "PLAN.md"):
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("initial\n", encoding="utf-8")
        command("add", ".")
        command("commit", "--quiet", "-m", "source baseline")
        return root, command, command("rev-parse", "HEAD")

    def complete_c1_fixture(self, root, base):
        inputs = "tests/one.inputs"
        (root / inputs).parent.mkdir(parents=True)
        (root / inputs).write_bytes(b"")
        ends = []
        streams = {}
        for role in ("reference_stream", "native_stream"):
            relative = role + ".snd"
            stream = root / relative
            stream.write_bytes(b"STVSND01" + struct.pack("<BIII", 9, 0, 1, 123))
            streams[role] = relative
            ends.append({"role": role, **runner.pin(stream), **runner.sound_endpoint(stream, 0), "path": relative})
        data = {"verified_at": base, "recorder_sha256": "exe", "rom_zip_sha256": "rom", "mame_sha256": runner.MAME_SHA,
                "jobs": [{"name": "1p", "state": "PASS_COMPLETE_STRICT", "frames": 0, "input": inputs,
                          "input_sha256": runner.sha(root / inputs), "exit_code": 0, **streams,
                          "strict_diff_command": "python tools/lockstep/sndfile.py diff reference_stream.snd native_stream.snd",
                          "strict_diff": {"exit_code": 0, "stdout": "NO_DIVERGENCE (both streams ended)", "endpoints": ends}}]}
        path = root / "c1.json"
        path.write_text(json.dumps(data), encoding="utf-8")
        return path, {"1p": (0, inputs)}

    def test_committed_sound_translator_and_ymfm_changes_invalidate_c1(self):
        for relative in ("tools/m6809_aot/translate.py", "third_party/ymfm/src/ymfm.cpp"):
            with self.subTest(source=relative):
                root, command, base = self.source_repo("translator" if "m6809" in relative else "ymfm")
                proof, cases = self.complete_c1_fixture(root, base)
                with patch.object(runner, "C1_CASES", cases):
                    self.assertEqual(runner.c1_dependency(root, proof, {"sha256": "exe"}, {"sha256": "rom"})["state"], "READY")
                    (root / relative).write_text("changed sound implementation\n", encoding="utf-8")
                    command("add", relative)
                    command("commit", "--quiet", "-m", "sound source change")
                    with self.assertRaisesRegex(runner.ProofError, "D10 source-touching commit"):
                        runner.c1_dependency(root, proof, {"sha256": "exe"}, {"sha256": "rom"})

    def test_reverted_source_commit_still_invalidates_old_proof(self):
        root, command, base = self.source_repo("reverted")
        relative = "tools/m6809_aot/translate.py"
        for content, message in (("changed\n", "source change"), ("initial\n", "revert source change")):
            (root / relative).write_text(content, encoding="utf-8")
            command("add", relative)
            command("commit", "--quiet", "-m", message)
        self.assertEqual(command("diff", "--name-only", base, "HEAD"), "")
        with self.assertRaisesRegex(runner.ProofError, "D10 source-touching commit"):
            runner.check_d10_sources(root, base)

    def test_doc_only_commit_is_allowed_and_finished_commit_is_recordable(self):
        root, command, base = self.source_repo("docs")
        (root / "PLAN.md").write_text("updated plan\n", encoding="utf-8")
        command("add", "PLAN.md")
        command("commit", "--quiet", "-m", "documentation only")
        finished = runner.check_d10_sources(root, base)
        self.assertNotEqual(finished, base)
        self.assertEqual(finished, command("rev-parse", "HEAD"))
        (root / "PLAN.md").write_text("uncommitted documentation\n", encoding="utf-8")
        self.assertEqual(runner.check_d10_sources(root, base), finished)
        (root / "tools/m6809_aot/translate.py").write_text("dirty sound translator\n", encoding="utf-8")
        with self.assertRaisesRegex(runner.ProofError, "uncommitted changes"):
            runner.check_d10_sources(root, base)

    def test_translation_index_is_read_as_utf8(self):
        path = self.path / "index.csv"
        path.write_text("address,instruction,source,chunk,status\n00000000,EXGPC A7,\u58f0.ASM:1,chunk,translated\n", encoding="utf-8")
        self.assertEqual(runner.translation_rows(path)[0]["source"], "\u58f0.ASM:1")

    def test_environment_is_child_only(self):
        # A real os.environ clear/restore drops empty Git config values on Windows.
        with patch.object(runner.os, "environ", dict(runner.os.environ, STV_LS_DUMP_EVERY="1", STV_SCHED_DBG="bad")):
            env = runner.child_environment()
            self.assertEqual(env["STV_LS_DUMP_EVERY"], "60")
            self.assertNotIn("STV_SCHED_DBG", env)
            self.assertEqual(runner.os.environ["STV_LS_DUMP_EVERY"], "1")

    def test_atomic_report_replaces_complete_json(self):
        path = self.path / "report.json"
        runner.atomic_json(path, {"state": "RUNNING"})
        runner.atomic_json(path, {"state": "PASS"})
        self.assertEqual(json.loads(path.read_text(encoding="utf-8")), {"state": "PASS"})
        self.assertNotIn(b"\r\n", path.read_bytes())
        self.assertEqual(list(self.path.glob("*.tmp")), [])

    def test_interruption_terminates_child_and_records_failure(self):
        report = runner.Report(self.path, {"steps": []})
        process = Mock(pid=123)
        process.wait.side_effect = [KeyboardInterrupt(), -15]
        process.poll.return_value = None
        with patch.object(runner.subprocess, "Popen", return_value=process), self.assertRaises(KeyboardInterrupt):
            report.run("interrupted", ["unused"], self.path)
        process.terminate.assert_called_once()
        data = json.loads(report.path.read_text(encoding="utf-8"))
        self.assertEqual(data["steps"][0]["state"], "FAILED")
        self.assertEqual(data["steps"][0]["exit_code"], -15)

    def test_parallel_cases_keep_atomic_report_and_all_owned_pids(self):
        report = runner.Report(self.path, {"steps": []})
        markers = self.path / "ready"
        markers.mkdir()
        errors, snapshots = [], []
        stop = threading.Event()
        def read_reports():
            last_read = time.monotonic()
            while not stop.is_set():
                try:
                    data = json.loads(report.path.read_text(encoding="utf-8"))
                    names = [step["name"] for step in data["steps"]]
                    self.assertEqual(len(names), len(set(names)))
                    snapshots.append(data)
                    last_read = time.monotonic()
                    protected = report.snapshot()
                    protected["steps"].append({"name": "external mutation"})
                    self.assertFalse(any(step["name"] == "external mutation" for step in report.snapshot()["steps"]))
                except PermissionError as error:
                    # Windows replacement can temporarily deny a reader open;
                    # successful reads must always be complete JSON snapshots.
                    if time.monotonic() - last_read > 2:
                        errors.append(error)
                        return
                except BaseException as error:
                    errors.append(error)
                    return
                stop.wait(0.002)
        reader = threading.Thread(target=read_reports)
        reader.start()
        script = ("from pathlib import Path\nimport sys,time\n"
                  "p=Path(sys.argv[1]);p.write_text('ready')\n"
                  "end=time.monotonic()+3\n"
                  "while len(list(p.parent.glob('*.ready')))<3 and time.monotonic()<end: time.sleep(.01)\n"
                  "sys.exit(0 if len(list(p.parent.glob('*.ready')))>=3 else 7)\n")
        cases = [(f"case{i}",) for i in range(6)]
        def work(case):
            name = case[0]
            directory = self.path / name
            directory.mkdir()
            report.run(name, [sys.executable, "-c", script, markers / (name + ".ready")], directory)
            return name
        try:
            results = runner.run_main_group(report, cases, 3, work)
        finally:
            stop.set()
            reader.join(timeout=3)
        self.assertEqual(results, [case[0] for case in cases])
        self.assertFalse(reader.is_alive())
        self.assertEqual(errors, [])
        self.assertTrue(snapshots)
        data = json.loads(report.path.read_text(encoding="utf-8"))
        self.assertEqual(data["main_group"]["state"], "PASS")
        self.assertEqual(len(data["steps"]), 6)
        self.assertEqual(len({step["pid"] for step in data["steps"]}), 6)
        self.assertTrue(all(step["state"] == "PASS" and step["exit_code"] == 0 and step["command"] for step in data["steps"]))
        self.assertEqual(report.children, {})

    def test_case_failure_terminates_live_sibling_and_cancels_queue(self):
        report = runner.Report(self.path, {"steps": []})
        ready = self.path / "sleep.ready"
        fail_script = ("from pathlib import Path\nimport sys,time\np=Path(sys.argv[1]);end=time.monotonic()+3\n"
                       "while not p.exists() and time.monotonic()<end: time.sleep(.01)\nsys.exit(9)\n")
        sleep_script = "from pathlib import Path;import sys,time;Path(sys.argv[1]).write_text('ready');time.sleep(6)"
        def work(case):
            name = case[0]
            directory = self.path / name
            directory.mkdir()
            report.run(name, [sys.executable, "-c", fail_script if name == "fail" else sleep_script, ready], directory)
        started = time.monotonic()
        with self.assertRaisesRegex(runner.ProofError, "process failed"):
            runner.run_main_group(report, [("fail",), ("sleep",), ("queued",)], 2, work)
        self.assertLess(time.monotonic() - started, 5)
        data = json.loads(report.path.read_text(encoding="utf-8"))
        states = {case["name"]: case["state"] for case in data["main_group"]["cases"]}
        self.assertEqual(states, {"fail": "FAILED", "sleep": "CANCELLED", "queued": "CANCELLED"})
        steps = {step["name"]: step for step in data["steps"]}
        self.assertEqual(steps["fail"]["exit_code"], 9)
        self.assertEqual(steps["sleep"]["state"], "CANCELLED")
        self.assertIn("pid", steps["sleep"])
        self.assertFalse(any(step["name"] == "queued" and "pid" in step for step in data["steps"]))
        self.assertEqual(report.children, {})

    def test_group_interruption_terminates_all_owned_live_children(self):
        report = runner.Report(self.path, {"steps": []})
        script = "from pathlib import Path;import sys,time;Path(sys.argv[1]).write_text('ready');time.sleep(6)"
        def work(case):
            name = case[0]
            directory = self.path / name
            directory.mkdir()
            report.run(name, [sys.executable, "-c", script, self.path / (name + ".ready")], directory)
        def interrupted(_):
            deadline = time.monotonic() + 3
            while len(list(self.path.glob("*.ready"))) < 2 and time.monotonic() < deadline:
                time.sleep(.01)
            self.assertEqual(len(list(self.path.glob("*.ready"))), 2)
            raise KeyboardInterrupt()
        with patch.object(runner, "as_completed", side_effect=interrupted), self.assertRaises(KeyboardInterrupt):
            runner.run_main_group(report, [("one",), ("two",)], 2, work)
        data = json.loads(report.path.read_text(encoding="utf-8"))
        self.assertTrue(all(step["state"] == "CANCELLED" and "pid" in step for step in data["steps"]))
        self.assertEqual(report.children, {})

    def test_worker_bound_rejects_values_outside_one_to_three(self):
        for workers in (0, 4):
            with self.subTest(workers=workers), contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as caught:
                runner.main(["--workers", str(workers)])
            self.assertEqual(caught.exception.code, 2)

    def test_inventory_is_seven_cases_and_indirect_hits_never_close_targets(self):
        self.assertEqual(len(runner.CASES), 7)
        self.assertEqual(len(runner.SNAPS), 12)
        index, codemap, cov = self.path / "index.csv", self.path / "codemap.json", self.path / "fresh.cov"
        index.write_text("address,instruction,source,chunk,status\n" + "".join(
            f"{i:08x},EXGPC A7,UTIL.ASM:{i},chunk,translated\n" for i in range(147)))
        codemap.write_text(json.dumps({"indirect_sites": [f"{i:08x}" for i in range(147)]}))
        cov.write_bytes(b"\x01" * 147)
        measured = runner.indirect_classifications(index, codemap, [cov])
        self.assertEqual(measured["target_closure"], "OPEN")
        self.assertTrue(all(row["fresh_coverage"] == "HIT" and row["target_closure"] == "OPEN" for row in measured["sites"]))


    def test_corrected_inventory_rejects_legacy_missing_duplicate_and_extra_sites(self):
        index, codemap = self.path / "index.csv", self.path / "codemap.json"
        index.write_text("address,instruction,source,chunk,status\n" + "".join(
            f"{i:08x},EXGPC A7,UTIL.ASM:{i},chunk,translated\n" for i in range(148)))
        for sites in ([f"{i:08x}" for i in range(128)], [f"{i:08x}" for i in range(146)],
                      [f"{i:08x}" for i in range(148)], [f"{i:08x}" for i in range(146)]+["00000000"]):
            with self.subTest(size=len(sites)):
                codemap.write_text(json.dumps({"indirect_sites":sites}))
                with self.assertRaisesRegex(runner.ProofError,"147 unique"):
                    runner.indirect_classifications(index,codemap)

    def test_correct_count_cannot_hide_missing_or_untranslated_instruction_rows(self):
        index, codemap = self.path / "index.csv", self.path / "codemap.json"
        codemap.write_text(json.dumps({"indirect_sites":[f"{i:08x}" for i in range(147)]}))
        for last in ("", "00000092,EXGPC A7,UTIL.ASM:146,chunk,blocked\n"):
            with self.subTest(last=last):
                index.write_text("address,instruction,source,chunk,status\n" + "".join(
                    f"{i:08x},EXGPC A7,UTIL.ASM:{i},chunk,translated\n" for i in range(146))+last)
                with self.assertRaisesRegex(runner.ProofError,"accepted translated"):
                    runner.indirect_classifications(index,codemap)

    def renewal_fixture(self):
        root, command, base = self.source_repo("renewal")
        proof, cases = self.complete_c1_fixture(root, base)
        data = json.loads(proof.read_text())
        data["recorder_sha256"] = "a" * 64
        proof.write_text(json.dumps(data))
        return root, command, proof, cases

    def test_explicit_renewal_reuses_reference_without_claiming_current_native(self):
        root, command, proof, cases = self.renewal_fixture()
        (root / "tools/m6809_aot/translate.py").write_text("new source\n")
        command("add", "."); command("commit", "--quiet", "-m", "new implementation")
        with patch.object(runner, "C1_CASES", cases):
            with self.assertRaises(runner.ProofError):
                runner.c1_dependency(root, proof, {"sha256": "b" * 64}, {"sha256": "rom"})
            result = runner.c1_dependency(root, proof, {"sha256": "b" * 64}, {"sha256": "rom"}, renew_native=True)
        self.assertEqual(result["state"], "REFERENCE_READY_NATIVE_RENEWAL_REQUIRED")
        self.assertFalse(result["native_gate_proof"])
        self.assertFalse(result["recorder_matches"])
        self.assertFalse(result["d10_sources_match"])

    def test_reference_renewal_rejects_non_commit_provenance(self):
        root, command, proof, cases = self.renewal_fixture()
        data = json.loads(proof.read_text())
        data["verified_at"] = command("hash-object", "tools/m6809_aot/translate.py")
        proof.write_text(json.dumps(data))
        with patch.object(runner, "C1_CASES", cases), self.assertRaisesRegex(runner.ProofError, "existing commit"):
            runner.c1_dependency(root, proof, {"sha256": "b" * 64}, {"sha256": "rom"}, renew_native=True)

    def test_patchpoint_and_bot_source_changes_invalidate_c1(self):
        for name, relative in (("patchpoints", "data/enhanced_patchpoints.json"), ("bot", "tools/lockstep/bot.hpp"), ("content", "tools/lockstep/content_observer.hpp")):
            with self.subTest(source=relative):
                root, command, base = self.source_repo(name)
                proof, cases = self.complete_c1_fixture(root, base)
                source = root / relative; source.parent.mkdir(parents=True, exist_ok=True); source.write_text("changed\n")
                command("add", relative); command("commit", "--quiet", "-m", "generation/harness input change")
                with patch.object(runner, "C1_CASES", cases), self.assertRaisesRegex(runner.ProofError, "D10 source-touching commit"):
                    runner.c1_dependency(root, proof, {"sha256": "exe"}, {"sha256": "rom"})

    def test_reference_renewal_preserves_pending_status(self):
        root, _, proof, cases = self.renewal_fixture()
        data = json.loads(proof.read_text()); data["jobs"][0]["state"] = "RUNNING"; proof.write_text(json.dumps(data))
        with patch.object(runner, "C1_CASES", cases):
            result = runner.c1_dependency(root, proof, {"sha256": "b" * 64}, {"sha256": "rom"}, renew_native=True)
        self.assertEqual(result["state"], "PENDING")
        self.assertFalse(result["native_gate_proof"])

    def test_reference_renewal_rejects_changed_stream(self):
        root, _, proof, cases = self.renewal_fixture()
        (root / "reference_stream.snd").write_bytes(b"STVSND01")
        with patch.object(runner, "C1_CASES", cases), self.assertRaises(runner.ProofError):
            runner.c1_dependency(root, proof, {"sha256": "b" * 64}, {"sha256": "rom"}, renew_native=True)

    def test_reference_renewal_rejects_changed_input(self):
        root, _, proof, cases = self.renewal_fixture()
        (root / "tests/one.inputs").write_text("different input\n")
        with patch.object(runner, "C1_CASES", cases), self.assertRaisesRegex(runner.ProofError, "input/frame"):
            runner.c1_dependency(root, proof, {"sha256": "b" * 64}, {"sha256": "rom"}, renew_native=True)

    def test_reference_renewal_rejects_wrong_oracle_identity(self):
        root, _, proof, cases = self.renewal_fixture()
        data = json.loads(proof.read_text()); data["mame_sha256"] = "c" * 64; proof.write_text(json.dumps(data))
        with patch.object(runner, "C1_CASES", cases), self.assertRaisesRegex(runner.ProofError, "ROM/MAME"):
            runner.c1_dependency(root, proof, {"sha256": "b" * 64}, {"sha256": "rom"}, renew_native=True)

    def test_reference_renewal_requires_historical_recorder_provenance(self):
        root, _, proof, cases = self.renewal_fixture()
        data = json.loads(proof.read_text()); data["recorder_sha256"] = ""; proof.write_text(json.dumps(data))
        with patch.object(runner, "C1_CASES", cases), self.assertRaisesRegex(runner.ProofError, "historical C1 recorder"):
            runner.c1_dependency(root, proof, {"sha256": "b" * 64}, {"sha256": "rom"}, renew_native=True)

    def test_strict_renewal_rejects_prefix_or_inconclusive_text(self):
        root, _, _, _ = self.renewal_fixture()
        reference, native = root / "reference_stream.snd", root / "native_stream.snd"
        for text in ("NO_DIVERGENCE (prefix)", "NO_DIVERGENCE (both streams ended): 1 frame summaries, 0 conclusive, last frame 0", "NO_DIVERGENCE (both streams ended)"):
            with self.subTest(text=text), self.assertRaises(runner.ProofError):
                runner.strict_sound_result(text, reference, native, 0)

    def test_strict_renewal_requires_equal_complete_endpoints(self):
        root, _, _, _ = self.renewal_fixture()
        reference, native = root / "reference_stream.snd", root / "native_stream.snd"
        text = "NO_DIVERGENCE (both streams ended): 1 frame summaries, 1 conclusive, last frame 0"
        self.assertEqual(len(runner.strict_sound_result(text, reference, native, 0)["endpoints"]), 2)
        native.write_bytes(b"STVSND01" + struct.pack("<BIII", 9, 0, 2, 123))
        with self.assertRaisesRegex(runner.ProofError, "endpoints differ"):
            runner.strict_sound_result(text, reference, native, 0)

    def test_renewed_c1_requires_fresh_successful_native_and_diff_steps(self):
        root, _, proof, cases = self.renewal_fixture()
        report = runner.Report(root, {"verified_at": runner.git(root, "rev-parse", "HEAD"), "steps": []})
        with patch.object(runner, "C1_CASES", cases), self.assertRaisesRegex(runner.ProofError, "must finish"):
            runner.renewed_c1_proof(report, proof, {"path": "exe", "sha256": "b" * 64}, {"sha256": "rom"})

    def test_renewed_c1_receipt_is_accepted_only_for_current_commit_and_recorder(self):
        root, _, proof, cases = self.renewal_fixture()
        reference, native = root / "reference_stream.snd", root / "native_stream.snd"
        text = "NO_DIVERGENCE (both streams ended): 1 frame summaries, 1 conclusive, last frame 0"
        steps = [{"name": "1p-native", "state": "PASS", "exit_code": 0, "command": ["fixture-native"]},
                 {"name": "1p-sound-diff", "state": "PASS", "exit_code": 0,
                  "command": ["python", "tools/lockstep/sndfile.py", "diff", str(reference), str(native)],
                  "proof": runner.strict_sound_result(text, reference, native, 0), "log": "fixture-diff.log", "log_sha256": "d" * 64}]
        report = runner.Report(root, {"verified_at": runner.git(root, "rev-parse", "HEAD"), "steps": steps})
        with patch.object(runner, "C1_CASES", cases), patch.object(runner, "ROOT", root):
            receipt = runner.renewed_c1_proof(report, proof, {"path": "exe", "sha256": "b" * 64}, {"sha256": "rom"})
            output = root / "renewed.json"; output.write_text(json.dumps(receipt))
            result = runner.c1_dependency(root, output, {"sha256": "b" * 64}, {"sha256": "rom"})
            self.assertEqual(result["state"], "READY")
            self.assertTrue(result["native_gate_proof"])
            with self.assertRaisesRegex(runner.ProofError, "recorder provenance"):
                runner.c1_dependency(root, output, {"sha256": "a" * 64}, {"sha256": "rom"})

class ResumeTests(unittest.TestCase):
    # Reuse only the fixture helpers, without rerunning inherited tests.
    setUp = RebaselineTests.setUp
    main_stream = RebaselineTests.main_stream
    sound_stream = RebaselineTests.sound_stream
    def resume_fixture(self):
        root = self.path
        output = root / 'build/prior'
        output.mkdir(parents=True)
        native = root / 'build/native.exe'
        native.write_bytes(b'fixture executable, never run')
        rom = root / 'original/user-supplied-roms/smashtv.zip'
        rom.parent.mkdir(parents=True)
        rom.write_bytes(b'fixture ROM, never imported')
        inputs = root / 'tests/scenarios/unit.inputs'
        inputs.parent.mkdir(parents=True)
        inputs.write_bytes(b'')
        oracle = root / 'build/oracle/ls.bin'
        oracle.parent.mkdir()
        stream = output / 'native.bin'
        stream.write_bytes(self.main_stream('temp.bin', [0,1,2]).read_bytes())
        oracle.write_bytes(stream.read_bytes())
        sound = output / 'native.snd'
        sound.write_bytes(self.sound_stream('temp.snd', [0,1,2]).read_bytes())
        cov = output / 'native.cov'
        cov.write_bytes(b'\x01')
        content = output / 'content.jsonl'
        content.write_text(json.dumps(CONTENT_META)+'\n'+json.dumps(content_row(0))+'\n')
        log = output / 'native.log'
        log.write_text('fixture complete')
        proof = dict(endpoint=runner.main_endpoint(stream,2),stream=runner.pin(stream),
                     sound_stream=runner.pin(sound),coverage=runner.pin(cov),content_stream=runner.pin(content),content_samples=runner.content_samples(content,2))
        step = dict(name='unit-native',state='PASS',exit_code=0,proof=proof,log=str(log),log_sha256=runner.sha(log),
                    command=list(map(str,[native,rom,stream,2,'--native-hcount','--inputs',inputs,'--snd',sound,'--coverage',cov,'--content',content])),
                    environment={k:v for k,v in runner.child_environment().items() if k.startswith('STV_')})
        files = [native,rom,inputs,oracle]
        data = dict(verified_at='a'*40,result='FAILED',owned_live_pids=[],main_group=dict(state='PASS',cases=[dict(name='unit',state='PASS')]),
                    pins={str(p.resolve()):runner.pin(p) for p in files},steps=[step])
        return root,output/'report.json',native,files,root/'new-c1.json',data

    def check_resume(self, fixture):
        root,path,native,files,c1,data = fixture
        path.write_text(json.dumps(data))
        with patch.object(runner,'CASES',(('unit',2,'unit.inputs','oracle'),)), patch.object(runner,'check_d10_sources',return_value='b'*40):
            return runner.resume_recordings(root,path,native,files,c1)

    def test_resume_complete_capture_pinned(self):
        captures,pins = self.check_resume(self.resume_fixture())
        self.assertEqual(list(captures),['unit'])
        self.assertEqual(len(pins),6)
        self.assertEqual(captures['unit']['state'],'PASS')

    def test_resume_rejects_running_or_live_report(self):
        for field,value in [('result','RUNNING'),('owned_live_pids',[123])]:
            f=self.resume_fixture()
            f[-1][field]=value
            with self.assertRaisesRegex(runner.ProofError,'terminal report'):
                self.check_resume(f)
            self.tearDown_fixture(f)

    def tearDown_fixture(self, f):
        # Each test case uses a distinct temporary fixture, preserving mutations.
        self.temp.cleanup()
        self.temp=tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path=Path(self.temp.name)

    def test_resume_rejects_incomplete_main_group(self):
        f=self.resume_fixture();f[-1]['main_group']['cases'][0]['state']='FAILED'
        with self.assertRaisesRegex(runner.ProofError,'every completed'):
            self.check_resume(f)

    def test_resume_rejects_prerequisite_change_or_missing_pin(self):
        for changed in (True,False):
            f=self.resume_fixture()
            if changed:f[3][0].write_bytes(b'changed')
            else:f[-1]['pins'].pop(str(f[3][0].resolve()))
            with self.assertRaises(runner.ProofError):self.check_resume(f)
            self.tearDown_fixture(f)

    def test_resume_rejects_artifact_change(self):
        for field in ('stream','sound_stream','coverage','content_stream'):
            f=self.resume_fixture();Path(f[-1]['steps'][0]['proof'][field]['path']).write_bytes(b'changed')
            with self.assertRaisesRegex(runner.ProofError,'changed'):self.check_resume(f)
            self.tearDown_fixture(f)

    def test_resume_rejects_wrong_command_or_environment(self):
        for field,value in [('command',['fake']),('environment',{})]:
            f=self.resume_fixture();f[-1]['steps'][0][field]=value
            with self.assertRaisesRegex(runner.ProofError,'differs'):self.check_resume(f)
            self.tearDown_fixture(f)

    def test_resume_rejects_changed_log(self):
        f=self.resume_fixture();Path(f[-1]['steps'][0]['log']).write_text('changed')
        with self.assertRaisesRegex(runner.ProofError,'log changed'):self.check_resume(f)

    def test_resume_rejects_duplicate_or_failed_native_step(self):
        for duplicate in (True,False):
            f=self.resume_fixture()
            if duplicate:f[-1]['steps'].append(f[-1]['steps'][0])
            else:f[-1]['steps'][0]['exit_code']=1
            with self.assertRaisesRegex(runner.ProofError,'incomplete/duplicate'):self.check_resume(f)
            self.tearDown_fixture(f)

    def test_resume_rejects_wrong_endpoint_or_missing_source(self):
        for missing in (True,False):
            f=self.resume_fixture()
            if missing:f[-1]['verified_at']=''
            else:f[-1]['steps'][0]['proof']['endpoint']['last_frame']=1
            with self.assertRaises(runner.ProofError):self.check_resume(f)
            self.tearDown_fixture(f)

    def test_command_capture_enables_sound_scheduler(self):
        cmd=runner.command_capture(Path('native.exe'),Path('rom.zip'),self.path)
        self.assertEqual(cmd[cmd.index('--snd')+1],self.path/'native.snd')
        self.assertEqual(cmd[cmd.index('--wav')+1],self.path/'native.wav')
        self.assertEqual(cmd[-2:],['--snd-commands','600,150,0,255'])


class ContentTests(unittest.TestCase):
    setUp = RebaselineTests.setUp
    def capture(self, rows, final=60, meta=None):
        p=self.path/'content.jsonl'
        p.write_text('\n'.join(json.dumps(x) for x in [meta if meta is not None else CONTENT_META,*rows])+'\n')
        return runner.content_samples(p,final)

    def test_complete_sample_cadence_and_signed_coordinates(self):
        result=self.capture([content_row(0),content_row(60)])
        self.assertEqual(result['samples'],2)
        self.assertEqual(result['last_sample_frame'],60)
        self.assertFalse(result['content_gate_proof'])

    def test_post_boundaries_explicit_and_not_absence(self):
        row=content_row(0)
        row['objects']=[dict(boundary_address=0xffffffff,reason='invalid_or_cyclic_ram_node')]
        row['object_list_complete']=False
        self.assertEqual(self.capture([row],0)['object_boundary_samples'],1)

    def test_missing_endpoint_rejected(self):
        with self.assertRaisesRegex(runner.ProofError,'incomplete'):
            self.capture([content_row(0)])

    def test_bad_cadence_duplicate_or_boolean_frame_rejected(self):
        for value in (1,0,True):
            with self.subTest(value=value),self.assertRaises(runner.ProofError):
                self.capture([content_row(0),content_row(value)])

    def test_wrong_schema_or_phase_rejected(self):
        for key,value in [('phase','after_game_step'),('cadence',1),('cadence',60.0),('schema','unknown')]:
            meta=dict(CONTENT_META);meta[key]=value
            with self.subTest(key=key),self.assertRaises(runner.ProofError):self.capture([content_row(0)],0,meta)

    def test_wrong_raw_word_or_coordinate_rejected(self):
        for key,value in [('circuit',True),('wave',-1),('gamstate',65536),('world_x',-32769),('world_y',32768)]:
            row=content_row(0);row[key]=value
            with self.subTest(key=key),self.assertRaises(runner.ProofError):self.capture([row],0)

    def test_named_boundary_cannot_be_complete(self):
        row=content_row(0);row['objects']=[dict(boundary_address=1,reason='invalid_or_cyclic_ram_node')]
        with self.assertRaises(runner.ProofError):self.capture([row],0)

    def test_incomplete_list_requires_named_boundary(self):
        row=content_row(0);row['process_list_complete']=False
        with self.assertRaisesRegex(runner.ProofError,'lacks a named'):self.capture([row],0)

    def test_raw_process_fields_and_unaligned_pdata_value(self):
        row=content_row(0);row['processes']=[dict(address=0x01002000,pid=128,wake=0xffe337a0,saved_sp=0x01002200,
            pdata198_raw=4,saved_a8=0x01086220,saved_a9=0,saved_a10=0,saved_a11=0)]
        self.assertEqual(self.capture([row],0)['process_boundary_samples'],0)

    def test_object_addresses_bounds_duplicates_and_shapes_rejected(self):
        item=dict(address=0x01001000,oid=0x4101,image=0xffe12340,x=-172,y=46)
        for rows in ([dict(item,address=0x01001008)],[dict(item,address=0x010ffff0)],[item,item],[dict(item,oid=True)],[dict(item,extra=1)]):
            row=content_row(0);row['objects']=rows
            with self.subTest(rows=rows),self.assertRaises(runner.ProofError):self.capture([row],0)

    def test_truncated_json_rejected(self):
        p=self.path/'partial.jsonl';p.write_text(json.dumps(CONTENT_META)+'\n'+json.dumps(content_row(0))[:-1])
        with self.assertRaises(ValueError):runner.content_samples(p,0)


if __name__ == "__main__":
    unittest.main()
