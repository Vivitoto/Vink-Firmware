#include "InputService.h"
#include "../display/DisplayService.h"
#include "../reader/ReaderBookService.h"
#include "../system/SystemLog.h"
#include "../VinkPaperS3.h"

namespace vink3 {

InputService g_inputService;

namespace {
// EDCBook / M5ReadPaper input model: a small fixed-period task calls
// M5.update(), then consumes M5Unified's edge flags.  The previous GT911 INT
// experiment made the app depend on an edge that can be missed or masked while
// the EPD worker is busy; fixed 10 ms polling is boring, but it is predictable.
constexpr uint32_t kInputTickMs = 10;
constexpr uint32_t kDebounceMs = 18;
constexpr uint8_t kLiftDebounceFrames = 3;
constexpr uint32_t kReaderQuickTurnStableMs = 35; // legacy ceiling; EDCBook-style reader turns now fire on press-down
constexpr uint32_t kReaderQuickTurnMinIntervalMs = 90;
constexpr uint32_t kMoveDiagnosticMs = 120;
constexpr uint32_t kPowerBootIgnoreMs = 3000;
constexpr uint32_t kLongPressMs = 600;
constexpr int16_t kTapSlopPx = 50;
constexpr int16_t kLongPressMovePx = 48;
constexpr int16_t kSwipeThresholdPx = 82;

const char* touchCoordModeName(TouchCoordMode) {
    return "official-raw-540x960";
}

TouchPoint transformRawPaperS3Point(int rawX, int rawY) {
    // Official PaperS3 baseline: M5Unified reports logical 540x960 portrait
    // coordinates after rotation 0.  Keep the input layer raw and clamp only;
    // no coordinate-mode guessing, no inferred rotation/remap.
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

Message makeTouchMessage(MessageType type, uint32_t now, const TouchPoint& point,
                         const TouchPoint& rawPoint, int32_t value = 0) {
    Message msg;
    msg.type = type;
    msg.timestampMs = now;
    msg.touch = point;
    msg.rawTouch = rawPoint;
    msg.value = value;
    return msg;
}
} // namespace

bool InputService::begin(StateMachine* stateMachine) {
    if (!stateMachine) return false;
    stateMachine_ = stateMachine;

    if (!task_) {
        BaseType_t ok = xTaskCreatePinnedToCore(taskThunk, "vink3-input", 8192, this, 4, &task_, 0);
        if (ok != pdPASS) {
            task_ = nullptr;
            return false;
        }
    }

    // PaperS3's side key is handled by the AXP2101 PMIC. Keep it diagnostic-only
    // until the real device confirms the exact M5Unified event mapping.
    M5.BtnPWR.setDebounceThresh(0);
    M5.BtnPWR.setHoldThresh(0);

    Serial.println("[vink3][input] service started; EDCBook-style 10ms M5.update touch polling");
    return true;
}

void InputService::taskThunk(void* arg) {
    static_cast<InputService*>(arg)->taskLoop();
}

void InputService::taskLoop() {
    TickType_t lastWake = xTaskGetTickCount();
    for (;;) {
        M5.update();
        const uint32_t now = millis();
        pollPowerButton(now);
        pollTouch(now);
        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(kInputTickMs));
    }
}

void InputService::resetTouchGesture() {
    wasPressed_ = false;
    pressConsumed_ = false;
    lastMovePostMs_ = 0;
    liftMissingFrames_ = 0;
}

void InputService::suppressFor(uint32_t cooldownMs) {
    suppressUntilMs_ = millis() + cooldownMs;
    // Treat transition suppression as “ignore this physical press until it is
    // lifted”. This prevents the release edge of the tap that caused a page
    // change from being reinterpreted on the freshly rendered page.
    waitRelease_ = true;
    resetTouchGesture();
    pendingReaderTurn_ = false;
    Serial.printf("[vink3][touch] suppress for %lu ms / until release\n", static_cast<unsigned long>(cooldownMs));
}

void InputService::suppressUntilRelease(uint32_t cooldownMs) {
    suppressUntilMs_ = millis() + cooldownMs;
    waitRelease_ = true;
    resetTouchGesture();
    pendingReaderTurn_ = false;
    Serial.printf("[vink3][touch] suppress until release for %lu ms\n", static_cast<unsigned long>(cooldownMs));
}

bool InputService::readerQuickTurnZone(const TouchPoint& point, bool& outNext) const {
    if (!stateMachine_ || stateMachine_->state() != SystemState::ReaderMenu) return false;
    if (!g_readerBook.isReadingBodyVisible()) return false;
    if (point.y < 150) return false; // keep header/book-entry/TOC gestures calm
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

bool InputService::postReaderQuickTurn(bool next, uint32_t now, const TouchPoint& point, const TouchPoint& rawPoint) {
    if (!stateMachine_ || now - lastReaderQuickTurnMs_ < kReaderQuickTurnMinIntervalMs) return false;
    if (stateMachine_->state() != SystemState::ReaderMenu || !g_readerBook.isReadingBodyVisible()) return false;

    Message msg = makeTouchMessage(next ? MessageType::PageNext : MessageType::PagePrev,
                                   now, point, rawPoint, next ? 1 : -1);
    if (!stateMachine_->post(msg, 0)) return false;
    lastReaderQuickTurnMs_ = now;
    g_systemLog.append(next ? "reader press turn next" : "reader press turn prev");
    return true;
}

void InputService::pollPowerButton(uint32_t now) {
    if (!stateMachine_) return;

    // Use PMIC PKEY events instead of GPIO-level polling: wasClicked()/wasHold().
    const bool clicked = M5.BtnPWR.wasClicked();
    const bool singleClicked = M5.BtnPWR.wasSingleClicked();
    const bool held = M5.BtnPWR.wasHold();
    if (!powerArmed_) {
        if (now > kPowerBootIgnoreMs) {
            powerArmed_ = true;
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
    // Intentionally unused. PaperS3 side key is not a stable GPIO input.
}

void InputService::pollTouch(uint32_t now) {
    if (!stateMachine_) return;

    if (!M5.Touch.isEnabled()) {
        waitRelease_ = false;
        resetTouchGesture();
        return;
    }

    auto detail = M5.Touch.getDetail();
    const int count = M5.Touch.getCount();
    const bool pressed = detail.isPressed() && count > 0;
    const TouchPoint rawPoint(static_cast<int16_t>(detail.x), static_cast<int16_t>(detail.y));
    const TouchPoint currentPoint = normalizeTouchPoint(detail.x, detail.y);
    const bool displayPushing = g_inDisplayPush;

    if (!displayPushing && pendingReaderTurn_) {
        const bool next = pendingReaderTurnNext_;
        const TouchPoint point = pendingReaderTurnPoint_;
        const TouchPoint raw = pendingReaderTurnRawPoint_;
        pendingReaderTurn_ = false;
        if (postReaderQuickTurn(next, now, point, raw)) {
            waitRelease_ = pressed;
            suppressUntilMs_ = now + 20;
            return;
        }
    }

    if (displayPushing) {
        bool next = false;
        if (pressed && readerQuickTurnZone(currentPoint, next)) {
            pendingReaderTurn_ = true;
            pendingReaderTurnNext_ = next;
            pendingReaderTurnPoint_ = currentPoint;
            pendingReaderTurnRawPoint_ = rawPoint;
            resetTouchGesture();
            return;
        }
        // Do not globally swallow UI taps while the EPD is refreshing.  v0.4.38
        // real-device testing showed that a long display-push window made tabs
        // and buttons feel dead.  Reader body page-turns still use the pending
        // intent path above; ordinary UI gestures continue through the normal
        // state-machine queue and will schedule the next display update.
    }

    if (waitRelease_ || now < suppressUntilMs_) {
        if (!pressed && now >= suppressUntilMs_) {
            waitRelease_ = false;
            resetTouchGesture();
            Serial.println("[vink3][touch] release observed, suppression cleared");
        } else if (pressed) {
            lastPoint_ = currentPoint;
            lastRawPoint_ = rawPoint;
        }
        return;
    }

    const bool pressEdge = detail.wasPressed() || (pressed && !wasPressed_);
    if (pressEdge) {
        if (now - lastPressMs_ < kDebounceMs) return;
        lastPressMs_ = now;
        wasPressed_ = true;
        pressConsumed_ = false;
        liftMissingFrames_ = 0;
        pressStartedMs_ = now;
        pressPoint_ = currentPoint;
        lastPoint_ = currentPoint;
        pressRawPoint_ = rawPoint;
        lastRawPoint_ = rawPoint;
        lastMovePostMs_ = now;

        Serial.printf("[vink3][touch] down raw=%d,%d norm=%d,%d count=%d mode=%s\n",
                      rawPoint.x, rawPoint.y, currentPoint.x, currentPoint.y, count,
                      touchCoordModeName(gPaperS3TouchCoordMode));

        bool next = false;
        if (readerQuickTurnZone(currentPoint, next) && postReaderQuickTurn(next, now, currentPoint, rawPoint)) {
            // EDCBook handles reading page-turns on press, not release. The
            // release edge is deliberately swallowed so one tap cannot become a
            // page turn plus a center/menu tap after the page redraws.
            pressConsumed_ = true;
            waitRelease_ = true;
            suppressUntilMs_ = now + 20;
            return;
        }

        stateMachine_->post(makeTouchMessage(MessageType::TouchDown, now, pressPoint_, pressRawPoint_, count), 0);
        return;
    }

    if (pressed && wasPressed_) {
        lastPoint_ = currentPoint;
        lastRawPoint_ = rawPoint;
        if (!pressConsumed_ && now - lastMovePostMs_ >= kMoveDiagnosticMs) {
            lastMovePostMs_ = now;
            stateMachine_->post(makeTouchMessage(MessageType::TouchMove, now, currentPoint, rawPoint, count), 0);
        }
        return;
    }

    if (!pressed && wasPressed_ && !detail.wasReleased()) {
        if (liftMissingFrames_ < kLiftDebounceFrames) {
            liftMissingFrames_++;
            return;
        }
    }

    const bool releaseEdge = detail.wasReleased() || (!pressed && wasPressed_);
    if (!releaseEdge) return;

    wasPressed_ = false;
    liftMissingFrames_ = 0;
    const TouchPoint releasePoint = lastPoint_;
    const TouchPoint releaseRawPoint = lastRawPoint_;
    const int16_t dx = releasePoint.x - pressPoint_.x;
    const int16_t dy = releasePoint.y - pressPoint_.y;
    const int absDx = abs(dx);
    const int absDy = abs(dy);
    const uint32_t heldMs = now - pressStartedMs_;

    if (pressConsumed_) {
        pressConsumed_ = false;
        waitRelease_ = false;
        Serial.printf("[vink3][touch] up consumed reader-turn dx=%d dy=%d held=%lu\n",
                      dx, dy, static_cast<unsigned long>(heldMs));
        return;
    }

    Serial.printf("[vink3][touch] up raw=%d,%d norm=%d,%d dx=%d dy=%d held=%lu mode=%s\n",
                  releaseRawPoint.x, releaseRawPoint.y, releasePoint.x, releasePoint.y,
                  dx, dy, static_cast<unsigned long>(heldMs), touchCoordModeName(gPaperS3TouchCoordMode));

    stateMachine_->post(makeTouchMessage(MessageType::TouchUp, now, releasePoint, releaseRawPoint, count), 0);

    Message semantic = makeTouchMessage(MessageType::None, now, releasePoint, releaseRawPoint, count);
    if (max(absDx, absDy) <= kTapSlopPx) {
        semantic.type = heldMs >= kLongPressMs ? MessageType::LongPress : MessageType::Tap;
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
    stateMachine_->post(semantic, 0);
}

} // namespace vink3
