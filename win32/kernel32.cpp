// Portable kernel32 (+ the advapi32/shell32 bits GD uses). See win32.hpp.
// Windows types are defined here by hand: WCHAR is char16_t (wchar_t is 32-bit on PS4),
// LONG/DWORD are 32-bit, and every export uses the Microsoft x64 calling convention.
#include "win32.hpp"
#include "platform.hpp"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#define W32 __attribute__((ms_abi))

namespace gdps4::win32 {
namespace {

using BYTE = uint8_t; using WORD = uint16_t; using DWORD = uint32_t; using LONG = int32_t;
using BOOL = int32_t; using UINT = uint32_t; using ULONG = uint32_t; using WCHAR = char16_t;
using HANDLE = void*; using LPVOID = void*; using SIZE_T = size_t; using ULONG_PTR = uintptr_t;
using LONGLONG = int64_t; using ULONGLONG = uint64_t; using HRESULT = int32_t;
constexpr BOOL TRUE_ = 1, FALSE_ = 0;
#define INVALID_HANDLE ((HANDLE)(intptr_t)-1)

// Windows error codes used here
enum : DWORD {
    E_SUCCESS = 0, E_INVALID_FUNCTION = 1, E_FILE_NOT_FOUND = 2, E_PATH_NOT_FOUND = 3, E_ACCESS_DENIED = 5,
    E_INVALID_HANDLE = 6, E_NOT_ENOUGH_MEMORY = 8, E_NO_MORE_FILES = 18, E_HANDLE_EOF = 38, E_NOT_SUPPORTED = 50,
    E_FILE_EXISTS = 80, E_INVALID_PARAMETER = 87, E_INSUFFICIENT_BUFFER = 122, E_DIR_NOT_EMPTY = 145,
    E_ALREADY_EXISTS = 183, E_ENVVAR_NOT_FOUND = 203, E_NO_TOKEN = 1008, E_INVALID_FLAGS = 1004,
    E_RESOURCE_TYPE_NOT_FOUND = 1813, E_PROC_NOT_FOUND = 127, E_MOD_NOT_FOUND = 126, E_TIMEOUT = 1460,
};

Hooks g_h;
void log(const char* fmt, ...) {
    char b[512]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    if (g_h.log) g_h.log(b); else plat::debug_log(b);
}

thread_local DWORD t_last_error;
void set_err(DWORD e) { t_last_error = e; }
DWORD err_from_errno(int e) {
    switch (e) {
    case ENOENT: return E_FILE_NOT_FOUND;
    case EEXIST: return E_ALREADY_EXISTS;
    case EACCES: case EPERM: return E_ACCESS_DENIED;
    case ENOTEMPTY: return E_DIR_NOT_EMPTY;
    case ENOMEM: return E_NOT_ENOUGH_MEMORY;
    default: return E_INVALID_FUNCTION;
    }
}

// ---- UTF-16 / UTF-8 / code page 1252 ------------------------------------------------------
size_t wlen(const WCHAR* s) { size_t n = 0; while (s && s[n]) n++; return n; }
std::string to_utf8(const WCHAR* s, size_t n = (size_t)-1) {
    std::string o; if (!s) return o;
    if (n == (size_t)-1) n = wlen(s);
    for (size_t i = 0; i < n; i++) {
        uint32_t c = s[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000)
            c = 0x10000 + ((c - 0xD800) << 10) + (s[++i] - 0xDC00);
        if (c < 0x80) o += (char)c;
        else if (c < 0x800) { o += (char)(0xC0 | c >> 6); o += (char)(0x80 | (c & 63)); }
        else if (c < 0x10000) { o += (char)(0xE0 | c >> 12); o += (char)(0x80 | ((c >> 6) & 63)); o += (char)(0x80 | (c & 63)); }
        else { o += (char)(0xF0 | c >> 18); o += (char)(0x80 | ((c >> 12) & 63)); o += (char)(0x80 | ((c >> 6) & 63)); o += (char)(0x80 | (c & 63)); }
    }
    return o;
}
std::u16string from_utf8(const char* s, size_t n = (size_t)-1) {
    std::u16string o; if (!s) return o;
    if (n == (size_t)-1) n = strlen(s);
    for (size_t i = 0; i < n;) {
        uint8_t b = (uint8_t)s[i]; uint32_t c; int extra;
        if (b < 0x80) { c = b; extra = 0; }
        else if ((b >> 5) == 6) { c = b & 31; extra = 1; }
        else if ((b >> 4) == 14) { c = b & 15; extra = 2; }
        else if ((b >> 3) == 30) { c = b & 7; extra = 3; }
        else { o += u'\uFFFD'; i++; continue; }
        if (i + extra >= n + (extra ? 0 : 1) && extra) { o += u'\uFFFD'; break; }
        i++;
        bool bad = false;
        for (int k = 0; k < extra; k++, i++) {
            if (i >= n || ((uint8_t)s[i] >> 6) != 2) { bad = true; break; }
            c = (c << 6) | ((uint8_t)s[i] & 63);
        }
        if (bad) { o += u'\uFFFD'; continue; }
        if (c >= 0x10000) { c -= 0x10000; o += (char16_t)(0xD800 + (c >> 10)); o += (char16_t)(0xDC00 + (c & 1023)); }
        else o += (char16_t)c;
    }
    return o;
}
// cp1252 0x80-0x9F block; other bytes map 1:1 (Latin-1)
const char16_t k1252[32] = {0x20AC, 0x81, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039,
                            0x0152, 0x8D, 0x017D, 0x8F, 0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                            0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D, 0x017E, 0x0178};
constexpr UINT kACP = 1252, kOEMCP = 437, CP_UTF8_ = 65001;
UINT real_cp(UINT cp) { return cp == 0 || cp == 3 ? kACP : cp == 1 || cp == 2 ? kOEMCP : cp; }
std::u16string ansi_to_w(const char* s, size_t n = (size_t)-1, UINT cp = 0) {
    cp = real_cp(cp);
    if (cp == CP_UTF8_) return from_utf8(s, n);
    std::u16string o; if (!s) return o;
    if (n == (size_t)-1) n = strlen(s);
    for (size_t i = 0; i < n; i++) {
        uint8_t b = (uint8_t)s[i];
        o += (cp == 1252 && b >= 0x80 && b < 0xA0) ? k1252[b - 0x80] : (char16_t)b;
    }
    return o;
}
std::string w_to_ansi(const WCHAR* s, size_t n, UINT cp, bool* used_default) {
    cp = real_cp(cp);
    if (cp == CP_UTF8_) return to_utf8(s, n);
    std::string o;
    for (size_t i = 0; i < n; i++) {
        char16_t c = s[i];
        if (c < 0x80 || (c >= 0xA0 && c < 0x100)) { o += (char)c; continue; }
        int hit = -1;
        if (cp == 1252) for (int k = 0; k < 32; k++) if (k1252[k] == c) { hit = k; break; }
        if (hit >= 0) o += (char)(0x80 + hit);
        else { o += '?'; if (used_default) *used_default = true; }
    }
    return o;
}

// ---- paths ------------------------------------------------------------------------------
std::u16string g_cwd;   // Windows-style, no trailing backslash
std::mutex g_cwd_mx;
std::u16string normalize(std::u16string p) {
    for (auto& c : p) if (c == u'/') c = u'\\';
    bool abs = p.size() >= 2 && p[1] == u':';
    bool unc = p.rfind(u"\\\\", 0) == 0;
    if (!abs && !unc) {
        std::lock_guard<std::mutex> l(g_cwd_mx);
        if (!p.empty() && p[0] == u'\\') p = g_cwd.substr(0, 2) + p;
        else p = g_cwd + u"\\" + p;
    }
    // collapse . and ..
    std::vector<std::u16string> parts; std::u16string cur, root = p.substr(0, 2);
    for (size_t i = 3; i <= p.size(); i++) {
        if (i == p.size() || p[i] == u'\\') {
            if (cur == u"..") { if (!parts.empty()) parts.pop_back(); }
            else if (!cur.empty() && cur != u".") parts.push_back(cur);
            cur.clear();
        } else cur += p[i];
    }
    std::u16string out = root + u"\\";
    for (size_t i = 0; i < parts.size(); i++) out += (i ? u"\\" : u"") + parts[i];
    return out;
}
std::string host_path(const WCHAR* p) {
    std::u16string full = normalize(p ? std::u16string(p) : std::u16string());
    return g_h.map_path ? g_h.map_path(to_utf8(full.c_str())) : to_utf8(full.c_str());
}

// ---- time ----------------------------------------------------------------------------------
constexpr int64_t kEpochDiff = 116444736000000000LL;   // 1601 -> 1970 in 100ns
struct FILETIME { DWORD lo, hi; };
struct SYSTEMTIME { WORD year, month, dow, day, hour, minute, second, ms; };
uint64_t now_filetime() { return (uint64_t)(plat::realtime_unix_ns() / 100 + kEpochDiff); }
void put_ft(FILETIME* f, uint64_t v) { if (f) { f->lo = (DWORD)v; f->hi = (DWORD)(v >> 32); } }
uint64_t get_ft(const FILETIME* f) { return ((uint64_t)f->hi << 32) | f->lo; }
// civil date algorithms (Howard Hinnant), days since 1970-01-01
int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2; int64_t era = (y >= 0 ? y : y - 399) / 400; unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}
void civil_from_days(int64_t z, int& y, unsigned& m, unsigned& d) {
    z += 719468; int64_t era = (z >= 0 ? z : z - 146096) / 146097; unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; int64_t yy = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100); unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1; m = mp + (mp < 10 ? 3 : -9); y = (int)(yy + (m <= 2));
}
void ft_to_st(uint64_t ft, SYSTEMTIME* st) {
    int64_t t = (int64_t)ft - kEpochDiff;   // 100ns since 1970
    int64_t secs = t >= 0 ? t / 10000000 : -((-t + 9999999) / 10000000);
    int64_t rem = t - secs * 10000000, days = secs >= 0 ? secs / 86400 : -((-secs + 86399) / 86400);
    int64_t sod = secs - days * 86400; int y; unsigned m, d; civil_from_days(days, y, m, d);
    st->year = (WORD)y; st->month = (WORD)m; st->day = (WORD)d;
    st->dow = (WORD)(((days % 7) + 11) % 7);   // 1970-01-01 was a Thursday (4)
    st->hour = (WORD)(sod / 3600); st->minute = (WORD)(sod / 60 % 60); st->second = (WORD)(sod % 60); st->ms = (WORD)(rem / 10000);
}
uint64_t st_to_ft(const SYSTEMTIME* st) {
    int64_t days = days_from_civil(st->year, st->month, st->day);
    int64_t secs = days * 86400 + st->hour * 3600 + st->minute * 60 + st->second;
    return (uint64_t)(secs * 10000000 + st->ms * 10000LL + kEpochDiff);
}

// ---- handle table ------------------------------------------------------------------------
struct Obj { virtual ~Obj() = default; virtual bool waitable() const { return false; } };
std::mutex g_wait_mx;                 // guards signal state of every waitable object
std::condition_variable g_wait_cv;    // notified on any signal change
struct Waitable : Obj {
    bool waitable() const override { return true; }
    virtual bool signaled() const = 0;   // g_wait_mx held
    virtual void acquire() {}            // g_wait_mx held: consume on successful wait
};
struct Event : Waitable {
    bool manual, state;
    Event(bool m, bool s) : manual(m), state(s) {}
    bool signaled() const override { return state; }
    void acquire() override { if (!manual) state = false; }
};
struct Mutex : Waitable {   // ReleaseMutex isn't imported; behaves as a one-shot lock
    uint64_t owner = 0; int count = 0;
    bool signaled() const override { return count == 0 || owner == plat::thread_id(); }
    void acquire() override { owner = plat::thread_id(); count++; }
};
struct Semaphore : Waitable {
    LONG count, max;
    Semaphore(LONG c, LONG m) : count(c), max(m) {}
    bool signaled() const override { return count > 0; }
    void acquire() override { count--; }
};
struct Thread : Waitable {
    std::atomic<bool> done{false}; DWORD exit_code = 259 /*STILL_ACTIVE*/; uint64_t tid = 0;
    std::mutex start_mx; std::condition_variable start_cv; bool suspended = false;
    bool signaled() const override { return done; }
};
struct File : Obj {
    intptr_t fd = -1; bool is_dir = false, console = false; int std_no = -1; std::string path;
    ~File() override { if (fd >= 0 && !console) plat::file_close(fd); }
};
struct FindData : Obj { std::vector<std::string> names; size_t next = 0; std::string dir; };
struct Mapping : Obj { std::shared_ptr<Obj> file; uint64_t size = 0; };

std::mutex g_handles_mx;
std::unordered_map<uintptr_t, std::shared_ptr<Obj>> g_handles;
uintptr_t g_next_handle = 0x1000;
HANDLE add_handle(std::shared_ptr<Obj> o) {
    std::lock_guard<std::mutex> l(g_handles_mx);
    uintptr_t h = g_next_handle; g_next_handle += 4;
    g_handles[h] = std::move(o);
    return (HANDLE)h;
}
std::shared_ptr<Thread> current_thread_obj();
std::shared_ptr<Obj> get_obj(HANDLE h) {
    if (h == (HANDLE)(intptr_t)-2) return current_thread_obj();
    std::lock_guard<std::mutex> l(g_handles_mx);
    auto it = g_handles.find((uintptr_t)h);
    return it == g_handles.end() ? nullptr : it->second;
}
template <class T> std::shared_ptr<T> get_as(HANDLE h) { return std::dynamic_pointer_cast<T>(get_obj(h)); }

// ---- per-thread state: TLS, FLS, TEB -----------------------------------------------------
constexpr int kTlsSlots = 1088;
std::mutex g_tls_mx;
std::vector<bool> g_tls_used(kTlsSlots), g_fls_used(kTlsSlots);
std::vector<void*> g_fls_cb(kTlsSlots);   // ms_abi void(void*)
thread_local void* t_tls[kTlsSlots];
thread_local void* t_fls[kTlsSlots];
thread_local void* t_teb;
thread_local std::shared_ptr<Thread> t_self;
std::shared_ptr<Thread> current_thread_obj() {
    if (!t_self) { t_self = std::make_shared<Thread>(); t_self->tid = plat::thread_id(); }
    return t_self;
}
void run_fls_callbacks() {
    for (int i = 0; i < kTlsSlots; i++) {
        void* cb; { std::lock_guard<std::mutex> l(g_tls_mx); cb = g_fls_used[i] ? g_fls_cb[i] : nullptr; }
        if (cb && t_fls[i]) { void* v = t_fls[i]; t_fls[i] = nullptr; ((void (W32*)(void*))cb)(v); }
    }
}

// Win32 TEB/PEB for PS4 (gs-relative fields MSVC code reads directly).
struct alignas(16) Peb { uint8_t raw[0x400]; };
Peb g_peb;
}  // namespace

void* current_teb() { return t_teb; }
void* attach_current_thread() {
    if (t_teb) return t_teb;
    auto* teb = (uint8_t*)calloc(1, 0x2000);
    *(void**)(teb + 0x30) = teb;                           // NtTib.Self
    *(void**)(teb + 0x60) = &g_peb;                        // ProcessEnvironmentBlock
    *(uint64_t*)(teb + 0x40) = 4242;                       // ClientId.UniqueProcess
    *(uint64_t*)(teb + 0x48) = plat::thread_id();          // ClientId.UniqueThread
    // Stack bounds: approximate from the current stack pointer (used by __chkstk/_alloca probes).
    uint64_t sp = (uint64_t)__builtin_frame_address(0);
    *(uint64_t*)(teb + 0x08) = (sp + 0x10000) & ~0xFFFull;           // StackBase (above us)
    *(uint64_t*)(teb + 0x10) = (sp - 0x100000) & ~0xFFFull;          // StackLimit
    if (!plat::set_gs_base(teb)) { free(teb); return nullptr; }   // Windows host: real TEB already
    t_teb = teb;
    return teb;
}

namespace {
// ================================ exports ================================================
// --- errors ---
W32 DWORD GetLastError() { return t_last_error; }
W32 void SetLastError(DWORD e) { t_last_error = e; }

// --- handles ---
W32 BOOL CloseHandle(HANDLE h) {
    if (h == (HANDLE)(intptr_t)-1 || h == (HANDLE)(intptr_t)-2) return TRUE_;
    std::lock_guard<std::mutex> l(g_handles_mx);
    if (!g_handles.erase((uintptr_t)h)) { set_err(E_INVALID_HANDLE); return FALSE_; }
    return TRUE_;
}
W32 BOOL DuplicateHandle(HANDLE, HANDLE src, HANDLE, HANDLE* out, DWORD, BOOL, DWORD opts) {
    auto o = get_obj(src);
    if (!o) { set_err(E_INVALID_HANDLE); return FALSE_; }
    *out = add_handle(o);
    if (opts & 1 /*DUPLICATE_CLOSE_SOURCE*/) CloseHandle(src);
    return TRUE_;
}

// --- memory: virtual ---
struct Region { size_t size; int prot; };
std::mutex g_vm_mx;
std::map<uintptr_t, Region> g_regions;
int prot_from_page(DWORD p) {
    switch (p & 0xFF) {
    case 0x01: return 0; case 0x02: return 1; case 0x04: case 0x08: return 3;
    case 0x10: return 4 | 1; case 0x20: return 5; case 0x40: case 0x80: return 7;
    default: return 3;
    }
}
DWORD page_from_prot(int p) {
    if (p & 4) return (p & 2) ? 0x40 : 0x20;
    return (p & 2) ? 0x04 : (p & 1) ? 0x02 : 0x01;
}
W32 LPVOID VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD prot) {
    std::lock_guard<std::mutex> l(g_vm_mx);
    if (addr) {   // commit (or re-protect) inside an existing reservation
        auto it = g_regions.upper_bound((uintptr_t)addr);
        if (it != g_regions.begin()) {
            --it;
            if ((uintptr_t)addr < it->first + it->second.size) {
                plat::vm_protect(addr, size, prot_from_page(prot));
                return addr;
            }
        }
        if (!(type & 0x2000)) { set_err(E_INVALID_PARAMETER); return nullptr; }
    }
    size = (size + 0xFFFF) & ~(SIZE_T)0xFFFF;
    void* p = plat::vm_reserve(size, (type & 0x1000) != 0, prot_from_page(prot));
    if (!p) { set_err(E_NOT_ENOUGH_MEMORY); return nullptr; }
    memset(p, 0, (type & 0x1000) ? size : 0);
    g_regions[(uintptr_t)p] = {size, prot_from_page(prot)};
    return p;
}
W32 BOOL VirtualFree(LPVOID addr, SIZE_T, DWORD type) {
    if (!(type & 0x8000 /*MEM_RELEASE*/)) return TRUE_;   // decommit: keep pages
    std::lock_guard<std::mutex> l(g_vm_mx);
    auto it = g_regions.find((uintptr_t)addr);
    if (it == g_regions.end()) { set_err(E_INVALID_PARAMETER); return FALSE_; }
    plat::vm_release(addr, it->second.size); g_regions.erase(it);
    return TRUE_;
}
W32 BOOL VirtualProtect(LPVOID addr, SIZE_T size, DWORD prot, DWORD* old) {
    int oldp = 3;
    {
        std::lock_guard<std::mutex> l(g_vm_mx);
        auto it = g_regions.upper_bound((uintptr_t)addr);
        if (it != g_regions.begin()) { --it; oldp = it->second.prot; }
    }
    if (old) *old = page_from_prot(oldp);
    if (!plat::vm_protect(addr, size, prot_from_page(prot))) { set_err(E_INVALID_PARAMETER); return FALSE_; }
    return TRUE_;
}
struct MBI { void* base; void* alloc_base; DWORD alloc_prot; DWORD pad0; SIZE_T size; DWORD state, prot, type, pad1; };
W32 SIZE_T VirtualQuery(const void* addr, MBI* mbi, SIZE_T len) {
    if (len < sizeof(MBI)) { set_err(E_INSUFFICIENT_BUFFER); return 0; }
    memset(mbi, 0, sizeof *mbi);
    std::lock_guard<std::mutex> l(g_vm_mx);
    auto it = g_regions.upper_bound((uintptr_t)addr);
    if (it != g_regions.begin()) {
        --it;
        if ((uintptr_t)addr < it->first + it->second.size) {
            mbi->base = (void*)((uintptr_t)addr & ~0xFFFull); mbi->alloc_base = (void*)it->first;
            mbi->alloc_prot = mbi->prot = page_from_prot(it->second.prot);
            mbi->size = it->first + it->second.size - (uintptr_t)mbi->base;
            mbi->state = 0x1000; mbi->type = 0x20000;
            return sizeof(MBI);
        }
    }
    // Not ours (stack, images, libc heap): report a committed RW page.
    mbi->base = mbi->alloc_base = (void*)((uintptr_t)addr & ~0xFFFull);
    mbi->alloc_prot = mbi->prot = 0x04; mbi->size = 0x1000; mbi->state = 0x1000; mbi->type = 0x20000;
    return sizeof(MBI);
}

// --- memory: heaps (malloc with a 16-byte size header, keeps 16-byte alignment) ---
void* heap_alloc(size_t n, bool zero) {
    auto* p = (uint8_t*)(zero ? calloc(1, n + 16) : malloc(n + 16));
    if (!p) return nullptr;
    *(size_t*)p = n;
    return p + 16;
}
void heap_free(void* p) { if (p) free((uint8_t*)p - 16); }
size_t heap_size(const void* p) { return *(const size_t*)((const uint8_t*)p - 16); }
void* heap_realloc(void* p, size_t n, bool zero) {
    if (!p) return heap_alloc(n, zero);
    size_t old = heap_size(p);
    auto* b = (uint8_t*)realloc((uint8_t*)p - 16, n + 16);
    if (!b) return nullptr;
    *(size_t*)b = n;
    if (zero && n > old) memset(b + 16 + old, 0, n - old);
    return b + 16;
}
const HANDLE kProcessHeap = (HANDLE)0xF00D0000;
W32 HANDLE GetProcessHeap() { return kProcessHeap; }
W32 HANDLE HeapCreate(DWORD, SIZE_T, SIZE_T) { static std::atomic<uintptr_t> n{0xF00D1000}; return (HANDLE)(n += 16); }
W32 BOOL HeapDestroy(HANDLE) { return TRUE_; }   // blocks share the process allocator; nothing to drop
W32 LPVOID HeapAlloc(HANDLE, DWORD flags, SIZE_T n) {
    void* p = heap_alloc(n, flags & 0x8);
    if (!p) set_err(E_NOT_ENOUGH_MEMORY);
    return p;
}
W32 BOOL HeapFree(HANDLE, DWORD, LPVOID p) { heap_free(p); return TRUE_; }
W32 LPVOID HeapReAlloc(HANDLE, DWORD flags, LPVOID p, SIZE_T n) {
    if (flags & 0x10 /*HEAP_REALLOC_IN_PLACE_ONLY*/) { if (n <= heap_size(p)) { *(size_t*)((uint8_t*)p - 16) = n; return p; } return nullptr; }
    void* r = heap_realloc(p, n, flags & 0x8);
    if (!r) set_err(E_NOT_ENOUGH_MEMORY);
    return r;
}
W32 SIZE_T HeapSize(HANDLE, DWORD, const void* p) { return p ? heap_size(p) : (SIZE_T)-1; }
W32 BOOL HeapValidate(HANDLE, DWORD, const void*) { return TRUE_; }
W32 SIZE_T HeapCompact(HANDLE, DWORD) { return 1 << 20; }
W32 BOOL HeapWalk(HANDLE, void*) { set_err(E_NO_MORE_FILES); return FALSE_; }
W32 BOOL HeapQueryInformation(HANDLE, int cls, void* info, SIZE_T len, SIZE_T* ret) {
    if (cls == 0 && len >= 4) { *(ULONG*)info = 2; if (ret) *ret = 4; return TRUE_; }   // LFH
    set_err(E_INVALID_PARAMETER); return FALSE_;
}
W32 HANDLE GlobalAlloc(UINT flags, SIZE_T n) { return heap_alloc(n, flags & 0x40); }
W32 LPVOID GlobalLock(HANDLE h) { return h; }
W32 BOOL GlobalUnlock(HANDLE) { return TRUE_; }
W32 HANDLE LocalAlloc(UINT flags, SIZE_T n) { return heap_alloc(n, flags & 0x40); }
W32 HANDLE LocalFree(HANDLE h) { heap_free(h); return nullptr; }
struct MEMORYSTATUS { DWORD len, load; SIZE_T total_phys, avail_phys, total_page, avail_page, total_virt, avail_virt; };
W32 void GlobalMemoryStatus(MEMORYSTATUS* m) {
    m->load = 50; m->total_phys = 4ull << 30; m->avail_phys = 2ull << 30;
    m->total_page = 8ull << 30; m->avail_page = 4ull << 30; m->total_virt = 128ull << 40; m->avail_virt = 120ull << 40;
}

// --- sync: critical sections (our recursive_mutex pointer lives in LockSemaphore, +0x18) ---
std::recursive_timed_mutex* cs_mutex(void* cs) {
    auto* slot = (std::atomic<std::recursive_timed_mutex*>*)((uint8_t*)cs + 0x18);
    auto* m = slot->load(std::memory_order_acquire);
    if (!m) {
        auto* fresh = new std::recursive_timed_mutex;
        if (slot->compare_exchange_strong(m, fresh)) m = fresh; else delete fresh;
    }
    return m;
}
W32 void InitializeCriticalSection(void* cs) { memset(cs, 0, 40); cs_mutex(cs); }
W32 BOOL InitializeCriticalSectionAndSpinCount(void* cs, DWORD) { InitializeCriticalSection(cs); return TRUE_; }
W32 BOOL InitializeCriticalSectionEx(void* cs, DWORD, DWORD) { InitializeCriticalSection(cs); return TRUE_; }
W32 void DeleteCriticalSection(void* cs) {
    auto* slot = (std::atomic<std::recursive_timed_mutex*>*)((uint8_t*)cs + 0x18);
    delete slot->exchange(nullptr);
}
W32 void EnterCriticalSection(void* cs) {
    cs_mutex(cs)->lock();
    *(uint64_t*)((uint8_t*)cs + 0x10) = plat::thread_id();   // OwningThread
    ++*(LONG*)((uint8_t*)cs + 0x0C);                        // RecursionCount
}
W32 BOOL TryEnterCriticalSection(void* cs) {
    if (!cs_mutex(cs)->try_lock()) return FALSE_;
    *(uint64_t*)((uint8_t*)cs + 0x10) = plat::thread_id();
    ++*(LONG*)((uint8_t*)cs + 0x0C);
    return TRUE_;
}
W32 void LeaveCriticalSection(void* cs) {
    if (--*(LONG*)((uint8_t*)cs + 0x0C) == 0) *(uint64_t*)((uint8_t*)cs + 0x10) = 0;
    cs_mutex(cs)->unlock();
}

// --- sync: SRW locks, condition variables, init-once (pointer-sized, zero = initial) ---
template <class T> T* lazy(void* slotp) {
    auto* slot = (std::atomic<T*>*)slotp;
    T* v = slot->load(std::memory_order_acquire);
    if (!v) { T* fresh = new T; if (slot->compare_exchange_strong(v, fresh)) v = fresh; else delete fresh; }
    return v;
}
W32 void AcquireSRWLockExclusive(void* l) { lazy<std::mutex>(l)->lock(); }
W32 BOOL TryAcquireSRWLockExclusive(void* l) { return lazy<std::mutex>(l)->try_lock(); }
W32 void ReleaseSRWLockExclusive(void* l) { lazy<std::mutex>(l)->unlock(); }
W32 void InitializeConditionVariable(void* cv) { *(void**)cv = nullptr; }
W32 void WakeConditionVariable(void* cv) { lazy<std::condition_variable_any>(cv)->notify_one(); }
W32 void WakeAllConditionVariable(void* cv) { lazy<std::condition_variable_any>(cv)->notify_all(); }
template <class M> BOOL cv_sleep(void* cv, M& m, DWORD ms) {
    auto* c = lazy<std::condition_variable_any>(cv);
    if (ms == 0xFFFFFFFF) { c->wait(m); return TRUE_; }
    if (c->wait_for(m, std::chrono::milliseconds(ms)) == std::cv_status::timeout) { set_err(E_TIMEOUT); return FALSE_; }
    return TRUE_;
}
W32 BOOL SleepConditionVariableSRW(void* cv, void* srw, DWORD ms, ULONG) { return cv_sleep(cv, *lazy<std::mutex>(srw), ms); }
W32 BOOL SleepConditionVariableCS(void* cv, void* cs, DWORD ms) {
    // Windows releases the CS fully; we hold it once in practice (STL/CRT usage).
    LONG* rec = (LONG*)((uint8_t*)cs + 0x0C); LONG saved = *rec; *rec = 0;
    BOOL r = cv_sleep(cv, *cs_mutex(cs), ms);
    *rec = saved; *(uint64_t*)((uint8_t*)cs + 0x10) = plat::thread_id();
    return r;
}
struct OnceState { std::mutex m; std::atomic<int> done{0}; };
W32 BOOL InitOnceExecuteOnce(void* once, BOOL (W32* fn)(void*, void*, void**), void* param, void** ctx) {
    auto* st = lazy<OnceState>(once);
    if (st->done.load(std::memory_order_acquire)) return TRUE_;
    std::lock_guard<std::mutex> l(st->m);
    if (st->done.load()) return TRUE_;
    BOOL ok = fn(once, param, ctx);
    if (ok) st->done.store(1, std::memory_order_release);
    return ok;
}

// --- sync: SLists (global lock; GD only uses them during CRT/STL bookkeeping) ---
std::mutex g_slist_mx;
W32 void InitializeSListHead(void* head) { memset(head, 0, 16); }
W32 void* InterlockedPushEntrySList(void* head, void* entry) {
    std::lock_guard<std::mutex> l(g_slist_mx);
    void* old = *(void**)head; *(void**)entry = old; *(void**)head = entry; ++*(uint16_t*)((uint8_t*)head + 8);
    return old;
}
W32 void* InterlockedFlushSList(void* head) {
    std::lock_guard<std::mutex> l(g_slist_mx);
    void* old = *(void**)head; *(void**)head = nullptr; *(uint16_t*)((uint8_t*)head + 8) = 0;
    return old;
}

// --- sync: kernel objects + waits ---
W32 HANDLE CreateEventExW(void*, const WCHAR*, DWORD flags, DWORD) { return add_handle(std::make_shared<Event>(flags & 1, flags & 2)); }
W32 HANDLE CreateEventA(void*, BOOL manual, BOOL initial, const char*) { return add_handle(std::make_shared<Event>(manual, initial)); }
W32 HANDLE CreateMutexW(void*, BOOL owned, const WCHAR*) {
    auto m = std::make_shared<Mutex>();
    if (owned) m->acquire();
    return add_handle(m);
}
W32 HANDLE CreateSemaphoreExW(void*, LONG init, LONG max, const WCHAR*, DWORD, DWORD) { return add_handle(std::make_shared<Semaphore>(init, max)); }
W32 HANDLE CreateSemaphoreA(void*, LONG init, LONG max, const char*) { return add_handle(std::make_shared<Semaphore>(init, max)); }
W32 BOOL SetEvent(HANDLE h) {
    auto e = get_as<Event>(h); if (!e) { set_err(E_INVALID_HANDLE); return FALSE_; }
    { std::lock_guard<std::mutex> l(g_wait_mx); e->state = true; }
    g_wait_cv.notify_all(); return TRUE_;
}
W32 BOOL ResetEvent(HANDLE h) {
    auto e = get_as<Event>(h); if (!e) { set_err(E_INVALID_HANDLE); return FALSE_; }
    std::lock_guard<std::mutex> l(g_wait_mx); e->state = false; return TRUE_;
}
W32 BOOL ReleaseSemaphore(HANDLE h, LONG n, LONG* prev) {
    auto s = get_as<Semaphore>(h); if (!s) { set_err(E_INVALID_HANDLE); return FALSE_; }
    { std::lock_guard<std::mutex> l(g_wait_mx);
      if (prev) *prev = s->count;
      if (s->count + n > s->max) { set_err(298 /*ERROR_TOO_MANY_POSTS*/); return FALSE_; }
      s->count += n; }
    g_wait_cv.notify_all(); return TRUE_;
}
W32 DWORD WaitForMultipleObjects(DWORD n, const HANDLE* hs, BOOL all, DWORD ms) {
    std::vector<std::shared_ptr<Waitable>> objs;
    for (DWORD i = 0; i < n; i++) {
        auto w = std::dynamic_pointer_cast<Waitable>(get_obj(hs[i]));
        if (!w) { set_err(E_INVALID_HANDLE); return 0xFFFFFFFF; }
        objs.push_back(w);
    }
    std::unique_lock<std::mutex> l(g_wait_mx);
    auto ready = [&]() -> int {
        if (all) {
            for (auto& o : objs) if (!o->signaled()) return -1;
            for (auto& o : objs) o->acquire();
            return 0;
        }
        for (DWORD i = 0; i < n; i++) if (objs[i]->signaled()) { objs[i]->acquire(); return (int)i; }
        return -1;
    };
    int r;
    if (ms == 0xFFFFFFFF) { while ((r = ready()) < 0) g_wait_cv.wait(l); }
    else {
        auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while ((r = ready()) < 0)
            if (g_wait_cv.wait_until(l, until) == std::cv_status::timeout && (r = ready()) < 0) return 0x102;
    }
    return (DWORD)r;
}
W32 DWORD WaitForSingleObject(HANDLE h, DWORD ms) { return WaitForMultipleObjects(1, &h, TRUE_, ms); }
W32 DWORD WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL) { return WaitForSingleObject(h, ms); }

// --- TLS / FLS ---
W32 DWORD TlsAlloc() {
    std::lock_guard<std::mutex> l(g_tls_mx);
    for (int i = 0; i < kTlsSlots; i++) if (!g_tls_used[i]) { g_tls_used[i] = true; return i; }
    return 0xFFFFFFFF;
}
W32 BOOL TlsFree(DWORD i) { std::lock_guard<std::mutex> l(g_tls_mx); if (i >= kTlsSlots) return FALSE_; g_tls_used[i] = false; return TRUE_; }
W32 LPVOID TlsGetValue(DWORD i) { set_err(0); return i < kTlsSlots ? t_tls[i] : nullptr; }
W32 BOOL TlsSetValue(DWORD i, LPVOID v) { if (i >= kTlsSlots) return FALSE_; t_tls[i] = v; return TRUE_; }
W32 DWORD FlsAlloc(void* cb) {
    std::lock_guard<std::mutex> l(g_tls_mx);
    for (int i = 0; i < kTlsSlots; i++) if (!g_fls_used[i]) { g_fls_used[i] = true; g_fls_cb[i] = cb; return i; }
    return 0xFFFFFFFF;
}
W32 BOOL FlsFree(DWORD i) { std::lock_guard<std::mutex> l(g_tls_mx); if (i >= kTlsSlots) return FALSE_; g_fls_used[i] = false; g_fls_cb[i] = nullptr; return TRUE_; }
W32 LPVOID FlsGetValue(DWORD i) { set_err(0); return i < kTlsSlots ? t_fls[i] : nullptr; }
W32 BOOL FlsSetValue(DWORD i, LPVOID v) { if (i >= kTlsSlots) return FALSE_; t_fls[i] = v; return TRUE_; }

// --- threads ---
using ThreadProc = DWORD (W32*)(void*);
W32 HANDLE CreateThread(void*, SIZE_T, ThreadProc start, void* param, DWORD flags, DWORD* tid_out) {
    auto t = std::make_shared<Thread>();
    t->suspended = flags & 0x4;   // CREATE_SUSPENDED
    HANDLE h = add_handle(t);
    std::mutex id_mx; std::condition_variable id_cv; bool have_id = false;
    std::thread([t, start, param, &id_mx, &id_cv, &have_id] {
        void* teb = attach_current_thread();
        t_self = t; t->tid = plat::thread_id();
        { std::lock_guard<std::mutex> l(id_mx); have_id = true; } id_cv.notify_one();
        { std::unique_lock<std::mutex> l(t->start_mx); t->start_cv.wait(l, [&] { return !t->suspended; }); }
        if (g_h.thread_attach) g_h.thread_attach(teb);
        DWORD rc = start(param);
        run_fls_callbacks();
        if (g_h.thread_detach) g_h.thread_detach(teb);
        { std::lock_guard<std::mutex> l(g_wait_mx); t->exit_code = rc; t->done = true; }
        g_wait_cv.notify_all();
    }).detach();
    { std::unique_lock<std::mutex> l(id_mx); id_cv.wait(l, [&] { return have_id; }); }
    if (tid_out) *tid_out = (DWORD)t->tid;
    return h;
}
W32 DWORD ResumeThread(HANDLE h) {
    auto t = get_as<Thread>(h); if (!t) { set_err(E_INVALID_HANDLE); return 0xFFFFFFFF; }
    { std::lock_guard<std::mutex> l(t->start_mx); if (!t->suspended) return 0; t->suspended = false; }
    t->start_cv.notify_all(); return 1;
}
W32 DWORD SuspendThread(HANDLE) { set_err(E_NOT_SUPPORTED); return 0xFFFFFFFF; }
W32 BOOL GetExitCodeThread(HANDLE h, DWORD* code) {
    auto t = get_as<Thread>(h); if (!t) { set_err(E_INVALID_HANDLE); return FALSE_; }
    std::lock_guard<std::mutex> l(g_wait_mx); *code = t->exit_code; return TRUE_;
}
W32 HANDLE GetCurrentThread() { return (HANDLE)(intptr_t)-2; }
W32 DWORD GetCurrentThreadId() { return (DWORD)plat::thread_id(); }
W32 HANDLE GetCurrentProcess() { return (HANDLE)(intptr_t)-1; }
W32 DWORD GetCurrentProcessId() { return 4242; }
W32 DWORD GetCurrentProcessorNumber() { return 0; }
W32 void Sleep(DWORD ms) { if (ms == 0) plat::thread_yield(); else plat::sleep_ms(ms); }
W32 BOOL SwitchToThread() { plat::thread_yield(); return TRUE_; }
W32 int GetThreadPriority(HANDLE) { return 0; }
W32 BOOL SetThreadPriority(HANDLE, int) { return TRUE_; }
W32 ULONG_PTR SetThreadAffinityMask(HANDLE, ULONG_PTR) { return (1ull << plat::cpu_count()) - 1; }
W32 BOOL GetProcessAffinityMask(HANDLE, ULONG_PTR* p, ULONG_PTR* s) { *p = *s = (1ull << plat::cpu_count()) - 1; return TRUE_; }
W32 BOOL SetProcessAffinityMask(HANDLE, ULONG_PTR) { return TRUE_; }
W32 BOOL GetThreadContext(HANDLE, void*) { set_err(E_NOT_SUPPORTED); return FALSE_; }
W32 BOOL SetThreadContext(HANDLE, const void*) { set_err(E_NOT_SUPPORTED); return FALSE_; }
W32 void ExitThread(DWORD code) {
    log("gdps4: ExitThread(%u) is not supported (thread keeps running)\n", code);
    for (;;) plat::sleep_ms(1000);
}
W32 void FreeLibraryAndExitThread(void*, DWORD code) { ExitThread(code); }
W32 void FlushProcessWriteBuffers() { std::atomic_thread_fence(std::memory_order_seq_cst); }

// --- thread pool (one std::thread per callback; enough for msvcp's needs) ---
struct PoolWork { void* cb; void* ctx; };
struct PoolTimer { void* cb; void* ctx; std::atomic<uint64_t> gen{0}; };
struct PoolWait { void* cb; void* ctx; std::atomic<uint64_t> gen{0}; };
W32 void* CreateThreadpoolWork(void* cb, void* ctx, void*) { return new PoolWork{cb, ctx}; }
W32 void SubmitThreadpoolWork(PoolWork* w) {
    std::thread([w] { attach_current_thread(); ((void (W32*)(void*, void*, void*))w->cb)(nullptr, w->ctx, w); }).detach();
}
W32 void CloseThreadpoolWork(PoolWork*) {}   // may still be running; leaked deliberately
W32 void* CreateThreadpoolTimer(void* cb, void* ctx, void*) { auto* t = new PoolTimer; t->cb = cb; t->ctx = ctx; return t; }
W32 void SetThreadpoolTimer(PoolTimer* t, const FILETIME* due, DWORD period, DWORD) {
    uint64_t g = ++t->gen;
    if (!due) return;   // NULL cancels
    int64_t d = (int64_t)get_ft(due);
    int64_t delay_ms = d < 0 ? -d / 10000 : std::max<int64_t>(0, (d - (int64_t)now_filetime()) / 10000);
    std::thread([t, g, delay_ms, period] {
        attach_current_thread();
        plat::sleep_ms((uint32_t)delay_ms);
        do {
            if (t->gen != g) return;
            ((void (W32*)(void*, void*, void*))t->cb)(nullptr, t->ctx, t);
            if (period) plat::sleep_ms(period);
        } while (period);
    }).detach();
}
W32 void WaitForThreadpoolTimerCallbacks(PoolTimer* t, BOOL cancel) { if (cancel) ++t->gen; }
W32 void CloseThreadpoolTimer(PoolTimer* t) { ++t->gen; }
W32 void* CreateThreadpoolWait(void* cb, void* ctx, void*) { auto* w = new PoolWait; w->cb = cb; w->ctx = ctx; return w; }
W32 void SetThreadpoolWait(PoolWait* w, HANDLE h, const FILETIME* timeout) {
    uint64_t g = ++w->gen;
    if (!h) return;
    DWORD ms = 0xFFFFFFFF;
    if (timeout) { int64_t d = (int64_t)get_ft(timeout); ms = (DWORD)(d < 0 ? -d / 10000 : 0); }
    std::thread([w, g, h, ms] {
        attach_current_thread();
        DWORD r = WaitForSingleObject(h, ms);
        if (w->gen == g) ((void (W32*)(void*, void*, void*, DWORD))w->cb)(nullptr, w->ctx, w, r);
    }).detach();
}
W32 void CloseThreadpoolWait(PoolWait* w) { ++w->gen; }
W32 void FreeLibraryWhenCallbackReturns(void*, void*) {}

// --- time ---
W32 BOOL QueryPerformanceCounter(int64_t* c) { *c = (int64_t)(plat::monotonic_ns() / 100); return TRUE_; }
W32 BOOL QueryPerformanceFrequency(int64_t* f) { *f = 10000000; return TRUE_; }
W32 DWORD GetTickCount() { return (DWORD)(plat::monotonic_ns() / 1000000); }
W32 ULONGLONG GetTickCount64() { return plat::monotonic_ns() / 1000000; }
W32 void GetSystemTimeAsFileTime(FILETIME* f) { put_ft(f, now_filetime()); }
W32 void GetSystemTimePreciseAsFileTime(FILETIME* f) { put_ft(f, now_filetime()); }
W32 void GetSystemTime(SYSTEMTIME* st) { ft_to_st(now_filetime(), st); }
W32 void GetLocalTime(SYSTEMTIME* st) { ft_to_st(now_filetime(), st); }   // time zone: UTC
W32 BOOL SetLocalTime(const SYSTEMTIME*) { set_err(E_ACCESS_DENIED); return FALSE_; }
W32 BOOL FileTimeToSystemTime(const FILETIME* f, SYSTEMTIME* st) { ft_to_st(get_ft(f), st); return TRUE_; }
W32 BOOL SystemTimeToFileTime(const SYSTEMTIME* st, FILETIME* f) { put_ft(f, st_to_ft(st)); return TRUE_; }
W32 DWORD GetTimeZoneInformation(void* tzi) { memset(tzi, 0, 172); return 0; /*TIME_ZONE_ID_UNKNOWN*/ }
W32 BOOL SystemTimeToTzSpecificLocalTime(const void*, const SYSTEMTIME* u, SYSTEMTIME* l) { *l = *u; return TRUE_; }
W32 BOOL TzSpecificLocalTimeToSystemTime(const void*, const SYSTEMTIME* l, SYSTEMTIME* u) { *u = *l; return TRUE_; }

// --- files ---
DWORD attrs_of(const plat::FileStat& st) { return st.is_dir ? 0x10 : 0x20; }
uint64_t ft_of(const plat::FileStat& st) { return (uint64_t)(st.mtime_unix_ns / 100 + kEpochDiff); }
W32 HANDLE CreateFileW(const WCHAR* name, DWORD access, DWORD, void*, DWORD disp, DWORD, HANDLE) {
    std::string path = host_path(name);
    plat::FileStat st = plat::stat(path);
    bool write = access & (0x40000000 | 0x10000000 | 0x2 | 0x4 | 0x100);   // GENERIC_WRITE/ALL, FILE_WRITE_DATA, APPEND, WRITE_ATTRIBUTES
    bool read = (access & (0x80000000 | 0x10000000 | 0x1)) || !write;
    if (st.is_dir) {   // FILE_FLAG_BACKUP_SEMANTICS directory handles
        auto f = std::make_shared<File>(); f->is_dir = true; f->path = path;
        return add_handle(f);
    }
    bool create = disp == 1 || disp == 2 || disp == 4, trunc = disp == 2 || disp == 5, excl = disp == 1;
    if (disp == 3 || disp == 5) create = false;
    intptr_t fd = plat::file_open(path, read, write || trunc, create, trunc, excl);
    if (fd < 0) {
        int e = plat::last_errno();
        set_err(e == EEXIST ? E_FILE_EXISTS : e == ENOENT && !plat::stat(path.substr(0, path.find_last_of("/\\"))).exists
                                                  ? E_PATH_NOT_FOUND : err_from_errno(e));
        return INVALID_HANDLE;
    }
    auto f = std::make_shared<File>(); f->fd = fd; f->path = path;
    set_err((disp == 2 || disp == 4) && st.exists ? E_ALREADY_EXISTS : 0);
    return add_handle(f);
}
W32 HANDLE CreateFileA(const char* name, DWORD a, DWORD s, void* sa, DWORD d, DWORD f, HANDLE t) {
    return CreateFileW(ansi_to_w(name).c_str(), a, s, sa, d, f, t);
}
struct CREATEFILE2_PARAMS { DWORD size, attrs, flags, sqos; void* sa; HANDLE tmpl; };
W32 HANDLE CreateFile2(const WCHAR* name, DWORD access, DWORD share, DWORD disp, const CREATEFILE2_PARAMS* p) {
    return CreateFileW(name, access, share, p ? p->sa : nullptr, disp, p ? p->attrs | p->flags : 0, nullptr);
}
struct OVERLAPPED { ULONG_PTR internal, internal_high; DWORD offset, offset_high; HANDLE event; };
W32 BOOL ReadFile(HANDLE h, void* buf, DWORD n, DWORD* got, OVERLAPPED* ov) {
    auto f = get_as<File>(h); if (!f || f->fd < 0) { set_err(E_INVALID_HANDLE); return FALSE_; }
    if (f->console) { if (got) *got = 0; return TRUE_; }
    if (ov) plat::file_seek(f->fd, ((int64_t)ov->offset_high << 32) | ov->offset, 0);
    int64_t r = plat::file_read(f->fd, buf, n);
    if (r < 0) { set_err(err_from_errno(plat::last_errno())); return FALSE_; }
    if (got) *got = (DWORD)r;
    if (ov && r == 0 && n) { set_err(E_HANDLE_EOF); return FALSE_; }
    return TRUE_;
}
W32 BOOL WriteFile(HANDLE h, const void* buf, DWORD n, DWORD* put, OVERLAPPED* ov) {
    auto f = get_as<File>(h); if (!f) { set_err(E_INVALID_HANDLE); return FALSE_; }
    if (f->console) {
        std::string s((const char*)buf, n); plat::debug_log(s.c_str());
        if (put) *put = n; return TRUE_;
    }
    if (f->fd < 0) { set_err(E_INVALID_HANDLE); return FALSE_; }
    if (ov) plat::file_seek(f->fd, ((int64_t)ov->offset_high << 32) | ov->offset, 0);
    int64_t r = plat::file_write(f->fd, buf, n);
    if (r < 0) { set_err(err_from_errno(plat::last_errno())); return FALSE_; }
    if (put) *put = (DWORD)r;
    return TRUE_;
}
W32 BOOL SetFilePointerEx(HANDLE h, int64_t dist, int64_t* newpos, DWORD method) {
    auto f = get_as<File>(h); if (!f || f->fd < 0) { set_err(E_INVALID_HANDLE); return FALSE_; }
    int64_t r = plat::file_seek(f->fd, dist, (int)method);
    if (r < 0) { set_err(E_INVALID_PARAMETER); return FALSE_; }
    if (newpos) *newpos = r;
    return TRUE_;
}
W32 DWORD SetFilePointer(HANDLE h, LONG lo, LONG* hi, DWORD method) {
    int64_t d = hi ? ((int64_t)*hi << 32) | (uint32_t)lo : (int64_t)lo, np;
    if (!SetFilePointerEx(h, d, &np, method)) return 0xFFFFFFFF;
    if (hi) *hi = (LONG)(np >> 32);
    set_err(0);
    return (DWORD)np;
}
W32 BOOL GetFileSizeEx(HANDLE h, int64_t* size) {
    auto f = get_as<File>(h); if (!f) { set_err(E_INVALID_HANDLE); return FALSE_; }
    *size = (int64_t)plat::stat(f->path).size; return TRUE_;
}
W32 DWORD GetFileSize(HANDLE h, DWORD* hi) {
    int64_t s; if (!GetFileSizeEx(h, &s)) return 0xFFFFFFFF;
    if (hi) *hi = (DWORD)(s >> 32);
    return (DWORD)s;
}
W32 DWORD GetFileType(HANDLE h) {
    auto f = get_as<File>(h);
    if (!f) { set_err(E_INVALID_HANDLE); return 0; }
    return f->console ? 2 /*CHAR*/ : 1 /*DISK*/;
}
W32 BOOL FlushFileBuffers(HANDLE) { return TRUE_; }
W32 BOOL SetEndOfFile(HANDLE h) {
    auto f = get_as<File>(h); if (!f || f->fd < 0) { set_err(E_INVALID_HANDLE); return FALSE_; }
    return plat::file_truncate(f->fd, (uint64_t)plat::file_seek(f->fd, 0, 1));
}
struct BY_HANDLE_FILE_INFORMATION { DWORD attrs; FILETIME c, a, w; DWORD vol, size_hi, size_lo, links, idx_hi, idx_lo; };
W32 BOOL GetFileInformationByHandle(HANDLE h, BY_HANDLE_FILE_INFORMATION* fi) {
    auto f = get_as<File>(h); if (!f) { set_err(E_INVALID_HANDLE); return FALSE_; }
    auto st = plat::stat(f->path);
    memset(fi, 0, sizeof *fi);
    fi->attrs = attrs_of(st); put_ft(&fi->c, ft_of(st)); fi->a = fi->w = fi->c;
    fi->vol = 0x4744; fi->size_hi = (DWORD)(st.size >> 32); fi->size_lo = (DWORD)st.size; fi->links = 1;
    uint64_t idx = std::hash<std::string>()(f->path); fi->idx_hi = (DWORD)(idx >> 32); fi->idx_lo = (DWORD)idx;
    return TRUE_;
}
W32 BOOL GetFileInformationByHandleEx(HANDLE h, int cls, void* out, DWORD size) {
    auto f = get_as<File>(h); if (!f) { set_err(E_INVALID_HANDLE); return FALSE_; }
    auto st = plat::stat(f->path);
    if (cls == 0 && size >= 40) {   // FILE_BASIC_INFO
        auto* p = (int64_t*)out; p[0] = p[1] = p[2] = p[3] = (int64_t)ft_of(st);
        ((DWORD*)out)[8] = attrs_of(st); return TRUE_;
    }
    if (cls == 1 && size >= 24) {   // FILE_STANDARD_INFO
        auto* p = (int64_t*)out; p[0] = (int64_t)((st.size + 4095) & ~4095ull); p[1] = (int64_t)st.size;
        ((DWORD*)out)[4] = 1; ((uint8_t*)out)[20] = 0; ((uint8_t*)out)[21] = st.is_dir; return TRUE_;
    }
    set_err(E_INVALID_PARAMETER); return FALSE_;
}
W32 BOOL SetFileInformationByHandle(HANDLE h, int cls, void* in, DWORD) {
    auto f = get_as<File>(h); if (!f) { set_err(E_INVALID_HANDLE); return FALSE_; }
    if (cls == 6 /*FileEndOfFileInfo*/ && f->fd >= 0) return plat::file_truncate(f->fd, *(uint64_t*)in);
    if (cls == 0 /*FileBasicInfo*/) return TRUE_;
    set_err(E_INVALID_PARAMETER); return FALSE_;
}
W32 BOOL SetFileTime(HANDLE, const FILETIME*, const FILETIME*, const FILETIME*) { return TRUE_; }
W32 BOOL LockFile(HANDLE, DWORD, DWORD, DWORD, DWORD) { return TRUE_; }
W32 BOOL UnlockFile(HANDLE, DWORD, DWORD, DWORD, DWORD) { return TRUE_; }
W32 BOOL LockFileEx(HANDLE, DWORD, DWORD, DWORD, DWORD, void*) { return TRUE_; }
W32 BOOL UnlockFileEx(HANDLE, DWORD, DWORD, DWORD, void*) { return TRUE_; }
W32 DWORD GetFileAttributesW(const WCHAR* name) {
    auto st = plat::stat(host_path(name));
    if (!st.exists) { set_err(E_FILE_NOT_FOUND); return 0xFFFFFFFF; }
    return attrs_of(st);
}
struct WIN32_FILE_ATTRIBUTE_DATA { DWORD attrs; FILETIME c, a, w; DWORD size_hi, size_lo; };
W32 BOOL GetFileAttributesExW(const WCHAR* name, int, WIN32_FILE_ATTRIBUTE_DATA* d) {
    auto st = plat::stat(host_path(name));
    if (!st.exists) { set_err(E_FILE_NOT_FOUND); return FALSE_; }
    d->attrs = attrs_of(st); put_ft(&d->c, ft_of(st)); d->a = d->w = d->c;
    d->size_hi = (DWORD)(st.size >> 32); d->size_lo = (DWORD)st.size;
    return TRUE_;
}
W32 BOOL SetFileAttributesW(const WCHAR*, DWORD) { return TRUE_; }
W32 BOOL CreateDirectoryW(const WCHAR* name, void*) {
    std::string p = host_path(name);
    if (plat::stat(p).exists) { set_err(E_ALREADY_EXISTS); return FALSE_; }
    if (!plat::make_dir(p)) { set_err(err_from_errno(plat::last_errno())); return FALSE_; }
    return TRUE_;
}
W32 BOOL CreateDirectoryA(const char* name, void* sa) { return CreateDirectoryW(ansi_to_w(name).c_str(), sa); }
W32 BOOL RemoveDirectoryW(const WCHAR* name) {
    if (!plat::remove_dir(host_path(name))) { set_err(err_from_errno(plat::last_errno())); return FALSE_; }
    return TRUE_;
}
W32 BOOL DeleteFileW(const WCHAR* name) {
    if (!plat::remove_file(host_path(name))) { set_err(err_from_errno(plat::last_errno())); return FALSE_; }
    return TRUE_;
}
W32 BOOL DeleteFileA(const char* name) { return DeleteFileW(ansi_to_w(name).c_str()); }
W32 BOOL MoveFileExW(const WCHAR* a, const WCHAR* b, DWORD) {
    if (!plat::rename_path(host_path(a), host_path(b))) { set_err(err_from_errno(plat::last_errno())); return FALSE_; }
    return TRUE_;
}
W32 HRESULT CopyFile2(const WCHAR* a, const WCHAR* b, void*) {
    intptr_t in = plat::file_open(host_path(a), true, false, false, false, false);
    if (in < 0) return (HRESULT)0x80070002;
    intptr_t out = plat::file_open(host_path(b), false, true, true, true, false);
    if (out < 0) { plat::file_close(in); return (HRESULT)0x80070005; }
    char buf[65536]; int64_t n;
    while ((n = plat::file_read(in, buf, sizeof buf)) > 0) plat::file_write(out, buf, (size_t)n);
    plat::file_close(in); plat::file_close(out);
    return 0;
}
W32 BOOL CreateHardLinkW(const WCHAR*, const WCHAR*, void*) { set_err(E_NOT_SUPPORTED); return FALSE_; }
W32 BOOL CreateSymbolicLinkW(const WCHAR*, const WCHAR*, DWORD) { set_err(E_NOT_SUPPORTED); return FALSE_; }
W32 DWORD GetFullPathNameW(const WCHAR* name, DWORD n, WCHAR* buf, WCHAR** filepart) {
    std::u16string full = normalize(name);
    if (full.size() + 1 > n) return (DWORD)full.size() + 1;
    memcpy(buf, full.c_str(), (full.size() + 1) * 2);
    if (filepart) { auto p = full.find_last_of(u'\\'); *filepart = buf + (p == std::u16string::npos ? 0 : p + 1); }
    return (DWORD)full.size();
}
W32 DWORD GetFullPathNameA(const char* name, DWORD n, char* buf, char** filepart) {
    std::u16string full = normalize(ansi_to_w(name));
    std::string a = w_to_ansi(full.c_str(), full.size(), 0, nullptr);
    if (a.size() + 1 > n) return (DWORD)a.size() + 1;
    memcpy(buf, a.c_str(), a.size() + 1);
    if (filepart) { auto p = a.find_last_of('\\'); *filepart = buf + (p == std::string::npos ? 0 : p + 1); }
    return (DWORD)a.size();
}
W32 DWORD GetCurrentDirectoryW(DWORD n, WCHAR* buf) {
    std::lock_guard<std::mutex> l(g_cwd_mx);
    if (g_cwd.size() + 1 > n) return (DWORD)g_cwd.size() + 1;
    memcpy(buf, g_cwd.c_str(), (g_cwd.size() + 1) * 2);
    return (DWORD)g_cwd.size();
}
W32 BOOL SetCurrentDirectoryW(const WCHAR* p) {
    std::u16string full = normalize(p);
    if (!plat::stat(host_path(full.c_str())).is_dir) { set_err(E_PATH_NOT_FOUND); return FALSE_; }
    while (full.size() > 3 && full.back() == u'\\') full.pop_back();
    std::lock_guard<std::mutex> l(g_cwd_mx); g_cwd = full; return TRUE_;
}
W32 DWORD GetTempPathW(DWORD n, WCHAR* buf) {
    const auto& t = g_h.temp_dir;
    if (t.size() + 1 > n) return (DWORD)t.size() + 1;
    memcpy(buf, t.c_str(), (t.size() + 1) * 2); return (DWORD)t.size();
}
W32 DWORD GetTempPathA(DWORD n, char* buf) {
    std::string a = w_to_ansi(g_h.temp_dir.c_str(), g_h.temp_dir.size(), 0, nullptr);
    if (a.size() + 1 > n) return (DWORD)a.size() + 1;
    memcpy(buf, a.c_str(), a.size() + 1); return (DWORD)a.size();
}
W32 UINT GetSystemDirectoryA(char* buf, UINT n) {
    const char s[] = "C:\\Windows\\System32";
    if (sizeof s > n) return sizeof s;
    memcpy(buf, s, sizeof s); return sizeof s - 1;
}
W32 BOOL GetDiskFreeSpaceExW(const WCHAR*, uint64_t* avail, uint64_t* total, uint64_t* free_) {
    if (avail) *avail = 16ull << 30; if (total) *total = 64ull << 30; if (free_) *free_ = 16ull << 30; return TRUE_;
}
W32 BOOL GetDiskFreeSpaceW(const WCHAR*, DWORD* spc, DWORD* bps, DWORD* nfree, DWORD* ntotal) {
    if (spc) *spc = 8; if (bps) *bps = 512; if (nfree) *nfree = 4000000; if (ntotal) *ntotal = 16000000; return TRUE_;
}
W32 BOOL GetDiskFreeSpaceA(const char*, DWORD* a, DWORD* b, DWORD* c, DWORD* d) { return GetDiskFreeSpaceW(nullptr, a, b, c, d); }
W32 UINT GetDriveTypeW(const WCHAR*) { return 3; /*DRIVE_FIXED*/ }
W32 DWORD GetLogicalDrives() { return 1u << 2; /*C:*/ }
W32 BOOL AreFileApisANSI() { return TRUE_; }

// --- directory search ---
bool wild_match(const char* pat, const char* s) {   // * and ?, case-insensitive
    if (!*pat) return !*s;
    if (*pat == '*') return wild_match(pat + 1, s) || (*s && wild_match(pat, s + 1));
    if (!*s) return false;
    if (*pat == '?' || tolower((unsigned char)*pat) == tolower((unsigned char)*s)) return wild_match(pat + 1, s + 1);
    return false;
}
struct WIN32_FIND_DATAW { DWORD attrs; FILETIME c, a, w; DWORD size_hi, size_lo, r0, r1; WCHAR name[260]; WCHAR alt[14]; };
bool fill_find(FindData& fd, WIN32_FIND_DATAW* out) {
    if (fd.next >= fd.names.size()) return false;
    const std::string& n = fd.names[fd.next++];
    auto st = plat::stat(fd.dir + "/" + n);
    memset(out, 0, sizeof *out);
    out->attrs = attrs_of(st); put_ft(&out->c, ft_of(st)); out->a = out->w = out->c;
    out->size_hi = (DWORD)(st.size >> 32); out->size_lo = (DWORD)st.size;
    std::u16string w = from_utf8(n.c_str());
    memcpy(out->name, w.c_str(), std::min<size_t>(w.size(), 259) * 2);
    return true;
}
W32 HANDLE FindFirstFileExW(const WCHAR* pattern, int, WIN32_FIND_DATAW* out, int, void*, DWORD) {
    std::u16string full = normalize(pattern);
    auto slash = full.find_last_of(u'\\');
    std::u16string dir = full.substr(0, slash), pat = full.substr(slash + 1);
    if (dir.size() == 2) dir += u"\\";
    auto fd = std::make_shared<FindData>();
    fd->dir = host_path(dir.c_str());
    std::string p8 = to_utf8(pat.c_str());
    if (p8 == "*.*") p8 = "*";
    for (auto& n : plat::list_dir(fd->dir)) if (wild_match(p8.c_str(), n.c_str())) fd->names.push_back(n);
    if (p8 == "*") { fd->names.insert(fd->names.begin(), ".."); fd->names.insert(fd->names.begin(), "."); }
    if (!fill_find(*fd, out)) { set_err(plat::stat(fd->dir).is_dir ? E_FILE_NOT_FOUND : E_PATH_NOT_FOUND); return INVALID_HANDLE; }
    return add_handle(fd);
}
W32 BOOL FindNextFileW(HANDLE h, WIN32_FIND_DATAW* out) {
    auto fd = get_as<FindData>(h); if (!fd) { set_err(E_INVALID_HANDLE); return FALSE_; }
    if (!fill_find(*fd, out)) { set_err(E_NO_MORE_FILES); return FALSE_; }
    return TRUE_;
}
W32 BOOL FindClose(HANDLE h) { return CloseHandle(h); }

// --- file mapping (read-only snapshot; enough for loading data files) ---
W32 HANDLE CreateFileMappingW(HANDLE file, void*, DWORD, DWORD hi, DWORD lo, const WCHAR*) {
    auto m = std::make_shared<Mapping>();
    m->file = get_obj(file);
    m->size = ((uint64_t)hi << 32) | lo;
    if (!m->size) { int64_t s; if (GetFileSizeEx(file, &s)) m->size = (uint64_t)s; }
    return add_handle(m);
}
W32 HANDLE CreateFileMappingA(HANDLE file, void* sa, DWORD prot, DWORD hi, DWORD lo, const char*) {
    return CreateFileMappingW(file, sa, prot, hi, lo, nullptr);
}
std::mutex g_views_mx;
std::map<void*, size_t> g_views;
W32 LPVOID MapViewOfFile(HANDLE mh, DWORD, DWORD off_hi, DWORD off_lo, SIZE_T n) {
    auto m = get_as<Mapping>(mh); if (!m) { set_err(E_INVALID_HANDLE); return nullptr; }
    auto f = std::dynamic_pointer_cast<File>(m->file);
    uint64_t off = ((uint64_t)off_hi << 32) | off_lo;
    if (!n) n = (SIZE_T)(m->size - off);
    void* mem = calloc(1, n ? n : 1);
    if (f && f->fd >= 0) {
        int64_t pos = plat::file_seek(f->fd, 0, 1);
        plat::file_seek(f->fd, (int64_t)off, 0); plat::file_read(f->fd, mem, n); plat::file_seek(f->fd, pos, 0);
    }
    std::lock_guard<std::mutex> l(g_views_mx); g_views[mem] = n;
    return mem;
}
W32 BOOL UnmapViewOfFile(const void* p) {
    std::lock_guard<std::mutex> l(g_views_mx);
    auto it = g_views.find((void*)p); if (it == g_views.end()) return FALSE_;
    free(it->first); g_views.erase(it); return TRUE_;
}
W32 BOOL FlushViewOfFile(const void*, SIZE_T) { return TRUE_; }

// --- pipes / processes: not available on PS4 ---
W32 BOOL CreatePipe(HANDLE*, HANDLE*, void*, DWORD) { set_err(E_NOT_SUPPORTED); return FALSE_; }
W32 BOOL PeekNamedPipe(HANDLE, void*, DWORD, DWORD*, DWORD*, DWORD*) { set_err(E_NOT_SUPPORTED); return FALSE_; }
W32 BOOL CreateProcessW(const WCHAR*, WCHAR*, void*, void*, BOOL, DWORD, void*, const WCHAR*, void*, void*) { set_err(E_NOT_SUPPORTED); return FALSE_; }
W32 HANDLE OpenProcess(DWORD, BOOL, DWORD) { set_err(E_ACCESS_DENIED); return nullptr; }
W32 BOOL GetExitCodeProcess(HANDLE, DWORD* c) { *c = 259; return TRUE_; }
W32 void ExitProcess(UINT code) { plat::exit_process((int)code); }
W32 BOOL TerminateProcess(HANDLE h, UINT code) {
    if (h == (HANDLE)(intptr_t)-1) plat::exit_process((int)code);
    set_err(E_ACCESS_DENIED); return FALSE_;
}

// --- console (no console on PS4; std handles go to the debug log) ---
std::shared_ptr<File> g_std[3];
W32 HANDLE GetStdHandle(DWORD which) {
    int i = which == (DWORD)-10 ? 0 : which == (DWORD)-11 ? 1 : which == (DWORD)-12 ? 2 : -1;
    if (i < 0) { set_err(E_INVALID_PARAMETER); return INVALID_HANDLE; }
    static HANDLE hs[3];
    static std::once_flag once;
    std::call_once(once, [] {
        for (int k = 0; k < 3; k++) { g_std[k] = std::make_shared<File>(); g_std[k]->console = true; g_std[k]->std_no = k; hs[k] = add_handle(g_std[k]); }
    });
    return hs[i];
}
W32 BOOL SetStdHandle(DWORD, HANDLE) { return TRUE_; }
W32 UINT GetConsoleCP() { return CP_UTF8_; }
W32 UINT GetConsoleOutputCP() { return CP_UTF8_; }
W32 BOOL GetConsoleMode(HANDLE, DWORD*) { set_err(E_INVALID_HANDLE); return FALSE_; }
W32 BOOL SetConsoleMode(HANDLE, DWORD) { set_err(E_INVALID_HANDLE); return FALSE_; }
W32 BOOL WriteConsoleW(HANDLE, const WCHAR* s, DWORD n, DWORD* put, void*) {
    plat::debug_log(to_utf8(s, n).c_str()); if (put) *put = n; return TRUE_;
}
W32 BOOL ReadConsoleW(HANDLE, void*, DWORD, DWORD* got, void*) { if (got) *got = 0; set_err(E_INVALID_HANDLE); return FALSE_; }
W32 BOOL ReadConsoleInputW(HANDLE, void*, DWORD, DWORD* got) { if (got) *got = 0; set_err(E_INVALID_HANDLE); return FALSE_; }
W32 BOOL PeekConsoleInputA(HANDLE, void*, DWORD, DWORD* got) { if (got) *got = 0; set_err(E_INVALID_HANDLE); return FALSE_; }
W32 BOOL GetNumberOfConsoleInputEvents(HANDLE, DWORD* n) { if (n) *n = 0; set_err(E_INVALID_HANDLE); return FALSE_; }
W32 BOOL SetConsoleCtrlHandler(void*, BOOL) { return TRUE_; }
W32 BOOL Beep(DWORD, DWORD) { return TRUE_; }

// --- strings / code pages ---
W32 UINT GetACP() { return kACP; }
W32 UINT GetOEMCP() { return kOEMCP; }
W32 BOOL IsValidCodePage(UINT cp) { cp = real_cp(cp); return cp == 1252 || cp == 437 || cp == CP_UTF8_ || cp == 20127 || cp == 28591; }
struct CPINFO { UINT max_char_size; BYTE default_char[2]; BYTE lead_byte[12]; };
W32 BOOL GetCPInfo(UINT cp, CPINFO* ci) {
    if (!IsValidCodePage(cp)) { set_err(E_INVALID_PARAMETER); return FALSE_; }
    memset(ci, 0, sizeof *ci);
    ci->max_char_size = real_cp(cp) == CP_UTF8_ ? 4 : 1; ci->default_char[0] = '?';
    return TRUE_;
}
W32 int MultiByteToWideChar(UINT cp, DWORD, const char* src, int n, WCHAR* dst, int cap) {
    size_t len = n < 0 ? strlen(src) + 1 : (size_t)n;
    std::u16string w = ansi_to_w(src, len, cp);
    if (cap == 0) return (int)w.size();
    if ((int)w.size() > cap) { set_err(E_INSUFFICIENT_BUFFER); return 0; }
    memcpy(dst, w.data(), w.size() * 2);
    return (int)w.size();
}
W32 int WideCharToMultiByte(UINT cp, DWORD, const WCHAR* src, int n, char* dst, int cap, const char*, BOOL* used_default) {
    size_t len = n < 0 ? wlen(src) + 1 : (size_t)n;
    bool ud = false;
    std::string a = w_to_ansi(src, len, cp, &ud);
    if (used_default) *used_default = ud;
    if (cap == 0) return (int)a.size();
    if ((int)a.size() > cap) { set_err(E_INSUFFICIENT_BUFFER); return 0; }
    memcpy(dst, a.data(), a.size());
    return (int)a.size();
}
char16_t up16(char16_t c) { return c < 0xD800 ? (char16_t)towupper(c) : c; }
char16_t lo16(char16_t c) { return c < 0xD800 ? (char16_t)towlower(c) : c; }
W32 int LCMapStringEx(const WCHAR*, DWORD flags, const WCHAR* src, int n, WCHAR* dst, int cap, void*, void*, intptr_t) {
    size_t len = n < 0 ? wlen(src) + 1 : (size_t)n;
    if (flags & 0x400 /*LCMAP_SORTKEY*/) {   // byte sort key: code units, big-endian, plus terminator
        size_t need = len * 2 + 1;
        if (cap == 0) return (int)need;
        if ((int)need > cap) { set_err(E_INSUFFICIENT_BUFFER); return 0; }
        auto* b = (uint8_t*)dst;
        for (size_t i = 0; i < len; i++) { b[2 * i] = (uint8_t)(src[i] >> 8); b[2 * i + 1] = (uint8_t)src[i]; }
        b[len * 2] = 0;
        return (int)need;
    }
    if (cap == 0) return (int)len;
    if ((int)len > cap) { set_err(E_INSUFFICIENT_BUFFER); return 0; }
    for (size_t i = 0; i < len; i++)
        dst[i] = (flags & 0x200) ? up16(src[i]) : (flags & 0x100) ? lo16(src[i]) : src[i];
    return (int)len;
}
W32 int LCMapStringW(DWORD, DWORD flags, const WCHAR* src, int n, WCHAR* dst, int cap) {
    return LCMapStringEx(nullptr, flags, src, n, dst, cap, nullptr, nullptr, 0);
}
W32 int CompareStringEx(const WCHAR*, DWORD flags, const WCHAR* a, int na, const WCHAR* b, int nb, void*, void*, intptr_t) {
    size_t la = na < 0 ? wlen(a) : (size_t)na, lb = nb < 0 ? wlen(b) : (size_t)nb;
    bool ic = flags & 0x1;   // NORM_IGNORECASE
    for (size_t i = 0; i < la && i < lb; i++) {
        char16_t x = ic ? lo16(a[i]) : a[i], y = ic ? lo16(b[i]) : b[i];
        if (x != y) return x < y ? 1 : 3;
    }
    return la == lb ? 2 : la < lb ? 1 : 3;
}
W32 int CompareStringW(DWORD, DWORD flags, const WCHAR* a, int na, const WCHAR* b, int nb) {
    return CompareStringEx(nullptr, flags, a, na, b, nb, nullptr, nullptr, 0);
}
W32 BOOL GetStringTypeW(DWORD type, const WCHAR* src, int n, WORD* out) {
    if (type != 1 /*CT_CTYPE1*/) { set_err(E_INVALID_PARAMETER); return FALSE_; }
    size_t len = n < 0 ? wlen(src) + 1 : (size_t)n;
    for (size_t i = 0; i < len; i++) {
        wint_t c = src[i]; WORD t = 0;
        if (c >= 0xD800 && c < 0xE000) { out[i] = 0x200; continue; }
        if (iswupper(c)) t |= 0x1; if (iswlower(c)) t |= 0x2; if (iswdigit(c)) t |= 0x4;
        if (iswspace(c)) t |= 0x8; if (iswpunct(c)) t |= 0x10; if (iswcntrl(c)) t |= 0x20;
        if (c == ' ' || c == '\t') t |= 0x40; if (iswxdigit(c)) t |= 0x80; if (iswalpha(c)) t |= 0x100;
        out[i] = t | 0x200;
    }
    return TRUE_;
}

// --- locale: en-US only ---
const char* const kDays[] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
const char* const kMonths[] = {"January", "February", "March", "April", "May", "June", "July", "August",
                               "September", "October", "November", "December"};
bool locale_value(DWORD type, std::string& v, DWORD& num, bool& is_num) {
    is_num = false;
    DWORD t = type & 0x0FFFFFFF;
    if (t >= 0x2A && t <= 0x30) { v = kDays[t - 0x2A]; return true; }
    if (t >= 0x31 && t <= 0x37) { v = std::string(kDays[t - 0x31]).substr(0, 3); return true; }
    if (t >= 0x38 && t <= 0x43) { v = kMonths[t - 0x38]; return true; }
    if (t >= 0x44 && t <= 0x4F) { v = std::string(kMonths[t - 0x44]).substr(0, 3); return true; }
    switch (t) {
    case 0x01: v = "0409"; num = 0x409; is_num = true; return true;                 // ILANGUAGE
    case 0x02: case 0x1001: v = "English"; return true;                               // SLANGUAGE / SENGLANGUAGE
    case 0x03: v = "ENU"; return true;                                                 // SABBREVLANGNAME
    case 0x06: case 0x1002: v = "United States"; return true;                        // SCOUNTRY / SENGCOUNTRY
    case 0x07: v = "USA"; return true;                                                 // SABBREVCTRYNAME
    case 0x0B: v = "437"; num = 437; is_num = true; return true;                      // IDEFAULTCODEPAGE
    case 0x1004: v = "1252"; num = 1252; is_num = true; return true;                  // IDEFAULTANSICODEPAGE
    case 0x0E: v = "."; return true;                                                   // SDECIMAL
    case 0x0F: v = ","; return true;                                                   // STHOUSAND
    case 0x10: v = "3;0"; return true;                                                 // SGROUPING
    case 0x11: v = "2"; num = 2; is_num = true; return true;                          // IDIGITS
    case 0x14: v = "$"; return true;                                                   // SCURRENCY
    case 0x15: v = "USD"; return true;                                                 // SINTLSYMBOL
    case 0x16: v = "."; return true;                                                   // SMONDECIMALSEP
    case 0x17: v = ","; return true;                                                   // SMONTHOUSANDSEP
    case 0x18: v = "3;0"; return true;                                                 // SMONGROUPING
    case 0x1F: v = "M/d/yyyy"; return true;                                            // SSHORTDATE
    case 0x20: v = "dddd, MMMM d, yyyy"; return true;                                  // SLONGDATE
    case 0x1003: v = "h:mm:ss tt"; return true;                                        // STIMEFORMAT
    case 0x28: v = "AM"; return true;                                                  // S1159
    case 0x29: v = "PM"; return true;                                                  // S2359
    case 0x59: v = "en"; return true;                                                  // SISO639LANGNAME
    case 0x5A: v = "US"; return true;                                                  // SISO3166CTRYNAME
    case 0x5C: v = "en-US"; return true;                                               // SNAME
    case 0x1009: case 0x100C: v = "0"; num = 0; is_num = true; return true;           // ICALENDARTYPE / IFIRSTDAYOFWEEK
    case 0x50: v = "-"; return true;                                                   // SNEGATIVESIGN
    case 0x51: v = ""; return true;                                                    // SPOSITIVESIGN
    }
    return false;
}
W32 int GetLocaleInfoEx(const WCHAR*, DWORD type, WCHAR* buf, int cap) {
    std::string v; DWORD num = 0; bool is_num;
    if (!locale_value(type, v, num, is_num)) { set_err(E_INVALID_FLAGS); return 0; }
    if (type & 0x20000000 /*LOCALE_RETURN_NUMBER*/) {
        if (cap == 0) return 2;
        if (cap < 2) { set_err(E_INSUFFICIENT_BUFFER); return 0; }
        memcpy(buf, &num, 4); return 2;
    }
    std::u16string w = from_utf8(v.c_str());
    if (cap == 0) return (int)w.size() + 1;
    if ((int)w.size() + 1 > cap) { set_err(E_INSUFFICIENT_BUFFER); return 0; }
    memcpy(buf, w.c_str(), (w.size() + 1) * 2);
    return (int)w.size() + 1;
}
W32 int GetLocaleInfoW(DWORD, DWORD type, WCHAR* buf, int cap) { return GetLocaleInfoEx(nullptr, type, buf, cap); }
W32 DWORD GetUserDefaultLCID() { return 0x409; }
W32 BOOL IsValidLocale(DWORD, DWORD) { return TRUE_; }
W32 BOOL EnumSystemLocalesW(BOOL (W32* cb)(WCHAR*), DWORD) { WCHAR s[] = u"00000409"; cb(s); return TRUE_; }
int put_w(const std::string& s, WCHAR* buf, int cap) {
    std::u16string w = from_utf8(s.c_str());
    if (cap == 0) return (int)w.size() + 1;
    if ((int)w.size() + 1 > cap) { set_err(E_INSUFFICIENT_BUFFER); return 0; }
    memcpy(buf, w.c_str(), (w.size() + 1) * 2);
    return (int)w.size() + 1;
}
W32 int GetDateFormatW(DWORD, DWORD, const SYSTEMTIME* st, const WCHAR*, WCHAR* buf, int cap) {
    SYSTEMTIME now; if (!st) { ft_to_st(now_filetime(), &now); st = &now; }
    char b[32]; snprintf(b, sizeof b, "%u/%u/%04u", st->month, st->day, st->year);
    return put_w(b, buf, cap);
}
W32 int GetTimeFormatW(DWORD, DWORD, const SYSTEMTIME* st, const WCHAR*, WCHAR* buf, int cap) {
    SYSTEMTIME now; if (!st) { ft_to_st(now_filetime(), &now); st = &now; }
    unsigned h12 = st->hour % 12 ? st->hour % 12 : 12;
    char b[32]; snprintf(b, sizeof b, "%u:%02u:%02u %s", h12, st->minute, st->second, st->hour < 12 ? "AM" : "PM");
    return put_w(b, buf, cap);
}

// --- process / environment ---
std::mutex g_env_mx;
std::map<std::u16string, std::u16string> g_env = {{u"SystemRoot", u"C:\\Windows"}, {u"TEMP", u"C:\\GD\\temp"},
                                                  {u"TMP", u"C:\\GD\\temp"}, {u"OS", u"Windows_NT"}};
std::string g_cmdline_a;
W32 WCHAR* GetCommandLineW() { return (WCHAR*)g_h.command_line.c_str(); }
W32 char* GetCommandLineA() { return (char*)g_cmdline_a.c_str(); }
W32 WCHAR* GetEnvironmentStringsW() {
    std::lock_guard<std::mutex> l(g_env_mx);
    std::u16string block;
    for (auto& [k, v] : g_env) { block += k; block += u'='; block += v; block += u'\0'; }
    block += u'\0';
    auto* out = (WCHAR*)heap_alloc(block.size() * 2, false);
    memcpy(out, block.data(), block.size() * 2);
    return out;
}
W32 BOOL FreeEnvironmentStringsW(WCHAR* p) { heap_free(p); return TRUE_; }
W32 BOOL SetEnvironmentVariableW(const WCHAR* k, const WCHAR* v) {
    std::lock_guard<std::mutex> l(g_env_mx);
    if (v) g_env[k] = v; else g_env.erase(k);
    return TRUE_;
}
W32 void GetStartupInfoW(void* si) { memset(si, 0, 104); *(DWORD*)si = 104; }
struct SYSTEM_INFO { WORD arch, reserved; DWORD page_size; void* min_addr; void* max_addr; ULONG_PTR active_mask;
                     DWORD nprocs, proc_type, alloc_gran; WORD level, revision; };
W32 void GetSystemInfo(SYSTEM_INFO* si) {
    memset(si, 0, sizeof *si);
    si->arch = 9; si->page_size = 4096; si->min_addr = (void*)0x10000; si->max_addr = (void*)0x7FFFFFFEFFFFull;
    si->nprocs = plat::cpu_count(); si->active_mask = (1ull << si->nprocs) - 1; si->proc_type = 8664;
    si->alloc_gran = 65536; si->level = 0x16; si->revision = 0;   // AMD family 16h (Jaguar)
}
W32 DWORD GetVersion() { return 0x23F00206; }   // 6.2 build 9200 (what un-manifested apps see)
W32 BOOL IsProcessorFeaturePresent(DWORD f) {
    switch (f) {
    case 6: case 7: case 10: case 13: return TRUE_;   // XMMI, 3DNOW-less SSE paths: SSE, SSE2, SSE3
    default: return FALSE_;                           // incl. PF_FASTFAIL_AVAILABLE (23): use the slow path
    }
}
W32 BOOL GetLogicalProcessorInformationEx(int, void*, DWORD* len) { if (len) *len = 0; set_err(E_NOT_SUPPORTED); return FALSE_; }
W32 BOOL IsDebuggerPresent() { return FALSE_; }
W32 void OutputDebugStringA(const char* s) { plat::debug_log(s); }
W32 void OutputDebugStringW(const WCHAR* s) { plat::debug_log(to_utf8(s).c_str()); }
void* g_unhandled_filter;
W32 void* SetUnhandledExceptionFilter(void* f) { void* old = g_unhandled_filter; g_unhandled_filter = f; return old; }
W32 LONG UnhandledExceptionFilter(void*) { log("gdps4: unhandled exception reached UnhandledExceptionFilter\n"); return 1; }
const uintptr_t kPtrCookie = 0x2B992DDFA232ull;
W32 void* EncodePointer(void* p) { return (void*)((uintptr_t)p ^ kPtrCookie); }
W32 void* DecodePointer(void* p) { return (void*)((uintptr_t)p ^ kPtrCookie); }
W32 UINT SetErrorMode(UINT) { return 0; }
W32 DWORD SetThreadExecutionState(DWORD s) { return s; }
W32 BOOL DisableThreadLibraryCalls(void*) { return TRUE_; }
W32 ULONGLONG VerSetConditionMask(ULONGLONG mask, DWORD type, BYTE cond) {
    for (int bit = 0; bit < 8; bit++) if (type & (1u << bit)) mask |= (ULONGLONG)(cond & 7) << (bit * 3);
    return mask;
}
W32 DWORD FormatMessageW(DWORD flags, const void*, DWORD id, DWORD, WCHAR* buf, DWORD n, void*) {
    std::u16string msg = from_utf8(("Win32 error " + std::to_string(id)).c_str());
    if (flags & 0x100 /*ALLOCATE_BUFFER*/) {
        auto* p = (WCHAR*)heap_alloc((msg.size() + 1) * 2, false);
        memcpy(p, msg.c_str(), (msg.size() + 1) * 2); *(WCHAR**)buf = p; return (DWORD)msg.size();
    }
    if (msg.size() + 1 > n) { set_err(E_INSUFFICIENT_BUFFER); return 0; }
    memcpy(buf, msg.c_str(), (msg.size() + 1) * 2); return (DWORD)msg.size();
}
W32 DWORD FormatMessageA(DWORD flags, const void*, DWORD id, DWORD, char* buf, DWORD n, void*) {
    std::string msg = "Win32 error " + std::to_string(id);
    if (flags & 0x100) {
        auto* p = (char*)heap_alloc(msg.size() + 1, false);
        memcpy(p, msg.c_str(), msg.size() + 1); *(char**)buf = p; return (DWORD)msg.size();
    }
    if (msg.size() + 1 > n) { set_err(E_INSUFFICIENT_BUFFER); return 0; }
    memcpy(buf, msg.c_str(), msg.size() + 1); return (DWORD)msg.size();
}

// --- modules (delegated to the PE loader) ---
std::string mod_name(std::string n) {
    for (auto& c : n) c = (char)tolower((unsigned char)c);
    if (auto p = n.find_last_of("\\/"); p != std::string::npos) n = n.substr(p + 1);
    if (n.find('.') == std::string::npos) n += ".dll";
    return n;
}
W32 void* GetModuleHandleW(const WCHAR* n) {
    if (!n) return g_h.exe_base;
    void* b = g_h.module_by_name ? g_h.module_by_name(mod_name(to_utf8(n))) : nullptr;
    if (!b) set_err(E_MOD_NOT_FOUND);
    return b;
}
W32 BOOL GetModuleHandleExW(DWORD flags, const WCHAR* n, void** out) {
    void* b = (flags & 4 /*FROM_ADDRESS*/) ? (g_h.module_containing ? g_h.module_containing(n) : nullptr) : GetModuleHandleW(n);
    *out = b;
    if (!b) { set_err(E_MOD_NOT_FOUND); return FALSE_; }
    return TRUE_;
}
W32 DWORD GetModuleFileNameW(void* m, WCHAR* buf, DWORD n) {
    std::string p = g_h.module_path ? g_h.module_path(m ? m : g_h.exe_base) : "";
    if (p.empty()) { set_err(E_MOD_NOT_FOUND); return 0; }
    std::u16string w = from_utf8(p.c_str());
    DWORD len = (DWORD)std::min<size_t>(w.size(), n ? n - 1 : 0);
    memcpy(buf, w.c_str(), len * 2); if (n) buf[len] = 0;
    if (w.size() >= n) { set_err(E_INSUFFICIENT_BUFFER); return n; }
    return len;
}
W32 void* GetProcAddress(void* m, const char* sym) {
    std::string s = (uintptr_t)sym >> 16 ? std::string(sym) : "#" + std::to_string((uintptr_t)sym);
    void* p = g_h.get_proc ? g_h.get_proc(m, s) : nullptr;
    if (!p) set_err(E_PROC_NOT_FOUND);
    return p;
}
W32 void* LoadLibraryExW(const WCHAR* n, HANDLE, DWORD) {
    void* b = g_h.load_library ? g_h.load_library(mod_name(to_utf8(n))) : nullptr;
    if (!b) set_err(E_MOD_NOT_FOUND);
    return b;
}
W32 void* GetModuleHandleA(const char* n) { return n ? GetModuleHandleW(ansi_to_w(n).c_str()) : g_h.exe_base; }
W32 BOOL GetModuleHandleExA(DWORD flags, const char* n, void** out) {
    if (flags & 4) return GetModuleHandleExW(flags, (const WCHAR*)n, out);   // FROM_ADDRESS: n is an address
    return GetModuleHandleExW(flags, n ? ansi_to_w(n).c_str() : nullptr, out);
}
W32 DWORD GetModuleFileNameA(void* m, char* buf, DWORD n) {
    std::string p = g_h.module_path ? g_h.module_path(m ? m : g_h.exe_base) : "";
    if (p.empty()) { set_err(E_MOD_NOT_FOUND); return 0; }
    std::u16string w = from_utf8(p.c_str());
    std::string a = w_to_ansi(w.c_str(), w.size(), 0, nullptr);
    DWORD len = (DWORD)std::min<size_t>(a.size(), n ? n - 1 : 0);
    memcpy(buf, a.c_str(), len); if (n) buf[len] = 0;
    if (a.size() >= n) { set_err(E_INSUFFICIENT_BUFFER); return n; }
    return len;
}
W32 void* LoadLibraryW(const WCHAR* n) { return LoadLibraryExW(n, nullptr, 0); }
W32 void* LoadLibraryA(const char* n) { return LoadLibraryExW(ansi_to_w(n).c_str(), nullptr, 0); }
W32 DWORD GetFileAttributesA(const char* n) { return GetFileAttributesW(ansi_to_w(n).c_str()); }
W32 BOOL FreeLibrary(void*) { return TRUE_; }
W32 void* FindResourceW(void*, const WCHAR*, const WCHAR*) { set_err(E_RESOURCE_TYPE_NOT_FOUND); return nullptr; }
W32 void* LoadResource(void*, void*) { return nullptr; }

// --- registry (in memory) ---
std::mutex g_reg_mx;
std::map<std::u16string, std::vector<uint8_t>> g_reg;
struct RegKey : Obj { std::u16string path; };
W32 LONG RegCreateKeyExW(void* parent, const WCHAR* sub, DWORD, WCHAR*, DWORD, DWORD, void*, void** out, DWORD* disp) {
    auto k = std::make_shared<RegKey>();
    auto pk = get_as<RegKey>(parent);
    k->path = (pk ? pk->path : u"HK" + from_utf8(std::to_string((uintptr_t)parent).c_str())) + u"\\" + (sub ? sub : u"");
    *out = add_handle(k);
    if (disp) *disp = 2;   // REG_OPENED_EXISTING_KEY
    return 0;
}
W32 LONG RegSetValueExW(void* key, const WCHAR* name, DWORD, DWORD type, const BYTE* data, DWORD n) {
    auto k = get_as<RegKey>(key); if (!k) return E_INVALID_HANDLE;
    std::vector<uint8_t> v(4); memcpy(v.data(), &type, 4); v.insert(v.end(), data, data + n);
    std::lock_guard<std::mutex> l(g_reg_mx); g_reg[k->path + u"\\" + (name ? name : u"")] = v;
    return 0;
}
W32 LONG RegQueryValueExW(void* key, const WCHAR* name, DWORD*, DWORD* type, BYTE* data, DWORD* n) {
    auto k = get_as<RegKey>(key); if (!k) return E_INVALID_HANDLE;
    std::lock_guard<std::mutex> l(g_reg_mx);
    auto it = g_reg.find(k->path + u"\\" + (name ? name : u""));
    if (it == g_reg.end()) return E_FILE_NOT_FOUND;
    DWORD sz = (DWORD)it->second.size() - 4;
    if (type) memcpy(type, it->second.data(), 4);
    if (data) { if (!n || *n < sz) { if (n) *n = sz; return 234 /*ERROR_MORE_DATA*/; } memcpy(data, it->second.data() + 4, sz); }
    if (n) *n = sz;
    return 0;
}
W32 LONG RegCloseKey(void* key) { CloseHandle(key); return 0; }

// --- security tokens: none ---
W32 BOOL OpenProcessToken(HANDLE, DWORD, HANDLE*) { set_err(E_NO_TOKEN); return FALSE_; }
W32 BOOL OpenThreadToken(HANDLE, DWORD, BOOL, HANDLE*) { set_err(E_NO_TOKEN); return FALSE_; }
W32 BOOL GetTokenInformation(HANDLE, int, void*, DWORD, DWORD*) { set_err(E_INVALID_HANDLE); return FALSE_; }
W32 BOOL ConvertSidToStringSidA(void*, char**) { set_err(E_INVALID_PARAMETER); return FALSE_; }

// --- shell32 ---
W32 WCHAR** CommandLineToArgvW(const WCHAR* cmd, int* argc) {
    std::vector<std::u16string> args; std::u16string cur; bool q = false, any = false;
    for (const WCHAR* p = cmd; p && *p; p++) {
        if (*p == u'"') { q = !q; any = true; }
        else if ((*p == u' ' || *p == u'\t') && !q) { if (any || !cur.empty()) args.push_back(cur); cur.clear(); any = false; }
        else cur += *p;
    }
    if (any || !cur.empty()) args.push_back(cur);
    size_t bytes = (args.size() + 1) * sizeof(WCHAR*);
    for (auto& a : args) bytes += (a.size() + 1) * 2;
    auto** out = (WCHAR**)heap_alloc(bytes, true);
    auto* s = (WCHAR*)(out + args.size() + 1);
    for (size_t i = 0; i < args.size(); i++) { out[i] = s; memcpy(s, args[i].c_str(), (args[i].size() + 1) * 2); s += args[i].size() + 1; }
    *argc = (int)args.size();
    return out;
}
W32 void DragAcceptFiles(void*, BOOL) {}
W32 UINT DragQueryFileW(void*, UINT, WCHAR*, UINT) { return 0; }
W32 BOOL DragQueryPoint(void*, void*) { return FALSE_; }
W32 void DragFinish(void*) {}
W32 void* ShellExecuteA(void*, const char*, const char*, const char*, const char*, int) { return (void*)31; /*SE_ERR_NOASSOC*/ }
W32 int SHCreateDirectoryExA(void*, const char* path, void*) {
    std::u16string full = normalize(ansi_to_w(path));
    if (plat::stat(host_path(full.c_str())).is_dir) return E_ALREADY_EXISTS;
    for (size_t i = 3; i <= full.size(); i++)
        if (i == full.size() || full[i] == u'\\') {
            std::string hp = host_path(full.substr(0, i).c_str());
            if (!plat::stat(hp).exists && !plat::make_dir(hp)) return (int)err_from_errno(plat::last_errno());
        }
    return 0;
}

// --- exceptions (PS4). On the Windows host these pass through to ntdll. ---
[[noreturn]] void seh_missing(const char* what) {
    log("gdps4: %s called: Windows structured exception dispatch is not implemented on PS4 yet\n", what);
    plat::exit_process(0xE0000001);
}
W32 void RaiseException(DWORD code, DWORD, DWORD, const ULONG_PTR*) {
    log("gdps4: RaiseException(0x%08x)\n", code);
    seh_missing("RaiseException");
}
W32 void RtlUnwindEx(void*, void*, void*, void*, void*, void*) { seh_missing("RtlUnwindEx"); }
W32 void RtlUnwind(void*, void*, void*, void*) { seh_missing("RtlUnwind"); }
W32 void* RtlVirtualUnwind(DWORD, uint64_t, uint64_t, void*, void*, void**, uint64_t*, void*) { seh_missing("RtlVirtualUnwind"); }
W32 void* RtlLookupFunctionEntry(uint64_t pc, uint64_t* base, void*) {
    void* b = g_h.module_containing ? g_h.module_containing((void*)pc) : nullptr;
    if (base) *base = (uint64_t)b;
    if (!b) return nullptr;
    // binary search .pdata of that image
    auto* img = (uint8_t*)b;
    auto* nt = img + *(uint32_t*)(img + 0x3C);
    uint32_t rva = *(uint32_t*)(nt + 24 + 112 + 3 * 8), size = *(uint32_t*)(nt + 24 + 112 + 3 * 8 + 4);
    struct RF { uint32_t begin, end, unwind; };
    auto* tab = (RF*)(img + rva); size_t lo = 0, hi = size / 12; uint32_t off = (uint32_t)(pc - (uint64_t)b);
    while (lo < hi) { size_t mid = (lo + hi) / 2; if (off < tab[mid].begin) hi = mid; else if (off >= tab[mid].end) lo = mid + 1; else return &tab[mid]; }
    return nullptr;
}
W32 void* RtlPcToFileHeader(void* pc, void** base) { void* b = g_h.module_containing ? g_h.module_containing(pc) : nullptr; *base = b; return b; }
W32 WORD RtlCaptureStackBackTrace(DWORD, DWORD, void**, DWORD* hash) { if (hash) *hash = 0; return 0; }
// Version queries (GLFW calls ntdll!RtlVerifyVersionInfo unconditionally). We report Windows 10 (10.0.19041).
W32 LONG RtlGetVersion(uint8_t* vi) {
    DWORD size = *(DWORD*)vi;
    *(DWORD*)(vi + 4) = 10; *(DWORD*)(vi + 8) = 0; *(DWORD*)(vi + 12) = 19041; *(DWORD*)(vi + 16) = 2;   // VER_PLATFORM_WIN32_NT
    memset(vi + 20, 0, 256);   // szCSDVersion
    if (size >= 284) { *(WORD*)(vi + 276) = 0; *(WORD*)(vi + 278) = 0; *(WORD*)(vi + 280) = 0x100; vi[282] = 1; }   // workstation
    return 0;
}
W32 LONG RtlVerifyVersionInfo(const uint8_t* vi, DWORD type_mask, ULONGLONG cond_mask) {
    // Compare major/minor/build against 10.0.19041 using the requested operators (VER_EQUAL..VER_LESS_EQUAL).
    const DWORD have[3] = {10, 0, 19041}, want[3] = {*(DWORD*)(vi + 4), *(DWORD*)(vi + 8), *(DWORD*)(vi + 12)};
    const DWORD bits[3] = {0x2 /*MAJOR*/, 0x1 /*MINOR*/, 0x4 /*BUILD*/};
    const int shift[3] = {3, 0, 6};   // condition bit groups for VER_MAJORVERSION/MINORVERSION/BUILDNUMBER
    for (int i = 0; i < 3; i++) {
        if (!(type_mask & bits[i])) continue;
        BYTE op = (BYTE)((cond_mask >> shift[i]) & 7);
        bool ok = op == 1 ? have[i] == want[i] : op == 2 ? have[i] > want[i] : op == 3 ? have[i] >= want[i]
                : op == 4 ? have[i] < want[i] : op == 5 ? have[i] <= want[i] : true;
        if (!ok) return (LONG)0xC0000059;   // STATUS_REVISION_MISMATCH
        if (op == 3 && have[i] > want[i]) break;   // greater major already satisfies ">=" for the rest
    }
    return 0;
}
// RtlCaptureContext: fill the CONTEXT with the caller's registers (offsets per winnt.h CONTEXT, x64).
__attribute__((naked, ms_abi)) void RtlCaptureContext(void*) {
    __asm__ volatile(
        "movq %rax, 0x78(%rcx)\n movq %rcx, 0x80(%rcx)\n movq %rdx, 0x88(%rcx)\n movq %rbx, 0x90(%rcx)\n"
        "leaq 8(%rsp), %rax\n movq %rax, 0x98(%rcx)\n movq %rbp, 0xA0(%rcx)\n movq %rsi, 0xA8(%rcx)\n"
        "movq %rdi, 0xB0(%rcx)\n movq %r8, 0xB8(%rcx)\n movq %r9, 0xC0(%rcx)\n movq %r10, 0xC8(%rcx)\n"
        "movq %r11, 0xD0(%rcx)\n movq %r12, 0xD8(%rcx)\n movq %r13, 0xE0(%rcx)\n movq %r14, 0xE8(%rcx)\n"
        "movq %r15, 0xF0(%rcx)\n movq (%rsp), %rax\n movq %rax, 0xF8(%rcx)\n"
        "movl $0x10000B, 0x30(%rcx)\n"   // ContextFlags = CONTEXT_FULL (AMD64)
        "movdqa %xmm6, 0x200(%rcx)\n movdqa %xmm7, 0x210(%rcx)\n movdqa %xmm8, 0x220(%rcx)\n movdqa %xmm9, 0x230(%rcx)\n"
        "movdqa %xmm10, 0x240(%rcx)\n movdqa %xmm11, 0x250(%rcx)\n movdqa %xmm12, 0x260(%rcx)\n"
        "movdqa %xmm13, 0x270(%rcx)\n movdqa %xmm14, 0x280(%rcx)\n movdqa %xmm15, 0x290(%rcx)\n"
        "movq 0x78(%rcx), %rax\n ret\n");
}

// ================================ export table ==========================================
struct Export { const char* name; void* fn; };
#define E(f) {#f, (void*)&f}
const Export kExports[] = {
    E(GetLastError), E(SetLastError), E(CloseHandle), E(DuplicateHandle),
    E(VirtualAlloc), E(VirtualFree), E(VirtualProtect), E(VirtualQuery),
    E(GetProcessHeap), E(HeapCreate), E(HeapDestroy), E(HeapAlloc), E(HeapFree), E(HeapReAlloc), E(HeapSize),
    E(HeapValidate), E(HeapCompact), E(HeapWalk), E(HeapQueryInformation),
    E(GlobalAlloc), E(GlobalLock), E(GlobalUnlock), E(LocalAlloc), E(LocalFree), E(GlobalMemoryStatus),
    E(InitializeCriticalSection), E(InitializeCriticalSectionAndSpinCount), E(InitializeCriticalSectionEx),
    E(DeleteCriticalSection), E(EnterCriticalSection), E(TryEnterCriticalSection), E(LeaveCriticalSection),
    E(AcquireSRWLockExclusive), E(TryAcquireSRWLockExclusive), E(ReleaseSRWLockExclusive),
    E(InitializeConditionVariable), E(WakeConditionVariable), E(WakeAllConditionVariable),
    E(SleepConditionVariableSRW), E(SleepConditionVariableCS), E(InitOnceExecuteOnce),
    E(InitializeSListHead), E(InterlockedPushEntrySList), E(InterlockedFlushSList),
    E(CreateEventExW), E(CreateEventA), E(CreateMutexW), E(CreateSemaphoreExW), E(CreateSemaphoreA),
    E(SetEvent), E(ResetEvent), E(ReleaseSemaphore),
    E(WaitForMultipleObjects), E(WaitForSingleObject), E(WaitForSingleObjectEx),
    E(TlsAlloc), E(TlsFree), E(TlsGetValue), E(TlsSetValue), E(FlsAlloc), E(FlsFree), E(FlsGetValue), E(FlsSetValue),
    E(CreateThread), E(ResumeThread), E(SuspendThread), E(GetExitCodeThread), E(GetCurrentThread), E(GetCurrentThreadId),
    E(GetCurrentProcess), E(GetCurrentProcessId), E(GetCurrentProcessorNumber), E(Sleep), E(SwitchToThread),
    E(GetThreadPriority), E(SetThreadPriority), E(SetThreadAffinityMask), E(GetProcessAffinityMask),
    E(SetProcessAffinityMask), E(GetThreadContext), E(SetThreadContext), E(ExitThread), E(FreeLibraryAndExitThread),
    E(FlushProcessWriteBuffers),
    E(CreateThreadpoolWork), E(SubmitThreadpoolWork), E(CloseThreadpoolWork), E(CreateThreadpoolTimer),
    E(SetThreadpoolTimer), E(WaitForThreadpoolTimerCallbacks), E(CloseThreadpoolTimer), E(CreateThreadpoolWait),
    E(SetThreadpoolWait), E(CloseThreadpoolWait), E(FreeLibraryWhenCallbackReturns),
    E(QueryPerformanceCounter), E(QueryPerformanceFrequency), E(GetTickCount), E(GetTickCount64),
    E(GetSystemTimeAsFileTime), E(GetSystemTimePreciseAsFileTime), E(GetSystemTime), E(GetLocalTime), E(SetLocalTime),
    E(FileTimeToSystemTime), E(SystemTimeToFileTime), E(GetTimeZoneInformation),
    E(SystemTimeToTzSpecificLocalTime), E(TzSpecificLocalTimeToSystemTime),
    E(CreateFileW), E(CreateFileA), E(CreateFile2), E(ReadFile), E(WriteFile), E(SetFilePointerEx), E(SetFilePointer),
    E(GetFileSizeEx), E(GetFileSize), E(GetFileType), E(FlushFileBuffers), E(SetEndOfFile),
    E(GetFileInformationByHandle), E(GetFileInformationByHandleEx), E(SetFileInformationByHandle), E(SetFileTime),
    E(LockFile), E(UnlockFile), E(LockFileEx), E(UnlockFileEx), E(GetFileAttributesW), E(GetFileAttributesExW),
    E(SetFileAttributesW), E(CreateDirectoryW), E(CreateDirectoryA), E(RemoveDirectoryW), E(DeleteFileW), E(DeleteFileA),
    E(MoveFileExW), E(CopyFile2), E(CreateHardLinkW), E(CreateSymbolicLinkW), E(GetFullPathNameW), E(GetFullPathNameA),
    E(GetCurrentDirectoryW), E(SetCurrentDirectoryW), E(GetTempPathW), E(GetTempPathA), E(GetSystemDirectoryA),
    E(GetDiskFreeSpaceExW), E(GetDiskFreeSpaceW), E(GetDiskFreeSpaceA), E(GetDriveTypeW), E(GetLogicalDrives),
    E(AreFileApisANSI), E(FindFirstFileExW), E(FindNextFileW), E(FindClose),
    E(CreateFileMappingW), E(CreateFileMappingA), E(MapViewOfFile), E(UnmapViewOfFile), E(FlushViewOfFile),
    E(CreatePipe), E(PeekNamedPipe), E(CreateProcessW), E(OpenProcess), E(GetExitCodeProcess), E(ExitProcess),
    E(TerminateProcess),
    E(GetStdHandle), E(SetStdHandle), E(GetConsoleCP), E(GetConsoleOutputCP), E(GetConsoleMode), E(SetConsoleMode),
    E(WriteConsoleW), E(ReadConsoleW), E(ReadConsoleInputW), E(PeekConsoleInputA), E(GetNumberOfConsoleInputEvents),
    E(SetConsoleCtrlHandler), E(Beep),
    E(GetACP), E(GetOEMCP), E(IsValidCodePage), E(GetCPInfo), E(MultiByteToWideChar), E(WideCharToMultiByte),
    E(LCMapStringEx), E(LCMapStringW), E(CompareStringEx), E(CompareStringW), E(GetStringTypeW),
    E(GetLocaleInfoEx), E(GetLocaleInfoW), E(GetUserDefaultLCID), E(IsValidLocale), E(EnumSystemLocalesW),
    E(GetDateFormatW), E(GetTimeFormatW),
    E(GetCommandLineW), E(GetCommandLineA), E(GetEnvironmentStringsW), E(FreeEnvironmentStringsW),
    E(SetEnvironmentVariableW), E(GetStartupInfoW), E(GetSystemInfo), E(GetVersion), E(IsProcessorFeaturePresent),
    E(GetLogicalProcessorInformationEx), E(IsDebuggerPresent), E(OutputDebugStringA), E(OutputDebugStringW),
    E(SetUnhandledExceptionFilter), E(UnhandledExceptionFilter), E(EncodePointer), E(DecodePointer), E(SetErrorMode),
    E(SetThreadExecutionState), E(DisableThreadLibraryCalls), E(VerSetConditionMask), E(FormatMessageW), E(FormatMessageA),
    E(GetModuleHandleW), E(GetModuleHandleExW), E(GetModuleFileNameW), E(GetProcAddress), E(LoadLibraryExW),
    E(FreeLibrary), E(FindResourceW), E(LoadResource),
    E(GetModuleHandleA), E(GetModuleHandleExA), E(GetModuleFileNameA), E(LoadLibraryW), E(LoadLibraryA),
    E(GetFileAttributesA),
    E(RegCreateKeyExW), E(RegSetValueExW), E(RegQueryValueExW), E(RegCloseKey),
    E(OpenProcessToken), E(OpenThreadToken), E(GetTokenInformation), E(ConvertSidToStringSidA),
    E(CommandLineToArgvW), E(DragAcceptFiles), E(DragQueryFileW), E(DragQueryPoint), E(DragFinish), E(ShellExecuteA),
    E(SHCreateDirectoryExA),
    E(RaiseException), E(RtlUnwindEx), E(RtlUnwind), E(RtlVirtualUnwind), E(RtlLookupFunctionEntry),
    E(RtlPcToFileHeader), E(RtlCaptureStackBackTrace), E(RtlCaptureContext), E(RtlGetVersion), E(RtlVerifyVersionInfo),
};
#undef E
const std::unordered_map<std::string, void*>& export_map() {
    static const auto m = [] {
        std::unordered_map<std::string, void*> r;
        for (auto& e : kExports) r[e.name] = e.fn;
        return r;
    }();
    return m;
}
}  // namespace

void init(const Hooks& hooks) {
    g_h = hooks;
    g_cwd = hooks.current_dir;
    g_cmdline_a = w_to_ansi(hooks.command_line.c_str(), hooks.command_line.size(), 0, nullptr);
    memset(&g_peb, 0, sizeof g_peb);
    *(void**)(g_peb.raw + 0x10) = hooks.exe_base;      // ImageBaseAddress
    *(void**)(g_peb.raw + 0x30) = kProcessHeap;        // ProcessHeap
    *(uint32_t*)(g_peb.raw + 0x118) = 10;              // OSMajorVersion
    *(uint16_t*)(g_peb.raw + 0x120) = 19041;           // OSBuildNumber
    // ProcessParameters (RTL_USER_PROCESS_PARAMETERS): the C runtime's thread start reads it
    // (PS4 console run: NULL here crashed every _beginthreadex thread at ucrtbase+0x21bd4).
    static uint8_t params[0x400];
    memset(params, 0, sizeof params);
    *(uint32_t*)(params + 0x00) = sizeof params;       // MaximumLength
    *(uint32_t*)(params + 0x04) = sizeof params;       // Length
    *(uint32_t*)(params + 0x08) = 1;                   // Flags: PARAMS_NORMALIZED
    static std::u16string image_path;
    image_path = u"C:\\GD\\GeometryDash.exe";
    auto ustr = [](uint8_t* at, const std::u16string& s) {   // UNICODE_STRING {Length, MaximumLength, Buffer}
        *(uint16_t*)at = (uint16_t)(s.size() * 2); *(uint16_t*)(at + 2) = (uint16_t)(s.size() * 2 + 2);
        *(const char16_t**)(at + 8) = s.c_str();
    };
    ustr(params + 0x60, image_path);                   // ImagePathName
    ustr(params + 0x70, g_h.command_line);             // CommandLine
    *(void**)(g_peb.raw + 0x20) = params;
}
bool owns_dll(const std::string& dll) {
    return dll == "kernel32.dll" || dll == "kernelbase.dll" || dll == "advapi32.dll" || dll == "shell32.dll" ||
           dll.rfind("api-ms-win-core-", 0) == 0 || dll.rfind("api-ms-win-security-", 0) == 0 ||
           dll.rfind("api-ms-win-eventing-", 0) == 0 || dll == "ntdll.dll";
}
void* lookup(const std::string& dll, const std::string& sym) {
    if (!owns_dll(dll)) return nullptr;
    auto& m = export_map();
    auto it = m.find(sym);
    return it == m.end() ? nullptr : it->second;
}

}  // namespace gdps4::win32
