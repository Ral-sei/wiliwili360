#!/usr/bin/env bash
# Background runner for the Xenon libc++ build (used by a Windows scheduled task).
cd /mnt/h/wiliwili || exit 1
bash scripts/build_xbox360_libcxx.sh > build-xbox360-libcxx.log 2>&1
echo "LIBCXX_DONE_$?" >> build-xbox360-libcxx.log
