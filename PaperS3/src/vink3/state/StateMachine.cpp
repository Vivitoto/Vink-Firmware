#include "StateMachine.h"
#include "../config/ConfigService.h"
#include "../display/DisplayService.h"
#include "../sync/WifiService.h"
#include "../reader/ReaderBookService.h"
#include "../reader/ReaderTextRenderer.h"
#include "../ui/VinkUiRenderer.h"
#include "../input/InputService.h"
#include "../../FontManager.h"
#include "../VinkPaperS3Core.h"
#include <esp_sleep.h>
#include <SD.h>

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

void enqueueReaderAwareRefresh(DisplayEffect effect = DisplayEffect::None, uint32_t timeoutMs = 100) {
    if (g_readerBook.consumeReadingPageRendered()) {
        g_displayService.enqueueReaderPageTurn(effect, timeoutMs);
    } else {
        g_displayService.enqueueFull(false, timeoutMs);
    }
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
        case SystemState::SettingsSystem:
            g_uiRenderer.renderSettingsSystem();
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
        case SystemState::Locked:
            // Handled by enterLockScreen() on entry; no further render needed
            // until wakeFromLockScreen() is called.
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

// Double-tap lock detection (file-level static to survive task loop iterations)
static TouchPoint lastLockTap_;
static uint32_t  lastLockTapMs_ = 0;
static bool      waitingSecondLockTap_ = false;

void StateMachine::enterLockScreen() {
    if (locked_) return;
    const auto& cfg = g_configService.get();
    if (!cfg.lockScreenEnabled) return;

    locked_ = true;
    state_ = SystemState::Locked;
    g_uiRenderer.renderLockScreen(cfg.lockScreenImagePath);
    g_displayService.enqueueFull(true, 100);
    g_displayService.waitIdle(5000);
    Serial.println("[vink3][lock] screen locked");
}

void StateMachine::wakeFromLockScreen() {
    if (!locked_) return;
    locked_ = false;
    lastLockTapMs_ = 0;
    waitingSecondLockTap_ = false;
    g_displayService.enqueueFull(true, 100);
    g_displayService.waitIdle(2000);
    g_readerBook.renderCurrentPage();
    g_displayService.enqueueFull(true, 100);
    g_readerBook.consumeReadingPageRendered();
    Serial.println("[vink3][lock] screen awakened");
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
                // Locked state: all taps go to double-tap detector only.
                if (state_ == SystemState::Locked) {
                    const int16_t zoneX = (kPaperS3Width * 2) / 3;  // right 1/3
                    const int16_t zoneY = (kPaperS3Height * 2) / 3; // bottom 1/3
                    if (message.touch.x >= zoneX && message.touch.y >= zoneY) {
                        uint32_t now = message.timestampMs;
                        if (waitingSecondLockTap_ && (now - lastLockTapMs_ <= 500)) {
                            waitingSecondLockTap_ = false;
                            lastLockTapMs_ = 0;
                            if (g_configService.get().lockScreenWakeOnDoubleClick) {
                                wakeFromLockScreen();
                            }
                        } else {
                            waitingSecondLockTap_ = true;
                            lastLockTapMs_ = now;
                        }
                    }
                    break;
                }

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
                case UiAction::OpenSettingsSystem:
                    state_ = SystemState::SettingsSystem;
                    renderState(state_);
                    g_displayService.enqueueFull(false, 100);
                    suppressAfterTransition();
                    break;

                // ── Transfer sub-page actions ───────────────────────
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

                case UiAction::CycleReaderMiddleRefresh:
                {
                    static const uint8_t values[] = { 5, 10, 15, 20, 30 };
                    const auto& cfg = g_configService.get();
                    uint8_t next = values[0];
                    for (size_t i = 0; i < sizeof(values); ++i) {
                        if (cfg.readerMiddleRefreshEvery < values[i]) { next = values[i]; break; }
                    }
                    g_configService.setReaderRefreshThresholds(next, cfg.readerFullRefreshEvery);
                    g_configService.save();
                    renderState(state_);
                    g_displayService.enqueueFull(false, 100);
                    break;
                }

                case UiAction::CycleReaderFullRefresh:
                {
                    static const uint8_t values[] = { 0, 10, 20, 30, 40, 60 };
                    const auto& cfg = g_configService.get();
                    uint8_t next = values[0];
                    for (size_t i = 0; i < sizeof(values); ++i) {
                        if (cfg.readerFullRefreshEvery < values[i] &&
                            (values[i] == 0 || values[i] > cfg.readerMiddleRefreshEvery)) {
                            next = values[i];
                            break;
                        }
                    }
                    if (next != 0 && next <= cfg.readerMiddleRefreshEvery) {
                        for (size_t i = 0; i < sizeof(values); ++i) {
                            if (values[i] > cfg.readerMiddleRefreshEvery) { next = values[i]; break; }
                        }
                    }
                    g_configService.setReaderRefreshThresholds(cfg.readerMiddleRefreshEvery, next);
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
                        // Tap-triggered page turn: get direction from handleTap
                        enqueueReaderAwareRefresh(
                            g_readerBook.consumeLastTapNextPage()
                                ? DisplayEffect::HorizontalShutter
                                : DisplayEffect::VerticalShutter);
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
                if (g_readerBook.nextPage()) enqueueReaderAwareRefresh(DisplayEffect::HorizontalShutter);  // forward
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
                     state_ == SystemState::SettingsSystem ||
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
                if (g_readerBook.prevPage()) enqueueReaderAwareRefresh(DisplayEffect::VerticalShutter);  // backward
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
                enqueueReaderAwareRefresh(DisplayEffect::HorizontalShutter);  // forward
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
                enqueueReaderAwareRefresh(DisplayEffect::VerticalShutter);  // backward
            } else if (state_ == SystemState::Library && g_readerBook.prevLibraryPage()) {
                g_displayService.enqueueFull(false, 100);
            }
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
            if (locked_) {
                // When locked, all taps are consumed by the double-tap detector.
                // LongPress carries no additional information here — just break.
                break;
            }
            break;

        case MessageType::PowerButton:
            if (locked_) {
                wakeFromLockScreen();
            } else {
                state_ = SystemState::Shutdown;
                shutdownPaperS3("正在关机");
            }
            break;

        case MessageType::LockScreen:
            enterLockScreen();
            break;

        case MessageType::WakeFromLockScreen:
            wakeFromLockScreen();
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
