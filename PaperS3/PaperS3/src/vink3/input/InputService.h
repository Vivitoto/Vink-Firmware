#pragma once
#include <Arduino.h>
#include <M5Unified.h>
#include "../state/StateMachine.h"

namespace vink3 {

class InputService {
public:
    bool begin(StateMachine* stateMachine);
    void suppressFor(uint32_t cooldownMs);
    void suppressUntilRelease(uint32_t cooldownMs);

private:
    static void taskThunk(void* arg);
    void taskLoop();
    void pollTouch(uint32_t now);
    void pollPowerButton(uint32_t now);
    void pollSideKey(uint32_t now);
    bool readerQuickTurnZone(const TouchPoint& point, bool& outNext) const;
    bool postReaderQuickTurn(bool next, uint32_t now, const TouchPoint& point, const TouchPoint& rawPoint);
    void resetTouchGesture();

    StateMachine* stateMachine_ = nullptr;
    TaskHandle_t task_ = nullptr;
    bool wasPressed_ = false;
    bool pressConsumed_ = false;
    bool waitRelease_ = false;
    TouchPoint pressPoint_{};
    TouchPoint lastPoint_{};
    TouchPoint pressRawPoint_{};
    TouchPoint lastRawPoint_{};
    uint32_t pressStartedMs_ = 0;
    uint32_t lastPressMs_ = 0;
    uint32_t lastMovePostMs_ = 0;
    uint8_t liftMissingFrames_ = 0;
    uint32_t suppressUntilMs_ = 0;
    uint32_t lastReaderQuickTurnMs_ = 0;
    bool pendingReaderTurn_ = false;
    bool pendingReaderTurnNext_ = false;
    TouchPoint pendingReaderTurnPoint_{};
    TouchPoint pendingReaderTurnRawPoint_{};
    bool powerArmed_ = false;
};

extern InputService g_inputService;

} // namespace vink3
