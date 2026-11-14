// swgl: software OpenGL ES 2.0 (see swgl.hpp). Single-threaded GL state + scalar rasterizer.
#include "swgl_internal.hpp"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <memory>
#include <set>
#include <unordered_map>

namespace swgl {

// ---- logging -----------------------------------------------------------------------------------
static void (*g_log)(const char*);
void set_log(void (*log)(const char*)) { g_log = log; }
void logf(const char* fmt, ...) {
    char b[2048]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    if (g_log) g_log(b);
}

namespace {
// ---- objects -------------------------------------------------------------------------------------
struct Buffer { std::vector<uint8_t> data; };
struct Renderbuffer { int w = 0, h = 0; GLenum fmt = 0; };
struct Framebuffer { GLuint color_tex = 0, color_rb = 0, depth_rb = 0, stencil_rb = 0; };
struct Shader { GLenum type; std::string src; bool deleted = false; };
struct Uniform { std::string name; float v[16] = {}; int iv = 0; };
}  // namespace
struct Program {
    std::vector<GLuint> shaders;
    std::map<std::string, int> attrib_binding;   // glBindAttribLocation
    std::map<std::string, int> attrib_loc;       // after link
    std::vector<Uniform> uniforms;
    std::map<std::string, int> uniform_loc;
    bool linked = false;
    VertexShape vs; FKind fs = FKind::Unknown;
    std::string vsrc, fsrc;
    int u_mvp = -1, u_color = -1, u_alpha_value = -1, u_tex0 = -1, u_texture = -1, u_mask = -1;
};
namespace {
struct Attrib {
    bool enabled = false; int size = 4; GLenum type = E_FLOAT; bool normalized = false; int stride = 0;
    const void* ptr = nullptr; GLuint buffer = 0; float cur[4] = {0, 0, 0, 1};
};

// ---- state --------------------------------------------------------------------------------------
GLenum g_error = E_NO_ERROR;
void set_error(GLenum e) { if (g_error == E_NO_ERROR) g_error = e; }

std::unordered_map<GLuint, std::unique_ptr<Texture>> g_textures;
std::unordered_map<GLuint, std::unique_ptr<Buffer>> g_buffers;
std::unordered_map<GLuint, std::unique_ptr<Renderbuffer>> g_renderbuffers;
std::unordered_map<GLuint, std::unique_ptr<Framebuffer>> g_framebuffers;
std::unordered_map<GLuint, std::unique_ptr<Shader>> g_shaders;
std::unordered_map<GLuint, std::unique_ptr<Program>> g_programs;
GLuint g_next_name = 1;
GLuint new_name() { return g_next_name++; }

GLuint g_bound_tex[16]; int g_active_unit = 0;
GLuint g_array_buffer = 0, g_element_buffer = 0, g_bound_fb = 0, g_bound_rb = 0, g_program = 0;
Attrib g_attribs[16];

float g_clear_color[4] = {0, 0, 0, 0}; float g_clear_depth = 1; int g_clear_stencil = 0;
bool g_blend = false, g_scissor = false, g_stencil = false, g_depth = false, g_cull = false, g_dither = true;
GLenum g_blend_src_rgb = E_ONE, g_blend_dst_rgb = E_ZERO, g_blend_src_a = E_ONE, g_blend_dst_a = E_ZERO;
GLenum g_blend_eq_rgb = E_FUNC_ADD, g_blend_eq_a = E_FUNC_ADD;
float g_blend_color[4] = {0, 0, 0, 0};
bool g_color_mask[4] = {true, true, true, true}; bool g_depth_mask = true;
GLenum g_depth_func = E_LESS;
GLenum g_stencil_func = E_ALWAYS; int g_stencil_ref = 0; unsigned g_stencil_vmask = 0xFF, g_stencil_wmask = 0xFF;
GLenum g_sfail = E_KEEP, g_dpfail = E_KEEP, g_dppass = E_KEEP;
GLenum g_cull_mode = E_BACK, g_front_face = E_CCW;
int g_viewport[4] = {0, 0, 0, 0}, g_scissor_box[4] = {0, 0, 0, 0};
int g_unpack_align = 4, g_pack_align = 4;
float g_line_width = 1;

// default framebuffer
int g_def_w = 0, g_def_h = 0;
std::vector<uint32_t> g_def_color; std::vector<uint8_t> g_def_stencil; std::vector<float> g_def_depth;
// per-renderbuffer storage for depth/stencil attachments of FBOs
std::unordered_map<GLuint, std::vector<uint8_t>> g_rb_stencil;
std::unordered_map<GLuint, std::vector<float>> g_rb_depth;
std::unordered_map<GLuint, std::vector<uint32_t>> g_rb_color;

struct Target { uint32_t* color = nullptr; uint8_t* stencil = nullptr; float* depth = nullptr; int w = 0, h = 0; };
Target target() {
    Target t;
    if (!g_bound_fb) {
        t.color = g_def_color.data(); t.stencil = g_def_stencil.data(); t.depth = g_def_depth.data(); t.w = g_def_w; t.h = g_def_h;
        return t;
    }
    auto it = g_framebuffers.find(g_bound_fb);
    if (it == g_framebuffers.end()) return t;
    Framebuffer& fb = *it->second;
    if (fb.color_tex) {
        auto tt = g_textures.find(fb.color_tex);
        if (tt != g_textures.end()) { t.color = tt->second->px.data(); t.w = tt->second->w; t.h = tt->second->h; }
    } else if (fb.color_rb) {
        auto& v = g_rb_color[fb.color_rb]; auto rb = g_renderbuffers[fb.color_rb].get();
        if (rb) { v.resize((size_t)rb->w * rb->h); t.color = v.data(); t.w = rb->w; t.h = rb->h; }
    }
    auto attach = [&](GLuint rb, bool stencil) {
        if (!rb) return;
        auto r = g_renderbuffers.find(rb); if (r == g_renderbuffers.end()) return;
        if (r->second->w != t.w || r->second->h != t.h) return;
        if (stencil) { auto& s = g_rb_stencil[rb]; s.resize((size_t)t.w * t.h); t.stencil = s.data(); }
        else { auto& d = g_rb_depth[rb]; d.resize((size_t)t.w * t.h, 1.0f); t.depth = d.data(); }
    };
    attach(fb.stencil_rb, true); attach(fb.depth_rb, false);
    return t;
}

Texture* bound_texture(int unit) {
    auto it = g_textures.find(g_bound_tex[unit]);
    return it == g_textures.end() ? nullptr : it->second.get();
}
Program* current_program() {
    auto it = g_programs.find(g_program);
    return it == g_programs.end() ? nullptr : it->second.get();
}

// ---- pixel format conversion --------------------------------------------------------------------
inline uint32_t pack(int r, int g, int b, int a) { return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24); }
bool convert_row(GLenum format, GLenum type, const uint8_t* src, uint32_t* dst, int w) {
    if (type == E_UNSIGNED_BYTE) {
        switch (format) {
        case E_RGBA: memcpy(dst, src, (size_t)w * 4); return true;
        case E_BGRA: for (int x = 0; x < w; x++) dst[x] = pack(src[4 * x + 2], src[4 * x + 1], src[4 * x], src[4 * x + 3]); return true;
        case E_RGB: for (int x = 0; x < w; x++) dst[x] = pack(src[3 * x], src[3 * x + 1], src[3 * x + 2], 255); return true;
        case E_ALPHA: for (int x = 0; x < w; x++) dst[x] = pack(0, 0, 0, src[x]); return true;
        case E_LUMINANCE: for (int x = 0; x < w; x++) dst[x] = pack(src[x], src[x], src[x], 255); return true;
        case E_LUMINANCE_ALPHA: for (int x = 0; x < w; x++) dst[x] = pack(src[2 * x], src[2 * x], src[2 * x], src[2 * x + 1]); return true;
        }
        return false;
    }
    const uint16_t* s = (const uint16_t*)src;
    if (type == E_UNSIGNED_SHORT_4_4_4_4 && format == E_RGBA) {
        for (int x = 0; x < w; x++) { uint16_t v = s[x]; dst[x] = pack(((v >> 12) & 15) * 17, ((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17); }
        return true;
    }
    if (type == E_UNSIGNED_SHORT_5_5_5_1 && format == E_RGBA) {
        for (int x = 0; x < w; x++) { uint16_t v = s[x]; dst[x] = pack(((v >> 11) & 31) * 255 / 31, ((v >> 6) & 31) * 255 / 31, ((v >> 1) & 31) * 255 / 31, (v & 1) * 255); }
        return true;
    }
    if (type == E_UNSIGNED_SHORT_5_6_5 && format == E_RGB) {
        for (int x = 0; x < w; x++) { uint16_t v = s[x]; dst[x] = pack(((v >> 11) & 31) * 255 / 31, ((v >> 5) & 63) * 255 / 63, (v & 31) * 255 / 31, 255); }
        return true;
    }
    return false;
}
int bytes_per_pixel(GLenum format, GLenum type) {
    if (type != E_UNSIGNED_BYTE) return 2;
    switch (format) { case E_RGBA: case E_BGRA: return 4; case E_RGB: return 3; case E_LUMINANCE_ALPHA: return 2; default: return 1; }
}
void upload(Texture* t, int x0, int y0, int w, int h, GLenum format, GLenum type, const void* pixels) {
    if (!pixels) return;
    int bpp = bytes_per_pixel(format, type);
    size_t row = (size_t)w * bpp; row = (row + g_unpack_align - 1) / g_unpack_align * g_unpack_align;
    std::vector<uint32_t> tmp((size_t)w);
    for (int y = 0; y < h; y++) {
        if (!convert_row(format, type, (const uint8_t*)pixels + row * y, tmp.data(), w)) {
            logf("swgl: unsupported texture format %#x/%#x\n", format, type); set_error(E_INVALID_ENUM); return;
        }
        memcpy(&t->px[(size_t)(y0 + y) * t->w + x0], tmp.data(), (size_t)w * 4);
    }
}

// ---- sampling -----------------------------------------------------------------------------------
inline int wrap(int i, int n, GLenum mode) {
    if (mode == E_CLAMP_TO_EDGE) return i < 0 ? 0 : i >= n ? n - 1 : i;
    if (mode == E_MIRRORED_REPEAT) { int p = ((i % (2 * n)) + 2 * n) % (2 * n); return p < n ? p : 2 * n - 1 - p; }
    return ((i % n) + n) % n;
}
inline void texel(const Texture* t, int x, int y, float* o) {
    uint32_t c = t->px[(size_t)wrap(y, t->h, t->wrap_t) * t->w + wrap(x, t->w, t->wrap_s)];
    o[0] = (c & 255) * (1 / 255.f); o[1] = ((c >> 8) & 255) * (1 / 255.f); o[2] = ((c >> 16) & 255) * (1 / 255.f); o[3] = (c >> 24) * (1 / 255.f);
}
}  // namespace

void sample(const Texture* t, float u, float v, float out[4]) {
    if (!t || t->w == 0 || t->px.empty()) { out[0] = out[1] = out[2] = 0; out[3] = 1; return; }   // incomplete texture: black
    float fx = u * t->w, fy = v * t->h;
    if (t->mag_filter == E_NEAREST) { texel(t, (int)std::floor(fx), (int)std::floor(fy), out); return; }
    fx -= 0.5f; fy -= 0.5f;
    int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
    float ax = fx - x0, ay = fy - y0, a[4], b[4], c[4], d[4];
    texel(t, x0, y0, a); texel(t, x0 + 1, y0, b); texel(t, x0, y0 + 1, c); texel(t, x0 + 1, y0 + 1, d);
    for (int i = 0; i < 4; i++) {
        float top = a[i] + (b[i] - a[i]) * ax, bot = c[i] + (d[i] - c[i]) * ax;
        out[i] = top + (bot - top) * ay;
    }
}

namespace {
// ---- fragment shading -----------------------------------------------------------------------------
std::set<std::string> g_warned;
// Returns false on discard.
inline bool shade(const Program& p, const Varyings& in, float out[4]) {
    auto tex_unit = [&](int loc) { return loc >= 0 ? p.uniforms[loc].iv : 0; };
    float t[4];
    switch (p.fs) {
    case FKind::Color: memcpy(out, in.color, 16); return true;
    case FKind::Tex: sample(bound_texture(tex_unit(p.u_tex0)), in.uv[0], in.uv[1], out); return true;
    case FKind::TexColor:
        sample(bound_texture(tex_unit(p.u_tex0)), in.uv[0], in.uv[1], t);
        for (int i = 0; i < 4; i++) out[i] = t[i] * in.color[i];
        return true;
    case FKind::TexUColor: {
        sample(bound_texture(tex_unit(p.u_tex0)), in.uv[0], in.uv[1], t);
        const float* uc = p.u_color >= 0 ? p.uniforms[p.u_color].v : nullptr;
        for (int i = 0; i < 4; i++) out[i] = t[i] * (uc ? uc[i] : 1.f);
        return true;
    }
    case FKind::TexAlphaOnly:
        sample(bound_texture(tex_unit(p.u_tex0)), in.uv[0], in.uv[1], t);
        out[0] = in.color[0]; out[1] = in.color[1]; out[2] = in.color[2]; out[3] = in.color[3] * t[3];
        return true;
    case FKind::TexAlphaTest: {
        sample(bound_texture(tex_unit(p.u_tex0)), in.uv[0], in.uv[1], t);
        float ref = p.u_alpha_value >= 0 ? p.uniforms[p.u_alpha_value].v[0] : 0.f;
        if (t[3] <= ref) return false;
        for (int i = 0; i < 4; i++) out[i] = t[i] * in.color[i];
        return true;
    }
    case FKind::TexMask: {
        float m[4];
        sample(bound_texture(tex_unit(p.u_texture)), in.uv[0], in.uv[1], t);
        sample(bound_texture(tex_unit(p.u_mask)), in.uv[0], in.uv[1], m);
        out[0] = in.color[0] * t[0]; out[1] = in.color[1] * t[1]; out[2] = in.color[2] * t[2]; out[3] = in.color[3] * m[3] * t[3];
        return true;
    }
    case FKind::Circle: {
        float len = std::sqrt(in.uv[0] * in.uv[0] + in.uv[1] * in.uv[1]);
        float s = (1.0f - len) >= 0 ? 1.f : 0.f;
        for (int i = 0; i < 4; i++) out[i] = in.color[i] * s;
        return true;
    }
    case FKind::Unknown:
    default:
        // Not translated yet: draw as texture*color (if the vertex shader provides UVs) or color.
        if (p.vs.has_uv) { sample(bound_texture(tex_unit(p.u_tex0)), in.uv[0], in.uv[1], t); for (int i = 0; i < 4; i++) out[i] = t[i] * in.color[i]; }
        else memcpy(out, in.color, 16);
        return true;
    }
}

// ---- per-fragment operations ------------------------------------------------------------------------
inline float factor(GLenum f, const float* s, const float* d, int i) {
    switch (f) {
    case E_ZERO: return 0; case E_ONE: return 1;
    case E_SRC_COLOR: return s[i]; case E_ONE_MINUS_SRC_COLOR: return 1 - s[i];
    case E_DST_COLOR: return d[i]; case E_ONE_MINUS_DST_COLOR: return 1 - d[i];
    case E_SRC_ALPHA: return s[3]; case E_ONE_MINUS_SRC_ALPHA: return 1 - s[3];
    case E_DST_ALPHA: return d[3]; case E_ONE_MINUS_DST_ALPHA: return 1 - d[3];
    case E_CONSTANT_COLOR: return g_blend_color[i]; case E_ONE_MINUS_CONSTANT_COLOR: return 1 - g_blend_color[i];
    case E_CONSTANT_ALPHA: return g_blend_color[3]; case E_ONE_MINUS_CONSTANT_ALPHA: return 1 - g_blend_color[3];
    case E_SRC_ALPHA_SATURATE: return i == 3 ? 1 : std::min(s[3], 1 - d[3]);
    }
    return 1;
}
inline float equation(GLenum eq, float s, float d) {
    switch (eq) {
    case E_FUNC_SUBTRACT: return s - d; case E_FUNC_REVERSE_SUBTRACT: return d - s;
    case E_MIN: return std::min(s, d); case E_MAX: return std::max(s, d);
    }
    return s + d;
}
inline bool compare(GLenum f, unsigned a, unsigned b) {
    switch (f) {
    case E_NEVER: return false; case E_LESS: return a < b; case E_EQUAL: return a == b; case E_LEQUAL: return a <= b;
    case E_GREATER: return a > b; case E_NOTEQUAL: return a != b; case E_GEQUAL: return a >= b;
    }
    return true;
}
inline bool comparef(GLenum f, float a, float b) {
    switch (f) {
    case E_NEVER: return false; case E_LESS: return a < b; case E_EQUAL: return a == b; case E_LEQUAL: return a <= b;
    case E_GREATER: return a > b; case E_NOTEQUAL: return a != b; case E_GEQUAL: return a >= b;
    }
    return true;
}
inline uint8_t stencil_op(GLenum op, uint8_t v) {
    switch (op) {
    case E_ZERO: return 0; case E_REPLACE: return (uint8_t)g_stencil_ref; case E_INCR: return v == 255 ? 255 : v + 1;
    case E_DECR: return v == 0 ? 0 : v - 1; case E_INVERT: return (uint8_t)~v; case E_INCR_WRAP: return (uint8_t)(v + 1);
    case E_DECR_WRAP: return (uint8_t)(v - 1);
    }
    return v;
}
inline void write_stencil(uint8_t* s, uint8_t nv) { *s = (uint8_t)((*s & ~g_stencil_wmask) | (nv & g_stencil_wmask)); }

inline void output(const Target& t, int x, int y, float z, const float* src_in) {
    size_t i = (size_t)y * t.w + x;
    if (g_stencil && t.stencil) {
        uint8_t* s = &t.stencil[i];
        if (!compare(g_stencil_func, (unsigned)g_stencil_ref & g_stencil_vmask, *s & g_stencil_vmask)) {
            write_stencil(s, stencil_op(g_sfail, *s)); return;
        }
        if (g_depth && t.depth && !comparef(g_depth_func, z, t.depth[i])) { write_stencil(s, stencil_op(g_dpfail, *s)); return; }
        write_stencil(s, stencil_op(g_dppass, *s));
    } else if (g_depth && t.depth && !comparef(g_depth_func, z, t.depth[i])) return;
    if (g_depth && t.depth && g_depth_mask) t.depth[i] = z;
    float src[4];
    for (int k = 0; k < 4; k++) src[k] = src_in[k] < 0 ? 0 : src_in[k] > 1 ? 1 : src_in[k];
    uint32_t dc = t.color[i];
    float out[4];
    if (g_blend) {
        float d[4] = {(dc & 255) / 255.f, ((dc >> 8) & 255) / 255.f, ((dc >> 16) & 255) / 255.f, (dc >> 24) / 255.f};
        for (int k = 0; k < 3; k++)
            out[k] = equation(g_blend_eq_rgb, src[k] * factor(g_blend_src_rgb, src, d, k), d[k] * factor(g_blend_dst_rgb, src, d, k));
        out[3] = equation(g_blend_eq_a, src[3] * factor(g_blend_src_a, src, d, 3), d[3] * factor(g_blend_dst_a, src, d, 3));
    } else memcpy(out, src, 16);
    uint32_t r = dc;
    for (int k = 0; k < 4; k++) {
        if (!g_color_mask[k]) continue;
        float v = out[k] < 0 ? 0 : out[k] > 1 ? 1 : out[k];
        uint32_t b = (uint32_t)(v * 255.f + 0.5f);
        r = (r & ~(0xFFu << (8 * k))) | (b << (8 * k));
    }
    t.color[i] = r;
}

// ---- vertex processing ----------------------------------------------------------------------------
struct V { float x, y, z, w; Varyings var; };   // clip-space position + varyings

void fetch(const Attrib& a, int idx, float out[4]) {
    out[0] = a.cur[0]; out[1] = a.cur[1]; out[2] = a.cur[2]; out[3] = a.cur[3];
    if (!a.enabled) return;
    const uint8_t* base;
    if (a.buffer) {
        auto it = g_buffers.find(a.buffer); if (it == g_buffers.end()) return;
        base = it->second->data.data() + (uintptr_t)a.ptr;
    } else base = (const uint8_t*)a.ptr;
    if (!base) return;
    int comp = a.type == E_FLOAT ? 4 : (a.type == E_SHORT || a.type == E_UNSIGNED_SHORT) ? 2 : 1;
    int stride = a.stride ? a.stride : comp * a.size;
    const uint8_t* p = base + (size_t)stride * idx;
    out[0] = 0; out[1] = 0; out[2] = 0; out[3] = 1;
    for (int c = 0; c < a.size && c < 4; c++) {
        switch (a.type) {
        case E_FLOAT: out[c] = ((const float*)p)[c]; break;
        case E_UNSIGNED_BYTE: out[c] = a.normalized ? p[c] / 255.f : p[c]; break;
        case E_BYTE: out[c] = a.normalized ? std::max(((const int8_t*)p)[c] / 127.f, -1.f) : ((const int8_t*)p)[c]; break;
        case E_UNSIGNED_SHORT: out[c] = a.normalized ? ((const uint16_t*)p)[c] / 65535.f : ((const uint16_t*)p)[c]; break;
        case E_SHORT: out[c] = a.normalized ? std::max(((const int16_t*)p)[c] / 32767.f, -1.f) : ((const int16_t*)p)[c]; break;
        }
    }
}
int attrib_index(const Program& p, const std::string& name) {
    auto it = p.attrib_loc.find(name);
    return it == p.attrib_loc.end() ? -1 : it->second;
}
void vertex(const Program& p, int idx, V& v) {
    float pos[4] = {0, 0, 0, 1};
    int ip = attrib_index(p, p.vs.pos_attr);
    if (ip >= 0) fetch(g_attribs[ip], idx, pos);
    const float* m = p.u_mvp >= 0 ? p.uniforms[p.u_mvp].v : nullptr;
    if (m) {   // column-major mat4 * vec4
        v.x = m[0] * pos[0] + m[4] * pos[1] + m[8] * pos[2] + m[12] * pos[3];
        v.y = m[1] * pos[0] + m[5] * pos[1] + m[9] * pos[2] + m[13] * pos[3];
        v.z = m[2] * pos[0] + m[6] * pos[1] + m[10] * pos[2] + m[14] * pos[3];
        v.w = m[3] * pos[0] + m[7] * pos[1] + m[11] * pos[2] + m[15] * pos[3];
    } else { v.x = pos[0]; v.y = pos[1]; v.z = pos[2]; v.w = pos[3]; }
    float* c = v.var.color; c[0] = c[1] = c[2] = c[3] = 1;
    if (p.vs.color_from_uniform && p.u_color >= 0) memcpy(c, p.uniforms[p.u_color].v, 16);
    else if (!p.vs.color_attr.empty()) {
        int ic = attrib_index(p, p.vs.color_attr);
        if (ic >= 0) fetch(g_attribs[ic], idx, c);
        if (p.vs.premultiply_color) { c[0] *= c[3]; c[1] *= c[3]; c[2] *= c[3]; }
    }
    v.var.uv[0] = v.var.uv[1] = 0;
    if (p.vs.has_uv) {
        int iu = attrib_index(p, p.vs.uv_attr);
        float uv[4]; if (iu >= 0) { fetch(g_attribs[iu], idx, uv); v.var.uv[0] = uv[0]; v.var.uv[1] = uv[1]; }
    }
}

// ---- rasterization --------------------------------------------------------------------------------
// Span-based: per triangle, three edge equations and one plane equation per varying; per row, the
// exact covered x-span is solved from the edges (top-left fill rule), and varyings step linearly.
// 2D draws (w == 1) with the common shaders/blend modes use an integer fast path (fast_span).
struct SV { float x, y, z, iw; float a[6]; };   // screen-space vertex, varyings pre-divided by w
constexpr int kVar = 6;
void to_screen(const V& v, SV& s) {
    float iw = 1.0f / v.w;
    s.x = g_viewport[0] + (v.x * iw * 0.5f + 0.5f) * g_viewport[2];
    s.y = g_viewport[1] + (v.y * iw * 0.5f + 0.5f) * g_viewport[3];
    s.z = v.z * iw * 0.5f + 0.5f;
    s.iw = iw;
    const float* src = v.var.color;   // color[4] then uv[2] are contiguous in Varyings
    for (int i = 0; i < kVar; i++) s.a[i] = src[i] * iw;
}

// ---- integer helpers (8-bit channels, packed RGBA with R in the low byte) ----
inline uint32_t lerp32(uint32_t c0, uint32_t c1, uint32_t a) {   // a in [0,256]
    uint32_t rb = (((c0 & 0x00FF00FFu) * (256 - a) + (c1 & 0x00FF00FFu) * a) >> 8) & 0x00FF00FFu;
    uint32_t ag = (((c0 >> 8) & 0x00FF00FFu) * (256 - a) + ((c1 >> 8) & 0x00FF00FFu) * a) & 0xFF00FF00u;
    return rb | ag;
}
inline int wrapi(int i, int n, GLenum mode) {
    if ((unsigned)i < (unsigned)n) return i;
    return wrap(i, n, mode);
}
// floor() for |x| < 2^22 without a libm call: truncate a positively offset value.
inline int ifloor(float x) { return (int)(x + 4194304.0f) - 4194304; }
// Texture coordinates in texels*256 (8 fractional bits, centered for bilinear): integer math from here on.
inline uint32_t sample_fx(const Texture* t, int fx, int fy) {
    uint32_t ax = (uint32_t)(fx & 255), ay = (uint32_t)(fy & 255);
    int x0 = fx >> 8, y0 = fy >> 8;
    const int w = t->w, h = t->h;
    const uint32_t* px = t->px.data();
    if ((unsigned)x0 < (unsigned)(w - 1) && (unsigned)y0 < (unsigned)(h - 1)) {   // all four texels inside: no wrapping
        const uint32_t* r0 = px + (size_t)y0 * w + x0;
        const uint32_t* r1 = r0 + w;
        if ((r0[0] | r0[1] | r1[0] | r1[1]) == 0) return 0;   // transparent black (common in sprite sheets)
        return lerp32(lerp32(r0[0], r0[1], ax), lerp32(r1[0], r1[1], ax), ay);
    }
    int xa = wrapi(x0, w, t->wrap_s), xb = wrapi(x0 + 1, w, t->wrap_s);
    const uint32_t* r0 = px + (size_t)wrapi(y0, h, t->wrap_t) * w;
    const uint32_t* r1 = px + (size_t)wrapi(y0 + 1, h, t->wrap_t) * w;
    return lerp32(lerp32(r0[xa], r0[xb], ax), lerp32(r1[xa], r1[xb], ax), ay);
}
inline uint32_t mul8(uint32_t a, uint32_t b) { uint32_t t = a * b + 128; return (t + (t >> 8)) >> 8; }   // a*b/255
// All four channels times one factor f (0..255), two channels per 32-bit multiply.
inline uint32_t scale32(uint32_t c, uint32_t f) {
    f += f >> 7;   // 0..255 -> 0..256 (255 maps to exactly 256)
    uint32_t rb = (((c & 0x00FF00FFu) * f) >> 8) & 0x00FF00FFu;
    uint32_t ag = (((c >> 8) & 0x00FF00FFu) * f) & 0xFF00FF00u;
    return rb | ag;
}
inline uint32_t modulate(uint32_t c, const uint32_t m[4]) {   // per-channel multiply by m (0..255)
    if (m[0] == m[1] && m[1] == m[2] && m[2] == m[3]) return scale32(c, m[0]);   // uniform tint/opacity
    return mul8(c & 255, m[0]) | (mul8((c >> 8) & 255, m[1]) << 8) | (mul8((c >> 16) & 255, m[2]) << 16) | (mul8(c >> 24, m[3]) << 24);
}
// Blend factor in 0..255 for one channel (k) given source/destination pixels.
inline uint32_t ifactor(GLenum f, uint32_t s, uint32_t d, int k) {
    switch (f) {
    case E_ZERO: return 0; case E_ONE: return 255;
    case E_SRC_ALPHA: return s >> 24; case E_ONE_MINUS_SRC_ALPHA: return 255 - (s >> 24);
    case E_DST_ALPHA: return d >> 24; case E_ONE_MINUS_DST_ALPHA: return 255 - (d >> 24);
    case E_SRC_COLOR: return (s >> (8 * k)) & 255; case E_ONE_MINUS_SRC_COLOR: return 255 - ((s >> (8 * k)) & 255);
    case E_DST_COLOR: return (d >> (8 * k)) & 255; case E_ONE_MINUS_DST_COLOR: return 255 - ((d >> (8 * k)) & 255);
    }
    return 255;
}
inline bool int_blend_ok(GLenum f) {
    switch (f) {
    case E_ZERO: case E_ONE: case E_SRC_ALPHA: case E_ONE_MINUS_SRC_ALPHA: case E_DST_ALPHA: case E_ONE_MINUS_DST_ALPHA:
    case E_SRC_COLOR: case E_ONE_MINUS_SRC_COLOR: case E_DST_COLOR: case E_ONE_MINUS_DST_COLOR: return true;
    }
    return false;
}
inline uint32_t blend_i(uint32_t s, uint32_t d) {
    // Most common in cocos2d: premultiplied (ONE, ONE_MINUS_SRC_ALPHA) and straight alpha.
    if (g_blend_src_rgb == E_ONE && g_blend_dst_rgb == E_ONE_MINUS_SRC_ALPHA && g_blend_src_a == E_ONE && g_blend_dst_a == E_ONE_MINUS_SRC_ALPHA) {
        uint32_t ia = 255 - (s >> 24);
        if (ia == 0) return s;
        if (ia == 255 && s == 0) return d;
        // s + d*(1-sa), packed; premultiplied colors keep each channel <= 255 (saturate defensively)
        uint32_t dd = scale32(d, ia);
        uint32_t rb = (s & 0x00FF00FFu) + (dd & 0x00FF00FFu), ag = ((s >> 8) & 0x00FF00FFu) + ((dd >> 8) & 0x00FF00FFu);
        rb |= ((rb >> 8) & 0x00010001u) * 255; ag |= ((ag >> 8) & 0x00010001u) * 255;   // saturate
        return (rb & 0x00FF00FFu) | ((ag & 0x00FF00FFu) << 8);
    }
    if (g_blend_src_rgb == E_SRC_ALPHA && g_blend_dst_rgb == E_ONE_MINUS_SRC_ALPHA &&
        g_blend_src_a == E_SRC_ALPHA && g_blend_dst_a == E_ONE_MINUS_SRC_ALPHA) {   // straight alpha
        uint32_t sa = s >> 24;
        if (sa == 0) return d;
        if (sa == 255) return s;
        uint32_t f = sa + (sa >> 7);
        uint32_t rb = (((s & 0x00FF00FFu) * f + (d & 0x00FF00FFu) * (256 - f)) >> 8) & 0x00FF00FFu;
        uint32_t ag = (((s >> 8) & 0x00FF00FFu) * f + ((d >> 8) & 0x00FF00FFu) * (256 - f)) & 0xFF00FF00u;
        // alpha channel: sa*sa + da*(1-sa) per the blend equation
        uint32_t a = mul8(sa, sa) + mul8(d >> 24, 255 - sa);
        return ((rb | ag) & 0x00FFFFFFu) | ((a > 255 ? 255 : a) << 24);
    }
    if (g_blend_src_rgb == E_SRC_ALPHA && g_blend_dst_rgb == E_ONE && g_blend_src_a == E_SRC_ALPHA && g_blend_dst_a == E_ONE) {   // additive glow
        uint32_t ss = scale32(s, s >> 24);
        uint32_t rb = (ss & 0x00FF00FFu) + (d & 0x00FF00FFu), ag = ((ss >> 8) & 0x00FF00FFu) + ((d >> 8) & 0x00FF00FFu);
        rb |= ((rb >> 8) & 0x00010001u) * 255; ag |= ((ag >> 8) & 0x00010001u) * 255;
        return (rb & 0x00FF00FFu) | ((ag & 0x00FF00FFu) << 8);
    }
    uint32_t r = 0;
    for (int k = 0; k < 4; k++) {
        GLenum fs = k == 3 ? g_blend_src_a : g_blend_src_rgb, fd = k == 3 ? g_blend_dst_a : g_blend_dst_rgb;
        uint32_t sc = (s >> (8 * k)) & 255, dc = (d >> (8 * k)) & 255;
        uint32_t v = mul8(sc, ifactor(fs, s, d, k)) + mul8(dc, ifactor(fd, s, d, k));
        r |= (v > 255 ? 255 : v) << (8 * k);
    }
    return r;
}

struct Plane { float dx, dy, c; float at(float x, float y) const { return dx * x + dy * y + c; } };

// Integer fast path: is this draw eligible?
struct Fast { bool ok = false; FKind kind = FKind::Unknown; const Texture* tex = nullptr; uint32_t ucolor[4] = {255, 255, 255, 255}; };
Fast fast_path(const Program& p, const Target& t, bool affine) {
    Fast f;
    if (!affine || (g_stencil && t.stencil) || (g_depth && t.depth)) return f;
    if (!(g_color_mask[0] && g_color_mask[1] && g_color_mask[2] && g_color_mask[3])) return f;
    if (g_blend && (g_blend_eq_rgb != E_FUNC_ADD || g_blend_eq_a != E_FUNC_ADD || !int_blend_ok(g_blend_src_rgb) ||
                    !int_blend_ok(g_blend_dst_rgb) || !int_blend_ok(g_blend_src_a) || !int_blend_ok(g_blend_dst_a))) return f;
    switch (p.fs) {
    case FKind::Color: break;
    case FKind::Tex: case FKind::TexColor: case FKind::TexUColor: case FKind::TexAlphaOnly: {
        int unit = p.u_tex0 >= 0 ? p.uniforms[p.u_tex0].iv : 0;
        f.tex = bound_texture(unit);
        if (!f.tex || f.tex->w == 0 || f.tex->px.empty()) return f;   // incomplete texture: generic path
        break;
    }
    default: return f;
    }
    if (p.fs == FKind::TexUColor && p.u_color >= 0)
        for (int k = 0; k < 4; k++) f.ucolor[k] = (uint32_t)(std::min(std::max(p.uniforms[p.u_color].v[k], 0.f), 1.f) * 255.f + 0.5f);
    f.kind = p.fs; f.ok = true;
    return f;
}
inline uint32_t to8(float v) { return v <= 0 ? 0 : v >= 1 ? 255 : (uint32_t)(v * 255.f + 0.5f); }

void fast_span(const Fast& f, const Target& t, int y, int x0, int x1, const Plane* pl, float px0, float py) {
    uint32_t* row = t.color + (size_t)y * t.w;
    float c[4], dc[4];
    for (int k = 0; k < 4; k++) { c[k] = pl[k].at(px0, py); dc[k] = pl[k].dx; }
    // Texture coordinates stepped in fixed point (texels * 256, bilinear-centered) - no float->int per pixel.
    bool tex = f.tex != nullptr, bilinear = tex && f.tex->mag_filter != E_NEAREST;
    float fu = 0, fv = 0, fdu = 0, fdv = 0;
    if (tex) {
        float sw = f.tex->w * 256.f, sh = f.tex->h * 256.f;
        fu = pl[4].at(px0, py) * sw - (bilinear ? 128.f : 0.f); fv = pl[5].at(px0, py) * sh - (bilinear ? 128.f : 0.f);
        fdu = pl[4].dx * sw; fdv = pl[5].dx * sh;
    }
    // Vertex color constant across the span? (sprites: always) -> convert once.
    bool const_color = dc[0] == 0 && dc[1] == 0 && dc[2] == 0 && dc[3] == 0;
    uint32_t m[4] = {to8(c[0]), to8(c[1]), to8(c[2]), to8(c[3])};
    bool white = m[0] == 255 && m[1] == 255 && m[2] == 255 && m[3] == 255;
    const bool blend = g_blend;
    for (int x = x0; x <= x1; x++) {
        if (!const_color) { m[0] = to8(c[0]); m[1] = to8(c[1]); m[2] = to8(c[2]); m[3] = to8(c[3]); white = (m[0] & m[1] & m[2] & m[3]) == 255; }
        uint32_t src;
        uint32_t texel = 0;
        if (tex) {
            if (bilinear) texel = sample_fx(f.tex, ifloor(fu), ifloor(fv));
            else {
                int tx = wrapi(ifloor(fu) >> 8, f.tex->w, f.tex->wrap_s), ty = wrapi(ifloor(fv) >> 8, f.tex->h, f.tex->wrap_t);
                texel = f.tex->px[(size_t)ty * f.tex->w + tx];
            }
        }
        switch (f.kind) {
        case FKind::Color: src = m[0] | (m[1] << 8) | (m[2] << 16) | (m[3] << 24); break;
        case FKind::Tex: src = texel; break;
        case FKind::TexUColor: src = modulate(texel, f.ucolor); break;
        case FKind::TexAlphaOnly: src = m[0] | (m[1] << 8) | (m[2] << 16) | (mul8(m[3], texel >> 24) << 24); break;
        default: src = white ? texel : modulate(texel, m); break;   // TexColor
        }
        if (!blend) row[x] = src;
        else if (src != 0 || g_blend_dst_rgb != E_ONE_MINUS_SRC_ALPHA || g_blend_dst_a != E_ONE_MINUS_SRC_ALPHA)
            row[x] = blend_i(src, row[x]);   // (a fully transparent texel leaves the destination unchanged)
        if (!const_color) for (int k = 0; k < 4; k++) c[k] += dc[k];
        fu += fdu; fv += fdv;
    }
}

void triangle(const Program& p, const Target& t, const V& v0, const V& v1, const V& v2, int band_y0, int band_y1) {
    if (v0.w <= 0 || v1.w <= 0 || v2.w <= 0) return;   // behind the eye: 2D content never is
    SV s[3]; to_screen(v0, s[0]); to_screen(v1, s[1]); to_screen(v2, s[2]);
    float area = (s[1].x - s[0].x) * (s[2].y - s[0].y) - (s[2].x - s[0].x) * (s[1].y - s[0].y);
    if (area == 0) return;
    if (g_cull) {
        bool ccw = area > 0, front = (g_front_face == E_CCW) == ccw;
        if (g_cull_mode == E_FRONT_AND_BACK || (g_cull_mode == E_BACK && !front) || (g_cull_mode == E_FRONT && front)) return;
    }
    if (area < 0) { std::swap(s[1], s[2]); area = -area; }
    int minx = (int)std::floor(std::min({s[0].x, s[1].x, s[2].x})), maxx = (int)std::ceil(std::max({s[0].x, s[1].x, s[2].x}));
    int miny = (int)std::floor(std::min({s[0].y, s[1].y, s[2].y})), maxy = (int)std::ceil(std::max({s[0].y, s[1].y, s[2].y}));
    minx = std::max(minx, 0); miny = std::max(miny, 0); maxx = std::min(maxx, t.w - 1); maxy = std::min(maxy, t.h - 1);
    if (g_scissor) {
        minx = std::max(minx, g_scissor_box[0]); miny = std::max(miny, g_scissor_box[1]);
        maxx = std::min(maxx, g_scissor_box[0] + g_scissor_box[2] - 1); maxy = std::min(maxy, g_scissor_box[1] + g_scissor_box[3] - 1);
    }
    miny = std::max(miny, band_y0); maxy = std::min(maxy, band_y1 - 1);   // this worker's rows only
    if (minx > maxx || miny > maxy) return;

    // Edge i is opposite vertex i: E_i(x,y) = A_i x + B_i y + C_i, >= 0 inside (CCW, y up).
    float A[3], B[3], C[3]; bool tl[3];
    for (int i = 0; i < 3; i++) {
        const SV& a = s[(i + 1) % 3]; const SV& b = s[(i + 2) % 3];
        float dx = b.x - a.x, dy = b.y - a.y;
        A[i] = -dy; B[i] = dx; C[i] = -dx * a.y + dy * a.x;
        tl[i] = (dy < 0) || (dy == 0 && dx > 0);   // top-left edges own their pixels
    }
    // Plane equations for 1/w, z and the varyings (barycentric = E_i / area).
    float inv_area = 1.0f / area;
    auto plane = [&](float f0, float f1, float f2) {
        return Plane{(A[0] * f0 + A[1] * f1 + A[2] * f2) * inv_area, (B[0] * f0 + B[1] * f1 + B[2] * f2) * inv_area,
                     (C[0] * f0 + C[1] * f1 + C[2] * f2) * inv_area};
    };
    Plane pl[kVar], piw = plane(s[0].iw, s[1].iw, s[2].iw), pz = plane(s[0].z, s[1].z, s[2].z);
    for (int k = 0; k < kVar; k++) pl[k] = plane(s[0].a[k], s[1].a[k], s[2].a[k]);
    bool affine = std::fabs(s[0].iw - s[1].iw) < 1e-6f && std::fabs(s[0].iw - s[2].iw) < 1e-6f;
    if (affine && s[0].iw != 1.0f) for (int k = 0; k < kVar; k++) { pl[k].dx /= s[0].iw; pl[k].dy /= s[0].iw; pl[k].c /= s[0].iw; }
    Fast fp = fast_path(p, t, affine);

    for (int y = miny; y <= maxy; y++) {
        float py = y + 0.5f;
        // Solve each edge for the covered x range on this row (pixel centers at x + 0.5).
        int xs = minx, xe = maxx;
        for (int i = 0; i < 3 && xs <= xe; i++) {
            float rowc = B[i] * py + C[i];
            if (A[i] > 0) {        // E >= 0  <=>  x + 0.5 >= -rowc / A
                float tx = -rowc / A[i] - 0.5f;
                int lo = tl[i] ? (int)std::ceil(tx) : (int)std::floor(tx) + 1;
                xs = std::max(xs, lo);
            } else if (A[i] < 0) { // E >= 0  <=>  x + 0.5 <= rowc / -A
                float tx = rowc / -A[i] - 0.5f;
                int hi = tl[i] ? (int)std::floor(tx) : (int)std::ceil(tx) - 1;
                xe = std::min(xe, hi);
            } else if (rowc < 0 || (rowc == 0 && !tl[i])) xs = xe + 1;   // horizontal edge: row fully in or out
        }
        if (xs > xe) continue;
        if (fp.ok) { fast_span(fp, t, y, xs, xe, pl, xs + 0.5f, py); continue; }
        // Generic path: exact float shading, stencil/depth, any blend mode, perspective-correct.
        for (int x = xs; x <= xe; x++) {
            float px = x + 0.5f;
            Varyings var; float* a = var.color;
            if (affine) for (int k = 0; k < kVar; k++) a[k] = pl[k].at(px, py);
            else { float wc = 1.0f / piw.at(px, py); for (int k = 0; k < kVar; k++) a[k] = pl[k].at(px, py) * wc; }
            float c[4];
            if (!shade(p, var, c)) continue;
            output(t, x, y, pz.at(px, py), c);
        }
    }
}
// Lines and points (cocos2d debug drawing): rendered as screen-space quads (two triangles appended to out).
void line_quad(const V& a, const V& b, std::vector<V>& out) {
    float hw = std::max(g_line_width, 1.0f) * 0.5f;
    float ax = a.x / a.w, ay = a.y / a.w, bx = b.x / b.w, by = b.y / b.w;
    float dx = (bx - ax) * g_viewport[2], dy = (by - ay) * g_viewport[3], len = std::sqrt(dx * dx + dy * dy);
    if (len == 0) return;
    float nx = -dy / len * hw * 2 / g_viewport[2], ny = dx / len * hw * 2 / g_viewport[3];
    V q[4] = {a, a, b, b};
    q[0].x = (ax + nx) * a.w; q[0].y = (ay + ny) * a.w; q[1].x = (ax - nx) * a.w; q[1].y = (ay - ny) * a.w;
    q[2].x = (bx + nx) * b.w; q[2].y = (by + ny) * b.w; q[3].x = (bx - nx) * b.w; q[3].y = (by - ny) * b.w;
    out.push_back(q[0]); out.push_back(q[1]); out.push_back(q[2]);
    out.push_back(q[1]); out.push_back(q[3]); out.push_back(q[2]);
}

// ---- worker pool: large draws are split into horizontal bands, one per thread ----------------------
// GL state is only read while a draw runs, and the calling thread waits for all bands before
// returning, so draw order and state changes stay exactly as issued.
int g_thread_override = 0;
struct Pool {
    std::vector<std::thread> threads;
    std::mutex m;
    std::condition_variable cv, done_cv;
    const std::function<void(int, int)>* job = nullptr;   // (band index, band count)
    uint64_t generation = 0;
    int pending = 0, nbands = 1;
    bool started = false;
    void start() {
        unsigned hw = std::thread::hardware_concurrency();
        int workers = (int)std::min(hw > 1 ? hw - 1 : 0u, 5u);   // + the calling thread = up to 6 bands (PS4 game cores)
        if (g_thread_override > 0) workers = g_thread_override - 1;
        if (const char* e = getenv("SWGL_THREADS")) workers = std::max(0, atoi(e) - 1);
        nbands = workers + 1;
        for (int i = 0; i < workers; i++)
            threads.emplace_back([this, i] {
                uint64_t seen = 0;
                for (;;) {
                    const std::function<void(int, int)>* j;
                    {
                        std::unique_lock<std::mutex> l(m);
                        cv.wait(l, [&] { return generation != seen; });
                        seen = generation; j = job;
                    }
                    (*j)(i + 1, nbands);
                    std::lock_guard<std::mutex> l(m);
                    if (--pending == 0) done_cv.notify_one();
                }
            });
        started = true;
        logf("swgl: %d render threads\n", nbands);
    }
    void run(const std::function<void(int, int)>& f) {
        if (!started) start();
        if (nbands == 1) { f(0, 1); return; }
        { std::lock_guard<std::mutex> l(m); job = &f; pending = nbands - 1; generation++; }
        cv.notify_all();
        f(0, nbands);   // the calling thread takes band 0
        std::unique_lock<std::mutex> l(m);
        done_cv.wait(l, [&] { return pending == 0; });
    }
};
Pool g_pool;

void draw(GLenum mode, int count, const std::function<int(int)>& index) {
    Program* p = current_program();
    if (!p || !p->linked) { set_error(E_INVALID_OPERATION); return; }
    Target t = target();
    if (!t.color || count <= 0) return;
    std::vector<V> v((size_t)count);
    for (int i = 0; i < count; i++) vertex(*p, index(i), v[i]);
    // Assemble triangles (as vertex triples) from the primitive mode.
    std::vector<V> tri;
    tri.reserve((size_t)count * 3);
    auto add = [&](const V& a, const V& b, const V& c) { tri.push_back(a); tri.push_back(b); tri.push_back(c); };
    switch (mode) {
    case E_TRIANGLES: for (int i = 0; i + 2 < count; i += 3) add(v[i], v[i + 1], v[i + 2]); break;
    case E_TRIANGLE_STRIP:
        for (int i = 0; i + 2 < count; i++) { if (i & 1) add(v[i + 1], v[i], v[i + 2]); else add(v[i], v[i + 1], v[i + 2]); }
        break;
    case E_TRIANGLE_FAN: for (int i = 1; i + 1 < count; i++) add(v[0], v[i], v[i + 1]); break;
    case E_LINES: for (int i = 0; i + 1 < count; i += 2) line_quad(v[i], v[i + 1], tri); break;
    case E_LINE_STRIP: case E_LINE_LOOP:
        for (int i = 0; i + 1 < count; i++) line_quad(v[i], v[i + 1], tri);
        if (mode == E_LINE_LOOP && count > 2) line_quad(v[count - 1], v[0], tri);
        break;
    case E_POINTS: break;   // unused by GD
    }
    if (tri.empty()) return;
    // Screen-space extent and rough pixel count decide whether threading pays off.
    float miny = 1e30f, maxy = -1e30f, work = 0;
    for (size_t i = 0; i + 2 < tri.size(); i += 3) {
        float x[3], y[3];
        for (int k = 0; k < 3; k++) {
            const V& q = tri[i + k];
            float iw = q.w != 0 ? 1.0f / q.w : 0;
            x[k] = g_viewport[0] + (q.x * iw * 0.5f + 0.5f) * g_viewport[2];
            y[k] = g_viewport[1] + (q.y * iw * 0.5f + 0.5f) * g_viewport[3];
        }
        float y0 = std::min({y[0], y[1], y[2]}), y1 = std::max({y[0], y[1], y[2]});
        miny = std::min(miny, y0); maxy = std::max(maxy, y1);
        work += std::fabs((x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0])) * 0.5f;
    }
    int ry0 = std::max(0, (int)std::floor(miny)), ry1 = std::min(t.h, (int)std::ceil(maxy) + 1);
    if (ry0 >= ry1) return;
    auto raster = [&](int band, int nbands) {
        int rows = ry1 - ry0;
        int b0 = ry0 + rows * band / nbands, b1 = ry0 + rows * (band + 1) / nbands;
        for (size_t i = 0; i + 2 < tri.size(); i += 3) triangle(*p, t, tri[i], tri[i + 1], tri[i + 2], b0, b1);
    };
    if (work < 16384 || ry1 - ry0 < 32) raster(0, 1);   // small draw: threads would cost more than they save
    else g_pool.run(raster);
}
}  // namespace

// ================================ public API ======================================================
void set_render_threads(int n) { g_thread_override = n; }
void set_default_framebuffer_size(int w, int h) {
    if (w == g_def_w && h == g_def_h) return;
    g_def_w = w; g_def_h = h;
    g_def_color.assign((size_t)w * h, 0xFF000000u); g_def_stencil.assign((size_t)w * h, 0); g_def_depth.assign((size_t)w * h, 1.0f);
    if (!g_viewport[2]) { g_viewport[2] = w; g_viewport[3] = h; g_scissor_box[2] = w; g_scissor_box[3] = h; }
}
const uint32_t* default_framebuffer(int* w, int* h) { *w = g_def_w; *h = g_def_h; return g_def_color.data(); }

GLenum glGetError() { GLenum e = g_error; g_error = E_NO_ERROR; return e; }

// --- textures
void glGenTextures(GLsizei n, GLuint* out) { for (int i = 0; i < n; i++) { out[i] = new_name(); g_textures[out[i]] = std::make_unique<Texture>(); } }
void glDeleteTextures(GLsizei n, const GLuint* t) {
    for (int i = 0; i < n; i++) { g_textures.erase(t[i]); for (auto& b : g_bound_tex) if (b == t[i]) b = 0; }
}
GLboolean glIsTexture(GLuint t) { return g_textures.count(t) ? 1 : 0; }
void glActiveTexture(GLenum unit) { int u = (int)(unit - E_TEXTURE0); if (u < 0 || u >= 16) { set_error(E_INVALID_ENUM); return; } g_active_unit = u; }
void glBindTexture(GLenum target, GLuint tex) {
    if (target != E_TEXTURE_2D) { set_error(E_INVALID_ENUM); return; }
    if (tex && !g_textures.count(tex)) g_textures[tex] = std::make_unique<Texture>();
    g_bound_tex[g_active_unit] = tex;
}
void glTexImage2D(GLenum, GLint level, GLint, GLsizei w, GLsizei h, GLint, GLenum format, GLenum type, const void* pixels) {
    if (level != 0) return;   // no mipmaps: sampling uses level 0
    Texture* t = bound_texture(g_active_unit);
    if (!t) { set_error(E_INVALID_OPERATION); return; }
    t->w = w; t->h = h; t->px.assign((size_t)w * h, 0);
    upload(t, 0, 0, w, h, format, type, pixels);
}
void glTexSubImage2D(GLenum, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, const void* pixels) {
    if (level != 0) return;
    Texture* t = bound_texture(g_active_unit);
    if (!t || x < 0 || y < 0 || x + w > t->w || y + h > t->h) { set_error(E_INVALID_VALUE); return; }
    upload(t, x, y, w, h, format, type, pixels);
}
void glCompressedTexImage2D(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void*) {
    logf("swgl: compressed textures are not supported\n"); set_error(E_INVALID_ENUM);
}
void glCompressedTexSubImage2D(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLsizei, const void*) { set_error(E_INVALID_ENUM); }
void glCopyTexSubImage2D(GLenum, GLint level, GLint xo, GLint yo, GLint x, GLint y, GLsizei w, GLsizei h) {
    Texture* t = bound_texture(g_active_unit); Target src = target();
    if (level || !t || !src.color) return;
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++) {
            int sx = x + c, sy = y + r, dx = xo + c, dy = yo + r;
            if (sx < 0 || sy < 0 || sx >= src.w || sy >= src.h || dx < 0 || dy < 0 || dx >= t->w || dy >= t->h) continue;
            t->px[(size_t)dy * t->w + dx] = src.color[(size_t)sy * src.w + sx];
        }
}
void glCopyTexImage2D(GLenum target, GLint level, GLenum, GLint x, GLint y, GLsizei w, GLsizei h, GLint) {
    Texture* t = bound_texture(g_active_unit);
    if (level || !t) return;
    t->w = w; t->h = h; t->px.assign((size_t)w * h, 0);
    glCopyTexSubImage2D(target, 0, 0, 0, x, y, w, h);
}
void glTexParameteri(GLenum, GLenum pname, GLint v) {
    Texture* t = bound_texture(g_active_unit); if (!t) return;
    switch (pname) {
    case E_TEXTURE_MIN_FILTER: t->min_filter = (GLenum)v; break; case E_TEXTURE_MAG_FILTER: t->mag_filter = (GLenum)v; break;
    case E_TEXTURE_WRAP_S: t->wrap_s = (GLenum)v; break; case E_TEXTURE_WRAP_T: t->wrap_t = (GLenum)v; break;
    }
}
void glTexParameterf(GLenum target, GLenum pname, GLfloat v) { glTexParameteri(target, pname, (GLint)v); }
void glTexParameteriv(GLenum target, GLenum pname, const GLint* v) { glTexParameteri(target, pname, *v); }
void glTexParameterfv(GLenum target, GLenum pname, const GLfloat* v) { glTexParameteri(target, pname, (GLint)*v); }
void glGetTexParameteriv(GLenum, GLenum pname, GLint* out) {
    Texture* t = bound_texture(g_active_unit); if (!t) return;
    switch (pname) {
    case E_TEXTURE_MIN_FILTER: *out = (GLint)t->min_filter; break; case E_TEXTURE_MAG_FILTER: *out = (GLint)t->mag_filter; break;
    case E_TEXTURE_WRAP_S: *out = (GLint)t->wrap_s; break; case E_TEXTURE_WRAP_T: *out = (GLint)t->wrap_t; break;
    }
}
void glGetTexParameterfv(GLenum target, GLenum pname, GLfloat* out) { GLint i = 0; glGetTexParameteriv(target, pname, &i); *out = (GLfloat)i; }
void glGenerateMipmap(GLenum) {}   // sampling always uses level 0 (GD is 2D, drawn near 1:1)
void glPixelStorei(GLenum pname, GLint v) {
    if (pname == E_UNPACK_ALIGNMENT) g_unpack_align = v; else if (pname == E_PACK_ALIGNMENT) g_pack_align = v;
}

// --- buffers
void glGenBuffers(GLsizei n, GLuint* out) { for (int i = 0; i < n; i++) { out[i] = new_name(); g_buffers[out[i]] = std::make_unique<Buffer>(); } }
void glDeleteBuffers(GLsizei n, const GLuint* b) {
    for (int i = 0; i < n; i++) {
        g_buffers.erase(b[i]);
        if (g_array_buffer == b[i]) g_array_buffer = 0; if (g_element_buffer == b[i]) g_element_buffer = 0;
    }
}
GLboolean glIsBuffer(GLuint b) { return g_buffers.count(b) ? 1 : 0; }
void glBindBuffer(GLenum target, GLuint b) {
    if (b && !g_buffers.count(b)) g_buffers[b] = std::make_unique<Buffer>();
    if (target == E_ARRAY_BUFFER) g_array_buffer = b; else if (target == E_ELEMENT_ARRAY_BUFFER) g_element_buffer = b;
    else set_error(E_INVALID_ENUM);
}
Buffer* bound_buffer(GLenum target) {
    GLuint b = target == E_ARRAY_BUFFER ? g_array_buffer : target == E_ELEMENT_ARRAY_BUFFER ? g_element_buffer : 0;
    auto it = g_buffers.find(b); return it == g_buffers.end() ? nullptr : it->second.get();
}
void glBufferData(GLenum target, GLsizeiptr size, const void* data, GLenum) {
    Buffer* b = bound_buffer(target); if (!b) { set_error(E_INVALID_OPERATION); return; }
    b->data.assign((size_t)size, 0);
    if (data) memcpy(b->data.data(), data, (size_t)size);
}
void glBufferSubData(GLenum target, GLintptr off, GLsizeiptr size, const void* data) {
    Buffer* b = bound_buffer(target);
    if (!b || off < 0 || (size_t)(off + size) > b->data.size()) { set_error(E_INVALID_VALUE); return; }
    memcpy(b->data.data() + off, data, (size_t)size);
}
void glGetBufferParameteriv(GLenum target, GLenum pname, GLint* out) {
    Buffer* b = bound_buffer(target); if (!b) return;
    if (pname == 0x8764 /*BUFFER_SIZE*/) *out = (GLint)b->data.size(); else if (pname == 0x8765 /*BUFFER_USAGE*/) *out = 0x88E8;
}

// --- vertex attributes
void glVertexAttribPointer(GLuint i, GLint size, GLenum type, GLboolean norm, GLsizei stride, const void* ptr) {
    if (i >= 16) { set_error(E_INVALID_VALUE); return; }
    Attrib& a = g_attribs[i]; a.size = size; a.type = type; a.normalized = norm; a.stride = stride; a.ptr = ptr; a.buffer = g_array_buffer;
}
void glEnableVertexAttribArray(GLuint i) { if (i < 16) g_attribs[i].enabled = true; }
void glDisableVertexAttribArray(GLuint i) { if (i < 16) g_attribs[i].enabled = false; }
void glVertexAttrib4f(GLuint i, GLfloat x, GLfloat y, GLfloat z, GLfloat w) { if (i < 16) { float* c = g_attribs[i].cur; c[0] = x; c[1] = y; c[2] = z; c[3] = w; } }
void glVertexAttrib1f(GLuint i, GLfloat x) { glVertexAttrib4f(i, x, 0, 0, 1); }
void glVertexAttrib2f(GLuint i, GLfloat x, GLfloat y) { glVertexAttrib4f(i, x, y, 0, 1); }
void glVertexAttrib3f(GLuint i, GLfloat x, GLfloat y, GLfloat z) { glVertexAttrib4f(i, x, y, z, 1); }
void glVertexAttrib1fv(GLuint i, const GLfloat* v) { glVertexAttrib4f(i, v[0], 0, 0, 1); }
void glVertexAttrib2fv(GLuint i, const GLfloat* v) { glVertexAttrib4f(i, v[0], v[1], 0, 1); }
void glVertexAttrib3fv(GLuint i, const GLfloat* v) { glVertexAttrib4f(i, v[0], v[1], v[2], 1); }
void glVertexAttrib4fv(GLuint i, const GLfloat* v) { glVertexAttrib4f(i, v[0], v[1], v[2], v[3]); }
void glGetVertexAttribiv(GLuint i, GLenum pname, GLint* out) {
    if (i >= 16) return; Attrib& a = g_attribs[i];
    switch (pname) {
    case 0x8622: *out = a.enabled; break; case 0x8623: *out = a.size; break; case 0x8624: *out = a.stride; break;
    case 0x8625: *out = (GLint)a.type; break; case 0x886A: *out = a.normalized; break; case 0x889F: *out = (GLint)a.buffer; break;
    }
}
void glGetVertexAttribfv(GLuint i, GLenum pname, GLfloat* out) {
    if (i >= 16) return;
    if (pname == 0x8626 /*CURRENT_VERTEX_ATTRIB*/) memcpy(out, g_attribs[i].cur, 16);
    else { GLint v = 0; glGetVertexAttribiv(i, pname, &v); *out = (GLfloat)v; }
}
void glGetVertexAttribPointerv(GLuint i, GLenum, void** out) { if (i < 16) *out = (void*)g_attribs[i].ptr; }

// --- shaders & programs
GLuint glCreateShader(GLenum type) { GLuint n = new_name(); g_shaders[n] = std::make_unique<Shader>(Shader{type, {}}); return n; }
void glDeleteShader(GLuint s) { auto it = g_shaders.find(s); if (it != g_shaders.end()) it->second->deleted = true; }
GLboolean glIsShader(GLuint s) { return g_shaders.count(s) ? 1 : 0; }
void glShaderSource(GLuint s, GLsizei n, const GLchar* const* strs, const GLint* lens) {
    auto it = g_shaders.find(s); if (it == g_shaders.end()) { set_error(E_INVALID_VALUE); return; }
    std::string src;
    for (int i = 0; i < n; i++) src += lens && lens[i] >= 0 ? std::string(strs[i], lens[i]) : std::string(strs[i]);
    it->second->src = src;
}
void glCompileShader(GLuint) {}   // sources are matched at link time
void glGetShaderiv(GLuint s, GLenum pname, GLint* out) {
    auto it = g_shaders.find(s); if (it == g_shaders.end()) { set_error(E_INVALID_VALUE); return; }
    switch (pname) {
    case E_SHADER_TYPE: *out = (GLint)it->second->type; break; case E_COMPILE_STATUS: *out = 1; break;
    case E_INFO_LOG_LENGTH: *out = 0; break; case E_SHADER_SOURCE_LENGTH: *out = (GLint)it->second->src.size() + 1; break;
    case E_DELETE_STATUS: *out = it->second->deleted; break;
    }
}
void glGetShaderInfoLog(GLuint, GLsizei n, GLsizei* len, GLchar* log) { if (len) *len = 0; if (n > 0) log[0] = 0; }
void glGetShaderSource(GLuint s, GLsizei n, GLsizei* len, GLchar* out) {
    auto it = g_shaders.find(s); if (it == g_shaders.end() || n <= 0) return;
    int k = std::min<int>(n - 1, (int)it->second->src.size()); memcpy(out, it->second->src.data(), k); out[k] = 0; if (len) *len = k;
}
void glGetShaderPrecisionFormat(GLenum, GLenum, GLint* range, GLint* precision) { range[0] = 127; range[1] = 127; *precision = 23; }
void glReleaseShaderCompiler() {}
void glShaderBinary(GLsizei, const GLuint*, GLenum, const void*, GLsizei) { set_error(E_INVALID_ENUM); }
GLuint glCreateProgram() { GLuint n = new_name(); g_programs[n] = std::make_unique<Program>(); return n; }
void glDeleteProgram(GLuint p) { if (p != g_program) g_programs.erase(p); }
GLboolean glIsProgram(GLuint p) { return g_programs.count(p) ? 1 : 0; }
void glAttachShader(GLuint p, GLuint s) { auto it = g_programs.find(p); if (it != g_programs.end()) it->second->shaders.push_back(s); }
void glDetachShader(GLuint p, GLuint s) {
    auto it = g_programs.find(p); if (it == g_programs.end()) return;
    auto& v = it->second->shaders; v.erase(std::remove(v.begin(), v.end(), s), v.end());
}
void glBindAttribLocation(GLuint p, GLuint i, const GLchar* name) { auto it = g_programs.find(p); if (it != g_programs.end()) it->second->attrib_binding[name] = (int)i; }
static int uniform_location(Program& p, const std::string& name) {
    auto it = p.uniform_loc.find(name);
    if (it != p.uniform_loc.end()) return it->second;
    // Only uniforms that appear in the program's source exist.
    if (p.vsrc.find(name) == std::string::npos && p.fsrc.find(name) == std::string::npos) return -1;
    int loc = (int)p.uniforms.size();
    p.uniforms.push_back(Uniform{name});
    p.uniform_loc[name] = loc;
    return loc;
}
void glLinkProgram(GLuint pn) {
    auto it = g_programs.find(pn); if (it == g_programs.end()) { set_error(E_INVALID_VALUE); return; }
    Program& p = *it->second;
    p.vsrc.clear(); p.fsrc.clear();
    for (GLuint s : p.shaders) {
        auto sh = g_shaders.find(s); if (sh == g_shaders.end()) continue;
        (sh->second->type == E_VERTEX_SHADER ? p.vsrc : p.fsrc) += sh->second->src;
    }
    p.vs = classify_vertex(p.vsrc);
    p.fs = classify_fragment(p.fsrc);
    // attribute locations: explicit bindings first, then the next free index
    p.attrib_loc = p.attrib_binding;
    int next = 0;
    for (const char* a : {"a_position", "a_color", "a_texCoord", "a_texcoord"}) {
        if (p.vsrc.find(a) == std::string::npos || p.attrib_loc.count(a)) continue;
        std::set<int> used; for (auto& [n, i] : p.attrib_loc) used.insert(i);
        while (used.count(next)) next++;
        p.attrib_loc[a] = next++;
    }
    p.u_mvp = uniform_location(p, "CC_MVPMatrix");
    p.u_color = uniform_location(p, "u_color");
    p.u_alpha_value = uniform_location(p, "CC_alpha_value");
    p.u_tex0 = uniform_location(p, "CC_Texture0");
    p.u_texture = uniform_location(p, "u_texture");
    p.u_mask = uniform_location(p, "u_mask");
    p.linked = true;
    if (p.fs == FKind::Unknown) {
        uint64_t h = 1469598103934665603ull; for (unsigned char c : p.fsrc) h = (h ^ c) * 1099511628211ull;
        char key[32]; snprintf(key, sizeof key, "%016llx", (unsigned long long)h);
        if (g_warned.insert(key).second)
            logf("swgl: program %u uses an untranslated fragment shader %s (drawn as texture*color):\n%s\n", pn, key, p.fsrc.c_str());
    }
}
void glValidateProgram(GLuint) {}
void glUseProgram(GLuint p) { g_program = p; }
void glGetProgramiv(GLuint pn, GLenum pname, GLint* out) {
    auto it = g_programs.find(pn); if (it == g_programs.end()) { set_error(E_INVALID_VALUE); return; }
    Program& p = *it->second;
    switch (pname) {
    case E_LINK_STATUS: case E_VALIDATE_STATUS: *out = p.linked; break; case E_INFO_LOG_LENGTH: *out = 0; break;
    case E_ATTACHED_SHADERS: *out = (GLint)p.shaders.size(); break; case E_ACTIVE_UNIFORMS: *out = (GLint)p.uniforms.size(); break;
    case E_ACTIVE_ATTRIBUTES: *out = (GLint)p.attrib_loc.size(); break; case E_DELETE_STATUS: *out = 0; break;
    case E_ACTIVE_UNIFORM_MAX_LENGTH: case E_ACTIVE_ATTRIBUTE_MAX_LENGTH: *out = 64; break;
    }
}
void glGetProgramInfoLog(GLuint, GLsizei n, GLsizei* len, GLchar* log) { if (len) *len = 0; if (n > 0) log[0] = 0; }
void glGetAttachedShaders(GLuint pn, GLsizei max, GLsizei* count, GLuint* out) {
    auto it = g_programs.find(pn); int k = 0;
    if (it != g_programs.end()) for (GLuint s : it->second->shaders) if (k < max) out[k++] = s;
    if (count) *count = k;
}
GLint glGetAttribLocation(GLuint pn, const GLchar* name) {
    auto it = g_programs.find(pn); if (it == g_programs.end()) return -1;
    auto a = it->second->attrib_loc.find(name); return a == it->second->attrib_loc.end() ? -1 : a->second;
}
GLint glGetUniformLocation(GLuint pn, const GLchar* name) {
    auto it = g_programs.find(pn); if (it == g_programs.end()) { set_error(E_INVALID_VALUE); return -1; }
    return uniform_location(*it->second, name);
}
void glGetActiveAttrib(GLuint pn, GLuint index, GLsizei n, GLsizei* len, GLint* size, GLenum* type, GLchar* name) {
    auto it = g_programs.find(pn); if (it == g_programs.end()) return;
    GLuint i = 0;
    for (auto& [nm, loc] : it->second->attrib_loc) {
        if (i++ != index) continue;
        snprintf(name, (size_t)n, "%s", nm.c_str()); if (len) *len = (GLsizei)strlen(name);
        *size = 1; *type = 0x8B52; return;   // FLOAT_VEC4
    }
}
void glGetActiveUniform(GLuint pn, GLuint index, GLsizei n, GLsizei* len, GLint* size, GLenum* type, GLchar* name) {
    auto it = g_programs.find(pn); if (it == g_programs.end() || index >= it->second->uniforms.size()) return;
    snprintf(name, (size_t)n, "%s", it->second->uniforms[index].name.c_str()); if (len) *len = (GLsizei)strlen(name);
    *size = 1; *type = 0x8B5C;   // FLOAT_MAT4 (callers only use the name)
}
static Uniform* uniform(GLint loc) {
    Program* p = current_program();
    if (!p || loc < 0 || loc >= (int)p->uniforms.size()) return nullptr;
    return &p->uniforms[loc];
}
void glUniform1f(GLint l, GLfloat a) { if (auto* u = uniform(l)) { u->v[0] = a; u->iv = (int)a; } }
void glUniform2f(GLint l, GLfloat a, GLfloat b) { if (auto* u = uniform(l)) { u->v[0] = a; u->v[1] = b; } }
void glUniform3f(GLint l, GLfloat a, GLfloat b, GLfloat c) { if (auto* u = uniform(l)) { u->v[0] = a; u->v[1] = b; u->v[2] = c; } }
void glUniform4f(GLint l, GLfloat a, GLfloat b, GLfloat c, GLfloat d) { if (auto* u = uniform(l)) { u->v[0] = a; u->v[1] = b; u->v[2] = c; u->v[3] = d; } }
void glUniform1i(GLint l, GLint a) { if (auto* u = uniform(l)) { u->iv = a; u->v[0] = (float)a; } }
void glUniform2i(GLint l, GLint a, GLint b) { glUniform2f(l, (float)a, (float)b); }
void glUniform3i(GLint l, GLint a, GLint b, GLint c) { glUniform3f(l, (float)a, (float)b, (float)c); }
void glUniform4i(GLint l, GLint a, GLint b, GLint c, GLint d) { glUniform4f(l, (float)a, (float)b, (float)c, (float)d); }
void glUniform1fv(GLint l, GLsizei, const GLfloat* v) { glUniform1f(l, v[0]); }
void glUniform2fv(GLint l, GLsizei, const GLfloat* v) { glUniform2f(l, v[0], v[1]); }
void glUniform3fv(GLint l, GLsizei, const GLfloat* v) { glUniform3f(l, v[0], v[1], v[2]); }
void glUniform4fv(GLint l, GLsizei, const GLfloat* v) { glUniform4f(l, v[0], v[1], v[2], v[3]); }
void glUniform1iv(GLint l, GLsizei, const GLint* v) { glUniform1i(l, v[0]); }
void glUniform2iv(GLint l, GLsizei, const GLint* v) { glUniform2i(l, v[0], v[1]); }
void glUniform3iv(GLint l, GLsizei, const GLint* v) { glUniform3i(l, v[0], v[1], v[2]); }
void glUniform4iv(GLint l, GLsizei, const GLint* v) { glUniform4i(l, v[0], v[1], v[2], v[3]); }
static void set_matrix(GLint l, int n, bool transpose, const GLfloat* v) {
    Uniform* u = uniform(l); if (!u) return;
    for (int c = 0; c < n; c++) for (int r = 0; r < n; r++) u->v[c * 4 + r] = transpose ? v[r * n + c] : v[c * n + r];
}
void glUniformMatrix2fv(GLint l, GLsizei, GLboolean t, const GLfloat* v) { set_matrix(l, 2, t, v); }
void glUniformMatrix3fv(GLint l, GLsizei, GLboolean t, const GLfloat* v) { set_matrix(l, 3, t, v); }
void glUniformMatrix4fv(GLint l, GLsizei, GLboolean t, const GLfloat* v) {
    Uniform* u = uniform(l); if (!u) return;
    if (!t) memcpy(u->v, v, 64); else for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) u->v[c * 4 + r] = v[r * 4 + c];
}
void glGetUniformfv(GLuint pn, GLint l, GLfloat* out) {
    auto it = g_programs.find(pn); if (it == g_programs.end() || l < 0 || l >= (int)it->second->uniforms.size()) return;
    memcpy(out, it->second->uniforms[l].v, 16);
}
void glGetUniformiv(GLuint pn, GLint l, GLint* out) {
    auto it = g_programs.find(pn); if (it == g_programs.end() || l < 0 || l >= (int)it->second->uniforms.size()) return;
    *out = it->second->uniforms[l].iv;
}

// --- framebuffers / renderbuffers
void glGenFramebuffers(GLsizei n, GLuint* out) { for (int i = 0; i < n; i++) { out[i] = new_name(); g_framebuffers[out[i]] = std::make_unique<Framebuffer>(); } }
void glDeleteFramebuffers(GLsizei n, const GLuint* f) { for (int i = 0; i < n; i++) { g_framebuffers.erase(f[i]); if (g_bound_fb == f[i]) g_bound_fb = 0; } }
GLboolean glIsFramebuffer(GLuint f) { return g_framebuffers.count(f) ? 1 : 0; }
void glBindFramebuffer(GLenum, GLuint f) { if (f && !g_framebuffers.count(f)) g_framebuffers[f] = std::make_unique<Framebuffer>(); g_bound_fb = f; }
void glGenRenderbuffers(GLsizei n, GLuint* out) { for (int i = 0; i < n; i++) { out[i] = new_name(); g_renderbuffers[out[i]] = std::make_unique<Renderbuffer>(); } }
void glDeleteRenderbuffers(GLsizei n, const GLuint* r) {
    for (int i = 0; i < n; i++) { g_renderbuffers.erase(r[i]); g_rb_stencil.erase(r[i]); g_rb_depth.erase(r[i]); g_rb_color.erase(r[i]); }
}
GLboolean glIsRenderbuffer(GLuint r) { return g_renderbuffers.count(r) ? 1 : 0; }
void glBindRenderbuffer(GLenum, GLuint r) { if (r && !g_renderbuffers.count(r)) g_renderbuffers[r] = std::make_unique<Renderbuffer>(); g_bound_rb = r; }
void glRenderbufferStorage(GLenum, GLenum fmt, GLsizei w, GLsizei h) {
    auto it = g_renderbuffers.find(g_bound_rb); if (it == g_renderbuffers.end()) { set_error(E_INVALID_OPERATION); return; }
    it->second->w = w; it->second->h = h; it->second->fmt = fmt;
    g_rb_stencil.erase(g_bound_rb); g_rb_depth.erase(g_bound_rb); g_rb_color.erase(g_bound_rb);
}
void glGetRenderbufferParameteriv(GLenum, GLenum pname, GLint* out) {
    auto it = g_renderbuffers.find(g_bound_rb); if (it == g_renderbuffers.end()) return;
    if (pname == 0x8D42) *out = it->second->w; else if (pname == 0x8D43) *out = it->second->h; else if (pname == 0x8D44) *out = (GLint)it->second->fmt;
}
void glFramebufferTexture2D(GLenum, GLenum attachment, GLenum, GLuint tex, GLint) {
    auto it = g_framebuffers.find(g_bound_fb); if (it == g_framebuffers.end()) { set_error(E_INVALID_OPERATION); return; }
    if (attachment == E_COLOR_ATTACHMENT0) { it->second->color_tex = tex; it->second->color_rb = 0; }
}
void glFramebufferRenderbuffer(GLenum, GLenum attachment, GLenum, GLuint rb) {
    auto it = g_framebuffers.find(g_bound_fb); if (it == g_framebuffers.end()) { set_error(E_INVALID_OPERATION); return; }
    Framebuffer& fb = *it->second;
    if (attachment == E_COLOR_ATTACHMENT0) { fb.color_rb = rb; fb.color_tex = 0; }
    else if (attachment == E_DEPTH_ATTACHMENT) fb.depth_rb = rb;
    else if (attachment == E_STENCIL_ATTACHMENT) fb.stencil_rb = rb;
    else if (attachment == 0x821A /*DEPTH_STENCIL_ATTACHMENT*/) { fb.depth_rb = rb; fb.stencil_rb = rb; }
}
GLenum glCheckFramebufferStatus(GLenum) {
    if (!g_bound_fb) return E_FRAMEBUFFER_COMPLETE;
    Target t = target();
    return t.color ? E_FRAMEBUFFER_COMPLETE : 0x8CD6;   // INCOMPLETE_ATTACHMENT
}
void glGetFramebufferAttachmentParameteriv(GLenum, GLenum attachment, GLenum pname, GLint* out) {
    auto it = g_framebuffers.find(g_bound_fb); if (it == g_framebuffers.end()) { *out = 0; return; }
    Framebuffer& fb = *it->second;
    if (pname == 0x8CD0 /*OBJECT_TYPE*/) *out = attachment == E_COLOR_ATTACHMENT0 ? (fb.color_tex ? (GLint)E_TEXTURE_2D : fb.color_rb ? (GLint)E_RENDERBUFFER : 0) : 0;
    else if (pname == 0x8CD1 /*OBJECT_NAME*/) *out = attachment == E_COLOR_ATTACHMENT0 ? (GLint)(fb.color_tex ? fb.color_tex : fb.color_rb) : 0;
}

// --- fixed-function state
void glEnable(GLenum cap) {
    switch (cap) {
    case E_BLEND: g_blend = true; break; case E_SCISSOR_TEST: g_scissor = true; break; case E_STENCIL_TEST: g_stencil = true; break;
    case E_DEPTH_TEST: g_depth = true; break; case E_CULL_FACE: g_cull = true; break; case E_DITHER: g_dither = true; break;
    }
}
void glDisable(GLenum cap) {
    switch (cap) {
    case E_BLEND: g_blend = false; break; case E_SCISSOR_TEST: g_scissor = false; break; case E_STENCIL_TEST: g_stencil = false; break;
    case E_DEPTH_TEST: g_depth = false; break; case E_CULL_FACE: g_cull = false; break; case E_DITHER: g_dither = false; break;
    }
}
GLboolean glIsEnabled(GLenum cap) {
    switch (cap) {
    case E_BLEND: return g_blend; case E_SCISSOR_TEST: return g_scissor; case E_STENCIL_TEST: return g_stencil;
    case E_DEPTH_TEST: return g_depth; case E_CULL_FACE: return g_cull; case E_DITHER: return g_dither;
    }
    return 0;
}
void glBlendFunc(GLenum s, GLenum d) { g_blend_src_rgb = g_blend_src_a = s; g_blend_dst_rgb = g_blend_dst_a = d; }
void glBlendFuncSeparate(GLenum sr, GLenum dr, GLenum sa, GLenum da) { g_blend_src_rgb = sr; g_blend_dst_rgb = dr; g_blend_src_a = sa; g_blend_dst_a = da; }
void glBlendEquation(GLenum m) { g_blend_eq_rgb = g_blend_eq_a = m; }
void glBlendEquationSeparate(GLenum r, GLenum a) { g_blend_eq_rgb = r; g_blend_eq_a = a; }
void glBlendColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { g_blend_color[0] = r; g_blend_color[1] = g; g_blend_color[2] = b; g_blend_color[3] = a; }
void glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) { g_color_mask[0] = r; g_color_mask[1] = g; g_color_mask[2] = b; g_color_mask[3] = a; }
void glDepthFunc(GLenum f) { g_depth_func = f; }
void glDepthMask(GLboolean f) { g_depth_mask = f; }
void glDepthRangef(GLfloat, GLfloat) {}
void glStencilFunc(GLenum f, GLint ref, GLuint mask) { g_stencil_func = f; g_stencil_ref = ref; g_stencil_vmask = mask; }
void glStencilFuncSeparate(GLenum, GLenum f, GLint ref, GLuint mask) { glStencilFunc(f, ref, mask); }
void glStencilMask(GLuint m) { g_stencil_wmask = m; }
void glStencilMaskSeparate(GLenum, GLuint m) { g_stencil_wmask = m; }
void glStencilOp(GLenum f, GLenum zf, GLenum zp) { g_sfail = f; g_dpfail = zf; g_dppass = zp; }
void glStencilOpSeparate(GLenum, GLenum f, GLenum zf, GLenum zp) { glStencilOp(f, zf, zp); }
void glCullFace(GLenum m) { g_cull_mode = m; }
void glFrontFace(GLenum m) { g_front_face = m; }
void glLineWidth(GLfloat w) { g_line_width = w; }
void glPolygonOffset(GLfloat, GLfloat) {}
void glSampleCoverage(GLfloat, GLboolean) {}
void glHint(GLenum, GLenum) {}
void glViewport(GLint x, GLint y, GLsizei w, GLsizei h) { g_viewport[0] = x; g_viewport[1] = y; g_viewport[2] = w; g_viewport[3] = h; }
void glScissor(GLint x, GLint y, GLsizei w, GLsizei h) { g_scissor_box[0] = x; g_scissor_box[1] = y; g_scissor_box[2] = w; g_scissor_box[3] = h; }
void glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { g_clear_color[0] = r; g_clear_color[1] = g; g_clear_color[2] = b; g_clear_color[3] = a; }
void glClearDepthf(GLfloat d) { g_clear_depth = d; }
void glClearStencil(GLint s) { g_clear_stencil = s; }
void glClear(GLbitfield mask) {
    static int calls = 0;
    if (getenv("SWGL_DEBUG") && ++calls % 30 == 0) logf("swgl: glClear #%d (fb %u)\n", calls, g_bound_fb);
    Target t = target(); if (!t.color) return;
    int x0 = 0, y0 = 0, x1 = t.w, y1 = t.h;
    if (g_scissor) { x0 = std::max(0, g_scissor_box[0]); y0 = std::max(0, g_scissor_box[1]); x1 = std::min(t.w, g_scissor_box[0] + g_scissor_box[2]); y1 = std::min(t.h, g_scissor_box[1] + g_scissor_box[3]); }
    uint32_t cc = 0, cmask = 0;
    for (int k = 0; k < 4; k++) {
        float v = std::min(std::max(g_clear_color[k], 0.f), 1.f);
        cc |= (uint32_t)(v * 255 + 0.5f) << (8 * k);
        if (g_color_mask[k]) cmask |= 0xFFu << (8 * k);
    }
    for (int y = y0; y < y1; y++) {
        size_t row = (size_t)y * t.w;
        if (mask & E_COLOR_BUFFER_BIT) for (int x = x0; x < x1; x++) t.color[row + x] = (t.color[row + x] & ~cmask) | (cc & cmask);
        if ((mask & E_STENCIL_BUFFER_BIT) && t.stencil) for (int x = x0; x < x1; x++) write_stencil(&t.stencil[row + x], (uint8_t)g_clear_stencil);
        if ((mask & E_DEPTH_BUFFER_BIT) && t.depth && g_depth_mask) for (int x = x0; x < x1; x++) t.depth[row + x] = g_clear_depth;
    }
}
void glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void* pixels) {
    Target t = target(); if (!t.color || format != E_RGBA || type != E_UNSIGNED_BYTE) { set_error(E_INVALID_OPERATION); return; }
    size_t row = (size_t)w * 4; row = (row + g_pack_align - 1) / g_pack_align * g_pack_align;
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++) {
            int sx = x + c, sy = y + r;
            uint32_t v = (sx >= 0 && sy >= 0 && sx < t.w && sy < t.h) ? t.color[(size_t)sy * t.w + sx] : 0;
            memcpy((uint8_t*)pixels + row * r + 4 * c, &v, 4);
        }
}
void glFinish() {}
void glFlush() {}

// --- draws
void glDrawArrays(GLenum mode, GLint first, GLsizei count) { draw(mode, count, [&](int i) { return first + i; }); }
void glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    const uint8_t* base = (const uint8_t*)indices;
    if (g_element_buffer) { Buffer* b = bound_buffer(E_ELEMENT_ARRAY_BUFFER); if (!b) return; base = b->data.data() + (uintptr_t)indices; }
    if (!base) return;
    if (type == E_UNSIGNED_SHORT) draw(mode, count, [&](int i) { return (int)((const uint16_t*)base)[i]; });
    else if (type == E_UNSIGNED_BYTE) draw(mode, count, [&](int i) { return (int)base[i]; });
    else if (type == E_UNSIGNED_INT) draw(mode, count, [&](int i) { return (int)((const uint32_t*)base)[i]; });
    else set_error(E_INVALID_ENUM);
}

// --- queries
void glGetIntegerv(GLenum pname, GLint* out) {
    switch (pname) {
    case 0x0D33: *out = 8192; return;                       // MAX_TEXTURE_SIZE
    case 0x84E8: *out = 8192; return;                       // MAX_RENDERBUFFER_SIZE
    case 0x0D3A: out[0] = out[1] = 8192; return;            // MAX_VIEWPORT_DIMS
    case 0x8869: *out = 16; return;                         // MAX_VERTEX_ATTRIBS
    case 0x8872: case 0x8B4D: case 0x8B4C: *out = 16; return;   // MAX_TEXTURE_IMAGE_UNITS, COMBINED, VERTEX
    case 0x8DFB: case 0x8DFD: *out = 256; return;           // MAX_VERTEX/FRAGMENT_UNIFORM_VECTORS
    case 0x8DFC: *out = 16; return;                         // MAX_VARYING_VECTORS
    case 0x84E2: *out = 16; return;                         // MAX_TEXTURE_UNITS (desktop)
    case 0x0BA2: memcpy(out, g_viewport, 16); return;       // VIEWPORT
    case 0x0C10: memcpy(out, g_scissor_box, 16); return;    // SCISSOR_BOX
    case 0x8CA6: *out = (GLint)g_bound_fb; return;          // FRAMEBUFFER_BINDING
    case 0x8CA7: *out = (GLint)g_bound_rb; return;          // RENDERBUFFER_BINDING
    case 0x8B8D: *out = (GLint)g_program; return;           // CURRENT_PROGRAM
    case 0x8069: *out = (GLint)g_bound_tex[g_active_unit]; return;   // TEXTURE_BINDING_2D
    case 0x84E0: *out = (GLint)(E_TEXTURE0 + g_active_unit); return; // ACTIVE_TEXTURE
    case 0x8894: *out = (GLint)g_array_buffer; return;      // ARRAY_BUFFER_BINDING
    case 0x8895: *out = (GLint)g_element_buffer; return;    // ELEMENT_ARRAY_BUFFER_BINDING
    case 0x0D52: case 0x0D53: case 0x0D54: case 0x0D55: *out = 8; return;   // RED/GREEN/BLUE/ALPHA_BITS
    case 0x0D56: *out = 24; return; case 0x0D57: *out = 8; return;           // DEPTH_BITS, STENCIL_BITS
    case E_UNPACK_ALIGNMENT: *out = g_unpack_align; return; case E_PACK_ALIGNMENT: *out = g_pack_align; return;
    case 0x0BE1: *out = (GLint)g_blend_src_rgb; return; case 0x0BE0: *out = (GLint)g_blend_dst_rgb; return;   // BLEND_SRC/DST
    case 0x80C9: *out = (GLint)g_blend_src_rgb; return; case 0x80C8: *out = (GLint)g_blend_dst_rgb; return;
    case 0x80CB: *out = (GLint)g_blend_src_a; return; case 0x80CA: *out = (GLint)g_blend_dst_a; return;
    case 0x0B92: *out = (GLint)g_stencil_func; return; case 0x0B97: *out = g_stencil_ref; return;
    case 0x0B93: *out = (GLint)g_stencil_vmask; return; case 0x0B98: *out = (GLint)g_stencil_wmask; return;
    case 0x0B94: *out = (GLint)g_sfail; return; case 0x0B95: *out = (GLint)g_dpfail; return; case 0x0B96: *out = (GLint)g_dppass; return;
    case 0x0B74: *out = (GLint)g_depth_func; return;
    case 0x86A2: *out = 0; return;                          // NUM_COMPRESSED_TEXTURE_FORMATS
    case 0x8B9A: *out = (GLint)E_UNSIGNED_BYTE; return; case 0x8B9B: *out = (GLint)E_RGBA; return;   // IMPLEMENTATION_COLOR_READ_*
    case 0x821B: *out = 2; return; case 0x821C: *out = 1; return;   // MAJOR/MINOR_VERSION (desktop 2.1)
    }
    *out = 0;
}
void glGetFloatv(GLenum pname, GLfloat* out) {
    switch (pname) {
    case 0x0C22: memcpy(out, g_clear_color, 16); return;   // COLOR_CLEAR_VALUE
    case 0x0B21: *out = g_line_width; return;              // LINE_WIDTH
    case 0x846E: case 0x846D: out[0] = 1; out[1] = 64; return;   // ALIASED_LINE_WIDTH_RANGE / POINT_SIZE_RANGE
    case 0x0B73: *out = g_clear_depth; return;
    }
    GLint v[4] = {0, 0, 0, 0}; glGetIntegerv(pname, v); out[0] = (GLfloat)v[0];
}
void glGetBooleanv(GLenum pname, GLboolean* out) {
    if (pname == 0x0C23) { for (int k = 0; k < 4; k++) out[k] = g_color_mask[k]; return; }   // COLOR_WRITEMASK
    if (pname == 0x0B72) { *out = g_depth_mask; return; }                                   // DEPTH_WRITEMASK
    GLint v = 0; glGetIntegerv(pname, &v); *out = v != 0;
}
const GLubyte* glGetString(GLenum name) {
    switch (name) {
    case 0x1F00: return (const GLubyte*)"gdps4";                                  // VENDOR
    case 0x1F01: return (const GLubyte*)"swgl (gdps4 software renderer)";         // RENDERER
    case 0x1F02: return (const GLubyte*)"2.1 swgl";                               // VERSION
    case 0x8B8C: return (const GLubyte*)"1.20";                                   // SHADING_LANGUAGE_VERSION
    case 0x1F03:                                                                   // EXTENSIONS
        return (const GLubyte*)"GL_ARB_framebuffer_object GL_EXT_framebuffer_object GL_ARB_vertex_buffer_object "
                               "GL_ARB_texture_non_power_of_two GL_ARB_shader_objects GL_ARB_vertex_shader "
                               "GL_ARB_fragment_shader GL_ARB_shading_language_100 GL_EXT_blend_func_separate "
                               "GL_EXT_blend_equation_separate GL_EXT_blend_minmax GL_ARB_multitexture";
    }
    return nullptr;
}

}  // namespace swgl
