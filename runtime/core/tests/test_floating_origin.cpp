#include <cmath>
#include <cstdio>
#include <vector>

#include "sg/floating_origin.hpp"
#include "test_common.hpp"

using sg::FloatingOriginManager;
using sg::GlobalPosition;
using sg::OriginShiftEvent;

namespace {

struct FakePhysics : sg::PhysicsShiftTarget {
    int calls = 0;
    double sumX = 0.0;
    double sumZ = 0.0;
    void TranslateAllBodies(const sg::Vec3f& offset) override {
        ++calls;
        sumX += offset.x;
        sumZ += offset.z;
    }
};

// Fixed-step traversal of the camera along a straight line, with canonical integration.
struct Traverse {
    GlobalPosition camera;
    FloatingOriginManager origin;
    int shifts = 0;
    double shiftSumX = 0.0;  // total applied offset on X (metres)
    double shiftSumZ = 0.0;
    uint64_t lastEpoch = 0;
    double maxStaticErr = 0.0;   // error of static entity render position vs exact delta
    double maxRelErr = 0.0;      // error in relative transform between two entities
    bool epochMonotonic = true;
    long inWindowSamples = 0;    // samples where both static entities were renderable

    Traverse(const GlobalPosition& start) : camera(start), origin(start.cellX, start.cellZ) {}
};

// Runs `seconds` of travel at `speed` m/s along `dir` (unit vector in X/Z), step `dt`.
void Drive(Traverse& t, const GlobalPosition& staticA, const GlobalPosition& staticB,
           double speed, double dirX, double dirZ, double seconds, double dt) {
    const int steps = static_cast<int>(seconds / dt);
    FakePhysics physics;
    for (int i = 0; i < steps; ++i) {
        auto next = t.camera.Advanced(sg::Vec3d{speed * dirX * dt, 0.0, speed * dirZ * dt});
        SG_CHECK(next.has_value());
        if (!next) return;
        t.camera = *next;

        const OriginShiftEvent ev = t.origin.Update(t.camera);
        if (ev.shifted) {
            ++t.shifts;
            t.shiftSumX += ev.offset.x;
            t.shiftSumZ += ev.offset.z;
            t.origin.ShiftPhysicsWorld(&physics, ev);
            if (ev.epoch <= t.lastEpoch) t.epochMonotonic = false;
            t.lastEpoch = ev.epoch;
            SG_CHECK(!t.origin.IsEpochCurrent(ev.epoch - 1));
        }
        SG_CHECK(t.origin.IsEpochCurrent(t.lastEpoch) || t.lastEpoch == 0);

        // Compare static entities with the exact double-precision delta from the camera.
        auto ra = t.origin.GetRenderCoordinate(staticA);
        auto rb = t.origin.GetRenderCoordinate(staticB);
        if (!ra || !rb) continue;  // out of active window, not part of this check
        ++t.inWindowSamples;

        // Render space is relative to the origin cell's corner (local = 0), not the camera.
        GlobalPosition corner;
        corner.cellX = t.origin.CurrentCellX();
        corner.cellZ = t.origin.CurrentCellZ();
        auto exactA = GlobalPosition::DeltaMeters(corner, staticA);
        auto exactB = GlobalPosition::DeltaMeters(corner, staticB);
        SG_CHECK(exactA.has_value() && exactB.has_value());
        if (!exactA || !exactB) return;

        // Render-space position of an entity equals its exact offset from the camera
        // when the camera sits at the render origin's cell.
        const double errA = std::fmax(std::fabs(ra->x - exactA->x), std::fabs(ra->z - exactA->z));
        t.maxStaticErr = std::fmax(t.maxStaticErr, errA);

        // Relative transform between two static entities must not drift.
        const double relRender_x = static_cast<double>(rb->x) - static_cast<double>(ra->x);
        const double relRender_z = static_cast<double>(rb->z) - static_cast<double>(ra->z);
        const double relExact_x = exactB->x - exactA->x;
        const double relExact_z = exactB->z - exactA->z;
        const double errRel = std::fmax(std::fabs(relRender_x - relExact_x),
                                        std::fabs(relRender_z - relExact_z));
        t.maxRelErr = std::fmax(t.maxRelErr, errRel);
    }
    SG_CHECK(physics.calls == t.shifts);
}

}  // namespace

void RunFloatingOriginTests() {
    // Basic shift: one-cell move east changes X render coordinates by -512 exactly.
    {
        auto cam = GlobalPosition::FromProjected(100.0, 100.0, 0.0);
        SG_CHECK(cam.has_value());
        if (!cam) return;
        FloatingOriginManager origin(cam->cellX, cam->cellZ);
        GlobalPosition ent = *cam;
        ent.localOffset.x = 400.0;

        auto before = origin.GetRenderCoordinate(ent);
        SG_CHECK(before.has_value());

        GlobalPosition moved = *cam;
        moved.cellX += 1;
        auto ev = origin.Update(moved);
        SG_CHECK(ev.shifted);
        SG_CHECK(ev.offset.x == -512.0f && ev.offset.z == 0.0f);
        SG_CHECK(ev.epoch == 1);
        SG_CHECK(!origin.IsEpochCurrent(0));

        auto after = origin.GetRenderCoordinate(ent);
        SG_CHECK(before && after);
        if (before && after) {
            // The entity's render position moves by exactly the shift offset.
            SG_CHECK_NEAR(after->x - before->x, ev.offset.x, 0.0);
            SG_CHECK_NEAR(after->z - before->z, ev.offset.z, 0.0);
        }

        // No second shift without a cell change.
        auto noop = origin.Update(moved);
        SG_CHECK(!noop.shifted);
        SG_CHECK(noop.epoch == ev.epoch);
    }

    // Traversal along the west–east axis at walking speed (1.4 m/s), 60 Hz, for 40 minutes.
    {
        auto start = GlobalPosition::FromProjected(4259.223, 28056.648, 0.0);  // west fixture
        auto entA = GlobalPosition::FromProjected(21732.379, 37222.397, 0.0);  // Bukit Timah fixture
        auto entB = GlobalPosition::FromProjected(30357.314, 29913.335, 0.0);  // Merlion fixture
        SG_CHECK(start && entA && entB);
        if (!start || !entA || !entB) return;

        Traverse t(*start);
        // Keep the static entities inside the active window at all times (±4 km).
        // Travel from the west point with entities placed near the camera path.
        auto nearA = start->Advanced(sg::Vec3d{900.0, 0.0, 0.0});
        auto nearB = start->Advanced(sg::Vec3d{1500.0, 0.0, 200.0});
        SG_CHECK(nearA && nearB);
        if (!nearA || !nearB) return;
        Drive(t, *nearA, *nearB, 1.4, 1.0, 0.0, 2400.0, 1.0 / 60.0);
        std::printf("walk: shifts=%d inWindow=%ld maxStaticErr=%.3e m maxRelErr=%.3e m\n", t.shifts, t.inWindowSamples, t.maxStaticErr,
                    t.maxRelErr);
        SG_CHECK(t.shifts > 0);
        SG_CHECK(t.inWindowSamples > 100000);
        SG_CHECK(t.epochMonotonic);
        // Acceptance: below 0.5 mm of float32 error across the active window.
        SG_CHECK(t.maxStaticErr <= 5e-4);
        SG_CHECK(t.maxRelErr <= 5e-4);
    }

    // Vehicle-speed traversal (30 m/s ≈ 108 km/h) along the north–south axis.
    {
        auto start = GlobalPosition::FromProjected(27664.053, 25777.828, 0.0);  // south fixture
        SG_CHECK(start.has_value());
        if (!start) return;
        Traverse t(*start);
        auto nearA = start->Advanced(sg::Vec3d{0.0, 0.0, 1800.0});
        auto nearB = start->Advanced(sg::Vec3d{-1200.0, 0.0, 600.0});
        SG_CHECK(nearA && nearB);
        if (!nearA || !nearB) return;
        Drive(t, *nearA, *nearB, 30.0, 0.0, 1.0, 600.0, 1.0 / 60.0);
        std::printf("drive: shifts=%d inWindow=%ld maxStaticErr=%.3e m maxRelErr=%.3e m\n", t.shifts, t.inWindowSamples, t.maxStaticErr,
                    t.maxRelErr);
        SG_CHECK(t.shifts > 0);
        SG_CHECK(t.inWindowSamples > 1000);
        SG_CHECK(t.maxStaticErr <= 5e-4);
        SG_CHECK(t.maxRelErr <= 5e-4);
    }

    // Accumulated shift offsets must equal the net origin travel, exactly.
    {
        auto start = GlobalPosition::FromProjected(20000.0, 30000.0, 0.0);
        SG_CHECK(start.has_value());
        if (!start) return;
        FloatingOriginManager origin(start->cellX, start->cellZ);
        GlobalPosition cam = *start;
        double sumX = 0.0;
        for (int i = 0; i < 20; ++i) {
            cam = *cam.Advanced(sg::Vec3d{300.0, 0.0, 0.0});
            auto ev = origin.Update(cam);
            if (ev.shifted) sumX += ev.offset.x;
        }
        // Net travel of 6000 m moves the origin by 6000/512 cells; the sum of offsets
        // must cancel to -(finalCell - startCell) * 512.
        const double expected = -static_cast<double>(origin.CurrentCellX() - start->cellX) * sg::kCellSize;
        SG_CHECK_NEAR(sumX, expected, 0.0);
    }

    // Physics shift is applied only when the origin moves, and null targets are ignored.
    {
        FloatingOriginManager origin(0, 0);
        FakePhysics physics;
        OriginShiftEvent none;
        origin.ShiftPhysicsWorld(&physics, none);
        origin.ShiftPhysicsWorld(nullptr, none);
        SG_CHECK(physics.calls == 0);

        GlobalPosition cam;
        cam.cellZ = -1;
        auto ev = origin.Update(cam);
        SG_CHECK(ev.shifted);
        origin.ShiftPhysicsWorld(&physics, ev);
        SG_CHECK(physics.calls == 1);
        SG_CHECK_NEAR(physics.sumZ, 512.0, 0.0);
    }

    // Entities outside the active window are reported as not renderable.
    {
        FloatingOriginManager origin(0, 0);
        GlobalPosition far;
        far.cellX = 100;
        SG_CHECK(!origin.GetRenderCoordinate(far).has_value());
    }
}
