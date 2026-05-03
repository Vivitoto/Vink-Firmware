#pragma once
#include <Arduino.h>
#include "../state/StateMachine.h"
#include <ArduinoJson.h>

namespace vink3 {

// ── LegadoConfig ─────────────────────────────────────────────────────────────

struct LegadoConfig {
    String baseUrl;      // e.g. "http://192.168.1.100:1122"
    String token;        // optional auth token
    bool enabled = false;

    bool isValid() const { return !baseUrl.isEmpty(); }
};

// ── BookProgress ─────────────────────────────────────────────────────────────

struct BookProgress {
    String name;
    String author;
    int32_t durChapterIndex = 0;
    int32_t durChapterPos = 0;
    int64_t durChapterTime = 0;
    String durChapterTitle;
};

// ── LegadoService ────────────────────────────────────────────────────────────

class LegadoService {
public:
    LegadoService();  // default constructor — stateMachine set to nullptr

    bool begin();
    void configure(const LegadoConfig& config);

    // Returns true if Legado HTTP service is configured and reachable.
    bool isConfigured() const { return config_.enabled && config_.isValid(); }
    bool isConnected() const { return httpConnected_; }

    // ── HTTP operations (blocking — call from a task) ─────────────────────

    // GET /getBookshelf → count book objects without returning JsonArray views
    // backed by temporary JSON documents.
    bool getBookshelfCount(int& outCount);

    // GET /getChapterList?url=<bookUrl> → count chapters without exposing
    // ArduinoJson views beyond the local document lifetime.
    bool getChapterCount(const String& bookUrl, int& outCount);

    // GET /getBookContent?url=<bookUrl>&index=<chapterIdx> → String (raw text).
    String getBookContent(const String& bookUrl, int chapterIndex);

    // POST /saveBookProgress — push progress; returns false on network error.
    bool saveBookProgress(const BookProgress& progress);

    // Official Legado Web API exposes /getBookshelf but not a standalone
    // /getBookProgress endpoint. Pull progress by matching name/author from
    // the bookshelf Book records.
    bool fetchBookProgress(const String& name, const String& author, BookProgress& out);

    // Returns the last HTTP error string for diagnostics.
    String lastError() const { return lastError_; }

private:
    StateMachine* stateMachine_ = nullptr;
    LegadoConfig config_;
    bool httpConnected_ = false;
    String lastError_;
    uint32_t lastRequestMs_ = 0;

    // Internal: build full URL for an endpoint.
    String buildUrl(const char* endpoint) const;

    // Internal: perform a blocking HTTP GET.
    bool httpGet(const String& url, String& outBody);

    // Internal: POST JSON body.
    bool httpPost(const String& url, const String& body, String& outBody);
};

extern LegadoService g_legadoService;

} // namespace vink3
