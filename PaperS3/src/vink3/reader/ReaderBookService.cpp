#include "ReaderBookService.h"
#include "ReaderTextRenderer.h"
#include "../../Config.h"
#include "../../TextCodec.h"
#include <esp_heap_caps.h>

namespace vink3 {

ReaderBookService g_readerBook;

bool ReaderBookService::begin() {
    ensureTocBuffer();
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

bool ReaderBookService::isTxtPath(const char* name) const {
    if (!name) return false;
    String s(name);
    s.toLowerCase();
    return s.endsWith(".txt");
}

void ReaderBookService::closeCurrent() {
    open_ = false;
    tocCount_ = 0;
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

void ReaderBookService::getTocCachePath(char* out, size_t len) const {
    // Keep generated TOC next to the book for easier file management:
    //   /books/foo.txt -> /books/foo.vink-toc
    // Use the original book path, not the temporary UTF-8 conversion path.
    strlcpy(out, bookPath_, len);
    char* slash = strrchr(out, '/');
    char* dot = strrchr(out, '.');
    if (dot && (!slash || dot > slash)) {
        strlcpy(dot, ".vink-toc", len - static_cast<size_t>(dot - out));
    } else {
        strlcat(out, ".vink-toc", len);
    }
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

bool ReaderBookService::openFirstBook() {
    if (!ensureSdReady()) return false;
    File dir = SD.open(BOOKS_DIR);
    if (!dir || !dir.isDirectory()) return false;
    File f = dir.openNextFile();
    while (f) {
        if (!f.isDirectory() && isTxtPath(f.name())) {
            char path[160];
            if (f.name()[0] == '/') strlcpy(path, f.name(), sizeof(path));
            else snprintf(path, sizeof(path), "%s/%s", BOOKS_DIR, f.name());
            f.close();
            dir.close();
            return openBook(path);
        }
        f = dir.openNextFile();
    }
    dir.close();
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
    return true;
}

void ReaderBookService::renderOpenOrHelp() {
    if (!open_ && !openFirstBook()) {
        g_readerText.renderTextPage(
            "没有找到 TXT",
            "请把 .txt 文件放到 SD 卡 /books 目录。\n"
            "v0.3 会自动识别 UTF-8 / GBK 文本、生成目录缓存，然后进入正文阅读。",
            1,
            1);
        return;
    }
    renderTocPage(0);
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
    const int start = page * kTocEntriesPerPage;
    const int end = min(tocCount_, start + kTocEntriesPerPage);
    size_t used = 0;
    used += snprintf(body + used, sizeof(body) - used, "目录共 %d 条\n", tocCount_);
    for (int i = start; i < end && used < sizeof(body) - 96; ++i) {
        used += snprintf(body + used, sizeof(body) - used, "%03d  %s\n", i + 1, toc_[i].title.c_str());
    }
    g_readerText.renderTextPage(title_, body, page + 1, totalPages);
}

} // namespace vink3
