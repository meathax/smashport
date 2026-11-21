"""Compare the private extracted source and supplied ROM ZIP with recorded hashes."""
import argparse
import hashlib
import json
from pathlib import Path


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("source_root", type=Path)
    parser.add_argument("rom_zip", type=Path)
    args = parser.parse_args()
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    files = manifest["files"]
    if len(files) != manifest["source_files"]:
        raise ValueError("Source-file manifest count differs")
    checked = set()
    for item in files:
        path = Path(item["path"])
        if path.parts[0] != "smashtv-main" or ".." in path.parts or path.is_absolute():
            raise ValueError(f"Invalid manifest path: {path}")
        relative = Path(*path.parts[1:])
        if relative in checked:
            raise ValueError(f"Duplicate manifest path: {relative}")
        checked.add(relative)
        source = args.source_root / relative
        if source.stat().st_size != item["bytes"] or sha256(source) != item["sha256"]:
            raise ValueError(f"Original source differs: {relative}")
    found = {p.relative_to(args.source_root) for p in args.source_root.rglob("*") if p.is_file()}
    if found != checked:
        raise ValueError("Extracted source file set differs from manifest")
    if sha256(args.rom_zip) != manifest["rom_zip_sha256"]:
        raise ValueError("Supplied ROM ZIP differs")
    print(json.dumps({"result": "PASS", "source_files": len(checked),
                      "rom_zip_sha256": manifest["rom_zip_sha256"]}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
