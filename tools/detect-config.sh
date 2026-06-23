#!/usr/bin/env bash
# infinity-scheduler — system configuration and patch compatibility check

KERNEL_VER=$(uname -r | grep -oP '^\d+\.\d+')
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PATCH_DIR="$PROJECT_DIR/patches/stable/linux-$KERNEL_VER-infinity"

echo "=== infinity-scheduler: System Configuration ==="
echo "Kernel: $(uname -r)"
grep -m1 "^PRETTY_NAME" /etc/os-release 2>/dev/null || echo "Distro: unknown"
echo "sched_ext: $(zcat /proc/config.gz 2>/dev/null | grep CONFIG_SCHED_CLASS_EXT= || echo 'N/A')"
echo "CPU: $(nproc) cores online"
echo ""

if [ -d "$PATCH_DIR" ]; then
    echo "Patches available for kernel $KERNEL_VER:"
    ls "$PATCH_DIR"/*.patch
    echo ""
    echo "Install: sudo bash $PROJECT_DIR/tools/install-infinity-scheduler.sh $KERNEL_VER"
else
    echo "No patches for kernel $KERNEL_VER yet."
    echo "Available:"
    ls "$PROJECT_DIR/patches/stable/" 2>/dev/null || echo "  (none)"
fi
