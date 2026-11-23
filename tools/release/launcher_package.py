"""Build the ROM-free Windows release ZIP consumed by RecompLauncher."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import re
import zipfile

ROOT = Path(__file__).resolve().parents[2]
PROFILE = ROOT / "recomplauncher.json"
VERSION_PATTERN = re.compile(r"^\d+\.\d+\.\d+(?:-[A-Za-z0-9.-]+)?$")


def digest(data):
    return hashlib.sha256(data).hexdigest()


def profile_for(version):
    if not VERSION_PATTERN.fullmatch(version):
        raise ValueError("Version must be semver, for example 1.2.0 or 1.2.0-beta.1")
    profile = json.loads(PROFILE.read_bytes())
    profile["prepare"]["version"] = version
    return profile


def validate_clean_installer(installer):
    data = installer.read_bytes()
    markers = (b"player.zip", "player.zip".encode("utf-16le"))
    if len(data) < 2 or data[:2] != b"MZ" or not any(marker in data for marker in markers):
        raise ValueError(
            "Installer is not the clean embedded-kit SmashTV.exe; "
            "never package build/rom-player/SmashTV.exe"
        )
    # The managed resource is a length-prefixed ZIP, followed by PE metadata.
    # The kit writer emits one disk, no archive comment and no ZIP64 trailer.
    end = data.rfind(b"PK\x05\x06")
    if end < 0 or end + 22 > len(data):
        raise ValueError("Installer lacks an embedded player kit")
    directory_size = int.from_bytes(data[end + 12:end + 16], "little")
    directory_offset = int.from_bytes(data[end + 16:end + 20], "little")
    start = end - directory_size - directory_offset
    if start < 4 or int.from_bytes(data[start-4:start], "little") != end + 22 - start:
        raise ValueError("Invalid embedded player kit extent")
    with zipfile.ZipFile(io.BytesIO(data[start:end+22])) as kit:
        required = ("runtime/eos-client.ini", "runtime/EOSSDK-Win64-Shipping.dll",
                    "host/eos.lib", "host/epic_rooms.o", "runtime/licenses/EOS.txt")
        manifest = json.loads(kit.read("manifest.json"))
        for path in required:
            if path not in manifest or digest(kit.read(path)) != manifest[path]:
                raise ValueError("Missing or invalid multiplayer payload: " + path)
    return data


def package(installer, out_dir, version):
    installer = installer.resolve()
    out_dir = out_dir.resolve()
    if out_dir.is_relative_to(ROOT):
        raise ValueError("RecompLauncher package output must be outside the checkout")
    installer_bytes = validate_clean_installer(installer)
    out_dir.mkdir(parents=True, exist_ok=True)
    name = f"SmashTV-{version}-Windows-x64.zip"
    destination = out_dir / name
    if destination.exists():
        raise FileExistsError(destination)
    profile = json.dumps(profile_for(version), indent=2, ensure_ascii=True) + "\n"
    notices = (ROOT / "THIRD_PARTY_NOTICES.md").read_bytes()
    members = {
        "SmashTV.exe": installer_bytes,
        "recomplauncher.json": profile.encode("utf-8"),
        "THIRD_PARTY_NOTICES.md": notices,
    }
    with zipfile.ZipFile(destination, "x", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for path, data in members.items():
            archive.writestr(path, data)
    inventory = [
        {"path": path, "bytes": len(data), "sha256": digest(data)}
        for path, data in members.items()
    ]
    audit = {
        "result": "PASS_RECOMPLAUNCHER_PACKAGE",
        "zip": str(destination),
        "zip_sha256": digest(destination.read_bytes()),
        "version": version,
        "files": inventory,
        "scope": "Clean installer and metadata only; no ROM, ROM-derived executable, cache, art or game data"
    }
    audit_path = out_dir / (name + ".audit.json")
    audit_path.write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
    return destination, audit_path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--installer", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--version", default="1.0.0")
    args = parser.parse_args()
    zip_path, audit_path = package(args.installer, args.out_dir, args.version)
    print(zip_path)
    print(audit_path)


if __name__ == "__main__":
    main()
