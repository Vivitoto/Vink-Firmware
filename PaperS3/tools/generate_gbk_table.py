#!/usr/bin/env python3
"""
Generate GBK-to-Unicode mapping table for ESP32 firmware.
Covers GB2312 Level 1 (0xB0A1-0xD7FA) and common symbols (0xA1A1-0xA9FE).
Output: a compact uint16_t lookup table for direct indexing.
"""

# GBK/GB2312 layout for level-1 Chinese characters:
#   First byte:  0xB0 ~ 0xD7  (24 zones)
#   Second byte: 0xA1 ~ 0xFE  (94 positions per zone)
# For symbols:
#   First byte:  0xA1 ~ 0xA9  (9 zones)
# We generate one unified table from 0xA1 to 0xD7 inclusive.
# Total zones = 0xD7 - 0xA1 + 1 = 55
# Each zone has 94 positions (0xA1~0xFE).
# Table size = 55 * 94 = 5170 entries.
# Each entry = uint16_t Unicode code point (0 = unmapped).
# Total bytes = 5170 * 2 = 10,340 bytes (~10KB Flash).

import codecs

FIRST_START = 0xA1
FIRST_END   = 0xD7   # inclusive; stops before 0xD8~0xFE (GBK extensions / level-2)
ZONES       = FIRST_END - FIRST_START + 1   # 55
POS_PER_ZONE = 0xFE - 0xA1 + 1               # 94

def main():
    table = [0] * (ZONES * POS_PER_ZONE)
    
    for first in range(FIRST_START, FIRST_END + 1):
        for second in range(0xA1, 0xFE + 1):
            gbk_bytes = bytes([first, second])
            try:
                ch = gbk_bytes.decode('gbk')
                cp = ord(ch)
                # Only store BMP characters (most Chinese chars are < 0xFFFF)
                if cp <= 0xFFFF:
                    zone = first - FIRST_START
                    pos  = second - 0xA1
                    idx  = zone * POS_PER_ZONE + pos
                    table[idx] = cp
            except UnicodeDecodeError:
                pass  # leave as 0
    
    # Count mapped entries
    mapped = sum(1 for v in table if v != 0)
    print(f"Mapped entries: {mapped} / {len(table)}")
    
    # Write C header
    with open('src/GBKTable.h', 'w', encoding='utf-8') as f:
        f.write('#pragma once\n')
        f.write('#include <Arduino.h>\n\n')
        f.write('// Auto-generated GBK-to-Unicode lookup table\n')
        f.write(f'// Zones: {ZONES}, Positions per zone: {POS_PER_ZONE}\n')
        f.write(f'// Coverage: 0x{FIRST_START:02X}A1 ~ 0x{FIRST_END:02X}FE (GB2312 Level-1 + symbols)\n')
        f.write(f'// Mapped entries: {mapped}\n\n')
        
        f.write(f'#define GBK_ZONE_START    0x{FIRST_START:02X}\n')
        f.write(f'#define GBK_ZONE_COUNT    {ZONES}\n')
        f.write(f'#define GBK_POS_PER_ZONE  {POS_PER_ZONE}\n')
        f.write(f'#define GBK_TABLE_SIZE    {len(table)}\n\n')
        
        f.write('// Lookup by: index = (firstByte - GBK_ZONE_START) * GBK_POS_PER_ZONE + (secondByte - 0xA1)\n')
        f.write(f'const uint16_t GBK_UNICODE_TABLE[{len(table)}] PROGMEM = {{\n')
        
        # Write in rows of 16 for readability
        for i in range(0, len(table), 16):
            row = table[i:i+16]
            vals = ', '.join(f'0x{v:04X}' for v in row)
            f.write(f'    {vals},\n')
        
        f.write('};\n')
    
    print("Written to src/GBKTable.h")

if __name__ == '__main__':
    main()
