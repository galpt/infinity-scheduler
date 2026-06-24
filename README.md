# infinity-scheduler

A fair-share CPU scheduler with accelerating budget consumption — the more a task runs, the faster its budget depletes. Interactive tasks reset this effect on wakeup. Built into CFS/EEVDF, no BPF or sched-ext dependency.

> [!TIP]
> The [v2 branch](https://github.com/galpt/infinity-scheduler/tree/v2) has a more refined implementation — no clamps, no external feedback loop, self-stabilizing by construction.

```
.
│
├── src/                                     ★ Reference implementation (kernel/sched/infinity_sched.[ch])
│   ├── infinity_sched.h                    Public API: constants, sysctl declarations, function declarations
│   └── infinity_sched.c                    Algorithm: fair-share slice, accelerating consumption, fork init
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
| `update_curr()` | Accelerating budget consumption via `infinity_consume()` — the core formula |
| `enqueue_task_fair()` (wakeup) | Resets `runtime_debt = 0` — the "infinity resets" on sleep |
| `dequeue_task_fair()` (sleep) | Records sleep timestamp for budget refill |
| `task_fork_fair()` | Initializes budget and debt via `infinity_fork_init()` |
| `pick_next_entity()` | NULL guard prevents dereference crash |

No explicit wakeup preemption logic is needed. The accelerating consumption naturally prevents CPU-bound tasks from maintaining a positive budget, while interactive tasks reset their debt on wakeup.

### Accelerating consumption formula

The Infinity scheduler divides time the same way the Limitless divides space — each unit of
runtime is multiplied by an ever-growing factor, so a CPU-bound task's remaining budget
approaches zero asymptotically but never reaches it.

$$
r = 1 + \frac{d}{\text{CARRIAGE}} \qquad \lim_{t \to \infty} b(t) = B_{\min}
$$

| Symbol | Meaning |
|---|---|
| $r$ | Acceleration factor — how fast this run's budget is consumed right now |
| $d$ | Accumulated runtime since the task last woke up (capped at $\text{CARRIAGE} \times \text{CAP}$) |
| $\text{CARRIAGE}$ | Base fair-share window, default 2ms |
| $\text{CAP}$ | Debt cap multiplier, default 256× |
| $b(t)$ | Remaining budget at time $t$ |
| $B_{\min}$ | Budget clamp floor — the budget stops dropping here, never reaching zero |

**Example:** a task that has run for 4ms without sleeping: $r = 1 + 4\text{ms} / 2\text{ms} = 3$, so its budget depletes 3× faster.  
After 20ms: $r = 1 + 20\text{ms} / 2\text{ms} = 11$, so its budget depletes 11× faster.  
The budget asymptotically approaches $B_{\min}$, exactly like a convergent series approaches its limit — never zero, never undefined.

## Tunables

The scheduler exposes sysctl parameters under `kernel.infinity_*` for live tuning.
All values are clamped at write time to safe ranges — the scheduler can never
enter an invalid state regardless of input.

| Parameter | Default | Range | Description |
|---|---|---|---|
| `infinity_carriage_ns` | 2000000 (2ms) | [1000, 100000000] | Base fair-share window (ns) |
| `infinity_debt_cap` | 256 | [1, 4096] | Runtime debt cap (multiplier × CARRIAGE_NS) |
| `infinity_refill_div` | 100 | [1, 65536] | Budget refill divisor |
| `infinity_smt_divisor` | 2 | [1, 16] | SMT secondary slice divisor (1 = no halving) |
| `infinity_self_stabilize` | 1 | [0, 1] | Automatic tuning |
| `infinity_running` | 1 (ro) | — | Active flag |
| `infinity_reset` | — | — | Write `1` to reset all tunables to defaults |

### Self-stabilize mode

When `self_stabilize = 1` (default), a feedback loop runs every 2 seconds and
monitors per-CPU budget exhaustion rates. If tasks are exhausting budget too
quickly, the carriage window is increased and the debt cap is decreased to
reduce acceleration aggressiveness. If the system is underutilized, the
values are tightened for better interactivity.

To disable auto-tuning and set values manually:

```bash
sysctl kernel.infinity_self_stabilize=0
sysctl kernel.infinity_carriage_ns=4000000     # 4ms base window
sysctl kernel.infinity_debt_cap=128            # less aggressive
```

Re-enable self-stabilize to let the feedback loop resume tuning:

```bash
sudo sysctl kernel.infinity_self_stabilize=1
```

To reset all tunables to their kernel defaults:

```bash
sudo sysctl kernel.infinity_reset=1
```

## Feature comparison

| Feature | scx_flow 3.1.0 | infinity-scheduler |
|---|---|---|
| Fair-share slice | Yes | Yes |
| Budget model | Linear consumption | **Accelerating** |
| Explicit preemption | Budget-gated | **None (inherent)** |
| Interactive floor | Yes | Yes |
| Sleep cap (250ms) | Yes | Yes |
| SMT halving | No | Yes |
| NULL guard | N/A (BPF) | Yes |
| Work stealing | Yes (BPF) | No |
| RT-stall immunity | No | Yes |

## License

GPL-2.0

## Credits

- **[EEVDF](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/kernel/sched/fair.c)** — Earliest Eligible Virtual Deadline First scheduling algorithm by Ion Stoica and Hussein Abdel-Wahab (1995), implemented in the Linux kernel by Peter Zijlstra and the kernel community. EEVDF serves as the foundation that the Infinity scheduler modifies.
- **[scx_flow 3.1.0](https://github.com/sched-ext/scx/tree/main/scheds/experimental/scx_flow)** — BPF sched-ext fair-share scheduler by the sched-ext community. The budget model and interactive floor logic are adapted from this implementation.
- **[BORE](https://github.com/firelzrd/bore-scheduler)** — Burst-Oriented Response Enhancer scheduler by Masahito S ([firelzrd](https://github.com/firelzrd)). BORE's approach to CPU-bound task suppression through burst scoring provided a reference point for Infinity's accelerating consumption design.
- **[BMQ / PDS / LF-BMQ](https://gitlab.com/alfredchen/projectc)** — BitMap Queue schedulers by Alfred Chen (Project C). Research into BMQ's complete scheduler replacement approach validated the decision to keep Infinity within EEVDF rather than replacing it entirely.
