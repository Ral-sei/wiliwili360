# Xbox 360 UI 性能审查报告

> 状态：代码审查结论，线程亲和性分配方案暂不固定。
>
> 范围：针对 Xbox 360 实机 UI 卡顿、HUD CPU 峰值约 19% 的现象，审查当前
> borealis、cpr、图片加载和 NanoVG D3D9 路径，并结合本地 Xbox 360 XDK
> 21256.17 文档给出后续优化方向。

## 1. 结论摘要

当前 UI 确实由 borealis 主循环线程统一执行，这是 borealis 的线程安全模型；
View、Yoga 布局、NanoVG 命令生成、纹理创建和 Present 不能随意拆到多个线程。
但目前更严重的问题是：Xbox 360 版本创建的工作线程没有硬件线程亲和性管理。

XDK 默认不会把新线程自动迁移到其他硬件线程，新线程默认运行在创建者所在的
硬件线程。当前网络和图片请求都从 UI 线程发起，因此会与 UI 渲染线程挤在同一
个硬件线程上。Xbox 360 有 6 个虚拟处理器，单个硬件线程满载约等于总 CPU 的
六分之一；这与 HUD 中约 19% 的 CPU 峰值和明显掉帧现象相符。该百分比的确切
HUD 统计口径尚未在本地 XDK 文档中确认，因此这里将其作为高可信度推断，而不是
最终测量结论。

视频播放时还会额外加入 FFmpeg/播放器解码、网络读取、音频输出和视频纹理更新，
所以播放页比普通首页更容易暴露这个线程调度问题。截图中的 `FPS:7` 表明当前
至少存在严重的帧预算超时。截图中的偏红画面已确认是视频原始内容，本报告不再
将它作为颜色格式或 endian 问题处理。

## 2. XDK 文档依据

本报告引用的文档来自本地 Xbox 360 XDK 21256.17 快照。

### 2.1 硬件线程分配

- `xbox360docs/sample_ThreadSetProcessor.htm`
  - 内核不会自动在处理器之间迁移软件线程。
  - 创建线程时，新线程默认分配到创建者当前所在的处理器。
  - Xbox 360 有 6 个虚拟处理器，即 3 个核心、每个核心 2 个硬件线程。
- `xbox360docs/Kernel_Threads_Synchronization_and_Scheduling.htm`
  - 默认情况下，软件线程运行在创建者所在的处理器上。
  - 时间片大约为 20ms；在 60Hz 渲染中，一次不利的抢占就可能跨过一帧的
    16.6ms 预算。
  - 默认线程栈为 64KB，文档说明可以降低到至少 12KB。
  - 文档还说明处理器 2、4 等存在 XContent、音频、Marketplace 和 XAM 任务，
    因此线程分配不能只按 CPU 总百分比判断。
- `xbox360docs/XSetThreadProcessor.htm`
  - 使用 `XSetThreadProcessor` 将软件线程固定到指定硬件线程。
- `xbox360docs/GetCurrentProcessorNumber.htm`
  - 可返回当前线程运行的 0 到 5 号硬件线程，用于启动时记录实际线程布局。

### 2.2 同步、优先级和计时

- `xbox360docs/SetThreadPriority.htm`：可以调整指定线程的调度优先级。
- `xbox360docs/InitializeCriticalSectionAndSpinCount.htm`、
  `EnterCriticalSection.htm`、`LeaveCriticalSection.htm`：XDK 提供阻塞式临界区，
  可替代高竞争场景下的手写自旋锁。
- `xbox360docs/GetTickCount.htm`：系统计时器分辨率为 1ms；如果需要更高精度，
  应使用 high-resolution timer。
- `xbox360docs/High_Resolution_Timer.htm`：高精度计时可使用性能计数器。

## 3. 当前代码证据

### 3.1 Xbox cpr 异步请求是“每个请求一个线程”

`library/cpr/include/cpr/async.h:11-34` 的 Xbox 分支直接调用 `CreateThread`。
当前实现没有有界线程池，也没有保存或关闭返回的线程句柄：

```cpp
CreateThread(nullptr, 0, xbox_async_thunk<std::function<void()>>,
             worker.release(), 0, nullptr);
```

这会带来四个问题：

1. 首页或播放页批量请求时创建大量线程；
2. 这些线程默认继承 UI 线程的硬件线程；
3. 创建和销毁线程本身有额外开销；
4. 不关闭线程句柄会造成长期资源泄漏，默认 64KB 线程栈也应纳入预算。

`library/cpr/include/cpr/threadpool.h:4-24` 的 Xbox `ThreadPool` 是 no-op 空壳，
不能解决上述问题。

### 3.2 图片请求会放大线程争用

`wiliwili/source/utils/image_helper.cpp:101-117` 的 Xbox `ImageThreadPool` 最终
调用 `cpr::async`。`image_helper.cpp:207-215` 每个图片请求都可能启动一个独立
线程，线程中执行网络读取和图片解码。

图片解码已经不在 UI 线程，这是正确方向；但线程创建没有上限，且没有硬件线程
分配。视频页同时加载封面、头像、评论图片时，会和视频网络读取、解码线程共同
争抢 UI 所在硬件线程。

### 3.3 UI 线程仍执行纹理创建和 GPU endian 重排

`image_helper.cpp:295-330` 通过 `brls::sync` 回到 UI 线程，在回调中调用
`nvgCreateImageRGBA`。

`library/borealis/library/lib/extern/nanovg_d3d/src/nanovg_d3d9.h:480-511`
又会在纹理锁定后逐像素执行 `GPUENDIAN_8IN32` 字节重排。网络和解码虽然已经
移到 worker，但这部分 CPU 拷贝和纹理上传仍然在渲染线程中完成；多张图片在同一
帧完成时会形成明显的帧尾尖峰。

### 3.4 borealis 异步任务循环在 Xbox 上被关闭

`library/borealis/library/lib/core/thread.cpp:164-225` 在 `PLATFORM_XBOX360`
下让 `Threading::start`、`stop` 和 `start_task_loop` 直接返回。

因此 `brls::async` 任务没有执行线程。当前代码中的 analytics、battery/wireless
等调用可能只会把任务加入队列而不被消费。这个行为也使“borealis 异步任务”和
“cpr 网络线程”两套线程模型不一致。

### 3.5 自旋锁会放大同一硬件线程上的竞争

`library/borealis/library/include/borealis/core/xbox360_mutex.hpp:12-17`
使用 `InterlockedCompareExchange` 加 `Sleep(0)` 自旋。少量短临界区可以接受，但
当多个图片/网络线程同时访问队列或缓存时，竞争线程仍然会在 UI 所在硬件线程上
反复唤醒，增加调度和缓存压力。

### 3.6 Xbox 当前每帧都会完整执行 UI 和 Present

`library/borealis/library/lib/core/application.cpp:168-238` 每轮都执行输入、动画、
布局、渲染、同步任务和销毁队列；Xbox 分支还跳过了普通平台的限帧 sleep。

`application.cpp:748-816` 每帧重建 NanoVG 绘制命令并调用 `Present`。
`wiliwili/library/borealis/library/lib/platforms/xbox360/xbox360_video.cpp:31`
设置了 `D3DPRESENT_INTERVAL_ONE`，一旦一帧超过 16.6ms，显示节奏可能直接落到
下一个垂直同步周期，表现为 30fps 或更低的跳变。

## 4. 视频播放卡顿的专项判断

### 4.1 最可能的主因：播放线程与 UI 共用硬件线程

视频播放至少包含以下活动：

```text
UI / NanoVG / Present
        + HTTP / DASH 读取
        + FFmpeg 解码
        + 音视频时钟和队列
        + 视频纹理上传或更新
        + 图片和评论加载
```

如果这些线程都从 UI 线程创建且没有 `XSetThreadProcessor`，它们默认会落到同一
硬件线程。网络线程通常处于等待状态，但当数据到达时会执行 TLS、拷贝、解析或
队列操作；解码线程则可能持续占用 CPU。20ms 时间片足以让 UI 错过多个 60Hz
垂直同步点。

### 4.2 截图中的 `FPS:7` 说明需要区分三个时间

后续诊断必须同时记录：

1. **UI frame time**：输入、布局、NanoVG、sync queue、Present 各自耗时；
2. **视频解码时间**：每帧解码耗时、解码队列深度、丢帧数；
3. **视频显示时间**：纹理更新、GPU 等待、音视频时钟偏差。

仅看总 CPU 百分比无法判断是 CPU 解码慢、GPU 等待、Present 错过 vblank，还是
视频纹理更新阻塞。

### 4.3 颜色问题排除

截图中的偏红画面已经确认是视频本来的内容，因此不作为本次卡顿原因。后续性能
测试仍应保留一段颜色正常、运动复杂度适中的标准视频，避免仅使用单个视频的
内容特征判断解码性能。

### 4.4 已确认的播放路径瓶颈：Xbox 强制使用 CPU 软渲染

`scripts/build_xbox360_linux.sh:58-63` 明确传入：

```text
-DMPV_SW_RENDER=ON
```

这会选择 `MPVCore` 的 CPU 输出路径，而不是 D3D9 视频纹理或硬件视频输出路径。
对应代码如下：

- `wiliwili/source/view/mpv_core.cpp:233-257`：mpv render update 回调通过
  `brls::sync` 排队到 borealis 主线程；软件渲染模式下，
  `mpvRenderContextRender` 在主线程执行。
- `wiliwili/source/view/mpv_core.cpp:716-751`：为视频分配
  `drawWidth * drawHeight * 4` 的 CPU RGBA 缓冲区，并将它作为 mpv 软件输出目标。
- `wiliwili/source/view/mpv_core.cpp:817-826`：每次 `redraw` 时调用
  `nvgUpdateImage`，把整帧 CPU 图像上传到 NanoVG 纹理。
- `library/borealis/library/lib/extern/nanovg_d3d/src/nanovg_d3d9.h:524-579`：
  `nvgUpdateImage` 会锁定 D3D9 纹理并复制更新区域；Xbox 纹理路径还包含
  `GPUENDIAN_8IN32` 的逐像素通道重排。

当前播放路径近似为：

```text
FFmpeg 解码
    -> mpv 软件 RGBA 输出到 CPU buffer
    -> 主线程 mpvRenderContextRender
    -> 主线程 nvgUpdateImage
    -> D3D9 LockRect + 整帧 memcpy/通道重排
    -> NanoVG 绘制视频矩形
    -> Present
```

在 1280x720 下，每帧 RGBA 数据约为 3.52 MiB；如果按 60fps 更新，单纯的
CPU/GPU 纹理传输量就可能超过 200 MiB/s，还没有计入解码、颜色转换、NanoVG
和 UI 绘制成本。截图中的 `FPS:7` 与这条路径高度吻合。

这也解释了为什么总 CPU 百分比不一定很高：瓶颈可能是主线程在 D3D9
`LockRect`、统一内存同步、Present 等等待状态，而不是六个硬件线程都在执行计算。

### 4.5 视频渲染回调被排队到 UI sync 队列

`MPVCore::on_update` 无论视频帧更新还是 mpv render 状态变化，都使用
`brls::sync`。而 `Application::internalMainLoop` 在完成一轮绘制后才调用
`Threading::performSyncTasks`。

这会造成两个问题：

1. 视频帧到达频率高于 UI 消费频率时，sync 队列可能积压多个渲染更新；
2. `mpvRenderContextRender` 被当作 UI 更新执行，解码线程和 UI 线程之间没有
   明确的 latest-frame 丢弃策略。

播放路径需要使用“只保留最新帧”的原子标志或有界队列。不能让每个 mpv update
事件都在主线程排队，否则 UI 只是在逐帧追赶已经过期的视频帧。

### 4.6 当前 Xbox 硬件解码默认也没有形成有效路径

`MPVCore::HARDWARE_DEC` 默认值为 `false`（`mpv_core.hpp:363-364`）。即使
构建的 libmpv/FFmpeg 已经包含 Xenon 可用的解码能力，当前默认配置仍会在
`mpv_core.cpp:384-390` 设置 `hwdec=no`。

仅打开 `HARDWARE_DEC` 不能自动解决问题；如果 renderer 仍然是
`MPV_SW_RENDER`，CPU RGBA 上传瓶颈仍然存在。必须分别验证软件输出、D3D9
视频 renderer 和硬解输出路径。

### 4.7 cbffmpeg 已有实机证据：解码线程默认会全部落到一个硬件线程

这不是只基于 XDK 文档的推断。`../cbffmpeg/docs/p2_decode_run.md:91-128`
记录了 Xbox 360 真机强缩放测试：

- 未开启 spread/bind 时，1、2、4、6 线程的微基准 speedup 均约为 `1.00x`；
- 每个 worker 都打印为 `hwthread=0`；
- 开启 spread 后，4 线程微基准达到约 `2.15x`；
- 1080p 解码测试从 1 线程约 `6.16 fps` 提升到 4 线程 spread 约 `12.75 fps`。

`cbffmpeg/compat/xbox360/pthread.c:862-868` 已实现
`xbox360_pthread_set_bind_spread()` 对 pthread worker 调用
`XSetThreadProcessor`，但当前仓库中该开关只在 `cbffmpeg/tests/p2_decode` 测试
程序被调用，wiliwili 的 MPV 初始化路径没有调用它。

因此当前视频播放至少有两层串行化风险：

1. FFmpeg H.264 frame workers 默认全部落在同一个硬件线程；
2. 软件 RGBA 输出、`mpvRenderContextRender`、`nvgUpdateImage` 和 UI 绘制也会
   争用主线程。

这比“borealis UI 单线程”更具体，也比普通网络线程争用更可能解释播放时 FPS
直接降到个位数。优先级应当是先在 wiliwili 嵌入启动时开启已经验证过的 FFmpeg
worker spread，再测量 CPU 输出/纹理上传路径；否则后续 UI 优化会被单线程解码
瓶颈掩盖。

## 5. 优化优先级

### P0：先解决硬件线程争用

- 实现真正的 Xbox 有界 worker pool，替换每请求 `CreateThread`；
- 保存并关闭线程句柄；
- 为 worker 设置合理的栈大小；
- 为 UI、网络、图片、解码线程预留独立的硬件线程方案；
- 将线程亲和性方案作为独立决策，不在本报告中写死具体编号；
- 记录 `GetCurrentProcessorNumber()`，用实机日志验证线程确实分布到预期位置。
- 在 MPV/FFmpeg 初始化阶段调用 cbffmpeg 已实现的
  `xbox360_pthread_set_bind_spread(1)`，或提供等价的 wiliwili 平台初始化入口；
- 先用 1/2/4 个解码线程做 A/B 测试，确认解码 FPS 和 UI FPS 是否随 spread 改善。

### P0：限制 UI 线程上的视频和纹理工作

- 解码、YUV 预处理和 RGBA endian 重排放到 worker；
- UI 线程只负责受控的 GPU resource creation/update；
- 为纹理上传设置每帧预算，剩余任务进入下一帧；
- 视频纹理更新使用 bounded ring，避免 UI 等待解码线程或 GPU 完成；
- 记录每次纹理上传耗时和每帧上传数量。

视频播放还应优先完成以下验证：

- 建立 Xbox D3D9 视频 renderer，优先让 shader 直接采样 YUV 纹理并在 GPU 完成
  YUV 到 RGB 转换，避免每帧 CPU RGBA 全量上传；
- 在 renderer 未完成前，至少将 CPU 输出分辨率限制为实际视频区域，而不是默认
  按整屏 1280x720 分配；
- 将 mpv render update 改为 latest-frame 合并，不为每个 update 事件创建一个
  `brls::sync` 任务；
- 在视频页临时关闭评论图片和非必要 UI 动画，作为 CPU 上传路径的对照实验；
- 分别测试 `MPV_SW_RENDER=ON`、可用的 D3D9 renderer 和硬解配置，记录每种
  路径的 FPS、主线程耗时、解码耗时和纹理上传带宽。
- 单独记录 decoder frame drop 与 output frame drop；如果 decoder drop 很高，
  先修 FFmpeg worker 分布，如果 output drop 很高，再修主线程纹理上传/Present。

### P1：恢复一致的任务调度模型

- 给 `Threading::start/stop` 实现真实的 Xbox worker；
- 或明确将 `brls::async` 统一路由到新的 Xbox worker pool；
- 用 XDK `CRITICAL_SECTION` 或事件对象替代高竞争自旋锁；
- 对 sync queue 设置单帧时间预算，避免大量 UI 更新集中在一帧执行。

### P1：建立可重复的性能测量

- 增加 frame、input、layout、NanoVG、sync、Present 的分段计时；
- 增加解码耗时、队列深度、丢帧数和音视频时钟偏差统计；
- 使用 PIX、XbPerfView 或 PMCPB 记录单帧和线程分布；
- 不使用 `-d` 或 verbose 日志做性能结论，避免 XBDM/文件日志干扰。

### P2：降低稳定负载

- 空闲页面考虑 render-on-demand，避免没有变化时完整重绘 NanoVG；
- 评估 HTTP keep-alive 和连接复用，减少 `Connection: close` 带来的 TLS 握手；
- 增加纹理 LRU、内存预算和解码并发上限；
- 将 `GetTickCount` 计时替换为 XDK high-resolution timer，改善帧间隔和动画测量。

## 6. 待决策项

- [ ] UI、网络、图片、视频解码和音频线程的具体硬件线程分配；
- [ ] worker 和 UI 的 `SetThreadPriority` 档位；
- [ ] 是否采用独立视频解码/渲染线程，还是将现有播放器线程池改造成有界队列；
- [ ] 是否对 borealis 增加空闲跳帧；
- [ ] 是否在视频页暂时降低评论/图片加载并发，作为验证线程争用的 A/B 实验。

## 7. 建议验收指标

- 720p UI 滚动和视频页交互的 frame time P95 小于 20ms；
- 视频播放时分别记录 6 个硬件线程的 CPU 使用率；
- 记录软件线程到硬件线程的映射，确认没有 worker 意外全部落到 UI 线程；
- 记录视频解码耗时、显示帧率、丢帧数和音视频时钟偏差；
- 记录纹理上传次数、单次耗时、峰值内存和线程句柄数量；
- 连续播放和反复进入/退出视频页 20 次，无线程、句柄、纹理或内存增长。
