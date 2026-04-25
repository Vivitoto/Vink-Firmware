#include "EbookReader.h"
#include "JsonHelper.h"
#include "TextCodec.h"
#include <M5Unified.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <strings.h>

EbookReader::EbookReader(FontManager& font) 
    : _font(font), _pages(nullptr), _currentPage(0), _totalPages(0), 
      _fileSize(0), _preloadBuffer(nullptr), _preloadSize(0), 
      _preloadPage(0), _hasPreload(false), _pageTurnCount(0), 
      _needsFullRefresh(true), _chapters(nullptr), _chapterCount(0),
      _chapterCapacity(0), _chaptersDetected(false),
      _bookmarks(nullptr), _bookmarkCount(0), _bookmarkCapacity(0),
      _isEpub(false), _epubChapter(0) {
    memset(_bookPath, 0, sizeof(_bookPath));
    memset(_bookName, 0, sizeof(_bookName));
    memset(_epubTempPath, 0, sizeof(_epubTempPath));
    _layout = LayoutConfig::Default();
    _refreshStrategy = RefreshStrategy::FromFrequency(RefreshFrequency::FREQ_MEDIUM);
}

EbookReader::~EbookReader() {
    closeBook();
    if (_preloadBuffer) {
        heap_caps_free(_preloadBuffer);
        _preloadBuffer = nullptr;
    }
}

bool EbookReader::openBook(const char* path) {
    closeBook();
    
    strncpy(_bookPath, path, sizeof(_bookPath) - 1);  // 原始路径用于进度/缓存命名
    _bookPath[sizeof(_bookPath) - 1] = '\0';
    const char* name = strrchr(path, '/');
    if (name) strncpy(_bookName, name + 1, sizeof(_bookName) - 1);
    else strncpy(_bookName, path, sizeof(_bookName) - 1);
    _bookName[sizeof(_bookName) - 1] = '\0';
    
    const char* ext = strrchr(path, '.');
    bool isEpub = ext && strcasecmp(ext, ".epub") == 0;
    if (isEpub) {
        if (!_epub.open(path)) {
            Serial.printf("[Reader] Failed to open EPUB: %s\n", path);
            _epub.close();
            return false;
        }
        _isEpub = true;
        _epubChapter = 0;
        loadLayoutConfig();
        if (!loadEpubChapter(0)) {
            closeBook();
            return false;
        }
        if (!_preloadBuffer) {
            _preloadBuffer = (char*)heap_caps_malloc(PRELOAD_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
        }
        _hasPreload = false;
        _pageTurnCount = 0;
        _needsFullRefresh = true;
        
        if (!loadProgress()) {
            _currentPage = 0;
        }
        loadBookmarks();
        return true;
    }
    
    // 先尝试打开原始文件检测编码
    File detectFile = SD.open(path, FILE_READ);
    if (!detectFile) {
        Serial.printf("[Reader] Failed to open: %s\n", path);
        return false;
    }
    
    // 编码检测
    TextEncoding encoding = TextCodec::detect(detectFile);
    detectFile.close();
    
    const char* openPath = path;
    _tempFilePath = "";
    
    if (encoding == TextEncoding::GBK) {
        Serial.println("[Reader] GBK detected, converting...");
        _tempFilePath = TextCodec::convertToUTF8(path);
        if (_tempFilePath.length() > 0) {
            openPath = _tempFilePath.c_str();
            Serial.printf("[Reader] Using temp file: %s\n", openPath);
        } else {
            Serial.println("[Reader] GBK conversion failed, using original");
        }
    }
    
    _file = SD.open(openPath, FILE_READ);
    if (!_file) {
        Serial.printf("[Reader] Failed to open: %s\n", openPath);
        return false;
    }
    
    _fileSize = _file.size();
    Serial.printf("[Reader] Opened: %s (%d bytes, encoding=%s)\n",
                  _bookName, _fileSize,
                  encoding == TextEncoding::UTF8 ? "UTF-8" :
                  encoding == TextEncoding::GBK ? "GBK" : "Unknown");
    
    loadLayoutConfig();
    
    if (!_preloadBuffer) {
        _preloadBuffer = (char*)heap_caps_malloc(PRELOAD_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
    }
    _hasPreload = false;
    _pageTurnCount = 0;
    _needsFullRefresh = true;
    
    if (!buildPages()) {
        closeBook();
        return false;
    }
    
    if (!loadProgress()) {
        _currentPage = 0;
    }
    
    loadBookmarks();
    
    return true;
}

void EbookReader::closeBook() {
    saveProgress();
    saveLayoutConfig();
    saveChaptersToCache();
    saveBookmarks();
    if (_file) _file.close();
    clearPages();
    clearChapters();
    clearBookmarks();
    
    if (_isEpub) {
        _epub.close();
        if (_epubTempPath[0] != '\0') {
            SD.remove(_epubTempPath);
            memset(_epubTempPath, 0, sizeof(_epubTempPath));
        }
        _isEpub = false;
        _epubChapter = 0;
    }
    
    // 清理 GBK 临时文件
    if (_tempFilePath.length() > 0) {
        TextCodec::cleanupTempFile(_tempFilePath.c_str());
        _tempFilePath = "";
    }
    
    _currentPage = 0;
    _totalPages = 0;
    _fileSize = 0;
    _hasPreload = false;
    _preloadSize = 0;
    _preloadPage = 0;
    _bookmarkCount = 0;
    _bookmarkCapacity = 0;
    memset(_bookPath, 0, sizeof(_bookPath));
    memset(_bookName, 0, sizeof(_bookName));
}

bool EbookReader::isOpen() const {
    return _file && _totalPages > 0;
}

void EbookReader::setLayoutConfig(const LayoutConfig& config) {
    uint32_t currentCharOffset = _pages ? _pages[_currentPage].startOffset : 0;
    _layout = config;
    if (isOpen()) {
        buildPages();
        gotoCharOffset(currentCharOffset);
        renderPage();
    }
}

void EbookReader::setFontSize(uint8_t size) {
    if (size < 12) size = 12;
    if (size > 48) size = 48;
    _layout.fontSize = size;
    if (isOpen()) {
        uint32_t currentCharOffset = _pages ? _pages[_currentPage].startOffset : 0;
        buildPages();
        gotoCharOffset(currentCharOffset);
        renderPage();
    }
}

void EbookReader::changeFontSize(int delta) {
    int newSize = (int)_layout.fontSize + delta;
    setFontSize((uint8_t)newSize);
}


bool EbookReader::buildPages() {
    clearPages();
    if (!_file || _fileSize == 0 || !_font.isLoaded()) {
        Serial.println("[Reader] Cannot build pages: file or font not ready");
        return false;
    }
    
    uint16_t lineHeight = _layout.calcLineHeight();
    uint16_t contentW = _layout.contentWidth();
    uint16_t contentH = _layout.contentHeight();
    
    size_t estPages = _fileSize / 300 + 100;
    _pages = (PageInfo*)heap_caps_malloc(estPages * sizeof(PageInfo), MALLOC_CAP_SPIRAM);
    if (!_pages) {
        Serial.println("[Reader] Failed to alloc page table");
        return false;
    }
    
    Serial.printf("[Reader] Building pages... (font=%dpx, line=%dpx, content=%dx%d)\n",
                  _layout.fontSize, lineHeight, contentW, contentH);
    
    uint32_t offset = 0;
    _totalPages = 0;
    uint16_t lines = 0;
    uint16_t currentLineWidth = 0;
    bool isParagraphStart = true;
    uint16_t indentPixels = _layout.indentFirstLine * _layout.fontSize;
    
    _pages[0].startOffset = 0;
    
    while (offset < _fileSize && _totalPages < estPages - 1) {
        uint8_t buf[6];
        _file.seek(offset);
        int readLen = _file.read(buf, 1);
        if (readLen <= 0) break;
        
        size_t pos = 0;
        uint32_t ch = decodeUTF8(buf, pos, 1);
        
        int needMore = 0;
        if ((buf[0] & 0xE0) == 0xC0) needMore = 1;
        else if ((buf[0] & 0xF0) == 0xE0) needMore = 2;
        else if ((buf[0] & 0xF8) == 0xF0) needMore = 3;
        
        if (needMore > 0) {
            _file.read(buf + 1, needMore);
            pos = 0;
            ch = decodeUTF8(buf, pos, needMore + 1);
        }
        
        if (ch == '\n') {
            lines++;
            currentLineWidth = 0;
            isParagraphStart = true;
            offset += pos;
            if (lines >= (contentH / lineHeight)) {
                _pages[_totalPages].endOffset = offset;
                _totalPages++;
                if (_totalPages < estPages) {
                    _pages[_totalPages].startOffset = offset;
                }
                lines = 0;
                currentLineWidth = 0;
                isParagraphStart = true;
            }
            continue;
        }
        
        if (ch == '\r') {
            offset += pos;
            continue;
        }
        
        uint8_t charW = _font.getCharAdvance(ch);
        uint16_t lineStartX = 0;
        if (isParagraphStart && _layout.indentFirstLine > 0) {
            lineStartX = indentPixels;
        }
        
        if (currentLineWidth > 0 && currentLineWidth + charW + lineStartX > contentW) {
            lines++;
            currentLineWidth = 0;
            isParagraphStart = false;
            if (lines >= (contentH / lineHeight)) {
                _pages[_totalPages].endOffset = offset;
                _totalPages++;
                if (_totalPages < estPages) {
                    _pages[_totalPages].startOffset = offset;
                }
                lines = 0;
                currentLineWidth = 0;
                isParagraphStart = true;
                continue;
            }
            if (ch != ' ') {
                isParagraphStart = true;
            }
        }
        
        currentLineWidth += charW;
        offset += pos;
    }
    
    if (_totalPages < estPages) {
        _pages[_totalPages].endOffset = offset;
        _totalPages++;
    }
    
    Serial.printf("[Reader] Total pages: %d\n", _totalPages);
    return _totalPages > 0;
}

void EbookReader::clearPages() {
    if (_pages) {
        heap_caps_free(_pages);
        _pages = nullptr;
    }
}

bool EbookReader::loadEpubChapter(int chapterIndex) {
    if (!_isEpub || chapterIndex < 0 || chapterIndex >= _epub.getChapterCount()) return false;
    
    if (_file) {
        _file.close();
    }
    if (_epubTempPath[0] != '\0') {
        SD.remove(_epubTempPath);
        memset(_epubTempPath, 0, sizeof(_epubTempPath));
    }
    
    if (!SD.exists(PROGRESS_DIR)) {
        SD.mkdir(PROGRESS_DIR);
    }
    
    char safeName[64];
    strncpy(safeName, _bookName, sizeof(safeName) - 1);
    safeName[sizeof(safeName) - 1] = '\0';
    for (int i = 0; safeName[i]; i++) {
        if (safeName[i] == '/' || safeName[i] == '\\' || safeName[i] == ' ') safeName[i] = '_';
    }
    snprintf(_epubTempPath, sizeof(_epubTempPath), "%s/%s_ch%d.tmp", PROGRESS_DIR, safeName, chapterIndex);
    
    const size_t bufferSize = 32768;
    bool bufferInPsram = true;
    char* buffer = (char*)heap_caps_malloc(bufferSize, MALLOC_CAP_SPIRAM);
    if (!buffer) {
        bufferInPsram = false;
        buffer = (char*)malloc(bufferSize);
    }
    if (!buffer) {
        Serial.println("[Reader] Failed to alloc EPUB chapter buffer");
        return false;
    }
    
    size_t textLen = 0;
    bool ok = _epub.readChapter(chapterIndex, buffer, bufferSize, textLen);
    if (!ok) {
        Serial.printf("[Reader] Failed to read EPUB chapter %d\n", chapterIndex);
        if (bufferInPsram) heap_caps_free(buffer);
        else free(buffer);
        return false;
    }
    
    File tempFile = SD.open(_epubTempPath, FILE_WRITE);
    if (!tempFile) {
        Serial.printf("[Reader] Failed to create EPUB temp: %s\n", _epubTempPath);
        if (bufferInPsram) heap_caps_free(buffer);
        else free(buffer);
        return false;
    }
    tempFile.write((const uint8_t*)buffer, textLen);
    tempFile.close();
    if (bufferInPsram) heap_caps_free(buffer);
    else free(buffer);
    
    _file = SD.open(_epubTempPath, FILE_READ);
    if (!_file) {
        Serial.printf("[Reader] Failed to open EPUB temp: %s\n", _epubTempPath);
        return false;
    }
    
    _epubChapter = chapterIndex;
    _fileSize = _file.size();
    _hasPreload = false;
    _preloadSize = 0;
    _preloadPage = 0;
    
    bool pagesOk = buildPages();
    Serial.printf("[Reader] EPUB chapter %d loaded: %d bytes, %d pages\n", chapterIndex, _fileSize, _totalPages);
    return pagesOk;
}

uint32_t EbookReader::decodeUTF8(const uint8_t* buf, size_t& pos, size_t len) {
    if (pos >= len) return 0;
    uint8_t c = buf[pos];
    if ((c & 0x80) == 0) {
        pos++;
        return c;
    } else if ((c & 0xE0) == 0xC0 && pos + 1 < len) {
        uint32_t ch = ((c & 0x1F) << 6) | (buf[pos + 1] & 0x3F);
        pos += 2;
        return ch;
    } else if ((c & 0xF0) == 0xE0 && pos + 2 < len) {
        uint32_t ch = ((c & 0x0F) << 12) | ((buf[pos + 1] & 0x3F) << 6) | (buf[pos + 2] & 0x3F);
        pos += 3;
        return ch;
    } else if ((c & 0xF8) == 0xF0 && pos + 3 < len) {
        uint32_t ch = ((c & 0x07) << 18) | ((buf[pos + 1] & 0x3F) << 12) | ((buf[pos + 2] & 0x3F) << 6) | (buf[pos + 3] & 0x3F);
        pos += 4;
        return ch;
    }
    pos++;
    return c;
}

bool EbookReader::nextPage() {
    if (_currentPage < _totalPages - 1) {
        _currentPage++;
        _pageTurnCount++;
        if (_refreshStrategy.usePartialUpdate && 
            _refreshStrategy.fullRefreshEvery > 0 &&
            _pageTurnCount % _refreshStrategy.fullRefreshEvery == 0) {
            _needsFullRefresh = true;
        }
        renderPage();
        preloadNextPage();
        return true;
    }
    
    if (_isEpub && _epubChapter + 1 < _epub.getChapterCount()) {
        if (loadEpubChapter(_epubChapter + 1)) {
            _currentPage = 0;
            _pageTurnCount++;
            _needsFullRefresh = true;
            renderPage();
            preloadNextPage();
            return true;
        }
    }
    return false;
}

void EbookReader::prevPage() {
    if (_currentPage > 0) {
        _currentPage--;
        _pageTurnCount++;
        if (_refreshStrategy.usePartialUpdate && 
            _refreshStrategy.fullRefreshEvery > 0 &&
            _pageTurnCount % _refreshStrategy.fullRefreshEvery == 0) {
            _needsFullRefresh = true;
        }
        _hasPreload = false;
        renderPage();
    }
}

void EbookReader::gotoPage(uint16_t pageNum) {
    if (pageNum < _totalPages) {
        _currentPage = pageNum;
        renderPage();
    }
}

void EbookReader::gotoCharOffset(uint32_t charOffset) {
    for (uint16_t i = 0; i < _totalPages; i++) {
        if (_pages[i].startOffset <= charOffset && charOffset < _pages[i].endOffset) {
            _currentPage = i;
            return;
        }
    }
    _currentPage = _totalPages > 0 ? _totalPages - 1 : 0;
}

void EbookReader::renderPage() {
    if (!isOpen() || !_pages) return;
    if (_needsFullRefresh || !_refreshStrategy.usePartialUpdate) {
        renderPageFull();
        _needsFullRefresh = false;
    } else {
        renderPageFast();
    }
}

void EbookReader::renderPageFull() {
    auto& display = M5.Display;
    // 对标梦西游：周期性使用 GC16 高质量刷新清残影。
    display.setEpdMode(epd_mode_t::epd_quality);
    display.clear();
    if (_hasPreload && _preloadPage == _currentPage) {
        drawTextPageFast();
    } else {
        drawTextPage();
    }
    drawStatusBar();
    display.display();
    Serial.printf("[Reader] Full refresh: page %d/%d\n", _currentPage + 1, _totalPages);
}

void EbookReader::renderPageFast() {
    auto& display = M5.Display;
    // 对标梦西游：快速翻页按策略动态选择波形。
    // 极速=DU4(最快/低对比度)，均衡=DU(快/低残影)，清晰=GL16(文本抗锯齿更好)。
    switch (_refreshStrategy.frequency) {
        case RefreshFrequency::FREQ_LOW:
            display.setEpdMode(epd_mode_t::epd_fastest);
            break;
        case RefreshFrequency::FREQ_MEDIUM:
            display.setEpdMode(epd_mode_t::epd_fast);
            break;
        case RefreshFrequency::FREQ_HIGH:
        default:
            display.setEpdMode(epd_mode_t::epd_text);
            break;
    }
    display.fillRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT - 4, 15);
    if (_hasPreload && _preloadPage == _currentPage) {
        drawTextPageFast();
    } else {
        drawTextPage();
    }
    drawStatusBar();
    display.display();
}

void EbookReader::preloadNextPage() {
    if (!_preloadBuffer || _currentPage + 1 >= _totalPages) return;
    uint16_t nextPage = _currentPage + 1;
    if (nextPage == _preloadPage && _hasPreload) return;
    PageInfo& page = _pages[nextPage];
    uint32_t pageSize = page.endOffset - page.startOffset;
    if (pageSize > PRELOAD_BUFFER_SIZE - 1) pageSize = PRELOAD_BUFFER_SIZE - 1;
    _file.seek(page.startOffset);
    int bytesRead = _file.read((uint8_t*)_preloadBuffer, pageSize);
    if (bytesRead > 0) {
        _preloadBuffer[bytesRead] = '\0';
        _preloadSize = bytesRead;
        _preloadPage = nextPage;
        _hasPreload = true;
        Serial.printf("[Reader] Preloaded page %d (%d bytes)\n", nextPage + 1, bytesRead);
    }
}

void EbookReader::drawTextPageFast() {
    if (!_hasPreload || _preloadPage != _currentPage) {
        drawTextPage();
        return;
    }
    auto& display = M5.Display;
    uint16_t x = _layout.marginLeft;
    uint16_t y = _layout.marginTop;
    uint16_t contentW = _layout.contentWidth();
    uint16_t lineHeight = _layout.calcLineHeight();
    bool isGrayFont = (_font.getFontType() == FontType::GRAY_4BPP);
    uint16_t currentLineWidth = 0;
    bool isParagraphStart = true;
    uint16_t indentPixels = _layout.indentFirstLine * _layout.fontSize;
    LineChar lineChars[256];
    uint8_t lineCharCount = 0;
    size_t bufPos = 0;
    while (bufPos < _preloadSize && y < SCREEN_HEIGHT - _layout.marginBottom - lineHeight) {
        uint8_t buf[6];
        size_t remaining = _preloadSize - bufPos;
        for (int i = 0; i < 6 && i < remaining; i++) buf[i] = _preloadBuffer[bufPos + i];
        size_t charPos = 0;
        uint32_t ch = decodeUTF8(buf, charPos, remaining > 4 ? 4 : remaining);
        if (ch == '\n') {
            if (lineCharCount > 0) {
                drawLineChars(lineChars, lineCharCount, y, lineHeight, currentLineWidth, isParagraphStart);
            }
            y += lineHeight;
            x = _layout.marginLeft;
            currentLineWidth = 0;
            lineCharCount = 0;
            isParagraphStart = true;
            bufPos += charPos;
            continue;
        }
        if (ch == '\r') {
            bufPos += charPos;
            continue;
        }
        uint8_t charW = _font.getCharAdvance(ch);
        uint16_t lineStartX = isParagraphStart ? indentPixels : 0;
        if (currentLineWidth > 0 && currentLineWidth + charW + lineStartX > contentW) {
            if (lineCharCount > 0) {
                drawLineChars(lineChars, lineCharCount, y, lineHeight, currentLineWidth, isParagraphStart);
            }
            y += lineHeight;
            x = _layout.marginLeft;
            currentLineWidth = 0;
            lineCharCount = 0;
            isParagraphStart = false;
            if (y > SCREEN_HEIGHT - _layout.marginBottom - lineHeight) break;
        }
        if (lineCharCount < 255) {
            lineChars[lineCharCount].unicode = ch;
            lineChars[lineCharCount].x = x;
            lineChars[lineCharCount].width = charW;
            lineCharCount++;
        }
        x += charW;
        currentLineWidth += charW;
        bufPos += charPos;
    }
    if (lineCharCount > 0 && y < SCREEN_HEIGHT - _layout.marginBottom - lineHeight) {
        drawLineChars(lineChars, lineCharCount, y, lineHeight, currentLineWidth, isParagraphStart);
    }
}

void EbookReader::drawTextPage() {
    if (!isOpen()) return;
    auto& display = M5.Display;
    PageInfo& page = _pages[_currentPage];
    uint16_t x = _layout.marginLeft;
    uint16_t y = _layout.marginTop;
    _file.seek(page.startOffset);
    uint32_t offset = page.startOffset;
    uint16_t contentW = _layout.contentWidth();
    uint16_t lineHeight = _layout.calcLineHeight();
    bool isGrayFont = (_font.getFontType() == FontType::GRAY_4BPP);
    uint16_t currentLineWidth = 0;
    bool isParagraphStart = true;
    uint16_t indentPixels = _layout.indentFirstLine * _layout.fontSize;
    LineChar lineChars[256];
    uint8_t lineCharCount = 0;
    while (offset < page.endOffset && y < SCREEN_HEIGHT - _layout.marginBottom - lineHeight) {
        uint8_t buf[6];
        int readLen = _file.read(buf, 1);
        if (readLen <= 0) break;
        size_t pos = 0;
        uint32_t ch = decodeUTF8(buf, pos, 1);
        int needMore = 0;
        if ((buf[0] & 0xE0) == 0xC0) needMore = 1;
        else if ((buf[0] & 0xF0) == 0xE0) needMore = 2;
        else if ((buf[0] & 0xF8) == 0xF0) needMore = 3;
        if (needMore > 0) {
            _file.read(buf + 1, needMore);
            pos = 0;
            ch = decodeUTF8(buf, pos, needMore + 1);
        }
        if (ch == '\n') {
            if (lineCharCount > 0) {
                drawLineChars(lineChars, lineCharCount, y, lineHeight, currentLineWidth, isParagraphStart);
            }
            y += lineHeight;
            x = _layout.marginLeft;
            currentLineWidth = 0;
            lineCharCount = 0;
            isParagraphStart = true;
            offset += pos;
            continue;
        }
        if (ch == '\r') {
            offset += pos;
            continue;
        }
        uint8_t charW = _font.getCharAdvance(ch);
        uint16_t lineStartX = isParagraphStart ? indentPixels : 0;
        if (currentLineWidth > 0 && currentLineWidth + charW + lineStartX > contentW) {
            if (lineCharCount > 0) {
                drawLineChars(lineChars, lineCharCount, y, lineHeight, currentLineWidth, isParagraphStart);
            }
            y += lineHeight;
            x = _layout.marginLeft;
            currentLineWidth = 0;
            lineCharCount = 0;
            isParagraphStart = false;
            if (y > SCREEN_HEIGHT - _layout.marginBottom - lineHeight) break;
        }
        if (lineCharCount < 255) {
            lineChars[lineCharCount].unicode = ch;
            lineChars[lineCharCount].x = x;
            lineChars[lineCharCount].width = charW;
            lineCharCount++;
        }
        x += charW;
        currentLineWidth += charW;
        offset += pos;
    }
    if (lineCharCount > 0 && y < SCREEN_HEIGHT - _layout.marginBottom - lineHeight) {
        drawLineChars(lineChars, lineCharCount, y, lineHeight, currentLineWidth, isParagraphStart);
    }
}

void EbookReader::drawLineChars(LineChar* chars, uint8_t count, uint16_t y, 
                                 uint16_t lineHeight, uint16_t lineWidth, bool isParagraphStart) {
    auto& display = M5.Display;
    bool isGrayFont = (_font.getFontType() == FontType::GRAY_4BPP);
    uint16_t startX = _layout.marginLeft;
    if (isParagraphStart && _layout.indentFirstLine > 0) {
        startX += _layout.indentFirstLine * _layout.fontSize;
    }
    int16_t extraSpace = 0;
    if (_layout.justify && count > 1 && !isParagraphStart) {
        uint16_t contentW = _layout.contentWidth();
        if (lineWidth < contentW - 10) {
            extraSpace = (contentW - lineWidth) / (count - 1);
        }
    }
    uint16_t x = startX;
    for (int i = 0; i < count; i++) {
        if (isGrayFont) {
            drawGrayChar(chars[i].unicode, x, y);
        } else {
            drawBitmapChar(chars[i].unicode, x, y);
        }
        x += chars[i].width + extraSpace;
    }
}

void EbookReader::drawBitmapChar(uint32_t unicode, uint16_t x, uint16_t y) {
    auto& display = M5.Display;
    uint8_t bw, bh;
    const uint8_t* bitmap = _font.getCharBitmap(unicode, bw, bh);
    if (!bitmap || bw == 0 || bh == 0) return;
    for (int row = 0; row < bh && (y + row) < SCREEN_HEIGHT; row++) {
        for (int col = 0; col < bw && (x + col) < SCREEN_WIDTH; col++) {
            int byteIdx = row * ((bw + 7) / 8) + (col / 8);
            int bitIdx = 7 - (col % 8);
            if (bitmap[byteIdx] & (1 << bitIdx)) {
                display.drawPixel(x + col, y + row, 0);
            }
        }
    }
}

void EbookReader::drawGrayChar(uint32_t unicode, uint16_t x, uint16_t y) {
    auto& display = M5.Display;
    uint8_t width, height, advance;
    int8_t bearingX, bearingY;
    const uint8_t* grayBitmap = _font.getCharBitmapGray(unicode, width, height, bearingX, bearingY, advance);
    if (!grayBitmap || width == 0 || height == 0) return;
    int drawX = x + bearingX;
    int drawY = y + (_layout.fontSize - bearingY);
    for (int row = 0; row < height && (drawY + row) < SCREEN_HEIGHT; row++) {
        for (int col = 0; col < width && (drawX + col) < SCREEN_WIDTH; col++) {
            if ((drawX + col) < 0 || (drawY + row) < 0) continue;
            int srcIdx = row * ((width + 1) / 2) + col / 2;
            uint8_t nibble;
            if (col % 2 == 0) {
                nibble = (grayBitmap[srcIdx] >> 4) & 0x0F;
            } else {
                nibble = grayBitmap[srcIdx] & 0x0F;
            }
            if (nibble > 0) {
                display.drawPixel(drawX + col, drawY + row, nibble);
            }
        }
    }
}

void EbookReader::drawStatusBar() {
    auto& display = M5.Display;
    int barY = SCREEN_HEIGHT - 4;
    display.drawRect(0, barY, SCREEN_WIDTH, 4, 0);
    if (_totalPages > 0) {
        int progressW = (int)((float)_currentPage / (_totalPages - 1) * SCREEN_WIDTH);
        display.fillRect(0, barY, progressW, 4, 0);
    }
    
    // 电量图标（左上角）
#if BATTERY_ICON_ENABLED
    BatteryInfo bat = BatteryInfo::read();
    if (bat.valid) {
        int bx = BATTERY_ICON_X;
        int by = BATTERY_ICON_Y;
        display.drawRect(bx, by, 20, 10, 0);
        display.drawRect(bx + 20, by + 2, 2, 6, 0);
        int fw = (bat.level * 16) / 100;
        if (fw > 0) display.fillRect(bx + 2, by + 2, fw, 6, 0);
    }
#endif
}

void EbookReader::getProgressPath(char* out, size_t len) {
    snprintf(out, len, "%s/", PROGRESS_DIR);
    char safeName[64];
    strncpy(safeName, _bookName, sizeof(safeName) - 1);
    for (int i = 0; safeName[i]; i++) {
        if (safeName[i] == '/' || safeName[i] == '\\') safeName[i] = '_';
    }
    strncat(out, safeName, len - strlen(out) - 1);
    strncat(out, ".json", len - strlen(out) - 1);
}

void EbookReader::saveProgress() {
    if (!isOpen() || _currentPage == 0) return;
    char path[128];
    getProgressPath(path, sizeof(path));
    File f = SD.open(path, FILE_WRITE);
    if (!f) return;
    DynamicJsonDocument doc(2048);
    doc["book"] = _bookPath;
    doc["page"] = _currentPage;
    doc["totalPages"] = _totalPages;
    doc["fontSize"] = _layout.fontSize;
    doc["timestamp"] = millis();
    serializeJson(doc, f);
    f.close();
    Serial.printf("[Reader] Progress saved: page %d/%d\n", _currentPage, _totalPages);
}

bool EbookReader::loadProgress() {
    char path[128];
    getProgressPath(path, sizeof(path));
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    DynamicJsonDocument doc(2048);
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) return false;
    const char* savedBook = doc["book"];
    if (!savedBook || strcmp(savedBook, _bookPath) != 0) return false;
    _currentPage = doc["page"] | 0;
    if (_currentPage >= _totalPages) _currentPage = 0;
    Serial.printf("[Reader] Progress loaded: page %d/%d\n", _currentPage, _totalPages);
    return true;
}

void EbookReader::getLayoutConfigPath(char* out, size_t len) {
    snprintf(out, len, "%s/", PROGRESS_DIR);
    char safeName[64];
    strncpy(safeName, _bookName, sizeof(safeName) - 1);
    for (int i = 0; safeName[i]; i++) {
        if (safeName[i] == '/' || safeName[i] == '\\') safeName[i] = '_';
    }
    strncat(out, safeName, len - strlen(out) - 1);
    strncat(out, ".layout.json", len - strlen(out) - 1);
}

void EbookReader::saveLayoutConfig() {
    char path[128];
    getLayoutConfigPath(path, sizeof(path));
    File f = SD.open(path, FILE_WRITE);
    if (!f) return;
    DynamicJsonDocument doc(2048);
    doc["fontSize"] = _layout.fontSize;
    doc["lineSpacing"] = _layout.lineSpacing;
    doc["paragraphSpacing"] = _layout.paragraphSpacing;
    doc["marginLeft"] = _layout.marginLeft;
    doc["marginRight"] = _layout.marginRight;
    doc["marginTop"] = _layout.marginTop;
    doc["marginBottom"] = _layout.marginBottom;
    doc["indentFirstLine"] = _layout.indentFirstLine;
    doc["justify"] = _layout.justify;
    doc["timestamp"] = millis();
    serializeJson(doc, f);
    f.close();
}

bool EbookReader::loadLayoutConfig() {
    char path[128];
    getLayoutConfigPath(path, sizeof(path));
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    DynamicJsonDocument doc(2048);
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) return false;
    _layout.fontSize = doc["fontSize"] | _layout.fontSize;
    _layout.lineSpacing = doc["lineSpacing"] | _layout.lineSpacing;
    _layout.paragraphSpacing = doc["paragraphSpacing"] | _layout.paragraphSpacing;
    _layout.marginLeft = doc["marginLeft"] | _layout.marginLeft;
    _layout.marginRight = doc["marginRight"] | _layout.marginRight;
    _layout.marginTop = doc["marginTop"] | _layout.marginTop;
    _layout.marginBottom = doc["marginBottom"] | _layout.marginBottom;
    _layout.indentFirstLine = doc["indentFirstLine"] | _layout.indentFirstLine;
    _layout.justify = doc["justify"] | _layout.justify;
    if (_layout.fontSize < 12) _layout.fontSize = 12;
    if (_layout.fontSize > 48) _layout.fontSize = 48;
    if (_layout.lineSpacing < 50) _layout.lineSpacing = 50;
    if (_layout.lineSpacing > 200) _layout.lineSpacing = 200;
    Serial.printf("[Reader] Layout loaded: font=%dpx, line=%d%%\n",
                  _layout.fontSize, _layout.lineSpacing);
    return true;
}

// ===== 章节管理 =====

#include "ChapterDetector.h"

void EbookReader::getChapterCachePath(char* out, size_t len) {
    snprintf(out, len, "%s/", PROGRESS_DIR);
    char safeName[64];
    strncpy(safeName, _bookName, sizeof(safeName) - 1);
    for (int i = 0; safeName[i]; i++) {
        if (safeName[i] == '/' || safeName[i] == '\\') safeName[i] = '_';
    }
    strncat(out, safeName, len - strlen(out) - 1);
    strncat(out, ".chapters.json", len - strlen(out) - 1);
}

bool EbookReader::detectChapters() {
    if (!_file || _fileSize == 0) return false;
    if (loadChaptersFromCache()) {
        Serial.printf("[Reader] Loaded %d chapters from cache\n", _chapterCount);
        return true;
    }
    Serial.println("[Reader] Detecting chapters...");
    _chapterCapacity = 500;
    _chapters = (ChapterInfo*)heap_caps_malloc(_chapterCapacity * sizeof(ChapterInfo), MALLOC_CAP_SPIRAM);
    if (!_chapters) {
        Serial.println("[Reader] Failed to alloc chapter array");
        return false;
    }
    ChapterDetector detector;
    ChapterDetectResult results[500];
    int count = detector.detect(_file, results, 500);
    if (count > 0) {
        for (int i = 0; i < count && i < _chapterCapacity; i++) {
            _chapters[i].index = i;
            _chapters[i].charOffset = results[i].charOffset;
            _chapters[i].title = results[i].title;
            _chapters[i].confidence = results[i].score;
            _chapters[i].pageNum = 0;
            if (_pages) {
                for (uint16_t p = 0; p < _totalPages; p++) {
                    if (_pages[p].startOffset <= results[i].charOffset && 
                        results[i].charOffset < _pages[p].endOffset) {
                        _chapters[i].pageNum = p;
                        break;
                    }
                }
            }
        }
        _chapterCount = count;
        _chaptersDetected = true;
        Serial.printf("[Reader] Detected %d chapters\n", _chapterCount);
        saveChaptersToCache();
        return true;
    }
    clearChapters();
    return false;
}

bool EbookReader::loadChaptersFromCache() {
    char path[128];
    getChapterCachePath(path, sizeof(path));
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    DynamicJsonDocument doc(2048);
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) return false;
    uint32_t cachedSize = doc["fileSize"] | 0;
    if (cachedSize != _fileSize) return false;
    JsonArray arr = doc["chapters"];
    int count = arr.size();
    if (count <= 0) return false;
    _chapterCapacity = count + 50;
    _chapters = (ChapterInfo*)heap_caps_malloc(_chapterCapacity * sizeof(ChapterInfo), MALLOC_CAP_SPIRAM);
    if (!_chapters) return false;
    for (int i = 0; i < count; i++) {
        _chapters[i].index = i;
        _chapters[i].charOffset = arr[i]["offset"] | 0;
        _chapters[i].title = arr[i]["title"] | "";
        _chapters[i].confidence = arr[i]["confidence"] | 0;
        _chapters[i].pageNum = arr[i]["pageNum"] | 0;
    }
    _chapterCount = count;
    _chaptersDetected = true;
    Serial.printf("[Reader] Loaded %d chapters from cache\n", _chapterCount);
    return true;
}

void EbookReader::saveChaptersToCache() {
    if (!_chapters || _chapterCount <= 0) return;
    char path[128];
    getChapterCachePath(path, sizeof(path));
    File f = SD.open(path, FILE_WRITE);
    if (!f) return;
    DynamicJsonDocument doc(2048);
    doc["bookName"] = _bookName;
    doc["fileSize"] = _fileSize;
    doc["chapterCount"] = _chapterCount;
    JsonArray arr = doc["chapters"].to<JsonArray>();
    for (int i = 0; i < _chapterCount; i++) {
        JsonObject obj = addJsonObject(arr);
        obj["offset"] = _chapters[i].charOffset;
        obj["title"] = _chapters[i].title;
        obj["confidence"] = _chapters[i].confidence;
        obj["pageNum"] = _chapters[i].pageNum;
    }
    serializeJson(doc, f);
    f.close();
    Serial.printf("[Reader] Saved %d chapters to cache\n", _chapterCount);
}

void EbookReader::clearChapters() {
    if (_chapters) {
        heap_caps_free(_chapters);
        _chapters = nullptr;
    }
    _chapterCount = 0;
    _chapterCapacity = 0;
    _chaptersDetected = false;
}

bool EbookReader::hasChapters() const {
    if (_isEpub) return _epub.getChapterCount() > 0;
    return _chaptersDetected && _chapters && _chapterCount > 0;
}


ChapterInfo* EbookReader::getChapter(int index) {
    if (index < 0 || index >= _chapterCount || !_chapters) return nullptr;
    return &_chapters[index];
}

int EbookReader::getCurrentChapterIndex() const {
    if (_isEpub) return _epubChapter;
    if (!_chapters || _chapterCount <= 0 || !_pages) return -1;
    uint32_t currentOffset = _pages[_currentPage].startOffset;
    for (int i = _chapterCount - 1; i >= 0; i--) {
        if (_chapters[i].charOffset <= currentOffset) {
            return i;
        }
    }
    return 0;
}

void EbookReader::gotoChapter(int chapterIndex) {
    if (_isEpub) {
        if (chapterIndex < 0 || chapterIndex >= _epub.getChapterCount()) return;
        if (loadEpubChapter(chapterIndex)) {
            _currentPage = 0;
            _needsFullRefresh = true;
            renderPage();
            preloadNextPage();
        }
        return;
    }
    if (chapterIndex < 0 || chapterIndex >= _chapterCount || !_chapters) return;
    uint32_t targetOffset = _chapters[chapterIndex].charOffset;
    gotoCharOffset(targetOffset);
    _needsFullRefresh = true;
    renderPage();
}

RefreshFrequency EbookReader::getRefreshFrequency() const {
    if (_refreshStrategy.fullRefreshEvery == 20) return RefreshFrequency::FREQ_LOW;
    if (_refreshStrategy.fullRefreshEvery == 10) return RefreshFrequency::FREQ_MEDIUM;
    if (_refreshStrategy.fullRefreshEvery == 5) return RefreshFrequency::FREQ_HIGH;
    return RefreshFrequency::FREQ_MEDIUM;
}

// ===== 书签管理 =====

void EbookReader::getBookmarkPath(char* out, size_t len) {
    snprintf(out, len, "%s/", PROGRESS_DIR);
    char safeName[64];
    strncpy(safeName, _bookName, sizeof(safeName) - 1);
    for (int i = 0; safeName[i]; i++) {
        if (safeName[i] == '/' || safeName[i] == '\\') safeName[i] = '_';
    }
    strncat(out, safeName, len - strlen(out) - 1);
    strncat(out, ".bookmarks.json", len - strlen(out) - 1);
}

bool EbookReader::loadBookmarks() {
    char path[128];
    getBookmarkPath(path, sizeof(path));
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    DynamicJsonDocument doc(2048);
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) return false;
    JsonArray arr = doc["bookmarks"];
    int count = arr.size();
    if (count <= 0) return false;
    _bookmarkCapacity = count + 10;
    _bookmarks = (Bookmark*)heap_caps_malloc(_bookmarkCapacity * sizeof(Bookmark), MALLOC_CAP_SPIRAM);
    if (!_bookmarks) return false;
    for (int i = 0; i < count; i++) {
        _bookmarks[i].index = i;
        _bookmarks[i].charOffset = arr[i]["offset"] | 0;
        _bookmarks[i].pageNum = arr[i]["pageNum"] | 0;
        _bookmarks[i].name = arr[i]["name"] | "";
        _bookmarks[i].timestamp = arr[i]["timestamp"] | 0;
    }
    _bookmarkCount = count;
    Serial.printf("[Reader] Loaded %d bookmarks\n", _bookmarkCount);
    return true;
}

void EbookReader::saveBookmarks() {
    if (!_bookmarks || _bookmarkCount <= 0) return;
    char path[128];
    getBookmarkPath(path, sizeof(path));
    File f = SD.open(path, FILE_WRITE);
    if (!f) return;
    DynamicJsonDocument doc(2048);
    doc["bookName"] = _bookName;
    doc["bookmarkCount"] = _bookmarkCount;
    JsonArray arr = doc["bookmarks"].to<JsonArray>();
    for (int i = 0; i < _bookmarkCount; i++) {
        JsonObject obj = addJsonObject(arr);
        obj["offset"] = _bookmarks[i].charOffset;
        obj["pageNum"] = _bookmarks[i].pageNum;
        obj["name"] = _bookmarks[i].name;
        obj["timestamp"] = _bookmarks[i].timestamp;
    }
    serializeJson(doc, f);
    f.close();
    Serial.printf("[Reader] Saved %d bookmarks\n", _bookmarkCount);
}

void EbookReader::clearBookmarks() {
    if (_bookmarks) {
        heap_caps_free(_bookmarks);
        _bookmarks = nullptr;
    }
    _bookmarkCount = 0;
    _bookmarkCapacity = 0;
}

bool EbookReader::addBookmark(const char* name) {
    if (!isOpen() || !_pages) return false;
    if (_bookmarkCount >= 20) {
        Serial.println("[Reader] Bookmark limit reached (20)");
        return false;
    }
    if (_bookmarkCount >= _bookmarkCapacity) {
        int newCap = _bookmarkCapacity + 10;
        Bookmark* newArr = (Bookmark*)heap_caps_malloc(newCap * sizeof(Bookmark), MALLOC_CAP_SPIRAM);
        if (!newArr) return false;
        if (_bookmarks) {
            memcpy(newArr, _bookmarks, _bookmarkCount * sizeof(Bookmark));
            heap_caps_free(_bookmarks);
        }
        _bookmarks = newArr;
        _bookmarkCapacity = newCap;
    }
    Bookmark& bm = _bookmarks[_bookmarkCount];
    bm.index = _bookmarkCount;
    bm.charOffset = _pages[_currentPage].startOffset;
    bm.pageNum = _currentPage;
    if (name && strlen(name) > 0) {
        bm.name = name;
    } else {
        bm.name = String("书签 ") + String(_bookmarkCount + 1);
    }
    bm.timestamp = millis();
    _bookmarkCount++;
    saveBookmarks();
    Serial.printf("[Reader] Bookmark added at page %d\n", bm.pageNum);
    return true;
}

bool EbookReader::deleteBookmark(int index) {
    if (index < 0 || index >= _bookmarkCount || !_bookmarks) return false;
    for (int i = index; i < _bookmarkCount - 1; i++) {
        _bookmarks[i] = _bookmarks[i + 1];
        _bookmarks[i].index = i;
    }
    _bookmarkCount--;
    saveBookmarks();
    Serial.printf("[Reader] Bookmark %d deleted\n", index);
    return true;
}

const Bookmark* EbookReader::getBookmark(int index) const {
    if (index < 0 || index >= _bookmarkCount || !_bookmarks) return nullptr;
    return &_bookmarks[index];
}

void EbookReader::gotoBookmark(int index) {
    if (index < 0 || index >= _bookmarkCount || !_bookmarks) return;
    gotoCharOffset(_bookmarks[index].charOffset);
    _needsFullRefresh = true;
    renderPage();
}
