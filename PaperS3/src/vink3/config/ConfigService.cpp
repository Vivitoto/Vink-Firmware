#include "ConfigService.h"
#include <SPIFFS.h>

namespace vink3 {

ConfigService g_configService;

bool ConfigService::begin() {
    if (!SPIFFS.exists(kConfigFilePath)) {
        Serial.println("[vink3][config] no config file — using defaults");
        return true;
    }
    if (!loadFromFile()) {
        Serial.println("[vink3][config] load failed — using defaults");
    }
    return true;
}

bool ConfigService::loadFromFile() {
    File f = SPIFFS.open(kConfigFilePath, FILE_READ);
    if (!f) return false;
    StaticJsonDocument<4096> doc;
    auto err = deserializeJson(doc, f);
    f.close();
    if (err) {
        Serial.printf("[vink3][config] JSON parse error %s\n", err.c_str());
        return false;
    }
    return loadFromJson(doc.as<JsonObjectConst>());
}

bool ConfigService::loadFromJson(JsonObjectConst obj) {
    // Layout
    if (obj.containsKey("fontSize"))       config_.fontSize      = constrain(obj["fontSize"].as<uint8_t>(), 12, 48);
    if (obj.containsKey("fontIndex"))      config_.fontIndex     = constrain(obj["fontIndex"].as<uint8_t>(), 0, 31);
    if (obj.containsKey("lineSpacing"))    config_.lineSpacing   = constrain(obj["lineSpacing"].as<uint8_t>(), 30, 200);
    if (obj.containsKey("marginLeft"))     config_.marginLeft    = constrain(obj["marginLeft"].as<uint8_t>(), 0, 120);
    if (obj.containsKey("marginRight"))    config_.marginRight   = constrain(obj["marginRight"].as<uint8_t>(), 0, 120);
    if (obj.containsKey("marginTop"))      config_.marginTop     = constrain(obj["marginTop"].as<uint8_t>(), 0, 160);
    if (obj.containsKey("marginBottom"))   config_.marginBottom  = constrain(obj["marginBottom"].as<uint8_t>(), 0, 160);
    if (obj.containsKey("paragraphSpacing")) config_.paragraphSpacing = constrain(obj["paragraphSpacing"].as<uint8_t>(), 0, 100);
    if (obj.containsKey("indentFirstLine"))  config_.indentFirstLine  = constrain(obj["indentFirstLine"].as<uint8_t>(), 0, 4);
    if (obj.containsKey("justify"))       config_.justify       = obj["justify"].as<bool>();

    // Refresh
    if (obj.containsKey("refreshFrequency")) {
        config_.refreshFrequency = static_cast<RefreshFrequency>(
            obj["refreshFrequency"].as<uint8_t>() % 3);
    }

    // WiFi
    if (obj.containsKey("wifiSsid"))     config_.wifiSsid     = obj["wifiSsid"].as<String>();
    if (obj.containsKey("wifiPassword")) config_.wifiPassword = obj["wifiPassword"].as<String>();

    // Legado
    if (obj.containsKey("legadoHost"))    config_.legadoHost    = obj["legadoHost"].as<String>();
    if (obj.containsKey("legadoPort"))    config_.legadoPort    = constrain(obj["legadoPort"].as<uint16_t>(), 1, 65535);
    if (obj.containsKey("legadoToken"))   config_.legadoToken   = obj["legadoToken"].as<String>();
    if (obj.containsKey("legadoEnabled")) config_.legadoEnabled = obj["legadoEnabled"].as<bool>();

    // System
    if (obj.containsKey("autoSleepMinutes")) config_.autoSleepMinutes = constrain(obj["autoSleepMinutes"].as<uint8_t>(), 0, 60);
    if (obj.containsKey("autoSleepEnabled"))  config_.autoSleepEnabled  = obj["autoSleepEnabled"].as<bool>();
    if (obj.containsKey("darkModeDefault"))   config_.darkModeDefault   = obj["darkModeDefault"].as<bool>();
    if (obj.containsKey("verticalTextDefault")) config_.verticalTextDefault = obj["verticalTextDefault"].as<bool>();
    if (obj.containsKey("simplifiedChinese"))  config_.simplifiedChinese = obj["simplifiedChinese"].as<bool>();

    Serial.println("[vink3][config] loaded from JSON");
    return true;
}

bool ConfigService::save() const {
    StaticJsonDocument<4096> doc;
    JsonObject obj = doc.to<JsonObject>();

    // Layout
    obj["fontSize"]      = config_.fontSize;
    obj["fontIndex"]     = config_.fontIndex;
    obj["lineSpacing"]   = config_.lineSpacing;
    obj["marginLeft"]    = config_.marginLeft;
    obj["marginRight"]   = config_.marginRight;
    obj["marginTop"]     = config_.marginTop;
    obj["marginBottom"]  = config_.marginBottom;
    obj["paragraphSpacing"] = config_.paragraphSpacing;
    obj["indentFirstLine"]  = config_.indentFirstLine;
    obj["justify"]       = config_.justify;

    // Refresh
    obj["refreshFrequency"] = static_cast<uint8_t>(config_.refreshFrequency);

    // WiFi
    obj["wifiSsid"]     = config_.wifiSsid.c_str();
    obj["wifiPassword"] = config_.wifiPassword.c_str();

    // Legado
    obj["legadoHost"]    = config_.legadoHost.c_str();
    obj["legadoPort"]    = config_.legadoPort;
    obj["legadoToken"]   = config_.legadoToken.c_str();
    obj["legadoEnabled"] = config_.legadoEnabled;

    // System
    obj["autoSleepMinutes"] = config_.autoSleepMinutes;
    obj["autoSleepEnabled"] = config_.autoSleepEnabled;
    obj["darkModeDefault"]  = config_.darkModeDefault;
    obj["verticalTextDefault"] = config_.verticalTextDefault;
    obj["simplifiedChinese"] = config_.simplifiedChinese;

    File f = SPIFFS.open(kConfigFilePath, FILE_WRITE);
    if (!f) {
        Serial.println("[vink3][config] save: could not open file for write");
        return false;
    }
    size_t written = serializeJson(doc, f);
    f.close();
    Serial.printf("[vink3][config] saved %u bytes\n", written);
    return written > 0;
}

LayoutConfig ConfigService::layout() const {
    LayoutConfig lc;
    lc.fontSize         = config_.fontSize;
    lc.lineSpacing      = config_.lineSpacing;
    lc.marginLeft       = config_.marginLeft;
    lc.marginRight      = config_.marginRight;
    lc.marginTop        = config_.marginTop;
    lc.marginBottom     = config_.marginBottom;
    lc.justify          = config_.justify;
    lc.indentFirstLine  = config_.indentFirstLine;
    lc.paragraphSpacing = config_.paragraphSpacing;
    return lc;
}

// ── Mutators ──────────────────────────────────────────────────────────────────

void ConfigService::setFontSize(uint8_t v) {
    config_.fontSize = (v < 12) ? 12 : (v > 48) ? 48 : v;
}

void ConfigService::setFontIndex(uint8_t v) {
    config_.fontIndex = v;
}

void ConfigService::setLineSpacing(uint8_t v) {
    config_.lineSpacing = (v < 30) ? 30 : (v > 200) ? 200 : v;
}

void ConfigService::setMargins(uint8_t l, uint8_t r, uint8_t t, uint8_t b) {
    config_.marginLeft   = l;
    config_.marginRight  = r;
    config_.marginTop   = t;
    config_.marginBottom = b;
}

void ConfigService::setRefreshFrequency(RefreshFrequency f) {
    config_.refreshFrequency = f;
}

void ConfigService::setJustify(bool v) {
    config_.justify = v;
}

void ConfigService::setSimplifiedChinese(bool v) {
    config_.simplifiedChinese = v;
}

void ConfigService::setAutoSleep(uint8_t minutes, bool enabled) {
    config_.autoSleepMinutes = (minutes < 1) ? 1 : (minutes > 60) ? 60 : minutes;
    config_.autoSleepEnabled = enabled;
}

void ConfigService::setWifi(const String& ssid, const String& pass) {
    config_.wifiSsid     = ssid;
    config_.wifiPassword = pass;
}

void ConfigService::setLegado(const String& host, uint16_t port, const String& token, bool enabled) {
    config_.legadoHost    = host;
    config_.legadoPort   = port;
    config_.legadoToken  = token;
    config_.legadoEnabled = enabled;
}

// ── Storage stats ─────────────────────────────────────────────────────────────

ConfigService::StorageStats ConfigService::storageStats() const {
    StorageStats s{};
    s.totalBytes    = SPIFFS.totalBytes();
    s.usedBytes     = SPIFFS.usedBytes();
    s.uptimeSeconds = millis() / 1000;
    return s;
}

} // namespace vink3
