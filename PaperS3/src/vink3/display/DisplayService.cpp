#include "DisplayService.h"
#include <algorithm>
#include <cstring>
#include <Preferences.h>
#include <lgfx/v1/platforms/esp32/Panel_EPD.hpp>

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
            16384,
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
    const uint8_t turnEngine = prefs.getUChar("turneng", static_cast<uint8_t>(readerPageTurnEngine_));
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
    if (turnEngine <= static_cast<uint8_t>(ReaderPageTurnEngine::ContinuousFlow)) {
        readerPageTurnEngine_ = static_cast<ReaderPageTurnEngine>(turnEngine);
    }
}

bool DisplayService::saveLocalSettings() const {
    Preferences prefs;
    if (!prefs.begin("vink-display", false)) return false;
    prefs.putUChar("refresh", static_cast<uint8_t>(readerRefreshStrategy_));
    prefs.putUChar("turnprof", static_cast<uint8_t>(readerPageTurnProfile_));
    prefs.putUChar("turnresid", static_cast<uint8_t>(readerPageTurnResidue_));
    prefs.putUChar("turneng", static_cast<uint8_t>(readerPageTurnEngine_));
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

void DisplayService::setReaderPageTurnEngine(ReaderPageTurnEngine engine) {
    readerPageTurnEngine_ = engine;
    resetReaderPageTurnCount();
    saveLocalSettings();
    Serial.printf("[vink3][display] reader page-turn engine -> %s\n", readerPageTurnEngineLabel());
}

void DisplayService::cycleReaderPageTurnEngine() {
    switch (readerPageTurnEngine_) {
        case ReaderPageTurnEngine::SerialStrip:
            setReaderPageTurnEngine(ReaderPageTurnEngine::ContinuousFlow);
            break;
        case ReaderPageTurnEngine::ContinuousFlow:
        default:
            setReaderPageTurnEngine(ReaderPageTurnEngine::SerialStrip);
            break;
    }
}

const char* DisplayService::readerPageTurnEngineLabel() const {
    switch (readerPageTurnEngine_) {
        case ReaderPageTurnEngine::SerialStrip:    return "细线分段";
        case ReaderPageTurnEngine::ContinuousFlow: return "连续推进";
    }
    return "细线分段";
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
// The actual PaperS3 panel driver is Panel_EPD.  The reverse notes show EDCBook's real
// implementation lives below the app layer in the panel waveform/renderer; on
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

    // v0.4.56 simplifies the non-reader path; pageTurnScrollMode is no longer
    // required because pushEdcBookPageTurn enforces the smooth text waveform
    // internally.  Helper retained for backward compatibility with diagnostic
    // builds that still call it; new code should not introduce new callers.
static epd_mode_t pageTurnScrollMode(epd_mode_t scheduledMode) __attribute__((unused));
static epd_mode_t pageTurnScrollMode(epd_mode_t scheduledMode) {
    return scheduledMode == kQualityRefresh ? kQualityRefresh : scheduledMode;
}

static int effectStepsForStrategy(ReaderRefreshStrategy /*s*/) __attribute__((unused));
static int effectStepsForStrategy(ReaderRefreshStrategy /*s*/) {
    // Retained for historical reference; no live caller now that the page-turn
    // strip width is controlled exclusively by ReaderPageTurnProfile through
    // pageTurnScrollStripWidth().  Kept private so accidental reintroduction
    // shows up as a duplicate symbol rather than a silent regression.
    return 24;
}

uint16_t DisplayService::pageTurnScrollStripWidth() const {
    // v0.4.57 real-device tuning: v0.4.56 made the setting effective again,
    // but the old EDCBook-derived seed math made the persisted Fast profile a
    // huge ~192 px reveal band.  On PaperS3 that looks like two connected
    // boards sweeping across the page, and after a quality/full refresh the
    // first following turn can drag a large black block.  Use explicit narrow
    // logical strips instead: still profile-controlled, but never a wide board.
    switch (readerPageTurnProfile_) {
        case ReaderPageTurnProfile::Fast:     return 48;  // ~12 passes; close to v0.4.28 visual width
        case ReaderPageTurnProfile::Balanced: return 32;  // ~17 passes
        case ReaderPageTurnProfile::Clean:
        default:                              return 24;  // ~23 passes; thinnest line-like reveal
    }
}

uint8_t DisplayService::pageTurnBandSeed() const {
    // Historical diagnostic only.  The live path uses explicit strip widths
    // above because the seed/offset formula made the default Fast profile much
    // too wide on real PaperS3 hardware.
    switch (readerPageTurnProfile_) {
        case ReaderPageTurnProfile::Fast:     return 6;
        case ReaderPageTurnProfile::Balanced: return 12;
        case ReaderPageTurnProfile::Clean:
        default:                              return 24;
    }
}

uint8_t DisplayService::pageTurnResidueCompensation() const {
    // Passed to Panel_EPD's private page-turn waveform: 0=light, 1=balanced,
    // 2=old-dark -> new-light strong cleanup. This controls single-turn residue
    // independently from the every-N-pages quality refresh.
    return static_cast<uint8_t>(readerPageTurnResidue_);
}

void DisplayService::pushEdcBookPageTurn(M5Canvas* canvas, DisplayEffect effect, epd_mode_t mode) {
    if (!canvas) return;

    M5.Display.waitDisplay();
    M5.Display.setColorDepth(kTextColorDepthHigh);

    // v0.4.56 recovery: reader pushes must never reach a Bayer-thresholded
    // waveform (`epd_fast`/`epd_fastest`), because that path quantises every
    // pixel to pure black/white through ordered dither and breaks glyph
    // strokes (see Negative_Findings #27).  v0.4.28-rc applied the same guard
    // inside pushShutterAnimation(); we apply it here for the scroll
    // wavefront.  Quality cleanup never enters this function because
    // DisplayService::push() handles it as a direct full-page refresh, so the
    // reveal waveform is always the smooth text waveform regardless of which
    // non-quality mode the scheduler chose.
    const epd_mode_t sweepMode = kNormalRefresh;
    M5.Display.setEpdMode(sweepMode);

    auto* panel = static_cast<lgfx::Panel_EPD*>(M5.Display.panel());
    if (!panel) {
        canvas->pushSprite(&M5.Display, 0, 0);
        M5.Display.waitDisplay();
        return;
    }

    const bool savedAutoDisplay = M5.Display.getPanel()->getAutoDisplay();
    M5.Display.setAutoDisplay(false);
    canvas->pushSprite(&M5.Display, 0, 0);
    M5.Display.setAutoDisplay(savedAutoDisplay);

    // v0.4.28 moved the visible animation below public clip/push timing: the
    // staged framebuffer is revealed by Panel_EPD's waveform/scan-cycle layer.
    const bool rtl = (effect == DisplayEffect::VerticalShutter);
    // Use the runtime page-turn profile for wavefront width. A previous recovery
    // accidentally used readerRefreshStrategy_ here, so the Settings "翻页速度"
    // row persisted but did not change the actual scroll passes, making the
    // reading page feel slower than the selected profile implied.
    const uint16_t stripWidth = pageTurnScrollStripWidth();
    Serial.printf("[vink3][display] v0.4.28 edcscroll Panel_EPD logical-strip mode=%d sweep=%d strip=%u direction=%s engine=%s\n",
                  static_cast<int>(mode), static_cast<int>(sweepMode), stripWidth, rtl ? "rtl" : "ltr",
                  readerPageTurnEngineLabel());
    // Keep the no-epdiy recovery on the known-good v0.4.28 logical-strip path.
    // The later private transition/waveform parameters underdrove text and, after
    // rotation, advanced along the physical Y axis, which looked like bottom-up
    // row refresh instead of the v0.4.28 right/left page-turn.
    panel->displayScroll(0, 0, kPaperS3Width, kPaperS3Height, stripWidth, rtl,
                         static_cast<uint8_t>(readerPageTurnEngine_));
    M5.Display.waitDisplay();
}

epd_mode_t DisplayService::chooseReaderRefreshMode(const DisplayRequest& request) {
    // v0.4.56 recovery: real-device feedback showed v0.4.54 reader pushes
    // looked like dot-matrix ink dots. Root cause was the v0.4.5x default
    // `normalMode = kLowRefresh (=epd_fastest)`, whose Panel_EPD draw path
    // thresholds every pixel through a 4x4 Bayer (`< 248 ? 0 : 0xF`). That is
    // the correct fast UI behavior for tabs/menus, but it destroys glyph
    // strokes by quantising the rgb565 grayscale framebuffer to pure black/
    // white through an ordered-dither pattern, which the user reads as
    // "strokes made of ink dots".
    //
    // v0.4.28-rc instead used `normalMode = kNormalRefresh (=epd_text)` for
    // every regular reader turn, where Panel_EPD applies the smooth
    // `(v + b - 8) >> 4` 16-gray mapping. Restore that as the default; page-turn
    // speed should be tuned by scroll strip width, not by binary fastest mode.
    // ReaderRefreshStrategy controls full-clean frequency; page-turn animation
    // speed is decoupled and controlled by ReaderPageTurnProfile strip width.
    uint32_t fullEvery = 10;
    epd_mode_t normalMode = kNormalRefresh;
    if (readerPageTurnProfile_ == ReaderPageTurnProfile::Fast) {
        // Fast page-turn profile still keeps the smooth text waveform for the
        // pixel push itself. The narrow Panel_EPD scroll wavefront is what
        // gives "fast" its perceived speed; binary Bayer thresholding is not
        // required and was the regression source.
        normalMode = kNormalRefresh;
    } else if (readerPageTurnProfile_ == ReaderPageTurnProfile::Clean) {
        normalMode = kNormalRefresh;
    }
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

    // Non-reader UI (tabs/settings/library) should feel like a native touch
    // surface.  v0.4.56 kept these pages on epd_text to avoid the reader's
    // dot-matrix regression, but full-screen UI transitions then flashed all
    // widgets with a slow GL16/text-looking update and felt badly detached from
    // the tap.  Keep the strict epd_text rule for reader pages only; ordinary
    // UI returns to the faster middle waveform, with periodic quality cleanup.
    const bool useQualityMode = request.quality ||
        pushCount_ >= kDisplayFullRefreshNormalThreshold;

    if (useQualityMode) {
        pushCount_ = 0;
        M5.Display.setColorDepth(kTextColorDepthHigh);
        return kQualityRefresh;
    }

    M5.Display.setColorDepth(kTextColorDepthHigh);
    return kMiddleRefresh;
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
        } else if (VINK_ENABLE_M5GFX_SCROLL_PAGE_TURN &&
                   (request.effect == DisplayEffect::VerticalShutter || request.effect == DisplayEffect::HorizontalShutter)) {
            // Restore the v0.4.28-rc edcscroll default: stage the complete next
            // page once, then let the patched Panel_EPD worker reveal it as a
            // narrow moving wavefront.  pushEdcBookPageTurn() forces the smooth
            // text waveform internally, so the caller does not have to map
            // non-quality modes here.
            pushEdcBookPageTurn(canvasToPush, request.effect, readerMode);
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
