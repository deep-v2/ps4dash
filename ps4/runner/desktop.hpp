// user32.dll + gdi32.dll for the PS4 runner: a single full-screen "desktop" with real
// window classes, windows and per-thread message queues, enough for GLFW's Win32 backend.
#pragma once
#include <string>

namespace gdps4::desktop {
struct Hooks {
    void (*log)(const char*) = nullptr;
    void* hinstance = nullptr;   // GetModuleHandle(NULL)
};
void init(const Hooks& h);
void* lookup(const std::string& dll, const std::string& sym);   // user32.dll / gdi32.dll export or nullptr
}  // namespace gdps4::desktop
