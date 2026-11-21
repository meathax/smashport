#!/usr/bin/env python3
"""Lockstep stream reader, indexer and first-divergence finder (PLAN.md P0/P3).

Stream format is defined in tools/lockstep/record.lua. The native replayer
emits the same format, so MAME and native streams compare directly.

  lsfile.py index STREAM [--json OUT]        per-type counts, frame range, digest
  lsfile.py diff A B [--window N] [--symbols MAP.json]
                                             first divergent frame/record only
  lsfile.py state STREAM FRAME REGION OUT    reconstruct a region image at FRAME

Never print raw streams into an agent session; use `diff` output only.
"""
import argparse, hashlib, json, struct, sys
from collections import deque
from itertools import zip_longest

MAGIC = b"STVLS001"
NAMES = {1: "ram", 2: "palette", 3: "cmos", 4: "dma", 5: "sound", 6: "hcount", 7: "input", 8: "frame_end", 9: "summary"}
BASES = {1: 0x01000000, 2: 0x01800000, 3: 0x01400000}  # bit addresses
SIZES = {1: 0x20000, 2: 0x4000, 3: 0x2000}


def records(path):
    with open(path, "rb") as f:
        if f.read(8) != MAGIC:
            raise SystemExit(f"{path}: not an STVLS001 stream")
        while True:
            head = f.read(7)
            if not head:
                return
            if len(head) != 7:
                raise SystemExit(f"{path}: truncated record header")
            kind, frame, length = struct.unpack("<BIH", head)
            payload = f.read(length)
            if len(payload) != length:
                raise SystemExit(f"{path}: truncated payload")
            yield kind, frame, payload


def strip_pc(kind, payload):
    """Tap records carry the CPU PC as context only; native runs do not model it."""
    if kind in (4, 5):
        return payload[:8]
    if kind == 6:
        return payload[:2]
    if kind == 7:
        return payload[:6]
    return payload


def frames(path, keep_pc=False):
    """Yield (frame, [records]) grouped by frame-end markers."""
    cur = []
    for kind, frame, payload in records(path):
        if not keep_pc:
            payload = strip_pc(kind, payload)
        if kind == 8:
            yield frame, cur
            cur = []
        else:
            cur.append((kind, frame, payload))
    if cur:
        yield None, cur


def describe(kind, payload):
    if kind in (1, 2, 3):
        idx = struct.unpack_from("<H", payload)[0]
        return f"{NAMES[kind]} chunk {idx} (bit 0x{BASES[kind] + idx * 0x800:08x})"
    if kind in (4, 5):
        a, d, m = struct.unpack_from("<IHH", payload)
        return f"{NAMES[kind]} write 0x{a:08x}=0x{d:04x}/0x{m:04x}"
    if kind == 6:
        v, = struct.unpack_from("<H", payload)
        return f"hcount read 0x{v:04x}"
    if kind == 7:
        a, v = struct.unpack_from("<IH", payload)
        return f"input read 0x{a:08x}=0x{v:04x}"
    return NAMES.get(kind, str(kind))


def load_symbols(path):
    if not path:
        return []
    with open(path) as f:
        data = json.load(f)
    return sorted((int(v, 16) if isinstance(v, str) else v, k) for k, v in data.items())


def symbolize(symbols, bitaddr):
    best = None
    for addr, name in symbols:
        if addr > bitaddr:
            break
        best = (addr, name)
    return f"{best[1]}+0x{(bitaddr - best[0]) // 8:x}" if best else f"0x{bitaddr:08x}"


def chunk_diff(kind, a, b, symbols):
    idx = struct.unpack_from("<H", a)[0]
    out = []
    for off in range(0, 256, 2):
        wa, wb = a[2 + off:4 + off], b[2 + off:4 + off]
        if wa != wb:
            bit = BASES[kind] + (idx * 256 + off) * 8
            out.append(f"  {symbolize(symbols, bit)} (bit 0x{bit:08x}): expected 0x{int.from_bytes(wa, 'little'):04x} actual 0x{int.from_bytes(wb, 'little'):04x}")
            if len(out) >= 16:
                out.append("  ...")
                break
    return out


def cmd_index(args):
    counts, first, last, digest = {}, None, None, hashlib.sha256()
    with open(args.stream, "rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            digest.update(block)
    for kind, frame, _ in records(args.stream):
        counts[NAMES.get(kind, str(kind))] = counts.get(NAMES.get(kind, str(kind)), 0) + 1
        first = frame if first is None else first
        last = frame
    info = {"stream": args.stream, "sha256": digest.hexdigest(), "first_frame": first, "last_frame": last, "records": counts}
    text = json.dumps(info, indent=1)
    if args.json:
        with open(args.json, "w") as f:
            f.write(text + "\n")
    print(text)


def cmd_diff(args):
    symbols = load_symbols(args.symbols)
    history = deque(maxlen=max(0, args.window))
    compared = 0
    for a, b in zip_longest(frames(args.a), frames(args.b)):
        if a is None or b is None:
            if args.allow_prefix and compared:
                print(f"MATCHING_PREFIX over {compared} complete frames (partial comparison only)")
                return 0
            print(f"STREAM_LENGTH_MISMATCH after {compared} complete frames")
            return 1
        fa, ra = a
        fb, rb = b
        if fa is None or fb is None:
            print("INCOMPLETE_FRAME: missing frame-end marker")
            return 1
        if fa != fb or ra != rb:
            print(f"FIRST_DIVERGENCE frame A={fa} B={fb}")
            for i, (x, y) in enumerate(zip(ra, rb)):
                if x != y:
                    print(f" record {i}: expected {describe(x[0], x[2])}")
                    print(f" record {i}: actual   {describe(y[0], y[2])}")
                    if x[0] == y[0] and x[0] in (1, 2, 3) and len(x[2]) == len(y[2]) == 258:
                        print("\n".join(chunk_diff(x[0], x[2], y[2], symbols)))
                    break
            else:
                print(f" record count expected {len(ra)} actual {len(rb)}")
            print(f"PRECEDING_FRAMES {list(history)}")
            return 1
        history.append(fa)
        compared += 1
    if not compared:
        print("EMPTY_COMPARISON: no complete frames")
        return 1
    print(f"NO_DIVERGENCE over {compared} complete frames (both streams ended)")
    return 0


def cmd_state(args):
    """Reconstruct only full region dumps at a completed, sampled frame."""
    kind = {v: k for k, v in NAMES.items()}[args.region]
    if args.frame < 0:
        raise SystemExit("state: frame must be nonnegative")
    image = bytearray(SIZES[kind])
    initialized = set()
    selected = False
    sampled = False
    expected = SIZES[kind] // 256
    last_frame = -1
    for frame, frame_records in frames(args.stream, keep_pc=True):
        if frame is None:
            raise SystemExit("state: incomplete terminal frame")
        if frame <= last_frame:
            raise SystemExit("state: frames must increase strictly")
        last_frame = frame
        if frame > args.frame:
            break
        for k, record_frame, payload in frame_records:
            if record_frame != frame:
                raise SystemExit("state: records disagree with their frame end")
            if k != kind:
                continue
            if len(payload) == 6:
                raise SystemExit("state: compact chunk hashes cannot reconstruct memory; record full dumps")
            if len(payload) != 258:
                raise SystemExit("state: invalid full region chunk length")
            idx = struct.unpack_from("<H", payload)[0]
            if idx >= expected:
                raise SystemExit("state: region chunk index out of range")
            image[idx * 256:(idx + 1) * 256] = payload[2:]
            initialized.add(idx)
            if frame == args.frame:
                sampled = True
        if frame == args.frame:
            selected = True
            break
    if not selected:
        raise SystemExit("state: requested frame has no complete frame end")
    if len(initialized) != expected:
        raise SystemExit("state: initial region dump is incomplete")
    # A sampled frame can have no changed chunks. In the current stream format,
    # sparse cadence is not stored, so callers must explicitly acknowledge that
    # an unchanged/stale snapshot cannot be distinguished when no chunks occur.
    if not sampled:
        raise SystemExit("state: requested frame has no region chunks; sample identity cannot be established")
    with open(args.out, "wb") as f:
        f.write(image)
    print(f"{args.region} at frame {args.frame}: {len(image)} bytes -> {args.out}")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    s = p.add_subparsers(dest="cmd", required=True)
    i = s.add_parser("index"); i.add_argument("stream"); i.add_argument("--json")
    d = s.add_parser("diff"); d.add_argument("a"); d.add_argument("b")
    d.add_argument("--window", type=int, default=5); d.add_argument("--symbols")
    d.add_argument("--allow-prefix", action="store_true", help="diagnostic only: accept a matching shorter stream")
    t = s.add_parser("state"); t.add_argument("stream"); t.add_argument("frame", type=int)
    t.add_argument("region", choices=["ram", "palette", "cmos"]); t.add_argument("out")
    args = p.parse_args()
    return {"index": cmd_index, "diff": cmd_diff, "state": cmd_state}[args.cmd](args) or 0


if __name__ == "__main__":
    sys.exit(main())
