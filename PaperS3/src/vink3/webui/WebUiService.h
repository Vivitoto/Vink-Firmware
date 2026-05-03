#pragma once
#include <Arduino.h>
#include <esp_http_server.h>

namespace vink3 {

class ConfigService;

class WebUiService {
public:
    void begin(ConfigService* configService);

    int registerHandlers(httpd_handle_t httpd);
    void unregisterHandlers(httpd_handle_t httpd);

    // Called by WifiService after startHttpServer() succeeds.
    void onHttpServerStarted();

    // Access config safely from static HTTP handlers.
    const ConfigService* configService() const { return config_; }

private:
    ConfigService* config_ = nullptr;
};

extern WebUiService g_webUi;

namespace webui_private {

enum class FsId : uint8_t { Sd };

// Path utilities
String urlDecode(const String& s);
bool isSafePath(const String& path);
String formatFileSize(size_t bytes);

// FS operations — Web UI file management is SD-only
String listFilesJson(const String& userPath);
bool mkDirRecursive(const String& userPath);
bool deleteFileOrDir(const String& userPath);
bool fileExists(const String& userPath);

// Map a user-facing path to the right FS and real path.
FsId fsForPath(const String& userPath, String& outRealPath);

} // namespace webui_private
} // namespace vink3