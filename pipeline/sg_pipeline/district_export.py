"""Stage 5: export one district (pilot) for the native Vulkan app.

Inputs:
  * the Singapore OSM extract (PBF) - roads and footpaths
  * buildings.sqlite (Stage 2)      - footprints and estimated heights
  * singapore_dsm_svy21_30m.tif     - surface model at 30 m (Stage 3), used at full resolution

The district rectangle is given in WGS84 (west south east north) and converted to SVY21.
The grid is aligned to the 30 m surface model so terrain is not resampled.

Outputs (binary, little-endian; origin = south-west corner of the terrain crop):
  terrain.bin   "SGT1", uint32 width, uint32 height, float32 cell_m, then float32 heights,
                row-major, row 0 = southernmost row
  buildings.bin "SGB1", uint32 count, then per record: float32 base, float32 height,
                uint16 n, n * (float32 x, float32 y)                    (same as Stage 4)
  roads.bin     "SGR1", uint32 nodes, uint32 edges, then nodes (float32 x, float32 y), then edges:
                uint32 a, uint32 b, uint8 mode (0 vehicle, 1 foot), uint8 cls,
                uint16 npts, float32 speed_mps, npts * (float32 x, float32 y)
                cls: 0 motorway, 1 trunk, 2 primary, 3 secondary, 4 tertiary,
                     5 residential/service/other road, 6 footway/path/pedestrian
  meta.json     origin, sizes, counts, speeds, caveats, attribution

Roads are kept if any point lies within the district plus a 200 m margin, so they are not
cut at the edge. Terrain and buildings are cut exactly to the district.
"""

from __future__ import annotations

import argparse
import datetime as _dt
import json
import math
import sqlite3
import struct
import sys
from collections import Counter
from pathlib import Path

import numpy as np
import osmium
import rasterio
import shapely
from pyproj import Transformer
from shapely.geometry import LineString, MultiPolygon, Point, Polygon, box as shp_box

from sg_pipeline.export_app_assets import sample_dsm

WGS84_TO_SVY21 = Transformer.from_crs(4326, 3414, always_xy=True).transform
MARGIN_M = 200.0

VEHICLE_CLASS = {
    "motorway": 0, "motorway_link": 0,
    "trunk": 1, "trunk_link": 1,
    "primary": 2, "primary_link": 2,
    "secondary": 3, "secondary_link": 3,
    "tertiary": 4, "tertiary_link": 4,
    "residential": 5, "unclassified": 5, "service": 5, "living_street": 5,
}
FOOT_CLASS = {"footway": 6, "pedestrian": 6, "path": 6}
# Default speeds in m/s when no usable maxspeed tag is present.
DEFAULT_SPEED = {0: 22.2, 1: 19.4, 2: 16.7, 3: 13.9, 4: 11.1, 5: 8.3, 6: 1.3}


def parse_maxspeed(value: str | None) -> float | None:
    if not value:
        return None
    try:
        kmh = float(value.split()[0])
    except ValueError:
        return None
    if 5 <= kmh <= 110:
        return kmh / 3.6
    return None


class RoadReader(osmium.SimpleHandler):
    def __init__(self, bbox_ll):
        super().__init__()
        self.w, self.s, self.e, self.n = bbox_ll
        self.ways = []  # (mode, cls, speed, refs, [(x,y) svy21])
        self.count = Counter()

    def way(self, w):
        tags = w.tags
        hw = tags.get("highway")
        if hw is None:
            return
        if hw in VEHICLE_CLASS:
            mode, cls = 0, VEHICLE_CLASS[hw]
        elif hw in FOOT_CLASS:
            mode, cls = 1, FOOT_CLASS[hw]
        else:
            return
        pts = []
        refs = []
        for nd in w.nodes:
            if not nd.location.valid():
                return
            refs.append(nd.ref)
            pts.append((nd.location.lon, nd.location.lat))
        if len(refs) < 2:
            return
        if not any(self.w - 0.01 <= lon <= self.e + 0.01 and self.s - 0.01 <= lat <= self.n + 0.01 for lon, lat in pts):
            return
        speed = parse_maxspeed(tags.get("maxspeed")) if mode == 0 else None
        speed = speed or DEFAULT_SPEED[cls]
        svy = [WGS84_TO_SVY21(lon, lat) for lon, lat in pts]
        self.ways.append((mode, cls, speed, refs, svy))
        self.count.update(set(refs))


def build_edges(ways, mode: int):
    """Split ways at junctions (nodes shared by 2+ ways of this mode, or way ends)."""
    selected = [w for w in ways if w[0] == mode]
    shared = Counter()
    for _m, _c, _s, refs, _p in selected:
        shared.update(set(refs))
    edges = []  # (a_ref, b_ref, cls, speed, pts)
    node_pos = {}
    for _m, cls, speed, refs, pts in selected:
        last = len(refs) - 1
        cuts = [i for i, r in enumerate(refs) if i in (0, last) or shared[r] > 1]
        for s, e in zip(cuts, cuts[1:]):
            seg = pts[s:e + 1]
            if len(seg) < 2:
                continue
            node_pos.setdefault(refs[s], seg[0])
            node_pos.setdefault(refs[e], seg[-1])
            edges.append((refs[s], refs[e], cls, speed, seg, mode))
    return edges, node_pos


def in_rect(p, rect):
    return rect[0] <= p[0] <= rect[2] and rect[1] <= p[1] <= rect[3]


def _clip_edge(a, b, cls, speed, seg, mode, box_geom, edge_id, out):
    """Keep the parts of an edge inside the box. Cut ends become new dead-end nodes."""
    if all(box_geom.covers(Point(p)) for p in seg):
        out.append((a, b, cls, speed, seg, mode))
        return
    inter = LineString(seg).intersection(box_geom)
    if inter.is_empty:
        return
    parts = list(inter.geoms) if hasattr(inter, "geoms") else [inter]
    for k, part in enumerate(parts):
        if part.geom_type != "LineString" or len(part.coords) < 2:
            continue
        pts = [tuple(c) for c in part.coords]
        start = a if math.dist(pts[0], seg[0]) < 1e-6 else ("cut", edge_id, k, 0)
        end = b if math.dist(pts[-1], seg[-1]) < 1e-6 else ("cut", edge_id, k, 1)
        out.append((start, end, cls, speed, pts, mode))


def export_roads(pbf: Path, rect_ll, rect_svy, origin_e, origin_n, out_path: Path) -> dict:
    reader = RoadReader(rect_ll)
    reader.apply_file(str(pbf), locations=True)
    box_geom = shp_box(rect_svy[0] - MARGIN_M, rect_svy[1] - MARGIN_M,
                       rect_svy[2] + MARGIN_M, rect_svy[3] + MARGIN_M)
    all_edges = []
    node_pos = {}
    edge_id = 0
    for mode in (0, 1):
        edges, pos = build_edges(reader.ways, mode)
        clipped = []
        for a, b, cls, speed, seg, m in edges:
            edge_id += 1
            _clip_edge(a, b, cls, speed, seg, m, box_geom, edge_id, clipped)
        for a, b, cls, speed, seg, m in clipped:
            all_edges.append((a, b, cls, speed, seg, m))
            node_pos.setdefault(a, pos[a] if not isinstance(a, tuple) else seg[0])
            node_pos.setdefault(b, pos[b] if not isinstance(b, tuple) else seg[-1])
    index = {}
    nodes = []
    for ref, p in node_pos.items():
        index[ref] = len(nodes)
        nodes.append(p)
    with out_path.open("wb") as f:
        f.write(b"SGR1")
        f.write(struct.pack("<II", len(nodes), len(all_edges)))
        for x, y in nodes:
            f.write(struct.pack("<ff", x - origin_e, y - origin_n))
        for a, b, cls, speed, seg, mode in all_edges:
            f.write(struct.pack("<IIBBHf", index[a], index[b], mode, cls, len(seg), speed))
            arr = np.array([[x - origin_e, y - origin_n] for x, y in seg], dtype="<f4")
            f.write(arr.tobytes())
    by_class = Counter(e[2] for e in all_edges)
    total_len = sum(sum(math.dist(seg[i], seg[i + 1]) for i in range(len(seg) - 1)) for *_r, seg, _m in all_edges)
    return {
        "nodes": len(nodes),
        "edges": len(all_edges),
        "edges_vehicle": sum(1 for e in all_edges if e[5] == 0),
        "edges_foot": sum(1 for e in all_edges if e[5] == 1),
        "edges_by_class": {str(k): v for k, v in sorted(by_class.items())},
        "total_length_km": round(total_len / 1000, 2),
        "ways_read": len(reader.ways),
    }


def export_district(pbf: Path, sqlite_path: Path, dsm: Path, rect_ll, out_dir: Path) -> dict:
    out_dir.mkdir(parents=True, exist_ok=True)
    w, s, e, n = rect_ll
    e0, n0 = WGS84_TO_SVY21(w, s)
    e1, n1 = WGS84_TO_SVY21(e, n)
    with rasterio.open(dsm) as ds:
        arr = ds.read(1).astype(np.float64)
        arr[(arr == -9999.0)] = np.nan
        t = ds.transform
        cell = t.a
        left, top = t.c, t.f
        H, W = arr.shape
    c0 = max(0, int(math.floor((e0 - left) / cell)))
    c1 = min(W, int(math.ceil((e1 - left) / cell)))
    r0 = max(0, int(math.floor((top - n1) / cell)))
    r1 = min(H, int(math.ceil((top - n0) / cell)))
    crop = arr[r0:r1, c0:c1]
    origin_e = left + c0 * cell
    origin_n = top - r1 * cell
    rect_svy = (origin_e, origin_n, origin_e + (c1 - c0) * cell, origin_n + (r1 - r0) * cell)

    # terrain: flip to south-first, nodata -> 0; header "SGT1", uint32 width, uint32 height, float32 cell
    filled = np.where(np.isnan(crop), 0.0, crop)[::-1, :].astype("<f4")
    with (out_dir / "terrain.bin").open("wb") as f:
        f.write(b"SGT1")
        f.write(struct.pack("<IIf", int(filled.shape[1]), int(filled.shape[0]), float(cell)))
        f.write(filled.tobytes())

    # buildings (full resolution, footprints inside the district)
    con = sqlite3.connect(sqlite_path)
    rows = con.execute("SELECT osm_id, height_m, footprint_svy21_wkb FROM buildings").fetchall()
    con.close()
    full_for_sampling = arr
    rect_poly = shapely.box(*rect_svy)
    records = []
    for _id, height_m, wkb in rows:
        geom = shapely.from_wkb(bytes(wkb))
        if not rect_poly.intersects(geom):
            continue
        for p in (list(geom.geoms) if isinstance(geom, MultiPolygon) else [geom]):
            if not isinstance(p, Polygon) or p.is_empty or not rect_poly.contains(p.representative_point()):
                continue
            ring = np.asarray(p.exterior.coords)[:-1]
            if len(ring) < 3:
                continue
            xs, ys = ring[:, 0], ring[:, 1]
            dsm_vals = sample_dsm(full_for_sampling, left, top, cell, xs, ys)
            base = float(np.nanmin(dsm_vals)) if np.isfinite(dsm_vals).any() else 0.0
            records.append((base, float(height_m), xs - origin_e, ys - origin_n))
    with (out_dir / "buildings.bin").open("wb") as f:
        f.write(b"SGB1")
        f.write(struct.pack("<I", len(records)))
        for base, h, xs, ys in records:
            f.write(struct.pack("<ffH", base, h, len(xs)))
            f.write(np.column_stack([xs, ys]).astype("<f4").tobytes())

    roads = export_roads(pbf, rect_ll, rect_svy, origin_e, origin_n, out_dir / "roads.bin")

    hs = np.sort(np.asarray([r[1] for r in records])) if records else np.zeros(1)
    meta = {
        "schema_version": 1,
        "generated_utc": _dt.datetime.now(_dt.timezone.utc).isoformat(timespec="seconds"),
        "district": {"name": "Marina Bay (pilot)", "wgs84_west_south_east_north": list(rect_ll)},
        "crs": "EPSG:3414 (SVY21), metres relative to origin",
        "origin_e": round(origin_e, 3),
        "origin_n": round(origin_n, 3),
        "size_m": [round((c1 - c0) * cell, 1), round((r1 - r0) * cell, 1)],
        "terrain": {"file": "terrain.bin", "width": int(filled.shape[1]), "height": int(filled.shape[0]),
                    "cell_m": cell, "row_order": "south_to_north", "dtype": "float32le",
                    "source": "Copernicus GLO-30 DSM at full 30 m resolution (surface model: buildings and trees included)"},
        "buildings": {"file": "buildings.bin", "records": len(records),
                      "height_m_p50": round(float(np.percentile(hs, 50)), 2),
                      "height_m_max": round(float(hs.max()), 2),
                      "heights": "ESTIMATED from OSM tags and building-type defaults"},
        "roads": {"file": "roads.bin", **roads,
                  "vehicle_speeds": "maxspeed tag where present, otherwise class defaults",
                  "foot_speed_mps": DEFAULT_SPEED[6]},
        "attribution": [
            "Building and road data: OpenStreetMap contributors (ODbL)",
            "Elevation: Copernicus DEM GLO-30 (c) DLR/ESA, via OpenTopography",
        ],
    }
    (out_dir / "meta.json").write_text(json.dumps(meta, indent=2, sort_keys=True), encoding="utf-8")
    return meta


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("pbf", type=Path)
    p.add_argument("--sqlite", type=Path, required=True)
    p.add_argument("--dsm", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--bbox", type=float, nargs=4, metavar=("WEST", "SOUTH", "EAST", "NORTH"),
                   default=[103.8480, 1.2740, 103.8700, 1.2930], help="Marina Bay (pilot) by default")
    args = p.parse_args(argv)
    meta = export_district(args.pbf, args.sqlite, args.dsm, tuple(args.bbox), args.out)
    print(json.dumps({"size_m": meta["size_m"], "terrain": [meta["terrain"]["width"], meta["terrain"]["height"]],
                      "buildings": meta["buildings"]["records"], "roads": {k: meta["roads"][k] for k in
                      ("nodes", "edges", "edges_vehicle", "edges_foot", "total_length_km")}}, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
