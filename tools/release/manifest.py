#!/usr/bin/env python3
"""Hash existing tracked and nonignored new files into the private workspace manifest.

FILE_MANIFEST.json excludes itself and ignored build/private outputs. It is a
workspace inventory, not a redistributable-file allowlist or release proof.
  python tools/release/manifest.py [--check] [--version 1.0]
"""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--version", default="1.0")
    args = parser.parse_args()
    raw = subprocess.run(["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"],
                         cwd=ROOT, capture_output=True, check=True).stdout
    paths = sorted({name.decode("utf-8") for name in raw.split(b"\0") if name})
    entries = []
    for name in paths:
        path = ROOT / name
        if name == "FILE_MANIFEST.json" or not path.is_file():
            continue
        with path.open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        entries.append({"path": name, "bytes": path.stat().st_size, "sha256": digest})
    manifest = {"version": args.version, "self_excluded": True,
                "scope": "private workspace: existing tracked and nonignored new files", "files": entries}
    text = json.dumps(manifest, indent=2) + "\n"
    path = ROOT / "FILE_MANIFEST.json"
    if args.check:
        same = path.is_file() and path.read_text(encoding="utf-8") == text
        print("FILE_MANIFEST.json", "up to date" if same else "differs")
        return 0 if same else 1
    path.write_text(text, encoding="utf-8", newline="\n")
    print(f"FILE_MANIFEST.json: {len(entries)} files")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
