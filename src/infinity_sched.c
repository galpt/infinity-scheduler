/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2026 Galih Tama <galpt@v.recipes>
 *
 * infinity_sched.c — Infinity scheduler algorithm.
 *
 * Accelerating budget consumption:
 *
 *   debt = min(runtime_debt + delta_exec, CARRIAGE_NS * CAP)
 *   rate = 1 + debt / CARRIAGE_NS
 *   budget -= delta_exec * rate
 *   runtime_debt = debt
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

static int clamp_refill_div(const struct ctl_table *table, int write,
			    void *buf, size_t *lenp, loff_t *ppos)
{
	int ret;
	unsigned long old, val;
	struct ctl_table tmp = *table;

	old = READ_ONCE(infinity_tune_refill_div);
	val = old;
	tmp.data = &val;
	ret = proc_doulongvec_minmax(&tmp, write, buf, lenp, ppos);
	if (write && ret == 0) {
		val = clamp(val, INFINITY_REFILL_DIV_MIN, INFINITY_REFILL_DIV_MAX);
		if (val != old)
			pr_info("Infinity: refill_div %lu -> %lu\n", old, val);
		WRITE_ONCE(infinity_tune_refill_div, val);
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
		.procname	= "infinity_refill_div",
		.data		= &infinity_tune_refill_div,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= clamp_refill_div,
	},
	{
		.procname	= "infinity_smt_divisor",
		.data		= &infinity_tune_smt_divisor,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= clamp_smt_divisor,
	},
	{
		.procname	= "infinity_self_stabilize",
		.data		= &infinity_tune_self_stabilize,
		.maxlen		= sizeof(bool),
		.mode		= 0644,
		.proc_handler	= proc_dobool,
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
		WRITE_ONCE(infinity_tune_refill_div, INFINITY_REFILL_DIV_DEFAULT);
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
static unsigned int infinity_stabilize_count;	/* debounce: consecutive high/low readings */

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
	unsigned long carriage, cap, old_carriage, old_cap;
	int nr_active_cpus = 0;
	bool adjusted = false;

	if (!READ_ONCE(infinity_tune_self_stabilize))
		goto reschedule;

	/*
	 * Aggregate per-CPU metrics over the last window.
	 * Each CPU tracks its own budget exhaustion rate.
	 */
	for_each_online_cpu(cpu) {
		struct infinity_cpu_stats *st = per_cpu_ptr(&infinity_stats, cpu);
		u64 n = st->exhaustion_idx;
		u64 exhaust_window = 0;
		int i;

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

	old_carriage = carriage = READ_ONCE(infinity_tune_carriage_ns);
	old_cap = cap = READ_ONCE(infinity_tune_debt_cap);
	avg_load = nr_active_cpus > 0 ? total_exhaustion_rate / nr_active_cpus : 0;

	/*
	 * Debounce: require 2 consecutive readings above/below threshold
	 * before adjusting.  Adjustments use +12.5% (×9/8 instead of ×5/4)
	 * to converge slowly over minutes rather than seconds.
	 *
	 * Thresholds: high if avg_load > 500 (was 50 — too sensitive).
	 * With INFINITY_STATS_WINDOW = 16 exhaustions per CPU, 500 means
	 * ~31 expensive-per-CPU budget events in the window.
	 */
	if (avg_load > 500 && carriage < INFINITY_CARRIAGE_NS_MAX) {
		infinity_stabilize_count++;
		if (infinity_stabilize_count >= 2) {
			carriage = min(carriage + carriage / 8, INFINITY_CARRIAGE_NS_MAX);
			if (cap > INFINITY_DEBT_CAP_MIN)
				cap = max(cap - cap / 8, (unsigned long)INFINITY_DEBT_CAP_MIN);
			adjusted = true;
			infinity_stabilize_count = 0;
		}
	} else if (avg_load < 50 && carriage > INFINITY_CARRIAGE_NS_MIN) {
		infinity_stabilize_count++;
		if (infinity_stabilize_count >= 2) {
			carriage = max(carriage - carriage / 8, INFINITY_CARRIAGE_NS_MIN);
			if (cap < INFINITY_DEBT_CAP_MAX)
				cap = min(cap + cap / 8, (unsigned long)INFINITY_DEBT_CAP_MAX);
			adjusted = true;
			infinity_stabilize_count = 0;
		}
	} else {
		/* Reset debounce counter on in-range reading */
		infinity_stabilize_count = 0;
	}

	if (adjusted) {
		WRITE_ONCE(infinity_tune_carriage_ns, carriage);
		WRITE_ONCE(infinity_tune_debt_cap, cap);
		pr_info("Infinity: self-stabilize carriage_ns %lu -> %lu, debt_cap %lu -> %lu\n",
			old_carriage, carriage, old_cap, cap);
	}

reschedule:
	schedule_delayed_work(&infinity_stabilize_work, msecs_to_jiffies(2000));
}

/* ------------------------------------------------------------------ */
/* Initialization                                                      */
/* ------------------------------------------------------------------ */

static int __init infinity_sched_init(void)
{
	/* Register sysctl table under kernel/ (matching BORE's approach).
	 * Note: we call __register_sysctl_init() directly instead of the
	 * register_sysctl_init() macro, because the macro computes ARRAY_SIZE
	 * which counts the sentinel {} entry.  Kernel 7.0.12's sysctl_check_table
	 * iterates all entries including the sentinel and rejects it (null procname).
	 * We pass ARRAY_SIZE - 1 to exclude the sentinel from validation.
	 */
	__register_sysctl_init("kernel", infinity_sysctl_table,
			      "infinity_sysctl_table",
			      ARRAY_SIZE(infinity_sysctl_table) - 1);
	__register_sysctl_init("kernel", infinity_reset_table,
			      "infinity_reset_table",
			      ARRAY_SIZE(infinity_reset_table) - 1);

	/* Initialize self-stabilize workqueue */
	INIT_DELAYED_WORK(&infinity_stabilize_work, infinity_stabilize_fn);
	schedule_delayed_work(&infinity_stabilize_work, msecs_to_jiffies(2000));

	pr_info("Infinity scheduler active: carriage=%lu ns, debt_cap=%lu, refill_div=%lu, smt_divisor=%lu\n",
		infinity_tune_carriage_ns, infinity_tune_debt_cap,
		infinity_tune_refill_div, infinity_tune_smt_divisor);

	return 0;
}

/*
 * Called during late init, after the sysctl and procfs infrastructures
 * are guaranteed to be available for creating new subdirectories.
 */
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
