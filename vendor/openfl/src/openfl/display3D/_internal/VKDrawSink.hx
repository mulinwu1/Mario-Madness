package openfl.display3D._internal;

import haxe.io.Bytes;

/**
	`[vkpkt]` 记录侧写手（v2 重写，最小版）。

	只做一件事：把一笔 draw/clear 写成 64B WirePacket（布局见 docs/draw_packet_spec.md §3），
	交给 native 记账。**不做**日志行、不算 sha256、不抓 GL 状态 —— 这些在后续阶段按需加回。

	为什么砍掉：旧版每笔 draw 都要构造字符串、拼数组、算 Sha256，且这些工作在
	记录开关关闭时也照做。重写后的关闭路径是：`emit()` 第一行读一次缓存 Bool 就返回。
**/
class VKDrawSink
{
	public static inline var PACKET_BYTES:Int = 64;

	/** 帧号：present() 里 ++，首帧为 1。 */
	public static var frameGen:Int = 1;

	/** 帧内 packet 序号：每次 emit 从 1 递增，帧末归零。 */
	public static var seq:Int = 0;

	/** 复用的 packet 缓冲（spec §2：逐 draw 覆盖写，不分配）。 **/
	static var __packet:Bytes = Bytes.alloc(PACKET_BYTES);

	/** 复用的快照对象（只做字段赋值，不 new）。 **/
	static var __snap:VKDrawSnapshot = new VKDrawSnapshot();

	/** Context3D 取快照用；同一笔同步用完即发。 **/
	public static function snapshot():VKDrawSnapshot
	{
		return __snap;
	}

	/** 记录器是否在工作（转调 native 开关判定）。 **/
	public static inline function active():Bool
	{
		return VKPktNative.recording();
	}

	public static function emit(s:VKDrawSnapshot):Void
	{
		if (!VKPktNative.recording()) return;

		seq++;
		writePacket(s, seq, frameGen);
		VKPktNative.emit(__packet);
	}

	public static function endFrame(animTime:Float):Void
	{
		if (!VKPktNative.recording()) return;

		VKPktNative.frame(frameGen, animTime);
		seq = 0;
	}

	// ------------------------------------------------------------------
	// WirePacket（spec §3，小端）
	// ------------------------------------------------------------------

	static function writePacket(s:VKDrawSnapshot, seqValue:Int, frame:Int):Void
	{
		var b = __packet;
		b.setInt32(0, seqValue); // u32 seq
		b.setUInt16(4, s.kind); // u16 kind
		b.setUInt16(6, s.flags); // u16 flags
		b.setInt32(8, frame); // u32 frameGen
		b.setInt32(12, s.program); // u32 program
		b.setInt32(16, s.fbo); // u32 fbo
		b.setInt32(20, s.ibo); // u32 ibo
		b.setInt32(24, s.vbo); // u32 vbo
		b.setInt32(28, s.first); // i32 first
		b.setInt32(32, s.count); // i32 count
		b.setInt32(36, s.clearColor); // u32 clearColor RGBA8
		b.setFloat(40, s.clearDepth); // f32 clearDepth
		b.set(44, s.clearMask & 0xFF); // u8 clearMask
		b.set(45, 0); // u8 stateVersion（native-only）
		b.setUInt16(46, 0); // u16 reserved
		b.setInt32(48, s.srcLine); // u32 srcLine
		b.setInt32(52, s.rtParams); // u32 rtParams
		b.set(56, s.progFormat); // u8 progFormat
		b.set(57, 0); // u8 reserved
		b.setUInt16(58, 0); // u16 reserved
		b.setInt32(60, 0); // u32 reserved
	}

	// ------------------------------------------------------------------
	// 字段打包辅助
	// ------------------------------------------------------------------

	/** spec §3 rtParams：bit0-2 selector，bit3-4 antiAlias(0/2/4/16 → 0/1/2/3)，bit5 depthStencil。 */
	public static function packRtParams(isRTT:Bool, surfaceSelector:Int, antiAlias:Int, depthStencil:Bool):Int
	{
		if (!isRTT) return depthStencil ? 0x20 : 0;
		var aa = switch (antiAlias)
		{
			case 0: 0;
			case 2: 1;
			case 4: 2;
			case 16: 3;
			default: 0;
		}
		return (surfaceSelector & 0x07) | (aa << 3) | (depthStencil ? 0x20 : 0);
	}

	/** clearColor（spec §3）：RGBA8，R 在低字节，A 在高字节；分量夹到 0..255。 */
	public static function packClearColor(r:Float, g:Float, b:Float, a:Float):Int
	{
		var rr = channel(r);
		var gg = channel(g);
		var bb = channel(b);
		var aa = channel(a);
		return (aa << 24) | (bb << 16) | (gg << 8) | rr;
	}

	static function channel(v:Float):Int
	{
		if (v <= 0) return 0;
		if (v >= 1) return 255;
		return Std.int(v * 255 + 0.5) & 0xFF;
	}
}
