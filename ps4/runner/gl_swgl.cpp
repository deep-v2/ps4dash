// opengl32.dll for the PS4 runner, on swgl (software GLES2) + sceVideoOut.
// Piglet (the PS4's GLES2) can't create a display for homebrew on FW 13.52 (see docs), so the game's
// GL calls run on swgl and every SwapBuffers copies the frame into a video-out buffer and flips.
// Implements the same interface as gl_piglet.cpp (gl_piglet.hpp).
#include "gl_piglet.hpp"
#include "../../swgl/swgl.hpp"
#include <orbis/VideoOut.h>
#include <orbis/libkernel.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <map>
#include <mutex>
#include <set>
#include <string>

#define MSABI __attribute__((ms_abi))
#include "../../swgl/gen/swgl_exports.inc"

namespace gdps4::gl {
namespace {
Hooks g_h;
// The game renders at 1280x720; video out scales it to the TV.
constexpr int kWidth = 1280, kHeight = 720;
void log(const char* fmt, ...) {
    char b[512]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    if (g_h.log) g_h.log(b);
}

// ---- video out ----------------------------------------------------------------------------------
int g_video = -1;
OrbisKernelEqueue g_flip_queue;
uint32_t* g_buffers[2];
int g_back = 0;
int64_t g_frame = 0;
bool init_video() {
    if (g_video >= 0) return true;
    g_video = sceVideoOutOpen(ORBIS_VIDEO_USER_MAIN, ORBIS_VIDEO_OUT_BUS_MAIN, 0, nullptr);
    if (g_video < 0) { log("gl: sceVideoOutOpen failed %#x\n", g_video); return false; }
    sceKernelCreateEqueue(&g_flip_queue, "gdps4 flip");
    sceVideoOutAddFlipEvent(g_flip_queue, g_video, nullptr);
    const size_t fb = (size_t)kWidth * kHeight * 4, align = 0x200000;
    size_t total = (fb * 2 + align - 1) / align * align;
    off_t off = 0; void* mem = nullptr;
    int rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), total, align, 3 /*WC_GARLIC*/, &off);
    if (rc < 0) { log("gl: sceKernelAllocateDirectMemory failed %#x\n", rc); return false; }
    rc = sceKernelMapDirectMemory(&mem, total, 0x33 /*CPU RW + GPU RW*/, 0, off, align);
    if (rc < 0) { log("gl: sceKernelMapDirectMemory failed %#x\n", rc); return false; }
    g_buffers[0] = (uint32_t*)mem; g_buffers[1] = (uint32_t*)((uint8_t*)mem + fb);
    memset(mem, 0, fb * 2);
    OrbisVideoOutBufferAttribute attr;
    sceVideoOutSetBufferAttribute(&attr, 0x80000000 /*A8R8G8B8 sRGB*/, 1 /*linear*/, 0, kWidth, kHeight, kWidth);
    rc = sceVideoOutRegisterBuffers(g_video, 0, (void* const*)g_buffers, 2, &attr);
    if (rc != 0) { log("gl: sceVideoOutRegisterBuffers failed %#x\n", rc); return false; }
    sceVideoOutSetFlipRate(g_video, 0);   // 60 Hz
    log("gl: video out %dx%d ready (software renderer swgl)\n", kWidth, kHeight);
    return true;
}
void wait_flip(int64_t frame) {   // wait until the flip of `frame` has happened (buffer free again)
    for (;;) {
        OrbisVideoOutFlipStatus st; memset(&st, 0, sizeof st);
        sceVideoOutGetFlipStatus(g_video, &st);
        if (st.flipArg >= frame) return;
        OrbisKernelEvent ev; int n = 0;
        if (sceKernelWaitEqueue(g_flip_queue, &ev, 1, &n, nullptr) != 0) return;
    }
}

// ---- WGL on swgl -----------------------------------------------------------------------------------
void* const kHGLRC = (void*)0x5961;
void* g_dc; bool g_current;
MSABI void* wglCreateContext(void* dc) {
    if (!init_video()) {
        if (g_h.fatal) g_h.fatal("could not open video out (see gl: lines above)");
        return nullptr;
    }
    swgl::set_log(g_h.log);
    swgl::set_render_threads(6);   // the PS4 gives games 6 (7 on later firmware) of its 8 cores
    swgl::set_default_framebuffer_size(kWidth, kHeight);
    g_dc = dc;
    return kHGLRC;
}
MSABI int wglDeleteContext(void*) { return 1; }
MSABI int wglMakeCurrent(void* dc, void* rc) {
    log("gl: wglMakeCurrent(%p, %p)\n", dc, rc); g_current = rc != nullptr; if (dc) g_dc = dc; return 1; }
MSABI void* wglGetCurrentContext() { return g_current ? kHGLRC : nullptr; }
MSABI void* wglGetCurrentDC() { return g_current ? g_dc : nullptr; }
MSABI int wglShareLists(void*, void*) { return 1; }
// cocos2d's setupVerticalSync calls wglGetExtensionsStringEXT without a NULL check.
MSABI const char* wglGetExtensionsStringEXT() { return "WGL_EXT_swap_control"; }
MSABI const char* wglGetExtensionsStringARB(void*) { return "WGL_EXT_swap_control"; }
int g_interval = 1;
MSABI int wglSwapIntervalEXT(int i) { g_interval = i; return 1; }
MSABI int wglGetSwapIntervalEXT() { return g_interval; }
MSABI void glClearDepth(double d) { swgl::glClearDepthf((float)d); }
MSABI void glDepthRange(double n, double f) { swgl::glDepthRangef((float)n, (float)f); }
MSABI void* wglGetProcAddress(const char* name);

const std::map<std::string, void*>& table() {
    static const std::map<std::string, void*> t = [] {
        std::map<std::string, void*> m;
        for (auto& e : kSwglExports) m[e.name] = e.fn;
        m["glClearDepth"] = (void*)&glClearDepth; m["glDepthRange"] = (void*)&glDepthRange;
        m["wglCreateContext"] = (void*)&wglCreateContext; m["wglDeleteContext"] = (void*)&wglDeleteContext;
        m["wglMakeCurrent"] = (void*)&wglMakeCurrent; m["wglGetCurrentContext"] = (void*)&wglGetCurrentContext;
        m["wglGetCurrentDC"] = (void*)&wglGetCurrentDC; m["wglShareLists"] = (void*)&wglShareLists;
        m["wglGetProcAddress"] = (void*)&wglGetProcAddress;
        m["wglGetExtensionsStringEXT"] = (void*)&wglGetExtensionsStringEXT;
        m["wglGetExtensionsStringARB"] = (void*)&wglGetExtensionsStringARB;
        m["wglSwapIntervalEXT"] = (void*)&wglSwapIntervalEXT; m["wglGetSwapIntervalEXT"] = (void*)&wglGetSwapIntervalEXT;
        return m;
    }();
    return t;
}
void* find(const std::string& n) {
    auto& t = table();
    if (auto it = t.find(n); it != t.end()) return it->second;
    for (const char* suf : {"EXT", "ARB", "OES"}) {
        size_t k = strlen(suf);
        if (n.size() > k && n.compare(n.size() - k, k, suf) == 0)
            if (auto it = t.find(n.substr(0, n.size() - k)); it != t.end()) return it->second;
    }
    return nullptr;
}
// Desktop-only GL entry points (GL 1.2-2.1 functions GLES2 lacks): glew requires every one to be non-null to
// report GL 2.0, but the game never calls them (verified on PC). One shared stand-in: if it ever is called,
// the run stops; the function's name is the last "gl: wglGetProcAddress" lookup of that name in the log.
MSABI void desktop_gl_called() {
    if (g_h.fatal) g_h.fatal("the game called a desktop-only GL function (not in OpenGL ES 2.0)");
    for (;;) sceKernelUsleep(1000000);
}
MSABI void* wglGetProcAddress(const char* name) {
    static std::set<std::string> seen;
    bool new_name = seen.insert(name).second;
    void* p = find(name);
    if (!p && strncmp(name, "gl", 2) == 0) p = (void*)&desktop_gl_called;
    static int seq = 0;
    ++seq;
    if (new_name)   // every lookup, numbered, logged before returning (crash hunt in glewInit)
        log("gl: #%d wglGetProcAddress(%s) -> %s\n", seq, name,
            !p ? "NULL" : p == (void*)&desktop_gl_called ? "desktop-only stand-in" : "swgl");
    return p;
}

// frame timing for the log
uint64_t g_last_report_us; int g_frames; uint64_t g_present_us;
uint64_t now_us() { return (uint64_t)sceKernelGetProcessTime(); }
}  // namespace

void init(const Hooks& h) { g_h = h; swgl::set_log(h.log); }
bool early_init() {
    OrbisKernelSwVersion v; memset(&v, 0, sizeof v); v.Size = sizeof v;
    if (sceKernelGetSystemSwVersion(&v) == 0) log("gl: firmware %s, renderer: swgl (software) + video out\n", v.VersionString);
    return true;
}
void* lookup(const std::string& sym) {
    if (sym.rfind("gl", 0) == 0 || sym.rfind("wgl", 0) == 0) {
        if (void* p = find(sym)) return p;
        if (sym.rfind("gl", 0) == 0) return wglGetProcAddress(sym.c_str());
    }
    return nullptr;
}
bool swap_buffers() {
    static int n = 0; bool trace = n++ < 3;
    if (trace) log("gl: SwapBuffers #%d begin\n", n);
    if (g_video < 0) return false;
    uint64_t t0 = now_us();
    int w, h; const uint32_t* src = swgl::default_framebuffer(&w, &h);
    if (!src || w != kWidth || h != kHeight) return false;
    // Wait until the buffer we're about to overwrite has been shown (it was flipped two frames ago).
    if (g_frame >= 2) wait_flip(g_frame - 1);
    uint32_t* dst = g_buffers[g_back];
    for (int y = 0; y < h; y++) {   // GL rows are bottom-up; RGBA -> BGRA
        const uint32_t* s = src + (size_t)(h - 1 - y) * w;
        uint32_t* d = dst + (size_t)y * w;
        for (int x = 0; x < w; x++) { uint32_t c = s[x]; d[x] = (c & 0xFF00FF00u) | ((c & 0xFF) << 16) | ((c >> 16) & 0xFF); }
    }
    sceVideoOutSubmitFlip(g_video, g_back, ORBIS_VIDEO_OUT_FLIP_VSYNC, ++g_frame);
    if (trace) log("gl: SwapBuffers #%d flipped\n", n);
    g_back ^= 1;
    uint64_t t1 = now_us();
    g_present_us += t1 - t0; g_frames++;
    if (!g_last_report_us) g_last_report_us = t1;
    if (t1 - g_last_report_us >= 5000000) {
        log("gl: %.1f fps (present %.1f ms/frame)\n", g_frames * 1e6 / (double)(t1 - g_last_report_us), g_present_us / 1000.0 / g_frames);
        g_frames = 0; g_present_us = 0; g_last_report_us = t1;
    }
    return true;
}
int width() { return kWidth; }
int height() { return kHeight; }

}  // namespace gdps4::gl
