#include "CjkTextRenderer.h"
#include "VinkUiFont24.h"
#include "VinkUiFont16.h"
#include "../../Config.h"
#include "../VinkPaperS3.h"
#include <pgmspace.h>
#include <cstring>

namespace vink3 {

namespace {
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
    progmemUiVisualTop_ = 0;
    progmemUiVisualBottom_ = 0;
    progmemUiBitmapStart_ = 0;
    progmemUi16Ready_ = false;
    progmemUi16CharCount_ = 0;
    progmemUi16FontSize_ = 0;
    progmemUi16Baseline_ = 0;
    progmemUi16BitmapStart_ = 0;
    if (!canvas_) return false;

    const bool progmemUi = beginProgmemUiFont();
    if (progmemUi) {
        Serial.printf("[vink3][cjk] PROGMEM Bold 24px UI font loaded: glyphs=%lu size=%lu\n",
                      static_cast<unsigned long>(progmemUiCharCount_),
                      static_cast<unsigned long>(g_vink_ui_font24_size));
    } else {
        Serial.println("[vink3][cjk] PROGMEM UI font unavailable");
    }

    // Try PROGMEM 16px font first (always available, no SPIFFS dependency).
    const bool progmemUi16 = beginProgmemUi16Font();
    if (progmemUi16) {
        Serial.printf("[vink3][cjk] PROGMEM 16px small font loaded: glyphs=%lu size=%lu\n",
                      static_cast<unsigned long>(progmemUi16CharCount_),
                      static_cast<unsigned long>(g_vink_ui_font16_size));
    } else {
        // Fallback: try SPIFFS 16px font (may be unreliable on real devices)
        if (fontSmall_.loadFont("/fonts/noto_bold_16.fnt")) {
            Serial.println("[vink3][cjk] SPIFFS Bold 16px small font loaded (fallback)");
        } else {
            Serial.println("[vink3][cjk] no 16px font available — small text will use 24px");
        }
    }

    if (progmemUi) return true;

    Serial.println("[vink3][cjk] compiled UI font unavailable; M5GFX ASCII fallback may miss Chinese");
    return false;

}

bool CjkTextRenderer::ready() const {
    return canvas_ && progmemUiReady_;
}

uint16_t CjkTextRenderer::fontSize() const {
    // Layout follows the compiled UI font. Keeping UI text independent from
    // SD/SPIFFS fonts avoids surprise missing-glyph fallbacks on fresh cards.
    if (progmemUiReady_) return progmemUiFontSize_;
    return 16;
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

    // Derive a real visual line box from representative UI glyphs. Boxed text
    // (tabs/buttons/cards/settings rows) should align its optical glyph center,
    // not the nominal fontSize(), otherwise labels sit high/low in their frames.
    bool haveBounds = false;
    int16_t top = 0;
    int16_t bottom = static_cast<int16_t>(progmemUiFontSize_);
    for (uint32_t sample : samples) {
        GrayGlyph glyph;
        if (!findProgmemUiGlyph(sample, glyph)) continue;
        const int16_t gt = static_cast<int16_t>(progmemUiBaseline_) - static_cast<int16_t>(glyph.bearingY);
        const int16_t gb = gt + static_cast<int16_t>(glyph.height);
        if (!haveBounds) {
            top = gt;
            bottom = gb;
            haveBounds = true;
        } else {
            if (gt < top) top = gt;
            if (gb > bottom) bottom = gb;
        }
    }
    progmemUiVisualTop_ = top;
    progmemUiVisualBottom_ = bottom;
}

int16_t CjkTextRenderer::lineTopForBox(int16_t y, int16_t h) const {
    const int16_t visualTop = progmemUiReady_ ? progmemUiVisualTop_ : 0;
    const int16_t visualBottom = progmemUiReady_ ? progmemUiVisualBottom_ : static_cast<int16_t>(fontSize());
    const int16_t visualCenter = static_cast<int16_t>((visualTop + visualBottom) / 2);
    return static_cast<int16_t>(y + h / 2 - visualCenter);
}

int16_t CjkTextRenderer::smallLineTopForBox(int16_t y, int16_t h) const {
    const int16_t lineH = (progmemUi16Ready_ ? static_cast<int16_t>(progmemUi16FontSize_) : (fontSmall_.isLoaded() ? 16 : static_cast<int16_t>(fontSize())));
    return static_cast<int16_t>(y + (h - lineH) / 2);
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
        } else {
            w += ch < 128 ? 8 : fontSize();
        }
    }
    return w;
}

void CjkTextRenderer::fitTextToWidth(const char* text, char* out, size_t outSize, int16_t maxWidth) {
    if (!out || outSize == 0) return;
    out[0] = '\0';
    if (!text) return;
    strlcpy(out, text, outSize);
    if (textWidth(out) <= maxWidth) return;

    static constexpr const char* kEllipsis = "...";
    const int16_t ellipsisWidth = textWidth(kEllipsis);
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(text);
    const size_t len = strlen(text);
    size_t pos = 0;
    size_t lastFit = 0;
    while (pos < len) {
        const size_t before = pos;
        (void)decodeUtf8(bytes, pos, len);
        if (pos <= before) break;
        char candidate[160];
        const size_t n = min(before == 0 ? pos : pos, sizeof(candidate) - 1);
        memcpy(candidate, text, n);
        candidate[n] = '\0';
        if (textWidth(candidate) + ellipsisWidth > maxWidth) break;
        lastFit = pos;
    }
    if (lastFit == 0) {
        strlcpy(out, kEllipsis, outSize);
        return;
    }
    const size_t copyLen = min(lastFit, outSize - 1);
    memcpy(out, text, copyLen);
    out[copyLen] = '\0';
    if (copyLen + strlen(kEllipsis) < outSize) strcat(out, kEllipsis);
}

uint16_t CjkTextRenderer::pixelColorForNibble(uint8_t nibble, uint16_t color) const {
    if (color == TFT_WHITE) return TFT_WHITE;
    if (color != TFT_BLACK) return color;
    // Gamma-0.7 anti-aliasing curve for EPD.
    // Pushes edge nibbles 1-4 deeper into visible gray so Bayer dithering
    // (IT8951 ±30) cannot scatter them back into imperceptible near-white noise.
    // Without this curve the anti-alias data in the font is wasted — every
    // edge pixel becomes random on/off chessboard noise that looks like jaggies.
    static const uint8_t kRemap[16] __attribute__((aligned(1))) = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
    };
    static const uint16_t k4BitToRgb565[16] __attribute__((aligned(2))) = {
        0xFFFF, 0xDEDB, 0xC618, 0xAD75, 0x9CD3, 0x8C51, 0x7BCF, 0x6B4D,
        0x5ACB, 0x4A69, 0x39E7, 0x3186, 0x2124, 0x10A2, 0x0841, 0x0000
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

void CjkTextRenderer::drawGlyph(uint32_t unicode, int16_t x, int16_t y, uint16_t color) {
    if (!canvas_) return;

    GrayGlyph uiGlyph;
    if (findProgmemUiGlyph(unicode, uiGlyph)) {
        drawProgmemUiGlyph(uiGlyph, x, y, color);
        return;
    }

    // Missing glyphs should be visible during debugging instead of silently
    // leaving blanks in book names or TOC rows. With the expanded GB2312 UI
    // font this should be rare.
    if (unicode > 0x20) {
        const int16_t box = max<int16_t>(8, fontSize() - 8);
        canvas_->drawRect(x + 2, y + 4, box, box, color);
    }

}

void CjkTextRenderer::drawText(int16_t x, int16_t y, const char* text, uint16_t color) {
    if (!canvas_ || !text) return;
    if (!progmemUiReady_) {
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
        } else {
            cx += ch < 128 ? 8 : fontSize();
        }
    }
}

void CjkTextRenderer::drawCentered(int16_t x, int16_t y, int16_t w, int16_t h, const char* text, uint16_t color) {
    const int16_t tw = textWidth(text ? text : "");
    drawText(x + (w - tw) / 2, lineTopForBox(y, h), text, color);
}

void CjkTextRenderer::drawRight(int16_t rightX, int16_t y, const char* text, uint16_t color) {
    drawText(rightX - textWidth(text ? text : ""), y, text, color);
}

// ── 16px PROGMEM font helpers ───────────────────────────────────────────

static uint8_t ui16Byte(uint32_t offset) {
    return pgm_read_byte(&g_vink_ui_font16_data[offset]);
}
static uint16_t ui16U16(uint32_t offset) {
    return static_cast<uint16_t>(ui16Byte(offset)) | (static_cast<uint16_t>(ui16Byte(offset + 1)) << 8);
}
static uint32_t ui16U32(uint32_t offset) {
    return static_cast<uint32_t>(ui16Byte(offset)) | (static_cast<uint32_t>(ui16Byte(offset + 1)) << 8) |
           (static_cast<uint32_t>(ui16Byte(offset + 2)) << 16) | (static_cast<uint32_t>(ui16Byte(offset + 3)) << 24);
}
static int8_t ui16I8(uint32_t offset) {
    return static_cast<int8_t>(ui16Byte(offset));
}

bool CjkTextRenderer::beginProgmemUi16Font() {
    if (!g_vink_ui_font16_available || g_vink_ui_font16_size < kGrayHeaderSize) return false;
    if (ui16Byte(0) != 'G' || ui16Byte(1) != 'R' || ui16Byte(2) != 'A' || ui16Byte(3) != 'Y') return false;
    const uint16_t version = ui16U16(4);
    progmemUi16FontSize_ = ui16U16(6);
    progmemUi16CharCount_ = ui16U32(8);
    const uint32_t bitmapBytes = ui16U32(12);
    progmemUi16BitmapStart_ = kGrayHeaderSize + progmemUi16CharCount_ * kGrayEntrySize;
    if (version != 1 || progmemUi16FontSize_ == 0 || progmemUi16CharCount_ == 0) return false;
    if (progmemUi16BitmapStart_ >= g_vink_ui_font16_size) return false;
    if (progmemUi16BitmapStart_ + bitmapBytes != g_vink_ui_font16_size) return false;
    progmemUi16Ready_ = true;
    progmemUi16Baseline_ = (progmemUi16FontSize_ * 7) / 8;
    return true;
}

bool CjkTextRenderer::findProgmemUi16Glyph(uint32_t unicode, GrayGlyph& out) const {
    if (!progmemUi16Ready_) return false;
    uint32_t lo = 0, hi = progmemUi16CharCount_;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint32_t off = kGrayHeaderSize + mid * kGrayEntrySize;
        uint32_t cu = ui16U32(off);
        if (cu == unicode) {
            out.bitmapOffset = ui16U32(off + 4);
            out.width = ui16Byte(off + 8);
            out.height = ui16Byte(off + 9);
            out.bearingX = ui16I8(off + 10);
            out.bearingY = ui16I8(off + 11);
            out.advance = ui16Byte(off + 12);
            return true;
        }
        if (cu < unicode) lo = mid + 1;
        else hi = mid;
    }
    return false;
}

void CjkTextRenderer::drawSmallGlyph(uint32_t unicode, int16_t x, int16_t y, uint16_t color) {
    if (!canvas_ || !progmemUi16Ready_) return;
    GrayGlyph glyph;
    if (!findProgmemUi16Glyph(unicode, glyph)) return;
    if (glyph.width == 0 || glyph.height == 0) return;

    const int16_t drawY = y + static_cast<int16_t>(progmemUi16Baseline_) - static_cast<int16_t>(glyph.bearingY);
    const uint32_t bitmapOffset = glyph.bitmapOffset;
    const uint8_t rowBytes = (glyph.width + 1) / 2;

    for (int16_t row = 0; row < glyph.height; ++row) {
        const int16_t py = drawY + row;
        if (py < 0 || py >= kPaperS3Height) continue;
        for (int16_t col = 0; col < glyph.width; ++col) {
            const int16_t px = x + glyph.bearingX + col;
            if (px < 0 || px >= kPaperS3Width) continue;
            const uint8_t packed = pgm_read_byte(&g_vink_ui_font16_data[bitmapOffset + row * rowBytes + col / 2]);
            const uint8_t nibble = (col % 2 == 0) ? ((packed >> 4) & 0x0F) : (packed & 0x0F);
            if (nibble > 0)
                canvas_->drawPixel(px, py, pixelColorForNibble(nibble, color));
        }
    }
}

// ── 16px small-text public API ───────────────────────────────────────────

int16_t CjkTextRenderer::textWidthSmall(const char* text) {
    if (!text) return 0;
    if (progmemUi16Ready_) {
        int16_t w = 0;
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(text);
        size_t pos = 0;
        const size_t len = strlen(text);
        while (pos < len) {
            uint32_t ch = decodeUtf8(bytes, pos, len);
            if (ch == '\n') break;
            GrayGlyph glyph;
            if (findProgmemUi16Glyph(ch, glyph))
                w += glyph.advance > 0 ? glyph.advance : (ch < 128 ? 8 : progmemUi16FontSize_);
            else
                w += ch < 128 ? 8 : progmemUi16FontSize_;
        }
        return w;
    }
    if (fontSmall_.isLoaded()) {
        int16_t w = 0;
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(text);
        size_t pos = 0;
        const size_t len = strlen(text);
        while (pos < len) {
            uint32_t ch = decodeUtf8(bytes, pos, len);
            if (ch == '\n') break;
            uint8_t adv = fontSmall_.getCharAdvance(ch);
            w += adv > 0 ? adv : 8;
        }
        return w;
    }
    return textWidth(text);
}

void CjkTextRenderer::drawTextSmall(int16_t x, int16_t y, const char* text, uint16_t color) {
    if (!text || !canvas_) return;

    if (progmemUi16Ready_) {
        // PROGMEM 16px font — same reliable path as the 24px drawText
        int16_t cx = x;
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(text);
        size_t pos = 0;
        const size_t len = strlen(text);
        while (pos < len && cx < kPaperS3Width) {
            uint32_t ch = decodeUtf8(bytes, pos, len);
            drawSmallGlyph(ch, cx, y, color);
            GrayGlyph glyph;
            if (findProgmemUi16Glyph(ch, glyph))
                cx += glyph.advance > 0 ? glyph.advance : (ch < 128 ? 8 : progmemUi16FontSize_);
            else
                cx += ch < 128 ? 8 : progmemUi16FontSize_;
        }
        return;
    }

    // Fallback: SPIFFS font
    if (fontSmall_.isLoaded()) {
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(text);
        size_t pos = 0;
        const size_t len = strlen(text);
        int16_t cx = x;
        while (pos < len) {
            uint32_t ch = decodeUtf8(bytes, pos, len);
            uint8_t w = 0, h = 0, adv = 0;
            int8_t bx = 0, by = 0;
            const uint8_t* bmp = fontSmall_.getCharBitmapGray(ch, w, h, bx, by, adv);
            if (bmp && w > 0 && h > 0) {
                const int16_t drawX = cx + bx;
                const int16_t drawY = y + (16 - h) / 2;
                for (int row = 0; row < h; row++) {
                    const int16_t py = drawY + row;
                    if (py < 0 || py >= kPaperS3Height) continue;
                    for (int col = 0; col < w; col++) {
                        const int16_t px = drawX + col;
                        if (px < 0 || px >= kPaperS3Width) continue;
                        const uint8_t packed = bmp[row * ((w + 1) / 2) + col / 2];
                        const uint8_t nibble = (col % 2 == 0) ? ((packed >> 4) & 0x0F) : (packed & 0x0F);
                        if (nibble > 0)
                            canvas_->drawPixel(px, py, pixelColorForNibble(nibble, color));
                    }
                }
                cx += adv > 0 ? adv : w;
            } else {
                cx += 8;
            }
        }
        return;
    }

    // No 16px font — use 24px PROGMEM
    drawText(x, y, text, color);
}

void CjkTextRenderer::drawCenteredSmall(int16_t x, int16_t y, int16_t w, int16_t h,
                                         const char* text, uint16_t color) {
    if (!text || !text[0]) return;
    const int16_t tw = textWidthSmall(text);
    const int16_t lx = x + (w - tw) / 2;
    const int16_t ly = smallLineTopForBox(y, h);
    drawTextSmall(lx > x + 6 ? lx : x + 6, ly, text, color);
}

} // namespace vink3
