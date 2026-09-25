#!/usr/bin/env bash
# Build OXDK's libc++ static library (libcxx-xenon.a) for the Xenon target.
#
# OXDK's oxdk360.mk does this when OXDK360_LIBCXX_LIB=1: Iostreams, locale and
# the streambuf machinery live in libc++'s compiled half, which has no prebuilt
# library for this target. The flags are taken from OXDK's own makefile through
# scripts/print_cxxflags.mk so the headers and the compiled half always match.
set -eu

REPO=${REPO:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}
BUILD=${BUILD:-${REPO}/build-xbox360-m2}
OXDK_LIB=${BUILD}/libcxx-xenon.a

: "${OXDK_DIR:=/mnt/h/OXDK}"
: "${XDK_DIR:=/root/xdk360}"
: "${OXDK_LLVM:=/root/oxdk-llvm/build}"
: "${LLVM_PREFIX:=${OXDK_LLVM}}"
export OXDK_DIR XDK_DIR OXDK_LLVM LLVM_PREFIX
export PATH="${OXDK_LLVM}/bin:/usr/bin:/bin"
export OXDK_ROOT="${OXDK_ROOT:-${OXDK_DIR}}"
export OXDK360_DIR="${OXDK360_DIR:-${OXDK_DIR}/xbox360}"
export OXDK360_LIBCXX_DIR="${OXDK360_LIBCXX_DIR:-${OXDK_LLVM}/../libcxx/include}"

python3 "${REPO}/scripts/fix_crlf.py" "${REPO}/scripts/print_cxxflags.mk"
FLAGS_MK=$(mktemp "${TMPDIR:-/tmp}/wiliwili_print_cxxflags.XXXXXX.mk")
trap 'rm -f "${FLAGS_MK}"' EXIT
cp "${REPO}/scripts/print_cxxflags.mk" "${FLAGS_MK}"
FLAGS=$(make -f "${FLAGS_MK}" \
    OXDK_ROOT="${OXDK_ROOT}" \
    OXDK360_DIR="${OXDK360_DIR}" \
    OXDK360_LIBCXX_DIR="${OXDK360_LIBCXX_DIR}" \
    CLANG="${OXDK_LLVM}/bin/clang" print)
FLAGS="${FLAGS} -isystem ${REPO}/library/borealis/library/cmake/xbox360-cshim -isystem ${OXDK_DIR}/xbox360/oxdk360/header-shim"

make -C "${OXDK_DIR}/xbox360/oxdk360/libcxx-lib" \
    CLANG="${OXDK_LLVM}/bin/clang" \
    LLVM_AR="${OXDK_LLVM}/bin/llvm-ar" \
    LIBCXX_SRC="${LIBCXX_SRC:-${OXDK_LLVM}/../libcxx}" \
    OXDK360_CXXFLAGS="${FLAGS}" \
    BUILD="${BUILD}/libcxx-obj" \
    LIB="${OXDK_LIB}"

if [ -f "${OXDK_LIB}" ]; then
    echo "libcxx-xenon.a built: $(stat -c%s "${OXDK_LIB}") bytes"
else
    echo "libcxx-xenon.a missing" >&2
    exit 1
fi
