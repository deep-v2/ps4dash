// M2 host run: load the ORIGINAL GD images with our loader and actually execute them,
// forwarding every non-RobTop import to the real Windows/third-party DLL (pass-through).
// This validates loader, static TLS, SEH registration and entry sequencing on PC
// before each pass-through is replaced by a PS4 implementation.
// Usage: loader_run <gd_dir>      (vanilla: Geode's XInput proxy is bypassed)
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include "../src/pe_loader.hpp"
#include <windows.h>
#include <psapi.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <map>

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

static void* resolve(const std::string& dll, const std::string& sym) {
    if (auto it = g_loaded.find(dll); it != g_loaded.end()) return it->second.find_export(sym);
    if (dll == "steam_api64.dll") {
        fprintf(g_log, "STEAM stub %s\n", sym.c_str());
        if (sym == "SteamInternal_SteamAPI_Init") return (void*)steam_init_ok;
        if (sym == "SteamInternal_ContextInit") return (void*)steam_context_init;
        if (sym == "SteamInternal_FindOrCreateUserInterface") return (void*)steam_find_iface;
        if (sym == "SteamAPI_GetHSteamUser") return (void*)steam_user;
        return (void*)steam_void;   // RegisterCallback, UnregisterCallback, RunCallbacks, Shutdown
    }
    if (dll == "shell32.dll" && sym == "SHGetFolderPathA") return (void*)shim_SHGetFolderPathA;
    if (dll == "kernel32.dll") {
        static const std::map<std::string, void*> k32 = {
            {"GetModuleHandleA", (void*)shim_GetModuleHandleA}, {"GetModuleHandleW", (void*)shim_GetModuleHandleW},
            {"GetModuleFileNameA", (void*)shim_GetModuleFileNameA}, {"GetModuleFileNameW", (void*)shim_GetModuleFileNameW},
            {"LoadLibraryA", (void*)shim_LoadLibraryA}, {"LoadLibraryW", (void*)shim_LoadLibraryW},
            {"GetProcAddress", (void*)shim_GetProcAddress},
            {"GetModuleHandleExW", (void*)shim_GetModuleHandleExW}, {"GetModuleHandleExA", (void*)shim_GetModuleHandleExA}};
        if (auto it = k32.find(sym); it != k32.end()) return it->second;
    }
    static std::map<std::string, HMODULE> mods;
    auto& h = mods[dll];
    if (!h) h = load_real(dll);
    if (!h) return nullptr;
    LPCSTR name = sym[0] == '#' ? MAKEINTRESOURCEA(std::stoul(sym.substr(1))) : sym.c_str();
    return (void*)GetProcAddress(h, name);
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

    PlatformHooks hooks;
    hooks.alloc_exec = [](size_t n) { return VirtualAlloc(nullptr, n, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE); };
    hooks.protect = [](void*, size_t, bool, bool, bool) { return true; };

    const char* order[] = {"libcocos2d.dll", "libExtensions.dll", "GeometryDash.exe"};
    for (const char* name : order) {
        LoadedImage img;
        auto r = load_pe(read_file(g_dir + "\\" + name), name, hooks, resolve, img, true);
        if (!r.ok) { fprintf(g_log, "FAIL %s\n", r.error.c_str()); return 1; }
        for (auto& u : r.unresolved) fprintf(g_log, "UNRESOLVED %s\n", u.c_str());
        // Register unwind tables so C++ exceptions/SEH work inside the mapped image.
        if (img.pdata_size)
            RtlAddFunctionTable((PRUNTIME_FUNCTION)(img.base + img.pdata_rva), img.pdata_size / 12, (DWORD64)img.base);

        fprintf(g_log, "LOADED %s at %p\n", name, (void*)img.base); fflush(g_log);
        g_loaded[lower(name)] = std::move(img);
    }

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
    for (const char* name : {"libcocos2d.dll", "libextensions.dll"}) {
        auto& img = g_loaded[name];
        for (auto cb : img.tls_callbacks) ((TlsCb)cb)(img.base, DLL_PROCESS_ATTACH, nullptr);
        BOOL ok = ((DllMain)(img.base + img.entry_rva))((HINSTANCE)img.base, DLL_PROCESS_ATTACH, nullptr);
        fprintf(g_log, "DllMain %s -> %d\n", name, ok); fflush(g_log);
        if (!ok) return 1;
    }
    auto& exe = g_loaded["geometrydash.exe"];
    fprintf(g_log, "ENTER GeometryDash.exe entry\n"); fflush(g_log);
    int rc = ((int (*)())(exe.base + exe.entry_rva))();   // mainCRTStartup; normally calls exit()
    fprintf(g_log, "EXIT rc=%d\n", rc);
    return rc;
}
