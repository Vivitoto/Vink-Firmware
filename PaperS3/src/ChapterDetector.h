#pragma once
#include <Arduino.h>
#include <SD.h>

// 章节识别结果
struct ChapterDetectResult {
    uint32_t charOffset;    // 章节在文件中的字符偏移
    String title;           // 章节标题
    int score;              // 置信度分数 (0-100)
    int chapterNumber;      // 提取到的章节编号
    int8_t level;           // 层级: 1=章节 (统一单层)
};

class ChapterDetector {
public:
    ChapterDetector();

    // 从文件中识别章节
    // 结果写入 results 数组，maxResults 为最大数量
    // 返回实际识别到的章节数
    int detect(File& file, ChapterDetectResult* results, int maxResults);

    // 设置是否启用调试输出
    void setDebug(bool enable) { _debug = enable; }

private:
    bool _debug;

    // 行缓冲区
    static const int LINE_BUF_SIZE = 512;
    char _lineBuffer[LINE_BUF_SIZE];

    // ── 匹配器 ──
    // 尝试匹配一行，返回是否成功并填充结果
    bool matchLine(const char* line, int lineLen, ChapterDetectResult& out);

    // 中文章节: 第(NUM)(关键字) 如 第一章 / 第一百二十回
    bool matchChineseChapter(const char* line, int len, ChapterDetectResult& out);

    // 阿拉伯数字章节: 1. / 001、 / 123 章 等变体
    bool matchArabicNumber(const char* line, int len, ChapterDetectResult& out);

    // 英文章节: Chapter 1 / CHAPTER 2
    bool matchEnglishChapter(const char* line, int len, ChapterDetectResult& out);

    // 特殊标记: ★ / ☆ / ※ / ◆ / 【】
    bool matchSpecialMark(const char* line, int len, ChapterDetectResult& out);

    // ── 辅助 ──
    // 中文数字 → 整数: "一百二十三" → 123
    int chineseToNumber(const char* str, int len) const;

    // 在 line[pos:] 解析数字（中文/阿拉伯/全角），返回数值，更新 pos
    int parseNumber(const char* line, int len, int& pos) const;

    // 检查 pos 处是否是合法的章节关键字（章/回/卷/节/集/部/篇/话/讲/课/幕/折/则/段/级/曲/序/记/终）
    // 返回 true 并设置 kwLen=3
    bool isChapterKeyword(const char* line, int len, int pos, int& kwLen) const;

    // 检查是否是广告/垃圾行（免费、订阅、加群、求月票、URL等）
    bool isSpamLine(const char* line, int len) const;

    // 打分
    int scoreLine(const char* line, int len, int baseScore, int chapterNumber,
                  uint32_t offset, uint32_t fileSize, int lastChapterOffset) const;
};
