#!/usr/bin/env bash
# Build OXDK's libc++ static library (libcxx-xenon.a) for the Xenon target.
#
# OXDK's oxdk360.mk does this when OXDK360_LIBCXX_LIB=1: Iostreams, locale and
# the streambuf machinery live in libc++'s compiled half, which has no prebuilt
# library for this target. The flags are taken from OXDK's own makefile through
# scripts/print_cxxflags.mk so the headers and the compiled half always match.
set -eu

REPO=/mnt/h/wiliwili
BUILD=${REPO}/build-xbox360-m2
OXDK_LIB=${BUILD}/libcxx-xenon.a

export OXDK_DIR=/mnt/h/OXDK
export XDK_DIR=/root/xdk360
export OXDK_LLVM=/root/oxdk-llvm/build
export LLVM_PREFIX=/root/oxdk-llvm/build
export PATH=/root/oxdk-llvm/build/bin:/usr/bin:/bin

python3 "${REPO}/scripts/fix_crlf.py" "${REPO}/scripts/print_cxxflags.mk"
cp "${REPO}/scripts/print_cxxflags.mk" /tmp/wiliwili_print_cxxflags.mk
FLAGS=$(cd /tmp && make -f wiliwili_print_cxxflags.mk print)

make -C "${OXDK_DIR}/xbox360/oxdk360/libcxx-lib" \
    CLANG="${OXDK_LLVM}/bin/clang" \
    LLVM_AR="${OXDK_LLVM}/bin/llvm-ar" \
    LIBCXX_SRC=/root/oxdk-llvm/libcxx \
    OXDK360_CXXFLAGS="${FLAGS}" \
    BUILD="${BUILD}/libcxx-obj" \
    LIB="${OXDK_LIB}"

if [ -f "${OXDK_LIB}" ]; then
    echo "libcxx-xenon.a built: $(stat -c%s "${OXDK_LIB}") bytes"
else
    echo "libcxx-xenon.a missing" >&2
    exit 1
fi
