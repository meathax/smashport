"""Compare native YUNIT switch routes/capacity with untouched assembly tables."""
import argparse
import hashlib
import json
import re
from pathlib import Path


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_routes(path: Path) -> list[tuple[str, str]]:
    text = path.read_text(encoding="latin1")
    section = text.split("SWTAB:", 1)[1].split(".ELSE", 1)[0]
    words = []
    for raw in section.splitlines():
        code = raw.split(";", 1)[0].strip()
        word = re.fullmatch(r"\.WORD\s+(\S+)", code, re.IGNORECASE)
        long = re.fullmatch(r"\.LONG\s+(\S+)", code, re.IGNORECASE)
        if word:
            words.append([word.group(1), None])
        elif long:
            if not words or words[-1][1] is not None:
                raise ValueError("YUNIT SWTAB long entry has no preceding word")
            words[-1][1] = long.group(1)
    if len(words) != 32 or any(routine is None for _, routine in words):
        raise ValueError("YUNIT SWTAB is not 32 complete process routes")
    return [(pid, routine) for pid, routine in words]


def native_routes(path: Path) -> list[tuple[str, str]]:
    text = path.read_text(encoding="utf-8")
    block = re.search(r"inline constexpr SwitchRoute yunit_switch_routes\[32\]=\{(.*?)\n\};", text, re.DOTALL)
    if not block:
        raise ValueError("Native YUNIT route table not found")
    routes = re.findall(r'\{"([^"]+)","([^"]+)"\}', block.group(1))
    if len(routes) != 32:
        raise ValueError("Native YUNIT route table does not have 32 entries")
    return routes


def verify_unstack_sleep(path: Path) -> None:
    text = path.read_text(encoding="latin1")
    section = text.split("UNSTACK:", 1)[1].split("SERVICE\n", 1)[0]
    code = "\n".join(line.split(";", 1)[0].strip() for line in section.splitlines())
    pattern = r"CALLA\s+GETPRC\s*\nUNSTKX:\s*\nSLEEP\s+1\s*\nJRUC\s+UNSTKL"
    if not re.search(pattern, code, re.IGNORECASE):
        raise ValueError("Source UNSTACK no longer sleeps after one process dispatch")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="Untouched smashtv-main directory")
    parser.add_argument("native", type=Path, help="switch_scan.hpp")
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    source = source_routes(args.source / "AUDIT.ASM")
    native = native_routes(args.native)
    verify_unstack_sleep(args.source / "AUDIT.ASM")
    for index, ((spid, sroutine), (npid, nroutine)) in enumerate(zip(source, native)):
        if (spid.upper(), sroutine) != (npid.upper(), nroutine):
            raise ValueError(f"Switch route differs from source at index {index}")
    ram = (args.source / "RAM.ASM").read_text(encoding="latin1")
    if not re.search(r"\.BSS\s+SWSTMN,256\b", ram) or not re.search(r"\.BSS\s+SWSTST,0\b", ram):
        raise ValueError("Source switch stack RAM extent differs")
    report = {
        "result": "PASS", "selected_source_branch": "AUDIT.ASM:SWTAB .IF YUNIT",
        "routes_compared": len(source), "switch_stack_bits": 256,
        "switch_stack_word_entries": 16,
        "unstack_processes_before_sleep": 1,
        "source_sha256": {name: digest(args.source / name) for name in ("AUDIT.ASM", "RAM.ASM", "MAIN.ASM")},
        "native_sha256": digest(args.native),
        "scope": "Source route labels, RAM extent and UNSTACK one-process-then-SLEEP structure vs native table; native order/deferral exercised by native_core and native_game_session. Not executed L8 scheduler timing or full game input dispatch.",
    }
    args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
