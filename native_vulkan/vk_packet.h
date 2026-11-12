#pragma once

// WirePacket / DrawRecord 的纯格式层。
//
// 设计约束（见 docs/draw_packet_spec.md v1.1）：
//   - 本文件不得 include 任何平台头（windows.h / Vulkan / hxcpp），只依赖 stdint。
//     这样能对单个 TU 做 -fsyntax-only / -c（只编不链）的本地检查。
//   - 这里只做"字节布局 + 纯函数"：解析、物化、canonical/sum、arena。
//     所有对可变 GL 状态的读取由 gl_packet.cpp 完成，物化后解码器只读 DrawRecord。
//   - 字节序小端；WirePacket 64B，DrawRecord 640B；reserved 必须为 0。

#include <stdint.h>
#include <stddef.h>

namespace lime {
namespace vkpkt {

constexpr uint32_t kWhlVersion = 1;
constexpr uint32_t kWireBytes = 64;
constexpr uint32_t kRecordBytes = 640;
constexpr uint32_t kMaxAttrSlots = 8;
constexpr uint32_t kMaxTexSlots = 8;
constexpr uint32_t kMaxDrawUniform = 4096;
constexpr uint32_t kArenaVertexInit = 64u * 1024u * 1024u;
constexpr uint32_t kArenaIndexInit = 8u * 1024u * 1024u;
constexpr uint32_t kArenaUniformInit = 8u * 1024u * 1024u;

enum Kind {
    KIND_DRAW_ELEMENTS = 0,
    KIND_DRAW_ARRAYS = 1,
    KIND_CLEAR = 2,
};

enum Flags {
    FLAG_IMPLICIT_CLEAR = 1u << 0,
    FLAG_HAS_PROGRAM = 1u << 1,
    FLAG_STAGE_OWNS_CONTEXT = 1u << 2,
};

enum AttrKind {
    ATTR_DISABLED = 0,
    ATTR_ARRAY = 1,
    ATTR_CONSTANT = 2,
    ATTR_CLIENT = 3,
};

// 压缩类型码（spec §5.2）
enum AttrTypeCode {
    ATYPE_FLOAT = 0,
    ATYPE_UBYTE = 1,
    ATYPE_BYTE = 2,
    ATYPE_USHORT = 3,
    ATYPE_SHORT = 4,
    ATYPE_UNKNOWN = 255,
};

// GL 顶点属性类型枚举 -> 压缩码。glType 必须是完整的 GL 枚举（0x1406 这类 16 位值），
// 任何 & 0xFF 的截断都会把 GL_FLOAT 变成 0x06 而恒落 ATYPE_UNKNOWN（R1 的根因）。
constexpr uint8_t AttrTypeFromGL (uint32_t glType) {
    switch (glType) {
        case 0x1406: return ATYPE_FLOAT;   // GL_FLOAT
        case 0x1401: return ATYPE_UBYTE;   // GL_UNSIGNED_BYTE
        case 0x1400: return ATYPE_BYTE;    // GL_BYTE
        case 0x1403: return ATYPE_USHORT;  // GL_UNSIGNED_SHORT
        case 0x1402: return ATYPE_SHORT;   // GL_SHORT
        default: return ATYPE_UNKNOWN;
    }
}

// R1 回归护栏：编译期确认 0x1406 命中 ATYPE_FLOAT
static_assert(AttrTypeFromGL (0x1406) == ATYPE_FLOAT, "GL_FLOAT(0x1406) must map to ATYPE_FLOAT");

// clearMask 位（spec §3）
enum ClearMaskBits {
    CLEAR_COLOR = 1u << 0,
    CLEAR_DEPTH = 1u << 1,
    CLEAR_STENCIL = 1u << 2,
};

// 纹理槽里"未使用"的 unit 标记。spec 的 640B 布局没有 samplerCount 字段，
// 解码侧用 unit=0xFF 表示该槽不参与 slots= 输出；有效槽按填充顺序（= shader 声明顺序）排列。
constexpr uint32_t kTexSlotUnused = 0xFFu;

struct alignas(8) WirePacket {

    uint32_t seq;           // 0
    uint16_t kind;          // 4
    uint16_t flags;         // 6
    uint32_t frameGen;      // 8
    uint32_t program;       // 12
    uint32_t fbo;           // 16
    uint32_t ibo;           // 20
    uint32_t vbo;           // 24
    int32_t first;          // 28
    int32_t count;          // 32
    uint32_t clearColor;    // 36  RGBA8, R 在低字节
    float clearDepth;       // 40
    uint8_t clearMask;      // 44
    uint8_t stateVersion;   // 45
    uint16_t reserved0;     // 46  必须 0
    uint32_t srcLine;       // 48
    uint32_t rtParams;      // 52
    uint8_t progFormat;     // 56
    uint8_t reserved1;      // 57
    uint16_t reserved2;     // 58
    uint32_t texName;       // 60  发射时刻的主采样纹理 GL 名（0 = 老 exe，未记录）

};

static_assert(sizeof(WirePacket) == kWireBytes, "WirePacket must be 64 bytes");
static_assert(offsetof(WirePacket, kind) == 4, "wire kind offset");
static_assert(offsetof(WirePacket, frameGen) == 8, "wire frameGen offset");
static_assert(offsetof(WirePacket, first) == 28, "wire first offset");
static_assert(offsetof(WirePacket, clearColor) == 36, "wire clearColor offset");
static_assert(offsetof(WirePacket, stateVersion) == 45, "wire stateVersion offset");
static_assert(offsetof(WirePacket, srcLine) == 48, "wire srcLine offset");
static_assert(offsetof(WirePacket, rtParams) == 52, "wire rtParams offset");
static_assert(offsetof(WirePacket, progFormat) == 56, "wire progFormat offset");

struct PktAttr {

    uint32_t bufferName;    // +0
    uint32_t byteOffset;    // +4
    uint16_t stride;        // +8
    uint8_t size;           // +10
    uint8_t type;           // +11  压缩码
    uint8_t kind;           // +12  AttrKind
    uint8_t normalized;     // +13
    uint16_t reserved;      // +14
    float constValue[4];    // +16

};

static_assert(sizeof(PktAttr) == 32, "PktAttr must be 32 bytes");
static_assert(offsetof(PktAttr, stride) == 8, "attr stride offset");
static_assert(offsetof(PktAttr, constValue) == 16, "attr constValue offset");

struct PktTex {

    uint32_t unit;          // +0  0..7；0xFF = 未使用
    uint32_t texName;       // +4  GL 纹理名（slots= 用它）
    uint32_t imageId;       // +8  g_texVk[texName]
    uint32_t samplerId;     // +12 当前烘焙进 image，恒 0（校验用）

};

static_assert(sizeof(PktTex) == 16, "PktTex must be 16 bytes");

// 单面模板状态 20B。func/mask/op 原样保存 gl_state 里的值（VK 枚举），与 spec 的
// "来源 gl_state.cpp FaceState" 一致。
struct PktFace {

    uint16_t func;          // +0
    uint16_t pad0;          // +2
    uint32_t valueMask;     // +4
    uint32_t writeMask;     // +8
    uint16_t sfail;         // +12
    uint16_t dpfail;        // +14
    uint16_t dppass;        // +16
    uint16_t reserved;      // +18

};

static_assert(sizeof(PktFace) == 20, "PktFace must be 20 bytes");
static_assert(offsetof(PktFace, valueMask) == 4, "face valueMask offset");

struct alignas(8) DrawRecord {

    uint32_t seq;               // 0
    uint32_t kind;              // 4
    uint32_t frameGen;          // 8
    uint32_t program;           // 12
    uint32_t fbo;               // 16
    uint32_t rtImage;           // 20  0 = 交换链当前 image
    uint16_t rtW;               // 24
    uint16_t rtH;               // 26
    uint32_t indexVkOffset;     // 28  arena 内 indexVkOffset = indexArenaOff + first*2
    int32_t first;              // 32
    int32_t count;              // 36
    uint32_t indexArenaOff;     // 40
    uint32_t indexArenaBytes;   // 44
    uint32_t vertexArenaOff;    // 48
    uint32_t vertexArenaBytes;  // 52
    uint16_t vertexStride;      // 56
    uint16_t attrCount;         // 58
    uint32_t uniformOffset;     // 60
    uint32_t uniformSize;       // 64
    uint32_t stateVersion;      // 68
    uint32_t flags;             // 72
    uint32_t srcLine;           // 76

    PktAttr attrs[kMaxAttrSlots];   // 80..335
    PktTex tex[kMaxTexSlots];       // 336..463

    uint8_t blendEnabled;       // 464
    uint8_t cullMode;           // 465  0=off 1=back 2=front 3=both
    uint8_t depthMask;          // 466
    uint8_t colorMaskBits;      // 467
    uint8_t depthTestEnabled;   // 468
    uint8_t stencilEnabled;     // 469
    uint8_t positionScaleY;     // 470  0=+1 1=-1
    uint8_t yFlip;              // 471

    uint16_t blendSrcRgb;       // 472
    uint16_t blendDstRgb;       // 474
    uint16_t blendSrcAlpha;     // 476
    uint16_t blendDstAlpha;     // 478
    uint16_t blendEqRgb;        // 480
    uint16_t blendEqAlpha;      // 482
    uint16_t depthFunc;         // 484
    uint16_t reserved0;         // 486

    int16_t scissorX;           // 488
    int16_t scissorY;           // 490
    int16_t scissorW;           // 492
    int16_t scissorH;           // 494
    int16_t vpX;                // 496
    int16_t vpY;                // 498
    int16_t vpW;                // 500
    int16_t vpH;                // 502

    uint8_t scissorEnabled;     // 504
    uint8_t frontFace;          // 505  0=CCW 1=CW
    uint16_t reserved1;         // 506
    uint32_t stencilRef;        // 508

    PktFace frontStencil;       // 512
    PktFace backStencil;        // 532

    float clearColorF[4];       // 552
    float clearDepth;           // 568
    int32_t clearStencil;       // 572
    uint32_t rtParams;          // 576
    uint8_t progFormat;         // 580
    uint8_t reserved2[3];       // 581
    uint8_t reserved3[56];      // 584..639

};

static_assert(sizeof(DrawRecord) == kRecordBytes, "DrawRecord must be 640 bytes");
static_assert(offsetof(DrawRecord, attrs) == 80, "record attrs offset");
static_assert(offsetof(DrawRecord, tex) == 336, "record tex offset");
static_assert(offsetof(DrawRecord, blendEnabled) == 464, "record state offset");
static_assert(offsetof(DrawRecord, blendSrcRgb) == 472, "record blend offset");
static_assert(offsetof(DrawRecord, scissorX) == 488, "record scissor offset");
static_assert(offsetof(DrawRecord, vpX) == 496, "record viewport offset");
static_assert(offsetof(DrawRecord, frontStencil) == 512, "record stencil offset");
static_assert(offsetof(DrawRecord, clearColorF) == 552, "record clear offset");
static_assert(offsetof(DrawRecord, rtParams) == 576, "record rtParams offset");
static_assert(offsetof(DrawRecord, progFormat) == 580, "record progFormat offset");

// ------------------------------------------------------------------ 物化输入
// 由 gl_packet.cpp 在 emit 调用内从 GL 状态镜像抓取；只在物化瞬间有效。

struct AttrInput {

    uint32_t bufferName = 0;
    uint32_t byteOffset = 0;
    uint16_t stride = 0;
    uint8_t size = 0;
    uint32_t glType = 0;        // GL 原始枚举（0x1406 等 16 位值），物化时用 AttrTypeFromGL 转压缩码
    uint8_t kind = ATTR_DISABLED;
    uint8_t normalized = 0;
    float constValue[4] = {0.0f, 0.0f, 0.0f, 0.0f};

};

static_assert(sizeof(AttrInput::glType) >= 2, "AttrInput.glType must hold 16-bit GL enums");

struct TexInput {

    uint32_t unit = kTexSlotUnused;
    uint32_t texName = 0;
    uint32_t imageId = 0;
    uint32_t samplerId = 0;

};

struct StateInput {

    uint8_t blendEnabled = 0;
    uint8_t cullMode = 1;
    uint8_t depthMask = 1;
    uint8_t colorMaskBits = 0x0F;
    uint8_t depthTestEnabled = 0;
    uint8_t stencilEnabled = 0;
    uint8_t positionScaleY = 0;
    uint8_t yFlip = 1;

    uint16_t blendSrcRgb = 0;
    uint16_t blendDstRgb = 0;
    uint16_t blendSrcAlpha = 0;
    uint16_t blendDstAlpha = 0;
    uint16_t blendEqRgb = 0;
    uint16_t blendEqAlpha = 0;
    uint16_t depthFunc = 0;

    int16_t scissorX = 0;
    int16_t scissorY = 0;
    int16_t scissorW = 0;
    int16_t scissorH = 0;
    int16_t vpX = 0;
    int16_t vpY = 0;
    int16_t vpW = 0;
    int16_t vpH = 0;

    uint8_t scissorEnabled = 0;
    uint8_t frontFace = 0;
    uint32_t stencilRef = 0;

    PktFace frontStencil;
    PktFace backStencil;

    float clearColor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float clearDepth = 1.0f;
    int32_t clearStencil = 0;

};

struct MaterializeInput {

    WirePacket wire;
    uint32_t rtImage = 0;
    uint16_t rtW = 0;
    uint16_t rtH = 0;
    uint32_t stateVersion = 0;

    uint16_t attrCount = 0;
    AttrInput attrs[kMaxAttrSlots];
    TexInput tex[kMaxTexSlots];
    StateInput state;

};

// ---------------------------------------------------------------- 纯函数 API

// 解析并校验 64B 包。失败时返回 false，err 写一行可检索原因（不含换行）。
bool ParseWire (const uint8_t* bytes, uint32_t nbytes, WirePacket* out,
                char* err, size_t errCap);

// 物化 640B DrawRecord。kind=CLEAR 时按 clearMask 写入"本次实际生效的 mask"，
// 并关闭 scissor（clear 是事件，不继承状态）。
void Materialize (const MaterializeInput& in, DrawRecord* out);

// blend= 取值（spec §10.3）。输出小写、无空格。
const char* BlendName (const StateInput& s, char* fallback, size_t cap);

// 把 record 的日志字段拼成规范串（不含 s=、sum=）：
//   f=..;prog=..;rt=..;prim=..;n=..;v0=..;idx=..;slots=..;blend=..;depth=..;dwrite=..;scissor=..;viewport=..;u=..
// uHex 必须是 8 位小写 hex。成功返回 true。
bool BuildCanonical (const DrawRecord& r, const char* uHex, char* out, size_t cap);

// 一行完整日志；extra 可为 NULL（[vkpkt] 用 NULL，[vkexec] 可传 "pending=1"）。
bool BuildLogLine (const char* prefix, const DrawRecord& r, const char* uHex,
                   const char* extra, char* out, size_t cap);

// 统一摘要实现：sha256(canonical)[:12]（小写 hex）。
bool BuildSum (const char* canonical, char out12[13]);

// sha256。out32 为 32 字节原始摘要。
void Sha256 (const void* data, size_t len, uint8_t out32[32]);

// uniform 摘要：sha256(完整 uniformData) 前 4 字节按大端拼成 32bit，
// 与 Haxe 侧 "%08x" 得到的 8 位 hex 对齐。
uint32_t UniformTag (const void* data, uint32_t len);

// 小工具：8 位小写 hex（不写前缀）。
void Hex8 (uint32_t v, char out[9]);

// --------------------------------------------------------------- 帧代 arena

struct Arena {

    uint8_t* base = nullptr;
    uint32_t cap = 0;
    uint32_t used = 0;
    bool oom = false;

};

bool ArenaInit (Arena* a, uint32_t initialBytes);
void ArenaFree (Arena* a);
void ArenaReset (Arena* a);

// 分配并返回字节偏移；容量不足按 2× 扩容（保留已有内容）。失败置 oom 并返回 false。
bool ArenaAlloc (Arena* a, uint32_t bytes, uint32_t align, uint32_t* outOff);
// 分配并复制。src 可为 NULL（只占位清零）。
bool ArenaAllocCopy (Arena* a, const void* src, uint32_t bytes, uint32_t align, uint32_t* outOff);

} // namespace vkpkt
} // namespace lime
