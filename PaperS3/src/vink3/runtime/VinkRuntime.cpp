#include "VinkRuntime.h"
#include "../config/ConfigService.h"
#include "../display/DisplayService.h"
#include "../sync/LegadoService.h"
#include "../sync/WifiService.h"
#include "../input/InputService.h"
#include "../reader/ReaderBookService.h"
#include "../reader/ReaderTextRenderer.h"
#include "../state/StateMachine.h"
#include "../ui/VinkUiRenderer.h"
#include "../../FontManager.h"
#include "../webui/WebUiService.h"
#include <SPIFFS.h>
#include <SD.h>
#include "esp_sleep.h"
#include "driver/gpio.h"

namespace vink3 {

uint8_t gPaperS3ActiveDisplayRotation = kPaperS3DisplayRotation;
volatile TouchCoordMode gPaperS3TouchCoordMode = TouchCoordMode::OfficialRaw540x960;
VinkRuntime g_runtime;

namespace {
String buildLegadoBaseUrlForRuntime(const VinkConfig& cfg) {
    String base = cfg.legadoHost;
    base.trim();
    if (base.isEmpty()) return base;
    if (!base.startsWith("http://") && !base.startsWith("https://")) {
        base = "http://" + base;
    }
    const int scheme = base.indexOf("://");
    const int hostStart = scheme >= 0 ? scheme + 3 : 0;
    int slash = base.indexOf('/', hostStart);
    if (slash < 0) slash = base.length();
    const String hostPort = base.substring(hostStart, slash);
    if (hostPort.indexOf(':') < 0 && cfg.legadoPort > 0) {
        base = base.substring(0, slash) + ":" + String(cfg.legadoPort) + base.substring(slash);
    }
    return base;
}

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

void drawOfficialBootProbe() {
    // A+B+D: use epd_text LUT for boot probe — crisper text rendering
    M5.Display.setEpdMode(epd_mode_t::epd_text);
    M5.Display.setTextDatum(middle_center);
    M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
    M5.Display.fillScreen(TFT_WHITE);
    delay(200);
    M5.Display.drawString("Vink PaperS3 official boot", M5.Display.width() / 2, M5.Display.height() / 2 - 28);
    M5.Display.drawString("M5.begin + rotation 0", M5.Display.width() / 2, M5.Display.height() / 2 + 18);
    M5.Display.waitDisplay();
    delay(800);
}
} // namespace

bool VinkRuntime::begin() {
    Serial.printf("[vink3][runtime] starting %s from ReadPaper V1.7.6 baseline\n", kVinkPaperS3FirmwareVersion);
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
    Serial.printf("\n[Vink %s] ReadPaper baseline %s @ %s\n", kVinkPaperS3FirmwareVersion, kReadPaperUpstreamVersion, kReadPaperUpstreamCommit);
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
    // Official M5PaperS3-UserDemo uses the default M5.begin() path. Keep this
    // path strict and visible; no fallback-board override, no clear_display
    // override, no ReadPaper-style startup masking.
    (void)cfg;
    M5.begin();
    delay(50);
    // Disable M5Unified default hold detection so InputService owns power-button logic exclusively.
    M5.BtnPWR.setDebounceThresh(0);
    M5.BtnPWR.setHoldThresh(0);
    configureOfficialPaperS3Gpios();

    M5.Display.setEpdMode(kQualityRefresh);
    M5.Display.setColorDepth(kTextColorDepthHigh);
    applyOfficialPaperS3DisplaySetup();
    Serial.printf("[vink3][display] official touch rotation=%u expected=%dx%d actual=%dx%d\n",
                  gPaperS3ActiveDisplayRotation, kPaperS3Width, kPaperS3Height,
                  M5.Display.width(), M5.Display.height());
    drawOfficialBootProbe();

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
    g_configService.begin();
    // Keep boot SD-free. ReaderBookService intentionally initializes SD lazily;
    // scanning/loading SD fonts here can wedge PaperS3 during startup with some
    // cards inserted, leaving the boot probe repeatedly refreshed by reset loops.
    // ReaderTextRenderer::begin() below loads the bundled ReadPaper PROGMEM font;
    // SD fonts remain available from the layout/settings path after boot.
    Serial.println("[vink3][runtime] boot font scan skipped; using default reader font");
    g_webUi.begin(&g_configService);
    if (!g_uiRenderer.begin(&canvas_)) return false;
    if (!g_readerText.begin(&canvas_)) return false;
    if (!g_readerBook.begin()) return false;
    if (!g_displayService.begin(&canvas_)) return false;
    if (!g_stateMachine.begin()) return false;
    if (!g_inputService.begin(&g_stateMachine)) return false;
    if (!g_wifiService.begin()) return false;
    if (!g_legadoService.begin()) return false;
    {
        const auto& cfg = g_configService.get();
        if (cfg.legadoEnabled && !cfg.legadoHost.isEmpty()) {
            LegadoConfig lc;
            lc.baseUrl = buildLegadoBaseUrlForRuntime(cfg);
            lc.token = cfg.legadoToken;
            lc.enabled = cfg.legadoEnabled;
            g_legadoService.configure(lc);
        }
    }
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

    // Auto-sleep: check idle timeout every loop tick.
    const auto& cfg = g_configService.get();
    if (cfg.autoSleepEnabled && cfg.autoSleepMinutes > 0) {
        const uint32_t idleMs = now - g_stateMachine.lastActivityMs();
        if (idleMs >= static_cast<uint32_t>(cfg.autoSleepMinutes) * 60000) {
            Message msg;
            msg.type = MessageType::SleepTimeout;
            msg.timestampMs = now;
            g_stateMachine.post(msg, 0);
        }
    }

    delay(1000);
}

M5Canvas* VinkRuntime::canvas() {
    return canvasReady_ ? &canvas_ : nullptr;
}

} // namespace vink3
