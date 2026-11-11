#include "vk_renderer.h"
#include "vk_shaders_spv.h"
#include "vk_platform.h"
#include "gl_packet.h"   // 仅为了 packet 执行路径的开关查询（ExecActive/SnapshotFrameGen）
#include "vk_packet.h"   // 取证转储要用 Sha256（同一实现，避免再写一份）
#include "vk_tuning.h"   // 运行时调参（vk_tuning.ini），默认值 === 加模块前的写死值
#define VK_NO_PROTOTYPES
#include "gl_shader.h"
#include "gl_state.h"
#include <vulkan/vulkan.h>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <chrono>
#if defined(_WIN32)
#include <direct.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#endif

// 这两个符号由 gl_core.cpp 提供，与平台无关（Android 也要用），不能放在 _WIN32 里。
// 诊断用：当前 GL 混合状态读取（只给 VK_ALPHALOG 用）。
extern "C" void lime_vk_get_blend_state(int* en, int* src, int* dst);
// 纹理真正销毁时通知 gl_core 清掉所有指向该 id 的 GL 名字（避免槽位复用串图）。
extern "C" void gl_vk_on_texture_destroyed(uint32_t vkId);

// gl_fbo 侧的当前目标（只读探针用，定义在 lime 命名空间的 gl_fbo.cpp:706）
namespace lime { uint32_t gl_vk_current_target (); }
namespace lime { extern int g_vkfixDrawTotal; extern int g_vkfixDrawFolded; }

// 接线探针（本轮只接不选）：gl_shader 自己建的管线。它不参与任何 draw，
// 只在 render pass 开头绑一次、紧接着换回固定管线（两次绑定之间没有 draw）。
static VkPipeline g_probePipeline = VK_NULL_HANDLE;
static int g_probeProgram = 0;

// 取证探针（只记状态，不改行为）：后缓冲帧是否已 open 但还没 close。
// 用来把"合成 quad 落进一个不会 EndFrame 的帧"与"BindRenderTarget(0) 失败"分开。
static int g_bbOpen = 0;
// 按 program 累计的绘制/整屏/顶点数（只计数，不受逐条日志上限影响）
static uint64_t g_progDraw[256];
static uint64_t g_progWhole[256];
static uint64_t g_progVerts[256];
static uint64_t g_progDrawTotal = 0;

#define MAX_SPRITES 4096
#define MAX_TEXTURES 512
// packet 路径采样器变体 descriptor set 的上限（纹理 × 采样器组合的缓存容量）
#define PKT_SET_MAX 512
// 帧环槽位数：每个槽一套命令缓冲 + fence + 顶点区 + UBO 区。
// 提交后不再 vkQueueWaitIdle，CPU 录制与 GPU 执行流水起来；槽位复用时才等自己的 fence。
#define VK_RING 3

// 交换链 extent：安卓常见 ROTATE_90/270，此时 imageExtent 是旋转前的尺寸，
// 宽高必须和窗口尺寸交换，否则整幅被拉伸。桌面 currentTransform 恒为 IDENTITY，行为不变。
static VkExtent2D PickSwapchainExtent (const VkSurfaceCapabilitiesKHR& caps, int fbWidth, int fbHeight) {
    VkExtent2D e = caps.currentExtent;
    if (e.width == 0xFFFFFFFFu) {
        e.width = (uint32_t)fbWidth;
        e.height = (uint32_t)fbHeight;
    }
    if (caps.currentTransform & (VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR |
                                 VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR)) {
        std::swap (e.width, e.height);
    }
    return e;
}

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
    // 逐顶点对应的 GL program id（与 verts 一一对应，每个顶点一条）：
    // 批处理按纹理合并顶点，不同 program 的笔不能共用一条管线，靠它拆段。
    std::vector<uint16_t> progs;
    // 取证：创建时 CPU 数据按 stride=97 采样的 mean/nb（与 [vk] texmean 同口径），
    // 供转储自校验；updates>0 时基线已失效（texSubImage2D 改过内容）
    float createMean = 0.0f;
    float createNb = 0.0f;
    bool hasCreateMean = false;
    uint32_t updates = 0;
};

struct RenderTarget {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE;
    VkRenderPass rp = VK_NULL_HANDLE;
    // offscreen_clear=loadop 专用：loadOp=CLEAR 的第二套 pass/fb（默认 image 路径不用）
    VkFramebuffer fbClear = VK_NULL_HANDLE;
    VkRenderPass rpClear = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    int w = 0, h = 0;
    bool used = false;
    std::vector<float> verts;
    // 逐顶点对应的 GL program id（与 verts 一一对应，每个顶点一条）：
    // 批处理按纹理合并顶点，不同 program 的笔不能共用一条管线，靠它拆段。
    std::vector<uint16_t> progs;
    // 取证用：创建帧号 / 最后一次"真的有顶点"的提交帧号（0 = 从未写过）
    uint64_t createdFrame = 0;
    uint64_t lastWriteFrame = 0;
    // 取证用：最近一次"clear 真的执行"的帧号 + 独立有效标志（别拿 0 当哨兵）
    uint64_t clearedFrame = 0;
    bool cleared = false;
    // 记录图像"真实"当前布局：创建=UNDEFINED，pass 结束=SRO，清除后=SRO。
    // 清除屏障的 oldLayout 必须按这个记录给，不能再写死（写死 SRO 对上 UNDEFINED 会崩 nvoglv64）。
    // 批处理路径的"游戏要求清屏"：gl_vk_handle_clear 打标，EndFrame 改用 rpClear 并按此色清。
    bool clearPending = false;
    float pcR = 0, pcG = 0, pcB = 0, pcA = 0;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

// 取证用：当前挂着但未提交的顶点总数（顶点 = 15 floats）
static size_t PendingVertsOf (const std::vector<TexSlot>& tex, const std::vector<RenderTarget>& tgt) {
    size_t n = 0;
    for (auto& t : tex) n += t.verts.size() / 15;
    for (auto& t : tgt) n += t.verts.size() / 15;
    return n;
}

// ---- 轻量性能统计（VK_PERF=0 关闭；默认每秒一行 [perf]，只走 stderr，不写盘）----
// 统计口径：一次 BeginFrame = 一个视觉帧；一次 EndFrame = 一次提交（可能一帧多次，
// 每个离屏目标一次）。draws/pipes/ds/rp 都是"每次提交"累计值，输出时除以视觉帧数。
struct PerfCounters {
    uint64_t frames = 0;      // BeginFrame 次数（只在后缓冲 glClear 时发生，可能为 0）
    uint64_t presents = 0;    // 真正 vkQueuePresentKHR 的次数 = 视觉帧数
    uint64_t submits = 0;     // EndFrame 里真正 vkQueueSubmit 的次数
    uint64_t rpBegins = 0;    // vkCmdBeginRenderPass 次数
    uint64_t draws = 0;       // vkCmdDraw 次数
    uint64_t pipeBinds = 0;   // vkCmdBindPipeline 次数
    uint64_t dsBinds = 0;     // vkCmdBindDescriptorSets 次数
    uint64_t verts = 0;       // 提交的顶点数
    double endMs = 0;         // EndFrame 总耗时（录制+提交+waitIdle）
    double waitMs = 0;        // 其中 vkQueueWaitIdle 的耗时
    double maxEndMs = 0;
    double lastBeginMs = 0;   // 上一帧 BeginFrame 的时间戳
    double frameMs = 0;       // 相邻 BeginFrame 的墙钟间隔累计
    double lastPrintMs = 0;
};
static PerfCounters g_perf;
static double PerfNowMs () {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}
static bool PerfOn () {
    static int s = -1;
    if (s < 0) { const char* v = getenv("VK_PERF"); s = (v && v[0] == '0') ? 0 : 1; }
    return s != 0;
}
// 逐帧取证日志（[vk] submit / bbframe / frame= / v6 / [vkfix] 等）默认关闭，
// VK_DIAG=1 打开；性能测试请保持关闭，避免每帧多次 stderr 写入干扰测量。
static bool DiagOn () {
    static int s = -1;
    if (s < 0) { const char* v = getenv("VK_DIAG"); s = (v && v[0] == '1') ? 1 : 0; }
    return s != 0;
}
static void PerfFrameBegin () {
    if (!PerfOn()) return;
    double now = PerfNowMs();
    if (g_perf.lastBeginMs > 0) g_perf.frameMs += now - g_perf.lastBeginMs;
    g_perf.lastBeginMs = now;
    g_perf.frames++;
}
static void PerfFlushIfDue () {
    if (!PerfOn()) return;
    double now = PerfNowMs();
    if (g_perf.lastPrintMs <= 0) { g_perf.lastPrintMs = now; return; }
    double span = now - g_perf.lastPrintMs;
    uint64_t fpsFrames = g_perf.presents ? g_perf.presents : g_perf.frames;
    if (span < 1000.0 || fpsFrames == 0) return;
    double f = (double)fpsFrames;
    fprintf(stderr,
            "[perf] fps=%.1f frame=%.2fms end=%.2fms(max %.2f) wait=%.2fms | "
            "draws/f=%.1f pipes/f=%.1f ds/f=%.1f rp/f=%.1f submits/f=%.1f verts/f=%.0f\n",
            f * 1000.0 / span,
            g_perf.frameMs / f, g_perf.endMs / f, g_perf.maxEndMs,
            g_perf.waitMs / f,
            (double)g_perf.draws / f, (double)g_perf.pipeBinds / f,
            (double)g_perf.dsBinds / f, (double)g_perf.rpBegins / f,
            (double)g_perf.submits / f, (double)g_perf.verts / f);
    g_perf.frames = g_perf.presents = g_perf.submits = g_perf.rpBegins = 0;
    g_perf.draws = g_perf.pipeBinds = g_perf.dsBinds = g_perf.verts = 0;
    g_perf.endMs = g_perf.waitMs = g_perf.frameMs = 0;
    g_perf.maxEndMs = 0;
    g_perf.lastPrintMs = now;
}

struct VkRenderer::Impl {
    void* nativeWindow = nullptr;
    int width = 0, height = 0;
    void* vkLib = nullptr;

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
    VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D extent{};

    VkRenderPass renderPass = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers;

    VkCommandPool cmdPool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    // 帧环：每个槽一套命令缓冲 + fence（提交后不 waitIdle，槽位复用时才等自己的 fence）
    VkCommandBuffer cmds[VK_RING] = {};
    VkFence fences[VK_RING] = {};
    bool fencePending[VK_RING] = {false, false, false};
    uint32_t slot = 0;
    // 该槽延迟销毁的纹理：本槽提交完成（fence 触发）后才能真正释放
    std::vector<uint32_t> slotDestroy[VK_RING];

    // Phase 0 取证（VK_PASSSTAT=1，默认关）：每槽 2 个 timestamp query 夹住本笔 pass，
    // 槽位 fence 触发后再读回，所以测量本身不引入等待。默认全零 = 零开销。
    bool passStat = false;
    double tsPeriodNs = 1.0;      // 每 tick 的纳秒数（VkPhysicalDeviceLimits.timestampPeriod）
    VkQueryPool passQueries[VK_RING] = {};
    bool pendValid[VK_RING] = {};
    uint64_t pendFrame[VK_RING] = {};
    uint32_t pendTarget[VK_RING] = {};
    uint32_t pendExtW[VK_RING] = {}, pendExtH[VK_RING] = {};
    uint32_t pendRanges[VK_RING] = {};
    uint32_t pendVerts[VK_RING] = {};
    uint8_t pendLoad[VK_RING] = {};   // 0=LOAD 1=CLEARP 2=CLEAR0 3=BB
    uint32_t pendProgMask[VK_RING] = {};  // 本笔用到的自定义 program 位掩码（prog < 32）

    // 雪花取证（VK_SNOWWATCH=1）：每帧回读后缓冲算高频噪声分，超过阈值就自动转储
    // 当帧所有被采样纹理/目标（配合 VK_DUMP_TEX=<dir>），把"整屏白噪声"钉到具体来源。
    bool snowWatch = false;
    bool snowDumpReq = false;

    VkShaderModule vsMod = VK_NULL_HANDLE, fsMod = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;

    VkBuffer vb = VK_NULL_HANDLE;
    VkDeviceMemory vbMem = VK_NULL_HANDLE;
    float* vbPtr = nullptr;
    size_t vbBytes = 0;            // 单槽顶点区字节数（缓冲总量 = vbBytes * VK_RING）
    VkDeviceSize vbSlotOff = 0;    // 当前槽在顶点缓冲里的字节偏移
    // 常量顶点属性：loc5(alpha 乘数)/loc6(colorMultiplier)/loc7(colorOffset)。
    // 用 stride=0 的绑定给所有顶点同一组值；loc1 里已经携带逐顶点 alpha。
    VkBuffer attrBuf = VK_NULL_HANDLE;
    VkDeviceMemory attrMem = VK_NULL_HANDLE;
    float* attrPtr = nullptr;

    VkBuffer ubo = VK_NULL_HANDLE;
    VkDeviceMemory uboMem = VK_NULL_HANDLE;
    float* uboPtr = nullptr;       // 当前槽的 UBO 区（随槽位切换）
    float* uboBase = nullptr;      // 整块映射起点
    uint32_t uboStride = 256;      // 每槽步长（>= minUniformBufferOffsetAlignment）

    std::vector<TexSlot> textures;
    // 延迟销毁队列：glTexImage2D 同名重建时先排队，等本帧命令缓冲提交完再真正释放。
    // 若立即释放，槽位会被下一笔 CreateTexture 复用，本帧早先记录的绘制段就会
    // 采样到新纹理（症状：整层贴图错乱、菜单多出亮层）。
    std::vector<uint32_t> pendingTexDestroy;
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
    // 交换链脏标记：present/acquire 返回 OUT_OF_DATE 或 SUBOPTIMAL 后置位，
    // 下一次开后缓冲帧前无条件重建（原来全仓库没有重建分支 → 后缓冲永久开不起来）
    bool swapDirty = false;
    // resize 节流：窗口尺寸与交换链不一致的起始时刻（GetTickCount64 毫秒）。
    // 动画期间持续 present 旧尺寸图但不重建交换链，稳定 200ms 后才重建。
    uint64_t sizeMismatchSince = 0;
    int lastWinW = 0;
    int lastWinH = 0;
    // 最近一次观察到窗口尺寸变化的时刻（GetTickCount64）。400ms 内视为“缩放动画中”，
    // 期间任何 OUT_OF_DATE / SUBOPTIMAL 都不重建交换链，否则每帧 waitIdle 会把
    // lime 的窗口缩放动画拖成几大跳（实测 922..935 后一下子 1280）。
    uint64_t lastResizeTick = 0;
    // 缩放动画期间交换链重建的限频计时（GetTickCount64 毫秒）。
    // Win32 surface 只允许交换链=当前窗口尺寸，动画每一步都会 OUT_OF_DATE；
    // 每帧重建太慢（一秒钟 24 次，帧率 244→69），这里限制为约 10 次/秒。
    uint64_t lastCoverTick = 0;
    // 取证：present 前把交换链图拷到主机可见缓冲，算亮度统计（VK_BBSTAT=1，默认关）
    VkBuffer statBuf = VK_NULL_HANDLE;
    VkDeviceMemory statMem = VK_NULL_HANDLE;
    void* statPtr = nullptr;
    size_t statBytes = 0;
    bool statWanted = false;   // VK_BBSTAT / VK_DUMP_PRESENTS：每帧回读
    // 色键透明（VK_COLORKEY，默认开）：游戏用 SetLayeredWindowAttributes 的色键，
    // 在 DXGI flip 模型下不生效；这里把“色键色”区域从窗口裁掉（SetWindowRgn），
    // 窗口外即桌面，等效原版 GDI 色键。ckR/G/B 是目标颜色（默认 RGB 87,25,0 = 6487）。
    bool ckEnabled = false;
    int ckR = 87, ckG = 25, ckB = 0;
    uint64_t ckLastTick = 0;
    bool ckActive = false;
    std::vector<RenderTarget> targets;
    // 批处理提交顺序：GL 里每笔绘制是立即执行的，而顶点按纹理分桶收集后
    // 若按纹理 id 升序提交，后画的整屏层会被压到先画的层下面（主菜单的
    // 静态噪声层就是这么被背景盖掉的）。这里按 GL 提交顺序记录每一笔的
    // 源 id / 该源顶点数组里的起始顶点 / 顶点数，EndFrame 严格照此顺序提交。
    // uni：绘制时刻该 program 的 uniform 快照（可为空）。
    struct FrameSeg {
        uint32_t id;
        size_t off;
        size_t count;
        std::vector<uint8_t> uni;
    };
    std::vector<FrameSeg> segs;
    uint32_t currentTarget = 0;
    // 最后一次真正收到顶点并提交的离屏目标（present 选源用）
    uint32_t lastGeomTarget = 0;
    // 取证转储：整帧（从开跑到目标帧收尾）被采样过的纹理/目标 id（去重，只增不清）
    std::vector<uint32_t> sampled;
    bool dumpDone = false;
    uint32_t frameImageIndex = 0;
    bool frameOpen = false;
    // ---- packet 执行路径（VK_PKT_EXEC=1）：独立命令流，不碰批处理的 verts 列表 ---- 
    bool pktOpen = false;
    uint32_t pktTarget = 0;
    uint64_t pktDraws = 0;
    VkBuffer pktVb = VK_NULL_HANDLE;
    VkDeviceMemory pktVbMem = VK_NULL_HANDLE;
    uint8_t* pktVbPtr = nullptr;
    VkDeviceSize pktVbBytes = 0;
    VkDeviceSize pktVbOff = 0;
    VkBuffer pktIb = VK_NULL_HANDLE;
    VkDeviceMemory pktIbMem = VK_NULL_HANDLE;
    uint8_t* pktIbPtr = nullptr;
    VkDeviceSize pktIbBytes = 0;
    VkDeviceSize pktIbOff = 0;
    // ---- P2：按 packet 状态选管线变体 / 按 (纹理,采样器组合) 选 descriptor set ----
    // 变体缓存：键是压缩后的状态位，值是该状态下建好的管线；默认状态走 o.pipeline
    struct PktPipeEntry {
        uint32_t key;
        VkPipeline pipe;
    };
    std::vector<PktPipeEntry> pktPipes;
    // 采样器缓存：键是 (min|mag|wrapU|wrapV) 压缩值
    struct PktSamplerEntry {
        uint16_t key;
        VkSampler smp;
    };
    std::vector<PktSamplerEntry> pktSamplers;
    // descriptor set 缓存：同一纹理在不同采样器组合下各有一份
    struct PktSetEntry {
        uint32_t texId;
        uint16_t key;
        VkDescriptorSet set;
    };
    std::vector<PktSetEntry> pktSets;
    // 变体专用 descriptor pool：不与批处理共用，避免挤掉原有 2*MAX_TEXTURES 配额
    VkDescriptorPool pktSetPool = VK_NULL_HANDLE;
    uint32_t pktPipeCount = 0;
    uint32_t pktSamplerCount = 0;
    uint32_t pktSetCount = 0;
    // 当前打开的 packet target 尺寸：逐笔 viewport/scissor 的 y 翻转要用
    VkExtent2D pktTargetExt = {0, 0};
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
static PFN_vkFreeDescriptorSets g_freeDS;
static PFN_vkUpdateDescriptorSets g_updateDS;
static PFN_vkCmdPipelineBarrier g_cmdBarrier;
static PFN_vkCmdClearColorImage g_cmdClearColorImage;
static PFN_vkCmdCopyBufferToImage g_cmdCopyImg;
static PFN_vkCmdCopyImageToBuffer g_cmdCopyImgToBuf;
static PFN_vkCmdBindPipeline g_bindPipe;
static PFN_vkCmdSetViewport g_setViewport;
static PFN_vkCmdSetScissor g_setScissor;
static PFN_vkCmdBindDescriptorSets g_bindDS;
static PFN_vkCmdBindVertexBuffers g_bindVB;
static PFN_vkCmdBindIndexBuffer g_bindIB;
static PFN_vkCmdDraw g_cmdDraw;
static PFN_vkCmdDrawIndexed g_cmdDrawIndexed;
static PFN_vkCmdBeginRenderPass g_beginRP;
static PFN_vkCmdEndRenderPass g_endRP;
static PFN_vkBeginCommandBuffer g_beginCB;
static PFN_vkEndCommandBuffer g_endCB;
static PFN_vkResetCommandBuffer g_resetCB;
static PFN_vkQueueSubmit g_submit;
static PFN_vkQueueWaitIdle g_waitIdle;
static PFN_vkCreateFence g_createFence;
static PFN_vkWaitForFences g_waitFences;
static PFN_vkResetFences g_resetFences;
static PFN_vkDestroyFence g_destroyFence;
// Phase 0：pass GPU 时间戳（VK_PASSSTAT=1 才用）
static PFN_vkCreateQueryPool g_createQP;
static PFN_vkDestroyQueryPool g_destroyQP;
static PFN_vkCmdResetQueryPool g_resetQP;
static PFN_vkCmdWriteTimestamp g_writeTS;
static PFN_vkGetQueryPoolResults g_getQP;
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

// 平台无关：路径解析在 vk_platform_win32.cpp / vk_platform_android.cpp 里。
// 本文件（以及所有 COMMON_SOURCES）不得出现任何 Win32 符号。
static std::string ExeDirLocal() {
    char dir[1024];
    vkplat::ExeDir(dir, (int)sizeof(dir));
    return std::string(dir);
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

// 设备挑选只表达偏好，不做硬性排除：安卓平板往往只有集成 GPU，
// 硬挑 DISCRETE 会让安卓直接起不来。分数只用于挑"更好的那块"，不是门槛。
static int DeviceTypeScore (VkPhysicalDeviceType t) {

    switch (t) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 3;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 2;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 1;
        default: return 0;
    }

}

// 日志里要能一眼看出跑在哪块卡上，所以用文字而不是数字枚举。
static const char* DeviceTypeName (VkPhysicalDeviceType t) {

    switch (t) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete";
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual";
        case VK_PHYSICAL_DEVICE_TYPE_CPU: return "cpu";
        default: return "other";
    }

}

VkRenderer::VkRenderer() : impl(new Impl()) {}

VkRenderer::~VkRenderer() {
    Shutdown();
    delete impl;
}

const char* VkRenderer::LastError() const { return impl->lastError; }

bool VkRenderer::Init(void* nativeWindow, int width, int height) {
    Impl& o = *impl;
    o.nativeWindow = nativeWindow;
    o.width = width;
    o.height = height;

    // 调参文件在设备/纹理初始化之前读进来，并立刻打一行生效值：
    // 之后所有 [sampler] / [pkt] 行都能和这一行对照，排除“以为改了但没生效”。
    vktuning::Load();
    vktuning::LogEffective();

    char err[160] = {0};
    o.vkLib = vkplat::LoadVulkan(err, (int)sizeof(err));
    if (!o.vkLib) {
        fprintf(stderr, "[vkerr] vk_load %s\n", err);
        o.lastError = "vulkan loader missing";
        return false;
    }
    o.gipa = (PFN_vkGetInstanceProcAddr)vkplat::Proc(o.vkLib, "vkGetInstanceProcAddr");
    auto createInstance = (PFN_vkCreateInstance)vkplat::Proc(o.vkLib, "vkCreateInstance");
    if (!o.gipa || !createInstance) {
        fprintf(stderr, "[vkerr] vk_entrypoints gipa=%d createInstance=%d\n",
                o.gipa != nullptr, createInstance != nullptr);
        o.lastError = "vulkan entrypoints missing";
        return false;
    }
    g_gipa = o.gipa;

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Lime Vulkan";
    app.apiVersion = VK_API_VERSION_1_0;

    uint32_t extCount = 0;
    const char* const* exts = vkplat::InstanceExtensions(&extCount);
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = extCount;
    ici.ppEnabledExtensionNames = exts;

    if (createInstance(&ici, nullptr, &o.inst) != VK_SUCCESS) { o.lastError = "instance failed"; return false; }

    auto enumDev = (PFN_vkEnumeratePhysicalDevices)o.gipa(o.inst, "vkEnumeratePhysicalDevices");
    auto getProps = (PFN_vkGetPhysicalDeviceProperties)o.gipa(o.inst, "vkGetPhysicalDeviceProperties");
    auto getMemProps = (PFN_vkGetPhysicalDeviceMemoryProperties)o.gipa(o.inst, "vkGetPhysicalDeviceMemoryProperties");
    auto getQFP = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)o.gipa(o.inst, "vkGetPhysicalDeviceQueueFamilyProperties");
    auto getSurfSupport = (PFN_vkGetPhysicalDeviceSurfaceSupportKHR)o.gipa(o.inst, "vkGetPhysicalDeviceSurfaceSupportKHR");
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
    g_freeDS = (PFN_vkFreeDescriptorSets)o.gipa(o.inst, "vkFreeDescriptorSets");
    g_updateDS = (PFN_vkUpdateDescriptorSets)o.gipa(o.inst, "vkUpdateDescriptorSets");
    g_cmdBarrier = (PFN_vkCmdPipelineBarrier)o.gipa(o.inst, "vkCmdPipelineBarrier");
    g_cmdClearColorImage = (PFN_vkCmdClearColorImage)o.gipa(o.inst, "vkCmdClearColorImage");
    g_cmdCopyImg = (PFN_vkCmdCopyBufferToImage)o.gipa(o.inst, "vkCmdCopyBufferToImage");
    g_cmdCopyImgToBuf = (PFN_vkCmdCopyImageToBuffer)o.gipa(o.inst, "vkCmdCopyImageToBuffer");
    g_bindPipe = (PFN_vkCmdBindPipeline)o.gipa(o.inst, "vkCmdBindPipeline");
    g_setViewport = (PFN_vkCmdSetViewport)o.gipa(o.inst, "vkCmdSetViewport");
    g_setScissor = (PFN_vkCmdSetScissor)o.gipa(o.inst, "vkCmdSetScissor");
    g_bindDS = (PFN_vkCmdBindDescriptorSets)o.gipa(o.inst, "vkCmdBindDescriptorSets");
    g_bindVB = (PFN_vkCmdBindVertexBuffers)o.gipa(o.inst, "vkCmdBindVertexBuffers");
    g_bindIB = (PFN_vkCmdBindIndexBuffer)o.gipa(o.inst, "vkCmdBindIndexBuffer");
    g_cmdDraw = (PFN_vkCmdDraw)o.gipa(o.inst, "vkCmdDraw");
    g_cmdDrawIndexed = (PFN_vkCmdDrawIndexed)o.gipa(o.inst, "vkCmdDrawIndexed");
    g_beginRP = (PFN_vkCmdBeginRenderPass)o.gipa(o.inst, "vkCmdBeginRenderPass");
    g_endRP = (PFN_vkCmdEndRenderPass)o.gipa(o.inst, "vkCmdEndRenderPass");
    g_beginCB = (PFN_vkBeginCommandBuffer)o.gipa(o.inst, "vkBeginCommandBuffer");
    g_endCB = (PFN_vkEndCommandBuffer)o.gipa(o.inst, "vkEndCommandBuffer");
    g_resetCB = (PFN_vkResetCommandBuffer)o.gipa(o.inst, "vkResetCommandBuffer");
    g_submit = (PFN_vkQueueSubmit)o.gipa(o.inst, "vkQueueSubmit");
    g_waitIdle = (PFN_vkQueueWaitIdle)o.gipa(o.inst, "vkQueueWaitIdle");
    g_createFence = (PFN_vkCreateFence)o.gipa(o.inst, "vkCreateFence");
    g_waitFences = (PFN_vkWaitForFences)o.gipa(o.inst, "vkWaitForFences");
    g_resetFences = (PFN_vkResetFences)o.gipa(o.inst, "vkResetFences");
    g_destroyFence = (PFN_vkDestroyFence)o.gipa(o.inst, "vkDestroyFence");
    g_createQP = (PFN_vkCreateQueryPool)o.gipa(o.inst, "vkCreateQueryPool");
    g_destroyQP = (PFN_vkDestroyQueryPool)o.gipa(o.inst, "vkDestroyQueryPool");
    g_resetQP = (PFN_vkCmdResetQueryPool)o.gipa(o.inst, "vkCmdResetQueryPool");
    g_writeTS = (PFN_vkCmdWriteTimestamp)o.gipa(o.inst, "vkCmdWriteTimestamp");
    g_getQP = (PFN_vkGetQueryPoolResults)o.gipa(o.inst, "vkGetQueryPoolResults");
    g_acquire = (PFN_vkAcquireNextImageKHR)o.gipa(o.inst, "vkAcquireNextImageKHR");
    g_present = (PFN_vkQueuePresentKHR)o.gipa(o.inst, "vkQueuePresentKHR");
    g_createShader = (PFN_vkCreateShaderModule)o.gipa(o.inst, "vkCreateShaderModule");
    g_createPL = (PFN_vkCreatePipelineLayout)o.gipa(o.inst, "vkCreatePipelineLayout");
    g_createPipe = (PFN_vkCreateGraphicsPipelines)o.gipa(o.inst, "vkCreateGraphicsPipelines");

    if (!enumDev || !getProps || !getMemProps || !getQFP || !getSurfSupport ||
        !createDevice || !createSwap || !getImages || !createRP || !createFB || !createPool || !allocCB) {
        o.lastError = "missing vulkan functions";
        return false;
    }

    // 先把 surface 建出来：判断一块设备"能不能用"的硬条件是"有 graphics 队列且能
    // present 到这块 surface"。旧实现先选设备后建 surface，没有独显时只能盲退 devs[0]。
    uint64_t surf = 0;
    if (!vkplat::CreateSurface((uint64_t)o.inst, (void*)o.gipa, nativeWindow, &surf, err, (int)sizeof(err))) {
        fprintf(stderr, "[vkerr] surface %s\n", err);
        o.lastError = "surface failed";
        return false;
    }
    o.surf = (VkSurfaceKHR)surf;

    VkPhysicalDeviceMemoryProperties memProps{};
    uint32_t n = 0;
    enumDev(o.inst, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    if (n > 0) enumDev(o.inst, &n, devs.data());
    if (n == 0) {
        fprintf(stderr, "[vkerr] no physical device\n");
        o.lastError = "no devices";
        return false;
    }

    // 独显优先，没有独显就退到集显/虚拟/其它；只要求它能 graphics+present。
    VkPhysicalDevice best = nullptr;
    int bestScore = -1;
    int bestQueue = -1;
    for (auto& d : devs) {
        uint32_t qn = 0;
        getQFP(d, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qn);
        if (qn > 0) getQFP(d, &qn, qs.data());
        int qi = -1;
        for (uint32_t i = 0; i < qn; i++) {
            VkBool32 ok = VK_FALSE;
            getSurfSupport(d, i, o.surf, &ok);
            if ((qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && ok) { qi = (int)i; break; }
        }
        if (qi < 0) continue;
        VkPhysicalDeviceProperties pp{};
        getProps(d, &pp);
        int score = DeviceTypeScore(pp.deviceType);
        if (score > bestScore) { bestScore = score; best = d; bestQueue = qi; }
    }
    if (!best) {
        fprintf(stderr, "[vkerr] no usable physical device (graphics+present unavailable)\n");
        o.lastError = "no graphics+present device";
        return false;
    }
    o.phys = best;
    o.qidx = (uint32_t)bestQueue;
    getMemProps(o.phys, &memProps);
    {
        VkPhysicalDeviceProperties sp{};
        getProps(o.phys, &sp);
        fprintf(stderr, "[vk] device=%s type=%s api=%u.%u.%u\n", sp.deviceName, DeviceTypeName(sp.deviceType),
                VK_VERSION_MAJOR(sp.apiVersion), VK_VERSION_MINOR(sp.apiVersion), VK_VERSION_PATCH(sp.apiVersion));
        // Phase 0：timestampPeriod 单位 ns/tick；0 视为不可用，退回 1 只保证不除零
        o.tsPeriodNs = sp.limits.timestampPeriod > 0.0f ? (double)sp.limits.timestampPeriod : 1.0;
        const char* ps = getenv("VK_PASSSTAT");
        o.passStat = (ps && ps[0] == '1') && g_createQP && g_resetQP && g_writeTS && g_getQP;
        if (ps && ps[0] == '1')
            fprintf(stderr, "[vk] passstat on=%d period=%.3fns bits64=1\n",
                    o.passStat ? 1 : 0, o.tsPeriodNs);
    }

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
    o.colorSpace = fmt.colorSpace;

    o.extent = PickSwapchainExtent (caps, width, height);
    fprintf(stderr, "[vk] swapchain extent=%ux%u preTransform=0x%x\n",
            o.extent.width, o.extent.height, (unsigned)caps.currentTransform);

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
    // 呈现模式：原版游戏初始化时调 wglSwapIntervalEXT(0)（apitrace 第 1651 行），要的是不锁帧。
    // 固定 FIFO 会把帧率钉在 60Hz，和游戏实际帧时间对不上就出现"一抖一抖"的规律卡顿。
    // 优先 MAILBOX（不锁帧、不撕裂），不支持回退 FIFO；VK_PRESENT_MODE=fifo 可强制回旧行为。
    swci.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
    {
        auto getPresentModes = (PFN_vkGetPhysicalDeviceSurfacePresentModesKHR)
                o.gipa(o.inst, "vkGetPhysicalDeviceSurfacePresentModesKHR");
        uint32_t pmc = 0;
        bool hasMailbox = false;
        if (getPresentModes) {
            getPresentModes(o.phys, o.surf, &pmc, nullptr);
            std::vector<VkPresentModeKHR> pms(pmc);
            if (pmc) getPresentModes(o.phys, o.surf, &pmc, pms.data());
            for (uint32_t i = 0; i < pmc; i++) if (pms[i] == VK_PRESENT_MODE_MAILBOX_KHR) hasMailbox = true;
        }
        const char* want = getenv("VK_PRESENT_MODE");
        if ((want && want[0] == 'f') || !hasMailbox) swci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        fprintf(stderr, "[vk] presentMode=%s (mailbox_available=%d)\n",
                swci.presentMode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : "fifo",
                hasMailbox ? 1 : 0);
    }
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

    // 单次上屏（第十四轮采纳为默认）：同一张交换链图在一个视觉帧内多次合成，
    // 第二次起用 LOAD pass 叠加上一层内容，最后只在真实 swap 时 present 一次。
    // 关键：中间合成结束在 COLOR_ATTACHMENT_OPTIMAL（不以 PRESENT_SRC 作 initialLayout）。
    // 标题页一个视觉帧会发两次 present（FBO 切换 + ContextFlip），两次各只铺一层，
    // 不定这个开关就会在 1001/1002 之间逐帧交替 → 闪烁。显式设 VK_SINGLE_PRESENT=0 可退回旧行为。
    {
        const char* v = getenv("VK_SINGLE_PRESENT");
        if (!(v && v[0] == '0')) {
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
    // 色键透明（VK_COLORKEY，默认关）：游戏用 SetLayeredWindowAttributes 的色键，
    // 我们走 DXGI flip 不支持 layered，只能回读后缓冲+SetWindowRgn 模拟——开销大且《Too Late》
    // 全屏段会明显卡。默认关=还原原版以外的行为（该区域显示暗红色块）；需要时 VK_COLORKEY=1 打开。
    {
        const char* ck = getenv("VK_COLORKEY");
        o.ckEnabled = (ck && ck[0] == '1');
        const char* ckrgb = getenv("VK_COLORKEY_RGB");
        int r = 0, g = 0, b = 0;
        if (ckrgb && sscanf(ckrgb, "%d,%d,%d", &r, &g, &b) == 3) {
            o.ckR = r; o.ckG = g; o.ckB = b;
        }
    }
    {
        const char* v = getenv("VK_BBSTAT");
        const char* d = getenv("VK_DUMP_PRESENTS");
        const char* sn = getenv("VK_SNOWWATCH");
        o.snowWatch = (sn && sn[0] == '1');
        o.statWanted = (v && v[0] == '1') || (d && d[0] == '1') || o.snowWatch;
        if (o.statWanted || o.ckEnabled) {
            // 色键扫描要覆盖最大窗口（全屏段 1920x1080），按屏幕尺寸分配。
            size_t needW = (size_t)o.extent.width;
            size_t needH = (size_t)o.extent.height;
            int sw = 0, sh = 0;
            vkplat::ScreenSize(&sw, &sh);
            if (sw > 0 && (size_t)sw > needW) needW = (size_t)sw;
            if (sh > 0 && (size_t)sh > needH) needH = (size_t)sh;
            o.statBytes = needW * needH * 4;
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
    cbi.commandBufferCount = VK_RING;
    allocCB(o.dev, &cbi, o.cmds);
    o.cmd = o.cmds[0];
    // 每个槽一个 fence：初始未触发（还没有任何提交），AcquireSlot 里只有"挂着提交"时才等
    for (uint32_t i = 0; i < VK_RING; i++) {
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (!g_createFence || g_createFence(o.dev, &fci, nullptr, &o.fences[i]) != VK_SUCCESS)
            fprintf(stderr, "[vk] ring fence create failed slot=%u\n", i);
    }
    // Phase 0：每槽一个 2-query 时间戳池（pass 前/后各写一次）
    if (o.passStat) {
        for (uint32_t i = 0; i < VK_RING; i++) {
            VkQueryPoolCreateInfo qpi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qpi.queryCount = 2;
            if (g_createQP(o.dev, &qpi, nullptr, &o.passQueries[i]) != VK_SUCCESS) {
                fprintf(stderr, "[vk] passstat query pool create failed slot=%u\n", i);
                o.passQueries[i] = VK_NULL_HANDLE;
            }
        }
    }
    fprintf(stderr, "[vk] ring slots=%d vbSlotBytes=%zu uboStride=%u\n",
            VK_RING, (size_t)MAX_SPRITES * 6 * 15 * sizeof(float), o.uboStride);

    // descriptor layout 按实际 SPIR-V：binding0 = UBO（动态偏移，按帧环槽位），binding1 = 贴图采样器
    VkDescriptorSetLayoutBinding dslb[2]{};
    dslb[0].binding = 0;
    dslb[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
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
    dps[0].descriptorCount = MAX_TEXTURES * 8;
    dps[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    dps[1].descriptorCount = MAX_TEXTURES * 8;
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    // FREE 位：纹理销毁/重建时要把它占用的 set 还回池里。没有这个位就只能泄漏，
    // 文本每秒重传一次纹理都能把 1024 个 set 吃光，之后 Allocate 失败拿到空句柄，
    // 下一次绑定/更新描述符直接崩在驱动里（本项就是 §26.12 的“~90 秒崩”）。
    dpci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dpci.maxSets = MAX_TEXTURES * 8;
    dpci.poolSizeCount = 2;
    dpci.pPoolSizes = dps;
    g_createDP(o.dev, &dpci, nullptr, &o.descPool);

    // packet 路径的采样器变体专用池：独立配额，不挤占批处理的 2*MAX_TEXTURES。
    // 带 FREE 位是为了纹理/目标销毁时能把手里的变体 set 还回来（避免悬空引用）。
    {
        VkDescriptorPoolSize vps[2]{};
        vps[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        vps[0].descriptorCount = PKT_SET_MAX;
        vps[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        vps[1].descriptorCount = PKT_SET_MAX;
        VkDescriptorPoolCreateInfo vpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        vpci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        vpci.maxSets = PKT_SET_MAX;
        vpci.poolSizeCount = 2;
        vpci.pPoolSizes = vps;
        if (g_createDP(o.dev, &vpci, nullptr, &o.pktSetPool) != VK_SUCCESS) {
            o.pktSetPool = VK_NULL_HANDLE;
            fprintf(stderr, "[sampler] warn=pkt_set_pool_failed\n");
        }
    }

    VkSamplerCreateInfo smpci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    smpci.magFilter = VK_FILTER_NEAREST;
    smpci.minFilter = VK_FILTER_NEAREST;
    smpci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    smpci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    smpci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    smpci.maxLod = 1.0f;
    g_createSampler(o.dev, &smpci, nullptr, &o.sampler);

    // ubo：按帧环槽位分块，每槽 256B（256 一定满足 minUniformBufferOffsetAlignment）
    o.uboStride = 256;
    VkBufferCreateInfo ubci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ubci.size = (VkDeviceSize)o.uboStride * VK_RING;
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
    o.uboBase = (float*)up;
    o.uboPtr = o.uboBase;
    memset(o.uboBase, 0, (size_t)o.uboStride * VK_RING);

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
    vattr[6].binding = 0;                        // 颜色调制改读逐顶点数据（GL loc6 数组）
    vattr[6].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    vattr[6].offset = sizeof(float) * 5;
    vattr[7].location = 7;
    vattr[7].binding = 0;                        // 同上：GL loc7 数组
    vattr[7].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    vattr[7].offset = sizeof(float) * 9;
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 2;
    vi.pVertexBindingDescriptions = vbind;
    vi.vertexAttributeDescriptionCount = 8;
    vi.pVertexAttributeDescriptions = vattr;

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    // 视口/剪裁改为动态状态：管线不再绑死创建时的 extent。
    // 写死的 viewport 会让 resize 后（或创建时尺寸不对时）画面只落在左下/左侧一小块。
    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = 1;
    vps.pViewports = nullptr;
    vps.scissorCount = 1;
    vps.pScissors = nullptr;
    VkDynamicState dynStates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo pds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    pds.dynamicStateCount = 2;
    pds.pDynamicStates = dynStates;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = 0xF;
    // 混合变体由 vk_tuning.ini 的 blend= 选择；默认 srcalpha 与加调参前写死的完全一致。
    // 说明：混合因子烘在管线里，切换变体需要重启（不是热更），见 docs/vk_tuning.md。
    switch (vktuning::Get().blendVariant) {
        case vktuning::BLEND_OFF:
            cba.blendEnable = VK_FALSE;
            cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            cba.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
            cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            break;
        case vktuning::BLEND_PREMUL:
            // 预乘 alpha：源已经是 (r*a, g*a, b*a, a)，再加 SRC_ALPHA 会二次乘
            cba.blendEnable = VK_TRUE;
            cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            break;
        case vktuning::BLEND_ADDITIVE:
            cba.blendEnable = VK_TRUE;
            cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            break;
        default:
            cba.blendEnable = VK_TRUE;
            cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            // dstAlpha 从 ZERO 改为 ONE_MINUS_SRC_ALPHA：
            // ZERO 会把目标 alpha 清零（贴图黑底=alpha 0 的区域变成实心黑），
            // 改成与 color 一致的合成式即可保留目标 alpha，且不像 premul 那样把 color 因子也改成 ONE
            // （那会把绿蓝放大）。color 三个因子保持原样不动。
            cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            break;
    }
    cba.colorBlendOp = VK_BLEND_OP_ADD;
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
    gp.pDynamicState = &pds;
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

    // vertex buffer：按帧环槽位分区，每槽 MAX_SPRITES 个四边形
    o.vbBytes = (size_t)MAX_SPRITES * 6 * 15 * sizeof(float);
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = (VkDeviceSize)o.vbBytes * VK_RING;
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
    // 观测（非变量）：binding1 常量的 map 结果与真值。loc5=0 的两种可能（map 失败 / 没写进去）
    // 靠这一行直接分辨，不需要再去猜。
    fprintf(stderr, "[vk] attr const buf map=%s vals=%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f\n",
            o.attrPtr ? "ok" : "NULL",
            o.attrPtr ? o.attrPtr[0] : -1.0f, o.attrPtr ? o.attrPtr[1] : -1.0f,
            o.attrPtr ? o.attrPtr[2] : -1.0f, o.attrPtr ? o.attrPtr[3] : -1.0f,
            o.attrPtr ? o.attrPtr[4] : -1.0f, o.attrPtr ? o.attrPtr[5] : -1.0f,
            o.attrPtr ? o.attrPtr[6] : -1.0f, o.attrPtr ? o.attrPtr[7] : -1.0f,
            o.attrPtr ? o.attrPtr[8] : -1.0f);

    o.textures.resize(MAX_TEXTURES);

    // 接线（本轮只接不选）：把设备上下文交给 gl_shader，让它能自己建管线。
    // 离屏管线必须有离屏 render pass：建一条与目标兼容的（R8G8B8A8、单附件、单 subpass，
    // 兼容性只看格式/样本数/附件数，loadOp 与 finalLayout 不参与比较），否则
    // gl_shader_build_pipeline(prog, layout, true) 会全部失败，滤镜链退回兜底。
    {
        VkRenderPass offRP = VK_NULL_HANDLE;
        VkAttachmentDescription att{};
        att.format = VK_FORMAT_R8G8B8A8_UNORM;
        att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        att.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkAttachmentReference ref{};
        ref.attachment = 0;
        ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &ref;
        VkRenderPassCreateInfo orpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        orpci.attachmentCount = 1;
        orpci.pAttachments = &att;
        orpci.subpassCount = 1;
        orpci.pSubpasses = &sub;
        createRP(o.dev, &orpci, nullptr, &offRP);

        lime::GLShaderVkContext ctx{};
        ctx.device = o.dev;
        ctx.backbufferRenderPass = o.renderPass;
        ctx.offscreenRenderPass = offRP;
        ctx.backbufferFormat = o.colorFormat;
        ctx.offscreenFormat = VK_FORMAT_R8G8B8A8_UNORM;
        ctx.viewportWidth = o.extent.width;
        ctx.viewportHeight = o.extent.height;
        ctx.uniformMemoryTypeIndex = FindMemType(memProps, 0xFFFFFFFFu,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        ctx.maxSamplers = 16;
        ctx.getDeviceProcAddr = (PFN_vkGetDeviceProcAddr) o.gipa(o.inst, "vkGetDeviceProcAddr");
        lime::gl_shader_vk_register(&ctx);
        fprintf(stderr, "[progpipe] register ok avail=%d device=%p rp=%p fmt=%d vp=%ux%u memtype=%u\n",
                lime::gl_shader_vk_available() ? 1 : 0, (void*) o.dev, (void*) o.renderPass,
                (int) o.colorFormat, o.extent.width, o.extent.height,
                (unsigned) ctx.uniformMemoryTypeIndex);
    }
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
    // P2 变体资源：状态变体管线 / 采样器缓存 / 变体描述符池（首条管线与 o.sampler 归原有路径）
    if (o.destroyPipeline) {
        for (auto& e : o.pktPipes) {
            if (e.pipe && e.pipe != o.pipeline) o.destroyPipeline(o.dev, e.pipe, nullptr);
        }
    }
    o.pktPipes.clear();
    if (o.destroySampler) {
        for (auto& e : o.pktSamplers) {
            if (e.smp) o.destroySampler(o.dev, e.smp, nullptr);
        }
    }
    o.pktSamplers.clear();
    o.pktSets.clear();
    if (o.destroyDP && o.pktSetPool) o.destroyDP(o.dev, o.pktSetPool, nullptr);
    o.pktSetPool = VK_NULL_HANDLE;
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
    if (g_destroyFence) {
        for (uint32_t i = 0; i < VK_RING; i++) {
            if (o.fences[i]) g_destroyFence(o.dev, o.fences[i], nullptr);
            o.fences[i] = VK_NULL_HANDLE;
        }
    }
    if (g_destroyQP) {
        for (uint32_t i = 0; i < VK_RING; i++) {
            if (o.passQueries[i]) g_destroyQP(o.dev, o.passQueries[i], nullptr);
            o.passQueries[i] = VK_NULL_HANDLE;
        }
    }
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
        ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                  | VK_IMAGE_USAGE_SAMPLED_BIT;   // TRANSFER_SRC：取证转储（VK_DUMP_TEX）要读回
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
        // 预览开关 swizzle=rb：采样时 R↔B 互换（默认 none = identity，与现状一致）
        if (vktuning::Get().swizzle == vktuning::SWIZZLE_RB) {
            tvi.components.r = VK_COMPONENT_SWIZZLE_B;
            tvi.components.g = VK_COMPONENT_SWIZZLE_G;
            tvi.components.b = VK_COMPONENT_SWIZZLE_R;
            tvi.components.a = VK_COMPONENT_SWIZZLE_A;
        }
        g_createView(o.dev, &tvi, nullptr, &t.view);

        std::vector<VkDescriptorSetLayout> layouts(1, o.dsl);
        VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dsai.descriptorPool = o.descPool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = layouts.data();
        VkResult dsr = g_allocDS(o.dev, &dsai, &t.set);
        if (dsr != VK_SUCCESS || t.set == VK_NULL_HANDLE) {
            // 池耗尽：以前这里不检查，t.set 为空仍继续 UpdateDescriptorSets，
            // 最终在驱动里 0xC0000005。这里直接放弃这张纹理（上层拿到 0 会跳过绘制），
            // 把刚建的 image/view 也清掉，避免半成品泄漏。
            static int s_dsFail = 0;
            if (s_dsFail < 20) {
                fprintf(stderr, "[vk] dspool exhausted r=%d w=%d h=%d n=%d\n",
                        (int) dsr, w, h, s_dsFail + 1);
            }
            s_dsFail++;
            if (t.view) { o.destroyImageView(o.dev, t.view, nullptr); t.view = VK_NULL_HANDLE; }
            if (t.image) { o.destroyImage(o.dev, t.image, nullptr); t.image = VK_NULL_HANDLE; }
            if (t.mem) { o.freeMemory(o.dev, t.mem, nullptr); t.mem = VK_NULL_HANDLE; }
            return 0;
        }

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
        wds[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        wds[0].pBufferInfo = &dbi;
        wds[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wds[1].dstSet = t.set;
        wds[1].dstBinding = 1;
        wds[1].descriptorCount = 1;
        wds[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wds[1].pImageInfo = &dii;
        g_updateDS(o.dev, 2, wds, 0, nullptr);

        // 取证：始终按 stride=97 采样存下创建时的 mean/nb（与下面的 [vk] texmean 完全同一口径），
        // 转储（VK_DUMP_TEX）时用它对账；只有大图才打印，避免刷屏
        size_t cnt = 0, sum = 0, nb = 0;
        size_t px = (size_t)w * h;
        for (size_t k = 0; k < px; k += 97) {
            const unsigned char* p = rgba + k * 4;
            unsigned int v = (p[0] + p[1] + p[2]) / 3;
            sum += v;
            if (v >= 8) nb++;
            cnt++;
        }
        t.createMean = cnt ? (float)((double)sum / (double)cnt) : 0.0f;
        t.createNb = cnt ? (float)(100.0 * (double)nb / (double)cnt) : 0.0f;
        t.hasCreateMean = cnt > 0;
        t.updates = 0;
        if ((size_t)w * h >= (size_t)2048 * 2048) {
            fprintf(stderr, "[vk] texmean id=%u w=%d h=%d mean=%.2f nb=%.1f\n",
                    (unsigned)i + 1, w, h,
                    t.createMean, t.createNb);
        }
        t.w = w;
        t.h = h;
        t.used = true;
        t.verts.clear();
        t.progs.clear();   // 必须与 verts 同步清空，否则 prog 标签逐帧累积、长度对不上
        return i + 1;
    }
    return 0;
}

// 真正释放纹理槽位。只能在“本帧命令缓冲已提交”之后调用（见 EndFrame 末尾），
// 否则槽位 id 会被同一帧里后续的 CreateTexture 复用，导致已记录的绘制段改采样新纹理。
void VkRenderer::DestroyTextureNow(uint32_t id) {
    Impl& o = *impl;
    if (id == 0 || id > o.textures.size()) return;
    TexSlot& t = o.textures[id - 1];
    if (!t.used) return;
    gl_vk_on_texture_destroyed(id);   // 清掉所有指向该 id 的 GL 名字
    PktDropSets(id);   // 变体 descriptor set 指向这张图，先还回池里
    // 批处理路径的 set 也要还回池（池带 FREE 位）。不然每次纹理重建都漏一个 set，
    // 池满后 Allocate 返回空句柄，绑定/更新时崩在驱动里。
    if (t.set) {
        if (g_freeDS) g_freeDS(o.dev, o.descPool, 1, &t.set);
        t.set = VK_NULL_HANDLE;
    }
    if (t.view) o.destroyImageView(o.dev, t.view, nullptr);
    if (t.sampler) o.destroySampler(o.dev, t.sampler, nullptr);
    if (t.image) o.destroyImage(o.dev, t.image, nullptr);
    if (t.mem) o.freeMemory(o.dev, t.mem, nullptr);
    t = TexSlot();
}

void VkRenderer::DestroyTexture(uint32_t id) {
    // 延迟到 EndFrame 提交完成之后真正销毁（见 pendingTexDestroy）。
    if (id == 0) return;
    impl->pendingTexDestroy.push_back(id);
}

uint32_t VkRenderer::TextureCount() const {
    uint32_t c = 0;
    for (auto& t : impl->textures) if (t.used) c++;
    return c;
}

// ---- GL 常量 -> Vulkan 枚举（只认最常用的几种，其余按最接近语义映射）----
static VkFilter GlFilterToVk(int glFilter) {
    switch (glFilter) {
        // 只认最近的采样语义：mipmap 变体在 maxLod=0 时等价于 level0 的最近/线性
        case 0x2600: // GL_NEAREST
        case 0x2700: // GL_NEAREST_MIPMAP_NEAREST
        case 0x2702: // GL_NEAREST_MIPMAP_LINEAR
            return VK_FILTER_NEAREST;
        default:     // GL_LINEAR / GL_LINEAR_MIPMAP_* / 其余
            return VK_FILTER_LINEAR;
    }
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
    // 调参入口：ini 的 by_glname / by_size / 全局默认依次覆盖 GL 记录值；
    // 全部为默认时这里原样返回 GL 传进来的枚举，行为与加调参前一致。
    const int effMin = vktuning::ResolveFilter(minFilter, t.w, t.h, texId);
    const int effMag = vktuning::ResolveFilter(magFilter, t.w, t.h, texId);
    const int effWrapS = vktuning::ResolveWrap(wrapS, t.w, t.h);
    const int effWrapT = vktuning::ResolveWrap(wrapT, t.w, t.h);
    sci.magFilter = GlFilterToVk(effMag);
    sci.minFilter = GlFilterToVk(effMin);
    // 诊断：实测游戏的采样方式（每个纹理 id 首次设置或参数变化时打印一次）
    if (texId < 1024) {
        static uint64_t s_sig[1024] = {0};
        static bool s_seen[1024] = {false};
        uint64_t sig = ((uint64_t)(uint32_t)minFilter << 48) | ((uint64_t)(uint32_t)magFilter << 32) |
                       ((uint64_t)(uint32_t)wrapS << 16) | (uint32_t)wrapT;
        if (!s_seen[texId] || s_sig[texId] != sig) {
            s_seen[texId] = true;
            s_sig[texId] = sig;
            // gl= 记录原值，eff= 调参后的实际值：两者不同才说明 ini 生效了
            fprintf(stderr, "[sampler] tex=%u %dx%d gl(min=0x%x mag=0x%x wrap=0x%x/0x%x) "
                    "eff(min=0x%x mag=0x%x wrap=0x%x/0x%x ts=%s)\n",
                    texId, t.w, t.h, minFilter, magFilter, wrapS, wrapT,
                    effMin, effMag, effWrapS, effWrapT, vktuning::Source());
        }
    }
    // 目前纹理只有 1 级 mip，mip 模式保持 NEAREST 并把 maxLod 钳到 0，
    // 否则采样器会去取不存在的 level 导致黑边/全黑
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = GlWrapToVk(effWrapS);
    sci.addressModeV = GlWrapToVk(effWrapT);
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
    t.updates++;   // 取证：创建时的 createMean 基线从此失效
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
        if (o.currentTarget == texId) {
            // 探针：目标是当前渲染目标时顶点被静默丢弃（缺口 B 的唯一证据点）
            fprintf(stderr, "[vk] draw-selfdrop tex=%u cur=%u f=%llu n=6 sprite\n",
                    texId, o.currentTarget, (unsigned long long) g_drawFrame);
            return;
        } // no self sampling
        vertsPtr = &o.targets[ti].verts;
    } else {
        if (texId == 0 || texId > o.textures.size()) return;
        TexSlot& t = o.textures[texId - 1];
        if (!t.used) return;
        vertsPtr = &t.verts;
    }
    NoteSampled(texId);
    float hw = w * 0.5f, hh = h * 0.5f;
    float qx[6] = {x - hw, x + hw, x + hw, x - hw, x + hw, x - hw};
    float qy[6] = {y - hh, y - hh, y + hh, y - hh, y + hh, y + hh};
    float qu[6] = {u0, u1, u1, u0, u1, u0};
    float qv[6] = {v0, v0, v1, v0, v1, v1};
    size_t segOff = vertsPtr->size() / 15;
    for (int i = 0; i < 6; i++) {
        PushLimeVertex(*vertsPtr, qx[i], qy[i], qu[i], qv[i]);
    }
    o.segs.push_back({texId, segOff, 6});
}

void VkRenderer::DrawRaw(uint32_t texId, const float* verts, int vertexCount, const float* vertexAlpha,
                         const float* vertexMult, const float* vertexOff,
                         const uint8_t* uniformData, uint32_t uniformSize) {
    Impl& o = *impl;
    // [vkfix] OpenFL 的 scrollRect 在变换带旋转/斜切时会放弃 scissor，改走 stencil 遮罩：
    // 先 glColorMask(false,false,false,false)，再用 Context3DMaskShader（1x1 不透明黑）
    // 画一块相机尺寸的矩形当遮罩。批处理/progpipe 路径没有 stencil，这笔会原样画成
    // 黑色四边形（Act2→Act3 camHUD 倾倒时随 GUI 转 15° 的"黑框"就是它）。
    // 颜色写全关 = 本笔不产生任何可见像素，直接跳过。
    {
        int cm[4] = {1, 1, 1, 1};
        lime::glstate::GetColorMask(cm);
        if (!cm[0] && !cm[1] && !cm[2] && !cm[3]) return;
    }
    std::vector<float>* vertsPtr = nullptr;
    std::vector<uint16_t>* progsPtr = nullptr;
    if (texId >= 1000) {
        uint32_t ti = texId - 1000;
        if (ti >= o.targets.size() || !o.targets[ti].used) return;
        if (o.currentTarget == texId) {
            // 探针：目标是当前渲染目标时顶点被静默丢弃（缺口 B 的唯一证据点）
            fprintf(stderr, "[vk] draw-selfdrop tex=%u cur=%u f=%llu n=%d\n",
                    texId, o.currentTarget, (unsigned long long) g_drawFrame, vertexCount);
            return;
        }
        vertsPtr = &o.targets[ti].verts;
        progsPtr = &o.targets[ti].progs;
    } else {
        if (texId == 0 || texId > o.textures.size()) return;
        TexSlot& t = o.textures[texId - 1];
        if (!t.used) return;
        vertsPtr = &t.verts;
        progsPtr = &t.progs;
    }
    NoteSampled(texId);
    if (vertexCount <= 0) return;
    // 诊断（VK_COLORLOG="from-to"，默认关）：按帧范围打印每笔实际使用的顶点色/alpha，
    // 用于定位 colorTransform 在哪一类精灵上丢失（例如主菜单静态噪声层不上色）。
    {
        static long long s_cf = -3, s_ct = -3;
        if (s_cf == -3) {
            const char* e = getenv("VK_COLORLOG");
            s_cf = -1; s_ct = -1;
            if (e && e[0]) {
                long long a = 0, b = 0;
                if (sscanf(e, "%lld-%lld", &a, &b) == 2) { s_cf = a; s_ct = b; }
                else { s_cf = atoll(e); s_ct = 0x7fffffffffffffffLL; }
            }
        }
        if (s_cf >= 0 && (long long)g_drawFrame >= s_cf && (long long)g_drawFrame <= s_ct) {
            const float* cm0 = vertexMult ? vertexMult : g_drawMult;
            const float* cf0 = vertexOff ? vertexOff : g_drawOff;
            float a0 = vertexAlpha ? vertexAlpha[0] : g_drawAlpha;
            fprintf(stderr,
                    "[colorlog] f=%llu cur=%u tex=%u n=%d prog=%d csrc=%s a=%.3f "
                    "col=[%.3f %.3f %.3f %.3f] off=[%.3f %.3f %.3f %.3f]\n",
                    (unsigned long long)g_drawFrame, (unsigned)o.currentTarget, texId,
                    vertexCount, lime::gl_shader_current_program(),
                    vertexMult ? "arr" : "const", a0,
                    cm0[0], cm0[1], cm0[2], cm0[3], cf0[0], cf0[1], cf0[2], cf0[3]);
        }
    }
    // 诊断（VK_ALPHALOG=1，默认关）：只打“含半透明顶点 alpha 且纹素 >=30”的笔（标题/菜单
    // 的纹理是 2..17，游戏内是 20+），最多 600 条。目的：拿到 NoteSplash / 长条（alpha=0.6）
    // 在我们的管线里的真实 alpha，判断是否被二次乘。
    {
        static int s_mode = -1, s_n = 0;
        if (s_mode < 0) { const char* e = getenv("VK_ALPHALOG"); s_mode = (e && e[0]) ? 1 : 0; }
        if (s_mode && s_n < 1200 && o.currentTarget >= 1006) {
            float amn = 1.0f, amx = 0.0f;
            if (vertexAlpha) {
                for (int i = 0; i < vertexCount; i++) {
                    float v = vertexAlpha[i];
                    if (v < amn) amn = v;
                    if (v > amx) amx = v;
                }
            } else {
                amn = amx = g_drawAlpha;
            }
            if (amn < 0.95f) {
                s_n++;
                int ben = 0, bsrc = 0, bdst = 0;
                lime_vk_get_blend_state(&ben, &bsrc, &bdst);
                fprintf(stderr,
                        "[alphalog] #%d f=%llu cur=%u tex=%u prog=%d n=%d amin=%.3f amax=%.3f blend=%d,%d,%d\n",
                        s_n, (unsigned long long)g_drawFrame, (unsigned)o.currentTarget, texId,
                        lime::gl_shader_current_program(), vertexCount, amn, amx, ben, bsrc, bdst);
            }
        }
    }
    // 后缓冲合成笔的 alpha（第十四轮）：标题页一个视觉帧分两笔铺后缓冲，
    // 批处理按纹理 id 排序 → 1001 先、1002 后。离屏画布现在按 GL 语义清成 (0,0,0,0)，
    // 两层都带像素 alpha，正常混合即可，默认 1.0（不再需要均一降 alpha 的折中）。
    // 保留 VK_COMP_ALPHA 作为调试旋钮：设 <1 时该笔按固定 alpha 叠。
    static float s_compAlpha = -1.0f;
    if (s_compAlpha < 0.0f) {
        const char* v = getenv("VK_COMP_ALPHA");
        s_compAlpha = (v && v[0]) ? (float)atof(v) : 1.0f;
    }
    static std::vector<float> s_alphaBuf;
    if (s_compAlpha < 1.0f && o.currentTarget == 0 && o.bbLoad && texId == 1002) {
        s_alphaBuf.assign((size_t)vertexCount, s_compAlpha);
        vertexAlpha = s_alphaBuf.data();
    }
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

    // 观测（非行为）：记录本笔 draw 对应的 GL program，并首次见到时打一次程序表。
    int vkProg = lime::gl_shader_current_program ();
    {
        static unsigned s_progSeen[256] = {0};
        if (vkProg >= 0 && vkProg < 256 && !s_progSeen[vkProg]) {
            s_progSeen[vkProg] = 1;
            fprintf(stderr, "[glprog] id=%d attrs=%d hasLoc5=%d ubo=%u\n",
                    vkProg, lime::gl_shader_attribute_count(vkProg),
                    lime::gl_shader_program_has_attribute(vkProg, 5) ? 1 : 0,
                    (unsigned) lime::gl_shader_uniform_buffer_size(vkProg));
        }
    }

    // 接线探针（本轮不选管线）：对"没有 loc5"的 program 用渲染器自己的 60B 交错布局
    // 建一条 gl_shader 管线，只为证明接线可用。该管线不进入任何 draw。
    if (vkProg > 0 && vkProg < 256) {
        // 同一个 program 需要两种管线：画到屏幕(offscreen=false)与画到离屏目标(true)。
        // 之前只建 false，导致"所有画到离屏的笔"取不到管线、全部退回兜底，
        // CRT / bloom 这些滤镜链因此从未执行。
        static signed char s_built[256][2] = {{0}};
        auto buildOne = [&](const lime::GLShaderVertexLayout* lay, bool off) {
            int slot = off ? 1 : 0;
            if (s_built[vkProg][slot] != 0) return;
            bool ok = lime::gl_shader_build_pipeline (vkProg, lay, off);
            VkPipeline pipe = ok ? lime::gl_shader_pipeline (vkProg, off) : VK_NULL_HANDLE;
            s_built[vkProg][slot] = ok ? 1 : -1;
            if (ok && pipe && !off) {
                g_probePipeline = pipe;
                g_probeProgram = vkProg;
            }
            fprintf(stderr, "[progpipe] build prog=%d off=%d ok=%d pipeline=%p\n",
                    vkProg, off ? 1 : 0, ok ? 1 : 0, (void*) pipe);
        };
        if (lime::gl_shader_program_has_attribute (vkProg, 5)) {
            // 带 loc5（alpha）的 program：原版 trace 里 loc5 是独立缓冲 stride=4，
            // 我们的顶点是 60 字节交错布局，按 offset 16 映射即可。
            // [vkfix 2026-11-11] 游戏 shader 顶点端是 `Alphav = openfl_Alpha * alpha`：
            // openfl_Alpha(loc1) 在原版 GL 里是常量 1.0（apitrace: glVertexAttrib1f(1, 1)
            // ×135k），真正的逐顶点 alpha 只在 loc5。这里之前没绑 loc1，读到什么全看驱动；
            // 显式绑到 binding1 的 stride=0 常量缓冲（值 1.0），保证 alpha 只乘一次。
            static const lime::GLShaderVertexAttribute s_vattrs5[6] = {
                {0, 0, 109 /* R32G32B32A32_SFLOAT */, 0},
                {1, 1, 100 /* R32_SFLOAT */, 0},   // openfl_Alpha ← 常量 1.0（binding1 stride=0）
                {5, 0, 100 /* R32_SFLOAT */, 16},
                {6, 0, 109, 20},
                {7, 0, 109, 36},
                {4, 0, 103 /* R32G32_SFLOAT */, 52},
            };
            lime::GLShaderVertexLayout layout5{0, 60, s_vattrs5, 6};
            buildOne(&layout5, false);
            buildOne(&layout5, true);
        } else {
            static const lime::GLShaderVertexAttribute s_vattrs[5] = {
                {0, 0, 103 /* R32G32_SFLOAT */, 0},
                {1, 0, 100 /* R32_SFLOAT */, 16},
                {2, 0, 109 /* R32G32B32A32_SFLOAT */, 20},
                {3, 0, 109, 36},
                {4, 0, 103, 52},
            };
            lime::GLShaderVertexLayout layout{0, 60, s_vattrs, 5};
            buildOne(&layout, false);
            buildOne(&layout, true);
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
            // 顶点色直接证据：loc6/loc7 每顶点 4 float（无数组时取常量）
            const float* cm = vertexMult ? vertexMult : g_drawMult;
            const float* cf = vertexOff ? vertexOff : g_drawOff;
            fprintf(stderr, "[vkdraw] f=%llu cur=%u tex=%u tsz=%.0fx%.0f n=%d uv=[%.3f..%.3f, %.3f..%.3f] pos=[%.1f..%.1f, %.1f..%.1f] norm=%d whole=%d asrc=%s amin=%.3f amax=%.3f prog=%d csrc=%s col=[%.3f %.3f %.3f %.3f] coff=[%.3f %.3f %.3f %.3f]\n",
                    (unsigned long long)g_drawFrame, o.currentTarget, texId, tw, th, vertexCount,
                    uMin, uMax, vMin, vMax, xMin, xMax, yMin, yMax,
                    pixelUv ? 1 : 0, spansWhole ? 1 : 0, vertexAlpha ? "arr" : "const", aMin, aMax, vkProg,
                    vertexMult ? "arr" : "const",
                    cm[0], cm[1], cm[2], cm[3], cf[0], cf[1], cf[2], cf[3]);
            s_logged++;
        }
        // 按 program 只计数不逐条打：主菜单段的 prog 分布靠这里，不受 6000 条上限影响
        if (vkProg >= 0 && vkProg < 256) {
            g_progDraw[vkProg]++;
            if (spansWhole) g_progWhole[vkProg]++;
            g_progVerts[vkProg] += (uint64_t) vertexCount;
        }
        g_progDrawTotal++;
    }

    vertsPtr->reserve(vertsPtr->size() + (size_t)vertexCount * 15);
    size_t segOff = vertsPtr->size() / 15;
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
        // 走到游戏自己的管线时，离屏结果送进后缓冲后整屏是上下颠倒的（GL 原点左下、
        // Vulkan Y 向下）。负高度视口改不动它（实测完全无变化），说明翻转发生在"采样"
        // 这一步，所以在最终上屏那一笔把 V 翻转回来。
        static int s_flipV = -1;
        if (s_flipV < 0) {
            const char* e = getenv("VK_PROGPIPE_USE");
            s_flipV = (e && e[0] == '1') ? 1 : 0;
        }
        if (s_flipV && o.currentTarget == 0 && texId >= 1000) v = 1.0f - v;
        // 每顶点颜色调制：loc6/loc7 的真实数据（来自 GL 的 loc6/loc7 顶点数组），
        // 没有逐顶点数组时退回 SetDrawColor 的常量
        static const float kIdentityMult[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        const float* m = vertexMult ? (vertexMult + (size_t)i * 4) : g_drawMult;
        // [vkfix] 数组路径的乘数三通道全零 = GL 侧没设 colorTransform，折成 identity，
        // 与常量路径同样处理；否则真 frag 会把整笔乘成黑。
        if (vertexMult && m[0] == 0.0f && m[1] == 0.0f && m[2] == 0.0f) m = kIdentityMult;
        const float* f = vertexOff ? (vertexOff + (size_t)i * 4) : g_drawOff;
        PushLimeVertex(*vertsPtr, px, py, u, v,
                       vertexAlpha ? vertexAlpha[i] : g_drawAlpha, m, f);
        progsPtr->push_back((uint16_t)((vkProg > 0 && vkProg < 65535) ? vkProg : 0));
    }
    o.segs.push_back({texId, segOff, (size_t)vertexCount});
    if (uniformData && uniformSize > 0) {
        o.segs.back().uni.assign(uniformData, uniformData + uniformSize);
    }
}

void VkRenderer::BeginFrame(float r, float g, float b, float a) {
    PerfFrameBegin();
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

bool VkRenderer::WindowSizeMatchesExtent() {
    Impl& o = *impl;
    if (!o.nativeWindow) return true;
    int w = 0, h = 0;
    // 平台不提供客户区尺寸（Android）= 视为始终一致，交换链尺寸就是画面尺寸。
    if (!vkplat::WindowClientSize(o.nativeWindow, &w, &h)) return true;
    return (int)o.extent.width == w && (int)o.extent.height == h;
}

// 每帧记录窗口客户区尺寸。必须独立于 present 成功与否调用：
// resize 动画期间 acquire 会持续返回 OUT_OF_DATE，帧根本开不起来、present 分支不执行，
// 若只在 present 分支里更新时间戳，lastResizeTick 会一直是旧值 → InResizeAnimation()
// 恒 false → 每帧都重建交换链（实测全屏放大 1 秒重建 24 次，帧率 244→69）。
// 返回 true 表示本帧尺寸相对上次有变化。
bool VkRenderer::ObserveWindowSize() {
    Impl& o = *impl;
    int winW = o.width, winH = o.height;
    if (o.nativeWindow) {
        int cw = 0, ch = 0;
        if (vkplat::WindowClientSize(o.nativeWindow, &cw, &ch)) {
            winW = cw;
            winH = ch;
        }
    }
    uint64_t nowMs = vkplat::TicksMs();
    if (winW != o.lastWinW || winH != o.lastWinH) {
        o.lastWinW = winW;
        o.lastWinH = winH;
        o.lastResizeTick = nowMs;
        o.sizeMismatchSince = nowMs;
        return true;
    }
    return false;
}

// 窗口缩放进度的统一判定：最近 400ms 内窗口尺寸变过就算“动画中”。
// 动画期间尽量不重建交换链（重建会 waitIdle，把帧时间拉长，lime 的缩放动画直接跳终点）。
bool VkRenderer::InResizeAnimation() {
    Impl& o = *impl;
    if (o.lastResizeTick == 0) return false;
    return ((uint64_t)vkplat::TicksMs() - o.lastResizeTick) < 400;
}

// 高精度毫秒计时（诊断）。平台实现见 vk_platform_*。
static double VkNowMs() {
    return vkplat::NowMs();
}

// 交换链重建：present/acquire 一旦返回 OUT_OF_DATE 或 SUBOPTIMAL，旧交换链上
// 所有后续 acquire 都会继续失败，而后缓冲帧开不起来 → 永久停在上一张清屏帧。
// 这里按 surface 当前能力重建交换链 + image view + 全部 framebuffer。
bool VkRenderer::RecreateSwapchain() {
    Impl& o = *impl;
    if (!o.dev || !o.swapchain || !o.gipa) return false;
    const double tStart = VkNowMs();
    auto createSwap = (PFN_vkCreateSwapchainKHR)o.gipa(o.inst, "vkCreateSwapchainKHR");
    auto getImages = (PFN_vkGetSwapchainImagesKHR)o.gipa(o.inst, "vkGetSwapchainImagesKHR");
    auto getSurfCaps = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)o.gipa(o.inst,
            "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    auto createFB = (PFN_vkCreateFramebuffer)o.gipa(o.inst, "vkCreateFramebuffer");
    if (!o.destroyFramebuffer || !o.destroyImageView || !o.destroySwapchain ||
        !createSwap || !getImages || !getSurfCaps || !createFB) return false;

    // 每帧提交后本来就 g_waitIdle(queue)，这里再等一次确保没有在飞的引用
    if (g_waitIdle) g_waitIdle(o.queue);

    uint32_t oldImages = (uint32_t)o.swapImages.size();
    uint32_t oldExtW = o.extent.width, oldExtH = o.extent.height;

    auto dropFB = [&](std::vector<VkFramebuffer>& fbs) {
        for (auto fb : fbs) if (fb) o.destroyFramebuffer(o.dev, fb, nullptr);
        fbs.clear();
    };
    dropFB(o.framebuffers);
    dropFB(o.framebuffersLoad);
    dropFB(o.framebuffersLoadKeep);
    dropFB(o.framebuffersKeep);
    for (auto v : o.swapViews) if (v) o.destroyImageView(o.dev, v, nullptr);
    o.swapViews.clear();
    o.swapImages.clear();
    // 计时（诊断）：缩放时每次重建的耗时分解，用来判断停顿在销毁旧链还是建新链。
    const double tDrop = VkNowMs();
    // 旧交换链先留着：VkSwapchainCreateInfoKHR.oldSwapchain 交给驱动做“原地换尺寸”，
    // 比先销毁再新建快得多（缩放动画期间每次重建的停顿主要来自这里）。
    VkSwapchainKHR oldSwap = o.swapchain;
    o.swapchain = VK_NULL_HANDLE;

    VkSurfaceCapabilitiesKHR caps{};
    if (getSurfCaps(o.phys, o.surf, &caps) != VK_SUCCESS) {
        fprintf(stderr, "[vk] swapchain recreate ok=0 path=caps\n");
        return false;
    }
    o.extent = PickSwapchainExtent (caps, o.width, o.height);
    {
        static int s_capsLogged = 0;
        if (s_capsLogged < 6) {
            s_capsLogged++;
            fprintf(stderr, "[vk] surf caps cur=%ux%u min=%ux%u max=%ux%u\n",
                    caps.currentExtent.width, caps.currentExtent.height,
                    caps.minImageExtent.width, caps.minImageExtent.height,
                    caps.maxImageExtent.width, caps.maxImageExtent.height);
        }
    }
    if (o.extent.width == 0 || o.extent.height == 0) {
        fprintf(stderr, "[vk] swapchain recreate ok=0 path=zeroextent\n");
        return false;
    }

    VkSwapchainCreateInfoKHR swci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    swci.surface = o.surf;
    swci.minImageCount = caps.minImageCount + 1;
    swci.imageFormat = o.colorFormat;
    swci.imageColorSpace = o.colorSpace;
    swci.imageExtent = o.extent;
    swci.imageArrayLayers = 1;
    swci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    swci.preTransform = caps.currentTransform;
    swci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swci.oldSwapchain = oldSwap;
    // 呈现模式：原版游戏初始化时调 wglSwapIntervalEXT(0)（apitrace 第 1651 行），要的是不锁帧。
    // 固定 FIFO 会把帧率钉在 60Hz，和游戏实际帧时间对不上就出现"一抖一抖"的规律卡顿。
    // 优先 MAILBOX（不锁帧、不撕裂），不支持回退 FIFO；VK_PRESENT_MODE=fifo 可强制回旧行为。
    swci.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
    {
        auto getPresentModes = (PFN_vkGetPhysicalDeviceSurfacePresentModesKHR)
                o.gipa(o.inst, "vkGetPhysicalDeviceSurfacePresentModesKHR");
        uint32_t pmc = 0;
        bool hasMailbox = false;
        if (getPresentModes) {
            getPresentModes(o.phys, o.surf, &pmc, nullptr);
            std::vector<VkPresentModeKHR> pms(pmc);
            if (pmc) getPresentModes(o.phys, o.surf, &pmc, pms.data());
            for (uint32_t i = 0; i < pmc; i++) if (pms[i] == VK_PRESENT_MODE_MAILBOX_KHR) hasMailbox = true;
        }
        const char* want = getenv("VK_PRESENT_MODE");
        if ((want && want[0] == 'f') || !hasMailbox) swci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        fprintf(stderr, "[vk] presentMode=%s (mailbox_available=%d)\n",
                swci.presentMode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : "fifo",
                hasMailbox ? 1 : 0);
    }
    swci.clipped = VK_TRUE;
    VkResult cr = createSwap(o.dev, &swci, nullptr, &o.swapchain);
    if (cr != VK_SUCCESS) {
        o.swapchain = VK_NULL_HANDLE;
        if (oldSwap) o.destroySwapchain(o.dev, oldSwap, nullptr);
        fprintf(stderr, "[vk] swapchain recreate ok=0 path=create err=%d\n", (int)cr);
        return false;
    }
    const double tCreate = VkNowMs();
    // 新交换链建好后再销毁旧的（oldSwapchain 交接完成，驱动已可从旧的解绑）
    if (oldSwap) o.destroySwapchain(o.dev, oldSwap, nullptr);
    const double tDestroy = VkNowMs();

    uint32_t ic = 0;
    getImages(o.dev, o.swapchain, &ic, nullptr);
    o.swapImages.resize(ic);
    getImages(o.dev, o.swapchain, &ic, o.swapImages.data());
    o.swapViews.assign(ic, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < ic; i++) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = o.swapImages[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = o.colorFormat;
        vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.levelCount = 1;
        vci.subresourceRange.layerCount = 1;
        g_createView(o.dev, &vci, nullptr, &o.swapViews[i]);
    }

    auto mkFBSet = [&](VkRenderPass rp, std::vector<VkFramebuffer>& fbs) {
        if (rp == VK_NULL_HANDLE) return;
        fbs.assign(ic, VK_NULL_HANDLE);
        for (uint32_t i = 0; i < ic; i++) {
            VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fci.renderPass = rp;
            fci.attachmentCount = 1;
            fci.pAttachments = &o.swapViews[i];
            fci.width = o.extent.width;
            fci.height = o.extent.height;
            fci.layers = 1;
            createFB(o.dev, &fci, nullptr, &fbs[i]);
        }
    };
    mkFBSet(o.renderPass, o.framebuffers);
    mkFBSet(o.renderPassLoad, o.framebuffersLoad);
    mkFBSet(o.renderPassLoadKeep, o.framebuffersLoadKeep);
    mkFBSet(o.renderPassKeep, o.framebuffersKeep);

    o.frameImageIndex = 0;
    o.bbHeld = false;
    o.bbLoad = false;
    o.sizeMismatchSince = 0;
    fprintf(stderr, "[vk] swapchain recreate ok=1 oldImages=%u newImages=%u %ux%u -> %ux%u\n",
            oldImages, ic, oldExtW, oldExtH, o.extent.width, o.extent.height);
    {
        const double tEnd = VkNowMs();
        fprintf(stderr, "[vk] reccost total=%.1fms drop=%.1fms create=%.1fms oldDestroy=%.1fms fb=%.1fms\n",
                tEnd - tStart, tDrop - tStart, tCreate - tDrop, tDestroy - tCreate, tEnd - tDestroy);
    }
    return true;
}

bool VkRenderer::BindRenderTarget(uint32_t id, float r, float g, float b, float a) {
    Impl& o = *impl;
    // packet 执行路径（VK_PKT_EXEC=1）独占渲染：这里不开批处理帧，改由 packet 侧
    // 按"绑定即开 pass"的时机立即录制。开关关闭时走下面原有的逐字逻辑。
    if (lime::pkt::ExecActive()) {
        return PktBeginTarget(id, r, g, b, a);
    }
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
            if (id == 0) {
                g_bbOpen = 1;
                fprintf(stderr, "[vk] bbframe open f=%llu ok=1 target=0 path=reuse load=%d\n",
                        (unsigned long long) g_drawFrame, o.bbLoad ? 1 : 0);
            }
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
            if (id == 0) {
                g_bbOpen = 1;
                fprintf(stderr, "[vk] bbframe open f=%llu ok=1 target=0 path=same load=%d\n",
                        (unsigned long long) g_drawFrame, o.bbLoad ? 1 : 0);
            }
            return true;
        }
        EndFrame ();
    }
    // 探针（只记状态）：0 → 离屏切换时后缓冲若还挂着一批没提交的顶点，这批顶点
    // 会被 1641 的 flush 条件（只对 currentTarget>=1000）跳过，从而并进离屏 pass。
    if (o.frameOpen && o.currentTarget == 0 && id >= 1000) {
        size_t pv = PendingVertsOf (o.textures, o.targets);
        fprintf(stderr, "[vk] bbframe migrate f=%llu from=0 to=%u pendingVerts=%zu\n",
                (unsigned long long) g_drawFrame, id, pv);
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
        // 先观察窗口尺寸：即使本帧 acquire 失败，也要让“动画中”的判定拿到最新时间戳，
        // 否则 lastResizeTick 永远停在旧值 → 全屏放大时每帧都重建交换链。
        ObserveWindowSize();
        // 交换链脏（present/acquire 曾返回 OUT_OF_DATE/SUBOPTIMAL）→ 开帧前无条件重建。
        // 不依赖 acquire 是否报错：present 只要失败一次，[vk] pres 就停更、后缓冲再也开不起来。
        if (o.swapDirty) {
            if (RecreateSwapchain()) o.swapDirty = false;
            else return false;
        }
        if (o.bbHeld && o.renderPassLoad != VK_NULL_HANDLE) {
            // 实验 VK_SINGLE_PRESENT=1：本帧已持有同一张图，后续合成用 LOAD pass 叠加
            o.bbLoad = true;
        } else {
            VkResult ar = g_acquire(o.dev, o.swapchain, UINT64_MAX, VK_NULL_HANDLE, VK_NULL_HANDLE, &o.frameImageIndex);
            // resize 动画期间 acquire 会报 SUBOPTIMAL；此时交换链尺寸其实还“够用”，
            // 不能标脏，否则又变成每帧重建（窗口缩放被拖成几大跳）。
            // 只有尺寸真的对上了还报 SUBOPTIMAL 才需要重建。
            if (ar == VK_SUBOPTIMAL_KHR && WindowSizeMatchesExtent() && !InResizeAnimation()) o.swapDirty = true;
            if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) {
                // OUT_OF_DATE：交换链尺寸和窗口对不上，不重建就一帧都出不来。
                // Win32 的 surface 只允许交换链 = 当前窗口尺寸（min=max=current），
                // 无法建一个“更大”的覆盖交换链，所以只能重建。这里做限频：
                // 缩放动画中最多每 100ms 重建一次（约 10 次/秒），内容跟着窗口走，
                // 但不至于每帧 waitIdle+建链（实测每步一次会掉到 69fps）。
                const uint64_t nowC = (uint64_t)vkplat::TicksMs();
                static int s_recMs = -1;
                if (s_recMs < 0) {
                    const char* rv = getenv("VK_RESIZE_RECREATE_MS");
                    int v = (rv && rv[0]) ? atoi(rv) : 0;
                    s_recMs = (v > 0) ? v : 100;
                }
                if (!InResizeAnimation() || nowC - o.lastCoverTick >= (uint64_t)s_recMs) {
                    o.lastCoverTick = nowC;
                    o.swapDirty = true;
                }
                fprintf(stderr, "[vk] bbframe open f=%llu ok=0 target=0 path=acquire err=%d\n",
                        (unsigned long long) g_drawFrame, (int) ar);
                return false;
            }
            o.bbHeld = true;
            o.bbLoad = false;
        }
        g_bbOpen = 1;
        fprintf(stderr, "[vk] bbframe open f=%llu ok=1 target=0 path=acquire img=%u load=%d\n",
                (unsigned long long) g_drawFrame, o.frameImageIndex, o.bbLoad ? 1 : 0);
    } else {
        uint32_t ti = id - 1000;
        if (ti >= o.targets.size() || !o.targets[ti].used) {
            fprintf(stderr, "[vk] bbframe open f=%llu ok=0 target=%u path=offscreen\n",
                    (unsigned long long) g_drawFrame, id);
            return false;
        }
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

// 批处理路径的离屏清屏：gl_vk_handle_clear 真的执行过 glClear 时打标，
// EndFrame 里改用 loadOp=CLEAR 的 pass 并按这个颜色清（apitrace 实测原版每帧清 (0,0,0,0)）。
void VkRenderer::RequestClear(uint32_t id, float r, float g, float b, float a) {
    Impl& o = *impl;
    if (id < 1000) return;
    uint32_t ti = id - 1000;
    if (ti >= o.targets.size() || !o.targets[ti].used) return;
    o.targets[ti].clearPending = true;
    o.targets[ti].pcR = r;
    o.targets[ti].pcG = g;
    o.targets[ti].pcB = b;
    o.targets[ti].pcA = a;
}

// ---------------------------------------------------------------- packet 执行路径
//
// 与批处理完全独立：进目标时就开 render pass 并 begin 命令缓冲；每笔 draw 立刻把
// bind + vkCmdDraw* 写进这块缓冲（不逐笔 vkQueueSubmit）；出目标时 end pass + 提交一次。
// 批处理路径的 DrawRaw / PresentTarget / EndFrame 一行未改，靠 ExecActive 开关分流。

bool VkRenderer::PktTargetOpen() const { return impl->pktOpen; }

bool VkRenderer::PktEnsureBuffers() {
    Impl& o = *impl;
    if (o.pktVb != VK_NULL_HANDLE && o.pktIb != VK_NULL_HANDLE) return true;
    auto getMemProps = (PFN_vkGetPhysicalDeviceMemoryProperties)g_gipa(o.inst, "vkGetPhysicalDeviceMemoryProperties");
    VkPhysicalDeviceMemoryProperties mp{};
    getMemProps(o.phys, &mp);
    auto mk = [&](VkDeviceSize bytes, VkBufferUsageFlags usage,
                  VkBuffer* buf, VkDeviceMemory* mem, uint8_t** ptr, VkDeviceSize* cap) -> bool {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = bytes;
        bci.usage = usage;
        if (g_createBuf(o.dev, &bci, nullptr, buf) != VK_SUCCESS) return false;
        VkMemoryRequirements mr{};
        g_getBufReqs(o.dev, *buf, &mr);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex = FindMemType(mp, mr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (g_allocMem(o.dev, &mai, nullptr, mem) != VK_SUCCESS) return false;
        g_bindBufMem(o.dev, *buf, *mem, 0);
        void* p = nullptr;
        g_mapMem(o.dev, *mem, 0, mr.size, 0, &p);
        if (!p) return false;
        *ptr = (uint8_t*)p;
        *cap = bytes;
        return true;
    };
    // 每 pass 重置偏移，一帧几何量按 4MB 顶点 / 1MB 索引估计
    if (!mk((VkDeviceSize)4u << 20, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            &o.pktVb, &o.pktVbMem, &o.pktVbPtr, &o.pktVbBytes)) {
        o.lastError = "pkt vb failed";
        return false;
    }
    if (!mk((VkDeviceSize)1u << 20, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
            &o.pktIb, &o.pktIbMem, &o.pktIbPtr, &o.pktIbBytes)) {
        o.lastError = "pkt ib failed";
        return false;
    }
    fprintf(stderr, "[pkt] buffers vb=%llu ib=%llu\n",
            (unsigned long long)o.pktVbBytes, (unsigned long long)o.pktIbBytes);
    return true;
}

// ===================== P2：采样器 / descriptor set / 管线变体 =====================

// 采样器组合键：4 个 4 bit 字段（已解析过的值，合法范围 0..5）
static uint16_t PktSamplerKey (const PktSamplerState& ss) {
    return (uint16_t)((ss.minFilter & 0x0Fu) | ((ss.magFilter & 0x0Fu) << 4) |
                      ((ss.wrapU & 0x0Fu) << 8) | ((ss.wrapV & 0x0Fu) << 12));
}

// 全 0xFF = 记录里没带采样器信息 → 沿用该纹理现有的 descriptor set（旧行为，不动画面）
static bool PktSamplerUnset (const PktSamplerState& ss) {
    return ss.minFilter == 0xFF && ss.magFilter == 0xFF &&
           ss.wrapU == 0xFF && ss.wrapV == 0xFF;
}

// 逐字段解析：未指定（0xFF）按"加调参前写死的那套"补齐 —— NEAREST + CLAMP_TO_EDGE。
// 这与纹理从未被 texParameteri 过时的实际采样方式一致（那时走 o.sampler）。
static void PktResolveSampler (const PktSamplerState& in, PktSamplerState& out) {
    out = in;
    if (out.minFilter == 0xFF) out.minFilter = 0;
    if (out.magFilter == 0xFF) out.magFilter = 0;
    if (out.wrapU == 0xFF) out.wrapU = 0;
    if (out.wrapV == 0xFF) out.wrapV = 0;
}

static VkSamplerAddressMode PktWrapToVk (uint8_t wrap) {
    switch (wrap) {
        case 1: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case 2: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}

static const char* PktFilterName (uint8_t f) {
    switch (f) {
        case 1: return "linear";
        case 2: return "nearest_mipmap_nearest";
        case 3: return "linear_mipmap_nearest";
        case 4: return "nearest_mipmap_linear";
        case 5: return "linear_mipmap_linear";
        default: return "nearest";
    }
}

static const char* PktWrapName (uint8_t w) {
    switch (w) {
        case 1: return "repeat";
        case 2: return "mirror";
        default: return "clamp";
    }
}

// 归一化混合因子 -> VkBlendFactor
static VkBlendFactor PktBlendToVk (uint8_t f) {
    switch (f) {
        case PKT_BLEND_ONE: return VK_BLEND_FACTOR_ONE;
        case PKT_BLEND_ZERO: return VK_BLEND_FACTOR_ZERO;
        case PKT_BLEND_ONE_MINUS_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case PKT_BLEND_DST_ALPHA: return VK_BLEND_FACTOR_DST_ALPHA;
        case PKT_BLEND_ONE_MINUS_DST_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case PKT_BLEND_SRC_COLOR: return VK_BLEND_FACTOR_SRC_COLOR;
        case PKT_BLEND_ONE_MINUS_SRC_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case PKT_BLEND_DST_COLOR: return VK_BLEND_FACTOR_DST_COLOR;
        case PKT_BLEND_ONE_MINUS_DST_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case PKT_BLEND_CONST_ALPHA: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
        case PKT_BLEND_ONE_MINUS_CONST_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        default: return VK_BLEND_FACTOR_SRC_ALPHA;
    }
}

static VkCullModeFlags PktCullToVk (uint8_t c) {
    switch (c) {
        case 1: return VK_CULL_MODE_BACK_BIT;
        case 2: return VK_CULL_MODE_FRONT_BIT;
        case 3: return VK_CULL_MODE_FRONT_AND_BACK;
        default: return VK_CULL_MODE_NONE;
    }
}

// "默认状态"的混合取自 vk_tuning 的 blend 变体 —— 它与 Init 建首条管线时用的是同一份配置，
// 所以只要 packet 状态等于它，就能安全地复用 o.pipeline（画面与现在完全一致）。
static void PktDefaultBlend (uint8_t& en, uint8_t& src, uint8_t& dst) {
    switch (vktuning::Get().blendVariant) {
        case vktuning::BLEND_OFF:
            en = 0; src = PKT_BLEND_ONE; dst = PKT_BLEND_ZERO; break;
        case vktuning::BLEND_PREMUL:
            en = 1; src = PKT_BLEND_ONE; dst = PKT_BLEND_ONE_MINUS_SRC_ALPHA; break;
        case vktuning::BLEND_ADDITIVE:
            en = 1; src = PKT_BLEND_SRC_ALPHA; dst = PKT_BLEND_ONE; break;
        default:
            en = 1; src = PKT_BLEND_SRC_ALPHA; dst = PKT_BLEND_ONE_MINUS_SRC_ALPHA; break;
    }
}

// 管线变体键：混合(9 bit) + 剔除(2) + 绕序(1) + 颜色掩码(4)
static uint32_t PktPipeKey (uint8_t en, uint8_t src, uint8_t dst,
                            uint8_t cull, uint8_t ff, uint8_t cm) {
    return (uint32_t)(en & 0x1) |
           ((uint32_t)(src & 0x0F) << 1) |
           ((uint32_t)(dst & 0x0F) << 5) |
           ((uint32_t)(cull & 0x3) << 9) |
           ((uint32_t)(ff & 0x1) << 11) |
           ((uint32_t)(cm & 0x0F) << 12);
}

void* VkRenderer::PktPipelineFor(const PktPipelineState& st) {
    Impl& o = *impl;
    uint8_t defEn = 1, defSrc = PKT_BLEND_SRC_ALPHA, defDst = PKT_BLEND_ONE_MINUS_SRC_ALPHA;
    PktDefaultBlend(defEn, defSrc, defDst);

    uint8_t en = st.blendEnabled ? 1 : 0;
    uint8_t src = defSrc, dst = defDst;
    if (en) {
        if (st.blendSrc != PKT_BLEND_UNSET) src = st.blendSrc;
        if (st.blendDst != PKT_BLEND_UNSET) dst = st.blendDst;
    }
    // [vkfix 2026-11-11] 兼容位保留：批处理里的半透明内容实测与原版一致（长条 alpha 0.6、
    // 预乘混合、图集 UV 均正确），不要再把 SRC_ALPHA 改写成 ONE——那会让长条/溅射显得更实。
    uint8_t cull = (st.cullMode > 3) ? 0 : st.cullMode;
    uint8_t ff = st.frontFace ? 1 : 0;
    uint8_t cm = (uint8_t)(st.colorMask & 0x0F);

    // 兜底：与 Init 建的那条管线状态一致时直接复用，不新建任何东西
    if (en == defEn && cull == 0 && ff == 0 && cm == 0x0F &&
        (!en || (src == defSrc && dst == defDst))) {
        return (void*)(uintptr_t)o.pipeline;
    }

    uint32_t key = PktPipeKey(en, src, dst, cull, ff, cm);
    for (auto& e : o.pktPipes) if (e.key == key) return (void*)(uintptr_t)e.pipe;

    // ---- 以下与 Init 里建首条管线的字段逐项一致（改那边要同步这里）----
    VkVertexInputBindingDescription vbind[2]{};
    vbind[0].binding = 0;
    vbind[0].stride = sizeof(float) * 15;
    vbind[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    vbind[1].binding = 1;
    vbind[1].stride = 0;
    vbind[1].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription vattr[8]{};
    vattr[0].location = 0; vattr[0].binding = 0;
    vattr[0].format = VK_FORMAT_R32G32B32A32_SFLOAT; vattr[0].offset = 0;
    vattr[1].location = 1; vattr[1].binding = 0;
    vattr[1].format = VK_FORMAT_R32_SFLOAT; vattr[1].offset = sizeof(float) * 4;
    vattr[2].location = 2; vattr[2].binding = 0;
    vattr[2].format = VK_FORMAT_R32G32B32A32_SFLOAT; vattr[2].offset = sizeof(float) * 5;
    vattr[3].location = 3; vattr[3].binding = 0;
    vattr[3].format = VK_FORMAT_R32G32B32A32_SFLOAT; vattr[3].offset = sizeof(float) * 9;
    vattr[4].location = 4; vattr[4].binding = 0;
    vattr[4].format = VK_FORMAT_R32G32_SFLOAT; vattr[4].offset = sizeof(float) * 13;
    vattr[5].location = 5; vattr[5].binding = 1;
    vattr[5].format = VK_FORMAT_R32_SFLOAT; vattr[5].offset = 0;
    vattr[6].location = 6; vattr[6].binding = 1;
    vattr[6].format = VK_FORMAT_R32G32B32A32_SFLOAT; vattr[6].offset = sizeof(float) * 1;
    vattr[7].location = 7; vattr[7].binding = 1;
    vattr[7].format = VK_FORMAT_R32G32B32A32_SFLOAT; vattr[7].offset = sizeof(float) * 5;

    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 2;
    vi.pVertexBindingDescriptions = vbind;
    vi.vertexAttributeDescriptionCount = 8;
    vi.pVertexAttributeDescriptions = vattr;

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = 1;
    vps.pViewports = nullptr;
    vps.scissorCount = 1;
    vps.pScissors = nullptr;
    VkDynamicState dynStates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo pds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    pds.dynamicStateCount = 2;
    pds.pDynamicStates = dynStates;

    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = PktCullToVk(cull);
    rs.frontFace = ff ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState cba{};
    cba.blendEnable = en ? VK_TRUE : VK_FALSE;
    cba.srcColorBlendFactor = PktBlendToVk(src);
    cba.dstColorBlendFactor = PktBlendToVk(dst);
    // GL 的 glBlendFunc 同一对因子同时作用于 RGB 与 A，这里按同样的语义填
    cba.srcAlphaBlendFactor = PktBlendToVk(src);
    cba.dstAlphaBlendFactor = PktBlendToVk(dst);
    cba.colorBlendOp = VK_BLEND_OP_ADD;
    cba.alphaBlendOp = VK_BLEND_OP_ADD;
    cba.colorWriteMask = (VkColorComponentFlags)(
        ((cm & 1) ? VK_COLOR_COMPONENT_R_BIT : 0) |
        ((cm & 2) ? VK_COLOR_COMPONENT_G_BIT : 0) |
        ((cm & 4) ? VK_COLOR_COMPONENT_B_BIT : 0) |
        ((cm & 8) ? VK_COLOR_COMPONENT_A_BIT : 0));
    VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cbs.attachmentCount = 1;
    cbs.pAttachments = &cba;

    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = o.vsMod;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = o.fsMod;
    stages[1].pName = "main";
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vps;
    gp.pDynamicState = &pds;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cbs;
    gp.layout = o.pipeLayout;
    gp.renderPass = o.renderPass;
    gp.subpass = 0;

    VkPipeline pipe = VK_NULL_HANDLE;
    if (g_createPipe(o.dev, VK_NULL_HANDLE, 1, &gp, nullptr, &pipe) != VK_SUCCESS || !pipe) {
        // 建不出来就退回默认管线（宁可状态不准也不黑屏）
        fprintf(stderr, "[pkt] warn=pipe_variant_failed key=0x%x fallback=default\n", key);
        return (void*)(uintptr_t)o.pipeline;
    }
    o.pktPipes.push_back({key, pipe});
    o.pktPipeCount++;
    fprintf(stderr, "[pkt] pipe variant n=%u key=0x%x blend=%d(%s,%s) cull=%d ff=%d cm=0x%x\n",
            o.pktPipeCount, key, (int)en, PktBlendToVk(src) == VK_BLEND_FACTOR_SRC_ALPHA ? "srcalpha" : "other",
            PktBlendToVk(dst) == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA ? "1-srcalpha" : "other",
            (int)cull, (int)ff, (unsigned)cm);
    return (void*)(uintptr_t)pipe;
}

void* VkRenderer::PktSamplerFor(const PktSamplerState& ss) {
    Impl& o = *impl;
    PktSamplerState r;
    PktResolveSampler(ss, r);
    uint16_t key = PktSamplerKey(r);
    for (auto& e : o.pktSamplers) if (e.key == key) return (void*)(uintptr_t)e.smp;

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    // min 带 mip 的几种（2..5）在只有 1 级 mip 的情况下等价于最近/线性过滤
    sci.minFilter = (r.minFilter == 1 || r.minFilter == 3 || r.minFilter == 5)
                    ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sci.magFilter = (r.magFilter == 1) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = PktWrapToVk(r.wrapU);
    sci.addressModeV = PktWrapToVk(r.wrapV);
    sci.addressModeW = sci.addressModeV;
    sci.minLod = 0;
    // 纹理只分配 1 级 mip，maxLod 必须钳 0，否则取到不存在的 level 会黑边/全黑
    //（与 SetTextureSampler 的口径一致）
    sci.maxLod = 0;
    sci.maxAnisotropy = 1;
    VkSampler smp = VK_NULL_HANDLE;
    if (g_createSampler(o.dev, &sci, nullptr, &smp) != VK_SUCCESS || !smp) {
        fprintf(stderr, "[sampler] err=create key=0x%x\n", key);
        return (void*)(uintptr_t)VK_NULL_HANDLE;
    }
    o.pktSamplers.push_back({key, smp});
    o.pktSamplerCount++;
    fprintf(stderr, "[sampler] filter=%s/%s wrapU=%s wrapV=%s mip=%s(maxLod=0) key=0x%x n=%u\n",
            PktFilterName(r.minFilter), PktFilterName(r.magFilter),
            PktWrapName(r.wrapU), PktWrapName(r.wrapV),
            (r.minFilter >= 2) ? "nearest" : "none", key, o.pktSamplerCount);
    return (void*)(uintptr_t)smp;
}

void* VkRenderer::PktSetFor(uint32_t texId, const PktSamplerState& ss) {
    Impl& o = *impl;
    VkImageView view = VK_NULL_HANDLE;
    VkDescriptorSet base = VK_NULL_HANDLE;
    if (texId >= 1000) {
        uint32_t ti = texId - 1000;
        if (ti < o.targets.size() && o.targets[ti].used) {
            view = o.targets[ti].view;
            base = o.targets[ti].set;
        }
        // 离屏目标在当前路径统一用 o.sampler（ApplySampler 也跳过 id>=1000），
        // 这里返回现有 set，保证合成画面与调参前一致。
        return (void*)(uintptr_t)base;
    }
    if (texId == 0 || texId > o.textures.size()) return (void*)(uintptr_t)VK_NULL_HANDLE;
    TexSlot& t = o.textures[texId - 1];
    if (!t.used) return (void*)(uintptr_t)VK_NULL_HANDLE;
    view = t.view;
    base = t.set;

    if (PktSamplerUnset(ss)) return (void*)(uintptr_t)base;   // 记录里没带采样器 → 旧行为
    if (o.pktSetPool == VK_NULL_HANDLE) return (void*)(uintptr_t)base;
    // 若 vk_tuning 里配了过滤/wrap 覆盖，就沿用 SetTextureSampler 已合成好的采样器：
    // 那条路径把 ini 规则算进去了，逐笔再建一套会让两条路径的采样规则打架。
    {
        const vktuning::Values& tv = vktuning::Get();
        bool tuned = (tv.filterDefault != vktuning::FILTER_GL || tv.filterRuleCount > 0 ||
                      tv.filterByNameCount > 0 || tv.wrapDefault != vktuning::WRAP_GL ||
                      tv.wrapRuleCount > 0);
        if (tuned) {
            static bool s_logged = false;
            if (!s_logged) { s_logged = true; fprintf(stderr, "[sampler] skip=pkt_variant_use_vk_tuning\n"); }
            return (void*)(uintptr_t)base;
        }
    }

    PktSamplerState r;
    PktResolveSampler(ss, r);
    uint16_t key = PktSamplerKey(r);
    for (auto& e : o.pktSets)
        if (e.texId == texId && e.key == key) return (void*)(uintptr_t)e.set;

    VkSampler smp = (VkSampler)(uintptr_t)PktSamplerFor(ss);
    if (!smp) return (void*)(uintptr_t)base;

    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = o.pktSetPool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &o.dsl;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (g_allocDS(o.dev, &dsai, &set) != VK_SUCCESS || !set) {
        fprintf(stderr, "[sampler] err=ds_full tex=%u key=0x%x fallback=base\n", texId, key);
        return (void*)(uintptr_t)base;
    }

    VkDescriptorImageInfo dii{};
    dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    dii.imageView = view;
    dii.sampler = smp;
    VkDescriptorBufferInfo dbi{};
    dbi.buffer = o.ubo;
    dbi.offset = 0;
    dbi.range = 256;
    VkWriteDescriptorSet wds[2]{};
    wds[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wds[0].dstSet = set;
    wds[0].dstBinding = 0;
    wds[0].descriptorCount = 1;
    wds[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    wds[0].pBufferInfo = &dbi;
    wds[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wds[1].dstSet = set;
    wds[1].dstBinding = 1;
    wds[1].descriptorCount = 1;
    wds[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wds[1].pImageInfo = &dii;
    g_updateDS(o.dev, 2, wds, 0, nullptr);

    o.pktSets.push_back({texId, key, set});
    o.pktSetCount++;
    fprintf(stderr, "[sampler] set tex=%u key=0x%x n=%u\n", texId, key, o.pktSetCount);
    return (void*)(uintptr_t)set;
}

void VkRenderer::PktDropSets(uint32_t texId) {
    Impl& o = *impl;
    for (size_t i = 0; i < o.pktSets.size(); ) {
        if (o.pktSets[i].texId == texId) {
            if (g_freeDS && o.pktSetPool != VK_NULL_HANDLE)
                g_freeDS(o.dev, o.pktSetPool, 1, &o.pktSets[i].set);
            o.pktSets[i] = o.pktSets.back();
            o.pktSets.pop_back();
        } else {
            i++;
        }
    }
}

// trace（默认关闭）：VK_TRACE_DRAW=1 打开；预算 VK_TRACE_MAX（默认 64）**按帧**计。
// 只读诊断：回答"这次 begin 的 clear 到底有没有真的执行"。
static bool TraceClearWant () {
    static int on = -1;
    static int mx = 64;
    static uint64_t gen = 0;
    static int used = 0;
    if (on < 0) {
        const char* v = getenv("VK_TRACE_DRAW");
        on = (v && v[0] == '1') ? 1 : 0;
        const char* vm = getenv("VK_TRACE_MAX");
        if (vm && vm[0]) mx = atoi(vm);
    }
    if (!on) return false;
    // 按帧单独计数：clear 行不再吃"整局一次"的预算，菜单/后期帧也能拿到自己的 clear 行
    if (gen != g_drawFrame) { gen = g_drawFrame; used = 0; }
    if (used >= mx) return false;
    used++;
    return true;
}

// 离屏目标 clear（image 路径）：在 render pass 之外整幅清色。
// 为什么不会破坏"保留语义"：只有收到明确的 clear 包（colorClear=true）或
// offscreen_rebind_clear 打开的"同目标重绑 clear"才调用；普通绑定仍走 LOAD pass。
// 旧布局必须按记录的真实状态给（见 RenderTarget::layout）：创建时是 UNDEFINED
// （srcAccess=0 / TOP_OF_PIPE），pass 结束后是 SHADER_READ_ONLY_OPTIMAL
// （srcAccess=SHADER_READ / FRAGMENT_SHADER）。以前写死 SRO，第一次 clear 撞上
// UNDEFINED 会直接 0xc0000005（nvoglv64.dll）。
static void ClearOffscreenImage (VkCommandBuffer cmd, VkImage img, VkImageLayout& layout,
                                 float r, float g, float b, float a) {
    if (!g_cmdBarrier || !g_cmdClearColorImage || !img) return;
    // oldLayout 按记录的真实布局给：UNDEFINED 时 srcAccess=0 + TOP_OF_PIPE（驱动不读旧内容）；
    // 其余（正常是 SHADER_READ_ONLY_OPTIMAL）按 shader read 过渡。写死任何一种都会崩 nvoglv64。
    bool fromUndefined = (layout == VK_IMAGE_LAYOUT_UNDEFINED);
    VkImageMemoryBarrier bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    bar.oldLayout = layout;
    bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.image = img;
    bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    bar.srcAccessMask = fromUndefined ? 0 : VK_ACCESS_SHADER_READ_BIT;
    bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    g_cmdBarrier(cmd,
                 fromUndefined ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                 0, 1, nullptr, 0, nullptr, 1, &bar);

    VkClearColorValue cc{};
    cc.float32[0] = r; cc.float32[1] = g; cc.float32[2] = b; cc.float32[3] = a;
    VkImageSubresourceRange rng{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    // 崩点定位用：fprintf 是同步的，若 nvoglv64 在这条调用里 AV，日志只会停在 pre 这行。
    fprintf(stderr, "[imgcmd] clear pre img=%p layout=%d\n", (void*)img, (int)bar.oldLayout);
    g_cmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &rng);
    fprintf(stderr, "[imgcmd] clear post\n");

    VkImageMemoryBarrier back = bar;
    back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    back.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    back.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    back.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    g_cmdBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
                 0, 1, nullptr, 0, nullptr, 1, &back);
    layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;   // 清完回到可采样布局
}

static void TraceClearLog (uint32_t id, float r, float g, float b, float a,
                           int applied, const char* why, uint8_t mask) {
    if (!TraceClearWant()) return;
    fprintf(stderr, "[trace] clear= target=%u mask=%u rgb=(%.3f,%.3f,%.3f,%.3f) applied=%d why=%s loadOp=%s\n",
            id, (unsigned)mask, r, g, b, a, applied, why,
            (why && (!strcmp(why, "rp_clear_applied") || !strcmp(why, "clear_loadop"))) ? "clear" : "load");
}

bool VkRenderer::PktTargetClearedThisFrame(uint32_t id) {
    Impl& o = *impl;
    if (id < 1000) return false;
    uint32_t ti = id - 1000;
    if (ti >= o.targets.size() || !o.targets[ti].used) return false;
    return o.targets[ti].cleared && o.targets[ti].clearedFrame == g_drawFrame;
}

bool VkRenderer::PktBeginTarget(uint32_t id, float r, float g, float b, float a,
                                bool colorClear, uint8_t clearMask) {
    Impl& o = *impl;
    const vktuning::Values& tv0 = vktuning::Get();
    // 只有"明确的 clear 包 + 离屏目标 + 开关未关"才真的清；普通绑定保持 LOAD 语义
    bool wantImageClear = colorClear && id >= 1000 && tv0.offscreenClear == vktuning::CLEAR_IMAGE;
    bool wantLoadClear = colorClear && id >= 1000 && tv0.offscreenClear == vktuning::CLEAR_LOADOP;
    // 诊断路径：vkCmdClearColorImage（nvoglv64 上 0xc0000005，正式跑不用，仅 imgcmd 模式可达）
    bool wantImgCmdClear = colorClear && id >= 1000 && tv0.offscreenClear == vktuning::CLEAR_IMGCMD;
    // 同目标重绑时的"真清除"：OpenFL 每次 glClear 都会再 Bind，之前这类 clear 全被丢弃，
    // 画布永远不清。独立开关，不依赖 offscreen_clear（避开 image 那条路径的崩溃绑定）。
    bool wantRebindClear = (clearMask & 1) && id >= 1000
                           && tv0.offscreenRebindClear == vktuning::ON;
    // 要清就走"loadOp=CLEAR 的整幅 pass"（不碰 vkCmdClearColorImage）：同一 pass 内先清后画，
    // 颜色取包里 rgb，等价于整幅替换，且不依赖任何图像布局记录。
    bool wantPassClear = wantImageClear || wantLoadClear || wantRebindClear;
    // 预览开关 offscreen_loadop=clear：离屏目标每次绑定的 pass 都用 CLEAR（清成黑）
    if (id >= 1000 && vktuning::Get().offscreenLoadop == vktuning::LOADOP_CLEAR) wantPassClear = true;

    // 同一目标重复绑定（同上）
    if (o.pktOpen && o.pktTarget == id) {
        o.clearR = r; o.clearG = g; o.clearB = b; o.clearA = a;
        if (!wantPassClear && !wantImgCmdClear) {
            TraceClearLog(id, r, g, b, a, 0, "same_target_rebind", clearMask);
            return true;
        }
        // glClear 是整幅生效：先收尾当前 pass（旧内容本就要丢弃），再按 clear 语义重开
        PktEndTarget();
    } else if (o.pktOpen) {
        PktEndTarget();
    }
    if (!PktEnsureBuffers()) return false;
    o.pktVbOff = 0;
    o.pktIbOff = 0;
    o.pktDraws = 0;

    VkFramebuffer fb = VK_NULL_HANDLE;
    VkRenderPass rp = VK_NULL_HANDLE;
    VkExtent2D ext = o.extent;
    uint32_t ti = 0;
    bool usedClearPass = false;
    if (id >= 1000) {
        ti = id - 1000;
        if (ti >= o.targets.size() || !o.targets[ti].used) return false;
        if (wantPassClear && o.targets[ti].rpClear && o.targets[ti].fbClear) {
            fb = o.targets[ti].fbClear;
            rp = o.targets[ti].rpClear;
            usedClearPass = true;
        } else {
            fb = o.targets[ti].fb;
            rp = o.targets[ti].rp;
        }
        ext = {(uint32_t)o.targets[ti].w, (uint32_t)o.targets[ti].h};
    } else {
        if (o.swapDirty) {
            if (RecreateSwapchain()) o.swapDirty = false;
            else return false;
        }
        VkResult ar = g_acquire(o.dev, o.swapchain, UINT64_MAX, VK_NULL_HANDLE, VK_NULL_HANDLE,
                                &o.frameImageIndex);
        if (ar == VK_SUBOPTIMAL_KHR && WindowSizeMatchesExtent() && !InResizeAnimation()) o.swapDirty = true;
        if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) {
            if (!InResizeAnimation()) o.swapDirty = true;
            fprintf(stderr, "[pkt] begin target=0 ok=0 acquire_err=%d\n", (int)ar);
            TraceClearLog(0, r, g, b, a, 0, "acquire_err", clearMask);
            return false;
        }
        o.bbHeld = true;
        o.bbLoad = false;
        fb = o.framebuffers[o.frameImageIndex];
        rp = o.renderPass;
    }
    if (!fb || !rp) {
        fprintf(stderr, "[pkt] begin target=%u ok=0 fb=%p rp=%p\n", id, (void*)fb, (void*)rp);
        return false;
    }

    // 帧环：取当前槽（等它上一次提交完成）
    AcquireSlot();
    g_resetCB(o.cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VkResult br = g_beginCB(o.cmd, &bi);
    if (br != VK_SUCCESS) {
        fprintf(stderr, "[pkt] begin target=%u ok=0 beginCB=%d\n", id, (int)br);
        return false;
    }

    // 投影矩阵按当前目标尺寸重算（与批处理同一个 UBO，着色器读的就是它）
    if (o.uboPtr) {
        memset(o.uboPtr, 0, 64);
        o.uboPtr[0] = 2.0f / (float)ext.width;
        o.uboPtr[5] = 2.0f / (float)ext.height;
        o.uboPtr[10] = 1.0f;
        o.uboPtr[12] = -1.0f;
        o.uboPtr[13] = -1.0f;
        o.uboPtr[15] = 1.0f;
    }

    // 诊断路径（imgcmd）：在 pass 之外把整幅目标清成 clear 包的颜色（layout 往返见 ClearOffscreenImage）
    if (wantImgCmdClear && !usedClearPass) {
        ClearOffscreenImage(o.cmd, o.targets[ti].image, o.targets[ti].layout, r, g, b, a);
    }

    VkClearValue cv{};
    cv.color.float32[0] = r;
    cv.color.float32[1] = g;
    cv.color.float32[2] = b;
    cv.color.float32[3] = a;
    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = rp;
    rbi.framebuffer = fb;
    rbi.renderArea.offset = {0, 0};
    rbi.renderArea.extent = ext;
    rbi.clearValueCount = 1;
    rbi.pClearValues = &cv;
    g_beginRP(o.cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    if (id < 1000) {
        TraceClearLog(id, r, g, b, a, 1, "rp_clear_applied", clearMask);
    } else if (usedClearPass) {
        o.targets[ti].cleared = true;
        o.targets[ti].clearedFrame = g_drawFrame;
        const char* why = wantRebindClear ? "rebind_clear"
                        : (wantLoadClear ? "clear_loadop" : "clear_image");
        TraceClearLog(id, r, g, b, a, 1, why, clearMask);
    } else if (wantImgCmdClear) {
        o.targets[ti].cleared = true;
        o.targets[ti].clearedFrame = g_drawFrame;
        TraceClearLog(id, r, g, b, a, 1, "clear_imagecmd", clearMask);
    } else {
        // 普通绑定（不是 clear 包）或 offscreen_clear=off：LOAD pass 保留上一 pass 内容
        TraceClearLog(id, r, g, b, a, 0, "rp_load_ignores_clear", clearMask);
    }

    // viewport/scissor 取"绑定这一刻"的 GL 状态（不是帧末）
    const vktuning::Values& tv = vktuning::Get();
    VkViewport vp;
    VkRect2D sc;
    if (tv.viewportPolicy == vktuning::POLICY_FULL_TARGET) {
        vp = {0, 0, (float)ext.width, (float)ext.height, 0, 1};
    } else {
        vp = lime::glstate::GetVulkanViewport((int)ext.height);
        if (vp.width <= 0.0f || vp.height <= 0.0f) vp = {0, 0, (float)ext.width, (float)ext.height, 0, 1};
    }
    if (tv.scissorPolicy == vktuning::POLICY_FULL_TARGET) {
        sc = {{0, 0}, ext};
    } else {
        sc = lime::glstate::GetVulkanScissor((int)ext.width, (int)ext.height);
        if (sc.extent.width == 0 || sc.extent.height == 0) sc = {{0, 0}, ext};
    }
    g_setViewport(o.cmd, 0, 1, &vp);
    g_setScissor(o.cmd, 0, 1, &sc);

    o.pktOpen = true;
    o.pktTarget = id;
    o.pktTargetExt = ext;
    fprintf(stderr, "[pkt] begin target=%u rp=%p fb=%p ext=%ux%u vp=%d,%d,%d,%d sc=%d,%d,%d,%d clear=%.2f,%.2f,%.2f,%.2f policy=vp:%s,sc:%s\n",
            id, (void*)rp, (void*)fb, ext.width, ext.height,
            (int)vp.x, (int)vp.y, (int)vp.width, (int)vp.height,
            (int)sc.offset.x, (int)sc.offset.y, (int)sc.extent.width, (int)sc.extent.height,
            r, g, b, a,
            tv.viewportPolicy == vktuning::POLICY_FULL_TARGET ? "full" : "keep",
            tv.scissorPolicy == vktuning::POLICY_FULL_TARGET ? "full" : "keep");
    return true;
}

bool VkRenderer::PktDraw(const PktDrawDesc& d) {
    Impl& o = *impl;
    if (!o.pktOpen) return false;
    if (!d.verts || d.vertexCount <= 0) return false;

    VkDescriptorSet set = (VkDescriptorSet)(uintptr_t)PktSetFor(d.texId, d.sampler);
    if (!set) return false;
    NoteSampled(d.texId);   // 取证：本帧真正被采样的纹理/目标

    VkDeviceSize vbytes = (VkDeviceSize)d.vertexCount * 15u * sizeof(float);
    if (o.pktVbOff + vbytes > o.pktVbBytes) {
        fprintf(stderr, "[pkt] err=vb_full need=%llu cap=%llu n=%d\n",
                (unsigned long long)vbytes, (unsigned long long)o.pktVbBytes, d.vertexCount);
        return false;
    }
    memcpy(o.pktVbPtr + o.pktVbOff, d.verts, (size_t)vbytes);
    VkDeviceSize voff = o.pktVbOff;
    o.pktVbOff += vbytes;

    // [diag 2026-11-11] 长条(sustain)取证：细高四边形单独打一条，
    // 记录纹理/UV/顶点 alpha/颜色调制/混合，用来定位"中间空心"。
    {
        static int s_tallPkt = 0;
        if (s_tallPkt < 400 && d.vertexCount >= 6) {
            const float* vp = d.verts;
            float x0 = vp[0], x1 = vp[0], y0 = vp[1], y1 = vp[1];
            float u0 = vp[13], u1 = vp[13], v0 = vp[14], v1 = vp[14];
            for (int i = 1; i < d.vertexCount; i++) {
                const float* q = vp + (size_t)i * 15;
                if (q[0] < x0) x0 = q[0]; if (q[0] > x1) x1 = q[0];
                if (q[1] < y0) y0 = q[1]; if (q[1] > y1) y1 = q[1];
                if (q[13] < u0) u0 = q[13]; if (q[13] > u1) u1 = q[13];
                if (q[14] < v0) v0 = q[14]; if (q[14] > v1) v1 = q[14];
            }
            float w = x1 - x0, h = y1 - y0;
            if (h > 100.0f && w < 80.0f && w > 0.5f) {
                s_tallPkt++;
                fprintf(stderr,
                        "[tallpkt] tex=%u n=%d pos=[%.1f..%.1f, %.1f..%.1f] uv=[%.4f..%.4f, %.4f..%.4f] "
                        "a=%.3f cm=[%.2f %.2f %.2f %.2f] cf=[%.1f %.1f %.1f %.1f] blend=%d,%d,%d\n",
                        (unsigned)d.texId, d.vertexCount, x0, x1, y0, y1, u0, u1, v0, v1,
                        vp[4], vp[5], vp[6], vp[7], vp[8], vp[9], vp[10], vp[11], vp[12],
                        d.state.blendEnabled ? 1 : 0, (int)d.state.blendSrc, (int)d.state.blendDst);
            }
        }
    }

    bool indexed = ((d.indices != nullptr || d.indices32 != nullptr) && d.indexCount > 0);
    bool use32 = (d.index32 != 0 && d.indices32 != nullptr);
    VkDeviceSize ioff = 0;
    if (indexed) {
        VkDeviceSize isz = use32 ? sizeof(uint32_t) : sizeof(uint16_t);
        VkDeviceSize ibytes = (VkDeviceSize)d.indexCount * isz;
        if (o.pktIbOff + ibytes > o.pktIbBytes) {
            // 32 位索引装不下时退回 16 位（顶点已按展开顺序排列，<=65535 时等价）
            if (use32 && d.indexCount <= 65535) {
                fprintf(stderr, "[pkt] warn=ib_full_32 fallback16 n=%d\n", d.indexCount);
                use32 = false;
                ibytes = (VkDeviceSize)d.indexCount * sizeof(uint16_t);
            }
            if (o.pktIbOff + ibytes > o.pktIbBytes) {
                fprintf(stderr, "[pkt] err=ib_full need=%llu cap=%llu n=%d\n",
                        (unsigned long long)ibytes, (unsigned long long)o.pktIbBytes, d.indexCount);
                return false;
            }
        }
        if (use32) {
            memcpy(o.pktIbPtr + o.pktIbOff, d.indices32, (size_t)((VkDeviceSize)d.indexCount * sizeof(uint32_t)));
        } else if (d.indices) {
            memcpy(o.pktIbPtr + o.pktIbOff, d.indices, (size_t)((VkDeviceSize)d.indexCount * sizeof(uint16_t)));
        } else {
            // 只有 32 位表却退回了 16 位：现场压一份 16 位
            uint16_t* dst = (uint16_t*)(o.pktIbPtr + o.pktIbOff);
            for (int i = 0; i < d.indexCount; i++) dst[i] = (uint16_t)d.indices32[i];
        }
        ioff = o.pktIbOff;
        o.pktIbOff += ibytes;
    }

    // viewport / scissor 逐笔：记录里带了有效矩形就按笔下发，否则沿用 pass 打开时的值。
    // GL 视口原点在左下，Vulkan 视口原点在左上：y 需要按 target 高度翻一次。
    if (d.viewport.valid && o.pktTargetExt.height > 0) {
        int th = (int)o.pktTargetExt.height;
        VkViewport dvp;
        dvp.x = (float)d.viewport.x;
        dvp.y = (float)(th - d.viewport.y - d.viewport.h);
        dvp.width = (float)d.viewport.w;
        dvp.height = (float)d.viewport.h;
        dvp.minDepth = 0.0f;
        dvp.maxDepth = 1.0f;
        g_setViewport(o.cmd, 0, 1, &dvp);
    }
    if (d.scissor.valid && o.pktTargetExt.height > 0) {
        int th = (int)o.pktTargetExt.height;
        VkRect2D dsc;
        dsc.offset.x = d.scissor.x;
        dsc.offset.y = th - d.scissor.y - d.scissor.h;
        if (dsc.offset.x < 0) dsc.offset.x = 0;
        if (dsc.offset.y < 0) dsc.offset.y = 0;
        dsc.extent.width = (uint32_t)d.scissor.w;
        dsc.extent.height = (uint32_t)d.scissor.h;
        g_setScissor(o.cmd, 0, 1, &dsc);
    }

    // 管线变体按状态选：默认状态复用 Init 建的那条（兜底），缺的变体才新建并缓存。
    VkPipeline pipe = (VkPipeline)(uintptr_t)PktPipelineFor(d.state);
    if (!pipe) return false;

    const vktuning::Values& tv = vktuning::Get();
    int effDepthTest = (tv.depthTest == vktuning::KEEP) ? d.state.depthTest : (tv.depthTest == vktuning::ON);
    int effDepthWrite = (tv.depthWrite == vktuning::KEEP) ? d.state.depthWrite : (tv.depthWrite == vktuning::ON);
    int effStencil = (tv.stencilTest == vktuning::KEEP) ? d.state.stencilTest : (tv.stencilTest == vktuning::ON);
    g_bindPipe(o.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    // binding0 是动态 UBO：传当前帧环槽位的偏移
    uint32_t pktDynOff = (uint32_t)o.slot * o.uboStride;
    g_bindDS(o.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, o.pipeLayout, 0, 1, &set, 1, &pktDynOff);
    VkBuffer vbufs[2] = {o.pktVb, o.attrBuf};
    VkDeviceSize voffs[2] = {voff, 0};
    g_bindVB(o.cmd, 0, 2, vbufs, voffs);
    if (indexed) {
        g_bindIB(o.cmd, o.pktIb, ioff, use32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
        g_cmdDrawIndexed(o.cmd, (uint32_t)d.indexCount, 1, 0, 0, 0);
    } else {
        g_cmdDraw(o.cmd, (uint32_t)d.vertexCount, 1, 0, 0);
    }
    o.pktDraws++;
    fprintf(stderr, "[pkt] draw tgt=%u i=%llu tex=%u set=%p pipe=%p cmd=%s n=%d vbOff=%llu ibOff=%llu "
            "state=blend%d(%u,%u) depth%d/%d stencil%d cull%d vp=%d sc=%d\n",
            o.pktTarget, (unsigned long long)o.pktDraws, d.texId, (void*)set, (void*)pipe,
            indexed ? (use32 ? "vkCmdDrawIndexed(u32)" : "vkCmdDrawIndexed(u16)") : "vkCmdDraw",
            indexed ? d.indexCount : d.vertexCount,
            (unsigned long long)voff, (unsigned long long)ioff,
            (int)d.state.blendEnabled, (unsigned)d.state.blendSrc, (unsigned)d.state.blendDst,
            effDepthTest, effDepthWrite, effStencil, (int)d.state.cullMode,
            d.viewport.valid ? 1 : 0, d.scissor.valid ? 1 : 0);
    return true;
}

void VkRenderer::PktEndTarget() {
    Impl& o = *impl;
    if (!o.pktOpen) return;
    uint32_t t = o.pktTarget;
    uint64_t draws = o.pktDraws;
    g_endRP(o.cmd);
    g_endCB(o.cmd);
    // pass 的 finalLayout 固定是 SRO；结束后图像的"真实布局"就是 SRO（给清除屏障用）
    if (t >= 1000) {
        uint32_t ti = t - 1000;
        if (ti < o.targets.size() && o.targets[ti].used)
            o.targets[ti].layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &o.cmd;
    g_submit(o.queue, 1, &si, VK_NULL_HANDLE);
    g_waitIdle(o.queue);
    // 帧环：packet 路径本身全同步，提交完直接轮转到下一个槽
    o.slot = (o.slot + 1) % VK_RING;

    // 离屏目标按批处理同样的口径记录"最后一个真正写过几何的目标"（合成选源用）
    if (t >= 1000 && draws > 0) o.lastGeomTarget = t;

    bool present = (t == 0 && o.presentReq);
    if (present) {
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.swapchainCount = 1;
        pi.pSwapchains = &o.swapchain;
        pi.pImageIndices = &o.frameImageIndex;
        VkResult pr = g_present(o.queue, &pi);
        if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) o.swapDirty = true;
        if (pr != VK_SUCCESS && pr != VK_SUBOPTIMAL_KHR) {
            fprintf(stderr, "[pkt] present failed r=%d img=%u\n", (int)pr, o.frameImageIndex);
        }
        o.bbHeld = false;
        o.presentReq = false;
    }
    fprintf(stderr, "[pkt] end target=%u draws=%llu present=%d img=%u\n",
            t, (unsigned long long)draws, present ? 1 : 0, o.frameImageIndex);
    o.pktOpen = false;
    // 取证转储：只在"真正 present 的那一笔"（帧末）读回，避免在一帧中间的 t==0 提前转、
    // 把后续采样（id<1000 的源贴图）漏掉。present=1 时 GPU 已 idle，正好读回。
    if (present) MaybeDumpTextures((uint64_t)lime::pkt::SnapshotFrameGen());
}

bool VkRenderer::PktComposite(uint32_t srcTargetId) {
    Impl& o = *impl;
    int tw = 0, th = 0;
    if (!GetRenderTargetSize(srcTargetId, &tw, &th) || tw <= 0 || th <= 0) return false;
    if (o.extent.width == 0 || o.extent.height == 0) return false;
    float W = (float)o.extent.width, H = (float)o.extent.height;
    static const float one[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    static const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    std::vector<float> q;
    q.reserve(6 * 15);
    PushLimeVertex(q, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, one, zero);
    PushLimeVertex(q, W,    0.0f, 1.0f, 0.0f, 1.0f, one, zero);
    PushLimeVertex(q, 0.0f, H,    0.0f, 1.0f, 1.0f, one, zero);
    PushLimeVertex(q, 0.0f, H,    0.0f, 1.0f, 1.0f, one, zero);
    PushLimeVertex(q, W,    0.0f, 1.0f, 0.0f, 1.0f, one, zero);
    PushLimeVertex(q, W,    H,    1.0f, 1.0f, 1.0f, one, zero);
    PktDrawDesc d{};
    d.texId = srcTargetId;
    d.verts = q.data();
    d.vertexCount = 6;
    d.indices = nullptr;
    d.indexCount = 0;
    d.state.blendEnabled = 1;
    // 合成沿用"默认混合"（与 Init 管线一致）；0 在新枚举里是 ONE，必须显式标未指定
    d.state.blendSrc = PKT_BLEND_UNSET;
    d.state.blendDst = PKT_BLEND_UNSET;
    d.state.depthTest = 0;
    d.state.depthWrite = 0;
    d.state.stencilTest = 0;
    d.state.cullMode = 0;
    d.state.frontFace = 0;
    d.state.scissorTest = 0;
    d.state.colorMask = 0xF;
    // 采样器/视口不逐笔覆盖：离屏目标是全程用 o.sampler，视口沿用 pass 打开时的整幅
    d.sampler.minFilter = 0xFF;
    d.sampler.magFilter = 0xFF;
    d.sampler.wrapU = 0xFF;
    d.sampler.wrapV = 0xFF;
    d.viewport.valid = 0;
    d.scissor.valid = 0;
    return PktDraw(d);
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
    ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
              | VK_IMAGE_USAGE_TRANSFER_DST_BIT
              | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;   // TRANSFER_SRC：取证转储（VK_DUMP_TEX）要读回
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

    // 第二套 pass/fb：loadOp=CLEAR（所有"要清"的场景都用它：image/loadop/rebind_clear）
    {
        VkAttachmentDescription attClear = att;
        attClear.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        VkRenderPassCreateInfo rpciClear = rpci;
        rpciClear.pAttachments = &attClear;
        createRP2(o.dev, &rpciClear, nullptr, &t.rpClear);
        if (t.rpClear) {
            VkFramebufferCreateInfo fciClear = fci;
            fciClear.renderPass = t.rpClear;
            createFB2(o.dev, &fciClear, nullptr, &t.fbClear);
        }
    }

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
    wds[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
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
    // 帧环流水下可能有提交还在用这张图：销毁前彻底等干净（目标销毁很少发生）
    if (g_waitIdle) g_waitIdle(o.queue);
    fprintf(stderr, "[vk] rt del id=%u %dx%d f=%llu lastWrite=%llu\n", id, t.w, t.h,
            (unsigned long long)g_drawFrame, (unsigned long long)t.lastWriteFrame);
    PktDropSets(id);   // 变体 descriptor set 指向这张图，先还回池里
    if (t.fb) o.destroyFramebuffer(o.dev, t.fb, nullptr);
    if (t.rp) o.destroyRenderPass(o.dev, t.rp, nullptr);
    if (t.fbClear) o.destroyFramebuffer(o.dev, t.fbClear, nullptr);
    if (t.rpClear) o.destroyRenderPass(o.dev, t.rpClear, nullptr);
    if (t.view) o.destroyImageView(o.dev, t.view, nullptr);
    if (t.image) o.destroyImage(o.dev, t.image, nullptr);
    if (t.mem) o.freeMemory(o.dev, t.mem, nullptr);
    t = RenderTarget();
    // 目标 id 按槽位复用，留着旧 id 会让 present 采样到无关的目标
    if (o.lastGeomTarget == id) o.lastGeomTarget = 0;
}

// ---- 取证转储（默认关）：VK_DUMP_TEX=<dir> + VK_DUMP_FRAME=<帧号> ----
// 目的：不再猜机制，把"该帧真正被采样"的纹理/离屏目标按 RGBA8 原样落盘给眼睛看。
// 开关只在环境变量里读一次；不设 VK_DUMP_TEX 时 NoteSampled 只做一次 bool 判断。
struct TexDumpCfg {
    int on = 0;          // 0=关 1=开（只解析一次）
    std::string dir;
    uint64_t frame = 0;  // VK_DUMP_FRAME
    bool frameSet = false;  // false = 第一有机会的帧就转
    double atSec = -1.0;    // VK_DUMP_AT_SEC：启动后按墙钟秒数触发（优先于 frame）
    std::vector<uint32_t> ids;  // VK_DUMP_IDS=5,6,7,13（显式清单，绕过自动判定）
    bool idsSet = false;
    std::string trigger;  // VK_DUMP_TRIGGER：该文件出现时才转（手动触发）
    bool snowAuto = false;  // VK_SNOWWATCH：只有噪声探测命中时才转（自动抓雪花）
};

static TexDumpCfg& TexDumpConfig () {
    static TexDumpCfg c;
    static bool parsed = false;
    if (!parsed) {
        parsed = true;
        const char* d = getenv("VK_DUMP_TEX");
        if (d && d[0]) {
            c.on = 1;
            c.dir = d;
            const char* f = getenv("VK_DUMP_FRAME");
            if (f && f[0]) {
                c.frame = (uint64_t)strtoull(f, nullptr, 10);
                c.frameSet = true;
            }
            const char* a = getenv("VK_DUMP_AT_SEC");
            if (a && a[0]) {
                c.atSec = atof(a);
            }
            const char* tg = getenv("VK_DUMP_TRIGGER");
            if (tg && tg[0]) {
                c.trigger = tg;
            }
            const char* sw = getenv("VK_SNOWWATCH");
            if (sw && sw[0] == '1') {
                c.snowAuto = true;
            }
            const char* s = getenv("VK_DUMP_IDS");
            if (s && s[0]) {
                c.idsSet = true;
                const char* p = s;
                while (*p) {
                    while (*p && (*p == ',' || *p == ' ' || *p == '\t')) p++;
                    if (!*p) break;
                    char* e = nullptr;
                    unsigned long v = strtoul(p, &e, 10);
                    if (e == p) break;
                    if (v > 0) c.ids.push_back((uint32_t)v);
                    p = e;
                }
            }
        }
    }
    return c;
}

void VkRenderer::NoteSampled(uint32_t texId) {
    if (!TexDumpConfig().on || texId == 0) return;
    Impl& o = *impl;
    // 只增不清：一帧里所有被采样过的纹理/目标都保留（id 去重）。
    // 之前按 frameGen 分桶清空，导致一帧 6 张只抓到最后一个 epoch 的 2 张。
    for (size_t i = 0; i < o.sampled.size(); i++) {
        if (o.sampled[i] == texId) return;
    }
    o.sampled.push_back(texId);
}

bool VkRenderer::MaybeDumpTextures(uint64_t frameHint) {
    TexDumpCfg& cfg = TexDumpConfig();
    if (!cfg.on) return false;
    Impl& o = *impl;
    if (o.dumpDone) return false;
    // 命中口径：VK_DUMP_AT_SEC 优先（按墙钟秒数，避免加载/菜单帧率过高提前触发）；
    // 否则给 VK_DUMP_FRAME 就等"当前帧 >= 目标帧"的第一次收尾；只给 VK_DUMP_IDS
    // 就在第一次收尾直接转；都没给就第一帧收尾转。
    // VK_DUMP_TRIGGER=<文件>：文件存在才转（外部手动触发，便于"雪花出现时按一下截图"）。
    // VK_SNOWWATCH=1：只有噪声探测命中（o.snowDumpReq）才转——自动抓雪花帧。
    if (cfg.snowAuto) {
        if (!o.snowDumpReq) return false;
    } else if (!cfg.trigger.empty()) {
#if defined(_WIN32)
        if (GetFileAttributesA(cfg.trigger.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
#else
        FILE* tf = fopen(cfg.trigger.c_str(), "rb");
        if (!tf) return false;
        fclose(tf);
#endif
    } else if (cfg.atSec >= 0.0) {
        static const auto t0 = std::chrono::steady_clock::now();
        double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (el < cfg.atSec) return false;
    } else if (cfg.frameSet && frameHint < cfg.frame) {
        return false;
    }
    o.dumpDone = true;

    // 转储清单：显式 ids 优先，否则用累积的 sampled（只增不清，整帧所有采样都保留）
    std::vector<uint32_t> ids;
    if (cfg.idsSet) ids = cfg.ids;
    else ids = o.sampled;
    if (ids.empty())
        fprintf(stderr, "[dump] empty frame=%llu idset=%d dir=%s\n",
                (unsigned long long)frameHint, cfg.idsSet ? 1 : 0, cfg.dir.c_str());

#if defined(_WIN32)
    _mkdir(cfg.dir.c_str());
#else
    mkdir(cfg.dir.c_str(), 0755);
#endif

    char manPath[640];
    snprintf(manPath, sizeof(manPath), "%s/manifest.txt", cfg.dir.c_str());
    FILE* man = fopen(manPath, "w");
    // manifest 每行：id w h fmt bytes sha256 首个非零字节偏移 [truncated]
    // w/h 是"实际写入文件"的尺寸；超 4096 的边被截断时行尾多一个 truncated 标记
    uint32_t dumped = 0, skipped = 0;
    uint64_t dumpedBytes = 0;

    for (size_t k = 0; k < ids.size(); k++) {
        uint32_t id = ids[k];
        VkImage img = VK_NULL_HANDLE;
        int w = 0, h = 0;
        TexSlot* ts = nullptr;       // 贴图才有 CPU 创建时 mean 可对账
        if (id < 1000) {
            if (id > o.textures.size()) { skipped++; continue; }
            ts = &o.textures[id - 1];
            if (!ts->used) { skipped++; continue; }
            img = ts->image; w = ts->w; h = ts->h;
        } else {
            uint32_t ti = id - 1000;
            if (ti >= o.targets.size() || !o.targets[ti].used) { skipped++; continue; }
            img = o.targets[ti].image; w = o.targets[ti].w; h = o.targets[ti].h;
        }
        if (!img || w <= 0 || h <= 0) { skipped++; continue; }

        // 单张上限 4096：超出就截断（取左上 4096×4096），manifest 标 truncated
        int tw = w > 4096 ? 4096 : w;
        int th = h > 4096 ? 4096 : h;
        bool trunc = (tw != w || th != h);
        size_t bytes = (size_t)tw * th * 4;
        if (dumpedBytes + (uint64_t)bytes > (uint64_t)2 * 1024 * 1024 * 1024) {
            fprintf(stderr, "[dump] skip id=%u bytes=%zu reason=budget used=%llu\n",
                    id, bytes, (unsigned long long)dumpedBytes);
            skipped++;
            continue;
        }

        // staging 缓冲（HOST_VISIBLE|HOST_COHERENT）
        VkBuffer sb = VK_NULL_HANDLE;
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = (VkDeviceSize)bytes;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (g_createBuf(o.dev, &bci, nullptr, &sb) != VK_SUCCESS || !sb) { skipped++; continue; }

        VkMemoryRequirements smr{};
        g_getBufReqs(o.dev, sb, &smr);
        VkPhysicalDeviceMemoryProperties mp{};
        ((PFN_vkGetPhysicalDeviceMemoryProperties)g_gipa(o.inst, "vkGetPhysicalDeviceMemoryProperties"))(o.phys, &mp);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = smr.size;
        mai.memoryTypeIndex = FindMemType(mp, smr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkDeviceMemory sm = VK_NULL_HANDLE;
        if (g_allocMem(o.dev, &mai, nullptr, &sm) != VK_SUCCESS || !sm) {
            o.destroyBuffer(o.dev, sb, nullptr);
            skipped++;
            continue;
        }
        g_bindBufMem(o.dev, sb, sm, 0);

        // 一次性命令缓冲：SHADER_READ_ONLY -> TRANSFER_SRC -> 拷回 -> SHADER_READ_ONLY
        VkCommandBuffer cb = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cbi.commandPool = o.cmdPool;
        cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbi.commandBufferCount = 1;
        ((PFN_vkAllocateCommandBuffers)g_gipa(o.inst, "vkAllocateCommandBuffers"))(o.dev, &cbi, &cb);
        VkCommandBufferBeginInfo cbb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        cbb.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        g_beginCB(cb, &cbb);

        VkImageMemoryBarrier b1{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b1.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b1.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b1.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b1.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b1.image = img;
        b1.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b1.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        b1.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        g_cmdBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     0, 0, nullptr, 0, nullptr, 1, &b1);

        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {(uint32_t)tw, (uint32_t)th, 1};
        g_cmdCopyImgToBuf(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sb, 1, &region);

        VkImageMemoryBarrier b2 = b1;
        b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b2.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b2.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        g_cmdBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
                     0, 0, nullptr, 0, nullptr, 1, &b2);

        g_endCB(cb);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        g_submit(o.queue, 1, &si, VK_NULL_HANDLE);
        g_waitIdle(o.queue);
        ((PFN_vkFreeCommandBuffers)g_gipa(o.inst, "vkFreeCommandBuffers"))(o.dev, o.cmdPool, 1, &cb);

        std::vector<unsigned char> px(bytes);
        void* sp = nullptr;
        g_mapMem(o.dev, sm, 0, smr.size, 0, &sp);
        if (sp) memcpy(px.data(), sp, bytes);
        g_unmapMem(o.dev, sm);
        o.destroyBuffer(o.dev, sb, nullptr);
        o.freeMemory(o.dev, sm, nullptr);

        // 落盘：tex_<id>_<w>x<h>.raw（RGBA8、行主序、无压缩）
        char path[700];
        snprintf(path, sizeof(path), "%s/tex_%u_%dx%d.raw", cfg.dir.c_str(), id, tw, th);
        FILE* fp = fopen(path, "wb");
        if (!fp) {
            fprintf(stderr, "[dump] err=open path=%s\n", path);
            skipped++;
            continue;
        }
        fwrite(px.data(), 1, bytes, fp);
        fclose(fp);

        // 自校验：与创建时 [vk] texmean 同一口径（stride=97，(r+g+b)/3）
        uint64_t sum = 0, cnt = 0, nb = 0;
        size_t npx = (size_t)tw * th;
        for (size_t p = 0; p < npx; p += 97) {
            const unsigned char* q = px.data() + p * 4;
            unsigned int v = (q[0] + q[1] + q[2]) / 3;
            sum += v;
            if (v >= 8) nb++;
            cnt++;
        }
        double mean = cnt ? (double)sum / (double)cnt : 0.0;
        double nbPct = cnt ? 100.0 * (double)nb / (double)cnt : 0.0;

        size_t firstnz = bytes;
        for (size_t b = 0; b < bytes; b++) {
            if (px[b]) { firstnz = b; break; }
        }

        uint8_t dig[32];
        lime::vkpkt::Sha256(px.data(), bytes, dig);
        char shex[65];
        for (int i = 0; i < 32; i++) snprintf(shex + i * 2, 3, "%02x", (unsigned)dig[i]);

        if (man) {
            fprintf(man, "%u %d %d RGBA8 %zu %s %zu%s\n",
                    id, tw, th, bytes, shex, firstnz, trunc ? " truncated" : "");
        }
        dumped++;
        dumpedBytes += (uint64_t)bytes;

        // 对账：GPU 读回算出的 mean 必须与 CPU 创建时的 [vk] texmean 一致才可信
        if (ts) {
            if (ts->updates > 0 || !ts->hasCreateMean) {
                fprintf(stderr, "[dump] id=%u %dx%d mean=%.2f nb=%.1f verify=stale(updates=%u) bytes=%zu\n",
                        id, tw, th, mean, nbPct, ts->updates, bytes);
            } else {
                double d = mean - (double)ts->createMean;
                if (d < 0) d = -d;
                fprintf(stderr, "[dump] id=%u %dx%d mean=%.2f texmean=%.2f nb=%.1f/%.1f delta=%.3f ok=%d bytes=%zu%s\n",
                        id, tw, th, mean, (double)ts->createMean, nbPct, (double)ts->createNb,
                        d, (d < 0.01 && !trunc) ? 1 : 0, bytes, trunc ? " truncated" : "");
            }
        } else {
            fprintf(stderr, "[dump] id=%u %dx%d mean=%.2f nb=%.1f verify=n/a(target) bytes=%zu%s\n",
                    id, tw, th, mean, nbPct, bytes, trunc ? " truncated" : "");
        }
    }

    if (man) fclose(man);
    fprintf(stderr, "[dump] done frame=%llu dumped=%u skipped=%u bytes=%llu dir=%s idset=%d n=%zu\n",
            (unsigned long long)frameHint, dumped, skipped, (unsigned long long)dumpedBytes,
            cfg.dir.c_str(), cfg.idsSet ? 1 : 0, ids.size());
    return dumped > 0;
}

// 帧环取槽：等该槽上一次提交的 fence（保证命令缓冲/顶点区/UBO 区可安全复用），
// 并真正释放该槽延迟销毁的纹理。fence 不可用时退回 waitIdle（保守但不流水）。
void VkRenderer::AcquireSlot() {
    Impl& o = *impl;
    WaitSlotFence();
    const uint32_t s = o.slot;
    if (!o.slotDestroy[s].empty()) {
        std::vector<uint32_t> ids;
        ids.swap(o.slotDestroy[s]);
        for (uint32_t id : ids) DestroyTextureNow(id);
    }
    o.cmd = o.cmds[s];
    o.uboPtr = o.uboBase + (size_t)s * (o.uboStride / sizeof(float));
    o.vbSlotOff = (VkDeviceSize)s * o.vbBytes;
}

void VkRenderer::WaitSlotFence() {
    Impl& o = *impl;
    const uint32_t s = o.slot;
    if (!o.fencePending[s]) return;
    const double tw = PerfNowMs();
    if (g_waitFences && g_resetFences && o.fences[s] != VK_NULL_HANDLE) {
        g_waitFences(o.dev, 1, &o.fences[s], VK_TRUE, UINT64_MAX);
        g_resetFences(o.dev, 1, &o.fences[s]);
    } else if (g_waitIdle) {
        g_waitIdle(o.queue);
    }
    // Phase 0（VK_PASSSTAT=1）：fence 触发 = 本笔 pass 真正跑完，时间戳可读。
    // 在这里读回，测量本身不额外引入等待。顺带打出 pass 图元数据（目标/尺寸/负载模式/图元数）。
    if (o.passStat && o.pendValid[s]) {
        o.pendValid[s] = false;
        if (o.passQueries[s] != VK_NULL_HANDLE && g_getQP) {
            uint64_t ts[2] = {0, 0};
            VkResult qr = g_getQP(o.dev, o.passQueries[s], 0, 2, sizeof(ts), ts,
                                  sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
            static const char* kLoad[4] = {"LOAD", "CLEARP", "CLEAR0", "BB"};
            if (qr == VK_SUCCESS) {
                double ms = (double)(ts[1] - ts[0]) * o.tsPeriodNs / 1e6;
                fprintf(stderr, "[pass] f=%llu tgt=%u ext=%ux%u load=%s ranges=%u verts=%u progs=0x%x ms=%.3f\n",
                        (unsigned long long)o.pendFrame[s], o.pendTarget[s],
                        o.pendExtW[s], o.pendExtH[s], kLoad[o.pendLoad[s] & 3],
                        o.pendRanges[s], o.pendVerts[s], o.pendProgMask[s], ms);
            } else {
                fprintf(stderr, "[pass] f=%llu tgt=%u result=%d\n",
                        (unsigned long long)o.pendFrame[s], o.pendTarget[s], (int)qr);
            }
        }
    }
    o.fencePending[s] = false;
    if (PerfOn()) g_perf.waitMs += PerfNowMs() - tw;
}

void VkRenderer::EndFrame() {
    Impl& o = *impl;
    const double perfT0 = PerfNowMs();
    // 探针（审计口径）：每个视觉帧只出一次的两份状态快照——渲染器 currentTarget 与 gl_fbo g_currentTarget
    if (DiagOn() && o.presentReq) {
        fprintf(stderr, "[vk] efstate f=%llu req=1 open=%d cur=%u gcur=%u bbHeld=%d bbOpen=%d\n",
                (unsigned long long) g_drawFrame, o.frameOpen ? 1 : 0, o.currentTarget,
                lime::gl_vk_current_target (), o.bbHeld ? 1 : 0, g_bbOpen);
    }
    if (!o.frameOpen) {
        // 审计要求：不再用 g_bbOpen 门控，每次空 EndFrame 都打，否则"零 noop"无法区分原因
        if (DiagOn()) fprintf(stderr, "[vk] bbframe close f=%llu target=%u noop=1 BBOPEN=%d req=%d\n",
                (unsigned long long) g_drawFrame, o.currentTarget, g_bbOpen, o.presentReq ? 1 : 0);
        return;
    }
    // 探针（只记状态）：后缓冲 close 与 open 是否配对；离屏 close 时后缓冲还开着 = 顶点被并走
    if (DiagOn()) {
        size_t pv = PendingVertsOf (o.textures, o.targets);
        if (o.currentTarget == 0) {
            fprintf(stderr, "[vk] bbframe close f=%llu target=0 verts=%zu open=%d\n",
                    (unsigned long long) g_drawFrame, pv, g_bbOpen ? 1 : 0);
            g_bbOpen = 0;
        } else if (g_bbOpen) {
            fprintf(stderr, "[vk] bbframe close f=%llu target=%u verts=%zu BBOPEN=1 backbufferNotClosed\n",
                    (unsigned long long) g_drawFrame, o.currentTarget, pv);
        }
    } else {
        g_bbOpen = 0;
    }

    // 帧环：换到下一个槽位录制（等该槽上一次提交的 fence，UBO/顶点区随槽位切换）
    AcquireSlot();
    float* dst = o.vbPtr + (size_t)(o.vbSlotOff / sizeof(float));
    // id = 本段采样源的批次号（1..N 贴图，1000+ 离屏槽位）：接自定义管线时要按它取 view/sampler
    struct DrawRange {
        VkDescriptorSet set;
        size_t first;
        size_t count;
        uint16_t prog;
        uint32_t id;
        const std::vector<uint8_t>* uni;   // 该笔的 uniform 快照（可为空）
    };
    std::vector<DrawRange> ranges;
    size_t totalVerts = 0;

    // 取证：按提交实际拼出的批次顺序记录源 id（贴图 1..N，离屏 1000+槽位）
    std::vector<uint32_t> batchIds;
    // 提交一段顶点：src 指向该段在源顶点数组里的第一个顶点，cnt 为顶点数。
    // 同一张贴图里可能混着不同 GL program 的笔：按 program 连续段拆 range，
    // 这样每段能绑自己的管线；没有逐顶点 program 信息时整段按 0（退回固定管线）。
    // 诊断 VK_PROGPIPE_DBG=1：标签数与顶点数对不上时整段退化成 prog=0，
    // 着色器就永远绑不上。先把差值打出来，指认是哪一批漏了 push。
    auto pushSeg = [&](const float* src, size_t cnt, const std::vector<uint16_t>* pv,
                       size_t pvOff, VkDescriptorSet set, uint32_t id,
                       const std::vector<uint8_t>* uni) {
        if (!src || cnt == 0) return;
        if (totalVerts + cnt > (size_t)MAX_SPRITES * 6) return;
        memcpy(dst + totalVerts * 15, src, cnt * 15 * sizeof(float));
        bool tagsOk = pv && (pvOff + cnt) <= pv->size();
        static int s_mismatch = 0;
        if (pv && !tagsOk && s_mismatch < 12) {
            s_mismatch++;
            fprintf(stderr, "[progpipe] mismatch id=%u verts=%zu tags=%zu off=%zu\n",
                    id, cnt, pv->size(), pvOff);
        }
        if (tagsOk) {
            size_t start = 0;
            for (size_t k = 1; k <= cnt; k++) {
                if (k == cnt || (*pv)[pvOff + k] != (*pv)[pvOff + start]) {
                    ranges.push_back({set, totalVerts + start, k - start,
                                      (*pv)[pvOff + start], id, uni});
                    start = k;
                }
            }
        } else {
            ranges.push_back({set, totalVerts, cnt, 0, id, uni});
        }
        totalVerts += cnt;
        batchIds.push_back(id);
    };
    // 实验 VK_ORDER_11FIRST=1（默认关）：3× 全屏源（贴图 11）改为最先画。
    // 保留仅作诊断；正常路径已按 GL 提交顺序提交，不再需要它纠正层序。
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
    // 按 GL 提交顺序把每段发出去（顺序错了会把整屏层叠关系画反）
    auto pushOneSeg = [&](const Impl::FrameSeg& s) {
        if (s.count == 0 || skipped(s.id)) return;
        if (s.id >= 1000) {
            uint32_t ti = s.id - 1000;
            if (ti >= o.targets.size() || !o.targets[ti].used) return;
            const std::vector<float>& v = o.targets[ti].verts;
            if ((s.off + s.count) * 15 > v.size()) return;
            pushSeg(v.data() + s.off * 15, s.count, &o.targets[ti].progs, s.off,
                    o.targets[ti].set, s.id, &s.uni);
        } else {
            if (s.id == 0 || s.id > o.textures.size()) return;
            TexSlot& t = o.textures[s.id - 1];
            if (!t.used) return;
            if ((s.off + s.count) * 15 > t.verts.size()) return;
            pushSeg(t.verts.data() + s.off * 15, s.count, &t.progs, s.off, t.set, s.id, &s.uni);
        }
    };
    if (s_11first) for (auto& s : o.segs) if (s.id == 11) pushOneSeg(s);
    for (auto& s : o.segs) {
        if (s_11first && s.id == 11) continue;
        pushOneSeg(s);
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
            pushSeg(q.data(), 6, nullptr, 0, o.textures[7].set, 8, nullptr);
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
        // 注意：lastWriteFrame 是首次使用清屏判定的输入，必须无条件维护，
        // 不能像以前一样塞在 DiagOn 里——否则 VK_DIAG=0 时每个离屏 pass 都会被当成"首次使用"。
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
        if (DiagOn() && s_submitLog < 30000) {
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
        // 汇总（只计数，不逐条打）：tgt=0 的提交帧数/顶点/空帧，以及按 program 的累计绘制
        {
            static uint64_t s_t0 = 0, s_t0v = 0, s_t0empty = 0, s_t104 = 0, s_t105 = 0, s_osum = 0;
            if (o.currentTarget == 0) {
                s_t0++;
                s_t0v += totalVerts;
                if (totalVerts == 0) s_t0empty++;
            } else if (o.currentTarget == 1004) {
                s_t104++;
            } else if (o.currentTarget == 1005) {
                s_t105++;
            }
            s_osum++;
            if (DiagOn() && s_osum % 60 == 0) {
                fprintf(stderr,
                        "[vksum] f=%llu t0=%llu t0verts=%llu t0empty=%llu t1004=%llu t1005=%llu draws=%llu"
                        " p1=%llu p2=%llu p3=%llu p4=%llu p5=%llu p6=%llu p7=%llu w1=%llu w5=%llu w6=%llu bbopen=%d\n",
                        (unsigned long long) s_frames, (unsigned long long) s_t0, (unsigned long long) s_t0v,
                        (unsigned long long) s_t0empty, (unsigned long long) s_t104, (unsigned long long) s_t105,
                        (unsigned long long) g_progDrawTotal,
                        (unsigned long long) g_progDraw[1], (unsigned long long) g_progDraw[2],
                        (unsigned long long) g_progDraw[3], (unsigned long long) g_progDraw[4],
                        (unsigned long long) g_progDraw[5], (unsigned long long) g_progDraw[6],
                        (unsigned long long) g_progDraw[7],
                        (unsigned long long) g_progWhole[1], (unsigned long long) g_progWhole[5],
                        (unsigned long long) g_progWhole[6], g_bbOpen);
            }
        }
        if (DiagOn() && (s_frames == 1 || s_frames % 30 == 0 || o.currentTarget == 0)) {
            float first[4] = {0, 0, 0, 0};
            if (totalVerts > 0) memcpy(first, dst, sizeof(first));
            fprintf(stderr, "[vk] frame=%llu verts=%zu ranges=%zu target=%u first=(%.1f,%.1f,%.2f,%.2f)\n",
                    (unsigned long long)s_frames, totalVerts, ranges.size(), o.currentTarget,
                    first[0], first[1], first[2], first[3]);
            if (o.currentTarget == 0 && totalVerts >= 6) {
                fprintf(stderr, "[vk] v6=(%.1f,%.1f,%.3f,%.3f) (%.1f,%.1f,%.3f,%.3f) (%.1f,%.1f,%.3f,%.3f) | (%.1f,%.1f,%.3f,%.3f) (%.1f,%.1f,%.3f,%.3f) (%.1f,%.1f,%.3f,%.3f)\n",
                        dst[0], dst[1], dst[13], dst[14],
                        dst[15], dst[16], dst[28], dst[29],
                        dst[30], dst[31], dst[43], dst[44],
                        dst[45], dst[46], dst[58], dst[59],
                        dst[60], dst[61], dst[73], dst[74],
                        dst[75], dst[76], dst[88], dst[89]);
                // 第二个四边形（顶点 6..11）：上屏合成有两个 range，只打前 6 个顶点会漏掉主体
                if (totalVerts >= 12) {
                    fprintf(stderr, "[vk] v6b=(%.1f,%.1f) (%.1f,%.1f) (%.1f,%.1f) | (%.1f,%.1f) (%.1f,%.1f) (%.1f,%.1f)\n",
                            dst[90], dst[91], dst[105], dst[106],
                            dst[120], dst[121], dst[135], dst[136],
                            dst[150], dst[151], dst[165], dst[166]);
                }
            }
        }
    }
    bool clearZero = false;
    bool clearPendingUse = false;
    uint32_t progMask = 0;   // Phase 0：本笔用到哪些自定义 program（prog < 32 的位掩码）
    if (o.currentTarget >= 1000) {
        uint32_t ti = o.currentTarget - 1000;
        if (ti < o.targets.size() && o.targets[ti].used) {
            RenderTarget& rt = o.targets[ti];
            if (!rt.cleared && rt.lastWriteFrame == 0 && rt.rpClear && rt.fbClear) {
                // 首次使用：GL 驱动会把新建纹理当 0 初始化，Vulkan 不会。
                // 若首笔走 LOAD，没被精灵覆盖的区域就是显存里的随机噪声（雪花屏）。
                // 等价 GL 语义：首用强制清成 (0,0,0,0)。
                fb = rt.fbClear;
                rp = rt.rpClear;
                clearZero = true;
                rt.cleared = true;      // 只清这一次；不标记会导致每次 pass 都清（黑闪/丢内容）
                rt.clearedFrame = g_drawFrame;
                if (DiagOn()) fprintf(stderr, "[vk] target first-use clear tgt=%u %dx%d f=%llu\n",
                        o.currentTarget, rt.w, rt.h, (unsigned long long)g_drawFrame);
            } else if (rt.clearPending && rt.rpClear && rt.fbClear) {
                // 游戏这一帧对这个目标发过 glClear（apitrace 实测：原版每帧都清 (0,0,0,0)）：
                // 走 CLEAR pass，清屏色用它给的 rgba，清完再画，等价于 GL 的整幅替换。
                fb = rt.fbClear;
                rp = rt.rpClear;
                clearPendingUse = true;
            } else if (vktuning::Get().offscreenLoadop == vktuning::LOADOP_CLEAR
                && rt.rpClear && rt.fbClear) {
                // 预览开关 offscreen_loadop=clear：每个离屏 pass 都走 CLEAR 版 pass/fb，
                // 清成 (0,0,0,0)，让自反馈链每帧从干净状态开始（默认 load = 现状）
                fb = rt.fbClear;
                rp = rt.rpClear;
                clearZero = true;
            } else {
                fb = rt.fb;
                rp = rt.rp;
                // 取证：既没清过、也从没被写过，却要 LOAD → Vulkan 下是未定义显存内容
                if (!rt.cleared && rt.lastWriteFrame == 0) {
                    static uint64_t s_lb = 0;
                    if (s_lb < 50) {
                        fprintf(stderr, "[vk] rt load-before-write tgt=%u %dx%d f=%llu\n",
                                o.currentTarget, rt.w, rt.h, (unsigned long long)g_drawFrame);
                        s_lb++;
                    }
                }
            }
            ext = {(uint32_t)rt.w, (uint32_t)rt.h};
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
    // 跨提交依赖：本笔会采样/覆盖上一笔刚写过的目标。帧环不再每笔 waitIdle，
    // 用一条队列级全屏障把"前一笔写完 → 本笔读/写"的顺序和可见性钉死。
    {
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        g_cmdBarrier(o.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                     0, 1, &mb, 0, nullptr, 0, nullptr);
    }
    // Phase 0（VK_PASSSTAT=1）：本槽时间戳池复位，pass 起点打第一针（必须在 render pass 之外）
    if (o.passStat && o.passQueries[o.slot] != VK_NULL_HANDLE) {
        g_resetQP(o.cmd, o.passQueries[o.slot], 0, 2);
        g_writeTS(o.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, o.passQueries[o.slot], 0);
    }
    VkClearValue cv{};
    if (clearZero) {
        // 强制清洗不沿用上次 clear 包的颜色，避免把旧颜色带进来
        cv.color.float32[0] = 0.0f;
        cv.color.float32[1] = 0.0f;
        cv.color.float32[2] = 0.0f;
        cv.color.float32[3] = 0.0f;
    } else if (clearPendingUse) {
        uint32_t ti = o.currentTarget - 1000;
        cv.color.float32[0] = o.targets[ti].pcR;
        cv.color.float32[1] = o.targets[ti].pcG;
        cv.color.float32[2] = o.targets[ti].pcB;
        cv.color.float32[3] = o.targets[ti].pcA;
        o.targets[ti].clearPending = false;
        o.targets[ti].cleared = true;
        o.targets[ti].clearedFrame = g_drawFrame;
        if (DiagOn()) fprintf(stderr, "[vk] offscreen clear tgt=%u rgba=(%.2f,%.2f,%.2f,%.2f) f=%llu\n",
                o.currentTarget, o.targets[ti].pcR, o.targets[ti].pcG,
                o.targets[ti].pcB, o.targets[ti].pcA, (unsigned long long)g_drawFrame);
    } else {
        cv.color.float32[0] = o.clearR;
        cv.color.float32[1] = o.clearG;
        cv.color.float32[2] = o.clearB;
        cv.color.float32[3] = o.clearA;
    }
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
        // 颜色调制接线：std140 = mat4(0..63) / bool openfl_HasColorTransform(64)
        // / vec2 openfl_TextureSize(72) / bool hasColorTransform(80) / bool hasTransform(84)。
        // openfl_HasColorTransform 现在打开：顶点着色器只有在它为 true 时才把
        // loc6/loc7（逐顶点 mult/off）写进 varying，之前保持 0 导致顶点色白接了。
        // 无数组的笔在 BuildVertexColor 里已回退成 identity(1/0)，不会乘黑。
        uint32_t* ub32 = (uint32_t*)o.uboPtr;
        ub32[64 / 4] = 1;   // openfl_HasColorTransform = true
        ub32[80 / 4] = 1;   // hasColorTransform = true
        ub32[84 / 4] = 1;   // hasTransform = true
        o.uboPtr[72 / 4] = (float)ext.width;
        o.uboPtr[73 / 4] = (float)ext.height;
    }

    g_beginRP(o.cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    if (PerfOn()) { g_perf.rpBegins++; g_perf.verts += (uint64_t)totalVerts; }
    if (totalVerts > 0) {
        // 接线探针（本轮只接不选，一次）：只在 backbuffer pass、且探针管线已建成时，
        // 先绑探针管线再立刻换回固定管线；两次绑定之间没有任何 draw，故不改变像素。
        static int s_probeEnabled = -1;
        static int s_probeDone = 0;
        if (s_probeEnabled < 0) {
            const char* e = getenv("VK_PROGPIPE_PROBE");
            s_probeEnabled = (e && e[0] == '0') ? 0 : 1;
        }
        if (s_probeEnabled && !s_probeDone && rp == o.renderPass
                && g_probePipeline != VK_NULL_HANDLE) {
            g_bindPipe(o.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_probePipeline);
            s_probeDone = 1;
            fprintf(stderr, "[progpipe] bind ok prog=%d pipeline=%p rp=%p\n",
                    g_probeProgram, (void*) g_probePipeline, (void*) rp);
        }
        // 每个 pass 都按 GL 侧当前 viewport/scissor 动态设置（管线里不再写死）。
        // 这两个值就是 openfl 每帧/每次绑 RT 时通过 gl.viewport / gl.scissor 发下来的。
        VkViewport dynVp = lime::glstate::GetVulkanViewport((int)ext.height);
        if (dynVp.width <= 0.0f || dynVp.height <= 0.0f) {
            dynVp = {0, 0, (float)ext.width, (float)ext.height, 0, 1};
        }
        VkRect2D dynSc = lime::glstate::GetVulkanScissor((int)ext.width, (int)ext.height);
        if (dynSc.extent.width == 0 || dynSc.extent.height == 0) {
            dynSc = {{0, 0}, ext};
        }
        g_setViewport(o.cmd, 0, 1, &dynVp);
        g_setScissor(o.cmd, 0, 1, &dynSc);
        // 观测（只在变化时打）：写死的 viewport 是"画面只在左侧一块"的直接嫌疑，
        // 这条日志给出实际提交值与 pass 尺寸，用于与截图逐帧对齐。
        {
            static int s_lvp[4] = {-1, -1, -1, -1};
            static int s_lsc[4] = {-1, -1, -1, -1};
            int cvp[4] = {(int)dynVp.x, (int)dynVp.y, (int)dynVp.width, (int)dynVp.height};
            int csc[4] = {(int)dynSc.offset.x, (int)dynSc.offset.y,
                          (int)dynSc.extent.width, (int)dynSc.extent.height};
            if (memcmp(cvp, s_lvp, sizeof(cvp)) != 0 || memcmp(csc, s_lsc, sizeof(csc)) != 0) {
                memcpy(s_lvp, cvp, sizeof(cvp));
                memcpy(s_lsc, csc, sizeof(csc));
                fprintf(stderr, "[vk] vpstate f=%llu tgt=%u ext=%ux%u vp=%d,%d,%d,%d sc=%d,%d,%d,%d\n",
                        (unsigned long long)g_drawFrame, (unsigned)o.currentTarget,
                        ext.width, ext.height, cvp[0], cvp[1], cvp[2], cvp[3],
                        csc[0], csc[1], csc[2], csc[3]);
            }
        }
        g_bindPipe(o.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, o.pipeline);
        VkBuffer vbufs[2] = {o.vb, o.attrBuf};
        VkDeviceSize voffs[2] = {o.vbSlotOff, 0};
        g_bindVB(o.cmd, 0, 2, vbufs, voffs);
        // 全新的一次提交：复位每个 program 的动态 UBO 槽位计数（每笔一个槽位）
        lime::gl_shader_reset_uniform_slots();
        // VK_PROGPIPE_USE=1：按每段的 GL program 绑它自己的管线；取不到就退回固定管线。
        // 默认关（0）时行为与之前逐字一致。这是让游戏 shader 真正参与绘制的开关。
        static int s_useProg = -1;
        if (s_useProg < 0) {
            const char* e = getenv("VK_PROGPIPE_USE");
            s_useProg = (e && e[0] == '1') ? 1 : 0;
        }
        VkPipeline boundPipe = o.pipeline;
        VkPipelineLayout boundLayout = o.pipeLayout;
        static uint64_t s_progSw = 0;
        static uint64_t s_progDS = 0;
        // 诊断（VK_PROGPIPE_DBG=1）：分别确认"段里有没有记下 prog"与"管线取不取得到"
        static int s_progDbg = -1;
        if (s_progDbg < 0) {
            const char* e = getenv("VK_PROGPIPE_DBG");
            s_progDbg = (e && e[0] == '1') ? 1 : 0;
        }
        for (auto& r : ranges) {
            // 诊断（VK_DRAWLOG_FRAME=<n> 或 "<from>-<to>"，默认关）：逐笔打印
            // prog / 贴图 / 顶点像素范围 / 程序 UBO 里的 openfl_Matrix 实际值 / 顶点色，
            // 用于区分"顶点被压扁"与"矩阵被写错"。只读，不影响渲染。
            {
                static long long s_dlFrame = -2, s_dlEnd = -2;
                if (s_dlFrame == -2) {
                    const char* e = getenv("VK_DRAWLOG_FRAME");
                    s_dlFrame = -1; s_dlEnd = -1;
                    if (e && e[0]) {
                        long long a = 0, b = 0;
                        if (sscanf(e, "%lld-%lld", &a, &b) == 2) { s_dlFrame = a; s_dlEnd = b; }
                        else { s_dlFrame = atoll(e); s_dlEnd = s_dlFrame; }
                    }
                }
                if (s_dlFrame >= 0 && (long long)g_drawFrame >= s_dlFrame
                        && (long long)g_drawFrame <= s_dlEnd) {
                    const uint8_t* ub = nullptr;
                    uint32_t usz = 0;
                    float m00 = 0, m01 = 0, m10 = 0, m11 = 0, mtx = 0, mty = 0, mw = 0;
                    if (r.prog > 0 && lime::gl_shader_uniform_block((int)r.prog, &ub, &usz)
                            && ub && usz >= 64) {
                        const float* f = (const float*)ub;
                        m00 = f[0]; m01 = f[1]; m10 = f[4]; m11 = f[5];
                        mtx = f[12]; mty = f[13]; mw = f[15];
                    }
                    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
                    float a0 = 0, cm[4] = {0, 0, 0, 0}, cf[4] = {0, 0, 0, 0};
                    if (r.count > 0 && dst) {
                        const float* vp = dst + r.first * 15;
                        a0 = vp[4];
                        for (int k = 0; k < 4; k++) { cm[k] = vp[5 + k]; cf[k] = vp[9 + k]; }
                        x0 = x1 = vp[0];
                        y0 = y1 = vp[1];
                        for (size_t k = 1; k < r.count; k++) {
                            const float* q = vp + k * 15;
                            if (q[0] < x0) x0 = q[0];
                            if (q[0] > x1) x1 = q[0];
                            if (q[1] < y0) y0 = q[1];
                            if (q[1] > y1) y1 = q[1];
                        }
                    }
                    fprintf(stderr,
                            "[drawlog] f=%llu tgt=%u prog=%u id=%u first=%zu n=%zu "
                            "pos=[%.1f..%.1f, %.1f..%.1f] m00=%.7f m01=%.7f m10=%.7f m11=%.7f "
                            "tx=%.4f ty=%.4f w=%.4f cx=[%.3f..%.3f] wcalc=[%.3f..%.3f] "
                            "a0=%.3f cm=[%.3f %.3f %.3f %.3f] cf=[%.3f %.3f %.3f %.3f]\n",
                            (unsigned long long)g_drawFrame, (unsigned)o.currentTarget,
                            (unsigned)r.prog, r.id, r.first, r.count,
                            x0, x1, y0, y1, m00, m01, m10, m11, mtx, mty, mw,
                            m00 * x0 + m10 * y0 + mtx, m00 * x1 + m10 * y1 + mtx,
                            mtx * x0 + mty * y0 + mw, mtx * x1 + mty * y1 + mw,
                            a0, cm[0], cm[1], cm[2], cm[3], cf[0], cf[1], cf[2], cf[3]);
                }
            }
            if (s_progDbg) {
                static unsigned char s_seenProg[256] = {0};
                static int s_seenNull = 0;
                int pg = (int)r.prog;
                if (pg > 0 && pg < 256 && !s_seenProg[pg]) {
                    s_seenProg[pg] = 1;
                    VkPipeline p = lime::gl_shader_pipeline(pg, false);
                    fprintf(stderr, "[progpipe] dbg prog=%d pipe=%s\n", pg,
                            p == VK_NULL_HANDLE ? "null" : "ok");
                }
                if (pg == 0 && s_seenNull < 3) {
                    s_seenNull++;
                    fprintf(stderr, "[progpipe] dbg range-without-prog count=%zu\n", ranges.size());
                }
            }
            // 二分工具（VK_PROGPIPE_SKIP=2,3,6）：列出的 prog 强制退回兜底管线，
            // 用于定位"哪个 prog 的离屏笔把场景画没了"。不设 = 全部走自定义。
            static unsigned char s_skipProgs[256] = {0};
            static int s_skipInit = 0;
            if (!s_skipInit) {
                s_skipInit = 1;
                const char* e = getenv("VK_PROGPIPE_SKIP");
                if (e && *e) {
                    unsigned cur = 0; bool has = false;
                    for (const char* p = e; ; p++) {
                        if (*p >= '0' && *p <= '9') { cur = cur * 10u + (unsigned)(*p - '0'); has = true; }
                        else {
                            if (has && cur < 256) s_skipProgs[cur] = 1;
                            cur = 0; has = false;
                            if (!*p) break;
                        }
                    }
                }
            }
            // prog2（1×1 全屏填充笔）暂固定走兜底：它的自定义管线会把不透明黑盖到内容上
            // （提交序按纹理槽号，tex10 在 tex3 之后），SKIP=2 实测场景即恢复（黑 89%→75.7%）。
            if (s_useProg && r.prog > 0 && r.prog != 2 && !s_skipProgs[r.prog]) {
                // 同一个 program 在两个阶段用两套管线：画到离屏目标（currentTarget>=1000）
                // 和画到屏幕（0）。Vulkan 要求管线创建时的 render pass 与当前绑定的一致，
                // 否则行为未定义（实测表现为整屏全黑）。三个调用必须共用同一个 off。
                const bool off = (o.currentTarget >= 1000);
                VkPipeline pp = lime::gl_shader_pipeline((int)r.prog, off);
                VkPipelineLayout pl = lime::gl_shader_pipeline_layout((int)r.prog);
                VkDescriptorSet ds = lime::gl_shader_descriptor_set((int)r.prog, off);
                if (pp != VK_NULL_HANDLE && pl != VK_NULL_HANDLE && ds != VK_NULL_HANDLE) {
                    VkImageView vw = VK_NULL_HANDLE;
                    VkSampler sp = VK_NULL_HANDLE;
                    if (r.id >= 1000) {
                        uint32_t ti = r.id - 1000;
                        if (ti < o.targets.size() && o.targets[ti].used) vw = o.targets[ti].view;
                        // 离屏目标没有自己的 sampler，借一个已用贴图槽的（都为空时留 0，
                        // 由 gl_shader_write_sampler 内部回退到默认采样器）
                        for (size_t k = 0; k < o.textures.size(); k++) {
                            if (o.textures[k].used && o.textures[k].sampler) {
                                sp = o.textures[k].sampler;
                                break;
                            }
                        }
                    } else if (r.id > 0 && (size_t)r.id <= o.textures.size()) {
                        vw = o.textures[r.id - 1].view;
                        sp = o.textures[r.id - 1].sampler;
                    }
                    if (vw != VK_NULL_HANDLE) {
                        // 按“这一笔绘制时刻”的 uniform 快照写入独立动态偏移槽位。
                        // 批处理帧尾统一提交时，同一个 program 的多笔绘制不能再共用一块
                        // UBO（否则最后一笔的 hasColorTransform/glitchAmount 会覆盖前面所有笔）。
                        uint32_t dynOff = 0;
                        bool okU = false;
                        if (r.uni && !r.uni->empty()) {
                            uint32_t o2 = lime::gl_shader_upload_uniform_slot(
                                    (int)r.prog, r.uni->data(), (uint32_t)r.uni->size());
                            if (o2 != 0xFFFFFFFFu) { dynOff = o2; okU = true; }
                        }
                        if (!okU) okU = lime::gl_shader_upload_uniforms((int)r.prog);
                        // 采样器名字不统一：8 属性那批叫 bitmap，5 属性那批（上屏用）叫
                        // openfl_Texture。两个都试，先命中的那个生效。
                        bool okS = lime::gl_shader_write_sampler((int)r.prog, off, "bitmap", vw, sp);
                        if (!okS) {
                            okS = lime::gl_shader_write_sampler((int)r.prog, off, "openfl_Texture", vw, sp);
                        }
                        // 一次性诊断：把两条返回值和关键尺寸打出来，区分"描述符没通"与"顶点塌陷"
                        static unsigned char s_rep[256][2] = {{0}};
                        int pg = (int)r.prog;
                        if (pg > 0 && pg < 256 && !s_rep[pg][off ? 1 : 0]) {
                            s_rep[pg][off ? 1 : 0] = 1;
                            fprintf(stderr,
                                    "[progpipe] state prog=%d off=%d upload=%d sampler=%d "
                                    "id=%u view=%p smp=%p bind=%d\n",
                                    pg, off ? 1 : 0, okU ? 1 : 0, okS ? 1 : 0,
                                    r.id, (void*)vw, (void*)sp,
                                    lime::gl_shader_sampler_binding(pg, "bitmap"));
                        }
                        // 说明：这里不再切换"负高度 viewport"。program 管线以前写死
                        // viewport，这行 setViewport 实际不生效；现在 viewport/scissor
                        // 改成动态状态（gl_shader.cpp），若保留负高度会把整屏上下镜像。
                        // 游戏矩阵本身已按 Vulkan NDC 方向输出（实测 prog3 m11>0），
                        // 沿用 pass 级动态 viewport 即可。
                        if (pp != boundPipe) {
                            g_bindPipe(o.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pp);
                            boundPipe = pp;
                            s_progSw++;
                            if (PerfOn()) g_perf.pipeBinds++;
                        }
                        boundLayout = pl;
                        // 上屏笔（tgt=0）的 Y 方向：游戏给后缓冲的矩阵按 GL 的 y-up NDC
                        // 生成（实测 m11<0，y=0 → +0.9889），Vulkan 是 y-down。用负高度
                        // 视口把 Y 翻回来（等价于 NDC.y = -NDC.y，且保留矩阵里的平移量，
                        // FPS 文本这种带 4px 偏移的小四边形才能落在原位置）。
                        // 同时取消 pack 阶段对离屏源（id>=1000）做的 V 翻转：翻转几何
                        // 已经由负视口完成，两个都做会互相抵消。
                        // 尺寸不匹配的过渡帧不做任何翻转（见 DrawRaw 里同条件），
                        // 否则补偿会把整层镜像（缩放动画中“画面倒过来”）。
                        if (o.currentTarget == 0 && WindowSizeMatchesExtent()) {
                            VkViewport nvp = dynVp;
                            nvp.y = dynVp.y + dynVp.height;
                            nvp.height = -dynVp.height;
                            g_setViewport(o.cmd, 0, 1, &nvp);
                            float* vp = dst + r.first * 15;
                            for (size_t k = 0; k < r.count; k++) {
                                float* q = vp + k * 15;
                                if (r.id >= 1000) q[14] = 1.0f - q[14];
                            }
                        }
                        // UBO binding 0 是动态 UBO：传本笔的槽位偏移；program 没有
                        // uniform block（uniformSize=0，binding 0 不存在）时不能传动态偏移。
                        if (lime::gl_shader_uniform_buffer_size((int)r.prog) > 0) {
                            g_bindDS(o.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &ds, 1, &dynOff);
                        } else {
                            g_bindDS(o.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &ds, 0, nullptr);
                        }
                        g_cmdDraw(o.cmd, (uint32_t)r.count, 1, (uint32_t)r.first, 0);
                        if (PerfOn()) { g_perf.dsBinds++; g_perf.draws++; }
                        if (r.prog < 32) progMask |= (1u << r.prog);
                        s_progDS++;
                        continue;
                    }
                }
            }
            if (boundPipe != o.pipeline) {
                g_bindPipe(o.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, o.pipeline);
                boundPipe = o.pipeline;
                if (PerfOn()) g_perf.pipeBinds++;
            }
            boundLayout = o.pipeLayout;
            // 固定管线用正常（y 向下）视口：上一笔 progpipe 上屏笔可能刚把它切成负高度
            if (o.currentTarget == 0) g_setViewport(o.cmd, 0, 1, &dynVp);
            // binding0 是动态 UBO：传当前帧环槽位的偏移
            uint32_t fbDynOff = (uint32_t)o.slot * o.uboStride;
            g_bindDS(o.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, o.pipeLayout, 0, 1, &r.set, 1, &fbDynOff);
            g_cmdDraw(o.cmd, (uint32_t)r.count, 1, (uint32_t)r.first, 0);
            if (PerfOn()) { g_perf.dsBinds++; g_perf.draws++; }
        }
        if (s_useProg && DiagOn() && (g_drawFrame % 120) == 1) {
            fprintf(stderr, "[progpipe] use switches=%llu ranges=%zu ds=%llu f=%llu\n",
                    (unsigned long long)s_progSw, ranges.size(),
                    (unsigned long long)s_progDS, (unsigned long long)g_drawFrame);
        }
    }
    g_endRP(o.cmd);

    // Phase 0：pass 终点针（BOTTOM_OF_PIPE = pass 命令全部完成时写入）
    if (o.passStat && o.passQueries[o.slot] != VK_NULL_HANDLE) {
        g_writeTS(o.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, o.passQueries[o.slot], 1);
    }

    // 取证 VK_BBSTAT=1（默认关）：present 前把后缓冲拷到主机缓冲，供亮度统计。
    // 只在本层 target=0 且有几何时做；barrier 走 PRESENT_SRC -> TRANSFER_SRC -> PRESENT_SRC。
    // 色键扫描：只有窗口带 WS_EX_LAYERED 且距上次扫描 >=120ms 才回读，避免每帧回读。
    bool ckScan = false;
    if (o.ckEnabled && o.nativeWindow) {
        const bool layered = vkplat::WindowIsLayered(o.nativeWindow);
        if (!layered) {
            if (o.ckActive) {
                vkplat::ClearChromaRegion(o.nativeWindow, &o.ckActive);
                fprintf(stderr, "[vk] colorkey region cleared (window no longer layered)\n");
            }
        } else {
            const uint64_t nowCk = vkplat::TicksMs();
            if (nowCk - o.ckLastTick >= 120) { o.ckLastTick = nowCk; ckScan = true; }
        }
    }
    // 雪花探测也要覆盖"空后缓冲"（verts=0 的 present）：整屏白噪声很可能就是
    // 一张什么都没画的交换链图被 present 出去，所以 snowWatch 时不受 totalVerts 限制。
    bool doStat = (o.statBuf != VK_NULL_HANDLE && o.statPtr && o.currentTarget == 0 &&
                   (totalVerts > 0 || (o.snowWatch && finalPresent)) &&
                   (size_t)o.extent.width * (size_t)o.extent.height * 4 <= o.statBytes) &&
                  (o.statWanted || ckScan);
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
    // 帧环：提交后不再 waitIdle，挂该槽的 fence；槽位下一次复用时才等它。
    VkFence submitFence = o.fences[o.slot];
    g_submit(o.queue, 1, &si, submitFence);
    if (PerfOn()) g_perf.submits++;
    if (submitFence != VK_NULL_HANDLE) o.fencePending[o.slot] = true;

    // Phase 0：登记本笔 pass 的元数据，槽位 fence 触发时读回并打 [pass]（含时间戳）
    if (o.passStat && o.passQueries[o.slot] != VK_NULL_HANDLE) {
        o.pendValid[o.slot] = true;
        o.pendFrame[o.slot] = g_drawFrame;
        o.pendTarget[o.slot] = o.currentTarget;
        o.pendExtW[o.slot] = ext.width;
        o.pendExtH[o.slot] = ext.height;
        o.pendRanges[o.slot] = (uint32_t)ranges.size();
        o.pendVerts[o.slot] = (uint32_t)totalVerts;
        o.pendLoad[o.slot] = clearZero ? 2 : (clearPendingUse ? 1 : (o.currentTarget == 0 ? 3 : 0));
        o.pendProgMask[o.slot] = progMask;
    }

    // 读回（stat/转储）必须等 GPU 完成：只有这两条取证路径会把 waitIdle 加回来。
    if (doStat) WaitSlotFence();

    // 本帧命令缓冲已提交：被替换的旧纹理挂到该槽的延迟销毁队列，
    // 等这个槽下一次被取用（fence 已触发，本笔提交确定完成）才真正释放。
    if (!o.pendingTexDestroy.empty()) {
        for (size_t pi = 0; pi < o.pendingTexDestroy.size(); pi++)
            o.slotDestroy[o.slot].push_back(o.pendingTexDestroy[pi]);
        o.pendingTexDestroy.clear();
    }

    if (doStat) {
        const uint8_t* px = (const uint8_t*)o.statPtr;
        // 色键透明：把非色键色区域设为窗口 region（窗口外 = 桌面）。
        // DXGI flip 模型不支持 SetLayeredWindowAttributes 的色键，用 region 等效实现。
        if (ckScan && o.ckEnabled && o.nativeWindow) {
            vkplat::ApplyChromaRegion(o.nativeWindow, px, (int)o.extent.width, (int)o.extent.height,
                                      o.ckR, o.ckG, o.ckB, &o.ckActive);
        }
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
        const double meanV = cnt ? sum / (double)cnt : 0.0;
        // 雪花探测：相邻像素亮度差均值（白噪声 ≈ 80+，正常画面通常 < 30）
        if (o.snowWatch) {
            const size_t W = (size_t)o.extent.width, H = (size_t)o.extent.height;
            double hf = 0.0;
            size_t hn = 0;
            for (size_t yy = 0; yy + 2 < H; yy += 3) {
                for (size_t xx = 0; xx + 2 < W; xx += 3) {
                    size_t i0 = (yy * W + xx) * 4;
                    size_t i1 = (yy * W + xx + 2) * 4;
                    int a = ((int)px[i0] + px[i0 + 1] + px[i0 + 2]) / 3;
                    int b = ((int)px[i1] + px[i1 + 1] + px[i1 + 2]) / 3;
                    hf += (double)(a > b ? a - b : b - a);
                    hn++;
                }
            }
            const double score = hn ? hf / (double)hn : 0.0;
            static uint64_t s_sw = 0;
            s_sw++;
            if (!o.snowDumpReq && score > 45.0 && meanV > 70.0) {
                o.snowDumpReq = true;
                fprintf(stderr, "[snow] hit n=%llu f=%llu score=%.2f mean=%.2f ext=%zux%zu\n",
                        (unsigned long long)s_sw, (unsigned long long)g_drawFrame,
                        score, meanV, W, H);
            } else if (s_sw <= 20000 && (s_sw % 120) == 0) {
                fprintf(stderr, "[snow] n=%llu f=%llu score=%.2f mean=%.2f\n",
                        (unsigned long long)s_sw, (unsigned long long)g_drawFrame, score, meanV);
            }
        }
        if (s_stat <= 20000) {
            fprintf(stderr, "[vk] bbstat n=%llu f=%llu mean=%.2f nb=%.2f img=%u load=%d final=%d\n",
                    (unsigned long long)s_stat, (unsigned long long)g_drawFrame,
                    meanV, cnt ? 100.0 * (double)nb / (double)cnt : 0.0,
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
        // [vkfix] 每帧一行：常量乘数全零折叠成 identity 的笔数 / 总笔数
        if (DiagOn()) fprintf(stderr, "[vkfix] zero_mult_identity=%d/%d\n",
                lime::g_vkfixDrawFolded, lime::g_vkfixDrawTotal);
        lime::g_vkfixDrawFolded = 0;
        lime::g_vkfixDrawTotal = 0;
        // 实验 VK_SINGLE_PRESENT=1：中间合成只提交几何、保持 image 不归还；
        // 只有真实 swap（gl_fbo 调 RequestPresent）才 present 一次。
        if (!willPresent) {
            static uint64_t s_nopres = 0;
            s_nopres++;
            if (DiagOn() && s_nopres <= 20000) {
                fprintf(stderr, "[vk] nopres n=%llu f=%llu tex=%u verts=%zu load=%d\n",
                        (unsigned long long)s_nopres, (unsigned long long)g_drawFrame,
                        g_bbLastTex, totalVerts, o.bbLoad ? 1 : 0);
            }
        } else {
            // 取证：每次 present 一行（一个视觉帧通常 2 次，其中只有 ContextFlip 那次
            // 会在 gl_fbo 里另打一条 [glvk] present），用于把黑帧对齐到交换链图。
            static uint64_t s_pres = 0;
            s_pres++;
            // Phase 0：视觉帧边界标记（[pass] 行按 f= 归属到这个 p 区间）
            if (o.passStat) {
                fprintf(stderr, "[passmark] p=%llu f=%llu\n",
                        (unsigned long long)s_pres, (unsigned long long)g_drawFrame);
            }
            if (DiagOn() && s_pres <= 20000) {
                fprintf(stderr, "[vk] pres n=%llu f=%llu img=%u tex=%u verts=%zu load=%d\n",
                        (unsigned long long)s_pres, (unsigned long long)g_drawFrame,
                        o.frameImageIndex, g_bbLastTex, totalVerts, o.bbLoad ? 1 : 0);
            }
            // 过渡态保护：窗口动画缩放时，交换链尺寸可能还停在旧值。
            // 注意 o.width/o.height 只在 Init 赋一次，改分辨率后会过期，必须现取窗口
            // 客户区尺寸比较（否则永久判定不一致 → 永不 present → 黑屏，实测过）。
            // ObserveWindowSize 每帧都更新动画时间戳（acquire 失败的分支也会调用），
            // 这里直接复用其结果，避免两处计时不同步。
            ObserveWindowSize();
            int winW = o.lastWinW, winH = o.lastWinH;
            bool sizeOK = ((int)o.extent.width == winW && (int)o.extent.height == winH);
            uint64_t nowMs = (uint64_t)vkplat::TicksMs();
            bool resizing = (nowMs - o.lastResizeTick) < 400;
            VkResult pr = VK_SUCCESS;
            // present 之前必须等本次提交执行完（没有 render-finished 信号量，
            // 用该槽的 fence 等效保证交换链图内容完整）
            WaitSlotFence();
            if (sizeOK) {
                o.sizeMismatchSince = 0;
                VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
                pi.swapchainCount = 1;
                pi.pSwapchains = &o.swapchain;
                pi.pImageIndices = &o.frameImageIndex;
                pr = g_present(o.queue, &pi);
                if (PerfOn()) g_perf.presents++;
            } else {
                // 尺寸还没对上（窗口缩放动画中）：仍然必须 present 一次，把这个已 acquire
                // 的交换链图像还给驱动。之前这里只标记 swapDirty 不 present，图像就永远停在
                // acquired 状态；紧接着 RecreateSwapchain 销毁旧交换链，NVIDIA 驱动会直接
                // fast-fail（实测 0xc0000409 @ nvoglv64.dll，窗口从 1282 一路放大到 1594 那次）。
                // 旧尺寸图 present 出去最多是缩放动画中闪一帧，不会崩。
                //
                // 重建节流：动画期间每帧 waitIdle+重建会把帧时间拉长；稳定 200ms 后只重建一次。
                uint64_t nowMs2 = (uint64_t)vkplat::TicksMs();
                if (o.sizeMismatchSince == 0) o.sizeMismatchSince = nowMs2;
                if (!resizing && nowMs2 - o.sizeMismatchSince >= 200) {
                    o.swapDirty = true;
                }
                VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
                pi.swapchainCount = 1;
                pi.pSwapchains = &o.swapchain;
                pi.pImageIndices = &o.frameImageIndex;
                pr = g_present(o.queue, &pi);
                if (PerfOn()) g_perf.presents++;
                static uint64_t s_skip = 0;
                if (s_skip < 40) {
                    s_skip++;
                    fprintf(stderr, "[vk] present resize swap=%ux%u win=%dx%d f=%llu pr=%d wait=%llums\n",
                            o.extent.width, o.extent.height, winW, winH,
                            (unsigned long long)g_drawFrame, (int)pr,
                            (unsigned long long)(nowMs2 - o.sizeMismatchSince));
                }
            }
            static uint64_t s_badPres = 0;
            if (resizing) {
                // 窗口缩放动画中：present 必然 OUT_OF_DATE/SUBOPTIMAL，全部忽略，
                // 只靠上面的稳定计时在动画结束后重建一次。
            } else if (pr == VK_ERROR_OUT_OF_DATE_KHR) {
                o.swapDirty = true;
                fprintf(stderr, "[vk] swapchain dirty reason=present err=%d f=%llu img=%u\n",
                        (int)pr, (unsigned long long)g_drawFrame, o.frameImageIndex);
            } else if (pr == VK_SUBOPTIMAL_KHR) {
                o.swapDirty = true;
            }
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

    // 取证转储（批处理路径；packet 路径在 PktEndTarget 里触发）：要先等本笔执行完才能读回。
    if (TexDumpConfig().on) WaitSlotFence();
    MaybeDumpTextures(lime::pkt::ExecActive() ? (uint64_t)lime::pkt::SnapshotFrameGen()
                                              : (uint64_t)g_drawFrame);

    if (PerfOn()) {
        double dt = PerfNowMs() - perfT0;
        g_perf.endMs += dt;
        if (dt > g_perf.maxEndMs) g_perf.maxEndMs = dt;
        PerfFlushIfDue();
    }

    // 本槽用完了：下一笔 EndFrame 从下一个槽开录（真正的流水来自这里）
    o.slot = (o.slot + 1) % VK_RING;
    o.frameOpen = false;
    g_bbLastTex = 0;
    // 触发式转储：每个视觉帧重新累积采样清单，避免把整场跑过的几百张纹理都转出来
    if (TexDumpConfig().on && (TexDumpConfig().snowAuto || !TexDumpConfig().trigger.empty()) &&
        finalPresent && o.currentTarget == 0) {
        o.sampled.clear();
    }
    for (auto& t : o.textures) { t.verts.clear(); t.progs.clear(); }
    for (auto& t : o.targets) { t.verts.clear(); t.progs.clear(); }
    o.segs.clear();
}
