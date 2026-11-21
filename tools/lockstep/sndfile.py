#!/usr/bin/env python3
"""Read STVSND01 streams; diff fails on divergence, truncation or unequal length.

  sndfile.py summary FILE
  sndfile.py dump FILE [FROM] [N]
  sndfile.py diff REF TEST [--allow-prefix]

Frame hashes are cumulative. Counts can differ at a vblank boundary, so only
equal-count summaries are conclusive; the last compared summary must agree.
Full access windows also compare time, address and data (PC is context only).
Main sound writes are compared separately. Type 4 video/CMOS control taps are
MAME-only diagnostic context; the native recorder does not emit those taps.
--allow-prefix is for diagnostics and cannot prove an end-to-end gate.
"""
import argparse
import struct
import sys
from itertools import zip_longest

SIZES = {1: 21, 2: 21, 3: 18, 4: 18, 9: 12}


def records(path):
    with open(path, "rb") as stream:
        if stream.read(8) != b"STVSND01":
            raise ValueError(f"{path}: bad STVSND01 magic")
        while head := stream.read(1):
            kind = head[0]
            if kind not in SIZES:
                raise ValueError(f"{path}: unknown record kind {kind}")
            payload = stream.read(SIZES[kind])
            if len(payload) != SIZES[kind]:
                raise ValueError(f"{path}: truncated kind {kind} payload")
            if kind in (1, 2):
                frame, sec, att, addr, data, pc = struct.unpack("<IIQHBH", payload)
                yield kind, frame, dict(t=sec * 10**18 + att, addr=addr, data=data, pc=pc)
            elif kind in (3, 4):
                frame, sec, att, data = struct.unpack("<IIQH", payload)
                yield kind, frame, dict(t=sec * 10**18 + att, data=data)
            else:
                frame, count, digest = struct.unpack("<III", payload)
                yield kind, frame, dict(count=count, hash=digest)


def fmt(kind, frame, record):
    name = {1: "R", 2: "W", 3: "SND", 4: "CTL", 9: "FRAME"}[kind]
    if kind in (1, 2):
        return f"{name} f{frame} t={record['t'] / 5e11:.4f}cyc {record['addr']:04x}={record['data']:02x} pc={record['pc']:04x}"
    if kind in (3, 4):
        return f"{name} f{frame} t={record['t'] / 5e11:.4f}cyc data={record['data']:04x}"
    return f"{name} f{frame} count={record['count']} hash={record['hash']:08x}"


def diff(args):
    summaries = lambda path: (item for item in records(path) if item[0] == 9)
    compared = conclusive = 0
    last_a = last_b = None
    partial = False
    for a, b in zip_longest(summaries(args.ref), summaries(args.test)):
        if a is None or b is None:
            if args.allow_prefix and compared:
                partial = True
                break
            print(f"STREAM_LENGTH_MISMATCH after {compared} frame summaries")
            return 1
        if a[1] != b[1]:
            print(f"FRAME_MISMATCH expected {a[1]} actual {b[1]}")
            return 1
        last_a, last_b = a, b
        compared += 1
        if a[2]["count"] == b[2]["count"]:
            conclusive += 1
            if a[2]["hash"] != b[2]["hash"]:
                print(f"FIRST_HASH_MISMATCH frame {a[1]}: expected {a[2]} actual {b[2]}")
                return 1
    if not compared:
        print("EMPTY_COMPARISON: no frame summaries")
        return 1
    if last_a[2] != last_b[2]:
        print(f"INCONCLUSIVE_END frame {last_a[1]}: expected {last_a[2]} actual {last_b[2]}")
        return 1

    # Compare each ordered tap sequence separately: the two CPUs can interleave
    # their callbacks differently while their local timestamps remain identical.
    for kinds in ((1, 2), (3,)):
        accesses = lambda path: (item for item in records(path) if item[0] in kinds)
        count = 0
        for a, b in zip_longest(accesses(args.ref), accesses(args.test)):
            if a is None or b is None:
                if partial and args.allow_prefix:
                    break
                print(f"ACCESS_LENGTH_MISMATCH kinds {kinds} after {count} records")
                return 1
            ra = {k: v for k, v in a[2].items() if k != "pc"}
            rb = {k: v for k, v in b[2].items() if k != "pc"}
            if a[0] != b[0] or ra != rb:
                print(f"FIRST_ACCESS_MISMATCH record {count}: {fmt(*a)} / {fmt(*b)}")
                return 1
            count += 1
    label = "MATCHING_PREFIX (partial comparison only)" if partial else "NO_DIVERGENCE (both streams ended)"
    print(f"{label}: {compared} frame summaries, {conclusive} conclusive, last frame {last_a[1]}")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    summary = commands.add_parser("summary")
    summary.add_argument("file")
    dump = commands.add_parser("dump")
    dump.add_argument("file")
    dump.add_argument("start", type=int, nargs="?", default=0)
    dump.add_argument("n", type=int, nargs="?", default=40)
    compare = commands.add_parser("diff")
    compare.add_argument("ref")
    compare.add_argument("test")
    compare.add_argument("--allow-prefix", action="store_true")
    args = parser.parse_args()
    try:
        if args.command == "diff":
            return diff(args)
        if args.command == "summary":
            kinds = {}
            first = last = None
            for kind, frame, record in records(args.file):
                kinds[kind] = kinds.get(kind, 0) + 1
                if kind == 9:
                    first = frame if first is None else first
                    last = (frame, record["count"], record["hash"])
            print("kinds", kinds, "frames", first, "..", last)
        else:
            for i, item in enumerate(records(args.file)):
                if i >= args.start + args.n:
                    break
                if i >= args.start:
                    print(i, fmt(*item))
    except (OSError, ValueError, struct.error) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
