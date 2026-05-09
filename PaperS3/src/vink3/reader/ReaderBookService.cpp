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
#include <new>

namespace vink3 {

namespace {
// TOC cache schema. Bump whenever chapter detection/title display rules change
// so old bad `.vink-toc` files do not survive firmware updates.
static constexpr uint32_t kTocCacheMagic = 0x56435435UL; // VCT5 (v0.5: added level field)
static constexpr uint32_t kProgressMagic = 0x56505233UL; // VPR3
static constexpr uint32_t kPageCacheMagic = 0x56504734UL; // VPG4
static constexpr uint32_t kLastBookMagic = 0x564C4231UL; // VLB1
static constexpr size_t kPathBufSize = 192;
static constexpr const char* kConfigRoot = "/config";
static constexpr const char* kVinkCacheRoot = "/config/vink-cache";
static constexpr const char* kSidecarRoot = "/config/vink-cache/books";
static constexpr const char* kLastBookRecordPath = "/config/vink-cache/.vink-last-book";
static constexpr const char* kLegacyLastBookRecordPath = "/books/.vink-last-book";
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
        if (!SD.exists(kConfigRoot)) SD.mkdir(kConfigRoot);
        if (!SD.exists(kVinkCacheRoot)) SD.mkdir(kVinkCacheRoot);
        if (!SD.exists(kSidecarRoot)) SD.mkdir(kSidecarRoot);
    }
    Serial.printf("[vink3][book] SD %s\n", sdReady_ ? "ready" : "unavailable");
    return sdReady_;
}

bool ReaderBookService::ensureSdReadyForTransfer() {
    return ensureSdReady();
}

bool ReaderBookService::ensureTocBuffer() {
    if (toc_) return true;
    void* mem = heap_caps_malloc(sizeof(ChapterDetectResult) * kMaxTocEntries, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!mem) mem = malloc(sizeof(ChapterDetectResult) * kMaxTocEntries);
    if (!mem) {
        Serial.println("[vink3][book] failed to allocate TOC buffer");
        return false;
    }
    toc_ = static_cast<ChapterDetectResult*>(mem);
    for (int i = 0; i < kMaxTocEntries; ++i) {
        new (&toc_[i]) ChapterDetectResult();
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
    showingReaderMenu_ = false;
    showingToc_ = true;
    bookPath_[0] = '\0';
    activeTextPath_[0] = '\0';
    title_[0] = '\0';
    activeTextSize_ = 0;
    activeTextFingerprint_ = 0;
}

void ReaderBookService::setTitleFromPath(const char* path) {
    const char* slash = strrchr(path, '/');
    const char* name = slash ? slash + 1 : path;
    strlcpy(title_, name, sizeof(title_));
    char* dot = strrchr(title_, '.');
    if (dot) *dot = '\0';
}

void ReaderBookService::getSidecarPath(char* out, size_t len, const char* suffix) const {
    // Keep generated metadata out of /books so the bookshelf directory stays
    // clean for user-managed book files. /config is the SD-side root reserved
    // for Vink configuration and cache data:
    //   /books/foo.txt -> /config/vink-cache/books/ab/ab12...ef.vink-toc
    //   /books/dir/foo.txt -> /config/vink-cache/books/cd/cd34...90.vink-pages
    // Use the original book path, not the temporary UTF-8 conversion path.
    getSidecarPathForBook(out, len, bookPath_, suffix);
}

void ReaderBookService::getSidecarPathForBook(char* out, size_t len, const char* bookPath, const char* suffix) const {
    if (!out || len == 0) return;
    out[0] = '\0';
    if (!bookPath || !bookPath[0]) return;

    const uint64_t hash = hashBookPath(bookPath);
    char hex[17];
    formatHashHex(hash, hex, sizeof(hex));
    // Hash paths avoid long/non-ASCII mirrored cache names and keep one flat,
    // deterministic mapping per normalized absolute book path. A 2-hex shard
    // keeps large libraries from putting every cache file in one directory.
    snprintf(out, len, "%s/%c%c/%s%s", kSidecarRoot, hex[0], hex[1], hex, suffix ? suffix : "");
}

uint64_t ReaderBookService::hashBookPath(const char* bookPath) const {
    // FNV-1a 64-bit over the absolute source path. This is tiny compared with
    // SD IO/pagination work and avoids fragile filename/path mirroring in the
    // cache directory. Cache payloads still validate file size/schema/layout.
    uint64_t h = 1469598103934665603ULL;
    if (!bookPath) return h;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(bookPath); *p; ++p) {
        h ^= static_cast<uint64_t>(*p);
        h *= 1099511628211ULL;
    }
    return h;
}

void ReaderBookService::formatHashHex(uint64_t hash, char* out, size_t len) const {
    if (!out || len == 0) return;
    snprintf(out, len, "%08lx%08lx",
             static_cast<unsigned long>((hash >> 32) & 0xFFFFFFFFUL),
             static_cast<unsigned long>(hash & 0xFFFFFFFFUL));
}

void ReaderBookService::getLegacySidecarPathForBook(char* out, size_t len, const char* bookPath, const char* suffix) const {
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

void ReaderBookService::ensureParentDirForPath(const char* path) const {
    if (!path || !path[0] || !sdReady_) return;
    char dir[kPathBufSize];
    strlcpy(dir, path, sizeof(dir));
    char* slash = strrchr(dir, '/');
    if (!slash || slash == dir) return;
    *slash = '\0';

    for (char* p = dir + 1; *p; ++p) {
        if (*p != '/') continue;
        *p = '\0';
        if (dir[0] && !SD.exists(dir)) SD.mkdir(dir);
        *p = '/';
    }
    if (!SD.exists(dir)) SD.mkdir(dir);
}

bool ReaderBookService::removeSidecarForCurrentBook(const char* suffix) {
    if (!open_ || !ensureSdReady() || !suffix) return false;
    bool removed = false;
    char path[kPathBufSize];
    getSidecarPath(path, sizeof(path), suffix);
    if (path[0] && SD.exists(path)) {
        SD.remove(path);
        removed = true;
        Serial.printf("[vink3][book] sidecar removed: %s\n", path);
    }
    getLegacySidecarPathForBook(path, sizeof(path), bookPath_, suffix);
    if (path[0] && SD.exists(path)) {
        SD.remove(path);
        removed = true;
        Serial.printf("[vink3][book] legacy sidecar removed: %s\n", path);
    }
    return removed;
}

uint8_t ReaderBookService::detectBookFlags(const char* bookPath) const {
    if (!bookPath || !bookPath[0]) return 0;
    uint8_t flags = 0;
    char sidecar[kPathBufSize];
    getSidecarPathForBook(sidecar, sizeof(sidecar), bookPath, ".vink-toc");
    if (sidecar[0] && SD.exists(sidecar)) flags |= kBookHasTocCache;
    getLegacySidecarPathForBook(sidecar, sizeof(sidecar), bookPath, ".vink-toc");
    if (sidecar[0] && SD.exists(sidecar)) flags |= kBookHasTocCache;
    getSidecarPathForBook(sidecar, sizeof(sidecar), bookPath, ".vink-progress");
    if (sidecar[0] && SD.exists(sidecar)) flags |= kBookHasProgress;
    getLegacySidecarPathForBook(sidecar, sizeof(sidecar), bookPath, ".vink-progress");
    if (sidecar[0] && SD.exists(sidecar)) flags |= kBookHasProgress;
    getSidecarPathForBook(sidecar, sizeof(sidecar), bookPath, ".vink-pages");
    if (sidecar[0] && SD.exists(sidecar)) flags |= kBookHasPageCache;
    getLegacySidecarPathForBook(sidecar, sizeof(sidecar), bookPath, ".vink-pages");
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

uint64_t ReaderBookService::sampleFileFingerprint(const char* path, uint32_t* outSize) const {
    if (outSize) *outSize = 0;
    if (!path || !path[0]) return 0;
    File f = SD.open(path, FILE_READ);
    if (!f) return 0;

    const uint32_t size = f.size();
    if (outSize) *outSize = size;
    uint64_t h = 1469598103934665603ULL;
    auto mixByte = [&](uint8_t b) {
        h ^= static_cast<uint64_t>(b);
        h *= 1099511628211ULL;
    };
    auto mix32 = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i) mixByte(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
    };
    mix32(size);

    static constexpr uint32_t kFingerprintSampleBytes = 512;
    uint8_t buf[128];
    auto sampleRange = [&](uint32_t offset, uint32_t count) {
        if (count == 0 || !f.seek(offset)) return;
        uint32_t remaining = count;
        while (remaining > 0) {
            const uint32_t want = min<uint32_t>(remaining, sizeof(buf));
            int got = f.read(buf, want);
            if (got <= 0) break;
            for (int i = 0; i < got; ++i) mixByte(buf[i]);
            remaining -= static_cast<uint32_t>(got);
        }
    };

    const uint32_t head = min<uint32_t>(size, kFingerprintSampleBytes);
    sampleRange(0, head);
    if (size > kFingerprintSampleBytes) {
        const uint32_t tail = min<uint32_t>(size - head, kFingerprintSampleBytes);
        sampleRange(size - tail, tail);
    }
    f.close();
    return h;
}

void ReaderBookService::refreshActiveTextIdentity() {
    activeTextFingerprint_ = sampleFileFingerprint(activeTextPath_, &activeTextSize_);
    Serial.printf("[vink3][book] active text fingerprint: size=%lu fp=%08lx%08lx\n",
                  static_cast<unsigned long>(activeTextSize_),
                  static_cast<unsigned long>((activeTextFingerprint_ >> 32) & 0xFFFFFFFFUL),
                  static_cast<unsigned long>(activeTextFingerprint_ & 0xFFFFFFFFUL));
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
    if (!f) {
        getLegacySidecarPathForBook(cachePath, sizeof(cachePath), bookPath_, ".vink-toc");
        f = SD.open(cachePath, FILE_READ);
    }
    if (!f) return false;
    uint32_t magic = 0;
    uint16_t count = 0;
    uint32_t cachedSize = 0;
    uint64_t cachedFingerprint = 0;
    f.read(reinterpret_cast<uint8_t*>(&magic), sizeof(magic));
    f.read(reinterpret_cast<uint8_t*>(&count), sizeof(count));
    f.read(reinterpret_cast<uint8_t*>(&cachedSize), sizeof(cachedSize));
    f.read(reinterpret_cast<uint8_t*>(&cachedFingerprint), sizeof(cachedFingerprint));
    if (magic != kTocCacheMagic || count == 0 || count > kMaxTocEntries || cachedSize != activeTextSize() || cachedFingerprint != activeTextFingerprint_) {
        f.close();
        Serial.printf("[vink3][book] TOC cache stale/invalid: magic=0x%08lx count=%u size=%lu, rebuilding\n",
                      static_cast<unsigned long>(magic), static_cast<unsigned>(count), static_cast<unsigned long>(cachedSize));
        return false;
    }
    tocCount_ = 0;
    for (uint16_t i = 0; i < count && f.available(); ++i) {
        uint8_t level = 1;
        uint16_t titleLen = 0;
        f.read(reinterpret_cast<uint8_t*>(&toc_[i].charOffset), sizeof(toc_[i].charOffset));
        f.read(reinterpret_cast<uint8_t*>(&toc_[i].chapterNumber), sizeof(toc_[i].chapterNumber));
        f.read(&level, sizeof(level));
        toc_[i].level = static_cast<int8_t>(level);
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
    ensureParentDirForPath(cachePath);
    File f = SD.open(cachePath, FILE_WRITE);
    if (!f) return;
    uint32_t magic = kTocCacheMagic;
    uint16_t count = static_cast<uint16_t>(tocCount_);
    uint32_t fileSize = activeTextSize();
    uint64_t fingerprint = activeTextFingerprint_;
    f.write(reinterpret_cast<const uint8_t*>(&magic), sizeof(magic));
    f.write(reinterpret_cast<const uint8_t*>(&count), sizeof(count));
    f.write(reinterpret_cast<const uint8_t*>(&fileSize), sizeof(fileSize));
    f.write(reinterpret_cast<const uint8_t*>(&fingerprint), sizeof(fingerprint));
    for (int i = 0; i < tocCount_; ++i) {
        uint8_t level = static_cast<uint8_t>(toc_[i].level >= 0 ? toc_[i].level : 1);
        uint16_t titleLen = min<size_t>(toc_[i].title.length(), 120);
        f.write(reinterpret_cast<const uint8_t*>(&toc_[i].charOffset), sizeof(toc_[i].charOffset));
        f.write(reinterpret_cast<const uint8_t*>(&toc_[i].chapterNumber), sizeof(toc_[i].chapterNumber));
        f.write(&level, sizeof(level));
        f.write(reinterpret_cast<const uint8_t*>(&toc_[i].score), sizeof(toc_[i].score));
        f.write(reinterpret_cast<const uint8_t*>(&titleLen), sizeof(titleLen));
        f.write(reinterpret_cast<const uint8_t*>(toc_[i].title.c_str()), titleLen);
    }
    f.close();
    Serial.printf("[vink3][book] TOC cache saved: %s (%d entries)\n", cachePath, tocCount_);
}

uint32_t ReaderBookService::activeTextSize() const {
    if (activeTextSize_ > 0) return activeTextSize_;
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
    if (!f) {
        getLegacySidecarPathForBook(path, sizeof(path), bookPath_, ".vink-progress");
        f = SD.open(path, FILE_READ);
    }
    if (!f) return false;
    uint32_t magic = 0;
    uint32_t cachedSize = 0;
    uint64_t cachedFingerprint = 0;
    uint16_t chapter = 0;
    uint16_t page = 0;
    f.read(reinterpret_cast<uint8_t*>(&magic), sizeof(magic));
    f.read(reinterpret_cast<uint8_t*>(&cachedSize), sizeof(cachedSize));
    f.read(reinterpret_cast<uint8_t*>(&cachedFingerprint), sizeof(cachedFingerprint));
    f.read(reinterpret_cast<uint8_t*>(&chapter), sizeof(chapter));
    f.read(reinterpret_cast<uint8_t*>(&page), sizeof(page));
    f.close();
    (void)page;
    if (magic != kProgressMagic || cachedSize != activeTextSize() || cachedFingerprint != activeTextFingerprint_ || chapter >= tocCount_) return false; // VPR3
    // v0.4.5-on-0.4.2: restore at chapter level only. Building a whole chapter
    // during open regressed large books; the visible page is measured lazily.
    currentTocIndex_ = chapter;
    currentPage_ = 0;
    pageCount_ = 0;
    pageWindowStart_ = chapterContentStart(chapter);
    pageWindowEnd_ = pageWindowStart_;
    pageWindowTruncated_ = true;
    hasProgress_ = true;
    showingToc_ = false;
    Serial.printf("[vink3][book] progress loaded: chapter=%u (chapter-level restore)\n", chapter);
    return true;
}

void ReaderBookService::saveProgress() {
    if (!bookPath_[0] || currentTocIndex_ < 0 || !ensureSdReady()) return;
    char path[kPathBufSize];
    getProgressPath(path, sizeof(path));
    ensureParentDirForPath(path);
    File f = SD.open(path, FILE_WRITE);
    if (!f) return;
    uint32_t magic = kProgressMagic;
    uint32_t fileSize = activeTextSize();
    uint64_t fingerprint = activeTextFingerprint_;
    uint16_t chapter = static_cast<uint16_t>(currentTocIndex_);
    // Store chapter-level progress for streaming pagination. Old exact page
    // numbers are intentionally not persisted because only the visible window is
    // measured under the current layout.
    uint16_t page = 0;
    f.write(reinterpret_cast<const uint8_t*>(&magic), sizeof(magic));
    f.write(reinterpret_cast<const uint8_t*>(&fileSize), sizeof(fileSize));
    f.write(reinterpret_cast<const uint8_t*>(&fingerprint), sizeof(fingerprint));
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
    if (!f) {
        getLegacySidecarPathForBook(path, sizeof(path), bookPath, ".vink-progress");
        f = SD.open(path, FILE_READ);
    }
    if (!f) return false;
    uint32_t magic = 0;
    uint32_t cachedSize = 0;
    uint64_t cachedFingerprint = 0;
    f.read(reinterpret_cast<uint8_t*>(&magic), sizeof(magic));
    f.read(reinterpret_cast<uint8_t*>(&cachedSize), sizeof(cachedSize));
    f.read(reinterpret_cast<uint8_t*>(&cachedFingerprint), sizeof(cachedFingerprint));
    f.read(reinterpret_cast<uint8_t*>(&chapter), sizeof(chapter));
    f.read(reinterpret_cast<uint8_t*>(&page), sizeof(page));
    f.close();
    (void)cachedSize;
    (void)cachedFingerprint;
    return magic == kProgressMagic;
}

bool ReaderBookService::loadLastBookPath(char* out, size_t len) const {
    if (!out || len == 0) return false;
    out[0] = '\0';
    if (!sdReady_) return false;
    File f = SD.open(kLastBookRecordPath, FILE_READ);
    if (!f) f = SD.open(kLegacyLastBookRecordPath, FILE_READ);
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
    ensureParentDirForPath(kLastBookRecordPath);
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
    ReaderRenderOptions opt = g_readerText.currentOptions();
    const ReaderSettings& settings = g_readerText.settings();
    uint32_t h = 2166136261UL;
    auto mix = [&](uint32_t v) { h ^= v; h *= 16777619UL; };
    mix(opt.fontSize);
    mix(static_cast<uint16_t>(opt.marginLeft));
    mix(static_cast<uint16_t>(opt.marginTop));
    mix(static_cast<uint16_t>(opt.marginRight));
    mix(static_cast<uint16_t>(opt.marginBottom));
    mix(static_cast<uint16_t>(opt.lineGap));
    mix(static_cast<uint16_t>(opt.firstLineIndentPx));
    mix(static_cast<uint16_t>(opt.letterGap));
    mix(static_cast<uint16_t>(opt.paragraphGap));
    mix(static_cast<uint16_t>(opt.underlineOffset));
    mix(opt.vertical ? 1 : 0);
    mix(opt.indentFirstLine ? 1 : 0);
    mix(opt.compactBlankLines ? 1 : 0);
    mix(opt.dynamicLineHeight ? 1 : 0);
    mix(opt.breakLineOpt ? 1 : 0);
    mix(opt.underline ? 1 : 0);
    mix(settings.formatting1);
    mix(settings.renderOpt1);
    mix(settings.spacing);
    mix(settings.layoutAlgorithmVersion);
    mix(settings.fontMetricVersion);
    mix(static_cast<uint16_t>(g_readerText.fontSize()));
    return h;
}

bool ReaderBookService::fileOffsetStartsParagraph(uint32_t offset, uint32_t chapterStart) const {
    if (!activeTextPath_[0]) return true;
    if (offset <= chapterStart) return true;
    File f = SD.open(activeTextPath_, FILE_READ);
    if (!f) return false;
    const uint32_t probeStart = offset > 24 ? offset - 24 : 0;
    if (!f.seek(probeStart)) {
        f.close();
        return false;
    }
    char buf[25];
    const int n = f.read(reinterpret_cast<uint8_t*>(buf), offset - probeStart);
    f.close();
    if (n <= 0) return false;
    for (int i = n - 1; i >= 0; --i) {
        const char c = buf[i];
        if (c == '\n' || c == '\r') return true;
        if (c != ' ' && c != '\t') return false;
    }
    return probeStart == 0;
}

ReaderRenderOptions ReaderBookService::currentRenderOptionsForOffset(uint32_t offset, uint32_t chapterStart) const {
    ReaderRenderOptions opt = g_readerText.currentOptions();
    opt.startsAtParagraph = fileOffsetStartsParagraph(offset, chapterStart);
    return opt;
}

int ReaderBookService::pageIndexForOffset(uint32_t offset) const {
    if (!pageStarts_ || pageCount_ <= 0) return 0;
    int result = 0;
    for (int i = 0; i < pageCount_; ++i) {
        if (pageStarts_[i] <= offset) result = i;
        else break;
    }
    return result;
}

bool ReaderBookService::measurePageEndOffset(uint32_t offset, uint32_t fullEnd, uint32_t& outEnd) const {
    outEnd = offset;
    const uint32_t start = chapterContentStart(currentTocIndex_);
    if (!activeTextPath_[0] || fullEnd <= offset) return false;
    File f = SD.open(activeTextPath_, FILE_READ);
    if (!f || !f.seek(offset)) {
        if (f) f.close();
        return false;
    }
    const uint32_t toRead = min<uint32_t>(4095, fullEnd - offset);
    char buf[4096];
    int n = f.read(reinterpret_cast<uint8_t*>(buf), toRead);
    f.close();
    if (n <= 0) return false;
    size_t len = trimUtf8Tail(buf, static_cast<size_t>(n));
    size_t consumed = g_readerText.measurePageBytes(buf, len, currentRenderOptionsForOffset(offset, start));
    if (consumed == 0 || consumed > len) consumed = len;
    if (consumed == 0) return false;
    outEnd = min<uint32_t>(fullEnd, offset + static_cast<uint32_t>(consumed));
    return outEnd > offset;
}

bool ReaderBookService::loadChapterPageCache(int index, uint32_t start, uint32_t end) {
    (void)index;
    (void)start;
    (void)end;
    // v0.4.5-on-0.4.2 intentionally ignores persistent .vink-pages caches.
    // They were layout-sensitive and caused slow/stale cross-version restores.
    return false;
}

bool ReaderBookService::chapterPageCacheValid(int index, uint32_t start, uint32_t end) {
    (void)index;
    (void)start;
    (void)end;
    return false;
}

void ReaderBookService::saveChapterPageCacheData(int index, uint32_t start, uint32_t end, const uint32_t* starts, int count) {
    (void)index;
    (void)start;
    (void)end;
    (void)starts;
    (void)count;
    // Persistent page caches are no longer written. Streaming pagination keeps
    // only the current in-RAM page window.
}

void ReaderBookService::saveChapterPageCache(int index, uint32_t start, uint32_t end) {
    saveChapterPageCacheData(index, start, end, pageStarts_, pageCount_);
}

bool ReaderBookService::preheatChapterPageCache(int index) {
    (void)index;
    return false;
}

void ReaderBookService::resetPreheatCursor(int afterChapter) {
    nextPreheatTocIndex_ = afterChapter + 1;
    if (nextPreheatTocIndex_ < 0 || nextPreheatTocIndex_ >= tocCount_) nextPreheatTocIndex_ = -1;
}

void ReaderBookService::maybePreheatNextChapter() {
    // No synchronous/eager pre-pagination in the UI task.
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
    refreshActiveTextIdentity();

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
    strlcpy(progressText, "上次进度:未开始", sizeof(progressText));

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
            snprintf(progressText, sizeof(progressText), "上次进度:第 %u 章 · 第 %u 页",
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
            "书架页使用 UI 字体,正文页才使用阅读字体。",
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
            "EPUB 文件已能在书架中识别显示,",
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
    if (showingReaderMenu_) {
        renderReaderMenuPage();
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
    snprintf(lineTitle, sizeof(lineTitle), "书籍:%s", title_[0] ? title_ : "TXT");
    snprintf(lineSize, sizeof(lineSize), "大小:%s", sizeText);
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
    snprintf(lineTitle, sizeof(lineTitle), "书籍:%s", title_[0] ? title_ : "TXT");
    snprintf(lineChapter, sizeof(lineChapter), "章节:%s", chapterTitle);
    const char* info[] = {
        lineTitle,
        lineChapter,
        "正在准备当前页...",
        "长章节会按当前屏逐页测量。",
        "不再读取或写入 .vink-pages。",
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
        snprintf(progress, sizeof(progress), "%s", chapterTitle);
    } else {
        strlcpy(progress, "无", sizeof(progress));
    }
    char lineTitle[180];
    char lineSize[48];
    char lineToc[48];
    char lineCache[96];
    char lineProgress[180];
    snprintf(lineTitle, sizeof(lineTitle), "书籍:%s", title_);
    snprintf(lineSize, sizeof(lineSize), "大小:%s", sizeText);
    snprintf(lineToc, sizeof(lineToc), "目录:%d 条", tocCount_);
    snprintf(lineCache, sizeof(lineCache), "状态:进度%s · 目录%s · 分页%s",
             (cacheFlags & kBookHasProgress) ? "已存" : "无",
             (cacheFlags & kBookHasTocCache) ? "已缓存" : "未缓存",
             (cacheFlags & kBookHasPageCache) ? "有旧缓存" : "无旧缓存");
    snprintf(lineProgress, sizeof(lineProgress), "进度:%s", progress);
    const char* info[] = {lineTitle, lineSize, lineToc, lineCache, lineProgress, "阅读按当前页流式分页;旧分页缓存可清理"};
    const char* actions[] = {"继续阅读", "目录", "从头开始", "清除分页缓存", "重新生成目录"};
    // This is still UI/navigation chrome after choosing a book. Keep it on the
    // UI font path; only actual page body rendering may use the reading font.
    g_uiRenderer.renderUiActionPage(SystemState::Reader, "书籍入口", info, 6, actions, 5);
}

void ReaderBookService::renderReaderMenuPage() {
    if (!open_) {
        renderOpenOrHelp();
        return;
    }
    char lineTitle[180];
    char lineChapter[180];
    char lineRefresh[80];
    char lineAa[80];
    char lineRender[120];
    const char* chapterTitle = (currentTocIndex_ >= 0 && currentTocIndex_ < tocCount_) ? toc_[currentTocIndex_].title.c_str() : "未进入章节";
    snprintf(lineTitle, sizeof(lineTitle), "书籍:%s", title_);
    snprintf(lineChapter, sizeof(lineChapter), "章节:%s", chapterTitle);
    snprintf(lineRefresh, sizeof(lineRefresh), "翻页刷新:%s", g_displayService.readerRefreshStrategyLabel());
    snprintf(lineAa, sizeof(lineAa), "抗锯齿:%s", g_readerText.antiAliasLabel());
    snprintf(lineRender, sizeof(lineRender), "下划线:%s · 翻页动画:%s", g_readerText.underlineLabel(), g_readerText.pageTurnEffectLabel());
    const char* info[] = {lineTitle, lineChapter, lineRefresh, lineAa, lineRender, "左上角可回书籍入口;小范围改动会重建分页"};
    const char* actions[] = {"继续阅读", "翻页刷新", "抗锯齿", "排版优化", "下划线", "翻页动画"};
    g_uiRenderer.renderUiActionPage(SystemState::Reader, "阅读菜单", info, 6, actions, 6);
}

bool ReaderBookService::continueReading() {
    showingBookEntry_ = false;
    showingReaderMenu_ = false;
    showingToc_ = false;
    if (hasProgress_ && currentTocIndex_ >= 0) {
        const int savedPage = currentPage_;
        const uint32_t resumeOffset = hasPendingResumeOffset_ ? pendingResumeOffset_ : 0;
        if (pageCount_ <= 0 && !buildChapterPages(currentTocIndex_)) return renderChapterPreview(currentTocIndex_);
        if (pageCount_ > 0) {
            if (hasPendingResumeOffset_) {
                currentPage_ = pageIndexForOffset(resumeOffset);
                hasPendingResumeOffset_ = false;
            } else {
                currentPage_ = min(max(savedPage, 0), pageCount_ - 1);
            }
            return renderCurrentReadingPage();
        }
    }
    if (tocCount_ > 0) return openTocEntry(0);
    showingToc_ = true;
    renderTocPage(0);
    return true;
}

bool ReaderBookService::restartReading() {
    showingBookEntry_ = false;
    showingReaderMenu_ = false;
    showingToc_ = false;
    hasProgress_ = false;
    currentPage_ = 0;
    currentTocIndex_ = -1;
    if (tocCount_ > 0) return openTocEntry(0);
    showingToc_ = true;
    renderTocPage(0);
    return true;
}

bool ReaderBookService::openReaderMenu() {
    if (!open_) return false;
    showingBookEntry_ = false;
    showingReaderMenu_ = true;
    showingToc_ = false;
    renderReaderMenuPage();
    return true;
}

bool ReaderBookService::closeReaderMenu() {
    if (!open_) return false;
    showingReaderMenu_ = false;
    // If the user changed an Vink-native layout option from the reader menu,
    // pagination was intentionally invalidated while preserving a byte offset.
    // Re-enter through continueReading() so the chapter is rebuilt and resumes
    // near the same text instead of making the "继续阅读" button look dead.
    if (pageCount_ <= 0 && currentTocIndex_ >= 0) return continueReading();
    return renderCurrentReadingPage();
}

bool ReaderBookService::cycleRefreshStrategy() {
    g_displayService.cycleReaderRefreshStrategy();
    showingReaderMenu_ = true;
    renderReaderMenuPage();
    return true;
}

bool ReaderBookService::toggleAntiAlias() {
    g_readerText.toggleAntiAlias();
    showingReaderMenu_ = true;
    renderReaderMenuPage();
    return true;
}

bool ReaderBookService::toggleUnderline() {
    g_readerText.toggleUnderline();
    invalidatePaginationForLayoutChange();
    showingReaderMenu_ = true;
    renderReaderMenuPage();
    return true;
}

bool ReaderBookService::togglePageTurnEffect() {
    g_readerText.togglePageTurnEffect();
    showingReaderMenu_ = true;
    renderReaderMenuPage();
    return true;
}

bool ReaderBookService::cycleLayoutPreset() {
    g_readerText.cycleLayoutPreset();
    invalidatePaginationForLayoutChange();
    showingReaderMenu_ = true;
    renderReaderMenuPage();
    return true;
}

void ReaderBookService::invalidatePaginationForLayoutChange() {
    // Layout options affect only the in-RAM streaming page window; the stable
    // `.vink-toc` chapter byte index stays valid. Preserve the current byte
    // offset so Continue rebuilds near the same text under the new layout.
    if (pageStarts_ && currentPage_ >= 0 && currentPage_ < pageCount_) {
        pendingResumeOffset_ = pageStarts_[currentPage_];
        hasPendingResumeOffset_ = true;
        hasProgress_ = true;
    }
    pageCount_ = 0;
    nextPreheatTocIndex_ = -1;
}

bool ReaderBookService::clearPageCache() {
    if (!open_ || !ensureSdReady()) return false;
    removeSidecarForCurrentBook(".vink-pages");
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
    removeSidecarForCurrentBook(".vink-toc");
    removeSidecarForCurrentBook(".vink-pages");
    return openBook(reopenPath);
}

bool ReaderBookService::nextPage() {
    if (showingBookEntry_) return continueReading();
    if (showingReaderMenu_) return closeReaderMenu();
    if (showingToc_) return nextTocPage();
    if (!open_ || pageCount_ <= 0) return false;
    if (currentPage_ + 1 < pageCount_) {
        currentPage_++;
        return renderCurrentReadingPage();
    }
    if (appendNextStreamingPage()) {
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
    if (showingReaderMenu_) return closeReaderMenu();
    if (showingToc_) return prevTocPage();
    if (!open_ || pageCount_ <= 0) return false;
    if (currentPage_ > 0) {
        currentPage_--;
        return renderCurrentReadingPage();
    }
    if (currentTocIndex_ > 0) {
        // Chapter starts are hard page boundaries. When the reader is on the
        // first page of a chapter, the true previous page is the tail page of
        // the previous chapter. Build that tail window on demand in RAM only;
        // do not resurrect persistent .vink-pages caches.
        if (!buildPreviousChapterTailPages(currentTocIndex_ - 1)) return false;
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
    lastRenderWasReadingPage_ = false;
    lastTapPageTurn_ = false;
    lastTapNextPage_ = false;
    if (!open_) return false;
    if (showingReaderMenu_) {
        if (x >= kEntryButtonX && x < kEntryButtonX + kEntryButtonW && y >= kEntryContinueY && y < kEntryContinueY + kEntryButtonH) return closeReaderMenu();
        if (x >= kEntryButtonX && x < kEntryButtonX + kEntryButtonW && y >= kEntryTocY && y < kEntryTocY + kEntryButtonH) return cycleRefreshStrategy();
        if (x >= kEntryButtonX && x < kEntryButtonX + kEntryButtonW && y >= kEntryRestartY && y < kEntryRestartY + kEntryButtonH) return toggleAntiAlias();
        if (x >= kEntryButtonX && x < kEntryButtonX + kEntryButtonW && y >= kEntryClearPagesY && y < kEntryClearPagesY + kEntryButtonH) return cycleLayoutPreset();
        if (x >= kEntryButtonX && x < kEntryButtonX + kEntryButtonW && y >= kEntryRebuildTocY && y < kEntryRebuildTocY + kEntryButtonH) return toggleUnderline();
        if (x >= kEntryButtonX && x < kEntryButtonX + kEntryButtonW && y >= kEntryPageTurnY && y < kEntryPageTurnY + kEntryButtonH) return togglePageTurnEffect();
        return false;
    }
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
        if (x < kPaperS3Width / 3) {
            lastTapPageTurn_ = true;
            lastTapNextPage_ = false;
            return prevPage();
        }
        if (x > (kPaperS3Width * 2) / 3) {
            lastTapPageTurn_ = true;
            lastTapNextPage_ = true;
            return nextPage();
        }
        return openReaderMenu();
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
        snprintf(body, sizeof(body), "已打开:%s\n未识别到目录。下一步将直接进入正文分页。", title_);
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
        // Fit visually with the UI font. Do not blindly byte-truncate titles.
        g_cjkText.fitTextToWidth(toc_[i].title.c_str(), titleBuf, sizeof(titleBuf), 430);
        // Multi-level indent: level-0 (卷/部) flush left; level-1 (章/回) 2-space indent;
        // level-2 (节) 4-space indent.
        const int8_t lv = toc_[i].level;
        if (lv == 1) snprintf(rows[rowCount], sizeof(rows[rowCount]), "    %s", titleBuf);
        else if (lv == 2) snprintf(rows[rowCount], sizeof(rows[rowCount]), "        %s", titleBuf);
        else snprintf(rows[rowCount], sizeof(rows[rowCount]), "%s", titleBuf);
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

uint32_t ReaderBookService::chapterContentStart(int index) const {
    if (index < 0 || index >= tocCount_ || !activeTextPath_[0]) return 0;
    uint32_t start = toc_[index].charOffset;
    if (start == 0) {
        // Drop UTF-8 BOM only. For real chapter entries, the TOC offset is the
        // page-splitting anchor and must point at the first byte of the title
        // line itself, e.g. the "第" in "第一章 你好". Layout/font changes rebuild
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
    const uint32_t fullEnd = chapterEndOffset(index);
    if (fullEnd <= start) return false;

    currentTocIndex_ = index;
    currentPage_ = 0;
    pageCount_ = 0;
    pageWindowStart_ = start;
    pageWindowEnd_ = start;
    pageWindowTruncated_ = false;
    pageStarts_[pageCount_++] = start;

    if (!measurePageEndOffset(start, fullEnd, pageWindowEnd_)) return false;
    pageWindowTruncated_ = pageWindowEnd_ < fullEnd;
    Serial.printf("[vink3][book] streaming page ready: toc=%d window=%lu-%lu\n",
                  index, static_cast<unsigned long>(pageWindowStart_),
                  static_cast<unsigned long>(pageWindowEnd_));
    return true;
}

bool ReaderBookService::buildPreviousChapterTailPages(int index) {
    if (index < 0 || index >= tocCount_ || !activeTextPath_[0] || !pageStarts_) return false;
    const uint32_t start = chapterContentStart(index);
    const uint32_t fullEnd = chapterEndOffset(index);
    if (fullEnd <= start) return false;

    currentTocIndex_ = index;
    currentPage_ = 0;
    pageCount_ = 0;
    pageWindowStart_ = start;
    pageWindowEnd_ = start;
    pageWindowTruncated_ = false;
    pageStarts_[pageCount_++] = start;

    uint32_t pageStart = start;
    uint32_t pageEnd = start;
    uint16_t measuredPages = 0;
    while (pageStart < fullEnd) {
        if (!measurePageEndOffset(pageStart, fullEnd, pageEnd)) return false;
        measuredPages++;
        if (pageEnd >= fullEnd) {
            pageWindowEnd_ = fullEnd;
            break;
        }

        if (pageCount_ < kMaxChapterPages) {
            pageStarts_[pageCount_++] = pageEnd;
        } else {
            memmove(pageStarts_, pageStarts_ + 1, sizeof(pageStarts_[0]) * (kMaxChapterPages - 1));
            pageStarts_[kMaxChapterPages - 1] = pageEnd;
            pageWindowTruncated_ = true;
        }
        pageStart = pageEnd;
        pageWindowEnd_ = pageEnd;

        // Long chapters can still be expensive when locating the previous
        // chapter's last page. Yield periodically so the ESP32-S3 watchdog and
        // background tasks are not starved. Results remain RAM-only.
        if ((measuredPages & 0x07) == 0) delay(1);
    }

    pageWindowStart_ = pageStarts_[0];
    currentPage_ = pageCount_ - 1;
    Serial.printf("[vink3][book] previous chapter tail ready: toc=%d pages=%d measured=%u window=%lu-%lu%s\n",
                  index, pageCount_, measuredPages,
                  static_cast<unsigned long>(pageWindowStart_),
                  static_cast<unsigned long>(pageWindowEnd_),
                  pageWindowTruncated_ ? " truncated" : "");
    return pageCount_ > 0 && pageWindowEnd_ > pageStarts_[currentPage_];
}

bool ReaderBookService::appendNextStreamingPage() {
    if (currentTocIndex_ < 0 || currentTocIndex_ >= tocCount_ || !pageStarts_) return false;
    const uint32_t fullEnd = chapterEndOffset(currentTocIndex_);
    if (pageWindowEnd_ >= fullEnd || pageCount_ >= kMaxChapterPages) return false;
    const uint32_t start = pageWindowEnd_;
    uint32_t nextEnd = start;
    if (!measurePageEndOffset(start, fullEnd, nextEnd)) return false;
    pageStarts_[pageCount_++] = start;
    pageWindowEnd_ = nextEnd;
    pageWindowTruncated_ = pageWindowEnd_ < fullEnd;
    return true;
}

bool ReaderBookService::renderCurrentReadingPage() {
    lastRenderWasReadingPage_ = false;
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
    char header[160];
    strlcpy(header, toc_[currentTocIndex_].title.c_str(), sizeof(header));
    const uint16_t progressPermille = activeTextSize_ > 0
        ? static_cast<uint16_t>(min<uint32_t>(1000, (static_cast<uint64_t>(start) * 1000ULL) / activeTextSize_))
        : static_cast<uint16_t>(tocCount_ > 0 ? (static_cast<uint32_t>(currentTocIndex_) * 1000UL) / tocCount_ : 0);
    g_readerText.renderTextPage(header, body, currentPage_ + 1, 0, currentRenderOptionsForOffset(start, chapterContentStart(currentTocIndex_)), progressPermille);
    lastRenderWasReadingPage_ = true;
    saveProgress();
    // Do not pre-paginate synchronously from the UI/state task. On large books
    // or malformed chapter spans, eager preheat can monopolize the ESP32-S3 long
    // enough to trip the watchdog when the user turns a page. Build the next
    // chapter on demand instead; it is safer than a surprise reboot.
    return true;
}

bool ReaderBookService::consumeReadingPageRendered() {
    const bool rendered = lastRenderWasReadingPage_;
    lastRenderWasReadingPage_ = false;
    return rendered;
}

bool ReaderBookService::consumeLastTapPageTurn() {
    const bool wasPageTurn = lastTapPageTurn_;
    lastTapPageTurn_ = false;
    return wasPageTurn;
}

bool ReaderBookService::consumeLastTapNextPage() {
    const bool wasNext = lastTapNextPage_;
    lastTapNextPage_ = false;
    return wasNext;
}

bool ReaderBookService::renderChapterPreview(int index) {
    lastRenderWasReadingPage_ = false;
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
    const uint16_t progressPermille = activeTextSize_ > 0
        ? static_cast<uint16_t>(min<uint32_t>(1000, (static_cast<uint64_t>(start) * 1000ULL) / activeTextSize_))
        : static_cast<uint16_t>(tocCount_ > 0 ? (static_cast<uint32_t>(index) * 1000UL) / tocCount_ : 0);
    g_readerText.renderTextPage(header, content, 1, 1, currentRenderOptionsForOffset(start, chapterContentStart(index)), progressPermille);
    return true;
}

} // namespace vink3

namespace vink3 {

void ReaderBookService::saveCurrentProgress() {
    if (!open_) return;
    saveProgress();
}

} // namespace vink3
