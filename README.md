# Infinity Scheduler — Benchmark Archive

This directory contains benchmark results comparing the **Infinity Scheduler** against three mainstream Linux CPU schedulers available on CachyOS. All benchmarks were conducted on identical hardware across four kernel boots using the [CachyOS Mini-Benchmarker](https://github.com/CachyOS/cachyos-benchmarker).

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

> [!NOTE]
> The Infinity Scheduler source used for this benchmark is from the [v2-rt](https://github.com/galpt/infinity-scheduler/tree/v2-rt) branch.

## Benchmark Suite

12 tests from the CachyOS Mini-Benchmarker (v2.2):

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

## Raw Results

All times in seconds (lower is better).

| Benchmark | Infinity | EEVDF | BORE | BMQ |
|-----------|----------|-------|------|-----|
| stress-ng cpu-cache-mem | 14.54 | 14.62 | 14.29 | 15.12 |
| y-cruncher pi 1b | 42.75 | 42.58 | 42.59 | 42.68 |
| perf sched msg fork thread | 11.78 | 10.04 | 12.26 | 10.37 |
| perf memcpy | 10.37 | 10.86 | 10.87 | 10.17 |
| namd 92K atoms | 52.54 | 51.56 | 52.70 | 52.33 |
| calculating prime numbers | 13.19 | 13.02 | 12.95 | 13.33 |
| argon2 hashing | 7.78 | 7.76 | 7.79 | 7.08 |
| ffmpeg compilation | 66.31 | 65.25 | 65.04 | 65.63 |
| xz compression | 55.06 | 54.36 | 55.45 | 54.73 |
| kernel defconfig | 142.01 | 130.69 | 133.31 | 133.14 |
| blender render | 102.57 | 101.22 | 100.96 | 101.75 |
| x265 encoding | 23.69 | 23.17 | 23.30 | 22.48 |
| **Total time (s)** | **542.58** | **525.13** | **531.51** | **528.81** |
| **Total score** | **73.69** | **71.91** | **73.34** | **71.64** |

## Directory Structure

| Directory | Contents |
|-----------|----------|
| `results-infinity/` | Raw logs for the Infinity Scheduler |
| `results-cachyos/` | Raw logs for default CachyOS (tuned EEVDF) |
| `results-bore/` | Raw logs for CachyOS BORE |
| `results-bmq/` | Raw logs for CachyOS BMQ |
| `results-all/` | Combined comparison charts and consolidated CSV/JSON across all 4 kernels |

## Visualizations

The main comparison chart (`kernel_version_comparison_All.png`) in `results-all/` groups every kernel side-by-side per benchmark. Open `test_performance.html` in any browser for an interactive report.

## Methodology

- Each kernel was tested in a separate boot to ensure no cross-contamination.
- Page cache was dropped before each run.
- All benchmarks ran under KDE Plasma 6 on CachyOS with identical system configurations.
- The `sched-ext` (`scx`) framework was not used — all tests used the kernel's built-in scheduler.
- The benchmarker's downloaded assets were cached and reused across runs to eliminate network variability.
