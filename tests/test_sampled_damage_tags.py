import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools/scenarios'))
from summarize_damage_tags import summarize_rows


def row(frame, dead=0, lives=3, **changes):
    result = dict(frame=frame, circuit=1, wave=17, state=16, slot=1,
                  process=0x01087e10, owner=1, dead=dead, lives=lives)
    result.update(changes)
    return result


class SampledDamageTests(unittest.TestCase):
    def test_persistent_tag_matches_one_drop_only(self):
        result = summarize_rows([row(1), row(2, 0x8300), row(3, 0x8300, 2), row(4, 0x8300, 1)])
        first, second = result['events']
        self.assertEqual((first['type_bits'], first['tag_age_frames']), (3, 1))
        self.assertFalse(first['same_sample'])
        self.assertEqual(second['classification'], 'unassociated_decrease')

    def test_same_sample_is_explicit(self):
        event = summarize_rows([row(1), row(2, 0x8300, 2)])['events'][0]
        self.assertTrue(event['same_sample'])

    def test_tag_cleared_at_life_decrease_keeps_preceding_scope(self):
        event = summarize_rows([row(1), row(2, 0x8300), row(3, 0, 2)])['events'][0]
        self.assertEqual((event['classification'], event['tag']), ('preceding_cleared_tag', 0x8300))
        self.assertFalse(event['same_sample'])

    def test_inherited_tag_is_not_a_new_assignment(self):
        event = summarize_rows([row(1, 0x8300), row(2, 0x8300, 2)])['events'][0]
        self.assertEqual(event['classification'], 'unassociated_decrease')

    def test_slot_reuse_and_invalid_owner_are_unassociated(self):
        for changes in ({'owner': 2}, {'process': 0x01087e11}, {'process': 0x010883b0}):
            event = summarize_rows([row(1), row(2, 0x8300), row(3, 0x8300, 2, **changes)])['events'][0]
            self.assertEqual(event['classification'], 'unassociated_decrease')

    def test_boundaries_and_corruption_are_excluded(self):
        for changes, first, reason in (({'wave': 18}, None, 'noncombat_or_room_boundary'),
                                       ({}, 3, 'at_or_after_invalid_list')):
            result = summarize_rows([row(1), row(2, 0x8300), row(3, 0x8300, 2, **changes)], first)
            self.assertEqual(result['events'], [])
            self.assertEqual(result['excluded'][0]['reason'], reason)

    def test_unordered_rows_fail(self):
        with self.assertRaises(ValueError):
            summarize_rows([row(3), row(2)])

    def test_malformed_fields_and_duplicate_samples_fail(self):
        for sample in (row(1, dead=65536), row(1, lives=-1), row(1, owner=256)):
            with self.assertRaises(ValueError):
                summarize_rows([sample])
        with self.assertRaises(ValueError):
            summarize_rows([row(1), row(1, 0x8300, 2)])


if __name__ == '__main__':
    unittest.main()
