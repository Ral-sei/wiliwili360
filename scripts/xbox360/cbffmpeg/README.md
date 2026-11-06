# Xbox 360 cbffmpeg poll() 补丁

`x360-poll-select.patch` 针对 cbffmpeg 的 mpv compat 层
（`$CBFFMPEG_DIR/compat/mpv/`）。cbffmpeg 不在本仓库内、也没有版本管理，
重装或换机器后需要重新应用本补丁。

应用方式（在 cbffmpeg 根目录）：

```sh
patch -p1 < scripts/xbox360/cbffmpeg/x360-poll-select.patch
```

然后重新打包 mpv 的 compat 归档（wiliwili 链接的就是这个文件）：

```sh
scripts/mpv_x360_compat_archive.sh
```

## 改动

- `compat/mpv/x360_mpv_posix.c`：`poll()` 从"先睡满 timeout、再把请求的
  事件一律报 ready"的桩，改成 Winsock `select()` 的真实可读/可写等待
  （socket fd）；非 socket CRT fd、超过 `FD_SETSIZE` 的集合仍回退旧行为。
- `compat/mpv/include/poll.h`：同步注释。

## 原因

FFmpeg 的 `tcp_read()` 在**每次 `recv()` 之前**都调用
`ff_network_wait_fd()`，超时参数是 `POLLING_TIME = 100 ms`
（`libavformat/network.c:74`、`network.h:249`）。旧桩让每个 TLS record
都固定多等 100 ms，播放读流速度被钉死在 `(单次 record 字节数) / 0.1 s`：
真机实测 26.49 KB/s、缓存填不满、频繁缓冲。改成真等待后同样内容实测
1.6–2.9 MB/s，缓存 40 s 以上。详见
`docs/XBOX360_NETWORK_PERFORMANCE_REVIEW.md` §5.13。

## 验证

补丁应用在 cbffmpeg 的原始 `compat/mpv/` 文件上后，结果与真机验证过的
当前文件逐字节一致（生成时做过 round-trip 校验）。真机（devkit 17559）
播放/直播验证见 §5.13、§5.15。
