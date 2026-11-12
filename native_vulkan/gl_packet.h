#pragma once

// packet 解码运行时（gl_packet.cpp）与 GL 状态镜像之间的窄接口。
//
// 本头不得 include 任何平台 / Vulkan / hxcpp 头；实现分别在 gl_core.cpp
// （属性/纹理/缓冲）与 gl_fbo.cpp（FBO 目标）里，保持"新增解码不散进渲染器"。

#include <stdint.h>
#include <stddef.h>

namespace lime {
namespace pkt {

// g_attrs[i] 的只读快照。normalized 目前未被 gl_core 记录，恒为 0（见 STATUS 缺口说明）。
struct AttrSnap {

    uint32_t bufferName;    // glVertexAttribPointer 时的 GL_ARRAY_BUFFER；0 = client
    uint32_t byteOffset;    // 数组内字节偏移（client 时为 0）
    int32_t stride;
    int32_t size;
    int32_t glType;         // GL 原始枚举
    uint8_t kind;           // 0=disabled 1=array 2=constant 3=client
    uint8_t enabled;
    uint8_t client;
    uint8_t constSet;
    uint8_t constN;
    uint8_t reserved[3];
    float constValue[4];
    const void* clientPtr;

};

// 抓取第 index 个属性槽（0..15）。未初始化槽返回 false。
bool SnapshotAttr (int index, AttrSnap* out);

// 第 unit 个纹理单元当前绑定的 GL 纹理名（0 = 未绑定）。
int SnapshotTexBound (int unit);

// 当前 active texture unit 上绑定的 GL 纹理名（与批处理 DrawRaw 同口径）。
int SnapshotTexBoundActive ();

// GL 纹理名 → VkRenderer texture id（离屏别名同样适用）。
uint32_t SnapshotTexVk (int glName);

// GL buffer 内容（只读）。name 无效或为空时返回 false。
bool SnapshotBuffer (int name, const uint8_t** data, uint32_t* bytes);

// 执行路径是否在工作（VK_PKT_EXEC=1）。GL 侧据此跳过对应绘制，避免同一笔画两遍。
bool ExecActive ();

// 按 packet 立即执行一笔（内部再 ParseWire 一次）。仅在 ExecActive() 为真时由 emit 调用。
bool ExecPacketRaw (const uint8_t* bytes, uint32_t nbytes);

// 当前 packet 帧代（就是 trace 里的 f=）。取证转储（VK_DUMP_TEX）用它对齐"该帧"。
uint32_t SnapshotFrameGen ();

} // namespace pkt
} // namespace lime

// Haxe CFFI 入口（cpp prim 名，注册在 patch_lime.py 注入的 DEFINE_PRIME 行）。
// 全部返回 int；VK_PKT=0（默认）时 mode/emit/frame/utag 都是零开销空实现。
namespace lime {

int lime_vk_pkt_mode ();
int lime_vk_pkt_emit (double pointer, int bytes);
int lime_vk_pkt_utag ();
int lime_vk_pkt_frame (int frameGen, double animTime);

}
