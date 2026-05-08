#pragma once
#include "ReadPaperFullFont.h"

namespace vink3 {

// Vink-branded adapter around the existing PROGMEM book-font blob. Keeping the
// large generated blob under its original file avoids duplicating ~47MB of
// generated C++ while letting reader code use Vink-owned names.
static const bool& g_vink_book_font_available = g_readpaper_full_font_available;
static const uint32_t& g_vink_book_font_size = g_readpaper_full_font_size;

inline uint8_t vinkBookFontByte(uint32_t offset) {
    return readpaperFullByte(offset);
}

inline uint16_t vinkBookFontU16(uint32_t offset) {
    return readpaperFullU16(offset);
}

inline uint32_t vinkBookFontU32(uint32_t offset) {
    return readpaperFullU32(offset);
}

inline int8_t vinkBookFontI8(uint32_t offset) {
    return readpaperFullI8(offset);
}

} // namespace vink3
