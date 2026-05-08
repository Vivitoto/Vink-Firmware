#include "DisplayService.h"
#include "../config/ConfigService.h"
#include <cstring>
#include <vector>
#include <algorithm>
#include <cstdlib>

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
    Serial.println("[vink3][display] service started on official M5.Display path");
    return true;
}

bool DisplayService::enqueue(const DisplayRequest& request, uint32_t timeoutMs) {
    if (!queue_) return false;

    // Match Vink reference core: render side snapshots the canvas before the display
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
    if (ok && request.quality) {
        forceNextReaderFullRefresh_ = false;
    }
    return ok;
}

bool DisplayService::enqueueReaderPageTurn(uint32_t timeoutMs) {
    return enqueueReaderPageTurn(DisplayEffect::None, timeoutMs);
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

void DisplayService::markReaderChapterTransition() {
    forceNextReaderFullRefresh_ = true;
}

void DisplayService::forceNextReaderFullRefresh() {
    markReaderChapterTransition();
}

void DisplayService::setReaderPageTurnDirection(bool forward) {
    lastPageTurnForward_ = forward;
}

// Vertical-wipe page-turn animation: a vertical update boundary sweeps across
// the page. This is a clean-room approximation of the EDCBook epdiy path found
// in the v2.0.0 binary: it caps the progression table to 24 steps and aligns
// strip widths to 16 pixels. The important detail is directional *vertical*
// update order, not horizontal bands.
// HorizontalShutter: next page / forward  -> left-to-right wipe.
// VerticalShutter:   prev page / backward -> right-to-left wipe.
void DisplayService::pushShutterAnimation(M5Canvas* canvas, DisplayEffect effect) {
    if (!canvas) return;

    constexpr uint16_t kMaxProgressionSteps = 24;
    constexpr uint16_t kAlignPx = 16;
    constexpr uint32_t kStripDelayMs = 10;
    constexpr epd_mode_t kShutterMode = epd_mode_t::epd_fast;

    const uint16_t width = canvas->width();
    const uint16_t height = canvas->height();
    if (width == 0 || height == 0) return;

    const uint16_t alignedStep = std::max<uint16_t>(
        kAlignPx,
        ((width + (kMaxProgressionSteps * kAlignPx) - 1) / (kMaxProgressionSteps * kAlignPx)) * kAlignPx);
    const uint16_t maxStripW = alignedStep;

    M5Canvas strip(&M5.Display);
    strip.setPsram(true);
    strip.setColorDepth(canvas->getColorDepth());
    if (!strip.createSprite(maxStripW, height)) {
        // If PSRAM is tight, fall back to a normal single push rather than
        // showing a half-updated page.
        M5.Display.setColorDepth(kTextColorDepthHigh);
        M5.Display.setEpdMode(kNormalRefresh);
        canvas->pushSprite(&M5.Display, 0, 0);
        M5.Display.waitDisplay();
        return;
    }

    const uint16_t bpp = static_cast<uint16_t>(canvas->getColorDepth()) & static_cast<uint16_t>(lgfx::bit_mask);
    const bool byteAligned = (bpp == 8 || bpp == 16 || bpp == 24 || bpp == 32);
    const size_t srcRowBytes = height ? canvas->bufferLength() / height : 0;
    const size_t dstRowBytes = height ? strip.bufferLength() / height : 0;
    const uint8_t* srcBase = reinterpret_cast<const uint8_t*>(canvas->getBuffer());
    uint8_t* dstBase = reinterpret_cast<uint8_t*>(strip.getBuffer());

    auto copyStrip = [&](uint16_t x, uint16_t w) {
        strip.clear();
        if (byteAligned && srcBase && dstBase && srcRowBytes > 0 && dstRowBytes > 0) {
            const size_t bytesPerPixel = bpp / 8;
            const size_t copyBytes = static_cast<size_t>(w) * bytesPerPixel;
            const size_t srcXBytes = static_cast<size_t>(x) * bytesPerPixel;
            for (uint16_t y = 0; y < height; y++) {
                memcpy(dstBase + static_cast<size_t>(y) * dstRowBytes,
                       srcBase + static_cast<size_t>(y) * srcRowBytes + srcXBytes,
                       copyBytes);
            }
        } else {
            // Safe generic fallback for palette/packed formats.
            for (uint16_t yy = 0; yy < height; yy++) {
                for (uint16_t xx = 0; xx < w; xx++) {
                    strip.drawPixel(xx, yy, canvas->readPixel(x + xx, yy));
                }
            }
        }
    };

    M5.Display.waitDisplay();
    M5.Display.setColorDepth(kTextColorDepthHigh);
    M5.Display.setEpdMode(kShutterMode);

    if (effect == DisplayEffect::HorizontalShutter) {
        // Forward: left -> right.
        for (uint16_t x = 0; x < width; x += alignedStep) {
            const uint16_t w = std::min<uint16_t>(alignedStep, width - x);
            copyStrip(x, w);
            strip.pushSprite(&M5.Display, x, 0);
            if (x + alignedStep < width) delay(kStripDelayMs);
        }
    } else {
        // Backward: right -> left.
        int32_t x = static_cast<int32_t>(width);
        while (x > 0) {
            const uint16_t w = static_cast<uint16_t>(std::min<int32_t>(alignedStep, x));
            x -= w;
            copyStrip(static_cast<uint16_t>(x), w);
            strip.pushSprite(&M5.Display, x, 0);
            if (x > 0) delay(kStripDelayMs);
        }
    }

    strip.deleteSprite();
    M5.Display.waitDisplay();
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

epd_mode_t DisplayService::chooseRefreshMode(const DisplayRequest& request) {
    M5.Display.setColorDepth(kTextColorDepthHigh);
    if (request.readerPageTurn) return chooseReaderRefreshMode(request);
    return chooseUiRefreshMode(request);
}

epd_mode_t DisplayService::chooseUiRefreshMode(const DisplayRequest& request) {
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
        return kMiddleRefresh;
    }

    // Keep normal UI pushes on the text LUT for crisper menus/settings.
    // quality requests still use epd_quality.
    return kTextRefresh;
}

epd_mode_t DisplayService::chooseReaderRefreshMode(const DisplayRequest& request) {
    const RefreshStrategy strategy = g_configService.refreshStrategy();
    const uint32_t nextTurn = readerPageTurnCount_ + 1;
    const bool useQualityMode = request.quality ||
        (strategy.fullRefreshEvery > 0 && nextTurn >= strategy.fullRefreshEvery);

    if (useQualityMode) {
        readerPageTurnCount_ = 0;
        return kQualityRefresh;
    }

    readerPageTurnCount_ = nextTurn;
    if (strategy.middleRefreshEvery > 0 &&
        nextTurn >= strategy.middleRefreshEvery &&
        nextTurn % strategy.middleRefreshEvery == 0) {
        return kMiddleRefresh;
    }

    return kNormalRefresh;
}

void DisplayService::push(const DisplayRequest& request, M5Canvas* canvasToPush) {
    if (!canvasToPush) return;

    busy_ = true;
    g_inDisplayPush = true;

    M5.Display.waitDisplay();

    // Shutter animation: progressive strip-based push for page-turn visual effect.
    // Still run reader refresh selection first so page-turn counters and periodic
    // quality cleanup behave the same as the non-animated reader path.
    if (request.readerPageTurn && request.effect != DisplayEffect::None) {
        const epd_mode_t readerMode = chooseReaderRefreshMode(request);
        if (readerMode == kQualityRefresh) {
            // Periodic quality cleanup is intentionally a single full update;
            // the animated partial wipe resumes on the next page turn.
            M5.Display.setColorDepth(kTextColorDepthHigh);
            M5.Display.setEpdMode(readerMode);
            canvasToPush->pushSprite(&M5.Display, 0, 0);
            M5.Display.waitDisplay();
        } else {
            pushShutterAnimation(canvasToPush, request.effect);
        }
        pushCount_++;
        g_inDisplayPush = false;
        busy_ = false;
        return;
    }

    // Normal (non-animated) push path
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
