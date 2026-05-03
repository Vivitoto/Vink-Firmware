#include "ReaderBookService.h"
#include "ReaderTextRenderer.h"
#include "../display/DisplayService.h"
#include "../config/ConfigService.h"
#include "../ui/VinkUiRenderer.h"
#include "../text/CjkTextRenderer.h"
#include "../sync/LegadoService.h"
#include "../ReadPaper176.h"
#include "../../Config.h"
#include "../../TextCodec.h"
#include <SPI.h>
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
    // Keep boot non-blocking: SD is initialized lazily when the user opens the
    // library/book path. Previous PaperS3 releases showed that SD work during
    // startup can make the device look stuck if the card is absent or slow.
    return true;
}

bool ReaderBookService::ensureSdReady() {
    if (sdReady_) return true;

    // Official PaperS3 microSD wiring from M5Stack docs:
    // CS=G47, SCK=G39, MOSI=G38, MISO=G40. Do not rely on board defaults here;
    // explicit pins keep Vink aligned with the product manual and factory demo.
    SPI.begin(kSdSckPin, kSdMisoPin, kSdMosiPin, kSdCsPin);
    const uint32_t freqs[] = {kSdPrimaryFrequency, kSdFallbackFrequency1, kSdFallbackFrequency2};
    for (uint32_t freq : freqs) {
        Serial.printf("[vink3][book] SD init CS=%d SCK=%d MOSI=%d MISO=%d freq=%lu\n",
                      kSdCsPin, kSdSckPin, kSdMosiPin, kSdMisoPin, static_cast<unsigned long>(freq));
        if (SD.begin(kSdCsPin, SPI, freq)) {
            sdReady_ = true;
            Serial.printf("[vink3][book] SD ready at %lu Hz\n", static_cast<unsigned long>(freq));
            break;
        }
        delay(50);
    }

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
    pageWindowStart_ = 0;
    pageWindowEnd_ = 0;
    pageWindowTruncated_ = false;
    hasProgress_ = false;
    showingBookEntry_ = false;
    showingToc_ = true;
    showingEndOfBook_ = false;
    showingBookmarks_ = false;
    showingReaderMenu_ = false;
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

int ReaderBookService::readBookProgressPercent(const char* bookPath, uint16_t& outChapter, uint16_t& outPage) const {
    if (!bookPath || !bookPath[0]) return -1;
    char sidecar[160];
    getSidecarPathForBook(sidecar, sizeof(sidecar), bookPath, ".vink-progress");
    if (!SD.exists(sidecar)) return -1;
    File f = SD.open(sidecar, FILE_READ);
    if (!f) return -1;
    uint32_t magic = 0, cachedSize = 0;
    uint16_t chapter = 0, page = 0;
    f.read(reinterpret_cast<uint8_t*>(&magic), sizeof(magic));
    f.read(reinterpret_cast<uint8_t*>(&cachedSize), sizeof(cachedSize));
    f.read(reinterpret_cast<uint8_t*>(&chapter), sizeof(chapter));
    f.read(reinterpret_cast<uint8_t*>(&page), sizeof(page));
    f.close();
    if ((magic != 0x56505232UL && magic != 0x56505233UL)) return -1;
    // Load TOC to get chapter count for progress calculation
    char tocPath[160];
    getSidecarPathForBook(tocPath, sizeof(tocPath), bookPath, ".vink-toc");
    if (!SD.exists(tocPath)) return -1;
    File tf = SD.open(tocPath, FILE_READ);
    if (!tf) return -1;
    uint32_t tMagic = 0, tSize = 0, tVer = 0;
    uint16_t tCount = 0;
    tf.read(reinterpret_cast<uint8_t*>(&tMagic), sizeof(tMagic));
    tf.read(reinterpret_cast<uint8_t*>(&tSize), sizeof(tSize));
    tf.read(reinterpret_cast<uint8_t*>(&tVer), sizeof(tVer));
    tf.read(reinterpret_cast<uint8_t*>(&tCount), sizeof(tCount));
    tf.close();
    if (tMagic != 0x56435433UL || tCount == 0) return -1;
    outChapter = chapter;
    outPage = page;
    // Progress: how far through the book (chapter + relative page within chapter)
    // We approximate as: (chapter / totalChapters) * 100
    return min(100, (int)((uint32_t)chapter * 100 / tCount));
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
    char cachePath[192];
    getTocCachePath(cachePath, sizeof(cachePath));
    File f = SD.open(cachePath, FILE_READ);
    if (!f) return false;
    uint32_t magic = 0;
    uint32_t cachedSize = 0;
    uint32_t detectorVersion = 0;
    uint16_t count = 0;
    f.read(reinterpret_cast<uint8_t*>(&magic), sizeof(magic));
    f.read(reinterpret_cast<uint8_t*>(&cachedSize), sizeof(cachedSize));
    f.read(reinterpret_cast<uint8_t*>(&detectorVersion), sizeof(detectorVersion));
    f.read(reinterpret_cast<uint8_t*>(&count), sizeof(count));
    if (magic != 0x56435433UL || cachedSize != activeTextSize() || detectorVersion != 20260502UL || count == 0 || count > kMaxTocEntries) {
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
    char cachePath[192];
    getTocCachePath(cachePath, sizeof(cachePath));
    File f = SD.open(cachePath, FILE_WRITE);
    if (!f) return;
    uint32_t magic = 0x56435433UL; // VCT3: byte-accurate offsets + detector-version guarded
    uint32_t fileSize = activeTextSize();
    uint32_t detectorVersion = 20260502UL;
    uint16_t count = static_cast<uint16_t>(tocCount_);
    f.write(reinterpret_cast<const uint8_t*>(&magic), sizeof(magic));
    f.write(reinterpret_cast<const uint8_t*>(&fileSize), sizeof(fileSize));
    f.write(reinterpret_cast<const uint8_t*>(&detectorVersion), sizeof(detectorVersion));
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
    char path[192];
    getProgressPath(path, sizeof(path));
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    uint32_t magic = 0;
    uint32_t cachedSize = 0;
    uint16_t chapter = 0;
    uint16_t page = 0;
    uint32_t savedOffset = 0;
    f.read(reinterpret_cast<uint8_t*>(&magic), sizeof(magic));
    f.read(reinterpret_cast<uint8_t*>(&cachedSize), sizeof(cachedSize));
    f.read(reinterpret_cast<uint8_t*>(&chapter), sizeof(chapter));
    f.read(reinterpret_cast<uint8_t*>(&page), sizeof(page));
    if (magic == 0x56505233UL) { // VPR3: includes current page start offset
        f.read(reinterpret_cast<uint8_t*>(&savedOffset), sizeof(savedOffset));
    }
    f.close();
    if ((magic != 0x56505232UL && magic != 0x56505233UL) || cachedSize != activeTextSize() || chapter >= tocCount_) return false;
    if (!buildChapterPages(chapter)) return false;
    currentTocIndex_ = chapter;
    currentPage_ = page < pageCount_ ? page : 0;
    if (savedOffset > 0 && pageStarts_ && pageCount_ > 0) {
        int nearest = 0;
        for (int i = 0; i < pageCount_; ++i) {
            if (pageStarts_[i] > savedOffset) break;
            nearest = i;
        }
        currentPage_ = nearest;
    }
    hasProgress_ = true;
    showingToc_ = false;
    Serial.printf("[vink3][book] progress loaded: chapter=%u page=%u\n", chapter, page);
    return true;
}

void ReaderBookService::saveProgress() {
    if (!bookPath_[0] || currentTocIndex_ < 0 || !ensureSdReady()) return;
    char path[192];
    getProgressPath(path, sizeof(path));
    File f = SD.open(path, FILE_WRITE);
    if (!f) return;
    uint32_t magic = 0x56505233UL; // VPR3: includes page start offset for layout-resilient restore
    uint32_t fileSize = activeTextSize();
    uint16_t chapter = static_cast<uint16_t>(currentTocIndex_);
    uint16_t page = static_cast<uint16_t>(max(0, currentPage_));
    uint32_t pageOffset = (pageStarts_ && currentPage_ >= 0 && currentPage_ < pageCount_) ? pageStarts_[currentPage_] : 0;
    f.write(reinterpret_cast<const uint8_t*>(&magic), sizeof(magic));
    f.write(reinterpret_cast<const uint8_t*>(&fileSize), sizeof(fileSize));
    f.write(reinterpret_cast<const uint8_t*>(&chapter), sizeof(chapter));
    f.write(reinterpret_cast<const uint8_t*>(&page), sizeof(page));
    f.write(reinterpret_cast<const uint8_t*>(&pageOffset), sizeof(pageOffset));
    f.close();
    // Also sync to Legado if configured
    syncProgressToLegado();
}

void ReaderBookService::syncProgressToLegado() {
    if (!open_ || !g_legadoService.isConfigured()) return;
    if (!g_configService.get().legadoEnabled) return;
    BookProgress bp;
    // Legado official /saveBookProgress matches by BookProgress.name + author.
    // Local TXT imports commonly have an empty author; keep Vink's title as the
    // book name and an empty author unless a later metadata parser fills it.
    bp.name = title_;
    bp.author = "";
    bp.durChapterIndex = currentTocIndex_;
    bp.durChapterPos = currentPage_;
    bp.durChapterTime = millis();
    if (currentTocIndex_ >= 0 && currentTocIndex_ < tocCount_) {
        bp.durChapterTitle = toc_[currentTocIndex_].title.c_str();
    }
    if (!g_legadoService.saveBookProgress(bp)) {
        Serial.printf("[vink3][book] legado sync save failed: %s\n", g_legadoService.lastError().c_str());
    } else {
        Serial.printf("[vink3][book] progress synced to legado: ch=%d pg=%d\n", currentTocIndex_, currentPage_);
    }
}

void ReaderBookService::syncProgressFromLegado() {
    if (!open_ || !g_legadoService.isConfigured()) return;
    if (!g_configService.get().legadoEnabled) return;
    BookProgress bp;
    if (!g_legadoService.fetchBookProgress(title_, "", bp)) return;
    // Only apply if Legado has a more advanced chapter/page. Legado Web API has
    // no standalone getBookProgress endpoint; this value comes from /getBookshelf.
    if (bp.durChapterIndex <= 0 && bp.durChapterPos <= 0) return;
    if (bp.durChapterIndex >= tocCount_) return;
    if (bp.durChapterIndex > currentTocIndex_ ||
        (bp.durChapterIndex == currentTocIndex_ && bp.durChapterPos > currentPage_)) {
        if (!buildChapterPages(bp.durChapterIndex)) return;
        currentTocIndex_ = bp.durChapterIndex;
        currentPage_ = bp.durChapterPos < pageCount_ ? bp.durChapterPos : 0;
        hasProgress_ = true;
        showingToc_ = false;
        Serial.printf("[vink3][book] progress restored from legado: ch=%d pg=%d\n", currentTocIndex_, currentPage_);
    }
}

void ReaderBookService::getBookmarkSidecarPath(char* out, size_t len) const {
    getSidecarPath(out, len, ".vink-bookmarks");
}

bool ReaderBookService::loadBookmarks() {
    if (!bookPath_[0] || !ensureSdReady()) return false;
    bookmarkCount_ = 0;
    char path[192];
    getBookmarkSidecarPath(path, sizeof(path));
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    uint32_t magic = 0, fileSize = 0;
    uint16_t count = 0;
    f.read(reinterpret_cast<uint8_t*>(&magic), sizeof(magic));
    f.read(reinterpret_cast<uint8_t*>(&fileSize), sizeof(fileSize));
    f.read(reinterpret_cast<uint8_t*>(&count), sizeof(count));
    if (magic != 0x56424B33UL || count > kMaxBookmarks) { f.close(); return false; }
    // Validate against current book size
    if (fileSize != activeTextSize()) { f.close(); return false; }
    int n = min<int>(count, kMaxBookmarks);
    for (int i = 0; i < n; ++i) {
        f.read(reinterpret_cast<uint8_t*>(&bookmarks_[i].chapter), sizeof(bookmarks_[i].chapter));
        f.read(reinterpret_cast<uint8_t*>(&bookmarks_[i].page), sizeof(bookmarks_[i].page));
        f.read(reinterpret_cast<uint8_t*>(&bookmarks_[i].offset), sizeof(bookmarks_[i].offset));
        uint8_t exLen = 0;
        f.read(&exLen, 1);
        if (exLen > 0) {
            size_t r = f.read(reinterpret_cast<uint8_t*>(bookmarks_[i].excerpt), min<uint8_t>(exLen, sizeof(bookmarks_[i].excerpt) - 1));
            bookmarks_[i].excerpt[r] = '\0';
        } else {
            bookmarks_[i].excerpt[0] = '\0';
        }
        f.read(reinterpret_cast<uint8_t*>(&bookmarks_[i].createdAt), sizeof(bookmarks_[i].createdAt));
    }
    f.close();
    bookmarkCount_ = n;
    Serial.printf("[vink3][book] bookmarks loaded: %d\n", bookmarkCount_);
    return true;
}

void ReaderBookService::saveBookmarks() {
    if (!bookPath_[0] || !ensureSdReady() || bookmarkCount_ <= 0) return;
    char path[192];
    getBookmarkSidecarPath(path, sizeof(path));
    File f = SD.open(path, FILE_WRITE);
    if (!f) return;
    uint32_t magic = 0x56424B33UL; // VBK3
    uint32_t fileSize = activeTextSize();
    uint16_t count = static_cast<uint16_t>(bookmarkCount_);
    f.write(reinterpret_cast<const uint8_t*>(&magic), sizeof(magic));
    f.write(reinterpret_cast<const uint8_t*>(&fileSize), sizeof(fileSize));
    f.write(reinterpret_cast<const uint8_t*>(&count), sizeof(count));
    for (int i = 0; i < bookmarkCount_; ++i) {
        f.write(reinterpret_cast<const uint8_t*>(&bookmarks_[i].chapter), sizeof(bookmarks_[i].chapter));
        f.write(reinterpret_cast<const uint8_t*>(&bookmarks_[i].page), sizeof(bookmarks_[i].page));
        f.write(reinterpret_cast<const uint8_t*>(&bookmarks_[i].offset), sizeof(bookmarks_[i].offset));
        uint8_t exLen = strlen(bookmarks_[i].excerpt);
        f.write(&exLen, 1);
        if (exLen > 0) f.write(reinterpret_cast<const uint8_t*>(bookmarks_[i].excerpt), exLen);
        f.write(reinterpret_cast<const uint8_t*>(&bookmarks_[i].createdAt), sizeof(bookmarks_[i].createdAt));
    }
    f.close();
    Serial.printf("[vink3][book] bookmarks saved: %d\n", bookmarkCount_);
}

bool ReaderBookService::addBookmarkAtCurrent() {
    if (!open_ || bookmarkCount_ >= kMaxBookmarks) return false;
    Bookmark& b = bookmarks_[bookmarkCount_];
    b.chapter = static_cast<uint16_t>(max(0, currentTocIndex_));
    b.page = static_cast<uint16_t>(currentPage_);
    b.offset = (pageStarts_ && currentPage_ < pageCount_) ? pageStarts_[currentPage_] : 0;
    b.createdAt = millis();
    // Extract excerpt: up to 60 chars from current page text
    b.excerpt[0] = '\0';
    if (pageStarts_ && currentPage_ < pageCount_) {
        File fh = SD.open(bookPath_, FILE_READ);
        if (fh) {
            uint32_t start = pageStarts_[currentPage_];
            fh.seek(start);
            char buf[64];
            size_t r = fh.read(reinterpret_cast<uint8_t*>(buf), sizeof(buf) - 1);
            buf[r] = '\0';
            // Trim to first line or 60 chars
            buf[min<size_t>(r, 60)] = '\0';
            char* lf = strchr(buf, '\n');
            if (lf) *lf = '\0';
            strlcpy(b.excerpt, buf, sizeof(b.excerpt));
            fh.close();
        }
    }
    bookmarkCount_++;
    saveBookmarks();
    Serial.printf("[vink3][book] bookmark added: ch=%d pg=%d\n", b.chapter, b.page);
    return true;
}

void ReaderBookService::renderBookmarkPage(uint16_t page) {
    if (!open_) { renderOpenOrHelp(); return; }
    if (bookmarkCount_ == 0) {
        loadBookmarks();
        if (bookmarkCount_ == 0) {
            char body[] = "暂无书签。\n\n阅读时在屏幕中央区域长按可添加书签。\n\n书签会自动保存在书籍同目录下。";
            g_readerText.renderTextPage("书签", body, 1, 1);
            return;
        }
    }
    const uint16_t totalPages = max(1, (bookmarkCount_ + kBookmarkPages - 1) / kBookmarkPages);
    if (page >= totalPages) page = totalPages - 1;
    bookmarkPage_ = page;
    char rows[kBookmarkPages][96];
    const char* rowPtrs[kBookmarkPages];
    int rowCount = 0;
    const int start = bookmarkPage_ * kBookmarkPages;
    const int end = min(bookmarkCount_, start + kBookmarkPages);
    for (int i = start; i < end; ++i) {
        char chapterTitle[40] = "";
        if (bookmarks_[i].chapter < tocCount_) {
            strlcpy(chapterTitle, toc_[bookmarks_[i].chapter].title.c_str(), sizeof(chapterTitle));
        }
        snprintf(rows[rowCount], sizeof(rows[rowCount]), "%s 第%d页 %s",
                 chapterTitle[0] ? chapterTitle : "", bookmarks_[i].page + 1,
                 bookmarks_[i].excerpt[0] ? bookmarks_[i].excerpt : "");
        rowPtrs[rowCount] = rows[rowCount];
        rowCount++;
    }
    char summary[40];
    snprintf(summary, sizeof(summary), "共 %d 个书签", bookmarkCount_);
    g_readerText.renderListPage("书签", summary, rowPtrs, rowCount, kListFirstRowY, kListRowH,
                                bookmarkPage_ + 1, totalPages, 1);
}

bool ReaderBookService::nextBookmarkPage() {
    if (bookmarkCount_ == 0) return false;
    const uint16_t totalPages = (bookmarkCount_ + kBookmarkPages - 1) / kBookmarkPages;
    if (bookmarkPage_ + 1 >= totalPages) return false;
    bookmarkPage_++;
    renderBookmarkPage(bookmarkPage_);
    return true;
}

bool ReaderBookService::prevBookmarkPage() {
    if (bookmarkCount_ == 0 || bookmarkPage_ == 0) return false;
    bookmarkPage_--;
    renderBookmarkPage(bookmarkPage_);
    return true;
}

bool ReaderBookService::handleBookmarkTap(int16_t x, int16_t y) {
    if (!open_ || bookmarkCount_ == 0) return false;
    if (y < kListFirstRowY || y >= kListFirstRowY + kBookmarkPages * kListRowH) return false;
    int row = (y - kListFirstRowY) / kListRowH;
    int index = bookmarkPage_ * kBookmarkPages + row;
    if (index < 0 || index >= bookmarkCount_) return false;
    // Jump to bookmarked position
    int ch = bookmarks_[index].chapter;
    if (ch >= 0 && ch < tocCount_) {
        if (!buildChapterPages(ch)) return false;
        currentTocIndex_ = ch;
        // Find nearest page to saved offset
        uint32_t targetOffset = bookmarks_[index].offset;
        currentPage_ = 0;
        if (pageStarts_ && pageCount_ > 0) {
            for (int i = 0; i < pageCount_; ++i) {
                if (pageStarts_[i] > targetOffset) break;
                currentPage_ = i;
            }
        }
        showingToc_ = false;
        showingBookmarks_ = false;
        return renderCurrentReadingPage();
    }
    return false;
}

uint32_t ReaderBookService::pageLayoutKey() const {
    // Page cache must be tied to the exact PaperS3 text layout. Font size,
    // margins, line spacing, and justification change where page breaks land.
    const auto& cfg = g_configService.get();
    const LayoutConfig lc = g_configService.layout();
    uint32_t h = 2166136261UL; // FNV-1a
    auto mix = [&](uint32_t v) { h ^= v; h *= 16777619UL; };
    mix(cfg.fontSize);
    mix(cfg.lineSpacing);
    mix(lc.marginLeft);
    mix(lc.marginRight);
    mix(lc.marginTop);
    mix(lc.marginBottom);
    mix(lc.indentFirstLine);
    mix(lc.paragraphSpacing);
    mix(cfg.justify ? 1 : 0);
    return h;
}

bool ReaderBookService::loadChapterPageCache(int index, uint32_t start, uint32_t end) {
    if (!bookPath_[0] || !pageStarts_ || !ensureSdReady()) return false;
    char path[192];
    getPageCachePath(path, sizeof(path));
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    uint32_t magic = 0;
    uint32_t cachedSize = 0;
    uint16_t chapter = 0;
    uint16_t count = 0;
    uint32_t cachedStart = 0;
    uint32_t cachedEnd = 0;
    uint32_t cachedLayout = 0;
    f.read(reinterpret_cast<uint8_t*>(&magic), sizeof(magic));
    f.read(reinterpret_cast<uint8_t*>(&cachedSize), sizeof(cachedSize));
    f.read(reinterpret_cast<uint8_t*>(&chapter), sizeof(chapter));
    f.read(reinterpret_cast<uint8_t*>(&count), sizeof(count));
    f.read(reinterpret_cast<uint8_t*>(&cachedStart), sizeof(cachedStart));
    f.read(reinterpret_cast<uint8_t*>(&cachedEnd), sizeof(cachedEnd));
    f.read(reinterpret_cast<uint8_t*>(&cachedLayout), sizeof(cachedLayout));
    if (magic != 0x56504733UL || cachedSize != activeTextSize() || chapter != index || count == 0 || count > kMaxChapterPages || cachedStart != start || cachedEnd != end || cachedLayout != pageLayoutKey()) {
        f.close();
        return false;
    }
    size_t need = static_cast<size_t>(count) * sizeof(uint32_t);
    size_t got = f.read(reinterpret_cast<uint8_t*>(pageStarts_), need);
    f.close();
    if (got != need) return false;
    pageCount_ = count;
    currentPage_ = 0;
    pageWindowStart_ = cachedStart;
    pageWindowEnd_ = cachedEnd;
    pageWindowTruncated_ = false;
    Serial.printf("[vink3][book] page cache loaded: chapter=%d pages=%d\n", index, pageCount_);
    return true;
}

void ReaderBookService::saveChapterPageCache(int index, uint32_t start, uint32_t end) {
    if (!bookPath_[0] || !pageStarts_ || pageCount_ <= 0 || !ensureSdReady()) return;
    char path[192];
    getPageCachePath(path, sizeof(path));
    File f = SD.open(path, FILE_WRITE);
    if (!f) return;
    uint32_t magic = 0x56504733UL; // VPG3: includes layout fingerprint
    uint32_t fileSize = activeTextSize();
    uint16_t chapter = static_cast<uint16_t>(index);
    uint16_t count = static_cast<uint16_t>(pageCount_);
    uint32_t layout = pageLayoutKey();
    f.write(reinterpret_cast<const uint8_t*>(&magic), sizeof(magic));
    f.write(reinterpret_cast<const uint8_t*>(&fileSize), sizeof(fileSize));
    f.write(reinterpret_cast<const uint8_t*>(&chapter), sizeof(chapter));
    f.write(reinterpret_cast<const uint8_t*>(&count), sizeof(count));
    f.write(reinterpret_cast<const uint8_t*>(&start), sizeof(start));
    f.write(reinterpret_cast<const uint8_t*>(&end), sizeof(end));
    f.write(reinterpret_cast<const uint8_t*>(&layout), sizeof(layout));
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
    syncProgressFromLegado();
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
        uint16_t ch = 0, pg = 0;
        int pct = g_readerBook.readBookProgressPercent(bookPaths_[i], ch, pg);
        char pctBuf[24] = "";
        if (pct >= 0) snprintf(pctBuf, sizeof(pctBuf), " %d%%", pct);
        snprintf(rows[rowCount], sizeof(rows[rowCount]), "%c%03d [%s]%s %s",
                 current ? '*' : ' ', i + 1, flags, pctBuf, titleBuf);
        rowPtrs[rowCount] = rows[rowCount];
        rowCount++;
    }
    g_readerText.renderListPage("书架", summary, rowPtrs, rowCount, kListFirstRowY, kListRowH, bookPage_ + 1, totalPages, 1);
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
    if (showingEndOfBook_) {
        renderEndOfBookPage();
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
    g_readerText.renderActionPage("书籍入口", info, 6, actions, 3, 0);
}

bool ReaderBookService::continueReading() {
    showingBookEntry_ = false;
    showingToc_ = false;
    showingEndOfBook_ = false;
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
    showingEndOfBook_ = false;
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
    if (showingEndOfBook_) return renderEndOfBookPage();
    if (!open_ || pageCount_ <= 0) return false;
    if (currentPage_ + 1 < pageCount_) {
        currentPage_++;
        return renderCurrentReadingPage();
    }
    if (pageWindowTruncated_ && pageWindowEnd_ < chapterEndOffset(currentTocIndex_)) {
        return buildChapterPagesFrom(currentTocIndex_, pageWindowEnd_, false) && renderCurrentReadingPage();
    }
    if (currentTocIndex_ + 1 < tocCount_) {
        return openTocEntry(currentTocIndex_ + 1);
    }
    showingEndOfBook_ = true;
    return renderEndOfBookPage();
}

bool ReaderBookService::prevPage() {
    if (showingBookEntry_) return false;
    if (showingToc_) return prevTocPage();
    if (showingEndOfBook_) {
        showingEndOfBook_ = false;
        return renderCurrentReadingPage();
    }
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
    if (showingBookmarks_) return handleBookmarkTap(x, y);
    if (showingReaderMenu_) return handleReaderMenuTap(x, y);
    if (showingEndOfBook_) {
        if (x < kPaperS3Width / 3) return prevPage();
        showingEndOfBook_ = false;
        showingBookEntry_ = true;
        renderBookEntryPage();
        return true;
    }
    if (showingBookEntry_) {
        if (x >= kEntryButtonX && x < kEntryButtonX + kEntryButtonW && y >= kEntryContinueY && y < kEntryContinueY + kEntryButtonH) return continueReading();
        if (x >= kEntryButtonX && x < kEntryButtonX + kEntryButtonW && y >= kEntryTocY && y < kEntryTocY + kEntryButtonH) {
            showingBookEntry_ = false;
            showingToc_ = true;
            if (currentTocIndex_ >= 0) tocPage_ = currentTocIndex_ / kTocEntriesPerPage;
            renderTocPage(tocPage_);
            return true;
        }
        if (x >= kEntryButtonX && x < kEntryButtonX + kEntryButtonW && y >= kEntryRestartY && y < kEntryRestartY + kEntryButtonH) return restartReading();
        return false;
    }
    if (!showingToc_) {
        // PaperS3 e-paper reading should use coarse, forgiving zones. Keep the
        // small top-left/menu affordances, but also support large reference-style
        // zones: left third = previous page, right third = next page, center = menu.
        if (x < 170 && y < 90) {
            showingBookEntry_ = true;
            showingToc_ = false;
            renderBookEntryPage();
            return true;
        }
        if (x < 210 && y >= 90 && y < 150) {
            showingToc_ = true;
            if (currentTocIndex_ >= 0) tocPage_ = currentTocIndex_ / kTocEntriesPerPage;
            renderTocPage(tocPage_);
            return true;
        }
        if (x < kPaperS3Width / 3) return prevPage();
        if (x > (kPaperS3Width * 2) / 3) return nextPage();
        // Center tap → show reader menu overlay
        renderReaderMenuOverlay();
        return true;
    }
    if (tocCount_ <= 0) return false;
    if (y < kTocFirstRowY || y >= kTocFirstRowY + kTocEntriesPerPage * kTocRowH) return false;
    int row = (y - kTocFirstRowY) / kTocRowH;
    int index = tocPage_ * kTocEntriesPerPage + row;
    if (index < 0 || index >= tocCount_) return false;
    return openTocEntry(index);
}

bool ReaderBookService::handleLongPress(int16_t x, int16_t y) {
    if (!open_ || showingToc_ || showingBookEntry_ || showingBookmarks_) return false;
    if (y < 90 || y > kPaperS3Height - 50) return false;
    // Long press center zone adds bookmark
    if (x >= 170 && x <= 370) {
        if (addBookmarkAtCurrent()) {
            // Briefly flash a confirmation overlay via a text page
            char msg[80];
            snprintf(msg, sizeof(msg), "书签已添加：第%d页", currentPage_ + 1);
            g_readerText.renderTextPage("书签", msg, 1, 1);
            g_displayService.enqueueFull(false, 100);
            delay(1200);
            renderCurrentReadingPage();
            g_displayService.enqueueFull(false, 100);
        }
        return true;
    }
    return false;
}

void ReaderBookService::renderReaderMenuOverlay() {
    showingReaderMenu_ = true;
    M5Canvas* c = g_readerText.canvas();
    if (!c) return;
    c->fillRect(0, 0, kPaperS3Width, kPaperS3Height, 0x8410);
    c->fillRect(20, 180, kPaperS3Width - 40, 600, TFT_WHITE);
    c->drawRoundRect(20, 180, kPaperS3Width - 40, 600, 18, TFT_BLACK);

    // Header: time left, battery right
    char timeBuf[12];
    char battBuf[16];
    g_uiRenderer.formatTimeStr(timeBuf, sizeof(timeBuf));
    g_uiRenderer.formatBatterySimple(battBuf, sizeof(battBuf));
    c->setTextColor(TFT_BLACK, TFT_WHITE);
    g_cjkText.drawText(40, 200, timeBuf, 0x8410);
    g_cjkText.drawRight(kPaperS3Width - 40, 200, battBuf, 0x8410);

    // Title: font size + current chapter
    char titleBuf[80];
    snprintf(titleBuf, sizeof(titleBuf), "字号: %d", g_configService.get().fontSize);
    g_cjkText.drawCentered(60, 240, kPaperS3Width - 80, 40, titleBuf, TFT_BLACK);

    // Button row 1: font- (40,310,180,64) / font+ (260,310,180,64)
    const int16_t btnW = 180;
    const int16_t btnH = 64;
    const int16_t btnRow1Y = 310;
    c->fillRoundRect(40, btnRow1Y, btnW, btnH, 18, TFT_WHITE);
    c->drawRoundRect(40, btnRow1Y, btnW, btnH, 18, TFT_BLACK);
    g_cjkText.drawCentered(40, btnRow1Y, btnW, btnH, "字号 -", TFT_BLACK);
    c->fillRoundRect(260, btnRow1Y, btnW, btnH, 18, TFT_WHITE);
    c->drawRoundRect(260, btnRow1Y, btnW, btnH, 18, TFT_BLACK);
    g_cjkText.drawCentered(260, btnRow1Y, btnW, btnH, "字号 +", TFT_BLACK);

    // Button row 2: TOC (40,400) / bookmarks (260,400)
    const int16_t btnRow2Y = 400;
    c->fillRoundRect(40, btnRow2Y, btnW, btnH, 18, TFT_WHITE);
    c->drawRoundRect(40, btnRow2Y, btnW, btnH, 18, TFT_BLACK);
    g_cjkText.drawCentered(40, btnRow2Y, btnW, btnH, "目录", TFT_BLACK);
    c->fillRoundRect(260, btnRow2Y, btnW, btnH, 18, TFT_WHITE);
    c->drawRoundRect(260, btnRow2Y, btnW, btnH, 18, TFT_BLACK);
    g_cjkText.drawCentered(260, btnRow2Y, btnW, btnH, "书签", TFT_BLACK);

    // Button row 3: progress (40,490) / dark mode (260,490)
    const int16_t btnRow3Y = 490;
    c->fillRoundRect(40, btnRow3Y, btnW, btnH, 18, TFT_WHITE);
    c->drawRoundRect(40, btnRow3Y, btnW, btnH, 18, TFT_BLACK);
    g_cjkText.drawCentered(40, btnRow3Y, btnW, btnH, "进度", TFT_BLACK);
    c->fillRoundRect(260, btnRow3Y, btnW, btnH, 18, TFT_WHITE);
    c->drawRoundRect(260, btnRow3Y, btnW, btnH, 18, TFT_BLACK);
    g_cjkText.drawCentered(260, btnRow3Y, btnW, btnH, "深色", TFT_BLACK);

    // Close hint at bottom
    g_cjkText.drawCentered(40, 580, kPaperS3Width - 80, 40, "点击屏幕边缘关闭", 0x8410);
}

bool ReaderBookService::handleReaderMenuTap(int16_t x, int16_t y) {
    // Click outside the white panel closes the menu
    if (x < 20 || x > 520 || y < 180 || y > 780) {
        showingReaderMenu_ = false;
        renderCurrentReadingPage();
        return true;
    }
    // Button row 1: 字号- (40,310,180,64) / 字号+ (260,310,180,64)
    if (y >= 310 && y < 374) {
        if (x >= 40 && x < 220) {
            uint8_t cur = g_configService.get().fontSize;
            if (cur > 12) { g_configService.setFontSize(cur - 2); g_configService.save(); }
        } else if (x >= 260 && x < 440) {
            uint8_t cur = g_configService.get().fontSize;
            if (cur < 48) { g_configService.setFontSize(cur + 2); g_configService.save(); }
        }
        showingReaderMenu_ = false;
        renderCurrentReadingPage();
        return true;
    }
    // Button row 2: 目录 (40,400) / 书签 (260,400)
    if (y >= 400 && y < 464) {
        if (x >= 40 && x < 220) {
            showingReaderMenu_ = false;
            showingToc_ = true;
            if (currentTocIndex_ >= 0) tocPage_ = currentTocIndex_ / kTocEntriesPerPage;
            renderTocPage(tocPage_);
            return true;
        } else if (x >= 260 && x < 440) {
            showingReaderMenu_ = false;
            showingBookmarks_ = true;
            bookmarkPage_ = 0;
            renderBookmarkPage(0);
            return true;
        }
    }
    // Button row 3: 进度 (40,490) / 深色 (260,490)
    if (y >= 490 && y < 554) {
        if (x >= 40 && x < 220) {
            showingReaderMenu_ = false;
            showingBookEntry_ = true;
            renderBookEntryPage();
            return true;
        } else if (x >= 260 && x < 440) {
            // Toggle dark mode
            g_configService.mut().darkModeDefault = !g_configService.get().darkModeDefault;
            g_configService.save();
            showingReaderMenu_ = false;
            renderCurrentReadingPage();
            return true;
        }
    }
    return false;
}

bool ReaderBookService::openTocEntry(int index) {
    if (index < 0 || index >= tocCount_) return false;
    currentTocIndex_ = index;
    currentPage_ = 0;
    showingToc_ = false;
    showingEndOfBook_ = false;
    if (!buildChapterPages(index)) {
        if (renderChapterPreview(index)) {
            saveProgress();
            return true;
        }
        char body[360];
        snprintf(body, sizeof(body),
                 "章节：%s\n\n"
                 "分页和预览都失败了。\n"
                 "可以返回目录选择其他章节；若反复出现，请重建目录/分页缓存。",
                 toc_[index].title.c_str());
        g_readerText.renderTextPage("章节打开失败", body, 1, 1);
        saveProgress();
        return true;
    }
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
    snprintf(summary, sizeof(summary), "目录共 %d 条 · *为当前章节 · 右侧为位置", tocCount_);
    const uint32_t textSize = activeTextSize();
    char rows[kTocEntriesPerPage][128];
    const char* rowPtrs[kTocEntriesPerPage];
    int rowCount = 0;
    for (int i = start; i < end && rowCount < kTocEntriesPerPage; ++i) {
        const char marker = (i == currentTocIndex_) ? '*' : ' ';
        char titleBuf[78];
        strlcpy(titleBuf, toc_[i].title.c_str(), sizeof(titleBuf));
        trimUtf8Tail(titleBuf, strlen(titleBuf));
        const uint32_t pct = textSize > 0 ? min<uint32_t>(99, (toc_[i].charOffset * 100ULL) / textSize) : 0;
        snprintf(rows[rowCount], sizeof(rows[rowCount]), "%c%03d %3lu%%  %s", marker, i + 1, static_cast<unsigned long>(pct), titleBuf);
        rowPtrs[rowCount] = rows[rowCount];
        rowCount++;
    }
    g_readerText.renderListPage(title_, summary, rowPtrs, rowCount, kTocFirstRowY, kTocRowH, page + 1, totalPages, 0);
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
    return buildChapterPagesFrom(index, start, true);
}

bool ReaderBookService::buildChapterPagesFrom(int index, uint32_t start, bool allowCache) {
    if (index < 0 || index >= tocCount_ || !activeTextPath_[0] || !pageStarts_) return false;
    const uint32_t fullEnd = chapterEndOffset(index);
    if (fullEnd <= start) return false;
    if (allowCache && start == chapterContentStart(index) && loadChapterPageCache(index, start, fullEnd)) return true;
    showBlockingChapterStatus(index);
    File f = SD.open(activeTextPath_, FILE_READ);
    if (!f) return false;

    const auto& cfg = g_configService.get();
    const LayoutConfig lc = g_configService.layout();
    ReaderRenderOptions ro;
    ro.fontSize = cfg.fontSize;
    ro.marginLeft = lc.marginLeft;
    ro.marginTop = lc.marginTop;
    ro.marginRight = lc.marginRight;
    ro.marginBottom = lc.marginBottom;
    ro.lineGap = cfg.fontSize * (cfg.lineSpacing - 100) / 100;
    ro.indentFirstLine = lc.indentFirstLine;
    ro.paragraphSpacing = lc.paragraphSpacing;
    ro.justify = cfg.justify;

    uint32_t offset = start;
    pageCount_ = 0;
    pageWindowStart_ = start;
    pageWindowEnd_ = start;
    pageWindowTruncated_ = false;
    while (offset < fullEnd && pageCount_ < kMaxChapterPages) {
        pageStarts_[pageCount_++] = offset;
        if (!f.seek(offset)) break;
        const uint32_t toRead = min<uint32_t>(4095, fullEnd - offset);
        char buf[4096];
        int n = f.read(reinterpret_cast<uint8_t*>(buf), toRead);
        if (n <= 0) break;
        size_t len = trimUtf8Tail(buf, static_cast<size_t>(n));
        size_t consumed = g_readerText.measurePageBytes(buf, len, ro);
        if (consumed == 0) consumed = len;
        if (consumed == 0) break;
        offset += consumed;
    }
    if (offset < fullEnd && pageCount_ >= kMaxChapterPages) {
        pageWindowTruncated_ = true;
        Serial.printf("[vink3][book] chapter pagination hit page cap: toc=%d cap=%d offset=%lu end=%lu\n",
                      index, kMaxChapterPages, static_cast<unsigned long>(offset), static_cast<unsigned long>(fullEnd));
    }
    pageWindowEnd_ = pageWindowTruncated_ ? offset : fullEnd;
    f.close();
    currentPage_ = 0;
    Serial.printf("[vink3][book] chapter pages built: toc=%d pages=%d window=%lu-%lu layout=%08lx\n",
                  index, pageCount_, static_cast<unsigned long>(pageWindowStart_),
                  static_cast<unsigned long>(pageWindowEnd_), static_cast<unsigned long>(pageLayoutKey()));
    if (pageCount_ > 0 && !pageWindowTruncated_ && start == chapterContentStart(index)) saveChapterPageCache(index, start, fullEnd);
    return pageCount_ > 0;
}

bool ReaderBookService::renderCurrentReadingPage() {
    if (currentTocIndex_ < 0 || currentTocIndex_ >= tocCount_ || pageCount_ <= 0 || !pageStarts_) return false;
    const uint32_t start = pageStarts_[currentPage_];
    const uint32_t end = (currentPage_ + 1 < pageCount_) ? pageStarts_[currentPage_ + 1] : pageWindowEnd_;
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

    char titleBuf[68];
    strlcpy(titleBuf, toc_[currentTocIndex_].title.c_str(), sizeof(titleBuf));
    trimUtf8Tail(titleBuf, strlen(titleBuf));
    char header[96];
    snprintf(header, sizeof(header), "%03d %s", currentTocIndex_ + 1, titleBuf);

    // Build render options from current config.
    const auto& cfg = g_configService.get();
    const LayoutConfig lc = g_configService.layout();
    ReaderRenderOptions ro;
    ro.fontSize  = cfg.fontSize;
    ro.marginLeft  = lc.marginLeft;
    ro.marginTop   = lc.marginTop;
    ro.marginRight = lc.marginRight;
    ro.marginBottom = lc.marginBottom;
    ro.lineGap    = cfg.fontSize * (cfg.lineSpacing - 100) / 100;  // percent → pixel extra
    ro.indentFirstLine = lc.indentFirstLine;
    ro.paragraphSpacing = lc.paragraphSpacing;
    ro.justify    = cfg.justify;
    ro.dark       = cfg.darkModeDefault;

    g_readerText.renderTextPage(header, body, currentPage_ + 1, pageCount_, ro);
    saveProgress();
    return true;
}

bool ReaderBookService::renderEndOfBookPage() {
    if (!open_) return false;
    showingEndOfBook_ = true;
    const char* chapterTitle = (currentTocIndex_ >= 0 && currentTocIndex_ < tocCount_) ? toc_[currentTocIndex_].title.c_str() : "最后一章";
    char body[700];
    snprintf(body, sizeof(body),
             "《%s》已读完。\n\n"
             "最后位置：%s · 第 %d 页\n\n"
             "左侧点击/右滑：回到最后一页\n"
             "中间或右侧点击：打开书籍入口\n"
             "也可以从书籍入口回目录或从头开始。",
             title_[0] ? title_ : "当前书籍",
             chapterTitle,
             currentPage_ + 1);

    const auto& cfg = g_configService.get();
    ReaderRenderOptions ro;
    ro.fontSize = cfg.fontSize;
    ro.dark = cfg.darkModeDefault;
    g_readerText.renderTextPage("本书已读完", body, 1, 1, ro);
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

    char titleBuf[68];
    strlcpy(titleBuf, toc_[index].title.c_str(), sizeof(titleBuf));
    trimUtf8Tail(titleBuf, strlen(titleBuf));
    char header[96];
    snprintf(header, sizeof(header), "%03d %s", index + 1, titleBuf);
    g_readerText.renderTextPage(header, content, 1, 1);
    return true;
}

bool ReaderBookService::rebuildCurrentChapter() {
    if (currentTocIndex_ < 0 || currentTocIndex_ >= tocCount_) return false;
    const uint32_t start = chapterContentStart(currentTocIndex_);
    return buildChapterPagesFrom(currentTocIndex_, start, false) && renderCurrentReadingPage();
}

void ReaderBookService::rebuildCurrentChapterAsync() {
    if (layoutRebuildTask_) {
        vTaskDelete(layoutRebuildTask_);
        layoutRebuildTask_ = nullptr;
    }
    layoutRebuildChapter_ = currentTocIndex_;
    layoutRebuildTargetPage_ = currentPage_;
    lastLayoutKey_ = pageLayoutKey();
    xTaskCreatePinnedToCore(
        [](void* arg) {
            static_cast<ReaderBookService*>(arg)->layoutRebuildTaskEntry();
        },
        "vink3-layout-rebuild",
        8192,
        this,
        2,
        &layoutRebuildTask_,
        1);
}

void ReaderBookService::layoutRebuildTaskEntry() {
    if (layoutRebuildChapter_ < 0 || layoutRebuildChapter_ >= tocCount_) {
        layoutRebuildTask_ = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    renderChapterLoadingPage(layoutRebuildChapter_);
    g_displayService.enqueueFull(true, 100);
    {
        const uint32_t start = chapterContentStart(layoutRebuildChapter_);
        if (!buildChapterPagesFrom(layoutRebuildChapter_, start, false)) {
            layoutRebuildTask_ = nullptr;
            vTaskDelete(nullptr);
            return;
        }
    }
    if (layoutRebuildTargetPage_ >= pageCount_) {
        layoutRebuildTargetPage_ = max(0, pageCount_ - 1);
    }
    currentPage_ = layoutRebuildTargetPage_;
    layoutRebuildTask_ = nullptr;
    renderCurrentReadingPage();
    g_displayService.enqueueFull(true, 100);
    vTaskDelete(nullptr);
}

void ReaderBookService::onLayoutChanged() {
    if (!open_ || !bookPath_[0]) return;
    char path[192];
    getPageCachePath(path, sizeof(path));
    if (SD.exists(path)) SD.remove(path);
}

void ReaderBookService::invalidateAllPageCache() {
    if (!open_ || !bookPath_[0]) return;
    char path[192];
    getPageCachePath(path, sizeof(path));
    if (SD.exists(path)) SD.remove(path);
}

} // namespace vink3

namespace vink3 {

void ReaderBookService::saveCurrentProgress() {
    if (!open_) return;
    saveProgress();
}

} // namespace vink3
