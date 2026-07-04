/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2026 Galih Tama <galpt@v.recipes>
 *
 * infinity_sched.h — Infinity scheduler API (v4.5).
 *
 * Architecture:
 *
 *   fair.c (Linux scheduler)         infinity_sched.c (Infinity algorithm)
 *   ──────────────────────────       ─────────────────────────────────────
 *   update_deadline()        ──call──► infinity_maybe_reweight() — EMA weight
 *   update_curr()            ──call──► infinity_consume()        — EMA budget
 *   update_curr()            ──call──► infinity_maybe_reweight() — EMA weight
 *   enqueue_task_fair()      ──call──► infinity_wakeup()         — EMA decay
 *   dequeue_task_fair()      ──call──► (records last_sleep_ns)   — sleep tracking
 *   pick_eevdf()             ──check──► futex_waiting            — protect_slice bypass
 *   update_curr_rt()         ──call──► infinity_rt_consume()     — RT EMA climb
 *   enqueue_task_rt()        ──call──► infinity_rt_wakeup()      — RT EMA decay
 *   dequeue_task_rt()        ──call──► (records rt_last_sleep_ns)
 *   task_tick_rt()           ──call──► infinity_rr_timeslice()   — adaptive RR slice
 *   task_fork_fair()         ──call──► infinity_fork_init()      — fork init
 *   init/init_task.c         ──init──► infinity.{}               — static init
 *
 * The weight-based approach replaces the old slice + vruntime scaling:
 * instead of shortening the slice and accelerating vruntime for CPU-bound
 * tasks, we modulate the task's EEVDF weight via reweight_entity().
 * EEVDF natively computes a shorter slice and later deadline from a lower
 * weight — no second level of fairness logic needed.
 *
 * Tunables:
 *   kernel.infinity_smt_divisor   — SMT secondary slice divisor (default 2)
 *   kernel.infinity_running       — read-only flag, 1 if active
 *
 * The carriage_ns (base fair-share window) is auto-scaled from CPU count
 * at init.  Deadline tracking uses the kernel's built-in hrtick_start().
 *
 * Self-stabilizing by construction: the EMA naturally converges between
 * 0 and BUDGET_MAX without any clamps or external feedback loop.
 * Higher EMA → lower effective weight → later deadline.
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
 * α = 32 gives τ = 6ms × 256 / 32 = 48ms for climb,
 * τ_decay = 48 / 4 = 12ms.
 * The 48ms attack catches parallel thread storms (shader compilation)
 * within ~2 scheduler ticks, isolating them from interactive tasks
 * before they cause visible stutter.
 */
#define INFINITY_EMA_ALPHA		32

/**
 * Decay divisor: τ_decay = τ_climb / DIV.
 * Kept at 4 for 1:4 climb/decay asymmetry.
 */
#define INFINITY_EMA_DECAY_DIV		4

/** Fixed-point shift for fractional precision (8 bits). */
#define INFINITY_FP_SHIFT		8
#define INFINITY_FP_ONE			(1 << INFINITY_FP_SHIFT)

/**
 * Weight reduction slope versus EMA percentage: × 8/10.
 * At EMA=100%, weight is reduced by 80%: effective = base × 20/100.
 * The minimum effective weight is base/50 (2% — denom floor at 2).
 * This ensures that even a massive thread storm (32+ shader threads)
 * has less aggregate weight than a single interactive thread, keeping
 * the mouse responsive during background compilation.
 */
#define INFINITY_WEIGHT_SLOPE_NUM	8
#define INFINITY_WEIGHT_SLOPE_DEN	10
#define INFINITY_WEIGHT_DENOM_MIN	2ULL

/* ------------------------------------------------------------------ */
/* SMT divisor bounds                                                  */
/* ------------------------------------------------------------------ */

#define INFINITY_SMT_DIVISOR_DEFAULT	2
#define INFINITY_SMT_DIVISOR_MIN	1
#define INFINITY_SMT_DIVISOR_MAX	16

/**
 * Effective EMA with asymmetric two-pole correction.
 *
 * When EMA is increasing (task accelerating, d > 0), the effective EMA
 * equals the raw EMA — no shielding for thread storms.  When EMA is
 * decreasing (task recovering / sleeping, d < 0), half the rate-of-change
 * is subtracted, boosting the effective EMA above raw so interactive
 * tasks recover their priority faster.
 *
 * At steady state (d ≈ 0) the correction is negligible.
 */
static inline u64 infinity_effective_ema(struct infinity_ctx *ctx)
{
	s64 d = (s64)ctx->ema - (s64)ctx->prev_ema;
	s64 effective;

	if (d > 0)
		effective = (s64)ctx->ema;
	else
		effective = (s64)ctx->ema - (d >> 1);

	if (effective < 0)
		return 0;
	return (u64)effective;
}

/* ------------------------------------------------------------------ */
/* Weight calculation from EMA                                          */
/* ------------------------------------------------------------------ */

/**
 * infinity_calc_weight — Compute EMA-modulated EEVDF weight.
 * @p:    Task whose weight to compute.
 * @ema:  Current effective EMA (from infinity_effective_ema).
 *
 * The base weight comes from @p's static priority (user's nice value).
 * The EMA modulates it:
 *   effective = base × (100 - pct × 8/10) / 100
 * with a floor of base/10 to prevent complete starvation.
 *
 * Tasks with uclamp_min > 0 are bypassed (return their base weight).
 *
 * Return: Effective weight for EEVDF.
 */
static inline u32 infinity_calc_weight(struct task_struct *p, u64 ema)
{
	u32 base = p->se.load.weight;

#ifdef CONFIG_UCLAMP_TASK
	if (p->uclamp_req[UCLAMP_MIN].value > 0)
		return base;
#endif

	if (ema > INFINITY_BUDGET_MAX_NS)
		ema = INFINITY_BUDGET_MAX_NS;

	if (ema) {
		u64 pct = ema * 100ULL / INFINITY_BUDGET_MAX_NS;
		u64 denom = 100ULL - pct * INFINITY_WEIGHT_SLOPE_NUM /
				      INFINITY_WEIGHT_SLOPE_DEN;
		if (denom < INFINITY_WEIGHT_DENOM_MIN)
			denom = INFINITY_WEIGHT_DENOM_MIN;
		return (u32)max(1ULL, base * denom / 100ULL);
	}
	return base;
}

/* ------------------------------------------------------------------ */
/* RT EMA constants                                                    */
/* ------------------------------------------------------------------ */

/** RT budget ceiling (10ms — larger than fair to give RT tasks runway). */
#define INFINITY_RT_BUDGET_NS		10000000ULL

/** RT alpha: same time constant as fair path. */
#define INFINITY_RT_ALPHA		4

/* ------------------------------------------------------------------ */
/* External sysctl tunables                                            */
/* ------------------------------------------------------------------ */

extern unsigned long infinity_tune_smt_divisor;

/* ------------------------------------------------------------------ */
/* API — called from fair.c and rt.c                                   */
/* ------------------------------------------------------------------ */

void infinity_consume(struct infinity_ctx *ctx, u64 delta_ns);
void infinity_wakeup(struct infinity_ctx *ctx, u64 sleep_ns);
void infinity_fork_init(struct infinity_ctx *ctx, u64 now);

void infinity_rt_consume(struct infinity_ctx *ctx, u64 delta_ns);
void infinity_rt_wakeup(struct infinity_ctx *ctx, u64 sleep_ns);
unsigned int infinity_rr_timeslice(struct task_struct *p,
				   unsigned int rr_default);

#endif /* __INFINITY_SCHED_H */
