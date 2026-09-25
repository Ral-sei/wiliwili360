#!/bin/bash
export PATH=/root/oxdk-llvm/build/bin:$PATH

echo "=== Check libcxx-xenon.a members for .deplibs ==="
cd /mnt/h/wiliwili/build-xbox360-m2/.oxdklink/inputs
llvm-ar t 6aaf65ba6c-libcxx-xenon.a 2>/dev/null | wc -l
# Check each member
tmpdir=$(mktemp -d)
cd "$tmpdir"
llvm-ar x /mnt/h/wiliwili/build-xbox360-m2/.oxdklink/inputs/6aaf65ba6c-libcxx-xenon.a 2>/dev/null
count=0
for f in *.o; do
  [ -f "$f" ] || continue
  result=$(llvm-objdump -h "$f" 2>&1 | grep deplibs)
  if [ -n "$result" ]; then
    echo "HAS .deplibs: $f"
    echo "$result"
    count=$((count + 1))
  fi
done
echo "Checked members, found $count with .deplibs"
rm -rf "$tmpdir"

echo ""
echo "=== Try manually running the stage1 link to see full output ==="
cd /mnt/h/wiliwili
# Find the actual ld.lld command oxdklink.py would run
python3 -c "
import sys, os
sys.path.insert(0, '/mnt/h/OXDK/xbox360/tools/oxdklink')
sys.path.insert(0, '/mnt/h/OXDK/xbox360/tools/common')
import toolchain
LLD = toolchain.tool('ld.lld')
print('LLD:', LLD)
print('NM:', toolchain.tool('llvm-nm', 'LLVM_NM'))
" 2>&1

echo ""
echo "=== Actually run stage1 with --verbose equivalent ==="
cd /mnt/h/wiliwili
# Simulate what oxdklink.py does for pass 1
# Get the link command by running oxdklink.py with -v
echo "Note: The issue is ld.lld --dependent-libraries (default: on)"
echo "ld.lld finds .deplibs sections in input files and tries to resolve library names"
echo "Since libc++.lib doesn't exist on the link line, it fails"
echo ""
echo "The fix is either:"
echo "  1. Add --no-dependent-libraries to the ld.lld command in oxdklink.py"
echo "  2. Or ensure ALL input files (including libcxx-xenon.a members) have no .deplibs"

echo "=== Done ==="
