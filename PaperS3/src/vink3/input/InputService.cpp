#include "InputService.h"
#include "../display/DisplayService.h"
#include "../system/SystemLog.h"
#include "../ReadPaper176.h"

namespace vink3 {

InputService g_inputService;

namespace {
constexpr uint32_t kPollDelayMs = 10;
constexpr uint32_t kDebounceMs = 35;
constexpr uint32_t kMoveDiagnosticMs = 100;
constexpr uint32_t kPowerBootIgnoreMs = 1200;
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
    // PaperS3's side key is primarily hardware-managed (single click powers on,
    // double-click powers off, long press enters download mode). On real hardware
    // the short press can be consumed by the PMIC/reset path before a release
    // event is usable, so Vink triggers graceful shutdown on the press edge, not
    // after release, matching the on-screen shutdown button as closely as possible.
    M5.BtnPWR.setDebounceThresh(0);
    M5.BtnPWR.setHoldThresh(0);

    // PaperS3's side key is connected to GPIO36. M5Unified's button layer
    // does not configure it for this board (pmic_adc mode), so we read the
    // pin directly. When the side key is pressed the power circuit may cut
    // power after a short delay — we try to beat that window.
    pinMode(36, INPUT);  // no pullup/pulldown — let the power latch work normally

    Serial.println("[vink3][input] service started; side-key on GPIO36");
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
        pollSideKey(now);
        pollTouch();
        vTaskDelay(pdMS_TO_TICKS(kPollDelayMs));
    }
}

void InputService::suppressFor(uint32_t cooldownMs) {
    suppressUntilMs_ = millis() + cooldownMs;
    waitRelease_ = false;
    wasPressed_ = false;
    lastMovePostMs_ = 0;
    Serial.printf("[vink3][touch] suppress for %lu ms\n", static_cast<unsigned long>(cooldownMs));
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

void InputService::pollPowerButton(uint32_t now) {
    if (!stateMachine_) return;

    const bool pressed = M5.BtnPWR.isPressed();
    if (!powerArmed_) {
        if (now > kPowerBootIgnoreMs && !pressed) {
            powerArmed_ = true;
            powerWasPressed_ = false;
            powerPressStartedMs_ = 0;
            lastPowerClickMs_ = 0;
            Serial.println("[vink3][power] BtnPWR single-click detector armed");
        }
        return;
    }

    if (pressed && !powerWasPressed_) {
        powerWasPressed_ = true;
        powerPressStartedMs_ = now;
        powerArmed_ = false;
        Message msg;
        msg.type = MessageType::PowerButton;
        msg.timestampMs = now;
        stateMachine_->post(msg, 0);
        Serial.println("[vink3][power] BtnPWR press edge -> graceful shutdown");
        g_systemLog.append("BtnPWR press edge -> shutdown");
        return;
    }

    if (!pressed && powerWasPressed_) {
        powerWasPressed_ = false;
        powerPressStartedMs_ = 0;
    }
}

void InputService::pollSideKey(uint32_t now) {
    if (!stateMachine_) return;

    // GPIO36 reads LOW when the side key is pressed (connected to power latch).
    // No pull-up: when floating it may read either way, so we arm after boot
    // regardless and detect the HIGH→LOW transition on press.
    const bool low = digitalRead(36) == LOW;

    if (!sideKeyArmed_) {
        if (now > kPowerBootIgnoreMs) {
            sideKeyArmed_ = true;
            sideKeyWasLow_ = low;
            Serial.println("[vink3][power] side-key GPIO36 detector armed");
            g_systemLog.append("side-key GPIO36 armed");
        }
        return;
    }

    if (low && !sideKeyWasLow_) {
        sideKeyWasLow_ = true;
        sideKeyArmed_ = false;
        Serial.println("[vink3][power] side-key GPIO36 LOW -> graceful shutdown");
        g_systemLog.append("side-key GPIO36 -> shutdown");
        Message msg;
        msg.type = MessageType::PowerButton;
        msg.timestampMs = now;
        stateMachine_->post(msg, 0);
        return;
    }

    sideKeyWasLow_ = low;
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
                // Do not force a release after every EPD push. Rapid reading taps
                // often land while the previous page is still refreshing; requiring
                // release here makes the second tap disappear and feels laggy.
                // Keep edge state clean, then let a still-held press become a normal
                // down event as soon as the panel is no longer busy.
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


} // namespace vink3
