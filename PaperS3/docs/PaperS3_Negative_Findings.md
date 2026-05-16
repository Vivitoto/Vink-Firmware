# PaperS3 Negative Findings / 不再重复踩坑清单

本文件记录 Vink PaperS3 显示、翻页、残影探索中已经证明不合适或风险很高的思路。后续改动前先查这里；如果再次触碰其中某项，必须写明为什么这次条件不同、怎么验证。

## 原则

- 区分两条链路：**翻页动画连续性** 与 **单页刷新残影控制**。
- 不把“周期全刷”当成单页残影的主解法；周期全刷只能作为长读兜底。
- 不把“缩小条带宽度”当成动画连续性的主解法；它只能微调跳动/速度权衡。
- 不用未经验证的驱动强转或临时库补丁冒险上设备。

## 已踩/应避免的方向

### 1. 用 `clipRect + pushSprite + waitDisplay()` 逐条带动画当最终方案

**结论：不适合作为最终方案。**

原因：每个条带都完整等待一次 EPD 波形结束后才开始下一条带，视觉天然是一格一格跳，不是真正连续扫线。条带变窄会让跳动细一点，但总耗时增加；条带变宽会快一点但更不连续。

可保留用途：安全 fallback、驱动级 scroll 失败时回退。

### 2. 只把条带从 60px 改成 36/45px

**结论：只能改善表象，不能根治。**

原因：当前上层调度仍然是“刷完一条再刷下一条”。根因是调度层级太高，而不是单纯 strip width。

### 3. 把快速动画强行换成 `epd_text` / GL16

**结论：不符合用户目标。**

原因：GL16/text 更干净但可见相位多，动画会变慢、波前变宽，用户已经明确“快速模式速度满意”，所以不能用牺牲速度来换连续性。

### 4. 用 `epd_fastest` / DU 类模式解决动画速度，同时期待单页干净

**结论：速度可用，但残影不够。**

原因：DU/fastest 对 old black → new white、灰阶区域的补偿弱；正文旧笔画和 footer/章节灰区会残留。它可以作为短波形素材，但不能单独承担单页清晰度。

### 5. 用“第 N 页 quality/full refresh”解释或解决单页残影

**结论：方向不够。**

原因：用户要的是“单页翻完就尽量干净”，不是等 5/10/20 页后统一清。周期 quality/full 只能清累计残影，不能解决每次翻页后的即时残留。

正确方向：old/new framebuffer 差异驱动的每页短补偿，重点处理旧黑字消失区、灰阶 footer/章节区域和高差异区域。

### 6. 清晰模式只是设置 `fullEvery = 1`，但不做 old/new 差异补偿

**结论：可能更干净，但不等于极致。**

原因：通用 GC16/quality 刷新不针对“旧页到新页”的转换矩阵；可能慢、闪，但仍不一定对旧字消失/灰阶脏边做到最优。

### 7. 在 `.pio/libdeps` 里临时改 M5GFX 当长期方案

**结论：不可作为长期工程方案。**

原因：PlatformIO 重新拉库/清理依赖后会丢 patch。驱动级 page-turn scroll 如果要继续，必须 vendor/fork M5GFX 或用可重复应用的补丁流程，并由 smoke/build 验证 patch 实际生效。

### 8. 盲目把当前 PaperS3 显示路径强转成 `Panel_EPDiy` 或直接调用 epdiy highlevel

**结论：高风险，禁止盲做。**

原因：当前运行路径由 M5GFX/M5Unified 管理，历史研究也指出错误强转/绕过驱动契约可能导致重启、内存/rotation/packed framebuffer 不一致。epdiy 的 diff/dirty-mask 思路可参考，但不能未经验证直接接入活跃 panel。

### 9. 把 ReadPaper/M5ReadPaper 当成 EDCBook/梦西游源码权威

**结论：错误来源假设。**

原因：用户已经纠正两者不是同一项目。M5ReadPaper 只能作为 PaperS3 阅读器通用参考；EDCBook 行为必须来自 EDCBook bin 逆向、用户真机反馈、或真实 EDCBook 来源。

## 当前优先探索方向

1. **动画连续性**：底层 page-turn scroll / wavefront scheduling。先整页写入 framebuffer，再由驱动层按短私有 LUT 和 strip/phase 推进，而不是上层逐条带 wait。
2. **单页残影极限控制**：old/new framebuffer diff + per-turn compensation。按转换类型补偿：旧黑→白、旧黑→灰、新黑出现、灰阶 footer/章节区域。
3. **周期 full/quality**：只作为长读兜底，不作为单页残影主方案。

## 每次新实验必须记录

- 方案名称和代码路径
- 目标：动画连续性 / 单页残影 / 长读累计残影 / 稳定性
- 为什么不是重复上述 dead end
- 预期副作用：速度、闪烁、字体变淡、灰阶变脏、触摸卡顿、重启风险
- 本地验证命令和结果
- 真机结果：照片/视频反馈、模式、页数、是否可接受
- 结论：继续 / 调参 / 废弃

### 10. 把 AA 问题和翻页补偿问题绑在一起判断

**结论：错误分层，后续禁止混测后直接归因。**

2026-05-15 真机反馈确认：即使关闭抗锯齿、只走纯黑白渲染，使用当前实验翻页路径后仍出现“墨迹毛糙、不均匀”。因此该问题不能归因于 AA palette / TTF coverage 量化。

正确分层：

- **AA / 抗锯齿**：字体栅格化、coverage 量化、4bpp 灰阶映射、framebuffer 内容生成。
- **翻页补偿 / 残影控制**：old framebuffer → new framebuffer 的 EPD 物理刷新、LUT/waveform、eraser/compensation。

两者会在视觉上互相放大：AA 产生更多灰阶边缘，坏 waveform 更容易把灰阶刷脏；但代码设计和实验归因必须独立。后续必须先用 AA 关闭、纯黑白页面验证翻页路径墨迹均匀，再测试 AA 效果。

### 11. 用 2-4 phase 私有短 LUT 直接承担正文页翻页刷新

**结论：当前实验失败，不能作为默认稳定路径。**

2026-05-15 真机反馈：`快速 + 残影均衡/强 + AA均衡/锐利` 以及 AA 关闭的纯黑白测试，均出现翻页后字体墨迹非常不均衡、毛糙。轻补偿残影严重且仍毛糙。

原因判断：私有 page-turn LUT 只有 2-4 个 phase，不足以让 PaperS3 墨水粒子充分稳定，尤其正文黑白边缘和灰阶区域会出现不均匀沉积。把 `scroll_waveform=false` 切回标准文本 LUT 可能改善墨迹，但会让 `scroll_compensation`/轻均衡强补偿失效；因此不能一边用标准 LUT，一边继续宣称“残影补偿”生效。

后续方向：

- 稳定路径先使用标准文本/quality LUT，目标是 AA 关闭时翻页后墨迹均匀。
- 补偿实验必须重新设计为标准 LUT 基础上的 old/new-aware eraser/补偿，或单独实验模式；有效前不要在 UI 中暴露“残影补偿”档位。

### 12. 只把 EDCBook offset table 映射成多段标准 text LUT strip 刷新

**结论：改善了字体清晰基础，但没有达到 EDCBook 式翻页观感。**

2026-05-15 真机反馈，版本：`v0.4.35-rc` / `Vink-PaperS3-v0.4.35-rc-edc-offset-diff-full-16MB.bin`。

正向结果：

- `AA 均衡`效果不错。
- 翻页之后字体也相对清晰，说明 AA policy 与标准 text LUT baseline 没有再明显破坏字形。

问题：

- 快速 / 均衡 / 清晰所有翻页档位都像“一块板刷刷过去”。
- 整页刷完后，还会沿翻页方向再整页闪动一下。

初步判断：

- 当前 `effectSteps -> offsets[]` 只是改变 strip 分段数量；每段仍是一次完整标准 text LUT 区域刷新，所以视觉上仍像板刷，而不是 EDCBook 的 phase/progression 级滚动。
- 后闪可能来自 scroll 分支最后的 `while (remain) { remain = run_cycle(); }`、标准 text LUT 收尾相位，或某个过大的 update rect 在 strip 循环后被排队执行。
- 不能把“offset table 分段”误认为已经复刻 EDCBook 的 `epd_draw_base_scroll`。EDCBook 证据更像是在 render/waveform pipeline 内使用 offsets/progression，而不是上层/中层对区域逐段跑完整 waveform。

后续恢复时优先排查：

- `Panel_EPD::task_update()` scroll 分支每个 strip 与最终 remain drain 的实际 update rect。
- `prepare_update(strip)` 里 `raw0/raw1/d0..d3` 是否只限 strip，是否在尾段扩大到整页。
- 是否需要记录每次 `run_cycle()` 的 `upd.x/w`、phase/step、mode，确认后闪来源。
- 如果继续追 EDCBook，应考虑 phase/progression 层级，而不是继续只调 strip 数量。

### 13. 在 M5GFX Panel_EPD clean-room scroll 中使用明显 per-row source offset

**结论：当前 M5GFX 实验路径会造成真机可见斜线，不能作为默认路径。**

2026-05-16 真机反馈，版本：`v0.4.38-rc`。现象：翻页不是直的一条，而是斜的；新页模糊，触屏响应也被长刷新窗口拖慢。

原因判断：`build_edcbook_row_offset()` 把每个输出行映射到不同的 source bucket，顶部先行、底部滞后，视觉上自然形成左上到右下的斜线波前。这不是 EDCBook 证据直接证明的安全做法，而是 M5GFX clean-room 近似过度。

处理：恢复/修复时，M5GFX recovery build 先让 `build_edcbook_row_offset()` 返回 `0`，保证波前竖直；未来若继续探索 row offset，必须在 epdiy phase/progression 层级做小幅验证，不能在 M5GFX source-bucket 层直接拉大行偏移。

### 14. epdiy backend 与 M5GFX Panel_EPD 同时持有 ED047TC1 总线

**结论：禁止共存。**

`M5.begin()` 会为 PaperS3 创建 `Panel_EPD`、`Bus_EPD`、esp_lcd i80 bus、Panel_EPD worker task；epdiy backend 再初始化 `epd_lcd_init()` 会再次配置同一组 ED047TC1 GPIO/LCD/RMT/GDMA 资源。两个驱动共持同一总线时，strict epdiy 包很可能表现为开机后无显示/保留旧屏。

后续要求：如果使用 epdiy backend，必须先显式停止 M5GFX Panel_EPD worker、删除 update queue、释放 `esp_lcd_panel_io` 与 `i80_bus`，再调用 `epd_init()`。这不是回退问题，是总线所有权问题。

### 15. EDCBook 不是完全舍弃 M5Unified，而是分离平台初始化与 EPD 所有权

**结论：M5Unified 可以用于平台服务，但 M5GFX 显示输出不能再参与 EPD 刷新。**

2026-05-16 进一步审计 EDCBook v2.0.0：二进制中有 `[Boot] after M5.begin t=%ums`，并且配置字节符合 `clear_display=false, output_power=true, pmic_button=true, internal_imu=true` 的 `M5Unified::config_t` 布局。因此 EDCBook 很可能仍调用 `M5.begin(cfg)` 做板级/I2C/触摸/按钮/电源辅助初始化。

但显示更新证据全部指向 epdiy：`epd_hl_init`、`epd_hl_get_framebuffer`、`epd_hl_update_area_ex`、`epd_lcd_init`、`epd_renderer_init`、`epd_draw_base_scroll`、`/lib/epdiy/src/output_lcd/lcd_driver.c`。没有找到 `Panel_EPD` / `Bus_EPD` 作为活跃刷新路径的证据。

Vink 正确边界：

- 允许 `M5.begin(clear_display=false)` 负责平台服务初始化。
- epdiy 初始化前必须隔离/释放 M5GFX 的 PaperS3 `Panel_EPD` 总线占用（如果 M5Unified 构造了它）。
- epdiy ready 后禁止再调用 `M5.Display.*` 物理刷新路径。
- UI 只生成 framebuffer；EPD 刷新、diff、scroll/page-turn 全部进入 epdiy backend。
