#pragma once
#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>

namespace vink3 {

struct GbkUnicodeEntry {
    uint16_t gbk;
    uint16_t unicode;
};

extern const GbkUnicodeEntry kGbkUnicodeTable[] PROGMEM;
extern const size_t kGbkUnicodeTableSize;

uint16_t gbkToUnicode(uint16_t gbk);
int unicodeToUtf8(uint16_t unicode, uint8_t* out);

} // namespace vink3
