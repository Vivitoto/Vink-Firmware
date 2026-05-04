#pragma once
#include <Arduino.h>
#include <M5Unified.h>
#include "../ReadPaper176.h"

namespace vink3 {

enum class DisplayEffect : uint8_t {
    None = 0,
    VerticalShutter = 1,
    HorizontalShutter = 2,
    Rect = 3,
};

// ReadPaper 1.7.6 style display message: flags + effect + rectangle.
struct DisplayRequest {
    bool transparent = false; // ReadPaper flags[0]
    bool invert = false;      // ReadPaper flags[1]
    bool quality = false;     // ReadPaper flags[2]
    bool reserved = false;    // ReadPaper flags[3]
    DisplayEffect effect = DisplayEffect::None;
    int16_t x = 0;
    int16_t y = 0;
    int16_t w = kPaperS3Width;
    int16_t h = kPaperS3Height;
};

class DisplayService {
public:
    bool begin(M5Canvas* canvas, uint8_t queueLen = 3);
    bool enqueue(const DisplayRequest& request, uint32_t timeoutMs = 20);
    bool enqueueFull(bool quality = false, uint32_t timeoutMs = 20);
    bool waitIdle(uint32_t timeoutMs = 3000) const;
    bool isBusy() const;
    uint32_t pushCount() const;
    void resetPushCount();

private:
    static void taskThunk(void* arg);
    void taskLoop();
    struct QueuedDisplayRequest {
        DisplayRequest request;
        M5Canvas* canvasSnapshot = nullptr;
    };

    void push(const DisplayRequest& request, M5Canvas* canvasToPush);
    M5Canvas* cloneCanvas() const;
    epd_mode_t chooseRefreshMode(const DisplayRequest& request);

    M5Canvas* canvas_ = nullptr;
    QueueHandle_t queue_ = nullptr;
    TaskHandle_t task_ = nullptr;
    volatile bool busy_ = false;
    volatile uint32_t pushCount_ = 0;
    // PaperS3 official/examples and reference firmware favor fast EPD updates for
    // interactive UI, with periodic quality refreshes to clean ghosting.
    bool fastRefresh_ = true;
};

extern DisplayService g_displayService;
extern volatile bool g_inDisplayPush;

} // namespace vink3
