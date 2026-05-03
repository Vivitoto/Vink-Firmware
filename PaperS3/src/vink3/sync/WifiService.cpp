#include "WifiService.h"
#include "../webui/WebUiService.h"
#include "../config/ConfigService.h"
#include <SPIFFS.h>
#include <SD.h>
#include <esp_wifi.h>
#include <esp_http_server.h>

namespace vink3 {

WifiService g_wifiService;

// ── Lifecycle ────────────────────────────────────────────────────────────────

bool WifiService::begin() {
    WiFi.mode(WIFI_OFF);

    // Auto-reconnect to saved STA network on every boot.
    // This handles the case where the device reboots (e.g. after a crash or
    // manual restart) and should reconnect to the configured WiFi without
    // requiring the user to open settings and re-save.
    {
        const auto& cfg = g_configService.get();
        if (!cfg.wifiSsid.isEmpty()) {
            configureSta(cfg.wifiSsid, cfg.wifiPassword);
            Serial.printf("[vink3][wifi] saved SSID found, attempting reconnect: %s\n",
                          cfg.wifiSsid.c_str());
            connectSta();
        }
    }

    Serial.println("[vink3][wifi] service initialized");
    return true;
}

void WifiService::stop() {
    stopHttpServer();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    mode_ = WifiOpMode::Off;
    Serial.println("[vink3][wifi] stopped");
}

void WifiService::configureSta(const String& ssid, const String& password) {
    staSsid_ = ssid;
    staPassword_ = password;
}

bool WifiService::connectSta() {
    if (staSsid_.isEmpty()) return false;
    WiFi.mode(WIFI_STA);
    WiFi.begin(staSsid_.c_str(), staPassword_.isEmpty() ? nullptr : staPassword_.c_str());
    mode_ = WifiOpMode::Sta;
    return true;
}

bool WifiService::isStaConnected() const {
    return WiFi.status() == WL_CONNECTED;
}

bool WifiService::startAp(const String& ssid, const String& password, bool withWebUi) {
    stop();

    apSsid_ = ssid.isEmpty() ? "Vink-PaperS3" : ssid;
    apPassword_ = password;

    bool apStarted = WiFi.softAP(apSsid_.c_str(),
                                 apPassword_.isEmpty() ? nullptr : apPassword_.c_str(),
                                 6,
                                 0,
                                 4);
    if (!apStarted) {
        Serial.println("[vink3][wifi] softAP start failed");
        mode_ = WifiOpMode::Off;
        return false;
    }

    mode_ = withWebUi ? WifiOpMode::ApWebUi : WifiOpMode::Ap;

    if (withWebUi && !startHttpServer()) {
        Serial.println("[vink3][wifi] WebUI server start failed");
        WiFi.softAPdisconnect(true);
        mode_ = WifiOpMode::Off;
        return false;
    }

    Serial.printf("[vink3][wifi] AP started: SSID=%s IP=%s\n",
                 apSsid_.c_str(), WiFi.softAPIP().toString().c_str());
    return true;
}

String WifiService::getLocalIp() const {
    if (mode_ == WifiOpMode::Ap || mode_ == WifiOpMode::ApWebUi) {
        return WiFi.softAPIP().toString();
    }
    if (mode_ == WifiOpMode::Sta && isStaConnected()) {
        return WiFi.localIP().toString();
    }
    return String();
}

String WifiService::getActiveSsid() const {
    if (mode_ == WifiOpMode::Sta) return staSsid_;
    if (mode_ == WifiOpMode::Ap || mode_ == WifiOpMode::ApWebUi) return apSsid_;
    return String();
}

// ── HTTP Server ──────────────────────────────────────────────────────────────

namespace {

httpd_handle_t s_httpd = nullptr;

bool findFile(const String& path, size_t& outSize) {
    outSize = 0;
    File f = SD.open(path.c_str(), FILE_READ);
    if (!f) f = SPIFFS.open(path.c_str(), FILE_READ);
    if (!f) return false;
    outSize = f.size();
    f.close();
    return true;
}

esp_err_t rootHandler(httpd_req_t* req) {
    httpd_resp_set_status(req, "301 Moved Permanently");
    httpd_resp_set_hdr(req, "Location", "/index.html");
    return httpd_resp_send(req, nullptr, 0);
}

esp_err_t infoHandler(httpd_req_t* req) {
    char json[384];
    snprintf(json, sizeof(json),
        "{\"ssid\":\"%s\",\"ip\":\"%s\",\"mode\":\"%s\",\"freeHeap\":%u}",
        g_wifiService.getActiveSsid().c_str(),
        g_wifiService.getLocalIp().c_str(),
        g_wifiService.mode() == WifiOpMode::ApWebUi ? "ap-webui" : "ap",
        ESP.getFreeHeap());
    httpd_resp_set_hdr(req, "Content-Type", "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, json, strlen(json));
}

esp_err_t booksHandler(httpd_req_t* req) {
    // deprecated: legacy placeholder, use /api/files or reader library APIs.
    httpd_resp_set_hdr(req, "Content-Type", "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, "{\"books\":[]}", 14);
}

esp_err_t otaHandler(httpd_req_t* req) {
    if (req->method == HTTP_POST) {
        httpd_resp_set_hdr(req, "Content-Type", "application/json");
        return httpd_resp_send(req, "{\"error\":\"OTA not implemented yet\"}", 34);
    }
    httpd_resp_set_hdr(req, "Content-Type", "application/json");
    return httpd_resp_send(req, "{\"error\":\"POST required\"}", 26);
}

const char* mimeFor(const String& path) {
    if (path.endsWith(".txt") || path.endsWith(".TXT")) return "text/plain";
    if (path.endsWith(".html") || path.endsWith(".htm")) return "text/html";
    if (path.endsWith(".json")) return "application/json";
    if (path.endsWith(".epub") || path.endsWith(".EPUB")) return "application/epub+zip";
    if (path.endsWith(".png")) return "image/png";
    if (path.endsWith(".jpg") || path.endsWith(".jpeg")) return "image/jpeg";
    if (path.endsWith(".gif")) return "image/gif";
    return "application/octet-stream";
}

esp_err_t fileHandler(httpd_req_t* req) {
    String path = req->uri[0] == '/' ? String(req->uri + 1) : String(req->uri);
    if (path.endsWith("/")) path += "index.html";

    File f = SD.open(path.c_str(), FILE_READ);
    if (!f) f = SPIFFS.open(path.c_str(), FILE_READ);
    if (!f) {
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_send(req, "File not found", 13);
    }

    size_t fileSize = f.size();
    const char* mime = mimeFor(path);

    // Parse Range header if present.
    char rangeHdr[64];
    size_t rangeStart = 0;
    bool isRange = false;
    if (httpd_req_get_hdr_value_len(req, "Range") > 0 &&
        httpd_req_get_hdr_value_str(req, "Range", rangeHdr, sizeof(rangeHdr)) > 0 &&
        strncmp(rangeHdr, "bytes=", 6) == 0) {
        rangeStart = atoll(rangeHdr + 6);
        isRange = true;
    }

    if (isRange) {
        if (rangeStart >= fileSize) rangeStart = 0;
        size_t rangeEnd = fileSize - 1;
        httpd_resp_set_status(req, "206 Partial Content");
        char ct[64];
        snprintf(ct, sizeof(ct), "%s; ranges=bytes", mime);
        httpd_resp_set_hdr(req, "Content-Type", ct);
        char cr[64];
        snprintf(cr, sizeof(cr), "bytes %zu-%zu/%zu", rangeStart, rangeEnd, fileSize);
        httpd_resp_set_hdr(req, "Content-Range", cr);
        char clen[32];
        snprintf(clen, sizeof(clen), "%zu", rangeEnd - rangeStart + 1);
        httpd_resp_set_hdr(req, "Content-Length", clen);
        httpd_resp_set_hdr(req, "Accept-Ranges", "bytes");
        f.seek(rangeStart);
        uint8_t buf[4096];
        size_t remaining = rangeEnd - rangeStart + 1;
        while (remaining > 0) {
            size_t n = f.read(buf, remaining < sizeof(buf) ? remaining : sizeof(buf));
            if (n == 0) break;
            httpd_resp_send_chunk(req, (char*)buf, n);
            remaining -= n;
        }
        f.close();
        return ESP_OK;
    }

    // Full file
    httpd_resp_set_hdr(req, "Content-Type", mime);
    char clen[32];
    snprintf(clen, sizeof(clen), "%zu", fileSize);
    httpd_resp_set_hdr(req, "Content-Length", clen);
    httpd_resp_set_hdr(req, "Accept-Ranges", "bytes");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    uint8_t buf[8192];
    while (fileSize > 0) {
        size_t n = f.read(buf, fileSize < sizeof(buf) ? fileSize : sizeof(buf));
        if (n == 0) break;
        httpd_resp_send_chunk(req, (char*)buf, n);
        fileSize -= n;
    }
    f.close();
    httpd_resp_send_chunk(req, nullptr, 0);
    return ESP_OK;
}

constexpr httpd_uri_t s_uris[] = {
    { "/",           HTTP_GET, rootHandler },
    { "/api/info",   HTTP_GET, infoHandler },
    { "/api/books",  HTTP_GET, booksHandler },
    { "/api/ota",    HTTP_POST, otaHandler },
    { "/",           HTTP_GET, fileHandler },
};

} // anonymous namespace

bool WifiService::startHttpServer() {
    if (s_httpd) return true;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.max_uri_handlers = 24;
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn = httpd_uri_match_wildcard;

    esp_err_t err = httpd_start(&s_httpd, &cfg);
    if (err != ESP_OK) {
        Serial.printf("[vink3][wifi] httpd_start failed: %d\n", err);
        return false;
    }

    for (const auto& uri : s_uris) {
        httpd_register_uri_handler(s_httpd, &uri);
    }

    // Register Web UI config + file management handlers.
    g_webUi.registerHandlers(s_httpd);

    httpServerRunning_ = true;
    httpd_ = s_httpd;
    Serial.printf("[vink3][wifi] HTTP server started on port %d\n", cfg.server_port);
    return true;
}

void WifiService::stopHttpServer() {
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = nullptr;
    }
    httpServerRunning_ = false;
    httpd_ = nullptr;
}

void WifiService::handleLoop() {
    // esp_http_server handles connections internally; nothing needed here.
    (void)httpd_;
}

} // namespace vink3
