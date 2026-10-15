"""SVY21 / Singapore Transverse Mercator (EPSG:3414) transforms and grid helpers.

Projection work goes through pyproj (PROJ). Nothing here re-implements the
projection by hand. Coordinates are metres; X = easting, Z = northing, matching
the runtime convention in runtime/core/include/sg/global_position.hpp.
"""

from __future__ import annotations

import math
from dataclasses import dataclass

from pyproj import CRS, Transformer

EPSG_SVY21 = 3414
EPSG_WGS84 = 4326

# Macro-cell size used by the runtime floating-origin system (metres).
CELL_SIZE_M = 512.0

# SVY21 grid origin as defined by the Singapore Land Authority:
# latitude of origin 1°22′N, central meridian 103°50′E,
# false easting 28001.642 m, false northing 38744.572 m.
ORIGIN_LAT_DEG = 1.0 + 22.0 / 60.0
ORIGIN_LON_DEG = 103.0 + 50.0 / 60.0
ORIGIN_EASTING_M = 28001.642
ORIGIN_NORTHING_M = 38744.572

_to_svy21 = Transformer.from_crs(EPSG_WGS84, EPSG_SVY21, always_xy=True)
_to_wgs84 = Transformer.from_crs(EPSG_SVY21, EPSG_WGS84, always_xy=True)


def crs_name() -> str:
    """Human-readable name of the CRS as known to PROJ (used in the build manifest)."""
    return CRS.from_epsg(EPSG_SVY21).name


def wgs84_to_svy21(lon_deg: float, lat_deg: float) -> tuple[float, float]:
    """Return (easting, northing) in metres for a WGS84 longitude/latitude."""
    if not (math.isfinite(lon_deg) and math.isfinite(lat_deg)):
        raise ValueError("non-finite WGS84 coordinate")
    e, n = _to_svy21.transform(lon_deg, lat_deg)
    if not (math.isfinite(e) and math.isfinite(n)):
        raise ValueError("projection produced non-finite result")
    return float(e), float(n)


def svy21_to_wgs84(easting_m: float, northing_m: float) -> tuple[float, float]:
    """Return (longitude, latitude) in degrees for SVY21 metres."""
    if not (math.isfinite(easting_m) and math.isfinite(northing_m)):
        raise ValueError("non-finite SVY21 coordinate")
    lon, lat = _to_wgs84.transform(easting_m, northing_m)
    return float(lon), float(lat)


@dataclass(frozen=True)
class CellIndex:
    """Canonical macro-cell index plus local offset in [0, CELL_SIZE_M)."""

    cell_x: int
    cell_z: int
    local_x: float
    local_z: float


def cell_index(easting_m: float, northing_m: float) -> CellIndex:
    """Floor-based macro-cell split. Mirrors GlobalPosition::FromProjected in C++."""
    if not (math.isfinite(easting_m) and math.isfinite(northing_m)):
        raise ValueError("non-finite coordinate")
    qx = math.floor(easting_m / CELL_SIZE_M)
    qz = math.floor(northing_m / CELL_SIZE_M)
    rx = easting_m - qx * CELL_SIZE_M
    rz = northing_m - qz * CELL_SIZE_M
    # Rounding guard, identical to the C++ implementation.
    if rx >= CELL_SIZE_M:
        rx -= CELL_SIZE_M
        qx += 1
    if rz >= CELL_SIZE_M:
        rz -= CELL_SIZE_M
        qz += 1
    if rx < 0.0:
        rx += CELL_SIZE_M
        qx -= 1
    if rz < 0.0:
        rz += CELL_SIZE_M
        qz -= 1
    return CellIndex(int(qx), int(qz), rx, rz)
