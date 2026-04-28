#include "VinkRuntime.h"
#include "../display/DisplayService.h"
#include "../input/InputService.h"
#include "../reader/ReaderBookService.h"
#include "../reader/ReaderTextRenderer.h"
#include "../state/StateMachine.h"
#include "../sync/LegadoService.h"
#include "../ui/VinkUiRenderer.h"
#include <SPIFFS.h>
#include <SD.h>
#include "esp_sleep.h"
#include "driver/gpio.h"

namespace vink3 {

VinkRuntime g_runtime;

bool VinkRuntime::begin() {
    Serial.println("[vink3][runtime] starting v0.3.0 from ReadPaper V1.7.6 baseline");
    if (!beginHardware()) return false;
    if (!beginCanvas()) return false;
    if (!beginServices()) return false;
    drawBoot();
    return true;
}

bool VinkRuntime::beginHardware() {
    if (hardwareReady_) return true;

    Serial.begin(115200);
    delay(200);
    Serial.printf("\n[Vink v0.3.0] ReadPaper baseline %s @ %s\n", kReadPaperUpstreamVersion, kReadPaperUpstreamCommit);
    Serial.printf("[vink3][boot] wake cause=%d psram size=%u free=%u\n",
                  static_cast<int>(esp_sleep_get_wakeup_cause()),
                  ESP.getPsramSize(), ESP.getFreePsram());

    auto cfg = M5.config();
    // Mirrors ReadPaper 1.7.6 setup order: keep the EPD from being cleared by
    // M5.begin(), force PaperS3 fallback, and bring power/RTC/IMU online early.
    cfg.clear_display = false;
    cfg.output_power = true;
    cfg.internal_imu = true;
    cfg.internal_rtc = true;
    cfg.internal_spk = false;
    cfg.internal_mic = false;
    cfg.fallback_board = m5::board_t::board_M5PaperS3;
    M5.begin(cfg);
    delay(50);

    gpio_wakeup_enable(GPIO_NUM_48, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();

    M5.Display.powerSaveOff();
    M5.Display.setEpdMode(kLowRefresh);
    M5.Display.setColorDepth(kTextColorDepth);
    // Vink's user-facing PaperS3 portrait orientation is rotation 0 (handle/top
    // direction matches the existing Crosslink shell and touch geometry). The
    // v0.3 canvas is 540x960, so using ReadPaper's alternate rotation here can
    // clip/rotate the framebuffer and make the device appear unresponsive.
    M5.Display.setRotation(0);

    if (!SPIFFS.begin(false)) {
        Serial.println("[vink3][boot] SPIFFS mount failed; continuing without formatting");
    }

    hardwareReady_ = true;
    return true;
}

bool VinkRuntime::beginCanvas() {
    if (canvasReady_) return true;
    canvas_.setPsram(true);
    canvas_.setColorDepth(kTextColorDepth);
    if (!canvas_.createSprite(kPaperS3Width, kPaperS3Height)) {
        Serial.println("[vink3][runtime] full-screen canvas allocation failed");
        return false;
    }
    canvas_.fillSprite(TFT_WHITE);
    canvasReady_ = true;
    Serial.println("[vink3][runtime] global full-screen canvas ready");
    return true;
}

bool VinkRuntime::beginServices() {
    if (!g_uiRenderer.begin(&canvas_)) return false;
    if (!g_readerText.begin(&canvas_)) return false;
    if (!g_readerBook.begin()) return false;
    if (!g_displayService.begin(&canvas_)) return false;
    if (!g_stateMachine.begin()) return false;
    if (!g_inputService.begin(&g_stateMachine)) return false;
    if (!g_legadoService.begin(&g_stateMachine)) return false;
    return true;
}

void VinkRuntime::drawBoot() {
    g_uiRenderer.renderBoot();
    g_displayService.enqueueFull(true, 100);

    Message bootDone;
    bootDone.type = MessageType::BootComplete;
    bootDone.timestampMs = millis();
    g_stateMachine.post(bootDone, 100);
}

void VinkRuntime::loop() {
    // ReadPaper's main task becomes a lightweight supervisor after services are
    // started. Keep this loop intentionally quiet; state/input/display tasks own work.
    const uint32_t now = millis();
    if (now - lastHeartbeatLogMs_ > 60000) {
        lastHeartbeatLogMs_ = now;
        Serial.printf("[vink3][runtime] state=%u displayPush=%lu freeHeap=%u freePsram=%u\n",
                      static_cast<unsigned>(g_stateMachine.state()),
                      static_cast<unsigned long>(g_displayService.pushCount()),
                      ESP.getFreeHeap(), ESP.getFreePsram());
    }
    delay(1000);
}

M5Canvas* VinkRuntime::canvas() {
    return canvasReady_ ? &canvas_ : nullptr;
}

} // namespace vink3
