#include "ChapterDetector.h"
#include <ctype.h>

// 规则表：按优先级排列
const ChapterDetector::Rule ChapterDetector::RULES[] = {
    // 中文数字章节（最强信号）
    {"第*章", 100, 0},
    {"第*回", 95, 0},
    {"第*卷", 90, 0},
    {"第*节", 85, 0},
    {"第*集", 85, 0},

    // 阿拉伯数字
    {"第*章", 80, 1},
    {"第*回", 75, 1},
    {"*.", 60, 1},
    {"*、", 55, 1},

    // 英文
    {"Chapter *", 70, 2},
    {"CHAPTER *", 70, 2},
    {"CH *", 65, 2},

    // 特殊标记
    {"★", 40, 0},
    {"☆", 40, 0},
    {"◆", 40, 0},
    {"▲", 40, 0},
    {"【*】", 35, 0},
};

const int ChapterDetector::RULE_COUNT = sizeof(RULES) / sizeof(RULES[0]);

ChapterDetector::ChapterDetector() : _debug(false) {
}

int ChapterDetector::detect(File& file, ChapterDetectResult* results, int maxResults) {
    if (!file || maxResults <= 0) return 0;

    uint32_t fileSize = file.size();
    if (fileSize == 0) return 0;

    if (_debug) {
        Serial.printf("[ChapterDetector] Starting detection, fileSize=%d, maxResults=%d\n",
                      fileSize, maxResults);
    }

    int count = 0;
    int lastChapterOffset = 0;
    int lastChapterNumber = 0;
    int consecutiveEmptyLines = 0;

    file.seek(0);

    while (file.available() && count < maxResults) {
        uint32_t lineStartOffset = file.position();
        int lineLen = readLine(file, _lineBuffer, LINE_BUF_SIZE);
        if (lineLen <= 0) {
            if (file.available()) {
                consecutiveEmptyLines++;
                continue;
            }
            break;
        }

        // 跳过空行
        bool isEmpty = true;
        for (int i = 0; i < lineLen; i++) {
            if (_lineBuffer[i] > ' ') {
                isEmpty = false;
                break;
            }
        }

        if (isEmpty) {
            consecutiveEmptyLines++;
            continue;
        }

        // 尝试匹配章节
        ChapterDetectResult result;
        if (matchLine(_lineBuffer, lineLen, result)) {
            // 启发式打分
            result.score = scoreLine(_lineBuffer, lineLen, result.score,
                                     result.chapterNumber, lineStartOffset, fileSize,
                                     lastChapterOffset);

            if (result.score >= 50) {  // 只保留高置信度结果
                bool accept = true;
                const bool volumeLike = result.title.indexOf("卷") >= 0 ||
                                        result.title.indexOf("部") >= 0 ||
                                        result.title.indexOf("集") >= 0 ||
                                        result.title.indexOf("篇") >= 0;
                if (count > 0 && result.chapterNumber > 0 && lastChapterNumber > 0 && !volumeLike) {
                    if (result.chapterNumber == lastChapterNumber) {
                        accept = false;  // Some web TXT dumps duplicate a heading as "...免费阅读".
                    } else if (result.chapterNumber > lastChapterNumber + 50) {
                        accept = false;  // Likely an OCR/ad/header typo inside a continuous novel.
                    } else if (result.chapterNumber < lastChapterNumber && lastChapterNumber - result.chapterNumber < 50) {
                        accept = false;  // Avoid short backwards jumps caused by duplicated/mislabeled headers.
                    }
                }
                if (accept) {
                    result.charOffset = lineStartOffset;
                    results[count] = result;
                    lastChapterOffset = static_cast<int>(lineStartOffset);
                    lastChapterNumber = result.chapterNumber;
                    count++;

                    if (_debug) {
                        Serial.printf("[ChapterDetector] Found #%d: score=%d, offset=%d, title=%s\n",
                                      count, result.score, result.charOffset, result.title.c_str());
                    }
                } else if (_debug) {
                    Serial.printf("[ChapterDetector] Skipped outlier: number=%d, last=%d, title=%s\n",
                                  result.chapterNumber, lastChapterNumber, result.title.c_str());
                }
            }
        }

        consecutiveEmptyLines = 0;
    }

    if (_debug) {
        Serial.printf("[ChapterDetector] Detection complete: %d chapters found\n", count);
    }

    return count;
}

bool ChapterDetector::matchLine(const char* line, int lineLen, ChapterDetectResult& out) {
    auto trimHeading = [](const char*& p, int& n) {
        bool changed = true;
        while (changed && n > 0) {
            changed = false;
            while (n > 0 && (*p == ' ' || *p == '\t' || *p == '\r')) { p++; n--; changed = true; }
            while (n >= 3 &&
                   static_cast<uint8_t>(p[0]) == 0xE3 &&
                   static_cast<uint8_t>(p[1]) == 0x80 &&
                   static_cast<uint8_t>(p[2]) == 0x80) { // U+3000 ideographic space
                p += 3; n -= 3; changed = true;
            }
            while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == '\t' || p[n - 1] == '\r')) { n--; changed = true; }
            while (n >= 3 &&
                   static_cast<uint8_t>(p[n - 3]) == 0xE3 &&
                   static_cast<uint8_t>(p[n - 2]) == 0x80 &&
                   static_cast<uint8_t>(p[n - 1]) == 0x80) {
                n -= 3; changed = true;
            }
        }
    };

    // Trim ASCII whitespace and common full-width/ideographic spaces. TXT novel
    // chapter headings often start with two U+3000 spaces after GBK→UTF-8 conversion.
    trimHeading(line, lineLen);

    // Web TXT often wraps headings as 【第十二章】 or prefixes them as “正文 第三章”.
    // Strip only presentation wrappers/prefixes here; actual scoring still happens below.
    if (lineLen >= 6 &&
        static_cast<uint8_t>(line[0]) == 0xE3 && static_cast<uint8_t>(line[1]) == 0x80 && static_cast<uint8_t>(line[2]) == 0x90 && // 【
        static_cast<uint8_t>(line[lineLen - 3]) == 0xE3 && static_cast<uint8_t>(line[lineLen - 2]) == 0x80 && static_cast<uint8_t>(line[lineLen - 1]) == 0x91) { // 】
        line += 3;
        lineLen -= 6;
        trimHeading(line, lineLen);
    }
    if (lineLen >= 2 && ((line[0] == '[' && line[lineLen - 1] == ']') ||
                         (line[0] == '(' && line[lineLen - 1] == ')'))) {
        line++;
        lineLen -= 2;
        trimHeading(line, lineLen);
    }
    if (lineLen >= 6 &&
        static_cast<uint8_t>(line[0]) == 0xE6 && static_cast<uint8_t>(line[1]) == 0xAD && static_cast<uint8_t>(line[2]) == 0xA3 &&
        static_cast<uint8_t>(line[3]) == 0xE6 && static_cast<uint8_t>(line[4]) == 0x96 && static_cast<uint8_t>(line[5]) == 0x87) { // 正文
        const char* p = line + 6;
        int n = lineLen - 6;
        while (n > 0 && (*p == ' ' || *p == '\t' || *p == ':' || *p == '-')) { p++; n--; }
        if (n >= 3 && static_cast<uint8_t>(p[0]) == 0xEF && static_cast<uint8_t>(p[1]) == 0xBC && static_cast<uint8_t>(p[2]) == 0x9A) { p += 3; n -= 3; } // ：
        trimHeading(p, n);
        if (n > 0) { line = p; lineLen = n; }
    }

    // 截断过长的行（标题通常不超过50字）
    if (lineLen <= 0 || lineLen > 200) return false;

    // 尝试各种匹配器
    if (matchChineseChapter(line, lineLen, out)) return true;
    if (matchArabicChapter(line, lineLen, out)) return true;
    if (matchEnglishChapter(line, lineLen, out)) return true;
    if (matchVolume(line, lineLen, out)) return true;
    if (matchSimpleNumber(line, lineLen, out)) return true;
    if (matchSpecialMark(line, lineLen, out)) return true;

    return false;
}

bool ChapterDetector::matchChineseChapter(const char* line, int len, ChapterDetectResult& out) {
    // 模式：第[中文数字]章/回/卷/节/集
    if (len < 4) return false;

    // 必须以"第"开头
    if (line[0] != 0xE7 || line[1] != 0xAC || line[2] != 0xAC) return false;  // "第"

    // 查找"章/回/卷/节/集/部"
    const char* keywords[] = {
        "\xE7\xAB\xA0",  // 章
        "\xE5\x9B\x9E",  // 回
        "\xE5\x8D\xB7",  // 卷
        "\xE8\x8A\x82",  // 节
        "\xE9\x9B\x86",  // 集
        "\xE9\x83\xA8",  // 部
    };
    const char* keywordNames[] = {"章", "回", "卷", "节", "集", "部"};

    for (int k = 0; k < 6; k++) {
        const char* kw = keywords[k];
        // 在 line 中查找 keyword
        for (int i = 3; i < len - 2; i++) {
            if ((unsigned char)line[i] == (unsigned char)kw[0] &&
                (unsigned char)line[i+1] == (unsigned char)kw[1] &&
                (unsigned char)line[i+2] == (unsigned char)kw[2]) {

                // 提取"第"和 keyword 之间的内容
                int numLen = i - 3;
                if (numLen > 0 && numLen < 20) {
                    int num = chineseToNumber(line + 3, numLen);
                    if (num > 0) {
                        // Preserve the book's original chapter title for display.
                        // The parsed numeric value is only for ordering/dedup heuristics.
                        out.title = String(line, len);
                        out.score = 100;
                        out.chapterNumber = num;
                        return true;
                    }
                }
            }
        }
    }

    return false;
}

bool ChapterDetector::matchArabicChapter(const char* line, int len, ChapterDetectResult& out) {
    // 模式：第[0-9]+章/回/卷/节
    if (len < 5) return false;

    // 必须以"第"开头
    if (line[0] != 0xE7 || line[1] != 0xAC || line[2] != 0xAC) return false;

    // 查找数字
    int numStart = -1, numEnd = -1;
    for (int i = 3; i < len; i++) {
        if (isdigit(line[i])) {
            if (numStart < 0) numStart = i;
            numEnd = i;
        } else if (numStart >= 0) {
            break;
        }
    }

    if (numStart < 0 || numEnd < 0) return false;

    // 解析数字
    int num = 0;
    for (int i = numStart; i <= numEnd; i++) {
        num = num * 10 + (line[i] - '0');
    }

    // 检查后面的关键字
    int kwStart = numEnd + 1;
    if (kwStart >= len) return false;

    const char* keywords[] = {
        "\xE7\xAB\xA0",  // 章
        "\xE5\x9B\x9E",  // 回
        "\xE5\x8D\xB7",  // 卷
        "\xE8\x8A\x82",  // 节
    };
    const char* keywordNames[] = {"章", "回", "卷", "节"};

    for (int k = 0; k < 4; k++) {
        const char* kw = keywords[k];
        if (kwStart + 2 < len &&
            (unsigned char)line[kwStart] == (unsigned char)kw[0] &&
            (unsigned char)line[kwStart+1] == (unsigned char)kw[1] &&
            (unsigned char)line[kwStart+2] == (unsigned char)kw[2]) {

            // Preserve original title text; keep `num` only for ordering/dedup.
            out.title = String(line, len);
            out.score = 80;
            out.chapterNumber = num;
            return true;
        }
    }

    return false;
}

bool ChapterDetector::matchEnglishChapter(const char* line, int len, ChapterDetectResult& out) {
    // 模式：Chapter/CHAPTER/CH [0-9]+
    if (len < 8) return false;

    const char* prefixes[] = {"Chapter", "CHAPTER", "CH", "Ch"};
    int prefixLens[] = {7, 7, 2, 2};

    for (int p = 0; p < 4; p++) {
        int plen = prefixLens[p];
        if (len < plen + 2) continue;

        bool match = true;
        for (int i = 0; i < plen; i++) {
            if (line[i] != prefixes[p][i]) {
                match = false;
                break;
            }
        }

        if (match) {
            // 查找后面的数字
            int numStart = -1;
            for (int i = plen; i < len; i++) {
                if (isdigit(line[i])) {
                    numStart = i;
                    break;
                } else if (line[i] != ' ' && line[i] != '.') {
                    break;
                }
            }

            if (numStart >= 0) {
                int num = 0;
                int numEnd = numStart;
                while (numEnd < len && isdigit(line[numEnd])) {
                    num = num * 10 + (line[numEnd] - '0');
                    numEnd++;
                }

                if (num > 0) {
                    out.title = String("Chapter ") + String(num);
                    if (numEnd < len) {
                        out.title += " ";
                        out.title += String(line + numEnd, len - numEnd);
                    }
                    out.score = 70;
                    out.chapterNumber = num;
                    return true;
                }
            }
        }
    }

    return false;
}

bool ChapterDetector::matchVolume(const char* line, int len, ChapterDetectResult& out) {
    // 模式：[中文数字] + 卷/册/部/篇，或“卷一 / 部二 / 篇三”。
    // This matcher used to scan the whole prose line and `chineseToNumber()`
    // tolerated unrelated Chinese characters, so body text such as “一部分”、
    // “腹部”、 “皮卷” could be emitted as fake TOC entries. Keep volume matching
    // strict: number text must be at the line boundary and the keyword must be
    // followed by a separator/end, unless the normal “第X部/卷” matcher handled it.
    if (len < 3 || len > 72) return false;

    auto nextCodepoint = [](const char* s, int n, int& pos) -> uint32_t {
        if (pos >= n) return 0;
        uint8_t c = static_cast<uint8_t>(s[pos]);
        if (c < 0x80) { pos++; return c; }
        if ((c & 0xE0) == 0xC0 && pos + 1 < n) {
            uint32_t cp = ((c & 0x1F) << 6) | (static_cast<uint8_t>(s[pos + 1]) & 0x3F);
            pos += 2; return cp;
        }
        if ((c & 0xF0) == 0xE0 && pos + 2 < n) {
            uint32_t cp = ((c & 0x0F) << 12) |
                          ((static_cast<uint8_t>(s[pos + 1]) & 0x3F) << 6) |
                          (static_cast<uint8_t>(s[pos + 2]) & 0x3F);
            pos += 3; return cp;
        }
        pos++; return c;
    };
    auto isNumberText = [&](const char* s, int n) -> bool {
        if (n <= 0 || n > 24) return false;
        bool seen = false;
        for (int pos = 0; pos < n; ) {
            uint32_t cp = nextCodepoint(s, n, pos);
            const bool ok = (cp >= '0' && cp <= '9') ||
                            (cp >= U'０' && cp <= U'９') ||
                            cp == U'零' || cp == U'〇' || cp == U'一' || cp == U'二' || cp == U'两' ||
                            cp == U'三' || cp == U'四' || cp == U'五' || cp == U'六' || cp == U'七' ||
                            cp == U'八' || cp == U'九' || cp == U'十' || cp == U'百' || cp == U'千' || cp == U'万';
            if (!ok) return false;
            seen = true;
        }
        return seen;
    };
    auto isSeparatorOrEnd = [&](int pos) -> bool {
        if (pos >= len) return true;
        if (line[pos] == ' ' || line[pos] == '\t' || line[pos] == ':' || line[pos] == '-' || line[pos] == '.') return true;
        if (pos + 2 < len && static_cast<uint8_t>(line[pos]) == 0xE3 && static_cast<uint8_t>(line[pos + 1]) == 0x80 &&
            (static_cast<uint8_t>(line[pos + 2]) == 0x80 || static_cast<uint8_t>(line[pos + 2]) == 0x81)) return true; // fullwidth space / 、
        if (pos + 2 < len && static_cast<uint8_t>(line[pos]) == 0xEF && static_cast<uint8_t>(line[pos + 1]) == 0xBC &&
            static_cast<uint8_t>(line[pos + 2]) == 0x9A) return true; // ：
        return false;
    };

    const char* keywords[] = {
        "\xE5\x8D\xB7",  // 卷
        "\xE5\x86\x8C",  // 册
        "\xE9\x83\xA8",  // 部
        "\xE7\xAF\x87",  // 篇
    };

    for (int k = 0; k < 4; k++) {
        const char* kw = keywords[k];

        // Prefix form: 卷一 风起 / 部2 旧事
        if (len > 3 &&
            static_cast<uint8_t>(line[0]) == static_cast<uint8_t>(kw[0]) &&
            static_cast<uint8_t>(line[1]) == static_cast<uint8_t>(kw[1]) &&
            static_cast<uint8_t>(line[2]) == static_cast<uint8_t>(kw[2])) {
            int numEnd = 3;
            while (numEnd < len && !isSeparatorOrEnd(numEnd)) numEnd++;
            if (isNumberText(line + 3, numEnd - 3)) {
                int num = chineseToNumber(line + 3, numEnd - 3);
                if (num > 0) {
                    out.title = String(line, len);
                    out.score = 70;
                    out.chapterNumber = num;
                    return true;
                }
            }
        }

        // Infix form: 一卷 风起 / 2部 旧事. Reject prose like “一部分”.
        for (int i = 1; i < len - 2; i++) {
            if ((unsigned char)line[i] == (unsigned char)kw[0] &&
                (unsigned char)line[i+1] == (unsigned char)kw[1] &&
                (unsigned char)line[i+2] == (unsigned char)kw[2] &&
                isNumberText(line, i) && isSeparatorOrEnd(i + 3)) {
                int num = chineseToNumber(line, i);
                if (num > 0) {
                    out.title = String(line, len);
                    out.score = 60;
                    out.chapterNumber = num;
                    return true;
                }
            }
        }
    }

    return false;
}

bool ChapterDetector::matchSimpleNumber(const char* line, int len, ChapterDetectResult& out) {
    // 纯数字行（如 "1.", "123", "第一章" 的变体）
    if (len < 1 || len > 10) return false;

    // 检查是否是纯数字或数字+标点
    int num = 0;
    int numLen = 0;
    for (int i = 0; i < len; i++) {
        if (isdigit(line[i])) {
            num = num * 10 + (line[i] - '0');
            numLen++;
        } else if (line[i] == '.' || line[i] == ' ') {
            // 允许 ASCII 标点和空格
        } else if (i + 2 < len &&
                   static_cast<uint8_t>(line[i]) == 0xE3 &&
                   static_cast<uint8_t>(line[i + 1]) == 0x80 &&
                   static_cast<uint8_t>(line[i + 2]) == 0x81) {
            // UTF-8 '、'
            i += 2;
        } else {
            return false;  // 包含非数字非标点字符
        }
    }

    if (numLen > 0 && num > 0 && num < 10000) {
        // 检查前后是否有空行（标题特征）
        out.title = String("第") + String(num) + String("章");
        out.score = 40;
        out.chapterNumber = num;
        return true;
    }

    return false;
}

bool ChapterDetector::matchSpecialMark(const char* line, int len, ChapterDetectResult& out) {
    // 特殊符号开头
    if (len < 2) return false;

    const char* marks = "★☆※◆■▲●◇○";
    bool hasMark = false;
    for (int i = 0; marks[i]; i += 3) {  // UTF-8 中文字符
        if (len >= 3 && (unsigned char)line[0] == (unsigned char)marks[i] &&
            (unsigned char)line[1] == (unsigned char)marks[i+1] &&
            (unsigned char)line[2] == (unsigned char)marks[i+2]) {
            hasMark = true;
            break;
        }
    }

    if (!hasMark) return false;

    out.title = String(line, len > 30 ? 30 : len);
    out.score = 30;
    out.chapterNumber = 0;
    return true;
}

int ChapterDetector::chineseToNumber(const char* str, int len) {
    int total = 0;
    int section = 0;
    int number = 0;

    auto nextCodepoint = [](const char* s, int n, int& pos) -> uint32_t {
        if (pos >= n) return 0;
        uint8_t c = static_cast<uint8_t>(s[pos]);
        if (c < 0x80) {
            pos++;
            return c;
        }
        if ((c & 0xE0) == 0xC0 && pos + 1 < n) {
            uint32_t cp = ((c & 0x1F) << 6) | (static_cast<uint8_t>(s[pos + 1]) & 0x3F);
            pos += 2;
            return cp;
        }
        if ((c & 0xF0) == 0xE0 && pos + 2 < n) {
            uint32_t cp = ((c & 0x0F) << 12) |
                          ((static_cast<uint8_t>(s[pos + 1]) & 0x3F) << 6) |
                          (static_cast<uint8_t>(s[pos + 2]) & 0x3F);
            pos += 3;
            return cp;
        }
        pos++;
        return c;
    };

    auto digitValue = [](uint32_t cp) -> int {
        switch (cp) {
            case U'零': case U'〇': return 0;
            case U'一': return 1;
            case U'二': case U'两': return 2;
            case U'三': return 3;
            case U'四': return 4;
            case U'五': return 5;
            case U'六': return 6;
            case U'七': return 7;
            case U'八': return 8;
            case U'九': return 9;
            default: return -1;
        }
    };

    for (int i = 0; i < len; ) {
        uint32_t cp = nextCodepoint(str, len, i);
        if ((cp >= '0' && cp <= '9') || (cp >= U'０' && cp <= U'９')) {
            int n = (cp <= '9') ? static_cast<int>(cp - '0') : static_cast<int>(cp - U'０');
            while (i < len) {
                int before = i;
                uint32_t next = nextCodepoint(str, len, i);
                if (next >= '0' && next <= '9') {
                    n = n * 10 + static_cast<int>(next - '0');
                    continue;
                }
                if (next >= U'０' && next <= U'９') {
                    n = n * 10 + static_cast<int>(next - U'０');
                    continue;
                }
                i = before;
                break;
            }
            number = n;
            continue;
        }

        int digit = digitValue(cp);
        if (digit >= 0) {
            number = digit;
            continue;
        }

        int unit = 0;
        if (cp == U'十') unit = 10;
        else if (cp == U'百') unit = 100;
        else if (cp == U'千') unit = 1000;
        else if (cp == U'万') {
            total += (section + number) * 10000;
            section = 0;
            number = 0;
            continue;
        }

        if (unit > 0) {
            if (number == 0) number = 1;
            section += number * unit;
            number = 0;
        }
    }

    int result = total + section + number;
    return result > 0 ? result : -1;
}

int ChapterDetector::scoreLine(const char* line, int len, int baseScore, int chapterNumber,
                                uint32_t offset, uint32_t fileSize, int lastChapterOffset) {
    int score = baseScore;

    // 1. 行长度检查（标题通常 2-40 字符）
    if (len >= 2 && len <= 20) {
        score += 15;
    } else if (len > 50) {
        score -= 30;  // 太长不像标题
    }

    // 2. 位置合理性（章节不能太密）
    int distance = offset - lastChapterOffset;
    if (distance < 500) {
        score -= 40;  // 太密，可能是误报
    } else if (distance > 2000) {
        score += 10;  // 间距合理
    }

    // 3. 章节号递增检查
    if (chapterNumber > 0) {
        // 正常情况下章节号递增，但也允许跳号
        if (chapterNumber <= 2000) {  // 合理范围内
            score += 10;
        } else {
            score -= 20;  // 章节号过大，可能误报
        }
    }

    // 4. 文件位置比例（章节应均匀分布）
    float progress = (float)offset / fileSize;
    if (progress > 0.9 && chapterNumber < 5) {
        score -= 15;  // 文件末尾出现低章节号，可疑
    }

    // 5. 纯文本检查（标题不应有太多数字/符号）
    int chineseCount = 0, totalCount = 0;
    for (int i = 0; i < len; ) {
        if ((unsigned char)line[i] >= 0xE0) {
            chineseCount++;
            i += 3;
        } else if (line[i] > ' ') {
            totalCount++;
            i++;
        } else {
            i++;
        }
    }
    if (chineseCount > 0 && totalCount > 0) {
        float ratio = (float)chineseCount / (chineseCount + totalCount);
        if (ratio > 0.5) score += 10;
    }

    // 限制分数范围
    if (score > 100) score = 100;
    if (score < 0) score = 0;

    return score;
}

int ChapterDetector::readLine(File& file, char* buf, int maxLen) {
    int len = 0;
    bool hitNewline = false;
    while (file.available() && len < maxLen - 1) {
        char c = file.read();
        if (c == '\n') {
            hitNewline = true;
            break;
        }
        if (c != '\r') {
            buf[len++] = c;
        }
    }
    // If a physical TXT line is longer than our buffer, discard the remainder.
    // Otherwise the tail of a prose paragraph can be processed as a separate
    // fake short line and accidentally become a TOC entry.
    if (!hitNewline && len >= maxLen - 1) {
        while (file.available()) {
            char c = file.read();
            if (c == '\n') break;
        }
    }
    buf[len] = '\0';
    return len;
}
