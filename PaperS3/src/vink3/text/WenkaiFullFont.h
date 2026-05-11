#pragma once
#include <Arduino.h>
#include <pgmspace.h>
#include <stdint.h>
#include <cstring>

namespace vink3 {

extern const bool g_wenkai_font32_available;
extern const uint32_t g_wenkai_font32_size;
extern const uint8_t g_wenkai_font32[] PROGMEM;

inline uint8_t wenkaiFullByte(uint32_t offset) {
    if (offset >= g_wenkai_font32_size) return 0;
    return pgm_read_byte(&g_wenkai_font32[offset]);
}

inline uint16_t wenkaiFullU16(uint32_t offset) {
    uint8_t b0 = wenkaiFullByte(offset);
    uint8_t b1 = wenkaiFullByte(offset + 1);
    return static_cast<uint16_t>(b0) | (static_cast<uint16_t>(b1) << 8);
}

inline uint32_t wenkaiFullU32(uint32_t offset) {
    uint32_t b0 = wenkaiFullByte(offset);
    uint32_t b1 = wenkaiFullByte(offset + 1);
    uint32_t b2 = wenkaiFullByte(offset + 2);
    uint32_t b3 = wenkaiFullByte(offset + 3);
    return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
}

inline int8_t wenkaiFullI8(uint32_t offset) {
    return static_cast<int8_t>(wenkaiFullByte(offset));
}

} // namespace vink3
