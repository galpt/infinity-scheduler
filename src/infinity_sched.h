/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2026 Galih Tama <galpt@v.recipes>
 *
 * infinity_sched.h — Infinity scheduler API (v3).
 *
 * Architecture:
 *
 *   fair.c / rt.c (Linux scheduler)   infinity_sched.c (Infinity algorithm)
 *   ────────────────────────────────   ─────────────────────────────────────
 *   update_deadline()       ──call──► infinity_slice()        — fair-share slice
 *   update_curr()           ──call──► infinity_consume()      — EMA budget consumption
 *   enqueue_task_fair()     ──call──► infinity_wakeup()       — EMA decay on wakeup
 *   dequeue_task_fair()     ──call──► (records last_sleep_ns) — sleep tracking
 *   enqueue_task_rt()       ──call──► infinity_rt_consume()   — RT EMA climb (priority modulation)
 *   dequeue_task_rt()       ──call──► infinity_rt_wakeup()    — RT EMA decay on block
 *   task_fork_fair()        ──call──► infinity_fork_init()    — fork init
 *   init/init_task.c        ──init──► infinity.{}             — static init
 *   pick_eevdf()            ──call──► infinity_should_yield() — protect_slice bypass (v3)
 *   place_entity()          ──call──► infinity_wakeup_scale() — EMA-modulated wakeup vslice (v3)
 *
 * Tunables (sysctl kernel.infinity_*):
 *   infinity_carriage_ns   — base fair-share window (default 2ms)
 *   infinity_smt_divisor   — SMT secondary slice divisor (default 2)
 *   infinity_running       — read-only flag, 1 if active
 *
 * infinity_ctx reserved fields: fork_time_ns (set), rt_disabled (set).
 * Both are placeholders for future rate-limiting and RT-opt-out logic.
 *
 * Self-stabilizing by construction: the EMA naturally converges between
 * 0 and BUDGET_MAX without any clamps or external feedback loop.
 * Higher EMA → shorter time slice (active throttle via infinity_slice()).
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

/** Minimum slice floor (500us). */
#define INFINITY_SLICE_MIN_NS		500000ULL

/** Maximum budget (2ms). */
#define INFINITY_BUDGET_MAX_NS		2000000ULL

/** EMA alpha: 1/16 of convergence per tick. */
#define INFINITY_EMA_ALPHA		16

/** Fixed-point shift for fractional precision (8 bits). */
#define INFINITY_FP_SHIFT		8
#define INFINITY_FP_ONE			(1 << INFINITY_FP_SHIFT)

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
 * infinity_should_yield — should this fair task yield its protect_slice?
 *
 * Called from pick_eevdf() when protect_slice() would keep the current
 * task running.  Returns true when the current task's EMA is high
 * (CPU-bound behaviour), meaning it is fair to let other tasks preempt.
 *
 * This is distinct from BORE's futex_waiting check: BORE queries a flag
 * on the waking task; this function examines the running task's own
 * consumption history via the EMA — a continuous asymptotic signal
 * that requires no flags, burst tracking, or external state.
 */
bool infinity_should_yield(struct task_struct *p);

/*
 * infinity_wakeup_scale — reduce vslice for low-EMA wakeups
 *
 * Called from place_entity() to shorten the vslice of a waking task
 * whose EMA is low (interactive behaviour).  The reduction is
 * proportional to the sleep depth encoded in the EMA:
 *
 *   EMA ~= 0             (slept long):  ~50% reduction
 *   EMA ~= BUDGET_MAX    (brief sleep):   ~0% reduction
 *
 * This is distinct from BORE's unconditional vslice >>= 1 on wakeup.
 * The scaling here is continuous, not a fixed halving.
 */
u64 infinity_wakeup_scale(u64 vslice, struct infinity_ctx *ctx);

void infinity_rt_consume(struct infinity_ctx *ctx);
void infinity_rt_wakeup(struct infinity_ctx *ctx);
u8   infinity_rt_effective_prio(u8 base_prio, struct infinity_ctx *ctx);

#endif /* __INFINITY_SCHED_H */
