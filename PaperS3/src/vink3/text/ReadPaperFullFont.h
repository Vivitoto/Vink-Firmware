#pragma once
#include <Arduino.h>
#include <pgmspace.h>
#include <stdint.h>
#include <cstring>

namespace vink3 {

extern const bool g_readpaper_full_font_available;
extern const uint32_t g_readpaper_full_font_size;
extern const uint8_t g_readpaper_full_font_data[] PROGMEM;

inline uint8_t readpaperFullByte(uint32_t offset) {
    if (offset >= g_readpaper_full_font_size) return 0;
    return pgm_read_byte(&g_readpaper_full_font_data[offset]);
}

inline uint16_t readpaperFullU16(uint32_t offset) {
    uint8_t b0 = readpaperFullByte(offset);
    uint8_t b1 = readpaperFullByte(offset + 1);
    return static_cast<uint16_t>(b0) | (static_cast<uint16_t>(b1) << 8);
}

inline uint32_t readpaperFullU32(uint32_t offset) {
    uint32_t b0 = readpaperFullByte(offset);
    uint32_t b1 = readpaperFullByte(offset + 1);
    uint32_t b2 = readpaperFullByte(offset + 2);
    uint32_t b3 = readpaperFullByte(offset + 3);
    return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
}

inline int8_t readpaperFullI8(uint32_t offset) {
    return static_cast<int8_t>(readpaperFullByte(offset));
}

} // namespace vink3
