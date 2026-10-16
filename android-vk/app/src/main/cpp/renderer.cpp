#include "renderer.h"

#include <android/log.h>

#include <algorithm>
#include <cstring>

#include "hud.h"

#define LOG_TAG "VkIsland"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static_assert(sizeof(Vertex) == 20, "Vertex must be 3 floats + 4 colour bytes + 4 normal bytes");
struct PushData {
    float viewProj[16];
    float offset[4];
};
static_assert(sizeof(PushData) == 80, "push constants: mat4 + vec4");
static_assert(sizeof(Mat4) == 64, "push constant is one mat4");

namespace {

void freeBuffer(VkDevice device, VkBuffer& buf, VkDeviceMemory& mem, void*& mapped) {
    if (mapped) {
        vkUnmapMemory(device, mem);
        mapped = nullptr;
    }
    if (buf != VK_NULL_HANDLE) vkDestroyBuffer(device, buf, nullptr);
    if (mem != VK_NULL_HANDLE) vkFreeMemory(device, mem, nullptr);
    buf = VK_NULL_HANDLE;
    mem = VK_NULL_HANDLE;
}

}  // namespace

bool Renderer::fail(const char* what, VkResult r) {
    err_ = std::string(what) + " failed (VkResult " + std::to_string(static_cast<int>(r)) + ")";
    LOGE("%s", err_.c_str());
    return false;
}

#define VK_TRY(call, what)                 \
    do {                                   \
        VkResult vr_ = (call);             \
        if (vr_ != VK_SUCCESS) return fail(what, vr_); \
    } while (0)

bool Renderer::init(ANativeWindow* window) {
    destroy();
    window_ = window;
    err_.clear();
    if (!window_) {
        err_ = "no window";
        return false;
    }
    if (!createInstance() || !createSurface() || !pickDevice() || !createDevice() ||
        !createRenderPass() || !createFrameResources() || !buildSwapchain()) {
        destroy();
        return false;
    }
    ready_ = true;  // a swapchain exists: clears work even before the scene is set
    LOGI("renderer ready: %ux%u", extent_.width, extent_.height);
    return true;
}

bool Renderer::uploadDeviceLocal(const void* data, VkDeviceSize bytes, VkBufferUsageFlags usage, Buffer& out) {
    Buffer staging;
    if (!createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging)) {
        return false;
    }
    std::memcpy(staging.mapped, data, static_cast<size_t>(bytes));

    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = bytes;
    bci.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult cr = vkCreateBuffer(device_, &bci, nullptr, &out.buf);
    if (cr != VK_SUCCESS) {
        freeBuffer(device_, staging.buf, staging.mem, staging.mapped);
        return fail("vkCreateBuffer(device-local)", cr);
    }
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device_, out.buf, &req);
    uint32_t type = 0;
    if (!findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, type)) {
        freeBuffer(device_, staging.buf, staging.mem, staging.mapped);
        err_ = "no device-local memory for mesh";
        return false;
    }
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    VkResult ar = vkAllocateMemory(device_, &mai, nullptr, &out.mem);
    if (ar != VK_SUCCESS) {
        freeBuffer(device_, staging.buf, staging.mem, staging.mapped);
        return fail("vkAllocateMemory(mesh)", ar);
    }
    VK_TRY(vkBindBufferMemory(device_, out.buf, out.mem, 0), "vkBindBufferMemory(mesh)");
    out.size = bytes;

    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = cmdPool_;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VK_TRY(vkAllocateCommandBuffers(device_, &cbai, &cb), "vkAllocateCommandBuffers(upload)");
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    VkBufferCopy region{0, 0, bytes};
    vkCmdCopyBuffer(cb, staging.buf, out.buf, 1, &region);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    VkResult sr = vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE);
    if (sr == VK_SUCCESS) vkQueueWaitIdle(queue_);
    vkFreeCommandBuffers(device_, cmdPool_, 1, &cb);
    freeBuffer(device_, staging.buf, staging.mem, staging.mapped);
    return sr == VK_SUCCESS || fail("vkQueueSubmit(upload)", sr);
}

void Renderer::freeMeshes() {
    if (!device_) {
        meshes_.clear();
        return;
    }
    for (GpuMesh& g : meshes_) {
        freeBuffer(device_, g.vb.buf, g.vb.mem, g.vb.mapped);
        freeBuffer(device_, g.ib.buf, g.ib.mem, g.ib.mapped);
    }
    meshes_.clear();
}

bool Renderer::setScene(const ShaderBlobs& shaders, const std::vector<MeshBlob>& meshes) {
    if (!device_) {
        err_ = "setScene before init";
        return false;
    }
    vkDeviceWaitIdle(device_);
    if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
    if (pipelineLayout_) vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
    pipeline_ = VK_NULL_HANDLE;
    pipelineLayout_ = VK_NULL_HANDLE;
    freeMeshes();

    if (!createPipeline(shaders)) return false;
    size_t totalVerts = 0, totalIdx = 0;
    for (const MeshBlob& m : meshes) {
        if (m.vertices.empty() || m.indices.empty()) continue;
        meshes_.push_back(GpuMesh{});
        GpuMesh& g = meshes_.back();
        g.indexCount = static_cast<uint32_t>(m.indices.size());
        for (int k = 0; k < 3; ++k) g.offset[k] = m.offset[k];
        if (!uploadDeviceLocal(m.vertices.data(), static_cast<VkDeviceSize>(m.vertices.size()) * sizeof(Vertex),
                               VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, g.vb)) {
            return false;
        }
        if (!uploadDeviceLocal(m.indices.data(), static_cast<VkDeviceSize>(m.indices.size()) * sizeof(uint32_t),
                               VK_BUFFER_USAGE_INDEX_BUFFER_BIT, g.ib)) {
            return false;
        }
        totalVerts += m.vertices.size();
        totalIdx += m.indices.size();
    }
    LOGI("scene set: %zu meshes, %zu vertices, %zu indices", meshes_.size(), totalVerts, totalIdx);
    return true;
}

bool Renderer::createInstance() {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "SingaporeVulkanPilot";
    app.applicationVersion = 1;
    app.pEngineName = "none";
    app.apiVersion = VK_API_VERSION_1_0;  // only core 1.0 is used, so older drivers still load

    const char* extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = 2;
    ci.ppEnabledExtensionNames = extensions;
    VK_TRY(vkCreateInstance(&ci, nullptr, &instance_), "vkCreateInstance");
    return true;
}

bool Renderer::createSurface() {
    VkAndroidSurfaceCreateInfoKHR sci{};
    sci.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    sci.window = window_;
    VK_TRY(vkCreateAndroidSurfaceKHR(instance_, &sci, nullptr, &surface_), "vkCreateAndroidSurfaceKHR");
    return true;
}

bool Renderer::pickDevice() {
    uint32_t count = 0;
    VK_TRY(vkEnumeratePhysicalDevices(instance_, &count, nullptr), "vkEnumeratePhysicalDevices");
    if (count == 0) {
        err_ = "no Vulkan devices";
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    VK_TRY(vkEnumeratePhysicalDevices(instance_, &count, devices.data()), "vkEnumeratePhysicalDevices");

    for (VkPhysicalDevice dev : devices) {
        // Needs the swapchain extension.
        uint32_t extCount = 0;
        vkEnumerateDeviceExtensionProperties(dev, nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> exts(extCount);
        vkEnumerateDeviceExtensionProperties(dev, nullptr, &extCount, exts.data());
        bool hasSwapchain = false;
        for (const auto& e : exts) {
            if (std::strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) hasSwapchain = true;
        }
        if (!hasSwapchain) continue;

        // Needs one queue family that can draw and present to this surface.
        uint32_t qCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qCount, nullptr);
        std::vector<VkQueueFamilyProperties> qprops(qCount);
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qCount, qprops.data());
        for (uint32_t q = 0; q < qCount; ++q) {
            if (!(qprops[q].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            VkBool32 presentOk = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(dev, q, surface_, &presentOk);
            if (presentOk) {
                physical_ = dev;
                queueFamily_ = q;
                vkGetPhysicalDeviceMemoryProperties(dev, &memProps_);
                return true;
            }
        }
    }
    err_ = "no device with a graphics+present queue and swapchain support";
    return false;
}

bool Renderer::createDevice() {
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = queueFamily_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    const char* devExts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = devExts;
    VK_TRY(vkCreateDevice(physical_, &dci, nullptr, &device_), "vkCreateDevice");
    vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);

    // Colour format: prefer 8-bit UNORM so the shader output is shown as written.
    uint32_t fCount = 0;
    VK_TRY(vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &fCount, nullptr), "surface formats");
    std::vector<VkSurfaceFormatKHR> formats(fCount);
    VK_TRY(vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &fCount, formats.data()), "surface formats");
    colorFormat_ = VK_FORMAT_R8G8B8A8_UNORM;
    colorSpace_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    bool found = false;
    for (const auto& f : formats) {
        if (f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
            (f.format == VK_FORMAT_R8G8B8A8_UNORM || f.format == VK_FORMAT_B8G8R8A8_UNORM)) {
            colorFormat_ = f.format;
            found = true;
            break;
        }
    }
    if (!found && !formats.empty() && formats[0].format != VK_FORMAT_UNDEFINED) {
        colorFormat_ = formats[0].format;
        colorSpace_ = formats[0].colorSpace;
    }

    // Depth format: first candidate that supports optimal depth attachments.
    const VkFormat candidates[] = {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM};
    depthFormat_ = VK_FORMAT_UNDEFINED;
    for (VkFormat f : candidates) {
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(physical_, f, &props);
        if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            depthFormat_ = f;
            break;
        }
    }
    if (depthFormat_ == VK_FORMAT_UNDEFINED) {
        err_ = "no supported depth format";
        return false;
    }
    return true;
}

bool Renderer::createRenderPass() {
    VkAttachmentDescription att[2]{};
    att[0].format = colorFormat_;
    att[0].samples = VK_SAMPLE_COUNT_1_BIT;
    att[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    att[1].format = depthFormat_;
    att[1].samples = VK_SAMPLE_COUNT_1_BIT;
    att[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &colorRef;
    sub.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpci{};
    rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = 2;
    rpci.pAttachments = att;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sub;
    rpci.dependencyCount = 1;
    rpci.pDependencies = &dep;
    VK_TRY(vkCreateRenderPass(device_, &rpci, nullptr, &renderPass_), "vkCreateRenderPass");
    return true;
}

bool Renderer::createPipeline(const ShaderBlobs& shaders) {
    auto makeModule = [this](const std::vector<uint32_t>& code, VkShaderModule& out) -> bool {
        VkShaderModuleCreateInfo smci{};
        smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smci.codeSize = code.size() * sizeof(uint32_t);
        smci.pCode = code.data();
        VkResult r = vkCreateShaderModule(device_, &smci, nullptr, &out);
        return r == VK_SUCCESS || fail("vkCreateShaderModule", r);
    };
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    if (!makeModule(shaders.vert, vs) || !makeModule(shaders.frag, fs)) {
        if (vs) vkDestroyShaderModule(device_, vs, nullptr);
        if (fs) vkDestroyShaderModule(device_, fs, nullptr);
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[3] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
        {1, 0, VK_FORMAT_R8G8B8A8_UNORM, 12},
        {2, 0, VK_FORMAT_R8G8B8A8_SNORM, 16},
    };
    VkPipelineVertexInputStateCreateInfo vis{};
    vis.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vis.vertexBindingDescriptionCount = 1;
    vis.pVertexBindingDescriptions = &binding;
    vis.vertexAttributeDescriptionCount = 3;
    vis.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vps{};
    vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vps.viewportCount = 1;  // viewport and scissor are dynamic
    vps.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;  // winding is not guaranteed; shading is two-sided
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                         VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cbs{};
    cbs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cbs.attachmentCount = 1;
    cbs.pAttachments = &cba;

    VkDynamicState dynStates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynStates;

    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pcr.offset = 0;
    pcr.size = sizeof(PushData);
    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    VkResult lr = vkCreatePipelineLayout(device_, &plci, nullptr, &pipelineLayout_);

    VkResult pr = VK_ERROR_UNKNOWN;
    if (lr == VK_SUCCESS) {
        VkGraphicsPipelineCreateInfo gpci{};
        gpci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gpci.stageCount = 2;
        gpci.pStages = stages;
        gpci.pVertexInputState = &vis;
        gpci.pInputAssemblyState = &ia;
        gpci.pViewportState = &vps;
        gpci.pRasterizationState = &rs;
        gpci.pMultisampleState = &ms;
        gpci.pDepthStencilState = &ds;
        gpci.pColorBlendState = &cbs;
        gpci.pDynamicState = &dyn;
        gpci.layout = pipelineLayout_;
        gpci.renderPass = renderPass_;
        gpci.subpass = 0;
        pr = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipeline_);
    }
    vkDestroyShaderModule(device_, vs, nullptr);
    vkDestroyShaderModule(device_, fs, nullptr);
    if (lr != VK_SUCCESS) return fail("vkCreatePipelineLayout", lr);
    if (pr != VK_SUCCESS) return fail("vkCreateGraphicsPipelines", pr);
    return true;
}

bool Renderer::createFrameResources() {
    VkCommandPoolCreateInfo cpci{};
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = queueFamily_;
    VK_TRY(vkCreateCommandPool(device_, &cpci, nullptr, &cmdPool_), "vkCreateCommandPool");

    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = cmdPool_;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = kFramesInFlight;
    VK_TRY(vkAllocateCommandBuffers(device_, &cbai, cmd_), "vkAllocateCommandBuffers");

    VkSemaphoreCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (int i = 0; i < kFramesInFlight; ++i) {
        VK_TRY(vkCreateSemaphore(device_, &sci, nullptr, &imageAvailable_[i]), "vkCreateSemaphore");
        VK_TRY(vkCreateFence(device_, &fci, nullptr, &inFlight_[i]), "vkCreateFence");
        if (!createBuffer(static_cast<VkDeviceSize>(kMaxAgentVertices) * sizeof(Vertex),
                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, agents_[i])) {
            return false;
        }
        if (!createBuffer(static_cast<VkDeviceSize>(kMaxHudVertices) * sizeof(Vertex),
                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, hud_[i])) {
            return false;
        }
    }
    return true;
}

bool Renderer::findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props, uint32_t& index) const {
    for (uint32_t i = 0; i < memProps_.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) && (memProps_.memoryTypes[i].propertyFlags & props) == props) {
            index = i;
            return true;
        }
    }
    return false;
}

bool Renderer::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props, Buffer& out) {
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_TRY(vkCreateBuffer(device_, &bci, nullptr, &out.buf), "vkCreateBuffer");

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device_, out.buf, &req);
    uint32_t type = 0;
    if (!findMemoryType(req.memoryTypeBits, props, type)) {
        err_ = "no suitable memory type for buffer";
        return false;
    }
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    VK_TRY(vkAllocateMemory(device_, &mai, nullptr, &out.mem), "vkAllocateMemory");
    VK_TRY(vkBindBufferMemory(device_, out.buf, out.mem, 0), "vkBindBufferMemory");
    VK_TRY(vkMapMemory(device_, out.mem, 0, size, 0, &out.mapped), "vkMapMemory");
    out.size = size;
    return true;
}

bool Renderer::createDepthBuffer() {
    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = depthFormat_;
    ici.extent = {extent_.width, extent_.height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_TRY(vkCreateImage(device_, &ici, nullptr, &depthImage_), "vkCreateImage(depth)");

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device_, depthImage_, &req);
    uint32_t type = 0;
    if (!findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, type)) {
        err_ = "no memory type for depth buffer";
        return false;
    }
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    VK_TRY(vkAllocateMemory(device_, &mai, nullptr, &depthMem_), "vkAllocateMemory(depth)");
    VK_TRY(vkBindImageMemory(device_, depthImage_, depthMem_, 0), "vkBindImageMemory");

    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = depthImage_;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = depthFormat_;
    vci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    VK_TRY(vkCreateImageView(device_, &vci, nullptr, &depthView_), "vkCreateImageView(depth)");
    return true;
}

// Experiment (build 15): the swapchain is already the landscape size of the screen, so the picture uses
// its width and height as they are, with no extra turn. Earlier builds swapped them, which gave a portrait
// size for a landscape picture (the island looked flattened and touches were mirrored).
uint32_t Renderer::displayWidth() const {
    return extent_.width;
}

uint32_t Renderer::displayHeight() const {
    return extent_.height;
}

namespace {

// Clip-space map from the upright picture to the stored picture: the presentation engine turns the stored
// picture clockwise by `deg`, so the stored picture is drawn turned counter-clockwise by `deg`. Only x and y
// change, so depth and w are untouched. Column-major, as Mat4 is.
Mat4 preRotation(int deg) {
    Mat4 r = mat4Identity();
    switch (deg) {
        case 90:  // (x, y) -> (y, -x)
            r.m[0] = 0.0f; r.m[4] = 1.0f; r.m[1] = -1.0f; r.m[5] = 0.0f;
            break;
        case 180:  // (x, y) -> (-x, -y)
            r.m[0] = -1.0f; r.m[5] = -1.0f;
            break;
        case 270:  // (x, y) -> (-y, x)
            r.m[0] = 0.0f; r.m[4] = -1.0f; r.m[1] = 1.0f; r.m[5] = 0.0f;
            break;
        default:
            break;
    }
    return r;
}

}  // namespace

bool Renderer::buildSwapchain() {
    VkSurfaceCapabilitiesKHR caps{};
    VK_TRY(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps), "surface capabilities");

    // How the presentation engine turns the stored picture for the screen (Vulkan: ROTATE_90 is clockwise).
    switch (caps.currentTransform) {
        case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR: rotationDeg_ = 90; break;
        case VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR: rotationDeg_ = 180; break;
        case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR: rotationDeg_ = 270; break;
        default: rotationDeg_ = 0; break;
    }
    LOGI("surface transform %d (%d deg), currentExtent %ux%u", static_cast<int>(caps.currentTransform),
         rotationDeg_, caps.currentExtent.width, caps.currentExtent.height);

    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xFFFFFFFFu) {
        ext.width = static_cast<uint32_t>(ANativeWindow_getWidth(window_));
        ext.height = static_cast<uint32_t>(ANativeWindow_getHeight(window_));
    }
    ext.width = std::min(std::max(ext.width, caps.minImageExtent.width), caps.maxImageExtent.width);
    ext.height = std::min(std::max(ext.height, caps.minImageExtent.height), caps.maxImageExtent.height);
    if (ext.width == 0 || ext.height == 0) return true;  // minimised: nothing to draw into yet

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) imageCount = caps.maxImageCount;

    VkCompositeAlphaFlagBitsKHR composite = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & composite)) {
        const VkCompositeAlphaFlagBitsKHR options[] = {VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                                                       VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
                                                       VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR};
        for (VkCompositeAlphaFlagBitsKHR o : options) {
            if (caps.supportedCompositeAlpha & o) {
                composite = o;
                break;
            }
        }
    }

    VkSwapchainCreateInfoKHR sci{};
    sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    sci.surface = surface_;
    sci.minImageCount = imageCount;
    sci.imageFormat = colorFormat_;
    sci.imageColorSpace = colorSpace_;
    sci.imageExtent = ext;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = composite;
    sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;  // always supported; vsync
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = VK_NULL_HANDLE;
    VK_TRY(vkCreateSwapchainKHR(device_, &sci, nullptr, &swapchain_), "vkCreateSwapchainKHR");
    extent_ = ext;

    uint32_t count = 0;
    VK_TRY(vkGetSwapchainImagesKHR(device_, swapchain_, &count, nullptr), "vkGetSwapchainImagesKHR");
    swapImages_.resize(count);
    VK_TRY(vkGetSwapchainImagesKHR(device_, swapchain_, &count, swapImages_.data()), "vkGetSwapchainImagesKHR");

    swapViews_.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = swapImages_[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = colorFormat_;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_TRY(vkCreateImageView(device_, &vci, nullptr, &swapViews_[i]), "vkCreateImageView(swapchain)");
    }

    if (!createDepthBuffer()) return false;

    framebuffers_.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        VkImageView views[2] = {swapViews_[i], depthView_};
        VkFramebufferCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass = renderPass_;
        fci.attachmentCount = 2;
        fci.pAttachments = views;
        fci.width = extent_.width;
        fci.height = extent_.height;
        fci.layers = 1;
        VK_TRY(vkCreateFramebuffer(device_, &fci, nullptr, &framebuffers_[i]), "vkCreateFramebuffer");
    }

    renderFinished_.resize(count);
    VkSemaphoreCreateInfo semi{};
    semi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (uint32_t i = 0; i < count; ++i) {
        VK_TRY(vkCreateSemaphore(device_, &semi, nullptr, &renderFinished_[i]), "vkCreateSemaphore");
    }
    return true;
}

void Renderer::destroySwapchain() {
    if (!device_) return;
    for (VkSemaphore s : renderFinished_) vkDestroySemaphore(device_, s, nullptr);
    renderFinished_.clear();
    for (VkFramebuffer fb : framebuffers_) vkDestroyFramebuffer(device_, fb, nullptr);
    framebuffers_.clear();
    if (depthView_) vkDestroyImageView(device_, depthView_, nullptr);
    if (depthImage_) vkDestroyImage(device_, depthImage_, nullptr);
    if (depthMem_) vkFreeMemory(device_, depthMem_, nullptr);
    depthView_ = VK_NULL_HANDLE;
    depthImage_ = VK_NULL_HANDLE;
    depthMem_ = VK_NULL_HANDLE;
    for (VkImageView v : swapViews_) vkDestroyImageView(device_, v, nullptr);
    swapViews_.clear();
    swapImages_.clear();
    if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
}

bool Renderer::drawFrame(const Mat4& viewProj, const float agentOffset[3], const std::vector<Vertex>& agents,
                         const std::vector<Vertex>& hud) {
    if (!ready_) return false;
    if (needsRecreate_) {
        vkDeviceWaitIdle(device_);
        destroySwapchain();
        needsRecreate_ = false;
    }
    if (swapchain_ == VK_NULL_HANDLE) {
        if (!buildSwapchain()) {
            ready_ = false;
            return false;
        }
        if (swapchain_ == VK_NULL_HANDLE) return false;  // still minimised
    }

    const int f = frame_;
    vkWaitForFences(device_, 1, &inFlight_[f], VK_TRUE, UINT64_MAX);

    uint32_t imageIndex = 0;
    VkResult ar = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, imageAvailable_[f], VK_NULL_HANDLE,
                                        &imageIndex);
    if (ar == VK_ERROR_OUT_OF_DATE_KHR) {
        needsRecreate_ = true;
        return false;
    }
    if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) {
        fail("vkAcquireNextImageKHR", ar);
        ready_ = false;
        return false;
    }
    if (ar == VK_SUBOPTIMAL_KHR) needsRecreate_ = true;
    vkResetFences(device_, 1, &inFlight_[f]);

    // This frame's agent buffer is free: its fence has signalled.
    const uint32_t agentVerts =
        static_cast<uint32_t>(std::min<size_t>(agents.size(), static_cast<size_t>(kMaxAgentVertices)));
    if (agentVerts > 0) std::memcpy(agents_[f].mapped, agents.data(), agentVerts * sizeof(Vertex));
    const uint32_t hudVerts = static_cast<uint32_t>(std::min<size_t>(hud.size(), kMaxHudVertices));
    if (hudVerts > 0) std::memcpy(hud_[f].mapped, hud.data(), hudVerts * sizeof(Vertex));

    VkCommandBuffer cb = cmd_[f];
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);

    VkClearValue clears[2]{};
    clears[0].color = {{clear_[0], clear_[1], clear_[2], 1.0f}};
    clears[1].depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo rpb{};
    rpb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpb.renderPass = renderPass_;
    rpb.framebuffer = framebuffers_[imageIndex];
    rpb.renderArea = {{0, 0}, extent_};
    rpb.clearValueCount = 2;
    rpb.pClearValues = clears;
    vkCmdBeginRenderPass(cb, &rpb, VK_SUBPASS_CONTENTS_INLINE);

    if (pipeline_) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        VkViewport vp{0.0f, 0.0f, static_cast<float>(extent_.width), static_cast<float>(extent_.height), 0.0f, 1.0f};
        VkRect2D sc{{0, 0}, extent_};
        vkCmdSetViewport(cb, 0, 1, &vp);
        vkCmdSetScissor(cb, 0, 1, &sc);

        // The scene is built for the upright picture; pre-rotate it into the stored picture.
        const Mat4 pre = mat4Identity();  // experiment: no extra turn (see displayWidth)
        const Mat4 sceneViewProj = mat4Mul(pre, viewProj);
        PushData pd{};
        std::memcpy(pd.viewProj, sceneViewProj.m, sizeof(pd.viewProj));
        VkDeviceSize offset = 0;
        for (const GpuMesh& g : meshes_) {
            pd.offset[0] = g.offset[0];
            pd.offset[1] = g.offset[1];
            pd.offset[2] = g.offset[2];
            pd.offset[3] = 0.0f;
            vkCmdPushConstants(cb, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PushData), &pd);
            vkCmdBindVertexBuffers(cb, 0, 1, &g.vb.buf, &offset);
            vkCmdBindIndexBuffer(cb, g.ib.buf, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cb, g.indexCount, 1, 0, 0, 0);
        }
        if (agentVerts > 0) {
            pd.offset[0] = agentOffset[0];
            pd.offset[1] = agentOffset[1];
            pd.offset[2] = agentOffset[2];
            pd.offset[3] = 0.0f;
            vkCmdPushConstants(cb, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PushData), &pd);
            vkCmdBindVertexBuffers(cb, 0, 1, &agents_[f].buf, &offset);
            vkCmdDraw(cb, agentVerts, 1, 0, 0);
        }
        if (hudVerts > 0) {
            // HUD: screen coordinates with no offset. Build 15 showed the HUD text sideways (bottom to top)
            // with no extra turn; build 14 showed it upright with a 270 degree turn (180 then 90), so the HUD
            // gets that turn here. The scene keeps its own transform (identity).
            const Mat4 hudPre = preRotation(270);
            std::memcpy(pd.viewProj, hudPre.m, sizeof(pd.viewProj));
            pd.offset[0] = pd.offset[1] = pd.offset[2] = pd.offset[3] = 0.0f;
            vkCmdPushConstants(cb, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PushData), &pd);
            vkCmdBindVertexBuffers(cb, 0, 1, &hud_[f].buf, &offset);
            vkCmdDraw(cb, hudVerts, 1, 0, 0);
        }
    }
    vkCmdEndRenderPass(cb);
    if (vkEndCommandBuffer(cb) != VK_SUCCESS) {
        err_ = "vkEndCommandBuffer failed";
        ready_ = false;
        return false;
    }

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &imageAvailable_[f];
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &renderFinished_[imageIndex];
    VkResult sr = vkQueueSubmit(queue_, 1, &si, inFlight_[f]);
    if (sr != VK_SUCCESS) {
        fail("vkQueueSubmit", sr);
        ready_ = false;
        return false;
    }

    VkPresentInfoKHR pi{};
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &renderFinished_[imageIndex];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &imageIndex;
    VkResult pr = vkQueuePresentKHR(queue_, &pi);
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) {
        needsRecreate_ = true;
    } else if (pr != VK_SUCCESS) {
        fail("vkQueuePresentKHR", pr);
        ready_ = false;
        return false;
    }

    frame_ = (frame_ + 1) % kFramesInFlight;
    return true;
}

void Renderer::destroy() {
    ready_ = false;
    needsRecreate_ = false;
    if (device_) {
        vkDeviceWaitIdle(device_);
        destroySwapchain();
        for (int i = 0; i < kFramesInFlight; ++i) {
            if (imageAvailable_[i]) vkDestroySemaphore(device_, imageAvailable_[i], nullptr);
            if (inFlight_[i]) vkDestroyFence(device_, inFlight_[i], nullptr);
            imageAvailable_[i] = VK_NULL_HANDLE;
            inFlight_[i] = VK_NULL_HANDLE;
            freeBuffer(device_, agents_[i].buf, agents_[i].mem, agents_[i].mapped);
            freeBuffer(device_, hud_[i].buf, hud_[i].mem, hud_[i].mapped);
        }
        if (cmdPool_) vkDestroyCommandPool(device_, cmdPool_, nullptr);
        cmdPool_ = VK_NULL_HANDLE;
        for (int i = 0; i < kFramesInFlight; ++i) cmd_[i] = VK_NULL_HANDLE;
        if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
        if (pipelineLayout_) vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
        if (renderPass_) vkDestroyRenderPass(device_, renderPass_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
        pipelineLayout_ = VK_NULL_HANDLE;
        renderPass_ = VK_NULL_HANDLE;
        freeMeshes();
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (surface_ && instance_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
    surface_ = VK_NULL_HANDLE;
    if (instance_) vkDestroyInstance(instance_, nullptr);
    instance_ = VK_NULL_HANDLE;
    physical_ = VK_NULL_HANDLE;
    queue_ = VK_NULL_HANDLE;
    frame_ = 0;
}
