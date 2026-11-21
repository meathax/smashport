from copy import deepcopy
from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/scenarios'))
import dma_interrupt_contract as audit


class DmaContractTests(unittest.TestCase):
    def test_rejects_a_register_stack_mask_and_external_branch(self):
        _,_,_,_,ins,_=audit.a.load_current(audit.ROOT)
        entry=audit.a.window(ins,0xffe00000,0xffe00140)
        helper=audit.a.window(ins,0xffe00710,0xffe00ce0)
        audit.validate(entry,helper)
        for kind in ('register','stack','mask','call','branch'):
            e,h=deepcopy(entry),deepcopy(helper)
            if kind=='register':h[0].ops[0].reg='A7'
            elif kind=='stack':e[0].ops[0].reg='SP'
            elif kind=='mask':h[37].ops[1].value=0x8000
            elif kind=='call':e[7].ops[0].value=0xffe00ce0
            else:e[5].ops[0].value=0xffe00140
            with self.subTest(kind=kind),self.assertRaises(audit.a.ContractError):audit.validate(e,h)


if __name__=='__main__':unittest.main()
