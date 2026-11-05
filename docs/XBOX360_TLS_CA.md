# Xbox 360 TLS 证书校验（CA bundle）

> 状态：Phase 1（curl）2026-10-26 落地；**Phase 2（FFmpeg/mpv 播放链路）2026-11-04 落地并通过
> devkit 实机验收**。验收中顺带定位并修复了「CRT 时间函数返回垃圾」的平台级 bug
> （见 §7）——不修它，任何开启校验的构建都无法完成一次握手。
>
> 相关：`XBOX360_PORTING_PLAN.md` M4、`XBOX360_NETWORK_PERFORMANCE_REVIEW.md`。

## 1. 背景

Xbox 360 分支此前强制 `BILI::setTlsVerify(false)`，原因不是 mbedTLS 缺能力，而是**没有 CA 信任链来源**：

- `mbedtls/config-xbox360.h` 关闭了 `MBEDTLS_FS_IO`，`mbedtls_x509_crt_parse_file()` 不可用；
- `third_party/curl/lib/vtls/mbedtls.c` 只有在设置了 `CURLOPT_CAINFO` 时才挂 CA 链，没人设置 → `verifypeer=1` 会直接以 `CA_CHAIN_REQUIRED` 失败；
- `third_party/ffmpeg-4.4.8/libavformat/tls_mbedtls.c` 在无 `MBEDTLS_FS_IO` 时把 authmode 写死为 `VERIFY_NONE`。

零售机形态是单个 XEX，无法像 devkit 那样在 `game:\resources\` 里塞一个 PEM，因此方案是**把 bundle 编译进 XEX**。

## 2. 结构

```text
third_party/ca/cacert.pem                  Mozilla bundle（固定 SHA256）
        │  scripts/gen_xbox360_ca_bundle.py（构建期生成，校验 SHA256）
        ▼
build-*/xbox360_ca_bundle.c                const unsigned char x360_ca_bundle_pem[]
        │
        ▼
wiliwili/source/platform/xbox360/xbox360_ca.c
        │  解析一次 → 进程级 mbedtls_x509_crt 链（永不释放）
        ├──────────────► curl  (vtls/mbedtls.c)
        └──────────────► FFmpeg (tls_mbedtls.c，Phase 2 已接通)
```

两个关键约束：

1. **必须作为可执行文件的 object**，不能放进静态库。`APP_PLATFORM_LIB` 顺序是
   `… → mbedtls → curl`，`oxdklink` 单遍扫描且没有 `--start-group`：放前面的归档 curl 看不见，
   放后面的归档拿不到 mbedtls 符号。所以 `xbox360_ca.c` 和生成的 bundle 都加进 `MAIN_SRC`。
2. **解析一次**。Xbox 强制 `Connection: close`，如果按连接解析整包（136 张根）会非常慢。
   现在在 `ProgramConfig::init()` 里、`x360_mbedtls_pool_init()` 之后解析一次，
   curl/FFmpeg 共用同一个只读链。

## 3. 行为

| 项 | 行为 |
|---|---|
| 默认校验 | `SettingItem::TLS_VERIFY` 默认 1（此前被强制关闭） |
| 证书链 | 内嵌 bundle |
| 主机名 | mbedTLS `VERIFY_REQUIRED` + curl 已有的 `mbedtls_ssl_set_hostname()` 自动做 CN/SAN 匹配 |
| 有效期 | `MBEDTLS_HAVE_TIME_DATE` + `mbedtls_platform_gmtime_r()` 的 Xbox 实现 |
| 时钟异常 | 系统时间早于 2024-01-01 时打 warning，并**本次会话降级为不校验**（主机时钟不可被远程篡改，属可用性兜底） |
| 缺链 | `verifypeer` 但链为空 → `CURLE_SSL_CACERT_BADFILE`（fail-closed） |
| 应急覆盖 | `game:\config\wiliwili\cacert.pem` 存在时**追加**到内嵌链（不替换） |

启动日志：

```text
[x360-tls] ca=<n> skipped=<n> parse=<n>ms clock=<unix>
```

`skipped` 是 bundle 里 mbedTLS 2.28 无法解析的证书数（Ed25519 根等）。
`mbedtls_x509_crt_parse()` 对这种输入返回**正数**（跳过的张数），不是失败。

## 4. 更新 bundle

1. 从 <https://curl.se/ca/cacert.pem> 取新文件；
2. 更新 `third_party/ca/README.md` 的日期/SHA256/张数；
3. 更新 `CMakeLists.txt` 里的 `XBOX360_CA_SHA256`；
4. 重新构建。脚本会在 SHA256 不匹配时直接失败。

根证书过期但无法重新出包时，把新的 PEM 放到 `game:\config\wiliwili\cacert.pem` 即可。

## 5. Phase 2（2026-11-04）：FFmpeg/mpv 播放链路

播放链路此前完全不校验：`libavformat/tls_mbedtls.c` 在无 `MBEDTLS_FS_IO` 时把
authmode 写死 `VERIFY_NONE`，设置页的「TLS 验证」开关只管 curl。现在两条链路
共用一个决策：

| 件 | 改动 |
|---|---|
| `xbox360_ca.h/.c` | 新增 `x360_tls_set_verify()/x360_tls_verify()`：设置项 + 时钟降级后的**单一有效开关**，默认关 |
| `config_helper.cpp` | init 与设置项切换两处把有效决策推给该开关；切换路径同样带时钟守卫（降级粘性） |
| `third_party/ffmpeg-4.4.8/libavformat/tls_mbedtls.c` | `_XBOX` 分支：authmode 跟随 `x360_tls_verify()`，挂 `x360_ca_chain()` 全局链，链缺失 fail-closed，握手后复核 `verify_result`；`shr->verify`（mpv 侧）不再是开关来源 |
| `wiliwili/source/view/mpv_core.cpp` | mpv `tls-verify` 镜像同一决策（顺带让 mpv 侧日志/属性一致） |

`tls_mbedtls.c` 同步到了 `cbffmpeg/ffmpeg-4.4.8` 同源副本。主机名校验沿用
`mbedtls_ssl_set_hostname()`（CN/SAN 匹配）；证书链来自内嵌 bundle，与 curl 同一条。

## 6. 验证

已完成（本机 WSL / Xenon）：

- `scripts/build_xbox360_local.sh` 全量配置 + 编译 + 链接通过；
- 164 objects，XEX `29,114,368` B（原 28,917,760 B，+192 KB），镜像 `0x1bc0000`，
  md5 `F36237499825B70AC2E00E0C27291AA6`；
- `llvm-nm`：curl 对象引用 `U x360_ca_chain`，CA 对象定义 `T x360_ca_chain`/`x360_ca_init`/`x360_tls_clock_ok`，bundle 对象 `R x360_ca_bundle_pem`（0x30DB5 = 200,117 B）；
- 118 个 PEM 块全部可解码为 DER；生成长度 = PEM + 1（含 NUL）。

**主机侧真实链路验证**（同一份 vendored mbedTLS 2.28 源码 + 固定 bundle，
`mbedtls_x509_crt_verify()` 对服务器实际发送的证书链）：

| 主机 | 结果 |
|---|---|
| `api.bilibili.com` | `flags=0x00000000` |
| `api.live.bilibili.com` | `flags=0x00000000` |
| `i0.hdslb.com` | `flags=0x00000000` |
| `passport.bilibili.com` | `flags=0x00000000` |
| `upos-sz-mirrorali.bilivideo.com` | `flags=0x00000000` |
| `upos-sz-mirrorcos.bilivideo.com` | `flags=0x00000000` |
| `api.bilibili.com` + 错误主机名 | `0x00000004`（CN mismatch），校验失败 |

说明 bundle 覆盖 API、直播 API、图片、登录和视频 CDN 五类链路，且主机名校验确实生效。

devkit 实机验收（2026-11-04，XEX 镜像 `0x1430000`，配置 `tls_verify: true`；日志
`logs/xbdm_tls_p2_run*.txt`，截图 `logs/tls_p2_*.png`）：

| 用例 | 结果 |
|---|---|
| 启动日志 | ✅ `[x360-tls] ca=118 skipped=0 parse=26ms clock=1793784160` |
| 首页（默认校验开） | ✅ WBI + feed 均 200，卡片/封面正常 |
| 图片墙 | ✅ `hdslb.com` 缩略图、头像、视频预览图全部经验证 TLS 加载 |
| **播放链路（Phase 2 核心）** | ✅ `cplayer: Playing: https://upos-…bilivideo.com/….m4s`，1080p 播放 avsync 0.007s、cache 30s、`decoder 0` 丢帧；FFmpeg TLS 挂链+校验生效 |
| 校验确实在拦截（负向） | ✅ 同机同配置跑未修时钟的旧包：全部握手 `-0x2700 CERT_VERIFY_FAILED`，首页「WBI签名获取失败」——校验失败即拒绝，不静默放行 |
| 二维码登录 | ⏳ 未登出用户会话故未进登录页；主机侧已证 `passport.bilibili.com flags=0` |
| 设置页关闭 TLS（逃生口） | ⚠️ 被既有设置页冻结阻塞（见 §8）；init 路径两态（true/false）已验证 |
| 时钟改到 2005 → 降级 | ⏳ 见 §8（改时钟通道受限，待补） |
| devkit + 零售机 | devkit ✅ / 零售机 ⏳ |

## 7. CRT 时间函数返回垃圾（2026-11-04 实机定位，必须记住）

**症状**：Phase 2 首次实机验收时，开启校验后所有 TLS 握手失败：
`mbedtls_ssl_handshake returned -0x2700`（`MBEDTLS_ERR_X509_CERT_VERIFY_FAILED`），
首页显示「WBI签名获取失败」。启动日志的 `clock=` 字段是怪值
`7704238758480052224`。

**根因**：本 XDK 的 CRT `time()/_time64()` 在真机上返回
**`unix_seconds << 32`**（怪值高 32 位 = `1793782869`，正是当时的 Unix 时间；
低 32 位为 0）。这是大端机上 32 位值写入 64 位槽的典型症状。cbffmpeg
`compat/xbox360/posix_time.c` 头注早已记录同一现象（实测 `7690104669648977952`），
并明确决定**不覆盖 `_time64`**（CRT 内部调用链走 MSVC ABI，替换会误伤）。

mbedTLS 的证书有效期校验（`x509.c`）和会话时间戳（`ssl_cli.c`）走
`mbedtls_time(NULL)` → 默认就是 `time()` → 假时间 → 所有证书"过期"。
`x360_tls_clock_ok()` 原先也读 `time()`：假值恒 ≥ 2024，**下界检查永远不会触发
降级**，连"时钟未同步（2005）→ 本次会话降级"的可用性兜底都是坏的。

**修复**（`wiliwili/source/platform/xbox360/xbox360_ca.c` + mbedtls config）：

- `x360_unix_seconds()`：唯一可信墙钟，`KeQuerySystemTime`（内核 RTC）FILETIME
  → Unix 秒（常量同 `platform_gmtime_xbox360.c`）；
- `config-xbox360.h` 开 `MBEDTLS_PLATFORM_TIME_ALT`，`x360_ca_init()` 里
  `mbedtls_platform_set_time(x360_mbedtls_time)` 装钩子——curl 和 FFmpeg 两个
  TLS 栈的 x509/ssl 时间全部走正确时钟；
- `x360_tls_clock_ok()` 与 `[x360-tls] ... clock=` 统计改用 `x360_unix_seconds()`。

注意：**没有**去修 CRT 的 `_time64`（遵守 cbffmpeg 的既有结论）；进程里其他
`_time64` 使用者（curl cookie 过期、libxml2 等）仍是旧行为，属遗留事项。

## 8. 尚未覆盖

- **零售机实机验收**：devkit 已过；零售机按惯例必须复验（SNL 等差异教训）。
- **二维码登录（passport.bilibili.com）实机**：验证机已登录（保留用户会话，
  未登出）；主机侧同一 bundle+mbedTLS 已证 `flags=0`，握手机制与已验收的
  api/CDN 完全相同。
- **时钟改到 2005 → 会话降级**：修复后逻辑正确（`x360_tls_clock_ok()` 读真实时钟），
  但改控制台时钟的通道受限：XBDM `setsystime` 任意状态（含 `stop` 后）都返回 406，
  COM 自动化（`IXboxConsole.put_SystemTime`）本机未注册，`xbdm.dll` 无 `DmSetSystemTime`。
  逻辑等价路径已在 init 降级分支覆盖；如需实测可从控制台仪表板手动改时钟。
- **设置页开关（逃生口）实机**：被**既有**的设置页冻结问题阻塞——2026-11-04
  A/B 对照（同树旧包 `wiliwili_dbg2.xex` vs 新包）证明两个包都在
  `Intent::openSetting()` 后瞬间挂死，与 TLS 改动无关。开关逻辑本身经
  init 路径（配置 `tls_verify` 真/假两态）验证；UI 切换路径留待冻结修复后补测。
- 直播弹幕走 `ws://`（明文），不在本方案范围内。
