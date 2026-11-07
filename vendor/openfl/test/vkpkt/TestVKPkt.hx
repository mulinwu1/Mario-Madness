package;

import openfl.display3D._internal.VKDrawSink;
import openfl.display3D._internal.VKDrawSnapshot;

/**
	离线验证：只依赖 VKDrawSink（不引用 lime / GL / 游戏代码）。

	产物：
	  stderr: [vkpkt] ...                  ← 交给 tools/vkaudit 的解析口径
	  stdout: [vkraw] s=.. <64字节hex>      ← 独立核对线格式偏移
	  stdout: [vkcase] s=.. 期望的线字段    ← 与解码结果逐字段比对

	跑法：
	  haxe -cp <openfl>/src -cp test/vkpkt -main TestVKPkt -python out/test_vkpkt.py
	  python out/test_vkpkt.py > out/test_vkpkt.log 2>&1
**/
class TestVKPkt
{
	static var frame:Int = 10;
	static var seq:Int = 0;

	static function main():Void
	{
		Sys.putEnv("VK_PKT_FRAMES", "all");

		// native 尚未接入：只给 utag，不给 emit（离线不需要 ndll）
		VKDrawSink.nativeUtag = function():Int return 0x0A1B2C3D;

		scenario1IndexedBasic();
		scenario2PremultipliedScissorOff();
		scenario3ArraysYFlip();
		scenario4ExplicitClear();
		scenario5ImplicitClear();
		scenario6RenderToTexture();
		scenario7Subtract();
		scenario8UnknownBlend();
		scenario9NoTextures();
		scenario10AgALArrays();

		scenario11ProgramSwitchSlots();
		scenario12ClearAfterBlendChange();

		scenarios13Helpers();

		Sys.println("[vkend] scenarios=14 packets=" + VKDrawSink.seq);
	}

	// ------------------------------------------------------------------

	static function emit(name:String, s:VKDrawSnapshot):Void
	{
		VKDrawSink.frameGen = frame;

		// 先把“写进 64 字节线”的期望值打出来，Python 侧按 (s) 与解码结果比对
		var line = VKDrawSink.emit(s);

		// emit 之后 seq 已递增，packet 即本笔
		var s0 = VKDrawSink.seq;

		Sys.println("[vkcase] name=" + name + " s=" + s0
			+ " kind=" + s.kind
			+ " flags=" + s.flags
			+ " f=" + frame
			+ " prog=" + s.program
			+ " rt=" + s.fbo
			+ " ibo=" + s.ibo
			+ " vbo=" + s.vbo
			+ " first=" + s.first
			+ " count=" + s.count
			+ " clearColor=" + s.clearColor
			+ " clearDepth=" + s.clearDepth
			+ " clearMask=" + s.clearMask
			+ " srcLine=" + s.srcLine
			+ " rtParams=" + s.rtParams
			+ " progFormat=" + s.progFormat);

		Sys.println("[vkraw] s=" + s0 + " " + VKDrawSink.packet.toHex());
		Sys.println("[vklog] s=" + s0 + " " + (line == null ? "<null>" : line));
	}

	static function base():VKDrawSnapshot
	{
		var s = new VKDrawSnapshot();
		s.flags = 0x06; // hasProgram | stageOwnsContext
		s.rtParams = 0;
		s.srcLine = 1218;
		s.viewport = [0, 0, 921, 691];
		s.clearDepth = 1.0;
		return s;
	}

	// s1 索引绘制：backbuffer、alpha 混合、depth 开、scissor 开、双槽
	static function scenario1IndexedBasic():Void
	{
		var s = base();
		s.kind = 0;
		s.program = 3;
		s.fbo = 0;
		s.ibo = 7;
		s.first = 0;
		s.count = 6;
		s.blendName = "alpha";
		s.depthEnabled = true;
		s.depthWrite = true;
		s.scissor = [0, 0, 921, 691];
		s.textures = [2, 3];
		emit("S1_indexed_basic", s);
	}

	// s2 预乘 alpha、depth 关、scissor 关、第二槽未绑定
	static function scenario2PremultipliedScissorOff():Void
	{
		var s = base();
		s.kind = 0;
		s.program = 5;
		s.fbo = 0;
		s.ibo = 8;
		s.first = 12;
		s.count = 3;
		s.blendName = "premultiplied";
		s.depthEnabled = false;
		s.depthWrite = false;
		s.scissor = [0, 0, 0, 0];
		s.textures = [4, -1];
		emit("S2_premultiplied_scissor_off", s);
	}

	// s3 非索引绘制 + scissor 需要 Y 翻转（Stage3D 为 null 的显示列表路径）
	static function scenario3ArraysYFlip():Void
	{
		var s = base();
		s.kind = 1;
		s.program = 3;
		s.fbo = 0;
		s.vbo = 9;
		s.first = 120;
		s.count = 18;
		s.srcLine = 2028;
		s.blendName = "add";
		s.depthEnabled = false;
		s.depthWrite = false;
		s.scissor = VKDrawSink.resolveScissor(true, 10, 20, 100, 50, true, 1000); // → 10,930,100,50
		s.textures = [];
		emit("S3_arrays_yflip", s);
	}

	// s4 显式 clear（COLOR|DEPTH），scissor 已被强制关闭
	static function scenario4ExplicitClear():Void
	{
		var s = base();
		s.kind = 2;
		s.flags = 0x04; // 无 program
		s.clearColor = VKDrawSink.packClearColor(0, 1, 0, 1); // 0xFF00FF00
		s.clearDepth = 0.5;
		s.clearMask = 3;
		s.scissor = [0, 0, 0, 0];
		s.textures = [];
		s.srcLine = 523;
		emit("S4_explicit_clear", s);
	}

	// s5 隐式 clear：flags.bit0
	static function scenario5ImplicitClear():Void
	{
		var s = base();
		s.kind = 2;
		s.flags = 0x07; // implicit | hasProgram | stageOwns
		s.clearColor = 0xFF000000;
		s.clearDepth = 1.0;
		s.clearMask = 1;
		s.scissor = [0, 0, 0, 0];
		s.srcLine = 523;
		emit("S5_implicit_clear", s);
	}

	// s6 RTT：selector=2 aa=4 depthStencil=true → rtParams=0x32
	static function scenario6RenderToTexture():Void
	{
		var s = base();
		s.kind = 0;
		s.program = 9;
		s.fbo = 5;
		s.ibo = 11;
		s.first = 0;
		s.count = 12;
		s.rtParams = VKDrawSink.packRtParams(true, 2, 4, true);
		s.blendName = "none";
		s.depthEnabled = false;
		s.depthWrite = false;
		s.scissor = [0, 0, 800, 600];
		s.textures = [6];
		emit("S6_render_to_texture", s);
	}

	// s7 SUBTRACT 方程（不只是因子）
	static function scenario7Subtract():Void
	{
		var s = base();
		s.kind = 0;
		s.program = 4;
		s.ibo = 12;
		s.count = 6;
		s.blendName = VKDrawSink.blendName(true, 0x800A, 1, 1);
		s.depthEnabled = true;
		s.depthWrite = true;
		s.scissor = [0, 0, 921, 691];
		s.textures = [2, 3];
		emit("S7_subtract", s);
	}

	// s8 未知组合
	static function scenario8UnknownBlend():Void
	{
		var s = base();
		s.kind = 0;
		s.program = 4;
		s.ibo = 13;
		s.count = 6;
		s.blendName = VKDrawSink.blendName(true, 0x8006, 0x0302, 0x0001);
		s.depthEnabled = true;
		s.depthWrite = false;
		s.scissor = [0, 0, 921, 691];
		s.textures = [2];
		emit("S8_unknown_blend", s);
	}

	// s9 无纹理槽
	static function scenario9NoTextures():Void
	{
		var s = base();
		s.kind = 0;
		s.program = 1;
		s.ibo = 14;
		s.count = 3;
		s.depthEnabled = false;
		s.scissor = [0, 0, 0, 0];
		s.textures = [];
		emit("S9_no_textures", s);
	}

	// s10 AGAL 路径（progFormat=1）
	static function scenario10AgALArrays():Void
	{
		var s = base();
		s.kind = 1;
		s.program = 21;
		s.progFormat = 1;
		s.vbo = 15;
		s.first = 0;
		s.count = 6;
		s.srcLine = 2028;
		s.blendName = "alpha";
		s.depthEnabled = false;
		s.depthWrite = false;
		s.scissor = [0, 0, 0, 0];
		s.textures = [3];
		emit("S10_agal_arrays", s);
	}

	// s11 程序切换后的 slots：__state.textures 残留 3 个寄存器，当前 program 只声明 2 个 sampler
	static function scenario11ProgramSwitchSlots():Void
	{
		var stale = [2, 3, 4]; // 上一 program 留下 3 个寄存器
		var s = base();
		s.kind = 0;
		s.program = 5;
		s.ibo = 16;
		s.first = 0;
		s.count = 3;
		s.blendName = "alpha";
		s.depthEnabled = true;
		s.depthWrite = true;
		s.scissor = [0, 0, 921, 691];
		s.textures = VKDrawSink.samplerSlots(stale, 2); // 新口径：按 program 声明数截到 2 项
		emit("S11_program_switch_slots", s);
		// 旧口径（取 textures 数组长度）会写出这一行；Python 侧断言两者不同
		Sys.println("[vkstale] name=S11 slots=" + VKDrawSink.slotText(stale));
	}

	// s12 两次 flush 之间改 blend/program：clear 记录前必须 __flushGL()，才能取到新值
	static function scenario12ClearAfterBlendChange():Void
	{
		var s = base();
		s.kind = 2;
		s.flags = 0x07; // implicit | hasProgram | stageOwns
		s.program = 8; // 切换后的 program
		s.clearColor = 0xFF000000;
		s.clearDepth = 1.0;
		s.clearMask = 1;
		s.scissor = [0, 0, 0, 0];
		s.blendName = "add"; // flush 后 native GL 镜像也会是 add
		s.depthEnabled = true;
		s.depthWrite = true;
		s.textures = [5, -1];
		s.srcLine = 523;
		emit("S12_clear_after_blend_change", s);
		// 旧口径（clear 前不 flush）会留下切换前的 prog=3 / blend=alpha
		Sys.println("[vkstale] name=S12 prog=3 blend=alpha");
	}

	// 纯函数口径（不进 packet，只打印断言供 Python 校验）
	static function scenarios13Helpers():Void
	{
		var a = VKDrawSink.resolveScissor(false, 1, 2, 3, 4, true, 100);
		var b = VKDrawSink.resolveScissor(true, 10.9, 20.9, 100.9, 50.9, true, 1000); // → 10,930,100,50
		var c = VKDrawSink.packRtParams(false, 3, 16, true); // 非 RTT → 0x20
		var d = VKDrawSink.packClearColor(0.2, 0.4, 0.6, 1); // → 0xFF996633
		var e = VKDrawSink.slotText([1, -1, 3]);
		var f = VKDrawSink.hexN(-1, 8);
		var g = VKDrawSink.slotText(VKDrawSink.samplerSlots([7], 2)); // → 7:-（不足按 program 补 -）
		var h = VKDrawSink.slotText(VKDrawSink.samplerSlots([2, 3, 4], 2)); // → 2:3（截断）
		var i = VKDrawSink.slotText(VKDrawSink.samplerSlots([2, 3], 0)); // → -

		Sys.println("[vkhelp] scissor_off=" + VKDrawSink.rectText(a)
			+ " scissor_yflip=" + VKDrawSink.rectText(b)
			+ " rtparams_backbuffer_ds=" + c
			+ " clearcolor=" + d
			+ " slots=" + e
			+ " hexneg=" + f
			+ " slot_pad=" + g
			+ " slot_trunc=" + h
			+ " slot_empty=" + i);
	}
}
