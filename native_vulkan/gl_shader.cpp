#include "gl_shader.h"
#include "gl_compat.h"

#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>

#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <cstddef>
#include <stdint.h>

#ifdef _MSC_VER
// glslang 16.6.0 官方静态库是 /MD 构建，直接链接进 /MT 的 lime.ndll 会报
// LNK2038 RuntimeLibrary 不匹配；因此官方库由 glslang_shim 包成 lime_glslang.dll，
// 这里只链它的 import lib（import lib 不携带 CRT 标记）。DLL 与 ndll 同目录部署。
#pragma comment (lib, "C:\\glslang\\lib\\lime_glslang.lib")
#endif

namespace lime {

// ---------------------------------------------------------------------------
// GL 常量（本文件不依赖 OpenGLBindings.h，单独定义需要的枚举值）
// ---------------------------------------------------------------------------

enum {
	GL_FALSE = 0,
	GL_TRUE = 1,
	GL_FLOAT = 0x1406,
	GL_FLOAT_VEC2 = 0x8B50,
	GL_FLOAT_VEC3 = 0x8B51,
	GL_FLOAT_VEC4 = 0x8B52,
	GL_INT = 0x1404,
	GL_INT_VEC2 = 0x8B53,
	GL_INT_VEC3 = 0x8B54,
	GL_INT_VEC4 = 0x8B55,
	GL_UNSIGNED_INT = 0x1405,
	GL_UNSIGNED_INT_VEC2 = 0x8DC6,
	GL_UNSIGNED_INT_VEC3 = 0x8DC7,
	GL_UNSIGNED_INT_VEC4 = 0x8DC8,
	GL_BOOL = 0x8B56,
	GL_BOOL_VEC2 = 0x8B57,
	GL_BOOL_VEC3 = 0x8B58,
	GL_BOOL_VEC4 = 0x8B59,
	GL_FLOAT_MAT2 = 0x8B5A,
	GL_FLOAT_MAT3 = 0x8B5B,
	GL_FLOAT_MAT4 = 0x8B5C,
	GL_FLOAT_MAT2x3 = 0x8B65,
	GL_FLOAT_MAT2x4 = 0x8B66,
	GL_FLOAT_MAT3x2 = 0x8B67,
	GL_FLOAT_MAT3x4 = 0x8B68,
	GL_FLOAT_MAT4x2 = 0x8B69,
	GL_FLOAT_MAT4x3 = 0x8B6A,
	GL_SAMPLER_2D = 0x8B5E,
	GL_SAMPLER_3D = 0x8B5F,
	GL_SAMPLER_CUBE = 0x8B60,
	GL_SAMPLER_2D_SHADOW = 0x8B62,
	GL_SAMPLER_2D_ARRAY = 0x8DC1,
	GL_SAMPLER_2D_ARRAY_SHADOW = 0x8DC4,
	GL_SAMPLER_CUBE_SHADOW = 0x8DC5,
	GL_SAMPLER_2D_MULTISAMPLE = 0x9108,
	GL_VERTEX_SHADER = 0x8B31,
	GL_FRAGMENT_SHADER = 0x8B30,
	GL_DELETE_STATUS = 0x8B80,
	GL_COMPILE_STATUS = 0x8B81,
	GL_LINK_STATUS = 0x8B82,
	GL_VALIDATE_STATUS = 0x8B83,
	GL_INFO_LOG_LENGTH = 0x8B84,
	GL_ATTACHED_SHADERS = 0x8B85,
	GL_ACTIVE_UNIFORMS = 0x8B86,
	GL_ACTIVE_UNIFORM_MAX_LENGTH = 0x8B87,
	GL_SHADER_SOURCE_LENGTH = 0x8B88,
	GL_ACTIVE_ATTRIBUTES = 0x8B89,
	GL_ACTIVE_ATTRIBUTE_MAX_LENGTH = 0x8B8A,
	GL_SHADER_TYPE = 0x8B4F,
	GL_UNIFORM_BLOCK_BINDING = 0x8A3F,
	GL_UNIFORM_BLOCK_DATA_SIZE = 0x8A40,
	GL_UNIFORM_BLOCK_NAME_LENGTH = 0x8A41,
	GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS = 0x8A36,
	GL_UNIFORM_TYPE = 0x8A37,
	GL_UNIFORM_SIZE = 0x8A38,
	GL_UNIFORM_NAME_LENGTH = 0x8A39,
	GL_UNIFORM_BLOCK_INDEX = 0x8A3A,
	GL_UNIFORM_OFFSET = 0x8A3B,
	GL_UNIFORM_ARRAY_STRIDE = 0x8A3C,
	GL_UNIFORM_MATRIX_STRIDE = 0x8A3D,
	GL_UNIFORM_IS_ROW_MAJOR = 0x8A3E,
	GL_INVALID_INDEX = 0xFFFFFFFFu
};

// ---------------------------------------------------------------------------
// 内部数据模型
// ---------------------------------------------------------------------------

static const int STAGE_VERTEX = 0;
static const int STAGE_FRAGMENT = 1;

struct UniformInfo {

	std::string name;        // 基础名（数组不带下标）
	std::string glName;      // GL 查询用名：数组为 name[0]
	int glType;              // GL 类型枚举
	int arraySize;           // 数组元素个数，非数组为 1
	int location;            // 基础 location（数组是第一个元素的 location）
	int binding;             // sampler 的 descriptor binding；UBO 成员为 0
	int kind;                // 0=float 1=int 2=uint 3=bool 4=opaque sampler
	int components;          // 每个元素的标量分量数（矩阵为行数）
	int columns;             // 矩阵列数，非矩阵为 1
	bool rowMajor;           // 矩阵是否 row_major
	uint32_t offset;         // std140 字节偏移（sampler 无意义）
	uint32_t arrayStride;    // 数组元素 stride
	uint32_t matrixStride;   // 矩阵列 stride
	uint32_t byteSize;       // 单个元素占用字节数（用于 UBO 边界计算）
	int textureUnit;         // sampler 的纹理单元

};

struct AttribInfo {

	std::string name;
	int glType;
	int location;
	int arraySize;

};

struct PipelineSlot {

	bool built;
	uint64_t fingerprint;
	VkPipeline pipeline;
	VkPipelineLayout layout;
	VkDescriptorSetLayout descriptorLayout;
	VkDescriptorPool descriptorPool;
	VkDescriptorSet descriptorSet;
	VkBuffer uniformBuffer;
	VkDeviceMemory uniformMemory;
	void* mapped;
	uint32_t bufferSize;

	PipelineSlot () : built (false), fingerprint (0), pipeline (VK_NULL_HANDLE), layout (VK_NULL_HANDLE),
		descriptorLayout (VK_NULL_HANDLE), descriptorPool (VK_NULL_HANDLE), descriptorSet (VK_NULL_HANDLE),
		uniformBuffer (VK_NULL_HANDLE), uniformMemory (VK_NULL_HANDLE), mapped (NULL), bufferSize (0) {}

};

struct Program {

	int id;
	bool deleted;
	bool linked;
	bool linkOk;
	bool validateOk;
	std::string log;
	std::vector<int> attached;
	std::map<std::string, int> attribBindings;

	std::vector<UniformInfo> uniforms;
	std::vector<int> locationSlot;   // location -> uniform 下标
	std::vector<int> locationElem;   // location -> 数组元素下标
	int nextLocation;

	std::vector<AttribInfo> attribs;
	std::map<std::string, int> attribLocations;

	uint32_t uniformSize;
	std::vector<uint8_t> uniformData;
	bool uniformDirty;

	std::vector<uint32_t> spirvVertex;
	std::vector<uint32_t> spirvFragment;
	std::string glslVertex;
	std::string glslFragment;

	PipelineSlot pipelineBack;
	PipelineSlot pipelineOffscreen;

	Program () : id (0), deleted (false), linked (false), linkOk (false), validateOk (false),
		nextLocation (0), uniformSize (0), uniformDirty (false) {}

};

struct Shader {

	int id;
	int glType;
	std::string source;
	std::string log;
	bool compiled;
	bool compileOk;
	bool deleted;
	int refCount;

	Shader () : id (0), glType (0), compiled (false), compileOk (false), deleted (false), refCount (0) {}

};

static std::map<int, Shader*> g_shaders;
static std::map<int, Program*> g_programs;
static int g_nextShaderId = 1;
static int g_nextProgramId = 1;
static int g_currentProgram = 0;

static Program* FindProgram (int id) {

	std::map<int, Program*>::iterator it = g_programs.find (id);
	return (it == g_programs.end ()) ? NULL : it->second;

}

static Shader* FindShader (int id) {

	std::map<int, Shader*>::iterator it = g_shaders.find (id);
	return (it == g_shaders.end ()) ? NULL : it->second;

}

// ---------------------------------------------------------------------------
// 通用字符串处理
// ---------------------------------------------------------------------------

static std::string StripLineEndings (const std::string& in) {

	std::string out;
	out.reserve (in.size ());
	for (size_t i = 0; i < in.size (); i++) {

		if (in[i] == '\r') {

			if (i + 1 < in.size () && in[i + 1] == '\n') continue;
			out.push_back ('\n');

		} else {

			out.push_back (in[i]);

		}

	}
	return out;

}

static std::string StripComments (const std::string& in) {

	std::string out;
	out.reserve (in.size ());
	size_t i = 0;
	size_t n = in.size ();

	while (i < n) {

		char c = in[i];
		if (c == '/' && i + 1 < n && in[i + 1] == '/') {

			while (i < n && in[i] != '\n') i++;
			out.push_back ('\n');

		} else if (c == '/' && i + 1 < n && in[i + 1] == '*') {

			i += 2;
			while (i + 1 < n && !(in[i] == '*' && in[i + 1] == '/')) {

				if (in[i] == '\n') out.push_back ('\n');
				i++;

			}
			i += 2;

		} else {

			out.push_back (c);
			i++;

		}

	}
	return out;

}

static bool IsIdentStart (char c) {

	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';

}

static bool IsIdentChar (char c) {

	return IsIdentStart (c) || (c >= '0' && c <= '9');

}

static std::vector<std::string> Tokenize (const std::string& s) {

	std::vector<std::string> tokens;
	size_t i = 0;
	size_t n = s.size ();

	while (i < n) {

		char c = s[i];
		if (c == ' ' || c == '\t' || c == '\n') { i++; continue; }
		if (IsIdentStart (c)) {

			size_t start = i;
			while (i < n && IsIdentChar (s[i])) i++;
			tokens.push_back (s.substr (start, i - start));
			continue;

		}
		if (c >= '0' && c <= '9') {

			size_t start = i;
			while (i < n && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == 'x' || s[i] == 'X' ||
				(s[i] >= 'a' && s[i] <= 'f') || (s[i] >= 'A' && s[i] <= 'F'))) i++;
			tokens.push_back (s.substr (start, i - start));
			continue;

		}
		if (c == '[') {

			size_t start = i;
			int depth = 0;
			while (i < n) {

				if (s[i] == '[') depth++;
				if (s[i] == ']') { depth--; if (depth == 0) { i++; break; } }
				i++;

			}
			tokens.push_back (s.substr (start, i - start));
			continue;

		}
		if (c == '(') {

			size_t start = i;
			int depth = 0;
			while (i < n) {

				if (s[i] == '(') depth++;
				if (s[i] == ')') { depth--; if (depth == 0) { i++; break; } }
				i++;

			}
			tokens.push_back (s.substr (start, i - start));
			continue;

		}
		tokens.push_back (std::string (1, c));
		i++;

	}
	return tokens;

}

static bool IsPrecision (const std::string& t) {

	return t == "lowp" || t == "mediump" || t == "highp";

}

static bool IsQualifier (const std::string& t) {

	return IsPrecision (t) || t == "invariant" || t == "flat" || t == "smooth" || t == "centroid" ||
		t == "noperspective" || t == "const" || t == "coherent" || t == "volatile" || t == "restrict" ||
		t == "readonly" || t == "writeonly";

}

enum { BASE_FLOAT, BASE_INT, BASE_UINT, BASE_BOOL, BASE_SAMPLER };

static bool TypeInfo (const std::string& type, int* kind, int* components, int* columns, bool* sampler) {

	*columns = 1;
	*sampler = false;

	if (type == "float") { *kind = BASE_FLOAT; *components = 1; return true; }
	if (type == "vec2") { *kind = BASE_FLOAT; *components = 2; return true; }
	if (type == "vec3") { *kind = BASE_FLOAT; *components = 3; return true; }
	if (type == "vec4") { *kind = BASE_FLOAT; *components = 4; return true; }
	if (type == "int") { *kind = BASE_INT; *components = 1; return true; }
	if (type == "ivec2") { *kind = BASE_INT; *components = 2; return true; }
	if (type == "ivec3") { *kind = BASE_INT; *components = 3; return true; }
	if (type == "ivec4") { *kind = BASE_INT; *components = 4; return true; }
	if (type == "uint") { *kind = BASE_UINT; *components = 1; return true; }
	if (type == "uvec2") { *kind = BASE_UINT; *components = 2; return true; }
	if (type == "uvec3") { *kind = BASE_UINT; *components = 3; return true; }
	if (type == "uvec4") { *kind = BASE_UINT; *components = 4; return true; }
	if (type == "bool") { *kind = BASE_BOOL; *components = 1; return true; }
	if (type == "bvec2") { *kind = BASE_BOOL; *components = 2; return true; }
	if (type == "bvec3") { *kind = BASE_BOOL; *components = 3; return true; }
	if (type == "bvec4") { *kind = BASE_BOOL; *components = 4; return true; }

	if (type == "mat2") { *kind = BASE_FLOAT; *components = 2; *columns = 2; return true; }
	if (type == "mat3") { *kind = BASE_FLOAT; *components = 3; *columns = 3; return true; }
	if (type == "mat4") { *kind = BASE_FLOAT; *components = 4; *columns = 4; return true; }
	if (type == "mat2x2") { *kind = BASE_FLOAT; *components = 2; *columns = 2; return true; }
	if (type == "mat2x3") { *kind = BASE_FLOAT; *components = 3; *columns = 2; return true; }
	if (type == "mat2x4") { *kind = BASE_FLOAT; *components = 4; *columns = 2; return true; }
	if (type == "mat3x2") { *kind = BASE_FLOAT; *components = 2; *columns = 3; return true; }
	if (type == "mat3x3") { *kind = BASE_FLOAT; *components = 3; *columns = 3; return true; }
	if (type == "mat3x4") { *kind = BASE_FLOAT; *components = 4; *columns = 3; return true; }
	if (type == "mat4x2") { *kind = BASE_FLOAT; *components = 2; *columns = 4; return true; }
	if (type == "mat4x3") { *kind = BASE_FLOAT; *components = 3; *columns = 4; return true; }
	if (type == "mat4x4") { *kind = BASE_FLOAT; *components = 4; *columns = 4; return true; }

	if (type.compare (0, 7, "sampler") == 0) {

		*kind = BASE_SAMPLER; *components = 1; *sampler = true; return true;

	}

	return false;

}

static std::string NormalizeSamplerType (const std::string& type) {

	if (type == "samplerExternalOES") return "sampler2D";
	return type;

}

static int GLTypeFrom (int kind, int components, int columns) {

	if (kind == BASE_SAMPLER) return 0;

	if (columns > 1) {

		if (columns == 2 && components == 2) return GL_FLOAT_MAT2;
		if (columns == 3 && components == 3) return GL_FLOAT_MAT3;
		if (columns == 4 && components == 4) return GL_FLOAT_MAT4;
		if (columns == 2 && components == 3) return GL_FLOAT_MAT2x3;
		if (columns == 2 && components == 4) return GL_FLOAT_MAT2x4;
		if (columns == 3 && components == 2) return GL_FLOAT_MAT3x2;
		if (columns == 3 && components == 4) return GL_FLOAT_MAT3x4;
		if (columns == 4 && components == 2) return GL_FLOAT_MAT4x2;
		if (columns == 4 && components == 3) return GL_FLOAT_MAT4x3;

	}

	if (kind == BASE_FLOAT) {

		if (components == 1) return GL_FLOAT;
		if (components == 2) return GL_FLOAT_VEC2;
		if (components == 3) return GL_FLOAT_VEC3;
		if (components == 4) return GL_FLOAT_VEC4;

	} else if (kind == BASE_INT) {

		if (components == 1) return GL_INT;
		if (components == 2) return GL_INT_VEC2;
		if (components == 3) return GL_INT_VEC3;
		if (components == 4) return GL_INT_VEC4;

	} else if (kind == BASE_UINT) {

		if (components == 1) return GL_UNSIGNED_INT;
		if (components == 2) return GL_UNSIGNED_INT_VEC2;
		if (components == 3) return GL_UNSIGNED_INT_VEC3;
		if (components == 4) return GL_UNSIGNED_INT_VEC4;

	} else if (kind == BASE_BOOL) {

		if (components == 1) return GL_BOOL;
		if (components == 2) return GL_BOOL_VEC2;
		if (components == 3) return GL_BOOL_VEC3;
		if (components == 4) return GL_BOOL_VEC4;

	}

	return GL_FLOAT;

}

// ---------------------------------------------------------------------------
// GLSL 声明扫描
// ---------------------------------------------------------------------------

struct DeclInfo {

	std::string type;
	std::string name;
	std::string array; // 形如 "[2]"，非数组为空

};

struct ScannedStage {

	std::vector<DeclInfo> uniforms;
	std::vector<DeclInfo> varyings;
	std::vector<DeclInfo> attribs;
	std::vector<std::string> extensions;
	std::string body;

};

// 在顶层深度扫描 `keyword ...;` 声明，把声明从源码中挖空，并返回解析结果。
// 处理多声明符（uniform float a, b;）、精度限定符、数组、初始化表达式。
static bool ScanDeclarations (std::string& source, const std::string& keyword, std::vector<DeclInfo>* out) {

	bool found = false;
	size_t i = 0;
	size_t n = source.size ();
	int depth = 0;

	while (i < n) {

		char c = source[i];
		if (c == '{') { depth++; i++; continue; }
		if (c == '}') { depth--; i++; continue; }

		if (depth == 0 && IsIdentStart (c)) {

			size_t wordStart = i;
			while (i < n && IsIdentChar (source[i])) i++;
			std::string word = source.substr (wordStart, i - wordStart);

			if (word == keyword && (wordStart == 0 || !IsIdentChar (source[wordStart - 1]))) {

				size_t start = wordStart;
				size_t end = i;
				int braceDepth = 0;
				while (end < n) {

					if (source[end] == '{') braceDepth++;
					else if (source[end] == '}') braceDepth--;
					else if (source[end] == ';' && braceDepth == 0) break;
					end++;

				}
				if (end >= n) break;
				std::string statement = source.substr (start, end - start + 1);

				// uniform block（带花括号）不在本模块处理范围，保持原样
				if (statement.find ('{') == std::string::npos) {

					std::vector<std::string> tokens = Tokenize (statement);
					size_t p = 0;
					if (!tokens.empty () && tokens[0] == keyword) {

						p = 1;

					}
					while (p < tokens.size () && (IsQualifier (tokens[p]) || tokens[p] == "layout")) {

						if (tokens[p] == "layout" && p + 1 < tokens.size () && tokens[p + 1].size () > 0 && tokens[p + 1][0] == '(') {

							p += 2;

						} else {

							p++;

						}

					}
					if (p < tokens.size ()) {

						std::string type = tokens[p];
						int kind, comps, cols;
						bool sampler;
						bool validType = TypeInfo (type, &kind, &comps, &cols, &sampler);
						if (validType) {

							p++;
							std::string currentType = type;
							while (p < tokens.size ()) {

								std::string t = tokens[p];

								if (t == "=") {

									while (p < tokens.size () && tokens[p] != ",") p++;
									continue;

								}
								if (t == ",") { p++; continue; }

								if (IsIdentStart (t[0])) {

									int k2, c2, col2;
									bool s2;
									if (TypeInfo (t, &k2, &c2, &col2, &s2)) {

										// `uniform mat4 a, mat4 b` 这种非法写法不常见，当作类型跳过
										currentType = t;
										p++;
										continue;

									}
									DeclInfo decl;
									decl.type = currentType;
									decl.name = t;
									if (p + 1 < tokens.size () && tokens[p + 1].size () > 0 && tokens[p + 1][0] == '[') {

										decl.array = tokens[p + 1];
										p++;

									}
									out->push_back (decl);
									found = true;
									p++;

								} else {

									p++;

								}

							}

						}

					}

				}

				// 挖空声明，保留换行以维持行列信息
				for (size_t k = start; k <= end; k++) {

					if (source[k] != '\n') source[k] = ' ';

				}
				i = end + 1;
				continue;

			}

			continue;

		}

		i++;

	}

	return found;

}

static ScannedStage ScanStage (const std::string& rawSource, int stage) {

	ScannedStage result;
	std::string source = StripComments (StripLineEndings (rawSource));

	// 去掉 #pragma / #version，收集 #extension
	std::string filtered;
	filtered.reserve (source.size ());
	size_t pos = 0;

	while (pos < source.size ()) {

		size_t lineEnd = source.find ('\n', pos);
		if (lineEnd == std::string::npos) lineEnd = source.size ();
		std::string line = source.substr (pos, lineEnd - pos);
		std::string trimmed;
		for (size_t k = 0; k < line.size (); k++) {

			if (line[k] != ' ' && line[k] != '\t' && line[k] != '\n') trimmed.push_back (line[k]);

		}

		if (trimmed.compare (0, 7, "#pragma") == 0 || trimmed.compare (0, 8, "#version") == 0) {

			// 丢弃

		} else if (trimmed.compare (0, 10, "#extension") == 0) {

			result.extensions.push_back (line);

		} else {

			filtered += line;
			filtered.push_back ('\n');

		}

		pos = lineEnd + 1;

	}

	ScanDeclarations (filtered, "uniform", &result.uniforms);

	if (stage == STAGE_VERTEX) {

		ScanDeclarations (filtered, "attribute", &result.attribs);
		ScanDeclarations (filtered, "in", &result.attribs);
		ScanDeclarations (filtered, "varying", &result.varyings);
		ScanDeclarations (filtered, "out", &result.varyings);

	} else {

		ScanDeclarations (filtered, "varying", &result.varyings);
		ScanDeclarations (filtered, "in", &result.varyings);

	}

	// 扫描后声明已经被挖空，剩下的就是函数体和语句
	result.body = filtered;
	return result;

}

// ---------------------------------------------------------------------------
// GLSL ES 1.00 -> Vulkan GLSL 3.10 改写
// ---------------------------------------------------------------------------

struct MergedUniform {

	std::string type;
	std::string name;
	std::string array;
	int binding;
	int samplerIndex;

};

static std::string ArraySuffix (const std::string& array) {

	return array.empty () ? std::string () : (" " + array);

}

static bool IsSamplerDecl (const DeclInfo& decl) {

	if (decl.type.compare (0, 7, "sampler") == 0) return true;
	if (decl.type == "samplerExternalOES") return true;
	return false;

}

static void MergeUniforms (const ScannedStage& vertex, const ScannedStage& fragment, std::vector<MergedUniform>* out) {

	std::set<std::string> seen;
	std::vector<DeclInfo> all = vertex.uniforms;
	all.insert (all.end (), fragment.uniforms.begin (), fragment.uniforms.end ());

	for (size_t i = 0; i < all.size (); i++) {

		if (seen.find (all[i].name) != seen.end ()) continue;
		seen.insert (all[i].name);

		MergedUniform u;
		u.type = NormalizeSamplerType (all[i].type);
		u.name = all[i].name;
		u.array = all[i].array;
		u.binding = 0;
		u.samplerIndex = -1;
		out->push_back (u);

	}

	int samplerIndex = 0;
	for (size_t i = 0; i < out->size (); i++) {

		if (out->at (i).type.compare (0, 7, "sampler") == 0) {

			out->at (i).samplerIndex = samplerIndex;
			out->at (i).binding = 1 + samplerIndex;
			samplerIndex++;

		}

	}

}

// 合并 varyings：先顶点 out，再片元 in；同名只占一个 location
static void MergeVaryings (const ScannedStage& vertex, const ScannedStage& fragment,
	std::vector<DeclInfo>* out, std::map<std::string, int>* locations) {

	std::set<std::string> seen;
	std::vector<DeclInfo> all = vertex.varyings;
	all.insert (all.end (), fragment.varyings.begin (), fragment.varyings.end ());

	int next = 0;
	for (size_t i = 0; i < all.size (); i++) {

		if (seen.find (all[i].name) != seen.end ()) continue;
		seen.insert (all[i].name);
		out->push_back (all[i]);
		locations->insert (std::make_pair (all[i].name, next));
		next++;

	}

}

static void AssignAttributeLocations (const std::vector<DeclInfo>& attribs, const std::map<std::string, int>& userBindings,
	std::map<std::string, int>* locations) {

	std::set<int> used;
	for (std::map<std::string, int>::const_iterator it = userBindings.begin (); it != userBindings.end (); ++it) {

		used.insert (it->second);

	}

	int next = 0;
	for (size_t i = 0; i < attribs.size (); i++) {

		std::map<std::string, int>::const_iterator bound = userBindings.find (attribs[i].name);
		if (bound != userBindings.end ()) {

			locations->insert (std::make_pair (attribs[i].name, bound->second));
			continue;

		}
		if (locations->find (attribs[i].name) != locations->end ()) continue;

		while (used.find (next) != used.end ()) next++;
		locations->insert (std::make_pair (attribs[i].name, next));
		used.insert (next);
		next++;

	}

}

// 函数体里的 ES 1.00 内建名在 3.10 下不存在，按标识符逐个替换（不会误伤同前缀的名字）
static std::string RewriteBody (const std::string& body) {

	std::string out;
	out.reserve (body.size ());
	size_t i = 0;
	size_t n = body.size ();

	while (i < n) {

		char c = body[i];
		if (IsIdentStart (c)) {

			size_t start = i;
			while (i < n && IsIdentChar (body[i])) i++;
			std::string word = body.substr (start, i - start);

			if (word == "gl_FragColor") out += "_limeFragColor";
			else if (word == "texture2D") out += "texture";
			else if (word == "textureCube") out += "texture";
			else if (word == "texture2DProj") out += "textureProj";
			else if (word == "gl_FragData") out += "_limeFragData";
			else out += word;

			continue;

		}
		out.push_back (c);
		i++;

	}
	return out;

}

static std::string BuildStageSource (const ScannedStage& stage, int shaderStage,
	const std::vector<MergedUniform>& uniforms, const std::vector<DeclInfo>& varyings,
	const std::map<std::string, int>& varyingLocations, const std::map<std::string, int>& attribLocations,
	const std::vector<DeclInfo>& attribs) {

	std::string out;
	out.reserve (stage.body.size () + 512);
	out += "#version 310 es\n";
	out += "precision highp float;\n";
	out += "precision highp int;\n";
	out += "precision highp sampler2D;\n";

	for (size_t i = 0; i < stage.extensions.size (); i++) {

		out += stage.extensions[i];
		out += "\n";

	}

	// 合成一个匿名 std140 uniform block，顶点/片元使用完全相同的成员顺序与类型，
	// 这样同一个 VkBuffer 可以同时绑定到两个 stage。
	bool hasBlockMembers = false;
	std::vector<std::string> members;
	for (size_t i = 0; i < uniforms.size (); i++) {

		if (uniforms[i].samplerIndex >= 0) continue;
		members.push_back ("    " + uniforms[i].type + " " + uniforms[i].name + ArraySuffix (uniforms[i].array) + ";");
		hasBlockMembers = true;

	}
	if (hasBlockMembers) {

		out += "layout(set = 0, binding = 0, std140) uniform LimeUniforms {\n";
		for (size_t i = 0; i < members.size (); i++) {

			out += members[i];
			out += "\n";

		}
		out += "};\n";

	}

	for (size_t i = 0; i < uniforms.size (); i++) {

		if (uniforms[i].samplerIndex < 0) continue;
		out += "layout(set = 0, binding = " + std::to_string (uniforms[i].binding) + ") uniform "
			+ uniforms[i].type + " " + uniforms[i].name + ArraySuffix (uniforms[i].array) + ";\n";

	}

	bool hasFragColor = stage.body.find ("gl_FragColor") != std::string::npos;
	if (shaderStage == STAGE_FRAGMENT && hasFragColor) {

		out += "layout(location = 0) out vec4 _limeFragColor;\n";

	}

	for (size_t i = 0; i < varyings.size (); i++) {

		std::map<std::string, int>::const_iterator loc = varyingLocations.find (varyings[i].name);
		int location = (loc == varyingLocations.end ()) ? 0 : loc->second;
		std::string keyword = (shaderStage == STAGE_VERTEX) ? "out" : "in";
		out += "layout(location = " + std::to_string (location) + ") " + keyword + " "
			+ varyings[i].type + " " + varyings[i].name + ArraySuffix (varyings[i].array) + ";\n";

	}

	if (shaderStage == STAGE_VERTEX) {

		for (size_t i = 0; i < attribs.size (); i++) {

			std::map<std::string, int>::const_iterator loc = attribLocations.find (attribs[i].name);
			int location = (loc == attribLocations.end ()) ? 0 : loc->second;
			out += "layout(location = " + std::to_string (location) + ") in "
				+ attribs[i].type + " " + attribs[i].name + ArraySuffix (attribs[i].array) + ";\n";

		}

	}

	out += "\n";
	out += RewriteBody (stage.body);
	return out;

}

// ---------------------------------------------------------------------------
// glslang 封装（运行时 GLSL -> SPIR-V）
// ---------------------------------------------------------------------------

static bool g_glslangReady = false;

static void EnsureGlslang () {

	if (!g_glslangReady) {

		glslang_initialize_process ();
		g_glslangReady = true;

	}

}

static bool CompileStage (const std::string& code, int shaderStage, std::vector<uint32_t>* spirv, std::string* log) {

	EnsureGlslang ();

	glslang_input_t input = {};
	input.language = GLSLANG_SOURCE_GLSL;
	input.stage = (shaderStage == STAGE_VERTEX) ? GLSLANG_STAGE_VERTEX : GLSLANG_STAGE_FRAGMENT;
	input.client = GLSLANG_CLIENT_VULKAN;
	input.client_version = GLSLANG_TARGET_VULKAN_1_0;
	input.target_language = GLSLANG_TARGET_SPV;
	input.target_language_version = GLSLANG_TARGET_SPV_1_0;
	input.code = code.c_str ();
	input.default_version = 310;
	input.default_profile = GLSLANG_NO_PROFILE;
	input.force_default_version_and_profile = 0;
	input.forward_compatible = 0;
	input.messages = (glslang_messages_t) (GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT | GLSLANG_MSG_ENHANCED);
	input.resource = glslang_default_resource ();
	input.callbacks = glsl_include_callbacks_t ();
	input.callbacks_ctx = NULL;

	glslang_shader_t* shader = glslang_shader_create (&input);
	glslang_shader_set_options (shader, GLSLANG_SHADER_AUTO_MAP_BINDINGS | GLSLANG_SHADER_AUTO_MAP_LOCATIONS);

	if (!glslang_shader_preprocess (shader, &input)) {

		*log += glslang_shader_get_info_log (shader);
		*log += glslang_shader_get_info_debug_log (shader);
		glslang_shader_delete (shader);
		return false;

	}

	if (!glslang_shader_parse (shader, &input)) {

		*log += glslang_shader_get_info_log (shader);
		*log += glslang_shader_get_info_debug_log (shader);
		glslang_shader_delete (shader);
		return false;

	}

	glslang_program_t* program = glslang_program_create ();
	glslang_program_add_shader (program, shader);

	if (!glslang_program_link (program, (glslang_messages_t) (GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT | GLSLANG_MSG_ENHANCED))) {

		*log += glslang_program_get_info_log (program);
		*log += glslang_program_get_info_debug_log (program);
		glslang_program_delete (program);
		glslang_shader_delete (shader);
		return false;

	}

	glslang_program_SPIRV_generate (program, input.stage);

	const char* messages = glslang_program_SPIRV_get_messages (program);
	if (messages && messages[0]) {

		*log += messages;

	}

	size_t size = glslang_program_SPIRV_get_size (program);
	unsigned int* words = glslang_program_SPIRV_get_ptr (program);
	if (size == 0 || !words) {

		*log += "glslang produced empty SPIR-V\n";
		glslang_program_delete (program);
		glslang_shader_delete (shader);
		return false;

	}

	spirv->assign (words, words + size);

	glslang_program_delete (program);
	glslang_shader_delete (shader);
	return true;

}

// ---------------------------------------------------------------------------
// SPIR-V 反射：std140 uniform block 的成员偏移 / stride
//
// glslang 会把匿名 block 的成员按 std140 重新排布，layout 由它决定，
// 所以偏移不能自己猜，必须从 SPIR-V 的 OpMemberDecorate 里读回来。
// ---------------------------------------------------------------------------

struct SpvTypeNode {

	int opcode;
	uint32_t a;                  // 分量 / 元素 / 指向的类型 id
	uint32_t b;                  // 向量分量数、矩阵列数、数组长度 id、指针 storage class
	std::vector<uint32_t> members;

	SpvTypeNode () : opcode (0), a (0), b (0) {}

};

struct SpvMemberLayout {

	uint32_t offset;
	uint32_t arrayStride;
	uint32_t matrixStride;
	bool rowMajor;
	bool hasOffset;

	SpvMemberLayout () : offset (0), arrayStride (0), matrixStride (0), rowMajor (false), hasOffset (false) {}

};

struct SpvReflection {

	std::map<uint32_t, SpvTypeNode> types;
	std::map<uint32_t, uint32_t> constants;
	std::map<uint32_t, std::vector<std::string> > memberNames;
	std::map<uint32_t, std::vector<SpvMemberLayout> > memberLayouts;
	std::map<uint32_t, bool> isBlock;

};

// SPIR-V 里的字符串是按 4 字节小端打包的
static std::string SpvDecodeString (const std::vector<uint32_t>& words, size_t start, size_t end) {

	std::string s;
	for (size_t i = start; i < end && i < words.size (); i++) {

		uint32_t w = words[i];
		for (int b = 0; b < 4; b++) {

			char c = (char) ((w >> (b * 8)) & 0xFF);
			if (c == 0) return s;
			s.push_back (c);

		}

	}
	return s;

}

static SpvMemberLayout& EnsureMemberLayout (SpvReflection* r, uint32_t structId, uint32_t index) {

	std::vector<SpvMemberLayout>& list = r->memberLayouts[structId];
	if (list.size () <= index) list.resize (index + 1);
	return list[index];

}

static bool ParseSpirv (const std::vector<uint32_t>& words, SpvReflection* r) {

	if (words.size () < 5) return false;

	size_t i = 5;
	while (i < words.size ()) {

		uint32_t head = words[i];
		uint32_t wc = head >> 16;
		uint32_t op = head & 0xFFFF;
		if (wc == 0) break;

		switch (op) {

		case 5: // OpName（全局名本模块不需要，OpMemberName 才是有用的）
			break;

		case 6: // OpMemberName
			if (wc >= 3) {

				uint32_t structId = words[i + 1];
				uint32_t index = words[i + 2];
				std::vector<std::string>& names = r->memberNames[structId];
				if (names.size () <= index) names.resize (index + 1);
				names[index] = SpvDecodeString (words, i + 3, i + wc);

			}
			break;

		case 71: // OpDecorate
			if (wc >= 3) {

				uint32_t target = words[i + 1];
				uint32_t decoration = words[i + 2];
				if (decoration == 2 || decoration == 3) r->isBlock[target] = true;

			}
			break;

		case 72: // OpMemberDecorate
			if (wc >= 4) {

				uint32_t structId = words[i + 1];
				uint32_t index = words[i + 2];
				uint32_t decoration = words[i + 3];
				SpvMemberLayout& ml = EnsureMemberLayout (r, structId, index);

				if (decoration == 35) { ml.offset = words[i + 4]; ml.hasOffset = true; }
				else if (decoration == 6) ml.arrayStride = words[i + 4];
				else if (decoration == 7) ml.matrixStride = words[i + 4];
				else if (decoration == 4) ml.rowMajor = true;

			}
			break;

		case 20: // OpTypeBool
			r->types[words[i + 1]].opcode = 20;
			break;

		case 21: // OpTypeInt
			r->types[words[i + 1]].opcode = 21;
			break;

		case 22: // OpTypeFloat
			r->types[words[i + 1]].opcode = 22;
			break;

		case 23: // OpTypeVector
			{

				SpvTypeNode& t = r->types[words[i + 1]];
				t.opcode = 23;
				t.a = words[i + 2];
				t.b = words[i + 3];

			}
			break;

		case 24: // OpTypeMatrix
			{

				SpvTypeNode& t = r->types[words[i + 1]];
				t.opcode = 24;
				t.a = words[i + 2];
				t.b = words[i + 3];

			}
			break;

		case 25: // OpTypeImage
		case 27: // OpTypeSampledImage
			r->types[words[i + 1]].opcode = (int) op;
			break;

		case 28: // OpTypeArray
			{

				SpvTypeNode& t = r->types[words[i + 1]];
				t.opcode = 28;
				t.a = words[i + 2];
				t.b = words[i + 3];

			}
			break;

		case 29: // OpTypeRuntimeArray
			{

				SpvTypeNode& t = r->types[words[i + 1]];
				t.opcode = 29;
				t.a = words[i + 2];

			}
			break;

		case 30: // OpTypeStruct
			{

				SpvTypeNode& t = r->types[words[i + 1]];
				t.opcode = 30;
				for (size_t k = i + 2; k < i + wc; k++) t.members.push_back (words[k]);

			}
			break;

		case 32: // OpTypePointer
			{

				SpvTypeNode& t = r->types[words[i + 1]];
				t.opcode = 32;
				t.b = words[i + 2];
				t.a = words[i + 3];

			}
			break;

		case 43: // OpConstant
			if (wc >= 4) r->constants[words[i + 2]] = words[i + 3];
			break;

		default:
			break;

		}

		i += wc;

	}

	return true;

}

static uint32_t SpvArrayLength (const SpvReflection& r, uint32_t lengthId) {

	std::map<uint32_t, uint32_t>::const_iterator it = r.constants.find (lengthId);
	return (it == r.constants.end ()) ? 1 : it->second;

}

// std140 下类型的自然大小。偏移已经由 SPIR-V 给出，这里只用于算 block 总大小。
static uint32_t SpvTypeBytes (const SpvReflection& r, uint32_t typeId) {

	std::map<uint32_t, SpvTypeNode>::const_iterator it = r.types.find (typeId);
	if (it == r.types.end ()) return 4;

	const SpvTypeNode& t = it->second;
	switch (t.opcode) {

	case 20:
	case 21:
	case 22:
		return 4;

	case 23:
		return 4 * t.b;

	case 24:
		return SpvTypeBytes (r, t.a) * t.b;

	case 25:
	case 27:
	case 32:
		return 8;

	case 28:
	case 29:
		{

			uint32_t len = (t.opcode == 28) ? SpvArrayLength (r, t.b) : 1;
			uint32_t elem = SpvTypeBytes (r, t.a);
			uint32_t stride = (elem + 15) & ~15u;
			return stride * len;

		}

	case 30:
		{

			uint32_t size = 0;
			for (size_t i = 0; i < t.members.size (); i++) {

				uint32_t m = SpvTypeBytes (r, t.members[i]);
				size += (m + 15) & ~15u;

			}
			return size;

		}

	}

	return 4;

}

static uint32_t SpvMemberBytes (const SpvReflection& r, uint32_t typeId, const SpvMemberLayout& ml) {

	std::map<uint32_t, SpvTypeNode>::const_iterator it = r.types.find (typeId);
	if (it == r.types.end ()) return 4;

	const SpvTypeNode& t = it->second;

	if (t.opcode == 28 || t.opcode == 29) {

		if (ml.arrayStride > 0) {

			uint32_t len = (t.opcode == 28) ? SpvArrayLength (r, t.b) : 1;
			return ml.arrayStride * len;

		}

	}

	if (t.opcode == 24 && ml.matrixStride > 0) {

		return ml.matrixStride * t.b;

	}

	return SpvTypeBytes (r, typeId);

}

// 读取第一个带 Block 装饰的 struct，输出成员名 -> layout 以及 std140 字节大小
static bool ReflectUniformBlock (const std::vector<uint32_t>& words,
	std::map<std::string, SpvMemberLayout>* members, uint32_t* blockSize) {

	SpvReflection r;
	if (!ParseSpirv (words, &r)) return false;

	uint32_t blockId = 0;
	for (std::map<uint32_t, bool>::const_iterator it = r.isBlock.begin (); it != r.isBlock.end (); ++it) {

		if (it->second) { blockId = it->first; break; }

	}
	if (blockId == 0) return false;

	std::map<uint32_t, SpvTypeNode>::const_iterator type = r.types.find (blockId);
	if (type == r.types.end () || type->second.opcode != 30) return false;

	std::map<uint32_t, std::vector<SpvMemberLayout> >::const_iterator layouts = r.memberLayouts.find (blockId);
	std::map<uint32_t, std::vector<std::string> >::const_iterator names = r.memberNames.find (blockId);

	uint32_t end = 0;
	const std::vector<uint32_t>& memberTypes = type->second.members;

	for (size_t i = 0; i < memberTypes.size (); i++) {

		std::string name;
		if (names != r.memberNames.end () && i < names->second.size ()) name = names->second[i];
		if (name.empty ()) continue;

		SpvMemberLayout ml;
		if (layouts != r.memberLayouts.end () && i < layouts->second.size ()) ml = layouts->second[i];
		if (!ml.hasOffset) continue;

		members->insert (std::make_pair (name, ml));

		uint32_t bytes = SpvMemberBytes (r, memberTypes[i], ml);
		if (ml.offset + bytes > end) end = ml.offset + bytes;

	}

	*blockSize = (end + 15) & ~15u;
	return !members->empty ();

}

// ---------------------------------------------------------------------------
// program 的链接：扫描两 stage -> 合并 uniform/varying -> 生成 Vulkan GLSL
//                    -> glslang 编译 -> SPIR-V 反射 -> 建立 GL 的 location 表
// ---------------------------------------------------------------------------

static void DestroyPipelineSlot (PipelineSlot* slot);

static int ParseArrayCount (const std::string& array) {

	size_t open = array.find ('[');
	size_t close = array.find (']');
	if (open == std::string::npos || close == std::string::npos || close <= open + 1) return 1;

	std::string digits = array.substr (open + 1, close - open - 1);
	int value = 0;
	for (size_t i = 0; i < digits.size (); i++) {

		if (digits[i] < '0' || digits[i] > '9') return 1;
		value = value * 10 + (digits[i] - '0');

	}
	return value > 0 ? value : 1;

}

static bool IsSamplerTypeName (const std::string& type) {

	return type.compare (0, 7, "sampler") == 0;

}

static int SamplerGLType (const std::string& type) {

	if (type == "sampler3D") return GL_SAMPLER_3D;
	if (type == "samplerCube") return GL_SAMPLER_CUBE;
	if (type == "sampler2DShadow") return GL_SAMPLER_2D_SHADOW;
	if (type == "sampler2DArray") return GL_SAMPLER_2D_ARRAY;
	if (type == "sampler2DArrayShadow") return GL_SAMPLER_2D_ARRAY_SHADOW;
	if (type == "samplerCubeShadow") return GL_SAMPLER_CUBE_SHADOW;

	return GL_SAMPLER_2D;

}

static uint32_t AlignUp (uint32_t value, uint32_t alignment) {

	return (value + alignment - 1) & ~(alignment - 1);

}

// 反射失败时的兜底：按 std140 规则顺序打包（和 glslang 对同序成员的排布一致）
static uint32_t PackUniformsStd140 (std::vector<UniformInfo>* uniforms) {

	uint32_t cursor = 0;

	for (size_t i = 0; i < uniforms->size (); i++) {

		UniformInfo& u = uniforms->at (i);
		if (u.kind == BASE_SAMPLER) continue;

		uint32_t align = 4;
		uint32_t elemSize = 4;

		if (u.columns > 1) {

			align = 16;
			elemSize = 16u * u.columns;

		} else if (u.components > 1) {

			align = (u.components == 2) ? 8 : 16;
			elemSize = 4u * u.components;

		}

		if (u.arraySize > 1) align = 16;

		cursor = AlignUp (cursor, align);
		u.offset = cursor;
		u.matrixStride = (u.columns > 1) ? 16u : 0;
		u.arrayStride = (u.arraySize > 1) ? AlignUp (elemSize, 16) : 0;
		u.byteSize = elemSize;
		cursor += (u.arraySize > 1) ? (u.arrayStride * u.arraySize) : elemSize;

	}

	return AlignUp (cursor, 16);

}

static bool BuildProgram (Program* p) {

	Shader* vertexShader = NULL;
	Shader* fragmentShader = NULL;

	for (size_t i = 0; i < p->attached.size (); i++) {

		Shader* shader = FindShader (p->attached[i]);
		if (!shader) continue;
		if (shader->glType == GL_VERTEX_SHADER) vertexShader = shader;
		else if (shader->glType == GL_FRAGMENT_SHADER) fragmentShader = shader;

	}

	if (!vertexShader || !fragmentShader) {

		p->log = "link error: a program needs one compiled vertex and one compiled fragment shader";
		return false;

	}

	if (!vertexShader->compileOk) {

		p->log = "link error: vertex shader " + std::to_string (vertexShader->id) + " did not compile";
		return false;

	}

	if (!fragmentShader->compileOk) {

		p->log = "link error: fragment shader " + std::to_string (fragmentShader->id) + " did not compile";
		return false;

	}

	ScannedStage vertexStage = ScanStage (vertexShader->source, STAGE_VERTEX);
	ScannedStage fragmentStage = ScanStage (fragmentShader->source, STAGE_FRAGMENT);

	std::vector<MergedUniform> merged;
	MergeUniforms (vertexStage, fragmentStage, &merged);

	std::vector<DeclInfo> varyings;
	std::map<std::string, int> varyingLocations;
	MergeVaryings (vertexStage, fragmentStage, &varyings, &varyingLocations);

	std::map<std::string, int> attribLocations;
	AssignAttributeLocations (vertexStage.attribs, p->attribBindings, &attribLocations);

	std::vector<DeclInfo> noAttribs;

	p->glslVertex = BuildStageSource (vertexStage, STAGE_VERTEX, merged, varyings,
		varyingLocations, attribLocations, vertexStage.attribs);
	p->glslFragment = BuildStageSource (fragmentStage, STAGE_FRAGMENT, merged, varyings,
		varyingLocations, attribLocations, noAttribs);

	std::string stageLog;
	std::vector<uint32_t> vertexSpirv;
	std::vector<uint32_t> fragmentSpirv;

	if (!CompileStage (p->glslVertex, STAGE_VERTEX, &vertexSpirv, &stageLog)) {

		p->log = "vertex shader compile error:\n" + stageLog;
		return false;

	}

	if (!CompileStage (p->glslFragment, STAGE_FRAGMENT, &fragmentSpirv, &stageLog)) {

		p->log = "fragment shader compile error:\n" + stageLog;
		return false;

	}

	p->spirvVertex.swap (vertexSpirv);
	p->spirvFragment.swap (fragmentSpirv);

	// ---- uniform 表 ----
	p->uniforms.clear ();

	std::map<std::string, SpvMemberLayout> members;
	uint32_t reflectedSize = 0;
	bool reflected = ReflectUniformBlock (p->spirvVertex, &members, &reflectedSize);

	for (size_t i = 0; i < merged.size (); i++) {

		const MergedUniform& src = merged[i];
		UniformInfo info;

		info.name = src.name;
		info.arraySize = ParseArrayCount (src.array);
		info.glName = src.name;
		if (info.arraySize > 1) info.glName += "[0]";

		int kind = 0, components = 1, columns = 1;
		bool sampler = false;
		TypeInfo (src.type, &kind, &components, &columns, &sampler);

		info.kind = kind;
		info.components = components;
		info.columns = columns;
		info.binding = src.binding;
		info.textureUnit = 0;
		info.offset = 0;
		info.arrayStride = 0;
		info.matrixStride = 0;
		info.rowMajor = false;

		if (sampler) {

			info.glType = SamplerGLType (src.type);
			info.byteSize = 0;

		} else {

			info.glType = GLTypeFrom (kind, components, columns);
			info.byteSize = 4u * components;

			if (reflected) {

				std::map<std::string, SpvMemberLayout>::const_iterator layout = members.find (src.name);
				if (layout != members.end ()) {

					info.offset = layout->second.offset;
					info.arrayStride = layout->second.arrayStride;
					info.matrixStride = layout->second.matrixStride;
					info.rowMajor = layout->second.rowMajor;

				}

			}

			if (columns > 1 && info.arrayStride > 0 && info.arraySize > 1) info.byteSize = info.arrayStride;
			else if (columns > 1) info.byteSize = 16u * columns;
			else if (info.arraySize > 1 && info.arrayStride > 0) info.byteSize = info.arrayStride;

		}

		p->uniforms.push_back (info);

	}

	if (!reflected) {

		p->uniformSize = PackUniformsStd140 (&p->uniforms);

	} else {

		p->uniformSize = reflectedSize;

	}

	// ---- GL location 分配：数组每个元素占一个 location ----
	p->locationSlot.clear ();
	p->locationElem.clear ();
	p->nextLocation = 0;

	for (size_t i = 0; i < p->uniforms.size (); i++) {

		UniformInfo& u = p->uniforms[i];
		u.location = p->nextLocation;

		for (int e = 0; e < u.arraySize; e++) {

			p->locationSlot.push_back ((int) i);
			p->locationElem.push_back (e);
			p->nextLocation++;

		}

	}

	// ---- 顶点属性 ----
	p->attribs.clear ();
	p->attribLocations.clear ();

	for (size_t i = 0; i < vertexStage.attribs.size (); i++) {

		const DeclInfo& decl = vertexStage.attribs[i];
		std::map<std::string, int>::const_iterator location = attribLocations.find (decl.name);
		if (location == attribLocations.end ()) continue;

		AttribInfo attrib;
		attrib.name = decl.name;
		attrib.location = location->second;
		attrib.arraySize = ParseArrayCount (decl.array);

		int kind = 0, components = 1, columns = 1;
		bool sampler = false;
		TypeInfo (decl.type, &kind, &components, &columns, &sampler);
		attrib.glType = GLTypeFrom (kind, components, columns);

		p->attribs.push_back (attrib);
		p->attribLocations[decl.name] = attrib.location;

	}

	p->uniformData.assign (p->uniformSize, 0);
	p->uniformDirty = true;

	DestroyPipelineSlot (&p->pipelineBack);
	DestroyPipelineSlot (&p->pipelineOffscreen);

	p->linked = true;
	p->linkOk = true;
	p->validateOk = false;
	p->log.clear ();
	return true;

}

// ---------------------------------------------------------------------------
// Vulkan 侧：pipeline / descriptor set / uniform buffer
// ---------------------------------------------------------------------------

struct VkDeviceFns {

	PFN_vkCreateShaderModule CreateShaderModule;
	PFN_vkDestroyShaderModule DestroyShaderModule;
	PFN_vkCreatePipelineLayout CreatePipelineLayout;
	PFN_vkDestroyPipelineLayout DestroyPipelineLayout;
	PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines;
	PFN_vkDestroyPipeline DestroyPipeline;
	PFN_vkCreateDescriptorSetLayout CreateDescriptorSetLayout;
	PFN_vkDestroyDescriptorSetLayout DestroyDescriptorSetLayout;
	PFN_vkCreateDescriptorPool CreateDescriptorPool;
	PFN_vkDestroyDescriptorPool DestroyDescriptorPool;
	PFN_vkAllocateDescriptorSets AllocateDescriptorSets;
	PFN_vkUpdateDescriptorSets UpdateDescriptorSets;
	PFN_vkCreateBuffer CreateBuffer;
	PFN_vkDestroyBuffer DestroyBuffer;
	PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements;
	PFN_vkAllocateMemory AllocateMemory;
	PFN_vkFreeMemory FreeMemory;
	PFN_vkBindBufferMemory BindBufferMemory;
	PFN_vkMapMemory MapMemory;
	PFN_vkUnmapMemory UnmapMemory;
	PFN_vkCreateSampler CreateSampler;
	PFN_vkDestroySampler DestroySampler;

};

static VkDeviceFns g_vkFns;
static GLShaderVkContext g_vkCtx;
static bool g_vkReady = false;
static VkSampler g_defaultSampler = VK_NULL_HANDLE;

static uint64_t FnvMix (uint64_t hash, uint64_t value) {

	hash ^= value;
	hash *= 1099511628211ULL;
	return hash;

}

static uint32_t VertexFormatForComponents (int components) {

	if (components == 1) return (uint32_t) VK_FORMAT_R32_SFLOAT;
	if (components == 2) return (uint32_t) VK_FORMAT_R32G32_SFLOAT;
	if (components == 3) return (uint32_t) VK_FORMAT_R32G32B32_SFLOAT;
	return (uint32_t) VK_FORMAT_R32G32B32A32_SFLOAT;

}

static void DestroyPipelineSlot (PipelineSlot* slot) {

	VkDevice device = g_vkCtx.device;
	bool destroyable = g_vkReady && device;

	if (destroyable) {

		if (slot->mapped) {

			g_vkFns.UnmapMemory (device, slot->uniformMemory);
			slot->mapped = NULL;

		}
		if (slot->uniformBuffer) g_vkFns.DestroyBuffer (device, slot->uniformBuffer, NULL);
		if (slot->uniformMemory) g_vkFns.FreeMemory (device, slot->uniformMemory, NULL);
		if (slot->pipeline) g_vkFns.DestroyPipeline (device, slot->pipeline, NULL);
		if (slot->layout) g_vkFns.DestroyPipelineLayout (device, slot->layout, NULL);
		if (slot->descriptorPool) g_vkFns.DestroyDescriptorPool (device, slot->descriptorPool, NULL);
		if (slot->descriptorLayout) g_vkFns.DestroyDescriptorSetLayout (device, slot->descriptorLayout, NULL);

	}

	slot->uniformBuffer = VK_NULL_HANDLE;
	slot->uniformMemory = VK_NULL_HANDLE;
	slot->pipeline = VK_NULL_HANDLE;
	slot->layout = VK_NULL_HANDLE;
	slot->descriptorPool = VK_NULL_HANDLE;
	slot->descriptorSet = VK_NULL_HANDLE;
	slot->descriptorLayout = VK_NULL_HANDLE;
	slot->bufferSize = 0;
	slot->built = false;
	slot->fingerprint = 0;

}

static VkShaderModule CreateShaderModule (const std::vector<uint32_t>& spirv) {

	if (spirv.empty ()) return VK_NULL_HANDLE;

	VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	info.codeSize = spirv.size () * sizeof (uint32_t);
	info.pCode = spirv.data ();

	VkShaderModule module = VK_NULL_HANDLE;
	if (g_vkFns.CreateShaderModule (g_vkCtx.device, &info, NULL, &module) != VK_SUCCESS) return VK_NULL_HANDLE;
	return module;

}

static bool CreatePipelineSlot (Program* p, PipelineSlot* slot, const GLShaderVertexLayout* layout,
	bool offscreen) {

	if (!g_vkReady || !p || !p->linkOk) return false;

	VkRenderPass renderPass = offscreen ? g_vkCtx.offscreenRenderPass : g_vkCtx.backbufferRenderPass;
	if (!renderPass) return false;

	uint32_t width = g_vkCtx.viewportWidth;
	uint32_t height = g_vkCtx.viewportHeight;
	if (width == 0 || height == 0) return false;

	VkDevice device = g_vkCtx.device;

	VkShaderModule vertexModule = CreateShaderModule (p->spirvVertex);
	VkShaderModule fragmentModule = CreateShaderModule (p->spirvFragment);

	if (!vertexModule || !fragmentModule) {

		if (vertexModule) g_vkFns.DestroyShaderModule (device, vertexModule, NULL);
		if (fragmentModule) g_vkFns.DestroyShaderModule (device, fragmentModule, NULL);
		return false;

	}

	// ---- descriptor set layout ----
	std::vector<VkDescriptorSetLayoutBinding> bindings;
	int samplerCount = 0;

	if (p->uniformSize > 0) {

		VkDescriptorSetLayoutBinding binding{};
		binding.binding = 0;
		binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		binding.descriptorCount = 1;
		binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
		bindings.push_back (binding);

	}

	for (size_t i = 0; i < p->uniforms.size (); i++) {

		if (p->uniforms[i].kind != BASE_SAMPLER) continue;

		VkDescriptorSetLayoutBinding binding{};
		binding.binding = p->uniforms[i].binding;
		binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		binding.descriptorCount = 1;
		binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
		bindings.push_back (binding);
		samplerCount++;

	}

	VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	layoutInfo.bindingCount = (uint32_t) bindings.size ();
	layoutInfo.pBindings = bindings.empty () ? NULL : bindings.data ();

	VkDescriptorSetLayout descriptorLayout = VK_NULL_HANDLE;
	if (g_vkFns.CreateDescriptorSetLayout (device, &layoutInfo, NULL, &descriptorLayout) != VK_SUCCESS) {

		g_vkFns.DestroyShaderModule (device, vertexModule, NULL);
		g_vkFns.DestroyShaderModule (device, fragmentModule, NULL);
		return false;

	}

	// ---- descriptor pool + set ----
	std::vector<VkDescriptorPoolSize> poolSizes;
	if (p->uniformSize > 0) {

		VkDescriptorPoolSize size{};
		size.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		size.descriptorCount = 1;
		poolSizes.push_back (size);

	}
	if (samplerCount > 0) {

		VkDescriptorPoolSize size{};
		size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		size.descriptorCount = (uint32_t) samplerCount;
		poolSizes.push_back (size);

	}

	VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	poolInfo.maxSets = 1;
	poolInfo.poolSizeCount = (uint32_t) poolSizes.size ();
	poolInfo.pPoolSizes = poolSizes.empty () ? NULL : poolSizes.data ();

	VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
	VkDescriptorSet descriptorSet = VK_NULL_HANDLE;

	if (g_vkFns.CreateDescriptorPool (device, &poolInfo, NULL, &descriptorPool) == VK_SUCCESS) {

		VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
		allocInfo.descriptorPool = descriptorPool;
		allocInfo.descriptorSetCount = 1;
		allocInfo.pSetLayouts = &descriptorLayout;
		g_vkFns.AllocateDescriptorSets (device, &allocInfo, &descriptorSet);

	}

	// ---- uniform buffer ----
	VkBuffer uniformBuffer = VK_NULL_HANDLE;
	VkDeviceMemory uniformMemory = VK_NULL_HANDLE;
	void* mapped = NULL;
	uint32_t bufferSize = p->uniformSize;

	if (bufferSize > 0) {

		if (bufferSize < 16) bufferSize = 16;

		VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
		bufferInfo.size = bufferSize;
		bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
		bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

		if (g_vkFns.CreateBuffer (device, &bufferInfo, NULL, &uniformBuffer) != VK_SUCCESS) {

			g_vkFns.DestroyDescriptorPool (device, descriptorPool, NULL);
			g_vkFns.DestroyDescriptorSetLayout (device, descriptorLayout, NULL);
			g_vkFns.DestroyShaderModule (device, vertexModule, NULL);
			g_vkFns.DestroyShaderModule (device, fragmentModule, NULL);
			return false;

		}

		VkMemoryRequirements requirements{};
		g_vkFns.GetBufferMemoryRequirements (device, uniformBuffer, &requirements);

		VkMemoryAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		allocateInfo.allocationSize = requirements.size;
		allocateInfo.memoryTypeIndex = g_vkCtx.uniformMemoryTypeIndex;

		if (g_vkFns.AllocateMemory (device, &allocateInfo, NULL, &uniformMemory) != VK_SUCCESS) {

			g_vkFns.DestroyBuffer (device, uniformBuffer, NULL);
			g_vkFns.DestroyDescriptorPool (device, descriptorPool, NULL);
			g_vkFns.DestroyDescriptorSetLayout (device, descriptorLayout, NULL);
			g_vkFns.DestroyShaderModule (device, vertexModule, NULL);
			g_vkFns.DestroyShaderModule (device, fragmentModule, NULL);
			return false;

		}

		g_vkFns.BindBufferMemory (device, uniformBuffer, uniformMemory, 0);
		g_vkFns.MapMemory (device, uniformMemory, 0, bufferSize, 0, &mapped);

		if (mapped && !p->uniformData.empty ()) {

			memcpy (mapped, p->uniformData.data (), p->uniformData.size ());

		}

	}

	// ---- pipeline layout ----
	VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	pipelineLayoutInfo.setLayoutCount = 1;
	pipelineLayoutInfo.pSetLayouts = &descriptorLayout;

	VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
	if (g_vkFns.CreatePipelineLayout (device, &pipelineLayoutInfo, NULL, &pipelineLayout) != VK_SUCCESS) {

		if (uniformMemory) g_vkFns.FreeMemory (device, uniformMemory, NULL);
		if (uniformBuffer) g_vkFns.DestroyBuffer (device, uniformBuffer, NULL);
		g_vkFns.DestroyDescriptorPool (device, descriptorPool, NULL);
		g_vkFns.DestroyDescriptorSetLayout (device, descriptorLayout, NULL);
		g_vkFns.DestroyShaderModule (device, vertexModule, NULL);
		g_vkFns.DestroyShaderModule (device, fragmentModule, NULL);
		return false;

	}

	// ---- 顶点输入：调用方给了就用它，否则按 program 的属性推导交错布局 ----
	std::vector<VkVertexInputAttributeDescription> attributeDescriptions;
	uint32_t bindingIndex = 0;
	uint32_t stride = 0;

	if (layout && layout->attributes && layout->attributeCount > 0) {

		bindingIndex = layout->binding;
		stride = layout->stride;

		for (uint32_t i = 0; i < layout->attributeCount; i++) {

			VkVertexInputAttributeDescription description{};
			description.location = layout->attributes[i].location;
			description.binding = layout->attributes[i].binding;
			description.format = (VkFormat) layout->attributes[i].format;
			description.offset = layout->attributes[i].offset;
			attributeDescriptions.push_back (description);

		}

	} else {

		uint32_t offset = 0;
		for (size_t i = 0; i < p->attribs.size (); i++) {

			int components = 4;
			if (p->attribs[i].glType == GL_FLOAT) components = 1;
			else if (p->attribs[i].glType == GL_FLOAT_VEC2) components = 2;
			else if (p->attribs[i].glType == GL_FLOAT_VEC3) components = 3;

			VkVertexInputAttributeDescription description{};
			description.location = (uint32_t) p->attribs[i].location;
			description.binding = 0;
			description.format = (VkFormat) VertexFormatForComponents (components);
			description.offset = offset;
			attributeDescriptions.push_back (description);

			offset += 4 * components;

		}
		stride = offset;

	}

	VkVertexInputBindingDescription bindingDescription{};
	bindingDescription.binding = bindingIndex;
	bindingDescription.stride = stride;
	bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

	VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	vertexInput.vertexBindingDescriptionCount = stride ? 1 : 0;
	vertexInput.pVertexBindingDescriptions = stride ? &bindingDescription : NULL;
	vertexInput.vertexAttributeDescriptionCount = (uint32_t) attributeDescriptions.size ();
	vertexInput.pVertexAttributeDescriptions = attributeDescriptions.empty () ? NULL : attributeDescriptions.data ();

	VkPipelineInputAssemblyStateCreateInfo inputAssembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
	inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	VkViewport viewport{};
	viewport.width = (float) width;
	viewport.height = (float) height;
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;

	VkRect2D scissor{};
	scissor.extent.width = width;
	scissor.extent.height = height;

	VkPipelineViewportStateCreateInfo viewportState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
	viewportState.viewportCount = 1;
	viewportState.pViewports = &viewport;
	viewportState.scissorCount = 1;
	viewportState.pScissors = &scissor;

	VkPipelineRasterizationStateCreateInfo rasterization{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
	rasterization.polygonMode = VK_POLYGON_MODE_FILL;
	rasterization.cullMode = VK_CULL_MODE_NONE;
	rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rasterization.lineWidth = 1.0f;

	VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineDepthStencilStateCreateInfo depthStencil{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};

	VkPipelineColorBlendAttachmentState blendAttachment{};
	blendAttachment.blendEnable = VK_TRUE;
	blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
	blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
	blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
	blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
		VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

	VkPipelineColorBlendStateCreateInfo colorBlend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
	colorBlend.attachmentCount = 1;
	colorBlend.pAttachments = &blendAttachment;

	VkPipelineShaderStageCreateInfo stages[2] = {};
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vertexModule;
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = fragmentModule;
	stages[1].pName = "main";

	VkGraphicsPipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
	pipelineInfo.stageCount = 2;
	pipelineInfo.pStages = stages;
	pipelineInfo.pVertexInputState = &vertexInput;
	pipelineInfo.pInputAssemblyState = &inputAssembly;
	pipelineInfo.pViewportState = &viewportState;
	pipelineInfo.pRasterizationState = &rasterization;
	pipelineInfo.pMultisampleState = &multisample;
	pipelineInfo.pDepthStencilState = &depthStencil;
	pipelineInfo.pColorBlendState = &colorBlend;
	pipelineInfo.layout = pipelineLayout;
	pipelineInfo.renderPass = renderPass;
	pipelineInfo.subpass = 0;

	VkPipeline pipeline = VK_NULL_HANDLE;
	VkResult result = g_vkFns.CreateGraphicsPipelines (device, VK_NULL_HANDLE, 1, &pipelineInfo, NULL, &pipeline);

	g_vkFns.DestroyShaderModule (device, vertexModule, NULL);
	g_vkFns.DestroyShaderModule (device, fragmentModule, NULL);

	if (result != VK_SUCCESS) {

		if (uniformMemory) g_vkFns.FreeMemory (device, uniformMemory, NULL);
		if (uniformBuffer) g_vkFns.DestroyBuffer (device, uniformBuffer, NULL);
		g_vkFns.DestroyPipelineLayout (device, pipelineLayout, NULL);
		g_vkFns.DestroyDescriptorPool (device, descriptorPool, NULL);
		g_vkFns.DestroyDescriptorSetLayout (device, descriptorLayout, NULL);
		return false;

	}

	// ---- 写 UBO descriptor ----
	if (descriptorSet && uniformBuffer) {

		VkDescriptorBufferInfo bufferInfo{};
		bufferInfo.buffer = uniformBuffer;
		bufferInfo.offset = 0;
		bufferInfo.range = bufferSize;

		VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
		write.dstSet = descriptorSet;
		write.dstBinding = 0;
		write.descriptorCount = 1;
		write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		write.pBufferInfo = &bufferInfo;

		g_vkFns.UpdateDescriptorSets (device, 1, &write, 0, NULL);

	}

	slot->pipeline = pipeline;
	slot->layout = pipelineLayout;
	slot->descriptorLayout = descriptorLayout;
	slot->descriptorPool = descriptorPool;
	slot->descriptorSet = descriptorSet;
	slot->uniformBuffer = uniformBuffer;
	slot->uniformMemory = uniformMemory;
	slot->mapped = mapped;
	slot->bufferSize = bufferSize;
	slot->built = true;

	return true;

}

// ---------------------------------------------------------------------------
// 对外 API
// ---------------------------------------------------------------------------

#define LIME_LOAD_VK(name) g_vkFns.name = (PFN_vk##name) context->getDeviceProcAddr (context->device, "vk" #name)

void gl_shader_vk_register (const GLShaderVkContext* context) {

	if (!context || !context->device || !context->getDeviceProcAddr) return;

	EnsureGlslang ();

	// 槽位/描述符属于旧设备，必须在换上下文之前销毁
	if (g_vkReady && g_vkCtx.device != context->device) gl_shader_vk_unregister ();

	g_vkCtx = *context;

	LIME_LOAD_VK (CreateShaderModule);
	LIME_LOAD_VK (DestroyShaderModule);
	LIME_LOAD_VK (CreatePipelineLayout);
	LIME_LOAD_VK (DestroyPipelineLayout);
	LIME_LOAD_VK (CreateGraphicsPipelines);
	LIME_LOAD_VK (DestroyPipeline);
	LIME_LOAD_VK (CreateDescriptorSetLayout);
	LIME_LOAD_VK (DestroyDescriptorSetLayout);
	LIME_LOAD_VK (CreateDescriptorPool);
	LIME_LOAD_VK (DestroyDescriptorPool);
	LIME_LOAD_VK (AllocateDescriptorSets);
	LIME_LOAD_VK (UpdateDescriptorSets);
	LIME_LOAD_VK (CreateBuffer);
	LIME_LOAD_VK (DestroyBuffer);
	LIME_LOAD_VK (GetBufferMemoryRequirements);
	LIME_LOAD_VK (AllocateMemory);
	LIME_LOAD_VK (FreeMemory);
	LIME_LOAD_VK (BindBufferMemory);
	LIME_LOAD_VK (MapMemory);
	LIME_LOAD_VK (UnmapMemory);
	LIME_LOAD_VK (CreateSampler);
	LIME_LOAD_VK (DestroySampler);

	g_vkReady = true;

}

void gl_shader_vk_unregister () {

	if (g_vkReady) {

		for (std::map<int, Program*>::iterator it = g_programs.begin (); it != g_programs.end (); ++it) {

			DestroyPipelineSlot (&it->second->pipelineBack);
			DestroyPipelineSlot (&it->second->pipelineOffscreen);

		}

		if (g_defaultSampler) g_vkFns.DestroySampler (g_vkCtx.device, g_defaultSampler, NULL);

	}

	g_defaultSampler = VK_NULL_HANDLE;
	g_vkReady = false;
	memset (&g_vkFns, 0, sizeof (g_vkFns));
	memset (&g_vkCtx, 0, sizeof (g_vkCtx));

}

bool gl_shader_vk_available () {

	return g_vkReady;

}

void gl_shader_vk_update_viewport (uint32_t width, uint32_t height) {

	g_vkCtx.viewportWidth = width;
	g_vkCtx.viewportHeight = height;

	for (std::map<int, Program*>::iterator it = g_programs.begin (); it != g_programs.end (); ++it) {

		DestroyPipelineSlot (&it->second->pipelineBack);
		DestroyPipelineSlot (&it->second->pipelineOffscreen);

	}

}

static PipelineSlot* SlotFor (Program* p, bool offscreen) {

	return offscreen ? &p->pipelineOffscreen : &p->pipelineBack;

}

static uint64_t SlotFingerprint (Program* p, const GLShaderVertexLayout* layout, bool offscreen) {

	uint64_t hash = 1469598103934665603ULL;
	hash = FnvMix (hash, (uint64_t) offscreen);
	hash = FnvMix (hash, g_vkCtx.viewportWidth);
	hash = FnvMix (hash, g_vkCtx.viewportHeight);
	hash = FnvMix (hash, (uint64_t) p->spirvVertex.size ());
	hash = FnvMix (hash, (uint64_t) p->spirvFragment.size ());
	hash = FnvMix (hash, (uint64_t) p->uniformSize);

	if (layout && layout->attributes) {

		hash = FnvMix (hash, (uint64_t) layout->binding);
		hash = FnvMix (hash, (uint64_t) layout->stride);
		hash = FnvMix (hash, (uint64_t) layout->attributeCount);

		for (uint32_t i = 0; i < layout->attributeCount; i++) {

			hash = FnvMix (hash, (uint64_t) layout->attributes[i].location);
			hash = FnvMix (hash, (uint64_t) layout->attributes[i].format);
			hash = FnvMix (hash, (uint64_t) layout->attributes[i].offset);

		}

	} else {

		hash = FnvMix (hash, (uint64_t) p->attribs.size ());

	}

	return hash;

}

bool gl_shader_build_pipeline (int program, const GLShaderVertexLayout* layout, bool offscreen) {

	Program* p = FindProgram (program);
	if (!p || !p->linkOk) return false;
	if (!g_vkReady) return false;

	PipelineSlot* slot = SlotFor (p, offscreen);
	uint64_t fingerprint = SlotFingerprint (p, layout, offscreen);

	if (slot->built && slot->fingerprint == fingerprint) return true;

	DestroyPipelineSlot (slot);

	if (!CreatePipelineSlot (p, slot, layout, offscreen)) return false;

	slot->fingerprint = fingerprint;
	return true;

}

VkPipeline gl_shader_pipeline (int program, bool offscreen) {

	Program* p = FindProgram (program);
	if (!p) return VK_NULL_HANDLE;
	return SlotFor (p, offscreen)->pipeline;

}

VkPipelineLayout gl_shader_pipeline_layout (int program) {

	Program* p = FindProgram (program);
	if (!p) return VK_NULL_HANDLE;
	return p->pipelineBack.layout ? p->pipelineBack.layout : p->pipelineOffscreen.layout;

}

VkDescriptorSet gl_shader_descriptor_set (int program, bool offscreen) {

	Program* p = FindProgram (program);
	if (!p) return VK_NULL_HANDLE;
	return SlotFor (p, offscreen)->descriptorSet;

}

uint32_t gl_shader_uniform_buffer_size (int program) {

	Program* p = FindProgram (program);
	return p ? p->uniformSize : 0;

}

bool gl_shader_upload_uniforms (int program) {

	Program* p = FindProgram (program);
	if (!p || p->uniformData.empty ()) return false;

	bool uploaded = false;

	PipelineSlot* slots[2] = { &p->pipelineBack, &p->pipelineOffscreen };
	for (int i = 0; i < 2; i++) {

		if (!slots[i]->mapped || slots[i]->bufferSize == 0) continue;
		memcpy (slots[i]->mapped, p->uniformData.data (), p->uniformData.size ());
		uploaded = true;

	}

	p->uniformDirty = false;
	return uploaded;

}

static std::string StripArraySuffix (const std::string& name) {

	size_t open = name.find ('[');
	return (open == std::string::npos) ? name : name.substr (0, open);

}

static UniformInfo* FindUniform (Program* p, const std::string& name) {

	for (size_t i = 0; i < p->uniforms.size (); i++) {

		if (p->uniforms[i].name == name) return &p->uniforms[i];

	}
	return NULL;

}

int gl_shader_sampler_binding (int program, const char* name) {

	Program* p = FindProgram (program);
	if (!p || !name) return -1;

	UniformInfo* uniform = FindUniform (p, StripArraySuffix (name));
	if (!uniform || uniform->kind != BASE_SAMPLER) return -1;
	return uniform->binding;

}

int gl_shader_uniform_texture_unit (int program, int location) {

	Program* p = FindProgram (program);
	if (!p || location < 0 || location >= (int) p->locationSlot.size ()) return -1;

	int index = p->locationSlot[location];
	if (index < 0 || index >= (int) p->uniforms.size ()) return -1;
	if (p->uniforms[index].kind != BASE_SAMPLER) return -1;
	return p->uniforms[index].textureUnit;

}

int gl_shader_current_program () {

	return g_currentProgram;

}

const char* gl_shader_program_glsl (int program, int stage) {

	Program* p = FindProgram (program);
	if (!p) return NULL;
	return (stage == STAGE_FRAGMENT) ? p->glslFragment.c_str () : p->glslVertex.c_str ();

}

const char* gl_shader_program_log (int program) {

	Program* p = FindProgram (program);
	return p ? p->log.c_str () : "";

}

static VkSampler EnsureDefaultSampler () {

	if (g_defaultSampler || !g_vkReady) return g_defaultSampler;

	VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	info.magFilter = VK_FILTER_NEAREST;
	info.minFilter = VK_FILTER_NEAREST;
	info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

	if (g_vkFns.CreateSampler (g_vkCtx.device, &info, NULL, &g_defaultSampler) != VK_SUCCESS) {

		g_defaultSampler = VK_NULL_HANDLE;

	}
	return g_defaultSampler;

}

bool gl_shader_write_sampler (int program, bool offscreen, const char* name, VkImageView view, VkSampler sampler) {

	Program* p = FindProgram (program);
	if (!p || !name || !view || !g_vkReady) return false;

	UniformInfo* uniform = FindUniform (p, StripArraySuffix (name));
	if (!uniform || uniform->kind != BASE_SAMPLER) return false;

	PipelineSlot* slot = SlotFor (p, offscreen);
	if (!slot->descriptorSet) return false;

	if (!sampler) sampler = EnsureDefaultSampler ();
	if (!sampler) return false;

	VkDescriptorImageInfo imageInfo{};
	imageInfo.sampler = sampler;
	imageInfo.imageView = view;
	imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
	write.dstSet = slot->descriptorSet;
	write.dstBinding = uniform->binding;
	write.descriptorCount = 1;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.pImageInfo = &imageInfo;

	g_vkFns.UpdateDescriptorSets (g_vkCtx.device, 1, &write, 0, NULL);
	return true;

}

// ---------------------------------------------------------------------------
// shader / program 的 GL 入口
// ---------------------------------------------------------------------------

static std::string HxToString (HxString string) {

	return std::string (string.__s.c_str ());

}

static void ReleaseShaderIfUnused (Shader* shader) {

	if (!shader || !shader->deleted || shader->refCount > 0) return;
	std::map<int, Shader*>::iterator it = g_shaders.find (shader->id);
	if (it != g_shaders.end ()) g_shaders.erase (it);
	delete shader;

}

// 单独编译一个 stage（和 GL 一样在 compileShader 阶段就报语法错误）。
// 完整链接时会用两 stage 合并后的 uniform 列表重新生成，这里只需要能过。
static bool CompileSingleStage (Shader* shader, std::string* log) {

	int stageIndex = (shader->glType == GL_VERTEX_SHADER) ? STAGE_VERTEX : STAGE_FRAGMENT;
	ScannedStage stage = ScanStage (shader->source, stageIndex);
	ScannedStage empty;

	std::vector<MergedUniform> merged;
	if (stageIndex == STAGE_VERTEX) MergeUniforms (stage, empty, &merged);
	else MergeUniforms (empty, stage, &merged);

	std::vector<DeclInfo> varyings;
	std::map<std::string, int> varyingLocations;
	MergeVaryings (stage, empty, &varyings, &varyingLocations);

	std::map<std::string, int> attribLocations;
	std::map<std::string, int> bindings;
	AssignAttributeLocations (stage.attribs, bindings, &attribLocations);

	std::string source = BuildStageSource (stage, stageIndex, merged, varyings,
		varyingLocations, attribLocations, stage.attribs);

	std::vector<uint32_t> spirv;
	return CompileStage (source, stageIndex, &spirv, log);

}

int lime_gl_create_shader (int type) {

	if (type != GL_VERTEX_SHADER && type != GL_FRAGMENT_SHADER) return 0;

	Shader* shader = new Shader ();
	shader->id = g_nextShaderId++;
	shader->glType = type;
	g_shaders[shader->id] = shader;
	return shader->id;

}

void lime_gl_shader_source (int handle, HxString source) {

	Shader* shader = FindShader (handle);
	if (!shader) return;

	shader->source = StripLineEndings (HxToString (source));
	shader->compiled = false;
	shader->compileOk = false;
	shader->log.clear ();

}

void lime_gl_compile_shader (int shader) {

	Shader* target = FindShader (shader);
	if (!target) return;

	target->compiled = true;
	target->log.clear ();

	if (target->source.empty ()) {

		target->compileOk = false;
		target->log = "shader source is empty";
		return;

	}

	std::string log;
	target->compileOk = CompileSingleStage (target, &log);
	if (!target->compileOk) target->log = log;

}

void lime_gl_delete_shader (int shader) {

	Shader* target = FindShader (shader);
	if (!target) return;

	target->deleted = true;
	ReleaseShaderIfUnused (target);

}

bool lime_gl_is_shader (int handle) {

	return FindShader (handle) != NULL;

}

int lime_gl_get_shaderi (int shader, int pname) {

	Shader* target = FindShader (shader);
	if (!target) return 0;

	if (pname == GL_COMPILE_STATUS) return target->compileOk ? 1 : 0;
	if (pname == GL_SHADER_TYPE) return target->glType;
	if (pname == GL_DELETE_STATUS) return target->deleted ? 1 : 0;
	if (pname == GL_INFO_LOG_LENGTH) return (int) target->log.size () + 1;
	if (pname == GL_SHADER_SOURCE_LENGTH) return (int) target->source.size () + 1;

	return 0;

}

void lime_gl_get_shaderiv (int shader, int pname, double params) {

	int value = lime_gl_get_shaderi (shader, pname);
	if (params) *(int*) (uintptr_t) params = value;

}

value lime_gl_get_shader_info_log (int handle) {

	Shader* shader = FindShader (handle);
	if (!shader || shader->log.empty ()) return alloc_null ();

	return alloc_string (shader->log.c_str ());

}

value lime_gl_get_shader_source (int shader) {

	Shader* target = FindShader (shader);
	if (!target || target->source.empty ()) return alloc_null ();

	return alloc_string (target->source.c_str ());

}

int lime_gl_create_program () {

	Program* program = new Program ();
	program->id = g_nextProgramId++;
	g_programs[program->id] = program;
	return program->id;

}

void lime_gl_attach_shader (int program, int shader) {

	Program* target = FindProgram (program);
	if (!target) return;

	Shader* attachment = FindShader (shader);
	if (!attachment) return;
	if (attachment->glType != GL_VERTEX_SHADER && attachment->glType != GL_FRAGMENT_SHADER) return;

	for (size_t i = 0; i < target->attached.size (); i++) {

		if (target->attached[i] == shader) return;

	}

	target->attached.push_back (shader);
	attachment->refCount++;
	target->linked = false;
	target->linkOk = false;

}

void lime_gl_detach_shader (int program, int shader) {

	Program* target = FindProgram (program);
	if (!target) return;

	for (size_t i = 0; i < target->attached.size (); i++) {

		if (target->attached[i] != shader) continue;

		target->attached.erase (target->attached.begin () + i);
		Shader* attachment = FindShader (shader);
		if (attachment) {

			attachment->refCount--;
			ReleaseShaderIfUnused (attachment);

		}
		target->linked = false;
		target->linkOk = false;
		return;

	}

}

void lime_gl_link_program (int program) {

	Program* target = FindProgram (program);
	if (!target) return;

	target->linkOk = BuildProgram (target);

}

void lime_gl_validate_program (int handle) {

	Program* target = FindProgram (handle);
	if (!target) return;

	target->validateOk = target->linked && target->linkOk;

}

void lime_gl_use_program (int handle) {

	if (handle == 0) {

		g_currentProgram = 0;
		return;

	}
	if (FindProgram (handle)) g_currentProgram = handle;

}

void lime_gl_delete_program (int program) {

	Program* target = FindProgram (program);
	if (!target) return;

	for (size_t i = 0; i < target->attached.size (); i++) {

		Shader* shader = FindShader (target->attached[i]);
		if (!shader) continue;
		shader->refCount--;
		ReleaseShaderIfUnused (shader);

	}
	target->attached.clear ();

	DestroyPipelineSlot (&target->pipelineBack);
	DestroyPipelineSlot (&target->pipelineOffscreen);

	if (g_currentProgram == program) g_currentProgram = 0;

	std::map<int, Program*>::iterator it = g_programs.find (program);
	if (it != g_programs.end ()) g_programs.erase (it);
	delete target;

}

bool lime_gl_is_program (int handle) {

	return FindProgram (handle) != NULL;

}

int lime_gl_get_programi (int program, int pname) {

	Program* target = FindProgram (program);
	if (!target) return 0;

	if (pname == GL_LINK_STATUS) return target->linkOk ? 1 : 0;
	if (pname == GL_VALIDATE_STATUS) return target->validateOk ? 1 : 0;
	if (pname == GL_DELETE_STATUS) return 0;
	if (pname == GL_ATTACHED_SHADERS) return (int) target->attached.size ();
	if (pname == GL_ACTIVE_UNIFORMS) return (int) target->uniforms.size ();
	if (pname == GL_ACTIVE_ATTRIBUTES) return (int) target->attribs.size ();
	if (pname == GL_ACTIVE_UNIFORM_MAX_LENGTH || pname == GL_ACTIVE_ATTRIBUTE_MAX_LENGTH) {

		size_t longest = 0;
		for (size_t i = 0; i < target->uniforms.size (); i++) {

			if (target->uniforms[i].glName.size () > longest) longest = target->uniforms[i].glName.size ();

		}
		for (size_t i = 0; i < target->attribs.size (); i++) {

			if (target->attribs[i].name.size () > longest) longest = target->attribs[i].name.size ();

		}
		return (int) longest + 1;

	}
	if (pname == GL_INFO_LOG_LENGTH) return (int) target->log.size () + 1;

	return 0;

}

void lime_gl_get_programiv (int program, int pname, double params) {

	int value = lime_gl_get_programi (program, pname);
	if (params) *(int*) (uintptr_t) params = value;

}

value lime_gl_get_program_info_log (int handle) {

	Program* target = FindProgram (handle);
	if (!target || target->log.empty ()) return alloc_null ();

	return alloc_string (target->log.c_str ());

}

value lime_gl_get_attached_shaders (int program) {

	Program* target = FindProgram (program);
	if (!target || target->attached.empty ()) return alloc_null ();

	value result = alloc_array ((int) target->attached.size ());
	for (size_t i = 0; i < target->attached.size (); i++) {

		val_array_set_i (result, (int) i, alloc_int (target->attached[i]));

	}
	return result;

}

void lime_gl_bind_attrib_location (int program, int index, HxString name) {

	Program* target = FindProgram (program);
	if (!target || index < 0) return;

	target->attribBindings[HxToString (name)] = index;

}

int lime_gl_get_attrib_location (int program, HxString name) {

	Program* target = FindProgram (program);
	if (!target) return -1;

	std::map<std::string, int>::const_iterator it = target->attribLocations.find (HxToString (name));
	return (it == target->attribLocations.end ()) ? -1 : it->second;

}

int lime_gl_get_frag_data_location (int program, HxString name) {

	Program* target = FindProgram (program);
	if (!target || !target->linkOk) return -1;

	std::string value = HxToString (name);
	if (value == "gl_FragColor" || value == "_limeFragColor" || value == "lime_FragColor") return 0;
	return -1;

}

value lime_gl_get_active_uniform (int program, int index) {

	Program* target = FindProgram (program);
	if (!target || index < 0 || index >= (int) target->uniforms.size ()) return alloc_null ();

	UniformInfo& uniform = target->uniforms[index];

	value result = alloc_empty_object ();
	alloc_field (result, val_id ("size"), alloc_int (uniform.arraySize));
	alloc_field (result, val_id ("type"), alloc_int (uniform.glType));
	alloc_field (result, val_id ("name"), alloc_string (uniform.glName.c_str ()));
	return result;

}

value lime_gl_get_active_attrib (int program, int index) {

	Program* target = FindProgram (program);
	if (!target || index < 0 || index >= (int) target->attribs.size ()) return alloc_null ();

	AttribInfo& attrib = target->attribs[index];

	value result = alloc_empty_object ();
	alloc_field (result, val_id ("size"), alloc_int (attrib.arraySize));
	alloc_field (result, val_id ("type"), alloc_int (attrib.glType));
	alloc_field (result, val_id ("name"), alloc_string (attrib.name.c_str ()));
	return result;

}

int lime_gl_get_uniform_location (int program, HxString name) {

	Program* target = FindProgram (program);
	if (!target || !target->linkOk) return -1;

	std::string full = HxToString (name);
	if (full.find ('.') != std::string::npos) return -1;

	std::string base = full;
	int element = 0;
	size_t open = full.find ('[');

	if (open != std::string::npos) {

		size_t close = full.find (']', open);
		if (close == std::string::npos) return -1;

		std::string digits = full.substr (open + 1, close - open - 1);
		element = 0;
		for (size_t i = 0; i < digits.size (); i++) {

			if (digits[i] < '0' || digits[i] > '9') return -1;
			element = element * 10 + (digits[i] - '0');

		}
		base = full.substr (0, open);

	}

	for (size_t i = 0; i < target->uniforms.size (); i++) {

		UniformInfo& uniform = target->uniforms[i];
		if (uniform.name != base) continue;
		if (element >= uniform.arraySize) return -1;
		return uniform.location + element;

	}

	return -1;

}

// ---------------------------------------------------------------------------
// uniform 写入 / 读取
//
// GL 的 location 是程序级编号，数组每个元素占一个 location；
// 真正的字节偏移来自 SPIR-V 反射，这里只按 location -> (uniform, 元素) 映射写字节。
// ---------------------------------------------------------------------------

static UniformInfo* UniformAt (Program* program, int location, int* element) {

	if (!program || location < 0 || location >= (int) program->locationSlot.size ()) return NULL;

	int index = program->locationSlot[location];
	if (index < 0 || index >= (int) program->uniforms.size ()) return NULL;

	if (element) *element = program->locationElem[location];
	return &program->uniforms[index];

}

static uint32_t ElementBase (const UniformInfo& uniform, int element) {

	return uniform.offset + ((uniform.arraySize > 1) ? uniform.arrayStride * (uint32_t) element : 0);

}

static bool WriteScalars (int location, const void* values, int components) {

	Program* program = FindProgram (g_currentProgram);
	int element = 0;
	UniformInfo* uniform = UniformAt (program, location, &element);
	if (!uniform) return false;

	if (uniform->kind == BASE_SAMPLER) {

		if (components != 1) return false;
		uniform->textureUnit = *(const int*) values;
		program->uniformDirty = true;
		return true;

	}

	uint32_t base = ElementBase (*uniform, element);
	size_t bytes = 4u * components;
	if (base + bytes > program->uniformData.size ()) return false;

	memcpy (program->uniformData.data () + base, values, bytes);
	program->uniformDirty = true;
	return true;

}

static bool ReadScalars (int programHandle, int location, void* out, int components) {

	Program* program = FindProgram (programHandle);
	int element = 0;
	UniformInfo* uniform = UniformAt (program, location, &element);
	if (!uniform) return false;

	if (uniform->kind == BASE_SAMPLER) {

		*(int*) out = uniform->textureUnit;
		return true;

	}

	uint32_t base = ElementBase (*uniform, element);
	size_t bytes = 4u * components;
	if (base + bytes > program->uniformData.size ()) return false;

	memcpy (out, program->uniformData.data () + base, bytes);
	return true;

}

static void WriteFloatArray (int location, int count, const float* values, int components) {

	if (!values) return;
	for (int i = 0; i < count; i++) {

		WriteScalars (location + i, values + i * components, components);

	}

}

static void WriteIntArray (int location, int count, const int* values, int components) {

	if (!values) return;
	for (int i = 0; i < count; i++) {

		WriteScalars (location + i, values + i * components, components);

	}

}

static void WriteMatrices (int location, int count, bool transpose, const float* values, int rows, int columns) {

	Program* program = FindProgram (g_currentProgram);
	if (!program || !values) return;

	for (int i = 0; i < count; i++) {

		int element = 0;
		UniformInfo* uniform = UniformAt (program, location + i, &element);
		if (!uniform || uniform->columns < 2) continue;

		uint32_t stride = uniform->matrixStride ? uniform->matrixStride : 16u;
		uint32_t base = ElementBase (*uniform, element);

		for (int c = 0; c < columns; c++) {

			for (int r = 0; r < rows; r++) {

				float value = transpose ? values[r * columns + c] : values[c * rows + r];
				uint32_t dst = uniform->rowMajor
					? base + (uint32_t) r * stride + (uint32_t) c * 4u
					: base + (uint32_t) c * stride + (uint32_t) r * 4u;

				if (dst + 4u > program->uniformData.size ()) continue;
				*(float*) (program->uniformData.data () + dst) = value;

			}

		}

		values += rows * columns;

	}

	program->uniformDirty = true;

}

void lime_gl_uniform1f (int location, float v0) {

	WriteScalars (location, &v0, 1);

}

void lime_gl_uniform2f (int location, float v0, float v1) {

	float values[2] = { v0, v1 };
	WriteScalars (location, values, 2);

}

void lime_gl_uniform3f (int location, float v0, float v1, float v2) {

	float values[3] = { v0, v1, v2 };
	WriteScalars (location, values, 3);

}

void lime_gl_uniform4f (int location, float v0, float v1, float v2, float v3) {

	float values[4] = { v0, v1, v2, v3 };
	WriteScalars (location, values, 4);

}

void lime_gl_uniform1i (int location, int v0) {

	WriteScalars (location, &v0, 1);

}

void lime_gl_uniform2i (int location, int v0, int v1) {

	int values[2] = { v0, v1 };
	WriteScalars (location, values, 2);

}

void lime_gl_uniform3i (int location, int v0, int v1, int v2) {

	int values[3] = { v0, v1, v2 };
	WriteScalars (location, values, 3);

}

void lime_gl_uniform4i (int location, int v0, int v1, int v2, int v3) {

	int values[4] = { v0, v1, v2, v3 };
	WriteScalars (location, values, 4);

}

void lime_gl_uniform1ui (int location, int v0) {

	uint32_t value = (uint32_t) v0;
	WriteScalars (location, &value, 1);

}

void lime_gl_uniform2ui (int location, int v0, int v1) {

	uint32_t values[2] = { (uint32_t) v0, (uint32_t) v1 };
	WriteScalars (location, values, 2);

}

void lime_gl_uniform3ui (int location, int v0, int v1, int v2) {

	uint32_t values[3] = { (uint32_t) v0, (uint32_t) v1, (uint32_t) v2 };
	WriteScalars (location, values, 3);

}

void lime_gl_uniform4ui (int location, int v0, int v1, int v2, int v3) {

	uint32_t values[4] = { (uint32_t) v0, (uint32_t) v1, (uint32_t) v2, (uint32_t) v3 };
	WriteScalars (location, values, 4);

}

void lime_gl_uniform1fv (int location, int count, double _value) {

	WriteFloatArray (location, count, (const float*) (uintptr_t) _value, 1);

}

void lime_gl_uniform2fv (int location, int count, double _value) {

	WriteFloatArray (location, count, (const float*) (uintptr_t) _value, 2);

}

void lime_gl_uniform3fv (int location, int count, double _value) {

	WriteFloatArray (location, count, (const float*) (uintptr_t) _value, 3);

}

void lime_gl_uniform4fv (int location, int count, double _value) {

	WriteFloatArray (location, count, (const float*) (uintptr_t) _value, 4);

}

void lime_gl_uniform1iv (int location, int count, double _value) {

	WriteIntArray (location, count, (const int*) (uintptr_t) _value, 1);

}

void lime_gl_uniform2iv (int location, int count, double _value) {

	WriteIntArray (location, count, (const int*) (uintptr_t) _value, 2);

}

void lime_gl_uniform3iv (int location, int count, double _value) {

	WriteIntArray (location, count, (const int*) (uintptr_t) _value, 3);

}

void lime_gl_uniform4iv (int location, int count, double _value) {

	WriteIntArray (location, count, (const int*) (uintptr_t) _value, 4);

}

void lime_gl_uniform1uiv (int location, int count, double _value) {

	WriteIntArray (location, count, (const int*) (uintptr_t) _value, 1);

}

void lime_gl_uniform2uiv (int location, int count, double _value) {

	WriteIntArray (location, count, (const int*) (uintptr_t) _value, 2);

}

void lime_gl_uniform3uiv (int location, int count, double _value) {

	WriteIntArray (location, count, (const int*) (uintptr_t) _value, 3);

}

void lime_gl_uniform4uiv (int location, int count, double _value) {

	WriteIntArray (location, count, (const int*) (uintptr_t) _value, 4);

}

void lime_gl_uniform_matrix2fv (int location, int count, bool transpose, double _value) {

	WriteMatrices (location, count, transpose, (const float*) (uintptr_t) _value, 2, 2);

}

void lime_gl_uniform_matrix2x3fv (int location, int count, bool transpose, double _value) {

	WriteMatrices (location, count, transpose, (const float*) (uintptr_t) _value, 3, 2);

}

void lime_gl_uniform_matrix2x4fv (int location, int count, bool transpose, double _value) {

	WriteMatrices (location, count, transpose, (const float*) (uintptr_t) _value, 4, 2);

}

void lime_gl_uniform_matrix3fv (int location, int count, bool transpose, double _value) {

	WriteMatrices (location, count, transpose, (const float*) (uintptr_t) _value, 3, 3);

}

void lime_gl_uniform_matrix3x2fv (int location, int count, bool transpose, double _value) {

	WriteMatrices (location, count, transpose, (const float*) (uintptr_t) _value, 2, 3);

}

void lime_gl_uniform_matrix3x4fv (int location, int count, bool transpose, double _value) {

	WriteMatrices (location, count, transpose, (const float*) (uintptr_t) _value, 4, 3);

}

void lime_gl_uniform_matrix4fv (int location, int count, bool transpose, double _value) {

	WriteMatrices (location, count, transpose, (const float*) (uintptr_t) _value, 4, 4);

}

void lime_gl_uniform_matrix4x2fv (int location, int count, bool transpose, double _value) {

	WriteMatrices (location, count, transpose, (const float*) (uintptr_t) _value, 2, 4);

}

void lime_gl_uniform_matrix4x3fv (int location, int count, bool transpose, double _value) {

	WriteMatrices (location, count, transpose, (const float*) (uintptr_t) _value, 3, 4);

}

float lime_gl_get_uniformf (int program, int location) {

	float values[4] = { 0, 0, 0, 0 };
	ReadScalars (program, location, values, 1);
	return values[0];

}

int lime_gl_get_uniformi (int program, int location) {

	int values[4] = { 0, 0, 0, 0 };
	ReadScalars (program, location, values, 1);
	return values[0];

}

int lime_gl_get_uniformui (int program, int location) {

	uint32_t values[4] = { 0, 0, 0, 0 };
	ReadScalars (program, location, values, 1);
	return (int) values[0];

}

void lime_gl_get_uniformfv (int program, int location, double params) {

	if (!params) return;

	Program* target = FindProgram (program);
	int element = 0;
	UniformInfo* uniform = UniformAt (target, location, &element);
	if (!uniform) return;

	float values[4] = { 0, 0, 0, 0 };
	ReadScalars (program, location, values, uniform->columns > 1 ? 1 : uniform->components);
	memcpy ((void*) (uintptr_t) params, values, sizeof (float) * (uniform->columns > 1 ? 1 : uniform->components));

}

void lime_gl_get_uniformiv (int program, int location, double params) {

	if (!params) return;

	Program* target = FindProgram (program);
	int element = 0;
	UniformInfo* uniform = UniformAt (target, location, &element);
	if (!uniform) return;

	int values[4] = { 0, 0, 0, 0 };
	ReadScalars (program, location, values, uniform->columns > 1 ? 1 : uniform->components);
	memcpy ((void*) (uintptr_t) params, values, sizeof (int) * (uniform->columns > 1 ? 1 : uniform->components));

}

void lime_gl_get_uniformuiv (int program, int location, double params) {

	if (!params) return;

	Program* target = FindProgram (program);
	int element = 0;
	UniformInfo* uniform = UniformAt (target, location, &element);
	if (!uniform) return;

	uint32_t values[4] = { 0, 0, 0, 0 };
	ReadScalars (program, location, values, uniform->columns > 1 ? 1 : uniform->components);
	memcpy ((void*) (uintptr_t) params, values, sizeof (uint32_t) * (uniform->columns > 1 ? 1 : uniform->components));

}

// ---------------------------------------------------------------------------
// uniform block / 反射查询
//
// 本层把所有松散 uniform 合并成一个匿名 std140 block 发布给 GL，
// 因此没有“命名 uniform block”这一层；相关入口按 GL 语义返回“无 block”。
// ---------------------------------------------------------------------------

int lime_gl_get_uniform_block_index (int program, HxString uniformBlockName) {

	Program* target = FindProgram (program);
	if (!target) return (int) GL_INVALID_INDEX;
	return (int) GL_INVALID_INDEX;

}

int lime_gl_get_active_uniform_blocki (int program, int uniformBlockIndex, int pname) {

	Program* target = FindProgram (program);
	if (!target || uniformBlockIndex == (int) GL_INVALID_INDEX) return 0;

	if (pname == GL_UNIFORM_BLOCK_BINDING) return 0;
	if (pname == GL_UNIFORM_BLOCK_DATA_SIZE) return (int) target->uniformSize;
	if (pname == GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS) return (int) target->uniforms.size ();

	return 0;

}

void lime_gl_get_active_uniform_blockiv (int program, int uniformBlockIndex, int pname, double params) {

	int value = lime_gl_get_active_uniform_blocki (program, uniformBlockIndex, pname);
	if (params) *(int*) (uintptr_t) params = value;

}

value lime_gl_get_active_uniform_block_name (int program, int uniformBlockIndex) {

	return alloc_null ();

}

void lime_gl_uniform_block_binding (int program, int uniformBlockIndex, int uniformBlockBinding) {

	// 匿名 block 固定 set=0 binding=0，没有可重绑的命名 block

}

void lime_gl_get_active_uniformsiv (int program, value uniformIndices, int pname, double params) {

	Program* target = FindProgram (program);
	if (!target || !params) return;

	int count = val_array_size (uniformIndices);
	int* out = (int*) (uintptr_t) params;

	for (int i = 0; i < count; i++) {

		int index = val_int (val_array_i (uniformIndices, i));
		if (index < 0 || index >= (int) target->uniforms.size ()) { out[i] = 0; continue; }

		UniformInfo& uniform = target->uniforms[index];

		if (pname == GL_UNIFORM_TYPE) out[i] = uniform.glType;
		else if (pname == GL_UNIFORM_SIZE) out[i] = uniform.arraySize;
		else if (pname == GL_UNIFORM_NAME_LENGTH) out[i] = (int) uniform.glName.size () + 1;
		else if (pname == GL_UNIFORM_BLOCK_INDEX) out[i] = -1;
		else if (pname == GL_UNIFORM_OFFSET) out[i] = (int) uniform.offset;
		else if (pname == GL_UNIFORM_ARRAY_STRIDE) out[i] = (int) uniform.arrayStride;
		else if (pname == GL_UNIFORM_MATRIX_STRIDE) out[i] = (int) uniform.matrixStride;
		else if (pname == GL_UNIFORM_IS_ROW_MAJOR) out[i] = uniform.rowMajor ? 1 : 0;
		else out[i] = 0;

	}

}

// ---------------------------------------------------------------------------
// 其余 shader 相关入口
// ---------------------------------------------------------------------------

// Vulkan 没有 transform feedback，这里只保留 GL 的对象生命周期语义
static std::set<int> g_transformFeedbacks;
static int g_nextTransformFeedbackId = 1;

int lime_gl_create_transform_feedback () {

	int id = g_nextTransformFeedbackId++;
	g_transformFeedbacks.insert (id);
	return id;

}

void lime_gl_delete_transform_feedback (int transformFeedback) {

	g_transformFeedbacks.erase (transformFeedback);

}

bool lime_gl_is_transform_feedback (int handle) {

	return g_transformFeedbacks.find (handle) != g_transformFeedbacks.end ();

}

void lime_gl_begin_transform_feedback (int primitiveNode) {}

void lime_gl_end_transform_feedback () {}

void lime_gl_pause_transform_feedback () {}

void lime_gl_resume_transform_feedback () {}

void lime_gl_bind_transform_feedback (int target, int transformFeedback) {}

void lime_gl_transform_feedback_varyings (int program, value varyings, int bufferMode) {}

value lime_gl_get_transform_feedback_varying (int program, int index) {

	return alloc_null ();

}

value lime_gl_get_shader_precision_format (int shadertype, int precisiontype) {

	// 0x8DF0..0x8DF5
	enum {
		GL_LOW_FLOAT = 0x8DF0,
		GL_MEDIUM_FLOAT = 0x8DF1,
		GL_HIGH_FLOAT = 0x8DF2,
		GL_LOW_INT = 0x8DF3,
		GL_MEDIUM_INT = 0x8DF4,
		GL_HIGH_INT = 0x8DF5
	};

	int rangeMin = 15, rangeMax = 15, precision = 10;

	if (precisiontype == GL_HIGH_FLOAT) { rangeMin = 127; rangeMax = 127; precision = 23; }
	else if (precisiontype == GL_LOW_FLOAT) { rangeMin = 15; rangeMax = 15; precision = 9; }
	else if (precisiontype == GL_HIGH_INT) { rangeMin = 31; rangeMax = 30; precision = 0; }
	else if (precisiontype == GL_LOW_INT) { rangeMin = 15; rangeMax = 14; precision = 0; }
	else if (precisiontype == GL_MEDIUM_INT) { rangeMin = 15; rangeMax = 14; precision = 0; }

	value result = alloc_empty_object ();
	alloc_field (result, val_id ("rangeMin"), alloc_int (rangeMin));
	alloc_field (result, val_id ("rangeMax"), alloc_int (rangeMax));
	alloc_field (result, val_id ("precision"), alloc_int (precision));
	return result;

}

void lime_gl_release_shader_compiler () {}

void lime_gl_program_parameteri (int program, int pname, int value) {}

void lime_gl_shader_binary (value shaders, int binaryformat, double binary, int length) {}

void lime_gl_program_binary (int program, int binaryFormat, double binary, int length) {}

void lime_gl_get_program_binary (int program, int binaryFormat, value bytes) {}

}





