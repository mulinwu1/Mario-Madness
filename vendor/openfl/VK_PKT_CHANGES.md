# openfl 分叉改动说明（[vkpkt] 记录侧）

分叉位置：`vendor/openfl`（由 9.2.0 原样复制，未改动 `ref_openfl`）。
所有记录代码都编译开关保护：**`openfl_vk_pkt` 未定义时，行为与上游逐字一致**。

---

## 1. 改了哪些文件

### 新增

| 文件 | 作用 |
|------|------|
| `src/openfl/display3D/_internal/VKDrawSink.hx` | 64 字节 WirePacket 序列化、`[vkpkt]` 行、canonical/sum、`VK_PKT_FRAMES` 开关、native 回调槽 |
| `src/openfl/display3D/_internal/VKDrawSnapshot.hx` | 一笔 draw/clear 的已解析快照（纯数据） |
| `src/openfl/display3D/_internal/VKPktNative.hx` | lime CFFI 薄壳（`lime_vk_pkt_mode/emit/utag/frame`），懒加载，符号缺失不报错 |
| `test/vkpkt/TestVKPkt.hx` | 离线场景生成器（Haxe → Python 可跑） |
| `test/vkpkt/check_packets.py` | 离线核验：线偏移/字段顺序/独立重算 sum/vkaudit recon |

`VKDrawSink` 不 import lime/GL，因此可以脱离 lime 单独编译运行——离线验证就是靠这一点。

### 修改

| 文件 | 位置（upstream 行号） | 改动 |
|------|----------------------|------|
| `Context3D.hx` | 523 `gl.clear` 之前 | 发 clear packet（记录前先 `__flushGL()`，保证 blend/program/纹理与 native GL 镜像一致） |
| `Context3D.hx` | 1191–1205 隐式 clear 分支 | 置 `__vkImplicitClear`，让 clear packet 的 flags.bit0 生效；入口复位 `__vkStateFlushed` |
| `Context3D.hx` | 1218 `gl.drawElements` 之前 | 发索引 draw packet；`__flushGL()` 改走 `__vkFlushDraw()`（隐式 clear 已刷过则跳过） |
| `Context3D.hx` | 2028 `gl.drawArrays` 之前 | 发非索引 draw packet；同上 |
| `Context3D.hx` | `__vkTextureNames()` | `slots` 长度改为**当前 program 声明的 sampler 数**（不再用 `__state.textures.length`） |
| `Context3D.hx` | `present()` 内 | `VKDrawSink.frameGen++`（换前置缓冲 = 新帧代号） |
| `Context3D.hx` | `__flushGLViewport` 三处 `gl.viewport` | 记录实际 viewport（含 RTT / Stage3D 分支） |
| `Context3D.hx` | 文件尾 | `__vkEmitDraw / __vkEmitClear / __vkEndFrame / __vkSetViewport` 与取数辅助 |
| `OpenGLRenderer.hx` | 780 / 789 / 801 / 810 四处 `__gl.clear` 之前 | 发 clear packet（raw 清屏，scissor 仍按当前状态；调用点已先 `__flushGL()`，故 `flushFirst=false`） |
| `OpenGLRenderer.hx` | `setViewport()` | 同步 Context3D 的视口镜像 |
| `OpenGLRenderer.hx` | `__render` 末尾、`present()` 之前 | `__vkEndFrame(...)`：native frame() + seq 归零 |

> 说明：`.github/`、`.vscode/` 有意未复制进分叉；业务 Haxe 代码、`native_vulkan/`、`Project.xml`、CI 一律未动。

---

## 2. 为什么记录点钉在这里（与子任务文字的冲突）

子任务文字写的是“钉在入口那一刻，也就是 `__flushGL` 之前”；
但 `docs/draw_packet_spec.md` §3 的硬约束是：

> 记录点**必须**在 `__flushGL()` 之后、`program.__flush()` 之后、替换 `gl.drawElements / gl.drawArrays` 的那条语句。

两者冲突。按“格式文档为唯一真值”的约定，本分叉**采用文档口径**，冲突点在此显式记录。

原因很实际：立刻提交模式下，native 是从自己的 GL 镜像里取 DrawRecord 的
（顶点属性槽、depth compare、colorMask、cull/front、positionScale、uniform 摘要都由 GL 侧落地）。
- 在入口记录：`__flushGL` 还没执行，native 镜像还是**上一笔**的状态，`u` 也是旧 program 的摘要；
- 在 `gl.draw*` 语句前记录：本笔的 state 已全部落地，`program.__flush()` 也已把常量/采样器默认值推送完，
  `lime_vk_pkt_utag()` 取到的才是本笔真正要用的 uniform 摘要。

隐式 clear 仍发生在入口（1191–1205），它自己会先走 523 的 clear packet，因此“首帧隐式 clear”天然有序，不冲突。

---

## 3. 陷阱逐条落地

| 陷阱 | 处理 |
|------|------|
| 混合**方程**不只是因子 | `__vkBlendName()` 读 `__contextState.__glBlendEquation` + 两个 RGB 因子；输出 `subtract / reverse_subtract / min / max`，未知组合输出 `x<src>:<dst>:<eq>` |
| `clear` 是事件不是状态 | 记录点在 `gl.clear` 前；此时 `__setGLScissorTest(false)` 已生效、COLOR/DEPTH 的 mask 已被强制 true，所以 scissor 自然解析成 `0,0,0,0`，`dwrite` 取强制后的值；`clearMask` 自带本次要清的位 |
| `setVertexBufferAt` 立即生效 | 不在 setter 记；非索引 draw 的 `vbo` 取记录时刻的 `__currentGLArrayBuffer`（仅日志用，真值由 native 的 `g_attrs` 负责） |
| 顶点/索引缓冲原地复用 | packet 只带 GL buffer name；版本号由 native 记录（`ibo` 为 0 时表示无仍是同一对象） |
| 同帧 RTT 先写后读 / `present` 换前置缓冲 | `frameGen` 在 `present()` 递增；RTT 用 `fbo`（当前 GL FBO name）+ `rtParams` 表达；帧末 `frame()` 在 present 之前用本帧代号调用 |
| 首帧隐式 clear | `flags.bit0`，两条隐式分支都置位 |
| `positionScale ±1` | 它是 DrawRecord 字段，来自 native `g_state`；WirePacket 无此字段，Haxe 不需要写 |
| `setProgram` 用 sampler 默认值覆盖状态 | 所有取数都在 draw 时刻读 `__state.*`，`setProgram` 的覆盖已经发生，不会被 setter 缓存骗过 |

补充：`slots` 的取值域 = **当前 program 声明的 sampler 数**（native `gl_shader_sampler_count` 同口径）。
`__state.textures` 是寄存器下标数组、跨 program 只增不清，直接用它的长度会在程序切换后多出尾项；
现在由 `VKDrawSink.samplerSlots(names, samplerCount)` 截断/补齐：多余寄存器丢弃，缺的槽补 `-`。
GLSL 的 sampler 数取 `program.__glslSamplerNames` 的非 null 项；AGAL 取 `__agalSamplerUsageMask` 的置位数。
`clear` 行同理：记录前显式 `__flushGL()`（`__vkFlushForClear`，同一笔 draw 内只刷一次），
否则 native 读 GL 镜像、Haxe 读逻辑态，两次 flush 之间改过的 blend/program 会让同一行两侧不同。

---

## 4. 已知偏差 / 未定义口径（native 侧必须对齐）

1. **`stateVersion` 写 0**：spec 说它来自 native `g_version`，但 Haxe 没有对应 FFI；该字段仅日志用，vkaudit 不参与 sum。
2. **clear 行的 `n/v0/idx`**：spec §10.1 只定义了索引/非索引，未定义 clear。本分叉取 `n=0 v0=- idx=-`；native 侧若不同口径，`sum` 必然不符。
3. **`darken/lighten`**：spec 表里它们与 `min/max` 条件完全相同、无法区分；本分叉输出 `min/max`。
4. **`animTime`**：`lime_vk_pkt_frame(f, t)` 的 `t` 用 `haxe.Timer.stamp()`（墙钟），openfl 层拿不到 flixel 游戏时钟。
5. **多 Context3D**：`frameGen/seq` 是静态共享，假定单 Stage 主线；多 context 同时 present 时序号会穿插。
6. **后缓冲也是 FBO**：本 openfl 把 Stage3D 画进 `__backBufferTexture` 的 FBO，`rt` 因此通常不是 0。字段口径仍严格按 spec：取 `__contextState.__currentGLFramebuffer.id`，两侧都用 GL FBO name 才能对上。
7. **clear 不进摘要**：`DIGEST_FIELDS` 不含 clearColor/clearMask，所以仅颜色不同的两条 clear 会得到相同 `sum`。这是 spec 设计，未擅自扩展。
8. **AGAL 的 `slots` 计数**：取 `__agalSamplerUsageMask` 的置位数，未把 AGAL 转换生成的 `_alpha` sampler uniform 计入。
   若 native 的 `gl_shader_sampler_count` 把 alpha sampler 也算进列表，AGAL 路径的 `slots` 长度会不一致；
   本游戏着色器全部走 GLSL，不受影响。

---

## 5. 离线验证（可复现）

```powershell
# 1) 编译（不需要 lime，只用 Haxe 标准库）
haxe -cp vendor/openfl/src -cp vendor/openfl/test/vkpkt -main TestVKPkt -python out/test_vkpkt.py
python out/test_vkpkt.py > out/stdout.txt 2> out/stderr.txt
Get-Content out/stdout.txt, out/stderr.txt | Set-Content out/test_vkpkt.log

# 2) 核验（线偏移/宽度、字段顺序、独立重算 sum、场景口径、vkaudit recon）
python vendor/openfl/test/vkpkt/check_packets.py out/test_vkpkt.log tools/vkaudit/vkaudit.py
```

当前结果：**12 个场景 / 12 条 `[vkpkt]`**，`vkpkt=12 raw=12 case=12`，`vkaudit recon` 输出 `OK`
（对上 12 / 缺失 0 / 多余 0 / 顺序 0 / 摘要不符 0，两侧格式不符 0）。
覆盖：索引/非索引、alpha/预乘/加法/减法/未知混合、depth 开/关、scissor 开/关/Y 翻转、
RTT（selector/aa/depthStencil）、显式 clear、隐式 clear、空槽位、AGAL progFormat，
以及两处 native 对账修正的回归用例：

- `S11_program_switch_slots`：`__state.textures` 残留 3 个寄存器、当前 program 只声明 2 个 sampler。
  新口径 `slots=2:3`；同一条用例里打印旧口径基线 `slots=2:3:4` 并断言两者不同——旧实现必然多一个尾项。
- `S12_clear_after_blend_change`：两次 flush 之间把 blend 改为 `add`、program 切到 8 后 clear。
  新口径 `prog=8 blend=add`；用同一行的其余字段重建旧口径（`prog=3 blend=alpha`）并断言 `sum` 不同
  （旧 `sum=c1a957113a2c`，新 `sum=6d7b6cab1195`）——旧实现会与 native 的 GL 镜像不符。

另：`VKPktNative.hx` 中 `lime_vk_pkt_emit` 的注释已按 spec §2 改为“消费 packet 数；<0 = 错误码”。

**真实验证缺口**（如实说明）：
- 没有跑真实游戏；Context3D 的取数胶水（`__state`/`__contextState` 的实际值）只做了 cpp 类型检查，未做运行时比对；
- native 四个符号尚不存在，因此 `lime_vk_pkt_emit` 的真实提交没有跑过；
- `clear` 的 mask 强制、`setProgram` 覆盖等“依赖 GL 生命周期”的行为，离线测试只能验证编码后的事实，不能验证发生时序。
- 新增的“clear 前 `__flushGL()`”是 **Context3D 运行时行为**，离线用例只能证明记录口径（截断后的 `slots`、flush 后的 `prog/blend`）正确，
  不能证明真实 GL 在 clear 前确实被刷过；这部分依赖 cpp 类型检查（开关开/关均通过）与后续真机对账。
