package openfl.display3D._internal;

/**
	一笔 draw/clear 的已解析快照（v2 精简版）。

	只保留 WirePacket（spec §3）真正携带的字段。状态类字段（blend/depth/scissor/
	viewport/textures）由 native 在收到 packet 的那一刻从 GL 状态镜像抓取，
	Haxe 侧不再重复抓一遍 —— 旧版为此每笔 draw 分配字符串和数组，是记录器的主要开销来源。
**/
class VKDrawSnapshot
{
	public var kind:Int = 0; // 0=elements 1=arrays 2=clear
	public var flags:Int = 0; // bit0 implicitClear, bit1 hasProgram, bit2 stageOwnsContext
	public var program:Int = 0;
	public var progFormat:Int = 0; // 0=GLSL 1=AGAL
	public var fbo:Int = 0;
	public var ibo:Int = 0;
	public var vbo:Int = 0;
	public var first:Int = 0;
	public var count:Int = 0;
	public var clearColor:Int = 0;
	public var clearDepth:Float = 1.0;
	public var clearMask:Int = 0;
	public var srcLine:Int = 0;
	public var rtParams:Int = 0;

	public function new() {}
}
