# `vk_tuning.ini`：不重编就能改的渲染参数

对象：`angle_full\bin\lime.ndll`（Vulkan 翻译层）里的 `vk_tuning` 模块。
用途：把"每次遇到显示问题就得重编 ndll + 等 CI"改成"改一行 ini、重启游戏（1~2 秒）"。

## 0. 放在哪、怎么生效

- 文件路径：**exe 同目录** `vk_tuning.ini`（即 `angle_full\bin\vk_tuning.ini`）。
- 缺失 / 读不到 / 某行写错：**不报错、不阻断启动**，该键回落到内置默认。
- 只读一次（进程内首次 Vulkan Init），**改文件后重启游戏生效**。不做热重载：采样器和管线在纹理/设备初始化时建，热重载要重建全部采样器与管线，收益小于风险，还会引入"改了一半生效"的中间态。
- 安卓：`ExeDir` 为空（进程在 `/system/bin`），**不读 ini，全部走默认**。安卓要调只能改默认值或后续加 assets 路径。

启动时一定会在 stderr 打一行生效值，拿它当唯一事实来源：

```
[vk] tuning src=vk_tuning.ini texture.filter=nearest texture.filter.by_size=64:nearest texture.filter.by_glname=- texture.wrap=gl texture.wrap.by_size=- blend=srcalpha depth.test=keep depth.write=keep stencil.test=keep viewport=keep scissor=keep mip=off unknown=-
```

- `src=default` 表示没读到文件（此时全部默认）。
- `unknown=` 里会列出**没识别或解析失败**的键值，用来排除"拼错键名所以没生效"。
- 逐纹理的实际采样器值看 `[sampler] tex=.. gl(...) eff(...) ts=...`：`gl=` 是 GL 记录的原值，`eff=` 是 ini 合成后的实际值，两者不同才说明 ini 真的起了作用。

## 1. 字段表

| 键 | 取值 | 默认 | 当前是否生效 | 说明 |
|---|---|---|---|---|
| `texture.filter` | `gl` \| `nearest` \| `linear` | `gl` | ✅ | 全局过滤覆盖。`gl` = 沿用 GL 记录值（等于加本模块前的行为） |
| `texture.filter.by_size` | `最大边:模式` 逗号分隔，如 `64:nearest,256:linear` | 空 | ✅ | 按纹理尺寸覆盖，`max(w,h) <= 最大边` 时命中，**顺序从前到后，先命中先用** |
| `texture.filter.by_glname` | `GL纹理名:nearest` 逗号分隔，如 `12,0` | 空 | ✅ | 按 GL 纹理名强制 NEAREST（见 §4 限制） |
| `texture.wrap` | `gl` \| `clamp` \| `repeat` \| `mirror` | `gl` | ✅ | 全局 wrap 覆盖 |
| `texture.wrap.by_size` | `最大边:模式` | 空 | ✅ | 同上，按尺寸覆盖 wrap |
| `blend` | `srcalpha` \| `premul` \| `additive` \| `off` | `srcalpha` | ✅（需重启） | 混合变体。`srcalpha` = SRC_ALPHA / 1-SRC_ALPHA（加本模块前的写死值） |
| `depth.test` | `keep` \| `on` \| `off` | `keep` | ⚠️ 仅状态 | 目前渲染目标没有深度附件，改它只影响 `[pkt] draw ... state=` 里的记录位，不影响画面 |
| `depth.write` | `keep` \| `on` \| `off` | `keep` | ⚠️ 仅状态 | 同上 |
| `stencil.test` | `keep` \| `on` \| `off` | `keep` | ⚠️ 仅状态 | 同上 |
| `viewport` | `keep` \| `full` | `keep` | ✅ | `keep` = 用绑定这一刻的 GL 视口；`full` = 强制整 target 全幅（怀疑视口残留时用） |
| `scissor` | `keep` \| `full` | `keep` | ✅ | 同上，作用于裁剪框 |
| `mip` | `off` \| `on` | `off` | ⚠️ 半生效 | `off` = 不请求生成 mip；`on` = 走生成请求。但纹理只分配 1 级，目前不会真的多出 level（保留位） |

`#` 和 `;` 开头是注释；`[section]` 行会被忽略（写成 INI 风格不会报错）。

## 2. 症状 → 先调哪个键

原则：**先 `vkaudit` 对账定位到第几帧第几笔，再决定调 ini 还是改代码。能调 ini 的不改代码。**

| 症状 | 先试 | 预期 |
|---|---|---|
| 像素画 / UI 图糊、边缘发虚 | `texture.filter = nearest`，或 `texture.filter.by_size = 64:nearest` | 锐利、方块感回来；`[sampler]` 的 `eff=` 变 NEAREST |
| 大图（天空、背景）出现明显色块 | `texture.filter = linear` | 平滑；若反而更糊说明是分辨率问题不是过滤问题 |
| 精灵边缘出现一圈黑边 | `texture.wrap = clamp`（默认已是 clamp 兜底时，查 `mip` 与预乘） | 黑边消失 |
| 平铺背景出现接缝 | `texture.wrap = repeat` | 接缝消失；若出现边缘拉伸说明该图不该 repeat |
| 图层叠加发白 / 透明处发灰 | `blend = premul`；再试 `blend = srcalpha` | 用错变体的那一项会明显变好 |
| 粒子 / 光效太暗，想要叠加发光 | `blend = additive` | 亮部叠加；暗部不再压暗背景 |
| 整屏出现不该有的遮挡/缺角 | `scissor = full` | 若缺角消失 → 记录里的 scissor 有误，回到记录侧查 |
| resize 后只画左上角一块 | `viewport = full` | 若铺满 → 视口状态取错；仍只画一块 → 查投影重算与 extent 同步 |
| 想验证某张贴图 | `texture.filter.by_glname = <编号>:nearest`，编号从 `[sampler] tex=` 抄 | 只影响该贴图 |

## 3. 三条纪律

1. **默认值 = 加本模块前的写死值**。任何"默认行为变了"的怀疑，先把 `vk_tuning.ini` 挪走重启，看 `[vk] tuning src=default`，此时行为必须与旧版逐字一致。
2. **一次只改一个键**，改完看 `[vk] tuning` 行确认真的被读到（不是 `unknown=`），再看画面。多项一起改，出问题无法归因。
3. 调参救不了的，再改代码；改代码要重编 ndll（CI），成本高一个数量级，所以先穷尽 ini + `vkaudit`。

## 4. 已知限制

- **没有文件路径，只有 GL 纹理名**。记录侧（`gl_texture_params.cpp` / `[sampler]`）能拿到的是 GL 纹理编号和尺寸，拿不到贴图文件名，所以"按名字前缀覆盖"落地成 `texture.filter.by_glname`（按编号）。要按文件名，需要在纹理上传路径上把文件名带下来，属于记录侧改动。
- **不做逐 draw 的 blend**。混合烘在管线里，`blend=` 是全局单值；按 draw 的 blend 差异要等管线变体，不在本模块范围。
- **深度/模板是预留位**。没有深度附件时这两项只能是"记录状态"，不是"改变画面"。
- 安卓不读 ini（§0）。

## 5. 审计口径（离线优先）

1. 先跑 `tools/vkaudit` 对账：定位到具体帧号 → 笔序号 → 该笔的纹理 / 状态 / 顶点范围（见 `docs/draw_packet_spec.md`）。
2. 判断是"缺一笔"、"状态错"、还是"采样错"：
   - 缺笔 → 记录侧/执行侧，不看 ini；
   - 采样错（糊、黑边、接缝）→ 优先 ini 的 filter / wrap / mip；
   - 叠加错（发白、发灰）→ 优先 ini 的 blend；
   - 位置/裁切错 → 优先 viewport / scissor 策略，再查投影重算。
3. 只有在 ini 里找不到对应开关时，才动代码；动代码前先把当前 ini 抄进结论，避免"配置和代码一起变"。
