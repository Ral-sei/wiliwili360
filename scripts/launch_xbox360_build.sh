#!/usr/bin/env bash
# Launch the Xbox 360 build in the background, guarded against duplicates.
cd /mnt/h/wiliwili || exit 1
if pgrep -f oxdklink >/dev/null 2>&1 || pgrep -f "cmake --build build-xbox360-m2" >/dev/null 2>&1; then
    echo RUNNING
else
    nohup bash scripts/build_xbox360_wsl.sh >/dev/null 2>&1 &
    echo STARTED
fi
