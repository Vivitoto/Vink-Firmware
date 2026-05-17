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


### 16. 遇到 scroll 翻页观感差就直接回退到普通整页刷新

**结论：方向错误。**

2026-05-17 复盘：`v0.4.40-rc` 真机反馈显示 scroll 翻页仍有从下到上一行一行扫、触摸被 display busy 拖慢、TAB 像全刷的问题。我随后做了 `v0.4.41/v0.4.42` 式回稳处理：禁用默认 scroll、回到直接整页刷新。这被用户明确否定：应该沿着动画/调度路线继续优化，而不是回退。

正确处理：保留 scroll/page-turn 路径为默认主线，优化 band 数、active frame 数、UI waveform、busy 窗口和输入排队；只有在显式诊断编译开关下才允许 no-scroll fallback。

### 17. scroll 默认使用过多窄 band 导致多次全屏 scan pass

**结论：需要收敛 band 数，而不是继续加密 strip。**

`v0.4.40-rc` 默认均衡档使用 `effectSteps=24`（约 12 个 band，再叠加 active frames），会产生太多物理 scan pass；在 PaperS3 上容易被感知为从下到上一行一行刷，并显著拉长 `g_inDisplayPush` 时间。

处理：`v0.4.43-rc` 把默认翻页档位改为 Fast，并把 effectSteps 收敛为 Fast=6、Balanced=12、Clean=24。目标是保留 scroll 路线，但减少可见 scan pass 和触摸等待；后续再在这个更短窗口上调残影补偿。

### 18. 把底层 Panel_EPD row/bucket 扫描直接当作“滑动动画”

**结论：抽象层级错误。**

用户明确指出：翻页应该是一整页滑过去，而不是从下到上一行一行扫。`v0.4.40/v0.4.43` 的 M5GFX scroll 仍然把 Panel_EPD worker 的 scanline / dirty-bucket 输出过程暴露成可见动画，导致肉眼看到硬件扫描，而不是页面滑动。

正确方向：动画层必须先合成完整 old/new 中间帧（整页画布），每个 phase 都是完整 540×960 目标图像，再交给 EPD 做物理刷新。底层 scanline 只能是传输细节，不能成为视觉动画本身。

处理：`v0.4.44-rc` 在 `DisplayService` 中新增 full-frame page-slide compositor：保留上一帧整页 framebuffer，按 Fast/Balanced/Clean 合成 3/4/5 个完整滑动帧，旧页整体移出、新页整体移入。无上一帧或内存不足时才 fallback 到旧 Panel_EPD scroll，并记录日志。

### 19. epdiy `epd_draw_base_scroll()` 只是在 strip 外层循环完整 waveform

**结论：仍然不够深，不能冒充 EDCBook renderer-level scroll。**

2026-05-17 继续复盘：当前 epdiy backend 虽然已经有 `epd_hl_update_area_ex()` / `epd_draw_base_scroll()`，但实现方式是按 `scroll_offsets[]` 把区域拆成 strip，然后每个 strip 单独调用完整 `lcd_do_update_frames()` 跑完整 waveform。这个层级仍然太高，本质接近“strip A 完整刷完，再 strip B 完整刷完”，真机上容易表现成板刷/后闪，和 2026-05-15 的负面反馈一致。

正确方向：scroll progression 应进入 LCD scanline renderer。一次 scroll update 应该是一组扩展的物理 scan-cycle：`total_frames = waveform_frames + scroll_count - 1`；每个横向 bucket 在同一物理帧里根据 wavefront 位置选择自己的 waveform phase。这样每个 bucket 仍收到完整 old/new differential waveform，但不会把“逐 strip 完整刷新”暴露成视觉动画。

处理：`epd_draw_base_scroll()` 改为缓存每个 waveform phase 的 conversion LUT，并在 `output_lcd/render_lcd.c` 中按 `phase = current_frame - bucket_progression` 为每个 bucket 选择 LUT。后续调参应围绕 `scroll_offsets`、bucket 数、phase timing 和 waveform，而不是回到 app-layer 多次整帧 push 或 strip-level whole-waveform loop。

### 20. `EPD_OPTIONS_DEFAULT | EPD_FEED_QUEUE_32` 不等于“默认 LUT + 32 队列”

**结论：这是 epdiy strict 包可能无显示的底层初始化风险。**

`EPD_OPTIONS_DEFAULT` 在 epdiy 里是 `0`。把它和 `EPD_FEED_QUEUE_32` 做 OR 之后只剩 feed queue bit，没有 `EPD_LUT_1K` 或 `EPD_LUT_64K`。`epd_renderer_init()` 只有在 options 恰好等于 `EPD_OPTIONS_DEFAULT` 时才选择默认 LUT；带了 feed queue bit 但没带 LUT bit 会进入 invalid-options 分支，renderer LUT/队列初始化不可靠。

正确处理：PaperS3 LCD/S3 vector path 应显式传 `EPD_LUT_1K | EPD_FEED_QUEUE_32`。以后凡是组合 epdiy init options，不能把 zero-valued default 当成普通 flag 参与 OR。

### 21. 用完整 30-phase GL16/GC16 做移动翻页

**结论：层级虽然对了，但 waveform 窗口仍可能过长。**

2026-05-17 深挖 v0.4.45 后发现：ED047TC1 的 `MODE_GL16` / `MODE_GC16` 都是 30 phase，而 EDCBook 逆向证据显示普通翻页有 `0..15` progression table，并且 scroll context 使用压缩的 scroll phase/window（`scroll_mode_frames` 为 2 或 3），不是完整 30-phase 灰阶更新。

如果把完整 30-phase waveform 放进 renderer-level scroll，即使不再是 strip-level whole-waveform loop，也可能表现为刷新窗口过长、尾段后闪、触摸长时间被 display busy 拖慢。

处理：v0.4.46 起，epdiy scroll renderer 对 ED047TC1 长 waveform 做保守 cap：`active_frames = min(waveform_frames, 15)`，保留 renderer-level bucket phase progression，但避免把 30-phase full grayscale cleanup 当作移动翻页动画。周期性质量清理由 reader full-clean 策略承担。

### 22. 仅做 bucket-local waveform phase progression 还不是“整页空间滑动”

**结论：当前 renderer-level scroll 比 app-layer 多整帧和 strip-level whole-waveform 正确，但它仍主要是 fixed-position wipe/reveal。**

2026-05-17 复审 v0.4.46-rc 代码后确认：`EpdiyPaperS3Backend::pushPageTurn()` 当前把最终新页面写入 epdiy front framebuffer，再由 `epd_hl_update_area_ex()` 生成 old/new difference buffer；`render_lcd.c` 在每个 bucket 内仍从同一个物理 `sx` 读取最终 diff，只是让不同 bucket 使用不同 waveform phase。

这能形成移动 wavefront，避免 strip A 完整刷完再刷 strip B 的“刷子感”，但没有 EDCBook 级的 source remap / `row_offset_table` / transition-frame composition，因此内容本身不会逐帧按空间偏移重新取样；视觉上可能仍更接近“列向擦入/擦出”，不是完整页面像纸一样滑过去。

后续如果真机反馈仍不像 M5ReadPaper / EDCBook，需要新增 transition-frame renderer：每个物理 frame/bucket 从 old/new 两页按 scroll offset 合成中间图，再生成对应的 old->intermediate 或 intermediate->new transition byte，而不是只对最终 diff 延迟相位。实现前必须保留 dirty mask、difference transition byte 和 ED047TC1 waveform 安全窗口，不能回到 app-layer 多整帧动画。

### 23. Scroll 多 bucket 共用单一 frame_time 的相位时长问题

**结论：已修。**

renderer-level scroll 的一个物理 frame 里会同时包含多个 bucket-local waveform phase。如果 `prepare_context_for_next_frame()` 只用 `current_frame` 对应的 `phase_times`，晚启动的 bucket 可能正在 phase 0/1，却被较后 phase 的时长驱动，造成欠驱动、尾闪或残影。

v0.4.46-rc 修正为：scroll 模式下遍历当前物理 frame 中所有 active bucket-local phase，使用最大的 `phase_times[phase]` 作为该 physical frame 的 `frame_time`。同时 scroll 已经预构建 per-phase LUT cache，`prepare_context_for_next_frame()` 不再重复 build 普通 `ctx->conversion_lut`，减少每帧 CPU 负担。

### 24. 把 fixed-position wavefront/wipe 当成 EDCBook 效果交付

**结论：不接受。**

用户明确要求的是 EDCBook / M5ReadPaper 的整页滑动效果：旧页内容和新页内容必须按空间位置重映射，肉眼看到的是一整页纸滑过去。仅仅让不同 bucket 在最终 diff 上延迟进入 waveform phase，哪怕 renderer 层级更低、timing 更正确，也仍是 fixed-position wipe/reveal，不是目标效果。

处理：v0.4.47-rc 起，epdiy reader page-turn 默认不再使用 fixed-position bucket wipe。默认路径改为 true page-slide compositor：每个中间阶段按 old/new 两页横向位移合成完整 540x960 页面，再交给 epdiy 更新。这个版本优先保证视觉语义正确；如果速度不够，后续优化必须把同一套 spatial remap 下沉到 renderer/transition-byte 层，不能回退到 wipe。

### 25. 把“双页同时滑动”误认为 EDCBook 的覆盖推进线

**结论：目标语义需要修正。**

2026-05-17 用户进一步澄清：EDCBook 更像“一条线推过去，新页面覆盖旧页面，而且残影控制好”，而不是旧页整体平移出屏、新页整体平移入屏。也就是说，旧页在推进线前方应保持原地，推进线后方显示最终位置的新页。

之前 v0.4.47-v0.4.50 的 true page-slide / fused slide-diff 路线解决了“不要固定位置 wipe”和“不要硬件扫线冒充动画”的问题，但视觉语义仍是 old/new 双页面空间滑动，不是 cover-line page turn。

正确方向：

- 下一页：旧页固定；新页按最终坐标从右向左覆盖旧页。
- 上一页：旧页固定；新页按最终坐标从左向右覆盖旧页。
- 推进线是视觉边界；后续残影控制应集中在新覆盖区域、推进线附近和最终落页。
- 不再把“双页同时滑动”作为默认目标效果；除非用户明确要求纸张滑入/滑出动画。

处理：v0.4.51-rc 起，默认 epdiy compositor 改为 page-cover fused diff：每个 step 比较当前覆盖状态与上一覆盖状态，直接生成 epdiy difference frame，不再做旧页源坐标平移。
