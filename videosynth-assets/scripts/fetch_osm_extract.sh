#!/usr/bin/env bash
# Fetches the OpenStreetMap extract that render_osm_map.py draws the
# OSM_TOWN and OSM_RURAL map stills from, and writes it, trimmed to the tags
# the renderer reads, to sources/osm/bradford_on_avon.json.
#
# The extract is Bradford-on-Avon, Wiltshire: a market town on the River Avon
# and the Kennet and Avon Canal, with the farmland and woods around it. One
# Overpass API query covers both maps: the land cover, water, main roads,
# railways and place names over the whole rural area, and every building,
# road, path and boundary in the town-centre area. The data is
# (c) OpenStreetMap contributors under the ODbL 1.0 (see
# sources/osm/LICENSE.md); the extract is committed so the maps rebuild without
# a network fetch. Only rerun this script to refresh the extract: Overpass is
# a shared service, so keep it to occasional single requests.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${ROOT_DIR}/sources/osm/bradford_on_avon.json"
OVERPASS_URL="${OVERPASS_URL:-https://overpass-api.de/api/interpreter}"

if [[ "${VIDEOSYNTH_OSM_FETCH_IN_SHELL:-}" != "1" ]]; then
  if ! command -v nix >/dev/null 2>&1; then
    echo "Error: nix is required but not found in PATH." >&2
    exit 1
  fi
  VIDEOSYNTH_OSM_FETCH_IN_SHELL=1 exec nix shell nixpkgs#curl nixpkgs#python3 \
    -c bash "${BASH_SOURCE[0]}" "$@"
fi

# south,west,north,east. Each covers its map in render_osm_map.py with a
# margin, so features crossing the frame edge are complete.
RURAL_BBOX="51.3225,-2.3005,51.3655,-2.2115"
TOWN_BBOX="51.3432,-2.2577,51.3494,-2.2453"

read -r -d '' QUERY <<EOF || true
[out:json][timeout:180];
(
  nwr["landuse"](${RURAL_BBOX});
  nwr["natural"~"^(wood|water|wetland|scrub|heath|grassland)$"](${RURAL_BBOX});
  nwr["leisure"~"^(park|golf_course|pitch|recreation_ground|nature_reserve)$"](${RURAL_BBOX});
  way["waterway"](${RURAL_BBOX});
  way["highway"~"^(motorway|trunk|primary|secondary|tertiary|unclassified)(_link)?$"](${RURAL_BBOX});
  way["railway"~"^(rail|disused|abandoned)$"](${RURAL_BBOX});
  node["place"~"^(town|village|hamlet|suburb|neighbourhood|locality|isolated_dwelling|farm)$"](${RURAL_BBOX});
  way["building"](${TOWN_BBOX});
  way["highway"](${TOWN_BBOX});
  way["barrier"~"^(wall|hedge|fence|retaining_wall)$"](${TOWN_BBOX});
  nwr["amenity"]["name"](${TOWN_BBOX});
  nwr["historic"]["name"](${TOWN_BBOX});
  nwr["tourism"]["name"](${TOWN_BBOX});
);
out geom qt;
EOF

work_dir="$(mktemp -d)"
trap 'rm -rf "${work_dir}"' EXIT

curl -sS --fail -A "videosynth-assets map extract (single request)" \
  --data-urlencode "data=${QUERY}" -o "${work_dir}/raw.json" "${OVERPASS_URL}"

mkdir -p "$(dirname "${OUT}")"
python3 -I - "${work_dir}/raw.json" "${OUT}" <<'PY'
"""Trims the Overpass response to the tags and geometry the renderer reads,
with coordinates rounded to 1e-6 degree (about 0.1 m)."""
import json
import sys

KEEP_TAGS = {
    "name", "highway", "building", "landuse", "natural", "water", "waterway",
    "leisure", "amenity", "historic", "tourism", "railway", "barrier",
    "place", "wetland", "tunnel", "bridge", "layer", "type", "area",
}


def point(p):
    return [round(p["lon"], 6), round(p["lat"], 6)]


with open(sys.argv[1]) as raw:
    response = json.load(raw)

elements = []
for element in response["elements"]:
    tags = {k: v for k, v in element.get("tags", {}).items() if k in KEEP_TAGS}
    out = {"type": element["type"], "id": element["id"], "tags": tags}
    if element["type"] == "node":
        out["geometry"] = [point(element)]
    elif element["type"] == "way":
        out["geometry"] = [point(p) for p in element["geometry"]]
    else:
        out["members"] = [
            {"role": m["role"], "geometry": [point(p) for p in m["geometry"]]}
            for m in element.get("members", [])
            if m["type"] == "way" and "geometry" in m
        ]
    elements.append(out)
elements.sort(key=lambda e: (e["type"], e["id"]))

with open(sys.argv[2], "w") as out:
    json.dump({
        "copyright": "(c) OpenStreetMap contributors. Data available under "
                     "the Open Database License (ODbL) 1.0: "
                     "https://opendatacommons.org/licenses/odbl/1-0/",
        "timestamp_osm_base": response["osm3s"]["timestamp_osm_base"],
        "elements": elements,
    }, out, separators=(",", ":"))
    out.write("\n")
print("Wrote %s (%d elements)" % (sys.argv[2], len(elements)))
PY
