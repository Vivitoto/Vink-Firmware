#include "DisplayService.h"
#include "EpdiyPaperS3Backend.h"
#include <algorithm>
#include <cstring>
#include <Preferences.h>
#include <lgfx/v1/platforms/esp32/Panel_EPD.hpp>

#ifndef VINK_USE_EPDIY_BACKEND
#define VINK_USE_EPDIY_BACKEND 0
#endif
#ifndef VINK_EPDIY_STRICT
#define VINK_EPDIY_STRICT 0
#endif
#ifndef VINK_ENABLE_M5GFX_SCROLL_PAGE_TURN
// Keep the scroll/page-turn path enabled by default. Real-device feedback on
// v0.4.40 means we should optimize its scheduling and band count, not retreat to
// a plain full-page refresh as the main solution.
#define VINK_ENABLE_M5GFX_SCROLL_PAGE_TURN 1
#endif

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
#if VINK_USE_EPDIY_BACKEND
    Serial.println("[vink3][display] service started with EXPERIMENTAL epdiy architecture backend");
#else
    Serial.println("[vink3][display] service started on official M5.Display path");
#endif
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
#if VINK_USE_EPDIY_BACKEND
    if (!g_epdiyPaperS3Backend.isReady()) {
        M5.Display.waitDisplay();
    }
#else
    M5.Display.waitDisplay();
#endif
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
    return cloneCanvasFrom(canvas_);
}

M5Canvas* DisplayService::cloneCanvasFrom(const M5Canvas* source) const {
    if (!source) return nullptr;
    void* src = source->getBuffer();
    const size_t len = source->bufferLength();
    if (!src || len == 0) return nullptr;

    M5Canvas* clone = new M5Canvas(&M5.Display);
    if (!clone) return nullptr;
    clone->setPsram(true);
    clone->setColorDepth(source->getColorDepth());
    if (!clone->createSprite(source->width(), source->height())) {
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
    const uint8_t turnResidue = prefs.getUChar("turnresid", static_cast<uint8_t>(readerPageTurnResidue_));
    // Older RCs exposed numeric cleanup intervals. The EDCBook-like scroll path
    // keeps full-clean frequency and per-turn residue compensation as separate
    // controls, so a single turn can clean disappeared strokes without forcing a
    // full quality refresh.
    prefs.end();
    if (raw <= static_cast<uint8_t>(ReaderRefreshStrategy::Clear)) {
        readerRefreshStrategy_ = static_cast<ReaderRefreshStrategy>(raw);
    }
    if (turnProfile <= static_cast<uint8_t>(ReaderPageTurnProfile::Fast)) {
        readerPageTurnProfile_ = static_cast<ReaderPageTurnProfile>(turnProfile);
    }
    if (turnResidue <= static_cast<uint8_t>(ReaderPageTurnResidue::Strong)) {
        readerPageTurnResidue_ = static_cast<ReaderPageTurnResidue>(turnResidue);
    }
}

bool DisplayService::saveLocalSettings() const {
    Preferences prefs;
    if (!prefs.begin("vink-display", false)) return false;
    prefs.putUChar("refresh", static_cast<uint8_t>(readerRefreshStrategy_));
    prefs.putUChar("turnprof", static_cast<uint8_t>(readerPageTurnProfile_));
    prefs.putUChar("turnresid", static_cast<uint8_t>(readerPageTurnResidue_));
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

void DisplayService::setReaderPageTurnResidue(ReaderPageTurnResidue residue) {
    readerPageTurnResidue_ = residue;
    resetReaderPageTurnCount();
    saveLocalSettings();
    Serial.printf("[vink3][display] reader page-turn residue -> %s\n", readerPageTurnResidueLabel());
}

void DisplayService::cycleReaderPageTurnResidue() {
    switch (readerPageTurnResidue_) {
        case ReaderPageTurnResidue::Light:
            setReaderPageTurnResidue(ReaderPageTurnResidue::Balanced);
            break;
        case ReaderPageTurnResidue::Balanced:
            setReaderPageTurnResidue(ReaderPageTurnResidue::Strong);
            break;
        case ReaderPageTurnResidue::Strong:
        default:
            setReaderPageTurnResidue(ReaderPageTurnResidue::Light);
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

const char* DisplayService::readerPageTurnResidueLabel() const {
    switch (readerPageTurnResidue_) {
        case ReaderPageTurnResidue::Light:    return "轻";
        case ReaderPageTurnResidue::Balanced: return "中";
        case ReaderPageTurnResidue::Strong:   return "强";
    }
    return "中";
}

// EDCBook / M5ReadPaper page-turn baseline.  The old public-strip fallback was
// M5DisplayStripSweep(canvas, effect, mode); v0.4.44 keeps that as historical
// context but moves the visible path to a full-frame page compositor.
// The actual PaperS3 panel driver is Panel_EPD, not Panel_EPDiy.  The reverse notes show EDCBook's real
// implementation lives below the app layer in an epdiy scroll renderer; on
// PaperS3 the closest honest equivalent is: stage the immutable full next-page
// framebuffer with auto-display disabled, then ask the patched Panel_EPD
// scan-cycle worker to reveal logical portrait strips.
static inline int clampInt(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static uint8_t buildEdcBookOffsets(uint16_t* offsets, uint16_t width, uint8_t effectSteps) {
    const int n = clampInt(effectSteps / 2, 1, 24);
    const int alignedStep = ((width + n * 16 - 1) / (n * 16)) * 16;
    uint8_t count = 0;
    offsets[count++] = 0;
    for (int x = alignedStep; x < width && count < 24; x += alignedStep) {
        offsets[count++] = static_cast<uint16_t>(x);
    }
    offsets[count++] = width;
    return count;
}

static epd_mode_t pageTurnScrollMode(epd_mode_t scheduledMode) {
    return scheduledMode == kQualityRefresh ? kQualityRefresh : scheduledMode;
}

uint16_t DisplayService::pageTurnScrollStripWidth() const {
    // Compatibility label for the smoke invariant: the EDCBook-derived path no
    // longer uses one fixed strip width, and the old high-speed DU-like mode is intentionally not used.
    // Its default first band is still a narrow 16px-aligned portrait sweep.
    uint16_t offsets[25] = {0};
    const uint8_t n = buildEdcBookOffsets(offsets, kPaperS3Width, pageTurnBandSeed());
    return n > 1 ? offsets[1] - offsets[0] : kPaperS3Width;
}

uint8_t DisplayService::pageTurnBandSeed() const {
    // Same seed values recovered from EDCBook's update_area_ex path:
    // n = clamp(effectSteps / 2, 1, 24), step = ceil(width/(n*16))*16.
    switch (readerPageTurnProfile_) {
        case ReaderPageTurnProfile::Fast:     return 6;   // ~192px first band, ~5 physical passes
        case ReaderPageTurnProfile::Balanced: return 12;  // ~96px first band, ~8 physical passes
        case ReaderPageTurnProfile::Clean:
        default:                              return 24;  // ~48px first band, ~15 physical passes
    }
}

uint8_t DisplayService::pageTurnResidueCompensation() const {
    // Passed to Panel_EPD's private page-turn waveform: 0=light, 1=balanced,
    // 2=old-dark -> new-light strong cleanup. This controls single-turn residue
    // independently from the every-N-pages quality refresh.
    return static_cast<uint8_t>(readerPageTurnResidue_);
}

uint8_t DisplayService::pageTurnCompositorFrames() const {
    // Full-frame page compositor: keep the frame count low because each phase is
    // a complete 540x960 target image. The goal is a visible whole-page slide,
    // not exposing the Panel_EPD scanline/dirty-bucket renderer as animation.
    switch (readerPageTurnProfile_) {
        case ReaderPageTurnProfile::Clean:    return 5;
        case ReaderPageTurnProfile::Balanced: return 4;
        case ReaderPageTurnProfile::Fast:
        default:                              return 3;
    }
}

bool DisplayService::ensureTransitionCanvas(const M5Canvas* source) {
    if (!source) return false;
    if (transitionCanvas_ &&
        transitionCanvas_->width() == source->width() &&
        transitionCanvas_->height() == source->height() &&
        transitionCanvas_->getColorDepth() == source->getColorDepth() &&
        transitionCanvas_->getBuffer()) {
        return true;
    }
    if (transitionCanvas_) {
        delete transitionCanvas_;
        transitionCanvas_ = nullptr;
    }
    transitionCanvas_ = new M5Canvas(&M5.Display);
    if (!transitionCanvas_) return false;
    transitionCanvas_->setPsram(true);
    transitionCanvas_->setColorDepth(source->getColorDepth());
    if (!transitionCanvas_->createSprite(source->width(), source->height())) {
        delete transitionCanvas_;
        transitionCanvas_ = nullptr;
        return false;
    }
    return transitionCanvas_->getBuffer() != nullptr;
}

void DisplayService::rememberDisplayedCanvas(const M5Canvas* canvas) {
    M5Canvas* clone = cloneCanvasFrom(canvas);
    if (!clone) {
        Serial.println("[vink3][display] remember displayed canvas skipped: snapshot allocation failed");
        return;
    }
    if (lastDisplayedCanvas_) delete lastDisplayedCanvas_;
    lastDisplayedCanvas_ = clone;
}

static bool composeHorizontalPageCoverFrame(M5Canvas* dstCanvas, const M5Canvas* oldCanvas, const M5Canvas* newCanvas,
                                            DisplayEffect effect, uint8_t frame, uint8_t frames) {
    if (!dstCanvas || !oldCanvas || !newCanvas || frame == 0 || frames == 0) return false;
    if (newCanvas->width() != kPaperS3Width || newCanvas->height() != kPaperS3Height) return false;
    if (oldCanvas->width() != newCanvas->width() || oldCanvas->height() != newCanvas->height() ||
        oldCanvas->getColorDepth() != newCanvas->getColorDepth() || dstCanvas->getColorDepth() != newCanvas->getColorDepth()) {
        return false;
    }
    if (newCanvas->getColorDepth() != 16) return false;

    const auto* oldBuf = static_cast<const uint16_t*>(oldCanvas->getBuffer());
    const auto* newBuf = static_cast<const uint16_t*>(newCanvas->getBuffer());
    auto* frameBuf = static_cast<uint16_t*>(dstCanvas->getBuffer());
    if (!oldBuf || !newBuf || !frameBuf) return false;

    const bool next = (effect == DisplayEffect::VerticalShutter);
    const int16_t covered = (kPaperS3Width * frame + frames - 1) / frames;
    for (int16_t y = 0; y < kPaperS3Height; ++y) {
        const size_t row = static_cast<size_t>(y) * kPaperS3Width;
        uint16_t* dst = frameBuf + row;
        const uint16_t* oldRow = oldBuf + row;
        const uint16_t* newRow = newBuf + row;
        if (next) {
            // EDCBook-like cover turn: old page stays fixed; the final-position
            // new page covers it from right to left behind a moving vertical line.
            const int16_t oldCount = kPaperS3Width - covered;
            if (oldCount > 0) memcpy(dst, oldRow, static_cast<size_t>(oldCount) * sizeof(uint16_t));
            if (covered > 0) memcpy(dst + oldCount, newRow + oldCount, static_cast<size_t>(covered) * sizeof(uint16_t));
        } else {
            // Previous page mirrors the direction: new page covers from left to right.
            if (covered > 0) memcpy(dst, newRow, static_cast<size_t>(covered) * sizeof(uint16_t));
            const int16_t oldCount = kPaperS3Width - covered;
            if (oldCount > 0) memcpy(dst + covered, oldRow + covered, static_cast<size_t>(oldCount) * sizeof(uint16_t));
        }
    }
    return true;
}

bool DisplayService::pushCompositedPageCover(M5Canvas* newCanvas, DisplayEffect effect, epd_mode_t mode) {
    if (!newCanvas || !lastDisplayedCanvas_) return false;
    if (!ensureTransitionCanvas(newCanvas)) return false;

    const bool next = (effect == DisplayEffect::VerticalShutter);
    const uint8_t frames = pageTurnCompositorFrames();
    const uint32_t prevFreq = getCpuFrequencyMhz();
    if (prevFreq < 240) setCpuFrequencyMhz(240);

    Serial.printf("[vink3][display] full-frame page-cover compositor profile=%s frames=%u direction=%s mode=%d\n",
                  readerPageTurnProfileLabel(), static_cast<unsigned>(frames), next ? "next/new-covers-right-to-left" : "prev/new-covers-left-to-right",
                  static_cast<int>(mode));

    M5.Display.waitDisplay();
    M5.Display.setColorDepth(kTextColorDepthHigh);

    bool ok = true;
    for (uint8_t f = 1; f <= frames; ++f) {
        if (!composeHorizontalPageCoverFrame(transitionCanvas_, lastDisplayedCanvas_, newCanvas, effect, f, frames)) {
            ok = false;
            break;
        }
        M5.Display.setEpdMode(f == frames ? mode : kLowRefresh);
        transitionCanvas_->pushSprite(&M5.Display, 0, 0);
        M5.Display.waitDisplay();
    }

    if (prevFreq < 240) setCpuFrequencyMhz(prevFreq);
    if (!ok) return false;
    rememberDisplayedCanvas(newCanvas);
    return true;
}

bool DisplayService::pushEpdiyCompositedPageCover(M5Canvas* newCanvas, DisplayEffect effect, bool quality) {
    if (!newCanvas || !lastDisplayedCanvas_) return false;

    const bool next = (effect == DisplayEffect::VerticalShutter);
    const uint8_t frames = pageTurnCompositorFrames();
    const uint32_t prevFreq = getCpuFrequencyMhz();
    if (prevFreq < 240) setCpuFrequencyMhz(240);

    const uint8_t physicalFramesPerStep =
        readerPageTurnProfile_ == ReaderPageTurnProfile::Clean ? 3 : 2;
    Serial.printf("[vink3][display] epdiy TRUE page-cover compositor profile=%s steps=%u physicalFramesPerStep=%u direction=%s final=%s\n",
                  readerPageTurnProfileLabel(), static_cast<unsigned>(frames),
                  static_cast<unsigned>(physicalFramesPerStep), next ? "next/new-covers-right-to-left" : "prev/new-covers-left-to-right",
                  quality ? "GC16" : "GL16");

    bool ok = true;
    for (uint8_t f = 1; f <= frames; ++f) {
        // Build each old/new cover transition as a fused epdiy difference
        // frame.  The old page remains fixed; the final-position new page
        // covers it behind a moving vertical line, avoiding the older dual-page
        // spatial slide, RGB565 transition canvas, per-step front/back diff
        // pass, and per-step back_fb memcpy.
        if (!g_epdiyPaperS3Backend.pushPageCoverFrame(lastDisplayedCanvas_, newCanvas, effect, f, frames,
                                                      quality && f == frames,
                                                      physicalFramesPerStep)) {
            ok = false;
            break;
        }
    }

    if (prevFreq < 240) setCpuFrequencyMhz(prevFreq);
    if (!ok) return false;
    rememberDisplayedCanvas(newCanvas);
    return true;
}

void DisplayService::pushEdcBookPageTurn(M5Canvas* canvas, DisplayEffect effect, epd_mode_t mode) {
    if (!canvas) return;

    M5.Display.waitDisplay();
    M5.Display.setColorDepth(kTextColorDepthHigh);
    M5.Display.setEpdMode(mode);

    auto* panel = static_cast<lgfx::Panel_EPD*>(M5.Display.panel());
    if (!panel) {
        canvas->pushSprite(&M5.Display, 0, 0);
        M5.Display.waitDisplay();
        return;
    }

    uint16_t offsets[25] = {0};
    const uint8_t offsetCount = buildEdcBookOffsets(offsets, kPaperS3Width, pageTurnBandSeed());
    const uint8_t bandCount = offsetCount > 1 ? offsetCount - 1 : 0;
    const bool rightToLeft = (effect == DisplayEffect::VerticalShutter);
    const uint16_t stripWidth = pageTurnScrollStripWidth();
    const uint8_t residueComp = pageTurnResidueCompensation();
    Serial.printf("[vink3][display] EDCBook page-turn profile=%s residue=%s comp=%u mode=%d bands=%u strip0=%u direction=%s waveform=private-old-new-lut\n",
                  readerPageTurnProfileLabel(), readerPageTurnResidueLabel(), residueComp,
                  static_cast<int>(mode), bandCount, stripWidth, rightToLeft ? "rtl" : "ltr");

    const uint32_t prevFreq = getCpuFrequencyMhz();
    if (prevFreq < 240) setCpuFrequencyMhz(240);

    // Stage the full next-page framebuffer first; no public clip/push timing is
    // used for the animation. The patched Panel_EPD worker owns the
    // waveform/scan-cycle layer and reveals the already-staged buffer by strips.
    const bool savedAutoDisplay = M5.Display.getPanel()->getAutoDisplay();
    M5.Display.setAutoDisplay(false);
    canvas->pushSprite(&M5.Display, 0, 0);
    M5.Display.setAutoDisplay(savedAutoDisplay);
    panel->displayScroll(0, 0, kPaperS3Width, kPaperS3Height,
                         stripWidth, rightToLeft,
                         residueComp,  // single-turn residue compensation
                         pageTurnBandSeed());
    M5.Display.waitDisplay();
    if (prevFreq < 240) setCpuFrequencyMhz(prevFreq);
}

epd_mode_t DisplayService::chooseReaderRefreshMode(const DisplayRequest& request) {
    // Stable baseline: ReaderRefreshStrategy controls full-clean frequency, decoupled
    // from page-turn animation speed. PageTurnProfile now controls the non-quality
    // waveform used by the EDCBook band renderer.
    uint32_t fullEvery = 10;
    epd_mode_t normalMode = kLowRefresh;
    if (readerPageTurnProfile_ == ReaderPageTurnProfile::Clean) normalMode = kNormalRefresh;
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
#if !VINK_USE_EPDIY_BACKEND
        M5.Display.setColorDepth(kTextColorDepthHigh);
#endif
        return kQualityRefresh;
    }

    readerPageTurnCount_ = nextTurn;
#if !VINK_USE_EPDIY_BACKEND
    M5.Display.setColorDepth(kTextColorDepthHigh);
#endif
    return normalMode;
}

epd_mode_t DisplayService::chooseRefreshMode(const DisplayRequest& request) {
    if (request.readerPageTurn) return chooseReaderRefreshMode(request);

    const bool useQualityMode = request.quality ||
        (fastRefresh_ && pushCount_ >= kDisplayQualityFastThreshold) ||
        (!fastRefresh_ && pushCount_ >= kDisplayFullRefreshNormalThreshold);

    if (useQualityMode) {
        pushCount_ = 0;
#if !VINK_USE_EPDIY_BACKEND
        M5.Display.setColorDepth(kTextColorDepthHigh);
#endif
        return kQualityRefresh;
    }

#if !VINK_USE_EPDIY_BACKEND
    M5.Display.setColorDepth(kTextColorDepthHigh);
#endif
    // Non-reader UI (tabs/settings/library) should acknowledge taps quickly,
    // but v0.4.41 showed epd_fastest is too dirty for real UI pages. Use the
    // middle fast waveform for ordinary UI pushes and keep periodic quality
    // cleanup instead of making every tab switch look like text/quality full refresh.
    return fastRefresh_ ? kMiddleRefresh : kNormalRefresh;
}

void DisplayService::push(const DisplayRequest& request, M5Canvas* canvasToPush) {
    if (!canvasToPush) return;

    busy_ = true;
    g_inDisplayPush = true;

#if !VINK_USE_EPDIY_BACKEND
    M5.Display.waitDisplay();
#endif

    if (request.readerPageTurn && request.effect != DisplayEffect::None) {
        const epd_mode_t readerMode = chooseReaderRefreshMode(request);
#if VINK_USE_EPDIY_BACKEND
        if (readerMode == kQualityRefresh) {
            if (g_epdiyPaperS3Backend.pushCanvas(canvasToPush, true)) {
                rememberDisplayedCanvas(canvasToPush);
            }
#if VINK_EPDIY_STRICT
            else {
                Serial.println("[vink3][display] epdiy quality page-turn failed; strict validation build will not fall back to M5GFX");
            }
#endif
        } else if (request.effect == DisplayEffect::VerticalShutter || request.effect == DisplayEffect::HorizontalShutter) {
            // The user asked for the EDCBook effect: content must spatially move.
            // Do not use the old epdiy fixed-position bucket wipe as the default.
            if (!pushEpdiyCompositedPageCover(canvasToPush, request.effect, false)) {
#if VINK_EPDIY_STRICT
                Serial.println("[vink3][display] epdiy true page-cover failed; strict validation build will not fall back to fixed wipe/M5GFX");
#else
                Serial.println("[vink3][display] epdiy true page-cover unavailable; falling back to M5GFX compositor");
                if (!pushCompositedPageCover(canvasToPush, request.effect, pageTurnScrollMode(readerMode))) {
                    M5.Display.setColorDepth(kTextColorDepthHigh);
                    M5.Display.setEpdMode(kNormalRefresh);
                    canvasToPush->pushSprite(&M5.Display, 0, 0);
                    M5.Display.waitDisplay();
                    rememberDisplayedCanvas(canvasToPush);
                }
#endif
            }
        } else if (g_epdiyPaperS3Backend.pushCanvas(canvasToPush, false)) {
            rememberDisplayedCanvas(canvasToPush);
        }
#else
        if (readerMode == kQualityRefresh) {
            M5.Display.setColorDepth(kTextColorDepthHigh);
            M5.Display.setEpdMode(readerMode);
            canvasToPush->pushSprite(&M5.Display, 0, 0);
            M5.Display.waitDisplay();
            rememberDisplayedCanvas(canvasToPush);
        } else if (VINK_ENABLE_M5GFX_SCROLL_PAGE_TURN &&
                   (request.effect == DisplayEffect::VerticalShutter || request.effect == DisplayEffect::HorizontalShutter)) {
            // Build complete old/new intermediate pages before every physical
            // push. This keeps the visible animation at page-compositor level;
            // the low-level Panel_EPD row scanner is no longer the animation.
            if (!pushCompositedPageCover(canvasToPush, request.effect, pageTurnScrollMode(readerMode))) {
                Serial.println("[vink3][display] compositor page-cover unavailable; falling back to Panel_EPD scroll");
                pushEdcBookPageTurn(canvasToPush, request.effect, pageTurnScrollMode(readerMode));
                rememberDisplayedCanvas(canvasToPush);
            }
        } else {
            // Explicit fallback only for diagnostic builds that compile the
            // scroll path out. Normal RCs should keep optimizing scroll instead
            // of silently reverting to a plain full-page reader refresh.
            const epd_mode_t stableDirectMode = (readerMode == kQualityRefresh) ? kQualityRefresh : kNormalRefresh;
            Serial.printf("[vink3][display] diagnostic no-scroll fallback: direct full-page requested=%d actual=%d\n",
                          static_cast<int>(readerMode), static_cast<int>(stableDirectMode));
            M5.Display.setColorDepth(kTextColorDepthHigh);
            M5.Display.setEpdMode(stableDirectMode);
            canvasToPush->pushSprite(&M5.Display, 0, 0);
            M5.Display.waitDisplay();
            rememberDisplayedCanvas(canvasToPush);
        }
#endif
        pushCount_++;
        g_inDisplayPush = false;
        busy_ = false;
        return;
    }

#if VINK_USE_EPDIY_BACKEND
    if (g_epdiyPaperS3Backend.pushCanvas(canvasToPush, request.quality)) {
        if (!request.transparent && request.x == 0 && request.y == 0 && request.w == kPaperS3Width && request.h == kPaperS3Height) {
            rememberDisplayedCanvas(canvasToPush);
        }
        pushCount_++;
        g_inDisplayPush = false;
        busy_ = false;
        return;
    }
#if VINK_EPDIY_STRICT
    Serial.println("[vink3][display] epdiy full update failed; strict validation build will not fall back to M5GFX");
    pushCount_++;
    g_inDisplayPush = false;
    busy_ = false;
    return;
#else
    Serial.println("[vink3][display] epdiy full update failed; falling back to M5GFX path");
#endif
#endif

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
    if (!request.transparent && x == 0 && y == 0 && request.w == kPaperS3Width && request.h == kPaperS3Height) {
        rememberDisplayedCanvas(canvasToPush);
    }

    pushCount_++;
    g_inDisplayPush = false;
    busy_ = false;
}

} // namespace vink3
