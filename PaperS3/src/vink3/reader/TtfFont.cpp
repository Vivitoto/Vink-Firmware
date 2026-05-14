#include "TtfFont.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "../text/stb_truetype.h"

#include "../VinkPaperS3.h"
#include <SPI.h>
#include <cstring>

// ── Gamma-0.7 4bpp → RGB565 table (same as CjkTextRenderer / ReaderTextRenderer) ──
static const uint16_t k4BitToRgb565[16] = {
    0xFFFF, 0xDEDB, 0xC618, 0xAD75, 0x9CD3, 0x8C51, 0x7BCF, 0x6B4D,
    0x5ACB, 0x4A69, 0x39E7, 0x3186, 0x2124, 0x10A2, 0x0841, 0x0000
};

static uint16_t pixelColorForNibbleTtf(uint8_t nibble, uint16_t color) {
    if (color == TFT_WHITE) return TFT_WHITE;
    if (color != TFT_BLACK) return color;
    return k4BitToRgb565[nibble & 0x0F];
}

/// Local UTF-8→codepoint decoder (same logic as ReaderTextRenderer::decodeUtf8).
static uint32_t decodeUtf8(const uint8_t* buf, size_t& pos, size_t len) {
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

namespace vink3 {

TtfFont::TtfFont() {
    memset(cache_, 0, sizeof(cache_));
}

TtfFont::~TtfFont() {
    unload();
}

bool TtfFont::loadFromSd(const char* path) {
    unload();
    if (!path || !path[0]) return false;

    SPI.begin(kSdSckPin, kSdMisoPin, kSdMosiPin, kSdCsPin);
    if (SD.cardType() == CARD_NONE) {
        const uint32_t freqs[] = {kSdPrimaryFrequency, kSdFallbackFrequency1, kSdFallbackFrequency2};
        bool ok = false;
        for (uint32_t freq : freqs) {
            if (SD.begin(kSdCsPin, SPI, freq)) { ok = true; break; }
            delay(50);
        }
        if (!ok) {
            Serial.printf("[vink3][ttf] SD init failed for: %s\n", path);
            return false;
        }
    }

    File f = SD.open(path, FILE_READ);
    if (!f) {
        Serial.printf("[vink3][ttf] failed to open: %s\n", path);
        return false;
    }

    ttfDataSize_ = f.size();
    if (ttfDataSize_ < 256 || ttfDataSize_ > 4 * 1024 * 1024) {
        Serial.printf("[vink3][ttf] invalid size %zu for %s\n", ttfDataSize_, path);
        f.close();
        return false;
    }

    // Allocate in PSRAM (ESP32-S3 has 8MB, prefer it for large TTF files)
    ttfData_ = static_cast<uint8_t*>(heap_caps_malloc(ttfDataSize_, MALLOC_CAP_SPIRAM));
    if (!ttfData_) {
        ttfData_ = static_cast<uint8_t*>(heap_caps_malloc(ttfDataSize_, MALLOC_CAP_8BIT));
    }
    if (!ttfData_) {
        Serial.printf("[vink3][ttf] PSRAM alloc failed for %zu bytes\n", ttfDataSize_);
        f.close();
        return false;
    }

    const size_t read = f.read(ttfData_, ttfDataSize_);
    f.close();
    if (read != ttfDataSize_) {
        Serial.printf("[vink3][ttf] short read: %zu/%zu\n", read, ttfDataSize_);
        free(ttfData_);
        ttfData_ = nullptr;
        ttfDataSize_ = 0;
        return false;
    }

    // Validate with stbtt — TTF uses offset 0, OTF/TTC uses GetFontOffsetForIndex
    stbtt_fontinfo info;
    fontOffset_ = stbtt_GetFontOffsetForIndex(ttfData_, 0);
    if (fontOffset_ < 0) fontOffset_ = 0;
    if (!stbtt_InitFont(&info, ttfData_, fontOffset_)) {
        // Try offset 0 as plain TTF
        fontOffset_ = 0;
        if (!stbtt_InitFont(&info, ttfData_, 0)) {
            Serial.printf("[vink3][ttf] stbtt_InitFont failed for %s\n", path);
            free(ttfData_);
            ttfData_ = nullptr;
            ttfDataSize_ = 0;
            return false;
        }
    }

    strncpy(fontPath_, path, sizeof(fontPath_) - 1);
    fontPath_[sizeof(fontPath_) - 1] = '\0';
    loaded_ = true;
    cacheAge_ = 0;
    memset(cache_, 0, sizeof(cache_));
    recalcMetrics();

    // Extract a short display name
    const char* fname = strrchr(path, '/');
    fname = fname ? fname + 1 : path;
    Serial.printf("[vink3][ttf] loaded: %s (%zu bytes) size=%upx asc=%d desc=%d lh=%d\n",
                  fname, ttfDataSize_, pxSize_, ascender_, descender_, lineHeight_);
    return true;
}

void TtfFont::unload() {
    for (int i = 0; i < kCacheSlots; ++i) {
        if (cache_[i].bitmap) {
            free(cache_[i].bitmap);
            cache_[i].bitmap = nullptr;
        }
    }
    if (ttfData_) {
        free(ttfData_);
        ttfData_ = nullptr;
    }
    ttfDataSize_ = 0;
    fontPath_[0] = '\0';
    loaded_ = false;
    cacheAge_ = 0;
}

void TtfFont::setSize(uint8_t pxSize) {
    if (pxSize < 16) pxSize = 16;
    if (pxSize > 64) pxSize = 64;
    if (pxSize == pxSize_) return;
    pxSize_ = pxSize;
    // Clear glyph cache since metrics changed
    for (int i = 0; i < kCacheSlots; ++i) {
        if (cache_[i].bitmap) {
            free(cache_[i].bitmap);
            cache_[i].bitmap = nullptr;
        }
        cache_[i].unicode = 0;
        cache_[i].useOrder = 0;
    }
    cacheAge_ = 0;
    recalcMetrics();
}

void TtfFont::recalcMetrics() {
    if (!loaded_ || !ttfData_) return;
    stbtt_fontinfo info;
    if (!stbtt_InitFont(&info, ttfData_, fontOffset_)) return;
    scale_ = stbtt_ScaleForPixelHeight(&info, pxSize_);
    int asc, desc, lg;
    stbtt_GetFontVMetrics(&info, &asc, &desc, &lg);
    ascender_  = static_cast<int16_t>(asc * scale_);
    descender_ = static_cast<int16_t>(desc * scale_);
    lineHeight_ = static_cast<int16_t>((asc - desc) * scale_);
}

bool TtfFont::hasGlyph(uint32_t unicode) const {
    if (!loaded_ || !ttfData_) return false;
    if (unicode == '\n' || unicode == '\r' || unicode == ' ' || unicode == '\t') return true;
    stbtt_fontinfo info;
    if (!stbtt_InitFont(&info, ttfData_, fontOffset_)) return false;
    return stbtt_FindGlyphIndex(&info, static_cast<int>(unicode)) != 0;
}

int16_t TtfFont::charAdvance(uint32_t unicode) const {
    if (!loaded_ || !ttfData_) return pxSize_;
    stbtt_fontinfo info;
    if (!stbtt_InitFont(&info, ttfData_, fontOffset_)) return pxSize_;
    int adv, lsb;
    stbtt_GetCodepointHMetrics(&info, static_cast<int>(unicode), &adv, &lsb);
    int16_t a = static_cast<int16_t>(adv * scale_);
    return a > 0 ? a : static_cast<int16_t>(pxSize_ / 2);
}

int16_t TtfFont::textWidth(const char* text) const {
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

int TtfFont::findCachedGlyph(uint32_t unicode) const {
    for (int i = 0; i < kCacheSlots; ++i) {
        if (cache_[i].unicode == unicode && cache_[i].bitmap) return i;
    }
    return -1;
}

int TtfFont::insertCacheSlot(uint32_t unicode, uint8_t w, uint8_t h,
                              int8_t bx, int8_t by, uint8_t adv) {
    // Find the least-recently-used slot
    int lruIdx = 0;
    uint8_t lruOrder = cache_[0].useOrder;
    for (int i = 1; i < kCacheSlots; ++i) {
        if (cache_[i].useOrder < lruOrder) {
            lruOrder = cache_[i].useOrder;
            lruIdx = i;
        }
    }

    // Free old bitmap
    if (cache_[lruIdx].bitmap) {
        free(cache_[lruIdx].bitmap);
        cache_[lruIdx].bitmap = nullptr;
    }

    // Allocate new bitmap
    const size_t bmpSize = static_cast<size_t>(w) * h;
    cache_[lruIdx].bitmap = static_cast<uint8_t*>(heap_caps_malloc(bmpSize, MALLOC_CAP_SPIRAM));
    if (!cache_[lruIdx].bitmap) {
        cache_[lruIdx].bitmap = static_cast<uint8_t*>(malloc(bmpSize));
    }
    if (!cache_[lruIdx].bitmap) {
        cache_[lruIdx].unicode = 0;
        return -1;
    }

    cache_[lruIdx].unicode = unicode;
    cache_[lruIdx].width = w;
    cache_[lruIdx].height = h;
    cache_[lruIdx].bearingX = bx;
    cache_[lruIdx].bearingY = by;
    cache_[lruIdx].advance = adv;
    cache_[lruIdx].useOrder = ++cacheAge_;
    return lruIdx;
}

bool TtfFont::drawGlyphToBuffer(uint32_t unicode, uint8_t* buf,
                                 int* w, int* h, int* advance) {
    if (!loaded_ || !ttfData_) return false;

    // Check LRU cache
    int ci = findCachedGlyph(unicode);
    if (ci >= 0) {
        cache_[ci].useOrder = ++cacheAge_;
        *w = cache_[ci].width;
        *h = cache_[ci].height;
        *advance = cache_[ci].advance;
        memcpy(buf, cache_[ci].bitmap, static_cast<size_t>(*w) * (*h));
        return true;
    }

    // Render via stb_truetype
    stbtt_fontinfo info;
    if (!stbtt_InitFont(&info, ttfData_, fontOffset_)) return false;

    int cw = 0, ch = 0, xoff = 0, yoff = 0;
    unsigned char* rendered = stbtt_GetCodepointBitmap(
        &info, scale_, scale_, static_cast<int>(unicode),
        &cw, &ch, &xoff, &yoff);

    if (!rendered || cw <= 0 || ch <= 0) {
        if (rendered) stbtt_FreeBitmap(rendered, nullptr);
        *w = 0; *h = 0; *advance = 0;
        return false;
    }
    // drawGlyph() uses a fixed 64×64 scratch buffer. Reject oversized glyphs
    // before any memcpy into the caller buffer or cache.
    if (cw > 64 || ch > 64) {
        stbtt_FreeBitmap(rendered, nullptr);
        *w = 0; *h = 0; *advance = 0;
        return false;
    }

    int adv, lsb;
    stbtt_GetCodepointHMetrics(&info, static_cast<int>(unicode), &adv, &lsb);
    int16_t fa = static_cast<int16_t>(adv * scale_);
    if (fa < 1) fa = static_cast<int16_t>(pxSize_ / 2);

    // Clamp glyph dimensions for cache (shouldn't normally exceed 255)
    uint8_t cw8 = static_cast<uint8_t>(cw > 255 ? 255 : cw);
    uint8_t ch8 = static_cast<uint8_t>(ch > 255 ? 255 : ch);
    uint8_t adv8 = static_cast<uint8_t>(fa > 255 ? 255 : fa);

    // Store in cache
    const int slot = insertCacheSlot(unicode, cw8, ch8,
                                     static_cast<int8_t>(xoff), static_cast<int8_t>(yoff), adv8);
    const size_t bmpSize = static_cast<size_t>(cw) * ch;
    if (slot >= 0 && cache_[slot].bitmap) {
        memcpy(cache_[slot].bitmap, rendered, bmpSize);
    }

    // Copy to caller's buffer
    memcpy(buf, rendered, bmpSize);
    stbtt_FreeBitmap(rendered, nullptr);

    *w = cw;
    *h = ch;
    *advance = fa;
    return true;
}

bool TtfFont::drawGlyph(uint32_t unicode, int16_t x, int16_t y,
                         uint16_t color, M5Canvas* canvas) {
    if (!loaded_ || !ttfData_ || !canvas) return false;

    // Allocate on heap for safety; max reasonable glyph at 64px = ~4KB
    constexpr int kMaxGlyphPixels = 64 * 64;
    uint8_t* bmpBuf = static_cast<uint8_t*>(malloc(kMaxGlyphPixels));
    if (!bmpBuf) return false;

    int gw = 0, gh = 0, adv = 0;
    if (!drawGlyphToBuffer(unicode, bmpBuf, &gw, &gh, &adv)) {
        free(bmpBuf);
        return false;
    }
    if (gw <= 0 || gh <= 0 || gw > 64 || gh > 64) {
        free(bmpBuf);
        return false;
    }

    // Look up cached entry for bearing info
    int ci = findCachedGlyph(unicode);
    int16_t bx = 0, by = 0;
    if (ci >= 0) {
        bx = cache_[ci].bearingX;
        by = cache_[ci].bearingY;
    }

    // y is the line-top (same convention as Wenkai32 drawGlyph).
    // stb_truetype: xoff/yoff are from the current pen position (baseline).
    // Our drawing origin: y is line-top, baseline = y + ascender_.
    // Bitmap top-left = (x + bx, (y + ascender_) + by)
    const int16_t drawX = x + bx;
    const int16_t drawY = y + ascender_ + by;

    // Quantize 8bpp → 4bpp and draw
    for (int row = 0; row < gh; row++) {
        const int16_t py = drawY + row;
        if (py < 0 || py >= kPaperS3Height) continue;
        for (int col = 0; col < gw; col++) {
            const int16_t px = drawX + col;
            if (px < 0 || px >= kPaperS3Width) continue;
            const uint8_t val8 = bmpBuf[row * gw + col];
            if (val8 == 0) continue;
            // Quantize 0-255 → 0-15 (round to nearest)
            const uint8_t nibble = (val8 + 8) >> 4;
            if (nibble > 0) {
                canvas->drawPixel(px, py, pixelColorForNibbleTtf(nibble, color));
            }
        }
    }

    free(bmpBuf);
    return true;
}

int TtfFont::scanSdFonts(char paths[][64], int maxCount) {
    if (maxCount <= 0) return 0;
    int count = 0;

    // User-triggered path only: initialize SD lazily here too, so font source
    // switching works even before the library/shelf has touched the card.
    SPI.begin(kSdSckPin, kSdMisoPin, kSdMosiPin, kSdCsPin);
    if (SD.cardType() == CARD_NONE) {
        const uint32_t freqs[] = {kSdPrimaryFrequency, kSdFallbackFrequency1, kSdFallbackFrequency2};
        bool ok = false;
        for (uint32_t freq : freqs) {
            if (SD.begin(kSdCsPin, SPI, freq)) { ok = true; break; }
            delay(50);
        }
        if (!ok) return 0;
    }

    // Try opening the /fonts directory first
    File root = SD.open("/fonts");
    if (!root || !root.isDirectory()) {
        // Fallback: scan SD root
        root.close();
        root = SD.open("/");
        if (!root) return 0;
    }

    while (count < maxCount) {
        File entry = root.openNextFile();
        if (!entry) break;
        if (entry.isDirectory()) {
            entry.close();
            continue;
        }

        const char* name = entry.name();
        entry.close();
        if (!name) continue;
        const char* dot = strrchr(name, '.');
        if (!dot) continue;

        if (strcasecmp(dot, ".ttf") == 0 || strcasecmp(dot, ".otf") == 0) {
            // Build full path
            char full[64];
            const char* dirPath = root.path();
            if (dirPath && dirPath[0] == '/' && dirPath[1]) {
                snprintf(full, sizeof(full), "%s/%s", dirPath, name);
            } else {
                snprintf(full, sizeof(full), "/%s", name);
            }
            full[sizeof(full) - 1] = '\0';
            strncpy(paths[count], full, 63);
            paths[count][63] = '\0';
            count++;
        }
    }
    root.close();
    return count;
}

} // namespace vink3
