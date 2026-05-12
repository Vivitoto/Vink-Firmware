#pragma once
#include <Arduino.h>

namespace vink3 {

class SystemLogService {
public:
    static constexpr uint8_t kMaxLines = 18;
    static constexpr size_t kLineSize = 96;

    void append(const char* text);
    void appendf(const char* fmt, ...);
    void clear();
    uint8_t count() const;
    bool line(uint8_t index, char* out, size_t outSize) const;

private:
    uint32_t sequence() const;
};

extern SystemLogService g_systemLog;

} // namespace vink3
