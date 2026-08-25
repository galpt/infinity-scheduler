#!/usr/bin/env bash
#
# safe-clean-infinity-kernel-source.sh
# Cleans build artifacts, stale DKMS modules, and superseded boot files.
# Keeps the running kernel, the newest Infinity kernel, and the default distro kernel.
#
# Usage:
#   ./safe-clean-infinity-kernel-source.sh          # normal (prompts for sudo)
#   ./safe-clean-infinity-kernel-source.sh --yes    # skip confirmation prompts

set -euo pipefail

KERNEL_SRC="/usr/src/linux-infinity"
LOG_FILE="/tmp/safe-clean-infinity-kernel-source-$(date +%Y%m%d-%H%M%S).log"

# ──────────────────────────────────────────────────────────────────────────────
# 1. Source Tree Artifact Cleanup
# ──────────────────────────────────────────────────────────────────────────────
clean_source_tree() {
    local yes="${1:-}"
    if [ ! -d "$KERNEL_SRC" ] || [ ! -f "$KERNEL_SRC/Makefile" ]; then
        echo "Skipping source cleanup: $KERNEL_SRC is not a valid kernel tree."
        return 0
    fi

    if [ "$yes" != "--yes" ]; then
        local current_size
        current_size=$(du -sh "$KERNEL_SRC" 2>/dev/null | awk '{print $1}')
        echo "Kernel source tree: $KERNEL_SRC ($current_size)"
        echo "This will remove compiled build artifacts (.o files, vmlinux, etc.)"
        echo "Source code, .config, and headers will be preserved."
        echo ""
        read -rp "Proceed with source cleanup? [y/N] " reply
        case "$reply" in
            [yY]|[yY][eE][sS]) ;;
            *) echo "Aborted source cleanup."; return 0 ;;
        esac
    fi

    echo "Cleaning kernel source tree..."
    sudo make -C "$KERNEL_SRC" clean 2>&1 | tee "$LOG_FILE"
    echo "Source clean complete. Log written to: $LOG_FILE"
    echo "Remaining size: $(du -sh "$KERNEL_SRC" 2>/dev/null | awk '{print $1}')"
}

# ──────────────────────────────────────────────────────────────────────────────
# 2. DKMS Cleanup for Superseded Kernels
# ──────────────────────────────────────────────────────────────────────────────
clean_stale_dkms() {
    local yes="${1:-}"
    command -v dkms &>/dev/null || { echo "dkms not installed — nothing to clean."; return 0; }

    local newest
    newest=$(ls -d /lib/modules/*-infinity 2>/dev/null | sed 's|^/lib/modules/||' | sort -V | tail -1) || true
    [ -n "$newest" ] || return 0

    local running=$(uname -r)
    local stale=() line mod kern

    while IFS= read -r line; do
        [ -z "$line" ] && continue
        mod=${line%%,*}
        kern=$(printf '%s' "$line" | cut -d, -f2 | tr -d ' ')

        case "$kern" in *-infinity) ;; *) continue ;; esac
        [ "$kern" = "$running" ] && continue
        [ "$kern" = "$newest" ] && continue

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
    for s in "${stale[@]}"; do echo "  - ${s%%|*} for kernel ${s##*|}"; done

    if [ "$yes" != "--yes" ]; then
        read -rp "Remove these DKMS builds? [y/N] " reply
        case "$reply" in
            [yY]|[yY][eE][sS]) ;;
            *) echo "Aborted DKMS cleanup."; return 0 ;;
        esac
    fi

    for entry in "${stale[@]}"; do
        local mod_name=${entry%%|*}
        local mod_ver=${mod_name#*/}
        kern=${entry##*|}
        echo "Removing ${mod_name} for kernel ${kern}..."
        sudo dkms remove "$mod_name" -k "$kern" 2>&1 || true
        sudo rm -rf "/var/lib/dkms/${mod_name%/*}/${mod_ver}/${kern}" 2>/dev/null || true
    done
    echo "Stale DKMS builds removed."
}

# ──────────────────────────────────────────────────────────────────────────────
# 3. Boot File & Limine Entry Cleanup
# ──────────────────────────────────────────────────────────────────────────────
clean_stale_boot_files() {
    local yes="${1:-}"
    local newest
    newest=$(ls -d /lib/modules/*-infinity 2>/dev/null | sed 's|^/lib/modules/||' | sort -V | tail -1) || true
    [ -z "$newest" ] && return 0

    local running=$(uname -r)
    local stale=()

    for kdir in /lib/modules/*-infinity; do
        [ -d "$kdir" ] || continue
        local kern=$(basename "$kdir")
        [ "$kern" = "$running" ] && continue
        [ "$kern" = "$newest" ] && continue
        stale+=("$kern")
    done

    if [ ${#stale[@]} -eq 0 ]; then
        echo "No stale boot files to clean."
        return 0
    fi

    echo ""
    echo "Stale Infinity kernels found in /boot and /lib/modules:"
    for kern in "${stale[@]}"; do echo "  - $kern"; done

    if [ "$yes" != "--yes" ]; then
        read -rp "Remove these kernels and their bootloader entries? [y/N] " reply
        case "$reply" in
            [yY]|[yY][eE][sS]) ;;
            *) echo "Aborted boot file cleanup."; return 0 ;;
        esac
    fi

    for kern in "${stale[@]}"; do
        echo "Removing boot files and modules for $kern..."
        sudo rm -rf "/lib/modules/$kern" 2>/dev/null || true
        sudo rm -f "/boot/vmlinuz-$kern" "/boot/initramfs-$kern.img" "/boot/System.map-$kern" 2>/dev/null || true

        # Strip exact entries out of Limine
        for lconf in /boot/limine/limine.conf /boot/limine.conf /limine/limine.conf /limine.conf /boot/efi/limine.conf /efi/limine.conf; do
            if [ -f "$lconf" ]; then
                sudo awk -v ver="($kern)" '
                    /^\// { if (index($0, ver) > 0) skip = 1; else skip = 0 }
                    !skip { print }
                ' "$lconf" > "${lconf}.tmp" && sudo mv "${lconf}.tmp" "$lconf"
            fi
        done
    done

    if command -v limine-update &>/dev/null; then
        sudo limine-update >/dev/null 2>&1 || true
    fi
    echo "Stale boot files and Limine entries removed."
}

# ── Execute Sequence ──────────────────────────────────────────────────────────
clean_source_tree "${1:-}"
clean_stale_dkms "${1:-}"
clean_stale_boot_files "${1:-}"
