#!/usr/bin/env bash
# Query symbols of the staged Xbox 360 link artifacts.
set -u
export PATH=/root/oxdk-llvm/build/bin:/usr/bin:/bin
ELF=/mnt/h/wiliwili/build-xbox360-m2/.oxdklink/title.elf
LIBCXX=/mnt/h/wiliwili/build-xbox360-m2/libcxx-xenon.a
echo "=== cxxabiv1 vtables in title.elf ==="
llvm-nm --defined-only "$ELF" | grep cxxabiv1
echo "=== cxxabiv1 in libcxx-xenon.a ==="
llvm-nm --defined-only "$LIBCXX" 2>/dev/null | grep cxxabiv1
echo "=== dynamic_cast in title.elf ==="
llvm-nm --defined-only "$ELF" | grep -i dynamic_cast
echo "=== terminate in title.elf ==="
llvm-nm --defined-only "$ELF" | grep -iE 'terminate|__cxa_pure' | head
