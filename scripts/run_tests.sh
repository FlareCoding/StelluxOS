#!/bin/bash
set -euo pipefail

ARCH="${1:-}"
TIMEOUT="${STLX_TEST_TIMEOUT:-60}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BOLD='\033[1m'
DIM='\033[2m'
NC='\033[0m'

if [[ -z "$ARCH" ]]; then
    echo "Usage: $0 <ARCH>"
    echo "  ARCH: x86_64 or aarch64"
    exit 1
fi

SERIAL_LOG="$(mktemp)"
QEMU_PID=""

cleanup() {
    if [[ -n "$QEMU_PID" ]]; then
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    rm -f "$SERIAL_LOG"
}
trap cleanup EXIT

IMAGE="$ROOT_DIR/images/stellux-${ARCH}.img"
BUILD_DIR="$ROOT_DIR/build"

if [[ ! -f "$IMAGE" ]]; then
    echo "Error: Disk image not found: $IMAGE"
    echo "Run: make image ARCH=$ARCH STLX_UNIT_TESTS_ENABLED=1"
    exit 1
fi

: > "$SERIAL_LOG"

echo -e "${BOLD}=== Stellux Unit Tests (${ARCH}) ===${NC}"
echo ""

if [[ "$ARCH" == "x86_64" ]]; then
    OVMF_CODE=""
    for f in /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF_CODE.fd \
             /usr/share/ovmf/OVMF.fd /usr/share/qemu/OVMF.fd; do
        if [[ -f "$f" ]]; then OVMF_CODE="$f"; break; fi
    done
    OVMF_VARS_SRC=""
    for f in /usr/share/OVMF/OVMF_VARS_4M.fd /usr/share/OVMF/OVMF_VARS.fd; do
        if [[ -f "$f" ]]; then OVMF_VARS_SRC="$f"; break; fi
    done

    if [[ -z "$OVMF_CODE" || -z "$OVMF_VARS_SRC" ]]; then
        echo -e "${RED}Error: OVMF firmware not found. Run: make deps${NC}"
        exit 1
    fi

    OVMF_VARS="$BUILD_DIR/OVMF_VARS_TEST.fd"
    mkdir -p "$BUILD_DIR"
    cp "$OVMF_VARS_SRC" "$OVMF_VARS"

    qemu-system-x86_64 \
        -machine q35 \
        -cpu qemu64,+fsgsbase \
        -m 4G \
        -smp 4 \
        -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
        -drive if=pflash,format=raw,file="$OVMF_VARS" \
        -drive format=raw,file="$IMAGE" \
        -device qemu-xhci \
        -device usb-kbd \
        -serial file:"$SERIAL_LOG" \
        -display none \
        -no-reboot \
        -no-shutdown &
    QEMU_PID=$!

elif [[ "$ARCH" == "aarch64" ]]; then
    QEMU_EFI="/usr/share/qemu-efi-aarch64/QEMU_EFI.fd"
    if [[ ! -f "$QEMU_EFI" ]]; then
        echo -e "${RED}Error: AArch64 EFI firmware not found at $QEMU_EFI. Run: make deps${NC}"
        exit 1
    fi

    qemu-system-aarch64 \
        -machine virt \
        -cpu cortex-a57 \
        -m 4G \
        -smp 4 \
        -bios "$QEMU_EFI" \
        -drive format=raw,file="$IMAGE" \
        -device qemu-xhci \
        -device usb-kbd \
        -serial file:"$SERIAL_LOG" \
        -display none \
        -no-reboot \
        -no-shutdown &
    QEMU_PID=$!
else
    echo "Error: Unknown ARCH=$ARCH"
    exit 1
fi

SENTINEL_LINE=""
SENTINEL_LINE=$(timeout "$TIMEOUT" bash -c "
    while true; do
        if [[ -f '$SERIAL_LOG' ]]; then
            LINE=\$(grep -m1 'STLX_TESTS_COMPLETE' '$SERIAL_LOG' 2>/dev/null || true)
            if [[ -n \"\$LINE\" ]]; then
                echo \"\$LINE\"
                exit 0
            fi
        fi
        sleep 0.5
    done
" 2>/dev/null || true)

kill "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true
QEMU_PID=""

# Strip carriage returns from serial log (kernel outputs \r\n)
if [[ -f "$SERIAL_LOG" ]]; then
    tr -d '\r' < "$SERIAL_LOG" > "${SERIAL_LOG}.clean"
    mv "${SERIAL_LOG}.clean" "$SERIAL_LOG"
fi

if [[ -z "$SENTINEL_LINE" ]]; then
    echo -e "${RED}TIMEOUT: Kernel did not complete tests within ${TIMEOUT}s${NC}"
    echo ""
    echo "Serial log:"
    cat "$SERIAL_LOG" 2>/dev/null || true
    exit 2
fi

SENTINEL_LINE=$(echo "$SENTINEL_LINE" | tr -d '\r')
KERNEL_EXIT=$(echo "$SENTINEL_LINE" | grep -oE '[0-9]+$' || echo "1")

PASSED=0
FAILED=0
SKIPPED=0
FAIL_DETAILS=""

while IFS= read -r line; do
    # Only count indented lines (test-level results, not suite-level)
    if [[ "$line" =~ ^[[:space:]]+ok\ [0-9]+\ (.+)$ ]]; then
        name="${BASH_REMATCH[1]}"
        if [[ "$name" =~ \#\ SKIP ]]; then
            SKIPPED=$((SKIPPED + 1))
            FAIL_DETAILS="${FAIL_DETAILS}  ${YELLOW}⊘${NC} ${name}\n"
        else
            PASSED=$((PASSED + 1))
            FAIL_DETAILS="${FAIL_DETAILS}  ${GREEN}✓${NC} ${name}\n"
        fi

    elif [[ "$line" =~ ^[[:space:]]+not\ ok\ [0-9]+\ (.+)$ ]]; then
        name="${BASH_REMATCH[1]}"
        FAILED=$((FAILED + 1))
        FAIL_DETAILS="${FAIL_DETAILS}  ${RED}✗${NC} ${name}\n"

    elif [[ "$line" =~ ^[[:space:]]+#\ (.+)$ ]]; then
        diag="${BASH_REMATCH[1]}"
        if [[ "$diag" =~ ^(EXPECT|ASSERT|left:|right:|expression:) ]]; then
            FAIL_DETAILS="${FAIL_DETAILS}    ${DIM}# ${diag}${NC}\n"
        fi

    # Suite-level results (non-indented ok/not ok)
    elif [[ "$line" =~ ^not\ ok\ [0-9]+\ (.+)$ ]]; then
        name="${BASH_REMATCH[1]}"
        FAIL_DETAILS="${FAIL_DETAILS}${RED}  ✗ [suite] ${name}${NC}\n"

    elif [[ "$line" =~ ^ok\ [0-9]+\ (.+)$ ]]; then
        name="${BASH_REMATCH[1]}"
        FAIL_DETAILS="${FAIL_DETAILS}${GREEN}  ✓ [suite] ${name}${NC}\n"
    fi
done < "$SERIAL_LOG"

echo -e "$FAIL_DETAILS"
echo -e "${BOLD}---${NC}"

SUMMARY=""
if [[ $PASSED -gt 0 ]]; then
    SUMMARY="${GREEN}${PASSED} passed${NC}"
fi
if [[ $FAILED -gt 0 ]]; then
    [[ -n "$SUMMARY" ]] && SUMMARY="${SUMMARY}, "
    SUMMARY="${SUMMARY}${RED}${FAILED} failed${NC}"
fi
if [[ $SKIPPED -gt 0 ]]; then
    [[ -n "$SUMMARY" ]] && SUMMARY="${SUMMARY}, "
    SUMMARY="${SUMMARY}${YELLOW}${SKIPPED} skipped${NC}"
fi

if [[ -z "$SUMMARY" ]]; then
    SUMMARY="${DIM}no tests found${NC}"
fi

echo -e "$SUMMARY"
echo ""

if [[ "$KERNEL_EXIT" -eq 0 && $FAILED -eq 0 ]]; then
    exit 0
else
    exit 1
fi
