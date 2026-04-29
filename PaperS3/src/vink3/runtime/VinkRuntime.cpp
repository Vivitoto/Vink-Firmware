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

uint8_t gPaperS3ActiveDisplayRotation = kPaperS3DisplayRotation;
VinkRuntime g_runtime;

namespace {
void configureOfficialPaperS3Gpios() {
    pinMode(static_cast<int>(kUsbDetectPin), INPUT);
    pinMode(static_cast<int>(kChargeStatePin), INPUT);
    pinMode(static_cast<int>(kBatteryAdcPin), INPUT);
    pinMode(static_cast<int>(kBuzzerPin), OUTPUT);
    digitalWrite(static_cast<int>(kBuzzerPin), LOW);
    analogReadResolution(12);
#if defined(ADC_11db)
    analogSetPinAttenuation(static_cast<int>(kBatteryAdcPin), ADC_11db);
#endif
}

void applyPaperS3PortraitRotation() {
    M5.Display.setRotation(kPaperS3DisplayRotation);
    delay(20);
    gPaperS3ActiveDisplayRotation = kPaperS3DisplayRotation;

    if (M5.Display.width() == kPaperS3Width && M5.Display.height() == kPaperS3Height) {
        return;
    }

    Serial.printf("[vink3][display] preferred rotation %u exposes %dx%d, searching 540x960 portrait\n",
                  kPaperS3DisplayRotation, M5.Display.width(), M5.Display.height());
    for (uint8_t rotation = 0; rotation < 4; ++rotation) {
        M5.Display.setRotation(rotation);
        delay(20);
        if (M5.Display.width() == kPaperS3Width && M5.Display.height() == kPaperS3Height) {
            gPaperS3ActiveDisplayRotation = rotation;
            Serial.printf("[vink3][display] selected rotation=%u for logical %dx%d\n",
                          rotation, M5.Display.width(), M5.Display.height());
            return;
        }
    }

    // Last-resort fallback: restore official baseline and make the mismatch loud
    // in serial logs/diagnostics instead of silently clipping the canvas.
    M5.Display.setRotation(kPaperS3DisplayRotation);
    gPaperS3ActiveDisplayRotation = kPaperS3DisplayRotation;
    Serial.printf("[vink3][display][WARN] no rotation matched %dx%d; active=%u actual=%dx%d\n",
                  kPaperS3Width, kPaperS3Height, gPaperS3ActiveDisplayRotation,
                  M5.Display.width(), M5.Display.height());
}
} // namespace

bool VinkRuntime::begin() {
    Serial.println("[vink3][runtime] starting v0.3.2-rc from ReadPaper V1.7.6 baseline");
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
    Serial.printf("\n[Vink v0.3.2-rc] ReadPaper baseline %s @ %s\n", kReadPaperUpstreamVersion, kReadPaperUpstreamCommit);
    Serial.printf("[vink3][boot] wake cause=%d psram size=%u free=%u flash=%u\n",
                  static_cast<int>(esp_sleep_get_wakeup_cause()),
                  ESP.getPsramSize(), ESP.getFreePsram(), ESP.getFlashChipSize());
    Serial.printf("[vink3][boot] official PaperS3 profile: EPD %dx%d, GT911 SDA=%d SCL=%d INT=%d, SD CS=%d SCK=%d MOSI=%d MISO=%d, BAT_ADC=%d USB_DET=%d CHG=%d BUZZER=%d\n",
                  kPaperS3PhysicalWidth, kPaperS3PhysicalHeight,
                  static_cast<int>(kGt911SdaPin), static_cast<int>(kGt911SclPin), static_cast<int>(kGt911IntPin),
                  kSdCsPin, kSdSckPin, kSdMosiPin, kSdMisoPin,
                  static_cast<int>(kBatteryAdcPin), static_cast<int>(kUsbDetectPin),
                  static_cast<int>(kChargeStatePin), static_cast<int>(kBuzzerPin));

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
    configureOfficialPaperS3Gpios();

    gpio_wakeup_enable(kGt911IntPin, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();

    M5.Display.powerSaveOff();
    M5.Display.wakeup();
    M5.Display.setEpdMode(kLowRefresh);
    M5.Display.setColorDepth(kTextColorDepth);
    // Start from the official touch example baseline, but verify that the active
    // library build really exposes Vink's 540x960 portrait framebuffer.
    applyPaperS3PortraitRotation();
    Serial.printf("[vink3][display] rotation=%u logical=%dx%d actual=%dx%d\n",
                  gPaperS3ActiveDisplayRotation, kPaperS3Width, kPaperS3Height,
                  M5.Display.width(), M5.Display.height());

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
