// Floating origin: keeps render and physics coordinates camera-relative.
//
// Usage contract (per frame, on the main simulation thread):
//   1. ev = origin.Update(cameraCanonicalPosition);
//   2. if (ev.shifted): origin.ShiftPhysicsWorld(physics, ev) and push ev.offset
//      to every render/audio/particle system before any system reads positions.
//   3. Background jobs record origin.OriginEpoch() when they start and must
//      discard their results when IsEpochCurrent() returns false.
#pragma once

#include <cstdint>
#include <optional>

#include "sg/global_position.hpp"
#include "sg/math.hpp"

namespace sg {

struct OriginShiftEvent {
    bool shifted = false;
    int64_t fromCellX = 0;
    int64_t fromCellZ = 0;
    int64_t toCellX = 0;
    int64_t toCellZ = 0;
    // Added to every camera-relative coordinate after this event:
    // offset = -(toCell - fromCell) * kCellSize on X and Z. Exact in float32 for small deltas.
    Vec3f offset{0.0f, 0.0f, 0.0f};
    uint64_t epoch = 0;  // origin epoch after the event
};

// Implemented by the physics integration layer (for example, a Jolt wrapper).
class PhysicsShiftTarget {
public:
    virtual ~PhysicsShiftTarget() = default;
    // Translates every active body by `offset`. Velocities, orientations,
    // contacts, and constraints are preserved.
    virtual void TranslateAllBodies(const Vec3f& offset) = 0;
};

class FloatingOriginManager {
public:
    explicit FloatingOriginManager(int64_t initialCellX = 0, int64_t initialCellZ = 0);

    // Moves the origin to the camera's cell when they differ. Returns the shift event.
    OriginShiftEvent Update(const GlobalPosition& camera);

    // Camera-relative render coordinate, or nullopt when outside the active window.
    std::optional<Vec3f> GetRenderCoordinate(const GlobalPosition& entity) const;

    // Applies the shift to physics bodies. No-op when the event did not shift.
    void ShiftPhysicsWorld(PhysicsShiftTarget* physics, const OriginShiftEvent& event) const;

    bool IsEpochCurrent(uint64_t epoch) const { return epoch == originEpoch_; }

    int64_t CurrentCellX() const { return originCellX_; }
    int64_t CurrentCellZ() const { return originCellZ_; }
    uint64_t OriginEpoch() const { return originEpoch_; }

private:
    int64_t originCellX_;
    int64_t originCellZ_;
    uint64_t originEpoch_ = 0;
};

}  // namespace sg
