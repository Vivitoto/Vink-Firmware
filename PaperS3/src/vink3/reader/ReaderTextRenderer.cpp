#include "ReaderTextRenderer.h"
#include "../../Config.h"
#include "../ReadPaper176.h"
#include "../text/ReadPaperFullFont.h"

namespace {
constexpr uint32_t kReadPaperHeaderSize = 134;
constexpr uint32_t kReadPaperEntrySize = 20;
}

namespace vink3 {

ReaderTextRenderer g_readerText;

bool ReaderTextRenderer::begin(M5Canvas* canvas) {
    canvas_ = canvas;
    return canvas_ && loadDefaultFont();
}

bool ReaderTextRenderer::loadDefaultFont() {
    // Reader body font is intentionally separate from the UI subset font.
    // v0.3 uses ReadPaper's complete PROGMEM Book font, enabled by the larger
    // single-app partition. Bundled fonts remain only as emergency fallback.
    if (beginReadPaperFullFont()) {
        Serial.printf("[vink3][reader] ReadPaper full PROGMEM font loaded: glyphs=%lu size=%lu\n",
                      static_cast<unsigned long>(readPaperCharCount_),
                      static_cast<unsigned long>(g_readpaper_full_font_size));
        return true;
    }
    if (font_.loadBundledFont(FONT_FILE_24)) {
        Serial.println("[vink3][reader] bundled 24px reader font loaded");
        return true;
    }
    if (font_.loadBundledFont(FONT_FILE_20)) {
        Serial.println("[vink3][reader] bundled 20px reader font loaded");
        return true;
    }
    if (font_.loadBundledFont(FONT_FILE_16)) {
        Serial.println("[vink3][reader] bundled 16px reader font loaded");
        return true;
    }
    Serial.println("[vink3][reader] reader font load failed");
    return false;
}

bool ReaderTextRenderer::loadFont(const char* path) {
    if (!path || !path[0]) return loadDefaultFont();
    return font_.loadFont(path);
}

bool ReaderTextRenderer::ready() const {
    return canvas_ && (readPaperFullReady_ || font_.isLoaded());
}

uint16_t ReaderTextRenderer::fontSize() const {
    if (optionsFontSize_ > 0) return optionsFontSize_;
    if (readPaperFullReady_) return readPaperFontHeight_;
    return font_.isLoaded() ? font_.getFontSize() : 24;
}

uint32_t ReaderTextRenderer::decodeUtf8(const uint8_t* buf, size_t& pos, size_t len) {
    if (pos >= len) return 0;
    uint8_t c = buf[pos];
    if ((c & 0x80) == 0) { pos++; return c; }
    if ((c & 0xE0) == 0xC0 && pos + 1 < len) {
        uint32_t ch = ((c & 0x1F) << 6) | (buf[pos + 1] & 0x3F);
        pos += 2; return ch;
    }
    if ((c & 0xF0) == 0xE0 && pos + 2 < len) {
        uint32_t ch = ((c & 0x0F) << 12) | ((buf[pos + 1] & 0x3F) << 6) | (buf[pos + 2] & 0x3F);
        pos += 3; return ch;
    }
    if ((c & 0xF8) == 0xF0 && pos + 3 < len) {
        uint32_t ch = ((c & 0x07) << 18) | ((buf[pos + 1] & 0x3F) << 12) | ((buf[pos + 2] & 0x3F) << 6) | (buf[pos + 3] & 0x3F);
        pos += 4; return ch;
    }
    pos++;
    return c;
}

bool ReaderTextRenderer::beginReadPaperFullFont() {
    if (!g_readpaper_full_font_available || g_readpaper_full_font_size < kReadPaperHeaderSize) return false;
    readPaperCharCount_ = readpaperFullU32(0);
    readPaperFontHeight_ = readpaperFullByte(4);
    const uint8_t version = readpaperFullByte(5);
    if (readPaperCharCount_ == 0 || readPaperFontHeight_ == 0 || version != 3) return false;
    const uint32_t entriesEnd = kReadPaperHeaderSize + readPaperCharCount_ * kReadPaperEntrySize;
    if (entriesEnd >= g_readpaper_full_font_size) return false;
    readPaperFullReady_ = true;
    return true;
}

bool ReaderTextRenderer::findReadPaperGlyph(uint32_t unicode, ReadPaperGlyph& out) const {
    if (!readPaperFullReady_ || unicode > 0xFFFF) return false;
    int32_t lo = 0;
    int32_t hi = static_cast<int32_t>(readPaperCharCount_) - 1;
    while (lo <= hi) {
        int32_t mid = lo + (hi - lo) / 2;
        uint32_t off = kReadPaperHeaderSize + static_cast<uint32_t>(mid) * kReadPaperEntrySize;
        uint16_t cp = readpaperFullU16(off);
        if (cp == unicode) {
            out.unicode = cp;
            out.width = readpaperFullU16(off + 2);
            out.bitmapW = readpaperFullByte(off + 4);
            out.bitmapH = readpaperFullByte(off + 5);
            out.xOffset = readpaperFullI8(off + 6);
            out.yOffset = readpaperFullI8(off + 7);
            out.bitmapOffset = readpaperFullU32(off + 8);
            out.bitmapSize = readpaperFullU32(off + 12);
            return out.bitmapOffset + out.bitmapSize <= g_readpaper_full_font_size;
        }
        if (cp < unicode) lo = mid + 1;
        else hi = mid - 1;
    }
    return false;
}

uint8_t ReaderTextRenderer::charAdvance(uint32_t unicode) const {
    if (readPaperFullReady_) {
        ReadPaperGlyph glyph;
        if (findReadPaperGlyph(unicode, glyph) && glyph.width > 0) return glyph.width;
        return unicode < 128 ? 8 : fontSize();
    }
    if (!font_.isLoaded()) return unicode < 128 ? 8 : 24;
    uint8_t adv = const_cast<FontManager&>(font_).getCharAdvance(unicode);
    return adv > 0 ? adv : (unicode < 128 ? 8 : fontSize());
}

int16_t ReaderTextRenderer::textWidth(const char* text) const {
    if (!text) return 0;
    int16_t w = 0;
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(text);
    size_t pos = 0;
    const size_t len = strlen(text);
    while (pos < len) {
        uint32_t ch = decodeUtf8(bytes, pos, len);
        if (ch == '\n') break;
        w += charAdvance(ch);
    }
    return w;
}

uint16_t ReaderTextRenderer::pixelColorForNibble(uint8_t nibble, uint16_t color) const {
    if (color == TFT_WHITE) return TFT_WHITE;
    if (color != TFT_BLACK) return color;
    // A+B+D: linear kRemap; all ReadPaper AA pixels go through unified path
    static const uint8_t kRemap[16] __attribute__((aligned(1))) = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
    };
    static const uint16_t k4BitToRgb565[16] __attribute__((aligned(2))) = {
        0xFFFF, 0xEFFF, 0xCFFF, 0xADAD, 0x8A8A, 0x7B7B,
        0x6B6B, 0x5B5B, 0x4B4B, 0x39A5, 0x294A, 0x2108,
        0x1800, 0x1000, 0x0841, 0x0000
    };
    return k4BitToRgb565[kRemap[nibble & 0x0F]];
}

void ReaderTextRenderer::drawReadPaperGlyph(const ReadPaperGlyph& glyph, int16_t x, int16_t y, uint16_t color) {
    if (!canvas_) return;
    const int16_t drawX = x + glyph.xOffset;
    // ReadPaper V3 glyph entries already store a visual yOffset inside the
    // 32px line box. Do not center each bitmap by height: that puts '.', 'o',
    // 'v' and punctuation in the middle of /books/.txt prompts. Use yOffset so
    // lowercase, punctuation, and descenders keep the original ReadPaper metrics.
    const int16_t drawY = y + glyph.yOffset;
    uint32_t pixelIdx = 0;
    uint8_t bitPos = 0;
    uint32_t bytePos = 0;
    const uint32_t totalPixels = static_cast<uint32_t>(glyph.bitmapW) * glyph.bitmapH;

    auto nextBit = [&]() -> int {
        if (bytePos >= glyph.bitmapSize) return -1;
        uint8_t current = readpaperFullByte(glyph.bitmapOffset + bytePos);
        int bit = (current >> (7 - bitPos)) & 0x01;
        bitPos++;
        if (bitPos >= 8) {
            bitPos = 0;
            bytePos++;
        }
        return bit;
    };

    while (pixelIdx < totalPixels && bytePos < glyph.bitmapSize) {
        int first = nextBit();
        if (first < 0) break;
        uint8_t pixel = 0;
        if (first == 0) {
            pixel = 0;
        } else {
            int second = nextBit();
            if (second < 0) break;
            pixel = second == 0 ? 10 : 11;
        }

        if (pixel != 0) {
            const int16_t px = drawX + static_cast<int16_t>(pixelIdx % glyph.bitmapW);
            const int16_t py = drawY + static_cast<int16_t>(pixelIdx / glyph.bitmapW);
            if (px >= 0 && px < kPaperS3Width && py >= 0 && py < kPaperS3Height) {
                canvas_->drawPixel(px, py, pixelColorForNibble(pixel, color));
            }
        }
        pixelIdx++;
    }
}

void ReaderTextRenderer::drawGlyph(uint32_t unicode, int16_t x, int16_t y, uint16_t color) {
    if (!canvas_) return;
    if (readPaperFullReady_) {
        ReadPaperGlyph glyph;
        if (findReadPaperGlyph(unicode, glyph)) {
            drawReadPaperGlyph(glyph, x, y, color);
            return;
        }
    }
    if (!font_.isLoaded()) return;
    if (font_.getFontType() == FontType::GRAY_4BPP) {
        uint8_t width = 0, height = 0, advance = 0;
        int8_t bearingX = 0, bearingY = 0;
        const uint8_t* bmp = font_.getCharBitmapGray(unicode, width, height, bearingX, bearingY, advance);
        if (!bmp || width == 0 || height == 0) return;
        const int16_t drawX = x + bearingX;
        // Reader management/list pages pass y as a visual top coordinate. Do
        // not baseline-align gray fallback glyphs with per-character bearingY;
        // it makes Latin words look staggered on the e-paper screen.
        const int16_t drawY = y + max<int16_t>(0, (static_cast<int16_t>(font_.getFontSize()) - static_cast<int16_t>(height)) / 2);
        for (int row = 0; row < height; row++) {
            const int16_t py = drawY + row;
            if (py < 0 || py >= kPaperS3Height) continue;
            for (int col = 0; col < width; col++) {
                const int16_t px = drawX + col;
                if (px < 0 || px >= kPaperS3Width) continue;
                const int srcIdx = row * ((width + 1) / 2) + col / 2;
                const uint8_t nibble = (col % 2 == 0) ? ((bmp[srcIdx] >> 4) & 0x0F) : (bmp[srcIdx] & 0x0F);
                if (nibble > 0) canvas_->drawPixel(px, py, pixelColorForNibble(nibble, color));
            }
        }
        return;
    }

    uint8_t width = 0, height = 0;
    const uint8_t* bmp = font_.getCharBitmap(unicode, width, height);
    if (!bmp || width == 0 || height == 0) return;
    for (int row = 0; row < height; row++) {
        const int16_t py = y + row;
        if (py < 0 || py >= kPaperS3Height) continue;
        for (int col = 0; col < width; col++) {
            const int16_t px = x + col;
            if (px < 0 || px >= kPaperS3Width) continue;
            int byteIdx = row * ((width + 7) / 8) + col / 8;
            int bitIdx = 7 - (col % 8);
            if (bmp[byteIdx] & (1 << bitIdx)) canvas_->drawPixel(px, py, color);
        }
    }
}

void ReaderTextRenderer::drawText(int16_t x, int16_t y, const char* text, uint16_t color) {
    if (!canvas_ || !text) return;
    if (!readPaperFullReady_ && !font_.isLoaded()) return;
    int16_t cx = x;
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(text);
    size_t pos = 0;
    const size_t len = strlen(text);
    while (pos < len && cx < kPaperS3Width) {
        uint32_t ch = decodeUtf8(bytes, pos, len);
        if (ch == '\n') break;
        drawGlyph(ch, cx, y, color);
        cx += charAdvance(ch);
    }
}

size_t ReaderTextRenderer::findWrapBreak(const char* text, size_t start, int16_t maxWidth) const {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(text);
    const size_t len = strlen(text);
    size_t pos = start;
    size_t lastGood = start;
    int16_t width = 0;
    while (pos < len) {
        size_t before = pos;
        uint32_t ch = decodeUtf8(bytes, pos, len);
        if (ch == '\n') return before;
        uint8_t adv = charAdvance(ch);
        if (width + adv > maxWidth) return lastGood > start ? lastGood : before;
        width += adv;
        lastGood = pos;
    }
    return len;
}

size_t ReaderTextRenderer::measurePageBytes(const char* text, size_t len, const ReaderRenderOptions& options) const {
    if (!text || len == 0) return 0;
    size_t pos = 0;
    int16_t y = options.marginTop;
    const int16_t maxWidth = kPaperS3Width - options.marginLeft - options.marginRight;
    const int16_t lineHeight = fontSize() + options.lineGap;
    const int16_t paragraphExtra = (lineHeight * options.paragraphSpacing) / 100;
    const int16_t indentPx = static_cast<int16_t>(fontSize()) * options.indentFirstLine;
    const int16_t bottom = kPaperS3Height - options.marginBottom;
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(text);
    bool paragraphStart = true;

    while (pos < len && y + lineHeight < bottom) {
        int newlineCount = 0;
        while (pos < len && (text[pos] == '\n' || text[pos] == '\r')) { pos++; newlineCount++; }
        if (newlineCount > 0) {
            if (!paragraphStart) y += paragraphExtra;
            paragraphStart = true;
        }
        if (pos >= len || y + lineHeight >= bottom) break;

        const size_t lineStart = pos;
        size_t lastGood = pos;
        int16_t width = 0;
        const int16_t lineMaxWidth = max<int16_t>(fontSize(), maxWidth - (paragraphStart ? indentPx : 0));
        bool endedByNewline = false;
        while (pos < len) {
            const size_t before = pos;
            uint32_t ch = decodeUtf8(bytes, pos, len);
            if (ch == '\n' || ch == '\r') {
                pos = before;
                endedByNewline = true;
                break;
            }
            const uint8_t adv = charAdvance(ch);
            if (width + adv > lineMaxWidth) {
                pos = lastGood > lineStart ? lastGood : pos;
                break;
            }
            width += adv;
            lastGood = pos;
        }
        if (pos <= lineStart) {
            size_t force = lineStart;
            decodeUtf8(bytes, force, len);
            pos = force > lineStart ? force : lineStart + 1;
        }
        y += lineHeight;
        if (endedByNewline) y += paragraphExtra;
        paragraphStart = endedByNewline;
    }
    return pos;
}

void ReaderTextRenderer::drawShellTabs(int activeTab, const ReaderRenderOptions& options) {
    static constexpr const char* kLabels[] = {"阅读", "书架", "同步", "设置"};
    static constexpr int16_t kTabX0 = 18;
    static constexpr int16_t kTabsY = 76;
    static constexpr int16_t kTabW = 120;
    static constexpr int16_t kTabsH = 64;
    static constexpr int16_t kTabGap = 8;
    const uint16_t fg = options.dark ? TFT_WHITE : TFT_BLACK;
    const uint16_t bg = options.dark ? TFT_BLACK : TFT_WHITE;
    for (int i = 0; i < 4; ++i) {
        const int16_t x = kTabX0 + i * (kTabW + kTabGap);
        const bool selected = i == activeTab;
        // Match shell tabs: do not fill selected tab black. On PaperS3 photos
        // white CJK on black can look swallowed; outline + underline is safer.
        canvas_->fillRoundRect(x, kTabsY, kTabW, kTabsH, 16, bg);
        canvas_->drawRoundRect(x, kTabsY, kTabW, kTabsH, 16, fg);
        if (selected) {
            canvas_->drawRoundRect(x + 2, kTabsY + 2, kTabW - 4, kTabsH - 4, 14, fg);
            canvas_->fillRoundRect(x + 28, kTabsY + kTabsH - 9, kTabW - 56, 4, 2, fg);
        }
        const char* label = kLabels[i];
        const int16_t tx = x + (kTabW - textWidth(label)) / 2;
        const int16_t ty = kTabsY + (kTabsH - fontSize()) / 2;
        drawText(tx, ty, label, fg);
    }
}

void ReaderTextRenderer::renderTextPage(const char* title, const char* body, uint16_t page, uint16_t totalPages, const ReaderRenderOptions& options) {
    if (!canvas_) return;
    if (!ready()) loadDefaultFont();
    setOptionsFontSize(options.fontSize);
    canvas_->fillSprite(options.dark ? TFT_BLACK : TFT_WHITE);
    const uint16_t fg = options.dark ? TFT_WHITE : TFT_BLACK;
    const uint16_t mid = options.dark ? 0xC618 : 0x8410;

    drawText(options.marginLeft, 28, title ? title : "未命名书籍", mid);
    canvas_->drawFastHLine(options.marginLeft, 64, kPaperS3Width - options.marginLeft - options.marginRight, mid);

    const char* text = body ? body : "";
    size_t pos = 0;
    const size_t len = strlen(text);
    int16_t y = options.marginTop;
    const int16_t maxWidth = kPaperS3Width - options.marginLeft - options.marginRight;
    const int16_t lineHeight = fontSize() + options.lineGap;
    const int16_t paragraphExtra = (lineHeight * options.paragraphSpacing) / 100;
    const int16_t indentPx = static_cast<int16_t>(fontSize()) * options.indentFirstLine;
    const int16_t bottom = kPaperS3Height - options.marginBottom;
    bool paragraphStart = true;
    while (pos < len && y + lineHeight < bottom) {
        int newlineCount = 0;
        while (pos < len && (text[pos] == '\n' || text[pos] == '\r')) { pos++; newlineCount++; }
        if (newlineCount > 0) {
            if (!paragraphStart) y += paragraphExtra;
            paragraphStart = true;
        }
        if (pos >= len || y + lineHeight >= bottom) break;
        const int16_t lineIndent = paragraphStart ? indentPx : 0;
        size_t end = findWrapBreak(text, pos, max<int16_t>(fontSize(), maxWidth - lineIndent));
        if (end <= pos) break;
        char line[256];
        size_t n = end - pos;
        if (n >= sizeof(line)) n = sizeof(line) - 1;
        memcpy(line, text + pos, n);
        line[n] = '\0';
        drawText(options.marginLeft + lineIndent, y, line, fg);
        const bool endedByNewline = (end < len && (text[end] == '\n' || text[end] == '\r'));
        pos = end;
        y += lineHeight;
        if (endedByNewline) y += paragraphExtra;
        paragraphStart = endedByNewline;
    }

    char footer[64];
    const uint16_t safeTotal = totalPages > 0 ? totalPages : 1;
    const uint16_t safePage = page > 0 ? page : 1;
    const uint16_t percent = min<uint16_t>(100, (static_cast<uint32_t>(safePage) * 100U) / safeTotal);
    snprintf(footer, sizeof(footer), "%u / %u · %u%%", static_cast<unsigned>(safePage), static_cast<unsigned>(safeTotal), static_cast<unsigned>(percent));
    drawText(kPaperS3Width - options.marginRight - textWidth(footer), kPaperS3Height - 34, footer, mid);

    // Thin visual progress rail: easier to read at a glance on e-paper than
    // footer numbers alone, and it costs only a few static draw operations.
    const int16_t railX = options.marginLeft;
    const int16_t railY = kPaperS3Height - 12;
    const int16_t railW = max<int16_t>(24, kPaperS3Width - options.marginLeft - options.marginRight);
    canvas_->drawFastHLine(railX, railY, railW, mid);
    const int16_t fillW = static_cast<int16_t>((static_cast<uint32_t>(railW) * percent) / 100U);
    if (fillW > 0) canvas_->fillRect(railX, railY - 1, fillW, 3, fg);
}

void ReaderTextRenderer::renderListPage(const char* title, const char* summary, const char* const* rows, int rowCount, int16_t rowY, int16_t rowH, uint16_t page, uint16_t totalPages, int activeTab, const ReaderRenderOptions& options) {
    if (!canvas_) return;
    if (!ready()) loadDefaultFont();
    setOptionsFontSize(options.fontSize);
    canvas_->fillSprite(options.dark ? TFT_BLACK : TFT_WHITE);
    const uint16_t fg = options.dark ? TFT_WHITE : TFT_BLACK;
    const uint16_t mid = options.dark ? 0xC618 : 0x8410;

    drawText(options.marginLeft, 22, title ? title : "列表", fg);
    drawText(kPaperS3Width - options.marginRight - textWidth(kVinkPaperS3FirmwareVersion), 22, kVinkPaperS3FirmwareVersion, mid);
    canvas_->drawFastHLine(options.marginLeft, 61, kPaperS3Width - options.marginLeft - options.marginRight, fg);
    drawShellTabs(activeTab, options);
    // Summary/help text is secondary; keep it smaller than primary row labels.
    if (summary && summary[0]) drawText(options.marginLeft, 162, summary, mid);

    const int16_t maxWidth = kPaperS3Width - options.marginLeft - options.marginRight;
    for (int i = 0; rows && i < rowCount; ++i) {
        const int16_t y = rowY + i * rowH;
        if (y + rowH > kPaperS3Height - options.marginBottom) break;
        canvas_->drawFastHLine(options.marginLeft, y + rowH - 4, maxWidth, mid);
        const char* row = rows[i] ? rows[i] : "";
        char line[192];
        size_t end = findWrapBreak(row, 0, maxWidth);
        if (end == 0) end = min(strlen(row), sizeof(line) - 1);
        size_t n = min(end, sizeof(line) - 1);
        memcpy(line, row, n);
        line[n] = '\0';
        const int16_t ty = y + max<int16_t>(0, (rowH - static_cast<int16_t>(fontSize())) / 2);
        drawText(options.marginLeft, ty, line, fg);
    }

    char footer[48];
    snprintf(footer, sizeof(footer), "%u / %u", static_cast<unsigned>(page), static_cast<unsigned>(totalPages));
    drawText(kPaperS3Width - options.marginRight - textWidth(footer), kPaperS3Height - 34, footer, mid);
}

void ReaderTextRenderer::renderActionPage(const char* title, const char* const* infoLines, int infoCount, const char* const* actions, int actionCount, int activeTab, const ReaderRenderOptions& options) {
    if (!canvas_) return;
    if (!ready()) loadDefaultFont();
    canvas_->fillSprite(options.dark ? TFT_BLACK : TFT_WHITE);
    const uint16_t fg = options.dark ? TFT_WHITE : TFT_BLACK;
    const uint16_t bg = options.dark ? TFT_BLACK : TFT_WHITE;
    const uint16_t mid = options.dark ? 0xC618 : 0x8410;

    drawText(options.marginLeft, 22, title ? title : "操作", fg);
    drawText(kPaperS3Width - options.marginRight - textWidth(kVinkPaperS3FirmwareVersion), 22, kVinkPaperS3FirmwareVersion, mid);
    canvas_->drawFastHLine(options.marginLeft, 61, kPaperS3Width - options.marginLeft - options.marginRight, fg);
    drawShellTabs(activeTab, options);

    const int16_t maxWidth = kPaperS3Width - options.marginLeft - options.marginRight;
    const int16_t lineHeight = fontSize() + options.lineGap;
    int16_t y = 160;
    for (int i = 0; infoLines && i < infoCount && y < 510; ++i) {
        const char* lineText = infoLines[i] ? infoLines[i] : "";
        size_t start = 0;
        int wraps = 0;
        while (lineText[start] && wraps < 2 && y < 510) {
            size_t end = findWrapBreak(lineText, start, maxWidth);
            if (end <= start) break;
            char line[256];
            size_t n = end - start;
            if (n >= sizeof(line)) n = sizeof(line) - 1;
            memcpy(line, lineText + start, n);
            line[n] = '\0';
            drawText(options.marginLeft, y, line, mid);
            y += lineHeight;
            start = end;
            wraps++;
        }
    }

    static constexpr int16_t kButtonX = 70;
    static constexpr int16_t kButtonW = 400;
    static constexpr int16_t kButtonH = 64;
    static constexpr int16_t kButtonY[] = {560, 660, 760};
    const int drawCount = min(actionCount, static_cast<int>(sizeof(kButtonY) / sizeof(kButtonY[0])));
    for (int i = 0; actions && i < drawCount; ++i) {
        const int16_t by = kButtonY[i];
        const bool primary = i == 0;
        canvas_->fillRoundRect(kButtonX, by, kButtonW, kButtonH, 18, primary ? fg : bg);
        canvas_->drawRoundRect(kButtonX, by, kButtonW, kButtonH, 18, fg);
        const char* label = actions[i] ? actions[i] : "";
        const int16_t tx = kButtonX + (kButtonW - textWidth(label)) / 2;
        // Button labels are primary actions: keep 24/32px text and center it
        // in the button's actual geometry, separate from smaller notes below.
        const int16_t lineH = static_cast<int16_t>(fontSize()) + 6;
        const int16_t ty = by + (kButtonH - lineH) / 2;
        drawText(tx, ty, label, primary ? bg : fg);
    }
}

void ReaderTextRenderer::renderPlaceholderPage() {
    static const char* sample =
        "这是 Vink v0.3 的正文渲染层。界面文字使用 ReadPaper UI 子集字体，正文阅读使用完整 ReadPaper Book PROGMEM 字体。\n"
        "下一步会把本地 TXT、分页、书签和 Legado 进度映射接到这里；中文覆盖不再依赖按书抽子集。";
    renderTextPage("示例正文", sample, 1, 1);
}

} // namespace vink3
