# Xbox 360 播放性能优化计划

> 状态：**已完成根因归因与实机验证（§11–§13）**——根因为 M1（mpv 默认 LANCZOS+HQ flags 在
> 标量 sws 下对非 1:1 窗口化几何需 440ms/帧）；对策“1:1 surface（主）+ sws area/fast（兜底）”
> 经四轮实机采集验证，720p 窗口化/全屏与 360p 窗口化均达 30fps 视频 + 53–60fps UI。
> 已回写 `XBOX360_PORTING_PLAN.md` M7；遗留观察项见 §13 末尾。
>
> 依据：当前工作树代码审查、用户提供的实机观察，以及
> `XBOX360_NETWORK_PERFORMANCE_REVIEW.md`、`XBOX360_UI_PERFORMANCE_REVIEW.md` 中可复核的线索。
> 两份审查报告是线索，不视为本计划的权威结论。
>
> 对应主计划：`XBOX360_PORTING_PLAN.md` 的 M5/M6/M7。性能任务最终状态应回写主计划。
>
> 更新（2026-10-02）：H1 代码级核对完成（§8）；H2 代码审计完成（§9），首要候选机制收敛为
> “mpv 对非 1:1 几何的 CPU swscale 缩放与色彩重映射”；P0 测量代码已实施并构建通过（§10，
> 产出 `build-xbox360-local3/wiliwili` XEX），待实机四场景采集。色条实机核验仍属 P0/P1 验收项。

## 1. 当前实机现象

- FFmpeg pthread spread/bind 已启用，用户确认利用全部硬件线程。
- 全屏 720p 播放基本流畅，但偶尔卡顿。
- 非 720p 视频（包括更低分辨率）以及非全屏播放极其卡顿。
- 该结果不符合“单纯按解码像素数增长”的简单解释。它提示需要优先验证输出尺寸、格式/stride、纹理上传与窗口区域更新；这只是诊断优先级，不是根因结论。

## 2. 代码审查结论

以下是当前工作树中可直接核对的实现事实：

1. `scripts/build_xbox360_linux.sh` 配置 `MPV_SW_RENDER=ON`，Xbox 构建走 mpv 软件 render API。
2. `MPVCore::init()` 在创建 mpv 前调用 `xbox360_pthread_set_bind_spread(1)`，并配置 `vd-lavc-threads=6`。因此“尚未启用 spread”不再是待办；仍可在启动日志/调试计数中复核实际映射，但不应重复把启用 spread 当作优化方案。
3. `MPVCore::on_update()` 将 render update 合并为一个 `brls::sync` 任务；`mpvRenderContextRender()` 在该主线程回调中执行。应用帧循环在绘制后消费 sync 队列。
4. Xbox 的软件输出目标尺寸由 `setFrameSize(rect)` 得到，即显示控件矩形乘 `windowScale`，并限制到 1280×720；代码注释表示 GPU 会把输出贴图拉伸到显示矩形。
5. Xbox `draw()` 将输出交给 `Xbox360YUVRenderer::draw()`。renderer 对新帧执行三次 D3D9 L8 纹理 `LockRect`/逐行 memcpy，再提交 YUV shader quad；已有平均计时分别覆盖 `mpvRenderContextRender()` 和 renderer 调用/上传，但统计不含分位数，且 renderer 的 submit 时间包含状态读写、提交以及可能的同步成本。
6. Xbox YUV renderer 按 I420 三平面计算 U/V 地址；`mpv_core.hpp` 当前 Xbox 软件渲染参数的格式由大端宏选择为 `abgr`，其内存契约与 I420 三平面之间需要明确验证，不能仅凭名称推断二者一致。
7. 全屏切换创建新的 `VideoView` 并复制播放 UI 状态。需要确认播放器绘制矩形何时更新、渲染尺寸何时重建，以及切换前后 mpv 软件 render 参数、texture 尺寸是否一致。
8. 现有帧探针仅输出平均阶段耗时；没有 P95/max、源视频尺寸、render target 尺寸、目标 rect、上传字节数、源帧/显示帧计数和丢帧关联数据。

相关代码索引：

| 关注点 | 文件/符号 |
|---|---|
| Xbox build render mode | `scripts/build_xbox360_linux.sh`，`MPV_SW_RENDER=ON` |
| spread/bind 与解码线程数 | `wiliwili/source/view/mpv_core.cpp`，`MPVCore::init()` |
| 更新合并和 render 调用 | `wiliwili/source/view/mpv_core.cpp`，`MPVCore::on_update()` |
| 软件 render 参数、格式和 stride | `wiliwili/include/view/mpv_core.hpp`，`mpv_params` / `sw_format` |
| 输出表面尺寸与内存分配 | `wiliwili/source/view/mpv_core.cpp`，`MPVCore::setFrameSize()` |
| 视频帧 GPU 上传和绘制 | `wiliwili/source/view/mpv_core.cpp`，`MPVCore::draw()` |
| YUV 纹理分配、上传与状态提交 | `library/borealis/library/lib/platforms/xbox360/xbox360_yuv.cpp` |
| 帧阶段计时 | `library/borealis/library/lib/platforms/xbox360/xbox360_perf.cpp` |
| 全屏 view 替换 | `wiliwili/source/view/video_view.cpp`，`VideoView::setFullScreen()` |

## 3. 首要待验证问题

按优先级排序；不得在数据采集前直接把任何一项标记为根因。

### H1：软件 render 像素格式和 YUV renderer 输入契约是否一致

核对 mpv/libmpv 的 Xbox 实际版本中 `MPV_RENDER_PARAM_SW_FORMAT` 的语义、输出内存布局、plane 数、stride 和字节序，并与 `Xbox360YUVRenderer::draw()` 的 I420 假设逐字节比对。用固定的合成色条/灰阶帧核验 Y/U/V 平面、色彩矩阵及 pitch，记录 render callback 每次实际输出的格式和尺寸。若契约不一致，先修正格式适配，再做性能结论。

### H2：尺寸变化、目标矩形和全屏切换时序

对照源视频实际解码尺寸、mpv software render 尺寸、YUV 纹理尺寸、NanoVG rect 与屏幕 viewport。检查低分辨率源是否被错误地要求输出到显示尺寸，非全屏 rect 变更是否触发反复分配/释放，及 fullscreen view 替换是否有旧 rect/尺寸的一帧或多帧窗口。补充日志只在尺寸变化时输出，避免逐帧日志污染测量。

### H3：主线程 render 与 D3D9 上传/锁定是否阻塞

分开计时 mpv render、各平面 LockRect、逐行 memcpy、UnlockRect、flush、状态保存/恢复和 DrawPrimitive。捕获 P50/P95/max、每帧像素和字节数、锁等待/失败次数。全屏 720p 表现较好不能排除小 rect/不同尺寸触发的 D3D9 纹理路径问题。

### H4：低分辨率素材是否实际采用了不同的编码/帧率/色彩属性

测试矩阵需使用同一内容、尽量同帧率/编码，只改变 rendition；另用同一 rendition 比较窗口状态。记录 codec、profile、bit depth、chroma、fps、SAR/DAR、色彩属性及实际解码尺寸，避免将码率选择、编码复杂度和显示模式混为一谈。

### H5：sync 队列、UI 叠层和 Present 是否造成间歇尖峰

现有 `PHASE_SYNC` 包含 mpv render，不能区分其与其他同步回调；增加 mpv render 子阶段与独立上传阶段指标。分别测试播放页隐藏非必要图片/弹幕和 OSD 的对照组，观察 frame-time 尖峰是否跟 UI 工作相关。

## 4. 诊断与优化阶段

### P0：固定基线并扩充低扰动测量

- 固定同一视频内容和播放区间，建立 2×2 场景：720p/低分辨率 × 全屏/非全屏。每项至少重复 3 次，记录固件/构建版本、输出模式、视频属性和播放时长。
- 增加尺寸变化日志：源解码宽高、software render 宽高、格式、plane strides、YUV texture 尺寸、目标 rect、windowScale、viewport、每帧上传字节数。
- 增加帧级计时直方图或固定大小采样环形缓冲区，报告 P50/P95/max；避免在热点逐帧打印。
- 区分 decoder output、mpv render callback、renderer 上传、UI frame、Present 的帧计数及累计丢帧。若当前 FFmpeg/mpv 暴露不了可靠 decoder drop 计数，记录限制并以可观测的队列/回调指标替代，不伪造对应指标。
- 每个场景同时记录播放稳定区间与 seek/切换后的过渡区间，避免只测稳态平均值。

验收：日志能够解释每个场景的实际 render 目标、像素格式与主要耗时；性能采集本身不显著改变帧率。

### P1：先验证并修正帧数据与尺寸契约

- 完成 H1 的格式/plane/stride/字节序审计和色条验证。
- 完成 H2 的 resize/fullscreen lifecycle 审计；只在尺寸真实变化时重新分配缓冲区和 D3D 纹理。
- 对无新帧的 draw 明确验证不上传；对尺寸变化和 renderer 初始化记录资源创建次数。
- 先做正确性修复和单元/样例验证，再比较性能，避免对错误输出路径做微优化。

验收：测试素材颜色/亮度正确，所有平面无越界；全屏与窗口切换不出现错误尺寸、重复分配或旧帧资源引用。

### P1：定位瓶颈后做最小、单变量性能实验

- 若 `mpvRenderContextRender` 占主导：比较不同 render surface 尺寸，并查明 surface 尺寸变化是否引入额外 swscale/格式转换；确认限制输出尺寸的正确方式后再调整。
- 若 `LockRect`/memcpy/Unlock 占主导：比较 D3D9 texture upload 的可用更新策略、pitch 与纹理格式；保留 Xbox 360 对齐/tiling 约束，不能直接套用桌面 D3D9 假设。
- 若视频控件 rect 变小反而导致明显变慢：做控件矩形固定、切换 fullscreen 的 A/B，确认性能是否随 render surface 或 viewport 变化；不要先假设非全屏需要更少像素。
- 若 `PHASE_SYNC` 或 UI build/flush 占主导：分开处理队列回调、弹幕、图片纹理和 UI overlay，保持 borealis/D3D resource 操作的线程亲和性约束。
- 若解码耗时仍占主导：spread/bind 已开启，接下来比较 1/2/4/6 解码线程，记录实际硬件线程映射、同步开销及 UI 影响，选择实测最优值，不默认 6 一定最快。
- 每轮只改变一个变量，保留原构建和配置作对照；结果需同时覆盖四个基准场景。

### P2：结构性 renderer 改进（以测量结果为准）

- 如当前软件输出尺寸/格式转换是主要成本，评估 MPV/Xbox 的更直接 YUV 输出或 D3D9 renderer 路径；先做独立 renderer 样例验证格式、色彩、pitch、锁定、状态恢复和内存，再接入播放器。
- 评估 latest-frame 覆盖策略是否已由 `renderPending` 与 mpv update 语义充分实现。当前已有原子合并标志，不能把“每帧无限排队”当作已确认问题；需要测量 sync queue backlog 和实际 render 次数后再改。
- 有证据表明资源反复分配时，再做 surface/texture 缓存或安全的双/三缓冲；计算 Xbox 360 内存预算，处理 resize、暂停、退出和设备生命周期。
- 仅当 API burst 与播放卡顿在时间上相关时，才将网络 DNS、TLS 和请求线程池纳入播放性能修复范围；否则作为独立的 M7 网络子项。

## 5. 场景与验收

基线场景：

| 场景 | 视频来源 | 播放区域 | 目的 |
|---|---|---|---|
| A | 720p rendition | 全屏 | 已知相对较好基线 |
| B | 低于 720p rendition | 全屏 | 验证分辨率/格式相关差异 |
| C | 同一 720p rendition | 非全屏 | 隔离显示 rect 与 fullscreen 路径 |
| D | 同一低分辨率 rendition | 非全屏 | 复现最差情况 |

建议统一验收目标，最终数值以当前主计划目标和实机能力确认后落定：

- 480p 和 720p 各自记录播放 FPS、UI FPS、frame time P50/P95/max、decoder/render/display 丢帧及缓冲停顿。
- 非全屏不得比同 rendition 全屏出现数量级性能退化；具体允许差异应由 P0 基线确定并在主计划中固定。
- 720p 全屏持续播放至少 10 分钟；四场景各重复 3 次。记录偶发卡顿事件，而不只报告平均 FPS。
- 全屏/非全屏切换、seek、暂停/恢复和退出后，帧数据、纹理资源和线程/句柄数量无持续增长，画面颜色与比例正确。
- 保持性能数据与日志配置，说明工具版本和采样开销；禁止用 verbose/XBDM 逐帧输出作为最终性能数字。

## 6. 暂不采纳的旧结论

- “spread/bind 尚未调用”：与当前 `MPVCore::init()` 不符；当前调用已存在，用户也确认实机启用。
- “Xbox 播放路径必然为整帧 RGBA 上传”：当前代码是 `MPV_SW_RENDER` 加 Xbox 专用 YUV 三纹理 renderer；应以当前构建产物、mpv render 参数和运行时计数核实，不能沿用旧报告描述。
- “每个 mpv update 都排入独立 sync 任务”：当前 `renderPending` 原子标志会合并待处理更新；剩余队列积压需测量证明。
- “低分辨率慢的原因是 CPU 解码”：当前实测关系不足以支持该结论，先检查格式、输出尺寸、上传和显示路径。
- 网络 DNS/TLS/线程池问题可能影响详情页打开和缓冲，但现有证据不足以解释稳定播放中“非全屏/低分辨率极慢”的模式；先分开测量。

## 7. 文档与实施约定

- 本文是诊断计划，不宣称任何未实测根因已修复。
- 每项实施记录：代码提交/配置、基线与对照、四场景结果、正确性回归、资源生命周期结果。
- 完成的性能任务回写 `XBOX360_PORTING_PLAN.md` M7；旧审查报告保留为历史分析并标明结论已由后续代码/实机证据更新。
- 不因性能试验改变 Desktop/Switch 默认 renderer、MPV 配置或通用 UI 行为；平台特定改动必须用对应平台构建回归。

## 8. H1 代码级核对记录（2026-10-02）

核对范围：wiliwili 侧 `mpv_core.hpp` / `mpv_core.cpp` / `xbox360_yuv.cpp`；mpv 侧
`/home/lbx/cb360/cbffmpeg/mpv-0.36.0/video/out/libmpv_sw.c`（定制版本，含 YUV 输出路径）、
`video/mp_image.c`、`video/csputils.c`、`video/img_format.c`。纯代码审查，未上机，无代码修改。

**结论：代码级契约一致**，未发现格式/plane/stride/字节序不匹配。§2 第 6 条的“abgr 字节序
担忧”不适用于 Xbox 分支（Xbox 走 `sw_format = "yuv420p"`，非 "abgr"）。H1 的代码审计部分
完成；色条/灰阶实机核验仍为 P0/P1 验收项，未完成前不宣称 H1 已关闭。

逐项核对：

1. **格式解析**：`"yuv420p"` → `mp_imgfmt_from_name()` → `av_get_pix_fmt("yuv420p")` →
   `IMGFMT_420P`。mpv 侧 `yuv_output` 条件（YUV、uint、字节对齐、3 平面、4:2:0、宽高为偶）
   全部满足，因为 `setFrameSize()` 强制 `drawWidth/Height &= ~1`。
2. **内存布局**：mpv 写入 `[Y: pitch*h][U: pitch/2*ceil(h/2)][V: 同]`（`libmpv_sw.c:221-233`）；
   `xbox360_yuv.cpp:363-371` 的读取偏移逐项相同；wiliwili 分配
   `pitch*h + pitch*((h+1)/2)`（`mpv_core.cpp:809`）与 mpv 所需总量精确相等（h 为偶时
   = 1.5*pitch*h）。无越界、无缺口。
3. **stride/对齐**：`pitch = align64(w)` ≥ w（mpv 要求 `stride ≥ w*bpp`，yuv420p 的 bpp=1）、
   pitch 为偶（mpv 与 renderer 双重校验）；chroma stride = pitch/2 为 32 的倍数；缓冲基址
   32 字节对齐，U/V 偏移均为 32 的倍数，满足注释宣称的 swscale 平面对齐要求。
4. **色彩矩阵**：mpv 对输出目标的 csp 用 `mp_csp_guess_colorspace(w, h)`（阈值
   `w>=1280 || h>576 → BT.709，否则 BT.601`），levels 固定 TV/limited（`mp_image.c:843`、
   `csputils.c:274`）；shader 用同一 `sw_size` 与同一阈值选择 limited-range 系数
   （`xbox360_yuv.cpp:455`）。两侧输入相同，矩阵必然一致；src→dst 转换由 swscale 完成，
   dst→RGB 由 shader 完成，链路自洽。表面尺寸被截断到 1280×720 不影响一致性，因为两侧   读取的都是截断后的 `sw_size`。
5. **OSD/字幕**：YUV 输出目标跳过 `osd_draw_on_image()`（`libmpv_sw.c:276`），mpv 字幕与
   OSD 不会合成进视频表面，画面内 UI 全部由 wiliwili 自绘。属既定设计，记录为已知行为，
   若未来需要 mpv 字幕需另行方案。
6. **已有测量点**：mpv 内置 `[x360-sw]` 探针，每 30 次 render 输出 reinit/clear/scale 平均
   耗时与 `src→dst` 几何（`libmpv_sw.c:14-30`），可直接用于 P0 判断 swscale 是 1:1 拷贝
   还是逐像素重采样，以及 sws reinit 是否被反复触发。

遗留项（转 H2/P0，不属 H1）：

- 表面尺寸为 `rect*windowScale` 后独立截断到 1280×720。当 `windowScale>1`（如 1080p 输出
  模式）且 rect 宽高比 ≠ 1280:720 时，两轴截断比例不同 → 表面宽高比 ≠ 显示 rect 宽高比，
  GPU 拉伸会变形。需在 H2 连同 mpv 的 dst_rc（letterbox）一起核对。
- wiliwili 的返回值检查不完整：`on_update()` 路径已检查并记录 `mpvRenderContextRender()`
  返回值（`mpv_core.cpp:302-306`），但 `setFrameSize()` 内的同步 render（`mpv_core.cpp:857`）
  未检查。P0 日志中应覆盖后者。
- 色条/灰阶实机核验（平面顺序、色彩、pitch、每帧实际输出格式与尺寸）待 P0/P1 执行。

## 9. H2 尺寸/矩形/全屏时序代码审计（2026-10-02）

范围：wiliwili 侧 `MPVCore::setFrameSize()/draw()/on_update()`、`VideoView::setFullScreen()`、
borealis `Application::frame()` 活动栈绘制；mpv 侧 `video/out/vo_libmpv.c`、
`video/out/libmpv_sw.c`、`video/sws_utils.c`。纯代码审查，未上机，无代码修改。

### 9.1 已核实的生命周期事实

1. **稳态不重复分配**：`setFrameSize()` 仅在 `requiredBytes` 增大时重新 malloc
   （`mpv_core.cpp:814-835`）；YUV 纹理仅在尺寸变化时重建（`xbox360_yuv.cpp:278`）。
2. **尺寸变化时的一次性成本**：`setFrameSize()` 在尺寸变化时同步执行一次
   `mpvRenderContextRender` + `ReportSwap`（`mpv_core.cpp:857`）。mpv 侧仅当 viewport
   变化才 `m_config_cache_update` + `mp_get_src_dst_rects` + backend `resize`
   （`vo_libmpv.c:348-363`）；sws 仅在 `anything_changed` 时 reinit（`libmpv_sw.c:143-209`）。
   **稳态无逐帧 reinit**（`cache_valid`，`sws_utils.c:166`）。
3. **无新帧不上传**：`draw()` 传 `freshFrame ? pixels : nullptr`，无新帧时跳过三次
   `LockRect`/memcpy（`mpv_core.cpp:943-953`、`xbox360_yuv.cpp:363`）。
4. **稳态每帧只有一个 VideoView 在绘制**：`Application::frame` 只绘制到第一个不透明
   activity 为止（`application.cpp:781-791`）；`isTranslucent()` 默认仅在 fadeIn/动画中为
   true（`view.cpp:1362`），全屏容器与播放页均不覆写 → 全屏 push 后旧播放页停止绘制；
   PlayerSetting 等半透明上层不会带来第二个 VideoView。“双 VideoView 交替 setFrameSize”
   只可能出现在 activity 淡入动画的过渡帧，需计数器确认是否发生，不是稳态问题。
5. **全屏切换**：`setFullScreen(true)` 新建 VideoView 并 push（`TransitionAnimation::NONE`，
   `video_view.cpp:1243-1292`），退出走 pop（`video_view.cpp:1301+`）。切换后 rect 一次性
   变化 → 一次 `setFrameSize` → 一次同步 render。
6. **mpv 渲染合并与节拍**：`on_update()` 用 `renderPending` 原子合并，sync 任务中执行
   render + `ReportSwap` 并检查返回值（`mpv_core.cpp:280-319`）；
   `blockForTargetTime = 0`（`mpv_core.hpp:432`）→ `mpv_render_context_render` 不等待 present。
7. **矩形实际尺寸**：全屏 = 1280×720（`video_activity.xml` 100%）；窗口化播放页 video 宽 =
   样式 600 或 800（`register_helper.cpp:211/245`），高 = 列内剩余（约 720−177−margin），
   **非 16:9**。`windowScale = 窗口宽/1280`（`application.cpp:1190`），720p 输出下 = 1。

### 9.2 关键机制推导（候选，非结论）

mpv 把视频 **aspect-fit 进 surface**（`mp_get_src_dst_rects`），surface 内区由 CPU swscale
缩放并写出，GPU 再把整张 surface 拉伸到显示 rect。于是四场景为：

| 场景 | src→dst 几何（mpv 内部） | 色彩重映射（两侧分别猜测） |
|---|---|---|
| A 720p 全屏 | 1280×720 → 1280×720，**1:1** | 源 709 ↔ 目标 709，一致 |
| B 低分辨率 全屏 | 小 → 放大至 ~1280×720 | 源按源尺寸猜 601 → 目标按 surface 猜 709，**不一致** |
| C 720p 窗口 | 1280×720 → 缩小至 ~600×337 | 源 709 → 目标按 surface（<1280 宽）猜 601，**不一致** |
| D 低分辨率 窗口 | 小 → ~600×337 | 601 → 601，一致 |

“仅 A 良好”与“仅 A 为 1:1 几何”完全对应；B/C 还叠加逐帧 YUV 矩阵重映射
（猜测规则：`mp_image.c:843`、`csputils.c:274`，两侧各自按各自尺寸猜）。
代码层面已排除：逐帧 sws reinit、逐帧纹理重建、逐帧 realloc、无新帧上传、OSD 合成。
**未能排除（须 P0 数据判定）**：非 1:1 swscale 在 Xenon（可能走 C 实现无 SIMD）的绝对成本、
`unaligned libswscale parameter; using slow copy`（`sws_utils.c:380`，mp_verbose 级）是否
每帧触发、窗口化场景的 UI/同步开销（H5）。

### 9.3 并入 P0 的计数器/日志清单

1. `setFrameSize` 调用与**真实尺寸变化**计数（每秒），含 from/to rect、`sw_size`、pitch
   —— 判定/排除 rect 逐帧震荡（含全屏过渡的双 VideoView 过渡帧）。
2. `ensureTextures`/`init` 资源重建计数。
3. 捕获既有 `[x360-sw]` 探针（`libmpv_sw.c:14-30`，每 30 次输出 reinit/clear/scale 平均
   与 src→dst 几何）—— 9.2 三个未决项的直接判据；同时采集 `[x360-perf]` renderer 计时。
4. mpv verbose 日志中 “unaligned libswscale parameter” 是否出现（对应 M4 慢拷贝路径）。
5. `setFrameSize()` 内同步 render 的返回值记录（当前未检查）。
6. 各阶段 P50/P95/max 与帧计数（见 P0 原有条目）。

### 9.4 遗留风险（本次不修复）

- `rect` 在参数校验前赋值（`mpv_core.cpp:786`），NaN/零尺寸早退会锁存退化 rect，直到下次
  rect 变化才恢复。
- `windowScale>1`（如 1080p 输出）时 1280/720 两轴独立截断可能改变表面宽高比（见 §8 遗留）。
- 全屏 push/pop 动画期间的双 VideoView 过渡帧（由 9.3-1 计数确认量级）。

## 10. P0 测量代码实施记录（2026-10-02）

对应 §3 P0 与 §9.3 清单。只增加计数与日志，不改渲染逻辑、分配策略、mpv 参数；Xbox 专属
计数均在 `PLATFORM_XBOX360` 宏内，不影响其他平台。

### 改动清单

| 文件 | 改动 |
|---|---|
| `library/borealis/.../xbox360_perf.hpp` | 新增 `Ring`：256 定容量采样环，`add()` 仅两次写入，`quantile()` 只在报告时复制+插入排序，热点路径无分配无日志 |
| `library/borealis/.../xbox360_perf.cpp` | frame phase 报告新增第二行 `p50/p95/max`（begin/build/flush/present/sync），随窗口重置 |
| `wiliwili/source/view/mpv_core.cpp` | ① `X360PerfProbe`（mpv render、yuv renderer 调用）报告改为 avg+p95+max；② 新增 `X360SizeProbe`：setFrameSize 调用/真实尺寸变化/表面分配计数，每 2s 一行汇总，尺寸变化时 info（前 8 次及每 64 次）/debug（其余），含 from→to surface、pitch、rect；③ `setFrameSize()` 内同步 `mpvRenderContextRender` 返回值检查并记错误 |
| `library/borealis/.../xbox360_yuv.cpp` | ① `DrawProbe` 增加 upload/submit 采样环（p95/max）、每帧上传 KiB、LockRect 失败计数、表面创建/释放累计计数；② `uploadPlane` 返回实际拷贝字节，锁定失败计入 `lockFails`；③ `ensureTextures`/`releaseObjects` 计数 `texCreated`/`texReleased`；④ flush 改为 2s 时间窗（原仅 60 帧门槛，低帧率下出数太慢） |

### 采集时应同时出现的日志行

- `[x360-perf] frame ... fps (N frames) | begin/build/flush/present/sync ... ms`
- `[x360-perf] p50/p95/max ms | begin ... build ... flush ... present ... sync ...`
- `[x360-perf] mpv render call (swscale+clear): avg/p95/max ...`
- `[x360-perf] yuv renderer call (flush+upload+draw): avg/p95/max ...`
- `[x360-perf] yuv plane upload: ... KiB/frame, N lock fails` 与 `state+quad draw: ... surfaces created/released`
- `[x360-perf] setFrameSize: N calls, M size changes, K surface allocs, surface WxH pitch P`
- `MPVCore::setFrameSize #N: surface ... -> ... pitch ... -> ...`（仅尺寸真实变化时）
- mpv 侧既有 `[x360-sw] render avg ... reinit/clear/scale | src ... -> dst ...`（每 30 次 render）
- mpv verbose：`unaligned libswscale parameter; using slow copy.`（若出现即 M4 触发）

### 判读规则（对应 §9.2 未决项）

- **M1（非 1:1 swscale）**：`[x360-sw]` 的 `scale ms` 在 B/C/D 显著高于 A，且 `src→dst` 非 1:1。
- **M2（逐帧 reinit）**：`reinit ms` 持续 >0 或 SWS 打印信息刷屏。
- **M3（rect 震荡）**：`setFrameSize` 汇总中 size changes/窗口 接近 UI 帧数，或 `surfaces created`
  持续增长。
- **M4（慢拷贝）**：mpv verbose 出现 unaligned slow copy 行。
- **H5（UI/同步）**：`build/flush/sync` 分位数主导，且开/关弹幕、图片、OSD 的 A/B 有差异。
- **帧计数对照**：PHASE_BEGIN（UI 帧）、mpv render 样本数、upload 新帧数三者每窗口对比，
  即可得到渲染/上传相对 UI 的丢帧关系（decoder 侧无可靠丢帧计数则按 P0 原文记录限制）。

### 状态

- 构建：**已通过**（2026-10-02，`build-xbox360-local3`，424/424 链接成功，
  产出 XEX：`build-xbox360-local3/wiliwili`）。改动文件（xbox360_perf/yuv、mpv_core）全部
  编译通过，仅既有第三方头文件警告，无新增告警。
- 使用既有活动构建目录 `build-xbox360-local3`（SW=ON、Release，2026-10-01 配置）。
  注：脚本默认的新目录 `build-xbox360-linux` 从零 configure 会失败——工具链经 `include()`
  引入时 `add_compile_options`（`-D_WIN32`/`-fms-extensions` 等，`toolchain.cmake:143-159`）
  不进入 CMake 编译器探测的 try_compile，XDK `crtdefs.h` 因缺 `_WIN32` 报错；属既有环境
  问题，与本次改动无关，未修改工具链。
- ~~待办：构建通过 → 实机按 §5 四场景各 3 次采集 → 依判读规则归因 → 回写 M7。~~
  **已完成**：见 §11–§13（根因 M1，对策已实机验证，M7 已回写）。

## 11. 实机 P0 第一轮记录（2026-10-02，场景 A/C）

装置：devkit ccb @192.168.3.126，构建 `build-xbox360-local3`（含 §10 全部探针），
持续日志 `logs/p0_perf_20261002_210108.txt`（xbdm listen --plain）。

### 场景 C：720p 窗口化（surface 800x450，pitch 832）

| 环节 | 实测（avg/p50） | 判定 |
|---|---|---|
| **mpv render（swscale+clear）** | **450–470 ms，p95 ~490** | 🔴 每帧如此，非偶发 |
| UI 帧率 | 2–3 fps（sync 相位同源：同步 render 阻塞 UI 线程） | 结果 |
| YUV 上传 | 0.5 ms，527 KiB/帧，0 lock fail | 🟢 |
| state+quad 绘制 | 0.02 ms | 🟢 |
| setFrameSize | 稳态零调用（全程仅 1 次真实尺寸变化日志） | 🟢 **M3 排除** |
| 表面创建/释放 | 窗口期 3 created / 0 released 不增长 | 🟢 |

### 场景 A：720p 全屏（surface 1280x720，1:1）

| 环节 | 实测 | 判定 |
|---|---|---|
| mpv render | 5–20 ms avg，p95 23–66 | 🟢 1:1 几何下 swscale 健康 |
| UI 帧率 | 39–54 fps | 🟡 |
| **新视频帧产出** | **仅 ~4–7 帧/s**（upload new frames 折算） | 🔴 卡顿来源在产帧侧 |
| YUV 上传 | 7–16 ms，1350 KiB/帧 | 🟡 |
| present / build | p50 8–12 / 7–10 ms，偶发 128 ms 尖峰 | 🟡 次要 |

### 交叉事实

- 本轮推翻“A 场景一定流畅”的前提：**A 与 C 卡顿机制不同**——C 是 render
  （swscale 路径）450ms；A 的 render 健康，但视频帧产出仅 ~5fps。
- cbffmpeg P2 解码基线（同机）：720p30 High CABAC 单线程 11.3fps、4t spread 最优
  **25.35fps（0.85× 实时，本就未达实时）**；wiliwili 侧 `vd-lavc-threads=6` +
  `xbox360_pthread_set_bind_spread(1)` 已就位。A 的 ~5fps 与基线差距大，
  需统计行区分“解码慢”与“帧被丢弃”。
- 窗口化 surface 800x450（16:9），720p 源 → 降采样；全屏 1:1——与 M1 几何吻合，
  但 450ms 对 800x450 降采样数量级反常（应为毫秒级），**必须**由 `[x360-sw]`
  的 reinit/clear/scale 分解定性，数据齐备前不作归因。

### 第一轮暴露的采集缺口（已修，待第二轮验证）

1. `mpv_request_log_messages` 从未调用（cbffmpeg.md 已记为已知问题）→ mpv 全部日志
   （含 `[x360-sw]` 探针、slow-copy 提示）不可见。已在 `mpv_initialize` 后
   `mpv_request_log_messages(mpv, "v")`（仅 `PLATFORM_XBOX360`）。
2. slow-copy 为 `mp_verbose`，release 级别下被 `Logger::debug` 过滤 → 已在
   LOG_MESSAGE 处理器将含 "slow copy" 的行提级为 info（仅 Xbox）。
3. 新增 `[x360-mpv]` 每 2s 帧统计行：container/est-vf fps、vo/decoder/vo-delayed
   丢帧计数、demuxer 缓存——区分 A 场景“解码慢”与“丢帧”。

### 待第二轮回答

- C 的 450ms 在 reinit / clear / scale 之间如何分布，`src→dst` 是否 1:1。
- A 的 ~5fps 产帧瓶颈：`decoder-frame-drop-count` vs 解码速率 vs vo 丢帧。
- 若分别证实，则为**两个独立问题**（swscale 路径异常 + 解码产帧不足），
  按纪律在数据齐备前均标“待验证”。

## 12. 实机 P0 第二轮记录（2026-10-02，[x360-sw]/[x360-mpv] 齐备 → M1 归因）

日志：`logs/p0_perf_20261002_212359.txt`（新构建含 §11 的三项日志修复）。

### 分解数据（每 30 次 render 一行均值）

| 场景 | 几何 src→dst | reinit | clear | **scale** |
|---|---|---|---|---|
| C 窗口化 | 1280x720 → 800x450 | 0.00–7.93 ms（仅首帧） | 0.01–0.07 ms | **427.6 / 439.3 / 479.7 ms** |
| A 全屏 | 1280x720 → 1280x720 | 0.00 | 0.01 | **1.73–4.00 ms** |

- **M1 证实：450ms 全部落在 `sws_scale()` 内**。reinit（M2）、clear、slow-copy（M4，
  全程无 "unaligned…slow copy" 消息）全部排除；`setFrameSize` 每次切换仅 1–3 次调用，
  稳态零调用，M3 保持排除。

### 全屏段（A）健康度（同一日志 21:28:03 切换后）

- 视频帧产出 **30–34 帧/s**（upload new frames 折算，container 29.97fps），
  mpv render ~2ms，UI 53–59fps，`decoder-frame-drop-count=0`，vo 丢帧计数早期
  爬升后平台化 —— 本轮全屏实测健康。第一轮 A 的 ~5fps 产出未能复现（当时视频源
  未知，标记"未复现，存疑"）。

### 归因（代码证据）

1. mpv sws 默认：`sws_utils.c` → `.defaults = { .scaler = SWS_LANCZOS }`，`fast=false`
   → 每次 init `flags |= SWS_FULL_CHR_H_INT|SWS_FULL_CHR_H_INP|SWS_ACCURATE_RND`。
2. 本 ffmpeg 构建 **`HAVE_ALTIVEC=0` / `HAVE_VSX=0`**（cbffmpeg P6 SIMD 未做）→
   重采样跑纯标量 C。Lanczos（~8 tap × 水平+垂直，含全色度插值）在标量 C 下
   对 1280x720→800x450 达 439ms 与观测吻合。
3. 1:1（全屏）命中 swscale **unscaled 同格式直拷贝**路径 → 1.8ms，与算法选择无关。

### 对策（本轮实施，仅 `PLATFORM_XBOX360`）

`MPVCore::init()` Xbox 分支新增：
```cpp
mpvSetOptionString(mpv, "sws-scaler", "area");   // 框式降采样，替掉 LANCZOS
mpvSetOptionString(mpv, "sws-fast", "yes");      // 去掉 HQ 全色度 flags
```
（返回值以 `[x360-sw] sws-scaler=area -> {r1}, sws-fast=yes -> {r2}` 打日志，防选项名失配。
全屏 unscaled 路径不受影响。）

### 待第三轮验证
- 场景 C 窗口化 scale 耗时（目标 <33ms，30fps 预算）；UI 帧率恢复。
- 若 area+fast 达标且有余量，可 A/B 回退 `sws-fast`（仅去 fast，保留 chroma 质量）。
- 备选路线（若选项不可行）：窗口化改为 1:1 surface（1280x720 渲染 + borealis 纹理缩放），
  彻底绕开 CPU 重采样，代价为上传量 527→1350 KiB。
- 深层修复（长期）：为 cbffmpeg 的 swscale 启用 AltiVec（现构建 `--disable-altivec`，
  `HAVE_ALTIVEC=0`）。注意这不是开个开关——Xenon 是 VMX128，比标准 VMX 少 22 条指令
  （整族 vector 整数乘），上游 altivec 直接启用会崩（cbffmpeg 已实测），须走预检+改写路线
  （`cbffmpeg/docs/p6_altivec_preflight.md`）；且该预检目前圈定的改写面是 h264 解码内核，
  swscale 属于**未纳入 P6 范围的新增项**。

## 13. 实机 P0 第三、四轮记录（2026-10-02，对策与验证）

### 第三轮：sws 算法替换（area + fast）——必要但不充分

日志 `logs/p0_perf_20261002_214130.txt`；选项生效（`sws-scaler=area -> 0, sws-fast=yes -> 0`）。

| 指标 | 第二轮（LANCZOS 默认） | 第三轮（area+fast） |
|---|---|---|
| 窗口化 scale | 427–480 ms | **80.8–106.5 ms**（4.2×） |
| 窗口化视频产出 | ~2 fps | 2–5 fps |
| 窗口化 UI | 2–3 fps | 25–51 fps |

标量 C（`HAVE_ALTIVEC=0`）下 area 仍 ~100ms，超 33ms 预算 → 仅靠换算法不够。

### 第四轮：1:1 surface（主对策）——通过

改动（仅 `PLATFORM_XBOX360`，`mpv_core.cpp` 两处）：

1. `setFrameSize`：rect 与视频宽高比一致（相对差 ≤1%）且视频尺寸已知时，
   **surface 用视频原始尺寸**而非 rect 尺寸；否则回退 rect（mpv letterbox，显示正确性不变）。
   依据：`Xbox360YUVRenderer::draw` 的纹理尺寸 `(sw_size[0/1])` 与屏幕矩形 `(rect)` 本就分开传参，
   GPU quad 拉伸纹理到 rect，显示缩放免费；mpv 目标面与视频同尺寸 → swscale 走 unscaled 直拷贝。
2. `draw()`：每 250ms 检查 `video-params/w,h` 变化（首帧加载、切清晰度）→ 自动重跑 `setFrameSize`，
   不依赖 rect 变化。

日志 `logs/p0_perf_20261002_215840.txt` 验证：

| 场景 | surface | scale | 视频产出 | UI | 上传 |
|---|---|---|---|---|---|
| 720p 窗口化（rect 800x450） | **1280x720**（override 生效） | **1.8–3.1 ms** | **30–33 fps** | 53–60 fps | 1350 KiB, 12–13 ms |
| 640x360 窗口化（切清晰度，rect 未变） | **640x360**（尺寸触发生效） | 7.3–10 ms | 30 fps | 54–60 fps | 338 KiB, 0.3 ms |
| 切换/加载瞬态 | — | 单次 88–100 ms | — | — | — |

- 无 "slow copy"；表面 12 created / 9 released（4 次切换稳态 3 个，无泄漏）；decoder 0 丢帧。
- 360p 即便 src==dst 仍 ~9ms（非纯拷贝，疑为色彩空间/范围转换），预算内，列为观察项。
- 切清晰度瞬间单次 render 88ms 与旧路径行为一致，一次性瞬态。

### 四场景矩阵状态

| 场景 | 状态 | 依据 |
|---|---|---|
| A 720p 全屏 | ✅ 30fps / UI 58fps | 第二轮日志（21:28 段） |
| C 720p 窗口化 | ✅ 30fps / UI 53–60fps | 第四轮日志（22:05–22:06 段） |
| D 低清窗口化 | ✅ 30fps（640x360 实测） | 第四轮日志（22:06–22:07 段） |
| B 低清全屏 | 同一 1:1 路径覆盖，待顺手确认 | — |

### 最终归因（回写 M7 用）

- **根因 = M1**：窗口化时 surface 跟随 rect（800x450），mpv 默认 `SWS_LANCZOS` + HQ flags 在
  **纯标量 C**（`HAVE_ALTIVEC=0`）下对 1280x720→800x450 重采样需 **440ms/帧**；
  全屏因 surface 恰为 1280x720 命中 unscaled 拷贝（1.8ms）——即“全屏流畅、窗口卡死”的全部原因。
- M2 reinit（0–8ms 一次性）、M3 rect 震荡（稳态零调用）、M4 slow-copy（全程无消息）均排除；
  H5 UI 在根因消除后恢复 53–60fps，非独立瓶颈。
- 对策分层：**主对策 = 1:1 surface**（消除 CPU 重采样）；**兜底 = `sws-scaler=area` +
  `sws-fast`**（宽高比不匹配/超过 1280x720 上限的回退路径仍需要）。

### 遗留观察项

- ~~1080p 源超过 surface 上限 1280x720 → 仍会走标量重采样（历史行为），且上传 3.1MiB/帧
  超预算——按用户决策忽略，surface 上限维持 1280x720 不动~~ **2026-10-04 取代：直播
  85% 房间只有原画档、qn=250 被服务端无视 → 1080p 不可避免，用户决策改为"PSV 怎么处理
  360 就怎么处理"，surface 上限已提到 3840x2160（1:1 + GPU 缩放），见 §14.6。**
- 360p 1:1 下 ~9ms 的非拷贝开销待定性（疑色彩参数转换）。
- 长期（低优先级）：cbffmpeg 侧 swscale SIMD——构建带 `--disable-altivec`
  （`HAVE_ALTIVEC=0`）。上游 `libswscale/ppc/*altivec*` 不可直接启用：Xenon 是 VMX128，
  缺标准 VMX 的 22 条指令（整族 vector 整数乘），cbffmpeg 已实测上游 altivec 启用即崩，
  需按 `cbffmpeg/docs/p6_altivec_preflight.md` 的预检+改写路线走；该预检圈定的改写面
  目前是 h264 解码内核（chroma MC/qpel/weight），**swscale 是范围外的新增项**。
  1:1 surface 落地后 swscale 只剩回退路径（比例不匹配/超上限/色彩转换），本项优先级进一步降低。

---

## 14. 直播卡顿排查：A/B 分类仪器与 round-4/5 记录（2026-10-03 ~ 10-04）

统一日志：`logs/p0_perf_20261003_164913.txt`（跨轮续写）。XEX md5：round-4 = `c8a954c18d7805494723e7c74da3e902`，round-5a = `b1f79233855002dfecc5fb991e083a1f`，round-5b = `c801d799c727479c126b5ddfe2faf5e7`。

### 14.1 A/B 两类冻结形态与仪器（round-3，2026-10-03 晚）

- **A 类** = sync 任务内停摆：`slow sync task took … from <tag>`。
- **B 类** = sync 之外主线程停摆：`[stall-task]` 看门狗阶段机，打 `busy`/`end <N>s at <phase>` 行。
- 分段计时：focus chain 总时长分解；Hint/View 构造分段（`inflate/text/rest`）；child dtor 分段；hint create/reuse。
- 堆探针（round-5 加）：wiliwili 侧 operator new/delete 经 `toolchain.cmake` 对 `xbox360_runtime` target `-Dmalloc/-Dfree` 改道到 `xbox360_perf.cpp` 的 `x360_heap_probe_*`；ffmpeg 侧 `libavutil/mem.c` 的 `av_malloc/av_realloc/av_free` 计时（`_XBOX` 门控）。均走 DbgPrint 裸通道（不进 Logger，防 fmt 分配递归），阈值 5ms。

**状态**：仪器可出数，A/B 分类与堆探针均已在实机产生数据（见 14.4）。

### 14.2 round-4 实测：焦点切换冻结归因（2026-10-04 13:19–13:2x）

- focus chain 冻结 **5554 / 9277 / 5885 ms**，其中 hints 段 = 5554/9277ms（`lost 0 gained 0`）。
- 冻结 100% 落在 `globalHintsUpdateEvent.fire()` → `refillHints`：每次焦点切换销毁+重建全部按钮 Hint。
- 分段对称性：构造侧 attrs 段（`registerCommonAttributes` ~90 次 map 插入）120–660ms；析构侧成员析构+`delete` 同量级变慢；**零分配 style 段恒 0ms**；yoga 单次分配偶发 13–20ms。
- 主页（无解码）零慢行。

**状态（分两级）**：
- ✅ **症状级已实测确认**：焦点切换冻结 = Hint 销毁/重建风暴（每次焦点移动全量重建）。
- ⏳ **机制级 = 假说（未验证）**：构造/析构变慢指向 CRT 堆锁争用（软解期分配密集），需 `[heap-slow]` 两侧对账点名持锁者后才能标根因。

### 14.3 round-5 修复与探针（2026-10-04 两次部署）

1. **Hint 复用池**（`hint.cpp/hpp`，Xbox 路径）：`refillHints` 摘下不销毁（`removeView(free=false)`）+ 按钮键池 `map<ControllerButton, Hint*>`；命中仅 `applyAction`（两次 setText+主题查找）；按钮一生只建一次；`~Hints` 统一释放；非 Xbox 路径逐字节原样。
2. **阶段机双时钟**：`g_stageTick`（活动，`log:*` 也刷新，看门狗用）+ `g_phaseTick/g_phaseDesc`（真实阶段，`log:*` 不参与，`end` 行用）——修 round-4 探针自身日志喂饱 tick 导致 `end` 行消失的盲区。
3. **堆探针两侧对账**（round-5a 后修正三点 → round-5b）：
   - wiliwili 链接的是 `wiliwili/build/ffmpeg-x360-tls`（源 `wiliwili/third_party/ffmpeg-4.4.8`，与 `cbffmpeg/ffmpeg-4.4.8` 同源副本）——round-5a 只重建了 cbffmpeg p4 树，**ffmpeg 侧探针实际未生效**（0 条输出）。已把 mem.c 同步到 third_party 并重建 tls 树 libavutil.a（compile flags 确认 `-D_XBOX`）。
   - 一次性 256 条预算在启动风暴（14:05:08 共 63 条、14:05:14 共 40 条）后于 14:05:57 耗尽 → **12s 析构风暴期整段失明**。改为**令牌桶**：64 枚、每 5s 回填，≥50ms 大鱼绕过令牌必放行（速率 ≈13 条/s 兜底）。
   - wiliwili 侧 `[heap-slow]` 打印增加 `tid`，用于按时间序列区分持锁者（打出长操作）与受害者（其它线程紧随的等待集群）。
   - 已知盲区：curl/TLS/网络线程的 CRT malloc 未钩（`-D` 只加在 runtime target）；若 av_*/new/del 均无大鱼但冻结仍在，持锁者在未钩路径。

### 14.4 round-5a 实测：Hint 池验证（2026-10-04 14:04:57–14:12）

| 判读项 | 结果 | 状态 |
|---|---|---|
| focus chain >阈值 | **仅 1 条 1091ms**（历史卡首次出现按钮 → 一次性 Hint ctor，inflate 1040ms） | ✅ 池生效 |
| `hint reuse took` | 0 条（reuse 路径从未超阈值） | ✅ |
| phase `end` 行 | `14:06:24 end 12s at loop:delete` 实名出现 | ✅ 双时钟生效 |
| `[heap-slow]` wiliwili 侧 | 261 条：小块 new/del 5–64ms 集群（受害者形态）；大块偶发 `new 65456 B 64 ms` | ⚠️ 受害者已见，**持锁者未点名**（预算 14:05:57 耗尽 + ffmpeg 侧 0 条） |
| 遗留冻结（B 类） | 14:06:12–24 退出全屏销毁 VideoProfile/播放器浮层 → Label/Box 慢析构 **140–1457ms/个，累计 12s**（`loop:delete` 析构风暴） | ❌ 未修，round-5b 定位持锁者 |
| 播放健康 | 直播 14:06:05–33 container 30fps；vo drops 0→195（风暴期约 +15/2s）；其余 UI 60fps | 参考 |

**状态**：✅ 症状级修复（焦点切换风暴）实测通过；⏳ 机制级（堆锁持锁者）与遗留析构风暴待 round-5b 数据。

### 14.5 round-5b 实测（2026-10-04 14:17 部署，14:21–14:29 实机）

- 内容 = 14.3 修正三点（正确 ffmpeg 树 + 令牌桶 + tid），XEX md5 `c801d799c727479c126b5ddfe2faf5e7`，回读校验一致。
- 场次：14:21:46 进直播间（1080p 源）播放 44s → 14:22:00 退出全屏（9s 析构风暴）→ 14:22:41 暂停/离开 → 14:26:26 起点播 720p 至 14:29+。
- 判读链逐项：
  1. **ffmpeg 探针接通 ✓**：`av_*` 慢行 1744 条（malloc 790 / free 856 / realloc 98）。
  2. **令牌桶 ✓**：单秒峰值 103 条，14:21–14:29 连续覆盖，无预算截断；≥50ms 必打行 8 条。
  3. **tid ✓**：16 个线程，主线程 `4177526784` 占 296 条。
  4. **双时钟实名 ✓**：`[stall-task] end 9s at loop:delete N4brls3BoxE` @14:22:11。
- **持锁者点名：未出现**。最长单笔 68ms、≥50ms 仅 8 条；`[heap-slow]` 直方图众数恰为 **20ms（206 次）+ 40ms（24 次）** 的量子倍数形态 → CRT 堆锁"单点巨持有者"假说**在已钩路径上证伪**。真实形态 = CPU 饱和下锁持有者被时间片抢占引发的全网护航（人人 5~40ms 受害），暂停（ffmpeg 停止分配）即消失——堆慢行是**受害者/放大器**，不是第一因；机制定性从"假说"更新为"实测形态"，具体持锁代码无需 round-6 扩钩追查（除非后续出现 ≥100ms 单笔）。

### 14.6 主根因实锤（round-5b 日志）与 PSV-parity 对策（2026-10-04 15:12 部署）

**用户反馈**："播放时才卡、暂停不卡"。14:17 场次日志三层归因：

1. **第一因 = 1080p 直播源 × surface 720p 上限 → 每帧标量重采样**
   - `[x360-sw] render avg over 30 calls: src 1920x1080 -> dst 1280x720 | scale 254~359 ms`（14:22:16 / 14:22:29）
   - 外层吻合：`mpv render call (swscale+clear): 324~389 ms avg`（14:21:57–14:22:39 连续）
   - `cplayer: Video (h264 1920x1080 30fps)` / `VO: [libmpv] 1920x1080`；同期 UI 掉到 ~6fps、`drops: vo` 15~22/s、A/V desync 警告
   - 对照组（14:26 后点播 720p）：`src 1280x720 -> dst 1280x720 | scale 2.4ms`、render 3.5ms、UI 60fps ✓
   - 暂停不卡机制：不产帧 → 不跑 swscale，且 ffmpeg 停止分配 → §14.5 的 20/40ms 堆护航一并消失（伴生放大器；14:22:11 的 9s 析构风暴在同窗口）
2. **qn=250 三处限制为何失效（服务端实测 2026-10-04）**：复刻 wiliwili 请求（登录/未登录、platform 变体均测）抽样 20 个在线房间——**约 85% 返回 `accept_qn=[10000]`**（主播未开转码），无视 qn=250 直接回原画；仅转码房间尊重 qn（服务端 clamp：qn=150→就近回 250，qn=400→降回 250 亦实测）。→ 限档必要但不充分，1080p 直播不可回避。
3. **用户决策（2026-10-04）："PSV 是怎么处理的，360 就怎么处理"**。PSV 核查：
   - PSV 构建**不开** `MPV_SW_RENDER`（mpv 走 GPU/FBO 渲染路径，`fbo-format=rgba8`）→ 原生尺寸帧上传 + shader 缩放，**从不 CPU 重采样**；1080p 源 = 解码能跑多快播多快（掉帧但 UI 流畅），无冻结路径
   - PSV `defaultQuality=150` 与 360 的 250 同属"按设备屏幕上限限档"模式（360=720p 屏，保持 250）
   - 全库无任何按分辨率拒播的逻辑（PSV/PS4/Switch 皆无）→ 360 也不加

**对策（仅 `PLATFORM_XBOX360`，`MPVCore::setFrameSize`）**：surface 上限 1280x720 → **3840x2160**（D3D9 纹理上限 / 直播原画上限）。
- 1080p → surface 1920x1080 = video 原生 → libmpv_sw 走 unscaled copy（~2ms）→ `Xbox360YUVRenderer::draw` viewport 硬件缩放（已核：该函数分辨率无关，任意纹理适配视口 rect，`ensureTextures` 按帧尺寸创建）
- ≤720p 行为不变（上限不再构成约束）；>4K 兜底 keep-aspect 重采样保留为死代码
- §13 遗留项"上传 3.1MiB/帧超预算按用户决策忽略"被本决策取代（PSV 同样原生上传）

**round-6 待验证**（15:12 部署，XEX md5 `8d31a8579f8dcd9c26086b3fc171086e`，回读校验一致）：
1. 1080p-only 直播间：`[x360-sw] src 1920x1080 -> dst 1920x1080` copy 路径 + `mpv render call` 回到 ~2-4ms
2. `yuv plane upload: 3049 KiB/frame` 实测 ms 与 UI fps（若单帧上传 >40ms 或 UI <45fps → 上传节流方案再议）
3. `drops: decoder` 是否 >0（1080p 解码能力首次实测；round-5b 场次 decoder=0 提示解码跟得上，但当时主程被重采样占用）
4. 720p 回归（点播/转码直播间 surface 与上传行为不变）

**遗留（round-7 候选）**：点播 720p 场次的间歇掉帧爆发（14:27:04–18、14:28:13–21，mpv 出帧降至 7~9fps；同窗 `heap-slow` 翻倍、UI build p95 57~197ms、`surfaces created 18/2s` 纹理换页）——未归因。

## 15. 借鉴 Xbox 360 YouTube 客户端分析的优化（2026-10-04）

来源：`H:\youtube\分析报告_Xbox360_YouTube_播放链路_性能与FFmpeg移植.md` + IDA 实证（`H:\youtube\default.xex.i64`）+ XDK 文档（xbox360-sdk-doc skill）。三项实施均限 Xbox 360 路径，全部复用既有 `[x360-perf]` 探针做硬件 A/B。

### 15.1 报告主张的 IDA/SDK 复核结论（先验证后借鉴）

| 报告主张 | 复核 | 结论 |
|---|---|---|
| 媒体线程静态钉核 + 高优先级 | `xav::Player` 构造 `0x837D3ED8` 尾调 `sub_83303508(-2,4)`（钉 hw4）+ `sub_83303378(-2,15)`（优先级，15→16 映射）；线程创建包装 `sub_83369D48` 查表 `dword_83AD3F28={4,4,4,4,4,3,4,4,5,4,4,4,4,0,1,2}`（媒体 hw4/5，UI hw0-2） | 属实 |
| dcbt 软件预取 | 报告统计 296 处；SDK prefetch 教程同样给出 `__dcbt` 模式 | 属实，可借鉴 |
| 128B 对齐媒体内存池 | 报告 §3；Xenon L1/L2 缓存行 = 128B | 属实，可借鉴 |
| 后台/媒体线程优先级策略 | XDK `Scheduling_Priorities`：所有线程 NORMAL 创建，CPU 密集后台线程应 BELOW_NORMAL/LOWEST；`Priority_Inversion`：**内核无反转缓解**，等待方必须阻塞而非自旋，且不可让中优先级线程饿死持锁低优先级线程 | 只降后台、不升前台（避免无缓解的反转风险） |
| 2MB 网络读缓冲 | wiliwili 侧 mpv demuxer cache 实测已生效（日志 `cache 145.0s`/直播缓存增长） | 已覆盖，不改 |
| 硬解零拷贝（`eglSetPlayerLBSHELL` + `XamLoadExtraAVCodecs2`） | 报告 §3.2/§5 | 留作 **M7 评估**，本轮不动 |
| VMX128 SIMD | cbffmpeg 侧（P6 长期项，AltiVec/Xenon 保持禁用） | 留作 cbffmpeg 侧 |

线程现状盘点（本轮之前）：main（hw0，UI+mpv render+上传）、borealis `task_loop`（继承 hw0，NORMAL）、cpr 池 hw{1,3,5} BELOW_NORMAL（既有先例）、libmpv/ffmpeg pthread spread `{2,4,1,3}`（NORMAL，cbffmpeg `pthread.c`）、弹幕 mongoose/task（继承 hw0，NORMAL）、看门狗（hw0，NORMAL，必须能在主程卡死时抢到片 → 不动）、mpv 线程**无任何优先级设置**。

### 15.2 本轮实施（3 项）

1. **`uploadPlane` 软件预取**（`library/borealis/.../xbox360_yuv.cpp`）：超前 2 行对 src 与 dst 各按 128B 发 `dcbt`（边界 `off+128<=row`，绝不越行）。动机：1080p 帧 3MiB ≫ L2，行拷贝逐行冷 miss；dst 预热替代 dcbz（避免行尾/对齐踩踏风险）。
   - **A/B 指标**：`[x360-perf] yuv plane upload` avg/p95（round-6 基线：720p 1350KiB/帧 11.6~13.4ms、1080p 3049KiB/帧实测待补）。若回归 → 先退成"仅 src 预取"再评估。
2. **帧缓冲 128B 对齐**（`mpv_core.cpp setFrameSize`）：基址对齐 32→128（`malloc(+127)`），pitch 取整 64→128；仍满足 swscale 的 32B 约束，chroma = pitch/2 一致性不变（已核 renderer/pre-fill/`requiredBytes` 三处公式）。
   - **A/B 指标**：同上 `yuv plane upload`；`setFrameSize` 日志 pitch 变化（如 1216/1088 类宽度）。
3. **后台线程静态分区**（YouTube 表驱动钉核 + XDK 后台降级）：borealis `task_loop`、直播弹幕 mongoose/task 四条线程 → `XSetThreadProcessor(hw5)` + `THREAD_PRIORITY_BELOW_NORMAL`（mpv spread 不用 hw5；cpr 池已在 hw{1,3,5} 且同为 BELOW_NORMAL，同级轮转无饿死路径）。
   - **动机**：这些线程此前继承 hw0，与主线程 20ms 时间片轮转——正是 §14.6 "锁持有者被时间片抢占"的同类形态，且落在 round-7 候选（UI build p95 57~197ms）的嫌疑面上。
   - **A/B 指标**：`ui build` p95、round-7 掉帧爆发窗口是否收敛；线程映射无启动日志可查（实测确认，见 §15.5；建议下轮在 setup 后补一行 DbgPrint）。

### 15.3 未采纳/后续候选（需单独 A/B）

- **cbffmpeg pthread 优先级分层**（如 AO/core 高于解码 worker）：需重建 cbffmpeg（P4 归档，见 `cbffmpeg/docs/wiliwili_mpv_embed.md` §4）→ 候选。**round-8 已实施并 A/B：无收益无害，候选关闭（§15.6）**。
- **cpr 池 {1,3,5} 与 mpv spread {2,4,1,3} 在 hw1/hw3 重叠**：现状 cpr 已让位（BELOW_NORMAL）；若 round-6/7 显示 HTTP/图片延迟 → 候选改 {5,5,?} 或调 spread 表（后者需 cbffmpeg 重建，且 P2 实测 `{2,4,1}` 为解码最优，不可轻动）。
- **主线程/解码线程优先级提升**：YouTube 将媒体线程升到 15，但其媒体线程独占 hw4/5；wiliwili 解码线程与其它 mpv 线程同级共享 hw1-4，且内核无反转缓解 → 无实测证据前不升。
- **硬解零拷贝**：M7 评估（报告给了完整机制与地址）。

### 15.4 构建环境修复与 XEX 构建验证（2026-10-10）

WSL 重置后本机构建环境迁移（工具链 `/home/lbx/*` → 现址），两处修复：

1. **`scripts/build_xbox360_local.sh` 路径本机化**：`REPO=/mnt/h/wiliwili`、`OXDK_DIR=/mnt/h/OXDK`、`XDK_DIR=/root/xdk360`（本机 XDK 根即此，无 `/XDK` 后缀）、`OXDK_LLVM=/root/oxdk-llvm/build`、`BILI360_DIR=/mnt/h/bili360-main`；configure 前自动清理含 `/home/lbx` 的陈旧 CMake 缓存（`CMAKE_HOME_DIRECTORY`/编译器/oxdklink 链接规则被冻结，无法原地重配）。
2. **`toolchain.cmake` try-compile 修复**：`-include stdint.h` + XDK 头在 `CMAKE_<LANG>_FLAGS` 里，但 `-D_WIN32/-fms-extensions/-fshort-wchar/-ffreestanding` 等只在 `add_compile_options`——**try-compile 只继承前者**，编译器/ABI 检查拉进 XDK CRT 头后报 `crtdefs.h: Only Win32 target supported` 等 16 错。修复：Xenon flag 集合提取为 `XBOX360_COMMON_FLAGS`，同时注入 `add_compile_options` 与 `CMAKE_C_FLAGS/CMAKE_CXX_FLAGS`，检查与真实编译同参（10-09 已证真实编译 flag 可过）。手动复现验证：同命令 + 补加 flag → 编译通过；不加 → 原样 16 错。

验证结果（10-10 18:35）：424/424 目标全部构建成功，产出 `build-xbox360-local/wiliwili`（24,457,216B，cxex 头部完整；部署时改名）。round-7 三项改动均已编入：`xbox360_yuv.cpp.o` 反汇编确认 `dcbt`×2（src+dst），`mpv_core.cpp.o`/`thread.cpp.o` 编译无警告。10-09 的 GLint 构建失败确认为配置漂移（缺 `-DMPV_SW_RENDER=ON`，落到 mpv_core.hpp 的 GL 分支），与代码无关。

### 15.5 硬件 A/B 实测记录（round-7，2026-10-10）

**部署与采集**：`build-xbox360-local/wiliwili`（XEX2 魔数核验）→ `devkit:\wiliwili\wiliwili.xex`，`xbdm magicboot title="DEVKIT:\wiliwili\wiliwili.xex" directory="DEVKIT:\wiliwili"` 启动（日志确认 `Loaded EXE wiliwili.xex (\Device\Harddisk0\Partition1\DEVKIT\wiliwili\wiliwili.xex)`）；`xbdm.py 192.168.2.9 listen` 采集。日志：`logs/round7_20261010_xbdm_listen.txt`（启动段）、`logs/round7_20261010_xbdm_playback.txt`（播放+退出段）、截图 `logs/round7_20261010_home.bmp`。采集注意事项：listen 必须带 `PYTHONIOENCODING=utf-8`（标题推 UTF-8 中文行时 Windows GBK stdout 直接 `UnicodeEncodeError` 崩掉采集）。

**结果（720p 1350KiB/帧，约 2.5 分钟播放场次，20:21~20:28）**：

| 指标 | round-6/7 基线 | round-7 实测 | 结论 |
|---|---|---|---|
| `yuv plane upload` avg/p95/max | 11.6~13.4ms | **1.64~1.75 / 1.94~1.96 / ≤2.08ms，0 lock fails** | ≈7× 提升，无回归，**不回滚** |
| 播放期 UI build p50/p95 | 爆发窗 p95 57~197ms | 18.2~20.5 / **24.3~25.8ms**（max ≤69.6） | 收敛 |
| 掉帧爆发窗（7~9fps 自发） | 14:27/14:28 两段 | **无**；仅 20:26:22-29 一窗 12/16/6fps，同窗 sync other 40.14ms/frame、tasks 1，与用户在播放上叠开设置/OSD 同时，瞬时恢复 | 目标签名未复现 |
| 首页稳态 | 60fps | 60fps（121帧/2s），build p95 13.9 | 正常 |
| 退出 | — | 20:28:26 `Exiting...` 干净退出回 xshell，无 abort | 正常 |

**已知残留（非本轮目标）**：
- `heap-slow` av_malloc/av_free 5~17ms 播放期仍现（P0 §5.11 堆锁争用，round-5 探针）。
- 退出确认框停留期（20:26:58~20:28:26）恒定 38fps / build 25.2ms——底页每帧全量重建，独立候选。
- 启动首窗 `slow sync task 1368ms from rcmd`（冷启动推荐流首批，既有特征）。

**限制与归因**：7× 为三项改动（dcbt 预取 + 128B 对齐 + hw5 分区）的合并效果，未做单项消融；线程亲和性无日志可核（§15.2 第 3 项），建议下轮在 task_loop/弹幕线程 setup 后补 `DbgPrint("[x360-thread] tid %lu processor %ld")` 一行以复核。1080p upload 基线已于 2026-11-02 补齐（3038 KiB/帧 → 4.6~5.0ms，见 §15.8）。

### 15.6 round-8：pthread 优先级分层 + 线程探针（2026-10-10，候选关闭）

**实施**：cbffmpeg `compat/xbox360/pthread.c` 基于线程名的降级分层（mpv 以 `mpv/<role>` 命名线程，HAVE_GLIBC_THREAD_NAME=1）：`mpv/vdec`/`mpv/adec`/`mpv/worker`/`mpv/demux` → BELOW_NORMAL，`ao`/`vo`/mpv core 保持 NORMAL；只降不升（XDK `Priority_Inversion` 无内核缓解）。配套 `[x360-thread]` DbgPrint 探针（create fn/pin、set-name 分层结果）+ borealis task_loop 与 `danmaku_live` hw5 探针（§15.5 建议的亲和核验随之补齐，文件作用域 `extern "C" DbgPrint` 声明——本 clang 不接受块作用域/lambda 内 linkage spec）。兼容层单文件增量重建：`cbffmpeg/scripts/mpv_x360_compat_archive.sh` 只重编 pthread.o 并重归档，无需重建 cbffmpeg。

**实机发现**：`vdec`/`adec` 分层从未触发（0 行）——解码跑在未命名线程上（61 个线程中 30× fn=8253BC6C 疑似 avcodec frame worker）；实际只 demote 了 `mpv/demux`×12 + `mpv/worker`×3。PIX 计数器与 round-7 基线无可测差异（Thread0 满载、Thread1-4 43~76% idle、hw5 后台驻留 100% idle、GPU wasted 79% 为 SW 渲染架构预期）。

**A/B（round-8b）**：关分层单变量构建（探针保留，打印 `-> normal (tiering off, round-8b A/B)`）复播，症状逐项复现 → 证实同期出现的“缓存上不去/频繁缓冲”与分层无关（网络侧，见 §15.7）。

**结论：候选关闭**——无收益、无害。代码停在关分层态，探针保留（未来任何线程改动可用其核验构建身份、创建分布与亲和）。

### 15.7 “缓存上不去/频繁缓冲”网络侧诊断（2026-10-10 晚，非线程问题）

**现象**：21:19 起多个播放场次缓存停在 0~2s、频繁缓冲；同晚 20:21~20:28（round-7）场次完全干净。同为 720p 1350KiB/帧。

**排除客户端回归**：round-8b 关分层构建与 round-7 干净场客户端仅差分层开关（已证无关），期间无任何其它代码变更 → 指向外部。

**根因定位（KDNet 内核采集，`kdnet.py start`，22:02 起；比 xbdm listen 稳定——后者曾静默中断）**：

- **CDN 边缘分配差异**：round-7 视频 `BV1by411i7XP` 走 `upos-sz-mirrorhwb/hwo1.bilivideo.com`（健康）；21:19 后场次被分配 `b-*.edge.mountaintoys.cn:4483`（116.207.128.136）/`upos-sz-estgoss.bilivideo.com`（117.185.238.23）——慢边缘。
- **链路画像 ≈600~700kbps 总带宽**：视频流仅 1.08× 实时（22:03 场次 2 分半缓存只到 8s，约十五分钟才爬到 41.6s）；音频流仅得 ~71kbps（30280 档需 ≥192k）→ 连接长时间无字节，空闲 ~120s 被服务端掐断（22:04:25→22:06:25 恰 +120s）→ `tcp: ffurl_read returned 0xffffff76` → `Will reconnect at <offset>` 续传（偏移 8.8→12.3MB 连续推进、缓存不回退——**mpv 重连/续传机制工作正常**）。21 点档每 ~2min 一次 `mbedtls_ssl_read returned -0x0`（服务端 FIN）同属节点行为。
- 疑问残留：同一慢节点上视频可用而音频被饿，带宽分配机制未深究；“晚间 ISP/线路拥塞 vs 单节点劣化”最终判据（重播 BV1by411i7XP 对比宿主与填充）未执行。

**客户端现状与后续候选（均需另开 A/B）**：wiliwili 不发任何 CDN 选择参数（playurl 仅 `fnval`+WBI，`video_detail_api.cpp`）；DASH 画质切换不重新请求 playurl（`player_base_activity.cpp:447`），重进视频才重新分配节点。候选：① 检测持续低速→提示/自动重进换节点；② 网页播放器式 upos 宿主改写（改写点 `player_base_activity.cpp:690` 的 `base_url`）；③ 提高 `INMEMORY_CACHE` 选项上限（`config_helper.cpp:252` 现 {0,1,5,10}MB）以吸收慢速滴流。

**日志**：`%LOCALAPPDATA%\xbox360-tools\kdnet\kd.log`（round-8b 场次）；round-7 干净场 `logs/round7_20261010_xbdm_playback.txt`。



### 15.8 1080p 上传病理：pitch 对齐（2026-11-02，已实施）

**背景**：§15.5 的 720p 上传已降至 1.6~1.75ms，但 1080p 源（surface 1920×1080，
3038 KiB/帧）实测 4.59~5.0ms，约 660 MB/s，比 1440×1080（2278 KiB → 1.48ms，
~1540 MB/s）按字节慢 2.3×。本节点补齐该尺寸的基线并归因。

**探针（`xbox360_yuv.cpp`，`RUNTIME_PROBES=1`）**：`uploadPlane` 一次性打印
`locked.Pitch` vs `sourcePitch` 与所走分支，flush 增加 lock/copy 拆时和 bulk/row 计数：

```text
[x360-perf] yuv plane pitch: source 1920 locked 2048 (1920x1080) -> row path
[x360-perf] yuv plane pitch: source 960  locked 1024 (960x540)   -> row path
[x360-perf] yuv upload split: lock 0.38 ms, copy 46.71 ms, bulk 0 plane(s), row 30 plane(s)
```

**归因**：Xenon D3D9 驱动对线性 L8 纹理的 `LockRect` pitch **按 256 B 向上取整**
（实测 896→1024、448→512、1920→2048、960→1024）。`setFrameSize` 的旧公式按 128 B
对齐，于是 1920/960 两档源 stride 小于纹理 pitch，`uploadPlane` 落回逐行拷贝
（2160 次/帧，~660 MB/s）；1440/1280 等 256 整数倍宽度则保持整平面单次拷贝
（~1540 MB/s）。**不是缺向量化**——逐行路径同样走 `XMemCpyStreaming_WriteCombined`。

**对策（Option S）**：`Xbox360YUVRenderer::planePitch(w,h)` 建好该尺寸平面后
`LockRect` 读回真实 pitch；`MPVCore::setFrameSize` 用它作为 mpv 的
`MPV_RENDER_PARAM_SW_STRIDE`（失败时回退 128 B 公式）。mpv 按新 stride 写，色度
stride 仍取 `stride/2`（`libmpv_sw.c` 既有约定），因此 1920→2048 同时使色度
960→1024 命中，三平面全部 bulk。代价：I420 缓冲 3110→3318 KiB（+6.6%），
每次尺寸切换多一次 `LockRect`。

**实测（同机、RUNTIME_PROBES=1，1080p 播放稳态）**：

| 指标 | 修复前 | 修复后 |
|---|---|---|
| `yuv plane upload` avg/p95/max | 4.67~5.00 / 4.91 / 4.95 ms | **2.01~2.10 / 2.15~2.26 / ≈2.5 ms** |
| 分支 | row 3 平面 | **bulk 3 平面** |
| copy / lock | ~4.3 / ~0.35 ms | ~1.9 / ~0.4 ms |
| 有效吞吐 | ~660 MB/s | ~1700 MB/s（含 padding 3240 KiB） |

同轮 800×450 窗口回退面也由 row(1.32ms) 变 bulk(0.39ms)。画面经截图核验
（`logs/pitchfix_play.bmp`）无色偏/色度偏移。**每帧省 ~2.6ms 主线程**。

**逐尺寸基线（2026-11-02 采集，RUNTIME_PROBES=1）**：

| surface | 上传字节 | 修复前 | 修复后 |
|---|---|---|---|
| 800×450 | 527 KiB | 1.02 ms（row） | 0.39 ms（bulk） |
| 854×480 | 599 KiB | 0.64 ms（row） | （按同一规则变 bulk，未复测） |
| 960×720 | 1012 KiB | 1.24 ms | 1.24 ms（pitch 不变） |
| 1280×720 | 1350 KiB | 1.62 ms | 1.62 ms（Y bulk / 色度仍 row：1280 是 256 倍数但 640 不是） |
| 1440×1080 | 2278 KiB | 1.48 ms（bulk） | 1.48 ms（pitch 不变） |
| **1920×1080** | **3038 KiB** | **4.59~5.00 ms（row）** | **2.01~2.10 ms（bulk）** |

**遗留**：宽度为 256 整数倍、但一半不是（1280、640、1792…）时色度平面仍逐行拷贝；
若需消除，须把纹理建为 pitch 宽并同步缩放取样 UV（色度、亮度各一套），属可选后续。

**同轮另两项（已实机验证）**：① `libmpv_sw.c` 条件化清屏——整面覆盖时跳过
`sw_clear_black`（clear 1.83→0.00ms）；② `vd-lavc-skiploopfilter=nonref` +
`vd-lavc-fast`。两项后 mpv render 调用 4.9~5.2→3.14~3.24ms（1080p，copy/sws 30/0）。

**范围外**：本会话观察到的偶发 VO 掉帧爬升 / avsync 漂移 / 音频欠载为既有网络无关
问题，重启应用即恢复，不在本节修复范围内。

### 15.9 1080p 音画不同步：根因是解码吞吐，对策 CATCHUP（2026-11-02）

**现象**：§15.8 的上传优化落地后，1080p 仍「音画不同步、画面明显拖慢」。用户复测复现。

**排除渲染路径**：改前（row path，sync 队列延迟仅 10 ms）与改后（bulk，upload 2.05 ms）
avsync 增长速度一致；mpv render 2.6 ms + 上传 2.05 ms + 提交 0.3 ms ≈ 5 ms/帧，
远低于 33 ms 预算。上传从来不是这个症状的原因。

**量化方法**：`avsync` 的斜率直接给出视频时钟倍率
（`video_clock = 1 - d(avsync)/dt`；音频实测 1.0×）。`est-vf` 只是容器帧率，
`decoder 丢帧 0` 也不代表解码跟得上 —— 两个都不能用来判断解码能力。

**运行时扫点（`game:\x360_play.cfg`，同一段 1080p30，30 s 窗口）**：

| 配置 | video_clock | 推算解码 | VO 丢帧/s |
|---|---|---|---|
| 3 worker, vmx=1（用户当时配置） | 0.51× | ~15 fps | 8.3 |
| 5 worker, vmx=1 | 0.56× | ~17 fps | 10.0 |
| 6 worker, vmx=1 | **0.66×** | **~20 fps** | 11.1 |
| 6 worker, vmx=0 | 0.65× | ~20 fps | 12.1 |

- **线程数**是当前最有效的运行时杠杆：6 worker 比 3 worker 快约 30%。
- **VMX 内核开/关在整机内测不出差异**：与 P6 §9.1 独立 harness 的 +6.6%
  （14.04→14.97 fps @3t）一致，落在播放噪声里。chroma/qpel/IDCT 不是 1080p 的瓶颈。
- 即便最优配置也只有 ~20 fps，**1080p30 全帧解码在这套 CPU/解码器上不成立**
  （参考播放器对 H.264 同样靠预设丢非参考帧，见其 REPORT_zh §9）。

**对策：CATCHUP（丢非参考帧保同步）** —— 对齐参考播放器的 H.264 preset 2：

- `mpv_core.cpp` Xbox `LOW_QUALITY` 分支新增 `vd-lavc-skipframe=nonref`
  （与既有 `vd-lavc-skiploopfilter=nonref` 并存）；H.264 解码器跳过非参考图像，
  解码量约减半，视频时钟跟上音频，运动表现为跳帧而非慢放。
- 新增运行时开关（`game:\x360_play.cfg`）：`skipframe=no|bidir|nonref|all`、
  `skiploop=nonref|all`，便于不重编 A/B。

**A/B（6 worker，同一段 1080p30）**：

| 配置 | video_clock | avsync 斜率 | VO 丢帧/s |
|---|---|---|---|
| `skipframe=no`（基线） | 0.80× | +0.197 s/s | 7.6 |
| `skipframe=nonref` | **1.00×** | **-0.004 s/s** | **0.2** |
| `skipframe=nonref` + `skiploop=all` | 0.97× | +0.029 s/s | 9.8（该场次 cache 掉到 5.5 s，网络侧干扰） |

结论：`skipframe=nonref` 是唯一把音画拉回同步的配置（斜率≈0），且不再有 VO 丢帧；
再加 `skiploop=all` 无额外收益。**保持 `skiploop=nonref`（画质优先）+ `skipframe=nonref`。**

**配套配置**：`game:\x360_play.cfg` 建议 `vd_threads=6`（原 3 为历史实验值）。
`spread=2,4,3,5` 维持不变：六项表 `{1,3,5,2,0,4}` 在播放器内会与主线程抢 hw0。

**遗留**：丢非参考帧后实际呈现约 8 fps（跳帧观感）；若要同时提高呈现率，
只能继续提升解码吞吐（P6 未覆盖的 CABAC 等），不在本节点范围。

### 15.9 1080p 音画不同步：解码吞吐天花板与 CATCHUP（2026-11-02）

**症状**：1080p30 H.264（`h264 1920x1080 30.000fps`）画面明显慢放，`avsync` 单调增长，
越播越不同步。§15.8 的上传优化（4.6→2.0 ms）与此无关：改前/改后的 avsync 斜率相同。

**定量（RUNTIME_PROBES=1，由 `[x360-mpv]` 的 avsync 斜率反推视频时钟，音频恒为 1.0×；
解码吞吐 ≈ 视频时钟 × 30）**：

| 配置（spread=2,4,3,5） | 视频时钟 | VO 丢帧/s |
|---|---|---|
| 3 线程, vmx=1（原 cfg） | 0.51×（≈15 fps） | 8.3 |
| 5 线程, vmx=1 | 0.56×（≈17 fps） | 10.0 |
| 6 线程, vmx=0 | 0.65×（≈20 fps） | 12.1 |
| 6 线程, vmx=1 | 0.66×（≈20 fps） | 11.1 |
| 6 线程, skipframe=no（稳态段） | 0.80×（≈24 fps） | 7.6 |

- **`decoder 丢帧 0` 不等于解码跟得上**：它只统计解码器主动丢弃的帧；`est-vf 30.00`
  也只是容器帧率估计。真正的判据是 avsync 斜率/视频时钟。
- VMX 内核（chroma MC / qpel / IDCT 已接入，日志 `x360 h264 ...: VMX128 kernels on`）
  开/关在同一场次无差异；P6 独立 harness 也只有 14.04→14.97 fps（3 线程，+6%）。
  即这些内核不是 1080p 的瓶颈，继续手写同类内核收益有限。
- 线程数是最有效的廉价杠杆：3→6 线程视频时钟 0.51→0.66×。

**对策 CATCHUP**（对齐参考播放器的 H.264 第二档预设）：`vd-lavc-skipframe=nonref`
不解码非参考帧，并叠加 `vd-lavc-skiploopfilter=all` 关闭去块滤波，视频时钟跟随音频。
同一场次 A/B（6 线程，`x360_play.cfg` 覆盖）：

| 配置 | avsync 斜率 | 视频时钟 | 呈现帧率 | VO 丢帧/s |
|---|---|---|---|---|
| skipframe=no, skiploop=nonref | +0.197 s/s | 0.80× | 6.3 fps | 7.6 |
| skipframe=nonref, skiploop=nonref | +0.177 s/s | 0.82× | 5.6 fps | 5.4 |
| **skipframe=nonref, skiploop=all** | **+0.002 s/s** | **1.00×** | 6.6 fps | **0.5** |
| skipframe=bidir, skiploop=all | +0.000 s/s | 1.00× | 5.8 fps | 1.1 |

关键发现：**只丢非参考帧不够**（0.82×），参考帧的去块滤波才是最后 ~15% 解码时间；
两者叠加才能把时钟拉回 1.00×。画面变为跳帧（约 6–8 张/秒，时间轴正确），音画完全
同步；这是本硬件上 1080p30 的唯一可行形态——参考实现同样对 H.264 提供 CATCHUP
（`skip_frame=NONREF` + `skip_loop_filter=ALL`）预设。`bidir` 变体同样同步且丢帧更少，
保留为 cfg 可选项。

**解码吞吐侧的其它尝试（2026-11-02）**：

- **切片线程不可用**：`vd-lavc-o=thread_type=slice`（cfg `avopts=` 可配）在
  `BV1TP411Y7bt` 1080p 上视频时钟掉到 **0.49×**——该码流切片数不足以并行，FFmpeg
  默认的帧线程（`slice|frame` → frame 优先）才是正确选择。
- **4 worker 基线复测**（纯净 cfg）：avsync 0.00→0.01 / 65 s，起始 1.56 s 的偏移
  被完全追回，0 次欠载，视频时钟 1.00×。
- **`skipframe=bidir` 对照**：同样同步（斜率 0.001，VO 掉帧 0.3/s），但呈现帧率
  5.4 fps，低于 `nonref` 的 6.3 fps（`est-vf` 10–14 vs 18–20）——所以默认仍用
  `nonref`，bidir 仅作为 cfg 可选项。
- **weighted prediction VMX128 已接入**（`libavcodec/xbox360/h264_weight_vmx.c`，此前
  写好未接入；`ff_h264dsp_weight_init_x360()` + 运行时自检 + C 回退，`libavcodec.a`
  已重建）。`BV1TP411Y7bt` 实测：呈现 6.4 vs 6.3 fps、`est-vf` 相当、同步无变化
  → 该码流几乎不用加权预测，收益在噪声内；保留（自检保护、零实测代价，对使用加权
  预测的码流才有意义）。
- **结论**：配置层（线程/绑核/切片/跳过策略）与现有 VMX 内核的吞吐挖掘到此为止；
  再往上需要 CABAC 熵解码级别的剖析与改写（尚未开始，工作量与风险都大）。

**与参考播放器的同文件对比（2026-11-02，排除内容差异）**：参考播放器
（`H:\Xbox360MediaPlayer.Alpha10-ZorgX`）的日志显示它解码 `KontonBoogie.mp4`
（1920x1080 H.264 High L4.0，**refs=1**，2.55 Mbps）为 `decoded_fps=30.10`、
`preset=0`（全帧 + 全去块）、5 worker。用我们的解码器（`tests/p2_decode`，
FFmpeg 4.4.8 + 现有 VMX 内核，5 线程，spread）解码**同一个文件**：
4639 帧 / 155.2 s = **29.9 fps（稳态窗口 32.6 fps，0 错误）**。
→ 两边解码能力相当；B 站 1080p 流（refs/码率更高）在播放器里全帧只有 ~17–20 fps
是**内容更重**，不是解码器落后。参考播放器的 30 fps 结论不可外推到 B 站流。
`vc`：p2_decode 重新链到 `build/ffmpeg-x360-tls/libavcodec.a`（含 weight VMX）。

**两条流的 SPS 对比（`[x360-sps]` 探针，2026-11-02）**：

| | profile | level | max_num_ref_frames | 尺寸 | 播放器内全帧解码 |
|---|---|---|---|---|---|
| 参考播放器的本地 mp4 | High (100) | 4.0 | **1** | 1920×1080 | 我们的解码器 29.9 fps（harness） |
| wiliwili 播的 B 站 DASH 流 | High (100) | 5.0 | **8** | 1920×1088 | ~17–20 fps |

**refs 消融实验（2026-11-02）**：把同一素材用 x264 重编码为 High@L5.2、`refs=8`、
2.29 Mbps（trace_headers 确认 `max_num_ref_frames=8`）：

| 被测对象 | 原文件（refs=1, 2.55 Mbps） | 重编码（refs=8, 2.29 Mbps） |
|---|---|---|
| 参考播放器 | 30.10 fps | **30.10 fps**（0 丢帧） |
| 我们的解码器（p2_decode harness，5 线程） | 29.9 fps（155.2 s / 4639 帧） | **31.6 fps（146.8 s / 4639 帧）** |

→ **refs 不是瓶颈**：两边在同码率下都能 30 fps，refs=8 甚至略快（编码细节差异）。
B 站流在播放器里只有 ~17–20 fps 的差异只能来自**码率/残差密度更高**（约 3× 本地文件）
与播放器内 UI 争核，而不是参考帧数量或解码器代际。要最终判定参考播放器能否吃下
B 站流，只能把 B 站码流录下来喂给它（`--stream-record`），目前未做。

**另两个必要条件（2026-11-02 追加实测）**：

1. **worker 数 4 + 绑核 `2,4,3,5`**：CATCHUP 只解参考帧后，工作量恰好装进 4 个
   worker 硬件线程，1:1 不超订，且全部避开核 0（主线程）。120 s 场次：
   avsync 0.01→0.40（斜率 0.003，视频时钟 1.00×），中途一次 7.6 s 的瞬态被**完全追回**
   （VO 掉帧 0.4/s）。对照：同 4 worker 但用 shim 内置 6 项表 `{1,3,5,2,0,4}` → 0.96×
   （worker 5/6 落到 hw0 与主线程抢核）；6 worker 超订 → 0.97×，~2 s/min 慢漂移。
   CATCHUP 档因此在 `MPVCore::init()` 里显式设 `vd-lavc-threads=4` 并调用
   `xbox360_pthread_set_spread_cpus({2,4,3,5})`；cfg 的 `vd_threads=`/`spread=` 仍可覆盖，
   非 CATCHUP 路径维持 6 worker 与 shim 默认表。
   端到端复测（同一构建、纯净 cfg）：avsync 0.17→1.86 / 121.7 s（斜率 0.014，视频时钟
   0.99×，VO 掉帧 0.9/s）；理想场次为 0.003（1.00×）。残余差异来自单次启动期音频欠载与
   场景复杂度波动，已比原 0.2 s/s（2 分钟漂 24 s）改善一个数量级。
2. **音频设备缓冲 1.0 s**（`--audio-buffer=1.0`，`ao_xaudio2` 按此分配设备环）：
   默认 0.2 s 时 1080p 启动期会出现 `Audio device underrun`，欠载会把音频时钟前跳，
   跳帧策略无法追回（实测 2 分钟携带 ~2 s 偏移）。1.0 s 场次欠载 0 次，播放期无感
   （只影响暂停/起播的音频起振）。

**落地（wiliwili 侧，运行时可覆盖）**：
- **必须作为 mpv 初始化参数设置**：运行时 `set vd-lavc-skipframe` / `mpv_set_property_string()`
  会被 mpv 接受、属性回读也正确，但解码器不采用（实测 avsync 仍 +0.18 s/s、
  `est-vf` 停在 18–22）。`mpvSetOptionString()` 在 `mpv_initialize()` 之前设置才生效。
- `MPVCore::init()` 按持久化的 `VIDEO_QUALITY`（>=80）设初值；`MPVCore::setSkipNonRefFrames(bool)`
  由 `player_base_activity` 在打开 DASH 视频时按 `v.height >= 1080`、FLV/直播按
  `quality >= 80` 请求；若请求值与当前 mpv 实例不同，`setUrl` 在 `loadfile` 前调用既有
  `restart()` 重建 mpv（每次清晰度切换至多一次，常见路径因持久化清晰度已匹配而不触发）。
- 720p 保持全帧（默认清晰度 64 不受影响）。
- CATCHUP 档同时设 `vd-lavc-skiploopfilter=all`（缺它只有 0.82×）、
  `vd-lavc-threads=4`（cfg 的 `vd_threads=` 仍可覆盖）与 `audio-buffer=0.5`。
- `game:\x360_play.cfg` 的 `skipframe=` / `skiploop=` 仍可覆盖门控，用于 A/B。
- 1080p 全帧解码在当前 CPU 上不可达；要全帧只能继续提解码吞吐（CABAC 等未做，
  收益与风险未知）。

### 15.9 补遗：REFCACHE 负结果与解码 worker 绑核表（2026-11-02 实施，结论沉淀）

- **REFCACHE（逐切片参考进度缓存）无收益**：缓存命中可跳过
  `ff_thread_await_progress` 的互斥量+条件变量，但 A/B 实测 16–18 fps
  （开）≈ 17 fps（关）——**互斥量等待不是瓶颈，帧依赖链才是**。代码保留在
  `h264_mb.c`（`x360_ref_cache_*`，零害），不要在此基础上继续投入。
- **解码 worker 绑核表**：pthread 兼容层把 `lavc/frame` 线程钉到
  `{1,3,5,2,4}`（每核第二硬线程优先，避开 hw0 的主线程），BELOW_NORMAL 优先级。
  试过 `{1,5,2,4,0}` 会与主线程抢 hw0，整体掉到 ~13 fps（回退）。
  播放器内 CATCHUP 档覆盖为 4 项 `{2,4,3,5}`（1:1 不超订，见 §15.9）。

### 15.10 CABAC/重建热点剖析与多块 IDCT 分发修复（2026-11-03）

**问题**：REFCACHE/绑核挖掘尽后，1080p 全帧解码仍停在 ~20 fps（p2 harness,
bbb_1080, 3 worker）。剩余差距在帧内单线程临界路径的哪里？VMX128 覆盖了
CABAC 解码和残差/帧内重建内核吗，还是标量兜底在主导？

**仪器（持久化，见 `XBOX360_RUNTIME_KNOBS.md` §3）**：`prof=1` 开关下，
libavcodec 的 H.264 解码器用 `mftb`（Xenon 上编译为单条 `mfspr`）按宏块
累积周期桶，每 10 帧 1080p 打一行 `[x360-h264prof]`：cabac（熵解码+MB 解析）、
pred（帧内重建）、mc（运动补偿，含 await/mcd/wt 子桶）、idct（残差重建）、other。

**静态发现（第一问的答案）**：

| 路径 | VMX128 覆盖 | 备注 |
|---|---|---|
| CABAC 熵解码 | **无**（全平台皆标量，`cabac_functions.h` 仅 x86/arm/mips 有汇编） | 逐 bin 串行，天然不可 SIMD |
| 残差重建 4x4 IDCT | 有（P6 M4） | **但此前多块 helper 绕过了它**，见下 |
| 帧内预测 pred4x4/8x8/8x8l/16x16 | **无**（`h264pred.c` 无 x360 接入） | P6 计划从未覆盖 |
| chroma MC / qpel / weight | 有（P6 M1/M2/M3） | — |
| 去块滤波 | CATCHUP 跳过 | — |

**多块 IDCT 分发绕过（本轮修复）**：`h264idct_template.c` 的
`ff_h264_idct_add16` / `add16intra` / `add8` / `add8_422` 在循环内**直接调用
静态 C 单块函数**，不走 `H264DSPContext` 函数指针——P6 M4 换上的 VMX 单块
内核只覆盖 intra4x4 路径，P/B 帧残差重建一直是标量。这正是当时
"IDCT 收益在噪声里"（P6 §9.2）的真正原因。修复：`xbox360/h264_idct_vmx.c`
新增四个多块包装（逐条复刻 C 的分发语义，含 `nnz==1` DC 快路径与 422 变体
的 i+4 索引怪癖），运行时自检对拍 C helper，失配回退；4:2:2 走同一
`h264_idct_add8` 槽位（init 收 chroma_format_idc 决定装哪个）。

**剖析数据（bbb_1080.h264 1080p60 High@L4.2 CABAC，600 帧，spread，skiplf=1）**：

| 运行 | fps | base (mftb/10帧/线程) | cabac | pred | mc | 其中 await | 其中 mcd | 其中 wt | idct |
|---|---|---|---|---|---|---|---|---|---|
| 3w prof | 19.3–20.3 | 71–74 M | 15% | 0% | 79% | 9–10% | 56% | 10% | 1% |
| 6w prof | 18.1 | 151 M | 12% | 0% | 83% | 34% | — | — | 1% |
| 3w vmx=0 | 11.6–15.9 | 89–121 M | 12% | 0% | 83% | 9–11% | 54% | 15% | 1% |

（TB=50 MHz：71M ticks/10帧 ≈ 142 ms/帧/线程 ≈ 900 ticks/宏块；mcd =
`mc_dir_part`（qpel/chroma 内核 + 边界仿真），wt = 加权预测胶水，均 mc 子集。）

**结论**：

1. **MC 才是临界路径（~80%），不是 CABAC（15%）**。CABAC 全标量属实，
   但即使完全消掉也只有 +18% 上限；帧内预测 ≈0%（inter 主导内容）；
   残差 IDCT 修复后也只 ~1%（该码流码率低、残差稀疏）。
2. **MC 是内存受限，不是标量兜底**：mcd 子桶在 vmx=1 下仍占 55%；
   6 worker 时每线程周期翻倍（71M→151M）且桶占比不变——共享资源
   （内存/L2）饱和，加线程不加吞吐。VMX 内核把 mcd/weight 压到 C 的
   1/1.4–1/2.2（**vmx=0 A/B 实测吞吐 -20~-45%**），但撞的是带宽墙。
   6-tap qpel 每宏块读参考行 ~3 帧面积，refs=8 时随机 MV 的工作集远超
   1 MB L2——软件侧无杠杆（prefetch_motion 已在）。
3. **"23→31 fps 差距"的前提不成立**：同文件对比（§15.9 refs 消融）我们
   31.6 fps vs 参考播放器 30.10 fps（refs=8 重编码）——**同内容下解码能力
   相当甚至略快**。差距来自 B 站码流内容更重（两边都掉：参考播放器在
   B 站流上也只有 17–20 fps），不是解码器落后。
4. 旧结论"VMX 内核开/关无差异"（§15.9）**已被更严格的 harness A/B 推翻**：
   当时多块 IDCT 未接入（vmx=1 的收益面小），且播放器内测量噪声大。
   现在 vmx=0/1 = 11.6–15.9 vs 19.2–20.3 fps。**内核保留且是主要抓手之一。**
5. **接受为硬上限**：全帧 1080p30 在这套内存子系统上 ≈20–25 fps 封顶，
   与 worker 数无关；CATCHUP（丢非参考帧 + 跳去块）仍是 1080p 的正确形态。
   再往上只剩低期望值选项（emu_edge 拆分、bipred scratchpad 消除、
   参考帧布局），除非内容侧（码率/refs）变化，不再投入。

**顺带修复**：`h264_mb.c` 的 `#include "libavutil/atomic.h"`（树里不存在，
REFCACHE 遗留）改为 `<stdatomic.h>`；两处 `typeof` 改 `__typeof__`（该编译
选项下 GNU 扩展不可用）；`x360_h264_clear_ref_cache` 补原型声明——此前
工作区状态实际编不过，本轮一并修掉。

**整机播放流畅度验收（2026-11-03，`XBOX360_RUNTIME_PROBES=1` 构建，
BV1TP411Y7bt 1080p30 DASH，CATCHUP 默认档，UI 开评论区 50fps 在跑）**：

| 指标 | 实测 | 判定 |
|---|---|---|
| avsync（每个 2:34 完整播放段） | 0.000 → +0.002 / +0.005 s | 时间轴正确，视频时钟 1.00× |
| 音频欠载 | **0 次**（234 s 窗口） | ✓ |
| decoder 丢帧 | 0 | ✓ |
| VO 丢帧 | 0.45–2.8/s（CATCHUP 跳帧呈现形态） | 与 §15.9 基线一致 |
| est-vf | 18–22（只解参考帧） | 支撑时间轴 |
| 循环重播 | 2:34 内容 = 2:35 墙钟 × 多轮 | 逐轮验证 1.00× |

- avsync 孤立尖峰（0.7–1.9 s，出现在循环 seek 处）**当场完全追回**，不残留漂移。
- 解码吞吐实测（`[x360-dec]` 窗口）：平均 19 fps、10–30 波动；真实 B 站流
  与本地素材（§15.9 refs 消融）结论一致——**解码能力不输参考播放器**。
- 结论：1080p 在本硬件上的可行形态维持 CATCHUP（跳帧呈现 + 音画同步），
  本轮内核修复（多块 IDCT/weight）不改变形态，但 vmx 路径实测快 20–45%，
  为网络/渲染抖动留出更多余量。
