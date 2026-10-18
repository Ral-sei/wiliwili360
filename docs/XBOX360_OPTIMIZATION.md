# wiliwili Xbox 360 优化清单

> 状态：进行中（2026-10-16 创建）
>
> 范围：**仅 Xbox 360 路径**（`library/borealis/.../platforms/xbox360/`、
> `wiliwili/source/platform/xbox360/`、以及所有 `PLATFORM_XBOX360` 条件分支和
> Xbox 工具链/构建脚本）。不改动 Desktop/Switch/PSV 行为。
>
> 配套文档：`XBOX360_PORTING_PLAN.md`（长线计划）、
> `XBOX360_PLAYBACK_PERFORMANCE_PLAN.md`（播放性能，含 `[x360-perf]` 探针说明）、
> `XBOX360_EMPTY_IMPL_AUDIT.md`（空实现审计）。
>
> 每条均给出：位置、问题、方案、验证方式、状态。**每完成一条即在本文件打勾并
> 在 §3 记录。**

---

## 1. 结论摘要

| ID | 优化项 | 风险 | 状态 |
|---|---|---|---|
| O1 | 视频帧上传改用 `XMemCpyStreaming_WriteCombined`（并复核 dst `dcbt`） | 低 | ✅ 已完成 |
| O2 | 运行时探针（堆探针 `malloc/free` 改名）从 release 热路径剥离 | 低 | ✅ 已完成 |
| O3 | 输入每帧重复 `XInputGetState` | 极低 | ✅ 已完成 |
| O4 | `clear()` 每帧多余的 Z 缓冲清除 | 极低 | ✅ 已完成 |
| O5 | 探针自身开销（`probeHeap` 64MB 试探 / 纹理缓存列表双遍历） | 极低 | ✅ 已完成 |
| O6 | 三平面三把锁 → 一次 GPU cache 失效批量 | 中 | ⏸ 评估后暂不实施（§2 O6） |
| O7 | NanoVG D3D9 flush 冗余 render/sampler 状态去重 | 中 | ⏸ 评估后暂不实施（§2 O7） |
| O8 | 参数调优（`vd-lavc-threads`、`INMEMORY_CACHE`、`video-params` 轮询） | 低（需实机 A/B） | ⬜ 待办 |
| O9 | NanoVG UI 纹理上传（create/update）改用 `XMemCpyStreaming_WriteCombined`，RGBA 字节交换移出 WC 显存 | 低 | ✅ 已完成（实机待验） |
| O10 | XAV 硬件解码路径（XDK 公开 API，YouTube 同款引擎）接入 `HARDWARE_DEC` 开关 | 高（工具链阻塞） | ⏸ 代码就位，`-DXBOX360_XAV=ON` 才编译；默认 OFF（见 §2 O10） |
| P1 | `Xbox360Mutex` 自旋让步预算 4096 → 64（上下文切换 / critical-section 阻塞） | 低 | ✅ 已完成 |
| P2 | 日志刷屏治理：探针输出纳入 `X360_RUNTIME_PROBES` + mpv 日志请求 `v`→`warn` | 低 | ✅ 已完成（§6） |
| P3 | 停摆阶段机纳入 `X360_RUNTIME_PROBES`（主线程 `volatile` 写 + 看门狗线程） | 低 | ✅ 已完成 |
| P4 | hw5 上叠了 4 条后台线程 → CPU5 上下文切换 | 中（需实机） | ⬜ 待定 |
| P5 | `D3D: CPU frame time too long` → NanoVG flush 状态去重（即 O7） | 中 | ⬜ 待定 |
| P6 | Thread0 LHS 指令级定位 | — | ⬜ 需 PIX 下钻 |
| L1 | ffmpeg 侧 `[heap-slow]` 探针源码 gating（`third_party/ffmpeg-4.4.8/libavutil/mem.c`） | 低 | ✅ 已完成（已重建 ffmpeg） |
| L2 | cbffmpeg 兼容层 `[stall]`/`[x360-thread]`/`[xnet]` 探针 gating（`compat/xbox360/pthread.c`、`xbox_winsock_extra.c`） | 低 | ✅ 已完成（已重建 compat 归档） |

参考但不作为本轮改动（结构性/产品取舍，见 §4）：
- 主线程软件渲染管线拆分（暂无必要）
- borealis UI build 成本 / 退出确认框底页全量重建
- 关闭 `NVG_STENCIL_STROKES`

### 已排除（不要重复排查）

| 假设 | 排除依据（本地 XDK 文档，版本 21256.17） |
|---|---|
| `D3DPOOL_MANAGED` → `DEFAULT` 能加速 | `_dx_d3dpool_graphics.htm`：D3DPOOL 枚举在 Xbox 360 上 "serves no purpose"，仅为移植保留；所有 pool 对资源都合法，实际内存由 usage 决定。 |
| 给视频纹理加 `D3DUSAGE_DYNAMIC` | `_dx_D3DUSAGE_constants_graphics.htm`：只内部使用 `D3DUSAGE_CPU_CACHED_MEMORY` 与 `D3DUSAGE_RUNCOMMANDBUFFER_TIMESTAMP`；DYNAMIC 不被采纳。 |
| 改用 `D3DLOCK_DISCARD` | `_dx_D3DLOCK_constants_graphics.htm`：`D3DLOCK_DISCARD` / `D3DLOCK_DONOTWAIT` 在 Xbox 360 不支持。现有 NOOVERWRITE + 三缓冲轮换已是正确做法。 |

---

## 2. 逐条说明

### O1 — 视频帧上传改用 `XMemCpyStreaming_WriteCombined`

- **位置**：`library/borealis/library/lib/platforms/xbox360/xbox360_yuv.cpp`，`uploadPlane()`（原为逐行 `std::memcpy` + 手工 `dcbt`）。
- **问题**：`LockRect` 拿到的纹理指针通常是 **write-combined（WC）** 内存。WC 存储缓冲区要求写入对齐、≥4 字节、顺序无空洞、不得二次写同一地址
  （`Kernel_Write_Combined.htm`）。行宽 `drawWidth` 只保证偶数（`&= ~1`），不保证 4 字节对齐（如 854），行尾会触发 WC 部分刷新，
  对应 PIX 的 "Bad Write-Combined Writes"。当前对 **dst** 发的 `dcbt` 是读提示，对不可缓存目标大概率无效。
- **方案**：改用 XDK 专用 API `XMemCpyStreaming_WriteCombined(dest, src, count)`
  （`XMemCpyStreaming_WriteCombined.htm`：cached→WC 的高性能拷贝，内部 dcbt/dcbf/dcbz128，只占 128KB L2）。
  同时复核/移除 dst 上的 `dcbt`。
- **验证**：既有 `[x360-perf] yuv plane upload` 探针 A/B（round-7 基线 720p ≈1.64–1.75ms；1080p 3049KiB/帧待测）。
  本机只做 Xenon 编译/链接验证；观感与耗时需实机。
- **实施**：`uploadPlane()` 逐行 `memcpy` 改为 `XMemCpyStreaming_WriteCombined`；手工 `dcbt` 预取整段删除；
  新增 `locked.Pitch == sourcePitch` 时的**整平面单次拷贝**快路径（源/目标行距相同时整块无空洞），否则逐行。
  文件头新增 `#include <xbox.h>`（该 API 声明处，实现在 `xapilib.lib`，已在链接库列表内）。
- **构建验证**（2026-10-16）：`build-xbox360-local` 增量编译 + 链接通过，XEX 24,457,216B；
  `llvm-nm` 确认 `xbox360_yuv.cpp.o` 引用 `U XMemCpyStreaming_WriteCombined`，
  且 `title.elf` 中该符号及 `XMemCpyStreaming128_Writecombined_Helper` 等已解析为定义（`0x83208630`）。
- **实机待办**：上机采集 `yuv plane upload` 耗时对比（尤其 1080p），确认无画面异常。
- **状态**：✅（编译/链接已验证；实机耗时对比待补）

### O2 — 运行时堆探针从 release 热路径剥离

- **位置**：`library/borealis/library/cmake/toolchain.cmake`（`xbox360_runtime` 的 `-Dmalloc/-Dfree` 改名）+
  `library/borealis/library/lib/platforms/xbox360/xbox360_perf.cpp`（`x360_heap_probe_malloc/free`、`heapNote`）。
- **问题**：OXDK `builtins.c` 的 `operator new/delete` 直连 `malloc/free`，改名后 **全程序每次 `new`/`delete` 都进堆探针**，
  每次额外两次 `GetTickCount()` + 分支。UI 每帧成百上千次分配，release 也在付。
- **方案**：新增 CMake 选项 `XBOX360_RUNTIME_PROBES`（**默认 OFF**）：
  关闭时不改 `malloc/free` 别名，`xbox360_perf.cpp` 的堆探针段整体不编译；需要 `[heap-slow]` 回归时
  `-DXBOX360_RUNTIME_PROBES=ON` 打开，行为与历史构建一致。探针开关只作用于 `xbox360_runtime` 与
  `xbox360_perf.cpp`（`set_source_files_properties` 注入 `X360_RUNTIME_PROBES`），切换不触发全量重编译。
- **实施**：`toolchain.cmake` 加 `option(XBOX360_RUNTIME_PROBES ... OFF)`，`-Dmalloc/-Dfree` 移入 `if()`；
  `borealis/library/CMakeLists.txt` 给 `xbox360_perf.cpp` 注入 `X360_RUNTIME_PROBES=0/1`；
  `xbox360_perf.cpp` 用 `#if X360_RUNTIME_PROBES` 包住堆探针段与 `probeHeap`（见 O5）。
- **验证**（2026-10-16）：OFF（默认）与 ON 两种配置均通过 Xenon 编译 + 链接（XEX 24,457,216B）。
  - OFF：`xbox360_perf.cpp.o` 无 `x360_heap_probe_*`/`heapNote` 符号；`libxbox360_runtime.a` 的
    `builtins.c.o` 直接 `U malloc`（未改名）。
  - ON：`xbox360_perf.cpp.o` 定义 `T x360_heap_probe_malloc/free`，`builtins.c.o` 引用它们（`U`）。
- **未纳入本轮**：`stageMark`/`stageMarkIfMain`/看门狗阶段机未做开关——实测每帧约 10 次 + 每条主线程日志 4 次，
  单次仅一次 `GetTickCount` + 少量字节拷贝（`heapNote` 的原子操作在 ≥5ms 慢分配才触发），量级远低于堆探针，
  且调用点遍布 `application.cpp`/`view.cpp`/`hint.cpp`/`logger.hpp`，改动面与收益不成比例。如后续需要可另开开关。
- **状态**：✅

### O3 — 输入每帧重复 `XInputGetState`

- **位置**：`xbox360_input.cpp`，`updateUnifiedControllerState()` → `getControllersConnectedCount()` + `updateControllerState()`。
- **问题**：每帧对 0 号手柄调用两次 `XInputGetState`。
- **方案**：一次轮询结果复用（`updateUnifiedControllerState` 内直接查一次并决定是否填充）。
- **实施**：`updateUnifiedControllerState()` 直接调用 `updateControllerState(s, 0)`——后者本就先 `clearPolledState()`
  再 `XInputGetState()`，且手柄缺席时保留已清空状态；删去多余的 `getControllersConnectedCount()` 预检。
- **验证**：Xenon 编译 + 链接通过（2026-10-16）。手柄导航需实机确认无回归。
- **状态**：✅（编译已验证；实机待确认）

### O4 — `clear()` 每帧多余的 Z 缓冲清除

- **位置**：`xbox360_video.cpp`，`Xbox360VideoContext::clear()`。
- **问题**：`D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL`，但 NanoVG flush 与 YUV renderer 均设 `ZENABLE=FALSE`，
  Z 从不使用（stencil 需要，保留）。
- **方案**：去掉 `D3DCLEAR_ZBUFFER`。
- **实施**：`clear()` 改为 `D3DCLEAR_TARGET | D3DCLEAR_STENCIL`（stencil 保留，NanoVG 描边走 stencil）。
- **验证**：Xenon 编译 + 链接通过（2026-10-16）。实机 UI/视频需确认无异常。
- **状态**：✅（编译已验证；实机待确认）

### O5 — 探针自身开销

- **位置**：`xbox360_perf.cpp`，`probeHeap()`（每 10s 从 64MB 往下试分配）与 `report()`（每 2s 遍历纹理缓存列表）。
- **问题**：大块试探分配可能造成瞬时卡顿/堆碎片。
- **实施**：`probeHeap()` 及其调用点纳入 O2 的 `X360_RUNTIME_PROBES` 开关（默认 OFF 时不编译、不试探；
  健康行 `heap>=0 KiB` 保持不变）。
- **复核结论（列表双遍历）**：`Cache::getCacheList()` 返回 `std::list<Node<K,T>>&`（引用，非拷贝），
  两次遍历只是两次链表遍历，最大 2048 节点量级、每 2s 一次 —— **不是问题，未改动**。
- **验证**：Xenon 编译 + 链接通过（2026-10-16）。
- **状态**：✅

### O6 — 三平面三把锁 → 一次 GPU cache 失效批量

- **位置**：`xbox360_yuv.cpp`，`draw()` 中三次 `uploadPlane()`。
- **问题**：Y/U/V 是 3 张纹理，每帧 3 次 Lock/Unlock，每次 Unlock 触发 GPU cache 失效指令。
- **方案**：用 `D3DLOCK_NO_GPU_CACHE_INVALIDATE` + 收尾统一 `InvalidateGpuCache`（`_dx_D3DLOCK_constants_graphics.htm`），
  或三平面并进一张 I420 纹理（源缓冲本就连续，但需改 shader UV）。
- **验证**：`[x360-perf] yuv plane upload` A/B；实机画面正确性。
- **评估结论（2026-10-16，暂不实施）**：批量失效的前提是三块纹理在地址空间连续，能合成一个 `InvalidateGpuCache` 区间；
  Y/U/V 是三次独立 `CreateTexture`，地址不保证相邻，合并无依据。若强行用 `NO_GPU_CACHE_INVALIDATE` 又漏发失效，
  GPU 会采到旧帧（画面撕裂/花屏），风险高于收益（每帧仅省 2 条失效指令）。三平面合一才是有意义的做法，
  但需改 shader UV 与纹理布局，改动面大、收益需实机证明，暂缓。
- **状态**：⏸ 评估后暂不实施

### O7 — NanoVG D3D9 flush 冗余状态去重

- **位置**：`library/borealis/library/lib/extern/nanovg_d3d/src/nanovg_d3d9.h`，`D3D9nvg__renderFlush()` 及 `fill`/`stroke`。
- **问题**：每次 flush 无条件设置 ~24 个 render state + 6 个 sampler state；一帧至少 2 次 flush。
- **方案**：影子状态缓存，跳过未变化项。
- **验证**：`[x360-perf] flush` 分位数 A/B。
- **备注**：文档实测 `build` 为主耗时、`flush` 占比小，预期收益有限，优先级低。
- **评估结论（2026-10-16，暂不实施）**：`D3D9nvg__renderFlush` 无条件设状态是上游 backend 的既有设计，
  加影子状态需要覆盖 `fill`/`stroke`/`convexFill`/`triangles` 各分支的全部读写点，任何漏项都会造成跨帧状态泄漏
  （花屏/闪烁）。而 `[x360-perf]` 历史数据显示 flush 不是瓶颈（build 才是），收益/风险不划算，暂缓。
- **状态**：⏸ 评估后暂不实施

### O8 — 参数调优（需实机 A/B）

- `mpv_core.cpp` `vd-lavc-threads=6` vs 4（cbffmpeg P2 基线 4 线程 spread 最优）。
- `config_helper.cpp` `INMEMORY_CACHE` 上限 `{0,1,5,10}MB`（性能计划 §15.7 慢 CDN 候选：提高上限吸收滴流）。
- `mpv_core.cpp` 每 250ms 主线程同步 `getInt("video-params/w")` → 改 `mpv_observe_property`。
- **状态**：⬜

### O9 — NanoVG UI 纹理上传改用 `XMemCpyStreaming_WriteCombined`

- **位置**：`library/borealis/library/lib/extern/nanovg_d3d/src/nanovg_d3d9.h`，
  `D3D9nvg__renderCreateTexture` 与 `D3D9nvg__renderUpdateTexture`。
- **问题**：O1 只修了视频三平面；**UI 纹理**（图片/缩略图/字体图集/弹幕遮罩）仍在裸 `memcpy` 进 `LockRect` 的 write-combined 显存。
  更严重的是 `renderUpdateTexture` 的 RGBA 分支**先在 WC 显存上 `memcpy`、再原地逐像素读回做 GPUENDIAN_8IN32 字节交换**——
  读 WC 内存正是 PIX 的 `Uncached/Write-Combined Reads`（文档要求必须修），写则是 `Bad Write-Combined Writes`。
  主线程触发点：智能防挡弹幕遮罩 `danmaku_core.cpp:495` 的 `nvgUpdateImage(maskTex, ...)`（RGBA），以及图片/缩略图的 `nvgCreateImageRGBA`。
- **方案**：
  1. 新增 `D3D9nvg__copyToTexture()`：Xbox 走 `XMemCpyStreaming_WriteCombined`（`<xbox.h>`，`xapilib.lib`），其它平台仍 `memcpy`；
  2. create/update 的所有行拷贝改走该 helper；
  3. `renderUpdateTexture` 的 RGBA 分支与 create 对齐：**字节交换进 cached scratch 行，再流式写入纹理**（分配失败才退回原地交换的旧路径）。
- **验证**：`nanovg_d3d9_backend.cpp.o` 单独强制重编通过（`llvm-nm` 确认 `U XMemCpyStreaming_WriteCombined`）；
  全量链接通过，XEX 24,391,680B。实机 A/B 用 `[tex]` 探针（需 `-DXBOX360_RUNTIME_PROBES=ON`）看弹幕遮罩/图片页 `fill` 段耗时。
- **状态**：✅（编译/链接已验证；实机耗时对比待补）

### O10 — XAV 硬件解码路径（默认 OFF，工具链阻塞）

- **动机**：对照 `H:\youtube\default.xex`（官方 YouTube 客户端）——其播放链路
  `WebMediaPlayerImpl → ShellPipeline → xav::Player → CreateXavPlayer`，即 **XAV**。
  核对该 XDK 后确认 **XAV 是公开 API**（`/root/xdk360/include/xbox/xav.h`、
  `lib/xbox/xav.lib`；`CreateXAVPlayer` / `IXAVPlayerGetTexture::GetTexture` 直接返回
  `IDirect3DTexture9*`，`XAVPreRender`/`XAVPostRender` 包住 Swap）。这是软解
  （720p30 约 0.85× 实时）之外唯一的量级提升路径。
- **已实现（`X360_XAV` 开关内）**：
  - 新增 `library/.../platforms/xbox360/xbox360_xav.{hpp,cpp}`：XAVInitialize/CreateXAVPlayer/
    GetTexture/PreRender/PostRender 封装 + `IXAVPlayerEvents` 事件汇。
  - `xbox360_video.cpp`：`XAVPostRender`（Swap 前）/`XAVPreRender`（Swap 后）接入 present；
    暴露 `getPresentParameters()`（XAVInitialize 需要创建时的 `D3DPRESENT_PARAMETERS`）。
  - `mpv_core.cpp`：`HARDWARE_DEC` 打开时 XAV 播视频、mpv 降为 `vid=no` 只出音频
    （DASH 的 `audio-file` 改喂给 mpv，避免视频流被下载两次）；暂停/seek/停止转发到 XAV；
    `xbox360_yuv.cpp` 新增 `drawTexture()`（RGB passthrough quad）。
  - `setting_activity.cpp` + 7 个语言包：打开开关时右上角通知「实验功能，需要 codec pack」。
- **实测阻塞（2026-10-16，本机构建）**：
  1. XAV 入口是 **MSVC C++ 修饰名**（`?XAVInitialize@@YAJPBUXAVInit@@QAK@Z`、
     `?CreateXAVPlayer@@YAJPAPAUIXAVPlayer@@PAUIXAVPlayerEvents@@PAUIXAudio2@@@Z` 等），
     OXDK clang 产出 Itanium 名 → 直接用 `<xav.h>` 声明链接必然失败。已用 `asm()` 显式绑定
     真实符号（见 `xbox360_xav.cpp`）。
  2. 定义这些符号的归档成员（`xav.lib` 内 offset 31301500、成员名 `/5844`，PowerPC COFF、
     1030 节）**`llvm-nm` 无法识别**（`The file was not recognized as a valid object file`），
     于是 oxdklink 的 `archive_defines` 找不到定义 → 链接报
     `not provided by the console and not in any library`（5 个 XAV 入口 + `IsEqualGUID`
     + `open/close/read/write/lseek/isatty/fdopen/dup2/tempnam/unlink/j1/vsnprintf_s/_setjmp`）。
  3. 即 XAV 的依赖闭包含**桌面/POSIX CRT 与 MSVC C++ 运行库**符号——与 OXDK 明确
     「不用微软 C++ 库（Itanium/微软名字不可互通）」的前提冲突。
- **结论**：这是 **OXDK 工具链侧**（coff2elf/oxdklink 支持该 COFF 成员并补齐闭包）的工作，
  不是 wiliwili 的代码改动能解决的。代码保留在 `X360_XAV` 开关后（默认 OFF），
  开关通知同样 gate，避免向用户承诺不可用功能。启用方式：`-DXBOX360_XAV=ON`
  （`toolchain.cmake` 的选项；打开后 `--libs` 追加 `xav`）。
- **另有两项产品级阻塞（与工具链无关，需先解决）**：
  - `IXAVPlayer::Open(url)` 只吃 URL，`XAVPropertyKey` 全表**无自定义 HTTP 头**；B 站 CDN
    需要 `Referer: https://www.bilibili.com` → 需要机内 header 注入代理。
  - `Open` 只吃单 URL，B 站默认 DASH 音视频分离 → 需要代理侧 mux，或按本实现的
    「XAV 只出画面 + mpv 出音频」组合（XAV 用 `Mute(TRUE)`/`pAudio2=nullptr` 静音）。
- **状态**：⏸ 代码就位、编译通过（`-DX360_XAV=1` 单 TU 验证），默认构建不含 XAV。

---

## 3. 进度记录

| 日期 | ID | 内容 | 构建/验证 |
|---|---|---|---|
| 2026-10-16 | — | 创建本文件，完成代码审查与 XDK 文档核验 | — |
| 2026-10-16 | O1 | `uploadPlane` 改用 `XMemCpyStreaming_WriteCombined`，删除手工 `dcbt`，加整平面快路径 | Xenon 编译+链接通过；nm 确认符号解析 |
| 2026-10-16 | O3 | `updateUnifiedControllerState` 去掉重复 `XInputGetState` | Xenon 编译+链接通过 |
| 2026-10-16 | O4 | `clear()` 去掉 `D3DCLEAR_ZBUFFER` | Xenon 编译+链接通过 |
| 2026-10-16 | O2 | 新增 `XBOX360_RUNTIME_PROBES`（默认 OFF），关闭堆探针 `malloc/free` 改名 | OFF/ON 两配置均编译+链接通过；nm 双向核对 |
| 2026-10-16 | O5 | `probeHeap` 纳入同一开关；`getCacheList` 双遍历复核为非问题 | Xenon 编译+链接通过 |
| 2026-10-16 | P1 | `Xbox360Mutex` 自旋让步 4096→64 | Xenon 编译+链接通过 |
| 2026-10-16 | P3 | 停摆阶段机纳入 `X360_RUNTIME_PROBES`；OFF 时无 `g_stage*`/看门狗符号，ON 时恢复 | OFF/ON 两配置均编译+链接通过；nm 双向核对 |
| 2026-10-16 | P2 | 探针输出统一纳入 `X360_RUNTIME_PROBES`：`[x360-perf]`/`[x360-mpv]`/`[tls]`/`[curl]` 默认关闭；mpv 日志请求 `v`→`warn` | OFF/ON 两配置均编译+链接通过；构建产物中相关探针字符串已消失 |
| 2026-10-16 | L1 | `third_party/ffmpeg-4.4.8/libavutil/mem.c` 的 `[heap-slow]` 探针 gating + 重建 `libavutil.a` | ffmpeg 增量重建通过；`libavutil.a`/最终 XEX 内 `heap-slow` 计数为 0 |
| 2026-10-16 | L2 | 全部 Xbox 探针日志纳入开关（borealis 慢路径 / `[x360-playurl]` / `[x360-thread]` / `[tex]` / `[stall]` / RTTI）；cbffmpeg compat `pthread.c`+`xbox_winsock_extra.c` gate 并重建归档 | `ninja -t clean` 全量重编通过；最终 XEX 除 `x360-sw`(2，运行期拦掉) 外全部探针字符串为 0 |
| 2026-10-16 | O9 | NanoVG 纹理 create/update 改 `XMemCpyStreaming_WriteCombined`；`renderUpdateTexture` RGBA 字节交换移出 WC 显存 | `nanovg_d3d9_backend.cpp.o` 强制重编 + 全量链接通过（XEX 24,391,680B）；nm 确认符号解析 |
| 2026-10-16 | O10 | XAV 硬件解码路径接入 `HARDWARE_DEC`（默认 `X360_XAV=OFF`）；XAV 入口按 MSVC 修饰名 `asm()` 绑定 | `-DX360_XAV=1` 单 TU 编译通过；默认 OFF 全量链接通过（XEX 24,391,680B，9 个 XDK 库）。**链接阻塞：xav.lib 定义成员 COFF 无法翻译**（见 §2 O10） |
| 2026-10-16 | — | 发现构建系统不记录头文件依赖（`#deps 0`），纯头文件改动不触发重编 | 见 §6.6；需 clean 重建规避 |

---

## 4. 暂不改动（结构性 / 产品取舍）

- **主线程软渲染管线**：mpv render + swscale + 上传全在主线程；1:1 surface 后已 ~2–4ms，暂不拆线程。
- **UI build 成本**：`XBOX360_PLAYBACK_PERFORMANCE_PLAN.md` §15.5 记录的退出确认框底页每帧全量重建（38fps / build 25.2ms），
  属 borealis 核心，单独评估。
- **`NVG_STENCIL_STROKES`**：每 stroke 三 pass，去掉影响视觉质量。

---

## 5. PIX 警告分析（2026-10-16）

用户提供的 PIX CPU Top Issues（40 Unique Warnings）与代码/架构的对应关系如下。
计数单位为"出现该条件的帧数"，`Last Tick` 为该条件最后一次出现的时间戳。

| PIX 警告 | 优先级/计数 | 对应代码/原因 | 处理 |
|---|---|---|---|
| `PMCPB: Thread0 Instructions Per Cycle is too low` | P1, 97% / 11329 | Thread0 = 主线程：UI build（性能计划 §15.5 实测 ~18–20ms/帧）+ mpv swscale + YUV 上传 + NanoVG flush，线程本就饱和 | 结构性；减少主线程工作（日志/状态 churn/探针，见 P2/P3/P5） |
| `PMCPB: Thread0 LHS are too high` | P1, 96% / 11329 | PPC load-hit-store：store 后 20 条指令内 load 同址，或地址相差 4KB 倍数（`..._pix_tracedump_lhs.htm`）。`volatile` 全局、float↔int 转换、指针/数组间接访问最易触发 | P3 已移除主线程阶段机的 `volatile` 写；其余需 P6 指令级下钻 |
| `Too many threads blocking on Critical Sections` | P1, 69% / 11072 | CRT 堆锁（全程序 `malloc/free`）+ `Xbox360Mutex` 自旋 + Logger `logMtx` | 堆锁是"受害者/放大器"（性能计划 §14.5 已证）；自旋预算见 P1；日志量见 P2 |
| `PMCPB: Thread0 L1 Misses are too high` | P1, 40% / 10526 | 主线程工作集：3MiB 视频帧 + UI 几何/字符串 | 结构性；1:1 surface 已消除重采样 |
| `PMCPB: Thread1/2/3/4/5 Instructions Per Cycle is too low`、`I-Cache Miss Cycles` | P1 | mpv/ffmpeg 解码线程 + cpr worker + 后台线程 | 解码/网络固有；hw5 叠线程见 P4 |
| `CPU: Too many context switches (CPU 5)` | P1, 96% / 11381 | hw5 上叠了 `task_loop` + 弹幕 mongoose + 弹幕 task + 1 个 cpr worker（`async.h` `{1,3,5}`） | P4 |
| `CPU: Too many context switches (CPU 2)` | P1, 60% / 10668 | hw2 在 mpv pthread spread `{2,4,1,3}` 内，ffmpeg 解码 worker 池 | 解码线程池固有 |
| `D3D: CPU frame time too long` | P1, 270 | 每帧 D3D CPU 提交：NanoVG flush（≥2 次/帧）逐元素状态设置 + YUV `saveState/restoreState` + `clear` + `Present` | O4 已去掉多余 Z 清除；进一步见 P5 |
| `Too many threads blocked on Critical Sections`、`Audio dropout detected`、`System Monitor Buffer Overrun` | P1/P2 | 与上同源（锁/音频） | 随 P1/P2/P4 |

### 本轮据此实施

- **P1 ✅ `Xbox360Mutex` 自旋预算 4096 → 64**（`library/borealis/library/include/borealis/core/xbox360_mutex.hpp`）。
  每次竞争失败原先要连做 4096 次 `Sleep(0)`（= 4096 次内核让步）才转入 1ms 阻塞，本身就在制造上下文切换；
  短临界区 64 次让步足够，长持锁场景更早进入阻塞。
- **P3 ✅ 停摆阶段机纳入 `X360_RUNTIME_PROBES`**（`xbox360_perf.cpp`）。
  关闭时 `stageMark`/`stageMarkIfMain`/`stageSetMainThread` 变 no-op，主线程不再每帧/每条日志写
  `volatile char g_stage[128]`/`g_phaseDesc[128]`/`g_stageTick`/`g_phaseTick`，也不再创建看门狗线程。
  与 PIX 的 Thread0 LHS/IPC 与 critical-section 信号方向一致。

### 待定（需实机或 PIX 下钻）

- ~~**P2**：`mpv_core.cpp` 在初始化后 `mpvRequestLogMessages(mpv, "v")`，release 下 mpv 会灌大量 verbose 日志。~~
  **已完成（2026-10-16）**：探针输出统一纳入 `X360_RUNTIME_PROBES`，mpv 日志请求默认降到 `warn`。见 §6。
- **P4**：`task_loop`（`thread.cpp`）、弹幕 mongoose/task（`danmaku_live.cpp`）与 1 个 cpr worker（`cpr/async.h`）都钉在 hw5，
  4 条线程抢一个硬件线程 → CPU5 上下文切换。可选：把 cpr worker 移出 hw5、或把 `task_loop` 与弹幕合并/错峰。
  需要实机 A/B（hw5 与 hw4 是否为同一物理核的 SMT 兄弟会影响是否偷解码核的发射槽）。
- **P5**：即 O7（NanoVG flush 状态去重）。PIX 的 `D3D: CPU frame time too long` 使其优先级上升，
  但仍需覆盖所有 flush 分支、风险不变，建议在拿到 `[x360-perf] flush` 分位数后再决定。
- **P6**：LHS 指令级定位需要 PIX 的 Source / Top Issues 下钻（把 `.pix` 采集给我，或导出 LHS 的 Symbol/Info 列），
  才能点名到具体函数。已知敏感面：`volatile`（已随 P3 收敛）、float↔int 转换、指针/数组间接访问、地址相差 4KB 倍数的缓冲。

---

## 6. 日志刷屏治理（2026-10-16）

### 6.1 刷屏来源实测

对 `logs/round7_20261010_xbdm_playback.txt`（约 2.5 分钟，5021 行）归一化统计：
XBDM 每条 `debugstr` 后面还跟一个空行，所以"看起来"翻倍；真实约 2500 行 ≈ 17 行/s。
按来源拆分：

| 来源 | 占比 | 通道 |
|---|---|---|
| `[x360-perf]`（frame / p50/p95/max / sync split / health / mpv render call / yuv upload / state+quad / renderer call / setFrameSize 汇总） | ≈38% | `Logger::info` |
| `[heap-slow]`（`new`/`del` + ffmpeg `av_malloc`/`av_free`） | ≈20% | `DbgPrint` |
| `[tls]`/`[curl]`（握手、socket、分配器） | ≈9% | `DbgPrint` |
| `[x360-sw]`（mpv 软渲染探针）+ 其后的空行 | ≈11% | mpv 日志 → `Logger` |
| `[x360-mpv]`（fps / render-pipe） | ≈3% | `Logger::info` |
| 其它（`terminate thread` 等） | 余量 | — |

### 6.2 处理方式：单一开关 `XBOX360_RUNTIME_PROBES`（默认 OFF）

沿用 O2/P3 引入的 CMake 选项 `XBOX360_RUNTIME_PROBES`（定义于
`library/borealis/library/cmake/toolchain.cmake`，默认 OFF），把**所有纯诊断探针输出**统一纳入：

- 宏 `X360_RUNTIME_PROBES=0/1` 注入到：`wiliwili` 目标（`CMakeLists.txt` 的 `APP_PLATFORM_OPTION`）、
  `borealis` 的 `xbox360_perf.cpp`/`xbox360_yuv.cpp`（`set_source_files_properties`）、
  `xbox360_mbedtls`/`xbox360_curl`（`library/CMakeLists.txt` 的 `target_compile_definitions`）。
- 关闭（默认）时：
  - `mpv_core.cpp` 的 `X360PerfProbe`/`X360SizeProbe`/`X360MpvStatsProbe`/`X360RenderPipeProbe` 四个 `flush()`
    直接返回（不再输出 `[x360-perf]`/`[x360-mpv]`）。
  - `xbox360_yuv.cpp` 的 `DrawProbe::flush()` 直接返回。
  - `xbox360_perf.cpp` 的 `report()` 直接返回（不再输出 frame/p50/health）。
  - `mpvRequestLogMessages(mpv, "v")` 改为 `"warn"` → mpv 的 `[x360-sw]`（verbose 级）不再下发。
  - `third_party/curl/lib/{connect.c,vtls/mbedtls.c}`、`third_party/mbedtls/library/{ecp.c,platform.c}`
    在本地 `extern DbgPrint` 声明之后 `#define DbgPrint(...) ((void)0)` → `[tls]`/`[curl]` 全部编译为空。
  - `third_party/ffmpeg-4.4.8/libavutil/mem.c` 同样处理 → `[heap-slow]`（ffmpeg 侧）编译为空。
- 打开（`-DXBOX360_RUNTIME_PROBES=ON`）时行为与历史构建一致，恢复全部探针。

### 6.3 构建验证（2026-10-16）

- 默认 OFF 与 ON 两配置均 Xenon 编译 + 链接通过（0 错误）。
  - OFF：XEX 24,391,680 B（image `0x1740000`）；`grep` 确认构建产物中已无
    `[tls] hs-exit`、`[curl] socket`、`est-vf`（mpv stats 探针）、`render-pipe`、
    `yuv plane upload`、`x360-perf`（`xbox360_perf.cpp.o` 内）等字符串。
  - ON：XEX 24,457,216 B（与历史构建一致），探针恢复。
- **ffmpeg 侧已重建生效**：`third_party/ffmpeg-4.4.8/libavutil/mem.c` 的改动已通过
  `make -C build/ffmpeg-x360-tls -j12 libavutil/libavutil.a`（`src` 软链到 `third_party/ffmpeg-4.4.8`）
  重新编译并重归档；`grep heap-slow libavutil.a` = 0，重链后最终 XEX 内 `heap-slow` 计数为 0。

### 6.5 最终 XEX 校验（2026-10-16）

重链后对 `build-xbox360-local/wiliwili` 直接 grep：

| 探针字符串 | 计数 | 说明 |
|---|---|---|
| `x360-perf` / `x360-playurl` / `x360-mpv` | 0 | 周期 + 慢路径探针全部编译为空 |
| `x360-thread` | 0 | wiliwili + cbffmpeg 两侧均已消失 |
| `[stall]` / `[stall-task]` | 0 | `xbox360_mutex.hpp` 与 cbffmpeg pthread.c 均已 gate |
| `heap-slow` | 0 | wiliwili + ffmpeg 两侧均已消失 |
| `[xnet]` | 0 | cbffmpeg `xbox_winsock_extra.c` 已 gate |
| `hs-exit` / `curl] socket` | 0 | mbedtls/curl 探针已编译为空 |
| `est-vf` | 0 | mpv 侧统计探针已关闭 |
| `[tex]`（`create=`） | 0 | NanoVG 建纹理计时探针已 gate |
| `x360-sw` | 2 | 仍在**预构建的 libmpv.a** 内（未重建 mpv），但 mpv 日志请求已降为 `warn`，verbose 级消息不会下发 → 不再输出 |

> `[x360-sw]` 字符串残留无需处理：它只是预构建 mpv 里的常量，日志开关已在请求层拦掉。

### 6.6 ⚠️ 构建系统注意：本 Xbox 构建不记录头文件依赖

本轮发现：`ninja -t deps <obj>` 对 wiliwili/borealis 的所有对象都显示 `#deps 0` ——
**Ninja 没有记录任何头文件依赖**。后果：**纯头文件改动不会触发重编**。

- 实例：`xbox360_mutex.hpp` 的 `[stall]` gate 加完后，普通 `cmake --build` 只重链不重编，
  XEX 里仍带旧字符串；必须 `ninja -C build-xbox360-local -t clean` 后全量重编才生效。
- 能正常触发重编的：**编译命令行变化**（CMake 宏/选项，如 `-DX360_RUNTIME_PROBES`）与**源文件改动**。
- 影响：改 `xbox360_perf.hpp` / `xbox360_mutex.hpp` / `nanovg_d3d9.h` 等头文件后，
  务必强制重建；否则会链出"看起来对、其实没编进去"的 XEX。
- 建议（未做）：修 CMake/工具链让编译器的 `-MD` depfile 真正生效，或改头文件后统一 clean 重建。

### 6.4 覆盖范围（默认 OFF 时全部静默）

第二轮把**所有** Xbox 探针日志都纳入开关，默认构建的 `Logger`/`DbgPrint` 输出与其它平台一致：

- borealis 慢路径 `[x360-perf]`：`application.cpp`(5)、`view.cpp`(2)、`hint.cpp`(2 info + 1 debug)、
  `box.cpp`(1)、`thread.cpp`(2) → 改用 `X360_PROBE_INFO`/`X360_PROBE_DEBUG` 宏
  （定义在 `xbox360_perf.hpp`；OFF 时展开为 `do { if (false) Logger::info(...); } while(0)` ——
  参数仍被"使用"、无 `-Wunused-variable` 告警，死分支被 `-O3` 消除）。
- wiliwili：`video_detail.cpp` 的 `[x360-playurl]`(4)、`mpv_core.cpp` 的 `[x360-mpv] command_async`
  与 `[x360-sw] sws-scaler`、`main.cpp` 的 `Xbox RTTI` 启动转储 → 同上宏 / `#if`。
- `[x360-thread]` DbgPrint：`thread.cpp`(1)、`danmaku_live.cpp`(2) → `#if X360_RUNTIME_PROBES`。
- `nanovg_d3d9.h` 的 `[tex]` 建纹理计时、`xbox360_mutex.hpp` 的 `[stall] spinlock` → `#if`。
- cbffmpeg 兼容层（外部仓 `H:/cbffmpeg`）：`compat/xbox360/pthread.c` 的 `[stall]`/`[x360-thread]`
  与 `compat/xbox360/xbox_winsock_extra.c` 的 `[xnet]` → per-file `#define DbgPrint(...) ((void)0)`；
  用 `cbffmpeg/scripts/mpv_x360_compat_archive.sh` 重建 `build/compat-x360/libmpv_xbox360_compat.a`。

正常 `warn`/`error` 级日志不受影响；非探针的一次性信息（如 `Xbox video mode:`）保留。
