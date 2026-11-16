// Fake Windows desktop for GLFW (inside libcocos2d): see desktop.hpp.
// Layouts follow winuser.h/wingdi.h for x64; WCHAR is char16_t.
#include "desktop.hpp"
#include "gl_piglet.hpp"
#include "../../win32/platform.hpp"
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#define MSABI __attribute__((ms_abi))

namespace gdps4::desktop {
namespace {
using BOOL = int32_t; using UINT = uint32_t; using DWORD = uint32_t; using LONG = int32_t; using WORD = uint16_t;
using LRESULT = intptr_t; using WPARAM = uintptr_t; using LPARAM = intptr_t; using HWND = void*; using WCHAR = char16_t;
using WndProc = LRESULT (MSABI*)(HWND, UINT, WPARAM, LPARAM);
struct RECT { LONG left, top, right, bottom; };
struct POINT { LONG x, y; };
struct MSG { HWND hwnd; UINT message; UINT pad; WPARAM wParam; LPARAM lParam; DWORD time; POINT pt; DWORD priv; };

Hooks g_h;
void log(const char* fmt, ...) {
    char b[512]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    if (g_h.log) g_h.log(b);
}
int W() { return gl::width(); }
int H() { return gl::height(); }
DWORD now_ms() { return (DWORD)(plat::monotonic_ns() / 1000000); }

std::u16string wstr(const WCHAR* s) { std::u16string o; while (s && *s) o += *s++; return o; }
std::u16string astr(const char* s) { std::u16string o; while (s && *s) o += (char16_t)(unsigned char)*s++; return o; }
bool is_atom(const void* p) { return ((uintptr_t)p >> 16) == 0; }

// ---- classes & windows --------------------------------------------------------------------
struct Class { std::u16string name; uint16_t atom; WndProc proc; int wnd_extra; void* hinst; };
struct Window {
    HWND id; Class* cls; WndProc proc; bool unicode; LONG style, exstyle; HWND parent; void* hinst;
    intptr_t userdata = 0, id_value = 0; std::vector<uint8_t> extra; std::map<std::u16string, void*> props;
    RECT rect; bool visible = false; uint64_t thread;
};
std::recursive_mutex g_mx;
std::map<std::u16string, std::unique_ptr<Class>> g_classes;
std::map<uint16_t, Class*> g_atoms;
uint16_t g_next_atom = 0xC000;
std::map<uintptr_t, std::unique_ptr<Window>> g_windows;
uintptr_t g_next_hwnd = 0x10010;
HWND g_active, g_focus, g_capture;

Class* find_class(const void* name, bool ansi) {
    if (is_atom(name)) { auto it = g_atoms.find((uint16_t)(uintptr_t)name); return it == g_atoms.end() ? nullptr : it->second; }
    auto it = g_classes.find(ansi ? astr((const char*)name) : wstr((const WCHAR*)name));
    return it == g_classes.end() ? nullptr : it->second.get();
}
Window* win(HWND h) { std::lock_guard<std::recursive_mutex> l(g_mx); auto it = g_windows.find((uintptr_t)h); return it == g_windows.end() ? nullptr : it->second.get(); }

// ---- message queues (per thread) -----------------------------------------------------------
struct Queue { std::deque<MSG> q; };
std::mutex g_q_mx;
std::condition_variable g_q_cv;
std::map<uint64_t, Queue> g_queues;
DWORD g_last_msg_time;
void post_to_thread(uint64_t tid, HWND h, UINT m, WPARAM w, LPARAM l) {
    MSG msg{}; msg.hwnd = h; msg.message = m; msg.wParam = w; msg.lParam = l; msg.time = now_ms();
    { std::lock_guard<std::mutex> lk(g_q_mx); g_queues[tid].q.push_back(msg); }
    g_q_cv.notify_all();
}
bool match(const MSG& m, HWND h, UINT lo, UINT hi) {
    if (h && h != (HWND)(intptr_t)-1 && m.hwnd != h) return false;
    if (h == (HWND)(intptr_t)-1 && m.hwnd) return false;
    if (lo || hi) return m.message >= lo && m.message <= hi;
    return true;
}
bool take(MSG* out, HWND h, UINT lo, UINT hi, bool remove) {
    std::lock_guard<std::mutex> lk(g_q_mx);
    auto& q = g_queues[plat::thread_id()].q;
    for (auto it = q.begin(); it != q.end(); ++it)
        if (match(*it, h, lo, hi)) { *out = *it; g_last_msg_time = it->time; if (remove) q.erase(it); return true; }
    return false;
}
LRESULT call_proc(Window* w, UINT m, WPARAM wp, LPARAM lp) { return w && w->proc ? w->proc(w->id, m, wp, lp) : 0; }

// ---- user32: classes / windows ----------------------------------------------------------------
uint16_t register_class(const std::u16string& name, WndProc proc, int extra, void* hinst) {
    std::lock_guard<std::recursive_mutex> l(g_mx);
    auto c = std::make_unique<Class>(); c->name = name; c->atom = g_next_atom++; c->proc = proc; c->wnd_extra = extra; c->hinst = hinst;
    g_atoms[c->atom] = c.get();
    uint16_t atom = c->atom;
    log("desktop: RegisterClass '%s' atom=%#x\n", std::string(name.begin(), name.end()).c_str(), atom);
    g_classes[name] = std::move(c);
    return atom;
}
MSABI uint16_t RegisterClassExW(const uint8_t* wc) {
    return register_class(wstr(*(const WCHAR* const*)(wc + 64)), *(WndProc*)(wc + 8), *(int*)(wc + 20), *(void**)(wc + 24));
}
MSABI uint16_t RegisterClassA(const uint8_t* wc) {
    return register_class(astr(*(const char* const*)(wc + 64)), *(WndProc*)(wc + 8), *(int*)(wc + 20), *(void**)(wc + 24));
}
MSABI BOOL UnregisterClassW(const WCHAR*, void*) { return 1; }
HWND create_window(DWORD exstyle, const void* cls_name, bool ansi, const void* title, DWORD style, int x, int y, int w, int h,
                   HWND parent, void* menu, void* hinst, void* param) {
    Window* wnd;
    {
        std::lock_guard<std::recursive_mutex> l(g_mx);
        Class* c = find_class(cls_name, ansi);
        if (!c) { log("desktop: CreateWindow with unknown class\n"); return nullptr; }
        auto nw = std::make_unique<Window>();
        nw->id = (HWND)g_next_hwnd; g_next_hwnd += 0x10;
        nw->cls = c; nw->proc = c->proc; nw->unicode = !ansi; nw->style = (LONG)style; nw->exstyle = (LONG)exstyle;
        nw->parent = parent; nw->hinst = hinst; nw->extra.resize(c->wnd_extra > 0 ? c->wnd_extra : 0);
        nw->rect = {0, 0, W(), H()};   // every window is full screen
        nw->thread = plat::thread_id();
        wnd = nw.get();
        g_windows[(uintptr_t)nw->id] = std::move(nw);
    }
    // CREATESTRUCT (same layout for A and W; strings are passed through as given)
    uint8_t cs[80] = {};
    *(void**)(cs + 0) = param; *(void**)(cs + 8) = hinst; *(void**)(cs + 16) = menu; *(HWND*)(cs + 24) = parent;
    *(int*)(cs + 32) = H(); *(int*)(cs + 36) = W(); *(int*)(cs + 40) = 0; *(int*)(cs + 44) = 0;
    *(LONG*)(cs + 48) = (LONG)style; *(const void**)(cs + 56) = title; *(const void**)(cs + 64) = cls_name; *(DWORD*)(cs + 72) = exstyle;
    if (!call_proc(wnd, 0x0081 /*WM_NCCREATE*/, 0, (LPARAM)cs)) { log("desktop: WM_NCCREATE refused\n"); return nullptr; }
    if (call_proc(wnd, 0x0001 /*WM_CREATE*/, 0, (LPARAM)cs) == -1) { log("desktop: WM_CREATE refused\n"); return nullptr; }
    log("desktop: CreateWindow -> %p (style %#x)\n", wnd->id, style);
    (void)x; (void)y; (void)w; (void)h;
    if (style & 0x10000000 /*WS_VISIBLE*/) { wnd->visible = true; }
    return wnd->id;
}
MSABI HWND CreateWindowExW(DWORD ex, const WCHAR* cls, const WCHAR* title, DWORD style, int x, int y, int w, int h,
                           HWND parent, void* menu, void* hinst, void* param) {
    return create_window(ex, cls, false, title, style, x, y, w, h, parent, menu, hinst, param);
}
MSABI HWND CreateWindowExA(DWORD ex, const char* cls, const char* title, DWORD style, int x, int y, int w, int h,
                           HWND parent, void* menu, void* hinst, void* param) {
    return create_window(ex, cls, true, title, style, x, y, w, h, parent, menu, hinst, param);
}
MSABI BOOL DestroyWindow(HWND h) {
    Window* w = win(h); if (!w) return 0;
    call_proc(w, 0x0002 /*WM_DESTROY*/, 0, 0); call_proc(w, 0x0082 /*WM_NCDESTROY*/, 0, 0);
    std::lock_guard<std::recursive_mutex> l(g_mx);
    if (g_active == h) g_active = nullptr;
    if (g_focus == h) g_focus = nullptr;
    g_windows.erase((uintptr_t)h);
    return 1;
}
void activate(Window* w) {
    if (!w || g_active == w->id) return;
    g_active = g_focus = w->id;
    // Focus/activation/size, as Windows sends them: GLFW reports focus, so GD doesn't pause itself.
    post_to_thread(w->thread, w->id, 0x0006 /*WM_ACTIVATE*/, 1 /*WA_ACTIVE*/, 0);
    post_to_thread(w->thread, w->id, 0x0007 /*WM_SETFOCUS*/, 0, 0);
    post_to_thread(w->thread, w->id, 0x0005 /*WM_SIZE*/, 0, ((LPARAM)H() << 16) | W());
    log("desktop: window %p active + focused\n", w->id);
}
MSABI BOOL ShowWindow(HWND h, int cmd) {
    Window* w = win(h); if (!w) return 0;
    bool was = w->visible;
    w->visible = cmd != 0;
    if (w->visible) activate(w);
    return was;
}
MSABI BOOL SetForegroundWindow(HWND h) { activate(win(h)); return 1; }
MSABI HWND SetFocus(HWND h) { HWND old = g_focus; activate(win(h)); return old; }
MSABI BOOL BringWindowToTop(HWND h) { activate(win(h)); return 1; }
MSABI HWND GetActiveWindow() { return g_active; }
MSABI HWND GetDesktopWindow() { return (HWND)0x10000; }
MSABI HWND GetAncestor(HWND h, UINT) { return h; }
MSABI HWND WindowFromPoint(POINT) { return g_active; }
MSABI BOOL SetWindowPos(HWND, HWND, int, int, int, int, UINT) { return 1; }
MSABI BOOL GetClientRect(HWND h, RECT* r) { *r = {0, 0, W(), H()}; return win(h) != nullptr; }
MSABI BOOL GetWindowRect(HWND h, RECT* r) { *r = {0, 0, W(), H()}; return win(h) != nullptr || h == GetDesktopWindow(); }
MSABI BOOL AdjustWindowRectEx(RECT*, DWORD, BOOL, DWORD) { return 1; }
MSABI BOOL ClientToScreen(HWND, POINT*) { return 1; }
MSABI BOOL ScreenToClient(HWND, POINT*) { return 1; }
MSABI BOOL GetWindowPlacement(HWND, uint8_t* wp) {
    memset(wp + 4, 0, 40); *(UINT*)(wp + 8) = 1; *(RECT*)(wp + 28) = {0, 0, W(), H()}; return 1;
}
MSABI BOOL SetWindowPlacement(HWND, const void*) { return 1; }
MSABI BOOL SetWindowTextA(HWND, const char*) { return 1; }
MSABI int GetWindowTextA(HWND, char* s, int n) { if (n > 0) s[0] = 0; return 0; }
intptr_t get_long(HWND h, int idx) {
    Window* w = win(h); if (!w) return 0;
    switch (idx) {
    case -4: return (intptr_t)w->proc; case -6: return (intptr_t)w->hinst; case -8: return (intptr_t)w->parent;
    case -12: return w->id_value; case -16: return w->style; case -20: return w->exstyle; case -21: return w->userdata;
    }
    if (idx >= 0 && (size_t)idx + 8 <= w->extra.size()) return *(intptr_t*)&w->extra[idx];
    if (idx >= 0 && (size_t)idx + 4 <= w->extra.size()) return *(int32_t*)&w->extra[idx];
    return 0;
}
intptr_t set_long(HWND h, int idx, intptr_t v, bool wide) {
    Window* w = win(h); if (!w) return 0;
    intptr_t old = get_long(h, idx);
    switch (idx) {
    case -4: w->proc = (WndProc)v; break; case -6: w->hinst = (void*)v; break; case -8: w->parent = (HWND)v; break;
    case -12: w->id_value = v; break; case -16: w->style = (LONG)v; break; case -20: w->exstyle = (LONG)v; break;
    case -21: w->userdata = v; break;
    default:
        if (idx >= 0 && (size_t)idx + (wide ? 8 : 4) <= w->extra.size()) memcpy(&w->extra[idx], &v, wide ? 8 : 4);
    }
    return old;
}
MSABI intptr_t GetWindowLongPtrW(HWND h, int i) { return get_long(h, i); }
MSABI intptr_t GetWindowLongPtrA(HWND h, int i) { return get_long(h, i); }
MSABI LONG GetWindowLongW(HWND h, int i) { return (LONG)get_long(h, i); }
MSABI intptr_t SetWindowLongPtrW(HWND h, int i, intptr_t v) { return set_long(h, i, v, true); }
MSABI intptr_t SetWindowLongPtrA(HWND h, int i, intptr_t v) { return set_long(h, i, v, true); }
MSABI LONG SetWindowLongW(HWND h, int i, LONG v) { return (LONG)set_long(h, i, v, false); }
std::u16string prop_key(const WCHAR* k) { return is_atom(k) ? u"#" + std::u16string(1, (char16_t)(uintptr_t)k) : wstr(k); }
MSABI BOOL SetPropW(HWND h, const WCHAR* k, void* v) { Window* w = win(h); if (!w) return 0; w->props[prop_key(k)] = v; return 1; }
MSABI void* GetPropW(HWND h, const WCHAR* k) { Window* w = win(h); if (!w) return nullptr; auto it = w->props.find(prop_key(k)); return it == w->props.end() ? nullptr : it->second; }
MSABI void* RemovePropW(HWND h, const WCHAR* k) { Window* w = win(h); if (!w) return nullptr; void* v = GetPropW(h, k); w->props.erase(prop_key(k)); return v; }

// ---- user32: messages ----------------------------------------------------------------------
MSABI LRESULT DefWindowProcW(HWND h, UINT m, WPARAM, LPARAM) {
    switch (m) {
    case 0x0081: return 1;                        // WM_NCCREATE
    case 0x0010: DestroyWindow(h); return 0;      // WM_CLOSE
    }
    return 0;
}
MSABI LRESULT DefWindowProcA(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }
MSABI LRESULT SendMessageW(HWND h, UINT m, WPARAM w, LPARAM l) { return call_proc(win(h), m, w, l); }
MSABI BOOL PostMessageA(HWND h, UINT m, WPARAM w, LPARAM l) {
    Window* wnd = win(h);
    post_to_thread(wnd ? wnd->thread : plat::thread_id(), h, m, w, l);
    return 1;
}
MSABI BOOL PeekMessageW(MSG* msg, HWND h, UINT lo, UINT hi, UINT flags) { return take(msg, h, lo, hi, flags & 1); }
MSABI BOOL GetMessageA(MSG* msg, HWND h, UINT lo, UINT hi) {
    for (;;) {
        if (take(msg, h, lo, hi, true)) return msg->message != 0x0012 /*WM_QUIT*/;
        // Poll instead of a timed condition-variable wait: the PS4 process died ~3 ms after the
        // raw-input thread first blocked here, and this rules the timed wait in or out.
        static thread_local unsigned waits = 0;
        if (waits < 3) log("desktop: GetMessageA idle wait %u begin\n", waits);
        plat::sleep_ms(5);
        if (waits < 3) log("desktop: GetMessageA idle wait %u end\n", waits);
        waits++;
    }
}
MSABI LRESULT DispatchMessageW(const MSG* m) { return call_proc(win(m->hwnd), m->message, m->wParam, m->lParam); }
MSABI LRESULT DispatchMessageA(const MSG* m) { return DispatchMessageW(m); }
MSABI BOOL TranslateMessage(const MSG*) { return 0; }
MSABI LONG GetMessageTime() { return (LONG)g_last_msg_time; }

// ---- user32: input / cursor ----------------------------------------------------------------------
POINT g_cursor{960, 540};
MSABI int16_t GetKeyState(int) { return 0; }
MSABI UINT MapVirtualKeyW(UINT, UINT) { return 0; }
MSABI int ToUnicode(UINT, UINT, const uint8_t*, WCHAR*, int, UINT) { return 0; }
MSABI BOOL GetCursorPos(POINT* p) { *p = g_cursor; return 1; }
MSABI BOOL SetCursorPos(int x, int y) { g_cursor = {x, y}; return 1; }
MSABI void* SetCursor(void* c) { return c; }
MSABI void* LoadCursorW(void*, const WCHAR*) { return (void*)0xC0C0; }
MSABI void* LoadImageW(void*, const WCHAR*, UINT, int, int, UINT) { return (void*)0xC0C1; }
MSABI BOOL DestroyIcon(void*) { return 1; }
MSABI BOOL ClipCursor(const RECT*) { return 1; }
MSABI HWND SetCapture(HWND h) { HWND o = g_capture; g_capture = h; return o; }
MSABI BOOL ReleaseCapture() { g_capture = nullptr; return 1; }
MSABI BOOL TrackMouseEvent(void*) { return 1; }
MSABI UINT SendInput(UINT n, void*, int) { return n; }
MSABI BOOL RegisterRawInputDevices(const void*, UINT, UINT) { return 1; }
MSABI UINT GetRawInputData(void*, UINT, void*, UINT* size, UINT) { if (size) *size = 0; return 0; }
MSABI UINT GetRawInputDeviceList(void*, UINT* n, UINT) { if (n) *n = 0; return 0; }
MSABI UINT GetRawInputDeviceInfoA(void*, UINT, void*, UINT* size) { if (size) *size = 0; return (UINT)-1; }
MSABI void* RegisterDeviceNotificationW(void*, void*, DWORD) { return (void*)0xD0D0; }
MSABI BOOL UnregisterDeviceNotification(void*) { return 1; }

// ---- user32: system / monitors -------------------------------------------------------------------
MSABI BOOL SystemParametersInfoW(UINT action, UINT, void* pv, UINT) {
    switch (action) {
    case 0x2000: if (pv) *(DWORD*)pv = 0; break;                                  // SPI_GETFOREGROUNDLOCKTIMEOUT
    case 0x0030: if (pv) *(RECT*)pv = {0, 0, W(), H()}; break;                    // SPI_GETWORKAREA
    case 0x0032: case 0x0034: case 0x003A: if (pv) ((DWORD*)pv)[1] = 0; break;   // GET FILTER/TOGGLE/STICKYKEYS: no flags
    }
    return 1;
}
void* const kMonitor = (void*)0x10020;
MSABI void* MonitorFromWindow(HWND, DWORD) { return kMonitor; }
MSABI BOOL GetMonitorInfoW(void*, uint8_t* mi) {
    DWORD cb = *(DWORD*)mi;
    *(RECT*)(mi + 4) = {0, 0, W(), H()}; *(RECT*)(mi + 20) = {0, 0, W(), H()}; *(DWORD*)(mi + 36) = 1;   // PRIMARY
    if (cb >= 104) { const char16_t n[] = u"\\\\.\\DISPLAY1"; memcpy(mi + 40, n, sizeof n); }
    return 1;
}
MSABI BOOL EnumDisplayMonitors(void* hdc, const RECT*, BOOL (MSABI* cb)(void*, void*, RECT*, LPARAM), LPARAM lp) {
    RECT r{0, 0, W(), H()};
    cb(kMonitor, hdc, &r, lp);
    return 1;
}
MSABI BOOL EnumDisplayDevicesW(const WCHAR* device, DWORD idx, uint8_t* dd, DWORD) {
    if (idx != 0) return 0;
    memset(dd + 4, 0, 836);
    auto put = [&](size_t off, const char16_t* s) { for (size_t i = 0; s[i]; i++) ((char16_t*)(dd + off))[i] = s[i]; };
    if (!device) { put(4, u"\\\\.\\DISPLAY1"); put(68, u"PS4 Piglet"); *(DWORD*)(dd + 324) = 0x1 | 0x4; }   // attached, primary
    else { put(4, u"\\\\.\\DISPLAY1\\Monitor0"); put(68, u"TV"); *(DWORD*)(dd + 324) = 0x1 | 0x2; }         // active, attached
    return 1;
}
MSABI BOOL EnumDisplaySettingsW(const WCHAR*, DWORD mode, uint8_t* dm) {
    if (mode != (DWORD)-1 && mode != (DWORD)-2 && mode != 0) return 0;
    WORD size = *(WORD*)(dm + 68);
    memset(dm, 0, size ? size : 220); *(WORD*)(dm + 68) = size ? size : 220;
    *(DWORD*)(dm + 72) = 0x00040000 | 0x00080000 | 0x00100000 | 0x00400000;   // BITSPERPEL|PELSWIDTH|PELSHEIGHT|DISPLAYFREQUENCY
    *(DWORD*)(dm + 168) = 32; *(DWORD*)(dm + 172) = (DWORD)W(); *(DWORD*)(dm + 176) = (DWORD)H(); *(DWORD*)(dm + 184) = 60;
    return 1;
}
MSABI LONG ChangeDisplaySettingsExW(const WCHAR*, void*, HWND, DWORD, void*) { return 0; }

// ---- DPI (user32/shcore) and DWM: GLFW calls these through GetProcAddress without NULL checks
// once the OS reports Windows 10. One display at 96 DPI (no scaling); DWM composition off. ----
MSABI BOOL SetProcessDPIAware() { return 1; }
MSABI BOOL SetProcessDpiAwarenessContext(void*) { return 1; }
MSABI BOOL ChangeWindowMessageFilterEx(HWND, UINT, DWORD, void*) { return 1; }
MSABI BOOL EnableNonClientDpiScaling(HWND) { return 1; }
MSABI UINT GetDpiForWindow(HWND) { return 96; }
MSABI BOOL AdjustWindowRectExForDpi(RECT*, DWORD, BOOL, DWORD, UINT) { return 1; }
MSABI int GetSystemMetricsForDpi(int idx, UINT) {
    switch (idx) { case 0: return W(); case 1: return H(); case 4: return 0; case 32: case 33: return 0; }   // CXSCREEN, CYSCREEN, CYCAPTION, CXFRAME, CYFRAME
    return 0;
}
MSABI int32_t GetDpiForMonitor(void*, int, UINT* x, UINT* y) { if (x) *x = 96; if (y) *y = 96; return 0; }
MSABI int32_t SetProcessDpiAwareness(int) { return 0; }
MSABI int32_t DwmIsCompositionEnabled(BOOL* enabled) { *enabled = 0; return 0; }
MSABI int32_t DwmFlush() { return 0; }
MSABI int32_t DwmEnableBlurBehindWindow(HWND, const void*) { return 0; }
MSABI int32_t DwmGetColorizationColor(DWORD* c, BOOL* opaque) { if (c) *c = 0; if (opaque) *opaque = 1; return 0; }

// ---- user32: clipboard / dialogs ----------------------------------------------------------------
MSABI BOOL OpenClipboard(HWND) { return 1; }
MSABI BOOL CloseClipboard() { return 1; }
MSABI BOOL EmptyClipboard() { return 1; }
MSABI void* GetClipboardData(UINT) { return nullptr; }
MSABI void* SetClipboardData(UINT, void* h) { return h; }
MSABI int MessageBoxW(HWND, const WCHAR* text, const WCHAR* cap, UINT) {
    std::u16string t = wstr(text), c = wstr(cap);
    log("desktop: MessageBox [%s] %s\n", std::string(c.begin(), c.end()).c_str(), std::string(t.begin(), t.end()).c_str());
    return 1;   // IDOK
}
MSABI int MessageBoxA(HWND, const char* text, const char* cap, UINT) { log("desktop: MessageBox [%s] %s\n", cap ? cap : "", text ? text : ""); return 1; }
MSABI intptr_t DialogBoxIndirectParamW(void*, const void*, HWND, void*, LPARAM) { log("desktop: dialog boxes are not supported\n"); return -1; }
MSABI BOOL EndDialog(HWND, intptr_t) { return 1; }
MSABI HWND GetDlgItem(HWND, int) { return nullptr; }
MSABI BOOL SetDlgItemTextA(HWND, int, const char*) { return 1; }
MSABI BOOL SetDlgItemTextW(HWND, int, const WCHAR*) { return 1; }
MSABI BOOL SetRect(RECT* r, int l, int t, int ri, int b) { *r = {l, t, ri, b}; return 1; }
MSABI BOOL OffsetRect(RECT* r, int dx, int dy) { r->left += dx; r->right += dx; r->top += dy; r->bottom += dy; return 1; }
MSABI BOOL PtInRect(const RECT* r, POINT p) { return p.x >= r->left && p.x < r->right && p.y >= r->top && p.y < r->bottom; }

// ---- gdi32: device contexts / pixel formats --------------------------------------------------------
std::map<uintptr_t, HWND> g_dcs;
MSABI void* GetDC(HWND h) { std::lock_guard<std::recursive_mutex> l(g_mx); uintptr_t dc = (uintptr_t)h ^ 0xDC0000; g_dcs[dc] = h; return (void*)dc; }
MSABI int ReleaseDC(HWND, void*) { return 1; }
MSABI void* CreateDCW(const WCHAR*, const WCHAR*, const WCHAR*, const void*) { return (void*)0xDC0001; }
MSABI BOOL DeleteDC(void*) { return 1; }
MSABI int GetDeviceCaps(void*, int idx) {
    switch (idx) {
    case 8: return W(); case 10: return H();            // HORZRES, VERTRES
    case 4: return 885; case 6: return 498;              // HORZSIZE, VERTSIZE (mm, 40" TV)
    case 12: return 32; case 14: return 1;               // BITSPIXEL, PLANES
    case 88: case 90: return 96;                         // LOGPIXELSX/Y
    case 116: return 60;                                 // VREFRESH
    }
    return 0;
}
// One pixel format: RGBA8, double-buffered, OpenGL, depth 24 + stencil 8.
void fill_pfd(uint8_t* p) {
    memset(p, 0, 40);
    *(WORD*)(p + 0) = 40; *(WORD*)(p + 2) = 1;
    *(DWORD*)(p + 4) = 0x4 | 0x20 | 0x1;     // DRAW_TO_WINDOW | SUPPORT_OPENGL | DOUBLEBUFFER
    p[8] = 0; p[9] = 32;                     // PFD_TYPE_RGBA, 32 color bits
    p[10] = 8; p[11] = 16; p[12] = 8; p[13] = 8; p[14] = 8; p[15] = 0; p[16] = 8; p[17] = 24;
    p[23] = 24; p[24] = 8;                   // depth, stencil
}
MSABI int ChoosePixelFormat(void*, const void*) { return 1; }
MSABI int DescribePixelFormat(void*, int idx, UINT size, uint8_t* pfd) {
    if (pfd && size >= 40 && idx == 1) fill_pfd(pfd);
    return 1;   // number of formats
}
MSABI BOOL SetPixelFormat(void*, int, const void*) { return 1; }
MSABI BOOL SwapBuffers(void*) { return gl::swap_buffers(); }
MSABI BOOL SetDeviceGammaRamp(void*, void*) { return 1; }
MSABI void* GetStockObject(int) { return (void*)0x5700; }
MSABI void* CreateRectRgn(int, int, int, int) { return (void*)0x5701; }
MSABI BOOL DeleteObject(void*) { return 1; }

// ---- export tables ----------------------------------------------------------------------------------
#define E(f) {#f, (void*)&f}
const std::map<std::string, void*>& user32() {
    static const std::map<std::string, void*> m = {
        E(RegisterClassExW), E(RegisterClassA), E(UnregisterClassW), E(CreateWindowExW), E(CreateWindowExA), E(DestroyWindow),
        E(ShowWindow), E(SetForegroundWindow), E(SetFocus), E(BringWindowToTop), E(GetActiveWindow), E(GetDesktopWindow),
        E(GetAncestor), E(WindowFromPoint), E(SetWindowPos), E(GetClientRect), E(GetWindowRect), E(AdjustWindowRectEx),
        E(ClientToScreen), E(ScreenToClient), E(GetWindowPlacement), E(SetWindowPlacement), E(SetWindowTextA), E(GetWindowTextA),
        E(GetWindowLongPtrW), E(GetWindowLongPtrA), E(GetWindowLongW), E(SetWindowLongPtrW), E(SetWindowLongPtrA), E(SetWindowLongW),
        E(SetPropW), E(GetPropW), E(RemovePropW), E(DefWindowProcW), E(DefWindowProcA), E(SendMessageW), E(PostMessageA),
        E(PeekMessageW), E(GetMessageA), E(DispatchMessageW), E(DispatchMessageA), E(TranslateMessage), E(GetMessageTime),
        E(GetKeyState), E(MapVirtualKeyW), E(ToUnicode), E(GetCursorPos), E(SetCursorPos), E(SetCursor), E(LoadCursorW),
        E(LoadImageW), E(DestroyIcon), E(ClipCursor), E(SetCapture), E(ReleaseCapture), E(TrackMouseEvent), E(SendInput),
        E(RegisterRawInputDevices), E(GetRawInputData), E(GetRawInputDeviceList), E(GetRawInputDeviceInfoA),
        E(RegisterDeviceNotificationW), E(UnregisterDeviceNotification), E(SystemParametersInfoW), E(MonitorFromWindow),
        E(GetMonitorInfoW), E(EnumDisplayMonitors), E(EnumDisplayDevicesW), E(EnumDisplaySettingsW), E(ChangeDisplaySettingsExW),
        E(OpenClipboard), E(CloseClipboard), E(EmptyClipboard), E(GetClipboardData), E(SetClipboardData), E(MessageBoxW),
        E(MessageBoxA), E(DialogBoxIndirectParamW), E(EndDialog), E(GetDlgItem), E(SetDlgItemTextA), E(SetDlgItemTextW),
        E(SetRect), E(OffsetRect), E(PtInRect), E(GetDC), E(ReleaseDC),
        E(SetProcessDPIAware), E(SetProcessDpiAwarenessContext), E(ChangeWindowMessageFilterEx),
        E(EnableNonClientDpiScaling), E(GetDpiForWindow), E(AdjustWindowRectExForDpi), E(GetSystemMetricsForDpi)};
    return m;
}
const std::map<std::string, void*>& gdi32() {
    static const std::map<std::string, void*> m = {
        E(CreateDCW), E(DeleteDC), E(GetDeviceCaps), E(ChoosePixelFormat), E(DescribePixelFormat), E(SetPixelFormat),
        E(SwapBuffers), E(SetDeviceGammaRamp), E(GetStockObject), E(CreateRectRgn), E(DeleteObject)};
    return m;
}
const std::map<std::string, void*>& shcore() {
    static const std::map<std::string, void*> m = {E(GetDpiForMonitor), E(SetProcessDpiAwareness)};
    return m;
}
const std::map<std::string, void*>& dwmapi() {
    static const std::map<std::string, void*> m = {E(DwmIsCompositionEnabled), E(DwmFlush), E(DwmEnableBlurBehindWindow),
                                                   E(DwmGetColorizationColor)};
    return m;
}
#undef E
}  // namespace

void init(const Hooks& h) { g_h = h; }
void* lookup(const std::string& dll, const std::string& sym) {
    const std::map<std::string, void*>* t = dll == "user32.dll" ? &user32() : dll == "gdi32.dll" ? &gdi32()
                                          : dll == "shcore.dll" ? &shcore() : dll == "dwmapi.dll" ? &dwmapi() : nullptr;
    if (!t) return nullptr;
    const auto& m = *t;
    auto it = m.find(sym);
    return it == m.end() ? nullptr : it->second;
}

}  // namespace gdps4::desktop
