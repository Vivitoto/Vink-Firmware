#include "SystemLog.h"
#include <Preferences.h>
#include <cstdarg>

namespace vink3 {

SystemLogService g_systemLog;

namespace {
static constexpr const char* kNs = "vink-log";
static constexpr const char* kSeqKey = "seq";
static constexpr const char* kCountKey = "count";

void keyFor(uint8_t slot, char* out, size_t outSize) {
    snprintf(out, outSize, "l%02u", static_cast<unsigned>(slot));
}
}

uint32_t SystemLogService::sequence() const {
    Preferences prefs;
    if (!prefs.begin(kNs, true)) return 0;
    const uint32_t seq = prefs.getUInt(kSeqKey, 0);
    prefs.end();
    return seq;
}

void SystemLogService::append(const char* text) {
    if (!text || !text[0]) return;
    Preferences prefs;
    if (!prefs.begin(kNs, false)) return;

    uint32_t seq = prefs.getUInt(kSeqKey, 0);
    uint8_t count = prefs.getUChar(kCountKey, 0);
    if (count > kMaxLines) count = kMaxLines;

    const uint8_t slot = seq % kMaxLines;
    char key[8];
    keyFor(slot, key, sizeof(key));

    char line[kLineSize];
    snprintf(line, sizeof(line), "%08lu %s", static_cast<unsigned long>(millis()), text);
    prefs.putString(key, line);
    prefs.putUInt(kSeqKey, seq + 1);
    if (count < kMaxLines) prefs.putUChar(kCountKey, count + 1);
    prefs.end();
}

void SystemLogService::appendf(const char* fmt, ...) {
    if (!fmt) return;
    char buf[kLineSize];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    append(buf);
}

void SystemLogService::clear() {
    Preferences prefs;
    if (!prefs.begin(kNs, false)) return;
    prefs.clear();
    prefs.end();
}

uint8_t SystemLogService::count() const {
    Preferences prefs;
    if (!prefs.begin(kNs, true)) return 0;
    uint8_t count = prefs.getUChar(kCountKey, 0);
    prefs.end();
    return count > kMaxLines ? kMaxLines : count;
}

bool SystemLogService::line(uint8_t index, char* out, size_t outSize) const {
    if (!out || outSize == 0) return false;
    out[0] = '\0';
    Preferences prefs;
    if (!prefs.begin(kNs, true)) return false;
    uint8_t c = prefs.getUChar(kCountKey, 0);
    if (c > kMaxLines) c = kMaxLines;
    if (index >= c) {
        prefs.end();
        return false;
    }

    const uint32_t seq = prefs.getUInt(kSeqKey, 0);
    const uint32_t first = seq >= c ? seq - c : 0;
    const uint8_t slot = (first + index) % kMaxLines;
    char key[8];
    keyFor(slot, key, sizeof(key));
    String s = prefs.getString(key, "");
    prefs.end();
    if (!s.length()) return false;
    strlcpy(out, s.c_str(), outSize);
    return true;
}

} // namespace vink3
