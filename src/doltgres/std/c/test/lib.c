/*
 * Copyright 2026 Dolthub, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * The C port of coretests' lib.rs helper the flt2dec tests use (Rust
 * 1.99.1): test_rng, a XorShiftRng seeded from std's RandomState over the
 * caller's location; and the test-only ports of the random number crates
 * core's sort tests use (the port text's section 15): rand_xorshift 0.4.0's
 * XorShiftRng, rand_core 0.9.5's seed_from_u64, and rand 0.9.5's
 * StandardUniform and UniformInt<i32>.
 */
#include "postgres.h"

#include "doltgres/std/fmt.h"
#include "doltgres/std/hash.h"
#include "doltgres/std/int128.h"
#include "doltgres/std/num.h"
#include "doltgres/std/str.h"
#include "doltgres/std/test_flt2dec.h"
#include "doltgres/std/unwind.h"

/*
 * coretests_test_rng is test_rng, #[track_caller]: a XorShiftRng's four
 * words in result, seeded from a random hash of the caller's location and the
 * bytes 0 to 7.
 */
void
coretests_test_rng(uint32 *result, const char *file, uint32 line, uint32 col)
{
	StdRandomState state = std_RandomState_new();
	StdSipHasher13 hasher_state;
	StdHasher	hasher = {STD_RANDOM_STATE_BUILD_HASHER_OPS.hasher, &hasher_state};
	uint64		hc64;
	uint8		seed[16];

	STD_RANDOM_STATE_BUILD_HASHER_OPS.build_hasher(&state, &hasher_state);
	std_Hasher_write_str(hasher, file, strlen(file));
	std_Hasher_write_u32(hasher, line);
	std_Hasher_write_u32(hasher, col);
	hc64 = hasher.ops->finish(hasher.self);
	std_u64_to_le_bytes(hc64, seed);
	for (uint8 i = 0; i < 8; i++)
		seed[8 + i] = i;
	coretests_XorShiftRng_from_seed(seed, result);
}

/* coretests_XorShiftRng_next_u32 is rand_xorshift's XorShiftRng::next_u32 over its x, y, z, and w. */
uint32
coretests_XorShiftRng_next_u32(uint32 *self)
{
	uint32		x = self[0];
	uint32		t = x ^ (x << 11);
	uint32		w_ = self[3];

	self[0] = self[1];
	self[1] = self[2];
	self[2] = self[3];
	self[3] = w_ ^ (w_ >> 19) ^ (t ^ (t >> 8));
	return self[3];
}

/* coretests_XorShiftRng_next_u64 is XorShiftRng::next_u64, rand_core's next_u64_via_u32: the first word low. */
uint64
coretests_XorShiftRng_next_u64(uint32 *self)
{
	uint64		x = coretests_XorShiftRng_next_u32(self);
	uint64		y = coretests_XorShiftRng_next_u32(self);

	return (y << 32) | x;
}

/*
 * coretests_XorShiftRng_from_seed is rand_xorshift's XorShiftRng::from_seed:
 * the seed's four little-endian words, or 0xBAD5EED in each when all four
 * are zero.
 */
void
coretests_XorShiftRng_from_seed(const uint8 *seed, uint32 *result)
{
	uint32		seed_u32[4];

	for (size_t i = 0; i < 4; i++)
		seed_u32[i] = std_u32_from_le_bytes(STD_PTR_AT(seed, i, 4));
	if (seed_u32[0] == 0 && seed_u32[1] == 0 && seed_u32[2] == 0 && seed_u32[3] == 0)
	{
		for (size_t i = 0; i < 4; i++)
			seed_u32[i] = 0xBAD5EED;
	}
	memcpy(result, seed_u32, sizeof(seed_u32));
}

/*
 * coretests_XorShiftRng_seed_from_u64_pcg32 is seed_from_u64's pcg32: the
 * state advanced, then PCG's output function's word, little-endian.
 */
static void
coretests_XorShiftRng_seed_from_u64_pcg32(uint64 *state, uint8 *result)
{
	uint64		advanced = std_wrapping_add_u64(std_wrapping_mul_u64(*state, UINT64CONST(6364136223846793005)),
												UINT64CONST(11634580027462260723));
	uint32		xorshifted;
	uint32		rot;

	*state = advanced;
	xorshifted = (uint32) STD_SHR_FOLDED(u64, STD_SHR_FOLDED(u64, advanced, 18) ^ advanced, 27);
	rot = (uint32) STD_SHR_FOLDED(u64, advanced, 59);
	std_u32_to_le_bytes(std_rotate_right_u32(xorshifted, rot), result);
}

/*
 * coretests_XorShiftRng_seed_from_u64 is rand_core's SeedableRng::seed_from_u64
 * at XorShiftRng: the 16-byte seed filled by pcg32's words in its 4-byte
 * chunks, which leave no remainder, then from_seed.
 */
void
coretests_XorShiftRng_seed_from_u64(uint64 state, uint32 *result)
{
	uint8		seed[16] = {0};

	for (size_t i = 0; i < 4; i++)
		coretests_XorShiftRng_seed_from_u64_pcg32(&state, STD_PTR_AT(seed, i, 4));
	coretests_XorShiftRng_from_seed(seed, result);
}

/* rand_StandardUniform_sample_u32 is StandardUniform's u32: the RNG's next_u32. */
uint32
rand_StandardUniform_sample_u32(uint32 *rng)
{
	return coretests_XorShiftRng_next_u32(rng);
}

/* rand_StandardUniform_sample_i32 is StandardUniform's i32, impl_int_from_uint!'s: a random u32 as i32. */
int32
rand_StandardUniform_sample_i32(uint32 *rng)
{
	return (int32) rand_StandardUniform_sample_u32(rng);
}

/* rand_StandardUniform_sample_f64 is StandardUniform's f64, float_impls!'s: a random u64's top 53 bits times 2^-53, in [0, 1). */
float8
rand_StandardUniform_sample_f64(uint32 *rng)
{
	uint64		value = coretests_XorShiftRng_next_u64(rng);

	return (1.0 / (float8) ((uint64) 1 << 53)) * (float8) STD_SHR_FOLDED(u64, value, 11);
}

/*
 * coretests_rand_UniformInt_new_inclusive is rand's UniformInt::new_inclusive
 * over [low, high] at an integer type of bits bits, signed or not, sampled as
 * sample_bits: Err EmptyRange, unwrapped at the caller's file, line, and
 * column, unless low <= high; its range wraps to 0 for the whole type.
 */
CoretestsUniformInt
coretests_rand_UniformInt_new_inclusive(uint64 low, uint64 high, bool is_signed, uint32 bits, uint32 sample_bits,
										const char *file, uint32 line, uint32 col)
{
	uint64		type_mask = bits == 64 ? PG_UINT64_MAX : (UINT64CONST(1) << bits) - 1;
	uint64		sample_mask = sample_bits == 64 ? PG_UINT64_MAX : (UINT64CONST(1) << sample_bits) - 1;
	bool		ordered = is_signed ? (int64) (low << (64 - bits)) <= (int64) (high << (64 - bits)) : low <= high;
	uint64		range;
	CoretestsUniformInt result;

	if (!ordered)
	{
		StdStr		error = {"EmptyRange", 10};

		std_result_unwrap_failed("called `Result::unwrap()` on an `Err` value",
								 sizeof("called `Result::unwrap()` on an `Err` value") - 1, &error,
								 std_str_Display_fmt, file, line, col);
	}
	range = ((high - low) + 1) & type_mask;
	result.low = low;
	result.range = range;
	result.thresh = range > 0 ? ((((~range) + 1) & sample_mask) % range) & type_mask : 0;
	return result;
}

/*
 * coretests_rand_UniformInt_new is rand's UniformInt::new over [low, high):
 * Err EmptyRange, unwrapped at the caller's file, line, and column, unless
 * low < high, else new_inclusive to high - 1.
 */
CoretestsUniformInt
coretests_rand_UniformInt_new(uint64 low, uint64 high, bool is_signed, uint32 bits, uint32 sample_bits,
							  const char *file, uint32 line, uint32 col)
{
	bool		ordered = is_signed ? (int64) (low << (64 - bits)) < (int64) (high << (64 - bits)) : low < high;
	uint64		type_mask = bits == 64 ? PG_UINT64_MAX : (UINT64CONST(1) << bits) - 1;

	if (!ordered)
	{
		StdStr		error = {"EmptyRange", 10};

		std_result_unwrap_failed("called `Result::unwrap()` on an `Err` value",
								 sizeof("called `Result::unwrap()` on an `Err` value") - 1, &error,
								 std_str_Display_fmt, file, line, col);
	}
	return coretests_rand_UniformInt_new_inclusive(low, (high - 1) & type_mask, is_signed, bits, sample_bits, file,
												   line, col);
}

/*
 * coretests_rand_UniformInt_sample is UniformInt::sample, Lemire's method:
 * a sample_bits-bit draw widened by the range, redrawn while its low half is
 * under the threshold, its high half added to low, wrapping in the type.
 */
uint64
coretests_rand_UniformInt_sample(const CoretestsUniformInt *self, uint32 *rng, uint32 bits, uint32 sample_bits)
{
	uint64		type_mask = bits == 64 ? PG_UINT64_MAX : (UINT64CONST(1) << bits) - 1;
	uint64		range = self->range;
	uint64		thresh = self->thresh;
	uint64		hi;

	if (range == 0)
		return (sample_bits == 64 ? coretests_XorShiftRng_next_u64(rng) : coretests_XorShiftRng_next_u32(rng)) &
			type_mask;
	for (;;)
	{
		uint64		lo;

		if (sample_bits == 64)
		{
			StdU128		wide = std_mul_u64_wide(coretests_XorShiftRng_next_u64(rng), range);

			hi = wide.hi;
			lo = wide.lo;
		}
		else
		{
			uint64		wide = (uint64) coretests_XorShiftRng_next_u32(rng) * range;

			hi = wide >> 32;
			lo = wide & 0xffffffff;
		}
		if (lo >= thresh)
			break;
	}
	return (self->low + hi) & type_mask;
}
