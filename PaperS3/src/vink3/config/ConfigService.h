#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "Config.h"

namespace vink3 {

struct VinkConfig {
    // ── Layout ──────────────────────────────────────────────────
    uint8_t fontSize       = 24;
    uint8_t lineSpacing     = 60;   // percent of fontSize
    uint8_t marginLeft      = 24;
    uint8_t marginRight     = 24;
    uint8_t marginTop       = 20;
    uint8_t marginBottom    = 20;
    bool    justify         = false;

    // ── Refresh ─────────────────────────────────────────────────
    RefreshFrequency refreshFrequency = RefreshFrequency::FREQ_MEDIUM;

    // ── WiFi ────────────────────────────────────────────────────
    String wifiSsid;
    String wifiPassword;

    // ── Legado ──────────────────────────────────────────────────
    String legadoHost;
    uint16_t legadoPort = 1122;
    String legadoToken;
    bool    legadoEnabled = false;

    // ── System ─────────────────────────────────────────────────
    uint8_t autoSleepMinutes = 5;
    bool    autoSleepEnabled = true;
    bool    darkModeDefault  = false;
    bool    verticalTextDefault = false;
    bool    simplifiedChinese = true;  // true=简体 false=繁体
};

class ConfigService {
public:
    // Load from SPIFFS; falls back to defaults if file is absent or corrupt.
    bool begin();

    // Write current config to SPIFFS.
    bool save() const;

    // Getters — all inline for zero overhead.
    const VinkConfig& get() const { return config_; }
    VinkConfig&       mut()       { return config_; }

    // Convenience accessors for the most common keys.
    LayoutConfig      layout() const;
    RefreshStrategy   refreshStrategy() const;
    RefreshFrequency refreshFrequency() const { return config_.refreshFrequency; }

    void setFontSize(uint8_t v);
    void setLineSpacing(uint8_t v);
    void setMargins(uint8_t l, uint8_t r, uint8_t t, uint8_t b);
    void setRefreshFrequency(RefreshFrequency f);
    void setJustify(bool v);
    void setSimplifiedChinese(bool v);
    void setAutoSleep(uint8_t minutes, bool enabled);
    void setWifi(const String& ssid, const String& pass);
    void setLegado(const String& host, uint16_t port, const String& token, bool enabled);

    // Runtime stat snapshot — filled once at begin(), not kept live.
    struct StorageStats {
        size_t totalBytes;
        size_t usedBytes;
        uint32_t uptimeSeconds;
    };
    StorageStats storageStats() const;

private:
    static constexpr const char* kConfigFilePath = "/vink_config.json";
    VinkConfig config_;

    bool loadFromJson(JsonObjectConst obj);

    bool loadFromFile();
};

extern ConfigService g_configService;

} // namespace vink3
