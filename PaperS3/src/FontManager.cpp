#include "FontManager.h"
#include "BuiltinFont.h"
#include <esp_heap_caps.h>
#include <pgmspace.h>
#include <string.h>
#include <SPIFFS.h>

static bool ensureFontResourceFS() {
    static bool tried = false;
    static bool ok = false;
    if (!tried) {
        tried = true;
        ok = SPIFFS.begin(false);
        Serial.printf("[FS] SPIFFS %s\n", ok ? "mounted" : "mount failed");
    }
    return ok;
}

FontManager::FontManager() : 
    _fontType(FontType::BITMAP_1BPP),
    _builtinLoaded(false),
    _builtinBitmapData(nullptr),
    _index_1bpp(nullptr), 
    _index_gray(nullptr),
    _bitmapBuffer(nullptr) {
    memset(&_header_1bpp, 0, sizeof(_header_1bpp));
    memset(&_header_gray, 0, sizeof(_header_gray));
}

FontManager::~FontManager() {
    unload();
}

bool FontManager::loadFont(const char* path) {
    unload();
    
    strncpy(_currentPath, path, sizeof(_currentPath) - 1);
    _currentPath[sizeof(_currentPath) - 1] = '\0';
    
    _file = SD.open(path, FILE_READ);
    if (!_file && ensureFontResourceFS()) {
        _file = SPIFFS.open(path, FILE_READ);
        if (_file) {
            Serial.printf("[Font] Loaded from SPIFFS: %s\n", path);
        }
    }
    if (!_file) {
        Serial.printf("[Font] Failed to open: %s, trying builtin\n", path);
        return loadBuiltinFont();
    }
    
    // 读取前4字节判断格式
    char magic[4];
    if (_file.read((uint8_t*)magic, 4) != 4) {
        Serial.println("[Font] Failed to read magic, trying builtin");
        _file.close();
        return loadBuiltinFont();
    }
    
    // 回退到文件开头
    _file.seek(0);
    
    if (strncmp(magic, "GRAY", 4) == 0) {
        if (loadGrayFont()) return true;
        Serial.println("[Font] SD gray font failed, trying builtin");
        return loadBuiltinFont();
    } else if (strncmp(magic, "FNT", 3) == 0) {
        if (loadBitmapFont()) return true;
        Serial.println("[Font] SD bitmap font failed, trying builtin");
        return loadBuiltinFont();
    } else {
        Serial.printf("[Font] Unknown format: %.4s, trying builtin\n", magic);
        _file.close();
        return loadBuiltinFont();
    }
}

bool FontManager::loadBundledFont(const char* path) {
    unload();

    if (!ensureFontResourceFS()) {
        Serial.println("[Font] SPIFFS unavailable for bundled font, trying builtin");
        return loadBuiltinFont();
    }

    _file = SPIFFS.open(path, FILE_READ);
    if (!_file) {
        Serial.printf("[Font] Bundled font missing: %s, trying builtin\n", path);
        return loadBuiltinFont();
    }

    strncpy(_currentPath, path, sizeof(_currentPath) - 1);
    _currentPath[sizeof(_currentPath) - 1] = '\0';

    char magic[4];
    if (_file.read((uint8_t*)magic, 4) != 4) {
        Serial.println("[Font] Failed to read bundled font magic, trying builtin");
        _file.close();
        return loadBuiltinFont();
    }
    _file.seek(0);

    bool ok = false;
    if (strncmp(magic, "GRAY", 4) == 0) {
        ok = loadGrayFont();
    } else if (strncmp(magic, "FNT", 3) == 0) {
        ok = loadBitmapFont();
    } else {
        Serial.printf("[Font] Unknown bundled font format: %.4s\n", magic);
        _file.close();
    }

    if (ok) {
        Serial.printf("[Font] Loaded bundled UI font from SPIFFS: %s\n", path);
        return true;
    }

    Serial.println("[Font] Bundled font failed, trying builtin");
    return loadBuiltinFont();
}

bool FontManager::loadBuiltinFont() {
    unload();

    _fontType = FontType::BITMAP_1BPP;
    _builtinLoaded = true;
    _builtinBitmapData = BUILTIN_FONT_BITMAPS;
    strncpy(_currentPath, "builtin://16px", sizeof(_currentPath) - 1);
    _currentPath[sizeof(_currentPath) - 1] = '\0';

    memcpy_P(&_header_1bpp, &BUILTIN_FONT_HEADER, sizeof(_header_1bpp));
    if (strncmp(_header_1bpp.magic, "BINT", 4) != 0) {
        Serial.println("[Font] Invalid builtin font magic");
        unload();
        return false;
    }

    size_t indexSize = _header_1bpp.charCount * sizeof(CharIndex_1bpp);
    // Built-in fallback is small and is used by Shell/UI; keep its index in
    // internal RAM first so full-screen PSRAM canvas churn cannot corrupt UI text.
    _index_1bpp = (CharIndex_1bpp*)heap_caps_malloc(indexSize, MALLOC_CAP_INTERNAL);
    if (!_index_1bpp) {
        _index_1bpp = (CharIndex_1bpp*)heap_caps_malloc(indexSize, MALLOC_CAP_SPIRAM);
    }
    if (!_index_1bpp) {
        Serial.println("[Font] Failed to alloc builtin index");
        unload();
        return false;
    }
    memcpy_P(_index_1bpp, BUILTIN_FONT_INDEX, indexSize);

    _bitmapBuffer = (uint8_t*)heap_caps_malloc(512, MALLOC_CAP_INTERNAL);
    if (!_bitmapBuffer) {
        _bitmapBuffer = (uint8_t*)heap_caps_malloc(512, MALLOC_CAP_SPIRAM);
    }
    if (!_bitmapBuffer) {
        Serial.println("[Font] Failed to alloc builtin bitmap buffer");
        unload();
        return false;
    }

    Serial.printf("[Font] Loaded builtin 1bpp: size=%d, chars=%d, bitmap=%d bytes\n",
                  _header_1bpp.fontSize, _header_1bpp.charCount, BUILTIN_FONT_BITMAP_SIZE);
    return true;
}

bool FontManager::loadBitmapFont() {
    _fontType = FontType::BITMAP_1BPP;
    
    // 读取文件头
    if (_file.read((uint8_t*)&_header_1bpp, sizeof(_header_1bpp)) != sizeof(_header_1bpp)) {
        Serial.println("[Font] Failed to read 1bpp header");
        _file.close();
        return false;
    }
    
    if (strncmp(_header_1bpp.magic, "FNT", 3) != 0 || _header_1bpp.version != 1 ||
        _header_1bpp.charCount == 0 || _header_1bpp.charCount > 20000) {
        Serial.println("[Font] Invalid 1bpp header");
        _file.close();
        return false;
    }

    // 在 PSRAM 中分配索引表
    size_t indexSize = _header_1bpp.charCount * sizeof(CharIndex_1bpp);
    size_t fileSize = _file.size();
    size_t dataStart = sizeof(FontHeader_1bpp) + indexSize;
    if (dataStart >= fileSize) {
        Serial.println("[Font] Invalid 1bpp file size");
        _file.close();
        return false;
    }
    _index_1bpp = (CharIndex_1bpp*)heap_caps_malloc(indexSize, MALLOC_CAP_SPIRAM);
    if (!_index_1bpp) {
        Serial.println("[Font] Failed to alloc 1bpp index in PSRAM");
        _file.close();
        return false;
    }
    
    // 读取索引表
    if (_file.read((uint8_t*)_index_1bpp, indexSize) != indexSize) {
        Serial.println("[Font] Failed to read 1bpp index");
        unload();
        return false;
    }
    
    for (uint32_t i = 0; i < _header_1bpp.charCount; ++i) {
        const CharIndex_1bpp& ci = _index_1bpp[i];
        size_t bitmapSize = ((ci.width + 7) / 8) * ci.height;
        if (ci.width == 0 || ci.height == 0 || ci.width > 80 || ci.height > 80 ||
            bitmapSize == 0 || bitmapSize > 512 || ci.offset < dataStart ||
            (size_t)ci.offset + bitmapSize > fileSize) {
            Serial.printf("[Font] Invalid 1bpp glyph index at %u\n", (unsigned)i);
            unload();
            return false;
        }
    }

    // 分配临时点阵缓冲区（最大支持 64x64 = 512 bytes for 1bpp）
    _bitmapBuffer = (uint8_t*)heap_caps_malloc(512, MALLOC_CAP_SPIRAM);
    if (!_bitmapBuffer) {
        Serial.println("[Font] Failed to alloc bitmap buffer");
        unload();
        return false;
    }
    
    Serial.printf("[Font] Loaded 1bpp: size=%d, chars=%d\n", 
                  _header_1bpp.fontSize, _header_1bpp.charCount);
    return true;
}

bool FontManager::loadGrayFont() {
    _fontType = FontType::GRAY_4BPP;
    
    // 读取文件头
    if (_file.read((uint8_t*)&_header_gray, sizeof(_header_gray)) != sizeof(_header_gray)) {
        Serial.println("[Font] Failed to read gray header");
        _file.close();
        return false;
    }
    
    if (strncmp(_header_gray.magic, "GRAY", 4) != 0 || _header_gray.version != 1 ||
        _header_gray.charCount == 0 || _header_gray.charCount > 20000 ||
        _header_gray.bitmapSize == 0) {
        Serial.println("[Font] Invalid gray header");
        _file.close();
        return false;
    }

    // 在 PSRAM 中分配索引表
    size_t indexSize = _header_gray.charCount * sizeof(CharIndex_gray);
    size_t fileSize = _file.size();
    size_t dataStart = sizeof(FontHeader_gray) + indexSize;
    if (dataStart >= fileSize || dataStart + _header_gray.bitmapSize != fileSize) {
        Serial.println("[Font] Invalid gray file size");
        _file.close();
        return false;
    }
    _index_gray = (CharIndex_gray*)heap_caps_malloc(indexSize, MALLOC_CAP_SPIRAM);
    if (!_index_gray) {
        Serial.println("[Font] Failed to alloc gray index in PSRAM");
        _file.close();
        return false;
    }
    
    // 读取索引表
    if (_file.read((uint8_t*)_index_gray, indexSize) != indexSize) {
        Serial.println("[Font] Failed to read gray index");
        unload();
        return false;
    }
    
    for (uint32_t i = 0; i < _header_gray.charCount; ++i) {
        const CharIndex_gray& ci = _index_gray[i];
        size_t rowBytes = (ci.width + 1) / 2;
        size_t bitmapSize = rowBytes * ci.height;
        if (ci.width == 0 || ci.height == 0 || ci.width > 96 || ci.height > 96 ||
            ci.advance > 96 || bitmapSize == 0 || bitmapSize > 2048 ||
            ci.offset < dataStart || (size_t)ci.offset + bitmapSize > fileSize) {
            Serial.printf("[Font] Invalid gray glyph index at %u\n", (unsigned)i);
            unload();
            return false;
        }
    }

    // 分配临时点阵缓冲区（最大支持 64x64 4bpp = 2048 bytes）
    _bitmapBuffer = (uint8_t*)heap_caps_malloc(2048, MALLOC_CAP_SPIRAM);
    if (!_bitmapBuffer) {
        Serial.println("[Font] Failed to alloc bitmap buffer");
        unload();
        return false;
    }
    
    Serial.printf("[Font] Loaded gray: size=%d, chars=%d\n", 
                  _header_gray.fontSize, _header_gray.charCount);
    return true;
}

void FontManager::unload() {
    _builtinLoaded = false;
    _builtinBitmapData = nullptr;
    if (_index_1bpp) {
        heap_caps_free(_index_1bpp);
        _index_1bpp = nullptr;
    }
    if (_index_gray) {
        heap_caps_free(_index_gray);
        _index_gray = nullptr;
    }
    if (_bitmapBuffer) {
        heap_caps_free(_bitmapBuffer);
        _bitmapBuffer = nullptr;
    }
    if (_file) {
        _file.close();
    }
    memset(&_header_1bpp, 0, sizeof(_header_1bpp));
    memset(&_header_gray, 0, sizeof(_header_gray));
    memset(_currentPath, 0, sizeof(_currentPath));
}

int FontManager::scanFonts(char paths[][128], char names[][64], int maxCount) {
    int count = 0;
    auto scanFs = [&](fs::FS& fs, const char* prefix) {
        if (count >= maxCount) return;
        File root = fs.open(FONTS_DIR);
        if (!root || !root.isDirectory()) {
            if (root) root.close();
            return;
        }
        File file = root.openNextFile();
        while (file && count < maxCount) {
            String name = file.name();
            int slash = name.lastIndexOf('/');
            if (slash >= 0) name = name.substring(slash + 1);
            if (name.endsWith(".fnt")) {
                String fullPath = String(FONTS_DIR) + "/" + name;
                strncpy(paths[count], fullPath.c_str(), 127);
                paths[count][127] = '\0';

                String displayName = String(prefix) + name;
                displayName.replace(".fnt", "");
                strncpy(names[count], displayName.c_str(), 63);
                names[count][63] = '\0';
                count++;
            }
            file.close();
            file = root.openNextFile();
        }
        root.close();
    };

    scanFs(SD, "SD ");
    if (ensureFontResourceFS()) scanFs(SPIFFS, "内置 ");
    return count;
}

bool FontManager::isLoaded() const {
    if (_builtinLoaded) {
        return _index_1bpp != nullptr && _builtinBitmapData != nullptr;
    }
    if (_fontType == FontType::GRAY_4BPP) {
        return _index_gray != nullptr && _file;
    }
    return _index_1bpp != nullptr && _file;
}

uint16_t FontManager::getFontSize() const {
    if (_fontType == FontType::GRAY_4BPP) {
        return _header_gray.fontSize;
    }
    return _header_1bpp.fontSize;
}

int FontManager::findCharIndex(uint32_t unicode) {
    if (_fontType == FontType::GRAY_4BPP) {
        if (!_index_gray || _header_gray.charCount == 0) return -1;
        
        int left = 0, right = _header_gray.charCount - 1;
        while (left <= right) {
            int mid = left + (right - left) / 2;
            if (_index_gray[mid].unicode == unicode) return mid;
            if (_index_gray[mid].unicode < unicode) left = mid + 1;
            else right = mid - 1;
        }
    } else {
        if (!_index_1bpp || _header_1bpp.charCount == 0) return -1;
        
        int left = 0, right = _header_1bpp.charCount - 1;
        while (left <= right) {
            int mid = left + (right - left) / 2;
            if (_index_1bpp[mid].unicode == unicode) return mid;
            if (_index_1bpp[mid].unicode < unicode) left = mid + 1;
            else right = mid - 1;
        }
    }
    return -1;
}

const uint8_t* FontManager::getCharBitmap(uint32_t unicode, uint8_t& outWidth, uint8_t& outHeight) {
    if (_fontType != FontType::BITMAP_1BPP) {
        outWidth = 0;
        outHeight = 0;
        return nullptr;
    }
    
    int idx = findCharIndex(unicode);
    if (idx < 0) {
        outWidth = 0;
        outHeight = 0;
        return nullptr;
    }
    
    const CharIndex_1bpp& ci = _index_1bpp[idx];
    outWidth = ci.width;
    outHeight = ci.height;
    
    if (ci.width == 0 || ci.height == 0 || ci.width > 80 || ci.height > 80) {
        outWidth = 0; outHeight = 0; return nullptr;
    }
    size_t bitmapSize = ((ci.width + 7) / 8) * ci.height;
    if (bitmapSize == 0 || bitmapSize > 512) {
        outWidth = 0; outHeight = 0; return nullptr;
    }
    
    if (_builtinLoaded) {
        memcpy_P(_bitmapBuffer, _builtinBitmapData + ci.offset, bitmapSize);
    } else {
        if (!_file.seek(ci.offset) || _file.read(_bitmapBuffer, bitmapSize) != bitmapSize) {
            outWidth = 0; outHeight = 0; return nullptr;
        }
    }
    
    return _bitmapBuffer;
}

const uint8_t* FontManager::getCharBitmapGray(uint32_t unicode, uint8_t& outWidth, uint8_t& outHeight,
                                              int8_t& outBearingX, int8_t& outBearingY, uint8_t& outAdvance) {
    if (_fontType != FontType::GRAY_4BPP) {
        outWidth = 0;
        outHeight = 0;
        outBearingX = outBearingY = outAdvance = 0;
        return nullptr;
    }
    
    int idx = findCharIndex(unicode);
    if (idx < 0) {
        outWidth = 0;
        outHeight = 0;
        outBearingX = outBearingY = outAdvance = 0;
        return nullptr;
    }
    
    const CharIndex_gray& ci = _index_gray[idx];
    outWidth = ci.width;
    outHeight = ci.height;
    outBearingX = ci.bearingX;
    outBearingY = ci.bearingY;
    outAdvance = ci.advance;
    
    if (ci.width == 0 || ci.height == 0 || ci.width > 96 || ci.height > 96 || ci.advance > 96) {
        outWidth = 0; outHeight = 0; outBearingX = outBearingY = outAdvance = 0; return nullptr;
    }
    // 4bpp: 每行字节数 = (width + 1) / 2
    size_t rowBytes = (ci.width + 1) / 2;
    size_t bitmapSize = rowBytes * ci.height;
    if (bitmapSize == 0 || bitmapSize > 2048) {
        outWidth = 0; outHeight = 0; outBearingX = outBearingY = outAdvance = 0; return nullptr;
    }
    
    if (!_file.seek(ci.offset) || _file.read(_bitmapBuffer, bitmapSize) != bitmapSize) {
        outWidth = 0; outHeight = 0; outBearingX = outBearingY = outAdvance = 0; return nullptr;
    }
    
    return _bitmapBuffer;
}

uint8_t FontManager::getCharWidth(uint32_t unicode) {
    int idx = findCharIndex(unicode);

    // ASCII used to be forced to fontSize/2. That made wide English glyphs such
    // as W/M/Vink-PaperS3 overlap visually because drawUiGlyph() rendered the
    // actual bitmap but the cursor advanced by only a fixed half-width.
    if (unicode < 128) {
        if (unicode == ' ' || unicode == '\t') return getFontSize() / 2;
        if (idx >= 0) {
            if (_fontType == FontType::GRAY_4BPP) return _index_gray[idx].width;
            return _index_1bpp[idx].width;
        }
        return getFontSize() / 2;
    }
    
    if (idx < 0) return getFontSize();  // 字符不存在，返回默认宽度
    
    if (_fontType == FontType::GRAY_4BPP) {
        return _index_gray[idx].width;
    }
    return _index_1bpp[idx].width;
}

uint8_t FontManager::getCharAdvance(uint32_t unicode) {
    int idx = findCharIndex(unicode);

    if (unicode < 128) {
        if (unicode == ' ' || unicode == '\t') return getFontSize() / 2;
        if (idx >= 0) {
            uint8_t adv = (_fontType == FontType::GRAY_4BPP) ? _index_gray[idx].advance : _index_1bpp[idx].width;
            return min<uint8_t>(255, adv + 1);  // light UI tracking for Latin text
        }
        return getFontSize() / 2 + 1;
    }
    
    if (idx < 0) return getFontSize();
    
    if (_fontType == FontType::GRAY_4BPP) {
        return _index_gray[idx].advance;
    }
    return _index_1bpp[idx].width;
}

void FontManager::unpackGray4To8(const uint8_t* src, uint8_t* dst, int width, int height) {
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int srcIdx = y * ((width + 1) / 2) + x / 2;
            uint8_t nibble;
            if (x % 2 == 0) {
                nibble = (src[srcIdx] >> 4) & 0x0F;
            } else {
                nibble = src[srcIdx] & 0x0F;
            }
            // 4bpp (0-15) 扩展到 8bpp (0-255)
            dst[y * width + x] = nibble * 17;
        }
    }
}
