"""Prepare native player sources using only the ROM selected for this installation."""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import zipfile
import zlib

ROOT = Path(__file__).resolve().parents[2]
PROFILE = ROOT / "docs/reference-rom-profile.json"
LOCATIONS = ROOT / "tools/player/l8_locations.json"
sys.path.insert(0, str(ROOT / "tools/gsp2cpp"))
import compare
from codemap import unresolved_transfers
from decode import Decoder


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def read_chips(selected):
    profile = json.loads(PROFILE.read_bytes())["roms"]
    expected = {(r["size"], r["crc32"]): r for r in profile}
    found = {}
    sizes = {r["size"] for r in profile}
    clone_crcs = {0x33b626c3, 0x5f6fbc25, 0xa50ccb71, 0xef0b0279,
                  0x81f564b9, 0xe5017d25, 0xf1666017, 0x908aca5d}

    def offer(data):
        """Match one candidate file by content (size, CRC32, SHA-1), never by name."""
        record = expected.get((len(data), zlib.crc32(data)))
        if record and record["name"] not in found and hashlib.sha1(data).hexdigest() == record["sha1"]:
            found[record["name"]] = data

    def collect_zip(archive, depth):
        """Merged, split, non-merged and renamed sets; clone subfolders and
        ZIPs inside the ZIP (collection packs) are searched too."""
        clone = False
        members = archive.infolist()
        if len(members) > 4096:
            raise ValueError("Too many members in ROM archive")
        for item in members:
            if item.is_dir():
                continue
            if item.CRC in clone_crcs and item.file_size == 0x20000:
                clone = True
            if (item.file_size, item.CRC) in expected:
                offer(archive.read(item))
            elif (depth < 2 and item.filename.lower().endswith(".zip")
                  and item.file_size <= 64 * 1024 * 1024):
                try:
                    with zipfile.ZipFile(io.BytesIO(archive.read(item))) as inner:
                        clone |= collect_zip(inner, depth + 1)
                except zipfile.BadZipFile:
                    pass
        return clone

    def collect(path):
        if path.is_dir():
            # An extracted set: loose chip files and/or ZIPs in the folder tree.
            clone = False
            for child in sorted(path.rglob("*")):
                if not child.is_file():
                    continue
                if child.suffix.lower() == ".zip":
                    try:
                        clone |= collect(child)
                    except (zipfile.BadZipFile, ValueError, OSError):
                        pass
                elif child.stat().st_size in sizes:
                    offer(child.read_bytes())
            return clone
        if path.stat().st_size > 256 * 1024 * 1024:
            raise ValueError(f"ROM archive exceeds 256 MiB: {path.name}")
        with zipfile.ZipFile(path) as archive:
            return collect_zip(archive, 0)

    clone = collect(selected)
    if (found or clone) and len(found) < len(profile) and selected.is_file():
        # Split clone sets keep their parent beside them; MAME parent names vary.
        for sibling in sorted(selected.parent.glob("*.zip")):
            if sibling != selected and "smash" in sibling.name.lower():
                try:
                    collect(sibling)
                except (zipfile.BadZipFile, ValueError, OSError):
                    pass
            if len(found) == len(profile):
                break
    missing = [r["name"] for r in profile if r["name"] not in found]
    if missing:
        raise ValueError("This ZIP does not supply the retail L8 set. For split sets, "
                         "keep the parent ZIP beside it. Missing: " + ", ".join(missing))
    return [(r["name"], found[r["name"]]) for r in profile]


def prepare(selected, cache, generated):
    chips = read_chips(selected)
    cache.parent.mkdir(parents=True, exist_ok=True)
    # A stable normalized archive also handles merged sets and renamed members.
    if selected.resolve() != cache.resolve():
        with tempfile.TemporaryDirectory(dir=cache.parent) as td:
            temporary = Path(td) / "selected.zip"
            with zipfile.ZipFile(temporary, "w") as archive:
                for name, data in chips:
                    archive.writestr(zipfile.ZipInfo(name), data)
            os.replace(temporary, cache)
    generators = [Path(__file__), PROFILE, LOCATIONS]
    for directory in ("tools/gsp2cpp", "tools/m6809_aot"):
        generators.extend(sorted((ROOT / directory).glob("*.py")))
    generators += [ROOT / "tools/gsp2cpp/isa_table.json", *sorted((ROOT / "data").glob("enhanced*.json"))]
    key = hashlib.sha256()
    for _, content in chips:
        key.update(content)
    for path in generators:
        key.update(bytes.fromhex(digest(path)))
    destination = generated / key.hexdigest()
    if destination.exists():
        sealed = json.loads((destination / "outputs.json").read_bytes())
        if sealed != output_hashes(destination):
            raise ValueError(f"Generated native files changed: {destination}")
        return destination
    generated.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=generated) as td:
        root = Path(td)
        output = root / "program"
        locations = {}
        for source, rows in json.loads(LOCATIONS.read_bytes()).items():
            for offset, line in rows:
                locations[f"{0xffe00000 + offset:08x}"] = [0, 0, f"{source}:{line}" if source else line]
        compare.ZIP = str(cache)
        rom = compare.retail("l8")
        words = [rom[index] | (rom[index + 1] << 8) for index in range(0, len(rom), 2)]
        decoder = Decoder()
        decoded = {}
        for address, annotation in locations.items():
            pc = int(address, 16)
            instruction = decoder.decode(words, (pc - 0xffe00000) // 16, pc)
            if instruction is None:
                raise ValueError(f"Invalid translated instruction location: {address}")
            decoded[pc] = instruction
            annotation[:2] = [instruction.words, instruction.op]
        codemap = root / "locations.json"
        codemap.write_text(json.dumps({
            "rev": "l8",
            "note": "derived from the user-selected ROM; private",
            "instructions": locations,
            "vectors": [],
            "indirect_sites": [f"{pc:08x}" for pc in
                               sorted(set(unresolved_transfers(decoded)))],
            "conflicts": [],
        }), encoding="utf-8")
        subprocess.run([sys.executable, ROOT / "tools/gsp2cpp/emit.py", "--rom", cache,
                        "--codemap", codemap, "--out", output / "l8"], check=True)
        (output / "l8_codemap.json").write_text(codemap.read_text(encoding="utf-8"),
                                                encoding="utf-8")
        index = (output / "l8/l8_translation_index.csv").read_text().splitlines()[1:]
        if len(index) != len(locations) or any(not row.endswith(",translated") for row in index):
            raise ValueError("Incomplete main program translation")
        subprocess.run([sys.executable, ROOT / "tools/m6809_aot/emit6809.py", cache,
                        "--out", output / "snd"], check=True)
        (output / "outputs.json").write_text(json.dumps(output_hashes(output)), encoding="utf-8")
        try:
            output.rename(destination)
        except FileExistsError:
            if output_hashes(output) != output_hashes(destination):
                raise
    return destination


def output_hashes(directory):
    return {p.relative_to(directory).as_posix(): digest(p)
            for p in directory.rglob("*") if p.is_file() and p.name != "outputs.json"}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ("rom", "cache", "generated", "result"):
        parser.add_argument("--" + flag, required=True, type=Path)
    args = parser.parse_args()
    try:
        path = prepare(args.rom.resolve(), args.cache.resolve(), args.generated.resolve())
        args.result.write_text(str(path), encoding="utf-8")
    except (OSError, ValueError, zipfile.BadZipFile, subprocess.CalledProcessError) as error:
        sys.exit(str(error))
