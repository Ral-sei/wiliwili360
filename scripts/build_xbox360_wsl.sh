#!/usr/bin/env bash
# Xbox 360 OXDK incremental build helper (run inside WSL).
set -u
: "${REPO:=/home/lbx/cb360/wiliwili}"
cd "${REPO}" || exit 1
: "${OXDK_DIR:=/home/lbx/cb360/OXDK}"
: "${XDK_DIR:=/home/lbx/cb360/Microsoft Xbox 360 SDK}"
: "${OXDK_LLVM:=/home/lbx/oxdk-llvm/build}"
: "${LLVM_PREFIX:=${OXDK_LLVM}}"
: "${BILI360_DIR:=/home/lbx/cb360/bili360-main}"
export OXDK_DIR XDK_DIR OXDK_LLVM LLVM_PREFIX BILI360_DIR
export PATH="${OXDK_LLVM}/bin:/usr/bin:/bin"
cmake --build build-xbox360-m2 --target wiliwili -j12 > build-xbox360-current.log 2>&1
echo "DONE_$?" >> build-xbox360-current.log
