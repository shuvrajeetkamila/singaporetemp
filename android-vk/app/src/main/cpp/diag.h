// On-screen diagnostics: a 5x7 text drawer that works on a plain pixel buffer, so a failure can be
// read from the phone screen without logcat. Portable (no Android headers) so it can be tested on a desktop.
#pragma once

#include <cstdint>

// Pixels are packed 0xAABBGGRR as in an RGBA_8888 little-endian buffer: r | g<<8 | b<<16 | a<<24.
struct PixelBuffer {
    uint32_t* px;
    int width;
    int height;
    int stride;  // in pixels
};

// Packs an opaque colour for PixelBuffer.
constexpr uint32_t packRgba(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint32_t>(r) | (static_cast<uint32_t>(g) << 8) | (static_cast<uint32_t>(b) << 16) |
           (0xFFu << 24);
}

void fillPixels(const PixelBuffer& b, uint32_t colour);

// Draws `count` text lines from the top-left corner, scaled to fit the buffer width.
// Unknown characters are drawn as blanks. Returns the pixel scale used (0 if nothing was drawn).
int drawTextLines(const PixelBuffer& b, const char* const* lines, int count, uint32_t colour);

// True if the 5x7 font has a glyph for `c` (letters are case-insensitive).
bool hasGlyph(char c);

// Android-only (diag_android.cpp): stage and detail strings shown if the app fails or crashes.
void diagSetStage(const char* stage);
void diagSetDetail(const char* detail);
const char* diagStage();
const char* diagDetail();
void diagSetWindow(struct ANativeWindow* window);
void diagInstallCrashHandler();
// Paints the lines on the window, without Vulkan. Returns false if the window cannot be locked.
bool diagPaint(struct ANativeWindow* window, uint32_t background, const char* const* lines, int count);
