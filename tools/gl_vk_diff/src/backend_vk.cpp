// Vulkan 后端：两条路径共用同一套 VkRenderer 实例管理。
//  mode=0 (vk-direct)    ：工装直接调 VkRenderer，回答「渲染器本身画得对不对」
//  mode=1 (vk-translate)：走 lime_gl_* 翻译层，回答「GL 语义翻译得对不对」
//
// 说明：这里 #include 了 ../../native_vulkan/vk_renderer.cpp，为了复用同一份实现而不是复制代码；
// 同时借 private->public 拿到内部 RenderTarget 图片做读回，因为公开的 ReadPixels 目前还是空实现。
// native_vulkan/ 下的文件一行都没改。
#include "backends.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#define VK_USE_PLATFORM_WIN32_KHR
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#define private public
#include "vk_renderer.h"
#undef private

#include "../../../native_vulkan/vk_renderer.cpp"
#include "gl_vk_bridge.h"

namespace gvd {
namespace {

HWND MakeHiddenWindow(int w, int h) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSA wc;
        std::memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = GetModuleHandleA(nullptr);
        wc.lpszClassName = "gvd_hidden_vk";
        if (!RegisterClassA(&wc)) return nullptr;
        registered = true;
    }
    // 不 ShowWindow：交换链建在隐藏窗口上，离屏结果从 RT 读回，不依赖窗口可见性。
    return CreateWindowExA(0, "gvd_hidden_vk", "gvd", WS_POPUP, 0, 0, w, h, nullptr, nullptr,
                           GetModuleHandleA(nullptr), nullptr);
}

uint32_t CurrentTargetOf(VkRenderer& r) { return r.impl->currentTarget; }

// 从 VkRenderer 的离屏目标读回像素，并翻转成「第 0 行 = 底部」的 GL 约定。
bool ReadTargetImage(VkRenderer& r, uint32_t id, Image& out) {
    const bool dbg = std::getenv("GVD_DEBUG") != nullptr;
    if (id < 1000) {
        if (dbg) fprintf(stderr, "[vkread] id=%u 不是离屏目标\n", id);
        return false;
    }
    VkRenderer::Impl& o = *r.impl;
    uint32_t ti = id - 1000;
    if (ti >= o.targets.size() || !o.targets[ti].used) {
        if (dbg) fprintf(stderr, "[vkread] id=%u ti=%u size=%zu 不存在\n", id, ti, o.targets.size());
        return false;
    }
    RenderTarget& t = o.targets[ti];
    if (t.w <= 0 || t.h <= 0) return false;
    if (dbg) fprintf(stderr, "[vkread] id=%u used=%d size=%dx%d\n", id, (int)t.used, t.w, t.h);

    const int w = t.w, h = t.h;
    const size_t bytes = (size_t)w * h * 4;

    VkBufferCreateInfo bci;
    std::memset(&bci, 0, sizeof(bci));
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buf = VK_NULL_HANDLE;
    if (g_createBuf(o.dev, &bci, nullptr, &buf) != VK_SUCCESS) return false;

    VkMemoryRequirements mr;
    g_getBufReqs(o.dev, buf, &mr);
    PFN_vkGetPhysicalDeviceMemoryProperties getMemProps =
        (PFN_vkGetPhysicalDeviceMemoryProperties)g_gipa(o.inst, "vkGetPhysicalDeviceMemoryProperties");
    if (!getMemProps) return false;
    VkPhysicalDeviceMemoryProperties mp;
    getMemProps(o.phys, &mp);
    uint32_t mt = FindMemType(mp, mr.memoryTypeBits,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    VkMemoryAllocateInfo mai;
    std::memset(&mai, 0, sizeof(mai));
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = mt;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (g_allocMem(o.dev, &mai, nullptr, &mem) != VK_SUCCESS) {
        o.destroyBuffer(o.dev, buf, nullptr);
        return false;
    }
    g_bindBufMem(o.dev, buf, mem, 0);

    PFN_vkAllocateCommandBuffers allocCB =
        (PFN_vkAllocateCommandBuffers)g_gipa(o.inst, "vkAllocateCommandBuffers");
    PFN_vkFreeCommandBuffers freeCB = (PFN_vkFreeCommandBuffers)g_gipa(o.inst, "vkFreeCommandBuffers");
    PFN_vkCmdCopyImageToBuffer copyImg =
        (PFN_vkCmdCopyImageToBuffer)g_gipa(o.inst, "vkCmdCopyImageToBuffer");
    if (!allocCB || !freeCB || !copyImg) {
        o.freeMemory(o.dev, mem, nullptr);
        o.destroyBuffer(o.dev, buf, nullptr);
        return false;
    }

    VkCommandBufferAllocateInfo cbi;
    std::memset(&cbi, 0, sizeof(cbi));
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbi.commandPool = o.cmdPool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    allocCB(o.dev, &cbi, &cb);

    VkCommandBufferBeginInfo bi;
    std::memset(&bi, 0, sizeof(bi));
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    g_beginCB(cb, &bi);

    // EndFrame 的 render pass 把目标留在 SHADER_READ_ONLY；这里从该布局转到 TRANSFER_SRC。
    // （不能用 UNDEFINED 当 oldLayout：那等于声明「内容可丢弃」，实测部分驱动会把渲染结果丢掉。）
    VkImageMemoryBarrier b;
    std::memset(&b, 0, sizeof(b));
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = t.image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    g_cmdBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);

    VkBufferImageCopy region;
    std::memset(&region, 0, sizeof(region));
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width = (uint32_t)w;
    region.imageExtent.height = (uint32_t)h;
    region.imageExtent.depth = 1;
    copyImg(cb, t.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);

    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    g_cmdBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                 nullptr, 0, nullptr, 1, &b);

    g_endCB(cb);
    VkSubmitInfo si;
    std::memset(&si, 0, sizeof(si));
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    g_submit(o.queue, 1, &si, VK_NULL_HANDLE);
    g_waitIdle(o.queue);

    out.Alloc(w, h);
    void* p = nullptr;
    VkResult mrres = g_mapMem(o.dev, mem, 0, bytes, 0, &p);
    if (dbg) fprintf(stderr, "[vkread] map=%d p=%p first=(%u,%u,%u,%u)\n", (int)mrres, p,
                     p ? ((unsigned char*)p)[0] : 0, p ? ((unsigned char*)p)[1] : 0,
                     p ? ((unsigned char*)p)[2] : 0, p ? ((unsigned char*)p)[3] : 0);
    if (mrres == VK_SUCCESS && p) {
        for (int y = 0; y < h; ++y) {
            std::memcpy(out.At(0, h - 1 - y), (const uint8_t*)p + (size_t)y * w * 4, (size_t)w * 4);
        }
        g_unmapMem(o.dev, mem);
    }

    freeCB(o.dev, o.cmdPool, 1, &cb);
    o.destroyBuffer(o.dev, buf, nullptr);
    o.freeMemory(o.dev, mem, nullptr);
    return p != nullptr;
}

struct CapInfo {
    bool ok = false;
    const char* why = "";
};

class VkBackend : public IGl {
public:
    VkBackend(int mode, int w, int h) : mode_(mode), reqW_(w), reqH_(h) {}

    ~VkBackend() override {
        if (r_) {
            r_->Shutdown();
            if (mode_ == 1) {
                // 先把翻译层指回空，再把它的帧状态清零，避免下一个场景读到上个渲染器的残留 id
                lime::gl_vk_attach_renderer(nullptr);
                lime::gl_vk_present_frame();
            }
            delete r_;
            r_ = nullptr;
        }
        if (hwnd_) {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
    }

    const char* Name() const override { return mode_ == 0 ? "vk-direct" : "vk-translate"; }
    bool Has(Caps c) const override { return caps_[c].ok; }
    const char* Why(Caps c) const override { return caps_[c].why; }
    Counters& Perf() override { return counters_; }
    const Image& Captured() const override { return img_; }
    const char* LastProblem() const override { return problem_.c_str(); }

    bool Init() {
        hwnd_ = MakeHiddenWindow(reqW_, reqH_);
        if (!hwnd_) {
            problem_ = "创建隐藏窗口失败";
            return false;
        }
        r_ = new VkRenderer();
        if (!r_->Init(hwnd_, reqW_, reqH_)) {
            const char* e = r_->LastError();
            problem_ = std::string("VkRenderer::Init 失败: ") + (e ? e : "(无错误信息)");
            return false;
        }
        if (mode_ == 1) {
            lime::gl_vk_attach_renderer(r_);
            // 清掉上一个场景留在翻译层里的帧/FBO 绑定状态（此时 frameOpen=false，EndFrame 直接返回）
            lime::gl_vk_present_frame();
        }
        resetCB_ = (PFN_vkResetCommandBuffer)g_gipa(r_->impl->inst, "vkResetCommandBuffer");
        fw_ = reqW_;
        fh_ = reqH_;

        // 能力声明。SCISSOR / BLEND 故意声明为「可用」：
        // 翻译层接口确实存在，跑起来才能暴露「记录状态但没下发」这种静默错误。
        caps_[CAP_FBO] = {true, ""};
        caps_[CAP_TINT] = {false, "没有逐 draw 颜色：着色器缺少 tint uniform，翻译层的 uniform 上传未实现"};
        if (mode_ == 0) {
            // direct 模式只能直接调 VkRenderer 公开接口，它没有 scissor
            caps_[CAP_SCISSOR] = {false, "VkRenderer 公开接口没有 scissor，direct 模式无法表达"};
        } else {
            // translate 模式接口存在（lime_gl_scissor），故意声明可用：
            // 跑起来若像素不符，就证明「状态只被记录、没有下发」。
            caps_[CAP_SCISSOR] = {true, ""};
        }
        caps_[CAP_BLEND] = {true, ""};
        caps_[CAP_COLORMASK] = {false, "VkRenderer 管线没有动态颜色写掩码"};
        if (mode_ == 0) {
            caps_[CAP_CLEAR_TO_TARGET] = {false, "VkRenderer 没有独立的 clear 接口，只能通过 BindRenderTarget 清屏"};
        } else {
            caps_[CAP_CLEAR_TO_TARGET] = {true, ""};
        }
        return true;
    }

    // ---- 纹理 ----
    int CreateTexture() override {
        int name = nextTex_++;
        texNames_.push_back(name);
        return name;
    }

    void BindTexture(int tex) override {
        curTex_ = tex;
        texBound_ = tex;
        if (mode_ == 1) {
            lime::lime_gl_active_texture(GL_TEXTURE0);
            lime::lime_gl_bind_texture(GL_TEXTURE_2D, tex);
        }
    }

    void TexImage2D(int level, int w, int h, const unsigned char* rgba) override {
        texW_[texBound_] = w;
        texH_[texBound_] = h;
        if (rgba) {
            counters_.texUploads++;
            counters_.texBytes += (uint64_t)w * h * 4;
        }
        if (mode_ == 0) {
            uint32_t id = rgba ? r_->CreateTexture(w, h, rgba) : r_->CreateEmptyTexture(w, h);
            texVk_[texBound_] = id;
        } else {
            lime::lime_gl_active_texture(GL_TEXTURE0);
            lime::lime_gl_bind_texture(GL_TEXTURE_2D, texBound_);
            lime::lime_gl_tex_image_2d(GL_TEXTURE_2D, level, GL_RGBA8, w, h, 0, GL_RGBA,
                                       GL_UNSIGNED_BYTE, (double)(uintptr_t)rgba);
        }
    }

    // ---- 顶点 ----
    int CreateBuffer() override {
        int name = nextBuf_++;
        bufs_[name] = std::vector<Vertex>();
        return name;
    }

    void BindBuffer(int buf) override {
        curBuf_ = buf;
        if (mode_ == 1) lime::lime_gl_bind_buffer(GL_ARRAY_BUFFER, buf);
    }

    void BufferData(const void* data, size_t bytes) override {
        auto& v = bufs_[curBuf_];
        v.resize(bytes / sizeof(Vertex));
        if (bytes) std::memcpy(v.data(), data, bytes);
        if (mode_ == 1)
            lime::lime_gl_buffer_data(GL_ARRAY_BUFFER, (int)bytes, (double)(uintptr_t)data, GL_DYNAMIC_DRAW);
    }

    void VertexAttribPointer(int index, int size, size_t stride, size_t offset) override {
        if (index < 0 || index > 1) return;
        attrSize_[index] = size;
        attrStride_[index] = (size_t)stride;
        attrOffset_[index] = (size_t)offset;
        if (mode_ == 1)
            lime::lime_gl_vertex_attrib_pointer(index, size, GL_FLOAT, false, (int)stride, (double)offset);
    }

    void EnableVertexAttribArray(int index) override {
        if (mode_ == 1) lime::lime_gl_enable_vertex_attrib_array(index);
    }

    void DrawArrays(int first, int count) override {
        counters_.draws++;
        counters_.quads += (uint64_t)count;
        if (mode_ == 1) {
            lime::lime_gl_draw_arrays(GL_TRIANGLES, first, count);
            return;
        }
        // 直连模式：自己按 attrs 描述把顶点摘出来，交给 DrawRaw
        auto it = bufs_.find(curBuf_);
        if (it == bufs_.end()) return;
        const std::vector<Vertex>& src = it->second;
        std::vector<float> out;
        out.reserve((size_t)count * 4);
        for (int i = 0; i < count; ++i) {
            int idx = first + i;
            if (idx < 0 || idx >= (int)src.size()) break;
            out.push_back(src[idx].x);
            out.push_back(src[idx].y);
            out.push_back(src[idx].u);
            out.push_back(src[idx].v);
        }
        if (out.empty()) return;
        uint32_t texId = 0;
        auto rt = texToRt_.find(texBound_);
        if (rt != texToRt_.end())
            texId = (uint32_t)rt->second;
        else {
            auto tv = texVk_.find(texBound_);
            if (tv != texVk_.end()) texId = tv->second;
        }
        if (texId == 0) return;
        r_->DrawRaw(texId, out.data(), (int)(out.size() / 4));
    }

    // ---- 离屏 ----
    int CreateFramebuffer() override {
        if (mode_ == 1) {
            int fb = lime::lime_gl_create_framebuffer();
            fbIds_.insert(fb);
            return fb;
        }
        int fb = nextFb_++;
        fbIds_.insert(fb);
        return fb;
    }

    void BindFramebuffer(int fb) override {
        // 0 统一指回本帧的捕获目标（工装自己持有的 RT），
        // 这样场景中途「切回主目标」也能落在可读回的地方。
        if (fb == 0) {
            Flush();
            r_->BindRenderTarget(mainRT_, clear_[0], clear_[1], clear_[2], clear_[3]);
            return;
        }
        if (mode_ == 1) {
            lime::lime_gl_bind_framebuffer(GL_FRAMEBUFFER, fb);
            curFb_ = fb;
            fbRt_[fb] = (int)CurrentTargetOf(*r_);
            return;
        }
        Flush();
        curFb_ = fb;
        auto it = fbRt_.find(fb);
        if (it == fbRt_.end()) return;  // 还没 attach 纹理，没有可画的目标
        r_->BindRenderTarget((uint32_t)it->second, clear_[0], clear_[1], clear_[2], clear_[3]);
    }

    void FramebufferTexture2D(int tex) override {
        if (mode_ == 1) {
            lime::lime_gl_framebuffer_texture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
            // 附件是在 bind 之后才挂上的：先问一次完整性让 gl_fbo 建出 RT，
            // 再重新 bind 一次（此时 rtId 已非 0，BindDrawFbo 才会真正切目标）。
            lime::lime_gl_check_framebuffer_status(GL_FRAMEBUFFER);
            lime::lime_gl_bind_framebuffer(GL_FRAMEBUFFER, curFb_);
            fbRt_[curFb_] = (int)CurrentTargetOf(*r_);
            return;
        }
        int w = 8, h = 8;
        auto iw = texW_.find(tex);
        auto ih = texH_.find(tex);
        if (iw != texW_.end()) w = iw->second;
        if (ih != texH_.end()) h = ih->second;
        uint32_t rt = r_->CreateRenderTarget(w, h);
        fbRt_[curFb_] = (int)rt;
        texToRt_[tex] = (int)rt;
        // GL 语义：挂上附件时目标 FBO 就是当前绘制目标，这里立刻切过去（先冲刷上一段）
        Flush();
        r_->BindRenderTarget(rt, clear_[0], clear_[1], clear_[2], clear_[3]);
    }

    // ---- 状态 ----
    void Enable(int cap) override {
        if (mode_ == 1) lime::lime_gl_enable(cap);
    }
    void Disable(int cap) override {
        if (mode_ == 1) lime::lime_gl_disable(cap);
    }
    void BlendFunc(int src, int dst) override {
        if (mode_ == 1) lime::lime_gl_blend_func(src, dst);
    }
    void ColorMask(bool r, bool g, bool b, bool a) override {
        if (mode_ == 1) lime::lime_gl_color_mask(r, g, b, a);
    }
    void Scissor(int x, int y, int w, int h) override {
        if (mode_ == 1) lime::lime_gl_scissor(x, y, w, h);
    }
    void Viewport(int x, int y, int w, int h) override {
        if (mode_ == 1) lime::lime_gl_viewport(x, y, w, h);
    }
    void ClearColor(float r, float g, float b, float a) override {
        if (mode_ == 1) lime::lime_gl_clear_color(r, g, b, a);
    }
    void Clear(int mask) override {
        (void)mask;
        if (mode_ == 1) lime::lime_gl_clear(GL_COLOR_BUFFER_BIT);
    }
    void SetTint(float, float, float, float) override {}

    // ---- 帧 ----
    bool BeginFrame(int w, int h, const float clear[4]) override {
        if (!r_) return false;
        for (int i = 0; i < 4; ++i) clear_[i] = clear[i];
        if (mainRT_ == 0 || fw_ != w || fh_ != h) {
            mainRT_ = r_->CreateRenderTarget(w, h);
            if (mainRT_ == 0) {
                problem_ = "CreateRenderTarget 失败";
                return false;
            }
            fw_ = w;
            fh_ = h;
        }
        if (!r_->BindRenderTarget(mainRT_, clear[0], clear[1], clear[2], clear[3])) {
            problem_ = "BindRenderTarget 失败";
            return false;
        }
        if (mode_ == 1) {
            lime::lime_gl_clear_color(clear[0], clear[1], clear[2], clear[3]);
            lime::lime_gl_viewport(0, 0, w, h);
        }
        return true;
    }

    bool EndFrame() override {
        if (!r_ || mainRT_ == 0) return false;
        Flush();
        counters_.presents++;
        if (!capture_) return true;
        if (!ReadTargetImage(*r_, mainRT_, img_)) {
            problem_ = "从 VkRenderer 的离屏目标读回像素失败";
            return false;
        }
        return true;
    }

    void SetCaptureEnabled(bool on) override { capture_ = on; }

private:
    // VkRenderer::EndFrame 只在首次使用时能录命令（它从不重置自己的命令缓冲，
    // 第二次 vkBeginCommandBuffer 会失败且返回值被忽略 → 之后所有帧静默丢失）。
    // 上游缺陷不在本工装修复范围内，这里每次冲刷前先替它 reset。
    void Flush() {
        if (!r_) return;
        if (resetCB_) resetCB_(r_->impl->cmd, 0);
        r_->PresentTarget();
    }

    int mode_ = 0;
    int reqW_ = 0, reqH_ = 0;
    HWND hwnd_ = nullptr;
    VkRenderer* r_ = nullptr;
    int fw_ = 0, fh_ = 0;
    float clear_[4] = {0, 0, 0, 1};
    uint32_t mainRT_ = 0;
    PFN_vkResetCommandBuffer resetCB_ = nullptr;
    bool capture_ = true;
    Image img_;
    std::string problem_;
    Counters counters_;
    CapInfo caps_[CAP_COUNT];

    int nextTex_ = 1, nextBuf_ = 1, nextFb_ = 100;
    int curTex_ = 0, curBuf_ = 0, texBound_ = 0, curFb_ = 0;
    std::vector<int> texNames_;
    std::map<int, int> texVk_;
    std::map<int, int> texW_, texH_;
    std::map<int, int> texToRt_;
    std::map<int, int> fbRt_;
    std::map<int, std::vector<Vertex>> bufs_;
    std::set<int> fbIds_;
    size_t attrSize_[2] = {2, 2};
    size_t attrStride_[2] = {16, 16};
    size_t attrOffset_[2] = {0, 8};
};

}  // namespace

IGl* CreateVkBackend(int mode, int width, int height, std::string& problem) {
    VkBackend* b = new VkBackend(mode, width, height);
    if (!b->Init()) {
        problem = b->LastProblem();
        delete b;
        return nullptr;
    }
    return b;
}

}  // namespace gvd
