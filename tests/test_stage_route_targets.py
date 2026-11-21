import sys
import unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/scenarios'))
from audit_all_stages import parse_route_targets
class Targets(unittest.TestCase):
 def test_default_unchanged(self):self.assertEqual(parse_route_targets([]),{})
 def test_multiple_paths(self):self.assertEqual(parse_route_targets(['0=5,13,16','2=5,18,22']),{0:'5,13,16',2:'5,18,22'})
 def test_bad_paths(self):
  for value in ['0','x=5,18,22','0=5,18','0=5,18,22,1','0=0,18,22','0=11,18,22','0=5,19,22','0=5,18,23','-1=5,18,22']:
   with self.subTest(value=value),self.assertRaises(ValueError):parse_route_targets([value])
 def test_duplicate_rejected(self):
  with self.assertRaises(ValueError):parse_route_targets(['0=5,18,18','0=5,18,22'])
if __name__=='__main__':unittest.main()
