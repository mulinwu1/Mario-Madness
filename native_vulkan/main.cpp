#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#define VK_USE_PLATFORM_WIN32_KHR
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <vector>

static HWND g_hwnd;
static const int W = 1280, H = 720;

static LRESULT CALLBACK WP(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLOSE || (m == WM_KEYDOWN && w == VK_ESCAPE)) { PostQuitMessage(0); return 0; }
    return DefWindowProc(h, m, w, l);
}

static HWND MakeWin() {
    WNDCLASS wc{};
    wc.lpfnWndProc = WP;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = "VKDemo";
    RegisterClass(&wc);
    RECT r{0, 0, W, H};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    return CreateWindow("VKDemo", "Lime Vulkan Demo", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, 0, 0, wc.hInstance, 0);
}

int main() {
    g_hwnd = MakeWin();
    if (!g_hwnd) { printf("[vk] window failed\n"); return 1; }

    HMODULE dll = LoadLibraryA("vulkan-1.dll");
    if (!dll) { printf("[vk] vulkan-1.dll missing\n"); return 1; }
    auto gipa = (PFN_vkGetInstanceProcAddr)GetProcAddress(dll, "vkGetInstanceProcAddr");
    auto createInstance = (PFN_vkCreateInstance)GetProcAddress(dll, "vkCreateInstance");
    if (!gipa || !createInstance) { printf("[vk] entrypoints missing\n"); return 1; }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Lime Vulkan Demo";
    app.apiVersion = VK_API_VERSION_1_0;

    const char* ext[] = {"VK_KHR_surface", "VK_KHR_win32_surface"};
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = ext;

    VkInstance inst = nullptr;
    if (createInstance(&ici, nullptr, &inst) != VK_SUCCESS) { printf("[vk] instance failed\n"); return 1; }

    auto enumDev = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
    auto getProps = (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
    auto getQFP = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)gipa(inst, "vkGetPhysicalDeviceQueueFamilyProperties");
    auto getSurfSupport = (PFN_vkGetPhysicalDeviceSurfaceSupportKHR)gipa(inst, "vkGetPhysicalDeviceSurfaceSupportKHR");
    auto createSurface = (PFN_vkCreateWin32SurfaceKHR)gipa(inst, "vkCreateWin32SurfaceKHR");
    auto getSurfCaps = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)gipa(inst, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    auto getSurfFormats = (PFN_vkGetPhysicalDeviceSurfaceFormatsKHR)gipa(inst, "vkGetPhysicalDeviceSurfaceFormatsKHR");
    auto createDevice = (PFN_vkCreateDevice)gipa(inst, "vkCreateDevice");

    if (!enumDev || !getProps || !getQFP || !getSurfSupport || !createSurface || !createDevice) {
        printf("[vk] missing instance functions\n");
        return 1;
    }

    uint32_t n = 0;
    enumDev(inst, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    enumDev(inst, &n, devs.data());
    if (n == 0) { printf("[vk] no devices\n"); return 1; }

    VkPhysicalDevice phys = devs[0];
    VkPhysicalDeviceProperties props{};
    getProps(phys, &props);
    printf("[vk] device: %s  API %u.%u.%u\n", props.deviceName,
        VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
        VK_VERSION_PATCH(props.apiVersion));

    VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    sci.hinstance = GetModuleHandle(nullptr);
    sci.hwnd = g_hwnd;
    VkSurfaceKHR surf = nullptr;
    if (createSurface(inst, &sci, nullptr, &surf) != VK_SUCCESS) { printf("[vk] surface failed\n"); return 1; }

    uint32_t qn = 0;
    getQFP(phys, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qs(qn);
    getQFP(phys, &qn, qs.data());

    int qidx = -1;
    for (uint32_t i = 0; i < qn; i++) {
        VkBool32 ok = VK_FALSE;
        getSurfSupport(phys, i, surf, &ok);
        if ((qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && ok) { qidx = (int)i; break; }
    }
    if (qidx < 0) { printf("[vk] no graphics+present queue\n"); return 1; }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = (uint32_t)qidx;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    const char* dext[] = {"VK_KHR_swapchain"};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = dext;

    VkDevice dev = nullptr;
    if (createDevice(phys, &dci, nullptr, &dev) != VK_SUCCESS) { printf("[vk] device failed\n"); return 1; }

    auto getQueue = (PFN_vkGetDeviceQueue)gipa(inst, "vkGetDeviceQueue");
    auto createSwap = (PFN_vkCreateSwapchainKHR)gipa(inst, "vkCreateSwapchainKHR");
    auto getImages = (PFN_vkGetSwapchainImagesKHR)gipa(inst, "vkGetSwapchainImagesKHR");
    auto createView = (PFN_vkCreateImageView)gipa(inst, "vkCreateImageView");
    auto createRP = (PFN_vkCreateRenderPass)gipa(inst, "vkCreateRenderPass");
    auto createFB = (PFN_vkCreateFramebuffer)gipa(inst, "vkCreateFramebuffer");
    auto createPool = (PFN_vkCreateCommandPool)gipa(inst, "vkCreateCommandPool");
    auto allocCB = (PFN_vkAllocateCommandBuffers)gipa(inst, "vkAllocateCommandBuffers");
    auto beginCB = (PFN_vkBeginCommandBuffer)gipa(inst, "vkBeginCommandBuffer");

    VkQueue queue = nullptr;
    getQueue(dev, (uint32_t)qidx, 0, &queue);

    VkSurfaceCapabilitiesKHR caps{};
    getSurfCaps(phys, surf, &caps);

    uint32_t fmtCount = 0;
    getSurfFormats(phys, surf, &fmtCount, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(fmtCount);
    getSurfFormats(phys, surf, &fmtCount, fmts.data());
    VkSurfaceFormatKHR fmt = fmts[0];
    for (auto& f : fmts) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM) { fmt = f; break; }
    }

    VkExtent2D ext2 = caps.currentExtent;
    if (ext2.width == 0xFFFFFFFF) { ext2.width = W; ext2.height = H; }

    VkSwapchainCreateInfoKHR swci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    swci.surface = surf;
    swci.minImageCount = caps.minImageCount + 1;
    swci.imageFormat = fmt.format;
    swci.imageColorSpace = fmt.colorSpace;
    swci.imageExtent = ext2;
    swci.imageArrayLayers = 1;
    swci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    swci.preTransform = caps.currentTransform;
    swci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swci.clipped = VK_TRUE;

    VkSwapchainKHR sw = nullptr;
    if (createSwap(dev, &swci, nullptr, &sw) != VK_SUCCESS) { printf("[vk] swapchain failed\n"); return 1; }

    uint32_t imgCount = 0;
    getImages(dev, sw, &imgCount, nullptr);
    std::vector<VkImage> imgs(imgCount);
    getImages(dev, sw, &imgCount, imgs.data());

    std::vector<VkImageView> views(imgCount);
    for (uint32_t i = 0; i < imgCount; i++) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = imgs[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = fmt.format;
        vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.levelCount = 1;
        vci.subresourceRange.layerCount = 1;
        createView(dev, &vci, nullptr, &views[i]);
    }

    VkAttachmentDescription att{};
    att.format = fmt.format;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
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

    VkRenderPass rp = nullptr;
    createRP(dev, &rpci, nullptr, &rp);

    std::vector<VkFramebuffer> fbs(imgCount);
    for (uint32_t i = 0; i < imgCount; i++) {
        VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fci.renderPass = rp;
        fci.attachmentCount = 1;
        fci.pAttachments = &views[i];
        fci.width = ext2.width;
        fci.height = ext2.height;
        fci.layers = 1;
        createFB(dev, &fci, nullptr, &fbs[i]);
    }

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.queueFamilyIndex = (uint32_t)qidx;
    VkCommandPool pool = nullptr;
    createPool(dev, &pci, nullptr, &pool);

    VkCommandBufferAllocateInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbi.commandPool = pool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    VkCommandBuffer cb = nullptr;
    allocCB(dev, &cbi, &cb);

    auto acquire = (PFN_vkAcquireNextImageKHR)gipa(inst, "vkAcquireNextImageKHR");
    auto present = (PFN_vkQueuePresentKHR)gipa(inst, "vkQueuePresentKHR");
    auto submit = (PFN_vkQueueSubmit)gipa(inst, "vkQueueSubmit");
    auto waitIdle = (PFN_vkQueueWaitIdle)gipa(inst, "vkQueueWaitIdle");
    auto endCB = (PFN_vkEndCommandBuffer)gipa(inst, "vkEndCommandBuffer");
    auto beginRP = (PFN_vkCmdBeginRenderPass)gipa(inst, "vkCmdBeginRenderPass");
    auto endRP = (PFN_vkCmdEndRenderPass)gipa(inst, "vkCmdEndRenderPass");

    printf("[vk] swapchain %ux%u, images=%u\n", ext2.width, ext2.height, imgCount);
    printf("[vk] running - press ESC or close window to quit\n");
    fflush(stdout);

    MSG msg{};
    bool run = true;
    uint32_t idx = 0;
    float t = 0.0f;

    while (run) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { run = false; break; }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (!run) break;

        VkResult ar = acquire(dev, sw, UINT64_MAX, VK_NULL_HANDLE, VK_NULL_HANDLE, &idx);
        if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) { Sleep(1); continue; }

        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        beginCB(cb, &bi);

        t += 0.016f;
        VkClearValue cv{};
        cv.color.float32[0] = 0.05f + 0.25f * (0.5f + 0.5f * (float)((int)(t * 4) % 2));
        cv.color.float32[1] = 0.10f;
        cv.color.float32[2] = 0.55f;
        cv.color.float32[3] = 1.0f;

        VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rbi.renderPass = rp;
        rbi.framebuffer = fbs[idx];
        rbi.renderArea.offset = {0, 0};
        rbi.renderArea.extent = ext2;
        rbi.clearValueCount = 1;
        rbi.pClearValues = &cv;
        beginRP(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        endRP(cb);
        endCB(cb);

        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        submit(queue, 1, &si, VK_NULL_HANDLE);
        waitIdle(queue);

        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.swapchainCount = 1;
        pi.pSwapchains = &sw;
        pi.pImageIndices = &idx;
        present(queue, &pi);

        Sleep(16);
    }

    waitIdle(queue);
    printf("[vk] exiting cleanly\n");
    return 0;
}
