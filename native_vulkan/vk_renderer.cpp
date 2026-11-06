#include "vk_renderer.h"
#include "vk_shaders_spv.h"
#define VK_USE_PLATFORM_WIN32_KHR
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <cstring>
#include <cstdio>

#define MAX_SPRITES 4096
#define MAX_TEXTURES 512

struct TexSlot {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE; // 0 = 用全局默认 sampler
    int w = 0, h = 0;
    size_t bytes = 0;
    bool used = false;
    std::vector<float> verts;
};

struct RenderTarget {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE;
    VkRenderPass rp = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    int w = 0, h = 0;
    bool used = false;
    std::vector<float> verts;
};

struct VkRenderer::Impl {
    HWND hwnd = nullptr;
    int width = 0, height = 0;
    HMODULE dll = nullptr;

    PFN_vkGetInstanceProcAddr gipa = nullptr;
    PFN_vkDestroyInstance destroyInstance = nullptr;
    PFN_vkDestroyDevice destroyDevice = nullptr;
    PFN_vkDestroySwapchainKHR destroySwapchain = nullptr;
    PFN_vkDestroyRenderPass destroyRenderPass = nullptr;
    PFN_vkDestroyFramebuffer destroyFramebuffer = nullptr;
    PFN_vkDestroyImageView destroyImageView = nullptr;
    PFN_vkDestroyImage destroyImage = nullptr;
    PFN_vkDestroyBuffer destroyBuffer = nullptr;
    PFN_vkDestroyShaderModule destroyShader = nullptr;
    PFN_vkDestroyPipeline destroyPipeline = nullptr;
    PFN_vkDestroyPipelineLayout destroyPipelineLayout = nullptr;
    PFN_vkDestroyDescriptorSetLayout destroyDSL = nullptr;
    PFN_vkDestroyDescriptorPool destroyDP = nullptr;
    PFN_vkDestroySampler destroySampler = nullptr;
    PFN_vkDestroyCommandPool destroyCmdPool = nullptr;
    PFN_vkFreeMemory freeMemory = nullptr;

    VkInstance inst = VK_NULL_HANDLE;
    VkSurfaceKHR surf = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t qidx = 0;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    std::vector<VkImage> swapImages;
    std::vector<VkImageView> swapViews;
    VkFormat colorFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};

    VkRenderPass renderPass = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers;

    VkCommandPool cmdPool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;

    VkShaderModule vsMod = VK_NULL_HANDLE, fsMod = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;

    VkBuffer vb = VK_NULL_HANDLE;
    VkDeviceMemory vbMem = VK_NULL_HANDLE;
    float* vbPtr = nullptr;
    size_t vbBytes = 0;

    VkBuffer ubo = VK_NULL_HANDLE;
    VkDeviceMemory uboMem = VK_NULL_HANDLE;
    float* uboPtr = nullptr;

    std::vector<TexSlot> textures;
    std::vector<RenderTarget> targets;
    uint32_t currentTarget = 0;
    uint32_t frameImageIndex = 0;
    bool frameOpen = false;
    float clearR = 0, clearG = 0, clearB = 0, clearA = 1;
    const char* lastError = "";
};

// ---- global function pointers (single instance) ----
static PFN_vkCreateImage g_createImage;
static PFN_vkGetImageMemoryRequirements g_getImgReqs;
static PFN_vkBindImageMemory g_bindImgMem;
static PFN_vkCreateImageView g_createView;
static PFN_vkCreateBuffer g_createBuf;
static PFN_vkGetBufferMemoryRequirements g_getBufReqs;
static PFN_vkAllocateMemory g_allocMem;
static PFN_vkBindBufferMemory g_bindBufMem;
static PFN_vkMapMemory g_mapMem;
static PFN_vkUnmapMemory g_unmapMem;
static PFN_vkCreateSampler g_createSampler;
static PFN_vkCreateDescriptorSetLayout g_createDSL;
static PFN_vkCreateDescriptorPool g_createDP;
static PFN_vkAllocateDescriptorSets g_allocDS;
static PFN_vkUpdateDescriptorSets g_updateDS;
static PFN_vkCmdPipelineBarrier g_cmdBarrier;
static PFN_vkCmdCopyBufferToImage g_cmdCopyImg;
static PFN_vkCmdBindPipeline g_bindPipe;
static PFN_vkCmdBindDescriptorSets g_bindDS;
static PFN_vkCmdBindVertexBuffers g_bindVB;
static PFN_vkCmdDraw g_cmdDraw;
static PFN_vkCmdBeginRenderPass g_beginRP;
static PFN_vkCmdEndRenderPass g_endRP;
static PFN_vkBeginCommandBuffer g_beginCB;
static PFN_vkEndCommandBuffer g_endCB;
static PFN_vkResetCommandBuffer g_resetCB;
static PFN_vkQueueSubmit g_submit;
static PFN_vkQueueWaitIdle g_waitIdle;
static PFN_vkAcquireNextImageKHR g_acquire;
static PFN_vkQueuePresentKHR g_present;
static PFN_vkCreateShaderModule g_createShader;
static PFN_vkCreatePipelineLayout g_createPL;
static PFN_vkCreateGraphicsPipelines g_createPipe;

static PFN_vkGetInstanceProcAddr g_gipa = nullptr;

static uint32_t FindMemType(const VkPhysicalDeviceMemoryProperties& mp, uint32_t bits, VkMemoryPropertyFlags flags) {
    for (uint32_t i = 0; i < 32; i++) {
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & flags) == flags) return i;
    }
    return 0;
}

// 内嵌着色器的顶点格式：loc0=Position(vec4) loc1=Alpha loc2=ColorMultiplier(vec4)
// loc3=ColorOffset(vec4) loc4=TexCoord(vec2)，共 15 个 float
static void PushLimeVertex(std::vector<float>& out, float x, float y, float u, float v) {
    out.push_back(x);
    out.push_back(y);
    out.push_back(0.0f);
    out.push_back(1.0f);
    out.push_back(1.0f);                                   // alpha
    out.push_back(1.0f); out.push_back(1.0f); out.push_back(1.0f); out.push_back(1.0f); // colorMultiplier
    out.push_back(0.0f); out.push_back(0.0f); out.push_back(0.0f); out.push_back(0.0f); // colorOffset
    out.push_back(u);
    out.push_back(v);
}

static std::vector<char> ReadFileLocal(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return {};
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    std::vector<char> buf(sz);
    if (sz > 0) fread(buf.data(), 1, sz, f);
    fclose(f);
    return buf;
}

static std::string ExeDirLocal() {
    char p[MAX_PATH];
    GetModuleFileNameA(nullptr, p, MAX_PATH);
    std::string s(p);
    size_t pos = s.find_last_of("\\/");
    return pos == std::string::npos ? std::string("") : s.substr(0, pos + 1);
}

VkRenderer::VkRenderer() : impl(new Impl()) {}

VkRenderer::~VkRenderer() {
    Shutdown();
    delete impl;
}

const char* VkRenderer::LastError() const { return impl->lastError; }

bool VkRenderer::Init(HWND hwnd, int width, int height) {
    Impl& o = *impl;
    o.hwnd = hwnd;
    o.width = width;
    o.height = height;

    o.dll = LoadLibraryA("vulkan-1.dll");
    if (!o.dll) { o.lastError = "vulkan-1.dll missing"; return false; }
    o.gipa = (PFN_vkGetInstanceProcAddr)GetProcAddress(o.dll, "vkGetInstanceProcAddr");
    auto createInstance = (PFN_vkCreateInstance)GetProcAddress(o.dll, "vkCreateInstance");
    if (!o.gipa || !createInstance) { o.lastError = "vulkan entrypoints missing"; return false; }
    g_gipa = o.gipa;

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Lime Vulkan";
    app.apiVersion = VK_API_VERSION_1_0;

    const char* exts[] = {"VK_KHR_surface", "VK_KHR_win32_surface"};
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = exts;

    if (createInstance(&ici, nullptr, &o.inst) != VK_SUCCESS) { o.lastError = "instance failed"; return false; }

    auto enumDev = (PFN_vkEnumeratePhysicalDevices)o.gipa(o.inst, "vkEnumeratePhysicalDevices");
    auto getProps = (PFN_vkGetPhysicalDeviceProperties)o.gipa(o.inst, "vkGetPhysicalDeviceProperties");
    auto getMemProps = (PFN_vkGetPhysicalDeviceMemoryProperties)o.gipa(o.inst, "vkGetPhysicalDeviceMemoryProperties");
    auto getQFP = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)o.gipa(o.inst, "vkGetPhysicalDeviceQueueFamilyProperties");
    auto getSurfSupport = (PFN_vkGetPhysicalDeviceSurfaceSupportKHR)o.gipa(o.inst, "vkGetPhysicalDeviceSurfaceSupportKHR");
    auto createSurface = (PFN_vkCreateWin32SurfaceKHR)o.gipa(o.inst, "vkCreateWin32SurfaceKHR");
    auto getSurfCaps = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)o.gipa(o.inst, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    auto getSurfFormats = (PFN_vkGetPhysicalDeviceSurfaceFormatsKHR)o.gipa(o.inst, "vkGetPhysicalDeviceSurfaceFormatsKHR");
    auto createDevice = (PFN_vkCreateDevice)o.gipa(o.inst, "vkCreateDevice");
    auto createSwap = (PFN_vkCreateSwapchainKHR)o.gipa(o.inst, "vkCreateSwapchainKHR");
    auto getImages = (PFN_vkGetSwapchainImagesKHR)o.gipa(o.inst, "vkGetSwapchainImagesKHR");
    auto createRP = (PFN_vkCreateRenderPass)o.gipa(o.inst, "vkCreateRenderPass");
    auto createFB = (PFN_vkCreateFramebuffer)o.gipa(o.inst, "vkCreateFramebuffer");
    auto createPool = (PFN_vkCreateCommandPool)o.gipa(o.inst, "vkCreateCommandPool");
    auto allocCB = (PFN_vkAllocateCommandBuffers)o.gipa(o.inst, "vkAllocateCommandBuffers");

    o.destroyInstance = (PFN_vkDestroyInstance)o.gipa(o.inst, "vkDestroyInstance");
    o.destroyDevice = (PFN_vkDestroyDevice)o.gipa(o.inst, "vkDestroyDevice");
    o.destroySwapchain = (PFN_vkDestroySwapchainKHR)o.gipa(o.inst, "vkDestroySwapchainKHR");
    o.destroyRenderPass = (PFN_vkDestroyRenderPass)o.gipa(o.inst, "vkDestroyRenderPass");
    o.destroyFramebuffer = (PFN_vkDestroyFramebuffer)o.gipa(o.inst, "vkDestroyFramebuffer");
    o.destroyImageView = (PFN_vkDestroyImageView)o.gipa(o.inst, "vkDestroyImageView");
    o.destroyImage = (PFN_vkDestroyImage)o.gipa(o.inst, "vkDestroyImage");
    o.destroyBuffer = (PFN_vkDestroyBuffer)o.gipa(o.inst, "vkDestroyBuffer");
    o.destroyShader = (PFN_vkDestroyShaderModule)o.gipa(o.inst, "vkDestroyShaderModule");
    o.destroyPipeline = (PFN_vkDestroyPipeline)o.gipa(o.inst, "vkDestroyPipeline");
    o.destroyPipelineLayout = (PFN_vkDestroyPipelineLayout)o.gipa(o.inst, "vkDestroyPipelineLayout");
    o.destroyDSL = (PFN_vkDestroyDescriptorSetLayout)o.gipa(o.inst, "vkDestroyDescriptorSetLayout");
    o.destroyDP = (PFN_vkDestroyDescriptorPool)o.gipa(o.inst, "vkDestroyDescriptorPool");
    o.destroySampler = (PFN_vkDestroySampler)o.gipa(o.inst, "vkDestroySampler");
    o.destroyCmdPool = (PFN_vkDestroyCommandPool)o.gipa(o.inst, "vkDestroyCommandPool");
    o.freeMemory = (PFN_vkFreeMemory)o.gipa(o.inst, "vkFreeMemory");

    g_createImage = (PFN_vkCreateImage)o.gipa(o.inst, "vkCreateImage");
    g_getImgReqs = (PFN_vkGetImageMemoryRequirements)o.gipa(o.inst, "vkGetImageMemoryRequirements");
    g_bindImgMem = (PFN_vkBindImageMemory)o.gipa(o.inst, "vkBindImageMemory");
    g_createView = (PFN_vkCreateImageView)o.gipa(o.inst, "vkCreateImageView");
    g_createBuf = (PFN_vkCreateBuffer)o.gipa(o.inst, "vkCreateBuffer");
    g_getBufReqs = (PFN_vkGetBufferMemoryRequirements)o.gipa(o.inst, "vkGetBufferMemoryRequirements");
    g_allocMem = (PFN_vkAllocateMemory)o.gipa(o.inst, "vkAllocateMemory");
    g_bindBufMem = (PFN_vkBindBufferMemory)o.gipa(o.inst, "vkBindBufferMemory");
    g_mapMem = (PFN_vkMapMemory)o.gipa(o.inst, "vkMapMemory");
    g_unmapMem = (PFN_vkUnmapMemory)o.gipa(o.inst, "vkUnmapMemory");
    g_createSampler = (PFN_vkCreateSampler)o.gipa(o.inst, "vkCreateSampler");
    g_createDSL = (PFN_vkCreateDescriptorSetLayout)o.gipa(o.inst, "vkCreateDescriptorSetLayout");
    g_createDP = (PFN_vkCreateDescriptorPool)o.gipa(o.inst, "vkCreateDescriptorPool");
    g_allocDS = (PFN_vkAllocateDescriptorSets)o.gipa(o.inst, "vkAllocateDescriptorSets");
    g_updateDS = (PFN_vkUpdateDescriptorSets)o.gipa(o.inst, "vkUpdateDescriptorSets");
    g_cmdBarrier = (PFN_vkCmdPipelineBarrier)o.gipa(o.inst, "vkCmdPipelineBarrier");
    g_cmdCopyImg = (PFN_vkCmdCopyBufferToImage)o.gipa(o.inst, "vkCmdCopyBufferToImage");
    g_bindPipe = (PFN_vkCmdBindPipeline)o.gipa(o.inst, "vkCmdBindPipeline");
    g_bindDS = (PFN_vkCmdBindDescriptorSets)o.gipa(o.inst, "vkCmdBindDescriptorSets");
    g_bindVB = (PFN_vkCmdBindVertexBuffers)o.gipa(o.inst, "vkCmdBindVertexBuffers");
    g_cmdDraw = (PFN_vkCmdDraw)o.gipa(o.inst, "vkCmdDraw");
    g_beginRP = (PFN_vkCmdBeginRenderPass)o.gipa(o.inst, "vkCmdBeginRenderPass");
    g_endRP = (PFN_vkCmdEndRenderPass)o.gipa(o.inst, "vkCmdEndRenderPass");
    g_beginCB = (PFN_vkBeginCommandBuffer)o.gipa(o.inst, "vkBeginCommandBuffer");
    g_endCB = (PFN_vkEndCommandBuffer)o.gipa(o.inst, "vkEndCommandBuffer");
    g_resetCB = (PFN_vkResetCommandBuffer)o.gipa(o.inst, "vkResetCommandBuffer");
    g_submit = (PFN_vkQueueSubmit)o.gipa(o.inst, "vkQueueSubmit");
    g_waitIdle = (PFN_vkQueueWaitIdle)o.gipa(o.inst, "vkQueueWaitIdle");
    g_acquire = (PFN_vkAcquireNextImageKHR)o.gipa(o.inst, "vkAcquireNextImageKHR");
    g_present = (PFN_vkQueuePresentKHR)o.gipa(o.inst, "vkQueuePresentKHR");
    g_createShader = (PFN_vkCreateShaderModule)o.gipa(o.inst, "vkCreateShaderModule");
    g_createPL = (PFN_vkCreatePipelineLayout)o.gipa(o.inst, "vkCreatePipelineLayout");
    g_createPipe = (PFN_vkCreateGraphicsPipelines)o.gipa(o.inst, "vkCreateGraphicsPipelines");

    if (!enumDev || !getProps || !getMemProps || !getQFP || !getSurfSupport || !createSurface ||
        !createDevice || !createSwap || !getImages || !createRP || !createFB || !createPool || !allocCB) {
        o.lastError = "missing vulkan functions";
        return false;
    }

    VkPhysicalDeviceMemoryProperties memProps{};
    uint32_t n = 0;
    enumDev(o.inst, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    enumDev(o.inst, &n, devs.data());
    if (n == 0) { o.lastError = "no devices"; return false; }

    for (auto& d : devs) {
        VkPhysicalDeviceProperties pp{};
        getProps(d, &pp);
        if (pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) o.phys = d;
    }
    if (!o.phys) o.phys = devs[0];
    getMemProps(o.phys, &memProps);
    {
        VkPhysicalDeviceProperties sp{};
        getProps(o.phys, &sp);
        fprintf(stderr, "[vk] device=%s type=%d api=%u.%u.%u\n", sp.deviceName, (int)sp.deviceType,
                VK_VERSION_MAJOR(sp.apiVersion), VK_VERSION_MINOR(sp.apiVersion), VK_VERSION_PATCH(sp.apiVersion));
    }

    VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    sci.hinstance = GetModuleHandle(nullptr);
    sci.hwnd = hwnd;
    if (createSurface(o.inst, &sci, nullptr, &o.surf) != VK_SUCCESS) { o.lastError = "surface failed"; return false; }

    uint32_t qn = 0;
    getQFP(o.phys, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qs(qn);
    getQFP(o.phys, &qn, qs.data());
    int qi = -1;
    for (uint32_t i = 0; i < qn; i++) {
        VkBool32 ok = VK_FALSE;
        getSurfSupport(o.phys, i, o.surf, &ok);
        if ((qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && ok) { qi = (int)i; break; }
    }
    if (qi < 0) { o.lastError = "no graphics+present queue"; return false; }
    o.qidx = (uint32_t)qi;

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = o.qidx;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char* dexts[] = {"VK_KHR_swapchain"};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = dexts;
    if (createDevice(o.phys, &dci, nullptr, &o.dev) != VK_SUCCESS) { o.lastError = "device failed"; return false; }

    auto getQueue = (PFN_vkGetDeviceQueue)o.gipa(o.inst, "vkGetDeviceQueue");
    getQueue(o.dev, o.qidx, 0, &o.queue);

    VkSurfaceCapabilitiesKHR caps{};
    getSurfCaps(o.phys, o.surf, &caps);
    uint32_t fc = 0;
    getSurfFormats(o.phys, o.surf, &fc, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(fc);
    getSurfFormats(o.phys, o.surf, &fc, fmts.data());
    VkSurfaceFormatKHR fmt = fmts[0];
    for (auto& f : fmts) if (f.format == VK_FORMAT_B8G8R8A8_UNORM) { fmt = f; break; }
    o.colorFormat = fmt.format;

    o.extent = caps.currentExtent;
    if (o.extent.width == 0xFFFFFFFF) { o.extent.width = (uint32_t)width; o.extent.height = (uint32_t)height; }

    VkSwapchainCreateInfoKHR swci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    swci.surface = o.surf;
    swci.minImageCount = caps.minImageCount + 1;
    swci.imageFormat = fmt.format;
    swci.imageColorSpace = fmt.colorSpace;
    swci.imageExtent = o.extent;
    swci.imageArrayLayers = 1;
    swci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    swci.preTransform = caps.currentTransform;
    swci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swci.clipped = VK_TRUE;
    if (createSwap(o.dev, &swci, nullptr, &o.swapchain) != VK_SUCCESS) { o.lastError = "swapchain failed"; return false; }

    uint32_t ic = 0;
    getImages(o.dev, o.swapchain, &ic, nullptr);
    o.swapImages.resize(ic);
    getImages(o.dev, o.swapchain, &ic, o.swapImages.data());
    o.swapViews.resize(ic);
    for (uint32_t i = 0; i < ic; i++) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = o.swapImages[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = fmt.format;
        vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.levelCount = 1;
        vci.subresourceRange.layerCount = 1;
        g_createView(o.dev, &vci, nullptr, &o.swapViews[i]);
    }

    VkAttachmentDescription att{};
    att.format = fmt.format;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref{};
    ref.attachment = 0;
    ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpci.attachmentCount = 1;
    rpci.pAttachments = &att;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sub;
    createRP(o.dev, &rpci, nullptr, &o.renderPass);

    o.framebuffers.resize(ic);
    for (uint32_t i = 0; i < ic; i++) {
        VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fci.renderPass = o.renderPass;
        fci.attachmentCount = 1;
        fci.pAttachments = &o.swapViews[i];
        fci.width = o.extent.width;
        fci.height = o.extent.height;
        fci.layers = 1;
        createFB(o.dev, &fci, nullptr, &o.framebuffers[i]);
    }

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.queueFamilyIndex = o.qidx;
    createPool(o.dev, &pci, nullptr, &o.cmdPool);
    VkCommandBufferAllocateInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbi.commandPool = o.cmdPool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    allocCB(o.dev, &cbi, &o.cmd);

    // descriptor layout 按实际 SPIR-V：binding0 = UBO，binding1 = 贴图采样器
    VkDescriptorSetLayoutBinding dslb[2]{};
    dslb[0].binding = 0;
    dslb[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    dslb[0].descriptorCount = 1;
    dslb[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    dslb[1].binding = 1;
    dslb[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    dslb[1].descriptorCount = 1;
    dslb[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dslci.bindingCount = 2;
    dslci.pBindings = dslb;
    g_createDSL(o.dev, &dslci, nullptr, &o.dsl);

    VkDescriptorPoolSize dps[2]{};
    dps[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    dps[0].descriptorCount = MAX_TEXTURES * 2;
    dps[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    dps[1].descriptorCount = MAX_TEXTURES * 2;
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = MAX_TEXTURES * 2;
    dpci.poolSizeCount = 2;
    dpci.pPoolSizes = dps;
    g_createDP(o.dev, &dpci, nullptr, &o.descPool);

    VkSamplerCreateInfo smpci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    smpci.magFilter = VK_FILTER_NEAREST;
    smpci.minFilter = VK_FILTER_NEAREST;
    smpci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    smpci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    smpci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    smpci.maxLod = 1.0f;
    g_createSampler(o.dev, &smpci, nullptr, &o.sampler);

    // ubo
    VkBufferCreateInfo ubci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ubci.size = 256;
    ubci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    g_createBuf(o.dev, &ubci, nullptr, &o.ubo);
    VkMemoryRequirements umr{};
    g_getBufReqs(o.dev, o.ubo, &umr);
    VkMemoryAllocateInfo umai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    umai.allocationSize = umr.size;
    umai.memoryTypeIndex = FindMemType(memProps, umr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    g_allocMem(o.dev, &umai, nullptr, &o.uboMem);
    g_bindBufMem(o.dev, o.ubo, o.uboMem, 0);
    void* up = nullptr;
    g_mapMem(o.dev, o.uboMem, 0, umr.size, 0, &up);
    o.uboPtr = (float*)up;
    memset(o.uboPtr, 0, 256);

    // shaders
    std::string base = ExeDirLocal();
    auto vsCode = ReadFileLocal(base + "shader.vert.spv");
    auto fsCode = ReadFileLocal(base + "crt_test.spv");
    if (fsCode.empty()) fsCode = ReadFileLocal(base + "shader.frag.spv");
    // 外部 .spv 不存在时用 ndll 内嵌的同一份字节码，避免“缺文件就起不来”
    if (vsCode.empty()) vsCode.assign((const char*)kVkVertSpv, (const char*)kVkVertSpv + sizeof(kVkVertSpv));
    if (fsCode.empty()) fsCode.assign((const char*)kVkFragSpv, (const char*)kVkFragSpv + sizeof(kVkFragSpv));
    if (vsCode.empty() || fsCode.empty()) { o.lastError = "spv missing"; return false; }
    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = vsCode.size();
    smci.pCode = (const uint32_t*)vsCode.data();
    g_createShader(o.dev, &smci, nullptr, &o.vsMod);
    smci.codeSize = fsCode.size();
    smci.pCode = (const uint32_t*)fsCode.data();
    g_createShader(o.dev, &smci, nullptr, &o.fsMod);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = o.vsMod;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = o.fsMod;
    stages[1].pName = "main";

    // 顶点布局按内嵌着色器实际 loc：0=Position(vec4) 1=Alpha(float)
    // 2=ColorMultiplier(vec4) 3=ColorOffset(vec4) 4=TextureCoord(vec2)，共 15 floats
    VkVertexInputBindingDescription vbind{};
    vbind.binding = 0;
    vbind.stride = sizeof(float) * 15;
    vbind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription vattr[5]{};
    vattr[0].location = 0;
    vattr[0].binding = 0;
    vattr[0].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    vattr[0].offset = 0;
    vattr[1].location = 1;
    vattr[1].binding = 0;
    vattr[1].format = VK_FORMAT_R32_SFLOAT;
    vattr[1].offset = sizeof(float) * 4;
    vattr[2].location = 2;
    vattr[2].binding = 0;
    vattr[2].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    vattr[2].offset = sizeof(float) * 5;
    vattr[3].location = 3;
    vattr[3].binding = 0;
    vattr[3].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    vattr[3].offset = sizeof(float) * 9;
    vattr[4].location = 4;
    vattr[4].binding = 0;
    vattr[4].format = VK_FORMAT_R32G32_SFLOAT;
    vattr[4].offset = sizeof(float) * 13;
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &vbind;
    vi.vertexAttributeDescriptionCount = 5;
    vi.pVertexAttributeDescriptions = vattr;

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport vp{0, 0, (float)o.extent.width, (float)o.extent.height, 0, 1};
    VkRect2D sc{{0, 0}, o.extent};
    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = 1;
    vps.pViewports = &vp;
    vps.scissorCount = 1;
    vps.pScissors = &sc;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = 0xF;
    cba.blendEnable = VK_TRUE;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.colorBlendOp = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    cba.alphaBlendOp = VK_BLEND_OP_ADD;
    VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cbs.attachmentCount = 1;
    cbs.pAttachments = &cba;

    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &o.dsl;
    g_createPL(o.dev, &plci, nullptr, &o.pipeLayout);

    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vps;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cbs;
    gp.layout = o.pipeLayout;
    gp.renderPass = o.renderPass;
    gp.subpass = 0;
    if (g_createPipe(o.dev, VK_NULL_HANDLE, 1, &gp, nullptr, &o.pipeline) != VK_SUCCESS) {
        o.lastError = "pipeline failed";
        return false;
    }

    // vertex buffer
    o.vbBytes = (size_t)MAX_SPRITES * 6 * 15 * sizeof(float);
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = o.vbBytes;
    bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    g_createBuf(o.dev, &bci, nullptr, &o.vb);
    VkMemoryRequirements vmr{};
    g_getBufReqs(o.dev, o.vb, &vmr);
    VkMemoryAllocateInfo vmai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    vmai.allocationSize = vmr.size;
    vmai.memoryTypeIndex = FindMemType(memProps, vmr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    g_allocMem(o.dev, &vmai, nullptr, &o.vbMem);
    g_bindBufMem(o.dev, o.vb, o.vbMem, 0);
    void* vp2 = nullptr;
    g_mapMem(o.dev, o.vbMem, 0, vmr.size, 0, &vp2);
    o.vbPtr = (float*)vp2;

    o.textures.resize(MAX_TEXTURES);
    fprintf(stderr, "[vk] init ok swapchain=%ux%u images=%zu format=%d\n",
            o.extent.width, o.extent.height, o.swapImages.size(), (int)o.colorFormat);
    return true;
}

void VkRenderer::Shutdown() {
    Impl& o = *impl;
    if (!o.dev) return;
    if (g_waitIdle) g_waitIdle(o.queue);
    for (auto& t : o.textures) {
        if (t.view) o.destroyImageView(o.dev, t.view, nullptr);
        if (t.image) o.destroyImage(o.dev, t.image, nullptr);
        if (t.mem) o.freeMemory(o.dev, t.mem, nullptr);
    }
    if (o.destroyDP) o.destroyDP(o.dev, o.descPool, nullptr);
    if (o.destroyDSL) o.destroyDSL(o.dev, o.dsl, nullptr);
    if (o.destroySampler) o.destroySampler(o.dev, o.sampler, nullptr);
    if (o.destroyPipeline) o.destroyPipeline(o.dev, o.pipeline, nullptr);
    if (o.destroyPipelineLayout) o.destroyPipelineLayout(o.dev, o.pipeLayout, nullptr);
    if (o.destroyShader) {
        if (o.vsMod) o.destroyShader(o.dev, o.vsMod, nullptr);
        if (o.fsMod) o.destroyShader(o.dev, o.fsMod, nullptr);
    }
    if (o.destroyFramebuffer) for (auto fb : o.framebuffers) o.destroyFramebuffer(o.dev, fb, nullptr);
    if (o.destroyRenderPass) o.destroyRenderPass(o.dev, o.renderPass, nullptr);
    if (o.destroyImageView) for (auto v : o.swapViews) o.destroyImageView(o.dev, v, nullptr);
    if (o.destroySwapchain) o.destroySwapchain(o.dev, o.swapchain, nullptr);
    if (o.destroyBuffer) {
        if (o.vb) o.destroyBuffer(o.dev, o.vb, nullptr);
        if (o.ubo) o.destroyBuffer(o.dev, o.ubo, nullptr);
    }
    if (o.freeMemory) {
        if (o.vbMem) o.freeMemory(o.dev, o.vbMem, nullptr);
        if (o.uboMem) o.freeMemory(o.dev, o.uboMem, nullptr);
    }
    if (o.destroyCmdPool) o.destroyCmdPool(o.dev, o.cmdPool, nullptr);
    if (o.destroyDevice) o.destroyDevice(o.dev, nullptr);
    if (o.destroyInstance) o.destroyInstance(o.inst, nullptr);
    o.dev = VK_NULL_HANDLE;
}

// 2x2 box 降采样一档（RGBA8）
static void DownscaleHalf(const unsigned char* src, int sw, int sh, std::vector<unsigned char>& dst) {
    int dw = sw / 2, dh = sh / 2;
    dst.assign((size_t)dw * dh * 4, 0);
    for (int y = 0; y < dh; y++) {
        for (int x = 0; x < dw; x++) {
            size_t di = ((size_t)y * dw + x) * 4;
            size_t si0 = ((size_t)(y * 2) * sw + (size_t)(x * 2)) * 4;
            size_t si1 = si0 + (size_t)sw * 4;
            for (int c = 0; c < 4; c++) {
                int sum = (int)src[si0 + c] + (int)src[si0 + 4 + c] +
                          (int)src[si1 + c] + (int)src[si1 + 4 + c];
                dst[di + c] = (unsigned char)(sum >> 2);
            }
        }
    }
}

uint32_t VkRenderer::CreateTexture(int w, int h, const unsigned char* rgba) {
    // 大图集降级：高度超过 4096 就逐级减半，避免贴着 maxImageDimension2D 分配直接失败。
    // UV 仍是 0..1，采样位置不变，只是清晰度下降。
    if (!rgba || w <= 0 || h <= 0 || h <= 4096) return CreateTextureOnce(w, h, rgba);

    std::vector<unsigned char> bufA, bufB;
    const unsigned char* src = rgba;
    std::vector<unsigned char>* last = nullptr;
    int sw = w, sh = h;
    while (sh > 4096 && sw > 1) {
        std::vector<unsigned char>& dst = (last == &bufA) ? bufB : bufA;
        DownscaleHalf(src, sw, sh, dst);
        src = dst.data();
        sw /= 2;
        sh /= 2;
        last = &dst;
    }
    return CreateTextureOnce(sw, sh, src);
}

uint32_t VkRenderer::CreateTextureOnce(int w, int h, const unsigned char* rgba) {
    Impl& o = *impl;
    for (uint32_t i = 0; i < o.textures.size(); i++) {
        if (o.textures[i].used) continue;
        TexSlot& t = o.textures[i];
        size_t bytes = (size_t)w * h * 4;

        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent = {(uint32_t)w, (uint32_t)h, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (g_createImage(o.dev, &ici, nullptr, &t.image) != VK_SUCCESS) return 0;

        VkMemoryRequirements imr{};
        g_getImgReqs(o.dev, t.image, &imr);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = imr.size;
        uint32_t localIdx = 7;
        VkPhysicalDeviceMemoryProperties mp{};
        ((PFN_vkGetPhysicalDeviceMemoryProperties)g_gipa(o.inst, "vkGetPhysicalDeviceMemoryProperties"))(o.phys, &mp);
        localIdx = FindMemType(mp, imr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        mai.memoryTypeIndex = localIdx;
        if (g_allocMem(o.dev, &mai, nullptr, &t.mem) != VK_SUCCESS) return 0;
        g_bindImgMem(o.dev, t.image, t.mem, 0);
        t.bytes = imr.size;

        VkBufferCreateInfo sbci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        sbci.size = bytes;
        sbci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VkBuffer stage = VK_NULL_HANDLE;
        g_createBuf(o.dev, &sbci, nullptr, &stage);
        VkMemoryRequirements smr{};
        g_getBufReqs(o.dev, stage, &smr);
        mai.allocationSize = smr.size;
        mai.memoryTypeIndex = FindMemType(mp, smr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkDeviceMemory stageMem = VK_NULL_HANDLE;
        g_allocMem(o.dev, &mai, nullptr, &stageMem);
        g_bindBufMem(o.dev, stage, stageMem, 0);
        void* sptr = nullptr;
        g_mapMem(o.dev, stageMem, 0, smr.size, 0, &sptr);
        memcpy(sptr, rgba, bytes);
        g_unmapMem(o.dev, stageMem);

        VkCommandBuffer upCB = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo ubi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ubi.commandPool = o.cmdPool;
        ubi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ubi.commandBufferCount = 1;
        ((PFN_vkAllocateCommandBuffers)g_gipa(o.inst, "vkAllocateCommandBuffers"))(o.dev, &ubi, &upCB);
        VkCommandBufferBeginInfo ubegin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        ubegin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        g_beginCB(upCB, &ubegin);

        VkImageMemoryBarrier b1{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b1.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b1.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b1.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b1.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b1.image = t.image;
        b1.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b1.subresourceRange.levelCount = 1;
        b1.subresourceRange.layerCount = 1;
        b1.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        g_cmdBarrier(upCB, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b1);

        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
        g_cmdCopyImg(upCB, stage, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        VkImageMemoryBarrier b2{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b2.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b2.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b2.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b2.image = t.image;
        b2.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b2.subresourceRange.levelCount = 1;
        b2.subresourceRange.layerCount = 1;
        b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        g_cmdBarrier(upCB, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b2);

        g_endCB(upCB);
        VkSubmitInfo us{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        us.commandBufferCount = 1;
        us.pCommandBuffers = &upCB;
        g_submit(o.queue, 1, &us, VK_NULL_HANDLE);
        g_waitIdle(o.queue);
        ((PFN_vkFreeCommandBuffers)g_gipa(o.inst, "vkFreeCommandBuffers"))(o.dev, o.cmdPool, 1, &upCB);
        o.destroyBuffer(o.dev, stage, nullptr);
        o.freeMemory(o.dev, stageMem, nullptr);

        VkImageViewCreateInfo tvi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        tvi.image = t.image;
        tvi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        tvi.format = VK_FORMAT_R8G8B8A8_UNORM;
        tvi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        tvi.subresourceRange.levelCount = 1;
        tvi.subresourceRange.layerCount = 1;
        g_createView(o.dev, &tvi, nullptr, &t.view);

        std::vector<VkDescriptorSetLayout> layouts(1, o.dsl);
        VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dsai.descriptorPool = o.descPool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = layouts.data();
        g_allocDS(o.dev, &dsai, &t.set);

        VkDescriptorImageInfo dii{};
        dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        dii.imageView = t.view;
        dii.sampler = t.sampler ? t.sampler : o.sampler;
        VkDescriptorBufferInfo dbi{};
        dbi.buffer = o.ubo;
        dbi.offset = 0;
        dbi.range = 256;
        VkWriteDescriptorSet wds[2]{};
        wds[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wds[0].dstSet = t.set;
        wds[0].dstBinding = 0;
        wds[0].descriptorCount = 1;
        wds[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        wds[0].pBufferInfo = &dbi;
        wds[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wds[1].dstSet = t.set;
        wds[1].dstBinding = 1;
        wds[1].descriptorCount = 1;
        wds[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wds[1].pImageInfo = &dii;
        g_updateDS(o.dev, 2, wds, 0, nullptr);

        t.w = w;
        t.h = h;
        t.used = true;
        t.verts.clear();
        return i + 1;
    }
    return 0;
}

void VkRenderer::DestroyTexture(uint32_t id) {
    Impl& o = *impl;
    if (id == 0 || id > o.textures.size()) return;
    TexSlot& t = o.textures[id - 1];
    if (!t.used) return;
    if (t.view) o.destroyImageView(o.dev, t.view, nullptr);
    if (t.sampler) o.destroySampler(o.dev, t.sampler, nullptr);
    if (t.image) o.destroyImage(o.dev, t.image, nullptr);
    if (t.mem) o.freeMemory(o.dev, t.mem, nullptr);
    t = TexSlot();
}

uint32_t VkRenderer::TextureCount() const {
    uint32_t c = 0;
    for (auto& t : impl->textures) if (t.used) c++;
    return c;
}

// ---- GL 常量 -> Vulkan 枚举（只认最常用的几种，其余按最接近语义映射）----
static VkFilter GlFilterToVk(int glFilter) {
    return (glFilter == 0x2600) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR; // NEAREST / LINEAR
}

static VkSamplerAddressMode GlWrapToVk(int wrap) {
    switch (wrap) {
        case 0x2901: return VK_SAMPLER_ADDRESS_MODE_REPEAT;           // GL_REPEAT
        case 0x8370: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;  // GL_MIRRORED_REPEAT
        default:     return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;    // CLAMP_TO_EDGE / 其余
    }
}

bool VkRenderer::SetTextureSampler(uint32_t texId, int minFilter, int magFilter,
                                   int wrapS, int wrapT, int baseLevel, int maxLevel,
                                   float minLod, float maxLod) {
    Impl& o = *impl;
    if (texId == 0 || texId > o.textures.size()) return false;
    TexSlot& t = o.textures[texId - 1];
    if (!t.used) return false;
    (void)baseLevel;
    (void)maxLevel;
    (void)minLod;

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = GlFilterToVk(magFilter);
    sci.minFilter = GlFilterToVk(minFilter);
    // 目前纹理只有 1 级 mip，mip 模式保持 NEAREST 并把 maxLod 钳到 0，
    // 否则采样器会去取不存在的 level 导致黑边/全黑
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = GlWrapToVk(wrapS);
    sci.addressModeV = GlWrapToVk(wrapT);
    sci.addressModeW = sci.addressModeV;
    sci.minLod = 0;
    sci.maxLod = 0;
    sci.maxAnisotropy = 1;
    if (g_createSampler(o.dev, &sci, nullptr, &t.sampler) != VK_SUCCESS) {
        t.sampler = VK_NULL_HANDLE;
        return false;
    }

    VkDescriptorImageInfo dii{};
    dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    dii.imageView = t.view;
    dii.sampler = t.sampler;
    VkWriteDescriptorSet wds{};
    wds.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wds.dstSet = t.set;
    wds.dstBinding = 1;
    wds.descriptorCount = 1;
    wds.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wds.pImageInfo = &dii;
    g_updateDS(o.dev, 1, &wds, 0, nullptr);
    return true;
}

bool VkRenderer::UpdateSubImage(uint32_t texId, int level, int x, int y, int w, int h,
                                const unsigned char* rgba) {
    Impl& o = *impl;
    if (level != 0 || !rgba || w <= 0 || h <= 0) return false;
    if (texId == 0 || texId > o.textures.size()) return false;
    TexSlot& t = o.textures[texId - 1];
    if (!t.used) return false;
    if (x < 0 || y < 0 || x + w > t.w || y + h > t.h) return false;

    size_t bytes = (size_t)w * h * 4;
    VkBufferCreateInfo sbci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    sbci.size = bytes;
    sbci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkBuffer stage = VK_NULL_HANDLE;
    if (g_createBuf(o.dev, &sbci, nullptr, &stage) != VK_SUCCESS) return false;

    VkMemoryRequirements smr{};
    g_getBufReqs(o.dev, stage, &smr);
    VkPhysicalDeviceMemoryProperties mp{};
    ((PFN_vkGetPhysicalDeviceMemoryProperties)g_gipa(o.inst, "vkGetPhysicalDeviceMemoryProperties"))(o.phys, &mp);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = smr.size;
    mai.memoryTypeIndex = FindMemType(mp, smr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory stageMem = VK_NULL_HANDLE;
    if (g_allocMem(o.dev, &mai, nullptr, &stageMem) != VK_SUCCESS) {
        o.destroyBuffer(o.dev, stage, nullptr);
        return false;
    }
    g_bindBufMem(o.dev, stage, stageMem, 0);
    void* sptr = nullptr;
    g_mapMem(o.dev, stageMem, 0, smr.size, 0, &sptr);
    memcpy(sptr, rgba, bytes);
    g_unmapMem(o.dev, stageMem);

    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbi.commandPool = o.cmdPool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    if (((PFN_vkAllocateCommandBuffers)g_gipa(o.inst, "vkAllocateCommandBuffers"))(o.dev, &cbi, &cb) != VK_SUCCESS) {
        o.destroyBuffer(o.dev, stage, nullptr);
        o.freeMemory(o.dev, stageMem, nullptr);
        return false;
    }
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    g_beginCB(cb, &begin);

    VkImageMemoryBarrier b1{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b1.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b1.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b1.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b1.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b1.image = t.image;
    b1.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b1.subresourceRange.levelCount = 1;
    b1.subresourceRange.layerCount = 1;
    b1.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b1.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    g_cmdBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                 0, 0, nullptr, 0, nullptr, 1, &b1);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {x, y, 0};
    region.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
    g_cmdCopyImg(cb, stage, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier b2{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b2.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b2.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b2.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b2.image = t.image;
    b2.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b2.subresourceRange.levelCount = 1;
    b2.subresourceRange.layerCount = 1;
    b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    g_cmdBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                 0, 0, nullptr, 0, nullptr, 1, &b2);

    g_endCB(cb);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    g_submit(o.queue, 1, &submit, VK_NULL_HANDLE);
    g_waitIdle(o.queue);
    ((PFN_vkFreeCommandBuffers)g_gipa(o.inst, "vkFreeCommandBuffers"))(o.dev, o.cmdPool, 1, &cb);
    o.destroyBuffer(o.dev, stage, nullptr);
    o.freeMemory(o.dev, stageMem, nullptr);
    return true;
}

uint32_t VkRenderer::CreateEmptyTexture(int w, int h) {
    // 先用全透明像素占位，后续 texSubImage2D 再填内容
    std::vector<unsigned char> zeros((size_t)w * h * 4, 0);
    return CreateTexture(w, h, zeros.data());
}

bool VkRenderer::GenerateMipmaps(uint32_t texId, int maxLevel) {
    (void)maxLevel;
    // 纹理只有 1 级 mip；这里返回 true 表示“请求已被接受”，但不会生成额外 level
    return texId != 0 && texId <= impl->textures.size() && impl->textures[texId - 1].used;
}

uint32_t VkRenderer::CreateRenderTargetEx(int w, int h, bool withDepth) {
    (void)withDepth; // 目前还没有 depth 附件，深度依赖由调用方降级处理
    return CreateRenderTarget(w, h);
}

bool VkRenderer::ReadPixels(uint32_t targetId, int x, int y, int w, int h, unsigned char* rgba) {
    // 离屏回读还没接：需要把目标 image 转 TRANSFER_SRC 再拷到 staging。
    // 先明确返回 false，避免上层把未初始化的像素当成真实结果。
    (void)targetId; (void)x; (void)y; (void)w; (void)h; (void)rgba;
    return false;
}

void VkRenderer::SetUniform(int index, float value) {
    Impl& o = *impl;
    if (o.uboPtr && index >= 0 && index < 64) o.uboPtr[index] = value;
}

void VkRenderer::DrawSprite(uint32_t texId, float x, float y, float w, float h,
                            float u0, float v0, float u1, float v1) {
    Impl& o = *impl;
    std::vector<float>* vertsPtr = nullptr;
    if (texId >= 1000) {
        uint32_t ti = texId - 1000;
        if (ti >= o.targets.size() || !o.targets[ti].used) return;
        if (o.currentTarget == texId) return; // no self sampling
        vertsPtr = &o.targets[ti].verts;
    } else {
        if (texId == 0 || texId > o.textures.size()) return;
        TexSlot& t = o.textures[texId - 1];
        if (!t.used) return;
        vertsPtr = &t.verts;
    }
    float hw = w * 0.5f, hh = h * 0.5f;
    float qx[6] = {x - hw, x + hw, x + hw, x - hw, x + hw, x - hw};
    float qy[6] = {y - hh, y - hh, y + hh, y - hh, y + hh, y + hh};
    float qu[6] = {u0, u1, u1, u0, u1, u0};
    float qv[6] = {v0, v0, v1, v0, v1, v1};
    for (int i = 0; i < 6; i++) {
        PushLimeVertex(*vertsPtr, qx[i], qy[i], qu[i], qv[i]);
    }
}

void VkRenderer::DrawRaw(uint32_t texId, const float* verts, int vertexCount) {
    Impl& o = *impl;
    std::vector<float>* vertsPtr = nullptr;
    if (texId >= 1000) {
        uint32_t ti = texId - 1000;
        if (ti >= o.targets.size() || !o.targets[ti].used) return;
        if (o.currentTarget == texId) return;
        vertsPtr = &o.targets[ti].verts;
    } else {
        if (texId == 0 || texId > o.textures.size()) return;
        TexSlot& t = o.textures[texId - 1];
        if (!t.used) return;
        vertsPtr = &t.verts;
    }
    if (vertexCount <= 0) return;
    // NDC 必须按“当前渲染目标”的尺寸换算；用交换链尺寸会让离屏 pass 整体错位
    float w = (float)o.extent.width;
    float h = (float)o.extent.height;
    if (o.currentTarget >= 1000) {
        uint32_t ti = o.currentTarget - 1000;
        if (ti >= o.targets.size() || !o.targets[ti].used) return;
        w = (float)o.targets[ti].w;
        h = (float)o.targets[ti].h;
    }
    if (w <= 0 || h <= 0) return;

    // 采样源尺寸：uv 归一化要用（有的路径传进来的 uv 是像素值而不是 0..1）
    float tw = 0.0f, th = 0.0f;
    if (texId >= 1000) {
        uint32_t ti = texId - 1000;
        tw = (float)o.targets[ti].w;
        th = (float)o.targets[ti].h;
    } else {
        tw = (float)o.textures[texId - 1].w;
        th = (float)o.textures[texId - 1].h;
    }

    vertsPtr->reserve(vertsPtr->size() + (size_t)vertexCount * 15);
    for (int i = 0; i < vertexCount; i++) {
        float px = verts[i * 4 + 0];
        float py = verts[i * 4 + 1];
        float u = verts[i * 4 + 2];
        float v = verts[i * 4 + 3];
        // 像素坐标直接进顶点：像素→NDC 的换算由 UBO 里的投影矩阵完成
        // uv 超出 0..1 说明传进来的是像素坐标，按采样源尺寸归一化，否则采样越界全黑
        if (u > 1.001f || u < -1.001f) u = tw > 0 ? u / tw : 1.0f;
        if (v > 1.001f || v < -1.001f) v = th > 0 ? v / th : 1.0f;
        PushLimeVertex(*vertsPtr, px, py, u, v);
    }
}

void VkRenderer::BeginFrame(float r, float g, float b, float a) {
    BindRenderTarget(0, r, g, b, a);
}

int VkRenderer::ScreenWidth() const { return impl->extent.width; }
int VkRenderer::ScreenHeight() const { return impl->extent.height; }

bool VkRenderer::GetRenderTargetSize(uint32_t id, int* w, int* h) const {
    if (id < 1000) return false;
    uint32_t ti = id - 1000;
    if (ti >= impl->targets.size() || !impl->targets[ti].used) return false;
    if (w) *w = impl->targets[ti].w;
    if (h) *h = impl->targets[ti].h;
    return true;
}

bool VkRenderer::FrameHasGeometry() const {
    if (!impl->frameOpen) return false;
    for (auto& t : impl->textures) if (!t.verts.empty()) return true;
    for (auto& t : impl->targets) if (!t.verts.empty()) return true;
    return false;
}

bool VkRenderer::BindRenderTarget(uint32_t id, float r, float g, float b, float a) {
    Impl& o = *impl;
    // 同一目标重复绑定（OpenFL 每个 pass 开头都会 glClear，gl_vk_handle_clear 又会 Bind 一次）
    // 不能清空已经累积的顶点，否则整条 filter 链最后合成时 verts=0，屏幕只剩清屏色。
    // 这里只更新清屏色，等 EndFrame 时真正清屏。
    if (o.frameOpen && o.currentTarget == id) {
        o.clearR = r;
        o.clearG = g;
        o.clearB = b;
        o.clearA = a;
        return true;
    }
    o.currentTarget = id;
    o.clearR = r;
    o.clearG = g;
    o.clearB = b;
    o.clearA = a;
    // 注意：切换目标时不能清空顶点列表。OpenFL 的 filter 链最后一个 pass 常常是
    // “先绑后缓冲、再画一张全屏 quad”，一清就把这批顶点吃掉 → 屏幕只剩清屏色。
    // 顶点在 EndFrame 提交后由 EndFrame 自己清。
    if (id == 0) {
        VkResult ar = g_acquire(o.dev, o.swapchain, UINT64_MAX, VK_NULL_HANDLE, VK_NULL_HANDLE, &o.frameImageIndex);
        if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) return false;
    } else {
        uint32_t ti = id - 1000;
        if (ti >= o.targets.size() || !o.targets[ti].used) return false;
    }
    o.frameOpen = true;
    return true;
}

void VkRenderer::PresentTarget() {
    EndFrame();
}

uint32_t VkRenderer::CreateRenderTarget(int w, int h) {
    Impl& o = *impl;
    o.targets.push_back(RenderTarget());
    uint32_t idx = (uint32_t)o.targets.size() - 1;
    RenderTarget& t = o.targets[idx];

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = {(uint32_t)w, (uint32_t)h, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (g_createImage(o.dev, &ici, nullptr, &t.image) != VK_SUCCESS) {
        o.targets.pop_back();
        return 0;
    }

    VkMemoryRequirements imr{};
    g_getImgReqs(o.dev, t.image, &imr);
    VkPhysicalDeviceMemoryProperties mp{};
    ((PFN_vkGetPhysicalDeviceMemoryProperties)g_gipa(o.inst, "vkGetPhysicalDeviceMemoryProperties"))(o.phys, &mp);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = imr.size;
    mai.memoryTypeIndex = FindMemType(mp, imr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (g_allocMem(o.dev, &mai, nullptr, &t.mem) != VK_SUCCESS) {
        o.destroyImage(o.dev, t.image, nullptr);
        o.targets.pop_back();
        return 0;
    }
    g_bindImgMem(o.dev, t.image, t.mem, 0);

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = t.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = VK_FORMAT_R8G8B8A8_UNORM;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    g_createView(o.dev, &vci, nullptr, &t.view);

    VkAttachmentDescription att{};
    att.format = VK_FORMAT_R8G8B8A8_UNORM;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    // 离屏 pass 不能 CLEAR：多段 filter 会复用同一批目标，清屏会把上一 pass 的内容抹掉
    att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    // finalLayout 是 SHADER_READ_ONLY（这一 pass 结束就要被采样），initialLayout 与之一致才能保留内容
    att.initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    att.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference ref{};
    ref.attachment = 0;
    ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpci.attachmentCount = 1;
    rpci.pAttachments = &att;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sub;
    auto createRP2 = (PFN_vkCreateRenderPass)g_gipa(o.inst, "vkCreateRenderPass");
    createRP2(o.dev, &rpci, nullptr, &t.rp);

    VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fci.renderPass = t.rp;
    fci.attachmentCount = 1;
    fci.pAttachments = &t.view;
    fci.width = (uint32_t)w;
    fci.height = (uint32_t)h;
    fci.layers = 1;
    auto createFB2 = (PFN_vkCreateFramebuffer)g_gipa(o.inst, "vkCreateFramebuffer");
    createFB2(o.dev, &fci, nullptr, &t.fb);

    std::vector<VkDescriptorSetLayout> layouts(1, o.dsl);
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = o.descPool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = layouts.data();
    if (g_allocDS(o.dev, &dsai, &t.set) != VK_SUCCESS) {
        o.destroyFramebuffer(o.dev, t.fb, nullptr);
        o.destroyRenderPass(o.dev, t.rp, nullptr);
        o.destroyImageView(o.dev, t.view, nullptr);
        o.destroyImage(o.dev, t.image, nullptr);
        o.freeMemory(o.dev, t.mem, nullptr);
        o.targets.pop_back();
        return 0;
    }

    VkDescriptorImageInfo dii{};
    dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    dii.imageView = t.view;
    dii.sampler = o.sampler;
    VkDescriptorBufferInfo dbi{};
    dbi.buffer = o.ubo;
    dbi.offset = 0;
    dbi.range = 256;
    VkWriteDescriptorSet wds[2]{};
    wds[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wds[0].dstSet = t.set;
    wds[0].dstBinding = 0;
    wds[0].descriptorCount = 1;
    wds[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    wds[0].pBufferInfo = &dbi;
    wds[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wds[1].dstSet = t.set;
    wds[1].dstBinding = 1;
    wds[1].descriptorCount = 1;
    wds[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wds[1].pImageInfo = &dii;
    g_updateDS(o.dev, 2, wds, 0, nullptr);

    t.w = w;
    t.h = h;
    t.used = true;
    return 1000 + idx;
}

void VkRenderer::DestroyRenderTarget(uint32_t id) {
    Impl& o = *impl;
    if (id < 1000) return;
    uint32_t ti = id - 1000;
    if (ti >= o.targets.size()) return;
    RenderTarget& t = o.targets[ti];
    if (!t.used) return;
    if (t.fb) o.destroyFramebuffer(o.dev, t.fb, nullptr);
    if (t.rp) o.destroyRenderPass(o.dev, t.rp, nullptr);
    if (t.view) o.destroyImageView(o.dev, t.view, nullptr);
    if (t.image) o.destroyImage(o.dev, t.image, nullptr);
    if (t.mem) o.freeMemory(o.dev, t.mem, nullptr);
    t = RenderTarget();
}

void VkRenderer::EndFrame() {
    Impl& o = *impl;
    if (!o.frameOpen) return;

    float* dst = o.vbPtr;
    struct DrawRange { VkDescriptorSet set; size_t first; size_t count; };
    std::vector<DrawRange> ranges;
    size_t totalVerts = 0;

    auto pushBatch = [&](std::vector<float>& v, VkDescriptorSet set) {
        if (v.empty()) return;
        size_t vc = v.size() / 15;
        if (totalVerts + vc > (size_t)MAX_SPRITES * 6) return;
        memcpy(dst + totalVerts * 15, v.data(), v.size() * sizeof(float));
        ranges.push_back({set, totalVerts, vc});
        totalVerts += vc;
    };
    for (auto& t : o.textures) if (t.used) pushBatch(t.verts, t.set);
    for (auto& t : o.targets) if (t.used) pushBatch(t.verts, t.set);

    VkFramebuffer fb = o.framebuffers[o.frameImageIndex];
    VkRenderPass rp = o.renderPass;
    VkExtent2D ext = o.extent;
    {
        static uint64_t s_frames = 0;
        s_frames++;
        if (s_frames == 1 || s_frames % 30 == 0 || o.currentTarget == 0) {
            float first[4] = {0, 0, 0, 0};
            if (totalVerts > 0) memcpy(first, o.vbPtr, sizeof(first));
            fprintf(stderr, "[vk] frame=%llu verts=%zu ranges=%zu target=%u first=(%.1f,%.1f,%.2f,%.2f)\n",
                    (unsigned long long)s_frames, totalVerts, ranges.size(), o.currentTarget,
                    first[0], first[1], first[2], first[3]);
        }
    }
    if (o.currentTarget >= 1000) {
        uint32_t ti = o.currentTarget - 1000;
        if (ti < o.targets.size() && o.targets[ti].used) {
            fb = o.targets[ti].fb;
            rp = o.targets[ti].rp;
            ext = {(uint32_t)o.targets[ti].w, (uint32_t)o.targets[ti].h};
        }
    }

    // 每帧必须复位：没复位时第二帧起 vkBeginCommandBuffer 会静默失败（画面只更新第一帧）
    g_resetCB(o.cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VkResult br = g_beginCB(o.cmd, &bi);
    if (br != VK_SUCCESS) {
        fprintf(stderr, "[vk] begin command buffer failed: %d\n", (int)br);
        o.frameOpen = false;
        return;
    }
    VkClearValue cv{};
    cv.color.float32[0] = o.clearR;
    cv.color.float32[1] = o.clearG;
    cv.color.float32[2] = o.clearB;
    cv.color.float32[3] = o.clearA;
    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = rp;
    rbi.framebuffer = fb;
    rbi.renderArea.offset = {0, 0};
    rbi.renderArea.extent = ext;
    rbi.clearValueCount = 1;
    rbi.pClearValues = &cv;

    // 每个 pass 的坐标系不同：UBO 里的投影矩阵按当前渲染目标尺寸重算，
    // 否则离屏 pass 会整体错位（着色器读的就是这块 UBO）
    if (o.uboPtr) {
        memset(o.uboPtr, 0, 64);
        o.uboPtr[0] = 2.0f / (float)ext.width;
        o.uboPtr[5] = 2.0f / (float)ext.height;
        o.uboPtr[10] = 1.0f;
        o.uboPtr[12] = -1.0f;
        o.uboPtr[13] = -1.0f;
        o.uboPtr[15] = 1.0f;
    }

    g_beginRP(o.cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    if (totalVerts > 0) {
        g_bindPipe(o.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, o.pipeline);
        VkDeviceSize off = 0;
        g_bindVB(o.cmd, 0, 1, &o.vb, &off);
        for (auto& r : ranges) {
            g_bindDS(o.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, o.pipeLayout, 0, 1, &r.set, 0, nullptr);
            g_cmdDraw(o.cmd, (uint32_t)r.count, 1, (uint32_t)r.first, 0);
        }
    }
    g_endRP(o.cmd);
    g_endCB(o.cmd);

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &o.cmd;
    g_submit(o.queue, 1, &si, VK_NULL_HANDLE);
    g_waitIdle(o.queue);

    if (o.currentTarget == 0) {
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.swapchainCount = 1;
        pi.pSwapchains = &o.swapchain;
        pi.pImageIndices = &o.frameImageIndex;
        g_present(o.queue, &pi);
    }

    o.frameOpen = false;
    for (auto& t : o.textures) t.verts.clear();
    for (auto& t : o.targets) t.verts.clear();
}
