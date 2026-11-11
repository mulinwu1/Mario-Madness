package;

class CppAPI
{
	#if windows
	public static function obtainRAM():Int
	{
		return WindowsData.obtainRAM();
	}

	public static function darkMode()
	{
		WindowsData.setWindowColorMode(DARK);
	}

	public static function lightMode()
	{
		WindowsData.setWindowColorMode(LIGHT);
	}

	public static function setWindowOppacity(a:Float)
	{
		WindowsData.setWindowAlpha(a);
	}

	public static function _setWindowLayered()
	{
		WindowsData._setWindowLayered();
	}

	public static function setWallpaper(path:String)
	{
		if(path == 'old') {
			if(Wallpaper.oldWallpaper != null) {
			path = Wallpaper.oldWallpaper;
			}else{
				return;
			}}
		Wallpaper.setWallpaper(path);
	}

	public static function setOld()
	{
		Wallpaper.setOld();
	}

	public static function hideTaskbar()
	{
		WindowsData.hideTaskbar();
	}

	public static function restoreTaskbar()
	{
		WindowsData.restoreTaskbar();
	}

	public static function hideWindows()
	{
		WindowsData.hideWindows();
	}

	public static function restoreWindows()
	{
		WindowsData.restoreWindows();
	}

	public static function setTransparency(winName:String, color:Int)
	{
		Transparency.setTransparency(winName, color);
	}
	
	public static function removeWindowIcon()
	{
		WindowsData.removeWindowIcon();
	}

	public static function reset()
	{
		Transparency.reset();
	}
	public static function allowHighDPI() {
		WindowsData.registerHighDpi();
	}
	#else
	// 非 Windows（Android 等）：这些 Windows 专有 API 一律空实现，
	// 保证 PlayState 里无条件的 CppAPI 调用能编译通过。
	public static function obtainRAM():Int return 0;
	public static function darkMode() {}
	public static function lightMode() {}
	public static function setWindowOppacity(a:Float) {}
	public static function _setWindowLayered() {}
	public static function setWallpaper(path:String) {}
	public static function setOld() {}
	public static function hideTaskbar() {}
	public static function restoreTaskbar() {}
	public static function hideWindows() {}
	public static function restoreWindows() {}
	public static function setTransparency(winName:String, color:Int) {}
	public static function removeWindowIcon() {}
	public static function reset() {}
	public static function allowHighDPI() {}
	#end
}
