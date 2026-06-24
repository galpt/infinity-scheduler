/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2026 Galih Tama <galpt@v.recipes>
 *
 * infinity_sched.h — Infinity scheduler API.
 *
 * Architecture:
 *
 *   fair.c (EEVDF framework)           infinity_sched.c (Infinity algorithm)
 *   ────────────────────────           ─────────────────────────────────────
 *   update_deadline()       ──call──► infinity_slice()        — fair-share slice
 *   update_curr()           ──call──► infinity_consume()      — accelerating budget consumption
 *   enqueue_task_fair()     ──call──► (resets runtime_debt)   — wakeup reset
 *   dequeue_task_fair()     ──call──► (records last_sleep_ns) — sleep tracking
 *   task_fork_fair()        ──call──► infinity_fork_init()    — fork init
 *   init/init_task.c        ──init──► infinity.{}             — static init
 *
 * Tunables (sysctl kernel.infinity.*):
 *   carriage_ns   — base fair-share window (default 2ms)
 *   debt_cap      — runtime debt cap multiplier (default 256x)
 *   refill_div    — budget refill divisor (default 100)
 *   self_stabilize — automatic tuning (default 1)
 *   running       — read-only flag, 1 if active
 *
 * Self-stabilize mode: when enabled, a feedback loop monitors per-CPU
 * scheduling metrics and adjusts tunables within safe clamped ranges.
 * The mathematical bounds guarantee the scheduler can never enter an
 * unstable state — any value within the clamp range produces valid
 * behavior, just with different throughput/latency trade-offs.
 */

#ifndef __INFINITY_SCHED_H
#define __INFINITY_SCHED_H

#include <linux/sched.h>

/* ------------------------------------------------------------------ */
/* Tunable defaults                                                    */
/* ------------------------------------------------------------------ */

/** Base fair-share window (2ms). */
#define INFINITY_CARRIAGE_NS_DEFAULT	2000000ULL

/** Runtime debt cap multiplier (256x). */
#define INFINITY_DEBT_CAP_DEFAULT	256

/** Budget refill divisor (100). */
#define INFINITY_REFILL_DIV_DEFAULT	100

/* ------------------------------------------------------------------ */
/* Clamp bounds                                                        */
/* ------------------------------------------------------------------ */

#define INFINITY_CARRIAGE_NS_MIN	1000ULL
#define INFINITY_CARRIAGE_NS_MAX	100000000ULL	/* 100ms */

#define INFINITY_DEBT_CAP_MIN		1
#define INFINITY_DEBT_CAP_MAX		65536

#define INFINITY_REFILL_DIV_MIN		1
#define INFINITY_REFILL_DIV_MAX		65536

/* ------------------------------------------------------------------ */
/* Hard constants (not user-tunable)                                   */
/* ------------------------------------------------------------------ */

/** Minimum slice floor (500us). */
#define INFINITY_SLICE_MIN_NS		500000ULL

/** Budget clamp bounds. */
#define INFINITY_BUDGET_MAX_NS		2000000ULL
#define INFINITY_BUDGET_MIN_NS		500000ULL

/** Sleep threshold for interactive floor (750us). */
#define INFINITY_INTERACTIVE_SLEEP_MIN_NS	750000ULL

/** Minimum budget refill for interactive tasks (100us). */
#define INFINITY_INTERACTIVE_FLOOR_NS	100000ULL

/** Maximum sleep time accounted for refill calculation (250ms). */
#define INFINITY_SLEEP_MAX_NS		250000000ULL

/** Initial budget for newly forked tasks (one minimum slice). */
#define INFINITY_INIT_BUDGET_NS		INFINITY_SLICE_MIN_NS

/* ------------------------------------------------------------------ */
/* Per-task context (embedded in struct task_struct as .infinity)     */
/* (struct infinity_ctx is defined in include/linux/sched.h)         */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* External sysctl tunables                                            */
/* ------------------------------------------------------------------ */

extern unsigned long infinity_tune_carriage_ns;
extern unsigned long infinity_tune_debt_cap;
extern unsigned long infinity_tune_refill_div;
extern bool infinity_tune_self_stabilize;

/* ------------------------------------------------------------------ */
/* API — called from fair.c                                           */
/* ------------------------------------------------------------------ */

u64 infinity_slice(unsigned long nr_runnable, bool on_smt_secondary);
void infinity_consume(struct infinity_ctx *ctx, u64 delta_ns);
void infinity_refill_budget(struct infinity_ctx *ctx, u64 sleep_ns);
void infinity_fork_init(struct infinity_ctx *ctx, u64 now);

/**
 * infinity_try_stabilize — update budget exhaustion stat (called from fair.c).
 * Called each time a task exhausts its budget in update_curr.
 */
void infinity_try_stabilize(void);

static inline s64 infinity_clamp_budget(s64 b)
{
	if (b > (s64)INFINITY_BUDGET_MAX_NS)
		return INFINITY_BUDGET_MAX_NS;
	if (b < -(s64)INFINITY_BUDGET_MIN_NS)
		return -(s64)INFINITY_BUDGET_MIN_NS;
	return b;
}

#endif /* __INFINITY_SCHED_H */
