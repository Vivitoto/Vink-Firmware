#pragma once
#include <Arduino.h>
#include <M5Unified.h>
#include "../../FontManager.h"

namespace vink3 {

struct ReaderRenderOptions {
    uint8_t fontSize = 24;
    int16_t marginLeft = 34;
    int16_t marginTop = 86;
    int16_t marginRight = 30;
    int16_t marginBottom = 46;
    int16_t lineGap = 12;
    uint8_t indentFirstLine = 2;
    uint8_t paragraphSpacing = 50;
    bool justify = false;
    bool vertical = false;
    bool dark = false;
};

class ReaderTextRenderer {
public:
    bool begin(M5Canvas* canvas);
    bool loadDefaultFont();
    bool loadFont(const char* path);
    bool ready() const;
    uint16_t fontSize() const;
    M5Canvas* canvas() const { return canvas_; }
    void setOptionsFontSize(uint8_t s) { optionsFontSize_ = s; }

    void renderPlaceholderPage();
    void renderTextPage(const char* title, const char* body, uint16_t page, uint16_t totalPages, const ReaderRenderOptions& options = ReaderRenderOptions{});
    void renderListPage(const char* title, const char* summary, const char* const* rows, int rowCount, int16_t rowY, int16_t rowH, uint16_t page, uint16_t totalPages, int activeTab = 0, const ReaderRenderOptions& options = ReaderRenderOptions{});
    void renderActionPage(const char* title, const char* const* infoLines, int infoCount, const char* const* actions, int actionCount, int activeTab = 0, const ReaderRenderOptions& options = ReaderRenderOptions{});
    size_t measurePageBytes(const char* text, size_t len, const ReaderRenderOptions& options = ReaderRenderOptions{}) const;

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

    static uint32_t decodeUtf8(const uint8_t* buf, size_t& pos, size_t len);
    bool beginReadPaperFullFont();
    bool findReadPaperGlyph(uint32_t unicode, ReadPaperGlyph& out) const;
    uint8_t charAdvance(uint32_t unicode) const;
    int16_t textWidth(const char* text) const;
    void drawGlyph(uint32_t unicode, int16_t x, int16_t y, uint16_t color);
    void drawReadPaperGlyph(const ReadPaperGlyph& glyph, int16_t x, int16_t y, uint16_t color);
    uint16_t pixelColorForNibble(uint8_t nibble, uint16_t color) const;
    void drawText(int16_t x, int16_t y, const char* text, uint16_t color = TFT_BLACK);
    void drawShellTabs(int activeTab, const ReaderRenderOptions& options);
    size_t findWrapBreak(const char* text, size_t start, int16_t maxWidth) const;

    M5Canvas* canvas_ = nullptr;
    bool readPaperFullReady_ = false;
    uint32_t readPaperCharCount_ = 0;
    uint8_t readPaperFontHeight_ = 0;
    uint8_t optionsFontSize_ = 0;
    FontManager font_;
};

extern ReaderTextRenderer g_readerText;

} // namespace vink3
