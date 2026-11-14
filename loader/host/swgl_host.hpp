// PC host glue for swgl: opengl32 exports (WGL + GL) and gdi32!SwapBuffers backed by the software renderer.
#pragma once
#include <string>

namespace swgl_host {
// missing(name): returns a stub for a desktop-only GL function that stops the run if it is called.
void init(void (*log)(const char*), void* (*missing)(const std::string& name));
void* lookup_gl(const std::string& sym);   // opengl32.dll export, or nullptr
void* swap_buffers_fn();                    // replacement for gdi32!SwapBuffers
}  // namespace swgl_host
