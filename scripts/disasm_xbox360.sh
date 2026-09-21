#!/usr/bin/env bash
# Disassemble address ranges of the staged Xbox 360 link ELF.
# Usage: disasm_xbox360.sh <title.elf> <start-hex> <stop-hex> [start stop ...]
set -u
ELF="$1"
shift
export PATH=/root/oxdk-llvm/build/bin:/usr/bin:/bin
echo "=== symbols near each range ==="
while [ $# -ge 2 ]; do
    echo "--- range $1 .. $2 ---"
    llvm-nm --defined-only -n "$ELF" | grep -iE "^00${1:0:6}" | head -8
    llvm-objdump -d --start-address=0x$1 --stop-address=0x$2 "$ELF"
    shift 2
done
