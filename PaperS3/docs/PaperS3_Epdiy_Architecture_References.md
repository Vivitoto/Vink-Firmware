# PaperS3 epdiy Architecture Reference Notes

This is the evidence log for the experimental architecture-level PaperS3 epdiy backend. It records which hardware/display assumptions are now backed by official docs or reference projects, and which still require real PaperS3 validation.

## Sources checked

- M5Stack PaperS3 product page: `https://docs.m5stack.com/en/core/PaperS3`
- M5Stack PaperS3 Chinese product page: `https://docs.m5stack.com/zh_CN/core/PaperS3`
- M5Stack PaperS3 Arduino compilation/upload page: `https://docs.m5stack.com/en/arduino/m5papers3/program`
- PaperS3 schematic PDF: `https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/517/sch_papers3_V1.0.pdf`
  - Local copy: `/home/vito/.openclaw/workspace/references/papers3/sch_papers3_V1.0.pdf`
- ED047TC1 datasheet PDF: `https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/517/C139_ED047TC1_datasheet.pdf`
  - Local copy: `/home/vito/.openclaw/workspace/references/papers3/C139_ED047TC1_datasheet.pdf`
- M5Stack factory firmware: `https://github.com/m5stack/M5PaperS3-UserDemo`
  - `README.md`
  - `dependencies.lock`
  - `repos.json`
  - `main/hal/hal.cpp`
- Local PlatformIO dependency snapshot:
  - `.pio/libdeps/m5papers3/M5GFX/src/M5GFX.cpp`
  - `.pio/libdeps/m5papers3/M5GFX/src/lgfx/v1/platforms/esp32/Bus_EPD.cpp`
  - `.pio/libdeps/m5papers3/M5GFX/src/lgfx/v1/platforms/esp32/Bus_EPD.h`
  - `.pio/libdeps/m5papers3/M5GFX/library.json`
  - `.pio/libdeps/m5papers3/M5Unified/library.json`
- epdiy pinned upstream reference:
  - `https://raw.githubusercontent.com/vroland/epdiy/d84d26ebebd780c4c9d4218d76fbe2727ee42b47/src/displays.c`
  - `https://raw.githubusercontent.com/vroland/epdiy/d84d26ebebd780c4c9d4218d76fbe2727ee42b47/src/board/lilygo_board_s3.c`
  - `https://raw.githubusercontent.com/vroland/epdiy/d84d26ebebd780c4c9d4218d76fbe2727ee42b47/library.json`

## Confirmed from official docs / reference code

### Board and software baseline

- PaperS3 is `SKU:C139`, based on `ESP32-S3R8`.
- Official product page lists `16MB` external flash and `8MB` PSRAM.
- Official development notes require PSRAM enabled and PSRAM set to Octal mode.
- Official factory firmware `dependencies.lock` targets `ESP-IDF v5.3.3` and `esp32s3`.
- Official factory firmware `repos.json` pins:
  - `M5GFX` branch/version `0.2.15`
  - `M5Unified` branch/version `0.2.10`
- Current local Arduino dependency snapshot is newer:
  - `M5Unified` `0.2.11`
  - `M5GFX` `0.2.20`

### Product-page non-display identifiers

- Official USB function string includes `/CDC/MSC/Firmware` via `OTG/CDC/MSC/Firmware Flashing`.
- Official certification string includes `/FCC/MIC` via `CE/FCC/MIC certification`.

### Display model, geometry, and epdiy profile

- Official PaperS3 page identifies the panel as `EPD_ED047TC1`.
- Official page says `4.7"`, `960 x 540` / `960×540`, and 16-level grayscale.
- ED047TC1 datasheet describes `540 x 960` pixels and supports landscape/portrait modes; this matches the same physical panel seen from the opposite orientation.
- epdiy pinned commit `d84d26ebebd780c4c9d4218d76fbe2727ee42b47` defines `ED047TC1` as:
  - `.width = 960`
  - `.height = 540`
  - `.bus_width = 8`
  - `.bus_speed = 20`
  - `.default_waveform = &epdiy_ED047TC1`

### ED047TC1 interface pins

Official product page pin map confirms:

| ED047TC1 signal | ESP32-S3 GPIO |
|---|---:|
| `DB0` | `G6` |
| `DB1` | `G14` |
| `DB2` | `G7` |
| `DB3` | `G12` |
| `DB4` | `G9` |
| `DB5` | `G11` |
| `DB6` | `G8` |
| `DB7` | `G10` |
| `XSTL` | `G13` |
| `XLE` | `G15` |
| `SPV` | `G17` |
| `CKV` | `G18` |
| `PWR` | `G45` |

ED047TC1 datasheet names the panel-side roles:

| Panel pin | Datasheet signal | Role |
|---:|---|---|
| 10 | `XCL` | Clock source driver |
| 11 | `XLE` | Latch enable source driver |
| 12 | `XOE` | Output enable source driver |
| 13 | `XSTL` | Start pulse source driver |
| 14-21 | `D0`..`D7` | Data signal source driver |
| 36 | `CKV` | Clock gate driver |
| 37 | `SPV` | Start pulse gate driver |
| 38 | `MODE 1` | Output mode selection gate driver |

Schematic text extraction confirms the M5Stack board adds two important details missing or ambiguous in the product-page pin table:

- `GPIO16` maps to `EPD_XCL` / `XCL`.
- `GPIO46[strap]` maps to `BST_EN`.
- `GPIO45[strap]` maps to `EPD_PWR`; in the ED047TC1 connector context this net is used for the `XOE`/output-enable side of the panel path.

M5GFX's `board_M5PaperS3` profile matches and clarifies the working driver mapping:

```text
pin_data[0..7] = GPIO6, GPIO14, GPIO7, GPIO12, GPIO9, GPIO11, GPIO8, GPIO10
pin_pwr        = GPIO46
pin_spv        = GPIO17
pin_ckv        = GPIO18
pin_sph        = GPIO13
pin_oe         = GPIO45
pin_le         = GPIO15
pin_cl         = GPIO16
bus_width      = 8
bus_speed      = 16000000
```

M5GFX `Bus_EPD.h` defines the naming semantics:

- `pin_sph` = start pulse source driver (`XSTL`)
- `pin_spv` = start pulse gate driver
- `pin_oe` = output enable source driver (`XOE`)
- `pin_le` = latch enable source driver (`XLE`)
- `pin_cl` = clock source driver (`XCL`)
- `pin_ckv` = clock gate driver

### Power/control sequencing

M5GFX `Bus_EPD::powerControl(true)` sequence for PaperS3 is source-backed:

1. Set `pin_oe` high (`GPIO45`, panel `XOE`/official `PWR` net).
2. Delay ~100 us.
3. Set `pin_pwr` high (`GPIO46`, schematic `BST_EN`).
4. Delay ~100 us.
5. Set `pin_spv` high (`GPIO17`).
6. Delay ~1 ms.

Power off reverses the important parts:

1. Delay ~1 ms.
2. Set `pin_pwr` low (`GPIO46`, `BST_EN`).
3. Delay ~10 us.
4. Set `pin_oe` low (`GPIO45`).
5. Delay ~100 us.
6. Set `pin_spv` low (`GPIO17`).

This is stronger evidence than the product-page `PWR G45` label alone, because it is the actual M5GFX PaperS3 driver path.

### Pixel clock / timing

- epdiy `ED047TC1` declares `.bus_speed = 20` MHz.
- M5GFX PaperS3 driver uses `bus_cfg.bus_speed = 16000000`.
- epdiy LCD backends such as `lilygo_board_s3.c` use:
  - `ckv_high_time = 60`
  - `line_front_porch = 4`
  - `le_high_time = 4`
- Therefore Vink's first PaperS3 epdiy backend should keep M5GFX's board-proven `16 MHz` as the safer bring-up default, while treating `20 MHz` as a later hardware-tuned candidate.

### Factory firmware confirms non-display pins

`M5PaperS3-UserDemo/main/hal/hal.cpp` confirms these direct GPIO uses:

- charge state: `GPIO4`, where `0` means charging and `1` means full in factory comments.
- USB detect: `GPIO5`, where high means USB input.
- battery ADC: `GPIO3` / `ADC_CHANNEL_2`, with factory estimate `raw * 3.5 / 4096 * 2`.
- microSD SPI:
  - MISO `GPIO40`
  - MOSI `GPIO38`
  - SCLK `GPIO39`
  - CS `GPIO47`
- `Hal::powerOff()` uses `M5.Display.sleep()`, `M5.Display.waitDisplay()`, then `M5.Power.powerOff()`.

## Vink code implications

- The epdiy board should keep using the M5GFX-confirmed data/control GPIO order.
- Code comments should distinguish:
  - `GPIO45`: official `PWR`/schematic `EPD_PWR`, used by M5GFX as `pin_oe` / ED047TC1 `XOE`.
  - `GPIO46`: schematic `BST_EN`, used by M5GFX as `pin_pwr`.
- The opt-in epdiy path should remain opt-in until actual PaperS3 display output is confirmed.
- Use `ED047TC1`, 960x540 physical framebuffer, 8-bit parallel bus, epdiy waveform `epdiy_ED047TC1`.
- Use `16 MHz` initially, not epdiy's default `20 MHz`, because M5GFX's PaperS3-specific path is the board-proven reference.
- Keep local smoke/build checks separate from the real-device result label; this path remains `需要真机验证` until tested on hardware.

## EDCBook clean-room cross-check before v0.4.39-rc

After comparing the strict epdiy validation package against the EDCBook reverse notes, the following code-level risks were fixed before publishing:

- Avoid M5GFX `Panel_EPD::waitDisplay()` after epdiy has initialized and taken over the LCD/RMT/GDMA renderer. The first push may wait on M5GFX before takeover; later pushes avoid M5GFX EPD bus calls.
- Keep `front_fb` / `back_fb` coherent after successful `epd_hl_update_area_ex()`. EDCBook starts from a front/back diff; leaving `back_fb` stale would make the next page turn compare against an older page and overdrive unrelated pixels.
- Intersect scroll strip masks with epdiy's dirty-column mask. EDCBook evidence points to dirty-line/dirty-column gating; strip-only masks are too broad and can drive unchanged pixels.
- Fix the dirty crop `max_x` nibble mask to use `max_x` parity instead of `min_x` parity.

These fixes do not prove hardware success; they only reduce avoidable software-level mismatches before real-device testing.

## Still requires real PaperS3 validation

These are not fully closeable from docs/source alone:

1. Whether the epdiy LCD/RMT/GDMA renderer produces a correct image on PaperS3 with direct GPIO control and no M5GFX `Panel_EPD` layer.
2. Whether M5GFX's `GPIO45`/`GPIO46` power sequence is sufficient when called from the epdiy backend instead of M5GFX's own transaction model.
3. Whether the practical pixel clock should stay `16 MHz` or can move to epdiy's `20 MHz` without line artifacts, tearing, or worse ghosting.
4. Whether the ED047TC1 waveform bundled in the pinned epdiy commit gives the desired PaperS3 grayscale and partial-update quality.
5. Whether VCOM/temperature assumptions are acceptable; the current bring-up has no PaperS3-specific VCOM measurement/control path and uses a fixed ambient temperature callback.
6. Whether Vink's portrait-to-landscape framebuffer transform matches the physical panel orientation on the user's unit.
7. Whether `epd_draw_base_scroll()` visually matches EDCBook-style page turning once the backend is active.

## Not a blocker / resolved by references

- The DB0..DB7 GPIO order is no longer guesswork; official docs and M5GFX agree.
- `XSTL`, `XLE`, `SPV`, `CKV`, and `XCL` are no longer guesswork; official docs/datasheet/schematic/M5GFX cover them.
- The apparent `PWR G45` vs M5GFX `pin_pwr GPIO46` mismatch is explainable: the product pin table labels `G45` as `PWR`, while the schematic and M5GFX show `GPIO46` as boost enable (`BST_EN`) and `GPIO45` as the panel output-enable/`EPD_PWR` net.
