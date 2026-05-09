#pragma once
#include <Arduino.h>
#include <IPAddress.h>
#include <esp_http_server.h>

namespace vink3 {

enum class WifiOpMode : uint8_t {
    Off,
    ApWebUi,
};

class WifiService {
public:
    bool startAp(const char* ssid = "Vink-PaperS3", const char* password = nullptr, bool webUi = true);
    void stop();
    bool startHttpServer();
    void stopHttpServer();

    WifiOpMode mode() const { return mode_; }
    bool apRunning() const { return mode_ == WifiOpMode::ApWebUi; }
    bool httpServerRunning() const { return httpd_ != nullptr; }
    IPAddress getLocalIp() const { return localIp_; }
    const char* getActiveSsid() const { return activeSsid_; }

private:
    WifiOpMode mode_ = WifiOpMode::Off;
    httpd_handle_t httpd_ = nullptr;
    IPAddress localIp_{};
    char activeSsid_[32] = {0};
};

extern WifiService g_wifiService;

} // namespace vink3
