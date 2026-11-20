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
 * The C form of foldhash 0.2.0's fast hasher (the port text's section 15),
 * from its lib.rs, seed.rs, and fast.rs for 64-bit targets with the std
 * feature: the folded multiply, the short and long byte hashes, the
 * per-hasher seed from a stack address and the backend's last one, the
 * process's global seed from addresses and the time, FoldHasher, and
 * fast::RandomState.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/std/alloc.h"
#include "doltgres/std/atomic.h"
#include "doltgres/std/foldhash.h"
#include "doltgres/std/time.h"

/* FOLDHASH_FAST_FILE is fast.rs as the remapped build names it, where a sponge's overflow panics. */
#define FOLDHASH_FAST_FILE "cargo-registry/index.crates.io-1949cf8c6b5b557f" STD_SEP "foldhash-0.2.0" STD_SEP "src" STD_SEP "fast.rs"

/* FOLDHASH_ARBITRARY1 through FOLDHASH_ARBITRARY11 are foldhash's arbitrary constants, digits of pi. */
#define FOLDHASH_ARBITRARY1 UINT64CONST(0x13198a2e03707344)
#define FOLDHASH_ARBITRARY2 UINT64CONST(0xa4093822299f31d0)
#define FOLDHASH_ARBITRARY5 UINT64CONST(0xbe5466cf34e90c6c)

/* foldhash_folded_multiply is folded_multiply: the full product's halves xored. */
static uint64
foldhash_folded_multiply(uint64 x, uint64 y)
{
	StdU128		full = std_mul_u64_wide(x, y);

	return full.lo ^ full.hi;
}

/* foldhash_hash_bytes_short is hash_bytes_short: a hash of at most 16 bytes folded into the accumulator. */
static uint64
foldhash_hash_bytes_short(const uint8 *bytes, size_t len, uint64 accumulator, const uint64 *seeds)
{
	uint64		s0 = accumulator;
	uint64		s1 = seeds[1];

	if (len >= 8)
	{
		uint64		a;
		uint64		b;

		memcpy(&a, bytes, 8);
		memcpy(&b, bytes + len - 8, 8);
		s0 ^= a;
		s1 ^= b;
	}
	else if (len >= 4)
	{
		uint32		a;
		uint32		b;

		memcpy(&a, bytes, 4);
		memcpy(&b, bytes + len - 4, 4);
		s0 ^= (uint64) a;
		s1 ^= (uint64) b;
	}
	else if (len > 0)
	{
		uint8		lo = bytes[0];
		uint8		mid = bytes[len / 2];
		uint8		hi = bytes[len - 1];

		s0 ^= (uint64) lo;
		s1 ^= ((uint64) hi << 8) | (uint64) mid;
	}
	return foldhash_folded_multiply(s0, s1);
}

/* foldhash_load is load: eight bytes at an offset, unaligned. */
static uint64
foldhash_load(const uint8 *bytes, size_t offset)
{
	uint64		v;

	memcpy(&v, bytes + offset, 8);
	return v;
}

/* foldhash_hash_bytes_long is hash_bytes_long: a hash of more than 16 bytes folded into the accumulator. */
static uint64
foldhash_hash_bytes_long(const uint8 *v, size_t len, uint64 accumulator, const uint64 *seeds)
{
	uint64		s0 = accumulator;
	uint64		s1 = s0 + seeds[1];

	if (len > 128)
	{
		uint64		s2 = s0 + seeds[2];
		uint64		s3 = s0 + seeds[3];

		if (len > 256)
		{
			uint64		s4 = s0 + seeds[4];
			uint64		s5 = s0 + seeds[5];

			for (;;)
			{
				s0 = foldhash_folded_multiply(foldhash_load(v, 0) ^ s0, foldhash_load(v, 48) ^ seeds[0]);
				s1 = foldhash_folded_multiply(foldhash_load(v, 8) ^ s1, foldhash_load(v, 56) ^ seeds[0]);
				s2 = foldhash_folded_multiply(foldhash_load(v, 16) ^ s2, foldhash_load(v, 64) ^ seeds[0]);
				s3 = foldhash_folded_multiply(foldhash_load(v, 24) ^ s3, foldhash_load(v, 72) ^ seeds[0]);
				s4 = foldhash_folded_multiply(foldhash_load(v, 32) ^ s4, foldhash_load(v, 80) ^ seeds[0]);
				s5 = foldhash_folded_multiply(foldhash_load(v, 40) ^ s5, foldhash_load(v, 88) ^ seeds[0]);
				v += 96;
				len -= 96;
				if (len <= 256)
					break;
			}
			s0 ^= s4;
			s1 ^= s5;
		}
		for (;;)
		{
			s0 = foldhash_folded_multiply(foldhash_load(v, 0) ^ s0, foldhash_load(v, 32) ^ seeds[0]);
			s1 = foldhash_folded_multiply(foldhash_load(v, 8) ^ s1, foldhash_load(v, 40) ^ seeds[0]);
			s2 = foldhash_folded_multiply(foldhash_load(v, 16) ^ s2, foldhash_load(v, 48) ^ seeds[0]);
			s3 = foldhash_folded_multiply(foldhash_load(v, 24) ^ s3, foldhash_load(v, 56) ^ seeds[0]);
			v += 64;
			len -= 64;
			if (len <= 128)
				break;
		}
		s0 ^= s2;
		s1 ^= s3;
	}
	s0 = foldhash_folded_multiply(foldhash_load(v, 0) ^ s0, foldhash_load(v, len - 16) ^ seeds[0]);
	s1 = foldhash_folded_multiply(foldhash_load(v, 8) ^ s1, foldhash_load(v, len - 8) ^ seeds[0]);
	if (len >= 32)
	{
		s0 = foldhash_folded_multiply(foldhash_load(v, 16) ^ s0, foldhash_load(v, len - 32) ^ seeds[0]);
		s1 = foldhash_folded_multiply(foldhash_load(v, 24) ^ s1, foldhash_load(v, len - 24) ^ seeds[0]);
		if (len >= 64)
		{
			s0 = foldhash_folded_multiply(foldhash_load(v, 32) ^ s0, foldhash_load(v, len - 48) ^ seeds[0]);
			s1 = foldhash_folded_multiply(foldhash_load(v, 40) ^ s1, foldhash_load(v, len - 40) ^ seeds[0]);
			if (len >= 96)
			{
				s0 = foldhash_folded_multiply(foldhash_load(v, 48) ^ s0, foldhash_load(v, len - 64) ^ seeds[0]);
				s1 = foldhash_folded_multiply(foldhash_load(v, 56) ^ s1, foldhash_load(v, len - 56) ^ seeds[0]);
			}
		}
	}
	return s0 ^ s1;
}

/* foldhash_seed_PER_HASHER_NONDETERMINISM is the backend's last per-hasher seed, seed.rs's thread-local cell. */
static uint64 foldhash_seed_PER_HASHER_NONDETERMINISM = 0;

/* foldhash_seed_gen_per_hasher_seed is gen_per_hasher_seed: a stack address folded with the backend's last seed. */
static uint64
foldhash_seed_gen_per_hasher_seed(void)
{
	volatile uint64 per_hasher_seed = 0;
	uint64		stack_ptr = (uint64) (uintptr_t) &per_hasher_seed;
	uint64		nondeterminism;

	per_hasher_seed = stack_ptr;
	nondeterminism = foldhash_seed_PER_HASHER_NONDETERMINISM;
	per_hasher_seed = foldhash_folded_multiply(per_hasher_seed, FOLDHASH_ARBITRARY1 ^ nondeterminism);
	foldhash_seed_PER_HASHER_NONDETERMINISM = per_hasher_seed;
	return foldhash_folded_multiply(per_hasher_seed, FOLDHASH_ARBITRARY2);
}

/* foldhash_seed_SharedSeed_from_u64 is SharedSeed::from_u64: six seeds mixed from one, each with ones forced. */
static FoldhashSharedSeed
foldhash_seed_SharedSeed_from_u64(uint64 seed)
{
#define FOLDHASH_MIX(x) foldhash_folded_multiply((x), FOLDHASH_ARBITRARY5)
	uint64		seed_a = FOLDHASH_MIX(FOLDHASH_MIX(FOLDHASH_MIX(seed)));
	uint64		seed_b = FOLDHASH_MIX(FOLDHASH_MIX(FOLDHASH_MIX(seed_a)));
	uint64		seed_c = FOLDHASH_MIX(FOLDHASH_MIX(FOLDHASH_MIX(seed_b)));
	uint64		seed_d = FOLDHASH_MIX(FOLDHASH_MIX(FOLDHASH_MIX(seed_c)));
	uint64		seed_e = FOLDHASH_MIX(FOLDHASH_MIX(FOLDHASH_MIX(seed_d)));
	uint64		seed_f = FOLDHASH_MIX(FOLDHASH_MIX(FOLDHASH_MIX(seed_e)));
	const uint64 forced_ones = (UINT64CONST(1) << 63) | (UINT64CONST(1) << 31) | 1;
	FoldhashSharedSeed result = {{
			seed_a | forced_ones, seed_b | forced_ones, seed_c | forced_ones,
			seed_d | forced_ones, seed_e | forced_ones, seed_f | forced_ones
	}};
#undef FOLDHASH_MIX

	return result;
}

/* FOLDHASH_SEED_UNINIT, LOCKED, and INIT are the global seed's states. */
#define FOLDHASH_SEED_UNINIT 0
#define FOLDHASH_SEED_LOCKED 1
#define FOLDHASH_SEED_INIT 2

/* FoldhashSeedGlobalSeedStorage is the process's global seed and its state. */
typedef struct FoldhashSeedGlobalSeedStorage
{
	StdAtomicU8 state;
	FoldhashSharedSeed seed;
} FoldhashSeedGlobalSeedStorage;

/* foldhash_seed_GLOBAL_SEED_STORAGE is the process's global seed. */
static FoldhashSeedGlobalSeedStorage foldhash_seed_GLOBAL_SEED_STORAGE;

/* foldhash_seed_global_mix is generate_global_seed's mix, a value folded into the seed. */
static uint64
foldhash_seed_global_mix(uint64 seed, uint64 x)
{
	return foldhash_folded_multiply(seed ^ x, FOLDHASH_ARBITRARY5);
}

/* foldhash_seed_generate_global_seed mixes a stack, a function's, a static's, and an allocation's addresses and the time into a seed. */
static FoldhashSharedSeed
foldhash_seed_generate_global_seed(void)
{
	volatile uint64 seed = 0;
	uint64		stack_ptr = (uint64) (uintptr_t) &seed;
	uint64		func_ptr = (uint64) (uintptr_t) foldhash_seed_generate_global_seed;
	uint64		static_ptr = (uint64) (uintptr_t) &foldhash_seed_GLOBAL_SEED_STORAGE;
	StdSystemTime now = std_SystemTime_now();
	StdDuration duration;
	StdDuration error;
	uint8	   *box_ptr;

	seed = foldhash_seed_global_mix(seed, stack_ptr);
	seed = foldhash_seed_global_mix(seed, func_ptr);
	seed = foldhash_seed_global_mix(seed, static_ptr);
	if (std_SystemTime_duration_since(&now, STD_UNIX_EPOCH, &duration, &error))
	{
		seed = foldhash_seed_global_mix(seed, (uint64) std_Duration_subsec_nanos(&duration));
		seed = foldhash_seed_global_mix(seed, std_Duration_as_secs(&duration));
	}
	box_ptr = std_alloc(1, 1);
	*box_ptr = 0;
	seed = foldhash_seed_global_mix(seed, (uint64) (uintptr_t) box_ptr);
	std_free(box_ptr, 1, 1);
	return foldhash_seed_SharedSeed_from_u64(seed);
}

/* foldhash_seed_GlobalSeed_init_slow makes the process's global seed, unless another thread has. */
static void
foldhash_seed_GlobalSeed_init_slow(void)
{
	FoldhashSharedSeed seed = foldhash_seed_generate_global_seed();

	for (;;)
	{
		uint8		expected = FOLDHASH_SEED_UNINIT;

		if (std_AtomicU8_compare_exchange_weak_acquire(&foldhash_seed_GLOBAL_SEED_STORAGE.state, &expected,
													   FOLDHASH_SEED_LOCKED))
		{
			foldhash_seed_GLOBAL_SEED_STORAGE.seed = seed;
			std_AtomicU8_store_release(&foldhash_seed_GLOBAL_SEED_STORAGE.state, FOLDHASH_SEED_INIT);
			return;
		}
		if (expected == FOLDHASH_SEED_INIT)
			return;
		std_hint_spin_loop();
	}
}

/* foldhash_seed_GlobalSeed_new is GlobalSeed::new: the process's global seed, made at its first use. */
static const FoldhashSharedSeed *
foldhash_seed_GlobalSeed_new(void)
{
	if (std_AtomicU8_load_acquire(&foldhash_seed_GLOBAL_SEED_STORAGE.state) != FOLDHASH_SEED_INIT)
		foldhash_seed_GlobalSeed_init_slow();
	return &foldhash_seed_GLOBAL_SEED_STORAGE.seed;
}

/* foldhash_fast_FoldHasher_write_num is write_num: an integer of a width into the sponge, folding it when full. */
static void
foldhash_fast_FoldHasher_write_num(FoldhashFastFoldHasher *self, StdU128 x, size_t bits)
{
	if (std_add_usize((size_t) self->sponge_len, bits, FOLDHASH_FAST_FILE, 37, 12) > 128)
	{
		self->accumulator = foldhash_folded_multiply(self->sponge.lo ^ self->accumulator,
													 self->sponge.hi ^ self->seeds[0]);
		self->sponge = x;
		self->sponge_len = (uint8) bits;
	}
	else
	{
		self->sponge = std_u128_or(self->sponge, std_shl_u128(x, self->sponge_len, FOLDHASH_FAST_FILE, 44, 28));
		self->sponge_len = std_add_u8(self->sponge_len, (uint8) bits, FOLDHASH_FAST_FILE, 45, 13);
	}
}

/* foldhash_fast_FoldHasher_Hasher_write folds bytes into the accumulator after rotating it by their length. */
static void
foldhash_fast_FoldHasher_Hasher_write(void *self_, const uint8 *bytes, size_t len)
{
	FoldhashFastFoldHasher *self = self_;

	self->accumulator = std_rotate_right_u64(self->accumulator, (uint32) len);
	if (len <= 16)
		self->accumulator = foldhash_hash_bytes_short(bytes, len, self->accumulator, self->seeds);
	else
		self->accumulator = foldhash_hash_bytes_long(bytes, len, self->accumulator, self->seeds);
}

/* foldhash_fast_FoldHasher_Hasher_write_u8 puts a u8 in the sponge. */
static void
foldhash_fast_FoldHasher_Hasher_write_u8(void *self, uint8 i)
{
	StdU128		x = {0, i};

	foldhash_fast_FoldHasher_write_num(self, x, 8);
}

/* foldhash_fast_FoldHasher_Hasher_write_u16 puts a u16 in the sponge. */
static void
foldhash_fast_FoldHasher_Hasher_write_u16(void *self, uint16 i)
{
	StdU128		x = {0, i};

	foldhash_fast_FoldHasher_write_num(self, x, 16);
}

/* foldhash_fast_FoldHasher_Hasher_write_u32 puts a u32 in the sponge. */
static void
foldhash_fast_FoldHasher_Hasher_write_u32(void *self, uint32 i)
{
	StdU128		x = {0, i};

	foldhash_fast_FoldHasher_write_num(self, x, 32);
}

/* foldhash_fast_FoldHasher_Hasher_write_u64 puts a u64 in the sponge. */
static void
foldhash_fast_FoldHasher_Hasher_write_u64(void *self, uint64 i)
{
	StdU128		x = {0, i};

	foldhash_fast_FoldHasher_write_num(self, x, 64);
}

/* foldhash_fast_FoldHasher_Hasher_write_u128 folds a u128 into the accumulator. */
static void
foldhash_fast_FoldHasher_Hasher_write_u128(void *self_, StdU128 i)
{
	FoldhashFastFoldHasher *self = self_;

	self->accumulator = foldhash_folded_multiply(i.lo ^ self->accumulator, i.hi ^ self->seeds[0]);
}

/* foldhash_fast_FoldHasher_Hasher_write_usize puts a usize in the sponge, as a u64 on 64-bit targets. */
static void
foldhash_fast_FoldHasher_Hasher_write_usize(void *self, size_t i)
{
	StdU128		x = {0, (uint64) i};

	foldhash_fast_FoldHasher_write_num(self, x, 64);
}

/* foldhash_fast_FoldHasher_Hasher_finish folds what is left in the sponge into the accumulator. */
static uint64
foldhash_fast_FoldHasher_Hasher_finish(const void *self_)
{
	const FoldhashFastFoldHasher *self = self_;

	if (self->sponge_len > 0)
		return foldhash_folded_multiply(self->sponge.lo ^ self->accumulator,
										self->sponge.hi ^ self->seeds[0]);
	else
		return self->accumulator;
}

/* foldhash_fast_FoldHasher_Hasher_write_i8 is Hasher's write_i8 for the hasher. */
static void
foldhash_fast_FoldHasher_Hasher_write_i8(void *self, int8 i)
{
	StdHasher	hasher = {&FOLDHASH_FAST_FOLD_HASHER_HASHER_OPS, self};

	std_Hasher_write_i8(hasher, i);
}

/* foldhash_fast_FoldHasher_Hasher_write_i16 is Hasher's write_i16 for the hasher. */
static void
foldhash_fast_FoldHasher_Hasher_write_i16(void *self, int16 i)
{
	StdHasher	hasher = {&FOLDHASH_FAST_FOLD_HASHER_HASHER_OPS, self};

	std_Hasher_write_i16(hasher, i);
}

/* foldhash_fast_FoldHasher_Hasher_write_i32 is Hasher's write_i32 for the hasher. */
static void
foldhash_fast_FoldHasher_Hasher_write_i32(void *self, int32 i)
{
	StdHasher	hasher = {&FOLDHASH_FAST_FOLD_HASHER_HASHER_OPS, self};

	std_Hasher_write_i32(hasher, i);
}

/* foldhash_fast_FoldHasher_Hasher_write_i64 is Hasher's write_i64 for the hasher. */
static void
foldhash_fast_FoldHasher_Hasher_write_i64(void *self, int64 i)
{
	StdHasher	hasher = {&FOLDHASH_FAST_FOLD_HASHER_HASHER_OPS, self};

	std_Hasher_write_i64(hasher, i);
}

/* foldhash_fast_FoldHasher_Hasher_write_i128 is Hasher's write_i128 for the hasher. */
static void
foldhash_fast_FoldHasher_Hasher_write_i128(void *self, StdI128 i)
{
	StdHasher	hasher = {&FOLDHASH_FAST_FOLD_HASHER_HASHER_OPS, self};

	std_Hasher_write_i128(hasher, i);
}

/* foldhash_fast_FoldHasher_Hasher_write_isize is Hasher's write_isize for the hasher. */
static void
foldhash_fast_FoldHasher_Hasher_write_isize(void *self, ssize_t i)
{
	StdHasher	hasher = {&FOLDHASH_FAST_FOLD_HASHER_HASHER_OPS, self};

	std_Hasher_write_isize(hasher, i);
}

/* foldhash_fast_FoldHasher_Hasher_write_length_prefix is Hasher's write_length_prefix for the hasher. */
static void
foldhash_fast_FoldHasher_Hasher_write_length_prefix(void *self, size_t len)
{
	StdHasher	hasher = {&FOLDHASH_FAST_FOLD_HASHER_HASHER_OPS, self};

	std_Hasher_write_length_prefix(hasher, len);
}

/* foldhash_fast_FoldHasher_Hasher_write_str is Hasher's write_str for the hasher, foldhash's own being nightly's. */
static void
foldhash_fast_FoldHasher_Hasher_write_str(void *self, const char *s, size_t len)
{
	StdHasher	hasher = {&FOLDHASH_FAST_FOLD_HASHER_HASHER_OPS, self};

	std_Hasher_write_str(hasher, s, len);
}

/* FOLDHASH_FAST_FOLD_HASHER_HASHER_OPS is FoldHasher's table of Hasher. */
const StdHasherOps FOLDHASH_FAST_FOLD_HASHER_HASHER_OPS = {
	.finish = foldhash_fast_FoldHasher_Hasher_finish,
	.write = foldhash_fast_FoldHasher_Hasher_write,
	.write_u8 = foldhash_fast_FoldHasher_Hasher_write_u8,
	.write_u16 = foldhash_fast_FoldHasher_Hasher_write_u16,
	.write_u32 = foldhash_fast_FoldHasher_Hasher_write_u32,
	.write_u64 = foldhash_fast_FoldHasher_Hasher_write_u64,
	.write_u128 = foldhash_fast_FoldHasher_Hasher_write_u128,
	.write_usize = foldhash_fast_FoldHasher_Hasher_write_usize,
	.write_i8 = foldhash_fast_FoldHasher_Hasher_write_i8,
	.write_i16 = foldhash_fast_FoldHasher_Hasher_write_i16,
	.write_i32 = foldhash_fast_FoldHasher_Hasher_write_i32,
	.write_i64 = foldhash_fast_FoldHasher_Hasher_write_i64,
	.write_i128 = foldhash_fast_FoldHasher_Hasher_write_i128,
	.write_isize = foldhash_fast_FoldHasher_Hasher_write_isize,
	.write_length_prefix = foldhash_fast_FoldHasher_Hasher_write_length_prefix,
	.write_str = foldhash_fast_FoldHasher_Hasher_write_str,
};

/* foldhash_fast_RandomState_Default_default is fast::RandomState's Default: a new per-hasher seed, the global seed made. */
void
foldhash_fast_RandomState_Default_default(void *result)
{
	FoldhashFastRandomState *state = result;

	state->per_hasher_seed = foldhash_seed_gen_per_hasher_seed();
	(void) foldhash_seed_GlobalSeed_new();
}

/* foldhash_fast_RandomState_BuildHasher_build_hasher writes a FoldHasher under the state's seed and the global seed. */
static void
foldhash_fast_RandomState_BuildHasher_build_hasher(const void *self_, void *result)
{
	const FoldhashFastRandomState *self = self_;
	FoldhashFastFoldHasher *hasher = result;

	hasher->accumulator = self->per_hasher_seed;
	hasher->sponge.hi = 0;
	hasher->sponge.lo = 0;
	hasher->sponge_len = 0;
	hasher->seeds = foldhash_seed_GLOBAL_SEED_STORAGE.seed.seeds;
}

StaticAssertDecl(sizeof(FoldhashFastFoldHasher) <= STD_HASHER_SIZE, "fast::RandomState's hasher fits a hasher's storage");

/* FOLDHASH_FAST_RANDOM_STATE_BUILD_HASHER_OPS is fast::RandomState's table of BuildHasher. */
const StdBuildHasherOps FOLDHASH_FAST_RANDOM_STATE_BUILD_HASHER_OPS = {
	foldhash_fast_RandomState_BuildHasher_build_hasher, sizeof(FoldhashFastFoldHasher),
	&FOLDHASH_FAST_FOLD_HASHER_HASHER_OPS
};
