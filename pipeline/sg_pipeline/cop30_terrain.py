"""Stage 3 of the pipeline: Copernicus GLO-30 DSM GeoTIFF -> SVY21 (EPSG:3414) elevation grid.

Inputs:  a GeoTIFF in WGS84 (EPSG:4326) as exported from OpenTopography (Copernicus GLO-30).
Outputs: a float32 GeoTIFF in EPSG:3414 at a fixed resolution (kept outside Git), and a
         JSON report (kept inside Git) with coverage, value statistics and spot checks.

Caveats recorded in the report:
  * GLO-30 is a Digital Surface Model (includes buildings and vegetation), not bare ground.
  * Heights are referenced to the EGM2008 geoid (EPSG:3855) per the OpenTopography page.
  * 30 m source resolution; the output is a resampled grid, not a higher-resolution product.
"""

from __future__ import annotations

import argparse
import datetime as _dt
import hashlib
import json
import sys
from pathlib import Path

import numpy as np
import rasterio
from rasterio.warp import Resampling, calculate_default_transform, reproject, transform as rio_transform

TARGET_CRS = "EPSG:3414"
DEFAULT_RES_M = 30.0

# Spot checks: (name, lon, lat, expected_note). Values are compared to the raster and
# reported, not asserted, because a DSM differs from ground level over buildings and trees.
SPOT_CHECKS = [
    ("Bukit Timah Hill summit area (~163 m official)", 103.7770, 1.3520),
    ("Changi coast (near sea level)", 103.9880, 1.3340),
    ("Marina Bay (near sea level)", 103.8600, 1.2810),
    ("Tuas (low, flat)", 103.6370, 1.3160),
    ("Woodlands (low, built-up)", 103.7860, 1.4370),
    ("Kranji Reservoir (water)", 103.7330, 1.4190),
]


def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def reproject_to_svy21(src_path: Path, out_path: Path, res_m: float = DEFAULT_RES_M) -> dict:
    with rasterio.open(src_path) as src:
        if src.crs is None or src.crs.to_string() != "EPSG:4326":
            raise SystemExit(f"Expected EPSG:4326 input, got {src.crs}")
        if src.count != 1:
            raise SystemExit(f"Expected a single-band DEM, got {src.count} bands")
        transform, width, height = calculate_default_transform(
            src.crs, TARGET_CRS, src.width, src.height, *src.bounds, resolution=res_m
        )
        profile = src.profile.copy()
        profile.update(crs=TARGET_CRS, transform=transform, width=width, height=height,
                       dtype="float32", nodata=-9999.0, compress="deflate", tiled=True)
        src_nodata = src.nodata
        out_path.parent.mkdir(parents=True, exist_ok=True)
        with rasterio.open(out_path, "w", **profile) as dst:
            reproject(
                source=rasterio.band(src, 1),
                destination=rasterio.band(dst, 1),
                src_transform=src.transform,
                src_crs=src.crs,
                src_nodata=src_nodata,
                dst_transform=transform,
                dst_crs=TARGET_CRS,
                dst_nodata=-9999.0,
                resampling=Resampling.bilinear,
            )
    return {"width": width, "height": height, "resolution_m": res_m, "crs": TARGET_CRS}


def describe(src_path: Path, out_path: Path) -> dict:
    with rasterio.open(out_path) as ds:
        a = ds.read(1, masked=True)
        valid = a.compressed()
        pct = {f"p{p}": round(float(np.percentile(valid, p)), 2) for p in (1, 5, 50, 95, 99)}
        checks = []
        for name, lon, lat in SPOT_CHECKS:
            xs, ys = rio_transform("EPSG:4326", ds.crs, [lon], [lat])
            r, c = ds.index(xs[0], ys[0])
            if 0 <= r < ds.height and 0 <= c < ds.width and not a.mask[r, c]:
                checks.append({"name": name, "lon": lon, "lat": lat, "dsm_m": round(float(a[r, c]), 2)})
            else:
                checks.append({"name": name, "lon": lon, "lat": lat, "dsm_m": None})
        return {
            "valid_cells": int(valid.size),
            "total_cells": int(a.size),
            "min_m": round(float(valid.min()), 2),
            "max_m": round(float(valid.max()), 2),
            "mean_m": round(float(valid.mean()), 2),
            "percentiles_m": pct,
            "cells_below_0m": int((valid < 0).sum()),
            "bounds_svy21_m": [round(v, 1) for v in ds.bounds],
            "spot_checks": checks,
        }


def build(src_path: Path, out_path: Path, report_path: Path, res_m: float) -> dict:
    report = {
        "schema_version": 1,
        "generated_utc": _dt.datetime.now(_dt.timezone.utc).isoformat(timespec="seconds"),
        "input": {"file": src_path.name, "bytes": src_path.stat().st_size, "sha256": sha256_of(src_path)},
        "source": "Copernicus GLO-30 DSM via OpenTopography (DGED 2023_1), user-downloaded",
        "vertical_reference": "EGM2008 geoid (EPSG:3855) per OpenTopography page",
        "caveats": [
            "Digital Surface Model: includes buildings and vegetation, not bare ground",
            "30 m source resolution",
        ],
    }
    report["grid"] = reproject_to_svy21(src_path, out_path, res_m)
    report["output"] = {"file": out_path.name, "stored_in_git": False, "path": str(out_path)}
    report["stats"] = describe(src_path, out_path)
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(json.dumps(report, indent=2, sort_keys=True), encoding="utf-8")
    return report


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("tif", type=Path)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--report", type=Path, required=True)
    p.add_argument("--res", type=float, default=DEFAULT_RES_M)
    args = p.parse_args(argv)
    rep = build(args.tif, args.out, args.report, args.res)
    print(json.dumps({k: rep[k] for k in ("grid", "stats")}, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
