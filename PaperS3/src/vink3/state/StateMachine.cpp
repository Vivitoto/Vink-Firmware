#include "StateMachine.h"
#include "../config/ConfigService.h"
#include "../display/DisplayService.h"
#include "../sync/LegadoService.h"
#include "../sync/WifiService.h"
#include "../reader/ReaderBookService.h"
#include "../reader/ReaderTextRenderer.h"
#include "../ui/VinkUiRenderer.h"
#include "../input/InputService.h"
#include "../../FontManager.h"
#include "../ReadPaper176.h"
#include <esp_sleep.h>

namespace vink3 {

StateMachine g_stateMachine;

namespace {
SystemState tabStateForAction(UiAction action) {
    switch (action) {
        case UiAction::TabReader: return SystemState::Reader;
        case UiAction::TabLibrary: return SystemState::Library;
        case UiAction::TabTransfer: return SystemState::Transfer;
        case UiAction::TabSettings: return SystemState::Settings;
        default: return SystemState::Home;
    }
}

String buildLegadoBaseUrl(const VinkConfig& cfg) {
    String base = cfg.legadoHost;
    base.trim();
    if (base.isEmpty()) return base;
    if (!base.startsWith("http://") && !base.startsWith("https://")) {
        base = "http://" + base;
    }
    const int scheme = base.indexOf("://");
    const int hostStart = scheme >= 0 ? scheme + 3 : 0;
    int slash = base.indexOf('/', hostStart);
    if (slash < 0) slash = base.length();
    const String hostPort = base.substring(hostStart, slash);
    if (hostPort.indexOf(':') < 0 && cfg.legadoPort > 0) {
        base = base.substring(0, slash) + ":" + String(cfg.legadoPort) + base.substring(slash);
    }
    return base;
}

// Wait indefinitely for the power button to be released.
// On PaperS3 the power key must be released before we enter deep sleep,
// otherwise releasing it after esp_deep_sleep_start() wakes the device.
// If the key is still held after 10 s we give up and enter deep sleep anyway
// (the shutdown has already been triggered via M5.Power.powerOff).
static void waitPowerKeyRelease(uint32_t timeoutMs = 10000) {
    const uint32_t start = millis();
    while (M5.BtnPWR.isPressed()) {
        M5.update();
        delay(30);
        if (millis() - start >= timeoutMs) {
            Serial.println("[vink3][power] BtnPWR release timeout, proceeding anyway");
            break;
        }
    }
}

static void enterSleep(const char* reason) {
    // Auto sleep is deliberately disabled by default until the PaperS3 wake
    // source is validated on real hardware. GPIO38 is SD MOSI, not GT911 INT;
    // do not arm it as a wake source. If the user explicitly enables the setting
    // before wake validation, show a safe notice and keep the device awake.
    Serial.println("[vink3][power] auto sleep requested but wake path is not validated; staying awake");
    g_uiRenderer.renderShutdown(reason ? "自动休眠未启用：唤醒路径待真机验证" : "休眠待验证");
    g_displayService.enqueueFull(false, 100);
    g_stateMachine.onActivity();
}

void shutdownPaperS3(const char* reason) {
    Serial.println("[vink3][power] shutdown requested");
    g_readerBook.saveCurrentProgress();
    g_uiRenderer.renderShutdown(reason ? reason : "正在关机");
    g_displayService.enqueueFull(true, 100);
    g_displayService.waitIdle(5000);
    delay(300);

    // Official/factory order: sleep the EPD, wait for it, then ask M5Unified
    // to power off. Do not add a manual GPIO44 pulse unless real-device logs
    // prove M5Unified's PaperS3 LOW→HIGH pulse sequence is insufficient.
    waitPowerKeyRelease();  // blocking; do NOT enter sleep while button is held
    M5.Display.sleep();
    M5.Display.waitDisplay();
    delay(200);
    M5.Power.powerOff();

    // After powerOff() returns the PMIC may still be ramping down.
    // Give it up to 2 s to actually cut power. If the button is released
    // during this window the device stays off; if it is still held the
    // device will wake from deep sleep (see below) and the user sees a boot -
    // this is unavoidable when powerOff() does not guarantee hard power-cut.
    delay(2000);

    // Final safety net: ensure the button is released before deep sleep.
    // If it is still held, enter deep sleep anyway — the shutdown has been
    // triggered; the next release will wake the device which is expected.
    waitPowerKeyRelease(8000);

    // Do not configure a touch wake source here yet: official GT911 INT is
    // GPIO48, while GPIO38 is SD MOSI. Deep-sleep wake capability for GPIO48
    // must be verified before enabling touch wake. Use deep sleep only as a
    // last-resort CPU halt if powerOff() did not cut power.
    esp_deep_sleep_start();
    // esp_deep_sleep_start() never returns; if we are here something is wrong
    // with the CPU or the compiler.
    Serial.println("[vink3][power] WARNING: deep_sleep_start returned!");
    for (;;) delay(1000);  // spin forever
}

void suppressAfterTransition(uint32_t cooldownMs = 220) {
    g_inputService.suppressUntilRelease(cooldownMs);
}

void renderState(SystemState state) {
    switch (state) {
        case SystemState::Home:
        case SystemState::Reader:
            g_uiRenderer.renderReaderHome();
            break;
        case SystemState::ReaderMenu:
            g_readerBook.renderCurrent();
            break;
        case SystemState::Library:
            g_readerBook.renderLibraryPage();
            break;
        case SystemState::Transfer:
            g_uiRenderer.renderTransfer();
            break;
        case SystemState::Settings:
            g_uiRenderer.renderSettings();
            break;
        case SystemState::SettingsLayout:
            g_uiRenderer.renderSettingsLayout();
            break;
        case SystemState::SettingsRefresh:
            g_uiRenderer.renderSettingsRefresh();
            break;
        case SystemState::SettingsWifi:
            g_uiRenderer.renderSettingsWifi();
            break;
        case SystemState::SettingsLegado:
            g_uiRenderer.renderSettingsLegado();
            break;
        case SystemState::SettingsSystem:
            g_uiRenderer.renderSettingsSystem();
            break;
        case SystemState::TransferLegadoStatus:
            g_uiRenderer.renderTransferLegadoStatus();
            break;
        case SystemState::TransferWifiAp:
            g_uiRenderer.renderTransferWifiAp();
            break;
        case SystemState::TransferUsb:
            g_uiRenderer.renderTransferUsb();
            break;
        case SystemState::TransferExport:
            g_uiRenderer.renderTransferExport();
            break;
        case SystemState::Diagnostics:
        {
            Message blank;
            blank.timestampMs = millis();
            g_uiRenderer.renderDiagnostics(blank, "等待触摸");
            break;
        }
        case SystemState::LegadoSync:
            g_uiRenderer.renderLegadoSync("Legado 同步服务已就绪");
            break;
        default:
            g_uiRenderer.renderHome(state);
            break;
    }
}
} // namespace

bool StateMachine::begin(uint8_t queueLen) {
    if (!queue_) {
        queue_ = xQueueCreate(queueLen, sizeof(Message));
        if (!queue_) return false;
    }
    if (!task_) {
        BaseType_t ok = xTaskCreatePinnedToCore(taskThunk, "vink3-state", 8192, this, 3, &task_, 1);
        if (ok != pdPASS) {
            task_ = nullptr;
            return false;
        }
    }
    Serial.println("[vink3][state] service started");
    return true;
}

bool StateMachine::post(const Message& message, uint32_t timeoutMs) {
    if (!queue_) return false;
    return xQueueSend(queue_, &message, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

SystemState StateMachine::state() const {
    return state_;
}

void StateMachine::taskThunk(void* arg) {
    static_cast<StateMachine*>(arg)->taskLoop();
}

void StateMachine::taskLoop() {
    Message message;
    for (;;) {
        if (xQueueReceive(queue_, &message, portMAX_DELAY) == pdTRUE) {
            handle(message);
        }
    }
}

void StateMachine::handle(const Message& message) {
    switch (message.type) {
        case MessageType::BootComplete:
            // v0.3.7-rc: v0.3.6 confirmed the official portrait baseline,
            // Vink-owned canvas refresh, and raw touch path on real PaperS3.
            // Start in the normal reader home again while keeping diagnostics
            // available from Settings for future hardware checks.
            state_ = SystemState::Reader;
            renderState(state_);
            g_displayService.enqueueFull(true, 100);
            suppressAfterTransition(300);
            break;

        case MessageType::Tap:
            onActivity();
            {
                if (state_ == SystemState::Diagnostics) {
                g_uiRenderer.renderDiagnostics(message, "tap");
                g_displayService.enqueueFull(false, 100);
                break;
            }
            const UiAction action = g_uiRenderer.hitTest(state_, message.touch.x, message.touch.y);
            switch (action) {
                case UiAction::TabReader:
                case UiAction::TabLibrary:
                case UiAction::TabTransfer:
                case UiAction::TabSettings:
                    state_ = tabStateForAction(action);
                    renderState(state_);
                    g_displayService.enqueueFull(false, 100);
                    suppressAfterTransition();
                    break;

                case UiAction::OpenLibrary:
                    state_ = SystemState::Library;
                    g_readerBook.renderLibraryPage();
                    g_displayService.enqueueFull(false, 100);
                    suppressAfterTransition();
                    break;

                case UiAction::OpenTransfer:
                    state_ = SystemState::Transfer;
                    renderState(state_);
                    g_displayService.enqueueFull(false, 100);
                    break;

                case UiAction::OpenSettings:
                    state_ = SystemState::Settings;
                    renderState(state_);
                    g_displayService.enqueueFull(false, 100);
                    break;

                case UiAction::OpenSettingsLayout:
                    state_ = SystemState::SettingsLayout;
                    renderState(state_);
                    g_displayService.enqueueFull(false, 100);
                    suppressAfterTransition();
                    break;

                case UiAction::OpenSettingsRefresh:
                    state_ = SystemState::SettingsRefresh;
                    renderState(state_);
                    g_displayService.enqueueFull(false, 100);
                    suppressAfterTransition();
                    break;

                case UiAction::OpenSettingsWifi:
                    state_ = SystemState::SettingsWifi;
                    renderState(state_);
                    g_displayService.enqueueFull(false, 100);
                    suppressAfterTransition();
                    break;

                case UiAction::SaveWifiSettings:
                {
                    const auto& cfg = g_configService.get();
                    if (!cfg.wifiSsid.isEmpty()) {
                        g_wifiService.configureSta(cfg.wifiSsid, cfg.wifiPassword);
                    }
                    g_uiRenderer.renderSettingsWifi();
                    g_displayService.enqueueFull(false, 100);
                    break;
                }

                case UiAction::OpenSettingsLegado:
                    state_ = SystemState::SettingsLegado;
                    renderState(state_);
                    g_displayService.enqueueFull(false, 100);
                    suppressAfterTransition();
                    break;

                case UiAction::CycleLegadoEnabled:
                {
                    bool cur = g_configService.get().legadoEnabled;
                    g_configService.mut().legadoEnabled = !cur;
                    g_configService.save();
                    g_uiRenderer.renderSettingsLegado();
                    g_displayService.enqueueFull(false, 100);
                    break;
                }

                case UiAction::CycleLegadoSyncEnabled:
                {
                    bool cur = g_configService.get().legadoEnabled;
                    g_configService.mut().legadoEnabled = !cur;
                    g_configService.save();
                    // Reconfigure service if enabled
                    if (!cur) {
                        const auto& cfg = g_configService.get();
                        if (cfg.legadoEnabled && !cfg.legadoHost.isEmpty()) {
                            LegadoConfig lc;
                            lc.baseUrl = buildLegadoBaseUrl(cfg);
                            lc.token   = cfg.legadoToken;
                            lc.enabled = cfg.legadoEnabled;
                            g_legadoService.configure(lc);
                        }
                    } else {
                        g_legadoService.configure(LegadoConfig{});
                    }
                    g_uiRenderer.renderTransferLegadoStatus();
                    g_displayService.enqueueFull(false, 100);
                    break;
                }

                case UiAction::SaveLegadoSettings:
                {
                    // Configure Legado service with current saved config.
                    const auto& cfg = g_configService.get();
                    if (cfg.legadoEnabled && !cfg.legadoHost.isEmpty()) {
                        LegadoConfig lc;
                        lc.baseUrl = buildLegadoBaseUrl(cfg);
                        lc.token   = cfg.legadoToken;
                        lc.enabled = cfg.legadoEnabled;
                        g_legadoService.configure(lc);
                        g_uiRenderer.renderLegadoSync("配置已保存，正在测试连接...", -1, nullptr);
                    } else {
                        g_configService.mut().legadoEnabled = false;
                        g_configService.save();
                        g_uiRenderer.renderLegadoSync("Legado 已停用", -1, nullptr);
                    }
                    g_displayService.enqueueFull(false, 100);
                    break;
                }

                case UiAction::OpenSettingsSystem:
                    state_ = SystemState::SettingsSystem;
                    renderState(state_);
                    g_displayService.enqueueFull(false, 100);
                    suppressAfterTransition();
                    break;

                // ── Transfer sub-page actions ───────────────────────
                case UiAction::OpenTransferLegado:
                    state_ = SystemState::TransferLegadoStatus;
                    renderState(state_);
                    g_displayService.enqueueFull(false, 100);
                    suppressAfterTransition();
                    break;

                case UiAction::OpenTransferWifiAp:
                    state_ = SystemState::TransferWifiAp;
                    renderState(state_);
                    g_displayService.enqueueFull(false, 100);
                    suppressAfterTransition();
                    break;

                case UiAction::OpenTransferUsb:
                    state_ = SystemState::TransferUsb;
                    renderState(state_);
                    g_displayService.enqueueFull(false, 100);
                    suppressAfterTransition();
                    break;


                case UiAction::OpenTransferExport:
                    state_ = SystemState::TransferExport;
                    renderState(state_);
                    g_displayService.enqueueFull(false, 100);
                    suppressAfterTransition();
                    break;

                case UiAction::ToggleWifiAp:
                    if (g_wifiService.mode() == WifiOpMode::ApWebUi) {
                        g_wifiService.stop();
                        g_uiRenderer.renderTransferWifiAp();
                        g_displayService.enqueueFull(false, 100);
                    } else {
                        // Start AP + Web UI: SSID = Vink-PaperS3, no password
                        g_wifiService.startAp("Vink-PaperS3", String(), true);
                        g_uiRenderer.renderTransferWifiAp();
                        g_displayService.enqueueFull(false, 100);
                    }
                    break;

                case UiAction::ToggleWebUi:
                    if (g_wifiService.httpServerRunning()) {
                        g_wifiService.stopHttpServer();
                    } else {
                        g_wifiService.startHttpServer();
                    }
                    renderState(state_);
                    g_displayService.enqueueFull(false, 100);
                    break;

                case UiAction::CycleWifiMode:
                    if (state_ == SystemState::TransferWifiAp) {
                        // Legacy fallback: Off → ApWebUi → Ap → Off.
                        WifiOpMode cur = g_wifiService.mode();
                        if (cur == WifiOpMode::Off) {
                            g_wifiService.startAp("Vink-PaperS3", String(), true);
                        } else if (cur == WifiOpMode::ApWebUi) {
                            g_wifiService.stop();
                            g_wifiService.startAp("Vink-PaperS3", String(), false);
                        } else {
                            g_wifiService.stop();
                        }
                        g_uiRenderer.renderTransferWifiAp();
                        g_displayService.enqueueFull(false, 100);
                    }
                    break;

                case UiAction::SetWifiOff:
                    if (state_ == SystemState::TransferWifiAp) {
                        g_wifiService.stop();
                        g_uiRenderer.renderTransferWifiAp();
                        g_displayService.enqueueFull(false, 100);
                    }
                    break;

                case UiAction::SetWifiApWebUi:
                    if (state_ == SystemState::TransferWifiAp) {
                        g_wifiService.startAp("Vink-PaperS3", String(), true);
                        g_uiRenderer.renderTransferWifiAp();
                        g_displayService.enqueueFull(false, 100);
                    }
                    break;

                case UiAction::SetWifiSta:
                    if (state_ == SystemState::TransferWifiAp) {
                        const auto& cfg = g_configService.get();
                        if (!cfg.wifiSsid.isEmpty()) {
                            g_wifiService.stop();
                            g_wifiService.configureSta(cfg.wifiSsid, cfg.wifiPassword);
                            g_wifiService.connectSta();
                        } else {
                            g_wifiService.stop();
                        }
                        g_uiRenderer.renderTransferWifiAp();
                        g_displayService.enqueueFull(false, 100);
                    }
                    break;

                // Cycling actions — modify config and re-render
                case UiAction::CycleRefreshFrequency:
                {
                    auto f = g_configService.refreshFrequency();
                    g_configService.setRefreshFrequency(RefreshFrequency(
                        (static_cast<uint8_t>(f) + 1) % 3));
                    g_configService.save();
                    renderState(state_);
                    g_displayService.enqueueFull(false, 100);
                    break;
                }

                case UiAction::CycleFontSize:
                {
                    uint8_t sizes[] = { 18, 24, 30, 36 };
                    uint8_t cur = g_configService.get().fontSize;
                    uint8_t next = sizes[0];
                    for (size_t i = 0; i < sizeof(sizes); i++) {
                        if (cur < sizes[i]) { next = sizes[i]; break; }
                    }
                    g_configService.setFontSize(next);
                    g_configService.save();
                    g_readerBook.onLayoutChanged();
                    g_readerBook.rebuildCurrentChapterAsync();
                    break;
                }

                case UiAction::CycleFontFamily:
                {
                    char paths[32][128];
                    char names[32][64];
                    int count = FontManager::scanFonts(paths, names, 32);
                    if (count <= 0) break;
                    uint8_t curIdx = g_configService.get().fontIndex;
                    uint8_t nextIdx = (curIdx + 1) % count;
                    g_configService.setFontIndex(nextIdx);
                    g_configService.save();
                    if (paths[nextIdx][0]) {
                        g_readerText.loadFont(paths[nextIdx]);
                    }
                    g_readerBook.onLayoutChanged();
                    g_readerBook.rebuildCurrentChapterAsync();
                    break;
                }

                case UiAction::CycleLineSpacing:
                {
                    uint8_t spacings[] = { 50, 60, 70, 80 };
                    uint8_t cur = g_configService.get().lineSpacing;
                    uint8_t next = spacings[0];
                    for (size_t i = 0; i < sizeof(spacings); i++) {
                        if (cur < spacings[i]) { next = spacings[i]; break; }
                    }
                    g_configService.setLineSpacing(next);
                    g_configService.save();
                    g_readerBook.onLayoutChanged();
                    g_readerBook.rebuildCurrentChapterAsync();
                    break;
                }

                case UiAction::CycleJustify:
                {
                    g_configService.setJustify(!g_configService.get().justify);
                    g_configService.save();
                    g_readerBook.onLayoutChanged();
                    g_readerBook.rebuildCurrentChapterAsync();
                    break;
                }

                case UiAction::CycleSimplified:
                {
                    g_configService.setSimplifiedChinese(!g_configService.get().simplifiedChinese);
                    g_configService.save();
                    g_readerBook.onLayoutChanged();
                    g_readerBook.rebuildCurrentChapterAsync();
                    break;
                }

                case UiAction::CycleMarginLeft:
                {
                    static const uint8_t kMarginValues[] = { 16, 24, 34, 48, 64 };
                    uint8_t cur = g_configService.get().marginLeft;
                    uint8_t next = kMarginValues[0];
                    for (size_t i = 0; i < sizeof(kMarginValues); i++) {
                        if (cur < kMarginValues[i]) { next = kMarginValues[i]; break; }
                    }
                    g_configService.setMargins(next, next,
                        g_configService.get().marginTop,
                        g_configService.get().marginBottom);
                    g_configService.save();
                    g_readerBook.onLayoutChanged();
                    g_readerBook.rebuildCurrentChapterAsync();
                    break;
                }

                case UiAction::OpenDiagnostics:
                    state_ = SystemState::Diagnostics;
                    g_uiRenderer.renderDiagnostics(message, "进入诊断");
                    g_displayService.enqueueFull(true, 100);
                    suppressAfterTransition();
                    break;

                case UiAction::RequestShutdown:
                    state_ = SystemState::Shutdown;
                    shutdownPaperS3("正在关机");
                    break;

                case UiAction::StartLegadoSync:
                {
                    // Keep the ReadPaper-like event path: UI hit-test creates an action,
                    // state posts a service-level sync message, service reports result.
                    state_ = SystemState::LegadoSync;
                    g_uiRenderer.renderLegadoSync("正在同步 Legado 书架...", -1, nullptr);
                    g_displayService.enqueueFull(false, 100);
                    Message start;
                    start.type = MessageType::LegadoSyncStart;
                    start.timestampMs = millis();
                    post(start, 20);
                    break;
                }

                case UiAction::OpenCurrentBook:
                {
                    const bool fromLibrary = state_ == SystemState::Library;
                    if (fromLibrary) {
                        if (!g_readerBook.handleLibraryTap(message.touch.x, message.touch.y)) break;
                        state_ = SystemState::ReaderMenu;
                        g_readerBook.renderCurrent();
                    } else {
                        state_ = SystemState::ReaderMenu;
                        g_readerBook.renderOpenOrHelp();
                    }
                    g_displayService.enqueueFull(false, 100);
                    suppressAfterTransition();
                    break;
                }

                case UiAction::None:
                    if (state_ == SystemState::ReaderMenu && g_readerBook.handleTap(message.touch.x, message.touch.y)) {
                        g_displayService.enqueueFull(false, 100);
                    } else if (state_ == SystemState::Library && g_readerBook.handleLibraryTap(message.touch.x, message.touch.y)) {
                        state_ = SystemState::ReaderMenu;
                        g_readerBook.renderCurrent();
                        g_displayService.enqueueFull(false, 100);
                    }
                    break;

                case UiAction::BackHome:
                default:
                    break;
            }
            break;
        }

        case MessageType::SwipeLeft:
            onActivity();
            if (state_ == SystemState::Diagnostics) {
                g_uiRenderer.renderDiagnostics(message, "swipe-left");
                g_displayService.enqueueFull(false, 100);
                break;
            }
            if (state_ == SystemState::ReaderMenu) {
                if (g_readerBook.nextPage()) g_displayService.enqueueFull(false, 100);
                break;
            }
            if (state_ == SystemState::Library) {
                if (g_readerBook.nextLibraryPage()) g_displayService.enqueueFull(false, 100);
                else { state_ = SystemState::Transfer; renderState(state_); g_displayService.enqueueFull(false, 100); }
                break;
            }
            if (state_ == SystemState::Reader) {
                // Swipe left → previous tab
                state_ = SystemState::Library;
                renderState(state_);
                g_displayService.enqueueFull(false, 100);
                break;
            }
            else if (state_ == SystemState::Transfer) state_ = SystemState::Settings;
            else if (state_ == SystemState::SettingsLayout ||
                     state_ == SystemState::SettingsRefresh ||
                     state_ == SystemState::SettingsWifi ||
                     state_ == SystemState::SettingsLegado ||
                     state_ == SystemState::SettingsSystem ||
                     state_ == SystemState::TransferLegadoStatus ||
                     state_ == SystemState::TransferWifiAp ||
                     state_ == SystemState::TransferUsb ||
                     state_ == SystemState::TransferExport)
                state_ = SystemState::Settings;
            renderState(state_);
            g_displayService.enqueueFull(false, 100);
            break;

        case MessageType::SwipeRight:
            onActivity();
            if (state_ == SystemState::Diagnostics) {
                g_uiRenderer.renderDiagnostics(message, "swipe-right");
                g_displayService.enqueueFull(false, 100);
                break;
            }
            if (state_ == SystemState::ReaderMenu) {
                if (g_readerBook.prevPage()) g_displayService.enqueueFull(false, 100);
                break;
            }
            if (state_ == SystemState::Library) {
                if (g_readerBook.prevLibraryPage()) g_displayService.enqueueFull(false, 100);
                else { state_ = SystemState::Reader; renderState(state_); g_displayService.enqueueFull(false, 100); }
                break;
            }
            if (state_ == SystemState::Reader) {
                // Swipe right → next tab
                state_ = SystemState::Transfer;
                renderState(state_);
                g_displayService.enqueueFull(false, 100);
                break;
            }
            else if (state_ == SystemState::Transfer) state_ = SystemState::Library;
            renderState(state_);
            g_displayService.enqueueFull(false, 100);
            break;

        case MessageType::SwipeUp:
            onActivity();
            if (state_ == SystemState::Diagnostics) {
                g_uiRenderer.renderDiagnostics(message, "swipe-up");
                g_displayService.enqueueFull(false, 100);
                break;
            }
            if (state_ == SystemState::ReaderMenu && g_readerBook.nextPage()) {
                g_displayService.enqueueFull(false, 100);
            } else if (state_ == SystemState::Library && g_readerBook.nextLibraryPage()) {
                g_displayService.enqueueFull(false, 100);
            }
            break;

        case MessageType::SwipeDown:
            onActivity();
            if (state_ == SystemState::Diagnostics) {
                g_uiRenderer.renderDiagnostics(message, "swipe-down");
                g_displayService.enqueueFull(false, 100);
                break;
            }
            if (state_ == SystemState::ReaderMenu && g_readerBook.prevPage()) {
                g_displayService.enqueueFull(false, 100);
            } else if (state_ == SystemState::Library && g_readerBook.prevLibraryPage()) {
                g_displayService.enqueueFull(false, 100);
            }
            break;

        case MessageType::LegadoSyncStart:
        {
            state_ = SystemState::LegadoSync;
            g_uiRenderer.renderLegadoSync("正在同步...");
            g_displayService.enqueueFull(false, 100);

            // Sync Legado service config with current saved settings.
            const auto& cfg = g_configService.get();
            if (cfg.legadoEnabled && !cfg.legadoHost.isEmpty()) {
                LegadoConfig lc;
                lc.baseUrl  = buildLegadoBaseUrl(cfg);
                lc.token    = cfg.legadoToken;
                lc.enabled  = cfg.legadoEnabled;
                g_legadoService.configure(lc);

                // Attempt to fetch bookshelf count as connectivity test without
                // returning ArduinoJson views backed by temporary documents.
                int count = 0;
                if (!g_legadoService.getBookshelfCount(count)) {
                    String err = g_legadoService.lastError();
                    Message failed;
                    failed.type = MessageType::LegadoSyncFailed;
                    failed.timestampMs = millis();
                    post(failed, 0);
                } else {
                    Message done;
                    done.type = MessageType::LegadoSyncDone;
                    done.timestampMs = millis();
                    done.scratch = static_cast<uint32_t>(count);  // pass book count
                    post(done, 0);
                }
            } else {
                g_uiRenderer.renderLegadoSync("Legado 未配置，请在设置中填写服务器地址并启用", -1, "未配置");
                g_displayService.enqueueFull(false, 100);
                Message failed;
                failed.type = MessageType::LegadoSyncFailed;
                failed.timestampMs = millis();
                post(failed, 0);
            }
            break;
        }

        case MessageType::LegadoSyncDone:
            state_ = SystemState::LegadoSync;
            g_uiRenderer.renderLegadoSync("同步完成",
                                          static_cast<int>(message.scratch), nullptr);
            g_displayService.enqueueFull(false, 100);
            break;

        case MessageType::LegadoSyncFailed:
            state_ = SystemState::LegadoSync;
            g_uiRenderer.renderLegadoSync("同步失败", -1,
                                          g_legadoService.lastError().c_str());
            g_displayService.enqueueFull(true, 100);
            break;

        case MessageType::LongPress:
            onActivity();
            if (state_ == SystemState::Diagnostics) {
                g_uiRenderer.renderDiagnostics(message, "long-press");
                g_displayService.enqueueFull(false, 100);
                break;
            }
            if (state_ == SystemState::ReaderMenu) {
                if (g_readerBook.handleLongPress(message.touch.x, message.touch.y)) {
                    g_displayService.enqueueFull(false, 100);
                }
                break;
            }
            break;

        case MessageType::PowerButton:
            state_ = SystemState::Shutdown;
            shutdownPaperS3("正在关机");
            break;

        case MessageType::SleepTimeout:
            if (state_ == SystemState::Boot || state_ == SystemState::Shutdown) break;
            enterSleep("自动休眠");
            break;

        case MessageType::TouchDown:
            onActivity();
            if (state_ == SystemState::Diagnostics) {
                g_uiRenderer.renderDiagnostics(message, "down");
                g_displayService.enqueueFull(false, 100);
            }
            break;

        case MessageType::TouchMove:
            if (state_ == SystemState::Diagnostics) {
                g_uiRenderer.renderDiagnostics(message, "move");
                g_displayService.enqueueFull(false, 100);
            }
            break;

        case MessageType::TouchUp:
            if (state_ == SystemState::Diagnostics) {
                g_uiRenderer.renderDiagnostics(message, "up");
                g_displayService.enqueueFull(false, 100);
            }
            break;

        case MessageType::DisplayDone:
        case MessageType::None:
        default:
            break;
    }
}

} // namespace vink3
