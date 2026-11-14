// swgl internals shared between swgl.cpp (state + pipeline) and shaders.cpp (shader recognition).
#pragma once
#include "swgl.hpp"
#include <array>
#include <map>
#include <string>
#include <vector>

namespace swgl {

// GLES2 enums used by swgl
enum : GLenum {
    E_NO_ERROR = 0, E_INVALID_ENUM = 0x0500, E_INVALID_VALUE = 0x0501, E_INVALID_OPERATION = 0x0502, E_OUT_OF_MEMORY = 0x0505,
    E_POINTS = 0, E_LINES = 1, E_LINE_LOOP = 2, E_LINE_STRIP = 3, E_TRIANGLES = 4, E_TRIANGLE_STRIP = 5, E_TRIANGLE_FAN = 6,
    E_ZERO = 0, E_ONE = 1, E_SRC_COLOR = 0x0300, E_ONE_MINUS_SRC_COLOR = 0x0301, E_SRC_ALPHA = 0x0302,
    E_ONE_MINUS_SRC_ALPHA = 0x0303, E_DST_ALPHA = 0x0304, E_ONE_MINUS_DST_ALPHA = 0x0305, E_DST_COLOR = 0x0306,
    E_ONE_MINUS_DST_COLOR = 0x0307, E_SRC_ALPHA_SATURATE = 0x0308, E_CONSTANT_COLOR = 0x8001,
    E_ONE_MINUS_CONSTANT_COLOR = 0x8002, E_CONSTANT_ALPHA = 0x8003, E_ONE_MINUS_CONSTANT_ALPHA = 0x8004,
    E_FUNC_ADD = 0x8006, E_FUNC_SUBTRACT = 0x800A, E_FUNC_REVERSE_SUBTRACT = 0x800B, E_MIN = 0x8007, E_MAX = 0x8008,
    E_BYTE = 0x1400, E_UNSIGNED_BYTE = 0x1401, E_SHORT = 0x1402, E_UNSIGNED_SHORT = 0x1403, E_INT = 0x1404,
    E_UNSIGNED_INT = 0x1405, E_FLOAT = 0x1406, E_FIXED = 0x140C,
    E_ALPHA = 0x1906, E_RGB = 0x1907, E_RGBA = 0x1908, E_LUMINANCE = 0x1909, E_LUMINANCE_ALPHA = 0x190A, E_BGRA = 0x80E1,
    E_UNSIGNED_SHORT_4_4_4_4 = 0x8033, E_UNSIGNED_SHORT_5_5_5_1 = 0x8034, E_UNSIGNED_SHORT_5_6_5 = 0x8363,
    E_TEXTURE_2D = 0x0DE1, E_TEXTURE0 = 0x84C0, E_TEXTURE_MAG_FILTER = 0x2800, E_TEXTURE_MIN_FILTER = 0x2801,
    E_TEXTURE_WRAP_S = 0x2802, E_TEXTURE_WRAP_T = 0x2803, E_NEAREST = 0x2600, E_LINEAR = 0x2601,
    E_NEAREST_MIPMAP_NEAREST = 0x2700, E_LINEAR_MIPMAP_NEAREST = 0x2701, E_NEAREST_MIPMAP_LINEAR = 0x2702,
    E_LINEAR_MIPMAP_LINEAR = 0x2703, E_REPEAT = 0x2901, E_CLAMP_TO_EDGE = 0x812F, E_MIRRORED_REPEAT = 0x8370,
    E_ARRAY_BUFFER = 0x8892, E_ELEMENT_ARRAY_BUFFER = 0x8893, E_FRAMEBUFFER = 0x8D40, E_RENDERBUFFER = 0x8D41,
    E_COLOR_ATTACHMENT0 = 0x8CE0, E_DEPTH_ATTACHMENT = 0x8D00, E_STENCIL_ATTACHMENT = 0x8D20,
    E_FRAMEBUFFER_COMPLETE = 0x8CD5,
    E_BLEND = 0x0BE2, E_CULL_FACE = 0x0B44, E_DEPTH_TEST = 0x0B71, E_STENCIL_TEST = 0x0B90, E_SCISSOR_TEST = 0x0C11,
    E_DITHER = 0x0BD0, E_POLYGON_OFFSET_FILL = 0x8037, E_SAMPLE_ALPHA_TO_COVERAGE = 0x809E, E_SAMPLE_COVERAGE = 0x80A0,
    E_FRONT = 0x0404, E_BACK = 0x0405, E_FRONT_AND_BACK = 0x0408, E_CW = 0x0900, E_CCW = 0x0901,
    E_NEVER = 0x0200, E_LESS = 0x0201, E_EQUAL = 0x0202, E_LEQUAL = 0x0203, E_GREATER = 0x0204, E_NOTEQUAL = 0x0205,
    E_GEQUAL = 0x0206, E_ALWAYS = 0x0207, E_KEEP = 0x1E00, E_REPLACE = 0x1E01, E_INCR = 0x1E02, E_DECR = 0x1E03,
    E_INVERT = 0x150A, E_INCR_WRAP = 0x8507, E_DECR_WRAP = 0x8508,
    E_COLOR_BUFFER_BIT = 0x4000, E_DEPTH_BUFFER_BIT = 0x0100, E_STENCIL_BUFFER_BIT = 0x0400,
    E_VERTEX_SHADER = 0x8B31, E_FRAGMENT_SHADER = 0x8B30, E_COMPILE_STATUS = 0x8B81, E_LINK_STATUS = 0x8B82,
    E_INFO_LOG_LENGTH = 0x8B84, E_SHADER_SOURCE_LENGTH = 0x8B88, E_SHADER_TYPE = 0x8B4F, E_DELETE_STATUS = 0x8B80,
    E_VALIDATE_STATUS = 0x8B83, E_ATTACHED_SHADERS = 0x8B85, E_ACTIVE_UNIFORMS = 0x8B86, E_ACTIVE_ATTRIBUTES = 0x8B89,
    E_ACTIVE_UNIFORM_MAX_LENGTH = 0x8B87, E_ACTIVE_ATTRIBUTE_MAX_LENGTH = 0x8B8A,
    E_UNPACK_ALIGNMENT = 0x0CF5, E_PACK_ALIGNMENT = 0x0D05,
};

struct Texture {
    int w = 0, h = 0;
    std::vector<uint32_t> px;   // RGBA8, R in the lowest byte; row 0 = first row uploaded (t = 0)
    GLenum min_filter = E_NEAREST_MIPMAP_LINEAR, mag_filter = E_LINEAR, wrap_s = E_REPEAT, wrap_t = E_REPEAT;
};

// Varyings produced by the vertex stage and consumed by the fragment stage.
struct Varyings { float color[4]; float uv[2]; };

// Recognized shader kinds (by source content; see shaders.cpp).
enum class VKind { Unknown, Generic };
enum class FKind {
    Unknown, Color, Tex, TexColor, TexUColor, TexAlphaOnly, TexAlphaTest, TexMask, Circle,
};
struct VertexShape {   // what the vertex shader reads/writes
    std::string pos_attr = "a_position", color_attr, uv_attr;
    bool color_from_uniform = false;   // v_fragmentColor = u_color
    bool premultiply_color = false;    // v_color = vec4(a_color.rgb * a_color.a, a_color.a)
    bool has_uv = false, has_color = false;
};
struct Sampler { int unit = 0; };

struct Program;
// Shader stage implementations, chosen when a program links.
VertexShape classify_vertex(const std::string& src);
FKind classify_fragment(const std::string& src);
const char* fkind_name(FKind k);

// Texture sampling (swgl.cpp), used by fragment shading.
void sample(const Texture* t, float u, float v, float out[4]);

// Logging
void logf(const char* fmt, ...);

}  // namespace swgl
