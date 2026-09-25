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

cmake -S "${REPO}" -B "${BUILD_DIR}" -G Ninja \
    -DPLATFORM_XBOX360=ON \
    -DDISABLE_OPENCC=ON \
    -DDISABLE_WEBP=ON \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"

echo "Building Xbox 360 XEX in ${BUILD_DIR} (jobs=${JOBS})"
cmake --build "${BUILD_DIR}" --target wiliwili --parallel "${JOBS}" 2>&1 | tee "${LOG_FILE}"
printf 'XEX: %s/wiliwili\n' "${BUILD_DIR}"
