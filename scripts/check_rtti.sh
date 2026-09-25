#!/bin/bash
export PATH=/root/oxdk-llvm/build/bin:$PATH
cd /mnt/h/wiliwili/build-xbox360-m2/.oxdklink

echo "=== Check vtable binding for RTTI types ==="
# Check if the dummy vtables from builtins.c are weak and the real ones win
llvm-nm title.elf 2>/dev/null | grep -E "_ZTVN10__cxxabiv1(16__enum|17__class|20__si_class|21__vmi_class)" | head -10

echo ""
echo "=== Check _ZTIi (typeinfo for int) ==="
llvm-nm title.elf 2>/dev/null | grep "_ZTIi" | head -5

echo ""
echo "=== Check if __dynamic_cast is the real one (not stub) ==="
llvm-nm --defined-only title.elf 2>/dev/null | grep "__dynamic_cast$" | head -5

echo ""
echo "=== Verify no leftover stubs ==="
# The stub __dynamic_cast in m3_shims should have been removed
# Check the m3_shims staged object for __dynamic_cast
llvm-nm /mnt/h/wiliwili/build-xbox360-m2/.oxdklink/inputs/57a3da07ff-xbox360_m3_shims.cpp.o 2>/dev/null | grep "__dynamic_cast" | head -5

echo ""
echo "=== Check xbox360_platform_shims for private_typeinfo ==="
# Find the platform shims archive
ls /mnt/h/wiliwili/build-xbox360-m2/*.a 2>/dev/null
ls /mnt/h/wiliwili/build-xbox360-m2/lib*.a 2>/dev/null
find /mnt/h/wiliwili/build-xbox360-m2 -name "*platform_shims*" -o -name "*shims*" 2>/dev/null | head -10
