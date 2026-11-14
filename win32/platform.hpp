// Platform backend for the portable Win32 layer. Two implementations:
//   platform_posix.cpp - PS4 (OpenOrbis libc/libkernel), the real target
//   platform_win.cpp   - Windows host, only to validate the Win32 layer on PC
// Everything here uses host types and the host calling convention; the Win32 layer on
// top converts to Windows semantics (UTF-16 paths, HANDLEs, FILETIME, error codes).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gdps4::plat {

// ---- memory ----
void* vm_reserve(size_t size, bool commit, int prot);   // prot: 1=R 2=W 4=X
bool vm_protect(void* p, size_t size, int prot);
bool vm_release(void* p, size_t size);

// ---- files (paths are host-native UTF-8, already translated) ----
struct FileStat { bool exists = false, is_dir = false; uint64_t size = 0; int64_t mtime_unix_ns = 0; };
FileStat stat(const std::string& path);
intptr_t file_open(const std::string& path, bool read, bool write, bool create, bool truncate, bool must_not_exist);
int64_t file_read(intptr_t fd, void* buf, size_t n);
int64_t file_write(intptr_t fd, const void* buf, size_t n);
int64_t file_seek(intptr_t fd, int64_t off, int whence);   // 0=SET 1=CUR 2=END, returns new pos or -1
bool file_truncate(intptr_t fd, uint64_t size);
void file_close(intptr_t fd);
bool make_dir(const std::string& path);
bool remove_file(const std::string& path);
bool remove_dir(const std::string& path);
bool rename_path(const std::string& from, const std::string& to);
std::vector<std::string> list_dir(const std::string& path);   // names only, no "." / ".."
int last_errno();   // POSIX errno meaning (ENOENT, EEXIST, EACCES, ...)

// ---- time ----
uint64_t monotonic_ns();
int64_t realtime_unix_ns();

// ---- threads ----
uint64_t thread_id();
void thread_yield();
void sleep_ms(uint32_t ms);
// Point this thread's gs base at a Win32 TEB built by the Win32 layer.
// PS4: sysarch(AMD64_SET_GSBASE). Windows host: returns false (threads already have a real TEB).
bool set_gs_base(void* teb);
unsigned cpu_count();

// ---- process ----
[[noreturn]] void exit_process(int code);
void debug_log(const char* msg);

} // namespace gdps4::plat
