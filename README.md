# infinity-scheduler

A fair-share CPU scheduler with accelerating budget consumption — the more a task runs, the faster its budget depletes. Interactive tasks reset this effect on wakeup. Built into CFS/EEVDF, no BPF or sched-ext dependency.

## Project structure

```
.
│
├── src/                           ★ Reference implementation (kernel/sched/infinity_sched.[ch])
│   ├── infinity_sched.h            Public API: constants, struct infinity_ctx, function declarations
│   └── infinity_sched.c            Algorithm: fair-share slice, accelerating consumption, fork init
│
├── patches/
│   ├── stable/
│   │   ├── linux-7.0.12-infinity/  Kernel 7.0.12
│   │   │   └── 0001-*.patch
│   │   ├── linux-6.18-infinity/    Kernel 6.18 LTS
│   │   │   └── 0001-*.patch
│   │   ├── linux-7.1-infinity/     Kernel 7.1
│   │   │   └── 0001-*.patch
│   │   └── ...                     Future kernel versions
│
├── tools/
│   ├── install-infinity-scheduler.sh   ★ One-command install
│   ├── build-kernel.sh             Standalone kernel build helper
│   ├── adapt-patches.py            Auto-adapt patches for other kernel versions
│   ├── fix-patch-format.py         Patch sanitization
│   └── fix-patch-counts.py         Hunk count adjustment
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
git clone https://github.com/galpt/infinity-scheduler.git
cd infinity-scheduler
sudo bash tools/install-infinity-scheduler.sh
reboot
```

After reboot, select "Infinity scheduler kernel (...)" at the boot menu.

## How it works

EEVDF functions modified by the Infinity scheduler:

| EEVDF function | Infinity change |
|---|---|
| `update_deadline()` | Fair-share slice via `infinity_slice()` |
| `update_curr()` | Accelerating budget consumption via `infinity_consume()` — the core Limitless technique |
| `enqueue_task_fair()` (wakeup) | Resets `runtime_debt = 0` — the "infinity resets" on sleep |
| `dequeue_task_fair()` (sleep) | Records sleep timestamp for budget refill |
| `task_fork_fair()` | Initializes budget and debt via `infinity_fork_init()` |
| `pick_next_entity()` | NULL guard prevents dereference crash |

No explicit wakeup preemption logic is needed. The accelerating consumption naturally prevents CPU-bound tasks from maintaining a positive budget, while interactive tasks reset their debt on wakeup.

### The Limitless formula

```c
runtime_debt += delta_exec;
rate = 1 + min(runtime_debt, DEBT_CAP) / CARRIAGE_NS;
consumption = delta_exec * rate;
budget -= consumption;
```

A task that has run for 4ms without sleeping: rate = 3x, budget depletes 3x faster.
A task that has run for 20ms without sleeping: rate = 11x, budget depletes 11x faster.

## Feature comparison

| Feature | scx_flow 3.1.0 | infinity-scheduler |
|---|---|---|
| Fair-share slice | Yes | Yes |
| Budget model | Linear consumption | **Accelerating (Limitless)** |
| Explicit preemption | Budget-gated | **None (physics)** |
| Interactive floor | Yes | Yes |
| Sleep cap (250ms) | Yes | Yes |
| SMT halving | No | Yes |
| NULL guard | N/A (BPF) | Yes |
| Work stealing | Yes (BPF) | No |
| RT-stall immunity | No | Yes |

## License

GPL-2.0
