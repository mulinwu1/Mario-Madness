package openfl.display3D._internal;

#if (lime_cffi && !macro)
import haxe.io.Bytes;
import lime.system.CFFI;
import lime.utils.DataPointer;

/**
	`[vkpkt]` 记录侧的 native 薄壳。

	符号约定（与 docs/draw_packet_spec.md §2 一致，native 侧由另一条线实现）：
	  lime_vk_pkt_mode()        -> int  0=B形态(旧 GL 路径) 1=packet
	  lime_vk_pkt_emit(ptr,n)   -> int  消费 packet 数；<0 = 错误码（spec §2）
	  lime_vk_pkt_utag()        -> int  当前 program 的 uniform 摘要（必须在 emit 前调用）
	  lime_vk_pkt_frame(f,t)    -> int  帧末：f=帧号，t=动画秒数

	符号尚不存在时（native 还没接）不会报错：回调保持 null，Haxe 侧日志仍可独立工作。
**/
class VKPktNative
{
	static var __mode:Dynamic = null;
	static var __emit:Dynamic = null;
	static var __utag:Dynamic = null;
	static var __frame:Dynamic = null;

	static var __ready:Bool = false;

	public static function init():Void
	{
		if (__ready) return;
		__ready = true;

		if (!CFFI.enabled) return;

		try
		{
			__mode = CFFI.load("lime", "lime_vk_pkt_mode", 0, true);
			__emit = CFFI.load("lime", "lime_vk_pkt_emit", 2, true);
			__utag = CFFI.load("lime", "lime_vk_pkt_utag", 0, true);
			__frame = CFFI.load("lime", "lime_vk_pkt_frame", 2, true);
		}
		catch (e:Dynamic)
		{
			return;
		}

		VKDrawSink.nativeMode = function():Int
		{
			// 符号缺失视为 packet 形态：日志可独立验证，emit 自然为空操作
			if (__mode == null) return 1;
			return intOf(__mode());
		};

		VKDrawSink.nativeEmit = function(bytes:Bytes, n:Int):Int
		{
			if (__emit == null) return 0;
			return intOf(__emit(DataPointer.fromBytes(bytes), n));
		};

		VKDrawSink.nativeUtag = function():Int
		{
			if (__utag == null) return 0;
			return intOf(__utag());
		};

		VKDrawSink.nativeFrame = function(f:Int, t:Float):Int
		{
			if (__frame == null) return 0;
			return intOf(__frame(f, t));
		};
	}

	static inline function intOf(v:Dynamic):Int
	{
		if (v == null) return 0;
		return Std.int(v);
	}
}
#end
