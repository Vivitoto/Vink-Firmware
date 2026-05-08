#include "WebUiService.h"
#include "../config/ConfigService.h"
#include "../VinkPaperS3Core.h"
#include <SPIFFS.h>
#include <SD.h>
#include <esp_timer.h>

namespace vink3 {
namespace webui_private {

// ── Singleton accessor for static HTTP handlers ──────────────────────────────

static WebUiService* s_instance = nullptr;

void setInstance(WebUiService* inst) { s_instance = inst; }

// ── Path utilities ───────────────────────────────────────────────────────────

String urlDecode(const String& s) {
    String out;
    out.reserve(s.length());
    for (size_t i = 0; i < s.length(); ++i) {
        if (s[i] == '%' && i + 2 < s.length()) {
            auto hex2int = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'A' && c <= 'F') return 10 + c - 'A';
                if (c >= 'a' && c <= 'f') return 10 + c - 'a';
                return 0;
            };
            out += static_cast<char>(
                hex2int(s[i + 1]) * 16 + hex2int(s[i + 2]));
            i += 2;
        } else if (s[i] == '+') {
            out += ' ';
        } else {
            out += s[i];
        }
    }
    return out;
}

bool hasUnsafePathChars(const String& path) {
    for (size_t i = 0; i < path.length(); ++i) {
        if (static_cast<uint8_t>(path[i]) < 0x20 || path[i] == '\\') return true;
    }
    return false;
}

String normalizeUserPath(String path) {
    if (path.isEmpty()) path = "/";
    if (!path.startsWith("/")) path = "/" + path;
    while (path.indexOf("//") >= 0) path.replace("//", "/");
    int q = path.indexOf('?');
    if (q >= 0) path = path.substring(0, q);
    if (path.length() > 1 && path.endsWith("/")) path.remove(path.length() - 1);
    return path;
}

bool isSdRootPath(const String& path) {
    return normalizeUserPath(path) == "/";
}

String sdPathForUserPath(const String& userPath) {
    // Web UI file management is SD-only; never expose SPIFFS/system partition.
    return normalizeUserPath(userPath);
}

bool isSafePath(const String& path) {
    if (path.isEmpty()) return true;
    if (hasUnsafePathChars(path)) return false;
    if (path.indexOf("..") >= 0) return false;
    return true;
}

bool queryParamValue(const char* uri, const char* key, String& out) {
    // Do not match unrelated keys such as xpath when looking for path.
    const char* q = strchr(uri, '?');
    if (!q) return false;
    String needle = String(key) + "=";
    String query(q + 1);
    int start = 0;
    while (start <= static_cast<int>(query.length())) {
        int amp = query.indexOf('&', start);
        if (amp < 0) amp = query.length();
        String part = query.substring(start, amp);
        if (part.startsWith(needle)) {
            out = urlDecode(part.substring(needle.length()));
            return true;
        }
        start = amp + 1;
    }
    return false;
}

String formatFileSize(size_t bytes) {
    if (bytes < 1024) return String(bytes) + " B";
    if (bytes < 1024 * 1024) return String(bytes / 1024) + " KB";
    if (bytes < 1024 * 1024 * 1024) return String(bytes / (1024 * 1024)) + " MB";
    return String(bytes / (1024 * 1024 * 1024)) + " GB";
}

String jsonEscape(const String& s) {
    String out;
    out.reserve(s.length() + 8);
    for (size_t i = 0; i < s.length(); ++i) {
        const char c = s[i];
        if (c == '\\' || c == '"') {
            out += '\\';
            out += c;
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (static_cast<uint8_t>(c) < 0x20) {
            char buf[7];
            snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(static_cast<uint8_t>(c)));
            out += buf;
        } else {
            out += c;
        }
    }
    return out;
}

String displayNameForEntry(const String& rawName, const String& dirPath) {
    String name = rawName;
    String dir = dirPath;
    while (dir.endsWith("/") && dir.length() > 1) dir.remove(dir.length() - 1);
    if (dir != "/" && name.startsWith(dir + "/")) name = name.substring(dir.length() + 1);
    else if (dir == "/" && name.startsWith("/")) name = name.substring(1);
    int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);
    return name;
}

// ── FS dispatch ─────────────────────────────────────────────────────────────

FsId fsForPath(const String& userPath, String& outRealPath) {
    outRealPath = sdPathForUserPath(userPath);
    return FsId::Sd;
}

static File openFile(const String& path, FsId fs, const char* mode) {
    (void)fs;
    return SD.open(path.c_str(), mode);
}

static bool dirExists(const String& path, FsId fs) {
    (void)fs;
    File f = SD.open(path.c_str());
    if (!f) return false;
    bool d = f.isDirectory();
    f.close();
    return d;
}

static bool renamePath(const String& fromUserPath, const String& toUserPath) {
    String fromReal = sdPathForUserPath(fromUserPath);
    String toReal = sdPathForUserPath(toUserPath);
    return SD.rename(fromReal.c_str(), toReal.c_str());
}

String listFilesJson(const String& userPath) {
    String realPath;
    FsId fs = fsForPath(userPath, realPath);
    File root = openFile(realPath, fs, "r");
    if (!root || !root.isDirectory()) {
        if (root) root.close();
        return "[]";
    }
    String json = "[";
    bool first = true;
    File entry = root.openNextFile();
    while (entry) {
        String name = displayNameForEntry(String(entry.name()), realPath);
        if (!name.isEmpty() && isSafePath(name)) {
            if (!first) json += ",";
            first = false;
            json += "{\"name\":\"" + jsonEscape(name) + "\",\"isDir\":";
            json += entry.isDirectory() ? "true" : "false";
            json += ",\"size\":" + String(entry.size());
            json += ",\"modified\":" + String(static_cast<unsigned long>(entry.getLastWrite())) + "}";
        }
        File next = root.openNextFile();
        entry.close();
        entry = next;
    }
    root.close();
    json += "]";
    return json;
}

static bool mkDirRecursive(const String& path, FsId fs) {
    if (path.isEmpty() || path == "/") return true;
    int pos = 1;
    while (pos < static_cast<int>(path.length())) {
        int next = path.indexOf('/', pos);
        if (next < 0) next = path.length();
        String sub = path.substring(0, next);
        if (sub.length() > 1) {
            (void)fs;
            if (!dirExists(sub, FsId::Sd) && !SD.mkdir(sub.c_str())) return false;
        }
        pos = next + 1;
    }
    return true;
}

bool deleteFileOrDir(const String& userPath) {
    String realPath = sdPathForUserPath(userPath);
    return SD.remove(realPath.c_str()) || SD.rmdir(realPath.c_str());
}

bool fileExists(const String& userPath) {
    String realPath = sdPathForUserPath(userPath);
    File f = SD.open(realPath.c_str(), "r");
    if (!f) return false;
    f.close();
    return true;
}

String sidecarPathForBook(String bookPath, const char* suffix) {
    if (bookPath.isEmpty() || !suffix) return String();
    int slash = bookPath.lastIndexOf('/');
    int dot = bookPath.lastIndexOf('.');
    if (dot > slash) bookPath = bookPath.substring(0, dot);
    return bookPath + suffix;
}

bool isTxtFileName(const String& name) {
    String lower = name;
    lower.toLowerCase();
    return lower.endsWith(".txt");
}

int readBookProgressPercent(const String& bookPath, uint16_t& outChapter, uint16_t& outPage) {
    outChapter = 0;
    outPage = 0;
    const String progressPath = sidecarPathForBook(bookPath, ".vink-progress");
    const String tocPath = sidecarPathForBook(bookPath, ".vink-toc");
    if (!SD.exists(progressPath.c_str()) || !SD.exists(tocPath.c_str())) return -1;
    File pf = SD.open(progressPath.c_str(), FILE_READ);
    if (!pf) return -1;
    uint32_t magic = 0, cachedSize = 0;
    uint16_t chapter = 0, page = 0;
    pf.read(reinterpret_cast<uint8_t*>(&magic), sizeof(magic));
    pf.read(reinterpret_cast<uint8_t*>(&cachedSize), sizeof(cachedSize));
    pf.read(reinterpret_cast<uint8_t*>(&chapter), sizeof(chapter));
    pf.read(reinterpret_cast<uint8_t*>(&page), sizeof(page));
    pf.close();
    if (magic != 0x56505232UL && magic != 0x56505233UL) return -1;
    File tf = SD.open(tocPath.c_str(), FILE_READ);
    if (!tf) return -1;
    uint32_t tMagic = 0, tSize = 0, tVer = 0;
    uint16_t tCount = 0;
    tf.read(reinterpret_cast<uint8_t*>(&tMagic), sizeof(tMagic));
    tf.read(reinterpret_cast<uint8_t*>(&tSize), sizeof(tSize));
    tf.read(reinterpret_cast<uint8_t*>(&tVer), sizeof(tVer));
    tf.read(reinterpret_cast<uint8_t*>(&tCount), sizeof(tCount));
    tf.close();
    if (tMagic != 0x56435433UL || tCount == 0) return -1;
    outChapter = chapter;
    outPage = page;
    const uint32_t pct = (static_cast<uint32_t>(chapter) * 100UL) / tCount;
    return pct > 100 ? 100 : static_cast<int>(pct);
}

String libraryJson() {
    String json = "{\"books\":[";
    bool first = true;
    File dir = SD.open("/books");
    if (dir && dir.isDirectory()) {
        File entry = dir.openNextFile();
        while (entry) {
            if (!entry.isDirectory()) {
                String name = displayNameForEntry(String(entry.name()), "/books");
                if (isTxtFileName(name) && isSafePath(name)) {
                    String path = "/books/" + name;
                    String title = name;
                    int dot = title.lastIndexOf('.');
                    if (dot > 0) title = title.substring(0, dot);
                    uint16_t chapter = 0, page = 0;
                    int progress = readBookProgressPercent(path, chapter, page);
                    const bool hasToc = SD.exists(sidecarPathForBook(path, ".vink-toc").c_str());
                    const bool hasProgress = SD.exists(sidecarPathForBook(path, ".vink-progress").c_str());
                    const bool hasPages = SD.exists(sidecarPathForBook(path, ".vink-pages").c_str());
                    const bool hasBookmarks = SD.exists(sidecarPathForBook(path, ".vink-bookmarks").c_str());
                    if (!first) json += ",";
                    first = false;
                    json += "{\"name\":\"" + jsonEscape(name) + "\",";
                    json += "\"title\":\"" + jsonEscape(title) + "\",";
                    json += "\"path\":\"" + jsonEscape(path) + "\",";
                    json += "\"size\":" + String(entry.size()) + ",";
                    json += "\"hasToc\":" + String(hasToc ? "true" : "false") + ",";
                    json += "\"hasProgress\":" + String(hasProgress ? "true" : "false") + ",";
                    json += "\"hasPageCache\":" + String(hasPages ? "true" : "false") + ",";
                    json += "\"hasBookmarks\":" + String(hasBookmarks ? "true" : "false") + ",";
                    json += "\"chapter\":" + String(chapter) + ",";
                    json += "\"page\":" + String(page) + ",";
                    json += "\"progressPercent\":" + String(progress);
                    json += "}";
                }
            }
            File next = dir.openNextFile();
            entry.close();
            entry = next;
        }
        dir.close();
    } else if (dir) {
        dir.close();
    }
    json += "],\"booksDirExists\":";
    json += SD.exists("/books") ? "true" : "false";
    json += "}";
    return json;
}



uint8_t cleanupGeneratedCacheForBook(const String& path) {
    uint8_t removed = 0;
    const String toc = sidecarPathForBook(path, ".vink-toc");
    const String pages = sidecarPathForBook(path, ".vink-pages");
    if (SD.exists(toc.c_str()) && SD.remove(toc.c_str())) removed++;
    if (SD.exists(pages.c_str()) && SD.remove(pages.c_str())) removed++;
    return removed;
}

uint16_t cleanupAllGeneratedBookCaches() {
    uint16_t removed = 0;
    File dir = SD.open("/books");
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        return 0;
    }
    File entry = dir.openNextFile();
    while (entry) {
        if (!entry.isDirectory()) {
            String name = displayNameForEntry(String(entry.name()), "/books");
            if (isTxtFileName(name) && isSafePath(name)) {
                removed += cleanupGeneratedCacheForBook("/books/" + name);
            }
        }
        File next = dir.openNextFile();
        entry.close();
        entry = next;
    }
    dir.close();
    return removed;
}



uint16_t cleanupOrphanGeneratedBookCaches() {
    uint16_t removed = 0;
    File dir = SD.open("/books");
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        return 0;
    }
    File entry = dir.openNextFile();
    while (entry) {
        if (!entry.isDirectory()) {
            String name = displayNameForEntry(String(entry.name()), "/books");
            String lower = name;
            lower.toLowerCase();
            const bool isToc = lower.endsWith(".vink-toc");
            const bool isPages = lower.endsWith(".vink-pages");
            if ((isToc || isPages) && isSafePath(name)) {
                const int suffixLen = isToc ? 9 : 11; // strlen(".vink-toc/pages")
                const String base = name.substring(0, name.length() - suffixLen);
                const String txtLower = "/books/" + base + ".txt";
                const String txtUpper = "/books/" + base + ".TXT";
                if (!SD.exists(txtLower.c_str()) && !SD.exists(txtUpper.c_str())) {
                    const String cachePath = "/books/" + name;
                    if (SD.remove(cachePath.c_str())) removed++;
                }
            }
        }
        File next = dir.openNextFile();
        entry.close();
        entry = next;
    }
    dir.close();
    return removed;
}

String configJson(const VinkConfig& c, bool maskSecrets) {
    String json;
    json.reserve(1024);
    json += "{";
    json += "\"fontSize\":" + String(c.fontSize) + ",";
    json += "\"lineSpacing\":" + String(c.lineSpacing) + ",";
    json += "\"paragraphSpacing\":" + String(c.paragraphSpacing) + ",";
    json += "\"indentFirstLine\":" + String(c.indentFirstLine) + ",";
    json += "\"marginLeft\":" + String(c.marginLeft) + ",";
    json += "\"marginRight\":" + String(c.marginRight) + ",";
    json += "\"marginTop\":" + String(c.marginTop) + ",";
    json += "\"marginBottom\":" + String(c.marginBottom) + ",";
    json += "\"justify\":" + String(c.justify ? "true" : "false") + ",";
    json += "\"refreshFrequency\":" + String(static_cast<uint8_t>(c.refreshFrequency)) + ",";
    json += "\"readerMiddleRefreshEvery\":" + String(c.readerMiddleRefreshEvery) + ",";
    json += "\"readerFullRefreshEvery\":" + String(c.readerFullRefreshEvery) + ",";
    json += "\"wifiSsid\":\"" + jsonEscape(c.wifiSsid) + "\",";
    json += "\"wifiPassword\":\"";
    if (maskSecrets && !c.wifiPassword.isEmpty()) json += "***";
    else json += jsonEscape(c.wifiPassword);
    json += "\",";
    json += "\"lockScreenImagePath\":\"" + jsonEscape(c.lockScreenImagePath) + "\",";
    json += "\"lockScreenEnabled\":" + String(c.lockScreenEnabled ? "true" : "false") + ",";
    json += "\"lockScreenWakeOnDoubleClick\":" + String(c.lockScreenWakeOnDoubleClick ? "true" : "false") + ",";
    json += "\"autoSleepMinutes\":" + String(c.autoSleepMinutes) + ",";
    json += "\"autoSleepEnabled\":" + String(c.autoSleepEnabled ? "true" : "false") + ",";
    json += "\"simplifiedChinese\":" + String(c.simplifiedChinese ? "true" : "false");
    json += "}";
    return json;
}

// ── HTTP response helpers ────────────────────────────────────────────────────

static esp_err_t jsonReply(httpd_req_t* req, int statusCode,
                           const char* statusMsg,
                           const char* json, size_t jsonLen) {
    char status[32];
    snprintf(status, sizeof(status), "%d %s", statusCode, statusMsg);
    httpd_resp_set_status(req, status);
    httpd_resp_set_hdr(req, "Content-Type", "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    if (json)
        return httpd_resp_send(req, json, jsonLen);
    return httpd_resp_send(req, nullptr, 0);
}

static esp_err_t jsonOk(httpd_req_t* req, const char* json, size_t len) {
    return jsonReply(req, 200, "OK", json, len);
}

static esp_err_t jsonErr(httpd_req_t* req, int code, const char* msg) {
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"error\":%d,\"message\":\"%s\"}", code, msg);
    return jsonReply(req, code, "Error", buf, strlen(buf));
}

// ── Extract user path from URI ───────────────────────────────────────────────

// e.g. "/api/files/foo/bar" -> "/foo/bar"
static String uriToUserPath(const char* uri) {
    String u(uri);
    int idx = u.indexOf("/api/files");
    if (idx >= 0) {
        String tail = u.substring(idx + 10);  // skip "/api/files" (10 chars)
        if (tail.isEmpty()) return "/";
        // Browser JS encodes absolute destinations like /books/a.txt as
        // /api/files/%2Fbooks%2Fa.txt. Decode first, then normalize the leading
        // slash so SD paths do not become //books/a.txt.
        return normalizeUserPath(urlDecode(tail));
    }
    return "/";
}

// ── GET /api/config ─────────────────────────────────────────────────────────

static esp_err_t apiConfigGet(httpd_req_t* req) {
    (void)req;
    const ConfigService* cfg = s_instance ? s_instance->configService() : nullptr;
    VinkConfig c{};
    if (cfg) c = cfg->get();
    const String json = configJson(c, true);
    return jsonOk(req, json.c_str(), json.length());
}

// ── POST /api/config ─────────────────────────────────────────────────────────

static esp_err_t apiConfigPost(httpd_req_t* req) {
    if (!s_instance || !s_instance->configService()) {
        return jsonErr(req, 500, "Config service not ready");
    }
    uint32_t contentLen = req->content_len;
    if (contentLen >= 4096) return jsonErr(req, 413, "Payload too large");

    char* body = static_cast<char*>(malloc(contentLen + 1));
    if (!body) return jsonErr(req, 500, "Out of memory");
    memset(body, 0, contentLen + 1);

    uint32_t received = 0;
    while (received < contentLen) {
        int n = httpd_req_recv(req, body + received, contentLen - received);
        if (n <= 0) { free(body); return jsonErr(req, 400, "Incomplete read"); }
        received += n;
    }

    StaticJsonDocument<1024> doc;
    auto err = deserializeJson(doc, body);
    free(body);
    if (err) return jsonErr(req, 400, "Invalid JSON");

    JsonObject obj = doc.as<JsonObject>();
    ConfigService* config = const_cast<ConfigService*>(s_instance->configService());
    auto& cfg = config->mut();

    if (obj.containsKey("fontSize"))        cfg.fontSize        = constrain(obj["fontSize"].as<uint8_t>(), 12, 48);
    if (obj.containsKey("lineSpacing"))     cfg.lineSpacing     = constrain(obj["lineSpacing"].as<uint8_t>(), 50, 200);
    if (obj.containsKey("paragraphSpacing")) cfg.paragraphSpacing = constrain(obj["paragraphSpacing"].as<uint8_t>(), 0, 100);
    if (obj.containsKey("indentFirstLine"))  cfg.indentFirstLine  = constrain(obj["indentFirstLine"].as<uint8_t>(), 0, 4);
    if (obj.containsKey("marginLeft"))      cfg.marginLeft      = constrain(obj["marginLeft"].as<uint8_t>(), 0, 120);
    if (obj.containsKey("marginRight"))     cfg.marginRight     = constrain(obj["marginRight"].as<uint8_t>(), 0, 120);
    if (obj.containsKey("marginTop"))       cfg.marginTop       = constrain(obj["marginTop"].as<uint8_t>(), 0, 160);
    if (obj.containsKey("marginBottom"))    cfg.marginBottom    = constrain(obj["marginBottom"].as<uint8_t>(), 0, 160);
    if (obj.containsKey("justify"))         cfg.justify         = obj["justify"].as<bool>();
    if (obj.containsKey("refreshFrequency")) {
        config->setRefreshFrequency(static_cast<RefreshFrequency>(
            constrain(obj["refreshFrequency"].as<uint8_t>(), 0, 2)));
    }
    if (obj.containsKey("readerMiddleRefreshEvery") || obj.containsKey("readerFullRefreshEvery")) {
        const uint8_t middleEvery = obj.containsKey("readerMiddleRefreshEvery")
            ? obj["readerMiddleRefreshEvery"].as<uint8_t>()
            : cfg.readerMiddleRefreshEvery;
        const uint8_t fullEvery = obj.containsKey("readerFullRefreshEvery")
            ? obj["readerFullRefreshEvery"].as<uint8_t>()
            : cfg.readerFullRefreshEvery;
        config->setReaderRefreshThresholds(middleEvery, fullEvery);
    }
    if (obj.containsKey("wifiSsid"))        cfg.wifiSsid         = obj["wifiSsid"].as<String>();
    if (obj.containsKey("wifiPassword"))    cfg.wifiPassword     = obj["wifiPassword"].as<String>();
    if (obj.containsKey("lockScreenImagePath")) cfg.lockScreenImagePath = obj["lockScreenImagePath"].as<String>();
    if (obj.containsKey("lockScreenEnabled")) cfg.lockScreenEnabled = obj["lockScreenEnabled"].as<bool>();
    if (obj.containsKey("lockScreenWakeOnDoubleClick")) cfg.lockScreenWakeOnDoubleClick = obj["lockScreenWakeOnDoubleClick"].as<bool>();
    if (obj.containsKey("autoSleepMinutes")) cfg.autoSleepMinutes = constrain(obj["autoSleepMinutes"].as<uint8_t>(), 0, 60);
    if (obj.containsKey("autoSleepEnabled")) cfg.autoSleepEnabled = obj["autoSleepEnabled"].as<bool>();
    if (obj.containsKey("simplifiedChinese")) cfg.simplifiedChinese = obj["simplifiedChinese"].as<bool>();

    bool ok = config->save();
    char resp[64];
    snprintf(resp, sizeof(resp), "{\"saved\":%s}", ok ? "true" : "false");
    return jsonOk(req, resp, strlen(resp));
}

// ── GET /api/files?path=/ ───────────────────────────────────────────────────

static esp_err_t apiFileDownload(httpd_req_t* req) {
    String userPath = uriToUserPath(req->uri);
    if (!isSafePath(userPath) || userPath == "/") return jsonErr(req, 403, "Invalid path");

    String realPath;
    FsId fs = fsForPath(userPath, realPath);
    File fh = openFile(realPath, fs, "r");
    if (!fh) return jsonErr(req, 404, "Not found");
    if (fh.isDirectory()) {
        fh.close();
        return jsonErr(req, 400, "Cannot download directory");
    }

    String name = displayNameForEntry(String(fh.name()), realPath);
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_hdr(req, "Content-Type", "application/octet-stream");
    String disp = "attachment; filename=\"" + jsonEscape(name) + "\"";
    httpd_resp_set_hdr(req, "Content-Disposition", disp.c_str());
    char buf[4096];
    while (fh.available()) {
        size_t n = fh.readBytes(buf, sizeof(buf));
        if (n == 0) break;
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            fh.close();
            return ESP_FAIL;
        }
    }
    fh.close();
    return httpd_resp_send_chunk(req, nullptr, 0);
}

static esp_err_t apiFilesGet(httpd_req_t* req) {
    String userPath = "/";
    queryParamValue(req->uri, "path", userPath);
    userPath = normalizeUserPath(userPath);
    if (!isSafePath(userPath)) return jsonErr(req, 403, "Invalid path");

    String json = listFilesJson(userPath);
    return jsonOk(req, json.c_str(), json.length());
}

// ── PUT /api/files/* — create / overwrite file ───────────────────────────────

static esp_err_t apiFilePut(httpd_req_t* req) {
    String userPath = normalizeUserPath(uriToUserPath(req->uri));
    if (!isSafePath(userPath)) return jsonErr(req, 403, "Invalid path");

    String realPath;
    FsId fs = fsForPath(userPath, realPath);
    if (realPath == "/" || realPath.endsWith("/")) return jsonErr(req, 400, "Invalid path");

    // Create parent directories if needed.
    int lastSlash = realPath.lastIndexOf('/');
    if (lastSlash > 0) {
        String dirPath = realPath.substring(0, lastSlash);
        mkDirRecursive(dirPath, fs);
    }

    File fh = openFile(realPath, fs, "w");
    if (!fh) return jsonErr(req, 500, "Cannot open for write");

    size_t remaining = req->content_len;
    char buf[4096];
    while (remaining > 0) {
        int n = httpd_req_recv(req, buf, remaining < sizeof(buf) ? remaining : sizeof(buf));
        if (n <= 0) break;
        if (fh.write(reinterpret_cast<const uint8_t*>(buf), n) != static_cast<size_t>(n)) {
            fh.close();
            return jsonErr(req, 500, "Write error");
        }
        remaining -= n;
    }
    fh.close();
    if (remaining > 0) return jsonErr(req, 400, "Incomplete upload");
    return jsonOk(req, "{\"ok\":true}", 12);
}

// ── POST /api/files/mkdir ───────────────────────────────────────────────────
// Body: {"path": "/foo/bar"}

static esp_err_t apiMkdir(httpd_req_t* req) {
    uint32_t len = req->content_len;
    if (len >= 256) return jsonErr(req, 413, "Payload too large");
    char body[256] = {0};
    size_t received = 0;
    while (received < len) {
        int n = httpd_req_recv(req, body + received, len - received);
        if (n <= 0) return jsonErr(req, 400, "Incomplete read");
        received += n;
    }
    StaticJsonDocument<128> doc;
    auto err = deserializeJson(doc, body);
    if (err) return jsonErr(req, 400, "Invalid JSON");
    const char* path = doc["path"];
    if (!path) return jsonErr(req, 400, "Missing 'path'");
    String userPath = normalizeUserPath(urlDecode(path));
    if (!isSafePath(userPath)) return jsonErr(req, 403, "Invalid path");

    String realPath;
    FsId fs = fsForPath(userPath, realPath);
    bool ok = mkDirRecursive(realPath, fs);
    if (!ok) return jsonErr(req, 500, "Cannot create directory");
    char resp[64];
    snprintf(resp, sizeof(resp), "{\"created\":%s}", ok ? "true" : "false");
    return jsonOk(req, resp, strlen(resp));
}

// ── POST /api/files/rename ──────────────────────────────────────────────────
// Body: {"from":"/old.txt","to":"/new.txt"}

static esp_err_t apiFileRename(httpd_req_t* req) {
    uint32_t len = req->content_len;
    if (len >= 512) return jsonErr(req, 413, "Payload too large");
    char body[512] = {0};
    size_t received = 0;
    while (received < len) {
        int n = httpd_req_recv(req, body + received, len - received);
        if (n <= 0) return jsonErr(req, 400, "Incomplete read");
        received += n;
    }
    StaticJsonDocument<256> doc;
    auto err = deserializeJson(doc, body);
    if (err) return jsonErr(req, 400, "Invalid JSON");
    const char* from = doc["from"];
    const char* to = doc["to"];
    if (!from || !to) return jsonErr(req, 400, "Missing path");
    String fromPath = normalizeUserPath(urlDecode(from));
    String toPath = normalizeUserPath(urlDecode(to));
    if (!isSafePath(fromPath) || !isSafePath(toPath)) return jsonErr(req, 403, "Invalid path");
    if (isSdRootPath(fromPath) || isSdRootPath(toPath)) {
        return jsonErr(req, 400, "Invalid root path");
    }

    String toReal;
    FsId fs = fsForPath(toPath, toReal);
    int lastSlash = toReal.lastIndexOf('/');
    if (lastSlash > 0) mkDirRecursive(toReal.substring(0, lastSlash), fs);
    bool ok = renamePath(fromPath, toPath);
    char resp[64];
    snprintf(resp, sizeof(resp), "{\"renamed\":%s}", ok ? "true" : "false");
    return jsonOk(req, resp, strlen(resp));
}

// ── DELETE /api/files/* ─────────────────────────────────────────────────────

static esp_err_t apiFileDelete(httpd_req_t* req) {
    String userPath = normalizeUserPath(uriToUserPath(req->uri));
    if (!isSafePath(userPath)) return jsonErr(req, 403, "Invalid path");
    if (isSdRootPath(userPath)) {
        return jsonErr(req, 400, "Cannot delete root");
    }

    bool ok = deleteFileOrDir(userPath);
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"deleted\":%s}", ok ? "true" : "false");
    return jsonOk(req, buf, strlen(buf));
}

// ── POST /api/system/reboot ─────────────────────────────────────────────────

static esp_err_t apiSystemReboot(httpd_req_t* req) {
    (void)req;
    delay(200);
    esp_restart();
    return jsonOk(req, "{\"reboot\":true}", 14);  // unreachable
}

// ── GET /api/system/info ─────────────────────────────────────────────────────

static esp_err_t apiSystemInfo(httpd_req_t* req) {
    (void)req;
    ConfigService::StorageStats st{};
    if (s_instance && s_instance->configService()) {
        st = s_instance->configService()->storageStats();
    }
    char buf[640];
    snprintf(buf, sizeof(buf),
        "{"
        "\"freeHeap\":%u,"
        "\"freePsram\":%u,"
        "\"flashSize\":%u,"
        "\"sdCardSize\":%llu,"
        "\"sdTotal\":%llu,"
        "\"sdUsed\":%llu,"
        "\"uptimeSeconds\":%lu,"
        "\"version\":\"%s\","
        "\"width\":%d,"
        "\"height\":%d"
        "}",
        ESP.getFreeHeap(),
        ESP.getFreePsram(),
        ESP.getFlashChipSize(),
        static_cast<unsigned long long>(SD.cardSize()),
        static_cast<unsigned long long>(SD.totalBytes()),
        static_cast<unsigned long long>(SD.usedBytes()),
        static_cast<unsigned long>(st.uptimeSeconds),
        kVinkPaperS3FirmwareVersion,
        kPaperS3Width,
        kPaperS3Height
    );
    return jsonOk(req, buf, strlen(buf));
}

// ── GET /api/library ─────────────────────────────────────────────────────────

static esp_err_t apiLibraryGet(httpd_req_t* req) {
    const String json = libraryJson();
    return jsonOk(req, json.c_str(), json.length());
}

// ── GET /api/export/config ──────────────────────────────────────────────────

static esp_err_t apiExportConfig(httpd_req_t* req) {
    const ConfigService* cfg = s_instance ? s_instance->configService() : nullptr;
    VinkConfig c{};
    if (cfg) c = cfg->get();
    const String json = configJson(c, false);
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_hdr(req, "Content-Type", "application/json");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"vink-config.json\"");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, json.c_str(), json.length());
}



// ── POST /api/library/cleanup-all-cache ──────────────────────────────────────

static esp_err_t apiLibraryCleanupAllCache(httpd_req_t* req) {
    (void)req;
    const uint16_t removed = cleanupAllGeneratedBookCaches();
    char resp[48];
    snprintf(resp, sizeof(resp), "{\"removed\":%u}", removed);
    return jsonOk(req, resp, strlen(resp));
}



// ── POST /api/library/cleanup-orphan-cache ───────────────────────────────────

static esp_err_t apiLibraryCleanupOrphanCache(httpd_req_t* req) {
    (void)req;
    const uint16_t removed = cleanupOrphanGeneratedBookCaches();
    char resp[48];
    snprintf(resp, sizeof(resp), "{\"removed\":%u}", removed);
    return jsonOk(req, resp, strlen(resp));
}

// ── POST /api/library/cleanup-cache ─────────────────────────────────────────
// Body: {"path":"/books/name.txt"}; removes generated TOC/page cache only.

static esp_err_t apiLibraryCleanupCache(httpd_req_t* req) {
    uint32_t len = req->content_len;
    if (len >= 256) return jsonErr(req, 413, "Payload too large");
    char body[256] = {0};
    size_t received = 0;
    while (received < len) {
        int n = httpd_req_recv(req, body + received, len - received);
        if (n <= 0) return jsonErr(req, 400, "Incomplete read");
        received += n;
    }
    StaticJsonDocument<128> doc;
    auto err = deserializeJson(doc, body);
    if (err) return jsonErr(req, 400, "Invalid JSON");
    const char* rawPath = doc["path"];
    if (!rawPath) return jsonErr(req, 400, "Missing path");
    String path = normalizeUserPath(urlDecode(rawPath));
    if (!path.startsWith("/books/") || !isSafePath(path) || !isTxtFileName(path)) {
        return jsonErr(req, 403, "Invalid book path");
    }
    const uint8_t removed = cleanupGeneratedCacheForBook(path);
    char resp[48];
    snprintf(resp, sizeof(resp), "{\"removed\":%u}", removed);
    return jsonOk(req, resp, strlen(resp));
}

// ── OPTIONS preflight (CORS) ─────────────────────────────────────────────────

static esp_err_t apiOptions(httpd_req_t* req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    return httpd_resp_send(req, nullptr, 0);
}

// ── GET / — serve the web UI ────────────────────────────────────────────────

static const char s_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="zh">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Vink-PaperS3</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,BlinkMacSystemFont,sans-serif;background:#f0f0f0;color:#222;min-height:100vh}
.bar{background:#1a1a1a;color:#fff;padding:12px 16px;display:flex;gap:4px;align-items:center}
.bar button{background:#2a2a2a;color:#ccc;border:1px solid #444;padding:6px 12px;border-radius:4px;cursor:pointer;font-size:14px}
.bar button.active{background:#4a4a4a;color:#fff;border-color:#666}
.bar button:hover{background:#333}
.content{padding:16px;max-width:800px;margin:0 auto}
h2{font-size:16px;color:#555;margin-bottom:12px;padding-bottom:6px;border-bottom:1px solid #ddd}
.card{background:#fff;border-radius:8px;box-shadow:0 1px 4px rgba(0,0,0,.1);padding:16px;margin-bottom:12px}
.row{display:flex;align-items:center;gap:8px;margin:6px 0;font-size:14px}
.row label{flex:0 0 140px;color:#444}
.row input{flex:1;padding:6px 8px;border:1px solid #ccc;border-radius:4px;font-size:14px}
.row input[type=checkbox]{width:auto;flex:none}
.row input[readonly]{background:#f5f5f5}
.row .hint{font-size:12px;color:#999;margin-left:8px}
.row.right{flex-direction:row-reverse}
button.save{background:#1a1a1a;color:#fff;border:none;padding:10px 24px;border-radius:6px;cursor:pointer;font-size:15px}
button.save:hover{background:#333}
.msg{padding:8px 12px;border-radius:4px;font-size:14px;margin-top:8px;display:none}
.msg.ok{background:#d4edda;color:#155724}
.msg.err{background:#f8d7da;color:#721c24}
.file-list{margin-top:8px}
.file-item{display:flex;align-items:center;padding:8px 0;border-bottom:1px solid #eee;font-size:14px}
.file-item:last-child{border-bottom:none}
.file-item .name{flex:1;word-break:break-all;color:#333}
.file-item .meta{color:#999;font-size:12px;margin:0 12px}
.file-item .actions{display:flex;gap:4px;flex-wrap:wrap;justify-content:flex-end}
.file-item .act{border:none;background:#f2f2f2;color:#333;cursor:pointer;font-size:13px;padding:4px 8px;border-radius:4px;text-decoration:none}
.file-item .act:hover{background:#e0e0e0}
.file-item .danger{color:#c00}
.file-item .danger:hover{color:#f00}
.toolbar{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin:8px 0}
.toolbar button{border:1px solid #ccc;background:#fff;border-radius:5px;padding:6px 10px;cursor:pointer}
.toolbar button:hover{background:#f8f8f8}
.breadcrumb{font-size:13px;color:#666;margin-bottom:8px}
.breadcrumb a{color:#1a1a1a;text-decoration:none}
.info-grid{display:grid;grid-template-columns:1fr 1fr;gap:8px;font-size:13px;color:#555}
.info-grid div{padding:6px 8px;background:#f8f8f8;border-radius:4px}
.badge{display:inline-block;font-size:12px;border-radius:999px;padding:2px 7px;margin-left:5px;background:#eee;color:#555}
.badge.ok{background:#d4edda;color:#155724}.badge.warn{background:#fff3cd;color:#856404}
.progress{height:7px;background:#eee;border-radius:5px;overflow:hidden;margin-top:5px}.progress i{display:block;height:100%;background:#333}
</style>
</head>
<body>
<div class="bar">
  <button id="tabFiles" class="active" onclick="showTab('files')">📁 文件</button>
  <button id="tabBooks" onclick="showTab('books')">📚 书架</button>
  <button id="tabConfig" onclick="showTab('config')">⚙️ 配置</button>
  <button id="tabBackup" onclick="showTab('backup')">⬇️ 备份</button>
  <button id="tabInfo" onclick="showTab('info')">ℹ️ 系统</button>
</div>

<div class="content">

<!-- Files tab -->
<div id="panelFiles">
  <div class="card">
    <div class="breadcrumb" id="breadcrumb">路径: <a href="#" onclick="chdir('/')">/</a></div>
    <div class="toolbar">
      <button onclick="chdir('/')">SD 根目录</button>
      <button onclick="ensureBooks()">书籍目录 /books</button>
      <button onclick="loadFiles(currentPath)">刷新</button>
    </div>
    <div id="fileList" class="file-list"></div>
  </div>
  <div class="card">
    <h2>上传文件</h2>
    <input type="file" id="fileInput" multiple>
    <button class="save" onclick="uploadFile()" style="margin-top:8px">上传到当前目录</button>
    <button class="save" onclick="uploadToBooks()" style="margin-top:8px;background:#333;margin-left:8px">上传到 /books</button>
    <button class="save" onclick="newFolder()" style="margin-top:8px;background:#555;margin-left:8px">📁 新建目录</button>
    <div id="uploadMsg" class="msg"></div>
  </div>
</div>

<!-- Books tab -->
<div id="panelBooks" style="display:none">
  <div class="card">
    <h2>本地书架</h2>
    <div class="toolbar">
      <button onclick="ensureBooks()">创建/打开 /books</button>
      <button onclick="loadBooks()">刷新书架</button>
      <button onclick="cleanupAllBookCaches()">批量清理目录/分页缓存</button>
      <button onclick="cleanupOrphanBookCaches()">清理无主目录/分页缓存</button>
    </div>
    <div id="bookList" class="file-list"></div>
  </div>
  <div class="card">
    <h2>说明</h2>
    <div style="font-size:13px;color:#666;line-height:1.7">把 TXT 上传到 <b>/books</b> 后，设备会在首次打开时自动识别目录、生成分页缓存和阅读进度。这里可以查看缓存/进度状态，并安全清理目录/分页缓存；不会删除阅读进度。</div>
  </div>
</div>

<!-- Config tab -->
<div id="panelConfig" style="display:none">
  <div class="card">
    <h2>阅读排版</h2>
    <div class="row"><label>字体大小</label><input type="number" id="fontSize" min="12" max="48"> <span class="hint">12~48 px</span></div>
    <div class="row"><label>行间距</label><input type="number" id="lineSpacing" min="50" max="200"> <span class="hint">50~200 %</span></div>
    <div class="row"><label>段间距</label><input type="number" id="paragraphSpacing" min="0" max="100"> <span class="hint">0~100 %</span></div>
    <div class="row"><label>首行缩进</label><input type="number" id="indentFirstLine" min="0" max="4"> <span class="hint">0~4 字</span></div>
    <div class="row"><label>左边距</label><input type="number" id="marginLeft" min="0" max="120"> <span class="hint">px</span></div>
    <div class="row"><label>右边距</label><input type="number" id="marginRight" min="0" max="120"> <span class="hint">px</span></div>
    <div class="row"><label>上边距</label><input type="number" id="marginTop" min="0" max="160"> <span class="hint">px</span></div>
    <div class="row"><label>下边距</label><input type="number" id="marginBottom" min="0" max="160"> <span class="hint">px</span></div>
    <div class="row"><label>两端对齐</label><input type="checkbox" id="justify"></div>
  </div>
  <div class="card">
    <h2>刷新策略</h2>
    <div class="row"><label>刷新频率</label>
      <select id="refreshFrequency" style="flex:1;padding:6px 8px;font-size:14px">
        <option value="0">极速</option>
        <option value="1">均衡</option>
        <option value="2">清晰</option>
      </select>
    </div>
    <div class="row"><label>中间刷新</label><input type="number" id="readerMiddleRefreshEvery" min="1" max="60"> <span class="hint">页</span></div>
    <div class="row"><label>GC16 全刷</label><input type="number" id="readerFullRefreshEvery" min="0" max="120"> <span class="hint">0=从不，页</span></div>
  </div>
  <div class="card">
    <h2>WiFi（STA 模式）</h2>
    <div class="row"><label>SSID</label><input type="text" id="wifiSsid" placeholder="路由器名称"></div>
    <div class="row"><label>密码</label><input type="password" id="wifiPassword" placeholder="留空为开放网络"></div>
  </div>
  <div class="card">
    <h2>系统</h2>
    <div class="row"><label>自动休眠</label><input type="checkbox" id="autoSleepEnabled"></div>
    <div class="row"><label>休眠时间</label><input type="number" id="autoSleepMinutes" min="0" max="60"> <span class="hint">分钟，0 为关闭</span></div>
    <div class="row"><label>锁屏图片</label><input type="text" id="lockScreenImagePath" placeholder="/covers/lock.jpg"></div>
    <div class="row"><label>启用锁屏</label><input type="checkbox" id="lockScreenEnabled"></div>
    <div class="row"><label>双击唤醒</label><input type="checkbox" id="lockScreenWakeOnDoubleClick"></div>
    <div class="row"><label>简体中文</label><input type="checkbox" id="simplifiedChinese"></div>
  </div>
  <div class="card">
    <button class="save" onclick="saveConfig()">💾 保存配置</button>
    <button class="save" onclick="reboot()" style="background:#888;margin-left:8px">🔄 重启设备</button>
    <div id="saveMsg" class="msg"></div>
  </div>
</div>

<!-- Backup tab -->
<div id="panelBackup" style="display:none">
  <div class="card">
    <h2>配置备份 / 维护</h2>
    <div class="toolbar">
      <a class="act" href="/api/export/config" download style="text-decoration:none;color:#333;background:#f2f2f2;padding:8px 12px;border-radius:5px">下载完整配置</a>
      <button onclick="chdir('/covers/');showTab('files')">打开封面/锁屏目录</button>
    </div>
    <div style="font-size:13px;color:#666;line-height:1.7;margin-top:8px">完整配置备份会包含 WiFi 密码，仅建议在自己的局域网/热点下使用并妥善保存。</div>
  </div>
</div>

<!-- Info tab -->
<div id="panelInfo" style="display:none">
  <div class="card">
    <h2>系统信息</h2>
    <div class="info-grid" id="sysInfo"></div>
  </div>
</div>

</div>

<script>
const API = '';
let currentPath = '/';

function $(id) { return document.getElementById(id); }
function h(s) { return String(s).replace(/[&<>\"]/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;'}[c])); }
function u(s) { return encodeURIComponent(s); }
function fmtBytes(n) {
  n = Number(n||0);
  if (n < 1024) return n + ' B';
  if (n < 1048576) return (n/1024).toFixed(1) + ' KB';
  if (n < 1073741824) return (n/1048576).toFixed(1) + ' MB';
  return (n/1073741824).toFixed(2) + ' GB';
}

function showTab(name) {
  ['files','books','config','backup','info'].forEach(t => {
    $('tab'+t.charAt(0).toUpperCase()+t.slice(1)).classList.remove('active');
    $('panel'+t.charAt(0).toUpperCase()+t.slice(1)).style.display = 'none';
  });
  $('tab'+name.charAt(0).toUpperCase()+name.slice(1)).classList.add('active');
  $('panel'+name.charAt(0).toUpperCase()+name.slice(1)).style.display = '';
  if (name === 'files') loadFiles(currentPath);
  if (name === 'books') loadBooks();
  if (name === 'config') loadConfig();
  if (name === 'info') loadInfo();
}

function joinPath(base, name) {
  if (!base || base === '/') return '/' + name;
  return base.replace(/\/+$/,'') + '/' + name;
}

function chdir(path) {
  if (path !== '/' && !path.endsWith('/')) path += '/';
  loadFiles(path);
}

function loadFiles(path) {
  currentPath = path;
  fetch(API+'/api/files?path='+encodeURIComponent(path))
    .then(r=>r.json())
    .then(files=>{
      $('breadcrumb').innerHTML = '路径: <a href="#" onclick="chdir(\'/\')">/</a>' +
        (path==='/'?'':' / '+path.split('/').filter(Boolean).map((part,i,arr)=>{
          const partial = '/'+arr.slice(0,i+1).join('/');
          return '<a href="#" onclick="chdir(decodeURIComponent(\''+u(partial)+'\'))">'+h(part)+'</a>';
        }).join(' / '));
      $('fileList').innerHTML = files.length===0?'<div style="color:#999;font-size:13px">空目录</div>':
        files.map(f=>{
          const p = joinPath(path, f.name);
          const ep = u(p);
          const open = f.isDir ? '<button class="act" onclick="chdir(decodeURIComponent(\''+ep+'\'))">打开</button>' :
            '<a class="act" href="/api/files/'+ep+'" download>下载</a>';
          const rename = '<button class="act" onclick="renameFile(decodeURIComponent(\''+ep+'\'))">重命名</button>';
          const del = '<button class="act danger" onclick="delFile(decodeURIComponent(\''+ep+'\'))">删除</button>';
          return '<div class="file-item">'+
            '<span class="name">'+(f.isDir?'📁 ':'📄 ')+h(f.name)+'</span>'+
            (f.isDir?'':'<span class="meta">'+fmtBytes(f.size)+'</span>')+
            '<span class="actions">'+open+rename+del+'</span>'+
          '</div>';
        }).join('');
    })
    .catch(()=>{$('fileList').innerHTML='<div style="color:#c00;font-size:13px">读取文件列表失败，请确认 SD 卡已挂载</div>';});
}

function delFile(path) {
  if (!confirm('删除 '+path+'？')) return;
  fetch(API+'/api/files/'+encodeURIComponent(path),{method:'DELETE'})
    .then(r=>r.json())
    .then(j=>{ if (j.deleted) loadFiles(currentPath); else alert('删除失败'); });
}

function newFolder() {
  const name = prompt('输入目录名称（不含路径）：');
  if (!name || !name.trim()) return;
  const dest = joinPath(currentPath, name.trim());
  fetch(API+'/api/files',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({path:dest})})
    .then(r=>r.json())
    .then(j=>{ loadFiles(currentPath); });
}

function ensureBooks() {
  fetch(API+'/api/files',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({path:'/books'})})
    .then(()=>chdir('/books'));
}

function badge(txt, ok) { return '<span class="badge '+(ok?'ok':'warn')+'">'+h(txt)+'</span>'; }
function loadBooks() {
  fetch(API+'/api/library').then(r=>r.json()).then(j=>{
    const books = j.books || [];
    if (!j.booksDirExists) {
      $('bookList').innerHTML = '<div style="color:#999;font-size:13px">/books 目录还不存在，点击上方按钮创建。</div>';
      return;
    }
    $('bookList').innerHTML = books.length===0 ? '<div style="color:#999;font-size:13px">还没有 TXT 书籍，请上传到 /books。</div>' :
      books.map(b=>{
        const pct = Number(b.progressPercent);
        const pctText = pct >= 0 ? pct + '%' : '未读';
        const ep = u(b.path);
        return '<div class="file-item"><span class="name">📖 '+h(b.title)+
          '<div style="font-size:12px;color:#999;margin-top:3px">'+h(b.path)+' · '+fmtBytes(b.size)+'</div>'+
          '<div>'+badge('目录 '+(b.hasToc?'有':'无'), b.hasToc)+badge('分页 '+(b.hasPageCache?'有':'无'), b.hasPageCache)+badge('进度 '+pctText, pct>=0)+badge('书签 '+(b.hasBookmarks?'有':'无'), b.hasBookmarks)+'</div>'+
          (pct>=0?'<div class="progress"><i style="width:'+pct+'%"></i></div>':'')+
          '</span><span class="actions">'+
          '<a class="act" href="/api/files/'+ep+'" download>下载</a>'+
          '<button class="act" onclick="cleanupBookCache(decodeURIComponent(\''+ep+'\'))">清理缓存</button>'+
          '</span></div>';
      }).join('');
  }).catch(()=>{$('bookList').innerHTML='<div style="color:#c00;font-size:13px">读取书架失败，请确认 SD 卡已挂载</div>';});
}

function cleanupBookCache(path) {
  if (!confirm('只清理目录/分页缓存，不删除阅读进度：'+path+'？')) return;
  fetch(API+'/api/library/cleanup-cache',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({path})})
    .then(r=>r.json()).then(j=>{ alert('已清理缓存文件：'+(j.removed||0)+' 个'); loadBooks(); });
}

function cleanupAllBookCaches() {
  if (!confirm('清理 /books 下所有 TXT 的目录/分页缓存？阅读进度不会删除。')) return;
  fetch(API+'/api/library/cleanup-all-cache',{method:'POST'})
    .then(r=>r.json()).then(j=>{ alert('已清理缓存文件：'+(j.removed||0)+' 个'); loadBooks(); });
}

function cleanupOrphanBookCaches() {
  if (!confirm('清理找不到对应 TXT 的目录/分页缓存？阅读进度不会删除。')) return;
  fetch(API+'/api/library/cleanup-orphan-cache',{method:'POST'})
    .then(r=>r.json()).then(j=>{ alert('已清理无主缓存文件：'+(j.removed||0)+' 个'); loadBooks(); });
}

function renameFile(path) {
  const oldName = path.split('/').filter(Boolean).pop() || '';
  const name = prompt('新名称（不含路径）：', oldName);
  if (!name || !name.trim() || name.indexOf('/') >= 0) return;
  const parent = path.substring(0, path.lastIndexOf('/')) || '/';
  const dest = joinPath(parent, name.trim());
  fetch(API+'/api/files/rename',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({from:path,to:dest})})
    .then(r=>r.json())
    .then(j=>{ if (j.renamed) loadFiles(currentPath); else alert('重命名失败'); });
}

async function uploadSelectedTo(basePath) {
  const fi = $('fileInput');
  if (!fi.files.length) return;
  const m = $('uploadMsg');
  m.className = 'msg';
  m.style.display='block';
  if (basePath === '/books') {
    await fetch(API+'/api/files',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({path:'/books'})});
  }
  for (let i=0; i<fi.files.length; i++) {
    const f = fi.files[i], dest = joinPath(basePath, f.name);
    m.textContent = '上传中 '+(i+1)+'/'+fi.files.length+'：'+f.name;
    const r = await fetch(API+'/api/files/'+encodeURIComponent(dest),{method:'PUT',body:f});
    const j = await r.json();
    if (!j.ok) { m.className='msg err'; m.textContent='上传失败：'+f.name; return; }
  }
  m.className='msg ok';
  m.textContent='上传成功：'+fi.files.length+' 个文件 → '+basePath;
  fi.value='';
  currentPath = basePath;
  loadFiles(currentPath);
}

async function uploadFile() { await uploadSelectedTo(currentPath); }
async function uploadToBooks() { await uploadSelectedTo('/books'); }

function loadConfig() {
  fetch(API+'/api/config').then(r=>r.json()).then(c=>{
    $('fontSize').value = c.fontSize;
    $('lineSpacing').value = c.lineSpacing;
    $('paragraphSpacing').value = c.paragraphSpacing;
    $('indentFirstLine').value = c.indentFirstLine;
    $('marginLeft').value = c.marginLeft;
    $('marginRight').value = c.marginRight;
    $('marginTop').value = c.marginTop;
    $('marginBottom').value = c.marginBottom;
    $('justify').checked = c.justify;
    $('refreshFrequency').value = c.refreshFrequency;
    $('readerMiddleRefreshEvery').value = c.readerMiddleRefreshEvery;
    $('readerFullRefreshEvery').value = c.readerFullRefreshEvery;
    $('wifiSsid').value = c.wifiSsid||'';
    $('wifiPassword').value = '';
    $('wifiPassword').placeholder = c.wifiPassword?'●●●●●●●':'留空不修改';
    $('lockScreenImagePath').value = c.lockScreenImagePath || '';
    $('lockScreenEnabled').checked = c.lockScreenEnabled;
    $('lockScreenWakeOnDoubleClick').checked = c.lockScreenWakeOnDoubleClick;
    $('autoSleepEnabled').checked = c.autoSleepEnabled;
    $('autoSleepMinutes').value = c.autoSleepMinutes;
    $('simplifiedChinese').checked = c.simplifiedChinese;
  });
}

function saveConfig() {
  const payload = {
    fontSize: +$('fontSize').value,
    lineSpacing: +$('lineSpacing').value,
    paragraphSpacing: +$('paragraphSpacing').value,
    indentFirstLine: +$('indentFirstLine').value,
    marginLeft: +$('marginLeft').value,
    marginRight: +$('marginRight').value,
    marginTop: +$('marginTop').value,
    marginBottom: +$('marginBottom').value,
    justify: $('justify').checked,
    refreshFrequency: +$('refreshFrequency').value,
    readerMiddleRefreshEvery: +$('readerMiddleRefreshEvery').value,
    readerFullRefreshEvery: +$('readerFullRefreshEvery').value,
    wifiSsid: $('wifiSsid').value,
    lockScreenImagePath: $('lockScreenImagePath').value,
    lockScreenEnabled: $('lockScreenEnabled').checked,
    lockScreenWakeOnDoubleClick: $('lockScreenWakeOnDoubleClick').checked,
    autoSleepEnabled: $('autoSleepEnabled').checked,
    autoSleepMinutes: +$('autoSleepMinutes').value,
    simplifiedChinese: $('simplifiedChinese').checked
  };
  const wp = $('wifiPassword').value;
  if (wp) payload.wifiPassword = wp;
  fetch(API+'/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)})
    .then(r=>r.json())
    .then(j=>{
      const m = $('saveMsg');
      m.className = 'msg '+(j.saved?'ok':'err');
      m.style.display='block';
      m.textContent = j.saved?'配置已保存':'保存失败';
    });
}

function reboot() {
  if (!confirm('确定重启设备？')) return;
  fetch(API+'/api/system/reboot',{method:'POST'});
  alert('设备即将重启');
}

function loadInfo() {
  fetch(API+'/api/system/info').then(r=>r.json()).then(i=>{
    $('sysInfo').innerHTML =
      '<div>固件版本</div><div>'+i.version+'</div>'+
      '<div>可用内存</div><div>'+fmtBytes(i.freeHeap)+'</div>'+
      '<div>PSRAM 可用</div><div>'+fmtBytes(i.freePsram)+'</div>'+
      '<div>Flash 容量</div><div>'+fmtBytes(i.flashSize)+'</div>'+
      '<div>屏幕尺寸</div><div>'+i.width+' × '+i.height+'</div>'+
      '<div>SD 卡容量</div><div>'+fmtBytes(i.sdCardSize||i.sdTotal)+'</div>'+
      '<div>SD 已用</div><div>'+fmtBytes(i.sdUsed)+'</div>'+
      '<div>运行时间</div><div>'+Math.floor(i.uptimeSeconds/60)+' 分钟</div>';
  });
}
</script>
</body>
</html>
)rawliteral";

static esp_err_t rootHandler(httpd_req_t* req) {
    httpd_resp_set_hdr(req, "Content-Type", "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, s_html, strlen(s_html));
}

} // namespace webui_private

// ── WebUiService ─────────────────────────────────────────────────────────────

WebUiService g_webUi;  // global singleton

void WebUiService::begin(ConfigService* configService) {
    config_ = configService;
    webui_private::setInstance(this);
}

void WebUiService::onHttpServerStarted() {
    // Nothing needed yet; reserved for future use.
}

int WebUiService::registerHandlers(httpd_handle_t httpd) {
    using namespace webui_private;

    constexpr httpd_uri_t uris[] = {
        { "/",                HTTP_GET, rootHandler           },
        { "/index.html",       HTTP_GET, rootHandler           },
        { "/api/config",      HTTP_GET,  apiConfigGet          },
        { "/api/config",      HTTP_POST, apiConfigPost         },
        { "/api/files",       HTTP_GET,  apiFilesGet           },
        { "/api/files",       HTTP_POST, apiMkdir               },
        { "/api/files/rename", HTTP_POST, apiFileRename         },
        { "/api/files/*",     HTTP_GET,  apiFileDownload        },
        { "/api/files/*",     HTTP_PUT,  apiFilePut             },
        { "/api/files/*",     HTTP_DELETE, apiFileDelete        },
        { "/api/system/reboot", HTTP_POST, apiSystemReboot     },
        { "/api/system/info", HTTP_GET,  apiSystemInfo          },
        { "/api/library",     HTTP_GET,  apiLibraryGet          },
        { "/api/library/cleanup-cache", HTTP_POST, apiLibraryCleanupCache },
        { "/api/library/cleanup-orphan-cache", HTTP_POST, apiLibraryCleanupOrphanCache },
        { "/api/export/config", HTTP_GET, apiExportConfig        },
        { "/api/library/cleanup-all-cache", HTTP_POST, apiLibraryCleanupAllCache },
        { "/api/config",      HTTP_OPTIONS, apiOptions          },
        { "/api/files",       HTTP_OPTIONS, apiOptions          },
        { "/api/files/rename", HTTP_OPTIONS, apiOptions         },
        { "/api/files/*",     HTTP_OPTIONS, apiOptions          },
        { "/api/system/reboot", HTTP_OPTIONS, apiOptions       },
        { "/api/system/info", HTTP_OPTIONS, apiOptions         },
        { "/api/library",     HTTP_OPTIONS, apiOptions         },
        { "/api/library/cleanup-cache", HTTP_OPTIONS, apiOptions },
        { "/api/library/cleanup-orphan-cache", HTTP_OPTIONS, apiOptions },
        { "/api/export/config", HTTP_OPTIONS, apiOptions       },
        { "/api/library/cleanup-all-cache", HTTP_OPTIONS, apiOptions },
    };

    int count = 0;
    for (const auto& uri : uris) {
        if (httpd_register_uri_handler(httpd, &uri) == ESP_OK) count++;
    }
    Serial.printf("[vink3][webui] registered %d URI handlers\n", count);
    return count;
}

void WebUiService::unregisterHandlers(httpd_handle_t httpd) {
    (void)httpd;
    // esp_http_server doesn't support per-handler unregister.
}

} // namespace vink3
