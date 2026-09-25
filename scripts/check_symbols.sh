#!/bin/bash
export PATH=/root/oxdk-llvm/build/bin:$PATH
cd /mnt/h/wiliwili/build-xbox360-m2/.oxdklink

echo "=== __dynamic_cast symbol ==="
llvm-nm --defined-only --demangle title.elf 2>/dev/null | grep "__dynamic_cast" | head -5

echo ""
echo "=== __si_class_type_info symbols ==="
llvm-nm --defined-only --demangle title.elf 2>/dev/null | grep "si_class_type_info" | head -10

echo ""
echo "=== __abort_message ==="
llvm-nm --defined-only --demangle title.elf 2>/dev/null | grep "__abort_message" | head -5

echo ""
echo "=== set_terminate ==="
llvm-nm --defined-only --demangle title.elf 2>/dev/null | grep "set_terminate" | head -5
