#!/usr/bin/env bash
# Inspect xbox360_runtime.a members for RTTI/typeinfo coverage.
set -u
export PATH=/root/oxdk-llvm/build/bin:/usr/bin:/bin
rm -rf /tmp/xr && mkdir -p /tmp/xr && cd /tmp/xr
llvm-ar x /mnt/h/wiliwili/build-xbox360-m2/libxbox360_runtime.a
for o in *.o; do
    echo "## $o"
    llvm-nm --defined-only "$o" | grep cxxabiv1 || echo "  (no cxxabiv1 symbols)"
done
echo "=== OXDK sources mentioning cxxabiv1 ==="
grep -rln cxxabiv1 /mnt/h/OXDK/xbox360/oxdk360/ 2>/dev/null | head
