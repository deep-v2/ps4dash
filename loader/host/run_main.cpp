// M2 host run: load the ORIGINAL GD images with our loader and actually execute them,
// forwarding every non-RobTop import to the real Windows/third-party DLL (pass-through).
// This validates loader, static TLS, SEH registration and entry sequencing on PC
// before each pass-through is replaced by a PS4 implementation.
// Usage: loader_run <gd_dir>      (vanilla: Geode's XInput proxy is bypassed)
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include "../src/pe_loader.hpp"
#include "../../win32/win32.hpp"
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <mutex>

using namespace gdps4;

static std::vector<uint8_t> read_file(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}
static std::string lower(std::string s) { std::transform(s.begin(), s.end(), s.begin(), ::tolower); return s; }

static std::map<std::string, LoadedImage> g_loaded;
static std::string g_dir;
static FILE* g_log;

static HMODULE load_real(const std::string& dll) {
    // Vanilla redirects: bypass Geode/mod proxies sitting in the game folder.
    if (dll == "xinput1_4.dll") return LoadLibraryA("C:\\Windows\\System32\\XInput1_4.dll");
    if (dll == "libcurl.dll") return LoadLibraryA((g_dir + "\\libcurl_orig.dll").c_str());
    HMODULE h = LoadLibraryExA((g_dir + "\\" + dll).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    return h ? h : LoadLibraryA(dll.c_str());
}

// First real shim: every shell-folder query (incl. CSIDL_LOCAL_APPDATA, where GD keeps
// CCGameManager.dat / CCLocalLevels.dat) goes to a sandbox, never the user's real saves.
// On PS4 this same function will return /data/gd.
static std::string g_sandbox, g_glfw_log, g_raw_log;
static HANDLE g_glfw_h = INVALID_HANDLE_VALUE, g_raw_h = INVALID_HANDLE_VALUE;   // opened in main()
static HRESULT WINAPI shim_SHGetFolderPathA(HWND, int csidl, HANDLE, DWORD, LPSTR out) {
    strcpy_s(out, MAX_PATH, g_sandbox.c_str());
    fprintf(g_log, "SHGetFolderPathA(csidl=0x%x) -> %s\n", csidl, out); fflush(g_log);
    return S_OK;
}

// Module identity shims: our mapped images are not known to Windows, so answer
// module queries ourselves (the game derives its save-folder name from its exe name).
// The game's exe identity is our host exe's REAL HMODULE: Win32 APIs (RegisterClassEx,
// DirectInput8Create, LoadIcon...) reject handles that aren't real loaded modules.
static HMODULE host_handle() { static HMODULE h = GetModuleHandleW(nullptr); return h; }
static LoadedImage* image_by_handle(HMODULE h) {
    if (h == host_handle()) return &g_loaded["geometrydash.exe"];
    for (auto& [n, img] : g_loaded) if ((HMODULE)img.base == h) return &img;
    return nullptr;
}
static LoadedImage* image_by_name(std::string n) {
    n = lower(n); if (auto p = n.find_last_of("\\/"); p != std::string::npos) n = n.substr(p + 1);
    if (n.find('.') == std::string::npos) n += ".dll";
    auto it = g_loaded.find(n); return it == g_loaded.end() ? nullptr : &it->second;
}
static std::string wide_to_utf8(LPCWSTR w) {
    char b[MAX_PATH * 3]; WideCharToMultiByte(CP_UTF8, 0, w, -1, b, sizeof b, 0, 0); return b;
}
static HMODULE exe_handle() { return host_handle(); }
static HMODULE WINAPI shim_GetModuleHandleA(LPCSTR n) {
    if (!n) return exe_handle();
    if (auto i = image_by_name(n)) return (HMODULE)i->base;
    return GetModuleHandleA(n);
}
static HMODULE WINAPI shim_GetModuleHandleW(LPCWSTR n) {
    if (!n) return exe_handle();
    if (auto i = image_by_name(wide_to_utf8(n))) return (HMODULE)i->base;
    return GetModuleHandleW(n);
}
static HMODULE WINAPI shim_LoadLibraryA(LPCSTR n) {
    if (auto i = image_by_name(n)) return (HMODULE)i->base;
    return LoadLibraryA(n);
}
static HMODULE WINAPI shim_LoadLibraryW(LPCWSTR n) {
    if (auto i = image_by_name(wide_to_utf8(n))) return (HMODULE)i->base;
    return LoadLibraryW(n);
}
static FARPROC WINAPI shim_GetProcAddress(HMODULE h, LPCSTR s) {
    if (auto i = image_by_handle(h))
        return (FARPROC)i->find_export((uintptr_t)s >> 16 ? std::string(s) : "#" + std::to_string((uintptr_t)s));
    return GetProcAddress(h, s);
}
static DWORD WINAPI shim_GetModuleFileNameA(HMODULE h, LPSTR out, DWORD n) {
    LoadedImage* i = h ? image_by_handle(h) : &g_loaded["geometrydash.exe"];
    if (!i) return GetModuleFileNameA(h, out, n);
    std::string p = g_dir + "\\" + i->name;
    lstrcpynA(out, p.c_str(), n); return (DWORD)std::min<size_t>(p.size(), n);
}
static DWORD WINAPI shim_GetModuleFileNameW(HMODULE h, LPWSTR out, DWORD n) {
    LoadedImage* i = h ? image_by_handle(h) : &g_loaded["geometrydash.exe"];
    if (!i) return GetModuleFileNameW(h, out, n);
    std::string p = g_dir + "\\" + i->name;
    int len = MultiByteToWideChar(CP_UTF8, 0, p.c_str(), -1, out, n);
    return len ? len - 1 : 0;
}
// GLFW gets its HINSTANCE via GetModuleHandleExW(FROM_ADDRESS, &glfwInit).
static LoadedImage* image_containing(const void* a) {
    for (auto& [n, img] : g_loaded)
        if ((uint8_t*)a >= img.base && (uint8_t*)a < img.base + img.size) return &img;
    return nullptr;
}
static BOOL WINAPI shim_GetModuleHandleExW(DWORD fl, LPCWSTR n, HMODULE* out) {
    LoadedImage* i = (fl & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) ? image_containing(n)
                   : !n ? &g_loaded["geometrydash.exe"] : image_by_name(wide_to_utf8(n));
    // FROM_ADDRESS (GLFW asking for its own HINSTANCE) and NULL get the real host handle.
    if (i) { *out = ((fl & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) || !n) ? host_handle() : (HMODULE)i->base; return TRUE; }
    return GetModuleHandleExW(fl, n, out);
}
static BOOL WINAPI shim_GetModuleHandleExA(DWORD fl, LPCSTR n, HMODULE* out) {
    LoadedImage* i = (fl & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) ? image_containing(n)
                   : !n ? &g_loaded["geometrydash.exe"] : image_by_name(n);
    // FROM_ADDRESS (GLFW asking for its own HINSTANCE) and NULL get the real host handle.
    if (i) { *out = ((fl & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) || !n) ? host_handle() : (HMODULE)i->base; return TRUE; }
    return GetModuleHandleExA(fl, n, out);
}

// Offline Steam (no Steam on PS4; the real steam_api64.dll is never loaded).
// GD refuses to start unless SteamAPI_Init succeeds, then calls virtual methods on
// Steam interfaces. Every interface is a dummy object whose methods all return 0.
static uint64_t steam_ret0() { return 0; }
struct SteamDummy { void** vt; };
static SteamDummy* steam_dummy() {
    static void* vt[512]; static SteamDummy obj{vt};
    if (!vt[0]) for (auto& f : vt) f = (void*)steam_ret0;
    return &obj;
}
static int steam_init_ok(void*) { return 0; }                     // ESteamAPIInitResult_OK
static void* steam_context_init(void*) {                           // context = table of interface ptrs
    static SteamDummy* ctx[64]; for (auto& p : ctx) p = steam_dummy(); return ctx;
}
static void* steam_find_iface(...) { return steam_dummy(); }
static uint64_t steam_user() { return 1; }
static void steam_void() {}

// File access trace (files.log): which game files are opened, in order, with ms timestamps.
static HANDLE g_files_h = INVALID_HANDLE_VALUE;
static void trace_file(const char* what, const char* path, bool ok) {
    char b[1200]; DWORD w;
    wsprintfA(b, "%8lu %-6s %s %s\r\n", GetTickCount(), what, ok ? "ok  " : "FAIL", path ? path : "(null)");
    WriteFile(g_files_h, b, lstrlenA(b), &w, nullptr);
}
// Must call ucrtbase's own functions: a FILE* from this exe's CRT is invalid in the game's CRT.
static FARPROC ucrt(const char* n) {
    // Same CRT as the game: our mapped ucrtbase when runtimes are PE-loaded (M4), else Windows' copy.
    if (auto it = g_loaded.find("ucrtbase.dll"); it != g_loaded.end() && it->second.base)
        return (FARPROC)it->second.find_export(n);
    static HMODULE h = LoadLibraryA("ucrtbase.dll"); return GetProcAddress(h, n);
}
static FILE* __cdecl shim_fopen(const char* p, const char* m) {
    static auto real = (FILE* (__cdecl*)(const char*, const char*))ucrt("fopen");
    FILE* f = real(p, m); trace_file("fopen", p, f); return f;
}
static errno_t __cdecl shim_fopen_s(FILE** o, const char* p, const char* m) {
    static auto real = (errno_t (__cdecl*)(FILE**, const char*, const char*))ucrt("fopen_s");
    errno_t e = real(o, p, m); trace_file("fopens", p, e == 0); return e;
}
static FILE* __cdecl shim_wfopen(const wchar_t* p, const wchar_t* m) {
    static auto real = (FILE* (__cdecl*)(const wchar_t*, const wchar_t*))ucrt("_wfopen");
    FILE* f = real(p, m); trace_file("wfopen", wide_to_utf8(p).c_str(), f); return f;
}
static DWORD WINAPI shim_GetFileAttributesA(LPCSTR p) {
    DWORD a = GetFileAttributesA(p); trace_file("attrib", p, a != INVALID_FILE_ATTRIBUTES); return a;
}

// GL frame probe: glClear runs once per frame; every 60th call log the current
// context and pending GL error (main thread, so this is the real render state).
static HANDLE g_gl_h = INVALID_HANDLE_VALUE;
static void WINAPI shim_glClear(unsigned mask) {
    static HMODULE gl = GetModuleHandleA("opengl32.dll");
    static auto real = (void (WINAPI*)(unsigned))GetProcAddress(gl, "glClear");
    static auto err = (unsigned (WINAPI*)())GetProcAddress(gl, "glGetError");
    static auto cur = (void* (WINAPI*)())GetProcAddress(gl, "wglGetCurrentContext");
    static auto dc = (void* (WINAPI*)())GetProcAddress(gl, "wglGetCurrentDC");
    static unsigned n = 0;
    if (n++ % 60 == 0) {
        char b[200]; DWORD w;
        wsprintfA(b, "%8lu glClear#%u tid=%lu ctx=%p dc=%p err_before=0x%x\r\n", GetTickCount(), n, GetCurrentThreadId(),
                  cur(), dc(), err());
        WriteFile(g_gl_h, b, lstrlenA(b), &w, nullptr);
    }
    real(mask);
}

// ---- PE image set (M4) -------------------------------------------------------
// Everything in this set is mapped by OUR loader, never by Windows: the game, the Microsoft
// C/C++ runtimes and the media/system libraries the game ships. Only the OS layer below them
// (kernel32, ntdll, user32, gdi32, opengl32, ...) passes through to Windows; that layer is
// exactly what the PS4 build has to implement. GDPS4_PASSTHROUGH_RUNTIMES=1 restores the M2 setup.
static std::string g_sys_dir = "C:\\Windows\\System32";
static std::map<std::string, std::string> pe_set() {
    std::map<std::string, std::string> m = {
        {"geometrydash.exe", g_dir}, {"libcocos2d.dll", g_dir}, {"libextensions.dll", g_dir}};
    if (getenv("GDPS4_PASSTHROUGH_RUNTIMES")) return m;
    for (auto n : {"ucrtbase.dll", "msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll"}) m[n] = g_sys_dir;
    for (auto n : {"zlib1.dll", "libpng16.dll", "jpeg62.dll", "tiff.dll", "pthreadvce3.dll", "sqlite3.dll",
                   "glew32.dll", "liblzma.dll", "turbojpeg.dll"})
        m[n] = g_dir;
    return m;
}
static std::string canon_dll(std::string d) {
    d = lower(d);
    if (d.rfind("api-ms-win-crt-", 0) == 0) return pe_set().count("ucrtbase.dll") ? "ucrtbase.dll" : d;
    if (d.find('.') == std::string::npos) d += ".dll";
    return d;
}
static std::vector<std::string> g_init_order;   // dependencies first
static void* resolve(const std::string& dll, const std::string& sym);
static PlatformHooks g_hooks;

static bool load_image(const std::string& name) {
    if (g_loaded.count(name)) return true;
    auto set = pe_set(); auto it = set.find(name);
    if (it == set.end()) return false;
    auto file = read_file(it->second + "\\" + name);
    if (file.empty()) { fprintf(g_log, "FAIL %s: cannot read %s\n", name.c_str(), it->second.c_str()); return false; }
    g_loaded[name];   // placeholder: breaks import cycles (find_export on it returns null)
    LoadedImage img;
    auto r = load_pe(file, name, g_hooks, resolve, img, true);
    if (!r.ok) { fprintf(g_log, "FAIL %s\n", r.error.c_str()); g_loaded.erase(name); return false; }
    for (auto& u : r.unresolved) fprintf(g_log, "UNRESOLVED %s (in %s)\n", u.c_str(), name.c_str());
    // Unwind tables so C++ exceptions/SEH work inside the mapped image.
    if (img.pdata_size)
        RtlAddFunctionTable((PRUNTIME_FUNCTION)(img.base + img.pdata_rva), img.pdata_size / 12, (DWORD64)img.base);
    fprintf(g_log, "LOADED %s at %p from %s\n", name.c_str(), (void*)img.base, it->second.c_str()); fflush(g_log);
    g_loaded[name] = std::move(img);
    g_init_order.push_back(name);
    return true;
}
static void* resolve_in_image(const std::string& name, const std::string& sym, int depth = 0) {
    auto& img = g_loaded[name];
    std::string fwd = img.forwarder(sym);
    if (fwd.empty()) return img.find_export(sym);
    if (depth > 8) return nullptr;
    auto dot = fwd.find('.');   // "VCRUNTIME140.memset" or "NTDLL.#12"
    return resolve(fwd.substr(0, dot) + ".dll", fwd.substr(dot + 1));
}

// Log every GL entry point the game looks up at run time (glew -> wglGetProcAddress): the GL
// surface the PS4 Piglet layer must provide. Only names that resolve on this PC are counted.
// GDPS4_GLTRACE=1: every returned pointer goes through a thunk that logs the first CALL
// ("GLCALL name") and jumps on, so we learn which GL functions the game really uses.
static std::vector<std::string> g_gl_names;
static std::vector<char> g_gl_called;
static std::mutex g_gl_mx;
static void WINAPI gl_called(uint32_t id) {
    std::lock_guard<std::mutex> l(g_gl_mx);
    if (!g_gl_called[id]) { g_gl_called[id] = 1; fprintf(g_log, "GLCALL %s\n", g_gl_names[id].c_str()); fflush(g_log); }
}
static void* gl_trace_thunk(const std::string& name, void* real) {
    static uint8_t* mem; static size_t used = 0x10000;
    if (used + 128 > 0x10000) { mem = (uint8_t*)VirtualAlloc(nullptr, 0x10000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE); used = 0; }
    uint32_t id; { std::lock_guard<std::mutex> l(g_gl_mx); id = (uint32_t)g_gl_names.size(); g_gl_names.push_back(name); g_gl_called.push_back(0); }
    uint8_t* p = mem + used; used += 128; uint8_t* q = p;
    auto put = [&](std::initializer_list<uint8_t> b) { for (auto x : b) *q++ = x; };
    put({0x51, 0x52, 0x41, 0x50, 0x41, 0x51});                         // push rcx, rdx, r8, r9
    put({0x48, 0x83, 0xEC, 0x68});                                     // sub rsp,0x68 (keeps 16-byte alignment)
    put({0xF3, 0x0F, 0x7F, 0x44, 0x24, 0x20, 0xF3, 0x0F, 0x7F, 0x4C, 0x24, 0x30,
         0xF3, 0x0F, 0x7F, 0x54, 0x24, 0x40, 0xF3, 0x0F, 0x7F, 0x5C, 0x24, 0x50});   // movdqu [rsp+..],xmm0-3
    put({0xB9}); memcpy(q, &id, 4); q += 4;                            // mov ecx,id
    uint64_t fn = (uint64_t)&gl_called; put({0x48, 0xB8}); memcpy(q, &fn, 8); q += 8; put({0xFF, 0xD0});   // call
    put({0xF3, 0x0F, 0x6F, 0x44, 0x24, 0x20, 0xF3, 0x0F, 0x6F, 0x4C, 0x24, 0x30,
         0xF3, 0x0F, 0x6F, 0x54, 0x24, 0x40, 0xF3, 0x0F, 0x6F, 0x5C, 0x24, 0x50});   // movdqu xmm0-3,[rsp+..]
    put({0x48, 0x83, 0xC4, 0x68, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59}); // add rsp; pop r9, r8, rdx, rcx
    uint64_t r = (uint64_t)real; put({0x48, 0xB8}); memcpy(q, &r, 8); q += 8; put({0xFF, 0xE0});   // jmp real
    return p;
}
static void* WINAPI shim_wglGetProcAddress(LPCSTR name) {
    static auto real = (PROC (WINAPI*)(LPCSTR))GetProcAddress(LoadLibraryA("opengl32.dll"), "wglGetProcAddress");
    PROC p = real(name);
    static std::set<std::string> seen;
    if (p && seen.insert(name).second) { fprintf(g_log, "WGLPROC %s\n", name); fflush(g_log); }
    if (p && getenv("GDPS4_GLTRACE")) return gl_trace_thunk(name, (void*)p);
    return (void*)p;
}

static void* resolve(const std::string& dll_in, const std::string& sym) {
    const std::string& dll = dll_in;
    if (dll == "opengl32.dll" && sym == "glClear") return (void*)shim_glClear;
    if (dll == "opengl32.dll" && sym == "wglGetProcAddress") return (void*)shim_wglGetProcAddress;
    {
        std::string c = canon_dll(dll_in);
        if (g_loaded.count(c) || load_image(c)) {
            // File-open tracing shims stay in front of the (mapped) CRT.
            if (c == "ucrtbase.dll") {
                if (sym == "fopen") return (void*)shim_fopen;
                if (sym == "fopen_s") return (void*)shim_fopen_s;
                if (sym == "_wfopen") return (void*)shim_wfopen;
            }
            return resolve_in_image(c, sym);
        }
    }
    if (dll == "api-ms-win-crt-stdio-l1-1-0.dll") {
        if (sym == "fopen") return (void*)shim_fopen;
        if (sym == "fopen_s") return (void*)shim_fopen_s;
        if (sym == "_wfopen") return (void*)shim_wfopen;
    }
    if (dll == "kernel32.dll" && sym == "GetFileAttributesA" && !getenv("GDPS4_WIN32")) return (void*)shim_GetFileAttributesA;
    if (dll == "steam_api64.dll" && !getenv("GDPS4_REAL_STEAM")) {   // diagnostic switch: pass through
        fprintf(g_log, "STEAM stub %s\n", sym.c_str());
        if (sym == "SteamInternal_SteamAPI_Init") return (void*)steam_init_ok;
        if (sym == "SteamInternal_ContextInit") return (void*)steam_context_init;
        if (sym == "SteamInternal_FindOrCreateUserInterface") return (void*)steam_find_iface;
        if (sym == "SteamAPI_GetHSteamUser") return (void*)steam_user;
        return (void*)steam_void;   // RegisterCallback, UnregisterCallback, RunCallbacks, Shutdown
    }
    if (dll == "shell32.dll" && sym == "SHGetFolderPathA") return (void*)shim_SHGetFolderPathA;
    if (dll == "kernel32.dll" && !getenv("GDPS4_WIN32")) {   // with the Win32 layer on, it answers these
        static const std::map<std::string, void*> k32 = {
            {"GetModuleHandleA", (void*)shim_GetModuleHandleA}, {"GetModuleHandleW", (void*)shim_GetModuleHandleW},
            {"GetModuleFileNameA", (void*)shim_GetModuleFileNameA}, {"GetModuleFileNameW", (void*)shim_GetModuleFileNameW},
            {"LoadLibraryA", (void*)shim_LoadLibraryA}, {"LoadLibraryW", (void*)shim_LoadLibraryW},
            {"GetProcAddress", (void*)shim_GetProcAddress},
            {"GetModuleHandleExW", (void*)shim_GetModuleHandleExW}, {"GetModuleHandleExA", (void*)shim_GetModuleHandleExA}};
        if (auto it = k32.find(sym); it != k32.end()) return it->second;
    }
    // M4: route the kernel layer to our portable Win32 implementation (the code the PS4 runs).
    // Exception dispatch stays with Windows on the PC host; on PS4 it's still a known gap.
    if (getenv("GDPS4_WIN32") && gdps4::win32::owns_dll(dll)) {
        static const char* const host_seh[] = {"RaiseException", "RtlUnwindEx", "RtlUnwind", "RtlVirtualUnwind",
                                               "RtlLookupFunctionEntry", "RtlPcToFileHeader", "RtlCaptureContext",
                                               "RtlCaptureStackBackTrace"};
        bool seh = std::find_if(std::begin(host_seh), std::end(host_seh), [&](const char* s) { return sym == s; }) != std::end(host_seh);
        if (!seh) {
            if (void* p = gdps4::win32::lookup(dll, sym)) return p;
            fprintf(g_log, "W32MISSING %s!%s\n", dll.c_str(), sym.c_str());
        }
    }
    static std::map<std::string, HMODULE> mods;
    auto& h = mods[dll];
    if (!h) h = load_real(dll);
    if (!h) return nullptr;
    fprintf(g_log, "PASSTHROUGH %s!%s\n", dll.c_str(), sym.c_str());   // = what the PS4 layer must implement
    LPCSTR name = sym[0] == '#' ? MAKEINTRESOURCEA(std::stoul(sym.substr(1))) : sym.c_str();
    void* real = (void*)GetProcAddress(h, name);
    // GDPS4_GLTRACE also traces first calls of the window/GL imports (user32, gdi32, opengl32).
    if (real && getenv("GDPS4_GLTRACE") && (dll == "user32.dll" || dll == "gdi32.dll" || dll == "opengl32.dll") &&
        sym != "glClear" && sym != "wglGetProcAddress")
        return gl_trace_thunk(dll + "!" + sym, real);
    return real;
}

// Static TLS for manually mapped images, via TLS donor DLLs (see tls_donor.cpp).
// ntdll owns every slot and per-thread block; we never touch TEB arrays ourselves.
struct TlsImage { LoadedImage* img; };
static std::vector<TlsImage> g_tls;
static bool g_tls_ready = false;
static std::string g_host_dir;

static void setup_static_tls() {
    int k = 0;
    for (auto& [n, img] : g_loaded) {
        if (!img.tls_index_va) continue;
        size_t raw = img.tls_raw_end - img.tls_raw_start;
        // Separate file per image so each copy is its own module with its own slot.
        std::string src = g_host_dir + "\\tls_donor.dll", dst = g_host_dir + "\\tls_donor_" + std::to_string(k++) + ".dll";
        if (!CopyFileA(src.c_str(), dst.c_str(), FALSE))
            fprintf(g_log, "TLS donor copy %s -> %s failed (%lu)\n", src.c_str(), dst.c_str(), GetLastError());
        HMODULE d = LoadLibraryA(dst.c_str());
        if (!d) { fprintf(g_log, "TLS donor load failed for %s (%lu)\n", n.c_str(), GetLastError()); continue; }
        auto slot = ((ULONG(*)())GetProcAddress(d, "gdps4_tls_slot"))();
        auto cur = ((void*(*)())GetProcAddress(d, "gdps4_tls_this_thread"))();
        // Overwrite the donor's in-memory template (ntdll copies it for every new thread).
        auto b = (uint8_t*)d;
        auto nt = (IMAGE_NT_HEADERS64*)(b + ((IMAGE_DOS_HEADER*)b)->e_lfanew);
        auto td = (IMAGE_TLS_DIRECTORY64*)(b + nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].VirtualAddress);
        size_t cap = td->EndAddressOfRawData - td->StartAddressOfRawData;
        if (raw > cap) { fprintf(g_log, "TLS template too big for donor: %zu\n", raw); continue; }
        DWORD oldp; VirtualProtect((void*)td->StartAddressOfRawData, cap, PAGE_READWRITE, &oldp);
        memset((void*)td->StartAddressOfRawData, 0, cap);
        memcpy((void*)td->StartAddressOfRawData, (void*)img.tls_raw_start, raw);
        VirtualProtect((void*)td->StartAddressOfRawData, cap, oldp, &oldp);
        // This thread's block, straight from the TEB slot ntdll filled in.
        void* blk = ((void**)*(void**)((uint8_t*)NtCurrentTeb() + 0x58))[slot];
        (void)cur; memset(blk, 0, cap); memcpy(blk, (void*)img.tls_raw_start, raw);
        *(uint32_t*)img.tls_index_va = slot;
        g_tls.push_back({&img});
        fprintf(g_log, "TLS %s -> donor slot=%lu raw=%zu index_va=%p zerofill=%u\n", n.c_str(), slot, raw,
                (void*)img.tls_index_va, img.tls_zero_fill);
        auto live = (uint8_t*)((void**)*(void**)((uint8_t*)NtCurrentTeb() + 0x58))[slot];
        fprintf(g_log, "    template:");
        for (size_t i = 0; i < std::min<size_t>(raw, 16); i++) fprintf(g_log, " %02x", ((uint8_t*)img.tls_raw_start)[i]);
        fprintf(g_log, "\n    live    :");
        for (size_t i = 0; i < std::min<size_t>(raw, 16); i++) fprintf(g_log, " %02x", live[i]);
        fprintf(g_log, "  (live==donor block: %d)\n", live == cur);
    }
    g_tls_ready = true;
}

// TLS callback of loader_run.exe itself: runs on every new thread before its start
// routine, so game threads see their TLS too; then forward THREAD_ATTACH/DETACH to
// the mapped images' own TLS callbacks.
static void NTAPI host_tls_callback(PVOID, DWORD reason, PVOID) {
    if (!g_tls_ready || (reason != DLL_THREAD_ATTACH && reason != DLL_THREAD_DETACH)) return;
    using TlsCb = void(NTAPI*)(PVOID, DWORD, PVOID);
    for (auto& t : g_tls) for (auto cb : t.img->tls_callbacks) ((TlsCb)cb)(t.img->base, reason, nullptr);
}
extern "C" {
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:host_tls_callback_ptr")
#pragma const_seg(".CRT$XLB")
extern const PIMAGE_TLS_CALLBACK host_tls_callback_ptr;
const PIMAGE_TLS_CALLBACK host_tls_callback_ptr = host_tls_callback;
#pragma const_seg()
}

static void describe(uint64_t a) {
    for (auto& [n, img] : g_loaded)
        if (a >= (uint64_t)img.base && a < (uint64_t)img.base + img.size) {
            fprintf(g_log, "%s+0x%llx", n.c_str(), (unsigned long long)(a - (uint64_t)img.base)); return;
        }
    HMODULE m; char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &m)) {
        GetModuleFileNameA(m, name, MAX_PATH);
        fprintf(g_log, "%s+0x%llx", strrchr(name, '\\') ? strrchr(name, '\\') + 1 : name, (unsigned long long)(a - (uint64_t)m));
    } else fprintf(g_log, "%p", (void*)a);
}
static LONG WINAPI crash_filter(EXCEPTION_POINTERS* e) {
    DWORD code = e->ExceptionRecord->ExceptionCode;
    if (code == 0xE06D7363 || code == 0x406D1388 || code == DBG_PRINTEXCEPTION_C || code == 0x4001000A)
        return EXCEPTION_CONTINUE_SEARCH;               // C++ throw / thread naming / debug print: not a crash
    {   // Raw first line: survives a broken CRT/TLS on this thread.
        HANDLE h = g_raw_h;
        char b[200]; DWORD wr;
        uint64_t a = (uint64_t)e->ExceptionRecord->ExceptionAddress; const char* in_img = "?"; uint64_t off = a;
        for (auto& [n, img] : g_loaded)
            if (a >= (uint64_t)img.base && a < (uint64_t)img.base + img.size) { in_img = n.c_str(); off = a - (uint64_t)img.base; }
        wsprintfA(b, "EXC 0x%08lx tid=%lu at %s+0x%I64x info=%I64x %I64x\r\n", code, GetCurrentThreadId(), in_img, off,
                  e->ExceptionRecord->NumberParameters > 0 ? e->ExceptionRecord->ExceptionInformation[0] : 0,
                  e->ExceptionRecord->NumberParameters > 1 ? e->ExceptionRecord->ExceptionInformation[1] : 0);
        WriteFile(h, b, lstrlenA(b), &wr, nullptr);
    }
    static volatile LONG in = 0;                         // no thread_local: TLS may be broken here
    if (InterlockedCompareExchange(&in, 1, 0)) return EXCEPTION_CONTINUE_SEARCH;
    fprintf(g_log, "CRASH code=0x%08lx at ", code); describe((uint64_t)e->ExceptionRecord->ExceptionAddress);
    // Real unwind using .pdata (ours are registered with RtlAddFunctionTable).
    fprintf(g_log, "\n  backtrace:\n");
    CONTEXT c = *e->ContextRecord;
    static uint64_t frames[200000]; int nf = 0;
    for (; nf < 200000 && c.Rip; nf++) {
        frames[nf] = c.Rip;
        DWORD64 ib; auto rf = RtlLookupFunctionEntry(c.Rip, &ib, nullptr);
        if (!rf) { c.Rip = *(DWORD64*)c.Rsp; c.Rsp += 8; continue; }   // leaf
        PVOID hd; DWORD64 ef;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, ib, c.Rip, rf, &c, &hd, &ef, nullptr);
    }
    fprintf(g_log, "   (%d frames)\n", nf);
    for (int f = 0; f < nf; f++) {
        if (f >= 8 && f < nf - 24) { if (f == 8) fprintf(g_log, "   ...\n"); continue; }
        fprintf(g_log, "   #%05d ", f); describe(frames[f]);
        DWORD64 ib; if (auto rf = RtlLookupFunctionEntry(frames[f], &ib, nullptr)) fprintf(g_log, "  (fn rva 0x%lx)", rf->BeginAddress);
        fprintf(g_log, "\n");
    }
    fflush(g_log);
    InterlockedExchange(&in, 0);
    return EXCEPTION_CONTINUE_SEARCH;
}

// Stall diagnostics: after a delay, suspend every other thread, unwind its stack
// and log it to stacks.log, then resume. Run once.
static std::string g_stacks_log;
static DWORD g_main_tid;
static DWORD WINAPI watchdog(LPVOID delay_ms) {
    Sleep((DWORD)(uintptr_t)delay_ms);
    FILE* f = fopen(g_stacks_log.c_str(), "w");
    if (!f) return 0;
    FILE* saved = g_log; g_log = f;   // describe() prints to g_log
    {   // Director state via exported getters (read-only).
        auto& cc = g_loaded["libcocos2d.dll"];
        auto shared = (void* (*)())cc.find_export("?sharedDirector@CCDirector@cocos2d@@SAPEAV12@XZ");
        auto frames = (unsigned (*)(void*))cc.find_export("?getTotalFrames@CCDirector@cocos2d@@QEAAIXZ");
        auto paused = (bool (*)(void*))cc.find_export("?isPaused@CCDirector@cocos2d@@QEAA_NXZ");
        auto dt = (float (*)(void*))cc.find_export("?getDeltaTime@CCDirector@cocos2d@@UEAAMXZ");
        auto interval = (double (*)(void*))cc.find_export("?getAnimationInterval@CCDirector@cocos2d@@QEAANXZ");
        auto sched = (void* (*)(void*))cc.find_export("?getScheduler@CCDirector@cocos2d@@UEAAPEAVCCScheduler@2@XZ");
        auto tscale = (float (*)(void*))cc.find_export("?getTimeScale@CCScheduler@cocos2d@@QEAAMXZ");
        auto running = (void* (*)(void*))cc.find_export("?getRunningScene@CCDirector@cocos2d@@QEAAPEAVCCScene@2@XZ");
        auto children = (void* (*)(void*))cc.find_export("?getChildren@CCNode@cocos2d@@UEAAPEAVCCArray@2@XZ");
        auto count = (unsigned (*)(void*))cc.find_export("?count@CCArray@cocos2d@@QEBAIXZ");
        auto at = (void* (*)(void*, unsigned))cc.find_export("?objectAtIndex@CCArray@cocos2d@@QEAAPEAVCCObject@2@I@Z");
        auto& gd = g_loaded["geometrydash.exe"];
        void* d = shared();
        int secs = getenv("GDPS4_WATCH_SECS") ? atoi(getenv("GDPS4_WATCH_SECS")) : 3;
        for (int i = 0; i < secs; i++) {
            fprintf(f, "DIRECTOR t=%ds frames=%u paused=%d dt=%f", i, frames(d), paused(d), dt(d));
            // LoadingLayer (child 0 of the running scene): m_loadStep at +0x19c (from loadAssets).
            void* sc = running(d); void* ch = sc ? children(sc) : nullptr;
            void* layer = ch && count(ch) ? at(ch, 0) : nullptr;
            // Identify by vtable: LoadingLayer's primary vtable lives in GeometryDash.exe.
            uint64_t vt = layer ? *(uint64_t*)layer : 0;
            bool in_gd = vt >= (uint64_t)gd.base && vt < (uint64_t)gd.base + gd.size;
            fprintf(f, " layer_vt=%s+0x%llx", in_gd ? "gd" : "?", (unsigned long long)(in_gd ? vt - (uint64_t)gd.base : vt));
            if (layer && in_gd) fprintf(f, " loadStep=%d", *(int*)((uint8_t*)layer + 0x19c));
            {   // CCApplication frame limiter (run(): interval = flag@0xc4 ? ticks@0x20 : ticks@0x18).
                static auto app_get = (uint8_t* (*)())cc.find_export("?sharedApplication@CCApplication@cocos2d@@SAPEAV12@XZ");
                uint8_t* a = app_get();
                LARGE_INTEGER now; QueryPerformanceCounter(&now);
                fprintf(f, " app: ticks18=%lld ticks20=%lld f_bc=%f f_c0=%f flag_c4=%d c7=%d c9=%d now=%lld",
                        *(long long*)(a + 0x18), *(long long*)(a + 0x20), *(float*)(a + 0xbc), *(float*)(a + 0xc0),
                        a[0xc4], a[0xc7], a[0xc9], now.QuadPart);
            }
            fprintf(f, " fg=%d\n", GetForegroundWindow() == FindWindowA(nullptr, "Geometry Dash")); fflush(f);
            if (getenv("GDPS4_FOCUS_AT") && i == atoi(getenv("GDPS4_FOCUS_AT"))) {   // test: give the game focus
                HWND h = FindWindowA(nullptr, "Geometry Dash");
                // Foreground-lock workaround: a synthetic Alt press lets this process take the foreground.
                keybd_event(VK_MENU, 0, 0, 0); keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
                BOOL ok = SetForegroundWindow(h);
                // Then a real click in the middle of the window (user-input activation).
                RECT r; GetWindowRect(h, &r);
                SetCursorPos((r.left + r.right) / 2, (r.top + r.bottom) / 2);
                mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0); mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
                fprintf(f, "FOCUS requested hwnd=%p ok=%d + click\n", (void*)h, ok);
            }
            Sleep(1000);
        }
        (void)interval; (void)tscale; (void)sched;
    }
    {   // Sampling profile of the main thread: 200 samples, 25 ms apart.
        // Each sample records the innermost frames that lie inside game images.
        HANDLE mt = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, g_main_tid);
        std::map<std::string, int> hist;
        for (int s = 0; s < 200 && mt; s++) {
            SuspendThread(mt);
            CONTEXT c{}; c.ContextFlags = CONTEXT_FULL;
            std::string key;
            if (GetThreadContext(mt, &c)) {
                int depth = 0;
                for (int i = 0; i < 30 && c.Rip && depth < 4; i++) {
                    for (auto& [n, img] : g_loaded)
                        if (c.Rip >= (uint64_t)img.base && c.Rip < (uint64_t)img.base + img.size) {
                            DWORD64 ib0; auto r0 = RtlLookupFunctionEntry(c.Rip, &ib0, nullptr);
                            char b[96]; sprintf(b, "%s+0x%lx < ", n.c_str(), r0 ? r0->BeginAddress : 0);
                            key += b; depth++;
                        }
                    DWORD64 ib; auto rf = RtlLookupFunctionEntry(c.Rip, &ib, nullptr);
                    if (!rf) { c.Rip = *(DWORD64*)c.Rsp; c.Rsp += 8; continue; }
                    PVOID hd; DWORD64 ef;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, ib, c.Rip, rf, &c, &hd, &ef, nullptr);
                }
            }
            ResumeThread(mt);
            hist[key]++;
            Sleep(25);
        }
        std::vector<std::pair<int, std::string>> v;
        for (auto& [k, n] : hist) v.push_back({n, k});
        std::sort(v.rbegin(), v.rend());
        fprintf(f, "PROFILE main thread (200 samples, innermost game frames first)\n");
        for (size_t i = 0; i < v.size() && i < 15; i++) fprintf(f, "  %3d  %s\n", v[i].first, v[i].second.c_str());
        if (mt) CloseHandle(mt);
    }
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te{sizeof te};
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
        HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!t) continue;
        SuspendThread(t);
        CONTEXT c{}; c.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(t, &c)) {
            fprintf(f, "THREAD %lu\n", te.th32ThreadID);
            for (int i = 0; i < 40 && c.Rip; i++) {
                fprintf(f, "   #%02d ", i); describe(c.Rip);
                DWORD64 ib; auto rf = RtlLookupFunctionEntry(c.Rip, &ib, nullptr);
                if (rf) fprintf(f, "  (fn rva 0x%lx)", rf->BeginAddress);
                fprintf(f, "\n");
                if (!rf) { c.Rip = *(DWORD64*)c.Rsp; c.Rsp += 8; continue; }
                PVOID hd; DWORD64 ef;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, ib, c.Rip, rf, &c, &hd, &ef, nullptr);
            }
        }
        ResumeThread(t); CloseHandle(t);
    }
    CloseHandle(snap);
    g_log = saved; fclose(f);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <gd_dir>\n", argv[0]); return 2; }
    g_dir = argv[1];
    g_log = fopen("loader_run.log", "w");
    char cwd[MAX_PATH]; GetCurrentDirectoryA(MAX_PATH, cwd);
    { char e[MAX_PATH]; GetModuleFileNameA(nullptr, e, MAX_PATH); *strrchr(e, '\\') = 0; g_host_dir = e; }
    g_sandbox = std::string(cwd) + "\\sandbox_localappdata";
    g_glfw_log = std::string(cwd) + "\\glfw_errors.log";   // absolute: cwd moves to the game folder below
    g_raw_log = std::string(cwd) + "\\exceptions.log";
    g_glfw_h = CreateFileA(g_glfw_log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
    g_raw_h = CreateFileA(g_raw_log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
    CreateDirectoryA(g_sandbox.c_str(), nullptr);
    SetCurrentDirectoryA(g_dir.c_str());               // GD resolves Resources/ relative to cwd
    SetDllDirectoryA(g_dir.c_str());
    AddVectoredExceptionHandler(0, crash_filter);
    g_stacks_log = std::string(cwd) + "\\stacks.log";
    g_main_tid = GetCurrentThreadId();
    g_gl_h = CreateFileA((std::string(cwd) + "\\gl.log").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                         CREATE_ALWAYS, 0, nullptr);
    g_files_h = CreateFileA((std::string(cwd) + "\\files.log").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                            CREATE_ALWAYS, 0, nullptr);
    if (getenv("GDPS4_WATCHDOG_MS"))   // opt-in diagnostics: director/limiter state, profile, all stacks
        CreateThread(nullptr, 0, watchdog, (LPVOID)(uintptr_t)atoi(getenv("GDPS4_WATCHDOG_MS")), 0, nullptr);

    PlatformHooks hooks;
    hooks.alloc_exec = [](size_t n) { return VirtualAlloc(nullptr, n, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE); };
    hooks.protect = [](void*, size_t, bool, bool, bool) { return true; };

    g_hooks = hooks;
    if (getenv("GDPS4_WIN32")) {   // hook the portable Win32 layer up to this loader
        gdps4::win32::Hooks wh;
        wh.exe_base = host_handle();   // real HMODULE: GLFW/user32 need it on the PC host
        wh.module_by_name = [](const std::string& n) -> void* {
            if (auto i = image_by_name(n)) return i->base;
            return (void*)GetModuleHandleA(n.c_str());
        };
        wh.load_library = [](const std::string& n) -> void* {
            if (load_image(canon_dll(n))) return g_loaded[canon_dll(n)].base;
            return (void*)LoadLibraryA(n.c_str());
        };
        wh.module_containing = [](const void* a) -> void* {
            if (image_containing(a)) return host_handle();   // GLFW asks for its own HINSTANCE
            HMODULE m = nullptr;
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &m);
            return (void*)m;
        };
        wh.module_path = [](void* base) -> std::string {
            if (auto i = image_by_handle((HMODULE)base)) {
                auto set = pe_set(); auto it = set.find(lower(i->name));
                return (it != set.end() ? it->second : g_dir) + "\\" + i->name;
            }
            char b[MAX_PATH] = ""; GetModuleFileNameA((HMODULE)base, b, MAX_PATH); return b;
        };
        wh.get_proc = [](void* base, const std::string& s) -> void* {
            if (auto i = image_by_handle((HMODULE)base)) return resolve_in_image(lower(i->name), s);
            return (void*)GetProcAddress((HMODULE)base, s[0] == '#' ? MAKEINTRESOURCEA(std::stoul(s.substr(1))) : s.c_str());
        };
        wh.map_path = [](const std::string& p) { return p; };   // PC host: Windows paths are host paths
        wh.command_line = u"\"GeometryDash.exe\"";
        { std::u16string cd; for (char c : g_dir) cd += (char16_t)(unsigned char)c; wh.current_dir = cd; }
        { std::u16string td; for (char c : std::string(cwd) + "\\sandbox_temp\\") td += (char16_t)(unsigned char)c; wh.temp_dir = td; }
        CreateDirectoryA((std::string(cwd) + "\\sandbox_temp").c_str(), nullptr);
        wh.log = [](const char* m) { fprintf(g_log, "W32LOG %s", m); fflush(g_log); };
        gdps4::win32::init(wh);
        fprintf(g_log, "Win32 layer: ON\n");
    }
    if (!load_image("geometrydash.exe")) return 1;   // pulls in every PE-set dependency recursively
    for (auto& n : g_init_order) fprintf(g_log, "INIT ORDER %s\n", n.c_str());
    fflush(g_log);

    // Only now, after every pass-through DLL has been loaded by Windows, claim TLS slots.
    setup_static_tls();

    // Diagnostic hook (in-memory only): libcocos2d+0x77760 is cocos2d's GLFW error callback
    // (code, message). It calls back into GLFW and recurses; log and swallow instead.
    // callback cannot re-enter GLFW and recurse.
    {
        struct H {
            // No CRT here (its per-thread state may be what's broken): raw Win32 only.
            static void put(const char* s) {
                HANDLE h = g_glfw_h;
                DWORD w; WriteFile(h, s, lstrlenA(s), &w, nullptr);
            }
            static void str(uint64_t v) {
                MEMORY_BASIC_INFORMATION mi;
                if (v > 0x10000 && VirtualQuery((void*)v, &mi, sizeof mi) && mi.State == MEM_COMMIT &&
                    !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
                    char b[200]; const char* s = (const char*)v; int i = 0;
                    b[i++] = ' '; b[i++] = '"';
                    for (int k = 0; k < 160 && s[k]; k++) b[i++] = (s[k] >= 32 && s[k] < 127) ? s[k] : '.';
                    b[i++] = '"'; b[i] = 0; put(b);
                }
            }
            static void log(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
                static int n = 0; if (++n > 50) return;
                char buf[160];
                wsprintfA(buf, "GLFW ERROR args=%I64x %I64x %I64x %I64x", a, b, c, d); put(buf);
                str(a); str(b); str(c); str(d); put("\r\n");
            } };
        uint8_t* p = g_loaded["libcocos2d.dll"].base + 0x77760; uint64_t t = (uint64_t)&H::log;
        p[0] = 0x48; p[1] = 0xB8; memcpy(p + 2, &t, 8); p[10] = 0xFF; p[11] = 0xE0;   // movabs rax,t; jmp rax
    }

    using DllMain = BOOL(WINAPI*)(HINSTANCE, DWORD, LPVOID);
    using TlsCb = void(NTAPI*)(PVOID, DWORD, PVOID);
    for (auto& name : g_init_order) {
        if (name == "geometrydash.exe") continue;
        auto& img = g_loaded[name];
        for (auto cb : img.tls_callbacks) ((TlsCb)cb)(img.base, DLL_PROCESS_ATTACH, nullptr);
        if (!img.entry_rva) continue;
        BOOL ok = ((DllMain)(img.base + img.entry_rva))((HINSTANCE)img.base, DLL_PROCESS_ATTACH, nullptr);
        fprintf(g_log, "DllMain %s -> %d\n", name.c_str(), ok); fflush(g_log);
        if (!ok) return 1;
    }
    auto& exe = g_loaded["geometrydash.exe"];
    fprintf(g_log, "ENTER GeometryDash.exe entry\n"); fflush(g_log);
    int rc = ((int (*)())(exe.base + exe.entry_rva))();   // mainCRTStartup; normally calls exit()
    fprintf(g_log, "EXIT rc=%d\n", rc);
    return rc;
}
