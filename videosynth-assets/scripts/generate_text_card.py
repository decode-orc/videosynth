#!/usr/bin/env python3
"""Renders TEXT_CARD: hard-edged coloured text as an overlay generator draws it.

The Domesday discs key RGB text and graphics from the BBC Micro over their
pictures, so a decoder sees saturated, pixel-aligned text on flat colour and
on picture. This card carries that text in three sizes, glyphs 8, 16 and 24
rows tall (the 8-row font drawn at 1x, 2x and 3x, pixel-replicated with no
anti-aliasing), in the colour pairs:

  white, yellow and cyan on blue;  black on white;  red and green on black;
  yellow on red;  white and magenta on green.

Each background is a band across the card, split into one column per
foreground colour, with one line of text per size. Below the bands, the
same text in several colours and sizes is keyed over a strip of the
PHOTO_GALTSTROMS photograph. Text and backgrounds are full-level R'G'B'
primaries and secondaries, as the BBC Micro's RGB output gives.

The font is drawn for this card (5x7 glyphs in a 6x8 cell, descenders in the
eighth row) and is part of this script; it carries the script's licence.

Usage: generate_text_card.py <PHOTO_GALTSTROMS_720x576.gbrpf32le> <out>
Both files are raw float32 720x576 in ffmpeg's gbrpf32le plane order
(G, B, R).
"""

import array
import sys

WIDTH = 720
HEIGHT = 576

CELL_WIDTH = 6
CELL_HEIGHT = 8
SCALES = (1, 2, 3)  # glyphs 8, 16 and 24 rows tall
LEFT_MARGIN = 6
LINE_GAP = 4  # rows above each line and below the last
BAND_ROWS = 64  # 4 + 8 + 4 + 16 + 4 + 24 + 4
PHOTO_TOP = 5 * BAND_ROWS
# The photograph strip shows these source rows (the ironworks buildings).
PHOTO_SOURCE_TOP = 200

BLACK = (0.0, 0.0, 0.0)
WHITE = (1.0, 1.0, 1.0)
RED = (1.0, 0.0, 0.0)
GREEN = (0.0, 1.0, 0.0)
BLUE = (0.0, 0.0, 1.0)
YELLOW = (1.0, 1.0, 0.0)
CYAN = (0.0, 1.0, 1.0)
MAGENTA = (1.0, 0.0, 1.0)

# (background, foregrounds), top to bottom.
BANDS = [
    (BLUE, [WHITE, YELLOW, CYAN]),
    (WHITE, [BLACK]),
    (BLACK, [RED, GREEN]),
    (RED, [YELLOW]),
    (GREEN, [WHITE, MAGENTA]),
]

# (scale, colour) for the lines keyed over the photograph, top to bottom.
PHOTO_LINES = [
    (3, WHITE), (2, YELLOW), (1, CYAN), (3, BLACK), (2, RED), (1, WHITE),
    (3, YELLOW), (2, CYAN), (1, MAGENTA), (2, GREEN),
]

SAMPLE_TEXT = ("Domesday 1986: Bradford-on-Avon, Wiltshire. The quick brown "
               "fox jumps over the lazy dog (A&B) 0123456789 ?!% / "
               "Galtstrom Bruk, Sweden. ")

# 5x7 glyphs, '#' set; rows 0-6 hold capitals and the baseline is row 6,
# row 7 holds descenders. Missing rows are blank.
FONT = {
    " ": [],
    "A": [".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"],
    "B": ["####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."],
    "C": [".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."],
    "D": ["####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####."],
    "E": ["#####", "#....", "#....", "####.", "#....", "#....", "#####"],
    "F": ["#####", "#....", "#....", "####.", "#....", "#....", "#...."],
    "G": [".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".####"],
    "H": ["#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"],
    "I": [".###.", "..#..", "..#..", "..#..", "..#..", "..#..", ".###."],
    "J": ["..###", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##.."],
    "K": ["#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"],
    "L": ["#....", "#....", "#....", "#....", "#....", "#....", "#####"],
    "M": ["#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#"],
    "N": ["#...#", "#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#"],
    "O": [".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."],
    "P": ["####.", "#...#", "#...#", "####.", "#....", "#....", "#...."],
    "Q": [".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#"],
    "R": ["####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"],
    "S": [".####", "#....", "#....", ".###.", "....#", "....#", "####."],
    "T": ["#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."],
    "U": ["#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."],
    "V": ["#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."],
    "W": ["#...#", "#...#", "#...#", "#.#.#", "#.#.#", "#.#.#", ".#.#."],
    "X": ["#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#"],
    "Y": ["#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."],
    "Z": ["#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"],
    "a": [".....", ".....", ".###.", "....#", ".####", "#...#", ".####"],
    "b": ["#....", "#....", "#.##.", "##..#", "#...#", "#...#", "####."],
    "c": [".....", ".....", ".###.", "#....", "#....", "#...#", ".###."],
    "d": ["....#", "....#", ".##.#", "#..##", "#...#", "#...#", ".####"],
    "e": [".....", ".....", ".###.", "#...#", "#####", "#....", ".###."],
    "f": ["..##.", ".#..#", ".#...", "###..", ".#...", ".#...", ".#..."],
    "g": [".....", ".....", ".####", "#...#", "#...#", ".####", "....#",
          ".###."],
    "h": ["#....", "#....", "#.##.", "##..#", "#...#", "#...#", "#...#"],
    "i": ["..#..", ".....", ".##..", "..#..", "..#..", "..#..", ".###."],
    "j": ["...#.", ".....", "..##.", "...#.", "...#.", "...#.", "#..#.",
          ".##.."],
    "k": ["#....", "#....", "#..#.", "#.#..", "##...", "#.#..", "#..#."],
    "l": [".##..", "..#..", "..#..", "..#..", "..#..", "..#..", ".###."],
    "m": [".....", ".....", "##.#.", "#.#.#", "#.#.#", "#...#", "#...#"],
    "n": [".....", ".....", "#.##.", "##..#", "#...#", "#...#", "#...#"],
    "o": [".....", ".....", ".###.", "#...#", "#...#", "#...#", ".###."],
    "p": [".....", ".....", "####.", "#...#", "#...#", "####.", "#....",
          "#...."],
    "q": [".....", ".....", ".####", "#...#", "#...#", ".####", "....#",
          "....#"],
    "r": [".....", ".....", "#.##.", "##..#", "#....", "#....", "#...."],
    "s": [".....", ".....", ".####", "#....", ".###.", "....#", "####."],
    "t": [".#...", ".#...", "###..", ".#...", ".#...", ".#..#", "..##."],
    "u": [".....", ".....", "#...#", "#...#", "#...#", "#..##", ".##.#"],
    "v": [".....", ".....", "#...#", "#...#", "#...#", ".#.#.", "..#.."],
    "w": [".....", ".....", "#...#", "#...#", "#.#.#", "#.#.#", ".#.#."],
    "x": [".....", ".....", "#...#", ".#.#.", "..#..", ".#.#.", "#...#"],
    "y": [".....", ".....", "#...#", "#...#", "#...#", ".####", "....#",
          ".###."],
    "z": [".....", ".....", "#####", "...#.", "..#..", ".#...", "#####"],
    "0": [".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###."],
    "1": ["..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."],
    "2": [".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"],
    "3": ["#####", "...#.", "..#..", "...#.", "....#", "#...#", ".###."],
    "4": ["...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."],
    "5": ["#####", "#....", "####.", "....#", "....#", "#...#", ".###."],
    "6": ["..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###."],
    "7": ["#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..."],
    "8": [".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."],
    "9": [".###.", "#...#", "#...#", ".####", "....#", "...#.", ".##.."],
    ".": [".....", ".....", ".....", ".....", ".....", ".##..", ".##.."],
    ",": [".....", ".....", ".....", ".....", ".....", ".##..", ".##..",
          ".#..."],
    ":": [".....", ".##..", ".##..", ".....", ".##..", ".##..", "....."],
    "-": [".....", ".....", ".....", ".###.", ".....", ".....", "....."],
    "'": ["..#..", "..#..", ".#..."],
    "(": ["...#.", "..#..", ".#...", ".#...", ".#...", "..#..", "...#."],
    ")": [".#...", "..#..", "...#.", "...#.", "...#.", "..#..", ".#..."],
    "/": [".....", "....#", "...#.", "..#..", ".#...", "#....", "....."],
    "&": [".##..", "#..#.", "#.#..", ".#...", "#.#.#", "#..#.", ".##.#"],
    "!": ["..#..", "..#..", "..#..", "..#..", "..#..", ".....", "..#.."],
    "?": [".###.", "#...#", "....#", "...#.", "..#..", ".....", "..#.."],
    "%": ["##...", "##..#", "...#.", "..#..", ".#...", "#..##", "...##"],
}


def glyph_set(char, row, column):
    """True where the glyph for char sets cell pixel (row, column)."""
    rows = FONT[char]
    return (row < len(rows) and column < len(rows[row])
            and rows[row][column] == "#")


def draw_text(frame, text, left, top, scale, colour, right):
    """Draws whole characters from text, starting at (left, top), until the
    next would cross right; returns the number drawn."""
    advance = CELL_WIDTH * scale
    count = 0
    for char in text:
        if left + advance > right:
            break
        for row in range(CELL_HEIGHT):
            for column in range(CELL_WIDTH):
                if not glyph_set(char, row, column):
                    continue
                for dy in range(scale):
                    y = top + row * scale + dy
                    start = (y * WIDTH + left + column * scale)
                    for dx in range(scale):
                        frame[start + dx] = colour
        left += advance
        count += 1
    return count


def next_word(offset):
    """Offset of the first word starting at or after offset in SAMPLE_TEXT,
    so each line starts on a word."""
    offset %= len(SAMPLE_TEXT)
    if offset == 0 or SAMPLE_TEXT[offset - 1] == " ":
        return offset
    return (SAMPLE_TEXT.find(" ", offset) + 1) % len(SAMPLE_TEXT)


def line_tops(top):
    """Top row of each size's line within a band starting at top."""
    tops = []
    y = top + LINE_GAP
    for scale in SCALES:
        tops.append(y)
        y += CELL_HEIGHT * scale + LINE_GAP
    return tops


def read_photo(path):
    planes = array.array("f")
    with open(path, "rb") as raw:
        planes.frombytes(raw.read())
    if sys.byteorder != "little":
        planes.byteswap()
    count = WIDTH * HEIGHT
    g, b, r = (planes[i * count:(i + 1) * count] for i in range(3))
    return [(r[i], g[i], b[i]) for i in range(count)]


def render(photo):
    frame = [BLACK] * (WIDTH * HEIGHT)
    offset = 0
    for band, (background, foregrounds) in enumerate(BANDS):
        top = band * BAND_ROWS
        for i in range(top * WIDTH, (top + BAND_ROWS) * WIDTH):
            frame[i] = background
        column_width = WIDTH // len(foregrounds)
        for index, colour in enumerate(foregrounds):
            left = index * column_width
            for scale, y in zip(SCALES, line_tops(top)):
                text = SAMPLE_TEXT[offset:] + SAMPLE_TEXT
                offset = next_word(offset + draw_text(
                    frame, text, left + LEFT_MARGIN, y, scale, colour,
                    left + column_width))

    source = PHOTO_SOURCE_TOP * WIDTH
    frame[PHOTO_TOP * WIDTH:] = photo[source:source
                                      + (HEIGHT - PHOTO_TOP) * WIDTH]
    y = PHOTO_TOP + LINE_GAP
    for scale, colour in PHOTO_LINES:
        if y + CELL_HEIGHT * scale > HEIGHT:
            break
        text = SAMPLE_TEXT[offset:] + SAMPLE_TEXT
        offset = next_word(offset + draw_text(frame, text, LEFT_MARGIN, y,
                                              scale, colour, WIDTH))
        y += CELL_HEIGHT * scale + LINE_GAP
    return frame


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    frame = render(read_photo(sys.argv[1]))
    out = array.array("f")
    for channel in (1, 2, 0):  # gbrpf32le plane order
        out.extend(pixel[channel] for pixel in frame)
    if sys.byteorder != "little":
        out.byteswap()
    with open(sys.argv[2], "wb") as raw:
        raw.write(out.tobytes())


if __name__ == "__main__":
    main()
