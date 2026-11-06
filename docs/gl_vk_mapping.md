# GL → Vulkan 能力映射与覆盖矩阵

> 基线：`mulinwu1/Mario-Madness` @ `main` / `af103da`（2026-11-06T14:24:52Z）
> 扫描对象：`native_vulkan/gl_compat.h`（269 个 `lime_gl_*` 入口）、`native_vulkan/gl_core.cpp`、`native_vulkan/gl_stubs.cpp`
> 本表由 `tools/coverage/coverage.ps1` 统计生成骨架、人工核对语义后填写；状态列以脚本实际扫描结果为准。

## 1. 覆盖总览

| 模块 | 入口数 | 真实现 | 空桩 | 未覆盖 | 真实现覆盖率 | 有落点覆盖率 |
| --- | --- | --- | --- | --- | --- | --- |
| 状态 | 30 | 8 | 22 | 0 | 26.7% | 100.0% |
| 纹理 | 27 | 5 | 22 | 0 | 18.5% | 100.0% |
| 缓冲/顶点 | 55 | 10 | 45 | 0 | 18.2% | 100.0% |
| shader/program | 77 | 0 | 77 | 0 | 0.0% | 100.0% |
| FBO/renderbuffer | 24 | 0 | 24 | 0 | 0.0% | 100.0% |
| 其他 | 56 | 0 | 56 | 0 | 0.0% | 100.0% |
| **合计** | **269** | **23** | **246** | **0** | **8.6%** | **100.0%** |

复跑命令：

```powershell
powershell -ExecutionPolicy Bypass -File tools\coverage\coverage.ps1            # 汇总表
powershell -ExecutionPolicy Bypass -File tools\coverage\coverage.ps1 -Gaps      # 缺口清单
powershell -ExecutionPolicy Bypass -File tools\coverage\coverage.ps1 -List      # 逐条明细
powershell -ExecutionPolicy Bypass -File tools\coverage\coverage.ps1 -Markdown  # 可贴回本文件的汇总表
```

## 2. 读取本表的口径

| 状态 | 含义 |
| --- | --- |
| 真实现 | 有非空函数体，且数据/状态确实被创建或消费 |
| 真实现(记录) | 有非空函数体，但只写入 shim 自有的静态变量，当前渲染管线并不读取，改它不会改变画面 |
| 真实现(CPU) | 有非空函数体，但数据只存在 `std::vector` 等 CPU 容器里，没有创建 `VkBuffer`，GPU 侧无对应资源 |
| 空桩 | 定义存在于 `gl_stubs.cpp`，函数体为空或仅 `return 0/false`，调用被静默吞掉 |
| 未覆盖 | 头文件声明了，但任何 `*.cpp` 中都找不到定义（当前为 0） |

> 注意：23 个"真实现"里，真正进入 Vulkan 命令流的只有 `clear/clear_color`（`vkCmdBeginRenderPass` 的 `loadOp=CLEAR`）、`tex_image_2d`（`VkRenderer::CreateTexture`）、`draw_arrays`/`draw_elements`（`VkRenderer::DrawRaw`）、`delete_texture`/`delete_buffer`（销毁路径）。其余多为状态记录或纯 CPU 容器，见第 9 节。

## 3. 状态（30）

| 入口 | GL 语义 | Vulkan 落点 | 状态 | 文件 |
| --- | --- | --- | --- | --- |
| lime_gl_enable | 开启能力（仅 `GL_BLEND=0x0BE2`） | 记录 `g_blendEnabled`；落点为 `VkPipelineColorBlendAttachmentState.blendEnable`（pipeline 创建时固定，改值需重建 pipeline） | 真实现(记录) | gl_core.cpp |
| lime_gl_disable | 关闭能力（仅 BLEND） | 同上，`blendEnable = VK_FALSE` | 真实现(记录) | gl_core.cpp |
| lime_gl_blend_func | 设置混合因子 | 记录 `g_blendSrc/g_blendDst`；落点 `srcColorBlendFactor/dstColorBlendFactor`（需 GL 枚举→`VkBlendFactor` 映射表） | 真实现(记录) | gl_core.cpp |
| lime_gl_blend_func_separate | RGB 与 Alpha 分别设混合因子 | `srcColorBlendFactor/srcAlphaBlendFactor/dstColorBlendFactor/dstAlphaBlendFactor` | 空桩 | gl_stubs.cpp |
| lime_gl_blend_equation | 设置混合方程 | `colorBlendOp/alphaBlendOp`（`GL_FUNC_ADD/SUBTRACT/REVERSE_SUBTRACT/MIN/MAX` → `VkBlendOp`） | 空桩 | gl_stubs.cpp |
| lime_gl_blend_equation_separate | RGB/Alpha 分别设方程 | `colorBlendOp/alphaBlendOp` 分离赋值 | 空桩 | gl_stubs.cpp |
| lime_gl_blend_color | 设置常量混合色 | `vkCmdSetBlendConstants`（或 `VkPipelineColorBlendStateCreateInfo.blendConstants`；需 `VK_DYNAMIC_STATE_BLEND_CONSTANTS` 或 `VK_EXT_blend_operation_advanced`） | 空桩 | gl_stubs.cpp |
| lime_gl_clear | 按 mask 清屏 | `VkRenderer::BeginFrame` → render pass `loadOp=CLEAR`；细分 mask 需 `vkCmdClearAttachments`（color/depth/stencil 分别清） | 真实现 | gl_core.cpp |
| lime_gl_clear_color | 设置清屏色 | `VkClearValue.color`，由 `BeginFrame(r,g,b,a)` 消费 | 真实现 | gl_core.cpp |
| lime_gl_clear_depthf | 设置/清除深度值 | `VkClearDepthStencilValue.depth` + `VkAttachmentDescription.loadOp=CLEAR`（需先有深度附件） | 空桩 | gl_stubs.cpp |
| lime_gl_clear_stencil | 设置/清除模板值 | `VkClearDepthStencilValue.stencil` + stencil `loadOp=CLEAR` | 空桩 | gl_stubs.cpp |
| lime_gl_color_mask | 颜色写掩码 | `VkPipelineColorBlendAttachmentState.colorWriteMask`（`VK_COLOR_COMPONENT_*_BIT`） | 真实现(记录) | gl_core.cpp |
| lime_gl_cull_face | 背面剔除模式 | `VkPipelineRasterizationStateCreateInfo.cullMode`（GL_BACK→`VK_CULL_MODE_BACK_BIT`，GL_FRONT 需配合 `frontFace` 反转） | 空桩 | gl_stubs.cpp |
| lime_gl_front_face | 正面绕序 | `rasterization.frontFace`（`VK_FRONT_FACE_COUNTER_CLOCKWISE/CLOCKWISE`） | 空桩 | gl_stubs.cpp |
| lime_gl_depth_func | 深度比较函数 | `VkPipelineDepthStencilStateCreateInfo.depthCompareOp`（`VkCompareOp`） | 空桩 | gl_stubs.cpp |
| lime_gl_depth_mask | 深度写开关 | `depthWriteEnable` | 空桩 | gl_stubs.cpp |
| lime_gl_depth_rangef | 深度范围 | `VkViewport.minDepth/maxDepth`（动态状态） | 空桩 | gl_stubs.cpp |
| lime_gl_scissor | 设置裁剪矩形 | 记录值；落点 `vkCmdSetScissor` + `rasterization.scissorTestEnable`（需 `VK_DYNAMIC_STATE_SCISSOR`；注意 Y 轴换算） | 真实现(记录) | gl_core.cpp |
| lime_gl_viewport | 设置视口 | 记录值；落点 `vkCmdSetViewport`（需 `VK_DYNAMIC_STATE_VIEWPORT`；GL 原点左下，Vulkan 左上） | 真实现(记录) | gl_core.cpp |
| lime_gl_line_width | 线宽 | `rasterization.lineWidth`（>1 需 `wideLines` 特性，多数移动 GPU 只支持 1.0） | 空桩 | gl_stubs.cpp |
| lime_gl_polygon_offset | 多边形偏移 | `rasterization.depthBiasEnable/depthBiasConstantFactor/depthBiasSlopeFactor` | 空桩 | gl_stubs.cpp |
| lime_gl_sample_coverage | 覆盖率/反转 | 无直接对应；MSAA 由 `VkPipelineMultisampleStateCreateInfo` 固定，需 `VK_EXT_sample_locations` 才可近似 | 空桩 | gl_stubs.cpp |
| lime_gl_hint | 行为提示 | Vulkan 无对应概念（`GL_LINE_SMOOTH_HINT` 等），可安全忽略 | 空桩 | gl_stubs.cpp |
| lime_gl_is_enabled | 查询能力是否开启 | 读 shim 侧状态表即可，无需 Vk 对象 | 空桩 | gl_stubs.cpp |
| lime_gl_stencil_func | 模板比较 | `VkStencilOpState.compareOp/compareMask/reference` | 空桩 | gl_stubs.cpp |
| lime_gl_stencil_func_separate | 按面设置模板比较 | 前/后两个 `VkStencilOpState` | 空桩 | gl_stubs.cpp |
| lime_gl_stencil_mask | 模板写掩码 | `VkStencilOpState.writeMask` | 空桩 | gl_stubs.cpp |
| lime_gl_stencil_mask_separate | 按面设置写掩码 | 前/后 `writeMask` | 空桩 | gl_stubs.cpp |
| lime_gl_stencil_op | 模板操作 | `VkStencilOpState.failOp/passOp/depthFailOp`（`VkStencilOp`） | 空桩 | gl_stubs.cpp |
| lime_gl_stencil_op_separate | 按面设置模板操作 | 前/后 `failOp/passOp/depthFailOp` | 空桩 | gl_stubs.cpp |

## 4. 纹理（27）

| 入口 | GL 语义 | Vulkan 落点 | 状态 | 文件 |
| --- | --- | --- | --- | --- |
| lime_gl_create_texture | 创建纹理名 | 仅分配 GL 名字并占位 `g_texVk`；Vk 侧对象延迟到首次上传（`VkRenderer::CreateTexture`） | 真实现 | gl_core.cpp |
| lime_gl_bind_texture | 绑定纹理到当前 unit | 记录 `g_texBound[unit]`；落点为 descriptor set（`VkDescriptorImageInfo{imageView,sampler}`） | 真实现(记录) | gl_core.cpp |
| lime_gl_active_texture | 选择活动纹理单元 | 记录 `g_activeUnit`（仅接受 `>=0x84C0`）；落点为 descriptor set 数组下标 | 真实现(记录) | gl_core.cpp |
| lime_gl_delete_texture | 删除纹理 | `VkRenderer::DestroyTexture` → `vkDestroyImageView/vkDestroyImage/vkFreeMemory/vkFreeDescriptorSets` | 真实现 | gl_core.cpp |
| lime_gl_tex_image_2d | 上传 2D 纹理（仅 level 0、RGB/RGBA8） | `VkRenderer::CreateTexture`：`VkImage(DEVICE_LOCAL)` + staging `VkBuffer` + `vkCmdCopyBufferToImage` + `VkImageView` + descriptor | 真实现 | gl_core.cpp |
| lime_gl_tex_sub_image_2d | 局部更新纹理区域 | `vkCmdCopyBufferToImage` 的 `VkBufferImageCopy.imageOffset/imageExtent` + layout 切换（`TRANSFER_DST` ↔ `SHADER_READ`） | 空桩 | gl_stubs.cpp |
| lime_gl_tex_sub_image_3d | 3D 纹理局部更新 | 同上，`imageSubresource.baseArrayLayer/layerCount` 表达 z 偏移 | 空桩 | gl_stubs.cpp |
| lime_gl_tex_image_3d | 上传 3D 纹理 | `VkImage` 带 `VK_IMAGE_TYPE_3D` + `VkBufferImageCopy.extent.depth` | 空桩 | gl_stubs.cpp |
| lime_gl_tex_storage_2d | 预分配 2D 纹理存储 | `vkCreateImage`（不可变存储） / `vkBindImageMemory`；后续用 `texSubImage` 填充 | 空桩 | gl_stubs.cpp |
| lime_gl_tex_storage_3d | 预分配 3D 纹理存储 | 同上，`VK_IMAGE_TYPE_3D` | 空桩 | gl_stubs.cpp |
| lime_gl_copy_tex_image_2d | 从当前 read framebuffer 拷贝为纹理 | `vkCmdCopyImage`/`vkCmdBlitImage` + 两步 layout barrier | 空桩 | gl_stubs.cpp |
| lime_gl_copy_tex_sub_image_2d | 拷贝子区域到纹理 | `vkCmdCopyImage` 的 `VkImageCopy` 区域 + barrier | 空桩 | gl_stubs.cpp |
| lime_gl_copy_tex_sub_image_3d | 拷贝子区域到 3D 纹理 | `VkImageCopy.extent.depth` | 空桩 | gl_stubs.cpp |
| lime_gl_compressed_tex_image_2d | 上传压缩纹理（ETC/ASTC 等） | `VkImage` 用对应 `VK_FORMAT_ETC2_*`/`VK_FORMAT_ASTC_*` + `vkCmdCopyBufferToImage`（块对齐） | 空桩 | gl_stubs.cpp |
| lime_gl_compressed_tex_image_3d | 上传 3D 压缩纹理 | 同上 + `extent.depth` | 空桩 | gl_stubs.cpp |
| lime_gl_compressed_tex_sub_image_2d | 压缩纹理局部更新 | `VkBufferImageCopy` 按块对齐的 offset/extent | 空桩 | gl_stubs.cpp |
| lime_gl_compressed_tex_sub_image_3d | 3D 压缩纹理局部更新 | 同上 + `baseArrayLayer` | 空桩 | gl_stubs.cpp |
| lime_gl_generate_mipmap | 生成 mipmap 链 | `vkCmdBlitImage` 逐级下采样（或 `vkCmdCopyImage`）；目标 image 需 `mipLevels>1` 且每级切 layout | 空桩 | gl_stubs.cpp |
| lime_gl_tex_parameteri | 设置过滤/包裹模式 | 需重建 `VkSampler`（`VkSamplerCreateInfo.magFilter/minFilter/mipmapMode/addressModeU/V/W`） | 空桩 | gl_stubs.cpp |
| lime_gl_tex_parameterf | 同上（浮点参数，如各向异性/LOD bias） | `maxAnisotropy/mipLodBias/minLod/maxLod` | 空桩 | gl_stubs.cpp |
| lime_gl_get_tex_parameteri | 读纹理参数 | 读 shim 侧参数副本（Vulkan 无查询接口） | 空桩 | gl_stubs.cpp |
| lime_gl_get_tex_parameterf | 读纹理参数（浮点） | 同上 | 空桩 | gl_stubs.cpp |
| lime_gl_get_tex_parameterfv | 读纹理参数（向量） | 同上 | 空桩 | gl_stubs.cpp |
| lime_gl_get_tex_parameteriv | 读纹理参数（整型向量） | 同上 | 空桩 | gl_stubs.cpp |
| lime_gl_is_texture | 是否为有效纹理名 | 查 shim 名字表，无需 Vk 对象 | 空桩 | gl_stubs.cpp |
| lime_gl_pixel_storei | 像素存储对齐/行序 | 影响 `VkBufferImageCopy.bufferRowLength/bufferImageHeight`，以及解包时的行对齐 | 空桩 | gl_stubs.cpp |
| lime_gl_get_internalformativ | 查询内部格式能力 | 查 `vkGetPhysicalDeviceFormatProperties` 结果并做 GL 枚举映射 | 空桩 | gl_stubs.cpp |

## 5. 缓冲 / 顶点（55）

### 5.1 Buffer 对象

| 入口 | GL 语义 | Vulkan 落点 | 状态 | 文件 |
| --- | --- | --- | --- | --- |
| lime_gl_create_buffer | 创建 buffer 名 | 仅分配 GL 名字；Vk 侧在首次 `bufferData` 时创建 `VkBuffer` | 真实现 | gl_core.cpp |
| lime_gl_bind_buffer | 绑定 ARRAY/ELEMENT buffer | 记录 `g_boundArrayBuffer/g_boundElementBuffer` | 真实现(记录) | gl_core.cpp |
| lime_gl_buffer_data | 上传顶点/索引数据 | 当前仅存入 CPU `std::vector`；落点应为 `VkBuffer(VERTEX/INDEX)` + `VK_MEMORY_PROPERTY_DEVICE_LOCAL` 或 `HOST_VISIBLE` staging | 真实现(CPU) | gl_core.cpp |
| lime_gl_buffer_sub_data | 更新 buffer 局部 | 同上的 CPU 侧覆盖；落点 `vkCmdUpdateBuffer` 或 mapped staging + `vkCmdCopyBuffer` | 真实现(CPU) | gl_core.cpp |
| lime_gl_delete_buffer | 删除 buffer | CPU 容器清空；落点 `vkDestroyBuffer/vkFreeMemory` | 真实现 | gl_core.cpp |
| lime_gl_copy_buffer_sub_data | buffer 间拷贝 | `vkCmdCopyBuffer`（`VkBufferCopy` 的 srcOffset/dstOffset/size） | 空桩 | gl_stubs.cpp |
| lime_gl_bind_buffer_base | 绑定 UBO/SSBO 到 binding 点 | uniform: `vkCmdBindDescriptorSets`；storage: 同 + `VK_DESCRIPTOR_TYPE_STORAGE_BUFFER` | 空桩 | gl_stubs.cpp |
| lime_gl_bind_buffer_range | 绑定 buffer 区间 | `VkDescriptorBufferInfo{offset,range}` 写入 descriptor set | 空桩 | gl_stubs.cpp |
| lime_gl_clear_bufferfi | 清 buffer 为浮点/整型值 | `vkCmdFillBuffer`（仅 32 位值，64 位需 compute shader） | 空桩 | gl_stubs.cpp |
| lime_gl_clear_bufferfv | 浮点向量填 buffer | `vkCmdFillBuffer` 逐 dword 或 compute shader | 空桩 | gl_stubs.cpp |
| lime_gl_clear_bufferiv | 整型向量填 buffer | 同上 | 空桩 | gl_stubs.cpp |
| lime_gl_clear_bufferuiv | 无符号整型向量填 buffer | 同上 | 空桩 | gl_stubs.cpp |
| lime_gl_map_buffer_range | 映射 buffer 区间 | `vkMapMemory` 返回指针（需 `HOST_VISIBLE` 内存，非 DEVICE_LOCAL） | 空桩 | gl_stubs.cpp |
| lime_gl_unmap_buffer | 解除映射 | `vkUnmapMemory` + 必要时 flush（非 `HOST_COHERENT` 时） | 空桩 | gl_stubs.cpp |
| lime_gl_get_buffer_sub_data | 回读 buffer 数据 | `vkCmdCopyBuffer` 到 host-visible staging + fence 等待 | 空桩 | gl_stubs.cpp |
| lime_gl_get_buffer_parameteri | 查询 buffer 参数 | 读 shim 元数据（size/usage），Vulkan 无查询 | 空桩 | gl_stubs.cpp |
| lime_gl_get_buffer_parameteriv | 同上（数组出参） | 同上 | 空桩 | gl_stubs.cpp |
| lime_gl_get_buffer_parameteri64v | 同上（64 位） | 同上 | 空桩 | gl_stubs.cpp |
| lime_gl_get_buffer_pointerv | 查询映射指针 | 对应 `vkMapMemory` 缓存的指针副本 | 空桩 | gl_stubs.cpp |
| lime_gl_is_buffer | 是否为有效 buffer | 查 shim 名字表 | 空桩 | gl_stubs.cpp |

### 5.2 Vertex Array（VAO）

| 入口 | GL 语义 | Vulkan 落点 | 状态 | 文件 |
| --- | --- | --- | --- | --- |
| lime_gl_create_vertex_array | 创建 VAO | shim 侧属性集合；落点为 `VkVertexInputBindingDescription` + `VkVertexInputAttributeDescription`（pipeline 创建时固定） | 空桩 | gl_stubs.cpp |
| lime_gl_bind_vertex_array | 绑定 VAO | 切换当前属性集合；落点为绘制时使用的 vertex input 配置 | 空桩 | gl_stubs.cpp |
| lime_gl_delete_vertex_array | 删除 VAO | 释放 shim 属性集合 | 空桩 | gl_stubs.cpp |
| lime_gl_is_vertex_array | 是否为有效 VAO | 查 shim 名字表 | 空桩 | gl_stubs.cpp |

### 5.3 顶点属性

| 入口 | GL 语义 | Vulkan 落点 | 状态 | 文件 |
| --- | --- | --- | --- | --- |
| lime_gl_vertex_attrib_pointer | 设置属性格式/偏移 | 记录 `g_attrs[i]`；落点 `VkVertexInputAttributeDescription{location,format,offset}` + binding `stride/inputRate` | 真实现(记录) | gl_core.cpp |
| lime_gl_enable_vertex_attrib_array | 启用属性数组 | 记录 `enabled`；落点为 attribute description 是否出现在 pipeline 中 | 真实现(记录) | gl_core.cpp |
| lime_gl_disable_vertex_attrib_array | 禁用属性数组 | 同上 | 真实现(记录) | gl_core.cpp |
| lime_gl_vertex_attrib_divisor | 实例化步进 | `VkVertexInputBindingDescription.inputRate`（`INSTANCE`/`VERTEX`）+ `VK_EXT_vertex_attribute_divisor` | 空桩 | gl_stubs.cpp |
| lime_gl_vertex_attrib_ipointer | 整型属性指针 | attribute description 的整型 `VkFormat`（`R*_SINT/UINT`） | 空桩 | gl_stubs.cpp |
| lime_gl_get_vertex_attrib_pointerv | 查询属性指针 | 读 shim 属性集合 | 空桩 | gl_stubs.cpp |
| lime_gl_vertex_attrib1f | 设置 1 分量常量属性 | 无 VkBuffer，需通过 push constant / UBO 传入 | 空桩 | gl_stubs.cpp |
| lime_gl_vertex_attrib1fv | 同上（数组形式） | 同上 | 空桩 | gl_stubs.cpp |
| lime_gl_vertex_attrib2f | 设置 2 分量常量属性 | push constant / UBO（`vec2` 8 字节对齐） | 空桩 | gl_stubs.cpp |
| lime_gl_vertex_attrib2fv | 同上（数组形式） | 同上 | 空桩 | gl_stubs.cpp |
| lime_gl_vertex_attrib3f | 设置 3 分量常量属性 | push constant / UBO（std140 `vec3` 占 16 字节） | 空桩 | gl_stubs.cpp |
| lime_gl_vertex_attrib3fv | 同上（数组形式） | 同上 | 空桩 | gl_stubs.cpp |
| lime_gl_vertex_attrib4f | 设置 4 分量常量属性 | push constant / UBO（`vec4` 16 字节） | 空桩 | gl_stubs.cpp |
| lime_gl_vertex_attrib4fv | 同上（数组形式） | 同上 | 空桩 | gl_stubs.cpp |
| lime_gl_vertex_attribi4i | 设置整型常量属性 | push constant（有符号整型） | 空桩 | gl_stubs.cpp |
| lime_gl_vertex_attribi4iv | 同上（数组形式） | 同上 | 空桩 | gl_stubs.cpp |
| lime_gl_vertex_attribi4ui | 设置无符号整型常量属性 | push constant（无符号整型） | 空桩 | gl_stubs.cpp |
| lime_gl_vertex_attribi4uiv | 同上（数组形式） | 同上 | 空桩 | gl_stubs.cpp |
| lime_gl_get_vertex_attribf | 查询常量属性 | 读 shim 副本 | 空桩 | gl_stubs.cpp |
| lime_gl_get_vertex_attribfv | 查询常量属性（向量） | 读 shim 副本 | 空桩 | gl_stubs.cpp |
| lime_gl_get_vertex_attribi | 查询整型常量属性 | 读 shim 副本 | 空桩 | gl_stubs.cpp |
| lime_gl_get_vertex_attribiv | 查询整型常量属性（向量） | 读 shim 副本 | 空桩 | gl_stubs.cpp |
| lime_gl_get_vertex_attribii | 查询 64 位整型常量属性 | 读 shim 副本 | 空桩 | gl_stubs.cpp |
| lime_gl_get_vertex_attribiiv | 查询 64 位整型常量属性（向量） | 读 shim 副本 | 空桩 | gl_stubs.cpp |
| lime_gl_get_vertex_attribiui | 查询无符号 64 位常量属性 | 读 shim 副本 | 空桩 | gl_stubs.cpp |
| lime_gl_get_vertex_attribiuiv | 查询无符号 64 位常量属性（向量） | 读 shim 副本 | 空桩 | gl_stubs.cpp |

### 5.4 绘制

| 入口 | GL 语义 | Vulkan 落点 | 状态 | 文件 |
| --- | --- | --- | --- | --- |
| lime_gl_draw_arrays | 非索引绘制 | CPU 组装 pos/uv → `VkRenderer::DrawRaw` → `vkCmdDraw`（固定 `TRIANGLE_LIST`，仅支持 attrib0=pos、attrib1=uv、stride 默认 16） | 真实现 | gl_core.cpp |
| lime_gl_draw_elements | 索引绘制 | CPU 按索引展开后 `DrawRaw`；未走 `vkCmdDrawIndexed`/`VK_INDEX_TYPE_UINT16` | 真实现 | gl_core.cpp |
| lime_gl_draw_range_elements | 限定索引范围绘制 | CPU 展开 + `vkCmdDrawIndexed`（`firstIndex/vertexOffset`） | 空桩 | gl_stubs.cpp |
| lime_gl_draw_arrays_instanced | 实例化非索引绘制 | `vkCmdDraw(instanceCount, firstInstance)`；vertex input `inputRate=INSTANCE` | 空桩 | gl_stubs.cpp |
| lime_gl_draw_elements_instanced | 实例化索引绘制 | `vkCmdDrawIndexed(instanceCount)` | 空桩 | gl_stubs.cpp |

## 6. shader / program（77）

| 入口 | GL 语义 | Vulkan 落点 | 状态 | 文件 |
| --- | --- | --- | --- | --- |
| lime_gl_create_shader | 创建 shader 对象 | shim 保存 GLSL 源；Vk 侧为 `VkShaderModule`（需 GLSL→SPIR-V） | 空桩 | gl_stubs.cpp |
| lime_gl_shader_source | 设置 shader 源码 | 存 GLSL 源，供翻译器消费；Vulkan 无运行时 GLSL | 空桩 | gl_stubs.cpp |
| lime_gl_compile_shader | 编译 shader | GLSL→SPIR-V（glslang/ANGLE shader translator）+ `vkCreateShaderModule`；错误需写入 infoLog | 空桩 | gl_stubs.cpp |
| lime_gl_create_program | 创建 program | `VkPipelineLayout` + `VkPipeline` 的容器 | 空桩 | gl_stubs.cpp |
| lime_gl_attach_shader | 挂载 shader | 记录关联；链接时合并为 graphics pipeline | 空桩 | gl_stubs.cpp |
| lime_gl_detach_shader | 卸载 shader | 解除关联 | 空桩 | gl_stubs.cpp |
| lime_gl_link_program | 链接 program | `vkCreateGraphicsPipelines`（含 vertex input / raster / blend / multisample 全部状态） | 空桩 | gl_stubs.cpp |
| lime_gl_use_program | 启用 program | `vkCmdBindPipeline` | 空桩 | gl_stubs.cpp |
| lime_gl_delete_shader | 删除 shader | `vkDestroyShaderModule` | 空桩 | gl_stubs.cpp |
| lime_gl_delete_program | 删除 program | `vkDestroyPipeline/vkDestroyPipelineLayout` | 空桩 | gl_stubs.cpp |
| lime_gl_validate_program | 校验 program 可用性 | 建 pipeline 时校验（`VkResult`），无独立校验接口 | 空桩 | gl_stubs.cpp |
| lime_gl_release_shader_compiler | 释放编译器资源 | 空操作（驱动器侧管理） | 空桩 | gl_stubs.cpp |
| lime_gl_get_shader_source | 取回 shader 源 | 读 shim 保存的 GLSL 源 | 空桩 | gl_stubs.cpp |
| lime_gl_get_shader_info_log | 取编译日志 | 读翻译器/`vkCreateShaderModule` 的错误字符串 | 空桩 | gl_stubs.cpp |
| lime_gl_get_program_info_log | 取链接日志 | 读 pipeline 创建错误字符串 | 空桩 | gl_stubs.cpp |
| lime_gl_get_shaderi | 查询 shader 参数 | 读 shim 元数据（类型/编译状态/源码长度） | 空桩 | gl_stubs.cpp |
| lime_gl_get_shaderiv | 同上（数组出参） | 同上 | 空桩 | gl_stubs.cpp |
| lime_gl_get_programi | 查询 program 参数 | 读 shim 元数据（链接状态/附件数/active uniform 数） | 空桩 | gl_stubs.cpp |
| lime_gl_get_programiv | 同上（数组出参） | 同上 | 空桩 | gl_stubs.cpp |
| lime_gl_get_attached_shaders | 查询挂载的 shader 列表 | 读 shim 关联表 | 空桩 | gl_stubs.cpp |
| lime_gl_is_shader | 是否为有效 shader | 查 shim 名字表 | 空桩 | gl_stubs.cpp |
| lime_gl_is_program | 是否为有效 program | 查 shim 名字表 | 空桩 | gl_stubs.cpp |
| lime_gl_get_active_uniform | 反射 active uniform | 由 SPIR-V 反射（`spirv-cross`/`glslang`）生成 shim 反射表 | 空桩 | gl_stubs.cpp |
| lime_gl_get_active_uniform_block_name | 反射 UBO 块名 | 同上 | 空桩 | gl_stubs.cpp |
| lime_gl_get_active_uniform_blocki | 查询 UBO 块属性 | 同上 | 空桩 | gl_stubs.cpp |
| lime_gl_get_active_uniform_blockiv | 同上（数组出参） | 同上 | 空桩 | gl_stubs.cpp |
| lime_gl_get_uniform_block_index | 按名取 UBO 块索引 | 反射表查找 | 空桩 | gl_stubs.cpp |
| lime_gl_bind_attrib_location | 预绑定属性位置 | `VkVertexInputAttributeDescription.location`（须在链接前固定） | 空桩 | gl_stubs.cpp |
| lime_gl_get_attrib_location | 查询属性位置 | 反射表查找 | 空桩 | gl_stubs.cpp |
| lime_gl_get_frag_data_location | 查询 frag 输出位置 | `VkPipelineColorBlendAttachmentState` 对应的 `location` | 空桩 | gl_stubs.cpp |
| lime_gl_get_uniform_location | 查询 uniform 位置 | 反射表查找（返回 shim 自增 id） | 空桩 | gl_stubs.cpp |
| lime_gl_get_active_uniformsiv | 批量查询 uniform 属性 | 反射表 | 空桩 | gl_stubs.cpp |
| lime_gl_get_shader_precision_format | 查询精度格式 | 静态返回（`VK_FORMAT_R32G32B32A32_SFLOAT` / `R16G16B16A16_SFLOAT`） | 空桩 | gl_stubs.cpp |
| lime_gl_program_parameteri | 设置 program 参数 | 建 pipeline 时消费（如 `GL_PROGRAM_BINARY_RETRIEVABLE_HINT`），Vulkan 侧无对应，记录即可 | 空桩 | gl_stubs.cpp |
| lime_gl_program_binary | 加载 program 二进制 | SPIR-V 二进制可直接 `vkCreateShaderModule`；GL 二进制格式不兼容，需忽略或缓存 SPIR-V | 空桩 | gl_stubs.cpp |
| lime_gl_get_program_binary | 导出 program 二进制 | 导出缓存的 SPIR-V | 空桩 | gl_stubs.cpp |
| lime_gl_shader_binary | 加载 shader 二进制 | 同上（SPIR-V 可直接用） | 空桩 | gl_stubs.cpp |
| lime_gl_uniform_block_binding | 绑定 UBO 块到 binding 点 | `VkDescriptorSetLayoutBinding.binding` + `vkCmdBindDescriptorSets` | 空桩 | gl_stubs.cpp |
| lime_gl_get_uniformf | 回读 1 分量 uniform | 读 shim 侧 uniform 影子副本（Vulkan 无法回读已提交 uniform） | 空桩 | gl_stubs.cpp |
| lime_gl_get_uniformfv | 回读 1 分量 uniform（数组） | 同上 | 空桩 | gl_stubs.cpp |
| lime_gl_get_uniformi | 回读整型 uniform | 读 shim 影子副本 | 空桩 | gl_stubs.cpp |
| lime_gl_get_uniformiv | 回读整型 uniform（数组） | 同上 | 空桩 | gl_stubs.cpp |
| lime_gl_get_uniformui | 回读无符号 uniform | 读 shim 影子副本 | 空桩 | gl_stubs.cpp |
| lime_gl_get_uniformuiv | 回读无符号 uniform（数组） | 同上 | 空桩 | gl_stubs.cpp |
| lime_gl_uniform1f | 上传 1 分量 float | `VkRenderer::SetUniform` → UBO 写入 / `vkCmdPushConstants` | 空桩 | gl_stubs.cpp |
| lime_gl_uniform1fv | 上传 1 分量 float 数组 | UBO 连续写入（`VkWriteDescriptorSet`） | 空桩 | gl_stubs.cpp |
| lime_gl_uniform1i | 上传 1 分量 int（常为 sampler） | UBO/push constant；sampler 需转为 descriptor 绑定 | 空桩 | gl_stubs.cpp |
| lime_gl_uniform1iv | 上传 1 分量 int 数组 | UBO 连续写入 | 空桩 | gl_stubs.cpp |
| lime_gl_uniform1ui | 上传 1 分量 uint | UBO/push constant（无符号） | 空桩 | gl_stubs.cpp |
| lime_gl_uniform1uiv | 上传 1 分量 uint 数组 | UBO 连续写入 | 空桩 | gl_stubs.cpp |
| lime_gl_uniform2f | 上传 2 分量 float（如分辨率） | UBO/push constant：`vec2`，注意 std140 8 字节对齐 | 空桩 | gl_stubs.cpp |
| lime_gl_uniform2fv | 上传 2 分量 float 数组 | UBO 连续写入（每元素 8 字节） | 空桩 | gl_stubs.cpp |
| lime_gl_uniform2i | 上传 2 分量 int | UBO/push constant（整型） | 空桩 | gl_stubs.cpp |
| lime_gl_uniform2iv | 上传 2 分量 int 数组 | UBO 连续写入 | 空桩 | gl_stubs.cpp |
| lime_gl_uniform2ui | 上传 2 分量 uint | UBO/push constant（无符号） | 空桩 | gl_stubs.cpp |
| lime_gl_uniform2uiv | 上传 2 分量 uint 数组 | UBO 连续写入 | 空桩 | gl_stubs.cpp |
| lime_gl_uniform3f | 上传 3 分量 float | `vec3` 在 std140 中占 16 字节，不能按 12 字节紧凑写 | 空桩 | gl_stubs.cpp |
| lime_gl_uniform3fv | 上传 3 分量 float 数组 | UBO 连续写入（每元素占 16 字节） | 空桩 | gl_stubs.cpp |
| lime_gl_uniform3i | 上传 3 分量 int | UBO/push constant（整型，同样 16 字节对齐） | 空桩 | gl_stubs.cpp |
| lime_gl_uniform3iv | 上传 3 分量 int 数组 | UBO 连续写入（每元素占 16 字节） | 空桩 | gl_stubs.cpp |
| lime_gl_uniform3ui | 上传 3 分量 uint | UBO/push constant（无符号） | 空桩 | gl_stubs.cpp |
| lime_gl_uniform3uiv | 上传 3 分量 uint 数组 | UBO 连续写入（每元素占 16 字节） | 空桩 | gl_stubs.cpp |
| lime_gl_uniform4f | 上传 4 分量 float | `vec4` 16 字节对齐 | 空桩 | gl_stubs.cpp |
| lime_gl_uniform4fv | 上传 4 分量 float 数组 | UBO 连续写入（每元素 16 字节） | 空桩 | gl_stubs.cpp |
| lime_gl_uniform4i | 上传 4 分量 int | UBO/push constant（整型） | 空桩 | gl_stubs.cpp |
| lime_gl_uniform4iv | 上传 4 分量 int 数组 | UBO 连续写入 | 空桩 | gl_stubs.cpp |
| lime_gl_uniform4ui | 上传 4 分量 uint | UBO/push constant（无符号） | 空桩 | gl_stubs.cpp |
| lime_gl_uniform4uiv | 上传 4 分量 uint 数组 | UBO 连续写入 | 空桩 | gl_stubs.cpp |
| lime_gl_uniform_matrix2fv | 上传 2×2 矩阵 | UBO/std140：每列 `vec2` 占 8 字节，需 transpose 语义对齐 | 空桩 | gl_stubs.cpp |
| lime_gl_uniform_matrix2x3fv | 上传 2×3 矩阵 | 每列占 12→16 字节（std140 `vec3` 填充） | 空桩 | gl_stubs.cpp |
| lime_gl_uniform_matrix2x4fv | 上传 2×4 矩阵 | 每列 `vec4` 16 字节 | 空桩 | gl_stubs.cpp |
| lime_gl_uniform_matrix3fv | 上传 3×3 矩阵 | 每列 `vec3` 16 字节填充；GL 的 `transpose` 标志需在 CPU 侧处理 | 空桩 | gl_stubs.cpp |
| lime_gl_uniform_matrix3x2fv | 上传 3×2 矩阵 | 每列 `vec2` 8 字节 | 空桩 | gl_stubs.cpp |
| lime_gl_uniform_matrix3x4fv | 上传 3×4 矩阵 | 每列 `vec4` 16 字节 | 空桩 | gl_stubs.cpp |
| lime_gl_uniform_matrix4fv | 上传 4×4 矩阵 | 每列 `vec4` 16 字节；`transpose=false` 时可直接 memcpy 到 std140 | 空桩 | gl_stubs.cpp |
| lime_gl_uniform_matrix4x2fv | 上传 4×2 矩阵 | 每列 `vec2` 8 字节 | 空桩 | gl_stubs.cpp |
| lime_gl_uniform_matrix4x3fv | 上传 4×3 矩阵 | 每列 `vec3` 16 字节填充 | 空桩 | gl_stubs.cpp |

> 上表共 77 行，与 `gl_compat.h` 中 shader/program 类入口一一对应，无遗漏、无重复（由 `tools/coverage/coverage.ps1` 全量核对）。

## 7. FBO / renderbuffer（24）

| 入口 | GL 语义 | Vulkan 落点 | 状态 | 文件 |
| --- | --- | --- | --- | --- |
| lime_gl_create_framebuffer | 创建 FBO | `VkRenderer::CreateRenderTarget`（`VkImage + VkImageView + VkFramebuffer + VkRenderPass`），返回 id+1000 | 空桩 | gl_stubs.cpp |
| lime_gl_bind_framebuffer | 绑定 FBO（0=默认） | `VkRenderer::BindRenderTarget`（0=backbuffer） | 空桩 | gl_stubs.cpp |
| lime_gl_delete_framebuffer | 删除 FBO | `VkRenderer::DestroyRenderTarget` | 空桩 | gl_stubs.cpp |
| lime_gl_framebuffer_texture2D | 挂载纹理为附件 | `VkFramebufferCreateInfo.pAttachments` + 该 image 切到 `COLOR_ATTACHMENT_OPTIMAL` layout | 空桩 | gl_stubs.cpp |
| lime_gl_framebuffer_texture_layer | 挂载纹理某一层 | `VkImageView` 用 `VK_IMAGE_VIEW_TYPE_2D_ARRAY` + `baseArrayLayer` | 空桩 | gl_stubs.cpp |
| lime_gl_framebuffer_renderbuffer | 挂载 renderbuffer 为附件 | `VkFramebuffer` 指向 renderbuffer 的 `VkImageView` | 空桩 | gl_stubs.cpp |
| lime_gl_create_renderbuffer | 创建 renderbuffer | 无独立对象；建 `VkImage`（可含 depth/stencil 格式） | 空桩 | gl_stubs.cpp |
| lime_gl_bind_renderbuffer | 绑定 renderbuffer | shim 记录当前 renderbuffer | 空桩 | gl_stubs.cpp |
| lime_gl_delete_renderbuffer | 删除 renderbuffer | `vkDestroyImageView/vkDestroyImage/vkFreeMemory` | 空桩 | gl_stubs.cpp |
| lime_gl_renderbuffer_storage | 分配 renderbuffer 存储 | `vkCreateImage` + `vkBindImageMemory` | 空桩 | gl_stubs.cpp |
| lime_gl_renderbuffer_storage_multisample | 分配 MSAA renderbuffer | `VkImageCreateInfo.samples` + `vkGetPhysicalDeviceImageFormatProperties` 校验 | 空桩 | gl_stubs.cpp |
| lime_gl_check_framebuffer_status | 检查 FBO 完整性 | 无直接对应；需自行校验格式/采样数/附件数后返回 GL 枚举（`GL_FRAMEBUFFER_COMPLETE=0x8CD5`） | 空桩 | gl_stubs.cpp |
| lime_gl_draw_buffers | 设置 MRT 输出 | `VkSubpassDescription.colorAttachmentCount` + `VkPipelineColorBlendStateCreateInfo.pAttachments` | 空桩 | gl_stubs.cpp |
| lime_gl_read_buffer | 设置回读缓冲 | `vkCmdCopyImageToBuffer` 的源 image 选择 | 空桩 | gl_stubs.cpp |
| lime_gl_blit_framebuffer | FBO 间 blit | `vkCmdBlitImage`（`VK_FILTER_NEAREST/LINEAR`，需源 image 为 `SRC_OPTIMAL`） | 空桩 | gl_stubs.cpp |
| lime_gl_read_pixels | 回读像素 | `vkCmdCopyImageToBuffer`（host-visible buffer）+ `vkWaitForFences`；注意 Y 轴翻转 | 空桩 | gl_stubs.cpp |
| lime_gl_invalidate_framebuffer | 丢弃附件内容 | `VkRenderPass` 的 `loadOp=DONT_CARE` 或 `vkCmdClearAttachments` | 空桩 | gl_stubs.cpp |
| lime_gl_invalidate_sub_framebuffer | 丢弃附件子区域 | `vkCmdClearAttachments` 的 `VkClearRect` | 空桩 | gl_stubs.cpp |
| lime_gl_get_framebuffer_attachment_parameteri | 查询附件参数 | 读 shim 元数据（格式/尺寸/采样数） | 空桩 | gl_stubs.cpp |
| lime_gl_get_framebuffer_attachment_parameteriv | 同上（数组出参） | 同上 | 空桩 | gl_stubs.cpp |
| lime_gl_get_renderbuffer_parameteri | 查询 renderbuffer 参数 | 读 shim 元数据 | 空桩 | gl_stubs.cpp |
| lime_gl_get_renderbuffer_parameteriv | 同上（数组出参） | 同上 | 空桩 | gl_stubs.cpp |
| lime_gl_is_framebuffer | 是否为有效 FBO | 查 shim 名字表 | 空桩 | gl_stubs.cpp |
| lime_gl_is_renderbuffer | 是否为有效 renderbuffer | 查 shim 名字表 | 空桩 | gl_stubs.cpp |

## 8. 其他（56）

| 入口 | GL 语义 | Vulkan 落点 | 状态 | 文件 |
| --- | --- | --- | --- | --- |
| lime_gl_finish | 等待所有命令完成 | `vkDeviceWaitIdle` / `vkQueueWaitIdle` | 空桩 | gl_stubs.cpp |
| lime_gl_flush | 提交已排队命令 | `vkQueueSubmit` + `vkQueueSubmit` 后不等待 | 空桩 | gl_stubs.cpp |
| lime_gl_get_error | 取错误码（粘性队列） | 无直接对应；需 shim 维护错误队列，从 `VkResult` 翻译（当前恒返回 0，会吞错误） | 空桩 | gl_core.cpp |
| lime_gl_get_string | 查询驱动/版本字符串 | 静态返回（`vkGetPhysicalDeviceProperties` 的 `deviceName/apiVersion`） | 空桩 | gl_stubs.cpp |
| lime_gl_get_stringi | 查询 GLSL 版本等 | 静态返回（Vulkan 无 GLSL 版本概念） | 空桩 | gl_stubs.cpp |
| lime_gl_get_extension | 查询扩展支持 | 由 `vkEnumerateDeviceExtensionProperties` + GL 扩展名映射表合成 | 空桩 | gl_stubs.cpp |
| lime_gl_get_context_attributes | 查询上下文属性 | 静态返回（基于 `VkPhysicalDeviceProperties`/format 支持） | 空桩 | gl_stubs.cpp |
| lime_gl_get_boolean | 查询布尔状态 | 读 shim 状态表 | 空桩 | gl_stubs.cpp |
| lime_gl_get_booleanv | 同上（数组出参） | 同上 | 空桩 | gl_stubs.cpp |
| lime_gl_get_float | 查询浮点状态 | 读 shim 状态表 | 空桩 | gl_stubs.cpp |
| lime_gl_get_floatv | 同上（数组出参） | 同上 | 空桩 | gl_stubs.cpp |
| lime_gl_get_integer | 查询整型状态 | 读 shim 状态表 | 空桩 | gl_stubs.cpp |
| lime_gl_get_integerv | 同上（数组出参） | 同上 | 空桩 | gl_stubs.cpp |
| lime_gl_get_integer64v | 查询 64 位整型状态 | 读 shim 状态表 | 空桩 | gl_stubs.cpp |
| lime_gl_get_integer64i_v | 同上（带索引） | 同上 | 空桩 | gl_stubs.cpp |
| lime_gl_get_integeri_v | 同上（带索引） | 同上 | 空桩 | gl_stubs.cpp |
| lime_gl_get_active_attrib | 反射 active attribute | SPIR-V/glslang 反射表 | 空桩 | gl_stubs.cpp |
| lime_gl_create_query | 创建查询对象 | `VkQueryPool`（`vkCreateQueryPool`） | 空桩 | gl_stubs.cpp |
| lime_gl_delete_query | 删除查询对象 | `vkDestroyQueryPool` | 空桩 | gl_stubs.cpp |
| lime_gl_begin_query | 开始查询 | `vkCmdBeginQuery` + `vkCmdResetQueryPool` | 空桩 | gl_stubs.cpp |
| lime_gl_end_query | 结束查询 | `vkCmdEndQuery` | 空桩 | gl_stubs.cpp |
| lime_gl_get_queryi | 查询结果（阻塞） | `vkGetQueryPoolResults`（`VK_QUERY_RESULT_WAIT_BIT`） | 空桩 | gl_stubs.cpp |
| lime_gl_get_queryiv | 查询结果（数组） | 同上 | 空桩 | gl_stubs.cpp |
| lime_gl_get_query_objectui | 查询对象参数 | shim 元数据 | 空桩 | gl_stubs.cpp |
| lime_gl_get_query_objectuiv | 同上（数组） | 同上 | 空桩 | gl_stubs.cpp |
| lime_gl_is_query | 是否为有效查询 | 查 shim 名字表 | 空桩 | gl_stubs.cpp |
| lime_gl_create_sampler | 创建 sampler 对象 | `VkSampler`（`vkCreateSampler`） | 空桩 | gl_stubs.cpp |
| lime_gl_delete_sampler | 删除 sampler | `vkDestroySampler` | 空桩 | gl_stubs.cpp |
| lime_gl_bind_sampler | 绑定 sampler 到 unit | 写入 descriptor set 的 `VkDescriptorImageInfo.sampler` | 空桩 | gl_stubs.cpp |
| lime_gl_sampler_parameteri | 设置 sampler 参数（整型） | 重建 `VkSamplerCreateInfo`（`addressMode*`/`compareEnable`） | 空桩 | gl_stubs.cpp |
| lime_gl_sampler_parameterf | 设置 sampler 参数（浮点） | `maxAnisotropy/mipLodBias` | 空桩 | gl_stubs.cpp |
| lime_gl_get_sampler_parameteri | 查询 sampler 参数 | 读 shim 副本 | 空桩 | gl_stubs.cpp |
| lime_gl_get_sampler_parameteriv | 同上（数组） | 同上 | 空桩 | gl_stubs.cpp |
| lime_gl_get_sampler_parameterf | 同上（浮点） | 同上 | 空桩 | gl_stubs.cpp |
| lime_gl_get_sampler_parameterfv | 同上（浮点数组） | 同上 | 空桩 | gl_stubs.cpp |
| lime_gl_is_sampler | 是否为有效 sampler | 查 shim 名字表 | 空桩 | gl_stubs.cpp |
| lime_gl_fence_sync | 插入 fence sync | `vkCreateFence` + `vkQueueSubmit(pSignalFences)` | 空桩 | gl_stubs.cpp |
| lime_gl_client_wait_sync | 客户端等待 sync | `vkWaitForFences` | 空桩 | gl_stubs.cpp |
| lime_gl_wait_sync | 服务端等待 sync | `vkCmdWaitEvents`（或 submit 的 wait semaphore） | 空桩 | gl_stubs.cpp |
| lime_gl_delete_sync | 删除 sync 对象 | `vkDestroyFence` | 空桩 | gl_stubs.cpp |
| lime_gl_is_sync | 是否为有效 sync | 查 shim 名字表 | 空桩 | gl_stubs.cpp |
| lime_gl_get_sync_parameteri | 查询 sync 参数 | shim 元数据/`vkGetFenceStatus` | 空桩 | gl_stubs.cpp |
| lime_gl_get_sync_parameteriv | 同上（数组） | 同上 | 空桩 | gl_stubs.cpp |
| lime_gl_create_transform_feedback | 创建 TF 对象 | Vulkan 无 TF；映射为 SSBO + geometry/compute shader | 空桩 | gl_stubs.cpp |
| lime_gl_delete_transform_feedback | 删除 TF 对象 | 释放对应 SSBO | 空桩 | gl_stubs.cpp |
| lime_gl_bind_transform_feedback | 绑定 TF 对象 | 绑定 SSBO descriptor set | 空桩 | gl_stubs.cpp |
| lime_gl_begin_transform_feedback | 开始 TF 捕获 | 无直接对应（需 geometry/compute 变体管线） | 空桩 | gl_stubs.cpp |
| lime_gl_end_transform_feedback | 结束 TF 捕获 | 同上 | 空桩 | gl_stubs.cpp |
| lime_gl_pause_transform_feedback | 暂停 TF | 同上 | 空桩 | gl_stubs.cpp |
| lime_gl_resume_transform_feedback | 恢复 TF | 同上 | 空桩 | gl_stubs.cpp |
| lime_gl_transform_feedback_varyings | 设置捕获 varying | 映射为 SSBO 输出变量 | 空桩 | gl_stubs.cpp |
| lime_gl_get_transform_feedback_varying | 查询 TF varying | Vulkan 无 TF；可映射 transform feedback 扩展或 compute 写 SSBO | 空桩 | gl_stubs.cpp |
| lime_gl_is_transform_feedback | 是否为有效 TF 对象 | 查 shim 名字表 | 空桩 | gl_stubs.cpp |
| lime_gl_object_register | 注册 CFFI 对象 | 纯 shim 侧句柄表，无需 Vk 对象 | 空桩 | gl_stubs.cpp |
| lime_gl_object_from_id | 按 id 取对象 | 同上 | 空桩 | gl_stubs.cpp |
| lime_gl_object_deregister | 注销对象 | 同上 | 空桩 | gl_stubs.cpp |

## 9. 已知语义陷阱

1. **坐标原点 / Y 轴方向（视口与绘制）**
   GL 原点在左下、Vulkan 在左上。当前 `VkRenderer::DrawRaw` 已做 `y_ndc = 1 - 2y/h` 的转换（[vk_renderer.cpp](file:///C:/Users/yuu/Documents/Trae-CN/temp_20261106_01/native_vulkan/vk_renderer.cpp#L830-L831)），但 `lime_gl_viewport` 记录的值从未提交到 `vkCmdSetViewport`，且 `glScissor` 需要 `rect.y = targetHeight - (glY + glH)` 的换算。移植时不要同时翻转 NDC 和视口，否则会二次翻转。

2. **纹理 Y 轴方向（上传行序）**
   `lime_gl_tex_image_2d` 直接 `memcpy` 上传，未翻转行；GL 纹理首行是"底部"，Vulkan `VkBufferImageCopy` 首行是"顶部"。当前绘制之所以没暴露出问题，是因为游戏 UV 与 `DrawRaw` 的 NDC 翻转恰好抵消了一部分；一旦 FBO 离屏渲染接入，采样方向会反。规范做法：上传时翻转行，或统一在 UV 上处理，二者只能选一个，并在文档/代码注释里固定约定。

3. **预乘 Alpha（premultiplied alpha）**
   现有 pipeline 固定为 `blendEnable=TRUE`、`srcColorBlendFactor=SRC_ALPHA`、`dstColorBlendFactor=ONE_MINUS_SRC_ALPHA`（[vk_renderer.cpp](file:///C:/Users/yuu/Documents/Trae-CN/temp_20261106_01/native_vulkan/vk_renderer.cpp#L517-L520)），即"直通 alpha"。而 `gl_core` 记录的 `g_blendSrc/g_blendDst` 从未生效，`glDisable(GL_BLEND)` 也无效——**透明区域始终在混合，不透明绘制也走了混合路径**。flixel 的图集通常是预乘 alpha，若直接切到 `ONE / ONE_MINUS_SRC_ALPHA` 会让半透明像素变暗或变亮，需按图集实际来源二选一，并给每个 pipeline 明确标注。

4. **FBO 完整状态码**
   `glCheckFramebufferStatus` 在 GL 中 `GL_FRAMEBUFFER_COMPLETE = 0x8CD5`，而当前空桩返回 `0`。`0` 不是 COMPLETE 值，调用方会走"FBO 不完整"分支，导致后处理/离屏渲染被静默禁用（不是崩溃，是功能消失）。接入时要么返回真实 0x8CD5，要么在创建阶段就用 `vkGetPhysicalDeviceImageFormatProperties` + 附件格式/采样数一致性做校验。

5. **着色器编译模型**
   GL 的 `compileShader` 只是排队，错误在 `linkProgram` 才暴露；Vulkan 必须 `vkCreateShaderModule` 时就拿到 SPIR-V 并即时失败。`getShaderInfoLog`/`getProgramInfoLog` 需要在 shim 里缓存上次错误的完整字符串，否则日志二次读取会变空，排查线上问题时等于瞎了。另外 GLSL 里大量 ES 1.00 写法（`uvec3` 位移、`round()`、`texture2D`）不能直接喂给 SPIR-V 编译器，需要先做源码级规整。

6. **`glGetError` 的错误模型**
   GL 是粘性错误队列（读一次清一个），Vulkan 每个调用返回 `VkResult`。当前 `lime_gl_get_error` 恒返回 0，会把真实的分配失败/`VK_ERROR_OUT_OF_DEVICE_MEMORY` 全部吞掉——这正是之前大纹理 OOM 难以定位的原因之一。建议在 shim 里维护一个环形错误队列，在 vk 调用失败处 `push`，`get_error` 时 `pop`。

7. **像素格式与通道顺序**
   当前上传只支持 `GL_RGBA`/`GL_RGB` + `GL_UNSIGNED_BYTE`（`0x1908/0x1907/0x1401`），其余格式直接 `return`（静默丢弃）。Vulkan 侧固定 `VK_FORMAT_R8G8B8A8_UNORM`。`GL_BGRA`、`GL_UNSIGNED_SHORT_5_6_5`、`GL_UNSIGNED_SHORT_4_4_4_4` 需要显式 swizzle 或改用对应 `VkFormat`；`GL_SRGB8_ALPHA8` 要映射 `VK_FORMAT_R8G8B8A8_SRGB`，否则颜色会整体偏暗/偏亮。

8. **纹理单元与 sampler 的绑定模型**
   GL 是 `unit → texture`，sampler object 可独立绑定并覆盖纹理参数；Vulkan 的 descriptor 同时打包 `imageView + sampler`，sampler 参数一改就得分重建 `VkSampler` 并更新 descriptor set。当前 `bind_texture` 只写了一个 per-unit 数组，绘制时只取 `g_activeUnit` 对应的那一张——**多纹理材质（CRT/辉光/多图层）会全采样同一张图**。

9. **顶点属性与 VAO**
   GL 允许 client-side 指针和随意 stride；Vulkan 的 vertex input 在 pipeline 创建时固定。当前 `lime_gl_draw_arrays` 硬编码了 `attrib0=位置、attrib1=UV、stride 默认 16`（[gl_core.cpp](file:///C:/Users/yuu/Documents/Trae-CN/temp_20261106_01/native_vulkan/gl_core.cpp#L259-L273)），`g_attrs[2..15]` 记录了但从未使用。任何"位置+颜色""位置+UV+颜色"布局都会读错；索引绘制也是 CPU 展开，`vkCmdDrawIndexed` 完全没用上。

10. **绘制模式与图元拓扑**
    Vulkan **没有** `GL_QUADS`、`GL_POLYGON`、`GL_TRIANGLE_FAN`。`DrawRaw` 固定 `VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST`，`mode` 参数被完全忽略。flixel 大量使用三角形，暂时可用，但一旦有 `QUADS`（tilemap/矩形批处理）必须 CPU 侧三角化，不能指望拓扑转换。

11. **std140 对齐（uniform 上传）**
    `vec3` 在 std140 里占 16 字节、`mat3` 每列占 16 字节。GL 的 `uniform3fv` 是 12 字节紧凑数据，直接 `memcpy` 进 UBO 会错位。矩阵的 `transpose` 参数也必须在 CPU 侧先转置（std140 要求列主序）。

12. **FBO attachment 与采样同一张 image**
    GL 允许 texture 既作 color attachment 又被采样（未定义行为但常见）。Vulkan 里同一 image 同时处于 `COLOR_ATTACHMENT_OPTIMAL` 和 `SHADER_READ_ONLY_OPTIMAL` 会触发 layout 冲突/验证层报错。后处理链（Bloom/CRT/终曲）必须用 ping-pong 双 target 或插入 barrier + layout transition。

13. **渲染通道生命周期与 clear**
   `glClear` 在 render pass 之外调用在 Vulkan 是非法命令。当前实现把 clear 折叠进 `BeginFrame → BindRenderTarget` 的 `loadOp=CLEAR`，等于**每帧无条件清屏**；`glClear(GL_DEPTH_BUFFER_BIT)` 之类细分 mask 无效，且没有深度/模板附件。将来加深度附件时，需要在 `VkAttachmentDescription` 里同时配置 `loadOp/storeOp`，并区分 backbuffer（`storeOp=STORE`）与离屏（`DONT_CARE` 省带宽，Mali 尤其敏感）。

14. **`readPixels` / `blitFramebuffer` 的行序**
   `vkCmdCopyImageToBuffer` 输出的首行是 image 顶部，GL `glReadPixels` 的首行是底部。回读截图/存档校验类功能需要显式翻转，否则上下颠倒。

15. **默认对象与 0 号名字**
   GL 中 `framebuffer 0` = 默认帧缓冲、`vertex array 0` = 默认 VAO，而 Vulkan 没有"0 号对象"概念。shim 必须把 0 解释为 backbuffer/默认 VAO；当前 `VkRenderer` 用 `id=0` 表示 backbuffer、`id>=1000` 表示 render target，这个约定要在 GL 名字映射层保持一致，不能混用。

16. **sRGB 与 swapchain 格式**
   Vulkan swapchain 常用 `VK_FORMAT_B8G8R8A8_SRGB`，而当前纹理是 `R8G8B8A8_UNORM`。若 swapchain 是 sRGB 而纹理不是，采样后写入会被硬件再做一次线性→sRGB 编码，整体发灰/发白。要么全链路 UNORM + 手动 gamma，要么统一 sRGB，不能混。

## 10. 缺口优先级（按对"跑起来"的阻塞程度）

| 优先级 | 模块 | 缺口 | 影响 |
| --- | --- | --- | --- |
| P0 | shader/program | 77/77 空桩 | 没有 GLSL→SPIR-V、没有 pipeline 创建；任何带 shader 的绘制都无法成立，是当前最大单点 |
| P0 | FBO/renderbuffer | 24/24 空桩 | 后处理、CRT/辉光、终曲全链路依赖离屏；`check_framebuffer_status` 返回 0 会让上层直接跳过 |
| P1 | 纹理 | 22/27 空桩 | 子图更新（Haxe 图集动态打包）、mipmap、过滤/包裹参数、压缩纹理全部缺失 |
| P1 | 缓冲/顶点 | 45/55 空桩 | 索引/实例化/VAO/UBO 缺失；当前 `bufferData` 只在 CPU 侧，GPU 上没有任何 `VkBuffer` |
| P2 | 状态 | 22/30 空桩 | 深度/模板/剔除缺失；混合状态被 pipeline 固定值覆盖 |
| P2 | 其他 | 56/56 空桩 | query/sync/TF 多为可选；但 `get_error` 恒 0 会掩盖真实 OOM/设备错误，建议优先补 |

> 有效能力提示：虽然 269 个入口里 23 个"真实现"，但**真正接入 Vulkan 命令流的只有 5 条路径**（clear、纹理上传、2 种 draw、2 种 delete）。换算成"可用渲染能力"，当前大约相当于 GL 1.0 的"单纹理 + 非索引三角形"子集。之前会话里 demo 能跑到 46~50 FPS，是因为 demo 只用了这 5 条路径，不代表游戏侧 GL 调用已被覆盖。

## 11. 并行会话的本地增量（未提交到 `main`）

映射表第 3~8 节描述的是 `main @ af103da` 的已提交状态。写入本表时，本地草稿目录里并行会话又新增了以下文件，**尚未提交**，因此表内状态列没有把它们计入，避免下次重跑脚本时对不上：

| 文件 | 覆盖内容 | 本地实测 |
| --- | --- | --- |
| `gl_fbo.cpp` | 22/24 个 FBO/renderbuffer 入口的真实现（`create/bind/delete framebuffer`、`renderbuffer_storage`、`framebuffer_texture2D/layer`、`blit_framebuffer`、`invalidate_*`、查询类等） | 仅剩 `read_buffer`、`read_pixels` 仍是空桩 |
| `gl_state.h` / `gl_state.cpp` | 状态机内部实现（Cap/BlendFactor/BlendOp/CompareOp/StencilOp 的枚举映射与状态快照），**尚未提供 `lime_gl_*` 入口**，所以统计脚本在本文件中扫不到入口定义 | 0 个入口 |
| `gl_texture_params.cpp` | 纹理参数（过滤/包裹/LOD/各向异性）的枚举映射与校验内部实现，同样尚未挂到 `lime_gl_*` 入口 | 0 个入口 |

按本地目录（含上述未提交文件）重跑脚本的实测值：`269` 个入口中真实现 `45`、空桩 `224`，总体真实现率 `16.7%`，FBO/renderbuffer 模块 `91.7%`。合入 `main` 后请以此为准并更新第 1 节总览。

> 注意：`gl_fbo.cpp` 与 `gl_stubs.cpp` 目前同时存在同名定义，未删除桩函数前会链接冲突；这类清理属于写源码的会话职责，本文档不代改。

## 12. 维护约定

- `gl_stubs.cpp` 是自动生成的兜底集合；任何入口从空桩转真实现后，应从 `gl_stubs.cpp` 删除同名定义，否则会链接冲突。
- 新增 `gl_shader.cpp` / `gl_fbo.cpp` / `gl_state.cpp` / `gl_texture.cpp` 等文件后，直接重跑 `tools/coverage/coverage.ps1`，脚本会自动扫描并归入对应模块，无需改脚本。
- 本表的"状态"列若与脚本输出不一致，以脚本为准并同步更新本表。
- 表内所有"Vulkan 落点"均为设计落点，不代表已实现；已实现的以 `gl_core.cpp` 实际代码为准。
