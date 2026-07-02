# infinity-scheduler (dev)

A fair-share CPU scheduler based on the limit concept in mathematics — every scheduling parameter approaches its bound asymptotically without discrete thresholds. Interactive tasks that sleep frequently naturally keep their budget; CPU-bound tasks converge toward a minimum. Same concept applies to real-time tasks through smooth priority modulation. Built into CFS/EEVDF and RT, no BPF or sched-ext dependency.

> [!TIP]
> **TL;DR — dev makes Infinity tick-independent, more aggressive, and self-tuning.**
>
> A per-task hrtimer fires at the exact deadline expiry — no longer dependent on
> the periodic tick. The vruntime scaling slope is × 8/10 (max 5×) — balanced
> for both interactive boosts and application launch latency. Subsystem tags
> (INPUT, GRAPHICS, AUDIO) let hardware drivers explicitly mark interactive
> threads — bypassing EMA heuristics for verified real-time tasks. The slice minimum is
> 50% of fair share (proportional, not a fixed 400µs floor) so the EMA always has
> room to modulate. Carriage_ns auto-scales from CPU count — one less knob to
> worry about. Decay is 4× faster than climb (τ = 24ms vs 96ms) for quicker
> interactive recovery during brief sleeps.

```mermaid
flowchart TB
    classDef fairStroke fill:#fff,stroke:#3b82f6,stroke-width:2
    classDef algoNode fill:#eef2ff,stroke:#6366f1,stroke-width:2
    classDef tagNode fill:#f0fdf4,stroke:#22c55e,stroke-width:2
    classDef wakeNode fill:#f0fdfa,stroke:#14b8a6,stroke-width:2
    classDef rtStroke fill:#fff,stroke:#f59e0b,stroke-width:2
    classDef rtNode fill:#fffbeb,stroke:#d97706,stroke-width:2
    classDef infraNode fill:#f8fafc,stroke:#94a3b8,stroke-width:2

    subgraph FAIR["Fair tasks (SCHED_OTHER)"]
        direction TB

        TASK["Task
 \ 
wakes / sleeps"] --> GAUGE["EMA gauge
 \ 
0 → BUDGET_MAX
 \ 
τ_climb 96ms
τ_decay 24ms"]
        class GAUGE fairStroke

        GAUGE --> TWOPOLE["two-pole correction
 \ 
effective = ema − Δema/2
 \ 
neutral at wakeup"]
        class TWOPOLE algoNode

        TWOPOLE --> SLICE["infinity_slice()
 \ 
EMA↑ → slice↓
 \ 
min 50% of share"]
        class SLICE algoNode

        TWOPOLE --> VRT["infinity_vruntime_scale()
 \ 
×8/10 slope, max 5×"]
        class VRT algoNode

        subgraph TAGS["Subsystem tags (50ms expiry)"]
            INPUT["INPUT
 \ 
evdev_read()"]
            GRAPHICS["GRAPHICS
 \ 
dma_fence_signal()"]
            AUDIO["AUDIO
 \ 
snd_pcm_read()"]
        end
        class INPUT,GRAPHICS,AUDIO tagNode

        TAGS -- "INPUT/AUDIO: 1× bypass" --> VRT
        TAGS -- "GRAPHICS: ×5/10 (max 2×)" --> VRT

        VRT --> UPD["update_curr()
 \ 
vruntime += scaled_delta"]
        class UPD fairStroke

        UPD --> HRTICK["hrtick_start(rq, slice_ns)
 \ 
tick-independent timer
 \ 
fires at exact slice expiry"]
        class HRTICK algoNode

        HRTICK --> PICK["pick_eevdf()
 \ 
EEVDF tree
 \ 
earliest deadline wins"]
        class PICK algoNode

        PICK --> FUTEX["futex_waiting?
 \ 
bypass protect_slice"]
        class FUTEX algoNode

        FUTEX --> RUN["Task runs
 \ 
until block or preempt"]
        class RUN fairStroke

        subgraph WAKEUP["Wakeup path"]
            WQ["enqueue_task_fair()"]
            WQ --> DECAY["infinity_wakeup()
 \ 
ema decays: step = f(sleep_ns)
 \ 
40s cap, 128-bit safety"]
            DECAY --> WUP["infinity_wakeup_scale()
 \ 
vslice' = vslice × ema / BUDGET_MAX
 \ 
→ 0 as ema → 0, no cap"]
            WUP --> PLACE["place_entity()
 \ 
deadline = vruntime + vslice'"]
            PLACE --> PICK
        end
        class DECAY,WUP,PLACE wakeNode

        RUN -. "block / preempt" .-> WAKEUP
        RUN --> GAUGE
    end
    class FAIR fairStroke

    subgraph RT["RT tasks (SCHED_FIFO/RR)"]
        direction TB
        RT_T["RT task runs"] --> RT_C["infinity_rt_consume()
 \ 
EMA climbs with runtime"]
        RT_C --> RT_D["infinity_rt_wakeup()
 \ 
time-proportional decay
 \ 
same τ as fair path
 \ 
dedicated rt_last_sleep_ns"]
        RT_D --> RT_P["infinity_rt_effective_prio()
 \ 
rt_ema↑ → priority↓
 \ 
moved to lower RT queue"]
        RT_P --> RT_Q["RT queue placement
 \ 
gated to root_task_group"]
    end
    class RT_T,RT_Q rtStroke
    class RT_C,RT_D,RT_P rtNode
    class RT rtStroke

    subgraph INFRA["Scheduler infrastructure"]
        AC["carriage_ns
 \ 
auto-scaled from CPU count
 \ 
1 + ilog min(cpus, 8)"]
        OF["sleep decay
 \ 
mul_u64_u64_div_u64
 \ 
128-bit overflow safety"]
        TU["tunables
 \ 
smt_divisor
 \ 
running (ro)"]
    end
    class AC,OF,TU infraNode
    class INFRA infraNode
```

## Quick start

```bash
# 1. Clone the dev branch
git clone -b dev https://github.com/galpt/infinity-scheduler.git
cd infinity-scheduler

# 2. Build and install (detects running kernel version automatically)
sudo bash tools/install-infinity-scheduler.sh

# 3. Reboot and select "Infinity scheduler kernel" at the boot menu
reboot
```

> [!TIP]
> `sudo bash tools/install-infinity-scheduler.sh --remove` removes only Infinity
> scheduler boot entries — the default kernel is never touched.

```bash
# Verify it's running
uname -r                              # → 7.0.12-infinity
sysctl kernel.infinity_running        # → kernel.infinity_running = 1
sudo dmesg | grep Infinity            # → Infinity scheduler active: carriage=...
```

## Project structure

```
.
├── src/                    ★ Reference implementation (kernel/sched/infinity_sched.[ch])
├── patches/stable/         0001-infinity-scheduler.patch for each kernel version
├── tools/                  Install script, build helpers, patch fixers
├── CONTRIBUTING.md
└── LICENSE
```

Patches for version X.Y apply to all X.Y.Z point releases with `patch -F 3`.

## Tunables

| Parameter | Default | Range | Description |
|---|---|---|---|
| `infinity_smt_divisor` | 2 | [1, 16] | SMT secondary slice divisor (1 = no halving) |
| `infinity_running` | 1 (ro) | — | Active flag |

The base fair-share window (`carriage_ns`) is auto-scaled from CPU count at init,
matching stock EEVDF's CPU-count scaling behaviour.  No user tunable is needed.

## Feature comparison

| Feature | scx_flow 3.1.0 | infinity-scheduler |
|---|---|---|
| Fair-share slice | Yes | Yes |
| Budget model | Linear consumption | **EMA (Limitless)** |
| SMT halving | No | Yes |
| NULL guard | N/A (BPF) | Yes |
| Wakeup deadline boost | N/A | **Asymptotic vslice** |
| Work stealing | Yes (BPF) | No (not needed — EEVDF + kernel load balancer) |
| RT-stall immunity | No | Yes |

## License

GPL-2.0

## Credits

- **[EEVDF](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/kernel/sched/fair.c)** — Earliest Eligible Virtual Deadline First scheduling algorithm by Ion Stoica and Hussein Abdel-Wahab (1995), implemented in the Linux kernel by Peter Zijlstra and the kernel community. EEVDF serves as the foundation that the Infinity scheduler modifies.
- **[scx_flow 3.1.0](https://github.com/sched-ext/scx/tree/main/scheds/experimental/scx_flow)** — BPF sched-ext fair-share scheduler by the sched-ext community. The budget model and interactive floor logic are adapted from this implementation.
- **[BORE](https://github.com/firelzrd/bore-scheduler)** — Burst-Oriented Response Enhancer scheduler by Masahito S ([firelzrd](https://github.com/firelzrd)). BORE's approach to CPU-bound task suppression through burst scoring provided a reference point for Infinity's accelerating consumption design.
- **[BMQ / PDS / LF-BMQ](https://gitlab.com/alfredchen/projectc)** — BitMap Queue schedulers by Alfred Chen (Project C). Research into BMQ's complete scheduler replacement approach validated the decision to keep Infinity within EEVDF rather than replacing it entirely.
- **[LINUX DO](https://linux.do/)** — Chinese Linux community where the Infinity scheduler is discussed and promoted. Feedback from the community helps shape the project's development direction.
- **[CachyOS community](https://cachyos.org/)** — Testers and early adopters who provided real-world feedback during development, helping validate the scheduler's behavior under diverse workloads.
