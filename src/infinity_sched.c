/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2026 Galih Tama <galpt@v.recipes>
 *
 * infinity_sched.c — Infinity scheduler algorithm (v2 EMA).
 *
 * Exponential Moving Average budget tracking:
 *
 *   While running:  ema = ema + (BUDGET_MAX - ema) * α / 256
 *   While sleeping: ema = ema - ema * α / 256  (via catch-up on wakeup)
 *   budget = BUDGET_MAX - ema   (naturally in [0, BUDGET_MAX] — no clamps)
 *   rate_fp = 256 + ema * DEBT_CAP / BUDGET_MAX  (fixed-point 8-bit)
 *   consumption = delta_exec * rate_fp / 256
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
unsigned long infinity_tune_debt_cap   = INFINITY_DEBT_CAP_DEFAULT;
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

static int clamp_debt_cap(const struct ctl_table *table, int write,
			  void *buf, size_t *lenp, loff_t *ppos)
{
	int ret;
	unsigned long old, val;
	struct ctl_table tmp = *table;

	old = READ_ONCE(infinity_tune_debt_cap);
	val = old;
	tmp.data = &val;
	ret = proc_doulongvec_minmax(&tmp, write, buf, lenp, ppos);
	if (write && ret == 0) {
		val = clamp(val, INFINITY_DEBT_CAP_MIN, INFINITY_DEBT_CAP_MAX);
		if (val != old)
			pr_info("Infinity: debt_cap %lu -> %lu\n", old, val);
		WRITE_ONCE(infinity_tune_debt_cap, val);
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
		.procname	= "infinity_debt_cap",
		.data		= &infinity_tune_debt_cap,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= clamp_debt_cap,
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
		WRITE_ONCE(infinity_tune_debt_cap, INFINITY_DEBT_CAP_DEFAULT);
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

	pr_info("Infinity scheduler active: carriage=%lu ns, debt_cap=%lu, smt_divisor=%lu\n",
		infinity_tune_carriage_ns, infinity_tune_debt_cap,
		infinity_tune_smt_divisor);

	return 0;
}

late_initcall(infinity_sched_init);

/* ------------------------------------------------------------------ */
/* infinity_slice                                                      */
/* ------------------------------------------------------------------ */

u64 infinity_slice(unsigned long nr_runnable, bool on_smt_secondary)
{
	u64 slice;

	if (nr_runnable == 0)
		nr_runnable = 1;

	slice = READ_ONCE(infinity_tune_carriage_ns) / nr_runnable;

	if (slice < INFINITY_SLICE_MIN_NS)
		slice = INFINITY_SLICE_MIN_NS;
	if (slice > INFINITY_BUDGET_MAX_NS)
		slice = INFINITY_BUDGET_MAX_NS;

	if (on_smt_secondary) {
		unsigned long div = READ_ONCE(infinity_tune_smt_divisor);
		if (div > 1)
			slice = div64_u64(slice, div);
	}

	return slice;
}

/* ------------------------------------------------------------------ */
/* infinity_consume — EMA budget consumption                           */
/* ------------------------------------------------------------------ */

void infinity_consume(struct infinity_ctx *ctx, u64 delta_ns)
{
	u64 cap, rate_fp, consumption;

	cap = READ_ONCE(infinity_tune_debt_cap);

	/* EMA step: converge toward BUDGET_MAX while running */
	ctx->ema = ctx->ema + (INFINITY_BUDGET_MAX_NS - ctx->ema) *
		   INFINITY_EMA_ALPHA / INFINITY_FP_ONE;

	/*
	 * Fixed-point rate (INFINITY_FP_SHIFT = 8 bits fractional):
	 *   rate_fp = 256 + ema * DEBT_CAP * 256 / BUDGET_MAX
	 */
	rate_fp = div64_u64(ctx->ema * cap * INFINITY_FP_ONE,
			    INFINITY_BUDGET_MAX_NS);
	rate_fp += INFINITY_FP_ONE;

	consumption = delta_ns * rate_fp / INFINITY_FP_ONE;

	/* Budget = BUDGET_MAX - ema (naturally bounded, no clamp) */
	if (ctx->ema >= INFINITY_BUDGET_MAX_NS)
		ctx->budget_ns = 0;
	else
		ctx->budget_ns = INFINITY_BUDGET_MAX_NS - ctx->ema;
}

/* ------------------------------------------------------------------ */
/* infinity_wakeup — EMA decay catch-up on wakeup                      */
/* ------------------------------------------------------------------ */

void infinity_wakeup(struct infinity_ctx *ctx, u64 sleep_ns)
{
	u64 ticks = sleep_ns / (INFINITY_FP_ONE * 1000);
	int i;

	if (ticks == 0)
		return;

	/* Cap iterations to prevent unbounded loops */
	if (ticks > 256)
		ticks = 256;

	/* Decay EMA toward 0 for each tick slept */
	for (i = 0; i < ticks; i++)
		ctx->ema = ctx->ema - (ctx->ema * INFINITY_EMA_ALPHA /
					INFINITY_FP_ONE);
}

/* ------------------------------------------------------------------ */
/* infinity_fork_init                                                 */
/* ------------------------------------------------------------------ */

void infinity_fork_init(struct infinity_ctx *ctx, u64 now)
{
	ctx->budget_ns = INFINITY_INIT_BUDGET_NS;
	ctx->ema = 0;
	ctx->rt_ema = 0;
	ctx->last_sleep_ns = now;
	ctx->fork_time_ns = now;
}

/* ------------------------------------------------------------------ */
/* infinity_rt_consume — EMA climb on RT wakeup                        */
/* ------------------------------------------------------------------ */

void infinity_rt_consume(struct infinity_ctx *ctx)
{
	/* Asymptotic climb toward RT_BUDGET — no clamp, true Limitless */
	u64 step = (u64)((s64)(INFINITY_RT_BUDGET_NS - ctx->rt_ema)) *
		   INFINITY_RT_ALPHA / INFINITY_FP_ONE;
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
	s16 adj = (s16)base_prio - (s16)decay;
	return (u8)max((int)adj, INFINITY_RT_PRIO_FLOOR);
}
