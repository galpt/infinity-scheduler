/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2026 Galih Tama <galpt@v.recipes>
 *
 * infinity_sched.h — Infinity scheduler API (dev).
 *
 * Architecture:
 *
 *   fair.c / rt.c (Linux scheduler)   infinity_sched.c (Infinity algorithm)
 *   ────────────────────────────────   ─────────────────────────────────────
 *   update_deadline()       ──call──► infinity_slice()        — fair-share slice
 *   update_curr()           ──call──► infinity_consume()      — EMA budget consumption
 *   update_curr()           ──call──► infinity_vruntime_scale() — EMA vruntime scaling
 *   enqueue_task_fair()     ──call──► infinity_wakeup()       — EMA decay on wakeup
 *   dequeue_task_fair()     ──call──► (records last_sleep_ns) — sleep tracking
 *   pick_eevdf()            ──check──► futex_waiting          — bypass protect_slice
 *   update_curr_rt()        ──call──► (stock RT scheduler, no Infinity hooks)
 *   enqueue_task_rt()       ──call──► (stock RT scheduler)
 *   dequeue_task_rt()       ──call──► (stock RT scheduler)
 *   task_fork_fair()        ──call──► infinity_fork_init()    — fork init
 *   init/init_task.c        ──init──► infinity.{}             — static init
 *   place_entity()          ──call──► infinity_wakeup_scale() — asymptotic vslice on wakeup
 *
 * Tunables:
 *   kernel.infinity_smt_divisor   — SMT secondary slice divisor (default 2)
 *   kernel.infinity_running       — read-only flag, 1 if active
 *
 * The carriage_ns (base fair-share window) is auto-scaled from CPU count at
 * init, matching stock EEVDF's CPU-count scaling behaviour.  No tunable needed.
 *
 * Deadline tracking uses the kernel's built-in hrtick_start() mechanism rather
 * than a custom hrtimer — this avoids the lock inversion (rq->lock vs
 * cpu_base->lock) that a raw hrtimer would introduce inside scheduler locks.
 *
 * Self-stabilizing by construction: the EMA naturally converges between
 * 0 and BUDGET_MAX without any clamps or external feedback loop.
 * Higher EMA → shorter time slice and faster vruntime advance.
 */

#ifndef __INFINITY_SCHED_H
#define __INFINITY_SCHED_H

#include <linux/sched.h>

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

/** Default base fair-share window (2ms, auto-scaled by CPU count). */
#define INFINITY_BASE_CARRIAGE_NS	2000000ULL

/** Maximum budget ceiling (6ms). */
#define INFINITY_BUDGET_MAX_NS		6000000ULL

/**
 * EMA time constant: τ = BUDGET_MAX × FP_ONE / ALPHA.
 * α = 16 gives τ = 6ms × 256 / 16 = 96ms for climb.
 */
#define INFINITY_EMA_ALPHA		16

/**
 * Decay is 4× faster than climb: τ_decay = τ_climb / 4 = 24ms.
 * Faster decay means interactive tasks recover their EMA more quickly
 * during brief sleeps (e.g., GPU wait), without losing the asymptotic
 * convergence guarantee of the limit concept.
 */
#define INFINITY_EMA_DECAY_DIV		4

/** Fixed-point shift for fractional precision (8 bits). */
#define INFINITY_FP_SHIFT		8
#define INFINITY_FP_ONE			(1 << INFINITY_FP_SHIFT)

/**
 * Vruntime scaling slope: × 8/10 (max 5× at EMA=100%).
 * An × 8/10 slope ensures that short-lived initialization bursts which
 * saturate the EMA within a few hundred milliseconds disqualify from
 * EEVDF selection only briefly before the burst passes.
 *
 * At max scaling: 100 / (100 - 80) = 5×.
 * The denominator is always ≥ 20, so the scaling is bounded and cannot
 * diverge regardless of EMA input.
 */
#define INFINITY_VRUNTIME_SLOPE_NUM	8
#define INFINITY_VRUNTIME_SLOPE_DEN	10

/* ------------------------------------------------------------------ */
/* SMT divisor bounds                                                  */
/* ------------------------------------------------------------------ */

#define INFINITY_SMT_DIVISOR_DEFAULT	2
#define INFINITY_SMT_DIVISOR_MIN	1
#define INFINITY_SMT_DIVISOR_MAX	16

/**
 * Effective EMA with two-pole correction.
 *
 * Subtracts half the rate-of-change from the raw EMA, so oscillating
 * workloads (interactive tasks with alternating compute/sleep) receive
 * a systematic boost over sustained CPU-bound tasks.  A CPU-bound task
 * at steady state (dEMA ≈ 0) gets no correction — full penalty applies.
 */
static inline u64 infinity_effective_ema(struct infinity_ctx *ctx)
{
	s64 d = (s64)ctx->ema - (s64)ctx->prev_ema;
	s64 effective = (s64)ctx->ema - (d >> 1);
	if (effective < 0)
		return 0;
	return (u64)effective;
}

/* ------------------------------------------------------------------ */
/* RT EMA constants                                                    */
/* ------------------------------------------------------------------ */

/** RT budget ceiling (10ms — larger than fair to give RT tasks runway). */
#define INFINITY_RT_BUDGET_NS		10000000ULL

/** RT alpha: same time constant as fair path. */
#define INFINITY_RT_ALPHA		4

/**
 * Bounded priority decay window (max 8 slots).
 *
 * A wider window (30) would allow a misbehaving RT audio or compositor
 * thread to demote far below its dependent supervisor, creating a
 * priority inversion deadlock.  Compressing to 8 provides meaningful
 * separation (a priority-45 task can drop to at most 53) without
 * letting the gap cross typical dependency boundaries.
 */
#define INFINITY_RT_PRIO_RANGE		8

/** Hard floor — RT never decays below this (MAX_RT_PRIO - 1 = 98). */
#define INFINITY_RT_PRIO_FLOOR		(MAX_RT_PRIO - 1)

/* ------------------------------------------------------------------ */
/* External sysctl tunables                                            */
/* ------------------------------------------------------------------ */

extern unsigned long infinity_tune_smt_divisor;

/* ------------------------------------------------------------------ */
/* API — called from fair.c and rt.c                                   */
/* ------------------------------------------------------------------ */

u64 infinity_slice(unsigned long nr_runnable, bool on_smt_secondary, u64 ema);
void infinity_consume(struct infinity_ctx *ctx, u64 delta_ns);
void infinity_wakeup(struct infinity_ctx *ctx, u64 sleep_ns);
void infinity_fork_init(struct infinity_ctx *ctx, u64 now);

/*
 * infinity_wakeup_scale — scale vslice asymptotically on wakeup
 *
 * Called from place_entity() to shorten the vslice of a waking task
 * as a continuous function of its effective EMA.  The vslice
 * approaches zero as EMA → 0 (instant scheduling on wakeup) and
 * approaches the nominal vslice as EMA → BUDGET_MAX.
 *
 * The +1 term guarantees a positive vslice for EEVDF tree placement.
 */
u64 infinity_wakeup_scale(u64 vslice, struct infinity_ctx *ctx);

/**
 * infinity_vruntime_scale - Scale vruntime advancement by EMA
 * @vdelta: Nominal virtual runtime increment
 * @p:     Task whose vruntime is being advanced
 *
 * Adjusts the pace of vruntime accumulation for CPU-bound tasks (high EMA)
 * while preserving latency-sensitive interactive tasks.  Two bypass mechanisms
 * prevent throttling of interactive workloads:
 *
 *   - Utilization clamping: if the task has set sched_util_min > 0 via
 *     sched_setattr(), it declared itself interactive — honor that.
 *   - Hardware-wakeup tracking: if the task was recently woken by a threaded
 *     IRQ handler, it gets a 50ms vruntime grace period.
 *
 * Return: Optimized virtual runtime delta.
 */
u64 infinity_vruntime_scale(u64 vdelta, struct task_struct *p);

void infinity_rt_consume(struct infinity_ctx *ctx, u64 delta_ns);
void infinity_rt_wakeup(struct infinity_ctx *ctx, u64 sleep_ns);
unsigned int infinity_rr_timeslice(struct task_struct *p,
				   unsigned int rr_default);

#endif /* __INFINITY_SCHED_H */
