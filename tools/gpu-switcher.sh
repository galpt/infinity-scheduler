#!/usr/bin/env bash
# ──────────────────────────────────────────────────────────────────────────────
# gpu-switcher.sh — Unified GPU Driver Switcher for CachyOS & Infinity Scheduler
# ──────────────────────────────────────────────────────────────────────────────
set -euo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'
NC='\033[0m'
info() { echo -e "${CYAN}==>${NC} $*"; }
ok()   { echo -e "  ${GREEN}✓${NC} $*"; }
warn() { echo -e "  ${YELLOW}⚠${NC} $*"; }
die()  { echo -e "  ${RED}✗${NC} $*"; exit 1; }

check_root() {
    if [[ $EUID -ne 0 ]]; then
        die "Must be run as root (sudo)."
    fi
}

# Resolve which chwd NVIDIA profile is installed (e.g. nvidia-open-dkms.prime).
# CachyOS 'chwd' force-adds MODULES+=(nvidia ...) via its own 10-chwd.conf
# drop-in, so we must drive chwd itself rather than fight it indirectly.
CHWD_NVIDIA_PROFILE=""
resolve_nvidia_chwd_profile() {
    if ! command -v chwd &>/dev/null; then
        warn "chwd not found — falling back to direct 10-chwd.conf management."
        return
    fi
    CHWD_NVIDIA_PROFILE="$(chwd --list-installed 2>/dev/null | grep -oE 'nvidia[^[:space:]]*' | head -n1 || true)"
    if [[ -n "$CHWD_NVIDIA_PROFILE" && ! "$CHWD_NVIDIA_PROFILE" =~ ^nvidia[-_a-zA-Z0-9.]*$ ]]; then
        warn "Parsed chwd profile name '$CHWD_NVIDIA_PROFILE' looks invalid — ignoring it."
        CHWD_NVIDIA_PROFILE=""
    fi
    if [ -z "$CHWD_NVIDIA_PROFILE" ]; then
        warn "No installed nvidia chwd profile found."
    else
        ok "Using chwd nvidia profile: '$CHWD_NVIDIA_PROFILE'"
    fi
}

# Decide the "open" target: this machine has no nova module, so it is nouveau.
OPEN_TARGET=""
OPEN_CMD=""
OPEN_MODS=""
resolve_open_target() {
    if modinfo nova_drm &>/dev/null || modinfo nova_core &>/dev/null; then
        OPEN_TARGET="nova"
        OPEN_CMD="nova_drm.modeset=1"
        OPEN_MODS="nova_core? nova_drm? nova?"
    else
        OPEN_TARGET="nouveau"
        # modeset=2 = headless: the NVIDIA GPU stays usable for DRM render work
        # (nova/nouveau render node, DRM scheduling) but exposes NO display, so the
        # login/compositor always uses the other GPU (e.g. an AMD iGPU). modeset=1
        # on hybrid laptops makes nouveau create a display card that cannot scan out
        # (no CRTC for the internal panel) -> black login screen.
        OPEN_CMD="nouveau.modeset=2"
        OPEN_MODS="nouveau?"
        # Fatal, not a warning: switching to "the open driver" when no open kernel
        # module exists would silently leave the system with a broken/vacant GPU
        # (and a black login screen). Refuse loudly instead.
        if ! modinfo nouveau >/dev/null 2>&1; then
            die "No open GPU kernel module is available (nouveau/nova not installed) — \
the open driver cannot work and would leave you with a black screen. \
Install a kernel that provides nouveau/nova (e.g. 'sudo pacman -S linux-cachyos') or stay on NVIDIA."
        fi
    fi
    info "Open target: $OPEN_TARGET"
}

# Swap the modeset token from one driver to another inside single-quoted cmdline
# values in /etc/default/limine (used by update_cachyos_limine_default).
strip_gpu_cmdline_tokens() {
    sed -E 's/\b(nvidia_drm\.modeset=[0-9]|nouveau\.modeset=[0-9]|nova_drm\.modeset=[0-9]|nova\.modeset=[0-9])\b//g'
}

# Snapshot before touching any boot config so a broken reboot is one command to undo.
# Never fatal — if we cannot snapshot, we warn and continue.
snapshot_safety() {
    command -v snapper >/dev/null 2>&1 || return 0
    local desc
    desc="gpu-switcher before switch to ${1:-unknown} ($(date +%Y-%m-%d_%H%M%S))"
    if snapper create -d "$desc" >/dev/null 2>&1; then
        ok "Pre-switch btrfs snapshot created: '$desc' (roll back with: sudo snapper rollback)"
    else
        warn "Could not create a snapper snapshot — consider 'sudo snapper create -d gpu-switcher' first."
    fi
}

fail_step() {
    echo -e "  ${RED}✗${NC} $*" >&2
    die "Aborting mid-switch. Restore from the *.bak files above, or run the opposite switch."
}

# Optionally pull in nouveau userspace/firmware deps (nouveau target only).
provision_open_deps() {
    if [ "$OPEN_TARGET" != "nouveau" ] || ! command -v pacman &>/dev/null; then
        return
    fi
    local want=()
    if ! pacman -Q nouveau-fw &>/dev/null 2>&1; then
        want+=(nouveau-fw)
    fi
    if ! pacman -Q vulkan-nouveau &>/dev/null 2>&1; then
        want+=(vulkan-nouveau)
    fi
    [ "${#want[@]}" -eq 0 ] && { ok "Open driver dependencies already present."; return; }

    if pacman -Ss '^nouveau-fw$' &>/dev/null && pacman -Ss '^vulkan-nouveau$' &>/dev/null; then
        pacman -S --needed --noconfirm "${want[@]}" 2>&1 | tail -3 \
            || warn "Dependency install failed (continuing)."
    else
        warn "Firmware / vulkan-nouveau not found in repos (continuing without)."
    fi
}

find_limine_confs() {
    local found=()
    for candidate in /boot/limine/limine.conf /boot/limine.conf /limine/limine.conf /limine.conf /boot/efi/limine.conf /efi/limine.conf; do
        if [ -f "$candidate" ]; then
            found+=("$candidate")
        fi
    done
    if [ ${#found[@]} -eq 0 ]; then
        die "Could not locate limine.conf in standard boot paths."
    fi
    echo "${found[@]}"
}

# Decide current state. Three states ONLY: NVIDIA | OPEN | NONE — NEVER default to NVIDIA.
detect_driver_state() {
    if lsmod 2>/dev/null | grep -qE '^nvidia '; then
        echo "NVIDIA"
        return
    fi
    if lsmod 2>/dev/null | grep -qE '^(nova_core|nova_drm|nova |nouveau )'; then
        echo "OPEN"
        return
    fi

    # No driver module currently loaded — infer the intended state from configuration.
    if [ -n "$CHWD_NVIDIA_PROFILE" ] || [ -f /etc/mkinitcpio.conf.d/10-chwd.conf ] \
        || grep -qE '^MODULES=.+nvidia' /etc/mkinitcpio.conf 2>/dev/null; then
        echo "NVIDIA"
        return
    fi
    if grep -qE 'nvidia_drm\.modeset=[0-9]' /etc/default/limine 2>/dev/null; then
        echo "NVIDIA"
        return
    fi
    if grep -qE 'nouveau\.modeset=[0-9]|nova[-_]?drm\.modeset=[0-9]' /etc/default/limine 2>/dev/null; then
        echo "OPEN"
        return
    fi
    if [ -f /etc/modprobe.d/nvidia-blacklist.conf ]; then
        echo "OPEN"
        return
    fi
    echo "NONE"
}

# Update /etc/default/limine (CachyOS default preset). Idempotent.
update_cachyos_limine_default() {
    local to_add="$1"
    local file="/etc/default/limine"

    [ -f "$file" ] || return
    cp -p "$file" "${file}.bak"

    # Strip ALL gpu cmdline tokens (open + nvidia).
    sed -i -E '/^KERNEL_CMDLINE/ s/\b(nvidia_drm\.modeset=[0-9]|nouveau\.modeset=[0-9]|nova_drm\.modeset=[0-9]|nova\.modeset=[0-9])\b//g' "$file"
    # Append to_add once, if not already present, right before the trailing quote.
    # A KERNEL_CMDLINE line without a trailing quote is left unchanged.
    sed -i -E "/^KERNEL_CMDLINE/ {
        /${to_add}/! s/([\"'])\$/ ${to_add}\\1/
    }" "$file"
    # Collapse whitespace runs.
    sed -i -E '/^KERNEL_CMDLINE/ s/[ \t]+/ /g' "$file"

    ok "Updated default CachyOS cmdline in $file."
}

# Patch dynamically generated limine.conf entries (Infinity scheduler).
update_infinity_limine_entries() {
    local to_add="$1"
    local lconf
    local limine_files
    read -r -a limine_files <<< "$(find_limine_confs)"

    for lconf in "${limine_files[@]}"; do
        info "Patching dynamically generated $lconf ..."
        cp -p "$lconf" "${lconf}.bak"
        # Strip ALL gpu cmdline tokens (open + nvidia) from cmdline: lines.
        sed -i -E "/^[ \t]*(\+|-)?(kernel_cmdline:|cmdline:)/ s/\b(nvidia_drm\.modeset=[0-9]|nouveau\.modeset=[0-9]|nova_drm\.modeset=[0-9]|nova\.modeset=[0-9])\b//g" "$lconf"
        # Append to_add once if not already present.
        sed -i -E "/^[ \t]*(\+|-)?(kernel_cmdline:|cmdline:)/ { /${to_add}/! s/\$/ ${to_add}/ }" "$lconf"
        # Collapse whitespace runs.
        sed -i -E "/^[ \t]*(\+|-)?(kernel_cmdline:|cmdline:)/ s/[ \t]+/ /g" "$lconf"
        # Normalize module_path hash.
        sed -i -E 's/(module_path:.*)#[0-9a-fA-F]+/\1/g' "$lconf"
        ok "Injected cmdlines and normalized hash paths in $lconf"
    done
}

# Update mkinitcpio MODULES. First drive chwd to remove its nvidia injection,
# then remove stray GPU drop-ins, then normalize the base MODULES= line.
update_mkinitcpio_modules() {
    local to_add="$1"
    local file="/etc/mkinitcpio.conf"

    # Step 1 — chwd owns the nvidia MODULES injection via 10-chwd.conf. If we know
    # a profile, drive chwd --remove to drop it; otherwise fall back to direct file
    # management of 10-chwd.conf.
    if [ -n "$CHWD_NVIDIA_PROFILE" ]; then
        chwd --remove "$CHWD_NVIDIA_PROFILE" >/dev/null 2>&1 \
            || warn "chwd --remove reported an issue (continuing)."
        CHWD_NVIDIA_PROFILE=""
        # Safeguard: if chwd left a stale 10-chwd.conf that still injects nvidia,
        # back it up and drop it (otherwise the original "still NVIDIA after reboot"
        # bug would silently return). Files regenerated without nvidia are preserved.
        if [ -f /etc/mkinitcpio.conf.d/10-chwd.conf ] && \
           grep -qE 'MODULES\+?=.*\b(nvidia|nvidia_drm|nvidia_modeset|nvidia_uvm)\b' /etc/mkinitcpio.conf.d/10-chwd.conf 2>/dev/null; then
            cp -p /etc/mkinitcpio.conf.d/10-chwd.conf /etc/mkinitcpio.conf.d/10-chwd.conf.bak
            rm -f /etc/mkinitcpio.conf.d/10-chwd.conf
            warn "10-chwd.conf still injected nvidia after chwd --remove — moved it aside (10-chwd.conf.bak)."
        fi
    elif ! command -v chwd &>/dev/null && [ -f /etc/mkinitcpio.conf.d/10-chwd.conf ]; then
        warn "chwd unavailable — backing up and removing 10-chwd.conf directly (fallback)."
        cp -p /etc/mkinitcpio.conf.d/10-chwd.conf /etc/mkinitcpio.conf.d/10-chwd.conf.bak
        rm -f /etc/mkinitcpio.conf.d/10-chwd.conf
    fi

    # Step 2 — Remove stray GPU-related drop-ins under /etc/mkinitcpio.conf.d.
    # Only GPU drop-ins are removed (never e.g. 10-supernova.conf, never 10-chwd.conf,
    # never 10-limine-snapper-sync.conf). A nova* drop-in is only removed if it really
    # references the nova driver.
    for f in /etc/mkinitcpio.conf.d/*.conf; do
        [ -f "$f" ] || continue
        case "$f" in
            *nvidia*.conf|*nouveau*.conf)
                rm -f "$f" ;;
            *nova*.conf)
                if grep -qE '\b(nova|nova_core|nova_drm)\b' "$f" 2>/dev/null; then
                    rm -f "$f"
                fi ;;
        esac
    done

    # Step 3 — Normalize the base MODULES= line: strip all gpu module tokens, then
    # fold whitespace-only parens into MODULES=(), then append to_add once when the
    # line is a single-line MODULES=( ... ). Nothing else is mangled.
    if [ -f "$file" ]; then
        cp -p "$file" "${file}.bak"
        sed -i -E '/^MODULES=/ s/\b(nvidia_modeset|nvidia_uvm|nvidia_drm|nvidia|nova_core|nova_drm|nova|nouveau)\b\??//g' "$file"
        # Collapse runs of whitespace and strip leading/trailing inside the parens.
        # MODULES=( ) -> MODULES=(), MODULES=(a   b) -> MODULES=(a b).
        sed -i -E '/^MODULES=/ s/[[:space:]]+/ /g; /^MODULES=/ s/^MODULES=\( /MODULES=(/; /^MODULES=/ s/ \)/)/g' "$file"
        # Append to_add only on a single-line MODULES=( ... ) line.
        sed -i -E '/^MODULES=\([^)]*\)[[:space:]]*$/ s/^(MODULES=\([^)]*)\)/\1 '"$to_add"')/' "$file"
        # Normalize cosmetic spaces left by the append (e.g. MODULES=( nvidia? ...) or
        # accidental double spaces). Does not change the append semantics.
        sed -i -E '/^MODULES=/ s/^MODULES=\( /MODULES=(/; /^MODULES=/ s/[[:space:]]+\)/)/; /^MODULES=/ s/[[:space:]]{2,}/ /g' "$file"
        ok "Updated early KMS MODULES in $file."
    fi
}

# Rebuild all initramfs images. Wrapped so any real failure aborts cleanly.
rebuild_all_ramdisks() {
    info "1. Rebuilding CachyOS system presets..."
    if command -v limine-mkinitcpio &>/dev/null; then
        limine-mkinitcpio 2>&1 | tail -5 || fail_step "CachyOS preset rebuild (limine-mkinitcpio) failed."
    else
        mkinitcpio -P 2>&1 | tail -5 || fail_step "CachyOS preset rebuild (mkinitcpio -P) failed."
    fi
    ok "CachyOS default initramfs rebuilt."

    info "2. Rebuilding Infinity Scheduler kernel initramfs..."
    local found_infinity=0
    local img ikern
    for img in /boot/vmlinuz-infinity-*; do
        if [ ! -f "$img" ]; then
            continue
        fi
        ikern=$(basename "$img" | sed 's/^vmlinuz-infinity-//')

        if [ -d "/lib/modules/$ikern" ]; then
            found_infinity=1
            info "   Generating initramfs for kernel $ikern ..."
            depmod -a "$ikern" 2>/dev/null || true
            mkinitcpio -k "$ikern" -g "/boot/initramfs-infinity-${ikern}.img" 2>&1 | tail -4 \
                || fail_step "Infinity kernel $ikern initramfs rebuild failed."
            ok "   Rebuilt: /boot/initramfs-infinity-${ikern}.img"
        fi
    done

    if [ "$found_infinity" -eq 0 ]; then
        info "   No active Infinity kernels found in /boot. Skipping."
    fi
}

# Switch to the open target (nova or nouveau — see resolve_open_target). Name kept for
# backward-compat with main().
switch_to_nova() {
    echo ""
    info "Switching GPU stack to $OPEN_TARGET ..."

    resolve_nvidia_chwd_profile
    resolve_open_target
    snapshot_safety "$OPEN_TARGET"   # btrfs snapshot first: any boot break is one rollback away
    provision_open_deps

    update_mkinitcpio_modules "$OPEN_MODS"   # performs the chwd --remove inside

    update_cachyos_limine_default "$OPEN_CMD"

    info "Writing modprobe blacklists..."
    ln -sf /dev/null /etc/modprobe.d/nvidia-utils.conf
    rm -f /etc/modprobe.d/nouveau-blacklist.conf
    cat > /etc/modprobe.d/nvidia-blacklist.conf <<'EOF'
blacklist nvidia
blacklist nvidia_modeset
blacklist nvidia_uvm
blacklist nvidia_drm
install nvidia /bin/false
install nvidia_modeset /bin/false
install nvidia_uvm /bin/false
install nvidia_drm /bin/false
EOF
    ok "Hard-blocked proprietary NVIDIA modules."

    # Rebuild ramdisks FIRST
    rebuild_all_ramdisks

    # Patch Limine SECOND
    update_infinity_limine_entries "$OPEN_CMD"

    if command -v limine-update &>/dev/null; then
        info "Refreshing Limine deployment..."
        limine-update 2>&1 | tail -3 || fail_step "limine-update failed."
    fi

    echo ""
    ok "Switch to $OPEN_TARGET complete. Works across both CachyOS and Infinity kernels upon reboot."
}

switch_to_nvidia() {
    echo ""
    info "Switching GPU stack to proprietary NVIDIA..."

    resolve_nvidia_chwd_profile
    resolve_open_target
    snapshot_safety "NVIDIA"   # btrfs snapshot first: any boot break is one rollback away

    if [ -n "$CHWD_NVIDIA_PROFILE" ]; then
        # chwd regenerates 10-chwd.conf with its nvidia MODULES block.
        chwd --install "$CHWD_NVIDIA_PROFILE" >/dev/null 2>&1 \
            || warn "chwd --install reported an issue (continuing)."
    elif [ ! -f /etc/mkinitcpio.conf.d/10-chwd.conf ]; then
        warn "No chwd profile to install — writing a chwd-style 10-chwd.conf drop-in directly (fallback)."
        mkdir -p /etc/mkinitcpio.conf.d
        cat > /etc/mkinitcpio.conf.d/10-chwd.conf <<'EOF'
# Generated by chwd
MODULES+=(nvidia nvidia_modeset nvidia_uvm nvidia_drm)
EOF
    fi

    # Normalize the base MODULES= line to the NVIDIA set (strips any leftover
    # open-driver tokens). CHWD_NVIDIA_PROFILE is cleared so this does not undo
    # the chwd --install above.
    CHWD_NVIDIA_PROFILE=""
    update_mkinitcpio_modules "nvidia? nvidia_modeset? nvidia_uvm? nvidia_drm?"

    info "Restoring NVIDIA configurations..."
    # Removing /etc/modprobe.d/nvidia-utils.conf restores the package-provided
    # /usr/lib/modprobe.d/nvidia-utils.conf nouveau/nova blacklist (correct for NVIDIA).
    rm -f /etc/modprobe.d/nvidia-utils.conf
    rm -f /etc/modprobe.d/nvidia-blacklist.conf
    mkdir -p /etc/modprobe.d
    cat > /etc/modprobe.d/nouveau-blacklist.conf <<'EOF'
blacklist nouveau
blacklist nova
blacklist nova_core
blacklist nova_drm
EOF
    echo "options nvidia_drm modeset=1" > /etc/modprobe.d/nvidia-modeset.conf
    ok "Configured NVIDIA DRM modesetting and restored modules."

    update_cachyos_limine_default "nvidia_drm.modeset=1"

    # Rebuild ramdisks FIRST
    rebuild_all_ramdisks

    # Patch Limine SECOND
    update_infinity_limine_entries "nvidia_drm.modeset=1"

    if command -v limine-update &>/dev/null; then
        info "Refreshing Limine deployment..."
        limine-update 2>&1 | tail -3 || fail_step "limine-update failed."
    fi

    echo ""
    ok "Switch to NVIDIA complete. Works across both CachyOS and Infinity kernels upon reboot."
}

main() {
    check_root
    resolve_nvidia_chwd_profile
    resolve_open_target

    local current_state
    current_state="$(detect_driver_state)"
    info "Current running kernel: $(uname -r)"

    case "$current_state" in
        NVIDIA)
            read -rp "You are currently using NVIDIA. Do you want to switch to ${OPEN_TARGET}? [y/N] " ans
            case "$ans" in [yY]|[yY][eE][sS]) switch_to_nova ;; *) echo "Aborted."; exit 0 ;; esac
            ;;
        OPEN)
            read -rp "You are currently using ${OPEN_TARGET}. Do you want to switch to NVIDIA? [y/N] " ans
            case "$ans" in [yY]|[yY][eE][sS]) switch_to_nvidia ;; *) echo "Aborted."; exit 0 ;; esac
            ;;
        NONE)
            read -rp "No GPU driver detected. Do you want to switch to NVIDIA instead? [y/N] " ans
            case "$ans" in [yY]|[yY][eE][sS]) switch_to_nvidia ;; *) echo "Nothing to do.";; esac
            ;;
    esac
}

main "$@"
