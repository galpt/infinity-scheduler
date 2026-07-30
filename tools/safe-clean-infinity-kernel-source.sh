#!/usr/bin/env bash
#
# safe-clean-infinity-kernel-source.sh
# Cleans build artifacts from /usr/src/linux-infinity after a kernel rebuild.
# Safe between reboots — only removes compiled .o files, vmlinux binaries, etc.
# Keeps source code, .config, and headers intact (DKMS still works).
#
# Usage:
#   ./safe-clean-infinity-kernel-source.sh          # normal (prompts for sudo)
#   ./safe-clean-infinity-kernel-source.sh --yes    # skip confirmation prompt

set -euo pipefail

KERNEL_SRC="/usr/src/linux-infinity"
LOG_FILE="/tmp/safe-clean-infinity-kernel-source-$(date +%Y%m%d-%H%M%S).log"

if [ ! -d "$KERNEL_SRC" ]; then
    echo "Error: $KERNEL_SRC does not exist. Nothing to clean."
    exit 1
fi

if [ ! -f "$KERNEL_SRC/Makefile" ]; then
    echo "Error: $KERNEL_SRC/Makefile not found. This doesn't look like a kernel source tree."
    exit 1
fi

if [ "${1:-}" != "--yes" ]; then
    CURRENT_SIZE=$(du -sh "$KERNEL_SRC" 2>/dev/null | awk '{print $1}')
    echo "Kernel source tree: $KERNEL_SRC"
    echo "Current size: $CURRENT_SIZE"
    echo "This will remove compiled build artifacts (.o files, vmlinux, etc.)"
    echo "Source code, .config, and headers will be preserved."
    echo ""
    read -rp "Proceed? [y/N] " reply
    case "$reply" in
        [yY]|[yY][eE][sS]) ;;
        *) echo "Aborted."; exit 0 ;;
    esac
fi

echo "Cleaning kernel source tree..."
sudo make -C "$KERNEL_SRC" clean 2>&1 | tee "$LOG_FILE"

echo ""
echo "Done. Log written to: $LOG_FILE"

# Show what's left
REMAINING=$(du -sh "$KERNEL_SRC" 2>/dev/null | awk '{print $1}')
echo "Remaining size: $REMAINING"
