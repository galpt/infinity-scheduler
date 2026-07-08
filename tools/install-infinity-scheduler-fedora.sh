#!/usr/bin/env bash
# ──────────────────────────────────────────────────────────────────────────────
# install-infinity-scheduler-fedora.sh — Build and install the Infinity scheduler
# kernel as a SEPARATE BLS boot entry on Fedora 44 (KDE).  NEVER replaces or
# re-defaults the running Fedora kernel — it is always added as an extra entry.
# The build is tagged with the infinity-scheduler repo branch, so the release
# string / boot entry read e.g. "7.0.14-infinity-v4.6".
#
# Fedora-native toolchain:
#   - dnf              for build dependencies
#   - dracut           for the initramfs (hostonly — picks up LUKS/LVM)
#   - grubby           for the Bootloader Spec (BLS) entry under
#                      /boot/loader/entries/  (GRUB_ENABLE_BLSCFG=true)
#
# Usage:
#   sudo bash install-infinity-scheduler-fedora.sh             # build + install (auto-detect kernel)
#   sudo bash install-infinity-scheduler-fedora.sh 7.1         # build for kernel 7.1
#   sudo bash install-infinity-scheduler-fedora.sh --remove    # remove Infinity BLS entry + files
#   sudo bash install-infinity-scheduler-fedora.sh --status    # show current state
#
# e.g. sudo bash tools/install-infinity-scheduler-fedora.sh
# e.g. sudo INFINITY_BRANCH=v4.6 bash tools/install-infinity-scheduler-fedora.sh
# ──────────────────────────────────────────────────────────────────────────────
set -euo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'
NC='\033[0m'
info()  { echo -e "${CYAN}==>${NC} $*"; }
ok()    { echo -e "  ${GREEN}✓${NC} $*"; }
warn()  { echo -e "  ${YELLOW}⚠${NC} $*"; }
err()   { echo -e "  ${RED}✗${NC} $*"; }
die()   { err "$*"; exit 1; }

# Resolve the infinity-scheduler repo's branch/tag so it can be baked into the
# kernel local version (e.g. 7.0.14-infinity-v4.6).  Order of preference:
#   INFINITY_BRANCH env override → current branch name → exact tag → short hash.
# The result is sanitized to characters valid in a kernel LOCALVERSION.
detect_branch() {
    local b=""
    if [ -n "${INFINITY_BRANCH:-}" ]; then
        b="$INFINITY_BRANCH"
    elif git -C "$INFINITY_DIR" rev-parse --git-dir &>/dev/null; then
        b="$(git -C "$INFINITY_DIR" symbolic-ref --short -q HEAD 2>/dev/null || true)"
        if [ -z "$b" ]; then
            b="$(git -C "$INFINITY_DIR" describe --tags --exact-match 2>/dev/null || true)"
            [ -z "$b" ] && b="$(git -C "$INFINITY_DIR" rev-parse --short HEAD 2>/dev/null || true)"
        fi
    fi
    # Keep only [A-Za-z0-9._-]; collapse/trim stray dashes.
    echo "$b" | tr -c 'A-Za-z0-9._-' '-' | sed 's/-\{2,\}/-/g; s/^-//; s/-$//'
}

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
INFINITY_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
KERNEL_VER="${KERNEL_VER:-$(uname -r | grep -oP '^\d+\.\d+(\.\d+)?')}"

# Kernel local version — includes the repo branch when detectable so the
# release string, boot files and menu title read e.g. 7.0.14-infinity-v4.6.
INFINITY_BRANCH="$(detect_branch)"
LOCALVERSION_SUFFIX="-infinity"
[ -n "$INFINITY_BRANCH" ] && LOCALVERSION_SUFFIX="-infinity-$INFINITY_BRANCH"

# Use KERNEL_VER from env (user override) or detect from running kernel.
# Example:  sudo KERNEL_VER=7.1 bash install-infinity-scheduler-fedora.sh
if [ -n "${1:-}" ] && [[ "$1" != "--"* ]]; then
    KERNEL_VER="$1"
fi

PATCH_DIR="$INFINITY_DIR/patches/stable/linux-$KERNEL_VER-infinity"
if [ ! -d "$PATCH_DIR" ]; then
    # No patches for this exact version — find the closest available
    # by comparing major.minor version numbers.
    local_base="$(echo "$KERNEL_VER" | grep -oP '^\d+\.\d+')"
    BEST_DIST=999
    BEST_PATCH=""
    for d in "$INFINITY_DIR/patches/stable/"*; do
        [ -d "$d" ] || continue
        v="$(basename "$d" | sed 's/linux-//;s/-infinity//')"
        d_major="$(echo "$v" | grep -oP '^\d+\.\d+')"
        # Squared Euclidean distance: (major_diff)^2 + (minor_diff)^2
        dist=$(( (${local_base%%.*} - ${d_major%%.*}) * (${local_base%%.*} - ${d_major%%.*}) \
              + (${local_base#*.} - ${d_major#*.}) * (${local_base#*.} - ${d_major#*.}) ))
        [ "$dist" -lt "$BEST_DIST" ] && BEST_DIST=$dist && BEST_PATCH="$d"
    done
    if [ -z "$BEST_PATCH" ]; then
        echo "No patches found in $INFINITY_DIR/patches/stable/"
        exit 1
    fi
    PATCH_DIR="$BEST_PATCH"
    PATCH_VER="$(basename "$BEST_PATCH" | sed 's/linux-//;s/-infinity//')"
    info "Using patches for $PATCH_VER (apply to kernel $KERNEL_VER with fuzz)."
fi
KERNEL_SRC="${KERNEL_SRC:-/usr/src/linux-infinity}"
DISTRO="Fedora"


check_root() { [[ $EUID -eq 0 ]] || die "Must be run as root (sudo)."; }

cmd_status() {
    echo "Infinity-scheduler status (Fedora)"
    echo "  Running kernel: $(uname -r)"
    echo "  Distro: $(grep -m1 '^PRETTY_NAME' /etc/os-release 2>/dev/null | cut -d'"' -f2 || echo "$DISTRO")"
    echo "  Repo branch: ${INFINITY_BRANCH:-(unknown)}"
    echo "  Build release suffix: $LOCALVERSION_SUFFIX"
    echo ""
    if [ -d "$PATCH_DIR" ]; then
        ok "Patches available for kernel $KERNEL_VER"
    else
        warn "No patches for kernel $KERNEL_VER"
    fi
    local infinity_kernels; infinity_kernels=$(ls /boot/vmlinuz-infinity-* 2>/dev/null | head -3)
    if [ -n "$infinity_kernels" ]; then
        ok "Infinity kernel image(s) installed:"
        for f in $infinity_kernels; do
            echo "    $(basename "$f")"
        done
    else
        warn "Infinity kernel not installed"
    fi
    if command -v grubby &>/dev/null; then
        local entries; entries=$(grubby --info=ALL 2>/dev/null | grep -iE "Infinity scheduler kernel" || true)
        if [ -n "$entries" ]; then
            ok "BLS boot entry present:"
            echo "$entries" | sed 's/^/    /'
        else
            warn "No Infinity BLS boot entry found"
        fi
        echo ""
        echo "  Default boot kernel: $(grubby --default-title 2>/dev/null || echo unknown)"
    fi
    echo ""
    echo "  To install: sudo bash $0"
    echo "  To remove:  sudo bash $0 --remove"
}

check_secureboot() {
    # A self-compiled, unsigned kernel will not boot when Secure Boot is
    # enforcing.  Warn early (before a 30-minute build) so the user can either
    # disable Secure Boot or enroll a MOK first.
    if command -v mokutil &>/dev/null; then
        local sb; sb=$(mokutil --sb-state 2>/dev/null || true)
        if echo "$sb" | grep -qi "enabled"; then
            warn "Secure Boot is ENABLED."
            warn "  A self-built kernel is unsigned and will be rejected at boot."
            warn "  Disable Secure Boot in firmware, or sign + enroll a MOK, then re-run."
        fi
    fi
}

check_deps() {
    local auto_install=false
    [[ "${INFINITY_AUTO_DEPS:-1}" == "1" ]] && auto_install=true

    command -v dnf &>/dev/null || die "dnf not found — this script targets Fedora. Use install-infinity-scheduler.sh for other distros."

    local missing_bin=() missing_dev=()
    for cmd in bc flex bison python3 gcc make; do
        command -v "$cmd" &>/dev/null || missing_bin+=("$cmd")
    done
    # libelf development headers (elfutils-libelf-devel)
    if [ ! -f /usr/include/libelf.h ] && ! pkg-config --exists libelf 2>/dev/null; then
        missing_dev+=("elfutils-libelf-devel")
    fi
    # openssl headers (openssl-devel)
    if [ ! -f /usr/include/openssl/opensslv.h ]; then
        missing_dev+=("openssl-devel")
    fi
    # ncurses headers — needed if the user later runs menuconfig
    if [ ! -f /usr/include/ncurses.h ] && [ ! -f /usr/include/ncurses/ncurses.h ]; then
        missing_dev+=("ncurses-devel")
    fi

    # pahole (from dwarves) is optional but recommended for BTF.
    if ! command -v pahole &>/dev/null; then
        if [[ "$auto_install" == true ]]; then
            dnf install -y dwarves 2>&1 | tail -2 || true
        fi
        command -v pahole &>/dev/null || warn "pahole (dwarves) not available — kernel will build without BTF support."
    fi

    if [ ${#missing_bin[@]} -eq 0 ] && [ ${#missing_dev[@]} -eq 0 ]; then
        ok "All build dependencies satisfied"
        return 0
    fi

    if [[ "$auto_install" == false ]]; then
        info "Missing build dependencies. Set INFINITY_AUTO_DEPS=1 to auto-install."
        for b in "${missing_bin[@]}"; do echo "  - $b"; done
        for d in "${missing_dev[@]}"; do echo "  - $d"; done
        die "Install missing packages and re-run."
    fi

    # Map the missing binaries to Fedora package names.
    local pkgs=()
    for b in "${missing_bin[@]}"; do
        case "$b" in
            bc)      pkgs+=("bc");;
            flex)    pkgs+=("flex");;
            bison)   pkgs+=("bison");;
            python3) pkgs+=("python3");;
            gcc)     pkgs+=("gcc");;
            make)    pkgs+=("make");;
        esac
    done
    pkgs+=("${missing_dev[@]}")
    # Always ensure core build tooling and perl (kernel build scripts need it).
    pkgs+=("perl")

    mapfile -t pkgs < <(printf "%s\n" "${pkgs[@]}" | sort -u)
    info "Installing: ${pkgs[*]} ..."
    dnf install -y "${pkgs[@]}" 2>&1 | tail -8

    # Re-check after install
    local still_missing=0
    for cmd in bc flex bison python3 gcc make; do
        command -v "$cmd" &>/dev/null || { err "Failed to install $cmd"; still_missing=1; }
    done
    if [ $still_missing -eq 1 ]; then
        die "Some dependencies could not be installed automatically."
    fi
    ok "All build dependencies satisfied"
}

check_nvidia() {
    # Fedora uses akmod-nvidia (RPM Fusion), which auto-rebuilds modules for new
    # kernels via the akmods service.  We only need to trigger a rebuild for the
    # freshly built kernel.  On AMD/Intel systems (no nvidia module) this is a
    # silent no-op.
    if ! lsmod 2>/dev/null | grep -q "^nvidia "; then
        return 0  # NVIDIA not in use, nothing to do
    fi
    if command -v akmods &>/dev/null; then
        info "NVIDIA driver active — akmods will rebuild modules after kernel install."
    else
        warn "NVIDIA driver active but 'akmods' not found."
        warn "  Install akmod-nvidia from RPM Fusion so modules build for the Infinity kernel."
    fi
}

prepare_source() {
    # Always start fresh — delete any previous clone and re-clone.
    # This avoids all edge cases with stale patches, committed changes,
    # half-built trees, or .rej/.orig files from failed runs.
    info "Cloning kernel source v$KERNEL_VER to $KERNEL_SRC..."
    rm -rf "$KERNEL_SRC"
    mkdir -p "$(dirname "$KERNEL_SRC")"

    # Use the stable kernel tree — it has all tags including point releases
    # (v7.0, v7.0.12, etc.), while Linus's tree only has major version tags.
    git clone --depth 1 --branch "v$KERNEL_VER" \
        "https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git" "$KERNEL_SRC" \
        2>/dev/null || git clone --depth 1 --branch "v$KERNEL_VER" \
        "https://github.com/torvalds/linux.git" "$KERNEL_SRC"

    cd "$KERNEL_SRC"

    # Generate .config from the running kernel.  Fedora does NOT enable
    # CONFIG_IKCONFIG_PROC, so /proc/config.gz is usually absent — prefer the
    # shipped /boot/config-$(uname -r) and fall back to /proc/config.gz.
    info "Using running kernel's config..."
    if [ -f "/boot/config-$(uname -r)" ]; then
        cp "/boot/config-$(uname -r)" .config
    else
        zcat /proc/config.gz > .config 2>/dev/null || die "Could not find a kernel config (/boot/config-$(uname -r) or /proc/config.gz)."
    fi
    make olddefconfig >/dev/null 2>&1 || true

    if [ -n "$INFINITY_BRANCH" ]; then
        info "Tagging build with repo branch '$INFINITY_BRANCH' (local version: $LOCALVERSION_SUFFIX)."
    fi

    if [ -f "scripts/config" ]; then
        # Set LOCALVERSION so make kernelrelease returns e.g. 7.0.12-infinity-v4.6
        # (the branch suffix is added when the repo branch is detectable).
        if ! ./scripts/config --set-str CONFIG_LOCALVERSION "$LOCALVERSION_SUFFIX" 2>/dev/null; then
            sed -i '/^CONFIG_LOCALVERSION=/d' .config
            echo "CONFIG_LOCALVERSION=\"$LOCALVERSION_SUFFIX\"" >> .config
        fi
        ./scripts/config --disable CONFIG_LOCALVERSION_AUTO 2>/dev/null || true

        # Fedora fix: the shipped config points these at Fedora's signing/
        # revocation certificate files, which do not exist in a vanilla tree.
        # Leaving them set makes the build fail with a missing-cert error.
        # Blank them so the kernel builds without Fedora's keys.
        ./scripts/config --set-str CONFIG_SYSTEM_TRUSTED_KEYS "" 2>/dev/null || true
        ./scripts/config --set-str CONFIG_SYSTEM_REVOCATION_KEYS "" 2>/dev/null || true

        make olddefconfig >/dev/null 2>&1 || true
    fi
}

apply_patches() {
    cd "$KERNEL_SRC"
    [ -d "$PATCH_DIR" ] || die "No patches for kernel $KERNEL_VER"

    # Patches are generated via git format-patch and have correct hunk counts;
    # no reformatting is required here.
    for p in "$PATCH_DIR"/*.patch; do
        name=$(basename "$p")
        info "Applying: $name"
        if out=$(patch -p1 -N -F 10 < "$p" 2>&1); then
            ok "$name"
        elif echo "$out" | grep -q "Reversed\|already applied"; then
            ok "Already applied: $name"
        else
            echo "$out" | grep -i -E "FAILED|error|malformed|misordered" | head -5
            die "Failed to apply $name. The patch may need updating."
        fi
    done

    # Commit patches to kernel git to keep the tree clean.
    git add -A 2>/dev/null
    git commit -m "Infinity-scheduler: apply Infinity patches" \
        --author "Infinity Scheduler <infinity@localhost>" 2>/dev/null || true

    # Remove .git — prevents scripts/setlocalversion from detecting dirty
    # state and appending '-dirty', giving a clean release like 7.0.12-infinity
    # so module paths and boot file names stay consistent.
    rm -rf ".git"
}

build_kernel() {
    cd "$KERNEL_SRC"

    info "Building kernel (this takes a while)..."
    if ! make -j"$(nproc)" 2>&1; then
        warn "Build failed — checking config..."
        if grep -q "^# CONFIG_HAVE_GENERIC_COHERENT is not set" .config 2>/dev/null; then
            warn "Config appears stale. Regenerating from scratch..."
            make mrproper 2>/dev/null || true
            if [ -f "/boot/config-$(uname -r)" ]; then
                cp "/boot/config-$(uname -r)" .config
            else
                zcat /proc/config.gz > .config 2>/dev/null || true
            fi
            ./scripts/config --set-str CONFIG_LOCALVERSION "$LOCALVERSION_SUFFIX" 2>/dev/null || true
            ./scripts/config --disable CONFIG_LOCALVERSION_AUTO 2>/dev/null || true
            ./scripts/config --set-str CONFIG_SYSTEM_TRUSTED_KEYS "" 2>/dev/null || true
            ./scripts/config --set-str CONFIG_SYSTEM_REVOCATION_KEYS "" 2>/dev/null || true
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

    # make kernelrelease gives the clean release string (e.g. 7.0.12-infinity)
    # matching the module path modules_install just used.
    local ver
    ver=$(make kernelrelease 2>/dev/null || echo "unknown")

    local img="/boot/vmlinuz-infinity-$ver"
    local initrd="/boot/initramfs-infinity-$ver.img"

    info "Installing kernel image to $img ..."
    cp "arch/x86/boot/bzImage" "$img"
    chmod 644 "$img"
    cp System.map "/boot/System.map-infinity-$ver"

    # Trigger NVIDIA (akmod) rebuild for the new kernel, if applicable.
    if lsmod 2>/dev/null | grep -q "^nvidia " && command -v akmods &>/dev/null; then
        info "Rebuilding NVIDIA akmod modules for kernel $ver..."
        akmods --kernels "$ver" 2>&1 | tail -5 || \
            warn "akmods rebuild failed (NVIDIA may be unavailable on the Infinity kernel)."
    fi

    # Generate initramfs with dracut.  Hostonly mode (Fedora default) pulls in
    # the modules this machine's root needs — including LUKS (crypt) and LVM.
    info "Generating initramfs with dracut..."
    if command -v dracut &>/dev/null; then
        dracut --force --kver "$ver" "$initrd" 2>&1 | tail -5
    else
        die "dracut not found — cannot generate initramfs on Fedora."
    fi

    # Add a BLS boot entry via grubby (Fedora uses GRUB_ENABLE_BLSCFG=true, so
    # entries live in /boot/loader/entries/*.conf).  --copy-default inherits the
    # working cmdline (root=, rd.luks.uuid=, rd.lvm.lv=, nosmt, ...) and does
    # NOT change the default boot kernel.
    command -v grubby &>/dev/null || die "grubby not found — cannot add a boot entry on Fedora."

    local title="Infinity scheduler kernel ($ver)"

    # Remove any stale entry for this exact image so re-runs don't duplicate.
    if grubby --info="$img" &>/dev/null; then
        info "Removing stale boot entry for $img ..."
        grubby --remove-kernel="$img" 2>&1 | tail -2 || true
    fi

    info "Adding BLS boot entry: $title"
    grubby --add-kernel="$img" \
           --initrd="$initrd" \
           --title="$title" \
           --copy-default 2>&1 | tail -5

    if grubby --info="$img" &>/dev/null; then
        ok "Infinity kernel installed — BLS boot entry added."
        echo ""
        echo "  Reboot and select '$title' at the GRUB menu."
    else
        warn "Kernel installed but grubby did not report the entry."
        warn "  Kernel image:  $img"
        warn "  Initramfs:     $initrd"
        warn "  Add a boot entry manually with grubby if needed."
    fi
    echo "  The default $DISTRO kernel remains the boot default and is unchanged."
    echo ""
    echo "  To remove: sudo bash $0 --remove"
}

cmd_remove() {
    check_root
    local running
    running=$(uname -r)

    # Safety: never remove if currently booted into an Infinity kernel.
    if [[ "$running" == *-infinity* ]]; then
        die "Refusing to remove: running '$running'. Reboot into the default $DISTRO kernel first, then re-run --remove."
    fi

    command -v grubby &>/dev/null || warn "grubby not found — BLS entries won't be removed automatically."

    echo ""
    info "This will remove all Infinity scheduler kernel files and boot entries:"
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

    # Remove BLS boot entries (one per installed Infinity image).
    if command -v grubby &>/dev/null; then
        for img in /boot/vmlinuz-infinity-*; do
            [ -f "$img" ] || continue
            if grubby --info="$img" &>/dev/null; then
                info "Removing BLS entry for $(basename "$img") ..."
                grubby --remove-kernel="$img" 2>&1 | tail -2 && ok "Boot entry removed"
            fi
        done
    fi

    # Remove kernel module trees for the Infinity kernels.
    for img in /boot/vmlinuz-infinity-*; do
        [ -f "$img" ] || continue
        local ver; ver="$(basename "$img" | sed 's/^vmlinuz-infinity-//')"
        if [ -n "$ver" ] && [ -d "/lib/modules/$ver" ]; then
            rm -rf "/lib/modules/$ver" && ok "Removed modules: /lib/modules/$ver"
        fi
    done

    # Remove kernel, initramfs and System.map images.
    for f in /boot/vmlinuz-infinity-* /boot/initramfs-infinity-* /boot/System.map-infinity-*; do
        [ -f "$f" ] && rm -f "$f" && ok "Removed: $(basename "$f")"
    done

    ok "Infinity kernel removed. Default $DISTRO kernel is still in place."
}

# ── Main ──────────────────────────────────────────────────────────────────────
case "${1:-}" in
    -h|--help)
        echo "Usage: sudo bash install-infinity-scheduler-fedora.sh [--remove|--status]"
        echo "       sudo bash install-infinity-scheduler-fedora.sh [kernel-version]"
        exit 0 ;;
    --status) cmd_status; exit 0 ;;
    --remove) cmd_remove; exit 0 ;;
    "")
        check_root
        check_secureboot
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
            PATCH_DIR="$INFINITY_DIR/patches/stable/linux-$KERNEL_VER-infinity"
            check_root
            check_secureboot
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
