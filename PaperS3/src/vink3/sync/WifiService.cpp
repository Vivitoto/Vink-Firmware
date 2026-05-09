#include "WifiService.h"
#include "../webui/WebUiService.h"
#include <WiFi.h>

namespace vink3 {

WifiService g_wifiService;

bool WifiService::startAp(const char* ssid, const char* password, bool webUi) {
    if (mode_ == WifiOpMode::ApWebUi && (!webUi || httpServerRunning())) return true;

    stopHttpServer();
    WiFi.mode(WIFI_AP);
    const char* useSsid = (ssid && ssid[0]) ? ssid : "Vink-PaperS3";
    strlcpy(activeSsid_, useSsid, sizeof(activeSsid_));
    const bool hasPassword = password && strlen(password) >= 8;
    bool ok = hasPassword ? WiFi.softAP(useSsid, password) : WiFi.softAP(useSsid);
    if (!ok) {
        Serial.println("[vink3][wifi] softAP start failed");
        WiFi.mode(WIFI_OFF);
        activeSsid_[0] = '\0';
        localIp_ = IPAddress();
        mode_ = WifiOpMode::Off;
        return false;
    }

    localIp_ = WiFi.softAPIP();
    mode_ = WifiOpMode::ApWebUi;
    Serial.printf("[vink3][wifi] AP ready ssid=%s ip=%s\n", activeSsid_, localIp_.toString().c_str());

    if (webUi && !startHttpServer()) {
        stop();
        return false;
    }
    return true;
}

void WifiService::stop() {
    stopHttpServer();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    activeSsid_[0] = '\0';
    localIp_ = IPAddress();
    mode_ = WifiOpMode::Off;
    Serial.println("[vink3][wifi] stopped");
}

bool WifiService::startHttpServer() {
    if (httpd_) return true;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 16;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&httpd_, &config);
    if (err != ESP_OK) {
        Serial.printf("[vink3][webui] httpd_start failed: %d\n", static_cast<int>(err));
        httpd_ = nullptr;
        return false;
    }
    if (g_webUi.registerHandlers(httpd_) != ESP_OK) {
        httpd_stop(httpd_);
        httpd_ = nullptr;
        return false;
    }
    Serial.println("[vink3][webui] HTTP server started");
    return true;
}

void WifiService::stopHttpServer() {
    if (!httpd_) return;
    g_webUi.unregisterHandlers(httpd_);
    httpd_stop(httpd_);
    httpd_ = nullptr;
    Serial.println("[vink3][webui] HTTP server stopped");
}

} // namespace vink3
