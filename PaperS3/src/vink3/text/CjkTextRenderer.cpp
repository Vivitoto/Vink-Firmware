#include "CjkTextRenderer.h"
#include "ReadPaperUiFont.h"
#include "../../Config.h"
#include "../ReadPaper176.h"
#include <pgmspace.h>
#include <cstring>

namespace vink3 {

namespace {
constexpr uint32_t kReadPaperHeaderSize = 134;
constexpr uint32_t kReadPaperEntrySize = 20;
}

CjkTextRenderer g_cjkText;

bool CjkTextRenderer::begin(M5Canvas* canvas) {
    canvas_ = canvas;
    readPaperSubsetReady_ = false;
    readPaperCharCount_ = 0;
    readPaperFontHeight_ = 0;
    if (!canvas_) return false;

    // Vink UI prefers bundled Simplified Chinese SC fonts, but real-device
    // feedback showed the UI can become mostly chrome/boxes if the SPIFFS font
    // is missing, not flashed, or lacks a glyph. Always arm the compiled
    // ReadPaper UI subset as a PROGMEM glyph-level fallback; do not treat it as
    // mutually exclusive with the SC font.
    const bool bundled16 = font_.loadBundledFont(FONT_FILE_16) &&
        strcmp(font_.getCurrentFontPath(), FONT_FILE_16) == 0;
    if (bundled16) Serial.println("[vink3][cjk] bundled SC 16px UI font loaded");
    const bool bundled20 = bundled16 ? false :
        (font_.loadBundledFont(FONT_FILE_20) && strcmp(font_.getCurrentFontPath(), FONT_FILE_20) == 0);
    if (bundled20) Serial.println("[vink3][cjk] bundled SC 20px UI font loaded");
    if (!bundled16 && !bundled20 && font_.isLoaded()) {
        Serial.printf("[vink3][cjk] bundled SC font unavailable, primary fallback active: %s\n",
                      font_.getCurrentFontPath());
    }

    const bool subset = beginReadPaperSubset();
    if (subset) {
        Serial.printf("[vink3][cjk] ReadPaper V3 UI subset glyph fallback armed: glyphs=%lu size=%lu\n",
                      static_cast<unsigned long>(readPaperCharCount_),
                      static_cast<unsigned long>(g_readpaper_ui_font_size));
    }

    if (font_.isLoaded() || subset) return true;

    if (font_.loadBuiltinFont()) {
        Serial.println("[vink3][cjk] fallback built-in UI font loaded; Chinese coverage may be limited");
        return true;
    }

    Serial.println("[vink3][cjk] CJK font load failed; M5GFX text fallback may miss Chinese");
    return false;
}

bool CjkTextRenderer::ready() const {
    return canvas_ && (readPaperSubsetReady_ || font_.isLoaded());
}

uint16_t CjkTextRenderer::fontSize() const {
    // Layout must follow the primary renderer actually used for most glyphs.
    // The ReadPaper subset may be armed only as a missing-glyph fallback; using
    // its taller height here made tabs/buttons/settings rows calculate the wrong
    // center line even when the bundled 16px SC font was active.
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
        if (font_.isLoaded()) {
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
    // E-paper photos made low-alpha antialias pixels look like missing
    // characters. Bias UI glyphs toward solid black for legibility.
    if (nibble >= 4) return TFT_BLACK;
    return 0x8410;
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
                // baseline. Using `fontSize - bearingY` here made Latin letters
                // jump up/down within the same word on PaperS3 photos because
                // each glyph has a different bearingY. Keep glyphs top-aligned
                // inside the UI line box; descender support is less important
                // than stable chrome labels for tabs/buttons/settings rows.
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
    if (!readPaperSubsetReady_ && !font_.isLoaded()) {
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
        if (font_.isLoaded()) {
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
    const int16_t th = fontSize();
    drawText(x + (w - tw) / 2, y + (h - th) / 2 - 2, text, color);
}

void CjkTextRenderer::drawRight(int16_t rightX, int16_t y, const char* text, uint16_t color) {
    drawText(rightX - textWidth(text ? text : ""), y, text, color);
}

} // namespace vink3
