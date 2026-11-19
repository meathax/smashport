#!/usr/bin/env python3
"""Pick TI's encoding when several opcodes decode to the same MAME text (P1).

Some TMS34010 encodings have don't-care bits (CALLR 0x0D20-0x0D3F, NOP ...), and
"SP" names both A15 and B15. The TI assembler emitted one specific form. This
tool counts first-word opcodes in linear unidasm disassemblies of the retail
program ROMs (LA5, LA6, L8) and records, for every ambiguous template, the
opcode frequencies. Data misdecoded as code adds noise, so the result is only a
ranking; the LA5 byte comparison is the proof. Output: isa_canon.json.
"""
import collections, json, os, re, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import compare

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    table = json.load(open(os.path.join(HERE, "isa_table.json")))["opcodes"]
    groups = collections.defaultdict(list)
    for op, (n, text) in table.items():
        groups[text].append(int(op, 16))
    amb = {t: ops for t, ops in groups.items() if len(ops) > 1}
    counts = collections.Counter()
    with tempfile.TemporaryDirectory() as tmp:
        for rev in ("la5", "la6", "l8"):
            p = os.path.join(tmp, rev + ".bin")
            open(p, "wb").write(compare.retail(rev))
            out = subprocess.run([r"D:\Arcade\AI\mameexe\unidasm.exe", p, "-arch", "tms34010"],
                                 capture_output=True, text=True, check=True).stdout
            for line in out.splitlines():
                m = re.match(r"[0-9a-f]+: ([0-9a-f]{4})", line)
                if m:
                    counts[int(m.group(1), 16)] += 1
    canon = {}
    for text, ops in amb.items():
        ranked = sorted(ops, key=lambda o: (-counts[o], o))
        canon[text] = [[f"{o:04x}", counts[o]] for o in ranked if counts[o]] or [[f"{ranked[0]:04x}", 0]]
    json.dump(canon, open(os.path.join(HERE, "isa_canon.json"), "w"), indent=0, sort_keys=True)
    print(len(canon), "ambiguous templates ranked")


if __name__ == "__main__":
    main()
