// GDI text rendering (gdi32 memory DCs, fonts, bitmaps, GetDIBits + user32 DrawTextW) for
// platforms without GDI. cocos2d's Win32 CCImage::initWithString renders CCLabelTTF text
// through exactly this API surface; glyphs come from stb_truetype and an embedded font
// (Liberation Sans, metric-compatible with Arial) plus any .ttf the game registers with
// AddFontResourceW.
#pragma once
#include <functional>
#include <string>

namespace gdps4::gditext {
struct Hooks {
    void (*log)(const char*) = nullptr;
    std::function<std::string(const std::u16string& win_path)> map_path;   // AddFontResourceW path -> host path
};
void init(const Hooks& h);
// gdi32.dll / user32.dll exports this layer owns, or nullptr.
void* lookup(const std::string& dll, const std::string& sym);
}  // namespace gdps4::gditext
