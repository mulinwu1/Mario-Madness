// vk_tuning.ini 解析与生效值（详见 docs/vk_tuning.md）
//
// 为什么不做成“热重载”：采样器 / 管线是在纹理与设备初始化时建的，热重载要能重建
// 全部采样器与管线，收益远小于风险。重启游戏（1~2 秒）足够快，也不会引入
// “改了但一半生效”的中间态。
#include "vk_tuning.h"
#include "vk_platform.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>

namespace vktuning {

static Values g_v;
static bool g_loaded = false;
static bool g_logged = false;
static char g_src[64] = "default";
static char g_bad[256] = {0};   // 解析失败的条目，打进 [vk] tuning 行便于定位

// ---------------------------------------------------------------- 默认值
// 除 blendVariant 外全部等于引入本模块之前的写死行为；本轮只动这一个变量。
static void SetDefaults () {
    memset (&g_v, 0, sizeof (g_v));
    g_v.filterDefault = FILTER_GL;
    g_v.wrapDefault = WRAP_GL;
    // GL 侧 OpenFL 每笔都发 glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA)
    // （实跑日志 [gldraw/ie] blend=1,1,771），真 prog3 frag 输出的也是预乘色
    // （vk_prog_3.frag:70 的 color.rgb*color.a）。默认混合必须与之一致，
    // 否则预乘色再被 SRC_ALPHA 乘一次：离屏画布退化成非预乘态并整体变暗，
    // 下一轮采样时 frag:58 的 rgb/a 被 clamp 压成三通道全 1（顶带中性灰 74、
    // 整屏 97.6% 黑，第十一轮实测）。回退：vk_tuning.ini 写 blend=srcalpha，无需重编。
    g_v.blendVariant = BLEND_PREMUL;
    g_v.depthTest = KEEP;
    g_v.depthWrite = KEEP;
    g_v.stencilTest = KEEP;
    g_v.viewportPolicy = POLICY_RECORDED;
    g_v.scissorPolicy = POLICY_RECORDED;
    // image 路径会让 nvoglv64.dll 崩溃（0xc0000005），先默认 off；屏障修好后验证不崩再切回 image
    g_v.offscreenClear = CLEAR_OFF;
    // 同目标重绑的"真清除"默认打开：这是修复画布永不清的病灶路径，且不依赖 offscreen_clear
    g_v.offscreenRebindClear = ON;
    // 预览开关：默认 = 现状（LOAD / identity），编出来必须与现状逐像素一致
    g_v.offscreenLoadop = LOADOP_LOAD;
    g_v.swizzle = SWIZZLE_NONE;
    g_v.mipGenerate = 0;
}

// ---------------------------------------------------------------- 词法
static void Trim (char* s) {
    if (!s) return;
    char* p = s;
    while (*p && (unsigned char)*p <= ' ') ++p;
    if (p != s) memmove (s, p, strlen (p) + 1);
    size_t n = strlen (s);
    while (n > 0) {
        unsigned char c = (unsigned char)s[n - 1];
        if (c > ' ') break;
        s[--n] = '\0';
    }
}

static void Lower (char* s) {
    for (; s && *s; ++s) *s = (char)tolower ((unsigned char)*s);
}

static int ParseFilterMode (const char* v) {
    if (!strcmp (v, "gl") || !strcmp (v, "keep")) return FILTER_GL;
    if (!strcmp (v, "nearest")) return FILTER_NEAREST;
    if (!strcmp (v, "linear")) return FILTER_LINEAR;
    return -1;
}

static int ParseWrapMode (const char* v) {
    if (!strcmp (v, "gl") || !strcmp (v, "keep")) return WRAP_GL;
    if (!strcmp (v, "clamp")) return WRAP_CLAMP;
    if (!strcmp (v, "repeat")) return WRAP_REPEAT;
    if (!strcmp (v, "mirror")) return WRAP_MIRROR;
    return -1;
}

static int ParseOnOff (const char* v) {
    if (!strcmp (v, "keep")) return KEEP;
    if (!strcmp (v, "on") || !strcmp (v, "1") || !strcmp (v, "true")) return ON;
    if (!strcmp (v, "off") || !strcmp (v, "0") || !strcmp (v, "false")) return OFF;
    return -2;
}

static int ParsePolicy (const char* v) {
    if (!strcmp (v, "keep") || !strcmp (v, "recorded")) return POLICY_RECORDED;
    if (!strcmp (v, "full") || !strcmp (v, "target")) return POLICY_FULL_TARGET;
    return -1;
}

static int ParseOffscreenClear (const char* v) {
    if (!strcmp (v, "image")) return CLEAR_IMAGE;
    if (!strcmp (v, "loadop")) return CLEAR_LOADOP;
    if (!strcmp (v, "off") || !strcmp (v, "none")) return CLEAR_OFF;
    if (!strcmp (v, "imgcmd")) return CLEAR_IMGCMD;
    return -1;
}

static int ParseBlend (const char* v) {
    if (!strcmp (v, "srcalpha") || !strcmp (v, "default")) return BLEND_SRC_ALPHA;
    if (!strcmp (v, "premul") || !strcmp (v, "premultiplied")) return BLEND_PREMUL;
    if (!strcmp (v, "add") || !strcmp (v, "additive")) return BLEND_ADDITIVE;
    if (!strcmp (v, "off") || !strcmp (v, "none")) return BLEND_OFF;
    return -1;
}

static int ParseLoadop (const char* v) {
    if (!strcmp (v, "load") || !strcmp (v, "keep")) return LOADOP_LOAD;
    if (!strcmp (v, "clear")) return LOADOP_CLEAR;
    return -1;
}

static int ParseSwizzle (const char* v) {
    if (!strcmp (v, "none") || !strcmp (v, "identity") || !strcmp (v, "keep")) return SWIZZLE_NONE;
    if (!strcmp (v, "rb") || !strcmp (v, "bgra")) return SWIZZLE_RB;
    return -1;
}

static void AddBad (const char* key, const char* val) {
    size_t used = strlen (g_bad);
    if (used > sizeof (g_bad) - 48) return;
    snprintf (g_bad + used, sizeof (g_bad) - used, "%s%s=%s", used ? "," : "", key, val);
}

// 形如 "64:nearest,128:antialias" -> 追加到 rules；mode 由调用方给的解析函数决定
static int ParseSizeRules (char* val, SizeRule* out, int cap, int (*parseMode) (const char*)) {
    int n = 0;
    char* p = val;
    while (p && *p && n < cap) {
        char* comma = strchr (p, ',');
        if (comma) *comma = '\0';
        Trim (p);
        char* colon = strchr (p, ':');
        if (colon) {
            *colon = '\0';
            char* dimStr = p;
            char* modeStr = colon + 1;
            Trim (dimStr);
            Trim (modeStr);
            int dim = atoi (dimStr);
            int mode = parseMode (modeStr);
            if (dim > 0 && mode >= 0) {
                out[n].maxDim = dim;
                out[n].mode = mode;
                ++n;
            }
        }
        if (!comma) break;
        p = comma + 1;
    }
    return n;
}

static int ParseNameRules (char* val, uint32_t* out, int cap, int (*parseMode) (const char*), int* err) {
    // 只支持 "12:nearest" 形式；名字是 GL 纹理名（记录侧拿不到文件路径，见文档）
    int n = 0;
    char* p = val;
    while (p && *p && n < cap) {
        char* comma = strchr (p, ',');
        if (comma) *comma = '\0';
        Trim (p);
        char* colon = strchr (p, ':');
        if (colon) {
            *colon = '\0';
            Trim (p);
            Trim (colon + 1);
            int name = atoi (p);
            int mode = parseMode (colon + 1);
            if (name > 0 && mode >= 0) {
                out[n++] = (uint32_t)name;
            } else {
                *err = 1;
            }
        }
        if (!comma) break;
        p = comma + 1;
    }
    return n;
}

// ---------------------------------------------------------------- 读取
static void Apply (char* key, char* val) {
    Lower (key);
    Lower (val);

    if (!strcmp (key, "texture.filter") || !strcmp (key, "filter")) {
        int m = ParseFilterMode (val);
        if (m < 0) AddBad (key, val); else g_v.filterDefault = m;
    } else if (!strcmp (key, "texture.filter.by_size") || !strcmp (key, "filter.by_size")) {
        g_v.filterRuleCount = ParseSizeRules (val, g_v.filterRules, 8, ParseFilterMode);
    } else if (!strcmp (key, "texture.filter.by_glname") || !strcmp (key, "filter.by_glname")) {
        int err = 0;
        g_v.filterByNameCount = ParseNameRules (val, g_v.filterByNames, 16, ParseFilterMode, &err);
        if (err) AddBad (key, val);
    } else if (!strcmp (key, "texture.wrap") || !strcmp (key, "wrap")) {
        int m = ParseWrapMode (val);
        if (m < 0) AddBad (key, val); else g_v.wrapDefault = m;
    } else if (!strcmp (key, "texture.wrap.by_size") || !strcmp (key, "wrap.by_size")) {
        g_v.wrapRuleCount = ParseSizeRules (val, g_v.wrapRules, 8, ParseWrapMode);
    } else if (!strcmp (key, "blend")) {
        int m = ParseBlend (val);
        if (m < 0) AddBad (key, val); else g_v.blendVariant = m;
    } else if (!strcmp (key, "depth.test")) {
        int m = ParseOnOff (val);
        if (m == -2) AddBad (key, val); else g_v.depthTest = m;
    } else if (!strcmp (key, "depth.write")) {
        int m = ParseOnOff (val);
        if (m == -2) AddBad (key, val); else g_v.depthWrite = m;
    } else if (!strcmp (key, "stencil.test")) {
        int m = ParseOnOff (val);
        if (m == -2) AddBad (key, val); else g_v.stencilTest = m;
    } else if (!strcmp (key, "viewport")) {
        int m = ParsePolicy (val);
        if (m < 0) AddBad (key, val); else g_v.viewportPolicy = m;
    } else if (!strcmp (key, "scissor")) {
        int m = ParsePolicy (val);
        if (m < 0) AddBad (key, val); else g_v.scissorPolicy = m;
    } else if (!strcmp (key, "mip")) {
        int m = ParseOnOff (val);
        if (m == -2) AddBad (key, val);
        else g_v.mipGenerate = (m == ON) ? 1 : 0;
    } else if (!strcmp (key, "offscreen_clear")) {
        int m = ParseOffscreenClear (val);
        if (m < 0) AddBad (key, val); else g_v.offscreenClear = m;
    } else if (!strcmp (key, "offscreen_rebind_clear")) {
        int m = ParseOnOff (val);
        if (m == -2 || m == KEEP) AddBad (key, val);
        else g_v.offscreenRebindClear = (m == ON) ? ON : OFF;
    } else if (!strcmp (key, "offscreen_loadop")) {
        int m = ParseLoadop (val);
        if (m < 0) AddBad (key, val); else g_v.offscreenLoadop = m;
    } else if (!strcmp (key, "swizzle")) {
        int m = ParseSwizzle (val);
        if (m < 0) AddBad (key, val); else g_v.swizzle = m;
    } else {
        // 未知键不报错（允许留着其他分支的调参），但记进 [vk] tuning 的 unknown=
        AddBad (key, val);
    }
}

void Load () {
    if (g_loaded) return;
    g_loaded = true;
    SetDefaults ();

    char dir[512] = {0};
    vkplat::ExeDir (dir, (int)sizeof (dir));
    // 安卓 ExeDir 恒为空串（进程在 /system/bin），此时只用默认值，不去探测文件
    if (!dir[0]) return;

    char path[600];
    snprintf (path, sizeof (path), "%s%s", dir, "vk_tuning.ini");

    FILE* f = fopen (path, "rb");
    if (!f) return;

    char line[512];
    bool first = true;
    while (fgets (line, (int)sizeof (line), f)) {
        char* p = line;
        // UTF-8 BOM
        if (first && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB) p += 3;
        first = false;
        Trim (p);
        if (!p[0] || p[0] == ';' || p[0] == '#' || p[0] == '[') continue;
        char* eq = strchr (p, '=');
        if (!eq) continue;
        *eq = '\0';
        char* key = p;
        char* val = eq + 1;
        Trim (key);
        Trim (val);
        if (!key[0]) continue;
        Apply (key, val);
    }
    fclose (f);
    snprintf (g_src, sizeof (g_src), "vk_tuning.ini");
}

const Values& Get () {
    if (!g_loaded) Load ();
    return g_v;
}

const char* Source () {
    if (!g_loaded) Load ();
    return g_src;
}

// ---------------------------------------------------------------- 生效值日志
static const char* FilterName (int m) {
    return m == FILTER_NEAREST ? "nearest" : (m == FILTER_LINEAR ? "linear" : "gl");
}

static const char* WrapName (int m) {
    switch (m) {
        case WRAP_CLAMP: return "clamp";
        case WRAP_REPEAT: return "repeat";
        case WRAP_MIRROR: return "mirror";
        default: return "gl";
    }
}

static const char* BlendName (int m) {
    switch (m) {
        case BLEND_PREMUL: return "premul";
        case BLEND_ADDITIVE: return "additive";
        case BLEND_OFF: return "off";
        default: return "srcalpha";
    }
}

static const char* PolicyName (int m) {
    return m == POLICY_FULL_TARGET ? "full" : "keep";
}

static const char* OffscreenClearName (int m) {
    if (m == CLEAR_LOADOP) return "loadop";
    if (m == CLEAR_OFF) return "off";
    if (m == CLEAR_IMGCMD) return "imgcmd";
    return "image";
}

static const char* TriName (int m) {
    return m == ON ? "on" : (m == OFF ? "off" : "keep");
}

void LogEffective () {
    if (g_logged) return;
    g_logged = true;
    const Values& v = g_v;

    char sizeRules[192] = {0};
    for (int i = 0; i < v.filterRuleCount; ++i) {
        char one[32];
        snprintf (one, sizeof (one), "%s%d:%s", i ? "," : "",
                  v.filterRules[i].maxDim, FilterName (v.filterRules[i].mode));
        strncat (sizeRules, one, sizeof (sizeRules) - strlen (sizeRules) - 1);
    }
    char wrapRules[192] = {0};
    for (int i = 0; i < v.wrapRuleCount; ++i) {
        char one[32];
        snprintf (one, sizeof (one), "%s%d:%s", i ? "," : "",
                  v.wrapRules[i].maxDim, WrapName (v.wrapRules[i].mode));
        strncat (wrapRules, one, sizeof (wrapRules) - strlen (wrapRules) - 1);
    }
    char names[192] = {0};
    for (int i = 0; i < v.filterByNameCount; ++i) {
        char one[16];
        snprintf (one, sizeof (one), "%s%u", i ? "," : "", (unsigned)v.filterByNames[i]);
        strncat (names, one, sizeof (names) - strlen (names) - 1);
    }

    fprintf (stderr,
             "[vk] tuning src=%s texture.filter=%s texture.filter.by_size=%s texture.filter.by_glname=%s "
             "texture.wrap=%s texture.wrap.by_size=%s blend=%s depth.test=%s depth.write=%s stencil.test=%s "
             "viewport=%s scissor=%s offscreen_clear=%s offscreen_rebind_clear=%s "
             "offscreen_loadop=%s swizzle=%s mip=%s unknown=%s\n",
             g_src,
             FilterName (v.filterDefault), sizeRules[0] ? sizeRules : "-", names[0] ? names : "-",
             WrapName (v.wrapDefault), wrapRules[0] ? wrapRules : "-",
             BlendName (v.blendVariant),
             TriName (v.depthTest), TriName (v.depthWrite), TriName (v.stencilTest),
             PolicyName (v.viewportPolicy), PolicyName (v.scissorPolicy),
             OffscreenClearName (v.offscreenClear),
             v.offscreenRebindClear == ON ? "on" : "off",
             v.offscreenLoadop == LOADOP_CLEAR ? "clear" : "load",
             v.swizzle == SWIZZLE_RB ? "rb" : "none",
             v.mipGenerate ? "on" : "off",
             g_bad[0] ? g_bad : "-");
}

// ---------------------------------------------------------------- 合成
static int FilterToGl (int mode) {
    return (mode == FILTER_NEAREST) ? 0x2600 /* GL_NEAREST */ : 0x2601 /* GL_LINEAR */;
}

static int WrapToGl (int mode) {
    switch (mode) {
        case WRAP_REPEAT: return 0x2901;   // GL_REPEAT
        case WRAP_MIRROR: return 0x8370;   // GL_MIRRORED_REPEAT
        case WRAP_CLAMP:  return 0x812F;   // GL_CLAMP_TO_EDGE
        default:          return 0;        // 0 = 不覆盖
    }
}

int ResolveFilter (int glFilter, int w, int h, uint32_t glName) {
    const Values& v = Get ();
    // 1) 纹理名白名单优先（像素画按名字锁定 nearest）
    for (int i = 0; i < v.filterByNameCount; ++i) {
        if (v.filterByNames[i] == glName) return FilterToGl (FILTER_NEAREST);
    }
    // 2) 尺寸规则：小图默认按 nearest 处理（像素画最常见的判据）
    int maxDim = (w > h) ? w : h;
    if (maxDim > 0) {
        for (int i = 0; i < v.filterRuleCount; ++i) {
            if (maxDim <= v.filterRules[i].maxDim) return FilterToGl (v.filterRules[i].mode);
        }
    }
    // 3) 全局默认
    if (v.filterDefault != FILTER_GL) return FilterToGl (v.filterDefault);
    return glFilter;
}

int ResolveWrap (int glWrap, int w, int h) {
    const Values& v = Get ();
    int maxDim = (w > h) ? w : h;
    if (maxDim > 0) {
        for (int i = 0; i < v.wrapRuleCount; ++i) {
            if (maxDim <= v.wrapRules[i].maxDim) return WrapToGl (v.wrapRules[i].mode);
        }
    }
    if (v.wrapDefault != WRAP_GL) return WrapToGl (v.wrapDefault);
    return glWrap;
}

} // namespace vktuning
