#include "DisplayService.h"
#include <lgfx/v1/platforms/esp32/Panel_EPD.hpp>
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
    const uint8_t turnProfile = prefs.getUChar("turnprof", static_cast<uint8_t>(readerPageTurnProfile_));
    const uint8_t ghostProfile = prefs.getUChar("ghostprof", static_cast<uint8_t>(readerGhostingProfile_));
    // Older RCs exposed numeric cleanup intervals. v0.4.30-rc removes them from
    // UI/WebUI, so ignore stale NVS keys and derive frequency solely from the
    // low/medium/high strategy.
    prefs.end();
    if (raw <= static_cast<uint8_t>(ReaderRefreshStrategy::Clear)) {
        readerRefreshStrategy_ = static_cast<ReaderRefreshStrategy>(raw);
    }
    if (turnProfile <= static_cast<uint8_t>(ReaderPageTurnProfile::Fast)) {
        readerPageTurnProfile_ = static_cast<ReaderPageTurnProfile>(turnProfile);
    }
    if (ghostProfile <= static_cast<uint8_t>(ReaderGhostingProfile::Strong)) {
        readerGhostingProfile_ = static_cast<ReaderGhostingProfile>(ghostProfile);
    }
}

bool DisplayService::saveLocalSettings() const {
    Preferences prefs;
    if (!prefs.begin("vink-display", false)) return false;
    prefs.putUChar("refresh", static_cast<uint8_t>(readerRefreshStrategy_));
    prefs.putUChar("turnprof", static_cast<uint8_t>(readerPageTurnProfile_));
    prefs.putUChar("ghostprof", static_cast<uint8_t>(readerGhostingProfile_));
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

void DisplayService::setReaderPageTurnProfile(ReaderPageTurnProfile profile) {
    readerPageTurnProfile_ = profile;
    resetReaderPageTurnCount();
    saveLocalSettings();
    Serial.printf("[vink3][display] reader page-turn profile -> %s\n", readerPageTurnProfileLabel());
}

void DisplayService::cycleReaderPageTurnProfile() {
    switch (readerPageTurnProfile_) {
        case ReaderPageTurnProfile::Clean:
            setReaderPageTurnProfile(ReaderPageTurnProfile::Balanced);
            break;
        case ReaderPageTurnProfile::Balanced:
            setReaderPageTurnProfile(ReaderPageTurnProfile::Fast);
            break;
        case ReaderPageTurnProfile::Fast:
        default:
            setReaderPageTurnProfile(ReaderPageTurnProfile::Clean);
            break;
    }
}

void DisplayService::setReaderGhostingProfile(ReaderGhostingProfile profile) {
    readerGhostingProfile_ = profile;
    resetReaderPageTurnCount();
    saveLocalSettings();
    Serial.printf("[vink3][display] reader ghosting profile -> %s\n", readerGhostingProfileLabel());
}

void DisplayService::cycleReaderGhostingProfile() {
    switch (readerGhostingProfile_) {
        case ReaderGhostingProfile::Light:
            setReaderGhostingProfile(ReaderGhostingProfile::Balanced);
            break;
        case ReaderGhostingProfile::Balanced:
            setReaderGhostingProfile(ReaderGhostingProfile::Strong);
            break;
        case ReaderGhostingProfile::Strong:
        default:
            setReaderGhostingProfile(ReaderGhostingProfile::Light);
            break;
    }
}

const char* DisplayService::readerRefreshStrategyLabel() const {
    switch (readerRefreshStrategy_) {
        case ReaderRefreshStrategy::Speed: return "低";
        case ReaderRefreshStrategy::Balanced: return "中";
        case ReaderRefreshStrategy::Clear: return "高";
    }
    return "中";
}

const char* DisplayService::readerPageTurnProfileLabel() const {
    switch (readerPageTurnProfile_) {
        case ReaderPageTurnProfile::Clean:    return "清晰";
        case ReaderPageTurnProfile::Balanced: return "均衡";
        case ReaderPageTurnProfile::Fast:     return "快速";
    }
    return "清晰";
}

const char* DisplayService::readerGhostingProfileLabel() const {
    switch (readerGhostingProfile_) {
        case ReaderGhostingProfile::Light:    return "轻";
        case ReaderGhostingProfile::Balanced: return "均衡";
        case ReaderGhostingProfile::Strong:   return "强";
    }
    return "均衡";
}

// Native IT8951 page-turn sweep. Keep this isolated in DisplayService so it
// does not touch boot/runtime initialization.
//
// Do not draw software refresh bars: each strip pushes real new page pixels and
// asks the EPD controller to refresh that region with a cleaner text waveform.
// The visual direction is centralized here so it can be flipped after real-device
// validation without changing tap/swipe handlers.

// EDCBook formula: n = clamp(effectSteps/2, 1, 24), 16px-aligned strips
static inline int clampInt(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static int buildScrollOffsets(int* off, int w, int eSteps) {
    int n = clampInt(eSteps / 2, 1, 24);
    int step = (w + n * 16 - 1) / (n * 16) * 16;
    int an = (w + step - 1) / step;
    if (an < 1) an = 1;
    step = (w + an - 1) / an;
    for (int i = 0; i < an; i++) off[i] = i * step;
    off[an] = w;
    return an;
}
static int effectStepsForStrategy(ReaderRefreshStrategy s) {
    switch (s) {
        case ReaderRefreshStrategy::Speed:  return 12;
        case ReaderRefreshStrategy::Clear:  return 48;
        default:                            return 24;
    }
}

uint8_t DisplayService::pageTurnCompensationLevel() const {
    // One burn should cover the main single-page ghosting experiments. The
    // driver uses this value to select old/new-aware private page-turn LUTs
    // per changed pixel instead of waiting for periodic full refresh.
    switch (readerGhostingProfile_) {
        case ReaderGhostingProfile::Light:    return 0;
        case ReaderGhostingProfile::Balanced: return 1;
        case ReaderGhostingProfile::Strong:   return 2;
    }
    return 1;
}

uint16_t DisplayService::pageTurnScrollStripWidth() const {
    // Fallback strip width if the driver cannot use the EDC-style offset table.
    // the old high-speed DU-like mode is intentionally not used for animation.
    switch (readerPageTurnProfile_) {
        case ReaderPageTurnProfile::Balanced: return 108;
        case ReaderPageTurnProfile::Fast:     return 180;
        case ReaderPageTurnProfile::Clean:
        default:                              return 64;
    }
}

uint8_t DisplayService::pageTurnScrollEffectSteps() const {
    // EDCBook-like offset table seed: n = clamp(effectSteps / 2, 1, 24),
    // step = ceil(width / (n * 16)) * 16. These values map the existing runtime
    // page-turn profiles to band counts while keeping one-burn tunability.
    switch (readerPageTurnProfile_) {
        case ReaderPageTurnProfile::Fast:     return 12; // ~6 strips @ 540px
        case ReaderPageTurnProfile::Balanced: return 24; // ~12 strips
        case ReaderPageTurnProfile::Clean:
        default:                              return 48; // ~17 strips, 16px aligned
    }
}

static epd_mode_t pageTurnScrollMode(epd_mode_t scheduledMode) {
    // Stable baseline: displayScroll reveals strips but lets Panel_EPD use the
    // normal text/quality LUTs. The previous private short page-turn LUTs caused
    // rough, uneven ink even with AA disabled, so compensation experiments must
    // be reintroduced separately only after this baseline is clean.
    if (scheduledMode == kQualityRefresh) return kQualityRefresh;
    return kNormalRefresh;
}

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

void DisplayService::pushSweepBandsEffect(M5Canvas* canvas, DisplayEffect effect, epd_mode_t mode) {
    if (!canvas) return;
    // PaperS3 in the current M5Unified/M5GFX stack uses lgfx::Panel_EPD, not
    // lgfx::Panel_EPDiy. Do not reinterpret the active panel as Panel_EPDiy or
    // read a fake epd_hl pointer; that violates the driver contract and can reset
    // the device as soon as page-turn animation runs. Use the official Panel_EPD
    // path: draw the rendered next-page snapshot into the M5GFX framebuffer, then
    // request real EPD waveform updates for vertical strips.
    M5DisplayStripSweep(canvas, effect, mode);
}

void DisplayService::M5DisplayStripSweep(M5Canvas* canvas, DisplayEffect effect, epd_mode_t mode) {
    if (!canvas) return;
    M5.Display.waitDisplay();
    M5.Display.setColorDepth(kTextColorDepthHigh);

    // EDCBook-style migration for PaperS3's actual Panel_EPD driver. First copy
    // the fully-rendered next page into Panel_EPD's framebuffer without starting
    // a display transfer, then ask the patched driver to reveal logical portrait
    // strips from inside its EPD worker. This moves the strip scheduler below
    // pushSprite/setClipRect and into the waveform/scan-cycle layer.
    auto* panel = static_cast<lgfx::Panel_EPD*>(M5.Display.panel());
    if (!panel) return;
    const epd_mode_t sweepMode = pageTurnScrollMode(mode);
    M5.Display.setEpdMode(sweepMode);

    const bool savedAutoDisplay = M5.Display.getPanel()->getAutoDisplay();
    M5.Display.setAutoDisplay(false);
    canvas->pushSprite(&M5.Display, 0, 0);
    M5.Display.setAutoDisplay(savedAutoDisplay);

    const bool rtl = (effect == DisplayEffect::VerticalShutter);
    const uint16_t stripWidth = pageTurnScrollStripWidth();
    const uint8_t effectSteps = pageTurnScrollEffectSteps();
    const uint8_t compensation = 0;  // disabled while using the stable standard-LUT path
    Serial.printf("[vink3][display] page-turn scroll profile=%s ghost=disabled-standard-lut strip=%u effectSteps=%u comp=%u\n",
                  readerPageTurnProfileLabel(), stripWidth, effectSteps, compensation);
    panel->displayScroll(0, 0, kPaperS3Width, kPaperS3Height, stripWidth, rtl, compensation, effectSteps);
    M5.Display.waitDisplay();
}

epd_mode_t DisplayService::chooseReaderRefreshMode(const DisplayRequest& request) {
    // ReaderRefreshStrategy is now the full-clean frequency setting, decoupled
    // from page-turn animation speed. Animation always uses the clean line-sweep
    // path; this only decides how often a stronger quality waveform is scheduled.
    uint32_t fullEvery = 10;
    constexpr epd_mode_t normalMode = kNormalRefresh;
    switch (readerRefreshStrategy_) {
        case ReaderRefreshStrategy::Speed:    // 全刷频率：低
            fullEvery = 20;
            break;
        case ReaderRefreshStrategy::Clear:    // 全刷频率：高
            fullEvery = 5;
            break;
        case ReaderRefreshStrategy::Balanced: // 全刷频率：中
        default:
            fullEvery = 10;
            break;
    }

    const uint32_t nextTurn = readerPageTurnCount_ + 1;
    const bool useQualityMode = request.quality || (fullEvery > 0 && nextTurn >= fullEvery);
    if (useQualityMode) {
        readerPageTurnCount_ = 0;
        M5.Display.setColorDepth(kTextColorDepthHigh);
        return kQualityRefresh;
    }

    readerPageTurnCount_ = nextTurn;
    M5.Display.setColorDepth(kTextColorDepthHigh);
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
        } else if (request.effect == DisplayEffect::VerticalShutter || request.effect == DisplayEffect::HorizontalShutter) {
            pushSweepBandsEffect(canvasToPush, request.effect, readerMode);
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
