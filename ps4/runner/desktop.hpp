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
// Controller-driven mouse: moves the cursor by (dx, dy) pixels and posts WM_MOUSEMOVE /
// WM_LBUTTONDOWN / WM_LBUTTONUP to the focused window like real mouse input.
void mouse_input(float dx, float dy, bool left_down);
bool cursor_pos(int* x, int* y);   // false until the virtual mouse has been used
}  // namespace gdps4::desktop
