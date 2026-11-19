#!/usr/bin/env python3
"""Reproduce the P1 revision-anchoring result end to end (development only).

Steps: synthesize the missing includes, assemble+link the 48 objects, locate every
linked global in LA5/LA6/L8, measure anchored identity, and write
  generated/gsp2cpp/symbols_<rev>.json   source label -> retail bit address (unique hits)
  evidence/p1-revision-anchoring-<date>.json
All inputs are hashed. Nothing here is used by the shipping game.

  python tools/gsp2cpp/p1_report.py [--date 2026-09-24]
"""
import argparse, hashlib, json, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
OUT = os.path.join(ROOT, "build", "p1")
GEN = os.path.join(ROOT, "generated", "gsp2cpp")


def run(*args):
    subprocess.run([sys.executable, *args], check=True, cwd=ROOT, stdout=subprocess.DEVNULL)


def sha(path):
    return hashlib.sha256(open(path, "rb").read()).hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--date", default="2026-09-24")
    a = ap.parse_args()
    os.makedirs(OUT, exist_ok=True)
    run("tools/gsp2cpp/synth_includes.py")
    run("tools/gsp2cpp/loadimg.py", "--write")
    image = os.path.join(OUT, "robo.bin")
    run("tools/gsp2cpp/asm.py", "link", "--mem", "ROM2=FFFF7000", "--out", image,
        "--map", os.path.join(OUT, "robo.map.json"), "--report", os.path.join(OUT, "report.json"))
    link = json.load(open(os.path.join(OUT, "report.json")))
    revs = {}
    for rev in ("la5", "la6", "l8"):
        loc = os.path.join(OUT, f"locate_{rev}.json")
        anc = os.path.join(OUT, f"anchored_{rev}.json")
        run("tools/gsp2cpp/locate.py", image, "--rev", rev, "--json", loc)
        run("tools/gsp2cpp/anchored.py", image, "--rev", rev, "--locate", loc, "--json", anc)
        L = json.load(open(loc))
        A = json.load(open(anc))
        symbols = {x["symbol"]: x["ref"] for x in L if x["how"] == "unique"}
        with open(os.path.join(GEN, f"symbols_{rev}.json"), "w") as f:
            json.dump({"source": "tools/gsp2cpp/p1_report.py", "rev": rev,
                       "note": "source label -> retail bit address; unique relocation-masked matches only",
                       "symbols": dict(sorted(symbols.items(), key=lambda kv: kv[1]))}, f, indent=0)
        revs[rev] = {"globals": len(L), "located": sum(1 for x in L if x["ref"]),
                     "unique": len(symbols), "span_words": A["words"], "span_words_equal": A["matched"],
                     "identity_percent": round(100.0 * A["matched"] / A["words"], 2),
                     "differing_spans": len(A["differing_spans"]),
                     "modules_below_90_percent": {k: v for k, v in A["modules"].items()
                                                  if v["words"] and v["matched"] < 0.9 * v["words"]}}
    used = link["includes_used"]
    report = {
        "date": a.date, "phase": "P1", "tool": "tools/gsp2cpp (asm.py, loadimg.py, locate.py, anchored.py)",
        "inputs": {"objects": link["objects"], "files_used": used,
                   "include_variant": "SYS.INC -> SYS.RBO (only copy defining PALCNT/NUMPAL=32)",
                   "memory_map": "BACKUP/ROBO.CMD (ROM 0xFFE00000) with ROM2 origin 0xFFFF7000 (LA5 places DOMNTR there; LA6/L8 use 0xFFFF6C00)",
                   "synthesized": {n: sha(os.path.join(GEN, "synth", n)) for n in sorted(os.listdir(os.path.join(GEN, "synth")))},
                   "isa_table_sha256": sha(os.path.join(HERE, "isa_table.json"))},
        "assembly": {"module_errors": link["module_errors"], "unresolved_externals": link["unresolved_externals"],
                     "link_problems": link["link_problems"]},
        "revisions": revs,
        "conclusion": ("The archive is an LA5-era snapshot: every retail revision matches less well than LA5. "
                       "Where source equals LA5 the assembler output is byte-identical (all instruction classes). "
                       "Remaining spans are source-versus-retail edits; the L8 program therefore cannot be produced "
                       "from this source, and translation input becomes the L8 ROM with source-aligned labels (PLAN D1)."),
    }
    path = os.path.join(ROOT, "evidence", f"p1-revision-anchoring-{a.date}.json")
    json.dump(report, open(path, "w"), indent=1)
    for rev, r in revs.items():
        print(f"{rev}: {r['unique']} unique symbols, identity {r['identity_percent']}%")
    print(path)


if __name__ == "__main__":
    main()
