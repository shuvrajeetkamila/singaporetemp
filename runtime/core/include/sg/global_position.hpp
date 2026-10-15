// Canonical global position for the 512 m macro-cell grid.
//
// Coordinates are SVY21 / EPSG:3414 metres: X = easting, Z = northing,
// Y = height in the project's documented vertical reference. The project origin
// is the SVY21 grid origin (0 m, 0 m), i.e. cell (0, 0) with local offset (0, 0).
//
// Canonical positions are always stored as (integer cell, local offset in
// [0, kCellSize)). Render-space coordinates are derived from them on demand and
// never accumulated back into a large world-space float.
#pragma once

#include <cstdint>
#include <optional>

#include "sg/math.hpp"

namespace sg {

inline constexpr double kCellSize = 512.0;

// Largest camera-relative cell delta that is valid for float32 render space.
// ±8 cells = ±4 km around the origin cell; the streaming radius must stay inside it.
inline constexpr int64_t kMaxActiveCellDelta = 8;

// Canonical cell indices are limited to ±2^40. At 512 m per cell this is far
// beyond any real map extent and keeps cell * kCellSize exact in double.
inline constexpr int64_t kMaxCellMagnitude = int64_t{1} << 40;

enum class GeoStatus {
    Ok,
    NonFinite,          // NaN or infinite input
    CellOverflow,       // checked int64 / domain bound exceeded
    OutOfActiveRange,   // cell delta outside the float32 render window
};

const char* ToString(GeoStatus status);

struct GlobalPosition {
    int64_t cellX = 0;
    int64_t cellZ = 0;
    // X/Z in [0, kCellSize) after Normalize(); Y is the project height.
    Vec3d localOffset{0.0, 0.0, 0.0};

    // Floor-based normalization. Moves whole cells out of localOffset.x/z with
    // checked int64 arithmetic. On failure the position is left unchanged.
    GeoStatus Normalize();

    // Builds a normalized position from projected coordinates (metres).
    static std::optional<GlobalPosition> FromProjected(double easting, double northing, double height);

    // Exact difference `to - from` in metres. Returns nullopt on overflow.
    static std::optional<Vec3d> DeltaMeters(const GlobalPosition& from, const GlobalPosition& to);

    // Returns a normalized copy moved by `deltaMeters` (X/Y/Z). Fails on non-finite input or overflow.
    std::optional<GlobalPosition> Advanced(const Vec3d& deltaMeters) const;

    // Camera-relative float32 render coordinate. Returns nullopt when the cell
    // delta is outside ±kMaxActiveCellDelta; such entities must be culled or unloaded.
    std::optional<Vec3f> ToRenderSpace(int64_t originCellX, int64_t originCellZ) const;

    bool operator==(const GlobalPosition& other) const {
        return cellX == other.cellX && cellZ == other.cellZ &&
               localOffset.x == other.localOffset.x &&
               localOffset.y == other.localOffset.y &&
               localOffset.z == other.localOffset.z;
    }
};

}  // namespace sg
