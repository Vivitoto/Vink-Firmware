#include "ReaderBookService.h"
#include "ReaderTextRenderer.h"
#include "../../Config.h"
#include "../../TextCodec.h"
#include <esp_heap_caps.h>

namespace vink3 {

ReaderBookService g_readerBook;

bool ReaderBookService::begin() {
    ensureTocBuffer();
    ensureBookBuffers();
    if (!pageStarts_) {
        pageStarts_ = static_cast<uint32_t*>(heap_caps_calloc(kMaxChapterPages, sizeof(uint32_t), MALLOC_CAP_SPIRAM));
        if (!pageStarts_) pageStarts_ = static_cast<uint32_t*>(calloc(kMaxChapterPages, sizeof(uint32_t)));
    }
    ensureSdReady();
    return true;
}

bool ReaderBookService::ensureSdReady() {
    if (sdReady_) return true;
    sdReady_ = SD.begin();
    if (sdReady_) {
        if (!SD.exists(BOOKS_DIR)) SD.mkdir(BOOKS_DIR);
        if (!SD.exists(PROGRESS_DIR)) SD.mkdir(PROGRESS_DIR);
    }
    Serial.printf("[vink3][book] SD %s\n", sdReady_ ? "ready" : "unavailable");
    return sdReady_;
}

bool ReaderBookService::ensureTocBuffer() {
    if (toc_) return true;
    toc_ = static_cast<ChapterDetectResult*>(heap_caps_calloc(kMaxTocEntries, sizeof(ChapterDetectResult), MALLOC_CAP_SPIRAM));
    if (!toc_) {
        toc_ = static_cast<ChapterDetectResult*>(calloc(kMaxTocEntries, sizeof(ChapterDetectResult)));
    }
    if (!toc_) {
        Serial.println("[vink3][book] failed to allocate TOC buffer");
        return false;
    }
    return true;
}

bool ReaderBookService::ensureBookBuffers() {
    if (bookPaths_ && bookTitles_ && bookFlags_) return true;
    if (!bookPaths_) {
        bookPaths_ = static_cast<char (*)[160]>(heap_caps_calloc(kMaxBooks, sizeof(*bookPaths_), MALLOC_CAP_SPIRAM));
        if (!bookPaths_) bookPaths_ = static_cast<char (*)[160]>(calloc(kMaxBooks, sizeof(*bookPaths_)));
    }
    if (!bookTitles_) {
        bookTitles_ = static_cast<char (*)[72]>(heap_caps_calloc(kMaxBooks, sizeof(*bookTitles_), MALLOC_CAP_SPIRAM));
        if (!bookTitles_) bookTitles_ = static_cast<char (*)[72]>(calloc(kMaxBooks, sizeof(*bookTitles_)));
    }
    if (!bookFlags_) {
        bookFlags_ = static_cast<uint8_t*>(heap_caps_calloc(kMaxBooks, sizeof(uint8_t), MALLOC_CAP_SPIRAM));
        if (!bookFlags_) bookFlags_ = static_cast<uint8_t*>(calloc(kMaxBooks, sizeof(uint8_t)));
    }
    if (!bookPaths_ || !bookTitles_ || !bookFlags_) {
        Serial.println("[vink3][book] failed to allocate library buffers");
        return false;
    }
    return true;
}

bool ReaderBookService::isTxtPath(const char* name) const {
    if (!name) return false;
    String s(name);
    s.toLowerCase();
    return s.endsWith(".txt");
}

bool ReaderBookService::scanBooks() {
    if (!ensureSdReady() || !ensureBookBuffers()) return false;
    bookCount_ = 0;
    File dir = SD.open(BOOKS_DIR);
    if (!dir || !dir.isDirectory()) return false;
    File f = dir.openNextFile();
    while (f && bookCount_ < kMaxBooks) {
        if (!f.isDirectory() && isTxtPath(f.name())) {
            if (f.name()[0] == '/') strlcpy(bookPaths_[bookCount_], f.name(), sizeof(bookPaths_[bookCount_]));
            else snprintf(bookPaths_[bookCount_], sizeof(bookPaths_[bookCount_]), "%s/%s", BOOKS_DIR, f.name());
            const char* slash = strrchr(bookPaths_[bookCount_], '/');
            const char* name = slash ? slash + 1 : bookPaths_[bookCount_];
            strlcpy(bookTitles_[bookCount_], name, sizeof(bookTitles_[bookCount_]));
            char* dot = strrchr(bookTitles_[bookCount_], '.');
            if (dot) *dot = '\0';
            bookFlags_[bookCount_] = detectBookFlags(bookPaths_[bookCount_]);
            bookCount_++;
        }
        f = dir.openNextFile();
    }
    dir.close();
    booksScanned_ = true;
    if (bookPage_ * kBooksPerPage >= bookCount_) bookPage_ = 0;
    Serial.printf("[vink3][book] library scan: %d TXT books\n", bookCount_);
    return true;
}

void ReaderBookService::closeCurrent() {
    open_ = false;
    tocCount_ = 0;
    tocPage_ = 0;
    currentTocIndex_ = -1;
    pageCount_ = 0;
    currentPage_ = 0;
    hasProgress_ = false;
    showingBookEntry_ = false;
    showingToc_ = true;
    bookPath_[0] = '\0';
    activeTextPath_[0] = '\0';
    title_[0] = '\0';
}

void ReaderBookService::setTitleFromPath(const char* path) {
    const char* slash = strrchr(path, '/');
    const char* name = slash ? slash + 1 : path;
    strlcpy(title_, name, sizeof(title_));
    char* dot = strrchr(title_, '.');
    if (dot) *dot = '\0';
}

void ReaderBookService::getSidecarPath(char* out, size_t len, const char* suffix) const {
    // Keep generated metadata next to the book for easier file management:
    //   /books/foo.txt -> /books/foo.vink-toc / foo.vink-progress / foo.vink-pages
    // Use the original book path, not the temporary UTF-8 conversion path.
    getSidecarPathForBook(out, len, bookPath_, suffix);
}

void ReaderBookService::getSidecarPathForBook(char* out, size_t len, const char* bookPath, const char* suffix) const {
    if (!out || len == 0) return;
    out[0] = '\0';
    if (!bookPath || !bookPath[0]) return;
    strlcpy(out, bookPath, len);
    char* slash = strrchr(out, '/');
    char* dot = strrchr(out, '.');
    if (dot && (!slash || dot > slash)) {
        strlcpy(dot, suffix, len - static_cast<size_t>(dot - out));
    } else {
        strlcat(out, suffix, len);
    }
}

uint8_t ReaderBookService::detectBookFlags(const char* bookPath) const {
    if (!bookPath || !bookPath[0]) return 0;
    uint8_t flags = 0;
    char sidecar[160];
    getSidecarPathForBook(sidecar, sizeof(sidecar), bookPath, ".vink-toc");
    if (sidecar[0] && SD.exists(sidecar)) flags |= kBookHasTocCache;
    getSidecarPathForBook(sidecar, sizeof(sidecar), bookPath, ".vink-progress");
    if (sidecar[0] && SD.exists(sidecar)) flags |= kBookHasProgress;
    getSidecarPathForBook(sidecar, sizeof(sidecar), bookPath, ".vink-pages");
    if (sidecar[0] && SD.exists(sidecar)) flags |= kBookHasPageCache;
    return flags;
}

void ReaderBookService::getTocCachePath(char* out, size_t len) const {
    getSidecarPath(out, len, ".vink-toc");
}

void ReaderBookService::getProgressPath(char* out, size_t len) const {
    getSidecarPath(out, len, ".vink-progress");
}

void ReaderBookService::getPageCachePath(char* out, size_t len) const {
    getSidecarPath(out, len, ".vink-pages");
}

bool ReaderBookService::loadTocCache() {
    if (!ensureTocBuffer() || !bookPath_[0]) return false;
    char cachePath[96];
    getTocCachePath(cachePath, sizeof(cachePath));
    File f = SD.open(cachePath, FILE_READ);
    if (!f) return false;
    uint32_t magic = 0;
    uint16_t count = 0;
    f.read(reinterpret_cast<uint8_t*>(&magic), sizeof(magic));
    f.read(reinterpret_cast<uint8_t*>(&count), sizeof(count));
    if (magic != 0x56435431UL || count == 0 || count > kMaxTocEntries) {
        f.close();
        return false;
    }
    tocCount_ = 0;
    for (uint16_t i = 0; i < count && f.available(); ++i) {
        uint8_t type = 0;
        uint16_t titleLen = 0;
        f.read(reinterpret_cast<uint8_t*>(&toc_[i].charOffset), sizeof(toc_[i].charOffset));
        f.read(reinterpret_cast<uint8_t*>(&toc_[i].chapterNumber), sizeof(toc_[i].chapterNumber));
        f.read(&type, sizeof(type));
        f.read(reinterpret_cast<uint8_t*>(&toc_[i].score), sizeof(toc_[i].score));
        f.read(reinterpret_cast<uint8_t*>(&titleLen), sizeof(titleLen));
        if (titleLen > 120) titleLen = 120;
        char buf[128];
        size_t n = f.read(reinterpret_cast<uint8_t*>(buf), titleLen);
        buf[n] = '\0';
        toc_[i].title = String(buf);
        tocCount_++;
    }
    f.close();
    Serial.printf("[vink3][book] TOC cache loaded: %d entries\n", tocCount_);
    return tocCount_ > 0;
}

void ReaderBookService::saveTocCache() {
    if (!bookPath_[0] || tocCount_ <= 0 || !ensureSdReady()) return;
    char cachePath[96];
    getTocCachePath(cachePath, sizeof(cachePath));
    File f = SD.open(cachePath, FILE_WRITE);
    if (!f) return;
    uint32_t magic = 0x56435431UL; // VCT1
    uint16_t count = static_cast<uint16_t>(tocCount_);
    f.write(reinterpret_cast<const uint8_t*>(&magic), sizeof(magic));
    f.write(reinterpret_cast<const uint8_t*>(&count), sizeof(count));
    for (int i = 0; i < tocCount_; ++i) {
        uint8_t type = toc_[i].title.indexOf("卷") >= 0 ? 1 : 0;
        uint16_t titleLen = min<size_t>(toc_[i].title.length(), 120);
        f.write(reinterpret_cast<const uint8_t*>(&toc_[i].charOffset), sizeof(toc_[i].charOffset));
        f.write(reinterpret_cast<const uint8_t*>(&toc_[i].chapterNumber), sizeof(toc_[i].chapterNumber));
        f.write(&type, sizeof(type));
        f.write(reinterpret_cast<const uint8_t*>(&toc_[i].score), sizeof(toc_[i].score));
        f.write(reinterpret_cast<const uint8_t*>(&titleLen), sizeof(titleLen));
        f.write(reinterpret_cast<const uint8_t*>(toc_[i].title.c_str()), titleLen);
    }
    f.close();
    Serial.printf("[vink3][book] TOC cache saved: %s (%d entries)\n", cachePath, tocCount_);
}

uint32_t ReaderBookService::activeTextSize() const {
    const char* p = activeTextPath_[0] ? activeTextPath_ : bookPath_;
    if (!p || !p[0]) return 0;
    File f = SD.open(p, FILE_READ);
    if (!f) return 0;
    uint32_t size = f.size();
    f.close();
    return size;
}

bool ReaderBookService::loadProgress() {
    if (!bookPath_[0] || !ensureSdReady()) return false;
    char path[96];
    getProgressPath(path, sizeof(path));
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    uint32_t magic = 0;
    uint32_t cachedSize = 0;
    uint16_t chapter = 0;
    uint16_t page = 0;
    f.read(reinterpret_cast<uint8_t*>(&magic), sizeof(magic));
    f.read(reinterpret_cast<uint8_t*>(&cachedSize), sizeof(cachedSize));
    f.read(reinterpret_cast<uint8_t*>(&chapter), sizeof(chapter));
    f.read(reinterpret_cast<uint8_t*>(&page), sizeof(page));
    f.close();
    if (magic != 0x56505232UL || cachedSize != activeTextSize() || chapter >= tocCount_) return false; // VPR2
    if (!buildChapterPages(chapter)) return false;
    currentTocIndex_ = chapter;
    currentPage_ = page < pageCount_ ? page : 0;
    hasProgress_ = true;
    showingToc_ = false;
    Serial.printf("[vink3][book] progress loaded: chapter=%u page=%u\n", chapter, page);
    return true;
}

void ReaderBookService::saveProgress() {
    if (!bookPath_[0] || currentTocIndex_ < 0 || !ensureSdReady()) return;
    char path[96];
    getProgressPath(path, sizeof(path));
    File f = SD.open(path, FILE_WRITE);
    if (!f) return;
    uint32_t magic = 0x56505232UL; // VPR2
    uint32_t fileSize = activeTextSize();
    uint16_t chapter = static_cast<uint16_t>(currentTocIndex_);
    uint16_t page = static_cast<uint16_t>(max(0, currentPage_));
    f.write(reinterpret_cast<const uint8_t*>(&magic), sizeof(magic));
    f.write(reinterpret_cast<const uint8_t*>(&fileSize), sizeof(fileSize));
    f.write(reinterpret_cast<const uint8_t*>(&chapter), sizeof(chapter));
    f.write(reinterpret_cast<const uint8_t*>(&page), sizeof(page));
    f.close();
}

bool ReaderBookService::loadChapterPageCache(int index, uint32_t start, uint32_t end) {
    if (!bookPath_[0] || !pageStarts_ || !ensureSdReady()) return false;
    char path[96];
    getPageCachePath(path, sizeof(path));
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    uint32_t magic = 0;
    uint32_t cachedSize = 0;
    uint16_t chapter = 0;
    uint16_t count = 0;
    uint32_t cachedStart = 0;
    uint32_t cachedEnd = 0;
    f.read(reinterpret_cast<uint8_t*>(&magic), sizeof(magic));
    f.read(reinterpret_cast<uint8_t*>(&cachedSize), sizeof(cachedSize));
    f.read(reinterpret_cast<uint8_t*>(&chapter), sizeof(chapter));
    f.read(reinterpret_cast<uint8_t*>(&count), sizeof(count));
    f.read(reinterpret_cast<uint8_t*>(&cachedStart), sizeof(cachedStart));
    f.read(reinterpret_cast<uint8_t*>(&cachedEnd), sizeof(cachedEnd));
    if (magic != 0x56504732UL || cachedSize != activeTextSize() || chapter != index || count == 0 || count > kMaxChapterPages || cachedStart != start || cachedEnd != end) {
        f.close();
        return false;
    }
    size_t need = static_cast<size_t>(count) * sizeof(uint32_t);
    size_t got = f.read(reinterpret_cast<uint8_t*>(pageStarts_), need);
    f.close();
    if (got != need) return false;
    pageCount_ = count;
    currentPage_ = 0;
    Serial.printf("[vink3][book] page cache loaded: chapter=%d pages=%d\n", index, pageCount_);
    return true;
}

void ReaderBookService::saveChapterPageCache(int index, uint32_t start, uint32_t end) {
    if (!bookPath_[0] || !pageStarts_ || pageCount_ <= 0 || !ensureSdReady()) return;
    char path[96];
    getPageCachePath(path, sizeof(path));
    File f = SD.open(path, FILE_WRITE);
    if (!f) return;
    uint32_t magic = 0x56504732UL; // VPG2
    uint32_t fileSize = activeTextSize();
    uint16_t chapter = static_cast<uint16_t>(index);
    uint16_t count = static_cast<uint16_t>(pageCount_);
    f.write(reinterpret_cast<const uint8_t*>(&magic), sizeof(magic));
    f.write(reinterpret_cast<const uint8_t*>(&fileSize), sizeof(fileSize));
    f.write(reinterpret_cast<const uint8_t*>(&chapter), sizeof(chapter));
    f.write(reinterpret_cast<const uint8_t*>(&count), sizeof(count));
    f.write(reinterpret_cast<const uint8_t*>(&start), sizeof(start));
    f.write(reinterpret_cast<const uint8_t*>(&end), sizeof(end));
    f.write(reinterpret_cast<const uint8_t*>(pageStarts_), static_cast<size_t>(pageCount_) * sizeof(uint32_t));
    f.close();
    Serial.printf("[vink3][book] page cache saved: chapter=%d pages=%d\n", index, pageCount_);
}

bool ReaderBookService::openFirstBook() {
    if (!booksScanned_ && !scanBooks()) return false;
    if (bookCount_ <= 0) return false;
    return openBook(bookPaths_[0]);
}

bool ReaderBookService::openBook(const char* path) {
    if (!path || !path[0] || !ensureSdReady() || !ensureTocBuffer()) return false;
    closeCurrent();
    strlcpy(bookPath_, path, sizeof(bookPath_));
    strlcpy(activeTextPath_, path, sizeof(activeTextPath_));
    setTitleFromPath(path);

    File detectFile = SD.open(path, FILE_READ);
    if (!detectFile) {
        Serial.printf("[vink3][book] open failed: %s\n", path);
        return false;
    }
    TextEncoding encoding = TextCodec::detect(detectFile);
    detectFile.close();
    if (encoding == TextEncoding::GBK) {
        String tmp = TextCodec::convertToUTF8(path);
        if (tmp.length() > 0) strlcpy(activeTextPath_, tmp.c_str(), sizeof(activeTextPath_));
    }

    open_ = true;
    if (!loadTocCache()) {
        File f = SD.open(activeTextPath_, FILE_READ);
        if (f) {
            ChapterDetector detector;
            tocCount_ = detector.detect(f, toc_, kMaxTocEntries);
            f.close();
            Serial.printf("[vink3][book] TOC detected: %d entries for %s\n", tocCount_, title_);
            saveTocCache();
        }
    }
    hasProgress_ = loadProgress();
    showingBookEntry_ = true;
    return true;
}

void ReaderBookService::renderOpenOrHelp() {
    if (!open_) {
        renderLibraryPage(bookPage_);
        return;
    }
    renderCurrent();
}

void ReaderBookService::renderLibraryPage(uint16_t page) {
    scanBooks();
    char body[900];
    body[0] = '\0';
    if (bookCount_ <= 0) {
        g_readerText.renderTextPage(
            "书架为空",
            "请把 .txt 文件放到 SD 卡 /books 目录。\n"
            "v0.3 会自动识别 UTF-8 / GBK 文本、生成目录缓存，然后进入正文阅读。",
            1,
            1);
        return;
    }
    const uint16_t totalPages = (bookCount_ + kBooksPerPage - 1) / kBooksPerPage;
    if (page >= totalPages) page = totalPages - 1;
    bookPage_ = page;
    const int start = bookPage_ * kBooksPerPage;
    const int end = min(bookCount_, start + kBooksPerPage);
    size_t used = 0;
    used += snprintf(body + used, sizeof(body) - used, "共 %d 本 TXT · *当前 · 读/目/页=进度/目录/页表\n", bookCount_);
    for (int i = start; i < end && used < sizeof(body) - 96; ++i) {
        const bool current = open_ && strcmp(bookPaths_[i], bookPath_) == 0;
        char titleBuf[56];
        strlcpy(titleBuf, bookTitles_[i], sizeof(titleBuf));
        trimUtf8Tail(titleBuf, strlen(titleBuf));
        char flags[16];
        snprintf(flags, sizeof(flags), "%s%s%s",
                 (bookFlags_[i] & kBookHasProgress) ? "读" : "-",
                 (bookFlags_[i] & kBookHasTocCache) ? "目" : "-",
                 (bookFlags_[i] & kBookHasPageCache) ? "页" : "-");
        used += snprintf(body + used, sizeof(body) - used, "%c%03d [%s] %s\n", current ? '*' : ' ', i + 1, flags, titleBuf);
    }
    g_readerText.renderTextPage("书架", body, bookPage_ + 1, totalPages);
}

bool ReaderBookService::nextLibraryPage() {
    if (!booksScanned_) scanBooks();
    if (bookCount_ <= 0) return false;
    const uint16_t totalPages = (bookCount_ + kBooksPerPage - 1) / kBooksPerPage;
    if (bookPage_ + 1 >= totalPages) return false;
    bookPage_++;
    renderLibraryPage(bookPage_);
    return true;
}

bool ReaderBookService::prevLibraryPage() {
    if (bookCount_ <= 0 || bookPage_ == 0) return false;
    bookPage_--;
    renderLibraryPage(bookPage_);
    return true;
}

bool ReaderBookService::handleLibraryTap(int16_t x, int16_t y) {
    (void)x;
    if (!booksScanned_) scanBooks();
    if (bookCount_ <= 0) return false;
    if (y < kListFirstRowY || y >= kListFirstRowY + kBooksPerPage * kListRowH) return false;
    int row = (y - kListFirstRowY) / kListRowH;
    int index = bookPage_ * kBooksPerPage + row;
    if (index < 0 || index >= bookCount_) return false;
    return openBook(bookPaths_[index]);
}

void ReaderBookService::renderCurrent() {
    if (!open_) {
        renderOpenOrHelp();
        return;
    }
    if (showingBookEntry_) {
        renderBookEntryPage();
        return;
    }
    if (showingToc_) {
        renderTocPage(tocPage_);
        return;
    }
    if (pageCount_ > 0) {
        if (!renderCurrentReadingPage()) renderTocPage(tocPage_);
        return;
    }
    if (!renderChapterPreview(currentTocIndex_)) {
        renderTocPage(tocPage_);
    }
}

void ReaderBookService::renderBookEntryPage() {
    if (!open_) {
        renderOpenOrHelp();
        return;
    }
    char body[900];
    char progress[160];
    const char* chapterTitle = (currentTocIndex_ >= 0 && currentTocIndex_ < tocCount_) ? toc_[currentTocIndex_].title.c_str() : "尚未开始";
    if (hasProgress_) {
        snprintf(progress, sizeof(progress), "%s · 第 %d 页", chapterTitle, currentPage_ + 1);
    } else {
        strlcpy(progress, "无", sizeof(progress));
    }
    snprintf(body, sizeof(body),
             "书籍：%s\n"
             "进度：%s\n\n"
             "继续阅读\n"
             "目录\n"
             "从头开始\n\n"
             "提示：点选操作；阅读中左右/上下滑动翻页。",
             title_,
             progress);
    g_readerText.renderTextPage("书籍入口", body, 1, 1);
}

bool ReaderBookService::continueReading() {
    showingBookEntry_ = false;
    showingToc_ = false;
    if (hasProgress_ && currentTocIndex_ >= 0 && pageCount_ > 0) {
        return renderCurrentReadingPage();
    }
    if (tocCount_ > 0) return openTocEntry(0);
    showingToc_ = true;
    renderTocPage(0);
    return true;
}

bool ReaderBookService::restartReading() {
    showingBookEntry_ = false;
    showingToc_ = false;
    hasProgress_ = false;
    currentPage_ = 0;
    currentTocIndex_ = -1;
    if (tocCount_ > 0) return openTocEntry(0);
    showingToc_ = true;
    renderTocPage(0);
    return true;
}

bool ReaderBookService::nextPage() {
    if (showingBookEntry_) return continueReading();
    if (showingToc_) return nextTocPage();
    if (!open_ || pageCount_ <= 0) return false;
    if (currentPage_ + 1 < pageCount_) {
        currentPage_++;
        return renderCurrentReadingPage();
    }
    if (currentTocIndex_ + 1 < tocCount_) {
        return openTocEntry(currentTocIndex_ + 1);
    }
    return false;
}

bool ReaderBookService::prevPage() {
    if (showingBookEntry_) return false;
    if (showingToc_) return prevTocPage();
    if (!open_ || pageCount_ <= 0) return false;
    if (currentPage_ > 0) {
        currentPage_--;
        return renderCurrentReadingPage();
    }
    if (currentTocIndex_ > 0 && buildChapterPages(currentTocIndex_ - 1)) {
        currentTocIndex_--;
        currentPage_ = max(0, pageCount_ - 1);
        showingToc_ = false;
        return renderCurrentReadingPage();
    }
    return false;
}

bool ReaderBookService::nextTocPage() {
    if (!open_ || !showingToc_ || tocCount_ <= 0) return false;
    const uint16_t totalPages = (tocCount_ + kTocEntriesPerPage - 1) / kTocEntriesPerPage;
    if (tocPage_ + 1 >= totalPages) return false;
    tocPage_++;
    renderTocPage(tocPage_);
    return true;
}

bool ReaderBookService::prevTocPage() {
    if (!open_ || !showingToc_ || tocCount_ <= 0 || tocPage_ == 0) return false;
    tocPage_--;
    renderTocPage(tocPage_);
    return true;
}

bool ReaderBookService::handleTap(int16_t x, int16_t y) {
    if (!open_) return false;
    if (showingBookEntry_) {
        if (y >= 190 && y < 270) return continueReading();
        if (y >= 300 && y < 380) {
            showingBookEntry_ = false;
            showingToc_ = true;
            renderTocPage(tocPage_);
            return true;
        }
        if (y >= 410 && y < 490) return restartReading();
        return false;
    }
    if (!showingToc_) {
        // Tap upper-left area to return to the book entry while reading.
        if (x < 170 && y < 90) {
            showingBookEntry_ = true;
            showingToc_ = false;
            renderBookEntryPage();
            return true;
        }
        if (x < 210 && y >= 90 && y < 150) {
            showingToc_ = true;
            renderTocPage(tocPage_);
            return true;
        }
        return false;
    }
    if (tocCount_ <= 0) return false;
    if (y < kTocFirstRowY || y >= kTocFirstRowY + kTocEntriesPerPage * kTocRowH) return false;
    int row = (y - kTocFirstRowY) / kTocRowH;
    int index = tocPage_ * kTocEntriesPerPage + row;
    if (index < 0 || index >= tocCount_) return false;
    return openTocEntry(index);
}

bool ReaderBookService::openTocEntry(int index) {
    if (index < 0 || index >= tocCount_) return false;
    currentTocIndex_ = index;
    currentPage_ = 0;
    showingToc_ = false;
    if (!buildChapterPages(index)) return renderChapterPreview(index);
    return renderCurrentReadingPage();
}

void ReaderBookService::renderTocPage(uint16_t page) {
    if (!open_) {
        renderOpenOrHelp();
        return;
    }
    char body[900];
    body[0] = '\0';
    if (tocCount_ <= 0) {
        snprintf(body, sizeof(body), "已打开：%s\n未识别到目录。下一步将直接进入正文分页。", title_);
        g_readerText.renderTextPage(title_, body, 1, 1);
        return;
    }
    const int totalPages = (tocCount_ + kTocEntriesPerPage - 1) / kTocEntriesPerPage;
    if (page >= totalPages) page = totalPages - 1;
    tocPage_ = page;
    showingToc_ = true;
    const int start = page * kTocEntriesPerPage;
    const int end = min(tocCount_, start + kTocEntriesPerPage);
    size_t used = 0;
    used += snprintf(body + used, sizeof(body) - used, "目录共 %d 条 · *为当前章节\n", tocCount_);
    for (int i = start; i < end && used < sizeof(body) - 96; ++i) {
        const char marker = (i == currentTocIndex_) ? '*' : ' ';
        used += snprintf(body + used, sizeof(body) - used, "%c%03d  %s\n", marker, i + 1, toc_[i].title.c_str());
    }
    g_readerText.renderTextPage(title_, body, page + 1, totalPages);
}

size_t ReaderBookService::trimUtf8Tail(char* text, size_t len) const {
    while (len > 0) {
        uint8_t c = static_cast<uint8_t>(text[len - 1]);
        if ((c & 0x80) == 0) break;
        if ((c & 0xC0) == 0x80) {
            len--;
            continue;
        }
        // Drop an incomplete lead byte at the end.
        len--;
        break;
    }
    text[len] = '\0';
    return len;
}

uint32_t ReaderBookService::chapterContentStart(int index) {
    if (index < 0 || index >= tocCount_ || !activeTextPath_[0]) return 0;
    File f = SD.open(activeTextPath_, FILE_READ);
    if (!f) return toc_[index].charOffset;
    uint32_t start = toc_[index].charOffset;
    if (!f.seek(start)) {
        f.close();
        return start;
    }
    while (f.available()) {
        char c = f.read();
        start++;
        if (c == '\n') break;
    }
    while (f.available()) {
        int c = f.peek();
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') {
            f.read();
            start++;
            continue;
        }
        break;
    }
    f.close();
    return start;
}

uint32_t ReaderBookService::chapterEndOffset(int index) {
    if (index + 1 < tocCount_) return toc_[index + 1].charOffset;
    File f = SD.open(activeTextPath_, FILE_READ);
    if (!f) return 0;
    uint32_t size = f.size();
    f.close();
    return size;
}

bool ReaderBookService::buildChapterPages(int index) {
    if (index < 0 || index >= tocCount_ || !activeTextPath_[0] || !pageStarts_) return false;
    const uint32_t start = chapterContentStart(index);
    const uint32_t end = chapterEndOffset(index);
    if (end <= start) return false;
    if (loadChapterPageCache(index, start, end)) return true;
    File f = SD.open(activeTextPath_, FILE_READ);
    if (!f) return false;

    uint32_t offset = start;
    pageCount_ = 0;
    while (offset < end && pageCount_ < kMaxChapterPages) {
        pageStarts_[pageCount_++] = offset;
        if (!f.seek(offset)) break;
        const uint32_t toRead = min<uint32_t>(4095, end - offset);
        char buf[4096];
        int n = f.read(reinterpret_cast<uint8_t*>(buf), toRead);
        if (n <= 0) break;
        size_t len = trimUtf8Tail(buf, static_cast<size_t>(n));
        size_t consumed = g_readerText.measurePageBytes(buf, len);
        if (consumed == 0) consumed = len;
        if (consumed == 0) break;
        offset += consumed;
    }
    f.close();
    currentPage_ = 0;
    Serial.printf("[vink3][book] chapter pages built: toc=%d pages=%d\n", index, pageCount_);
    if (pageCount_ > 0) saveChapterPageCache(index, start, end);
    return pageCount_ > 0;
}

bool ReaderBookService::renderCurrentReadingPage() {
    if (currentTocIndex_ < 0 || currentTocIndex_ >= tocCount_ || pageCount_ <= 0 || !pageStarts_) return false;
    const uint32_t start = pageStarts_[currentPage_];
    const uint32_t end = (currentPage_ + 1 < pageCount_) ? pageStarts_[currentPage_ + 1] : chapterEndOffset(currentTocIndex_);
    if (end <= start) return false;
    File f = SD.open(activeTextPath_, FILE_READ);
    if (!f || !f.seek(start)) {
        if (f) f.close();
        return false;
    }
    const uint32_t toRead = min<uint32_t>(4095, end - start);
    char body[4096];
    int n = f.read(reinterpret_cast<uint8_t*>(body), toRead);
    f.close();
    if (n <= 0) return false;
    trimUtf8Tail(body, static_cast<size_t>(n));
    char header[96];
    snprintf(header, sizeof(header), "%03d %s", currentTocIndex_ + 1, toc_[currentTocIndex_].title.c_str());
    g_readerText.renderTextPage(header, body, currentPage_ + 1, pageCount_);
    saveProgress();
    return true;
}

bool ReaderBookService::renderChapterPreview(int index) {
    if (index < 0 || index >= tocCount_ || !activeTextPath_[0]) return false;
    File f = SD.open(activeTextPath_, FILE_READ);
    if (!f) return false;
    uint32_t start = toc_[index].charOffset;
    if (!f.seek(start)) {
        f.close();
        return false;
    }

    char body[2300];
    int n = f.read(reinterpret_cast<uint8_t*>(body), sizeof(body) - 1);
    f.close();
    if (n <= 0) return false;
    size_t len = trimUtf8Tail(body, static_cast<size_t>(n));

    // Skip the chapter title line itself; title is already shown in the header.
    char* content = body;
    while (*content && *content != '\n') content++;
    while (*content == '\n' || *content == '\r') content++;
    while (content[0] == static_cast<char>(0xE3) &&
           content[1] == static_cast<char>(0x80) &&
           content[2] == static_cast<char>(0x80)) {
        content += 3;
    }
    (void)len;

    char header[96];
    snprintf(header, sizeof(header), "%03d %s", index + 1, toc_[index].title.c_str());
    g_readerText.renderTextPage(header, content, 1, 1);
    return true;
}

} // namespace vink3
