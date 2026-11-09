# Xbox 360 运行时开关（持久化）

> 状态：持续维护。本文件收录**值得长期保留**的运行时开关（不重编即可 A/B）。
> 一次性实验开关不在此列——那些用完即删，不进提交。

## 发布档位（出厂默认，无需配置文件）

发布版**不附带** `x360_play.cfg`：下面的档位已经写进代码默认值，开箱即是真机实测
最优组合。`x360_play.cfg` 只是高级用户的可选覆盖，删掉即回到出厂档位。

播放器按内容自动分档（`player_base_activity.cpp`：DASH 流 `height >= 1080` 或
FLV `quality >= 80` 进 CATCHUP 档，其余进全质量档），不需要用户干预。

### CATCHUP 档（1080p 内容）

| 开关 | 值 | 说明 |
|---|---|---|
| `skipframe` | `nonref` | 全帧 1080p 只有 ~19fps，丢非参考帧保音画同步 |
| `skiploop` | `all` | CATCHUP 档跳过去块（+20% 吞吐）；全质量档不跳 |
| `vd_threads` | `4` | 6 worker 会挤占主核，UI build 8ms→24–27ms |
| `spread` | `2,4,3,5` | worker 避开 hw0 主核 |
| `vmx` | `1` | VMX128 内核比纯 C 快 20–45% |
| `prof` | `0` | 剖析关闭 |
| `audio_buffer` | `1.0` | 吸收启动突发，防欠载 |

### 全质量档（720p 及以下）

| 开关 | 值 | 说明 |
|---|---|---|
| `skipframe` | `no` | 720p 全帧可达 30fps，无需丢帧 |
| `skiploop` | 不设置（全去块） | deblock 已是 VMX128 内核，720p 扛得住 |
| `vd_threads` | `6` | 全帧档沿用历史默认 |
| 其余同上 | | |

### 清晰度策略

- 点播默认 **720P**（`WILI_VIDEO_QUALITY_DEFAULT 64`）；直播默认 **超清 720P**（qn 250）。
- 1080P **不限制但不推荐**：菜单里可手动选择，播放时自动进 CATCHUP 档（只保证音画同步）。
- 上限封顶 **1080P60（116，`WILI_VIDEO_QUALITY_*_MAX`）**：4K/8K 既不请求也不在菜单中出现。

## 1. `game:\x360_play.cfg`（wiliwili 播放器）

文件放在 XEX 同目录（`game:\` = XEX 所在目录；DEVKIT 部署时为 `DEVKIT:\wiliwili\`
或启动时所在目录）。每行一条 `键=值`，`#` 开头为注释。解析代码：
`wiliwili/source/view/mpv_core.cpp`（`MPVCore::init()` 早期读取）。

| 键 | 值 | 作用 | 默认 |
|---|---|---|---|
| `vd_threads=` | 1..16 | 解码器帧线程数（`vd-lavc-threads`）。CATCHUP 档内置 4，非 CATCHUP 内置 6 | 依画质档 |
| `spread=` | 0..5 列表，最多 6 项，逗号分隔 | lavc worker 绑核表（`xbox360_pthread_set_spread_cpus`）。示例 `spread=2,4,3,5` | CATCHUP: 2,4,3,5；否则 shim 六项表 {1,3,5,2,0,4} |
| `vmx=` | 0/1 | VMX128 H.264 内核总开关（chroma MC / qpel / weight / IDCT）。**解码器 init 时读取，重启生效** | 1 |
| `prof=` | 0/1 | H.264 CABAC/重建周期计数剖析。开启后周期性打 `[x360-h264prof]` 日志（见 §3） | 0 |
| `skipframe=` | no\|bidir\|nonref\|all | `vd-lavc-skipframe` 覆盖（CATCHUP 丢帧策略）。**mpv init 时生效**；优先于按画质的自动门控 | 依画质档 |
| `skiploop=` | nonref\|all\|… | `vd-lavc-skiploopfilter` 覆盖 | 依画质档 |
| `audio_buffer=` | 秒 | AO 设备环大小（`audio-buffer`）。1.0 吸收启动突发防欠载 | 1.0 |
| `mpvopt=` | `键=值` | 任意 mpv **初始化前**选项透传，如 `mpvopt=video-sync=audio` | — |
| `avopts=` | `键=值[,键=值…]` | 透传到 `vd-lavc-o`（libavcodec AVOptions），如 `avopts=thread_type=slice` | — |

注意：

- `skipframe`/`skiploop`/`audio_buffer`/`mpvopt`/`avopts` 均须在 `mpv_initialize()`
  之前设置才生效（运行时 set 属性解码器不采用，实测见性能计划 §15.9）。
- `vmx` 在解码器 init 时读取，切换后需重启应用。
- `vmx` 之下的单内核粒度开关是**构建期**宏 `X360_CHROMA_MASK`（chroma MC
  按位注册：1=put8/2=put4/4=avg8/8=avg4，默认 15），重新
  configure/编译 FFmpeg 生效，见 `scripts/ffmpeg_x360_configure_local.sh`。
- 所有覆盖项在日志打 `[x360-play] … (override)`。

### 示例

```
# 1080p CATCHUP 调优
vd_threads=4
spread=2,4,3,5
skipframe=nonref
skiploop=all
audio_buffer=1.0
vmx=1
```

## 2. `game:\p2cfg.txt`（p2 解码 harness，`cbffmpeg/tests/p2_decode*`）

每行 `命令 参数 线程数 绑核模式`（4 字段；后两个字段对开关类命令借用为值）。

| 命令 | 说明 |
|---|---|
| `video <文件> <线程> <绑核>` | 解码 `game:\media\<文件>`，校验帧数并打 fps |
| `audio <文件> <线程> <绑核>` | AAC 解码测试 |
| `skiplf x <0/1> x` | skip_loop_filter 开关 |
| `vmx x <0/1> x` | VMX128 内核开关（同播放器 `vmx=`） |
| `prof x <0/1> x` | H.264 剖析开关（同播放器 `prof=`） |

绑核模式：`>=0` 钉到指定硬件线程；`-1` float（不钉）；`-2` spread（绑核表轮转）。

## 3. `[x360-h264prof]` 剖析输出

`prof=1` 时 libavcodec 的 H.264 解码器按宏块累积周期桶（mftb 时基），
每约 10 帧 1080p 的宏块数由各 worker 线程打一行：

```
[x360-h264prof] id=<sl指针低12位> mb=<累计宏块> base=<窗口总周期>
                cabac=N% pred=N% mc=N% idct=N% other=N% await=N%
```

| 桶 | 范围 |
|---|---|
| `cabac` | `ff_h264_decode_mb_cabac/cavlc`：熵解码 + MB 解析 + cache |
| `pred` | 帧内重建（pred8x8/4x4/8x8l/16x16 + intra4x4 融合 add） |
| `mc` | 运动补偿（qpel/chroma/weight 内核） |
| `await` | `await_references` 等参考行的等待（`mc` 的子集） |
| `mcd` | `mc_dir_part`：qpel/chroma 内核 + 边界仿真（`mc` 的子集） |
| `wt` | 加权预测胶水：luma/chroma weight-op/avg（`mc` 的子集） |
| `idct` | 残差重建（idct_luma + chroma idct） |
| `other` | MB 循环簿记（terminate、行尾、滤波调用等） |

占比以 `base`（MB 循环总周期）为分母。实现：`libavcodec/h264dec.h`（`x360_prof`）、
`h264_slice.c`（计时/输出）、`h264_mb.c`/`h264_mb_template.c`（桶划分）。
关闭时仅每个测量点一次可预测分支，无读时基开销。
