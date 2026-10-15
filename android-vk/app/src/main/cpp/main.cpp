// NativeActivity entry point: asset loading, the frame loop, touch camera.
#include <android/asset_manager.h>
#include <android/input.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "agents.h"
#include "core_math.h"
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
    bool assetsLoaded = false;

    ShaderBlobs shaders;
    District district;
    TrafficSim sim;
    std::vector<Vertex> staticMesh;
    std::vector<Vertex> agentMesh;
    Renderer renderer;

    // Orbit camera around `target`. Scene units are metres: x east, y up, z = -north.
    Vec3 target{0.0f, 0.0f, 0.0f};
    float yaw = 0.6f;
    float pitch = 0.75f;
    float dist = 2200.0f;

    // Touch gesture state.
    bool gestureValid = false;
    int gesturePointers = 0;
    float g0x = 0.0f, g0y = 0.0f, g1x = 0.0f, g1y = 0.0f;
};

bool readAsset(AAssetManager* mgr, const char* name, std::vector<uint8_t>& out) {
    AAsset* asset = AAssetManager_open(mgr, name, AASSET_MODE_BUFFER);
    if (!asset) {
        LOGE("missing asset: %s", name);
        return false;
    }
    off_t len = AAsset_getLength(asset);
    out.resize(static_cast<size_t>(len > 0 ? len : 0));
    bool ok = !out.empty() && AAsset_read(asset, out.data(), out.size()) == static_cast<int>(out.size());
    AAsset_close(asset);
    if (!ok) LOGE("could not read asset: %s", name);
    return ok;
}

bool readSpirv(AAssetManager* mgr, const char* name, std::vector<uint32_t>& out) {
    std::vector<uint8_t> bytes;
    if (!readAsset(mgr, name, bytes) || bytes.size() % 4 != 0) return false;
    out.resize(bytes.size() / 4);
    std::memcpy(out.data(), bytes.data(), bytes.size());
    return true;
}

bool loadAssets(AppState& st) {
    AAssetManager* mgr = st.app->activity->assetManager;
    if (!readSpirv(mgr, "shaders/mesh.vert.spv", st.shaders.vert) ||
        !readSpirv(mgr, "shaders/mesh.frag.spv", st.shaders.frag)) {
        return false;
    }

    std::vector<uint8_t> terrain, buildings, roads;
    if (!readAsset(mgr, "district/terrain.bin", terrain) ||
        !readAsset(mgr, "district/buildings.bin", buildings) ||
        !readAsset(mgr, "district/roads.bin", roads)) {
        return false;
    }
    std::string err;
    if (!parseTerrain(terrain, st.district, err) || !parseBuildings(buildings, st.district, err) ||
        !parseRoads(roads, st.district, err)) {
        LOGE("district data rejected: %s", err.c_str());
        return false;
    }

    buildStaticMesh(st.district, st.staticMesh);
    st.sim.init(&st.district.roads, 20251015u, kCars, kPeople);
    st.target = {st.district.sizeX * 0.5f, 0.0f, -st.district.sizeZ * 0.5f};
    st.assetsLoaded = true;
    LOGI("loaded: %zu buildings, %zu road edges, %zu static vertices, %zu agents", st.district.buildings.size(),
         st.district.roads.edges.size(), st.staticMesh.size(), st.sim.agents().size());
    return true;
}

Mat4 computeViewProj(const AppState& st, float aspect) {
    const float cp = std::cos(st.pitch), sp = std::sin(st.pitch);
    const float sy = std::sin(st.yaw), cy = std::cos(st.yaw);
    const Vec3 offset{cp * sy, sp, cp * cy};
    const Vec3 eye = st.target + offset * st.dist;
    const Mat4 view = mat4LookAt(eye, st.target, Vec3{0.0f, 1.0f, 0.0f});
    const Mat4 proj = mat4Perspective(kFovY, aspect, 2.0f, 40000.0f);
    return mat4Mul(proj, view);
}

void onAppCmd(android_app* app, int32_t cmd) {
    AppState* st = static_cast<AppState*>(app->userData);
    if (!st) return;
    switch (cmd) {
        case APP_CMD_INIT_WINDOW:
            if (app->window) {
                if (!st->assetsLoaded && !loadAssets(*st)) {
                    LOGE("assets failed to load; nothing will be drawn");
                    break;
                }
                if (!st->renderer.init(app->window, st->shaders, st->staticMesh)) {
                    LOGE("renderer init failed: %s", st->renderer.lastError().c_str());
                }
            }
            break;
        case APP_CMD_TERM_WINDOW:
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
            st->pitch = std::min(std::max(st->pitch + dy * 0.004f, 0.08f), 1.45f);
        } else {
            // Two fingers: pinch to zoom, move both to pan.
            const float dPrev = std::hypot(st->g1x - st->g0x, st->g1y - st->g0y);
            const float dNow = std::hypot(x1 - x0, y1 - y0);
            if (dPrev > 1.0f && dNow > 1.0f) {
                st->dist = std::min(std::max(st->dist * dPrev / dNow, 60.0f), 15000.0f);
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

        st.sim.step(dt);
        buildAgentMesh(st.district, st.sim, st.agentMesh);

        const int32_t w = ANativeWindow_getWidth(app->window);
        const int32_t h = ANativeWindow_getHeight(app->window);
        const float aspect = (w > 0 && h > 0) ? static_cast<float>(w) / static_cast<float>(h) : 1.0f;
        if (!st.renderer.drawFrame(computeViewProj(st, aspect), st.agentMesh)) {
            usleep(16000);  // skipped frame (minimised or resizing); avoid spinning
        }
    }

    st.renderer.destroy();
    LOGI("exiting");
}
