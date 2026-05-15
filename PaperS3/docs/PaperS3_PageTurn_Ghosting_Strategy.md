# PaperS3 Page-turn Continuity & Single-page Ghosting Strategy

Date: 2026-05-15
Scope: 找思路，不直接改固件主逻辑。目标是拆开并解决两个独立问题：

1. 快速模式翻页速度可接受，但动画不连贯。要求：**保留快速速度感，提高连续性**。
2. 单页刷新后残留明显，不管哪个模式。要求：**每一页翻完就尽量干净，不靠等第 N 页全刷**。

Before implementing, check `docs/PaperS3_Negative_Findings.md` to avoid repeating rejected paths.

## Current failure model

### A. 动画不连贯

当前主仓路径是 `DisplayService::pushShutterAnimation()`：

```text
for each strip:
  setClipRect(strip)
  pushSprite(full page with clip)
  waitDisplay()
```

这会让每个条带完整跑完一次波形后才启动下一条带，所以视觉是离散跳块。条带变窄只是让块更小，同时总耗时上升；条带变宽会保速度但更跳。

**关键结论：动画连续性不是 strip width 问题，而是调度层级问题。**

### B. 单页残影明显

当前模式使用通用 EPD 模式：

- `fastest/fast`：速度快，但 old black → new white 清除弱，灰阶残留明显。
- `text/quality`：更干净但慢、闪、波形长；仍不是针对“旧页→新页”的专用补偿。
- 周期 quality/full：只能清长期累计，不能让当前这一页翻完立刻干净。

**关键结论：单页残影需要 old/new 差异驱动的 per-turn compensation，不是单纯换模式或加周期全刷。**

## Track 1 — 保速提高动画连续性

### Recommended direction

把翻页动画从 DisplayService 上层条带循环下沉到 M5GFX `Panel_EPD` worker 层：

```text
1. DisplayService 渲染下一页到 snapshot canvas
2. M5.Display.setAutoDisplay(false)
3. canvas->pushSprite() 只写入 Panel_EPD framebuffer，不立即物理刷新
4. 调用 Panel_EPD::displayScroll(...)
5. driver worker 内部按 strip/phase 推进 EPD 扫线
```

这样每个 strip 不再是“完整刷完再下一个”，而是在底层扫描周期里推进，视觉可以从“块状跳变”变成“连续波前”。

### Implementation shape

- 保留当前 `clipRect + pushSprite + waitDisplay()` 作为 fallback。
- 正式化旧分支的 `Panel_EPD::displayScroll()`，不要依赖临时 `.pio/libdeps` 修改：
  - 方案 A：vendor/fork M5GFX 到项目 `lib/`。
  - 方案 B：保留 patch 文件并在 build/smoke 中验证 patch 已应用。
- 在 `Panel_EPD` 内部使用 private page-turn LUT，不暴露成公共 `epd_mode_t`。
- 快速模式优先使用 2–3 active frame 的短 LUT，而不是 `epd_text` / `epd_quality`。

### First experiment set

| ID | Scheduler | LUT | Strip | Goal |
|---|---|---|---:|---|
| A0 | current clip+wait | current fast | 36/45 | baseline only |
| B0 | Panel_EPD displayScroll | private 3-frame | 180 | 接近快速速度，检查是否仍跳 |
| B1 | Panel_EPD displayScroll | private 3-frame | 128 | 速度/连续性中间点 |
| B2 | Panel_EPD displayScroll | private 3-frame | 108 | 旧分支实用默认候选 |
| B3 | Panel_EPD displayScroll | private 2-frame | 128/108 | 更短波前，测残影是否变坏 |

Success criteria:

- 翻页总耗时接近用户满意的快速模式。
- 肉眼看到连续扫线，而不是一格一格跳。
- 不出现 tap during refresh 重启、`waitDisplay()` 卡死、局部错位。

## Track 2 — 单页残影极限控制

### Recommended direction

每次翻页都利用 old/new framebuffer 差异做短补偿：

```text
old_fb = 上一页已显示的目标状态
new_fb = 下一页 framebuffer
transition = classify(old_fb, new_fb)
page-turn LUT / dirty-tail settle 根据 transition 类型补偿
```

重点不是整屏闪，而是只对“容易残留”的转换增加短补偿。

### Transition classes

按 4bpp 灰阶大致分组：

- `old dark -> new white/near-white`：最容易留下旧黑字，优先清白侧。
- `old dark -> new gray`：灰阶 footer / 章节区容易变脏，需要温和清，不要洗淡。
- `old white -> new dark`：新字成形，要求快且边缘清楚。
- `old gray -> new white/gray`：AA 边缘和页脚，避免发糊。
- `unchanged/low-diff`：尽量不驱动，避免把干净区域越刷越脏。

### Two-stage per-turn compensation

1. **In-sweep compensated LUT**
   - 2–4 active frames。
   - 不用通用 fast/DU；短 LUT 里加入轻微 old-state compensation。
   - 不做大面积前置白擦，避免出现“黑板擦”宽带和洗淡正文。

2. **Dirty-tail local settle**
   - 扫线后只对高风险区域追加 1–2 个短 frame：
     - 旧黑字消失 mask；
     - footer/章节灰阶区域；
     - 高 diff 区域。
   - 不全屏 quality flash。

### First experiment set

| ID | In-sweep LUT | Tail settle | Expected |
|---|---|---|---|
| C0 | 3-frame target-directed | none | 速度/连续性基线 |
| C1 | 3-frame white-side stronger | none | 旧黑字残留下降 |
| C2 | 4-frame light precharge + target | none | 单页干净度上限候选 |
| D0 | best C | old-dark→white mask, 1 frame | 局部清旧字，不全屏闪 |
| D1 | best C | old-dark→white + gray zones, 1 frame | 页脚/章节灰残影优化 |
| D2 | best C | same masks, 2 frames | 极限清影，评估速度代价 |

Success criteria:

- 单页翻完后的旧文字残留明显降低。
- footer/章节灰色不糊、不发黑、不被洗淡。
- 不靠第 N 页 quality 才变干净。
- 保留长读周期 cleanup 作为兜底，但不是主要解释。

## Required instrumentation before serious tuning

每个实验包需要能在串口/系统日志记录：

- page-turn scheduler: fallback / displayScroll
- LUT variant: A/B/C/D
- strip width / strip count
- active frame count
- tail settle enabled / mask type / frame count
- total refresh duration ms
- mode: speed / balanced / clear
- whether fallback path was used

建议新增一个隐藏/诊断开关，允许同一个固件切换：

- scroll profile: fast / balanced / clean
- LUT variant: A/B/C
- tail settle: off / old-black-white / gray-aware

这样真机测试不用每个参数重刷固件。

## Guardrails

- Do not claim success without real-device validation.
- Do not patch `.pio/libdeps` as the only source of truth.
- Do not cast active panel to `Panel_EPDiy`.
- Do not solve single-page ghosting with only `fullEvery` / periodic refresh.
- Do not regress boot path, touch suppression, side-key shutdown/lock behavior.
- Keep fallback path available until displayScroll is proven stable.

## Immediate next implementation recommendation

Build a local RC experiment in this order:

1. Restore/port `Panel_EPD::displayScroll()` cleanly and persistently.
2. Add one private 3-frame page-turn LUT.
3. Add runtime logs and a fallback flag.
4. Use strip widths 180/128/108 as selectable profiles.
5. Only after continuity is proven, add old/new diff masks and dirty-tail settle.

Rationale: if the scheduler remains upper-layer clip+wait, residual compensation tuning is confounded by discontinuous animation. Fix the scheduling layer first, then tune single-page ghost control.

## v0.4.34 one-burn experiment suite adjustment

User does not have time to flash many binaries. Therefore the implementation should prefer one firmware image with runtime-selectable profiles.

Runtime knobs in the one-burn suite:

- 翻页档位: 清晰 / 均衡 / 快速 controls strip width (64 / 108 / 180 px).
- 残影补偿: 轻 / 均衡 / 强 controls private page-turn LUT selection inside `Panel_EPD`.
  - 轻: shortest private LUT, fastest and least cleanup.
  - 均衡: default AA-preserving LUT.
  - 强: old-dark -> new near-white transitions use a stronger tail-heavy LUT; new dark glyph targets stay on balanced path to avoid washed text.

This is still not the final “perfect” residual solution, but it moves in the correct direction: old/new-aware in-sweep compensation within one flashable RC, not periodic full refresh and not per-binary parameter roulette.
