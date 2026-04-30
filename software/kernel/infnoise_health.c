// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Health monitoring for Infinite Noise TRNG kernel driver
 *
 * Copyright (C) 2024 Manuel Domke
 *
 * Implements the two continuous health tests required by NIST SP 800-90B
 * Section 4.4 — Repetition Count Test (RCT) and Adaptive Proportion Test
 * (APT) — running on the bit stream emitted by the noise source before
 * any conditioning.
 *
 * Each test has two cutoffs:
 *
 *   Transient (α = 2^-30): latches @failed; the recovery path resets
 *                          state and re-arms the tests.
 *   Permanent (α = 2^-60): latches @dead; @dead survives reset, and
 *                          the recovery path refuses to re-arm.
 *
 * Crucially, a transient trip does *not* short-circuit the per-bit
 * loop — the counters keep climbing — so a genuinely stuck noise
 * source escalates from transient to permanent inside a single batch.
 */

#include <linux/kernel.h>
#include <linux/string.h>
#include "infnoise.h"

static void rct_trip_transient(struct infnoise_health *health)
{
	if (health->failed)
		return;
	pr_warn("infnoise: RCT transient: %u consecutive %u-bits\n",
		health->rct_count, health->rct_a ? 1 : 0);
	health->failed = true;
	health->transient_failures++;
}

static void rct_trip_permanent(struct infnoise_health *health)
{
	if (health->dead)
		return;
	pr_crit("infnoise: RCT permanent: %u consecutive %u-bits — device condemned\n",
		health->rct_count, health->rct_a ? 1 : 0);
	health->dead = true;
}

static void apt_trip_transient(struct infnoise_health *health)
{
	if (health->failed)
		return;
	pr_warn("infnoise: APT transient: %u/%u matches in window\n",
		health->apt_count, health->apt_pos + 1);
	health->failed = true;
	health->transient_failures++;
}

static void apt_trip_permanent(struct infnoise_health *health)
{
	if (health->dead)
		return;
	pr_crit("infnoise: APT permanent: %u/%u matches in window — device condemned\n",
		health->apt_count, health->apt_pos + 1);
	health->dead = true;
}

/**
 * infnoise_health_init - Initialize health check state
 * @health: Health check state structure
 *
 * Zeroes counters and arms both tests. Always succeeds; no allocations.
 */
void infnoise_health_init(struct infnoise_health *health)
{
	memset(health, 0, sizeof(*health));
}

/**
 * infnoise_health_reset - Re-arm the transient tests after recovery
 * @health: Health check state structure
 *
 * Clears per-sample state so RCT/APT restart from scratch and drops
 * the transient @failed latch. The permanent @dead latch is sticky:
 * once set, this function leaves it alone and returns without touching
 * any other field — there is no recovery from a permanent failure.
 *
 * Cumulative statistics (total_bits, total_ones, total_zeros,
 * transient_failures) are preserved across resets.
 */
void infnoise_health_reset(struct infnoise_health *health)
{
	if (health->dead)
		return;

	health->rct_count = 0;
	health->rct_a = false;
	health->apt_count = 0;
	health->apt_pos = 0;
	health->apt_a = false;
	health->warmup_complete = false;
	health->failed = false;
}

/**
 * infnoise_health_add_bit - Process one sample from the noise source
 * @health: Health check state structure
 * @bit:    The sample value (0 or 1)
 *
 * Updates RCT and APT counters. Returns @false only on permanent
 * failure (caller should give up on the device); transient failures
 * are signalled via @health->failed, which the caller is expected to
 * inspect after the batch completes.
 */
bool infnoise_health_add_bit(struct infnoise_health *health, bool bit)
{
	if (health->dead)
		return false;

	health->total_bits++;
	if (bit)
		health->total_ones++;
	else
		health->total_zeros++;

	/* ---- Repetition Count Test (SP 800-90B 4.4.1) ----------------- */
	if (health->rct_count == 0 || bit != health->rct_a) {
		health->rct_a = bit;
		health->rct_count = 1;
	} else {
		health->rct_count++;
		if (health->rct_count >= INM_RCT_PERM_CUTOFF) {
			rct_trip_permanent(health);
			return false;
		}
		if (health->rct_count >= INM_RCT_TRANS_CUTOFF)
			rct_trip_transient(health);
	}

	/* ---- Adaptive Proportion Test (SP 800-90B 4.4.2) -------------- */
	if (health->apt_pos == 0) {
		/* Start of a new window: this sample is the reference. */
		health->apt_a = bit;
		health->apt_count = 1;
	} else if (bit == health->apt_a) {
		health->apt_count++;
		if (health->apt_count >= INM_APT_PERM_CUTOFF) {
			apt_trip_permanent(health);
			return false;
		}
		if (health->apt_count >= INM_APT_TRANS_CUTOFF)
			apt_trip_transient(health);
	}

	health->apt_pos++;
	if (health->apt_pos >= INM_APT_WINDOW) {
		health->apt_pos = 0;
		/*
		 * Mark warmup done only on a clean window. If the window
		 * tripped transient, we leave warmup_complete alone — the
		 * recovery path will reset and try again from scratch.
		 */
		if (!health->failed && !health->dead)
			health->warmup_complete = true;
	}

	return true;
}

/**
 * infnoise_health_ok - Check if health status is acceptable
 * @health: Health check state structure
 *
 * Returns true once the first APT window has completed cleanly and
 * neither test has tripped since the most recent reset.
 */
bool infnoise_health_ok(struct infnoise_health *health)
{
	return health->warmup_complete && !health->failed && !health->dead;
}

/**
 * infnoise_health_get_stats - Get health statistics
 * @health: Health check state structure
 * @stats:  Output statistics structure
 *
 * Fills the UAPI statistics structure. Per-bit entropy is reported as
 * the design constant since RCT/APT do not estimate entropy at runtime.
 */
void infnoise_health_get_stats(struct infnoise_health *health,
			       struct infnoise_stats *stats)
{
	memset(stats, 0, sizeof(*stats));
	stats->total_bits = health->total_bits;
	stats->total_ones = health->total_ones;
	stats->total_zeros = health->total_zeros;
	stats->entropy_estimate = INM_REPORTED_ENTROPY_FP;
	stats->k_estimate = INM_REPORTED_K_FP;
	stats->warmup_complete = health->warmup_complete;
	stats->health_ok = infnoise_health_ok(health);
}
