// Minimal math types for the runtime core.
// Deliberately dependency-free; the engine can substitute glm types as long as
// layouts stay the same (three packed components, no padding).
#pragma once

namespace sg {

struct Vec3d {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct Vec3f {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

}  // namespace sg
