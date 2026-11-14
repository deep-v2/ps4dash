// gdps4 PS4 runner (M4): loads the original Geometry Dash binaries plus the Microsoft
// C/C++ runtimes from /data/gdps4/game, links them against the portable Win32 layer,
// runs every DllMain/TLS callback in Windows order and enters GeometryDash.exe.
// Anything not implemented yet (window/GL/audio/network) resolves to a stub that stops the
// run and reports its name: that is the next piece to build.
// Log: /data/gdps4/runner.log (+ on-screen notifications).
#include "../../loader/src/pe_loader.hpp"
#include "../../win32/win32.hpp"
#include "../../win32/platform.hpp"
#include <orbis/libkernel.h>
#include <orbis/Pad.h>
#include <orbis/UserService.h>
#include <dirent.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#define MSABI __attribute__((ms_abi))
using namespace gdps4;

static const char* kRoot = "/data/gdps4";
static const char* kGameDir = "/data/gdps4/game";
static FILE* g_log;
static std::mutex g_log_mx;

static void logf(const char* fmt, ...) {
    char b[1024]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    std::lock_guard<std::mutex> l(g_log_mx);
    printf("[gdps4] %s", b);
    if (g_log) { fputs(b, g_log); fflush(g_log); }
}
static void notify(const char* msg) {
    OrbisNotificationRequest req; memset(&req, 0, sizeof req);
    req.type = NotificationRequest; req.targetId = -1;
    strncpy(req.message, msg, sizeof(req.message) - 1);
    sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
}
[[noreturn]] static void halt(const char* why) {
    logf("HALT: %s\n", why);
    notify((std::string("gdps4 stopped:\n") + why).c_str());
    for (;;) sceKernelUsleep(1000000);
}
static std::string lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }

// ---- case-insensitive file lookup (Windows filenames are case-insensitive) --------------
static std::mutex g_dir_mx;
static std::map<std::string, std::vector<std::string>> g_dir_cache;
static std::string resolve_case(const std::string& path) {
    struct stat st;
    if (::stat(path.c_str(), &st) == 0) return path;
    std::string out;
    size_t i = 1;   // walk components after the leading '/'
    out = "";
    while (i <= path.size()) {
        size_t j = path.find('/', i); if (j == std::string::npos) j = path.size();
        std::string comp = path.substr(i, j - i), dir = out.empty() ? "/" : out;
        std::string cand = out + "/" + comp;
        if (::stat(cand.c_str(), &st) != 0) {
            std::lock_guard<std::mutex> l(g_dir_mx);
            auto it = g_dir_cache.find(dir);
            if (it == g_dir_cache.end()) it = g_dir_cache.emplace(dir, plat::list_dir(dir)).first;
            std::string lc = lower(comp);
            for (auto& n : it->second) if (lower(n) == lc) { cand = out + "/" + n; break; }
        }
        out = cand; i = j + 1;
    }
    return out;
}
// Windows path -> PS4 path.  C:\GD\save\... -> /data/gdps4/save/...,  C:\GD\... -> /data/gdps4/game/...
static std::string map_path(const std::string& win) {
    std::string w = win; for (auto& c : w) if (c == '\\') c = '/';
    std::string lw = lower(w), rest, base;
    if (lw.rfind("c:/gd/save", 0) == 0) { base = std::string(kRoot) + "/save"; rest = w.substr(10); }
    else if (lw.rfind("c:/gd", 0) == 0) { base = kGameDir; rest = w.substr(5); }
    else if (w.size() >= 2 && w[1] == ':') { base = std::string(kRoot) + "/drive_" + (char)tolower(w[0]); rest = w.substr(2); }
    else return w;
    return resolve_case(base + rest);
}

// ---- images ------------------------------------------------------------------------------
static const char* const kPeSet[] = {
    "geometrydash.exe", "libcocos2d.dll", "libextensions.dll",
    "ucrtbase.dll", "msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll",
    "zlib1.dll", "libpng16.dll", "jpeg62.dll", "tiff.dll", "pthreadvce3.dll", "sqlite3.dll",
    "glew32.dll", "liblzma.dll", "turbojpeg.dll"};
static bool in_pe_set(const std::string& n) { for (auto s : kPeSet) if (n == s) return true; return false; }
static std::map<std::string, LoadedImage> g_loaded;
static std::vector<std::string> g_init_order;
static PlatformHooks g_hooks;

static std::string canon_dll(std::string d) {
    d = lower(d);
    if (d.rfind("api-ms-win-crt-", 0) == 0) return "ucrtbase.dll";
    if (d.find('.') == std::string::npos) d += ".dll";
    return d;
}
static LoadedImage* image_containing(const void* a) {
    for (auto& [n, img] : g_loaded)
        if (img.base && (const uint8_t*)a >= img.base && (const uint8_t*)a < img.base + img.size) return &img;
    return nullptr;
}
static LoadedImage* image_by_base(const void* b) {
    for (auto& [n, img] : g_loaded) if (img.base == b) return &img;
    return nullptr;
}

// ---- stubs for not-yet-implemented imports -------------------------------------------------
static std::vector<std::string> g_stub_names;
static uint8_t* g_stub_mem; static size_t g_stub_used;
static void unimplemented(uint64_t id) {   // SysV; entered from an MS-ABI call site via the stub
    std::string n = id < g_stub_names.size() ? g_stub_names[id] : "?";
    halt(("game called unimplemented " + n).c_str());
}
static void* make_stub(const std::string& name) {
    if (!g_stub_mem || g_stub_used + 32 > 0x10000) {
        g_stub_mem = (uint8_t*)g_hooks.alloc_exec(0x10000); g_stub_used = 0;
    }
    uint64_t id = g_stub_names.size(); g_stub_names.push_back(name);
    uint8_t* p = g_stub_mem + g_stub_used; g_stub_used += 32;
    uint64_t fn = (uint64_t)&unimplemented;
    p[0] = 0x48; p[1] = 0x83; p[2] = 0xEC; p[3] = 0x08;            // sub rsp,8 (realign for the call)
    p[4] = 0x48; p[5] = 0xBF; memcpy(p + 6, &id, 8);               // movabs rdi,id
    p[14] = 0x48; p[15] = 0xB8; memcpy(p + 16, &fn, 8);            // movabs rax,unimplemented
    p[24] = 0xFF; p[25] = 0xD0;                                    // call rax
    p[26] = 0xCC;                                                  // int3 (never returns)
    return p;
}

// ---- small shims that aren't kernel32 ---------------------------------------------------------
MSABI static int32_t shim_SHGetFolderPathA(void*, int csidl, void*, uint32_t, char* out) {
    strcpy(out, "C:\\GD\\save");
    logf("SHGetFolderPathA(0x%x) -> C:\\GD\\save\n", csidl);
    return 0;
}
// Offline Steam (same behavior as the PC host; validated there in M2).
MSABI static uint64_t steam_ret0() { return 0; }
struct SteamDummy { void** vt; };
static SteamDummy* steam_dummy() {
    static void* vt[512]; static SteamDummy obj{vt};
    if (!vt[0]) for (auto& f : vt) f = (void*)&steam_ret0;
    return &obj;
}
MSABI static int steam_init_ok(void*) { return 0; }
MSABI static void* steam_context_init(void*) { static SteamDummy* ctx[64]; for (auto& p : ctx) p = steam_dummy(); return ctx; }
MSABI static void* steam_find_iface(void*, const char*) { return steam_dummy(); }
MSABI static uint64_t steam_user() { return 1; }
MSABI static void steam_void() {}

// Offline network (until a PS4 network backend exists): libcurl's API with every transfer
// failing as "couldn't connect", which GD already handles as a failed server request.
// libwebsockets: context creation fails (NULL), its documented error result.
enum { CURLE_OK = 0, CURLE_COULDNT_CONNECT = 7 };
MSABI static int curl_global_init(long) { return CURLE_OK; }
MSABI static void curl_global_cleanup() {}
MSABI static void* curl_easy_init() { static int warned; if (!warned++) logf("network: offline (curl requests fail)\n"); return calloc(1, 64); }
MSABI static int curl_easy_setopt(void*, int, void*) { return CURLE_OK; }
MSABI static int curl_easy_perform(void*) { return CURLE_COULDNT_CONNECT; }
MSABI static void curl_easy_reset(void*) {}
MSABI static void curl_easy_cleanup(void* h) { free(h); }
MSABI static int curl_easy_getinfo(void*, int info, void* out) {
    switch (info & 0xF00000) {   // CURLINFO type bits
    case 0x200000: *(int32_t*)out = 0; break;                     // LONG (32-bit on Windows): e.g. RESPONSE_CODE
    case 0x300000: *(double*)out = 0; break;                      // DOUBLE
    case 0x600000: *(int64_t*)out = 0; break;                     // OFF_T
    default: *(void**)out = nullptr; break;                       // STRING / SLIST / PTR / SOCKET
    }
    return CURLE_OK;
}
MSABI static void* lws_create_context(const void*) { logf("network: offline (websocket context not created)\n"); return nullptr; }
MSABI static void* lws_null() { return nullptr; }
MSABI static int lws_fail() { return -1; }
MSABI static void lws_void() {}
static void* net_lookup(const std::string& dll, const std::string& sym) {
    if (dll == "libcurl.dll") {
        static const std::map<std::string, void*> m = {
            {"curl_global_init", (void*)&curl_global_init}, {"curl_global_cleanup", (void*)&curl_global_cleanup},
            {"curl_easy_init", (void*)&curl_easy_init}, {"curl_easy_setopt", (void*)&curl_easy_setopt},
            {"curl_easy_perform", (void*)&curl_easy_perform}, {"curl_easy_reset", (void*)&curl_easy_reset},
            {"curl_easy_cleanup", (void*)&curl_easy_cleanup}, {"curl_easy_getinfo", (void*)&curl_easy_getinfo}};
        auto it = m.find(sym); return it == m.end() ? nullptr : it->second;
    }
    if (dll == "websockets.dll") {
        if (sym == "lws_create_context") return (void*)&lws_create_context;
        if (sym == "lws_service" || sym == "lws_write" || sym == "lws_callback_on_writable") return (void*)&lws_fail;
        if (sym == "lws_context_destroy") return (void*)&lws_void;
        return (void*)&lws_null;   // lws_get_context, lws_context_user, lws_canonical_hostname, lws_client_connect_via_info, lws_frame_is_binary
    }
    return nullptr;
}

// XInput on the DualShock 4 (scePad). GD reads controllers through XInputGetState (xinput1_4 #2);
// the DS4 is presented as XInput controller 0, with GD's own Xbox-controller mapping applying.
struct XINPUT_STATE { uint32_t packet; uint16_t buttons; uint8_t lt, rt; int16_t lx, ly, rx, ry; };
static int g_pad = -1;
static uint32_t g_pad_packet;
static void pad_open() {
    static bool tried = false;
    if (tried) return;
    tried = true;
    int32_t user = -1;
    sceUserServiceInitialize(nullptr);
    if (sceUserServiceGetInitialUser(&user) != 0) { logf("pad: no initial user\n"); return; }
    if (scePadInit() != 0) { logf("pad: scePadInit failed\n"); return; }
    g_pad = scePadOpen(user, 0, 0, nullptr);
    logf("pad: scePadOpen(user %d) -> %d\n", user, g_pad);
}
static int16_t stick_axis(uint8_t v, bool invert) {
    int s = ((int)v - 128) * 257;   // 0..255 -> about -32896..32639
    if (invert) s = -s - 1;
    return (int16_t)std::max(-32768, std::min(32767, s));
}
MSABI static uint32_t XInputGetState(uint32_t index, XINPUT_STATE* st) {
    if (index != 0) return 1167;   // ERROR_DEVICE_NOT_CONNECTED
    pad_open();
    OrbisPadData d; memset(&d, 0, sizeof d);
    if (g_pad < 0 || scePadReadState(g_pad, &d) != 0 || !d.connected) return 1167;
    uint32_t b = d.buttons; uint16_t x = 0;
    if (b & ORBIS_PAD_BUTTON_UP) x |= 0x0001;      if (b & ORBIS_PAD_BUTTON_DOWN) x |= 0x0002;
    if (b & ORBIS_PAD_BUTTON_LEFT) x |= 0x0004;    if (b & ORBIS_PAD_BUTTON_RIGHT) x |= 0x0008;
    if (b & ORBIS_PAD_BUTTON_OPTIONS) x |= 0x0010; if (b & ORBIS_PAD_BUTTON_TOUCH_PAD) x |= 0x0020;   // START, BACK
    if (b & ORBIS_PAD_BUTTON_L3) x |= 0x0040;      if (b & ORBIS_PAD_BUTTON_R3) x |= 0x0080;
    if (b & ORBIS_PAD_BUTTON_L1) x |= 0x0100;      if (b & ORBIS_PAD_BUTTON_R1) x |= 0x0200;
    if (b & ORBIS_PAD_BUTTON_CROSS) x |= 0x1000;   if (b & ORBIS_PAD_BUTTON_CIRCLE) x |= 0x2000;     // A, B
    if (b & ORBIS_PAD_BUTTON_SQUARE) x |= 0x4000;  if (b & ORBIS_PAD_BUTTON_TRIANGLE) x |= 0x8000;   // X, Y
    static uint16_t last; static uint32_t last_buttons;
    if (x != last || b != last_buttons) g_pad_packet++;
    last = x; last_buttons = b;
    st->packet = g_pad_packet; st->buttons = x;
    st->lt = d.analogButtons.l2; st->rt = d.analogButtons.r2;
    st->lx = stick_axis(d.leftStick.x, false); st->ly = stick_axis(d.leftStick.y, true);
    st->rx = stick_axis(d.rightStick.x, false); st->ry = stick_axis(d.rightStick.y, true);
    return 0;
}

// LoadLibrary/GetModuleHandle of DLLs the Win32 layer implements (kernel32, apisets, ...):
// a pseudo module whose GetProcAddress answers from the layer, like a real loaded DLL.
static std::map<std::string, std::unique_ptr<uint8_t[]>> g_pseudo;
static void* pseudo_module(const std::string& dll) {
    auto& p = g_pseudo[dll];
    if (!p) p.reset(new uint8_t[16]());
    return p.get();
}
static const std::string* pseudo_name(const void* base) {
    for (auto& [n, p] : g_pseudo) if (p.get() == base) return &n;
    return nullptr;
}

static bool load_image(const std::string& name);
static void* resolve(const std::string& dll, const std::string& sym);
static void* resolve_in_image(const std::string& name, const std::string& sym) {
    auto& img = g_loaded[name];
    std::string fwd = img.forwarder(sym);
    if (fwd.empty()) return img.find_export(sym);
    auto dot = fwd.find('.');
    return resolve(fwd.substr(0, dot) + ".dll", fwd.substr(dot + 1));
}
static void* resolve(const std::string& dll_in, const std::string& sym) {
    std::string dll = canon_dll(dll_in);
    if (in_pe_set(dll)) {
        if (!load_image(dll)) return make_stub(dll + "!" + sym);
        if (void* p = resolve_in_image(dll, sym)) return p;
        logf("MISSING EXPORT %s!%s\n", dll.c_str(), sym.c_str());
        return make_stub(dll + "!" + sym);
    }
    if (dll == "shell32.dll" && sym == "SHGetFolderPathA") return (void*)&shim_SHGetFolderPathA;
    if (void* p = net_lookup(dll, sym)) return p;
    if (dll == "xinput1_4.dll" && (sym == "#2" || sym == "XInputGetState")) return (void*)&XInputGetState;
    if (dll == "steam_api64.dll") {
        if (sym == "SteamInternal_SteamAPI_Init") return (void*)&steam_init_ok;
        if (sym == "SteamInternal_ContextInit") return (void*)&steam_context_init;
        if (sym == "SteamInternal_FindOrCreateUserInterface") return (void*)&steam_find_iface;
        if (sym == "SteamAPI_GetHSteamUser") return (void*)&steam_user;
        return (void*)&steam_void;
    }
    if (win32::owns_dll(dll)) {
        if (void* p = win32::lookup(dll, sym)) return p;
        logf("WIN32 MISSING %s!%s\n", dll.c_str(), sym.c_str());
    }
    return make_stub(dll + "!" + sym);   // user32/gdi32/opengl32/fmod/ws2_32/... : later milestones
}
static bool load_image(const std::string& name) {
    if (g_loaded.count(name)) return g_loaded[name].base != nullptr || true;
    std::string path = resolve_case(std::string(kGameDir) + "/" + name);
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> file{std::istreambuf_iterator<char>(f), {}};
    if (file.empty()) { logf("FAIL %s: not found at %s\n", name.c_str(), path.c_str()); return false; }
    g_loaded[name];   // placeholder breaks import cycles
    LoadedImage img;
    auto r = load_pe(file, name, g_hooks, resolve, img, true);
    if (!r.ok) { logf("FAIL %s: %s\n", name.c_str(), r.error.c_str()); g_loaded.erase(name); return false; }
    logf("LOADED %-20s base=%p size=%#zx tls=%d\n", name.c_str(), img.base, img.size, img.tls_index_va != 0);
    g_loaded[name] = std::move(img);
    g_init_order.push_back(name);
    return true;
}

// ---- static TLS (TEB+0x58 array; we own the TEB on PS4) -----------------------------------
struct TlsImg { LoadedImage* img; uint32_t index; };
static std::vector<TlsImg> g_tls;
static void build_thread_tls(void* teb) {
    if (!teb || g_tls.empty()) return;
    auto** arr = (void**)calloc(g_tls.size() + 1, sizeof(void*));
    for (auto& t : g_tls) {
        size_t raw = t.img->tls_raw_end - t.img->tls_raw_start;
        auto* block = (uint8_t*)calloc(1, raw + t.img->tls_zero_fill + 16);
        memcpy(block, (void*)t.img->tls_raw_start, raw);
        arr[t.index] = block;
    }
    *(void***)((uint8_t*)teb + 0x58) = arr;
}
using DllMainFn = int (MSABI*)(void*, uint32_t, void*);
using TlsCbFn = void (MSABI*)(void*, uint32_t, void*);
static void notify_images(uint32_t reason) {   // DLL_THREAD_ATTACH(2)/DETACH(3) for new threads
    for (auto& n : g_init_order) {
        auto& img = g_loaded[n];
        for (auto cb : img.tls_callbacks) ((TlsCbFn)cb)(img.base, reason, nullptr);
        if (n != "geometrydash.exe" && img.entry_rva) ((DllMainFn)(img.base + img.entry_rva))(img.base, reason, nullptr);
    }
}

// ---- crash reporting ------------------------------------------------------------------------
static void on_fault(int sig, siginfo_t* si, void* ctx) {
    // FreeBSD amd64 ucontext_t: sigset_t (16 bytes), then mcontext_t whose mc_rip is the 21st
    // 8-byte field (offset 160). OpenOrbis' headers describe Linux's layout instead.
    uint64_t rip = *(uint64_t*)((uint8_t*)ctx + 16 + 160);
    char where[128] = "outside game images";
    if (auto* img = image_containing((void*)rip))
        snprintf(where, sizeof where, "%s+%#llx", img->name.c_str(), (unsigned long long)(rip - (uint64_t)img->base));
    char b[256];
    snprintf(b, sizeof b, "signal %d at %s (rip=%#llx, addr=%p)", sig, where, (unsigned long long)rip, si->si_addr);
    halt(b);
}

int main() {
    mkdir(kRoot, 0777); mkdir((std::string(kRoot) + "/save").c_str(), 0777); mkdir((std::string(kRoot) + "/temp").c_str(), 0777);
    g_log = fopen((std::string(kRoot) + "/runner.log").c_str(), "w");
    logf("gdps4 runner (M4)\n");
    notify("gdps4: starting Geometry Dash...");
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.__sa_handler.__sa_sigaction = on_fault; sa.sa_flags = SA_SIGINFO;   // OpenOrbis' sa_sigaction macro is broken
    sigaction(SIGSEGV, &sa, nullptr); sigaction(SIGBUS, &sa, nullptr); sigaction(SIGILL, &sa, nullptr);
    sigaction(SIGFPE, &sa, nullptr);

    g_hooks.alloc_exec = [](size_t n) { return plat::vm_reserve(n, true, 7); };
    g_hooks.protect = [](void*, size_t, bool, bool, bool) { return true; };

    // 1. Load everything (recursively, dependencies first).
    if (!load_image("geometrydash.exe")) halt("could not load GeometryDash.exe (copy the staged files to /data/gdps4/game)");
    logf("loaded %zu images, %zu import stubs for later milestones\n", g_init_order.size(), g_stub_names.size());

    // 2. Win32 layer.
    win32::Hooks wh;
    wh.exe_base = g_loaded["geometrydash.exe"].base;
    wh.module_by_name = [](const std::string& n) -> void* {
        auto it = g_loaded.find(canon_dll(n));
        if (it != g_loaded.end()) return it->second.base;
        return win32::owns_dll(canon_dll(n)) ? pseudo_module(canon_dll(n)) : nullptr;
    };
    wh.load_library = [](const std::string& n) -> void* {
        std::string c = canon_dll(n);
        if (in_pe_set(c) && load_image(c)) return g_loaded[c].base;
        if (win32::owns_dll(c)) return pseudo_module(c);
        logf("LoadLibrary(%s): not available\n", n.c_str());
        return nullptr;
    };
    wh.module_containing = [](const void* a) -> void* { auto* i = image_containing(a); return i ? i->base : nullptr; };
    wh.module_path = [](void* b) -> std::string { auto* i = image_by_base(b); return i ? "C:\\GD\\" + i->name : ""; };
    wh.get_proc = [](void* b, const std::string& s) -> void* {
        if (auto* i = image_by_base(b)) return resolve_in_image(lower(i->name), s);
        if (auto* pn = pseudo_name(b)) return win32::lookup(*pn, s);
        return nullptr;
    };
    wh.thread_attach = [](void* teb) { build_thread_tls(teb); notify_images(2); };
    wh.thread_detach = [](void*) { notify_images(3); };
    wh.map_path = map_path;
    wh.command_line = u"\"C:\\GD\\GeometryDash.exe\"";
    wh.current_dir = u"C:\\GD";
    wh.temp_dir = u"C:\\GD\\save\\temp\\";
    wh.log = [](const char* m) { logf("W32: %s", m); };
    win32::init(wh);

    // 3. Main thread TEB + static TLS.
    void* teb = win32::attach_current_thread();
    if (!teb) halt("could not set the gs base for the main thread");
    uint32_t k = 0;
    for (auto& n : g_init_order) {
        auto& img = g_loaded[n];
        if (!img.tls_index_va) continue;
        *(uint32_t*)img.tls_index_va = k;
        g_tls.push_back({&img, k++});
    }
    build_thread_tls(teb);
    logf("TEB %p, %zu images with static TLS\n", teb, g_tls.size());

    // 4. DllMain(PROCESS_ATTACH) in dependency order.
    for (auto& n : g_init_order) {
        if (n == "geometrydash.exe") continue;
        auto& img = g_loaded[n];
        for (auto cb : img.tls_callbacks) ((TlsCbFn)cb)(img.base, 1, nullptr);
        if (!img.entry_rva) continue;
        int ok = ((DllMainFn)(img.base + img.entry_rva))(img.base, 1, nullptr);
        logf("DllMain %-20s -> %d\n", n.c_str(), ok);
        if (!ok) halt(("DllMain failed: " + n).c_str());
    }
    notify("gdps4: all DLLs initialized, entering GeometryDash.exe");

    // 5. The game.
    auto& exe = g_loaded["geometrydash.exe"];
    for (auto cb : exe.tls_callbacks) ((TlsCbFn)cb)(exe.base, 1, nullptr);
    logf("ENTER GeometryDash.exe\n");
    int rc = ((int (MSABI*)())(exe.base + exe.entry_rva))();
    char b[64]; snprintf(b, sizeof b, "GeometryDash.exe returned %d", rc);
    halt(b);
}
