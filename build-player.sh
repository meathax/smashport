#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
    echo "Usage: bash build-player.sh <retail L8 ZIP> [build-directory]" >&2
    exit 64
fi

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
rom=$(cd -- "$(dirname -- "$1")" && pwd)/$(basename -- "$1")
build=${2:-"$root/build/portable-player"}
mkdir -p -- "$build"
build=$(cd -- "$build" && pwd)
prepare=$(mktemp -d "$build/setup.XXXXXXXX")
trap 'rm -f -- "$prepare/cache.bin" "$prepare/inputs.txt"; rmdir -- "$prepare"' EXIT

"${PYTHON:-python3}" "$root/tools/player/prepare.py" --rom "$rom" \
    --cache "$prepare/cache.bin" --generated "$build/generated" --result "$prepare/inputs.txt"
args=(-DSTV_PLAYER_INPUTS="$(cat "$prepare/inputs.txt")" -DCMAKE_BUILD_TYPE=Release)
if [[ -n ${STV_SDL_SOURCE:-} ]]; then args+=(-DSTV_SDL_SOURCE="$STV_SDL_SOURCE"); fi
cmake -S "$root" -B "$build" "${args[@]}"
cmake --build "$build" --config Release --target SmashTV --parallel "${STV_JOBS:-4}"

# Local Developer tools profile for this private build. The Windows release
# profile still targets the ROM-free Windows setup wrapper.
"${PYTHON:-python3}" - "$root/recomplauncher.json" "$build/recomplauncher.json" <<'PY'
import json
from pathlib import Path
import sys

source, output = map(Path, sys.argv[1:])
original = json.loads(source.read_bytes())
profile = {
    "$schema": original["$schema"],
    "format": original["format"],
    "name": original["name"],
    "repo": original["repo"],
    "min": original["min"],
    "inputs": original["inputs"],
    "start": {
        "program": ["SmashTV.app/Contents/MacOS/SmashTV"] if sys.platform == "darwin" else ["SmashTV"],
        "args": ["--rom={input:rom}", "--data={data}"],
    },
    "settings": [setting for setting in original["settings"] if setting["id"] == "profile"],
    "saves": ["{data}/cmos-l8.bin", "{data}/cmos-l8-16x9.bin"],
}
output.write_text(json.dumps(profile, indent=2) + "\n", encoding="utf-8")
PY
