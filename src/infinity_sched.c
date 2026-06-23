/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2026 Galih Tama <galpt@v.recipes>
 *
 * infinity_sched.c — Infinity scheduler algorithm.
 *
 * The Limitless concept: "No matter how many times someone divides a
 * number it will never be reduced to zero."
 *
 * This is implemented as accelerating budget consumption:
 *
 *   runtime_debt += delta_exec
 *   rate = 1 + min(runtime_debt, DEBT_CAP) / CARRIAGE_NS
 *   budget -= delta_exec * rate
 *
 * A freshly woken task has runtime_debt = 0, so rate = 1 (linear).
 * As the task runs without sleeping, runtime_debt grows, rate increases,
 * and budget depletes faster and faster — approaching zero asymptotically
 * but never reaching it.
 *
 * When the task sleeps and wakes up, runtime_debt resets to 0.
 * Interactive tasks that sleep frequently maintain low runtime_debt and
 * keep their budget.  CPU-bound tasks see their budget vanish rapidly
 * and become unable to preempt.
 */

#include <linux/types.h>
#include <linux/sched.h>
#include <linux/math64.h>
#include "infinity_sched.h"

/* ------------------------------------------------------------------ */
/* infinity_slice                                                     */
/* ------------------------------------------------------------------ */

u64 infinity_slice(unsigned long nr_runnable, bool on_smt_secondary)
{
	u64 slice;

	if (nr_runnable == 0)
		nr_runnable = 1;

	slice = INFINITY_CARRIAGE_NS / nr_runnable;

	if (slice < INFINITY_SLICE_MIN_NS)
		slice = INFINITY_SLICE_MIN_NS;
	if (slice > INFINITY_BUDGET_MAX_NS)
		slice = INFINITY_BUDGET_MAX_NS;

	if (on_smt_secondary)
		slice >>= 1;

	return slice;
}

/* ------------------------------------------------------------------ */
/* infinity_consume — the Limitless budget consumption                */
/* ------------------------------------------------------------------ */

void infinity_consume(struct infinity_ctx *ctx, u64 delta_ns)
{
	u64 debt, rate, consumption;

	/*
	 * Accelerating consumption: the longer a task runs without sleeping,
	 * the faster its remaining budget is consumed.
	 *
	 *   rate = 1 + min(runtime_debt, DEBT_CAP) / CARRIAGE_NS
	 *
	 * With the debt cap at 256 * CARRIAGE_NS, the maximum multiplier
	 * is 257.  delta_ns * 257 ≤ 4ms * 257 ≈ 1e9 ns, safe for u64.
	 */
	debt = min(ctx->runtime_debt + delta_ns, INFINITY_DEBT_CAP);
	rate = 1 + div64_u64(debt, INFINITY_CARRIAGE_NS);
	consumption = delta_ns * rate;
	ctx->budget_ns = infinity_clamp_budget(ctx->budget_ns - (s64)consumption);
	ctx->runtime_debt = debt;
}

/* ------------------------------------------------------------------ */
/* infinity_refill_budget                                             */
/* ------------------------------------------------------------------ */

void infinity_refill_budget(struct infinity_ctx *ctx, u64 sleep_ns)
{
	s64 refill_ns;

	if (sleep_ns > INFINITY_SLEEP_MAX_NS)
		sleep_ns = INFINITY_SLEEP_MAX_NS;

	refill_ns = (s64)(sleep_ns / INFINITY_BUDGET_REFILL_DIV);

	/* Interactive floor for short-sleeping tasks. */
	if (sleep_ns >= INFINITY_INTERACTIVE_SLEEP_MIN_NS &&
	    refill_ns < (s64)INFINITY_INTERACTIVE_FLOOR_NS)
		refill_ns = (s64)INFINITY_INTERACTIVE_FLOOR_NS;

	ctx->budget_ns = infinity_clamp_budget(ctx->budget_ns + refill_ns);
}

/* ------------------------------------------------------------------ */
/* infinity_fork_init                                                 */
/* ------------------------------------------------------------------ */

void infinity_fork_init(struct infinity_ctx *ctx, u64 now)
{
	ctx->budget_ns = INFINITY_INIT_BUDGET_NS;
	ctx->runtime_debt = 0;
	ctx->last_sleep_ns = now;
	ctx->fork_time_ns = now;
}
