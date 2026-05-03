#pragma once
#include <Arduino.h>
#include <SD.h>

// 章节识别结果
struct ChapterDetectResult {
    uint32_t charOffset;    // 章节在文件中的字符偏移
    String title;           // 章节标题
    int score;              // 置信度分数 (0-100)
    int chapterNumber;      // 提取到的章节编号
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
    
    // 字符缓冲区
    static const int LINE_BUF_SIZE = 512;
    char _lineBuffer[LINE_BUF_SIZE];
    
    // 规则匹配
    struct Rule {
        const char* pattern;    // 正则/关键词模式
        int baseScore;          // 基础分数
        int type;               // 0=中文数字, 1=阿拉伯数字, 2=英文
    };
    
    static const Rule RULES[];
    static const int RULE_COUNT;
    
    // 核心识别逻辑
    bool matchLine(const char* line, int lineLen, ChapterDetectResult& out);
    
    // 各种匹配器
    bool matchChineseChapter(const char* line, int len, ChapterDetectResult& out);
    bool matchArabicChapter(const char* line, int len, ChapterDetectResult& out);
    bool matchEnglishChapter(const char* line, int len, ChapterDetectResult& out);
    bool matchVolume(const char* line, int len, ChapterDetectResult& out);
    bool matchSimpleNumber(const char* line, int len, ChapterDetectResult& out);
    bool matchSpecialMark(const char* line, int len, ChapterDetectResult& out);
    
    // 辅助函数
    int chineseToNumber(const char* str, int len);  // "一百二十三" -> 123
    bool isChineseDigit(char c);
    int getChineseDigitValue(char c);
    bool isAllChinese(const char* str, int len);
    int getLineChapterNumber(const char* line, int len);
    
    // 启发式打分
    int scoreLine(const char* line, int len, int baseScore, int chapterNumber, 
                  uint32_t offset, uint32_t fileSize, int lastChapterOffset);
    
    // 行读取
    int readLine(File& file, char* buf, int maxLen);
};
