# infinity-scheduler

A fair-share CPU scheduler with exponential moving average (EMA) budget tracking — the longer a task runs, the higher its EMA climbs and the faster its remaining budget depletes. Interactive tasks see their EMA decay naturally during sleep. Built into CFS/EEVDF, no BPF or sched-ext dependency. Self-stabilizing by construction — no external feedback loop needed.

## Project structure

```
.
│
├── src/                                     ★ Reference implementation (kernel/sched/infinity_sched.[ch])
│   ├── infinity_sched.h                    Public API: constants, sysctl declarations, function declarations
│   └── infinity_sched.c                    Algorithm: fair-share slice, EMA consumption, wakeup decay, fork init
│
├── patches/
│   ├── stable/
│   │   ├── linux-7.0.12-infinity/          Kernel 7.0.12
│   │   │   └── 0001-*.patch
│   │   ├── linux-6.18-infinity/            Kernel 6.18 LTS
│   │   │   └── 0001-*.patch
│   │   ├── linux-7.1-infinity/             Kernel 7.1
│   │   │   └── 0001-*.patch
│   │   └── ...                             Future kernel versions
│
├── tools/
│   ├── install-infinity-scheduler.sh        ★ One-command install
│   ├── build-kernel.sh                     Standalone kernel build helper
│   ├── adapt-patches.py                    Auto-adapt patches for other kernel versions
│   ├── fix-patch-format.py                 Patch sanitization
│   └── fix-patch-counts.py                 Hunk count adjustment
│
├── CODE_OF_CONDUCT.md
├── CONTRIBUTING.md
├── README.md
└── LICENSE
```

> [!NOTE]
> Patches for version X.Y apply to all X.Y.Z point releases with `patch -F 3`.

## Quick start

```bash
# 1. Clone the repository
git clone https://github.com/galpt/infinity-scheduler.git
cd infinity-scheduler

# 2. Build and install (detects running kernel version automatically)
sudo bash tools/install-infinity-scheduler.sh

# 3. Reboot and select "Infinity scheduler kernel" at the boot menu
reboot
```

> [!TIP]
> `sudo bash tools/install-infinity-scheduler.sh --remove` removes only Infinity scheduler boot entries, kernel images, and initramfs — the default kernel and its boot entries are never touched.

### Verify it's running

```bash
# The kernel version ends with -infinity
uname -r                                           # → 7.0.12-infinity

# The running flag is set
sysctl kernel.infinity_running                     # → kernel.infinity_running = 1

# The boot log confirms activation (requires sudo)
sudo dmesg | grep Infinity                         # → Infinity scheduler active: carriage=...
```

## How it works

EEVDF functions modified by the Infinity scheduler:

| EEVDF function | Infinity replacement |
|---|---|
| `update_deadline()` | Fair-share slice via `infinity_slice()` |
| `update_curr()` | EMA budget consumption via `infinity_consume()` — the core formula |
| `enqueue_task_fair()` (wakeup) | EMA decay catch-up via `infinity_wakeup()` |
| `dequeue_task_fair()` (sleep) | Records sleep timestamp for wakeup decay |
| `task_fork_fair()` | Initializes budget and EMA via `infinity_fork_init()` |
| `pick_next_entity()` | NULL guard prevents dereference crash |

No explicit wakeup preemption logic is needed. The EMA naturally converges toward `BUDGET_MAX` while running and toward `0` while sleeping — the budget is `BUDGET_MAX - ema`, always in `[0, BUDGET_MAX]` without any clamp.

### EMA consumption formula

The EMA replaces the old accumulator + clamp approach with a smooth asymptotic convergence:

```
While running:  ema += (BUDGET_MAX - ema) * α / 256
While sleeping: ema -= ema * α / 256           (via catch-up on wakeup)
budget = BUDGET_MAX - ema                       (naturally bounded, no clamp)
rate   = 1 + ema * DEBT_CAP / BUDGET_MAX        (smooth fixed-point)
consumption = delta_exec * rate                 (8-bit fractional precision)
```

| Symbol | Meaning |
|---|---|
| `ema` | Exponential moving average — tracks recent runtime history (approaches `BUDGET_MAX` while running, `0` while sleeping) |
| `α` (`1/16`) | Decay factor — determines how fast the EMA converges |
| `BUDGET_MAX` | Maximum budget (2ms) — the EMA never exceeds this bound |
| `DEBT_CAP` | Max acceleration multiplier (default 256×) |
| `rate` | Budget consumption rate — climbs from 1× to 257× as ema grows |

**Example:** A task that runs continuously: after 16 ticks (~64ms), `ema ≈ 0.63 × BUDGET_MAX`, budget ≈ 740µs, rate ≈ 1 + 0.63 × 256 ≈ 162×. The budget depletes 162× faster. After 256 ticks, `ema` converges close to `BUDGET_MAX` and budget approaches 0 — but never reaches it, the true Limitless.

## Tunables

The scheduler exposes sysctl parameters under `kernel.infinity_*` for live tuning.
All values are clamped at write time to safe ranges — the scheduler can never
enter an invalid state regardless of input.

| Parameter | Default | Range | Description |
|---|---|---|---|
| `infinity_carriage_ns` | 2000000 (2ms) | [1000, 100000000] | Base fair-share window (ns) |
| `infinity_debt_cap` | 256 | [1, 4096] | Max acceleration multiplier |
| `infinity_smt_divisor` | 2 | [1, 16] | SMT secondary slice divisor (1 = no halving) |
| `infinity_running` | 1 (ro) | — | Active flag |
| `infinity_reset` | — | — | Write `1` to reset all tunables to defaults |

The EMA formula is self-stabilizing by construction — no auto-tuning sysctl is needed.
To set values manually and let the physics handle the rest:

```bash
sudo sysctl kernel.infinity_carriage_ns=4000000     # 4ms base window
sudo sysctl kernel.infinity_debt_cap=128            # less aggressive
```

To reset all tunables to their kernel defaults:

```bash
sudo sysctl kernel.infinity_reset=1
```

## Feature comparison

| Feature | scx_flow 3.1.0 | infinity-scheduler |
|---|---|---|
| Fair-share slice | Yes | Yes |
| Budget model | Linear consumption | **EMA (Limitless)** |
| Explicit preemption | Budget-gated | **None (inherent)** |
| SMT halving | No | Yes |
| NULL guard | N/A (BPF) | Yes |
| Work stealing | Yes (BPF) | No |
| RT-stall immunity | No | Yes |

EMA budget tracking converges asymptotically without clamps — the Limitless
property holds mathematically.  No external feedback loop, no refill divisor,
no interactive floor constants, no sleep cap.  The physics of the EMA is the
only regulation mechanism.

## License

GPL-2.0

## Credits

- **[EEVDF](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/kernel/sched/fair.c)** — Earliest Eligible Virtual Deadline First scheduling algorithm by Ion Stoica and Hussein Abdel-Wahab (1995), implemented in the Linux kernel by Peter Zijlstra and the kernel community. EEVDF serves as the foundation that the Infinity scheduler modifies.
- **[scx_flow 3.1.0](https://github.com/sched-ext/scx/tree/main/scheds/experimental/scx_flow)** — BPF sched-ext fair-share scheduler by the sched-ext community. The budget model and interactive floor logic are adapted from this implementation.
- **[BORE](https://github.com/firelzrd/bore-scheduler)** — Burst-Oriented Response Enhancer scheduler by Masahito S ([firelzrd](https://github.com/firelzrd)). BORE's approach to CPU-bound task suppression through burst scoring provided a reference point for Infinity's accelerating consumption design.
- **[BMQ / PDS / LF-BMQ](https://gitlab.com/alfredchen/projectc)** — BitMap Queue schedulers by Alfred Chen (Project C). Research into BMQ's complete scheduler replacement approach validated the decision to keep Infinity within EEVDF rather than replacing it entirely.
