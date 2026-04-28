#include "CjkTextRenderer.h"
#include "../../Config.h"
#include "../ReadPaper176.h"

namespace vink3 {

CjkTextRenderer g_cjkText;

bool CjkTextRenderer::begin(M5Canvas* canvas) {
    canvas_ = canvas;
    if (!canvas_) return false;

    // UI should be independent of SD card fonts. Prefer bundled SPIFFS CJK fonts.
    if (font_.loadBundledFont(FONT_FILE_20)) {
        Serial.println("[vink3][cjk] bundled 20px UI font loaded");
        return true;
    }
    if (font_.loadBundledFont(FONT_FILE_16)) {
        Serial.println("[vink3][cjk] bundled 16px UI font loaded");
        return true;
    }
    if (font_.loadBuiltinFont()) {
        Serial.println("[vink3][cjk] fallback built-in UI font loaded");
        return true;
    }

    Serial.println("[vink3][cjk] CJK font load failed; M5GFX text fallback may miss Chinese");
    return false;
}

bool CjkTextRenderer::ready() const {
    return canvas_ && font_.isLoaded();
}

uint16_t CjkTextRenderer::fontSize() const {
    return font_.isLoaded() ? font_.getFontSize() : 16;
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
    if (!font_.isLoaded()) return strlen(text) * 8;

    int16_t w = 0;
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(text);
    size_t pos = 0;
    const size_t len = strlen(text);
    while (pos < len) {
        uint32_t ch = decodeUtf8(bytes, pos, len);
        uint8_t adv = font_.getCharAdvance(ch);
        w += adv > 0 ? adv : (ch < 128 ? 8 : fontSize());
    }
    return w;
}

uint16_t CjkTextRenderer::pixelColorForNibble(uint8_t nibble, uint16_t color) const {
    if (color == TFT_WHITE) return TFT_WHITE;
    if (color != TFT_BLACK) return color;
    // Basic grayscale mapping for 4bpp antialias pixels on PaperS3.
    if (nibble >= 11) return TFT_BLACK;
    if (nibble >= 6) return 0x8410;  // mid gray
    return 0xC618;                   // light gray
}

void CjkTextRenderer::drawGlyph(uint32_t unicode, int16_t x, int16_t y, uint16_t color) {
    if (!canvas_ || !font_.isLoaded()) return;

    if (font_.getFontType() == FontType::GRAY_4BPP) {
        uint8_t width = 0, height = 0, advance = 0;
        int8_t bearingX = 0, bearingY = 0;
        const uint8_t* bmp = font_.getCharBitmapGray(unicode, width, height, bearingX, bearingY, advance);
        if (!bmp || width == 0 || height == 0) return;
        const int16_t drawX = x + bearingX;
        const int16_t drawY = y + (font_.getFontSize() - bearingY);
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

void CjkTextRenderer::drawText(int16_t x, int16_t y, const char* text, uint16_t color) {
    if (!canvas_ || !text) return;
    if (!font_.isLoaded()) {
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
        uint8_t adv = font_.getCharAdvance(ch);
        cx += adv > 0 ? adv : (ch < 128 ? 8 : fontSize());
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
