# infinity-scheduler

A fair-share CPU scheduler with accelerating budget consumption — the more a task runs, the faster its budget depletes. Interactive tasks reset this effect on wakeup. Built into CFS/EEVDF, no BPF or sched-ext dependency.

## Project structure

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
sysctl kernel.infinity.running                     # → kernel.infinity.running = 1

# The boot log confirms activation
dmesg | grep Infinity                              # → Infinity scheduler active: carriage=...
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

```c
debt = min(runtime_debt + delta_exec, CARRIAGE_NS * DEBT_CAP);
rate = 1 + debt / CARRIAGE_NS;
budget -= delta_exec * rate;
runtime_debt = debt;
```

A task that has run for 4ms without sleeping: rate = 3x, budget depletes 3x faster.
A task that has run for 20ms without sleeping: rate = 11x, budget depletes 11x faster.

## Tunables

The scheduler exposes sysctl parameters under `kernel.infinity.*` for live tuning.
All values are clamped at write time to safe ranges — the scheduler can never
enter an invalid state regardless of input.

| Parameter | Default | Range | Description |
|---|---|---|---|
| `carriage_ns` | 2000000 (2ms) | [1000, 100000000] | Base fair-share window (ns) |
| `debt_cap` | 256 | [1, 4096] | Runtime debt cap (multiplier × CARRIAGE_NS) |
| `refill_div` | 100 | [1, 65536] | Budget refill divisor |
| `smt_divisor` | 2 | [1, 16] | SMT secondary slice divisor (1 = no halving) |
| `self_stabilize` | 1 | [0, 1] | Automatic tuning |
| `running` | 1 (ro) | — | Active flag |

### Self-stabilize mode

When `self_stabilize = 1` (default), a feedback loop runs every 2 seconds and
monitors per-CPU budget exhaustion rates. If tasks are exhausting budget too
quickly, the carriage window is increased and the debt cap is decreased to
reduce acceleration aggressiveness. If the system is underutilized, the
values are tightened for better interactivity.

To disable auto-tuning and set values manually:

```bash
sysctl kernel.infinity.self_stabilize=0
sysctl kernel.infinity.carriage_ns=4000000     # 4ms base window
sysctl kernel.infinity.debt_cap=128            # less aggressive
```

Re-enable self-stabilize to let the feedback loop resume tuning:

```bash
sysctl kernel.infinity.self_stabilize=1
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

- **[EEVDF](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/kernel/sched/fair.c)** — Earliest Eligible Virtual Deadline First scheduling algorithm by Stride, Inc. and the Linux kernel community. EEVDF serves as the foundation that the Infinity scheduler modifies.
- **[scx_flow 3.1.0](https://github.com/sched-ext/scx/tree/main/scheds/experimental/scx_flow)** — BPF sched-ext fair-share scheduler by the sched-ext community. The budget model and interactive floor logic are adapted from this implementation.
- **[BORE](https://github.com/firelzrd/bore-scheduler)** — Burst-Oriented Response Enhancer scheduler by Masahito S ([firelzrd](https://github.com/firelzrd)). Insights from BORE's burst-aware scheduling influenced the self-stabilize feedback approach.
- **[BMQ / PDS / LF-BMQ](https://gitlab.com/alfredchen/projectc)** — BitMap Queue schedulers by Alfred Chen (Project C). Research into BMQ's complete scheduler replacement approach validated the decision to keep Infinity within EEVDF rather than replacing it entirely.
