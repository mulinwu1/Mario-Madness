#include "scenes.h"

#include <vector>

namespace gvd {

const char* StatusName(Status s) {
    switch (s) {
        case ST_PASS: return "PASS";
        case ST_DIFF: return "DIFF";
        case ST_UNSUPPORTED: return "UNSUPPORTED";
        case ST_ERROR: return "ERROR";
    }
    return "UNKNOWN";
}

namespace {

const int kTex = 64;
using Bytes = std::vector<unsigned char>;

// 棋盘格 + 非对称方向标记：上半部有红条，左上角有绿块。
// 这样一旦出现上下翻转 / 左右翻转，肉眼和哈希都会立刻暴露。
Bytes TexChecker() {
    Bytes b((size_t)kTex * kTex * 4);
    for (int y = 0; y < kTex; ++y) {
        for (int x = 0; x < kTex; ++x) {
            unsigned char v = (((x / 8) + (y / 8)) & 1) ? 255 : 90;
            unsigned char r = v, g = v, bl = v;
            if (y < 6) { r = 255; g = 0; bl = 0; }                 // 顶部红条
            if (x < 4) { r = 0; g = 0; bl = 255; }                 // 左侧蓝条
            if (x >= 8 && x < 24 && y >= 8 && y < 24) { r = 0; g = 255; bl = 0; }  // 左上角绿块
            unsigned char* p = &b[((size_t)y * kTex + x) * 4];
            p[0] = r; p[1] = g; p[2] = bl; p[3] = 255;
        }
    }
    return b;
}

// 竖直渐变，用来验证采样坐标与通道顺序。
Bytes TexGradient() {
    Bytes b((size_t)kTex * kTex * 4);
    for (int y = 0; y < kTex; ++y) {
        for (int x = 0; x < kTex; ++x) {
            unsigned char* p = &b[((size_t)y * kTex + x) * 4];
            p[0] = (unsigned char)(255 - y * 255 / (kTex - 1));
            p[1] = 40;
            p[2] = (unsigned char)(y * 255 / (kTex - 1));
            p[3] = 255;
        }
    }
    return b;
}

// 径向 alpha 圆盘，用来验证混合。
Bytes TexCircle() {
    Bytes b((size_t)kTex * kTex * 4);
    const float c = (kTex - 1) * 0.5f;
    for (int y = 0; y < kTex; ++y) {
        for (int x = 0; x < kTex; ++x) {
            float dx = x - c, dy = y - c;
            float d = (dx * dx + dy * dy);
            float r = c;
            float a = d > r * r ? 0.0f : (1.0f - d / (r * r));
            unsigned char* p = &b[((size_t)y * kTex + x) * 4];
            p[0] = 40; p[1] = 200; p[2] = 255;
            p[3] = (unsigned char)(a * 255.0f);
        }
    }
    return b;
}

Bytes TexSolid(unsigned char r, unsigned char g, unsigned char bl) {
    Bytes b((size_t)8 * 8 * 4);
    for (size_t i = 0; i < 64; ++i) {
        b[i * 4 + 0] = r; b[i * 4 + 1] = g; b[i * 4 + 2] = bl; b[i * 4 + 3] = 255;
    }
    return b;
}

std::vector<Vertex> Quad(float x0, float y0, float x1, float y1) {
    std::vector<Vertex> v(6);
    v[0] = {x0, y0, 0.0f, 0.0f};
    v[1] = {x1, y0, 1.0f, 0.0f};
    v[2] = {x1, y1, 1.0f, 1.0f};
    v[3] = {x0, y0, 0.0f, 0.0f};
    v[4] = {x1, y1, 1.0f, 1.0f};
    v[5] = {x0, y1, 0.0f, 1.0f};
    return v;
}

int MakeTexture(IGl& gl, const Bytes& bytes, int size) {
    int tex = gl.CreateTexture();
    gl.BindTexture(tex);
    gl.TexImage2D(0, size, size, bytes.data());
    return tex;
}

void UploadVertices(IGl& gl, const std::vector<Vertex>& verts) {
    int vb = gl.CreateBuffer();
    gl.BindBuffer(vb);
    gl.BufferData(verts.data(), verts.size() * sizeof(Vertex));
    gl.VertexAttribPointer(0, 2, sizeof(Vertex), 0);
    gl.EnableVertexAttribArray(0);
    gl.VertexAttribPointer(1, 2, sizeof(Vertex), 8);
    gl.EnableVertexAttribArray(1);
}

void DrawQuad(IGl& gl, int tex, float x0, float y0, float x1, float y1) {
    gl.BindTexture(tex);
    UploadVertices(gl, Quad(x0, y0, x1, y1));
    gl.DrawArrays(0, 6);
}

// ---------- 场景 ----------

bool Scene01Clear(IGl& gl, int, int, std::string&) {
    // 只要帧起点的 clear 正确，画面就应该是纯色。
    (void)gl;
    return true;
}

bool Scene02OneTexture(IGl& gl, int w, int h, std::string&) {
    int tex = MakeTexture(gl, TexChecker(), kTex);
    float cx = w * 0.5f, cy = h * 0.5f;
    DrawQuad(gl, tex, cx - 96, cy - 96, cx + 96, cy + 96);
    return true;
}

bool Scene03MultiTexture(IGl& gl, int, int, std::string&) {
    int texs[3] = {MakeTexture(gl, TexChecker(), kTex), MakeTexture(gl, TexGradient(), kTex),
                   MakeTexture(gl, TexCircle(), kTex)};
    for (int t = 0; t < 3; ++t) {
        std::vector<Vertex> all;
        for (int i = 0; i < 4; ++i) {
            float x0 = 40.0f + i * 150.0f;
            float y0 = 40.0f + t * 90.0f;
            std::vector<Vertex> q = Quad(x0, y0, x0 + 120, y0 + 70);
            all.insert(all.end(), q.begin(), q.end());
        }
        gl.BindTexture(texs[t]);
        UploadVertices(gl, all);
        gl.DrawArrays(0, (int)all.size());
    }
    return true;
}

bool Scene04TintAlpha(IGl& gl, int w, int h, std::string& note) {
    if (!gl.Has(CAP_TINT)) {
        note = gl.Why(CAP_TINT);
        return false;
    }
    int tex = MakeTexture(gl, TexGradient(), kTex);
    gl.BindTexture(tex);
    gl.Enable(GL_BLEND);
    gl.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // 半透明 + 冷色调 tint
    gl.SetTint(0.35f, 0.75f, 1.0f, 0.55f);
    UploadVertices(gl, Quad(w * 0.5f - 150, 60.0f, w * 0.5f - 10, 220.0f));
    gl.DrawArrays(0, 6);

    // 纯 tint 改变颜色、不改变 alpha
    gl.SetTint(1.0f, 0.45f, 0.2f, 1.0f);
    UploadVertices(gl, Quad(w * 0.5f + 10, 60.0f, w * 0.5f + 150, 220.0f));
    gl.DrawArrays(0, 6);

    gl.SetTint(1.0f, 1.0f, 1.0f, 1.0f);
    gl.Disable(GL_BLEND);
    return true;
}

bool Scene05Offscreen(IGl& gl, int w, int h, std::string& note) {
    if (!gl.Has(CAP_FBO)) {
        note = gl.Why(CAP_FBO);
        return false;
    }
    int src = MakeTexture(gl, TexChecker(), kTex);

    // RT 尺寸故意与窗口一致（gl_core 的 DrawRaw 用窗口 extent 做 NDC 换算，
    // 尺寸不一致会被额外缩放，这条单独记缺口，不在这里混淆结论）。
    int rtTex = gl.CreateTexture();
    gl.BindTexture(rtTex);
    gl.TexImage2D(0, w, h, nullptr);

    int fb = gl.CreateFramebuffer();
    if (fb == 0) {
        note = "glCreateFramebuffer 返回 0";
        return false;
    }
    gl.BindFramebuffer(fb);
    gl.FramebufferTexture2D(rtTex);

    // pass 1：把 64x64 纹理放大铺满整个 RT（全屏覆盖，不需要 clear）
    gl.BindTexture(src);
    UploadVertices(gl, Quad(0, 0, (float)w, (float)h));
    gl.DrawArrays(0, 6);

    gl.BindFramebuffer(0);

    // pass 2：把 RT 当纹理，1:1 铺满主目标 → 结果应是「放大 10 倍的棋盘格」
    gl.BindTexture(rtTex);
    UploadVertices(gl, Quad(0, 0, (float)w, (float)h));
    gl.DrawArrays(0, 6);
    return true;
}

bool Scene06BlendOverlay(IGl& gl, int w, int h, std::string& note) {
    if (!gl.Has(CAP_BLEND)) {
        note = gl.Why(CAP_BLEND);
        return false;
    }
    // 注意：故意让「创建顺序」与「绘制顺序」相反。
    // VkRenderer::EndFrame 是按纹理槽顺序批量绘制的，只有这样才能暴露「draw order 丢失」。
    int checker = MakeTexture(gl, TexChecker(), kTex);
    int circle = MakeTexture(gl, TexCircle(), kTex);

    gl.Enable(GL_BLEND);
    gl.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // 先画半透明圆盘
    DrawQuad(gl, circle, w * 0.5f - 90, h * 0.5f - 90, w * 0.5f + 90, h * 0.5f + 90);
    // 再画不透明棋盘压在右半边 → 后画的必须盖住先画的
    DrawQuad(gl, checker, w * 0.5f + 10, h * 0.5f - 60, w * 0.5f + 170, h * 0.5f + 60);

    gl.Disable(GL_BLEND);
    return true;
}

bool Scene07Scissor(IGl& gl, int w, int h, std::string& note) {
    if (!gl.Has(CAP_SCISSOR)) {
        note = gl.Why(CAP_SCISSOR);
        return false;
    }
    int tex = MakeTexture(gl, TexChecker(), kTex);
    gl.Enable(GL_SCISSOR_TEST);
    gl.Scissor(0, 0, w / 2, h);  // GL 约定：左下原点，裁到左半边
    DrawQuad(gl, tex, 0, 0, (float)w, (float)h);
    gl.Disable(GL_SCISSOR_TEST);
    return true;
}

// 补充诊断：glClear 是否作用于「当前绑定的离屏目标」。
// 正确实现 = 画面变绿；若仍然是我们先前画的红 → clear 把渲染目标切走了。
bool Scene08ClearRouting(IGl& gl, int w, int h, std::string& note) {
    if (!gl.Has(CAP_CLEAR_TO_TARGET)) {
        note = gl.Why(CAP_CLEAR_TO_TARGET);
        return false;
    }
    int red = MakeTexture(gl, TexSolid(255, 0, 0), 8);
    DrawQuad(gl, red, 0, 0, (float)w, (float)h);
    gl.ClearColor(0.0f, 1.0f, 0.0f, 1.0f);
    gl.Clear(GL_COLOR_BUFFER_BIT);
    return true;
}

const Scene kScenes[] = {
    {"01_clear", "纯色清屏", "帧起点 clear / 颜色精度", "检查 render pass loadOp 与 clear 颜色",
     {0.2f, 0.4f, 0.6f, 1.0f}, Scene01Clear},
    {"02_one_texture", "单纹理绘制", "纹理上传 + 四边形 + NEAREST 采样", "检查纹理上传格式、上下翻转、采样过滤",
     {0.1f, 0.1f, 0.1f, 1.0f}, Scene02OneTexture},
    {"03_multi_texture", "多纹理批绘制", "3 张纹理 × 4 个四边形，共 12 个 quad", "检查 descriptor set / 纹理槽位串号",
     {0.05f, 0.05f, 0.08f, 1.0f}, Scene03MultiTexture},
    {"04_tint_alpha", "带 tint/alpha 的 sprite", "逐 draw 颜色（uniform）+ alpha 混合", "uniform 上传未实现时直接判 UNSUPPORTED",
     {0.0f, 0.0f, 0.0f, 1.0f}, Scene04TintAlpha},
    {"05_offscreen_rt", "离屏 RT 再用作纹理", "FBO 创建/绑定/纹理附件 + RT 当纹理采样", "检查 FBO 入口是否为空实现",
     {0.15f, 0.0f, 0.15f, 1.0f}, Scene05Offscreen},
    {"06_blend_overlay", "blend 叠加", "alpha 混合 + 绘制顺序（后画的必须盖住先画的）", "顺序错乱 = 按纹理批处理丢失 draw order",
     {0.0f, 0.1f, 0.2f, 1.0f}, Scene06BlendOverlay},
    {"07_scissor", "scissor 裁剪", "裁剪矩形，只画左半边", "状态被记录但没下发 = 整屏都被画出来",
     {0.3f, 0.3f, 0.0f, 1.0f}, Scene07Scissor},
    {"08_clear_routing", "glClear 目标路由（诊断）", "glClear 是否作用于已绑定的离屏目标", "画面仍是红色 = clear 把目标切回了后备缓冲",
     {1.0f, 1.0f, 1.0f, 1.0f}, Scene08ClearRouting},
};

}  // namespace

int SceneCount() { return (int)(sizeof(kScenes) / sizeof(kScenes[0])); }

const Scene& SceneAt(int index) { return kScenes[index]; }

const Scene* FindScene(const std::string& name) {
    for (int i = 0; i < SceneCount(); ++i) {
        if (name == kScenes[i].name) return &kScenes[i];
    }
    return nullptr;
}

}  // namespace gvd
