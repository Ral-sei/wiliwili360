#!/usr/bin/env bash
# Build wiliwili for Xbox 360 with the local Linux/XDK checkout.
#
# This is a machine-local companion to build_xbox360_wsl.sh.  Keep the
# machine-specific paths here so the shared build helper remains portable.
set -euo pipefail

REPO=/mnt/h/wiliwili
OXDK_DIR=/mnt/h/OXDK
# On this machine /root/xdk360 IS the XDK root (include/xbox sits directly
# under it), so no trailing /XDK. `-isystem ${XDK_DIR}/include/xbox` must aim
# at a real directory, otherwise <sal.h>/<stdio.h> fall through to the host
# headers and mbedtls fails to compile.
XDK_DIR=/root/xdk360
OXDK_LLVM=/root/oxdk-llvm/build
BILI360_DIR=/mnt/h/bili360-main
BUILD_DIR="${REPO}/build-xbox360-local"
JOBS="${JOBS:-12}"

for required in cmake ninja python3 make; do
    command -v "${required}" >/dev/null || {
        echo "missing required command: ${required}" >&2
        exit 1
    }
done

for path in "${REPO}" "${OXDK_DIR}" "${XDK_DIR}" "${OXDK_LLVM}/bin/clang++"; do
    [ -e "${path}" ] || {
        echo "missing Xbox 360 dependency: ${path}" >&2
        exit 1
    }
done

export REPO OXDK_DIR XDK_DIR OXDK_LLVM BILI360_DIR
export LLVM_PREFIX="${OXDK_LLVM}"
export OXDK_ROOT="${OXDK_DIR}"
export OXDK360_DIR="${OXDK_DIR}/xbox360"
export PATH="${OXDK_LLVM}/bin:/usr/bin:/bin:${PATH}"

# oxdklink reads only the LLD environment variable (oxdklink.py:32). cbffmpeg's
# x360-ld.sh appends --wrap=getsockopt --wrap=WSAStartup to ld.lld: FFmpeg judges
# non-blocking connect completion by SO_ERROR, which XNet does not implement, and
# the WSAStartup wrap guarantees XNet starts with XNET_STARTUP_BYPASS_SECURITY
# before any WSAStartup. Without LLD the link fails outright on undefined
# __real_getsockopt/__real_WSAStartup — and even if it did not, mpv's http/tcp
# would fail with 10042/10057. Same logic as build_xbox360_linux.sh.
if [ -z "${LLD:-}" ]; then
    CBFFMPEG_DIR=${CBFFMPEG_DIR:-${REPO}/../cbffmpeg}
    export CBFFMPEG_DIR
    if [ -x "${CBFFMPEG_DIR}/compat/xbox360/x360-ld.sh" ]; then
        export LLD="${CBFFMPEG_DIR}/compat/xbox360/x360-ld.sh"
    fi
fi

# A CMake cache written by another machine freezes CMAKE_HOME_DIRECTORY,
# compilers and the oxdklink link rule under /home/lbx; it cannot be
# reconfigured in place. Drop the whole build dir in that case so configure
# regenerates every path from the exports above.
if [ -f "${BUILD_DIR}/CMakeCache.txt" ] && grep -q '/home/lbx' "${BUILD_DIR}/CMakeCache.txt"; then
    echo "Stale ${BUILD_DIR} cache (paths from another machine); removing it"
    rm -rf "${BUILD_DIR}"
fi
mkdir -p "${BUILD_DIR}"

# FFmpeg 4.4.8 is vendored in third_party/ and tracked in git. Use the in-tree
# source and build tree; stop early with the exact commands if the archives are
# missing (CMake would otherwise fail later with the same information).
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
    -DCMAKE_BUILD_TYPE=Release

echo "Building Xbox 360 XEX in ${BUILD_DIR} (jobs=${JOBS})"
cmake --build "${BUILD_DIR}" --target wiliwili --parallel "${JOBS}"
printf 'XEX: %s/wiliwili\n' "${BUILD_DIR}"
