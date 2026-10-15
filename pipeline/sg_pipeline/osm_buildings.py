"""Stage 2 of the building pipeline: OpenStreetMap PBF -> clean building footprints.

Steps implemented here (see docs/BUILDING_PIPELINE.md):
  1. Read the PBF and assemble areas (closed building ways and building multipolygons).
  2. Find the Singapore country boundary (OSM admin_level=2 relation) and use it to clip.
  3. Convert outlines from WGS84 to SVY21 metres (pyproj, via sg_pipeline.svy21).
  4. Repair invalid geometry and drop slivers, counting every removal.
  5. Assign height by a documented rule order, with a confidence class per building.
  6. Assign each building to its 512 m macro-cell.
  7. Write a SQLite table (outside Git) and a JSON report (inside Git).

Heights that come from defaults are estimates. They are labelled as such and must be
replaced by better data when it becomes available.
"""

from __future__ import annotations

import argparse
import datetime as _dt
import hashlib
import json
import math
import re
import sqlite3
import sys
from collections import Counter
from pathlib import Path

import osmium
import shapely
from shapely.geometry import MultiPolygon, Polygon
from shapely.ops import transform as shp_transform
from pyproj import Transformer

from sg_pipeline import svy21

# ---------------------------------------------------------------------------
# Documented assumptions (replace with better data when available)
# ---------------------------------------------------------------------------

METRES_PER_LEVEL = 3.0  # assumed storey height when only a level count is known

# Default heights (metres) by OSM building tag. These are estimates, not measurements.
TYPE_DEFAULT_HEIGHT_M = {
    "house": 7.0, "detached": 7.0, "semidetached_house": 7.0, "terrace": 8.0,
    "residential": 12.0, "apartments": 20.0, "commercial": 20.0, "retail": 12.0,
    "office": 40.0, "industrial": 12.0, "warehouse": 12.0, "school": 15.0,
    "hotel": 40.0, "dormitory": 30.0, "train_station": 15.0, "public": 15.0,
    "university": 20.0, "parking": 8.0, "garage": 4.0, "roof": 4.0,
    "storage_tank": 10.0, "construction": 10.0, "service": 4.0, "yes": 10.0,
}
DEFAULT_HEIGHT_M = 10.0

MIN_HEIGHT_M = 2.0
MAX_PLAUSIBLE_HEIGHT_M = 350.0   # above this, the source value is treated as invalid
MIN_FOOTPRINT_M2 = 4.0           # smaller polygons are treated as slivers and dropped

_HEIGHT_RE = re.compile(r"^\s*([0-9]+(?:\.[0-9]+)?)\s*(m|meters|metres)?\s*$", re.IGNORECASE)

WGS84_TO_SVY21 = Transformer.from_crs(4326, 3414, always_xy=True).transform


# ---------------------------------------------------------------------------
# Height rules
# ---------------------------------------------------------------------------

def parse_height_tag(value: str | None) -> float | None:
    """Parse an OSM height value in metres. Returns None for anything not understood."""
    if value is None:
        return None
    m = _HEIGHT_RE.match(value)
    if not m:
        return None
    h = float(m.group(1))
    return h if math.isfinite(h) else None


def estimate_height(tags: dict[str, str]) -> tuple[float, str, str]:
    """Return (height_m, rule, confidence) using the first rule that applies.

    Order: explicit height tag -> level count -> building-type default.
    """
    h = parse_height_tag(tags.get("height"))
    if h is not None and MIN_HEIGHT_M <= h <= MAX_PLAUSIBLE_HEIGHT_M:
        return h, "osm_height_tag", "medium"

    try:
        levels = float(tags.get("building:levels", ""))
    except ValueError:
        levels = float("nan")
    if math.isfinite(levels) and 1 <= levels <= 120:
        return max(MIN_HEIGHT_M, levels * METRES_PER_LEVEL), "osm_levels_x_3m", "medium-low"

    btype = tags.get("building", "yes")
    base = TYPE_DEFAULT_HEIGHT_M.get(btype, DEFAULT_HEIGHT_M)
    return base, f"type_default:{btype if btype in TYPE_DEFAULT_HEIGHT_M else 'other'}", "low"


# ---------------------------------------------------------------------------
# Reading the PBF
# ---------------------------------------------------------------------------

def _is_singapore_boundary(tags) -> bool:
    return (
        tags.get("boundary") == "administrative"
        and tags.get("admin_level") == "2"
        and (tags.get("name") == "Singapore" or tags.get("ISO3166-1:alpha2") == "SG")
    )


def read_areas(pbf_path: str, failed: Counter):
    """Yield (kind, osm_id, tags, wkb) for the Singapore boundary and every building area.

    Only osmium Area objects are used: closed building ways and building multipolygon
    relations, with rings assembled by the reader. Geometry failures are counted in `failed`.
    """
    wkbf = osmium.geom.WKBFactory()
    fp = osmium.FileProcessor(pbf_path).with_areas()
    for obj in fp:
        if not isinstance(obj, osmium.osm.Area):
            continue
        tags = {k: v for k, v in obj.tags}
        is_boundary = _is_singapore_boundary(tags)
        is_building = "building" in tags
        if not (is_boundary or is_building):
            continue
        try:
            wkb = wkbf.create_multipolygon(obj)
        except Exception:  # noqa: BLE001 - osmium raises on broken or incomplete rings
            failed["geometry_failed"] += 1
            continue
        prefix = "w" if obj.from_way() else "r"
        kind = "boundary" if is_boundary else "building"
        # osmium returns WKB as a hex string by default.
        raw = bytes.fromhex(wkb) if isinstance(wkb, str) else bytes(wkb)
        yield kind, f"{prefix}{obj.orig_id()}", tags, raw


def load_boundary_from_osm_xml(xml_path: Path, relation_id: str = "536780"):
    """Assemble the outer rings of an OSM relation from a full-download .osm XML file.

    Returns a WGS84 shapely geometry. Raises SystemExit if any member way is missing,
    because a partial boundary would silently keep or drop buildings.
    """
    import xml.etree.ElementTree as ET
    from shapely.geometry import LineString
    from shapely.ops import linemerge, polygonize

    root = ET.parse(xml_path).getroot()
    nodes = {n.get("id"): (float(n.get("lon")), float(n.get("lat"))) for n in root.iter("node")}
    ways = {w.get("id"): [nd.get("ref") for nd in w.findall("nd")] for w in root.iter("way")}
    rels = [r for r in root.iter("relation") if r.get("id") == relation_id]
    if not rels:
        raise SystemExit(f"Relation {relation_id} not found in {xml_path}")
    rel = rels[0]
    outer = [m.get("ref") for m in rel.findall("member") if m.get("type") == "way" and m.get("role") == "outer"]
    missing = [w for w in outer if w not in ways or any(n not in nodes for n in ways[w])]
    if missing or not outer:
        raise SystemExit(f"Boundary incomplete: {len(missing)} of {len(outer)} outer ways missing or lack nodes")
    lines = [LineString([nodes[n] for n in ways[w]]) for w in outer]
    polys = list(polygonize(linemerge(lines)))
    if not polys:
        raise SystemExit("Boundary ways do not close into polygons")
    return shapely.make_valid(shapely.union_all(polys))


# ---------------------------------------------------------------------------
# Pipeline
# ---------------------------------------------------------------------------

def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def build(pbf_path: Path, out_db: Path, report_path: Path, source_note: str, boundary_osm: Path | None = None) -> dict:
    report: dict = {
        "schema_version": 1,
        "generated_utc": _dt.datetime.now(_dt.timezone.utc).isoformat(timespec="seconds"),
        "input": {
            "file": pbf_path.name,
            "bytes": pbf_path.stat().st_size,
            "sha256": sha256_of(pbf_path),
            "note": source_note,
        },
        "crs": "EPSG:3414 (SVY21)",
        "assumptions": {
            "metres_per_level": METRES_PER_LEVEL,
            "type_default_height_m": TYPE_DEFAULT_HEIGHT_M,
            "default_height_m": DEFAULT_HEIGHT_M,
            "min_height_m": MIN_HEIGHT_M,
            "max_plausible_height_m": MAX_PLAUSIBLE_HEIGHT_M,
            "min_footprint_m2": MIN_FOOTPRINT_M2,
        },
    }

    boundary_wkb: bytes | None = None
    buildings: list[tuple[str, dict, bytes]] = []
    counts: Counter = Counter()
    failed: Counter = Counter()

    for kind, osm_id, tags, wkb in read_areas(str(pbf_path), failed):
        if kind == "boundary":
            counts["boundary_candidates"] += 1
            if boundary_wkb is None or len(wkb) > len(boundary_wkb):
                boundary_wkb = wkb  # keep the largest matching relation
        else:
            counts["building_areas_read"] += 1
            buildings.append((osm_id, tags, wkb))

    if boundary_osm is not None:
        boundary_ll = load_boundary_from_osm_xml(boundary_osm)
        boundary_wkb = boundary_ll.wkb
        boundary_src = f"OSM relation 536780 from uploaded file {boundary_osm.name}"
    else:
        boundary_src = "OSM admin_level=2 relation 'Singapore' (from the input file)"

    if boundary_wkb is None:
        raise SystemExit("No complete Singapore boundary (admin_level=2 relation 536780 or place relation 17140517) could be assembled from this PBF: member ways are missing in the extract. Refusing to write unclipped output.")

    boundary_ll = shapely.from_wkb(boundary_wkb)
    boundary = shapely.make_valid(shp_transform(lambda x, y, *z: WGS84_TO_SVY21(x, y), boundary_ll))
    report["boundary"] = {
        "source": boundary_src,
        "area_km2": round(boundary.area / 1e6, 2),
        "parts": len(boundary.geoms) if hasattr(boundary, "geoms") else 1,
    }
    shapely.prepare(boundary)

    if out_db.exists():
        out_db.unlink()
    out_db.parent.mkdir(parents=True, exist_ok=True)
    con = sqlite3.connect(out_db)
    con.execute("""
        CREATE TABLE buildings (
            osm_id TEXT PRIMARY KEY,
            building_tag TEXT NOT NULL,
            footprint_area_m2 REAL NOT NULL,
            height_m REAL NOT NULL,
            height_rule TEXT NOT NULL,
            height_confidence TEXT NOT NULL,
            cell_x INTEGER NOT NULL,
            cell_z INTEGER NOT NULL,
            centroid_e REAL NOT NULL,
            centroid_n REAL NOT NULL,
            footprint_svy21_wkb BLOB NOT NULL
        )""")

    rule_counts: Counter = Counter()
    conf_counts: Counter = Counter()
    type_counts: Counter = Counter()
    height_stats: list[float] = []
    cell_set: set[tuple[int, int]] = set()

    for osm_id, tags, wkb in buildings:
        counts["building_areas_considered"] += 1
        geom_ll = shapely.from_wkb(wkb)
        geom = shapely.make_valid(shp_transform(lambda x, y, *z: WGS84_TO_SVY21(x, y), geom_ll))
        polys = [g for g in getattr(geom, "geoms", [geom]) if isinstance(g, Polygon) and not g.is_empty]
        if not polys:
            counts["dropped_not_polygon"] += 1
            continue
        geom = MultiPolygon(polys) if len(polys) > 1 else polys[0]
        if geom.area < MIN_FOOTPRINT_M2:
            counts["dropped_sliver"] += 1
            continue
        rep = geom.representative_point()
        if not boundary.contains(rep):
            counts["dropped_outside_singapore"] += 1
            continue

        height, rule, conf = estimate_height(tags)
        if height > MAX_PLAUSIBLE_HEIGHT_M:
            counts["height_clamped"] += 1
            height = MAX_PLAUSIBLE_HEIGHT_M
        c = geom.centroid
        ci = svy21.cell_index(c.x, c.y)
        cell_set.add((ci.cell_x, ci.cell_z))
        btag = tags.get("building", "yes")

        con.execute(
            "INSERT INTO buildings VALUES (?,?,?,?,?,?,?,?,?,?,?)",
            (osm_id, btag, round(geom.area, 2), round(height, 2), rule, conf,
             ci.cell_x, ci.cell_z, round(c.x, 3), round(c.y, 3), geom.wkb),
        )
        counts["kept"] += 1
        rule_counts[rule] += 1
        conf_counts[conf] += 1
        type_counts[btag] += 1
        height_stats.append(height)

    con.commit()
    con.close()

    heights = sorted(height_stats)

    def pct(p: float) -> float:
        return round(heights[min(len(heights) - 1, int(p * len(heights)))], 2) if heights else float("nan")

    report["counts"] = dict(counts)
    report["geometry_failures"] = dict(failed)
    report["height_rules"] = dict(rule_counts.most_common())
    report["height_confidence"] = dict(conf_counts.most_common())
    report["building_tags_kept"] = dict(type_counts.most_common(40))
    report["height_m_percentiles"] = {"p5": pct(0.05), "p50": pct(0.5), "p95": pct(0.95), "max": round(heights[-1], 2) if heights else None}
    report["macro_cells_occupied"] = len(cell_set)
    report["output"] = {"sqlite": str(out_db), "table": "buildings", "stored_in_git": False}
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(json.dumps(report, indent=2, sort_keys=True), encoding="utf-8")
    return report


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("pbf", type=Path)
    p.add_argument("--out-db", type=Path, required=True)
    p.add_argument("--report", type=Path, required=True)
    p.add_argument("--boundary-osm", type=Path, default=None, help="Full OSM relation 536780 XML export (recommended)")
    p.add_argument("--source-note", default="BBBike Singapore extract (OpenStreetMap data, ODbL)")
    args = p.parse_args(argv)
    rep = build(args.pbf, args.out_db, args.report, args.source_note, args.boundary_osm)
    print(json.dumps({k: rep[k] for k in ("counts", "height_rules", "height_m_percentiles")}, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
