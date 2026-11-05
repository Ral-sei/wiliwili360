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

---

# 第二部分：2026-10-18 深度逆向（完整契约）

> 本部分把 §1–§13 的「接口骨架」补齐为**可直接实现**的完整契约：
> 三个应用侧接口的真实 vtable、属性 schema、样本投递协议（寄存器级）、
> 以及 H.264 支持的运行时证据。
>
> 修正 §4.1：`media::xav::Source` vtable **确为 `0x822EC6AC`**（由 COL `0x823A74EC`
> 反查得 `COL 指针存放在 0x822EC6A8`，与构造函数写入值一致）。
> §3/§5 中「描述对象/属性集合无实现类」的说法不准确——它们的实现类分别是
> `media::xav::StreamDescription` 与 `media::xav::Attributes`，见 §14。

---

## 14. 三个应用侧接口的真实实现类（已确认）

用 COL→CHD→BCD 反向遍历（`pBaseClassArray` 中含目标接口 TD）得到：

| 接口 | 实现类（RTTI） | COL | vtable | 大小 |
|---|---|---|---|---|
| `IXcpManagedMediaStreamSource` | `media::xav::Source` | `0x823A74EC` | **`0x822EC6AC`** | 400 |
| `IXcpManagedMediaStreamDescription` | `media::xav::StreamDescription` | `0x8236FD6C` | **`0x8200EE54`** | 20 |
| `IXcpManagedMediaStreamAttributeCollection` | `media::xav::Attributes` | `0x82370130` | **`0x8200F1A4`** | 68 |
| `IPALStream` | `media::xav::DecoderBufferPALStream` | `0x823704DC` | **`0x8200F4F0`** | 24 |

> `IPALStream` 的 TD = `0x83A87470`（`.?AUIPALStream@@`，**struct**，有独立纯接口
> vtable `0x8200F4BC`），其实现类 TD = `0x83A8748C`。
>
> 这三个 `media::xav::*` 类**属于应用侧（lbshell）代码，不在 `xav.lib` 里**，
> 因此移植时必须自己重写——好在它们都很小（§15–§17 给出全部槽位语义）。

### 14.1 `IXcpManagedMediaStreamDescription`（5 槽，已确认）

构造函数 `0x824C7C58(this, type, id, attributes)`：

```
+0  vtable (0x8200EE54)
+4  uint32 type        // 0 = audio, 1 = video
+8  uint32 id          // 流标识（XAV 侧作为 StreamIndex 使用）
+12 refptr attributes  // IXcpManagedMediaStreamAttributeCollection*
+16 0
```

| 槽 | 偏移 | 函数 | 语义 |
|---|---|---|---|
| 0 | 0 | `0x824D1708` | AddRef（原子自增 `+0x10`） |
| 1 | 4 | `0x824D1748` | Release（原子自减；归零调用 `(*(vtbl))(this,1)`） |
| 2 | 8 | `0x824D1600` | `GetType(uint32* out)` → `out = *(this+4)` |
| 3 | 12 | `0x824D1610` | `GetId(uint32* out)` → `out = *(this+8)` |
| 4 | 16 | `0x824D1620` | `GetAttributes(IAttrColl** out)` → `out = *(this+12)` 并 AddRef |

反汇编 `0x824D1600`：`lwz r11,4(r3); li r3,0; stw r11,0(r4); blr` —— 返回值恒 0。
`0x824D1610` 同形，取 `+8`。`0x824D1620` 额外 `(***(...))(*(a1+12))` = AddRef。

### 14.2 `IXcpManagedMediaStreamAttributeCollection`（5 槽，已确认）

```
+0  vtable (0x8200F1A4)
+4  std::map<wstring, wstring> 起始（实现细节）
+48 vector 起始 / +52 vector 结束（索引化访问用，元素 28 字节）
+64 原子引用计数
```

| 槽 | 偏移 | 函数 | 语义 |
|---|---|---|---|
| 0 | 0 | `0x824CA788` | AddRef（`+0x40` 原子自增） |
| 1 | 4 | `0x824CB6A0` | Release（`+0x40` 原子自减；归零 `sub_824CA7C8` + free） |
| 2 | 8 | `0x824D29E0` | `GetCount()` → `(*(this+52) - *(this+48)) / 28` |
| 3 | 12 | `0x824D3358` | `Set(namePtr, nameLen, valuePtr, valueLen)`（宽字符） |
| 4 | 16 | `0x824D2C80` | `GetAt(index, outType, outName, outValueSize, outValueData)` |

`GetAt` 精确语义（反汇编 `0x824D2C80`）：

```c
HRESULT GetAt(uint32 index, uint32* outType, const wchar_t** outName,
              uint32* outSize, const void** outData)
{
    if (index >= (end - begin) / 28) return E_BOUNDS;   // 0x80070005-ish
    entry = begin + 28 * index;
    *outType = *(entry + 24);
    *outName = entry->name.data;      // std::wstring
    *outSize = entry->value.size;
    *outData = entry->value.data;
    return 0;
}
```

> 应用侧另有一个便捷 `Set(std::wstring key, std::wstring value)`：
> `sub_824D32E0(attr, &key, &value)`（非虚函数，直接调用）。

---

## 15. 属性 schema（Smooth Streaming / MSS 风格，已确认）

字符串均为 **UTF-16BE**，位于 `.rdata` 的低地址区（`0x8200EE00`–`0x8200F2A0`）：

| 键 | 地址 | 值类型 | 来源 |
|---|---|---|---|
| `VideoFourCC` | `0x8200F234` | 宽字符串（如 `"H264"`） | `0x8200EE18` = `"H264"` |
| `Width` | `0x8200F220` | 宽字符串十进制（`"%d"` 格式串在 `0x8200F22C`） | 应用侧按宽高比选 832/1024/1280 |
| `Height` | `0x8200F210` | 宽字符串十进制 | 同上，1088/768/720 |
| `CodecPrivateData` | `0x8200F1EC` | **十六进制字符串**（`sub_8338C020` 逐字符解码） | 音频 WAVEFORMATEX；视频本应用路径**未设置** |
| `KeyFrameFlag` | `0x8200F1C4` | `"True"` / `"False"` | `0x8200F1E0`=`"True"`，`0x8200F1B8`=`"False"` |
| `CanSeek` | `0x82279CAC` | `"True"` | 集合级属性 |
| `Duration` | `0x82264B34` | 十进制（`10 * ticks`） | 集合级属性 |
| `DRMHeader` | `0x82264BCC` | PlayReady WRM header | 仅 DRM |
| `DRMAlgorithmID` | `0x8200F24C` | `"Aes128Ctr"`（`0x8200EE24`） | 仅 DRM |
| `DRMKeyIdentifier` | `0x8200F294` | 十六进制 16 字节 | 仅 DRM |
| `DRMSubSampleMapping` | `0x8200F26C` | 十六进制 | 仅 DRM |

构建代码：`Source::SetupStreams` = `sub_837F08B8`（应用侧），逐流调用
`sub_824CA848`（`Stream` 的 `Configure`），其中：

- **视频**（`type == 2` 分支）：`VideoFourCC="H264"`、`Width`、`Height`；
- **音频**（`type == 1` 分支）：`CodecPrivateData` = 20 字节结构的十六进制；
- 集合级：`CanSeek="True"`、`Duration=<10*ticks>`。

---

## 16. `IPALStream`（样本载荷容器，已确认）

`DecoderBufferPALStream` 构造函数 `0x824D16B8(this, refptr buffer, uint64 offset)`：

```
+0  vtable (0x8200F4F0)
+4  refptr buffer      // 提供数据的对象（vtbl[0] = 取数据指针，vtbl[1] = 取长度）
+8  uint64 position    // 当前读位置
+16 原子引用计数
```

| 槽 | 偏移 | 函数 | 语义 |
|---|---|---|---|
| 0 | 0 | `0x824D17D0` | 标量删除析构（把 vtable 换回 `0x8200F4BC` 后 free） |
| 1 | 4 | `0x824D1708` | AddRef（`+0x10` 原子自增） |
| 2 | 8 | `0x824D1748` | Release（`+0x10` 原子自减；归零 `(*(vtbl))(this,1)`） |
| 3 | 12 | `0x824D16A8` | 桩：`return 0x80000032` |
| 4 | 16 | `0x824D1840` | `Read(dst, size, &read, ...)`，按 `+8` 位置拷贝并推进 |
| 5 | 20 | `0x824D1960` | `Seek(uint64 pos, uint32 origin, uint64* out)`；`origin != 0` → `E_INVALIDARG` |
| 6 | 24 | `0x824D16A8` | 桩 `0x80000032` |
| 7 | 28 | `0x824D1900` | `GetSize(uint64* out)` → buffer 长度 |
| 8 | 32 | `0x8272AD38` | `GetPosition(uint64* out)` → `+8` |
| 9 | 36 | `0x824D16A8` | 桩 `0x80000032` |

> 关键：**XAV 从 `IPALStream` 里"读"出压缩数据**（不是直接拿指针），
> 所以我们只要实现 `Read/Seek/GetSize/GetPosition` 即可，缓冲区布局自由。

---

## 17. 回调接口与样本投递协议（寄存器级，已确认）

回调对象由 **XAV 创建**并通过 `Source` 槽 8 交给应用；应用只调用它。
以下槽位与参数由 `sub_837ED8E0` 的**实际指令**确认（不是反编译推测）：

### 17.1 `OnOpenMediaCompleted`（槽 0）

`Source::OpenMedia` (`0x837EE280`) 调用：

```
r3 = cb, r4 = *(Source+320) /* attribute collection */,
r5 = count, r6 = descArray   // IXcpManagedMediaStreamDescription*[]
```

### 17.2 `OnGetSampleCompleted`（槽 2）—— 核心

`sub_837ED8E0` 的指令序列（`0x837ED960`–`0x837ED9E4`）：

```asm
837ED960  lwz  r31, 8(r11)      ; r11 = *(a2) = 结果结构；r31 = IPALStream*
837ED968  bne  cr6, loc_837ED994
; --- 无数据分支 ---
837ED96C  lwz  r3, 0x78(r29)    ; cb
837ED970  lwz  r4, 4(r11)       ; desc
837ED978  lwz  r10, 0x18(r11)   ; vtbl + 0x18 = 槽 6
837ED980  bctrl                 ; cb->slot6(desc)
; --- 有数据分支 ---
837ED994  lwz  r10, 0(r31)      ; IPALStream vtbl
837ED99C  lwz  r28, 0xC(r11)    ; attrColl
837ED9A0  ld   r27, 0x20(r11)   ; u64 #3
837ED9A4  ld   r26, 0x18(r11)   ; u64 #2
837ED9A8  ld   r25, 0x10(r11)   ; u64 #1
837ED9AC  lwz  r9, 4(r10)       ; IPALStream->AddRef
837ED9B4  bctrl
837ED9B8  lwz  r3, 0x78(r29)    ; cb
837ED9C0  lwz  r4, 0(r30)       ; r30 = a2
837ED9C4  mr   r8, r27          ; arg6 = u64 #3
837ED9C8  mr   r7, r26          ; arg5 = u64 #2
837ED9CC  mr   r6, r25          ; arg4 = u64 #1
837ED9D0  mr   r5, r31          ; arg3 = IPALStream*
837ED9D8  lwz  r10, 8(r11)      ; vtbl + 0x08 = 槽 2
837ED9E0  lwz  r4, 4(r4)        ; arg2 = desc
837ED9E4  bctrl                 ; cb->slot2(desc, stream, u64#1, u64#2, u64#3, attrColl)
```

对应 xav.lib 的 mangled 名完全一致：

```cpp
HRESULT OnGetSampleCompleted(
    IXcpManagedMediaStreamDescription*       desc,
    IPALStream*                              stream,
    uint64_t                                 a,   // 结果结构 +0x10
    uint64_t                                 b,   // 结果结构 +0x18
    uint64_t                                 c,   // 结果结构 +0x20
    IXcpManagedMediaStreamAttributeCollection* attrs);
```

结果结构由 `sub_824C8720(out, buffer, data, attrs, u64_1, u64_2, u64_3)` 填充
（**7 个参数**，反编译只显示了 6 个；真实寄存器映射见 `0x824C87DC`）：

```
+0x04 = buffer  (refptr)
+0x08 = IPALStream(new DecoderBufferPALStream(data, u64_1))   // u64_1 作为起始偏移
+0x0C = attrs   (refptr)
+0x10 = u64_1   // 样本在 buffer 内的字节偏移（实测恒 0）
+0x18 = u64_2   // 时间戳（由 `sub_824CB5A8` 的虚调用返回值 extsw 而来）
+0x20 = u64_3   // 时长（`10 * ticks`）
```

> 调用点 `0x824CB5B0`–`0x824CB5C8`：
> `r7(a5)=0`、`r8(a6)=extsw(时间戳)`、`r9(a7)=10*ticks`。

### 17.3 其余已知槽位

| 槽 | 偏移 | 调用点 | 语义 |
|---|---|---|---|
| 3 | 12 | `Source::GetSample` `0x837EDC10` | `cb->slot3(float progress)`；`lfs f1, flt_822DD99C`（值 0.0f），随后把属性 `XAV.Source.Buffering` 置 `"true"` |
| 6 | 24 | `sub_837ED8E0` `0x837ED978` | `cb->slot6(desc)`，样本为空/请求失败时调用 |
| 8 | 32 | `Source::CloseMedia` `0x837EC9F0` | `cb->slot8()` 无参，媒体关闭 |

> 槽 6 只有 1 个参数（desc），与 xav.lib 中
> `OnMediaStreamEnded(desc)` / `OnSwitchMediaStreamCompleted(desc)` 之一对应；
> 仅凭单一调用点无法判定具体名称，**但参数形态已确定**。

### 17.4 最小可用实现顺序

```
SetMediaStreamSource(src)      // IXAVPlayer 槽 33（obj+3824 → vtable 0x82253C30）
Open(-1)                       // 槽 4
Play()                         // 槽 5
  ↓ XAV 驱动
src->SetCallback(cb)           // Source 槽 8
src->OpenMedia()               // Source 槽 2 → cb->slot0(attrs, n, descs)
src->GetSample(desc)           // Source 槽 5 → 异步取数据
  → 有数据: cb->slot2(desc, palStream, off, ts, dur, attrs)
  → 无数据: cb->slot6(desc)
src->Seek(pos)                 // Source 槽 4
src->CloseMedia()              // Source 槽 3 → cb->slot8()
```

---

## 18. H.264 支持的运行时证据（已确认）

| 证据 | 地址 | 说明 |
|---|---|---|
| `XMF_AllocDecoderPages_H264_VC1_MF` | `0x8336C9E8` | IDA 自带符号名；为 H.264/VC-1 预分配解码器页 |
| `"H264Decoder_Pages"` | `0x82255868` | 解码器内存池名，被上者引用（`0x8336C9E8`/`0x8336CA70`） |
| `"MediaFoundation_Pages"` | 同上函数内 | MF 内存池 |
| `"VC1Decoder_Pages"` | 同上函数内 | VC-1 内存池 |
| `"Failed to prepare AVC sample for decode"` | `0x822EA69C` | 应用侧 `sub_837D1170` 的 AVC 样本准备失败路径 |
| `"Failed to allocate StreamParserBuffer"` | 同上函数 | 同上 |
| `"H264"` / `"AVC1"` | `0x82279838` / `0x82279A44` | FourCC 常量 |
| `"avc1"` / `"avc1.*"` | `0x8202B83C` / `0x822E9CFC` | demuxer 侧匹配 |

属性解析器（XAV 运行时读描述对象）：

| 函数 | 地址 | 作用 |
|---|---|---|
| `sub_8338C9A8` | `0x8338C9A8` | 描述 → 媒体类型分发：`GetType()`==1 → 视频，==0 → 音频 |
| `sub_8338C628` | `0x8338C628` | 视频：读 `VideoFourCC`/`Width`/`Height`/`CodecPrivateData`，组 88 字节格式块，FourCC 存 `+0x40`（64） |
| `sub_8338C160` | `0x8338C160` | 音频：读 `CodecPrivateData`，按 `wFormatTag`（0x55/0x160/0x161/0x162/0x163/0x1610/0xFFFE）字节交换 |
| `sub_8338C020` | `0x8338C020` | 十六进制字符串 → 二进制 |
| `sub_83493688` | `0x83493688` | `OnOpenMediaCompleted` 下游：逐描述建类型并建拓扑 |
| `sub_83407A38` | `0x83407A38` | 用媒体类型创建流对象 |

> **结论：XAV 运行时确实具备 H.264 解码路径**，`VideoFourCC="H264"` 会被接受并
> 映射到 `H264Decoder_Pages` 内存池。§12 的 U4 由此可以关闭。

---

## 19. 移植检查清单（供实现阶段使用）

必须自己实现（都不在 `xav.lib` 内）：

1. `IXcpManagedMediaStreamSource`（12 槽，vtable 布局见 §4.1）
2. `IXcpManagedMediaStreamDescription`（5 槽，§14.1）
3. `IXcpManagedMediaStreamAttributeCollection`（5 槽，§14.2）
4. `IPALStream`（10 槽，§16）

必须准备的属性：

- 视频流：`VideoFourCC="H264"`、`Width="1280"`、`Height="720"`
- 音频流：`CodecPrivateData=<WAVEFORMATEX 的 hex>`
- 集合级：`CanSeek="True"`、`Duration=<10*ticks>`

样本投递：`cb->slot2(desc, palStream, 0, timestamp, duration, attrs)`

### 19.1 仍然未知 / 风险

| 编号 | 风险 | 说明 |
|---|---|---|
| R1 | 样本字节格式 | 应用侧视频**不设** `CodecPrivateData`，SPS/PPS 必须带内。`sub_837D0470`（AVC 样本准备）对 subsample 偏移做 `+ (4 - NAL_length_size)` 修正，说明送进 XAV 的是 **AVCC 长度前缀**（4 字节）而非 Annex-B；仍需实机确认 |
| R2 | `u64 #2/#3` 单位 | 由 `10 * ticks` 推断为 100ns；需实机验证 |
| R3 | 槽 6 具体语义 | 参数形态已定；`xav.lib` 中 `CMFMediaSourceWrapper` 同时有 `OnMediaStreamEnded(desc)` 和 `OnSwitchMediaStreamCompleted(desc)`，槽 6 对应前者（样本为空 = 流结束），后者未在本应用路径被调用 |
| R4 | 链接 | ~~硬阻塞~~ **已解除**，见 §20.1（`@`→`#` 转义问题，用 `llvm-objcopy --redefine-sym` 绕过） |

---

## 20. `xav.lib` 里可直接复用的私有源胶水（2026-10-18 确认）

把 `xav.lib`（250 MB / 2105 个归档成员）用 OXDK 的
`xbox360/tools/oxdklink/mkarchive.py` 转换后得到 **893 个 ELF 对象 + 1208 个导入**，
仅 1 个成员失败（`xam.xex` 的导入记录，非代码）。符号表证明下面这些都在库里：

| 符号（`@`→`#` 转义前） | 作用 |
|---|---|
| `CMFMediaSourceWrapper::CreateInstance(IXcpManagedMediaStreamSource*, CMFMediaSourceWrapper**)` | 把应用侧源包成 MF media source |
| `CMFMediaSourceWrapper::OnOpenMediaCompleted(IAttrColl*, uint32, IDesc**)` | 回调槽 0 |
| `CMFMediaSourceWrapper::OnGetSampleCompleted(desc, IPALStream*, _J, _J, _J, IAttrColl*)` | 回调槽 2（三个 `_J` = 64 位，与 §17 一致） |
| `CMFMediaSourceWrapper::OnMediaStreamEnded(desc)` | 回调槽 6 |
| `CMFMediaSourceWrapper::OnStreamTick(desc, _J)` | 流时钟 |
| `CMFMediaSourceWrapper::OnSwitchMediaStreamCompleted(desc)` | 切流完成 |
| `CMediaAttributeCollection::CreateInstance(IAttrColl**)` + `AddAttribute(coll, name, value)` | 属性集合与写入 |
| `CChunkStreamBuffer::CreateInstance(IMFMediaBuffer*, CChunkStreamBuffer**)` | `IPALStream` 实现（`Read/Seek/GetSize/GetOffset/Clone`） |
| `CreateMBRMediaSourceFromMSS(IMFMediaSource**, source, IPropertyStore*, IMFAsyncCallback*, IUnknown*)` | **有定义**（此前误判为不存在） |
| `CMediaStreamDescription::CreateInstance(IXcpManagedMediaStreamDescription**)` | 描述对象（MBR 用，无 type/id setter） |

> 结论：**只需要自己实现 `IXcpManagedMediaStreamSource` 的 12 个槽**，
> 描述对象、属性集合、`IPALStream`、回调都可以复用库里现成的实现。

### 20.1 链接注意事项（已实测）

`coff2elf.py` 会把 MSVC 修饰名里的 `@` 一律转义成 `#`
（`coff2elf.py` 第 239–247 行，`ESCAPE_CHAR = '#'`，目的是避免 ELF 把 `@` 当版本分隔符）。
因此 `xav.lib` 里定义的是 `?XAVInitialize##YAJPBUXAVInit##QAK#Z`。

`#` 在 PPC 汇编里是注释符，**`asm("?XAVInitialize@@...")` 这类绑定不可能成立**。
正确做法是用 `llvm-objcopy --redefine-sym` 把入口点重命名成纯 C 名：

```sh
llvm-objcopy \
  --redefine-sym '?XAVInitialize##YAJPBUXAVInit##QAK#Z=XAVInitialize' \
  --redefine-sym '?XAVShutdown##YAJXZ=XAVShutdown' \
  --redefine-sym '?XAVPreRender##YAJXZ=XAVPreRender' \
  --redefine-sym '?XAVPostRender##YAJXZ=XAVPostRender' \
  --redefine-sym '?CreateXAVPlayer##YAJPAPAUIXAVPlayer##PAUIXAVPlayerEvents##PAUIXAudio2###Z=CreateXAVPlayer' \
  xav.a xav_ren.a
```

之后用普通 `extern "C"` 声明即可（`xbox360_xav.cpp` 已改为这种方式）。

> **注意（尚未自动化）**：重命名目前是对 oxdklink 的转换缓存做的——
> `~/.cache/oxdk360/xav.a`（`toolchain.cache_dir()`，可用 `OXDK360_CACHE` 覆盖）。
> `mkarchive.build()` 以 `sha1(xav.lib 大小:修改时间:coff2elf.py 哈希)` 作为缓存戳，
> 戳不变就直接复用，所以手工重命名过的归档会一直生效；一旦 `xav.lib` 或
> `coff2elf.py` 变化，缓存会重建并丢掉重命名，需要重新执行上面的命令。
> 要做成可复现的构建，得把这一步挂到 `scripts/oxdklink_xbox360.py` 里（在
> oxdklink 转换之后、链接之前），或在 `mkarchive.py` 加一个重命名钩子。

> **修正 O10**：此前「`xav.lib` 无法链接」的结论来自用 `llvm-nm` 去读 COFF 成员
> （LLVM 22 不认识 `IMAGE_FILE_MACHINE_POWERPCBE` = 0x01F2），那是工具用法错误。
> 成员本身是合法 COFF（`coff2elf` 逐个转换成功），真正的坑只有上面的 `@`→`#` 转义。

### 20.3 用 clang 实现这些接口时的 vtable 注意事项（实测）

XAV 按 **MSVC 布局**取槽（`(*(vtbl + 4*N))`）。OXDK 的 `+xenon-abi` 让普通虚函数
的槽位与 MSVC 一致，但**虚析构仍占 2 个槽**（complete + deleting），实测：

| 类 | 期望槽数 | 用虚析构时 vtable 大小 | 改成普通虚函数后 |
|---|---|---|---|
| `IPalStream` | 12（0 = 析构） | 60 字节 = 15 字 = 2 前缀 + **13** 槽 ❌ | 56 字节 = 2 前缀 + **12** 槽 ✅ |
| `Source` | 12（11 = 析构） | — | 56 字节 ✅ |
| `StreamDescription` | 5 | — | 28 字节 ✅ |

> 结论：**槽 0 是析构的接口（`IPALStream`）必须把析构声明成普通虚函数**
> （例如 `virtual void Destroy()`，实现里 `delete this`），不能用
> `virtual ~T()`。`Source` 的析构本来就在槽 11（接口无虚析构），所以不受影响。
>
> 另外注意 Itanium vtable 前面还有 2 个字（offset-to-top / typeinfo），但对象里的
> vptr 指向**第一个函数槽**，所以 `(*(void***)obj)[N]` 与 MSVC 的槽号一致。

### 20.2 打开 `XBOX360_XAV` 后剩余的链接阻塞（逐个实测，2026-10-18）

| 现象 | 根因 | 处理 |
|---|---|---|
| `duplicate symbol: _strtoi64` | `xav.a` 引用 `_strtoui64` → 拉入 `libcMT` 的 `00532_strtoq.obj`，它里面的 `_strtoi64` 是 MSVC 单寄存器 64 位 ABI，与本项目 compat 里的强定义冲突 | 在 `cbffmpeg/compat/xbox360/xbox_posix_extra.c` 补 `_strtoui64` / `_strtoui64_l`（clang ABI），让 `strtoq.obj` 永不入链 |
| `open/close/read/write/lseek/isatty/fdopen/dup2/unlink/tempnam/vsnprintf/vsnprintf_s/_setjmp/j1` 未解析 | `oldnames.lib` 的 118 个成员是 **machine 0x0000** 的 139 字节 `/alternatename` 记录，`coff2elf` 直接拒绝（`machine 0x0000, expected 0x01F2`），115 个成员里 114 个失败 | 在 `wiliwili/source/platform/xbox360/xbox360_crt_shims.c` 按别名表转发到 `libcMT` 的 `_` 前缀实现（`_setjmp`→`setjmp`，`j1`→`_j1`） |
| `XHttp*`（13 个）未解析 | `xav.lib` 的 HTTP 栈调用 Xbox Live HTTP API | `--libs` 追加 `xhttp`（仅 `XBOX360_XAV=ON` 时） |
| `__real_WSAStartup` / `__real_getsockopt` 未解析 | `xav.a` 引用 `gethostbyname` → 拉入 `xbox_winsock_extra.o`，该对象的 `__wrap_*` 依赖链接期 `--wrap` | 未启用 `--wrap` 时，在 crt shims 里把 `__real_*` 直接转发给 XNet 入口（若将来启用 `--wrap` 会自递归，注释已标注） |
| `IsEqualGUID` 未解析 | `xav.a` 里是每个 TU 各一份的内联 COMDAT，链接器一份都不选；`d3dx9`/`xaudio2` 有真定义，但归档只扫描一趟，XAV 单独引用时它们不会被拉入 | 在 crt shims 里用 `#if defined(X360_XAV)` 条件提供（无条件提供会在默认构建与 `d3dx9.a` 撞 `duplicate symbol`） |

---

## 21. 实机验证记录（2026-10-18，devkit 192.168.2.9）

用本地 Annex-B 流（320x176 / 25fps / 250 帧，`game:\xav_test.h264`）驱动私有源；
`mpv_core.cpp` 的 `xavFeedLocalH264File()` 在该文件存在时接管 XAV 视频流。

### 21.1 已确认可用

```
XAV initialized (experimental hardware decode)
XAV source: SetCallback(1)
XAV source: OpenMedia -> 1 streams, hr=0x00000000
XAV state -> 0 → 1 → 2 → 3 → 4 → 5
XAV source: Seek(0)
XAV media source installed: video=1 audio=0 open=0x00000000 play=0x00000000
```

即 `IXAVPlayer` 槽 33 `SetMediaStreamSource` 接受我们的对象、`Open("")` 与 `Play()`
返回 S_OK、XAV 会回调 `OpenMedia`（槽 0）并调用 `Seek`（槽 4）。
**接口布局与调用协议已验证正确。**

### 21.2 未通过

状态停在 `state=5`（`XAVPlayerStateStarting`），**`GetSample`（槽 5）一次都没被调用**，
`delivered=0`、`texture=0`。补上 `CodecPrivateData`（AVCC 配置记录 hex）后无变化。
唯一的 alert 是 `level=4 (Info) component=0 (Load) type=0`，其 `hr` 字段实为浮点 1.0（非错误码）。

同一台机器上：
- `sample.wmv`（VC-1 720p）走公开 `Open()` 正常播放（`state=6`、画面正常）；
- H.264/AAC MP4 走公开 `Open()` 只能解析、不播放；
- 系统自带视频播放器也打不开视频。

### 21.3 结论与后续

现象组合（WMV 可用、H.264 在公开与私有两条路径上都起不来、系统播放器同样失败）
指向**这台 devkit 的媒体栈 / 可选媒体更新（H.264 解码器）不可用**，而非接口实现问题。

需换机复验：同一个 `wiliwili.xex` + `xav_test.h264`，若到达 `state=6` 且
`XAV draw: delivered>0 texture=1`，则实现正确。

若换机后仍停在 `state=5`，按序排查：
1. 样本是否必须是 **AVCC（4 字节长度前缀）** 而非 Annex-B；
2. `Duration` 属性单位（参考实现用 `10 * ticks`）；
3. 是否必须同时声明音频流（参考实现总是音视频各一条）。

### 21.4 已排除项与当前主线索

已排除（都实测过，行为不变）：
- 样本格式：Annex-B 与 AVCC 结果相同（`state=5`、`delivered=0`）；
- `CodecPrivateData`：补上 AVCC 配置记录后结果相同；
- 属性写入：回读确认 5 条属性名字/值/类型全对
  （`VideoFourCC`=11、`KeyFrameFlag`=12、`CodecPrivateData`=16、`Width`=5、`Height`=6，
  且 `size` 返回的是**字符数**，与 XAV 的十六进制解码器一致）；
- 描述对象：`GetStreamId`/`GetType`/`GetAttributes` 都被 XAV 调用，`type=1`（视频）。

**主线索**：XAV 侧的"异步打开完成"实现 `sub_83493F20` 在 `sub_83493688`
建完拓扑之后，会看 **wrapper+444 这个"模式"字段**：

```c
v16 = a1[111];              // wrapper + 444
if ( v16 == 0 ) goto LABEL_43;   // 直接返回成功，什么都不做
v19 = a1[111];
if ( v19 == 1 || v19 == 2 )      sub_83480B30(...);   // 推流模式
else if ( v19 == 3 )             sub_83480B98(...);
```

`a1[111] == 0` 时函数以成功返回但不再建立播放。

> **2026-10-18 更正**：这个字段**不是推流模式，而是 DRM 模式**。`sub_834911A0`
> 遍历集合级属性后这样赋值：
>
> ```c
> if (v8)                      *(a1+444) = 1;   // 有 XCP_BLOB_WMDRM_OBJECT  → WMDRM
> else if (v10) { if(!v9) fail; *(a1+444) = 3; } // PRNDSessionID + DRMHeader → PlayReady+会话
> else if (v9)                 *(a1+444) = 2;   // 只有 DRMHeader           → PlayReady
> else                         /* 0 */;         // 无 DRM
> if (*(a1+444) != 0) return sub_8348F130(...);  // 只有有 DRM 才继续做解密器设置
> ```
>
> 同一函数里还识别 `Duration`、`XCP_MS_BOOL_IS_LIVE`、`XCP_MS_BOOL_DVREnabled`、
> `AllowedSourceIDs`。**未加密内容的模式 0 是正常的**，所以这条线索作废——
> `state=5` 之后不来取样本与 DRM 无关。

### 21.5 追加实验（同一台 devkit）

| 实验 | 结果 |
|---|---|
| `Open()` 传非空 URL（`http://127.0.0.1/x`） | 无变化，仍停在 `state=5` |
| 主动投递首个样本（不等待 `GetSample`） | **回调槽 2 有效**：XAV 收到后会反调描述对象的 `GetType`/`GetStreamId`，但之后**不再要下一帧**，`texture=0` |
| 样本数据校验 | `pushed 250/250`，投递时 2407 字节正确送达 |

即：`OnGetSampleCompleted`（槽 2）能被正确调用、样本内容正确，但 XAV 的 stream
**从不主动 `RequestSample`**，所以只收到我们主动推的那一帧就停了。这与 §21.4 的
"模式字段为 0、管线没建立"判断一致。

运行时抓到的回调对象 vtable（XEX 固定基址 `0x82000000`）：

```
vtbl = 0x82076cc8   s0 = 0x835c53d8   s2 = 0x835c5560   s3 = 0x835c56a8
s6 = 0x835c5868     s8 = 0x835c19d8
```

注意：`0x82076cc8` 处的 vtable 内容在**文件里是 `0xFFFFFFFF`**（加载器按重定位填充），
所以静态读不到；而槽目标落在 IDA 未识别为函数起点的 thunk 区（相邻 adjustor thunk
被合并进前一个函数），直接反汇编会对不上，需要沿跳转跟进去。

### 21.6 实现侧修掉的三个真实缺陷（2026-10-18 晚）

| 缺陷 | 现象 | 修法 |
|---|---|---|
| `Seek` 清空队列 | 日志显示 XAV 在自己的线程（`0xf9000030`）调 `Seek(0)`，而 demuxer 在主线程 push，250 个样本被清到只剩 130，还出现 0 字节样本 | 参考实现的 `Seek` 只记录位置、**不清队列**；按此移除清空 |
| 队列无并发保护 | 同上，`std::deque` 被两个线程同时改，样本内容被破坏 | 加 `SpinLock`（OXDK 的 libc++ 没有 `<mutex>`，用 `std::atomic_flag`） |
| `IPALStream` 提前释放 | 回调返回后立刻 `Release()`（引用计数归零 → 删除），而 XAV 是**回调返回之后**才去读这个流 | 由源对象持有到销毁（参考实现同样是调用方持有） |

另外：一次性把 250 个样本全推给 XAV 会让它的内部队列耗尽，
`OnGetSampleCompleted` 返回 `E_OUTOFMEMORY (0x8007000e)` 并触发断言
（日志出现 `Linked statically with XAVATAR.LIB ...` 后重启）。**必须按需投递**。

### 21.7 当前最终状态

- 单帧投递：`delivered 1 samples (14861 bytes)` —— IDR 帧完整送达；
- XAV 状态：`0→1→2→3→4→5` 后停在 `Starting`，`texture=0`；
- `GetSample`（槽 5）**仍未被调用**，说明 XAV 的解码管线没有向源请求数据；
- 唯一 alert 仍是 `Load/Info/LoadDefault`，没有拓扑错误（`LoadCreateTopology` 是 4，未出现）。

结论：**接口层已经正确**（XAV 收下对象、读描述与属性、建拓扑、收样本、还回
`E_OUTOFMEMORY` 说明它在为样本分配缓冲），但解码管线不消费样本。
在**两台不同主机**上表现一致，且这两台的系统视频播放器都打不开视频，
最可能的原因是 H.264 解码器（Optional Media Update）不可用——WMV/VC-1 可以正常播放。

下一步需要一台**确认能播 H.264** 的主机复验；若那时仍停在 `state=5`，
再按 §21.3 的顺序排查时间戳单位与 `OnStreamTick`。

### 21.8 零售机复验 + 崩溃定位（2026-10-18 深夜）

主机刷回零售（`xbconsoletype` = `reviewerkit`，IP 改为 `192.168.2.10`，KDNet 不可用
但 XBDM 仍可用）。用户提供了一张该机此前用 YouTube 播放成功的照片，stats for nerds 显示：

```
Codecs: avc1.4d401f (136) / mp4a.40.2 (140)
Res:    1280x720@30 / 1280x720@30
```

**这证明该机的 H.264 硬解可用**，所以之前的失败与"缺可选媒体更新"无关，是本文档
描述的实现问题。

在零售机上重跑同一份 XEX，行为与 devkit 完全一致（`state 0→1→2→3→4→5`，
`Seek(0)`，投递一帧 14861 字节后 `texture=0`），但**随后崩溃**：

```
exception code=0xc0000005 thread=0xf9000030 address=0x835c3650 read=0x0000010c
Cipher[2861195|ExceptionHandler]: Exception is title-level again => going to dash
```

`0x835C3650` 位于 `sub_835C3638`，反编译为：

```c
int sub_835C3638(int a1, int a2)
{
    if (a2 == 0) return E_POINTER;
    v3 = *(_DWORD *)(a1 + 44);           // ← 崩溃点：a1 为 0xE0
    if (v3 == 844715353 /*YUY2*/ || v3 == 1498831189 /*UYVY*/
        || ... || (v3 >= 0x14 && v3 <= 0xFF)) { /* 走通用处理 */ }
    return -1072875852;
}
```

这是一个**像素格式 → D3D 格式**的转换/校验函数，说明 XAV 已经走到"处理解码输出"
这一步，但传入的格式描述对象指针是坏的（`0xE0`）。

下一步优先怀疑：`IPALStream` 的**方法顺序/槽数**与真实接口不符——我们的 12 槽顺序
是从 `DecoderBufferPALStream` 的 vtable 反推的（dtor, AddRef, Release, SetSize,
Read, Seek, Write, GetSize, GetOffset, Clone, CanSeek, GetCapabilities），
而 xav.lib 里 `CChunkStreamBuffer` 的方法集合提示真实顺序可能是
（dtor, AddRef, Release, Read, Seek, GetSize, GetOffset, CanSeek, GetCapabilities,
Clone, SetSize, Write, ...）。顺序错了会让 XAV 把 `GetSize` 当 `Read` 之类调用，
拿到垃圾长度，进而在像素格式处理里用到坏指针。

验证方法：在 `IPALStream` 的每个槽里打印序号（当前只在 Read/GetSize 里打了），
对照 XAV 实际调用的先后，即可确定真实顺序。

### 21.9 2026-10-19：`OnGetSampleCompleted` 三个 `_J` 的语义（已确认）

在 XDK 内核上（IP `192.168.2.9`）用带全量 `PalStream` 日志的构建跑出：

```
XAV pal: Seek a=0 b=0
XAV pal: Read a=0 b=0          ← 旧参数：把时间戳当成了长度
```

改成 `(offset=0, length=样本字节数, timestamp)` 后：

```
XAV pal: Seek a=0 b=0
XAV pal: Read a=14861 b=14861   ← 读到完整 IDR 帧
```

**结论：`OnGetSampleCompleted(desc, pal, offset, length, timestamp, attrs)`**
—— 第二个 `_J` 是**长度**，不是时间戳。此前按 `(offset, timestamp, duration)`
传参，第一帧时间戳正好是 0，于是 XAV 每次都 `Read(0)`，拿到空样本后解析崩溃。

`Seek(offset)` + `Read(length)` 的调用序列与 xav.lib 的
`CreateMFMediaBufferFromStreamBuffer(IPALStream*, _J, _J, IMFMediaBuffer**)`
完全对应（两个 `_J` 就是 offset / length）。

### 21.10 当前剩余崩溃

样本能读到了，但 XAV 随后仍崩溃：

```
exception code=0xc0000005 thread=0xf9000030 address=0x835c40d0 read=0x0000010c
```

顺序是：`Read(14861)` → `desc->GetStreamId()` → 崩溃。崩溃点在
`sub_835C3638` 这类**像素格式 → D3D 格式**的转换函数里（`*(a1+44)` 与
`YUY2`/`UYVY` 比较），`a1` 为 `0xE0`（坏指针）。
`GetSample`（槽 5）依然没有被调用，`texture` 仍为 0。

注意：XAV 的地址会随我们代码大小变化而移动，所以崩溃地址只能在同一份构建里对照。

下一步方向：确认这个像素格式对象应当由谁提供（很可能是解码器输出的
`MFVIDEOFORMAT`/`D3DFORMAT` 描述），以及 `IXAVPlayer` 的
`XAVOpenParamVideoWorkThreadAffinity`/`fGameMode` 是否影响它。

### 21.11 2026-10-19 后续排除（全部实测，崩溃不变）

| 实验 | 结果 |
|---|---|
| `_J` 改成 `(offset, length, timestamp)` | ✅ `Read` 拿到完整 14861 字节（**这是真修复**） |
| 视频流 id = 0 / 1 | 都崩，无差别 |
| `XAVInit.fGameMode` = TRUE / FALSE | 都崩，无差别 |
| 样本改回 **Annex-B** 且不传 `CodecPrivateData` | `Read 14857` 成功，仍崩 |
| 不投递样本 | **不崩**（`state=5` 稳定），确认崩溃由投递触发 |

固定特征：每次都是 `exception code=0xc0000005 ... read=0x0000010c`，
即某个**空指针 + 0x10C** 的成员访问；崩溃发生在 `Read` 返回之后、
`desc->GetStreamId()` 之后，`texture` 始终为 0，`GetSample` 从未被调用。

> 注意：XAV 的代码地址会随我们目标文件大小变化而整体移动
> （同一份二进制内才可比），所以只能用"崩溃数据地址 `0x10c`"这类不变量来对照。

剩余可能：
1. XAV 期望 `OnGetSampleCompleted` 的第 6 个参数不是**流的**属性集合，
   而是别的对象（我们传的是 `Stream.attributes`）；
2. 解码器输出的 `MFVIDEOFORMAT` 需要 `CodecPrivateData` 的某种特定编码
   （AVCC 记录已试，仍崩）；
3. 需要同时声明**音频流**——参考实现总是音视频成对，XAV 的 sink 可能
   在等待音频时钟才建立输出格式。

### 21.12 第 6 个参数的真实身份（2026-10-19 收尾）

回看 `sub_824CAFC8` 的调用点：

```c
v34 = (a1 + 144);
if (*(_BYTE *)(v12 + 56) == 0) v34 = (a1 + 148);   // 样本的 keyframe 标志
sub_837D1DA8(v45, *v34);
v18 = v45[0];
sub_824C8720(out, a1 + 140, a2, v18, ...);          // v18 → 结果结构 +0x0C
```

而 `sub_824CA848` 建 `Stream+144` / `Stream+148` 时，正是两份
**只含 `KeyFrameFlag` 的集合**（值分别为 `"True"` / `"False"`）。
所以第 6 个参数是**逐样本的关键帧标记集合**，不是流的编解码属性。

实测：

| 第 6 个参数 | 结果 |
|---|---|
| `nullptr` | `OnGetSampleCompleted hr=0x80004003`（E_POINTER），**不崩** |
| 流的编解码属性 | 崩溃 |
| 集合级属性 | 崩溃 |
| 只含 `KeyFrameFlag` 的集合（True/False 两份） | 仍崩（`read=0x10c`） |

即：null 会被 XAV 提前拒绝，任何非空集合都会继续往下走并在**解码/输出格式**
环节崩溃——说明崩溃不在参数校验，而在 XAV 自己的解码输出路径。

> 日志采集注意：XBDM 的 debugstr 会**丢行**（`XAV draw` 每 120 帧一条，
> 12 秒里只收到 2 条），所以"某条日志没出现"不能当作"代码没执行"的证据。
> 需要可靠日志时应改为写入文件。

### 21.13 文件日志（`game:\xav_log.txt`）的可靠结论（2026-10-19）

新增 `xbox360_xav_log.{hpp,cpp}`（每行 fopen/append/fclose，崩溃后仍可读），
把关键路径同时写文件。一次崩溃前（程序只活约 100 ms，随后自动重启）的完整记录：

```
SetCallback vtbl=0x82076cc8 s0=0x835c7e60 s2=0x835c7fe8 s8=0x835c4460
OpenMedia streams=1 hr=0x00000000
Seek 0 queue=117
deliver #1 size=14857 ts=0
acquireFrame hr=0x8000000a tex=0 state=5      ← E_PENDING：还没有帧
hook postRender #1
hook preRender #1
acquireFrame hr=0x8000000a tex=0 state=5
hook postRender #2
hook preRender #2
```

由此确认：

1. **`XAVPreRender`/`XAVPostRender` 每帧都在正常调用**（此前"钩子没跑"的判断是丢行导致的误判）；
2. `GetTexture` 返回 `E_PENDING`，即 XAV 自己也没有帧可给；
3. `GetSample` 依旧从未被调用；
4. 崩溃发生在 `deliver` 之后约 80 ms，在 XAV 的解码线程上。

结论：**接口层已经完全打通，卡点是 XAV 的 AVC 解码器不产出帧**。
最可能的原因是样本的比特流封装形式与它的期望不符（Annex-B / AVCC /
"仅 VCL + CodecPrivateData 里的 SPS/PPS" 三种都试过前两种，均崩）。
下一步应导出参考实现（YouTube）实际送进 `OnGetSampleCompleted` 的
`IPALStream` 内容做逐字节对比——那是唯一能确定封装形式的办法。

---

## 22. 修正记录（2026-10-19）：回调表与 `GetSample` 参数

§21 的结论已被推翻：接口层并没有打通，缺了两个回调，并且 `GetSample`
的参数类型判断错误。

### 22.1 完整的回调接口（12 槽）

从 `00663_mfmediasourcewrapper.obj.o` 的
`??_7CMFMediaSourceWrapper##6BIXcpManagedMediaStreamSourceCallback###`（偏移
`0xbcf8`）读出：

| 槽 | 方法 | 说明 |
|---|---|---|
| 0 | `OnOpenMediaCompleted(collection, count, descs)` | 必需；collection 为 NULL 会返回 `E_POINTER` |
| 1 | `OnSeekCompleted(__int64 position)` | **必需**，见 §22.2 |
| 2 | `OnGetSampleCompleted(desc, pal, off, len, ts, attrs)` | 样本投递 |
| 3 | `OnGetSampleProgress(float)` | 缓冲进度 |
| 4 | `OnGetDiagonosticCompleted(kind, __int64)` | |
| 5 | `OnSwitchMediaStreamCompleted(desc)` | |
| 6 | `OnMediaStreamEnded(desc)` | **必需**，见 §22.3 |
| 7 | `OnMediaStreamError(int, wchar_t*)` | |
| 8 | `OnClose()` | |
| 9 | `OnBeginDrmSetupDecryptor(IPALMemory*, GUID*, u32)` | |
| 10 | `OnGetMaxDecryptorCount(u32*)` | |
| 11 | `OnStreamTick(desc, __int64)` | |
| 12 | `OnRateChangeRequest(int)` | |

### 22.2 不回调 `OnSeekCompleted` 的后果

`CMFMediaSourceWrapper::OnSeekAsyncCompleted` 负责为每个流创建
`CMFMediaStream`，并写 `streams[i] + 0x10C = 1`（clean-point 待标记）。

不回调槽 1 时：

- `streams[0]` 始终为 NULL；
- 主动投递样本 → `CMFMediaSourceWrapper::SetCleanPointOnSample`（`+0x10`）
  执行 `*(streams[index] + 0x10C)`，读 `0x0000010C` 崩溃；
- MF 管线永远不请求样本 → `GetSample` 从未被调用。

一个缺失的回调同时解释了「崩溃」和「`GetSample` 不调用」两个现象。

### 22.3 不回调 `OnMediaStreamEnded` 的后果

解码器有重排序缓冲，流结束后必须通过槽 6 通知 XAV，否则解码器不会被
flush，短流的帧永远出不来。实测补上后播放器状态从 5
（`XAVPlayerStateStarting`）推进到 6（`XAVPlayerStatePlaying`）。

### 22.4 `GetSample` 的参数是 `MediaStreamType`

`CMFMediaSourceWrapper::DoGetSample(MediaStreamType)`（虚表槽 18）把枚举
原样透传给 `this[88]` 的第 5 号方法，所以源侧收到的是 **0 = 音频、
1 = 视频**，而不是 `IXcpManagedMediaStreamDescription*`。实测：

```
GetSample #1 desc=00000001 video=413C7AD0 audio=00000000 vq=71
```

按指针比较会返回 `E_INVALIDARG`，队列里的样本一个都送不出去。

### 22.5 媒体类型的构造（已核实正确）

`CreateMFVideoMediaTypeFromMediaStreamDescription`（`00661_sourceapi.obj.o`）
读取流属性：

| 属性 | 处理 |
|---|---|
| `VideoFourCC` | 4 个宽字符按小端拼成 `biCompression` |
| `Width` | `wtol(...)` → `biWidth` |
| `Height` | `wtol(...)` → `biHeight` |
| `CodecPrivateData` | `base16decode` → 追加在 `VIDEOINFOHEADER` 之后 |

`biPlanes = 1`、`biBitCount = 24`，最后
`MFCreateVideoMediaTypeFromVideoHeader(header, 88 + cbCodec, 1, 1, 2, ...)`。

### 22.6 当前状态

协议层已端到端跑通：无崩溃、250 个样本全部投递且被完整读取、播放器进入
`Playing`，但解码器仍不产出帧。

`H264Decode*` 来自 `dash.ExtraAVCodecs.xex` 的 `H264_XBOX` 段，经
`InitExtraAVCodecs` 填充的函数表转发（`H264DecodeCreateEx` = 表项 `+260`；
表为空时它返回 NULL）。`XAVInitialize` 内部已经调用
`ExtraAVCodecs_Init()`，但它的返回值只存进 `g_hrAVInit`，不影响
`XAVInitialize` 的返回，所以失败时看不出来。

待查：解码器为何接受全部数据却不输出。

---

## 23. 结论（2026-10-19）：XAV 的 H.264 解码不可用

协议层已经打通，但解码器不产出帧。三组独立测试结果一致：

| 测试 | 环境 | 结果 |
|---|---|---|
| wiliwili 私有源 | 开发机 | 250 个样本全部投递且被 `pal Read` 读走，`texture=0`，零帧 |
| wiliwili 私有源 | 另一台零售/RGH 主机 | 同上：`delivered=250`、`texture=0`、`state=5` |
| 官方 XDK `SimpleXAV` | 开发机 | 用 XAV 自己的解复用器读 MP4，`state=4`、`dirty=0`、`pos=0`，零帧 |

第二条排除了「我们的代码有问题」，第三条排除了「样本封装格式不对」——
官方样例走的是 XAV 自己的解复用器，同样解不出画面。

### 23.1 已排除的原因

- **codec pack 缺失**：`CodecPack02.xex` 已确认加载
  （`AutoDownloadCodecPack = TRUE` 时 modload 可见）。注意该属性不是「是否弹
  下载提示」，而是「是否把 codec pack 载入进程」——设为 `FALSE` 时整个包不会
  加载，这一点在 `SimpleXAV` 上做过 A/B 验证。
- **解复用失败**：`res=320x176`、`dur=100266666` 都能正确解析。
- **音频分支**：补上 `IXAudio2` + `CreateMasteringVoice` 后构建阶段的崩溃消失，
  但画面依旧没有。
- **`Play()` 失败**：`Play()` 返回 `S_OK`，`XAVOpenParamAutoPlay = TRUE`，
  状态仍停在 `Stopped`。
- **应用侧暂停**：日志中没有任何 `setPaused(1)` 调用。

### 23.2 仍然未知的部分

`H264Decode*` 来自 `dash.ExtraAVCodecs.xex` 的 `H264_XBOX` 段，经
`InitExtraAVCodecs` 填充的函数表转发（`H264DecodeCreateEx` = 表项 `+260`）。
解码器接受了全部输入（`pal Read` 逐样本读满）却不产出任何帧，具体原因没有
定位到——需要逆向该模块，或确认它是否需要 PlayReady/DRM 初始化路径。

### 23.3 处理方式

- `XBOX360_XAV` 保持默认 `OFF`，默认构建完全不受影响。
- 开启该选项时，设置页的「硬件解码」开关在 Xbox 360 上**隐藏**，
  避免用户打开一个不可能工作的功能。
- 相关代码（私有源实现、代理、诊断日志）保留在树内，作为后续研究的基础。

### 23.4 顺带确认的可用方案：PC 端转发代理

XAV 的公开 `Open(url)` 无法访问 B 站 CDN，因为 `XAVOpenParam` 里没有设置
`Referer` / `User-Agent` 的选项，而 CDN 强制要求两者。用 PC 端代理补头是可行的：

```
XAV → http://<pc>:8899/stream → 代理补 Referer/UA → CDN
```

代理必须支持 HTTP Range（XAV 会去读 MP4 尾部的 `moov`），
标准库 `http.server` 不支持，会导致 `NS_E_INVALID_DATA`。
实测代理方案能让 XAV 正确解析 MP4（`state=4`、尺寸/时长正确），
只是最终仍然解不出帧——与上面的结论一致。
