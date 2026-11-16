// See gdi_text.hpp. Structure layouts follow wingdi.h / winuser.h for x64.
#include "gdi_text.hpp"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "../third_party/stb/stb_truetype.h"

#ifndef GDPS4_FONT
#error "build must define GDPS4_FONT as the path of the embedded .ttf"
#endif
// Embed the default font in the binary (no file to stage, works the same on PC and PS4).
#ifdef _WIN32
__asm__(".section .rdata,\"dr\"\n.globl gdps4_font_start\ngdps4_font_start:\n.incbin \"" GDPS4_FONT "\"\n"
        ".globl gdps4_font_end\ngdps4_font_end:\n.byte 0\n.text\n");
#else
__asm__(".section .rodata\n.globl gdps4_font_start\ngdps4_font_start:\n.incbin \"" GDPS4_FONT "\"\n"
        ".globl gdps4_font_end\ngdps4_font_end:\n.byte 0\n.previous\n");
#endif
extern "C" const unsigned char gdps4_font_start[], gdps4_font_end[];

#define MSABI __attribute__((ms_abi))

namespace gdps4::gditext {
namespace {
using BOOL = int; using UINT = unsigned; using LONG = int32_t; using COLORREF = uint32_t;
struct RECT { LONG left, top, right, bottom; };
struct SIZE { LONG cx, cy; };
struct LOGFONTA {                   // 60 bytes
    LONG lfHeight, lfWidth, lfEscapement, lfOrientation, lfWeight;
    uint8_t lfItalic, lfUnderline, lfStrikeOut, lfCharSet, lfOutPrecision, lfClipPrecision, lfQuality, lfPitchAndFamily;
    char lfFaceName[32];
};
static_assert(sizeof(LOGFONTA) == 60, "LOGFONTA layout");

Hooks g_h;
void log(const char* fmt, ...) {
    char b[512]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    if (g_h.log) g_h.log(b);
}

struct Face { std::string family; std::vector<unsigned char> owned; const unsigned char* data; stbtt_fontinfo info; };
std::vector<std::unique_ptr<Face>> g_faces;   // [0] = embedded default
std::map<std::u16string, size_t> g_added;     // AddFontResourceW path -> face index

struct Obj {
    enum Kind { Font, Bitmap, Dc, Other } kind;
    LOGFONTA lf{};                            // Font
    int w = 0, h = 0; std::vector<uint32_t> px;   // Bitmap: 0x00RRGGBB, top-down
    uintptr_t font = 0, bitmap = 0;           // Dc
    COLORREF text = 0, bk = 0xFFFFFF; int bkmode = 2 /*OPAQUE*/;
};
std::recursive_mutex g_mx;
std::map<uintptr_t, Obj> g_objs;
uintptr_t g_next = 0x6A0010;
uintptr_t g_stock[32];

uintptr_t make(Obj o) { uintptr_t h = g_next; g_next += 0x10; g_objs.emplace(h, std::move(o)); return h; }
Obj* get(const void* h) { auto it = g_objs.find((uintptr_t)h); return it == g_objs.end() ? nullptr : &it->second; }

std::string family_of(const stbtt_fontinfo& f) {
    int len = 0;   // name id 1 (family), Microsoft/Unicode/English-US
    const char* s = stbtt_GetFontNameString(&f, &len, STBTT_PLATFORM_ID_MICROSOFT, STBTT_MS_EID_UNICODE_BMP, STBTT_MS_LANG_ENGLISH, 1);
    std::string out;
    for (int i = 0; s && i + 1 < len; i += 2) out += (char)s[i + 1];   // UTF-16BE, ASCII names
    return out;
}
std::string lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }
void init_default_face() {
    if (!g_faces.empty()) return;
    auto f = std::make_unique<Face>(); f->data = gdps4_font_start;
    if (!stbtt_InitFont(&f->info, f->data, stbtt_GetFontOffsetForIndex(f->data, 0))) { log("gdi: embedded font failed to load\n"); return; }
    f->family = family_of(f->info);
    log("gdi: text rendering via stb_truetype, default face '%s' (%u KB)\n", f->family.c_str(), (unsigned)((gdps4_font_end - gdps4_font_start) >> 10));
    g_faces.push_back(std::move(f));
}
Face* face_for(const LOGFONTA& lf) {
    init_default_face();
    if (g_faces.empty()) return nullptr;
    std::string want = lower(std::string(lf.lfFaceName, strnlen(lf.lfFaceName, 32)));
    for (auto& f : g_faces) if (!want.empty() && lower(f->family) == want) return f.get();
    return g_faces[0].get();   // Arial, Helvetica, MS Shell Dlg, ... -> metric-compatible default
}
float scale_for(Face* f, const LOGFONTA& lf) {
    int h = lf.lfHeight ? lf.lfHeight : -12;
    // GDI: negative = character (em) height, positive = cell height (ascent - descent).
    return h < 0 ? stbtt_ScaleForMappingEmToPixels(&f->info, (float)-h) : stbtt_ScaleForPixelHeight(&f->info, (float)h);
}

// ---- text layout ------------------------------------------------------------------------------
struct Metrics { Face* f; float scale; int ascent, line; bool bold; };
Metrics metrics(const LOGFONTA& lf) {
    Metrics m{face_for(lf), 0, 0, 0, lf.lfWeight >= 700};
    if (!m.f) return m;
    m.scale = scale_for(m.f, lf);
    int a, d, g; stbtt_GetFontVMetrics(&m.f->info, &a, &d, &g);
    m.ascent = (int)std::ceil(a * m.scale);
    m.line = (int)std::ceil((a - d) * m.scale);   // GDI tmHeight: ascent + descent, no line gap
    return m;
}
float advance(const Metrics& m, char32_t c, char32_t next) {
    int adv, lsb; stbtt_GetCodepointHMetrics(&m.f->info, (int)c, &adv, &lsb);
    float w = adv * m.scale;
    if (next) w += stbtt_GetCodepointKernAdvance(&m.f->info, (int)c, (int)next) * m.scale;
    return w + (m.bold ? 1.f : 0.f);
}
float width_of(const Metrics& m, const std::u32string& s) {
    float w = 0;
    for (size_t i = 0; i < s.size(); i++) w += advance(m, s[i], i + 1 < s.size() ? s[i + 1] : 0);
    return w;
}
std::u32string decode(const char16_t* s, int n) {
    std::u32string out;
    for (int i = 0; i < n; i++) {
        char32_t c = s[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00); i++; }
        out += c;
    }
    return out;
}
// Splits into lines on \n (\r dropped); with DT_WORDBREAK, wraps at spaces to fit max_w.
std::vector<std::u32string> layout(const Metrics& m, const std::u32string& text, bool wrap, float max_w, bool single) {
    std::vector<std::u32string> lines; std::u32string cur;
    auto flush_para = [&](const std::u32string& para) {
        if (!wrap) { lines.push_back(para); return; }
        std::u32string line; size_t i = 0;
        while (i <= para.size()) {
            size_t j = para.find(U' ', i); if (j == std::u32string::npos) j = para.size();
            std::u32string word = para.substr(i, j - i);
            std::u32string cand = line.empty() ? word : line + U' ' + word;
            if (!line.empty() && width_of(m, cand) > max_w) { lines.push_back(line); line = word; }
            else line = cand;
            i = j + 1;
        }
        lines.push_back(line);
    };
    for (char32_t c : text) {
        if (c == U'\r') continue;
        if (c == U'\n' && !single) { flush_para(cur); cur.clear(); continue; }
        cur += c == U'\t' ? U' ' : c;
    }
    flush_para(cur);
    return lines;
}
void draw_line(Obj& bmp, const Metrics& m, const std::u32string& s, float x, int baseline, COLORREF color) {
    uint32_t r = color & 0xFF, g = (color >> 8) & 0xFF, b = (color >> 16) & 0xFF;
    for (int pass = 0; pass < (m.bold ? 2 : 1); pass++) {
        float pen = x + pass;
        for (size_t i = 0; i < s.size(); i++) {
            char32_t c = s[i];
            float fx = pen - std::floor(pen);
            int x0, y0, x1, y1;
            stbtt_GetCodepointBitmapBoxSubpixel(&m.f->info, (int)c, m.scale, m.scale, fx, 0, &x0, &y0, &x1, &y1);
            int gw = x1 - x0, gh = y1 - y0;
            if (gw > 0 && gh > 0) {
                std::vector<unsigned char> cov((size_t)gw * gh);
                stbtt_MakeCodepointBitmapSubpixel(&m.f->info, cov.data(), gw, gh, gw, m.scale, m.scale, fx, 0, (int)c);
                int ox = (int)std::floor(pen) + x0, oy = baseline + y0;
                for (int yy = 0; yy < gh; yy++) {
                    int py = oy + yy; if (py < 0 || py >= bmp.h) continue;
                    for (int xx = 0; xx < gw; xx++) {
                        int pxx = ox + xx; if (pxx < 0 || pxx >= bmp.w) continue;
                        uint32_t a = cov[(size_t)yy * gw + xx]; if (!a) continue;
                        uint32_t& d = bmp.px[(size_t)py * bmp.w + pxx];
                        uint32_t dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
                        dr = (dr * (255 - a) + r * a) / 255; dg = (dg * (255 - a) + g * a) / 255; db = (db * (255 - a) + b * a) / 255;
                        d = (dr << 16) | (dg << 8) | db;   // GDI leaves the alpha byte 0
                    }
                }
            }
            pen += advance(m, c, i + 1 < s.size() ? s[i + 1] : 0) - (m.bold ? 1.f : 0.f);
        }
    }
}

// ---- gdi32 ------------------------------------------------------------------------------------------
MSABI void* CreateCompatibleDC(void*) {
    std::lock_guard<std::recursive_mutex> l(g_mx);
    Obj bmp{Obj::Bitmap}; bmp.w = bmp.h = 1; bmp.px.assign(1, 0);
    Obj dc{Obj::Dc}; dc.bitmap = make(std::move(bmp));
    return (void*)make(std::move(dc));
}
MSABI BOOL DeleteDC(void* h) { std::lock_guard<std::recursive_mutex> l(g_mx); g_objs.erase((uintptr_t)h); return 1; }
MSABI BOOL DeleteObject(void* h) {
    std::lock_guard<std::recursive_mutex> l(g_mx);
    for (uintptr_t s : g_stock) if (s == (uintptr_t)h) return 1;   // stock objects are never freed
    g_objs.erase((uintptr_t)h); return 1;
}
MSABI void* GetStockObject(int i) {
    std::lock_guard<std::recursive_mutex> l(g_mx);
    if (i < 0 || i >= 32) return nullptr;
    if (!g_stock[i]) {
        if (i >= 10 && i <= 17 && i != 15) {   // OEM_FIXED .. DEFAULT_GUI_FONT
            Obj f{Obj::Font}; f.lf.lfHeight = -11; f.lf.lfWeight = 400; strcpy(f.lf.lfFaceName, "MS Shell Dlg");
            g_stock[i] = make(std::move(f));
        } else g_stock[i] = make(Obj{Obj::Other});
    }
    return (void*)g_stock[i];
}
MSABI void* CreateFontIndirectA(const LOGFONTA* lf) {
    if (!lf) return nullptr;
    std::lock_guard<std::recursive_mutex> l(g_mx);
    Obj f{Obj::Font}; f.lf = *lf;
    return (void*)make(std::move(f));
}
MSABI int GetObjectA(void* h, int size, void* out) {
    std::lock_guard<std::recursive_mutex> l(g_mx);
    Obj* o = get(h); if (!o) return 0;
    if (o->kind == Obj::Font) {
        if (!out) return sizeof(LOGFONTA);
        int n = std::min(size, (int)sizeof(LOGFONTA)); memcpy(out, &o->lf, n); return n;
    }
    if (o->kind == Obj::Bitmap) {   // BITMAP: type, width, height, widthbytes, planes(2), bpp(2), bits(8)
        if (!out) return 32;
        uint8_t b[32] = {}; int32_t v[4] = {0, o->w, o->h, o->w * 4}; memcpy(b, v, 16);
        uint16_t pb[2] = {1, 32}; memcpy(b + 16, pb, 4);
        int n = std::min(size, 32); memcpy(out, b, n); return n;
    }
    return 0;
}
MSABI void* CreateBitmap(int w, int h, UINT, UINT bpp, const void* bits) {
    std::lock_guard<std::recursive_mutex> l(g_mx);
    Obj b{Obj::Bitmap}; b.w = std::max(w, 1); b.h = std::max(h, 1); b.px.assign((size_t)b.w * b.h, 0);
    if (bits && bpp == 32) for (size_t i = 0; i < b.px.size(); i++) b.px[i] = ((const uint32_t*)bits)[i] & 0xFFFFFF;
    return (void*)make(std::move(b));
}
MSABI void* SelectObject(void* dc, void* h) {
    std::lock_guard<std::recursive_mutex> l(g_mx);
    Obj* d = get(dc); Obj* o = get(h);
    if (!d || d->kind != Obj::Dc || !o) return nullptr;
    uintptr_t old = 0;
    if (o->kind == Obj::Font) { old = d->font; d->font = (uintptr_t)h; if (!old) old = (uintptr_t)GetStockObject(13); }
    else if (o->kind == Obj::Bitmap) { old = d->bitmap; d->bitmap = (uintptr_t)h; }
    else return (void*)h;
    return (void*)old;
}
MSABI COLORREF SetTextColor(void* dc, COLORREF c) {
    std::lock_guard<std::recursive_mutex> l(g_mx);
    Obj* d = get(dc); if (!d) return 0xFFFFFFFF;
    COLORREF old = d->text; d->text = c; return old;
}
MSABI int SetBkMode(void* dc, int mode) {
    std::lock_guard<std::recursive_mutex> l(g_mx);
    Obj* d = get(dc); if (!d) return 0;
    int old = d->bkmode; d->bkmode = mode; return old;
}
MSABI int GetDIBits(void* dc, void* hbm, UINT start, UINT lines, void* bits, uint8_t* bi, UINT) {
    std::lock_guard<std::recursive_mutex> l(g_mx);
    Obj* b = get(hbm); if (!b || b->kind != Obj::Bitmap || !bi) return 0;
    (void)dc;
    if (!bits) {   // query: fill BITMAPINFOHEADER
        int32_t v[3] = {40, b->w, b->h}; memcpy(bi, v, 12);
        uint16_t pb[2] = {1, 32}; memcpy(bi + 12, pb, 4);
        uint32_t rest[6] = {0 /*BI_RGB*/, (uint32_t)(b->w * b->h * 4), 0, 0, 0, 0}; memcpy(bi + 16, rest, 24);
        return b->h;
    }
    int32_t want_h; memcpy(&want_h, bi + 8, 4);
    bool top_down = want_h < 0;
    UINT n = 0;
    for (UINT i = 0; i < lines && start + i < (UINT)b->h; i++, n++) {
        // Scan line k of the DIB is row k from the bottom unless the header asks for top-down.
        UINT scan = start + i;
        int src_row = top_down ? (int)scan : b->h - 1 - (int)scan;
        memcpy((uint32_t*)bits + (size_t)i * b->w, &b->px[(size_t)src_row * b->w], (size_t)b->w * 4);
    }
    return (int)n;
}
MSABI int AddFontResourceW(const char16_t* path) {
    if (!path) return 0;
    std::lock_guard<std::recursive_mutex> l(g_mx);
    std::u16string p(path);
    if (g_added.count(p)) return 1;
    std::string host = g_h.map_path ? g_h.map_path(p) : std::string(p.begin(), p.end());
    std::ifstream in(host, std::ios::binary);
    std::vector<unsigned char> data{std::istreambuf_iterator<char>(in), {}};
    auto f = std::make_unique<Face>(); f->owned = std::move(data); f->data = f->owned.data();
    if (f->owned.empty() || !stbtt_InitFont(&f->info, f->data, stbtt_GetFontOffsetForIndex(f->data, 0))) {
        log("gdi: AddFontResourceW(%s) failed\n", host.c_str()); return 0;
    }
    f->family = family_of(f->info);
    log("gdi: AddFontResourceW(%s) -> '%s'\n", host.c_str(), f->family.c_str());
    init_default_face();
    g_added[p] = g_faces.size(); g_faces.push_back(std::move(f));
    return 1;
}
MSABI BOOL RemoveFontResourceW(const char16_t*) { return 1; }   // faces stay loaded; handles may still use them

// ---- user32 DrawTextW -----------------------------------------------------------------------------
MSABI int DrawTextW(void* dc, const char16_t* str, int n, RECT* rc, UINT fmt) {
    if (!str || !rc) return 0;
    std::lock_guard<std::recursive_mutex> l(g_mx);
    Obj* d = get(dc); if (!d || d->kind != Obj::Dc) return 0;
    Obj* fo = get((void*)(d->font ? d->font : (uintptr_t)GetStockObject(13)));
    if (!fo || fo->kind != Obj::Font) return 0;
    Metrics m = metrics(fo->lf); if (!m.f) return 0;
    if (n < 0) { n = 0; while (str[n]) n++; }
    const bool calc = fmt & 0x400, wrap = fmt & 0x10, single = fmt & 0x20;
    float box_w = (float)(rc->right - rc->left);
    auto lines = layout(m, decode(str, n), wrap && !single, box_w, single);
    int text_h = (int)lines.size() * m.line;
    static int logged = 0;
    if (logged < 8) {
        logged++; std::string a; for (int i = 0; i < n && i < 40; i++) a += str[i] < 128 ? (char)str[i] : '?';
        log("gdi: DrawTextW('%s', fmt %#x, rect %dx%d, font '%s' h=%d w=%d) -> %zu lines, h=%d\n", a.c_str(), fmt,
            rc->right - rc->left, rc->bottom - rc->top, fo->lf.lfFaceName, fo->lf.lfHeight, fo->lf.lfWeight, lines.size(), text_h);
    }
    if (calc) {
        float wmax = 0; for (auto& s : lines) wmax = std::max(wmax, width_of(m, s));
        rc->right = rc->left + (int)std::ceil(wmax);
        rc->bottom = rc->top + text_h;
        return text_h;
    }
    Obj* bmp = get((void*)d->bitmap); if (!bmp || bmp->kind != Obj::Bitmap) return text_h;
    int y = rc->top;
    if (single && (fmt & 0x8)) y = rc->bottom - m.line;                              // DT_BOTTOM
    else if (single && (fmt & 0x4)) y = rc->top + (rc->bottom - rc->top - m.line) / 2;   // DT_VCENTER
    for (auto& s : lines) {
        float w = width_of(m, s), x = (float)rc->left;
        if (fmt & 0x1) x = rc->left + (box_w - w) / 2;   // DT_CENTER
        else if (fmt & 0x2) x = rc->right - w;            // DT_RIGHT
        if (d->bkmode == 2) {                             // OPAQUE: fill the line cell with the bk colour
            uint32_t bk = ((d->bk & 0xFF) << 16) | (d->bk & 0xFF00) | ((d->bk >> 16) & 0xFF);
            for (int yy = std::max(y, 0); yy < std::min(y + m.line, bmp->h); yy++)
                for (int xx = std::max((int)x, 0); xx < std::min((int)std::ceil(x + w), bmp->w); xx++) bmp->px[(size_t)yy * bmp->w + xx] = bk;
        }
        draw_line(*bmp, m, s, x, y + m.ascent, d->text);
        y += m.line;
    }
    return text_h;
}

#define E(f) {#f, (void*)&f}
const std::map<std::string, void*>& gdi32() {
    static const std::map<std::string, void*> t = {E(CreateCompatibleDC), E(DeleteDC), E(DeleteObject), E(GetStockObject),
        E(CreateFontIndirectA), E(GetObjectA), E(CreateBitmap), E(SelectObject), E(SetTextColor), E(SetBkMode),
        E(GetDIBits), E(AddFontResourceW), E(RemoveFontResourceW)};
    return t;
}
#undef E
}  // namespace

void init(const Hooks& h) { g_h = h; }
void* lookup(const std::string& dll, const std::string& sym) {
    if (dll == "user32.dll") return sym == "DrawTextW" ? (void*)&DrawTextW : nullptr;
    if (dll != "gdi32.dll") return nullptr;
    auto it = gdi32().find(sym);
    return it == gdi32().end() ? nullptr : it->second;
}
}  // namespace gdps4::gditext
