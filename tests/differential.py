#!/usr/bin/env python3
"""Independent stdlib oracles for utility code. NOT MAME/game-logic differential validation."""
from __future__ import annotations
import argparse
import hashlib
import io
import json
import random
import struct
import subprocess
import tempfile
import zipfile
import zlib
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tool", type=Path)
    parser.add_argument("--synthetic", type=Path, help="Output of stv_tests FIXTURE_DIRECTORY")
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    tool = str(args.tool.resolve())
    counts = {"hash_cases": 0, "inflate_cases": 0, "inflate_rejections": 0,
              "zip_cases": 0, "zip_rejections": 0, "synthetic_package_checks": 0}
    block_types: set[int] = set()
    rng = random.Random(0x534D4153)

    def run(*arguments: object, success: bool = True) -> subprocess.CompletedProcess[str]:
        result = subprocess.run([tool, *map(str, arguments)], capture_output=True, text=True, timeout=30)
        if success and result.returncode:
            raise AssertionError(f"Command failed {arguments}: {result.stdout}\n{result.stderr}")
        if not success and result.returncode == 0:
            raise AssertionError(f"Invalid fixture was accepted: {arguments}")
        return result

    with tempfile.TemporaryDirectory(prefix="stv-differential-") as td:
        root = Path(td)
        data_path, raw_path = root / "input.bin", root / "input.deflate"
        out_path = root / "output.bin"
        datasets = [b"", b"a", b"abc", b"\x00" * 100000, bytes(range(256)) * 600,
                    b"faithful native port fixture\n" * 4000]
        datasets += [rng.randbytes(n) for n in [2, 7, 31, 55, 56, 63, 64, 65, 127, 128, 129, 1023, 32768, 65536]]
        datasets += [bytes(rng.choices(range(24), weights=range(24, 0, -1), k=rng.randrange(1, 16000))) for _ in range(24)]
        for data in datasets:
            data_path.write_bytes(data)
            got = json.loads(run("hash", data_path).stdout)
            expected = {"size": len(data), "crc32": zlib.crc32(data), "sha1": hashlib.sha1(data).hexdigest(),
                        "sha256": hashlib.sha256(data).hexdigest()}
            assert got == expected, (got, expected)
            counts["hash_cases"] += 1
            for level, strategy in [(0, zlib.Z_DEFAULT_STRATEGY), (6, zlib.Z_FIXED), (6, zlib.Z_DEFAULT_STRATEGY)]:
                c = zlib.compressobj(level, zlib.DEFLATED, -15, 8, strategy)
                raw = c.compress(data) + c.flush()
                block_types.add((raw[0] >> 1) & 3)
                raw_path.write_bytes(raw)
                out_path.unlink(missing_ok=True)
                run("inflate", raw_path, len(data), out_path)
                assert out_path.read_bytes() == data
                counts["inflate_cases"] += 1
        assert block_types == {0, 1, 2}, f"Not all DEFLATE block types were exercised: {block_types}"
        raw = zlib.compress(b"fixture" * 30, wbits=-15)
        for damaged, length in [(raw[:-1], 210), (raw + b"junk", 210), (raw, 209), (raw, 211), (b"\x07", 0), (b"", 0)]:
            raw_path.write_bytes(damaged)
            out_path.unlink(missing_ok=True)
            run("inflate", raw_path, length, out_path, success=False)
            assert not out_path.exists()
            counts["inflate_rejections"] += 1

        class NonSeekable(io.RawIOBase):
            def __init__(self) -> None:
                super().__init__()
                self.value = bytearray()
            def writable(self) -> bool:
                return True
            def write(self, value: bytes) -> int:
                self.value.extend(value)
                return len(value)

        for method in [zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED]:
            for descriptor in [False, True]:
                sink = NonSeekable() if descriptor else io.BytesIO()
                names = ["first.bin", "folder/second.bin", "renamed without extension", "empty.bin"]
                payload = [rng.randbytes(4096), b"123xyz" * 4000, bytes(range(256)), b""]
                with zipfile.ZipFile(sink, "w", compression=method, allowZip64=False) as z:
                    for name, content in zip(names, payload):
                        z.writestr(name, content)
                archive = bytes(sink.value) if descriptor else sink.getvalue()
                archive_path = root / "fixture.zip"
                archive_path.write_bytes(archive)
                result = json.loads(run("zip-report", archive_path).stdout)
                assert result["count"] == len(names)
                for index, entry in enumerate(result["entries"]):
                    assert entry == {"name": names[index], "size": len(payload[index]), "method": method,
                                     "crc32": zlib.crc32(payload[index])}
                    out_path.unlink(missing_ok=True)
                    run("zip-extract", archive_path, index, out_path)
                    assert out_path.read_bytes() == payload[index]
                counts["zip_cases"] += 1

        for name in ["../escape", "/absolute", "D:\\outside", "safe/../../bad", "safe\\..\\bad"]:
            archive_path = root / "bad.zip"
            with zipfile.ZipFile(archive_path, "w", compression=zipfile.ZIP_DEFLATED) as z:
                z.writestr(name, b"test")
            run("zip-report", archive_path, success=False)
            counts["zip_rejections"] += 1
        with zipfile.ZipFile(root / "bad.zip", "w") as z:
            z.writestr("A.bin", b"first")
            z.writestr("a.BIN", b"second")
        run("zip-report", root / "bad.zip", success=False)
        counts["zip_rejections"] += 1
        # Main/sound ROM code is never executed. A file merely named like a known ROM must not pass.
        with zipfile.ZipFile(root / "fake-rom.zip", "w") as z:
            z.writestr("la8_smash_tv_game_rom_u105.u105", b"\x00" * 0x20000)
        result = run("import", root / "fake-rom.zip", root / "should-not-exist", success=False)
        assert "MISSING/INCOMPATIBLE" in result.stderr and not (root / "should-not-exist").exists()
        counts["zip_rejections"] += 1

    if args.synthetic:
        fixture = args.synthetic.resolve()
        raw = (fixture / "synthetic-input.zip").read_bytes()
        package = (fixture / "synthetic-package.stvdata").read_bytes()
        manifest = json.loads((fixture / "synthetic-manifest.json").read_text())
        assert manifest["archive_sha256"] == hashlib.sha256(raw).hexdigest()
        assert manifest["package_sha256"] == hashlib.sha256(package).hexdigest()
        assert package[:8] == b"STVDATA\0" and struct.unpack_from("<I", package, 16)[0] == 0x54455354
        assert package[40:72] == hashlib.sha256(package[72:]).digest()
        counts["synthetic_package_checks"] += 4
        with zipfile.ZipFile(io.BytesIO(raw)) as z:
            for index, record in enumerate(manifest["inputs"]):
                payload = z.read(record["archive_entry"])
                assert record["sha1"] == hashlib.sha1(payload).hexdigest()
                assert record["sha256"] == hashlib.sha256(payload).hexdigest()
                assert record["size"] == len(payload)
                role, size, crc, region, offset, stride, reload_offset = struct.unpack_from("<7I", package, 128 + index * 96)
                region_offset = struct.unpack_from("<Q", package, 72 + region * 8)[0]
                assert (role, size, crc) == (index, len(payload), zlib.crc32(payload))
                assert bytes(package[region_offset + offset + j * stride] for j in range(size)) == payload
                if reload_offset:
                    assert package[region_offset + reload_offset:region_offset + reload_offset + size] == payload
                counts["synthetic_package_checks"] += 5 + bool(reload_offset)
        # Synthetic payloads must NOT satisfy the shipping reference profile.
        run("verify", fixture / "synthetic-package.stvdata", success=False)
        counts["synthetic_package_checks"] += 1

    report = {"result": "PASS", "oracles": ["Python hashlib", "Python zlib", "Python zipfile", "independent struct parsing"],
              "deflate_block_types_exercised": sorted(block_types), "counts": counts,
              "original_roms_used": False, "mame_differential": "NOT RUN", "windows_execution": "NOT RUN"}
    text = json.dumps(report, indent=2) + "\n"
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(text)
    print(text, end="")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
