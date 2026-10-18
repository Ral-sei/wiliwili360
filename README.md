<h1 align="center">wiliwili Xbox 360 移植版</h1>

<p align="center">
  把 <a href="https://github.com/xfangfang/wiliwili">wiliwili</a> 带到 Xbox 360 的分支
</p>

<p align="center">
  <a href="https://github.com/Ral-sei/wiliwili360/releases"><img src="https://img.shields.io/github/v/release/Ral-sei/wiliwili360?label=release" alt="release"></a>
  <img src="https://img.shields.io/badge/-Xbox%20360-107C10?style=flat&logo=xbox" alt="Xbox 360">
  <img src="https://img.shields.io/badge/toolchain-OXDK-blue" alt="OXDK">
</p>


## 下载

前往 **[Releases](https://github.com/Ral-sei/wiliwili360/releases)** 下载。

1. 下载最新 release 中的 Xbox 360 压缩包并解压；
2. 将解压出的 `wiliwili.xex` 和 `resources` 目录放在**同一个目录**下；
3. 把该目录放入主机硬盘（例如 `Hdd1:\wiliwili\`），通过 Aurora / XeXmenu / FSD 启动；

配置文件与缓存位于 `game:\config\wiliwili\`，删除该目录可恢复默认设置。

## 当前状态

已经打通的链路：

- **界面**：XDK D3D9 + NanoVG 渲染，XML 布局、字体、图片、焦点导航均可用；
- **网络**：XNet + libcurl + mbedTLS，WBI 签名、首页推荐、搜索、视频详情、`playurl` 等接口可用；
- **播放**：libmpv 0.36 + FFmpeg 4.4.8 软件渲染，YUV 由 GPU 上屏，点播 720p 实机可达约 30fps 视频 + 53–60fps UI；
- **输入**：XInput 手柄导航与播放器按键；
- **其它**：扫码登录、历史、收藏、直播、弹幕等页面已接入，部分功能仍在校验；

已知限制（仍在补齐）：

- DLNA 投屏不支持；
- TLS 证书校验暂时关闭（尚未内置 CA bundle）；
- XAV 硬件解码为实验功能且当前工具链不可用，默认关闭，仅软件解码；
- 镜像画面、着色器、均衡器不可用。

更多细节与实机记录见 [`docs/XBOX360_PORTING_PLAN.md`](docs/XBOX360_PORTING_PLAN.md) 及其配套文档。

## 编译

本移植使用 [OXDK](https://github.com/MrMilenko/OXDK) 提供的 Xenon ABI 工具链交叉编译，
产物是 Xbox 360 可执行文件 **XEX**，不是 Windows 的 exe。

### 依赖

| 依赖 | 说明 |
|---|---|
| [OXDK](https://github.com/MrMilenko/OXDK) | 链接脚本、`oxdklink`、Xenon 运行库与 cshim |
| Xbox 360 XDK | 需自行安装（不可再分发），提供 `xboxkrnl.lib`、`d3d9.lib` 等 |
| OXDK LLVM | 带 Xenon ABI 补丁的 clang/lld，默认 `$HOME/oxdk-llvm/build` |
| FFmpeg 4.4.8 | 已随仓库提供：`third_party/ffmpeg-4.4.8`（含 Xbox 补丁，已入库） |
| cbffmpeg | 外部依赖树：`libmpv.a`、libass/libxml2、compat 层，以及 mpv 的构建脚本 |
| 构建机 | Debian/Ubuntu 或 WSL（需要 `cmake`、`ninja`、`python3`、`make`） |

> XDK 安装路径通常带空格，构建脚本会在构建目录里创建一个无空格软链，无需手动处理。

### 准备 FFmpeg / libmpv

`scripts/build_xbox360_linux.sh` **只编译 wiliwili 本体**，不会编译 FFmpeg 和 mpv。
FFmpeg 源码已随仓库提供（`third_party/ffmpeg-4.4.8`），但静态库需要先构建一次；
mpv 的源码与构建脚本在外部 cbffmpeg 仓库中。构建脚本会自动把 `FFMPEG_SRC` 指向
`third_party/ffmpeg-4.4.8`、`FFMPEG_BUILD_DIR` 指向 `build/ffmpeg-x360-tls`，
并在静态库缺失时打印下面的构建命令后退出。

**1. FFmpeg 4.4.8**（源码已入库，只需构建一次）

```sh
sh scripts/ffmpeg_x360_configure_local.sh
make -C build/ffmpeg-x360-tls -j$(nproc)
```

`third_party/ffmpeg-4.4.8` 已包含 `scripts/xbox360/ffmpeg/ffmpeg.patch` 的全部改动
（零售机必需的 TCP socket SNL 标记 `0x5801`、XDK `hostent` 布局修正、mbedTLS 握手修复）。
如果重新解压源码，需要重新打补丁：

```sh
cd third_party/ffmpeg-4.4.8
patch -p1 < ../../scripts/xbox360/ffmpeg/ffmpeg.patch
```

**2. libmpv 0.36.0**（在 cbffmpeg 目录内构建）

```sh
cd /path/to/cbffmpeg
scripts/mpv_x360_configure_p4.sh
scripts/mpv_x360_build_p4.sh
scripts/mpv_x360_compat_archive.sh
```

产物：

```text
cbffmpeg/build/mpv-x360-p4/libmpv.a
cbffmpeg/build/compat-x360/libmpv_xbox360_compat.a
cbffmpeg/build/libass-x360/lib/*.a
cbffmpeg/build/libxml2-x360/lib/libxml2.a
```

仓库不包含 mpv 源码（`mpv-*` 仍在 `.gitignore` 中），构建由外部 cbffmpeg 提供。

**3. 路径覆盖**

| 变量 | 默认值 |
|---|---|
| `CBFFMPEG_DIR` | `../cbffmpeg` |
| `FFMPEG_SRC` | `<repo>/third_party/ffmpeg-4.4.8` |
| `FFMPEG_BUILD_DIR` | `<repo>/build/ffmpeg-x360-tls` |
| `MPV_BUILD_DIR` | `$CBFFMPEG_DIR/build/mpv-x360-p4` |

`scripts/ffmpeg_x360_configure_local.sh` 还支持 `FFMPEG_SRC`、`FFMPEG_BUILD`、
`FFMPEG_COMPAT`、`OXDK`、`XDK`、`LLVM`、`PKG_CONFIG_LIBDIR` 等环境变量，
也接受上面一键构建使用的 `OXDK_DIR`、`XDK_DIR`、`OXDK_LLVM` 命名
（脚本内默认值是作者本机路径，务必按需覆盖）。它依赖
`cbffmpeg/compat/xbox360` 与 libxml2 的 `.pc` 文件：

```sh
export FFMPEG_COMPAT=/path/to/cbffmpeg/compat/xbox360
export PKG_CONFIG_LIBDIR=/path/to/cbffmpeg/build/libxml2-x360/lib/pkgconfig
```

### 构建

确保上面的静态库已就绪：

```sh
git clone --recursive https://github.com/Ral-sei/wiliwili360.git
cd wiliwili360

export OXDK_DIR=/path/to/OXDK
export XDK_DIR=/path/to/xdk360          # 含 include/xbox 的 XDK 根目录
export OXDK_LLVM=$HOME/oxdk-llvm/build
export CBFFMPEG_DIR=/path/to/cbffmpeg  # 需先构建出 P4 静态库

scripts/build_xbox360_linux.sh
```

产物：`build-xbox360-linux/wiliwili`（XEX），完整日志在 `build-xbox360-linux/build.log`。

`build_xbox360_linux.sh` 支持用环境变量覆盖路径与并行度：

```sh
BUILD_DIR=build-xbox360 JOBS=8 BUILD_TYPE=Release scripts/build_xbox360_linux.sh
```

### 手动构建

```sh
cmake -S . -B build-xbox360 -G Ninja \
  -DPLATFORM_XBOX360=ON \
  -DMPV_SW_RENDER=ON \
  -DDISABLE_OPENCC=ON \
  -DDISABLE_WEBP=ON \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build-xbox360 --target wiliwili -j$(nproc)
```

需要注意：

- `LLD` 必须指向 cbffmpeg 的 `compat/xbox360/x360-ld.sh`（构建脚本会自动探测）。
  它给 `ld.lld` 追加 `--wrap=getsockopt --wrap=WSAStartup`，否则 mpv 的 http/tcp 会以
  10042/10057 全线失败；
- FFmpeg/mpv 归档路径可用 `FFMPEG_BUILD_DIR`、`MPV_BUILD_DIR` 覆盖；任一归档缺失会直接
  在 configure 阶段 `FATAL_ERROR`，不会链出一个缺符号的 XEX；
- `third_party/` 已内置 Xbox 版 curl 与 mbedTLS，`BILI360_DIR` 仅作为回退。

### 可选构建开关

| 开关 | 默认 | 说明 |
|---|---|---|
| `-DXBOX360_XAV=ON` | OFF | 实验性 XAV 硬件解码路径，当前受工具链阻塞，不可用 |
| `-DXBOX360_NANOVG_D3D9=ON` | OFF | 使用 vendored NanoVG D3D9 核心（默认用 borealis fork） |
| `-DXBOX360_RUNTIME_PROBES=ON` | OFF | 打开 `[x360-*]` 运行时诊断日志，排障时使用 |

更完整的依赖安装、故障排查与实机部署说明见
[`docs/XBOX360_DEBIAN_BUILD.md`](docs/XBOX360_DEBIAN_BUILD.md)。

---

# 原 README


<p align="center">
    <img src="resources/svg/cn.xfangfang.wiliwili.svg" alt="logo" height="128" width="128"/>
</p>
<p align="center">
  一个专为手柄用户设计的第三方 <a href="https://www.bilibili.com">B站</a> 客户端
</p>
<p align="center">
<b><a href="#特点">特点</a></b>
|
<b><a href="#安装">安装</a></b>
|
<b><a href="#文档">文档</a></b>
|
<b><a href="#开发">开发</a></b>
</p>

- - -

[![GitHub release (latest by date)](https://img.shields.io/github/v/release/xfangfang/wiliwili)](https://github.com/xfangfang/wiliwili/releases)
![GitHub All Releases](https://img.shields.io/github/downloads/xfangfang/wiliwili/total)
![GitHub stars](https://img.shields.io/github/stars/xfangfang/wiliwili?style=flat)
![GitHub forks](https://img.shields.io/github/forks/xfangfang/wiliwili)
[![Crowdin](https://badges.crowdin.net/wiliwili/localized.svg)](https://crowdin.com/project/wiliwili)
![NS](https://img.shields.io/badge/-Nintendo%20Switch-e4000f?style=flat&logo=Nintendo%20Switch)
![PSV](https://img.shields.io/badge/-PSVita-003791?style=flat&logo=PlayStation)
![PS4](https://img.shields.io/badge/-PS4-003791?style=flat&logo=PlayStation)
![MS](https://img.shields.io/badge/-Windows%207+-357ec7?style=flat&logo=Windows)
![mac](https://img.shields.io/badge/-macOS%2010.11+-black?style=flat&logo=Apple)
![Linux](https://img.shields.io/badge/-Linux-lightgrey?style=flat&logo=Linux&logoColor=white)
[![fedora](https://img.shields.io/badge/fedora-copr-blue?logo=fedora)](https://copr.fedorainfracloud.org/coprs/mochaa/wiliwili/)
[![Scoop Version (extras bucket)](https://img.shields.io/scoop/v/wiliwili?bucket=extras)](https://scoop.sh/#/apps?q=wiliwili)
[![aur](https://img.shields.io/aur/version/wiliwili?color=blue&logo=archlinux)](https://aur.archlinux.org/packages/wiliwili/)
[![Flathub](https://img.shields.io/flathub/v/cn.xfangfang.wiliwili)](https://flathub.org/apps/cn.xfangfang.wiliwili)
[![nightly.link](https://img.shields.io/badge/nightly.link-%E6%B5%8B%E8%AF%95%E7%89%88-green)](https://nightly.link/xfangfang/wiliwili/workflows/build.yaml/dev)
[![layout](https://img.shields.io/badge/wiliwili-自定义布局-yellow)](https://github.com/xfangfang/wiliwili_theme)

<br>

# 特点

wiliwili 拥有非常接近官方PC客户端的B站浏览体验  
同时支持**触屏**、**鼠标**、**键盘** 与 **手柄**操控  
无论是电脑还是游戏掌机都能获得全新的使用体验

多语言：简、繁、日、韩、英 ...   
搜索页：热搜 视频 番剧 影视  
筛选页：快速找到想看的影视内容  
动态页：关注的UP主最近视频动态  
直播页：关注的主播与其他系统推荐  
播放页：视频 番剧 电影 纪录片 综艺，支持弹幕与评论  
个人页：扫码登录 历史记录 个人收藏 我的追番 我的追剧  
主题色：拥有深浅两色主题，跟随系统自动切换

<br>

# 安装

### Nintendo Switch

1. 下载 `wiliwili-NintendoSwitch.zip`：[wiliwili releases](https://github.com/xfangfang/wiliwili/releases)
2. 将 wiliwili.nro 放置在**内存卡** `switch` 目录下。
3. 在主页 `按住` R键打开任意游戏进入 hbmenu，在列表中选择 wiliwili 点击打开即可。
4. [可选] 在应用内安装桌面图标，入口：设置/实用工具/使用教程

<details>

<br>

桌面图标会优先尝试打开 `switch/wiliwili.nro`，如果其不存在，则尝试打开 `switch/wiliwili/wiliwili.nro`，如果这两个路径都不存在，则打开
hbmenu 自行选择路径。

默认提供的为 OpenGL 版本，最高只能播放 4k@30，你也可以下载到支持原生图形 api
的 [deko3d 版本](https://nightly.link/xfangfang/wiliwili/workflows/build.yaml/dev)，可以流畅播放 4k@60，不过可能会偶尔崩溃。

</details>

### PSVita

下载 `wiliwili-PSVita.vpk` 安装即可：[wiliwili releases](https://github.com/xfangfang/wiliwili/releases)

开启硬解后可以流畅播放 720P 横屏视频，480P 竖屏视频，部分直播 1080P 原画。

### PS4

下载 `wiliwili-PS4.pkg` 安装即可：[wiliwili releases](https://github.com/xfangfang/wiliwili/releases)

只支持软解，如果想播放 4k@60 需要在设置中开启低画质解码。

### PC

PC客户端支持切换硬件解码、秒开流畅适合老电脑、支持鼠标操控（左键点击 右键返回 中键刷新）

下载对应系统的安装包运行即可：[wiliwili releases](https://github.com/xfangfang/wiliwili/releases)

> [!TIP]
> 现在 Linux & Steam Deck 用户可以通过系统自带的软件商店（如Discover、GNOME Software）搜索 `wiliwili` 进行下载。  
> 更多使用技巧请参考 [项目 WIKI](https://github.com/xfangfang/wiliwili/wiki)  

<br>

# 文档

在各位开发者的帮助下，wiliwili 支持了一系列包管理器，同时 wiliwili 还拥有丰富的自定义选项，包括：使用 Anime4K
提升观感，自定义字体及图标等等  
前往 [项目 WIKI](https://github.com/xfangfang/wiliwili/wiki) 查看更多使用技巧

<br>

# TODO list

如果你有其他改进的想法或创意，欢迎在讨论区交流：[Discussions](https://github.com/xfangfang/wiliwili/discussions/categories/ideas)

<details>

- [x] 初步完成底层基础组件、首页各类推荐视频、用户视频播放页
- [x] 微调页面、解决播放器启动速度慢、解决播放页面退出卡顿
- [x] 临时解决异步加载导致的空指针问题（图片异步加载某些情况还会出现问题，待修复）
- [x] 添加番剧/影视播放、添加扫码登录、播放历史、用户收藏夹（收藏夹相关部分工作不稳定）
- [x] 初步添加搜索
- [x] 播放页新增分集与UP主最新投稿
- [x] 完善视频播放页用户评论内容
- [x] 重构图片异步加载逻辑
- [x] 解决收藏夹、搜索页某些情况导致闪退的问题
- [x] 完善搜索页：番剧、影视 转为竖图
- [x] 完善播放页投稿列表：调整结构、自动加载下一页
- [x] 播放页展示合集与推荐
- [x] 添加动态页
- [x] 添加视频检索页
- [x] 完善设置页
- [x] 弹幕相关设置
- [x] 点赞、投币、收藏
- [x] 拖拽调节进度
- [x] 增加单手模式使用一个手柄来控制播放器
- [x] NSP forwarder自动检查多个位置的nro文件，避免无法打开
- [x] 增加设置使首页无法通过返回退出，避免误触
- [x] 使用教程添加未指明的快捷键说明
- [x] 重压摇杆临时快进
- [x] 支持切换按键图标
- [x] 应用内多语言切换
- [x] 重构搜索页面
- [x] 评论@显示不同颜色
- [x] 完善评论图片
- [x] 评论大表情包所在行增加行高
- [x] 支持webp图片
- [ ] 搜索支持搜索用户
- [ ] 长按一键三连
- [ ] 支持个人主页
- [ ] 评论跳转进度
- [ ] 评论跳转搜索
- [ ] 评论下方的更多信息 (up主点赞等内容)
- [ ] 投票评论
- [ ] 互动视频

</details>

<br>

# 反馈问题前要做的事

1. 网络相关的问题附加 `网络诊断截图`，入口：应用内设置/实用工具/网络诊断
2. [Switch用户] 要确保 `大气层`和`系统固件` 更新到 **最新** ，`内存卡`为 **FAT32**
3. [Switch用户] 如果打开应用黑屏时间过长，可以尝试删除内存卡目录 `config/wiliwili` 重新进入
4. 确保 `系统时间`正确、系统`网络设置`正确（主要是DNS）、如果使用了`网络代理`请在反馈前关闭并重新测试
5. 查找有没有其他人出现过类似的问题：[Issues](https://github.com/xfangfang/wiliwili/issues?q=is%3Aissue)
6. **完整且详细地** 描述你的问题，最好附加演示视频、截图。
7. 尝试复现问题，尽力找到BUG出现的规律

<br>

# 贡献

### 软件移植

本应用基于 nanovg 绘制界面，nanovg 底层可移植切换到任意图形库，已有 OpenGL/Vulkan/Metal 等支持。   
视频播放部分则使用 FFMPEG + MPV 绘制，默认使用 OpenGL，有 D3D11/Deko3d/Gxm 或软件渲染支持。  
触摸/按键/输入法等平台相关功能通过 GLFW 或 SDL 来支持，也可以脱离二者直接实现，比如 Gxm 版 PSV。

如果你要移植的设备支持 OpenGL(ES) 那么一般来说，直接编译就能正常运行。  
如果你要移植的设备使用其他底层图形库，那么首先需要移植 nanovg，这可以确保应用主要界面正常，
其次为了更好的性能表现需要 ffmpeg 的硬解和 mpv 的渲染支持。  

如果你有想要移植的设备欢迎发一条 issue 讨论，Android / iOS 不在讨论之内。

### 新功能

如果你有想完成的创意，请在开发前发布一个 issue 讨论，避免和别人的创意撞车浪费了时间

### 多语言支持

如果你想为软件添加多语言的翻译支持，或者发现了某些翻译存在问题需要订正，请查看 [#52](https://github.com/xfangfang/wiliwili/issues/52)
了解如何贡献翻译

### 代码分支

主分支 yoga 为最新版本的代码  
开发分支 dev 为正在开发中的代码，任何新的 PR 都需要向 dev 分支提交

<br>

# 开发

```shell
# 拉取代码
git clone --recursive https://github.com/xfangfang/wiliwili.git
cd wiliwili
```

### PC本地运行

目前 wiliwili 支持运行在 Linux macOS 和 Windows上

<details>

#### macOS

```shell
# macOS: install dependencies
brew install mpv webp

cmake -B build -DPLATFORM_DESKTOP=ON
make -C build wiliwili -j$(sysctl -n hw.ncpu)
```

#### Linux

不同 Linux 的编译过程或依赖可能不同，这里是一份总结：[#89](https://github.com/xfangfang/wiliwili/discussions/89)

欢迎在上面的链接中写出你所使用系统的编译过程供大家参考。

```shell
# Ubuntu: install dependencies
sudo apt install libssl-dev libmpv-dev libwebp-dev

cmake -B build -DPLATFORM_DESKTOP=ON
make -C build wiliwili -j$(nproc)
```

```shell
# 如果你想安装在系统路径，并生成一个桌面图标，请使用如下内容编译
cmake -B build -DPLATFORM_DESKTOP=ON -DINSTALL=ON -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX:PATH=/usr
make -C build wiliwili -j$(nproc)
sudo make -C build install

# uninstall (run after install)
sudo xargs -a build/install_manifest.txt rm
```

#### Windows

```shell
# Windows: install dependencies (MSYS2 MinGW64)
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-make \
  git mingw-w64-x86_64-mpv mingw-w64-x86_64-libwebp

cmake -B build -G "MinGW Makefiles" -DPLATFORM_DESKTOP=ON
mingw32-make -C build wiliwili -j$(nproc)
```


</details>

### 交叉编译 Switch 可执行文件 (wiliwili.nro)

推荐使用docker构建，本地构建配置环境略微繁琐不过可用来切换底层的ffmpeg或mpv等其他依赖库更灵活地进行调试。

<details>

以下介绍 OpenGL 下的构建方法，deko3d (更好的硬解支持)请参考：`scripts/build_switch_deko3d.sh`

#### Docker

```shell
docker run --rm -v $(pwd):/data devkitpro/devkita64:20251117 \
  bash -c "/data/scripts/build_switch.sh"
```

#### 本地编译

```shell
# 1. 安装devkitpro环境: https://github.com/devkitPro/pacman/releases

# 2. 安装依赖
sudo dkp-pacman -S switch-glfw switch-libwebp switch-cmake switch-curl devkitA64

# 3. 安装自定义依赖
# devkitpro提供的提供的 ffmpeg/mpv 无法播放网络视频
# 手动编译方法请参考：scripts/README.md
base_url="https://github.com/xfangfang/wiliwili/releases/download/v0.1.0"
sudo dkp-pacman -U \
    $base_url/switch-ffmpeg-7.1-1-any.pkg.tar.zst \
    $base_url/switch-libmpv-0.36.0-3-any.pkg.tar.zst

# 4. build
cmake -B cmake-build-switch -DPLATFORM_SWITCH=ON
make -C cmake-build-switch wiliwili.nro -j$(nproc)
```

</details>

### 交叉编译 PSV 可执行文件

使用本地环境来编译可以参考：
 - [borealis 编译指南](https://github.com/xfangfang/borealis/wiki/PS-Vita)
 - [wiliwili vita 编译指南](https://gist.github.com/xfangfang/305da139721ad4e96d7a9d9a1a550a9d)

注意: 我们使用自定义的 mbedtls, curl 和 ffmpeg 作为依赖, 在使用本地环境编译时，请先卸载 vitasdk 中的相关库，再安装[指定的](https://github.com/xfangfang/wiliwili/tree/yoga/scripts/psv)依赖.

<details>

```shell
# 构建 OpenGL ES 2.0 版
docker run --rm -v $(pwd):/src/ xfangfang/wiliwili_psv_builder:latest \
    "cmake -B cmake-build-psv -G Ninja -DPLATFORM_PSV=ON \
        -DMPV_NO_FB=ON -DUSE_SYSTEM_CURL=ON -DUSE_SYSTEM_SDL2=ON \
        -DCMAKE_BUILD_TYPE=Release && \
        cmake --build cmake-build-psv"

# 构建 Gxm 版 (推荐)
docker run --rm -v $(pwd):/src/ xfangfang/wiliwili_psv_builder:latest-gxm \
    "cmake -B cmake-build-psv -G Ninja -DPLATFORM_PSV=ON \
        -DUSE_SYSTEM_CURL=ON -DUSE_GXM=ON -DUSE_VITA_SHARK=OFF \
        -DCMAKE_BUILD_TYPE=Release && \
        cmake --build cmake-build-psv"
```

</details>

### 交叉编译 PS4 可执行文件

使用本地环境来编译可以参考: 
 - [PacBrew 环境安装](https://github.com/PacBrew/pacbrew-packages)
 - [borealis 编译指南](https://github.com/xfangfang/borealis/wiki/PS4)
 - [编译 wiliwili 依赖的第三方库](https://github.com/xfangfang/wiliwili/blob/dev/scripts/ps4/Dockerfile)

<details>

```shell
docker run --rm -v $(pwd):/src/ xfangfang/wiliwili_ps4_builder:latest \
    "cmake -B cmake-build-ps4 -DPLATFORM_PS4=ON \
        -DMPV_NO_FB=ON \
        -DUSE_SYSTEM_CPR=ON && \
        make -C cmake-build-ps4 -j$(nproc)"
```

</details>

### GLFW or SDL

wiliwili 使用 nanovg 绘制图形和文字，对于创建窗口、按键触摸、输入法等支持是通过 GLFW(默认) 或 SDL 完成的。

因为 GLFW 支持平台有限，在移植到新平台时可以使用 SDL 或者自行实现上述对应接口。

```shell
# 示例
cmake -B build -DPLATFORM_DESKTOP=ON -DUSE_SDL2=ON
cmake --build build
```

<br>

# 应用截图

<p align="center">
<img src="docs/images/screenshot-3.jpg" alt="screenshot">
<img src="docs/images/screenshot-4.jpg" alt="screenshot">
</p>

# Acknowledgement

The development of wiliwili cannot do without the support of the following organization and open source projects.

- Toolchain: devkitpro, switchbrew, vitasdk OpenOrbis and PacBrew
    - https://github.com/devkitPro
    - https://github.com/switchbrew/libnx
    - https://github.com/vitasdk
    - https://github.com/OpenOrbis
    - https://github.com/PacBrew
- UI Library: natinusala and XITRIX
    - https://github.com/natinusala/borealis
    - https://github.com/XITRIX/borealis
- Video Player: Cpasjuste, proconsule, fish47 and averne
    - https://github.com/Cpasjuste/pplay
    - https://github.com/proconsule/nxmp
    - https://github.com/fish47/FFmpeg-vita
    - https://github.com/fish47/mpv-vita
    - http://github.com/averne/FFmpeg
    - http://github.com/averne/mpv
- Misc
    - https://github.com/libcpr/cpr
    - https://github.com/nlohmann/json
    - https://github.com/nayuki/QR-Code-generator
    - https://github.com/BYVoid/OpenCC
    - https://github.com/imageworks/pystring
    - https://github.com/sammycage/lunasvg
    - https://github.com/cesanta/mongoose
    - https://chromium.googlesource.com/webm/libwebp
    - https://github.com/fancycode/MemoryModule

# Special thanks

- Thanks to Crowdin for supporting [open-source projects](https://crowdin.com/page/open-source-project-setup-request).
