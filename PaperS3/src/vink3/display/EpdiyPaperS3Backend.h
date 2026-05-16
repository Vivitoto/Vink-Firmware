#pragma once

#include <Arduino.h>
#include <M5Unified.h>
#include "DisplayService.h"

namespace vink3 {

// Experimental architecture-level PaperS3 display backend that drives the
// ED047TC1 through epdiy's LCD/RMT/GDMA renderer instead of M5GFX Panel_EPD.
// It is compiled for bring-up but kept opt-in until the hardware path is proven.
class EpdiyPaperS3Backend {
public:
    bool begin();
    bool isReady() const { return ready_; }

    bool pushCanvas(M5Canvas* canvas, bool quality);
    bool pushPageTurn(M5Canvas* canvas, DisplayEffect effect, bool quality,
                      uint8_t effectSteps, uint8_t residueCompensation);

private:
    bool copyCanvasToFrontFramebuffer(M5Canvas* canvas);
    static uint8_t rgb565ToGray4(uint16_t rgb565);

    bool ready_ = false;
    bool powered_ = false;
};

extern EpdiyPaperS3Backend g_epdiyPaperS3Backend;

} // namespace vink3
