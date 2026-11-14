// M3 probe: can a jailbroken PS4 host the original Windows GD code?
//  1. RWX memory: map, write x86-64, execute.
//  2. gs base: sysarch(AMD64_SET_GSBASE) and wrgsbase, then read gs:[0x30] like MSVC code does.
//  3. Real RobTop code: load /data/gdps4/libcocos2d.dll with our PE loader and call
//     CCPoint(float,float), CCPoint::operator+ and ccpLength through the Microsoft x64 ABI.
// Every risky step runs under a SIGSEGV/SIGBUS/SIGILL guard so one refusal can't kill the run.
// Results: on-screen notifications + /data/gdps4/m3_results.txt
#include "../../loader/src/pe_loader.hpp"
#include <orbis/libkernel.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <fstream>
#include <string>
#include <vector>

static FILE* g_out;
static std::string g_summary;

static void notify(const char* msg) {
    OrbisNotificationRequest req;
    memset(&req, 0, sizeof req);
    req.type = NotificationRequest;
    req.targetId = -1;
    strncpy(req.message, msg, sizeof(req.message) - 1);
    sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
}
static void logf(const char* fmt, ...) {
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    printf("%s\n", b);
    if (g_out) { fprintf(g_out, "%s\n", b); fflush(g_out); }
}
static void result(const char* name, bool ok, const char* detail) {
    logf("[%s] %s: %s", ok ? "PASS" : "FAIL", name, detail);
    g_summary += std::string(ok ? "OK " : "NO ") + name + "\n";
}

// ---- fault guard -----------------------------------------------------------
static sigjmp_buf g_jmp;
static volatile int g_sig;
static void on_fault(int s) { g_sig = s; siglongjmp(g_jmp, 1); }
#define GUARDED(...)                                                      \
    ({ bool _ok = false; g_sig = 0;                                       \
       if (sigsetjmp(g_jmp, 1) == 0) { __VA_ARGS__; _ok = true; }         \
       _ok; })

// ---- 1. executable memory -------------------------------------------------
static void* alloc_rwx(size_t n, const char** how) {
    n = (n + 0x3FFF) & ~(size_t)0x3FFF;   // 16 KiB pages
    void* p = nullptr;
    if (sceKernelMapNamedFlexibleMemory(&p, n, ORBIS_KERNEL_PROT_CPU_READ | ORBIS_KERNEL_PROT_CPU_WRITE |
                                        ORBIS_KERNEL_PROT_CPU_EXEC, 0, "gdps4") == 0 && p) {
        *how = "sceKernelMapNamedFlexibleMemory RWX"; return p;
    }
    p = mmap(nullptr, n, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p != MAP_FAILED) { *how = "mmap RWX"; return p; }
    *how = "none"; return nullptr;
}

// ---- 2. gs base -----------------------------------------------------------
static long raw_sysarch(int op, void* arg) {
    long ret; char cf;
    __asm__ volatile("syscall; setc %1" : "=a"(ret), "=r"(cf) : "a"(165L), "D"((long)op), "S"(arg)
                     : "rcx", "r11", "memory");
    return cf ? -ret : ret;
}
enum { AMD64_GET_GSBASE = 130, AMD64_SET_GSBASE = 131 };

int main() {
    signal(SIGSEGV, on_fault); signal(SIGBUS, on_fault); signal(SIGILL, on_fault);
    mkdir("/data/gdps4", 0777);
    g_out = fopen("/data/gdps4/m3_results.txt", "w");
    logf("gdps4 M3 probe");
    notify("gdps4 M3 probe running...");

    // 1. RWX memory + execute
    const char* how; uint8_t* code = (uint8_t*)alloc_rwx(0x4000, &how);
    int got = 0;
    bool exec_ok = code && GUARDED({
        static const uint8_t ret42[] = {0xB8, 0x2A, 0, 0, 0, 0xC3};   // mov eax,42; ret
        memcpy(code, ret42, sizeof ret42);
        got = ((int (*)())code)();
    }) && got == 42;
    { char d[160]; snprintf(d, sizeof d, "alloc=%s at %p, call returned %d, signal=%d", how, code, got, g_sig);
      result("exec_memory", exec_ok, d); }

    // 2. gs base. Fake TEB with the self pointer at +0x30, as MSVC code expects.
    static uint64_t teb[0x200];
    teb[0x30 / 8] = (uint64_t)teb;
    long sr = -1;
    bool sysarch_ok = GUARDED({ uint64_t base = (uint64_t)teb; sr = raw_sysarch(AMD64_SET_GSBASE, &base); }) && sr == 0;
    { char d[96]; snprintf(d, sizeof d, "sysarch(AMD64_SET_GSBASE) ret=%ld signal=%d", sr, g_sig);
      result("gs_sysarch", sysarch_ok, d); }
    bool wr_ok = false;
    if (!sysarch_ok)
        wr_ok = GUARDED({ __asm__ volatile("wrgsbase %0" :: "r"((uint64_t)teb)); });
    if (!sysarch_ok) { char d[64]; snprintf(d, sizeof d, "wrgsbase signal=%d", g_sig); result("gs_wrgsbase", wr_ok, d); }
    uint64_t seen = 0;
    bool gs_read = exec_ok && (sysarch_ok || wr_ok) && GUARDED({
        static const uint8_t rd[] = {0x65, 0x48, 0x8B, 0x04, 0x25, 0x30, 0, 0, 0, 0xC3};   // mov rax,gs:[0x30]; ret
        memcpy(code + 0x100, rd, sizeof rd);
        seen = ((uint64_t (*)())(code + 0x100))();
    }) && seen == (uint64_t)teb;
    { char d[128]; snprintf(d, sizeof d, "gs:[0x30]=%#llx expected %#llx signal=%d", (unsigned long long)seen,
                           (unsigned long long)teb, g_sig);
      result("gs_teb_read", gs_read, d); }

    // 3. Real RobTop code from the user's own libcocos2d.dll
    std::ifstream f("/data/gdps4/libcocos2d.dll", std::ios::binary);
    std::vector<uint8_t> dll{std::istreambuf_iterator<char>(f), {}};
    if (dll.empty()) {
        result("pe_load", false, "copy libcocos2d.dll from your GD folder to /data/gdps4/ first");
    } else {
        gdps4::PlatformHooks hooks;
        hooks.alloc_exec = [](size_t n) { const char* h; return alloc_rwx(n, &h); };
        hooks.protect = [](void*, size_t, bool, bool, bool) { return true; };
        // No shims yet: every import points at a guard that faults if anything calls it.
        static uint8_t* trap = code + 0x200; trap[0] = 0x0F; trap[1] = 0x0B;   // ud2
        gdps4::LoadedImage img;
        gdps4::LoadResult r;
        bool loaded = GUARDED({
            r = gdps4::load_pe(dll, "libcocos2d.dll", hooks, [](const std::string&, const std::string&) -> void* {
                return trap; }, img, true);
        }) && r.ok;
        { char d[160]; snprintf(d, sizeof d, "base=%p size=%#zx exports=%zu err=%s signal=%d", img.base, img.size,
                               img.exports.size(), r.error.c_str(), g_sig);
          result("pe_load", loaded, d); }
        if (loaded) {
            struct CCPoint { float x, y; };
            using Ctor = CCPoint* (__attribute__((ms_abi)) *)(CCPoint*, float, float);
            using Add = CCPoint* (__attribute__((ms_abi)) *)(const CCPoint*, CCPoint*, const CCPoint*);
            using Len = float (__attribute__((ms_abi)) *)(const CCPoint*);
            auto ctor = (Ctor)img.find_export("??0CCPoint@cocos2d@@QEAA@MM@Z");
            auto add = (Add)img.find_export("??HCCPoint@cocos2d@@QEBA?AV01@AEBV01@@Z");
            auto len = (Len)img.find_export("?ccpLength@cocos2d@@YAMAEBVCCPoint@1@@Z");
            CCPoint a{}, b{}, s{}; float l = -1;
            bool ran = ctor && add && len && GUARDED({
                ctor(&a, 3.0f, 4.0f); ctor(&b, 1.5f, -2.0f);
                add(&a, &s, &b);
                l = len(&a);
            });
            bool ok = ran && a.x == 3.0f && a.y == 4.0f && s.x == 4.5f && s.y == 2.0f && l == 5.0f;
            char d[200]; snprintf(d, sizeof d, "CCPoint(3,4)=(%g,%g) +(1.5,-2)=(%g,%g) ccpLength=%g signal=%d",
                                  a.x, a.y, s.x, s.y, l, g_sig);
            result("robtop_code", ok, d);
        }
    }

    logf("done");
    notify(("gdps4 M3:\n" + g_summary).c_str());
    if (g_out) fclose(g_out);
    for (;;) sceKernelUsleep(1000000);   // stay alive so the notification is readable
}
