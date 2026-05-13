#include "ReaderTextRenderer.h"
#include "../../Config.h"
#include "../VinkPaperS3.h"
#include "../text/WenkaiFullFont.h"
#include "../text/CjkTextRenderer.h"
#include "../system/SystemLog.h"
#include "TtfFont.h"
#include <Preferences.h>

namespace {
constexpr uint32_t kGrayHeaderSize = 16;
constexpr uint32_t kGrayEntrySize = 16;
constexpr int16_t kReaderHeaderDividerY = 58;
constexpr int16_t kReaderBodyTopMin = 68;
constexpr int16_t kReaderFooterReserve = 48;
}

namespace vink3 {

ReaderTextRenderer g_readerText;

bool ReaderTextRenderer::begin(M5Canvas* canvas) {
    canvas_ = canvas;
    applyLayoutPresetToSettings();
    loadLocalSettings();
    // A fresh device may not have the vink-reader NVS namespace yet. In that
    // case loadLocalSettings() cannot apply a saved font, but boot must still
    // continue and draw the first page instead of halting on a white screen.
    if (!ready()) loadDefaultFont();
    return canvas_ && ready();
}

bool ReaderTextRenderer::loadDefaultFont() {
    // Reader body font: 32px Wenkai PROGMEM is the primary font.
    // SPIFFS fonts provide 16px/20px fallback.
    if (beginWenkai32Font()) {
        Serial.printf("[vink3][reader] Wenkai 32px PROGMEM font loaded: glyphs=%lu size=%lu\n",
                      static_cast<unsigned long>(wenkai32CharCount_),
                      static_cast<unsigned long>(g_wenkai_font32_size));
        return true;
    }
    if (font_.loadBundledFont(FONT_FILE_24)) {
        Serial.println("[vink3][reader] bundled 24px reader font loaded");
        return true;
    }
    if (font_.loadBundledFont(FONT_FILE_20)) {
        Serial.println("[vink3][reader] bundled 20px reader font loaded");
        return true;
    }
    if (font_.loadBundledFont(FONT_FILE_16)) {
        Serial.println("[vink3][reader] bundled 16px reader font loaded");
        return true;
    }
    Serial.println("[vink3][reader] reader font load failed");
    return false;
}

bool ReaderTextRenderer::loadFont(const char* path) {
    if (!path || !path[0]) return loadDefaultFont();
    return font_.loadFont(path);
}

bool ReaderTextRenderer::ready() const {
    return canvas_ && (wenkai32Ready_ || font_.isLoaded() || (sdCardFontActive_ && ttfFont_.isLoaded()));
}

uint16_t ReaderTextRenderer::fontSize() const {
    if (sdCardFontActive_ && ttfFont_.isLoaded()) return ttfFont_.currentSize();
    if (wenkai32Ready_) return wenkai32FontHeight_;
    return font_.isLoaded() ? font_.getFontSize() : fontSizeSetting_;
}

void ReaderTextRenderer::applyReaderFontSize(uint8_t size, bool persist) {
    // Until smaller generated reader fonts are proven on-device, keep the body
    // renderer on the compiled full Wenkai font. The older 16/20px SPIFFS fonts
    // are useful as UI/fallback resources but can silently miss large CJK ranges,
    // which presents as a page with normal header/footer and completely blank
    // body text. Prefer readable embedded Wenkai over honoring a stale small-size
    // preference.
    (void)size;
    fontSizeSetting_ = 32;
    font_.unload();
    beginWenkai32Font();
    if (persist) saveLocalSettings();
}

void ReaderTextRenderer::setReaderFontSize(uint8_t size) {
    applyReaderFontSize(size, true);
}

void ReaderTextRenderer::applyLayoutPresetToSettings() {
    // Only mutate layout-related fields (formatting1, spacing).
    // Leave renderOpt1 (underline, antiAlias, pageTurnEffect) untouched
    // so independent toggles survive preset changes.
    auto on = [](uint16_t& raw, uint8_t idx) { ReaderSettings::setSlot(raw, idx, 1); };
    auto off = [](uint16_t& raw, uint8_t idx) { ReaderSettings::setSlot(raw, idx, 0); };
    auto level = [](uint16_t& raw, uint8_t idx, uint8_t value) { ReaderSettings::setSlot(raw, idx, value); };

    // Reset only layout slots
    settings_.formatting1 = 0;
    settings_.spacing = 0;

    switch (layoutPreset_) {
        case 0:
            // 原始：Vink packed model with all formatting optimizations off.
            settings_.showMode = ReaderShowMode::Std;
            settings_.flashPeriod = ReaderFlashPeriod::Pages10;
            break;
        case 2:
            // 紧凑：same Vink options, tighter spacing slots.
            on(settings_.formatting1, 0); // Indent
            on(settings_.formatting1, 1); // BlankLineOpt
            on(settings_.formatting1, 2); // BreakLineOpt
            on(settings_.formatting1, 3); // NewPage
            on(settings_.formatting1, 4); // DynamicLineHeight
            level(settings_.spacing, 0, 0);
            level(settings_.spacing, 1, 0);
            level(settings_.spacing, 2, 0);
            level(settings_.spacing, 3, 1);
            level(settings_.spacing, 4, 0);
            level(settings_.spacing, 5, 1);
            settings_.showMode = ReaderShowMode::Fast;
            settings_.flashPeriod = ReaderFlashPeriod::Pages20;
            break;
        case 1:
        default:
            // 优化：Vink-native default behavior built around the formal
            // formatting1/render_opt1/spacing option groups.
            on(settings_.formatting1, 0); // Indent
            on(settings_.formatting1, 1); // BlankLineOpt
            on(settings_.formatting1, 2); // BreakLineOpt
            on(settings_.formatting1, 3); // NewPage
            on(settings_.formatting1, 4); // DynamicLineHeight
            level(settings_.spacing, 0, 1);
            level(settings_.spacing, 1, 1);
            level(settings_.spacing, 2, 1);
            level(settings_.spacing, 3, 1);
            level(settings_.spacing, 4, 1);
            level(settings_.spacing, 5, 1);
            settings_.showMode = ReaderShowMode::Std;
            settings_.flashPeriod = ReaderFlashPeriod::Pages10;
            break;
    }
}

void ReaderTextRenderer::loadLocalSettings() {
    Preferences prefs;
    // On some devices Preferences::begin(name, true) (read-only) can
    // fail if the NVS namespace was never created, returning false even
    // though the NVS subsystem is healthy. Fall back to read-write mode
    // which creates the namespace on first access.
    bool opened = prefs.begin("vink-reader", true);
    if (!opened) {
        opened = prefs.begin("vink-reader", false);
        if (!opened) {
            Serial.println("[vink3][reader] loadLocalSettings: NVS open failed (RO+RW)");
            g_systemLog.append("reader NVS open fail");
            return;
        }
        Serial.println("[vink3][reader] loadLocalSettings: opened in RW mode (fresh namespace)");
    }
    const uint8_t preset = prefs.getUChar("preset", layoutPreset_);
    const uint16_t formatting = prefs.getUShort("fmt1", settings_.formatting1);
    const uint16_t render = prefs.getUShort("rend1", settings_.renderOpt1);
    const uint16_t spacing = prefs.getUShort("spacing", settings_.spacing);
    const bool hasSaved = prefs.isKey("rend1");
    fontSizeSetting_ = prefs.getUChar("font", fontSizeSetting_);
    sdFontSize_ = prefs.getUChar("sdsz", sdFontSize_);
    if (sdFontSize_ < 16) sdFontSize_ = 16;
    if (sdFontSize_ > 64) sdFontSize_ = 64;
    webLineSpacing_ = prefs.getUChar("line", webLineSpacing_);
    webParagraphSpacing_ = prefs.getUChar("para", webParagraphSpacing_);
    webIndentFirstLine_ = prefs.getUChar("indent", webIndentFirstLine_);
    webMarginLeft_ = prefs.getUChar("mleft", webMarginLeft_);
    webMarginTop_ = prefs.getUChar("mtop", webMarginTop_);
    webMarginBottom_ = prefs.getUChar("mbot", webMarginBottom_);
    // v0.4.6 adds compact reader chrome; migrate old defaults in RAM only so
    // existing devices use the reclaimed text area without touching NVS at boot.
    if (webLineSpacing_ == 60) webLineSpacing_ = 50;
    if (webMarginTop_ == 78) webMarginTop_ = kReaderBodyTopMin;
    if (webMarginBottom_ == 34) webMarginBottom_ = 48;
    webJustify_ = prefs.getBool("justify", webJustify_);
    prefs.end();

    if (preset <= 2) layoutPreset_ = preset;
    applyLayoutPresetToSettings();
    settings_.formatting1 = formatting;
    settings_.renderOpt1 = render;
    settings_.spacing = spacing;
    Serial.printf("[vink3][reader] loadLocalSettings: hasSaved=%d preset=%u fmt1=0x%04x rend1=0x%04x spacing=0x%04x font=%u\n",
                  hasSaved ? 1 : 0, preset, formatting, render, spacing, fontSizeSetting_);
    g_systemLog.appendf("reader load rend1=0x%04x preset=%u", render, preset);
    // Apply persisted font choice without writing NVS during boot.
    applyReaderFontSize(fontSizeSetting_, false);
}

bool ReaderTextRenderer::saveLocalSettings() const {
    Preferences prefs;
    if (!prefs.begin("vink-reader", false)) {
        Serial.println("[vink3][reader] saveLocalSettings: NVS open failed");
        return false;
    }
    prefs.putUChar("preset", layoutPreset_);
    prefs.putUShort("fmt1", settings_.formatting1);
    prefs.putUShort("rend1", settings_.renderOpt1);
    prefs.putUShort("spacing", settings_.spacing);
    prefs.putUChar("font", fontSizeSetting_);
    prefs.putUChar("sdsz", sdFontSize_);
    prefs.putUChar("line", webLineSpacing_);
    prefs.putUChar("para", webParagraphSpacing_);
    prefs.putUChar("indent", webIndentFirstLine_);
    prefs.putUChar("mleft", webMarginLeft_);
    prefs.putUChar("mtop", webMarginTop_);
    prefs.putUChar("mbot", webMarginBottom_);
    prefs.putBool("justify", webJustify_);
    prefs.end();
    return true;
}

void ReaderTextRenderer::toggleAntiAlias() {
    if (settings_.schema == 0) applyLayoutPresetToSettings();
    setAntiAlias(!antiAliasEnabled());
}

void ReaderTextRenderer::setAntiAlias(bool enabled) {
    if (settings_.schema == 0) applyLayoutPresetToSettings();
    ReaderSettings::setSlot(settings_.renderOpt1, 1, enabled ? 1 : 0);
    saveLocalSettings();
    Serial.printf("[vink3][reader] anti-alias -> %s\n", antiAliasLabel());
}

void ReaderTextRenderer::toggleUnderline() {
    if (settings_.schema == 0) applyLayoutPresetToSettings();
    setUnderline(!underlineEnabled());
}

void ReaderTextRenderer::setUnderline(bool enabled) {
    if (settings_.schema == 0) applyLayoutPresetToSettings();
    ReaderSettings::setSlot(settings_.renderOpt1, 0, enabled ? 1 : 0);
    saveLocalSettings();
    Serial.printf("[vink3][reader] Vink underline -> %s render_opt1=0x%04x\n", underlineLabel(), settings_.renderOpt1);
}

void ReaderTextRenderer::togglePageTurnEffect() {
    if (settings_.schema == 0) applyLayoutPresetToSettings();
    setPageTurnEffect(!pageTurnEffectEnabled());
}

void ReaderTextRenderer::setPageTurnEffect(bool enabled) {
    if (settings_.schema == 0) applyLayoutPresetToSettings();
    ReaderSettings::setSlot(settings_.renderOpt1, 3, enabled ? 1 : 0);
    saveLocalSettings();
    Serial.printf("[vink3][reader] Vink page-turn effect -> %s render_opt1=0x%04x\n", pageTurnEffectLabel(), settings_.renderOpt1);
}

void ReaderTextRenderer::toggleDoubleTapUnlock() {
    if (settings_.schema == 0) applyLayoutPresetToSettings();
    setDoubleTapUnlock(!doubleTapUnlockEnabled());
}

void ReaderTextRenderer::setDoubleTapUnlock(bool enabled) {
    if (settings_.schema == 0) applyLayoutPresetToSettings();
    ReaderSettings::setSlot(settings_.renderOpt1, 6, enabled ? 1 : 0);
    saveLocalSettings();
    Serial.printf("[vink3][reader] double-tap lock/unlock -> %s render_opt1=0x%04x\n", doubleTapUnlockLabel(), settings_.renderOpt1);
}

void ReaderTextRenderer::cyclePageMargin() {
    if (settings_.schema == 0) applyLayoutPresetToSettings();
    const uint8_t next = (max(settings_.topBottomLevel(), settings_.leftRightLevel()) + 1) & 0x03;
    setPageMarginLevel(next);
}

void ReaderTextRenderer::setPageMarginLevel(uint8_t level) {
    if (settings_.schema == 0) applyLayoutPresetToSettings();
    level &= 0x03;
    ReaderSettings::setSlot(settings_.spacing, 0, level);
    ReaderSettings::setSlot(settings_.spacing, 1, level);

    static constexpr int16_t kTopMargins[4] = {68, 78, 92, 108};
    static constexpr int16_t kBottomMargins[4] = {22, 30, 40, 52};
    static constexpr int16_t kSideMargins[4] = {20, 28, 38, 48};
    webMarginTop_ = static_cast<uint8_t>(kTopMargins[level]);
    webMarginBottom_ = static_cast<uint8_t>(kBottomMargins[level]);
    webMarginLeft_ = static_cast<uint8_t>(kSideMargins[level]);
    saveLocalSettings();
    Serial.printf("[vink3][reader] page margin -> %s spacing=0x%04x\n", pageMarginLabel(), settings_.spacing);
}

const char* ReaderTextRenderer::pageMarginLabel() const {
    const uint8_t level = max(settings_.topBottomLevel(), settings_.leftRightLevel());
    switch (level) {
        case 0: return "窄";
        case 1: return "标准";
        case 2: return "宽";
        case 3: return "超宽";
    }
    return "标准";
}

void ReaderTextRenderer::cycleLineSpacing() {
    if (settings_.schema == 0) applyLayoutPresetToSettings();
    setLineSpacingLevel((settings_.lineSpacingLevel() + 1) & 0x03);
}

void ReaderTextRenderer::setLineSpacingLevel(uint8_t level) {
    if (settings_.schema == 0) applyLayoutPresetToSettings();
    level &= 0x03;
    ReaderSettings::setSlot(settings_.spacing, 2, level);

    static constexpr int16_t kLineGaps[4] = {3, 7, 12, 17};
    const int16_t gap = kLineGaps[level];
    const uint16_t fz = fontSize();
    uint16_t percent = (static_cast<int32_t>(gap) * 100 + static_cast<int32_t>(fz / 2))
                          / static_cast<int32_t>(max<uint8_t>(fz, 1));
    if (percent > 200) percent = 200;
    webLineSpacing_ = static_cast<uint8_t>(percent);

    saveLocalSettings();
    Serial.printf("[vink3][reader] line spacing -> %s spacing=0x%04x\n", lineSpacingLabel(), settings_.spacing);
}

const char* ReaderTextRenderer::readerFontSizeLabel() const {
    switch (fontSizeSetting_) {
        case 16: return "16px";
        case 20: return "20px";
        case 32: return "32px";
        default: return "16px";
    }
}

void ReaderTextRenderer::cycleReaderFontSize() {
    uint8_t next = fontSizeSetting_;
    if (next == 16) next = 20;
    else if (next == 20) next = 32;
    else next = 16;
    setReaderFontSize(next);
}

const char* ReaderTextRenderer::lineSpacingLabel() const {
    switch (settings_.lineSpacingLevel()) {
        case 0: return "紧凑";
        case 1: return "标准";
        case 2: return "宽松";
        case 3: return "超宽";
    }
    return "标准";
}

void ReaderTextRenderer::cycleLayoutPreset() {
    setLayoutPreset((layoutPreset_ + 1) % 3);
}

void ReaderTextRenderer::setLayoutPreset(uint8_t preset) {
    layoutPreset_ = preset % 3;
    applyLayoutPresetToSettings();
    saveLocalSettings();
    Serial.printf("[vink3][reader] Vink layout preset -> %s formatting1=0x%04x render_opt1=0x%04x spacing=0x%04x\n",
                  layoutPresetLabel(), settings_.formatting1, settings_.renderOpt1, settings_.spacing);
}

const char* ReaderTextRenderer::layoutPresetLabel() const {
    switch (layoutPreset_) {
        case 0: return "原始";
        case 1: return "优化";
        case 2: return "紧凑";
    }
    return "优化";
}

void ReaderTextRenderer::setWebLayout(uint8_t fontSize, uint8_t lineSpacing, uint8_t paragraphSpacing,
                                      uint8_t indentFirstLine, uint8_t marginLeft, uint8_t marginRight,
                                      uint8_t marginTop, uint8_t marginBottom, bool justify) {
    webLineSpacing_ = constrain(lineSpacing, static_cast<uint8_t>(0), static_cast<uint8_t>(200));
    webParagraphSpacing_ = constrain(paragraphSpacing, static_cast<uint8_t>(0), static_cast<uint8_t>(100));
    webIndentFirstLine_ = constrain(indentFirstLine, static_cast<uint8_t>(0), static_cast<uint8_t>(4));
    // Keep body side margins symmetric. WebUI keeps the old marginRight field
    // for API compatibility, but runtime and NVS use one local side-margin value.
    uint8_t sideMargin = marginLeft;
    if (marginRight != marginLeft) sideMargin = static_cast<uint8_t>((static_cast<uint16_t>(marginLeft) + marginRight) / 2);
    webMarginLeft_ = constrain(sideMargin, static_cast<uint8_t>(0), static_cast<uint8_t>(120));
    webMarginTop_ = constrain(marginTop, static_cast<uint8_t>(0), static_cast<uint8_t>(160));
    webMarginBottom_ = constrain(marginBottom, static_cast<uint8_t>(0), static_cast<uint8_t>(160));
    webJustify_ = justify;
    setReaderFontSize(fontSize);
    saveLocalSettings();
}

ReaderRenderOptions ReaderTextRenderer::currentOptions() const {
    if (settings_.schema == 0) const_cast<ReaderTextRenderer*>(this)->applyLayoutPresetToSettings();

    ReaderRenderOptions opt;
    opt.fontSize = fontSize();

    static constexpr int16_t kTopMargins[4] = {68, 78, 92, 108};
    static constexpr int16_t kBottomMargins[4] = {22, 30, 40, 52};
    static constexpr int16_t kSideMargins[4] = {20, 28, 38, 48};
    static constexpr int16_t kLineGaps[4] = {3, 7, 12, 17};
    // Vink keeps four letter-spacing levels. Clamp the tightest level to zero
    // for the current renderer
    // so glyphs do not overlap on the PaperS3 panel.
    static constexpr int16_t kLetterGaps[4] = {0, 0, 1, 2};
    static constexpr int16_t kParagraphGaps[4] = {0, 0, 0, 0};  // no paragraph spacing, per user preference
    static constexpr int16_t kUnderlineOffsets[4] = {1, 2, 4, 6};

    // 本地设置作为预设基础，再以合并后的本机排版参数（含 Web 配置写回）形成最终运行值。
    opt.marginTop = kTopMargins[settings_.topBottomLevel()];
    opt.marginBottom = kBottomMargins[settings_.topBottomLevel()];
    opt.marginLeft = kSideMargins[settings_.leftRightLevel()];
    opt.marginRight = kSideMargins[settings_.leftRightLevel()];
    opt.lineGap = kLineGaps[settings_.lineSpacingLevel()];
    opt.letterGap = kLetterGaps[settings_.letterSpacingLevel()];
    opt.paragraphGap = kParagraphGaps[settings_.paragraphSpacingLevel()];
    opt.underlineOffset = kUnderlineOffsets[settings_.underlineOffsetLevel()];
    opt.indentFirstLine = settings_.indentEnabled();
    opt.compactBlankLines = settings_.blankLineOptEnabled();
    opt.dynamicLineHeight = settings_.dynamicLineHeightEnabled();
    opt.breakLineOpt = settings_.breakLineOptEnabled();
    opt.underline = settings_.underlineEnabled();
    opt.firstLineIndentPx = opt.indentFirstLine ? max<int16_t>(fontSize() * 2, 56) : 0;

    // Web 配置与本机排版配置合并后作为最终运行时源。
    opt.marginLeft = webMarginLeft_;
    opt.marginRight = webMarginLeft_;
    opt.marginTop = max<int16_t>(webMarginTop_, kReaderBodyTopMin);
    opt.marginBottom = max<int16_t>(webMarginBottom_, kReaderFooterReserve);
    opt.lineGap = max<int16_t>(0, (static_cast<int16_t>(fontSize()) * webLineSpacing_) / 100);
    opt.paragraphGap = (opt.lineGap * webParagraphSpacing_) / 100;
    opt.indentFirstLine = webIndentFirstLine_ > 0;
    opt.firstLineIndentPx = opt.indentFirstLine ? static_cast<int16_t>(fontSize() * webIndentFirstLine_) : 0;
    opt.justify = webJustify_;

    // Apply the quick in-reader layout preset after base values so the menu's
    // “排版优化” switch has an immediately visible effect.
    if (layoutPreset_ == 0) {
        // 原始：disable Vink formatting transforms for comparison.
        opt.indentFirstLine = false;
        opt.compactBlankLines = false;
        opt.dynamicLineHeight = false;
        opt.breakLineOpt = false;
        opt.justify = false;
        opt.firstLineIndentPx = 0;
        opt.letterGap = 0;
        opt.underlineOffset = 2;
    } else if (layoutPreset_ == 2) {
        // 紧凑：visibly tighter safe text box and line spacing.
        opt.marginLeft = max<int16_t>(12, opt.marginLeft - 8);
        opt.marginRight = max<int16_t>(12, opt.marginRight - 8);
        opt.marginTop = max<int16_t>(kReaderBodyTopMin, opt.marginTop - 6);
        opt.marginBottom = max<int16_t>(kReaderFooterReserve, opt.marginBottom - 6);
        opt.lineGap = max<int16_t>(2, opt.lineGap - 3);
        opt.paragraphGap = max<int16_t>(0, opt.paragraphGap - 2);
    }
    return opt;
}
uint32_t ReaderTextRenderer::decodeUtf8(const uint8_t* buf, size_t& pos, size_t len) {
    if (pos >= len) return 0;
    uint8_t c = buf[pos];
    if ((c & 0x80) == 0) { pos++; return c; }
    if ((c & 0xE0) == 0xC0 && pos + 1 < len) {
        uint32_t ch = ((c & 0x1F) << 6) | (buf[pos + 1] & 0x3F);
        pos += 2; return ch;
    }
    if ((c & 0xF0) == 0xE0 && pos + 2 < len) {
        uint32_t ch = ((c & 0x0F) << 12) | ((buf[pos + 1] & 0x3F) << 6) | (buf[pos + 2] & 0x3F);
        pos += 3; return ch;
    }
    if ((c & 0xF8) == 0xF0 && pos + 3 < len) {
        uint32_t ch = ((c & 0x07) << 18) | ((buf[pos + 1] & 0x3F) << 12) | ((buf[pos + 2] & 0x3F) << 6) | (buf[pos + 3] & 0x3F);
        pos += 4; return ch;
    }
    pos++;
    return c;
}

uint8_t ReaderTextRenderer::wenkaiByte(uint32_t offset) {
    if (offset >= g_wenkai_font32_size) return 0;
    return pgm_read_byte(&g_wenkai_font32[offset]);
}

uint16_t ReaderTextRenderer::wenkaiU16(uint32_t offset) {
    uint8_t b0 = wenkaiByte(offset);
    uint8_t b1 = wenkaiByte(offset + 1);
    return static_cast<uint16_t>(b0) | (static_cast<uint16_t>(b1) << 8);
}

uint32_t ReaderTextRenderer::wenkaiU32(uint32_t offset) {
    uint32_t b0 = wenkaiByte(offset);
    uint32_t b1 = wenkaiByte(offset + 1);
    uint32_t b2 = wenkaiByte(offset + 2);
    uint32_t b3 = wenkaiByte(offset + 3);
    return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
}

int8_t ReaderTextRenderer::wenkaiI8(uint32_t offset) {
    return static_cast<int8_t>(wenkaiByte(offset));
}

bool ReaderTextRenderer::beginWenkai32Font() {
    if (!g_wenkai_font32_available || g_wenkai_font32_size < kGrayHeaderSize) return false;
    if (wenkaiByte(0) != 'G' || wenkaiByte(1) != 'R' || wenkaiByte(2) != 'A' || wenkaiByte(3) != 'Y') return false;
    const uint16_t version = wenkaiU16(4);
    wenkai32FontHeight_ = wenkaiU16(6);
    wenkai32CharCount_ = wenkaiU32(8);
    const uint32_t bitmapBytes = wenkaiU32(12);
    wenkai32BitmapStart_ = kGrayHeaderSize + wenkai32CharCount_ * kGrayEntrySize;
    if (version != 1 || wenkai32FontHeight_ == 0 || wenkai32CharCount_ == 0) return false;
    if (wenkai32BitmapStart_ >= g_wenkai_font32_size) return false;
    if (wenkai32BitmapStart_ + bitmapBytes != g_wenkai_font32_size) return false;
    wenkai32Ready_ = true;
    return true;
}

bool ReaderTextRenderer::findWenkai32Glyph(uint32_t unicode, GrayGlyph& out) const {
    if (!wenkai32Ready_) return false;
    int32_t lo = 0;
    int32_t hi = static_cast<int32_t>(wenkai32CharCount_) - 1;
    while (lo <= hi) {
        int32_t mid = lo + (hi - lo) / 2;
        uint32_t off = kGrayHeaderSize + static_cast<uint32_t>(mid) * kGrayEntrySize;
        uint32_t cp = wenkaiU32(off);
        if (cp == unicode) {
            out.unicode = cp;
            out.bitmapOffset = wenkaiU32(off + 4);
            out.width = wenkaiByte(off + 8);
            out.height = wenkaiByte(off + 9);
            out.bearingX = wenkaiI8(off + 10);
            out.bearingY = wenkaiI8(off + 11);
            out.advance = wenkaiByte(off + 12);
            const uint32_t bitmapSize = static_cast<uint32_t>((out.width + 1) / 2) * out.height;
            return out.width > 0 && out.height > 0 && out.advance > 0 &&
                   out.bitmapOffset >= wenkai32BitmapStart_ &&
                   out.bitmapOffset + bitmapSize <= g_wenkai_font32_size;
        }
        if (cp < unicode) lo = mid + 1;
        else hi = mid - 1;
    }
    return false;
}

uint8_t ReaderTextRenderer::charAdvance(uint32_t unicode) const {
    if (sdCardFontActive_ && ttfFont_.isLoaded() && ttfFont_.hasGlyph(unicode)) {
        int16_t a = ttfFont_.charAdvance(unicode);
        if (a > 0) return static_cast<uint8_t>(a > 255 ? 255 : a);
        return unicode < 128 ? static_cast<uint8_t>(ttfFont_.currentSize() / 2) : ttfFont_.currentSize();
    }
    if (wenkai32Ready_) {
        GrayGlyph glyph;
        if (findWenkai32Glyph(unicode, glyph) && glyph.advance > 0) {
            return glyph.advance;
        }
        return unicode < 128 ? 8 : fontSize();
    }
    if (!font_.isLoaded()) return unicode < 128 ? 8 : 24;
    uint8_t adv = const_cast<FontManager&>(font_).getCharAdvance(unicode);
    return adv > 0 ? adv : (unicode < 128 ? 8 : fontSize());
}

int16_t ReaderTextRenderer::textWidth(const char* text) const {
    if (!text) return 0;
    int16_t w = 0;
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(text);
    size_t pos = 0;
    const size_t len = strlen(text);
    while (pos < len) {
        uint32_t ch = decodeUtf8(bytes, pos, len);
        if (ch == '\n') break;
        w += charAdvance(ch);
    }
    return w;
}

uint16_t ReaderTextRenderer::pixelColorForNibble(uint8_t nibble, uint16_t color) const {
    if (color == TFT_WHITE) return TFT_WHITE;
    if (color != TFT_BLACK) return color;
    if (!antiAliasEnabled()) return (nibble >= 8) ? TFT_BLACK : TFT_WHITE;
    // PaperS3's M5GFX EPD panel accepts grayscale and stores 4bpp/16-level ink.
    // Keep true grayscale anti-aliasing for smooth text, but boost low coverage
    // edges darker so the hardware waveform does not look like weak/faint ink.
    static const uint8_t kInkBoost[16] __attribute__((aligned(1))) = {
        0, 3, 5, 6, 7, 8, 9, 10, 10, 11, 12, 13, 13, 14, 15, 15
    };
    static const uint16_t k4BitToRgb565[16] __attribute__((aligned(2))) = {
        0xFFFF, 0xDEDB, 0xC618, 0xAD75, 0x9CD3, 0x8C51, 0x7BCF, 0x6B4D,
        0x5ACB, 0x4A69, 0x39E7, 0x3186, 0x2124, 0x10A2, 0x0841, 0x0000
    };
    return k4BitToRgb565[kInkBoost[nibble & 0x0F]];
}

void ReaderTextRenderer::drawGlyph(uint32_t unicode, int16_t x, int16_t y, uint16_t color) {
    if (!canvas_) return;
    // SD card TTF font (highest priority when it actually contains the glyph).
    if (sdCardFontActive_ && ttfFont_.isLoaded() && ttfFont_.hasGlyph(unicode)) {
        if (ttfFont_.drawGlyph(unicode, x, y, color, canvas_)) return;
        // If an SD font is selected but cannot render this glyph, keep falling
        // through to the built-in Wenkai/tofu path. Otherwise a Latin-only or
        // broken SD font can make the whole body page blank while header/footer
        // still look normal.
    }
    // Wenkai32 PROGMEM (4bpp GRAY format, primary)
    if (wenkai32Ready_) {
        GrayGlyph glyph;
        if (findWenkai32Glyph(unicode, glyph)) {
            const int16_t drawX = x + glyph.bearingX;
            // y is line-top; align every glyph on a shared baseline. Drawing all
            // glyph bitmaps at y ignored bearingY, which made body text jump up
            // and down and clipped low punctuation/Latin descenders.
            const int16_t drawY = y + static_cast<int16_t>(fontSize()) - glyph.bearingY;
            const uint32_t rowBytes = (glyph.width + 1) / 2;
            for (uint8_t row = 0; row < glyph.height; row++) {
                const int16_t py = drawY + row;
                if (py < 0 || py >= kPaperS3Height) continue;
                for (uint8_t col = 0; col < glyph.width; col++) {
                    const int16_t px = drawX + col;
                    if (px < 0 || px >= kPaperS3Width) continue;
                    const uint8_t packed = wenkaiByte(glyph.bitmapOffset + row * rowBytes + col / 2);
                    const uint8_t nibble = (col % 2 == 0) ? ((packed >> 4) & 0x0F) : (packed & 0x0F);
                    if (nibble > 0) canvas_->drawPixel(px, py, pixelColorForNibble(nibble, color));
                }
            }
            return;
        }
        // Critical fallback: never let a missing body glyph silently disappear.
        // v0.4.15 shipped with a sparse Wenkai font, so common CJK characters
        // could advance layout without drawing any pixels, making pages look
        // completely blank. v0.4.16 expands GB2312 coverage, but rare/out-of-set
        // glyphs must still render a visible tofu box rather than invisible text.
        if (unicode > 0x20 && unicode != 0x3000) {
            const int16_t box = static_cast<int16_t>(fontSize()) - 8;
            const int16_t bx = x + 4;
            const int16_t by = y + 4;
            canvas_->drawRect(bx, by, box, box, color == TFT_WHITE ? TFT_BLACK : color);
            canvas_->drawLine(bx, by, bx + box - 1, by + box - 1, color == TFT_WHITE ? TFT_BLACK : color);
        }
        return;
    }
    // LovyanGFX fallback (16/20px SPIFFS GRAY fonts)
    if (!font_.isLoaded()) return;
    if (font_.getFontType() == FontType::GRAY_4BPP) {
        uint8_t w = 0, h = 0, adv = 0;
        int8_t bx = 0, by = 0;
        const uint8_t* bmp = font_.getCharBitmapGray(unicode, w, h, bx, by, adv);
        if (!bmp || w == 0 || h == 0) return;
        const int16_t drawX = x + bx;
        const int16_t drawY = y + static_cast<int16_t>(fontSize()) - by;
        for (int row = 0; row < h; row++) {
            const int16_t py = drawY + row;
            if (py < 0 || py >= kPaperS3Height) continue;
            for (int col = 0; col < w; col++) {
                const int16_t px = drawX + col;
                if (px < 0 || px >= kPaperS3Width) continue;
                const uint8_t packed = bmp[row * ((w + 1) / 2) + col / 2];
                const uint8_t nibble = (col % 2 == 0) ? ((packed >> 4) & 0x0F) : (packed & 0x0F);
                if (nibble > 0) canvas_->drawPixel(px, py, pixelColorForNibble(nibble, color));
            }
        }
    }
}

void ReaderTextRenderer::drawText(int16_t x, int16_t y, const char* text, uint16_t color, int16_t letterGap) {
    if (!canvas_ || !text) return;
    if (!wenkai32Ready_ && !font_.isLoaded() && !(sdCardFontActive_ && ttfFont_.isLoaded())) return;
    int16_t cx = x;
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(text);
    size_t pos = 0;
    const size_t len = strlen(text);
    while (pos < len && cx < kPaperS3Width) {
        uint32_t ch = decodeUtf8(bytes, pos, len);
        if (ch == '\n') break;
        drawGlyph(ch, cx, y, color);
        cx += charAdvance(ch) + letterGap;
    }
}

bool ReaderTextRenderer::isParagraphStart(const char* text, size_t pos, bool chunkStartsAtParagraph) const {
    if (!text) return false;
    if (pos == 0) return chunkStartsAtParagraph;
    size_t i = pos;
    while (i > 0) {
        char c = text[i - 1];
        if (c == '\n' || c == '\r') return true;
        if (c != ' ' && c != '\t') return false;
        --i;
    }
    return chunkStartsAtParagraph;
}

bool ReaderTextRenderer::isForbiddenLineStart(uint32_t unicode) const {
    switch (unicode) {
        case 0x0021: // !
        case 0x0025: // %
        case 0x0029: // )
        case 0x002C: // ,
        case 0x002E: // .
        case 0x003A: // :
        case 0x003B: // ;
        case 0x003F: // ?
        case 0x005D: // ]
        case 0x007D: // }
        case 0x00B7: // ·
        case 0x2019: // ’
        case 0x201D: // ”
        case 0x2026: // …
        case 0x3001: // 、
        case 0x3002: // 。
        case 0x3009: // 〉
        case 0x300B: // 》
        case 0x300D: // 」
        case 0x300F: // 』
        case 0x3011: // 】
        case 0x3015: // 〕
        case 0xFF01: // ！
        case 0xFF09: // ）
        case 0xFF0C: // ，
        case 0xFF0E: // ．
        case 0xFF1A: // ：
        case 0xFF1B: // ；
        case 0xFF1F: // ？
        case 0xFF3D: // ］
        case 0xFF5D: // ｝
            return true;
        default:
            return false;
    }
}

size_t ReaderTextRenderer::skipLeadingSourceIndent(const char* text, size_t pos, size_t len) const {
    while (pos < len) {
        const char c = text[pos];
        if (c == ' ' || c == '\t') {
            ++pos;
            continue;
        }
        // UTF-8 ideographic space U+3000. Vink-native indent should replace
        // source indentation, not stack on top of it.
        if (pos + 2 < len && static_cast<uint8_t>(text[pos]) == 0xE3 &&
            static_cast<uint8_t>(text[pos + 1]) == 0x80 &&
            static_cast<uint8_t>(text[pos + 2]) == 0x80) {
            pos += 3;
            continue;
        }
        break;
    }
    return pos;
}

size_t ReaderTextRenderer::nextLineEnd(const char* text, size_t len, size_t start, int16_t maxWidth, int16_t initialWidth, const ReaderRenderOptions& options, bool& hardBreak) const {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(text);
    size_t pos = start;
    size_t lastGood = start;
    int16_t width = initialWidth;
    hardBreak = false;

    while (pos < len) {
        const size_t before = pos;
        const uint32_t ch = decodeUtf8(bytes, pos, len);
        if (ch == '\n' || ch == '\r') {
            hardBreak = true;
            return before;
        }

        const int16_t adv = static_cast<int16_t>(charAdvance(ch)) + options.letterGap;
        if (width + adv > maxWidth) {
            // Keep measurePageBytes() and renderTextPage() byte-perfect. If the
            // measured page consumes more bytes than the renderer draws, the
            // last few characters of a page disappear when moving to next page.
            if (options.breakLineOpt && isForbiddenLineStart(ch) && lastGood > start) {
                return pos;
            }
            return lastGood > start ? lastGood : before;
        }
        width += adv;
        lastGood = pos;
    }
    return len;
}

size_t ReaderTextRenderer::findWrapBreak(const char* text, size_t start, int16_t maxWidth, int16_t letterGap) const {
    if (!text) return start;
    ReaderRenderOptions opt;
    opt.letterGap = letterGap;
    opt.breakLineOpt = false;
    bool hardBreak = false;
    return nextLineEnd(text, strlen(text), start, maxWidth, 0, opt, hardBreak);
}

size_t ReaderTextRenderer::measurePageBytes(const char* text, size_t len, const ReaderRenderOptions& options) const {
    if (!text || len == 0) return 0;
    size_t pos = 0;
    int16_t y = options.marginTop;
    const int16_t maxWidth = kPaperS3Width - options.marginLeft - options.marginRight;
    const int16_t baseLineHeight = fontSize() + options.lineGap;
    const int16_t lineHeight = options.dynamicLineHeight ? max<int16_t>(fontSize() + 3, baseLineHeight) : baseLineHeight;
    const int16_t bottom = kPaperS3Height - options.marginBottom;
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(text);

    while (pos < len && y + lineHeight <= bottom) {
        bool skippedBlank = false;
        while (pos < len && (text[pos] == '\n' || text[pos] == '\r')) { pos++; skippedBlank = true; }
        if (skippedBlank && !options.compactBlankLines) y += lineHeight / 2;
        if (pos >= len || y + lineHeight > bottom) break;

        const bool paragraphStart = options.indentFirstLine && isParagraphStart(text, pos, options.startsAtParagraph);
        if (paragraphStart) pos = skipLeadingSourceIndent(text, pos, len);
        const size_t lineStart = pos;
        bool hardBreak = false;
        pos = nextLineEnd(text, len, lineStart, maxWidth, paragraphStart ? options.firstLineIndentPx : 0, options, hardBreak);
        if (pos <= lineStart) {
            size_t force = lineStart;
            decodeUtf8(bytes, force, len);
            pos = force > lineStart ? force : lineStart + 1;
        }
        y += lineHeight;
        if (hardBreak && options.paragraphGap > 0) y += options.paragraphGap;
    }
    return pos;
}

void ReaderTextRenderer::drawShellTabs(int activeTab, const ReaderRenderOptions& options) {
    // Tab layout mirrors VinkUiRenderer::drawTabs for visual consistency.
    // Uses Reading font (not CJK UI font) since this is the reader layer.
    static constexpr const char* kLabels[] = {"阅读", "书架", "同步", "设置"};
    static constexpr int16_t kCenter  = kPaperS3Width / 2;
    static constexpr int16_t kTabW    = 128;
    static constexpr int16_t kTabH    = 62;
    static constexpr int16_t kTabGap  = 12;
    static constexpr int16_t kTabY    = 80;
    static constexpr int16_t kT4      = 4;
    static constexpr int16_t kLeft    = kCenter - (kTabW * kT4 + kTabGap * (kT4 - 1)) / 2;
    const uint16_t fg = options.dark ? TFT_WHITE : TFT_BLACK;
    const uint16_t bg = options.dark ? TFT_BLACK : TFT_WHITE;
    for (int i = 0; i < 4; ++i) {
        const int16_t x = kLeft + i * (kTabW + kTabGap);
        const bool selected = i == activeTab;
        if (selected) {
            canvas_->fillRoundRect(x, kTabY + 2, kTabW, kTabH - 4, 16, bg == TFT_BLACK ? 0x2124 : 0xF7BE);
        }
        canvas_->drawRoundRect(x, kTabY, kTabW, kTabH, 16, fg);
        if (selected) {
            canvas_->fillRoundRect(x + (kTabW - 48) / 2, kTabY + kTabH - 8, 48, 4, 2, fg);
        }
        const char* label = kLabels[i];
        const int16_t tx = x + (kTabW - textWidth(label)) / 2;
        const int16_t ty = kTabY + (kTabH - fontSize()) / 2;
        drawText(tx, ty, label, fg);
    }
}

uint16_t ReaderTextRenderer::utf8CharCount(const char* text) const {
    if (!text) return 0;
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(text);
    const size_t len = strlen(text);
    size_t pos = 0;
    uint16_t count = 0;
    while (pos < len) {
        decodeUtf8(bytes, pos, len);
        count++;
    }
    return count;
}

void ReaderTextRenderer::drawJustifiedText(int16_t x, int16_t y, const char* text, int16_t targetWidth, uint16_t color, int16_t letterGap) {
    if (!text || !text[0]) return;
    const uint16_t count = utf8CharCount(text);
    if (count <= 1) {
        drawText(x, y, text, color, letterGap);
        return;
    }
    const int16_t baseWidth = textWidth(text) + static_cast<int16_t>((count - 1) * letterGap);
    int16_t extra = targetWidth - baseWidth;
    if (extra <= 0) {
        drawText(x, y, text, color, letterGap);
        return;
    }
    extra = min<int16_t>(extra / static_cast<int16_t>(count - 1), 8);
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(text);
    const size_t len = strlen(text);
    size_t pos = 0;
    int16_t cx = x;
    while (pos < len) {
        uint32_t ch = decodeUtf8(bytes, pos, len);
        drawGlyph(ch, cx, y, color);
        cx += static_cast<int16_t>(charAdvance(ch)) + letterGap + extra;
    }
}

void ReaderTextRenderer::formatReaderTime(char* out, size_t outSize) const {
    if (!out || outSize == 0) return;
    m5::rtc_time_t rtc;
    if (M5.Rtc.isEnabled() && M5.Rtc.getTime(&rtc) &&
        rtc.hours >= 0 && rtc.hours < 24 && rtc.minutes >= 0 && rtc.minutes < 60) {
        snprintf(out, outSize, "%02d:%02d", rtc.hours, rtc.minutes);
        return;
    }
    snprintf(out, outSize, "--:--");
}

void ReaderTextRenderer::drawReadingChrome(const char* title, uint16_t progressPermille, const ReaderRenderOptions& options, uint16_t color) {
    const uint16_t mid = options.dark ? 0xC618 : 0x8410;
    const int16_t left = options.marginLeft;
    const int16_t right = kPaperS3Width - options.marginRight;

    if (g_cjkText.ready()) {
        char timeText[12];
        char battText[12];
        char pctText[12];

        // ── Header: time left, battery right (matches tab page status bar) ──
        formatReaderTime(timeText, sizeof(timeText));
        g_cjkText.drawText(left, 18, timeText, mid);

        {
            int level = M5.Power.getBatteryLevel();
            if (level > 0 && level <= 100) {
                snprintf(battText, sizeof(battText), "%d%%", level);
            } else {
                float v = M5.Power.getBatteryVoltage();
                if (v > 0.1f) snprintf(battText, sizeof(battText), "%.2fV", v);
                else snprintf(battText, sizeof(battText), "--%%");
            }
        }
        g_cjkText.drawRight(right, 18, battText, mid);

        // ── Footer: chapter name left, progress % right ──
        const uint16_t permille = min<uint16_t>(progressPermille, 1000);
        snprintf(pctText, sizeof(pctText), "%u.%u%%", permille / 10, permille % 10);
        g_cjkText.drawRight(right, kPaperS3Height - 38, pctText, mid);

        {
            char nameText[96];
            const int16_t nameMaxW = right - left - g_cjkText.textWidth(pctText) - 16;
            g_cjkText.fitTextToWidth(title ? title : "", nameText, sizeof(nameText), nameMaxW);
            g_cjkText.drawText(left, kPaperS3Height - 38, nameText, mid);
        }
    } else {
        char pctText[12];
        const uint16_t permille = min<uint16_t>(progressPermille, 1000);
        snprintf(pctText, sizeof(pctText), "%u.%u%%", permille / 10, permille % 10);
        drawText(left, 18, "--:--", mid);
        drawText(right - textWidth(pctText), kPaperS3Height - 38, pctText, mid);
        if (title && title[0]) drawText(left, kPaperS3Height - 38, title, mid);
    }

    canvas_->drawFastHLine(left, kReaderHeaderDividerY, right - left, mid);
}

void ReaderTextRenderer::renderTextPage(const char* title, const char* body, uint16_t page, uint16_t totalPages, const ReaderRenderOptions& options, uint16_t progressPermille) {
    (void)page;
    (void)totalPages;
    if (!canvas_) return;
    if (!ready()) loadDefaultFont();
    canvas_->fillSprite(options.dark ? TFT_BLACK : TFT_WHITE);
    const uint16_t fg = options.dark ? TFT_WHITE : TFT_BLACK;
    const uint16_t mid = options.dark ? 0xC618 : 0x8410;

    drawReadingChrome(title, progressPermille, options, fg);

    const char* text = body ? body : "";
    size_t pos = 0;
    const size_t len = strlen(text);
    int16_t y = options.marginTop;
    const int16_t maxWidth = kPaperS3Width - options.marginLeft - options.marginRight;
    const int16_t baseLineHeight = fontSize() + options.lineGap;
    const int16_t lineHeight = options.dynamicLineHeight ? max<int16_t>(fontSize() + 3, baseLineHeight) : baseLineHeight;
    const int16_t bottom = kPaperS3Height - options.marginBottom;
    uint8_t drawnLines = 0;
    while (pos < len && y + lineHeight <= bottom) {
        bool skippedBlank = false;
        while (pos < len && (text[pos] == '\n' || text[pos] == '\r')) { pos++; skippedBlank = true; }
        if (skippedBlank && !options.compactBlankLines) y += lineHeight / 2;
        if (pos >= len || y + lineHeight > bottom) break;
        const bool paragraphStart = options.indentFirstLine && isParagraphStart(text, pos, options.startsAtParagraph);
        if (paragraphStart) pos = skipLeadingSourceIndent(text, pos, len);
        const int16_t indent = paragraphStart ? options.firstLineIndentPx : 0;
        bool hardBreak = false;
        size_t end = nextLineEnd(text, len, pos, maxWidth, indent, options, hardBreak);
        if (end <= pos) break;
        char line[256];
        size_t n = end - pos;
        if (n >= sizeof(line)) n = sizeof(line) - 1;
        memcpy(line, text + pos, n);
        line[n] = '\0';
        if (options.justify && !hardBreak) drawJustifiedText(options.marginLeft + indent, y, line, maxWidth - indent, fg, options.letterGap);
        else drawText(options.marginLeft + indent, y, line, fg, options.letterGap);
        if (options.underline) {
            const int16_t uy = y + static_cast<int16_t>(fontSize()) + options.underlineOffset;
            for (int16_t x = options.marginLeft; x < kPaperS3Width - options.marginRight; x += 6) {
                canvas_->drawFastHLine(x, uy, 3, mid);
            }
        }
        drawnLines++;
        pos = end;
        y += lineHeight;
        if (hardBreak && options.paragraphGap > 0) y += options.paragraphGap;
    }

    if (len > 0 && drawnLines == 0) {
        g_systemLog.appendf("reader blank body len=%u top=%d bottom=%d line=%d ready=%d font=%u",
                            static_cast<unsigned>(len), static_cast<int>(options.marginTop),
                            static_cast<int>(bottom), static_cast<int>(lineHeight),
                            ready() ? 1 : 0, static_cast<unsigned>(fontSize()));
    }

}

void ReaderTextRenderer::renderListPage(const char* title, const char* summary, const char* const* rows, int rowCount, int16_t rowY, int16_t rowH, uint16_t page, uint16_t totalPages, int activeTab, const ReaderRenderOptions& options) {
    if (!canvas_) return;
    if (!ready()) loadDefaultFont();
    canvas_->fillSprite(options.dark ? TFT_BLACK : TFT_WHITE);
    const uint16_t fg = options.dark ? TFT_WHITE : TFT_BLACK;
    const uint16_t mid = options.dark ? 0xC618 : 0x8410;

    drawText(options.marginLeft, 22, title ? title : "列表", fg);
    drawText(kPaperS3Width - options.marginRight - textWidth(kVinkPaperS3FirmwareVersion), 22, kVinkPaperS3FirmwareVersion, mid);
    canvas_->drawFastHLine(options.marginLeft, 61, kPaperS3Width - options.marginLeft - options.marginRight, fg);
    drawShellTabs(activeTab, options);
    // Summary/help text is secondary; keep it smaller than primary row labels.
    if (summary && summary[0]) drawText(options.marginLeft, 162, summary, mid);

    const int16_t maxWidth = kPaperS3Width - options.marginLeft - options.marginRight;
    for (int i = 0; rows && i < rowCount; ++i) {
        const int16_t y = rowY + i * rowH;
        if (y + rowH > kPaperS3Height - options.marginBottom) break;
        canvas_->drawFastHLine(options.marginLeft, y + rowH - 4, maxWidth, mid);
        const char* row = rows[i] ? rows[i] : "";
        char line[192];
        size_t end = findWrapBreak(row, 0, maxWidth);
        if (end == 0) end = min(strlen(row), sizeof(line) - 1);
        size_t n = min(end, sizeof(line) - 1);
        memcpy(line, row, n);
        line[n] = '\0';
        const int16_t ty = y + max<int16_t>(0, (rowH - static_cast<int16_t>(fontSize())) / 2);
        drawText(options.marginLeft, ty, line, fg);
    }

    char footer[48];
    snprintf(footer, sizeof(footer), "%u / %u", static_cast<unsigned>(page), static_cast<unsigned>(totalPages));
    drawText(kPaperS3Width - options.marginRight - textWidth(footer), kPaperS3Height - 34, footer, mid);
}

void ReaderTextRenderer::renderActionPage(const char* title, const char* const* infoLines, int infoCount, const char* const* actions, int actionCount, int activeTab, const ReaderRenderOptions& options) {
    if (!canvas_) return;
    if (!ready()) loadDefaultFont();
    canvas_->fillSprite(options.dark ? TFT_BLACK : TFT_WHITE);
    const uint16_t fg = options.dark ? TFT_WHITE : TFT_BLACK;
    const uint16_t bg = options.dark ? TFT_BLACK : TFT_WHITE;
    const uint16_t mid = options.dark ? 0xC618 : 0x8410;

    drawText(options.marginLeft, 22, title ? title : "操作", fg);
    drawText(kPaperS3Width - options.marginRight - textWidth(kVinkPaperS3FirmwareVersion), 22, kVinkPaperS3FirmwareVersion, mid);
    canvas_->drawFastHLine(options.marginLeft, 61, kPaperS3Width - options.marginLeft - options.marginRight, fg);
    drawShellTabs(activeTab, options);

    const int16_t maxWidth = kPaperS3Width - options.marginLeft - options.marginRight;
    const int16_t lineHeight = fontSize() + options.lineGap;
    int16_t y = 160;
    for (int i = 0; infoLines && i < infoCount && y < 510; ++i) {
        const char* lineText = infoLines[i] ? infoLines[i] : "";
        size_t start = 0;
        int wraps = 0;
        while (lineText[start] && wraps < 2 && y < 510) {
            size_t end = findWrapBreak(lineText, start, maxWidth);
            if (end <= start) break;
            char line[256];
            size_t n = end - start;
            if (n >= sizeof(line)) n = sizeof(line) - 1;
            memcpy(line, lineText + start, n);
            line[n] = '\0';
            drawText(options.marginLeft, y, line, mid);
            y += lineHeight;
            start = end;
            wraps++;
        }
    }

    static constexpr int16_t kButtonX = 70;
    static constexpr int16_t kButtonW = 400;
    static constexpr int16_t kButtonH = 64;
    static constexpr int16_t kButtonY[] = {560, 660, 760};
    const int drawCount = min(actionCount, static_cast<int>(sizeof(kButtonY) / sizeof(kButtonY[0])));
    for (int i = 0; actions && i < drawCount; ++i) {
        const int16_t by = kButtonY[i];
        const bool primary = i == 0;
        canvas_->fillRoundRect(kButtonX, by, kButtonW, kButtonH, 18, primary ? fg : bg);
        canvas_->drawRoundRect(kButtonX, by, kButtonW, kButtonH, 18, fg);
        const char* label = actions[i] ? actions[i] : "";
        const int16_t tx = kButtonX + (kButtonW - textWidth(label)) / 2;
        // Button labels are primary actions: keep 24/32px text and center it
        // in the button's actual geometry, separate from smaller notes below.
        const int16_t lineH = static_cast<int16_t>(fontSize()) + 6;
        const int16_t ty = by + (kButtonH - lineH) / 2;
        drawText(tx, ty, label, primary ? bg : fg);
    }
}

// ── SD card TTF font source ──────────────────────────────────────────────────

void ReaderTextRenderer::cycleFontSource() {
    if (sdCardFontActive_ && ttfFont_.isLoaded()) {
        // Switch back to built-in
        ttfFont_.unload();
        sdCardFontActive_ = false;
        applyReaderFontSize(fontSizeSetting_, false);
        Serial.println("[vink3][reader] font source -> 内置文楷");
        saveLocalSettings();
        return;
    }

    // Scan SD for TTF fonts, cycle through them
    ttfFontCount_ = TtfFont::scanSdFonts(ttfFontPaths_, 4);
    if (ttfFontCount_ == 0) {
        Serial.println("[vink3][reader] no TTF/OTF fonts found on SD");
        return;
    }

    // Increment font index (cycle through available fonts)
    ttfFontIndex_ = (ttfFontIndex_ + 1) % ttfFontCount_;
    if (ttfFontIndex_ < 0 || ttfFontIndex_ >= ttfFontCount_) ttfFontIndex_ = 0;

    if (ttfFont_.loadFromSd(ttfFontPaths_[ttfFontIndex_])) {
        ttfFont_.setSize(sdFontSize_);
        sdCardFontActive_ = true;
        // Unload built-in fonts to free resources
        font_.unload();
        wenkai32Ready_ = false;
        Serial.printf("[vink3][reader] font source -> SD:%s\n",
                      strrchr(ttfFontPaths_[ttfFontIndex_], '/') ?
                      strrchr(ttfFontPaths_[ttfFontIndex_], '/') + 1 :
                      ttfFontPaths_[ttfFontIndex_]);
    }
    saveLocalSettings();
}

const char* ReaderTextRenderer::fontSourceLabel() const {
    if (sdCardFontActive_ && ttfFont_.isLoaded()) {
        static char label[48];
        const char* fname = strrchr(ttfFont_.currentPath(), '/');
        fname = fname ? fname + 1 : ttfFont_.currentPath();
        snprintf(label, sizeof(label), "SD:%.30s", fname);
        return label;
    }
    return "内置文楷";
}

void ReaderTextRenderer::stepSdFontSize(int delta) {
    if (delta == 0) return;
    int newSize = static_cast<int>(sdFontSize_) + delta;
    if (newSize < 16) newSize = 16;
    if (newSize > 64) newSize = 64;
    setSdFontSize(static_cast<uint8_t>(newSize));
}

void ReaderTextRenderer::setSdFontSize(uint8_t size) {
    if (size < 16) size = 16;
    if (size > 64) size = 64;
    if (size == sdFontSize_ && (!sdCardFontActive_ || ttfFont_.isLoaded())) return;
    sdFontSize_ = size;
    if (sdCardFontActive_ && ttfFont_.isLoaded()) {
        ttfFont_.setSize(size);
    }
    saveLocalSettings();
    Serial.printf("[vink3][reader] SD font size -> %upx\n", sdFontSize_);
}

const char* ReaderTextRenderer::sdFontSizeLabel() const {
    static char buf[8];
    snprintf(buf, sizeof(buf), "%upx", sdFontSize_);
    return buf;
}

void ReaderTextRenderer::renderPlaceholderPage() {
    static const char* sample =
        "这是 Vink v0.3 的正文渲染层。界面文字使用 ReadPaper UI 子集字体，正文阅读使用完整 ReadPaper Book PROGMEM 字体。\n"
        "下一步会把本地 TXT、分页、书签和 Legado 进度映射接到这里；中文覆盖不再依赖按书抽子集。";
    renderTextPage("示例正文", sample, 1, 1);
}

} // namespace vink3
