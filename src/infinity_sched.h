/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2026 Galih Tama <galpt@v.recipes>
 *
 * infinity_sched.h — Infinity scheduler API (v2 EMA).
 *
 * Architecture:
 *
 *   fair.c (EEVDF framework)           infinity_sched.c (Infinity algorithm)
 *   ────────────────────────           ─────────────────────────────────────
 *   update_deadline()       ──call──► infinity_slice()        — fair-share slice
 *   update_curr()           ──call──► infinity_consume()      — EMA budget consumption
 *   enqueue_task_fair()     ──call──► infinity_wakeup()       — EMA decay on wakeup
 *   dequeue_task_fair()     ──call──► (records last_sleep_ns) — sleep tracking
 *   task_fork_fair()        ──call──► infinity_fork_init()    — fork init
 *   init/init_task.c        ──init──► infinity.{}             — static init
 *
 * Tunables (sysctl kernel.infinity_*):
 *   infinity_carriage_ns   — base fair-share window (default 2ms)
 *   infinity_debt_cap      — max acceleration multiplier (default 256x)
 *   infinity_smt_divisor   — SMT secondary slice divisor (default 2)
 *   infinity_running       — read-only flag, 1 if active
 *
 * Self-stabilizing by construction: the EMA naturally converges between
 * 0 and BUDGET_MAX without any clamps or external feedback loop.
 */

#ifndef __INFINITY_SCHED_H
#define __INFINITY_SCHED_H

#include <linux/sched.h>

/* ------------------------------------------------------------------ */
/* Tunable defaults                                                    */
/* ------------------------------------------------------------------ */

/** Base fair-share window (2ms). */
#define INFINITY_CARRIAGE_NS_DEFAULT	2000000ULL

/** Runtime debt cap multiplier (256x). */
#define INFINITY_DEBT_CAP_DEFAULT	256

/** SMT secondary slice divisor (2 = half slice). */
#define INFINITY_SMT_DIVISOR_DEFAULT	2

/* ------------------------------------------------------------------ */
/* Clamp bounds                                                        */
/* ------------------------------------------------------------------ */

#define INFINITY_CARRIAGE_NS_MIN	1000ULL
#define INFINITY_CARRIAGE_NS_MAX	100000000ULL	/* 100ms */

#define INFINITY_DEBT_CAP_MIN		1
#define INFINITY_DEBT_CAP_MAX		4096

#define INFINITY_SMT_DIVISOR_MIN	1
#define INFINITY_SMT_DIVISOR_MAX	16

/* ------------------------------------------------------------------ */
/* Hard constants (not user-tunable)                                   */
/* ------------------------------------------------------------------ */

/** Minimum slice floor (500us). */
#define INFINITY_SLICE_MIN_NS		500000ULL

/** Maximum budget (2ms). */
#define INFINITY_BUDGET_MAX_NS		2000000ULL

/** EMA alpha: 1/16 of convergence per tick. */
#define INFINITY_EMA_ALPHA		16

/** Fixed-point shift for fractional precision (8 bits). */
#define INFINITY_FP_SHIFT		8
#define INFINITY_FP_ONE			(1 << INFINITY_FP_SHIFT)

/* ------------------------------------------------------------------ */
/* RT EMA constants                                                    */
/* ------------------------------------------------------------------ */

/** RT budget ceiling (10ms — larger than fair to give RT tasks runway). */
#define INFINITY_RT_BUDGET_NS		10000000ULL

/** RT alpha: slow climb (1/64 per wakeup). */
#define INFINITY_RT_ALPHA		4

/** RT beta: fast decay (1/4 per sleep). */
#define INFINITY_RT_BETA			64

/** Max priority decay. */
#define INFINITY_RT_PRIO_RANGE		30

/** Hard floor — RT never decays below this (MAX_RT_PRIO - 1 = 98). */
#define INFINITY_RT_PRIO_FLOOR		(MAX_RT_PRIO - 1)

/** Initial budget for newly forked tasks (one minimum slice). */
#define INFINITY_INIT_BUDGET_NS		INFINITY_SLICE_MIN_NS

/* ------------------------------------------------------------------ */
/* External sysctl tunables                                            */
/* ------------------------------------------------------------------ */

extern unsigned long infinity_tune_carriage_ns;
extern unsigned long infinity_tune_debt_cap;
extern unsigned long infinity_tune_smt_divisor;

/* ------------------------------------------------------------------ */
/* API — called from fair.c and rt.c                                   */
/* ------------------------------------------------------------------ */

u64 infinity_slice(unsigned long nr_runnable, bool on_smt_secondary);
void infinity_consume(struct infinity_ctx *ctx, u64 delta_ns);
void infinity_wakeup(struct infinity_ctx *ctx, u64 sleep_ns);
void infinity_fork_init(struct infinity_ctx *ctx, u64 now);

void infinity_rt_consume(struct infinity_ctx *ctx);
void infinity_rt_wakeup(struct infinity_ctx *ctx);
void infinity_rt_init(struct infinity_ctx *ctx);
u8   infinity_rt_effective_prio(u8 base_prio, struct infinity_ctx *ctx);

#endif /* __INFINITY_SCHED_H */
