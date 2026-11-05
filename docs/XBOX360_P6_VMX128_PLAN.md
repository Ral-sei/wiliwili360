# P6：H.264 VMX128 内核实施计划

> 状态：启动（2026-11-01）
> 前置：`cbffmpeg/docs/p6_altivec_preflight.md`（Xenon 缺失 22 条整数乘加指令，
> FP 改写路线已在真机 50000 次 bit-exact 验证）
> 参考：`H:\Xbox360MediaPlayer.Alpha10-ZorgX\_analysis\REPORT_zh.md`（逆向得到的
> 设计思路，不复制其二进制/汇编；本计划按 FFmpeg 4.4.8 C 参考自行实现）

## 1. 目标与边界

- 把 H.264 解码最热的 DSP 内核替换为 VMX128 内核，降低解码 CPU 占用。
- 当前实测（1080p30 / 720p60）：`decoder 丢帧 0`、`est-vf 30` 曾被当作“解码跟得上”
  的证据，**2026-11-02 更正**：二者都不代表解码吞吐。用 avsync 斜率反推，1080p30
  解码吞吐只有 **15~24 fps**（3~6 线程，见
  `XBOX360_PLAYBACK_PERFORMANCE_PLAN.md` §15.9），画面靠 CATCHUP
  （`vd-lavc-skipframe=nonref`）丢非参考帧才能与音频同步。VMX128 内核的目标仍是
  降低解码 CPU 占用，但现有 chroma/qpel/IDCT 内核只带来 ~6% 吞吐提升。
- 不改 FFmpeg 大版本（保持 4.4.8）；不改 mpv 架构。

## 2. 内核范围（按优先级）

| 阶段 | 内核 | FFmpeg 位置 | 备注 |
|---|---|---|---|
| M1 | chroma MC 8x8 / 4x4 put+avg | `h264chroma_template.c` | 每个 P/B 帧都用；上游 ppc 版因 `vec_mladd` 不可用 |
| M2 | qpel h/v/hv lowpass 16/8/4 | `h264qpel_template.c` | 上游 `h264qpel.c` 三个 lowpass 全部命中 drop 指令 |
| M3 | weight / biweight | `h264dsp.c` | 上游 689/750 行 `vec_mladd` |
| M4（可选） | IDCT / deblock | `h264dsp.c` | 上游这部分指令合法，可先直接复用/后接自写 |

已知可用的安全子集：`h264dsp.c` 的 IDCT/deblock/put/avg、`hpeldsp_altivec`、
`pixblockdsp`、`float_dsp` 不含 drop 指令；可作为 M0 低风险试验（需在 init 里
只选安全函数，跳过 chroma/qpel/weight）。

## 3. 实现约束

- 只用预检验证过的合法指令：`vperm/vsldoi/vpkuhus/vadds/vavgub/vadduwm/
  vmaddfp/vaddfp/vsubfp/vec_unpack*/vec_ctf(vcfsx)/vec_cts(vctsxs)/vec_pack/
  vec_packs/vec_sub(s)/vec_and/or/xor/andc/vec_sel/vec_cmpeq/vec_max/min/
  vec_sra/srl/splat/mergeh/lvx/stvx`。
- 整数乘加统一走 FP 桥：
  `vmaddfp(vec_ctf(a), vec_ctf(b), vec_ctf(c))`，要求 `|a*b+c| < 2^24`
  （chroma 4-tap ≤65280、qpel 6-tap ≤7650，已核算）。
  改写时必须显式补回 C 语义的**截断/舍入**（`+32 >> 6`、`+1 >> 1` 等）。
- 编译：仅新文件加 `-maltivec -fno-vectorize -fno-slp-vectorize`，
  防止 clang 自动向量化引入 drop 指令；其余平移编译。
- 每族配 **selftest**：与 FFmpeg 的 C 参考在真机跑随机 + 边界用例，
  bit-exact 才启用；失配自动回退 C，并打日志。

## 4. 落地位置

- 源码：`third_party/ffmpeg-4.4.8/libavcodec/xbox360/h264_vmx.{c,h}`
  （`cbffmpeg/ffmpeg-4.4.8` 为同源副本，改动两边同步）。
- 构建：`libavcodec/Makefile` 增加对象（`CONFIG_H264_DECODER` 下），
  单文件 CFLAGS 注入 `-maltivec`；重建 `build/ffmpeg-x360-tls` 的
  `libavcodec.a` 后重链 wiliwili。
- 接入：`ff_h264chroma_init()` / `ff_h264qpel_init()` / `ff_h264dsp_init()`
  在 `_XBOX` 且 selftest 通过时覆盖函数指针；否则保持 C。
- 独立验证：`cbffmpeg/tests/p6_h264/`（仿 `tests/vectest` 的 XEX 脚手架），
  先不依赖 FFmpeg，只跑内核 + C 参考对拍；通过后再接 init。

## 5. 验收

- 每个 selftest：≥50000 次随机 + 全部 x/y 权重组合（0..7）bit-exact，0 失配。
- 真机无非法指令异常；`[x360-mpv] decoder` 丢帧保持 0。
- 性能 A/B：固定同一段 720p60 与 1080p30，比较
  `[x360-perf] frame`（UI fps、build p50/p95）、`[x360-mpv]`（est-vf/drops/avsync）、
  `Audio device underrun` 计数；目标：UI build 与欠载显著下降。
- 回退开关：cfg `vmx=0`（或构建宏）可一键回到 C 参考。

## 6. 里程碑

1. **M1a**：`tests/p6_h264` 脚手架 + chroma MC 8x8 put selftest。
2. **M1b**：补 avg / 4x4 / 全部权重，接入 `ff_h264chroma_init`。
3. **M2**：qpel h/v/hv lowpass 16/8/4 + 接入。
4. **M3**：weight/biweight + 接入。
5. **M4**：真机性能 A/B，决定是否继续 IDCT/deblock。

## 7. 风险

- VMX128 缺整数乘法，FP 桥的中间和必须严格核算；超过 2^24 会失配。
- `vec_unpackh` 对 u8 是符号扩展，需 `& 0x00FF` 清零高半字。
- 新文件进入 FFmpeg 构建需要改 Makefile；构建系统不记录头文件依赖，
  改头文件后需 touch 相关 TU 或 clean 重建。
- 参考项目是逆向结果，禁止直接复制其汇编；只借鉴设计（selftest + 回退、
  FP 桥、按位深安装函数表）。

## 8. 实机进展（2026-11-01）

### 已完成

| 阶段 | 内容 | 验证 |
|---|---|---|
| M1 | chroma MC put/avg × 宽 4/8 | `tests/p6_h264` 真机对拍 1,382,020 次 0 失配；运行时自检通过 |
| M2 | qpel 16 亚像素位置 × 宽 4/8/16 × put/avg（96 个入口） | 真机对拍 3,661,140 次 0 失配；运行时自检通过 |

接入：`libavcodec/xbox360/h264_vmx.c`（chroma）、`h264_qpel_vmx.c`（qpel），
Makefile 单文件 `-maltivec -fno-vectorize -fno-slp-vectorize`，
init 处先用 FFmpeg 已装的 C 函数做自检，失配自动回退并打 WARNING。
运行时开关：`game:\x360_play.cfg` 的 `vmx=0`（重启生效，不用重编）。

### Xenon 上必须记住的三个坑

1. **`vec_ld` 是对齐语义**（clang 与 GCC 不同，不做字节移位）。非对齐指针会
   静默读到对齐窗口 → 全部改用 `vmx_util.h` 的 `p6_loadu()`（align-down + vperm）。
2. **LLVM 会把 `(x<<4)+(x<<2)` 折叠回 `x*20`**，PPC 后端随即选中
   `vmladduhm / vmulouh / vmsumuhm`——Xenon 没有这些指令（实测 bugcheck
   `c000001d`）。qpel 的移位量改为经 volatile 读取，阻断该折叠。
3. **`avg_*_mc20/mc02/mc22` 用的是 avg 版低通**（结果与 dst 做 `rnd_avg`），
   不是直接覆盖。自写 harness 的参考实现抄了同样的错，反而是**运行时对真实
   FFmpeg C 的自检**抓到了（返回码 34 = size16/pos2/avg）。

### 实测（1080p30，`BV1TP411Y7bt`，低画质开启）

- 解码吞吐 ≈ **18–19 fps**（由 avsync 漂移反推：视频时间 0.60–0.65x）。
  注意 `est-vf` 不是吞吐，它只是容器帧率估计，**不能**用来判断解码能力。
- `vmx=0` 时 UI 掉到 25–28fps、主观明显更卡 → 两个内核确实生效且有用。
- **deblock 不参与**：低画质模式 `vd-lavc-skiploopfilter=all` 已经跳过整块，
  所以下一块不能做 deblock，应优先 **IDCT**（4x4 add / dc-add），再考虑 CABAC。

### UI 掉帧（独立问题）

OSD 隐藏后 `build p50 = 0.31ms`，但 `p95 = 66–109ms`、`max = 168–352ms`，
`present` 恰好一个 vsync(≈16ms) → UI 锁在 40–45fps。即主线程**不是算不动，
是被抢占后错过 vsync**。默认绑核表 `{2,4,1,3}` 把 worker 铺到全部三个核
（含核 0 的第二个硬线程），改成 `spread=2,4,3,5` 后 `build max` 从 352ms
降到 168–252ms，方向对但未根治。

## 9. 2026-11-01 续：内核其实在倒扣、IDCT、绑核表

### 9.1 之前的 VMX 内核比纯 C 还慢（`-fno-builtin` 的坑）

用 `p2_decode` 在**同一个二进制内**用 `vmx=0/1` 对拍（而不是跨构建比），
第一次拿到的结果是反的：

```
bbb_1080 skip_loop_filter, spread:   vmx=0 → 6.10 fps   vmx=1 → 5.41 fps
```

原因在 `vmx_util.h` 的 `p6_store()`：它用 `memcpy(dst, &v, n)` 把向量写到
内存，而 FFmpeg 的编译旗标带 `-ffreestanding -fno-builtin`，所以 clang
**不内联**，每次存储都是一次真实的 `memcpy` 调用。反汇编 `h264_qpel_vmx.o`：

```
3317 R_PPC_REL24 memcpy
```

3317 次函数调用，每个 4x4/8x8 块都要走几次。全改成 `__builtin_memcpy` /
`__builtin_memset` 后降到 12 处（剩下的都是变长拷贝，在自检/hv 路径里）。
同一台机、同一素材：

```
vmx=0 → 6.10 fps    vmx=1 → 6.69 fps   (1 线程)
vmx=0 → 14.04 fps   vmx=1 → 14.97 fps  (3 线程)
```

**结论：之前部署的构建里，两个内核是负收益的。** 任何"关掉更卡"的主观判断
都建立在会走 memcpy 的版本上，不能当作内核有效的证据。

### 9.2 H.264 4x4 IDCT（M4）

`libavcodec/xbox360/h264_idct_vmx.c`，替换 `h264dsp->idct_add` 与
`idct_dc_add`（8 bit）。`tests/p6_idct` 真机对拍 96 组（8 种 stride × 6 种
系数幅度 × AC/DC）0 失配，运行时自检同样通过。

两个新坑：

1. **两趟的取样方式不同**。C 的第二趟读的是 `block[4i .. 4i+3]`（连续 4 个），
   第一趟读的是 `block[i], block[i+4], block[i+8], block[i+12]`（跨步）。所以
   两趟之间必须转置，否则结果只是"接近"（实测差 ±1~20，很容易被误判成舍入）。
2. **`stvewx` 存的元素由地址决定**，不是元素 0。`vec_ste(v, 0, dst)` 在
   `dst` 不是 16 字节对齐时，存的是 `dst` 落在 16 字节槽里的那个元素。必须先
   按 `dst & 12` 把行旋进对应槽位再存。（同理，dsp 契约里 dst 是 `align 4`，
   自检不能拿 1 字节对齐的地址去测。）

收益：180p/3 线程 18.50 → 18.41（**在噪声里**），6 线程下与跳过 deblock 的
组合才看得出来。IDCT 不是低画质路径的瓶颈。

### 9.3 绑核表：5/6 worker 的真正瓶颈

`p2_decode` 1080p + `skip_loop_filter=all` + 全部内核：

| worker | 旧表 4 项 `{2,4,1,3}` | 参考表的 6 项 `{1,3,5,2,0,4}` |
|---|---|---|
| 3 | 18.4 | 19.2 |
| 5 | 14.8 | 24.6 |
| 6 | 17.2 | **25.6** |

旧的 4 项表在 5/6 worker 时按 `n & 3` 回绕，第 5、6 个 worker 又落到 hw2/hw4，
**hw0 与 hw5 从头到尾没被用过**——六个硬线程只用了四个。换成覆盖 6 个硬线程的
表后，6 worker 从 17.2 涨到 25.6 fps（+49%）。这与参考实现
（`kProcessorMap = {1,3,5,2,0,4}`）完全一致。

顺序也有意义：hw1/3/5 是每个核的**第二个**硬线程，前 3 个 worker 先各占一个核
且不碰 hw0（主线程/UI 的老家），第 5 个才开始用 hw0。

### 9.4 FFmpeg 9.0.2 评估（结论：暂不迁移）

FFmpeg 9.0.2 已能为 Xbox 交叉编译并真机解码（600 帧 0 错误），需要两处补丁：

1. `libavutil/wchar_filename.h`：XDK 没有 `WC_ERR_INVALID_CHARS` /
   `GetFullPathNameW`，Xbox 路径是设备根路径，不需要归一化 → 直接短路。
2. `configure`：`h264_decoder_select` 少了 `bswapdsp`（h264 会链 `h274.o`，
   而 `h274.c` 用 `ff_bswapdsp_init`），大端下必需。

同素材、同内核、同绑核表对比：

| | 3 worker | 6 worker |
|---|---|---|
| 4.4.8 + 内核 | 18.64 | 25.62 |
| 9.0.2 + 内核 | 19.16 | 26.40 |

差距 1–3%，在噪声范围内。而 mpv 0.36 对 FFmpeg 9 有 **205 处**已删除 API
（`channel_layout`/`channels`、`avcodec_close`、`pkt_duration`、
`av_stream_get_side_data` 等），移植成本远大于收益。**继续留在 4.4.8。**

## 10. 2026-11-03：多块 IDCT 分发绕过的发现与修复（M4 结论修正）

### 10.1 §9.2 "IDCT 在噪声里"的真正原因

`h264idct_template.c` 的多块 helper（`ff_h264_idct_add16` / `add16intra` /
`add8` / `add8_422`）循环内**直接调用静态 C 单块函数**，不走
`H264DSPContext` 的函数指针。M4 只换了 `h264_idct_add` / `h264_idct_dc_add`
两个槽位 → **帧间/帧内 16x16 的残差重建（最常见的路径）从未用上 VMX**，
只有 intra4x4 路径（predict_luma 里走指针）受益。A/B 当然在噪声里。

### 10.2 修复

`xbox360/h264_idct_vmx.c` 新增多块包装 `ff_h264_idct_add16_x360` /
`add16intra_x360` / `add8_x360` / `add8_422_x360`：逐条复刻 C 的分发语义
（`nnz==1 && dc` 快路径、add16intra 的 else-if 顺序、422 变体第二循环
`scan8[i+4]`/`block_offset[i+4]` 的怪索引——对拍 C 才能 bit-exact）。
新增 `p6_idct_mb_selftest`（4 helper × 2 stride × 200 随机，nnz 0/1/2 混合、
DC 置零覆盖各分支），失配回退 C 并打 WARNING。4:2:2 的 `add8` 槽位装 422
变体（`ff_h264dsp_idct_init_x360` 增收 chroma_format_idc 参数）。

### 10.3 A/B（p2 harness, bbb_1080, 3 worker, skiplf）

| 配置 | fps | mcd（qpel/chroma 内核+emu） | wt（加权胶水） |
|---|---|---|---|
| vmx=1 | 19.2–20.3 | 56% | 10% |
| vmx=0 | 11.6–15.9 | 54% | 15–16% |

**内核合计价值 -20~-45% 吞吐**（vmx=0 对照），推翻 §9.1/§15.9 的
"开/关无差异"旧结论（当时多块路径未接入、播放器内噪声大）。
但 mcd 占比在 vmx=1 下仍 55%，且 6 worker 每线程周期翻倍 → **内存受限**，
继续写内核撞带宽墙。详见
`XBOX360_PLAYBACK_PERFORMANCE_PLAN.md` §15.10（剖析仪器 `prof=1`）。
