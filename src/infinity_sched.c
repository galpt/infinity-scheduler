/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2026 Galih Tama <galpt@v.recipes>
 *
 * infinity_sched.c — Infinity scheduler algorithm (v4).
 *
 * Fully continuous limit-based fair and RT scheduling:
 *
 *   While running:  ema += (BUDGET_MAX - ema) * delta_ns * α / (BUDGET_MAX * FP_ONE)
 *   While sleeping:  ema -= ema * sleep_ns * α / (BUDGET_MAX * FP_ONE)
 *   slice = share * (100 - ema_pct * 3/4) / 100  (active throttle)
 *   vslice' = vslice * ema / BUDGET_MAX  (asymptotic, no cap)
 *
 * Both climb and decay use the same time constant τ = 32ms (BUDGET_MAX ×
 * FP_ONE / ALPHA), making the system a symmetric running average of CPU
 * utilization — the EMA naturally stabilises at the task's duty cycle.
 * There is no hard reset, no decay threshold: sub-62.5µs micro-sleeps and
 * multi-second sleeps both register proportional decay with the same
 * continuous formula.
 *
 * A two-pole correction (effective EMA = EMA - dEMA/2) distinguishes
 * oscillating workloads (interactive: compute-sleep-compute) from
 * sustained CPU-bound tasks, giving interactivity a systematic boost.
 *
 * v4 adds:
 *   - Symmetric τ for climb and decay (no hard reset)
 *   - Asymptotic vslice (approaches zero as EMA → 0)
 *   - Time-proportional RT decay
 *   - Two-pole EMA correction for oscillating vs sustained workloads
 *
 * The EMA converges asymptotically toward BUDGET_MAX when running and
 * toward 0 when sleeping — the true Limitless.  No clamps, no thresholds,
 * no external feedback loop.  Self-stabilising by construction.
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
	u64 step;

	ctx->prev_ema = ctx->ema;

	step = div64_u64((INFINITY_BUDGET_MAX_NS - ctx->ema) * delta_ns *
			 INFINITY_EMA_ALPHA,
			 INFINITY_BUDGET_MAX_NS * INFINITY_FP_ONE);
	ctx->ema += step;
}

/* ------------------------------------------------------------------ */
/* infinity_wakeup — EMA decay on wakeup (symmetric τ with climb)      */
/* ------------------------------------------------------------------ */

void infinity_wakeup(struct infinity_ctx *ctx, u64 sleep_ns)
{
	u64 step;

	if (sleep_ns == 0)
		return;

	/*
	 * Decay is symmetric with climb: both use τ = BUDGET_MAX × FP_ONE / ALPHA.
	 * step = ema × sleep_ns × α / (BUDGET_MAX × FP_ONE)
	 *
	 * The step is clamped to ema to prevent underflow — the EMA approaches
	 * 0 asymptotically but never reaches it in finite time.  Every sleep
	 * duration, from sub-microsecond to multi-second, registers proportional
	 * decay through the same continuous formula.
	 */
	ctx->prev_ema = ctx->ema;
	step = div64_u64(ctx->ema * sleep_ns * INFINITY_EMA_ALPHA,
			 INFINITY_BUDGET_MAX_NS * INFINITY_FP_ONE);
	if (step > ctx->ema)
		step = ctx->ema;
	ctx->ema -= step;
}

/* ------------------------------------------------------------------ */
/* infinity_fork_init                                                 */
/* ------------------------------------------------------------------ */

void infinity_fork_init(struct infinity_ctx *ctx, u64 now)
{
	ctx->ema = 0;
	ctx->prev_ema = 0;
	ctx->rt_ema = 0;
	ctx->last_sleep_ns = now;
}

/* ------------------------------------------------------------------ */
/* infinity_wakeup_scale — asymptotic vslice scaling on wakeup         */
/* ------------------------------------------------------------------ */

u64 infinity_wakeup_scale(u64 vslice, struct infinity_ctx *ctx)
{
	u64 effective;

	/*
	 * Scale the vslice as a continuous function of the effective EMA.
	 * The formula vslice' = vslice × ema / BUDGET_MAX is asymptotic:
	 * at EMA → 0, the vslice approaches 0 (instant scheduling on
	 * wakeup); at EMA → BUDGET_MAX, the vslice approaches the nominal
	 * value.  The +1 term guarantees a positive vslice for EEVDF tree
	 * placement at EMA = 0.
	 *
	 * This replaces the v3 fixed-50%-reduction cap with a fully
	 * continuous scaling — every EMA value produces a distinct
	 * vslice, no thresholds, no discontinuities.
	 */
	effective = infinity_effective_ema(ctx);
	if (effective >= INFINITY_BUDGET_MAX_NS)
		return vslice;
	return div64_u64(vslice * effective, INFINITY_BUDGET_MAX_NS) + 1;
}

/* ------------------------------------------------------------------ */
/* infinity_vruntime_scale — EMA vruntime advancement scaling          */
/* ------------------------------------------------------------------ */

u64 infinity_vruntime_scale(u64 vdelta, u64 ema)
{
	if (ema) {
		u64 pct = ema * 100ULL / INFINITY_BUDGET_MAX_NS;
		u64 denom = 100ULL - pct * 3ULL / 4ULL;

		if (denom < 100ULL)
			vdelta = div64_u64(vdelta * 100ULL, denom);
	}
	return vdelta;
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
/* infinity_rt_wakeup — time-proportional RT EMA decay on wakeup       */
/* ------------------------------------------------------------------ */

void infinity_rt_wakeup(struct infinity_ctx *ctx, u64 sleep_ns)
{
	u64 step;

	if (sleep_ns == 0)
		return;

	/*
	 * Time-proportional RT EMA decay, matching the fair path.
	 * step = rt_ema × sleep_ns × α / (RT_BUDGET × FP_ONE)
	 *
	 * The old event-rate-dependent decay (divide by 4 per sleep) was
	 * independent of actual sleep duration.  A time-proportional decay
	 * means well-behaved RT tasks (short compute, long blocks) recover
	 * faster, while RT tasks that briefly block and resume retain more
	 * of their rt_ema — accurate runtime tracking regardless of wakeup
	 * frequency.
	 */
	step = div64_u64(ctx->rt_ema * sleep_ns * INFINITY_RT_ALPHA,
			 INFINITY_RT_BUDGET_NS * INFINITY_FP_ONE);
	if (step > ctx->rt_ema)
		step = ctx->rt_ema;
	ctx->rt_ema -= step;
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
