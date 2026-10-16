#include "camera.h"

#include <cmath>

float groundHeight(const SceneGround& g, float x, float z) {
    if (g.pilot) {
        // Pilot rectangle in scene coordinates: x in [ox, ox + sizeX], z in [oz - sizeZ, oz].
        const float ox = g.pilotOffset[0], oz = g.pilotOffset[2];
        const float lx = x - ox;
        const float north = -z + oz;  // pilot-local north: scene z = -north_global, north_local = north_global - dn
        if (lx >= 0.0f && lx <= g.pilot->sizeX && north >= 0.0f && north <= g.pilot->sizeZ) {
            return terrainHeightAt(*g.pilot, lx, north);
        }
    }
    if (g.island) {
        const float north = -z;  // island frame: origin at the south-west corner, north positive
        if (x >= 0.0f && x <= g.island->sizeX && north >= 0.0f && north <= g.island->sizeZ) {
            return terrainHeightAt(*g.island, x, north);
        }
    }
    return 0.0f;
}

Vec3 orbitEye(Vec3 target, float yaw, float pitch, float dist) {
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const float sy = std::sin(yaw), cy = std::cos(yaw);
    return target + Vec3{cp * sy, sp, cp * cy} * dist;
}

float keepEyeAboveGround(const SceneGround& g, Vec3 target, float yaw, float pitch, float dist, float clearance) {
    constexpr int kMaxSteps = 60;
    float d = dist;
    for (int i = 0; i < kMaxSteps; ++i) {
        const Vec3 eye = orbitEye(target, yaw, pitch, d);
        if (eye.y >= groundHeight(g, eye.x, eye.z) + clearance) return d;
        d *= 1.15f;
    }
    return d;
}
