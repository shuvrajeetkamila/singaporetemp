import math

import pytest

from sg_pipeline import svy21

# Approximate WGS84 locations (lon, lat). Used as test fixtures only, not survey values.
FIXTURES = {
    "west_tuas_approx": (103.6200, 1.2700),
    "east_changi_approx": (103.9915, 1.3644),
    "north_woodlands_approx": (103.7700, 1.4500),
    "south_sentosa_approx": (103.8303, 1.2494),
    "bukit_timah_summit_approx": (103.7770, 1.3529),
    "merlion_approx": (103.8545, 1.2868),
    "pulau_ubin_approx": (103.9600, 1.4050),
    "jurong_island_approx": (103.7000, 1.2600),
}


def test_crs_is_svy21():
    assert "SVY21" in svy21.crs_name()


def test_grid_origin_matches_sla_definition():
    # Exact 1°22′N 103°50′E must map to the published false easting/northing.
    e, n = svy21.wgs84_to_svy21(svy21.ORIGIN_LON_DEG, svy21.ORIGIN_LAT_DEG)
    assert e == pytest.approx(svy21.ORIGIN_EASTING_M, abs=1e-6)
    assert n == pytest.approx(svy21.ORIGIN_NORTHING_M, abs=1e-6)


def test_decimal_rounding_of_origin_is_visible():
    # Rounding 1°22′ to 1.3666667° moves the point by about 4 mm; this documents
    # why coordinates must be stored at full precision.
    e, n = svy21.wgs84_to_svy21(103.8333333, 1.3666667)
    assert math.hypot(e - svy21.ORIGIN_EASTING_M, n - svy21.ORIGIN_NORTHING_M) == pytest.approx(0.0055, abs=0.002)


@pytest.mark.parametrize("name", FIXTURES)
def test_round_trip_is_sub_millimetre(name):
    lon, lat = FIXTURES[name]
    e, n = svy21.wgs84_to_svy21(lon, lat)
    lon2, lat2 = svy21.svy21_to_wgs84(e, n)
    assert lon2 == pytest.approx(lon, abs=1e-9)
    assert lat2 == pytest.approx(lat, abs=1e-9)


def test_relative_placement_is_plausible():
    p = {k: svy21.wgs84_to_svy21(*v) for k, v in FIXTURES.items()}
    # Tuas is west of the Merlion, which is west of Changi.
    assert p["west_tuas_approx"][0] < p["merlion_approx"][0] < p["east_changi_approx"][0]
    # Woodlands is north of the Merlion; Sentosa is south of it.
    assert p["north_woodlands_approx"][1] > p["merlion_approx"][1] > p["south_sentosa_approx"][1]
    # Pulau Ubin lies north-east of the Merlion.
    assert p["pulau_ubin_approx"][0] > p["merlion_approx"][0]
    assert p["pulau_ubin_approx"][1] > p["merlion_approx"][1]


def test_non_finite_input_rejected():
    with pytest.raises(ValueError):
        svy21.wgs84_to_svy21(float("nan"), 1.3)
    with pytest.raises(ValueError):
        svy21.svy21_to_wgs84(float("inf"), 1.0)


@pytest.mark.parametrize(
    "e,n",
    [(0.0, 0.0), (511.9999999, 0.0), (512.0, 512.0), (-1.0, -1.0), (-512.0, 0.0), (-512.0000001, 0.0), (-1e-300, 0.0)],
)
def test_cell_index_is_floor_based(e, n):
    c = svy21.cell_index(e, n)
    assert 0.0 <= c.local_x < svy21.CELL_SIZE_M
    assert 0.0 <= c.local_z < svy21.CELL_SIZE_M
    assert c.cell_x * svy21.CELL_SIZE_M + c.local_x == pytest.approx(e, abs=1e-9)
    assert c.cell_z * svy21.CELL_SIZE_M + c.local_z == pytest.approx(n, abs=1e-9)


def test_cell_index_known_values():
    assert svy21.cell_index(-1.0, -1.0).cell_x == -1
    assert svy21.cell_index(-512.0, 0.0).cell_x == -1
    assert svy21.cell_index(-512.0000001, 0.0).cell_x == -2
    # -1e-300 is numerically zero: the rounding guard moves it into cell 0 at local 0,
    # the same canonical form the C++ GlobalPosition::Normalize produces.
    c = svy21.cell_index(-1e-300, 0.0)
    assert (c.cell_x, c.local_x) == (0, 0.0)


def test_cell_index_rejects_nan():
    with pytest.raises(ValueError):
        svy21.cell_index(float("nan"), 0.0)
