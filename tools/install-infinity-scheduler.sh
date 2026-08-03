#!/usr/bin/env bash
# ──────────────────────────────────────────────────────────────────────────────
# install-infinity-scheduler.sh — Build and install Infinity scheduler kernel as a
# separate GRUB entry.  NEVER replaces the running kernel.  The CachyOS
# kernel stays as the default boot option.
#
# Usage:
#   sudo bash install-infinity-scheduler.sh                         # build + install (auto-detect kernel)
#   sudo bash install-infinity-scheduler.sh 7.1                     # build for kernel 7.1
#   sudo bash install-infinity-scheduler.sh --remove                 # remove Infinity boot entries
#   sudo bash install-infinity-scheduler.sh --status                 # show current state
# ──────────────────────────────────────────────────────────────────────────────
set -euo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'
NC='\033[0m'
info()  { echo -e "${CYAN}==>${NC} $*"; }
ok()    { echo -e "  ${GREEN}✓${NC} $*"; }
warn()  { echo -e "  ${YELLOW}⚠${NC} $*"; }
err()   { echo -e "  ${RED}✗${NC} $*"; }
die()   { err "$*"; exit 1; }

# Set to 1 by check_nvidia() when an NVIDIA GPU is present; gates the
# DKMS rebuild and the nvidia_drm.modeset=1 setup in the install step.
NVIDIA_PRESENT=0

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
INFINITY_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
KERNEL_VER="${KERNEL_VER:-$(uname -r | grep -oP '^\d+\.\d+(\.\d+)?')}"

# Use KERNEL_VER from env (user override) or detect from running kernel.
# Example:  sudo KERNEL_VER=7.1 bash install-infinity-scheduler.sh
if [ -n "${1:-}" ] && [[ "$1" != "--"* ]]; then
    KERNEL_VER="$1"
fi

# Detect distro family
if [ -f /etc/fedora-release ] || [ -f /etc/redhat-release ]; then
    DISTRO_FAMILY="fedora"
else
    DISTRO_FAMILY="arch"  # Arch, CachyOS, EndeavourOS, etc.
fi

# Map kernel version to patch directory (e.g. 7.0.12 -> 7.0, 7.1.5 -> 7.1)
KERNEL_MAJOR="$(echo "$KERNEL_VER" | grep -oP '^\d+\.\d+')"

# resolve_latest_patch — query kernel.org for the latest stable release
# matching a given major.minor (e.g. 7.1 → 7.1.4).  Excludes -rc and
# -test tags.  Falls back to major.minor.0 if the query fails.
resolve_latest_patch() {
    local major_minor="$1"

    # Try the JSON endpoint first — fast and authoritative
    if command -v python3 &>/dev/null; then
        local json
        json=$(curl -s --max-time 10 "https://www.kernel.org/releases.json" 2>/dev/null \
               || wget -q -t 2 -T 10 -O - "https://www.kernel.org/releases.json" 2>/dev/null)
        if [ -n "$json" ]; then
            local ver
            ver=$(echo "$json" | python3 -c "
import json, sys
try:
    data = json.load(sys.stdin)
    releases = data.get('releases', [])
    prefix = '$major_minor.'
    versions = [r['version'] for r in releases
                if r['version'].startswith(prefix)
                and 'rc' not in r['version'].lower()
                and 'test' not in r['version'].lower()]
    if versions:
        print(max(versions, key=lambda v: [int(x) for x in v.split('.')]))
except Exception:
    pass
" 2>/dev/null)
            if [ -n "$ver" ]; then
                echo "$ver"
                return
            fi
        fi
    fi

    # Fallback: use git ls-remote on the stable git tree
    if command -v git &>/dev/null; then
        local tag
        tag=$(git ls-remote --refs --tags \
            "https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git" \
            "refs/tags/v${major_minor}.*" 2>/dev/null | \
            grep -v '\-rc' | \
            sed 's|.*refs/tags/v||' | \
            sort -t. -k1,1n -k2,2n -k3,3n | \
            tail -1)
        if [ -n "$tag" ]; then
            echo "$tag"
            return
        fi
    fi

    # Fallback: return the major.minor base
    echo "${major_minor}.0"
}

PATCH_DIR="$INFINITY_DIR/patches/$DISTRO_FAMILY/$KERNEL_MAJOR"
PATCH_VER="$KERNEL_MAJOR"

if [ ! -d "$PATCH_DIR" ]; then
    # No patches for this exact major — find the closest available
    local_base="$KERNEL_MAJOR"
    BEST_DIST=999
    BEST_DIR=""
    for d in "$INFINITY_DIR/patches/$DISTRO_FAMILY"/*/; do
        [ -d "$d" ] || continue
        v="$(basename "$d")"
        d_major="$(echo "$v" | grep -oP '^\d+\.\d+')"
        [ -z "$d_major" ] && continue
        dist=$(( (${local_base%%.*} - ${d_major%%.*}) * (${local_base%%.*} - ${d_major%%.*}) \
              + (${local_base#*.} - ${d_major#*.}) * (${local_base#*.} - ${d_major#*.}) ))
        if [ "$dist" -lt "$BEST_DIST" ]; then
            BEST_DIST=$dist
            BEST_DIR="$d"
            BEST_VER="$v"
        fi
    done
    if [ -z "$BEST_DIR" ]; then
        echo "No patches found under $INFINITY_DIR/patches/$DISTRO_FAMILY/"
        exit 1
    fi
    PATCH_DIR="$BEST_DIR"
    PATCH_VER="$BEST_VER"
    info "Using patches for $PATCH_VER (apply to kernel $KERNEL_VER with fuzz)."
fi

# Resolve the latest stable patch version for the supported major.minor.
# If patches target 7.1, find the latest non-RC 7.1.x release (e.g. 7.1.4)
# so users running a newer kernel (e.g. 7.2) still build against a version
# the patches are known to work on.
KERNEL_VER="$(resolve_latest_patch "$PATCH_VER")"
info "Using kernel source v$KERNEL_VER (patches for $PATCH_VER)."

# Collect patch files sorted by name
PATCH_FILES=()
while IFS= read -r -d '' f; do
    PATCH_FILES+=("$f")
done < <(find "$PATCH_DIR" -maxdepth 1 -name '*.patch' -print0 | sort -z)
[ ${#PATCH_FILES[@]} -gt 0 ] || die "No patches found in $PATCH_DIR"

KERNEL_SRC="${KERNEL_SRC:-/usr/src/linux-infinity}"
DISTRO="$(if [ "$DISTRO_FAMILY" = "fedora" ]; then echo "Fedora"; else echo "Arch/CachyOS"; fi)"


check_root() { [[ $EUID -eq 0 ]] || die "Must be run as root (sudo)."; }

cmd_status() {
    echo "Infinity-scheduler status"
    echo "  Running kernel: $(uname -r)"
    echo "  Distro: $DISTRO"
    echo ""
    if [ -d "$PATCH_DIR" ] && [ ${#PATCH_FILES[@]} -gt 0 ]; then
        ok "Patches available for kernel $KERNEL_VER (${#PATCH_FILES[@]} files)"
    else
        warn "No patches for kernel $KERNEL_VER"
    fi
    local INFINITY_kernels; INFINITY_kernels=$(ls /boot/vmlinuz-infinity-* 2>/dev/null | head -3)
    if [ -n "$INFINITY_kernels" ]; then
        ok "Infinity kernel(s) installed:"
        for f in $INFINITY_kernels; do
            echo "    $(basename "$f")"
        done
    else
        warn "Infinity kernel not installed"
    fi
    for limine_conf in /boot/limine/limine.conf /boot/limine.conf /limine/limine.conf /limine.conf; do
        if grep -qE "[Ii]nfinity scheduler kernel" "$limine_conf" 2>/dev/null; then
            ok "Limine entry: Infinity scheduler kernel"
            break
        fi
    done
    if grep -q "[Ii]nfinity-scheduler" /boot/grub/custom/infinity-scheduler.cfg 2>/dev/null; then
        ok "GRUB entry: infinity-scheduler"
    fi
    if ls /boot/loader/entries/infinity-scheduler-* 2>/dev/null | head -1 | grep -q .; then
        ok "systemd-boot entry: infinity-scheduler"
    fi
    echo ""
    echo "  To install: sudo bash $0"
    echo "  To remove:  sudo bash $0 --remove"
}

prepare_source() {
    # Always start fresh — delete any previous clone and re-clone.
    # This avoids all edge cases with stale patches, committed changes,
    # half-built trees, or .rej/.orig files from failed runs.
    info "Cloning kernel source v$KERNEL_VER to $KERNEL_SRC..."
    rm -rf "$KERNEL_SRC"
    mkdir -p "$(dirname "$KERNEL_SRC")"

    git clone --depth 1 --branch "v$KERNEL_VER" \
        "https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git" "$KERNEL_SRC" \
        2>/dev/null || git clone --depth 1 --branch "v$KERNEL_VER" \
        "https://github.com/torvalds/linux.git" "$KERNEL_SRC"

    cd "$KERNEL_SRC"

    # Generate .config from the running kernel
    info "Using running kernel's config..."
    zcat /proc/config.gz > .config 2>/dev/null || cp "/boot/config-$(uname -r)" .config
    make olddefconfig >/dev/null 2>&1 || true

    # Set LOCALVERSION so make kernelrelease returns something like 7.0.12-infinity.
    # After apply_patches removes .git, setlocalversion can't detect dirty
    # state, making the release string consistent with our boot file naming.
    if [ -f "scripts/config" ]; then
        if ! ./scripts/config --set-str CONFIG_LOCALVERSION "-infinity" 2>/dev/null; then
            sed -i '/^CONFIG_LOCALVERSION=/d' .config
            echo 'CONFIG_LOCALVERSION="-infinity"' >> .config
        fi
        ./scripts/config --disable CONFIG_LOCALVERSION_AUTO 2>/dev/null || true
        make olddefconfig >/dev/null 2>&1 || true
    fi
}

apply_patches() {
    cd "$KERNEL_SRC"
    [ ${#PATCH_FILES[@]} -gt 0 ] || die "No patches for kernel $KERNEL_VER in $PATCH_DIR"

    # Clean up any stale git am state from a previous interrupted run
    git am --abort 2>/dev/null || true

    # Sanitize patch files: ensure empty context lines have leading space and
    # hunk header counts match body length.  Idempotent — safe to run each time.
    # The patches are generated via git format-patch and require no reformatting.
    # Running fix-patch-format.py --rewrite here would corrupt diff headers.
    # if [ -f "$INFINITY_DIR/tools/fix-patch-format.py" ]; then
    #     python3 "$INFINITY_DIR/tools/fix-patch-format.py" --rewrite "$PATCH_DIR"/*.patch 2>/dev/null || true
    # fi
    # Patches are generated via git format-patch and have correct counts already.
    # Running fix-patch-counts.py --rewrite here would corrupt the hunk headers.
    # if [ -f "$INFINITY_DIR/tools/fix-patch-counts.py" ]; then
    #     python3 "$INFINITY_DIR/tools/fix-patch-counts.py" --rewrite "$PATCH_DIR"/*.patch 2>/dev/null || true
    # fi

    for p in "${PATCH_FILES[@]}"; do
        name=$(basename "$p")
        info "Applying: $name"
        if out=$(patch -p1 -N -F 10 -s < "$p" 2>&1); then
            ok "$name"
        elif echo "$out" | grep -q "Reversed\|already applied"; then
            ok "Already applied: $name"
        elif echo "$out" | grep -q "FAILED\|fuzz"; then
            echo "$out" | grep -E "FAILED|fuzz" | head -5
            die "Failed to apply $name. The patch may need updating."
        else
            ok "$name (with adjustments)"
        fi
    done

    # Commit patches to kernel git to keep the tree clean.
    git add -A 2>/dev/null
    git commit -m "Infinity-scheduler: apply Infinity patches" \
        --author "Infinity Scheduler <infinity@localhost>" 2>/dev/null || true

    # Remove .git — prevents scripts/setlocalversion from detecting dirty
    # state and appending '-dirty'.  Without a git repo, setlocalversion
    # falls back to just CONFIG_LOCALVERSION, giving a clean release like
    # 7.0.12-infinity.  This keeps module paths, mkinitcpio -k, and boot files
    # all consistent.  Same principle as infinity-scheduler's tarball extraction.
    rm -rf ".git"
}

check_deps() {
    local auto_install=false
    [[ "${INFINITY_AUTO_DEPS:-1}" == "1" ]] && auto_install=true

    local missing_bin=() missing_dev=()
    for cmd in bc flex bison python3; do
        command -v "$cmd" &>/dev/null || missing_bin+=("$cmd")
    done
    # Check for libelf
    if ! ldconfig -p 2>/dev/null | grep -q libelf && \
       ! pkg-config --exists libelf 2>/dev/null && \
       ! ldconfig -p 2>/dev/null | grep -q libelf; then
        missing_dev+=("libelf")
    fi
    # Check for openssl headers
    if [ ! -f /usr/include/openssl/opensslv.h ]; then
        missing_dev+=("openssl")
    fi

    # pahole / dwarves is optional but recommended for BTF
    if ! command -v pahole &>/dev/null && ! command -v dwarves &>/dev/null; then
        if command -v pacman &>/dev/null; then
            [[ "$auto_install" == true ]] && pacman -S --needed --noconfirm pahole 2>/dev/null | tail -2 || true
        elif command -v apt-get &>/dev/null; then
            [[ "$auto_install" == true ]] && apt-get install -y dwarves 2>/dev/null | tail -2 || true
        elif command -v dnf &>/dev/null; then
            [[ "$auto_install" == true ]] && dnf install -y dwarves 2>/dev/null | tail -2 || true
        fi
        if ! command -v pahole &>/dev/null; then
            warn "pahole not available — kernel will build without BTF support."
        fi
    fi

    if [ ${#missing_bin[@]} -eq 0 ] && [ ${#missing_dev[@]} -eq 0 ]; then
        return 0
    fi

    if [[ "$auto_install" == false ]]; then
        info "Missing build dependencies. Set INFINITY_AUTO_DEPS=1 to auto-install."
        for b in "${missing_bin[@]}"; do echo "  - $b"; done
        for d in "${missing_dev[@]}"; do echo "  - $d development headers"; done
        die "Install missing packages and re-run."
    fi

    # Auto-install — we are running as root
    local pkgs_bin=() pkgs_dev=()
    if command -v pacman &>/dev/null; then
        # Arch
        for b in "${missing_bin[@]}"; do
            case "$b" in
                bc) pkgs_bin+=("bc");;
                flex) pkgs_bin+=("flex");;
                bison) pkgs_bin+=("bison");;
                python3) pkgs_bin+=("python");;
            esac
        done
        for d in "${missing_dev[@]}"; do
            case "$d" in
                libelf) pkgs_bin+=("elfutils");;
                openssl) pkgs_bin+=("openssl");;
            esac
        done
        # base-devel is the meta-package for gcc/make/etc on Arch
        pkgs_bin+=("base-devel")
        # Remove duplicates
        mapfile -t pkgs_dev < <(printf "%s\n" "${pkgs_bin[@]}" | sort -u)
        info "Installing: ${pkgs_dev[*]} ..."
        pacman -S --needed --noconfirm "${pkgs_dev[@]}" 2>&1 | tail -5

    elif command -v apt-get &>/dev/null; then
        # Debian/Ubuntu
        for b in "${missing_bin[@]}"; do
            case "$b" in
                bc) pkgs_bin+=("bc");;
                flex) pkgs_bin+=("flex");;
                bison) pkgs_bin+=("bison");;
                python3) pkgs_bin+=("python3");;
            esac
        done
        for d in "${missing_dev[@]}"; do
            case "$d" in
                libelf) pkgs_dev+=("libelf-dev");;
                openssl) pkgs_dev+=("libssl-dev");;
            esac
        done
        pkgs_bin+=("build-essential")
        pkgs_all=("${pkgs_bin[@]}" "${pkgs_dev[@]}")
        mapfile -t pkgs_all < <(printf "%s\n" "${pkgs_all[@]}" | sort -u)
        info "Installing: ${pkgs_all[*]} ..."
        apt-get update -qq 2>/dev/null | tail -1 || true
        apt-get install -y "${pkgs_all[@]}" 2>&1 | tail -5

    elif command -v dnf &>/dev/null; then
        # Fedora
        for b in "${missing_bin[@]}"; do
            case "$b" in
                bc) pkgs_bin+=("bc");;
                flex) pkgs_bin+=("flex");;
                bison) pkgs_bin+=("bison");;
                python3) pkgs_bin+=("python3");;
            esac
        done
        for d in "${missing_dev[@]}"; do
            case "$d" in
                libelf) pkgs_dev+=("elfutils-libelf-devel");;
                openssl) pkgs_dev+=("openssl-devel");;
            esac
        done
        pkgs_bin+=("gcc" "make")
        pkgs_all=("${pkgs_bin[@]}" "${pkgs_dev[@]}")
        mapfile -t pkgs_all < <(printf "%s\n" "${pkgs_all[@]}" | sort -u)
        info "Installing: ${pkgs_all[*]} ..."
        dnf install -y "${pkgs_all[@]}" 2>&1 | tail -5

    else
        err "Unsupported package manager. Install these manually:"
        for b in "${missing_bin[@]}"; do echo "  - $b"; done
        for d in "${missing_dev[@]}"; do echo "  - $d development headers"; done
        die "Then re-run."
    fi

    # Re-check after install
    local still_missing=0
    for cmd in bc flex bison python3; do
        command -v "$cmd" &>/dev/null || { err "Failed to install $cmd"; still_missing=1; }
    done
    if [ $still_missing -eq 1 ]; then
        die "Some dependencies could not be installed automatically."
    fi
    ok "All build dependencies satisfied"
}

detect_nvidia() {
    # Prefer hardware detection: works even when the driver is not loaded
    # (e.g. after booting a kernel without NVIDIA modules, or on Optimus
    # laptops where the dGPU is not the display device).
    if command -v lspci &>/dev/null && lspci 2>/dev/null | grep -qiE 'nvidia|\[10de:'; then
        return 0
    fi
    # Fallback: the driver is loaded in the current kernel.
    lsmod 2>/dev/null | grep -q '^nvidia ' && return 0
    return 1
}

check_nvidia() {
    # If an NVIDIA GPU is present, ensure the DKMS infrastructure and a
    # DKMS nvidia source are available before the build.  The actual
    # module rebuild happens after kernel install, but installing the
    # driver package here catches any download/install failure early,
    # before 30 minutes of kernel compilation.
    if ! detect_nvidia; then
        info "No NVIDIA GPU detected — skipping NVIDIA driver setup."
        return 0
    fi
    NVIDIA_PRESENT=1

    # DKMS itself must be available to build nvidia for the new kernel.
    if ! command -v dkms &>/dev/null; then
        info "dkms not found — installing it..."
        if command -v pacman &>/dev/null; then
            pacman -S --needed --noconfirm dkms 2>&1 | tail -3
        elif command -v apt-get &>/dev/null; then
            apt-get update -qq 2>/dev/null || true
            apt-get install -y dkms 2>&1 | tail -3
        elif command -v dnf &>/dev/null; then
            dnf install -y dkms 2>&1 | tail -3
        else
            warn "Unsupported package manager — install dkms manually."
        fi
    fi
    if ! command -v dkms &>/dev/null; then
        warn "dkms unavailable — NVIDIA modules won't be built for the Infinity kernel."
        return 0
    fi

    # A DKMS nvidia source must be registered so the module can be built
    # for the Infinity kernel.  The CachyOS-specific prebuilt package
    # (linux-cachyos-nvidia-open) provides modules only for the CachyOS
    # kernel and conflicts with the DKMS variant, so it is replaced with
    # nvidia-open-dkms when present.
    if dkms status 2>/dev/null | grep -q '^nvidia/'; then
        info "DKMS NVIDIA source found — modules will be rebuilt for the new kernel."
        return 0
    fi

    info "NVIDIA GPU detected but no DKMS driver source installed — installing..."
    if command -v pacman &>/dev/null; then
        pacman -Rdd --noconfirm linux-cachyos-nvidia-open 2>/dev/null || true
        pacman -S --needed --noconfirm nvidia-open-dkms 2>&1 | tail -3 || \
            warn "nvidia-open-dkms install failed (NVIDIA won't be available on Infinity kernel)"
    elif command -v apt-get &>/dev/null; then
        apt-get update -qq 2>/dev/null || true
        if ! apt-get install -y nvidia-open-kernel-dkms 2>&1; then
            if ! apt-get install -y nvidia-kernel-dkms 2>&1; then
                apt-get install -y nvidia-driver 2>&1 || \
                    warn "NVIDIA DKMS driver install failed (NVIDIA won't be available on Infinity kernel)"
            fi
        fi
    elif command -v dnf &>/dev/null; then
        if ! dnf install -y akmod-nvidia 2>&1; then
            dnf install -y nvidia-open-dkms 2>&1 || \
                warn "NVIDIA DKMS driver install failed (NVIDIA won't be available on Infinity kernel)"
        fi
    else
        warn "Unsupported package manager — install an NVIDIA DKMS driver manually, then re-run."
    fi

    if ! dkms status 2>/dev/null | grep -q '^nvidia/'; then
        warn "NVIDIA DKMS source not registered after install — check the driver package."
    fi
}

build_kernel() {
    cd "$KERNEL_SRC"

    # Try build once.  If it fails with a common config issue, regenerate
    # config and retry.
    info "Building kernel (this takes a while)..."
    if ! make -j"$(nproc)" 2>&1; then
        warn "Build failed — checking config..."
        # Check for stale .config (e.g. from a different kernel version)
        if grep -q "^# CONFIG_HAVE_GENERIC_COHERENT is not set" .config 2>/dev/null; then
            # This is a sign of a deeply stale config — do mrproper
            warn "Config appears stale. Regenerating from scratch..."
            make mrproper 2>/dev/null || true
            zcat /proc/config.gz > .config 2>/dev/null || cp "/boot/config-$(uname -r)" .config
            make olddefconfig >/dev/null 2>&1 || true
        elif ! make olddefconfig 2>&1 | grep -q "updated"; then
            warn "Config needs refresh — running olddefconfig..."
            make olddefconfig >/dev/null 2>&1 || true
        fi
        info "Retrying build..."
        make -j"$(nproc)" 2>&1 || die "Build failed after config refresh. Try running 'make mrproper' manually in $KERNEL_SRC, then re-run this script."
    fi
    ok "Built successfully"
}

install_infinity_kernel() {
    cd "$KERNEL_SRC"

    # Install modules — does NOT affect other kernels
    info "Installing modules..."
    make modules_install -j"$(nproc)" 2>&1

    # Install kernel image with distinct name — does NOT touch /boot/vmlinuz-linux.
    # Use make kernelrelease for the version string.  Since .git was removed in
    # apply_patches(), scripts/setlocalversion can't detect dirty state and the
    # release is clean (e.g. 7.0.0-infinity).  This matches the module path that
    # make modules_install uses, keeping everything in sync.
    local ver
    ver=$(make kernelrelease 2>/dev/null || echo "unknown")

    # Rebuild NVIDIA modules for the new kernel via DKMS.
    # autoinstall skips kernel versions that are already registered,
    # which leaves modules from a previous build of the same kernel
    # string behind (mkinitcpio then reports "module not found" and the
    # module may fail to load).  Force a clean rebuild so the modules
    # always match the kernel just built.
    if [ "${NVIDIA_PRESENT:-0}" = "1" ] && command -v dkms &>/dev/null; then
        local nv_ver
        nv_ver=$(dkms status 2>/dev/null | grep '^nvidia/' | head -1 | cut -d, -f1 | cut -d/ -f2)
        info "Rebuilding NVIDIA modules for kernel $ver via DKMS..."
        if [ -n "$nv_ver" ]; then
            dkms remove "nvidia/$nv_ver" -k "$ver" 2>/dev/null || true
            if ! dkms autoinstall -k "$ver" 2>&1; then
                # autoinstall can fail if the install step gets confused
                # (e.g. stale depmod data) — force the install.
                dkms install -f -m nvidia -v "$nv_ver" -k "$ver" 2>&1 || true
            fi
        else
            dkms autoinstall -k "$ver" 2>&1 || true
        fi
        # Refresh the module dependency database so mkinitcpio/dracut can
        # resolve the freshly built modules.
        depmod -a "$ver" 2>&1 || true
    fi
    local img="/boot/vmlinuz-infinity-$ver"
    local initrd="/boot/initramfs-infinity-$ver.img"

    info "Installing kernel image to $img ..."
    cp "arch/x86/boot/bzImage" "$img"
    chmod 644 "$img"

    # Install System.map
    cp System.map "/boot/System.map-infinity-$ver"

    # Generate initramfs
    info "Generating initramfs..."
    if command -v mkinitcpio &>/dev/null; then
        mkinitcpio -k "$ver" -g "$initrd" 2>&1
    elif command -v dracut &>/dev/null; then
        dracut --force "$initrd" "$ver" 2>&1
    elif command -v update-initramfs &>/dev/null; then
        update-initramfs -u -k "$ver" 2>&1
    else
        warn "No initramfs tool found. You may need to generate it manually."
    fi

    # Add bootloader entry
    info "Adding bootloader entry..."
    local cmdline
    cmdline=$(cat /proc/cmdline 2>/dev/null || echo "root=UUID=$(findmnt -n -o UUID /) ro")

    # Ensure nvidia_drm.modeset=1 is in the cmdline.
    # The CachyOS kernel patches this into the driver itself, but our
    # vanilla kernel doesn't have that patch — without it, the NVIDIA
    # driver defaults to modeset=false and the display stays black.
    # Keyed on hardware presence rather than lsmod so a fresh install
    # (where the driver cannot be loaded yet) still gets the setting.
    if [ "${NVIDIA_PRESENT:-0}" = "1" ] && ! echo "$cmdline" | grep -q "nvidia_drm.modeset=1"; then
        cmdline="$cmdline nvidia_drm.modeset=1"
    fi

    # Also create a modprobe.d config as a fallback so modeset is
    # enabled even if the cmdline parameter is lost.
    if [ "${NVIDIA_PRESENT:-0}" = "1" ]; then
        mkdir -p /etc/modprobe.d
        echo "options nvidia_drm modeset=1" > /etc/modprobe.d/nvidia-infinity.conf
    fi

    if setup_limine_entry "$ver" "$cmdline"; then
        ok "Infinity kernel installed — Limine entry added."
        echo ""
        echo "  Reboot and select 'infinity scheduler kernel ($ver)' at the Limine menu."
    elif setup_grub_entry "$ver" "$cmdline"; then
        ok "Infinity kernel installed — GRUB entry added."
        echo ""
        echo "  Reboot and select 'infinity scheduler kernel ($ver)' at the GRUB menu."
    elif setup_sd_boot_entry "$ver" "$cmdline"; then
        ok "Infinity kernel installed — systemd-boot entry added."
        echo ""
        echo "  Reboot and select 'infinity scheduler kernel ($ver)' at the systemd-boot menu."
    else
        warn "Could not find Limine, GRUB, or systemd-boot. Boot entry not created."
        warn "  Kernel installed: /boot/vmlinuz-infinity-$ver"
        warn "  Initramfs:        /boot/initramfs-infinity-$ver.img"
        warn "  Add a boot entry manually."
    fi
    echo "  The default $DISTRO kernel is unchanged."
    echo ""
    echo "  To remove: sudo bash $0 --remove"
}

setup_limine_entry() {
    local ver="$1" cmdline="$2"
    local limine_conf=""
    for candidate in /boot/limine/limine.conf /boot/limine.conf /limine/limine.conf /limine.conf; do
        [ -f "$candidate" ] && { limine_conf="$candidate"; break; }
    done
    [ -z "$limine_conf" ] && return 1

    local entry_title="Infinity scheduler kernel ($ver)"
    local kernel_path="/vmlinuz-infinity-$ver"
    local initrd_path="/initramfs-infinity-$ver.img"

    # Remove old Infinity entries
    if grep -qE "[Ii]nfinity scheduler kernel" "$limine_conf" 2>/dev/null; then
        info "Removing old Infinity entries from Limine config..."
        awk '/^\/[Ii]nfinity scheduler kernel/ { skip = 1; next }
             skip && /^\// && !/^\/[Ii]nfinity scheduler kernel/ { skip = 0 }
             skip { next }
             1' "$limine_conf" > "${limine_conf}.tmp" && \
            mv "${limine_conf}.tmp" "$limine_conf"
    fi

    # Compute hashes for verified entry
    local kernel_hash="" initrd_hash=""
    if command -v b2sum &>/dev/null; then
        kernel_hash=$(b2sum "/boot$kernel_path" 2>/dev/null | cut -d' ' -f1) || kernel_hash=""
        initrd_hash=$(b2sum "/boot$initrd_path" 2>/dev/null | cut -d' ' -f1) || initrd_hash=""
    fi

    # Hash-verified entry
    {
        echo ""
        echo "/$entry_title"
        echo "    protocol: linux"
        if [ -n "$kernel_hash" ]; then
            echo "    kernel_path: boot():${kernel_path}#${kernel_hash}"
        else
            echo "    kernel_path: boot():${kernel_path}"
        fi
        if [ -n "$kernel_hash" ] && [ -n "$initrd_hash" ]; then
            echo "    module_path: boot():${initrd_path}#${initrd_hash}"
        else
            echo "    module_path: boot():${initrd_path}"
        fi
        echo "    cmdline: ${cmdline}"
    } >> "$limine_conf"

    # Fallback entry (no hashes)
    {
        echo ""
        echo "/$entry_title (fallback)"
        echo "    protocol: linux"
        echo "    kernel_path: boot():${kernel_path}"
        echo "    module_path: boot():${initrd_path}"
        echo "    cmdline: ${cmdline}"
        echo "    comment: No BLAKE2b hash check — safe after kernel reinstall"
    } >> "$limine_conf"

    info "  Limine entries added to $limine_conf"
    return 0
}

setup_grub_entry() {
    local ver="$1" cmdline="$2"
    if ! command -v grub-mkconfig &>/dev/null && ! command -v update-grub &>/dev/null; then
        return 1
    fi
    mkdir -p /boot/grub/custom
    local entry_file="/boot/grub/custom/infinity-scheduler.cfg"
    cat > "$entry_file" <<GRUB
menuentry "Infinity scheduler kernel ($ver)" {
    linux /vmlinuz-infinity-$ver ${cmdline}
    initrd /initramfs-infinity-$ver.img
}
GRUB
    if command -v grub-mkconfig &>/dev/null; then
        grub-mkconfig -o /boot/grub/grub.cfg 2>&1 | tail -5
    elif command -v update-grub &>/dev/null; then
        update-grub 2>&1 | tail -5
    fi
    return 0
}

setup_sd_boot_entry() {
    local ver="$1" cmdline="$2"
    local loader_dir="/boot/loader"
    local entries_dir="$loader_dir/entries"
    [ -d "$loader_dir" ] || return 1
    [ -f "$loader_dir/loader.conf" ] || return 1
    mkdir -p "$entries_dir"

    # Remove old Infinity entries first
    for old in "$entries_dir"/infinity-scheduler-*.conf; do
        [ -f "$old" ] && rm -f "$old"
    done

    local entry_file="$entries_dir/infinity-scheduler-${ver}.conf"
    cat > "$entry_file" <<SDCONF
title  Infinity scheduler kernel ($ver)
linux  /vmlinuz-infinity-$ver
initrd /initramfs-infinity-$ver.img
options $cmdline
SDCONF
    return 0
}

cmd_remove() {
    check_root
    local running
    running=$(uname -r)

    # Safety: never remove if currently booted into an Infinity kernel
    # (same check as infinity-scheduler's remove-kernel.sh)
    if [[ "$running" == *-infinity* ]]; then
        die "Refusing to remove: running '$running'. Reboot into the default $DISTRO kernel first, then re-run --remove."
    fi

    # Confirmation prompt
    local confirm
    echo ""
    info "This will remove all Infinity scheduler kernel files:"
    for f in /boot/vmlinuz-infinity-* /boot/initramfs-infinity-* /boot/System.map-infinity-*; do
        [ -f "$f" ] && echo "  $(basename "$f")"
    done
    echo ""
    read -r -p "Continue? [y/N] " confirm
    if [ "$confirm" != "y" ] && [ "$confirm" != "Y" ]; then
        info "Removal cancelled."
        exit 0
    fi

    info "Removing Infinity scheduler kernel..."

    # Remove Limine entries
    for limine_conf in /boot/limine/limine.conf /boot/limine.conf /limine/limine.conf /limine.conf; do
        [ -f "$limine_conf" ] || continue
        if grep -qE "[Ii]nfinity scheduler kernel" "$limine_conf" 2>/dev/null; then
            info "Removing Infinity entries from $limine_conf ..."
            awk '/^\/[Ii]nfinity scheduler kernel/ { skip = 1; next }
                 skip && /^\// && !/^\/[Ii]nfinity scheduler kernel/ { skip = 0 }
                 skip { next }
                 1' "$limine_conf" > "${limine_conf}.tmp" && \
                mv "${limine_conf}.tmp" "$limine_conf" && \
                ok "Limine entries removed from $limine_conf"
        fi
    done

    # Remove GRUB entry
    rm -f /boot/grub/custom/infinity-scheduler.cfg

    # Remove systemd-boot entries
    for old in /boot/loader/entries/infinity-scheduler-*.conf; do
        [ -f "$old" ] && rm -f "$old" && ok "Removed: $(basename "$old")"
    done

    # Remove kernel and initramfs images
    for f in /boot/vmlinuz-infinity-* /boot/initramfs-infinity-* /boot/System.map-infinity-*; do
        [ -f "$f" ] && rm -f "$f" && ok "Removed: $(basename "$f")"
    done

    # Regenerate GRUB config (if GRUB was used)
    if command -v grub-mkconfig &>/dev/null; then
        grub-mkconfig -o /boot/grub/grub.cfg 2>&1 | tail -5
    elif command -v update-grub &>/dev/null; then
        update-grub 2>&1 | tail -5
    fi

    # systemd-boot entries are files — already removed above, no regeneration needed

    ok "Infinity kernel removed. Default $DISTRO kernel is still in place."
}

# ── Main ──────────────────────────────────────────────────────────────────────
case "${1:-}" in
    -h|--help)
        echo "Usage: sudo bash install-infinity-scheduler.sh [--remove|--status]"
        echo "       sudo bash install-infinity-scheduler.sh [kernel-version]"
        exit 0 ;;
    --status) cmd_status; exit 0 ;;
    --remove) cmd_remove; exit 0 ;;
    "")
        check_root
        check_deps
        check_nvidia
        prepare_source
        apply_patches
        build_kernel
        install_infinity_kernel
        ;;
    *)
        # If it doesn't start with --, treat as kernel version override
        if [[ "$1" != --* ]]; then
            KERNEL_VER="$1"
            KERNEL_MAJOR="$(echo "$KERNEL_VER" | grep -oP '^\d+\.\d+')"
            PATCH_DIR="$INFINITY_DIR/patches/$DISTRO_FAMILY/$KERNEL_MAJOR"
            PATCH_FILES=()
            while IFS= read -r -d '' f; do PATCH_FILES+=("$f"); done < <(find "$PATCH_DIR" -maxdepth 1 -name '*.patch' -print0 | sort -z)
            [ ${#PATCH_FILES[@]} -gt 0 ] || die "No patches at $PATCH_DIR"
            check_root
            check_deps
            check_nvidia
            prepare_source
            apply_patches
            build_kernel
            install_infinity_kernel
        else
            die "Unknown option: $1"
        fi
        ;;
esac
