# Xbox 360 M0 基线记录

更新时间：2026-09-08

## 已确认

- OXDK 源码：`H:\OXDK`
- Xbox 360 参考工程：`H:\bili360-main`
- Xenon FFmpeg 参考树：`H:\ffmpeg-xenon`
- OXDK Xbox 360 构建入口：`H:\OXDK\xbox360\xbox360.mk`
- OXDK 样例：`H:\OXDK\xbox360\samples\d3dclear`、`abitest`、`title`
- XBDM 工具脚本：`H:\OXDK\xbox360\tools\xbdm\xbdm.py`

## 当前阻塞

已配置的 Microsoft Xbox 360 XDK：`D:\Microsoft Xbox 360 SDK`，且已确认
`lib/xbox/xboxkrnl.lib`（221440 bytes）存在。OXDK doctor 已能识别该 XDK。
patched LLVM/Xenon ABI 工具链已在 WSL Debian 中完成编译，安装目录为
`/root/oxdk-llvm/build`（由用户确认编译完成）。需在 WSL 资源恢复后运行
doctor 和样例构建完成最终验收。

当前 PowerShell 环境也未提供可用的 POSIX `sh`/`make` 命令；OXDK 的
`./oxdk doctor` 和样例 Makefile 需要在 MSYS2、Git Bash（带 make）或 Linux
环境中运行。不要把 PC D3D9 或普通 Windows clang 结果当作 Xbox 360 验收。

## 补齐环境后的复测命令

在 POSIX shell 中：

```sh
cd /path/to/OXDK
export XDK_DIR=/path/to/extracted/XDK
./oxdk doctor
make -C xbox360/samples/abitest
make -C xbox360/samples/d3dclear
make -C /path/to/bili360-main -f <project-makefile> BUILD=Release
```

实机部署（将 `<host>` 替换为开发机/实机 XBDM 地址）：

```sh
python3 xbox360/tools/xbdm/xbdm.py <host> deploy xbox360/samples/d3dclear/default.xex 'Hdd1:\\m0\\default.xex'
python3 xbox360/tools/xbdm/xbdm.py <host> cmd 'magicboot title="Hdd1:\\m0\\default.xex" directory="Hdd1:\\m0"'
python3 xbox360/tools/xbdm/xbdm.py <host> listen
python3 xbox360/tools/xbdm/xbdm.py <host> screenshot m0-d3dclear.png
```

## M0 验收记录模板

| 项目 | 状态 | 证据 |
|---|---|---|
| `oxdk doctor` 找到 XDK | PASS | `D:\Microsoft Xbox 360 SDK` |
| Xenon ABI clang 构建 | PASS | WSL `/root/oxdk-llvm/build` |
| 最小 XEX 启动/退出 | PASS (external) | 朋友环境/Xenia 运行 `abitest`，截图显示 `ALL CHECKS PASSED` |
| `d3dclear` 样例 | PASS (build) | 生成 `xbox360/samples/d3dclear/default.xex` |
| `bili360-main` Release | TODO | 需确认其 VS/XDK 配置 |
| FFmpeg/XDK/编译器版本固定 | PARTIAL | XDK 与 LLVM 路径已固定；版本记录待补 |

## 外部运行证据

`abitest` 已在外部 Xenia/采集卡环境启动，画面显示 `XENON ABI`，并通过
64-bit unsigned/signed/hex、64-bit 变参和 double 全部检查（`ALL CHECKS PASSED`）。
本机没有对应 XDK/Xenia 运行环境，因此该项证据来自协作者设备；`d3dclear`
仍只有本地 XEX 构建证据，待后续获得运行截图或 XBDM 日志。
