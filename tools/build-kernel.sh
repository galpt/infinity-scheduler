#!/usr/bin/env bash
# Build Linux kernel with FLOW scheduler patches applied.
# Usage:  sudo bash tools/build-kernel.sh /path/to/linux-source

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
FLOW_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
KERNEL_DIR="${1:-}"

if [ -z "$KERNEL_DIR" ]; then
    echo "Usage: sudo bash tools/build-kernel.sh /path/to/linux-source"
    exit 1
fi

# Detect kernel version and pick the right patch directory
KERNEL_VER=$(uname -r | grep -oP '^\d+\.\d+')
PATCH_DIR="$FLOW_DIR/patches/stable/linux-$KERNEL_VER-flow"

if [ ! -d "$PATCH_DIR" ]; then
    echo "No patches found for kernel $KERNEL_VER at $PATCH_DIR"
    echo "Available:"
    ls "$FLOW_DIR/patches/stable/"
    exit 1
fi

cd "$KERNEL_DIR"

# Apply all FLOW patches
for p in "$PATCH_DIR"/*.patch; do
    echo "Applying: $(basename "$p")"
    patch -p1 < "$p" || { echo "Patch failed: $p"; exit 1; }
done

# Build
make -j"$(nproc)"
make modules_install -j"$(nproc)"
make install
echo "Done. Reboot and select the FLOW kernel at the GRUB menu."
