"""Fail-closed checks for the development stream comparators; no ROMs required."""
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def ls_frame(frame, payload=b"", kind=9):
    return struct.pack("<BIH", kind, frame, len(payload)) + payload + struct.pack("<BIH", 8, frame, 0)


def snd_frame(frame, count=10, digest=111):
    return struct.pack("<BIII", 9, frame, count, digest)


def snd_access(frame=1, data=2, pc=3):
    return struct.pack("<BIIQHBH", 1, frame, 0, 100, 0x2000, data, pc)


class Comparators(unittest.TestCase):
    def compare(self, tool, a, b, ok, *options):
        magic = b"STVLS001" if tool == "lsfile" else b"STVSND01"
        with tempfile.TemporaryDirectory() as directory:
            pa, pb = Path(directory) / "a.bin", Path(directory) / "b.bin"
            pa.write_bytes(magic + a)
            pb.write_bytes(magic + b)
            result = subprocess.run([sys.executable, str(ROOT / f"tools/lockstep/{tool}.py"),
                                     "diff", str(pa), str(pb), *options], capture_output=True, text=True)
        self.assertEqual(result.returncode == 0, ok, result.stdout + result.stderr)

    def test_main_equal(self):
        self.compare("lsfile", ls_frame(1, b"abc"), ls_frame(1, b"abc"), True)

    def test_main_changed(self):
        self.compare("lsfile", ls_frame(1, b"abc"), ls_frame(1, b"abd"), False)

    def test_main_short_prefix_both_directions(self):
        for a, b in [(ls_frame(1), ls_frame(1) + ls_frame(2)), (ls_frame(1) + ls_frame(2), ls_frame(1))]:
            with self.subTest(a=len(a), b=len(b)):
                self.compare("lsfile", a, b, False)

    def test_main_explicit_prefix(self):
        self.compare("lsfile", ls_frame(1), ls_frame(1) + ls_frame(2), True, "--allow-prefix")

    def test_main_empty(self):
        self.compare("lsfile", b"", b"", False)

    def test_main_incomplete_frame(self):
        unfinished = struct.pack("<BIH", 9, 1, 3) + b"abc"
        self.compare("lsfile", unfinished, unfinished, False)

    def test_main_truncated_record(self):
        self.compare("lsfile", ls_frame(1), ls_frame(1) + b"\x08", False)

    def test_main_compact_ram_mismatch(self):
        self.compare("lsfile", ls_frame(1, struct.pack("<HI", 0, 123), 1),
                     ls_frame(1, struct.pack("<HI", 0, 124), 1), False)

    def test_sound_equal(self):
        self.compare("sndfile", snd_frame(1), snd_frame(1), True)

    def test_sound_changed_hash(self):
        self.compare("sndfile", snd_frame(1), snd_frame(1, digest=999), False)

    def test_sound_short_prefix_both_directions(self):
        for a, b in [(snd_frame(1), snd_frame(1) + snd_frame(2)), (snd_frame(1) + snd_frame(2), snd_frame(1))]:
            with self.subTest(a=len(a), b=len(b)):
                self.compare("sndfile", a, b, False)

    def test_sound_explicit_prefix(self):
        self.compare("sndfile", snd_frame(1), snd_frame(1) + snd_frame(2), True, "--allow-prefix")

    def test_sound_empty(self):
        self.compare("sndfile", b"", b"", False)

    def test_sound_inconclusive_end(self):
        self.compare("sndfile", snd_frame(1), snd_frame(1, count=11), False)

    def test_sound_straddling_vblank(self):
        self.compare("sndfile", snd_frame(1) + snd_frame(2, count=20, digest=222),
                     snd_frame(1, count=11, digest=112) + snd_frame(2, count=20, digest=222), True)

    def test_sound_access_changed(self):
        self.compare("sndfile", snd_access() + snd_frame(1), snd_access(data=4) + snd_frame(1), False)

    def test_sound_pc_is_context_only(self):
        self.compare("sndfile", snd_access() + snd_frame(1), snd_access(pc=4) + snd_frame(1), True)

    def test_sound_missing_access(self):
        self.compare("sndfile", snd_access() + snd_frame(1), snd_frame(1), False)

    def test_sound_main_command_changed(self):
        def command(data):
            return struct.pack("<BIIQH", 3, 1, 0, 100, data) + snd_frame(1)
        self.compare("sndfile", command(1), command(2), False)

    def test_sound_truncated_record(self):
        self.compare("sndfile", snd_frame(1), snd_frame(1) + b"\x09", False)

    def test_sound_unknown_record(self):
        self.compare("sndfile", b"\xff", b"\xff", False)


def record(kind, frame, payload=b''):
    return struct.pack('<BIH', kind, frame, len(payload)) + payload

def dump(region=1, frame=0, count=512):
    return b''.join(record(region, frame, struct.pack('<H', i) + bytes([i % 256]) * 256) for i in range(count)) + record(8, frame)

class State(unittest.TestCase):
    def run_state(self, body, *, frame=0, region='ram', good=False, expect=None):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source, output = root/'stream.bin', root/'state.bin'
            source.write_bytes(b'STVLS001' + body)
            p = subprocess.run([sys.executable, str(ROOT / "tools/lockstep/lsfile.py"), 'state', str(source), str(frame), region, str(output)], capture_output=True, text=True)
            self.assertEqual(p.returncode == 0, good, p.stdout + p.stderr)
            self.assertEqual(output.exists(), good, p.stdout + p.stderr)
            if good and expect is not None:
                self.assertEqual(output.read_bytes(), expect)

    def test_complete_ram(self):
        self.run_state(dump(), good=True, expect=b''.join(bytes([i%256])*256 for i in range(512)))

    def test_complete_palette(self):
        self.run_state(dump(2,count=64), region='palette', good=True, expect=b''.join(bytes([i])*256 for i in range(64)))

    def test_complete_cmos(self):
        self.run_state(dump(3,count=32), region='cmos', good=True, expect=b''.join(bytes([i])*256 for i in range(32)))

    def test_delta_keeps_fixed_size_and_unchanged_chunks(self):
        changed = bytes([99])*256
        self.run_state(dump() + record(1, 1, struct.pack('<H', 5)+changed) + record(8, 1), frame=1,
                       good=True, expect=b''.join(changed if i==5 else bytes([i%256])*256 for i in range(512)))

    def test_compact_rejected(self):
        self.run_state(record(1,0,struct.pack('<HI',0,123))+record(8,0))

    def test_mixed_compact_rejected(self):
        self.run_state(dump()+record(1,1,struct.pack('<HI',0,123))+record(8,1),frame=1)

    def test_empty_rejected(self):
        self.run_state(b'')

    def test_missing_initial_chunks_rejected(self):
        self.run_state(dump(count=511))

    def test_truncated_header_rejected(self):
        self.run_state(dump()[:-7]+b'\x08')

    def test_truncated_payload_rejected(self):
        self.run_state(record(1,0,struct.pack('<H',0)+b'a'*256)[:-1])

    def test_unfinished_frame_rejected(self):
        self.run_state(dump()[:-7])

    def test_unsampled_frame_rejected(self):
        self.run_state(dump()+record(8,1),frame=1)

    def test_missing_frame_rejected(self):
        self.run_state(dump()+record(8,2),frame=1)

    def test_chunk_index_rejected(self):
        self.run_state(dump()[:-7]+record(1,0,struct.pack('<H',512)+b'a'*256)+record(8,0))

    def test_negative_frame_rejected(self):
        self.run_state(dump(),frame=-1)

    def test_record_frame_disagreement_rejected(self):
        self.run_state(dump()[:-7]+record(8,1),frame=1)

    def test_frame_order_rejected(self):
        self.run_state(dump(frame=1)+record(8,0),frame=2)

    def test_unknown_payload_size_rejected(self):
        self.run_state(record(1,0,b'abc')+record(8,0))

    def test_unrequested_compact_region_does_not_break_ram(self):
        self.run_state(record(2,0,struct.pack('<HI',0,123))+dump(),good=True)


if __name__ == "__main__":
    unittest.main()
