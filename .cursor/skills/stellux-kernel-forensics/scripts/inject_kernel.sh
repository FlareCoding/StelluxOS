#!/bin/bash
# Build only the kernel from a scratch source tree and inject it into a copy of
# an existing disk image, so an experimental kernel can be A/B tested against
# the shipped one in seconds without rebuilding userland or touching the repo.
#
# usage: inject_kernel.sh --src DIR [--export] [--arch A] [--base IMG] --out IMG [-j N]
#   --src DIR      scratch source tree to build from
#   --export       first populate DIR with `git archive HEAD` from the repo (DIR must not exist)
#   --arch         default x86_64
#   --base IMG     image to copy, default images/stellux-<arch>.img under the repo root
#   --out IMG      resulting image (overwritten)
#   -j N           parallel jobs, default 8
#
# Prints kernel=<elf> image=<out>. Pass that ELF as --kernel to catch_frozen.sh
# and to gdb, since symbols must come from the ELF that is actually in the image.
set -u
SRC=""; EXPORT=0; ARCH=x86_64; BASE=""; OUT=""; JOBS=8
while [ $# -gt 0 ]; do
    case "$1" in
        --src) SRC="$2"; shift 2 ;;
        --export) EXPORT=1; shift ;;
        --arch) ARCH="$2"; shift 2 ;;
        --base) BASE="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        -j) JOBS="$2"; shift 2 ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
done
[ -n "$SRC" ] && [ -n "$OUT" ] || { echo "usage: inject_kernel.sh --src DIR [--export] --out IMG" >&2; exit 2; }

ROOT="${STLX_ROOT:-$(git rev-parse --show-toplevel 2>/dev/null || pwd)}"
[ -n "$BASE" ] || BASE="$ROOT/images/stellux-$ARCH.img"
[ -f "$BASE" ] || { echo "base image not found: $BASE" >&2; exit 2; }

if [ "$EXPORT" = 1 ]; then
    [ -e "$SRC" ] && { echo "refusing to export into existing $SRC" >&2; exit 2; }
    mkdir -p "$SRC"
    git -C "$ROOT" archive HEAD | tar -x -C "$SRC" || exit 1
fi
[ -d "$SRC/kernel" ] || { echo "no kernel/ under $SRC" >&2; exit 2; }

if ! make -C "$SRC/kernel" ARCH="$ARCH" BUILD_DIR=../build -j"$JOBS" > "$SRC/build.log" 2>&1; then
    tail -30 "$SRC/build.log" >&2
    echo "kernel build failed, full log: $SRC/build.log" >&2
    exit 1
fi
ELF="$SRC/build/kernel/$ARCH/kernel.elf"

cp "$BASE" "$OUT"
mcopy -o -i "$OUT@@1M" "$ELF" ::/kernel.elf || exit 1
mdir -i "$OUT@@1M" :: | grep -qi kernel || { echo "kernel.elf missing after injection" >&2; exit 1; }
echo "kernel=$ELF image=$OUT"
