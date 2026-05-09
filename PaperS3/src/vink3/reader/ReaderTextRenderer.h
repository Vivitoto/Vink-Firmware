#pragma once
#include <Arduino.h>
#include <M5Unified.h>
#include "../../FontManager.h"

namespace vink3 {

enum class ReaderShowMode : uint8_t {
    High = 0,
    Std = 1,
    Fast = 2,
};

enum class ReaderFlashPeriod : uint8_t {
    PerPage = 0,
    Pages5 = 1,
    Pages10 = 2,
    Pages20 = 3,
    Never = 4,
};

enum class ReaderFastTurnMode : uint8_t {
    SpeedPriority = 0,
    DisplayPriority = 1,
};

struct ReaderSettings {
    uint8_t schema = 1;
    // Vink reader-core model: formatting1, render_opt1 and spacing are
    // packed 2-bit option slots. Keep the raw words so the internal renderer,
    // settings UI, and pagination cache all share one compact layout schema.
    uint16_t formatting1 = 0;
    uint16_t renderOpt1 = 0;
    uint16_t spacing = 0;
    ReaderShowMode showMode = ReaderShowMode::Std;
    ReaderFlashPeriod flashPeriod = ReaderFlashPeriod::Pages10;
    ReaderFastTurnMode fastTurn = ReaderFastTurnMode::DisplayPriority;
    uint8_t layoutAlgorithmVersion = 2;
    uint8_t fontMetricVersion = 1;

    static uint8_t getSlot(uint16_t raw, uint8_t idx) { return (raw >> (idx * 2)) & 0x3; }
    static void setSlot(uint16_t& raw, uint8_t idx, uint8_t value) {
        raw = (raw & ~(0x3u << (idx * 2))) | ((value & 0x3u) << (idx * 2));
    }

    bool indentEnabled() const { return getSlot(formatting1, 0) != 0; }
    bool blankLineOptEnabled() const { return getSlot(formatting1, 1) != 0; }
    bool breakLineOptEnabled() const { return getSlot(formatting1, 2) != 0; }
    bool newPageEnabled() const { return getSlot(formatting1, 3) != 0; }
    bool dynamicLineHeightEnabled() const { return getSlot(formatting1, 4) != 0; }

    bool underlineEnabled() const { return getSlot(renderOpt1, 0) != 0; }
    bool antiAliasEnabled() const { return getSlot(renderOpt1, 1) != 0; }
    bool notchLockEnabled() const { return getSlot(renderOpt1, 2) != 0; }
    bool pageTurnEffectEnabled() const { return getSlot(renderOpt1, 3) != 0; }
    bool textShadowEnabled() const { return getSlot(renderOpt1, 4) != 0; }

    uint8_t topBottomLevel() const { return getSlot(spacing, 0); }
    uint8_t leftRightLevel() const { return getSlot(spacing, 1); }
    uint8_t lineSpacingLevel() const { return getSlot(spacing, 2); }
    uint8_t letterSpacingLevel() const { return getSlot(spacing, 3); }
    uint8_t paragraphSpacingLevel() const { return getSlot(spacing, 4); }
    uint8_t underlineOffsetLevel() const { return getSlot(spacing, 5); }
};

struct ReaderRenderOptions {
    uint8_t fontSize = 24;
    int16_t marginLeft = 30;
    int16_t marginTop = 78;
    int16_t marginRight = 28;
    int16_t marginBottom = 34;
    int16_t lineGap = 8;
    int16_t firstLineIndentPx = 0;
    int16_t letterGap = 0;
    int16_t paragraphGap = 0;
    int16_t underlineOffset = 2;
    bool vertical = false;
    bool dark = false;
    bool indentFirstLine = false;
    bool compactBlankLines = false;
    bool dynamicLineHeight = false;
    bool breakLineOpt = false;
    bool underline = false;
    bool justify = false;
    bool startsAtParagraph = true;
};

class ReaderTextRenderer {
public:
    bool begin(M5Canvas* canvas);
    bool loadDefaultFont();
    bool loadFont(const char* path);
    bool ready() const;
    uint16_t fontSize() const;
    void setReaderFontSize(uint8_t size);
    uint8_t readerFontSizeSetting() const { return fontSizeSetting_; }
    void toggleAntiAlias();
    void setAntiAlias(bool enabled);
    bool antiAliasEnabled() const { return settings_.antiAliasEnabled(); }
    const char* antiAliasLabel() const { return antiAliasEnabled() ? "开启" : "关闭"; }
    void toggleUnderline();
    void setUnderline(bool enabled);
    bool underlineEnabled() const { return settings_.underlineEnabled(); }
    const char* underlineLabel() const { return underlineEnabled() ? "开启" : "关闭"; }
    void togglePageTurnEffect();
    void setPageTurnEffect(bool enabled);
    bool pageTurnEffectEnabled() const { return settings_.pageTurnEffectEnabled(); }
    const char* pageTurnEffectLabel() const { return pageTurnEffectEnabled() ? "开启" : "关闭"; }
    void cyclePageMargin();
    void setPageMarginLevel(uint8_t level);
    uint8_t pageMarginLevel() const { return max(settings_.topBottomLevel(), settings_.leftRightLevel()); }
    const char* pageMarginLabel() const;
    void cycleLineSpacing();
    void setLineSpacingLevel(uint8_t level);
    uint8_t lineSpacingLevel() const { return settings_.lineSpacingLevel(); }
    const char* lineSpacingLabel() const;
    void cycleLayoutPreset();
    void setLayoutPreset(uint8_t preset);
    uint8_t layoutPreset() const { return layoutPreset_; }
    void setWebLayout(uint8_t fontSize, uint8_t lineSpacing, uint8_t paragraphSpacing,
                      uint8_t indentFirstLine, uint8_t marginLeft, uint8_t marginRight,
                      uint8_t marginTop, uint8_t marginBottom, bool justify);
    uint8_t webLineSpacing() const { return webLineSpacing_; }
    uint8_t webParagraphSpacing() const { return webParagraphSpacing_; }
    uint8_t webIndentFirstLine() const { return webIndentFirstLine_; }
    uint8_t webMarginLeft() const { return webMarginLeft_; }
    uint8_t webMarginRight() const { return webMarginRight_; }
    uint8_t webMarginTop() const { return webMarginTop_; }
    uint8_t webMarginBottom() const { return webMarginBottom_; }
    bool webJustify() const { return webJustify_; }
    bool saveLocalSettings() const;
    const ReaderSettings& settings() const { return settings_; }
    const char* layoutPresetLabel() const;
    ReaderRenderOptions currentOptions() const;

    void renderPlaceholderPage();
    void renderTextPage(const char* title, const char* body, uint16_t page, uint16_t totalPages, const ReaderRenderOptions& options = ReaderRenderOptions{}, uint16_t progressPermille = 0);
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
    void applyReaderFontSize(uint8_t size, bool persist);
    bool findReadPaperGlyph(uint32_t unicode, ReadPaperGlyph& out) const;
    uint8_t charAdvance(uint32_t unicode) const;
    int16_t textWidth(const char* text) const;
    void drawGlyph(uint32_t unicode, int16_t x, int16_t y, uint16_t color);
    void drawReadPaperGlyph(const ReadPaperGlyph& glyph, int16_t x, int16_t y, uint16_t color);
    uint16_t pixelColorForNibble(uint8_t nibble, uint16_t color) const;
    void drawText(int16_t x, int16_t y, const char* text, uint16_t color = TFT_BLACK, int16_t letterGap = 0);
    void drawJustifiedText(int16_t x, int16_t y, const char* text, int16_t targetWidth, uint16_t color, int16_t letterGap = 0);
    void drawReadingChrome(const char* title, uint16_t progressPermille, const ReaderRenderOptions& options, uint16_t color);
    void formatReaderTime(char* out, size_t outSize) const;
    uint16_t utf8CharCount(const char* text) const;
    void drawShellTabs(int activeTab, const ReaderRenderOptions& options);
    size_t nextLineEnd(const char* text, size_t len, size_t start, int16_t maxWidth, int16_t initialWidth, const ReaderRenderOptions& options, bool& hardBreak) const;
    size_t findWrapBreak(const char* text, size_t start, int16_t maxWidth, int16_t letterGap = 0) const;
    size_t skipLeadingSourceIndent(const char* text, size_t pos, size_t len) const;
    bool isParagraphStart(const char* text, size_t pos, bool chunkStartsAtParagraph) const;
    bool isForbiddenLineStart(uint32_t unicode) const;

    M5Canvas* canvas_ = nullptr;
    bool readPaperFullReady_ = false;
    uint32_t readPaperCharCount_ = 0;
    uint8_t readPaperFontHeight_ = 0;
    FontManager font_;
    ReaderSettings settings_;
    uint8_t layoutPreset_ = 1;
    uint8_t fontSizeSetting_ = 24;
    uint8_t webLineSpacing_ = 50;
    uint8_t webParagraphSpacing_ = 50;
    uint8_t webIndentFirstLine_ = 2;
    uint8_t webMarginLeft_ = 24;
    uint8_t webMarginRight_ = 24;
    uint8_t webMarginTop_ = 68;
    uint8_t webMarginBottom_ = 48;
    bool webJustify_ = false;
    void applyLayoutPresetToSettings();
    void loadLocalSettings();
};

extern ReaderTextRenderer g_readerText;

} // namespace vink3
