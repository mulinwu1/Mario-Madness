package openfl.display3D._internal;

import haxe.io.Bytes;

#if (lime_cffi && !macro)
import lime.system.CFFI;
import lime.utils.DataPointer;
#end

/**
	`[vkpkt]` native 薄壳（v2 重写，最小版）。

	设计原则（与旧版的区别写在这里，避免下一个人再踩）：

	1. **默认完全不工作**。只有环境变量 `VK_PKT=1`（或 `2`/`full`）才进入记录模式；
	   其余情况下 `recording()` 只返回一个缓存的 Bool —— 零 FFI、零分配、零 native 调用。
	   旧版每笔 draw 都会先跑 `VKPktNative.init()`（CFFI.load 四个符号），符号缺失或加载
	   异常时可能在主线程上炸掉，这是"普通玩法也闪退"的首要嫌疑。

	2. **故障自关**。符号加载失败、调用抛异常 → 把 `__dead` 置 true，记录器永久停用，
	   游戏继续按原路径跑。记录器永远不是游戏能否运行的先决条件。

	3. **不改变渲染流程**。本类只提供 emit/frame 两个旁路调用；GL 状态机、`__flushGL()`
	   的调用时机完全由 Context3D 原逻辑决定（旧版把 `__flushGL()` 换成了自己的
	   `__vkFlushDraw()`，一处标志位错误就会漏刷整个 GL 状态）。

	符号约定（native 侧 `gl_packet.cpp`）：
	  lime_vk_pkt_emit(double ptr, int bytes) -> int
	  lime_vk_pkt_frame(int frameGen, double animTime) -> int
**/
class VKPktNative
{
	// ---------------------------------------------------------------- 开关

	static var __on:Null<Bool> = null;
	static var __dead:Bool = false;
	static var __verbose:Bool = false;

	// ---------------------------------------------------------------- CFFI

	#if (lime_cffi && !macro)
	static var __loaded:Bool = false;
	static var __emit:Dynamic = null;
	static var __frame:Dynamic = null;
	// 2a 直连（与记录器无关，独立开关 VK_BACKEND）
	static var __bindFbo:Dynamic = null;
	static var __bindFboTried:Bool = false;
	#end

	static var __backendTarget:Null<Bool> = null;

	/**
		记录模式是否打开。Haxe 侧独立判定，不依赖 native：
		`VK_PKT=1`（摘要模式）/ `VK_PKT=2` 或 `full`（逐帧日志）。
	**/
	public static function recording():Bool
	{
		if (__dead) return false;
		if (__on == null)
		{
			var on = false;
			#if sys
			try
			{
				var v = Sys.getEnv("VK_PKT");
				if (v != null)
				{
					v = StringTools.trim(v).toLowerCase();
					on = (v == "1" || v == "2" || v == "full");
					__verbose = (v == "2" || v == "full");
				}
			}
			catch (e:Dynamic) {}
			#end
			__on = on;
			if (on) __load();
		}
		return __on;
	}

	public static function verbose():Bool
	{
		if (__on == null) recording();
		return __verbose;
	}

	static function __load():Void
	{
		#if (lime_cffi && !macro)
		if (__loaded) return;
		__loaded = true;
		if (!CFFI.enabled)
		{
			__dead = true;
			return;
		}
		try
		{
			__emit = CFFI.load("lime", "lime_vk_pkt_emit", 2, true);
		}
		catch (e:Dynamic)
		{
			__emit = null;
		}
		try
		{
			__frame = CFFI.load("lime", "lime_vk_pkt_frame", 2, true);
		}
		catch (e:Dynamic)
		{
			__frame = null;
		}
		// 只缺 frame 可以忍（帧摘要没有，但 packet 照记）；缺 emit 就没有记录的意义。
		if (__emit == null) __dead = true;
		if (!__dead)
		{
			// 启用握手：同时保证 `[vkpkt]` 字面量进 exe（CI 用它确认记录器真的编进去了）。
			#if sys
			try
			{
				Sys.stderr().writeString("[vkpkt] recorder armed\n");
				Sys.stderr().flush();
			}
			catch (e:Dynamic) {}
			#end
		}
		#else
		__dead = true;
		#end
	}

	/** 送一笔 64B packet。失败即永久停用记录器，异常不外抛。 **/
	public static function emit(packet:Bytes):Void
	{
		#if (lime_cffi && !macro)
		if (__dead || __emit == null) return;
		try
		{
			__emit(DataPointer.fromBytes(packet), 64);
		}
		catch (e:Dynamic)
		{
			__dead = true;
		}
		#end
	}

	/** 帧末通知（native 打摘要并重置帧计数）。 **/
	public static function frame(frameGen:Int, animTime:Float):Void
	{
		#if (lime_cffi && !macro)
		if (__dead || __frame == null) return;
		try
		{
			__frame(frameGen, animTime);
		}
		catch (e:Dynamic)
		{
			__dead = true;
		}
		#end
	}

	// ---------------------------------------------------------------- 2a：渲染目标直连
	// 与记录器相互独立：默认关（VK_BACKEND=1 或含 "target" 才开）。
	// 保留原实现，避免这次重写顺手丢掉已经推上去的能力。

	public static function backendTargetEnabled():Bool
	{
		if (__backendTarget == null)
		{
			var on = false;
			#if sys
			try
			{
				var v = Sys.getEnv("VK_BACKEND");
				on = (v != null) && (v == "1" || v.indexOf("target") >= 0);
			}
			catch (e:Dynamic) {}
			#end
			__backendTarget = on;
		}
		return __backendTarget;
	}

	/** 返回 true = 已直连；false = 调用方应回退 gl.bindFramebuffer。 **/
	public static function bindDrawFbo(fbo:Int):Bool
	{
		#if (lime_cffi && !macro)
		if (!__bindFboTried)
		{
			__bindFboTried = true;
			try
			{
				__bindFbo = CFFI.load("lime", "lime_vk_bind_draw_fbo", 1, true);
			}
			catch (e:Dynamic)
			{
				__bindFbo = null;
			}
		}
		if (__bindFbo == null) return false;
		try
		{
			var r:Dynamic = __bindFbo(fbo);
			return (r != null) && (Std.int(r) != 0);
		}
		catch (e:Dynamic)
		{
			__bindFbo = null;
			return false;
		}
		#else
		return false;
		#end
	}
}
