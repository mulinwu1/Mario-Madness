package openfl.display3D._internal;

import haxe.crypto.Sha256;
import haxe.io.Bytes;

/**
	`[vkpkt]` 记录侧写手（纯逻辑层）。

	设计约束（来自 docs/draw_packet_spec.md v1）：
	- WirePacket 固定 64 字节，小端，字段偏移/宽度见 spec §3；
	- 每帧 seq 从 1，frameGen 由 Context3D.present() 递增；
	- 日志行字段顺序与 tools/vkaudit/SPEC.md §2/§4 一致；
	- canonical/sum 只含 DIGEST_FIELDS（不含 s、sum）。

	本类**不引用 lime / GL**，因此可以在没有 lime 的环境里独立编译运行
	（离线验证就是这么做的）。真正的 native 调用由 Context3D 通过
	`nativeMode/nativeEmit/nativeUtag/nativeFrame` 四个回调注入。
**/
class VKDrawSink
{
	public static inline var VERSION:Int = 1;
	public static inline var PACKET_BYTES:Int = 64;

	/** 帧号：Context3D.present() 里 ++，首帧为 1。 */
	public static var frameGen:Int = 1;

	/** 帧内 packet 序号：每次 emit 从 1 递增，帧末归零。 */
	public static var seq:Int = 0;

	/** 复用的 packet 缓冲（spec §2：逐 draw 覆盖写，不分配）。 */
	public static var packet:Bytes = Bytes.alloc(PACKET_BYTES);

	// --- native 回调（Context3D 首次 draw 前注入；离线测试保持 null） ---
	public static var nativeMode:Void->Int = null;
	public static var nativeEmit:Bytes->Int->Int = null;
	public static var nativeUtag:Void->Int = null;
	public static var nativeFrame:Int->Float->Int = null;

	/** 总开关（测试/回退用；默认 true，实际是否出日志由 VK_PKT_FRAMES 决定）。 */
	public static var logEnabled:Bool = true;

	// GL 常量（GLES/GL 规范值，写成常量避免依赖 lime 的 GL 类）
	static inline var GL_ONE:Int = 0x0001;
	static inline var GL_SRC_ALPHA:Int = 0x0302;
	static inline var GL_ONE_MINUS_SRC_ALPHA:Int = 0x0303;
	static inline var GL_FUNC_ADD:Int = 0x8006;
	static inline var GL_SUBTRACT:Int = 0x800A;
	static inline var GL_REVERSE_SUBTRACT:Int = 0x800B;
	static inline var GL_MIN:Int = 0x8007;
	static inline var GL_MAX:Int = 0x8008;

	static var __cfgLoaded:Bool = false;
	static var __allFrames:Bool = false;
	static var __ranges:Array<Array<Int>> = [];

	static var __modeChecked:Bool = false;
	static var __modeOK:Bool = true;

	// ------------------------------------------------------------------
	// 对外：重置 / 帧末
	// ------------------------------------------------------------------

	public static function resetSeq():Void
	{
		seq = 0;
	}

	public static function endFrame(animTime:Float):Void
	{
		if (nativeFrame != null && modeOK())
		{
			try
			{
				nativeFrame(frameGen, animTime);
			}
			catch (e:Dynamic) {}
		}
		seq = 0;
	}

	// ------------------------------------------------------------------
	// 对外：emit 一笔
	// ------------------------------------------------------------------

	/**
		写 WirePacket → 调 native → 按开关输出 `[vkpkt]`。
		返回实际输出的日志行（未输出时返回 null）。
	**/
	public static function emit(s:VKDrawSnapshot):String
	{
		if (!modeOK()) return null;

		seq++;

		var u:Int = (nativeUtag != null) ? nativeUtag() : 0;
		writePacket(s, seq, frameGen, u);

		if (nativeEmit != null)
		{
			try
			{
				nativeEmit(packet, PACKET_BYTES);
			}
			catch (e:Dynamic) {}
		}

		if (!logFor(frameGen)) return null;

		var line = buildLine(s, seq, frameGen, u);
		#if sys
		Sys.stderr().writeString(line + "\n");
		Sys.stderr().flush();
		#else
		trace(line);
		#end
		return line;
	}

	// ------------------------------------------------------------------
	// WirePacket（spec §3）
	// ------------------------------------------------------------------

	static function writePacket(s:VKDrawSnapshot, seqValue:Int, frame:Int, u:Int):Void
	{
		var b = packet;
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
		b.set(45, 0); // u8 stateVersion（native-only，Haxe 侧写 0）
		b.setUInt16(46, 0); // u16 reserved
		b.setInt32(48, s.srcLine); // u32 srcLine
		b.setInt32(52, s.rtParams); // u32 rtParams
		b.set(56, s.progFormat); // u8 progFormat
		b.set(57, 0); // u8 reserved
		b.setUInt16(58, 0); // u16 reserved
		b.setInt32(60, s.texName); // u32 主采样纹理 GL 名（0=未记录）
	}

	// ------------------------------------------------------------------
	// 日志行 / canonical / sum（spec §10）
	// ------------------------------------------------------------------

	public static function buildLine(s:VKDrawSnapshot, s0:Int, f:Int, u:Int):String
	{
		var n = (s.kind == 2) ? 0 : s.count;
		var v0 = (s.kind == 1) ? "" + s.first : "-";
		var idx = (s.kind == 0) ? "" + s.count : "-";
		var slots = slotText(s.textures);
		var blend = s.blendName;
		var depth = s.depthEnabled ? 1 : 0;
		var dwrite = s.depthWrite ? 1 : 0;
		var scissor = rectText(s.scissor);
		var viewport = rectText(s.viewport);
		var uHex = hexN(u, 8);

		// 摘要字段顺序固定为 DIGEST_FIELDS（不含 s、sum）
		var vals:Array<String> = [
			"f=" + f,
			"prog=" + s.program,
			"rt=" + s.fbo,
			"prim=tris",
			"n=" + n,
			"v0=" + v0,
			"idx=" + idx,
			"slots=" + slots,
			"blend=" + blend,
			"depth=" + depth,
			"dwrite=" + dwrite,
			"scissor=" + scissor,
			"viewport=" + viewport,
			"u=" + uHex
		];

		var sum = Sha256.make(Bytes.ofString(vals.join(";"))).toHex().substr(0, 12);

		return "[vkpkt] f=" + f + " s=" + s0 + " " + vals.slice(1).join(" ") + " sum=" + sum;
	}

	// ------------------------------------------------------------------
	// 字段口径辅助（可被离线测试直接调用）
	// ------------------------------------------------------------------

	/** spec §10.1：slot 按 unit 0..n-1，未绑定写 '-'，冒号分隔。 */
	public static function slotText(textures:Array<Int>):String
	{
		if (textures == null || textures.length == 0) return "-";
		var parts:Array<String> = [];
		for (t in textures) parts.push(t < 0 ? "-" : "" + t);
		return parts.join(":");
	}

	/**
		slots 的取值域：只覆盖**当前 program 声明的 sampler 数**（native GatherTex 同口径）。
		`names` 是按 unit 递增的 texName 列表（未绑定 -1），`samplerCount` 为 program 声明数；
		不足的槽补 -1（native 对未绑定 unit 写 '-'），多余的寄存器（跨 program 残留）不进日志。
	**/
	public static function samplerSlots(names:Array<Int>, samplerCount:Int):Array<Int>
	{
		var result:Array<Int> = [];
		if (samplerCount <= 0) return result;
		var n = (samplerCount > 8) ? 8 : samplerCount; // kMaxTexSlots
		for (i in 0...n)
		{
			result.push((names != null && i < names.length) ? names[i] : -1);
		}
		return result;
	}

	public static function rectText(r:Array<Int>):String
	{
		if (r == null) return "0,0,0,0";
		return r[0] + "," + r[1] + "," + r[2] + "," + r[3];
	}

	/**
		scissor 口径：与 Context3D.__flushGLScissor (2263-2272) 逐字一致。
		关闭时统一 0,0,0,0；Y 翻转只在 renderToTexture==null && stage3D==null 时。
	**/
	public static function resolveScissor(enabled:Bool, x:Float, y:Float, w:Float, h:Float, applyYFlip:Bool, contextHeight:Int):Array<Int>
	{
		if (!enabled) return [0, 0, 0, 0];
		var ix = Std.int(x);
		var iy = Std.int(y);
		var iw = Std.int(w);
		var ih = Std.int(h);
		if (applyYFlip) iy = contextHeight - Std.int(h) - iy;
		return [ix, iy, iw, ih];
	}

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

	/** spec §10.3 blend 取值表。 */
	public static function blendName(enabled:Bool, eqRgb:Int, srcRgb:Int, dstRgb:Int):String
	{
		if (!enabled) return "none";
		switch (eqRgb)
		{
			case GL_SUBTRACT:
				return "subtract";
			case GL_REVERSE_SUBTRACT:
				return "reverse_subtract";
			case GL_MIN:
				return "min";
			case GL_MAX:
				return "max";
			default:
		}
		if (eqRgb == GL_FUNC_ADD)
		{
			if (srcRgb == GL_ONE && dstRgb == GL_ONE) return "add";
			if (srcRgb == GL_SRC_ALPHA && dstRgb == GL_ONE_MINUS_SRC_ALPHA) return "alpha";
			if (srcRgb == GL_ONE && dstRgb == GL_ONE_MINUS_SRC_ALPHA) return "premultiplied";
		}
		return "x" + hexN(srcRgb, 4) + ":" + hexN(dstRgb, 4) + ":" + hexN(eqRgb, 4);
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

	/** 定宽小写 hex（负数按 32 位补码处理）。 */
	public static function hexN(v:Int, n:Int):String
	{
		var s = "";
		var i = n - 1;
		while (i >= 0)
		{
			s += hexDigit((v >>> (i * 4)) & 0x0F);
			i--;
		}
		return s;
	}

	static inline function hexDigit(d:Int):String
	{
		return String.fromCharCode(d < 10 ? 48 + d : 87 + d);
	}

	// ------------------------------------------------------------------
	// 日志开关（spec §10.4 / vkaudit SPEC §8）
	// ------------------------------------------------------------------

	public static function logFor(f:Int):Bool
	{
		if (!logEnabled) return false;
		__loadConfig();
		if (__allFrames) return true;
		for (r in __ranges)
		{
			if (f >= r[0] && f <= r[1]) return true;
		}
		return false;
	}

	static function __loadConfig():Void
	{
		if (__cfgLoaded) return;
		__cfgLoaded = true;

		#if sys
		var v = Sys.getEnv("VK_PKT_FRAMES");
		if (v != null && v.length > 0)
		{
			v = StringTools.trim(v).toLowerCase();
			if (v == "all")
			{
				__allFrames = true;
			}
			else
			{
				for (part in v.split(","))
				{
					var p = StringTools.trim(part);
					if (p.length == 0) continue;
					var dots = p.indexOf("..");
					if (dots >= 0)
					{
						var a = Std.parseInt(p.substr(0, dots));
						var b = Std.parseInt(p.substr(dots + 2));
						if (a != null && b != null) __ranges.push([a, b]);
					}
					else
					{
						var n = Std.parseInt(p);
						if (n != null) __ranges.push([n, n]);
					}
				}
			}
		}
		#end
	}

	static function modeOK():Bool
	{
		if (__modeChecked) return __modeOK;
		__modeChecked = true;
		if (nativeMode != null)
		{
			try
			{
				__modeOK = (nativeMode() != 0);
			}
			catch (e:Dynamic)
			{
				__modeOK = true;
			}
		}
		return __modeOK;
	}
}
