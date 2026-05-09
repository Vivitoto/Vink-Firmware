#pragma once
#include <Arduino.h>
#include <esp_http_server.h>

namespace vink3 {

class WebUiService {
public:
    int registerHandlers(httpd_handle_t httpd);
    void unregisterHandlers(httpd_handle_t httpd);
};

extern WebUiService g_webUi;

} // namespace vink3
