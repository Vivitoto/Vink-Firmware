#include "LegadoService.h"
#include "WifiService.h"
#include <HTTPClient.h>
#include <WiFi.h>

namespace vink3 {

// ── Global instance ───────────────────────────────────────────────────────────

LegadoService g_legadoService;

// ── Lifecycle ──────────────────────────────────────────────────────────────────

LegadoService::LegadoService()
    : stateMachine_(nullptr)
    , httpConnected_(false)
    , lastRequestMs_(0) {}

bool LegadoService::begin() {
    Serial.println("[vink3][legado] service initialized");
    return true;
}

void LegadoService::configure(const LegadoConfig& config) {
    config_ = config;
    httpConnected_ = false;
    Serial.printf("[vink3][legado] configured: baseUrl=%s enabled=%d\n",
                  config_.baseUrl.c_str(), config_.enabled);
}

// ── URL builder ───────────────────────────────────────────────────────────────

String LegadoService::buildUrl(const char* endpoint) const {
    String url = config_.baseUrl;
    if (!url.endsWith("/")) url += '/';
    url += endpoint;
    return url;
}

// ── Internal HTTP helpers ──────────────────────────────────────────────────────

namespace {
String s_urlEncode(const String& raw) {
    const char* hex = "0123456789ABCDEF";
    String out;
    out.reserve(raw.length() * 3);
    for (size_t i = 0; i < raw.length(); ++i) {
        char c = raw[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out += c;
        } else if (c == ' ') {
            out += "%20";
        } else {
            out += '%';
            out += hex[(static_cast<unsigned char>(c) >> 4) & 0xF];
            out += hex[static_cast<unsigned char>(c) & 0xF];
        }
    }
    return out;
}
} // anonymous namespace

bool LegadoService::httpGet(const String& url, String& outBody) {
    HTTPClient http;
    http.setConnectTimeout(3000);
    http.setTimeout(8000);
    if (!http.begin(url)) {
        lastError_ = "http.begin failed";
        httpConnected_ = false;
        return false;
    }
    int code = http.GET();
    if (code < 0) {
        lastError_ = http.errorToString(code);
        httpConnected_ = false;
        http.end();
        return false;
    }
    httpConnected_ = (code == 200);
    outBody = http.getString();
    http.end();
    lastRequestMs_ = millis();
    return httpConnected_;
}

bool LegadoService::httpPost(const String& url, const String& body, String& outBody) {
    HTTPClient http;
    http.setConnectTimeout(3000);
    http.setTimeout(8000);
    if (!http.begin(url)) {
        lastError_ = "http.begin failed";
        httpConnected_ = false;
        return false;
    }
    http.addHeader("Content-Type", "application/json");
    if (!config_.token.isEmpty()) {
        http.addHeader("Authorization", config_.token.c_str());
    }
    int code = http.POST((uint8_t*)body.c_str(), body.length());
    if (code < 0) {
        lastError_ = http.errorToString(code);
        httpConnected_ = false;
        http.end();
        return false;
    }
    httpConnected_ = (code >= 200 && code < 300);
    outBody = http.getString();
    http.end();
    lastRequestMs_ = millis();
    return httpConnected_;
}

// ── Public API ────────────────────────────────────────────────────────────────

JsonArray LegadoService::getBookshelf() {
    String body;
    if (!httpGet(buildUrl("getBookshelf"), body)) {
        lastError_ = body.isEmpty() ? lastError_ : body;
        return JsonArray();
    }
    StaticJsonDocument<4096> doc;
    auto err = deserializeJson(doc, body);
    if (err) {
        lastError_ = "JSON parse error";
        return JsonArray();
    }
    JsonVariant data = doc["data"];
    if (data.is<JsonArray>()) {
        httpConnected_ = true;
        return data.as<JsonArray>();
    }
    lastError_ = "Invalid response format";
    return JsonArray();
}

JsonArray LegadoService::getChapterList(const String& bookUrl) {
    char buf[512];
    snprintf(buf, sizeof(buf), "getChapterList?url=%s", s_urlEncode(bookUrl).c_str());
    String body;
    if (!httpGet(buildUrl(buf), body)) return JsonArray();
    StaticJsonDocument<2048> doc;
    auto err = deserializeJson(doc, body);
    if (err) return JsonArray();
    return doc["data"].as<JsonArray>();
}

String LegadoService::getBookContent(const String& bookUrl, int chapterIndex) {
    char buf[512];
    snprintf(buf, sizeof(buf), "getBookContent?url=%s&index=%d",
             s_urlEncode(bookUrl).c_str(), chapterIndex);
    String body;
    if (!httpGet(buildUrl(buf), body)) return String();
    StaticJsonDocument<8192> doc;
    auto err = deserializeJson(doc, body);
    if (err) return String();
    return doc["data"].as<String>();
}

bool LegadoService::saveBookProgress(const BookProgress& progress) {
    if (!config_.isValid()) return false;
    StaticJsonDocument<512> doc;
    JsonObject obj = doc.to<JsonObject>();
    obj["bookUrl"] = progress.name;   // name used as URL key here
    obj["durChapterIndex"] = progress.durChapterIndex;
    obj["durChapterPos"] = progress.durChapterPos;
    obj["durChapterTime"] = progress.durChapterTime;
    obj["durChapterTitle"] = progress.durChapterTitle.c_str();
    String body;
    serializeJson(doc, body);
    String resp;
    if (!httpPost(buildUrl("saveBookProgress"), body, resp)) return false;
    StaticJsonDocument<256> rdoc;
    if (deserializeJson(rdoc, resp) != DeserializationError::Ok) return true;  // optimistic
    JsonObject ro = rdoc.as<JsonObject>();
    return !ro.containsKey("isSuccess") || ro["isSuccess"].as<bool>();
}

bool LegadoService::fetchBookProgress(const String& bookUrl, BookProgress& out) {
    if (bookUrl.isEmpty()) return false;
    char buf[256];
    snprintf(buf, sizeof(buf), "getBookProgress?url=%s", s_urlEncode(bookUrl).c_str());
    String body;
    if (!httpGet(buildUrl(buf), body)) return false;
    StaticJsonDocument<512> doc;
    auto err = deserializeJson(doc, body);
    if (err) return false;
    JsonObject root = doc.as<JsonObject>();
    if (!root.containsKey("isSuccess") || !root["isSuccess"].as<bool>()) return false;
    JsonObject data = root["data"].as<JsonObject>();
    out.name = data.containsKey("name") ? data["name"].as<String>() : String();
    out.author = data.containsKey("author") ? data["author"].as<String>() : String();
    out.durChapterIndex = data.containsKey("durChapterIndex") ? data["durChapterIndex"].as<int>() : 0;
    out.durChapterPos = data.containsKey("durChapterPos") ? data["durChapterPos"].as<int>() : 0;
    out.durChapterTime = data.containsKey("durChapterTime") ? data["durChapterTime"].as<int64_t>() : 0;
    out.durChapterTitle = data.containsKey("durChapterTitle") ? data["durChapterTitle"].as<String>() : String();
    return true;
}

} // namespace vink3