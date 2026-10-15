// Minimal Vulkan 1.1 renderer for Android: one static mesh (terrain, buildings, roads)
// plus a per-frame dynamic mesh (agents). Depth-tested, no textures.
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
    // Creates the instance, surface, device, swapchain and pipeline, and uploads the static mesh.
    bool init(ANativeWindow* window, const ShaderBlobs& shaders, const std::vector<Vertex>& staticMesh);
    void destroy();

    bool ready() const { return ready_; }
    const std::string& lastError() const { return err_; }

    // Draws one frame. Returns false when the frame was skipped (for example, a minimised window).
    bool drawFrame(const Mat4& viewProj, const std::vector<Vertex>& agents);

private:
    struct Buffer {
        VkBuffer buf = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        void* mapped = nullptr;
        VkDeviceSize size = 0;
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
    bool findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props, uint32_t& index) const;
    bool buildSwapchain();
    void destroySwapchain();
    bool createDepthBuffer();

    ANativeWindow* window_ = nullptr;
    bool ready_ = false;
    bool needsRecreate_ = false;
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
    int frame_ = 0;

    Buffer staticMesh_;
    uint32_t staticVertexCount_ = 0;
};
