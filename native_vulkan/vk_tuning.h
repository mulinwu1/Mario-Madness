// 运行时调参（vk_tuning.ini）：放在 exe 同目录，缺失即用内置默认。
//
// 约定（与本项目其它模块一致）：
//   * 默认值 === 加这个模块之前的写死值。改 ini 才偏离默认，便于区分
//     “配置导致的变化”与“代码回归”。
//   * 只读一次（进程内首次 Init 时），改文件重启即可生效，不需要重编 ndll。
//   * 解析失败一律回落到默认值，并在 [vk] tuning 行里标出，不阻断启动。
#pragma once

#include <stdint.h>

namespace vktuning {

// 过滤：0 = 沿用 GL 记录值（默认），1 = 强制 NEAREST，2 = 强制 LINEAR
enum { FILTER_GL = 0, FILTER_NEAREST = 1, FILTER_LINEAR = 2 };
// wrap：0 = 沿用 GL 记录值（默认），1 = clamp，2 = repeat，3 = mirror
enum { WRAP_GL = 0, WRAP_CLAMP = 1, WRAP_REPEAT = 2, WRAP_MIRROR = 3 };
// 混合变体：0 = 现在写死的那套（SRC_ALPHA / ONE_MINUS_SRC_ALPHA）
enum { BLEND_SRC_ALPHA = 0, BLEND_PREMUL = 1, BLEND_ADDITIVE = 2, BLEND_OFF = 3 };
// 保留位：-1 = 沿用记录值（默认）
enum { KEEP = -1, OFF = 0, ON = 1 };
// viewport / scissor 策略：0 = 按记录逐笔（默认），1 = 按 target 全幅
enum { POLICY_RECORDED = 0, POLICY_FULL_TARGET = 1 };

struct SizeRule {
    int maxDim;   // 适用上限：max(w,h) <= maxDim 时命中（mode 的含义与上面两套枚举一致）
    int mode;
};

struct Values {
    int filterDefault;
    int filterRuleCount;
    SizeRule filterRules[8];
    int filterByNameCount;
    uint32_t filterByNames[16];   // GL 纹理名（不是文件路径）

    int wrapDefault;
    int wrapRuleCount;
    SizeRule wrapRules[8];

    int blendVariant;

    // 预留：当前渲染只有 1 个无深度附件的管线，这三项只改 [pkt] draw 里记录的状态位
    int depthTest;
    int depthWrite;
    int stencilTest;

    int viewportPolicy;
    int scissorPolicy;

    int mipGenerate;   // 预留：纹理只分配 1 级 mip，打开不会凭空生成 level
};

void Load ();
const Values& Get ();
const char* Source ();       // "default" 或 "vk_tuning.ini"（不含路径）
void LogEffective ();        // [vk] tuning k=v ...（只打一次）

// 把 GL 传进来的过滤 / wrap 与 ini 规则合成实际要用的 GL 枚举
int ResolveFilter (int glFilter, int w, int h, uint32_t glName);
int ResolveWrap (int glWrap, int w, int h);

} // namespace vktuning
