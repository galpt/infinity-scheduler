// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2026 Galih Tama <galpt@v.recipes>
 *
 * infinity_sched.c -- Infinity scheduler algorithm (v4.7-gpu).
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
/* Sysctl tunables                                                     */
/* ------------------------------------------------------------------ */
unsigned long infinity_tune_smt_divisor = INFINITY_SMT_DIVISOR_DEFAULT;
static int infinity_running_flag = 1;
static const char infinity_version[] = "v4.7-gpu";
static int clamp_smt_divisor(const struct ctl_table *table, int write,
			     void *buf, size_t *lenp, loff_t *ppos)
{
	unsigned long old;
	int ret;

	old = READ_ONCE(infinity_tune_smt_divisor);
	ret = proc_doulongvec_minmax(table, write, buf, lenp, ppos);
	if (write && ret == 0) {
		unsigned long val;

		val = READ_ONCE(infinity_tune_smt_divisor);
		val = clamp(val, INFINITY_SMT_DIVISOR_MIN, INFINITY_SMT_DIVISOR_MAX);
		if (val != old)
			pr_info("Infinity: smt_divisor %lu -> %lu\n", old, val);
		WRITE_ONCE(infinity_tune_smt_divisor, val);
	}
	return ret;
}
/* ------------------------------------------------------------------ */
/* Human-readable number formatting                                    */
/* ------------------------------------------------------------------ */
static __maybe_unused void fmt_human(char *buf, size_t sz, u64 val)
{
	static const struct {
		u64 divisor;
		const char *unit;
	} table[] = {
		{ 1000000000000000000ULL, "E" },
		{ 1000000000000000ULL,    "P" },
		{ 1000000000000ULL,       "T" },
		{ 1000000000ULL,          "B" },
		{ 1000000ULL,             "M" },
		{ 1000ULL,                "K" },
	};
	int i;
	char tmp[32];

	for (i = 0; i < ARRAY_SIZE(table); i++) {
		if (val >= table[i].divisor) {
			u64 whole = val / table[i].divisor;
			u64 frac = (val % table[i].divisor) /
				   (table[i].divisor / 100);
			scnprintf(tmp, sizeof(tmp), "%llu.%02llu %s",
				  whole, frac, table[i].unit);
			strlcat(buf, tmp, sz);
			return;
		}
	}

	scnprintf(tmp, sizeof(tmp), "%llu", (unsigned long long)val);
	strlcat(buf, tmp, sz);
}
/* ------------------------------------------------------------------ */
/* fmt_val -- Write val to buf with comma separators, right-aligned    *
 * to @width.  If @width is 0, no padding is applied.
 */
/* ------------------------------------------------------------------ */
static __maybe_unused void fmt_val(char *buf, size_t sz, u64 val, unsigned int width)
{
	char tmp[32];
	unsigned int len;

	if (val == 0) {
		scnprintf(tmp, sizeof(tmp), "0");
	} else {
		char raw[24];
		int raw_len, i, pos = 0;

		scnprintf(raw, sizeof(raw), "%llu", (unsigned long long)val);
		raw_len = strlen(raw);

		for (i = 0; i < raw_len; i++) {
			if (i > 0 && (raw_len - i) % 3 == 0)
				tmp[pos++] = ',';
			tmp[pos++] = raw[i];
		}
		tmp[pos] = '\0';
	}

	len = strlen(tmp);
	if (width > 0 && len < width) {
		char padded[32];

		scnprintf(padded, sizeof(padded), "%*s", width, tmp);
		strlcat(buf, padded, sz);
	} else {
		strlcat(buf, tmp, sz);
	}
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

static int infinity_stats_proc_handler(const struct ctl_table *ctl, int write,
				       void *buffer, size_t *lenp,
				       loff_t *ppos)
{
	char *buf;
	u64 fbc, emc, wkc, rtc, gcb, gapp, gskp;
	u64 gic, gcca, gpbo, gldr, icf, ismt;
	char v1[16];
	const size_t bufsz = 4096;

	/*
	 * Table layout (printf-style, auto-aligned):
	 *
	 *   | %-22s | %13s | %-30s |
	 *   +------------------------+---------------+--------------------------------+
	 */
	char *SEP = "+------------------------+---------------+--------------------------------+";
	const char *ROW = "| %-22s | %13s | %-30s |\n";

	if (write)
		return -EROFS;

	buf = kmalloc(bufsz, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

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

	buf[0] = '\0';
	scnprintf(buf + strlen(buf), bufsz - strlen(buf),
		  "Infinity Scheduler %s\n\n", infinity_version);

	/* ---- CPU ---- */
	strlcat(buf, "CPU\n", bufsz);
	strlcat(buf, SEP, bufsz);
	strlcat(buf, "\n", bufsz);

	if (emc) {
		char pct[16];

		scnprintf(pct, sizeof(pct), "%llu%% of tasks",
			  mul_u64_u32_div(fbc, 100, emc));
		scnprintf(buf + strlen(buf), bufsz - strlen(buf), ROW,
			  "Futex boosts",
			  fill_pretty_llu(v1, sizeof(v1), fbc),
			  pct);
	} else {
		scnprintf(buf + strlen(buf), bufsz - strlen(buf), ROW,
			  "Futex boosts",
			  fill_pretty_llu(v1, sizeof(v1), fbc),
			  "N/A");
	}

	{
		char avg[32];
		u64 avg_wakeup = mul_u64_u32_div(emc, 100, max(wkc, 1ULL));

		scnprintf(avg, sizeof(avg), "~%llu.%02llu/wakeup",
			  avg_wakeup / 100, avg_wakeup % 100);
		scnprintf(buf + strlen(buf), bufsz - strlen(buf), ROW,
			  "EMA climbs",
			  fill_pretty_llu(v1, sizeof(v1), emc), avg);
	}

	scnprintf(buf + strlen(buf), bufsz - strlen(buf), ROW,
		  "Wakeup decays",
		  fill_pretty_llu(v1, sizeof(v1), wkc), "");

	scnprintf(buf + strlen(buf), bufsz - strlen(buf), ROW,
		  "Interactive cpufreq",
		  fill_pretty_llu(v1, sizeof(v1), icf),
		  "frequency ramp events");

	scnprintf(buf + strlen(buf), bufsz - strlen(buf), ROW,
		  "SMT placement",
		  fill_pretty_llu(v1, sizeof(v1), ismt),
		  "interactive moves to idle core");



	strlcat(buf, SEP, bufsz);
	strlcat(buf, "\n\n", bufsz);

	/* ---- RT ---- */
	strlcat(buf, "RT\n", bufsz);
	strlcat(buf, SEP, bufsz);
	strlcat(buf, "\n", bufsz);

	scnprintf(buf + strlen(buf), bufsz - strlen(buf), ROW,
		  "RT throttles",
		  fill_pretty_llu(v1, sizeof(v1), rtc),
		  "FIFO rogue demotions");

	strlcat(buf, SEP, bufsz);
	strlcat(buf, "\n\n", bufsz);

	/* ---- GPU ---- */
	strlcat(buf, "GPU\n", bufsz);
	strlcat(buf, SEP, bufsz);
	strlcat(buf, "\n", bufsz);

	scnprintf(buf + strlen(buf), bufsz - strlen(buf), ROW,
		  "Completion entered",
		  fill_pretty_llu(v1, sizeof(v1), gcb),
		  "fence callbacks fired");

	if (gcb) {
		u64 whole = mul_u64_u32_div(gapp, 100, gcb);
		u64 frac  = mul_u64_u32_div(gapp, 10000, gcb) % 100;
		char v2[16];

		scnprintf(v2, sizeof(v2), "%llu.%02llu%%", whole, frac);
		scnprintf(buf + strlen(buf), bufsz - strlen(buf), ROW,
			  "Accounting applied",
			  fill_pretty_llu(v1, sizeof(v1), gapp),
			  v2);
	} else {
		scnprintf(buf + strlen(buf), bufsz - strlen(buf), ROW,
			  "Accounting applied",
			  fill_pretty_llu(v1, sizeof(v1), gapp),
			  "N/A");
	}

	scnprintf(buf + strlen(buf), bufsz - strlen(buf), ROW,
		  "  >> lock contention",
		  fill_pretty_llu(v1, sizeof(v1), 0),
		  "(lock contention)");

	scnprintf(buf + strlen(buf), bufsz - strlen(buf), ROW,
		  "  >> entity not found",
		  fill_pretty_llu(v1, sizeof(v1), gskp),
		  "(entity_kill race)");

	scnprintf(buf + strlen(buf), bufsz - strlen(buf), ROW,
		  "Idle compensation",
		  fill_pretty_llu(v1, sizeof(v1), gic),
		  "proportional idle boost");

	scnprintf(buf + strlen(buf), bufsz - strlen(buf), ROW,
		  "CPU->GPU coupling",
		  fill_pretty_llu(v1, sizeof(v1), gcca),
		  "interactive vtime reduction");

	scnprintf(buf + strlen(buf), bufsz - strlen(buf), ROW,
		  "GPU->CPU coupling",
		  fill_pretty_llu(v1, sizeof(v1), gpbo),
		  "passover EMA boost");

	scnprintf(buf + strlen(buf), bufsz - strlen(buf), ROW,
		  "Drain count",
		  fill_pretty_llu(v1, sizeof(v1), gldr),
		  "batch drain operations");

	strlcat(buf, SEP, bufsz);
	strlcat(buf, "\n\n", bufsz);

	/* Footer */
	if (gcb) {
		u64 whole = mul_u64_u32_div(gapp, 100, gcb);
		u64 frac  = mul_u64_u32_div(gapp, 10000, gcb) % 100;
		char w1[16], w2[16];

		fill_pretty_llu(w1, sizeof(w1), gapp);
		fill_pretty_llu(w2, sizeof(w2), gcb);
		scnprintf(buf + strlen(buf), bufsz - strlen(buf),
			  "Accounting confidence: %llu.%02llu%%  (%s / %s completions)\n",
			  whole, frac, w1, w2);
	} else {
		strlcat(buf,
			"Accounting confidence: N/A  (no GPU jobs tracked)\n",
			bufsz);
	}

	if (!gcb)
		strlcat(buf, "Verdict: No GPU jobs recorded yet\n",
			bufsz);
	else if (gapp >= gcb)
		strlcat(buf,
			"Verdict: All counters healthy -- system operating normally\n",
			bufsz);
	else
		strlcat(buf,
			"Verdict: Accounting mismatch detected -- see above for details\n",
			bufsz);

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
static const struct ctl_table infinity_sysctl_table[] = {
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
	 * Each passover is worth one extra half-life of decay.
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
	u64 step;

	if (unlikely(ctx->rt_ema >= INFINITY_RT_BUDGET_NS)) {
		ctx->rt_ema = INFINITY_RT_BUDGET_NS;
		return;
	}
	/* Clamp delta_ns to prevent u64 overflow in the numerator for
	 * tickless (NO_HZ_FULL) configurations where delta_ns can span
	 * hundreds of seconds between calls.  Matches the same clamp
	 * used in infinity_consume for the Fair class.
	 */
	if (delta_ns > INFINITY_RT_BUDGET_NS)
		delta_ns = INFINITY_RT_BUDGET_NS;
	step = div64_u64((INFINITY_RT_BUDGET_NS - ctx->rt_ema) * delta_ns *
			   INFINITY_RT_ALPHA,
			   INFINITY_RT_BUDGET_NS * INFINITY_FP_ONE);
	ctx->rt_ema += step;
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
/* infinity_rr_timeslice -- adaptive SCHED_RR timeslice                */
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
