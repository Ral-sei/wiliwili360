# M1 NanoVG D3D9 后端审计

状态：已完成 XDK/Xenon 编译、shader 生成、独立 XEX 链接，并由真实 XDK 开发环境确认 NanoVG 图形输出

## 依赖版本

- 上游：`https://github.com/codecat/nanovg_d3d`
- 本地目录：`library/borealis/library/lib/extern/nanovg_d3d`
- 当前提交：`488f7ab` (`Fixed texture loading on d3d9`)
- 获取方式：通过 `https://gh-proxy.org/` 克隆

## 已确认内容

上游包含独立的 D3D9 NanoVG 实现：

- `src/nanovg_d3d9.h`：公开 `nvgCreateD3D9(IDirect3DDevice9*, int)` 和 `nvgDeleteD3D9`。
- `src/nanovg_d3d9.h`：实现 `NVGparams` 的纹理、viewport、flush、fill、stroke 和 triangles 回调。
- `src/D3D9VertexShader.h`、`D3D9PixelShader.h`、`D3D9PixelShaderAA.h`：预生成 shader 字节码。
- `example/example_d3d9.c`：可作为绘制行为和资源生命周期的参考样例。

核心渲染路径使用 D3D9 常见接口：动态 vertex buffer、`D3DPOOL_DEFAULT` 纹理、`LockRect` 上传、`CreateVertexShader`/`CreatePixelShader`，以及 stencil、blend、scissor 状态切换。

## 不能直接用于 Xbox 360 的部分

上游示例不是 Xbox 360 平台层，依赖 Win32 窗口和 PC D3D9 初始化（`CreateWindowEx`、`Direct3DCreate9`、`d3d9`/`user32` 等）。这些内容不能进入 borealis Xbox 360 后端。

此外，PC 可接受的 D3D9 shader 字节码、纹理格式、pitch/对齐、资源池语义和 COM ABI 仍需在 XDK 上逐项确认。当前提交只能证明上游存在 D3D9 路径，不能证明 Xenon 兼容。

本地 XDK 文档 `microcode.htm` 将 Xbox 360 shader 指令集列为 `xvs_3_0` 和
`xps_3_0`；`graphics_tools_fxc.htm` 说明 XDK `fxc` 在 `/T vs_3_0` 和
`/T ps_3_0` 下默认生成对应的 Xenon microcode。因此仓库中上游提交带来的
`D3D9*Shader.h` 只能视为 PC 参考产物，不能直接用于 Xbox 360。

## M1 下一步（需要 XDK 后执行）

1. 用 XDK 头文件编译最小翻译单元，仅包含 `nanovg.h`、`nanovg_d3d9.h` 和一个 XDK 创建的 `IDirect3DDevice9`。
2. 将 shader 字节码通过 XDK shader 工具或离线流程重新验证；禁止依赖运行时 PC `D3DXCompileShader`。
3. 在独立样例中依次验证矩形、圆角、图片、文字、stencil stroke、scissor 和 alpha blend。
4. 记录 XDK 纹理格式、线性纹理/tiling、锁定 pitch、显存占用和 device 状态恢复结果。
5. 验证 480p、720p、1080p viewport 与连续运行 30 分钟的资源回收。

### Shader 生成

已使用 `D:\Microsoft Xbox 360 SDK\bin\win32\fxc.exe`（D3DX9 Shader Compiler
2.0.21256.0 Xbox 360）运行 `scripts/build_nanovg_d3d9_xbox360.sh`，以
`vs_3_0`/`ps_3_0` 目标生成三个 `D3D9*Shader.h`。Xenon fxc 不实现 AA shader
的 `discard`，因此 `D3D9PixelShaderAA.hlsl` 在 `XBOX360` 宏下返回透明像素，
桌面编译仍使用原始 `discard`。

## 当前结论

`nanovg_d3d` 已完成基础 Xbox 360 后端验证，可继续接入 borealis 平台层。仍需补齐纹理、字体、stencil 和状态恢复测试。

### 真实 XDK 验证结果

朋友使用真实 XDK/XBDM 启动 `xbox360/samples/nanovg_d3d9/default.xex`，画面成功显示深色背景和蓝色圆角矩形。调试输出包含 `context created; rendering` 与 `first NanoVG frame presented`。

这确认了 XDK device、Xenon shader、NanoVG vertex buffer、D3D draw call 和 Present 链路均可工作。日志中的 XNet/XAM 警告与该无网络图形样例无关。

## 已落地的编译接入

当 `PLATFORM_XBOX360=ON` 且 `XBOX360_NANOVG_D3D9=ON` 时，borealis 会：

- 使用 `nanovg_d3d/src/nanovg.c`，避免与 borealis NanoVG 核心重复导出符号；
- 将 `nanovg_d3d/src` 放到 include 搜索路径前端；
- 编译 `lib/platforms/xbox360/nanovg_d3d9_backend.cpp`，仅提供 D3D9 NanoVG 实现；
- 不选择 GLFW/SDL/Win32 窗口后端，设备创建和 Present 留给 M2 的 `Xbox360VideoContext`。

顶层 `CMakeLists.txt` 在 Xbox 360 配置下也会跳过 Desktop 的 libmpv/pkg-config 探测。

本机具备 XDK/OXDK 工具链但没有开发机；已使用 Xenon clang 成功编译
`nanovg_d3d9_backend.cpp`，未进行 XEX 部署或实机运行。

本次编译沿用 OXDK `d3dclear` 的 Xenon 参数：

```bash
clang++ -target powerpc-unknown-none-elf -c -ffreestanding -fno-builtin \
  -fms-extensions -fms-compatibility -fshort-wchar -fno-autolink \
  -D_WIN32 -D_M_PPCBE -D_M_PPC -D_PPC_ -D_XBOX -D_XENON -DXBOX \
  -I "$OXDK_DIR/xbox360/oxdk360/header-shim" \
  -I "$XDK_DIR/include/xbox" \
  -I "$XDK_DIR/TechPreview/Jul12Compiler/include/xbox" \
  -I library/borealis/library/lib/extern/nanovg_d3d/src \
  -I library/borealis/library/include \
  library/borealis/library/lib/platforms/xbox360/nanovg_d3d9_backend.cpp \
  -o nanovg_d3d9_backend.o
```

已处理的 Xenon 差异：

- XDK `d3d9.h` 将 `IDirect3DDevice9` 映射为 `D3DDevice`；wrapper 先包含 XDK 基础头文件。
- XDK 不提供桌面动态纹理、锁定和自动 mipmap 标志；Xbox 360 分支使用零标志。
- XDK 没有桌面 `D3DSAMP_SRGBTEXTURE` sampler state，因此 Xbox 360 分支不调用它。
- 已补齐当前 NanoVG core 所需的 `renderTriangles` `fringe` 参数。

这些修改只在 XDK/Xenon 宏下生效，桌面 D3D9 路径仍使用原始标志和 sampler state。
