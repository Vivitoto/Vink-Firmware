#include "InputService.h"
#include "../display/DisplayService.h"
#include "../reader/ReaderBookService.h"
#include "../system/SystemLog.h"
#include "../VinkPaperS3.h"
#include <driver/gpio.h>

namespace vink3 {

InputService g_inputService;

namespace {
// GT911 INT pin on PaperS3 = GPIO48 (see VinkPaperS3.h).  The GT911 pulls
// INT low when touch data is available.  We configure a falling‑edge ISR
// that gives a binary semaphore so the input task wakes up immediately.
static SemaphoreHandle_t s_touchSemaphore = nullptr;

static void IRAM_ATTR gt911_isr_handler(void*) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (s_touchSemaphore) {
        xSemaphoreGiveFromISR(s_touchSemaphore, &xHigherPriorityTaskWoken);
    }
    if (xHigherPriorityTaskWoken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}
constexpr uint32_t kPollDelayMs = 5;    // ~200 Hz touch polling for responsive feel
// Keep the v0.4.36 GT911 interrupt path, but tune the app-level gesture
// classifier toward EDCBook's reading-first feel: lighter press debounce, a
// wider tap-jitter envelope, and a short multi-frame release debounce so one
// missing GT911/M5Unified sample does not prematurely end a gesture.
constexpr uint32_t kDebounceMs = 18;
constexpr uint8_t kLiftDebounceFrames = 3;
constexpr uint32_t kMoveDiagnosticMs = 100;
constexpr uint32_t kReaderQuickTurnStableMs = 35;
constexpr uint32_t kReaderQuickTurnMinIntervalMs = 120;
constexpr int16_t kReaderQuickTurnCancelPx = 58;
constexpr uint32_t kPowerBootIgnoreMs = 3000;
constexpr uint32_t kLongPressMs = 600;
constexpr int16_t kTapSlopPx = 50;
constexpr int16_t kLongPressMovePx = 50;
constexpr int16_t kSwipeThresholdPx = 80;

const char* touchCoordModeName(TouchCoordMode) {
    return "official-raw-540x960";
}

TouchPoint transformRawPaperS3Point(int rawX, int rawY) {
    // Official PaperS3 touch example draws touchDetail.x/y directly after
    // M5.Display.setRotation(0) and M5.update(). For this official portrait
    // baseline, do not infer, scale, rotate, or remap coordinates in the input layer.
    int x = rawX;
    int y = rawY;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= M5.Display.width()) x = M5.Display.width() - 1;
    if (y >= M5.Display.height()) y = M5.Display.height() - 1;
    return TouchPoint(static_cast<int16_t>(x), static_cast<int16_t>(y));
}

TouchPoint normalizeTouchPoint(int rawX, int rawY) {
    return transformRawPaperS3Point(rawX, rawY);
}
}

bool InputService::begin(StateMachine* stateMachine) {
    if (!stateMachine) return false;
    stateMachine_ = stateMachine;
    if (!task_) {
        BaseType_t ok = xTaskCreatePinnedToCore(taskThunk, "vink3-input", 8192, this, 2, &task_, 1);
        if (ok != pdPASS) {
            task_ = nullptr;
            return false;
        }
    }
    // PaperS3's side key is handled by the AXP2101 PMIC, not by a stable GPIO.
    // M5Unified exposes PMIC PKEY events as BtnPWR wasClicked()/wasHold(); do
    // not use GPIO36 or isPressed() as the shutdown source.
    M5.BtnPWR.setDebounceThresh(0);
    M5.BtnPWR.setHoldThresh(0);

    // GT911 hardware interrupt — wakes the input task on touch instead of polling.
    s_touchSemaphore = xSemaphoreCreateBinary();
    touchSem_ = s_touchSemaphore;
    if (s_touchSemaphore) {
        gpio_config_t io_conf = {};
        io_conf.intr_type = GPIO_INTR_NEGEDGE;  // GT911 asserts INT low on touch
        io_conf.mode = GPIO_MODE_INPUT;
        io_conf.pin_bit_mask = 1ULL << kGt911IntPin;
        io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
        gpio_config(&io_conf);
        gpio_install_isr_service(0);
        gpio_isr_handler_add(kGt911IntPin, gt911_isr_handler, nullptr);
        Serial.println("[vink3][input] GT911 INT on GPIO48, interrupt-driven touch");
    } else {
        Serial.println("[vink3][input] semaphore alloc failed, falling back to poll");
    }
    Serial.println("[vink3][input] service started; side power uses AXP2101 BtnPWR events; GPIO36 is ignored");
    return true;
}

void InputService::taskThunk(void* arg) {
    static_cast<InputService*>(arg)->taskLoop();
}

void InputService::taskLoop() {
    for (;;) {
        // Wait for GT911 interrupt (or timeout after 20 ms as fallback).
        if (touchSem_) {
            xSemaphoreTake(touchSem_, pdMS_TO_TICKS(20));
        } else {
            vTaskDelay(pdMS_TO_TICKS(kPollDelayMs));
        }
        M5.update();
        const uint32_t now = millis();
        pollPowerButton(now);
        pollTouch();
    }
}

void InputService::suppressFor(uint32_t cooldownMs) {
    suppressUntilMs_ = millis() + cooldownMs;
    waitRelease_ = false;
    wasPressed_ = false;
    lastMovePostMs_ = 0;
    liftMissingFrames_ = 0;
    resetReaderQuickTurn();
    pendingReaderTurn_ = false;
    Serial.printf("[vink3][touch] suppress for %lu ms\n", static_cast<unsigned long>(cooldownMs));
}

void InputService::suppressUntilRelease(uint32_t cooldownMs) {
    suppressUntilMs_ = millis() + cooldownMs;
    waitRelease_ = true;
    wasPressed_ = false;
    lastMovePostMs_ = 0;
    liftMissingFrames_ = 0;
    resetReaderQuickTurn();
    pendingReaderTurn_ = false;
    Serial.printf("[vink3][touch] suppress until release for %lu ms\n", static_cast<unsigned long>(cooldownMs));
}

void InputService::updateTouchCoordMode(int, int) {
    // Official baseline: no coordinate-mode guessing.
}

bool InputService::readerQuickTurnZone(const TouchPoint& point, bool& outNext) const {
    // Reading-body only: keep the v0.4.36 GT911 interrupt path, but make body
    // page turns EDCBook-like. Do not make the header, center menu, TOC/book
    // entry, settings, or library eager.
    if (!stateMachine_ || stateMachine_->state() != SystemState::ReaderMenu) return false;
    if (!g_readerBook.isReadingBodyVisible()) return false;
    if (point.y < 150) return false;
    if (point.x < kPaperS3Width / 3) {
        outNext = false;
        return true;
    }
    if (point.x > (kPaperS3Width * 2) / 3) {
        outNext = true;
        return true;
    }
    return false;
}

void InputService::resetReaderQuickTurn() {
    readerQuickTurnCandidate_ = false;
    readerQuickTurnNext_ = false;
    readerQuickTurnFired_ = false;
    readerQuickTurnStartedMs_ = 0;
}

bool InputService::postReaderQuickTurn(bool next, uint32_t now, const TouchPoint& point, const TouchPoint& rawPoint) {
    if (!stateMachine_ || now - lastReaderQuickTurnMs_ < kReaderQuickTurnMinIntervalMs) return false;
    if (stateMachine_->state() != SystemState::ReaderMenu || !g_readerBook.isReadingBodyVisible()) return false;
    Message msg;
    msg.type = next ? MessageType::PageNext : MessageType::PagePrev;
    msg.timestampMs = now;
    msg.touch = point;
    msg.rawTouch = rawPoint;
    msg.value = next ? 1 : -1;
    if (!stateMachine_->post(msg, 0)) return false;
    lastReaderQuickTurnMs_ = now;
    g_systemLog.append(next ? "reader quick turn next" : "reader quick turn prev");
    return true;
}

void InputService::pollPowerButton(uint32_t now) {
    if (!stateMachine_) return;

    // local4 diagnostic mode: for AXP2101 PMIC-backed power keys, M5Unified
    // reports events via wasClicked()/wasHold(). Only log these events here; do
    // not shut down yet. This proves whether the running firmware can observe
    // the side-key before we bind it to graceful shutdown.
    const bool clicked = M5.BtnPWR.wasClicked();
    const bool singleClicked = M5.BtnPWR.wasSingleClicked();
    const bool held = M5.BtnPWR.wasHold();
    if (!powerArmed_) {
        if (now > kPowerBootIgnoreMs) {
            powerArmed_ = true;
            powerWasPressed_ = false;
            powerPressStartedMs_ = 0;
            lastPowerClickMs_ = 0;
            Serial.println("[vink3][power] BtnPWR PMIC diagnostic armed; no shutdown action");
            g_systemLog.append("BtnPWR PMIC diag armed");
        }
        return;
    }

    if (clicked) {
        Serial.println("[vink3][power] BtnPWR PMIC click observed (diagnostic only)");
        g_systemLog.append("BtnPWR PMIC click observed");
    }
    if (singleClicked) {
        Serial.println("[vink3][power] BtnPWR PMIC single-click decided (diagnostic only)");
        g_systemLog.append("BtnPWR PMIC single-click decided");
    }
    if (held) {
        Serial.println("[vink3][power] BtnPWR PMIC hold observed (diagnostic only)");
        g_systemLog.append("BtnPWR PMIC hold observed");
    }
}


void InputService::pollSideKey(uint32_t) {
    // Intentionally unused. v0.4.21 field logs showed GPIO36 repeatedly pulsing
    // LOW throughout the 30 s diagnostic window on real PaperS3 hardware. That
    // makes GPIO36 unsafe as a software side-key shutdown source: any threshold
    // can turn into a delayed false shutdown. Keep side-key handling on
    // M5Unified's BtnPWR abstraction only, and keep the on-screen shutdown
    // button as the reliable fallback.
}

void InputService::pollTouch() {
    if (!stateMachine_) return;

    const bool displayPushing = g_inDisplayPush;
    if (!M5.Touch.isEnabled()) {
        if (wasPressed_) {
            wasPressed_ = false;
            waitRelease_ = false;
        }
        liftMissingFrames_ = 0;
        resetReaderQuickTurn();
        pendingReaderTurn_ = false;
        return;
    }

    auto detail = M5.Touch.getDetail();
    const int count = M5.Touch.getCount();
    const bool pressed = detail.isPressed() && count == 1;
    const uint32_t now = millis();

    if (!displayPushing && pendingReaderTurn_) {
        const bool next = pendingReaderTurnNext_;
        const TouchPoint point = pendingReaderTurnPoint_;
        const TouchPoint rawPoint = pendingReaderTurnRawPoint_;
        pendingReaderTurn_ = false;
        if (g_readerBook.isReadingBodyVisible() && postReaderQuickTurn(next, now, point, rawPoint)) {
            wasPressed_ = false;
            resetReaderQuickTurn();
            if (pressed) {
                waitRelease_ = true;
                suppressUntilMs_ = now + 80;
            }
            return;
        }
    }

    if (pressed) {
        liftMissingFrames_ = 0;
        updateTouchCoordMode(detail.x, detail.y);
        const TouchPoint rawPoint(static_cast<int16_t>(detail.x), static_cast<int16_t>(detail.y));
        const TouchPoint currentPoint = normalizeTouchPoint(detail.x, detail.y);

        // During display pushes and page transitions, keep internal edge state
        // fresh but do not emit normal UI actions. For reading-body left/right
        // zones, preserve one EDCBook-style page-turn intent so rapid taps during
        // EPD refresh are not swallowed.
        if (displayPushing || waitRelease_ || now < suppressUntilMs_) {
            lastPoint_ = currentPoint;
            lastRawPoint_ = rawPoint;
            if (displayPushing && !pendingReaderTurn_) {
                bool next = false;
                if (readerQuickTurnZone(currentPoint, next)) {
                    pendingReaderTurn_ = true;
                    pendingReaderTurnNext_ = next;
                    pendingReaderTurnPoint_ = currentPoint;
                    pendingReaderTurnRawPoint_ = rawPoint;
                    pendingReaderTurnMs_ = now;
                }
            }
            if (displayPushing) {
                // Keep edge state clean between refreshes. If the same physical
                // press is still held after a quick-turn action, wait for release
                // rather than emitting a second tap on the new page.
                wasPressed_ = false;
            }
            return;
        }

        if (!wasPressed_) {
            if (now - lastEventMs_ < kDebounceMs) return;
            wasPressed_ = true;
            pressStartedMs_ = now;
            pressPoint_ = currentPoint;
            lastPoint_ = currentPoint;
            pressRawPoint_ = rawPoint;
            lastRawPoint_ = rawPoint;
            lastMovePostMs_ = now;
            readerQuickTurnCandidate_ = readerQuickTurnZone(currentPoint, readerQuickTurnNext_);
            readerQuickTurnFired_ = false;
            readerQuickTurnPressPoint_ = currentPoint;
            readerQuickTurnRawPoint_ = rawPoint;
            readerQuickTurnStartedMs_ = now;
            Serial.printf("[vink3][touch] down raw=%d,%d norm=%d,%d count=%d mode=%s\n",
                          rawPoint.x, rawPoint.y, currentPoint.x, currentPoint.y, count,
                          touchCoordModeName(gPaperS3TouchCoordMode));
            Message msg;
            msg.type = MessageType::TouchDown;
            msg.timestampMs = now;
            msg.touch = pressPoint_;
            msg.rawTouch = pressRawPoint_;
            msg.value = count;
            stateMachine_->post(msg);
            return;
        }

        lastPoint_ = currentPoint;
        lastRawPoint_ = rawPoint;
        if (readerQuickTurnCandidate_ && !readerQuickTurnFired_) {
            const int16_t qdx = currentPoint.x - readerQuickTurnPressPoint_.x;
            const int16_t qdy = currentPoint.y - readerQuickTurnPressPoint_.y;
            if (max(abs(qdx), abs(qdy)) > kReaderQuickTurnCancelPx) {
                resetReaderQuickTurn();
            } else if (now - readerQuickTurnStartedMs_ >= kReaderQuickTurnStableMs &&
                       postReaderQuickTurn(readerQuickTurnNext_, now, readerQuickTurnPressPoint_, readerQuickTurnRawPoint_)) {
                readerQuickTurnFired_ = true;
                wasPressed_ = false;
                waitRelease_ = true;
                suppressUntilMs_ = now + 80;
                return;
            }
        }
        if (now - lastMovePostMs_ >= kMoveDiagnosticMs) {
            lastMovePostMs_ = now;
            Message move;
            move.type = MessageType::TouchMove;
            move.timestampMs = now;
            move.touch = currentPoint;
            move.rawTouch = rawPoint;
            move.value = count;
            stateMachine_->post(move, 0);
        }
        return;
    }

    if (!pressed && wasPressed_) {
        // GT911/M5Unified can occasionally miss a frame while a finger is still
        // down. EDCBook-like processing waits for several absent frames before
        // emitting lift; this costs ~15-60 ms on the interrupt+timeout loop but
        // avoids flaky taps and broken swipes from single missing samples.
        if (liftMissingFrames_ < kLiftDebounceFrames) {
            liftMissingFrames_++;
            return;
        }
    }

    if (!pressed) {
        if (!wasPressed_) liftMissingFrames_ = 0;
        if (waitRelease_ && now >= suppressUntilMs_) {
            waitRelease_ = false;
            Serial.println("[vink3][touch] release observed, suppression cleared");
        }
        if (displayPushing) return;
    }

    if (!pressed && wasPressed_) {
        wasPressed_ = false;
        liftMissingFrames_ = 0;
        resetReaderQuickTurn();
        lastEventMs_ = now;
        const TouchPoint releasePoint = lastPoint_;
        const TouchPoint releaseRawPoint = lastRawPoint_;
        const int16_t dx = releasePoint.x - pressPoint_.x;
        const int16_t dy = releasePoint.y - pressPoint_.y;
        const int absDx = abs(dx);
        const int absDy = abs(dy);
        const uint32_t heldMs = now - pressStartedMs_;
        Serial.printf("[vink3][touch] up raw=%d,%d norm=%d,%d dx=%d dy=%d held=%lu mode=%s\n",
                      releaseRawPoint.x, releaseRawPoint.y, releasePoint.x, releasePoint.y,
                      dx, dy, static_cast<unsigned long>(heldMs), touchCoordModeName(gPaperS3TouchCoordMode));

        Message up;
        up.type = MessageType::TouchUp;
        up.timestampMs = now;
        up.touch = releasePoint;
        up.rawTouch = releaseRawPoint;
        up.value = count;
        stateMachine_->post(up);

        Message semantic;
        semantic.timestampMs = now;
        semantic.rawTouch = releaseRawPoint;
        semantic.value = count;
        if (max(absDx, absDy) <= kTapSlopPx) {
            semantic.type = heldMs >= kLongPressMs ? MessageType::LongPress : MessageType::Tap;
            // Hit-test taps at press-down coordinate so release jitter does not
            // move a small button/list tap to a neighbouring target.
            semantic.touch = pressPoint_;
        } else if (max(absDx, absDy) <= kLongPressMovePx && heldMs >= kLongPressMs) {
            semantic.type = MessageType::LongPress;
            semantic.touch = pressPoint_;
        } else if (absDx > absDy && absDx >= kSwipeThresholdPx) {
            semantic.type = dx > 0 ? MessageType::SwipeRight : MessageType::SwipeLeft;
            semantic.touch = releasePoint;
        } else if (absDy >= kSwipeThresholdPx) {
            semantic.type = dy > 0 ? MessageType::SwipeDown : MessageType::SwipeUp;
            semantic.touch = releasePoint;
        } else {
            Serial.println("[vink3][touch] gesture cancelled: moved too far for tap, too short for swipe");
            return;
        }
        stateMachine_->post(semantic);
    }
}


} // namespace vink3
