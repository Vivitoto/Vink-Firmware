# EDCBook Antialias / Font Rendering Deep Dive

Date: 2026-05-15
Scope: reverse-driven investigation of EDCBook/梦西游 PaperS3 text antialiasing. This is not based on the shallow official “16 gray levels” explanation.

## Evidence sources

- `/home/vito/.openclaw/workspace/edcbook_reverse/edcbook_v2_font.bin`
- `/home/vito/.openclaw/workspace/edcbook_reverse/edcbook_v2_app0.elf`
- `/home/vito/.openclaw/workspace/edcbook_reverse/edcbook_v2_app0.objdump.txt`
- `/home/vito/.openclaw/workspace/Vink/reference-firmware/M5ReadPaper-latest/tools/edcbook/EDCBook_FontTool_1.2.py`
- `/home/vito/.openclaw/workspace/Vink/reference-firmware/M5ReadPaper-latest/tools/edcbook/bin_font_generator.py.ref`

## 1. EDCBook font.bin format

`edcbook_v2_font.bin` is a dedicated EDCBook bitmap-font container, not a raw TTF and not a simple monochrome dot matrix.

Observed structure:

```text
file size       = 10,485,760 bytes
valid end       = 8,633,430 bytes
trailing bytes  = 0xFF padding
char_count      = 35,714
font_height     = 30
entry_size      = 20 bytes
entry area end  = 714,285
codepoint range = U+0000..U+FFFD
entries sorted  = true
```

Header:

```c
uint32_t char_count;   // little-endian
uint8_t  font_height;
```

Entry format, matching `struct.pack('<HHBBbbIII')` in the EDCBook font tool:

```c
struct GlyphEntry {
    uint16_t unicode;        // BMP codepoint
    uint16_t advance_width;
    uint8_t  bitmap_w;
    uint8_t  bitmap_h;
    int8_t   x_offset;
    int8_t   y_offset;
    uint32_t bitmap_offset;
    uint32_t bitmap_size;
    uint32_t cached_bitmap;  // initially 0; firmware writes runtime cache address
};
```

## 2. Bitmap encoding: compressed 15-gray antialias data

The glyph bitmap payload is Huffman-like variable-length encoding:

```text
level 15 / white -> bit "0"
level 0 / black  -> bits "10"
level 1..14 gray -> bits "11" + 4-bit level
```

Generator evidence in `EDCBook_FontTool_1.2.py`:

```text
quantized[low_mask]  = 15
quantized[high_mask] = 0
quantized[mid_mask]  = (black_threshold - cropped_area[mid_mask]) // 14
15 -> '0'
0  -> '10'
gray -> '11' + 4 bits
```

Important semantic note:

- EDCBook: `0 = black`, `15 = white / skip`.
- Vink packed reader font currently uses the opposite drawing intuition in parts of the pipeline (`0` often behaves as white/transparent, high nibble as darker ink). Any mapping table copied from EDCBook must be inverted/adapted.

Sample decode results showed real gray antialias data:

```text
'A'  levels 0..15 present, gray_pixels=79 / 483
'汉' levels 0..15 present, gray_pixels=136 / 700
'我' levels 0..15 present, gray_pixels=178 / 784
'一' levels 0..15 present, gray_pixels=44 / 108

first 5000 non-empty glyph sample:
gray_glyphs = 4925 / 4950
levels      = 0..15 all present
avg bytes/pixel ≈ 0.276
```

Conclusion: EDCBook’s default font is **FreeType pre-rendered grayscale glyphs + crop/offset metrics + variable-length compression + runtime cache**.

## 3. Firmware-side reverse evidence

Strings in `edcbook_v2_app0.elf` / objdump:

```text
[Font] U+%04X: 无效bitmap_size %u
[Font] U+%04X: 无效bitmap_offset %u (fileSize: %u)
[Font] U+%04X: 位图越界 offset=%u size=%u file=%u
[Font] 默认字体已加载. 字符数: %u, 字号: %d, Y偏移: %d
[Font] 已加载 %u 个字符, 最大位图: %u字节, 缓存数: %d, Y偏移: %d
[Font] U+%04X: 读取位图失败 期望 %u, 实际 %u
[Font] 找不到 U+%04X 的字符表项, 回退默认字体
[Font] U+%04X 加载字符数据失败, 回退默认字体
render_opt1
AntiAlias
- AntiAlias: Reduce text aliasing (jaggies) in Fast display mode.
Default Font.bin
Invalid Font.bin
efont
WenQuanYi Bitmap Song
LXGW WenKai
```

### Entry validation

Around `0x4200bf60`, firmware reads fields matching the 20-byte entry:

- bytes `0..1`: unicode
- bytes `8..11`: bitmap_offset
- bytes `12..15`: bitmap_size
- bytes `16..19`: cached bitmap pointer

Relevant xrefs:

```text
0x4200bf96 -> "[Font] U+%04X: 无效bitmap_size %u"
0x4200bfe2 -> "[Font] U+%04X: 无效bitmap_offset %u..."
0x4200c002 -> "[Font] U+%04X: 位图越界..."
```

### Default font loading

Around `0x4200c018`:

- reads `charCount`
- reads `fontSize`
- allocates `charCount * 20` entries
- logs default-font load success

This aligns with `font.bin` header + 20-byte table.

### Glyph load/cache

Around `0x4200c4e8`:

- validates `bitmap_size`
- seeks by `bitmap_offset`
- reads compressed bitmap
- writes runtime cache address back into entry bytes `16..19`

Relevant xrefs:

```text
0x4200c58d -> invalid bitmap_size
0x4200c61e -> seek bitmap failed
0x4200c68c -> read bitmap failed
0x4200c6a8..0x4200c6bc -> write cached_bitmap back into entry[16..19]
```

### Glyph decode/draw

Around `0x4200e774`, the core draw path:

- reads `cached_bitmap`
- reads `bitmap_w`, `bitmap_h`, `x_offset`, `y_offset`
- decodes bitstream according to `0`, `10`, `11xxxx`
- maps decoded levels through palette/threshold tables before drawing

Observed mapping-table literal:

```text
literal 0x420009f0 -> 0x3c1fca2c
```

First 64 bytes of table area:

```text
offset 0:
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f

offset 16:
00 00 01 02 03 04 05 06 07 08 09 09 0f 0f 0f 0f

offset 32:
00 00 00 00 00 00 06 06 06 06 06 0f 0f 0f 0f 0f

offset 48:
00 00 00 00 00 00 00 00 00 00 0f 0f 0f 0f 0f 0f
```

This strongly suggests multiple render palettes / threshold levels:

- identity 16-level grayscale
- soft threshold
- medium threshold
- hard threshold

Combined with `render_opt1` and `AntiAlias` strings, EDCBook does not merely draw grayscale levels directly. It selects a mapping depending on display/render mode.

## 4. Why EDCBook antialiasing looks good

High-confidence chain:

```text
FreeType normal hinting/rasterization
→ white/black dual-threshold quantization
→ cropped glyph + accurate offsets/advance
→ 15-gray compressed font.bin
→ firmware runtime decode/cache
→ mode-dependent 16-level palette/threshold mapping
→ EPD framebuffer
```

### A. FreeType pre-rendering matters

The generator uses:

```python
face.load_char(char, freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_NORMAL)
```

This gives FreeType’s normal rasterizer/hinting. For small Chinese text, this is usually better than raw `stb_truetype` rasterization.

### B. Dual-threshold quantization matters

Generator logic:

```text
coverage < white_threshold -> white level 15
coverage > black_threshold -> black level 0
middle coverage            -> gray 1..14
```

This removes weak fuzzy edge noise while preserving meaningful edge gray.

Practical effect:

- weak antialias halo does not pollute white background;
- black stroke cores stay solid;
- medium edges remain smooth.

### C. Crop + metric offsets matter

The tool crops to non-white content and stores `x_offset` / `y_offset` separately. Text alignment is not based on a naive fixed cell; glyphs keep proper bearings and baseline position.

### D. Runtime mapping tables matter

EDCBook has multiple 16-level mapping tables. This is likely how it keeps text acceptable in fast display modes: keep grayscale source data, but compress/threshold levels differently depending on mode and antialias setting.

## 5. Vink gap analysis

Relevant Vink files:

- `src/vink3/reader/ReaderTextRenderer.cpp`
- `src/vink3/reader/TtfFont.cpp`
- `tools/generate_gray_font.py`

### Vink already has a useful base

Vink’s body renderer already supports packed 4bpp grayscale and contrast mapping:

```cpp
pixelColorForNibble()
kInkBoost[16]
k4BitToRgb565[16]
```

The embedded Wenkai path is closer to EDCBook than the SD TTF live path.

### Main differences

1. **Vink SD TTF path uses `stb_truetype` live rendering**
   - `TtfFont.cpp` uses `stbtt_GetCodepointBitmap`.
   - This lacks FreeType’s normal hinting/raster quality.

2. **Vink TTF antialias switch is not a full render-mode participant**
   - `pixelColorForNibbleTtf()` always maps grayscale.
   - It does not fully mirror EDCBook’s `render_opt1 / AntiAlias / show_mode` interaction.

3. **Vink font generation is too linear**
   - Current generator uses simple linear quantization like `gray // 16`.
   - EDCBook uses dual thresholds: weak edges become white, strong cores become black, only middle remains gray.

4. **EDCBook uses runtime palette tables**
   - Vink has one main boost table, but not a family of mode-dependent EDC-style mappings.

5. **Runtime model differs**
   - EDCBook: compressed font.bin + on-demand cache pointer in glyph table.
   - Vink embedded Wenkai: direct packed 4bpp, faster/simple but less flexible.
   - Vink SD TTF: runtime raster cache, more expensive and probably lower quality than offline FreeType.

## 6. Recommended experiments for Vink

Do not start with a full text-engine rewrite. The highest-value experiments are smaller:

### Experiment 1 — EDC-style generator quantization

Add an EDC mode to `tools/generate_gray_font.py`:

```text
white_threshold = 32
black_threshold = 223
if coverage < white_threshold:
    level = white / transparent
elif coverage > black_threshold:
    level = black
else:
    level = mapped middle 1..14
```

Adapt for Vink nibble semantics.

Goal: determine whether “FreeType-style thresholded grayscale” alone gets close to EDCBook.

### Experiment 2 — EDC-style mapping tables in firmware

Compare at least three mapping tables:

```text
A. current Vink kInkBoost
B. EDC identity mapping, inverted/adapted
C. EDC medium/hard threshold mapping, inverted/adapted
```

Evaluate on the same page under fast/text/quality refresh.

### Experiment 3 — Keep SD TTF as secondary, prefer offline-generated reader fonts

EDCBook’s quality likely comes from offline FreeType generation + small runtime decode, not live TTF. Vink should keep SD TTF as optional, but the primary reading path should remain pre-generated grayscale font data.

### Experiment 4 — Fixed-character comparison sheet

Use these characters:

```text
一 的 我 汉 爱 ， 。 “ ” A a g y
```

Compare:

- decoded EDCBook `font.bin`
- current Vink Wenkai32
- Vink EDC-threshold generated font

Metrics:

- weak edge halo
- horizontal stroke breakage
- punctuation blur
- Latin descender alignment
- fast-refresh gray-edge ghosting

## Working conclusion

EDCBook’s antialiasing quality is not just “the display supports 16 gray levels.” It is a full chain:

```text
FreeType raster quality
+ dual-threshold quantization
+ 15-gray compressed glyphs
+ baseline/crop metrics
+ runtime cache
+ mode-dependent grayscale mapping
```

For Vink, the likely biggest wins are:

1. reproduce EDCBook’s generator-side thresholding;
2. add EDC-style render-mode mapping tables;
3. avoid relying on `stb_truetype` live TTF for the primary reader font;
4. compare with decoded EDCBook glyphs before making large architecture changes.

## Addendum — If newer EDCBook directly supports SD-card TTF

User reports a newer EDCBook version supports direct SD-card `.ttf` fonts while still achieving strong antialiasing. The current inspected `edcbook_v2_app0.elf` contains web/MIME strings for `.ttf`, `.otf`, `.woff`, `.woff2`, and `.sfnt`, but its user-facing help still says `/font` supports generated `.bin` files. No FreeType symbols were confirmed in this v2 ELF. Therefore the next-version TTF path is not proven by the current binary, but the likely deeper mechanism is below.

A high-quality direct-TTF path probably does **not** mean “call a simple stb bitmap function and draw raw coverage.” To match EDCBook quality it would need to preserve the same core chain:

```text
TTF outlines
→ high-quality rasterizer/hinting or oversampled rasterizer
→ EPD-specific coverage quantization
→ mode-dependent 16-level palette/threshold mapping
→ glyph cache keyed by font/size/codepoint/render mode
→ page framebuffer / EPD waveform tuned for gray text
```

Likely implementation models:

1. **On-device FreeType + PSRAM glyph cache**
   - Best quality: real hinting and accurate glyph metrics.
   - Cost: larger binary, heap/PSRAM pressure, slower first render.
   - Needs aggressive cache and possibly background pre-render of current/next page.

2. **On-device TTF-to-EDC cached bitmap conversion**
   - User sees “direct TTF,” but firmware converts glyphs/pages into the same EDC-style compressed gray glyph cache on first use.
   - This would explain good quality without rendering every glyph from outlines every time.
   - Cache may live in PSRAM during reading and/or persist on SD next to the font.

3. **stb_truetype but with oversampling + EDC quantization**
   - Lower dependency cost, still better than raw `stbtt_GetCodepointBitmap`.
   - Use `stbtt_MakeCodepointBitmapSubpixel` or `stbtt_PackSetOversampling`-style supersampling, then downsample with gamma/threshold.
   - Quality may approach FreeType for many fonts but likely weaker for CJK small sizes/hinting.

4. **Hybrid: pre-render per page, not per glyph draw**
   - Layout produces a page glyph list.
   - Renderer rasterizes all missing glyphs into a cache before display push.
   - Page drawing then blits cached 4bpp glyphs, keeping turn latency predictable.

For Vink, the direct-TTF improvement path should be:

- Do not treat TTF as a separate low-quality fallback.
- Add a `GlyphRasterPolicy` layer shared by embedded and SD fonts:
  - source: embedded gray / SD TTF / future EDC bin;
  - quantizer: linear / EDC-threshold / gamma;
  - palette: current / EDC soft / EDC medium / EDC hard;
  - cache key includes font path, size, codepoint, antialias mode, palette mode.
- First experiment can avoid FreeType-on-device by improving stb path:
  1. render coverage at 2x or 3x oversampling;
  2. downsample to 8-bit coverage;
  3. apply EDC dual thresholds;
  4. store packed 4bpp in the existing glyph cache;
  5. draw through the same palette table as embedded Wenkai.
- If this is still visibly worse than EDCBook/new TTF path, then evaluate embedding FreeType or converting SD TTF to EDC-style cached `.vinkfont` on-device/off-device.

Key insight: direct TTF support and excellent antialiasing are compatible only if the runtime recreates the important EDCBook steps — high-quality rasterization, EPD-specific quantization, mapping tables, and caching. The “direct TTF” part is likely a font-source UX improvement, not the whole rendering secret.
