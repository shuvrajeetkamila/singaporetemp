#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>

#include "sg/global_position.hpp"
#include "test_common.hpp"

using sg::GlobalPosition;
using sg::GeoStatus;

namespace {

// SVY21 test fixtures. Computed with pyproj (EPSG:4326 -> EPSG:3414) from
// approximate WGS84 coordinates. They are test inputs only, not survey values.
struct Fixture {
    const char* name;
    double easting;
    double northing;
};

constexpr Fixture kFixtures[] = {
    {"west_tuas_approx", 4259.223, 28056.648},
    {"east_changi_approx", 45603.739, 38494.514},
    {"north_woodlands_approx", 20953.649, 47959.249},
    {"south_sentosa_approx", 27664.053, 25777.828},
    {"bukit_timah_summit_approx", 21732.379, 37222.397},
    {"merlion_approx", 30357.314, 29913.335},
    {"pulau_ubin_approx", 42097.911, 42983.660},
    {"jurong_island_approx", 13162.595, 26950.300},
};

void TestNormalizeFloorSemantics() {
    // Negative coordinates must floor toward -infinity, not truncate toward zero.
    auto a = GlobalPosition::FromProjected(-1.0, -1.0, 0.0);
    SG_CHECK(a.has_value());
    if (a) {
        SG_CHECK(a->cellX == -1 && a->cellZ == -1);
        SG_CHECK_NEAR(a->localOffset.x, 511.0, 0.0);
        SG_CHECK_NEAR(a->localOffset.z, 511.0, 0.0);
    }

    auto b = GlobalPosition::FromProjected(-512.0, 0.0, 0.0);
    SG_CHECK(b.has_value());
    if (b) {
        SG_CHECK(b->cellX == -1);
        SG_CHECK_NEAR(b->localOffset.x, 0.0, 0.0);
    }

    auto c = GlobalPosition::FromProjected(-512.0000001, 0.0, 0.0);
    SG_CHECK(c.has_value());
    if (c) {
        SG_CHECK(c->cellX == -2);
    }

    // Values just below a boundary stay in the lower cell.
    auto d = GlobalPosition::FromProjected(511.9999999, 0.0, 0.0);
    SG_CHECK(d.has_value());
    if (d) {
        SG_CHECK(d->cellX == 0);
        SG_CHECK(d->localOffset.x < sg::kCellSize);
    }

    // A tiny negative value whose remainder rounds to exactly kCellSize must be pulled back.
    auto e = GlobalPosition::FromProjected(-1e-300, 0.0, 0.0);
    SG_CHECK(e.has_value());
    if (e) {
        SG_CHECK(e->localOffset.x >= 0.0 && e->localOffset.x < sg::kCellSize);
    }
}

void TestNormalizeRejectsBadInput() {
    SG_CHECK(!GlobalPosition::FromProjected(NAN, 0.0, 0.0).has_value());
    SG_CHECK(!GlobalPosition::FromProjected(0.0, INFINITY, 0.0).has_value());
    SG_CHECK(!GlobalPosition::FromProjected(1e300, 0.0, 0.0).has_value());

    // Cell overflow must fail and leave the value unchanged.
    GlobalPosition p;
    p.cellX = sg::kMaxCellMagnitude;
    p.localOffset = sg::Vec3d{1024.0, 0.0, 0.0};
    const GlobalPosition before = p;
    SG_CHECK(p.Normalize() == GeoStatus::CellOverflow);
    SG_CHECK(p == before);

    // Checked int64 path: a cell at INT64_MAX must not wrap.
    GlobalPosition q;
    q.cellX = INT64_MAX;
    q.localOffset = sg::Vec3d{600.0, 0.0, 0.0};
    SG_CHECK(q.Normalize() == GeoStatus::CellOverflow);

    GlobalPosition nonFinite;
    nonFinite.localOffset.z = NAN;
    SG_CHECK(nonFinite.Normalize() == GeoStatus::NonFinite);
}

void TestFixturesRoundTripAndDelta() {
    for (const auto& f : kFixtures) {
        auto p = GlobalPosition::FromProjected(f.easting, f.northing, 0.0);
        SG_CHECK(p.has_value());
        if (!p) continue;
        SG_CHECK(p->localOffset.x >= 0.0 && p->localOffset.x < sg::kCellSize);
        SG_CHECK(p->localOffset.z >= 0.0 && p->localOffset.z < sg::kCellSize);

        // Reconstructing easting/northing from the canonical form must match the input.
        const double e = static_cast<double>(p->cellX) * sg::kCellSize + p->localOffset.x;
        const double n = static_cast<double>(p->cellZ) * sg::kCellSize + p->localOffset.z;
        SG_CHECK_NEAR(e, f.easting, 1e-9);
        SG_CHECK_NEAR(n, f.northing, 1e-9);
    }

    // DeltaMeters between fixtures matches plain double subtraction of projected coordinates.
    auto a = GlobalPosition::FromProjected(kFixtures[0].easting, kFixtures[0].northing, 0.0);
    auto b = GlobalPosition::FromProjected(kFixtures[1].easting, kFixtures[1].northing, 0.0);
    SG_CHECK(a.has_value() && b.has_value());
    if (a && b) {
        auto d = GlobalPosition::DeltaMeters(*a, *b);
        SG_CHECK(d.has_value());
        if (d) {
            SG_CHECK_NEAR(d->x, kFixtures[1].easting - kFixtures[0].easting, 1e-6);
            SG_CHECK_NEAR(d->z, kFixtures[1].northing - kFixtures[0].northing, 1e-6);
        }
    }
}

void TestFloat32PrecisionWithinActiveWindow() {
    // Local-only precision: error from float32 conversion must be at most half an ulp
    // of a value below 512 m, which is ~3.05e-5 m (0.031 mm).
    std::mt19937_64 rng(0x5EEDu);
    std::uniform_real_distribution<double> local(0.0, sg::kCellSize);
    std::uniform_int_distribution<int64_t> delta(-sg::kMaxActiveCellDelta, sg::kMaxActiveCellDelta);

    double maxLocalErr = 0.0;
    double maxWindowErr = 0.0;
    for (int i = 0; i < 200000; ++i) {
        GlobalPosition p;
        p.cellX = delta(rng);
        p.cellZ = delta(rng);
        p.localOffset = sg::Vec3d{local(rng), 0.0, local(rng)};

        auto r = p.ToRenderSpace(0, 0);
        SG_CHECK(r.has_value());
        if (!r) continue;

        // Exact reference: the double result before float conversion.
        const double exactX = static_cast<double>(p.cellX) * sg::kCellSize + p.localOffset.x;
        const double exactZ = static_cast<double>(p.cellZ) * sg::kCellSize + p.localOffset.z;
        const double errX = std::fabs(static_cast<double>(r->x) - exactX);
        const double errZ = std::fabs(static_cast<double>(r->z) - exactZ);
        maxWindowErr = std::fmax(maxWindowErr, std::fmax(errX, errZ));
        if (p.cellX == 0 && p.cellZ == 0) {
            maxLocalErr = std::fmax(maxLocalErr, std::fmax(errX, errZ));
        }
    }
    std::printf("float32 max error: local(<512 m)=%.3e m, active window(+/-4 km)=%.3e m\n",
                maxLocalErr, maxWindowErr);
    SG_CHECK(maxLocalErr <= 3.06e-5);
    // Across the full ±4 km window, float32 half-ulp is at most 2^-11 m ≈ 0.49 mm.
    SG_CHECK(maxWindowErr <= 2.45e-4);
}

void TestRenderSpaceRangeLimit() {
    GlobalPosition inside;
    inside.cellX = sg::kMaxActiveCellDelta;
    inside.cellZ = -sg::kMaxActiveCellDelta;
    SG_CHECK(inside.ToRenderSpace(0, 0).has_value());

    GlobalPosition outside;
    outside.cellX = sg::kMaxActiveCellDelta + 1;
    SG_CHECK(!outside.ToRenderSpace(0, 0).has_value());

    // Extreme deltas must be rejected without overflow.
    GlobalPosition huge;
    huge.cellX = sg::kMaxCellMagnitude;
    SG_CHECK(!huge.ToRenderSpace(0, 0).has_value());
    SG_CHECK(!huge.ToRenderSpace(-sg::kMaxCellMagnitude, 0).has_value());
}

void TestAdvanceAcrossBoundaries() {
    // Walking east at 1.4 m/s: moves across boundaries and keeps offsets normalized.
    auto p = GlobalPosition::FromProjected(511.0, 0.0, 0.0);
    SG_CHECK(p.has_value());
    if (!p) return;
    int crossings = 0;
    for (int i = 0; i < 1000; ++i) {
        auto next = p->Advanced(sg::Vec3d{0.5, 0.0, 0.0});
        SG_CHECK(next.has_value());
        if (!next) return;
        if (next->cellX != p->cellX) ++crossings;
        p = next;
        SG_CHECK(p->localOffset.x >= 0.0 && p->localOffset.x < sg::kCellSize);
    }
    SG_CHECK(crossings == 1);  // 500 m total travel from x=511 crosses cell 1 once and reaches cell 1 at 512
    SG_CHECK(p->cellX == 1);
}

void TestDeterministicCanonicalization() {
    // The same projected point must give identical canonical state regardless of path.
    auto direct = GlobalPosition::FromProjected(30000.25, 29000.75, 3.5);
    auto viaSteps = GlobalPosition::FromProjected(0.0, 0.0, 3.5);
    SG_CHECK(direct.has_value() && viaSteps.has_value());
    if (direct && viaSteps) {
        auto moved = viaSteps->Advanced(sg::Vec3d{30000.25, 0.0, 29000.75});
        SG_CHECK(moved.has_value());
        if (moved) {
            SG_CHECK(moved->cellX == direct->cellX && moved->cellZ == direct->cellZ);
            SG_CHECK_NEAR(moved->localOffset.x, direct->localOffset.x, 1e-9);
            SG_CHECK_NEAR(moved->localOffset.z, direct->localOffset.z, 1e-9);
        }
    }
}

}  // namespace

void RunGlobalPositionTests() {
    TestNormalizeFloorSemantics();
    TestNormalizeRejectsBadInput();
    TestFixturesRoundTripAndDelta();
    TestFloat32PrecisionWithinActiveWindow();
    TestRenderSpaceRangeLimit();
    TestAdvanceAcrossBoundaries();
    TestDeterministicCanonicalization();
}
