// Orbit camera helpers that keep the eye above the ground. Portable (no Android or Vulkan).
// Scene frame: x east, y up, z = -north, metres.
#pragma once

#include <cmath>

#include "core_math.h"
#include "world.h"

struct SceneGround {
    const District* pilot = nullptr;   // full-detail district, drawn at pilotOffset
    float pilotOffset[3] = {0.0f, 0.0f, 0.0f};
    const District* island = nullptr;  // base-detail island layer (optional), drawn at the origin
};

// Ground height (metres) at a scene point. Uses the pilot grid inside the pilot rectangle and the
// island grid elsewhere. Sea and anything outside the grids is 0.
float groundHeight(const SceneGround& g, float x, float z);

// Orbit eye position for a target, yaw, pitch and distance.
Vec3 orbitEye(Vec3 target, float yaw, float pitch, float dist);

// Returns the distance (>= dist) at which the eye is at least `clearance` metres above the ground.
// Pulls the camera out in steps; gives up after a fixed number of steps.
float keepEyeAboveGround(const SceneGround& g, Vec3 target, float yaw, float pitch, float dist, float clearance);

// Unit direction of the ray through pixel (px, py) of a w x h viewport (0,0 = top left). Matches the
// projection in computeViewProj: Vulkan clip space with Y flipped, camera up = cross(right, forward).
Vec3 screenRayDir(Vec3 eye, Vec3 target, float fovy, float aspect, float px, float py, float w, float h);

// First point along the ray where it meets the ground (height 0 outside the grids, so the sea is ground).
// Returns false if nothing is hit within maxDist.
bool pickGround(const SceneGround& g, Vec3 eye, Vec3 dir, float maxDist, Vec3& hit);

// Zooms by factor k (new distance / old) about pixel (px, py) of a w x h viewport, with the orbit
// camera at (target, yaw, pitch, dist) and vertical field of view fovy. The ground under that pixel stays
// under it. The new distance is clamped to [minDist, maxDist]. If there is no ground under the pixel, the
// target stays where it is.
void zoomAboutPixel(const SceneGround& g, Vec3 target, float yaw, float pitch, float dist, float k, float px,
                    float py, float w, float h, float fovy, float minDist, float maxDist, Vec3& outTarget,
                    float& outDist);

// Phone touch positions come in the phone's upright (portrait) frame, but the picture is landscape.
// Maps a touch (tx, ty) to a picture pixel (px, py). The touch frame is about 2340 px long, and the
// picture is pictureWidth px wide, so the unused strip (about 100 px, shown black) sits at the picture's left.
// Checked on build 16 with the drag test: a finger dragged up must move the picture up.
inline void touchToPicture(float tx, float ty, float pictureWidth, float& px, float& py) {
    px = pictureWidth - ty;
    py = tx;
}

// New orbit target after a one-finger drag of (dxPx, dyPx) screen pixels. The ground under the finger
// follows it: a drag up (dyPx < 0) moves the view toward the far side (forward), a drag right moves it left.
// metresPerPixel is the ground size of one pixel at the target.
inline Vec3 panTargetForDrag(Vec3 target, float yaw, float dxPx, float dyPx, float metresPerPixel) {
    const float sy = std::sin(yaw), cy = std::cos(yaw);
    const Vec3 right{cy, 0.0f, -sy};
    const Vec3 fwdH{-sy, 0.0f, -cy};
    return target - right * (dxPx * metresPerPixel) + fwdH * (dyPx * metresPerPixel);
}
