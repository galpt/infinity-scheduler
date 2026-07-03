/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2026 Galih Tama <galpt@v.recipes>
 *
 * infinity_sched.c — Infinity scheduler algorithm (dev).
 *
 * Fully continuous limit-based scheduling:
 *
 *   While running:  ema += (BUDGET_MAX - ema) × δ × α / (BUDGET_MAX × FP_ONE)
 *   While sleeping:  ema >>= min(sleep_ns / 24000000, 63)
 *                      Sub-period residual via 2nd-order Taylor expansion
 *                      (e^-x ≈ 1 - x + x²/2) for continuous decay.
 *   Weight:          effective = base × (100 - ema_pct × 8/10) / 100
 *                      with floor at base/10.
 *
 * A two-pole correction (effective EMA = EMA - dEMA/2) distinguishes
 * oscillating workloads (interactive: compute-sleep-compute) from
 * sustained CPU-bound tasks, giving interactivity a systematic boost.
 *
 * All task classification data is observed within the scheduler
 * (uclamp declarations, EMA tracking).  Driver hooks are not used.
 * Carriage_ns auto-scales from CPU count.
 */

#include <linux/types.h>
#include <linux/sched.h>
#include <linux/sched/clock.h>
#include <linux/math64.h>
#include <linux/sysctl.h>
#include "infinity_sched.h"

/* ------------------------------------------------------------------ */
/* Sysctl tunables                                                     */
/* ------------------------------------------------------------------ */

unsigned long infinity_tune_smt_divisor = INFINITY_SMT_DIVISOR_DEFAULT;
static int infinity_running_flag = 1;

/* Carriage (base fair-share window), auto-scaled at init. */
unsigned long infinity_carriage_ns = INFINITY_BASE_CARRIAGE_NS;

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

/* ------------------------------------------------------------------ */
/* Auto-carriage scaling                                               */
/* ------------------------------------------------------------------ */

static void __init auto_carriage_ns(void)
{
	unsigned int cpus = min_t(unsigned int, num_online_cpus(), 8);
	unsigned int factor = 1 + ilog2(cpus);

	infinity_carriage_ns = INFINITY_BASE_CARRIAGE_NS * factor;
	pr_info("Infinity: auto-scaled carriage=%lu ns (%u cpus, factor=%u)\n",
		infinity_carriage_ns, cpus, factor);
}

/* ------------------------------------------------------------------ */
/* Initialization                                                      */
/* ------------------------------------------------------------------ */

static int __init infinity_sched_init(void)
{
	__register_sysctl_init("kernel", infinity_sysctl_table,
			      "infinity_sysctl_table",
			      ARRAY_SIZE(infinity_sysctl_table) - 1);

	auto_carriage_ns();

	pr_info("Infinity scheduler active: smt_divisor=%lu\n",
		infinity_tune_smt_divisor);

	return 0;
}

late_initcall(infinity_sched_init);

/* ------------------------------------------------------------------ */
/* infinity_consume — EMA budget consumption                           */
/* ------------------------------------------------------------------ */

void infinity_consume(struct infinity_ctx *ctx, u64 delta_ns)
{
	u64 step;

	/* Safety clamp: prevent underflow if ema drifts past BUDGET_MAX */
	if (ctx->ema >= INFINITY_BUDGET_MAX_NS) {
		ctx->prev_ema = ctx->ema;
		return;
	}

	ctx->prev_ema = ctx->ema;

	step = div64_u64((INFINITY_BUDGET_MAX_NS - ctx->ema) * delta_ns *
			 INFINITY_EMA_ALPHA,
			 INFINITY_BUDGET_MAX_NS * INFINITY_FP_ONE);
	ctx->ema += step;
}

/* ------------------------------------------------------------------ */
/* infinity_wakeup — EMA decay on wakeup                               */
/* ------------------------------------------------------------------ */

void infinity_wakeup(struct infinity_ctx *ctx, u64 sleep_ns)
{
	if (sleep_ns == 0)
		return;

	/*
	 * Exponential shift decay with 32ms effective half-life
	 * (τ_cimb / DIV = 128ms / 4), using a 2nd-order Taylor
	 * expansion for the sub-period residual to maintain a
	 * continuous decay curve across the half-life boundary.
	 *
	 *   whole periods (≥ 24ms):  ema >>= periods  (exact exponential)
	 *   sub-period (< 24ms):     e^-x ≈ 1 - x + x²/2  (Taylor)
	 *
	 * At x = 1.0 (residual = 24ms) the Taylor formula gives
	 * 1 - 1 + 1/2 = 0.5, matching ema >>= 1 — no discontinuity.
	 */
	{
		u64 periods, residual;
		periods = div64_u64_rem(sleep_ns, 24000000ULL, &residual);

		if (periods > 63) {
			ctx->ema = 0;
		} else {
			/* Whole half-life shift cycles */
			ctx->ema >>= periods;

			/* Sub-period residual via Taylor e^-x ≈ 1 - x + x²/2 */
			if (residual && ctx->ema) {
				u64 fraction = div64_u64(residual *
					INFINITY_FP_ONE, 24000000ULL);
				u64 linear = (ctx->ema * fraction) >>
					INFINITY_FP_SHIFT;
				u64 quad = ((linear * fraction) >>
					INFINITY_FP_SHIFT) >> 1;

				if (linear > quad)
					ctx->ema -= min(ctx->ema,
							linear - quad);
			}
		}
	}

	ctx->prev_ema = ctx->ema;
}

/* ------------------------------------------------------------------ */
/* infinity_fork_init                                                   */
/* ------------------------------------------------------------------ */

void infinity_fork_init(struct infinity_ctx *ctx, u64 now)
{
	ctx->ema = 0;
	ctx->prev_ema = 0;
	ctx->rt_ema = 0;
	ctx->last_sleep_ns = now;
	ctx->rt_last_sleep_ns = 0;
}

/* ------------------------------------------------------------------ */
/* infinity_wakeup_scale — no longer used (replaced by weight)         */
/* ------------------------------------------------------------------ */

/*
 * infinity_wakeup_scale was removed in favour of weight modulation.
 * The EMA-modulated weight produces the same effect: a low-EMA task
 * has a higher weight, gets an earlier deadline, and is picked sooner.
 */

/* ------------------------------------------------------------------ */
/* infinity_slice — no longer used (replaced by weight)                */
/* ------------------------------------------------------------------ */

/*
 * infinity_slice was removed in favour of weight modulation.  EEVDF
 * natively computes the slice from the task's weight via calc_delta_fair.
 * SMT halving is handled directly in update_deadline.
 */

/* ------------------------------------------------------------------ */
/* infinity_vruntime_scale — no longer used (replaced by weight)       */
/* ------------------------------------------------------------------ */

/*
 * infinity_vruntime_scale was removed in favour of weight modulation.
 * Vruntime advances naturally because the weight determines the slice
 * and deadline — no separate vruntime scaling is needed.
 */

/* ------------------------------------------------------------------ */
/* infinity_rt_consume — EMA climb on RT runtime                       */
/* ------------------------------------------------------------------ */

void infinity_rt_consume(struct infinity_ctx *ctx, u64 delta_ns)
{
	u64 step;

	if (unlikely(ctx->rt_ema >= INFINITY_RT_BUDGET_NS)) {
		ctx->rt_ema = INFINITY_RT_BUDGET_NS;
		return;
	}

	step = div64_u64((INFINITY_RT_BUDGET_NS - ctx->rt_ema) * delta_ns *
			   INFINITY_RT_ALPHA,
			   INFINITY_RT_BUDGET_NS * INFINITY_FP_ONE);
	ctx->rt_ema += step;
}

/* ------------------------------------------------------------------ */
/* infinity_rt_wakeup — time-proportional RT EMA decay on wakeup       */
/* ------------------------------------------------------------------ */

void infinity_rt_wakeup(struct infinity_ctx *ctx, u64 sleep_ns)
{
	u64 periods, residual;

	if (sleep_ns == 0)
		return;

	periods = div64_u64_rem(sleep_ns, 160000000ULL, &residual);

	if (periods > 63) {
		ctx->rt_ema = 0;
	} else {
		ctx->rt_ema >>= periods;
		if (residual && ctx->rt_ema) {
			u64 fraction = div64_u64(residual *
				INFINITY_FP_ONE, 160000000ULL);
			u64 linear = (ctx->rt_ema * fraction) >>
				INFINITY_FP_SHIFT;
			u64 quad = ((linear * fraction) >>
				INFINITY_FP_SHIFT) >> 1;
			if (linear > quad)
				ctx->rt_ema -= min(ctx->rt_ema,
						   linear - quad);
		}
	}
}

/* ------------------------------------------------------------------ */
/* infinity_rr_timeslice — adaptive SCHED_RR timeslice                */
/* ------------------------------------------------------------------ */

unsigned int infinity_rr_timeslice(struct task_struct *p,
				   unsigned int rr_default)
{
	u64 decay_pct;

	if (!p->infinity.rt_ema)
		return rr_default;

	decay_pct = div64_u64(p->infinity.rt_ema * 90ULL,
			      INFINITY_RT_BUDGET_NS);
	if (decay_pct > 90)
		decay_pct = 90;

	return max(1U, (unsigned int)(rr_default * (100ULL - decay_pct)
				      / 100ULL));
}
