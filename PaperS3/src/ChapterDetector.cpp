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

    const uint32_t startedMs = millis();
    int count = 0;
    int lastChapterOffset = 0;
    int lastChapterNumber = 0;
    uint32_t linesScanned = 0;
    uint32_t candidatesScanned = 0;
    uint32_t longLinesSkipped = 0;

    auto isEmptyLine = [](const char* line, int lineLen) -> bool {
        for (int i = 0; i < lineLen; i++) {
            if (line[i] > ' ') return false;
        }
        return true;
    };

    auto acceptCandidate = [&](const char* line, int lineLen, uint32_t lineStartOffset) {
        if (lineLen <= 0 || isEmptyLine(line, lineLen)) return;
        candidatesScanned++;

        ChapterDetectResult result;
        if (!matchLine(line, lineLen, result)) return;

        result.score = scoreLine(line, lineLen, result.score,
                                 result.chapterNumber, lineStartOffset, fileSize,
                                 lastChapterOffset);
        if (result.score < 50) return;

        bool accept = true;
        const bool volumeLike = result.title.indexOf("卷") >= 0 ||
                               result.title.indexOf("部") >= 0 ||
                               result.title.indexOf("集") >= 0 ||
                               result.title.indexOf("篇") >= 0;
        ChapterMarker titleMarkers[4];
        const int titleMarkerCount = extractMarkers(result.title.c_str(), result.title.length(), titleMarkers, 4);
        const bool isMultiLayerCandidate = titleMarkerCount > 1;

        // In multi-marker titles like "第一章 第一回", chapter numbers may
        // repeat (1,2,...) across groups; skip strict monotonicity checks so
        // these still stay in TOC when single-layer parsing is preferred.
        if (count > 0 && !isMultiLayerCandidate && result.chapterNumber > 0 && lastChapterNumber > 0 && !volumeLike) {
            if (result.chapterNumber == lastChapterNumber) {
                accept = false;  // Some web TXT dumps duplicate a heading as "...免费阅读".
            } else if (result.chapterNumber > lastChapterNumber + 50) {
                accept = false;  // Likely an OCR/ad/header typo inside a continuous novel.
            } else if (result.chapterNumber < lastChapterNumber && lastChapterNumber - result.chapterNumber < 50) {
                accept = false;  // Avoid short backwards jumps caused by duplicated/mislabeled headers.
            }
        }
        if (!accept) {
            if (_debug) {
                Serial.printf("[ChapterDetector] Skipped outlier: number=%d, last=%d, title=%s\n",
                              result.chapterNumber, lastChapterNumber, result.title.c_str());
            }
            return;
        }

        result.charOffset = lineStartOffset;
        results[count] = result;
        lastChapterOffset = static_cast<int>(lineStartOffset);
        lastChapterNumber = result.chapterNumber;
        count++;

        if (_debug) {
            Serial.printf("[ChapterDetector] Found #%d: score=%d, offset=%d, title=%s\n",
                          count, result.score, result.charOffset, result.title.c_str());
        }
    };

    // Fast standalone-line scanner. Most TXT chapters are a short heading on a
    // physical line by itself. Read large chunks from SD, then reject normal
    // prose lines from their first few meaningful bytes while scanning. That
    // avoids copying whole paragraphs and avoids running matchers on every line.
    auto prefixLooksLikeCandidate = [&](const char* line, int lineLen) -> bool {
        const char* p = line;
        int n = lineLen;
        while (n > 0 && (*p == ' ' || *p == '\t' || *p == '\r')) { p++; n--; }
        while (n >= 3 &&
               static_cast<uint8_t>(p[0]) == 0xE3 &&
               static_cast<uint8_t>(p[1]) == 0x80 &&
               static_cast<uint8_t>(p[2]) == 0x80) { // U+3000 ideographic space
            p += 3;
            n -= 3;
        }
        return looksLikeCandidateLine(p, n);
    };

    constexpr int kPrefixProbeBytes = 24;
    file.seek(0);
    char chunk[4096];
    uint32_t nextLineStartOffset = 0;
    int lineLen = 0;
    bool lineTooLong = false;
    bool lineCandidate = false;
    bool lineRejected = false;

    while (file.available() && count < maxResults) {
        const uint32_t chunkStartOffset = file.position();
        const int n = file.read(reinterpret_cast<uint8_t*>(chunk), sizeof(chunk));
        if (n <= 0) break;

        for (int i = 0; i < n && count < maxResults; ++i) {
            const char c = chunk[i];
            const uint32_t byteOffset = chunkStartOffset + static_cast<uint32_t>(i);
            if (c == '\n') {
                if (lineTooLong) {
                    longLinesSkipped++;
                } else if (!lineRejected) {
                    while (lineLen > 0 && _lineBuffer[lineLen - 1] == '\r') lineLen--;
                    _lineBuffer[lineLen] = '\0';
                    acceptCandidate(_lineBuffer, lineLen, nextLineStartOffset);
                }
                lineLen = 0;
                lineTooLong = false;
                lineCandidate = false;
                lineRejected = false;
                nextLineStartOffset = byteOffset + 1;
                if ((++linesScanned & 0xFF) == 0) yield();
                continue;
            }

            if (lineTooLong || lineRejected) continue;
            if (lineLen < LINE_BUF_SIZE - 1) {
                _lineBuffer[lineLen++] = c;
            } else {
                lineTooLong = true;
                lineLen = 0;
                continue;
            }

            // If the line is already long enough to identify its kind and it
            // does not look like a chapter heading, drop the rest immediately.
            // Short lines are still checked at newline so brief chapter titles
            // such as “序”/“楔子”/“1.” are not lost.
            if (!lineCandidate && lineLen >= kPrefixProbeBytes) {
                _lineBuffer[lineLen] = '\0';
                if (prefixLooksLikeCandidate(_lineBuffer, lineLen)) {
                    lineCandidate = true;
                } else {
                    lineRejected = true;
                    lineLen = 0;
                }
            }
        }
    }

    if (count < maxResults && (lineLen > 0 || lineTooLong)) {
        if (lineTooLong) {
            longLinesSkipped++;
        } else if (!lineRejected) {
            while (lineLen > 0 && _lineBuffer[lineLen - 1] == '\r') lineLen--;
            _lineBuffer[lineLen] = '\0';
            acceptCandidate(_lineBuffer, lineLen, nextLineStartOffset);
        }
    }

    // Keep TOC in single-layer mode to avoid misclassifying nested headings like
    // "第一章 第一回" as a 2-level structure. This keeps chapter count stable
    // and avoids dropping large tails in two-layer title formats.
    for (int i = 0; i < count; ++i) {
        results[i].level = 1;
    }
    if (_debug) {
        Serial.printf("[ChapterDetector] Single-layer mode: %d entries, level=1\n", count);
    }

    Serial.printf("[ChapterDetector] Detection complete: %d chapters, %lu lines, %lu candidates, %lu long skipped, %lu ms\n",
                  count,
                  static_cast<unsigned long>(linesScanned),
                  static_cast<unsigned long>(candidatesScanned),
                  static_cast<unsigned long>(longLinesSkipped),
                  static_cast<unsigned long>(millis() - startedMs));

    return count;
}


bool ChapterDetector::looksLikeCandidateLine(const char* line, int lineLen) const {
    if (!line || lineLen <= 0) return false;
    const uint8_t b0 = static_cast<uint8_t>(line[0]);
    const uint8_t b1 = lineLen > 1 ? static_cast<uint8_t>(line[1]) : 0;
    const uint8_t b2 = lineLen > 2 ? static_cast<uint8_t>(line[2]) : 0;

    // ASCII/English headings: 1., 001、, Chapter 1, CH 1, wrappers.
    if ((line[0] >= '0' && line[0] <= '9') || line[0] == '[' || line[0] == '(' ||
        line[0] == 'C' || line[0] == 'c') return true;

    if (lineLen >= 3) {
        // 第 / 正 / 【
        if ((b0 == 0xE7 && b1 == 0xAC && b2 == 0xAC) ||
            (b0 == 0xE6 && b1 == 0xAD && b2 == 0xA3) ||
            (b0 == 0xE3 && b1 == 0x80 && b2 == 0x90)) return true;

        // Common special mark headings: ★ ☆ ※ ◆ ■ ▲ ● ◇ ○
        if ((b0 == 0xE2 && b1 == 0x98 && (b2 == 0x85 || b2 == 0x86)) ||
            (b0 == 0xE2 && b1 == 0x80 && b2 == 0xBB) ||
            (b0 == 0xE2 && b1 == 0x97 && (b2 == 0x86 || b2 == 0x8F || b2 == 0x87 || b2 == 0x8B)) ||
            (b0 == 0xE2 && b1 == 0x96 && (b2 == 0xA0 || b2 == 0xB2))) return true;

        // Volume prefix: 卷 / 册 / 部 / 篇, or Chinese-number prefix used by 一卷/二部.
        if ((b0 == 0xE5 && ((b1 == 0x8D && b2 == 0xB7) || (b1 == 0x86 && b2 == 0x8C))) ||
            (b0 == 0xE9 && b1 == 0x83 && b2 == 0xA8) ||
            (b0 == 0xE7 && b1 == 0xAF && b2 == 0x87)) return true;
        if ((b0 == 0xE9 && b1 == 0x9B && b2 == 0xB6) || // 零
            (b0 == 0xE3 && b1 == 0x80 && b2 == 0x87) ||                 // 〇
            (b0 == 0xE4 && ((b1 == 0xB8 && (b2 == 0x80 || b2 == 0x83 || b2 == 0x89)) || // 一/七/三
                            (b1 == 0xBA && b2 == 0x8C) || // 二
                            (b1 == 0xB9 && b2 == 0x9D))) || // 九
            (b0 == 0xE4 && b1 == 0xB8 && b2 == 0xA4) || // 两
            (b0 == 0xE5 && ((b1 == 0x8D && (b2 == 0x81 || b2 == 0x83)) || // 十/千
                            (b1 == 0x9B && b2 == 0x9B) || // 四
                            (b1 == 0x85 && (b2 == 0xAD || b2 == 0xAB)))) || // 六/八
            (b0 == 0xE4 && b1 == 0xBA && b2 == 0x94) || // 五
            (b0 == 0xE7 && b1 == 0x99 && b2 == 0xBE) || // 百
            (b0 == 0xE4 && b1 == 0xB8 && b2 == 0x87)) return true; // 万
    }

    return false;
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

    // 截断过长的行（标题通常不超过50字）。 Then run a cheap first-byte
    // prefilter so normal prose lines do not pay for Chinese-number parsing,
    // String construction, and every matcher. This is the hot path for first
    // TOC generation on large TXT files.
    if (lineLen <= 0 || lineLen > 200) return false;
    if (!looksLikeCandidateLine(line, lineLen)) return false;

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
    // Extract ALL 第NUMBER_KEYWORD patterns. Level is NOT hardcoded per keyword;
    // global inference in detect() figures out which keywords are volumes,
    // which are chapters, from how they co-occur within single lines.
    // e.g. "第三卷 第一百零七章" → two markers → first=vol, last=ch.
    //      "第一章 少年"          → one marker  → inferred as ch.
    if (len < 4) return false;
    if (line[0] != 0xE7 || line[1] != 0xAC || line[2] != 0xAC) return false;  // "第"

    constexpr int kMaxMarkers = 4;
    ChapterMarker markers[kMaxMarkers];
    int markerCount = extractMarkers(line, len, markers, kMaxMarkers);
    if (markerCount <= 0) return false;

    // Use the LAST marker's number as chapterNumber (innermost level)
    out.title = String(line, len);
    out.score = (markerCount >= 2) ? 95 : 100;
    out.chapterNumber = markers[markerCount - 1].number;
    out.level = -1;  // resolved globally in detect() post-processing
    return true;
}

bool ChapterDetector::matchArabicChapter(const char* line, int len, ChapterDetectResult& out) {
    // Same as matchChineseChapter: extract ALL 第NUMBER_KEYWORD patterns,
    // use the last marker's number, defer level to global inference.
    if (len < 5) return false;
    if (line[0] != 0xE7 || line[1] != 0xAC || line[2] != 0xAC) return false;

    constexpr int kMaxMarkers = 4;
    ChapterMarker markers[kMaxMarkers];
    int markerCount = extractMarkers(line, len, markers, kMaxMarkers);
    if (markerCount <= 0) return false;

    out.title = String(line, len);
    out.score = (markerCount >= 2) ? 75 : 80;
    out.chapterNumber = markers[markerCount - 1].number;
    out.level = -1;
    return true;
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
                    out.level = -1;
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
                    out.level = -1;
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
                    out.level = -1;
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
        out.level = -1;
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
    out.level = -1;
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

int ChapterDetector::extractMarkers(const char* line, int len, ChapterMarker* out, int maxOut) const {
    // Extract ALL 第(NUMBER)(KEYWORD) patterns from a line.
    // NUMBER can be Chinese digits, Arabic digits, or full-width digits.
    // KEYWORD is a single CJK character (3-byte UTF-8) immediately after the number.
    // Returns count of markers found.
    int count = 0;
    int pos = 0;
    while (pos + 5 < len && count < maxOut) {
        // Find next "第" (0xE7 0xAC 0xAC)
        int di = -1;
        for (int j = pos; j + 2 < len; ++j) {
            if ((uint8_t)line[j] == 0xE7 && (uint8_t)line[j+1] == 0xAC && (uint8_t)line[j+2] == 0xAC) {
                di = j;
                break;
            }
        }
        if (di < 0) break;

        int numPos = di + 3;  // after "第"
        if (numPos >= len) { pos = di + 3; continue; }

        // Parse number (Chinese, Arabic, or full-width digits)
        int number = parseNumberAt(line, len, numPos);
        if (number <= 0 || numPos >= len) { pos = di + 3; continue; }

        // Check for CJK keyword immediately after number
        int kwLen = 0;
        if (!isCjkKeyword(line, len, numPos, kwLen)) { pos = numPos; continue; }

        out[count].number = number;
        out[count].keyword = line + numPos;
        out[count].keywordLen = kwLen;
        out[count].bytePos = di;
        count++;
        pos = numPos + kwLen;
    }
    return count;
}

int ChapterDetector::parseNumberAt(const char* line, int len, int& pos) const {
    // Parse a number at pos: Chinese digits, Arabic digits, or full-width digits.
    // Advances pos past the number. Returns numeric value, or -1 if no number.
    int number = 0;
    int chineseSection = 0;
    int chineseAccum = 0;
    bool hasDigits = false;

    while (pos < len) {
        uint8_t b0 = (uint8_t)line[pos];
        // Arabic digit
        if (line[pos] >= '0' && line[pos] <= '9') {
            int d = line[pos] - '0';
            if (chineseSection > 0 || chineseAccum > 0) {
                // Mixing Chinese and Arabic: treat Arabic as final digits
                chineseAccum += chineseSection;
                chineseSection = d;
            } else {
                number = number * 10 + d;
            }
            hasDigits = true;
            pos++;
            continue;
        }
        // Full-width digit ０-９ (0xEF 0xBC 0x90 .. 0xEF 0xBC 0x99)
        if (pos + 2 < len && b0 == 0xEF && (uint8_t)line[pos+1] == 0xBC) {
            uint8_t b2 = (uint8_t)line[pos+2];
            if (b2 >= 0x90 && b2 <= 0x99) {
                int d = b2 - 0x90;
                if (chineseSection > 0 || chineseAccum > 0) {
                    chineseAccum += chineseSection;
                    chineseSection = d;
                } else {
                    number = number * 10 + d;
                }
                hasDigits = true;
                pos += 3;
                continue;
            }
        }
        // Chinese digit / unit
        if (pos + 2 < len && b0 >= 0xE0) {
            uint32_t cp = ((uint32_t)b0 << 16) | ((uint32_t)(uint8_t)line[pos+1] << 8) | (uint8_t)line[pos+2];
            int digit = -1;
            switch (cp) {
                case 0xE99BB6: case 0xE38087: digit = 0; break;  // 零, 〇
                case 0xE4B880: digit = 1; break;  // 一
                case 0xE4BA8C: case 0xE4B8A4: digit = 2; break;  // 二, 两
                case 0xE4B889: digit = 3; break;  // 三
                case 0xE59B9B: digit = 4; break;  // 四
                case 0xE4BA94: digit = 5; break;  // 五
                case 0xE585AD: digit = 6; break;  // 六
                case 0xE4B883: digit = 7; break;  // 七
                case 0xE585AB: digit = 8; break;  // 八
                case 0xE4B99D: digit = 9; break;  // 九
                case 0xE58D81: // 十
                    if (!hasDigits) { chineseSection = 10; hasDigits = true; pos += 3; }
                    else { chineseAccum += chineseSection; chineseSection = 10; pos += 3; }
                    continue;
                case 0xE799BE: // 百
                    chineseAccum += (chineseSection > 0 ? chineseSection : 1) * 100;
                    chineseSection = 0; pos += 3; continue;
                case 0xE58D83: // 千
                    chineseAccum += (chineseSection > 0 ? chineseSection : 1) * 1000;
                    chineseSection = 0; pos += 3; continue;
                case 0xE4B887: // 万
                    chineseAccum = (chineseAccum + (chineseSection > 0 ? chineseSection : 1)) * 10000;
                    chineseSection = 0; pos += 3; continue;
                default: break;
            }
            if (digit >= 0) {
                chineseSection = digit;
                hasDigits = true;
                pos += 3;
                continue;
            }
        }
        // Not a digit or unit → done
        break;
    }

    if (chineseSection > 0 || chineseAccum > 0) {
        number += chineseAccum + chineseSection;
    }
    return hasDigits ? number : -1;
}

bool ChapterDetector::isCjkKeyword(const char* line, int len, int pos, int& kwLen) const {
    // A valid chapter keyword is a single CJK character at pos.
    // Must be 3-byte UTF-8 and one of the known chapter/section marker characters.
    // Using a WHITELIST (not a blacklist) prevents false positives like
    // "第三批", "第九场", "第二天" which would slip through a counter-word filter.
    if (pos + 2 >= len) return false;
    uint8_t b0 = (uint8_t)line[pos];
    if (b0 < 0xE0) return false;

    uint32_t cp = ((uint32_t)b0 << 16) | ((uint32_t)(uint8_t)line[pos+1] << 8) | (uint8_t)line[pos+2];

    // Known chapter/section keywords (whitelist).
    // All other single CJK characters after 第… are rejected.
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
            kwLen = 3;
            return true;
        default:
            return false;
    }
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
    if (!buf || maxLen <= 1) return 0;
    int len = file.readBytesUntil('\n', buf, maxLen - 1);
    while (len > 0 && buf[len - 1] == '\r') len--;

    // If a physical TXT line is longer than our buffer, discard the remainder.
    // Otherwise the tail of a prose paragraph can be processed as a separate
    // fake short line and accidentally become a TOC entry. readBytesUntil()
    // batches SD reads and is much faster than byte-by-byte File::read().
    if (len >= maxLen - 1) {
        // Discard overlong physical lines in buffered chunks instead of
        // byte-by-byte File::read(); long wrapped web paragraphs otherwise
        // dominate first TOC generation time.
        char discard[128];
        while (file.available()) {
            int n = file.readBytesUntil('\n', discard, sizeof(discard));
            if (n < static_cast<int>(sizeof(discard)) - 1) break;
            yield();
        }
    }
    buf[len] = '\0';
    return len;
}
