#include "sg/global_position.hpp"

#include <cmath>

namespace sg {
namespace {

bool IsFinite(const Vec3d& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// Splits one horizontal axis into whole cells plus a remainder in [0, kCellSize).
// Writes outputs only on success, so callers can commit both axes atomically.
bool SplitAxis(double value, int64_t cellIn, int64_t* cellOut, double* localOut) {
    const double q = std::floor(value / kCellSize);
    // Also rejects NaN, because every comparison with NaN is false.
    if (!(q >= -static_cast<double>(kMaxCellMagnitude) &&
          q <= static_cast<double>(kMaxCellMagnitude))) {
        return false;
    }
    double rem = value - q * kCellSize;
    int64_t cell = static_cast<int64_t>(q);
    // Rounding guard: value - q*512 can land exactly on the boundary.
    if (rem >= kCellSize) {
        rem -= kCellSize;
        cell += 1;
    }
    if (rem < 0.0) {
        rem += kCellSize;
        cell -= 1;
    }
    int64_t total = 0;
    if (__builtin_add_overflow(cellIn, cell, &total)) {
        return false;
    }
    if (total > kMaxCellMagnitude || total < -kMaxCellMagnitude) {
        return false;
    }
    *cellOut = total;
    *localOut = rem;
    return true;
}

}  // namespace

const char* ToString(GeoStatus status) {
    switch (status) {
        case GeoStatus::Ok: return "ok";
        case GeoStatus::NonFinite: return "non-finite coordinate";
        case GeoStatus::CellOverflow: return "cell index overflow";
        case GeoStatus::OutOfActiveRange: return "outside active render range";
    }
    return "unknown";
}

GeoStatus GlobalPosition::Normalize() {
    if (!IsFinite(localOffset)) {
        return GeoStatus::NonFinite;
    }
    if (cellX > kMaxCellMagnitude || cellX < -kMaxCellMagnitude ||
        cellZ > kMaxCellMagnitude || cellZ < -kMaxCellMagnitude) {
        return GeoStatus::CellOverflow;
    }
    int64_t nextX = 0;
    int64_t nextZ = 0;
    double localX = 0.0;
    double localZ = 0.0;
    if (!SplitAxis(localOffset.x, cellX, &nextX, &localX) ||
        !SplitAxis(localOffset.z, cellZ, &nextZ, &localZ)) {
        return GeoStatus::CellOverflow;
    }
    // Commit only after both axes succeeded.
    cellX = nextX;
    cellZ = nextZ;
    localOffset.x = localX;
    localOffset.z = localZ;
    return GeoStatus::Ok;
}

std::optional<GlobalPosition> GlobalPosition::FromProjected(double easting, double northing, double height) {
    GlobalPosition p;
    p.localOffset = Vec3d{easting, height, northing};
    if (p.Normalize() != GeoStatus::Ok) {
        return std::nullopt;
    }
    return p;
}

std::optional<Vec3d> GlobalPosition::DeltaMeters(const GlobalPosition& from, const GlobalPosition& to) {
    int64_t dx = 0;
    int64_t dz = 0;
    if (__builtin_sub_overflow(to.cellX, from.cellX, &dx) ||
        __builtin_sub_overflow(to.cellZ, from.cellZ, &dz)) {
        return std::nullopt;
    }
    const Vec3d local{to.localOffset.x - from.localOffset.x,
                      to.localOffset.y - from.localOffset.y,
                      to.localOffset.z - from.localOffset.z};
    if (!IsFinite(local)) {
        return std::nullopt;
    }
    // Both inputs are canonical, so |dx| <= 2^41 and dx * kCellSize is exact in double.
    return Vec3d{static_cast<double>(dx) * kCellSize + local.x,
                 local.y,
                 static_cast<double>(dz) * kCellSize + local.z};
}

std::optional<GlobalPosition> GlobalPosition::Advanced(const Vec3d& delta) const {
    if (!std::isfinite(delta.x) || !std::isfinite(delta.y) || !std::isfinite(delta.z)) {
        return std::nullopt;
    }
    GlobalPosition next = *this;
    next.localOffset.x += delta.x;
    next.localOffset.y += delta.y;
    next.localOffset.z += delta.z;
    if (next.Normalize() != GeoStatus::Ok) {
        return std::nullopt;
    }
    return next;
}

std::optional<Vec3f> GlobalPosition::ToRenderSpace(int64_t originCellX, int64_t originCellZ) const {
    int64_t dx = 0;
    int64_t dz = 0;
    if (__builtin_sub_overflow(cellX, originCellX, &dx) ||
        __builtin_sub_overflow(cellZ, originCellZ, &dz)) {
        return std::nullopt;
    }
    if (dx < -kMaxActiveCellDelta || dx > kMaxActiveCellDelta ||
        dz < -kMaxActiveCellDelta || dz > kMaxActiveCellDelta) {
        return std::nullopt;
    }
    const double x = static_cast<double>(dx) * kCellSize + localOffset.x;
    const double z = static_cast<double>(dz) * kCellSize + localOffset.z;
    return Vec3f{static_cast<float>(x),
                 static_cast<float>(localOffset.y),
                 static_cast<float>(z)};
}

}  // namespace sg
