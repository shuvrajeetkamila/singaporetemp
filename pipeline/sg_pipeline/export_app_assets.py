"""Stage 4: export building footprints and the surface model into compact app assets.

Inputs:
  * buildings.sqlite   (Stage 2, osm_buildings.py)  - footprints in EPSG:3414
  * singapore_dsm_svy21_30m.tif (Stage 3, cop30_terrain.py) - DSM in EPSG:3414

Outputs (written to the Android app's assets folder):
  * terrain.f32     - float32 little-endian heights, row-major, row 0 = southernmost row,
                      sampled every `--stride` cells of the 30 m grid. Nodata -> 0 m.
  * buildings.bin   - binary records, one per exterior polygon (see FORMAT below).
  * meta.json       - origin, grid size, counts, height rules and caveats for the app to show.

Coordinates are metres east/north relative to `origin_e`/`origin_n` (SVY21). The origin is the
south-west corner of the terrain grid, so both layers share one frame.

BUILDINGS.BIN FORMAT (all little-endian):
  magic   4 bytes  b"SGB1"
  count   uint32   number of records
  record: float32 base_m, float32 height_m, uint16 n, then n * (float32 x, float32 y)
          x, y are metres relative to the origin. Ring is the exterior only, no closing vertex.
          base_m is the lowest DSM value under the footprint vertices (estimated ground).

Holes inside building footprints are ignored (the footprint is filled); counted in meta.json.
"""

from __future__ import annotations

import argparse
import datetime as _dt
import json
import struct
import sqlite3
import sys
from pathlib import Path

import numpy as np
import rasterio
import shapely
from shapely.geometry import MultiPolygon, Polygon

MAX_VERTICES = 65535


def load_terrain(tif: Path, stride: int):
    with rasterio.open(tif) as ds:
        arr = ds.read(1).astype(np.float64)
        nodata = ds.nodata
        if nodata is not None:
            arr[arr == nodata] = np.nan
        arr[arr == -9999.0] = np.nan
        t = ds.transform
        cell = t.a  # metres per cell (square grid)
        left, top = t.c, t.f
        height, width = ds.height, ds.width
    return arr, cell, left, top, height, width


def sample_dsm(full: np.ndarray, left: float, top: float, cell: float, xs: np.ndarray, ys: np.ndarray) -> np.ndarray:
    """Nearest-cell DSM value at SVY21 coordinates (xs, ys). Out-of-grid -> NaN."""
    col = np.floor((xs - left) / cell).astype(np.int64)
    row = np.floor((top - ys) / cell).astype(np.int64)
    h, w = full.shape
    ok = (row >= 0) & (row < h) & (col >= 0) & (col < w)
    out = np.full(xs.shape, np.nan)
    out[ok] = full[row[ok], col[ok]]
    return out


def export(sqlite_path: Path, tif: Path, out_dir: Path, stride: int) -> dict:
    full, cell, left, top, th, tw = load_terrain(tif, stride)
    out_dir.mkdir(parents=True, exist_ok=True)

    # --- terrain: flip so row 0 is the south edge, then subsample ---
    filled = np.where(np.isnan(full), 0.0, full)
    south_first = filled[::-1, :]
    sub = south_first[::stride, ::stride].astype("<f4")
    (out_dir / "terrain.f32").write_bytes(sub.tobytes())
    origin_e = left
    origin_n = top - th * cell  # south edge

    # --- buildings ---
    con = sqlite3.connect(sqlite_path)
    rows = con.execute("SELECT osm_id, height_m, footprint_svy21_wkb FROM buildings").fetchall()
    con.close()

    records = []
    stats = {"input_buildings": len(rows), "polygons_written": 0, "holes_ignored": 0,
             "skipped_too_many_vertices": 0, "skipped_empty": 0}
    heights = []
    for _osm_id, height_m, wkb in rows:
        geom = shapely.from_wkb(bytes(wkb))
        polys = list(geom.geoms) if isinstance(geom, MultiPolygon) else [geom]
        for p in polys:
            if not isinstance(p, Polygon) or p.is_empty:
                stats["skipped_empty"] += 1
                continue
            if p.interiors:
                stats["holes_ignored"] += len(p.interiors)
            ring = np.asarray(p.exterior.coords)[:-1]  # drop closing vertex
            if len(ring) < 3:
                stats["skipped_empty"] += 1
                continue
            if len(ring) > MAX_VERTICES:
                stats["skipped_too_many_vertices"] += 1
                continue
            xs = ring[:, 0]
            ys = ring[:, 1]
            dsm = sample_dsm(full, left, top, cell, xs, ys)
            base = float(np.nanmin(dsm)) if np.isfinite(dsm).any() else 0.0
            records.append((base, float(height_m), xs - origin_e, ys - origin_n))
            heights.append(float(height_m))
            stats["polygons_written"] += 1

    bin_path = out_dir / "buildings.bin"
    with bin_path.open("wb") as f:
        f.write(b"SGB1")
        f.write(struct.pack("<I", len(records)))
        for base, h, xs, ys in records:
            n = len(xs)
            f.write(struct.pack("<ffH", base, h, n))
            f.write(np.column_stack([xs, ys]).astype("<f4").tobytes())

    hs = np.sort(np.asarray(heights)) if heights else np.zeros(1)
    meta = {
        "schema_version": 1,
        "generated_utc": _dt.datetime.now(_dt.timezone.utc).isoformat(timespec="seconds"),
        "crs": "EPSG:3414 (SVY21), metres relative to origin",
        "origin_e": round(origin_e, 3),
        "origin_n": round(origin_n, 3),
        "terrain": {"file": "terrain.f32", "width": int(sub.shape[1]), "height": int(sub.shape[0]),
                    "cell_m": cell * stride, "row_order": "south_to_north", "dtype": "float32le",
                    "source": "Copernicus GLO-30 DSM (surface model: includes buildings and trees)"},
        "buildings": {"file": "buildings.bin", "records": len(records), **stats,
                      "height_m_p50": round(float(np.percentile(hs, 50)), 2),
                      "height_m_p95": round(float(np.percentile(hs, 95)), 2),
                      "height_m_max": round(float(hs.max()), 2),
                      "heights": "ESTIMATED from OSM tags and building-type defaults (see Stage 2 report)"},
        "attribution": [
            "Building footprints: OpenStreetMap contributors (ODbL)",
            "Elevation: Copernicus DEM GLO-30 (c) DLR/ESA, via OpenTopography",
        ],
    }
    (out_dir / "meta.json").write_text(json.dumps(meta, indent=2, sort_keys=True), encoding="utf-8")
    return meta


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--sqlite", type=Path, required=True)
    p.add_argument("--dsm", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--stride", type=int, default=2)
    args = p.parse_args(argv)
    meta = export(args.sqlite, args.dsm, args.out, args.stride)
    print(json.dumps({"terrain": meta["terrain"], "buildings": meta["buildings"]}, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
