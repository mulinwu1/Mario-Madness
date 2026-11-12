#include "gl_packet.h"
#include "vk_packet.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// packet 账本 v2（完全重写，最小版）
//
// 目标只有一个：证明 Haxe 侧的 WirePacket 流能稳定、按序、零风险地到达 native。
//
// 与旧实现的区别（旧版就是前面那条线写的 gl_packet.cpp）：
//   - 旧版在 emit 里抓 GL 状态（属性/纹理/blend/stencil）、物化 640B DrawRecord、
//     再把顶点/索引/常量复制进 3 个 arena。任何一处越界读写都会直接写坏进程内存，
//     表现为"字体花 + 0xc0000005"。
//   - 新版 emit 只做：校验 64B → memcpy 进固定环形缓冲 → 计数。不读 GL、不分配、
//     不碰 GL 对象。崩溃面被压到只剩一次 memcpy。
//   - 执行路径（VK_PKT_EXEC）暂时整体移除：ExecActive() 恒 false，GL 照常画。
//
// 后续阶段按顺序加回（每次只加一块、单独开关）：
//   1) 状态快照日志（只读 g_state，不 memcpy 顶点）
//   2) DrawRecord 物化 + arena（先不做执行）
//   3) VK_PKT_EXEC 逐笔直连 Vulkan
// ---------------------------------------------------------------------------

namespace lime {
namespace pkt {

namespace {

constexpr uint32_t kRingPackets = 32768; // 32768 * 64B = 2MB

struct Ledger {
	int mode = -1; // -1 未探测；0 关；1 开
	int logFrame = -1; // -1 未探测；0 摘要（每 60 帧）；1 每帧

	uint8_t* ring = nullptr;
	uint32_t ringCap = 0;

	uint32_t frameGen = 0xFFFFFFFFu;
	uint32_t nextSeq = 1;

	uint32_t framePackets = 0;
	uint32_t frameDraws = 0;
	uint32_t frameClears = 0;
	uint32_t frameSeqErr = 0;
	uint32_t frameReject = 0;

	uint64_t totalFrames = 0;
	uint64_t totalPackets = 0;
	uint64_t totalReject = 0;

	uint32_t lastProgram = 0;
	uint32_t lastFbo = 0;
};

Ledger g_l;

bool RingReady()
{
	if (g_l.ring) return true;
	size_t bytes = (size_t)kRingPackets * vkpkt::kWireBytes;
	g_l.ring = (uint8_t*)malloc(bytes);
	if (!g_l.ring) return false;
	memset(g_l.ring, 0, bytes);
	g_l.ringCap = kRingPackets;
	return true;
}

void ResetFrameCounters()
{
	g_l.framePackets = 0;
	g_l.frameDraws = 0;
	g_l.frameClears = 0;
	g_l.frameSeqErr = 0;
	g_l.frameReject = 0;
	g_l.nextSeq = 1;
}

// 每帧一行。summary 模式（VK_PKT=1）每 60 帧打一次，frame 模式（VK_PKT=2 / VK_PKT_LOG=1）每帧打。
void PrintFrame(const char* why)
{
	bool everyFrame = (g_l.logFrame == 1);
	if (!everyFrame && (g_l.totalFrames % 60) != 0) return;
	if (g_l.framePackets == 0 && g_l.frameReject == 0) return;

	fprintf(stderr,
			"[vkpkt] f=%u draws=%u clears=%u pkts=%u seqerr=%u reject=%u prog=%u rt=%u total=%llu (%s)\n",
			(unsigned)g_l.frameGen, (unsigned)g_l.frameDraws, (unsigned)g_l.frameClears,
			(unsigned)g_l.framePackets, (unsigned)g_l.frameSeqErr, (unsigned)g_l.frameReject,
			(unsigned)g_l.lastProgram, (unsigned)g_l.lastFbo,
			(unsigned long long)g_l.totalPackets, why);
}

bool LogEveryFrame()
{
	if (g_l.logFrame < 0)
	{
		const char* v = getenv("VK_PKT_LOG");
		g_l.logFrame = (v && v[0] == '1') ? 1 : 0;
	}
	return g_l.logFrame == 1;
}

} // namespace

// 执行路径恒关：GL 侧永远不会因为 packet 而跳过自己的绘制。
bool ExecActive()
{
	return false;
}

uint32_t SnapshotFrameGen()
{
	return g_l.frameGen;
}

} // namespace pkt
} // namespace lime

// ---------------------------------------------------------------- CFFI 入口

namespace lime {

using namespace pkt;

int lime_vk_pkt_mode()
{
	if (g_l.mode < 0)
	{
		const char* v = getenv("VK_PKT");
		g_l.mode = (v && (v[0] == '1' || v[0] == '2')) ? 1 : 0;
		if (g_l.mode)
		{
			bool ring = RingReady();
			bool every = LogEveryFrame() || (v && v[0] == '2');
			if (every) g_l.logFrame = 1;
			fprintf(stderr, "[vkpkt] on ring=%s log=%s\n", ring ? "2MB" : "FAILED",
					g_l.logFrame == 1 ? "frame" : "summary");
		}
	}
	return g_l.mode;
}

// 保留符号：v2 不再从 native 算 uniform 摘要（Haxe 也不调用它）。
int lime_vk_pkt_utag()
{
	return 0;
}

int lime_vk_pkt_frame(int frameGen, double animTime)
{
	(void)animTime;
	if (!lime_vk_pkt_mode()) return 0;

	g_l.frameGen = (uint32_t)frameGen;
	g_l.totalFrames++;
	PrintFrame("end");
	ResetFrameCounters();
	return 0;
}

int lime_vk_pkt_emit(double pointer, int bytes)
{
	if (!lime_vk_pkt_mode()) return 0;

	if (!pointer || bytes != (int)vkpkt::kWireBytes)
	{
		g_l.frameReject++;
		g_l.totalReject++;
		if (g_l.totalReject <= 4)
		{
			fprintf(stderr, "[vkpkt] err=bytes bytes=%d\n", bytes);
		}
		return -1;
	}

	// CFFI 约定：指针经 double 传递，先落整数再转指针（兼容 arm64）。
	const uint8_t* src = (const uint8_t*)(uintptr_t)(int64_t)pointer;

	vkpkt::WirePacket w;
	char err[48];
	if (!vkpkt::ParseWire(src, (uint32_t)bytes, &w, err, sizeof(err)))
	{
		g_l.frameReject++;
		g_l.totalReject++;
		if (g_l.totalReject <= 4)
		{
			fprintf(stderr, "[vkpkt] err=parse reason=%s\n", err);
		}
		return -1;
	}

	// 帧号变了但没收到 frame()：先把上一帧摘要打掉（某些平台可能不走 __render 末尾）。
	if (w.frameGen != g_l.frameGen)
	{
		PrintFrame("rollover");
		g_l.frameGen = w.frameGen;
		ResetFrameCounters();
	}

	if (w.seq != g_l.nextSeq) g_l.frameSeqErr++;
	g_l.nextSeq = w.seq + 1;

	// 只保留最近一帧的 packet（每帧从 0 覆盖写，不增长）。
	if (g_l.ring && g_l.framePackets < g_l.ringCap)
	{
		memcpy(g_l.ring + (size_t)g_l.framePackets * vkpkt::kWireBytes, src, vkpkt::kWireBytes);
	}

	g_l.framePackets++;
	if (w.kind == vkpkt::KIND_CLEAR)
	{
		g_l.frameClears++;
	}
	else
	{
		g_l.frameDraws++;
	}
	g_l.lastProgram = w.program;
	g_l.lastFbo = w.fbo;
	g_l.totalPackets++;

	return 1;
}

} // namespace lime
