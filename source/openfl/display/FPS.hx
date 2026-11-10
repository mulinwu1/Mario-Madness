package openfl.display;

import flixel.math.FlxMath;
import haxe.Timer;
import openfl.events.Event;
import openfl.text.TextField;
import openfl.text.TextFormat;
#if gl_stats
import openfl.display._internal.stats.Context3DStats;
import openfl.display._internal.stats.DrawCallContext;
#end
#if flash
import openfl.Lib;
#end

#if openfl
import openfl.system.System;
#end

/**
	The FPS class provides an easy-to-use monitor to display
	the current frame rate of an OpenFL project
**/
#if !openfl_debug
@:fileXml('tags="haxe,release"')
@:noDebug
#end
class FPS extends TextField
{
	/**
		The current frame rate, expressed using frames-per-second
	**/
	public var currentFPS(default, null):Int;

	@:noCompletion private var cacheCount:Int;
	@:noCompletion private var currentTime:Float;
	@:noCompletion private var times:Array<Float>;
	@:noCompletion private var _pixelFontName:String;
	@:noCompletion private var _fontApplied:Bool = false;

	public function new(x:Float = 10, y:Float = 10, color:Int = 0x000000)
	{
		super();

		this.x = x;
		this.y = y;

		currentFPS = 0;
		selectable = false;
		mouseEnabled = false;
		// 启动早期 mariones.ttf 还没进 AssetLibrary，getFont 返回空壳，TextFormat 会退化成
		// 系统衬线字体（表现为又小又细、和原版完全不像）。这里先记下目标字体名，
		// 在 __enterFrame 里字体真正可用后重设一次。
		var _fnt = Assets.getFont(Paths.font("mariones.ttf"));
		_pixelFontName = (_fnt != null) ? _fnt.fontName : null;
		defaultTextFormat = new TextFormat(_pixelFontName, 10, color);
		autoSize = LEFT;
		multiline = true;
		text = "FPS: ";

		cacheCount = 0;
		currentTime = 0;
		times = [];

		#if flash
		addEventListener(Event.ENTER_FRAME, function(e)
		{
			var time = Lib.getTimer();
			__enterFrame(time - currentTime);
		});
		#end
	}

	// Event Handlers
	@:noCompletion
	private #if !flash override #end function __enterFrame(deltaTime:Float):Void
	{
		if (!_fontApplied) {
			var _f2 = Assets.getFont(Paths.font("mariones.ttf"));
			if (_f2 != null && _f2.fontName != null) {
				_fontApplied = true;
				defaultTextFormat = new TextFormat(_f2.fontName, 10, color);
			}
		}
		currentTime += deltaTime;
		times.push(currentTime);

		while (times[0] < currentTime - 1000)
		{
			times.shift();
		}

		var currentCount = times.length;
		currentFPS = Math.round((currentCount + cacheCount) / 2);
		if (currentFPS > ClientPrefs.framerate) currentFPS = ClientPrefs.framerate;

		if (currentCount != cacheCount /*&& visible*/)
		{
			text = "FPS: " + currentFPS;
			var memoryMegas:Float = 0;
			
			#if openfl
			memoryMegas = Math.abs(FlxMath.roundDecimal(System.totalMemory / 1000000, 1));
			text += "\nMEMORY: " + memoryMegas + " MB";
			#end

			textColor = 0xFFa11b1b;
			if (memoryMegas > 3000 || currentFPS <= ClientPrefs.framerate / 2 || PlayState.virtualmode == true)
			{
				textColor = 0xFFe30000;
			}

			#if (gl_stats && !disable_cffi && (!html5 || !canvas))
			text += "\ntotalDC: " + Context3DStats.totalDrawCalls();
			text += "\nstageDC: " + Context3DStats.contextDrawCalls(DrawCallContext.STAGE);
			text += "\nstage3DDC: " + Context3DStats.contextDrawCalls(DrawCallContext.STAGE3D);
			#end

			text += "\n";
		}

		cacheCount = currentCount;
	}
}
