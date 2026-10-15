"""Packs the Stage 4 island export into the Vulkan app's island assets.

Inputs (Stage 4 output folder, produced by sg_pipeline.export_app_assets --stride 2):
  meta.json, terrain.f32 (float32le, row 0 = south), buildings.bin (SGB1)
Plus the pilot district meta.json (android-vk/app/src/main/assets/district/meta.json).

Outputs (android-vk/app/src/main/assets/island/):
  terrain.bin   "SGT1", uint32 width, uint32 height, float32 cell_m, then float32 heights
  buildings.bin copied unchanged (SGB1)
  layout.txt   "dx dn sx sz": pilot origin minus island origin (metres, SVY21 easting and northing),
               and the pilot size. The app cuts the pilot rectangle out of the island layer.
  meta.json     copy of the island metadata (attribution and caveats)
"""

import argparse
import json
import shutil
import struct
import sys
from pathlib import Path


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("island_dir", type=Path)
    ap.add_argument("--pilot-meta", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()

    meta = json.loads((args.island_dir / "meta.json").read_text())
    t = meta["terrain"]
    if t.get("dtype") != "float32le" or t.get("row_order") != "south_to_north":
        print("unexpected terrain format in island meta", file=sys.stderr)
        return 2
    w, h, cell = int(t["width"]), int(t["height"]), float(t["cell_m"])
    raw = (args.island_dir / t["file"]).read_bytes()
    if len(raw) != w * h * 4:
        print(f"terrain size mismatch: {len(raw)} bytes, expected {w * h * 4}", file=sys.stderr)
        return 2

    pilot = json.loads(args.pilot_meta.read_text())
    dx = float(pilot["origin_e"]) - float(meta["origin_e"])
    dn = float(pilot["origin_n"]) - float(meta["origin_n"])
    # Pilot terrain mesh extent: grid points are (n - 1) cells apart. The island cut-out must cover this.
    pt = pilot["terrain"]
    sx = (int(pt["width"]) - 1) * float(pt["cell_m"])
    sz = (int(pt["height"]) - 1) * float(pt["cell_m"])

    bld = (args.island_dir / meta["buildings"]["file"]).read_bytes()
    if bld[:4] != b"SGB1":
        print("island buildings.bin has the wrong magic", file=sys.stderr)
        return 2

    args.out.mkdir(parents=True, exist_ok=True)
    with (args.out / "terrain.bin").open("wb") as f:
        f.write(b"SGT1")
        f.write(struct.pack("<IIf", w, h, cell))
        f.write(raw)
    (args.out / "buildings.bin").write_bytes(bld)
    (args.out / "layout.txt").write_text(f"{dx:.3f} {dn:.3f} {sx:.3f} {sz:.3f}\n")
    shutil.copy(args.island_dir / "meta.json", args.out / "meta.json")

    print(f"island terrain {w}x{h} at {cell} m; pilot offset dx={dx:.3f} dn={dn:.3f}; pilot terrain {sx}x{sz} m")
    print(f"buildings.bin {len(bld)} bytes; terrain.bin {12 + len(raw)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
