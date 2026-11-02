#!/usr/bin/env python3
"""Move a project's LaserDisc keys into the laserdisc blocks.

Project files written before the LaserDisc settings were gathered into
`laserdisc:` blocks are rejected by the parser, each old key with an error
naming its new place. This script rewrites such files in place:

  cvbs_presets.pal_laserdisc_pilot_burst  -> laserdisc.pilot_burst
  cvbs_presets.ntsc_laserdisc_vbi_burst   -> laserdisc.vbi_burst
  line_injections.disc_type               -> laserdisc.disc_type
  output.efm_audio                        -> laserdisc.efm_audio
  channel.fm_beat                         -> laserdisc.channel.fm_beat
  sections[].section_type                 -> sections[].laserdisc.area
  sections[].line_injections laserdisc    -> sections[].laserdisc.codes

It edits the YAML text line by line, so comments and layout survive, and
expects block-style YAML as the emitter writes it. A layout it does not
recognise is reported and the file is left alone. Comments that mention the
old keys are not rewritten.

Usage:
  migrate_laserdisc_keys.py [--check] FILE...

With --check nothing is written; the exit status is 1 when any file would
change.
"""
import argparse
import re
import sys


class MigrationError(Exception):
    """The file uses a layout the migration does not recognise."""


def top_level_blocks(lines):
    """(key, start, end) for each top-level key; end is exclusive."""
    starts = [(i, m.group(1)) for i, l in enumerate(lines)
              if (m := re.match(r'^([a-z_]+):', l))]
    out = []
    for n, (i, key) in enumerate(starts):
        end = starts[n + 1][0] if n + 1 < len(starts) else len(lines)
        out.append((key, i, end))
    return out


def take_key(lines, start, end, key, indent):
    """Removes `key` (and its deeper-indented children) inside [start, end).

    Returns (value_text_or_None, child_lines, removed_count)."""
    pat = re.compile(r'^' + ' ' * indent + re.escape(key) + r':(.*)$')
    for i in range(start, end):
        m = pat.match(lines[i])
        if not m:
            continue
        j = i + 1
        while j < end and (lines[j].strip() == '' and j + 1 < end and
                           lines[j + 1].startswith(' ' * (indent + 1)) or
                           lines[j].startswith(' ' * (indent + 1))):
            j += 1
        children = lines[i + 1:j]
        del lines[i:j]
        return m.group(1).strip(), children, j - i
    return None, [], 0


def block_is_empty(lines, start, end):
    return all(l.strip() == '' or l.lstrip().startswith('#')
               for l in lines[start + 1:end])


def migrate_sections(lines, start, end):
    """Rewrites section_type / laserdisc line_injections inside sections."""
    body = lines[start:end]
    item_starts = [i for i, l in enumerate(body) if l.startswith('  - ')]
    item_starts.append(len(body))
    out = body[:item_starts[0]]
    for a, b in zip(item_starts, item_starts[1:]):
        item = body[a:b]
        area = None
        codes = None
        rest = []
        i = 0
        while i < len(item):
            line = item[i]
            m = re.match(r'^    section_type:\s*(\S+)', line)
            if m:
                area = m.group(1)
                rest.append('@@LASERDISC@@')
                i += 1
                continue
            if re.match(r'^    line_injections:\s*$', line):
                j = i + 1
                while j < len(item) and (item[j].startswith('      ') or
                                         item[j].strip() == ''):
                    if item[j].strip() == '' and not (
                            j + 1 < len(item) and
                            item[j + 1].startswith('      ')):
                        break
                    j += 1
                block = item[i + 1:j]
                entries = [l for l in block if l.strip()]
                if not entries or entries[0].strip() != '- type: laserdisc':
                    raise MigrationError('a section line_injections entry is not a laserdisc injection')
                if sum(1 for l in entries if l.startswith('      - ')) != 1:
                    raise MigrationError('a section has more than one line injection')
                if entries[1].strip() != 'codes:':
                    raise MigrationError('a laserdisc injection has no codes list')
                codes = [l[2:] if l.startswith('  ') else l
                         for l in block[block.index(entries[1]) + 1:]]
                if '@@LASERDISC@@' not in rest:
                    rest.append('@@LASERDISC@@')
                i = j
                continue
            rest.append(line)
            i += 1
        if '@@LASERDISC@@' in rest:
            idx = rest.index('@@LASERDISC@@')
            new = ['    laserdisc:']
            if area:
                new.append('      area: ' + area)
            if codes:
                new.append('      codes:')
                new.extend(codes)
            rest[idx:idx + 1] = new
        out.extend(rest)
    lines[start:end] = out


def migrate(text):
    lines = text.split('\n')
    ld = []  # (key, value, children)

    def block(key):
        for k, s, e in top_level_blocks(lines):
            if k == key:
                return s, e
        return None

    for key, src, dst in (('pal_laserdisc_pilot_burst', 'cvbs_presets',
                           'pilot_burst'),
                          ('ntsc_laserdisc_vbi_burst', 'cvbs_presets',
                           'vbi_burst')):
        b = block(src)
        if b:
            value, _, n = take_key(lines, b[0], b[1], key, 2)
            if n:
                ld.append((dst, value, []))
    b = block('line_injections')
    disc = None
    if b:
        value, _, n = take_key(lines, b[0], b[1], 'disc_type', 2)
        if n:
            disc = value
        b = block('line_injections')
        if block_is_empty(lines, b[0], b[1]):
            del lines[b[0]:b[0] + 1]
            while b[0] < len(lines) and lines[b[0]].strip() == '' and \
                    b[0] > 0 and lines[b[0] - 1].strip() == '':
                del lines[b[0]]
    if disc:
        ld.insert(0, ('disc_type', disc, []))
    b = block('output')
    if b:
        value, children, n = take_key(lines, b[0], b[1], 'efm_audio', 2)
        if n:
            ld.append(('efm_audio', value, children))
    fm = None
    b = block('channel')
    if b:
        value, children, n = take_key(lines, b[0], b[1], 'fm_beat', 2)
        if n:
            fm = (value, children)
        b = block('channel')
        if block_is_empty(lines, b[0], b[1]):
            del lines[b[0]:b[0] + 1]
            while b[0] < len(lines) and lines[b[0]].strip() == '' and \
                    b[0] > 0 and lines[b[0] - 1].strip() == '':
                del lines[b[0]]
    if fm is not None:
        value, children = fm
        ld.append(('channel', '',
                   ['    fm_beat:' + (' ' + value if value else '')] +
                   ['  ' + l if l.strip() else l for l in children]))

    b = block('sections')
    if b:
        migrate_sections(lines, b[0], b[1])

    if ld:
        new = ['laserdisc:']
        for key, value, children in ld:
            new.append('  ' + key + ':' + (' ' + value if value else ''))
            new.extend(children)
        new.append('')
        anchor = None
        for k, s, e in top_level_blocks(lines):
            if k in ('channel', 'chroma_error', 'sections'):
                anchor = s
                break
        # Keep a comment block that introduces the anchor with the anchor.
        while anchor > 0 and lines[anchor - 1].startswith('#'):
            anchor -= 1
        lines[anchor:anchor] = new
    return '\n'.join(lines)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description='Move LaserDisc keys into the laserdisc blocks.')
    parser.add_argument('--check', action='store_true',
                        help='report files that would change; write nothing')
    parser.add_argument('files', nargs='+')
    args = parser.parse_args(argv)
    status = 0
    for path in args.files:
        with open(path, encoding='utf-8') as f:
            text = f.read()
        try:
            new = migrate(text)
        except MigrationError as error:
            print(f'{path}: not migrated: {error}', file=sys.stderr)
            status = 2
            continue
        if new == text:
            continue
        if args.check:
            print(f'{path}: would migrate')
            status = max(status, 1)
            continue
        with open(path, 'w', encoding='utf-8') as f:
            f.write(new)
        print(f'{path}: migrated')
    return status


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
