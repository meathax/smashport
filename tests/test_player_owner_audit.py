"""Source-owner attribution, raw/full stream and strict-window regressions."""
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/scenarios'))
import player_owner_audit as audit
import rebaseline as runner

T1, L1, T2, L2, HELPER = [audit.BASE + n for n in (0x4000, 0x5000, 0x6000, 0x7000, 0x8000)]


class CurrentWeaponCatalogTests(unittest.TestCase):
    def binding(self):
        retail={}
        for _,_,constant,write,_,pid,target,call,entry in audit.actions.WEAPONS:
            for pc in [pid,target,call,entry]+([constant[1],write[1]] if write else []):retail[pc]=dict(pc=pc)
        return dict(retail=retail)

    def test_all_five_modes_without_historical_inventory(self):
        catalog=audit.weapon_catalog(self.binding())
        self.assertEqual([w['WPN_TYP_value'] for w in catalog['weapons']],list(range(5)))
        self.assertEqual(catalog['global_content_completeness'],'OPEN')
        self.assertTrue(all(not w['player_event_proof'] for w in catalog['weapons']))

    def test_missing_or_wrong_retail_site_rejected(self):
        for wrong in (False,True):
            binding=self.binding();pc=next(iter(binding['retail']))
            if wrong:binding['retail'][pc]['pc']='00000000'
            else:del binding['retail'][pc]
            with self.assertRaises(runner.ProofError):audit.weapon_catalog(binding)


def put(image, address, value, width=32):
    # Independent byte-aligned fixture writes; +0x198 is an odd-byte word.
    assert (address - audit.BASE) % 8 == 0
    struct.pack_into({8: '<B', 16: '<H', 32: '<I'}[width], image, (address - audit.BASE) // 8, value)


def image_fixture():
    image = bytearray(audit.RAM_SIZE)
    put(image, audit.BASE, T1)
    nodes = [T1, L1, T2, L2, HELPER]
    for node, following, pid in zip(nodes, nodes[1:] + [0], (247, 247, 248, 248, 247)):
        put(image, node, following)
        put(image, node + 0x20, pid, 16)
        put(image, node + 0xE0, 0xFFE08490)  # same animation helper for every process
    for player, torso, legs, mode in ((1, T1, L1, 3), (2, T2, L2, 4)):
        put(image, audit.REGISTRY + (player - 1) * 32, torso)
        put(image, torso + 0x1D8, player, 8)
        put(image, torso + 0x2C0, legs)
        put(image, legs + 0x160, torso)
        put(image, torso + 0x100, audit.BASE + 0xA000 + player * 0x100)
        put(image, legs + 0x100, audit.BASE + 0xA000 + player * 0x100)
        put(image, torso + 0x198, mode, 16)
        put(image, legs + 0x198, 2, 16)  # same PID and a plausible false weapon
    put(image, HELPER + 0x198, 1, 16)
    put(image, 0x010865A0, 16, 16)
    put(image, 0x010EB610, 1, 16)
    put(image, 0x010EB630, 7, 16)
    put(image, 0x01086360, 3, 16)
    put(image, 0x01086520, 2, 16)
    return image


def sidecar(image, frame=0):
    rows, complete = audit.process_list(image)
    return dict(frame=frame, gamstate=audit.read_bits(image, 0x010865A0, 16), circuit=1, wave=7,
                p1_lives=3, p2_lives=2, processes=rows, process_list_complete=complete)


def full_records(image):
    return [(1, 0, struct.pack('<H', n) + image[n * 256:(n + 1) * 256]) for n in range(audit.RAM_SIZE // 256)] + [
        (9, 0, struct.pack('<II', 123, 4)), (8, 0, b'')]


class OwnerTests(unittest.TestCase):
    def setUp(self):
        self.image = image_fixture()

    def check_reason(self, reason):
        bound, excluded = audit.owners(self.image, sidecar(self.image))
        self.assertEqual([p['player'] for p in bound], [2])
        self.assertEqual(excluded, [dict(player=1, reason=reason)])

    def test_same_pid_and_shared_wake_do_not_bind_legs_or_helper(self):
        bound, excluded = audit.owners(self.image, sidecar(self.image))
        self.assertEqual([(p['player'], p['torso'], p['legs'], p['weapon']) for p in bound],
                         [(1, T1, L1, 'grenade'), (2, T2, L2, 'nonstop')])
        self.assertEqual(excluded, [])

    def test_stale_registry_pointer_is_excluded(self):
        put(self.image, audit.REGISTRY, audit.BASE + 0xB000)
        self.check_reason('registry_owner_not_active')

    def test_empty_player_slot_is_excluded(self):
        put(self.image, audit.REGISTRY, 0)
        self.check_reason('empty_player_registry')

    def test_wrong_pid_is_excluded(self):
        put(self.image, T1 + 0x20, 200, 16)
        self.check_reason('registry_owner_pid_differs')

    def test_wrong_player_number_is_excluded(self):
        put(self.image, T1 + 0x1D8, 2, 8)
        self.check_reason('registry_owner_player_number_differs')

    def test_dead_leg_pointer_is_excluded(self):
        put(self.image, T1 + 0x2C0, audit.BASE + 0xB000)
        self.check_reason('leg_owner_not_distinct_and_active')

    def test_self_leg_pointer_is_excluded(self):
        put(self.image, T1 + 0x2C0, T1)
        self.check_reason('leg_owner_not_distinct_and_active')

    def test_leg_of_other_player_is_excluded(self):
        put(self.image, L1 + 0x20, 248, 16)
        self.check_reason('leg_owner_pid_differs')

    def test_stale_reciprocal_link_is_excluded(self):
        put(self.image, L1 + 0x160, T2)
        self.check_reason('leg_torso_backlink_differs')

    def test_mismatched_torso_object_is_excluded(self):
        put(self.image, L1 + 0x100, 0)
        self.check_reason('leg_torso_object_link_differs')

    def test_unknown_owned_selector_is_retained_without_weapon(self):
        put(self.image, T1 + 0x198, 65535, 16)
        bound, _ = audit.owners(self.image, sidecar(self.image))
        self.assertIsNone(bound[0]['weapon'])
        self.assertEqual(bound[0]['selector'], 65535)

    def test_byte_unaligned_weapon_preserves_adjacent_bits(self):
        self.image[(T1 + 0x198 - audit.BASE) // 8 - 1] = 0xA5
        put(self.image, T1 + 0x198, 0xFEDC, 16)
        self.image[(T1 + 0x198 - audit.BASE) // 8 + 2] = 0x5A
        self.assertEqual(audit.read_bits(self.image, T1 + 0x198, 16), 0xFEDC)

    def test_non_byte_field_read(self):
        self.image[:3] = b'\x69\xb7\x12'
        self.assertEqual(audit.read_bits(self.image, audit.BASE + 3, 16), 0x56ED)

    def test_invalid_ram_reads_fail(self):
        for address, width in [(audit.BASE - 1, 16), (audit.BASE + audit.RAM_SIZE * 8 - 8, 16), (audit.BASE, 0), (audit.BASE, 33)]:
            with self.subTest(address=address, width=width), self.assertRaises(runner.ProofError):
                audit.read_bits(self.image, address, width)

    def test_torso_extent_is_excluded_before_reading_out_of_range_player_number(self):
        near_end = audit.BASE + audit.RAM_SIZE * 8 - 0x1B0
        put(self.image, audit.BASE, near_end)
        put(self.image, near_end, L1)
        put(self.image, near_end + 0x20, 247, 16)
        put(self.image, audit.REGISTRY, near_end)
        self.check_reason('torso_link_outside_ram')

    def test_unaligned_process_pointer_is_an_incomplete_boundary(self):
        put(self.image, audit.BASE, T1 + 8)
        sample = sidecar(self.image)
        self.assertEqual(sample['processes'], [dict(boundary_address=T1 + 8, reason='invalid_or_cyclic_ram_node')])
        self.assertFalse(sample['process_list_complete'])

    def test_cyclic_list_keeps_boundary_and_excludes_both_owners(self):
        put(self.image, HELPER, T1)
        sample = sidecar(self.image)
        self.assertEqual(sample['processes'][-1], dict(boundary_address=T1, reason='invalid_or_cyclic_ram_node'))
        self.assertEqual(audit.owners(self.image, sample), ([], [dict(reason='incomplete_active_process_list')]))

    def test_out_of_range_list_keeps_boundary(self):
        put(self.image, HELPER, 0x01100000)
        self.assertFalse(sidecar(self.image)['process_list_complete'])

    def test_attract_does_not_count_gameplay_weapons(self):
        put(self.image, 0x010865A0, 0, 16)
        self.assertEqual(audit.owners(self.image, sidecar(self.image)), ([], [dict(reason='outside_in_game_state')]))

    def test_same_frame_sidecar_process_disagreement_fails(self):
        sample = sidecar(self.image)
        sample['processes'][0]['pdata198_raw'] = 1
        with self.assertRaisesRegex(runner.ProofError, 'sidecar disagree'):
            audit.owners(self.image, sample)

    def test_sidecar_global_disagreement_fails(self):
        sample = sidecar(self.image)
        sample['wave'] = 8
        with self.assertRaisesRegex(runner.ProofError, 'globals disagree'):
            audit.owners(self.image, sample)


class StreamTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.image = image_fixture()

    def stream(self, records, path='full.bin'):
        p = self.directory / path
        with p.open('wb') as output:
            output.write(b'STVLS001')
            for kind, frame, payload in records:
                audit.write_record(output, kind, frame, payload)
        return p

    def convert(self, records=None, sample=None, header=None, final=0):
        p = self.stream(records if records is not None else full_records(self.image))
        header = header or dict(schema='stv.content.samples.v1', cadence=60, phase='before_main_region_dump', process_pdata198='raw_unbound_field')
        content = self.directory / 'content.jsonl'
        content.write_text(json.dumps(header) + '\n' + json.dumps(sample or sidecar(self.image)) + '\n', encoding='utf-8')
        return audit.convert_and_observe(p, content, self.directory / 'compact.bin', final)

    def test_full_ram_binds_both_players_and_excludes_extra_pid_processes(self):
        result = self.convert()
        self.assertEqual(result['totals']['bound_player_samples'], 2)
        self.assertEqual(result['totals']['unattributed_player_pid_samples'], 3)
        self.assertEqual(result['endpoint']['frames'], 1)
        self.assertEqual(result['bound_weapon_selectors'][0]['weapon'], 'grenade')

    def test_compact_hash_is_not_ram(self):
        with self.assertRaisesRegex(runner.ProofError, 'compact hashes'):
            self.convert([(1, 0, struct.pack('<HI', 0, 123)), (9, 0, b'0' * 8), (8, 0, b'')])

    def test_missing_initial_chunk_fails(self):
        with self.assertRaisesRegex(runner.ProofError, 'initial full RAM'):
            self.convert(full_records(self.image)[1:])

    def test_duplicate_region_chunk_fails(self):
        rows = full_records(self.image)
        with self.assertRaisesRegex(runner.ProofError, 'duplicated'):
            self.convert([rows[0]] + rows)

    def test_out_of_range_region_chunk_fails(self):
        rows = full_records(self.image)
        rows[0] = (1, 0, struct.pack('<H', 512) + b'0' * 256)
        with self.assertRaisesRegex(runner.ProofError, 'index invalid'):
            self.convert(rows)

    def test_truncated_frame_fails(self):
        with self.assertRaisesRegex(runner.ProofError, 'frame sequence'):
            self.convert(full_records(self.image)[:-1])

    def test_shorter_stream_cannot_prove_intended_frame(self):
        with self.assertRaisesRegex(runner.ProofError, 'endpoint incomplete'):
            self.convert(final=1)

    def test_duplicate_summary_fails(self):
        rows = full_records(self.image)
        with self.assertRaisesRegex(runner.ProofError, 'duplicate'):
            self.convert(rows[:-1] + [rows[-2], rows[-1]])

    def test_record_frame_disagreement_fails(self):
        rows = full_records(self.image)
        rows[0] = (1, 1, rows[0][2])
        with self.assertRaisesRegex(runner.ProofError, 'frame end'):
            self.convert(rows)

    def test_sidecar_frame_disagreement_fails(self):
        with self.assertRaisesRegex(runner.ProofError, 'sample frame'):
            self.convert(sample=sidecar(self.image, 60))

    def test_unknown_sampling_phase_fails(self):
        with self.assertRaisesRegex(runner.ProofError, 'phase/cadence'):
            self.convert(header=dict(schema='stv.content.samples.v1', cadence=60, phase='after_dump', process_pdata198='raw_unbound_field'))

    def test_existing_compact_output_is_preserved(self):
        output = self.directory / 'compact.bin'
        output.write_bytes(b'preserved')
        with self.assertRaises(FileExistsError):
            self.convert()
        self.assertEqual(output.read_bytes(), b'preserved')

    def test_region_hash_matches_independent_zero_word_golden(self):
        # 64 multiply operations, each XORs zero; ordinary byte-FNV differs.
        expected = 2166136261 * pow(16777619, 64, 2 ** 32) % (2 ** 32)
        self.assertEqual(audit.chunk_hash(b'\0' * 256), expected)

    def test_complete_main_window_has_exact_endpoint(self):
        source = self.stream([(9, 0, b'0' * 8), (8, 0, b''), (9, 1, b'1' * 8), (8, 1, b'')])
        output = self.directory / 'window.bin'
        self.assertEqual(audit.main_window(source, output, 0)['frames'], 1)
        self.assertEqual(list(runner.lsfile.records(output)), [(9, 0, b'0' * 8), (8, 0, b'')])

    def test_main_window_missing_requested_endpoint_fails(self):
        source = self.stream([(9, 0, b'0' * 8), (8, 0, b'')])
        with self.assertRaisesRegex(runner.ProofError, 'window incomplete'):
            audit.main_window(source, self.directory / 'window.bin', 1)

    def test_sound_window_requires_final_summary(self):
        source = self.directory / 'snd.bin'
        source.write_bytes(b'STVSND01' + b'\x09' + struct.pack('<III', 0, 123, 456) + b'\x09' + struct.pack('<III', 1, 234, 567))
        result = audit.sound_window(source, self.directory / 'window.snd', 0)
        self.assertEqual((result['summaries'], result['count'], result['hash']), (1, 123, '000001c8'))

    def test_sound_window_rejects_truncation(self):
        source = self.directory / 'snd.bin'
        source.write_bytes(b'STVSND01' + b'\x09' + b'0' * 11)
        with self.assertRaisesRegex(runner.ProofError, 'truncated'):
            audit.sound_window(source, self.directory / 'window.snd', 0)

    def test_empty_sound_window_cannot_succeed(self):
        source = self.directory / 'snd.bin'
        source.write_bytes(b'STVSND01')
        with self.assertRaisesRegex(runner.ProofError, 'window incomplete'):
            audit.sound_window(source, self.directory / 'window.snd', 0)

    def test_unknown_sound_record_is_rejected(self):
        source = self.directory / 'snd.bin'
        source.write_bytes(b'STVSND01\x08')
        with self.assertRaisesRegex(runner.ProofError, 'unknown sound oracle record'):
            audit.sound_window(source, self.directory / 'window.snd', 0)

    def test_mutated_artifact_pin_is_rejected(self):
        source = self.directory / 'artifact'
        source.write_bytes(b'original')
        pin = runner.pin(source)
        source.write_bytes(b'modified')
        with self.assertRaisesRegex(runner.ProofError, 'artifact changed'):
            audit.checked_pin(pin)


class CaptureCommandTests(unittest.TestCase):
    def setUp(self):
        self.baseline = ['native.exe', 'retail.zip', 'full.bin', '10', '--native-hcount', '--inputs', 'legal.inputs',
                         '--snd', 'full.snd', '--coverage', 'full.cov', '--content', 'full.jsonl']

    def test_bounded_frame_and_fresh_outputs_are_accepted(self):
        command = list(self.baseline)
        command[2:4] = ['fresh.bin', '5']
        command[command.index('--snd') + 1] = 'fresh.snd'
        self.assertEqual(audit.capture_command(command, self.baseline), 5)

    def test_missing_sound_has_named_boundary(self):
        command = list(self.baseline)
        del command[7:9]
        with self.assertRaisesRegex(runner.ProofError, 'requires --inputs, --snd'):
            audit.capture_command(command, self.baseline)

    def test_missing_inputs_has_named_boundary(self):
        command = list(self.baseline)
        del command[5:7]
        with self.assertRaisesRegex(runner.ProofError, 'requires --inputs, --snd'):
            audit.capture_command(command, self.baseline)

    def test_duplicate_sound_is_rejected(self):
        with self.assertRaisesRegex(runner.ProofError, 'requires --inputs, --snd'):
            audit.capture_command(self.baseline + ['--snd', 'other.snd'], self.baseline)

    def test_changed_input_script_is_rejected(self):
        command = list(self.baseline)
        command[6] = 'other.inputs'
        with self.assertRaisesRegex(runner.ProofError, 'input/profile differs'):
            audit.capture_command(command, self.baseline)

    def test_enhanced_override_cannot_claim_reference_identity(self):
        with self.assertRaisesRegex(runner.ProofError, 'input/profile differs'):
            audit.capture_command(self.baseline + ['--enhanced'], self.baseline)

    def test_capture_cannot_extend_past_parent_oracle(self):
        command = list(self.baseline)
        command[3] = '11'
        with self.assertRaisesRegex(runner.ProofError, 'input/profile differs'):
            audit.capture_command(command, self.baseline)


if __name__ == '__main__':
    unittest.main()
