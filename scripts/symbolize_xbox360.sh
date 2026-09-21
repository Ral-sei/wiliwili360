#!/usr/bin/env bash
# Symbolize Xbox 360 crash addresses against the staged link ELF.
# Usage: symbolize_xbox360.sh <title.elf> <addr-hex> [addr-hex...]
set -u
ELF="${1:?usage: symbolize_xbox360.sh <title.elf> <addr-hex>...}"
shift
export PATH=/root/oxdk-llvm/build/bin:/usr/bin:/bin
SYMS=$(mktemp)
llvm-nm --defined-only --demangle "$ELF" | while read -r addr type name; do
    case "$addr" in
        ''|*[!0-9a-fA-F]*) continue ;;
    esac
    printf '%d %s\n' "0x$addr" "$name"
done | sort -n > "$SYMS"
for a in "$@"; do
    t=$((16#$a))
    last="?"
    while read -r v name; do
        if [ "$v" -le "$t" ]; then last="$name"; else break; fi
    done < "$SYMS"
    echo "0x$a -> $last"
done
rm -f "$SYMS"
