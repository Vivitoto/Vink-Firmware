#pragma once
#include <Arduino.h>
#include <pgmspace.h>
#include <stdint.h>
#include <cstring>

namespace vink3 {

extern const bool g_vink_book_font_available;
extern const uint32_t g_vink_book_font_size;
extern const uint8_t g_vink_book_font_data[] PROGMEM;

inline uint8_t vinkBookFontByte(uint32_t offset) {
    if (offset >= g_vink_book_font_size) return 0;
    return pgm_read_byte(&g_vink_book_font_data[offset]);
}

inline uint16_t vinkBookFontU16(uint32_t offset) {
    uint8_t b0 = vinkBookFontByte(offset);
    uint8_t b1 = vinkBookFontByte(offset + 1);
    return static_cast<uint16_t>(b0) | (static_cast<uint16_t>(b1) << 8);
}

inline uint32_t vinkBookFontU32(uint32_t offset) {
    uint32_t b0 = vinkBookFontByte(offset);
    uint32_t b1 = vinkBookFontByte(offset + 1);
    uint32_t b2 = vinkBookFontByte(offset + 2);
    uint32_t b3 = vinkBookFontByte(offset + 3);
    return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
}

inline int8_t vinkBookFontI8(uint32_t offset) {
    return static_cast<int8_t>(vinkBookFontByte(offset));
}

} // namespace vink3
