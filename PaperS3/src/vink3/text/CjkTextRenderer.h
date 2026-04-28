#pragma once
#include <Arduino.h>
#include <M5Unified.h>
#include "../../FontManager.h"

namespace vink3 {

// v0.3 UI text renderer. It avoids M5GFX drawString for UTF-8 Chinese and uses
// the bundled CJK bitmap font path instead. This is the minimal safe bridge while
// the larger ReadPaper text/book engine is being ported.
class CjkTextRenderer {
public:
    bool begin(M5Canvas* canvas);
    bool ready() const;
    uint16_t fontSize() const;
    int16_t textWidth(const char* text);
    void drawText(int16_t x, int16_t y, const char* text, uint16_t color = TFT_BLACK);
    void drawCentered(int16_t x, int16_t y, int16_t w, int16_t h, const char* text, uint16_t color = TFT_BLACK);
    void drawRight(int16_t rightX, int16_t y, const char* text, uint16_t color = TFT_BLACK);

private:
    static uint32_t decodeUtf8(const uint8_t* buf, size_t& pos, size_t len);
    void drawGlyph(uint32_t unicode, int16_t x, int16_t y, uint16_t color);
    uint16_t pixelColorForNibble(uint8_t nibble, uint16_t color) const;

    M5Canvas* canvas_ = nullptr;
    FontManager font_;
};

extern CjkTextRenderer g_cjkText;

} // namespace vink3
