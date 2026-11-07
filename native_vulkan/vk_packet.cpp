#include "vk_packet.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

namespace lime {
namespace vkpkt {

// ---------------------------------------------------------------- 小工具

void Hex8 (uint32_t v, char out[9]) {
    static const char* h = "0123456789abcdef";
    for (int i = 0; i < 8; i++) {
        out[7 - i] = h[v & 0xF];
        v >>= 4;
    }
    out[8] = 0;
}

// ---------------------------------------------------------------- SHA-256

namespace {

static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

inline uint32_t Rotr (uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

struct Ctx {
    uint32_t h[8];
    uint64_t total;
    uint8_t buf[64];
    uint32_t bufLen;
};

void Compress (Ctx* c, const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = (uint32_t)p[i * 4] | ((uint32_t)p[i * 4 + 1] << 8) |
               ((uint32_t)p[i * 4 + 2] << 16) | ((uint32_t)p[i * 4 + 3] << 24);
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3];
    uint32_t e = c->h[4], f = c->h[5], g = c->h[6], hh = c->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = hh + S1 + ch + K[i] + w[i];
        uint32_t S0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
        uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + mj;
        hh = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += hh;
}

void CtxInit (Ctx* c) {
    c->h[0] = 0x6a09e667u; c->h[1] = 0xbb67ae85u;
    c->h[2] = 0x3c6ef372u; c->h[3] = 0xa54ff53au;
    c->h[4] = 0x510e527fu; c->h[5] = 0x9b05688cu;
    c->h[6] = 0x1f83d9abu; c->h[7] = 0x5be0cd19u;
    c->total = 0;
    c->bufLen = 0;
}

void CtxUpdate (Ctx* c, const uint8_t* data, size_t len) {
    c->total += len;
    if (c->bufLen) {
        uint32_t need = 64 - c->bufLen;
        uint32_t take = (uint32_t)((len < need) ? len : need);
        memcpy(c->buf + c->bufLen, data, take);
        c->bufLen += take;
        data += take;
        len -= take;
        if (c->bufLen == 64) {
            Compress(c, c->buf);
            c->bufLen = 0;
        }
    }
    while (len >= 64) {
        Compress(c, data);
        data += 64;
        len -= 64;
    }
    if (len) {
        memcpy(c->buf, data, len);
        c->bufLen = (uint32_t)len;
    }
}

void CtxFinal (Ctx* c, uint8_t out[32]) {
    uint64_t bits = c->total * 8u;
    uint8_t pad[72];
    memset(pad, 0, sizeof(pad));
    uint32_t padLen;
    if (c->bufLen < 56) {
        padLen = 64 - c->bufLen;
    } else {
        padLen = 128 - c->bufLen;
    }
    pad[0] = 0x80;
    for (int i = 0; i < 8; i++) {
        pad[padLen - 1 - i] = (uint8_t)(bits >> (i * 8));
    }
    CtxUpdate(c, pad, padLen);
    for (int i = 0; i < 8; i++) {
        out[i * 4] = (uint8_t)(c->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(c->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(c->h[i]);
    }
}

} // namespace

void Sha256 (const void* data, size_t len, uint8_t out32[32]) {
    Ctx c;
    CtxInit(&c);
    if (len && data) CtxUpdate(&c, (const uint8_t*)data, len);
    CtxFinal(&c, out32);
}

uint32_t UniformTag (const void* data, uint32_t len) {
    if (!data || !len) return 0;
    uint8_t d[32];
    Sha256(data, len, d);
    return ((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16) |
           ((uint32_t)d[2] << 8) | (uint32_t)d[3];
}

// ---------------------------------------------------------------- 解析 / 物化

bool ParseWire (const uint8_t* bytes, uint32_t nbytes, WirePacket* out,
                char* err, size_t errCap) {
    if (err && errCap) err[0] = 0;
    if (!bytes || !out) {
        if (err) snprintf(err, errCap, "null_ptr");
        return false;
    }
    if (nbytes != kWireBytes) {
        if (err) snprintf(err, errCap, "bad_bytes=%u", nbytes);
        return false;
    }
    WirePacket w;
    memcpy(&w, bytes, kWireBytes);
    if (w.reserved0 != 0 || w.reserved1 != 0 || w.reserved2 != 0 || w.reserved3 != 0) {
        if (err) snprintf(err, errCap, "reserved_nonzero");
        return false;
    }
    if (w.kind > KIND_CLEAR) {
        if (err) snprintf(err, errCap, "bad_kind=%u", (unsigned)w.kind);
        return false;
    }
    if (w.kind != KIND_CLEAR) {
        if (w.first < 0 || w.count < 0) {
            if (err) snprintf(err, errCap, "bad_range first=%d count=%d", (int)w.first, (int)w.count);
            return false;
        }
    }
    if (w.rtParams & ~0x3Fu) {
        // bit0-5 之外必须为 0；与 reserved 一样属于两端版本不一致。
        if (err) snprintf(err, errCap, "rtparams_hi=%08x", (unsigned)w.rtParams);
        return false;
    }
    *out = w;
    return true;
}

void Materialize (const MaterializeInput& in, DrawRecord* out) {
    if (!out) return;
    memset(out, 0, sizeof(DrawRecord));

    const WirePacket& w = in.wire;
    out->seq = w.seq;
    out->kind = w.kind;
    out->frameGen = w.frameGen;
    out->program = w.program;
    out->fbo = w.fbo;
    out->rtImage = in.rtImage;
    out->rtW = in.rtW;
    out->rtH = in.rtH;
    out->first = w.first;
    out->count = w.count;
    out->stateVersion = in.stateVersion;
    out->flags = (uint32_t)w.flags;
    out->srcLine = w.srcLine;
    out->rtParams = w.rtParams;
    out->progFormat = w.progFormat;

    out->attrCount = in.attrCount > kMaxAttrSlots ? kMaxAttrSlots : in.attrCount;
    for (uint32_t i = 0; i < kMaxAttrSlots; i++) {
        const AttrInput& a = in.attrs[i];
        PktAttr& d = out->attrs[i];
        d.bufferName = a.bufferName;
        d.byteOffset = a.byteOffset;
        d.stride = a.stride;
        d.size = a.size;
        d.type = AttrTypeFromGL(a.glType);
        d.kind = a.kind;
        d.normalized = a.normalized;
        d.reserved = 0;
        for (int k = 0; k < 4; k++) d.constValue[k] = a.constValue[k];
    }

    for (uint32_t i = 0; i < kMaxTexSlots; i++) {
        out->tex[i].unit = kTexSlotUnused;
        out->tex[i].texName = 0;
        out->tex[i].imageId = 0;
        out->tex[i].samplerId = 0;
        const TexInput& t = in.tex[i];
        if (t.unit <= 7) {
            out->tex[i].unit = t.unit;
            out->tex[i].texName = t.texName;
            out->tex[i].imageId = t.imageId;
            out->tex[i].samplerId = t.samplerId;
        }
    }

    const StateInput& s = in.state;
    out->blendEnabled = s.blendEnabled ? 1 : 0;
    out->cullMode = s.cullMode;
    out->depthMask = s.depthMask ? 1 : 0;
    out->colorMaskBits = (uint8_t)(s.colorMaskBits & 0x0F);
    out->depthTestEnabled = s.depthTestEnabled ? 1 : 0;
    out->stencilEnabled = s.stencilEnabled ? 1 : 0;
    out->positionScaleY = s.positionScaleY ? 1 : 0;
    out->yFlip = s.yFlip ? 1 : 0;
    out->blendSrcRgb = s.blendSrcRgb;
    out->blendDstRgb = s.blendDstRgb;
    out->blendSrcAlpha = s.blendSrcAlpha;
    out->blendDstAlpha = s.blendDstAlpha;
    out->blendEqRgb = s.blendEqRgb;
    out->blendEqAlpha = s.blendEqAlpha;
    out->depthFunc = s.depthFunc;
    out->scissorX = s.scissorX;
    out->scissorY = s.scissorY;
    out->scissorW = s.scissorW;
    out->scissorH = s.scissorH;
    out->vpX = s.vpX;
    out->vpY = s.vpY;
    out->vpW = s.vpW;
    out->vpH = s.vpH;
    out->scissorEnabled = s.scissorEnabled ? 1 : 0;
    out->frontFace = s.frontFace ? 1 : 0;
    out->stencilRef = s.stencilRef;
    out->frontStencil = s.frontStencil;
    out->backStencil = s.backStencil;
    for (int i = 0; i < 4; i++) out->clearColorF[i] = s.clearColor[i];
    out->clearDepth = s.clearDepth;
    out->clearStencil = s.clearStencil;

    if (w.kind == KIND_CLEAR) {
        // clear 是事件：本次使用的 mask 必须在 packet/记录里自带。
        // 解码后记录的是"这一笔 clear 实际生效的 mask"，不是调用后的残留状态。
        out->colorMaskBits = (uint8_t)(((w.clearMask & CLEAR_COLOR) && (w.clearMask != 0)) ? 0x0F : 0x00);
        out->depthMask = (w.clearMask & CLEAR_DEPTH) ? 1 : 0;
        uint32_t sw = (w.clearMask & CLEAR_STENCIL) ? 0xFFFFFFFFu : 0u;
        out->frontStencil.writeMask = sw;
        out->backStencil.writeMask = sw;
        // glClear 临时关闭 scissor，之后按 GL 语义不恢复；本事件按关闭记录。
        out->scissorEnabled = 0;
        out->scissorX = 0; out->scissorY = 0; out->scissorW = 0; out->scissorH = 0;
        // 颜色/深度来自 wire（这次 clear 的参数），不是 g_state 的事后值。
        const uint32_t c = w.clearColor;
        out->clearColorF[0] = (float)((c >> 0) & 0xFF) / 255.0f;
        out->clearColorF[1] = (float)((c >> 8) & 0xFF) / 255.0f;
        out->clearColorF[2] = (float)((c >> 16) & 0xFF) / 255.0f;
        out->clearColorF[3] = (float)((c >> 24) & 0xFF) / 255.0f;
        out->clearDepth = w.clearDepth;
    }
}

// ---------------------------------------------------------------- blend 名

const char* BlendName (const StateInput& s, char* fallback, size_t cap) {
    enum { GL_ZERO = 0, GL_ONE = 1, GL_SRC_ALPHA = 0x0302, GL_ONE_MINUS_SRC_ALPHA = 0x0303,
           GL_FUNC_ADD = 0x8006, GL_FUNC_SUBTRACT = 0x800A, GL_FUNC_REVERSE_SUBTRACT = 0x800B,
           GL_FUNC_MIN = 0x8007, GL_FUNC_MAX = 0x8008 };

    if (!s.blendEnabled) return "none";
    if (s.blendEqRgb == GL_FUNC_SUBTRACT) return "subtract";
    if (s.blendEqRgb == GL_FUNC_REVERSE_SUBTRACT) return "reverse_subtract";
    if (s.blendEqRgb == GL_FUNC_MIN) return "min";
    if (s.blendEqRgb == GL_FUNC_MAX) return "max";
    if (s.blendEqRgb == GL_FUNC_ADD) {
        if (s.blendSrcRgb == GL_ONE && s.blendDstRgb == GL_ONE) return "add";
        if (s.blendSrcRgb == GL_SRC_ALPHA && s.blendDstRgb == GL_ONE_MINUS_SRC_ALPHA) return "alpha";
        if (s.blendSrcRgb == GL_ONE && s.blendDstRgb == GL_ONE_MINUS_SRC_ALPHA) return "premultiplied";
    }
    if (fallback && cap) {
        snprintf(fallback, cap, "x%04x:%04x:%04x",
                 (unsigned)s.blendSrcRgb, (unsigned)s.blendDstRgb, (unsigned)s.blendEqRgb);
        return fallback;
    }
    return "x";
}

// ---------------------------------------------------------------- 日志格式化

static const char* BlendNameFromRecord (const DrawRecord& r, char* fallback, size_t cap);

namespace {

struct LineFields {
    char f[16];
    char s[16];
    char prog[16];
    char rt[16];
    const char* prim;
    char n[16];
    char v0[16];
    char idx[16];
    char slots[128];
    char blend[48];
    char depth[4];
    char dwrite[4];
    char scissor[48];
    char viewport[48];
    char u[16];
};

void FillFields (const DrawRecord& r, const char* uHex, LineFields& o) {
    snprintf(o.f, sizeof(o.f), "%u", (unsigned)r.frameGen);
    snprintf(o.s, sizeof(o.s), "%u", (unsigned)r.seq);
    snprintf(o.prog, sizeof(o.prog), "%u", (unsigned)r.program);
    snprintf(o.rt, sizeof(o.rt), "%u", (unsigned)r.fbo);
    // 与记录侧 VKDrawSink.buildLine 逐字一致：prim 恒为 tris（clear 行同样如此），
    // clear 行 n 固定 0（packet 里 count 本就为 0，这里再强制一次防止越界值进摘要）。
    o.prim = "tris";
    snprintf(o.n, sizeof(o.n), "%d", (int)((r.kind == KIND_CLEAR) ? 0 : r.count));
    if (r.kind == KIND_DRAW_ARRAYS) {
        snprintf(o.v0, sizeof(o.v0), "%d", (int)r.first);
        snprintf(o.idx, sizeof(o.idx), "-");
    } else if (r.kind == KIND_DRAW_ELEMENTS) {
        snprintf(o.v0, sizeof(o.v0), "-");
        snprintf(o.idx, sizeof(o.idx), "%d", (int)r.count);
    } else {
        snprintf(o.v0, sizeof(o.v0), "-");
        snprintf(o.idx, sizeof(o.idx), "-");
    }

    // slots：按填充顺序（= shader 声明顺序），未绑定写 '-'。
    {
        size_t used = 0;
        for (uint32_t i = 0; i < kMaxTexSlots; i++) {
            if (r.tex[i].unit > 7) break;
            if (used) o.slots[used++] = ':';
            if (r.tex[i].texName) {
                int wrote = snprintf(o.slots + used, sizeof(o.slots) - used, "%u",
                                     (unsigned)r.tex[i].texName);
                if (wrote > 0) used += (size_t)wrote;
            } else {
                if (used + 1 < sizeof(o.slots)) o.slots[used++] = '-';
            }
            if (used + 2 >= sizeof(o.slots)) break;
        }
        if (!used) {
            o.slots[0] = '-';
            o.slots[1] = 0;
        } else {
            o.slots[used] = 0;
        }
    }

    char blendFallback[48];
    snprintf(o.blend, sizeof(o.blend), "%s",
             BlendNameFromRecord(r, blendFallback, sizeof(blendFallback)));
    snprintf(o.depth, sizeof(o.depth), "%u", (unsigned)(r.depthTestEnabled ? 1 : 0));
    snprintf(o.dwrite, sizeof(o.dwrite), "%u", (unsigned)(r.depthMask ? 1 : 0));

    if (!r.scissorEnabled || r.scissorW <= 0 || r.scissorH <= 0) {
        snprintf(o.scissor, sizeof(o.scissor), "0,0,0,0");
    } else {
        snprintf(o.scissor, sizeof(o.scissor), "%d,%d,%d,%d",
                 (int)r.scissorX, (int)r.scissorY, (int)r.scissorW, (int)r.scissorH);
    }
    snprintf(o.viewport, sizeof(o.viewport), "%d,%d,%d,%d",
             (int)r.vpX, (int)r.vpY, (int)r.vpW, (int)r.vpH);
    snprintf(o.u, sizeof(o.u), "%s", uHex ? uHex : "00000000");
}

} // namespace

// record -> StateInput 视图，只供 BlendName 复用（不改任何状态）。
static const char* BlendNameFromRecord (const DrawRecord& r, char* fallback, size_t cap) {
    StateInput s;
    s.blendEnabled = r.blendEnabled;
    s.blendSrcRgb = r.blendSrcRgb;
    s.blendDstRgb = r.blendDstRgb;
    s.blendEqRgb = r.blendEqRgb;
    return BlendName(s, fallback, cap);
}

bool BuildCanonical (const DrawRecord& r, const char* uHex, char* out, size_t cap) {
    if (!out || !cap) return false;
    LineFields f;
    FillFields(r, uHex, f);
    // 规范串不含 s 与 sum；字段顺序与 tools/vkaudit/SPEC.md §4 一致。
    snprintf(out, cap,
             "f=%s;prog=%s;rt=%s;prim=%s;n=%s;v0=%s;idx=%s;slots=%s;blend=%s;"
             "depth=%s;dwrite=%s;scissor=%s;viewport=%s;u=%s",
             f.f, f.prog, f.rt, f.prim, f.n, f.v0, f.idx, f.slots, f.blend,
             f.depth, f.dwrite, f.scissor, f.viewport, f.u);
    return true;
}

bool BuildSum (const char* canonical, char out12[13]) {
    if (!canonical || !out12) return false;
    uint8_t d[32];
    Sha256(canonical, strlen(canonical), d);
    static const char* h = "0123456789abcdef";
    for (int i = 0; i < 12; i++) {
        uint8_t byte = d[i / 2];
        out12[i] = h[(i % 2 == 0) ? (byte >> 4) : (byte & 0xF)];
    }
    out12[12] = 0;
    return true;
}

bool BuildLogLine (const char* prefix, const DrawRecord& r, const char* uHex,
                   const char* extra, char* out, size_t cap) {
    if (!out || !cap) return false;
    LineFields f;
    FillFields(r, uHex, f);
    char canonical[kRecordBytes * 2];
    BuildCanonical(r, uHex, canonical, sizeof(canonical));
    char sum[13];
    BuildSum(canonical, sum);
    if (extra && extra[0]) {
        snprintf(out, cap,
                 "%s f=%s s=%s prog=%s rt=%s prim=%s n=%s v0=%s idx=%s slots=%s blend=%s "
                 "depth=%s dwrite=%s scissor=%s viewport=%s u=%s sum=%s %s",
                 prefix ? prefix : "[vkexec]", f.f, f.s, f.prog, f.rt, f.prim, f.n,
                 f.v0, f.idx, f.slots, f.blend, f.depth, f.dwrite, f.scissor,
                 f.viewport, f.u, sum, extra);
    } else {
        snprintf(out, cap,
                 "%s f=%s s=%s prog=%s rt=%s prim=%s n=%s v0=%s idx=%s slots=%s blend=%s "
                 "depth=%s dwrite=%s scissor=%s viewport=%s u=%s sum=%s",
                 prefix ? prefix : "[vkexec]", f.f, f.s, f.prog, f.rt, f.prim, f.n,
                 f.v0, f.idx, f.slots, f.blend, f.depth, f.dwrite, f.scissor,
                 f.viewport, f.u, sum);
    }
    return true;
}

// ---------------------------------------------------------------- arena

bool ArenaInit (Arena* a, uint32_t initialBytes) {
    if (!a) return false;
    a->base = (uint8_t*)malloc(initialBytes ? initialBytes : 1u);
    if (!a->base) {
        a->cap = 0;
        a->used = 0;
        a->oom = true;
        return false;
    }
    a->cap = initialBytes ? initialBytes : 1u;
    a->used = 0;
    a->oom = false;
    return true;
}

void ArenaFree (Arena* a) {
    if (!a) return;
    free(a->base);
    a->base = nullptr;
    a->cap = 0;
    a->used = 0;
    a->oom = false;
}

void ArenaReset (Arena* a) {
    if (!a) return;
    a->used = 0;
    a->oom = false;
}

static bool ArenaEnsure (Arena* a, uint32_t need) {
    if (need <= a->cap - a->used) return true;
    uint32_t want = a->cap ? a->cap : (1u << 20);
    while (want < need && want < 0x80000000u) want *= 2;
    if (want < need) want = need;
    uint8_t* nb = (uint8_t*)malloc(want);
    if (!nb) {
        a->oom = true;
        return false;
    }
    if (a->base && a->used) memcpy(nb, a->base, a->used);
    free(a->base);
    a->base = nb;
    a->cap = want;
    return true;
}

bool ArenaAlloc (Arena* a, uint32_t bytes, uint32_t align, uint32_t* outOff) {
    if (!a) return false;
    if (!align) align = 1;
    uint32_t start = a->used;
    if (align > 1) {
        uint32_t rem = start % align;
        if (rem) start += align - rem;
    }
    if (bytes > 0x7FFFFFFFu) {
        a->oom = true;
        return false;
    }
    uint32_t end = start + bytes;
    if (!ArenaEnsure(a, end)) return false;
    if (outOff) *outOff = start;
    a->used = end;
    return true;
}

bool ArenaAllocCopy (Arena* a, const void* src, uint32_t bytes, uint32_t align, uint32_t* outOff) {
    uint32_t off = 0;
    if (!ArenaAlloc(a, bytes, align, &off)) return false;
    if (src && bytes) memcpy(a->base + off, src, bytes);
    if (outOff) *outOff = off;
    return true;
}

} // namespace vkpkt
} // namespace lime
