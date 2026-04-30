#include "StateMachine.h"
#include "../display/DisplayService.h"
#include "../reader/ReaderBookService.h"
#include "../reader/ReaderTextRenderer.h"
#include "../sync/LegadoService.h"
#include "../ui/VinkUiRenderer.h"
#include "../input/InputService.h"
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

void pulsePaperS3PowerOffPin() {
    pinMode(static_cast<int>(kPowerOffPulsePin), OUTPUT);
    digitalWrite(static_cast<int>(kPowerOffPulsePin), HIGH);
    delay(150);
    digitalWrite(static_cast<int>(kPowerOffPulsePin), LOW);
    delay(100);
}

void waitPowerKeyRelease(uint32_t timeoutMs = 3000) {
    const uint32_t start = millis();
    while (M5.BtnPWR.isPressed() && millis() - start < timeoutMs) {
        M5.update();
        delay(30);
    }
}

void shutdownPaperS3(const char* reason) {
    Serial.println("[vink3][power] shutdown requested");
    g_readerBook.saveCurrentProgress();
    g_uiRenderer.renderShutdown(reason ? reason : "正在关机");
    g_displayService.enqueueFull(true, 100);
    g_displayService.waitIdle(5000);
    delay(300);

    // Official/factory order first: sleep the EPD, wait for it, then ask
    // M5Unified to power off. GPIO44 is retained only as a fallback for units
    // where M5.Power.powerOff() does not fully cut power.
    waitPowerKeyRelease();
    M5.Display.sleep();
    M5.Display.waitDisplay();
    delay(200);
    M5.Power.powerOff();

    delay(500);
    pulsePaperS3PowerOffPin();
    waitPowerKeyRelease();
    // Do not arm GPIO36 as wake source: on PaperS3 it is not a verified side
    // power-key input and can make the shutdown fallback appear broken.
    esp_deep_sleep_start();
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
        case SystemState::Diagnostics:
        {
            Message blank;
            blank.timestampMs = millis();
            g_uiRenderer.renderDiagnostics(blank, "等待触摸");
            break;
        }
        case SystemState::LegadoSync:
            g_uiRenderer.renderLegadoSync("Legado sync service ready");
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
                    g_uiRenderer.renderLegadoSync("Starting Legado sync...");
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
            if (state_ == SystemState::Reader) state_ = SystemState::Library;
            else if (state_ == SystemState::Transfer) state_ = SystemState::Settings;
            renderState(state_);
            g_displayService.enqueueFull(false, 100);
            break;

        case MessageType::SwipeRight:
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
            if (state_ == SystemState::Settings) state_ = SystemState::Transfer;
            else if (state_ == SystemState::Transfer) state_ = SystemState::Library;
            renderState(state_);
            g_displayService.enqueueFull(false, 100);
            break;

        case MessageType::SwipeUp:
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
            state_ = SystemState::LegadoSync;
            g_uiRenderer.renderLegadoSync("Syncing reading progress...");
            g_displayService.enqueueFull(false, 100);
            // Placeholder until real HTTP API integration; keep result asynchronous via state message.
            {
                Message done;
                done.type = MessageType::LegadoSyncDone;
                done.timestampMs = millis();
                post(done, 20);
            }
            break;

        case MessageType::LegadoSyncDone:
            state_ = SystemState::LegadoSync;
            g_uiRenderer.renderLegadoSync("Sync complete");
            g_displayService.enqueueFull(false, 100);
            break;

        case MessageType::LegadoSyncFailed:
            state_ = SystemState::LegadoSync;
            g_uiRenderer.renderLegadoSync("Sync failed");
            g_displayService.enqueueFull(true, 100);
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
