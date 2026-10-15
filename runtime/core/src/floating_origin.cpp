#include "sg/floating_origin.hpp"

namespace sg {

FloatingOriginManager::FloatingOriginManager(int64_t initialCellX, int64_t initialCellZ)
    : originCellX_(initialCellX), originCellZ_(initialCellZ) {}

OriginShiftEvent FloatingOriginManager::Update(const GlobalPosition& camera) {
    OriginShiftEvent ev;
    ev.fromCellX = originCellX_;
    ev.fromCellZ = originCellZ_;
    ev.toCellX = originCellX_;
    ev.toCellZ = originCellZ_;
    ev.epoch = originEpoch_;

    if (camera.cellX == originCellX_ && camera.cellZ == originCellZ_) {
        return ev;
    }

    int64_t dx = 0;
    int64_t dz = 0;
    // Canonical cells are bounded to ±2^40, so these cannot overflow; the check
    // guards against an unnormalized caller-supplied position.
    if (__builtin_sub_overflow(camera.cellX, originCellX_, &dx) ||
        __builtin_sub_overflow(camera.cellZ, originCellZ_, &dz)) {
        return ev;  // no shift; caller sees shifted == false
    }

    ev.shifted = true;
    ev.offset = Vec3f{static_cast<float>(-dx * static_cast<int64_t>(kCellSize)),
                      0.0f,
                      static_cast<float>(-dz * static_cast<int64_t>(kCellSize))};

    originCellX_ = camera.cellX;
    originCellZ_ = camera.cellZ;
    ++originEpoch_;

    ev.toCellX = originCellX_;
    ev.toCellZ = originCellZ_;
    ev.epoch = originEpoch_;
    return ev;
}

std::optional<Vec3f> FloatingOriginManager::GetRenderCoordinate(const GlobalPosition& entity) const {
    return entity.ToRenderSpace(originCellX_, originCellZ_);
}

void FloatingOriginManager::ShiftPhysicsWorld(PhysicsShiftTarget* physics, const OriginShiftEvent& event) const {
    if (physics == nullptr || !event.shifted) {
        return;
    }
    physics->TranslateAllBodies(event.offset);
}

}  // namespace sg
