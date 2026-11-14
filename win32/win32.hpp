// Portable Win32 layer: kernel32/kernelbase/advapi32/shell32 functions the Microsoft
// runtimes and Geometry Dash import, implemented on top of plat:: (platform.hpp).
// All exported functions use the Microsoft x64 ABI so PE code can call them directly.
#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace gdps4::win32 {

// Callbacks into the PE loader / host program.
struct Hooks {
    // Module queries. "base" is the HMODULE (image base) of a loaded image.
    std::function<void*(const std::string& name_lower)> module_by_name;     // loaded image or nullptr
    std::function<void*(const std::string& name_lower)> load_library;       // loads if needed
    std::function<void*(const void* addr)> module_containing;               // image base or nullptr
    std::function<std::string(void* base)> module_path;                     // Windows-style path
    std::function<void*(void* base, const std::string& sym)> get_proc;      // "#N" for ordinals
    void* exe_base = nullptr;                                               // GetModuleHandle(NULL)
    // Called on every thread created through CreateThread, inside the new thread, before
    // the start routine (static TLS setup), and when it ends.
    std::function<void(void* teb)> thread_attach;
    std::function<void(void* teb)> thread_detach;
    // Windows path (absolute, backslashes) -> host path. Identity-like on the PC host.
    std::function<std::string(const std::string& win_path_utf8)> map_path;
    std::u16string command_line;          // GetCommandLineW
    std::u16string current_dir;           // initial GetCurrentDirectoryW (Windows-style)
    std::u16string temp_dir;              // GetTempPathW, with trailing backslash
    void (*log)(const char*) = nullptr;   // diagnostics for unimplemented paths
};

void init(const Hooks& hooks);
// Shim for dll!sym, or nullptr if this layer doesn't provide it.
// dll is lowercase (kernel32.dll, kernelbase.dll, api-ms-win-core-*.dll, advapi32.dll, shell32.dll).
void* lookup(const std::string& dll, const std::string& sym);
// Whether this layer is responsible for dll at all (so missing symbols are reported, not passed through).
bool owns_dll(const std::string& dll);

// PS4 only: per-thread Win32 TEB (gs base points at it). nullptr on the Windows host.
void* current_teb();
// Builds the TEB for the calling thread (main thread at startup). Returns it (or nullptr on host).
void* attach_current_thread();

} // namespace gdps4::win32
