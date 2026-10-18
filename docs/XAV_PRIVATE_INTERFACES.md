# XAV 私有媒体源接口（Xbox 360 / YouTube XEX 静态分析）

> 状态：初稿（2026-10-17）
>
> 目标：为 wiliwili 的 Xbox 360 端接入 XAV 硬件解码/自定义媒体源，静态逆向
> YouTube 零售版可执行文件中 **XAV 私有媒体源接口** 的真实形态：
> vtable 布局、方法签名、IID、以及最小调用序列。
>
> 目标二进制：`H:\youtube\default.xex`（IDA 数据库 `H:\youtube\default.xex.i64`）。
>
> 相关文档：`XBOX360_OPTIMIZATION.md`（O10 = XAV 路径与工具链阻塞）、
> `XBOX360_PLAYBACK_PERFORMANCE_PLAN.md`（§15 播放链路借鉴）。
>
> **本文件所有结论均标注证据地址；区分「已确认」与「推断」；不确定项在 §12 列出。**

---

## 1. 结论速查

| 项 | 结论 | 置信度 |
|---|---|---|
| 应用侧需要实现的接口 | **`IXcpManagedMediaStreamSource`**（基类 `IObject`） | 已确认（RTTI） |
| 该接口的参考实现（本次二进制内的实现体） | `media::xav::Source`（DLL/EXE 内实现） | 已确认（RTTI + vtable） |
| 该接口 vtable 布局 | `0x822EC6AC`，共 **12 槽** | 已确认 |
| 事件回调接口 | **`IXcpManagedMediaStreamSourceCallback`** | 名称已确认（mangled 串）；布局为推断 |
| 交给播放器的方式 | `IXAVPlayer::SetMediaStreamSource` = **vtable 槽 33**（字节偏移 132） | 已确认 |
| 播放器对象 IID | `{636907E3-9C92-48C4-8674-0228A01ED1F3}` @ `0x82253E6C` | 已确认 |
| `INativeMediaStreamSource` | **本二进制中不存在** | 已确认（全串搜索） |
| `CMFMediaSourceWrapper` / `CreateMBRMediaSourceFromMSS` | **本二进制中不存在** | 已确认（全串搜索） |
| 该 XAV 运行时实现体 | 无符号，集中在 `0x8330xxxx`–`0x8336xxxx` | 已确认 |

> ⚠️ 任务描述中给出的 `?_7INativeMediaStreamSource@@6B@`、
> `?CreateInstance@CMFMediaSourceWrapper@@...` 等符号 **在本 XEX 中检索不到**
> （见 §11）。本机上实际生效的私有源接口是 `IXcpManagedMediaStreamSource`
> 系列，请以本文件为准。

---

## 2. 方法论与关键前提

### 2.1 字节序（最重要）

Xbox 360 是 **PowerPC，大端（big-endian）**。本二进制中：

- 所有 4 字节指针/整数、RTTI 结构字段、GUID 字段，**均为大端**。
- 例：源码字节 `82 2b d4 40` = 指针 `0x822BD440`（不是 `0x40D42B82`）。

> 若用普通 PE/x86 习惯（小端）去解析，会得到看似「乱码 GUID」和无效指针，
> 这是本任务此前难以推进的直接原因之一。

### 2.2 段布局（已确认）

| 段 | 起 | 止 |
|---|---|---|
| `.rdata` | `0x82002000` | `0x823B3CCC` |
| `.pdata` | `0x823B4000` | `0x82444670` |
| `.text` | `0x82450000` | `0x83A7C5FC` |
| `.data` | `0x83A80000` | `0x83C64670` |
| `.idata` | `0x83C70000` | `0x83C70568` |

> 注意：`.pdata` 里存的是**函数地址表（异常展开信息）**，在做「谁调用了谁」
> 这类交叉引用统计时会大量误报，必须过滤 `0x823B4000–0x82444670` 区间。

### 2.3 RTTI 结构（本次逆向结果，大端）

TypeDescriptor（TD）位于 `.data`，**名字串在 TD+8**：

```
struct TypeDescriptor {          // 32-bit X360
    uint32_t pVFTable;           // 恒为 0x822BD440（运行库 type_info vftable）
    uint32_t spare;              // 0
    char     name[];             // 例如 ".?AVIXcpManagedMediaStreamSource@@"
};
```

在 `name` 前 8 字节即 TD 起始。例如名字串 `0x83AF67CC` ⇒ TD = `0x83AF67C4`。

Complete Object Locator（COL）与 ClassHierarchyDescriptor（CHD）：

```
struct RTTICompleteObjectLocator {   // 20 字节
    uint32_t signature;       // 0 或 1
    uint32_t offset;          // 0
    uint32_t cdOffset;        // 0
    uint32_t pTypeDescriptor; // → TD（绝对地址）
    uint32_t pClassDescriptor;// → CHD（绝对地址）
};
struct RTTIClassHierarchyDescriptor { // 16 字节
    uint32_t signature;       // 0
    uint32_t attributes;      // 0
    uint32_t numBaseClasses;
    uint32_t pBaseClassArray; // → 指针数组，元素指向 BCD
};
struct RTTIBaseClassDescriptor {     // 28 字节
    uint32_t pTypeDescriptor;
    uint32_t numContainedBases;
    int32_t  mdisp, pdisp, vdisp;
    uint32_t attributes;      // 常见 0x40
    uint32_t pClassHierarchyDescriptor;
};
```

**vtable 的 [-1] 槽（即 vtable-4）存放指向 COL 的指针**，据此可从 COL 反查 vtable：

- 在 `.rdata`/`.data` 中搜索「值等于 COL 地址」的 4 字节位置 `P`，
  则 vtable = `P+4`。
- 本二进制共解析出 **3419 个 COL / 3502 个 TypeDescriptor**。

### 2.4 应用侧 vs 运行库侧

- 应用侧（lbshell / Chromium 移植层）类名带调试串，例如
  `media::xav::Source`、`media::xav::XavPlayer`、`media::xav::Player`，
  文件路径形如
  `c:\src\buildbot\xb360_branch\src\lbshell\src\platform\xb360\chromium\media\xav\source.cc`。
- XAV 运行库侧（真正的 XAV 引擎）**无符号、无 RTTI 名字**，代码集中在
  `0x8330xxxx`–`0x8336xxxx`（如 `sub_8335CBA0`）。

---

## 3. RTTI 证据表（已确认）

| 类型名（mangled） | TD 地址 | 名字串地址 |
|---|---|---|
| `.?AVIXcpManagedMediaStreamSource@@` | `0x83AF67C4` | `0x83AF67CC` |
| `.?AVIXcpManagedMediaStreamDescription@@` | `0x83A86824` | `0x83A8682C` |
| `.?AVIXcpManagedMediaStreamAttributeCollection@@` | `0x83A86854` | `0x83A8685C` |
| `.?AUIObject@@` | `0x83A8680C` | `0x83A86814` |
| `.?AVSource@xav@media@@` | `0x83AF7770` | `0x83AF7778` |
| `.?AVPlayer@xav@media@@` | `0x83AF4858` | `0x83AF4860` |
| `.?AVXavPlayer@xav@media@@` | `0x83AF65E4` | `0x83AF65EC` |
| `.?AVListenerDelegate@XavPlayer@xav@media@@` | `0x83AF65B0` | `0x83AF65B8` |
| `.?AUIXAVPlayerEvents@@` | `0x83AF5FEC` | `0x83AF5FF4` |
| `.?AVShellPipeline@media@@` | `0x83AF3490` | `0x83AF3498` |

类型关系（由 CHD/BCD 解析）：

```
IObject                       (TD 0x83A8680C)
  └─ IXcpManagedMediaStreamSource            (TD 0x83AF67C4)
        └─ media::xav::Source                (TD 0x83AF7770, COL 0x823A74EC)
IUnknown
  └─ IXAVPlayerEvents                        (TD 0x83AF5FEC)
        └─ media::xav::XavPlayer::ListenerDelegate (COL 0x823A6DA0, vtable 0x822EBC24)
```

`media::xav::Source` 的 CHD（`0x823A7500`，`numBaseClasses=3`）基类数组
（BCA `0x823A7510`）：

| 序号 | BCD | 基类 |
|---|---|---|
| 0 | `0x823A7520` | `media::xav::Source` |
| 1 | `0x823A6F04` | `IXcpManagedMediaStreamSource` |
| 2 | `0x8236FCE0` | `IObject` |

`IXcpManagedMediaStreamSource` 的 CHD（`0x823A6EE8`，`numBaseClasses=2`）：
自身 + `IObject`。

---

## 4. `IXcpManagedMediaStreamSource` 接口

### 4.1 vtable 布局（已确认）

接口本体在二进制内**没有独立 vtable**（纯接口），但唯一实现体
`media::xav::Source` 的 vtable 与接口槽位一一对应：

- COL 指针存放在 `0x822EC6A8`，值 `0x823A74EC`
- **vtable 起始 = `0x822EC6AC`**

| 槽 | 字节偏移 | 函数地址 | 名称 | 置信度 |
|---|---|---|---|---|
| 0 | 0 | `0x837F03C0` | `IObject` 基类 · AddRef（`this+396` 原子自增） | 已确认（反编译） |
| 1 | 4 | `0x837F0400` | `IObject` 基类 · Release（原子自减；归零时经 vtable 槽 11 析构） | 已确认（反编译） |
| 2 | 8 | `0x837EE280` | `Source::OpenMedia()` | 已确认（调试串） |
| 3 | 12 | `0x837EC938` | `Source::CloseMedia()` | 已确认 |
| 4 | 16 | `0x837ED070` | `Source::Seek(...)` | 已确认 |
| 5 | 20 | `0x837EDAF8` | `Source::GetSample(...)` | 已确认 |
| 6 | 24 | `0x83A30660` | 桩：`li r3,0; blr`（返回 0） | 已确认（反汇编） |
| 7 | 28 | `0x83A30660` | 同上 | 已确认 |
| 8 | 32 | `0x837EE420` | `Source::SetCallback(IXcpManagedMediaStreamSourceCallback*)` | 已确认（调试串 + BindState） |
| 9 | 36 | `0x83A30660` | 桩：返回 0 | 已确认 |
| 10 | 40 | `0x837EF630` | `Source::HandleDrmSetupDecryptorCompleted(...)` | 已确认（反编译函数名） |
| 11 | 44 | `0x837F0868` | 标量删除析构（`operator delete` 语义） | 推断（见 §12） |

`IObject` = `{ AddRef, Release }`（2 个方法，**不含 QueryInterface**）。
因此 `IXcpManagedMediaStreamSource` 自身方法 = 槽 2..（+ 可能的析构）。

> 证据：`0x837F03C0` 字节序列 `lwsync;lwsync;li r11,1;addi r9,r3,0x18C;
> mfmsr;mtmsree;lwarx r10,0,r9;add r8,r11,r10 ...` = 原子加（AddRef）；
> `0x837F0400` 反编译为原子减并在归零时 `bctrl` vtable+44。

### 4.2 方法语义（从实现体反推，已确认行为 / 签名为推断）

以下签名以 PowerPC 32 位调用约定（`this` 在 r3，其余在 r4..）给出，
**参数类型由调用点推断，可能存在偏差**。

```cpp
// IObject（基类，2 个方法）
uint32_t AddRef();          // 槽 0
uint32_t Release();         // 槽 1

// IXcpManagedMediaStreamSource
HRESULT OpenMedia();                                  // 槽 2
HRESULT CloseMedia();                                 // 槽 3
HRESULT Seek(seek_param_t position, ...);             // 槽 4  （见下）
HRESULT GetSample(sample_t* out_sample, ...);         // 槽 5
HRESULT /*槽 6*/  ();                                  // 未实现，返回 0
HRESULT /*槽 7*/  ();                                  // 未实现，返回 0
HRESULT SetCallback(IXcpManagedMediaStreamSourceCallback* cb); // 槽 8
HRESULT /*槽 9*/  ();                                  // 未实现，返回 0
HRESULT HandleDrmSetupDecryptorCompleted(...);        // 槽 10（DRM）
/* 槽 11 = 析构 */
```

要点（均来自 `source.cc` 反编译，行号即调试串中的源码行）：

- **`OpenMedia`（`0x837EE280`，source.cc:173）**
  1. 若 `*(this+124)` 对象尚未就绪（其 vtable 槽 1 返回 0）→ 异步投递任务后返回 0；
  2. 读取 `*(this+332)` 个流 + `*(this+328)` 链表（每个节点取 `*(node[3]+140)`）组装流类型数组；
  3. 取 `*(this+120)`（回调对象），调用 **回调槽 0**：`cb->vtbl[0](cb, *(this+320), count, typeArray)`；
  4. 置 `*(this+268)=1`；若 `*(this+144)` 事件分发器存在，调用其 vtable+20。
- **`CloseMedia`（`0x837EC938`，source.cc:212）**
  调用 **回调槽 8**（无参）：`cb->vtbl[8](cb)`，随后 `*(this+120)=0`。
- **`GetSample`（`0x837EDAF8`，source.cc:245）**
  在满足条件时调用 **回调槽 3**（带一个 `double`）：`cb->vtbl[3](cb, 0.0)`。
- **`Seek`（`0x837ED070`，source.cc:233）** 对 64 位位置做 `/10` 后写入内部状态。
- **`SetCallback`（`0x837EE420`，source.cc:278）** 仅把参数存到 `*(this+120)`（在对象就绪时同步，否则异步投递）。

### 4.3 实现体内的重要字段偏移（已确认）

`media::xav::Source` 对象（构造函数 `0x837F0640`）部分布局：

| 偏移 | 含义 | 证据 |
|---|---|---|
| +120 | `IXcpManagedMediaStreamSourceCallback*`（回调对象） | `SetCallback` 写入；`OpenMedia/CloseMedia/GetSample` 读取 |
| +124 | XAV/`MessageLoopProxy` 类对象（用于投递异步任务） | 各方法 `*(this+124)` 判断 + `sub_82499EA0` 投递 |
| +144 | 事件分发器（`CallbackList`） | `OpenMedia` 中调用其 vtable+20 |
| +268 | `bool`：媒体已打开 | `OpenMedia` 写入 |
| +320 | 流信息键 | `OpenMedia` 传给回调 |
| +328 / +332 | 流链表头 / 流数量 | `OpenMedia` 遍历 |
| +396 | 引用计数 | `AddRef`/`Release` |

构造函数 `0x837F0640(this, refptr a2, refptr a3, int64* a4, cfg* a5, refptr a6)`
写入 vtable `0x822EC6AC`（`*(this)=&off_822EC6AC`）。

---

## 5. 事件回调 `IXcpManagedMediaStreamSourceCallback`

**注意：该接口在本二进制中无 RTTI、无独立 vtable**；名称仅出现在一处
mangled 串（`0x83AF6DD8`）：

```
.?AU?$BindState@V?$RunnableAdapter@P8Source@xav@media@@AAJPAVIXcpManagedMediaStreamSourceCallback@@@Z...
```

即 `HRESULT media::xav::Source::?(IXcpManagedMediaStreamSourceCallback*)`。

从实现体的调用点可推断出以下槽位（**推断，未确认完整布局**）：

| 槽 | 字节偏移 | 调用点 | 推断语义 |
|---|---|---|---|
| 0 | 0 | `Source::OpenMedia` (`0x837EE280`) | 媒体已打开，携带流数量/类型数组 |
| 3 | 12 | `Source::GetSample` (`0x837EDAF8`) | 携带 `double`（缓冲进度/时间） |
| 8 | 32 | `Source::CloseMedia` (`0x837EC938`) | 媒体关闭 |

> 其余槽位无法从单一实现体确定。**建议后续接入时以 XDK/XAV 头文件为准，
> 或在实机上对回调对象打点抓取其 vtable 全表。**

---

## 6. IID / GUID 表（已确认，大端存储）

XAV 运行库通过类似 `IUnknown` 的 `QueryInterface`（工厂对象槽 0）获取接口。
被 XAV 播放链路引用的 IID 位于 `.rdata`：

| 地址 | GUID | 引用位置 |
|---|---|---|
| `0x82253E6C` | `{636907E3-9C92-48C4-8674-0228A01ED1F3}` | `XavPlayer::Initialize`（`0x837E9D54`）、`sub_8335CBA0`（`0x8335CC4C`）、`sub_8335BE20` |
| `0x82253F18` | `{AEAA51E0-4910-4607-B99B-9E3613286ADB}` | `XavPlayer::Initialize`（`0x837E9DB4`）、`sub_8335C5D0`、`sub_8335CE38`、`sub_8335D0B8` |
| `0x82253E3C` | `{9FA7B2AD-0F42-42AE-B3A0-70C7A78E1220}` | `sub_8335BE20`、`sub_8335C968`、`sub_8335CC88`、`sub_8337BB08` |
| `0x82253E0C` | `{96F64745-64B7-4826-A5E9-0233417EA429}` | `sub_8335BC68`、`sub_8335BE20` |
| `0x82253E1C` | `{0419D266-4304-4CF2-9317-6BF91BE3BF71}` | `sub_8335BC68`、`sub_8335BE20` |
| `0x82253E2C` | `{3EF58C34-4E99-4026-9DDA-4132F629804A}` | `sub_8335BC68`、`sub_8335BE20` |
| `0x82253E4C` | `{82A20261-7024-4AE7-AB5C-021A4C4345CE}` | `sub_8335BE20`、`sub_8335C968` |
| `0x82253E5C` | `{98A1B0BB-03EB-4935-AE7C-93C1FA0E1C93}` | `sub_8335BE20` |
| `0x82253DFC` | `{294D3610-2E62-4D88-9EE4-5109028F0F32}` | `sub_8335BC68`、`sub_833DE6C0` |
| `0x82253F08` | `{FC0E10D2-AB2A-4501-A951-06BB1075184C}` | `sub_8335C5D0`、`sub_8335D020` |
| `0x82253A20` | `{B56C6AA7-A00A-4FD2-9728-70CAF7725FE5}` | `sub_8335BC68`、`sub_8335C968`、`sub_83373370` |
| `0x82253A30` | `{B3CC6A2A-F376-4574-B7FA-31AB0E087939}` | `sub_8335BC68`、`sub_8335BE20`、`sub_8335C968` |
| `0x82253700` | `{1302BCFB-35C5-4BB0-8244-E0B2E05569FC}` | `webkit_media` 侧（`sub_83355218` 等） |

> 前一份分析报告提到的「6 个公开 IID」（`0x82253E6C / 0x82253E3C / 0x82253A30 /
> 0x82253F18 / 0x82253A20 / 0x82253700`）与上表一致，可按需当作 XAV 公开
> 接口集合；其余为同一表中被 XAV 内部使用的接口。
>
> **这些 GUID 对应的接口名（`IXAVPlayer` 等）无法从本二进制确认**，不宜臆造。

---

## 7. XAV 播放器对象与 `SetMediaStreamSource`（槽 33）

### 7.1 对象来源

`media::xav::XavPlayer::Initialize`（`0x837E9C40`）建立到 XAV 运行库对象的桥接：

```
obj@this+20  = sub_837E9910(alloc(12), {this})          // 由本包装器派生的 XAV 对象
sub_8335CBA0(&factory, *(this+20), 0)                    // 取 XAV 类对象（工厂）
(*factory)(factory, &IID_82253E6C, &p)  → this+24        // ★ XAV 播放器主接口
(*p + 44)(p, &q)  (*q)(q, &IID_82253F18, &r) → this+28
(*p + 52)(p, &s)                          → this+32
(*p + 56)(p, &t)                          → this+36
```

- `sub_8335CBA0`（XAV 工厂获取器）内部 `sub_83368570(3880)` 分配 **3880 字节**的
  XAV 播放器对象，再对其执行 `QueryInterface(IID 0x82253E6C)`。
- `this+24` 即 **XAV 播放器主接口**，其 vtable 由 XAV 提供（无 RTTI）。

### 7.2 已确认的 XAV 播放器主接口槽位

从应用包装器 `media::xav::XavPlayer` 的调用点反推（`this+24` 的 vtable）：

| 槽 | 字节偏移 | 调用者 | 语义 |
|---|---|---|---|
| 4 | 16 | `XavPlayer::Open` (`0x837EA760`) | `Open(int64 position)` |
| 5 | 20 | `XavPlayer::Play` (`0x837EA870`) | `Play()` |
| 11 | 44 | `XavPlayer::Initialize` (`0x837E9D68`) | 取子对象（再 QI `IID 0x82253F18`） |
| 13 | 52 | `XavPlayer::Initialize` | 取子对象 |
| 14 | 56 | `XavPlayer::Initialize` | 取子对象 |
| **33** | **132** | `XavPlayer::SetMediaStreamSource` (`0x837EAE30`) | **`SetMediaStreamSource(IXcpManagedMediaStreamSource*)`** |

`XavPlayer::SetMediaStreamSource` 关键片段（已确认）：

```c
if (*(this+16) == 0)
    result = (*(*(*(this+24) + 132))(*(this+24), source);   // 槽 33
```

---

## 8. 应用侧包装类（`media::xav::*`，有符号）

### 8.1 `media::xav::XavPlayer`（`xav_player.cc`）

- 构造/初始化：`sub_837EA668(this, refptr, refptr, refptr)`，vtable `0x822EBC40`
- 字段：`+8/+12` refptr、`+16` bool「XAV 已就绪」、`+20` XAV 对象、`+24` 主接口、
  `+28/+32/+36` 子接口、`+40`、`+44` `MessageLoopProxy`
- 方法地址：

| 方法 | 地址 |
|---|---|
| `Initialize` | `0x837E9C40` |
| `Shutdown` | `0x837E9E80` |
| `WaitForXav` | `0x837EA030` |
| `SetMediaStreamSource` | `0x837EAE30` |
| `Open` | `0x837EA760` |
| `Play` | `0x837EA870` |
| `Pause` | `0x837EA960` |
| `Stop` | `0x837EAA50` |
| `SeekAbsolute` | `0x837EAB40` |
| `GetStatus` | `0x837EAC40` |
| `GetStats` | `0x837EAD38` |
| `SetProperty` | `0x837EAF30` |
| `OnXavUpdate` | `0x837EB210` |

> 这些方法普遍采用「就绪则同步调用、否则 `base::Bind` 异步投递 + `WaitForXav`」模式
> （见 `SetMediaStreamSource`、`Open`、`Play` 反编译）。

### 8.2 `media::xav::Player`（`Player` 层）

- vtable `0x822EAA3C`（12 槽），其中槽 5 = `Player::SetPlaybackRate` (`0x837D2B28`)
- `Player::Initialize` `0x837D42F0`：
  - 创建 `XavPlayer`（`sub_837EA668`）→ `Player+124`，并调用 `XavPlayer::Initialize`
  - 创建 `media::xav::Source`（`0x837F0640`）→ `Player+136`
- `Player::SetSource_Open_Play` `0x837D2FE8`：**一站式**：

```
XavPlayer::SetMediaStreamSource(*(Player+124), result, *(Player+136), ...)
XavPlayer::Open(*(Player+124), result, -1, ...)
XavPlayer::Play(*(Player+124), result)
```

---

## 9. 最小调用序列（供 wiliwili 移植参考）

应用侧要「用自己的媒体源 + XAV 播放」，完整链路（地址均为本二进制中已确认的
应用侧函数；XAV 侧为无符号函数）：

```cpp
// 1) 建立 XAV 播放器对象（包装 XavPlayer）
XavPlayer* xp = new XavPlayer(...);          // sub_837EA668
xp->Initialize();                            // 0x837E9C40
   // 内部：sub_8335CBA0 → QI(IID 0x82253E6C) → xp+24（XAV 主接口）
   //       QI(IID 0x82253F18) → xp+28；槽 13/14 → xp+32/+36

// 2) 构造自己的媒体源（实现 IXcpManagedMediaStreamSource）
//    参考实现：media::xav::Source，vtable 0x822EC6AC
Source* src = new Source(refptr a2, refptr a3, int64* a4, cfg* a5, refptr a6); // 0x837F0640

// 3) 交给 XAV
xp->SetMediaStreamSource(src);               // 0x837EAE30 → XAV 主接口 槽 33
   // XAV 随后会调用 src 的槽 8：SetCallback(myCallback)   ← 回调由 XAV 提供

// 4) 打开 + 播放
xp->Open(-1);                                // 0x837EA760 → XAV 主接口 槽 4
xp->Play();                                  // 0x837EA870 → XAV 主接口 槽 5

// —— 此后由 XAV 驱动 ——
// XAV 调用：src->OpenMedia()      (槽 2)   → 你回调 cb[0]
// XAV 调用：src->GetSample(...)   (槽 5)   → 你回调 cb[3]
// XAV 调用：src->Seek(...)        (槽 4)
// XAV 调用：src->CloseMedia()     (槽 3)   → 你回调 cb[8]
```

「一站式」等价入口：`media::xav::Player::SetSource_Open_Play` (`0x837D2FE8`)。

> 只要实现 `IXcpManagedMediaStreamSource`（AddRef/Release + 10 个方法）并提供
> 回调对象，即可让 XAV 从你自建的数据源取流——**无需** `Open(url)`。

---

## 10. 为什么不是 `INativeMediaStreamSource`

任务背景假设 YouTube 通过 `INativeMediaStreamSource` / `CMFMediaSourceWrapper`
接入自定义源。静态证据表明：

1. `SetMediaStreamSource`（`0x837EAE30`）实际读取 `*(this+24)` 的**槽 33**，
   而 `*(this+24)` 由 `IID 0x82253E6C` 的 `QueryInterface` 得到；
2. 传给槽 33 的对象类型在 RTTI 上被确认为 `media::xav::Source`，
   其唯一基类接口是 `IXcpManagedMediaStreamSource`；
3. `INativeMediaStreamSource`、`CMFMediaSourceWrapper`、`CreateMBRMediaSourceFromMSS`
   在本二进制字符串表中**完全不存在**（见 §11）。

结论：**本 XEX 采用的是 `IXcpManagedMediaStreamSource`（"managed" = 由宿主管理）
这套私有接口**。"Native" 系列符号很可能属于其它 XDK/其它版本，或为文档化名称，
不应作为本移植的目标接口。

---

## 11. 本二进制中确认「不存在」的符号

对全部 40449 条字符串做包含匹配，以下均无命中：

| 符号 | 结果 |
|---|---|
| `INativeMediaStreamSource` | 不存在 |
| `NativeMediaStreamSource`（作为类型名） | 不存在（仅函数名 `…SetMediaStreamSource` 含 `StreamSource`） |
| `CMFMediaSourceWrapper` | 不存在 |
| `MediaSourceWrapper` | 不存在 |
| `CreateMBRMediaSourceFromMSS` | 不存在 |
| `MFMediaSource` / `IMFMediaSource` | 不存在 |

存在的相关符号：

- `.?AVIXcpManagedMediaStreamSource@@` @ `0x83AF67CC`
- `.?AVIXcpManagedMediaStreamDescription@@` @ `0x83A8682C`
- `.?AVIXcpManagedMediaStreamAttributeCollection@@` @ `0x83A8685C`
- `.?AUIObject@@` @ `0x83A86814`
- `IXcpManagedMediaStreamSourceCallback`：仅出现在 `0x83AF6DD8` 的模板实例串中

---

## 12. 不确定项 / 后续验证建议

| 编号 | 不确定点 | 建议验证 |
|---|---|---|
| U1 | 槽 6/7/9 返回 0 的桩，是接口真实方法还是未使用槽 | 找 XAV 侧对源的调用点，看其访问的最大偏移；或读 XDK 头文件 |
| U2 | 槽 11 是接口的虚析构还是实现类附加的删除析构 | 检查接口是否声明虚析构；比较其它 `IObject` 派生实现体 |
| U3 | `IXcpManagedMediaStreamSourceCallback` 完整 vtable（目前仅知槽 0/3/8） | 实机对传给 `SetCallback` 的对象抓 vtable；或在 XAV 侧定位该对象的构造函数 |
| U4 | `OpenMedia/Seek/GetSample/SetCallback` 精确参数类型与返回码 | 需要 XAV 头文件；本文件只给出由调用点反推的近似签名 |
| U5 | §6 各 IID 对应的接口名 | 需要 XAV/XDK 头文件对照；本文件只列 GUID |
| U6 | XAV 主接口槽 4(`Open`)/5(`Play`)/33(`SetMediaStreamSource`) 之外的完整布局 | 反编译 `media::xav::Player` / `WebMediaPlayerImpl` 更多调用点 |
| U7 | `Player` vtable 12 槽的完整语义 | 目前仅槽 5 = `SetPlaybackRate` 已确认 |

---

## 13. 复现用工具片段（IDAPython，供后续会话复用）

> 已确认：通过 IDA MCP 的 `execute_python`，`db` 与 `ida_domain` 全局可用
> （先前「不可用」是误判——实为返回值无法 JSON 序列化所致）。
> 代码需定义 `run(db)` 或让末表达式可序列化。

```python
# 1) 在所有段中按大端值搜索指针
import ida_bytes, struct
def scan_be(val, segments):
    pat = struct.pack('>I', val); out = []
    for (st, en) in segments:
        d = ida_bytes.get_bytes(st, en - st); i = d.find(pat)
        while i != -1:
            out.append(st + i); i = d.find(pat, i + 1)
    return out

# 2) 由名字串得 TD，再由 TD 得 COL，再由 COL 得 vtable
#    名字串 ea -> TD = ea - 8
#    COL 满足 rd32(COL+12) == TD
#    vtable 起点：scan_be(COL) 的结果 + 4
```

---

## 附：本次分析使用的关键地址一览

| 地址 | 含义 |
|---|---|
| `0x82253E6C` | XAV 播放器主接口 IID（`XavPlayer+24`） |
| `0x82253F18` | XAV 子接口 IID（`XavPlayer+28`） |
| `0x822EC6AC` | `media::xav::Source` vtable（= `IXcpManagedMediaStreamSource` 布局） |
| `0x823A74EC` | `media::xav::Source` COL |
| `0x83AF67C4` | `IXcpManagedMediaStreamSource` TypeDescriptor |
| `0x83AF7770` | `media::xav::Source` TypeDescriptor |
| `0x837F0640` | `Source` 构造函数 |
| `0x837F0238` | `Source` 析构函数 |
| `0x837EE280` | `Source::OpenMedia` |
| `0x837EC938` | `Source::CloseMedia` |
| `0x837ED070` | `Source::Seek` |
| `0x837EDAF8` | `Source::GetSample` |
| `0x837EE420` | `Source::SetCallback` |
| `0x837EF630` | `Source::HandleDrmSetupDecryptorCompleted` |
| `0x837EAE30` | `XavPlayer::SetMediaStreamSource` → XAV 槽 33 |
| `0x837EA760` / `0x837EA870` | `XavPlayer::Open` / `Play` → XAV 槽 4 / 5 |
| `0x837E9C40` | `XavPlayer::Initialize`（QI `0x82253E6C` / `0x82253F18`） |
| `0x837D2FE8` | `Player::SetSource_Open_Play`（一站式） |
| `0x8335CBA0` | XAV 类对象获取器（分配 3880 字节播放器对象） |
| `0x83A30660` | 通用「返回 0」桩函数 |
| `0x822BD440` | 运行库 `type_info` vftable（所有 TD 的 `pVFTable`） |
