// PS4 backend (OpenOrbis libc + libkernel). Plain POSIX except memory (flexible memory
// with CPU exec, verified on hardware in M3) and the gs base (sysarch, verified in M3).
#include "platform.hpp"
#include <orbis/libkernel.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// libc++ needs this for thread_local objects with destructors; OpenOrbis' libc lacks it.
// Destructors are queued per thread and run by a pthread key destructor at thread exit.
namespace {
struct TlDtor { void (*fn)(void*); void* obj; TlDtor* next; };
pthread_key_t g_tl_key;
pthread_once_t g_tl_once = PTHREAD_ONCE_INIT;
void run_tl_dtors(void* head) {
    for (auto* d = (TlDtor*)head; d;) { auto* n = d->next; d->fn(d->obj); free(d); d = n; }
}
void make_tl_key() { pthread_key_create(&g_tl_key, run_tl_dtors); }
}
extern "C" int __cxa_thread_atexit_impl(void (*fn)(void*), void* obj, void*) {
    pthread_once(&g_tl_once, make_tl_key);
    auto* d = (TlDtor*)malloc(sizeof(TlDtor));
    if (!d) return -1;
    d->fn = fn; d->obj = obj; d->next = (TlDtor*)pthread_getspecific(g_tl_key);   // LIFO, like the C++ ABI
    pthread_setspecific(g_tl_key, d);
    return 0;
}

namespace gdps4::plat {
namespace {
int orbis_prot(int prot) {
    int p = 0;
    if (prot & 1) p |= ORBIS_KERNEL_PROT_CPU_READ;
    if (prot & 2) p |= ORBIS_KERNEL_PROT_CPU_WRITE;
    if (prot & 4) p |= ORBIS_KERNEL_PROT_CPU_EXEC;
    return p;
}
size_t round_page(size_t n) { return (n + 0x3FFF) & ~(size_t)0x3FFF; }   // 16 KiB pages
}

void* vm_reserve(size_t size, bool commit, int prot) {
    // Flexible memory can't be reserved without backing, so "reserve" commits RW(X) up front.
    void* p = nullptr;
    int pr = commit ? orbis_prot(prot) : orbis_prot(3);
    if (sceKernelMapNamedFlexibleMemory(&p, round_page(size), pr, 0, "gdps4-win32") == 0) return p;
    p = mmap(nullptr, round_page(size), PROT_READ | PROT_WRITE | ((prot & 4) ? PROT_EXEC : 0), MAP_PRIVATE | MAP_ANON, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
}
bool vm_protect(void* p, size_t size, int prot) { return sceKernelMprotect(p, round_page(size), orbis_prot(prot)) == 0; }
bool vm_release(void* p, size_t size) { return munmap(p, round_page(size)) == 0; }

FileStat stat(const std::string& path) {
    FileStat st; struct ::stat s;
    if (::stat(path.c_str(), &s) != 0) return st;
    st.exists = true; st.is_dir = S_ISDIR(s.st_mode); st.size = (uint64_t)s.st_size;
    st.mtime_unix_ns = (int64_t)s.st_mtim.tv_sec * 1000000000LL + s.st_mtim.tv_nsec;
    return st;
}
intptr_t file_open(const std::string& path, bool read, bool write, bool create, bool truncate, bool must_not_exist) {
    int fl = read && write ? O_RDWR : write ? O_WRONLY : O_RDONLY;
    if (create) fl |= O_CREAT;
    if (truncate) fl |= O_TRUNC;
    if (must_not_exist) fl |= O_CREAT | O_EXCL;
    int fd = ::open(path.c_str(), fl, 0666);
    if (fd < 0) {   // directories (FILE_FLAG_BACKUP_SEMANTICS) open read-only
        struct ::stat s;
        if (::stat(path.c_str(), &s) == 0 && S_ISDIR(s.st_mode)) fd = ::open(path.c_str(), O_RDONLY);
    }
    return fd;
}
int64_t file_read(intptr_t fd, void* buf, size_t n) { return ::read((int)fd, buf, n); }
int64_t file_write(intptr_t fd, const void* buf, size_t n) { return ::write((int)fd, buf, n); }
int64_t file_seek(intptr_t fd, int64_t off, int whence) { return ::lseek((int)fd, off, whence); }
bool file_truncate(intptr_t fd, uint64_t size) { return ::ftruncate((int)fd, (off_t)size) == 0; }
void file_close(intptr_t fd) { ::close((int)fd); }
bool make_dir(const std::string& p) { return ::mkdir(p.c_str(), 0777) == 0; }
bool remove_file(const std::string& p) { return ::unlink(p.c_str()) == 0; }
bool remove_dir(const std::string& p) { return ::rmdir(p.c_str()) == 0; }
bool rename_path(const std::string& a, const std::string& b) { return ::rename(a.c_str(), b.c_str()) == 0; }
std::vector<std::string> list_dir(const std::string& path) {
    std::vector<std::string> out;
    DIR* d = opendir(path.c_str());
    if (!d) return out;
    while (dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n != "." && n != "..") out.push_back(n);
    }
    closedir(d);
    return out;
}
int last_errno() { return errno; }

uint64_t monotonic_ns() {
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}
int64_t realtime_unix_ns() {
    timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}
uint64_t thread_id() { return (uint64_t)(uint32_t)scePthreadGetthreadid(); }
void thread_yield() { sched_yield(); }
void sleep_ms(uint32_t ms) { sceKernelUsleep(ms * 1000); }
bool set_gs_base(void* teb) {
    // FreeBSD sysarch(AMD64_SET_GSBASE = 131), syscall 165. Verified on PS4 in M3.
    uint64_t base = (uint64_t)teb; long ret; char cf;
    __asm__ volatile("syscall; setc %1" : "=a"(ret), "=r"(cf) : "a"(165L), "D"(131L), "S"(&base) : "rcx", "r11", "memory");
    return !cf && ret == 0;
}
unsigned cpu_count() { return 6; }
void start_thread(std::function<void()> fn, size_t stack_size) {
    auto* heap_fn = new std::function<void()>(std::move(fn));
    pthread_attr_t attr; pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack_size < 0x40000 ? 0x40000 : stack_size);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t th;
    int rc = pthread_create(&th, &attr, [](void* a) -> void* {
        auto* f = (std::function<void()>*)a; (*f)(); delete f; return nullptr;
    }, heap_fn);
    pthread_attr_destroy(&attr);
    if (rc != 0) { char b[96]; snprintf(b, sizeof b, "gdps4: pthread_create failed (%d)\n", rc); debug_log(b); delete heap_fn; }
}
void exit_process(int code) { _exit(code); }
void debug_log(const char* msg) { printf("%s", msg); }

} // namespace gdps4::plat
