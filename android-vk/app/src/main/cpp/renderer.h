// Minimal Vulkan 1.0 renderer for Android. Indexed meshes live in device-local memory,
// uploaded once through a staging buffer. Agents use a small per-frame host-visible buffer.
#pragma once

#include <android/native_window.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

#include "core_math.h"
#include "world.h"

struct ShaderBlobs {
    std::vector<uint32_t> vert;
    std::vector<uint32_t> frag;
};

class Renderer {
public:
    // Creates the instance, surface, device, render pass and swapchain. Clears work after this,
    // even before a scene exists, so the screen can show status colours.
    bool init(ANativeWindow* window);

    // Builds the pipeline and uploads the meshes. May be called again later.
    bool setScene(const ShaderBlobs& shaders, const std::vector<MeshBlob>& meshes);

    void setClearColor(float r, float g, float b) {
        clear_[0] = r;
        clear_[1] = g;
        clear_[2] = b;
    }
    bool hasScene() const { return pipeline_ != VK_NULL_HANDLE; }
    bool ready() const { return ready_; }

    // Size of the picture as the user sees it (the screen orientation). The swapchain may be stored
    // rotated by 90 or 270 degrees; in that case the width and height are swapped here.
    uint32_t displayWidth() const;
    uint32_t displayHeight() const;
    // Clockwise turn the presentation engine applies to the stored picture: 0, 90, 180 or 270 (Vulkan's
    // currentTransform). The renderer pre-rotates its clip space by the inverse so the picture is upright.
    int displayRotation() const { return rotationDeg_; }
    const std::string& lastError() const { return err_; }
    void destroy();

    // Draws one frame. Meshes are drawn with their own offsets; agents with `agentOffset`.
    // `hud` is in normalised device coordinates and is drawn last, on top of the scene.
    // Returns false when the frame was skipped (for example, a minimised window).
    bool drawFrame(const Mat4& viewProj, const float agentOffset[3], const std::vector<Vertex>& agents,
                   const std::vector<Vertex>& hud);

private:
    struct Buffer {
        VkBuffer buf = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        void* mapped = nullptr;
        VkDeviceSize size = 0;
    };
    struct GpuMesh {
        Buffer vb;
        Buffer ib;
        uint32_t indexCount = 0;
        float offset[3] = {0.0f, 0.0f, 0.0f};
    };

    static constexpr int kFramesInFlight = 2;
    static constexpr uint32_t kMaxAgentVertices = 36u * 8192u;

    bool fail(const char* what, VkResult r);  // records the error and returns false
    bool createInstance();
    bool createSurface();
    bool pickDevice();
    bool createDevice();
    bool createRenderPass();
    bool createPipeline(const ShaderBlobs& shaders);
    bool createFrameResources();
    bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props, Buffer& out);
    bool uploadDeviceLocal(const void* data, VkDeviceSize bytes, VkBufferUsageFlags usage, Buffer& out);
    void freeMeshes();
    bool findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props, uint32_t& index) const;
    bool buildSwapchain();
    void destroySwapchain();
    bool createDepthBuffer();

    ANativeWindow* window_ = nullptr;
    bool ready_ = false;
    bool needsRecreate_ = false;
    float clear_[3] = {0.62f, 0.78f, 0.92f};
    std::string err_;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;
    VkPhysicalDeviceMemoryProperties memProps_{};

    VkFormat colorFormat_ = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR colorSpace_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkFormat depthFormat_ = VK_FORMAT_UNDEFINED;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkExtent2D extent_{0, 0};
    int rotationDeg_ = 0;  // clockwise turn of the stored picture, from currentTransform
    std::vector<VkImage> swapImages_;
    std::vector<VkImageView> swapViews_;
    std::vector<VkFramebuffer> framebuffers_;
    std::vector<VkSemaphore> renderFinished_;  // one per swapchain image

    VkImage depthImage_ = VK_NULL_HANDLE;
    VkDeviceMemory depthMem_ = VK_NULL_HANDLE;
    VkImageView depthView_ = VK_NULL_HANDLE;

    VkCommandPool cmdPool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_[kFramesInFlight] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkSemaphore imageAvailable_[kFramesInFlight] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkFence inFlight_[kFramesInFlight] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    Buffer agents_[kFramesInFlight];
    Buffer hud_[kFramesInFlight];
    int frame_ = 0;

    std::vector<GpuMesh> meshes_;
};
