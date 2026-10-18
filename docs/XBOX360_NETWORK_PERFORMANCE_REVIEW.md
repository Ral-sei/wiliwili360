# Xbox 360 网络链路性能审查 —— 视频解析慢 & 主线程卡顿

> 审查日期：2026-10-01
> 首轮实测：2026-10-02（修复计划 #1 埋点已上真机，数据见 §5）
> 范围：`bilibili::HTTP` / cpr（Xbox 分支）/ curl fork（mbedTLS + XNet）/ 回调 → 主线程（borealis sync）
> 结论对应里程碑：M7「解码与窗口定位瓶颈，修正后 480p/720p 持续可播」

---

## 1. 现象

- 打开视频详情页到拿到 playurl（解析视频地址）耗时数秒级，明显慢于桌面端。
- 解析期间 UI 掉帧/卡顿（主线程被拖慢）。
- 视频页是典型并发 burst：detail、playurl、online、弹幕、分P、评论、推荐 + 20 张左右缩略图同时请求。

---

## 2. 根因分析：为什么"解析视频超慢"

### 2.1 每个请求都重新 DNS，且所有 DNS 被全局锁串行化（最大嫌疑）

- `wiliwili/include/api/bilibili/util/http.hpp:91-93`
  Xbox 分支**禁用了 `CURLOPT_SHARE`**。curl 的 DNS 缓存只存在于单个 easy handle 内，而
  `createSession()` 每个请求新建 session → `DNS_CACHE_TIMEOUT = 60` 完全失效，
  **每个请求都会真正发起一次 DNS 解析**。
- `third_party/curl/lib/xbox360_stubs.c:92-138`
  `gethostbyname()` 进入全局自旋锁后执行 `XNetDnsLookup`，并
  `while (dns->iStatus == WSAEINPROGRESS) WaitForSingleObject(event, 25)` ——
  **等待整个网络解析完成都在锁内**（注释写的是"只序列化 XNet lookup"，实现把等待也锁了）。
- 后果：视频页一次 burst 十来个 API 请求 + 图片请求，全部**排队一个一个做 DNS**，
  每个 DNS 又是 25ms 粒度轮询。DNS 阶段整体被放大成串行链。

### 2.2 零连接复用 —— 每个请求都要完整 TLS 握手

- `http.hpp:105-112`：Xbox 强制
  - `CURLOPT_HTTP_VERSION = HTTP/1.0`
  - `Connection: close`
  - `Accept-Encoding: identity`（不压缩）
- 每次请求 = 新 TCP 连接 + 完整 mbedTLS 握手（Xenon 软件 RSA/ECDHE，单次百 ms 级）；
  SSL session 因 SHARE 被禁也无法恢复；playurl 的 DASH JSON 还不压缩，传输体积更大。
- 降级原因（历史决策）：Xbox curl fork 对"服务端省略 content-length 的 keep-alive 响应"
  会一直等 EOF，故降级到 close-delimited。当前以**性能代价**换稳定性。
- 附带问题：`third_party/curl/lib/vtls/mbedtls.c:377` 握手循环**每次迭代都 DbgPrint**，
  连接（connect.c:1097/1115）、DNS（xbox360_stubs.c:102/113/133）也各有 DbgPrint ——
  若输出走 XBDM 调试通道，这些是阻塞且慢的写操作。

### 2.3 每请求 2 次 CreateThread + 4 槽闸门 + 1ms 自旋排队

- `library/cpr/include/cpr/async.h:4-47`（Xbox 分支）
  每次 `cpr::async` 都 `CreateThread` 新建 XDK 线程；槽位满（`xbox_async_limit = 4`）
  则在 thunk 内 `Sleep(1)` 自旋等待。
- `wiliwili/include/api/bilibili/util/http.hpp:160`（`_cpr_get` 外层 `cpr::async`）
  + `library/cpr/include/cpr/session.h:272`（`GetCallback` 内部再次 `async`）
  → **每个请求创建 2 个线程**。
- **未定义行为隐患**：`xbox_async_active` 是头文件里的
  `static volatile LONG`（每个编译单元一份），而 `xbox_async_thunk` 模板经 COMDAT 合并后
  引用哪一份不确定 —— 闸门到底是"全局 4 并发"还是"每 TU 4 并发"行为未定义。
  若合并为全局 4：**playurl 等 API 请求会和页面 20+ 张缩略图抢同一批 4 个槽**，
  排在图片下载后面。
- 图片同样走 `cpr::async`（`wiliwili/source/utils/image_helper.cpp:110` `ImageThreadPool::Submit`），
  与 API 共享同一套机制。

### 2.4 WBI 串行附加一跳

- `wiliwili/source/api/util/wbi.cpp:69-123`
  key 过期（>1h）或首次使用时，playurl 之前必须先串行完成一次 `Api::Nav` 请求
  （又是全套 DNS + TCP + TLS），首播延迟再加秒级。

### 2.5 JSON 解析本身不是问题

`parseJson`/`nlohmann` 解析运行在 cpr 工作线程（`GetCallback` 的回调链），**不在主线程**。
慢的是解析之前的网络阶段（DNS/握手/排队），不是解析算法。

---

## 3. 根因分析：为什么拖慢主线程

### 3.1 `performSyncTasks()` 一帧内倾倒所有积压回调（主要）

- `library/borealis/library/lib/core/application.cpp:206`
  所有网络回调经 `brls::sync` 排队，在主线程帧末 `Threading::performSyncTasks()` 一次性执行
  （已有 `xbox360perf::PHASE_SYNC` 埋点，见 application.cpp:205-207）。
- 视频页 10 个 API + 20 张图几乎同时完成 → **同一帧集中执行**：

| 工作项 | 位置 | 成本 |
|---|---|---|
| 图片纹理创建 + **逐像素字节交换** | `library/borealis/library/lib/extern/nanovg_d3d9/src/nanovg_d3d9.h:487-501` | 每张 672×378 ≈ 25 万次逐像素 swap；20 张集中在一帧 = 几十 ms。解码在工作线程，**swap 却在主线程** |
| 弹幕 `loadDanmakuData` 深拷贝 + `std::sort` | `wiliwili/source/view/danmaku_core.cpp:302-307` | 热门视频上万条，Xenon 上数 ms ~ 十几 ms，且持 `danmakuMutex` |
| 大结果体多次按值拷贝 | `wiliwili/source/presenter/video_detail.cpp:254` 等 | `VideoDetailAllResult` 进 sync lambda 拷一次、主线程赋值再拷一次 |

### 3.2 日志全局自旋锁 + 同步 stdout 写

- `library/borealis/library/include/borealis/core/logger.hpp:107-138`
  每条日志：`std::localtime` + `fmt::format` + `fmt::print(stdout)`，**全程持 `logMtx`**
  （Xbox 自旋锁 `Sleep(0)` 忙等，`xbox360_mutex.hpp:10-21`）。
- 工作线程日志量大时（image verbose、DNS/TLS DbgPrint、每 2s 的 `[x360-perf]` report），
  **主线程自己打日志会卡在自旋锁上等别人写完慢速 stdout**。
- OXDK 的 stdout 若接到 XBDM 网络调试输出，单行可达 ms 级。

### 3.3 主线程上的零散项

- key 未过期时 `wbi::encWbi` / `HTTP::signParameters`（MD5 + 字符串处理）同步跑在调用线程
  （通常是主线程）—— 成本较低，但非零。
- 每个请求的 `CreateThread` 发生在发起线程（视频页 ×10）。
- 图片请求失败/成功的 verbose 日志同样进入全局日志锁。

---

## 4. 修复计划（按性价比排序）

| # | 动作 | 涉及文件 | 预期收益 | 风险 |
|---|---|---|---|---|
| **1** | **先加测量**（✅ 2026-10-02 已完成，实测见 §5）：每请求记录 DNS / connect / TLS / TTFB / 总耗时 + `xbox_async_thunk` 排队等待时间；扩展 `xbox360perf` sync 阶段细分（图片纹理 / 弹幕 / 其它回调各占多少 ms） | `http.hpp`、`async.h`、`xbox360_perf.*`、`application.cpp` | 用数据确认 §2、§3 各项占比，避免盲改 | 低 |
| **2** | **DNS 去串行化**：加应用级 DNS 缓存（域名→IP，TTL 60s；视频页只有 api / i0.hdslb / upos 等几个域名），或走 curl `CURLOPT_RESOLVE` 注入（`config-xbox360.h:210` 注释已提示此路线）；把等待移出 `x360_dns_lock` | `xbox360_stubs.c`、`http.hpp` | 消除 burst 串行 DNS，playurl 首跳大幅提前 | 中 |
| **3** | **恢复部分复用**：先只在 Xbox 重新启用 CURL_SHARE 的 DNS + SSL session（`CurlSharedObject` 已带锁回调）；keep-alive 单独立项（见 §6） | `http.hpp:91-93` | 每请求省一次 DNS + 一次完整 TLS 握手 | 中 |
| **4** | **线程模型**：Xbox `cpr::async` 换成固定小线程池（4~6 worker + 任务队列），消除 per-TU 计数器未定义行为；图片与 API 分池或 API 优先，避免 playurl 排在缩略图后 | `async.h`（Xbox 分支）、`image_helper.cpp` | 请求延迟稳定，消除 CreateThread churn | 中高 |
| **5** | **削主线程 sync 尖峰**（#9 修掉 12s 后，剩余就是这条）：本轮实测仍有 `slow sync task 512~926ms`（tag `video`/`main`/`rcmd`/`cursor`）→ 图片纹理创建仍留主线程但可加每帧预算（每帧最多 N 张）；弹幕 sort 移到 worker、sync 只做提交；大结果体改 move 进 sync lambda。注意逐像素 swap 的 WC 代价已在 #9 消除（§5.10），本项只管**任务粒度与预算** | `nanovg_d3d9.h`、`image_helper.cpp`、`danmaku_core.cpp`、`video_detail.cpp`、`thread.cpp` | `PHASE_SYNC` 峰值下降，UI 不再卡帧 | 中 |
| **6** | **日志降噪**：`mbedtls.c:377` 握手逐迭代 DbgPrint 改为只打结果一行；验收跑默认 log level 降到 info；确认 OXDK stdout 是否阻塞，必要时加行缓冲 | `mbedtls.c`、`connect.c`、`xbox360_stubs.c`、`main.cpp` | 消除主线程自旋锁等待 | 低 |
| **7** | **WBI nav 修复**（✅ **2026-10-03 已完成**，六轮实测 `now=1791007114` epoch 正确、整场只 refresh 1 次；§5.5-2、§5.6-4、§5.7-1 根因：`wbi.cpp:74` 与 `:177` 的 `std::time(nullptr)` 在 XDK 返回 `(epoch<<32)|x` 打包值 → `now-last<3600` 恒假）：改用 `std::chrono::system_clock`（`wbiNow()` helper）；in-flight 合并（并发 `updateWbiKeys` 只发一条 nav）作为第二道保险（未做） | `wiliwili/source/api/util/wbi.cpp` | 消除每次开视频 13~16s ×2 的重复 nav 及 playurl 串行阻塞 | 极低（两行） |
| **11** | **mbedtls 私有分配器**（✅ **2026-10-03 已完成并确认注册生效**，见 §5.9：`platform.c` 里 `x360_pool_*` 4MB arena + 9 个 size class，`config_helper.cpp` Xbox 分支启动期 `x360_mbedtls_pool_init()` 注入；`config-xbox360.h` 已开 `MBEDTLS_ECP_NIST_OPTIM`。⚠ **池本身不够，真瓶颈在池锁，见 #13**） | `third_party/mbedtls/library/platform.c`、`wiliwili/source/utils/config_helper.cpp`、`third_party/mbedtls/include/mbedtls/config-xbox360.h` | 消除"小块 MPI 分配在 CRT 全局堆锁上排队"（五轮主因） | 中（只影响 mbedtls，非全局堆） |
| **13** | **池锁：`CRITICAL_SECTION` → 自旋锁**（✅ **2026-10-03 已完成，本轮根因，见 §5.9**）：七轮分解探针 84407 次慢操作里 `cs`(等锁) 合计 **2326 秒**、`hold` 仅 10 秒、`memset` 71ms、时钟 23ms，`cs` 严格量化在 20ms 整数倍 → 持锁者微秒级就释放，但**睡进内核等锁一次要付 ~1 个调度 quantum(20ms)**；6 路并发 → 每次分配 20/40/60ms → 一次 ECDH(≈4800 次分配) ≈ 97s。改为 `InterlockedCompareExchange` 紧自旋 + 拿不到才 `Sleep(0)` 让路（防活锁）后：握手 p50 **180ms**、max 591ms、fails 24→**0**、队列 max 136s→**6.2s** | `third_party/mbedtls/library/platform.c` | 消除 100s 级"解析视频卡死/主页之后全加载不出来" | 极低（临界区微秒级） |
| **12** | **减少握手数 / 并发错峰（§5.8-4 放大器）**：9 路并发 = 9 次全新握手，堆锁排队随并发超线性恶化；先做闸门错峰（burst 限流到 2~3 路）与 §6 的连接复用立项，DNS/SSL share（#3）同批 | `async.h`、`http.hpp` | 与 #11 相乘，进一步压掉 burst 尾延迟 | 中 |
| **8** | **阻塞 recv/send 硬超时**（§5.3-1 实测；⚠ **§5.7-2 / §5.8-2 已基本排除**：`recv n=12 wait=24~104ms`、`send wait≈0ms`、`iters=1`，慢不在网络 I/O 而在堆锁）：优先级下调为**防御性加固**——仍需给 `mbedtls_bio_recv/send` 加 deadline 防 `ret=-0x7280`/40s 挂死类个案；顺带复核 `fatal alert (-0x7280)` | `mbedtls.c:102,341-344` | 防挂死（非主路径提速） | 中（已非瓶颈） |
| **9** | **主线程 12.4s 冻结**（✅ **2026-10-03 已修复，12895ms → 218ms，见 §5.10**；根因不是"任务在主线程"本身，而是 `nanovg_d3d9.h:487-507` 的 GPUENDIAN_8IN32 逐像素交换**原地打在 write-combined 纹理显存上**，4800×2700 = 1296 万像素 × (2 读 + 4 写) ≈ 957ns/像素。改为在 19KB 普通缓存 scratch 里交换后再整行顺序 memcpy，输出字节不变：`create=13ms fill=202ms`。触发时机见 §5.10-1——快照请求与 playurl 同批过闸门，恰好卡在 mpv 出画面那一刻，所以表现为"播放开始才卡"） | `library/borealis/library/lib/extern/nanovg_d3d/src/nanovg_d3d9.h:487-507` | 消除开视频后 12s 的 1-8fps 冻结（59×） | 低（输出逐字节等价） |
| **10** | **history/report 节流**（§5.5-5 新增；⚠ §5.6-7 本轮仅 7 条、比首测收敛，优先级下调）：播放期间改为 15-30s 一次或暂停时上报 | `video_detail.cpp` / history 上报处 | 减少冗余连接 | 低 |
| **14** | **纹理缓存无上限 → 物理内存耗尽**（崩溃 / 封面全不显示 / 设置卡死，✅ **2026-10-03 已定位并修复，见 §5.11**；⚠ 比 #12~#10 都严重——会直接崩，故插到最前）：① `LRUCache::setCapacity()` 无条件 `c += DEFAULT_CAPACITY(400)` → 默认 200 变 **600 张**，封面 `@672w_378h` = 1.02MB/张 → **612MB > 512MB 物理内存**（实测 371 张时可用堆已跌破 64MB、440 张时 `heap>=0`）；② `Image::innerSetImage()` 覆盖旧纹理时不释放缓存引用 → `count` 恒 >0 → `deleteCache()` 只删 `count<=0` 的条目 → **LRU 永不生效** → 即使有上限也无限增长。修复：Xbox360 不叠加 +400 且硬限 **200 张**（PSV 同 512MB 靠 `@256w_144h`+DXT 仅 147KB/张）、补齐 `innerSetImage` 的 `removeCache`、删掉 `checkCache` 的重复释放（否则 count 下溢） | `library/borealis/library/include/borealis/core/cache_helper.hpp`、`library/borealis/library/lib/views/image.cpp` | 消除滚动首页后封面解码失败（`stbi outofmem`）、纹理创建失败、`write 0x00000000` 崩溃 | 低（三处小改，需验证引用配平） |

**执行顺序（2026-10-03 十轮实测后重排）**：
`✅ #7 wbi chrono → ✅ #11 mbedtls 私有池 → ✅ #13 池锁自旋（100s 卡死消除，§5.9）
→ ✅ #9 快照纹理字节交换（12.4s 冻结 → 218ms，§5.10）
→ ✅ #14 纹理缓存上限 + 引用泄漏（堆耗尽 → 封面全不显示 + 硬崩溃，§5.11，因严重度最高而插队）
→ 12 并发错峰/减少握手 → 5（剩余 sync 尖峰 512~926ms）
→ 2 / 6（DNS、日志降噪）→ 3 / 4（curl fork / 线程模型）
→ 8（bio deadline 防挂死）→ 10（history/report）`。
**残留待查**：① `#13` 后仍有 1789 次 20ms 量化慢分配（§5.9.3，需 `fb=` 计数器定罪）；
② `syncTex` 已 218ms，但 `slow sync task` 仍有 512~926ms 的亚秒尖峰（`video`/`main`/`rcmd`/`cursor`，§5.10-3）；
③ 直播页 `https://` 封面 `err=10 SSL_CONNECT_ERROR` 真失败（§5.11-6）；
④ 堆探针梯子最粗只到 64MB，看不到 64MB 以上的下降曲线（需加 256/192/128MB 桶）。
（原「四轮埋点」已全部执行完毕，结论见 §5.7 / §5.8 / §5.9 / §5.10 / §5.11。）

---

## 5. 首轮实测数据（2026-10-02，修复计划 #1 已执行）

> 环境：devkit "ccb"（192.168.3.126）真机，日志 `logs/p0_perf_20261002_234822.txt`
> 埋点（均仅 `PLATFORM_XBOX360` 编译）：`[x360-net]` 每请求一行
> （`http.hpp` `RequestLog` RAII，仅 `_cpr_get`/`_cpr_post` Xbox 分支）、
> `[x360-queue]`（`async.h` 闸门等待 ≥10ms 打一行）、`[x360-perf] sync split`
> （`xbox360_perf.*` + `thread.cpp` `noteSyncTasks`）、`[nav] ok` 升 info 级。

### 5.1 对 §2 假设的验证

| 指标 | 实测 | 结论 |
|---|---|---|
| API 请求耗时（`feed/rcmd`×3） | total 881/904/917ms，其中 **tls 529/534/563ms（约 60%）**；ttfb−tls≈300ms 为真服务端耗时；dns 2ms；conn 40ms | §2.2 确认：TLS 主导每请求耗时 |
| 双线程 + 闸门开销 | q1（提交→外层线程执行）2.9-9.5ms；q2（回调分发→进入）≈17.7ms | 每请求线程开销 ~20ms，远小于 TLS，不是主矛盾 |
| 回调处理（cb） | 首页/轻接口 1.4-25ms ✓；但视频页 `reply/main` **每次 1.8-5.7s**（7 次无一例外）、`playurl` 最大 12.5s | §2.5 只对小 JSON 成立：**大响应无压缩 parse 在 Xenon 上是秒级**，见 §5.4 |
| WBI Nav 串行 | 首轮 `t=515-569ms`；视频页复测 `t=5075-21374ms`（每次开视频成对触发 2 条） | §2.4 确认，且**比预估严重一个量级**，见 §5.5-2 |
| 4 槽闸门排队（两轮共 167 次 ≥10ms） | 首轮 p50 824ms / p95 4.5s / max 18.3s；视频页极值 **q1=24544ms**（fav 请求提交后在闸门外等 24.5s，4 槽全被卡死 burst 占满） | §2.3 确认：burst TLS 挂起会把闸门饿死 |
| DNS | 2-41ms（并发 3 请求各自新解析，返回不同 IP） | §2.1 本轮规模下未构成瓶颈；视频页 10+ burst 待复测 |

### 5.2 对 §3 假设的验证

| 指标 | 实测 | 结论 |
|---|---|---|
| 图片纹理创建（sync image 桶） | **每次 215-581ms**（中位 ~240ms） | §3.1 确认，**远超文档预估**（原估 20 张共几十 ms，实为单张即 200ms+，还有放大因素待查） |
| sync 单帧倾倒 | 爆发窗口单帧最多 14-18 个回调 | §3.1 确认 |
| 帧率侧证 | 5 个 `0 fps` 窗口：`sync avg 2138-3014ms/帧`（窗口内仅 1-2 帧渲染），首页/动态每次加载 **UI 冻结 2-4 秒** | §3.1 是卡顿主因，修复 #5 收益最大 |
| 弹幕（sync danmaku 桶） | 播放带弹幕视频：加载（sort+拷贝）**11.71ms**，一次性；播放期间无弹幕 sync 任务（渲染走 draw 路径） | ✅ 弹幕排除卡顿嫌疑，与 §3 预估一致 |

### 5.3 新发现（审查时未记录）

1. **TLS 握手挂死 ~41s**：00:15:01 动态页 3 个请求 `conn 40-81ms` 后进入阻塞 recv，
   41 秒内**无 WANT_READ 迭代、无 recv 返回打印**，00:15:42 三者同时完成（code 200）。
   根因链：`mbedtls_bio_recv`（`mbedtls.c:102`）裸阻塞 `recv()` → 握手前设置的
   `SO_RCVTIMEO/SO_SNDTIMEO=10000`（`mbedtls.c:341-344`）实测未生效 →
   `HTTP::TIMEOUT=10s` 只在 curl 状态机层检查、无法中断 bio 内阻塞 →
   唯一兜底是 TCP 重传恢复（~41s）。期间 UI 正常 60fps，但 3/4 槽位被占 →
   图片排队冲到 18.3s（§2.3 的放大器）。→ 修复计划新增 **#8**。
2. **3 次 `handshake ret=-0x7280`**（`MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE`）握手失败——
   疑似连接频率过高被服务端掐；无 keep-alive（每图一连接）会放大此风险，待复核。
3. **`sync split` 的 `other` 口径缺陷**：`other = PHASE_SYNC 每帧均值 − image 每回调均值`
   单位不一致，冻结窗口内只有 1-2 帧时 `other` 实为图片时间残余，仅供参考；
   image/danmaku 每回调耗时准确。下轮改为窗口总量（total）口径核算。
4. **图片不产生 `[x360-net]`**：图片直连 `session.Get()`（`image_helper.cpp:252`）不经过
   `_cpr_get`，目前只能看到其闸门排队；如需图片网络耗时需单独打点。
5. **部署注意**：xbdm `magicboot` 必须带 `directory="DEVKIT:\wiliwili"`，否则 `game:\`
   仍指向 xshell 目录，启动即 `Fatal: main.xml error 3`。

### 5.4 视频页复测（2026-10-02 深夜，7 次开视频，日志续接同一文件）

| 视频 | 点开 → MPV START_FILE | nav 阻塞 playurl | playurl TLS | playurl cb | mpv set_url 冻结 |
|---|---|---|---|---|---|
| V1 00:36:01 | **37.3s** | **15.8s** | 6.7s | **12.5s** | 12.3s |
| V2 00:39:50 | 13.2s | 5.1s | 7.2s | 0.25s | — |
| V3 00:43:39 | **27.4s** | 9.4s | 3.2s | 0.14s | **12.4s** |
| V5 00:46:28 | ~20s+ | 18.8s | 10.3s | 0.16s | — |
| V7/V8（最好情况） | ~13-15s | 4-5s | 2.8-3.7s | 3-55ms | 1.2s |

"解析视频超慢"实测分解为**五层叠加**（按单次占比排序）：

1. **WBI nav 串行阻塞 playurl 提交**（首路径 42%）：detail 1.2s 即返回，但
   `get_video_url` → `updateWbiKeys` 要等 nav 完成才签名发请求（V1 中 playurl 直到
   15.8s 后才 submit）。nav 本身又是成对两条、TLS 卡 5-21s（§5.5-2）。
2. **burst TLS 批量挂起 3~21s**：handler 同时发出的 5-8 个连接一起卡、一起放
   （同批 tls 值互差 <200ms），playurl 自己也常中招（§5.5-1）。
3. **cb 回调处理 0.14~12.5s**：`reply/main` 评论 JSON 无压缩 parse 稳定 1.8-5.7s；
   V1 playurl 12.5s 为异常值（同尺寸响应其它视频 0.14-2.9s），叠加当时 CPU 争抢（§5.5-4）。
4. **mpv set_url 同步冻结 12.3s**：主线程 0fps 直到 `MPV_EVENT_START_FILE` 前
   16-140ms 才恢复，两次实测（§5.5-3）。
5. **闸门排队**：q1 极值 24.5s（§5.1），q2 最大 647ms。

### 5.5 第二批新发现（视频页复测）

1. **burst TLS 批量挂起模式**：同一次页面打开同时建立的连接（detail/relation 除外——
   它们先到先跑没事）**一起卡 3~21s、一起放行**；卡顿窗口内的孤立单发请求也会中招
   （00:44:09 一条 history/report 卡 19.6s）。时长与 DNS IP 无关（同一 IP 时快时慢）、
   与连接数不成正比（3 连接卡 41s / 10 连接卡 4s 都有）→ 疑似握手被某个全局路径
   串行化/挂起（待二轮埋点：mbedtls per-fd 进出时间戳验证）。
2. **WBI nav 每次开视频都重新拉取，TTL 1 小时逻辑失效**：
   - 每次开视频恰有 **2 条 nav 同毫秒启动**（videoUrl + pageDetail 的 `updateWbiKeys`
     背靠背调用，key 为空时各刷一条，无 in-flight 合并）；
   - 刷新间隔实测 3.8 分钟、甚至 **10.9 秒**（V4 的 `requestVideoUrl` 二次触发），
     远小于 TTL → `g_mixin_key` 被 set 后又变 empty，**疑似内存破坏或
     `PlatformMutex` 竞态**（成对 nav 的两个回调并发写同一 string）；
   - 后果：每次开视频 playurl 被 nav 串行阻塞 5~21s。
   - 待办：`updateWbiKeys` 分支日志（`refresh/valid age=Xs empty=Y`）一锤定音。
3. **mpv set_url 同步冻结 12.3s ×2**（V1 `sync max 12294ms`、V3 `sync max 12371ms`）：
   两次都在 `MPV_EVENT_START_FILE` 前 16-140ms 结束 → 冻结的 sync 任务属于
   "设置 mpv 播放地址"链路，阻塞到 mpv 真正开始装载文件才返回——mpv 自身网络栈
   （独立于 app 闸门）大概率踩中同样的 TLS 挂起。冻结期间 UI 完全 0fps
   （V3 窗口 13.4s 仅渲染 1 帧）。→ 修复计划新增 **#9**。
4. **`reply/main` cb 稳定 1.8-5.7s**（7 次无一例外）：无压缩评论 JSON +
   nlohmann parse 在 Xenon 上是秒级 → §2.5 的"解析不是问题"只适用于小响应；
   需要 cb 分阶段埋点（decode / parse+body size / 业务回调）量化。
5. **`history/report` 播放期间每 1~4 秒一次**（70+ 条，每条一条新 TLS 连接，
   tls 0.6-3.8s）——自造的高频连接源，占闸门、可能触发服务端掐连接
   （fatal alert -0x7280）。→ 修复计划新增 **#10**。
6. **一批请求绕过 `_cpr_get`，无 `[x360-net]` 行**：`PageDetail`、`dm/list.so` 弹幕、
   Highlight、Snapshot（图片池）——burst 实际连接数比 net 行多，二轮埋点需补覆盖。
7. **18.5 分钟日志静默**（00:16:47→00:35:23 完全无输出，进程未重启，之后直接恢复）——
   疑似挂起/休眠，与 TLS 挂起是否同源待查。
8. **cb 内的 12.5s 与主线程 12.3s 冻结的耦合**（V1 两段几乎同起同终）：一条是 worker
   上的 playurl 回调、一条是 main 上的 set_url 任务，二者关系需 sync 任务标签确认。

### 5.6 第三轮实测（2026-10-03，二轮埋点生效：mbedtls 进出时间戳 / cb 拆分 / sync 标签）

> 日志 `logs/p0_perf_20261003_022724.txt`（2302 行，35 条 `[x360-net]`、59 次握手），
> 启动 02:29:38，两次开视频 02:38:43 / 02:39:57（BV1Sfa26cEcg、BV1VMYE64EeP）。

1. **TLS 握手是绝对主因，且"全局串行化"假设被证伪**：
   - 59 次握手 `dur` p50=**12590ms** / p90=19500ms / max=21330ms，42/59 >3s；
     `[x360-net]` 的 `tls` 阶段 p50 3892 / p90 16041 / max 16134ms。
   - 9 个 `hs enter` 在 ~110ms 内同时发出、`hs exit` 集中在 14~16s 之后成簇返回
     （`fd=369700865..369729544`）→ **并发同时进行、非全局锁串行**。
     §5.5-1 的"握手被某个全局路径串行化"猜测**不成立**，慢在共享的外部瓶颈
     （网络/CPU 争抢），与连接数不完全成正比的观测与此一致。
   - 对照组：单发或少量并发时握手仅 435~1998ms，**慢完全由 burst 触发**。
2. **`recv stall`（≥1s）本轮计数为 0，与 §5.3-1 的 41s 阻塞 recv 结论矛盾**：
   - 全程只有 1 条 `handshake ret`（+ffmpeg 一条 `-0x4e`），说明时间**不**花在单次
     长阻塞 recv，也**不**花在 WANT_READ `Sleep(10)` 重试循环（那会刷屏）。
   - 剩余嫌疑：大量 <1s 的涓流 recv、**阻塞 `send()`（未埋点）**、或 CPU/加密争抢。
     → 四轮埋点：recv 阈值降到 ~200ms + 按 fd 累计 recv/send 等待与次数、
     `hs exit` 打迭代数、`mbedtls_bio_send` 加 stall 计时。
   - 另有 1 次 `-0x7280` fatal alert、1 次 `ffmpeg: tls: -0x4e`。
3. **12.3s 主线程冻结根因锁定：`[Snapshot] syncTex 4800x2700 took 12321/12308 ms`**
   （各配对一条 `slow sync task took 12321/12309 ms (batch 1)`，两次开视频各一次）——
   `nvgCreateImageRGBA` 创建 4800×2700（~51.8MB）纹理在**主线程**执行，
   疑似卡在 `nanovg_d3d9.h` 逐像素 GPUENDIAN_8IN32 swap 循环（写纹理内存 ≈4.2MB/s，
   远低于正常拷贝速度）。期间帧率掉到 6-8fps。
   → **修正 #9**：此前 §5.5-3 归因于 mpv `set_url` 同步冻结，本轮直接证据指向
   快照纹理创建；mpv 冻结需用 sync 标签进一步区分（是否同一条任务）。
   → **✅ 疑点已坐实并修复，见 §5.10**：[tex] 分段探针 `create=13ms fill=202ms`，
   即原先 12.4s 几乎全在逐像素交换；sync 标签也确认与 mpv 无关。
   其它慢 sync 任务：1715/1055/937/632ms（启动与开视频期）。
4. **WBI 双发坐实，但出现新异常**：
   - 两对 `refresh age=5256/5056s inflight=1/2`（02:38:43）、`age=200/0s inflight=1/2`
     （02:39:57），每对两条 nav 并行、各耗 13.8~16.1s → **无 in-flight 合并，#7 成立**。
   - 异常：age=200s/0s（<3600）仍走 refresh 分支，与代码逻辑矛盾；启动时还出现
     `age=1879368616s`（≈2029 年，疑似 devkit 时钟异常）。疑 `(long)(now-last)` 截断
     （XDK `long` 32 位 vs `time_t` 64 位）/ 无锁撕裂读 / 时钟跳变。
     → 四轮埋点：refresh 分支打印原始 `now`/`last` 值。
5. **闸门饿死被量化**：`[x360-queue] wait` n=77，p50=**21005ms** / p95=29358 /
   max=41344ms（`active 4`）——下游是 15s 握手长期占满 4 个槽，印证 §2.3 放大器。
   （同一日志里 `[x360-net]` 的 q1/q2 却只有 5/11ms 均值：API 请求很少排到队首，
   长等待主要落在图片等无 net 行的任务上。）
6. **解析进一步被排除**：`[x360-parse] json 10.6~11.0ms`（body 66-69KB）、
   大响应 80.1ms（118KB）；`cb` 多数 <100ms。例外：`dm/list.so cb 513ms`、
   snapshot jpg `cb 899ms`（stbi 解码在 worker 上，非主线程）。
   → §5.5-4 的"大响应 parse 秒级"在本轮未复现，`reply/main` 1.8-5.7s 需复核
   （可能与本轮响应是否压缩有关）。
7. **`history/report` 本轮仅 7 条**（tls 0.7~2.0s），比 §5.5-5 的"每 1~4s 一条、70+ 条"
   明显收敛——节流优先级可下调（#10）。
8. **`bvc.bilivideo.com/pbp/data` 高亮进度 → code 404**（预存在问题，浪费一条
   ~15s 的握手连接）。

**四轮埋点计划**：① `mbedtls_bio_send` stall 计时；② recv stall 阈值 1000→200ms 并
按 fd 累计 recv/send 等待与调用数、`hs exit` 打印迭代数；③ `wbi.cpp` refresh 分支
打印原始 `now`/`last`；④ sync 任务标签区分 `syncTex` 与 mpv `set_url`。

---

### 5.7 第四轮实测（2026-10-03，四轮埋点生效：send stall / fd 累计 / wbi 原始值 / sync 标签）

> 日志 `logs/p0_perf_20261003_124201.txt`，99 次握手，`dur` p50=**5557ms** /
> max=23992ms（比三轮 p50 12.6s 低，但量级同源：慢仍由 burst 触发）。

1. **WBI TTL 根因坐实 = `std::time(nullptr)` 在 XDK 上返回 64 位打包值**：
   - 探针打印 `wbi refresh ... now=7692297454196417448 last=... age=1879368616`，
     拆开看 `now` 高 32 位 = **1791002567 = 当时真实 epoch**，低 32 位 = 1879368616
     = 打印出的 `age` ⇒ `now` 形如 `(epoch << 32) | x`。
   - 于是 `now - last < 3600` **恒为假** → 每次开视频都强制 refresh nav（§5.6-4
     的双发异常由此完全解释）；`age=5256/200/0/1879368616` 全是低 32 位截断噪声。
   - `encWbi()` 的 `wts`（`wbi.cpp:177`）同样是垃圾值（服务端容忍，仍 200）。
   - **修复**：`wbi.cpp:74` 与 `:177` 改用 `std::chrono::system_clock`
     （brls logger 的 chrono 路径实测正确，可直接参照）。
2. **握手时间不在网络 I/O**：`hs-exit ... iters=1 | recv n=12 wait=24~104ms |
   send n=4 wait≈0ms` —— 迭代仅 1 次、recv 累计等待 <105ms、send 几乎 0，
   与 `dur` 数秒~20s 相差两个数量级。3 次失败 `ret=-0x7280`（CONN_EOF，
   recv n=9-10 后对端关闭）。→ 排除 §2.1「DNS/网络慢」之外的单次阻塞 recv/send
   （§5.6-2 的嫌疑清单）。

---

### 5.8 第五轮实测（2026-10-03，逐 TLS 状态归因 + 分配器探针 + 线程 CPU 时间）

> 日志 `logs/p0_perf_20261003_132131.txt`。埋点：`mbedtls_ssl_handshake_step()`
> 自绘状态循环按状态累计耗时、`ecp mul` 计时、`mbedtls_calloc/free` ≥10ms 探针、
> `GetThreadTimes` 线程执行时间、`xbox_bio_active` 使传输期 recv/send 无锁。

1. **瓶颈状态锁定 = TLS `CLIENT_KEY_EXCHANGE`（ECDH）**：
   `hs-exit st: SERVER_HELLO=x CKE=<dominant> SV_CCS=x`，例如
   `dur=3409 cpu=313 ms | recv n=12 wait=14 ms` 中 `CKE=3389ms`（99%）。
   密码套件 `TLS-ECDHE-RSA-WITH-AES-128-GCM-SHA256`，`ecp mul grp=3` = **secp256r1**。
2. **是"阻塞"不是"CPU 密集"**：`fd=448495622 hs-exit dur=21637 ms cpu=512 ms
   iters=1 | recv wait=46 ms | send wait=0 ms` —— 21.6s 墙钟里线程只执行了
   **0.5s**，且网络等待可忽略 ⇒ 线程绝大部分时间**在等锁**。
3. **共享堆（CRT `calloc`/`free`）护送效应 = 主因**（该 21.6s 窗口内，3~9 路并发）：
   | 探针 | n | sum | p50 | p95 | max |
   |---|---|---|---|---|---|
   | `ecp mul` | 17 | **106802 ms** | 4153 | 17464 | **17464** |
   | 慢 `calloc`（≥10ms） | 1557 | 31788 ms | 20 | 40 | 60 |
   | 慢 `free`（≥10ms） | 1521 | 31287 ms | 20 | 40 | 60 |
   - 单次 P-256 标量乘最慢 **17.5s**（空闲态同一路只有 142~713ms）。
   - 慢的全是 **4~68 字节的微小分配**（`calloc n=17 sz=4 dt=20 ms`），大分配不慢
     ⇒ 不是"分配本身贵"，而是**在共享堆锁上排队**；`free` 同样 20ms 说明持有者
     是别的线程的大块 alloc/free（快照 4800×2700×4 ≈ **51.8MB** 缓冲等），
     量级 ~20ms 且呈 20/40/60 量化。
   - 慢 `calloc`+`free` 合计 63s，解释 `ecp mul` 106s 中的大头；
     其余为 <10ms 未打印的分配累积。全程慢 `calloc` 7800 条、慢 `free` 同量级。
4. **时间线相关性（10s 桶）**：静默期每桶慢 calloc 仅 4~22 次、握手 1~3 条；
   burst 期（13:24:40-13:25:00、13:27:10-13:27:40）每桶 **700~1350** 次，
   `hs` 合计墙钟 **128~141s / 10s**（≈14 路并发当量）、`ecp mul` 9~13 条合计
   60~80s。**放大器确认：并发越高，堆锁排队越长**（§2.3 的 4 槽闸门 + 每请求一线程
   正好制造了 9 路并发）。
5. **UI 掉帧与握手风暴是两件事**：burst 期间帧率多为 **60fps**（网络线程阻塞不影响
   渲染）；掉到 9fps / 1fps 只与 `syncTex` 冻结对齐（13:25:00 fps=9、13:27:47
   `syncTex 4800x2700 took 12640 ms` → fps=1），二者时间窗重叠但因果独立。
   `slow sync task took 1426/12640 ms` 标签已确认来源是
   `42030399904_0hyngb-0001.jpg`（快照大图纹理创建，§5.6-3 维持）。
6. **探针缺陷**：少数 `cpu=-1`（`GetThreadTimes` 在 begin 时刻返回 0），
   `DbgPrint` 需手工 `extern void DbgPrint(const char*,...)` 声明；
   `ecp mul`/`calloc` 行无 fd，跨并发只能窗口聚合。

**结论链**（§2/§3 假设的最终形态）：
`开视频 → 9 路并发请求 → 每路一次全新 TLS 握手 → CKE 阶段大量微小 MPI 分配 →
在 CRT 全局堆锁上排队（持有者是 51.8MB 级大块 alloc/free 与图片/纹理活动）
→ 单次 ECDH 从 0.15s 膨胀到 4~17s → 握手 wall 12~21s → 4 槽闸门被占满
→ queue p50 21s（§5.6-5）→ 视频页 15~30s 才出数据`；
另一条独立链：`nvgCreateImageRGBA 主线程逐像素 swap → 12.6s 冻结`。

**修复指向（按数据重排，详见 §4）**：① mbedtls 私有分配器（预留 arena /
`mbedtls_platform_set_calloc_free` 指向自有池，Xbox 分支当前走默认 CRT
`calloc`，见 `config_helper.cpp:1036-1043`）；② `wbi.cpp` chrono 修复（#7 已定位）；
③ 减少握手数（连接复用 / 闸门错峰）；④ syncTex 移出主线程（#9）；
⑤ `config-xbox360.h` 未启用 `MBEDTLS_ECP_NIST_OPTIM`（基线 142~713ms 也偏慢）。

---

### 5.9 第六~八轮实测（2026-10-03，#7 + #11 落地后出现新停顿；根因最终定位为**池锁的内核睡眠延迟**）

> 环境：devkit 真机，日志 `logs/p0_perf_20261003_135821.txt`（六轮）、
> `_143437.txt`（七轮）、`_144643.txt`（八轮）。
> 六轮 = #7 `wbi chrono` + #11 私有池 + `MBEDTLS_ECP_NIST_OPTIM` 首次上机；
> 七轮 = 加 `cs/hold/zero/q` 分解探针；八轮 = 池锁改自旋锁。

#### 5.9.1 六轮：私有池生效后反而出现 ~100s 停顿

- 早期一切正常：`[tls] pool ready arena=4096KB`、`[wbi] refresh now=1791007114`
  （epoch 正确，全场只 refresh 1 次 → **#7 修复确认生效**）、握手 p50 **148ms**
  （五轮是 10716ms），用户反馈"这次快多了"。
- 但 14:11:50-14:13:31 出现新停顿：5 路握手 `hs enter` 后**从未 exit**，
  `st: SERVER_HELLO=29 CERT=16 CKE=99575ms`、`ecp mul dt=97228/98135/98078/98087/84771`、
  `hs-fail dur≈99700 cpu=36ms recv wait≈40ms send wait=0` → 99.7s 里**只跑了 36ms**，
  既不是 CPU 也不是网络 I/O；最后服务端超时关闭 → `ret=-0x7280(EOF)` 同时全灭。
- 同窗口内**每一次** mbedtls 分配都 ≥10ms（20199 条 `[tls]`，200~360 条/秒，
  全场 UI 仍 60fps、日志正常 → 排除日志通道背压）。
- 算术自洽：每次分配/释放 ≈20ms、一次 ECDH ≈4800 次分配 → 4800×20ms ≈ **96s ≈ 实测 97s**。
  连带 `[x360-queue] wait 99933ms active 4` → **闸门 4 槽全被卡死的握手占住，
  后续请求排队 100s → "主页能加载一点、其它全加载不出来"**。

#### 5.9.2 七轮：分解探针定罪 —— 时间 99.8% 花在**等池锁**

探针（`platform.c`）在一次分配里拆出 `tot / cs(等锁) / hold(持锁) / zero(memset) / q(时钟自身)`：

```
[tls] pool registered fn=826FE1A0 pool=826FE1A0 crt=830F4460   ← 池确实装上了
[tls] pa n=8 sz=4 tot=40 cs=40 hold=0 zero=0 q=0
[tls] pf cls=3     tot=60 cs=60 hold=0 q=0
```

| 分项 | 七轮合计 | p50 | 说明 |
|---|---|---|---|
| `cs` 等锁 | **2,326,694 ms** | 20ms | 84407 次慢操作中 84384 次 `cs>0` |
| `hold` 持锁 | 10,480 ms | 0ms | 持锁者微秒级就释放 |
| `zero` memset | 71 ms | 0ms | |
| `q` 时钟 | 23 ms | 0ms | **探针自身无污染，排除测量假象** |

`cs` 取值严格量化在 **19/20/39/40ms**（20 的整数倍），`hold≈0` →
**不是持锁者不放，而是每次"睡进内核等锁"要付 ~1 个调度 quantum（20ms）的阻塞/唤醒延迟**；
6 路排队 = 每次分配 20/40/60ms。七轮全程：握手 n=11 p50=17918ms、
`[x360-queue] wait 81584 / 106016 / 136039 ms active 4`、fails=24。

同时排除的两条旧假设：① **池没注册**（`fn == pool` 指针一致，小块没走 CRT，
`pc big` 回退仅 2 次）；② **CRT 全局堆锁是主因**（五轮结论，本轮池内 `cs` 独占 99.8%）。

#### 5.9.3 八轮：`EnterCriticalSection` → 自旋锁，停顿消失

临界区只有"链表 push/pop + 一次打点"（微秒级），这种场合不该睡进内核：
`x360_pool_lock()` 先紧自旋 64 次，拿不到才 `Sleep(0)` 让路（防持锁者被换下去时活锁）。

| | round4 | round5 | round6 | round7(CS) | **round8(自旋)** |
|---|---|---|---|---|---|
| 握手 n | 135 | 58 | 261 | 11 | **122** |
| 握手 p50/p90/max (ms) | 4877/19227/23992 | 10688/20151/21717 | 133/234/1544 | 17918/55365/55409 | **180/373/591** |
| `ecp mul` p50/max (ms) | — | 4403/17464 | 36/**98135** | 52786/214090 | **52/385** |
| 握手失败 | 3 | 2 | 12 | 24 | **0** |
| 队列等待 max (ms) | 33052 | 31044 | 99933 | 136229 | **6199** |
| 慢分配条数 | — | 7800 | 10440 | 84407 | 1789 |

用户实测："**正常了**"（进视频页、首页翻页、连续开关均无停顿）。

**残留（未解决，优先级待定）**：八轮仍有 1789 次慢操作，`cs` 分布仍是
19/20/39/40ms 量化（合计 ~45s / 90s）。因自旋 64 次后仍会 `Sleep(0)`（睡一次 = 1 quantum），
需加 `fb=` 计数器区分"自旋落空进 Sleep"还是"拿锁前就被调度器换下"；
若确认是前者，把自旋预算提到数千次或**做按线程分片的本地缓存**（稳态不碰全局锁）。

#### 5.9.4 本轮结论链（覆盖 §5.8 结论链的分配器部分）

```
开视频 → 6~9 路并发请求 → 每路一次全新 TLS 握手 → CKE 阶段几千次微小 MPI 分配
→ 每次都去抢同一把池锁 → 内核阻塞/唤醒一次 ≈ 20ms（1 个 quantum）
→ 单次 ECDH 0.05s 膨胀到 97s → 服务端 100s 超时 EOF → 4 槽闸门被占死
→ 后续请求排队 100s → 视频解析卡死 / 主页之后全加载不出来
修复：微秒级临界区改自旋锁（不睡内核）→ 97s → 0.385s，fails 24 → 0
```

---

### 5.10 第九轮（#9 主线程 12.4s 冻结，✅ 已修复：12895ms → 218ms）

> 日志：`logs/p0_perf_20261003_144643.txt`（修复前）、`_152212.txt`（修复后）

#### 5.10.1 现象与触发时机（与用户实测对齐）

用户实测"**只有解析视频成功、mpv 开始播放时才卡 12 秒**"。日志时间线坐实因果：

```
14:47:07.867  setFrameSize 1920x1080 → 1280x720      ← 开视频页
14:47:09.370  [Snapshot] loadTexture start …bimp…0001.jpg
14:47:11.904  mpv: (+) Video --vid=1 (h264 1280x720)
14:47:11.969  [x360-net] GET bimp…jpg dns 27 conn 34  ← 快照 jpg 下载完成
14:47:12.043  [x360-mpv] container 29.97              ← mpv 已在解码
────────── 12.5s 内一条 perf 日志都没有：主线程整段死掉 ──────────
14:47:24.712  [Snapshot] syncTex 4800x2700 took 12510 ms
14:47:24.713  MPV_EVENT_FILE_LOADED                   ← 事件被压到此刻才处理
14:47:24.742  VO: [libmpv] 1280x720 yuv420p           ← 画面这时才出来
14:47:24.743  Audio device underrun / A/V desync       ← 音频被一起拖累
```

**时机成因**：`requestVideoSnapshot`（`video_detail.cpp:385,432`）与 playurl 在同一批发出、共用 4 槽闸门 → 快照 jpg 下载+解码恰好落在 playurl 之后 2~4s → `setSnapshotData`（`video_snapshot_core.cpp:35`）无条件 `loadTexture(0)` → 解码完 `brls::sync` 回主线程 → 正好卡在 mpv 要出画面的那一刻；mpv 的 `FILE_LOADED`/VO 初始化事件都在主线程泵取，被整体推后到冻结结束。
即"播放开始才卡"是**果**，不是因。

#### 5.10.2 根因：逐像素字节交换打在 write-combined 显存上

`nanovg_d3d9.h:487-507`（原实现）：

```c
memcpy(destination, source, w*4);          // 先整行写进纹理（快）
for (i = 0; i < w*4; i += 4) {
    b0 = destination[i];                   // ← 从 WC 显存里读回来！
    destination[i]   = destination[i + 3]; // ← 4 次单字节写 WC
    ...                                    //   每个非顺序字节访问都要 flush WC 缓冲
}
```

GPUENDIAN_8IN32 交换本应作用于**普通缓存内存**，实现里却对 `D3DPOOL_DEFAULT` 纹理原地做：
4800×2700 = **1296 万像素 × (2 读 + 4 写)** ≈ 实测 12895ms（≈957ns/像素）。
`stbi` 解码、`CreateTexture` 分配 51.8MB 都不是问题。

#### 5.10.3 修复与验证

改法（`nanovg_d3d9.h`，输出字节逐位不变，malloc 失败时退回原实现）：字节交换在一块 19KB 的**普通缓存 scratch** 里完成后，再整行 `memcpy` 顺序写入纹理。加 `[tex] create/fill` 分段探针。

| | 修复前 | 修复后 |
|---|---|---|
| `[tex] 4800x2700` | — | `create=13ms fill=202ms total=216ms` |
| `[Snapshot] syncTex` | 12429 / 12510 / 12895 ms | **218 ms** |
| 主线程 >2s 空白 | 12.5s | **无** |
| 用户实测 | 稳定卡 12s | 不卡 |

`create=13ms` 证明分配不是瓶颈，**`fill` 从 ~12400ms 降到 202ms（61×）** 即全部收益。

**残留（并入 #5）**：仍有亚秒级 sync 尖峰 `slow sync task 926ms from video / 691ms from main / 611ms from rcmd / 512ms from cursor`，最低帧 7~9fps 但仅一闪而过，不再是 12s 级体验问题。

⚠ 构建陷阱：`nanovg_d3d9.h` 是被 `nanovg_d3d9_backend.cpp` 编译的，里面 `extern void DbgPrint(...)` 必须包 `extern "C"`，否则 oxdklink 报 `not provided by the console … _Z8DbgPrintPKcz` 并**静默部署上一版二进制**（本轮踩过一次，导致一轮测试白跑）。

---

### 5.11 第十轮（2026-10-03，封面全不显示 / 设置卡死 / 硬崩溃 —— ✅ 根因定位：纹理缓存无上限 → 物理内存耗尽）

#### 5.11.1 现象与四次崩溃（同一根因族）

用户操作序列：**主页翻很多页 → 刷新 → 封面全部不显示 → 打开设置卡死 → 硬崩溃**（Cipher 弹 `Your game has crashed`）。

| 日志 | 异常 | 线程 | 顶部帧（符号化） |
|---|---|---|---|
| `_162108`（本轮，16:30:13） | `0xc0000005 write=0x00000000` @`0x82768a78`，两个 worker 各一次 | `0xf900003c` / `0xf90000e4` | `copyCacheLineLoop ← Curl_client_write ← Curl_readwrite ← curl_multi_perform ← curl_easy_perform ← cpr::Session::Get ← ImageHelper::requestImage ← xbox_async_thunk` |
| `_155719` | `0xc0000005 write=0x00000000` @`0x82C82794` | `0xf9000000` | `performSyncTasks ← HomeLive::onLiveList ← setDataSource ← reloadData ← … ← RecyclingGridItemLiveVideoCard::create ← View::View ← registerCommonAttributes ← registerStringXMLAttribute ← std::set 插入` |
| `_152212` | **26 次**：25× `0xc00000fd`（**栈溢出** @`0x800B2E48`）+ 1× 读 `0x8` | `0xf9000000` | 旧构建，地址无法对当前 ELF 符号化 |
| `_135821` | `0xc0000005 write=0x8` @`0x827214C8` | `0xf9000010` | 早于 #9 改动，同样早于本轮所有探针 |

崩溃点分散在**主线程 UI 构建**与**worker 线程 curl 写入**两处、异常类型混杂（写 0 / 读 8 / 栈溢出）——典型的"**内存耗尽后各处随机倒下**"，而非单一逻辑 bug。`write=0x00000000` 正是 `malloc` 拿不到内存返回 NULL 后仍被写入的形态。

#### 5.11.2 新增健康探针（用于三选一定性）

```
[x360-perf] health | stack max N KiB (sp A..B) | heap>=N KiB | tex cache N (N evictable)   每 2s 一行，堆探针每 10s
[img] decode fail: why={} bytes={} head={} heap>={} url={}     解码失败原因 + 首 16 字节 + 堆探针
[img] texture create failed {}x{} url={}                       解码成功但纹理建不出 = 显存/堆耗尽
```

- `stack max`：`performSyncTasks()` 与 `View::registerCommonAttributes()` 两处采样栈指针，取历史最深值。
- `heap>=`：阶梯试分配 64/32/16/8/4/2/1/0.5MB 后立刻释放，取能成功的最大块（⚠ 梯子最粗只到 64MB，64MB 以上的变化看不见，后续需加 256/192/128MB 桶）。
- `evictable`：引用计数已归零、可被 LRU 淘汰的条目数。

判据：`heap>=0` → 堆耗尽；`stack max` 持续增长 / 再出 `0xc00000fd` → 栈溢出；两者正常却仍 `0xc0000005` → 堆被越界写破坏。

#### 5.11.3 实测数据（决定性证据）

16:29:38 起用户在首页快速滚动，36 秒内纹理缓存 40 → 436，随后**同一时刻**出现解码失败与崩溃：

| 时刻 | tex cache | 可用堆（阶梯） | 结果 |
|---|---|---|---|
| 16:30:01 | 295 | ≥64MB | 正常 |
| 16:30:05 | 343 | ≥64MB | 正常 |
| 16:30:08 | 371 | **≥32MB**（64MB 桶已分配失败） | — |
| 16:30:10~14 | 416→436 | ≥32MB | — |
| 16:30:13.39 | ~440 | **`heap>=0`**（连 512KB 都拿不到） | `[img] decode fail: why=outofmem` ×22、`[img] texture create failed 672x378` |
| 16:30:13 | — | — | `copyCacheLineLoop` **写 0x00000000** → 崩溃 |

同期 `stack max` 全程稳定在 **18~20 KiB**（排除栈溢出），`heap` 单调下跌且与 tex cache 严格反相关 → **定性为堆耗尽**，非栈溢出、非越界写。

#### 5.11.4 根因（两个 bug 叠加）

**① 上限算法对 Xbox360 过大 —— `LRUCache::setCapacity()` 无条件 `c += DEFAULT_CAPACITY(400)`**

- 默认设置 200 → **600 张**；设置面板可选 100~500 → 最高 **900 张**；PSV/PS4 走 `setCapacity(1)` → **401 张**。
- 本平台封面用桌面尺寸 `@672w_378h`：672×378×4 = **1,016,064 B ≈ 1.02MB/张** → 600 张 ≈ **612MB > 物理内存 512MB**（实测约 440 张即耗尽，因另有 mpv/UI/curl 基础占用）。
- **PSV 为何扛得住（用户提供的参照）**：PSV 同为 512MB，但封面走 `@256w_144h` + GXM `NVG_IMAGE_DXT1/DXT5` ≈ **147KB/张**，401 张才 59MB（比本平台小 4~7 倍）；非 GXM 的 PSV-GL 走 `@224w_126h` ≈ 113KB/张。Xbox360 的 `nanovg_d3d9` **没有 DXT 上传路径**，只能靠 RGBA 原始尺寸。

**② 引用计数泄漏 → LRU 永远淘汰不掉**

`ImageHelper::load()` 与 `requestImage()` 用 `getCache()` / `addCache()` 为**新**纹理 +1，但 `Image::innerSetImage()` 覆盖旧纹理时只处理 `freeTexture` 分支（`nvgDeleteImage`），**缓存纹理分支不做 `removeCache`**：

```cpp
if (this->texture != 0 && this->freeTexture)   // 只管本地纹理
    nvgDeleteImage(vg, this->texture);
// ← 缓存纹理的 count 从不 -1
this->texture = tex;
```

而 `LRUCache::deleteCache()` **只删 `count <= 0` 的条目**（注释："If the reference count is not 0, the cache will never expire"）。于是每次滚动换图泄漏一个 count → 所有条目 `count` 恒 >0 → 即使 `size` 超过 capacity 也一条都删不掉 → **无限增长**。

旁证：15:57 轮 tex cache 涨到 436，而当时上限 600，`set()` 的 `if (size >= capacity)` 分支**根本没触发过**——上限与泄漏两个 bug 只要有一个存在就足以致命。

#### 5.11.5 修复（三处，均为小改动）

1. **`cache_helper.hpp::setCapacity`**：Xbox360 分支不叠加 `+400`，并硬限制 **200 张**（≈204MB，留 ≥200MB 给 mpv / 快照的 4800×2700 纹理 51.8MB×2）。其它平台（含 PSV/PS4 的 `setCapacity(1)` → 401）行为不变。
2. **`image.cpp::innerSetImage`**：旧纹理走 TextureCache 时补 `TextureCache::instance().removeCache(this->texture)`，维持"**视图对 `this->texture` 恰好持有 1 个引用**"的不变式（所有调用者——`checkCache`/`load`/`requestImage`/`setImageFromFile`/`setImageFromRes`——都已为新纹理取过引用）。
3. **`image.cpp::checkCache`**：删掉它自己那次 `removeCache`，否则与 #2 **双重释放** → `size_t count` 下溢成巨大值 → 同样永远淘汰不掉；顺带修掉"缓存未命中时旧图引用被提前释放、但仍被显示"的既有不一致。

探针补充：health 行加 `(N evictable)` —— 若 `tex cache` 顶到 200 而 `evictable` 恒为 0，说明仍有未发现的泄漏路径。

#### 5.11.6 状态与验收

- **待复现验证**：`tex cache` 应稳定 ≤200 且 `evictable > 0`，`heap>=` 不再跌到 0，滚动→刷新→设置→直播页全流程无崩溃、封面正常。
- 若验收后仍想缓存更多封面，走 PSV 路线：把 Xbox360 的 `h_ext`/`v_ext` 降到 `@448w_252h`（448KB/张，200 张 = 90MB）或给 `nanovg_d3d9` 加 DXT 上传（当前无此能力）。
- **独立现象待查**：直播页封面走 `https://` 时出现 `err=10`（`SSL_CONNECT_ERROR`，`cancel=false` 真失败），与主页 `http://` 封面不同；本轮无暇展开。

---

## 6. 待决策问题

1. ~~**是否先从 #1 测量做起？**~~ **已执行**（2026-10-02）：埋点与实测数据见 §5，
   后续修复项的预期收益已按数据重新排序（sync 冻结 > TLS 占比 > 闸门排队）。
2. **keep-alive（HTTP/1.1 连接复用）要不要恢复？**
   当前 HTTP/1.0 + `Connection: close` 是为绕开"服务端省略 content-length 时 curl fork
   一直等 keep-alive EOF"的 bug 而做的降级。恢复需要修 curl fork 的响应终结判断，
   建议单独立项；本轮先只做 DNS/SSL session 共享（#3），不动 HTTP 版本。
3. **#13 之后残留的 20ms 量化慢分配要不要继续追？**（§5.9.3：1789 次、`cs` 仍是
   19/20/39/40ms 整数倍，合计 ~45s/90s）。先加 `fb=` 计数器区分
   "自旋 64 次落空进 `Sleep(0)`"（解法：自旋预算提到数千次）与
   "拿锁前就被调度器换下"（解法：按线程分片本地缓存，稳态不碰全局锁）。
   当前用户实测已无感，属**健壮性**而非体验问题——但并发再上去（8~9 路）仍可能复发。

---

## 7. 关键文件索引

| 主题 | 路径 |
|---|---|
| HTTP 会话创建 / Xbox 降级开关 | `wiliwili/include/api/bilibili/util/http.hpp` |
| Xbox 线程闸门（每请求一线程） | `library/cpr/include/cpr/async.h` |
| GetCallback 二次 async | `library/cpr/include/cpr/session.h:272` |
| DNS 全局锁 + 槽位环 | `third_party/curl/lib/xbox360_stubs.c` |
| mbedTLS 握手循环 DbgPrint | `third_party/curl/lib/vtls/mbedtls.c:377` |
| 连接建立 DbgPrint | `third_party/curl/lib/connect.c:1097,1115` |
| WBI key 刷新链 | `wiliwili/source/api/util/wbi.cpp` |
| sync 任务倾倒点 | `library/borealis/library/lib/core/application.cpp:206` |
| 图片纹理逐像素 swap（✅ #9 已改为 scratch 内交换 + 顺序 memcpy，并加 `[tex]` 分段探针，§5.10） | `library/borealis/library/lib/extern/nanovg_d3d/src/nanovg_d3d9.h:487-507` |
| 快照大图下载/解码/贴图 | `wiliwili/source/view/video_snapshot_core.cpp`、`wiliwili/source/presenter/video_detail.cpp:385,432`（请求与 playurl 同批过闸门） |
| 弹幕加载排序 | `wiliwili/source/view/danmaku_core.cpp:302` |
| 日志自旋锁 + stdout | `library/borealis/library/include/borealis/core/logger.hpp:107-138` |
| Xbox 自旋锁实现 | `library/borealis/library/include/borealis/core/xbox360_mutex.hpp` |
| 帧耗时埋点 | `library/borealis/library/lib/platforms/xbox360/xbox360_perf.cpp` |
| 每请求网络计时埋点 `[x360-net]` | `wiliwili/include/api/bilibili/util/http.hpp`（`RequestLog`） |
| 闸门排队埋点 `[x360-queue]` | `library/cpr/include/cpr/async.h`（`xbox_async_thunk`） |
| sync 回调归类埋点 `sync split` | `thread.cpp::performSyncTasks`、`image_helper.cpp`、`danmaku_core.cpp`（`SyncScope`） |
| 图片请求线程池 | `wiliwili/source/utils/image_helper.cpp:101-130` |
| TLS 状态归因循环 / 线程 CPU 时间 / bio io 偏移（五轮） | `third_party/curl/lib/vtls/mbedtls.c`（`mbedtls_ssl_handshake_step` 自绘循环、`GetThreadTimes`、`xbox_bio_*`） |
| ECDH `ecp mul` 计时（五轮） | `third_party/mbedtls/library/ecp.c`（`mbedtls_ecp_mul_restartable` 内 `x360_ecp_ms`） |
| 堆分配阻塞探针 + 分解探针 `cs/hold/zero/q`（五~八轮） | `third_party/mbedtls/library/platform.c`（`mbedtls_calloc`/`mbedtls_free` ≥10ms 打印；池内再拆等锁/持锁/清零/时钟） |
| **私有池 + 自旋锁（#11/#13，本轮根因）** | `third_party/mbedtls/library/platform.c`（`x360_pool_*` arena/分档/free 链、`x360_pool_lock()` 紧自旋 + `Sleep(0)`、`x360_mbedtls_pool_init()` 注入并打印 `fn==pool` 自证） |
| mbedtls 分配器注入点（#11） | `wiliwili/source/utils/config_helper.cpp`（Xbox 分支 `curl_global_init` 前调用 `x360_mbedtls_pool_init()`） |
| WBI 时间源（#7） | `wiliwili/source/api/util/wbi.cpp`（`wbiNow()` = `std::chrono::system_clock`，用于 TTL 判断与 `wts`） |
| ECP 算法配置（#11） | `third_party/mbedtls/include/mbedtls/config-xbox360.h`（已启用 `MBEDTLS_ECP_NIST_OPTIM`，仅 secp256/384/521） |
