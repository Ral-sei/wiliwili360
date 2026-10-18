# Xbox 360 空实现（Stub）审查清单

> 审查日期：2026-09-30
> 审查范围：`wiliwili/source/platform/xbox360/`、`library/borealis/library/lib/platforms/xbox360/`、
> 以及所有 `#if defined(PLATFORM_XBOX360)` 条件分支（wiliwili + borealis + cpr）。
> 配套文档：`docs/XBOX360_PORTING_PLAN.md`（长线计划）、`docs/XBOX360_M1_NANOVG_D3D9_AUDIT.md`。
>
> 本文只记录**审查时点的代码事实**，每条给出文件位置与运行时影响，用于区分
> 「真 bug」「已声明的临时限制」「死代码/文档不同步」三类。
>
> **2026-10-04 复核**：逐条比对当前工作树后重写状态列，A 类 5 条已全部修复并通过
> `build-xbox360-local3` 全量编译链接；C 类 4 条属实、2 条（C2/C3）在复核时已过期；
> B 类 12 条属实且仍为**有意保留**的里程碑限制，本轮未动。详见 §6。

---

## 0. 结论摘要

| 类别 | 数量 | 说明 |
|---|---|---|
| A. 会真正影响功能的空实现 | 5 | **已于 2026-10-04 全部修复**，见 §6 |
| B. 计划已声明的临时限制（拒绝实现桩） | 12 | 有注释/有文档，按里程碑补齐；本轮未动 |
| C. 死代码与文档不同步 | 6 | 4 条属实已处理，2 条（C2/C3）复核时已过期 |
| D. 已确认解决、无需再管 | 3 | 曾经是空实现，现在已有真实现 |

---

## 1. A 类：会真正影响功能的空实现

### A1. `cpr::MultiPerform` 空壳 → 追番/影视分类筛选拿不到数据

- **位置**：`wiliwili/source/platform/xbox360/xbox360_m3_shims.cpp:8-13`
  ```cpp
  namespace cpr {
  MultiPerform::MultiPerform() = default;
  MultiPerform::~MultiPerform() = default;
  std::vector<Response> MultiPerform::Get() { return {}; }
  void MultiPerform::AddSession(std::shared_ptr<Session>&, HttpMethod) {}
  } // namespace cpr
  ```
- **调用方**：`wiliwili/source/api/home_api.cpp:224` `BilibiliClient::get_pgc_all_filter()`
  在 `cpr::async` 中构造 `MultiPerform`，塞入 6 个 session 后 `Get()`。
- **影响**：Xbox 上 `Get()` 恒返回空 vector → `PGCIndexFilters` 恒为空 →
  `wiliwili/source/presenter/pgc_index.cpp:51` 拿到空过滤器 → 追番/影视分类索引页无法筛选。
- **状态**：M4 首页验收后遗留的真实功能缺口，计划文档未记录。
- **建议**：用已可用的 cpr `Session` 串行 `session->Get()` 循环实现 `AddSession/Get`，
  或接 curl multi 接口；不需要并发也能恢复功能。

### A2. `Threading` 任务循环在 Xbox 上从不启动 → `brls::async` 死队列

- **位置**：`library/borealis/library/lib/core/thread.cpp:166,176,216`
  ```cpp
  void Threading::start()          { #if defined(PLATFORM_XBOX360) return; ... }
  void Threading::stop()           { #if defined(PLATFORM_XBOX360) return; ... }
  void Threading::start_task_loop(){ #if defined(PLATFORM_XBOX360) return; ... }
  ```
- **影响**：`Threading::async()` 把任务推进 `m_async_tasks`（`thread.cpp:73-76`），
  而该队列**只**由 `task_loop()`（`thread.cpp:191-211`）drain。循环不启动 ⇒ 任务永不执行，队列只进不出。
  - `performSyncTasks()`（主循环每帧调用，`application.cpp:204`）只处理 sync 队列和 delay 队列，**不处理 async 队列**。
- **当前受影响调用点**：
  - `library/borealis/library/lib/views/widgets/battery.cpp:63`、`wireless.cpp:108`
    —— Xbox 上 `canShowBatteryLevel()/canShowWirelessLevel()` 均为 false（`xbox360_platform.cpp:24-25`），控件不显示，暂无可见症状。
  - `wiliwili/source/api/analytics.cpp:88` GA 上报 —— `main.cpp:125` 已把 `GA(...)` 与 `checkUpdate()` 一并排除。
- **风险**：**隐蔽**。将来任何新代码用 `brls::async` 都会静默失效，不报错、不崩溃。
- **建议（最小改动）**：在 `Threading::performSyncTasks()` 末尾顺带 drain `m_async_tasks`；
  或为 Xbox 分支用 XDK 线程跑真正的 `task_loop`。

### A3. `Xbox360ImeManager` 拒绝实现 → 所有文本输入入口无响应

- **位置**：`library/borealis/library/lib/platforms/xbox360/xbox360_ime.cpp`
  ```cpp
  bool Xbox360ImeManager::openForText(...)   { return false; }
  bool Xbox360ImeManager::openForNumber(...) { return false; }
  ```
- **调用点**（返回 false 后上层不做任何提示）：
  - `wiliwili/source/activity/search_activity.cpp:29`（搜索输入框）
  - `wiliwili/source/activity/search_activity_tv.cpp:157`
  - `wiliwili/source/activity/player_base_activity.cpp:67`
  - `wiliwili/source/fragment/home_tab.cpp:27,70`（首页搜索入口）
  - `wiliwili/source/fragment/player_single_comment.cpp:152`（评论输入）
  - `wiliwili/source/fragment/inbox_chat.cpp:173`（私信输入）
  - `wiliwili/source/fragment/dynamic_article.cpp:309`
- **影响**：搜索、评论、私信、动态输入全部不可用；**没有任何提示文案**，用户按键后无反应。
- **状态**：计划 M3 未打勾项「暂时禁用或裁剪需要文本输入的功能」，且无虚拟键盘兜底。
- **建议**：短期先在这些入口加 `brls::Application::notify("输入法暂不可用")`；
  中期实现手柄虚拟键盘（XDK 无 XIM 输入法服务可用）。

### A4. 限帧（limited FPS）设置在 Xbox 上静默失效

- **位置**：`library/borealis/library/lib/core/application.cpp:231-235`
  ```cpp
  if (interval > 0) {
  #if !defined(PLATFORM_XBOX360)
      std::this_thread::sleep_for(std::chrono::microseconds(interval));
  #endif
  }
  ```
- **影响**：`setLimitedFPS()`（`application.cpp:869`）与设置页「限帧」项
  （`setting_activity.cpp:348,355`、`config_helper.cpp:728`）在 Xbox 上**完全不生效**，UI 却仍可操作。
- **建议**：Xbox 分支改用 `Sleep((interval + 999) / 1000)` 实现毫秒级帧间隔控制。

### A5. `std::to_string(float)/to_string(double)` 返回 `"0"`（错误值，非空值）

- **位置**：`wiliwili/source/platform/xbox360/xbox360_m3_shims.cpp:152-153`
  ```cpp
  string to_string(float)  { return string("0"); }
  string to_string(double) { return string("0"); }
  ```
- **影响**：任何浮点数格式化都会得到字面量 `"0"`——不崩溃、不报错，但**数据是错的**。
  当前浮点路径调用较少（`mpv_core_xbox360.cpp:17` 是唯一已知调用，而该文件已不参与编译），
  属于埋雷而非现行 bug。
- **建议**：用 `snprintf(buf, sizeof(buf), "%g", v)` 真实现替换，与 `__int_to_str` 同级替换即可。

---

## 2. B 类：已声明的临时限制（拒绝实现桩，按里程碑补齐）

| # | 位置 | 现状 | 对应计划项 |
|---|---|---|---|
| B1 | `xbox360_platform.cpp:24-25` | 电量/无线恒 0/false；`hasEthernetConnection()` 恒 true | M7（未列） |
| B2 | `xbox360_platform.cpp:25` 附近 | `getIpAddress()`/`getDnsServer()` 恒 `"-"`（网络诊断页显示 `-`） | M4 收尾 |
| B3 | `xbox360_platform.cpp` | `openBrowser`、`setBacklightBrightness`、`exitToHomeMode`、`forceEnableGamePlayRecording` 空转 | M7 |
| B4 | `xbox360_platform.cpp:18` | `getLocale()` 恒 `APP_LOCALE_DEFAULT` | — |
| B5 | `xbox360_input.cpp:79-81` | `getKeyboardKeyState` 恒 false；touch 清空；mouse 清零（主机平台合理） | — |
| B6 | `xbox360_input.cpp:26-33` | `getControllersConnectedCount()` 只查 0 号手柄；**多手柄聚合与重复按键/长按时间戳未实现** | **M2 未打勾项** |
| B7 | `xbox360_video.cpp` | `setSwapInterval()` 空转；`getScaleFactor()` 恒 1.0；**无设备丢失/恢复处理** | **M1/M2 未打勾项** |
| B8 | `wiliwili/source/view/video_profile.cpp:17-36` | 所有诊断字段写死 `"Unavailable"` —— **该假设已过时**（见 C2） | M5/M6 |
| B9 | `wiliwili/source/utils/shader_helper.cpp:147,171` | `ShaderHelper::load()/save()` 直接 `return` → shader pack / Anime4K 不支持 | M7（未列） |
| B10 | `wiliwili/source/utils/config_helper.cpp` `loadCustomThemes()` | 提前 return → 自定义主题不支持 | M7（未列） |
| B11 | `wiliwili/source/utils/version_helper.cpp:91` | `APPVersion::checkUpdate()` 提前 return（`main.cpp:125` 一并排除） | M7 |
| B12 | `wiliwili/source/utils/crash_helper.cpp:104` | `initCrashDump()` 空实现 → 无崩溃转储/日志 | **M7 未打勾项** |

**另有两处"能力有、入口没有"：**

- **DLNA**：`wiliwili/source/utils/activity_helper.cpp:119-123` 只 `notify("DLNA is not available on this build")`；
  `wiliwili/source/view/video_view.cpp:426-431` 投屏按钮 `return false`；
  `dlna_activity.cpp`/`api/dlna.cpp`/`player_dlna_search.cpp` 已在 `CMakeLists.txt:317-321` 排除编译。
- **震动**：`xbox360_input.cpp:82-86` 已实现 `XInputSetState`，但
  `wiliwili/source/activity/setting_activity.cpp:374-381` 的震动开关是 `#ifdef __SWITCH__`，
  Xbox 上 `cellVibration->setVisibility(GONE)`；`vibration_helper.cpp:43-59` 的
  `startVibrate()` 也只写了 `#ifdef __SWITCH__` 分支（Xbox 上设 `playing=true` 后什么都不做）。
  → 硬件能力就绪，UI 入口与播放逻辑未放行。

**网络/安全相关（M4 未打勾项，非纯桩但属"临时裁剪"）：**

- `wiliwili/source/utils/config_helper.cpp:1245-1251`：Xbox 分支强制 `BILI::setTlsVerify(false)`，
  注释说明"初始镜像未部署 CA bundle"。hostname/证书有效期校验一并关闭。
- `wiliwili/include/api/bilibili/util/http.hpp:96-101`：Xbox 强制 `Connection: close`。
- `wiliwili/include/api/bilibili/util/http.hpp:91-94`：Xbox 跳过 `CURLOPT_SHARE`（DNS/cookie 共享句柄未启用）。

---

## 3. C 类：死代码与文档不同步（建议清理）

### C1. `mpv_core_xbox360.cpp` 已是死文件

- **事实**：`CMakeLists.txt:314-321` 显式 `list(REMOVE_ITEM MAIN_SRC ... mpv_core_xbox360.cpp)`，
  现在编译的是真 `mpv_core.cpp`（配合 `MPV_SW_RENDER` 软渲染 + cbffmpeg P4 的 libmpv 静态库）。
- **文件内容**：整套 `MPVCore` 空壳（`isValid()` 恒 false、`setUrl` 只记 filepath 等 80 行桩），
  **不会被链接**。
- **建议**：删除该文件并同步删除 `CMakeLists.txt` 中的 `REMOVE_ITEM` 条目；
  若作为 M8 兜底保留，则在文件头注明"当前未参与编译"。

### C2. `video_profile.cpp` 的 Xbox 分支基于过时假设

- **位置**：`wiliwili/source/view/video_profile.cpp:17-36`
- **注释原文**：`// M3 has no libmpv backend...` —— 但 M5 libmpv 已接入。
- **建议**：改回与其它平台一致的 `mpvCore->getString/getInt/getNodeMap` 读取真实状态。

### C3. 计划文档状态行落后于代码

- `docs/XBOX360_PORTING_PLAN.md:3` 只记录到「M4 网络/API 首页链路已通过实机验收（2026-09-26）」，
  M5 全部任务仍为 `- [ ]`。
- 但 `CMakeLists.txt:164-197` 已链接 cbffmpeg P4 产出的
  `libmpv_xbox360_compat.a`、`ffmpeg` 六静态库、`xbox_float_math.o` 等，
  `build-xbox360-local3` 缓存 `MPV_SW_RENDER=ON` 且 `mpv_core.cpp.o` 已编译。
- **建议**：更新状态行与 M5 复选框，补记"P4 libmpv 已链接进 wiliwili"这一事实。

### C4. `docs/wiliwili_mpv_embed.md` 引用路径失效

- `CMakeLists.txt:164,192,312` 三处引用 `docs/wiliwili_mpv_embed.md`，
  实际文件位于 `../cbffmpeg/docs/wiliwili_mpv_embed.md`，**本仓库 `docs/` 下不存在**。
- **建议**：改为 `cbffmpeg/docs/...` 或在本仓库放一个指路 stub 文档。

### C5. 构建脚本不一致：`build_xbox360_local.sh` 缺 `MPV_SW_RENDER`

- `scripts/build_xbox360_linux.sh` 有 `-DMPV_SW_RENDER=ON`；
- `scripts/build_xbox360_local.sh`（第 38-43 行的 cmake 配置段）**没有**，依赖手工填的构建缓存。
- **风险**：换机器/清缓存后按该脚本配置，会按非软渲染路径编 `mpv_core.cpp`（需要 GL 路径），构建或运行异常。
- **建议**：补齐 `-DMPV_SW_RENDER=ON`。

### C6. `cpr::ThreadPool` Xbox 全空桩（休眠）

- **位置**：`library/cpr/include/cpr/threadpool.h:4-26` —— `Start/Stop/Pause/Resume/Wait` 全返回 0，
  `IsStarted` 恒 false、`IsStopped` 恒 true。
- **现状**：Xbox 路径用 `cpr::async`（每次 `CreateThread`，见 `cpr/async.h`）绕开了它，
  `image_helper.cpp:104-117` 的 `ImageThreadPool` 在 Xbox 分支也不再继承 `ThreadPool`。
- **建议**：保留但加注释标注"Xbox 未使用"，或直接 `#error` 防止误用。

---

## 4. D 类：已确认解决、无需再管

| 曾经的问题 | 现状 |
|---|---|
| `std::filesystem` 占位体（`__status` 恒 not-found 等） | 已被 `wiliwili/source/platform/xbox360/xbox360_filesystem.cpp` 的 XDK Win32 API 真实现替换 |
| stb_image 第 7 节 stub | `xbox360_m3_shims.cpp:216-221` 已是空 `extern "C"` 块；真实现来自 `nanovg_d3d/src/stb_image.h` |
| `__dynamic_cast`/vtable 恒 nullptr 桩 | 已改为编入 libc++abi `private_typeinfo.cpp` 真实现（`xbox360_m3_shims.cpp:170-180` 注释详述） |
| `xbox360_crt_shims.c` 中 `WriteConsoleW`/`_Unwind_Resume` | 有正当注释的合理裁剪（无 Win32 控制台、`-fno-exceptions`），非缺陷 |
| `third_party/curl/lib/xbox360_stubs.c` 的 `stat/fstat` 返回 -1 | 注释说明不使用 multipart 上传，属合理裁剪 |

---

## 5. 建议处理批次

**第一批（真 bug，改动小，性价比最高）**
1. 修 `MultiPerform`（A1）→ 恢复追番/影视分类筛选。
2. 修 `std::to_string(float/double)`（A5）→ 消除错误值。
3. 让 `performSyncTasks()` drain async 队列（A2）→ 消除静默死队列。
4. `scripts/build_xbox360_local.sh` 补 `-DMPV_SW_RENDER=ON`（C5）。

**第二批（文档/代码一致性）**
5. 更新 `XBOX360_PORTING_PLAN.md` 状态与 M5 复选框（C3）。
6. 删除 `mpv_core_xbox360.cpp` 死文件（C1）。
7. `video_profile.cpp` 改读真实 mpv 状态（C2）。
8. 修正 `wiliwili_mpv_embed.md` 引用路径（C4）。

**第三批（功能补齐，工作量大，按里程碑）**
9. 手柄虚拟键盘 → 解锁搜索/评论（A3）。
10. 限帧在 Xbox 上实现真实 sleep（A4）。
11. TLS CA bundle 部署（M4 未打勾项）。
12. 多手柄聚合 + 按键重复时间戳（M2 未打勾项）；震动入口与 `startVibrate` 放行 `PLATFORM_XBOX360`。
13. `VideoProfile`、shader pack、自定义主题、崩溃转储按对应里程碑补齐。

---

## 6. 2026-10-04 复核与修复记录

逐条比对当前工作树（基线 `750fc7a` 之后的未提交改动）重新判定，结论如下。
全部改动经 `build-xbox360-local3` 全量编译验证：**424/424 编译 + 链接通过，XEX 已生成**
（162 objects，image 0x13c0000）。各修复项均已在产物中核对到对应符号/引用。

### 6.1 A 类：5 条全部属实，已修复

| 条目 | 复核结论 | 修复方式 | 产物验证 |
|---|---|---|---|
| A1 `MultiPerform` 空壳 | **属实**（`Get()` 恒返回 `{}`） | 在 `xbox360_m3_shims.cpp` 内实现**串行**回退：`AddSession` 只入队 `sessions_`，`Get()` 按入队顺序逐个 `Session::Get()`。不接 curl multi——Xbox 版 libcurl 未导出 `curl_multi_poll/select`，故 `multiperform.cpp` 被 `library/CMakeLists.txt` 过滤；也**不设 `isUsedInMultiPerform`**，否则 `Session::DoEasyPerform()` 会拒绝执行 | `MultiPerform::Get`/`AddSession` 已定义，且 `vector<pair<shared_ptr<Session>, HttpMethod>>` 参与 |
| A2 `Threading` 任务循环从不启动 | **属实**（三处 `return`，`m_async_tasks` 只进不出） | **按审计原本建议的第二方案**：为 Xbox 分支启用真实 `task_loop` 线程（`<thread>` 走 xbox360-cshim 的 `CreateThread` 实现，`retro_sleep`→`Sleep`）。`start()`/`stop()`/`start_task_loop()` 去掉 `#if PLATFORM_XBOX360 return;`，改与 `BOREALIS_USE_STD_THREAD` 共用 std::thread 分支；`stop()` 加 `if (task_loop_thread)` 保护未启动的情形 | `Threading::start/stop/task_loop/start_task_loop/std_task_loop` 均为 `T`，`task_loop_thread` 存储存在 |
| A3 IME 拒绝实现且无提示 | **属实**（8 个调用点返回 false 后静默） | 短期方案：在 `Xbox360ImeManager::openForText/openForNumber` 内统一 `Application::notify("wiliwili/dialog/ime_unavailable"_i18n)`，一处覆盖全部 8 个调用点；新增 i18n key 已按行插入 7 个语言文件（各 +1 行，JSON 校验通过）。**完整虚拟键盘仍属 M7/里程碑工作，未做** | `xbox360_ime.cpp.o` 引用 `Application::notify` 与 `literals::operator""_i18n` |
| A4 限帧 `sleep_for` 被 `#if !defined(PLATFORM_XBOX360)` 排除 | **属实** | 去掉守卫。cshim `<thread>` 提供 `std::this_thread::sleep_for`（`Sleep()` 实现），`application.cpp` 本就无条件 include `<thread>`。默认 `LIMITED_FPS=0` → `limitedFrameTime=0` → 分支不进入，**无行为变化**，仅在用户开启限帧时生效 | `application.cpp` 唯一的 `sleep_for`（`:284`）已无守卫；该 TU 新增引用 `Sleep` |
| A5 `to_string(float/double)` 恒返回 `"0"` | **属实** | 用 `__float_to_str` 实现 `%g` 格式化 + 显式 `buf[n]='\0'`（工具链 `-Dsnprintf=_snprintf`，XDK `_snprintf` 截断时不补 NUL，故强制收尾） | `std::__1::to_stringf`/`to_stringd` 已定义，引用 `_snprintf` |

> **A2 与原建议的差异**：审计原文把「在 `performSyncTasks()` 末尾 drain async 队列」列为第一方案。
> 未采用——那样会把 `Analytics::send()`（GA 上报）等 async 任务挪到主线程执行，
> 改变 `brls::async` 的语义契约。改跑真实后台线程与其它平台一致。
>
> **A3 与原建议的差异**：审计建议在 8 个调用点逐个加 notify；改为在 IME manager 内加一处，
> 覆盖面相同而改动面更小，新增调用点也自动获得提示。

### 6.2 C 类：4 条属实已处理，2 条复核时已过期

| 条目 | 复核结论 | 处理 |
|---|---|---|
| C1 `mpv_core_xbox360.cpp` 死文件 | **属实**（被 `list(REMOVE_ITEM)` 摘除，不参与编译） | **未删除**，改在文件头写明「NOT COMPILED」及原因（与真 `mpv_core.cpp` 同为 `MPVCore` 成员、同时编译即 duplicate symbol；重新启用必须同改动移除真 `mpv_core.cpp`）。原建议的删除留给你决定 |
| C2 `video_profile.cpp` Xbox 分支 | **已过期** | 复核时 Xbox 分支已从工作树移除，现与其它平台一致走 `mpvCore->isValid()` + 真实 `getString/getInt/getNodeMap`，仅把 `Unavailable` 的语义改为「mpv 未就绪 / 字段缺失」共用 |
| C3 计划文档状态行落后 | **部分过期** | 状态行已记录 M5/M6 首次播放（2026-09-30）、M5 已有 2 个 `[x]`；本轮补记缺失的一条：P4 `libmpv_xbox360_compat.a` + FFmpeg 六静态库 + `xbox_float_math.o` 已链接进 wiliwili |
| C4 `docs/wiliwili_mpv_embed.md` 路径失效 | **属实** | `CMakeLists.txt` 三处引用改为 `../cbffmpeg/docs/wiliwili_mpv_embed.md`，并注明该文档不在本仓库 `docs/` 下 |
| C5 `build_xbox360_local.sh` 缺 `MPV_SW_RENDER` | **属实** | 补 `-DMPV_SW_RENDER=ON` |
| C6 `cpr::ThreadPool` Xbox 空桩 | **属实**（休眠未用） | 保留，加注释说明：`threadpool.cpp`/`async.cpp` 已被过滤，`async()` 在 Xbox 走 CreateThread + 4 槽闸门；`api.h` 的 `setup_multiasync` 引用 `GlobalThreadPool` 但仅 `library/cpr/test/` 实例化，wiliwili 不触发——若将来有人调用应**编译期失败**而非静默吞任务，故 stub 刻意不提供 `Submit()` |

### 6.3 复核中额外发现的构建缺陷（非审计条目）

排查编译失败时定位到 `scripts/build_xbox360_local.sh` 有两处独立缺陷，均会导致该脚本无法从零构建成功：

1. **`XDK_DIR=/home/lbx/xdk360` 少了一级**——该目录下只有 `XDK -> .../Microsoft Xbox 360 SDK` 软链、没有 `include/`。
   于是 `-isystem ${XDK_DIR}/include/xbox` 指向不存在的目录，`<sal.h>`/`<stdio.h>` 落到宿主 `/usr/include`，
   mbedtls 直接编译失败（`__float128 is not supported` / `_locale_t` unknown）。
   正确值为 `/home/lbx/xdk360/XDK`，或沿用 `build_xbox360_linux.sh` 的做法在构建目录内建 `xdk` 软链（`build-xbox360-local3/xdk` 即为此）。
   **已修复**（脚本内改为 `/home/lbx/xdk360/XDK` 并注释原因）。
2. **未设 `LLD`**——`oxdklink.py:32` 只读 `LLD` 环境变量，缺省为 `ld.lld`。cbffmpeg 的
   `compat/xbox360/x360-ld.sh` 会在 `ld.lld` 后追加 `--wrap=getsockopt --wrap=WSAStartup`。
   缺 `LLD` 时链接报 `oxdklink: not provided by the console and not in any library: __real_WSAStartup/__real_getsockopt`
   （`xbox_winsock_extra.c` 的 `__wrap_*` 已定义、但 `__real_*` 无人解析）；即便链接通过，
   按 `build_xbox360_linux.sh` 头注释，mpv 的 http/tcp 也会以 10042/10057 全线失败。
   **已修复**（移植 `build_xbox360_linux.sh` 的 `LLD` 探测段）。

> 另：`.git/index.lock` 存在 9月28日 留下的 0 字节残留（无 git 进程占用），已清除。

### 6.4 本轮**未**改动的范围

- **B 类 12 条全部保持原状**——它们是带注释/文档的**有意**临时限制（TLS CA、崩溃转储、震动、
  虚拟键盘、多手柄、shader pack、自定义主题、在线更新等），需按里程碑推进并多数依赖实机验证，
  不属于「补上空实现」的范畴。是否推进请另行决定。
- **C1 未删除文件**（仅加头注释），**A3 未实现完整虚拟键盘**，**A2 未在 `performSyncTasks()` 中 drain**——
  三处与原建议有出入，理由见 §6.1/§6.2 表下注。
