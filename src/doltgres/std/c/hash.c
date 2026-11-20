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
 * The C forms of core's Hasher defaults and BuildHasher::hash_one, core's
 * SipHasher13 (core/src/hash/sip.rs at Rust 1.99.1), and std's RandomState
 * (std/src/hash/random.rs), whose keys come from the backend's pair, the
 * first from the operating system's randomness, each later one's k0 one
 * more. Rust's std is built without overflow checks, so its arithmetic
 * here is plain C's.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/std/hash.h"
#include "doltgres/std/unwind.h"

/* std_Hasher_write_u8 is Hasher's write_u8: the byte written. */
void
std_Hasher_write_u8(StdHasher self, uint8 i)
{
	self.ops->write(self.self, &i, 1);
}

/* std_Hasher_write_u16 is Hasher's write_u16: its bytes in the target's order written. */
void
std_Hasher_write_u16(StdHasher self, uint16 i)
{
	self.ops->write(self.self, (const uint8 *) &i, sizeof(i));
}

/* std_Hasher_write_u32 is Hasher's write_u32: its bytes in the target's order written. */
void
std_Hasher_write_u32(StdHasher self, uint32 i)
{
	self.ops->write(self.self, (const uint8 *) &i, sizeof(i));
}

/* std_Hasher_write_u64 is Hasher's write_u64: its bytes in the target's order written. */
void
std_Hasher_write_u64(StdHasher self, uint64 i)
{
	self.ops->write(self.self, (const uint8 *) &i, sizeof(i));
}

/* std_Hasher_write_u128 is Hasher's write_u128: its bytes in the target's order written, the low half first. */
void
std_Hasher_write_u128(StdHasher self, StdU128 i)
{
	uint8		bytes[16];

	memcpy(bytes, &i.lo, 8);
	memcpy(bytes + 8, &i.hi, 8);
	self.ops->write(self.self, bytes, sizeof(bytes));
}

/* std_Hasher_write_usize is Hasher's write_usize: its bytes in the target's order written. */
void
std_Hasher_write_usize(StdHasher self, size_t i)
{
	self.ops->write(self.self, (const uint8 *) &i, sizeof(i));
}

/* std_Hasher_write_i8 is Hasher's write_i8: write_u8 of its bits. */
void
std_Hasher_write_i8(StdHasher self, int8 i)
{
	self.ops->write_u8(self.self, (uint8) i);
}

/* std_Hasher_write_i16 is Hasher's write_i16: write_u16 of its bits. */
void
std_Hasher_write_i16(StdHasher self, int16 i)
{
	self.ops->write_u16(self.self, (uint16) i);
}

/* std_Hasher_write_i32 is Hasher's write_i32: write_u32 of its bits. */
void
std_Hasher_write_i32(StdHasher self, int32 i)
{
	self.ops->write_u32(self.self, (uint32) i);
}

/* std_Hasher_write_i64 is Hasher's write_i64: write_u64 of its bits. */
void
std_Hasher_write_i64(StdHasher self, int64 i)
{
	self.ops->write_u64(self.self, (uint64) i);
}

/* std_Hasher_write_i128 is Hasher's write_i128: write_u128 of its bits. */
void
std_Hasher_write_i128(StdHasher self, StdI128 i)
{
	StdU128		bits = {i.hi, i.lo};

	self.ops->write_u128(self.self, bits);
}

/* std_Hasher_write_isize is Hasher's write_isize: write_usize of its bits. */
void
std_Hasher_write_isize(StdHasher self, ssize_t i)
{
	self.ops->write_usize(self.self, (size_t) i);
}

/* std_Hasher_write_length_prefix is Hasher's write_length_prefix: write_usize of the length. */
void
std_Hasher_write_length_prefix(StdHasher self, size_t len)
{
	self.ops->write_usize(self.self, len);
}

/* std_Hasher_write_str is Hasher's write_str: the string's bytes, then write_u8 of 0xff. */
void
std_Hasher_write_str(StdHasher self, const char *s, size_t len)
{
	self.ops->write(self.self, (const uint8 *) s, len);
	self.ops->write_u8(self.self, 0xff);
}

/* std_BuildHasher_hash_one is BuildHasher's hash_one: a new hasher's finish after the value's hash. */
uint64
std_BuildHasher_hash_one(const StdBuildHasherOps *ops, const void *self, const void *x,
						 void (*hash) (const void *x, StdHasher state))
{
	union
	{
		uint8		bytes[STD_HASHER_SIZE];
		uint64		align;
	}			hasher;
	StdHasher	state = {ops->hasher, hasher.bytes};

	ops->build_hasher(self, hasher.bytes);
	hash(x, state);
	return ops->hasher->finish(hasher.bytes);
}

/* STD_SIP_COMPRESS is sip.rs's compress!, one SipRound of a state. */
#define STD_SIP_COMPRESS(s) \
	do { \
		(s).v0 = (s).v0 + (s).v1; \
		(s).v2 = (s).v2 + (s).v3; \
		(s).v1 = std_rotate_left_u64((s).v1, 13); \
		(s).v1 ^= (s).v0; \
		(s).v3 = std_rotate_left_u64((s).v3, 16); \
		(s).v3 ^= (s).v2; \
		(s).v0 = std_rotate_left_u64((s).v0, 32); \
		(s).v2 = (s).v2 + (s).v1; \
		(s).v0 = (s).v0 + (s).v3; \
		(s).v1 = std_rotate_left_u64((s).v1, 17); \
		(s).v1 ^= (s).v2; \
		(s).v3 = std_rotate_left_u64((s).v3, 21); \
		(s).v3 ^= (s).v0; \
		(s).v2 = std_rotate_left_u64((s).v2, 32); \
	} while (0)

/* std_sip_u8to64_le loads up to seven bytes of a buffer at an offset as a little-endian u64. */
static uint64
std_sip_u8to64_le(const uint8 *buf, size_t start, size_t len)
{
	size_t		i = 0;
	uint64		out = 0;

	if (i + 3 < len)
	{
		uint32		data;

		memcpy(&data, buf + start + i, 4);
		out = (uint64) data;
		i += 4;
	}
	if (i + 1 < len)
	{
		uint16		data;

		memcpy(&data, buf + start + i, 2);
		out |= (uint64) data << (i * 8);
		i += 2;
	}
	if (i < len)
	{
		out |= (uint64) buf[start + i] << (i * 8);
		i += 1;
	}
	return out;
}

/* std_SipHasher13_reset starts the hasher's state over from its keys. */
static void
std_SipHasher13_reset(StdSipHasher13 *self)
{
	self->length = 0;
	self->state.v0 = self->k0 ^ UINT64CONST(0x736f6d6570736575);
	self->state.v1 = self->k1 ^ UINT64CONST(0x646f72616e646f6d);
	self->state.v2 = self->k0 ^ UINT64CONST(0x6c7967656e657261);
	self->state.v3 = self->k1 ^ UINT64CONST(0x7465646279746573);
	self->ntail = 0;
}

/* std_SipHasher13_new_with_keys is SipHasher13::new_with_keys: a hasher under two keys. */
StdSipHasher13
std_SipHasher13_new_with_keys(uint64 key0, uint64 key1)
{
	StdSipHasher13 state = {key0, key1};

	std_SipHasher13_reset(&state);
	return state;
}

/* std_SipHasher13_Hasher_write writes bytes: whole words compressed, the rest kept as the tail. */
static void
std_SipHasher13_Hasher_write(void *self_, const uint8 *msg, size_t length)
{
	StdSipHasher13 *self = self_;
	size_t		needed = 0;
	size_t		len;
	size_t		left;
	size_t		i;

	self->length += length;
	if (self->ntail != 0)
	{
		needed = 8 - self->ntail;
		self->tail |= std_sip_u8to64_le(msg, 0, Min(length, needed)) << (8 * self->ntail);
		if (length < needed)
		{
			self->ntail += length;
			return;
		}
		else
		{
			self->state.v3 ^= self->tail;
			STD_SIP_COMPRESS(self->state);
			self->state.v0 ^= self->tail;
			self->ntail = 0;
		}
	}
	len = length - needed;
	left = len & 0x7;
	i = needed;
	while (i < len - left)
	{
		uint64		mi;

		memcpy(&mi, msg + i, 8);
		self->state.v3 ^= mi;
		STD_SIP_COMPRESS(self->state);
		self->state.v0 ^= mi;
		i += 8;
	}
	self->tail = std_sip_u8to64_le(msg, i, left);
	self->ntail = left;
}

/* std_SipHasher13_Hasher_write_u8 is Hasher's write_u8 for the hasher. */
static void
std_SipHasher13_Hasher_write_u8(void *self, uint8 i)
{
	StdHasher	hasher = {&STD_SIP_HASHER13_HASHER_OPS, self};

	std_Hasher_write_u8(hasher, i);
}

/* std_SipHasher13_Hasher_write_str is SipHasher13's write_str: the string's bytes, then 0xff. */
static void
std_SipHasher13_Hasher_write_str(void *self, const char *s, size_t len)
{
	std_SipHasher13_Hasher_write(self, (const uint8 *) s, len);
	std_SipHasher13_Hasher_write_u8(self, 0xFF);
}

/* std_SipHasher13_Hasher_finish returns the hash of what the hasher was given, leaving it as it is. */
static uint64
std_SipHasher13_Hasher_finish(const void *self_)
{
	const StdSipHasher13 *self = self_;
	StdSipState state = self->state;
	uint64		b = (((uint64) self->length & 0xff) << 56) | self->tail;

	state.v3 ^= b;
	STD_SIP_COMPRESS(state);
	state.v0 ^= b;
	state.v2 ^= 0xff;
	STD_SIP_COMPRESS(state);
	STD_SIP_COMPRESS(state);
	STD_SIP_COMPRESS(state);
	return state.v0 ^ state.v1 ^ state.v2 ^ state.v3;
}

/* std_SipHasher13_Hasher_write_u16 is Hasher's write_u16 for the hasher. */
static void
std_SipHasher13_Hasher_write_u16(void *self, uint16 i)
{
	StdHasher	hasher = {&STD_SIP_HASHER13_HASHER_OPS, self};

	std_Hasher_write_u16(hasher, i);
}

/* std_SipHasher13_Hasher_write_u32 is Hasher's write_u32 for the hasher. */
static void
std_SipHasher13_Hasher_write_u32(void *self, uint32 i)
{
	StdHasher	hasher = {&STD_SIP_HASHER13_HASHER_OPS, self};

	std_Hasher_write_u32(hasher, i);
}

/* std_SipHasher13_Hasher_write_u64 is Hasher's write_u64 for the hasher. */
static void
std_SipHasher13_Hasher_write_u64(void *self, uint64 i)
{
	StdHasher	hasher = {&STD_SIP_HASHER13_HASHER_OPS, self};

	std_Hasher_write_u64(hasher, i);
}

/* std_SipHasher13_Hasher_write_u128 is Hasher's write_u128 for the hasher. */
static void
std_SipHasher13_Hasher_write_u128(void *self, StdU128 i)
{
	StdHasher	hasher = {&STD_SIP_HASHER13_HASHER_OPS, self};

	std_Hasher_write_u128(hasher, i);
}

/* std_SipHasher13_Hasher_write_usize is Hasher's write_usize for the hasher. */
static void
std_SipHasher13_Hasher_write_usize(void *self, size_t i)
{
	StdHasher	hasher = {&STD_SIP_HASHER13_HASHER_OPS, self};

	std_Hasher_write_usize(hasher, i);
}

/* std_SipHasher13_Hasher_write_i8 is Hasher's write_i8 for the hasher. */
static void
std_SipHasher13_Hasher_write_i8(void *self, int8 i)
{
	StdHasher	hasher = {&STD_SIP_HASHER13_HASHER_OPS, self};

	std_Hasher_write_i8(hasher, i);
}

/* std_SipHasher13_Hasher_write_i16 is Hasher's write_i16 for the hasher. */
static void
std_SipHasher13_Hasher_write_i16(void *self, int16 i)
{
	StdHasher	hasher = {&STD_SIP_HASHER13_HASHER_OPS, self};

	std_Hasher_write_i16(hasher, i);
}

/* std_SipHasher13_Hasher_write_i32 is Hasher's write_i32 for the hasher. */
static void
std_SipHasher13_Hasher_write_i32(void *self, int32 i)
{
	StdHasher	hasher = {&STD_SIP_HASHER13_HASHER_OPS, self};

	std_Hasher_write_i32(hasher, i);
}

/* std_SipHasher13_Hasher_write_i64 is Hasher's write_i64 for the hasher. */
static void
std_SipHasher13_Hasher_write_i64(void *self, int64 i)
{
	StdHasher	hasher = {&STD_SIP_HASHER13_HASHER_OPS, self};

	std_Hasher_write_i64(hasher, i);
}

/* std_SipHasher13_Hasher_write_i128 is Hasher's write_i128 for the hasher. */
static void
std_SipHasher13_Hasher_write_i128(void *self, StdI128 i)
{
	StdHasher	hasher = {&STD_SIP_HASHER13_HASHER_OPS, self};

	std_Hasher_write_i128(hasher, i);
}

/* std_SipHasher13_Hasher_write_isize is Hasher's write_isize for the hasher. */
static void
std_SipHasher13_Hasher_write_isize(void *self, ssize_t i)
{
	StdHasher	hasher = {&STD_SIP_HASHER13_HASHER_OPS, self};

	std_Hasher_write_isize(hasher, i);
}

/* std_SipHasher13_Hasher_write_length_prefix is Hasher's write_length_prefix for the hasher. */
static void
std_SipHasher13_Hasher_write_length_prefix(void *self, size_t len)
{
	StdHasher	hasher = {&STD_SIP_HASHER13_HASHER_OPS, self};

	std_Hasher_write_length_prefix(hasher, len);
}

/* STD_SIP_HASHER13_HASHER_OPS is SipHasher13's table of Hasher. */
const StdHasherOps STD_SIP_HASHER13_HASHER_OPS = {
	.finish = std_SipHasher13_Hasher_finish,
	.write = std_SipHasher13_Hasher_write,
	.write_u8 = std_SipHasher13_Hasher_write_u8,
	.write_u16 = std_SipHasher13_Hasher_write_u16,
	.write_u32 = std_SipHasher13_Hasher_write_u32,
	.write_u64 = std_SipHasher13_Hasher_write_u64,
	.write_u128 = std_SipHasher13_Hasher_write_u128,
	.write_usize = std_SipHasher13_Hasher_write_usize,
	.write_i8 = std_SipHasher13_Hasher_write_i8,
	.write_i16 = std_SipHasher13_Hasher_write_i16,
	.write_i32 = std_SipHasher13_Hasher_write_i32,
	.write_i64 = std_SipHasher13_Hasher_write_i64,
	.write_i128 = std_SipHasher13_Hasher_write_i128,
	.write_isize = std_SipHasher13_Hasher_write_isize,
	.write_length_prefix = std_SipHasher13_Hasher_write_length_prefix,
	.write_str = std_SipHasher13_Hasher_write_str,
};

/* std_random_keys_set is whether the backend's pair of keys has been taken from the operating system. */
static bool std_random_keys_set;

/* std_random_keys is the backend's pair of keys, the next RandomState's: RandomState::new's KEYS. */
static StdRandomState std_random_keys;

/* std_hashmap_random_keys is sys::random's hashmap_random_keys: sixteen bytes of the operating system's randomness. */
static StdRandomState
std_hashmap_random_keys(void)
{
	uint8		buf[16];
	StdRandomState keys;

	if (!pg_strong_random(buf, sizeof(buf)))
	{
		static const char text[] = "failed to generate random data";

		std_panic(STD_RUST_LIBRARY STD_SEP "std" STD_SEP "src" STD_SEP "sys" STD_SEP "random" STD_SEP "windows.rs",
				  17, 9, text, sizeof(text) - 1);
	}
	memcpy(&keys.k0, buf, 8);
	memcpy(&keys.k1, buf + 8, 8);
	return keys;
}

/* std_RandomState_new is RandomState::new: the backend's keys, whose k0 the next one takes one more of. */
StdRandomState
std_RandomState_new(void)
{
	StdRandomState state;

	if (!std_random_keys_set)
	{
		std_random_keys = std_hashmap_random_keys();
		std_random_keys_set = true;
	}
	state = std_random_keys;
	std_random_keys.k0 = state.k0 + 1;
	return state;
}

/* std_RandomState_BuildHasher_build_hasher writes a SipHasher13 under the state's keys. */
static void
std_RandomState_BuildHasher_build_hasher(const void *self_, void *result)
{
	const StdRandomState *self = self_;

	*(StdSipHasher13 *) result = std_SipHasher13_new_with_keys(self->k0, self->k1);
}

StaticAssertDecl(sizeof(StdSipHasher13) <= STD_HASHER_SIZE, "RandomState's hasher fits a hasher's storage");

/* STD_RANDOM_STATE_BUILD_HASHER_OPS is RandomState's table of BuildHasher. */
const StdBuildHasherOps STD_RANDOM_STATE_BUILD_HASHER_OPS = {
	std_RandomState_BuildHasher_build_hasher, sizeof(StdSipHasher13), &STD_SIP_HASHER13_HASHER_OPS
};

#ifdef DOLTGRES_TEST_HOST
/* std_hash_tests_hash_tuple is the test's value's Hash, (u64, &str)'s: the u64, then the str. */
static void
std_hash_tests_hash_tuple(const void *x, StdHasher state)
{
	std_Hasher_write_u64(state, UINT64CONST(0x0123456789abcdef));
	std_Hasher_write_str(state, "a key", 5);
}

/*
 * test_std_hash_tests_random_state_hash_one_is_rusts: RandomState's
 * hash_one of (0x0123456789abcdef, "a key") with its keys at
 * 0x0706050403020100 and 0x0f0e0d0c0b0a0908 is 0x98ac2d92f8dd889e, as
 * rustc 1.99.1's SipHasher13 over those keys gives it.
 */
void
test_std_hash_tests_random_state_hash_one_is_rusts(void)
{
	StdRandomState state = {UINT64CONST(0x0706050403020100), UINT64CONST(0x0f0e0d0c0b0a0908)};
	uint64		hash = std_BuildHasher_hash_one(&STD_RANDOM_STATE_BUILD_HASHER_OPS, &state, NULL,
												std_hash_tests_hash_tuple);

	if (hash != UINT64CONST(0x98ac2d92f8dd889e))
		std_panic(__FILE__, __LINE__, 0, "hash_one differs from Rust's", sizeof("hash_one differs from Rust's") - 1);
}
#endif							/* DOLTGRES_TEST_HOST */
