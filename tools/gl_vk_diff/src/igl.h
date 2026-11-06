// GL 语义抽象层：同一组测试场景分别下发到「GL/ANGLE 参考实现」与「Vulkan 翻译层」。
// 场景只调用这里的接口，不含任何后端相关代码，从而保证两条路径吃到的输入完全一致。
#pragma once
#include <cstddef>

#include "image.h"
#include "perf.h"

namespace gvd {

// 只用到的一小撮 GL 枚举，值与 GL 规范一致。
enum GlEnum : int {
    GL_TRIANGLES              = 0x0004,
    GL_FLOAT                  = 0x1406,
    GL_UNSIGNED_BYTE          = 0x1401,
    GL_TEXTURE_2D             = 0x0DE1,
    GL_ARRAY_BUFFER           = 0x8892,
    GL_ELEMENT_ARRAY_BUFFER   = 0x8893,
    GL_FRAMEBUFFER            = 0x8D40,
    GL_COLOR_ATTACHMENT0      = 0x8CE0,
    GL_RGBA                   = 0x1908,
    GL_RGBA8                  = 0x8058,
    GL_BLEND                  = 0x0BE2,
    GL_SCISSOR_TEST           = 0x0C11,
    GL_DEPTH_TEST             = 0x0B71,
    GL_CULL_FACE              = 0x0B44,
    GL_COLOR_BUFFER_BIT       = 0x4000,
    GL_SRC_ALPHA              = 0x0302,
    GL_ONE_MINUS_SRC_ALPHA    = 0x0303,
    GL_ONE                    = 0x0001,
    GL_ZERO                   = 0x0000,
    GL_NEAREST                = 0x2600,
    GL_CLAMP_TO_EDGE          = 0x812F,
    GL_TEXTURE_MIN_FILTER     = 0x2801,
    GL_TEXTURE_MAG_FILTER     = 0x2800,
    GL_TEXTURE_WRAP_S         = 0x2802,
    GL_TEXTURE_WRAP_T         = 0x2803,
    GL_STATIC_DRAW            = 0x88E4,
    GL_DYNAMIC_DRAW           = 0x88E8,
    GL_TEXTURE0               = 0x84C0
};

// 纹理顶点布局，和 flixel / gl_core 的期望一致：x, y, u, v（像素坐标，原点左上）。
struct Vertex { float x, y, u, v; };
static_assert(sizeof(Vertex) == 16, "Vertex 必须是 16 字节，才能与 stride=16 的假设相符");

// 后端能力位。判定策略：
//  - Has()==false  → 该场景对这条路径标记为 UNSUPPORTED（记入缺口清单，不算像素失败）
//  - Has()==true   → 场景照常跑；如果像素不一致，说明接口「装着能用其实没生效」，属真实 bug
enum Caps : int {
    CAP_FBO = 0,          // 离屏渲染目标（GL FBO 语义）
    CAP_TINT,             // 逐 draw 的 tint/alpha（uniform 上传）
    CAP_SCISSOR,          // 裁剪
    CAP_BLEND,            // 混合开关/因子
    CAP_COLORMASK,        // 写掩码
    CAP_CLEAR_TO_TARGET,  // glClear 作用于当前绑定的目标
    CAP_COUNT
};

enum Status : int {
    ST_PASS = 0,
    ST_DIFF,
    ST_UNSUPPORTED,
    ST_ERROR
};

const char* StatusName(Status s);

// 场景用来判断「这条后端能不能跑」的统一入口。
class IGl {
public:
    virtual ~IGl() {}
    virtual const char* Name() const = 0;
    virtual bool Has(Caps c) const = 0;
    virtual const char* Why(Caps c) const = 0;  // Has()==false 时给出原因

    // ---- 纹理 ----
    virtual int  CreateTexture() = 0;
    virtual void BindTexture(int tex) = 0;
    // rgba 为 nullptr 表示只分配存储（给 FBO 当颜色附件用）
    virtual void TexImage2D(int level, int w, int h, const unsigned char* rgba) = 0;

    // ---- 顶点缓冲 / 绘制 ----
    virtual int  CreateBuffer() = 0;
    virtual void BindBuffer(int buf) = 0;
    virtual void BufferData(const void* data, size_t bytes) = 0;
    virtual void VertexAttribPointer(int index, int size, size_t stride, size_t offset) = 0;
    virtual void EnableVertexAttribArray(int index) = 0;
    virtual void DrawArrays(int first, int count) = 0;

    // ---- 离屏目标 ----
    virtual int  CreateFramebuffer() = 0;
    virtual void BindFramebuffer(int fb) = 0;  // 0 = 本帧的捕获目标
    virtual void FramebufferTexture2D(int tex) = 0;

    // ---- 状态 ----
    virtual void Enable(int cap) = 0;
    virtual void Disable(int cap) = 0;
    virtual void BlendFunc(int src, int dst) = 0;
    virtual void ColorMask(bool r, bool g, bool b, bool a) = 0;
    virtual void Scissor(int x, int y, int w, int h) = 0;   // GL 约定：原点左下
    virtual void Viewport(int x, int y, int w, int h) = 0;
    virtual void ClearColor(float r, float g, float b, float a) = 0;
    virtual void Clear(int mask) = 0;
    virtual void SetTint(float r, float g, float b, float a) = 0;

    // ---- 帧 ----
    virtual bool BeginFrame(int w, int h, const float clear[4]) = 0;
    virtual bool EndFrame() = 0;
    // 关掉截图后，EndFrame 只做「提交 + 等待」，不做读回。
    // 性能基线必须关掉它，否则测出来的是读回开销，不是渲染开销。
    virtual void SetCaptureEnabled(bool) {}
    virtual const Image& Captured() const = 0;
    virtual const char* LastProblem() const = 0;
    virtual Counters& Perf() = 0;
};

}  // namespace gvd
