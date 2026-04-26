#!/usr/bin/env python3
"""
字库生成工具：从 TTF/OTF 字体生成 Vink-PaperS3 使用的二进制点阵字库

依赖：
    pip install pillow

用法：
    python generate_font.py --input font.ttf --output font24.fnt --size 24 --chars chars.txt

参数：
    --input     输入字体文件（TTF/OTF）
    --output    输出字库文件（.fnt）
    --size      字号（像素高度）
    --chars     字符列表文件（UTF-8 编码，每行一个字符或连续字符范围）

字符列表文件格式示例：
    # 常用汉字
    的一是在不了有和人这中大为上个国我以要他时来用们生到作地于出就分对成
    # 英文和数字
    abcdefghijklmnopqrstuvwxyz
    ABCDEFGHIJKLMNOPQRSTUVWXYZ
    0123456789
    # 标点符号
    ，。！？、；：“”‘'（）《》【】
"""

import argparse
import struct
import os
from PIL import Image, ImageDraw, ImageFont

def load_chars(chars_file):
    """加载字符列表"""
    chars = set()
    
    with open(chars_file, 'r', encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            for ch in line:
                chars.add(ch)
    
    # 强制包含 ASCII
    for c in range(32, 127):
        chars.add(chr(c))
    
    # 强制包含换行
    chars.add('\n')
    chars.add('\r')
    
    return sorted(chars, key=lambda c: ord(c))

def render_char(font, char, size):
    """渲染单个字符，返回位图数据"""
    # 获取字符尺寸
    bbox = font.getbbox(char)
    if bbox is None:
        return None, 0, 0
    
    left, top, right, bottom = bbox
    width = right - left
    height = bottom - top
    
    if width <= 0 or height <= 0:
        # 空白字符（如空格）
        width = size // 2
        height = size
        return bytes(((width + 7) // 8) * height), width, height
    
    # 创建图像
    img = Image.new('1', (width, height), 0)
    draw = ImageDraw.Draw(img)
    draw.text((-left, -top), char, font=font, fill=1)
    
    # 转换为位图数据（1bpp）
    bitmap = []
    row_bytes = (width + 7) // 8
    
    for y in range(height):
        row = []
        for x in range(width):
            pixel = img.getpixel((x, y))
            row.append(1 if pixel > 0 else 0)
        
        # 打包为字节
        for byte_start in range(0, width, 8):
            byte_val = 0
            for bit in range(8):
                if byte_start + bit < width and row[byte_start + bit]:
                    byte_val |= (1 << (7 - bit))
            bitmap.append(byte_val)
    
    return bytes(bitmap), width, height

def generate_font(input_path, output_path, size, chars):
    """生成字库文件"""
    print(f"Loading font: {input_path}")
    font = ImageFont.truetype(input_path, size)
    
    print(f"Rendering {len(chars)} characters...")
    
    char_data = []
    total_bitmap_size = 0
    
    for i, ch in enumerate(chars):
        bitmap, width, height = render_char(font, ch, size)
        if bitmap is None:
            continue
        
        char_data.append({
            'unicode': ord(ch),
            'width': width,
            'height': height,
            'bitmap': bitmap,
            'offset': 0  # 稍后填充
        })
        total_bitmap_size += len(bitmap)
        
        if (i + 1) % 100 == 0:
            print(f"  {i + 1}/{len(chars)} done")
    
    print(f"Total characters: {len(char_data)}")
    print(f"Total bitmap size: {total_bitmap_size} bytes")
    
    # 计算偏移
    header_size = 4 + 2 + 2 + 4  # magic + version + fontSize + charCount
    index_size = len(char_data) * (4 + 4 + 1 + 1)  # unicode + offset + width + height
    
    current_offset = header_size + index_size
    for item in char_data:
        item['offset'] = current_offset
        current_offset += len(item['bitmap'])
    
    # 写入文件
    print(f"Writing: {output_path}")
    with open(output_path, 'wb') as f:
        # 文件头
        f.write(b'FNT\x00')                           # magic
        f.write(struct.pack('<H', 1))                 # version
        f.write(struct.pack('<H', size))              # fontSize
        f.write(struct.pack('<I', len(char_data)))    # charCount
        
        # 索引表
        for item in char_data:
            f.write(struct.pack('<I', item['unicode']))
            f.write(struct.pack('<I', item['offset']))
            f.write(struct.pack('<B', item['width']))
            f.write(struct.pack('<B', item['height']))
        
        # 位图数据
        for item in char_data:
            f.write(item['bitmap'])
    
    file_size = os.path.getsize(output_path)
    print(f"Done! File size: {file_size} bytes ({file_size/1024:.1f} KB)")

def main():
    parser = argparse.ArgumentParser(description='Generate bitmap font for Vink-PaperS3')
    parser.add_argument('--input', '-i', required=True, help='Input TTF/OTF font file')
    parser.add_argument('--output', '-o', required=True, help='Output .fnt file')
    parser.add_argument('--size', '-s', type=int, default=24, help='Font size in pixels')
    parser.add_argument('--chars', '-c', default='', help='Characters file (UTF-8)')
    
    args = parser.parse_args()
    
    if args.chars and os.path.exists(args.chars):
        chars = load_chars(args.chars)
    else:
        # 默认：常用汉字 3500 + ASCII
        chars = []
        # ASCII
        for c in range(32, 127):
            chars.append(chr(c))
        # 常用标点
        chars.extend(list('，。！？、；：“”‘'（）《》【】—…·'))
        print("Warning: No chars file specified, using default ASCII + punctuation only")
        print("         For Chinese support, provide a chars file with common Hanzi")
    
    generate_font(args.input, args.output, args.size, chars)

if __name__ == '__main__':
    main()
