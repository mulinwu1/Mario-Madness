# 接入说明（一页）

## 1. 挂上分叉

在主线工程的 `Project.xml` 里把 openfl 指到分叉目录（`haxelib dev` 只登记路径，不改工程文件）：

```
haxelib dev openfl <绝对路径>\vendor\openfl
```

确认生效：

```
haxelib path openfl
```

应输出 `...\vendor\openfl\src`。

## 2. 打开记录开关

`Project.xml`（或构建参数）加一行：

```xml
<haxedef name="openfl_vk_pkt" />
```

等价命令行：`-D openfl_vk_pkt`。

**不加这一行 = 完全不记录，行为与上游 9.2.0 一致**，可随时回退。

## 3. 选择要记录哪些帧

运行环境变量（缺省 = 不输出任何 `[vkpkt]`）：

```
set VK_PKT_FRAMES=all          # 全部帧
set VK_PKT_FRAMES=100..130     # 闭区间
set VK_PKT_FRAMES=1,5,9        # 指定帧
set VK_PKT_FRAMES=100..130,300 # 混合
```

日志写 stderr，可与现有 `[vkdraw]` / `[glvk]` 行共存；工具按前缀抽取。

## 4. native 需要提供的四个符号（lime ndll）

| 符号 | 参数 | 返回值 |
|------|------|--------|
| `lime_vk_pkt_mode` | 无 | `0`=旧 GL 路径（Haxe 侧整套记录自动静默），`1`=packet |
| `lime_vk_pkt_emit` | `(const uint8_t* packet, int bytes)` | 已消费字节数 |
| `lime_vk_pkt_utag` | 无 | 当前 program 的 uniform 摘要（32 位，**必须在 emit 前一次调用**） |
| `lime_vk_pkt_frame` | `(int frameGen, double animTime)` | 忽略 |

四个符号用 `CFFI.load(..., lazy=true)` 懒加载：native 还没实现时不会崩，Haxe 侧日志照常可出
（`u` 退化为 `00000000`，emit 为空操作）。

## 5. 接入后第一个该看的日志

按顺序看三件事：

1. **首帧是不是以隐式 clear 开头**
   ```
   [vkpkt] f=1 s=1 prog=... rt=... n=0 v0=- idx=- ... scissor=0,0,0,0
   ```
   `s=1` 应是 clear（`flags` 的 bit0 在 wire 里为 1，日志行本身不打印 flags）；若第 1 条就是 draw，说明隐式 clear 分支没被走到（检查 `__cleared` / `renderToTexture`）。
2. **帧号/序号是否连续**
   同一帧 `s` 从 1 递增；跨帧 `f` 严格 +1。若 `f` 跳变而 `s` 没归零，说明帧末 `__vkEndFrame` 没被调用（检查 `__render` 的 present 路径）。
3. **`[vkexec]` 与 `[vkpkt]` 的 `sum` 是否逐行一致**
   ```
   python <绝对路径>\tools\vkaudit\vkaudit.py recon <packet.log> <exec.log> --out report.md
   ```
   全 `OK` 才说明 native 侧解码/提交没有改写状态；出现“摘要不符”的那一行，就是偏差起点。

## 6. 回退

去掉 `<haxedef name="openfl_vk_pkt" />` 即可；`haxelib dev` 恢复原版用：

```
haxelib dev openfl <原 openfl 9.2.0 路径>
```
