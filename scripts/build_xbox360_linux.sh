#!/usr/bin/env bash
# Build wiliwili for Xbox 360 from a native Linux checkout.
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=${REPO:-$(cd "${SCRIPT_DIR}/.." && pwd)}
BUILD_DIR=${BUILD_DIR:-${REPO}/build-xbox360-linux}
OXDK_DIR=${OXDK_DIR:-${REPO}/../OXDK}
XDK_DIR=${XDK_DIR:-${REPO}/../Microsoft Xbox 360 SDK}
OXDK_LLVM=${OXDK_LLVM:-${HOME}/oxdk-llvm/build}
BILI360_DIR=${BILI360_DIR:-${REPO}/../bili360-main}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
BUILD_TYPE=${BUILD_TYPE:-Release}
LOG_FILE=${LOG_FILE:-${BUILD_DIR}/build.log}

for required in cmake ninja python3 make; do
    command -v "${required}" >/dev/null || {
        echo "missing required command: ${required}" >&2
        exit 1
    }
done
for path in "${OXDK_DIR}" "${XDK_DIR}" "${OXDK_LLVM}/bin/clang++"; do
    [ -e "${path}" ] || { echo "missing Xbox 360 dependency: ${path}" >&2; exit 1; }
done

# CMake and the OXDK makefiles do not handle spaces in XDK paths on Linux.
mkdir -p "${BUILD_DIR}"
XDK_LINK=${BUILD_DIR}/xdk
if [ -L "${XDK_LINK}" ]; then
    :
elif [ -e "${XDK_LINK}" ]; then
    echo "build XDK link path is occupied: ${XDK_LINK}" >&2
    exit 1
else
    ln -s "${XDK_DIR}" "${XDK_LINK}"
fi

export OXDK_DIR OXDK_LLVM BILI360_DIR
export XDK_DIR=${XDK_LINK}
export LLVM_PREFIX=${LLVM_PREFIX:-${OXDK_LLVM}}
export OXDK360_CACHE=${OXDK360_CACHE:-${BUILD_DIR}/oxdk-cache}
export PATH="${OXDK_LLVM}/bin:/usr/bin:/bin:${PATH}"

# oxdklink 只认 LLD 这个环境变量（oxdklink.py:32）。cbffmpeg 的 x360-ld.sh 在
# ld.lld 后面追加 --wrap=getsockopt --wrap=WSAStartup：FFmpeg 判非阻塞 connect
# 成败要问 SO_ERROR，XDK 的 XNet 不实现它；WSAStartup 那条则保证任何一次
# WSAStartup 之前 XNet 已经以 XNET_STARTUP_BYPASS_SECURITY 启动。少了 LLD，
# mpv 的 http/tcp 会以 10042/10057 全线失败。详见 cbffmpeg 的
# compat/xbox360/x360-ld.sh 头注释。
if [ -z "${LLD:-}" ]; then
    CBFFMPEG_DIR=${CBFFMPEG_DIR:-${REPO}/../cbffmpeg}
    export CBFFMPEG_DIR
    if [ -x "${CBFFMPEG_DIR}/compat/xbox360/x360-ld.sh" ]; then
        export LLD="${CBFFMPEG_DIR}/compat/xbox360/x360-ld.sh"
    fi
fi

# FFmpeg 4.4.8 is vendored in third_party/ and tracked in git, so the source
# tree never comes from an external checkout. Point both the source and the
# build tree at this checkout and fail early with the exact commands when the
# static libraries have not been built yet.
export FFMPEG_SRC=${FFMPEG_SRC:-${REPO}/third_party/ffmpeg-4.4.8}
export FFMPEG_BUILD_DIR=${FFMPEG_BUILD_DIR:-${REPO}/build/ffmpeg-x360-tls}
[ -f "${FFMPEG_SRC}/configure" ] || {
    echo "missing vendored FFmpeg source: ${FFMPEG_SRC}" >&2
    exit 1
}
[ -f "${FFMPEG_BUILD_DIR}/libavformat/libavformat.a" ] || {
    echo "FFmpeg static libraries not found in ${FFMPEG_BUILD_DIR}" >&2
    echo "build them first:" >&2
    echo "  FFMPEG_SRC=${FFMPEG_SRC} FFMPEG_BUILD=${FFMPEG_BUILD_DIR} \\" >&2
    echo "    sh ${REPO}/scripts/ffmpeg_x360_configure_local.sh" >&2
    echo "  make -C ${FFMPEG_BUILD_DIR} -j${JOBS}" >&2
    exit 1
}

cmake -S "${REPO}" -B "${BUILD_DIR}" -G Ninja \
    -DPLATFORM_XBOX360=ON \
    -DMPV_SW_RENDER=ON \
    -DDISABLE_OPENCC=ON \
    -DDISABLE_WEBP=ON \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"

echo "Building Xbox 360 XEX in ${BUILD_DIR} (jobs=${JOBS})"
cmake --build "${BUILD_DIR}" --target wiliwili --parallel "${JOBS}" 2>&1 | tee "${LOG_FILE}"
printf 'XEX: %s/wiliwili\n' "${BUILD_DIR}"
