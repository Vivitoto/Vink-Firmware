#include "InputService.h"
#include "../display/DisplayService.h"
#include "../ReadPaper176.h"

namespace vink3 {

InputService g_inputService;

namespace {
constexpr uint32_t kPollDelayMs = 10;
constexpr uint32_t kDebounceMs = 120;
constexpr int16_t kSwipeThresholdPx = 80;

TouchPoint transformRawPaperS3Point(int rawX, int rawY) {
    // M5Unified normally returns rotation-aware display coordinates. If a future
    // M5GFX/M5Unified combination leaks physical 960x540 GT911 coordinates, use
    // the active display rotation selected at boot to map them back to Vink's
    // 540x960 portrait hit-test space.
    if (rawX >= 0 && rawX < kPaperS3Width && rawY >= 0 && rawY < kPaperS3Height) {
        return TouchPoint(static_cast<int16_t>(rawX), static_cast<int16_t>(rawY));
    }

    int x = rawX;
    int y = rawY;
    if (rawX >= 0 && rawX < kPaperS3PhysicalWidth && rawY >= 0 && rawY < kPaperS3PhysicalHeight) {
        switch (gPaperS3ActiveDisplayRotation & 0x03) {
            case 1:
                x = rawY;
                y = kPaperS3PhysicalWidth - 1 - rawX;
                break;
            case 3:
                x = kPaperS3PhysicalHeight - 1 - rawY;
                y = rawX;
                break;
            case 2:
                x = kPaperS3Width - 1 - rawY;
                y = kPaperS3PhysicalWidth - 1 - rawX;
                break;
            case 0:
            default:
                x = (static_cast<int32_t>(rawX) * kPaperS3Width) / kPaperS3PhysicalWidth;
                y = (static_cast<int32_t>(rawY) * kPaperS3Height) / kPaperS3PhysicalHeight;
                break;
        }
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
    Serial.println("[vink3][input] service started");
    return true;
}

void InputService::taskThunk(void* arg) {
    static_cast<InputService*>(arg)->taskLoop();
}

void InputService::taskLoop() {
    for (;;) {
        M5.update();
        pollTouch();
        vTaskDelay(pdMS_TO_TICKS(kPollDelayMs));
    }
}

void InputService::pollTouch() {
    if (!stateMachine_ || g_inDisplayPush) return;

    auto detail = M5.Touch.getDetail();
    const bool pressed = detail.isPressed();
    const uint32_t now = millis();

    if (pressed) {
        const TouchPoint rawPoint(static_cast<int16_t>(detail.x), static_cast<int16_t>(detail.y));
        const TouchPoint currentPoint = normalizeTouchPoint(detail.x, detail.y);
        if (!wasPressed_) {
            if (now - lastEventMs_ < kDebounceMs) return;
            wasPressed_ = true;
            pressStartedMs_ = now;
            pressPoint_ = currentPoint;
            lastPoint_ = currentPoint;
            pressRawPoint_ = rawPoint;
            lastRawPoint_ = rawPoint;
            Serial.printf("[vink3][touch] down raw=%d,%d norm=%d,%d count=%d\n",
                          rawPoint.x, rawPoint.y, currentPoint.x, currentPoint.y, M5.Touch.getCount());
            Message msg;
            msg.type = MessageType::TouchDown;
            msg.timestampMs = now;
            msg.touch = pressPoint_;
            msg.rawTouch = pressRawPoint_;
            stateMachine_->post(msg);
            return;
        }
        // Cache the last valid pressed coordinate. Some touch drivers report
        // invalid/zero coordinates after release; using release-time getDetail()
        // for Tap made PaperS3 appear unresponsive.
        lastPoint_ = currentPoint;
        lastRawPoint_ = rawPoint;
        return;
    }

    if (!pressed && wasPressed_) {
        wasPressed_ = false;
        lastEventMs_ = now;
        const TouchPoint releasePoint = lastPoint_;
        const TouchPoint releaseRawPoint = lastRawPoint_;
        const int16_t dx = releasePoint.x - pressPoint_.x;
        const int16_t dy = releasePoint.y - pressPoint_.y;
        Serial.printf("[vink3][touch] up raw=%d,%d norm=%d,%d dx=%d dy=%d held=%lu\n",
                      releaseRawPoint.x, releaseRawPoint.y, releasePoint.x, releasePoint.y,
                      dx, dy, static_cast<unsigned long>(now - pressStartedMs_));

        Message up;
        up.type = MessageType::TouchUp;
        up.timestampMs = now;
        up.touch = releasePoint;
        up.rawTouch = releaseRawPoint;
        stateMachine_->post(up);

        Message semantic;
        semantic.timestampMs = now;
        semantic.touch = releasePoint;
        semantic.rawTouch = releaseRawPoint;
        if (abs(dx) > abs(dy) && abs(dx) >= kSwipeThresholdPx) {
            semantic.type = dx > 0 ? MessageType::SwipeRight : MessageType::SwipeLeft;
        } else if (abs(dy) >= kSwipeThresholdPx) {
            semantic.type = dy > 0 ? MessageType::SwipeDown : MessageType::SwipeUp;
        } else if (now - pressStartedMs_ > 700) {
            semantic.type = MessageType::LongPress;
        } else {
            semantic.type = MessageType::Tap;
        }
        stateMachine_->post(semantic);
    }
}

} // namespace vink3
