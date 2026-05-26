#include "StateMachine.h"
#include "../display/DisplayService.h"
#include "../reader/ReaderBookService.h"
#include "../reader/ReaderTextRenderer.h"
#include "../sync/LegadoService.h"
#include "../sync/WifiService.h"
#include "../system/SystemLog.h"
#include "../ui/VinkUiRenderer.h"
#include "../input/InputService.h"
#include "../VinkPaperS3.h"
#include <esp_sleep.h>
#include <driver/gpio.h>

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

void shutdownPaperS3(const char* reason) {
    Serial.println("[vink3][power] shutdown requested");
    g_systemLog.appendf("shutdown requested: %s", reason ? reason : "power");
    g_readerBook.saveCurrentProgress();
    g_uiRenderer.renderShutdown(reason ? reason : "正在关机");
    g_displayService.enqueueFull(true, 100);
    g_displayService.waitIdle(5000);
    delay(300);

    // ReadPaper-style shutdown: save state, give SD/display time to settle,
    // then cut PaperS3 power with the board-specific GPIO44 PWROFF pulse.
    delay(500);
    M5.Display.waitDisplay();

    // E-ink keeps its last image after power is cut. Draw a final, explicit
    // "powered off" page before pulsing GPIO44 so real-device testing can
    // distinguish a completed shutdown path from a stuck busy page.
    g_uiRenderer.renderPowerOffReady();
    g_displayService.enqueueFull(true, 100);
    g_displayService.waitIdle(8000);
    M5.Display.waitDisplay();
    delay(1500);
    clearPaperS3RuntimeRunning();
    Serial.println("[vink3][power] final power-off page drawn; pulsing PaperS3 GPIO44 PWROFF");
    g_systemLog.append("power-off page drawn; GPIO44 pulse");
    Serial.flush();
    pulsePaperS3PowerOffPin();

    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_OFF);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_SLOW_MEM, ESP_PD_OPTION_OFF);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_FAST_MEM, ESP_PD_OPTION_OFF);
    Serial.println("[vink3][power] GPIO44 PWROFF pulse returned; entering fallback ESP32-S3 deep sleep");
    g_systemLog.append("GPIO44 pulse returned; deep sleep fallback");
    Serial.flush();
    delay(100);
    esp_deep_sleep_start();
    Serial.println("[vink3][power] WARNING: deep_sleep_start returned!");
    for (;;) delay(1000);
}

void suppressAfterTransition(uint32_t cooldownMs = 20) {
    // Minimal stale-event guard after rendering a new page. 20 ms is enough
    // to absorb the release event from the triggering tap without blocking
    // rapid consecutive reading taps.
    g_inputService.suppressFor(cooldownMs);
}

void enqueueReaderDisplay(bool pageTurnCandidate = false, DisplayEffect effect = DisplayEffect::HorizontalShutter) {
    if (pageTurnCandidate && g_readerText.pageTurnEffectEnabled()) {
        g_displayService.enqueueReaderPageTurn(effect, 100);
        return;
    }
    g_displayService.enqueueFull(false, 100);
}

bool shouldQualityRefreshTabSwitch() {
    // TAB switching should feel responsive. Most switches use the GL16/text
    // waveform (enqueueFull(false)); every few switches, use quality refresh to
    // clean accumulated ghosting without making every tab tap feel like a full
    // black/white flash.
    static uint8_t s_tabSwitchesSinceQuality = 0;
    constexpr uint8_t kQualityEveryTabSwitches = 8;
    s_tabSwitchesSinceQuality++;
    if (s_tabSwitchesSinceQuality >= kQualityEveryTabSwitches) {
        s_tabSwitchesSinceQuality = 0;
        return true;
    }
    return false;
}

constexpr uint32_t kDoubleTapWindowMs = 650;
constexpr int16_t kDoubleTapSlopPx = 54;
TouchPoint s_lastSemanticTap{};
uint32_t s_lastSemanticTapMs = 0;
bool s_lastSemanticTapInLockZone = false;
bool s_lastSemanticTapInUnlockZone = false;

// Auto-off: shutdown after N minutes of no touch/input activity.
// 0 = disabled. Runtime timer resets on every Tap or Swipe message.
static uint32_t s_lastInteractionMs = 0;
static uint16_t s_autoOffMinutes = 0;

bool isReaderLockZone(const TouchPoint& p) {
    // Top-centre header zone — roughly the status-bar / chapter-title strip.
    // Always active for double-tap to lock.
    return p.x >= 180 && p.x <= 360 && p.y >= 0 && p.y <= 64;
}

bool isLockScreenUnlockZone(const TouchPoint& p) {
    // Same top-centre zone, used on the lock screen for double-tap unlock.
    return p.x >= 180 && p.x <= 360 && p.y >= 0 && p.y <= 64;
}

bool consumeDoubleTapInZone(const Message& message, bool zoneNow, bool& previousZone) {
    const uint32_t dt = message.timestampMs - s_lastSemanticTapMs;
    const int dx = abs(message.touch.x - s_lastSemanticTap.x);
    const int dy = abs(message.touch.y - s_lastSemanticTap.y);
    const bool matched = zoneNow && previousZone && dt <= kDoubleTapWindowMs &&
                         max(dx, dy) <= kDoubleTapSlopPx;
    s_lastSemanticTap = message.touch;
    s_lastSemanticTapMs = message.timestampMs;
    s_lastSemanticTapInLockZone = isReaderLockZone(message.touch);
    s_lastSemanticTapInUnlockZone = isLockScreenUnlockZone(message.touch);
    if (matched) {
        previousZone = false;
        s_lastSemanticTapMs = 0;
    }
    return matched;
}

void renderSoftwareLockScreen() {
    g_readerBook.saveCurrentProgress();
    markPaperS3SoftwareLocked();
    g_uiRenderer.renderLockScreen(g_readerBook.isOpen() ? g_readerBook.title() : nullptr);
    g_displayService.enqueueFull(true, 100);
    g_systemLog.append("software lock entered");
}

bool resumeFromLockScreen() {
    // Shared lock-screen exit path. Double-tap unlock and side-key unlock must
    // both return to the reading body that was visible before locking. Normal
    // boot and unlocked side-key shutdown are handled elsewhere and must not be
    // routed through this function.
    clearPaperS3SoftwareLocked();
    if (!g_readerBook.isOpen()) g_readerBook.openLastBook();
    if (g_readerBook.isOpen()) {
        g_readerBook.skipBookEntryAndResume();
        g_systemLog.append("software lock resumed reader");
        return true;
    }
    g_readerBook.renderReaderHome();
    g_systemLog.append("software lock resume: no last book");
    return false;
}

void enqueueReaderAwareRefresh(DisplayEffect effect = DisplayEffect::HorizontalShutter) {
    if (g_readerBook.consumeReadingPageRendered()) {
        // Page-turn direction contract:
        // - next page  -> DisplayEffect::VerticalShutter
        // - prev page  -> DisplayEffect::HorizontalShutter
        // Keep this centralized so tap/swipe handlers cannot accidentally flip
        // the visual refresh direction when the native EPD path uses it.
        enqueueReaderDisplay(true, effect);
    } else {
        g_displayService.enqueueFull(false, 100);
    }
}

void enqueueLockResumeRefresh(bool resumedReader) {
    if (resumedReader) {
        // Lock-screen exit is a state restore, not a page turn. Even when the
        // reader body has just been rendered, consume the reader-page marker so
        // the page-turn animation path remains reserved for explicit next/prev
        // page actions only.
        (void)g_readerBook.consumeReadingPageRendered();
        g_displayService.enqueueFull(false, 100);
    } else {
        g_displayService.enqueueFull(true, 100);
    }
}

void renderState(SystemState state) {
    switch (state) {
        case SystemState::Home:
        case SystemState::Reader:
            g_readerBook.renderReaderHome();
            break;
        case SystemState::ReaderMenu:
            g_readerBook.renderCurrent();
            break;
        case SystemState::Library:
            g_readerBook.renderShelfGrid();
            break;
        case SystemState::Transfer:
            g_uiRenderer.renderTransfer();
            break;
        case SystemState::Settings:
            g_uiRenderer.renderSettings();
            break;
        case SystemState::Diagnostics:
        {
            Message blank;
            blank.timestampMs = millis();
            g_uiRenderer.renderDiagnostics(blank, "等待触摸");
            break;
        }
        case SystemState::SystemLogs:
            g_uiRenderer.renderSystemLogs();
            break;
        case SystemState::ShutdownConfirm:
            g_uiRenderer.renderShutdownConfirm();
            break;
        case SystemState::Locked:
            g_uiRenderer.renderLockScreen(g_readerBook.isOpen() ? g_readerBook.title() : nullptr);
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
        BaseType_t ok = xTaskCreatePinnedToCore(taskThunk, "vink3-state", 16384, this, 3, &task_, 1);
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
        // Block with a 10‑second timeout so we can periodically check the
        // auto‑off idle timer.
        if (xQueueReceive(queue_, &message, pdMS_TO_TICKS(10000)) == pdTRUE) {
            handle(message);
        }
        // Auto‑off check (runs after every message or every 10 s idle).
        if (s_autoOffMinutes > 0 && s_lastInteractionMs > 0
            && (millis() - s_lastInteractionMs) >= s_autoOffMinutes * 60000UL) {
            Serial.printf("[vink3][power] auto-off after %u min idle\n", s_autoOffMinutes);
            g_systemLog.appendf("auto-off: %u min idle", s_autoOffMinutes);
            shutdownPaperS3("auto-off");
        }
    }
}

void StateMachine::handle(const Message& message) {
    switch (message.type) {
        case MessageType::BootComplete:
            // v0.3.7-rc: v0.3.6 confirmed the official portrait baseline,
            // Vink-owned canvas refresh, and raw touch path on real PaperS3.
            // If the previous boot was a software lock and the side key caused
            // the reset, resume the last reader page instead of treating the
            // reset as a shutdown request.
            if (consumePaperS3SideKeyUnlockRequested()) {
                Serial.println("[vink3][boot] BootComplete: side-key unlock resume");
                const bool resumedReader = resumeFromLockScreen();
                state_ = resumedReader ? SystemState::ReaderMenu : SystemState::Reader;
                enqueueLockResumeRefresh(resumedReader);
                suppressAfterTransition(200);
                break;
            }
            state_ = SystemState::Reader;
            renderState(state_);
            g_displayService.enqueueFull(true, 100);
            suppressAfterTransition(150);
            // Load auto‑off timer from persisted settings.
            s_autoOffMinutes = ReaderTextRenderer::autoOffValueFromIndex(g_readerText.autoOffMinutesIndex());
            s_lastInteractionMs = millis();
            break;

        case MessageType::Tap:
        {
            s_lastInteractionMs = millis();

            if (state_ == SystemState::Locked) {
                if (!g_readerText.doubleTapUnlockEnabled()) break;
                const bool unlockZone = isLockScreenUnlockZone(message.touch);
                if (consumeDoubleTapInZone(message, unlockZone, s_lastSemanticTapInUnlockZone)) {
                    const bool resumedReader = resumeFromLockScreen();
                    state_ = resumedReader ? SystemState::ReaderMenu : SystemState::Reader;
                    enqueueLockResumeRefresh(resumedReader);
                    suppressAfterTransition(200);
                }
                break;
            }

            if (state_ == SystemState::ReaderMenu && g_readerBook.isReadingBody()) {
                // Double-tap to lock is always enabled.
                const bool lockZone = isReaderLockZone(message.touch);
                if (consumeDoubleTapInZone(message, lockZone, s_lastSemanticTapInLockZone)) {
                    renderSoftwareLockScreen();
                    state_ = SystemState::Locked;
                    suppressAfterTransition(200);
                    break;
                }
                if (lockZone) break; // first tap of the lock gesture is consumed
            }

            if (state_ == SystemState::Diagnostics) {
                const UiAction diagAction = g_uiRenderer.hitTest(state_, message.touch.x, message.touch.y);
                if (diagAction >= UiAction::TabReader && diagAction <= UiAction::TabSettings) {
                    g_uiRenderer.hideReaderSettings();
                    state_ = tabStateForAction(diagAction);
                    renderState(state_);
                    g_displayService.enqueueFull(shouldQualityRefreshTabSwitch(), 100);
                    suppressAfterTransition();
                } else {
                    g_uiRenderer.renderDiagnostics(message, "tap");
                }
                g_displayService.enqueueFull(false, 100);
                suppressAfterTransition();
                break;
            }
            if (state_ == SystemState::SystemLogs) {
                const UiAction logAction = g_uiRenderer.hitTest(state_, message.touch.x, message.touch.y);
                if (logAction >= UiAction::TabReader && logAction <= UiAction::TabSettings) {
                    g_uiRenderer.hideReaderSettings();
                    state_ = tabStateForAction(logAction);
                    renderState(state_);
                    g_displayService.enqueueFull(shouldQualityRefreshTabSwitch(), 100);
                    suppressAfterTransition();
                } else if (logAction == UiAction::ClearSystemLogs) {
                    g_systemLog.clear();
                    g_systemLog.append("system log cleared");
                    g_uiRenderer.resetSystemLogPage();
                    state_ = SystemState::SystemLogs;
                    renderState(state_);
                } else if (logAction == UiAction::BackToSettings || logAction == UiAction::TabSettings) {
                    state_ = SystemState::Settings;
                    renderState(state_);
                }
                g_displayService.enqueueFull(false, 100);
                suppressAfterTransition();
                break;
            }
            const UiAction action = g_uiRenderer.hitTest(state_, message.touch.x, message.touch.y);
            switch (action) {
                case UiAction::TabReader:
                case UiAction::TabLibrary:
                case UiAction::TabTransfer:
                case UiAction::TabSettings:
                    g_uiRenderer.hideReaderSettings();
                    state_ = tabStateForAction(action);
                    renderState(state_);
                    // Tab pages are high-contrast full-screen transitions. Use
                    // fast GL16/text refresh by default, then periodic quality
                    // refresh to balance responsiveness and ghost cleanup.
                    g_displayService.enqueueFull(shouldQualityRefreshTabSwitch(), 100);
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
                    g_uiRenderer.hideReaderSettings();
                    state_ = SystemState::Settings;
                    renderState(state_);
                    g_displayService.enqueueFull(false, 100);
                    break;

                case UiAction::OpenDiagnostics:
                    state_ = SystemState::Diagnostics;
                    g_uiRenderer.renderDiagnostics(message, "进入诊断");
                    g_displayService.enqueueFull(true, 100);
                    suppressAfterTransition();
                    break;

                case UiAction::OpenSystemLogs:
                    state_ = SystemState::SystemLogs;
                    g_uiRenderer.resetSystemLogPage();
                    g_uiRenderer.renderSystemLogs();
                    g_displayService.enqueueFull(true, 100);
                    suppressAfterTransition();
                    break;

                case UiAction::ClearSystemLogs:
                    g_systemLog.clear();
                    g_systemLog.append("system log cleared");
                    g_uiRenderer.resetSystemLogPage();
                    state_ = SystemState::SystemLogs;
                    g_uiRenderer.renderSystemLogs();
                    g_displayService.enqueueFull(false, 100);
                    suppressAfterTransition();
                    break;

                case UiAction::RequestShutdown:
                    state_ = SystemState::ShutdownConfirm;
                    g_uiRenderer.renderShutdownConfirm();
                    g_displayService.enqueueFull(true, 100);
                    suppressAfterTransition();
                    break;

                case UiAction::ConfirmShutdown:
                    state_ = SystemState::Shutdown;
                    shutdownPaperS3("正在关机");
                    break;

                case UiAction::CancelShutdown:
                    g_uiRenderer.hideReaderSettings();
                    state_ = SystemState::Settings;
                    renderState(state_);
                    g_displayService.enqueueFull(false, 100);
                    suppressAfterTransition();
                    break;

                case UiAction::CycleReaderRefreshStrategy:
                    g_displayService.cycleReaderRefreshStrategy();
                    state_ = SystemState::Settings;
                    renderState(state_);
                    g_displayService.enqueueFull(false, 100);
                    suppressAfterTransition();
                    break;

                case UiAction::ToggleReaderAntiAlias:
                    g_readerText.toggleAntiAlias();
                    state_ = SystemState::Settings;
                    renderState(state_);
                    g_displayService.enqueueFull(false, 100);
                    suppressAfterTransition();
                    break;

                case UiAction::ToggleReaderUnderline:
                    g_readerText.toggleUnderline();
                    g_readerBook.invalidatePaginationForLayoutChange();
                    state_ = SystemState::Settings;
                    renderState(state_);
                    g_displayService.enqueueFull(false, 100);
                    suppressAfterTransition();
                    break;

                case UiAction::CycleReaderAntialiasProfile:
                    g_readerText.cycleAntialiasProfile();
                    state_ = SystemState::Settings;
                    renderState(state_);
                    g_displayService.enqueueFull(false, 100);
                    suppressAfterTransition();
                    break;

                case UiAction::CycleReaderFontSize:
                    if (!g_readerText.isSdFont()) {
                        g_readerText.cycleReaderFontSize();
                        g_readerBook.invalidatePaginationForLayoutChange();
                    }
                    state_ = SystemState::Settings;
                    renderState(state_);
                    g_displayService.enqueueFull(false, 100);
                    suppressAfterTransition();
                    break;

                case UiAction::CycleReaderFontSource:
                    g_readerText.cycleFontSource();
                    g_readerBook.invalidatePaginationForLayoutChange();
                    state_ = SystemState::Settings;
                    renderState(state_);
                    g_displayService.enqueueFull(false, 100);
                    suppressAfterTransition();
                    break;

                case UiAction::DecreaseSdFontSizeBig:
                    if (g_readerText.isSdFont()) {
                        g_readerText.stepSdFontSize(-4);
                        g_readerBook.invalidatePaginationForLayoutChange();
                    }
                    state_ = SystemState::Settings;
                    renderState(state_);
                    g_displayService.enqueueFull(false, 100);
                    suppressAfterTransition();
                    break;

                case UiAction::DecreaseSdFontSize:
                    if (g_readerText.isSdFont()) {
                        g_readerText.stepSdFontSize(-1);
                        g_readerBook.invalidatePaginationForLayoutChange();
                    }
                    state_ = SystemState::Settings;
                    renderState(state_);
                    g_displayService.enqueueFull(false, 100);
                    suppressAfterTransition();
                    break;

                case UiAction::IncreaseSdFontSize:
                    if (g_readerText.isSdFont()) {
                        g_readerText.stepSdFontSize(1);
                        g_readerBook.invalidatePaginationForLayoutChange();
                    }
                    state_ = SystemState::Settings;
                    renderState(state_);
                    g_displayService.enqueueFull(false, 100);
                    suppressAfterTransition();
                    break;

                case UiAction::IncreaseSdFontSizeBig:
                    if (g_readerText.isSdFont()) {
                        g_readerText.stepSdFontSize(4);
                        g_readerBook.invalidatePaginationForLayoutChange();
                    }
                    state_ = SystemState::Settings;
                    renderState(state_);
                    g_displayService.enqueueFull(false, 100);
                    suppressAfterTransition();
                    break;

                case UiAction::CycleReaderLayoutPreset:
                    g_readerText.cycleLayoutPreset();
                    g_readerBook.invalidatePaginationForLayoutChange();
                    state_ = SystemState::Settings;
                    renderState(state_);
                    g_displayService.enqueueFull(false, 100);
                    suppressAfterTransition();
                    break;

                case UiAction::CycleReaderPageMargin:
                    g_readerText.cyclePageMargin();
                    g_readerBook.invalidatePaginationForLayoutChange();
                    state_ = SystemState::Settings;
                    renderState(state_);
                    g_displayService.enqueueFull(false, 100);
                    suppressAfterTransition();
                    break;

                case UiAction::CycleReaderLineSpacing:
                    g_readerText.cycleLineSpacing();
                    g_readerBook.invalidatePaginationForLayoutChange();
                    state_ = SystemState::Settings;
                    renderState(state_);
                    g_displayService.enqueueFull(false, 100);
                    suppressAfterTransition();
                    break;

                case UiAction::ToggleReaderPageTurnEffect:
                    g_readerText.togglePageTurnEffect();
                    state_ = SystemState::Settings;
                    renderState(state_);
                    g_displayService.enqueueFull(false, 100);
                    suppressAfterTransition();
                    break;

                case UiAction::CycleReaderPageTurnProfile:
                    g_displayService.cycleReaderPageTurnProfile();
                    state_ = SystemState::Settings;
                    renderState(state_);
                    g_displayService.enqueueFull(false, 100);
                    suppressAfterTransition();
                    break;

                case UiAction::CycleReaderPageTurnEngine:
                    g_displayService.cycleReaderPageTurnEngine();
                    state_ = SystemState::Settings;
                    renderState(state_);
                    g_displayService.enqueueFull(false, 100);
                    suppressAfterTransition();
                    break;

                case UiAction::ToggleWifiAp:
                    if (g_wifiService.httpServerRunning()) {
                        g_wifiService.stop();
                    } else {
                        g_wifiService.startAp("Vink-PaperS3", nullptr, true);
                    }
                    state_ = SystemState::Transfer;
                    renderState(state_);
                    g_displayService.enqueueFull(false, 100);
                    suppressAfterTransition();
                    break;

                case UiAction::OpenReaderSettings:
                    g_uiRenderer.showReaderSettings();
                    state_ = SystemState::Settings;
                    g_uiRenderer.renderSettings();
                    g_displayService.enqueueFull(false, 100);
                    suppressAfterTransition();
                    break;

                case UiAction::BackToSettings:
                    g_uiRenderer.hideReaderSettings();
                    g_uiRenderer.hideSystemSettings();
                    state_ = SystemState::Settings;
                    g_uiRenderer.renderSettings();
                    g_displayService.enqueueFull(false, 100);
                    suppressAfterTransition();
                    break;

                case UiAction::OpenSystemSettings:
                    g_uiRenderer.showSystemSettings();
                    state_ = SystemState::Settings;
                    g_uiRenderer.renderSettings();
                    g_displayService.enqueueFull(false, 100);
                    suppressAfterTransition();
                    break;

                case UiAction::ToggleDoubleTapUnlock:
                    g_readerText.toggleDoubleTapUnlock();
                    state_ = SystemState::Settings;
                    g_uiRenderer.renderSettings();
                    g_displayService.enqueueFull(false, 100);
                    suppressAfterTransition();
                    break;

                case UiAction::CycleAutoOffMinutes:
                    g_readerText.cycleAutoOffMinutes();
                    s_autoOffMinutes = ReaderTextRenderer::autoOffValueFromIndex(g_readerText.autoOffMinutesIndex());
                    state_ = SystemState::Settings;
                    g_uiRenderer.renderSettings();
                    g_displayService.enqueueFull(false, 100);
                    suppressAfterTransition();
                    break;

                case UiAction::OpenCurrentBook:
                {
                    state_ = SystemState::ReaderMenu;
                    g_readerBook.renderOpenOrHelp();
                    g_displayService.enqueueFull(false, 100);
                    suppressAfterTransition();
                    break;
                }

                case UiAction::OpenCurrentBookToc:
                {
                    if (!g_readerBook.isOpen()) g_readerBook.openLastBook();
                    if (g_readerBook.isOpen()) {
                        g_readerBook.showTocForCurrentBook();
                        state_ = SystemState::ReaderMenu;
                        g_readerBook.renderCurrent();
                    }
                    g_displayService.enqueueFull(false, 100);
                    suppressAfterTransition();
                    break;
                }

                case UiAction::RestartCurrentBook:
                {
                    if (g_readerBook.isOpen()) {
                        g_readerBook.restartReading();
                    } else {
                        g_readerBook.openLastBook();
                    }
                    state_ = SystemState::ReaderMenu;
                    g_readerBook.renderCurrent();
                    g_displayService.enqueueFull(false, 100);
                    suppressAfterTransition();
                    break;
                }

                case UiAction::None:
                    if (state_ == SystemState::Reader || state_ == SystemState::Home) {
                        if (g_readerBook.handleReaderHomeTap(message.touch.x, message.touch.y)) {
                            if (g_readerBook.lastLibraryTapOpenedBook()) {
                                state_ = SystemState::ReaderMenu;
                                g_readerBook.renderCurrent();
                            }
                            g_displayService.enqueueFull(false, 100);
                        }
                    } else if (state_ == SystemState::ReaderMenu) {
                        // Only TOC and book-entry pages draw visible tabs.
                        // Reading body and menu overlay have no tabs.
                        if (!g_readerBook.isReadingBody() && !g_readerBook.isShowingReaderMenu()) {
                        const UiAction tabAction = g_uiRenderer.hitTestTabs(message.touch.x, message.touch.y);
                        if (tabAction != UiAction::None) {
                            g_uiRenderer.hideReaderSettings();
                            state_ = tabStateForAction(tabAction);
                            renderState(state_);
                            g_displayService.enqueueFull(shouldQualityRefreshTabSwitch(), 100);
                            suppressAfterTransition();
                            break;
                        }
                        }
                        if (g_readerBook.handleTap(message.touch.x, message.touch.y)) {
                        if (g_readerBook.consumeLastTapBackHome()) {
                            state_ = SystemState::Reader;
                            renderState(state_);
                            g_displayService.enqueueFull(false, 100);
                            suppressAfterTransition();
                        } else if (g_readerBook.consumeLastTapPageTurn()) {
                            enqueueReaderAwareRefresh(g_readerBook.consumeLastTapNextPage()
                                ? DisplayEffect::VerticalShutter
                                : DisplayEffect::HorizontalShutter);
                        } else {
                            g_readerBook.renderCurrent();
                            g_displayService.enqueueFull(false, 100);
                        }
                        }
                    } else if (state_ == SystemState::Library && g_readerBook.handleShelfTap(message.touch.x, message.touch.y)) {
                        if (g_readerBook.lastLibraryTapOpenedBook()) {
                            state_ = SystemState::ReaderMenu;
                            g_readerBook.renderCurrent();
                        }
                        g_displayService.enqueueFull(false, 100);
                    }
                    break;

                case UiAction::BackHome:
                default:
                    break;
            }
            break;
        }

        case MessageType::PageNext:
            if (state_ == SystemState::ReaderMenu && g_readerBook.isReadingBodyVisible() && g_readerBook.nextPage()) {
                enqueueReaderAwareRefresh(DisplayEffect::VerticalShutter);
                suppressAfterTransition();
            }
            break;

        case MessageType::PagePrev:
            if (state_ == SystemState::ReaderMenu && g_readerBook.isReadingBodyVisible() && g_readerBook.prevPage()) {
                enqueueReaderAwareRefresh(DisplayEffect::HorizontalShutter);
                suppressAfterTransition();
            }
            break;

        case MessageType::SwipeLeft:
            s_lastInteractionMs = millis();
            if (state_ == SystemState::Diagnostics) {
                g_uiRenderer.renderDiagnostics(message, "swipe-left");
                g_displayService.enqueueFull(false, 100);
                break;
            }
            if (state_ == SystemState::ReaderMenu) {
                if (g_readerBook.isShowingToc()) {
                    if (g_readerBook.nextTocPage()) g_displayService.enqueueFull(false, 100);
                } else if (g_readerBook.nextPage()) {
                    enqueueReaderAwareRefresh(DisplayEffect::VerticalShutter);
                }
                break;
            }
            // Library: swipe left = next shelf page (no tab switching).
            if (state_ == SystemState::Library) {
                if (g_readerBook.nextShelfPage()) g_displayService.enqueueFull(false, 100);
                break;
            }
            break;

        case MessageType::SwipeRight:
            s_lastInteractionMs = millis();
            if (state_ == SystemState::Diagnostics) {
                g_uiRenderer.renderDiagnostics(message, "swipe-right");
                g_displayService.enqueueFull(false, 100);
                break;
            }
            if (state_ == SystemState::ReaderMenu) {
                if (g_readerBook.isShowingToc()) {
                    if (g_readerBook.prevTocPage()) g_displayService.enqueueFull(false, 100);
                } else if (g_readerBook.prevPage()) {
                    enqueueReaderAwareRefresh(DisplayEffect::HorizontalShutter);
                }
                break;
            }
            // Library: swipe right = previous shelf page (no tab switching).
            if (state_ == SystemState::Library) {
                if (g_readerBook.prevShelfPage()) g_displayService.enqueueFull(false, 100);
                break;
            }
            break;

        case MessageType::SwipeUp:
            s_lastInteractionMs = millis();
            if (state_ == SystemState::Settings) {
                if (g_uiRenderer.scrollSettings(+1)) {
                    g_uiRenderer.renderSettings();
                    g_displayService.enqueueFull(false, 100);
                }
                suppressAfterTransition();
                break;
            }
            if (state_ == SystemState::SystemLogs) {
                if (g_uiRenderer.scrollSystemLogs(+1)) {
                    g_uiRenderer.renderSystemLogs();
                    g_displayService.enqueueFull(false, 100);
                }
                suppressAfterTransition();
                break;
            }
            if (state_ == SystemState::Diagnostics) {
                g_uiRenderer.renderDiagnostics(message, "swipe-up");
                g_displayService.enqueueFull(false, 100);
                break;
            }
            if (state_ == SystemState::ReaderMenu && g_readerBook.nextPage()) {
                enqueueReaderAwareRefresh(DisplayEffect::VerticalShutter);
            } else if (state_ == SystemState::Library && g_readerBook.nextShelfPage()) {
                g_displayService.enqueueFull(false, 100);
            }
            break;

        case MessageType::SwipeDown:
            s_lastInteractionMs = millis();
            if (state_ == SystemState::Settings) {
                if (g_uiRenderer.scrollSettings(-1)) {
                    g_uiRenderer.renderSettings();
                    g_displayService.enqueueFull(false, 100);
                }
                suppressAfterTransition();
                break;
            }
            if (state_ == SystemState::SystemLogs) {
                if (g_uiRenderer.scrollSystemLogs(-1)) {
                    g_uiRenderer.renderSystemLogs();
                    g_displayService.enqueueFull(false, 100);
                }
                suppressAfterTransition();
                break;
            }
            if (state_ == SystemState::Diagnostics) {
                g_uiRenderer.renderDiagnostics(message, "swipe-down");
                g_displayService.enqueueFull(false, 100);
                break;
            }
            if (state_ == SystemState::ReaderMenu && g_readerBook.prevPage()) {
                enqueueReaderAwareRefresh(DisplayEffect::HorizontalShutter);
            } else if (state_ == SystemState::Library && g_readerBook.prevShelfPage()) {
                g_displayService.enqueueFull(false, 100);
            }
            break;

        case MessageType::LongPress:
            if (state_ == SystemState::Diagnostics) {
                g_uiRenderer.renderDiagnostics(message, "long-press");
                g_displayService.enqueueFull(false, 100);
            }
            break;

        case MessageType::PowerButton:
            state_ = SystemState::Shutdown;
            shutdownPaperS3("正在关机");
            break;

        case MessageType::SleepTimeout:
            // v0.3 does not auto-sleep yet; keep the message explicit so future
            // timeout logic cannot silently enter an unvalidated sleep path.
            Serial.println("[vink3][power] SleepTimeout ignored until real-device wake validation");
            break;

        case MessageType::TouchDown:
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
