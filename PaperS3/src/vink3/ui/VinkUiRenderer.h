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
    OpenLibrary,
    OpenTransfer,
    OpenSettings,
    OpenDiagnostics,
    RequestShutdown,
    ConfirmShutdown,
    CancelShutdown,
    CycleReaderRefreshStrategy,
    ToggleReaderAntiAlias,
    CycleReaderLayoutPreset,
    CycleReaderPageMargin,
    CycleReaderLineSpacing,
    ToggleReaderPageTurnEffect,
    ToggleWifiAp,
    BackHome,
    OpenReaderSettings,
    BackToSettings,
};

class VinkUiRenderer {
public:
    bool begin(M5Canvas* canvas);
    void renderBoot();
    void renderHome(SystemState state);
    void renderReaderHome(const char* bookTitle = nullptr, const char* bookPath = nullptr,
                          const char* progressText = nullptr, bool hasLastBook = false);
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
    void renderReaderMenuOverlay(const char* bookTitle, const char* chapterTitle,
                                 const char* refreshLabel, bool antiAliasOn,
                                 const char* layoutLabel, bool underlineOn,
                                 bool pageTurnEffectOn);
    void renderDiagnostics(const Message& lastTouch, const char* eventName);
    void renderShutdownConfirm();
    void renderShutdown(const char* reason);
    void renderPowerOffReady();

    UiAction hitTest(SystemState state, int16_t x, int16_t y) const;

private:
    void clear();
    void drawStatusBar(const char* title);
    void drawTabs(SystemState active);
    void drawCard(int16_t x, int16_t y, int16_t w, int16_t h, const char* title, const char* body);
    void drawButton(int16_t x, int16_t y, int16_t w, int16_t h, const char* label, bool primary = false);
    void drawThickBorder(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
    void drawSettingsGroup(int16_t x, int16_t y, const char* title, const char* const* rowLabels, const char* const* rowValues, int rowCount);
    void drawSettingsRow(int16_t rowX, int16_t y, int16_t rowW, const char* label, const char* value);
    void drawMenuItem(int16_t x, int16_t y, int16_t w, int16_t h,
                      const char* label, bool isToggle, bool isOn, const char* altText);
    UiAction hitTestTabs(int16_t x, int16_t y) const;

    M5Canvas* canvas_ = nullptr;
    bool showReaderSettings_ = false;
};

extern VinkUiRenderer g_uiRenderer;

} // namespace vink3
