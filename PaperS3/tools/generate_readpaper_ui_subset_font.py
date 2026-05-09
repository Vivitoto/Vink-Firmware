#!/usr/bin/env python3
"""Generate a small ReadPaper V3 font subset for Vink UI strings.

Input is ReadPaper's generated src/text/lite.cpp from the remote reference tree.
Output is a compact PROGMEM C++ file with the same V3 header/glyph-entry layout,
but containing only glyphs needed by the v0.3 shell UI.
"""
from __future__ import annotations

import argparse
import ast
import re
import struct
from pathlib import Path

HEADER_SIZE = 134
ENTRY_SIZE = 20

DEFAULT_EXTRA = (
    "，。；：！？、·《》〈〉（）()[]【】+-/%:：. "
    "0123456789"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
    "阅读书架同步设置当前书籍继续上一页下一页中心菜单打开目录章节跳页标注书签截图正文自动翻页刷新字号竖排繁简"
    "统计今日分钟关闭来源最近远程绑定传输与局域网立即配置热点存储确认接管导出策略先拉远端再比较位置避免覆盖新进度"
    "显示默认深色旋转质量网络地址系统电池睡眠版本调试点击标签卡片底层正常等待重试返回取消推送本地冲突用户选择"
)

STRING_RE = re.compile(r'"(?:[^"\\]|\\.)*"')
BYTE_RE = re.compile(r'0x([0-9A-Fa-f]{2})')


def extract_cpp_strings(path: Path) -> str:
    text = path.read_text(encoding="utf-8")
    out: list[str] = []
    for match in STRING_RE.finditer(text):
        literal = match.group(0)
        try:
            out.append(ast.literal_eval(literal))
        except Exception:
            pass
    return "\n".join(out)


def parse_lite_cpp(path: Path) -> bytes:
    text = path.read_text(encoding="utf-8", errors="ignore")
    start = text.find("const uint8_t g_progmem_font_data")
    if start < 0:
        raise SystemExit(f"cannot find g_progmem_font_data in {path}")
    brace = text.find("{", start)
    end = text.find("};", brace)
    if brace < 0 or end < 0:
        raise SystemExit("cannot locate font data braces")
    body = text[brace + 1:end]
    body = re.sub(r"//.*", "", body)
    data = bytearray()
    # ReadPaper's generated file includes offset comments with decimal numbers;
    # only consume explicit hexadecimal byte literals from the C array.
    for m in BYTE_RE.finditer(body):
        data.append(int(m.group(1), 16))
    return bytes(data)


def read_entry(data: bytes, idx: int) -> dict:
    off = HEADER_SIZE + idx * ENTRY_SIZE
    raw = data[off:off + ENTRY_SIZE]
    if len(raw) != ENTRY_SIZE:
        raise ValueError("short glyph entry")
    unicode_, width, bitmap_w, bitmap_h, x_off, y_off, bitmap_off, bitmap_size, cached = struct.unpack_from("<HHBBbbIII", raw)
    return {
        "unicode": unicode_,
        "width": width,
        "bitmap_w": bitmap_w,
        "bitmap_h": bitmap_h,
        "x_off": x_off,
        "y_off": y_off,
        "bitmap_off": bitmap_off,
        "bitmap_size": bitmap_size,
        "cached": cached,
    }


def pack_entry(e: dict, bitmap_off: int) -> bytes:
    return struct.pack(
        "<HHBBbbIII",
        e["unicode"], e["width"], e["bitmap_w"], e["bitmap_h"],
        e["x_off"], e["y_off"], bitmap_off, e["bitmap_size"], 0,
    )


def generate(source: bytes, chars: set[str]) -> tuple[bytes, list[str]]:
    char_count = struct.unpack_from("<I", source, 0)[0]
    font_height = source[4]
    version = source[5]
    if version != 3:
        raise SystemExit(f"expected ReadPaper V3 font, got version={version}")
    entries = [read_entry(source, i) for i in range(char_count)]
    by_cp = {e["unicode"]: e for e in entries}
    wanted = sorted({ord(ch) for ch in chars if ord(ch) <= 0xFFFF})
    selected = []
    missing = []
    for cp in wanted:
        e = by_cp.get(cp)
        if e:
            selected.append(e)
        elif cp > 0x20:
            missing.append(chr(cp))
    selected.sort(key=lambda e: e["unicode"])

    header = bytearray(source[:HEADER_SIZE])
    struct.pack_into("<I", header, 0, len(selected))
    # keep font_height/version/family/style from upstream
    bitmap_cursor = HEADER_SIZE + len(selected) * ENTRY_SIZE
    entry_bytes = bytearray()
    bitmap_bytes = bytearray()
    for e in selected:
        raw = source[e["bitmap_off"]:e["bitmap_off"] + e["bitmap_size"]]
        if len(raw) != e["bitmap_size"]:
            raise SystemExit(f"short bitmap for U+{e['unicode']:04X}")
        entry_bytes += pack_entry(e, bitmap_cursor)
        bitmap_bytes += raw
        bitmap_cursor += len(raw)
    return bytes(header + entry_bytes + bitmap_bytes), missing


def write_cpp(out_cpp: Path, data: bytes) -> None:
    out_cpp.parent.mkdir(parents=True, exist_ok=True)
    with out_cpp.open("w", encoding="utf-8") as f:
        f.write("#include <Arduino.h>\n")
        f.write("#include \"ReadPaperUiFont.h\"\n\n")
        f.write("namespace vink3 {\n")
        f.write("const bool g_readpaper_ui_font_available = true;\n")
        f.write(f"const uint32_t g_readpaper_ui_font_size = {len(data)};\n")
        f.write("const uint8_t g_readpaper_ui_font_data[] PROGMEM = {\n")
        for i in range(0, len(data), 16):
            chunk = data[i:i+16]
            f.write("    " + ", ".join(f"0x{b:02X}" for b in chunk) + ",\n")
        f.write("};\n")
        f.write("} // namespace vink3\n")


def write_header(out_h: Path) -> None:
    out_h.parent.mkdir(parents=True, exist_ok=True)
    out_h.write_text(
        "#pragma once\n"
        "#include <Arduino.h>\n\n"
        "namespace vink3 {\n"
        "extern const bool g_readpaper_ui_font_available;\n"
        "extern const uint32_t g_readpaper_ui_font_size;\n"
        "extern const uint8_t g_readpaper_ui_font_data[] PROGMEM;\n"
        "} // namespace vink3\n",
        encoding="utf-8",
    )


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--lite", type=Path, required=True)
    p.add_argument("--ui", type=Path, action="append", default=[])
    p.add_argument("--out-cpp", type=Path, required=True)
    p.add_argument("--out-h", type=Path, required=True)
    args = p.parse_args()

    chars = set(DEFAULT_EXTRA)
    for ui in args.ui:
        chars.update(extract_cpp_strings(ui))
    source = parse_lite_cpp(args.lite)
    subset, missing = generate(source, chars)
    write_header(args.out_h)
    write_cpp(args.out_cpp, subset)
    print(f"subset bytes: {len(subset)}")
    print(f"missing glyphs: {''.join(missing) if missing else '(none)'}")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
