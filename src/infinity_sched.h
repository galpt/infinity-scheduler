/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2026 Galih Tama <galpt@v.recipes>
 *
 * infinity_sched.h -- Infinity scheduler API (v4.8-gpu).
 *
 * Architecture:
 *
 *   fair.c (Linux scheduler)         infinity_sched.c (Infinity algorithm)
 *   ────────────────   ──────────────
 *   update_deadline()        ──call──► infinity_update_weight() -- EMA weight
 *   update_curr()            ──call──► infinity_consume()       -- EMA budget
 *   enqueue_task_fair()      ──call──► infinity_wakeup()        -- EMA decay
 *   place_entity()   ──check──► futex/ipc_waiting -- halve vslice on wakeup
 *   dequeue_task_fair()      ──call──► (records last_sleep_ns)  -- sleep tracking
 *   update_curr_rt()         ──call──► infinity_rt_consume()    -- RT EMA climb
 *   enqueue_task_rt()        ──call──► infinity_rt_wakeup()     -- RT EMA decay
 *   dequeue_task_rt()        ──call──► (records rt_last_sleep_ns)
 *   task_tick_rt()           ──call──► infinity_rr_timeslice()  -- adaptive RR slice
 *   sched_fork()             ──call──► infinity_fork_init()     -- fork init
 *   init/init_task.c         ──init──► infinity.{}              -- static init
 *
 * Weight-based modulation: the task's EEVDF weight is modulated by EMA.
 * EEVDF natively computes a shorter slice and later deadline from a lower
 * weight -- no second level of fairness logic needed.
 *
 * The base weight is always derived from the task's static priority (nice),
 * never from the live weight, so the modulation is idempotent and the nice
 * value is always honoured.
 *
 * Tunables:
 *   kernel.infinity_smt_divisor   -- SMT secondary slice divisor (default 2)
 *   kernel.infinity_running       -- read-only flag, 1 if active
 *   kernel.infinity_version       -- read-only branch version string
 *   kernel.infinity_stats         -- read-only CPU/GPU accounting table
 *
 * Self-stabilizing by construction: the EMA converges toward BUDGET_MAX
 * while running and is shifted down by sleep decay; the climb step is
 * clamped at the ceiling, so the EMA always stays within [0, BUDGET_MAX].
 * Higher EMA → lower effective weight → later deadline.
 */
#ifndef __INFINITY_SCHED_H
#define __INFINITY_SCHED_H
#include <linux/math64.h>
#include <linux/sched.h>
/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */
/** Maximum budget ceiling (6ms). */
#define INFINITY_BUDGET_MAX_NS		6000000ULL
/**
 * Documentational EMA time constant (the live alpha is hardware-adaptive,
 * computed from cpu_capacity in infinity_consume, 2048-4096):
 * step = (BUDGET_MAX - ema) × runtime × ALPHA / (...)
 * α = 3072 gives ~500µs continuous runtime to reach full EMA penalty.
 */
#define INFINITY_EMA_ALPHA		3072
/** Fixed-point shift for fractional precision (8 bits). */
#define INFINITY_FP_SHIFT		8
#define INFINITY_FP_ONE			(1 << INFINITY_FP_SHIFT)
/** IPC boost gradient: full 2x below 1ms sleep, linear falloff to 1x at 8ms. */
#define INFINITY_IPC_GRADIENT_FULL_NS	1000000ULL
#define INFINITY_IPC_GRADIENT_MAX_NS	8000000ULL
/** IPC boost rate limit: at most one boost per task per interval, so
 *  per-packet wakeup storms cannot churn the runqueue.
 */
#define INFINITY_IPC_RATE_LIMIT_NS	2000000ULL

/**
 * infinity_ipc_gradient -- IPC-wakeup vslice reduction, by sleep duration.
 * @sleep_ns:  Time the task slept (ns), 0 if unknown.
 *
 * Full 2x boost (red = FP_ONE/2) at sleep <= 1ms, linear falloff to no
 * boost (red = 0) at >= 8ms.  div64_u64 truncates down, so the result
 * is monotone non-increasing in sleep_ns -- no boost oscillation.
 *
 * Return: Reduction [0, FP_ONE/2] in fixed-point units.
 */
static inline u64 infinity_ipc_gradient(u64 sleep_ns)
{
	u64 span = INFINITY_IPC_GRADIENT_MAX_NS - INFINITY_IPC_GRADIENT_FULL_NS;

	if (sleep_ns <= INFINITY_IPC_GRADIENT_FULL_NS)
		return INFINITY_FP_ONE / 2;
	if (sleep_ns >= INFINITY_IPC_GRADIENT_MAX_NS)
		return 0;
	return div64_u64((span - (sleep_ns - INFINITY_IPC_GRADIENT_FULL_NS)) *
			 INFINITY_FP_ONE / 2, span);
}
/**
 * Weight reduction slope: effective = base × (100 - pct × 98/100) / 100.
 * At EMA=100%, denom = 2, weight = base × 2/100 = base × 2% (50× reduction).
 * With 32 storm threads at 2% = 640 < 1 interactive thread at 1024.
 */
#define INFINITY_WEIGHT_SLOPE_NUM	98
#define INFINITY_WEIGHT_SLOPE_DEN	100
/* ------------------------------------------------------------------ */
/* SMT divisor bounds                                                  */
/* ------------------------------------------------------------------ */
#define INFINITY_SMT_DIVISOR_DEFAULT	2
#define INFINITY_SMT_DIVISOR_MIN	1
#define INFINITY_SMT_DIVISOR_MAX	16
/* ------------------------------------------------------------------ */
/* Weight calculation from EMA                                          */
/* ------------------------------------------------------------------ */
/**
 * infinity_calc_weight -- Compute EMA-modulated EEVDF weight.
 * @p:    Task whose weight to compute.
 * @ema:  Raw EMA (clamped to BUDGET_MAX).
 *
 * The base weight comes from @p's static priority via sched_prio_to_weight[].
 * This is the nominal nice-derived weight, not the live se.load.weight,
 * so the modulation is idempotent and the nice value is always honoured.
 *
 *   effective = base × (100 - pct × 98/100) / 100
 *   at EMA=100%: denom = 2, weight = base × 2%
 *
 * Tasks with uclamp_min > 0 are bypassed (return their base weight).
 *
 * Return: Effective weight for EEVDF.
 */
static inline u32 infinity_calc_weight(struct task_struct *p, u64 ema)
{
	/*
	 * SCHED_IDLE tasks have their own weight (WEIGHT_IDLEPRIO = 3)
	 * that we must not override -- they are designed to yield to
	 * everything else by construction.
	 */
	if (task_has_idle_policy(p))
		return scale_load(WEIGHT_IDLEPRIO);

#ifdef CONFIG_UCLAMP_TASK
	if (p->uclamp_req[UCLAMP_MIN].value > 0)
		return scale_load(sched_prio_to_weight[p->static_prio - MAX_RT_PRIO]);
#endif

	int idx = p->static_prio - MAX_RT_PRIO;
	u32 base = scale_load(sched_prio_to_weight[idx]);

	if (ema > INFINITY_BUDGET_MAX_NS)
		ema = INFINITY_BUDGET_MAX_NS;

	if (ema) {
		u64 pct = ema * 100ULL / INFINITY_BUDGET_MAX_NS;
		u64 denom = 100ULL - pct * INFINITY_WEIGHT_SLOPE_NUM /
				      INFINITY_WEIGHT_SLOPE_DEN;
		if (denom < 2ULL)
			denom = 2ULL;
		return (u32)max(1ULL, base * denom / 100ULL);
	}
	return base;
}
/* ------------------------------------------------------------------ */
/* Cgroup EMA constants                                                */
/* ------------------------------------------------------------------ */
/** Cgroup aggregate EMA ceiling (2ms -- groups converge faster). */
#define INFINITY_CGROUP_EMA_CLIMB_NS	2000000ULL
/** Cgroup EMA alpha -- gentle slope to avoid oscillation. */
#define INFINITY_CGROUP_EMA_ALPHA	1
/** Cgroup EMA half-life for idle decay (16ms). */
#define INFINITY_CGROUP_EMA_HALFLIFE_NS	16000000ULL
/** Shield v2: engage at 40% aggregate EMA, linear to 50% reduction at 100%,
 *  quantized to 5pp steps (at most one reweight per bucket crossing).
 */
#define INFINITY_SHIELD_ENGAGE_PCT		40
#define INFINITY_SHIELD_MAX_REDUCE_PCT		50
#define INFINITY_SHIELD_STEP_PCT		5
/** Cross-CPU max recompute window (ms) -- half the 16ms empty-rq half-life. */
#define INFINITY_SHIELD_RESCAN_MS		8
#define INFINITY_SHIELD_ENGAGE_THRESHOLD_NS \
	(INFINITY_CGROUP_EMA_CLIMB_NS * INFINITY_SHIELD_ENGAGE_PCT / 100ULL)
/* ------------------------------------------------------------------ */
/* RT EMA constants                                                    */
/* ------------------------------------------------------------------ */
/** RT budget ceiling (10ms). */
#define INFINITY_RT_BUDGET_NS		10000000ULL
/** RT alpha. */
#define INFINITY_RT_ALPHA		4
/** RT EMA time constant: one tau of accumulated runtime closes the entire remaining gap (640ms). */
#define INFINITY_RT_TAU_NS (INFINITY_RT_BUDGET_NS * INFINITY_FP_ONE / INFINITY_RT_ALPHA)
/* ------------------------------------------------------------------ */
/* RT safety valve -- requeue throttle threshold                        */
/* ------------------------------------------------------------------ */
/**
 * rt_ema threshold: force rogue SCHED_FIFO to yield when rt_ema exceeds
 * 95% of INFINITY_RT_BUDGET_NS (10ms).  Expressed as a computed percentage
 * of the budget to prevent unit mismatch errors.
 */
#define INFINITY_RT_DEMOTE_THRESHOLD    (INFINITY_RT_BUDGET_NS * 95ULL / 100ULL)
/** RT valve: release (re-arm) threshold -- 10pp hysteresis band. */
#define INFINITY_RT_REARM_THRESHOLD	(INFINITY_RT_BUDGET_NS * 85ULL / 100ULL)
/** RT valve: min interval between forced requeues while engaged (ms). */
#define INFINITY_RT_REQUEUE_MS		5
/** RT valve: synthetic sleep applied to rt_ema on each forced requeue
 *  (100ms => 57% of the current EMA remains: one requeue drops a
 *  100%-burner from 95-100% to ~54-57%, below the 85% re-arm point).
 */
#define INFINITY_RT_REQUEUE_DECAY_NS	100000000ULL

/* ------------------------------------------------------------------ */
/* PELT divergence diagnostic                                          */
/* ------------------------------------------------------------------ */
/** PELT divergence diagnostic: |ema_pct - util_pct| threshold (pp). */
#define INFINITY_DIVERGENCE_THRESHOLD		50
#define INFINITY_DIVERGENCE_THRESHOLD_UNITS \
	(INFINITY_DIVERGENCE_THRESHOLD * SCHED_CAPACITY_SCALE / 100)
/** PELT divergence diagnostic: consecutive divergent sleeps before flagging. */
#define INFINITY_DIVERGENCE_STREAK		3

/* ------------------------------------------------------------------ */
/* External sysctl tunables                                            */
/* ------------------------------------------------------------------ */
extern unsigned long infinity_tune_smt_divisor;
/* ------------------------------------------------------------------ */
/* Stats counters                                                      */
/* ------------------------------------------------------------------ */
DECLARE_PER_CPU(atomic64_t, infinity_futex_boost_count);
DECLARE_PER_CPU(atomic64_t, infinity_ipc_boost_count);
DECLARE_PER_CPU(atomic64_t, infinity_ipc_wakeup_count);
DECLARE_PER_CPU(atomic64_t, infinity_ema_climb_count);
DECLARE_PER_CPU(atomic64_t, infinity_wakeup_count);
DECLARE_PER_CPU(atomic64_t, infinity_rt_throttle_count);
DECLARE_PER_CPU(atomic64_t, infinity_gpu_passover_boosts);
DECLARE_PER_CPU(atomic64_t, infinity_gpu_idle_compensations);
DECLARE_PER_CPU(atomic64_t, infinity_gpu_cpu_coupling_activations);
DECLARE_PER_CPU(atomic64_t, infinity_gpu_lock_drain_rounds);
DECLARE_PER_CPU(atomic64_t, infinity_cpufreq_interactive_count);
DECLARE_PER_CPU(atomic64_t, infinity_smt_interactive_count);
DECLARE_PER_CPU(atomic64_t, infinity_shield_engage_count);
DECLARE_PER_CPU(atomic64_t, infinity_divergence_count);
/* ------------------------------------------------------------------ */
/* API -- called from fair.c and rt.c                                   */
/* ------------------------------------------------------------------ */
void infinity_consume(struct infinity_ctx *ctx, u64 delta_ns, unsigned long cpu_capacity);
void infinity_wakeup(struct infinity_ctx *ctx, u64 sleep_ns);
void infinity_fork_init(struct infinity_ctx *ctx, u64 now);
void infinity_exec_reset(struct infinity_ctx *ctx);
void infinity_rt_consume(struct infinity_ctx *ctx, u64 delta_ns);
void infinity_rt_wakeup(struct infinity_ctx *ctx, u64 sleep_ns);
unsigned int infinity_rr_timeslice(struct task_struct *p,
				   unsigned int rr_default);
#endif /* __INFINITY_SCHED_H */
