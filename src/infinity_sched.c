/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2026 Galih Tama <galpt@v.recipes>
 *
 * infinity_sched.c — Infinity scheduler algorithm (v3).
 *
 * Exponential Moving Average based fair scheduling:
 *
 *   While running:  ema = ema + (BUDGET_MAX - ema) * α / 256
 *   While sleeping: ema = ema - ema * α / 256  (via catch-up on wakeup)
 *   slice = fair_share * (100 - ema_pct * 3/4) / 100  (active throttle)
 *
 * v3 adds EMA-modulated wakeup vslice for shorter interactive-task
 * deadlines, EMA-modulated RT queue placement via
 * infinity_rt_effective_prio(), a futex-waiting bypass in pick_eevdf(),
 * continuous EMA decay for micro-sleeps, and a 400us SLICE_MIN.
 *
 * The EMA converges asymptotically toward BUDGET_MAX when running and
 * toward 0 when sleeping — the true Limitless.  No clamps, no external
 * feedback loop.  Self-stabilizing by construction.
 */

#include <linux/types.h>
#include <linux/sched.h>
#include <linux/math64.h>
#include <linux/sysctl.h>
#include "infinity_sched.h"

/* ------------------------------------------------------------------ */
/* Sysctl tunables with safe clamps                                    */
/* ------------------------------------------------------------------ */

unsigned long infinity_tune_carriage_ns = INFINITY_CARRIAGE_NS_DEFAULT;
unsigned long infinity_tune_smt_divisor = INFINITY_SMT_DIVISOR_DEFAULT;
static int infinity_running_flag = 1;

/*
 * Write-time clamping handlers.
 * Every tunable write is intercepted, clamped to [MIN, MAX], and stored.
 * No value outside the safe range can be set — the scheduler can never
 * enter an invalid state regardless of user input.
 */
static int clamp_carriage_ns(const struct ctl_table *table, int write,
			     void *buf, size_t *lenp, loff_t *ppos)
{
	int ret;
	unsigned long old, val;
	struct ctl_table tmp = *table;

	old = READ_ONCE(infinity_tune_carriage_ns);
	val = old;
	tmp.data = &val;
	ret = proc_doulongvec_minmax(&tmp, write, buf, lenp, ppos);
	if (write && ret == 0) {
		val = clamp(val, INFINITY_CARRIAGE_NS_MIN, INFINITY_CARRIAGE_NS_MAX);
		if (val != old)
			pr_info("Infinity: carriage_ns %lu -> %lu\n", old, val);
		WRITE_ONCE(infinity_tune_carriage_ns, val);
	}
	return ret;
}

static int clamp_smt_divisor(const struct ctl_table *table, int write,
			     void *buf, size_t *lenp, loff_t *ppos)
{
	int ret;
	unsigned long old, val;
	struct ctl_table tmp = *table;

	old = READ_ONCE(infinity_tune_smt_divisor);
	val = old;
	tmp.data = &val;
	ret = proc_doulongvec_minmax(&tmp, write, buf, lenp, ppos);
	if (write && ret == 0) {
		val = clamp(val, INFINITY_SMT_DIVISOR_MIN, INFINITY_SMT_DIVISOR_MAX);
		if (val != old)
			pr_info("Infinity: smt_divisor %lu -> %lu\n", old, val);
		WRITE_ONCE(infinity_tune_smt_divisor, val);
	}
	return ret;
}

static struct ctl_table infinity_sysctl_table[] = {
	{
		.procname	= "infinity_carriage_ns",
		.data		= &infinity_tune_carriage_ns,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= clamp_carriage_ns,
	},
	{
		.procname	= "infinity_smt_divisor",
		.data		= &infinity_tune_smt_divisor,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= clamp_smt_divisor,
	},
	{
		.procname	= "infinity_running",
		.data		= &infinity_running_flag,
		.maxlen		= sizeof(int),
		.mode		= 0444,
		.proc_handler	= proc_dointvec,
	},
	{}
};

static unsigned long infinity_reset_val;

static int reset_handler(const struct ctl_table *table, int write,
			 void *buf, size_t *lenp, loff_t *ppos)
{
	int ret;
	struct ctl_table tmp = *table;

	infinity_reset_val = 0;
	tmp.data = &infinity_reset_val;
	ret = proc_doulongvec_minmax(&tmp, write, buf, lenp, ppos);
	if (write && ret == 0 && infinity_reset_val == 1) {
		WRITE_ONCE(infinity_tune_carriage_ns, INFINITY_CARRIAGE_NS_DEFAULT);
		WRITE_ONCE(infinity_tune_smt_divisor, INFINITY_SMT_DIVISOR_DEFAULT);
		pr_info("Infinity: reset to default values\n");
	}
	return ret;
}

static struct ctl_table infinity_reset_table[] = {
	{
		.procname	= "infinity_reset",
		.data		= &infinity_reset_val,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0200,
		.proc_handler	= reset_handler,
	},
	{}
};

/* ------------------------------------------------------------------ */
/* Initialization                                                      */
/* ------------------------------------------------------------------ */

static int __init infinity_sched_init(void)
{
	__register_sysctl_init("kernel", infinity_sysctl_table,
			      "infinity_sysctl_table",
			      ARRAY_SIZE(infinity_sysctl_table) - 1);
	__register_sysctl_init("kernel", infinity_reset_table,
			      "infinity_reset_table",
			      ARRAY_SIZE(infinity_reset_table) - 1);

	pr_info("Infinity scheduler active: carriage=%lu ns, smt_divisor=%lu\n",
		infinity_tune_carriage_ns,
		infinity_tune_smt_divisor);

	return 0;
}

late_initcall(infinity_sched_init);

/* ------------------------------------------------------------------ */
/* infinity_slice — fair-share slice with EMA modulation               */
/* ------------------------------------------------------------------ */

u64 infinity_slice(unsigned long nr_runnable, bool on_smt_secondary, u64 ema)
{
	u64 slice;

	if (nr_runnable == 0)
		nr_runnable = 1;

	slice = READ_ONCE(infinity_tune_carriage_ns) / nr_runnable;

	/* EMA modulation: higher EMA → shorter slice (active throttle) */
	if (ema > 0) {
		u64 pct = (ema * 100ULL) / INFINITY_BUDGET_MAX_NS;
		/* Reduce slice by up to 75% at ema=100% */
		slice = slice * (100ULL - pct * 3ULL / 4ULL) / 100ULL;
	}

	/* Apply SMT scaling before the safety floor, otherwise a 400us
	 * minimum slice would be divided to 25us on an SMT-16 core. */
	if (on_smt_secondary) {
		unsigned long div = READ_ONCE(infinity_tune_smt_divisor);
		if (div > 1)
			slice = div64_u64(slice, div);
	}

	/* Safety bounds — always last.  Note that the carriage ceiling
	 * (INFINITY_CARRIAGE_NS_MAX = 100ms) is intentionally wider than
	 * the slice cap (2ms) so that multi-task fairness scales correctly;
	 * carriage_ns *only* affects the per-task share divided by nr_runnable,
	 * and the 2ms budget cap prevents any single task from exceeding
	 * the design target regardless of the sysctl setting. */
	if (slice < INFINITY_SLICE_MIN_NS)
		slice = INFINITY_SLICE_MIN_NS;
	if (slice > INFINITY_BUDGET_MAX_NS)
		slice = INFINITY_BUDGET_MAX_NS;

	return slice;
}

/* ------------------------------------------------------------------ */
/* infinity_consume — EMA budget consumption                           */
/* ------------------------------------------------------------------ */

void infinity_consume(struct infinity_ctx *ctx, u64 delta_ns)
{
	/*
	 * Scale the EMA step proportionally to actual runtime.
	 * A task that runs for a full tick advances by α/256 of the
	 * remaining distance to BUDGET_MAX; a partial tick advances
	 * proportionally less.  This prevents context-switch frequency
	 * from biasing the EMA.
	 */
	u64 step = div64_u64((INFINITY_BUDGET_MAX_NS - ctx->ema) * delta_ns *
			   INFINITY_EMA_ALPHA,
			   INFINITY_BUDGET_MAX_NS * INFINITY_FP_ONE);
	ctx->ema += step;
}

/* ------------------------------------------------------------------ */
/* infinity_wakeup — EMA decay catch-up on wakeup                      */
/* ------------------------------------------------------------------ */

void infinity_wakeup(struct infinity_ctx *ctx, u64 sleep_ns)
{
	/*
	 * Continuous EMA decay proportional to sleep duration.
	 * Uses fine-grained scaling instead of per-tick loop so that
	 * sub-256us micro-sleeps (common in barrier-synchronized
	 * workloads like y-cruncher) register proportional decay.
	 */
	u64 dec;

	if (sleep_ns == 0)
		return;

	/* Fixed-point division against a 1ms time constant */
	dec = div64_u64(sleep_ns * INFINITY_EMA_ALPHA, 1000000ULL);
	if (dec > INFINITY_FP_ONE)
		dec = INFINITY_FP_ONE;

	ctx->ema = ctx->ema - (ctx->ema * dec / INFINITY_FP_ONE);
}

/* ------------------------------------------------------------------ */
/* infinity_fork_init                                                 */
/* ------------------------------------------------------------------ */

void infinity_fork_init(struct infinity_ctx *ctx, u64 now)
{
	ctx->ema = 0;
	ctx->rt_ema = 0;
	ctx->last_sleep_ns = now;
}

/* ------------------------------------------------------------------ */
/* infinity_wakeup_scale — EMA-modulated vslice for wakeups            */
/* ------------------------------------------------------------------ */

u64 infinity_wakeup_scale(u64 vslice, struct infinity_ctx *ctx)
{
	u64 ema_pct, boost;

	/*
	 * Scale the vslice down for waking tasks whose EMA is low
	 * (i.e. they slept long enough for the EMA to decay).  This
	 * moves their deadline earlier in the EEVDF tree so they are
	 * picked sooner after wakeup.  Tasks that only slept briefly
	 * retain their full vslice.
	 *
	 * ema_pct = 0   (slept long, interactive):  boost = 50%
	 * ema_pct = 100 (brief sleep, CPU-bound):    boost = 0%
	 */
	ema_pct = ctx->ema * 100ULL / INFINITY_BUDGET_MAX_NS;
	boost = (100ULL - ema_pct) * 50ULL / 100ULL;

	return vslice * (100ULL - boost) / 100ULL;
}

/* ------------------------------------------------------------------ */
/* infinity_rt_consume — EMA climb on RT wakeup                        */
/* ------------------------------------------------------------------ */

void infinity_rt_consume(struct infinity_ctx *ctx, u64 delta_ns)
{
	/* Scale the RT EMA step proportionally to actual runtime */
	u64 step = div64_u64((INFINITY_RT_BUDGET_NS - ctx->rt_ema) * delta_ns *
			   INFINITY_RT_ALPHA,
			   INFINITY_RT_BUDGET_NS * INFINITY_FP_ONE);
	ctx->rt_ema += step;
}

/* ------------------------------------------------------------------ */
/* infinity_rt_wakeup — EMA decay on RT block/sleep                    */
/* ------------------------------------------------------------------ */

void infinity_rt_wakeup(struct infinity_ctx *ctx)
{
	/* Fast decay toward 0 — well-behaved RT tasks recover quickly */
	ctx->rt_ema = ctx->rt_ema - (ctx->rt_ema * INFINITY_RT_BETA /
				      INFINITY_FP_ONE);
}

/* ------------------------------------------------------------------ */
/* infinity_rt_effective_prio — priority after EMA modulation          */
/* ------------------------------------------------------------------ */

u8 infinity_rt_effective_prio(u8 base_prio, struct infinity_ctx *ctx)
{
	u64 decay = div64_u64(ctx->rt_ema * INFINITY_RT_PRIO_RANGE,
			      INFINITY_RT_BUDGET_NS);
	s16 adj = (s16)base_prio + (s16)decay;
	return (u8)min((int)adj, INFINITY_RT_PRIO_FLOOR);
}
