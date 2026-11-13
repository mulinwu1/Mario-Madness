#include "gl_packet.h"
#include "vk_packet.h"
#include "gl_state.h"
#include "gl_shader.h"
#include "vk_renderer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

// 渲染器 / FBO / 纹理的只读查询（实现在 vk_renderer.cpp / gl_fbo.cpp / gl_texture_params.cpp）。
namespace lime {
VkRenderer* gl_vk_get_renderer ();
uint32_t gl_vk_fbo_target (int fboName);
bool gl_vk_texture_size (int glName, int* w, int* h);
bool gl_vk_texture_sampler_state (int glName, int* minFilter, int* magFilter,
                                  int* wrapS, int* wrapT);
}

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
	int exec = -1; // VK_PKT_EXEC：-1 未探测；0 关；1 开

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
void PrintState(const vkpkt::WirePacket& w){
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

// ---------------------------------------------------------------- uniform 快照（阶段 ②a）
//
// 只读：取当前 program 的完整 uniform 块（顶点 + 片元共用），算 32bit 摘要。
// 开关 VK_PKT_UNIFORM=1；VK_PKT_UNIFORM_FRAME / VK_PKT_UNIFORM_MAX 同状态日志口径。
// 用途：验证"每笔 draw 到达 native 的瞬间，常量是否就是这一笔真正生效的版本"。

int g_uniMode = -1;
int g_uniFrame = -1;
int g_uniMax = 16;
int g_uniUsed = 0;

bool UniWant(uint32_t frameGen)
{
	if (g_uniMode < 0)
	{
		const char* v = getenv("VK_PKT_UNIFORM");
		g_uniMode = (v && v[0] == '1') ? 1 : 0;
		const char* vf = getenv("VK_PKT_UNIFORM_FRAME");
		if (vf && vf[0]) g_uniFrame = atoi(vf);
		const char* vm = getenv("VK_PKT_UNIFORM_MAX");
		if (vm && vm[0]) g_uniMax = atoi(vm);
		if (g_uniMode)
		{
			fprintf(stderr, "[vkpkt] uniform on frame=%d max=%d\n", g_uniFrame, g_uniMax);
		}
	}
	if (!g_uniMode) return false;
	if (g_uniFrame >= 0 && (int)frameGen != g_uniFrame) return false;
	if (g_uniUsed >= g_uniMax) return false;
	g_uniUsed++;
	return true;
}

void PrintUniform(const vkpkt::WirePacket& w)
{
	if (w.program == 0) return;

	const uint8_t* data = nullptr;
	uint32_t bytes = 0;
	if (!gl_shader_uniform_block((int)w.program, &data, &bytes) || !data || !bytes) return;

	uint32_t tag = vkpkt::UniformTag(data, bytes);
	fprintf(stderr, "[vkpkt] u f=%u s=%u prog=%u size=%u tag=%08x\n",
			(unsigned)w.frameGen, (unsigned)w.seq, (unsigned)w.program,
			(unsigned)bytes, (unsigned)tag);
}

// ---------------------------------------------------------------- 帧代 arena + DrawRecord（阶段 ②b）
//
// 只搬运数据，不执行绘制（ExecActive() 仍恒 false）。
// 开关 VK_PKT_RECORD=1；VK_PKT_RECORD_FRAME / VK_PKT_RECORD_MAX 同其他阶段。
//
// 为什么先只做拷贝：arena 是"每 packet 自带副本"的前提。先证明索引/顶点/常量能完整落进
// 帧代缓冲，再接 vkCmdDraw；否则执行路径一开就分不清是数据错了还是命令错了。

int g_recMode = -1;
int g_recFrame = -1;
int g_recMax = 8;
int g_recUsed = 0;

vkpkt::Arena g_vtxArena;
vkpkt::Arena g_idxArena;
vkpkt::Arena g_uniArena;
bool g_arenaReady = false;

void RecResetFrame()
{
	if (!g_arenaReady) return;
	vkpkt::ArenaReset(&g_vtxArena);
	vkpkt::ArenaReset(&g_idxArena);
	vkpkt::ArenaReset(&g_uniArena);
}

bool RecWant(uint32_t frameGen)
{
	if (g_recMode < 0)
	{
		const char* v = getenv("VK_PKT_RECORD");
		g_recMode = (v && v[0] == '1') ? 1 : 0;
		const char* vf = getenv("VK_PKT_RECORD_FRAME");
		if (vf && vf[0]) g_recFrame = atoi(vf);
		const char* vm = getenv("VK_PKT_RECORD_MAX");
		if (vm && vm[0]) g_recMax = atoi(vm);
		if (g_recMode)
		{
			bool ok = vkpkt::ArenaInit(&g_vtxArena, 16u * 1024u * 1024u)
				&& vkpkt::ArenaInit(&g_idxArena, 4u * 1024u * 1024u)
				&& vkpkt::ArenaInit(&g_uniArena, 1u * 1024u * 1024u);
			g_arenaReady = ok;
			fprintf(stderr, "[vkpkt] record on frame=%d max=%d arena=%s\n",
					g_recFrame, g_recMax, ok ? "ok" : "FAILED");
			if (!ok)
			{
				g_recMode = 0;
				return false;
			}
		}
	}
	if (!g_recMode) return false;
	if (g_recFrame >= 0 && (int)frameGen != g_recFrame) return false;
	if (g_recUsed >= g_recMax) return false;
	g_recUsed++;
	return true;
}

struct RecStat {
	uint32_t idxOff = 0;
	uint32_t idxBytes = 0;
	uint32_t vtxOff = 0;
	uint32_t vtxBytes = 0;
	uint32_t uniOff = 0;
	uint32_t uniBytes = 0;
	int primary = -1;
};

void RecIndex(const vkpkt::WirePacket& w, RecStat* st)
{
	if (w.kind != vkpkt::KIND_DRAW_ELEMENTS || w.count <= 0) return;

	const uint8_t* data = nullptr;
	uint32_t bytes = 0;
	if (!SnapshotBuffer((int)w.ibo, &data, &bytes) || !data) return;

	uint64_t start = (uint64_t)w.first * 2u;
	uint64_t need = (uint64_t)w.count * 2u;
	if (start >= bytes || start + need > bytes) return;

	if (!vkpkt::ArenaAllocCopy(&g_idxArena, data + start, (uint32_t)need, 4, &st->idxOff)) return;
	st->idxBytes = (uint32_t)need;
}

void RecVertex(const vkpkt::WirePacket& w, RecStat* st)
{
	if (w.kind == vkpkt::KIND_CLEAR || w.count <= 0) return;

	// 主属性：最低的 enabled 且 size>=2 的槽（与之后执行路径的判定一致）。
	AttrSnap p;
	int slot = -1;
	for (int i = 0; i < 8; i++)
	{
		AttrSnap s;
		if (!SnapshotAttr(i, &s)) break;
		if (s.enabled && s.size >= 2)
		{
			p = s;
			slot = i;
			break;
		}
	}
	if (slot < 0) return;
	st->primary = slot;

	// 先只支持 FLOAT 位置（lime 的顶点数据全是 float32）。
	if (p.glType != 0x1406) return;
	uint32_t elem = (uint32_t)p.size * 4u;
	uint32_t stride = (p.stride > 0) ? (uint32_t)p.stride : elem;

	int64_t minIdx = 0, maxIdx = 0;
	if (w.kind == vkpkt::KIND_DRAW_ARRAYS)
	{
		minIdx = w.first;
		maxIdx = (int64_t)w.first + w.count - 1;
	}
	else
	{
		const uint8_t* idx = nullptr;
		uint32_t ib = 0;
		if (!SnapshotBuffer((int)w.ibo, &idx, &ib) || !idx) return;
		int64_t n = w.count;
		if ((int64_t)w.first + n > (int64_t)(ib / 2)) return;
		uint16_t mn = 0xFFFF, mx = 0;
		for (int64_t k = 0; k < n; k++)
		{
			uint16_t v = 0;
			memcpy(&v, idx + (size_t)(w.first + k) * 2, 2);
			if (v < mn) mn = v;
			if (v > mx) mx = v;
		}
		minIdx = mn;
		maxIdx = mx;
	}

	int64_t start = (int64_t)p.byteOffset + minIdx * (int64_t)stride;
	int64_t bytes = (maxIdx - minIdx) * (int64_t)stride + (int64_t)elem;
	if (start < 0 || bytes <= 0) return;

	const uint8_t* base = nullptr;
	uint32_t baseBytes = 0;
	if (p.client)
	{
		if (!p.clientPtr) return;
		base = (const uint8_t*)p.clientPtr;
	}
	else
	{
		if (!SnapshotBuffer((int)p.bufferName, &base, &baseBytes) || !base) return;
		if ((uint64_t)start >= baseBytes) return;
		if ((uint64_t)(start + bytes) > baseBytes) bytes = (int64_t)baseBytes - start;
	}
	if (bytes <= 0) return;

	if (!vkpkt::ArenaAllocCopy(&g_vtxArena, base + start, (uint32_t)bytes, 4, &st->vtxOff)) return;
	st->vtxBytes = (uint32_t)bytes;
}

void RecUniform(const vkpkt::WirePacket& w, RecStat* st)
{
	if (w.program == 0) return;

	const uint8_t* data = nullptr;
	uint32_t bytes = 0;
	if (!gl_shader_uniform_block((int)w.program, &data, &bytes) || !data || !bytes) return;

	uint32_t n = (bytes > vkpkt::kMaxDrawUniform) ? vkpkt::kMaxDrawUniform : bytes;
	if (!vkpkt::ArenaAllocCopy(&g_uniArena, data, n, 16, &st->uniOff)) return;
	st->uniBytes = n;
}

void PrintRecord(const vkpkt::WirePacket& w, const RecStat& st)
{
	fprintf(stderr,
			"[vkpkt] rec f=%u s=%u kind=%u prog=%u rt=%u range=%d/%d idx=+%u/%u vtx=+%u/%u uni=+%u/%u\n",
			(unsigned)w.frameGen, (unsigned)w.seq, (unsigned)w.kind,
			(unsigned)w.program, (unsigned)w.fbo,
			(int)w.first, (int)w.count,
			(unsigned)st.idxOff, (unsigned)st.idxBytes,
			(unsigned)st.vtxOff, (unsigned)st.vtxBytes,
			(unsigned)st.uniOff, (unsigned)st.uniBytes);
}

// ---------------------------------------------------------------- DrawRecord 物化（阶段 ③a）
//
// 只物化 + 打印，不执行（ExecActive() 仍恒 false）。开关 VK_PKT_MAT=1。
// 目的：证明 attrs/tex/state 三类输入在 emit 瞬间都能抓到，Materialize 输出稳定。
// 下一阶段才把 DrawRecord 交给 vkCmdDraw。

int g_matMode = -1;
int g_matFrame = -1;
int g_matMax = 6;
int g_matUsed = 0;

bool MatWant(uint32_t frameGen)
{
	if (g_matMode < 0)
	{
		const char* v = getenv("VK_PKT_MAT");
		g_matMode = (v && v[0] == '1') ? 1 : 0;
		const char* vf = getenv("VK_PKT_MAT_FRAME");
		if (vf && vf[0]) g_matFrame = atoi(vf);
		const char* vm = getenv("VK_PKT_MAT_MAX");
		if (vm && vm[0]) g_matMax = atoi(vm);
		if (g_matMode)
		{
			fprintf(stderr, "[vkpkt] mat on frame=%d max=%d\n", g_matFrame, g_matMax);
		}
	}
	if (!g_matMode) return false;
	if (g_matFrame >= 0 && (int)frameGen != g_matFrame) return false;
	if (g_matUsed >= g_matMax) return false;
	g_matUsed++;
	return true;
}

void GatherAttrs(vkpkt::AttrInput out[8], uint16_t* attrCount)
{
	for (int i = 0; i < 8; i++)
	{
		vkpkt::AttrInput& d = out[i];
		AttrSnap s;
		if (!SnapshotAttr(i, &s))
		{
			d.kind = vkpkt::ATTR_DISABLED;
			continue;
		}
		d.bufferName = s.bufferName;
		d.byteOffset = s.byteOffset;
		d.stride = (uint16_t)(s.stride > 0 ? s.stride : 0);
		d.size = (uint8_t)(s.size > 0 ? s.size : 0);
		d.glType = (uint32_t)(s.glType < 0 ? 0 : s.glType);
		d.normalized = 0;
		if (!s.enabled)
		{
			d.kind = s.constSet ? vkpkt::ATTR_CONSTANT : vkpkt::ATTR_DISABLED;
		}
		else if (s.client)
		{
			d.kind = vkpkt::ATTR_CLIENT;
		}
		else
		{
			d.kind = vkpkt::ATTR_ARRAY;
		}
		if (s.constSet)
		{
			for (int k = 0; k < 4; k++) d.constValue[k] = s.constValue[k];
		}
	}
	uint16_t count = 0;
	for (int i = 0; i < 8; i++)
	{
		if (out[i].kind != vkpkt::ATTR_DISABLED) count = (uint16_t)(i + 1);
	}
	*attrCount = count;
}

void GatherTex(int program, vkpkt::TexInput out[8], uint16_t* slots)
{
	for (int i = 0; i < 8; i++)
	{
		memset(&out[i], 0, sizeof(out[i]));
		out[i].unit = vkpkt::kTexSlotUnused;
	}
	if (program <= 0)
	{
		*slots = 0;
		return;
	}
	int n = gl_shader_sampler_count(program);
	if (n > 8) n = 8;
	for (int i = 0; i < n; i++)
	{
		int unit = gl_shader_sampler_unit(program, i);
		if (unit < 0 || unit > 7)
		{
			out[i].unit = vkpkt::kTexSlotUnused;
			continue;
		}
		int name = SnapshotTexBound(unit);
		out[i].unit = (uint32_t)unit;
		out[i].texName = (uint32_t)(name > 0 ? name : 0);
		out[i].imageId = out[i].texName ? SnapshotTexVk(name) : 0;
		out[i].samplerId = 0;
	}
	*slots = (uint16_t)n;
}

void GatherState(vkpkt::StateInput* s)
{
	memset(s, 0, sizeof(*s));

	s->blendEnabled = glstate::IsEnabled(0x0BE2) ? 1 : 0;
	s->depthTestEnabled = glstate::IsEnabled(0x0B71) ? 1 : 0;
	s->stencilEnabled = glstate::IsEnabled(0x0B90) ? 1 : 0;
	s->scissorEnabled = glstate::IsEnabled(0x0C11) ? 1 : 0;

	if (glstate::IsEnabled(0x0B44))
	{
		int mode = Q1(0x0B45, 0x0405);
		s->cullMode = (mode == 0x0404) ? 2 : (mode == 0x0408) ? 3 : 1;
	}
	else
	{
		s->cullMode = 0;
	}

	s->frontFace = (Q1(0x0B46, 0x0901) == 0x0900) ? 1 : 0;
	s->depthFunc = (uint16_t)Q1(0x0B74, 0x0201);
	s->depthMask = Q1(0x0B72, 1) ? 1 : 0;

	int cm[4] = {1, 1, 1, 1};
	glstate::GetColorMask(cm);
	s->colorMaskBits = (uint8_t)((cm[0] ? 1 : 0) | (cm[1] ? 2 : 0) | (cm[2] ? 4 : 0) | (cm[3] ? 8 : 0));

	s->blendSrcRgb = (uint16_t)Q1(0x80C9, 0x0001);
	s->blendDstRgb = (uint16_t)Q1(0x80C8, 0x0000);
	s->blendSrcAlpha = (uint16_t)Q1(0x80CB, 0x0001);
	s->blendDstAlpha = (uint16_t)Q1(0x80CA, 0x0000);
	s->blendEqRgb = (uint16_t)Q1(0x8009, 0x8006);
	s->blendEqAlpha = (uint16_t)Q1(0x883D, 0x8006);

	int sc[4] = {0, 0, 0, 0};
	glstate::GetScissorBox(sc);
	s->scissorX = (int16_t)sc[0];
	s->scissorY = (int16_t)sc[1];
	s->scissorW = (int16_t)sc[2];
	s->scissorH = (int16_t)sc[3];

	int vp[4] = {0, 0, 0, 0};
	glstate::GetViewportBox(vp);
	s->vpX = (int16_t)vp[0];
	s->vpY = (int16_t)vp[1];
	s->vpW = (int16_t)vp[2];
	s->vpH = (int16_t)vp[3];

	float cc[4] = {0, 0, 0, 0};
	glstate::GetClearColor(cc);
	for (int i = 0; i < 4; i++) s->clearColor[i] = cc[i];
	s->clearDepth = glstate::GetClearDepth();
	s->clearStencil = glstate::GetClearStencil();

	s->positionScaleY = 0;
	s->yFlip = glstate::GetClipSpaceYFlip() ? 1 : 0;
}

void PrintMat(const vkpkt::DrawRecord& r, const vkpkt::StateInput& st)
{
	unsigned mask = 0;
	for (int i = 0; i < 8; i++)
	{
		if (r.attrs[i].kind != vkpkt::ATTR_DISABLED) mask |= (1u << i);
	}
	int texSlots = 0;
	unsigned firstTex = 0;
	for (int i = 0; i < 8; i++)
	{
		if (r.tex[i].unit == vkpkt::kTexSlotUnused) continue;
		if (texSlots == 0) firstTex = r.tex[i].texName;
		texSlots++;
	}

	fprintf(stderr,
			"[vkpkt] mat f=%u s=%u prog=%u rt=%u attrs=%u mask=%02x tex=%d/%u "
			"blend=%u(%u,%u,%u) depth=%u/%u/%u stencil=%u cull=%u sc=%d,%d,%d,%d vp=%d,%d,%d,%d cm=%u\n",
			(unsigned)r.frameGen, (unsigned)r.seq, (unsigned)r.program, (unsigned)r.fbo,
			(unsigned)r.attrCount, mask, texSlots, firstTex,
			(unsigned)r.blendEnabled, (unsigned)r.blendSrcRgb, (unsigned)r.blendDstRgb, (unsigned)r.blendEqRgb,
			(unsigned)r.depthTestEnabled, (unsigned)r.depthMask, (unsigned)r.depthFunc,
			(unsigned)r.stencilEnabled, (unsigned)r.cullMode,
			(int)r.scissorX, (int)r.scissorY, (int)r.scissorW, (int)r.scissorH,
			(int)r.vpX, (int)r.vpY, (int)r.vpW, (int)r.vpH,
			(unsigned)r.colorMaskBits);
	(void)st;
}

// ---------------------------------------------------------------- 顶点解包 dry-run（阶段 ③b1）
//
// 只读 + 只打印，不执行。开关 VK_PKT_DRY=1。
// 验证"从属性槽到 pos/uv/alpha 的取值语义"（lime 的 loc0=Position、loc4=TexCoord、
// loc5=逐顶点 alpha），这一层错了，之后接 vkCmdDraw 必然花屏。

int g_dryMode = -1;
int g_dryFrame = -1;
int g_dryMax = 8;
int g_dryUsed = 0;

bool DryWant(uint32_t frameGen)
{
	if (g_dryMode < 0)
	{
		const char* v = getenv("VK_PKT_DRY");
		g_dryMode = (v && v[0] == '1') ? 1 : 0;
		const char* vf = getenv("VK_PKT_DRY_FRAME");
		if (vf && vf[0]) g_dryFrame = atoi(vf);
		const char* vm = getenv("VK_PKT_DRY_MAX");
		if (vm && vm[0]) g_dryMax = atoi(vm);
		if (g_dryMode)
		{
			fprintf(stderr, "[vkpkt] dry on frame=%d max=%d\n", g_dryFrame, g_dryMax);
		}
	}
	if (!g_dryMode) return false;
	if (g_dryFrame >= 0 && (int)frameGen != g_dryFrame) return false;
	if (g_dryUsed >= g_dryMax) return false;
	g_dryUsed++;
	return true;
}

struct DryAttr {
	const uint8_t* base = nullptr;
	uint32_t bytes = 0;
	uint32_t stride = 0;
	uint32_t off = 0;
	uint32_t elem = 0;
	uint32_t size = 0;
	int32_t type = 0;
	bool ok = false;
};

bool ResolveDry(int slot, DryAttr* out)
{
	AttrSnap s;
	if (!SnapshotAttr(slot, &s) || !s.enabled) return false;
	out->type = s.glType;
	out->size = (uint32_t)(s.size > 0 ? s.size : 0);
	out->off = s.byteOffset;
	out->elem = out->size * 4u; // FLOAT 假定，下面校验
	if (s.glType != 0x1406 || out->size == 0) return false;
	out->stride = (uint32_t)(s.stride > 0 ? s.stride : (int)out->elem);
	if (s.client)
	{
		if (!s.clientPtr) return false;
		out->base = (const uint8_t*)s.clientPtr;
		out->bytes = 0xFFFFFFFFu;
	}
	else
	{
		const uint8_t* p = nullptr;
		uint32_t n = 0;
		if (!SnapshotBuffer((int)s.bufferName, &p, &n) || !p) return false;
		out->base = p;
		out->bytes = n;
	}
	out->ok = true;
	return true;
}

bool ReadFloatAt(const DryAttr& a, int64_t vidx, int comp, float* out)
{
	if (!a.ok || comp < 0 || (uint32_t)comp >= a.size) return false;
	int64_t pos = (int64_t)a.off + vidx * (int64_t)a.stride + (int64_t)comp * 4;
	if (pos < 0) return false;
	if (a.bytes != 0xFFFFFFFFu && (uint64_t)pos + 4u > a.bytes) return false;
	memcpy(out, a.base + pos, 4);
	return true;
}

void PrintDry(const vkpkt::WirePacket& w)
{
	if (w.kind == vkpkt::KIND_CLEAR || w.count <= 0) return;

	// 位置：最低的 enabled 且 size>=2 的槽
	int posSlot = -1, uvSlot = -1, alphaSlot = -1;
	for (int i = 0; i < 8; i++)
	{
		AttrSnap s;
		if (!SnapshotAttr(i, &s)) break;
		if (s.enabled && s.size >= 2)
		{
			posSlot = i;
			break;
		}
	}
	// lime 约定：uv 在 loc4（退而求其次 loc1），alpha 在 loc5
	for (int i = 4; i >= 1; i -= 3)
	{
		AttrSnap s;
		if (!SnapshotAttr(i, &s)) continue;
		if (s.enabled && s.size >= 2)
		{
			uvSlot = i;
			break;
		}
	}
	{
		AttrSnap s;
		if (SnapshotAttr(5, &s) && s.enabled && s.size >= 1) alphaSlot = 5;
	}

	DryAttr pos, uv, alpha;
	if (posSlot < 0 || !ResolveDry(posSlot, &pos))
	{
		fprintf(stderr, "[vkpkt] dry f=%u s=%u prog=%u err=no_pos\n",
				(unsigned)w.frameGen, (unsigned)w.seq, (unsigned)w.program);
		return;
	}
	bool hasUv = (uvSlot >= 0) && ResolveDry(uvSlot, &uv);
	bool hasA = (alphaSlot >= 0) && ResolveDry(alphaSlot, &alpha);

	// 索引展开
	const uint8_t* idx = nullptr;
	uint32_t ib = 0;
	if (w.kind == vkpkt::KIND_DRAW_ELEMENTS)
	{
		if (!SnapshotBuffer((int)w.ibo, &idx, &ib) || !idx) return;
		if ((int64_t)w.first + w.count > (int64_t)(ib / 2)) return;
	}

	float x0 = 0, x1 = 0, y0 = 0, y1 = 0;
	float u0 = 0, u1 = 0, v0 = 0, v1 = 0;
	float a0 = 1, a1 = 1;
	bool first = true, firstUv = true, firstA = true;

	for (int i = 0; i < w.count; i++)
	{
		int64_t vidx = 0;
		if (idx)
		{
			uint16_t v = 0;
			memcpy(&v, idx + (size_t)(w.first + i) * 2, 2);
			vidx = v;
		}
		else
		{
			vidx = (int64_t)w.first + i;
		}
		float x = 0, y = 0;
		if (!ReadFloatAt(pos, vidx, 0, &x) || !ReadFloatAt(pos, vidx, 1, &y)) continue;
		if (first) { x0 = x1 = x; y0 = y1 = y; first = false; }
		if (x < x0) x0 = x;
		if (x > x1) x1 = x;
		if (y < y0) y0 = y;
		if (y > y1) y1 = y;

		if (hasUv)
		{
			float u = 0, vv = 0;
			ReadFloatAt(uv, vidx, 0, &u);
			ReadFloatAt(uv, vidx, 1, &vv);
			if (firstUv) { u0 = u1 = u; v0 = v1 = vv; firstUv = false; }
			if (u < u0) u0 = u;
			if (u > u1) u1 = u;
			if (vv < v0) v0 = vv;
			if (vv > v1) v1 = vv;
		}
		if (hasA)
		{
			float a = 1;
			ReadFloatAt(alpha, vidx, 0, &a);
			if (firstA) { a0 = a1 = a; firstA = false; }
			if (a < a0) a0 = a;
			if (a > a1) a1 = a;
		}
	}
	if (first) return;

	fprintf(stderr,
			"[vkpkt] dry f=%u s=%u prog=%u n=%d pos=[%.1f..%.1f,%.1f..%.1f] "
			"uv=[%.3f..%.3f,%.3f..%.3f] a=[%.3f..%.3f] slots=%d,%d,%d\n",
			(unsigned)w.frameGen, (unsigned)w.seq, (unsigned)w.program, (int)w.count,
			x0, x1, y0, y1, u0, u1, v0, v1, a0, a1,
			posSlot, uvSlot, alphaSlot);
}

// ---------------------------------------------------------------- packet 执行（阶段 ③b2）
//
// VK_PKT_EXEC=1 + VK_PKT=1 时，GL 侧跳过 draw（gl_core/gl_fbo 已有 ExecActive 分支），
// 由这里把每笔 packet 解包成 PktDrawDesc 交给渲染器。默认全关。
//
// 顶点布局与批处理路径完全相同（15 floats：pos4、alpha、mult4、off4、uv2）。

uint8_t PktBlend(int gl)
{
	switch (gl)
	{
		case 0x0001: return 0; // ONE
		case 0x0000: return 1; // ZERO
		case 0x0302: return 2; // SRC_ALPHA
		case 0x0303: return 3; // ONE_MINUS_SRC_ALPHA
		case 0x0304: return 4; // DST_ALPHA
		case 0x0305: return 5; // ONE_MINUS_DST_ALPHA
		case 0x0300: return 6; // SRC_COLOR
		case 0x0301: return 7; // ONE_MINUS_SRC_COLOR
		case 0x0306: return 8; // DST_COLOR
		case 0x0307: return 9; // ONE_MINUS_DST_COLOR
		case 0x8003: return 10; // CONST_ALPHA
		case 0x8004: return 11; // ONE_MINUS_CONST_ALPHA
		default: return 0xFF;
	}
}

uint8_t PktMinFilter(int gl)
{
	switch (gl)
	{
		case 0x2600: return 0; // NEAREST
		case 0x2601: return 1; // LINEAR
		case 0x2700: return 2;
		case 0x2701: return 3;
		case 0x2702: return 4;
		case 0x2703: return 5;
		default: return 0xFF;
	}
}

uint8_t PktMagFilter(int gl)
{
	if (gl == 0x2601) return 1;
	if (gl == 0x2600) return 0;
	return 0xFF;
}

uint8_t PktWrap(int gl)
{
	switch (gl)
	{
		case 0x812F: return 0; // CLAMP_TO_EDGE
		case 0x2901: return 1; // REPEAT
		case 0x8370: return 2; // MIRRORED_REPEAT
		case 0x2900: return 0; // CLAMP
		default: return 0xFF;
	}
}

bool ExecuteDraw(const vkpkt::WirePacket& w)
{
	VkRenderer* r = gl_vk_get_renderer();
	if (!r) return false;

	uint32_t rtImage = gl_vk_fbo_target((int)w.fbo);

	// clear 行：clear 是事件，颜色以包内 clearColor（RGBA8，R 在低字节）为准；
	// GL 状态只在退化的非 clear 包上兜底（与 vk_packet.cpp 的物化口径一致）。
	if (w.count <= 0)
	{
		float cc[4] = {0, 0, 0, 0};
		if (w.kind == vkpkt::KIND_CLEAR)
		{
			const uint32_t c = w.clearColor;
			cc[0] = (float)((c >> 0) & 0xFF) / 255.0f;
			cc[1] = (float)((c >> 8) & 0xFF) / 255.0f;
			cc[2] = (float)((c >> 16) & 0xFF) / 255.0f;
			cc[3] = (float)((c >> 24) & 0xFF) / 255.0f;
		}
		else
		{
			glstate::GetClearColor(cc);
		}
		bool colorClear = (w.clearMask & vkpkt::CLEAR_COLOR) != 0;
		return r->PktBeginTarget(rtImage, cc[0], cc[1], cc[2], cc[3], colorClear, w.clearMask);
	}
	if (w.kind != vkpkt::KIND_DRAW_ELEMENTS && w.kind != vkpkt::KIND_DRAW_ARRAYS) return false;

	// 采样源：优先与批处理 DrawRaw 同口径 —— 当前 active unit 上的绑定
	// （g_texBound[g_activeUnit]）。按 program 声明的 sampler unit 取会撞上
	// 该 unit 的残留绑定（菜单砖墙被 TV 噪声顶替即此因）。
	int texName = 0;
	uint32_t vkTex = 0;
	// 1) 包内自带的主纹理名（新 exe）：发射那一刻的记录，不受执行时 GL 残余状态影响。
	if (w.texName > 0)
	{
		uint32_t id = SnapshotTexVk((int)w.texName);
		if (id)
		{
			texName = (int)w.texName;
			vkTex = id;
			static int s_wireTrace = -1;
			if (s_wireTrace < 0)
			{
				const char* v = getenv("VK_PKT_TRACE");
				s_wireTrace = (v && v[0] == '1') ? 1 : 0;
			}
			if (s_wireTrace)
			{
				fprintf(stderr, "[vkexec] wire prog=%u rt=%u gl=%u vk=%u n=%d\n",
						(unsigned)w.program, rtImage, (unsigned)w.texName, (unsigned)id, (int)w.count);
			}
		}
	}
	// 2) 老 exe（wire.texName=0）兜底：与批处理同口径的 active unit 绑定
	{
		int activeName = vkTex ? 0 : SnapshotTexBoundActive();
		if (activeName > 0)
		{
			uint32_t id = SnapshotTexVk(activeName);
			if (id)
			{
				texName = activeName;
				vkTex = id;
				static int s_actTrace = -1;
				if (s_actTrace < 0)
				{
					const char* v = getenv("VK_PKT_TRACE");
					s_actTrace = (v && v[0] == '1') ? 1 : 0;
				}
				if (s_actTrace)
				{
					fprintf(stderr, "[vkexec] active prog=%u rt=%u gl=%d vk=%u n=%d\n",
							(unsigned)w.program, rtImage, activeName, (unsigned)id, (int)w.count);
				}
			}
		}
	}
	int samplerCount = (w.program > 0) ? gl_shader_sampler_count((int)w.program) : 0;
	for (int i = 0; !vkTex && i < samplerCount && i < 8; i++)
	{
		int unit = gl_shader_sampler_unit((int)w.program, i);
		if (unit < 0 || unit > 7) continue;
		int name = SnapshotTexBound(unit);
		if (name <= 0) continue;
		uint32_t id = SnapshotTexVk(name);
		// 诊断（VK_PKT_TRACE=1）：记录"执行这一刻"真实解析到的 GL 纹理名/Vk id，
		// 用来和批处理 [bbtrace] 的 tex 对比，定位合成取错纹理的问题。
		static int s_texTrace = -1;
		if (s_texTrace < 0)
		{
			const char* v = getenv("VK_PKT_TRACE");
			s_texTrace = (v && v[0] == '1') ? 1 : 0;
		}
		if (s_texTrace)
		{
			fprintf(stderr, "[vkexec] slot=%d prog=%u rt=%u unit=%d gl=%d vk=%u n=%d\n",
					i, (unsigned)w.program, rtImage, unit, name, (unsigned)id, (int)w.count);
		}
		if (id)
		{
			texName = name;
			vkTex = id;
			break;
		}
	}
	if (!vkTex) return false;

	// 属性槽：loc0=Position、loc4=TexCoord、loc5=逐顶点 alpha
	int posSlot = -1, uvSlot = -1, alphaSlot = -1;
	for (int i = 0; i < 8; i++)
	{
		AttrSnap s;
		if (!SnapshotAttr(i, &s)) break;
		if (s.enabled && s.size >= 2)
		{
			posSlot = i;
			break;
		}
	}
	for (int i = 4; i >= 1; i -= 3)
	{
		AttrSnap s;
		if (!SnapshotAttr(i, &s)) continue;
		if (s.enabled && s.size >= 2)
		{
			uvSlot = i;
			break;
		}
	}
	{
		AttrSnap s;
		if (SnapshotAttr(5, &s) && s.enabled && s.size >= 1) alphaSlot = 5;
	}
	if (posSlot < 0) return false;

	DryAttr pos, uv, alpha;
	if (!ResolveDry(posSlot, &pos)) return false;
	bool hasUv = (uvSlot >= 0) && ResolveDry(uvSlot, &uv);
	bool hasA = (alphaSlot >= 0) && ResolveDry(alphaSlot, &alpha);

	const uint8_t* idx = nullptr;
	uint32_t ib = 0;
	if (w.kind == vkpkt::KIND_DRAW_ELEMENTS)
	{
		if (!SnapshotBuffer((int)w.ibo, &idx, &ib) || !idx) return false;
		if ((int64_t)w.first + w.count > (int64_t)(ib / 2)) return false;
	}

	// 颜色乘数/偏移：与批处理 BuildVertexColor 同口径 —— 数组在 loc6/loc7。
	// 旧实现读 loc2/loc3，但 2D 程序里这两个槽通常是未使用常量 0，会把整笔乘成黑色
	// （主菜单砖墙 col=[1,0,0,1] 来自 loc6 数组，packet 侧曾因此全黑）。
	AttrSnap s6{}, s7{};
	bool ok6 = SnapshotAttr(6, &s6) != 0;
	bool ok7 = SnapshotAttr(7, &s7) != 0;
	bool mulArr = ok6 && s6.enabled && s6.size >= 4;
	bool offArr = ok7 && s7.enabled && s7.size >= 4;
	DryAttr cmul, coff;
	if (mulArr) mulArr = ResolveDry(6, &cmul);
	if (offArr) offArr = ResolveDry(7, &coff);
	float mult[4] = {1, 1, 1, 1};
	float off[4] = {0, 0, 0, 0};
	if (ok6 && !mulArr && s6.constSet)
		for (int k = 0; k < 4; k++) mult[k] = s6.constValue[k];
	if (ok7 && !offArr && s7.constSet)
		for (int k = 0; k < 4; k++) off[k] = s7.constValue[k];
	if (!mulArr)
	{
		bool zero = !ok6 || !s6.constSet ||
			(mult[0] == 0.0f && mult[1] == 0.0f && mult[2] == 0.0f);
		if (zero) for (int k = 0; k < 4; k++) mult[k] = 1.0f;
	}
	if (!offArr)
	{
		bool zero = !ok7 || !s7.constSet ||
			(off[0] == 0.0f && off[1] == 0.0f && off[2] == 0.0f);
		if (zero) for (int k = 0; k < 4; k++) off[k] = 0.0f;
	}
	float alphaConst = 1.0f;
	if (!hasA)
	{
		AttrSnap s;
		if (SnapshotAttr(1, &s) && !s.enabled && s.constSet) alphaConst *= s.constValue[0];
		// loc5 只有在该 program 真的声明了它时才参与乘法。否则那是未使用槽位的默认常量
		// （constSet=1、值 0），会把 alpha 乘成 0：prog=1 上屏整屏黑、prog=7 噪点消失
		// 都是这个原因（prog=1 的顶点着色器只声明了 loc1 的 openfl_Alpha）。
		if (w.program > 0 && gl_shader_program_has_attribute((int)w.program, 5)
			&& SnapshotAttr(5, &s) && !s.enabled && s.constSet)
			alphaConst *= s.constValue[0];
	}

	// 诊断：每个 program 的 alpha 来源（hasA=顶点属性 / alphaConst=常量）。
	// 上屏（prog=1）用 program 管线时整屏黑，就是这里取到 0 导致的。
	{
		static signed char s_aDbg[256] = {0};
		if (w.program > 0 && w.program < 256 && !s_aDbg[w.program])
		{
			s_aDbg[w.program] = 1;
			AttrSnap s1, s5;
			bool ok1 = SnapshotAttr(1, &s1) != 0;
			bool ok5 = SnapshotAttr(5, &s5) != 0;
			fprintf(stderr,
					"[pktalpha] prog=%u hasA=%d slot=%d alphaConst=%.3f "
					"loc1(ok=%d en=%d set=%d v=%.3f) loc5(ok=%d en=%d set=%d v=%.3f)\n",
					(unsigned)w.program, hasA ? 1 : 0, alphaSlot, alphaConst,
					ok1 ? 1 : 0, ok1 ? (s1.enabled ? 1 : 0) : -1, ok1 ? (s1.constSet ? 1 : 0) : -1,
					ok1 ? s1.constValue[0] : 0.0f,
					ok5 ? 1 : 0, ok5 ? (s5.enabled ? 1 : 0) : -1, ok5 ? (s5.constSet ? 1 : 0) : -1,
					ok5 ? s5.constValue[0] : 0.0f);
		}
	}

	std::vector<float> px((size_t)w.count), py((size_t)w.count);
	std::vector<float> pu, pv, pa;
	std::vector<float> pmul, poff;
	if (mulArr) pmul.resize((size_t)w.count * 4);
	if (offArr) poff.resize((size_t)w.count * 4);
	if (hasUv)
	{
		pu.resize((size_t)w.count);
		pv.resize((size_t)w.count);
	}
	if (hasA) pa.resize((size_t)w.count);

	float uMin = 0, uMax = 0, vMin = 0, vMax = 0;
	bool firstUv = true;
	for (int i = 0; i < w.count; i++)
	{
		int64_t vidx = 0;
		if (idx)
		{
			uint16_t v = 0;
			memcpy(&v, idx + (size_t)(w.first + i) * 2, 2);
			vidx = v;
		}
		else
		{
			vidx = (int64_t)w.first + i;
		}
		float x = 0, y = 0;
		if (!ReadFloatAt(pos, vidx, 0, &x) || !ReadFloatAt(pos, vidx, 1, &y)) return false;
		px[(size_t)i] = x;
		py[(size_t)i] = y;
		if (hasUv)
		{
			float u = 0, vv = 0;
			ReadFloatAt(uv, vidx, 0, &u);
			ReadFloatAt(uv, vidx, 1, &vv);
			pu[(size_t)i] = u;
			pv[(size_t)i] = vv;
			if (firstUv)
			{
				uMin = uMax = u;
				vMin = vMax = vv;
				firstUv = false;
			}
			if (u < uMin) uMin = u;
			if (u > uMax) uMax = u;
			if (vv < vMin) vMin = vv;
			if (vv > vMax) vMax = vv;
		}
		if (hasA)
		{
			float a = 1;
			ReadFloatAt(alpha, vidx, 0, &a);
			pa[(size_t)i] = a;
		}
		if (mulArr)
		{
			for (int k = 0; k < 4; k++)
				ReadFloatAt(cmul, vidx, k, &pmul[(size_t)i * 4 + k]);
		}
		if (offArr)
		{
			for (int k = 0; k < 4; k++)
				ReadFloatAt(coff, vidx, k, &poff[(size_t)i * 4 + k]);
		}
	}

	int tw = 0, th = 0;
	if (vkTex >= 1000) r->GetRenderTargetSize(vkTex, &tw, &th);
	else if (texName > 0) gl_vk_texture_size(texName, &tw, &th);
	bool pixelUv = hasUv && (uMax > 1.001f || uMin < -1.001f || vMax > 1.001f || vMin < -1.001f);

	std::vector<float> packed((size_t)w.count * 15);
	for (int i = 0; i < w.count; i++)
	{
		float u = hasUv ? pu[(size_t)i] : 0.0f;
		float v = hasUv ? pv[(size_t)i] : 0.0f;
		if (pixelUv)
		{
			float eu = u, ev = v;
			if (u <= uMin + 1e-3f && u < uMax) eu = u + 0.5f;
			else if (u >= uMax - 1e-3f) eu = u - 0.5f;
			if (v <= vMin + 1e-3f && v < vMax) ev = v + 0.5f;
			else if (v >= vMax - 1e-3f) ev = v - 0.5f;
			u = tw > 0 ? eu / tw : 1.0f;
			v = th > 0 ? ev / th : 1.0f;
		}
		float* o = packed.data() + (size_t)i * 15;
		o[0] = px[(size_t)i];
		o[1] = py[(size_t)i];
		o[2] = 0.0f;
		o[3] = 1.0f;
		o[4] = hasA ? pa[(size_t)i] : alphaConst;
		for (int k = 0; k < 4; k++) o[5 + k] = mulArr ? pmul[(size_t)i * 4 + k] : mult[k];
		for (int k = 0; k < 4; k++) o[9 + k] = offArr ? poff[(size_t)i * 4 + k] : off[k];
		o[13] = u;
		o[14] = v;
	}

	std::vector<uint16_t> seq;
	std::vector<uint32_t> seq32;
	bool indexed = (w.kind == vkpkt::KIND_DRAW_ELEMENTS);
	bool use32 = false;
	if (indexed)
	{
		if (w.count <= 65535)
		{
			seq.resize((size_t)w.count);
			for (int i = 0; i < w.count; i++) seq[(size_t)i] = (uint16_t)i;
		}
		else
		{
			seq32.resize((size_t)w.count);
			for (int i = 0; i < w.count; i++) seq32[(size_t)i] = (uint32_t)i;
			use32 = true;
		}
	}

	PktDrawDesc d{};
	d.texId = vkTex;
	d.verts = packed.data();
	d.vertexCount = w.count;
	d.indices = seq.empty() ? nullptr : seq.data();
	d.indices32 = seq32.empty() ? nullptr : seq32.data();
	d.indexCount = (int)(seq.size() ? seq.size() : seq32.size());
	d.index32 = use32 ? 1 : 0;
	// 游戏 program + 该笔完整常量块（program 管线只有拿到它才会执行自定义 GLSL）
	{
		const uint8_t* uni = nullptr;
		uint32_t uniSize = 0;
		if (w.program > 0) gl_shader_uniform_block((int)w.program, &uni, &uniSize);
		d.program = (int)w.program;
		d.uniform = uni;
		d.uniformSize = uniSize;
	}

	vkpkt::StateInput st;
	GatherState(&st);
	d.state.blendEnabled = st.blendEnabled;
	d.state.blendSrc = PktBlend(st.blendSrcRgb);
	d.state.blendDst = PktBlend(st.blendDstRgb);
	d.state.depthTest = st.depthTestEnabled;
	d.state.depthWrite = st.depthMask;
	d.state.stencilTest = 0;
	d.state.cullMode = st.cullMode;
	d.state.frontFace = st.frontFace;
	d.state.scissorTest = st.scissorEnabled;
	d.state.colorMask = st.colorMaskBits;

	int fMin = 0, fMag = 0, wS = 0, wT = 0;
	if (texName > 0 && gl_vk_texture_sampler_state(texName, &fMin, &fMag, &wS, &wT))
	{
		d.sampler.minFilter = PktMinFilter(fMin);
		d.sampler.magFilter = PktMagFilter(fMag);
		d.sampler.wrapU = PktWrap(wS);
		d.sampler.wrapV = PktWrap(wT);
	}
	else
	{
		d.sampler.minFilter = 0xFF;
		d.sampler.magFilter = 0xFF;
		d.sampler.wrapU = 0xFF;
		d.sampler.wrapV = 0xFF;
	}

	d.viewport.valid = (st.vpW > 0 && st.vpH > 0) ? 1 : 0;
	d.viewport.x = st.vpX;
	d.viewport.y = st.vpY;
	d.viewport.w = st.vpW;
	d.viewport.h = st.vpH;
	d.scissor.valid = (st.scissorEnabled && st.scissorW > 0 && st.scissorH > 0) ? 1 : 0;
	d.scissor.x = st.scissorX;
	d.scissor.y = st.scissorY;
	d.scissor.w = st.scissorW;
	d.scissor.h = st.scissorH;

	if (!r->PktBeginTarget(rtImage, 0, 0, 0, 0, false, 0)) return false;
	return r->PktDraw(d);
}

} // namespace

// 执行入口：从原始 64B 再解析一次（只在 VK_PKT_EXEC=1 时走，重复解析可忽略）。
bool ExecPacketRaw(const uint8_t* bytes, uint32_t nbytes)
{
	vkpkt::WirePacket w;
	char err[48];
	if (!vkpkt::ParseWire(bytes, nbytes, &w, err, sizeof(err))) return false;
	return ExecuteDraw(w);
}

// GL 侧据此跳过自己的绘制；默认关，只有 VK_PKT_EXEC=1 且 VK_PKT 打开时才接管。
bool ExecActive()
{
	if (g_l.exec < 0)
	{
		const char* v = getenv("VK_PKT_EXEC");
		g_l.exec = (v && v[0] == '1') ? 1 : 0;
	}
	return g_l.exec == 1 && lime_vk_pkt_mode() == 1;
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
	g_uniUsed = 0;
	g_recUsed = 0;
	g_matUsed = 0;
	g_dryUsed = 0;
	RecResetFrame();
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
		g_uniUsed = 0;
		g_recUsed = 0;
		g_matUsed = 0;
		g_dryUsed = 0;
		RecResetFrame();
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
	// 阶段 ②a：只读 uniform 摘要（VK_PKT_UNIFORM=1 才生效）
	if (UniWant(w.frameGen)) PrintUniform(w);
	// 阶段 ②b：索引/顶点/常量拷进帧代 arena（VK_PKT_RECORD=1 才生效，不执行）
	if (RecWant(w.frameGen))
	{
		RecStat st;
		RecIndex(w, &st);
		RecVertex(w, &st);
		RecUniform(w, &st);
		PrintRecord(w, st);
	}
	// 阶段 ③a：物化 DrawRecord 全字段（VK_PKT_MAT=1 才生效，不执行）
	if (MatWant(w.frameGen))
	{
		vkpkt::MaterializeInput in;
		memset(&in, 0, sizeof(in));
		in.wire = w;
		GatherAttrs(in.attrs, &in.attrCount);
		uint16_t slots = 0;
		GatherTex((int)w.program, in.tex, &slots);
		GatherState(&in.state);
		vkpkt::DrawRecord rec;
		vkpkt::Materialize(in, &rec);
		PrintMat(rec, in.state);
	}

	// 阶段 ③b1：顶点解包 dry-run（VK_PKT_DRY=1 才生效，不执行）
	if (DryWant(w.frameGen)) PrintDry(w);

	// 阶段 ③b2：packet 执行（VK_PKT_EXEC=1 才生效；GL 侧同时跳过自己的绘制）
	if (ExecActive()) ExecPacketRaw(src, (uint32_t)bytes);

	return 1;
}

} // namespace lime
