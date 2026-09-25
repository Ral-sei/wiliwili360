#!/bin/bash
export PATH=/root/oxdk-llvm/build/bin:$PATH

echo "=== List XDK cache archives ==="
ls -la /root/.cache/oxdk360/*.a 2>/dev/null

echo ""
echo "=== Check all cached .a files for .deplibs ==="
for f in /root/.cache/oxdk360/*.a; do
  [ -f "$f" ] || continue
  result=$(llvm-objdump -h "$f" 2>&1 | grep deplibs)
  if [ -n "$result" ]; then
    echo "HAS .deplibs: $(basename $f)"
    echo "$result"
  fi
done

echo ""
echo "=== Check if libcMT archive has .deplibs ==="
for f in /root/.cache/oxdk360/*libcMT* /root/.cache/oxdk360/*libc*; do
  [ -f "$f" ] || continue
  echo "Found: $(basename $f)"
  llvm-objdump -h "$f" 2>&1 | grep -E "deplibs|\.note"
  strings "$f" | grep -i "libc++" | head -5
done

echo ""
echo "=== Check ALL .o files inside cached archives ==="
for f in /root/.cache/oxdk360/*.a; do
  [ -f "$f" ] || continue
  # List members and check for .deplibs
  members=$(llvm-ar t "$f" 2>/dev/null)
  for m in $members; do
    # Extract and check each member - too slow, skip
    true
  done
done

echo ""
echo "=== Direct check: does libcMT.a or xapilib.a have .deplibs? ==="
for f in /root/.cache/oxdk360/*.a; do
  [ -f "$f" ] || continue
  llvm-readelf -S "$f" 2>/dev/null | grep -B1 deplibs
done

echo "=== Done ==="
