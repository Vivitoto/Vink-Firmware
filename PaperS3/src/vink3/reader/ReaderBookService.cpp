#include "ReaderBookService.h"
#include "ReaderTextRenderer.h"
#include "../display/DisplayService.h"
#include "../ui/VinkUiRenderer.h"
#include "../text/CjkTextRenderer.h"
#include "../ReadPaper176.h"
#include "../../Config.h"
#include "../../TextCodec.h"
#include <SPI.h>
#include <esp_heap_caps.h>

namespace vink3 {

namespace {
// TOC cache schema. Bump whenever chapter detection/title display rules change
// so old bad `.vink-toc` files do not survive firmware updates.
static constexpr uint32_t kTocCacheMagic = 0x56435432UL; // VCT2
static constexpr uint32_t kProgressMagic = 0x56505232UL; // VPR2
static constexpr uint32_t kPageCacheMagic = 0x56504732UL; // VPG2
static constexpr uint32_t kLastBookMagic = 0x564C4231UL; // VLB1
static constexpr size_t kPathBufSize = 192;
static constexpr const char* kLastBookRecordPath = "/books/.vink-last-book";
}

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

bool ReaderBookService::isBookPath(const char* name) const {
    if (!name) return false;
    String s(name);
    s.toLowerCase();
    return s.endsWith(".txt") || s.endsWith(".epub");
}

void ReaderBookService::normalizeChildPath(const char* dirPath, const char* rawName, char* out, size_t len) const {
    if (!out || len == 0) return;
    out[0] = '\0';
    if (!dirPath || !dirPath[0] || !rawName || !rawName[0]) return;
    String dir(dirPath);
    String raw(rawName);
    if (!dir.startsWith("/")) dir = String("/") + dir;
    while (dir.length() > 1 && dir.endsWith("/")) dir.remove(dir.length() - 1);

    String path;
    const String prefix = dir + "/";
    if (raw.startsWith(prefix) || (dir == "/" && raw.startsWith("/"))) {
        path = raw;
    } else if (raw.startsWith("/")) {
        // Some SD implementations return child names as "/name" even when the
        // parent directory was opened as "/books". Treat that as relative to
        // the current browser directory, not as an SD-root absolute path.
        path = (dir == "/") ? raw : dir + raw;
    } else {
        path = (dir == "/") ? String("/") + raw : dir + "/" + raw;
    }
    strlcpy(out, path.c_str(), len);
}

bool ReaderBookService::scanBooks() {
    if (!ensureSdReady() || !ensureBookBuffers()) return false;
    if (!currentLibraryDir_[0]) strlcpy(currentLibraryDir_, BOOKS_DIR, sizeof(currentLibraryDir_));
    bookCount_ = 0;

    if (strcmp(currentLibraryDir_, BOOKS_DIR) != 0) {
        char parent[sizeof(currentLibraryDir_)];
        parentDirOf(currentLibraryDir_, parent, sizeof(parent));
        addLibraryEntry(parent, true, ".. 上级目录");
    }

    scanBookDir(currentLibraryDir_, 0);
    sortBooks();
    booksScanned_ = true;
    if (bookPage_ * kBooksPerPage >= bookCount_) bookPage_ = 0;
    Serial.printf("[vink3][book] library browser: %s -> %d entries\n", currentLibraryDir_, bookCount_);
    return true;
}

void ReaderBookService::scanBookDir(const char* dirPath, uint8_t depth) {
    // File-browser mode: scan exactly the current directory. Subdirectories are
    // visible rows that can be tapped to enter; books are opened only when the
    // file row is tapped. This avoids dumping every nested book into one flat,
    // confusing bookshelf.
    if (!dirPath || !dirPath[0] || bookCount_ >= kMaxBooks || depth > 0) return;
    File dir = SD.open(dirPath);
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        return;
    }

    File f = dir.openNextFile();
    while (f && bookCount_ < kMaxBooks) {
        const char* rawName = f.name();
        if (rawName && rawName[0]) {
            char pathBuf[160];
            normalizeChildPath(dirPath, rawName, pathBuf, sizeof(pathBuf));
            String path(pathBuf);

            const char* slash = strrchr(path.c_str(), '/');
            const char* leaf = slash ? slash + 1 : path.c_str();
            const bool hidden = leaf[0] == '.';
            if (!hidden) {
                if (f.isDirectory()) {
                    addLibraryEntry(path.c_str(), true);
                } else if (isBookPath(path.c_str())) {
                    addLibraryEntry(path.c_str(), false);
                }
            }
        }
        f.close();
        f = dir.openNextFile();
    }
    dir.close();
}

void ReaderBookService::setDisplayNameFromPath(char* out, size_t len, const char* path) const {
    if (!out || len == 0) return;
    out[0] = '\0';
    if (!path || !path[0]) return;
    const char* slash = strrchr(path, '/');
    const char* name = slash ? slash + 1 : path;
    strlcpy(out, name, len);
    char* dot = strrchr(out, '.');
    if (dot && dot != out) *dot = '\0';
}

void ReaderBookService::parentDirOf(const char* path, char* out, size_t len) const {
    if (!out || len == 0) return;
    strlcpy(out, BOOKS_DIR, len);
    if (!path || !path[0] || strcmp(path, BOOKS_DIR) == 0) return;
    char tmp[160];
    strlcpy(tmp, path, sizeof(tmp));
    size_t n = strlen(tmp);
    while (n > 1 && tmp[n - 1] == '/') tmp[--n] = '\0';
    char* slash = strrchr(tmp, '/');
    if (!slash || slash == tmp) return;
    *slash = '\0';
    if (strncmp(tmp, BOOKS_DIR, strlen(BOOKS_DIR)) == 0) strlcpy(out, tmp, len);
}

bool ReaderBookService::addLibraryEntry(const char* path, bool isDirectory, const char* displayName) {
    if (!path || !path[0] || bookCount_ >= kMaxBooks) return false;
    if (!isDirectory && !isBookPath(path)) return false;
    if (strlen(path) >= sizeof(bookPaths_[bookCount_])) {
        Serial.printf("[vink3][book] skip library path too long: %s\n", path);
        return false;
    }

    strlcpy(bookPaths_[bookCount_], path, sizeof(bookPaths_[bookCount_]));
    if (displayName && displayName[0]) {
        strlcpy(bookTitles_[bookCount_], displayName, sizeof(bookTitles_[bookCount_]));
    } else {
        setDisplayNameFromPath(bookTitles_[bookCount_], sizeof(bookTitles_[bookCount_]), path);
    }
    bookFlags_[bookCount_] = isDirectory ? kBookIsDirectory : detectBookFlags(bookPaths_[bookCount_]);
    bookCount_++;
    return true;
}

bool ReaderBookService::addBookPath(const char* path) {
    return addLibraryEntry(path, false);
}

void ReaderBookService::sortBooks() {
    if (bookCount_ <= 1 || !bookPaths_ || !bookTitles_ || !bookFlags_) return;
    // Stable file-browser order: parent entry first, then directories, then book
    // files. Names shown in the browser have extensions stripped.
    for (int i = 1; i < bookCount_; ++i) {
        int j = i;
        while (j > 0) {
            const bool aParent = strcmp(bookTitles_[j - 1], ".. 上级目录") == 0;
            const bool bParent = strcmp(bookTitles_[j], ".. 上级目录") == 0;
            const bool aDir = (bookFlags_[j - 1] & kBookIsDirectory) != 0;
            const bool bDir = (bookFlags_[j] & kBookIsDirectory) != 0;
            int cmp = 0;
            if (aParent != bParent) cmp = aParent ? -1 : 1;
            else if (aDir != bDir) cmp = aDir ? -1 : 1;
            else {
                cmp = strcmp(bookTitles_[j - 1], bookTitles_[j]);
                if (cmp == 0) cmp = strcmp(bookPaths_[j - 1], bookPaths_[j]);
            }
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
    nextPreheatTocIndex_ = -1;
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

void ReaderBookService::getLastBookPath(char* out, size_t len) const {
    if (!out || len == 0) return;
    strlcpy(out, kLastBookRecordPath, len);
}

bool ReaderBookService::loadTocCache() {
    if (!ensureTocBuffer() || !bookPath_[0]) return false;
    char cachePath[kPathBufSize];
    getTocCachePath(cachePath, sizeof(cachePath));
    File f = SD.open(cachePath, FILE_READ);
    if (!f) return false;
    uint32_t magic = 0;
    uint16_t count = 0;
    f.read(reinterpret_cast<uint8_t*>(&magic), sizeof(magic));
    f.read(reinterpret_cast<uint8_t*>(&count), sizeof(count));
    if (magic != kTocCacheMagic || count == 0 || count > kMaxTocEntries) {
        f.close();
        Serial.printf("[vink3][book] TOC cache stale/invalid: magic=0x%08lx count=%u, rebuilding\n",
                      static_cast<unsigned long>(magic), static_cast<unsigned>(count));
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
    char cachePath[kPathBufSize];
    getTocCachePath(cachePath, sizeof(cachePath));
    File f = SD.open(cachePath, FILE_WRITE);
    if (!f) return;
    uint32_t magic = kTocCacheMagic;
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
    char path[kPathBufSize];
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
    if (magic != kProgressMagic || cachedSize != activeTextSize() || chapter >= tocCount_) return false; // VPR2
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
    char path[kPathBufSize];
    getProgressPath(path, sizeof(path));
    File f = SD.open(path, FILE_WRITE);
    if (!f) return;
    uint32_t magic = kProgressMagic;
    uint32_t fileSize = activeTextSize();
    uint16_t chapter = static_cast<uint16_t>(currentTocIndex_);
    uint16_t page = static_cast<uint16_t>(max(0, currentPage_));
    f.write(reinterpret_cast<const uint8_t*>(&magic), sizeof(magic));
    f.write(reinterpret_cast<const uint8_t*>(&fileSize), sizeof(fileSize));
    f.write(reinterpret_cast<const uint8_t*>(&chapter), sizeof(chapter));
    f.write(reinterpret_cast<const uint8_t*>(&page), sizeof(page));
    f.close();
    saveLastBookPath();
}

bool ReaderBookService::readProgressForBook(const char* bookPath, uint16_t& chapter, uint16_t& page) const {
    chapter = 0;
    page = 0;
    if (!bookPath || !bookPath[0] || !sdReady_) return false;
    char path[kPathBufSize];
    getSidecarPathForBook(path, sizeof(path), bookPath, ".vink-progress");
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    uint32_t magic = 0;
    uint32_t cachedSize = 0;
    f.read(reinterpret_cast<uint8_t*>(&magic), sizeof(magic));
    f.read(reinterpret_cast<uint8_t*>(&cachedSize), sizeof(cachedSize));
    f.read(reinterpret_cast<uint8_t*>(&chapter), sizeof(chapter));
    f.read(reinterpret_cast<uint8_t*>(&page), sizeof(page));
    f.close();
    return magic == kProgressMagic;
}

bool ReaderBookService::loadLastBookPath(char* out, size_t len) const {
    if (!out || len == 0) return false;
    out[0] = '\0';
    if (!sdReady_) return false;
    File f = SD.open(kLastBookRecordPath, FILE_READ);
    if (!f) return false;
    uint32_t magic = 0;
    uint16_t pathLen = 0;
    f.read(reinterpret_cast<uint8_t*>(&magic), sizeof(magic));
    f.read(reinterpret_cast<uint8_t*>(&pathLen), sizeof(pathLen));
    if (magic != kLastBookMagic || pathLen == 0 || pathLen >= len) {
        f.close();
        return false;
    }
    size_t n = f.read(reinterpret_cast<uint8_t*>(out), pathLen);
    f.close();
    out[n] = '\0';
    return n == pathLen && out[0] == '/' && SD.exists(out);
}

void ReaderBookService::saveLastBookPath() {
    if (!bookPath_[0] || !ensureSdReady()) return;
    File f = SD.open(kLastBookRecordPath, FILE_WRITE);
    if (!f) return;
    uint32_t magic = kLastBookMagic;
    uint16_t pathLen = static_cast<uint16_t>(min<size_t>(strlen(bookPath_), sizeof(bookPath_) - 1));
    f.write(reinterpret_cast<const uint8_t*>(&magic), sizeof(magic));
    f.write(reinterpret_cast<const uint8_t*>(&pathLen), sizeof(pathLen));
    f.write(reinterpret_cast<const uint8_t*>(bookPath_), pathLen);
    f.close();
}

bool ReaderBookService::openLastBook() {
    if (open_) return true;
    if (!ensureSdReady()) return false;
    char lastPath[sizeof(bookPath_)];
    if (!loadLastBookPath(lastPath, sizeof(lastPath))) return false;
    return openBook(lastPath);
}

uint32_t ReaderBookService::readerLayoutKey() const {
    // Page splitting depends on layout, not on the TOC byte offsets. When font
    // size, line gap, or margins become user-configurable, only this key changes
    // and `.vink-pages` records for the old layout are ignored; `.vink-toc`
    // remains valid.
    ReaderRenderOptions opt;
    uint32_t h = 2166136261UL;
    auto mix = [&](uint32_t v) { h ^= v; h *= 16777619UL; };
    mix(opt.fontSize);
    mix(static_cast<uint16_t>(opt.marginLeft));
    mix(static_cast<uint16_t>(opt.marginTop));
    mix(static_cast<uint16_t>(opt.marginRight));
    mix(static_cast<uint16_t>(opt.marginBottom));
    mix(static_cast<uint16_t>(opt.lineGap));
    mix(opt.vertical ? 1 : 0);
    mix(static_cast<uint16_t>(g_readerText.fontSize()));
    return h;
}

bool ReaderBookService::loadChapterPageCache(int index, uint32_t start, uint32_t end) {
    if (!bookPath_[0] || !pageStarts_ || !ensureSdReady()) return false;
    char path[kPathBufSize];
    getPageCachePath(path, sizeof(path));
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    const uint32_t wantedLayout = readerLayoutKey();
    bool found = false;
    while (f.available()) {
        uint32_t magic = 0;
        uint32_t cachedSize = 0;
        uint32_t layout = 0;
        uint16_t chapter = 0;
        uint16_t count = 0;
        uint32_t cachedStart = 0;
        uint32_t cachedEnd = 0;
        if (f.read(reinterpret_cast<uint8_t*>(&magic), sizeof(magic)) != sizeof(magic)) break;
        if (f.read(reinterpret_cast<uint8_t*>(&cachedSize), sizeof(cachedSize)) != sizeof(cachedSize)) break;
        if (f.read(reinterpret_cast<uint8_t*>(&layout), sizeof(layout)) != sizeof(layout)) break;
        if (f.read(reinterpret_cast<uint8_t*>(&chapter), sizeof(chapter)) != sizeof(chapter)) break;
        if (f.read(reinterpret_cast<uint8_t*>(&count), sizeof(count)) != sizeof(count)) break;
        if (f.read(reinterpret_cast<uint8_t*>(&cachedStart), sizeof(cachedStart)) != sizeof(cachedStart)) break;
        if (f.read(reinterpret_cast<uint8_t*>(&cachedEnd), sizeof(cachedEnd)) != sizeof(cachedEnd)) break;
        if (magic != kPageCacheMagic || count == 0 || count > kMaxChapterPages) break;
        const size_t need = static_cast<size_t>(count) * sizeof(uint32_t);
        if (cachedSize == activeTextSize() && layout == wantedLayout && chapter == index && cachedStart == start && cachedEnd == end) {
            size_t got = f.read(reinterpret_cast<uint8_t*>(pageStarts_), need);
            if (got != need) break;
            pageCount_ = count;
            currentPage_ = 0;
            found = true;
        } else {
            f.seek(f.position() + need);
        }
    }
    f.close();
    if (found) Serial.printf("[vink3][book] page cache loaded: chapter=%d pages=%d\n", index, pageCount_);
    return found;
}

bool ReaderBookService::chapterPageCacheValid(int index, uint32_t start, uint32_t end) {
    if (!bookPath_[0] || !ensureSdReady()) return false;
    char path[kPathBufSize];
    getPageCachePath(path, sizeof(path));
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    const uint32_t wantedLayout = readerLayoutKey();
    bool found = false;
    while (f.available()) {
        uint32_t magic = 0;
        uint32_t cachedSize = 0;
        uint32_t layout = 0;
        uint16_t chapter = 0;
        uint16_t count = 0;
        uint32_t cachedStart = 0;
        uint32_t cachedEnd = 0;
        if (f.read(reinterpret_cast<uint8_t*>(&magic), sizeof(magic)) != sizeof(magic)) break;
        if (f.read(reinterpret_cast<uint8_t*>(&cachedSize), sizeof(cachedSize)) != sizeof(cachedSize)) break;
        if (f.read(reinterpret_cast<uint8_t*>(&layout), sizeof(layout)) != sizeof(layout)) break;
        if (f.read(reinterpret_cast<uint8_t*>(&chapter), sizeof(chapter)) != sizeof(chapter)) break;
        if (f.read(reinterpret_cast<uint8_t*>(&count), sizeof(count)) != sizeof(count)) break;
        if (f.read(reinterpret_cast<uint8_t*>(&cachedStart), sizeof(cachedStart)) != sizeof(cachedStart)) break;
        if (f.read(reinterpret_cast<uint8_t*>(&cachedEnd), sizeof(cachedEnd)) != sizeof(cachedEnd)) break;
        if (magic != kPageCacheMagic || count == 0 || count > kMaxChapterPages) break;
        if (cachedSize == activeTextSize() && layout == wantedLayout && chapter == index && cachedStart == start && cachedEnd == end) {
            found = true;
            break;
        }
        f.seek(f.position() + static_cast<size_t>(count) * sizeof(uint32_t));
    }
    f.close();
    return found;
}

void ReaderBookService::saveChapterPageCacheData(int index, uint32_t start, uint32_t end, const uint32_t* starts, int count) {
    if (!bookPath_[0] || !starts || count <= 0 || !ensureSdReady()) return;
    char path[kPathBufSize];
    getPageCachePath(path, sizeof(path));
    File f = SD.open(path, FILE_APPEND);
    if (!f) return;
    uint32_t magic = kPageCacheMagic;
    uint32_t fileSize = activeTextSize();
    uint32_t layout = readerLayoutKey();
    uint16_t chapter = static_cast<uint16_t>(index);
    uint16_t pageCount = static_cast<uint16_t>(count);
    f.write(reinterpret_cast<const uint8_t*>(&magic), sizeof(magic));
    f.write(reinterpret_cast<const uint8_t*>(&fileSize), sizeof(fileSize));
    f.write(reinterpret_cast<const uint8_t*>(&layout), sizeof(layout));
    f.write(reinterpret_cast<const uint8_t*>(&chapter), sizeof(chapter));
    f.write(reinterpret_cast<const uint8_t*>(&pageCount), sizeof(pageCount));
    f.write(reinterpret_cast<const uint8_t*>(&start), sizeof(start));
    f.write(reinterpret_cast<const uint8_t*>(&end), sizeof(end));
    f.write(reinterpret_cast<const uint8_t*>(starts), static_cast<size_t>(count) * sizeof(uint32_t));
    f.close();
    Serial.printf("[vink3][book] page cache appended: chapter=%d pages=%d layout=0x%08lx\n", index, count, static_cast<unsigned long>(layout));
}

void ReaderBookService::saveChapterPageCache(int index, uint32_t start, uint32_t end) {
    saveChapterPageCacheData(index, start, end, pageStarts_, pageCount_);
}

bool ReaderBookService::preheatChapterPageCache(int index) {
    if (index < 0 || index >= tocCount_ || !activeTextPath_[0]) return false;
    const uint32_t start = chapterContentStart(index);
    const uint32_t end = chapterEndOffset(index);
    if (end <= start || chapterPageCacheValid(index, start, end)) return true;

    uint32_t starts[kMaxChapterPages];
    int count = 0;
    File f = SD.open(activeTextPath_, FILE_READ);
    if (!f) return false;
    uint32_t offset = start;
    while (offset < end && count < kMaxChapterPages) {
        starts[count++] = offset;
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
    if (count <= 0) return false;
    saveChapterPageCacheData(index, start, end, starts, count);
    Serial.printf("[vink3][book] preheated next chapter cache: toc=%d pages=%d\n", index, count);
    return true;
}

void ReaderBookService::resetPreheatCursor(int afterChapter) {
    nextPreheatTocIndex_ = afterChapter + 1;
    if (nextPreheatTocIndex_ < 0 || nextPreheatTocIndex_ >= tocCount_) nextPreheatTocIndex_ = -1;
}

void ReaderBookService::maybePreheatNextChapter() {
    if (currentTocIndex_ < 0 || tocCount_ <= 0 || pageCount_ <= 0) return;
    if (nextPreheatTocIndex_ <= currentTocIndex_) resetPreheatCursor(currentTocIndex_);
    if (nextPreheatTocIndex_ < 0 || nextPreheatTocIndex_ >= tocCount_) return;

    // Sequential pre-pagination: after the current chapter has a page table and
    // the visible page has been queued, move the preheat cursor forward one
    // chapter at a time. This is intentionally ordered, not “only at the last
    // page”, so normal reading gradually prepares chapter N+1, N+2, ... before
    // the user reaches those boundaries.
    g_displayService.enqueueFull(false, 100);
    g_displayService.waitIdle(1800);
    if (preheatChapterPageCache(nextPreheatTocIndex_)) {
        nextPreheatTocIndex_++;
        if (nextPreheatTocIndex_ >= tocCount_) nextPreheatTocIndex_ = -1;
    }
}

bool ReaderBookService::openFirstBook() {
    if (!booksScanned_ && !scanBooks()) return false;
    for (int i = 0; i < bookCount_; ++i) {
        if ((bookFlags_[i] & kBookIsDirectory) == 0) return openBook(bookPaths_[i]);
    }
    return false;
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
    saveLastBookPath();
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

void ReaderBookService::renderReaderHome() {
    char lastPath[sizeof(bookPath_)];
    char lastTitle[sizeof(title_)];
    char progressText[80];
    bool hasLast = false;
    lastPath[0] = '\0';
    lastTitle[0] = '\0';
    strlcpy(progressText, "上次进度：未开始", sizeof(progressText));

    if (open_ && bookPath_[0]) {
        strlcpy(lastPath, bookPath_, sizeof(lastPath));
        strlcpy(lastTitle, title_, sizeof(lastTitle));
        hasLast = true;
    } else if (ensureSdReady() && loadLastBookPath(lastPath, sizeof(lastPath))) {
        const char* slash = strrchr(lastPath, '/');
        const char* name = slash ? slash + 1 : lastPath;
        strlcpy(lastTitle, name, sizeof(lastTitle));
        char* dot = strrchr(lastTitle, '.');
        if (dot) *dot = '\0';
        hasLast = true;
    }

    if (hasLast) {
        uint16_t chapter = 0, page = 0;
        if (readProgressForBook(lastPath, chapter, page)) {
            snprintf(progressText, sizeof(progressText), "上次进度：第 %u 章 · 第 %u 页",
                     static_cast<unsigned>(chapter + 1), static_cast<unsigned>(page + 1));
        }
    }
    g_uiRenderer.renderReaderHome(lastTitle, lastPath, progressText, hasLast);
}

void ReaderBookService::renderOpenOrHelp() {
    if (!open_) {
        if (openLastBook()) {
            renderCurrent();
        } else {
            renderLibraryPage(bookPage_);
        }
        return;
    }
    renderCurrent();
}

void ReaderBookService::renderLibraryPage(uint16_t page) {
    scanBooks();
    char body[900];
    body[0] = '\0';
    if (bookCount_ <= 0) {
        const char* rows[] = {
            "请把 .txt 文件放到 SD 卡 /books 目录。",
            "支持 UTF-8 / GBK 文本和目录缓存。",
            "书架页使用 UI 字体，正文页才使用阅读字体。",
        };
        g_uiRenderer.renderUiListPage(SystemState::Library, "书架", "书架为空", rows, 3,
                                      kListFirstRowY, kListRowH, 1, 1);
        return;
    }
    const uint16_t totalPages = (bookCount_ + kBooksPerPage - 1) / kBooksPerPage;
    if (page >= totalPages) page = totalPages - 1;
    bookPage_ = page;
    const int start = bookPage_ * kBooksPerPage;
    const int end = min(bookCount_, start + kBooksPerPage);
    char summary[160];
    snprintf(summary, sizeof(summary), "文件浏览器 %s · %d 项", currentLibraryDir_, bookCount_);
    char rows[kBooksPerPage][180];
    const char* rowPtrs[kBooksPerPage];
    int rowCount = 0;
    int activeRow = -1;
    for (int i = start; i < end && rowCount < kBooksPerPage; ++i) {
        const bool current = open_ && strcmp(bookPaths_[i], bookPath_) == 0;
        if (current) activeRow = rowCount;
        if (bookFlags_[i] & kBookIsDirectory) {
            snprintf(rows[rowCount], sizeof(rows[rowCount]), "□  %s", bookTitles_[i]);
        } else {
            snprintf(rows[rowCount], sizeof(rows[rowCount]), "▤  %s", bookTitles_[i]);
        }
        rowPtrs[rowCount] = rows[rowCount];
        rowCount++;
    }
    // Library is a shell/UI page, not a reading page: keep it on the bundled UI
    // font path so it visually matches the other tabs. Only actual book body
    // rendering should use ReaderTextRenderer's full reading font.
    g_uiRenderer.renderUiListPage(SystemState::Library, "书架", summary, rowPtrs, rowCount,
                                  kListFirstRowY, kListRowH, bookPage_ + 1, totalPages, activeRow);
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
    lastLibraryTapOpenedBook_ = false;
    if (bookFlags_[index] & kBookIsDirectory) {
        strlcpy(currentLibraryDir_, bookPaths_[index], sizeof(currentLibraryDir_));
        bookPage_ = 0;
        booksScanned_ = false;
        renderLibraryPage(0);
        return true;
    }
    if (!isTxtPath(bookPaths_[index])) {
        const char* info[] = {
            "当前版本先支持 TXT 阅读。",
            "EPUB 文件已能在书架中识别显示，",
            "正文解析会放到后续版本。",
        };
        g_uiRenderer.renderUiActionPage(SystemState::Library, "暂不支持", info, 3, nullptr, 0);
        return true;
    }
    lastLibraryTapOpenedBook_ = openBook(bookPaths_[index]);
    return lastLibraryTapOpenedBook_;
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
    char sizeText[24];
    char lineTitle[180];
    char lineSize[48];
    char lineStage[96];
    formatBytes(bookFileSize(bookPath_), sizeText, sizeof(sizeText));
    snprintf(lineTitle, sizeof(lineTitle), "书籍：%s", title_[0] ? title_ : "TXT");
    snprintf(lineSize, sizeof(lineSize), "大小：%s", sizeText);
    snprintf(lineStage, sizeof(lineStage), "%s...", stage && stage[0] ? stage : "正在打开");
    const char* info[] = {
        lineTitle,
        lineSize,
        lineStage,
        "首次打开大书可能需要一会儿。",
        "完成后会自动进入书籍入口。",
    };
    g_uiRenderer.renderUiActionPage(SystemState::Library, "正在打开", info, 5, nullptr, 0);
}

void ReaderBookService::renderChapterLoadingPage(int index) {
    const char* chapterTitle = (index >= 0 && index < tocCount_) ? toc_[index].title.c_str() : "章节";
    char lineTitle[180];
    char lineChapter[180];
    snprintf(lineTitle, sizeof(lineTitle), "书籍：%s", title_[0] ? title_ : "TXT");
    snprintf(lineChapter, sizeof(lineChapter), "章节：%s", chapterTitle);
    const char* info[] = {
        lineTitle,
        lineChapter,
        "正在分页...",
        "首次进入长章节时会测量每页文字量。",
        "完成后会缓存到 .vink-pages。",
    };
    g_uiRenderer.renderUiActionPage(SystemState::Reader, "正在分页", info, 5, nullptr, 0);
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
    snprintf(lineCache, sizeof(lineCache), "状态：进度%s · 目录%s · 分页%s",
             (cacheFlags & kBookHasProgress) ? "已存" : "无",
             (cacheFlags & kBookHasTocCache) ? "已缓存" : "未缓存",
             (cacheFlags & kBookHasPageCache) ? "已预热" : "未预热");
    snprintf(lineProgress, sizeof(lineProgress), "进度：%s", progress);
    const char* info[] = {lineTitle, lineSize, lineToc, lineCache, lineProgress, "缓存维护可解决升级后的旧索引问题"};
    const char* actions[] = {"继续阅读", "目录", "从头开始", "清除分页缓存", "重新生成目录"};
    // This is still UI/navigation chrome after choosing a book. Keep it on the
    // UI font path; only actual page body rendering may use the reading font.
    g_uiRenderer.renderUiActionPage(SystemState::Reader, "书籍入口", info, 6, actions, 5);
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

bool ReaderBookService::clearPageCache() {
    if (!open_ || !ensureSdReady()) return false;
    char path[kPathBufSize];
    getPageCachePath(path, sizeof(path));
    if (path[0] && SD.exists(path)) {
        SD.remove(path);
        Serial.printf("[vink3][book] page cache removed: %s\n", path);
    }
    pageCount_ = 0;
    currentPage_ = 0;
    nextPreheatTocIndex_ = -1;
    showingBookEntry_ = true;
    renderBookEntryPage();
    return true;
}

bool ReaderBookService::rebuildTocCache() {
    if (!open_ || !ensureSdReady()) return false;
    char reopenPath[sizeof(bookPath_)];
    strlcpy(reopenPath, bookPath_, sizeof(reopenPath));
    char path[kPathBufSize];
    getTocCachePath(path, sizeof(path));
    if (path[0] && SD.exists(path)) {
        SD.remove(path);
        Serial.printf("[vink3][book] TOC cache removed: %s\n", path);
    }
    getPageCachePath(path, sizeof(path));
    if (path[0] && SD.exists(path)) {
        SD.remove(path);
        Serial.printf("[vink3][book] page cache removed with TOC rebuild: %s\n", path);
    }
    return openBook(reopenPath);
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
        if (x >= kEntryButtonX && x < kEntryButtonX + kEntryButtonW && y >= kEntryClearPagesY && y < kEntryClearPagesY + kEntryButtonH) return clearPageCache();
        if (x >= kEntryButtonX && x < kEntryButtonX + kEntryButtonW && y >= kEntryRebuildTocY && y < kEntryRebuildTocY + kEntryButtonH) return rebuildTocCache();
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
            renderTocPage(tocPage_);
            return true;
        }
        if (x < kPaperS3Width / 3) return prevPage();
        if (x > (kPaperS3Width * 2) / 3) return nextPage();
        showingBookEntry_ = true;
        showingToc_ = false;
        renderBookEntryPage();
        return true;
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
    resetPreheatCursor(index);
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
    snprintf(summary, sizeof(summary), "目录共 %d 条", tocCount_);
    char rows[kTocEntriesPerPage][128];
    const char* rowPtrs[kTocEntriesPerPage];
    int rowCount = 0;
    int activeRow = -1;
    for (int i = start; i < end && rowCount < kTocEntriesPerPage; ++i) {
        if (i == currentTocIndex_) activeRow = rowCount;
        char titleBuf[128];
        // Fit visually with the UI font. Do not blindly byte-truncate titles:
        // that caused headings like “第七百四十五章 你在何处？” to become
        // “第七百四十五章 你在”.
        g_cjkText.fitTextToWidth(toc_[i].title.c_str(), titleBuf, sizeof(titleBuf), 430);
        snprintf(rows[rowCount], sizeof(rows[rowCount]), "%s", titleBuf);
        rowPtrs[rowCount] = rows[rowCount];
        rowCount++;
    }
    // TOC/list navigation is UI chrome; use the UI font instead of the body
    // reading font to keep UI and reading typography strictly separated.
    // Current chapter is marked by row chrome in VinkUiRenderer, never by '*001'.
    g_uiRenderer.renderUiListPage(SystemState::Reader, title_, summary, rowPtrs, rowCount,
                                  kTocFirstRowY, kTocRowH, page + 1, totalPages, activeRow);
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
    uint32_t start = toc_[index].charOffset;
    if (start == 0) {
        // Drop UTF-8 BOM only. For real chapter entries, the TOC offset is the
        // page-splitting anchor and must point at the first byte of the title
        // line itself, e.g. the “第” in “第一章 你好”. Layout/font changes rebuild
        // page tables from this stable byte offset; they must not rebuild TOC.
        File f = SD.open(activeTextPath_, FILE_READ);
        if (f && f.available() >= 3) {
            uint8_t bom[3] = {0};
            f.read(bom, sizeof(bom));
            if (bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF) start = 3;
        }
        if (f) f.close();
    }
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
    if (loadChapterPageCache(index, start, end)) {
        if (index >= currentTocIndex_) resetPreheatCursor(index);
        return true;
    }
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
    if (pageCount_ > 0 && index >= currentTocIndex_) resetPreheatCursor(index);
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
    char header[160];
    strlcpy(header, toc_[currentTocIndex_].title.c_str(), sizeof(header));
    g_readerText.renderTextPage(header, body, currentPage_ + 1, pageCount_);
    saveProgress();
    maybePreheatNextChapter();
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

    char* content = body;
    (void)len;

    char header[160];
    strlcpy(header, toc_[index].title.c_str(), sizeof(header));
    g_readerText.renderTextPage(header, content, 1, 1);
    return true;
}

} // namespace vink3

namespace vink3 {

void ReaderBookService::saveCurrentProgress() {
    if (!open_) return;
    saveProgress();
}

} // namespace vink3
