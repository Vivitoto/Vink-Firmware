#pragma once
#include <Arduino.h>
#include <M5Unified.h>
#include <driver/gpio.h>

namespace vink3 {

static constexpr const char* kVinkPaperS3FirmwareVersion = "v0.3.13-rc-performance";

// Upstream baseline used for the v0.3.0 rewrite.
// Source: https://github.com/shinemoon/M5ReadPaper main @ e910d29 (data/version: V1.7.6)
static constexpr const char* kReadPaperUpstreamRepo = "shinemoon/M5ReadPaper";
static constexpr const char* kReadPaperUpstreamCommit = "e910d29";
static constexpr const char* kReadPaperUpstreamVersion = "V1.7.6";

// Official M5Stack PaperS3 hardware profile.
// Source: http://docs.m5stack.com/zh_CN/core/PaperS3
static constexpr int16_t kPaperS3Width = 540;   // official touch-example portrait width at rotation 0
static constexpr int16_t kPaperS3Height = 960;  // official touch-example portrait height at rotation 0
static constexpr int16_t kPaperS3PhysicalWidth = 960;
static constexpr int16_t kPaperS3PhysicalHeight = 540;
static constexpr uint8_t kPaperS3DisplayRotation = 0; // official M5PaperS3 touch-example baseline
extern uint8_t gPaperS3ActiveDisplayRotation;

enum class TouchCoordMode : uint8_t {
    OfficialRaw540x960 = 0,
};
extern volatile TouchCoordMode gPaperS3TouchCoordMode;

static constexpr uint8_t kTextColorDepth = 4;
static constexpr uint8_t kTextColorDepthHigh = 16;

static constexpr gpio_num_t kGt911SdaPin = GPIO_NUM_41;
static constexpr gpio_num_t kGt911SclPin = GPIO_NUM_42;
static constexpr gpio_num_t kGt911IntPin = GPIO_NUM_48;
static constexpr uint8_t kGt911PrimaryAddress = 0x14;
static constexpr uint8_t kGt911AltAddress = 0x5D;

static constexpr int kSdCsPin = 47;
static constexpr int kSdSckPin = 39;
static constexpr int kSdMosiPin = 38;
static constexpr int kSdMisoPin = 40;
static constexpr uint32_t kSdPrimaryFrequency = 25000000UL;
static constexpr uint32_t kSdFallbackFrequency1 = 8000000UL;
static constexpr uint32_t kSdFallbackFrequency2 = 4000000UL;

static constexpr gpio_num_t kBatteryAdcPin = GPIO_NUM_3;
static constexpr gpio_num_t kChargeStatePin = GPIO_NUM_4; // factory firmware: 0 charging, 1 full/not charging
static constexpr gpio_num_t kUsbDetectPin = GPIO_NUM_5;   // factory firmware: 1 USB-IN
static constexpr gpio_num_t kBuzzerPin = GPIO_NUM_21;
static constexpr gpio_num_t kLegacyM5PaperTouchIntPin = GPIO_NUM_36; // not PaperS3 power key; kept only as an audit note
static constexpr gpio_num_t kPowerOffPulsePin = GPIO_NUM_44; // PMIC power-off pulse used by PaperS3 references
static constexpr gpio_num_t kEpdPowerPin = GPIO_NUM_45;

static constexpr uint32_t kDisplayMiddleRefreshThreshold = 8;
static constexpr uint32_t kDisplayQualityFastThreshold = 18;
static constexpr uint32_t kDisplayFullRefreshNormalThreshold = 24;

static constexpr epd_mode_t kQualityRefresh = epd_mode_t::epd_quality;
static constexpr epd_mode_t kMiddleRefresh = epd_mode_t::epd_fast;
static constexpr epd_mode_t kNormalRefresh = epd_mode_t::epd_text;
static constexpr epd_mode_t kLowRefresh = epd_mode_t::epd_fastest;

// ── Colour constants (e-paper safe palette) ──────────────────────────────────
// Background / canvas clear colour for the 4-bit greyscale panel.
// 0x7BEF is the neutral mid-grey used throughout ReadPaper 1.7.6 — sits
// cleanly on unheated e-ink without ghosting.
static constexpr uint16_t COLOR_BG = 0x7BEF;

} // namespace vink3
