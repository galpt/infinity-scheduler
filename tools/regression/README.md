# v4.8 regression harness

Purpose: catch latency/throughput regressions in the infinity scheduler
before they reach users. Scenarios mirror the real-world failure classes
reported by users (notably the v4.6-era Alt+Tab lag, issue #16).

## Method

- Run on the target machine as root (perf and cyclictest parts need it).
- Record the environment with every result: kernel version (`uname -r`),
  CPU topology (`lscpu`), cpufreq governor (`cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor`),
  iteration counts, and medians/P99/spread.
- Baseline order: (1) pristine stock 7.1 kernel, (2) v4.7-gpu, (3) v4.8-gpu.
  The sanity floors below are loose; the meaningful signal is the delta
  between baselines measured with the same method.
- Results are committed to the repo together with the full method.

## Scenarios

| Script | Tool | Metric | Floor (sanity) |
|---|---|---|---|
| alt-tab | stress-ng + schbench | wakeup P99 under 14 CPU hogs | < 10 ms |
| wakeup-latency | schbench | wakeup P99, 3 iterations | < 3 ms |
| socket-latency | netperf TCP_RR | transactions/s (P0-1 path) | > 1000/s |
| rt | cyclictest + rogue SCHED_FIFO | max latency with rogue | < 50 ms |
| fork | stress-ng --fork | forks/s | > 2000/s |
| ema-pelt-trace | /proc/<pid>/infinity + perf | divergence pp + wakeup P99 | <= 50 pp / < 500 us |

Missing tools produce a WARN and a skip, never a false FAIL (netperf,
rt-app, bpftrace are not installed on the reference machine).

## P0-3 audit procedure (measurement-only)

The futex ping-pong and DELAY_DEQUEUE x DELAY_ZERO matrix run with the
DEFAULT scheduler features: the 7.1 kernel has no runtime feature toggle
(the sched_features debugfs surface does not exist in this tree), so
alternate feature states require a kernel rebuild. Trace with
`sched:sched_wakeup` and `sched:sched_switch` (see ema-pelt-trace.sh) and
record handoff-latency percentiles; expected outcome per the v4.8 design
review: the futex boost already fires for eligible sleepers.

## Usage

    sudo ./run-v48-regression.sh
