#!/usr/bin/env python3
"""Per-command sound goldens (PLAN P6): compare the MAME and native WAVs from
snd_commands.lua / stv_l8_lockstep --snd-commands, one window per command.

  snd_goldens.py <mame.wav> <native.wav> [--start 600 --period 150 --from 0 --to 255] [--json out]

Windows are DC-removed (the DAC idles at a constant level). A window is silent
when both sides stay below -60 dBFS RMS. Thresholds for an audible command: RMS
level within 1 dB; 20 ms envelope correlation >= 0.95, or, when both envelopes
are flat (std < 1 dB, where correlation is meaningless), a mean absolute
envelope difference <= 1 dB; onset within 2 envelope windows (40 ms).
"""
import argparse, array, json, math, sys, wave
sys.path.insert(0, __import__("os").path.dirname(__file__))
from wavcmp import env_db, corr

FRAME_S = 506 * 289 / 8e6          # Y-unit video frame
RATE = 48000                     # snd_commands.lua / native mixer output


def load_complete(path):
    """Keep wavcmp's PCM conversion, but reject incomplete recording payloads."""
    with wave.open(path, "rb") as w:
        ch, width, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        if width != 2:
            raise ValueError(f"{path}: only 16-bit PCM is supported")
        if rate != RATE:
            raise ValueError(f"{path}: expected {RATE} Hz, got {rate}")
        # One extra frame also catches a partial frame in the declared data chunk.
        raw = w.readframes(n + 1)
    expected = n * ch * width
    if len(raw) != expected:
        raise ValueError(f"{path}: truncated PCM payload; expected {expected} bytes, got {len(raw)}")
    pcm = array.array("h")
    pcm.frombytes(raw)
    if sys.byteorder == "big":
        pcm.byteswap()
    if ch > 1:
        pcm = array.array("h", (sum(pcm[i:i + ch]) // ch for i in range(0, len(pcm), ch)))
    return [v / 32768.0 for v in pcm], rate


def rms_db(x):
    return 10 * math.log10(sum(v * v for v in x) / max(len(x), 1) + 1e-20)


ap = argparse.ArgumentParser()
ap.add_argument("mame")
ap.add_argument("native")
ap.add_argument("--start", type=int, default=600)
ap.add_argument("--period", type=int, default=150)
ap.add_argument("--from", dest="first", type=int, default=0)
ap.add_argument("--to", type=int, default=255)
ap.add_argument("--json")
a = ap.parse_args()
if a.start < 0 or a.period <= 0:
    ap.error("start must be nonnegative and period must be positive")
if not 0 <= a.first <= a.to <= 255:
    ap.error("command range must satisfy 0 <= --from <= --to <= 255")
try:
    ref, rate = load_complete(a.mame)
    nat, _ = load_complete(a.native)
except (OSError, EOFError, ValueError, wave.Error) as error:
    ap.error(str(error))
ew = rate // 50
rows, fails = [], []
for k, cmd in enumerate(range(a.first, a.to + 1)):
    s0 = int((a.start + k * a.period) * FRAME_S * rate)
    s1 = int((a.start + (k + 1) * a.period) * FRAME_S * rate)
    r, t = ref[s0:s1], nat[s0:s1]
    if len(r) != s1 - s0 or len(t) != s1 - s0 or len(r) < ew * 4:
        rows.append({"cmd": cmd, "status": "missing"}); fails.append(cmd); continue
    if r:
        mr, mt = sum(r) / len(r), sum(t) / max(len(t), 1)
        r, t = [v - mr for v in r], [v - mt for v in t]
    lr, lt = rms_db(r), rms_db(t)
    if lr < -60 and lt < -60:
        rows.append({"cmd": cmd, "status": "silent"}); continue
    er, et = env_db(r, ew), env_db(t, ew)
    std = lambda e: (sum((v - sum(e) / len(e)) ** 2 for v in e) / len(e)) ** 0.5
    flat = std(er) < 1.0 and std(et) < 1.0
    ec = corr(er, et)
    mad = sum(abs(x - y) for x, y in zip(er, et)) / len(er)
    on = lambda e: next((i for i, v in enumerate(e) if v > max(e) - 30), None)
    od = abs((on(er) or 0) - (on(et) or 0))
    ok = abs(lr - lt) <= 1.0 and (mad <= 1.0 if flat else ec >= 0.95) and od <= 2
    rows.append({"cmd": cmd, "status": "pass" if ok else "FAIL", "level_db": round(lr, 2),
                 "level_diff_db": round(lt - lr, 3), "envelope_corr": round(ec, 4), "flat": flat,
                 "envelope_mad_db": round(mad, 3), "onset_diff_windows": od})
    if not ok:
        fails.append(cmd)
summary = {"commands": len(rows), "audible": sum(1 for r in rows if r["status"] in ("pass", "FAIL")),
           "silent": sum(1 for r in rows if r["status"] == "silent"), "failed": fails}
print(json.dumps(summary))
for r in rows:
    if r["status"] in ("FAIL", "missing"):
        print(r)
if a.json:
    json.dump({"summary": summary, "rows": rows}, open(a.json, "w"), indent=1)
sys.exit(1 if fails else 0)
