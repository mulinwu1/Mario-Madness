#pragma once

#include <stdint.h>
#include <stddef.h>
#include <vulkan/vulkan.h>

// GLES shader / program 子系统的对外接口。
//
// 这个文件由 gl_shader.cpp 实现，负责把 OpenFL / Flixel 运行时生成的 GLSL ES 1.00
// 源码翻译成 SPIR-V，并保持 GL 的 shader / program / uniform 语义（location、数组、
// uniform block、info log、getActiveUniform 等）。
//
// 与 VkRenderer 的接线方式：VkRenderer 的所有 Vulkan 对象都在它自己的 Impl 里，没有
// 公开 device / renderPass，所以这里不直接依赖 VkRenderer 的内部实现，而是由渲染器
// 在 Init 时调用 gl_shader_vk_register() 把自己的设备信息和 render pass 交过来，
// 之后 gl_shader 就能独立创建 VkPipeline / VkDescriptorSet / uniform buffer。
// 渲染器只需要在 draw 之前使用这里返回的 pipeline / descriptorSet，并按 sampler 名
// 上传纹理，不需要修改 gl_shader 的任何内部逻辑。

namespace lime {

// 顶点输入布局。GL 的顶点属性最终会落到 VkPipeline 的 vertex input state，
// 属性 format 使用 VkFormat 的数值（例如 VK_FORMAT_R32G32_SFLOAT = 103）。
struct GLShaderVertexAttribute {

	uint32_t location;   // GL attribute location（由 bindAttribLocation / 自动分配得到）
	uint32_t binding;    // VkVertexInputBindingDescription.binding
	uint32_t format;     // VkFormat
	uint32_t offset;     // 相对 binding 的字节偏移

};

struct GLShaderVertexLayout {

	uint32_t binding;                        // 交错顶点缓冲的 binding index（通常为 0）
	uint32_t stride;                         // 顶点步长
	const GLShaderVertexAttribute* attributes;
	uint32_t attributeCount;

};

// 渲染器向 shader 子系统提供的 Vulkan 设备上下文。
// getDeviceProcAddr 只需要一个，其余 device 级函数内部自行通过它加载。
struct GLShaderVkContext {

	VkDevice device;
	VkRenderPass backbufferRenderPass;       // 直接渲染到交换链时使用的 render pass
	VkRenderPass offscreenRenderPass;        // 渲染到离屏纹理（R8G8B8A8）时使用的 render pass
	VkFormat backbufferFormat;
	VkFormat offscreenFormat;
	uint32_t viewportWidth;                  // 静态 viewport 宽度（管线创建时写入）
	uint32_t viewportHeight;                 // 静态 viewport 高度；窗口尺寸变化后调用 update_viewport
	uint32_t uniformMemoryTypeIndex;         // host visible + coherent 的内存类型索引
	uint32_t maxSamplers;                    // 每个 program 支持的 sampler 上限，0 表示用默认 16
	PFN_vkGetDeviceProcAddr getDeviceProcAddr;

};

// 注册 / 注销设备上下文。注销时会销毁本子系统创建的全部 Vulkan 对象。
void gl_shader_vk_register (const GLShaderVkContext* context);
void gl_shader_vk_unregister ();
bool gl_shader_vk_available ();
// 窗口尺寸变化后调用：记录新尺寸并让所有已建管线在下次使用时重建
void gl_shader_vk_update_viewport (uint32_t width, uint32_t height);

// 为指定 program 构建（或复用）图形管线。
// layout 描述顶点输入；传 NULL 时使用位置 0(vec2)+UV 1(vec2) 的默认交错布局。
// offscreen 为 true 时使用离屏 render pass。
// 返回 false 表示当前没有注册 Vulkan 上下文、program 未链接成功、或管线创建失败。
bool gl_shader_build_pipeline (int program, const GLShaderVertexLayout* layout, bool offscreen);

VkPipeline gl_shader_pipeline (int program, bool offscreen);
VkPipelineLayout gl_shader_pipeline_layout (int program);
VkDescriptorSet gl_shader_descriptor_set (int program, bool offscreen);

// uniform buffer 的字节大小（std140）。渲染器可在每帧 draw 前调用 upload。
uint32_t gl_shader_uniform_buffer_size (int program);

// 把 CPU 侧 uniform 镜像写入 host visible buffer 并更新 descriptor set（仅 UBO 部分）。
bool gl_shader_upload_uniforms (int program);

// 按 sampler uniform 名写入纹理描述符。name 形如 "bitmap" 或 "bitmap[0]"。
// 多次调用同一个 binding 会覆盖之前的纹理。
bool gl_shader_write_sampler (int program, bool offscreen, const char* name, VkImageView view, VkSampler sampler);

// 查询信息：
// sampler uniform 在 descriptor set 中的 binding（找不到返回 -1）
int gl_shader_sampler_binding (int program, const char* name);
// uniform location 当前保存的纹理单元号（sampler uniform 才有意义，其他返回 -1）
int gl_shader_uniform_texture_unit (int program, int location);
// 当前 useProgram 绑定的 program（0 表示无）
int gl_shader_current_program ();
// 最近一次链接的 GLSL 源码（调试用，可能为 NULL）
const char* gl_shader_program_glsl (int program, int stage);
// 程序 / 着色器对象的错误日志（调试用，可能为空串）
const char* gl_shader_program_log (int program);

}
