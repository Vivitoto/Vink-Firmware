#pragma once
#include <Arduino.h>
#include <SD.h>
#include "../../ChapterDetector.h"
#include "../ReadPaper176.h"
#include "ReaderTextRenderer.h"

namespace vink3 {

struct ShelfEntry {
    uint64_t hash;
    uint32_t lastOpenMs;
    uint32_t addedMs;
    char path[160];
};

class ReaderBookService {
public:
    bool begin();
    bool openFirstBook();
    bool openBook(const char* path);
    bool isOpen() const { return open_; }
    int tocCount() const { return tocCount_; }
    const char* title() const { return title_; }
    const char* path() const { return bookPath_; }

    void renderReaderHome();
    void renderShelfGrid(uint16_t page = 0);
    bool nextShelfPage();
    bool prevShelfPage();
    bool handleShelfTap(int16_t x, int16_t y);
    bool handleReaderHomeTap(int16_t x, int16_t y);
    bool openLastBook();
    bool restartReading();
    void showTocForCurrentBook();
    void renderOpenOrHelp();
    void renderCurrent();
    void renderBookLoadingPage(const char* stage);
    void renderChapterLoadingPage(int index);
    void renderBookEntryPage();
    void renderReaderMenuPage();
    void renderLibraryPage(uint16_t page = 0);
    bool nextLibraryPage();
    bool prevLibraryPage();
    bool handleLibraryTap(int16_t x, int16_t y);
    bool lastLibraryTapOpenedBook() const { return lastLibraryTapOpenedBook_; }
    void renderTocPage(uint16_t page = 0);
    bool nextPage();
    bool prevPage();
    bool nextTocPage();
    bool prevTocPage();
    bool handleTap(int16_t x, int16_t y);
    bool openTocEntry(int index);
    void saveCurrentProgress();
    void invalidatePaginationForLayoutChange();
    bool ensureSdReadyForTransfer();
    bool consumeReadingPageRendered();
    bool consumeLastTapPageTurn();
    bool consumeLastTapNextPage();
    bool consumeLastTapBackHome();

private:
    static constexpr int kMaxTocEntries = 2000;
    static constexpr int kTocEntriesPerPage = 15;
    static constexpr int kMaxBooks = 160;
    static constexpr int kBooksPerPage = 12;
    static constexpr int kMaxShelfBooks = 36;
    static constexpr int kShelfBooksPerPage = 6;
    static constexpr int kShelfCols = 3;
    static constexpr int kShelfRows = 2;
    static constexpr uint8_t kMaxLibraryScanDepth = 6;
    static constexpr uint8_t kBookHasTocCache = 0x01;
    static constexpr uint8_t kBookHasProgress = 0x02;
    static constexpr uint8_t kBookHasPageCache = 0x04;
    static constexpr uint8_t kBookIsDirectory = 0x80;
    // List row touch width mirrors VinkUiRenderer::renderUiListPage() x/w.
    static constexpr int16_t kListTouchX = 28;
    static constexpr int16_t kListTouchW = kPaperS3Width - 56;
    static constexpr int16_t kListFirstRowY = 204;
    static constexpr int16_t kListRowH = 52;
    static constexpr int16_t kTocFirstRowY = 176;
    static constexpr int16_t kTocRowH = 48;
    // Shelf grid geometry must match VinkUiRenderer drawBookCard() pixel-for-pixel.
    static constexpr int16_t kShelfCardW = 148;
    static constexpr int16_t kShelfCardH = 170;
    static constexpr int16_t kShelfCardGap = 16;
    static constexpr int16_t kShelfGridY = 230;
    static constexpr int16_t kShelfBrowserEntryY = 158;
    static constexpr int16_t kShelfBrowserEntryH = 52;
    static constexpr int16_t kUiMarginX = 28;
    static constexpr int16_t kUiContentW = kPaperS3Width - kUiMarginX * 2;
    // Must match VinkUiRenderer::renderUiActionPage() button geometry.
    static constexpr int16_t kEntryButtonX = 64;
    static constexpr int16_t kEntryButtonW = 416;
    static constexpr int16_t kEntryButtonH = 52;
    static constexpr int16_t kEntryButtonGap = 16;
    static constexpr int16_t kEntryContinueY = 500;
    static constexpr int16_t kEntryTocY = kEntryContinueY + (kEntryButtonH + kEntryButtonGap);
    static constexpr int16_t kEntryRestartY = kEntryTocY + (kEntryButtonH + kEntryButtonGap);
    static constexpr int16_t kEntryRebuildTocY = kEntryRestartY + (kEntryButtonH + kEntryButtonGap);
    static constexpr int16_t kEntryPageTurnY = kEntryRebuildTocY + (kEntryButtonH + kEntryButtonGap);
    static constexpr int kMaxChapterPages = 512;

    bool ensureTocBuffer();
    bool ensureBookBuffers();
    bool ensureSdReady();
    bool scanBooks();
    void scanBookDir(const char* dirPath, uint8_t depth);
    bool addLibraryEntry(const char* path, bool isDirectory, const char* displayName = nullptr);
    bool addBookPath(const char* path);
    void sortBooks();
    void swapBookEntries(int a, int b);
    bool isTxtPath(const char* name) const;
    bool isBookPath(const char* name) const;
    void normalizeChildPath(const char* dirPath, const char* rawName, char* out, size_t len) const;
    void setDisplayNameFromPath(char* out, size_t len, const char* path) const;
    void parentDirOf(const char* path, char* out, size_t len) const;
    void closeCurrent();
    void setTitleFromPath(const char* path);
    void getSidecarPath(char* out, size_t len, const char* suffix) const;
    void getSidecarPathForBook(char* out, size_t len, const char* bookPath, const char* suffix) const;
    void getLegacySidecarPathForBook(char* out, size_t len, const char* bookPath, const char* suffix) const;
    uint64_t hashBookPath(const char* bookPath) const;
    void formatHashHex(uint64_t hash, char* out, size_t len) const;
    void ensureParentDirForPath(const char* path) const;
    bool removeSidecarForCurrentBook(const char* suffix);
    uint8_t detectBookFlags(const char* bookPath) const;
    uint32_t bookFileSize(const char* bookPath) const;
    uint64_t sampleFileFingerprint(const char* path, uint32_t* outSize = nullptr) const;
    void refreshActiveTextIdentity();
    void formatBytes(uint32_t bytes, char* out, size_t len) const;
    void formatBookFlags(uint8_t flags, char* out, size_t len) const;
    void showBlockingOpenStatus(const char* stage);
    void showBlockingChapterStatus(int index);
    void getTocCachePath(char* out, size_t len) const;
    void getProgressPath(char* out, size_t len) const;
    void getPageCachePath(char* out, size_t len) const;
    void getLastBookPath(char* out, size_t len) const;
    bool loadTocCache();
    void saveTocCache();
    uint32_t activeTextSize() const;
    bool loadProgress();
    void saveProgress();
    bool loadLastBookPath(char* out, size_t len) const;
    void saveLastBookPath();
    bool addToShelf(const char* bookPath);
    bool loadShelf();
    bool saveShelf();
    bool readProgressForBook(const char* bookPath, uint16_t& chapter, uint16_t& page) const;
    bool readProgressPercentForBook(const char* bookPath, char* out, size_t len);
    bool measurePageEndOffset(uint32_t start, uint32_t fullEnd, uint32_t& outEnd) const;
    bool loadChapterPageCache(int index, uint32_t start, uint32_t end);
    void saveChapterPageCache(int index, uint32_t start, uint32_t end);
    uint32_t readerLayoutKey() const;
    bool fileOffsetStartsParagraph(uint32_t offset, uint32_t chapterStart) const;
    ReaderRenderOptions currentRenderOptionsForOffset(uint32_t offset, uint32_t chapterStart) const;
    int pageIndexForOffset(uint32_t offset) const;
    void saveChapterPageCacheData(int index, uint32_t start, uint32_t end, const uint32_t* starts, int count);
    bool chapterPageCacheValid(int index, uint32_t start, uint32_t end);
    bool preheatChapterPageCache(int index);
    void resetPreheatCursor(int afterChapter);
    void maybePreheatNextChapter();
    bool buildChapterPages(int index);
    bool buildPreviousChapterTailPages(int index);
    bool appendNextStreamingPage();
    bool renderCurrentReadingPage();
    bool renderChapterPreview(int index);
    bool continueReading();
    bool openReaderMenu();
    bool closeReaderMenu();
    bool cycleRefreshStrategy();
    bool toggleAntiAlias();
    bool toggleUnderline();
    bool togglePageTurnEffect();
    bool cycleLayoutPreset();
    bool cyclePageMargin();
    bool rebuildTocCache();
    uint32_t chapterContentStart(int index) const;
    uint32_t chapterEndOffset(int index);
    size_t trimUtf8Tail(char* text, size_t len) const;

    bool sdReady_ = false;
    bool open_ = false;
    char bookPath_[160] = {0};
    char activeTextPath_[160] = {0};
    char currentLibraryDir_[160] = "/books";
    char title_[72] = {0};
    uint32_t activeTextSize_ = 0;
    uint64_t activeTextFingerprint_ = 0;
    ChapterDetectResult* toc_ = nullptr;
    uint32_t* pageStarts_ = nullptr;
    char (*bookPaths_)[160] = nullptr;
    char (*bookTitles_)[72] = nullptr;
    uint8_t* bookFlags_ = nullptr;
    int bookCount_ = 0;
    uint16_t bookPage_ = 0;
    bool booksScanned_ = false;
    bool lastLibraryTapOpenedBook_ = false;
    uint16_t shelfPage_ = 0;
    bool shelfShowingBrowser_ = false;
    ShelfEntry* shelf_ = nullptr;
    int shelfCount_ = 0;
    int tocCount_ = 0;
    uint16_t tocPage_ = 0;
    int currentTocIndex_ = -1;
    int pageCount_ = 0;
    int currentPage_ = 0;
    uint32_t pageWindowStart_ = 0;
    uint32_t pageWindowEnd_ = 0;
    bool pageWindowTruncated_ = false;
    int nextPreheatTocIndex_ = -1;
    uint32_t pendingResumeOffset_ = 0;
    bool hasPendingResumeOffset_ = false;
    bool hasProgress_ = false;
    bool lastRenderWasReadingPage_ = false;
    bool lastTapPageTurn_ = false;
    bool lastTapNextPage_ = false;
    bool lastTapBackHome_ = false;
    bool showingBookEntry_ = false;
    bool showingReaderMenu_ = false;
    bool showingToc_ = true;
};

extern ReaderBookService g_readerBook;

} // namespace vink3
