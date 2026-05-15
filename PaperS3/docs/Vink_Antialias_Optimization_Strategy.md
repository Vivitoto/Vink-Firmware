# Vink PaperS3 Antialias Optimization Strategy

Date: 2026-05-15
Context: This is the next optimization track after `v0.4.34-rc-oneburn-expsuite`. Do not mix it with page-turn/ghosting changes unless explicitly testing interaction.

Goal: explore EDCBook-style antialiasing quality for both embedded reader fonts and SD-card TTF fonts, with as few reflashes as possible.

## Core hypothesis

EDCBook's good text rendering is not tied to one storage format. The real reusable pipeline is:

```text
font source: built-in gray font / generated bin / SD TTF
→ high-quality glyph coverage
→ EPD-specific quantization
→ mode-dependent grayscale palette/threshold mapping
→ cached 4bpp glyphs
→ stable baseline/metrics
→ reader page framebuffer
```

Therefore Vink should not optimize only `font.bin` or only `TTF`. It should introduce a source-independent `GlyphRasterPolicy` so all font sources can share the same EPD-friendly treatment.

## Non-goals / avoid

- Do not regress the `v0.4.34-rc` page-turn/ghosting experiment suite.
- Do not introduce FreeType-on-device as the first step; app size, PSRAM, latency, and watchdog risk are high.
- Do not use simple linear `coverage >> 4` as the only TTF path.
- Do not make users flash one firmware per font/AA parameter.
- Do not treat SD TTF as a second-class path with permanently worse rendering.

## Phase 1 — Source-independent grayscale policy

Create a small render policy layer used by both embedded grayscale fonts and SD TTF glyphs.

Proposed enum:

```cpp
enum class ReaderAntialiasProfile : uint8_t {
    Current = 0,      // existing Vink mapping, safe fallback
    EdcSoft = 1,      // preserve more gray, smoother edges
    EdcBalanced = 2,  // weak edge -> white, strong core -> black
    EdcCrisp = 3,     // stronger threshold for fast refresh / low ghosting
};
```

Runtime controls:

- Device settings page: `抗锯齿档位` / `字体清晰度`.
- WebUI config: same select field.
- NVS key: e.g. `aaprof`.

This follows the one-burn rule: one firmware can test several AA profiles.

## Phase 2 — EDC-style quantization for SD TTF

Current SD TTF path:

```text
stbtt_GetCodepointBitmap()
→ 8bpp coverage
→ (val + 8) >> 4
→ draw RGB565 gray pixel
```

This is too direct. Replace the conversion stage with policy-driven quantization:

```cpp
uint8_t quantizeCoverageToNibble(uint8_t coverage, ReaderAntialiasProfile p) {
    // Vink nibble semantics: 0 = white/skip, 15 = black.
    switch (p) {
        case Current:
            return (coverage + 8) >> 4;
        case EdcSoft:
            // remove only very weak halo; keep more gray transition
            if (coverage < 24) return 0;
            if (coverage > 232) return 15;
            return curveOrLinearMiddle(coverage);
        case EdcBalanced:
            // close to EDCBook generator defaults, adapted to Vink semantics
            if (coverage < 32) return 0;
            if (coverage > 223) return 15;
            return mapMiddleWithGamma(coverage, 32, 223);
        case EdcCrisp:
            // more aggressive for fast refresh / lower gray ghosting
            if (coverage < 48) return 0;
            if (coverage > 208) return 15;
            return mapMiddleWithHarderGamma(coverage, 48, 208);
    }
}
```

Important: cache the **post-quantized 4bpp glyph** for the selected AA profile. The cache key must include:

```text
font path/hash + pixel size + codepoint + antialias profile
```

Otherwise switching profiles will reuse wrong glyph data.

## Phase 3 — Palette/mapping table after quantization

Separate two concepts:

1. coverage quantization: coverage -> nibble 0..15
2. display palette: nibble -> actual EPD/RGB565 gray

Current Vink has one main `kInkBoost`. Add selectable EDC-style tables, adapted from reverse evidence:

```text
Current Vink boost:
0,3,5,6,7,8,9,10,10,11,12,13,13,14,15,15

EDC identity adapted:
0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15

EDC soft threshold adapted:
0,0,1,2,3,4,5,6,7,8,9,9,15,15,15,15

EDC hard threshold adapted:
0,0,0,0,0,0,6,6,6,6,6,15,15,15,15,15
```

Because EDCBook semantic is `0=black,15=white` while Vink uses `0=white,15=black` in drawing, tables must be verified with visual test sheets rather than blindly copied.

## Phase 4 — Embedded Wenkai / generated font experiment

Vink's embedded Wenkai path already uses offline FreeType generation, so it is closer to EDCBook than SD TTF. The likely gap is quantization and palette.

Add an EDC quantization mode to `tools/generate_gray_font.py`:

```bash
python3 tools/generate_gray_font.py \
  --input LXGWWenKai.ttf \
  --output wenkai32_edc_balanced.fnt \
  --size 32 \
  --quant edc-balanced \
  --white-threshold 32 \
  --black-threshold 223
```

Do not immediately replace the default body font. First generate comparison sheets and, if needed, include two embedded variants only temporarily:

- current Wenkai32
- EDC-threshold Wenkai32

If flash size becomes an issue, compare off-device first and only embed the winning variant.

## Phase 5 — Optional SD TTF oversampling

If EDC quantization alone is not enough, improve the rasterizer stage before considering FreeType.

Experiment order:

1. Current stb 1x + current quantization.
2. stb 1x + EDC quantization/palette.
3. stb 2x oversample + box downsample + EDC quantization.
4. stb 3x oversample only if 2x clearly helps and performance is acceptable.

Implementation sketch:

```text
scale_hi = scale * oversample
render high-res glyph with stb
box-downsample coverage to target bitmap
apply EDC quantization
action: store 4bpp glyph in cache
```

Caveats:

- Need careful bearing/y-offset conversion from high-res to target coordinates.
- Larger temp buffers can stress heap; allocate once or use PSRAM.
- First-render latency may be high; log glyph render time.

## Phase 6 — FreeType-on-device only if needed

Only evaluate FreeType-on-device if the above still cannot approach EDCBook/new direct-TTF quality.

Risks:

- binary size increase;
- PSRAM/heap pressure;
- glyph render latency;
- watchdog risk during first page render;
- build/dependency complexity.

If attempted, do not draw from FreeType directly per glyph on every page paint. Use:

```text
FreeType raster -> EDC quantization -> 4bpp glyph cache -> page blit
```

## Diagnostics and test sheet

Add a fixed glyph comparison page or generated PNG/contact sheet using:

```text
一 的 我 汉 爱 ， 。 “ ” A a g y 0 8 9
```

For each AA profile record:

- weak edge halo;
- horizontal stroke breakage;
- punctuation blur;
- Latin descender alignment;
- gray edge ghosting after page turn;
- body text darkness;
- first-render latency and cache hits.

Runtime logs should include:

```text
font source = embedded / sd-ttf
AA profile = current / edc-soft / edc-balanced / edc-crisp
raster = stb-1x / stb-2x / future-freetype
glyph cache hit/miss count
slowest glyph render ms
```

## Recommended next RC shape

If implemented after `v0.4.34-rc`, make it another one-burn experiment suite, for example:

```text
v0.4.35-rc-aa-expsuite
```

Included toggles:

- existing page-turn profile;
- existing ghosting compensation;
- new antialias profile: current / soft / balanced / crisp;
- optional hidden/dev toggle for TTF raster: stb-1x / stb-2x.

Default should remain conservative:

```text
AA profile = Current or EdcBalanced only after local visual preview looks safe
TTF raster = stb-1x until memory/performance is measured
```

## Success criteria

- Body text edges are smoother than current Vink without becoming pale.
- Thin horizontal strokes do not break.
- Punctuation stays sharp.
- Fast page-turn does not turn gray edges into obvious residue.
- SD TTF path becomes visually close to embedded gray font path.
- User can compare profiles after one flash, not by repeatedly burning firmware.

## v0.4.35 implementation note

Implemented as a one-burn AA experiment suite on top of v0.4.34:

- Added `ReaderAntialiasProfile`: 当前 / 柔和 / 均衡 / 锐利.
- Stored profile in `renderOpt1` slot 5 and exposed it in device reader settings + WebUI.
- Embedded Wenkai path now selects profile-specific palette tables in `ReaderTextRenderer::pixelColorForNibble()`.
- SD TTF path now applies EDCBook-style dual-threshold quantization before drawing:
  - 当前: previous linear `(coverage + 8) >> 4` fallback.
  - 柔和: weak halo cutoff 24, black core 232.
  - 均衡: cutoff 32 / 223.
  - 锐利: cutoff 48 / 208.
- TTF profile is applied at draw-time from the 8bpp glyph cache, so switching profile does not require clearing the glyph cache. Future optimization can cache post-quantized 4bpp glyphs if performance becomes an issue.
