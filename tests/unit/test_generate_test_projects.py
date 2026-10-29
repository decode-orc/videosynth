#!/usr/bin/env python3
# File:        test_generate_test_projects.py
# Module:      project_fixture_tests
# Purpose:     Self-test of scripts/generate_test_projects.py over in-memory
#              YAML: capture seeds, section removal and replay.
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Simon Inns

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))

import generate_test_projects as gen  # noqa: E402

PRESSING = '''# Pressing A
project:
  name: Pressing
output:
  video_path: "{output}/pressing_A.cvbs"

sections:

  # Banner for the first section
  - name: First
    type: progressive
    noise:
      noise_db: 46.0
      noise_seed: 1001  # capture
      shared:
        noise_db: 44.0
        noise_seed: 77
    dropouts:
      random:
        scale: 3
        seed: 1001
      scratch:
        scale: 2
        seed: 1001
    line_injections:
      - type: laserdisc
        codes:
          - code_type: picture_number
            start_value: 1

  # Banner for the second section
  - name: Second
    type: progressive
    noise:
      noise_db: 46.0
      noise_seed: 1001
      shared:
        noise_db: 44.0
        noise_seed: 77
'''


def values_of(text: str, key: str) -> list[str]:
    return [line.split(':', 1)[1].split('#')[0].strip()
            for line in text.split('\n') if line.strip().startswith(key + ':')]


class CaptureSeedTest(unittest.TestCase):

    def test_replaces_capture_seeds_and_keeps_shared_seeds(self):
        derived = gen.set_capture_seed(PRESSING, 5202)
        noise_seeds = values_of(derived, 'noise_seed')
        # Order per section: capture noise_seed, then shared noise_seed.
        self.assertEqual(noise_seeds, ['5202', '77', '5202', '77'])
        self.assertEqual(values_of(derived, 'seed'), ['5202', '5202'])
        self.assertIn('noise_seed: 5202  # capture', derived)
        self.assertIn('# Banner for the second section', derived)

    def test_rejects_project_without_capture_seeds(self):
        with self.assertRaises(ValueError):
            gen.set_capture_seed(PRESSING.replace('noise_seed: 1001', 'x: 1')
                                 .replace('seed: 1001', 'x: 1'), 1)

    def test_rejects_flow_style_noise_block(self):
        flow = PRESSING.replace(
            '    noise:\n      noise_db: 46.0\n'
            '      noise_seed: 1001  # capture\n'
            '      shared:\n        noise_db: 44.0\n        noise_seed: 77\n',
            '    noise: {noise_db: 46.0, noise_seed: 1001}\n')
        self.assertIn('noise: {', flow)
        with self.assertRaises(ValueError):
            gen.set_capture_seed(flow, 1)


class SectionEditTest(unittest.TestCase):

    def test_remove_drops_entry_and_its_banner(self):
        derived = gen.edit_section_entries(PRESSING, {'First'}, set())
        self.assertNotIn('name: First', derived)
        self.assertNotIn('Banner for the first section', derived)
        self.assertIn('  - name: Second', derived)
        self.assertTrue(derived.startswith('# Pressing A'))

    def test_repeat_inserts_renamed_copy_after_original(self):
        derived = gen.edit_section_entries(PRESSING, set(), {'First'})
        names = values_of(derived, '- name')
        self.assertEqual(names, ['First', 'First_Replay', 'Second'])
        self.assertEqual(values_of(derived, 'start_value'), ['1', '1'])
        # The replay keeps the pressing seed and the section's own keys.
        self.assertEqual(values_of(derived, 'noise_seed').count('77'), 3)

    def test_unknown_section_is_an_error(self):
        with self.assertRaises(ValueError):
            gen.edit_section_entries(PRESSING, {'Missing'}, set())


class CapturesVariantTest(unittest.TestCase):

    VARIANT = {
        'name': 'captures',
        'sources': ['*.yaml'],
        'captures': [
            {'name_suffix': '_cap1', 'capture_seed': 1101},
            {'name_suffix': '_cap2', 'capture_seed': 1202,
             'remove_sections': ['First']},
            {'name_suffix': '_cap3', 'capture_seed': 1303,
             'repeat_sections': ['Second']},
        ],
    }

    def test_each_capture_gets_a_unique_name_and_video_path(self):
        derivations = gen.expand_captures(self.VARIANT)
        self.assertEqual([d['name_suffix'] for d in derivations],
                         ['_cap1', '_cap2', '_cap3'])
        paths = set()
        for derivation in derivations:
            derived = gen.derive(PRESSING, derivation)
            paths.update(values_of(derived, 'video_path'))
            self.assertEqual(values_of(derived, 'noise_seed').count('77'),
                             derived.count('shared:'))
        self.assertEqual(paths, {
            '"{output}/pressing_A_cap1.cvbs"',
            '"{output}/pressing_A_cap2.cvbs"',
            '"{output}/pressing_A_cap3.cvbs"',
        })

    def test_rejects_duplicate_or_missing_suffixes(self):
        duplicate = dict(self.VARIANT, captures=[
            {'name_suffix': '_a', 'capture_seed': 1},
            {'name_suffix': '_a', 'capture_seed': 2}])
        with self.assertRaises(ValueError):
            gen.expand_captures(duplicate)
        missing = dict(self.VARIANT, captures=[{'capture_seed': 1}])
        with self.assertRaises(ValueError):
            gen.expand_captures(missing)

    def test_variant_without_captures_derives_once(self):
        variant = {'name': 'plain', 'sources': [], 'name_suffix': '_clean'}
        self.assertEqual(gen.expand_captures(variant), [variant])


if __name__ == '__main__':
    unittest.main()
