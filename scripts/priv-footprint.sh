#!/usr/bin/env bash
#
# priv-footprint.sh - Measure the privileged footprint of a kernel image.
#
# Reports privileged text, rodata, data, and bss sizes from the ELF,
# plus source-level counts of privilege annotations and allocations.
#
# Usage:
#   scripts/priv-footprint.sh [path/to/kernel.elf]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
ELF="${1:-$ROOT_DIR/build/kernel/x86_64/kernel.elf}"

if [[ ! -f "$ELF" ]]; then
    echo "error: kernel image not found: $ELF" >&2
    exit 1
fi

sym() { nm "$ELF" | awk -v s="$1" '$3 == s { print "0x" $1 }'; }

PRIV_TEXT_START=$(sym __stlx_kern_priv_start)
RODATA_START=$(sym __rodata_start)
PRIV_RODATA_START=$(sym __priv_rodata_start)
RODATA_END=$(sym __rodata_end)

priv_text=$(( RODATA_START - PRIV_TEXT_START ))
priv_rodata=$(( RODATA_END - PRIV_RODATA_START ))
priv_data=$(size -A "$ELF" | awk '$1 == ".priv.data" { print $2 }')
priv_bss=$(size -A "$ELF" | awk '$1 == ".priv.bss" { print $2 }')
total_text=$(size -A "$ELF" | awk '$1 == ".text" { print $2 }')

code_annotations=$(grep -rho "__PRIVILEGED_CODE" "$ROOT_DIR/kernel" \
    --include="*.h" --include="*.cpp" 2>/dev/null | wc -l)
data_annotations=$(grep -rhoE "__PRIVILEGED_(DATA|BSS|RODATA)" "$ROOT_DIR/kernel" \
    --include="*.h" --include="*.cpp" 2>/dev/null | wc -l)
kalloc_sites=$(grep -rhoE "\bk[z]?alloc(_new)?\b" "$ROOT_DIR/kernel" \
    --include="*.cpp" --include="*.h" 2>/dev/null | wc -l)
ualloc_sites=$(grep -rhoE "\bu[z]?alloc(_new)?\b" "$ROOT_DIR/kernel" \
    --include="*.cpp" --include="*.h" 2>/dev/null | wc -l)
run_elevated=$(grep -rho "RUN_ELEVATED" "$ROOT_DIR/kernel" \
    --include="*.cpp" --include="*.h" 2>/dev/null | wc -l)

echo "priv_text_bytes    $priv_text"
echo "priv_rodata_bytes  $priv_rodata"
echo "priv_data_bytes    $priv_data"
echo "priv_bss_bytes     $priv_bss"
echo "total_text_bytes   $total_text"
echo "priv_text_pct      $(( priv_text * 100 / total_text ))"
echo "code_annotations   $code_annotations"
echo "data_annotations   $data_annotations"
echo "kalloc_sites       $kalloc_sites"
echo "ualloc_sites       $ualloc_sites"
echo "run_elevated       $run_elevated"
