/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2026 Galih Tama <galpt@v.recipes>
 *
 * infinity_sched.c — Infinity scheduler algorithm (dev).
 *
 * Fully continuous limit-based fair and RT scheduling:
 *
 *   While running:  ema += (BUDGET_MAX - ema) * delta_ns * α / (BUDGET_MAX * FP_ONE)
 *   While sleeping:  ema -= ema * sleep_ns * α * D / (BUDGET_MAX * FP_ONE)
 *   slice = share * (100 - ema_pct * 9/10) / 100  (active throttle, steep)
 *   vslice' = vslice * ema / BUDGET_MAX  (asymptotic, no cap)
 *
 * Key differences from v4:
 *   - Per-task hrtimer for deadline tracking (tick-independent)
 *   - 50% of fair share minimum slice (proportional, not absolute)
 *   - Carriage_ns auto-scaled from CPU count (no sysctl needed)
 *   - Faster decay τ (4× faster than climb, 24ms vs 96ms)
 *   - Steeper vruntime scaling (× 9/10 slope, max 10× vs 4×)
 *
 * The EMA converges asymptotically toward BUDGET_MAX when running and
 * toward 0 when sleeping — the true Limitless.  No clamps, no thresholds,
 * no external feedback loop.  Self-stabilising by construction.
 */

#include <linux/types.h>
#include <linux/sched.h>
#include <linux/math64.h>
#include <linux/sysctl.h>
#include "sched.h"
#include <linux/hrtimer.h>
#include "infinity_sched.h"

/* ------------------------------------------------------------------ */
/* Tunables with safe clamps                                           */
/* ------------------------------------------------------------------ */

unsigned long infinity_tune_smt_divisor = INFINITY_SMT_DIVISOR_DEFAULT;
static int infinity_running_flag = 1;

/* Auto-scaled carriage — set at init, not user-tunable */
static unsigned long infinity_carriage_ns;

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
/* Per-CPU deadline hrtimer (tick-independent scheduling)              */
/* ------------------------------------------------------------------ */

static DEFINE_PER_CPU(struct hrtimer, infinity_deadline_timer);

static enum hrtimer_restart infinity_deadline_callback(struct hrtimer *timer)
{
	int cpu = smp_processor_id();

	resched_curr(cpu_rq(cpu));
	return HRTIMER_NORESTART;
}

void infinity_timer_start(struct rq *rq, u64 slice_ns)
{
	struct hrtimer *timer;

	timer = this_cpu_ptr(&infinity_deadline_timer);
	hrtimer_start(timer, ns_to_ktime(slice_ns),
		      HRTIMER_MODE_REL_PINNED_SOFT);
}

void infinity_timer_cancel(void)
{
	struct hrtimer *timer;

	timer = this_cpu_ptr(&infinity_deadline_timer);
	hrtimer_try_to_cancel(timer);
}

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
	int cpu;

	__register_sysctl_init("kernel", infinity_sysctl_table,
			      "infinity_sysctl_table",
			      ARRAY_SIZE(infinity_sysctl_table) - 1);

	for_each_possible_cpu(cpu) {
		struct hrtimer *timer = per_cpu_ptr(&infinity_deadline_timer, cpu);

		hrtimer_setup(timer, infinity_deadline_callback,
			      CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED_SOFT);
	}

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
	 * Uses a steeper slope (× 9/10) for up to 10× reduction at EMA=100%.
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
	u64 step;

	if (sleep_ns == 0)
		return;

	/*
	 * Decay is 4× faster than climb (τ_decay = τ_climb / 4 = 24ms).
	 * This asymmetry gives interactive tasks faster recovery during
	 * brief sleeps (e.g. GPU pipeline bubbles) while the slower climb
	 * ensures CPU-bound tasks are not penalised prematurely.
	 *
	 * step = ema × sleep_ns × α × D / (BUDGET_MAX × FP_ONE)
	 *       where D = INFINITY_EMA_DECAY_DIV = 4
	 *
	 * The proportional clamp (step > ema) prevents underflow — the
	 * EMA approaches 0 asymptotically but never reaches it in finite
	 * time.
	 */
	ctx->prev_ema = ctx->ema;
	step = div64_u64(ctx->ema * sleep_ns * INFINITY_EMA_ALPHA *
			 INFINITY_EMA_DECAY_DIV,
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

u64 infinity_vruntime_scale(u64 vdelta, u64 ema)
{
	if (ema) {
		u64 pct = ema * 100ULL / INFINITY_BUDGET_MAX_NS;
		/*
		 * Steeper slope: × 9/10 instead of × 3/4.
		 * denom = 100 - pct × 9/10, always ≥ 10.
		 * At pct=100: scale = 100/10 = 10× (vs 4×).
		 */
		u64 denom = 100ULL - pct * INFINITY_VRUNTIME_SLOPE_NUM /
				      INFINITY_VRUNTIME_SLOPE_DEN;

		if (denom < 100ULL)
			vdelta = div64_u64(vdelta * 100ULL, denom);
	}
	return vdelta;
}

/* ------------------------------------------------------------------ */
/* infinity_rt_consume — EMA climb on RT runtime                       */
/* ------------------------------------------------------------------ */

void infinity_rt_consume(struct infinity_ctx *ctx, u64 delta_ns)
{
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

	step = div64_u64(ctx->rt_ema * sleep_ns * INFINITY_RT_ALPHA *
			 INFINITY_EMA_DECAY_DIV,
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
