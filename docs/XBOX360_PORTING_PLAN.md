# wiliwili Xbox 360 长线移植计划

> 状态：M0 工具链基线已完成；M1 NanoVG D3D9 已完成 Xenon 编译、shader 生成、XEX 链接，并由真实 XDK 确认基础图形输出；M3 wiliwili UI 最小集成已完成编译和链接，路径规范化已实施但待实机验证（2026-09-21）
>
> 目标：在 Xbox 360/XDK 上逐步运行 wiliwili，并保留现有 Desktop、Switch 等平台。
>
> 参考：本仓库、`H:/bili360-main`、`H:/OXDK`、`H:/ffmpeg-xenon`，以及本地 Xbox 360 XDK 文档。

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
FFmpeg + 自有 Xbox360PlayerCore
    ↓
完整播放页和生命周期
    ↓
评估 MPV/ANGLE（可选）
```

第一版不应把“完整移植 libmpv”作为前置条件。可以用 `bili360-main` 已验证的 custom AVIO + FFmpeg + D3D9/XAudio2 播放链路替代 Xbox 360 上的 libmpv 实现，但最终必须提供与 wiliwili 播放器契约等价的功能，而不是只保留固定视频播放。

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
内置 switch 字体和 material fallback。IME 仍为最小拒绝实现，设备状态恢复、Present
稳定性和 borealis demo 验收仍待完成。当前实现属于平台层骨架，不能视为 M2 验收完成。

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
- [x] `Xbox360Ime`：已提供最小拒绝实现；完整文本输入仍不支持。
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
  显示 `Unavailable`；M5/M6 接入 `Xbox360PlayerCore` 后改为读取等价播放器诊断状态。
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
> 的 `NativePath` 实现。实机验证尚未完成。

下一步仍以完整 `wiliwili` OXDK 编译和链接为门槛，随后再收敛到可打包、可启动的
M3 离线 XEX；上述临时裁剪必须在对应里程碑恢复或重新设计。

> **2026-09-21 更新：M3 编译和链接已完成。** 下一步是 M3 验收（在 Xenia 或
> 真机上启动 XEX，验证 UI 框架初始化和渲染）和 M4（网络/API 层接入真实
> Bilibili 数据）。详细计划见下方"下一步工作"章节。

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
- [ ] **路径验证**（待实机测试）：Xbox 360 SDK 要求只用反斜杠，`fopen` 能正常工作（ButterAndJelly 已验证），但 `game:\config\wiliwili` 目录是否可写、`game:\resources\` 是否能被 `fopen` 正确解析，仍需在 Xenia 或真机上确认。
- [ ] 将离线静态页面替换为首页、设置页、搜索页和静态视频详情页。
- [ ] 运行首页、设置页、搜索页和静态视频详情页。
- [ ] 验证图片加载、SVG、滚动、RecyclingGrid 和焦点路由。
- [ ] 实现 Xbox 360 资源路径和资源打包方式。
- [ ] 增加平台专用输入映射，不改变通用 `Intent` 导航。
- [ ] 暂时禁用或裁剪需要文本输入的功能。

验收：XEX 可在 Xenia 或真机启动，UI 可连续使用；页面切换和返回不会泄漏或崩溃。

当前阻塞（M3 验证）：
- **路径问题**：Xbox 360 SDK 要求路径只用反斜杠（`dev_overview_filenames.htm`：
  "Use the backslash (\) to separate components in a path. No other character is
  acceptable as a path separator."）。已通过 `resourcesDir()` 做了 `/`→`\` 转换，
  但实机验证尚未完成。已知 `game:\` 在 devkit 上可写（参考 ButterAndJelly 的
  `platform_xenon.cpp`，其 `kDataRoot = "game:\\data"` 且用 `fopen` 测试可写性）。
- 所有 M3 shims 都是空实现，实际运行时 cpr HTTP 请求会返回空数据，
  filesystem 操作会返回默认值，chrono 返回 epoch。M3 验证需要在
  Xenia 或真机上确认 UI 框架能正常初始化和渲染。

### M4：Xbox 360 网络/API 层（L3）

目标：在保留现有 API 数据模型的前提下接入已验证的 Xbox 360 网络实现。

推荐优先复用 `bili360-main` 的 libcurl + mbedTLS，而不是立即切换 XboxTLS。

任务：

- [ ] 将 `HttpRequest` 抽象成 wiliwili 可使用的响应接口。
- [ ] 初始化 `XNetStartup`，再调用 `WSAStartup`。
- [ ] 接入 XNet DNS resolver。
- [ ] 接入 CA bundle、hostname 校验、证书有效期校验。
- [ ] 实现 GET、POST、Cookie、Range、超时和取消。
- [ ] 保留 WBI 参数签名逻辑。
- [ ] 将 cpr 异步接口替换为平台 worker/回调队列。
- [ ] 限制响应体、日志和 Cookie 输出，避免泄露凭据。

当前进度：已增加 `XBOX360_BUILD_MBEDTLS=ON` 的 mbedTLS 静态库接线，来源为
`XBOX360_BILI360_DIR`/`BILI360_DIR` 指向的 `bili360-main/third_party/mbedtls`，并已增加
`XBOX360_BUILD_CURL=ON` 的 bili360 libcurl 静态库目标。`xbox360_mbedtls` 和
`xbox360_curl` 已可独立编译并生成 `.a` 文件；这只是源码/静态库准备，不代表 M4 网络链路完成。
尚未把 curl backend、XNet resolver、CA bundle、hostname/证书校验和异步 HTTP 接口接入
wiliwili。

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

### M5：FFmpeg + Xbox360PlayerCore（L5）

目标：用 Xbox 360 可行的播放器实现替代 libmpv 播放 Bilibili DASH，同时保持 wiliwili 的播放器功能契约，包括加载、暂停、恢复、停止、进度、seek、倍速、音量、清晰度、备用 URL、错误状态、自然 EOF 和返回页面生命周期。

任务：

- [ ] 从 `bili360-main` 移植独立视频/音频 CurlStream。
- [ ] 为两个 m4s 输入分别建立 custom AVIO。
- [ ] 复用 FFmpeg 1.2.x 的 `AVFormatContext`、`AVCodecContext` 和帧解码流程。
- [ ] 实现 AVC 质量选择和 AAC 音频选择。
- [ ] 将 YUV frame 上传至**Xbox 360 兼容的** D3D9 Y/U/V 纹理，确认 pitch、对齐、线性纹理格式和锁定/上传路径。
- [ ] 将 PC/通用 shader 改为 Xbox 360 XDK 可加载的 vertex/pixel shader 字节码或对应 XDK 编译流程。
- [ ] 将 AAC 解码 PCM 提交至 XAudio2。
- [ ] 用 XAudio2 时钟作为视频同步主时钟。
- [ ] 实现暂停、停止、进度、取消、自然 EOF 和基本 seek。
- [ ] 添加与 `MPVCore` 功能等价的 Xbox 360 播放器接口；允许内部实现完全不同，但不能把接口降级为固定 URL/固定质量/仅播放一次。
- [ ] 将状态转换为 `MPV_E` 或新的通用播放器事件。

验收：匿名 480p 视频能够启动、播放、暂停、退出，B 键取消后能回到 UI；不存在后台 worker 卡死。

性能目标：先达到 480p 稳定播放，再评估 720p；不把 1080p/硬件解码作为第一版门槛。

### M6：视频页和播放器生命周期集成（L4）

目标：把 Xbox360PlayerCore 接入 `VideoView`、播放器 Activity 和全局事件。它可以替代 Xbox 360 上的 MPVCore，但不能改变上层 wiliwili 的功能语义。

任务：

- [ ] 将 `VideoView` 中直接依赖 mpv 的调用改为播放器接口。
- [ ] 保留 Desktop/Switch 的 MPVCore 实现。
- [ ] Xbox 360 使用条件编译选择 Xbox360PlayerCore；允许 Xbox 360 内部不链接 libmpv，但必须实现等价的播放器行为。
- [ ] 映射音量、倍速、画面比例、暂停、重播和进度条。
- [ ] 处理 Activity 进入/退出、重复播放和播放失败。
- [ ] 确认 D3D9 UI overlay 与视频纹理不会互相破坏状态。

验收：从首页进入视频页、播放、返回首页、再次播放至少重复 20 次无泄漏或死锁。

### M7：功能补齐和性能优化（L4）

目标：从“能播放”变成可长期使用的 Xbox 360 版本。

任务：

- [ ] 登录 Cookie 持久化和刷新。
- [ ] 搜索历史、收藏、观看历史和稍后再看。
- [ ] 弹幕和直播功能评估，默认不作为首发阻塞项。
- [ ] 封面下载缓存和纹理 LRU。
- [ ] 网络缓冲、断线重试和 CDN 切换。
- [ ] 内存预算、纹理预算和 worker 数量限制。
- [ ] 720p 性能、帧丢弃和音视频同步调优。
- [ ] XEX 资源打包、版本信息、崩溃日志和部署脚本。

验收：长时间浏览和播放不超出内存预算；异常网络和取消路径均能回到可操作 UI。

### M8：MPV/ANGLE 研究线（L5，可选）

目标：评估是否值得保留现有 MPV 功能，而不是替代 Xbox360PlayerCore。

任务：

- [ ] 确认目标 MPV 版本与 FFmpeg 1.2 API 是否兼容。
- [ ] 清点 MPV 对 pthread、动态加载、文件系统、网络和 C++ runtime 的依赖。
- [ ] 验证 ANGLE 是否能在 Xbox 360 D3D9 上工作。
- [ ] 验证 MPV software renderer 的最小编译集合。
- [ ] 比较 MPV 和自有播放器的内存、性能和维护成本。

退出条件：如果需要大规模修改 MPV、ANGLE 或 FFmpeg，继续维护 Xbox360PlayerCore，不阻塞主线。

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
| libmpv 无法在 Xenon ABI 下编译 | L5 | MPV 路线中止 | 使用 Xbox360PlayerCore，不阻塞主线 |
| FFmpeg 1.2 MOV/DASH 行为有限 | L4 | seek、时长和自然 EOF 异常 | 以 API duration 控制进度，先支持固定质量和基本播放 |
| Xbox 360 内存/纹理预算不足 | L4 | 长时间运行崩溃 | bounded ring、纹理 LRU、限制并发和响应体 |
| C++ 异常/线程 ABI 不兼容 | L4 | 链接或运行时失败 | 平台 worker、显式错误码、避免新增异常和桌面 libc++ 线程设施 |
| Bilibili API/CDN 变化 | L3 | 播放或登录失效 | 封装 API、保留备用 URL、增加网络 smoke 测试 |
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
| MPV 播放控制 | `Xbox360PlayerCore`、FFmpeg custom AVIO 或移植后的 MPV | 上层可完成播放、暂停、停止、seek、倍速、音量和进度更新 |
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
- MPV/ANGLE 作为可选研究线，而不是主线阻塞项。

## 10. 下一步工作（2026-09-21）

### 当前状态
M3 编译和链接已完成，路径规范化已实施。XEX 产出 `build-xbox360-m2/wiliwili`。
### 当前状态
M3 编译和链接已完成，路径规范化已实施。XEX 产出 `build-xbox360-m2/wiliwili`。

2026-09-21 更新：`bili360-main` 的 `third_party/curl`（libcurl 7.37.1，含
`config-xbox360.h` 和 `xbox360_stubs.c`）与 `third_party/mbedtls`
（mbedTLS 2.28.10）已复制到本仓库 `third_party/`，根 `CMakeLists.txt` 的
Xbox 360 分支优先使用仓库内副本，`BILI360_DIR` 环境变量仅作为回退。
构建不再依赖 `H:/bili360-main` 的绝对路径（OXDK/XDK/oxdk-llvm 工具链仍为
本机环境依赖，不入库，XDK 不可再分发）。

### 待验证
1. **路径实机测试**：确认 `game:\resources\` 能被 `fopen` 正确解析（参考
   ButterAndJelly `platform_xenon.cpp` 的 `NativePath` 实现和 `fopen` 测试）。
2. **配置目录可写性**：确认 `game:\config\wiliwili` 在 devkit 上可写并能自动创建。
3. **UI 框架初始化**：在 Xenia 或真机上启动 XEX，确认字体加载、XML 解析、
   i18n 加载和 NanoVG 渲染正常。

### 验证方法
将 `build-xbox360-m2/wiliwili` 和 `resources\` 目录部署到 devkit 的游戏目录，
观察启动日志是否仍有 "doesn't exist" 或 "not writable" 错误。
