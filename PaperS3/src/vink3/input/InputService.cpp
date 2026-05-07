#include "InputService.h"
#include "../display/DisplayService.h"
#include "../ReadPaper176.h"

namespace vink3 {

InputService g_inputService;

namespace {
constexpr uint32_t kPollDelayMs = 10;
constexpr uint32_t kDebounceMs = 120;
constexpr uint32_t kMoveDiagnosticMs = 100;
constexpr uint32_t kPowerBootIgnoreMs = 1200;
constexpr uint32_t kPowerStablePressMs = 60;
constexpr uint32_t kPowerLongHoldMs = 1200;
constexpr uint32_t kLongPressMs = 700;
constexpr int16_t kTapSlopPx = 30;
constexpr int16_t kLongPressMovePx = 34;
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
    // PaperS3 side power key is exposed through M5Unified BtnPWR; GPIO36 is
    // a legacy M5Paper touch interrupt in references, not a reliable PaperS3
    // power-key input. Do not configure/read GPIO36 as a power button.
    M5.BtnPWR.setDebounceThresh(0);
    M5.BtnPWR.setHoldThresh(0);
    Serial.println("[vink3][input] service started");
    return true;
}

void InputService::taskThunk(void* arg) {
    static_cast<InputService*>(arg)->taskLoop();
}

void InputService::taskLoop() {
    for (;;) {
        M5.update();
        const uint32_t now = millis();
        pollPowerButton(now);
        pollTouch();
        vTaskDelay(pdMS_TO_TICKS(kPollDelayMs));
    }
}

void InputService::suppressUntilRelease(uint32_t cooldownMs) {
    suppressUntilMs_ = millis() + cooldownMs;
    waitRelease_ = true;
    wasPressed_ = false;
    lastMovePostMs_ = 0;
    Serial.printf("[vink3][touch] suppress until release for %lu ms\n", static_cast<unsigned long>(cooldownMs));
}

void InputService::updateTouchCoordMode(int, int) {
    // Official baseline: no coordinate-mode guessing.
}

void InputService::pollTouch() {
    if (!stateMachine_) return;

    const bool displayPushing = g_inDisplayPush;
    if (!M5.Touch.isEnabled()) {
        if (wasPressed_) {
            wasPressed_ = false;
            waitRelease_ = false;
        }
        return;
    }

    auto detail = M5.Touch.getDetail();
    const int count = M5.Touch.getCount();
    const bool pressed = detail.isPressed() && count == 1;
    const uint32_t now = millis();

    if (pressed) {
        updateTouchCoordMode(detail.x, detail.y);
        const TouchPoint rawPoint(static_cast<int16_t>(detail.x), static_cast<int16_t>(detail.y));
        const TouchPoint currentPoint = normalizeTouchPoint(detail.x, detail.y);

        // During display pushes and page transitions, keep internal edge state
        // fresh but do not emit UI actions. This avoids stale release/tap events
        // landing on a newly rendered page.
        if (displayPushing || waitRelease_ || now < suppressUntilMs_) {
            lastPoint_ = currentPoint;
            lastRawPoint_ = rawPoint;
            if (displayPushing) {
                waitRelease_ = true;
                suppressUntilMs_ = max<uint32_t>(suppressUntilMs_, now + 150);
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

    if (!pressed) {
        if (waitRelease_ && now >= suppressUntilMs_) {
            waitRelease_ = false;
            Serial.println("[vink3][touch] release observed, suppression cleared");
        }
        if (displayPushing) return;
    }

    if (!pressed && wasPressed_) {
        wasPressed_ = false;
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

void InputService::pollPowerButton(uint32_t now) {
    if (!stateMachine_) return;

    const bool pressed = M5.BtnPWR.isPressed();

    // Real-device feedback: using GPIO36 as a PaperS3 power key produced no
    // useful response. Treat M5Unified BtnPWR as the only physical source and
    // make the action visible/immediate: one stable press requests shutdown.
    if (!powerButtonArmed_) {
        if (now > kPowerBootIgnoreMs && !pressed) {
            powerButtonArmed_ = true;
            powerPressStartedMs_ = 0;
            powerWasPressed_ = false;
            powerLongPosted_ = false;
            Serial.println("[vink3][power] BtnPWR armed after boot release");
        }
        return;
    }

    if (pressed) {
        if (!powerWasPressed_) {
            powerWasPressed_ = true;
            powerLongPosted_ = false;
            powerPressStartedMs_ = now;
            Serial.println("[vink3][power] BtnPWR down");
        }
        if (!powerLongPosted_ && powerPressStartedMs_ != 0 && now - powerPressStartedMs_ >= kPowerLongHoldMs) {
            powerLongPosted_ = true;
            powerButtonArmed_ = false;
            Message msg;
            msg.type = MessageType::PowerButton;
            msg.timestampMs = now;
            stateMachine_->post(msg, 0);
            Serial.println("[vink3][power] BtnPWR long hold -> shutdown request");
        }
        return;
    }

    if (powerWasPressed_) {
        const uint32_t held = powerPressStartedMs_ ? now - powerPressStartedMs_ : 0;
        powerWasPressed_ = false;
        powerPressStartedMs_ = 0;
        if (!powerLongPosted_ && held >= kPowerStablePressMs) {
            powerButtonArmed_ = false;
            Message msg;
            msg.type = MessageType::PowerButton;
            msg.timestampMs = now;
            stateMachine_->post(msg, 0);
            Serial.printf("[vink3][power] BtnPWR click held=%lu -> shutdown request\n", static_cast<unsigned long>(held));
        }
        powerLongPosted_ = false;
    }
}

} // namespace vink3
