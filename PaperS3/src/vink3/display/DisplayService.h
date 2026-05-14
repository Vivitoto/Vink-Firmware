#pragma once
#include <Arduino.h>
#include <M5Unified.h>
#include "../VinkPaperS3.h"

namespace vink3 {

enum class DisplayEffect : uint8_t {
    None = 0,
    VerticalShutter = 1,
    HorizontalShutter = 2,
    Rect = 3,
    SweepBands = 4,  // EDCBook-style narrow vertical line sweep
};

enum class ReaderRefreshStrategy : uint8_t {
    Speed = 0,    // legacy value reused as full-clean frequency: low
    Balanced = 1, // full-clean frequency: medium
    Clear = 2,    // full-clean frequency: high
};

// ReadPaper 1.7.6 style display message: flags + effect + rectangle.
struct DisplayRequest {
    bool transparent = false; // ReadPaper flags[0]
    bool invert = false;      // ReadPaper flags[1]
    bool quality = false;     // ReadPaper flags[2]
    bool reserved = false;    // ReadPaper flags[3]
    bool readerPageTurn = false;
    DisplayEffect effect = DisplayEffect::None;
    int16_t x = 0;
    int16_t y = 0;
    int16_t w = kPaperS3Width;
    int16_t h = kPaperS3Height;
};

class DisplayService {
public:
    bool begin(M5Canvas* canvas, uint8_t queueLen = 8);
    bool enqueue(const DisplayRequest& request, uint32_t timeoutMs = 20);
    bool enqueueFull(bool quality = false, uint32_t timeoutMs = 20);
    bool enqueueReaderPageTurn(DisplayEffect effect = DisplayEffect::None, uint32_t timeoutMs = 20);
    bool waitIdle(uint32_t timeoutMs = 3000) const;
    bool isBusy() const;
    uint32_t pushCount() const;
    void resetPushCount();
    uint32_t readerPageTurnCount() const;
    void resetReaderPageTurnCount();
    void forceNextReaderFullRefresh();
    void cycleReaderRefreshStrategy();
    void setReaderRefreshStrategy(ReaderRefreshStrategy strategy);
    bool saveLocalSettings() const;
    ReaderRefreshStrategy readerRefreshStrategy() const { return readerRefreshStrategy_; }
    const char* readerRefreshStrategyLabel() const;

private:
    static void taskThunk(void* arg);
    void taskLoop();
    void push(const DisplayRequest& request, M5Canvas* canvasToPush);
    M5Canvas* cloneCanvas() const;
    bool enqueueCanvasCloneBlocking(M5Canvas* clone);
    M5Canvas* dequeueCanvasClone();
    epd_mode_t chooseRefreshMode(const DisplayRequest& request);
    void loadLocalSettings();
    epd_mode_t chooseReaderRefreshMode(const DisplayRequest& request);
    void pushShutterAnimation(M5Canvas* canvas, DisplayEffect effect, epd_mode_t mode);
    void pushSweepBandsEffect(M5Canvas* canvas, DisplayEffect effect, epd_mode_t mode);
    void M5DisplayStripSweep(M5Canvas* canvas, DisplayEffect effect, epd_mode_t mode);

    M5Canvas* canvas_ = nullptr;
    QueueHandle_t queue_ = nullptr;
    QueueHandle_t canvasQueue_ = nullptr;
    TaskHandle_t task_ = nullptr;
    volatile bool busy_ = false;
    volatile uint32_t pushCount_ = 0;
    volatile uint32_t readerPageTurnCount_ = 0;
    volatile bool forceNextReaderFullRefresh_ = false;
    // PaperS3 official/examples and reference firmware favor fast EPD updates for
    // interactive UI, with periodic quality refreshes to clean ghosting.
    bool fastRefresh_ = true;
    ReaderRefreshStrategy readerRefreshStrategy_ = ReaderRefreshStrategy::Balanced;
};

extern DisplayService g_displayService;
extern volatile bool g_inDisplayPush;

} // namespace vink3
