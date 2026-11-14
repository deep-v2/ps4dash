// Windows-host backend: lets the PC test program run the game through our Win32 layer.
// Deliberately thin; the Windows semantics live in kernel32.cpp, not here.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "platform.hpp"
#include <windows.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <thread>

namespace gdps4::plat {
namespace {
std::wstring wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}
std::string utf8(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}
thread_local int t_errno;
int map_err(DWORD e) {
    switch (e) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: return ENOENT;
    case ERROR_ALREADY_EXISTS: case ERROR_FILE_EXISTS: return EEXIST;
    case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: return EACCES;
    case ERROR_DIR_NOT_EMPTY: return ENOTEMPTY;
    default: return EIO;
    }
}
DWORD page_prot(int prot) {
    bool r = prot & 1, w = prot & 2, x = prot & 4;
    if (x) return w ? PAGE_EXECUTE_READWRITE : r ? PAGE_EXECUTE_READ : PAGE_EXECUTE;
    return w ? PAGE_READWRITE : r ? PAGE_READONLY : PAGE_NOACCESS;
}
}

void* vm_reserve(size_t size, bool commit, int prot) {
    return VirtualAlloc(nullptr, size, MEM_RESERVE | (commit ? MEM_COMMIT : 0), commit ? page_prot(prot) : PAGE_NOACCESS);
}
bool vm_protect(void* p, size_t size, int prot) {
    DWORD old; MEMORY_BASIC_INFORMATION mi;
    if (VirtualQuery(p, &mi, sizeof mi) && mi.State == MEM_RESERVE)
        return VirtualAlloc(p, size, MEM_COMMIT, page_prot(prot)) != nullptr;
    return VirtualProtect(p, size, page_prot(prot), &old);
}
bool vm_release(void* p, size_t) { return VirtualFree(p, 0, MEM_RELEASE); }

FileStat stat(const std::string& path) {
    FileStat st; WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(wide(path).c_str(), GetFileExInfoStandard, &d)) { t_errno = map_err(GetLastError()); return st; }
    st.exists = true; st.is_dir = d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
    st.size = ((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    uint64_t ft = ((uint64_t)d.ftLastWriteTime.dwHighDateTime << 32) | d.ftLastWriteTime.dwLowDateTime;
    st.mtime_unix_ns = ((int64_t)ft - 116444736000000000LL) * 100;
    return st;
}
intptr_t file_open(const std::string& path, bool read, bool write, bool create, bool truncate, bool must_not_exist) {
    DWORD disp = must_not_exist ? CREATE_NEW : create ? (truncate ? CREATE_ALWAYS : OPEN_ALWAYS)
                                             : (truncate ? TRUNCATE_EXISTING : OPEN_EXISTING);
    HANDLE h = CreateFileW(wide(path).c_str(), (read ? GENERIC_READ : 0) | (write ? GENERIC_WRITE : 0),
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, disp,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) { t_errno = map_err(GetLastError()); return -1; }
    return (intptr_t)h;
}
int64_t file_read(intptr_t fd, void* buf, size_t n) {
    DWORD got; if (!ReadFile((HANDLE)fd, buf, (DWORD)n, &got, nullptr)) { t_errno = map_err(GetLastError()); return -1; }
    return got;
}
int64_t file_write(intptr_t fd, const void* buf, size_t n) {
    DWORD put; if (!WriteFile((HANDLE)fd, buf, (DWORD)n, &put, nullptr)) { t_errno = map_err(GetLastError()); return -1; }
    return put;
}
int64_t file_seek(intptr_t fd, int64_t off, int whence) {
    LARGE_INTEGER d, r; d.QuadPart = off;
    if (!SetFilePointerEx((HANDLE)fd, d, &r, whence)) { t_errno = map_err(GetLastError()); return -1; }
    return r.QuadPart;
}
bool file_truncate(intptr_t fd, uint64_t size) {
    LARGE_INTEGER cur{}, z{}, s; s.QuadPart = (LONGLONG)size;
    SetFilePointerEx((HANDLE)fd, z, &cur, FILE_CURRENT);
    bool ok = SetFilePointerEx((HANDLE)fd, s, nullptr, FILE_BEGIN) && SetEndOfFile((HANDLE)fd);
    SetFilePointerEx((HANDLE)fd, cur, nullptr, FILE_BEGIN);
    return ok;
}
void file_close(intptr_t fd) { CloseHandle((HANDLE)fd); }
bool make_dir(const std::string& p) { if (CreateDirectoryW(wide(p).c_str(), nullptr)) return true; t_errno = map_err(GetLastError()); return false; }
bool remove_file(const std::string& p) { if (DeleteFileW(wide(p).c_str())) return true; t_errno = map_err(GetLastError()); return false; }
bool remove_dir(const std::string& p) { if (RemoveDirectoryW(wide(p).c_str())) return true; t_errno = map_err(GetLastError()); return false; }
bool rename_path(const std::string& a, const std::string& b) {
    if (MoveFileExW(wide(a).c_str(), wide(b).c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
    t_errno = map_err(GetLastError()); return false;
}
std::vector<std::string> list_dir(const std::string& path) {
    std::vector<std::string> out; WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wide(path + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) { t_errno = map_err(GetLastError()); return out; }
    do {
        std::string n = utf8(fd.cFileName);
        if (n != "." && n != "..") out.push_back(n);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}
int last_errno() { return t_errno; }

uint64_t monotonic_ns() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
int64_t realtime_unix_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
uint64_t thread_id() { return GetCurrentThreadId(); }
void thread_yield() { SwitchToThread(); }
void sleep_ms(uint32_t ms) { Sleep(ms); }
bool set_gs_base(void*) { return false; }   // Windows already has a real TEB on every thread
unsigned cpu_count() { return std::thread::hardware_concurrency(); }
void start_thread(std::function<void()> fn, size_t stack_size) {
    auto* heap_fn = new std::function<void()>(std::move(fn));
    HANDLE h = CreateThread(nullptr, stack_size, [](LPVOID a) -> DWORD {
        auto* f = (std::function<void()>*)a; (*f)(); delete f; return 0;
    }, heap_fn, 0, nullptr);
    if (h) CloseHandle(h); else delete heap_fn;
}
void exit_process(int code) { ExitProcess((UINT)code); }
void debug_log(const char* msg) { OutputDebugStringA(msg); fputs(msg, stderr); }

} // namespace gdps4::plat
