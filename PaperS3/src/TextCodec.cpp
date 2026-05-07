#include "TextCodec.h"
#include "GBKTable.h"
#include "Config.h"
#include "vink3/text/GbkUnicodeTable.h"

// ===== 编码检测 =====

TextEncoding TextCodec::detect(File& file) {
    if (!file) return TextEncoding::UTF8;
    
    uint32_t startPos = file.position();
    
    // 读取前 4KB 样本
    uint8_t sample[4096];
    size_t bytesRead = file.read(sample, sizeof(sample));
    file.seek(startPos);  // 恢复原位置
    
    if (bytesRead == 0) return TextEncoding::UTF8;
    
    // 检查 BOM
    if (bytesRead >= 3 && sample[0] == 0xEF && sample[1] == 0xBB && sample[2] == 0xBF) {
        Serial.println("[Codec] BOM detected: UTF-8");
        return TextEncoding::UTF8;
    }
    if (bytesRead >= 2) {
        if (sample[0] == 0xFE && sample[1] == 0xFF) {
            Serial.println("[Codec] BOM detected: UTF-16 BE (not supported)");
            return TextEncoding::UNKNOWN;
        }
        if (sample[0] == 0xFF && sample[1] == 0xFE) {
            Serial.println("[Codec] BOM detected: UTF-16 LE (not supported)");
            return TextEncoding::UNKNOWN;
        }
    }
    
    // 统计 UTF-8 有效性
    size_t utf8Valid = 0;
    size_t utf8Total = 0;
    size_t gbkValid = 0;
    size_t gbkTotal = 0;
    size_t asciiCount = 0;
    
    size_t i = 0;
    while (i < bytesRead) {
        uint8_t c = sample[i];
        
        // ASCII
        if (c < 0x80) {
            asciiCount++;
            i++;
            continue;
        }
        
        // 尝试 UTF-8 多字节序列
        size_t consumed = 0;
        if (isValidUTF8(&sample[i], bytesRead - i, consumed)) {
            utf8Valid++;
            utf8Total++;
            i += consumed;
        } else {
            utf8Total++;
            // 尝试 GBK 双字节
            if (i + 1 < bytesRead && isGBKRange(c, sample[i + 1])) {
                gbkValid++;
                gbkTotal++;
                i += 2;
            } else {
                // 既不是 UTF-8 也不是 GBK
                if (c >= 0x80) gbkTotal++;
                i++;
            }
        }
    }
    
    // 判断逻辑
    size_t nonAscii = bytesRead - asciiCount;
    
    if (nonAscii == 0) {
        // 纯 ASCII，默认 UTF-8
        return TextEncoding::UTF8;
    }
    
    float utf8Ratio = (float)utf8Valid / utf8Total;
    float gbkRatio = (gbkTotal > 0) ? (float)gbkValid / gbkTotal : 0;
    
    Serial.printf("[Codec] UTF-8 valid: %d/%d (%.1f%%), GBK valid: %d/%d (%.1f%%)\n",
                  utf8Valid, utf8Total, utf8Ratio * 100,
                  gbkValid, gbkTotal, gbkRatio * 100);
    
    if (utf8Ratio > 0.90f) {
        Serial.println("[Codec] Detected: UTF-8");
        return TextEncoding::UTF8;
    }
    
    if (gbkRatio > 0.80f && gbkRatio > utf8Ratio) {
        Serial.println("[Codec] Detected: GBK");
        return TextEncoding::GBK;
    }
    
    // 模糊情况，优先 UTF-8（误伤比 GBK 乱码更容易接受）
    Serial.println("[Codec] Uncertain, defaulting to UTF-8");
    return TextEncoding::UTF8;
}

bool TextCodec::isValidUTF8(const uint8_t* buf, size_t len, size_t& consumed) {
    uint8_t c = buf[0];
    
    if ((c & 0x80) == 0) {
        consumed = 1;
        return true;
    }
    
    size_t need;
    if ((c & 0xE0) == 0xC0) need = 2;
    else if ((c & 0xF0) == 0xE0) need = 3;
    else if ((c & 0xF8) == 0xF0) need = 4;
    else return false;
    
    if (len < need) return false;
    
    // 检查后续字节
    for (size_t i = 1; i < need; i++) {
        if ((buf[i] & 0xC0) != 0x80) return false;
    }
    
    // 验证编码范围（排除过编码）
    uint32_t codepoint;
    if (need == 2) {
        codepoint = ((c & 0x1F) << 6) | (buf[1] & 0x3F);
        if (codepoint < 0x80) return false;  // 过编码
    } else if (need == 3) {
        codepoint = ((c & 0x0F) << 12) | ((buf[1] & 0x3F) << 6) | (buf[2] & 0x3F);
        if (codepoint < 0x800) return false;
        if (codepoint >= 0xD800 && codepoint <= 0xDFFF) return false;  // 代理对
    } else {
        codepoint = ((c & 0x07) << 18) | ((buf[1] & 0x3F) << 12) | ((buf[2] & 0x3F) << 6) | (buf[3] & 0x3F);
        if (codepoint < 0x10000 || codepoint > 0x10FFFF) return false;
    }
    
    consumed = need;
    return true;
}

bool TextCodec::isGBKRange(uint8_t first, uint8_t second) {
    // GBK first byte range
    if (first < 0x81 || first > 0xFE) return false;
    // GBK second byte range (0x40-0xFE excluding 0x7F)
    if (second < 0x40 || second > 0xFE) return false;
    if (second == 0x7F) return false;
    return true;
}

// ===== GBK → Unicode 查表 =====

uint16_t TextCodec::gbkToUnicode(uint8_t first, uint8_t second) {
    uint16_t unicode = vink3::gbkToUnicode((static_cast<uint16_t>(first) << 8) | second);
    if (unicode != 0) return unicode;

    // Legacy compact GB2312 table fallback. The full ReadPaper-derived table above
    // covers novel text such as 庆余年 (澹/朕/眸/嫔/嗯 etc.); keep this as a safety net.
    if (first < GBK_ZONE_START || first >= GBK_ZONE_START + GBK_ZONE_COUNT) {
        return 0;
    }
    if (second < 0xA1 || second > 0xFE) {
        return 0;
    }
    int zone = first - GBK_ZONE_START;
    int pos  = second - 0xA1;
    int idx  = zone * GBK_POS_PER_ZONE + pos;
    if (idx < 0 || idx >= GBK_TABLE_SIZE) return 0;
    return pgm_read_word(&GBK_UNICODE_TABLE[idx]);
}

int TextCodec::unicodeToUTF8(uint16_t codepoint, uint8_t* out) {
    if (codepoint <= 0x007F) {
        out[0] = codepoint;
        return 1;
    } else if (codepoint <= 0x07FF) {
        out[0] = 0xC0 | (codepoint >> 6);
        out[1] = 0x80 | (codepoint & 0x3F);
        return 2;
    } else {
        out[0] = 0xE0 | (codepoint >> 12);
        out[1] = 0x80 | ((codepoint >> 6) & 0x3F);
        out[2] = 0x80 | (codepoint & 0x3F);
        return 3;
    }
}

// ===== GBK 转 UTF-8 文件转换 =====

String TextCodec::convertToUTF8(const char* inputPath) {
    // 生成临时文件路径
    const char* name = strrchr(inputPath, '/');
    if (!name) name = inputPath;
    else name++;
    
    char tempPath[128];
    snprintf(tempPath, sizeof(tempPath), "%s/%s_utf8.tmp", PROGRESS_DIR, name);
    
    // 如果已存在，直接返回
    if (SD.exists(tempPath)) {
        Serial.printf("[Codec] Temp file exists: %s\n", tempPath);
        return String(tempPath);
    }
    
    File inFile = SD.open(inputPath, FILE_READ);
    if (!inFile) return "";
    
    File outFile = SD.open(tempPath, FILE_WRITE);
    if (!outFile) {
        inFile.close();
        return "";
    }
    
    Serial.printf("[Codec] Converting %s → %s\n", inputPath, tempPath);
    
    GBKConverter converter;
    uint32_t totalIn = 0;
    uint32_t totalOut = 0;
    uint32_t lastPrint = 0;
    
    while (inFile.available()) {
        uint8_t buf[256];
        int len = inFile.read(buf, sizeof(buf));
        if (len <= 0) break;
        
        for (int i = 0; i < len; i++) {
            uint8_t utf8Buf[4];
            int n = converter.feedByte(buf[i], utf8Buf);
            if (n > 0) {
                outFile.write(utf8Buf, n);
                totalOut += n;
            }
        }
        
        totalIn += len;
        if (totalIn - lastPrint > 50000) {
            Serial.printf("[Codec] Converted %d bytes...\n", totalIn);
            lastPrint = totalIn;
        }
    }
    
    inFile.close();
    outFile.close();
    
    Serial.printf("[Codec] Done: %d bytes → %d bytes\n", totalIn, totalOut);
    return String(tempPath);
}

void TextCodec::cleanupTempFile(const char* path) {
    if (SD.exists(path)) {
        SD.remove(path);
        Serial.printf("[Codec] Removed temp: %s\n", path);
    }
}

// ===== GBKConverter 实现 =====

TextCodec::GBKConverter::GBKConverter() : _pending(0), _hasPending(false) {}

void TextCodec::GBKConverter::reset() {
    _hasPending = false;
    _pending = 0;
}

int TextCodec::GBKConverter::feedByte(uint8_t byte, uint8_t* outBuf) {
    if (!_hasPending) {
        if (byte < 0x80) {
            // ASCII 直接通过
            outBuf[0] = byte;
            return 1;
        }
        // 等待第二个字节
        _pending = byte;
        _hasPending = true;
        return 0;
    }
    
    // 已有等待的字节
    _hasPending = false;
    uint8_t first = _pending;
    uint8_t second = byte;
    
    // 检查是否形成有效 GBK 对
    if (isGBKRange(first, second)) {
        uint16_t unicode = gbkToUnicode(first, second);
        if (unicode != 0) {
            return unicodeToUTF8(unicode, outBuf);
        }
    }
    
    // 无效序列：输出替代字符 U+FFFD (EF BF BD)
    outBuf[0] = 0xEF;
    outBuf[1] = 0xBF;
    outBuf[2] = 0xBD;
    return 3;
}
