#!/usr/bin/env python3
"""Generate a compact built-in 1bpp Chinese font header for firmware flash.

The generated src/BuiltinFont.h is intentionally independent of SD-card font
files. It stores FontHeader_1bpp, CharIndex_1bpp and bitmap bytes in PROGMEM.
Bitmap offsets are relative to BUILTIN_FONT_BITMAPS (not to a file).

Dependencies:
    pip install freetype-py
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
from typing import Iterable

import freetype

FONT_SIZE = 16
DEFAULT_COUNT = 500

# Seeded with the requested common characters, then extended from the repo's
# chars_500/chars_3500 files and finally from CJK Unified Ideographs if needed.
COMMON_HAN_SEED = (
    "的一是在不了有和人这中大为上个国我以要他时来用们生到作地于出就分对成"
    "可主发年动同工也能下过子说产种面而方后多定行学法所民得经十三之进着等"
    "部度家电力里如水化高自二理起小物现实加量都两体制机当使点从业本去把性好"
    "应开它合还因由其些然前外天政四日那社义事平形相全表间样与关各重新线内数"
    "正心反你明看原又么利比或但质气第向道命此变条只没结解问意建月公无系军很"
    "情者最立代想已通并提直题党程展五果料象员革位入常文总次品式活设及管特件"
    "长求老头基资边流路级少图山统接知较将组见计别她手角期根论运农指几九区强"
    "放决西被干做必战先回则任取完举色权近远诉呀呢吗嘛吧呐呗咚嗡咿呜呕喔哎咦"
    "嘻嘿哼呵哈咬咱咳哇哟唷喂喃咕噜给让除按照据跟且虽即尽既否终曾快马刻顿"
    "忽突猛陡骤竟居"
)

# Fallback extension: the first 500 characters of a widely used frequency list.
# Kept local so the generator is deterministic/offline.
COMMON_HAN_EXT = (
    "的一是不了在人有我他这中大来上个国到说们为子和你地出道也时年得就那要下以生会"
    "自着去之过家学对可她里后小么心多天而能好都然没日于起还发成事只作当想看文无开"
    "手十用主行方又如前所本见经头面公同三已老从动两长知民样现分将外但身些与高意进"
    "把法此实回二理美点月明其种声全工己话儿者向情部正名定女问力机给等几很业最间新"
    "什打便位因重被走电四第门相次东政海口使教西再平真听世气信北少关并内加化由却代"
    "军产入先山五太水万市眼体别处总才场师书比住员九笑性通目华报立马命张活难神数件"
    "安表原车白应路期叫死常提感金何更反合放做系计或司利受光王果亲界及今京务制解各"
    "任至清物台象记边共风战干接它许八特觉望直服毛林题建南度统色字请交爱让认算论百"
    "吃义科怎元社术结六功指思非流每青管夫连远资队跟带花快条院变联言权往展该领传近"
    "留红治决周保达办运武半候七必城父强步完革深区即求品士转量空甚众技轻程告江语英"
    "基派满式李息写呢识极令黄德收脸钱党倒未持取设始版双历越史商千片容研像找友孩站"
    "广改议形委早房音火际则首单据导影失拿网香似斯专石若兵弟谁校读志飞观争究包组造"
    "落视济喜离虽坐集编宝谈府拉黑且随格尽剑讲布杀微怕母调局根曾准团段终乐切级克精"
)

PUNCTUATION = "，。！？、；：”“‘’（）《》【】—…·"
ASCII = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"


def unique_chars(text: Iterable[str]) -> list[str]:
    seen: set[str] = set()
    out: list[str] = []
    for ch in text:
        if not ch.strip() or ch in seen:
            continue
        seen.add(ch)
        out.append(ch)
    return out


def load_repo_chars(repo_root: Path) -> str:
    pieces = []
    for name in ("tools/chars_500.txt", "tools/chars_3500.txt"):
        path = repo_root / name
        if not path.exists():
            continue
        for line in path.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            pieces.append(line)
    return "".join(pieces)


def build_common_chars(repo_root: Path, count: int) -> list[str]:
    chars = unique_chars(COMMON_HAN_SEED + load_repo_chars(repo_root) + COMMON_HAN_EXT)
    # Last-resort deterministic fill so charCount remains exactly requested.
    cp = 0x4E00
    while len(chars) < count and cp <= 0x9FFF:
        ch = chr(cp)
        if ch not in chars:
            chars.append(ch)
        cp += 1
    return chars[:count]


def find_default_font() -> Path:
    candidates = [
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
        "/System/Library/Fonts/PingFang.ttc",
    ]
    for candidate in candidates:
        if os.path.exists(candidate):
            return Path(candidate)
    raise FileNotFoundError("No CJK font found; pass --font /path/to/font.ttf")


def render_char(face: freetype.Face, ch: str, threshold: int) -> tuple[bytes, int, int]:
    face.load_char(ch, freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_NORMAL)
    bitmap = face.glyph.bitmap
    width = min(bitmap.width, FONT_SIZE)
    height = min(bitmap.rows, FONT_SIZE)

    if width <= 0 or height <= 0:
        width = FONT_SIZE // 2
        height = FONT_SIZE
        return bytes(((width + 7) // 8) * height), width, height

    src_pitch = abs(bitmap.pitch)
    src = bytes(bitmap.buffer)
    row_bytes = (width + 7) // 8
    out = bytearray(row_bytes * height)

    for y in range(height):
        for x in range(width):
            value = src[y * src_pitch + x]
            if value >= threshold:
                out[y * row_bytes + x // 8] |= 1 << (7 - (x % 8))
    return bytes(out), width, height


def c_array(data: bytes, indent: str = "    ", per_line: int = 16) -> str:
    lines = []
    for i in range(0, len(data), per_line):
        chunk = data[i : i + per_line]
        lines.append(indent + ", ".join(f"0x{b:02X}" for b in chunk) + ",")
    return "\n".join(lines)


def generate(repo_root: Path, font_path: Path, output: Path, count: int, threshold: int) -> None:
    face = freetype.Face(str(font_path))
    face.set_pixel_sizes(0, FONT_SIZE)

    chars = build_common_chars(repo_root, count)
    entries = []
    bitmap_blob = bytearray()
    for ch in sorted(chars, key=ord):
        bitmap, width, height = render_char(face, ch, threshold)
        offset = len(bitmap_blob)
        bitmap_blob.extend(bitmap)
        entries.append((ord(ch), offset, width, height, ch))

    header = f"""#pragma once
#include <Arduino.h>
#include <pgmspace.h>

// Auto-generated by tools/generate_builtin_font.py. Do not edit by hand.
// Source font: {font_path}
// Format: 1bpp bitmaps, offsets relative to BUILTIN_FONT_BITMAPS.

static const FontHeader_1bpp BUILTIN_FONT_HEADER PROGMEM = {{
    {{'B', 'I', 'N', 'T'}},
    1,
    {FONT_SIZE},
    {len(entries)}
}};

static const CharIndex_1bpp BUILTIN_FONT_INDEX[] PROGMEM = {{
"""
    index_lines = [f"    {{0x{cp:04X}, {offset}, {width}, {height}}}, // {ch}" for cp, offset, width, height, ch in entries]
    footer = f"""
}};

static const uint8_t BUILTIN_FONT_BITMAPS[] PROGMEM = {{
{c_array(bytes(bitmap_blob))}
}};

static const uint32_t BUILTIN_FONT_BITMAP_SIZE = {len(bitmap_blob)};
"""
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(header + "\n".join(index_lines) + footer, encoding="utf-8")

    total_est = 12 + len(entries) * 12 + len(bitmap_blob)
    print(f"Generated {output}")
    print(f"chars={len(entries)} bitmap={len(bitmap_blob)} bytes estimated_flash={total_est} bytes ({total_est/1024:.1f} KiB)")
    if total_est > 32 * 1024:
        raise SystemExit(f"Generated font is too large: {total_est} bytes > 32 KiB")


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate built-in 1bpp Chinese font header")
    parser.add_argument("--font", type=Path, default=None, help="CJK TTF/TTC/OTF font path")
    parser.add_argument("--output", type=Path, default=Path("src/BuiltinFont.h"))
    parser.add_argument("--count", type=int, default=DEFAULT_COUNT)
    parser.add_argument("--threshold", type=int, default=96)
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parents[1]
    font_path = args.font or find_default_font()
    output = args.output if args.output.is_absolute() else repo_root / args.output
    generate(repo_root, font_path, output, args.count, args.threshold)


if __name__ == "__main__":
    main()
