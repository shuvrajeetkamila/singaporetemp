// NativeActivity entry point: asset loading, the frame loop, touch camera.
//
// Scene frame: metres, x east, y up, z = -north, origin at the island grid's south-west corner.
// The pilot district (full detail, with roads and traffic) is placed in that frame by a fixed
// offset; the rest of the island is drawn at base detail with the pilot rectangle cut out.
#include <android/asset_manager.h>
#include <android/input.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "agents.h"
#include "camera.h"
#include "core_math.h"
#include "diag.h"
#include "island.h"
#include "renderer.h"
#include "sim.h"
#include "world.h"

#define LOG_TAG "VkIsland"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

constexpr int kCars = 150;
constexpr int kPeople = 300;
constexpr float kFovY = 0.87266f;  // 50 degrees

struct AppState {
    android_app* app = nullptr;
    bool assetsLoaded = false;   // pilot district and shaders
    bool islandLoaded = false;   // base-detail island layer (optional)

    ShaderBlobs shaders;
    District pilot;
    District island;
    IslandLayout layout;
    TrafficSim sim;
    std::vector<MeshBlob> meshes;  // pilot static mesh first, then the island layer
    std::vector<Vertex> agentMesh;
    float pilotOffset[3] = {0.0f, 0.0f, 0.0f};
    Renderer renderer;

    // Orbit camera around `target` (scene frame).
    Vec3 target{0.0f, 0.0f, 0.0f};
    float yaw = 0.6f;
    float pitch = 0.9f;
    float dist = 2200.0f;
    bool distFitted = false;  // island start distance is fitted to the screen aspect once
    bool stoppedShown = false;  // the runtime Vulkan error is already on screen

    // Touch gesture state.
    bool gestureValid = false;
    int gesturePointers = 0;
    float g0x = 0.0f, g0y = 0.0f, g1x = 0.0f, g1y = 0.0f;
};

bool readAsset(AAssetManager* mgr, const char* name, std::vector<uint8_t>& out) {
    AAsset* asset = AAssetManager_open(mgr, name, AASSET_MODE_BUFFER);
    if (!asset) {
        LOGE("missing asset: %s", name);
        char msg[160];
        std::snprintf(msg, sizeof msg, "MISSING FILE %s", name);
        diagSetDetail(msg);
        return false;
    }
    off_t len = AAsset_getLength(asset);
    out.resize(static_cast<size_t>(len > 0 ? len : 0));
    bool ok = !out.empty() && AAsset_read(asset, out.data(), out.size()) == static_cast<int>(out.size());
    AAsset_close(asset);
    if (!ok) {
        LOGE("could not read asset: %s", name);
        char msg[160];
        std::snprintf(msg, sizeof msg, "CANNOT READ %s", name);
        diagSetDetail(msg);
    }
    return ok;
}

bool readSpirv(AAssetManager* mgr, const char* name, std::vector<uint32_t>& out) {
    std::vector<uint8_t> bytes;
    if (!readAsset(mgr, name, bytes) || bytes.size() % 4 != 0) return false;
    out.resize(bytes.size() / 4);
    std::memcpy(out.data(), bytes.data(), bytes.size());
    return true;
}

MeshBlob toMesh(const std::vector<Vertex>& verts, const float offset[3]) {
    MeshBlob m;
    m.vertices = verts;
    m.indices.resize(verts.size());
    for (size_t i = 0; i < verts.size(); ++i) m.indices[i] = static_cast<uint32_t>(i);
    for (int k = 0; k < 3; ++k) m.offset[k] = offset[k];
    return m;
}

// Pilot district files are required. Island files are optional: without them the app shows the
// pilot alone, in its own frame.
bool loadAssets(AppState& st) {
    AAssetManager* mgr = st.app->activity->assetManager;
    diagSetStage("LOADING SHADERS");
    if (!readSpirv(mgr, "shaders/mesh.vert.spv", st.shaders.vert) ||
        !readSpirv(mgr, "shaders/mesh.frag.spv", st.shaders.frag)) {
        return false;
    }

    diagSetStage("LOADING PILOT DISTRICT");
    std::vector<uint8_t> terrain, buildings, roads;
    if (!readAsset(mgr, "district/terrain.bin", terrain) || !readAsset(mgr, "district/buildings.bin", buildings) ||
        !readAsset(mgr, "district/roads.bin", roads)) {
        return false;
    }
    std::string err;
    if (!parseTerrain(terrain, st.pilot, err) || !parseBuildings(buildings, st.pilot, err) ||
        !parseRoads(roads, st.pilot, err)) {
        LOGE("pilot district rejected: %s", err.c_str());
        char msg[160];
        std::snprintf(msg, sizeof msg, "PILOT FILES REJECTED: %s", err.c_str());
        diagSetDetail(msg);
        return false;
    }

    // Island layer (optional).
    diagSetStage("LOADING ISLAND");
    std::vector<uint8_t> iTerrain, iBuildings, iLayout;
    bool haveIsland = readAsset(mgr, "island/terrain.bin", iTerrain) &&
                      readAsset(mgr, "island/buildings.bin", iBuildings) &&
                      readAsset(mgr, "island/layout.txt", iLayout);
    IslandLayout layout;
    if (haveIsland && parseLayout(iLayout, layout) && parseTerrain(iTerrain, st.island, err) &&
        parseBuildings(iBuildings, st.island, err)) {
        st.layout = layout;
        st.islandLoaded = true;
        st.pilotOffset[0] = layout.dx;
        st.pilotOffset[1] = 0.0f;
        st.pilotOffset[2] = -layout.dn;  // scene z = -north
        LOGI("island layer: %ux%u terrain, %zu buildings", st.island.tw, st.island.th, st.island.buildings.size());
    } else {
        LOGI("island layer not available (%s); showing the pilot district only", err.c_str());
        st.island = District{};
        st.islandLoaded = false;
        st.pilotOffset[0] = st.pilotOffset[1] = st.pilotOffset[2] = 0.0f;
    }

    // Meshes.
    diagSetStage("BUILDING PILOT MESH");
    std::vector<Vertex> pilotVerts;
    buildStaticMesh(st.pilot, pilotVerts);
    st.meshes.clear();
    st.meshes.push_back(toMesh(pilotVerts, st.pilotOffset));
    if (st.islandLoaded) {
        Rect exclude{st.layout.dx, st.layout.dn, st.layout.dx + st.layout.sx, st.layout.dn + st.layout.sz};
        MeshBlob islandMesh;
        diagSetStage("BUILDING ISLAND MESH");
        buildIslandMesh(st.island, exclude, islandMesh);
        st.meshes.push_back(std::move(islandMesh));
        LOGI("island mesh: %zu vertices, %zu indices", st.meshes.back().vertices.size(),
             st.meshes.back().indices.size());

        st.target = {st.island.sizeX * 0.5f, 0.0f, -st.island.sizeZ * 0.5f};
        st.dist = 60000.0f;
    } else {
        st.target = {st.pilot.sizeX * 0.5f, 0.0f, -st.pilot.sizeZ * 0.5f};
        st.dist = 2200.0f;
    }

    st.sim.init(&st.pilot.roads, 20251015u, kCars, kPeople);
    st.assetsLoaded = true;
    LOGI("loaded: pilot %zu buildings, %zu road edges, %zu agents", st.pilot.buildings.size(),
         st.pilot.roads.edges.size(), st.sim.agents().size());
    return true;
}

// Distance at which a bounding sphere around the island centre fits inside the view, for this aspect.
// Conservative: every point of the island lies within the sphere, so it is inside both view half-angles.
float islandFitDistance(const District& island, float aspect) {
    const float halfX = island.sizeX * 0.5f, halfZ = island.sizeZ * 0.5f;
    const float radius = std::sqrt(halfX * halfX + halfZ * halfZ) * 1.1f;
    const float tanV = std::tan(kFovY * 0.5f);
    const float halfV = kFovY * 0.5f;
    const float halfH = std::atan(tanV * std::max(aspect, 0.05f));
    const float half = std::min(halfV, halfH);
    return radius / std::sin(half);
}

// Keeps the orbit target on the ground and the eye above it. Without this the target stayed at sea
// level, so the camera could end up inside hills (solid green), under water (solid blue) or inside
// buildings (black) after a low-angle orbit or a pan.
void updateCamera(AppState& st) {
    SceneGround g;
    g.pilot = &st.pilot;
    g.pilotOffset[0] = st.pilotOffset[0];
    g.pilotOffset[1] = st.pilotOffset[1];
    g.pilotOffset[2] = st.pilotOffset[2];
    g.island = st.islandLoaded ? &st.island : nullptr;
    st.target.y = groundHeight(g, st.target.x, st.target.z);
    st.dist = keepEyeAboveGround(g, st.target, st.yaw, st.pitch, st.dist, 6.0f);
}

Mat4 computeViewProj(const AppState& st, float aspect) {
    const float cp = std::cos(st.pitch), sp = std::sin(st.pitch);
    const float sy = std::sin(st.yaw), cy = std::cos(st.yaw);
    const Vec3 offset{cp * sy, sp, cp * cy};
    const Vec3 eye = st.target + offset * st.dist;
    const Mat4 view = mat4LookAt(eye, st.target, Vec3{0.0f, 1.0f, 0.0f});
    // Near plane scales with distance so depth stays precise from street level to the whole island.
    const float zNear = std::min(std::max(st.dist * 0.05f, 1.0f), 2000.0f);
    const Mat4 proj = mat4Perspective(kFovY, aspect, zNear, 3.0e6f);
    return mat4Mul(proj, view);
}

// Shows a failure on screen: the renderer is torn down first so the window is free for plain pixels.
void showFailure(AppState& st, uint32_t background, const char* title, const char* detail) {
    st.renderer.destroy();
    char l2[200], l3[240], l4[320];
    std::snprintf(l2, sizeof l2, "STAGE: %s", diagStage());
    std::snprintf(l3, sizeof l3, "DETAIL: %s", detail && *detail ? detail : diagDetail());
    std::snprintf(l4, sizeof l4, "%s", "CLOSE AND REOPEN THE APP");
    const char* lines[] = {title, l2, l3, l4};
    LOGE("failure: %s / %s / %s", title, l2, l3);
    diagPaint(st.app->window, background, lines, 4);
}

void onAppCmd(android_app* app, int32_t cmd) {
    AppState* st = static_cast<AppState*>(app->userData);
    if (!st) return;
    switch (cmd) {
        case APP_CMD_INIT_WINDOW:
            if (app->window) {
                // Screen colour is the status: blue = Vulkan up, magenta = data failed,
                // orange = shaders/pipeline failed, sky blue = scene running, red = no Vulkan.
                // Failures also write the stage and the detail on screen as text.
                diagSetWindow(app->window);
                diagSetStage("STARTING VULKAN");
                if (!st->renderer.init(app->window)) {
                    showFailure(*st, packRgba(190, 0, 0), "VULKAN DID NOT START", st->renderer.lastError().c_str());
                    break;
                }
                st->renderer.setClearColor(0.10f, 0.30f, 0.90f);
                if (!st->assetsLoaded && !loadAssets(*st)) {
                    showFailure(*st, packRgba(200, 0, 200), "DATA DID NOT LOAD", "");
                    break;
                }
                diagSetStage("UPLOADING TO GPU");
                if (!st->renderer.setScene(st->shaders, st->meshes)) {
                    showFailure(*st, packRgba(230, 120, 0), "GPU UPLOAD FAILED", st->renderer.lastError().c_str());
                    break;
                }
                diagSetStage("RUNNING");
                // With the island, the background is the sea colour: the sea plane then runs to the horizon.
                // Pilot only: sky blue.
                if (st->islandLoaded) {
                    st->renderer.setClearColor(0.14f, 0.40f, 0.62f);
                } else {
                    st->renderer.setClearColor(0.62f, 0.78f, 0.92f);
                }
            }
            break;
        case APP_CMD_TERM_WINDOW:
            diagSetWindow(nullptr);
            st->renderer.destroy();
            break;
        default:
            break;
    }
}

int32_t onInput(android_app* app, AInputEvent* event) {
    AppState* st = static_cast<AppState*>(app->userData);
    if (!st || AInputEvent_getType(event) != AINPUT_EVENT_TYPE_MOTION) return 0;

    const int32_t masked = AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK;
    switch (masked) {
        case AMOTION_EVENT_ACTION_DOWN:
        case AMOTION_EVENT_ACTION_UP:
        case AMOTION_EVENT_ACTION_POINTER_DOWN:
        case AMOTION_EVENT_ACTION_POINTER_UP:
        case AMOTION_EVENT_ACTION_CANCEL:
            st->gestureValid = false;  // pointer set changed: take a new baseline on the next move
            return 1;
        case AMOTION_EVENT_ACTION_MOVE:
            break;
        default:
            return 1;
    }

    const size_t count = AMotionEvent_getPointerCount(event);
    const int n = count >= 2 ? 2 : 1;
    const float x0 = AMotionEvent_getX(event, 0), y0 = AMotionEvent_getY(event, 0);
    const float x1 = n == 2 ? AMotionEvent_getX(event, 1) : x0;
    const float y1 = n == 2 ? AMotionEvent_getY(event, 1) : y0;

    if (st->gestureValid && st->gesturePointers == n) {
        if (n == 1) {
            // One finger: orbit.
            const float dx = x0 - st->g0x, dy = y0 - st->g0y;
            st->yaw -= dx * 0.006f;
            st->pitch = std::min(std::max(st->pitch + dy * 0.004f, 0.2f), 1.45f);
        } else {
            // Two fingers: pinch to zoom, move both to pan.
            const float dPrev = std::hypot(st->g1x - st->g0x, st->g1y - st->g0y);
            const float dNow = std::hypot(x1 - x0, y1 - y0);
            if (dPrev > 1.0f && dNow > 1.0f) {
                st->dist = std::min(std::max(st->dist * dPrev / dNow, 60.0f), 400000.0f);
            }
            const float mx = ((x0 + x1) - (st->g0x + st->g1x)) * 0.5f;
            const float my = ((y0 + y1) - (st->g0y + st->g1y)) * 0.5f;
            const float h = ANativeWindow_getHeight(app->window) > 0
                                ? static_cast<float>(ANativeWindow_getHeight(app->window))
                                : 1000.0f;
            const float scale = st->dist * 0.933f / h;  // approx. metres per pixel at the target
            const float sy = std::sin(st->yaw), cy = std::cos(st->yaw);
            const Vec3 right{cy, 0.0f, -sy};
            const Vec3 fwdH{-sy, 0.0f, -cy};
            st->target = st->target - right * (mx * scale) - fwdH * (my * scale);
        }
    }

    st->gestureValid = true;
    st->gesturePointers = n;
    st->g0x = x0;
    st->g0y = y0;
    st->g1x = x1;
    st->g1y = y1;
    return 1;
}

}  // namespace

void android_main(struct android_app* app) {
    diagInstallCrashHandler();
    AppState st;
    st.app = app;
    app->userData = &st;
    app->onAppCmd = onAppCmd;
    app->onInputEvent = onInput;

    using clock = std::chrono::steady_clock;
    auto last = clock::now();

    while (!app->destroyRequested) {
        // Block for events while nothing can be drawn; otherwise poll without waiting.
        int timeout = st.renderer.ready() ? 0 : -1;
        int events = 0;
        android_poll_source* source = nullptr;
        while (ALooper_pollOnce(timeout, nullptr, &events, reinterpret_cast<void**>(&source)) >= 0) {
            if (source) source->process(app, source);
            if (app->destroyRequested) break;
            timeout = st.renderer.ready() ? 0 : -1;
        }
        if (app->destroyRequested) break;
        if (!st.renderer.ready()) continue;

        const auto now = clock::now();
        const float dt = std::min(std::chrono::duration<float>(now - last).count(), 0.05f);
        last = now;

        if (st.renderer.hasScene()) {
            st.sim.step(dt);
            buildAgentMesh(st.pilot, st.sim, st.agentMesh);
        } else {
            st.agentMesh.clear();
        }

        const int32_t w = ANativeWindow_getWidth(app->window);
        const int32_t h = ANativeWindow_getHeight(app->window);
        const float aspect = (w > 0 && h > 0) ? static_cast<float>(w) / static_cast<float>(h) : 1.0f;
        if (st.islandLoaded && !st.distFitted && w > 0 && h > 0) {
            st.dist = islandFitDistance(st.island, aspect);
            st.distFitted = true;
            LOGI("island start distance %.0f m for aspect %.2f", st.dist, aspect);
        }
        updateCamera(st);
        if (!st.renderer.drawFrame(computeViewProj(st, aspect), st.pilotOffset, st.agentMesh)) {
            // Rendering stopped with an error: show it as text instead of leaving a frozen or blank screen.
            if (!st.renderer.ready() && !st.stoppedShown) {
                st.stoppedShown = true;
                showFailure(st, packRgba(190, 0, 0), "VULKAN STOPPED", st.renderer.lastError().c_str());
            }
            usleep(16000);  // skipped frame (minimised or resizing); avoid spinning
        }
    }

    st.renderer.destroy();
    LOGI("exiting");
}
