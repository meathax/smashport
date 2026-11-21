"""Exercise actual metadata analysis with instructions from the typed decoder."""
from pathlib import Path
import argparse, importlib.util, sys, unittest

ROOT = next(p for p in Path(__file__).resolve().parents
            if (p / "tools/gsp2cpp/codemap.py").is_file())
sys.path.insert(0, str(ROOT / 'tools/gsp2cpp'))
from decode import Decoder
from compare import BASE

GENERATOR = ROOT / "tools/gsp2cpp/codemap.py"
candidate = None
decoder = Decoder()

def instruction(pc, name, reg=None, extensions=()):
    for op, (n, m, operands, text) in sorted(decoder.table.items()):
        regs = [r for kind, r, value in operands if kind == 'R']
        if m == name and n == len(extensions) + 1 and (reg is None or regs == [reg]):
            return decoder.decode([op, *extensions], 0, pc)
    raise AssertionError((name, reg, extensions))

class TransferAnalysis(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        global candidate
        spec = importlib.util.spec_from_file_location("candidate", GENERATOR.resolve())
        candidate = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(candidate)

    def program(self, call=None):
        producer = instruction(BASE, 'MOVI', 'A7', (0x8000, 0xffe1))
        pc = BASE + producer.words * 16
        code = {BASE: producer}
        if call:
            extensions = () if call == 'CALL' else ((0x1000,) if call == 'CALLR' else (0x1000, 0))
            middle = instruction(pc, call, 'B0' if call == 'CALL' else None, extensions)
            code[pc] = middle
            pc += middle.words * 16
        code[pc] = instruction(pc, 'EXGPC', 'A7')
        return code, pc

    def test_adjacent_literal_target_remains_locally_resolved(self):
        code, site = self.program()
        self.assertNotIn(site, candidate.unresolved_transfers(code))

    def test_CALLR_can_clobber_prior_register_constant(self):
        code, site = self.program('CALLR')
        self.assertIn(site, candidate.unresolved_transfers(code))

    def test_CALLA_can_clobber_prior_register_constant(self):
        code, site = self.program('CALLA')
        self.assertIn(site, candidate.unresolved_transfers(code))

    def test_CALL_can_clobber_prior_register_constant(self):
        code, site = self.program('CALL')
        self.assertIn(site, candidate.unresolved_transfers(code))

    def test_direct_branch_destination_is_a_new_fact_boundary(self):
        code, site = self.program()
        branch = instruction(BASE - 32, 'JREQ', extensions=(4,))
        # Use the decoder's actual target to position a literal producer before
        # a branch entry. Entry facts must not depend on the producer's path.
        site = branch.target
        producer = instruction(site - 48, 'MOVI', 'A7', (0x8000, 0xffe1))
        code = {branch.pc: branch, producer.pc: producer, site: instruction(site, 'EXGPC', 'A7')}
        self.assertIn(site, candidate.unresolved_transfers(code))

    def test_analysis_is_independent_of_dictionary_insertion_order(self):
        code, site = self.program('CALLR')
        self.assertEqual(candidate.unresolved_transfers(code), candidate.unresolved_transfers(dict(reversed(list(code.items())))))

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--generator', type=Path, default=GENERATOR)
    args = parser.parse_args()
    GENERATOR = args.generator
    unittest.main(argv=[sys.argv[0]], verbosity=2)
