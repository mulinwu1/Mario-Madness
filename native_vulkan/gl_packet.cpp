#include "gl_packet.h"
#include "vk_packet.h"
#include "gl_state.h"

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

// ---------------------------------------------------------------- 状态快照（阶段 ①）
//
// 只读：全部来自 gl_state 的镜像查询，不写任何 GL 状态、不分配。
// 开关 VK_PKT_STATE=1；可选 VK_PKT_STATE_FRAME=<帧号> 只打指定帧，
// VK_PKT_STATE_MAX=<n> 每帧最多打 n 笔（默认 32，防日志洪水）。
//
// 注意：QueryInteger 只对本模块认识的 pname 调用（先 HandlesPname），
// 否则会写入 GL_INVALID_ENUM 污染 glGetError。

int g_stateMode = -1; // -1 未探测；0 关；1 开
int g_stateFrame = -1; // -1 所有帧
int g_stateMax = 32;
int g_stateUsed = 0;

bool StateWant(uint32_t frameGen)
{
	if (g_stateMode < 0)
	{
		const char* v = getenv("VK_PKT_STATE");
		g_stateMode = (v && v[0] == '1') ? 1 : 0;
		const char* vf = getenv("VK_PKT_STATE_FRAME");
		if (vf && vf[0]) g_stateFrame = atoi(vf);
		const char* vm = getenv("VK_PKT_STATE_MAX");
		if (vm && vm[0]) g_stateMax = atoi(vm);
		if (g_stateMode)
		{
			fprintf(stderr, "[vkpkt] state on frame=%d max=%d\n", g_stateFrame, g_stateMax);
		}
	}
	if (!g_stateMode) return false;
	if (g_stateFrame >= 0 && (int)frameGen != g_stateFrame) return false;
	if (g_stateUsed >= g_stateMax) return false;
	g_stateUsed++;
	return true;
}

int Q1(int pname, int fallback)
{
	if (!glstate::HandlesPname(pname)) return fallback;
	int v = fallback;
	if (glstate::QueryInteger(pname, &v, 1) == 0) return fallback;
	return v;
}

const char* BlendName(int enabled, int eq, int src, int dst)
{
	static char buf[32];
	if (!enabled) return "none";
	switch (eq)
	{
		case 0x800A: return "subtract";
		case 0x800B: return "reverse_subtract";
		case 0x8007: return "min";
		case 0x8008: return "max";
		default: break;
	}
	if (eq == 0x8006)
	{
		if (src == 0x0001 && dst == 0x0001) return "add";
		if (src == 0x0302 && dst == 0x0303) return "alpha";
		if (src == 0x0001 && dst == 0x0303) return "premultiplied";
	}
	snprintf(buf, sizeof(buf), "x%04x:%04x:%04x", src, dst, eq);
	return buf;
}

// 字段口径对齐 docs/draw_packet_spec.md §10。
void PrintState(const vkpkt::WirePacket& w)
{
	const int GL_BLEND = 0x0BE2;
	const int GL_DEPTH_TEST = 0x0B71;
	const int GL_PNAME_BLEND_SRC_RGB = 0x80C9;
	const int GL_PNAME_BLEND_DST_RGB = 0x80C8;
	const int GL_PNAME_BLEND_EQ_RGB = 0x8009;
	const int GL_PNAME_DEPTH_WRITEMASK = 0x0B72;

	int blendOn = glstate::IsEnabled(GL_BLEND) ? 1 : 0;
	int src = Q1(GL_PNAME_BLEND_SRC_RGB, 0x0001);
	int dst = Q1(GL_PNAME_BLEND_DST_RGB, 0x0000);
	int eq = Q1(GL_PNAME_BLEND_EQ_RGB, 0x8006);
	int depth = glstate::IsEnabled(GL_DEPTH_TEST) ? 1 : 0;
	int dwrite = Q1(GL_PNAME_DEPTH_WRITEMASK, 1) ? 1 : 0;

	int sc[4] = {0, 0, 0, 0};
	glstate::GetScissorBox(sc);
	int vp[4] = {0, 0, 0, 0};
	glstate::GetViewportBox(vp);
	int cm[4] = {1, 1, 1, 1};
	glstate::GetColorMask(cm);

	int isClear = (w.kind == vkpkt::KIND_CLEAR);
	int n = isClear ? 0 : w.count;
	int v0 = (w.kind == vkpkt::KIND_DRAW_ARRAYS) ? w.first : -1;
	int idx = (w.kind == vkpkt::KIND_DRAW_ELEMENTS) ? w.count : -1;

	fprintf(stderr,
			"[vkpkt] f=%u s=%u prog=%u rt=%u prim=tris n=%d v0=%d idx=%d "
			"blend=%s depth=%d dwrite=%d scissor=%d,%d,%d,%d viewport=%d,%d,%d,%d cm=%d%d%d%d\n",
			(unsigned)w.frameGen, (unsigned)w.seq, (unsigned)w.program, (unsigned)w.fbo,
			n, v0, idx,
			BlendName(blendOn, eq, src, dst), depth, dwrite,
			sc[0], sc[1], sc[2], sc[3],
			vp[0], vp[1], vp[2], vp[3],
			cm[0], cm[1], cm[2], cm[3]);
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
	g_stateUsed = 0;
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
		g_stateUsed = 0;
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

	// 阶段 ①：只读状态快照（VK_PKT_STATE=1 才生效）
	if (StateWant(w.frameGen)) PrintState(w);

	return 1;
}

} // namespace lime
