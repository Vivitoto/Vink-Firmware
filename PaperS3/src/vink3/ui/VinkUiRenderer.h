#pragma once
#include <Arduino.h>
#include <M5Unified.h>
#include "../state/Messages.h"

namespace vink3 {

enum class UiAction : uint8_t {
    None,

    // ── Tab navigation ─────────────────────────────────────────────
    TabReader,
    TabLibrary,
    TabTransfer,
    TabSettings,

    // ── Top-level open actions ───────────────────────────────────
    OpenCurrentBook,
    OpenLibrary,
    OpenTransfer,
    OpenSettings,
    OpenDiagnostics,
    RequestShutdown,
    StartLegadoSync,
    BackHome,

    // ── Settings sub-pages ───────────────────────────────────────
    OpenSettingsLayout,
    OpenSettingsRefresh,
    OpenSettingsWifi,
    OpenSettingsLegado,
    OpenSettingsSystem,

    // ── In-page value cycling ────────────────────────────────────
    CycleRefreshFrequency,
    CycleFontSize,
    CycleFontFamily,
    CycleLineSpacing,
    CycleSimplified,
    CycleJustify,
    CycleMarginLeft,

    // ── Settings save / toggle ───────────────────────────────────
    SaveLegadoSettings,
    CycleLegadoEnabled,
    CycleLegadoSyncEnabled,
    SaveWifiSettings,

    // ── Transfer / sync sub-pages ────────────────────────────────
    OpenTransferLegado,
    OpenTransferWifiAp,
    OpenTransferUsb,
    OpenTransferExport,

    // ── WiFi mode actions ───────────────────────────────────────
    CycleWifiMode,
    SetWifiOff,
    SetWifiApWebUi,
    SetWifiSta,
    ToggleWifiAp,
    ToggleWebUi,
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
    void renderSettingsLayout();
    void renderSettingsRefresh();
    void renderSettingsWifi();
    void renderSettingsLegado();
    void renderSettingsSystem();
    void renderTransferLegadoStatus();
    void renderTransferWifiAp();
    void renderTransferUsb();
    void renderTransferWebDav();
    void renderTransferExport();
    void renderDiagnostics(const Message& lastTouch, const char* eventName);
    void renderShutdown(const char* reason);
    void renderLegadoSync(const char* status);
    void renderLegadoSync(const char* status, int bookCount, const char* errorMsg);

    // Shared time/battery formatters for use by other services (e.g. reader overlay)
    static void formatTimeStr(char* out, size_t outSize);
    static void formatBatterySimple(char* out, size_t outSize);

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
    void drawSettingsRowRaw(int16_t rowTopY, const char* label, const char* value);
    void drawCyclingRow(int16_t rowTopY, const char* label, const char* value);
    UiAction hitTestTabs(int16_t x, int16_t y) const;

    M5Canvas* canvas_ = nullptr;
};

extern VinkUiRenderer g_uiRenderer;

} // namespace vink3
