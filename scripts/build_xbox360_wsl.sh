#!/usr/bin/env bash
# Xbox 360 OXDK incremental build helper (run inside WSL).
set -u
cd /mnt/h/wiliwili || exit 1
export OXDK_DIR=/mnt/h/OXDK
export XDK_DIR=/root/xdk360
export OXDK_LLVM=/root/oxdk-llvm/build
export LLVM_PREFIX=/root/oxdk-llvm/build
export BILI360_DIR=/mnt/h/bili360-main
export PATH=/root/oxdk-llvm/build/bin:/usr/bin:/bin
cmake --build build-xbox360-m2 --target wiliwili -j2 > build-xbox360-current.log 2>&1
echo "DONE_$?" >> build-xbox360-current.log
