#include <Arduino.h>
#include "vink3/ReadPaper176.h"
#include "vink3/runtime/VinkRuntime.h"

namespace {
TaskHandle_t s_mainTask = nullptr;

void MainTask(void*) {
    if (!vink3::g_runtime.begin()) {
        Serial.printf("[MainTask] Vink %s runtime init failed, halting\n", vink3::kVinkPaperS3FirmwareVersion);
        for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    for (;;) {
        vink3::g_runtime.loop();
    }
}
} // namespace

void setup() {
    // ReadPaper 1.7.6 style: Arduino setup only starts a pinned supervisor task;
    // the runtime then creates display/input/state service tasks.
    BaseType_t ok = xTaskCreatePinnedToCore(
        MainTask,
        "MainTask",
        32768,
        nullptr,
        4,
        &s_mainTask,
        1);
    if (ok != pdPASS) {
        Serial.begin(115200);
        Serial.println("[setup] failed to create MainTask");
    }
}

void loop() {
    vTaskDelay(pdMS_TO_TICKS(1000));
}
