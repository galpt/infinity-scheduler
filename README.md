# infinity-scheduler

A fair-share CPU scheduler where the more a task runs, the faster its budget runs out — interactive tasks that sleep frequently naturally keep their budget. Same concept applies to real-time tasks through smooth priority modulation. Built into CFS/EEVDF and RT, no BPF or sched-ext dependency.  The v3 branch adds EMA-modulated wakeup vslice for shorter interactive-task deadlines and RT queue placement modulation.

> [!CAUTION]
> This is still being tested, with changes ongoing until it is stable enough for daily use. Please use the [v2-rt](https://github.com/galpt/infinity-scheduler/tree/v2-rt) branch and occasionally check this v3 branch. This notice will be updated once v3 is considered stable.

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
# 1. Clone the v3 branch
git clone -b v3 https://github.com/galpt/infinity-scheduler.git
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

EEVDF and RT functions modified by the Infinity scheduler:

| Function | Infinity replacement |
|---|---|
| `update_deadline()` | Fair-share slice via `infinity_slice()` |
| `update_curr()` | EMA budget consumption via `infinity_consume()` — the core formula |
| `enqueue_task_fair()` (wakeup) | EMA decay catch-up via `infinity_wakeup()` |
| `dequeue_task_fair()` (sleep) | Records sleep timestamp for wakeup decay |
| `enqueue_task_rt()` (wakeup) | EMA climb via `infinity_rt_consume()` — RT priority modulation |
| `dequeue_task_rt()` (block/sleep) | EMA decay via `infinity_rt_wakeup()` |
| `__enqueue_rt_entity()` (v3) | EMA-modulated RT queue placement via `infinity_rt_effective_prio()` |
| `task_fork_fair()` | Initializes budget and EMA via `infinity_fork_init()` |
| `pick_next_entity()` | NULL guard prevents dereference crash |
| `place_entity()` (v3) | EMA-modulated wakeup vslice via `infinity_wakeup_scale()` |

The EMA signal drives two scheduling decisions.  Higher EMA → shorter slice (active throttle via `infinity_slice()`).  Low-EMA (interactive) wakeups receive shortened vslices that place their deadlines earlier in the EEVDF tree.

### EMA consumption formula

The EMA replaces the old accumulator + clamp approach with a smooth asymptotic convergence:

| Operation | Formula | Description |
|---|---|---|
| While running | $ema \mathrel{+}= (B_{\max} - ema) \times \alpha / 256$ | EMA climbs toward `BUDGET_MAX` |
| While sleeping | $ema \mathrel{-}= ema \times \alpha / 256$ | EMA decays toward 0 (catch-up on wakeup) |
| Slice | $slice = share \times (100 - pct \times 3 / 4) / 100$ | Higher EMA → shorter slice (active throttle) |

| Symbol | Meaning |
|---|---|
| `ema` | Exponential moving average — tracks recent runtime history (approaches `BUDGET_MAX` while running, `0` while sleeping) |
| $\alpha = 1/16$ | Decay factor (6.25% per tick) — determines how fast the EMA converges |
| $B_{\max}$ | Maximum budget (2ms) — the EMA never exceeds this bound |
| `slice` | Per-task time slice — shrinks as EMA grows (active throttle) |

**Example:** A task that runs continuously: after 16 ticks (~64ms), $ema \approx 0.63 \times B_{\max}$, receiving a time slice of $slice = share \times (100 - 63 \times 3/4) / 100 \approx 0.53 \times share$. After 256 ticks, $ema$ converges close to $B_{\max}$ and the slice reaches its 500 µs minimum — but EMA never reaches BUDGET_MAX, the true Limitless.

## Tunables

The scheduler exposes sysctl parameters under `kernel.infinity_*` for live tuning.
All values are clamped at write time to safe ranges — the scheduler can never
enter an invalid state regardless of input.

| Parameter | Default | Range | Description |
|---|---|---|---|
| `infinity_carriage_ns` | 2000000 (2ms) | [1000, 100000000] | Base fair-share window (ns) |
| `infinity_smt_divisor` | 2 | [1, 16] | SMT secondary slice divisor (1 = no halving) |
| `infinity_running` | 1 (ro) | — | Active flag |
| `infinity_reset` | — | — | Write `1` to reset all tunables to defaults |

The EMA formula is self-stabilizing by construction — no auto-tuning sysctl is needed.
To set values manually:

```bash
sudo sysctl kernel.infinity_carriage_ns=4000000     # 4ms base window
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
| SMT halving | No | Yes |
| NULL guard | N/A (BPF) | Yes |
| Wakeup deadline boost | N/A | **EMA vslice scaling (v3)** |
| Work stealing | Yes (BPF) | No |
| RT-stall immunity | No | Yes |

EMA budget tracking converges asymptotically without clamps — the Limitless
property holds mathematically.  No external feedback loop, no refill divisor,
no interactive floor constants, no sleep cap, no tunable knobs beyond two
safe-clamped tunable sysctls (plus one read-only flag and one reset trigger).

## License

GPL-2.0

## Credits

- **[EEVDF](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/kernel/sched/fair.c)** — Earliest Eligible Virtual Deadline First scheduling algorithm by Ion Stoica and Hussein Abdel-Wahab (1995), implemented in the Linux kernel by Peter Zijlstra and the kernel community. EEVDF serves as the foundation that the Infinity scheduler modifies.
- **[scx_flow 3.1.0](https://github.com/sched-ext/scx/tree/main/scheds/experimental/scx_flow)** — BPF sched-ext fair-share scheduler by the sched-ext community. The budget model and interactive floor logic are adapted from this implementation.
- **[BORE](https://github.com/firelzrd/bore-scheduler)** — Burst-Oriented Response Enhancer scheduler by Masahito S ([firelzrd](https://github.com/firelzrd)). BORE's approach to CPU-bound task suppression through burst scoring provided a reference point for Infinity's accelerating consumption design.
- **[BMQ / PDS / LF-BMQ](https://gitlab.com/alfredchen/projectc)** — BitMap Queue schedulers by Alfred Chen (Project C). Research into BMQ's complete scheduler replacement approach validated the decision to keep Infinity within EEVDF rather than replacing it entirely.
