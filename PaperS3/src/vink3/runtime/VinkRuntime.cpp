#include "VinkRuntime.h"
#include "../display/DisplayService.h"
#include "../input/InputService.h"
#include "../reader/ReaderBookService.h"
#include "../reader/ReaderTextRenderer.h"
#include "../state/StateMachine.h"
#include "../sync/LegadoService.h"
#include "../system/SystemLog.h"
#include "../ui/VinkUiRenderer.h"
#include <SPIFFS.h>
#include <SD.h>
#include "esp_sleep.h"
#include "esp_system.h"
#include "driver/gpio.h"

namespace vink3 {

uint8_t gPaperS3ActiveDisplayRotation = kPaperS3DisplayRotation;
volatile TouchCoordMode gPaperS3TouchCoordMode = TouchCoordMode::OfficialRaw540x960;
VinkRuntime g_runtime;

namespace {
RTC_NOINIT_ATTR uint32_t s_runtimeRunningMagic;
RTC_NOINIT_ATTR uint32_t s_runtimeRunningMagicInv;
RTC_NOINIT_ATTR uint32_t s_softwareLockMagic;
RTC_NOINIT_ATTR uint32_t s_softwareLockMagicInv;
static constexpr uint32_t kRuntimeRunningMagic = 0x56494E4Bu; // "VINK"
static constexpr uint32_t kSoftwareLockMagic = 0x564C4F43u; // "VLOC"
bool s_sideKeyUnlockRequested = false;

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

void applyOfficialPaperS3DisplaySetup() {
    // Official M5PaperS3 touch example baseline: M5.begin(); Display.setRotation(0),
    // then M5.update() + M5.Touch.getDetail() with raw x/y used directly.
    // Keep Vink's UI in the PaperS3 portrait geometry exposed by this rotation.
    M5.Display.setRotation(kPaperS3DisplayRotation);
    gPaperS3ActiveDisplayRotation = kPaperS3DisplayRotation;
}

} // namespace

void markPaperS3RuntimeRunning() {
    s_runtimeRunningMagic = kRuntimeRunningMagic;
    s_runtimeRunningMagicInv = ~kRuntimeRunningMagic;
}

void clearPaperS3RuntimeRunning() {
    s_runtimeRunningMagic = 0;
    s_runtimeRunningMagicInv = 0;
}

bool wasPaperS3RuntimeRunningBeforeReset() {
    return s_runtimeRunningMagic == kRuntimeRunningMagic &&
           s_runtimeRunningMagicInv == ~kRuntimeRunningMagic;
}

void markPaperS3SoftwareLocked() {
    s_softwareLockMagic = kSoftwareLockMagic;
    s_softwareLockMagicInv = ~kSoftwareLockMagic;
}

void clearPaperS3SoftwareLocked() {
    s_softwareLockMagic = 0;
    s_softwareLockMagicInv = 0;
}

bool wasPaperS3SoftwareLockedBeforeReset() {
    return s_softwareLockMagic == kSoftwareLockMagic &&
           s_softwareLockMagicInv == ~kSoftwareLockMagic;
}

void markPaperS3SideKeyUnlockRequested() {
    s_sideKeyUnlockRequested = true;
}

bool isPaperS3SideKeyUnlockRequested() {
    return s_sideKeyUnlockRequested;
}

bool consumePaperS3SideKeyUnlockRequested() {
    const bool requested = s_sideKeyUnlockRequested;
    s_sideKeyUnlockRequested = false;
    return requested;
}

bool VinkRuntime::begin() {
    Serial.printf("[vink3][runtime] starting %s from ReadPaper V1.7.6 baseline\n", kVinkPaperS3FirmwareVersion);
    if (!beginHardware()) return false;
    if (!beginCanvas()) return false;
    if (handleSideKeyResetShutdown()) return false;
    if (!beginServices()) return false;
    drawBoot();
    markPaperS3RuntimeRunning();
    return true;
}

bool VinkRuntime::beginHardware() {
    if (hardwareReady_) return true;

    Serial.begin(115200);
    delay(200);
    Serial.printf("\n[Vink %s] ReadPaper baseline %s @ %s\n", kVinkPaperS3FirmwareVersion, kReadPaperUpstreamVersion, kReadPaperUpstreamCommit);
    const int resetReason = static_cast<int>(esp_reset_reason());
    const int wakeCause = static_cast<int>(esp_sleep_get_wakeup_cause());
    const bool priorRunning = wasPaperS3RuntimeRunningBeforeReset();
    Serial.printf("[vink3][boot] reset reason=%d wake cause=%d prior-running=%d psram size=%u free=%u flash=%u\n",
                  resetReason, wakeCause, priorRunning ? 1 : 0,
                  ESP.getPsramSize(), ESP.getFreePsram(), ESP.getFlashChipSize());
    g_systemLog.appendf("boot reset=%d wake=%d prior=%d", resetReason, wakeCause, priorRunning ? 1 : 0);
    g_systemLog.appendf("boot fw=%s heap=%u psram=%u", kVinkPaperS3FirmwareVersion, ESP.getFreeHeap(), ESP.getFreePsram());
    Serial.printf("[vink3][boot] official PaperS3 profile: EPD %dx%d, GT911 SDA=%d SCL=%d INT=%d, SD CS=%d SCK=%d MOSI=%d MISO=%d, BAT_ADC=%d USB_DET=%d CHG=%d BUZZER=%d\n",
                  kPaperS3PhysicalWidth, kPaperS3PhysicalHeight,
                  static_cast<int>(kGt911SdaPin), static_cast<int>(kGt911SclPin), static_cast<int>(kGt911IntPin),
                  kSdCsPin, kSdSckPin, kSdMosiPin, kSdMisoPin,
                  static_cast<int>(kBatteryAdcPin), static_cast<int>(kUsbDetectPin),
                  static_cast<int>(kChargeStatePin), static_cast<int>(kBuzzerPin));

    auto cfg = M5.config();
    // Vink draws a boot page immediately after M5.begin(), so the library's
    // default e-paper clear_display (white→black→white, two full refreshes)
    // is unnecessary flash that the user sees as extra screen flicker. Disable
    // it to shorten the visible startup sequence.
    cfg.clear_display = false;
    M5.begin(cfg);
    delay(50);
    configureOfficialPaperS3Gpios();

    M5.Display.setEpdMode(kQualityRefresh);
    M5.Display.setColorDepth(kTextColorDepthHigh);
    applyOfficialPaperS3DisplaySetup();
    Serial.printf("[vink3][display] official touch rotation=%u expected=%dx%d actual=%dx%d\n",
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
    canvas_.setColorDepth(kTextColorDepthHigh);
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

bool VinkRuntime::handleSideKeyResetShutdown() {
    // On PaperS3 the side key is not exposed as a normal ESP32-S3 GPIO button
    // in M5Unified. On real devices a running-device side-key click can instead
    // reset the ESP32. EDCBook handles this by software state: if the device was
    // fake-locked, the reset means "unlock/resume"; otherwise it emulates a
    // side-key shutdown by drawing the retained power-off page and pulsing GPIO44.
    if (esp_reset_reason() != ESP_RST_EXT || !wasPaperS3RuntimeRunningBeforeReset()) return false;

    if (wasPaperS3SoftwareLockedBeforeReset()) {
        Serial.println("[vink3][power] external reset while software-locked -> side-key unlock/resume");
        g_systemLog.append("side-key reset while locked -> unlock");
        clearPaperS3SoftwareLocked();
        markPaperS3SideKeyUnlockRequested();
        return false;
    }

    Serial.println("[vink3][power] external reset after running session -> side-key shutdown path");
    g_systemLog.append("side-key external reset -> shutdown");
    if (!g_uiRenderer.begin(&canvas_)) return false;
    if (!g_displayService.begin(&canvas_)) return false;
    g_uiRenderer.renderPowerOffReady();
    g_displayService.enqueueFull(true, 100);
    g_displayService.waitIdle(8000);
    M5.Display.waitDisplay();
    delay(800);
    clearPaperS3RuntimeRunning();
    Serial.println("[vink3][power] side-key reset page drawn; pulsing PaperS3 GPIO44 PWROFF");
    g_systemLog.append("side-key page drawn; GPIO44 pulse");
    Serial.flush();
    pulsePaperS3PowerOffPin();

    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_OFF);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_SLOW_MEM, ESP_PD_OPTION_OFF);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_FAST_MEM, ESP_PD_OPTION_OFF);
    Serial.println("[vink3][power] side-key GPIO44 pulse returned; entering fallback ESP32-S3 deep sleep");
    Serial.flush();
    delay(100);
    esp_deep_sleep_start();
    for (;;) delay(1000);
}

void VinkRuntime::drawBoot() {
    // Side-key reset while software-locked is an EDCBook-style resume path.
    // Do not flash the normal boot page first; leave the retained lock/reader
    // image on the EPD until BootComplete restores the reader page.
    if (!isPaperS3SideKeyUnlockRequested()) {
        g_uiRenderer.renderBoot();
        g_displayService.enqueueFull(true, 100);
        g_displayService.waitIdle(3000);
        delay(600);
    } else {
        Serial.println("[vink3][boot] skipping boot page for side-key unlock resume");
        g_systemLog.append("skip boot page: side-key unlock");
    }

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
