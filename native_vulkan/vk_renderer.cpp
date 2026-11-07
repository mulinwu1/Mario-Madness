#include "vk_renderer.h"
#include "vk_shaders_spv.h"
#define VK_USE_PLATFORM_WIN32_KHR
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <ctime>

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
    // 取证用：创建帧号 / 最后一次"真的有顶点"的提交帧号（0 = 从未写过）
    uint64_t createdFrame = 0;
    uint64_t lastWriteFrame = 0;
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
    // 常量顶点属性：loc5(alpha 乘数)/loc6(colorMultiplier)/loc7(colorOffset)。
    // 用 stride=0 的绑定给所有顶点同一组值；loc1 里已经携带逐顶点 alpha。
    VkBuffer attrBuf = VK_NULL_HANDLE;
    VkDeviceMemory attrMem = VK_NULL_HANDLE;
    float* attrPtr = nullptr;

    VkBuffer ubo = VK_NULL_HANDLE;
    VkDeviceMemory uboMem = VK_NULL_HANDLE;
    float* uboPtr = nullptr;

    std::vector<TexSlot> textures;
    // 实验 VK_SINGLE_PRESENT=1（默认关）：一视觉帧只 acquire/present 一次；
    // 中途绑定后缓冲只提交几何（LOAD pass 叠加），不 flip。
    VkRenderPass renderPassLoad = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffersLoad;
    // 单次 present 的中间合成必须保持内容：交换链图在两次合成之间停留在
    // COLOR_ATTACHMENT_OPTIMAL（不能用 PRESENT_SRC 作 initialLayout，实测会丢内容）。
    VkRenderPass renderPassLoadKeep = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffersLoadKeep;
    VkRenderPass renderPassKeep = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffersKeep;
    bool bbHeld = false;
    bool bbLoad = false;
    bool presentReq = false;
    // 取证：present 前把交换链图拷到主机可见缓冲，算亮度统计（VK_BBSTAT=1，默认关）
    VkBuffer statBuf = VK_NULL_HANDLE;
    VkDeviceMemory statMem = VK_NULL_HANDLE;
    void* statPtr = nullptr;
    size_t statBytes = 0;
    std::vector<RenderTarget> targets;
    uint32_t currentTarget = 0;
    // 最后一次真正收到顶点并提交的离屏目标（present 选源用）
    uint32_t lastGeomTarget = 0;
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
static PFN_vkCmdCopyImageToBuffer g_cmdCopyImgToBuf;
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

// 提交计数：DrawRaw 的取证日志按它分组，判断“同一帧内”的绘制
static uint64_t g_drawFrame = 0;
// 上一帧后缓冲最后一次采样到的纹理 id，用于区分两次后缓冲合成各自画的是哪一层
static uint32_t g_bbLastTex = 0;

static uint32_t FindMemType(const VkPhysicalDeviceMemoryProperties& mp, uint32_t bits, VkMemoryPropertyFlags flags) {
    for (uint32_t i = 0; i < 32; i++) {
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & flags) == flags) return i;
    }
    return 0;
}

// 内嵌着色器的顶点格式：loc0=Position(vec4) loc1=Alpha loc2=ColorMultiplier(vec4)
// loc3=ColorOffset(vec4) loc4=TexCoord(vec2)，共 15 个 float
// 每个绘制可带一组“常量顶点属性”（GL 的 glVertexAttrib1f/4f），对应 openfl 的
// per-draw alpha / colorMultiplier / colorOffset；默认 (alpha=1, mult=1, off=0)。
static float g_drawAlpha = 1.0f;
static float g_drawMult[4] = {1.0f, 1.0f, 1.0f, 1.0f};
static float g_drawOff[4] = {0.0f, 0.0f, 0.0f, 0.0f};

static void PushLimeVertex(std::vector<float>& out, float x, float y, float u, float v,
                           float alpha, const float* mult, const float* off) {
    out.push_back(x);
    out.push_back(y);
    out.push_back(0.0f);
    out.push_back(1.0f);
    out.push_back(alpha);                                  // alpha
    out.push_back(mult[0]); out.push_back(mult[1]);
    out.push_back(mult[2]); out.push_back(mult[3]);        // colorMultiplier
    out.push_back(off[0]); out.push_back(off[1]);
    out.push_back(off[2]); out.push_back(off[3]);          // colorOffset
    out.push_back(u);
    out.push_back(v);
}

static void PushLimeVertex(std::vector<float>& out, float x, float y, float u, float v) {
    PushLimeVertex(out, x, y, u, v, g_drawAlpha, g_drawMult, g_drawOff);
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

// 取证：把交换链图（B8G8R8A8）写成 24 位 BMP，用于和窗口截图逐像素比对
static bool WriteBmpBGRA(const char* path, const uint8_t* px, int w, int h) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    int row = w * 3;
    int pad = (4 - (row % 4)) % 4;
    size_t imgBytes = (size_t)(row + pad) * (size_t)h;
    uint8_t hdr[54] = {0};
    hdr[0] = 'B'; hdr[1] = 'M';
    uint32_t total = (uint32_t)(54 + imgBytes);
    memcpy(hdr + 2, &total, 4);
    uint32_t off = 54;
    memcpy(hdr + 10, &off, 4);
    uint32_t hsz = 40;
    memcpy(hdr + 14, &hsz, 4);
    int32_t ww = w, hh = h;
    memcpy(hdr + 18, &ww, 4);
    memcpy(hdr + 22, &hh, 4);
    uint16_t planes = 1, bpp = 24;
    memcpy(hdr + 26, &planes, 2);
    memcpy(hdr + 28, &bpp, 2);
    uint32_t img = (uint32_t)imgBytes;
    memcpy(hdr + 34, &img, 4);
    fwrite(hdr, 1, 54, f);
    uint8_t padbuf[3] = {0, 0, 0};
    for (int y = h - 1; y >= 0; y--) {
        const uint8_t* s = px + (size_t)y * (size_t)w * 4;
        for (int x = 0; x < w; x++) {
            uint8_t bgr[3] = {s[x * 4 + 0], s[x * 4 + 1], s[x * 4 + 2]};
            fwrite(bgr, 1, 3, f);
        }
        if (pad) fwrite(padbuf, 1, (size_t)pad, f);
    }
    fclose(f);
    return true;
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
    g_cmdCopyImgToBuf = (PFN_vkCmdCopyImageToBuffer)o.gipa(o.inst, "vkCmdCopyImageToBuffer");
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
    swci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
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

    // 实验 VK_SINGLE_PRESENT=1（默认关）：同一张交换链图在一个视觉帧内多次合成，
    // 第二次起用 LOAD pass 叠加上一层内容，最后只在真实 swap 时 present 一次。
    // 关键：中间合成结束在 COLOR_ATTACHMENT_OPTIMAL（不以 PRESENT_SRC 作 initialLayout）。
    {
        const char* v = getenv("VK_SINGLE_PRESENT");
        if (v && v[0] == '1') {
            auto mkRP = [&](VkAttachmentLoadOp lop, VkImageLayout init, VkImageLayout fin, VkRenderPass* out) {
                VkAttachmentDescription a2 = att;
                a2.loadOp = lop;
                a2.initialLayout = init;
                a2.finalLayout = fin;
                VkRenderPassCreateInfo rpci2{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
                rpci2.attachmentCount = 1;
                rpci2.pAttachments = &a2;
                rpci2.subpassCount = 1;
                rpci2.pSubpasses = &sub;
                *out = VK_NULL_HANDLE;
                createRP(o.dev, &rpci2, nullptr, out);
            };
            auto mkFB = [&](VkRenderPass rp, std::vector<VkFramebuffer>& fbs) {
                if (rp == VK_NULL_HANDLE) return;
                fbs.assign(ic, VK_NULL_HANDLE);
                for (uint32_t i = 0; i < ic; i++) {
                    VkFramebufferCreateInfo fci2{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
                    fci2.renderPass = rp;
                    fci2.attachmentCount = 1;
                    fci2.pAttachments = &o.swapViews[i];
                    fci2.width = o.extent.width;
                    fci2.height = o.extent.height;
                    fci2.layers = 1;
                    createFB(o.dev, &fci2, nullptr, &fbs[i]);
                }
            };
            // LOAD 叠加 + 最终 present（CA -> PRESENT）
            mkRP(VK_ATTACHMENT_LOAD_OP_LOAD, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, &o.renderPassLoad);
            // LOAD 叠加 + 继续持有（CA -> CA）
            mkRP(VK_ATTACHMENT_LOAD_OP_LOAD, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, &o.renderPassLoadKeep);
            // 首次合成（新图）+ 继续持有（UNDEFINED -> CA）
            mkRP(VK_ATTACHMENT_LOAD_OP_CLEAR, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, &o.renderPassKeep);
            mkFB(o.renderPassLoad, o.framebuffersLoad);
            mkFB(o.renderPassLoadKeep, o.framebuffersLoadKeep);
            mkFB(o.renderPassKeep, o.framebuffersKeep);
            fprintf(stderr, "[vk] singlepresent rp=%d loadKeep=%d keep=%d\n",
                    o.renderPassLoad != VK_NULL_HANDLE,
                    o.renderPassLoadKeep != VK_NULL_HANDLE,
                    o.renderPassKeep != VK_NULL_HANDLE);
        }
    }

    // 取证开关 VK_BBSTAT=1 / VK_DUMP_PRESENTS=1（默认关）：分配主机可见缓冲，
    // 把 present 前的交换链像素拷出来做亮度统计 / 写 BMP。
    {
        const char* v = getenv("VK_BBSTAT");
        const char* d = getenv("VK_DUMP_PRESENTS");
        if ((v && v[0] == '1') || (d && d[0] == '1')) {
            o.statBytes = (size_t)o.extent.width * (size_t)o.extent.height * 4;
            VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bci.size = o.statBytes;
            bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            if (g_createBuf(o.dev, &bci, nullptr, &o.statBuf) == VK_SUCCESS) {
                VkMemoryRequirements smr{};
                g_getBufReqs(o.dev, o.statBuf, &smr);
                VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
                mai.allocationSize = smr.size;
                mai.memoryTypeIndex = FindMemType(memProps, smr.memoryTypeBits,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
                if (g_allocMem(o.dev, &mai, nullptr, &o.statMem) == VK_SUCCESS) {
                    g_bindBufMem(o.dev, o.statBuf, o.statMem, 0);
                    g_mapMem(o.dev, o.statMem, 0, o.statBytes, 0, &o.statPtr);
                }
            }
        }
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
    VkVertexInputBindingDescription vbind[2]{};
    vbind[0].binding = 0;
    vbind[0].stride = sizeof(float) * 15;
    vbind[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    // binding1 = stride 0 的常量绑定，供 loc5/6/7 使用（否则未绑定属性读到 0，
    // 顶点着色器 openfl_Alphav = openfl_Alpha * alpha 会把 alpha 抹成 0）
    vbind[1].binding = 1;
    vbind[1].stride = 0;
    vbind[1].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription vattr[8]{};
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
    // loc5=alpha -> 1.0；loc6=colorMultiplier -> (1,1,1,1)；loc7=colorOffset -> 0
    vattr[5].location = 5;
    vattr[5].binding = 1;
    vattr[5].format = VK_FORMAT_R32_SFLOAT;
    vattr[5].offset = 0;
    vattr[6].location = 6;
    vattr[6].binding = 1;
    vattr[6].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    vattr[6].offset = sizeof(float) * 1;
    vattr[7].location = 7;
    vattr[7].binding = 1;
    vattr[7].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    vattr[7].offset = sizeof(float) * 5;
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 2;
    vi.pVertexBindingDescriptions = vbind;
    vi.vertexAttributeDescriptionCount = 8;
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

    // 常量顶点属性缓冲（binding1, stride=0）：loc5=1.0 / loc6=(1,1,1,1) / loc7=0
    static const float kConstAttrs[9] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    VkBufferCreateInfo abci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    abci.size = sizeof(kConstAttrs);
    abci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    g_createBuf(o.dev, &abci, nullptr, &o.attrBuf);
    VkMemoryRequirements amr{};
    g_getBufReqs(o.dev, o.attrBuf, &amr);
    VkMemoryAllocateInfo amai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    amai.allocationSize = amr.size;
    amai.memoryTypeIndex = FindMemType(memProps, amr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    g_allocMem(o.dev, &amai, nullptr, &o.attrMem);
    g_bindBufMem(o.dev, o.attrBuf, o.attrMem, 0);
    void* ap2 = nullptr;
    g_mapMem(o.dev, o.attrMem, 0, amr.size, 0, &ap2);
    o.attrPtr = (float*)ap2;
    if (o.attrPtr) memcpy(o.attrPtr, kConstAttrs, sizeof(kConstAttrs));

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

        // 取证（默认常开，仅大图打印一行）：确认大图上传的 CPU 数据本身是否全黑
        if ((size_t)w * h >= (size_t)2048 * 2048) {
            size_t cnt = 0, sum = 0, nb = 0;
            size_t px = (size_t)w * h;
            for (size_t k = 0; k < px; k += 97) {
                const unsigned char* p = rgba + k * 4;
                unsigned int v = (p[0] + p[1] + p[2]) / 3;
                sum += v;
                if (v >= 8) nb++;
                cnt++;
            }
            fprintf(stderr, "[vk] texmean id=%u w=%d h=%d mean=%.2f nb=%.1f\n",
                    (unsigned)i + 1, w, h,
                    cnt ? (double)sum / (double)cnt : 0.0,
                    cnt ? 100.0 * (double)nb / (double)cnt : 0.0);
        }
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

bool VkRenderer::SetDrawColor(float alpha, const float* mult, const float* offset) {
    g_drawAlpha = alpha;
    for (int i = 0; i < 4; i++) {
        g_drawMult[i] = mult ? mult[i] : 1.0f;
        g_drawOff[i] = offset ? offset[i] : 0.0f;
    }
    return true;
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
    // 诊断：实测游戏的采样方式（每个纹理 id 首次设置或参数变化时打印一次）
    if (texId < 1024) {
        static uint64_t s_sig[1024] = {0};
        static bool s_seen[1024] = {false};
        uint64_t sig = ((uint64_t)(uint32_t)minFilter << 48) | ((uint64_t)(uint32_t)magFilter << 32) |
                       ((uint64_t)(uint32_t)wrapS << 16) | (uint32_t)wrapT;
        if (!s_seen[texId] || s_sig[texId] != sig) {
            s_seen[texId] = true;
            s_sig[texId] = sig;
            fprintf(stderr, "[sampler] tex=%u min=0x%x(%s) mag=0x%x(%s) wrapS=0x%x wrapT=0x%x\n",
                    texId, minFilter, GlFilterToVk(minFilter) == VK_FILTER_NEAREST ? "NEAREST" : "LINEAR",
                    magFilter, GlFilterToVk(magFilter) == VK_FILTER_NEAREST ? "NEAREST" : "LINEAR",
                    wrapS, wrapT);
        }
    }
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

void VkRenderer::DrawRaw(uint32_t texId, const float* verts, int vertexCount, const float* vertexAlpha) {
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
    if (o.currentTarget == 0) {
        static int s_dbg = 0;
        if (s_dbg < 12 || s_dbg % 120 == 0) {
            fprintf(stderr, "[vk] draw->bb tex=%u n=%d\n", texId, vertexCount);
        }
        s_dbg++;
    }
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
    if (o.currentTarget == 0) g_bbLastTex = texId;
    if (texId >= 1000) {
        uint32_t ti = texId - 1000;
        tw = (float)o.targets[ti].w;
        th = (float)o.targets[ti].h;
        // 取证：采样的离屏源是否真的被写过、创建于哪一帧（判断"采样空目标"）
        static uint64_t s_sampleLog = 0;
        if (s_sampleLog < 20000) {
            fprintf(stderr, "[vkdraw] sample src=%u cur=%u srcLastWrite=%llu srcCreated=%llu\n",
                    texId, o.currentTarget,
                    (unsigned long long)o.targets[ti].lastWriteFrame,
                    (unsigned long long)o.targets[ti].createdFrame);
            s_sampleLog++;
        }
    } else {
        tw = (float)o.textures[texId - 1].w;
        th = (float)o.textures[texId - 1].h;
    }

    // 实验开关 VK_SKIP_3X=1：跳过"采样源尺寸 >= 2× 屏幕"的整图绘制（当前是
    // 2763x2073 的 3× 画布）。默认关闭；只用于判定它是不是黑帧的直接来源。
    {
        static int s_skip = -1;
        if (s_skip < 0) {
            const char* v = getenv ("VK_SKIP_3X");
            s_skip = (v && v[0] == '1') ? 1 : 0;
        }
        if (s_skip && (tw >= 2.0f * (float)o.width || th >= 2.0f * (float)o.height)) {
            static int s_n = 0;
            if (s_n < 5) {
                fprintf(stderr, "[vk] skip3x src=%u %dx%d cur=%u\n",
                        texId, (int)tw, (int)th, o.currentTarget);
            }
            s_n++;
            return;
        }
    }

    // 采样方式实测：多数贴图是 LINEAR。像素坐标 uv 正好落在帧边界时，双线性
    // 会混入相邻格（表现为贴图四周一圈异色边框）。按 min 侧 +0.5、max 侧 -0.5
    // 内缩半像素，使采样点落在首/末纹素中心；只对像素坐标 uv 生效。
    float uMin = 0, uMax = 0, vMin = 0, vMax = 0;
    bool pixelUv = false;
    if (vertexCount > 0) {
        uMin = uMax = verts[2];
        vMin = vMax = verts[3];
        for (int i = 0; i < vertexCount; i++) {
            float u = verts[i * 4 + 2];
            float v = verts[i * 4 + 3];
            if (u > 1.001f || u < -1.001f || v > 1.001f || v < -1.001f) pixelUv = true;
            if (u < uMin) uMin = u;
            if (u > uMax) uMax = u;
            if (v < vMin) vMin = v;
            if (v > vMax) vMax = v;
        }
    }

    // 取证：每次绘制一条紧凑记录（纹理 id/尺寸、uv 范围、顶点包围盒、是否触发
    // 归一化分支、uv 是否覆盖整张贴图）。前 12 帧全量，之后只打"覆盖整图"的嫌疑绘制
    {
        float xMin = verts[0], xMax = verts[0], yMin = verts[1], yMax = verts[1];
        for (int i = 0; i < vertexCount; i++) {
            float x = verts[i * 4 + 0];
            float y = verts[i * 4 + 1];
            if (x < xMin) xMin = x; if (x > xMax) xMax = x;
            if (y < yMin) yMin = y; if (y > yMax) yMax = y;
        }
        // norm=1 表示走了像素坐标分支（uv 已按纹理尺寸归一化并内缩）；
        // whole=1 表示 uv 跨度接近整张贴图 → 该绘制很可能把整图铺进四边形
        bool spansWhole = false;
        if (!pixelUv) spansWhole = (uMax - uMin) >= 0.9f && (vMax - vMin) >= 0.9f;
        else if (tw > 0 && th > 0) spansWhole = (uMax - uMin) >= tw * 0.9f && (vMax - vMin) >= th * 0.9f;
        float aMin = g_drawAlpha, aMax = g_drawAlpha;
        if (vertexAlpha) {
            aMin = vertexAlpha[0]; aMax = vertexAlpha[0];
            for (int i = 1; i < vertexCount; i++) {
                if (vertexAlpha[i] < aMin) aMin = vertexAlpha[i];
                if (vertexAlpha[i] > aMax) aMax = vertexAlpha[i];
            }
        }
        static uint64_t s_logged = 0;
        if (s_logged < 6000 && (g_drawFrame <= 12 || spansWhole)) {
            fprintf(stderr, "[vkdraw] f=%llu cur=%u tex=%u tsz=%.0fx%.0f n=%d uv=[%.3f..%.3f, %.3f..%.3f] pos=[%.1f..%.1f, %.1f..%.1f] norm=%d whole=%d asrc=%s amin=%.3f amax=%.3f\n",
                    (unsigned long long)g_drawFrame, o.currentTarget, texId, tw, th, vertexCount,
                    uMin, uMax, vMin, vMax, xMin, xMax, yMin, yMax,
                    pixelUv ? 1 : 0, spansWhole ? 1 : 0, vertexAlpha ? "arr" : "const", aMin, aMax);
            s_logged++;
        }
    }

    vertsPtr->reserve(vertsPtr->size() + (size_t)vertexCount * 15);
    for (int i = 0; i < vertexCount; i++) {
        float px = verts[i * 4 + 0];
        float py = verts[i * 4 + 1];
        float u = verts[i * 4 + 2];
        float v = verts[i * 4 + 3];
        if (pixelUv) {
            // 内缩与归一化必须一起做：若先 +0.5 再判断 |u|>1.001，min=0 的
            // 顶点会变成 0.5 而躲过归一化，整屏采样错位
            float eu = u, ev = v;
            if (u <= uMin + 1e-3f && u < uMax) eu = u + 0.5f;
            else if (u >= uMax - 1e-3f) eu = u - 0.5f;
            if (v <= vMin + 1e-3f && v < vMax) ev = v + 0.5f;
            else if (v >= vMax - 1e-3f) ev = v - 0.5f;
            u = tw > 0 ? eu / tw : 1.0f;
            v = th > 0 ? ev / th : 1.0f;
        } else {
            // 像素坐标直接进顶点：像素→NDC 的换算由 UBO 里的投影矩阵完成
            // uv 超出 0..1 说明传进来的是像素坐标，按采样源尺寸归一化，否则采样越界全黑
            if (u > 1.001f || u < -1.001f) u = tw > 0 ? u / tw : 1.0f;
            if (v > 1.001f || v < -1.001f) v = th > 0 ? v / th : 1.0f;
        }
        PushLimeVertex(*vertsPtr, px, py, u, v,
                       vertexAlpha ? vertexAlpha[i] : g_drawAlpha, g_drawMult, g_drawOff);
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

uint32_t VkRenderer::LastGeometryTarget() const {
    return impl->lastGeomTarget;
}

uint64_t VkRenderer::FrameIndex() const {
    return g_drawFrame;
}

uint32_t VkRenderer::BackbufferLastSample() const {
    return g_bbLastTex;
}

void VkRenderer::ResetLastGeometryTarget() {
    impl->lastGeomTarget = 0;
}

bool VkRenderer::BindRenderTarget(uint32_t id, float r, float g, float b, float a) {
    Impl& o = *impl;
    // 同一目标重复绑定（OpenFL 每个 pass 开头都会 glClear，gl_vk_handle_clear 又会 Bind 一次）
    // 不能清空已经累积的顶点，否则整条 filter 链最后合成时 verts=0，屏幕只剩清屏色。
    // 这里只更新清屏色，等 EndFrame 时真正清屏。
    if (o.frameOpen && o.currentTarget == id) {
        // 实验 VK_SINGLE_PRESENT=1：同一张交换链图本帧已被占用，直接复用，不 acquire 也不提交。
        // 这里不能改 bbLoad：新 acquire 的图 bbLoad=false（必须 CLEAR），已叠加的图 bbLoad=true（必须 LOAD）。
        if (id == 0 && o.bbHeld && o.renderPassLoad != VK_NULL_HANDLE) {
            static uint64_t s_reuse = 0;
            s_reuse++;
            if (s_reuse <= 2000) {
                fprintf(stderr, "[vk] bind0-reuse n=%llu load=%d clear=(%.2f,%.2f,%.2f)\n",
                        (unsigned long long)s_reuse, o.bbLoad ? 1 : 0, r, g, b);
            }
            o.clearR = r;
            o.clearG = g;
            o.clearB = b;
            o.clearA = a;
            return true;
        }
        // 取证开关 VK_FRESH_IMG=1（默认关）：第二次绑定后缓冲时，若已累积几何，
        // 先 EndFrame 做一次 present 归还当前 image，再 acquire 新图。
        // 目的：保持 acquire↔present 一一对应（不重走 §17 的 skip-present），
        // 验证"同一张 img0 被 present 两次"是不是黑帧的直接机制。
        static int s_fresh = -1;
        if (s_fresh < 0) {
            const char* v = getenv ("VK_FRESH_IMG");
            s_fresh = (v && v[0] == '1') ? 1 : 0;
        }
        if (!(s_fresh && id == 0 && FrameHasGeometry())) {
            o.clearR = r;
            o.clearG = g;
            o.clearB = b;
            o.clearA = a;
            return true;
        }
        EndFrame ();
    }
    // 切换目标前先把旧离屏 pass 真正提交进旧 FBO：否则整帧所有 pass 的顶点
    // 会被攒到 present 那一刻、用最后一张图（后缓冲）的投影一次性画出去，
    // 表现为帧间闪烁、元素位置偏移、贴图边缘露出。
    if (o.frameOpen && o.currentTarget != id && o.currentTarget >= 1000 && FrameHasGeometry()) {
        EndFrame ();
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
        if (o.bbHeld && o.renderPassLoad != VK_NULL_HANDLE) {
            // 实验 VK_SINGLE_PRESENT=1：本帧已持有同一张图，后续合成用 LOAD pass 叠加
            o.bbLoad = true;
        } else {
            VkResult ar = g_acquire(o.dev, o.swapchain, UINT64_MAX, VK_NULL_HANDLE, VK_NULL_HANDLE, &o.frameImageIndex);
            if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) return false;
            o.bbHeld = true;
            o.bbLoad = false;
        }
    } else {
        uint32_t ti = id - 1000;
        if (ti >= o.targets.size() || !o.targets[ti].used) return false;
    }
    o.frameOpen = true;
    return true;
}

void VkRenderer::PresentTarget() {
    // FBO 切换触发的收尾：只提交几何，不当作真实 flip（真实 flip 由 gl_fbo 显式 RequestPresent）
    EndFrame();
}

void VkRenderer::RequestPresent() {
    impl->presentReq = true;
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
    t.createdFrame = g_drawFrame;
    fprintf(stderr, "[vk] rt new id=%u %dx%d f=%llu\n", 1000 + idx, w, h,
            (unsigned long long)g_drawFrame);
    return 1000 + idx;
}

void VkRenderer::DestroyRenderTarget(uint32_t id) {
    Impl& o = *impl;
    if (id < 1000) return;
    uint32_t ti = id - 1000;
    if (ti >= o.targets.size()) return;
    RenderTarget& t = o.targets[ti];
    if (!t.used) return;
    fprintf(stderr, "[vk] rt del id=%u %dx%d f=%llu lastWrite=%llu\n", id, t.w, t.h,
            (unsigned long long)g_drawFrame, (unsigned long long)t.lastWriteFrame);
    if (t.fb) o.destroyFramebuffer(o.dev, t.fb, nullptr);
    if (t.rp) o.destroyRenderPass(o.dev, t.rp, nullptr);
    if (t.view) o.destroyImageView(o.dev, t.view, nullptr);
    if (t.image) o.destroyImage(o.dev, t.image, nullptr);
    if (t.mem) o.freeMemory(o.dev, t.mem, nullptr);
    t = RenderTarget();
    // 目标 id 按槽位复用，留着旧 id 会让 present 采样到无关的目标
    if (o.lastGeomTarget == id) o.lastGeomTarget = 0;
}

void VkRenderer::EndFrame() {
    Impl& o = *impl;
    if (!o.frameOpen) return;

    float* dst = o.vbPtr;
    struct DrawRange { VkDescriptorSet set; size_t first; size_t count; };
    std::vector<DrawRange> ranges;
    size_t totalVerts = 0;

    // 取证：按提交实际拼出的批次顺序记录源 id（贴图 1..N，离屏 1000+槽位）
    std::vector<uint32_t> batchIds;
    auto pushBatch = [&](std::vector<float>& v, VkDescriptorSet set, uint32_t id) {
        if (v.empty()) return;
        size_t vc = v.size() / 15;
        if (totalVerts + vc > (size_t)MAX_SPRITES * 6) return;
        memcpy(dst + totalVerts * 15, v.data(), v.size() * sizeof(float));
        ranges.push_back({set, totalVerts, vc});
        totalVerts += vc;
        batchIds.push_back(id);
    };
    // 实验 VK_ORDER_11FIRST=1（默认关）：3× 全屏源（贴图 11）改为最先画，
    // 而不是按 id 升序被放到最后盖住其它所有层。用于判定“黑帧=贴图 11 全屏覆盖”。
    static int s_11first = -1;
    if (s_11first < 0) {
        const char* v = getenv("VK_ORDER_11FIRST");
        s_11first = (v && v[0] == '1') ? 1 : 0;
    }
    // 实验 VK_SKIP_TEX=<id[,id]>（默认关）：整批丢掉指定贴图的顶点，用于定位覆盖源
    static const char* s_skipCfg = getenv("VK_SKIP_TEX");
    auto skipped = [](uint32_t id) {
        if (!s_skipCfg || !s_skipCfg[0]) return false;
        char buf[128];
        snprintf(buf, sizeof(buf), ",%s,", s_skipCfg);
        char key[32];
        snprintf(key, sizeof(key), ",%u,", id);
        return strstr(buf, key) != nullptr;
    };
    if (s_11first && o.textures.size() > 10 && o.textures[10].used) {
        pushBatch(o.textures[10].verts, o.textures[10].set, 11);
    }
    for (size_t i = 0; i < o.textures.size(); i++) {
        if (s_11first && i == 10) continue;
        if (!o.textures[i].used) continue;
        uint32_t id = (uint32_t)(i + 1);
        if (skipped(id)) continue;
        pushBatch(o.textures[i].verts, o.textures[i].set, id);
    }
    for (size_t i = 0; i < o.targets.size(); i++) {
        if (o.targets[i].used) pushBatch(o.targets[i].verts, o.targets[i].set, (uint32_t)(1000 + i));
    }

    // 取证：VK_TAG_PRESENT=1 时在屏幕角落画一块白色小方块，标记本次后缓冲合成
    // 采样的是哪条链：左下 = 1001（A 周期），右下 = 1002（B 周期）。
    // 默认关闭；只用白色贴图（GL name 8，60x15，首像素 ffffffff），不改渲染结果。
    {
        static int s_tag = -1;
        if (s_tag < 0) {
            const char* v = getenv("VK_TAG_PRESENT");
            s_tag = (v && v[0] == '1') ? 1 : 0;
        }
        if (s_tag && o.currentTarget == 0 && totalVerts > 0 &&
                (g_bbLastTex == 1001 || g_bbLastTex == 1002) &&
                o.textures.size() >= 8 && o.textures[7].used && o.textures[7].w == 60) {
            float W = (float)o.width, H = (float)o.height;
            float x0 = (g_bbLastTex == 1001) ? 0.0f : (W - 30.0f);
            float y0 = H - 15.0f;
            std::vector<float> q;
            PushLimeVertex(q, x0, y0, 0.0f, 0.0f);
            PushLimeVertex(q, x0 + 30.0f, y0, 1.0f, 0.0f);
            PushLimeVertex(q, x0 + 30.0f, H, 1.0f, 1.0f);
            PushLimeVertex(q, x0, y0, 0.0f, 0.0f);
            PushLimeVertex(q, x0 + 30.0f, H, 1.0f, 1.0f);
            PushLimeVertex(q, x0, H, 0.0f, 1.0f);
            pushBatch(q, o.textures[7].set, 8);
        }
    }

    // present 选源：记下最后一帧真正收到顶点（vertexCount>0）的离屏目标。
    // 只绑了 FBO、clear 过、没写几何的目标会呈现为黑纹帧，不能拿它当合成源。
    if (totalVerts > 0 && o.currentTarget >= 1000) {
        o.lastGeomTarget = o.currentTarget;
    }

    VkFramebuffer fb = o.framebuffers[o.frameImageIndex];
    VkRenderPass rp = o.renderPass;
    VkExtent2D ext = o.extent;
    // 实验 VK_SINGLE_PRESENT=1：一视觉帧只 present 一次（真实 swap 由 gl_fbo 置 presentReq）。
    // 中间合成必须把图保持在 COLOR_ATTACHMENT_OPTIMAL；只有最终那次以 PRESENT_SRC 收尾。
    bool singlePresent = (o.renderPassLoad != VK_NULL_HANDLE &&
                          o.renderPassLoadKeep != VK_NULL_HANDLE &&
                          o.renderPassKeep != VK_NULL_HANDLE);
    bool willPresent = true;
    bool finalPresent = true;   // 本次 pass 结束时图是否已转成 PRESENT_SRC
    if (o.currentTarget == 0) {
        if (!singlePresent) {
            fb = o.framebuffers[o.frameImageIndex];
            rp = o.renderPass;
            willPresent = true;
            finalPresent = true;
        } else {
            willPresent = (o.bbHeld && o.presentReq);
            finalPresent = willPresent;
            if (o.bbLoad) {
                rp = willPresent ? o.renderPassLoad : o.renderPassLoadKeep;
                fb = willPresent ? o.framebuffersLoad[o.frameImageIndex]
                                 : o.framebuffersLoadKeep[o.frameImageIndex];
            } else {
                rp = willPresent ? o.renderPass : o.renderPassKeep;
                fb = willPresent ? o.framebuffers[o.frameImageIndex]
                                 : o.framebuffersKeep[o.frameImageIndex];
            }
            if (fb == VK_NULL_HANDLE || rp == VK_NULL_HANDLE) {
                fprintf(stderr, "[vk] singlepresent pass missing (willPresent=%d load=%d)\n",
                        willPresent ? 1 : 0, o.bbLoad ? 1 : 0);
                o.frameOpen = false;
                return;
            }
        }
    }
    {
        static uint64_t s_frames = 0;
        s_frames++;
        g_drawFrame = s_frames;
        // 每次提交一条不门控的 summary（含离屏目标），用于把黑帧对齐到具体提交：
        // delsample = 距该目标上次真正写入的帧数（-1 = 从未写过）
        static uint64_t s_submitLog = 0;
        if (s_submitLog < 30000) {
            long long delsample = -1;
            uint64_t created = 0;
            if (o.currentTarget >= 1000) {
                uint32_t ti = o.currentTarget - 1000;
                if (ti < o.targets.size() && o.targets[ti].used) {
                    created = o.targets[ti].createdFrame;
                    if (o.targets[ti].lastWriteFrame != 0) {
                        delsample = (long long)(s_frames - o.targets[ti].lastWriteFrame);
                    }
                    if (totalVerts > 0) o.targets[ti].lastWriteFrame = s_frames;
                }
            }
            char srcs[256] = {0};
            {
                size_t n = (batchIds.size() < 16) ? batchIds.size() : 16;
                size_t pos = 0;
                for (size_t i = 0; i < n && pos + 12 < sizeof(srcs); i++) {
                    pos += (size_t)snprintf(srcs + pos, sizeof(srcs) - pos, "%s%u", i ? "," : "", batchIds[i]);
                }
                if (batchIds.size() > 16 && pos + 4 < sizeof(srcs)) snprintf(srcs + pos, sizeof(srcs) - pos, "+%zu", batchIds.size() - 16);
            }
            fprintf(stderr, "[vk] submit frame=%llu t=%lld tgt=%u verts=%zu ranges=%zu delsample=%lld created=%llu tex=%u srcs=%s\n",
                    (unsigned long long)s_frames, (long long)time(nullptr), o.currentTarget,
                    totalVerts, ranges.size(), delsample, (unsigned long long)created,
                    (o.currentTarget == 0) ? g_bbLastTex : 0u, srcs);
            s_submitLog++;
        }
        if (s_frames == 1 || s_frames % 30 == 0 || o.currentTarget == 0) {
            float first[4] = {0, 0, 0, 0};
            if (totalVerts > 0) memcpy(first, o.vbPtr, sizeof(first));
            fprintf(stderr, "[vk] frame=%llu verts=%zu ranges=%zu target=%u first=(%.1f,%.1f,%.2f,%.2f)\n",
                    (unsigned long long)s_frames, totalVerts, ranges.size(), o.currentTarget,
                    first[0], first[1], first[2], first[3]);
            if (o.currentTarget == 0 && totalVerts >= 6) {
                fprintf(stderr, "[vk] v6=(%.1f,%.1f,%.3f,%.3f) (%.1f,%.1f,%.3f,%.3f) (%.1f,%.1f,%.3f,%.3f) | (%.1f,%.1f,%.3f,%.3f) (%.1f,%.1f,%.3f,%.3f) (%.1f,%.1f,%.3f,%.3f)\n",
                        o.vbPtr[0], o.vbPtr[1], o.vbPtr[13], o.vbPtr[14],
                        o.vbPtr[15], o.vbPtr[16], o.vbPtr[28], o.vbPtr[29],
                        o.vbPtr[30], o.vbPtr[31], o.vbPtr[43], o.vbPtr[44],
                        o.vbPtr[45], o.vbPtr[46], o.vbPtr[58], o.vbPtr[59],
                        o.vbPtr[60], o.vbPtr[61], o.vbPtr[73], o.vbPtr[74],
                        o.vbPtr[75], o.vbPtr[76], o.vbPtr[88], o.vbPtr[89]);
            }
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
        // hasTransform 在 std140 里是 byte 84（float index 21）。置 1 才会走
        // flixel_texture2D 的 color * openfl_Alphav 分支；否则 alpha 被整个忽略，
        // 3× 黑色全屏源以 alpha=1 盖住所有层（黑帧）。hasColorTransform 保持 0。
        o.uboPtr[21] = 1.0f;
    }

    g_beginRP(o.cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    if (totalVerts > 0) {
        g_bindPipe(o.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, o.pipeline);
        VkBuffer vbufs[2] = {o.vb, o.attrBuf};
        VkDeviceSize voffs[2] = {0, 0};
        g_bindVB(o.cmd, 0, 2, vbufs, voffs);
        for (auto& r : ranges) {
            g_bindDS(o.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, o.pipeLayout, 0, 1, &r.set, 0, nullptr);
            g_cmdDraw(o.cmd, (uint32_t)r.count, 1, (uint32_t)r.first, 0);
        }
    }
    g_endRP(o.cmd);

    // 取证 VK_BBSTAT=1（默认关）：present 前把后缓冲拷到主机缓冲，供亮度统计。
    // 只在本层 target=0 且有几何时做；barrier 走 PRESENT_SRC -> TRANSFER_SRC -> PRESENT_SRC。
    bool doStat = (o.statBuf != VK_NULL_HANDLE && o.statPtr && o.currentTarget == 0 && totalVerts > 0);
    if (doStat) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        // 本次 pass 的 finalLayout 决定当前真实布局（PRESENT_SRC 或 COLOR_ATTACHMENT_OPTIMAL）
        VkImageLayout curLayout = finalPresent ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                                               : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        b.oldLayout = curLayout;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = o.swapImages[o.frameImageIndex];
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.levelCount = 1;
        b.subresourceRange.layerCount = 1;
        b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        g_cmdBarrier(o.cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);

        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent.width = (uint32_t)o.extent.width;
        region.imageExtent.height = (uint32_t)o.extent.height;
        region.imageExtent.depth = 1;
        g_cmdCopyImgToBuf(o.cmd, o.swapImages[o.frameImageIndex], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          o.statBuf, 1, &region);

        VkImageMemoryBarrier b2 = b;
        b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b2.newLayout = curLayout;
        b2.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b2.dstAccessMask = 0;
        g_cmdBarrier(o.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &b2);
    }
    g_endCB(o.cmd);

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &o.cmd;
    g_submit(o.queue, 1, &si, VK_NULL_HANDLE);
    g_waitIdle(o.queue);

    if (doStat) {
        const uint8_t* px = (const uint8_t*)o.statPtr;
        double sum = 0;
        size_t nb = 0, cnt = 0;
        for (size_t yy = 0; yy < (size_t)o.extent.height; yy += 4) {
            for (size_t xx = 0; xx < (size_t)o.extent.width; xx += 4) {
                size_t idx = (yy * (size_t)o.extent.width + xx) * 4;
                int v = ((int)px[idx] + (int)px[idx + 1] + (int)px[idx + 2]) / 3;
                sum += v;
                if (v > 24) nb++;
                cnt++;
            }
        }
        static uint64_t s_stat = 0;
        s_stat++;
        if (s_stat <= 20000) {
            fprintf(stderr, "[vk] bbstat n=%llu f=%llu mean=%.2f nb=%.2f img=%u load=%d final=%d\n",
                    (unsigned long long)s_stat, (unsigned long long)g_drawFrame,
                    cnt ? sum / (double)cnt : 0.0, cnt ? 100.0 * (double)nb / (double)cnt : 0.0,
                    o.frameImageIndex, o.bbLoad ? 1 : 0, finalPresent ? 1 : 0);
        }
        // 取证 VK_DUMP_PRESENTS=1（默认关）：落盘前 24 张交换链图 BMP，供与窗口截图比对
        static int s_dump = -1;
        if (s_dump < 0) {
            const char* dv = getenv("VK_DUMP_PRESENTS");
            s_dump = (dv && dv[0] == '1') ? 1 : 0;
        }
        if (s_dump) {
            static uint64_t s_dumped = 0;
            if (s_dumped < 24) {
                char name[160];
                snprintf(name, sizeof(name), "vk_pres_%02llu_f%llu_mean%.0f.bmp",
                         (unsigned long long)s_dumped, (unsigned long long)g_drawFrame,
                         cnt ? sum / (double)cnt : 0.0);
                s_dumped++;
                WriteBmpBGRA(name, px, o.extent.width, o.extent.height);
                fprintf(stderr, "[vk] dump %s\n", name);
            }
        }
    }

    if (o.currentTarget == 0) {
        // 实验 VK_SINGLE_PRESENT=1：中间合成只提交几何、保持 image 不归还；
        // 只有真实 swap（gl_fbo 调 RequestPresent）才 present 一次。
        if (!willPresent) {
            static uint64_t s_nopres = 0;
            s_nopres++;
            if (s_nopres <= 20000) {
                fprintf(stderr, "[vk] nopres n=%llu f=%llu tex=%u verts=%zu load=%d\n",
                        (unsigned long long)s_nopres, (unsigned long long)g_drawFrame,
                        g_bbLastTex, totalVerts, o.bbLoad ? 1 : 0);
            }
        } else {
            // 取证：每次 present 一行（一个视觉帧通常 2 次，其中只有 ContextFlip 那次
            // 会在 gl_fbo 里另打一条 [glvk] present），用于把黑帧对齐到交换链图。
            static uint64_t s_pres = 0;
            s_pres++;
            if (s_pres <= 20000) {
                fprintf(stderr, "[vk] pres n=%llu f=%llu img=%u tex=%u verts=%zu load=%d\n",
                        (unsigned long long)s_pres, (unsigned long long)g_drawFrame,
                        o.frameImageIndex, g_bbLastTex, totalVerts, o.bbLoad ? 1 : 0);
            }
            VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
            pi.swapchainCount = 1;
            pi.pSwapchains = &o.swapchain;
            pi.pImageIndices = &o.frameImageIndex;
            VkResult pr = g_present(o.queue, &pi);
            static uint64_t s_badPres = 0;
            if (pr != VK_SUCCESS && pr != VK_SUBOPTIMAL_KHR) {
                s_badPres++;
                if (s_badPres <= 2000) {
                    fprintf(stderr, "[vk] present failed r=%d n=%llu\n", (int)pr,
                            (unsigned long long)s_badPres);
                }
            }
            o.bbHeld = false;
            o.presentReq = false;
        }
    }

    o.frameOpen = false;
    g_bbLastTex = 0;
    for (auto& t : o.textures) t.verts.clear();
    for (auto& t : o.targets) t.verts.clear();
}
