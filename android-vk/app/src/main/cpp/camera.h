// Orbit camera helpers that keep the eye above the ground. Portable (no Android or Vulkan).
// Scene frame: x east, y up, z = -north, metres.
#pragma once

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
