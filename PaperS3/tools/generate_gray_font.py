#!/usr/bin/env python3
"""
灰度字库生成工具：从 TTF/OTF 字体生成 4bpp 灰度点阵字库

依赖：
    pip install freetype-py numpy

用法：
    python generate_gray_font.py --input font.ttf --output font24_gray.fnt --size 24 --chars chars.txt

参数：
    --input     输入字体文件（TTF/OTF）
    --output    输出字库文件（.fnt）
    --size      字号（像素高度）
    --chars     字符列表文件（UTF-8 编码，每行一个字符或连续字符范围）

作者：OpenClaw
日期：2026-04-24
"""

import argparse
import struct
import os
import json

try:
    import freetype
except ImportError:
    print("错误：需要安装 freetype-py")
    print("  pip install freetype-py")
    exit(1)


def load_chars(chars_file):
    """加载字符列表"""
    chars = set()
    
    if chars_file and os.path.exists(chars_file):
        with open(chars_file, 'r', encoding='utf-8') as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith('#'):
                    continue
                for ch in line:
                    chars.add(ch)
    else:
        # 默认：常用汉字 3500
        print("未提供字符表，使用默认常用汉字...")
        default_chars = """的一是在不了有和人这中大为上个国我以要他时来用们生到作地于出就分对成
会可主发年动同工也能下过子说产种面而方后多定行学法所民得经十三之进着等
部度家电力里如水化高自二理起小物现实加量都两体制机当使点从业本去把性好
应开它合还因由其些然前外天政四日那社义事平形相全表间样与关各重新线内数
正心反你明看原又么利比或但质气第向道命此变条只没结解问意建月公无系军很
情者最立代想已通并提直题党程展五果料象员革位入常文总次品式活设及管特件
长求老头基资边流路级少图山统接知较将组见计别她手角期根论运农指几九区强
放决西被干做必战先回则任取完举色权设近远诉知呀呢吗嘛吧呐呗咚嗡咿呜呕喔
哎咦嘻嘿哼呵哈咬咱咳哇哎哟哎呀哎唷哟喂呢喃咕咚咕噜"""
        for ch in default_chars:
            if not ch.isspace():
                chars.add(ch)
    
    # 强制包含 ASCII
    for c in range(32, 127):
        chars.add(chr(c))
    
    # 强制包含常用标点
    for ch in '，。！？、；：""''（）《》【】—…·\n\r\t ':
        chars.add(ch)
    
    return sorted(chars, key=lambda c: ord(c))


def render_char_gray(face, char, size):
    """渲染单个字符为灰度图，返回 4bpp 数据"""
    # 设置字符大小
    face.set_char_size(size * 64, size * 64)  # 1/64 pixel
    
    # 加载字符
    char_code = ord(char)
    
    # 处理控制字符（空格、换行等）
    if char_code < 32 or char_code == 0x20:
        # 空白字符
        width = size // 2 if char_code == 0x20 else 0  # 空格半宽
        height = size
        advance = width
        return bytes((width + 1) // 2 * height), width, height, advance, 0, 0
    
    face.load_char(char_code, freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_NORMAL)
    
    bitmap = face.glyph.bitmap
    width = bitmap.width
    height = bitmap.rows
    
    if width == 0 or height == 0:
        # 空白字符（如空格）
        width = size // 2
        height = size
        advance = width
        return bytes((width + 1) // 2 * height), width, height, advance, 0, 0
    
    # 获取灰度数据
    gray_data = bitmap.buffer
    
    # 转换为 4bpp（16级灰度）
    row_bytes_4bpp = (width + 1) // 2  # 每行字节数（4bpp，2像素/字节）
    bitmap_4bpp = bytearray(row_bytes_4bpp * height)
    
    for y in range(height):
        for x in range(width):
            # 获取原始灰度值 (0-255)
            gray = gray_data[y * bitmap.pitch + x]
            # 量化为 16 级 (0-15)
            gray_4bit = min(15, gray // 16)
            
            # 写入 4bpp
            byte_idx = y * row_bytes_4bpp + x // 2
            if x % 2 == 0:
                bitmap_4bpp[byte_idx] = (gray_4bit << 4)
            else:
                bitmap_4bpp[byte_idx] |= gray_4bit
    
    # 获取度量信息
    metrics = face.glyph.metrics
    advance = metrics.horiAdvance >> 6  # 水平步进
    bearing_x = metrics.horiBearingX >> 6
    bearing_y = metrics.horiBearingY >> 6
    
    return bytes(bitmap_4bpp), width, height, advance, bearing_x, bearing_y


def generate_gray_font(input_path, output_path, size, chars, face_index=0):
    """生成 4bpp 灰度字库文件"""
    print(f"加载字体: {input_path} (face index {face_index})")
    face = freetype.Face(input_path, face_index)
    
    print(f"字体信息:")
    family_name = face.family_name.decode('utf-8') if face.family_name else "Unknown"
    style_name = face.style_name.decode('utf-8') if face.style_name else "Regular"
    print(f"  家族名: {family_name}")
    print(f"  风格: {style_name}")
    print(f"  字形数: {face.num_glyphs}")
    
    print(f"\n渲染 {len(chars)} 个字符 (字号 {size}px)...")
    
    char_data = []
    total_bitmap_size = 0
    failed_chars = []
    
    for i, ch in enumerate(chars):
        try:
            result = render_char_gray(face, ch, size)
            bitmap, width, height, advance, bearing_x, bearing_y = result
            
            char_data.append({
                'unicode': ord(ch),
                'width': width,
                'height': height,
                'advance': advance,
                'bearingX': bearing_x,
                'bearingY': bearing_y,
                'bitmap': bitmap,
                'offset': 0  # 稍后填充
            })
            total_bitmap_size += len(bitmap)
            
            if (i + 1) % 500 == 0:
                print(f"  {i + 1}/{len(chars)} 完成")
                
        except Exception as e:
            failed_chars.append(ch)
            if len(failed_chars) <= 5:
                print(f"  警告: 字符 U+{ord(ch):04X} ('{ch}') 渲染失败: {e}")
    
    if failed_chars:
        print(f"\n共 {len(failed_chars)} 个字符渲染失败")
    
    print(f"\n统计:")
    print(f"  成功字符: {len(char_data)}")
    print(f"  位图数据: {total_bitmap_size} bytes ({total_bitmap_size/1024/1024:.2f} MB)")
    
    # 计算文件布局
    # 文件头: 16 bytes
    # 索引表: 每个字符 16 bytes (unicode + offset + width + height + advance + bearingX + bearingY + reserved)
    header_size = 16
    index_entry_size = 16
    index_size = len(char_data) * index_entry_size
    
    current_offset = header_size + index_size
    for item in char_data:
        item['offset'] = current_offset
        current_offset += len(item['bitmap'])
    
    # 写入字库文件
    print(f"\n写入: {output_path}")
    with open(output_path, 'wb') as f:
        # 文件头 (16 bytes)
        f.write(b'GRAY')                           # magic (4 bytes)
        f.write(struct.pack('<H', 1))              # version (2 bytes)
        f.write(struct.pack('<H', size))           # fontSize (2 bytes)
        f.write(struct.pack('<I', len(char_data))) # charCount (4 bytes)
        f.write(struct.pack('<I', total_bitmap_size)) # bitmapDataSize (4 bytes)
        
        # 索引表
        for item in char_data:
            f.write(struct.pack('<I', item['unicode']))   # unicode (4 bytes)
            f.write(struct.pack('<I', item['offset']))    # bitmapOffset (4 bytes)
            f.write(struct.pack('<B', item['width']))     # width (1 byte)
            f.write(struct.pack('<B', item['height']))    # height (1 byte)
            f.write(struct.pack('<b', item['bearingX']))  # bearingX (1 byte, signed)
            f.write(struct.pack('<b', item['bearingY']))  # bearingY (1 byte, signed)
            f.write(struct.pack('<B', min(255, item['advance'])))  # advance (1 byte)
            f.write(struct.pack('<B', 0))                  # reserved (1 byte)
            f.write(struct.pack('<H', 0))                  # reserved (2 bytes)
        
        # 位图数据
        for item in char_data:
            f.write(item['bitmap'])
    
    # 生成元数据 JSON
    meta_path = output_path.replace('.fnt', '.json')
    meta = {
        'magic': 'GRAY',
        'version': 1,
        'fontSize': size,
        'charCount': len(char_data),
        'bitmapDataSize': total_bitmap_size,
        'fontFamily': face.family_name.decode('utf-8') if face.family_name else "Unknown",
        'fontStyle': face.style_name.decode('utf-8') if face.style_name else "Regular",
        'sourceFile': os.path.basename(input_path),
        'faceIndex': face_index,
        'generatedBy': 'generate_gray_font.py'
    }
    with open(meta_path, 'w', encoding='utf-8') as f:
        json.dump(meta, f, ensure_ascii=False, indent=2)
    
    file_size = os.path.getsize(output_path)
    print(f"完成!")
    print(f"  字库文件: {file_size} bytes ({file_size/1024/1024:.2f} MB)")
    print(f"  元数据: {meta_path}")
    
    # 估算内存占用
    mem_kb = (len(char_data) * index_entry_size + total_bitmap_size) / 1024
    print(f"  加载到内存约: {mem_kb:.1f} KB")
    
    return len(char_data), file_size


def main():
    parser = argparse.ArgumentParser(
        description='生成 4bpp 灰度点阵字库 for Vink-PaperS3',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例:
  # 使用默认字符集（常用汉字）
  python generate_gray_font.py -i LXGWWenKai.ttf -o wenkai24.fnt -s 24
  
  # 使用自定义字符表
  python generate_gray_font.py -i LXGWWenKai.ttf -o wenkai24.fnt -s 24 -c mychars.txt
  
  # 生成不同字号
  python generate_gray_font.py -i LXGWWenKai.ttf -o wenkai16.fnt -s 16
  python generate_gray_font.py -i LXGWWenKai.ttf -o wenkai32.fnt -s 32
        """)
    
    parser.add_argument('--input', '-i', required=True, help='输入 TTF/OTF 字体文件')
    parser.add_argument('--output', '-o', required=True, help='输出 .fnt 字库文件')
    parser.add_argument('--size', '-s', type=int, default=24, help='字号（像素高度，默认24）')
    parser.add_argument('--chars', '-c', default='', help='字符列表文件（UTF-8，每行字符）')
    parser.add_argument('--face-index', type=int, default=0, help='TTC 字体 face index；Noto Sans CJK SC 为 2')
    
    args = parser.parse_args()
    
    if not os.path.exists(args.input):
        print(f"错误: 字体文件不存在: {args.input}")
        exit(1)
    
    chars = load_chars(args.chars) if args.chars else load_chars(None)
    generate_gray_font(args.input, args.output, args.size, chars, args.face_index)


if __name__ == '__main__':
    main()
