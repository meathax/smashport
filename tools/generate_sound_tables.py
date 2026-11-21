#!/usr/bin/env python3
"""Extract literal L8 SOUNDS.ASM command tables; never assemble or execute code."""
from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "original/archive/smashtv-main/SOUNDS.ASM"
CPP = ROOT / "generated/source_sound_tables.cpp"
REPORT = ROOT / "generated/source_sound_tables.json"


def asm_word(expression: str) -> int:
    """Decode the literal operators used by this table block in source order.

    The packed priority expressions use OR before a final hexadecimal
    subtraction (for example ``>F0FF|>800-10H``).  The emitted report retains
    the original expression so this narrow translation remains auditable.
    """
    tokens: list[str] = []
    pos = 0
    token_re = re.compile(r"\s*(>\s*[0-9A-F]+|[0-9][0-9A-F]*H|[0-9]+|[+\-|&])", re.I)
    while pos < len(expression):
        match = token_re.match(expression, pos)
        if not match:
            raise ValueError(f"Unsupported SOUNDS.ASM expression: {expression!r}")
        tokens.append(match.group(1).replace(" ", "").upper())
        pos = match.end()
    if not tokens or len(tokens) % 2 == 0:
        raise ValueError(f"Malformed SOUNDS.ASM expression: {expression!r}")

    def number(token: str) -> int:
        if token.startswith(">"):
            return int(token[1:], 16)
        if token.endswith("H"):
            return int(token[:-1], 16)
        return int(token, 10)

    result = number(tokens[0])
    for operator, operand in zip(tokens[1::2], tokens[2::2]):
        right = number(operand)
        if operator == "+":
            result += right
        elif operator == "-":
            result -= right
        elif operator == "|":
            result |= right
        elif operator == "&":
            result &= right
        else:
            raise ValueError(f"Unsupported SOUNDS.ASM operator: {operator}")
    if not 0 <= result <= 0xFFFF:
        raise ValueError(f"SOUNDS.ASM word outside u16: {expression!r} -> {result}")
    return result


def extract() -> tuple[list[dict], str]:
    raw = SOURCE.read_bytes()
    text = raw.decode("latin1")
    lines = text.splitlines()
    first = next((i for i, line in enumerate(lines) if re.match(r"^\s*CVSDOFF\s+\.WORD\b", line, re.I)), None)
    if first is None:
        raise ValueError("CVSDOFF table block is missing")
    records: list[dict] = []
    names: set[str] = set()
    for index in range(first, len(lines)):
        line = lines[index].split(";", 1)[0].strip()
        if re.match(r"^\.END\b", line, re.I):
            break
        match = re.match(r"^([A-Za-z_][A-Za-z0-9_]*)\s+\.WORD\s+(.+?)\s*$", line, re.I)
        if not match:
            continue
        label, expression_text = match.groups()
        label = label.upper()
        if label in names:
            raise ValueError(f"Duplicate sound table label {label}")
        names.add(label)
        expressions = [part.strip() for part in expression_text.split(",")]
        if any(not part for part in expressions):
            raise ValueError(f"Empty word in {label}")
        words = [asm_word(part) for part in expressions]
        if len(words) < 2 or len(words) > 256 or words[0] < 0xF000:
            raise ValueError(f"Unsupported command table shape for {label}")
        if words[-1] != 0 or words.count(0) != 1:
            raise ValueError(f"Command table {label} must end in one zero word")
        records.append({"label": label, "line": index + 1, "expressions": expressions, "words": words})

    if len(records) != 110:
        raise ValueError(f"Expected 110 literal sound tables, found {len(records)}")
    for label, channel in {
        "CVSDOFF": 0, "CVSDFOFF": 0, "SOUNDOFF": 0, "ALLOFF": 0,
        "MUSICOFF": 3, "MUSITOFF": 3, "YAMOFF": 3, "DIGOFF": 0,
    }.items():
        record = next((entry for entry in records if entry["label"] == label), None)
        if record is None or ((record["words"][0] >> 8) & 3) != channel:
            raise ValueError(f"Packed priority channel mismatch for {label}")
    return records, hashlib.sha256(raw).hexdigest()


def outputs(records: list[dict], source_sha256: str) -> tuple[bytes, bytes]:
    source_lines = [
        '#include "stv/sound_commands.hpp"',
        "namespace stv {",
        "namespace {",
    ]
    for index, record in enumerate(records):
        values = ", ".join(f"0x{word:04x}" for word in record["words"])
        source_lines.append(f"constexpr u16 words_{index:03d}[] = {{{values}}};")
    source_lines.extend(["}", "const SourceSoundTable source_sound_tables[] = {"])
    for index, record in enumerate(records):
        source_lines.append(
            f'    {{"{record["label"]}", words_{index:03d}, '
            f'{len(record["words"])}, {record["line"]}}},'
        )
    source_lines.extend([
        "};",
        "const usize source_sound_table_count = sizeof(source_sound_tables) / sizeof(source_sound_tables[0]);",
        "",
        "// Kept for the historical native command-trace fixtures; production calls use labels.",
    ])
    compat = (("SHOT", "sound_shot"), ("RIFL", "sound_rifle"), ("ICSND", "sound_pickup"))
    by_label = {record["label"]: record for record in records}
    for label, symbol in compat:
        words = by_label[label]["words"]
        if len(words) != 4:
            raise ValueError(f"Legacy fixture table {label} changed shape")
        values = ", ".join(f"0x{word:04x}" for word in words)
        source_lines.append(f"const u16 {symbol}[4] = {{{values}}};")
    source_lines.extend(["}", ""])
    cpp = "\n".join(source_lines).encode("utf-8")
    report = {
        "source_file": "original/archive/smashtv-main/SOUNDS.ASM",
        "source_sha256": source_sha256,
        "expression_evaluation": "source-order fold for the literal +, -, |, & expressions in this extracted table block",
        "table_count": len(records),
        "tables": records,
    }
    encoded = (json.dumps(report, indent=2, ensure_ascii=False) + "\n").encode("utf-8")
    return cpp, encoded


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true", help="verify generated outputs without writing")
    args = parser.parse_args()
    records, source_sha256 = extract()
    cpp, report = outputs(records, source_sha256)
    expected = ((CPP, cpp), (REPORT, report))
    if args.check:
        stale = [str(path.relative_to(ROOT)) for path, content in expected if not path.exists() or path.read_bytes() != content]
        if stale:
            print("Stale generated sound data: " + ", ".join(stale), file=sys.stderr)
            return 1
    else:
        for path, content in expected:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)
    print(f"source sound tables: {len(records)}; source SHA-256 {source_sha256}; check={'ok' if args.check else 'written'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
