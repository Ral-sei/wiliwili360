# Xbox 360 FFmpeg 补丁

`ffmpeg.patch` 针对 FFmpeg 4.4.8（`third_party/ffmpeg-4.4.8/`，该目录被
`.gitignore` 的 `ffmpeg-*` 规则忽略，重解压后需重新应用本补丁）。

应用方式（在解压后的 FFmpeg 源码树根目录）：

```sh
patch -p1 < scripts/xbox360/ffmpeg/ffmpeg.patch
```

包含三处改动（`libavformat/`）：

- `tcp.c`：`customize_fd()` 为每个新建 TCP socket 设置
  `setsockopt(fd, SOL_SOCKET, 0x5801, ...)`（SNL "unencrypted" 逐 socket
  退出标记）。零售机 SNL secure 模式会静默丢弃未标记 socket 的回包，
  `XNET_STARTUP_BYPASS_SECURITY` 仅 devkit 生效。**只设 0x5801；不要加
  0x5802**（devkit 上会导致 connect() 立即 WSAEHOSTUNREACH 10065）。
  详见 `docs/XBOX360_PORTING_PLAN.md` §10（2026-10-11）。
- `os_support.c`：`ff_getaddrinfo()` 用显式的 `xbox_hostent` 布局读取
  XDK hostent，避免头文件组合差异导致字段偏移错位。
- `tls_mbedtls.c`：
  - 握手前后切换 TCP 阻塞/非阻塞模式（XNet 会在 TLS 记录到达前报告
    可读，导致 mbedTLS 反复 WANT_READ）；
  - `mbedtls_ctr_drbg_seed()` 移出 `MBEDTLS_FS_IO` 条件块（Xbox 关闭
    文件系统证书加载时仍需初始化 CTR-DRBG，否则访问违规崩溃）；
  - 证书/密钥文件加载路径用 `MBEDTLS_FS_IO` 包裹并在缺失时报错。

基线：`H:/cbffmpeg/ffmpeg-4.4.8`（pristine 副本）。生成/校验脚本见
`docs/XBOX360_PORTING_PLAN.md` §10。
