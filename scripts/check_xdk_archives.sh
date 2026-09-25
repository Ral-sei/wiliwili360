#!/bin/bash
export PATH=/root/oxdk-llvm/build/bin:$PATH

echo "=== Find XDK translated archives ==="
# oxdklink.py uses toolchain.cache_dir() to find the cache
# Check common locations
find /root -maxdepth 4 -name "*.a" -path "*/cache/*" 2>/dev/null | head -20
find /root -maxdepth 4 -name "*.a" -path "*/xdk*" 2>/dev/null | head -20
find /tmp -maxdepth 4 -name "*.a" 2>/dev/null | head -20

echo ""
echo "=== Check toolchain.py for cache_dir ==="
cat /mnt/h/OXDK/xbox360/tools/common/toolchain.py 2>/dev/null | grep -A5 "cache_dir"

echo ""
echo "=== Check stage1.elf for .deplibs references ==="
if [ -f /mnt/h/wiliwili/build-xbox360-m2/.oxdklink/stage1.elf ]; then
    llvm-nm /mnt/h/wiliwili/build-xbox360-m2/.oxdklink/stage1.elf 2>/dev/null | grep -i "deplibs\|dependent" | head -10
    # Check for libc++ undefined symbols
    llvm-nm -u /mnt/h/wiliwili/build-xbox360-m2/.oxdklink/stage1.elf 2>/dev/null | grep "libc++" | head -10
fi

echo ""
echo "=== Check what oxdklink.py stage1 link command was ==="
# Try to find the cache directory from oxdklink.py
python3 -c "
import sys; sys.path.insert(0, '/mnt/h/OXDK/xbox360/tools/common')
import toolchain
print('cache_dir:', toolchain.cache_dir())
" 2>&1

echo ""
echo "=== Done ==="
