#pragma once
#include <Arduino.h>
#include <M5Unified.h>
#include <SD.h>

namespace vink3 {

/// TTF/OTF font loaded from SD card, rendered via stb_truetype.
class TtfFont {
public:
    TtfFont();
    ~TtfFont();

    /// Load a TTF/OTF file from SD card into PSRAM.
    bool loadFromSd(const char* path);

    /// Unload the font and free all memory.
    void unload();

    /// Whether a TTF font is loaded.
    bool isLoaded() const { return loaded_; }

    /// Full SD path of the loaded font.
    const char* currentPath() const { return fontPath_; }

    /// Current pixel height.
    uint8_t currentSize() const { return pxSize_; }

    /// Set pixel height (clamped to 16-64).
    void setSize(uint8_t pxSize);

    /// Advance width for a single unicode codepoint.
    int16_t charAdvance(uint32_t unicode) const;

    /// Total pixel width of a UTF-8 string (stops at newline).
    int16_t textWidth(const char* text) const;

    /// Draw a single glyph onto a canvas.
    void drawGlyph(uint32_t unicode, int16_t x, int16_t y,
                   uint16_t color, M5Canvas* canvas);

    /// Render glyph to a caller-provided buffer.
    /// @return true if the glyph was found and rendered.
    bool drawGlyphToBuffer(uint32_t unicode, uint8_t* buf,
                           int* w, int* h, int* advance);

    /// Scan /fonts on SD for .ttf / .otf files.
    /// @param paths  output array of full paths
    /// @param maxCount  max entries to fill
    /// @return number of fonts found
    static int scanSdFonts(char paths[][64], int maxCount);

    // ── metrics ──
    int16_t ascender() const { return ascender_; }
    int16_t descender() const { return descender_; }
    int16_t lineHeight() const { return lineHeight_; }
    float scale() const { return scale_; }

private:
    /// Recompute scale + VMetrics after a size change.
    void recalcMetrics();

    /// Look up a glyph in the LRU cache. Returns index or -1.
    int findCachedGlyph(uint32_t unicode) const;

    /// Insert a glyph into the LRU cache, evicting oldest if full.
    int insertCacheSlot(uint32_t unicode, uint8_t w, uint8_t h,
                        int8_t bx, int8_t by, uint8_t adv);

    bool loaded_ = false;
    uint8_t* ttfData_ = nullptr;
    size_t ttfDataSize_ = 0;
    char fontPath_[128] = {0};
    uint8_t pxSize_ = 32;
    int fontOffset_ = 0;  // 0 for TTF, stbtt_GetFontOffsetForIndex for OTF/TTC
    float scale_ = 1.0f;
    int16_t ascender_ = 0;
    int16_t descender_ = 0;
    int16_t lineHeight_ = 0;

    /// LRU glyph cache
    static constexpr int kCacheSlots = 16;
    struct CacheSlot {
        uint32_t unicode = 0;
        uint8_t width = 0;
        uint8_t height = 0;
        int8_t bearingX = 0;
        int8_t bearingY = 0;
        uint8_t advance = 0;
        uint8_t useOrder = 0;  // higher = more recent
        uint8_t* bitmap = nullptr;  // 8bpp grayscale
    };
    CacheSlot cache_[kCacheSlots] = {};
    uint8_t cacheAge_ = 0;
};

} // namespace vink3
