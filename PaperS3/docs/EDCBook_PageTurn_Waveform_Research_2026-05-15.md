# EDCBook page-turn waveform research — 2026-05-15

Context: Vink v0.4.30-rc currently uses M5GFX `Panel_EPD::displayScroll()` with `epd_text` / `kNormalRefresh` for animated page turns. Real-device feedback shows the visible wavefront is still a broad eraser band, not EDCBook's thin fast line.

## Key correction

EDCBook's good single-page-turn ghost control is not explained by periodic full refresh. The observed quality is per-turn: once the moving line passes, the new page is already relatively clean.

## EDCBook reverse evidence

From `edcbook_reverse/page_turn_deep_dive.md` and objdump:

- EDCBook adds a custom `epd_draw_base_scroll` in IRAM (`0x40376cfc`).
- `epd_hl_update_area_ex` computes a front/back framebuffer difference first (`epd_difference_image_base` family).
- Custom mode bit `0x20` selects the scroll path.
- The scroll renderer stores custom fields in render context:
  - `scroll_enabled` at `+208`
  - count/last index at `+212`
  - `scroll_mode_frames` at `+216`
  - direction flags at `+220`
  - offset/progression table at `+224+`
- `scroll_mode_frames` is derived as `2` or `3` in the custom draw path, not the full 12/15 visible frames of M5GFX text/quality LUTs.
- Two table builders exist:
  - phase progression table: `0..15`, capped by `effectSteps <= 24`.
  - width table: `step = ceil(width / (n*16))*16`, with `n = clamp(effectSteps/2, 1, 24)`.

## Why Vink's current implementation looks wide

M5GFX Panel_EPD LUT row counts:

- `quality`: 15 visible rows + 16 noop tail = 31 cycles before end.
- `text`: 12 visible rows + 19 noop tail = 31 cycles before end.
- `fast`: 8 visible rows + 1 noop tail.
- `fastest`: 5 visible rows + 1 noop tail.
- `eraser`: 2 visible rows + 1 noop tail.

The current scroll loop starts one new strip per global scan cycle. With `epd_text`, roughly 12 visible waveform phases are simultaneously in flight. Therefore visual band width is approximately:

```text
visible_width ≈ visible_waveform_frames * strip_width
```

Examples:

- 32 px strip × 12 visible text phases ≈ 384 px broad band.
- 16 px strip × 12 visible text phases ≈ 192 px broad band.

So reducing strip width alone cannot recreate EDCBook. It makes the band narrower but slower, and still not a thin line.

## Better model for EDCBook

EDCBook likely uses a dedicated page-turn waveform, not stock text/quality:

```text
front/back diff
+ custom short scroll waveform (2–3 active frames)
+ phase/offset progression inside render pipeline
+ final target settle
```

Single-turn ghost control likely comes from a short compensation waveform applied only to changed pixels, not from full text eraser every strip.

## Clean-room implementation direction for Vink

1. Keep the high-level flow:
   - render next page into Panel_EPD framebuffer with `AutoDisplay(false)`
   - call `Panel_EPD::displayScroll()`

2. Replace scroll internals:
   - Add private scroll/page-turn LUT(s) in `Panel_EPD.cpp`, not exposed as a public `epd_mode_t` unless necessary.
   - Do not use `epd_text` for normal animated turns.
   - Use a short LUT with 2–4 visible rows: a small old-state compensation impulse plus target drive.
   - Keep quality/text full refresh only for explicit frequency cleanup.

3. Expected experiments:
   - `lut_page_turn_a`: 3 active frames + 1 noop + end; target-directed, minimal pre-erase.
   - `lut_page_turn_b`: 4 active frames with one mild opposite/precharge frame for gray/white transitions.
   - `lut_page_turn_c`: hybrid that drives white-side transitions slightly harder than black-side transitions, because user reports gray footer/chapters blur most.

4. Success criteria:
   - Moving wavefront looks like a line, not an eraser block.
   - Single page turn leaves body text/gray footer clear enough without waiting for periodic full refresh.
   - Page turn remains faster than current `epd_text` 16/32 px variants.

## Important caution

Do not keep tuning `strip_width` as the main fix. The current broad band is mainly a waveform-duration problem. The right fix is a dedicated short compensated page-turn waveform plus current wavefront scheduling.
