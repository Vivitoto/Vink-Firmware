#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <esp_http_server.h>

namespace vink3 {

// ── WiFi Mode ───────────────────────────────────────────────────────────────

enum class WifiOpMode : uint8_t {
    Off = 0,
    Sta,       // connect to router
    Ap,        // AP hotspot (no server)
    ApWebUi,   // AP + built-in HTTP server on port 80
};

// ── WifiService ──────────────────────────────────────────────────────────────

class WifiService {
public:
    bool begin();
    void stop();

    // STA mode: connect to a router.
    void configureSta(const String& ssid, const String& password);
    bool connectSta();   // non-blocking; use isStaConnected() to poll
    bool isStaConnected() const;

    // AP mode: start a hotspot.
    bool startAp(const String& ssid, const String& password, bool withWebUi);
    bool isApActive() const { return mode_ == WifiOpMode::Ap || mode_ == WifiOpMode::ApWebUi; }

    WifiOpMode mode() const { return mode_; }
    String getLocalIp() const;
    String getActiveSsid() const;   // STA ssid or AP ssid
    bool httpServerRunning() const { return httpServerRunning_; }

    // Web UI server lifecycle.
    bool startHttpServer();
    void stopHttpServer();
    httpd_handle_t httpdHandle() const { return httpd_; }

    // Call periodically from main loop.
    void handleLoop();

private:
    WifiOpMode mode_ = WifiOpMode::Off;
    String staSsid_;
    String staPassword_;
    String apSsid_;
    String apPassword_;
    bool httpServerRunning_ = false;
    httpd_handle_t httpd_ = nullptr;
};

extern WifiService g_wifiService;

} // namespace vink3
