#!/usr/bin/env python3
"""Renders OpenStreetMap data as 1980s Ordnance Survey style map stills.

The maps stand in for the Ordnance Survey maps that dominate the Domesday
discs: large flat fills, hard edges at every angle, single-pixel lines,
hatching and dot fills, and dense small lettering in several colours. They
are drawn from the committed extract sources/osm/bradford_on_avon.json
(fetched by fetch_osm_extract.sh) in an OS-like style, not the OSM web style.

Every colour is an R'G'B' triple inside [0, 1], so the picture stays inside
the legal PAL gamut; the road fills, water lines and water lettering sit at
the gamut edge, the area fills are pale.

Rendering is aliased (hard-edged) on a palette canvas SUPERSAMPLE times the
720x576 raster in each direction; each output pixel is the mean of its
SUPERSAMPLE x SUPERSAMPLE block, taken in float R'G'B'. Lines and lettering
therefore land at sub-pixel positions without aliasing, and no edge is
softened beyond that box reduction.

OSM has no heights, so the contours are drawn from an analytic height field:
the ground rises away from the rivers and canal (distance from the nearest
waterway) with a few smooth hills added, so the contours follow the valley.
They are illustrative, not surveyed.

Maps:
  OSM_TOWN   Bradford-on-Avon town centre, 800 m across: buildings, streets
             and paths, street names, 5 m contours, a 100 m grid.
  OSM_RURAL  The town and its countryside, 6 km across: land cover fills,
             main roads, rivers, place names, 10 m contours, a 1 km grid.

Usage: render_osm_map.py <extract.json> <font_dir> <out_dir>
font_dir holds the Liberation fonts (SIL Open Font License 1.1). Writes
<out_dir>/<NAME>_720x576.gbrpf32le for each map, planar float32 in ffmpeg's
gbrpf32le plane order (G, B, R).
"""

import json
import math
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

WIDTH = 720
HEIGHT = 576
SUPERSAMPLE = 4
CANVAS_W = WIDTH * SUPERSAMPLE
CANVAS_H = HEIGHT * SUPERSAMPLE
# videosynth maps the 720 source pixels onto the 52 us active line, a 4:3
# picture, so the ground covered is 4 wide by 3 high.
PICTURE_ASPECT = 4.0 / 3.0
EARTH_METRES_PER_DEGREE = 111320.0
# The height field and the grid are anchored here (lon, lat), so the two maps
# share hills and grid lines.
TERRAIN_ORIGIN = (-2.2560, 51.3440)

ATTRIBUTION = "© OpenStreetMap contributors"

# Palette: index -> R'G'B'. The canvas holds indices so the box reduction
# averages exact float colours.
PALETTE_ENTRIES = [
    ("paper", (0.96, 0.95, 0.88)),
    ("farmland", (0.96, 0.93, 0.76)),
    ("grass", (0.84, 0.93, 0.68)),
    ("built_up", (0.93, 0.83, 0.76)),
    ("industrial", (0.88, 0.80, 0.87)),
    ("cemetery", (0.78, 0.88, 0.76)),
    ("allotments", (0.92, 0.88, 0.70)),
    ("wood", (0.60, 0.84, 0.42)),
    ("wood_symbol", (0.0, 0.42, 0.08)),
    ("scrub", (0.80, 0.90, 0.60)),
    ("water", (0.55, 0.80, 0.97)),
    ("water_line", (0.0, 0.35, 0.92)),
    ("water_text", (0.0, 0.18, 0.88)),
    ("contour", (0.74, 0.42, 0.10)),
    ("contour_index", (0.55, 0.28, 0.04)),
    ("grid", (0.12, 0.42, 0.96)),
    ("casing", (0.08, 0.06, 0.05)),
    ("road_primary", (0.94, 0.10, 0.08)),
    ("road_secondary", (0.98, 0.58, 0.10)),
    ("road_minor", (1.0, 0.92, 0.0)),
    ("road_street", (1.0, 1.0, 1.0)),
    ("path", (0.08, 0.06, 0.05)),
    ("rail", (0.0, 0.0, 0.0)),
    ("building_fill", (0.80, 0.76, 0.72)),
    ("building_line", (0.0, 0.0, 0.0)),
    ("boundary", (0.20, 0.20, 0.18)),
    ("text", (0.0, 0.0, 0.0)),
    ("text_green", (0.0, 0.40, 0.06)),
    ("text_red", (0.80, 0.0, 0.0)),
    ("pitch", (0.70, 0.90, 0.58)),
]
INDEX = {name: i for i, (name, _) in enumerate(PALETTE_ENTRIES)}
PALETTE = np.array([colour for _, colour in PALETTE_ENTRIES], dtype=np.float64)

# Area fills, bottom to top: (palette entry, predicate on tags).
AREA_CLASSES = [
    ("farmland", lambda t: t.get("landuse") in ("farmland", "farmyard")),
    ("grass", lambda t: t.get("landuse") in (
        "grass", "meadow", "recreation_ground", "village_green")
     or t.get("leisure") in ("park", "golf_course", "recreation_ground",
                             "nature_reserve")
     or t.get("natural") == "grassland"),
    ("allotments", lambda t: t.get("landuse") in (
        "allotments", "orchard", "vineyard", "plant_nursery")),
    ("built_up", lambda t: t.get("landuse") in ("residential", "railway")),
    ("industrial", lambda t: t.get("landuse") in (
        "industrial", "commercial", "retail")),
    ("cemetery", lambda t: t.get("landuse") == "cemetery"),
    ("pitch", lambda t: t.get("leisure") == "pitch"),
    ("scrub", lambda t: t.get("natural") in ("scrub", "heath")),
    ("wood", lambda t: t.get("natural") == "wood"
     or t.get("landuse") == "forest"),
    ("water", lambda t: t.get("natural") == "water"
     or t.get("landuse") == "reservoir"
     or t.get("leisure") == "swimming_pool"
     or t.get("waterway") == "riverbank"),
]

ROAD_CLASSES = {
    "motorway": "road_primary", "trunk": "road_primary",
    "primary": "road_primary", "secondary": "road_secondary",
    "tertiary": "road_minor", "unclassified": "road_minor",
    "residential": "road_street", "living_street": "road_street",
    "pedestrian": "road_street", "service": "road_street",
}
ROAD_RANK = ["service", "pedestrian", "living_street", "residential",
             "unclassified", "tertiary", "secondary", "primary", "trunk",
             "motorway"]
WATER_LABEL_CLASSES = ("river", "canal", "stream")
WATER_LABEL_REPEATS = 2
PATH_CLASSES = ("footway", "path", "bridleway", "cycleway", "steps", "track")

MAPS = [
    {
        "name": "OSM_TOWN",
        "centre": (51.3463, -2.2515),
        "ground_width_m": 800.0,
        # Output pixels: road width (fill) and casing per road class.
        "road_widths": {"motorway": 7.0, "trunk": 7.0, "primary": 7.0,
                        "secondary": 6.0, "tertiary": 5.0,
                        "unclassified": 4.5, "residential": 4.5,
                        "living_street": 4.0, "pedestrian": 3.5,
                        "service": 2.5},
        "casing": 0.75,
        "draw_buildings": True,
        "draw_paths": True,
        "draw_boundaries": True,
        "contour_interval_m": 5.0,
        "contour_index_every": 5,
        "grid_m": 100.0,
        "street_labels": True,
        "label_sizes": {"town": 16.0, "village": 13.0, "suburb": 11.0,
                        "hamlet": 11.0, "neighbourhood": 10.0,
                        "locality": 9.0, "farm": 9.0,
                        "isolated_dwelling": 9.0},
        "street_label_px": 9.0,
        "amenity_label_px": 8.0,
        "water_label_px": 11.0,
        "wood_symbol_pitch": 4.0,
    },
    {
        "name": "OSM_RURAL",
        "centre": (51.3440, -2.2560),
        "ground_width_m": 6000.0,
        "road_widths": {"motorway": 4.0, "trunk": 4.0, "primary": 4.0,
                        "secondary": 3.5, "tertiary": 3.0,
                        "unclassified": 2.5},
        "casing": 0.6,
        "draw_buildings": False,
        "draw_paths": False,
        "draw_boundaries": False,
        "contour_interval_m": 10.0,
        "contour_index_every": 5,
        "grid_m": 1000.0,
        "street_labels": False,
        "label_sizes": {"town": 18.0, "village": 13.0, "suburb": 10.0,
                        "hamlet": 10.0, "neighbourhood": 9.0,
                        "locality": 9.0, "farm": 8.0,
                        "isolated_dwelling": 8.0},
        "street_label_px": 0.0,
        "amenity_label_px": 0.0,
        "water_label_px": 11.0,
        "wood_symbol_pitch": 5.0,
    },
]


class Projection:
    """Local equirectangular projection onto the supersampled canvas; north
    is up and the ground covered is ground_width_m by 3/4 of that."""

    def __init__(self, centre, ground_width_m):
        self.lat0, self.lon0 = centre
        self.metres_per_lon = EARTH_METRES_PER_DEGREE * math.cos(
            math.radians(self.lat0))
        self.px_per_m = CANVAS_W / ground_width_m
        self.ground_width_m = ground_width_m
        self.ground_height_m = ground_width_m / PICTURE_ASPECT

    def metres(self, lon, lat):
        return ((lon - self.lon0) * self.metres_per_lon,
                (lat - self.lat0) * EARTH_METRES_PER_DEGREE)

    def point(self, lon, lat):
        x_m, y_m = self.metres(lon, lat)
        return (CANVAS_W / 2.0 + x_m * self.px_per_m,
                CANVAS_H / 2.0 - y_m * self.px_per_m * CANVAS_H / CANVAS_W
                * PICTURE_ASPECT)

    def points(self, geometry):
        return [self.point(lon, lat) for lon, lat in geometry]


def join_chains(ways):
    """Joins ways that share end points into the longest chains."""
    pending = [list(w) for w in ways if len(w) >= 2]
    chains = []
    while pending:
        ring = pending.pop()
        changed = True
        while ring[0] != ring[-1] and changed:
            changed = False
            for i, way in enumerate(pending):
                if way[0] == ring[-1]:
                    ring.extend(way[1:])
                elif way[-1] == ring[-1]:
                    ring.extend(reversed(way[:-1]))
                elif way[-1] == ring[0]:
                    ring[:0] = way[:-1]
                elif way[0] == ring[0]:
                    ring[:0] = list(reversed(way[1:]))
                else:
                    continue
                pending.pop(i)
                changed = True
                break
        chains.append(ring)
    return chains


def assemble_rings(ways):
    """Joins open member ways end to end into closed rings."""
    return [ring for ring in join_chains(ways)
            if len(ring) >= 4 and ring[0] == ring[-1]]


def load_features(path):
    """Areas as (tags, outer rings, inner rings) and lines as (tags,
    geometry), in lon/lat."""
    with open(path) as extract:
        elements = json.load(extract)["elements"]
    areas, lines, nodes = [], [], []
    for element in elements:
        tags = element["tags"]
        if element["type"] == "node":
            nodes.append((tags, element["geometry"][0]))
        elif element["type"] == "way":
            geometry = element["geometry"]
            closed = len(geometry) >= 4 and geometry[0] == geometry[-1]
            linear = ("highway" in tags or "barrier" in tags
                      or "railway" in tags
                      or tags.get("waterway") not in (None, "riverbank"))
            if closed and (not linear or tags.get("area") == "yes"):
                areas.append((tags, [geometry], []))
            else:
                lines.append((tags, geometry))
        elif tags.get("type") == "multipolygon":
            members = element["members"]
            outer = assemble_rings(
                [m["geometry"] for m in members if m["role"] != "inner"])
            inner = assemble_rings(
                [m["geometry"] for m in members if m["role"] == "inner"])
            areas.append((tags, outer, inner))
    return areas, lines, nodes


def scaled(output_pixels):
    return max(1, round(output_pixels * SUPERSAMPLE))


def fill_class(canvas, projection, areas, predicate):
    """Mask (L image) of every area matching predicate, holes cut out."""
    mask = Image.new("L", canvas.size, 0)
    draw = ImageDraw.Draw(mask)
    for tags, outer, inner in areas:
        if not predicate(tags):
            continue
        for ring in outer:
            draw.polygon(projection.points(ring), fill=255)
        for ring in inner:
            draw.polygon(projection.points(ring), fill=0)
    return mask


def outline_class(draw, projection, areas, predicate, index, width):
    for tags, outer, inner in areas:
        if predicate(tags):
            for ring in outer + inner:
                draw.line(projection.points(ring), fill=index, width=width)


def pattern(shape, period_x, period_y, size_x, size_y, stagger):
    """Boolean dot/dash pattern: size_x x size_y marks on a period grid, every
    other row offset by half a period when stagger is set."""
    rows = np.arange(shape[0])[:, None]
    cols = np.arange(shape[1])[None, :]
    row_cell = rows // period_y
    offset = (row_cell % 2) * (period_x // 2) if stagger else 0
    return (((cols + offset) % period_x < size_x)
            & (rows % period_y < size_y))


def apply(canvas_array, mask, index, where=None):
    selected = np.asarray(mask) > 0
    if where is not None:
        selected &= where
    canvas_array[selected] = index


def dashed(points, dash, gap):
    """Splits a polyline into dash polylines of length dash every dash+gap."""
    segments, current = [], [points[0]]
    position, drawing = 0.0, True
    limit = dash
    for (x0, y0), (x1, y1) in zip(points, points[1:]):
        length = math.hypot(x1 - x0, y1 - y0)
        start = 0.0
        while length - start > limit - position:
            t = (start + limit - position) / length
            point = (x0 + (x1 - x0) * t, y0 + (y1 - y0) * t)
            if drawing:
                current.append(point)
                segments.append(current)
            current = [point]
            start += limit - position
            position, drawing = 0.0, not drawing
            limit = dash if drawing else gap
        position += length - start
        if drawing:
            current.append((x1, y1))
    if drawing and len(current) > 1:
        segments.append(current)
    return segments


def waterway_segments(lines, projection):
    """Rivers, streams and the canal as metre-coordinate segments."""
    segments = []
    for tags, geometry in lines:
        if tags.get("waterway") in ("river", "canal", "stream"):
            pts = [projection.metres(lon, lat) for lon, lat in geometry]
            segments.extend(zip(pts, pts[1:]))
    return np.array(segments, dtype=np.float64).reshape(-1, 4)


def ridge(x, y, centre, length, width, angle_deg, height):
    """An elongated Gaussian hill along angle_deg (from east)."""
    angle = math.radians(angle_deg)
    u = (x - centre[0]) * math.cos(angle) + (y - centre[1]) * math.sin(angle)
    v = -(x - centre[0]) * math.sin(angle) + (y - centre[1]) * math.cos(angle)
    return height * np.exp(-0.5 * ((u / length) ** 2 + (v / width) ** 2))


def height_field(projection, segments):
    """Analytic ground height (m) on the canvas: rises away from the nearest
    waterway, with smooth hills; computed coarse and upsampled."""
    step = 8
    gw, gh = CANVAS_W // step + 1, CANVAS_H // step + 1
    xs = (np.arange(gw) * step - CANVAS_W / 2.0) / projection.px_per_m
    ys = -(np.arange(gh) * step - CANVAS_H / 2.0) / projection.px_per_m * (
        CANVAS_W / CANVAS_H / PICTURE_ASPECT)
    gx, gy = np.meshgrid(xs, ys)
    px_ = gx.ravel()[:, None]
    py_ = gy.ravel()[:, None]
    nearest = np.full(px_.shape[0], np.inf)
    for chunk in range(0, len(segments), 256):
        seg = segments[chunk:chunk + 256]
        ax, ay, bx, by = seg[:, 0], seg[:, 1], seg[:, 2], seg[:, 3]
        dx, dy = bx - ax, by - ay
        length2 = np.maximum(dx * dx + dy * dy, 1e-9)
        t = np.clip(((px_ - ax) * dx + (py_ - ay) * dy) / length2, 0.0, 1.0)
        d = np.hypot(px_ - (ax + t * dx), py_ - (ay + t * dy))
        nearest = np.minimum(nearest, d.min(axis=1))
    # Map-wide metre coordinates relative to the extract's centre, so both
    # maps sample the same hills.
    ox, oy = projection.metres(*TERRAIN_ORIGIN)
    wx = gx.ravel() - ox
    wy = gy.ravel() - oy
    valley = 18.0 + 95.0 * (1.0 - np.exp(-nearest / 900.0))
    hills = (ridge(wx, wy, (1400.0, 1100.0), 1300.0, 500.0, 30.0, 28.0)
             + ridge(wx, wy, (-1900.0, -900.0), 1500.0, 650.0, -20.0, 22.0)
             + ridge(wx, wy, (900.0, -1500.0), 900.0, 400.0, 65.0, 16.0)
             + 9.0 * np.sin(wx / 520.0) * np.cos(wy / 610.0)
             + 0.004 * wy)
    coarse = (valley + hills).reshape(gh, gw).astype(np.float32)
    image = Image.fromarray(coarse, mode="F").resize(
        (gw * step, gh * step), Image.BICUBIC)
    return np.asarray(image)[:CANVAS_H, :CANVAS_W]


def contour_masks(height, interval, index_every):
    """Boolean masks for ordinary and index contours: the pixels where the
    contour band changes from a neighbour, thickened."""
    band = np.floor(height / interval).astype(np.int64)
    edge = np.zeros(band.shape, dtype=bool)
    edge[:, 1:] |= band[:, 1:] != band[:, :-1]
    edge[1:, :] |= band[1:, :] != band[:-1, :]
    level = np.maximum(band, np.roll(band, 1, axis=1))
    level = np.maximum(level, np.roll(band, 1, axis=0))
    index = edge & (level % index_every == 0)
    thick = edge | np.roll(edge, 1, axis=1) | np.roll(edge, 1, axis=0)
    thick_index = index.copy()
    for dx in (-1, 1, 2):
        thick_index |= np.roll(index, dx, axis=1)
        thick_index |= np.roll(index, dx, axis=0)
    return thick & ~thick_index, thick_index


class Labeller:
    """Places labels without overlap; rotated labels use their bounding box.
    Glyphs are drawn aliased on the supersampled canvas."""

    def __init__(self, canvas, font_dir):
        self.canvas = canvas
        self.font_dir = font_dir
        self.placed = []
        self.fonts = {}

    def font(self, face, size_px):
        key = (face, size_px)
        if key not in self.fonts:
            self.fonts[key] = ImageFont.truetype(
                os.path.join(self.font_dir, face + ".ttf"),
                round(size_px * SUPERSAMPLE))
        return self.fonts[key]

    def free(self, box):
        x0, y0, x1, y1 = box
        if x0 < 0 or y0 < 0 or x1 > CANVAS_W or y1 > CANVAS_H:
            return False
        for a in self.placed:
            if x0 < a[2] and a[0] < x1 and y0 < a[3] and a[1] < y1:
                return False
        return True

    def text_image(self, text, face, size_px):
        font = self.font(face, size_px)
        left, top, right, bottom = font.getbbox(text)
        margin = SUPERSAMPLE
        image = Image.new("1", (right - left + 2 * margin,
                                bottom - top + 2 * margin), 0)
        draw = ImageDraw.Draw(image)
        draw.fontmode = "1"
        draw.text((margin - left, margin - top), text, font=font, fill=1)
        return image

    def place(self, text, face, size_px, colour, centre, angle=0.0,
              force=False):
        image = self.text_image(text, face, size_px)
        if angle:
            image = image.rotate(angle, resample=Image.NEAREST, expand=True)
        w, h = image.size
        x0 = round(centre[0] - w / 2.0)
        y0 = round(centre[1] - h / 2.0)
        box = (x0, y0, x0 + w, y0 + h)
        if not force and not self.free(box):
            return False
        self.placed.append(box)
        self.canvas.paste(INDEX[colour], (x0, y0), image)
        return True

    def along(self, text, face, size_px, colour, points):
        """Labels a polyline where a straight chord the length of the text
        stays close to it, trying the middle of the line first."""
        cumulative = [0.0]
        for (x0, y0), (x1, y1) in zip(points, points[1:]):
            cumulative.append(cumulative[-1] + math.hypot(x1 - x0, y1 - y0))
        total = cumulative[-1]
        width = self.font(face, size_px).getlength(text) + 2 * SUPERSAMPLE
        if width > total:
            return False
        tolerance = 0.35 * size_px * SUPERSAMPLE

        def at(distance):
            for i in range(1, len(cumulative)):
                if cumulative[i] >= distance:
                    span = cumulative[i] - cumulative[i - 1]
                    t = (distance - cumulative[i - 1]) / span if span else 0.0
                    (x0, y0), (x1, y1) = points[i - 1], points[i]
                    return (x0 + (x1 - x0) * t, y0 + (y1 - y0) * t)
            return points[-1]

        for fraction in (0.5, 0.35, 0.65, 0.2, 0.8):
            middle = min(max(total * fraction, width / 2.0),
                         total - width / 2.0)
            a, b = middle - width / 2.0, middle + width / 2.0
            (ax, ay), (bx, by) = at(a), at(b)
            chord = math.hypot(bx - ax, by - ay)
            if chord < 0.9 * width:
                continue
            inside = [p for p, d in zip(points, cumulative) if a < d < b]
            deviation = max((abs((bx - ax) * (ay - y) - (ax - x) * (by - ay))
                             / chord for x, y in inside), default=0.0)
            if deviation > tolerance:
                continue
            angle = -math.degrees(math.atan2(by - ay, bx - ax))
            if angle > 90.0:
                angle -= 180.0
            elif angle < -90.0:
                angle += 180.0
            if self.place(text, face, size_px, colour,
                          ((ax + bx) / 2.0, (ay + by) / 2.0), angle):
                return True
        return False


def centroid(points):
    xs = [p[0] for p in points]
    ys = [p[1] for p in points]
    return (sum(xs) / len(xs), sum(ys) / len(ys))


def render_map(spec, areas, lines, nodes, font_dir):
    projection = Projection(spec["centre"], spec["ground_width_m"])
    canvas = Image.new("L", (CANVAS_W, CANVAS_H), INDEX["paper"])
    draw = ImageDraw.Draw(canvas)

    # Land cover, bottom to top, with the wood and scrub symbols.
    for name, predicate in AREA_CLASSES:
        mask = fill_class(canvas, projection, areas, predicate)
        array = np.array(canvas)
        apply(array, mask, INDEX[name])
        pitch = round(spec["wood_symbol_pitch"] * SUPERSAMPLE)
        if name == "wood":
            apply(array, mask, INDEX["wood_symbol"],
                  pattern(array.shape, pitch, pitch, SUPERSAMPLE,
                          SUPERSAMPLE, stagger=True))
        elif name == "scrub":
            apply(array, mask, INDEX["wood_symbol"],
                  pattern(array.shape, 2 * pitch, 2 * pitch, SUPERSAMPLE,
                          SUPERSAMPLE, stagger=True))
        elif name == "water":
            # Marsh-style dashes on standing water larger than the river.
            apply(array, mask, INDEX["water_line"],
                  pattern(array.shape, 3 * pitch, 2 * pitch, 2 * pitch,
                          SUPERSAMPLE // 2, stagger=True)
                  & np.asarray(fill_class(
                      canvas, projection, areas,
                      lambda t: t.get("water") in ("wastewater", "pond")
                      or t.get("natural") == "wetland")).astype(bool))
        canvas = Image.fromarray(array, mode="L")
        draw = ImageDraw.Draw(canvas)

    outline_class(draw, projection, areas,
                  lambda t: t.get("natural") == "water", INDEX["water_line"],
                  scaled(0.75))
    outline_class(draw, projection, areas,
                  lambda t: t.get("leisure") == "pitch",
                  INDEX["road_street"], scaled(0.5))

    # Contours and grid sit under the line work.
    segments = waterway_segments(lines, projection)
    height = height_field(projection, segments)
    ordinary, index = contour_masks(height, spec["contour_interval_m"],
                                    spec["contour_index_every"])
    array = np.array(canvas)
    array[ordinary] = INDEX["contour"]
    array[index] = INDEX["contour_index"]
    canvas = Image.fromarray(array, mode="L")
    draw = ImageDraw.Draw(canvas)

    grid = spec["grid_m"]
    half_w = projection.ground_width_m / 2.0
    half_h = projection.ground_height_m / 2.0
    ox, oy = projection.metres(*TERRAIN_ORIGIN)
    grid_width = scaled(0.5)
    x_m = math.ceil((-half_w - ox) / grid) * grid + ox
    while x_m < half_w:
        x = CANVAS_W / 2.0 + x_m * projection.px_per_m
        draw.line([(x, 0), (x, CANVAS_H)], fill=INDEX["grid"],
                  width=grid_width)
        x_m += grid
    y_m = math.ceil((-half_h - oy) / grid) * grid + oy
    while y_m < half_h:
        y = CANVAS_H / 2.0 - y_m * CANVAS_H / projection.ground_height_m
        draw.line([(0, y), (CANVAS_W, y)], fill=INDEX["grid"],
                  width=grid_width)
        y_m += grid

    # Waterways.
    for tags, geometry in lines:
        waterway = tags.get("waterway")
        if waterway in ("river", "canal"):
            width = 1.5
        elif waterway in ("stream", "drain", "ditch"):
            width = 0.75
        else:
            continue
        draw.line(projection.points(geometry), fill=INDEX["water_line"],
                  width=scaled(width), joint="curve")

    if spec["draw_boundaries"]:
        for tags, geometry in lines:
            if "barrier" in tags:
                draw.line(projection.points(geometry),
                          fill=INDEX["boundary"], width=scaled(0.5))

    # Railways: a black line with ticks every few pixels.
    for tags, geometry in lines:
        railway = tags.get("railway")
        if railway == "rail":
            points = projection.points(geometry)
            draw.line(points, fill=INDEX["rail"], width=scaled(1.25),
                      joint="curve")
            for dash in dashed(points, scaled(0.5), scaled(5.0)):
                draw.line(dash, fill=INDEX["rail"], width=scaled(3.0))
        elif railway in ("disused", "abandoned"):
            for dash in dashed(projection.points(geometry), scaled(3.0),
                               scaled(2.0)):
                draw.line(dash, fill=INDEX["rail"], width=scaled(0.75))

    if spec["draw_paths"]:
        for tags, geometry in lines:
            if tags.get("highway") in PATH_CLASSES:
                for dash in dashed(projection.points(geometry),
                                   scaled(2.0), scaled(1.5)):
                    draw.line(dash, fill=INDEX["path"], width=scaled(0.75))

    # Roads: every casing first, then the fills, both by rank, so junctions
    # merge.
    widths = spec["road_widths"]
    roads = [(ROAD_RANK.index(tags["highway"].replace("_link", "")), tags,
              projection.points(geometry))
             for tags, geometry in lines
             if tags.get("highway", "").replace("_link", "") in widths]
    roads.sort(key=lambda r: r[0])
    casing = spec["casing"]
    for _, tags, points in roads:
        width = widths[tags["highway"].replace("_link", "")]
        draw.line(points, fill=INDEX["casing"],
                  width=scaled(width + 2.0 * casing), joint="curve")
    for _, tags, points in roads:
        kind = tags["highway"].replace("_link", "")
        draw.line(points, fill=INDEX[ROAD_CLASSES[kind]],
                  width=scaled(widths[kind]), joint="curve")

    # Buildings: grey fill, black 45 degree hatching and outline.
    if spec["draw_buildings"]:
        mask = fill_class(canvas, projection, areas,
                          lambda t: "building" in t)
        array = np.array(canvas)
        apply(array, mask, INDEX["building_fill"])
        rows = np.arange(CANVAS_H)[:, None]
        cols = np.arange(CANVAS_W)[None, :]
        hatch = (rows + cols) % (3 * SUPERSAMPLE) < SUPERSAMPLE // 2 + 1
        apply(array, mask, INDEX["building_line"], hatch)
        canvas = Image.fromarray(array, mode="L")
        draw = ImageDraw.Draw(canvas)
        outline_class(draw, projection, areas, lambda t: "building" in t,
                      INDEX["building_line"], scaled(0.75))

    # Lettering: place names first, then water, streets and amenities.
    labeller = Labeller(canvas, font_dir)
    labeller.place(ATTRIBUTION, "LiberationSans-Regular", 7.0, "text",
                   (CANVAS_W - 70 * SUPERSAMPLE, CANVAS_H - 6 * SUPERSAMPLE),
                   force=True)
    sizes = spec["label_sizes"]
    places = sorted(
        (n for n in nodes if n[0].get("place") in sizes and "name" in n[0]),
        key=lambda n: -sizes[n[0]["place"]])
    for tags, (lon, lat) in places:
        size = sizes[tags["place"]]
        face = ("LiberationSans-Bold" if size >= 12.0
                else "LiberationSans-Regular")
        if tags["place"] in ("farm", "isolated_dwelling", "locality"):
            face = "LiberationSans-Italic"
        labeller.place(tags["name"].upper() if size >= 16.0 else tags["name"],
                       face, size, "text", projection.point(lon, lat))

    # Water names repeat along the river and canal, as on OS maps; the
    # extract splits each into many ways, so they are joined by name first.
    labelled = set()
    water = {}
    for tags, geometry in lines:
        if tags.get("name") and tags.get("waterway") in WATER_LABEL_CLASSES:
            water.setdefault(tags["name"], []).append(geometry)
    for name, ways in sorted(water.items()):
        placed = 0
        for chain in sorted(join_chains(ways), key=len, reverse=True):
            points = projection.points(chain)
            for part in (points[:len(points) // 2], points[len(points) // 2:]):
                if placed < WATER_LABEL_REPEATS and len(part) > 1 and \
                        labeller.along(name, "LiberationSerif-Italic",
                                       spec["water_label_px"], "water_text",
                                       part):
                    placed += 1
        if placed:
            labelled.add(name)
    for tags, outer, _ in areas:
        name = tags.get("name")
        if name and name not in labelled and outer:
            points = projection.points(outer[0])
            if tags.get("natural") == "wood" or tags.get("landuse") == \
                    "forest":
                if labeller.place(name, "LiberationSerif-Italic", 10.0,
                                  "text_green", centroid(points)):
                    labelled.add(name)

    if spec["street_labels"]:
        for tags, geometry in sorted(lines, key=lambda l: -len(l[1])):
            name = tags.get("name")
            if (name and name not in labelled
                    and tags.get("highway") in ROAD_CLASSES):
                if labeller.along(name, "LiberationSans-Regular",
                                  spec["street_label_px"], "text",
                                  projection.points(geometry)):
                    labelled.add(name)
    if spec["amenity_label_px"]:
        colour_of = {"place_of_worship": "text_red", "pub": "text_red"}
        named = [(tags, outer[0]) for tags, outer, _ in areas
                 if outer and tags.get("name") and (
                     "amenity" in tags or "historic" in tags
                     or "tourism" in tags)]
        named += [(tags, [geometry]) for tags, geometry in nodes
                  if tags.get("name") and "place" not in tags]
        for tags, ring in named:
            if tags["name"] in labelled:
                continue
            centre = centroid(projection.points(ring))
            if labeller.place(tags["name"], "LiberationSans-Bold",
                              spec["amenity_label_px"],
                              colour_of.get(tags.get("amenity"), "text"),
                              centre):
                labelled.add(tags["name"])

    # Box reduction to the 720x576 raster in float R'G'B'.
    rgb = PALETTE[np.asarray(canvas)]
    rgb = rgb.reshape(HEIGHT, SUPERSAMPLE, WIDTH, SUPERSAMPLE, 3)
    return rgb.mean(axis=(1, 3))


def write_planar(path, rgb):
    with open(path, "wb") as out:
        for channel in (1, 2, 0):  # gbrpf32le plane order
            out.write(np.ascontiguousarray(
                rgb[:, :, channel], dtype="<f4").tobytes())


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    extract, font_dir, out_dir = sys.argv[1:]
    areas, lines, nodes = load_features(extract)
    for spec in MAPS:
        rgb = render_map(spec, areas, lines, nodes, font_dir)
        assert rgb.min() >= 0.0 and rgb.max() <= 1.0
        path = os.path.join(out_dir, "%s_%dx%d.gbrpf32le"
                            % (spec["name"], WIDTH, HEIGHT))
        write_planar(path, rgb)
        print(path)


if __name__ == "__main__":
    main()
