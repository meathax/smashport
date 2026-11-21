#!/usr/bin/env python3
"""Regress RecompLauncher metadata and ROM-free package staging."""
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
PROFILE = ROOT / "recomplauncher.json"
SCHEMA = ROOT / "tools/release/recomplauncher.schema.json"
SPEC = importlib.util.spec_from_file_location(
    "launcher_package", ROOT / "tools/release/launcher_package.py"
)
launcher_package = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(launcher_package)
SETUP_SPEC = importlib.util.spec_from_file_location(
    "player_setup", ROOT / "tools/release/installer/player_setup.py"
)
player_setup = importlib.util.module_from_spec(SETUP_SPEC)
sys.path.insert(0, str(ROOT / "tools/player"))
SETUP_SPEC.loader.exec_module(player_setup)


def main():
    import jsonschema

    profile = json.loads(PROFILE.read_bytes())
    schema = json.loads(SCHEMA.read_bytes())
    jsonschema.Draft7Validator.check_schema(schema)
    errors = sorted(jsonschema.Draft7Validator(schema).iter_errors(profile), key=lambda e: list(e.path))
    if errors:
        raise AssertionError("\n".join(f"{list(error.path)}: {error.message}" for error in errors))
    assert profile["inputs"] == [{
        "id": "rom",
        "label": "Your legally obtained retail Smash T.V. revision L8 ROM ZIP (merged or split set)",
        "role": "game",
        "formats": [{"type": "file", "extensions": ["zip"]}]
    }]
    assert profile["prepare"]["done"] == ["{data}/native-player/player.json"]
    assert "--rom={input:rom}" in profile["prepare"]["args"]
    assert profile["start"]["program"] == profile["prepare"]["program"] == ["SmashTV.exe"]
    assert "--play" in profile["start"]["args"]
    assert all(not path.startswith("{home}") and not path.startswith("{local}") and not path.startswith("{config}")
               for path in profile["saves"])
    assert profile["folders"]["logs"][0]["path"].startswith("{data}/")
    assert profile["folders"]["cache"][0]["path"].startswith("{data}/")
    assert not profile.get("game", {}).get("dumps")
    marker = player_setup.new_uuid()
    assert player_setup.is_uuid(marker) and not player_setup.is_uuid("not-a-uuid")
    assert player_setup.parallel_map(lambda value: value * 2, [1, 2, 3, 4], 2) == [2, 4, 6, 8]

    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        installer = root / "SmashTV.exe"
        installer.write_bytes(b"MZclean-launcher-kit\0player.zip\0not-a-rom-player")
        output = root / "outside"
        package, audit_path = launcher_package.package(installer, output, "1.2.3-beta.1")
        assert package.name == "SmashTV-1.2.3-beta.1-Windows-x64.zip"
        with zipfile.ZipFile(package) as archive:
            assert sorted(archive.namelist()) == [
                "SmashTV.exe",
                "THIRD_PARTY_NOTICES.md",
                "recomplauncher.json",
            ]
            packaged = json.loads(archive.read("recomplauncher.json"))
            assert packaged["prepare"]["version"] == "1.2.3-beta.1"
            assert all("selected-l8.zip" not in name for name in archive.namelist())
        audit = json.loads(audit_path.read_bytes())
        assert audit["result"] == "PASS_RECOMPLAUNCHER_PACKAGE"
        try:
            bad = root / "native.exe"
            bad.write_bytes(b"MZROM-derived-native-player")
            launcher_package.package(bad, output / "bad", "1.2.3")
        except ValueError:
            pass
        else:
            raise AssertionError("A native ROM-derived player was accepted as a clean installer")
    print("RecompLauncher profile and package checks passed")


if __name__ == "__main__":
    main()
