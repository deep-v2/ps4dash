// opengl32.dll for the PS4 runner: WGL on EGL, desktop GL 2.1 on Piglet's GLES2.
#pragma once
#include <functional>
#include <string>

namespace gdps4::gl {
struct Hooks {
    void (*log)(const char*) = nullptr;
    // Creates a stub that stops the run and reports `name` if called (runner's make_stub).
    std::function<void*(const std::string& name)> make_stub;
    void (*fatal)(const char* why) = nullptr;   // stops the run with a message (runner's halt)
};
void init(const Hooks& h);
bool early_init();   // call at process start, before game code (Piglet bring-up + diagnostics)
void* lookup(const std::string& sym);   // opengl32.dll export (gl*, wgl*), or nullptr
bool swap_buffers();                    // gdi32!SwapBuffers
int width();
int height();
}  // namespace gdps4::gl
