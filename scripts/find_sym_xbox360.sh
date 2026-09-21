#!/usr/bin/env bash
# Find which link input provides a given symbol.
# Usage: find_sym_xbox360.sh <symbol-substring>
set -u
export PATH=/root/oxdk-llvm/build/bin:/usr/bin:/bin
PAT="$1"
B=/mnt/h/wiliwili/build-xbox360-m2
for f in "$B"/library/borealis/library/libborealis.a \
         "$B"/libxbox360_runtime.a \
         "$B"/libxbox360_platform_shims.a \
         "$B"/libcxx-xenon.a \
         /root/xdk360/lib/xbox/xapilib.lib \
         /root/xdk360/lib/xbox/libcMT.lib \
         /root/xdk360/lib/xbox/xnet.lib \
         /root/xdk360/lib/xbox/d3d9.lib; do
    if [ -f "$f" ]; then
        hit=$(llvm-nm --defined-only "$f" 2>/dev/null | grep -m3 "$PAT")
        if [ -n "$hit" ]; then
            echo "## $f"
            echo "$hit"
        fi
    fi
done
# also scan staged objects
grep_hit=$(llvm-nm --defined-only "$B"/.oxdklink/inputs/*.o 2>/dev/null | grep -m5 "$PAT")
[ -n "$grep_hit" ] && echo "## staged objects" && echo "$grep_hit"
echo done
