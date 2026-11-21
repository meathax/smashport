from copy import deepcopy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools/scenarios'))
import clearance_contract as audit


class ClearanceContractTests(unittest.TestCase):
    def test_lookup_rejects_wrong_head_field_walk_and_restore(self):
        _, _, _, _, ins, _ = audit.a.load_current(audit.ROOT)
        seq = audit.a.window(ins, 0xffe03660, 0xffe03780)
        audit.validate_lookup(seq)
        for index, operand, value in [(1, 0, 0x01000020), (5, 0, 0xffe03770),
                (6, 0, 0x140), (9, 0, 0xffe036f0), (11, 1, 8)]:
            with self.subTest(index=index):
                changed = deepcopy(seq)
                changed[index].ops[operand].value = value
                with self.assertRaises(audit.a.ContractError):
                    audit.validate_lookup(changed)

    def test_retail_guard_rejects_wrong_exception_mask_helper_and_branches(self):
        _, _, _, _, ins, _ = audit.a.load_current(audit.ROOT)
        seq = audit.a.window(ins, 0xffec71e0, 0xffec7340)
        audit.validate(seq)
        for index, operand, value in [(1, 0, 0xffff), (2, 0, 0xffe03670),
                (3, 0, 0xffec7340), (5, 0, 1), (6, 0, 0xffec73a0),
                (8, 0, 6), (9, 0, 0xffec7340)]:
            with self.subTest(index=index):
                changed = deepcopy(seq)
                changed[index].ops[operand].value = value
                with self.assertRaises(audit.a.ContractError):
                    audit.validate(changed)
        with self.assertRaises(audit.a.ContractError):
            audit.validate(seq[:-1])


if __name__ == '__main__':
    unittest.main()
