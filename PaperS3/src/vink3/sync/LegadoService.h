#pragma once
#include <Arduino.h>
#include "../state/StateMachine.h"

namespace vink3 {

struct LegadoConfig {
    String baseUrl;
    String token;
    bool enabled = false;
};

class LegadoService {
public:
    bool begin(StateMachine* stateMachine);
    void configure(const LegadoConfig& config);
    bool requestProgressSync(const String& bookId, float progress);

private:
    StateMachine* stateMachine_ = nullptr;
    LegadoConfig config_{};
};

extern LegadoService g_legadoService;

} // namespace vink3
