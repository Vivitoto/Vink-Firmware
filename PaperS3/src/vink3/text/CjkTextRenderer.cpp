#include "CjkTextRenderer.h"
#include "ReadPaperUiFont.h"
#include "VinkUiFont24.h"
#include "../../Config.h"
#include "../ReadPaper176.h"
#include <pgmspace.h>
#include <cstring>

namespace vink3 {

namespace {
constexpr uint32_t kReadPaperHeaderSize = 134;
constexpr uint32_t kReadPaperEntrySize = 20;
constexpr uint32_t kGrayHeaderSize = 16;
constexpr uint32_t kGrayEntrySize = 16;
}

CjkTextRenderer g_cjkText;

bool CjkTextRenderer::begin(M5Canvas* canvas) {
    canvas_ = canvas;
    progmemUiReady_ = false;
    progmemUiCharCount_ = 0;
    progmemUiFontSize_ = 0;
    progmemUiBaseline_ = 0;
    progmemUiBitmapStart_ = 0;
    readPaperSubsetReady_ = false;
    readPaperCharCount_ = 0;
    readPaperFontHeight_ = 0;
    if (!canvas_) return false;

    // Official PaperS3 docs confirm a 16-level grayscale EPD, but the custom
    // font source is Vink-owned. Real-device feedback showed SPIFFS-dependent
    // UI fonts can silently fall back to the tiny built-in subset and then mix
    // with 32px ReadPaper glyphs. Make the compiled 24px Simplified Chinese UI
    // font the primary path so the shell does not depend on the filesystem.
    const bool progmemUi = beginProgmemUiFont();
    if (progmemUi) {
        Serial.printf("[vink3][cjk] PROGMEM SC 24px UI font loaded: glyphs=%lu size=%lu\n",
                      static_cast<unsigned long>(progmemUiCharCount_),
                      static_cast<unsigned long>(g_vink_ui_font24_size));
    }

    // SPIFFS font remains a secondary fallback only. It is useful for future font
    // switching, but it must not be required for the core shell to be readable.
    const bool bundled16 = font_.loadBundledFont(FONT_FILE_16) &&
        strcmp(font_.getCurrentFontPath(), FONT_FILE_16) == 0;
    if (bundled16) Serial.println("[vink3][cjk] bundled SC 16px UI font fallback loaded");
    const bool bundled20 = bundled16 ? false :
        (font_.loadBundledFont(FONT_FILE_20) &&
         strcmp(font_.getCurrentFontPath(), FONT_FILE_20) == 0);
    if (bundled20) Serial.println("[vink3][cjk] bundled SC 20px UI font fallback loaded");
    if (!bundled16 && !bundled20 && font_.isLoaded()) {
        Serial.printf("[vink3][cjk] bundled SC font unavailable, secondary fallback active: %s\n",
                      font_.getCurrentFontPath());
    }

    const bool subset = beginReadPaperSubset();
    if (subset) {
        Serial.printf("[vink3][cjk] ReadPaper V3 UI subset last-resort fallback armed: glyphs=%lu size=%lu\n",
                      static_cast<unsigned long>(readPaperCharCount_),
                      static_cast<unsigned long>(g_readpaper_ui_font_size));
    }

    if (progmemUi || font_.isLoaded() || subset) return true;

    if (font_.loadBuiltinFont()) {
        Serial.println("[vink3][cjk] fallback built-in UI font loaded; Chinese coverage may be limited");
        return true;
    }

    Serial.println("[vink3][cjk] CJK font load failed; M5GFX text fallback may miss Chinese");
    return false;
}

bool CjkTextRenderer::ready() const {
    return canvas_ && (progmemUiReady_ || readPaperSubsetReady_ || font_.isLoaded());
}

uint16_t CjkTextRenderer::fontSize() const {
    // Layout must follow the primary renderer actually used for most glyphs.
    // The compiled 24px UI font is now primary; SPIFFS and ReadPaper are only
    // fallbacks, so their height must not pull tab/button/settings centering.
    if (progmemUiReady_) return progmemUiFontSize_;
    if (font_.isLoaded()) return font_.getFontSize();
    if (readPaperSubsetReady_) return readPaperFontHeight_;
    return 16;
}

uint8_t CjkTextRenderer::rpByte(uint32_t offset) {
    if (offset >= g_readpaper_ui_font_size) return 0;
    return pgm_read_byte(&g_readpaper_ui_font_data[offset]);
}

uint16_t CjkTextRenderer::rpU16(uint32_t offset) {
    uint8_t b0 = rpByte(offset);
    uint8_t b1 = rpByte(offset + 1);
    return static_cast<uint16_t>(b0) | (static_cast<uint16_t>(b1) << 8);
}

uint32_t CjkTextRenderer::rpU32(uint32_t offset) {
    uint32_t b0 = rpByte(offset);
    uint32_t b1 = rpByte(offset + 1);
    uint32_t b2 = rpByte(offset + 2);
    uint32_t b3 = rpByte(offset + 3);
    return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
}

int8_t CjkTextRenderer::rpI8(uint32_t offset) {
    return static_cast<int8_t>(rpByte(offset));
}

uint8_t CjkTextRenderer::uiByte(uint32_t offset) {
    if (offset >= g_vink_ui_font24_size) return 0;
    return pgm_read_byte(&g_vink_ui_font24_data[offset]);
}

uint16_t CjkTextRenderer::uiU16(uint32_t offset) {
    uint8_t b0 = uiByte(offset);
    uint8_t b1 = uiByte(offset + 1);
    return static_cast<uint16_t>(b0) | (static_cast<uint16_t>(b1) << 8);
}

uint32_t CjkTextRenderer::uiU32(uint32_t offset) {
    uint32_t b0 = uiByte(offset);
    uint32_t b1 = uiByte(offset + 1);
    uint32_t b2 = uiByte(offset + 2);
    uint32_t b3 = uiByte(offset + 3);
    return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
}

int8_t CjkTextRenderer::uiI8(uint32_t offset) {
    return static_cast<int8_t>(uiByte(offset));
}

bool CjkTextRenderer::beginProgmemUiFont() {
    if (!g_vink_ui_font24_available || g_vink_ui_font24_size < kGrayHeaderSize) return false;
    if (uiByte(0) != 'G' || uiByte(1) != 'R' || uiByte(2) != 'A' || uiByte(3) != 'Y') return false;
    const uint16_t version = uiU16(4);
    progmemUiFontSize_ = uiU16(6);
    progmemUiCharCount_ = uiU32(8);
    const uint32_t bitmapBytes = uiU32(12);
    progmemUiBitmapStart_ = kGrayHeaderSize + progmemUiCharCount_ * kGrayEntrySize;
    if (version != 1 || progmemUiFontSize_ == 0 || progmemUiCharCount_ == 0) return false;
    if (progmemUiBitmapStart_ >= g_vink_ui_font24_size) return false;
    if (progmemUiBitmapStart_ + bitmapBytes != g_vink_ui_font24_size) return false;
    progmemUiReady_ = true;
    deriveProgmemUiMetrics();
    return true;
}

void CjkTextRenderer::deriveProgmemUiMetrics() {
    // FreeType stores each glyph relative to a baseline. Previous UI drawing
    // centered every bitmap by its own height, which made mixed-case words like
    // "Legado" float per letter and erased descenders: g/p/y should extend
    // below the L baseline. Derive one common baseline from representative CJK
    // UI glyphs, then draw every glyph against that same baseline.
    progmemUiBaseline_ = (progmemUiFontSize_ * 7) / 8;
    const uint32_t samples[] = {
        0x8BBE, // 设
        0x7F6E, // 置
        0x8FDB, // 进
        0x5EA6, // 度
        0x540C, // 同
        0x6B65, // 步
        'H', 'L', 'd', 'g', 'p', 'y'
    };
    uint16_t maxBearing = 0;
    for (uint32_t sample : samples) {
        GrayGlyph glyph;
        if (findProgmemUiGlyph(sample, glyph) && glyph.bearingY > maxBearing) {
            maxBearing = glyph.bearingY;
        }
    }
    if (maxBearing > 0) progmemUiBaseline_ = maxBearing;
}

bool CjkTextRenderer::findProgmemUiGlyph(uint32_t unicode, GrayGlyph& out) const {
    if (!progmemUiReady_) return false;
    int32_t lo = 0;
    int32_t hi = static_cast<int32_t>(progmemUiCharCount_) - 1;
    while (lo <= hi) {
        int32_t mid = lo + (hi - lo) / 2;
        uint32_t off = kGrayHeaderSize + static_cast<uint32_t>(mid) * kGrayEntrySize;
        uint32_t cp = uiU32(off);
        if (cp == unicode) {
            out.unicode = cp;
            out.bitmapOffset = uiU32(off + 4);
            out.width = uiByte(off + 8);
            out.height = uiByte(off + 9);
            out.bearingX = uiI8(off + 10);
            out.bearingY = uiI8(off + 11);
            out.advance = uiByte(off + 12);
            const uint32_t bitmapSize = static_cast<uint32_t>((out.width + 1) / 2) * out.height;
            return out.width > 0 && out.height > 0 && out.advance > 0 &&
                   out.bitmapOffset >= progmemUiBitmapStart_ &&
                   out.bitmapOffset + bitmapSize <= g_vink_ui_font24_size;
        }
        if (cp < unicode) lo = mid + 1;
        else hi = mid - 1;
    }
    return false;
}

bool CjkTextRenderer::beginReadPaperSubset() {
    if (!g_readpaper_ui_font_available || g_readpaper_ui_font_size < kReadPaperHeaderSize) return false;
    readPaperCharCount_ = rpU32(0);
    readPaperFontHeight_ = rpByte(4);
    const uint8_t version = rpByte(5);
    if (readPaperCharCount_ == 0 || readPaperFontHeight_ == 0 || version != 3) return false;
    const uint32_t entriesEnd = kReadPaperHeaderSize + readPaperCharCount_ * kReadPaperEntrySize;
    if (entriesEnd >= g_readpaper_ui_font_size) return false;
    readPaperSubsetReady_ = true;
    return true;
}

bool CjkTextRenderer::findReadPaperGlyph(uint32_t unicode, ReadPaperGlyph& out) const {
    if (!readPaperSubsetReady_ || unicode > 0xFFFF) return false;
    int32_t lo = 0;
    int32_t hi = static_cast<int32_t>(readPaperCharCount_) - 1;
    while (lo <= hi) {
        int32_t mid = lo + (hi - lo) / 2;
        uint32_t off = kReadPaperHeaderSize + static_cast<uint32_t>(mid) * kReadPaperEntrySize;
        uint16_t cp = rpU16(off);
        if (cp == unicode) {
            out.unicode = cp;
            out.width = rpU16(off + 2);
            out.bitmapW = rpByte(off + 4);
            out.bitmapH = rpByte(off + 5);
            out.xOffset = rpI8(off + 6);
            out.yOffset = rpI8(off + 7);
            out.bitmapOffset = rpU32(off + 8);
            out.bitmapSize = rpU32(off + 12);
            return out.bitmapOffset + out.bitmapSize <= g_readpaper_ui_font_size;
        }
        if (cp < unicode) lo = mid + 1;
        else hi = mid - 1;
    }
    return false;
}

uint32_t CjkTextRenderer::decodeUtf8(const uint8_t* buf, size_t& pos, size_t len) {
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

int16_t CjkTextRenderer::textWidth(const char* text) {
    if (!text) return 0;

    int16_t w = 0;
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(text);
    size_t pos = 0;
    const size_t len = strlen(text);
    while (pos < len) {
        uint32_t ch = decodeUtf8(bytes, pos, len);
        GrayGlyph uiGlyph;
        if (findProgmemUiGlyph(ch, uiGlyph)) {
            w += uiGlyph.advance > 0 ? uiGlyph.advance : (ch < 128 ? 8 : fontSize());
        } else if (font_.isLoaded()) {
            uint8_t adv = font_.getCharAdvance(ch);
            w += adv > 0 ? adv : (ch < 128 ? 8 : fontSize());
        } else if (readPaperSubsetReady_) {
            ReadPaperGlyph glyph;
            if (findReadPaperGlyph(ch, glyph)) {
                w += glyph.width > 0 ? glyph.width : (ch < 128 ? 8 : fontSize());
            } else {
                w += ch < 128 ? 8 : fontSize();
            }
        } else {
            w += ch < 128 ? 8 : fontSize();
        }
    }
    return w;
}

uint16_t CjkTextRenderer::pixelColorForNibble(uint8_t nibble, uint16_t color) const {
    if (color == TFT_WHITE) return TFT_WHITE;
    if (color != TFT_BLACK) return color;
    // E-paper has only 16 gray levels with a steep contrast response curve.
    // A+B+D: linear kRemap prevents Bayer dithering artifacts at AA edge
    // pixels (nibble 1-4). No aggressive compression — edge pixels stay closer
    // to background gray (~192), producing smoother Bayer transitions.
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

void CjkTextRenderer::drawProgmemUiGlyph(const GrayGlyph& glyph, int16_t x, int16_t y, uint16_t color) {
    if (!canvas_) return;
    const int16_t drawX = x + glyph.bearingX;
    // y is the UI line-box top. Use a shared baseline for all glyphs instead
    // of centering each bitmap independently; this keeps uppercase/lowercase,
    // CJK labels, and descenders aligned in the same word/row.
    const int16_t drawY = y + static_cast<int16_t>(progmemUiBaseline_) - static_cast<int16_t>(glyph.bearingY);
    const uint32_t rowBytes = (glyph.width + 1) / 2;
    for (uint8_t row = 0; row < glyph.height; row++) {
        const int16_t py = drawY + row;
        if (py < 0 || py >= kPaperS3Height) continue;
        for (uint8_t col = 0; col < glyph.width; col++) {
            const int16_t px = drawX + col;
            if (px < 0 || px >= kPaperS3Width) continue;
            const uint8_t packed = uiByte(glyph.bitmapOffset + row * rowBytes + col / 2);
            const uint8_t nibble = (col % 2 == 0) ? ((packed >> 4) & 0x0F) : (packed & 0x0F);
            if (nibble > 0) canvas_->drawPixel(px, py, pixelColorForNibble(nibble, color));
        }
    }
}

void CjkTextRenderer::drawReadPaperGlyph(const ReadPaperGlyph& glyph, int16_t x, int16_t y, uint16_t color) {
    if (!canvas_) return;
    const int16_t drawX = x + glyph.xOffset;
    // Experience-based, needs real PaperS3 confirmation: Vink UI renderers pass
    // y as a line-box top, while ReadPaper glyph yOffset is baseline-oriented.
    // Using y + fontHeight - yOffset caused mixed Latin/CJK fallback glyphs to
    // stair-step. Keep fallback glyphs visually top-aligned in the same line box.
    const int16_t drawY = y + max<int16_t>(0, (static_cast<int16_t>(fontSize()) - static_cast<int16_t>(glyph.bitmapH)) / 2);
    uint32_t pixelIdx = 0;
    uint8_t bitPos = 0;
    uint32_t bytePos = 0;
    const uint32_t totalPixels = static_cast<uint32_t>(glyph.bitmapW) * glyph.bitmapH;

    auto nextBit = [&]() -> int {
        if (bytePos >= glyph.bitmapSize) return -1;
        uint8_t current = rpByte(glyph.bitmapOffset + bytePos);
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
        uint8_t pixel = 0; // white / transparent
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

void CjkTextRenderer::drawGlyph(uint32_t unicode, int16_t x, int16_t y, uint16_t color) {
    if (!canvas_) return;

    GrayGlyph uiGlyph;
    if (findProgmemUiGlyph(unicode, uiGlyph)) {
        drawProgmemUiGlyph(uiGlyph, x, y, color);
        return;
    }

    // Prefer the bundled SC font when it actually contains the glyph. If the
    // SPIFFS font was not flashed, fell back to the tiny built-in font, or simply
    // misses a character, keep going to the compiled ReadPaper subset instead
    // of silently advancing the cursor and leaving blank Chinese labels.
    if (font_.isLoaded()) {
        if (font_.getFontType() == FontType::GRAY_4BPP) {
            uint8_t width = 0, height = 0, advance = 0;
            int8_t bearingX = 0, bearingY = 0;
            const uint8_t* bmp = font_.getCharBitmapGray(unicode, width, height, bearingX, bearingY, advance);
            if (bmp && width > 0 && height > 0) {
                const int16_t drawX = x + bearingX;
                // UI callers pass y as the top of a text box, not a FreeType
                // baseline. Draw against one baseline so Latin descenders stay
                // below capitals and mixed CJK/Latin labels do not stair-step.
                const int16_t fallbackBaseline = (static_cast<int16_t>(font_.getFontSize()) * 7) / 8;
                const int16_t drawY = y + fallbackBaseline - static_cast<int16_t>(bearingY);
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
        } else {
            uint8_t width = 0, height = 0;
            const uint8_t* bmp = font_.getCharBitmap(unicode, width, height);
            if (bmp && width > 0 && height > 0) {
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
                return;
            }
        }
    }

    if (readPaperSubsetReady_) {
        ReadPaperGlyph glyph;
        if (findReadPaperGlyph(unicode, glyph)) {
            drawReadPaperGlyph(glyph, x, y, color);
        }
    }
}

void CjkTextRenderer::drawText(int16_t x, int16_t y, const char* text, uint16_t color) {
    if (!canvas_ || !text) return;
    if (!progmemUiReady_ && !readPaperSubsetReady_ && !font_.isLoaded()) {
        canvas_->setTextColor(color, TFT_WHITE);
        canvas_->setTextSize(1);
        canvas_->setCursor(x, y);
        canvas_->print(text);
        return;
    }

    int16_t cx = x;
    int16_t cy = y;
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(text);
    size_t pos = 0;
    const size_t len = strlen(text);
    while (pos < len && cx < kPaperS3Width) {
        uint32_t ch = decodeUtf8(bytes, pos, len);
        if (ch == '\n') {
            cy += fontSize() + 6;
            cx = x;
            continue;
        }
        drawGlyph(ch, cx, cy, color);
        GrayGlyph uiGlyph;
        if (findProgmemUiGlyph(ch, uiGlyph)) {
            cx += uiGlyph.advance > 0 ? uiGlyph.advance : (ch < 128 ? 8 : fontSize());
        } else if (font_.isLoaded()) {
            uint8_t adv = font_.getCharAdvance(ch);
            cx += adv > 0 ? adv : (ch < 128 ? 8 : fontSize());
        } else if (readPaperSubsetReady_) {
            ReadPaperGlyph glyph;
            cx += findReadPaperGlyph(ch, glyph) && glyph.width > 0 ? glyph.width : (ch < 128 ? 8 : fontSize());
        } else {
            cx += ch < 128 ? 8 : fontSize();
        }
    }
}

void CjkTextRenderer::drawCentered(int16_t x, int16_t y, int16_t w, int16_t h, const char* text, uint16_t color) {
    const int16_t tw = textWidth(text ? text : "");
    // Leave room for descenders below the baseline; otherwise g/p/y look like
    // uppercase-height glyphs when centered in buttons and tabs.
    const int16_t th = static_cast<int16_t>(fontSize()) + 6;
    drawText(x + (w - tw) / 2, y + (h - th) / 2, text, color);
}

void CjkTextRenderer::drawRight(int16_t rightX, int16_t y, const char* text, uint16_t color) {
    drawText(rightX - textWidth(text ? text : ""), y, text, color);
}

} // namespace vink3
