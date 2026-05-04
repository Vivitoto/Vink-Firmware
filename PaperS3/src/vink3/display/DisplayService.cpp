#include "DisplayService.h"
#include <cstring>

namespace vink3 {

DisplayService g_displayService;
volatile bool g_inDisplayPush = false;

bool DisplayService::begin(M5Canvas* canvas, uint8_t queueLen) {
    if (!canvas) {
        Serial.println("[vink3][display] begin failed: canvas is null");
        return false;
    }
    canvas_ = canvas;
    if (!queue_) {
        queue_ = xQueueCreate(queueLen, sizeof(QueuedDisplayRequest));
        if (!queue_) {
            Serial.println("[vink3][display] begin failed: display queue create failed");
            return false;
        }
    }
    if (!task_) {
        BaseType_t ok = xTaskCreatePinnedToCore(
            taskThunk,
            "vink3-display",
            8192,
            this,
            2,
            &task_,
            1);
        if (ok != pdPASS) {
            Serial.println("[vink3][display] begin failed: task create failed");
            task_ = nullptr;
            return false;
        }
    }
    Serial.println("[vink3][display] service started on official M5.Display path");
    return true;
}

bool DisplayService::enqueue(const DisplayRequest& request, uint32_t timeoutMs) {
    if (!queue_) return false;

    // Match ReadPaper 1.7.6: render side snapshots the canvas before the display
    // task performs the physical push. This prevents UI drawing from racing the EPD.
    M5Canvas* clone = cloneCanvas();
    if (!clone) {
        // Never fall back to pushing the live global canvas. Under PSRAM pressure
        // that is safer than racing UI rendering against a physical EPD transfer.
        Serial.println("[vink3][display] enqueue skipped: canvas snapshot allocation failed");
        return false;
    }

    QueuedDisplayRequest queued;
    queued.request = request;
    queued.canvasSnapshot = clone;
    if (xQueueSend(queue_, &queued, pdMS_TO_TICKS(timeoutMs)) != pdTRUE) {
        delete clone;
        Serial.println("[vink3][display] enqueue skipped: display queue full");
        return false;
    }
    return true;
}

bool DisplayService::enqueueFull(bool quality, uint32_t timeoutMs) {
    DisplayRequest request;
    request.quality = quality;
    request.x = 0;
    request.y = 0;
    request.w = kPaperS3Width;
    request.h = kPaperS3Height;
    return enqueue(request, timeoutMs);
}

bool DisplayService::waitIdle(uint32_t timeoutMs) const {
    const uint32_t start = millis();
    while (isBusy()) {
        if (millis() - start >= timeoutMs) return false;
        delay(10);
    }
    M5.Display.waitDisplay();
    return true;
}

bool DisplayService::isBusy() const {
    const bool queued = queue_ && uxQueueMessagesWaiting(queue_) > 0;
    return busy_ || queued;
}

uint32_t DisplayService::pushCount() const {
    return pushCount_;
}

void DisplayService::resetPushCount() {
    pushCount_ = 0;
}

void DisplayService::taskThunk(void* arg) {
    static_cast<DisplayService*>(arg)->taskLoop();
}

void DisplayService::taskLoop() {
    QueuedDisplayRequest queued;
    for (;;) {
        if (xQueueReceive(queue_, &queued, portMAX_DELAY) == pdTRUE) {
            if (!queued.canvasSnapshot) {
                Serial.println("[vink3][display] dropped request: missing immutable canvas snapshot");
                continue;
            }
            push(queued.request, queued.canvasSnapshot);
            delete queued.canvasSnapshot;
            queued.canvasSnapshot = nullptr;
        }
    }
}

M5Canvas* DisplayService::cloneCanvas() const {
    if (!canvas_) return nullptr;
    void* src = canvas_->getBuffer();
    const size_t len = canvas_->bufferLength();
    if (!src || len == 0) return nullptr;

    M5Canvas* clone = new M5Canvas(&M5.Display);
    if (!clone) return nullptr;
    clone->setPsram(true);
    clone->setColorDepth(canvas_->getColorDepth());
    if (!clone->createSprite(canvas_->width(), canvas_->height())) {
        delete clone;
        return nullptr;
    }
    void* dst = clone->getBuffer();
    if (!dst || clone->bufferLength() < len) {
        delete clone;
        return nullptr;
    }
    memcpy(dst, src, len);
    return clone;
}

epd_mode_t DisplayService::chooseRefreshMode(const DisplayRequest& request) {
    const bool needMiddleStep = fastRefresh_ &&
        kDisplayMiddleRefreshThreshold > 0 &&
        pushCount_ >= kDisplayMiddleRefreshThreshold &&
        pushCount_ % kDisplayMiddleRefreshThreshold == 0;
    const bool useQualityMode = request.quality ||
        (fastRefresh_ && pushCount_ >= kDisplayQualityFastThreshold) ||
        (!fastRefresh_ && pushCount_ >= kDisplayFullRefreshNormalThreshold);

    if (useQualityMode) {
        pushCount_ = 0;
        M5.Display.setColorDepth(kTextColorDepthHigh);
        return kQualityRefresh;
    }

    M5.Display.setColorDepth(kTextColorDepthHigh);

    if (needMiddleStep) {
        M5.Display.setEpdMode(kNormalRefresh);
        M5.Display.waitDisplay();
    }

    // A+B+D: use epd_text LUT for normal UI pushes — crisper text rendering.
    // quality requests still use epd_quality.
    return kNormalRefresh;
}

void DisplayService::push(const DisplayRequest& request, M5Canvas* canvasToPush) {
    if (!canvasToPush) return;

    busy_ = true;
    g_inDisplayPush = true;

    M5.Display.waitDisplay();
    M5.Display.setColorDepth(kTextColorDepthHigh);
    M5.Display.setEpdMode(chooseRefreshMode(request));

    const int16_t x = request.x;
    const int16_t y = request.y;
    if (request.transparent) {
        canvasToPush->pushSprite(&M5.Display, x, y, request.invert ? TFT_BLACK : TFT_WHITE);
    } else {
        canvasToPush->pushSprite(&M5.Display, x, y);
    }
    M5.Display.waitDisplay();

    pushCount_++;
    g_inDisplayPush = false;
    busy_ = false;
}

} // namespace vink3
