#include "InputService.h"
#include "../display/DisplayService.h"
#include "../ReadPaper176.h"

namespace vink3 {

InputService g_inputService;

namespace {
constexpr uint32_t kPollDelayMs = 10;
constexpr uint32_t kDebounceMs = 120;
constexpr uint32_t kMoveDiagnosticMs = 100;
constexpr uint32_t kPowerBootIgnoreMs = 3000;
constexpr uint32_t kPowerStablePressMs = 80;
constexpr uint32_t kPowerDoubleClickWindowMs = 650;
constexpr uint32_t kPowerLongHoldMs = 1600;
constexpr uint32_t kLongPressMs = 700;
constexpr int16_t kTapSlopPx = 30;
constexpr int16_t kLongPressMovePx = 34;
constexpr int16_t kSwipeThresholdPx = 80;

const char* touchCoordModeName(TouchCoordMode mode) {
    switch (mode) {
        case TouchCoordMode::Logical540x960: return "logical540x960";
        case TouchCoordMode::PhysicalScale960x540: return "physical-scale";
        case TouchCoordMode::PhysicalRot90: return "physical-rot90";
        case TouchCoordMode::PhysicalRot180: return "physical-rot180";
        case TouchCoordMode::PhysicalRot270: return "physical-rot270";
        default: return "unknown";
    }
}

TouchPoint transformRawPaperS3Point(int rawX, int rawY) {
    // Do not infer the coordinate frame per point: physical 960x540 points in
    // the left/top part of the panel overlap the 540x960 logical range. Once a
    // strong physical-coordinate signal is seen, keep using that mode until boot.
    int x = rawX;
    int y = rawY;
    switch (gPaperS3TouchCoordMode) {
        case TouchCoordMode::PhysicalScale960x540:
            x = (static_cast<int32_t>(rawX) * kPaperS3Width) / kPaperS3PhysicalWidth;
            y = (static_cast<int32_t>(rawY) * kPaperS3Height) / kPaperS3PhysicalHeight;
            break;
        case TouchCoordMode::PhysicalRot90:
            x = rawY;
            y = kPaperS3PhysicalWidth - 1 - rawX;
            break;
        case TouchCoordMode::PhysicalRot180:
            x = kPaperS3Width - 1 - rawY;
            y = kPaperS3PhysicalWidth - 1 - rawX;
            break;
        case TouchCoordMode::PhysicalRot270:
            x = kPaperS3PhysicalHeight - 1 - rawY;
            y = rawX;
            break;
        case TouchCoordMode::Logical540x960:
        default:
            x = rawX;
            y = rawY;
            break;
    }

    // Clamp defensively so invalid release coordinates do not escape into hit-test.
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= kPaperS3Width) x = kPaperS3Width - 1;
    if (y >= kPaperS3Height) y = kPaperS3Height - 1;
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
    pinMode(static_cast<int>(kPowerKeyPin), INPUT_PULLUP);
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

void InputService::updateTouchCoordMode(int rawX, int rawY) {
    if (gPaperS3TouchCoordMode != TouchCoordMode::Logical540x960) return;
    const bool looksPhysicalLandscape = rawX >= kPaperS3Width && rawX < kPaperS3PhysicalWidth && rawY >= 0 && rawY < kPaperS3PhysicalHeight;
    if (!looksPhysicalLandscape) return;

    switch (gPaperS3ActiveDisplayRotation & 0x03) {
        case 1: gPaperS3TouchCoordMode = TouchCoordMode::PhysicalRot90; break;
        case 2: gPaperS3TouchCoordMode = TouchCoordMode::PhysicalRot180; break;
        case 3: gPaperS3TouchCoordMode = TouchCoordMode::PhysicalRot270; break;
        case 0:
        default: gPaperS3TouchCoordMode = TouchCoordMode::PhysicalScale960x540; break;
    }
    Serial.printf("[vink3][touch] detected physical coordinate mode: %s from raw=%d,%d rotation=%u\n",
                  touchCoordModeName(gPaperS3TouchCoordMode), rawX, rawY, gPaperS3ActiveDisplayRotation);
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

    const bool m5Pressed = M5.BtnPWR.isPressed();
    const bool gpioPressed = digitalRead(static_cast<int>(kPowerKeyPin)) == LOW;
    const bool pressed = m5Pressed || gpioPressed;

    // Official PaperS3 docs: single-click side key powers on, double-click powers
    // off. Ignore the boot press until it has been released after startup.
    if (!powerButtonArmed_) {
        if (now > kPowerBootIgnoreMs && !pressed) {
            powerButtonArmed_ = true;
            powerPressStartedMs_ = 0;
            powerWasPressed_ = false;
            powerLongPosted_ = false;
            powerClickCount_ = 0;
            powerFirstClickMs_ = 0;
            Serial.println("[vink3][power] power key armed after boot release");
        }
        return;
    }

    if (powerClickCount_ == 1 && powerFirstClickMs_ != 0 && now - powerFirstClickMs_ > kPowerDoubleClickWindowMs) {
        powerClickCount_ = 0;
        powerFirstClickMs_ = 0;
    }

    if (pressed) {
        if (!powerWasPressed_) {
            powerWasPressed_ = true;
            powerLongPosted_ = false;
            powerPressStartedMs_ = now;
        }
        if (!powerLongPosted_ && powerPressStartedMs_ != 0 && now - powerPressStartedMs_ >= kPowerLongHoldMs) {
            powerLongPosted_ = true;
            powerButtonArmed_ = false;
            powerClickCount_ = 0;
            powerFirstClickMs_ = 0;
            Message msg;
            msg.type = MessageType::PowerButton;
            msg.timestampMs = now;
            stateMachine_->post(msg, 0);
            Serial.println("[vink3][power] long power-key hold -> shutdown request");
        }
        return;
    }

    if (powerWasPressed_) {
        const uint32_t held = powerPressStartedMs_ ? now - powerPressStartedMs_ : 0;
        powerWasPressed_ = false;
        powerPressStartedMs_ = 0;
        if (!powerLongPosted_ && held >= kPowerStablePressMs && held < kPowerLongHoldMs) {
            if (powerClickCount_ == 0 || now - powerFirstClickMs_ > kPowerDoubleClickWindowMs) {
                powerClickCount_ = 1;
                powerFirstClickMs_ = now;
                Serial.println("[vink3][power] first power-key click, waiting for official double-click");
            } else {
                powerClickCount_ = 0;
                powerFirstClickMs_ = 0;
                powerButtonArmed_ = false;
                Message msg;
                msg.type = MessageType::PowerButton;
                msg.timestampMs = now;
                stateMachine_->post(msg, 0);
                Serial.println("[vink3][power] official double-click -> shutdown request");
            }
        }
        powerLongPosted_ = false;
    }
}

} // namespace vink3
