/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2026 Galih Tama <galpt@v.recipes>
 *
 * infinity_sched.c — Infinity scheduler algorithm (v4.6).
 *
 * Fully continuous limit-based scheduling:
 *
 *   While running:  ema += (BUDGET_MAX - ema) × δ × α / (BUDGET_MAX × FP_ONE)
 *   While sleeping:  ema >>= min(sleep_ns / 24000000, 63)
 *                      Sub-period residual via 2nd-order Taylor expansion
 *                      (e^-x ≈ 1 - x + x²/2) for continuous decay.
 *   Weight:          effective = base × (100 - ema_pct × 98/100) / 100
 *                      at EMA=100%: base × 2%
 *
 * All task classification data is observed within the scheduler
 * (uclamp declarations, EMA tracking, futex_waiting flag).
 */
#include <linux/math64.h>
#include <linux/sysctl.h>
#include <uapi/linux/sched/types.h>
#include "sched.h"
#include "infinity_sched.h"


/* ------------------------------------------------------------------ */
/* Sysctl tunables                                                     */
/* ------------------------------------------------------------------ */
unsigned long infinity_tune_smt_divisor = INFINITY_SMT_DIVISOR_DEFAULT;
static int infinity_running_flag = 1;
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
/* Initialization                                                      */
/* ------------------------------------------------------------------ */
static int __init infinity_sched_init(void)
{
	__register_sysctl_init("kernel", infinity_sysctl_table,
			      "infinity_sysctl_table",
			      ARRAY_SIZE(infinity_sysctl_table) - 1);
	pr_info("Infinity scheduler active: smt_divisor=%lu\n",
		infinity_tune_smt_divisor);
	return 0;
}
late_initcall(infinity_sched_init);
/* ------------------------------------------------------------------ */
/* infinity_consume — EMA budget consumption                           */
/* ------------------------------------------------------------------ */
void infinity_consume(struct infinity_ctx *ctx, u64 delta_ns,
		      unsigned long cpu_capacity)
{
	u64 step;
	u32 alpha;

	/*
	 * Hardware-adaptive alpha: scale reactivity by the core's physical
	 * capacity.  At max capacity (1024) the alpha reaches 4096 for
	 * sub-millisecond thread storm detection; at reduced capacity
	 * (power saving, thermal throttle) it backs down to 2048.
	 *
	 *   cap=1024 (max perf) → α = 4096  (τ_climb ≈ 0.38ms)
	 *   cap= 512 (mid)      → α = 3072  (τ_climb ≈ 0.5ms,  current default)
	 *   cap= 256 (low)      → α = 2560  (τ_climb ≈ 0.6ms)
	 */
	if (cpu_capacity >= SCHED_CAPACITY_SCALE)
		alpha = 4096;
	else
		alpha = 2048 + (u32)div64_u64(2048ULL * cpu_capacity,
					      SCHED_CAPACITY_SCALE);

	if (ctx->ema >= INFINITY_BUDGET_MAX_NS)
		return;

	if (delta_ns > INFINITY_BUDGET_MAX_NS)
		delta_ns = INFINITY_BUDGET_MAX_NS;

	step = div64_u64((INFINITY_BUDGET_MAX_NS - ctx->ema) * delta_ns *
			 alpha,
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
	 * Exponential shift decay with 24ms half-life, using a 2nd-order
	 * Taylor expansion for the sub-period residual to maintain a
	 * continuous decay curve across the half-life boundary.
	 */
	{
		u64 periods, residual;
		periods = div64_u64_rem(sleep_ns, 24000000ULL, &residual);
		if (periods > 63) {
			ctx->ema = 0;
		} else {
			ctx->ema >>= periods;
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
}
/* ------------------------------------------------------------------ */
/* infinity_fork_init                                                   */
/* ------------------------------------------------------------------ */
void infinity_fork_init(struct infinity_ctx *ctx, u64 now)
{
	ctx->ema = 0;
	ctx->rt_ema = 0;
	ctx->last_sleep_ns = now;
	ctx->rt_last_sleep_ns = 0;

}
/* ------------------------------------------------------------------ */
/* (Removed in v4.5: carriage_ns, auto_carriage_ns, two-pole,          *
 *  prev_ema, infinity_slice, infinity_vruntime_scale,                  *
 *  infinity_wakeup_scale.  All subsumed by weight modulation.)         */
/* ------------------------------------------------------------------ */
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
/* infinity_rt_wakeup — RT EMA decay on wakeup                         */
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
