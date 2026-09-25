#!/bin/bash
export PATH=/root/oxdk-llvm/build/bin:$PATH
cd /mnt/h/wiliwili/build-xbox360-m2/.oxdklink/inputs

echo "=== Checking all files for .deplibs section ==="
for f in *.o *.a; do
  [ -f "$f" ] || continue
  result=$(llvm-objdump -h "$f" 2>&1 | grep deplibs)
  if [ -n "$result" ]; then
    echo "HAS .deplibs: $f"
    echo "$result"
  fi
done

echo ""
echo "=== Checking all .o files for libc++ string ==="
for f in *.o; do
  [ -f "$f" ] || continue
  if strings "$f" | grep -q 'libc++'; then
    echo "HAS libc++ string: $f"
    strings "$f" | grep 'libc++'
  fi
done

echo ""
echo "=== Checking all .a files for libc++ string ==="
for f in *.a; do
  [ -f "$f" ] || continue
  if strings "$f" | grep -q 'libc++'; then
    echo "HAS libc++ string: $f"
    strings "$f" | grep 'libc++'
  fi
done

echo ""
echo "=== Check oxdklink intermediate files ==="
cd /mnt/h/wiliwili/build-xbox360-m2/.oxdklink
ls -la pehdr.o imports.o 2>&1
for f in pehdr.o imports.o; do
  [ -f "$f" ] || continue
  result=$(llvm-objdump -h "$f" 2>&1 | grep deplibs)
  if [ -n "$result" ]; then
    echo "HAS .deplibs: $f"
    echo "$result"
  fi
  if strings "$f" | grep -q 'libc++'; then
    echo "HAS libc++ string: $f"
  fi
done

echo ""
echo "=== Check XDK translated archives ==="
cd /mnt/h/wiliwili/build-xbox360-m2/.oxdklink
ls -la *.a 2>&1 | head -20
if ls *.a >/dev/null 2>&1; then
  for f in *.a; do
    [ -f "$f" ] || continue
    result=$(llvm-objdump -h "$f" 2>&1 | grep deplibs)
    if [ -n "$result" ]; then
      echo "HAS .deplibs: $f"
      echo "$result"
    fi
  done
fi

echo ""
echo "=== Done ==="
