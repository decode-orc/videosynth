#!/usr/bin/env python3
# File:        test_migrate_laserdisc_keys.py
# Module:      project_fixture_tests
# Purpose:     Self-test of scripts/migrate_laserdisc_keys.py over in-memory
#              YAML: every moved key lands in its laserdisc block, comments
#              survive, migrated text is stable, and unknown layouts are
#              refused.
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Simon Inns

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))

import migrate_laserdisc_keys as mig  # noqa: E402

OLD = '''# A PAL CAV disc
project:
  name: Disc

cvbs_presets:
  video_standard_preset: PAL
  pal_laserdisc_pilot_burst: true

output:
  video_path: "{output}/disc.cvbs"
  efm_audio:
    pair: 0

line_injections:
  disc_type: CAV
  vits:
    - vits_type: uk-national
      target_lines: [19]

channel:
  timing_offset_samples: 0.5
  fm_beat:
    level_db: -24.0

sections:
  # The programme
  - name: Programme
    type: progressive
    duration_frames: 4
    section_type: programme_area
    noise:
      noise_db: 46.0
    line_injections:
      - type: laserdisc
        codes:
          - code_type: picture_number
            start_value: 1
'''

NEW = '''# A PAL CAV disc
project:
  name: Disc

cvbs_presets:
  video_standard_preset: PAL

output:
  video_path: "{output}/disc.cvbs"

line_injections:
  vits:
    - vits_type: uk-national
      target_lines: [19]

laserdisc:
  disc_type: CAV
  pilot_burst: true
  efm_audio:
    pair: 0
  channel:
    fm_beat:
      level_db: -24.0

channel:
  timing_offset_samples: 0.5

sections:
  # The programme
  - name: Programme
    type: progressive
    duration_frames: 4
    laserdisc:
      area: programme_area
      codes:
        - code_type: picture_number
          start_value: 1
    noise:
      noise_db: 46.0
'''


class MigrateLaserdiscKeysTest(unittest.TestCase):

    def test_moves_every_key_into_the_laserdisc_blocks(self):
        self.assertEqual(mig.migrate(OLD), NEW)

    def test_migrated_text_is_stable(self):
        self.assertEqual(mig.migrate(NEW), NEW)

    def test_drops_blocks_the_move_empties(self):
        old = ('cvbs_presets:\n  video_standard_preset: PAL\n'
               'line_injections:\n  disc_type: CLV\n'
               'channel:\n  fm_beat: { level_db: -30 }\n'
               'sections:\n  - name: S\n')
        new = mig.migrate(old)
        self.assertNotIn('line_injections', new)
        self.assertNotIn('\nchannel:', new)
        self.assertIn('laserdisc:\n  disc_type: CLV\n  channel:\n'
                      '    fm_beat: { level_db: -30 }\n', new)

    def test_area_without_codes(self):
        old = ('sections:\n  - name: LeadIn\n    section_type: lead_in\n'
               '    duration_frames: 50\n')
        self.assertEqual(mig.migrate(old),
                         'sections:\n  - name: LeadIn\n    laserdisc:\n'
                         '      area: lead_in\n    duration_frames: 50\n')

    def test_refuses_a_non_laserdisc_section_injection(self):
        old = ('sections:\n  - name: S\n    line_injections:\n'
               '      - type: vitc\n        target_lines: [16]\n')
        with self.assertRaises(mig.MigrationError):
            mig.migrate(old)


if __name__ == '__main__':
    unittest.main()
