#!/bin/bash
export PATH=/root/oxdk-llvm/build/bin:$PATH
LLD=$(which ld.lld)
echo "ld.lld path: $LLD"
$LLD --help 2>&1 | grep -i -E "dependent|deplibs"
echo "---"
echo "Checking oxdklink cache..."
find /mnt/h/OXDK/xbox360/tools/oxdklink -name "*.a" -o -name "*.o" 2>/dev/null | head -20
echo "---"
find /root -path "*/oxdk*cache*" -name "*.a" 2>/dev/null | head -20
find /root -path "*/.cache*" -name "*.a" 2>/dev/null | head -20
echo "---done"
