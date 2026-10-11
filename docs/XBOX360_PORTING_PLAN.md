# wiliwili Xbox 360 长线移植计划

> 状态：M0 工具链基线已完成；M1 NanoVG D3D9 已完成 Xenon 编译、shader 生成、XEX 链接，并由真实 XDK 确认基础图形输出；M3 wiliwili UI 最小集成已通过 Xbox 360 实机验收（2026-09-25）；M4 网络/API 首页链路已通过 Xbox 360 实机验收（2026-09-26）；M5/M6 已完成首次真实视频播放验证（2026-09-30）；M7 点播卡顿根因已定位并实机验证修复（2026-10-02，720p 窗口化/全屏与 360p 窗口化均 30fps 视频 + 53–60fps UI），直播卡顿排查与 PSV-parity 对策已部署 round-6 待验证（2026-10-04，见 `XBOX360_PLAYBACK_PERFORMANCE_PLAN.md` §14）；空实现审计 A 类 5 条已修复并通过全量编译（2026-10-04，`XBOX360_EMPTY_IMPL_AUDIT.md` §6）。M5 完整闸门（暂停/seek/停止/EOF）、M6 生命周期验收与长期稳定性尚未通过
>
> 目标：在 Xbox 360/XDK 上逐步运行 wiliwili，并保留现有 Desktop、Switch 等平台。
>
> 参考：本仓库、`H:/bili360-main`、`H:/OXDK`、`H:/ffmpeg-xenon`，以及本地 Xbox 360 XDK 文档。
> 配套文档：`XBOX360_PLAYBACK_PERFORMANCE_PLAN.md`（播放性能，§11–§14 为最新实机记录）、`XBOX360_EMPTY_IMPL_AUDIT.md`（空实现审计）、`XBOX360_NETWORK_PERFORMANCE_REVIEW.md`、`XBOX360_UI_PERFORMANCE_REVIEW.md`、`XBOX360_DEBIAN_BUILD.md`。

## 1. 结论摘要

这不是一次简单的 CMake 平台添加，而是一个新平台后端和播放器后端项目。底层可行性已经有较强证据：`bili360-main` 已在 Xbox 360 上验证 Bilibili API、HTTPS、DASH、H.264/AAC、Xbox 360 XDK D3D9、XAudio2 和 XInput。

**完整移植原则：实现可以替换，功能契约不能随意缩水。** Xbox 360 可以使用新的播放器、网络、NanoVG、SDL2x360 或 XDK backend；不要求逐字复制 Desktop/Switch 的实现。但替换后的实现必须逐步覆盖 wiliwili 原有功能和行为，不能把“临时能显示首页/播放一个固定视频”当作最终移植完成。任何暂时不支持的功能都必须在里程碑中明确标注，并有后续补齐计划。

**重要边界：PC 的 D3D9 不能视为 Xbox 360 的 D3D9。** 文档中所有 D3D9 相关任务都默认需要移植为 Xbox 360 XDK/Xenon 可用的实现，包括设备创建、纹理格式与布局、shader 编译/字节码、资源池、状态管理、Present 和 XDK 库链接。不能直接链接 PC 的 `d3d9.dll`/`d3dx9.dll`，也不能假设 PC D3D9 backend 生成的 shader 或纹理路径在 Xbox 360 上可用。

推荐的长期路线是：

```text
NanoVG D3D9
    ↓
borealis Xbox 360 平台层
    ↓
wiliwili UI/API（先不接播放器）
    ↓
FFmpeg + MPV（优先评估 libmpv 最小移植）
    ↓
完整播放页和生命周期
    ↓
必要时切换到 Xbox360PlayerCore 兜底
```

当前方向调整为优先评估并移植 MPV，使 Xbox 360 播放器行为尽量与上游同步。第一步不是直接承诺完整移植，而是用固定版本的 MPV、FFmpeg-xenon 和最小 libmpv 配置做可行性闸门：确认 MPV 核心、解复用、解码、时钟、线程和渲染接口能在 Xenon ABI/XDK 上编译、链接并运行。若 MPV 需要大规模修改才能适配 FFmpeg 1.2.x 或 XDK runtime，才切换到 `Xbox360PlayerCore` 作为主线兜底；无论采用哪条路线，都必须提供与 wiliwili 播放器契约等价的功能。

## 2. 难度标记

| 标记 | 含义 | 典型工作量 |
|---|---|---|
| L1 | 简单 | 半天至 2 天，接口明确、风险低 |
| L2 | 较低 | 2 天至 1 周，需要平台验证 |
| L3 | 中等 | 1 至 3 周，需要新模块和实机测试 |
| L4 | 高 | 3 至 8 周，跨多个子系统 |
| L5 | 很高 | 需要重写或长期维护，存在技术路线不确定性 |

难度是单项估计，不代表可以简单相加。每个阶段必须先完成验收标准，再进入下一阶段。

## 3. 现状和边界

### 3.1 播放控件与播放内核的边界

wiliwili 的播放控件不是 MPV 的 OSD。当前职责大致如下：

```text
VideoView / Player Activity
  ├─ XML 控件、焦点、按键和手势
  ├─ 音量/进度/倍速/清晰度菜单
  ├─ 播放页生命周期和 APP_E 事件
  └─ 调用 MPVCore 的播放命令

MPVCore
  ├─ 解复用、解码、缓存和音视频同步（通过 libmpv）
  ├─ seek、pause、resume、stop、speed、volume 的实际执行
  ├─ 播放状态、错误、EOF 和进度状态
  ├─ 备用 URL/播放列表行为
  └─ 视频帧渲染到 borealis 可绘制的区域
```

代码中 `VideoView::pause/resume/setSpeed/setUrl` 直接调用 `MPVCore`，例如 `wiliwili/source/view/video_view.cpp`；`VideoView` 负责控件和交互，`MPVCore` 负责执行命令及产生播放状态。清晰度选择等业务还在 Player Activity/API 层完成，再通过 `VideoView::setUrl` 传入播放内核。

因此 Xbox 360 的目标不是移植 MPV 的菜单或 OSD，而是：

- 保留现有 `VideoView`、Activity、XML 控件和按键逻辑；
- 为 Xbox 360 提供一个能够承接这些调用的播放内核实现；
- 将新内核的状态转换为现有 `MPVCore`/`MPV_E` 所需的状态和事件；
- 让控件看到的暂停、进度、seek、EOF、错误和返回行为与现有实现一致。

这允许 Xbox 360 使用 FFmpeg + custom AVIO + D3D9/XAudio2 替代 libmpv 内部实现，同时不需要重写播放控件。若后续直接把 `VideoView` 改成调用新的播放器类，也必须保持相同的公开行为，而不是把控件逻辑迁移到 SDL/XUI 或播放器内部。

### 3.2 wiliwili 当前假设

- 顶层 CMake 当前只把 Desktop 和 Switch 作为主路径，见 `CMakeLists.txt`。
- UI 使用 borealis，绘制接口统一依赖 `NVGcontext`。
- Desktop/交叉平台播放器依赖 libmpv，核心封装是 `MPVCore`。
- 网络层依赖 cpr/libcurl，并在多个模块中使用异步请求、互斥锁和线程。
- 现有播放器、UI、事件总线都假设 mpv 的状态和事件存在。

### 3.3 已有 Xbox 360 参考实现

`H:/bili360-main` 已提供以下可复用设计：

- XNet DNS/socket 初始化；
- libcurl 7.37.1 + mbedTLS 2.28.10；
- WBI 和 Bilibili `view/playurl` 请求；
- 独立视频/音频 CurlStream；
- FFmpeg custom AVIO；
- H.264/AAC 解码；
- D3D9 YUV 纹理渲染；
- XAudio2 音频输出；
- XInput 手柄轮询；
- VS2010/XDK Xbox 360 工程和 XEX 部署。

`H:/ffmpeg-xenon` 的 FFmpeg 网络协议配置是关闭的，因此不能直接把 HTTPS URL 交给 FFmpeg；网络读取必须由 libcurl/XboxTLS 等外部层提供，再通过 AVIO 喂给 FFmpeg。

TLS 来源说明：当前主线推荐直接复用 `H:/bili360-main/third_party/mbedtls`
（mbedTLS 2.28.10）以及 `H:/bili360-main/third_party/curl` 的 mbedTLS
backend。该工程还包含 Xbox 专用的 `config-xbox360.h`、`XNetRandom` 熵源、
时间转换和 CA bundle 处理。`bili360-main/docs/DEPENDENCIES.md` 明确将
WTFPlay 的 `tls_xboxtls.c` 列为 deliberately not used，因此当前工作区没有
一个可直接引用的 XboxTLS 源码副本；若改用 XboxTLS，必须另外提供并固定其
来源/版本，不能把文档中的“XboxTLS”理解为已有依赖。

## 4. 长线里程碑

### M0：工具链和样例基线（L2）

目标：确认 OXDK、XDK、ffmpeg-xenon 和实机部署链路稳定。

任务：

- [x] `OXDK` `doctor` 能找到 Xbox 360 XDK（`D:\Microsoft Xbox 360 SDK`）。
- [x] Xenon ABI clang/lld 能构建最小 C/C++17 XEX（WSL `/root/oxdk-llvm/build`）。
- [x] 最小 XEX 已在协作者环境/Xenia 启动并通过 `abitest`（`ALL CHECKS PASSED`）。
- [x] 编译 `H:/OXDK/xbox360/samples/d3dclear`（已生成 `default.xex`）。
- [ ] 编译并运行 `H:/bili360-main` 的 Release 配置。
- [ ] 固定并记录 FFmpeg revision、XDK 版本、编译器版本。

验收：最小 XEX 已有外部启动与 ABI 通过证据；真实 XBDM 日志和 `d3dclear`
运行截图待补齐。

### M0 编译与验证（WSL 可复现流程）

以下流程在 WSL Debian 中验证。Microsoft XDK 需要用户自行安装，本文不
分发 XDK 内容。

1. 安装构建依赖：

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build git clang lld python3
```

2. 设置路径。Windows 盘符在 WSL 中分别挂载为 `/mnt/c`、`/mnt/d`、`/mnt/h`。
为避免路径空格导致 OXDK Makefile 判断失败，建立一个无空格的 XDK 别名：

```bash
ln -sfn "/mnt/d/Microsoft Xbox 360 SDK" "$HOME/xdk360"
export OXDK_DIR=/mnt/h/OXDK
export XDK_DIR="$HOME/xdk360"
```

确认 `$XDK_DIR/lib/xbox/xboxkrnl.lib` 存在后，构建带 Xenon ABI 补丁的 LLVM：

```bash
cd /mnt/h/OXDK
# 首次构建（约需 20 GB 磁盘空间，耗时较长）
bash ./scripts/build-llvm.sh "$HOME/oxdk-llvm"
# 若源码已下载或构建中断，从断点继续
cd "$HOME/oxdk-llvm"
ninja -C build -j2 clang lld llvm-ar llvm-nm llvm-objcopy llvm-objdump
```

将工具链加入当前 shell：

```bash
export OXDK_LLVM="$HOME/oxdk-llvm/build"
export LLVM_PREFIX="$HOME/oxdk-llvm/build"
export PATH="$HOME/oxdk-llvm/build/bin:/usr/bin:/bin"
```

3. 编译 OXDK 样例：

```bash
cd "$OXDK_DIR"
make -C xbox360/samples/abitest
make -C xbox360/samples/d3dclear
```

成功产物为：

```text
xbox360/samples/abitest/default.xex
xbox360/samples/d3dclear/default.xex
```

`abitest` 运行画面应显示 `ALL CHECKS PASSED`。真实开发机部署需要 XBDM
地址；Xenia 或采集卡环境可用于提供外部运行截图，但不能替代真实 XBDM
日志验收。

> 注意：OXDK 脚本从 Windows 工作区挂载时可能带 CRLF 换行。若出现
> `'$'\\r': command not found`，先执行 `sed -i 's/\\r$//'` 修复脚本副本，
> 或在 WSL 原生目录中运行 OXDK。

当前环境补充：本机已有 `/root/xdk360`、`/root/oxdk-llvm/build` 和 OXDK，
但没有 Xbox 360 开发机。因此可以继续进行 Xenon 交叉编译和链接；XBDM
部署、实机日志、截图和 Present/GPU 行为验证暂缓，不作为当前源码推进的阻塞条件。

### M1：NanoVG Xbox 360 D3D9 后端验证（L4）

目标：验证 `codecat/nanovg_d3d` 或自有 backend 能否**移植为 Xbox 360 XDK D3D9 实现**，并提供 borealis 所需的 NanoVG API。PC D3D9 的实现只能作为算法和接口参考，不能直接作为目标二进制或默认 ABI。

任务：

- [x] 确认 `nanovg_d3d` 的目标 API 包含 D3D9 路径（提交 `488f7ab`）。
- [x] 清点并隔离 PC-only 依赖：Win32 窗口示例不进入 Xbox 360 平台层；设备创建和 Present 留给 M2。
- [x] 建立独立 `nanovg_d3d_xbox360` 样例，使用 XDK 的 `d3d9.lib`/相关 XDK 库；已生成 `xbox360/samples/nanovg_d3d9/default.xex`。样例退出路径已改为保持 title 运行，不再调用 `HalReturnToFirmware`。
- [x] 增加 XDK `fxc` shader 生成脚本，目标为 `vs_3_0`/`ps_3_0`，对应 Xenon `xvs_3_0`/`xps_3_0`；实际 XDK shader 已执行（XDK fxc 2.0.21256.0）。
- [ ] 验证 Xbox 360 特有的线性/非线性纹理、对齐、显存与资源池约束。
- [x] 验证基础 path fill、抗锯齿和 alpha blend（真实 XDK 已显示蓝色圆角矩形）；stroke、stencil 和复杂状态仍待扩展样例。
- [ ] 验证图片创建、更新、删除和纹理尺寸查询。
- [ ] 验证字体加载、字体 atlas 和 UTF-8 文本。
- [ ] 验证 D3D 状态保存/恢复，避免影响 borealis 或播放器。
- [ ] 验证 480p/720p/1080p 显示模式和宽高缩放。

M1 当前已完成的源码工作：

- `library/borealis/library/lib/extern/nanovg_d3d` 固定在上游提交 `488f7ab`；
- `PLATFORM_XBOX360`、`XBOX360_NANOVG_D3D9` 和无 GLFW/OpenGL 的 toolchain 分支已接入；
- `nanovg_d3d9_backend.cpp` 和配套 `nanovg.c` 已用 Xenon clang 成功编译；
- 已处理 XDK `D3DDevice` 类型别名、缺失桌面 usage/lock 标志、缺失 sRGB sampler state，以及 `renderTriangles` 签名差异；
- 具体编译命令和 ABI 说明见 `docs/XBOX360_M1_NANOVG_D3D9_AUDIT.md`。

M1 尚未完成的部分主要是 GPU/纹理/Present 验证。独立 NanoVG XEX 已完成链接并可交给 Xenia Canary 启动；XDK shader 头文件已由本地 fxc 生成。Xenia headless 启动回归能够验证 XEX 加载路径，但不能替代真实 XBDM/GPU 验收。

验收：样例能绘制矩形、圆角、渐变、图片和中英文文本，并连续运行 30 分钟无资源泄漏。

阻塞条件：如果 `nanovg_d3d` 依赖桌面 Win32、D3D9Ex、PC shader/runtime 或 Xbox 360 不具备的特性，则 fork 后端或改用自有的 Xbox 360 D3D9 实现。即使 PC 样例能运行，也不能作为 Xbox 360 兼容性的验收依据。

### M2：borealis Xbox 360 平台层（L4）

> **M2 编译已完成**（2026-09-21）：borealis 核心库、所有视图组件和 wiliwili
> 应用代码均通过 Xenon clang 编译，并由 M3 链接验证。平台层骨架已就位，
> 但 Present 和真实硬件渲染尚未验收。

当前进度：已加入 `xbox360` 平台目录、`Xbox360Platform` 工厂分支、NanoVG D3D9
视频上下文，以及 XInput/IME/字体加载器接口实现。XInput 已完成首个手柄的连接检测、
A/B/X/Y、方向键、肩键、摇杆、扳机和震动映射；字体加载器支持用户 regular/icon/emoji、
内置 switch 字体和 material fallback。IME 仍为最小拒绝实现（2026-10-04 起打开时给出 i18n
提示，不再静默失败），完整文本输入仍待后续里程碑；设备状态恢复、Present 稳定性和
borealis demo 验收仍待完成。当前实现属于平台层骨架，不能视为 M2 验收完成。

目标：让 borealis 的基础应用在 Xbox 360 上运行，但暂不接 Bilibili 播放。

建议新增目录：

```text
library/borealis/library/include/borealis/platforms/xbox360/
library/borealis/library/lib/platforms/xbox360/
```

任务：

- [x] `Xbox360VideoContext`：已接入 XDK/Xenon D3D9 device、backbuffer、frame 生命周期和 Present 骨架；设备创建/Present 错误现在记录日志，NanoVG frame 所有权统一由 `Application::frame()` 管理；设备丢失/恢复和真实硬件稳定性仍未验收。
- [x] `Xbox360InputManager`：XInput 状态、基础焦点导航按钮和震动映射。
- [ ] `Xbox360InputManager`：重复按键/长按时间戳与多手柄聚合。
- [x] `Xbox360Ime`：已提供最小拒绝实现；2026-10-04 起打开文本/数字输入时给出 i18n 提示，完整文本输入仍不支持。
- [x] borealis `Threading`：Xbox 分支启用真实 `task_loop` 后台线程（2026-10-04），`brls::async` 队列可正常排空（此前为只进不出的死队列）。
- [x] `Xbox360FontLoader`：用户字体路径、内置字体和 material/emoji fallback。
- [x] `Xbox360Platform`：已接入工厂、窗口尺寸和平台对象生命周期骨架；完整初始化、退出、计时和资源路径仍待实现。
- [x] 将 NanoVG context 注入 `brls::Application`；运行时显示验收仍待完成。
- [x] 为 `commonOption.cmake` 增加 `PLATFORM_XBOX360`。
- [x] 为 CMake 增加 Xbox 360 配置入口：自动选择 `OXDK_LLVM` 的 Xenon clang，
  注入 XDK 头文件和 freestanding ABI 选项；当前仍待补齐 XEX 链接/库清单。

验收：borealis demo 或最小 wiliwili 页面可以显示、导航、退出；无网络、无播放器依赖。
当前阻塞：`Application::frame()` 已负责一次 `nvgBeginFrame/nvgEndFrame`，Xbox
`Xbox360VideoContext::beginFrame/endFrame` 仍重复调用 NanoVG，需要先统一 frame 所有权，
再进行 XEX 和真实硬件验收。

当前补充：CMake 已真正复刻 OXDK `OXDK360_LIBCXX=1` 模式，加入 `-nostdinc++`、
匹配版本 libc++ headers/config/cshim、Xenon ABI 和 `-fno-exceptions`；fmt 的桌面
`os.cc` 已在 Xbox 360 条件下排除，Yoga/NanoVG/C 侧 CRT 和 `inttypes.h` shim
也已接入。borealis 编译已推进到核心视图；剩余阻塞是既有代码无条件使用
`throw`/`try`，以及 libc++ cshim 缺少 `roundf`。这些必须按 OXDK 的真实限制逐点
裁剪，不能重新打开 exceptions，也不能混入宿主机 STL。

#### 2026-09-20 M2/M3 OXDK 编译推进记录

本轮继续使用 `H:/OXDK` 和 `/root/oxdk-llvm/build/bin/clang++` 的 Xenon ABI
工具链，保持 `-fno-exceptions`。已完成或正在验证的兼容修复：

- borealis 增加 `PlatformLockGuard`/`PlatformUniqueLock`，Xbox 使用单线程 no-op
  mutex RAII；其他平台仍映射到标准库锁。
- 删除 Xbox mutex cshim 中与 libc++ 内部 `lock_guard` 冲突的重复定义。
- wiliwili 中可无异常实现的解析改为显式返回值检查：坏 JSON 使用
  `json::parse(..., false)`，整数/颜色使用 `strtol`/`strtoul`，gzip 解压失败返回
  空串。
- `XNetRandom` 用于 UUID/随机文本；本地 XDK 文档确认声明位于 `winsockx.h`、
  链接需求为 `xnet.lib`，成功返回 0。源码使用等价声明避开 XDK 头文件顺序冲突。
- OXDK 自带 zlib 1.1.4 已作为 `xbox360_zlib` 静态目标接入，替代宿主 `-lz`。
- 修正 `_WIN32` 在 Xenon 上误选 Desktop Windows 分支的问题；Xbox 配置路径暂定
  `game:\\config\\wiliwili`，文件存在性使用 `fopen`。
- Xenon 为大端，直播/弹幕包的 `hton*`/`ntoh*` 路径使用恒等转换。

本轮明确的临时裁剪（不得视为对应功能已完成）：

- Xbox 构建排除 DLNA Activity/API；M3 不包含 DLNA，后续单独评估恢复。
- 进一步排除 `player_dlna_search.cpp`，避免 Fragment 仍把 `DLNAActivity`/`DlnaRenderer`
  拉入链接；M3 不提供 DLNA 导航入口。
- `ImageHelper` 在 M3 仅保留纹理缓存命中；远程图片请求会安全清理并返回。
  待 M4 HTTP worker 接入后恢复下载、取消和共享 curl handle。
- Xbox 暂不启动 CPR 全局线程池，不读取环境代理，不执行在线版本检查。
- Xbox CMake 配置跳过宿主 `FindThreads`，不再把 Debian 的 `-lpthreads` 传给
  `oxdklink`；线程能力由 OXDK/XDK 路径另行提供。
- OXDK 链接前恢复调用 `cmake/strip_deplibs.cmake`，移除 clang 生成对象中的
  `libc++.lib` 自动链接元数据；该元数据不能交给 Xenon `ld.lld` 解析。2026-09-20
  晚些时候发现归档成员内部同样携带 `.deplibs`（如 `libborealis.a` 的全部成员），
  ld.lld 照样读取并要求 `libc++.lib`，因此该脚本已扩展：对链接输入里的每个 `.a`
  先 `llvm-ar t/x` 提取成员，`llvm-objcopy --remove-section=.deplibs` 清理后按原
  成员顺序 `llvm-ar rcs` 重组。长期修复是让所有目标以 `-fno-autolink` 编译（编译
  命令已含该标志），归档重建只是对既有产物的兜底。
- Xbox 360 工具链的 `CMAKE_CXX_LINK_EXECUTABLE` 已接真实 `oxdklink.py`：
  对象在 `--` 分隔符之前传给脚本，`<LINK_LIBRARIES>` 在其后；`strip_deplibs`
  作为 PRE_LINK 步骤先于链接执行。注意工具链文件在 `project()` 之前运行，
  其 `add_compile_options` 不会进入目标编译命令，实际生效的编译标志（含
  `-fno-autolink`、`-D_WIN32` 等）由根 `CMakeLists.txt` 的 Xbox 分支提供。
- WSL 构建辅助脚本：`scripts/build_xbox360_wsl.sh`（前台增量构建，日志写
  `build-xbox360-current.log`，结尾追加 `DONE_<exit>`）。WSL 会话退出会杀掉
  其后台子进程，长链接需通过 Windows 计划任务 `WiliwiliXboxBuild`
  （`schtasks /Run /TN WiliwiliXboxBuild`）独立启动，避免会话终止导致构建中断。
  计划任务的 `/TR` 必须写 `C:\Windows\System32\wsl.exe` 全路径，否则 `/Run`
  会报 “system cannot find the file specified”。
- 2026-09-20 复现并修正了 `strip_deplibs.cmake` 的两个缺陷：
  1）Ninja 链接规则里 `<OBJECTS>` 只展开为对象文件，静态库走 `<LINK_LIBRARIES>`，
     因此脚本只收到对象、归档从未被清理；链接模板已改为
     `strip_deplibs -- <OBJECTS> <LINK_LIBRARIES>`。
  2）`llvm-ar t` 输出的是按行分隔的成员列表，`separate_arguments` 不按换行切分，
     导致 `llvm-ar rcs` 收到一个含换行的巨型文件名而失败；改为
     `string(REPLACE "\n" ";" ...)` 之后归档重组成功（`libpdr.a` 验证通过）。
  脚本另外改为“先生成临时归档、成功后替换”，因为旧版本会先删除原归档。
- 2026-09-20 文件丢失记录：旧版 `strip_deplibs.cmake` 在重组失败前已删除原归档，
  调试期间因此丢过一次 `build-xbox360-m2/library/borealis/library/libborealis.a`
  （构建产物，非源码）；重新构建已自动恢复。源码树本身未丢失文件。
- libc++ 的编译部分（iostreams、locale、streambuf、stdexcept、typeinfo 等）在
  Xenon 上没有预编译库。按 OXDK 的 `OXDK360_LIBCXX_LIB=1` 路线，用
  `scripts/build_xbox360_libcxx.sh` 调用 OXDK 自带的
  `xbox360/oxdk360/libcxx-lib/Makefile`（`LIBCXX_SRC=/root/oxdk-llvm/libcxx`）
  生成 `build-xbox360-m2/libcxx-xenon.a`，并由 CMake 目标 `xbox360_libcxx`
  保证它先于链接存在。标志通过 `scripts/print_cxxflags.mk`（设置
  `OXDK360_LIBCXX=1`、`XENON_ABI=1`、`OXDK360_LIBCXX_DIR`、`OXDK_ROOT`、
  `OXDK360_DIR`、`CLANG`）从 OXDK 自己的 makefile 取出，确保头文件与编译部分同版本。
  注意 `XENON_ABI=1` 才会带上 `-Xclang -target-feature -Xclang +xenon-abi`，
  缺它会因 `va_list` 类型不匹配在 XDK `swprintf.inl` 处报错。
- 新增 `wiliwili/source/platform/xbox360/xbox360_crt_shims.c`（静态库
  `xbox360_platform_shims`）：补齐 libcMT/OXDK 都没有的符号，目前包括
  `strtoll`/`strtoull`、`round`/`roundf`、`fminf`/`fmaxf`、
  `_BitScanReverse`/`_BitScanForward`、`_InterlockedIncrement/Decrement`，
  以及 `WriteConsoleW`（主机无控制台，返回失败）和 `_Unwind_Resume`
  （`-fno-exceptions` 下不应到达，到达即 abort）。这些是真实实现而非占位。
- 自定义主题目录扫描和 mpv shader pack load/save 暂时 no-op；资源打包和 Xbox
  文件系统目录创建完成后恢复。
- D3D NanoVG 尚无 borealis fork 的 `nvgFontDilate` 扩展；Xbox 暂时跳过字体膨胀，
  因此弹幕 stroke 效果未验收。
- Windows `dbghelp` minidump 明确不用于 Xbox；Xbox 崩溃日志/转储仍待独立实现。
- `VideoProfile` 在 M3 不读取 libmpv 的 `mpv_node` 缓存和编解码统计，Xbox 上暂时
  显示 `Unavailable`；M5/M6 接入 libmpv 或 Xbox360PlayerCore 后改为读取等价播放器诊断状态。
- 登录、直播和视频 API 源文件当前只要求通过无异常编译，不代表 M4/M5/M6 已接通。

当前链接阶段仍有明确阻塞：M3 的 CPR/HTTP worker 暂未实现，导致 `cpr::`、`curl_*`
符号未解析；OXDK libc++ 运行时还缺少 locale/iostream、部分 C 数学/字符串和
stb_image 实现。这些必须在 M4 transport/runtime slice 中补齐，不能链接宿主 Linux
`libc`/`libstdc++` 代替。

> **2026-09-21 更新：链接阻塞已全部解决。** M3 shims（`xbox360_m3_shims.cpp`）
> 提供了 cpr/curl/filesystem/chrono/stb_image/dynamic_cast/iostream BSS 等全部
> 缺失符号的 stub 实现；`title.ld` 增加了 `.got` 段放置规则。wiliwili 已完整链接为
> Xbox 360 XEX。上述"链接阻塞"条目已过时，保留作为历史记录。

> **2026-09-21 路径规范化：** Xbox 360 SDK 要求路径只用反斜杠（`/` 是保留字符）。
> 已实施修复：`BRLS_RESOURCES` 改为 `"game:\\resources\\"`，`resourcesDir()` 函数
> 自动将 `/` 转 `\`，`BRLS_ASSET` 统一返回 `std::string`，`CFG_PATH_SEP` 宏
> 控制 `config_helper` 中的拼接分隔符。参考了 ButterAndJelly 的 `platform_xenon.cpp`
> 的 `NativePath` 实现。该路径方案已在 2026-09-25 Xbox 360 实机验收中确认可用。

上述临时裁剪仍属于 M3 离线验证范围，网络/API 和播放器能力按 M4/M5 继续补齐。

> **2026-09-25 更新：M3 已通过 Xbox 360 实机验收。** XEX 已启动并稳定显示离线
> `HintActivity`，NanoVG、字体、XML、图片和 XInput 焦点导航均正常。下一步是
> M4（网络/API 层接入真实 Bilibili 数据）。详细计划见下方"下一步工作"章节。

### M3：wiliwili UI 最小集成（L3）

> **M3 已完成编译和链接**（2026-09-21）。完整的 wiliwili 代码库（145 个
> object 文件 + 9 个 XDK 库）通过 `oxdklink` 成功链接为 Xbox 360 XEX。
> 产出文件 `build-xbox360-m2/wiliwili`（~7.6 MB），base 0x82000000，
> entry 0x82675828。

目标：先集成 UI 和本地资源，隔离播放器复杂度。

任务：

- [x] 注册 Xbox 360 平台实现和 XML 视图；Xbox 360 首个入口暂时打开离线 `HintActivity`，隔离未完成网络/播放器。
- [x] **完整 wiliwili 编译和链接**：所有 activity、fragment、presenter、view、utils、api 源文件均通过 Xenon clang 编译并链接成功。
- [x] **M3 linker shims**（`xbox360_m3_shims.cpp`）：提供所有缺失符号的 stub 实现，包括：
  - cpr HTTP transport（Session、CurlHolder、CurlMultiHolder、MultiPerform、Parameters、Payload、Proxies、Cookies、util、CurlContainer\<Pair\>/\<Parameter\>）
  - curl easy/share API stubs
  - std::filesystem（path::__filename、directory_iterator、__create_directories、__status、__wide_to_char、__char_to_wide）
  - std::chrono::system_clock（now/from_time_t/to_time_t）
  - std::stoi/stoll/to_string、stb_image、__dynamic_cast
  - MSVC ABI BSS blobs（cerr/cout/clog/wcerr/wcout/wclog/cin/wcin）
  - __cxxabiv1 vtable stubs（pointer_type_info、function_type_info）
- [x] **linker script 修复**：`title.ld` 增加 `*(.got) *(.got.*)` 到 `.data` 段，解决 `.got` 与 `.rdata` 地址重叠。
- [x] **路径规范化**（2026-09-21）：
  - `BRLS_RESOURCES` 从 `"resources:/"` 改为 `"game:\\resources\\"`
  - `resourcesDir()` 函数自动将 `/` 转 `\`（参考 ButterAndJelly 的 `NativePath` 实现）
  - `BRLS_ASSET` 统一返回 `std::string`，所有 `std::string(BRLS_RESOURCES) + "xml/"` 改为 `BRLS_ASSET("xml/")`
  - `config_helper.hpp` 新增 `CFG_PATH_SEP` 宏，Xbox 360 用 `"\\"`
  - `shader_helper.cpp` 同步修复拼接
- [x] **路径验证**：`game:\config\wiliwili` 可写，`game:\resources\` 资源可正常读取，字体、XML 和图片加载通过实机验证。
- [ ] 将离线静态页面替换为首页、设置页、搜索页和静态视频详情页。
- [ ] 运行首页、设置页、搜索页和静态视频详情页。
- [ ] 验证图片加载、SVG、滚动、RecyclingGrid 和焦点路由。
- [ ] 实现 Xbox 360 资源路径和资源打包方式。
- [x] 增加平台专用输入映射，不改变通用 `Intent` 导航；D-pad 和左右摇杆可驱动 borealis 焦点导航。
- [ ] 暂时禁用或裁剪需要文本输入的功能。

验收：XEX 可在真机启动，UI 可连续使用；页面返回和退出确认流程正常。M3 已于
2026-09-25 通过实机验收。

当前阻塞（M3 已解除）：
- 所有 M3 shims 都是空实现，实际运行时 cpr HTTP 请求会返回空数据，
  filesystem 操作会返回默认值，chrono 返回 epoch。这些限制不影响已验收的
  离线 UI 入口，但会阻塞后续网络/API 功能。

#### 2026-09-25 实机验收记录

- 当前 Xbox 360 入口在 `main.cpp` 中有意打开离线 `HintActivity`，用于验证 UI
  框架和本地资源，不是首次运行向导，也没有完成或跳过状态。
- 页面中的 NS 图片和文案（包括 “Select a game from home screen” 以及 NSP
  forwarder 说明）来自项目原有的 Switch 安装教程资源；Xbox 360 构建只展示这组
  本地静态内容，不会执行 Switch NSP 安装流程。
- `HintActivity` 是根 Activity。按 B 会触发 borealis 的返回逻辑；没有上一级
  Activity 时显示退出确认对话框，这是预期行为。选择取消可回到教程页面，选择确认
  才退出应用。
- D-pad、左右摇杆、LB/RB/LT/RT 和 A/B 均已在实机验证可用；教程左右页可通过
  D-pad/摇杆或 LB/RB/LT/RT 切换。
- 初次实机运行时发现教程 PNG 图片颜色异常，背景、文字和交互控件颜色正常；按
  XDK 的 32 位纹理 `GPUENDIAN_8IN32` 规则，在 NanoVG RGBA 纹理创建和更新时执行
  每像素字节重排。重新构建部署后，已确认图片颜色正常。

### M4：Xbox 360 网络/API 层（L3）

> **M4 首页网络链路已完成实机验收**（2026-09-26）：XNet/WSA 初始化、XNet DNS、mbedTLS/curl/cpr 接入、WBI 请求和首页数据加载均已在真实 XDK 上验证。

目标：在保留现有 API 数据模型的前提下接入已验证的 Xbox 360 网络实现。

推荐优先复用 `bili360-main` 的 libcurl + mbedTLS，而不是立即切换 XboxTLS。

任务：

- [x] 将 `HttpRequest` 抽象成 wiliwili 可使用的响应接口。
- [x] 初始化 `XNetStartup`，再调用 `WSAStartup`。
- [x] 接入 XNet DNS resolver。
- [ ] 接入 CA bundle、hostname 校验、证书有效期校验（当前开发机未部署 CA bundle，Xbox 分支暂时关闭 TLS 校验）。
- [x] 实现 GET、POST、Cookie、Range、超时和取消所需的 cpr/curl transport。
- [x] 保留 WBI 参数签名逻辑。
- [x] 将 cpr 异步接口接入现有线程/回调队列。
- [x] 限制响应体、日志和 Cookie 输出，避免泄露凭据。

当前实现使用 `XBOX360_BILI360_DIR`/`BILI360_DIR` 指向的 mbedTLS 与 vendored curl，
并通过 cpr 接入 wiliwili 的异步 API。已处理旧 curl 在 Xenon ABI 下的
`Content-Length` 和 chunked 长度解析问题；否则首页请求会在已收到完整响应后继续等待并超时。
CA bundle/hostname/证书有效期校验仍需后续补齐后再恢复 TLS 校验。

#### 2026-09-26 实机验收记录

- 按“先启动 XBDM 日志监听，再部署并启动 XEX”的顺序验证。
- WBI 请求返回 HTTP 200；259 字节固定长度响应和约 41 KB chunked 响应均正常结束。
- 首页 Explore 页面加载出视频卡片，不再显示 `WBI签名获取失败` 或 `Network error.[Status code: 0]`。
- 最终截图：`/tmp/wiliwili-xbox360-m4-final.png`。

WSL 验证命令：

```sh
cd /mnt/h/wiliwili
export OXDK_DIR=/mnt/h/OXDK
export XDK_DIR=/root/xdk360
export OXDK_LLVM=/root/oxdk-llvm/build
export LLVM_PREFIX=/root/oxdk-llvm/build
export BILI360_DIR=/mnt/h/bili360-main
export PATH=/root/oxdk-llvm/build/bin:$PATH

rm -rf build-xbox360-tls
cmake -S . -B build-xbox360-tls -G Ninja \
  -DPLATFORM_XBOX360=ON \
  -DXBOX360_BUILD_MBEDTLS=ON \
  -DXBOX360_BUILD_CURL=ON \
  -DXBOX360_ENABLE_SSL=OFF \
  -DDISABLE_OPENCC=ON \
  -DDISABLE_WEBP=ON \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-xbox360-tls --target xbox360_mbedtls xbox360_curl -v
```

验收：匿名推荐、视频详情、`view/playurl` 和 QR 登录轮询可以完成；HTTPS 证书错误会失败关闭。

### M5：FFmpeg + MPV 可行性评估与最小移植（L5）

> **首次播放闸门已通过（2026-09-30）**：真实 Xbox 360 已能从视频详情页加载并播放 Bilibili 视频。当前仅证明解复用、TLS、解码、MPV 事件和视频输出链路可以贯通，不代表性能或完整播放器行为已经达标。

目标：优先把 MPV/libmpv 移植到 Xbox 360，保持与上游播放器实现同步。M5 首先是一个有明确退出条件的可行性评估，不以“能编译一部分 MPV”作为成功标准。

#### 当前初步评估

- **上层复用价值高**：`VideoView`、播放器 Activity、`MPV_E` 事件和 `VideoProfile` 已直接围绕 `MPVCore` 建模，成功移植 libmpv 可以显著减少 Xbox 360 专用播放器分叉。
- **核心技术风险高**：当前 Xbox 360 已验证的 `ffmpeg-xenon` 是 FFmpeg 1.2.x 路线；现代 MPV 通常依赖更新的 FFmpeg API、Meson 构建逻辑和更多 C/POSIX runtime 能力，版本必须先固定再评估，不能假设直接兼容。
- **后端风险可控但工作量大**：MPV 的视频输出需要接入 Xbox 360 XDK D3D9，音频输出需要 XAudio2；M1/M2 已验证的图形和平台骨架可以复用，但不等于 MPV renderer/backend 已可用。
- **网络路径可复用**：M4 的 XNet/libcurl transport 可以作为 MPV 的网络输入基础，但需要确认 MPV demuxer 对独立 DASH 视频/音频 URL、Range、取消和超时的调用方式。
- **结论**：MPV 路线已经通过“能在真实 Xbox 360 上启动并开始播放”的初步可行性检查，继续作为当前主线；仍保留 Xbox360PlayerCore 作为性能或兼容性无法达标时的退出路径。

任务：

- [ ] 固定 MPV、FFmpeg-xenon、XDK 和 Xenon clang/lld 版本，记录对应提交和补丁。
- [ ] 清点 MPV 对 FFmpeg API、pthread/线程、原子操作、动态加载、文件系统、时间/时钟、网络协议、iconv、Lua/脚本、音频输出和视频输出的依赖。
- [ ] 建立不依赖桌面窗口系统的最小 libmpv 构建配置，关闭 Lua、脚本、Wayland/X11/SDL、桌面音频后端和不适用于 XDK 的动态模块。
- [x] 验证 MPV 核心、FFmpeg 解复用/解码、消息队列和播放事件在 Xenon ABI 下编译、链接并运行。
- [x] 把 cbffmpeg P4 产出的 `libmpv_xbox360_compat.a`、FFmpeg 六静态库与
      `xbox_float_math.o` 链接进 wiliwili 本体（归档与顺序见 `CMakeLists.txt`
      §libmpv，文档在 `../cbffmpeg/docs/wiliwili_mpv_embed.md`）；`mpv_core.cpp`
      以 `-DMPV_SW_RENDER=ON` 软渲染路径编译。这是首次播放闸门的链接前提。
- [x] 让 MPV/FFmpeg 使用 Xbox 360 网络路径加载 Bilibili 视频 URL；独立视频/音频 URL 已能进入 demuxer 并开始播放。Range、取消、重试和超时仍需专项验证。
      零售机曾因 FFmpeg socket 未做 SNL 0x5801 标记导致所有流静默超时，已于 2026-10-11 修复
      （见 §10「2026-10-11 零售机视频流播放修复」）。
- [ ] 评估 FFmpeg 1.2.x 与目标 MPV 版本的 API/ABI 兼容性；若必须升级 FFmpeg，记录 Xenon 编解码器、线程和内存预算影响。
- [ ] 评估 MPV 视频输出：优先复用/扩展 Xbox 360 D3D9 backend；确认 YUV 纹理、shader、pitch、资源池和 UI overlay 状态恢复。
- [ ] 评估 MPV 音频输出：实现或接入 XAudio2 backend，并以音频时钟驱动视频同步。
- [ ] 在脱离 wiliwili 的最小播放器样例中完成匿名 480p 的加载、播放、暂停、seek、停止、EOF 和错误路径；目前完成的是 wiliwili 视频详情页中的首次播放，不是完整独立样例验收。
- [ ] 输出 `docs/XBOX360_M5_MPV_FEASIBILITY.md`，包含依赖矩阵、补丁清单、XEX 大小/内存/线程记录、实机日志和继续或退出建议。

验收闸门：第一阶段“真实 Xbox 360 可加载并开始播放”已通过。完整闸门仍要求匿名 480p DASH 能稳定播放、暂停、seek、停止并自然 EOF，且 XDK 线程退出、音视频同步和 UI overlay 没有已知死锁或状态破坏。性能方面：点播 720p 窗口化/全屏卡顿已于 2026-10-02 定位并修复（回写 M7），直播 1080p 对策 round-6 已部署待验证（2026-10-04），整体性能验收仍未完成；若优化后仍无法达到目标，再评估 `Xbox360PlayerCore` 兜底路线。

性能目标：先达到 480p 稳定播放，再评估 720p；不把 1080p/硬件解码作为第一版门槛。点播卡顿根因（非 1:1 几何下 mpv 默认 `SWS_LANCZOS` 软件重采样 440ms/帧）已于 2026-10-02 定位修复；直播 1080p 卡顿根因与对策见 `XBOX360_PLAYBACK_PERFORMANCE_PLAN.md` §14，round-6 待实机验证。下一步回到完整闸门验收（暂停、seek、停止、EOF、线程退出）。

### M6：视频页和播放器生命周期集成（L4）

> **首次视频页播放已验证（2026-09-30）**：用户从视频详情页手动开始播放后，MPV/FFmpeg 已完成真实网络加载并输出视频。该记录不等于生命周期验收；返回、重复播放、暂停、seek、EOF 和资源释放仍待逐项确认。

目标：把通过 M5 闸门的 libmpv 接入 `VideoView`、播放器 Activity 和全局事件；若 M5 退出到兜底路线，则接入等价的 Xbox360PlayerCore。两条实现都不能改变上层 wiliwili 的功能语义。

任务：

- [ ] 将 `VideoView` 中直接依赖平台播放器的调用收敛为稳定接口。
- [ ] 保留 Desktop/Switch 的 MPVCore 实现。
- [x] Xbox 360 当前选择 MPV/libmpv 路线并接入现有 `MPVCore` 播放路径；完整等价行为仍待验收。
- [ ] 映射音量、倍速、画面比例、暂停、重播和进度条。
- [ ] 处理 Activity 进入/退出、重复播放和播放失败。
- [ ] 确认 D3D9 UI overlay 与视频纹理不会互相破坏状态。

验收：从首页进入视频页、播放、返回首页、再次播放至少重复 20 次无泄漏或死锁。目前仅完成首次播放验证，尚未通过此项验收。

### M7：功能补齐和性能优化（L4）

目标：从“能播放”变成可长期使用的 Xbox 360 版本。

任务：

- [ ] 登录 Cookie 持久化和刷新。
- [ ] 搜索历史、收藏、观看历史和稍后再看。
- [ ] 弹幕和直播功能评估，默认不作为首发阻塞项。
- [ ] 封面下载缓存和纹理 LRU。
- [ ] 网络缓冲、断线重试和 CDN 切换。
- [ ] 内存预算、纹理预算和 worker 数量限制。
- [x] 定位并解决当前严重卡顿：记录解码 FPS、显示 FPS、丢帧数、缓冲状态、音频队列和主线程耗时。
  - 结果（2026-10-02，四轮实机采集，`XBOX360_PLAYBACK_PERFORMANCE_PLAN.md` §11–§13）：
    根因为 M1——窗口化时 surface 跟随视图 rect，mpv 默认 `SWS_LANCZOS`+HQ flags 在
    **纯标量 sws**（ffmpeg `HAVE_ALTIVEC=0`）下对 1280x720→800x450 需 **440ms/帧**；
    全屏因 surface 恰为 1280x720 命中 unscaled 拷贝而流畅。解码侧无罪
    （`decoder-frame-drop-count=0`，与 cbffmpeg P2 基线一致）。
  - 修复：**1:1 surface**（Xbox，`setFrameSize` 在宽高比一致时用视频原始尺寸 + `draw()`
    检测 `video-params` 变化触发）为主；`sws-scaler=area` + `sws-fast` 兜底回退路径。
  - 验证：720p 窗口化/全屏、640x360 窗口化均 30fps 视频 + 53–60fps UI；
    `[x360-perf]`/`[x360-sw]`/`[x360-mpv]` 探针留在树内供回归使用（日志行说明见性能计划 §10）。
  - 未录音频队列（当时未列为瓶颈，音频帧率 P2 已证 6269 帧/s 无压力）。
- [x] 直播卡顿排查与对策（2026-10-03 ~ 10-04，`XBOX360_PLAYBACK_PERFORMANCE_PLAN.md` §14）：
  - 焦点切换冻结 = Hint 销毁/重建风暴（每次焦点移动全量重建按钮 Hint，冻结 5.5~9.3s）；
    已用 Hint 复用池修复并实测通过（round-4 定位 / round-5a 验证）。
  - 堆锁“单点巨持锁者”假说在已钩路径上证伪（round-5b）：真实形态是 CPU 饱和下锁持有者
    被时间片抢占引发的全网护航，堆慢行是受害者/放大器而非第一因。
  - 主根因实锤 = 1080p 直播源 × surface 1280x720 上限 → 每帧标量重采样 254~359ms；
    服务端实测约 85% 房间无视 qn=250 返回原画，1080p 直播不可回避。
  - 对策（用户决策“PSV 怎么处理 360 就怎么处理”，仅 `PLATFORM_XBOX360`）：surface 上限
    提到 3840x2160，1:1 surface + `Xbox360YUVRenderer` viewport 硬件缩放。
  - round-6 已部署（XEX md5 `8d31a8579f8dcd9c26086b3fc171086e`）：1080p copy 路径、
    3049KiB/帧上传耗时、decoder 丢帧与 720p 回归待实机验证。
- [x] 空实现审计复核与 A 类修复（2026-10-04，`XBOX360_EMPTY_IMPL_AUDIT.md` §6）：
  `cpr::MultiPerform` 串行实现（恢复追番/影视分类筛选）、borealis `Threading` 真实后台线程、
  IME 打开时 i18n 提示、限帧 `sleep_for` 放行、`std::to_string(float/double)` 真实现；
  另修复 `build_xbox360_local.sh` 的 `XDK_DIR` 少一级与缺 `LLD` 两处从零构建缺陷。
  全量编译 424/424 + 链接通过（162 objects，image 0x13c0000）。B 类 12 条为有意保留的
  里程碑限制，本轮未动。
- [ ] 优化视频帧上传/纹理更新、D3D9 状态切换和 UI redraw，确认播放线程不会被 UI 或图片请求长期抢占。
- [ ] 评估 Xbox 360 硬件解码或更低开销的 H.264 视频输出路径；在确认软件解码性能瓶颈前不将其作为既定方案。
- [ ] 720p 性能、帧丢弃和音视频同步调优。
- [ ] XEX 资源打包、版本信息、崩溃日志和部署脚本。

验收：长时间浏览和播放不超出内存预算；异常网络和取消路径均能回到可操作 UI。

### M8：播放器路线兜底与长期维护（L5，可选）

目标：在 M5 的 MPV 路线无法满足内存、性能或 XDK 兼容性要求时，维护 FFmpeg + Xbox360PlayerCore 作为可发布的替代实现；若 MPV 通过闸门，则用于补齐 MPV 功能和上游同步。

任务：

- [ ] 比较 MPV 和 Xbox360PlayerCore 的内存、性能、补丁数量和维护成本。
- [ ] 为未被 MPV 覆盖的 XDK 能力补齐专用后端或适配层。
- [ ] 评估是否需要保留 MPV software renderer 或独立的视频输出路径。

退出条件：如果 MPV 需要大规模修改 MPV、FFmpeg 或 XDK runtime，或者无法达到 480p 稳定播放和生命周期验收，则继续维护 Xbox360PlayerCore，不阻塞主线。

## 5. 依赖和复用建议

### 优先复用

1. `bili360-main/src/network/`：XNet、libcurl、TLS 和流式输入。
2. `bili360-main/src/api/`：Bilibili JSON、WBI、DASH 选择逻辑。
3. `bili360-main/src/playback/`：D3D9 YUV renderer、XAudio2、XInput。
4. `ffmpeg-xenon`：已验证的 Xbox 360 FFmpeg 库和 pthreads。
5. `nanovg_d3d`：如果确实支持 Xbox 360 可用的 D3D9 路径。

### 不建议直接复用

- `bili360-main` 的 XUI 页面作为 borealis 页面替代品；
- 将当前 wiliwili 的 `MPVCore` 原样带到 Xbox 360，而不处理 Xenon/XDK 依赖；Xbox 360 可以使用等价的新实现；
- Desktop 的 GLFW/D3D11 后端；
- cpr 的完整异步实现，除非已经验证其线程和异常依赖适配 OXDK；Xbox 360 可以换用专用 transport，但 API 功能不能因此缩水。

## 6. 风险登记

| 风险 | 难度 | 影响 | 应对 |
|---|---:|---|---|
| PC D3D9 与 Xbox 360 XDK D3D9 不兼容 | L4 | PC backend、shader、纹理和资源管理无法直接使用 | 以 XDK/Xenon 为目标重写平台层；PC 代码只作为参考 |
| `nanovg_d3d` 实际不是 D3D9 backend | L4 | 无法直接接入 borealis | 先做独立 Xbox 360 样例；必要时 fork 或自写 XDK D3D9 backend |
| borealis 假设 OpenGL/D3D11 状态 | L4 | UI 绘制异常 | 参考 Switch/PSV 后端，集中封装状态切换 |
| libmpv 无法在 Xenon ABI 下编译 | L5 | MPV 路线中止 | 按 M5 退出条件使用 Xbox360PlayerCore，不阻塞主线 |
| FFmpeg 1.2 MOV/DASH 行为有限 | L4 | seek、时长和自然 EOF 异常 | 以 API duration 控制进度，先支持固定质量和基本播放 |
| Xbox 360 内存/纹理预算不足 | L4 | 长时间运行崩溃 | bounded ring、纹理 LRU、限制并发和响应体 |
| C++ 异常/线程 ABI 不兼容 | L4 | 链接或运行时失败 | 平台 worker、显式错误码、避免新增异常和桌面 libc++ 线程设施 |
| Bilibili API/CDN 变化 | L3 | 播放或登录失效 | 封装 API、保留备用 URL、增加网络 smoke 测试 |
| devkit/零售机行为差异 | L4 | devkit 验证通过的功能在零售机静默失效（如 SNL socket 标记，见 §10 2026-10-11） | 优先使用零售也支持的 API；每个网络/安全相关改动都要求零售实机复验 |
| D3D9 UI 与视频渲染互相破坏状态 | L4 | 花屏或 Present 失败 | 明确 renderer ownership，统一 begin/end frame 和状态恢复 |

## 7. 每个阶段的交付物

每个里程碑至少提交以下内容：

- 一个可重复执行的构建命令；
- 一个实机部署命令或部署说明；
- 一份测试日志或截图；
- 失败路径和已知限制；
- 内存、帧率、线程和资源生命周期记录；
- 不依赖开发机绝对路径的配置说明。

不要只提交“能编译”的结果。Xbox 360 平台问题很多只会在 XEX 加载、D3D Present、TLS 握手、线程退出或长时间播放时出现。

## 8. 推荐的首个开发切片

第一轮建议只做一个最小纵向切片：

```text
OXDK/XDK
  → D3D9 window/device
  → NanoVG D3D9 rectangle/text/image
  → borealis Application
  → 一个静态 wiliwili XML 页面
  → XInput A/B/方向键
  → 正常退出
```

完成这个切片后，再加入网络和播放器。这样可以先验证作者所说的“增加 borealis platform 模块就能跑”的核心假设，而不会把图形、网络、FFmpeg 和 mpv 问题混在同一次调试中。

## 9. 当前建议的完成定义

## 9.1 功能等价要求

平台实现可以不同，但下列能力不能因为使用 Xbox 360 专用代码而永久删除：

| 能力 | Xbox 360 允许的替代实现 | 完成要求 |
|---|---|---|
| MPV 播放控制 | Xbox 360 libmpv 或 `Xbox360PlayerCore` | 上层可完成播放、暂停、停止、seek、倍速、音量和进度更新 |
| MPV 事件 | Xbox 360 事件适配器 | 对 `VideoView`、Activity 和 `MPV_E` 提供等价状态 |
| cpr/libcurl | XNet + libcurl、XboxTLS 或专用 HTTP transport | API、Cookie、WBI、Range、超时、取消和错误语义完整 |
| borealis/NanoVG | Xbox 360 XDK D3D9 NanoVG backend | XML UI、字体、图片、裁剪、焦点和动画能力不因后端替换而消失 |
| SDL2/输入 | SDL2x360 或直接 XInput | 手柄导航、快捷键、长按、返回和播放器按键行为完整 |
| D3D9 视频输出 | SDL2 YUV renderer 或专用 Xbox 360 D3D9 renderer | 不得把质量、分辨率、同步和 overlay 永久锁死在验证样例级别 |

M0–M5 中可以有明确的临时限制，例如只支持匿名 480p、暂不支持完整输入法或只实现基础 seek；这些限制必须记录在对应验收报告中，不能被写成最终平台设计。

### Alpha

- UI 首页和设置可操作；
- API 推荐和视频详情可用；
- 匿名 480p H.264/AAC 可播放；
- XInput、暂停、停止、返回正常；
- 不依赖 MPV。

### Beta

- 登录和 Cookie 持久化；
- 720p 播放；
- 播放页和首页可反复切换；
- 网络失败、取消、EOF 路径稳定；
- 纹理和内存预算受控。

### 长期版

- 功能和现有 wiliwili 的目标子集一致；
- Xbox 360 平台代码与通用 UI/API 解耦；
- Desktop/Switch 构建不受影响；
- MPV 作为优先主线；如果 M5 闸门失败，Xbox360PlayerCore 作为已记录的兜底实现。

## 10. 下一步工作（最近更新 2026-10-11）

### 当前状态
M3 编译和链接、M4 首页网络/API、首次视频播放均已完成实机验证；M7 点播卡顿根因已定位并实机验证修复
（2026-10-02）；**零售机视频流播放已修复并在 devkit + 零售两端实机验证（2026-10-11，见下）**。
当前 XEX 产出于 `build-xbox360-local/wiliwili`（24,457,216 字节，162 objects，镜像 `0x1750000`）。
下一步优先完成直播 round-6 验证与 M5/M6 完整验收闸门（暂停/seek/停止/EOF、20 次进出循环无泄漏）。

#### 2026-09-30 播放链路实机验证记录

- 视频详情页可以正常进入，并能通过现有 MPVCore 播放路径请求 Bilibili 视频。
- FFmpeg 4.4.8 + MPV 0.36.0 已在真实 Xbox 360 XEX 中完成 HTTPS 视频加载和首次播放。
- FFmpeg mbedTLS 后端的 `mbedtls_ctr_drbg_seed()` 已移到 `MBEDTLS_FS_IO` 条件块之外；Xbox 关闭文件系统证书加载时仍会正确初始化 CTR-DRBG。
- 修复后未再出现 `mbedtls_internal_aes_encrypt` / `mbedtls_ctr_drbg_random` 引起的访问违规崩溃，播放已能开始。
- 当前限制：视频播放极其卡顿；尚未完成稳定 480p、暂停/seek/停止/EOF、重复进入退出、长时间播放和资源泄漏验收。

#### 2026-10-02 ~ 2026-10-04 性能排查与空实现审计更新

- **2026-10-02 点播卡顿根因与修复**（四轮实机采集，`XBOX360_PLAYBACK_PERFORMANCE_PLAN.md` §11–§13）：
  根因为非 1:1 几何下 mpv 默认 `SWS_LANCZOS`+HQ flags 在纯标量 sws 上的重采样
  （1280x720→800x450 需 440ms/帧）；以 **1:1 surface** 为主对策、`sws-scaler=area` + `sws-fast`
  为兜底，四场景矩阵中 720p 窗口化/全屏与 360p 窗口化均达 30fps 视频 + 53–60fps UI，已回写 M7。
- **2026-10-03 ~ 10-04 直播卡顿排查**（性能计划 §14）：修复焦点切换 Hint 重建风暴（复用池实测通过）；
  证伪堆锁单点持锁者假说；实锤主根因为 1080p 直播源 × 720p surface 上限导致每帧 254~359ms 重采样；
  按“PSV 怎么处理 360 就怎么处理”决策将 surface 上限提至 3840x2160，round-6 已部署（15:12）待验证。
- **2026-10-04 空实现审计复核**（`XBOX360_EMPTY_IMPL_AUDIT.md` §6）：A 类 5 条全部修复并通过
  `build-xbox360-local3` 全量编译链接（424/424，162 objects）；C 类 4 条属实已处理、2 条复核时已过期；
  B 类 12 条为有意保留的里程碑限制未动；另修复 `build_xbox360_local.sh` 的 `XDK_DIR`/`LLD`
  两处从零构建缺陷。TLS CA bundle（M4）、崩溃转储、虚拟键盘、多手柄聚合等仍属未完成的里程碑项。

#### 2026-10-11 零售机视频流播放修复：FFmpeg TCP socket SNL 标记（0x5801）

**症状**：devkit 播放正常，零售机（retail console）视频页所有 m4s 流在 ~5s 后静默超时
（`stream: Failed to open ...m4s` → `MPV ERROR: loading failed`），首页/API（curl 路径）正常。
XBDM 日志中没有任何 socket 错误码——连接"看起来成功"但对端回复被丢弃。

**根因**：Xbox 360 SNL（Secure Network Library）默认 secure 模式。

- `XNET_STARTUP_BYPASS_SECURITY` 是 devkit 专属能力；零售 xnet.lib 静默忽略该标志
  （XDK 文档 `xbox_secure_network_library_overview_xsnl.htm`、`xbox_xnetstartup_xsnl.htm`、
  `Networking_Secure_IN_ADDRs.htm`）。
- 即使在 devkit 上开启 bypass，未标记的 socket 也可能出现"连接成功但对端回复被静默丢弃"的行为。
- curl 之所以在零售上一直正常，是因为 `third_party/curl/lib/connect.c:1331-1343` 在**每个新 socket
  创建后**调用 `setsockopt(fd, SOL_SOCKET, 0x5801, ...)`（SNL "unencrypted" 每 socket 退出标记）。
- FFmpeg 的 `libavformat/tcp.c` 从未设置该标记 → mpv 流 socket 在零售上被 SNL 静默拦截。

**修复**（`third_party/ffmpeg-4.4.8/libavformat/tcp.c`，`customize_fd()` 增加 `_XBOX` 分支）：

```c
static void customize_fd(void *ctx, int fd)
{
    TCPContext *s = ctx;
#if defined(_XBOX)
    {
        int xnet_opt = 1;
        setsockopt(fd, SOL_SOCKET, 0x5801, (const char*)&xnet_opt, sizeof(xnet_opt));
    }
#endif /* _XBOX */
    ... /* 原有 SO_RCVBUF/SO_SNDBUF/TCP_NODELAY 逻辑不变 */
}
```

`customize_fd()` 由 `ff_socket()`（每个新建 TCP socket，含重试）和 `ff_connect_parallel()`
（并发 connect）调用，覆盖所有流 socket 生命周期。

**重要教训——不要同时设置 0x5802**：最初实现同时设置了 `0x5802`，结果在 devkit 上立即回归——
所有 connect() 立即返回 `WSAEHOSTUNREACH (10065)`（`[XNET:1] Warning: connect failed: 10065`）。
`0x5802` 在 bypass-security + 真实 IP 寻址组合下语义不明，**只设置 0x5801**，与 curl 的验证做法一致。
（对象码验证：llvm-objdump 确认 `22529`（0x5801）存在、`22530`（0x5802）不存在。）

**部署与验证**：

- 增量重建 `libavformat.a` → 重链 XEX（`build-xbox360-local/wiliwili`，24,457,216 字节，162 objects，
  重链日志 `logs/rebuild_x360_20261011b.log`）。
- devkit（192.168.2.9）实机验证：视频缓冲 216 KB/s，`src 1280x720 -> dst 1280x720` 渲染正常，
  截图 `logs/smoke_20261011_play.png`。
- 零售机验证：720p 视频正常播放（用户协助测试）。
- 零售机日志中的 `modload size=0x01750000`（新包镜像）可用于区分是否部署了带修复的 XEX
  （旧包为 `0x013e0000`）；部署新包后若仍失败，先确认日志中的 modload size。

**注意**：本修复只影响 FFmpeg/mpv 流 socket；首页/API 走 curl/cpr 路径（早已自带 0x5801），
两者互不影响。若零售机出现首页加载失败，应先排查该机 DNS/网络环境（曾出现 B 站域名被解析到
境外节点导致 curl error 56 的情况），而非回退本修复。

2026-09-21 更新：`bili360-main` 的 `third_party/curl`（libcurl 7.37.1，含
`config-xbox360.h` 和 `xbox360_stubs.c`）与 `third_party/mbedtls`
（mbedTLS 2.28.10）已复制到本仓库 `third_party/`，根 `CMakeLists.txt` 的
Xbox 360 分支优先使用仓库内副本，`BILI360_DIR` 环境变量仅作为回退。
构建不再依赖 `H:/bili360-main` 的绝对路径（OXDK/XDK/oxdk-llvm 工具链仍为
本机环境依赖，不入库，XDK 不可再分发）。

#### 2026-09-21 实机验证记录（devkit Cipher 日志）

第一轮修复后实机启动确认：

- **路径规范化生效**：i18n 目录、内置字体（switch_font/keymap/material/emoji）
  全部正常加载，不再出现 "doesn't exist" / 混合分隔符路径。
- **配置目录创建和写入生效**：`game:\config\wiliwili\wiliwili_config.json`
  成功写入并在重启后读取。
- **首个崩溃已定位并修复**：`AppletFrame` 构造中 `BoundView<Image>::resolve()`
  的 `dynamic_cast` 命中 M3 占位 stub（恒返回 nullptr）→ `std::terminate` →
  空 terminate handler → 跳转地址 0（IAR=0x0）。修复方式：
  - 编译 libc++abi `private_typeinfo.cpp`（位于 `$OXDK_LLVM/../libcxxabi`），
    提供 Itanium `__dynamic_cast` 与全部 `__cxxabiv1` typeinfo vtable；
  - M3 shim 中的 `__dynamic_cast`/vtable blob 桩已删除，改为提供
    `__abort_message`（DbgPrint 输出后 abort，崩溃原因可见于 XBDM 日志）；
  - OXDK `builtins.c` 的 4 个全零假 vtable 与 `_ZTIi` 假 typeinfo 在
    `oxdklink_xbox360.py` 暂存阶段用 `llvm-objcopy --weaken-symbol` 弱化，
    让真实定义获胜（修改 staging 逻辑后需删除
    `.oxdklink/inputs/*libxbox360_runtime.a` 再重链）。
- 构建脚本 `scripts/build_xbox360_wsl.sh` 改为 `-j12`。
- 新增调试脚本：`scripts/symbolize_xbox360.sh`（按 title.elf 符号化崩溃地址）、
  `scripts/disasm_xbox360.sh`、`scripts/find_sym_xbox360.sh`。

### M3 已完成

上述实机验收已覆盖 AppletFrame 构造、字体渲染、XML 视图树、i18n 文案、NanoVG
渲染、本地图片和 XInput 焦点导航。设置页、搜索页、视频详情页以及播放器仍属于后续
M5 及更后续工作，不影响 M3 的离线 UI 验收结论。

### 已知后续事项
- `std::terminate` 默认 handler 在本平台为空指针（OXDK libcxx_runtime.cpp 的
  `oxdk360_terminate_handler` 初始为 0）；下次 terminate 仍会跳 0。可在启动时
  `set_terminate` 安装 abort handler 改善崩溃诊断。
- `game:\config\wiliwili` 在 devkit 上已确认可写；零售机/模拟器行为待测。

### 2026-09-25 Debian 构建记录

已在纯 Debian 环境重新跑通 Xenon 编译和 XEX 链接。新增
`scripts/build_xbox360_linux.sh`，脚本会自动配置 CMake/Ninja、创建无空格的
XDK 路径、构建 OXDK libc++，并将 OXDK 转换缓存放在构建目录。详细依赖、环境变量、
构建命令和故障排查见 `docs/XBOX360_DEBIAN_BUILD.md`。

本次验证产物为 `build-xbox360-debian/wiliwili`，链接输出显示 147 个对象、9 个
XDK 库，基址 `0x82000000`。该记录只覆盖交叉编译和链接，Xenia/真实开发机部署仍
需单独验收。
