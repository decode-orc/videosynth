#!/usr/bin/env python3
"""Renders the synthetic still test patterns as planar float32 R'G'B' frames.

Each pattern is defined in BT.601 component terms (E'Y, E'R-E'Y, E'B-E'Y) and
converted to the gamma-corrected R'G'B' that videosynth ingests from an EXR
still. The output is raw little-endian float32 in ffmpeg's gbrpf32le plane
order (G, B, R), one file per pattern and raster; generate_test_patterns.sh
wraps the frames as EXR files.

Patterns:
  PAL_STRESS      720x576 only. A PAL decoder-stress card built from published
                  element parameters: line-alternating +/-(R-Y) and +/-(B-Y)
                  bars (the alternation itself comes from the project's
                  chroma_line_sign regions), steady +/-(R-Y), +/-(B-Y) and
                  +/-(G-Y) fields, 0.8-4.8 MHz definition gratings, a 75%
                  red square on 75% yellow, 75% colour bars, a grey scale and
                  a 3-pixel (about 230 ns) white grid.
  LUMA_ZONE_PLATE   Static circular luma zone plate reaching the source
                    Nyquist frequency at the raster edges, chroma neutral.
  CHROMA_ZONE_PLATE Static circular zone plate on (B-Y, R-Y) in quadrature at
                    constant luma, reaching the source Nyquist frequency at
                    the raster edges.

Usage: generate_test_patterns.py <out_dir>
Writes <out_dir>/<NAME>_<width>x<height>.gbrpf32le for every pattern/raster.
"""

import math
import struct
import sys

# ITU-R BT.601-5 Part A section 3.5.1: luma weights.
KR = 0.299
KG = 0.587
KB = 0.114

# BT.601 13.5 MHz luma sampling rate: source pixels per microsecond.
PIXEL_RATE_MHZ = 13.5

# PAL_STRESS geometry, in source pixels (x) and raster rows (y). The bar
# positions are mirrored in docs and in projects/general/pal_stress.yaml,
# whose chroma_line_sign regions must cover the alternating bars exactly.
GRID_PITCH = 48
GRID_LINE_WIDTH = 3  # about 230 ns at 13.5 MHz
GRID_LINE_ROWS = 2  # one row in each field
LEFT_PANEL_X = (48, 144)
RIGHT_PANEL_X = (576, 672)
# (y0, y1, kind) for the side panels; "alt" is the line-alternating bar.
PANEL_ROWS = [
    (96, 240, "alt"),
    (240, 288, "plus"),
    (288, 336, "minus"),
    (336, 384, "g_plus"),
    (384, 432, "g_minus"),
]
CIRCLE_CENTRE = (360.0, 288.0)
CIRCLE_RADIUS_ROWS = 216.0
# ITU-R BT.601-5 Part A: 625-line pixel aspect ratio 128/117.
PAL_PIXEL_ASPECT = 128.0 / 117.0
INNER_X = (200, 520)
GRATING_FREQUENCIES_MHZ = [0.8, 1.8, 2.8, 3.8, 4.8]

# Colour-difference amplitudes of the side-panel fields, on a 50% grey.
PANEL_LUMA = 0.5
PANEL_COLOUR_DIFFERENCE = 0.25
# R-Y = B-Y = -a gives G-Y = +a (KR + KB) / KG; choose a for |G-Y| = 0.25.
PANEL_G_DRIVE = PANEL_COLOUR_DIFFERENCE * KG / (KR + KB)


def rgb_from_components(y, r_minus_y, b_minus_y):
    """E'Y, E'R-E'Y, E'B-E'Y -> (R', G', B')."""
    r = y + r_minus_y
    b = y + b_minus_y
    g = y - (KR * r_minus_y + KB * b_minus_y) / KG
    return (r, g, b)


GREY = rgb_from_components(PANEL_LUMA, 0.0, 0.0)
WHITE = (1.0, 1.0, 1.0)
BLACK = (0.0, 0.0, 0.0)


def bars_75(position):
    """75% colour bars (white, yellow, cyan, green, magenta, red, blue,
    black) across position in [0, 1)."""
    index = min(7, int(position * 8.0))
    r = 0.75 if index in (0, 1, 4, 5) else 0.0
    g = 0.75 if index in (0, 1, 2, 3) else 0.0
    b = 0.75 if index in (0, 2, 4, 6) else 0.0
    return (r, g, b)


def panel_colour(kind, axis):
    """Side-panel field colour; axis is 'r' (left) or 'b' (right)."""
    a = PANEL_COLOUR_DIFFERENCE
    if kind in ("alt", "plus"):
        d = a
    elif kind == "minus":
        d = -a
    elif kind == "g_plus":
        return rgb_from_components(PANEL_LUMA, -PANEL_G_DRIVE, -PANEL_G_DRIVE)
    else:  # g_minus
        return rgb_from_components(PANEL_LUMA, PANEL_G_DRIVE, PANEL_G_DRIVE)
    if axis == "r":
        return rgb_from_components(PANEL_LUMA, d, 0.0)
    return rgb_from_components(PANEL_LUMA, 0.0, d)


def in_circle(x, y):
    dx = (x + 0.5 - CIRCLE_CENTRE[0]) * PAL_PIXEL_ASPECT
    dy = y + 0.5 - CIRCLE_CENTRE[1]
    return dx * dx + dy * dy <= CIRCLE_RADIUS_ROWS * CIRCLE_RADIUS_ROWS


def on_grid(x, y):
    half = GRID_LINE_WIDTH // 2
    on_column = (x + half) % GRID_PITCH < GRID_LINE_WIDTH and x > half
    on_row = (y + 1) % GRID_PITCH < GRID_LINE_ROWS and y > 1
    return on_column or on_row


def circle_interior(x, y):
    """The picture inside the circle, by horizontal band."""
    x0, x1 = INNER_X
    inside = x0 <= x < x1
    if 96 <= y < 144:
        return bars_75((x - x0) / (x1 - x0)) if inside else GREY
    if 144 <= y < 192:
        if not inside:
            return GREY
        level = min(5, int((x - x0) * 6 / (x1 - x0))) / 5.0
        return (level, level, level)
    if 192 <= y < 240:
        return WHITE if 300 <= x < 420 else BLACK
    if 240 <= y < 336:
        if not inside:
            return GREY
        width = (x1 - x0) // len(GRATING_FREQUENCIES_MHZ)
        frequency = GRATING_FREQUENCIES_MHZ[min(
            len(GRATING_FREQUENCIES_MHZ) - 1, (x - x0) // width)]
        cycles_per_pixel = frequency / PIXEL_RATE_MHZ
        level = 0.5 + 0.4 * math.sin(
            2.0 * math.pi * cycles_per_pixel * (x - x0 + 0.5))
        return (level, level, level)
    if 336 <= y < 384:
        if not inside:
            return GREY
        # 75% amplitude: 100% yellow plus its chroma overshoots the 10-bit
        # composite code range.
        if 336 <= x < 384:
            return (0.75, 0.0, 0.0)
        return (0.75, 0.75, 0.0)
    if 384 <= y < 432:
        if not inside:
            return GREY
        return WHITE if x < 360 else BLACK
    return GREY


def pal_stress(x, y, width, height):
    if in_circle(x, y):
        return circle_interior(x, y)
    for x0, x1, axis in ((*LEFT_PANEL_X, "r"), (*RIGHT_PANEL_X, "b")):
        if x0 <= x < x1:
            for y0, y1, kind in PANEL_ROWS:
                if y0 <= y < y1:
                    return panel_colour(kind, axis)
    return WHITE if on_grid(x, y) else GREY


def zone_phase(x, y, width, height):
    """Circular zone plate phase: the local frequency rises linearly from
    zero at the centre to 0.5 cycles/pixel (and 0.5 cycles/row) at the
    raster edges."""
    dx = x + 0.5 - width / 2.0
    dy = y + 0.5 - height / 2.0
    return math.pi * (dx * dx / width + dy * dy / height)


def luma_zone_plate(x, y, width, height):
    level = 0.5 + 0.4 * math.cos(zone_phase(x, y, width, height))
    return (level, level, level)


def chroma_zone_plate(x, y, width, height):
    phase = zone_phase(x, y, width, height)
    return rgb_from_components(0.5, 0.2 * math.sin(phase),
                               0.2 * math.cos(phase))


PATTERNS = [
    ("PAL_STRESS", pal_stress, [(720, 576)]),
    ("LUMA_ZONE_PLATE", luma_zone_plate, [(720, 576), (720, 486)]),
    ("CHROMA_ZONE_PLATE", chroma_zone_plate, [(720, 576), (720, 486)]),
]


def render(function, width, height):
    planes = {"r": [], "g": [], "b": []}
    for y in range(height):
        for x in range(width):
            r, g, b = function(x, y, width, height)
            planes["r"].append(r)
            planes["g"].append(g)
            planes["b"].append(b)
    count = width * height
    return b"".join(
        struct.pack("<%df" % count, *planes[name]) for name in ("g", "b", "r"))


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out_dir = sys.argv[1]
    for name, function, rasters in PATTERNS:
        for width, height in rasters:
            path = "%s/%s_%dx%d.gbrpf32le" % (out_dir, name, width, height)
            with open(path, "wb") as out:
                out.write(render(function, width, height))
            print(path)


if __name__ == "__main__":
    main()
