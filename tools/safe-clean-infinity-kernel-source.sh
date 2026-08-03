#!/usr/bin/env bash
#
# safe-clean-infinity-kernel-source.sh
# Cleans build artifacts from /usr/src/linux-infinity after a kernel rebuild.
# Safe between reboots — only removes compiled .o files, vmlinux binaries, etc.
# Keeps source code, .config, and headers intact (DKMS still works).
# Also removes DKMS module builds for old Infinity kernels — never the
# running kernel, the newest Infinity kernel, or the default distro kernel.
#
# Usage:
#   ./safe-clean-infinity-kernel-source.sh          # normal (prompts for sudo)
#   ./safe-clean-infinity-kernel-source.sh --yes    # skip confirmation prompts

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

# ──────────────────────────────────────────────────────────────────────────────
# DKMS cleanup for superseded Infinity kernels
# ──────────────────────────────────────────────────────────────────────────────
# clean_stale_dkms — remove DKMS module builds for Infinity kernels older
# than the newest installed one.  The running kernel, the newest Infinity
# kernel, and non-Infinity kernels (e.g. the default distro kernel) are
# never touched.
clean_stale_dkms() {
    local yes="${1:-}"
    command -v dkms &>/dev/null || { echo "dkms not installed — nothing to clean."; return 0; }

    local newest
    newest=$(ls -d /lib/modules/*-infinity 2>/dev/null | sed 's|^/lib/modules/||' | sort -V | tail -1) || true
    [ -n "$newest" ] || { echo "No Infinity kernels installed — nothing to clean."; return 0; }

    local running stale=() line mod kern
    running=$(uname -r)
    while IFS= read -r line; do
        [ -z "$line" ] && continue
        mod=${line%%,*}
        kern=$(printf '%s' "$line" | cut -d, -f2 | tr -d ' ')
        case "$kern" in
            *-infinity) ;;
            *) continue ;;   # never touch non-Infinity kernels
        esac
        [ "$kern" = "$running" ] && continue
        [ "$kern" = "$newest" ] && continue
        # Only kernels older than the newest Infinity kernel (version-sorted).
        if [ "$(printf '%s\n%s\n' "$kern" "$newest" | sort -V | head -1)" = "$kern" ]; then
            stale+=("${mod}|${kern}")
        fi
    done < <(dkms status 2>/dev/null | grep -v '^$' || true)

    if [ ${#stale[@]} -eq 0 ]; then
        echo "No stale DKMS builds to clean."
        return 0
    fi

    echo ""
    echo "Stale DKMS builds for old Infinity kernels:"
    local s
    for s in "${stale[@]}"; do
        echo "  - ${s%%|*} for kernel ${s##*|}"
    done
    if [ "$yes" != "--yes" ]; then
        read -rp "Remove these DKMS builds? [y/N] " reply
        case "$reply" in
            [yY]|[yY][eE][sS]) ;;
            *) echo "Aborted DKMS cleanup."; return 0 ;;
        esac
    fi

    local entry mod_name mod_ver
    for entry in "${stale[@]}"; do
        mod_name=${entry%%|*}
        kern=${entry##*|}
        mod_ver=${mod_name#*/}
        echo "Removing ${mod_name} for kernel ${kern}..."
        sudo dkms remove "$mod_name" -k "$kern" 2>&1 || true
        # Remove any leftover build directory dkms left behind.
        sudo rm -rf "/var/lib/dkms/${mod_name%/*}/${mod_ver}/${kern}" 2>/dev/null || true
    done
    echo "Stale DKMS builds removed."
}

clean_stale_dkms "${1:-}"
