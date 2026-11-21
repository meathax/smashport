#!/usr/bin/env python3
"""Compare a native sound render with MAME -wavwrite output (PLAN P6 goldens).

Both files are 16-bit PCM WAV starting at emulated time zero (standard library
only). Reports over the common span:
  - lag (samples, -200..200) maximising correlation on the loudest 2-second probe
  - sample correlation and signal-to-difference ratio after DC removal
  - 20 ms RMS-envelope correlation (dB) and the median / 95th-percentile level
    difference over windows where the reference is above -60 dBFS
  - onset: first 20 ms window above -40 dBFS in each

  wavcmp.py <reference.wav> <native.wav> [--json out.json]
"""
import argparse, array, json, math, sys, wave


def load(path):
    with wave.open(path, "rb") as w:
        ch, width, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        raw = w.readframes(n)
    if width != 2:
        sys.exit(f"{path}: only 16-bit PCM is supported")
    a = array.array("h")
    a.frombytes(raw)
    if sys.byteorder == "big":
        a.byteswap()
    if ch > 1:
        a = array.array("h", (sum(a[i:i + ch]) // ch for i in range(0, len(a), ch)))
    return [v / 32768.0 for v in a], rate


def mean(x):
    return sum(x) / len(x)


def corr(x, y):
    mx, my = mean(x), mean(y)
    sxy = sxx = syy = 0.0
    for a, b in zip(x, y):
        da, db = a - mx, b - my
        sxy += da * db
        sxx += da * da
        syy += db * db
    return sxy / math.sqrt(sxx * syy) if sxx > 0 and syy > 0 else 0.0


def env_db(x, win):
    out = []
    for i in range(0, len(x) - win + 1, win):
        s = 0.0
        for v in x[i:i + win]:
            s += v * v
        out.append(10 * math.log10(s / win + 1e-20))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("reference")
    ap.add_argument("native")
    ap.add_argument("--json")
    a = ap.parse_args()
    ref, r1 = load(a.reference)
    nat, r2 = load(a.native)
    if r1 != r2:
        sys.exit(f"sample rates differ: {r1} vs {r2}")
    n = min(len(ref), len(nat))
    ref, nat = ref[:n], nat[:n]
    probe = r1 * 2
    ew = r1 // 50
    er_all = env_db(ref, ew)
    per = probe // ew
    best_block = max(range(len(er_all) // per), key=lambda b: sum(er_all[b * per:(b + 1) * per]))
    s0 = best_block * probe
    best, lag = -2.0, 0
    for L in range(-200, 201, 1):
        if s0 + L < 0 or s0 + probe + L > n:
            continue
        c = corr(ref[s0:s0 + probe:4], nat[s0 + L:s0 + probe + L:4])
        if c > best:
            best, lag = c, L
    if lag >= 0:
        r, t = ref[: n - lag], nat[lag:]
    else:
        r, t = ref[-lag:], nat[: n + lag]
    mr, mt = mean(r), mean(t)
    r = [v - mr for v in r]
    t = [v - mt for v in t]
    sig = sum(v * v for v in r)
    diff = sum((x - y) ** 2 for x, y in zip(r, t))
    er, et = env_db(r, ew), env_db(t, ew)
    d = sorted(b - a_ for a_, b in zip(er, et) if a_ > -60)
    ad = sorted(abs(v) for v in d)
    onset = lambda e: next((i for i, v in enumerate(e) if v > -40), None)
    out = {
        "reference": a.reference,
        "native": a.native,
        "rate": r1,
        "seconds_compared": round(len(r) / r1, 2),
        "lag_samples": lag,
        "probe_correlation": round(best, 5),
        "sample_correlation": round(corr(r, t), 5),
        "signal_to_difference_db": round(10 * math.log10(sig / max(diff, 1e-30)), 2),
        "envelope_correlation_db": round(corr(er, et), 5),
        "level_diff_db_median": round(d[len(d) // 2], 3) if d else None,
        "level_diff_db_p95_abs": round(ad[int(len(ad) * 0.95)], 3) if ad else None,
        "onset_window_20ms": {"reference": onset(er), "native": onset(et)},
    }
    print(json.dumps(out, indent=1))
    if a.json:
        json.dump(out, open(a.json, "w"), indent=1)


if __name__ == "__main__":
    main()
