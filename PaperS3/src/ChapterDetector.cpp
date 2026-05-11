#include "ChapterDetector.h"
#include <ctype.h>

ChapterDetector::ChapterDetector() : _debug(false) {
}

// ══════════════════════════════════════════════════════════════════════════════
// 主入口: 扫描文件，识别所有章节
// ══════════════════════════════════════════════════════════════════════════════
int ChapterDetector::detect(File& file, ChapterDetectResult* results, int maxResults) {
    if (!file || maxResults <= 0) return 0;

    uint32_t fileSize = file.size();
    if (fileSize == 0) return 0;

    if (_debug) {
        Serial.printf("[ChapterDetector] Starting detection, fileSize=%u, maxResults=%d\n",
                      fileSize, maxResults);
    }

    const uint32_t startedMs = millis();
    int count = 0;
    int lastChapterNumber = 0;
    uint32_t lastChapterOffset = 0;
    uint32_t linesScanned = 0;

    // 重置行缓冲
    file.seek(0);
    char chunk[4096];
    uint32_t lineStartOffset = 0;
    int lineLen = 0;
    bool lineTooLong = false;

    while (file.available() && count < maxResults) {
        const uint32_t chunkStartOffset = file.position();
        const int n = file.read(reinterpret_cast<uint8_t*>(chunk), sizeof(chunk));
        if (n <= 0) break;

        for (int i = 0; i < n && count < maxResults; ++i) {
            const char c = chunk[i];

            if (c == '\n') {
                if (!lineTooLong && lineLen > 0) {
                    // 去掉行尾 \r
                    while (lineLen > 0 && _lineBuffer[lineLen - 1] == '\r') lineLen--;
                    _lineBuffer[lineLen] = '\0';

                    ChapterDetectResult result;
                    if (matchLine(_lineBuffer, lineLen, result)) {
                        result.score = scoreLine(_lineBuffer, lineLen,
                                                 result.score, result.chapterNumber,
                                                 lineStartOffset, fileSize,
                                                 lastChapterOffset);

                        // 过滤: 得分太低的不收录
                        if (result.score >= 40) {
                            // 检查是否与上一章间隔太密（小于300字节可能是同一行被重复匹配）
                            int distance = lineStartOffset - lastChapterOffset;
                            if (count == 0 || distance >= 300) {
                                result.charOffset = lineStartOffset;
                                // 单层模式: level 固定为 1
                                result.level = 1;
                                results[count] = result;
                                lastChapterOffset = lineStartOffset;
                                lastChapterNumber = result.chapterNumber;
                                count++;

                                if (_debug && count <= 10) {
                                    Serial.printf("[ChapterDetector] #%d: score=%d num=%d title=%s\n",
                                                  count, result.score, result.chapterNumber,
                                                  result.title.c_str());
                                }
                            }
                        } else if (_debug) {
                            Serial.printf("[ChapterDetector] Low score %d: %s\n",
                                          result.score, result.title.c_str());
                        }
                    }
                }

                lineLen = 0;
                lineTooLong = false;
                lineStartOffset = chunkStartOffset + static_cast<uint32_t>(i) + 1;
                linesScanned++;
                if ((linesScanned & 0xFF) == 0) yield();
                continue;
            }

            if (lineTooLong) continue;

            if (lineLen < LINE_BUF_SIZE - 1) {
                _lineBuffer[lineLen++] = c;
            } else {
                // 行太长（>511字节），丢弃
                lineTooLong = true;
                lineLen = 0;
            }
        }
    }

    // 处理最后一行（无换行符结尾）
    if (count < maxResults && lineLen > 0 && !lineTooLong) {
        while (lineLen > 0 && _lineBuffer[lineLen - 1] == '\r') lineLen--;
        _lineBuffer[lineLen] = '\0';

        ChapterDetectResult result;
        if (matchLine(_lineBuffer, lineLen, result)) {
            result.score = scoreLine(_lineBuffer, lineLen,
                                     result.score, result.chapterNumber,
                                     lineStartOffset, fileSize,
                                     lastChapterOffset);
            if (result.score >= 40) {
                int distance = lineStartOffset - lastChapterOffset;
                if (count == 0 || distance >= 300) {
                    result.charOffset = lineStartOffset;
                    result.level = 1;
                    results[count] = result;
                    count++;
                }
            }
        }
    }

    if (_debug) {
        Serial.printf("[ChapterDetector] Done: %d chapters, %lu lines, %lu ms\n",
                      count,
                      static_cast<unsigned long>(linesScanned),
                      static_cast<unsigned long>(millis() - startedMs));
    }

    return count;
}

// ══════════════════════════════════════════════════════════════════════════════
// 行匹配: 尝试所有匹配器
// ══════════════════════════════════════════════════════════════════════════════
bool ChapterDetector::matchLine(const char* line, int lineLen, ChapterDetectResult& out) {
    if (!line || lineLen <= 0 || lineLen > 200) return false;

    // 去掉行首空白
    const char* p = line;
    int n = lineLen;
    while (n > 0 && (*p == ' ' || *p == '\t' || *p == '\r')) { p++; n--; }
    // 去掉行首全角空格 U+3000
    while (n >= 3 &&
           static_cast<uint8_t>(p[0]) == 0xE3 &&
           static_cast<uint8_t>(p[1]) == 0x80 &&
           static_cast<uint8_t>(p[2]) == 0x80) {
        p += 3; n -= 3;
    }

    if (n <= 1) return false;

    // 去掉行尾空白和标点（只保留正文标题）
    const char* end = p + n;
    while (end > p && (*(end - 1) == ' ' || *(end - 1) == '\t' || *(end - 1) == '\r')) end--;
    while (end - p >= 3 &&
           static_cast<uint8_t>(end[-3]) == 0xE3 &&
           static_cast<uint8_t>(end[-2]) == 0x80 &&
           static_cast<uint8_t>(end[-1]) == 0x80) {
        end -= 3;
    }
    n = static_cast<int>(end - p);

    if (n <= 1 || n > 200) return false;

    // 过滤广告垃圾行
    if (isSpamLine(p, n)) return false;

    // 按优先级尝试各匹配器
    if (matchChineseChapter(p, n, out)) return true;
    if (matchArabicNumber(p, n, out)) return true;
    if (matchEnglishChapter(p, n, out)) return true;
    if (matchSpecialMark(p, n, out)) return true;

    return false;
}

// ══════════════════════════════════════════════════════════════════════════════
// 中文章节: 第(NUM)(关键字)
// ══════════════════════════════════════════════════════════════════════════════
bool ChapterDetector::matchChineseChapter(const char* line, int len, ChapterDetectResult& out) {
    // 必须以 "第" 开头
    if (len < 7) return false;
    if (static_cast<uint8_t>(line[0]) != 0xE7 ||
        static_cast<uint8_t>(line[1]) != 0xAC ||
        static_cast<uint8_t>(line[2]) != 0xAC) return false;

    // "第" 后面紧跟数字
    int pos = 3;
    int number = parseNumber(line, len, pos);
    if (number <= 0 || pos >= len) return false;

    // 数字后面紧跟关键字
    int kwLen = 0;
    if (!isChapterKeyword(line, len, pos, kwLen)) return false;

    int kwEnd = pos + kwLen;

    // 构建标题: 取整行作为标题
    out.title = String(line, len);
    out.score = 95;
    out.chapterNumber = number;
    out.level = -1;
    return true;
}

// ══════════════════════════════════════════════════════════════════════════════
// 阿拉伯数字章节: 1. / 001、 / 123 标题 等
// ══════════════════════════════════════════════════════════════════════════════
bool ChapterDetector::matchArabicNumber(const char* line, int len, ChapterDetectResult& out) {
    if (len < 2) return false;

    // 行首是数字
    const char* p = line;
    int n = len;

    // 跳过 【 (U+3010)
    if (n >= 3 &&
        static_cast<uint8_t>(p[0]) == 0xE3 &&
        static_cast<uint8_t>(p[1]) == 0x80 &&
        static_cast<uint8_t>(p[2]) == 0x90) {
        p += 3; n -= 3;
    }
    // 跳过 [
    if (n > 0 && p[0] == '[') { p++; n--; }

    if (n < 1) return false;

    // 解析阿拉伯数字
    if (!isdigit(p[0])) return false;

    int number = 0;
    int i = 0;
    while (i < n && isdigit(p[i])) {
        number = number * 10 + (p[i] - '0');
        i++;
    }
    if (number <= 0 || number > 10000) return false;

    // 数字后必须跟分隔符（. / 、/ ）/ 】/ 空格）
    if (i < n) {
        char sep = p[i];
        if (sep != '.' && sep != '、' && sep != ')' && sep != ' ' && sep != '\t' &&
            !(i + 2 < n &&
              static_cast<uint8_t>(p[i]) == 0xE3 &&
              static_cast<uint8_t>(p[i+1]) == 0x80 &&
              static_cast<uint8_t>(p[i+2]) == 0x91)) { // 】
            return false;
        }
    }

    // 构建标题
    char buf[64];
    snprintf(buf, sizeof(buf), "第%d章", number);
    if (i + 1 < n) {
        out.title = String(buf) + " " + String(p + i + 1, n - i - 1);
    } else {
        out.title = String(buf);
    }
    out.score = 55;
    out.chapterNumber = number;
    out.level = -1;
    return true;
}

// ══════════════════════════════════════════════════════════════════════════════
// 英文章节: Chapter N / CHAPTER N
// ══════════════════════════════════════════════════════════════════════════════
bool ChapterDetector::matchEnglishChapter(const char* line, int len, ChapterDetectResult& out) {
    if (len < 8) return false;

    // Chapter / CHAPTER / CH
    const char* prefixes[] = {"Chapter ", "CHAPTER ", "CH ", "Ch "};
    int prefixLens[] = {8, 8, 3, 3};

    for (int p = 0; p < 4; p++) {
        int plen = prefixLens[p];
        if (len < plen + 1) continue;

        bool match = true;
        for (int i = 0; i < plen - 1; i++) {  // -1 because we compare excluding the space
            if (line[i] != prefixes[p][i]) {
                match = false;
                break;
            }
        }

        if (match && isdigit(line[plen - 1])) {
            int number = 0;
            int i = plen - 1;
            while (i < len && isdigit(line[i])) {
                number = number * 10 + (line[i] - '0');
                i++;
            }
            if (number > 0 && number < 10000) {
                out.title = String(line, len);
                out.score = 60;
                out.chapterNumber = number;
                out.level = -1;
                return true;
            }
        }
    }

    return false;
}

// ══════════════════════════════════════════════════════════════════════════════
// 特殊标记: ★ ☆ ※ ◆ ■ ▲ ● ◇ ○ 【】
// ══════════════════════════════════════════════════════════════════════════════
bool ChapterDetector::matchSpecialMark(const char* line, int len, ChapterDetectResult& out) {
    if (len < 3) return false;

    uint8_t b0 = static_cast<uint8_t>(line[0]);
    uint8_t b1 = static_cast<uint8_t>(line[1]);
    uint8_t b2 = static_cast<uint8_t>(line[2]);

    bool isMark = false;
    // ★ (E2 98 85) ☆ (E2 98 86)
    if (b0 == 0xE2 && b1 == 0x98 && (b2 == 0x85 || b2 == 0x86)) isMark = true;
    // ※ (E2 80 BB)
    else if (b0 == 0xE2 && b1 == 0x80 && b2 == 0xBB) isMark = true;
    // ◆ (E2 97 86) ■ (E2 96 A0) ▲ (E2 96 B2) ● (E2 97 8F)
    else if (b0 == 0xE2 && ((b1 == 0x97 && (b2 == 0x86 || b2 == 0x8F)) ||
                            (b1 == 0x96 && (b2 == 0xA0 || b2 == 0xB2)))) isMark = true;
    // 【 (E3 80 90)
    else if (b0 == 0xE3 && b1 == 0x80 && b2 == 0x90) isMark = true;

    if (!isMark) return false;

    out.title = String(line, len > 60 ? 60 : len);
    out.score = 30;
    out.chapterNumber = 0;
    out.level = -1;
    return true;
}

// ══════════════════════════════════════════════════════════════════════════════
// 中文数字 → 整数
// ══════════════════════════════════════════════════════════════════════════════
int ChapterDetector::chineseToNumber(const char* str, int len) const {
    if (!str || len <= 0) return -1;

    int total = 0;
    int section = 0;
    int number = 0;

    for (int i = 0; i < len; ) {
        if (i + 2 < len) {
            uint32_t cp = (static_cast<uint32_t>(static_cast<uint8_t>(str[i])) << 16) |
                          (static_cast<uint32_t>(static_cast<uint8_t>(str[i+1])) << 8) |
                          static_cast<uint8_t>(str[i+2]);

            switch (cp) {
                case 0xE99BB6: case 0xE38087: // 零 〇
                    number = 0; i += 3; continue;
                case 0xE4B880:  // 一
                    number = 1; i += 3; continue;
                case 0xE4BA8C: case 0xE4B8A4: // 二 两
                    number = 2; i += 3; continue;
                case 0xE4B889:  // 三
                    number = 3; i += 3; continue;
                case 0xE59B9B:  // 四
                    number = 4; i += 3; continue;
                case 0xE4BA94:  // 五
                    number = 5; i += 3; continue;
                case 0xE585AD:  // 六
                    number = 6; i += 3; continue;
                case 0xE4B883:  // 七
                    number = 7; i += 3; continue;
                case 0xE585AB:  // 八
                    number = 8; i += 3; continue;
                case 0xE4B99D:  // 九
                    number = 9; i += 3; continue;
                case 0xE58D81:  // 十
                    if (number == 0) number = 1;
                    section += number * 10;
                    number = 0; i += 3; continue;
                case 0xE799BE:  // 百
                    if (number == 0) number = 1;
                    section += number * 100;
                    number = 0; i += 3; continue;
                case 0xE58D83:  // 千
                    if (number == 0) number = 1;
                    section += number * 1000;
                    number = 0; i += 3; continue;
                case 0xE4B887:  // 万
                    total += (section + number) * 10000;
                    section = 0; number = 0; i += 3; continue;
                default:
                    // 非数字，结束
                    break;
            }
        }

        // 阿拉伯数字夹在中文字中间
        if (i < len && isdigit(str[i])) {
            int n = 0;
            while (i < len && isdigit(str[i])) {
                n = n * 10 + (str[i] - '0');
                i++;
            }
            number = n;
            continue;
        }

        // 全角数字 ０-９
        if (i + 2 < len &&
            static_cast<uint8_t>(str[i]) == 0xEF &&
            static_cast<uint8_t>(str[i+1]) == 0xBC) {
            uint8_t b2 = static_cast<uint8_t>(str[i+2]);
            if (b2 >= 0x90 && b2 <= 0x99) {
                number = b2 - 0x90;
                i += 3;
                continue;
            }
        }

        break;  // 非数字字符，停止
    }

    int result = total + section + number;
    return (result > 0) ? result : -1;
}

// ══════════════════════════════════════════════════════════════════════════════
// 解析数字（中文 + 阿拉伯 + 全角混合），更新 pos
// ══════════════════════════════════════════════════════════════════════════════
int ChapterDetector::parseNumber(const char* line, int len, int& pos) const {
    if (pos >= len) return -1;

    // 先尝试阿拉伯数字（最快）
    if (isdigit(line[pos])) {
        int num = 0;
        while (pos < len && isdigit(line[pos])) {
            num = num * 10 + (line[pos] - '0');
            pos++;
        }
        if (num > 0) return num;
    }

    // 全角数字 ０-９
    if (pos + 2 < len &&
        static_cast<uint8_t>(line[pos]) == 0xEF &&
        static_cast<uint8_t>(line[pos+1]) == 0xBC) {
        int num = 0;
        while (pos + 2 < len &&
               static_cast<uint8_t>(line[pos]) == 0xEF &&
               static_cast<uint8_t>(line[pos+1]) == 0xBC) {
            uint8_t b2 = static_cast<uint8_t>(line[pos+2]);
            if (b2 >= 0x90 && b2 <= 0x99) {
                num = num * 10 + (b2 - 0x90);
                pos += 3;
            } else {
                break;
            }
        }
        if (num > 0) return num;
    }

    // 中文数字
    int startPos = pos;
    int num = chineseToNumber(line + pos, len - pos);
    if (num > 0) {
        // 推进 pos 越过中文数字
        while (pos < len) {
            if (pos + 2 < len) {
                uint32_t cp = (static_cast<uint32_t>(static_cast<uint8_t>(line[pos])) << 16) |
                              (static_cast<uint32_t>(static_cast<uint8_t>(line[pos+1])) << 8) |
                              static_cast<uint8_t>(line[pos+2]);
                bool isCN = false;
                switch (cp) {
                    case 0xE99BB6: case 0xE38087: // 零〇
                    case 0xE4B880: case 0xE4BA8C: case 0xE4B8A4:
                    case 0xE4B889: case 0xE59B9B: case 0xE4BA94:
                    case 0xE585AD: case 0xE4B883: case 0xE585AB:
                    case 0xE4B99D:
                    case 0xE58D81: case 0xE799BE: case 0xE58D83: case 0xE4B887:
                        isCN = true; break;
                }
                if (isCN) { pos += 3; continue; }
            }
            break;
        }
        return num;
    }

    return -1;
}

// ══════════════════════════════════════════════════════════════════════════════
// 检查 pos 处是否是合法的章节关键字
// ══════════════════════════════════════════════════════════════════════════════
bool ChapterDetector::isChapterKeyword(const char* line, int len, int pos, int& kwLen) const {
    if (pos + 2 >= len) return false;

    uint8_t b0 = static_cast<uint8_t>(line[pos]);
    if (b0 < 0xE0) return false;

    uint32_t cp = (static_cast<uint32_t>(b0) << 16) |
                  (static_cast<uint32_t>(static_cast<uint8_t>(line[pos+1])) << 8) |
                  static_cast<uint8_t>(line[pos+2]);

    // 白名单: 合法的章节关键字
    switch (cp) {
        case 0xE7ABA0:  // 章
        case 0xE88A82:  // 节
        case 0xE58DB7:  // 卷
        case 0xE59B9E:  // 回
        case 0xE7AF87:  // 篇
        case 0xE983A8:  // 部
        case 0xE99B86:  // 集
        case 0xE5B995:  // 幕
        case 0xE68A98:  // 折
        case 0xE58899:  // 则
        case 0xE8AF9D:  // 话 (web novel episode)
        case 0xE8AEB2:  // 讲 (lecture)
        case 0xE8AFBE:  // 课 (lesson)
        case 0xE6AEB5:  // 段 (section)
        case 0xE7BAA7:  // 级 (level/stage)
        case 0xE69BB2:  // 曲 (song/movement)
        case 0xE5BA8F:  // 序 (preface)
        case 0xE8AEB0:  // 记 (record/memoir)
        case 0xE7BB88:  // 终 (finale)
            kwLen = 3;
            return true;
        default:
            return false;
    }
}

// ══════════════════════════════════════════════════════════════════════════════
// 广告垃圾行检测
// ══════════════════════════════════════════════════════════════════════════════
bool ChapterDetector::isSpamLine(const char* line, int len) const {
    if (!line || len <= 0) return false;

    for (int i = 0; i <= len - 3; ++i) {
        uint8_t b0 = static_cast<uint8_t>(line[i]);

        // 免费 (E5 85 8D E8 B4 B9)
        if (i + 5 < len &&
            b0 == 0xE5 && static_cast<uint8_t>(line[i+1]) == 0x85 && static_cast<uint8_t>(line[i+2]) == 0x8D &&
            static_cast<uint8_t>(line[i+3]) == 0xE8 && static_cast<uint8_t>(line[i+4]) == 0xB4 && static_cast<uint8_t>(line[i+5]) == 0xB9)
            return true;

        // 订阅 (E8 AE A2 E9 98 85)
        if (i + 5 < len &&
            b0 == 0xE8 && static_cast<uint8_t>(line[i+1]) == 0xAE && static_cast<uint8_t>(line[i+2]) == 0xA2 &&
            static_cast<uint8_t>(line[i+3]) == 0xE9 && static_cast<uint8_t>(line[i+4]) == 0x98 && static_cast<uint8_t>(line[i+5]) == 0x85)
            return true;

        // 加群 (E5 8A A0 E7 BE A4)
        if (i + 5 < len &&
            b0 == 0xE5 && static_cast<uint8_t>(line[i+1]) == 0x8A && static_cast<uint8_t>(line[i+2]) == 0xA0 &&
            static_cast<uint8_t>(line[i+3]) == 0xE7 && static_cast<uint8_t>(line[i+4]) == 0xBE && static_cast<uint8_t>(line[i+5]) == 0xA4)
            return true;

        // 求月票 (E6 B1 82 E6 9C 88 E7 A5 A8)
        if (i + 8 < len &&
            b0 == 0xE6 && static_cast<uint8_t>(line[i+1]) == 0xB1 && static_cast<uint8_t>(line[i+2]) == 0x82 &&
            static_cast<uint8_t>(line[i+3]) == 0xE6 && static_cast<uint8_t>(line[i+4]) == 0x9C && static_cast<uint8_t>(line[i+5]) == 0x88 &&
            static_cast<uint8_t>(line[i+6]) == 0xE7 && static_cast<uint8_t>(line[i+7]) == 0xA5 && static_cast<uint8_t>(line[i+8]) == 0xA8)
            return true;

        // 求推荐 (E6 B1 82 E6 8E A8 E8 8D 90)
        if (i + 8 < len &&
            b0 == 0xE6 && static_cast<uint8_t>(line[i+1]) == 0xB1 && static_cast<uint8_t>(line[i+2]) == 0x82 &&
            static_cast<uint8_t>(line[i+3]) == 0xE6 && static_cast<uint8_t>(line[i+4]) == 0x8E && static_cast<uint8_t>(line[i+5]) == 0xA8 &&
            static_cast<uint8_t>(line[i+6]) == 0xE8 && static_cast<uint8_t>(line[i+7]) == 0x8D && static_cast<uint8_t>(line[i+8]) == 0x90)
            return true;

        // 更新 (E6 9B B4 E6 96 B0)
        if (i + 5 < len &&
            b0 == 0xE6 && static_cast<uint8_t>(line[i+1]) == 0x9B && static_cast<uint8_t>(line[i+2]) == 0xB4 &&
            static_cast<uint8_t>(line[i+3]) == 0xE6 && static_cast<uint8_t>(line[i+4]) == 0x96 && static_cast<uint8_t>(line[i+5]) == 0xB0)
            return true;

        // www. / http / .com / .cn / .net / .org
        if (line[i] == 'w' && i + 3 < len && line[i+1] == 'w' && line[i+2] == 'w' && line[i+3] == '.')
            return true;
        if (line[i] == 'h' && i + 4 < len && line[i+1] == 't' && line[i+2] == 't' && line[i+3] == 'p')
            return true;
        if (line[i] == '.' && i + 1 < len) {
            char c1 = line[i+1];
            if (c1 == 'c' || c1 == 'C' || c1 == 'n' || c1 == 'N' || c1 == 'o' || c1 == 'O') {
                if (i + 3 < len) {
                    if ((c1 == 'c' || c1 == 'C') && (line[i+2] == 'o' || line[i+2] == 'O') && (line[i+3] == 'm' || line[i+3] == 'M'))
                        return true;
                    if ((c1 == 'c' || c1 == 'C') && (line[i+2] == 'n' || line[i+2] == 'N'))
                        return true;
                    if ((c1 == 'n' || c1 == 'N') && (line[i+2] == 'e' || line[i+2] == 'E') && (line[i+3] == 't' || line[i+3] == 'T'))
                        return true;
                    if ((c1 == 'o' || c1 == 'O') && (line[i+2] == 'r' || line[i+2] == 'R') && (line[i+3] == 'g' || line[i+3] == 'G'))
                        return true;
                }
            }
        }
    }
    return false;
}

// ══════════════════════════════════════════════════════════════════════════════
// 打分
// ══════════════════════════════════════════════════════════════════════════════
int ChapterDetector::scoreLine(const char* line, int len, int baseScore,
                                int chapterNumber, uint32_t offset,
                                uint32_t fileSize, int lastChapterOffset) const {
    int score = baseScore;

    // 1. 长度: 标题 2-40 字节加分，太长扣分
    if (len >= 2 && len <= 30) {
        score += 10;
    } else if (len > 60) {
        score -= 25;
    }

    // 2. 位置合理性: 太密扣分，间距合理加分
    int distance = static_cast<int>(offset) - lastChapterOffset;
    if (distance < 600) {
        score -= 35;  // 太密，可能是误报
    } else if (distance > 3000) {
        score += 5;   // 间距合理
    }

    // 3. 章节号检查
    if (chapterNumber > 0 && chapterNumber <= 5000) {
        score += 5;
    } else if (chapterNumber > 5000) {
        score -= 10;
    }

    // 4. 文件位置比例
    float progress = (fileSize > 0) ? static_cast<float>(offset) / fileSize : 0;
    if (progress > 0.95 && chapterNumber < 10) {
        score -= 10;
    }

    // 限制范围
    if (score > 100) score = 100;
    if (score < 0) score = 0;

    return score;
}
