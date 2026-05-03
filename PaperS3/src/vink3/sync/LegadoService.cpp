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
    if (!config_.token.isEmpty()) {
        http.addHeader("Authorization", config_.token.c_str());
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

bool LegadoService::getBookshelfCount(int& outCount) {
    outCount = 0;
    String body;
    if (!httpGet(buildUrl("getBookshelf"), body)) {
        lastError_ = body.isEmpty() ? lastError_ : body;
        return false;
    }
    StaticJsonDocument<4096> doc;
    auto err = deserializeJson(doc, body);
    if (err) {
        lastError_ = "JSON parse error";
        return false;
    }
    JsonVariant data = doc["data"];
    if (data.is<JsonArray>()) {
        outCount = data.as<JsonArray>().size();
        httpConnected_ = true;
        return true;
    }
    if (doc.is<JsonArray>()) {
        outCount = doc.as<JsonArray>().size();
        httpConnected_ = true;
        return true;
    }
    lastError_ = "Invalid response format";
    return false;
}

bool LegadoService::getChapterCount(const String& bookUrl, int& outCount) {
    outCount = 0;
    char buf[512];
    snprintf(buf, sizeof(buf), "getChapterList?url=%s", s_urlEncode(bookUrl).c_str());
    String body;
    if (!httpGet(buildUrl(buf), body)) return false;
    StaticJsonDocument<2048> doc;
    auto err = deserializeJson(doc, body);
    if (err) {
        lastError_ = "JSON parse error";
        return false;
    }
    JsonVariant data = doc["data"];
    if (data.is<JsonArray>()) {
        outCount = data.as<JsonArray>().size();
        return true;
    }
    if (doc.is<JsonArray>()) {
        outCount = doc.as<JsonArray>().size();
        return true;
    }
    lastError_ = "Invalid response format";
    return false;
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
    // Official Legado BookProgress.kt fields. The Android Web controller parses
    // this object, finds the local book by name + author, updates durChapter*,
    // then calls AppWebDav.uploadBookProgress when Legado's own WebDAV sync is
    // enabled. No separate Vink-side WebDAV server is required.
    obj["name"] = progress.name.c_str();
    obj["author"] = progress.author.c_str();
    obj["durChapterIndex"] = progress.durChapterIndex;
    obj["durChapterPos"] = progress.durChapterPos;
    obj["durChapterTime"] = progress.durChapterTime;
    obj["durChapterTitle"] = progress.durChapterTitle.c_str();
    String body;
    serializeJson(doc, body);
    String resp;
    if (!httpPost(buildUrl("saveBookProgress"), body, resp)) return false;
    StaticJsonDocument<256> rdoc;
    if (deserializeJson(rdoc, resp) != DeserializationError::Ok) return true;  // optimistic for old builds
    JsonObject ro = rdoc.as<JsonObject>();
    if (ro.containsKey("isSuccess") && !ro["isSuccess"].as<bool>()) {
        lastError_ = ro.containsKey("errorMsg") ? ro["errorMsg"].as<String>() : String("saveBookProgress failed");
        return false;
    }
    return true;
}

bool LegadoService::fetchBookProgress(const String& name, const String& author, BookProgress& out) {
    if (name.isEmpty()) return false;
    String body;
    if (!httpGet(buildUrl("getBookshelf"), body)) return false;
    DynamicJsonDocument doc(16384);
    auto err = deserializeJson(doc, body);
    if (err) {
        lastError_ = "JSON parse error";
        return false;
    }
    JsonVariant data = doc["data"];
    JsonArray books;
    if (data.is<JsonArray>()) {
        books = data.as<JsonArray>();
    } else if (doc.is<JsonArray>()) {
        books = doc.as<JsonArray>();
    } else {
        lastError_ = "Invalid bookshelf response";
        return false;
    }
    for (JsonObject book : books) {
        const String bookName = book["name"] | "";
        const String bookAuthor = book["author"] | "";
        if (bookName != name) continue;
        if (!author.isEmpty() && bookAuthor != author) continue;
        out.name = bookName;
        out.author = bookAuthor;
        out.durChapterIndex = book["durChapterIndex"] | 0;
        out.durChapterPos = book["durChapterPos"] | 0;
        out.durChapterTime = book["durChapterTime"] | 0;
        out.durChapterTitle = book["durChapterTitle"] | "";
        httpConnected_ = true;
        return true;
    }
    lastError_ = "Book not found in Legado bookshelf";
    return false;
}

} // namespace vink3