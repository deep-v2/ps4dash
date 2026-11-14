// Piglet probe: the OpenOrbis Piglet sample's exact bring-up (modules, config, EGL calls, linked
// libraries), minus the texture. Answers one question on this firmware: does Piglet's EGL work
// for a homebrew app at all? If yes, the screen cycles red -> green -> blue for ~15 s.
// Log: /data/gdps4/piglet_probe.txt + notifications.
#include <stdint.h>
#include <orbis/Pigletv2VSH.h>
#include <orbis/Sysmodule.h>
#include <orbis/SystemService.h>
#include <orbis/libkernel.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <string>

static FILE* g_out;
static void logf(const char* fmt, ...) {
    char b[512]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    printf("%s", b);
    if (g_out) { fputs(b, g_out); fflush(g_out); }
}
static void notify(const char* m) {
    OrbisNotificationRequest r; memset(&r, 0, sizeof r);
    r.type = NotificationRequest; r.targetId = -1; strncpy(r.message, m, sizeof(r.message) - 1);
    sceKernelSendNotificationRequest(0, &r, sizeof r, 0);
}
static int finish(const char* m) { logf("%s\n", m); notify(m); if (g_out) fclose(g_out); for (;;) sceKernelUsleep(1000000); }

int main() {
    mkdir("/data/gdps4", 0777);
    g_out = fopen("/data/gdps4/piglet_probe.txt", "w");
    OrbisKernelSwVersion v; memset(&v, 0, sizeof v); v.Size = sizeof v;
    sceKernelGetSystemSwVersion(&v);
    logf("piglet probe, firmware %s\n", v.VersionString);

    // Same order as samples/piglet: setup() (system service + splash), loadModules(), createContext()
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SYSTEM_SERVICE);
    logf("hide splash -> %#x\n", sceSystemServiceHideSplashScreen());
    std::string prefix = std::string("/") + sceKernelGetFsSandboxRandomWord() + "/common/lib/";
    int ret = 0;
    int pig = sceKernelLoadStartModule((prefix + "libScePigletv2VSH.sprx").c_str(), 0, nullptr, 0, nullptr, &ret);
    logf("load libScePigletv2VSH -> %#x (start %d)\n", pig, ret);
    int pre = sceKernelLoadStartModule((prefix + "libScePrecompiledShaders.sprx").c_str(), 0, nullptr, 0, nullptr, &ret);
    logf("load libScePrecompiledShaders -> %#x (start %d)\n", pre, ret);

    OrbisPglConfig cfg; memset(&cfg, 0, sizeof cfg);
    cfg.size = sizeof cfg;
    cfg.flags = ORBIS_PGL_FLAGS_USE_COMPOSITE_EXT | ORBIS_PGL_FLAGS_USE_FLEXIBLE_MEMORY | 0x60;
    cfg.processOrder = 1;
    cfg.systemSharedMemorySize = 250 * 1024 * 1024;
    cfg.videoSharedMemorySize = 512 * 1024 * 1024;
    cfg.maxMappedFlexibleMemory = 170 * 1024 * 1024;
    cfg.drawCommandBufferSize = 1 * 1024 * 1024;
    cfg.lcueResourceBufferSize = 1 * 1024 * 1024;
    cfg.dbgPosCmd_0x40 = 1920; cfg.dbgPosCmd_0x44 = 1080; cfg.dbgPosCmd_0x48 = 0; cfg.dbgPosCmd_0x4C = 0;
    cfg.unk_0x5C = 2;
    logf("scePigletSetConfigurationVSH -> %d\n", scePigletSetConfigurationVSH(&cfg));
    OrbisPglConfig got; memset(&got, 0, sizeof got); got.size = sizeof got;
    if (scePigletGetConfigurationVSH(&got))
        logf("config read back: flags=%#x sys=%llu video=%llu flex=%llu\n", got.flags,
             (unsigned long long)got.systemSharedMemorySize, (unsigned long long)got.videoSharedMemorySize,
             (unsigned long long)got.maxMappedFlexibleMemory);
    else
        logf("scePigletGetConfigurationVSH failed\n");

    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    logf("eglGetDisplay -> %p (err %#x)\n", dpy, eglGetError());
    if (dpy == EGL_NO_DISPLAY) return finish("Piglet probe: eglGetDisplay FAILED (no display)");
    EGLint major = 0, minor = 0;
    if (!eglInitialize(dpy, &major, &minor)) { logf("eglInitialize err %#x\n", eglGetError()); return finish("Piglet probe: eglInitialize FAILED"); }
    logf("EGL %d.%d\n", major, minor);
    eglBindAPI(EGL_OPENGL_ES_API);
    eglSwapInterval(dpy, 0);
    const EGLint attribs[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 0,
                              EGL_STENCIL_SIZE, 0, EGL_SAMPLE_BUFFERS, 0, EGL_SAMPLES, 0, EGL_RENDERABLE_TYPE,
                              EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_NONE};
    EGLConfig config; EGLint n = 0;
    if (!eglChooseConfig(dpy, attribs, &config, 1, &n) || n < 1) return finish("Piglet probe: eglChooseConfig FAILED");
    static OrbisPglWindow win{0, 1920, 1080};
    const EGLint wattr[] = {EGL_RENDER_BUFFER, EGL_BACK_BUFFER, EGL_NONE};
    EGLSurface surf = eglCreateWindowSurface(dpy, config, &win, wattr);
    if (surf == EGL_NO_SURFACE) { logf("err %#x\n", eglGetError()); return finish("Piglet probe: eglCreateWindowSurface FAILED"); }
    const EGLint cattr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT, cattr);
    if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, surf, surf, ctx)) return finish("Piglet probe: context FAILED");
    logf("GL_VERSION %s / GL_RENDERER %s\n", (const char*)glGetString(GL_VERSION), (const char*)glGetString(GL_RENDERER));
    // Runtime shader compilation check: needed by cocos2d.
    GLuint sh = glCreateShader(GL_FRAGMENT_SHADER);
    const char* src = "precision mediump float; void main() { gl_FragColor = vec4(1.0); }";
    glShaderSource(sh, 1, &src, nullptr); glCompileShader(sh);
    GLint ok = 0; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    char info[256] = ""; glGetShaderInfoLog(sh, sizeof info, nullptr, info);
    logf("runtime shader compile: %s %s (glGetError %#x)\n", ok ? "OK" : "FAILED", info, glGetError());
    notify(ok ? "Piglet probe: GL works, shaders compile. Watch for red/green/blue." : "Piglet probe: GL works, shader compile FAILED. Watch for red/green/blue.");
    const float colors[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (int i = 0; i < 900; i++) {   // ~15 s at 60 Hz
        const float* c = colors[(i / 300) % 3];
        glClearColor(c[0], c[1], c[2], 1); glClear(GL_COLOR_BUFFER_BIT);
        eglSwapBuffers(dpy, surf);
    }
    return finish(ok ? "Piglet probe done: GL OK, shaders OK" : "Piglet probe done: GL OK, shaders FAILED");
}
