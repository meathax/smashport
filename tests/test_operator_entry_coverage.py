from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/scenarios'))
import operator_entry_coverage as audit


class CoverageTests(unittest.TestCase):
    def test_exact_slots_binary_values_and_unique_addresses(self):
        self.assertEqual(audit.coverage_hits(bytes([0,1]),[16,32]),{32})
        for data,addresses in [(bytes([1]),[16,32]),(bytes([2]),[16]),(bytes([1,1]),[16,16])]:
            with self.assertRaises(audit.runner.ProofError):audit.coverage_hits(data,addresses)


if __name__=='__main__':unittest.main()
