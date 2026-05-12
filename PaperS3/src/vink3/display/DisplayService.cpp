#include "DisplayService.h"
#include <algorithm>
#include <cstring>
#include <Preferences.h>

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
        queue_ = xQueueCreate(queueLen, sizeof(DisplayRequest));
        if (!queue_) {
            Serial.println("[vink3][display] begin failed: display queue create failed");
            return false;
        }
    }
    if (!canvasQueue_) {
        canvasQueue_ = xQueueCreate(queueLen, sizeof(M5Canvas*));
        if (!canvasQueue_) {
            Serial.println("[vink3][display] begin failed: canvas queue create failed");
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
    loadLocalSettings();
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
    if (!enqueueCanvasCloneBlocking(clone)) {
        delete clone;
        Serial.println("[vink3][display] enqueue skipped: canvas snapshot queue failed");
        return false;
    }

    if (xQueueSend(queue_, &request, pdMS_TO_TICKS(timeoutMs)) != pdTRUE) {
        // Remove the clone we just queued if possible; otherwise the display task
        // owns it and will delete it when it pops the next request.
        M5Canvas* discarded = dequeueCanvasClone();
        if (discarded) delete discarded;
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

bool DisplayService::enqueueReaderPageTurn(DisplayEffect effect, uint32_t timeoutMs) {
    DisplayRequest request;
    request.readerPageTurn = true;
    request.quality = forceNextReaderFullRefresh_;
    request.effect = effect;
    request.x = 0;
    request.y = 0;
    request.w = kPaperS3Width;
    request.h = kPaperS3Height;
    const bool ok = enqueue(request, timeoutMs);
    if (ok && request.quality) forceNextReaderFullRefresh_ = false;
    return ok;
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
    const bool canvasQueued = canvasQueue_ && uxQueueMessagesWaiting(canvasQueue_) > 0;
    return busy_ || queued || canvasQueued;
}

uint32_t DisplayService::pushCount() const {
    return pushCount_;
}

void DisplayService::resetPushCount() {
    pushCount_ = 0;
}

uint32_t DisplayService::readerPageTurnCount() const {
    return readerPageTurnCount_;
}

void DisplayService::resetReaderPageTurnCount() {
    readerPageTurnCount_ = 0;
    forceNextReaderFullRefresh_ = false;
}

void DisplayService::forceNextReaderFullRefresh() {
    forceNextReaderFullRefresh_ = true;
}

void DisplayService::taskThunk(void* arg) {
    static_cast<DisplayService*>(arg)->taskLoop();
}

void DisplayService::taskLoop() {
    DisplayRequest request;
    for (;;) {
        if (xQueueReceive(queue_, &request, portMAX_DELAY) == pdTRUE) {
            M5Canvas* canvasToPush = dequeueCanvasClone();
            if (!canvasToPush) {
                Serial.println("[vink3][display] dropped request: missing immutable canvas snapshot");
                continue;
            }
            push(request, canvasToPush);
            delete canvasToPush;
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

bool DisplayService::enqueueCanvasCloneBlocking(M5Canvas* clone) {
    if (!canvasQueue_ || !clone) return false;
    return xQueueSend(canvasQueue_, &clone, portMAX_DELAY) == pdTRUE;
}

M5Canvas* DisplayService::dequeueCanvasClone() {
    if (!canvasQueue_) return nullptr;
    M5Canvas* clone = nullptr;
    if (xQueueReceive(canvasQueue_, &clone, 0) == pdTRUE) return clone;
    return nullptr;
}

void DisplayService::loadLocalSettings() {
    Preferences prefs;
    if (!prefs.begin("vink-display", true)) return;
    const uint8_t raw = prefs.getUChar("refresh", static_cast<uint8_t>(readerRefreshStrategy_));
    readerMiddleRefreshEvery_ = prefs.getUChar("midEvery", readerMiddleRefreshEvery_);
    readerFullRefreshEvery_ = prefs.getUChar("fullEvery", readerFullRefreshEvery_);
    prefs.end();
    if (raw <= static_cast<uint8_t>(ReaderRefreshStrategy::Clear)) {
        readerRefreshStrategy_ = static_cast<ReaderRefreshStrategy>(raw);
    }
}

bool DisplayService::saveLocalSettings() const {
    Preferences prefs;
    if (!prefs.begin("vink-display", false)) return false;
    prefs.putUChar("refresh", static_cast<uint8_t>(readerRefreshStrategy_));
    prefs.putUChar("midEvery", readerMiddleRefreshEvery_);
    prefs.putUChar("fullEvery", readerFullRefreshEvery_);
    prefs.end();
    return true;
}

void DisplayService::cycleReaderRefreshStrategy() {
    switch (readerRefreshStrategy_) {
        case ReaderRefreshStrategy::Speed:
            setReaderRefreshStrategy(ReaderRefreshStrategy::Balanced);
            break;
        case ReaderRefreshStrategy::Balanced:
            setReaderRefreshStrategy(ReaderRefreshStrategy::Clear);
            break;
        case ReaderRefreshStrategy::Clear:
        default:
            setReaderRefreshStrategy(ReaderRefreshStrategy::Speed);
            break;
    }
}

void DisplayService::setReaderRefreshStrategy(ReaderRefreshStrategy strategy) {
    readerRefreshStrategy_ = strategy;
    resetPushCount();
    resetReaderPageTurnCount();
    saveLocalSettings();
    Serial.printf("[vink3][display] reader refresh strategy -> %s\n", readerRefreshStrategyLabel());
}

void DisplayService::setReaderRefreshIntervals(uint8_t middleEvery, uint8_t fullEvery) {
    readerMiddleRefreshEvery_ = middleEvery;
    readerFullRefreshEvery_ = fullEvery;
    resetReaderPageTurnCount();
    saveLocalSettings();
    Serial.printf("[vink3][display] reader refresh intervals -> middle=%u full=%u\n", readerMiddleRefreshEvery_, readerFullRefreshEvery_);
}

const char* DisplayService::readerRefreshStrategyLabel() const {
    switch (readerRefreshStrategy_) {
        case ReaderRefreshStrategy::Speed: return "高速";
        case ReaderRefreshStrategy::Balanced: return "标准";
        case ReaderRefreshStrategy::Clear: return "清晰";
    }
    return "标准";
}

// Native IT8951 page-turn sweep. Keep this isolated in DisplayService so it
// does not touch boot/runtime initialization.
//
// Do not draw software refresh bars: each strip pushes real new page pixels and
// asks the EPD controller to refresh that region with a cleaner text waveform.
// The visual direction is centralized here so it can be flipped after real-device
// validation without changing tap/swipe handlers.
void DisplayService::pushShutterAnimation(M5Canvas* canvas, DisplayEffect effect, epd_mode_t mode) {
    if (!canvas) return;

    const uint8_t savedRotation = M5.Display.getRotation();
    M5.Display.setRotation(0);

    M5.Display.waitDisplay();
    M5.Display.setColorDepth(kTextColorDepthHigh);
    if (mode == kLowRefresh || mode == epd_mode_t::epd_fast || mode == epd_mode_t::epd_fastest) {
        mode = kNormalRefresh;
    }
    M5.Display.setEpdMode(mode);

    // Native sweep contract:
    // - next page  / VerticalShutter   -> right-to-left strip refresh
    // - prev page  / HorizontalShutter -> left-to-right strip refresh
    // If real PaperS3 visual direction feels reversed, flip only this mapping.
    constexpr int16_t kSweepStripW = 60;
    const bool rightToLeft = (effect == DisplayEffect::VerticalShutter);
    if (rightToLeft) {
        for (int16_t sx = kPaperS3Width; sx > 0; sx -= kSweepStripW) {
            const int16_t x = max<int16_t>(0, sx - kSweepStripW);
            const int16_t w = sx - x;
            M5.Display.setClipRect(x, 0, w, kPaperS3Height);
            canvas->pushSprite(&M5.Display, 0, 0);
            M5.Display.waitDisplay();
        }
    } else {
        for (int16_t x = 0; x < kPaperS3Width; x += kSweepStripW) {
            const int16_t w = min<int16_t>(kSweepStripW, kPaperS3Width - x);
            M5.Display.setClipRect(x, 0, w, kPaperS3Height);
            canvas->pushSprite(&M5.Display, 0, 0);
            M5.Display.waitDisplay();
        }
    }
    M5.Display.clearClipRect();
    M5.Display.setRotation(savedRotation);
}

epd_mode_t DisplayService::chooseReaderRefreshMode(const DisplayRequest& request) {
    uint32_t fullEvery = 10;
    epd_mode_t normalMode = kNormalRefresh;
    switch (readerRefreshStrategy_) {
        case ReaderRefreshStrategy::Speed:
            fullEvery = 0;
            normalMode = kLowRefresh;
            break;
        case ReaderRefreshStrategy::Clear:
            fullEvery = 20;
            normalMode = kNormalRefresh;
            break;
        case ReaderRefreshStrategy::Balanced:
        default:
            fullEvery = 0;
            normalMode = kNormalRefresh;
            break;
    }

    if (readerFullRefreshEvery_ > 0) fullEvery = readerFullRefreshEvery_;

    const uint32_t nextTurn = readerPageTurnCount_ + 1;
    const bool useQualityMode = request.quality || (fullEvery > 0 && nextTurn >= fullEvery);
    if (useQualityMode) {
        readerPageTurnCount_ = 0;
        M5.Display.setColorDepth(kTextColorDepthHigh);
        return kQualityRefresh;
    }

    readerPageTurnCount_ = nextTurn;
    M5.Display.setColorDepth(kTextColorDepthHigh);
    if (readerMiddleRefreshEvery_ > 0 && (nextTurn % readerMiddleRefreshEvery_) == 0) {
        return kNormalRefresh;
    }
    return normalMode;
}

epd_mode_t DisplayService::chooseRefreshMode(const DisplayRequest& request) {
    if (request.readerPageTurn) return chooseReaderRefreshMode(request);

    const bool useQualityMode = request.quality ||
        (fastRefresh_ && pushCount_ >= kDisplayQualityFastThreshold) ||
        (!fastRefresh_ && pushCount_ >= kDisplayFullRefreshNormalThreshold);

    if (useQualityMode) {
        pushCount_ = 0;
        M5.Display.setColorDepth(kTextColorDepthHigh);
        return kQualityRefresh;
    }

    M5.Display.setColorDepth(kTextColorDepthHigh);
    return kNormalRefresh;
}

void DisplayService::push(const DisplayRequest& request, M5Canvas* canvasToPush) {
    if (!canvasToPush) return;

    busy_ = true;
    g_inDisplayPush = true;

    M5.Display.waitDisplay();

    if (request.readerPageTurn && request.effect != DisplayEffect::None) {
        const epd_mode_t readerMode = chooseReaderRefreshMode(request);
        if (readerMode == kQualityRefresh) {
            M5.Display.setColorDepth(kTextColorDepthHigh);
            M5.Display.setEpdMode(readerMode);
            canvasToPush->pushSprite(&M5.Display, 0, 0);
            M5.Display.waitDisplay();
        } else {
            pushShutterAnimation(canvasToPush, request.effect, readerMode);
        }
        pushCount_++;
        g_inDisplayPush = false;
        busy_ = false;
        return;
    }

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
