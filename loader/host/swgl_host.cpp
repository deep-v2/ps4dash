// PC host: run the game's OpenGL on swgl (the PS4 software renderer) and show its frames in the
// real window. Lets the renderer be validated against the real game before any console run.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "swgl_host.hpp"
#include "../../swgl/swgl.hpp"
#include <windows.h>
#include <chrono>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#define MSABI __attribute__((ms_abi))
#include "../../swgl/gen/swgl_exports.inc"

namespace swgl_host {
namespace {
void (*g_log)(const char*);
void log(const char* fmt, ...) {
    char b[512]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    if (g_log) g_log(b);
}
HDC g_dc; bool g_current;
void fit_to_window(HDC dc) {
    RECT r{}; HWND w = WindowFromDC(dc);
    if (w && GetClientRect(w, &r) && r.right > 0 && r.bottom > 0) swgl::set_default_framebuffer_size(r.right, r.bottom);
}

// --- WGL on swgl
MSABI HGLRC wglCreateContext(HDC dc) { g_dc = dc; fit_to_window(dc); log("swgl: context created\n"); return (HGLRC)0x5961; }
MSABI BOOL wglDeleteContext(HGLRC) { return TRUE; }
MSABI BOOL wglMakeCurrent(HDC dc, HGLRC rc) { g_current = rc != nullptr; if (dc) g_dc = dc; return TRUE; }
MSABI HGLRC wglGetCurrentContext() { return g_current ? (HGLRC)0x5961 : nullptr; }
MSABI HDC wglGetCurrentDC() { return g_current ? g_dc : nullptr; }
MSABI BOOL wglShareLists(HGLRC, HGLRC) { return TRUE; }
MSABI void glClearDepth(double d) { swgl::glClearDepthf((float)d); }
MSABI void glDepthRange(double n, double f) { swgl::glDepthRangef((float)n, (float)f); }
MSABI void* wglGetProcAddress(const char* name);
// cocos2d's setupVerticalSync calls wglGetExtensionsStringEXT without a NULL check.
MSABI const char* wglGetExtensionsStringEXT() { return "WGL_EXT_swap_control"; }
MSABI const char* wglGetExtensionsStringARB(HDC) { return "WGL_EXT_swap_control"; }
int g_swap_interval = 1;
MSABI BOOL wglSwapIntervalEXT(int i) { g_swap_interval = i; return TRUE; }
MSABI int wglGetSwapIntervalEXT() { return g_swap_interval; }

// --- presentation + frame timing
std::vector<uint32_t> g_bgra;
auto g_last_report = std::chrono::steady_clock::now();
int g_frames; double g_swap_ms, g_frame_ms;
auto g_last_swap = std::chrono::steady_clock::now();
MSABI BOOL SwapBuffers(HDC dc) {
    auto t0 = std::chrono::steady_clock::now();
    static bool first = true;
    int w, h; const uint32_t* px = swgl::default_framebuffer(&w, &h);
    if (first) { first = false; log("swgl: first SwapBuffers, framebuffer %dx%d\n", w, h); }
    static int calls = 0;
    if (getenv("SWGL_DEBUG")) log("swgl: SwapBuffers call %d\n", calls + 1);
    static auto window_start = std::chrono::steady_clock::now();
    static double render_ms_acc = 0;   // time between the previous present's end and this call = game + swgl rendering
    static auto prev_end = std::chrono::steady_clock::now();
    render_ms_acc += std::chrono::duration<double, std::milli>(t0 - prev_end).count();
    if (++calls % 20 == 0) {
        double span = std::chrono::duration<double, std::milli>(t0 - window_start).count();
        log("swgl: frames %d-%d: %.1f fps, avg render %.1f ms/frame at %dx%d\n", calls - 19, calls, 20000.0 / span,
            render_ms_acc / 20, w, h);
        window_start = t0; render_ms_acc = 0;
    }
    if (!px || !w || !h) return TRUE;
    g_bgra.resize((size_t)w * h);
    for (size_t i = 0; i < g_bgra.size(); i++) {   // RGBA -> BGRA (GDI)
        uint32_t c = px[i];
        g_bgra[i] = (c & 0xFF00FF00u) | ((c & 0xFF) << 16) | ((c >> 16) & 0xFF);
    }
    BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = h;   // bottom-up, like GL
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    RECT r{}; GetClientRect(WindowFromDC(dc), &r);
    StretchDIBits(dc, 0, 0, r.right, r.bottom, 0, 0, w, h, g_bgra.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
    if (r.right > 0 && r.bottom > 0 && (r.right != w || r.bottom != h)) swgl::set_default_framebuffer_size(r.right, r.bottom);
    auto t1 = std::chrono::steady_clock::now();
    g_swap_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();
    g_frame_ms += std::chrono::duration<double, std::milli>(t1 - g_last_swap).count();
    g_last_swap = t1; g_frames++; prev_end = t1;
    if (t1 - g_last_report > std::chrono::seconds(2)) {
        log("swgl: %dx%d, %.1f fps, frame %.1f ms (present %.1f ms)\n", w, h, g_frames * 1000.0 / g_frame_ms,
            g_frame_ms / g_frames, g_swap_ms / g_frames);
        g_frames = 0; g_swap_ms = g_frame_ms = 0; g_last_report = t1;
    }
    return TRUE;
}

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
        m["wglSwapIntervalEXT"] = (void*)&wglSwapIntervalEXT;
        m["wglGetSwapIntervalEXT"] = (void*)&wglGetSwapIntervalEXT;
        return m;
    }();
    return t;
}
void* (*g_missing)(const std::string&);
MSABI void* wglGetProcAddress(const char* name) {
    std::string n = name;
    auto& t = table();
    if (auto it = t.find(n); it != t.end()) return it->second;
    for (const char* suf : {"EXT", "ARB", "OES"}) {
        size_t k = strlen(suf);
        if (n.size() > k && n.compare(n.size() - k, k, suf) == 0)
            if (auto it = t.find(n.substr(0, n.size() - k)); it != t.end()) return it->second;
    }
    if (n.rfind("gl", 0) == 0 && g_missing) return g_missing(n);   // desktop-only GL: stops with its name if called
    return nullptr;
}
}  // namespace

void init(void (*log_fn)(const char*), void* (*missing)(const std::string&)) {
    g_log = log_fn; g_missing = missing;
    swgl::set_log(log_fn);
}
void* lookup_gl(const std::string& sym) {
    if (sym.rfind("gl", 0) == 0 && sym != "glClearDepth" && sym != "glDepthRange" && !table().count(sym))
        return wglGetProcAddress(sym.c_str());
    auto it = table().find(sym);
    return it == table().end() ? nullptr : it->second;
}
void* swap_buffers_fn() { return (void*)&SwapBuffers; }

}  // namespace swgl_host
