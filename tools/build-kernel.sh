#!/usr/bin/env bash
# Build Linux kernel with Infinity scheduler patches applied.

set -e

KERNEL_VER="${KERNEL_VER:-$(uname -r | grep -oP '^\d+\.\d+')}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PATCH_DIR="$PROJECT_DIR/patches/stable/linux-$KERNEL_VER-infinity"
KERNEL_SRC="${1:-/usr/src/linux}"

if [ ! -d "$PATCH_DIR" ]; then
    echo "No Infinity patches for kernel $KERNEL_VER."
    echo "Available:"
    ls "$PROJECT_DIR/patches/stable/"
    exit 1
fi

# Apply patches
cd "$KERNEL_SRC"
for p in "$PATCH_DIR"/*.patch; do
    echo "Applying: $(basename "$p")"
    patch -p1 -N -F 3 < "$p"
done

# Build
make olddefconfig
make -j"$(nproc)"

echo "Done. Reboot and select the Infinity kernel at the GRUB menu."
