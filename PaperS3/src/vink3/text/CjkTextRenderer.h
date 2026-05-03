#pragma once
#include <Arduino.h>
#include <M5Unified.h>
#include "../../FontManager.h"

namespace vink3 {

// v0.3 UI text renderer. It avoids M5GFX drawString for UTF-8 Chinese.
// Vink shell UI must prefer bundled Simplified Chinese SC fonts; the ReadPaper
// UI subset is fallback only so shared Han characters do not look Traditional/Japanese.
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
    struct ReadPaperGlyph {
        uint16_t unicode = 0;
        uint16_t width = 0;
        uint8_t bitmapW = 0;
        uint8_t bitmapH = 0;
        int8_t xOffset = 0;
        int8_t yOffset = 0;
        uint32_t bitmapOffset = 0;
        uint32_t bitmapSize = 0;
    };

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
    static uint8_t rpByte(uint32_t offset);
    static uint16_t rpU16(uint32_t offset);
    static uint32_t rpU32(uint32_t offset);
    static int8_t rpI8(uint32_t offset);
    static uint8_t uiByte(uint32_t offset);
    static uint16_t uiU16(uint32_t offset);
    static uint32_t uiU32(uint32_t offset);
    static int8_t uiI8(uint32_t offset);
    bool beginProgmemUiFont();
    void deriveProgmemUiMetrics();
    bool findProgmemUiGlyph(uint32_t unicode, GrayGlyph& out) const;
    bool beginReadPaperSubset();
    bool findReadPaperGlyph(uint32_t unicode, ReadPaperGlyph& out) const;
    void drawGlyph(uint32_t unicode, int16_t x, int16_t y, uint16_t color);
    void drawProgmemUiGlyph(const GrayGlyph& glyph, int16_t x, int16_t y, uint16_t color);
    void drawReadPaperGlyph(const ReadPaperGlyph& glyph, int16_t x, int16_t y, uint16_t color);
    uint16_t pixelColorForNibble(uint8_t nibble, uint16_t color) const;

    M5Canvas* canvas_ = nullptr;
    bool progmemUiReady_ = false;
    uint32_t progmemUiCharCount_ = 0;
    uint16_t progmemUiFontSize_ = 0;
    uint16_t progmemUiBaseline_ = 0;
    uint32_t progmemUiBitmapStart_ = 0;
    bool readPaperSubsetReady_ = false;
    uint32_t readPaperCharCount_ = 0;
    uint8_t readPaperFontHeight_ = 0;
    FontManager font_;
};

extern CjkTextRenderer g_cjkText;

} // namespace vink3
