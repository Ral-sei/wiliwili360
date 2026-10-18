#!/bin/sh
# P4: FFmpeg 4.4.8 的 Xbox 360 configure —— 打开网络栈 + DASH demuxer。
#
# 在 P3（scripts/ffmpeg_x360_configure_p3.sh，纯本地文件）的基础上扩展：
#   + --enable-network        P3 是 --disable-network。DASH 走 avio 读分片，
#                             CONFIG_NETWORK=1 才会编 network.o / os_support.o
#                             （ff_network_init、ff_poll、ff_getaddrinfo）。
#   + --enable-demuxer=dash   dash_demuxer_deps="libxml2"（configure:3323），
#                             分片本身仍由已启用的 mov/mpegts demuxer 解。
#   + --enable-libxml2        走 require_pkg_config libxml2 libxml-2.0
#                             （configure:6510），硬闸门，必须能答
#                             --exists/--cflags/--libs →
#                             --pkg-config 指向 scripts/x360-pkg-config.sh，
#                             PKG_CONFIG_LIBDIR 钉死成 Xbox 自己那份 .pc
#                             （宿主没装 pkg-config，装了也会捞到 x86_64 的
#                             libxml2，编译期"找到"、链接期才炸）。
#   - 其余 disable 面与 P3 完全一致：--disable-iconv、--disable-asm/altivec、
#     flip0 表（含 POLL_H）、protocol_list(game:) 补丁。
#
# 网络探测的现实（与 P2/P3 的"探测一律假阳性"同一根源，见
# scripts/x360-cc-fake.sh / x360-ld-fake.sh 注释）：
#   - check_func / check_func_headers 是 **链接** 探测（程序体只是
#     `extern int f();` + `return (long) f;`，不要求头文件里真有声明），
#     而 --ld=x360-ld-fake.sh 恒成功 → HAVE_GETADDRINFO / HAVE_INET_ATON
#     这类"XDK 里根本没有的符号"也会被判成 1 → 必须事后翻 0；
#     翻 0 之后 FFmpeg 才会启用自带兜底（os_support.c 的静态 inet_aton、
#     ff_getaddrinfo/ff_freeaddrinfo/ff_getnameinfo，network.h 的
#     `#define getaddrinfo ff_getaddrinfo`）。
#   - check_type / check_check_struct / check_headers 是 **编译** 探测，
#     结果是真的：XDK 的 winsock 是 winsockx.h 一个头打包到底，没有
#     poll.h / netdb.h / ws2tcpip.h / sockaddr_storage / socklen_t，
#     而宿主 /usr/include 这些头**排在搜索路径最后**（compat → header-shim
#     → XDK → … → /usr/include），会因 compat/features.h 空桩而编不过 ——
#     所以这些项天然就是 0，翻 0 表只是把"万一编过了"也钉死。
#
# 必须保持 1 的只有两项（winsock 分支的入场券）：
#   HAVE_WINSOCK2_H  configure:6079 先试 arpa/inet.h（失败），再试
#                    check_headers winsock2.h —— 失败就 disable network；
#                    且 network.h/os_support.c 的 winsock 错误码分支也靠它
#                    （XDK 只有 WSAGetLastError 语义，别无选择）。
#   HAVE_CLOSESOCKET XDK winsockx.h 真有 closesocket（xnet.lib 真有实现），
#                    否则 os_support.h 会 `#define closesocket close`，
#                    而 close 是 CRT 的文件句柄语义，接 socket 句柄必炸。
# 脚本末尾对这些不变量做硬断言，破了直接 exit 1。
#
# out-of-tree：源码树纯净，产物在 build/ffmpeg-x360-p4/。
#
#   sh scripts/ffmpeg_x360_configure_local.sh
#   make -C build/ffmpeg-x360-tls -j
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC=${FFMPEG_SRC:-$ROOT/third_party/ffmpeg-4.4.8}
BLD=${FFMPEG_BUILD:-$ROOT/build/ffmpeg-x360-tls}
COMPAT=${FFMPEG_COMPAT:-$ROOT/../cbffmpeg/compat/xbox360}
MBEDTLS=${MBEDTLS_DIR:-$ROOT/third_party/mbedtls}
MBEDTLS_CONFIG=${MBEDTLS_CONFIG_DIR:-$ROOT/third_party/mbedtls-xbox-config}
OXDK=${OXDK:-${OXDK_DIR:-/home/lbx/cb360/OXDK}}
XDK=${XDK:-${XDK_DIR:-/home/lbx/xdk360/XDK}}
LLVM=${LLVM:-${OXDK_LLVM:+${OXDK_LLVM}/bin}}
LLVM=${LLVM:-/home/lbx/oxdk-llvm/build/bin}
CLANG=$LLVM/clang

# libxml2 的 .pc 只认 Xbox 这份（scripts/libxml2_x360_build.sh 手写安装树）。
# 必须在 configure 之前 export —— FFmpeg 是在子 shell 里调 pkg-config 的，
# 读不到命令行上的临时变量。
export PKG_CONFIG_LIBDIR=${PKG_CONFIG_LIBDIR:-$ROOT/build/libxml2-x360/lib/pkgconfig}
[ -f "$PKG_CONFIG_LIBDIR/libxml-2.0.pc" ] || {
    echo "missing $PKG_CONFIG_LIBDIR/libxml-2.0.pc — 先跑 sh scripts/libxml2_x360_build.sh" >&2
    exit 1
}

# 与 OXDK/xbox360/xbox360.mk 的 OXDK360_CFLAGS 同步维护（同 P2/P3 脚本）。
CFLAGS="-target powerpc-unknown-none-elf -O2 -ffreestanding -fno-builtin -fms-extensions -fms-compatibility \
-fms-compatibility-version=14 -fshort-wchar -fno-autolink -fdelayed-template-parsing \
-Wno-invalid-token-paste -Wno-ignored-attributes -Wno-pragma-pack \
-D_WIN32 -D_M_PPCBE -D_M_PPC -D_PPC_ -D_XBOX -D_XENON -DXBOX \
-D_MSC_VER=1400 -D_MSC_FULL_VER=140050727 -D_SIZE_T_DEFINED -D_W64= \
-Xclang -target-feature -Xclang +xenon-abi \
-isystem$OXDK/xbox360/oxdk360/header-shim \
-I$COMPAT/include \
-isystem$XDK/include/xbox \
-isystem$XDK/TechPreview/Jul12Compiler/include/xbox \
-include $COMPAT/include/xbox_ffmpeg_prelude.h \
 -Dsnprintf=avpriv_snprintf -Dvsnprintf=avpriv_vsnprintf \
 -I$MBEDTLS_CONFIG -I$MBEDTLS/include \
 -D_XBOX"

mkdir -p "$BLD"
cd "$BLD"

"$SRC/configure" \
    --target-os=none \
    --arch=ppc \
    --enable-cross-compile \
    --cc="$CLANG" \
    --ld="$ROOT/scripts/x360-ld-fake.sh" \
    --ar="$LLVM/llvm-ar" \
    --ranlib="$LLVM/llvm-ranlib" \
    --nm="$LLVM/llvm-nm" \
    --extra-cflags="$CFLAGS" \
    --pkg-config="$ROOT/scripts/x360-pkg-config.sh" \
    --disable-autodetect \
    --disable-programs \
    --disable-doc \
    --disable-everything \
    --disable-avdevice \
    --disable-postproc \
    --disable-avresample \
    --enable-network \
    --enable-mbedtls \
    --enable-version3 \
    --enable-lzo \
    --disable-iconv \
    --enable-libxml2 \
    --enable-decoder=h264 \
    --enable-decoder=aac \
    --enable-parser=h264 \
    --enable-parser=aac \
    --enable-demuxer=mov \
    --enable-demuxer=h264 \
    --enable-demuxer=aac \
    --enable-demuxer=mpegts \
    --enable-demuxer=dash \
    --enable-demuxer=flv \
    --enable-demuxer=live_flv \
    --enable-demuxer=hls \
    --enable-protocol=file \
    --enable-protocol=tcp \
    --enable-protocol=http \
    --enable-protocol=tls \
    --enable-protocol=https \
    --enable-protocol=crypto \
    --enable-bsf=h264_mp4toannexb \
    --enable-bsf=aac_adtstoasc \
    --enable-filter=scale \
    --enable-filter=format \
    --enable-filter=aformat \
    --enable-filter=aresample \
    --enable-static \
    --disable-shared \
    --disable-asm \
    --disable-inline-asm \
    --disable-altivec \
    --disable-x86asm \
    --enable-pthreads \
    --disable-w32threads \
    --disable-os2threads \
    "$@"

# ---------------------------------------------------------------------------
# XDK 现实校正表（与 P2/P3 脚本同源维护，说明见 ffmpeg_x360_configure.sh）：
#   FCNTL MMAP MPROTECT SYSCTL SETRLIMIT GETRUSAGE FORK GETOPT GETHRTIME
#   POSIX_MEMALIGN MEMALIGN STRERROR_R ISATTY SYSCONF SCHED_GETAFFINITY USLEEP
#
# P3 追加（avformat/avfilter 引入的新面；原则：宿主系统存在、XDK 与 compat
# 都没有的头一律关掉——否则 #include 会解析到宿主 glibc 头，把 ino_t 之类
# 的类型定义带进来与 XDK 冲突，file.c 的 HAVE_DIRENT_H 已实测炸过）：
#   UNISTD_H       configure 误检通过，交叉编译拉宿主 unistd → swab 双声明
#   DIRENT         XDK 无 dirent.h（FileContext 的目录分支有 #if 保护）
#   POLL/TERMIOS   XDK 无；消费者在网络/设备组件
#   SYS_UN/SYS_SOUNDCARD/ASM_TYPES/LINUX_PERF_EVENT  Unix/设备专用
#   SYS_SELECT/SYS_PARAM/SYS_RESOURCE  XDK 无（select 消费者在网络组件，
#                  rlimit 走上表 SETRLIMIT=0；compat 有 sys/time.h →
#                  SYS_TIME 必须保持 1，av_gettime 依赖）
#
# P4 追加（网络栈；见脚本头部"探测现实"一节）。原则不变：翻 0 只在现值为 1
# 时动手，本来就是 0/未定义的项属于"照抄清单"，不产生副作用：
#   GETADDRINFO/INET_ATON   链接探测假阳性 → 必翻 0，换 FFmpeg 自带兜底
#   ARPA_INET_H             一旦 1，network.h 会 #include 宿主 arpa/inet.h，
#                           且 configure 走"优先 POSIX"分支绕开 winsock —— 与
#                           winsock2_h 分支互斥（configure:6079）
#   POLL_H/STRUCT_POLLFD    保持 0 → os_support.h 自定义 nfds_t/pollfd/POLL*，
#                           os_support.c 用 XDK 真有的 select() 实现 ff_poll()
#   STRUCT_ADDRINFO/SOCKLEN_T  保持 0 → network.h / os_support.h 的兜底定义
#   STRUCT_SOCKADDR_STORAGE/IN6/SA_LEN  保持 0 → network.h 自带 sockaddr_storage，
#                           sockaddr_union 不带 in6（XDK 无 AF_INET6）
#   STRUCT_MSGHDR_MSG_FLAGS/GROUP_SOURCE_REQ/IP_MREQ_SOURCE/IPV6_MREQ
#                           宿主 netinet/* 泄漏项，消费者（sctp/udp/ip）未启用
# ---------------------------------------------------------------------------

# 仅当现值为 1 时翻 0（FORK 等可能本来就是 0/未定义）
flip0() {
    for f in "$@"; do
        sed -i "s/^#define HAVE_$f 1\$/#define HAVE_$f 0/" config.h
    done
}

flip0 FCNTL MMAP MPROTECT SYSCTL SETRLIMIT GETRUSAGE FORK GETOPT \
      GETHRTIME POSIX_MEMALIGN MEMALIGN STRERROR_R ISATTY SYSCONF \
      SCHED_GETAFFINITY USLEEP UNISTD_H DIRENT_H POLL_H TERMIOS_H SYS_UN_H \
      SYS_SOUNDCARD_H ASM_TYPES_H LINUX_PERF_EVENT_H SYS_SELECT_H SYS_PARAM_H \
      SYS_RESOURCE_H

flip0 GETADDRINFO INET_ATON ARPA_INET_H \
      STRUCT_POLLFD STRUCT_ADDRINFO SOCKLEN_T \
      STRUCT_SOCKADDR_STORAGE STRUCT_SOCKADDR_IN6 STRUCT_SOCKADDR_SA_LEN \
      STRUCT_MSGHDR_MSG_FLAGS STRUCT_GROUP_SOURCE_REQ STRUCT_IP_MREQ_SOURCE \
      STRUCT_IPV6_MREQ

echo "XDK reality corrections applied:"
for f in FCNTL MMAP MPROTECT SYSCTL SETRLIMIT GETRUSAGE FORK GETOPT \
         GETHRTIME POSIX_MEMALIGN MEMALIGN STRERROR_R ISATTY SYSCONF \
         SCHED_GETAFFINITY USLEEP UNISTD_H DIRENT_H POLL_H TERMIOS_H \
         SYS_UN_H SYS_SOUNDCARD_H ASM_TYPES_H LINUX_PERF_EVENT_H \
         SYS_SELECT_H SYS_PARAM_H SYS_RESOURCE_H SYS_TIME_H; do
    printf '  HAVE_%-22s %s\n' "$f" "$(sed -n "s/^#define HAVE_$f //p" config.h)"
done

# config.h 里该符号可能是 `/* #undef HAVE_X */`（探测真失败）—— 统一读成 0，
# 断言才好写。
cfg() {
    v=$(sed -n "s/^#define HAVE_$1 //p" config.h)
    if [ -z "$v" ]; then echo 0; else echo "$v"; fi
}

echo "network pins:"
for f in WINSOCK2_H CLOSESOCKET GETADDRINFO INET_ATON ARPA_INET_H POLL_H \
         STRUCT_POLLFD STRUCT_ADDRINFO SOCKLEN_T STRUCT_SOCKADDR_STORAGE \
         STRUCT_SOCKADDR_IN6 STRUCT_SOCKADDR_SA_LEN STRUCT_MSGHDR_MSG_FLAGS \
         STRUCT_GROUP_SOURCE_REQ STRUCT_IP_MREQ_SOURCE STRUCT_IPV6_MREQ; do
    printf '  HAVE_%-26s %s\n' "$f" "$(cfg "$f")"
done

# ---------------------------------------------------------------------------
# 硬断言：破了就别往下 make，省一轮链接期才发现的糊涂账。
#   必须为 1 —— winsock 分支的入场券（理由见脚本头部）
#   必须为 0 —— 全是"宿主泄漏 / 假阳性"项，为 1 意味着 FFmpeg 会跳过自带
#               兜底，去链 XDK 里根本不存在的符号，或把 glibc 类型带进目标码
# ---------------------------------------------------------------------------
fail=0
for f in WINSOCK2_H CLOSESOCKET; do
    [ "$(cfg "$f")" = 1 ] || { echo "ASSERT FAIL: HAVE_$f=$(cfg "$f") (want 1)" >&2; fail=1; }
done
for f in GETADDRINFO INET_ATON ARPA_INET_H POLL_H STRUCT_POLLFD \
         STRUCT_ADDRINFO SOCKLEN_T STRUCT_SOCKADDR_STORAGE \
         STRUCT_SOCKADDR_IN6 STRUCT_SOCKADDR_SA_LEN; do
    [ "$(cfg "$f")" = 0 ] || { echo "ASSERT FAIL: HAVE_$f=$(cfg "$f") (want 0)" >&2; fail=1; }
done
[ "$fail" = 0 ] || exit 1

# ---------------------------------------------------------------------------
# game: URLProtocol 追加进生成的协议表（P3 引入，P4 沿用）：
#   FFmpeg 4.4 把 "game:\..." 判成 scheme=game 的 URL，注册表里只有 file →
#   avformat_open_input 报 Protocol not found（P3 首跑实录）。
#   4.4 没有运行时注册 API（avio_register_protocol 已删），协议表是 configure
#   生成的 static const 数组（build 产物，非源码），在此直接重写；
#   实现 ff_game_protocol 放 tests/p3_format/main.c（P4 libmpv 时期再挪到
#   libmpv 侧）。game: 协议本身不改源码树；但 libavformat 另有 3 处 Xbox
#   修复（tcp.c SNL 标记等）以源码补丁形式维护，见
#   scripts/xbox360/ffmpeg/ffmpeg.patch（重解压 FFmpeg 后需重新应用）。
# ---------------------------------------------------------------------------
PLIST="$BLD/libavformat/protocol_list.c"
# 第 4 步（真实 B 站 MPD）追加 http/tcp/tls/https。tls/https 由 FFmpeg
# 的 mbedTLS 后端提供，底层仍通过 Xbox 的 tcp 协议连接。
cat > "$PLIST" <<'EOF'
extern const URLProtocol ff_game_protocol;
static const URLProtocol * const url_protocols[] = {
    &ff_file_protocol,
    &ff_tcp_protocol,
    &ff_http_protocol,
    &ff_tls_protocol,
    &ff_https_protocol,
    &ff_crypto_protocol,
    &ff_game_protocol,
    NULL };
EOF
echo "protocol_list.c patched (game:/tcp:/http:/crypto: protocols):"
sed 's/^/  /' "$PLIST"

# 组件启用确认（P4 范围核对）：
echo "P4 components:"
grep -E "^#define CONFIG_(AVFILTER|SWSCALE|SWRESAMPLE|MOV_DEMUXER|MPEGTS_DEMUXER|H264_DEMUXER|AAC_DEMUXER|DASH_DEMUXER|FLV_DEMUXER|LIVE_FLV_DEMUXER|HLS_DEMUXER|LIBXML2|FILE_PROTOCOL|HTTP_PROTOCOL|HTTPS_PROTOCOL|TCP_PROTOCOL|CRYPTO_PROTOCOL|FILTER_SCALE|FILTER_ARESAMPLE|NETWORK|LZO) " config.h | sed 's/^#define /  /'

echo "configure done: $BLD/config.h"
