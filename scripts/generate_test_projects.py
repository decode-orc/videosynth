#!/usr/bin/env python3
"""Derive the generated project fixtures from the committed ones.

The committed tree under projects/ holds only hand-authored projects. Every
mechanically derivable variant (Y/C renderings, impairment-free reference
copies) is produced here at build time and written into the build tree, so the
repository never carries near-duplicate YAML.

The rules live in projects/variants.json; see that file for the format. Each
variant supports these transformations, all applied to the YAML *text* so the
authored comments survive into the derived project:

  remove_section_blocks   drop the named keys (and their children) from every
                          entry of the top-level `sections:` list
  signal_type             set output.signal_type and switch the video_path
                          suffix between .cvbs and .cvbsy accordingly
  name_suffix             append a suffix to both the generated filename and
                          the video_path stem
  captures                derive one project per entry instead of one per
                          source: the captures of one pressing. Each entry
                          takes its own name_suffix (required, unique) and:
      capture_seed        replaces every capture seed (noise.noise_seed and
                          dropouts.random/scratch.seed) in every section; the
                          pressing seeds (noise.shared.noise_seed and the
                          seed of a dropout block with `anchor: disc`) are
                          never touched
      remove_sections     drop the named section entries (a later start)
      repeat_sections     play each named section entry twice in a row, the
                          copy renamed <name>_Replay (a replayed range)
      chroma_error        replace the project's chroma_error block with this
                          mapping (the capture's player), or remove it with
                          null
      timing_offset_samples
                          set channel.timing_offset_samples, adding the key,
                          or the channel block, when the project lacks it

The capture keys may also be set on a variant itself, which applies them to
every derived project.

Usage:
  generate_test_projects.py --source projects --output <dir> [--stamp <file>]
"""

import argparse
import json
import re
import sys
from pathlib import Path

# Video path suffix per output signal type.
SIGNAL_TYPE_SUFFIX = {"composite": ".cvbs", "yc": ".cvbsy"}

VIDEO_PATH_RE = re.compile(r'^(?P<indent>\s*)video_path:\s*(?P<value>.+?)\s*$')
SIGNAL_TYPE_RE = re.compile(r'^(?P<indent>\s*)signal_type:\s*(?P<value>.+?)\s*$')


def indent_of(line: str) -> int:
    """Column of the first non-space character; len(line) for a blank line."""
    return len(line) - len(line.lstrip())


def remove_section_blocks(text: str, keys: set[str]) -> str:
    """Remove `keys` and their children from every entry of `sections:`.

    Only keys at a section entry's own property indent are removed, so a
    like-named key nested deeper inside another block is left alone.
    """
    lines = text.split('\n')
    result: list[str] = []
    in_sections = False
    item_indent: int | None = None
    property_indent: int | None = None
    skip_indent: int | None = None

    for line in lines:
        stripped = line.strip()

        # A run of lines belonging to a removed block: they are all indented
        # deeper than the key that introduced it. Blank lines inside the run
        # are dropped with it.
        if skip_indent is not None:
            if not stripped or indent_of(line) > skip_indent:
                continue
            skip_indent = None

        if stripped and indent_of(line) == 0:
            # A new top-level key ends any previous top-level block.
            in_sections = stripped.startswith('sections:')
            item_indent = None
            property_indent = None
            result.append(line)
            continue

        if in_sections and stripped.startswith('- '):
            # The first dash after `sections:` fixes the list's item indent;
            # deeper dashes belong to nested lists (line_injections, overlays)
            # and must not be mistaken for a new section entry.
            if item_indent is None:
                item_indent = indent_of(line)
            if indent_of(line) == item_indent:
                # A section's properties sit one level in from its dash.
                property_indent = item_indent + 2

        if (in_sections and property_indent is not None and stripped and
                indent_of(line) == property_indent):
            key = stripped.split(':', 1)[0].lstrip('- ').strip()
            if key in keys:
                skip_indent = property_indent
                continue

        result.append(line)

    # Collapse the blank-line runs left behind by the removed blocks.
    return re.sub(r'\n{3,}', '\n\n', '\n'.join(result))


# Paths, below a section entry, of the seeds that belong to one capture. The
# pressing seed (noise.shared.noise_seed) is deliberately absent.
CAPTURE_SEED_PATHS = {
    ('noise', 'noise_seed'),
    ('dropouts', 'random', 'seed'),
    ('dropouts', 'scratch', 'seed'),
}
# Blocks whose capture seeds must be visible line by line.
CAPTURE_SEED_BLOCKS = {('noise',), ('dropouts',), ('dropouts', 'random'),
                       ('dropouts', 'scratch')}

KEY_RE = re.compile(r'^(?P<lead>\s*(?:-\s+)?)(?P<key>[A-Za-z_][A-Za-z0-9_]*):'
                    r'(?P<rest>.*)$')


def is_comment_or_blank(line: str) -> bool:
    stripped = line.strip()
    return not stripped or stripped.startswith('#')


def split_sections(text: str) -> tuple[list[str], list[list[str]], list[str]]:
    """Split a project into (lines before, section entries, lines after).

    Each entry runs from its `- ` line to the next entry, and carries the
    comment and blank lines directly above its dash, so removing or copying an
    entry takes its banner comment with it.
    """
    lines = text.split('\n')
    start = next((i for i, line in enumerate(lines)
                  if line.startswith('sections:')), None)
    if start is None:
        raise ValueError('project declares no sections list')
    end = next((i for i in range(start + 1, len(lines))
                if lines[i].strip() and indent_of(lines[i]) == 0 and
                not lines[i].lstrip().startswith('#')), len(lines))

    entries: list[list[str]] = []
    current: list[str] | None = None
    pending: list[str] = []
    item_indent: int | None = None
    for line in lines[start + 1:end]:
        if line.strip().startswith('- ') and (
                item_indent is None or indent_of(line) == item_indent):
            item_indent = indent_of(line)
            if current is not None:
                entries.append(current)
            current = pending + [line]
            pending = []
        elif is_comment_or_blank(line) and (
                item_indent is None or not line.strip() or
                indent_of(line) <= item_indent):
            pending.append(line)
        elif current is None:
            raise ValueError('unexpected content before the first section')
        else:
            current.extend(pending)
            pending = []
            current.append(line)
    if current is not None:
        entries.append(current)
    return lines[:start + 1], entries, pending + lines[end:]


def join_sections(head: list[str], entries: list[list[str]],
                  tail: list[str]) -> str:
    return '\n'.join(head + [line for entry in entries for line in entry] +
                     tail)


def entry_key_lines(entry: list[str]):
    """Yield (index, path, match) for every mapping key in a section entry.

    `path` is the key's position below the entry, e.g. ('noise', 'shared',
    'noise_seed'); the entry's own properties have a one-element path.
    """
    stack: list[tuple[int, str]] = []
    for index, line in enumerate(entry):
        if is_comment_or_blank(line):
            continue
        match = KEY_RE.match(line)
        if match is None:
            continue
        # A key on a dash line sits where the following properties do.
        indent = len(match.group('lead'))
        while stack and stack[-1][0] >= indent:
            stack.pop()
        stack.append((indent, match.group('key')))
        yield index, tuple(key for _, key in stack), match


def entry_name(entry: list[str]) -> str:
    for _, path, match in entry_key_lines(entry):
        if path == ('name',):
            return match.group('rest').strip().strip('"\'')
    raise ValueError('section entry has no name')


def disc_anchored_blocks(entry: list[str]) -> set[tuple[str, ...]]:
    """Paths of the dropout blocks in an entry that set `anchor: disc`.

    Their seeds belong to the pressing, not to one capture.
    """
    return {path[:-1] for _, path, match in entry_key_lines(entry)
            if path[:1] == ('dropouts',) and path[-1] == 'anchor' and
            match.group('rest').split('#')[0].strip().strip('"\'') == 'disc'}


def set_capture_seed(text: str, seed: int) -> str:
    """Replace every capture seed in every section with `seed`."""
    head, entries, tail = split_sections(text)
    replaced = 0
    for entry in entries:
        pressing_blocks = disc_anchored_blocks(entry)
        for index, path, match in entry_key_lines(entry):
            if path[:-1] in pressing_blocks:
                continue
            rest = match.group('rest')
            if path in CAPTURE_SEED_BLOCKS and rest.strip().startswith('{'):
                raise ValueError(
                    f"capture_seed cannot rewrite flow-style '{path[-1]}' "
                    'blocks; write them in block style')
            if path in CAPTURE_SEED_PATHS:
                comment = ''
                if '#' in rest:
                    comment = '  ' + rest[rest.index('#'):].strip()
                entry[index] = (f"{match.group('lead')}{match.group('key')}: "
                                f'{seed}{comment}')
                replaced += 1
    if replaced == 0:
        raise ValueError('capture_seed: project declares no capture seeds')
    return join_sections(head, entries, tail)


def edit_section_entries(text: str, remove: set[str],
                         repeat: set[str]) -> str:
    """Drop the `remove` entries and play the `repeat` entries twice."""
    head, entries, tail = split_sections(text)
    names = [entry_name(entry) for entry in entries]
    missing = sorted((remove | repeat) - set(names))
    if missing:
        raise ValueError(f"no section named '{missing[0]}'")

    result: list[list[str]] = []
    for name, entry in zip(names, entries):
        if name in remove:
            continue
        result.append(entry)
        if name in repeat:
            first = next(i for i, line in enumerate(entry)
                         if not is_comment_or_blank(line))
            indent = ' ' * indent_of(entry[first])
            replay = ['', f'{indent}# Replay of {name}: the player rewinds and',
                      f'{indent}# plays the same picture numbers again.'
                      ] + entry[first:]
            for index, path, match in entry_key_lines(replay):
                if path == ('name',):
                    replay[index] = (f"{match.group('lead')}name: "
                                     f'{name}_Replay')
            result.append(replay)
    if not result:
        raise ValueError('remove_sections removed every section')
    return join_sections(head, result, tail)


def retarget_video_path(text: str, suffix: str, name_suffix: str) -> str:
    """Rewrite output.video_path to use `suffix`, with `name_suffix` on the stem."""
    lines = text.split('\n')
    for index, line in enumerate(lines):
        match = VIDEO_PATH_RE.match(line)
        if match is None:
            continue
        value = match.group('value')
        quote = value[0] if value[:1] in ('"', "'") else ''
        bare = value.strip('"\'')
        for known in SIGNAL_TYPE_SUFFIX.values():
            if bare.endswith(known):
                bare = bare[: -len(known)]
                break
        bare = bare + name_suffix + suffix
        lines[index] = f"{match.group('indent')}video_path: {quote}{bare}{quote}"
        return '\n'.join(lines)
    raise ValueError('project declares no output.video_path')


def set_signal_type(text: str, signal_type: str) -> str:
    """Set output.signal_type, inserting the key after video_path when absent."""
    lines = text.split('\n')
    for index, line in enumerate(lines):
        match = SIGNAL_TYPE_RE.match(line)
        if match is not None:
            lines[index] = f"{match.group('indent')}signal_type: {signal_type}"
            return '\n'.join(lines)

    for index, line in enumerate(lines):
        match = VIDEO_PATH_RE.match(line)
        if match is not None:
            lines.insert(index + 1,
                         f"{match.group('indent')}signal_type: {signal_type}")
            return '\n'.join(lines)
    raise ValueError('project declares no output block to set signal_type in')


def top_level_block_span(lines: list[str], key: str) -> tuple[int, int] | None:
    """Line range [start, end) of a top-level `key:` block and its children."""
    start = next((i for i, line in enumerate(lines)
                  if indent_of(line) == 0 and line.startswith(f'{key}:')), None)
    if start is None:
        return None
    end = start + 1
    while end < len(lines) and (not lines[end].strip() or
                                indent_of(lines[end]) > 0):
        end += 1
    # Leave the blank lines that separate the block from the next one.
    while end > start + 1 and not lines[end - 1].strip():
        end -= 1
    return start, end


def render_yaml_scalar(value) -> str:
    if isinstance(value, bool):
        return 'true' if value else 'false'
    if isinstance(value, (int, float)):
        return repr(value)
    if isinstance(value, str):
        return json.dumps(value)
    raise ValueError(f'cannot render {value!r} as a YAML scalar')


def render_yaml_block(key: str, mapping: dict, indent: int = 0) -> list[str]:
    """Block-style YAML for a mapping of scalars and nested mappings."""
    if not isinstance(mapping, dict) or not mapping:
        raise ValueError(f"'{key}' must be a non-empty mapping")
    pad = ' ' * indent
    lines = [f'{pad}{key}:']
    for child, value in mapping.items():
        if isinstance(value, dict):
            lines += render_yaml_block(child, value, indent + 2)
        else:
            lines.append(f'{pad}  {child}: {render_yaml_scalar(value)}')
    return lines


def insert_before_sections(lines: list[str], block: list[str]) -> list[str]:
    start = next((i for i, line in enumerate(lines)
                  if line.startswith('sections:')), None)
    if start is None:
        raise ValueError('project declares no sections list')
    return lines[:start] + block + [''] + lines[start:]


def set_chroma_error(text: str, chroma_error: dict | None) -> str:
    """Replace, add or (with None) remove the top-level chroma_error block."""
    lines = text.split('\n')
    span = top_level_block_span(lines, 'chroma_error')
    block = ([] if chroma_error is None else
             render_yaml_block('chroma_error', chroma_error))
    if span is not None:
        start, end = span
        if chroma_error is None:
            # Take the separating blank line with the block.
            if end < len(lines) and not lines[end].strip():
                end += 1
        lines[start:end] = block
    elif chroma_error is not None:
        lines = insert_before_sections(lines, block)
    return '\n'.join(lines)


def set_timing_offset(text: str, offset: float) -> str:
    """Set channel.timing_offset_samples, adding the key or block if absent."""
    if isinstance(offset, bool) or not isinstance(offset, (int, float)):
        raise ValueError('timing_offset_samples must be a number')
    value = render_yaml_scalar(float(offset))
    lines = text.split('\n')
    span = top_level_block_span(lines, 'channel')
    if span is None:
        return '\n'.join(insert_before_sections(
            lines, ['channel:', f'  timing_offset_samples: {value}']))

    start, end = span
    if lines[start].split('#')[0].strip() != 'channel:':
        raise ValueError('timing_offset_samples cannot rewrite a flow-style '
                         'channel block; write it in block style')
    for index in range(start + 1, end):
        match = KEY_RE.match(lines[index])
        if match is not None and match.group('key') == 'timing_offset_samples':
            rest = match.group('rest')
            comment = ('  ' + rest[rest.index('#'):].strip()
                       if '#' in rest else '')
            lines[index] = (f"{match.group('lead')}timing_offset_samples: "
                            f'{value}{comment}')
            return '\n'.join(lines)
    lines.insert(start + 1, f'  timing_offset_samples: {value}')
    return '\n'.join(lines)


def derive(text: str, variant: dict) -> str:
    """Apply one variant's transformations to a project's YAML text."""
    blocks = set(variant.get('remove_section_blocks', []))
    if blocks:
        text = remove_section_blocks(text, blocks)

    remove = set(variant.get('remove_sections', []))
    repeat = set(variant.get('repeat_sections', []))
    if remove & repeat:
        raise ValueError('a section cannot be both removed and repeated')
    if remove or repeat:
        text = edit_section_entries(text, remove, repeat)

    if 'capture_seed' in variant:
        text = set_capture_seed(text, int(variant['capture_seed']))

    if 'chroma_error' in variant:
        text = set_chroma_error(text, variant['chroma_error'])

    if 'timing_offset_samples' in variant:
        text = set_timing_offset(text, variant['timing_offset_samples'])

    signal_type = variant.get('signal_type', 'composite')
    if signal_type not in SIGNAL_TYPE_SUFFIX:
        raise ValueError(f"unknown signal_type '{signal_type}'")

    name_suffix = variant.get('name_suffix', '')
    text = retarget_video_path(text, SIGNAL_TYPE_SUFFIX[signal_type],
                               name_suffix)

    if 'signal_type' in variant:
        text = set_signal_type(text, signal_type)

    return text


def expand_captures(variant: dict) -> list[dict]:
    """One derivation per capture entry, or the variant itself when it has
    none. Capture entries inherit the variant's other transformations."""
    captures = variant.get('captures')
    if captures is None:
        return [variant]
    if not captures:
        raise ValueError(f"variant '{variant['name']}' has an empty captures "
                         'list')
    suffixes = [capture.get('name_suffix', '') for capture in captures]
    if any(not suffix for suffix in suffixes):
        raise ValueError(f"variant '{variant['name']}': every capture needs a "
                         'name_suffix')
    if len(set(suffixes)) != len(suffixes):
        raise ValueError(f"variant '{variant['name']}': capture name_suffix "
                         'values must be unique')
    base = {key: value for key, value in variant.items() if key != 'captures'}
    return [{**base, **capture,
             'name_suffix': base.get('name_suffix', '') +
             capture['name_suffix']}
            for capture in captures]


def collect_sources(patterns: list[str], search_roots: list[Path]) -> list[Path]:
    """Resolve globs against each search root in turn, de-duplicated by name."""
    found: dict[str, Path] = {}
    for root in search_roots:
        for pattern in patterns:
            for path in sorted(root.glob(pattern)):
                found.setdefault(str(path), path)
    return [found[key] for key in sorted(found)]


def generate(source_root: Path, output_root: Path, config_path: Path,
             verbose: bool) -> int:
    config = json.loads(config_path.read_text())
    variants = config.get('variants', [])
    if not variants:
        raise ValueError(f'{config_path} declares no variants')

    written = 0
    for variant in variants:
        name = variant['name']
        variant_dir = output_root / name
        variant_dir.mkdir(parents=True, exist_ok=True)

        # Later variants may build on the output of earlier ones.
        sources = collect_sources(variant['sources'],
                                  [source_root, output_root])
        if not sources:
            raise ValueError(f"variant '{name}' matched no source projects")

        expected: set[Path] = set()
        for source in sources:
            for derivation in expand_captures(variant):
                suffix = derivation.get('name_suffix', '')
                destination = variant_dir / f'{source.stem}{suffix}.yaml'
                try:
                    derived = derive(source.read_text(), derivation)
                except ValueError as error:
                    raise ValueError(f'{source}: {error}') from error
                # Only rewrite on change so the build stamp stays meaningful.
                if (not destination.exists() or
                        destination.read_text() != derived):
                    destination.write_text(derived)
                expected.add(destination)
                written += 1

        # Drop derived files whose source has since been renamed or deleted.
        for stale in variant_dir.glob('*.yaml'):
            if stale not in expected:
                stale.unlink()

        if verbose:
            print(f'{name}: {len(sources)} project(s) -> {variant_dir}')

    return written


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True,
                        help='committed project tree (projects/)')
    parser.add_argument('--output', type=Path, required=True,
                        help='directory to write the derived projects into')
    parser.add_argument('--config', type=Path, default=None,
                        help='rules file (default: <source>/variants.json)')
    parser.add_argument('--stamp', type=Path, default=None,
                        help='file to touch on success, for build systems')
    parser.add_argument('--quiet', action='store_true')
    args = parser.parse_args(argv)

    config_path = args.config or (args.source / 'variants.json')
    if not config_path.is_file():
        print(f'Error: rules file not found: {config_path}', file=sys.stderr)
        return 1

    try:
        written = generate(args.source, args.output, config_path,
                           not args.quiet)
    except (ValueError, OSError) as error:
        print(f'Error: {error}', file=sys.stderr)
        return 1

    if not args.quiet:
        print(f'Generated {written} project(s) in {args.output}')
    if args.stamp is not None:
        args.stamp.parent.mkdir(parents=True, exist_ok=True)
        args.stamp.write_text('')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
