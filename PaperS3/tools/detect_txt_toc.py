#!/usr/bin/env python3
"""Detect TXT encoding and generate a chapter/volume table of contents.

This host-side helper mirrors the firmware chapter-detection rules and is used
for large real novels before wiring them into the PaperS3 reader pipeline.
"""
from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

CHAPTER_RE = re.compile(
    r"^\s*(?:正文\s*[:：\-]?\s*)?[【\[\(（]?"
    r"(第(?P<num>[一二三四五六七八九十百千万零〇两0-9０-９]+)(?P<kind>[章节卷部集回篇])(?P<title>[^\n\r】\]\)）]{0,80}))"
    r"[】\]\)）]?\s*$"
)
VOLUME_PREFIX_RE = re.compile(
    r"^\s*(?P<kind>[卷部篇册])(?P<num>[一二三四五六七八九十百千万零〇两0-9０-９]+)(?P<title>[^\n\r]{0,80})\s*$"
)
ALT_RE = re.compile(r"^\s*(?:正文\s*[:：\-]?\s*)?[【\[\(（]?((?:Chapter|CHAPTER|Ch\.?|CH\.?)[\s.]+(?P<num>[0-9]+)(?P<title>[^\n\r】\]\)）]{0,80}))[】\]\)）]?\s*$")

CN_DIGITS = {"零": 0, "〇": 0, "一": 1, "二": 2, "两": 2, "三": 3, "四": 4, "五": 5, "六": 6, "七": 7, "八": 8, "九": 9}
CN_UNITS = {"十": 10, "百": 100, "千": 1000}

def detect_encoding(data: bytes) -> str:
    if data.startswith(b"\xef\xbb\xbf"):
        return "utf-8-sig"
    for enc in ("utf-8", "gb18030", "gbk", "big5"):
        try:
            data.decode(enc)
            return enc
        except UnicodeDecodeError:
            pass
    return "utf-8"


def cn_to_int(text: str) -> int:
    text = text.translate(str.maketrans("０１２３４５６７８９", "0123456789"))
    if text.isdigit():
        return int(text)
    total = 0
    section = 0
    number = 0
    for ch in text:
        if ch in CN_DIGITS:
            number = CN_DIGITS[ch]
        elif ch in CN_UNITS:
            if number == 0:
                number = 1
            section += number * CN_UNITS[ch]
            number = 0
        elif ch == "万":
            total += (section + number) * 10000
            section = 0
            number = 0
    return total + section + number


def iter_lines_with_offsets(text: str):
    offset = 0
    for line in text.splitlines(keepends=True):
        bare = line.rstrip("\r\n")
        yield offset, bare
        offset += len(line.encode("utf-8"))


def detect_toc(text: str, max_entries: int = 2000) -> list[dict]:
    entries: list[dict] = []
    last_offset = -10**9
    last_number: int | None = None
    for char_offset, line in iter_lines_with_offsets(text):
        stripped = line.strip(" \t\u3000")
        if not stripped or len(stripped) > 90:
            continue
        m = CHAPTER_RE.match(stripped)
        kind = None
        number = None
        title = None
        if m:
            kind = m.group("kind")
            number = cn_to_int(m.group("num"))
            title = m.group(1).strip()
        else:
            m = VOLUME_PREFIX_RE.match(stripped)
            if m:
                kind = m.group("kind")
                number = cn_to_int(m.group("num"))
                suffix = m.group('title').strip()
                title = f"第{number}{kind}" + (f" {suffix}" if suffix else "")
        if not m:
            m = ALT_RE.match(stripped)
            if m:
                kind = "章"
                number = int(m.group("num"))
                title = m.group(1).strip()
        if not m or not title:
            continue
        if number is None or number <= 0:
            continue
        entry_type = "volume" if kind in {"卷", "部", "集", "篇"} else "chapter"
        # Avoid duplicate/mislabeled web TXT headings before applying the dense-line
        # guard; otherwise a duplicate "...免费阅读" heading can hide the real next
        # chapter that follows only a few lines later.
        if entry_type == "chapter" and last_number is not None:
            if number == last_number:
                continue
            if number > last_number + 50:
                continue
            if number < last_number and last_number - number < 50:
                continue
        # Avoid dense false positives inside prose/code blocks, but allow a chapter
        # immediately after a volume heading (common TXT layout: 卷标题, blank, 第一章).
        if entries and char_offset - last_offset < 80 and entries[-1]["type"] != "volume":
            continue
        score = 100 if kind in {"章", "卷", "回"} else 85
        if char_offset - last_offset > 1000:
            score = min(100, score + 5)
        entries.append({
            "index": len(entries),
            "type": entry_type,
            "number": number,
            "title": title,
            "charOffset": char_offset,
            "score": score,
        })
        last_offset = char_offset
        if entry_type == "chapter":
            last_number = number
        if len(entries) >= max_entries:
            break
    return entries


def main() -> int:
    parser = argparse.ArgumentParser(description="Detect TXT TOC")
    parser.add_argument("txt", type=Path)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--preview", type=int, default=8)
    args = parser.parse_args()

    data = args.txt.read_bytes()
    enc = detect_encoding(data)
    text = data.decode(enc, errors="replace")
    toc = detect_toc(text)
    result = {
        "source": str(args.txt),
        "encoding": enc,
        "bytes": len(data),
        "chars": len(text),
        "lines": text.count("\n") + 1,
        "chapterCount": len(toc),
        "toc": toc,
    }
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"encoding: {enc}")
    print(f"bytes: {len(data)}")
    print(f"chars: {len(text)}")
    print(f"lines: {result['lines']}")
    print(f"chapters: {len(toc)}")
    for e in toc[: args.preview]:
        print(f"[{e['index']:03d}] {e['type']} #{e['number']} @{e['charOffset']}: {e['title']}")
    if len(toc) > args.preview:
        print("...")
        for e in toc[-min(args.preview, len(toc)):]:
            print(f"[{e['index']:03d}] {e['type']} #{e['number']} @{e['charOffset']}: {e['title']}")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
