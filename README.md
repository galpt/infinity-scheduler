# Infinity Scheduler — Benchmark Archive

This directory contains benchmark results comparing the **Infinity Scheduler** against three mainstream Linux CPU schedulers available on CachyOS. All benchmarks were conducted on identical hardware across four kernel boots using a slightly modified version of the [CachyOS Mini-Benchmarker](https://github.com/CachyOS/cachyos-benchmarker).

## Understanding The Infinity Scheduler

This section explains the Infinity scheduler by examining how each scheduler in this comparison works at the source level and what the Infinity scheduler was designed to address.

### How Linux Schedules Tasks: The Four Approaches

Every CPU scheduler must answer the same question: given N runnable tasks on a single CPU, which one runs next, and for how long? Each scheduler answers this question differently by trading off fairness, throughput, latency, and complexity.

#### EEVDF (kernel/sched/fair.c, ~13700 lines)

The default Linux scheduler since kernel 6.6. Each task is placed in a red-black tree ordered by its virtual deadline. The `pick_eevdf()` function walks the tree and picks the leftmost task whose virtual runtime has caught up to the average (`entity_eligible()`). The deadline is computed as:

```
deadline = vruntime + base_slice / weight
```

where `base_slice` is a fixed ~0.75 ms (scaled by CPU count) and `weight` is derived from the task's nice value. When a task sleeps and wakes up, its lag is mathematically preserved by `place_entity()` so it does not lose its position in the tree.

**Result:** EEVDF provides provable fairness — CPU-bound tasks are charged for runtime and cannot starve each other. But there is no mechanism to detect or accelerate interactive tasks beyond their static nice value. A compilation thread and a text editor with the same nice value receive equal time slices, even though the editor only needs microsecond bursts of CPU.

#### BORE (Burst-Oriented Response Enhancer, kernel/sched/bore.c + fair.c hooks, ~580 lines)

BORE builds on top of EEVDF by adding a `bore_ctx` to each task that tracks `burst_time` — the continuous time the task has been running without sleeping. While the task runs, `update_curr_bore()` accumulates this time and computes a penalty using `calc_burst_penalty()`:

```
greed = log2(burst_time)  (fixed-point)
penalty = max(greed - offset, 0) * scale
```

When the task sleeps, `restart_burst_bore()` resets the burst to zero. The penalty is added to the task's static priority, which changes its weight → the task's deadline in the EEVDF tree moves further out → the task is scheduled less frequently. A task that runs in a long burst (compilation, rendering) accumulates a high penalty and gets a shorter share. An interactive task that sleeps after a few microseconds of work keeps a minimal penalty.

BORE also includes a burst inheritance system: when a new process is forked, it inherits the penalty of its parent's subtree (other child processes), so a fork-heavy workload immediately knows its expected burst profile. A futex-waiting task is exempted from certain penalties, which helps GUI applications that synchronize via futexes.

**Result:** BORE improves desktop interactivity significantly. The trade-off is that BORE has several tunable parameters (smoothness, penalty_offset, penalty_scale, cache_lifetime, inherit_type) that affect behavior, and the burst detection is purely heuristic — it measures continuous runtime but cannot distinguish between "compute" and "interactive" burst patterns by themselves.

#### BMQ (BitMap Queue, kernel/sched/alt_core.c + bmq.h, ~7800 lines)

BMQ replaces the entire Linux scheduler with a fundamentally different approach. Each CPU has a runqueue consisting of an array of FIFO lists, one per priority level. A bitmap tracks which priority levels have runnable tasks. To pick the next task, the scheduler finds the lowest-numbered bit set in the bitmap and runs the first task in that FIFO queue. Each task gets a fixed 4 ms time slice.

Priority is determined by two things: the task's static nice value, and a dynamic boost/penalty value bounded between [-12, 0]. When a task uses its entire time slice, its boost decreases by 1 (it gets demoted). When a task blocks (sleeps or waits for I/O) early within its time slice, its boost increases by 1 (up to a maximum of 0). This means an interactive task that runs for only a few hundred microseconds before waiting for more input will quickly rise to the highest priority queue, while a CPU-bound task that uses its full 4 ms will sink to lower priority levels.

**Result:** BMQ has the lowest scheduling overhead of all four schedulers — O(1) enqueue and dequeue. The interactivity boost is simple and effective, but BMQ provides no fairness guarantees: a nice -20 task always dominates a nice +19 task regardless of behavior, and within the same priority it is simple FIFO round-robin. There is no concept of virtual runtime, deadlines, or proportional fairness. BMQ also does not support SCHED_DEADLINE, sched-ext, or core scheduling.

#### Infinity Scheduler (kernel/sched/infinity_sched.c + infinity_sched.h, ~430 lines)

The Infinity scheduler does not replace the scheduler like BMQ, nor does it add burst tracking like BORE. Instead, it introduces a continuous **Exponential Moving Average (EMA)** that tracks each task's CPU consumption history and uses it to modulate time slices directly within the existing EEVDF framework.

Each task has an `infinity_ctx` in its `task_struct` containing two 8-byte fields: `ema` (for fair-class tasks) and `rt_ema` (for real-time tasks). While the task runs, the EMA converges toward an upper bound (`INFINITY_BUDGET_MAX_NS` = 2 ms):

```
ema = ema + (BUDGET_MAX - ema) * α / 256
```

While the task sleeps, the EMA is decayed toward 0 on wakeup by the same α per tick slept. No clamps are applied to the EMA — it self-stabilizes through pure arithmetic convergence. This property is called Limitless in the source code (lines 229-231 and 258-261 of `infinity_sched.c`).

The time slice is modulated based on the EMA value in `infinity_slice()`:

```
slice = carriage_ns / nr_runnable
pct = ema * 100 / BUDGET_MAX
slice = slice * (100 - pct * 3 / 4) / 100
```

A task with a low EMA (sleeps often) keeps a full time slice. A task with a high EMA (constant CPU consumption) receives a progressively shorter slice, down to a minimum of 500 µs.

**For real-time tasks (SCHED_FIFO/SCHED_RR):** The Infinity scheduler is the only one among these four that modulates RT priority. A separate `rt_ema` tracks RT CPU consumption with a slow climb rate (α = 1/64 per wakeup) and fast decay rate (β = 1/4 per sleep). The effective RT priority is:

```
effective_prio = base_prio - rt_ema * PRIO_RANGE / RT_BUDGET
```

This means an RT task that runs for a long continuous period has its priority smoothly lowered, but never below `MAX_RT_PRIO - 1` (priority 98), ensuring it still has priority over fair-class tasks. A well-behaved RT task that runs briefly between sleeps maintains its full base priority.

The kernel's existing RT throttling mechanism (`update_curr_rt` in `kernel/sched/rt.c`) works differently: it tracks cumulative RT runtime per CPU and when a runtime budget expires, the entire RT runqueue is throttled until the next period — a binary on/off switch that affects all RT tasks on that CPU equally. The Infinity RT EMA provides a per-task, gradual alternative.

### What This Means in Practice

**Normal tasks (SCHED_OTHER):**

- CPU-bound tasks naturally slow down over time as their EMA rises — the desktop stays responsive without manual nice value tuning.
- The EMA self-stabilizes: install and forget. There are no parameters to tune per workload (carriage_ns and debt_cap exist as safe-guarded sysctls but the defaults work for general use).
- Fork bombs: `infinity_fork_init()` (line 268-274) initializes child tasks with `ema = 0`, giving new processes a fresh budget. They do not inherit the parent's consumption history.

**RT tasks (SCHED_FIFO/SCHED_RR):**

- Audio or game threads that exceed their expected CPU budget get a smooth priority reduction instead of the hard crackle or stutter that would occur from a sudden preemption or RT throttling event.
- Well-behaved RT tasks (short burst, long sleep) are unaffected — their `rt_ema` decays quickly.
- A buggy RT application that enters an infinite loop loses priority gradually rather than locking the entire system.
- Each task's RT EMA is tracked independently — there is no global throttling that cycles on/off for all RT tasks on a CPU.
- Kernel RT threads are included: a runaway kernel thread yields CPU smoothly instead of monopolizing a core.

### Comparing All Four Schedulers Side by Side

| Scheduler | How it tracks task behavior | How it throttles CPU hogs | RT task handling | Code footprint |
|-----------|----------------------------|---------------------------|------------------|----------------|
| **EEVDF** | Virtual runtime accumulated over lifetime | Fair share via vruntime/deadline progression — no burst detection | Standard SCHED_FIFO/RR, no per-task CPU tracking | ~13700 lines (fair.c) |
| **BORE** | Burst time: continuous runtime since last sleep | Adds penalty to priority (changes weight), which extends deadline | No RT changes — only affects SCHED_OTHER | ~580 lines added |
| **BMQ** | Time slice usage: used full slice vs. blocked early | Demotes priority on full-slice use; promotes on early block | RT tasks use same priority queue — O(n) insertion | ~7800 lines (replaces scheduler) |
| **Infinity** | EMA: asymptotic consumption history | Reduces time slice directly based on EMA value | Per-task RT EMA modulates priority smoothly | ~430 lines added |

### Caveats

The benchmark results in this archive cover throughput, compilation, rendering, and latency tests. These are primarily compute-bound and fixed-duration workloads that converge toward steady-state CPU consumption. The Infinity scheduler's EMA mechanism is designed to show its strength in mixed-interactive scenarios — wakeup storms, fork bursts, gaming, and real-time multimedia — where tasks with different sleep/wake patterns compete for the same core. The benchmark results may not fully capture these benefits.

The Infinity scheduler is under active development. It has been tested on the v2-rt branch against the workloads shown here, but production deployments may expose edge cases not yet covered. The source code (430 lines total) is concise enough to review in full for anyone evaluating it for their use case.

## Hardware

| Component | Specification |
|-----------|---------------|
| CPU | AMD Ryzen 7 6800H (8 cores / 16 threads) |
| RAM | 64 GB DDR5-4800 (2×32 GB) |
| GPU | Radeon Graphics (integrated) |
| Storage | NVMe SSD |

## Kernels Tested

| # | Kernel | Scheduler | Version |
|---|--------|-----------|---------|
| 1 | **Infinity Scheduler** | `infinity-sched` (v2-rt branch) | `7.1.1-infinity` |
| 2 | **CachyOS Default** | Tuned EEVDF | `7.1.1-2-cachyos` |
| 3 | **CachyOS BORE** | BORE (Burst-Oriented Response Enhancer) | `7.1.1-1-cachyos-bore` |
| 4 | **CachyOS BMQ** | BMQ (BitMap Queue) | `7.0.12-1-cachyos-bmq` |

## Benchmark Suite

15 tests from the CachyOS Mini-Benchmarker (v2.2), grouped into two categories:

### Category 1 — Throughput & Compilation (↓ lower is better)

| # | Test | Type |
|---|------|------|
| 1 | stress-ng cpu-cache-mem | CPU, memory & cache stress |
| 2 | y-cruncher pi 1b | Floating-point (π calculation) |
| 3 | perf sched msg fork thread | Interprocess communication |
| 4 | perf memcpy | Memory throughput |
| 5 | NAMD 92K atoms | Molecular dynamics simulation |
| 6 | primesieve (6.66×10¹¹) | Prime number search |
| 7 | argon2 hashing | Memory-hard hashing |
| 8 | ffmpeg compilation | Source compilation workload |
| 9 | xz compression | File compression |
| 10 | kernel defconfig | Kernel configuration build |
| 11 | blender render (BMW) | CPU-only 3D rendering |
| 12 | x265 encoding | Video encoding |

### Category 2 — Scheduler Latency

| # | Test | Type | Direction |
|---|------|------|-----------|
| 13 | schbench wakeup latency | Wakeup latency benchmark (P99) | ↓ lower is better |
| 14 | schbench throughput | Wakeup throughput (requests/sec) | ↑ higher is better |
| 15 | cyclictest | Scheduling latency (max latency) | ↓ lower is better |

## Raw Results

All times in seconds for Category 1 (↓ lower is better).
Scheduler-specific metrics in their respective units for Category 2.

### Category 1 — Throughput & Compilation

| Benchmark | Infinity | EEVDF | BORE | BMQ |
|-----------|----------|-------|------|-----|
| stress-ng cpu-cache-mem | 15.13 | 14.66 | 14.41 | 14.52 |
| y-cruncher pi 1b | 42.29 | 42.08 | 41.96 | 41.58 |
| perf sched msg fork thread | 11.338 | 9.856 | 10.439 | 10.812 |
| perf memcpy | 10.18 | 10.31 | 10.21 | 10.13 |
| namd 92K atoms | 52.27 | 51.90 | 58.53 | 60.10 |
| calculating prime numbers | 12.968 | 13.480 | 14.344 | 13.324 |
| argon2 hashing | 7.76 | 7.91 | 8.92 | 7.55 |
| ffmpeg compilation | 65.44 | 65.33 | 76.44 | 65.51 |
| xz compression | 54.90 | 55.26 | 68.53 | 55.03 |
| kernel defconfig | 136.14 | 133.56 | 140.13 | 132.28 |
| blender render | 101.67 | 101.20 | 102.45 | 101.92 |
| x265 encoding | 23.66 | 24.60 | 23.72 | 22.31 |
| **Total time (s)** | **533.75** | **530.15** | **570.08** | **535.07** |

### Category 2 — Scheduler Latency

| Metric | Infinity | EEVDF | BORE | BMQ | Direction |
|--------|----------|-------|------|-----|-----------|
| schbench P99 latency | 122.00 µs | 389.00 µs | 19.00 µs | 8.00 µs | ↓ lower is better |
| schbench avg RPS | 1916.23 | 1836.00 | 1916.73 | 1667.90 | ↑ higher is better |
| cyclictest max latency | 360.00 µs | 1191.00 µs | 438.00 µs | 499.00 µs | ↓ lower is better |

## Directory Structure

| Directory | Contents |
|-----------|----------|
| `results-infinity/` | Raw logs for the Infinity Scheduler kernel |
| `results-cachyos/` | Raw logs for default CachyOS (tuned EEVDF) kernel |
| `results-bore/` | Raw logs for CachyOS BORE kernel |
| `results-bmq/` | Raw logs for CachyOS BMQ kernel |
| `results-all/` | Combined comparison charts, logfiles, and consolidated CSV/JSON across all 4 kernels |

## Visualizations

The `categorized_comparison_All.png` chart in `results-all/` shows two sections per kernel:

- **Top section** — Category 1: Throughput & Compilation benchmarks
- **Bottom section** — Category 2: Scheduler Latency metrics with per-metric direction labels

A secondary cross-kernel comparison chart (`kernel_version_comparison_All.png`) groups every kernel side-by-side per benchmark. Open `test_performance.html` in any browser for an interactive report.

## Methodology

- Each kernel was tested in a separate boot to ensure no cross-contamination.
- Page cache was dropped before each run.
- All benchmarks ran under KDE Plasma 6 on CachyOS with identical system configurations.
- The `sched-ext` (`scx`) framework was not used — all tests used the kernel's built-in scheduler.
- The benchmarker's downloaded assets were cached and reused across runs to eliminate network variability.
