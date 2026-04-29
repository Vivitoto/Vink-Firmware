#pragma once
#include <Arduino.h>
#include <M5Unified.h>
#include "../state/StateMachine.h"

namespace vink3 {

class InputService {
public:
    bool begin(StateMachine* stateMachine);

private:
    static void taskThunk(void* arg);
    void taskLoop();
    void pollTouch();

    StateMachine* stateMachine_ = nullptr;
    TaskHandle_t task_ = nullptr;
    bool wasPressed_ = false;
    TouchPoint pressPoint_{};
    TouchPoint lastPoint_{};
    TouchPoint pressRawPoint_{};
    TouchPoint lastRawPoint_{};
    uint32_t pressStartedMs_ = 0;
    uint32_t lastEventMs_ = 0;
};

extern InputService g_inputService;

} // namespace vink3
