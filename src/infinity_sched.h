/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2026 Galih Tama <galpt@v.recipes>
 *
 * infinity_sched.h — Infinity scheduler API (v4).
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
 *   update_curr_rt()        ──call──► infinity_rt_consume()   — RT EMA climb (priority modulation)
 *   enqueue_task_rt()       ──call──► infinity_rt_wakeup()    — RT EMA time-proportional decay
 *   dequeue_task_rt()       ──call──► (records last_sleep_ns) — sleep tracking for RT
 *   __enqueue_rt_entity()   ──call──► infinity_rt_effective_prio() — RT queue placement
 *   task_fork_fair()        ──call──► infinity_fork_init()    — fork init
 *   init/init_task.c        ──init──► infinity.{}             — static init
 *   place_entity()          ──call──► infinity_wakeup_scale() — asymptotic vslice on wakeup
 *
 * Tunables (sysctl kernel.infinity_*):
 *   infinity_carriage_ns   — base fair-share window (default 2ms)
 *   infinity_smt_divisor   — SMT secondary slice divisor (default 2)
 *   infinity_running       — read-only flag, 1 if active
 *
 * Self-stabilizing by construction: the EMA naturally converges between
 * 0 and BUDGET_MAX without any clamps or external feedback loop.
 * Higher EMA → shorter time slice and faster vruntime advance.
 * v4 adds a fully continuous limit-based design: symmetric time constant
 * for climb and decay (no hard resets), asymptotic vslice on wakeup,
 * time-proportional RT decay, and a two-pole EMA correction that
 * distinguishes oscillating (interactive) from sustained (CPU-bound)
 * workloads.
 */

#ifndef __INFINITY_SCHED_H
#define __INFINITY_SCHED_H

#include <linux/sched.h>

/* ------------------------------------------------------------------ */
/* Tunable defaults                                                    */
/* ------------------------------------------------------------------ */

/** Base fair-share window (2ms). */
#define INFINITY_CARRIAGE_NS_DEFAULT	2000000ULL

/** SMT secondary slice divisor (2 = half slice). */
#define INFINITY_SMT_DIVISOR_DEFAULT	2

/* ------------------------------------------------------------------ */
/* Clamp bounds                                                        */
/* ------------------------------------------------------------------ */

#define INFINITY_CARRIAGE_NS_MIN	1000ULL
#define INFINITY_CARRIAGE_NS_MAX	100000000ULL	/* 100ms */

#define INFINITY_SMT_DIVISOR_MIN	1
#define INFINITY_SMT_DIVISOR_MAX	16

/* ------------------------------------------------------------------ */
/* Hard constants (not user-tunable)                                   */
/* ------------------------------------------------------------------ */

/** Minimum slice floor (400us). */
#define INFINITY_SLICE_MIN_NS		400000ULL

/** Maximum budget (2ms). */
#define INFINITY_BUDGET_MAX_NS		2000000ULL

/** EMA time constant numerator: τ = BUDGET_MAX × FP_ONE / ALPHA (32ms). */
#define INFINITY_EMA_ALPHA		16

/** Fixed-point shift for fractional precision (8 bits). */
#define INFINITY_FP_SHIFT		8
#define INFINITY_FP_ONE			(1 << INFINITY_FP_SHIFT)

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

/** RT alpha: slow climb (1/64 per wakeup). */
#define INFINITY_RT_ALPHA		4

/** RT beta: fast decay (1/4 per sleep). */
#define INFINITY_RT_BETA			64

/** Max priority decay. */
#define INFINITY_RT_PRIO_RANGE		30

/** Hard floor — RT never decays below this (MAX_RT_PRIO - 1 = 98). */
#define INFINITY_RT_PRIO_FLOOR		(MAX_RT_PRIO - 1)

/* ------------------------------------------------------------------ */
/* External sysctl tunables                                            */
/* ------------------------------------------------------------------ */

extern unsigned long infinity_tune_carriage_ns;
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
 * This is the continuous-limit version of the v3 fixed-50%-reduction
 * approach — there is no cap, no threshold, no discrete reduction.
 * The +1 term guarantees a positive vslice for EEVDF tree placement.
 */
u64 infinity_wakeup_scale(u64 vslice, struct infinity_ctx *ctx);

/*
 * infinity_vruntime_scale — scale vruntime advancement by EMA
 *
 * Called from update_curr() to advance vruntime faster for CPU-bound
 * tasks (high EMA).  The scaling factor mirrors infinity_slice(): a
 * task that receives a 25% slice at EMA=100% has its vruntime advanced
 * 4x faster, reducing its effective CPU allocation by the same ratio.
 *
 *   EMA ~= 0             (interactive):   ~1x (normal)
 *   EMA ~= BUDGET_MAX    (CPU-bound):     ~4x
 */
u64 infinity_vruntime_scale(u64 vdelta, u64 ema);

void infinity_rt_consume(struct infinity_ctx *ctx, u64 delta_ns);
void infinity_rt_wakeup(struct infinity_ctx *ctx, u64 sleep_ns);
u8   infinity_rt_effective_prio(u8 base_prio, struct infinity_ctx *ctx);

#endif /* __INFINITY_SCHED_H */
