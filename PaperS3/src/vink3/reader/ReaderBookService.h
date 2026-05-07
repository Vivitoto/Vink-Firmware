#pragma once
#include <Arduino.h>
#include <SD.h>
#include "../../ChapterDetector.h"

namespace vink3 {

class ReaderBookService {
public:
    bool begin();
    bool openFirstBook();
    bool openBook(const char* path);
    bool isOpen() const { return open_; }
    int tocCount() const { return tocCount_; }
    const char* title() const { return title_; }
    const char* path() const { return bookPath_; }

    void renderOpenOrHelp();
    void renderCurrent();
    void renderBookLoadingPage(const char* stage);
    void renderChapterLoadingPage(int index);
    void renderBookEntryPage();
    void renderLibraryPage(uint16_t page = 0);
    bool nextLibraryPage();
    bool prevLibraryPage();
    bool handleLibraryTap(int16_t x, int16_t y);
    void renderTocPage(uint16_t page = 0);
    bool nextPage();
    bool prevPage();
    bool nextTocPage();
    bool prevTocPage();
    bool handleTap(int16_t x, int16_t y);
    bool openTocEntry(int index);
    void saveCurrentProgress();

private:
    static constexpr int kMaxTocEntries = 1200;
    static constexpr int kTocEntriesPerPage = 13;
    static constexpr int kMaxBooks = 80;
    static constexpr int kBooksPerPage = 12;
    static constexpr uint8_t kMaxLibraryScanDepth = 6;
    static constexpr uint8_t kBookHasTocCache = 0x01;
    static constexpr uint8_t kBookHasProgress = 0x02;
    static constexpr uint8_t kBookHasPageCache = 0x04;
    static constexpr int16_t kListFirstRowY = 204;
    static constexpr int16_t kListRowH = 52;
    static constexpr int16_t kTocFirstRowY = kListFirstRowY;
    static constexpr int16_t kTocRowH = 48;
    static constexpr int16_t kEntryButtonX = 70;
    static constexpr int16_t kEntryButtonW = 400;
    static constexpr int16_t kEntryButtonH = 64;
    static constexpr int16_t kEntryContinueY = 560;
    static constexpr int16_t kEntryTocY = 660;
    static constexpr int16_t kEntryRestartY = 760;
    static constexpr int kMaxChapterPages = 512;

    bool ensureTocBuffer();
    bool ensureBookBuffers();
    bool ensureSdReady();
    bool scanBooks();
    void scanBookDir(const char* dirPath, uint8_t depth);
    bool addBookPath(const char* path);
    void sortBooks();
    void swapBookEntries(int a, int b);
    bool isTxtPath(const char* name) const;
    void closeCurrent();
    void setTitleFromPath(const char* path);
    void getSidecarPath(char* out, size_t len, const char* suffix) const;
    void getSidecarPathForBook(char* out, size_t len, const char* bookPath, const char* suffix) const;
    uint8_t detectBookFlags(const char* bookPath) const;
    uint32_t bookFileSize(const char* bookPath) const;
    void formatBytes(uint32_t bytes, char* out, size_t len) const;
    void formatBookFlags(uint8_t flags, char* out, size_t len) const;
    void showBlockingOpenStatus(const char* stage);
    void showBlockingChapterStatus(int index);
    void getTocCachePath(char* out, size_t len) const;
    void getProgressPath(char* out, size_t len) const;
    void getPageCachePath(char* out, size_t len) const;
    bool loadTocCache();
    void saveTocCache();
    uint32_t activeTextSize() const;
    bool loadProgress();
    void saveProgress();
    bool loadChapterPageCache(int index, uint32_t start, uint32_t end);
    void saveChapterPageCache(int index, uint32_t start, uint32_t end);
    bool buildChapterPages(int index);
    bool renderCurrentReadingPage();
    bool renderChapterPreview(int index);
    bool continueReading();
    bool restartReading();
    uint32_t chapterContentStart(int index);
    uint32_t chapterEndOffset(int index);
    size_t trimUtf8Tail(char* text, size_t len) const;

    bool sdReady_ = false;
    bool open_ = false;
    char bookPath_[160] = {0};
    char activeTextPath_[160] = {0};
    char title_[72] = {0};
    ChapterDetectResult* toc_ = nullptr;
    uint32_t* pageStarts_ = nullptr;
    char (*bookPaths_)[160] = nullptr;
    char (*bookTitles_)[72] = nullptr;
    uint8_t* bookFlags_ = nullptr;
    int bookCount_ = 0;
    uint16_t bookPage_ = 0;
    bool booksScanned_ = false;
    int tocCount_ = 0;
    uint16_t tocPage_ = 0;
    int currentTocIndex_ = -1;
    int pageCount_ = 0;
    int currentPage_ = 0;
    bool hasProgress_ = false;
    bool showingBookEntry_ = false;
    bool showingToc_ = true;
};

extern ReaderBookService g_readerBook;

} // namespace vink3
