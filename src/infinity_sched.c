// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2026 Galih Tama <galpt@v.recipes>
 *
 * infinity_sched.c -- Infinity scheduler algorithm (v4.8-gpu).
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
 * (uclamp declarations, EMA tracking, futex_waiting and ipc_waiting flags).
 */
#include <linux/fs.h>
#include <linux/math64.h>
#include <linux/slab.h>
#include <linux/sysctl.h>
#include <linux/string.h>
#include <uapi/linux/sched/types.h>
#include <drm/gpu_scheduler.h>
#include "sched.h"
#include "infinity_sched.h"


/* ------------------------------------------------------------------ */
/* Stats counters                                                      */
/* ------------------------------------------------------------------ */
/*
 * Per-CPU stat counters: increment sites run on arbitrary CPUs (tick,
 * wakeup, GPU fence callbacks), and a single global atomic would bounce
 * one cache line across sockets on multi-node machines.  Per-CPU storage
 * keeps the hot increments local; infinity_stats_total() sums all CPUs.
 */
DEFINE_PER_CPU(atomic64_t, infinity_futex_boost_count);
EXPORT_PER_CPU_SYMBOL(infinity_futex_boost_count);
DEFINE_PER_CPU(atomic64_t, infinity_ipc_boost_count);
EXPORT_PER_CPU_SYMBOL(infinity_ipc_boost_count);
DEFINE_PER_CPU(atomic64_t, infinity_ipc_wakeup_count);
EXPORT_PER_CPU_SYMBOL(infinity_ipc_wakeup_count);
DEFINE_PER_CPU(atomic64_t, infinity_shield_engage_count);
EXPORT_PER_CPU_SYMBOL(infinity_shield_engage_count);
DEFINE_PER_CPU(atomic64_t, infinity_divergence_count);
EXPORT_PER_CPU_SYMBOL(infinity_divergence_count);
DEFINE_PER_CPU(atomic64_t, infinity_ema_climb_count);
EXPORT_PER_CPU_SYMBOL(infinity_ema_climb_count);
DEFINE_PER_CPU(atomic64_t, infinity_wakeup_count);
EXPORT_PER_CPU_SYMBOL(infinity_wakeup_count);
DEFINE_PER_CPU(atomic64_t, infinity_rt_throttle_count);
EXPORT_PER_CPU_SYMBOL(infinity_rt_throttle_count);
DEFINE_PER_CPU(atomic64_t, infinity_gpu_completion_callbacks);
EXPORT_PER_CPU_SYMBOL(infinity_gpu_completion_callbacks);
DEFINE_PER_CPU(atomic64_t, infinity_gpu_accounting_applied);
EXPORT_PER_CPU_SYMBOL(infinity_gpu_accounting_applied);
DEFINE_PER_CPU(atomic64_t, infinity_gpu_accounting_skipped);
EXPORT_PER_CPU_SYMBOL(infinity_gpu_accounting_skipped);
DEFINE_PER_CPU(atomic64_t, infinity_gpu_passover_boosts);
EXPORT_PER_CPU_SYMBOL(infinity_gpu_passover_boosts);
DEFINE_PER_CPU(atomic64_t, infinity_gpu_idle_compensations);
EXPORT_PER_CPU_SYMBOL(infinity_gpu_idle_compensations);
DEFINE_PER_CPU(atomic64_t, infinity_gpu_cpu_coupling_activations);
EXPORT_PER_CPU_SYMBOL(infinity_gpu_cpu_coupling_activations);
DEFINE_PER_CPU(atomic64_t, infinity_gpu_lock_drain_rounds);
EXPORT_PER_CPU_SYMBOL(infinity_gpu_lock_drain_rounds);
DEFINE_PER_CPU(atomic64_t, infinity_cpufreq_interactive_count);
EXPORT_PER_CPU_SYMBOL(infinity_cpufreq_interactive_count);
DEFINE_PER_CPU(atomic64_t, infinity_smt_interactive_count);
EXPORT_PER_CPU_SYMBOL(infinity_smt_interactive_count);
/* ------------------------------------------------------------------ */
/* Sysctl tunables                                                     */
/* ------------------------------------------------------------------ */
unsigned long infinity_tune_smt_divisor = INFINITY_SMT_DIVISOR_DEFAULT;
static int infinity_running_flag = 1;
static const char infinity_version[] = "v4.8-gpu";
/* Infinity: smt_divisor bounds.  Out-of-range writes are rejected by the
 * proc layer (extra1/extra2) instead of silently clamped, closing the
 * window where an out-of-range value was briefly visible to scheduler
 * ticks.  The handler only reports changes.
 */
static int clamp_smt_divisor(const struct ctl_table *table, int write,
			     void *buf, size_t *lenp, loff_t *ppos)
{
	unsigned long old, val;
	int ret;

	old = READ_ONCE(infinity_tune_smt_divisor);
	ret = proc_doulongvec_minmax(table, write, buf, lenp, ppos);
	if (write && ret == 0) {
		val = READ_ONCE(infinity_tune_smt_divisor);
		if (val != old)
			pr_info("Infinity: smt_divisor %lu -> %lu\n", old, val);
	}
	return ret;
}
/* ------------------------------------------------------------------ */
/*
 * fill_pretty_llu - format u64 with K/M/B/T/Qa/Qi suffixes, two
 * decimal digits, floor-truncated (1230 -> "1.23K").  The output is
 * at most 8 characters, so the table columns never shift regardless
 * of how large the counters grow.
 */
static const struct infinity_suffix {
	u64		base;
	const char	*suffix;
} infinity_suffixes[] = {
	{ 1000000000000000000ULL, "Qi" },
	{ 1000000000000000ULL,    "Qa" },
	{ 1000000000000ULL,       "T"  },
	{ 1000000000ULL,          "B"  },
	{ 1000000ULL,             "M"  },
	{ 1000ULL,                "K"  },
};

static char *fill_pretty_llu(char *buf, size_t sz, u64 val)
{
	u64 q, r, frac;
	int i;

	for (i = 0; i < ARRAY_SIZE(infinity_suffixes); i++) {
		if (val < infinity_suffixes[i].base)
			continue;

		q = div64_u64_rem(val, infinity_suffixes[i].base, &r);
		frac = mul_u64_u32_div(r, 100, infinity_suffixes[i].base);
		scnprintf(buf, sz, "%llu.%02llu%s",
			  (unsigned long long)q, (unsigned long long)frac,
			  infinity_suffixes[i].suffix);
		return buf;
	}

	scnprintf(buf, sz, "%llu", (unsigned long long)val);
	return buf;
}

/* ------------------------------------------------------------------ */
/* Stats display handler                                               */
/* ------------------------------------------------------------------ */
/*
 * infinity_stats_total - sum a per-CPU stat counter across all CPUs.
 *
 * Each increment lands on exactly one CPU's counter, so the sum is the
 * true total.  64-bit counters cannot wrap in practice, so the sum is
 * exact for any value the kernel can produce.
 */
static u64 infinity_stats_total(const atomic64_t __percpu *counter)
{
	u64 total = 0;
	int cpu;

	for_each_possible_cpu(cpu)
		total += atomic64_read(per_cpu_ptr(counter, cpu));

	return total;
}

struct infinity_stats_row {
	const char	*label;
	char		value[16];
	char		note[64];
};

struct infinity_stats_section {
	const char		*name;
	struct infinity_stats_row *rows;
	int			nrows;
};

static size_t infinity_stats_emit_sep(char *buf, size_t sz, int lw, int vw, int nw)
{
	char *p = buf;
	size_t need = (size_t)lw + vw + nw + 13;

	if (sz < need)
		return 0;

	*p++ = '+';
	memset(p, '-', lw + 2);
	p += lw + 2;
	*p++ = '+';
	memset(p, '-', vw + 2);
	p += vw + 2;
	*p++ = '+';
	memset(p, '-', nw + 2);
	p += nw + 2;
	*p++ = '+';
	*p++ = '\n';
	*p = '\0';

	return need - 2;
}

#ifdef CONFIG_FAIR_GROUP_SCHED
/*
 * tg_shield_visitor -- count task groups whose shield is engaged (cached
 * cross-CPU EMA max at/above the engage threshold).  Read-only, no control
 * path; called under rcu_read_lock() from the stats handler, which is what
 * walk_tg_tree_from() requires.
 */
static int tg_shield_visitor(struct task_group *tg, void *data)
{
	int *n = data;

	if (tg != &root_task_group &&
	    READ_ONCE(tg->infinity_shield.shield_ema_max) >=
	    INFINITY_SHIELD_ENGAGE_THRESHOLD_NS)
		(*n)++;
	return 0;
}
#endif

static int infinity_stats_proc_handler(const struct ctl_table *ctl, int write,
				       void *buffer, size_t *lenp,
				       loff_t *ppos)
{
	/*
	 * The table columns are sized from the widest content in each
	 * column across all sections, so the borders always line up no
	 * matter how the labels, values or notes grow or shrink.  The
	 * buffer is sized from the same measurements, so the output can
	 * never be truncated either.
	 */
	struct infinity_stats_row cpu_rows[10], rt_rows[1], gpu_rows[7];
	struct infinity_stats_section sections[3] = {
		{ "CPU", cpu_rows, 10 },
		{ "RT",  rt_rows,  1 },
		{ "GPU", gpu_rows, 7 },
	};
	u64 fbc, emc, wkc, rtc, gcb, gapp, gskp;
	u64 gic, gcca, gpbo, gldr, icf, ismt;
	u64 ipb, ipw, sec;
	u64 dvg;
	char *buf;
	size_t bufsz, off = 0;
	int lw = 0, vw = 0, nw = 0, s, r;

	if (write)
		return -EROFS;

	fbc  = infinity_stats_total(&infinity_futex_boost_count);
	emc  = infinity_stats_total(&infinity_ema_climb_count);
	wkc  = infinity_stats_total(&infinity_wakeup_count);
	rtc  = infinity_stats_total(&infinity_rt_throttle_count);
	gcb  = infinity_stats_total(&infinity_gpu_completion_callbacks);
	gapp = infinity_stats_total(&infinity_gpu_accounting_applied);
	gskp = infinity_stats_total(&infinity_gpu_accounting_skipped);
	gic  = infinity_stats_total(&infinity_gpu_idle_compensations);
	gcca = infinity_stats_total(&infinity_gpu_cpu_coupling_activations);
	gpbo = infinity_stats_total(&infinity_gpu_passover_boosts);
	gldr = infinity_stats_total(&infinity_gpu_lock_drain_rounds);
	icf  = infinity_stats_total(&infinity_cpufreq_interactive_count);
	ismt = infinity_stats_total(&infinity_smt_interactive_count);
	ipb  = infinity_stats_total(&infinity_ipc_boost_count);
	ipw  = infinity_stats_total(&infinity_ipc_wakeup_count);
	sec  = infinity_stats_total(&infinity_shield_engage_count);
	dvg  = infinity_stats_total(&infinity_divergence_count);

	/* ---- CPU rows ---- */
	cpu_rows[0].label = "Futex boosts";
	fill_pretty_llu(cpu_rows[0].value, sizeof(cpu_rows[0].value), fbc);
	if (emc)
		scnprintf(cpu_rows[0].note, sizeof(cpu_rows[0].note),
			  "%llu%% of tasks", mul_u64_u32_div(fbc, 100, emc));
	else
		strscpy(cpu_rows[0].note, "N/A", sizeof(cpu_rows[0].note));

	cpu_rows[1].label = "EMA climbs";
	fill_pretty_llu(cpu_rows[1].value, sizeof(cpu_rows[1].value), emc);
	if (wkc) {
		u64 avg_wakeup = mul_u64_u32_div(emc, 100, wkc);

		scnprintf(cpu_rows[1].note, sizeof(cpu_rows[1].note),
			  "~%llu.%02llu/wakeup",
			  avg_wakeup / 100, avg_wakeup % 100);
	} else {
		strscpy(cpu_rows[1].note, "N/A", sizeof(cpu_rows[1].note));
	}

	cpu_rows[2].label = "Wakeup decays";
	fill_pretty_llu(cpu_rows[2].value, sizeof(cpu_rows[2].value), wkc);
	cpu_rows[2].note[0] = '\0';

	cpu_rows[3].label = "Interactive cpufreq";
	fill_pretty_llu(cpu_rows[3].value, sizeof(cpu_rows[3].value), icf);
	strscpy(cpu_rows[3].note, "frequency ramp events",
		sizeof(cpu_rows[3].note));

	cpu_rows[4].label = "SMT placement";
	fill_pretty_llu(cpu_rows[4].value, sizeof(cpu_rows[4].value), ismt);
	strscpy(cpu_rows[4].note, "interactive moves to idle core",
		sizeof(cpu_rows[4].note));

	cpu_rows[5].label = "IPC boosts";
	fill_pretty_llu(cpu_rows[5].value, sizeof(cpu_rows[5].value), ipb);
	if (ipw)
		scnprintf(cpu_rows[5].note, sizeof(cpu_rows[5].note),
			  "%llu%% of IPC wakeups",
			  mul_u64_u32_div(ipb, 100, ipw));
	else
		strscpy(cpu_rows[5].note, "no IPC wakeups yet",
			sizeof(cpu_rows[5].note));

	cpu_rows[6].label = "IPC wakeups";
	fill_pretty_llu(cpu_rows[6].value, sizeof(cpu_rows[6].value), ipw);
	strscpy(cpu_rows[6].note, "wait_woken candidates",
		sizeof(cpu_rows[6].note));

	cpu_rows[7].label = "Shield engages";
	fill_pretty_llu(cpu_rows[7].value, sizeof(cpu_rows[7].value), sec);
	strscpy(cpu_rows[7].note, "group share reductions applied",
		sizeof(cpu_rows[7].note));

	{
		int n = 0;

#ifdef CONFIG_FAIR_GROUP_SCHED
		rcu_read_lock();
		walk_tg_tree_from(&root_task_group, tg_shield_visitor, tg_nop, &n);
		rcu_read_unlock();
#endif
		cpu_rows[8].label = "Group shields";
		fill_pretty_llu(cpu_rows[8].value, sizeof(cpu_rows[8].value), n);
		strscpy(cpu_rows[8].note, "groups defending interactive tasks",
			sizeof(cpu_rows[8].note));
	}

	cpu_rows[9].label = "EMA vs PELT divergence";
	fill_pretty_llu(cpu_rows[9].value, sizeof(cpu_rows[9].value), dvg);
	strscpy(cpu_rows[9].note, "sustained episodes", sizeof(cpu_rows[9].note));

	/* ---- RT rows ---- */
	rt_rows[0].label = "RT throttles";
	fill_pretty_llu(rt_rows[0].value, sizeof(rt_rows[0].value), rtc);
	strscpy(rt_rows[0].note, "FIFO rogue requeues",
		sizeof(rt_rows[0].note));

	/* ---- GPU rows ---- */
	gpu_rows[0].label = "Completion entered";
	fill_pretty_llu(gpu_rows[0].value, sizeof(gpu_rows[0].value), gcb);
	strscpy(gpu_rows[0].note, "fence callbacks fired",
		sizeof(gpu_rows[0].note));

	gpu_rows[1].label = "Accounting applied";
	fill_pretty_llu(gpu_rows[1].value, sizeof(gpu_rows[1].value), gapp);
	if (gcb)
		scnprintf(gpu_rows[1].note, sizeof(gpu_rows[1].note),
			  "%llu.%02llu%%",
			  mul_u64_u32_div(gapp, 100, gcb),
			  mul_u64_u32_div(gapp, 10000, gcb) % 100);
	else
		strscpy(gpu_rows[1].note, "N/A", sizeof(gpu_rows[1].note));

	gpu_rows[2].label = "  >> entity not found";
	fill_pretty_llu(gpu_rows[2].value, sizeof(gpu_rows[2].value), gskp);
	strscpy(gpu_rows[2].note, "(entity_kill race)",
		sizeof(gpu_rows[2].note));

	gpu_rows[3].label = "Idle compensation";
	fill_pretty_llu(gpu_rows[3].value, sizeof(gpu_rows[3].value), gic);
	strscpy(gpu_rows[3].note, "proportional idle boost",
		sizeof(gpu_rows[3].note));

	gpu_rows[4].label = "CPU->GPU coupling";
	fill_pretty_llu(gpu_rows[4].value, sizeof(gpu_rows[4].value), gcca);
	strscpy(gpu_rows[4].note, "interactive vtime reduction",
		sizeof(gpu_rows[4].note));

	gpu_rows[5].label = "GPU->CPU coupling";
	fill_pretty_llu(gpu_rows[5].value, sizeof(gpu_rows[5].value), gpbo);
	strscpy(gpu_rows[5].note, "passover EMA boost",
		sizeof(gpu_rows[5].note));

	gpu_rows[6].label = "Drain count";
	fill_pretty_llu(gpu_rows[6].value, sizeof(gpu_rows[6].value), gldr);
	strscpy(gpu_rows[6].note, "batch drain operations",
		sizeof(gpu_rows[6].note));

	/* measure the widest content per column across all sections */
	for (s = 0; s < 3; s++)
		for (r = 0; r < sections[s].nrows; r++) {
			lw = max_t(int, lw, (int)strlen(sections[s].rows[r].label));
			vw = max_t(int, vw, (int)strlen(sections[s].rows[r].value));
			nw = max_t(int, nw, (int)strlen(sections[s].rows[r].note));
		}

	/*
	 * Size the buffer from the same measurements so the rendered
	 * table can never be truncated: the header, per-section name
	 * and two separators, every row, the blank lines between
	 * sections, plus a fixed allowance for the footer lines.
	 */
	bufsz = strlen("Infinity Scheduler ") + strlen(infinity_version) + 2;
	for (s = 0; s < 3; s++) {
		bufsz += strlen(sections[s].name) + 1;
		bufsz += 2 * ((size_t)lw + vw + nw + 13);
		bufsz += (size_t)sections[s].nrows * ((size_t)lw + vw + nw + 13);
		bufsz += 2;
	}
	bufsz += 256;

	buf = kmalloc(bufsz, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	off = scnprintf(buf, bufsz, "Infinity Scheduler %s\n\n",
			infinity_version);

	for (s = 0; s < 3; s++) {
		off += scnprintf(buf + off, bufsz - off, "%s\n",
				 sections[s].name);
		off += infinity_stats_emit_sep(buf + off, bufsz - off,
					       lw, vw, nw);

		for (r = 0; r < sections[s].nrows; r++)
			off += scnprintf(buf + off, bufsz - off,
					 "| %-*s | %*s | %-*s |\n",
					 lw, sections[s].rows[r].label,
					 vw, sections[s].rows[r].value,
					 nw, sections[s].rows[r].note);

		off += infinity_stats_emit_sep(buf + off, bufsz - off,
					       lw, vw, nw);
		off += scnprintf(buf + off, bufsz - off, "\n");
	}

	/* Footer */
	if (gcb) {
		u64 whole = mul_u64_u32_div(gapp, 100, gcb);
		u64 frac  = mul_u64_u32_div(gapp, 10000, gcb) % 100;
		char w1[16], w2[16];

		fill_pretty_llu(w1, sizeof(w1), gapp);
		fill_pretty_llu(w2, sizeof(w2), gcb);
		off += scnprintf(buf + off, bufsz - off,
				 "Accounting confidence: %llu.%02llu%%  (%s / %s completions)\n",
				 whole, frac, w1, w2);
	} else {
		off += scnprintf(buf + off, bufsz - off,
				 "Accounting confidence: N/A  (no GPU jobs tracked)\n");
	}

	if (!gcb)
		off += scnprintf(buf + off, bufsz - off,
				 "Verdict: No GPU jobs recorded yet\n");
	else if (gapp >= gcb)
		off += scnprintf(buf + off, bufsz - off,
				 "Verdict: All counters healthy -- system operating normally\n");
	else
		off += scnprintf(buf + off, bufsz - off,
				 "Verdict: Accounting mismatch detected -- see above for details\n");

	{
		const struct ctl_table tmp = {
			.data		= buf,
			.maxlen		= bufsz,
		};

		proc_dostring(&tmp, write, buffer, lenp, ppos);
	}
	kfree(buf);
	return 0;
}
/* ------------------------------------------------------------------ */
/* Sysctl table                                                        */
/* ------------------------------------------------------------------ */
static unsigned long infinity_smt_divisor_min = INFINITY_SMT_DIVISOR_MIN;
static unsigned long infinity_smt_divisor_max = INFINITY_SMT_DIVISOR_MAX;

static const struct ctl_table infinity_sysctl_table[] = {
	{
		.procname	= "infinity_smt_divisor",
		.data		= &infinity_tune_smt_divisor,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= clamp_smt_divisor,
		.extra1		= &infinity_smt_divisor_min,
		.extra2		= &infinity_smt_divisor_max,
	},
	{
		.procname	= "infinity_running",
		.data		= &infinity_running_flag,
		.maxlen		= sizeof(int),
		.mode		= 0444,
		.proc_handler	= proc_dointvec,
	},
	{
		.procname	= "infinity_version",
		.data		= (char *)infinity_version,
		.maxlen		= sizeof(infinity_version),
		.mode		= 0444,
		.proc_handler	= proc_dostring,
	},
	{
		.procname	= "infinity_stats",
		.data		= NULL,
		.maxlen		= 0,
		.mode		= 0444,
		.proc_handler	= infinity_stats_proc_handler,
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
/* infinity_consume -- EMA budget consumption                           */
/* ------------------------------------------------------------------ */
void infinity_consume(struct infinity_ctx *ctx, u64 delta_ns,
		      unsigned long cpu_capacity)
{
	u64 step, ema;
	u32 alpha;

	/*
	 * Hardware-adaptive alpha: scale reactivity by the core's physical
	 * capacity.  At max capacity (1024) the alpha reaches 4096 for
	 * sub-millisecond thread storm detection; at reduced capacity
	 * (power saving, thermal throttle) it backs down to 2048.
	 *
	 *   cap=1024 (max perf) → α = 4096  (τ_climb ≈ 0.38ms)
	 *   cap= 512 (mid)      → α = 3072  (τ_climb ≈ 0.5ms)
	 *   cap= 256 (low)      → α = 2560  (τ_climb ≈ 0.6ms)
	 */
	if (cpu_capacity >= SCHED_CAPACITY_SCALE)
		alpha = 4096;
	else
		alpha = 2048 + (u32)div64_u64(2048ULL * cpu_capacity,
					      SCHED_CAPACITY_SCALE);

	ema = READ_ONCE(ctx->ema);
	if (ema >= INFINITY_BUDGET_MAX_NS)
		return;

	if (delta_ns > INFINITY_BUDGET_MAX_NS)
		delta_ns = INFINITY_BUDGET_MAX_NS;

	step = div64_u64((INFINITY_BUDGET_MAX_NS - ema) * delta_ns * alpha,
			 INFINITY_BUDGET_MAX_NS * INFINITY_FP_ONE);
	/*
	 * With alpha up to 4096 (FP_ONE = 256) the step can exceed the
	 * remaining gap to the ceiling, which would push ema past
	 * INFINITY_BUDGET_MAX_NS (the weight math clamps it downstream,
	 * but the raw value must stay within [0, BUDGET] so the
	 * /proc/<pid>/infinity reading and the EMA invariants hold).
	 */
	if (step > INFINITY_BUDGET_MAX_NS - ema)
		step = INFINITY_BUDGET_MAX_NS - ema;
	WRITE_ONCE(ctx->ema, ema + step);
	atomic64_inc(this_cpu_ptr(&infinity_ema_climb_count));
}
/* ------------------------------------------------------------------ */
/* infinity_wakeup -- EMA decay on wakeup                               */
/* ------------------------------------------------------------------ */
void infinity_wakeup(struct infinity_ctx *ctx, u64 sleep_ns)
{
	if (sleep_ns == 0)
		return;

	/* GPU-to-CPU feedback: if this task's GPU entity has been
	 * repeatedly passed over in scheduling, accelerate the EMA
	 * decay so the task appears more interactive on the CPU side.
	 * Each passover adds one extra sleep-duration of decay (a 24ms
	 * sleep with one passover decays twice as far).
	 */
	{
		int passovers = atomic_xchg(&ctx->gpu_passovers, 0);

		if (passovers > 0) {
			u64 extra_ns = sleep_ns * min(passovers, 8);

			sleep_ns += extra_ns;
			atomic64_inc(this_cpu_ptr(&infinity_gpu_passover_boosts));
		}
	}

	/*
	 * Exponential shift decay with 24ms half-life, using a 2nd-order
	 * Taylor expansion for the sub-period residual to maintain a
	 * continuous decay curve across the half-life boundary.
	 */
	{
		u64 periods, residual;

		periods = div64_u64_rem(sleep_ns, 24000000ULL, &residual);
		if (periods > 63) {
			WRITE_ONCE(ctx->ema, 0);
		} else {
			u64 ema = READ_ONCE(ctx->ema);

			ema >>= periods;
			if (residual && ema) {
				u64 fraction = div64_u64(residual *
					INFINITY_FP_ONE, 24000000ULL);
				u64 linear = (ema * fraction) >>
					INFINITY_FP_SHIFT;
				u64 quad = ((linear * fraction) >>
					INFINITY_FP_SHIFT) >> 1;
				if (linear > quad)
					ema -= min(ema, linear - quad);
			}
			WRITE_ONCE(ctx->ema, ema);
		}
	}
	atomic64_inc(this_cpu_ptr(&infinity_wakeup_count));
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
	atomic_set(&ctx->gpu_passovers, 0);
	ctx->futex_waiting = false;
	ctx->ipc_waiting = false;
	ctx->ipc_last_boost = 0;
	ctx->rt_valve_armed = false;
	ctx->rt_valve_last_jiffies = 0;
	ctx->divergence_streak = 0;
}

/* ------------------------------------------------------------------ */
/* infinity_exec_reset -- clear per-image scheduler state on exec       */
/* ------------------------------------------------------------------ */
void infinity_exec_reset(struct infinity_ctx *ctx)
{
	/* exec() reuses the same task: the previous image's EMA penalty,
	 * RT valve state, IPC rate limit and divergence streak must not
	 * carry over into the new image (fork() already reinitializes
	 * everything for new tasks).
	 */
	WRITE_ONCE(ctx->ema, 0);
	WRITE_ONCE(ctx->rt_ema, 0);
	WRITE_ONCE(ctx->ipc_last_boost, 0);
	WRITE_ONCE(ctx->rt_valve_armed, false);
	WRITE_ONCE(ctx->rt_valve_last_jiffies, 0);
	ctx->divergence_streak = 0;
}

/* ------------------------------------------------------------------ */
/* (Removed in v4.5: carriage_ns, auto_carriage_ns, two-pole,          *
 *  prev_ema, infinity_slice, infinity_vruntime_scale,                  *
 *  infinity_wakeup_scale.  All subsumed by weight modulation.)
 */
/* ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ */
/* infinity_rt_consume -- EMA climb on RT runtime                       */
/* ------------------------------------------------------------------ */
void infinity_rt_consume(struct infinity_ctx *ctx, u64 delta_ns)
{
	u64 step, ema;

	ema = READ_ONCE(ctx->rt_ema);
	if (unlikely(ema >= INFINITY_RT_BUDGET_NS)) {
		WRITE_ONCE(ctx->rt_ema, INFINITY_RT_BUDGET_NS);
		return;
	}
	/* Clamp the fold at one time constant: a tickless (NO_HZ_FULL) run
	 * longer than one tau (640ms) saturates the EMA at the ceiling.
	 * The old 10ms delta clamp let a long tickless run add only 1/64
	 * of the remaining gap per call, making the 95% engage threshold
	 * unreachable.  The fold below is identical to the old formula for
	 * deltas under one tau and saturates at or beyond it, so the EMA
	 * now tracks wall-clock runtime.  The early return also bounds the
	 * multiply: delta < tau implies (BUDGET - ema) * delta <
	 * BUDGET * TAU, far below u64 limits.
	 */
	if (delta_ns >= INFINITY_RT_TAU_NS) {
		WRITE_ONCE(ctx->rt_ema, INFINITY_RT_BUDGET_NS);
		return;
	}
	step = div64_u64((INFINITY_RT_BUDGET_NS - ema) * delta_ns,
			 INFINITY_RT_TAU_NS);
	WRITE_ONCE(ctx->rt_ema, ema + step);
}
/* ------------------------------------------------------------------ */
/* infinity_rt_wakeup -- RT EMA decay on wakeup                         */
/* ------------------------------------------------------------------ */
void infinity_rt_wakeup(struct infinity_ctx *ctx, u64 sleep_ns)
{
	u64 periods, residual;

	if (sleep_ns == 0)
		return;
	periods = div64_u64_rem(sleep_ns, 160000000ULL, &residual);
	if (periods > 63) {
		WRITE_ONCE(ctx->rt_ema, 0);
	} else {
		u64 ema = READ_ONCE(ctx->rt_ema);

		ema >>= periods;
		if (residual && ema) {
			u64 fraction = div64_u64(residual *
				INFINITY_FP_ONE, 160000000ULL);
			u64 linear = (ema * fraction) >>
				INFINITY_FP_SHIFT;
			u64 quad = ((linear * fraction) >>
				INFINITY_FP_SHIFT) >> 1;
			if (linear > quad)
				ema -= min(ema, linear - quad);
		}
		WRITE_ONCE(ctx->rt_ema, ema);
	}
}
/* ------------------------------------------------------------------ */
/* infinity_rr_timeslice -- adaptive SCHED_RR timeslice                */
/* ------------------------------------------------------------------ */
unsigned int infinity_rr_timeslice(struct task_struct *p,
				   unsigned int rr_default)
{
	u64 rt_ema = READ_ONCE(p->infinity.rt_ema);
	u64 decay_pct;

	if (!rt_ema)
		return rr_default;
	decay_pct = div64_u64(rt_ema * 90ULL, INFINITY_RT_BUDGET_NS);
	if (decay_pct > 90)
		decay_pct = 90;
	return max(1U, (unsigned int)(rr_default * (100ULL - decay_pct)
				      / 100ULL));
}

/* ------------------------------------------------------------------ */
/* infinity_is_interactive_candidate -- sched-class gate for the DRM   */
/* scheduler's CPU<->GPU coupling (see include/drm/gpu_scheduler.h)    */
/* ------------------------------------------------------------------ */
bool infinity_is_interactive_candidate(struct task_struct *p)
{
	return p->sched_class == &fair_sched_class &&
	       !task_has_idle_policy(p);
}
EXPORT_SYMBOL_GPL(infinity_is_interactive_candidate);
