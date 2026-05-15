#pragma once
#include <Arduino.h>
#include <M5Unified.h>
#include "../state/Messages.h"

namespace vink3 {

enum class UiAction : uint8_t {
    None,
    TabReader,
    TabLibrary,
    TabTransfer,
    TabSettings,
    OpenCurrentBook,
    RestartCurrentBook,
    OpenCurrentBookToc,
    OpenLibrary,
    OpenTransfer,
    OpenSettings,
    OpenDiagnostics,
    OpenSystemLogs,
    ClearSystemLogs,
    RequestShutdown,
    ConfirmShutdown,
    CancelShutdown,
    CycleReaderRefreshStrategy,
    ToggleReaderAntiAlias,
    CycleReaderAntialiasProfile,
    CycleReaderLayoutPreset,
    CycleReaderPageMargin,
    CycleReaderLineSpacing,
    ToggleReaderPageTurnEffect,
    CycleReaderPageTurnProfile,
    CycleReaderGhostingProfile,
    CycleReaderFontSize,
    CycleReaderFontSource,
    IncreaseSdFontSize,
    DecreaseSdFontSize,
    IncreaseSdFontSizeBig,
    DecreaseSdFontSizeBig,
    ToggleWifiAp,
    BackHome,
    OpenReaderSettings,
    BackToSettings,
    OpenSystemSettings,
    ToggleDoubleTapUnlock,
};

class VinkUiRenderer {
public:
    bool begin(M5Canvas* canvas);
    void renderBoot();
    void renderHome(SystemState state);
    void renderReaderHome(const char* bookTitle = nullptr, const char* bookPath = nullptr,
                          const char* progressText = nullptr, bool hasLastBook = false,
                          const char* const* recentTitles = nullptr,
                          const char* const* recentSubs = nullptr,
                          int recentCount = 0);
    void renderShelfGrid(const char* const* titles, const char* const* subs,
                         int bookCount, uint16_t page, uint16_t totalPages,
                         int cols, int rows, bool showBrowserEntry);
    void drawBookCard(int16_t x, int16_t y, int16_t w, int16_t h,
                      const char* title, const char* subtitle, bool isEmpty);
    void renderLibrary();
    void renderUiListPage(SystemState active, const char* title, const char* summary,
                          const char* const* rows, int rowCount, int16_t rowY, int16_t rowH,
                          uint16_t page, uint16_t totalPages, int activeRow = -1);
    void renderUiActionPage(SystemState active, const char* title,
                            const char* const* infoLines, int infoCount,
                            const char* const* actions, int actionCount);
    void renderTransfer();
    void renderSettings();
    void renderReaderSettings();  // sub-page: all reader options
    bool isShowingReaderSettings() const { return showReaderSettings_; }
    void showReaderSettings();
    void hideReaderSettings();
    void renderSystemSettings();  // sub-page: system options
    bool isShowingSystemSettings() const { return showSystemSettings_; }
    void showSystemSettings();
    void hideSystemSettings();
    void renderReaderMenuOverlay(const char* bookTitle, const char* chapterTitle,
                                 const char* refreshLabel, bool antiAliasOn,
                                 const char* layoutLabel, bool underlineOn,
                                 bool pageTurnEffectOn);
    void renderDiagnostics(const Message& lastTouch, const char* eventName);
    void renderSystemLogs();
    bool scrollSystemLogs(int8_t pages);
    void resetSystemLogPage();
    void renderShutdownConfirm();
    void renderLockScreen(const char* bookTitle = nullptr);
    void renderShutdown(const char* reason);
    void renderPowerOffReady();

    UiAction hitTest(SystemState state, int16_t x, int16_t y) const;
    UiAction hitTestTabs(int16_t x, int16_t y) const;
    bool scrollSettings(int8_t pages);
    void resetSettingsScroll();

private:
    void clear();
    void drawStatusBar(const char* title);
    void drawTabs(SystemState active);
    void drawCard(int16_t x, int16_t y, int16_t w, int16_t h, const char* title, const char* body, bool smallBody = false);
    void drawButton(int16_t x, int16_t y, int16_t w, int16_t h, const char* label, bool primary = false);
    void drawThickBorder(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
    void drawSettingsGroup(int16_t x, int16_t y, const char* title, const char* const* rowLabels, const char* const* rowValues, int rowCount);
    void drawSettingsRow(int16_t rowX, int16_t y, int16_t rowW, const char* label, const char* value);
    void drawMenuItem(int16_t x, int16_t y, int16_t w, int16_t h,
                      const char* label, bool isToggle, bool isOn, const char* altText);

    M5Canvas* canvas_ = nullptr;
    bool showReaderSettings_ = false;
    bool showSystemSettings_ = false;
    bool lastReaderHomeHasBook_ = false;
    int16_t settingsScrollY_ = 0;
    int16_t readerSettingsScrollY_ = 0;
    int16_t systemSettingsScrollY_ = 0;
    // Content height below kContentY, set by each render function so
    // scrollSettings() stays card-count-agnostic.
    int16_t readerSettingsContentH_ = 0;
    int16_t systemSettingsContentH_ = 0;
    int16_t mainSettingsContentH_ = 0;
    uint8_t systemLogPage_ = 0;  // 0 = latest page; higher pages show older wrapped rows
};

extern VinkUiRenderer g_uiRenderer;

} // namespace vink3
