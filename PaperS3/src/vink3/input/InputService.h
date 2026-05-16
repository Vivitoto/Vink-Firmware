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
    void pollTouch();
    void pollPowerButton(uint32_t now);
    void pollSideKey(uint32_t now);
    void updateTouchCoordMode(int rawX, int rawY);
    bool readerQuickTurnZone(const TouchPoint& point, bool& outNext) const;
    bool postReaderQuickTurn(bool next, uint32_t now, const TouchPoint& point, const TouchPoint& rawPoint);
    void resetReaderQuickTurn();

    StateMachine* stateMachine_ = nullptr;
    TaskHandle_t task_ = nullptr;
    SemaphoreHandle_t touchSem_ = nullptr;
    bool wasPressed_ = false;
    TouchPoint pressPoint_{};
    TouchPoint lastPoint_{};
    TouchPoint pressRawPoint_{};
    TouchPoint lastRawPoint_{};
    uint32_t pressStartedMs_ = 0;
    uint32_t lastEventMs_ = 0;
    uint32_t suppressUntilMs_ = 0;
    bool waitRelease_ = false;
    uint32_t lastMovePostMs_ = 0;
    uint8_t liftMissingFrames_ = 0;
    bool readerQuickTurnCandidate_ = false;
    bool readerQuickTurnNext_ = false;
    bool readerQuickTurnFired_ = false;
    TouchPoint readerQuickTurnPressPoint_{};
    TouchPoint readerQuickTurnRawPoint_{};
    uint32_t readerQuickTurnStartedMs_ = 0;
    uint32_t lastReaderQuickTurnMs_ = 0;
    bool pendingReaderTurn_ = false;
    bool pendingReaderTurnNext_ = false;
    TouchPoint pendingReaderTurnPoint_{};
    TouchPoint pendingReaderTurnRawPoint_{};
    uint32_t pendingReaderTurnMs_ = 0;
    bool powerArmed_ = false;
    bool powerWasPressed_ = false;
    bool sideKeyArmed_ = false;
    bool sideKeyWasLow_ = false;
    bool sideKeyWaitLogged_ = false;
    uint32_t sideKeyArmStartMs_ = 0;
    uint8_t sideKeyTransitionCount_ = 0;
    uint32_t powerPressStartedMs_ = 0;
    uint32_t lastPowerClickMs_ = 0;
};

extern InputService g_inputService;

} // namespace vink3
