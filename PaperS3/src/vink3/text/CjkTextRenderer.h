#pragma once
#include <Arduino.h>
#include <M5Unified.h>
#include "../../FontManager.h"

namespace vink3 {

// UI text renderer with dual-size support:
// - 24px Noto Sans CJK SC Bold (PROGMEM, primary)
// - 16px Noto Sans CJK SC Bold (SPIFFS, small labels/subtitles)
class CjkTextRenderer {
public:
    bool begin(M5Canvas* canvas);
    bool ready() const;
    uint16_t fontSize() const;
    uint16_t fontSizeSmall() const { return 16; }
    bool hasSmallFont() const { return fontSmall_.isLoaded(); }
    int16_t textWidth(const char* text);
    int16_t textWidthSmall(const char* text);
    void fitTextToWidth(const char* text, char* out, size_t outSize, int16_t maxWidth);
    void drawText(int16_t x, int16_t y, const char* text, uint16_t color = TFT_BLACK);
    void drawTextSmall(int16_t x, int16_t y, const char* text, uint16_t color = TFT_BLACK);
    void drawCentered(int16_t x, int16_t y, int16_t w, int16_t h, const char* text, uint16_t color = TFT_BLACK);
    void drawCenteredSmall(int16_t x, int16_t y, int16_t w, int16_t h, const char* text, uint16_t color = TFT_BLACK);
    void drawRight(int16_t rightX, int16_t y, const char* text, uint16_t color = TFT_BLACK);

private:
    struct GrayGlyph {
        uint32_t unicode = 0;
        uint32_t bitmapOffset = 0;
        uint8_t width = 0;
        uint8_t height = 0;
        int8_t bearingX = 0;
        int8_t bearingY = 0;
        uint8_t advance = 0;
    };

    static uint32_t decodeUtf8(const uint8_t* buf, size_t& pos, size_t len);
    static uint8_t uiByte(uint32_t offset);
    static uint16_t uiU16(uint32_t offset);
    static uint32_t uiU32(uint32_t offset);
    static int8_t uiI8(uint32_t offset);
    bool beginProgmemUiFont();
    void deriveProgmemUiMetrics();
    bool findProgmemUiGlyph(uint32_t unicode, GrayGlyph& out) const;
    void drawGlyph(uint32_t unicode, int16_t x, int16_t y, uint16_t color);
    void drawProgmemUiGlyph(const GrayGlyph& glyph, int16_t x, int16_t y, uint16_t color);
    uint16_t pixelColorForNibble(uint8_t nibble, uint16_t color) const;

    M5Canvas* canvas_ = nullptr;
    bool progmemUiReady_ = false;
    uint32_t progmemUiCharCount_ = 0;
    uint16_t progmemUiFontSize_ = 0;
    uint16_t progmemUiBaseline_ = 0;
    uint32_t progmemUiBitmapStart_ = 0;
    FontManager fontSmall_;
};

extern CjkTextRenderer g_cjkText;

} // namespace vink3
