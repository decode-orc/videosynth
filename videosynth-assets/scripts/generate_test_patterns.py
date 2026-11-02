#!/usr/bin/env python3
"""Renders the synthetic still test patterns as planar float32 R'G'B' frames.

Each pattern is defined in BT.601 component terms (E'Y, E'R-E'Y, E'B-E'Y) and
converted to the gamma-corrected R'G'B' that videosynth ingests from an EXR
still. The output is raw little-endian float32 in ffmpeg's gbrpf32le plane
order (G, B, R), one file per pattern and raster; generate_test_patterns.sh
wraps the frames as EXR files.

Patterns:
  PAL_STRESS      720x576 only. A PAL decoder-stress card laid out after the
                  Philips PM5544 (F. Hendil, "Combined colour/monochrome
                  pattern generator PM 5544", Philips Electronic Measuring and
                  Microwave Notes 1970/2) and the public-domain (CC0)
                  Wikimedia Commons recreation of it, with no text or marks:
                  black/white border castellations (one carrying
                  line-alternating R-Y on black, the burst-gate check), a
                  white crosshatch on
                  grey, line-alternating +/-(R-Y) and +/-(B-Y) strips (the
                  alternation itself comes from the project's
                  chroma_line_sign regions), steady -/+(R-Y) and -/+(B-Y)
                  bars, G-Y = 0 rectangles at 326 and 146 degrees, and a
                  circle holding a black rectangle on white, white/black
                  steps with 230 ns needle pulses, a 250 kHz square wave,
                  75% colour bars, the centre cross, 0.8-4.8 MHz definition
                  lines, a six-step grey scale and a red square on yellow.
  LUMA_ZONE_PLATE   Static circular luma zone plate reaching the source
                    Nyquist frequency at the raster edges, chroma neutral.
  CHROMA_ZONE_PLATE Static circular zone plate on (B-Y, R-Y) in quadrature at
                    constant luma, reaching the source Nyquist frequency at
                    the raster edges.
  GREEN_MAGENTA_EDGES
                    Row-to-row (horizontal) edges between green and magenta,
                    the case where U and V both change across the edge and 2D
                    PAL decoders leave cross-luminance dots. Three columns
                    carry 100 % bars, 75 % bars (green brighter than magenta
                    in both) and 75 % chroma at equal luma: solid blocks,
                    then stripes 1, 2, 4 and 8 rows tall. Below them, the
                    same three variants as vertical edges (solid columns and
                    1, 2, 4 and 8 pixel stripes); at the foot, 2-row red/cyan
                    and blue/yellow stripes at 100 % and 75 % as controls on
                    other axes.

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

# ITU-R BT.1700 Annex 1 Part B: U = 0.493 (B'-Y'), V = 0.877 (R'-Y').
U_WEIGHT = 0.493
V_WEIGHT = 0.877

# PAL_STRESS is laid out in card units: 768 across the 52 us active line
# (square pixels on a 4:3 picture) by the 576 frame rows. videosynth maps the
# 720 source pixels onto the same 52 us window, so card unit u covers source
# pixels u * 720 / 768.
CARD_WIDTH = 768
CARD_HEIGHT = 576
ACTIVE_LINE_US = 52.0
US_PER_UNIT = ACTIVE_LINE_US / CARD_WIDTH
SUBSAMPLES = 8  # horizontal anti-aliasing per source pixel

# Crosshatch: 18 vertical lines every 42 units, centred on the card, and 14
# horizontal lines of two rows each (one row in each field).
GRID_PITCH = 42
GRID_FIRST_COLUMN = 27.0  # centre of the leftmost vertical line
GRID_COLUMNS = 18
GRID_FIRST_ROW = 14
GRID_ROWS = 14
GRID_LINE_ROWS = 2
# Hendil 1970: 230 ns lines give the fewest components at the subcarrier.
LINE_WIDTH = 0.230 / US_PER_UNIT
NEEDLE_WIDTH = LINE_WIDTH

BACKGROUND_LUMA = 0.48
# Every colour-difference field carries the same subcarrier amplitude, so the
# R-Y and B-Y strips match on a vectorscope.
CHROMA_AMPLITUDE = 0.233

CIRCLE_CENTRE = (CARD_WIDTH / 2.0, CARD_HEIGHT / 2.0)
CIRCLE_RADIUS = 252.0

# Circle interior, top to bottom (Hendil 1970 "Composition of the pattern").
BOX_ROWS = (56, 98)
BOX_X = (300.0, 468.0)  # black rectangle, about 11 us
STEP_TOP_ROWS = (98, 140)  # black/white step with needle pulse
SQUARE_WAVE_ROWS = (140, 182)  # 250 kHz at 75 % contrast
SQUARE_WAVE_PERIOD_US = 4.0
BARS_ROWS = (182, 268)  # 75 % colour bars
CENTRE_BAND_ROWS = (268, 308)  # crossed lines on black
CENTRE_LINE_ROWS = (287, 289)  # one row in each field
GRATING_ROWS = (308, 392)
GREY_SCALE_ROWS = (392, 434)
STEP_BOTTOM_ROWS = (434, 476)  # white/black step with needle pulse
COLOUR_STEP_FIRST_ROW = 476  # red square on yellow to the circle's foot
STEP_X = (258.0, 510.0)
NEEDLE_X = 277.0
CROSS_ROWS = (224, 350)
CROSS_BAR_X = (365.0, 403.0)
BAND_X0 = 132.0  # bars and grey scale: six 84-unit bands centred on the card
BAND_WIDTH = 84.0
GRATING_X = (174.0, 594.0)
GRATING_FREQUENCIES_MHZ = [0.8, 1.8, 2.8, 3.8, 4.8]
RED_SQUARE_X = (364.0, 404.0)  # about 3 us

# Side columns (left side; the right side is the mirror image). The outer
# strip fills grid cell 0 down the whole crosshatch; beside it a block from
# grid line 1 to grid line 3, split at line 2, holds the tall -/+ bar and the
# two G-Y = 0 rectangles.
SIDE_BLOCK_ROWS = (58, 518)
SIDE_SPLIT_ROW = 288
SIDE_SMALL_TOP_ROWS = (58, 140)
SIDE_SMALL_BOTTOM_ROWS = (436, 518)
# Burst-gate check (Hendil 1970): a far left-hand castellation carries
# line-alternating (R-Y) on black, which a mistimed burst gate passes to the
# reference generator. As with the strips, the image holds the + field and the
# project's chroma_line_sign region makes it alternate.
BURST_GATE_CELL_ROW = 2


# GREEN_MAGENTA_EDGES layout, in frame rows and source pixels.
EDGE_BLOCK_ROWS = 32  # solid green, magenta, green blocks
EDGE_STRIPE_BANDS = [(1, 48), (2, 48), (4, 64), (8, 64)]  # (rows, band rows)
EDGE_VERTICAL_BAND_ROWS = 40  # one band per variant
EDGE_VERTICAL_ZONE = 144  # pixels: solid columns, then 1/2/4/8 px stripes
EDGE_VERTICAL_SOLID = 48
EDGE_VERTICAL_STRIPES = [1, 2, 4, 8]
EDGE_CONTROL_STRIPE = 2
# Equal-luma pair: the 75 % green and magenta chroma about this luma. At
# 100 % chroma no equal luma keeps both inside R'G'B' [0, 1].
EDGE_EQUAL_LUMA = 0.5


def rgb_from_components(y, r_minus_y, b_minus_y):
    """E'Y, E'R-E'Y, E'B-E'Y -> (R', G', B')."""
    r = y + r_minus_y
    b = y + b_minus_y
    g = y - (KR * r_minus_y + KB * b_minus_y) / KG
    return (r, g, b)


def chroma_at(angle_deg):
    """(R-Y, B-Y) of a CHROMA_AMPLITUDE vector at angle_deg from +U."""
    angle = math.radians(angle_deg)
    return (CHROMA_AMPLITUDE * math.sin(angle) / V_WEIGHT,
            CHROMA_AMPLITUDE * math.cos(angle) / U_WEIGHT)


def background_colour(r_minus_y, b_minus_y):
    return rgb_from_components(BACKGROUND_LUMA, r_minus_y, b_minus_y)


def g_minus_y_zero_angle():
    """The U/V angle (degrees, 0-180) at which G-Y vanishes:
    KR (R-Y) + KB (B-Y) = 0."""
    return math.degrees(math.atan2(-V_WEIGHT * KB / KR, U_WEIGHT)) % 180.0


GREY = background_colour(0.0, 0.0)
WHITE = (1.0, 1.0, 1.0)
BLACK = (0.0, 0.0, 0.0)
PLUS_R = background_colour(*chroma_at(90.0))
MINUS_R = background_colour(*chroma_at(270.0))
PLUS_B = background_colour(*chroma_at(0.0))
MINUS_B = background_colour(*chroma_at(180.0))
G_ZERO_LOW = background_colour(*chroma_at(g_minus_y_zero_angle()))  # ~146
G_ZERO_HIGH = background_colour(
    *chroma_at(g_minus_y_zero_angle() + 180.0))  # ~326
# On black, so G' goes below zero; EXR float carries it and the source
# conversion keeps the sub-black excursion.
BURST_GATE = rgb_from_components(0.0, *chroma_at(90.0))
BAR_COLOURS = [
    (0.75, 0.75, 0.0),  # yellow
    (0.0, 0.75, 0.75),  # cyan
    (0.0, 0.75, 0.0),  # green
    (0.75, 0.0, 0.75),  # magenta
    (0.75, 0.0, 0.0),  # red
    (0.0, 0.0, 0.75),  # blue
]


def grid_column_centre(index):
    return GRID_FIRST_COLUMN + index * GRID_PITCH


def on_vertical_line(u):
    index = round((u - GRID_FIRST_COLUMN) / GRID_PITCH)
    if not 0 <= index < GRID_COLUMNS:
        return False
    return abs(u - grid_column_centre(index)) < LINE_WIDTH / 2.0


def on_horizontal_line(y):
    offset = y - GRID_FIRST_ROW
    return (0 <= offset < GRID_PITCH * (GRID_ROWS - 1) + GRID_LINE_ROWS
            and offset % GRID_PITCH < GRID_LINE_ROWS)


def grid_cell(u):
    """Index of the grid cell (between vertical lines index and index+1)."""
    return math.floor((u - GRID_FIRST_COLUMN) / GRID_PITCH)


def grid_cell_row(y):
    return (y - GRID_FIRST_ROW) // GRID_PITCH


def in_circle(u, y):
    du = u - CIRCLE_CENTRE[0]
    dy = y + 0.5 - CIRCLE_CENTRE[1]
    return du * du + dy * dy <= CIRCLE_RADIUS * CIRCLE_RADIUS


def within(value, span):
    return span[0] <= value < span[1]


def step_band(u, inside, outside):
    """The black/white step bands: `inside` between the steps with a
    needle pulse of `outside` just after the first step."""
    if within(u, STEP_X):
        if within(u, (NEEDLE_X, NEEDLE_X + NEEDLE_WIDTH)):
            return outside
        return inside
    return outside


def band_index(u):
    return min(5, max(0, math.floor((u - BAND_X0) / BAND_WIDTH)))


def grating(u_centre):
    """Sinusoidal definition lines, evaluated at the pixel centre so the
    anti-aliasing does not attenuate them."""
    width = (GRATING_X[1] - GRATING_X[0]) / len(GRATING_FREQUENCIES_MHZ)
    index = min(len(GRATING_FREQUENCIES_MHZ) - 1,
                int((u_centre - GRATING_X[0]) // width))
    start = GRATING_X[0] + index * width
    t_us = (u_centre - start) * US_PER_UNIT
    level = 0.5 + 0.5 * math.sin(
        2.0 * math.pi * GRATING_FREQUENCIES_MHZ[index] * t_us)
    return (level, level, level)


def circle_interior(u, y, u_centre):
    on_cross = within(y, CROSS_ROWS) and within(u, CROSS_BAR_X)
    if on_cross:
        on_centre_line = (within(y, CENTRE_LINE_ROWS)
                          or abs(u - CIRCLE_CENTRE[0]) < LINE_WIDTH / 2.0)
        return WHITE if on_centre_line else BLACK
    if y < BOX_ROWS[0]:
        return WHITE
    if y < BOX_ROWS[1]:
        return BLACK if within(u, BOX_X) else WHITE
    if y < STEP_TOP_ROWS[1]:
        return step_band(u, WHITE, BLACK)
    if y < SQUARE_WAVE_ROWS[1]:
        half_period = SQUARE_WAVE_PERIOD_US / 2.0 / US_PER_UNIT
        phase = math.floor((u - CIRCLE_CENTRE[0]) / half_period)
        return BLACK if phase % 2 == 0 else (0.75, 0.75, 0.75)
    if y < BARS_ROWS[1]:
        return BAR_COLOURS[band_index(u)]
    if y < CENTRE_BAND_ROWS[1]:
        if within(y, CENTRE_LINE_ROWS) or on_vertical_line(u):
            return WHITE
        return BLACK
    if y < GRATING_ROWS[1]:
        return grating(u_centre) if within(u, GRATING_X) else BLACK
    if y < GREY_SCALE_ROWS[1]:
        level = band_index(u) / 5.0
        return (level, level, level)
    if y < STEP_BOTTOM_ROWS[1]:
        return step_band(u, BLACK, WHITE)
    return BAR_COLOURS[4] if within(u, RED_SQUARE_X) else BAR_COLOURS[0]


def castellation(u, y):
    """Black/white blocks round the border, alternating cell by cell."""
    if y < GRID_FIRST_ROW or y >= CARD_HEIGHT - GRID_FIRST_ROW:
        cell = grid_cell(u)
        black = (0 <= cell < GRID_COLUMNS - 1 and cell % 2 == 0
                 and not on_vertical_line(u))
        return BLACK if black else WHITE
    if on_horizontal_line(y):
        return WHITE
    row = grid_cell_row(y)
    if row == BURST_GATE_CELL_ROW and u < CARD_WIDTH / 2.0:
        return BURST_GATE
    return BLACK if row % 2 == 0 else WHITE


def side_column(u, y, r_axis):
    """Strip and block content for one side; u is measured from that side's
    picture edge. Returns None where the crosshatch shows through."""
    plus, minus = (PLUS_R, MINUS_R) if r_axis else (PLUS_B, MINUS_B)
    if grid_cell(u) == 0 and not on_vertical_line(u):
        # Line-alternating strip: drawn as the + field; the project's
        # chroma_line_sign region makes it alternate with the V-switch.
        return WHITE if on_horizontal_line(y) else plus
    block_x = (grid_column_centre(1) + LINE_WIDTH / 2.0,
               grid_column_centre(3) - LINE_WIDTH / 2.0)
    if within(u, block_x) and within(y, SIDE_BLOCK_ROWS):
        small_top = within(y, SIDE_SMALL_TOP_ROWS)
        small_bottom = within(y, SIDE_SMALL_BOTTOM_ROWS)
        # Grid line 2 shows beside the tall bar except where a rectangle
        # abuts it.
        if not (small_top or small_bottom) and on_vertical_line(u):
            return None
        if u < grid_column_centre(2):
            return minus if y < SIDE_SPLIT_ROW else plus
        if small_top:
            return G_ZERO_HIGH
        if small_bottom:
            return G_ZERO_LOW
    return None


def pal_stress_card(u, y, u_centre):
    """PAL_STRESS colour at card position u (units) on frame row y."""
    left_edge = GRID_FIRST_COLUMN - LINE_WIDTH / 2.0
    right_edge = CARD_WIDTH - left_edge
    if (y < GRID_FIRST_ROW or y >= CARD_HEIGHT - GRID_FIRST_ROW
            or u < left_edge or u >= right_edge):
        return castellation(u, y)
    if in_circle(u, y):
        return circle_interior(u, y, u_centre)
    side = None
    if u < CARD_WIDTH / 2.0:
        side = side_column(u, y, r_axis=True)
    else:
        side = side_column(CARD_WIDTH - u, y, r_axis=False)
    if side is not None:
        return side
    if on_vertical_line(u) or on_horizontal_line(y):
        return WHITE
    return GREY


def pal_stress(x, y, width, height):
    """Box-filters the card over the source pixel's span of card units."""
    scale = CARD_WIDTH / width
    u_centre = (x + 0.5) * scale
    total = [0.0, 0.0, 0.0]
    for k in range(SUBSAMPLES):
        u = (x + (k + 0.5) / SUBSAMPLES) * scale
        colour = pal_stress_card(u, y, u_centre)
        for c in range(3):
            total[c] += colour[c]
    return tuple(value / SUBSAMPLES for value in total)


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


def bar_pair(level, first, second):
    """Two bar colours at `level` from 0/1 R'G'B' masks."""
    return (tuple(level * c for c in first), tuple(level * c for c in second))


GREEN = (0.0, 1.0, 0.0)
MAGENTA = (1.0, 0.0, 1.0)
GREEN_75_CHROMA = (-0.75 * KG, -0.75 * KG)  # (R-Y, B-Y) of 75 % green
EDGE_VARIANTS = [
    bar_pair(1.0, GREEN, MAGENTA),
    bar_pair(0.75, GREEN, MAGENTA),
    (rgb_from_components(EDGE_EQUAL_LUMA, *GREEN_75_CHROMA),
     rgb_from_components(EDGE_EQUAL_LUMA, *(-c for c in GREEN_75_CHROMA))),
]
EDGE_CONTROLS = [
    ((1.0, 0.0, 0.0), (0.0, 1.0, 1.0)),  # red / cyan
    ((0.0, 0.0, 1.0), (1.0, 1.0, 0.0)),  # blue / yellow
]


def horizontal_edges(y, pair):
    """Solid blocks then row stripes; None below the horizontal section."""
    first, second = pair
    if y < 3 * EDGE_BLOCK_ROWS:
        return second if (y // EDGE_BLOCK_ROWS) % 2 else first
    top = 3 * EDGE_BLOCK_ROWS
    for stripe, rows in EDGE_STRIPE_BANDS:
        if y < top + rows:
            return second if ((y - top) // stripe) % 2 else first
        top += rows
    return None


def vertical_edges(x, pair):
    first, second = pair
    zone = x // EDGE_VERTICAL_ZONE
    offset = x % EDGE_VERTICAL_ZONE
    stripe = (EDGE_VERTICAL_SOLID if zone == 0
              else EDGE_VERTICAL_STRIPES[min(zone - 1, 3)])
    return second if (offset // stripe) % 2 else first


def green_magenta_edges(x, y, width, height):
    horizontal_rows = 3 * EDGE_BLOCK_ROWS + sum(
        rows for _, rows in EDGE_STRIPE_BANDS)
    if y < horizontal_rows:
        column = min(x * len(EDGE_VARIANTS) // width, len(EDGE_VARIANTS) - 1)
        return horizontal_edges(y, EDGE_VARIANTS[column])
    y -= horizontal_rows
    vertical_rows = EDGE_VERTICAL_BAND_ROWS * len(EDGE_VARIANTS)
    if y < vertical_rows:
        return vertical_edges(x, EDGE_VARIANTS[y // EDGE_VERTICAL_BAND_ROWS])
    # Controls: red/cyan left, blue/yellow right; 100 % above 75 %.
    y -= vertical_rows
    level = 1.0 if y < (height - horizontal_rows - vertical_rows) // 2 \
        else 0.75
    first, second = EDGE_CONTROLS[0 if x < width // 2 else 1]
    first, second = bar_pair(level, first, second)
    return second if (y // EDGE_CONTROL_STRIPE) % 2 else first


PATTERNS = [
    ("PAL_STRESS", pal_stress, [(720, 576)]),
    ("LUMA_ZONE_PLATE", luma_zone_plate, [(720, 576), (720, 486)]),
    ("CHROMA_ZONE_PLATE", chroma_zone_plate, [(720, 576), (720, 486)]),
    ("GREEN_MAGENTA_EDGES", green_magenta_edges, [(720, 576), (720, 486)]),
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
