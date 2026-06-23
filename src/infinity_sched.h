/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2026 Galih Tama <galpt@v.recipes>
 *
 * infinity_sched.h — Infinity scheduler API.
 *
 * Inspired by the Limitless technique: "No matter how many times someone
 * divides a number it will never be reduced to zero."
 *
 * In scheduling terms: a running task's remaining budget is multiplied by an
 * ever-increasing factor the longer it runs, causing its effective budget to
 * approach zero asymptotically.  Interactive tasks that sleep frequently
 * reset this factor and naturally preempt CPU-bound tasks.
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
 * No explicit preemption is needed.  The accelerating consumption rate
 * naturally prevents CPU-bound tasks from maintaining a positive budget,
 * while interactive tasks reset their debt on wakeup and retain budget.
 */

#ifndef __INFINITY_SCHED_H
#define __INFINITY_SCHED_H

#include <linux/sched.h>

/* ------------------------------------------------------------------ */
/* Constants                                                          */
/* ------------------------------------------------------------------ */

/** Base fair-share window (2ms). */
#define INFINITY_CARRIAGE_NS		2000000ULL

/** Minimum slice floor (500us). */
#define INFINITY_SLICE_MIN_NS		500000ULL

/** Budget clamp bounds. */
#define INFINITY_BUDGET_MAX_NS		2000000ULL
#define INFINITY_BUDGET_MIN_NS		500000ULL

/** Minimum runtime before preemption guard (500us). */
#define INFINITY_PREEMPT_GUARD_NS	500000ULL

/** Budget refill divisor: refill = sleep_ns / INFINITY_BUDGET_REFILL_DIV. */
#define INFINITY_BUDGET_REFILL_DIV	100

/** Sleep threshold for interactive floor (750us). */
#define INFINITY_INTERACTIVE_SLEEP_MIN_NS	750000ULL

/** Minimum budget refill for interactive tasks (100us). */
#define INFINITY_INTERACTIVE_FLOOR_NS	100000ULL

/** Maximum sleep time accounted for refill calculation (250ms). */
#define INFINITY_SLEEP_MAX_NS		250000000ULL

/** Initial budget for newly forked tasks (one minimum slice). */
#define INFINITY_INIT_BUDGET_NS		INFINITY_SLICE_MIN_NS

/**
 * Runtime debt cap — prevents unbounded growth of the acceleration factor.
 * At CARRIAGE_NS = 2ms and cap = 256, the maximum multiplier is 257x.
 * delta_exec * rate ≤ 4ms * 257 ≈ 1e9, well within u64 range.
 */
#define INFINITY_DEBT_CAP		(INFINITY_CARRIAGE_NS * 256)

/* ------------------------------------------------------------------ */
/* Per-task context (embedded in struct task_struct as .infinity)     */
/* (struct infinity_ctx is defined in include/linux/sched.h)         */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* API — called from fair.c                                           */
/* ------------------------------------------------------------------ */

u64 infinity_slice(unsigned long nr_runnable, bool on_smt_secondary);
void infinity_consume(struct infinity_ctx *ctx, u64 delta_ns);
void infinity_refill_budget(struct infinity_ctx *ctx, u64 sleep_ns);
void infinity_fork_init(struct infinity_ctx *ctx, u64 now);

static inline s64 infinity_clamp_budget(s64 b)
{
	if (b > (s64)INFINITY_BUDGET_MAX_NS)
		return INFINITY_BUDGET_MAX_NS;
	if (b < -(s64)INFINITY_BUDGET_MIN_NS)
		return -(s64)INFINITY_BUDGET_MIN_NS;
	return b;
}

#endif /* __INFINITY_SCHED_H */
