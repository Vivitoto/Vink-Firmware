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
    StartLegadoSync,
    BackHome,
};

class VinkUiRenderer {
public:
    bool begin(M5Canvas* canvas);
    void renderBoot();
    void renderHome(SystemState state);
    void renderReaderHome();
    void renderLibrary();
    void renderTransfer();
    void renderSettings();
    void renderDiagnostics(const Message& lastTouch, const char* eventName);
    void renderLegadoSync(const char* status);

    UiAction hitTest(SystemState state, int16_t x, int16_t y) const;

private:
    void clear();
    void drawStatusBar(const char* title);
    void drawTabs(SystemState active);
    void drawCard(int16_t x, int16_t y, int16_t w, int16_t h, const char* title, const char* body);
    void drawButton(int16_t x, int16_t y, int16_t w, int16_t h, const char* label);
    void drawSettingsGroup(int16_t y, const char* title, const char* row1, const char* row1Value, const char* row2, const char* row2Value);
    void drawSettingsRow(int16_t y, const char* label, const char* value);
    void drawFooterHint(const char* hint);
    UiAction hitTestTabs(int16_t x, int16_t y) const;

    M5Canvas* canvas_ = nullptr;
};

extern VinkUiRenderer g_uiRenderer;

} // namespace vink3
