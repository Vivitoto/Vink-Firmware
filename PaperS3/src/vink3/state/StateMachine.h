#pragma once
#include <Arduino.h>
#include "Messages.h"

namespace vink3 {

class StateMachine {
public:
    bool begin(uint8_t queueLen = 12);
    bool post(const Message& message, uint32_t timeoutMs = 20);
    SystemState state() const;
    uint32_t lastActivityMs() const { return lastActivityMs_; }
    void onActivity() { lastActivityMs_ = millis(); }

private:
    static void taskThunk(void* arg);
    void taskLoop();
    void handle(const Message& message);

    QueueHandle_t queue_ = nullptr;
    TaskHandle_t task_ = nullptr;
    SystemState state_ = SystemState::Boot;
    uint32_t lastActivityMs_ = 0;
};

extern StateMachine g_stateMachine;

} // namespace vink3
