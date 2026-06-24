/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2026 Galih Tama <galpt@v.recipes>
 *
 * infinity_sched.c — Infinity scheduler algorithm.
 *
 * Accelerating budget consumption:
 *
 *   debt += delta_exec
 *   rate = 1 + min(debt / CARRIAGE_NS, CAP)
 *   budget -= delta_exec * rate
 *
 * A freshly woken task has debt = 0, rate = 1 (linear).
 * As the task runs, debt grows and rate increases — budget depletes
 * faster the longer the task runs, approaching zero asymptotically.
 * On wakeup from sleep, debt resets to 0.
 */

#include <linux/types.h>
#include <linux/sched.h>
#include <linux/math64.h>
#include <linux/sysctl.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include "infinity_sched.h"

/* ------------------------------------------------------------------ */
/* Sysctl tunables with safe clamps                                    */
/* ------------------------------------------------------------------ */

unsigned long infinity_tune_carriage_ns = INFINITY_CARRIAGE_NS_DEFAULT;
unsigned long infinity_tune_debt_cap   = INFINITY_DEBT_CAP_DEFAULT;
unsigned long infinity_tune_refill_div = INFINITY_REFILL_DIV_DEFAULT;
unsigned long infinity_tune_smt_divisor = INFINITY_SMT_DIVISOR_DEFAULT;
bool infinity_tune_self_stabilize = true;

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
	unsigned long val;
	struct ctl_table tmp = *table;

	val = READ_ONCE(infinity_tune_carriage_ns);
	tmp.data = &val;
	ret = proc_doulongvec_minmax(&tmp, write, buf, lenp, ppos);
	if (write && ret == 0) {
		val = clamp(val, INFINITY_CARRIAGE_NS_MIN, INFINITY_CARRIAGE_NS_MAX);
		WRITE_ONCE(infinity_tune_carriage_ns, val);
	}
	return ret;
}

static int clamp_debt_cap(const struct ctl_table *table, int write,
			  void *buf, size_t *lenp, loff_t *ppos)
{
	int ret;
	unsigned long val;
	struct ctl_table tmp = *table;

	val = READ_ONCE(infinity_tune_debt_cap);
	tmp.data = &val;
	ret = proc_doulongvec_minmax(&tmp, write, buf, lenp, ppos);
	if (write && ret == 0) {
		val = clamp(val, INFINITY_DEBT_CAP_MIN, INFINITY_DEBT_CAP_MAX);
		WRITE_ONCE(infinity_tune_debt_cap, val);
	}
	return ret;
}

static int clamp_refill_div(const struct ctl_table *table, int write,
			    void *buf, size_t *lenp, loff_t *ppos)
{
	int ret;
	unsigned long val;
	struct ctl_table tmp = *table;

	val = READ_ONCE(infinity_tune_refill_div);
	tmp.data = &val;
	ret = proc_doulongvec_minmax(&tmp, write, buf, lenp, ppos);
	if (write && ret == 0) {
		val = clamp(val, INFINITY_REFILL_DIV_MIN, INFINITY_REFILL_DIV_MAX);
		WRITE_ONCE(infinity_tune_refill_div, val);
	}
	return ret;
}

static int clamp_smt_divisor(const struct ctl_table *table, int write,
			     void *buf, size_t *lenp, loff_t *ppos)
{
	int ret;
	unsigned long val;
	struct ctl_table tmp = *table;

	val = READ_ONCE(infinity_tune_smt_divisor);
	tmp.data = &val;
	ret = proc_doulongvec_minmax(&tmp, write, buf, lenp, ppos);
	if (write && ret == 0) {
		val = clamp(val, INFINITY_SMT_DIVISOR_MIN, INFINITY_SMT_DIVISOR_MAX);
		WRITE_ONCE(infinity_tune_smt_divisor, val);
	}
	return ret;
}

static const struct ctl_table infinity_sysctl_table[] = {
	{
		.procname	= "carriage_ns",
		.data		= &infinity_tune_carriage_ns,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= clamp_carriage_ns,
	},
	{
		.procname	= "debt_cap",
		.data		= &infinity_tune_debt_cap,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= clamp_debt_cap,
	},
	{
		.procname	= "refill_div",
		.data		= &infinity_tune_refill_div,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= clamp_refill_div,
	},
	{
		.procname	= "smt_divisor",
		.data		= &infinity_tune_smt_divisor,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= clamp_smt_divisor,
	},
	{
		.procname	= "self_stabilize",
		.data		= &infinity_tune_self_stabilize,
		.maxlen		= sizeof(bool),
		.mode		= 0644,
		.proc_handler	= proc_dobool,
	},
	{
		.procname	= "running",
		.data		= &(int){1},
		.maxlen		= sizeof(int),
		.mode		= 0444,
		.proc_handler	= proc_dointvec,
		.extra1		= SYSCTL_ONE,
		.extra2		= SYSCTL_ONE,
	},
	{}
};

/* ------------------------------------------------------------------ */
/* Per-CPU stats tracking (BSS-preallocated, like scx_flow)           */
/* ------------------------------------------------------------------ */

#define INFINITY_STATS_WINDOW	16	/* sliding window length */

struct infinity_cpu_stats {
	/* Ring buffer of recent budget exhaustion counts */
	u64 budget_exhaustions[INFINITY_STATS_WINDOW];
	u64 exhaustion_idx;		/* current write index */
};

static DEFINE_PER_CPU(struct infinity_cpu_stats, infinity_stats);

/* ------------------------------------------------------------------ */
/* Self-stabilize delayed work                                         */
/* ------------------------------------------------------------------ */

static struct delayed_work infinity_stabilize_work;

/*
 * Called from update_curr via the budget exhaustion path.
 * Thread-safe: bare atomic increment on a per-CPU counter.
 */
void infinity_try_stabilize(void)
{
	struct infinity_cpu_stats *st = this_cpu_ptr(&infinity_stats);

	st->budget_exhaustions[st->exhaustion_idx % INFINITY_STATS_WINDOW]++;
	st->exhaustion_idx++;
}

static void infinity_stabilize_fn(struct work_struct *work)
{
	int cpu;
	u64 total_exhaustion_rate = 0;
	u64 avg_load = 0;
	unsigned long carriage, cap;
	int nr_active_cpus = 0;

	if (!READ_ONCE(infinity_tune_self_stabilize))
		goto reschedule;

	/*
	 * Aggregate per-CPU metrics over the last window.
	 * Each CPU tracks its own budget exhaustion rate.
	 * A high exhaustion rate means tasks are running out
	 * of budget too quickly, suggesting the acceleration
	 * is too aggressive or the carriage window is too small.
	 */
	for_each_online_cpu(cpu) {
		struct infinity_cpu_stats *st = per_cpu_ptr(&infinity_stats, cpu);
		u64 n = st->exhaustion_idx;
		u64 exhaust_window = 0;
		int i;

		/* Sum the last INFINITY_STATS_WINDOW samples */
		for (i = 0; i < INFINITY_STATS_WINDOW; i++) {
			u64 idx = (n >= INFINITY_STATS_WINDOW)
				? (n - INFINITY_STATS_WINDOW + i) % INFINITY_STATS_WINDOW
				: i;
			if (idx < INFINITY_STATS_WINDOW)
				exhaust_window += st->budget_exhaustions[idx];
		}

		if (exhaust_window > 0) {
			total_exhaustion_rate += exhaust_window;
			nr_active_cpus++;
		}
	}

	/*
	 * Decision logic:
	 *
	 * If exhaustion rate is high → tasks are hitting budget cap often.
	 *   -> Increase carriage_ns (longer base window = lower rate)
	 *   -> Decrease debt_cap (lower cap = less aggressive acceleration)
	 *
	 * If exhaustion rate is very low → system is underutilized.
	 *   -> Decrease carriage_ns (shorter window = better interactivity)
	 *   -> Increase debt_cap (higher cap = more room for bursts)
	 */
	carriage = READ_ONCE(infinity_tune_carriage_ns);
	cap = READ_ONCE(infinity_tune_debt_cap);
	avg_load = nr_active_cpus > 0 ? total_exhaustion_rate / nr_active_cpus : 0;

	if (avg_load > 50 && carriage < INFINITY_CARRIAGE_NS_MAX) {
		/* Exhaustion rate is high — ease up on acceleration */
		carriage = min(carriage * 2, INFINITY_CARRIAGE_NS_MAX);
		if (cap > INFINITY_DEBT_CAP_MIN)
			cap = max(cap / 2, (unsigned long)INFINITY_DEBT_CAP_MIN);
	} else if (avg_load < 10 && carriage > INFINITY_CARRIAGE_NS_MIN) {
		/* Exhaustion rate is low — tighten for better interactivity */
		carriage = max(carriage / 2, INFINITY_CARRIAGE_NS_MIN);
		if (cap < INFINITY_DEBT_CAP_MAX)
			cap = min(cap * 2, (unsigned long)INFINITY_DEBT_CAP_MAX);
	}

	WRITE_ONCE(infinity_tune_carriage_ns, carriage);
	WRITE_ONCE(infinity_tune_debt_cap, cap);

reschedule:
	/* Run every 2 seconds */
	schedule_delayed_work(&infinity_stabilize_work, msecs_to_jiffies(2000));
}

/* ------------------------------------------------------------------ */
/* Initialization                                                      */
/* ------------------------------------------------------------------ */

static int __init infinity_sched_init(void)
{
	/* Register sysctl table */
	register_sysctl_init("kernel/infinity", infinity_sysctl_table);

	/* Initialize self-stabilize workqueue */
	INIT_DELAYED_WORK(&infinity_stabilize_work, infinity_stabilize_fn);
	schedule_delayed_work(&infinity_stabilize_work, msecs_to_jiffies(2000));

	pr_info("Infinity scheduler active: carriage=%lu ns, debt_cap=%lu, refill_div=%lu, smt_divisor=%lu\n",
		infinity_tune_carriage_ns, infinity_tune_debt_cap,
		infinity_tune_refill_div, infinity_tune_smt_divisor);

	return 0;
}

/*
 * Called early from the kernel init path.
 * The Makefile adds infinity_sched.o to obj-y, so this runs during
 * the scheduler init phase.
 */
early_initcall(infinity_sched_init);

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
/* infinity_consume — accelerating budget consumption                  */
/* ------------------------------------------------------------------ */

void infinity_consume(struct infinity_ctx *ctx, u64 delta_ns)
{
	u64 debt, rate, cap, carriage, prev;
	u64 consumption;

	carriage = READ_ONCE(infinity_tune_carriage_ns);
	cap = READ_ONCE(infinity_tune_debt_cap);

	prev = ctx->budget_ns;
	debt = min(ctx->runtime_debt + delta_ns, carriage * cap);
	rate = 1 + div64_u64(debt, carriage);
	consumption = delta_ns * rate;
	ctx->budget_ns = infinity_clamp_budget(ctx->budget_ns - (s64)consumption);
	ctx->runtime_debt = debt;

	/* Track budget exhaustion for self-stabilize feedback */
	if (prev > 0 && ctx->budget_ns <= 0)
		infinity_try_stabilize();
}

/* ------------------------------------------------------------------ */
/* infinity_refill_budget                                             */
/* ------------------------------------------------------------------ */

void infinity_refill_budget(struct infinity_ctx *ctx, u64 sleep_ns)
{
	u64 refill_div;
	s64 refill_ns;

	refill_div = READ_ONCE(infinity_tune_refill_div);

	if (sleep_ns > INFINITY_SLEEP_MAX_NS)
		sleep_ns = INFINITY_SLEEP_MAX_NS;

	refill_ns = (s64)(sleep_ns / refill_div);

	/* Interactive tasks that sleep briefly get a minimum refill floor. */
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
