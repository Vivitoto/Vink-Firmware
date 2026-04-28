#include "ReaderBookService.h"
#include "ReaderTextRenderer.h"
#include "../display/DisplayService.h"
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
    sortBooks();
    booksScanned_ = true;
    if (bookPage_ * kBooksPerPage >= bookCount_) bookPage_ = 0;
    Serial.printf("[vink3][book] library scan: %d TXT books\n", bookCount_);
    return true;
}

void ReaderBookService::sortBooks() {
    if (bookCount_ <= 1 || !bookPaths_ || !bookTitles_ || !bookFlags_) return;
    // SD directory iteration order can vary by card/write history. Keep the
    // bookshelf stable across boots by sorting on the visible title/path.
    for (int i = 1; i < bookCount_; ++i) {
        int j = i;
        while (j > 0) {
            int cmp = strcmp(bookTitles_[j - 1], bookTitles_[j]);
            if (cmp == 0) cmp = strcmp(bookPaths_[j - 1], bookPaths_[j]);
            if (cmp <= 0) break;
            swapBookEntries(j - 1, j);
            --j;
        }
    }
}

void ReaderBookService::swapBookEntries(int a, int b) {
    if (a == b || a < 0 || b < 0 || a >= bookCount_ || b >= bookCount_) return;
    char pathTmp[160];
    char titleTmp[72];
    strlcpy(pathTmp, bookPaths_[a], sizeof(pathTmp));
    strlcpy(titleTmp, bookTitles_[a], sizeof(titleTmp));
    uint8_t flagsTmp = bookFlags_[a];
    strlcpy(bookPaths_[a], bookPaths_[b], sizeof(bookPaths_[a]));
    strlcpy(bookTitles_[a], bookTitles_[b], sizeof(bookTitles_[a]));
    bookFlags_[a] = bookFlags_[b];
    strlcpy(bookPaths_[b], pathTmp, sizeof(bookPaths_[b]));
    strlcpy(bookTitles_[b], titleTmp, sizeof(bookTitles_[b]));
    bookFlags_[b] = flagsTmp;
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

uint32_t ReaderBookService::bookFileSize(const char* bookPath) const {
    if (!bookPath || !bookPath[0]) return 0;
    File f = SD.open(bookPath, FILE_READ);
    if (!f) return 0;
    uint32_t size = f.size();
    f.close();
    return size;
}

void ReaderBookService::formatBytes(uint32_t bytes, char* out, size_t len) const {
    if (!out || len == 0) return;
    if (bytes >= 1024UL * 1024UL) {
        snprintf(out, len, "%lu.%lu MB", static_cast<unsigned long>(bytes / (1024UL * 1024UL)),
                 static_cast<unsigned long>((bytes % (1024UL * 1024UL)) * 10UL / (1024UL * 1024UL)));
    } else if (bytes >= 1024UL) {
        snprintf(out, len, "%lu.%lu KB", static_cast<unsigned long>(bytes / 1024UL),
                 static_cast<unsigned long>((bytes % 1024UL) * 10UL / 1024UL));
    } else {
        snprintf(out, len, "%lu B", static_cast<unsigned long>(bytes));
    }
}

void ReaderBookService::formatBookFlags(uint8_t flags, char* out, size_t len) const {
    if (!out || len == 0) return;
    snprintf(out, len, "%s%s%s",
             (flags & kBookHasProgress) ? "读" : "-",
             (flags & kBookHasTocCache) ? "目" : "-",
             (flags & kBookHasPageCache) ? "页" : "-");
}

void ReaderBookService::showBlockingOpenStatus(const char* stage) {
    renderBookLoadingPage(stage);
    g_displayService.enqueueFull(false, 100);
    // Give the display task a chance to push the status page before a large TXT
    // scan/conversion monopolizes the reader flow. If the display service is not
    // ready yet, waitIdle simply times out and opening continues normally.
    g_displayService.waitIdle(2500);
}

void ReaderBookService::showBlockingChapterStatus(int index) {
    renderChapterLoadingPage(index);
    g_displayService.enqueueFull(false, 100);
    // Same rationale as open status: make first-time chapter pagination visible
    // before doing the synchronous page-fit scan.
    g_displayService.waitIdle(2500);
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
        showBlockingOpenStatus("正在转码为 UTF-8");
        String tmp = TextCodec::convertToUTF8(path);
        if (tmp.length() > 0) strlcpy(activeTextPath_, tmp.c_str(), sizeof(activeTextPath_));
    }

    open_ = true;
    if (!loadTocCache()) {
        showBlockingOpenStatus("正在分析目录");
        File f = SD.open(activeTextPath_, FILE_READ);
        if (f) {
            ChapterDetector detector;
            tocCount_ = detector.detect(f, toc_, kMaxTocEntries);
            f.close();
            if (tocCount_ <= 0 && toc_) {
                toc_[0].charOffset = 0;
                toc_[0].chapterNumber = 1;
                toc_[0].score = 50;
                toc_[0].title = String("全文");
                tocCount_ = 1;
                Serial.printf("[vink3][book] no TOC found, using whole-book fallback for %s\n", title_);
            }
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
    char summary[96];
    snprintf(summary, sizeof(summary), "共 %d 本 TXT · *当前 · 读/目/页=进度/目录/页表", bookCount_);
    char rows[kBooksPerPage][96];
    const char* rowPtrs[kBooksPerPage];
    int rowCount = 0;
    for (int i = start; i < end && rowCount < kBooksPerPage; ++i) {
        const bool current = open_ && strcmp(bookPaths_[i], bookPath_) == 0;
        char titleBuf[56];
        strlcpy(titleBuf, bookTitles_[i], sizeof(titleBuf));
        trimUtf8Tail(titleBuf, strlen(titleBuf));
        char flags[16];
        formatBookFlags(bookFlags_[i], flags, sizeof(flags));
        snprintf(rows[rowCount], sizeof(rows[rowCount]), "%c%03d [%s] %s", current ? '*' : ' ', i + 1, flags, titleBuf);
        rowPtrs[rowCount] = rows[rowCount];
        rowCount++;
    }
    g_readerText.renderListPage("书架", summary, rowPtrs, rowCount, kListFirstRowY, kListRowH, bookPage_ + 1, totalPages);
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

void ReaderBookService::renderBookLoadingPage(const char* stage) {
    char body[700];
    char sizeText[24];
    formatBytes(bookFileSize(bookPath_), sizeText, sizeof(sizeText));
    snprintf(body, sizeof(body),
             "书籍：%s\n"
             "大小：%s\n\n"
             "%s...\n\n"
             "首次打开大书可能需要一会儿。\n"
             "完成后会自动进入书籍入口；以后会复用 .vink-toc 缓存。",
             title_[0] ? title_ : "TXT",
             sizeText,
             stage && stage[0] ? stage : "正在打开");
    g_readerText.renderTextPage("正在打开", body, 1, 1);
}

void ReaderBookService::renderChapterLoadingPage(int index) {
    char body[700];
    const char* chapterTitle = (index >= 0 && index < tocCount_) ? toc_[index].title.c_str() : "章节";
    snprintf(body, sizeof(body),
             "书籍：%s\n"
             "章节：%s\n\n"
             "正在分页...\n\n"
             "首次进入长章节时会测量每页可显示的文字量。\n"
             "完成后会缓存到 .vink-pages，之后进入会更快。",
             title_[0] ? title_ : "TXT",
             chapterTitle);
    g_readerText.renderTextPage("正在分页", body, 1, 1);
}

void ReaderBookService::renderBookEntryPage() {
    if (!open_) {
        renderOpenOrHelp();
        return;
    }
    char body[900];
    char progress[160];
    char sizeText[24];
    char flags[16];
    const char* chapterTitle = (currentTocIndex_ >= 0 && currentTocIndex_ < tocCount_) ? toc_[currentTocIndex_].title.c_str() : "尚未开始";
    const uint8_t cacheFlags = detectBookFlags(bookPath_);
    formatBytes(bookFileSize(bookPath_), sizeText, sizeof(sizeText));
    formatBookFlags(cacheFlags, flags, sizeof(flags));
    if (hasProgress_) {
        snprintf(progress, sizeof(progress), "%s · 第 %d 页", chapterTitle, currentPage_ + 1);
    } else {
        strlcpy(progress, "无", sizeof(progress));
    }
    char lineTitle[180];
    char lineSize[48];
    char lineToc[48];
    char lineCache[96];
    char lineProgress[180];
    snprintf(lineTitle, sizeof(lineTitle), "书籍：%s", title_);
    snprintf(lineSize, sizeof(lineSize), "大小：%s", sizeText);
    snprintf(lineToc, sizeof(lineToc), "目录：%d 条", tocCount_);
    snprintf(lineCache, sizeof(lineCache), "缓存：[%s] 读/目/页=进度/目录/页表", flags);
    snprintf(lineProgress, sizeof(lineProgress), "进度：%s", progress);
    const char* info[] = {lineTitle, lineSize, lineToc, lineCache, lineProgress, "提示：阅读中左右/上下滑动翻页"};
    const char* actions[] = {"继续阅读", "目录", "从头开始"};
    g_readerText.renderActionPage("书籍入口", info, 6, actions, 3);
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
        if (x >= kEntryButtonX && x < kEntryButtonX + kEntryButtonW && y >= kEntryContinueY && y < kEntryContinueY + kEntryButtonH) return continueReading();
        if (x >= kEntryButtonX && x < kEntryButtonX + kEntryButtonW && y >= kEntryTocY && y < kEntryTocY + kEntryButtonH) {
            showingBookEntry_ = false;
            showingToc_ = true;
            renderTocPage(tocPage_);
            return true;
        }
        if (x >= kEntryButtonX && x < kEntryButtonX + kEntryButtonW && y >= kEntryRestartY && y < kEntryRestartY + kEntryButtonH) return restartReading();
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
    char summary[64];
    snprintf(summary, sizeof(summary), "目录共 %d 条 · *为当前章节", tocCount_);
    char rows[kTocEntriesPerPage][128];
    const char* rowPtrs[kTocEntriesPerPage];
    int rowCount = 0;
    for (int i = start; i < end && rowCount < kTocEntriesPerPage; ++i) {
        const char marker = (i == currentTocIndex_) ? '*' : ' ';
        char titleBuf[92];
        strlcpy(titleBuf, toc_[i].title.c_str(), sizeof(titleBuf));
        trimUtf8Tail(titleBuf, strlen(titleBuf));
        snprintf(rows[rowCount], sizeof(rows[rowCount]), "%c%03d  %s", marker, i + 1, titleBuf);
        rowPtrs[rowCount] = rows[rowCount];
        rowCount++;
    }
    g_readerText.renderListPage(title_, summary, rowPtrs, rowCount, kTocFirstRowY, kTocRowH, page + 1, totalPages);
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
    if (toc_[index].title == "全文" && start == 0) {
        // Whole-book fallback has no chapter heading to skip. Drop UTF-8 BOM only.
        if (f.available() >= 3) {
            uint8_t bom[3] = {0};
            f.read(bom, sizeof(bom));
            if (bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF) start = 3;
        }
        f.close();
        return start;
    }
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
    showBlockingChapterStatus(index);
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
