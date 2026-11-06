# gl_vk_diff —— GL / Vulkan 渲染一致性 + 性能基线工装

给 `native_vulkan/` 的「GLES → Vulkan」翻译层做自动化的**对不对**验证，并给出一份可对比的**性能基线**。

同一组测试场景分别经过：

- **参考路径**：ANGLE（真实 GLES3，跑在 D3D11 之上）→ 离屏 FBO → `glReadPixels`
- **被测路径 A**：`VkRenderer` 公开接口直连（`vk-direct`）
- **被测路径 B**：`lime_gl_*` 翻译层 → `VkRenderer`（`vk-translate`）

三条路径都产出 RGBA8 帧缓冲，逐像素比较，给出差异图、哈希、通过/失败判定。

## 结论速览（在本机初版实现上的实测结果）

| 场景 | vk-direct | vk-translate |
|---|---|---|
| 01 纯色清屏 | PASS | PASS |
| 02 单纹理 | DIFF（整帧上下镜像） | DIFF（整帧上下镜像） |
| 03 多纹理批绘制 | DIFF | DIFF |
| 04 tint / alpha | UNSUPPORTED | UNSUPPORTED |
| 05 离屏 RT 当纹理 | DIFF | DIFF |
| 06 blend 叠加 | DIFF | DIFF |
| 07 scissor 裁剪 | UNSUPPORTED | DIFF（裁剪完全没生效） |
| 08 clear 目标路由 | UNSUPPORTED | DIFF（clear 被抢到后缓冲） |

> 这些 DIFF 是**被测实现的问题**，不是工装的问题；详见下文「已发现的问题」。工装本身在
> `01_clear` 上 triple-A 对齐，说明读回、行序、颜色空间这条链路是通的。

## 目录结构

```
tools/gl_vk_diff/
  src/
    igl.h            两条路径共用的「类 GL」抽象接口（保证输入完全一致）
    image.h/.cpp     RGBA8 图像、BMP 导出、FNV-1a 哈希、像素差指标
    perf.h/.cpp      计数器 + 帧时间分布 + GVD_PERF 单行摘要
    scenes.h/.cpp    8 个测试场景（含程序化纹理）
    backends.h       后端工厂声明
    backend_gl.cpp   ANGLE/EGL/GLES3 参考后端（动态加载，不依赖 GL 头文件）
    backend_vk.cpp   Vulkan 双模式后端（直连 / 翻译层）
    native_gl_fbo.cpp  native_vulkan/gl_fbo.cpp 的编译垫片（补它缺失的符号）
    main.cpp         CLI：diff / bench 两种模式
  shim/              让 native_vulkan 源码在本工装里能编译的最小垫片
  scripts/           fetch_deps / build / run / perf / manual_window_check
  docs/              手动验证步骤（需要真实窗口的部分）
```

## 构建与运行

```powershell
powershell -ExecutionPolicy Bypass -File scripts\fetch_deps.ps1   # 只下 Vulkan 头文件
powershell -ExecutionPolicy Bypass -File scripts\build.ps1        # MSVC + cl
powershell -ExecutionPolicy Bypass -File scripts\run.ps1 --mode=diff
powershell -ExecutionPolicy Bypass -File scripts\perf.ps1
```

`run.ps1` 会自动在仓库里找 `libEGL.dll` 所在目录并传 `--angle-dir`；也可以手动指定：

```powershell
build\gl_vk_diff.exe --mode=diff --angle-dir=D:\path\to\angle\bin --out=out
```

常用开关：

| 开关 | 说明 |
|---|---|
| `--mode=diff\|bench` | 运行模式 |
| `--w=N --h=N` | 渲染尺寸，默认 512x512 |
| `--cases=a,b` | 只跑指定场景 |
| `--vk=direct,translate` | 只跑指定 Vulkan 路径 |
| `--no-gl` | 不跑参考，只产出 Vulkan 截图/哈希（报告显示 NO-REF） |
| `--threshold=N` | 单通道容差，默认 8 |
| `--max-pct=X` | 允许超阈值像素占比，默认 0.5% |
| `--frames=N --quads=N` | bench 负载规模 |

输出：

- `out/<场景>/ref_gl.bmp`、`vk-direct.bmp`、`vk-translate.bmp`
- `out/<场景>/*.diff.bmp`（差异放大 4 倍的灰度图）
- `out/report.md`、`out/report.json`
- `out/perf.log`（bench 基线）

退出码：`0` = 全部 PASS/UNSUPPORTED；`1` = 存在 DIFF 或 ERROR；`2` = 命令行错误。

## 场景覆盖

| 场景 | 覆盖语义 | 关键点 |
|---|---|---|
| `01_clear` | 纯色清屏 | 基线：颜色、行序、alpha 是否对齐 |
| `02_one_texture` | 单纹理绘制 | 纹理 V 方向、UV 采样 |
| `03_multi_texture` | 多纹理批绘制 | 3 张纹理 × 4 个 quad，纹理绑定切换 |
| `04_tint_alpha` | 带 tint/alpha 的 sprite | 需要逐 draw 颜色；缺能力则记 UNSUPPORTED |
| `05_offscreen_rt` | 离屏 RT 再用作纹理 | 两 pass：RT 内 10 倍放大 → 再 1:1 采样 |
| `06_blend_overlay` | blend 叠加 + draw order | 半透明圆盘后画不透明棋盘；纹理创建顺序故意与绘制顺序相反 |
| `07_scissor` | scissor 裁剪 | 只裁左半边，裁剪是否真的下发 |
| `08_clear_routing` | clear 目标路由 | 先画红再 `glClear` 绿；验证 clear 是否落在当前目标 |

场景纹理都是程序化生成，且**故意非对称**（棋盘 + 顶部红条 + 左侧蓝条 + 左上绿块），
用来一眼识破上下/左右翻转；圆盘纹理带径向 alpha，用来测 blend。

## 判定规则

每条用例的结论四态：

- **PASS**：直接比较，超阈值像素占比 `<= --max-pct`。
- **DIFF**：像素不一致。若「上下翻转后」一致，报告会额外标注 `(Y 轴镜像)`——
  这只说明整帧方向反了，仍然算失败。
- **UNSUPPORTED**：被测实现没有对应能力（`Has(Caps)==false`），记入能力缺口，不算失败。
- **ERROR**：后端初始化/帧失败。

设计上故意让 `CAP_SCISSOR`、`CAP_BLEND` 在 translate 模式声明为**可用**：接口确实存在，
跑起来才能暴露「状态被记录了但没下发」这种静默错误（见问题 3）。

## 性能基线

bench 模式跑固定负载（默认 512 个四边形 / 帧、2 个 draw call / 帧、1 张纹理会话级上传），
**循环内不做读回**，只测「提交 + 等待」。每次运行输出一行可直接 grep 的摘要：

```
GVD_PERF backend=gl-angle size=512x512 frames=200 avg_ms=0.241 p50_ms=0.189 p95_ms=0.345 max_ms=5.458 fps=4154.6 present=200 tex_uploads=1 tex_bytes=16384 draw_calls=400 verts=614400
GVD_PERF backend=vk-direct size=512x512 frames=200 avg_ms=0.305 p50_ms=0.277 p95_ms=0.395 max_ms=2.138 fps=3279.8 present=200 tex_uploads=1 tex_bytes=16384 draw_calls=400 verts=614400
GVD_PERF backend=vk-translate size=512x512 frames=200 avg_ms=0.265 p50_ms=0.248 p95_ms=0.390 max_ms=1.311 fps=3773.5 present=200 tex_uploads=1 tex_bytes=16384 draw_calls=400 verts=614400
```

字段：`avg/p50/p95/max` 帧时间（毫秒）、`fps`、`present`（帧数）、`tex_uploads`/`tex_bytes`
（纹理上传次数与字节数）、`draw_calls`、`verts`。

用法：

```powershell
scripts\perf.ps1                        # 200 帧、512 quad
scripts\perf.ps1 -Frames 1000 -Quads 2000
```

对比两次改动：`git diff` 前后各跑一次，直接 diff `out/perf.log`。
注意 `vk-*` 的帧时间**包含 `VkRenderer::EndFrame` 里的 `vkQueueWaitIdle`**（上游没有做帧流水），
所以这个数反映的是「现在的实现每帧真的花多久」，适合做前后对比基线，不能当作纯 GPU 时间。

现阶段只做测量，不做任何提速改动。

## 已发现的问题（实测可复现）

以下都是**被测实现**的问题，工装只负责证明，不在本目录修：

1. **整帧上下镜像** — `VkRenderer::DrawRaw` 把像素 y 转成 NDC 时用了 GL 的约定：
   `native_vulkan/vk_renderer.cpp` 里 `1.0f - py * 2.0f / h`。Vulkan 的 NDC y=+1 是**画面底部**
   （GL 是顶部），所以整帧垂直翻转。场景 02/05/06 都命中，报告标注 `Y 轴镜像`。
2. **`VkRenderer::EndFrame` 只第一帧有效** — 它从 `vkResetCommandBuffer`，之后
   `vkBeginCommandBuffer` 会失败且返回值被忽略，导致**后续所有帧静默丢弃**
   （`vk_renderer.cpp` 的 `g_beginCB(o.cmd, &bi)`）。工装在每次冲刷前替它 reset 才能跑；
   这条如果不修，游戏里表现就是「第一帧之后画面不再更新」。
3. **状态类接口只记录不下发** — `gl_core.cpp` 的 `lime_gl_scissor` / `lime_gl_blend_func` /
   `lime_gl_color_mask` / `lime_gl_enable` 只写静态变量，从不调用 `VkRenderer`。
   场景 07 在 translate 模式裁剪完全没生效（93.75% 像素超阈值）。
4. **`glClear` 把帧抢到后缓冲** — `gl_core.cpp` 的 `lime_gl_clear` 先试 `gl_vk_handle_clear()`，
   失败就直接 `BeginFrame(...)`（= 绑后缓冲），丢掉当前离屏目标。场景 08 专门测这个。
5. **draw order 丢失** — `EndFrame` 按纹理槽顺序批量提交（`for (auto& t : o.textures)`），
   同一帧里跨纹理的绘制顺序被重排。场景 06 的创建顺序与绘制顺序故意相反来暴露。
6. **`ReadPixels` 是空实现** — `vk_renderer.cpp` 里明确 `return false`。工装改用
   `vkCmdCopyImageToBuffer` 自行读回（见 `backend_vk.cpp: ReadTargetImage`）。
7. **缺少逐 draw tint** — 只有 `gl_stubs.cpp` 里有 `lime_gl_uniform4f`，而它是重复定义、
   不参与构建；`VkRenderer` 着色器也没有 tint uniform。场景 04 记 UNSUPPORTED。
8. **`gl_fbo.cpp` 引用了未声明的 `g_rendererExt`** — 该变量在 `gl_vk_contract.cpp` 里是
   `static`，`gl_fbo.cpp` 单独编译必然失败。工装用 `src/native_gl_fbo.cpp` 在匿名命名空间补定义。
9. **`gl_vk_bridge.h` 依赖包含顺序** — 它在 `#include "gl_compat.h"` 之前就用了 `uint32_t`，
   某些 TU（例如首包含它的 `gl_fbo.cpp`）会编译失败。工装用 `shim/gvd_prelude.h` + `/FI` 强制
   先引入 `<cstdint>`。

## 边界与不改动的东西

- 本目录**不修改** `native_vulkan/` 与 `.github/workflows/`；所有为编译/读回/复位做的补偿都
  放在 `tools/gl_vk_diff/` 内，并在注释里写明「上游缺陷 + 为什么这样绕」。
- `native_vulkan/gl_stubs.cpp` 不在编译集合内（与官方构建清单一致：它与 `gl_fbo.cpp` 有重复定义）。
- 跑 diff 时每个场景都会新建一套 Vulkan 后端实例，保证场景间状态隔离。

## 无法自动化的部分

需要真实窗口/真实 swapchain 的验证（present 是否真的上屏、resize、全屏、输入）见
[docs/manual_window_check.md](docs/manual_window_check.md)，步骤和预期结果都写在里面。
