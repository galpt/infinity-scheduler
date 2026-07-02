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
 *   update_curr_rt()        ──call──► infinity_rt_consume()   — RT EMA climb (priority modulation)
 *   enqueue_task_rt()       ──call──► infinity_rt_wakeup()    — RT EMA time-proportional decay
 *   dequeue_task_rt()       ──call──► (records last_sleep_ns) — sleep tracking for RT
 *   __enqueue_rt_entity()   ──call──► infinity_rt_effective_prio() — RT queue placement
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
#include <linux/ktime.h>

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
 * Reduced from × 9/10 (max 10×) to prevent application launch stalls:
 * short-lived initialization bursts that saturate the EMA within a few
 * hundred milliseconds would otherwise have their vruntime advanced by
 * 10×, disqualifying them from EEVDF selection until the burst passes.
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

/** Max priority decay. */
#define INFINITY_RT_PRIO_RANGE		30

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
 * infinity_tag_active — return active subsystem tags (or 0 if expired)
 *
 * Returns the bitmask of active INPUT, GRAPHICS, AUDIO tags, or 0 if
 * the task has no tags or the 50ms expiry window has passed.  Tags are
 * set by hardware-driven kernel paths only — no user-space access.
 *
 * INPUT and AUDIO tasks use negligible CPU, so they get a full bypass
 * (1× vruntime).  GRAPHICS tasks (render threads) can use significant
 * CPU per frame, so they get a gentler slope (max 2× instead of 5×).
 */
static inline unsigned int infinity_tag_active(struct task_struct *p)
{
	unsigned int tags = p->infinity.subsystem_tags;

	if (!tags)
		return 0;
	if (ktime_get_ns() - p->infinity.tag_timestamp > 50000000ULL) {
		p->infinity.subsystem_tags = 0;
		return 0;
	}
	return tags;
}

/*
 * infinity_vruntime_scale — scale vruntime advancement by EMA
 *
 * Called from update_curr() to advance vruntime faster for CPU-bound
 * tasks (high EMA).  Tasks with an active subsystem tag (INPUT,
 * GRAPHICS, AUDIO) bypass the scaling entirely — they always run at
 * their nominal vruntime rate, regardless of their EMA history.
 *
 *   Tagged task:  ~1x (always, regardless of EMA)
 *   Untagged,
 *   EMA ~= 0:     ~1x (normal)
 *   EMA ~= 100%:  ~5x (maximum penalty)
 */
u64 infinity_vruntime_scale(u64 vdelta, struct task_struct *p);

void infinity_rt_consume(struct infinity_ctx *ctx, u64 delta_ns);
void infinity_rt_wakeup(struct infinity_ctx *ctx, u64 sleep_ns);
u8   infinity_rt_effective_prio(u8 base_prio, struct infinity_ctx *ctx);

#endif /* __INFINITY_SCHED_H */
