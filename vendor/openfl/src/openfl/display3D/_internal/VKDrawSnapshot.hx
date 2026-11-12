package openfl.display3D._internal;

/**
	一笔 draw/clear 的已解析快照。Context3D 在记录点填好，VKDrawSink 只负责序列化。
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
	/** 发射这一刻的主采样纹理（GL 名）；packet 自带，执行侧不再猜 GL 状态。 */
	public var texName:Int = 0;

	// 仅日志字段
	public var blendName:String = "none";
	public var depthEnabled:Bool = false;
	public var depthWrite:Bool = false;
	public var scissor:Array<Int> = [0, 0, 0, 0];
	public var viewport:Array<Int> = [0, 0, 0, 0];
	public var textures:Array<Int> = [];

	public function new() {}
}
