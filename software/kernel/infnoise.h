/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Infinite Noise TRNG kernel driver
 *
 * Copyright (C) 2024 Manuel Domke
 *
 * Based on the userspace driver by Bill Cox
 * Hardware design by Bill Cox
 */

#ifndef INFNOISE_H
#define INFNOISE_H

#include <linux/types.h>
#include <linux/usb.h>
#include <linux/hw_random.h>
#include <linux/mutex.h>
#include <linux/completion.h>
#include <linux/workqueue.h>
#include <linux/kref.h>
#include <crypto/hash.h>

/* USB device IDs - Infinite Noise TRNG (pid.codes registered) */
#define INFNOISE_VENDOR_ID	0x1209	/* pid.codes VID */
#define INFNOISE_PRODUCT_ID	0x3701	/* Infinite Noise PID */

/* FTDI commands (SIO requests) */
#define FTDI_SIO_RESET			0x00
#define FTDI_SIO_SET_MODEM_CTRL		0x01
#define FTDI_SIO_SET_FLOW_CTRL		0x02
#define FTDI_SIO_SET_BAUDRATE		0x03
#define FTDI_SIO_SET_DATA		0x04
#define FTDI_SIO_GET_MODEM_STATUS	0x05
#define FTDI_SIO_SET_EVENT_CHAR		0x06
#define FTDI_SIO_SET_ERROR_CHAR		0x07
#define FTDI_SIO_SET_LATENCY_TIMER	0x09
#define FTDI_SIO_GET_LATENCY_TIMER	0x0A
#define FTDI_SIO_SET_BITMODE		0x0B
#define FTDI_SIO_READ_PINS		0x0C
#define FTDI_SIO_READ_EEPROM		0x90
#define FTDI_SIO_WRITE_EEPROM		0x91

/* FTDI reset commands */
#define FTDI_SIO_RESET_SIO		0x00
#define FTDI_SIO_RESET_PURGE_RX		0x01
#define FTDI_SIO_RESET_PURGE_TX		0x02

/* FTDI bit-bang modes */
#define FTDI_BITMODE_RESET		0x00
#define FTDI_BITMODE_BITBANG		0x01
#define FTDI_BITMODE_MPSSE		0x02
#define FTDI_BITMODE_SYNCBB		0x04	/* Synchronous bit-bang */
#define FTDI_BITMODE_MCU		0x08
#define FTDI_BITMODE_OPTO		0x10
#define FTDI_BITMODE_CBUS		0x20
#define FTDI_BITMODE_SYNCFF		0x40

/* Pin definitions for FT240X */
#define INFNOISE_COMP1		1	/* Comparator 1 input */
#define INFNOISE_COMP2		4	/* Comparator 2 input */
#define INFNOISE_SWEN1		2	/* Switch enable 1 output */
#define INFNOISE_SWEN2		0	/* Switch enable 2 output */

/* Address/debug pins */
#define INFNOISE_ADDR0		3
#define INFNOISE_ADDR1		5
#define INFNOISE_ADDR2		6
#define INFNOISE_ADDR3		7

/* Output mask: all pins except COMP1 and COMP2 are outputs */
#define INFNOISE_MASK		(0xFF & ~(1 << INFNOISE_COMP1) & ~(1 << INFNOISE_COMP2))

/* FTDI interface index (1-based: interface 0 uses index 1) */
#define FTDI_INDEX_INTERFACE_A	1

/* Buffer sizes */
#define INFNOISE_BUFLEN		512	/* FT240X buffer size, must be multiple of 64 */
#define INFNOISE_BYTES_OUT	(INFNOISE_BUFLEN / 8)	/* 64 bytes extracted */

/* FTDI packet format: each 64-byte USB packet has 2 status bytes + 62 data bytes */
#define FTDI_PACKET_SIZE	64
#define FTDI_STATUS_SIZE	2
#define FTDI_DATA_PER_PACKET	(FTDI_PACKET_SIZE - FTDI_STATUS_SIZE)  /* 62 */
/* For 512 bytes of data, we need ceil(512/62) = 9 packets = 576 bytes */
#define INFNOISE_USB_READ_SIZE	(((INFNOISE_BUFLEN + FTDI_DATA_PER_PACKET - 1) / FTDI_DATA_PER_PACKET) * FTDI_PACKET_SIZE)

/*
 * NIST SP 800-90B health tests (Section 4.4)
 *
 * Both tests run continuously on the bit stream extracted from the noise
 * source (before conditioning). Two thresholds per test:
 *
 *   Transient  (α = 2^-30) — latches INFNOISE_HEALTH_FAIL; the current
 *                            batch is dropped and the recovery path
 *                            re-arms the tests.
 *
 *   Permanent  (α = 2^-60) — latches INFNOISE_HEALTH_DEAD; the device
 *                            is condemned and recovery is refused.
 *
 * The permanent test is reachable only because, after a transient trip,
 * the health module keeps counting bits within the same batch instead
 * of bailing out — so a genuinely stuck noise source escalates from
 * transient to permanent inside a single transfer.
 *
 * H = 0.88 bits/bit (design target for the Infinite Noise multiplier).
 */

/*
 * Repetition Count Test (4.4.1)
 *
 *   C = 1 + ⌈-log2(α) / H⌉
 *
 * Transient: 1 + ⌈30 / 0.88⌉ = 1 + 35 = 36
 * Permanent: 1 + ⌈60 / 0.88⌉ = 1 + 69 = 70
 */
#define INM_RCT_TRANS_CUTOFF	36
#define INM_RCT_PERM_CUTOFF	70

/*
 * Adaptive Proportion Test (4.4.2) — binary form, W = 1024.
 *
 * Critical value derived from BinomCDF^-1 with p = 2^-H = 2^-0.88 ≈ 0.5436.
 * Normal approximation with continuity correction:
 *
 *   μ = W·p ≈ 556.61
 *   σ = √(W·p·(1-p)) ≈ 15.94
 *   C ≈ ⌈μ + z(α)·σ + 0.5⌉
 *
 * Transient: z(2^-30) ≈ 6.00  ⇒  C ≈ 653 → 656 (+3 margin)
 * Permanent: z(2^-60) ≈ 8.78  ⇒  C ≈ 697 → 700 (+3 margin)
 */
#define INM_APT_WINDOW		1024
#define INM_APT_TRANS_CUTOFF	656
#define INM_APT_PERM_CUTOFF	700

/*
 * Stats fields are kept for UAPI compatibility but are no longer estimated
 * at runtime; report the design constants in 16.16 fixed-point.
 */
#define INM_REPORTED_ENTROPY_FP	57671	/* 0.88 * 65536 */
#define INM_REPORTED_K_FP	120586	/* 1.84 * 65536 */

/*
 * SHA3-256 conditioning state.
 *
 * Each refill cycle the driver consumes oversample_factor full USB
 * transfers worth of extracted bytes E (8 source samples per byte) and
 * computes:
 *
 *   O  = SHA3-256(S || 0x11 || E)   ← user-visible output (32 bytes)
 *   S' = SHA3-256(S || 0x00 || E)   ← next internal state (32 bytes)
 *
 * S starts as all zero. Output and next-state derivations both use the
 * *current* S, so an attacker who learns O cannot derive S' without
 * inverting the hash, and vice versa (forward security).
 */
#define INFNOISE_SHA3_OUTPUT_SIZE	32

/* Domain separators between the two H(S || ds || E) derivations. */
#define INFNOISE_DS_STATE	0x00	/* derive next state S' */
#define INFNOISE_DS_OUTPUT	0x11	/* derive output O */

/*
 * Bit-extraction recap. The hardware delivers INFNOISE_BUFLEN = 512 raw
 * USB bytes per transfer. infnoise_extract_bytes() reads exactly one
 * comparator bit per raw byte (alternating COMP1/COMP2 by phase) — so
 * per transfer we collect 512 independent source samples. Those bits
 * are then packed eight-to-a-byte into INFNOISE_BYTES_OUT = 64 output
 * bytes (extracted, *not* raw). Every extracted byte therefore carries
 * 8 source samples × H bits of min-entropy.
 *
 *   1 transfer  → 512 source samples → 64 extracted bytes
 *   At H = 0.88: 64 extracted bytes ≈ 450 bits of min-entropy
 *
 * NIST full-entropy condition (SP 800-90B §3.1.5.1.2 / SP 800-90C):
 * for an n-bit conditioned output to be claimed as full entropy, the
 * conditioning input must carry at least n + 64 bits of min-entropy.
 * For SHA3-256 (n = 256) that is 320 bits.
 *
 * Required extracted-input bytes B satisfy:
 *
 *   B · 8 · H ≥ NIST_FULL_ENTROPY_BITS
 *   B ≥ ⌈ NIST_FULL_ENTROPY_BITS · H_DEN / (8 · H_NUM) ⌉ = ⌈45.45⌉ = 46
 *
 * H is expressed as the integer ratio 88/100 to keep the math
 * floating-point free.
 */
#define INFNOISE_NIST_FULL_ENTROPY_BITS	320	/* 256 + 64 */
#define INFNOISE_DESIGN_H_NUM		88	/* 0.88 bits/bit */
#define INFNOISE_DESIGN_H_DEN		100
#define INFNOISE_NIST_MIN_INPUT_BYTES \
	(((INFNOISE_NIST_FULL_ENTROPY_BITS * INFNOISE_DESIGN_H_DEN) + \
	  (8 * INFNOISE_DESIGN_H_NUM) - 1) / \
	 (8 * INFNOISE_DESIGN_H_NUM))		/* = 46 */

/*
 * Oversampling: how many full USB transfers worth of *extracted* bytes
 * (INFNOISE_BYTES_OUT = 64 per transfer ≈ 450 bits of design entropy)
 * are absorbed per 32-byte SHA3 output.
 *
 *   - oversample = 1 already absorbs 64 ≥ 46 bytes (≈ 450 bits ≥ 320),
 *     satisfying the NIST 256 + 64 input-entropy requirement.
 *   - Default 2 doubles the absorbed entropy for additional margin
 *     against any deviation from the H = 0.88 design assumption.
 *   - Cap at 16 bounds the kernel-side input buffer at 1 KiB.
 *
 * The driver enforces oversample · INFNOISE_BYTES_OUT ≥
 * INFNOISE_NIST_MIN_INPUT_BYTES at compile time via BUILD_BUG_ON in
 * infnoise_main.c.
 */
#define INFNOISE_OVERSAMPLE_MIN		1
#define INFNOISE_OVERSAMPLE_MAX		16
#define INFNOISE_OVERSAMPLE_DEFAULT	2

/*
 * hwrng quality settings (bits of entropy per 1024 bits of input)
 * - Raw mode: 0.88 bits/bit (unwhitened, reflects true entropy rate)
 * - Whitened mode: SHA3-256 output is full entropy when ≥ 256 input
 *   bits of entropy are absorbed (always true with oversample ≥ 1).
 */
#define INFNOISE_QUALITY_RAW		880	/* Raw: 0.88 bits/bit */
#define INFNOISE_QUALITY_WHITENED	1000	/* Whitened: ~1.0 bits/bit */

/* Timeouts */
#define INFNOISE_USB_TIMEOUT	1000	/* USB timeout in ms */
#define INFNOISE_WARMUP_ROUNDS	5000	/* Max warmup rounds */

/* Error recovery */
#define INFNOISE_MAX_RETRIES		3	/* Retries per transfer */
#define INFNOISE_RECOVERY_THRESHOLD	5	/* Consecutive errors before recovery */
#define INFNOISE_RETRY_DELAY_MS		10	/* Delay between retries */

/* Module parameters */
extern bool infnoise_debug;
extern bool infnoise_raw_mode;
extern unsigned int infnoise_oversample;

/* IOCTL definitions */
#define INFNOISE_IOC_MAGIC	'N'
#define INFNOISE_GET_STATS	_IOR(INFNOISE_IOC_MAGIC, 1, struct infnoise_stats)
#define INFNOISE_SET_RAW	_IOW(INFNOISE_IOC_MAGIC, 2, int)
#define INFNOISE_GET_ENTROPY	_IOR(INFNOISE_IOC_MAGIC, 3, u32)

/*
 * Statistics structure for ioctl (UAPI)
 *
 * Layout must be identical on 32-bit and 64-bit so that a 32-bit
 * userspace process can issue INFNOISE_GET_STATS on a 64-bit kernel.
 * Explicit padding ensures sizeof() == 40 on both, and __aligned(8)
 * forces u64 alignment on 32-bit where u64 is normally 4-byte aligned.
 *
 * even_misfires/odd_misfires are retained for ABI stability and report
 * 0 — the prediction-based estimator that tracked them has been replaced
 * by the NIST SP 800-90B RCT/APT pair, which has no equivalent counters.
 */
struct infnoise_stats {
	__u64 total_bits;
	__u32 total_ones;
	__u32 total_zeros;
	__u32 even_misfires;	/* Always 0; kept for ABI compatibility */
	__u32 odd_misfires;	/* Always 0; kept for ABI compatibility */
	__u32 entropy_estimate;	/* Fixed-point 16.16: design constant */
	__u32 k_estimate;	/* Fixed-point 16.16: design constant */
	__u8 warmup_complete;
	__u8 health_ok;
	__u8 __pad[6];
} __aligned(8);

/* Health check state (NIST SP 800-90B RCT + APT, two-tier thresholds) */
struct infnoise_health {
	/* Repetition Count Test */
	u32 rct_count;		/* current run length */
	bool rct_a;		/* sample value being repeated */

	/* Adaptive Proportion Test */
	u32 apt_count;		/* count of samples == apt_a in window */
	u32 apt_pos;		/* position 0..INM_APT_WINDOW-1 */
	bool apt_a;		/* current window's reference sample */

	/* Statistics */
	u64 total_bits;
	u32 total_ones;
	u32 total_zeros;
	u32 transient_failures;	/* total RCT/APT transient trips observed */

	/* Status */
	bool warmup_complete;	/* first APT window completed cleanly */
	bool failed;		/* transient trip — cleared on reset */
	bool dead;		/* permanent trip — sticky across resets */
};

/* SHA3-256 conditioning state (kernel crypto API) */
struct infnoise_sha3 {
	struct crypto_shash *tfm;
	u8 state[INFNOISE_SHA3_OUTPUT_SIZE];	/* internal state S */
};

/* Device state flags */
enum infnoise_flags {
	INFNOISE_PRESENT	= BIT(0),	/* Device is connected */
	INFNOISE_WARMUP_DONE	= BIT(1),	/* Warmup complete */
	INFNOISE_HWRNG_REG	= BIT(2),	/* hwrng registered */
	INFNOISE_HEALTH_FAIL	= BIT(3),	/* Transient health failure */
	INFNOISE_HEALTH_DEAD	= BIT(4),	/* Permanent health failure */
};

/* Main device structure */
struct infnoise_device {
	struct kref kref;		/* Lifetime reference count */

	struct usb_device *udev;
	struct usb_interface *intf;

	/* USB endpoints */
	u8 bulk_in_ep;
	u8 bulk_out_ep;

	/* Buffers */
	u8 *clock_buf;		/* Clock pattern buffer (512 bytes) */
	u8 *usb_buf;		/* Raw USB read buffer (576 bytes w/ FTDI status) */
	u8 *read_buf;		/* Processed read buffer (512 bytes, status stripped) */
	u8 *out_buf;		/* Single-transfer extracted buffer (64 bytes) */
	u8 *input_buf;		/* Concatenated extracted bytes across transfers,
				 * fed as input E to SHA3 conditioning */

	/* State */
	struct infnoise_health health;
	struct infnoise_sha3 sha3;
	u8 sha3_out[INFNOISE_SHA3_OUTPUT_SIZE];	/* latest output O */
	unsigned int out_pos;			/* bytes consumed from sha3_out */

	/* hwrng interface */
	struct hwrng hwrng;
	char hwrng_name[32];

	/* Character device */
	struct usb_class_driver class;
	int minor;
	bool raw_mode;		/* Per-device raw mode */

	/* Synchronization */
	struct mutex lock;		/* Device access lock */
	struct completion warmup_done;	/* Warmup completion */
	unsigned long flags;

	/* Warmup work */
	struct work_struct warmup_work;

	/* Statistics */
	u64 bytes_generated;
	u32 usb_errors;
	u32 consecutive_errors;		/* Consecutive USB errors */
	u32 recoveries;			/* Successful recovery count */
};

/* SHA3 conditioning (infnoise_sha3.c) */
int  infnoise_sha3_init(struct infnoise_sha3 *sha3);
void infnoise_sha3_free(struct infnoise_sha3 *sha3);
int  infnoise_sha3_update(struct infnoise_sha3 *sha3,
			  const u8 *entropy, size_t entropy_len,
			  u8 *output);

/* Health check functions (infnoise_health.c) */
void infnoise_health_init(struct infnoise_health *health);
void infnoise_health_reset(struct infnoise_health *health);
bool infnoise_health_add_bit(struct infnoise_health *health, bool bit);
bool infnoise_health_ok(struct infnoise_health *health);
void infnoise_health_get_stats(struct infnoise_health *health,
			       struct infnoise_stats *stats);

#endif /* INFNOISE_H */
