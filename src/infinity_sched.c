/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2026 Galih Tama <galpt@v.recipes>
 *
 * infinity_sched.c — Infinity scheduler algorithm (dev).
 *
 * Fully continuous limit-based fair and RT scheduling:
 *
 *   While running:  ema += (BUDGET_MAX - ema) × δ × α / (BUDGET_MAX × FP_ONE)
 *   While sleeping:  ema >>= min(sleep_ns / 24000000, 63)
 *                      Sub-period residual via 2nd-order Taylor expansion
 *                      (e^-x ≈ 1 - x + x²/2) for continuous decay.
 *   slice = share × (100 - ema_pct × 8/10) / 100  (active throttle)
 *   vslice' = vslice × ema / BUDGET_MAX  (asymptotic, no cap)
 *
 * A two-pole correction (effective EMA = EMA - dEMA/2) distinguishes
 * oscillating workloads (interactive: compute-sleep-compute) from
 * sustained CPU-bound tasks, giving interactivity a systematic boost.
 *
 * All task classification data is observed within the scheduler
 * (uclamp declarations, wakeup source classification, EMA tracking).
 * Driver hooks are not used.  Carriage_ns auto-scales from CPU count.
 */

#include <linux/types.h>
#include <linux/sched.h>
#include <linux/sched/clock.h>
#include <linux/math64.h>
#include <linux/sysctl.h>
#include "infinity_sched.h"

/* ------------------------------------------------------------------ */
/* Tunables with safe clamps                                           */
/* ------------------------------------------------------------------ */

unsigned long infinity_tune_smt_divisor = INFINITY_SMT_DIVISOR_DEFAULT;
static int infinity_running_flag = 1;

/* Auto-scaled carriage — set at init, not user-tunable */
static unsigned long infinity_carriage_ns = INFINITY_BASE_CARRIAGE_NS;

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
/* infinity_slice — fair-share slice with EMA modulation               */
/* ------------------------------------------------------------------ */

u64 infinity_slice(unsigned long nr_runnable, bool on_smt_secondary, u64 ema)
{
	u64 slice, share;

	if (nr_runnable == 0)
		nr_runnable = 1;

	share = infinity_carriage_ns / nr_runnable;
	slice = share;

	/*
	 * EMA modulation: higher EMA → shorter slice.
	 * Uses an 8/10 slope so at EMA=100% the reduction is 80%,
	 * yielding a 5× vruntime scaling cap (100 / (100 - 80)).
	 */
	if (ema > 0) {
		u64 pct = (ema * 100ULL) / INFINITY_BUDGET_MAX_NS;
		slice = slice * (100ULL - pct * INFINITY_VRUNTIME_SLOPE_NUM /
				 INFINITY_VRUNTIME_SLOPE_DEN) / 100ULL;
	}

	/* SMT scaling */
	if (on_smt_secondary) {
		unsigned long div = READ_ONCE(infinity_tune_smt_divisor);
		if (div > 1)
			slice = div64_u64(slice, div);
	}

	/*
	 * Proportional minimum: 50% of fair share (not an absolute floor).
	 * The EMA modulation can never reduce the slice below half of the
	 * task's fair share, which guarantees that every task always makes
	 * measurable forward progress while preserving the ordering between
	 * interactive and CPU-bound tasks.
	 */
	{
		u64 min_slice = share >> 1;
		if (slice < min_slice)
			slice = min_slice;
	}

	/* Ceiling: single-task budget cap */
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
/* infinity_wakeup — EMA decay on wakeup (4× faster than climb)        */
/* ------------------------------------------------------------------ */

void infinity_wakeup(struct infinity_ctx *ctx, u64 sleep_ns)
{
	if (sleep_ns == 0)
		return;

	/*
	 * Hardware-wakeup classification: if the waker is a kernel thread
	 * at SCHED_FIFO priority, it is likely servicing a threaded IRQ
	 * handler.  Record the timestamp so infinity_vruntime_scale()
	 * can give the task a 50ms vruntime grace period.
	 */
	if (current->policy == SCHED_FIFO && (current->flags & PF_KTHREAD))
		ctx->last_hw_wakeup = sched_clock();

	/*
	 * Exponential shift decay with 24ms half-life, using a 2nd-order
	 * Taylor expansion for the sub-period residual to maintain a
	 * continuous decay curve across the half-life boundary.
	 *
	 *   whole periods (≥ 24ms):  ema >>= periods  (exact exponential)
	 *   sub-period (< 24ms):     e^-x ≈ 1 - x + x²/2  (Taylor)
	 *
	 * At x = 1.0 (residual = 24ms) the Taylor formula gives
	 * 1 - 1 + 1/2 = 0.5, matching ema >>= 1 — no discontinuity.
	 *
	 * This prevents the catastrophic linear collapse at x ≈ 1 that
	 * the old first-order formula produced (ema → 0 at 23.99ms vs
	 * ema/2 retained at 24.01ms).
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

	/*
	 * Set prev_ema after the decay so the two-pole correction
	 * (d = ema - prev_ema) evaluates to ~0 at wakeup time, preserving
	 * the full wakeup vslice reduction and interactive boost.
	 * During the subsequent compute burst infinity_consume() will
	 * overwrite prev_ema before climbing, re-enabling the correction.
	 */
	ctx->prev_ema = ctx->ema;
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
	ctx->rt_last_sleep_ns = 0;
	ctx->last_hw_wakeup = 0;

}

/* ------------------------------------------------------------------ */
/* infinity_wakeup_scale — asymptotic vslice scaling on wakeup         */
/* ------------------------------------------------------------------ */

u64 infinity_wakeup_scale(u64 vslice, struct infinity_ctx *ctx)
{
	u64 effective;

	/*
	 * Asymptotic vslice: vslice' = vslice × ema / BUDGET_MAX.
	 * At EMA → 0: vslice → 0 (instant scheduling on wakeup).
	 * At EMA → BUDGET_MAX: vslice approaches the nominal value.
	 * No cap, no threshold, fully continuous.
	 */
	effective = infinity_effective_ema(ctx);
	if (effective >= INFINITY_BUDGET_MAX_NS)
		return vslice;
	return div64_u64(vslice * effective, INFINITY_BUDGET_MAX_NS) + 1;
}

/* ------------------------------------------------------------------ */
/* infinity_vruntime_scale — EMA vruntime advancement scaling          */
/* ------------------------------------------------------------------ */

u64 infinity_vruntime_scale(u64 vdelta, struct task_struct *p)
{
	u64 ema;

	if (!p)
		return vdelta;

	/*
	 * Utilization clamping bypass: if the task has set
	 * sched_util_min > 0 via sched_setattr(), it has explicitly
	 * declared itself interactive.  Respect that declaration
	 * and bypass EMA scaling.
	 */
#ifdef CONFIG_UCLAMP_TASK
	if (p->uclamp_req[UCLAMP_MIN].value > 0)
		return vdelta;
#endif

	/*
	 * Hardware-wakeup bypass: if this task was recently woken by a
	 * threaded IRQ handler, the timestamp was set by infinity_wakeup()
	 * and recorded in last_hw_wakeup.  Run at nominal vruntime for
	 * 50ms.
	 */
	if (sched_clock() - p->infinity.last_hw_wakeup < 50000000ULL)
		return vdelta;

	ema = infinity_effective_ema(&p->infinity);

	/*
	 * Enforce BUDGET_MAX ceiling on effective EMA to guarantee that the
	 * denominator remains bounded (denom ≥ 20), ensuring stable vruntime
	 * advancement across continuous execution bursts.
	 */
	if (ema > INFINITY_BUDGET_MAX_NS)
		ema = INFINITY_BUDGET_MAX_NS;

	if (ema) {
		u64 pct = ema * 100ULL / INFINITY_BUDGET_MAX_NS;
		u64 denom = 100ULL - pct * INFINITY_VRUNTIME_SLOPE_NUM /
				      INFINITY_VRUNTIME_SLOPE_DEN;

		if (denom >= 20ULL && denom < 100ULL)
			vdelta = div64_u64(vdelta * 100ULL, denom);
	}
	return vdelta;
}

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

	/*
	 * Scale the RR timeslice by rt_ema consumption.
	 * A task with high rt_ema (sustained RT runtime) gets a shorter
	 * timeslice, causing more frequent requeue and giving other
	 * tasks at the same priority more CPU access.
	 *
	 *   rt_ema = 0%    → base timeslice (100ms default)
	 *   rt_ema = 100%  → 10ms minimum
	 */
	if (!p->infinity.rt_ema)
		return rr_default;

	decay_pct = div64_u64(p->infinity.rt_ema * 90ULL,
			      INFINITY_RT_BUDGET_NS);
	if (decay_pct > 90)
		decay_pct = 90;

	return max(1U, (unsigned int)(rr_default * (100ULL - decay_pct)
				      / 100ULL));
}
