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
    applyLayoutPresetToSettings();
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
    if (readPaperFullReady_) return readPaperFontHeight_;
    return font_.isLoaded() ? font_.getFontSize() : 24;
}

void ReaderTextRenderer::applyLayoutPresetToSettings() {
    settings_ = ReaderSettings{};
    auto on = [](uint16_t& raw, uint8_t idx) { ReaderSettings::setSlot(raw, idx, 1); };
    auto level = [](uint16_t& raw, uint8_t idx, uint8_t value) { ReaderSettings::setSlot(raw, idx, value); };

    switch (layoutPreset_) {
        case 0:
            // 原始：Vink packed model with all formatting optimizations off.
            settings_.showMode = ReaderShowMode::Std;
            settings_.flashPeriod = ReaderFlashPeriod::Pages10;
            break;
        case 2:
            // 紧凑：same Vink options, tighter spacing slots.
            on(settings_.formatting1, 0); // Indent
            on(settings_.formatting1, 1); // BlankLineOpt
            on(settings_.formatting1, 2); // BreakLineOpt
            on(settings_.formatting1, 3); // NewPage
            on(settings_.formatting1, 4); // DynamicLineHeight
            on(settings_.renderOpt1, 1);  // AntiAlias
            level(settings_.spacing, 0, 0);
            level(settings_.spacing, 1, 0);
            level(settings_.spacing, 2, 0);
            level(settings_.spacing, 3, 1);
            level(settings_.spacing, 4, 0);
            level(settings_.spacing, 5, 1);
            settings_.showMode = ReaderShowMode::Fast;
            settings_.flashPeriod = ReaderFlashPeriod::Pages20;
            break;
        case 1:
        default:
            // 优化：Vink-native default behavior built around the formal
            // formatting1/render_opt1/spacing option groups.
            on(settings_.formatting1, 0); // Indent
            on(settings_.formatting1, 1); // BlankLineOpt
            on(settings_.formatting1, 2); // BreakLineOpt
            on(settings_.formatting1, 3); // NewPage
            on(settings_.formatting1, 4); // DynamicLineHeight
            on(settings_.renderOpt1, 1);  // AntiAlias
            level(settings_.spacing, 0, 1);
            level(settings_.spacing, 1, 1);
            level(settings_.spacing, 2, 1);
            level(settings_.spacing, 3, 1);
            level(settings_.spacing, 4, 1);
            level(settings_.spacing, 5, 1);
            settings_.showMode = ReaderShowMode::Std;
            settings_.flashPeriod = ReaderFlashPeriod::Pages10;
            break;
    }
}

void ReaderTextRenderer::toggleAntiAlias() {
    if (settings_.schema == 0) applyLayoutPresetToSettings();
    ReaderSettings::setSlot(settings_.renderOpt1, 1, antiAliasEnabled() ? 0 : 1);
    Serial.printf("[vink3][reader] anti-alias -> %s\n", antiAliasLabel());
}

void ReaderTextRenderer::toggleUnderline() {
    if (settings_.schema == 0) applyLayoutPresetToSettings();
    ReaderSettings::setSlot(settings_.renderOpt1, 0, underlineEnabled() ? 0 : 1);
    Serial.printf("[vink3][reader] Vink underline -> %s render_opt1=0x%04x\n", underlineLabel(), settings_.renderOpt1);
}

void ReaderTextRenderer::togglePageTurnEffect() {
    if (settings_.schema == 0) applyLayoutPresetToSettings();
    ReaderSettings::setSlot(settings_.renderOpt1, 3, pageTurnEffectEnabled() ? 0 : 1);
    Serial.printf("[vink3][reader] Vink page-turn effect -> %s render_opt1=0x%04x\n", pageTurnEffectLabel(), settings_.renderOpt1);
}

void ReaderTextRenderer::cycleLayoutPreset() {
    layoutPreset_ = (layoutPreset_ + 1) % 3;
    applyLayoutPresetToSettings();
    Serial.printf("[vink3][reader] Vink layout preset -> %s formatting1=0x%04x render_opt1=0x%04x spacing=0x%04x\n",
                  layoutPresetLabel(), settings_.formatting1, settings_.renderOpt1, settings_.spacing);
}

const char* ReaderTextRenderer::layoutPresetLabel() const {
    switch (layoutPreset_) {
        case 0: return "原始";
        case 1: return "优化";
        case 2: return "紧凑";
    }
    return "优化";
}

ReaderRenderOptions ReaderTextRenderer::currentOptions() const {
    if (settings_.schema == 0) const_cast<ReaderTextRenderer*>(this)->applyLayoutPresetToSettings();

    ReaderRenderOptions opt;
    opt.fontSize = fontSize();

    static constexpr int16_t kTopMargins[4] = {72, 74, 82, 90};
    static constexpr int16_t kBottomMargins[4] = {26, 30, 36, 44};
    static constexpr int16_t kSideMargins[4] = {24, 28, 34, 40};
    static constexpr int16_t kLineGaps[4] = {5, 7, 10, 13};
    // Vink keeps four letter-spacing levels. Clamp the tightest level to zero
    // for the current renderer
    // so glyphs do not overlap on the PaperS3 panel.
    static constexpr int16_t kLetterGaps[4] = {0, 0, 1, 2};
    static constexpr int16_t kParagraphGaps[4] = {0, 3, 6, 10};
    static constexpr int16_t kUnderlineOffsets[4] = {1, 2, 4, 6};

    opt.marginTop = kTopMargins[settings_.topBottomLevel()];
    opt.marginBottom = kBottomMargins[settings_.topBottomLevel()];
    opt.marginLeft = kSideMargins[settings_.leftRightLevel()];
    opt.marginRight = kSideMargins[settings_.leftRightLevel()];
    opt.lineGap = kLineGaps[settings_.lineSpacingLevel()];
    opt.letterGap = kLetterGaps[settings_.letterSpacingLevel()];
    opt.paragraphGap = kParagraphGaps[settings_.paragraphSpacingLevel()];
    opt.underlineOffset = kUnderlineOffsets[settings_.underlineOffsetLevel()];
    opt.indentFirstLine = settings_.indentEnabled();
    opt.compactBlankLines = settings_.blankLineOptEnabled();
    opt.dynamicLineHeight = settings_.dynamicLineHeightEnabled();
    opt.breakLineOpt = settings_.breakLineOptEnabled();
    opt.underline = settings_.underlineEnabled();
    opt.firstLineIndentPx = opt.indentFirstLine ? max<int16_t>(fontSize() * 2, 56) : 0;

    // Original comparison mode intentionally disables the Vink formatting
    // transforms but still keeps Vink's safe text box.
    if (layoutPreset_ == 0) {
        opt.marginLeft = 32;
        opt.marginTop = 82;
        opt.marginRight = 30;
        opt.marginBottom = 40;
        opt.lineGap = 10;
        opt.letterGap = 0;
        opt.paragraphGap = 0;
        opt.underlineOffset = 2;
        opt.firstLineIndentPx = 0;
    }
    return opt;
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
    // Vink-native AntiAlias switch: enabled keeps softened gray edge
    // pixels for fast e-paper refresh; disabled uses a hard black/white cutoff.
    if (!antiAliasEnabled()) return (nibble >= 8) ? TFT_BLACK : TFT_WHITE;
    // Slightly darken mid tones so softened strokes remain visible in fast LUTs.
    static const uint8_t kRemap[16] __attribute__((aligned(1))) = {
        0, 0, 1, 2, 3, 4, 5, 7, 8, 10, 11, 12, 13, 14, 15, 15
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
    uint8_t bitPos = 0;
    uint32_t bytePos = 0;
    const uint32_t totalPixels = static_cast<uint32_t>(glyph.bitmapW) * glyph.bitmapH;
    if (totalPixels == 0) return;

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

    auto decodePixel = [&]() -> uint8_t {
        int first = nextBit();
        if (first <= 0) return 0;
        int second = nextBit();
        if (second < 0) return 0;
        return second == 0 ? 10 : 11;
    };

    // The bundled reader font stream is nearly binary, so a plain gray remap is
    // not enough to match Vink's “AntiAlias: reduce jaggies in Fast mode”
    // behavior. Build a tiny per-glyph coverage mask and apply an Vink-native
    // edge curve: light gray on diagonal edge pixels, stronger gray on direct
    // horizontal/vertical edge pixels, then the dark stroke core. This preserves
    // crisp text in Clear/Std while making Fast refresh look much less stairy.
    static constexpr uint32_t kMaxAAGlyphPixels = 48 * 48;
    if (antiAliasEnabled() && color == TFT_BLACK && totalPixels <= kMaxAAGlyphPixels) {
        uint8_t pixels[kMaxAAGlyphPixels];
        memset(pixels, 0, totalPixels);
        for (uint32_t i = 0; i < totalPixels; ++i) pixels[i] = decodePixel();

        const uint16_t directEdge = pixelColorForNibble(4, TFT_BLACK);
        const uint16_t diagonalEdge = pixelColorForNibble(2, TFT_BLACK);
        for (uint16_t row = 0; row < glyph.bitmapH; ++row) {
            for (uint16_t col = 0; col < glyph.bitmapW; ++col) {
                const uint32_t idx = static_cast<uint32_t>(row) * glyph.bitmapW + col;
                if (pixels[idx] != 0) continue;
                bool direct = false;
                bool diagonal = false;
                for (int8_t dy = -1; dy <= 1; ++dy) {
                    const int16_t ny = static_cast<int16_t>(row) + dy;
                    if (ny < 0 || ny >= glyph.bitmapH) continue;
                    for (int8_t dx = -1; dx <= 1; ++dx) {
                        if (dx == 0 && dy == 0) continue;
                        const int16_t nx = static_cast<int16_t>(col) + dx;
                        if (nx < 0 || nx >= glyph.bitmapW) continue;
                        const uint32_t nidx = static_cast<uint32_t>(ny) * glyph.bitmapW + nx;
                        if (pixels[nidx] == 0) continue;
                        if (dx == 0 || dy == 0) direct = true;
                        else diagonal = true;
                    }
                }
                if (!direct && !diagonal) continue;
                const int16_t px = drawX + static_cast<int16_t>(col);
                const int16_t py = drawY + static_cast<int16_t>(row);
                if (px >= 0 && px < kPaperS3Width && py >= 0 && py < kPaperS3Height) {
                    canvas_->drawPixel(px, py, direct ? directEdge : diagonalEdge);
                }
            }
        }

        for (uint32_t i = 0; i < totalPixels; ++i) {
            const uint8_t pixel = pixels[i];
            if (pixel == 0) continue;
            const int16_t px = drawX + static_cast<int16_t>(i % glyph.bitmapW);
            const int16_t py = drawY + static_cast<int16_t>(i / glyph.bitmapW);
            if (px >= 0 && px < kPaperS3Width && py >= 0 && py < kPaperS3Height) {
                canvas_->drawPixel(px, py, pixelColorForNibble(pixel, color));
            }
        }
        return;
    }

    for (uint32_t pixelIdx = 0; pixelIdx < totalPixels && bytePos < glyph.bitmapSize; ++pixelIdx) {
        const uint8_t pixel = decodePixel();
        if (pixel == 0) continue;
        const int16_t px = drawX + static_cast<int16_t>(pixelIdx % glyph.bitmapW);
        const int16_t py = drawY + static_cast<int16_t>(pixelIdx / glyph.bitmapW);
        if (px >= 0 && px < kPaperS3Width && py >= 0 && py < kPaperS3Height) {
            canvas_->drawPixel(px, py, pixelColorForNibble(pixel, color));
        }
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

void ReaderTextRenderer::drawText(int16_t x, int16_t y, const char* text, uint16_t color, int16_t letterGap) {
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
        cx += charAdvance(ch) + letterGap;
    }
}

bool ReaderTextRenderer::isParagraphStart(const char* text, size_t pos, bool chunkStartsAtParagraph) const {
    if (!text) return false;
    if (pos == 0) return chunkStartsAtParagraph;
    size_t i = pos;
    while (i > 0) {
        char c = text[i - 1];
        if (c == '\n' || c == '\r') return true;
        if (c != ' ' && c != '\t') return false;
        --i;
    }
    return chunkStartsAtParagraph;
}

bool ReaderTextRenderer::isForbiddenLineStart(uint32_t unicode) const {
    switch (unicode) {
        case 0x0021: // !
        case 0x0025: // %
        case 0x0029: // )
        case 0x002C: // ,
        case 0x002E: // .
        case 0x003A: // :
        case 0x003B: // ;
        case 0x003F: // ?
        case 0x005D: // ]
        case 0x007D: // }
        case 0x00B7: // ·
        case 0x2019: // ’
        case 0x201D: // ”
        case 0x2026: // …
        case 0x3001: // 、
        case 0x3002: // 。
        case 0x3009: // 〉
        case 0x300B: // 》
        case 0x300D: // 」
        case 0x300F: // 』
        case 0x3011: // 】
        case 0x3015: // 〕
        case 0xFF01: // ！
        case 0xFF09: // ）
        case 0xFF0C: // ，
        case 0xFF0E: // ．
        case 0xFF1A: // ：
        case 0xFF1B: // ；
        case 0xFF1F: // ？
        case 0xFF3D: // ］
        case 0xFF5D: // ｝
            return true;
        default:
            return false;
    }
}

size_t ReaderTextRenderer::skipLeadingSourceIndent(const char* text, size_t pos, size_t len) const {
    while (pos < len) {
        const char c = text[pos];
        if (c == ' ' || c == '\t') {
            ++pos;
            continue;
        }
        // UTF-8 ideographic space U+3000. Vink-native indent should replace
        // source indentation, not stack on top of it.
        if (pos + 2 < len && static_cast<uint8_t>(text[pos]) == 0xE3 &&
            static_cast<uint8_t>(text[pos + 1]) == 0x80 &&
            static_cast<uint8_t>(text[pos + 2]) == 0x80) {
            pos += 3;
            continue;
        }
        break;
    }
    return pos;
}

size_t ReaderTextRenderer::findWrapBreak(const char* text, size_t start, int16_t maxWidth, int16_t letterGap) const {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(text);
    const size_t len = strlen(text);
    size_t pos = start;
    size_t lastGood = start;
    int16_t width = 0;
    while (pos < len) {
        size_t before = pos;
        uint32_t ch = decodeUtf8(bytes, pos, len);
        if (ch == '\n') return before;
        int16_t adv = static_cast<int16_t>(charAdvance(ch)) + letterGap;
        if (width + adv > maxWidth) {
            if (isForbiddenLineStart(ch) && lastGood > start) return pos;
            return lastGood > start ? lastGood : before;
        }
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
    const int16_t baseLineHeight = fontSize() + options.lineGap;
    const int16_t lineHeight = options.dynamicLineHeight ? max<int16_t>(fontSize() + 3, baseLineHeight) : baseLineHeight;
    const int16_t bottom = kPaperS3Height - options.marginBottom;
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(text);

    while (pos < len && y + lineHeight <= bottom) {
        bool skippedBlank = false;
        while (pos < len && (text[pos] == '\n' || text[pos] == '\r')) { pos++; skippedBlank = true; }
        if (skippedBlank && !options.compactBlankLines) y += lineHeight / 2;
        if (pos >= len || y + lineHeight > bottom) break;

        const bool paragraphStart = options.indentFirstLine && isParagraphStart(text, pos, options.startsAtParagraph);
        if (paragraphStart) pos = skipLeadingSourceIndent(text, pos, len);
        const size_t lineStart = pos;
        size_t lastGood = pos;
        int16_t width = paragraphStart ? options.firstLineIndentPx : 0;
        bool hardBreak = false;
        while (pos < len) {
            const size_t before = pos;
            uint32_t ch = decodeUtf8(bytes, pos, len);
            if (ch == '\n' || ch == '\r') {
                pos = before;
                hardBreak = true;
                break;
            }
            const int16_t adv = static_cast<int16_t>(charAdvance(ch)) + options.letterGap;
            if (width + adv > maxWidth) {
                if (options.breakLineOpt && isForbiddenLineStart(ch) && lastGood > lineStart) {
                    lastGood = pos;
                }
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
        if (hardBreak && options.paragraphGap > 0) y += options.paragraphGap;
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
    const int16_t baseLineHeight = fontSize() + options.lineGap;
    const int16_t lineHeight = options.dynamicLineHeight ? max<int16_t>(fontSize() + 3, baseLineHeight) : baseLineHeight;
    const int16_t bottom = kPaperS3Height - options.marginBottom;
    while (pos < len && y + lineHeight <= bottom) {
        bool skippedBlank = false;
        while (pos < len && (text[pos] == '\n' || text[pos] == '\r')) { pos++; skippedBlank = true; }
        if (skippedBlank && !options.compactBlankLines) y += lineHeight / 2;
        if (pos >= len || y + lineHeight > bottom) break;
        const bool paragraphStart = options.indentFirstLine && isParagraphStart(text, pos, options.startsAtParagraph);
        if (paragraphStart) pos = skipLeadingSourceIndent(text, pos, len);
        const int16_t indent = paragraphStart ? options.firstLineIndentPx : 0;
        size_t end = findWrapBreak(text, pos, maxWidth - indent, options.letterGap);
        if (end <= pos) break;
        const bool hardBreak = text[end] == '\n' || text[end] == '\r';
        char line[256];
        size_t n = end - pos;
        if (n >= sizeof(line)) n = sizeof(line) - 1;
        memcpy(line, text + pos, n);
        line[n] = '\0';
        drawText(options.marginLeft + indent, y, line, fg, options.letterGap);
        if (options.underline) {
            const int16_t uy = y + static_cast<int16_t>(fontSize()) + options.underlineOffset;
            for (int16_t x = options.marginLeft; x < kPaperS3Width - options.marginRight; x += 6) {
                canvas_->drawFastHLine(x, uy, 3, mid);
            }
        }
        pos = end;
        y += lineHeight;
        if (hardBreak && options.paragraphGap > 0) y += options.paragraphGap;
    }

    char footer[48];
    snprintf(footer, sizeof(footer), "%u / %u", static_cast<unsigned>(page), static_cast<unsigned>(totalPages));
    drawText(kPaperS3Width - options.marginRight - textWidth(footer), kPaperS3Height - 34, footer, mid);
}

void ReaderTextRenderer::renderListPage(const char* title, const char* summary, const char* const* rows, int rowCount, int16_t rowY, int16_t rowH, uint16_t page, uint16_t totalPages, int activeTab, const ReaderRenderOptions& options) {
    if (!canvas_) return;
    if (!ready()) loadDefaultFont();
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
