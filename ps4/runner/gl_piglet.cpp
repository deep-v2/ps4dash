// opengl32.dll (WGL + desktop GL 2.1 as cocos2d/GLFW/glew use it) on the PS4's Piglet (EGL 1.4 + GLES2).
#include "gl_piglet.hpp"
#include <orbis/Pigletv2VSH.h>
#include <orbis/libkernel.h>
#include <orbis/Sysmodule.h>
#include <orbis/SystemService.h>
#include <dirent.h>
#include <map>
#include <stdarg.h>
#include <stdio.h>
#include <mutex>
#include <string>
#include <string.h>
#include <vector>

#define MSABI __attribute__((ms_abi))

namespace gdps4::gl {
namespace {
Hooks g_h;
EGLDisplay g_display = EGL_NO_DISPLAY;
EGLSurface g_surface = EGL_NO_SURFACE;
EGLContext g_context = EGL_NO_CONTEXT;
bool g_current = false;
constexpr int kWidth = 1920, kHeight = 1080;

void log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void log(const char* fmt, ...) {
    char b[1024]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    if (g_h.log) g_h.log(b);
}

// ---- Piglet bring-up (first wglCreateContext) --------------------------------------------
size_t free_flexible() {
    size_t sz = 0;   // OpenOrbis declares this with a size_t parameter; the real one takes size_t*
    ((int32_t (*)(size_t*))&sceKernelAvailableFlexibleMemorySize)(&sz);
    return sz;
}
bool load_modules() {
    std::string sandbox = std::string("/") + sceKernelGetFsSandboxRandomWord();
    int start_ret = 0;
    int pig = sceKernelLoadStartModule((sandbox + "/common/lib/libScePigletv2VSH.sprx").c_str(), 0, nullptr, 0, nullptr, &start_ret);
    log("gl: load libScePigletv2VSH -> %#x\n", pig);
    if (pig < 0) return false;
    // Runtime GLSL compiler: cocos2d compiles its shaders from source at startup. Try the known locations.
    const std::string paths[] = {sandbox + "/common/lib/libSceShaccVSH.sprx", "/system/common/lib/libSceShaccVSH.sprx",
                                 sandbox + "/priv/lib/libSceShaccVSH.sprx", "/system/priv/lib/libSceShaccVSH.sprx",
                                 "/system_ex/common_ex/lib/libSceShaccVSH.sprx"};
    for (auto& p : paths) {
        int r = sceKernelLoadStartModule(p.c_str(), 0, nullptr, 0, nullptr, &start_ret);
        log("gl: load %s -> %#x\n", p.c_str(), r);
        if (r >= 0) break;
    }
    return true;
}
bool try_config(uint64_t sys_mb, uint64_t video_mb, uint64_t flex_mb) {
    OrbisPglConfig cfg; memset(&cfg, 0, sizeof cfg);
    cfg.size = sizeof cfg;
    cfg.flags = ORBIS_PGL_FLAGS_USE_COMPOSITE_EXT | ORBIS_PGL_FLAGS_USE_FLEXIBLE_MEMORY | 0x60;
    cfg.processOrder = 1;
    cfg.systemSharedMemorySize = sys_mb << 20;
    cfg.videoSharedMemorySize = video_mb << 20;
    cfg.maxMappedFlexibleMemory = flex_mb << 20;
    cfg.drawCommandBufferSize = 1 << 20;
    cfg.lcueResourceBufferSize = 1 << 20;
    cfg.dbgPosCmd_0x40 = kWidth; cfg.dbgPosCmd_0x44 = kHeight;
    cfg.unk_0x5C = 2;
    bool set = scePigletSetConfigurationVSH(&cfg);
    g_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major = 0, minor = 0;
    bool init = g_display != EGL_NO_DISPLAY && eglInitialize(g_display, &major, &minor);
    log("gl: piglet config sys=%lluMB video=%lluMB flex=%lluMB: set=%d display=%p init=%d err=%#x (free flexible %zuMB)\n",
        (unsigned long long)sys_mb, (unsigned long long)video_mb, (unsigned long long)flex_mb, set, g_display, init,
        eglGetError(), free_flexible() >> 20);
    if (!init) g_display = EGL_NO_DISPLAY;
    return init;
}
bool init_egl() {
    if (g_display != EGL_NO_DISPLAY) return true;
    log("gl: free flexible memory before Piglet: %zuMB\n", free_flexible() >> 20);
    if (!load_modules()) return false;
    // The sample's sizes suit a tiny demo; the game has already used flexible memory, so step down.
    const uint64_t cfgs[][3] = {{250, 512, 170}, {128, 256, 128}, {96, 192, 96}, {64, 128, 64}, {32, 64, 32}};
    bool ok = false;
    for (auto& c : cfgs) if ((ok = try_config(c[0], c[1], c[2]))) break;
    if (!ok) return false;
    eglBindAPI(EGL_OPENGL_ES_API);
    // GD needs a stencil buffer (cocos2d clipping nodes). Try depth+stencil, then fall back.
    const EGLint ds[][2] = {{24, 8}, {16, 8}, {0, 8}, {0, 0}};
    EGLConfig config = nullptr; EGLint n = 0;
    for (auto& d : ds) {
        const EGLint attribs[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                  EGL_DEPTH_SIZE, d[0], EGL_STENCIL_SIZE, d[1], EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                                  EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_NONE};
        if (eglChooseConfig(g_display, attribs, &config, 1, &n) && n > 0) { log("gl: EGL config depth=%d stencil=%d\n", d[0], d[1]); break; }
    }
    if (n < 1) { log("gl: no EGL config\n"); return false; }
    static OrbisPglWindow win{0, (khronos_uint32_t)kWidth, (khronos_uint32_t)kHeight};
    const EGLint surf_attribs[] = {EGL_RENDER_BUFFER, EGL_BACK_BUFFER, EGL_NONE};
    g_surface = eglCreateWindowSurface(g_display, config, &win, surf_attribs);
    if (g_surface == EGL_NO_SURFACE) { log("gl: eglCreateWindowSurface failed %#x\n", eglGetError()); return false; }
    const EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    g_context = eglCreateContext(g_display, config, EGL_NO_CONTEXT, ctx_attribs);
    if (g_context == EGL_NO_CONTEXT) { log("gl: eglCreateContext failed %#x\n", eglGetError()); return false; }
    log("gl: EGL ready, %dx%d\n", kWidth, kHeight);
    return true;
}

// ---- WGL ---------------------------------------------------------------------------------
void* const kHGLRC = (void*)0x6C0001;
void* g_cur_dc;
MSABI void* wglCreateContext(void*) {
    if (init_egl()) return kHGLRC;
    // Without a context cocos2d's GLFW error callback recurses until the stack overflows (CE-34878-0).
    if (g_h.fatal) g_h.fatal("could not create the OpenGL context (Piglet/EGL), see gl: lines above");
    return nullptr;
}
MSABI int wglDeleteContext(void*) { return 1; }
MSABI int wglMakeCurrent(void* dc, void* rc) {
    if (!rc) { eglMakeCurrent(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT); g_current = false; g_cur_dc = nullptr; return 1; }
    if (!eglMakeCurrent(g_display, g_surface, g_surface, g_context)) { log("gl: eglMakeCurrent failed %#x\n", eglGetError()); return 0; }
    if (!g_current) {
        eglSwapInterval(g_display, 1);
        log("gl: GL_VERSION=%s GL_RENDERER=%s\n", (const char*)::glGetString(GL_VERSION), (const char*)::glGetString(GL_RENDERER));
    }
    g_current = true; g_cur_dc = dc;
    return 1;
}
MSABI void* wglGetCurrentContext() { return g_current ? kHGLRC : nullptr; }
MSABI void* wglGetCurrentDC() { return g_cur_dc; }
MSABI int wglShareLists(void*, void*) { return 1; }

// ---- desktop-GL fixups ------------------------------------------------------------------------
// What the game sees: desktop GL 2.1 with the extensions cocos2d's code paths need, all of which
// GLES2 provides natively (FBOs, VBOs, NPOT, shaders, separate blend).
const char* kExtensions =
    "GL_ARB_framebuffer_object GL_EXT_framebuffer_object GL_ARB_vertex_buffer_object "
    "GL_ARB_texture_non_power_of_two GL_ARB_shader_objects GL_ARB_vertex_shader GL_ARB_fragment_shader "
    "GL_ARB_shading_language_100 GL_EXT_blend_func_separate GL_EXT_blend_equation_separate GL_EXT_blend_minmax "
    "GL_ARB_multitexture";
MSABI const GLubyte* f_glGetString(GLenum name) {
    switch (name) {
    case GL_VENDOR: return (const GLubyte*)"gdps4";
    case GL_RENDERER: return (const GLubyte*)"Piglet (PS4 OpenGL ES 2.0) via gdps4";
    case GL_VERSION: return (const GLubyte*)"2.1 gdps4";
    case GL_SHADING_LANGUAGE_VERSION: return (const GLubyte*)"1.20";
    case GL_EXTENSIONS: return (const GLubyte*)kExtensions;
    }
    return ::glGetString(name);
}
// Desktop-only capabilities: GLES2 rejects them; remember the state so glIsEnabled stays consistent.
bool desktop_only_cap(GLenum cap) {
    switch (cap) {
    case 0x0DE1: /*TEXTURE_2D*/ case 0x0B20: /*LINE_SMOOTH*/ case 0x8861: /*POINT_SPRITE*/
    case 0x8642: /*VERTEX_PROGRAM_POINT_SIZE*/ case 0x0BC0: /*ALPHA_TEST*/ case 0x809D: /*MULTISAMPLE*/
    case 0x0B10: /*POINT_SMOOTH*/ case 0x0B41: /*POLYGON_SMOOTH*/ case 0x0B50: /*LIGHTING*/ case 0x0B60: /*FOG*/
        return true;
    }
    return false;
}
std::map<GLenum, bool> g_fake_caps;
MSABI void f_glEnable(GLenum cap) { if (desktop_only_cap(cap)) g_fake_caps[cap] = true; else ::glEnable(cap); }
MSABI void f_glDisable(GLenum cap) { if (desktop_only_cap(cap)) g_fake_caps[cap] = false; else ::glDisable(cap); }
MSABI GLboolean f_glIsEnabled(GLenum cap) { return desktop_only_cap(cap) ? g_fake_caps[cap] : ::glIsEnabled(cap); }
MSABI void f_glClearDepth(double d) { ::glClearDepthf((GLfloat)d); }
MSABI void f_glDepthRange(double n, double f) { ::glDepthRangef((GLfloat)n, (GLfloat)f); }
GLint gles_format(GLint f) {
    switch (f) {
    case 0x8058: case 4: case 0x8056: case 0x8057: return GL_RGBA;          // RGBA8, 4, RGBA4, RGB5_A1
    case 0x8051: case 3: case 0x8D62: return GL_RGB;                        // RGB8, 3, RGB565
    case 0x803C: return GL_ALPHA;                                           // ALPHA8
    case 0x8040: case 1: return GL_LUMINANCE;                               // LUMINANCE8, 1
    case 0x8045: case 2: return GL_LUMINANCE_ALPHA;                         // LUMINANCE8_ALPHA8, 2
    }
    return f;
}
MSABI void f_glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei w, GLsizei h, GLint border,
                          GLenum format, GLenum type, const void* pixels) {
    // GLES2: internalformat must equal format (no sized formats).
    ::glTexImage2D(target, level, (GLint)format, w, h, border, format, type, pixels);
    (void)internalformat;
}
// Desktop GLSL 1.10/1.20 -> GLSL ES 1.00: "#version 100" and a default float precision for
// fragment shaders (cocos2d's Windows build omits it; GLES2 requires it).
MSABI void f_glShaderSource(GLuint shader, GLsizei count, const GLchar* const* strs, const GLint* lens) {
    std::string src;
    for (GLsizei i = 0; i < count; i++) src += lens && lens[i] >= 0 ? std::string(strs[i], lens[i]) : std::string(strs[i]);
    std::string header = "#version 100\n";
    GLint type = 0; ::glGetShaderiv(shader, GL_SHADER_TYPE, &type);
    if (type == GL_FRAGMENT_SHADER)
        header += "#ifdef GL_FRAGMENT_PRECISION_HIGH\nprecision highp float;\n#else\nprecision mediump float;\n#endif\n";
    // Drop the shader's own #version line (desktop numbers are invalid in GLSL ES).
    size_t v = src.find("#version");
    if (v != std::string::npos) { size_t e = src.find('\n', v); src.erase(v, e == std::string::npos ? std::string::npos : e - v); }
    src = header + src;
    const GLchar* p = src.c_str();
    ::glShaderSource(shader, 1, &p, nullptr);
}
MSABI void f_glCompileShader(GLuint shader) {
    ::glCompileShader(shader);
    GLint ok = 0; ::glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char info[1024] = ""; ::glGetShaderInfoLog(shader, sizeof info, nullptr, info);
        char src[2048] = ""; ::glGetShaderSource(shader, sizeof src, nullptr, src);
        log("gl: shader %u failed to compile:\n%s\n--- source ---\n%s\n", shader, info, src);
    }
}
MSABI void f_glLinkProgram(GLuint program) {
    ::glLinkProgram(program);
    GLint ok = 0; ::glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) { char info[1024] = ""; ::glGetProgramInfoLog(program, sizeof info, nullptr, info); log("gl: program %u failed to link: %s\n", program, info); }
}

#include "gen/gl_wrappers.inc"

const std::map<std::string, void*>& table() {
    static const std::map<std::string, void*> t = [] {
        std::map<std::string, void*> m;
        for (auto& w : kGles2Wrappers) m[w.name] = w.fn;
        m["glGetString"] = (void*)&f_glGetString;
        m["glEnable"] = (void*)&f_glEnable;
        m["glDisable"] = (void*)&f_glDisable;
        m["glIsEnabled"] = (void*)&f_glIsEnabled;
        m["glTexImage2D"] = (void*)&f_glTexImage2D;
        m["glShaderSource"] = (void*)&f_glShaderSource;
        m["glCompileShader"] = (void*)&f_glCompileShader;
        m["glLinkProgram"] = (void*)&f_glLinkProgram;
        m["glClearDepth"] = (void*)&f_glClearDepth;
        m["glDepthRange"] = (void*)&f_glDepthRange;
        m["wglCreateContext"] = (void*)&wglCreateContext;
        m["wglDeleteContext"] = (void*)&wglDeleteContext;
        m["wglMakeCurrent"] = (void*)&wglMakeCurrent;
        m["wglGetCurrentContext"] = (void*)&wglGetCurrentContext;
        m["wglGetCurrentDC"] = (void*)&wglGetCurrentDC;
        m["wglShareLists"] = (void*)&wglShareLists;
        return m;
    }();
    return t;
}
void* find_gl(const std::string& name) {
    auto& t = table();
    if (auto it = t.find(name); it != t.end()) return it->second;
    // Extension aliases of core GLES2 functions: glGenFramebuffersEXT -> glGenFramebuffers, etc.
    for (const char* suf : {"EXT", "ARB", "OES"}) {
        size_t n = strlen(suf);
        if (name.size() > n && name.compare(name.size() - n, n, suf) == 0)
            if (auto it = t.find(name.substr(0, name.size() - n)); it != t.end()) return it->second;
    }
    return nullptr;
}
MSABI void* wglGetProcAddress(const char* name) {
    if (void* p = find_gl(name)) return p;
    if (strncmp(name, "gl", 2) == 0 && g_h.make_stub) {
        // Desktop-only GL entry point: glew requires every GL 1.2-2.1 pointer to be non-null to report
        // GL 2.0. The stub stops the run with the name if the game ever actually calls it.
        static std::mutex mx; static std::map<std::string, void*> stubs;
        std::lock_guard<std::mutex> l(mx);
        auto& s = stubs[name];
        if (!s) s = g_h.make_stub(std::string("opengl32.dll!") + name + " (desktop GL)");
        return s;
    }
    return nullptr;   // wgl* extensions: not offered (GLFW then uses the legacy pixel-format path)
}
}  // namespace

void init(const Hooks& h) { g_h = h; }

// Diagnostics + Piglet bring-up at process start, before any game code runs (and before this
// thread's gs base is changed): the same point where the OpenOrbis Piglet sample initializes.
bool early_init() {
    OrbisKernelSwVersion v; memset(&v, 0, sizeof v); v.Size = sizeof v;
    if (sceKernelGetSystemSwVersion(&v) == 0) log("gl: firmware %s (%#x)\n", v.VersionString, v.Version);
    std::string sandbox = std::string("/") + sceKernelGetFsSandboxRandomWord();
    for (std::string dir : {sandbox + "/common/lib", std::string("/system/common/lib"), std::string("/system/priv/lib")}) {
        DIR* d = opendir(dir.c_str());
        if (!d) { log("gl: cannot list %s\n", dir.c_str()); continue; }
        std::string hits;
        while (dirent* e = readdir(d)) {
            std::string n = e->d_name;
            for (const char* k : {"Pig", "Shacc", "shacc", "Shader", "Gnm", "Gl"})
                if (n.find(k) != std::string::npos) { hits += " " + n; break; }
        }
        closedir(d);
        log("gl: %s:%s\n", dir.c_str(), hits.empty() ? " (no GL/shader modules)" : hits.c_str());
    }
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SYSTEM_SERVICE);
    int r = sceSystemServiceHideSplashScreen();
    log("gl: hide splash -> %#x\n", r);
    bool ok = init_egl();
    log("gl: early Piglet init %s\n", ok ? "OK" : "FAILED");
    return ok;
}
void* lookup(const std::string& sym) {
    if (sym == "wglGetProcAddress") return (void*)&wglGetProcAddress;
    return find_gl(sym);
}
bool swap_buffers() {
    if (!g_current) return false;
    return eglSwapBuffers(g_display, g_surface);
}
int width() { return kWidth; }
int height() { return kHeight; }

}  // namespace gdps4::gl
