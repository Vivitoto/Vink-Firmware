#include "LegadoService.h"

namespace vink3 {

LegadoService g_legadoService;

bool LegadoService::begin(StateMachine* stateMachine) {
    if (!stateMachine) return false;
    stateMachine_ = stateMachine;
    Serial.println("[vink3][legado] service initialized");
    return true;
}

void LegadoService::configure(const LegadoConfig& config) {
    config_ = config;
}

bool LegadoService::requestProgressSync(const String& bookId, float progress) {
    if (!stateMachine_ || !config_.enabled || config_.baseUrl.isEmpty()) {
        return false;
    }

    Message start;
    start.type = MessageType::LegadoSyncStart;
    start.timestampMs = millis();
    stateMachine_->post(start);

    // Real HTTP sync is intentionally left for the next phase. This skeleton keeps
    // Legado as a service and reports through the state machine instead of UI code.
    (void)bookId;
    (void)progress;

    Message done;
    done.type = MessageType::LegadoSyncDone;
    done.timestampMs = millis();
    return stateMachine_->post(done);
}

} // namespace vink3
