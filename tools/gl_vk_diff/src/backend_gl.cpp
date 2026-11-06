// GL/ANGLE 参考后端：用真实的 GLES3（ANGLE）渲染同一批场景，作为「正确结果」基准。
// 不依赖任何 GL 头文件，全部入口从 libEGL/libGLESv2 动态取，避免给工装引入 SDK 依赖。
#include "backends.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace gvd {
namespace {

typedef void* EGLDisplay;
typedef void* EGLConfig;
typedef void* EGLSurface;
typedef void* EGLContext;
typedef int EGLint;
typedef unsigned int EGLBoolean;

typedef unsigned int GLuint;
typedef int GLint;
typedef int GLenum;
typedef int GLsizei;
typedef float GLfloat;
typedef unsigned char GLboolean;
typedef char GLchar;
typedef unsigned int GLbitfield;
typedef long long GLsizeiptr;

#define GLAPIENTRY __stdcall
#define EGLAPIENTRY __stdcall

typedef EGLDisplay(EGLAPIENTRY* PFN_eglGetDisplay)(void*);
typedef EGLBoolean(EGLAPIENTRY* PFN_eglInitialize)(EGLDisplay, EGLint*, EGLint*);
typedef EGLBoolean(EGLAPIENTRY* PFN_eglChooseConfig)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*);
typedef EGLSurface(EGLAPIENTRY* PFN_eglCreatePbufferSurface)(EGLDisplay, EGLConfig, const EGLint*);
typedef EGLContext(EGLAPIENTRY* PFN_eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint*);
typedef EGLBoolean(EGLAPIENTRY* PFN_eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
typedef EGLint(EGLAPIENTRY* PFN_eglGetError)();
typedef void*(EGLAPIENTRY* PFN_eglGetProcAddress)(const char*);
typedef EGLBoolean(EGLAPIENTRY* PFN_eglTerminate)(EGLDisplay);

typedef void(GLAPIENTRY* PFN_glGenTextures)(GLsizei, GLuint*);
typedef void(GLAPIENTRY* PFN_glBindTexture)(GLenum, GLuint);
typedef void(GLAPIENTRY* PFN_glTexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
typedef void(GLAPIENTRY* PFN_glTexParameteri)(GLenum, GLenum, GLint);
typedef void(GLAPIENTRY* PFN_glGenBuffers)(GLsizei, GLuint*);
typedef void(GLAPIENTRY* PFN_glBindBuffer)(GLenum, GLuint);
typedef void(GLAPIENTRY* PFN_glBufferData)(GLenum, GLsizeiptr, const void*, GLenum);
typedef void(GLAPIENTRY* PFN_glGenVertexArrays)(GLsizei, GLuint*);
typedef void(GLAPIENTRY* PFN_glBindVertexArray)(GLuint);
typedef void(GLAPIENTRY* PFN_glGenFramebuffers)(GLsizei, GLuint*);
typedef void(GLAPIENTRY* PFN_glBindFramebuffer)(GLenum, GLuint);
typedef void(GLAPIENTRY* PFN_glFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum(GLAPIENTRY* PFN_glCheckFramebufferStatus)(GLenum);
typedef void(GLAPIENTRY* PFN_glViewport)(GLint, GLint, GLsizei, GLsizei);
typedef void(GLAPIENTRY* PFN_glClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
typedef void(GLAPIENTRY* PFN_glClear)(GLbitfield);
typedef void(GLAPIENTRY* PFN_glEnable)(GLenum);
typedef void(GLAPIENTRY* PFN_glDisable)(GLenum);
typedef void(GLAPIENTRY* PFN_glBlendFunc)(GLenum, GLenum);
typedef void(GLAPIENTRY* PFN_glColorMask)(GLboolean, GLboolean, GLboolean, GLboolean);
typedef void(GLAPIENTRY* PFN_glScissor)(GLint, GLint, GLsizei, GLsizei);
typedef void(GLAPIENTRY* PFN_glDrawArrays)(GLenum, GLint, GLsizei);
typedef void(GLAPIENTRY* PFN_glVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
typedef void(GLAPIENTRY* PFN_glEnableVertexAttribArray)(GLuint);
typedef void(GLAPIENTRY* PFN_glReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);
typedef void(GLAPIENTRY* PFN_glFinish)();
typedef GLenum(GLAPIENTRY* PFN_glGetError)();
typedef GLuint(GLAPIENTRY* PFN_glCreateShader)(GLenum);
typedef void(GLAPIENTRY* PFN_glShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*);
typedef void(GLAPIENTRY* PFN_glCompileShader)(GLuint);
typedef void(GLAPIENTRY* PFN_glGetShaderiv)(GLuint, GLenum, GLint*);
typedef void(GLAPIENTRY* PFN_glGetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
typedef GLuint(GLAPIENTRY* PFN_glCreateProgram)();
typedef void(GLAPIENTRY* PFN_glAttachShader)(GLuint, GLuint);
typedef void(GLAPIENTRY* PFN_glLinkProgram)(GLuint);
typedef void(GLAPIENTRY* PFN_glGetProgramiv)(GLuint, GLenum, GLint*);
typedef void(GLAPIENTRY* PFN_glGetProgramInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
typedef void(GLAPIENTRY* PFN_glUseProgram)(GLuint);
typedef GLint(GLAPIENTRY* PFN_glGetUniformLocation)(GLuint, const GLchar*);
typedef void(GLAPIENTRY* PFN_glUniform4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
typedef void(GLAPIENTRY* PFN_glUniform1i)(GLint, GLint);

// EGL 常量
enum {
    EGL_NONE_ = 0x3038,
    EGL_SURFACE_TYPE = 0x3033,
    EGL_PBUFFER_BIT = 0x0001,
    EGL_RENDERABLE_TYPE = 0x3040,
    EGL_OPENGL_ES2_BIT = 0x0004,
    EGL_OPENGL_ES3_BIT = 0x0040,
    EGL_ALPHA_SIZE = 0x3021,
    EGL_BLUE_SIZE = 0x3022,
    EGL_GREEN_SIZE = 0x3023,
    EGL_RED_SIZE = 0x3024,
    EGL_WIDTH = 0x3057,
    EGL_HEIGHT = 0x3056,
    EGL_CONTEXT_CLIENT_VERSION = 0x3098
};

const char* kVertSrc =
    "#version 300 es\n"
    "layout(location = 0) in vec2 aPos;\n"
    "layout(location = 1) in vec2 aUV;\n"
    "out vec2 vUV;\n"
    "void main() { gl_Position = vec4(aPos, 0.0, 1.0); vUV = aUV; }\n";

// 与 native_vulkan/shader.frag 等价的采样着色器；额外多一个 uTint，
// 用来表达 flixel 里「逐 draw 颜色」这条语义（Vulkan 侧目前没有对应能力）。
const char* kFragSrc =
    "#version 300 es\n"
    "precision highp float;\n"
    "in vec2 vUV;\n"
    "uniform sampler2D uTex;\n"
    "uniform vec4 uTint;\n"
    "out vec4 oColor;\n"
    "void main() { oColor = texture(uTex, vUV) * uTint; }\n";

class GlBackend : public IGl {
public:
    explicit GlBackend(std::string& problem) { problem_ = problem; }

    ~GlBackend() override {
        if (display_ && eglTerminate_) eglTerminate_(display_);
    }

    const char* Name() const override { return "gl-angle"; }
    bool Has(Caps) const override { return true; }
    const char* Why(Caps) const override { return ""; }
    const char* LastProblem() const override { return problem_.c_str(); }
    Counters& Perf() override { return counters_; }

    bool Init(const char* angleDir) {
        if (angleDir && angleDir[0]) SetDllDirectoryA(angleDir);
        HMODULE egl = LoadLibraryA("libEGL.dll");
        if (!egl) {
            problem_ = "加载 libEGL.dll 失败（用 --angle-dir 指向 ANGLE bin 目录，或把 DLL 放到 exe 同目录）";
            return false;
        }
        HMODULE gles = LoadLibraryA("libGLESv2.dll");
        if (!gles) {
            problem_ = "加载 libGLESv2.dll 失败（检查 --angle-dir）";
            return false;
        }

#define ELOAD(name)                                                     \
    egl##name##_ = (PFN_egl##name)GetProcAddress(egl, "egl" #name);     \
    if (!egl##name##_) { problem_ = "缺少 EGL 入口: egl" #name; return false; }
        ELOAD(GetDisplay)
        ELOAD(Initialize)
        ELOAD(ChooseConfig)
        ELOAD(CreatePbufferSurface)
        ELOAD(CreateContext)
        ELOAD(MakeCurrent)
        ELOAD(GetError)
        ELOAD(GetProcAddress)
        ELOAD(Terminate)
#undef ELOAD

        display_ = eglGetDisplay_(nullptr);
        EGLint major = 0, minor = 0;
        if (!eglInitialize_(display_, &major, &minor)) {
            problem_ = "eglInitialize 失败，EGL error=" + std::to_string((int)eglGetError_());
            return false;
        }

        // ANGLE 的不同构建对 config 要求不完全一致，按由严到宽依次尝试。
        static const EGLint kTry1[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                                       EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                                       EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                                       EGL_ALPHA_SIZE, 8, EGL_NONE_};
        static const EGLint kTry2[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                                       EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                                       EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                                       EGL_NONE_};
        static const EGLint kTry3[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                                       EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                                       EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                                       EGL_ALPHA_SIZE, 8, EGL_NONE_};
        static const EGLint kTry4[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                                       EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                                       EGL_NONE_};

        EGLConfig cfg = nullptr;
        int chosen = -1;
        const EGLint* tries[4] = {kTry1, kTry2, kTry3, kTry4};
        for (int i = 0; i < 4 && chosen < 0; ++i) {
            EGLint nCfg = 0;
            if (eglChooseConfig_(display_, tries[i], &cfg, 1, &nCfg) && nCfg > 0 && cfg) {
                chosen = i + 1;
            }
        }
        if (chosen < 0) {
            problem_ = "eglChooseConfig 找不到可用配置（ES3/ES2 + pbuffer），EGL error=" +
                       std::to_string((int)eglGetError_());
            return false;
        }

        EGLint pbAttr[] = {EGL_WIDTH, 8, EGL_HEIGHT, 8, EGL_NONE_};
        surface_ = eglCreatePbufferSurface_(display_, cfg, pbAttr);
        if (!surface_) {
            problem_ = "eglCreatePbufferSurface 失败，EGL error=" + std::to_string((int)eglGetError_());
            return false;
        }

        EGLint ctxAttr[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE_};
        context_ = eglCreateContext_(display_, cfg, nullptr, ctxAttr);
        if (!context_) {
            problem_ = "eglCreateContext(ES3) 失败，EGL error=" + std::to_string((int)eglGetError_());
            return false;
        }
        if (!eglMakeCurrent_(display_, surface_, surface_, context_)) {
            problem_ = "eglMakeCurrent 失败，EGL error=" + std::to_string((int)eglGetError_());
            return false;
        }

#define GLOAD(name)                                                              \
    p_gl##name = (PFN_gl##name)eglGetProcAddress_("gl" #name);                   \
    if (!p_gl##name) { problem_ = "缺少 GL 入口: gl" #name; return false; }
        GLOAD(GenTextures)
        GLOAD(BindTexture)
        GLOAD(TexImage2D)
        GLOAD(TexParameteri)
        GLOAD(GenBuffers)
        GLOAD(BindBuffer)
        GLOAD(BufferData)
        GLOAD(GenVertexArrays)
        GLOAD(BindVertexArray)
        GLOAD(GenFramebuffers)
        GLOAD(BindFramebuffer)
        GLOAD(FramebufferTexture2D)
        GLOAD(CheckFramebufferStatus)
        GLOAD(Viewport)
        GLOAD(ClearColor)
        GLOAD(Clear)
        GLOAD(Enable)
        GLOAD(Disable)
        GLOAD(BlendFunc)
        GLOAD(ColorMask)
        GLOAD(Scissor)
        GLOAD(DrawArrays)
        GLOAD(VertexAttribPointer)
        GLOAD(EnableVertexAttribArray)
        GLOAD(ReadPixels)
        GLOAD(Finish)
        GLOAD(GetError)
        GLOAD(CreateShader)
        GLOAD(ShaderSource)
        GLOAD(CompileShader)
        GLOAD(GetShaderiv)
        GLOAD(GetShaderInfoLog)
        GLOAD(CreateProgram)
        GLOAD(AttachShader)
        GLOAD(LinkProgram)
        GLOAD(GetProgramiv)
        GLOAD(GetProgramInfoLog)
        GLOAD(UseProgram)
        GLOAD(GetUniformLocation)
        GLOAD(Uniform4f)
        GLOAD(Uniform1i)
#undef GLOAD

        if (!BuildProgram()) return false;

        p_glGenVertexArrays(1, &vao_);
        p_glBindVertexArray(vao_);
        p_glGenBuffers(1, &dynVbo_);
        p_glGenFramebuffers(1, &fbo_);

        p_glDisable(GL_DEPTH_TEST);
        p_glDisable(GL_CULL_FACE);
        ready_ = true;
        return true;
    }

    // ---- 纹理 ----
    int CreateTexture() override {
        GLuint t = 0;
        p_glGenTextures(1, &t);
        return (int)t;
    }

    void BindTexture(int tex) override {
        boundTex_ = tex;
        p_glBindTexture(GL_TEXTURE_2D, (GLuint)tex);
    }

    void TexImage2D(int level, int w, int h, const unsigned char* rgba) override {
        p_glBindTexture(GL_TEXTURE_2D, (GLuint)boundTex_);
        p_glTexImage2D(GL_TEXTURE_2D, level, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        if (rgba) {
            counters_.texUploads++;
            counters_.texBytes += (uint64_t)w * h * 4;
        }
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    // ---- 缓冲 / 绘制 ----
    int CreateBuffer() override {
        GLuint b = 0;
        p_glGenBuffers(1, &b);
        bufCpu_[(int)b] = std::vector<Vertex>();
        return (int)b;
    }

    void BindBuffer(int buf) override { boundBuf_ = buf; }

    void BufferData(const void* data, size_t bytes) override {
        auto& v = bufCpu_[boundBuf_];
        v.resize(bytes / sizeof(Vertex));
        std::memcpy(v.data(), data, bytes);
    }

    void VertexAttribPointer(int index, int size, size_t stride, size_t offset) override {
        if (index < 0 || index > 1) return;
        attrSize_[index] = size;
        attrStride_[index] = (int)stride;
        attrOffset_[index] = (int)offset;
    }

    void EnableVertexAttribArray(int) override {}

    void DrawArrays(int first, int count) override {
        auto it = bufCpu_.find(boundBuf_);
        if (it == bufCpu_.end()) return;
        const std::vector<Vertex>& src = it->second;

        std::vector<Vertex> ndc;
        ndc.reserve((size_t)count);
        for (int i = 0; i < count; ++i) {
            int idx = first + i;
            if (idx < 0 || idx >= (int)src.size()) break;
            Vertex v = src[idx];
            // 场景给的是「像素坐标、原点左上」，这里换成 GL 的 NDC（原点居中、y 向上）
            v.x = v.x * 2.0f / (float)fw_ - 1.0f;
            v.y = 1.0f - v.y * 2.0f / (float)fh_;
            ndc.push_back(v);
        }
        if (ndc.empty()) return;

        p_glBindVertexArray(vao_);
        p_glBindBuffer(GL_ARRAY_BUFFER, dynVbo_);
        p_glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(ndc.size() * sizeof(Vertex)), ndc.data(), GL_DYNAMIC_DRAW);
        for (int a = 0; a < 2; ++a) {
            p_glVertexAttribPointer(a, attrSize_[a], GL_FLOAT, 0, attrStride_[a], (const void*)(size_t)attrOffset_[a]);
        }
        p_glEnableVertexAttribArray(0);
        p_glEnableVertexAttribArray(1);
        p_glDrawArrays(GL_TRIANGLES, 0, (GLsizei)ndc.size());
        counters_.draws++;
        counters_.quads += ndc.size();
    }

    // ---- 帧 ----
    bool BeginFrame(int w, int h, const float clear[4]) override {
        if (!ready_) return false;
        if (capW_ != w || capH_ != h || capTex_ == 0) {
            if (capTex_ == 0) p_glGenTextures(1, &capTex_);
            p_glBindTexture(GL_TEXTURE_2D, capTex_);
            p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            capW_ = w;
            capH_ = h;
        }
        p_glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
        p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, capTex_, 0);
        if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != 0x8CD5) {  // GL_FRAMEBUFFER_COMPLETE
            problem_ = "捕获用 FBO 不完整";
            return false;
        }
        fw_ = w;
        fh_ = h;
        curFb_ = 0;
        p_glViewport(0, 0, w, h);
        p_glDisable(GL_SCISSOR_TEST);
        p_glDisable(GL_BLEND);
        p_glColorMask(1, 1, 1, 1);
        p_glClearColor(clear[0], clear[1], clear[2], clear[3]);
        p_glClear(GL_COLOR_BUFFER_BIT);
        p_glUseProgram(prog_);
        p_glUniform1i(uTex_, 0);
        p_glUniform4f(uTint_, 1.0f, 1.0f, 1.0f, 1.0f);
        return true;
    }

    bool EndFrame() override {
        if (!ready_) return false;
        p_glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
        p_glFinish();
        if (capture_) {
            img_.Alloc(fw_, fh_);
            p_glReadPixels(0, 0, fw_, fh_, GL_RGBA, GL_UNSIGNED_BYTE, img_.px.data());
            GLenum err = p_glGetError();
            if (err != 0) {
                problem_ = "GL error=0x" + std::to_string((int)err);
                return false;
            }
        }
        counters_.presents++;
        return true;
    }

    void SetCaptureEnabled(bool on) override { capture_ = on; }

    const Image& Captured() const override { return img_; }

    // ---- 逻辑 FBO ----
    int CreateFramebuffer() override {
        GLuint f = 0;
        p_glGenFramebuffers(1, &f);
        fboUser_[f] = true;
        return (int)f;
    }

    void BindFramebuffer(int fb) override {
        if (fb == 0) {
            curFb_ = 0;
            p_glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
            return;
        }
        curFb_ = fb;
        p_glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fb);
    }

    void FramebufferTexture2D(int tex) override {
        p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, (GLuint)tex, 0);
        if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != 0x8CD5) {
            problem_ = "场景自建 FBO 不完整（纹理可能没有分配存储）";
        }
    }

    // ---- 状态 ----
    void Enable(int cap) override { p_glEnable(cap); }
    void Disable(int cap) override { p_glDisable(cap); }
    void BlendFunc(int src, int dst) override { p_glBlendFunc(src, dst); }
    void ColorMask(bool r, bool g, bool b, bool a) override {
        p_glColorMask(r ? 1 : 0, g ? 1 : 0, b ? 1 : 0, a ? 1 : 0);
    }
    void Scissor(int x, int y, int w, int h) override { p_glScissor(x, y, w, h); }
    void Viewport(int x, int y, int w, int h) override { p_glViewport(x, y, w, h); }
    void ClearColor(float r, float g, float b, float a) override { p_glClearColor(r, g, b, a); }
    void Clear(int mask) override { p_glClear(mask); }
    void SetTint(float r, float g, float b, float a) override { p_glUniform4f(uTint_, r, g, b, a); }

private:
    bool BuildProgram() {
        GLuint vs = p_glCreateShader(0x8B31);  // GL_VERTEX_SHADER
        p_glShaderSource(vs, 1, &kVertSrc, nullptr);
        p_glCompileShader(vs);
        GLint ok = 0;
        p_glGetShaderiv(vs, 0x8B81, &ok);  // GL_COMPILE_STATUS
        if (!ok) {
            char log[512] = {0};
            p_glGetShaderInfoLog(vs, 511, nullptr, log);
            problem_ = std::string("顶点着色器编译失败: ") + log;
            return false;
        }
        GLuint fs = p_glCreateShader(0x8B30);  // GL_FRAGMENT_SHADER
        p_glShaderSource(fs, 1, &kFragSrc, nullptr);
        p_glCompileShader(fs);
        p_glGetShaderiv(fs, 0x8B81, &ok);
        if (!ok) {
            char log[512] = {0};
            p_glGetShaderInfoLog(fs, 511, nullptr, log);
            problem_ = std::string("片元着色器编译失败: ") + log;
            return false;
        }
        prog_ = p_glCreateProgram();
        p_glAttachShader(prog_, vs);
        p_glAttachShader(prog_, fs);
        p_glLinkProgram(prog_);
        p_glGetProgramiv(prog_, 0x8B82, &ok);  // GL_LINK_STATUS
        if (!ok) {
            char log[512] = {0};
            p_glGetProgramInfoLog(prog_, 511, nullptr, log);
            problem_ = std::string("着色器链接失败: ") + log;
            return false;
        }
        uTex_ = p_glGetUniformLocation(prog_, "uTex");
        uTint_ = p_glGetUniformLocation(prog_, "uTint");
        p_glUseProgram(prog_);
        return true;
    }

    // EGL 入口
    PFN_eglGetDisplay eglGetDisplay_ = nullptr;
    PFN_eglInitialize eglInitialize_ = nullptr;
    PFN_eglChooseConfig eglChooseConfig_ = nullptr;
    PFN_eglCreatePbufferSurface eglCreatePbufferSurface_ = nullptr;
    PFN_eglCreateContext eglCreateContext_ = nullptr;
    PFN_eglMakeCurrent eglMakeCurrent_ = nullptr;
    PFN_eglGetError eglGetError_ = nullptr;
    PFN_eglGetProcAddress eglGetProcAddress_ = nullptr;
    PFN_eglTerminate eglTerminate_ = nullptr;

    // GL 入口
    PFN_glGenTextures p_glGenTextures = nullptr;
    PFN_glBindTexture p_glBindTexture = nullptr;
    PFN_glTexImage2D p_glTexImage2D = nullptr;
    PFN_glTexParameteri p_glTexParameteri = nullptr;
    PFN_glGenBuffers p_glGenBuffers = nullptr;
    PFN_glBindBuffer p_glBindBuffer = nullptr;
    PFN_glBufferData p_glBufferData = nullptr;
    PFN_glGenVertexArrays p_glGenVertexArrays = nullptr;
    PFN_glBindVertexArray p_glBindVertexArray = nullptr;
    PFN_glGenFramebuffers p_glGenFramebuffers = nullptr;
    PFN_glBindFramebuffer p_glBindFramebuffer = nullptr;
    PFN_glFramebufferTexture2D p_glFramebufferTexture2D = nullptr;
    PFN_glCheckFramebufferStatus p_glCheckFramebufferStatus = nullptr;
    PFN_glViewport p_glViewport = nullptr;
    PFN_glClearColor p_glClearColor = nullptr;
    PFN_glClear p_glClear = nullptr;
    PFN_glEnable p_glEnable = nullptr;
    PFN_glDisable p_glDisable = nullptr;
    PFN_glBlendFunc p_glBlendFunc = nullptr;
    PFN_glColorMask p_glColorMask = nullptr;
    PFN_glScissor p_glScissor = nullptr;
    PFN_glDrawArrays p_glDrawArrays = nullptr;
    PFN_glReadPixels p_glReadPixels = nullptr;
    PFN_glFinish p_glFinish = nullptr;
    PFN_glGetError p_glGetError = nullptr;
    PFN_glCreateShader p_glCreateShader = nullptr;
    PFN_glShaderSource p_glShaderSource = nullptr;
    PFN_glCompileShader p_glCompileShader = nullptr;
    PFN_glGetShaderiv p_glGetShaderiv = nullptr;
    PFN_glGetShaderInfoLog p_glGetShaderInfoLog = nullptr;
    PFN_glCreateProgram p_glCreateProgram = nullptr;
    PFN_glAttachShader p_glAttachShader = nullptr;
    PFN_glLinkProgram p_glLinkProgram = nullptr;
    PFN_glGetProgramiv p_glGetProgramiv = nullptr;
    PFN_glGetProgramInfoLog p_glGetProgramInfoLog = nullptr;
    PFN_glUseProgram p_glUseProgram = nullptr;
    PFN_glGetUniformLocation p_glGetUniformLocation = nullptr;
    PFN_glUniform4f p_glUniform4f = nullptr;
    PFN_glUniform1i p_glUniform1i = nullptr;
    PFN_glVertexAttribPointer p_glVertexAttribPointer = nullptr;
    PFN_glEnableVertexAttribArray p_glEnableVertexAttribArray = nullptr;

    EGLDisplay display_ = nullptr;
    EGLSurface surface_ = nullptr;
    EGLContext context_ = nullptr;
    bool ready_ = false;

    GLuint prog_ = 0, vao_ = 0, dynVbo_ = 0, fbo_ = 0, capTex_ = 0;
    GLint uTex_ = -1, uTint_ = -1;
    int capW_ = 0, capH_ = 0;
    int fw_ = 0, fh_ = 0;
    int boundTex_ = 0, boundBuf_ = 0, curFb_ = 0;

    int attrSize_[2] = {2, 2};
    int attrStride_[2] = {16, 16};
    int attrOffset_[2] = {0, 8};

    std::map<int, std::vector<Vertex>> bufCpu_;
    std::map<int, bool> fboUser_;
    Image img_;
    std::string problem_;
    Counters counters_;
    bool capture_ = true;
};

}  // namespace

IGl* CreateGlBackend(const char* angleDir, std::string& problem) {
    GlBackend* b = new GlBackend(problem);
    if (!b->Init(angleDir)) {
        problem = b->LastProblem();
        delete b;
        return nullptr;
    }
    return b;
}

}  // namespace gvd
