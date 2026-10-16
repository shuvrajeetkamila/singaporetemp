#include "camera.h"

#include <algorithm>
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

Vec3 screenRayDir(Vec3 eye, Vec3 target, float fovy, float aspect, float px, float py, float w, float h) {
    const Vec3 fwd = normalize(target - eye);
    const Vec3 right = normalize(cross(fwd, Vec3{0.0f, 1.0f, 0.0f}));
    const Vec3 up = cross(right, fwd);
    const float ndcX = (w > 0.0f) ? 2.0f * px / w - 1.0f : 0.0f;
    const float ndcY = (h > 0.0f) ? 2.0f * py / h - 1.0f : 0.0f;  // -1 at the top of the screen
    const float t = std::tan(fovy * 0.5f);
    return normalize(fwd + right * (ndcX * t * aspect) + up * (-ndcY * t));
}

bool pickGround(const SceneGround& g, Vec3 eye, Vec3 dir, float maxDist, Vec3& hit) {
    // March outwards in steps that grow with distance, then bisect the first crossing below the ground.
    float prev = 0.0f;
    float s = 0.5f;
    bool above = true;
    while (s <= maxDist) {
        const Vec3 p = eye + dir * s;
        if (p.y <= groundHeight(g, p.x, p.z)) {
            above = false;
            break;
        }
        prev = s;
        s += std::max(0.5f, s * 0.01f);
    }
    if (above) return false;
    float lo = prev, hi = s;
    for (int i = 0; i < 40; ++i) {
        const float mid = 0.5f * (lo + hi);
        const Vec3 p = eye + dir * mid;
        if (p.y <= groundHeight(g, p.x, p.z)) hi = mid;
        else lo = mid;
    }
    hit = eye + dir * hi;
    hit.y = groundHeight(g, hit.x, hit.z);
    return true;
}

void zoomAboutPixel(const SceneGround& g, Vec3 target, float yaw, float pitch, float dist, float k, float px,
                    float py, float w, float h, float fovy, float minDist, float maxDist, Vec3& outTarget,
                    float& outDist) {
    outDist = std::min(std::max(dist * k, minDist), maxDist);
    outTarget = target;
    if (dist <= 0.0f || w <= 0.0f || h <= 0.0f) return;
    const float kk = outDist / dist;
    const Vec3 eye = orbitEye(target, yaw, pitch, dist);
    const Vec3 dir = screenRayDir(eye, target, fovy, w / h, px, py, w, h);
    Vec3 hit{};
    if (pickGround(g, eye, dir, maxDist, hit)) {
        // Eye and target both scale about the hit point, so the ray through (px, py) is unchanged.
        outTarget = hit + (target - hit) * kk;
    }
}
