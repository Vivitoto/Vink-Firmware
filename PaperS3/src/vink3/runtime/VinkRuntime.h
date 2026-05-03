#pragma once
#include <Arduino.h>
#include <M5Unified.h>
#include "../ReadPaper176.h"

namespace vink3 {

class VinkRuntime {
public:
    bool begin();
    void loop();
    void drawBoot();
    M5Canvas* canvas();

private:
    bool beginHardware();
    bool beginCanvas();
    bool beginServices();

    M5Canvas canvas_{&M5.Display};
    bool hardwareReady_ = false;
    bool canvasReady_ = false;
    uint32_t lastHeartbeatLogMs_ = 0;
};

extern VinkRuntime g_runtime;

} // namespace vink3
