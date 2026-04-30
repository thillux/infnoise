// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * SHA3-256 conditioning for the Infinite Noise TRNG kernel driver.
 *
 * Uses the kernel's crypto API (no hand-rolled primitive). Each refill
 * absorbs raw entropy E and emits both the user-visible output and the
 * next internal state from the same input:
 *
 *   O  = SHA3-256(S || INFNOISE_DS_OUTPUT || E)
 *   S' = SHA3-256(S || INFNOISE_DS_STATE  || E)
 *
 * The same pre-image structure with distinct domain-separator bytes
 * means O and S' are independent under the random-oracle assumption.
 * Knowledge of O does not help an attacker recover S' (forward secrecy)
 * and a state compromise does not reveal past O values.
 */

#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/err.h>
#include <crypto/hash.h>
#include "infnoise.h"

/*
 * Run SHA3-256 over (S || ds || entropy) into @out.
 *
 * @desc must already have ->tfm pointing at a sha3-256 transform.
 */
static int sha3_compose(struct shash_desc *desc, const u8 *state, u8 ds,
			const u8 *entropy, size_t entropy_len, u8 *out)
{
	int ret;

	ret = crypto_shash_init(desc);
	if (ret)
		return ret;

	ret = crypto_shash_update(desc, state, INFNOISE_SHA3_OUTPUT_SIZE);
	if (ret)
		return ret;

	ret = crypto_shash_update(desc, &ds, 1);
	if (ret)
		return ret;

	if (entropy_len) {
		ret = crypto_shash_update(desc, entropy, entropy_len);
		if (ret)
			return ret;
	}

	return crypto_shash_final(desc, out);
}

/**
 * infnoise_sha3_init - Allocate the SHA3-256 transform and zero state
 * @sha3: SHA3 wrapper
 *
 * Returns 0 on success or a negative errno (notably -ENOENT if the
 * kernel was built without CONFIG_CRYPTO_SHA3).
 */
int infnoise_sha3_init(struct infnoise_sha3 *sha3)
{
	struct crypto_shash *tfm;

	memset(sha3->state, 0, sizeof(sha3->state));

	tfm = crypto_alloc_shash("sha3-256", 0, 0);
	if (IS_ERR(tfm)) {
		sha3->tfm = NULL;
		return PTR_ERR(tfm);
	}

	if (crypto_shash_digestsize(tfm) != INFNOISE_SHA3_OUTPUT_SIZE) {
		crypto_free_shash(tfm);
		sha3->tfm = NULL;
		return -EINVAL;
	}

	sha3->tfm = tfm;
	return 0;
}

/**
 * infnoise_sha3_free - Release the SHA3 transform and wipe state
 * @sha3: SHA3 wrapper
 *
 * Safe to call on a partially-initialized struct (NULL tfm).
 */
void infnoise_sha3_free(struct infnoise_sha3 *sha3)
{
	if (sha3->tfm) {
		crypto_free_shash(sha3->tfm);
		sha3->tfm = NULL;
	}
	memzero_explicit(sha3->state, sizeof(sha3->state));
}

/**
 * infnoise_sha3_update - Absorb @entropy and produce @output
 * @sha3: SHA3 wrapper (must have been initialized)
 * @entropy: raw noise-source bytes to absorb
 * @entropy_len: length of @entropy in bytes
 * @output: 32-byte buffer receiving O = H(S || 0x11 || E)
 *
 * On success S is replaced with H(S || 0x00 || E); on error the state
 * is left untouched and @output may have been partially written.
 */
int infnoise_sha3_update(struct infnoise_sha3 *sha3, const u8 *entropy,
			 size_t entropy_len, u8 *output)
{
	SHASH_DESC_ON_STACK(desc, sha3->tfm);
	u8 new_state[INFNOISE_SHA3_OUTPUT_SIZE];
	int ret;

	desc->tfm = sha3->tfm;

	/* O = H(S || 0x11 || E) — uses current S. */
	ret = sha3_compose(desc, sha3->state, INFNOISE_DS_OUTPUT,
			   entropy, entropy_len, output);
	if (ret)
		goto out;

	/* S' = H(S || 0x00 || E) — also uses current S, into a temporary. */
	ret = sha3_compose(desc, sha3->state, INFNOISE_DS_STATE,
			   entropy, entropy_len, new_state);
	if (ret)
		goto out;

	memcpy(sha3->state, new_state, sizeof(sha3->state));

out:
	memzero_explicit(new_state, sizeof(new_state));
	shash_desc_zero(desc);
	return ret;
}
