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
 * The C forms of core's Hash, Hasher, and BuildHasher traits (the port
 * text's section 8) with their default methods, and of std's RandomState
 * and DefaultHasher, SipHash-1-3 under keys each RandomState takes from the
 * backend's pair, the first from the operating system's randomness.
 */
#ifndef DOLTGRES_STD_HASH_H
#define DOLTGRES_STD_HASH_H

#include "doltgres/std/int128.h"

/* StdHasherOps is Hasher's table, its methods in the trait's order. */
typedef struct StdHasherOps
{
	uint64		(*finish) (const void *self);
	void		(*write) (void *self, const uint8 *bytes, size_t len);
	void		(*write_u8) (void *self, uint8 i);
	void		(*write_u16) (void *self, uint16 i);
	void		(*write_u32) (void *self, uint32 i);
	void		(*write_u64) (void *self, uint64 i);
	void		(*write_u128) (void *self, StdU128 i);
	void		(*write_usize) (void *self, size_t i);
	void		(*write_i8) (void *self, int8 i);
	void		(*write_i16) (void *self, int16 i);
	void		(*write_i32) (void *self, int32 i);
	void		(*write_i64) (void *self, int64 i);
	void		(*write_i128) (void *self, StdI128 i);
	void		(*write_isize) (void *self, ssize_t i);
	void		(*write_length_prefix) (void *self, size_t len);
	void		(*write_str) (void *self, const char *s, size_t len);
} StdHasherOps;

/* StdHasher is a value of a type that implements Hasher: its table and the value. */
typedef struct StdHasher
{
	const StdHasherOps *ops;
	void	   *self;
} StdHasher;

/*
 * StdBuildHasherOps is BuildHasher's table: build_hasher, which writes a
 * new hasher of hasher_size bytes, and that hasher's table, its associated
 * type's.
 */
typedef struct StdBuildHasherOps
{
	void		(*build_hasher) (const void *self, void *result);
	size_t		hasher_size;
	const StdHasherOps *hasher;
} StdBuildHasherOps;

/* StdSipState is SipHash's state, its four words. */
typedef struct StdSipState
{
	uint64		v0;
	uint64		v2;
	uint64		v1;
	uint64		v3;
} StdSipState;

/* StdSipHasher13 is core's SipHasher13: its keys, its count of bytes, its state, and its unprocessed bytes. */
typedef struct StdSipHasher13
{
	uint64		k0;
	uint64		k1;
	size_t		length;
	StdSipState state;
	uint64		tail;
	size_t		ntail;
} StdSipHasher13;

/*
 * STD_HASHER_SIZE is the most bytes a hasher of the port takes,
 * SipHasher13's, the largest; each build-hasher table asserts that its
 * hasher fits.
 */
#define STD_HASHER_SIZE sizeof(StdSipHasher13)

/* StdRandomState is std's RandomState: the keys of its hashers. */
typedef struct StdRandomState
{
	uint64		k0;
	uint64		k1;
} StdRandomState;

extern const StdHasherOps STD_SIP_HASHER13_HASHER_OPS;
extern const StdBuildHasherOps STD_RANDOM_STATE_BUILD_HASHER_OPS;

extern void std_Hasher_write_u8(StdHasher self, uint8 i);
extern void std_Hasher_write_u16(StdHasher self, uint16 i);
extern void std_Hasher_write_u32(StdHasher self, uint32 i);
extern void std_Hasher_write_u64(StdHasher self, uint64 i);
extern void std_Hasher_write_u128(StdHasher self, StdU128 i);
extern void std_Hasher_write_usize(StdHasher self, size_t i);
extern void std_Hasher_write_i8(StdHasher self, int8 i);
extern void std_Hasher_write_i16(StdHasher self, int16 i);
extern void std_Hasher_write_i32(StdHasher self, int32 i);
extern void std_Hasher_write_i64(StdHasher self, int64 i);
extern void std_Hasher_write_i128(StdHasher self, StdI128 i);
extern void std_Hasher_write_isize(StdHasher self, ssize_t i);
extern void std_Hasher_write_length_prefix(StdHasher self, size_t len);
extern void std_Hasher_write_str(StdHasher self, const char *s, size_t len);
extern uint64 std_BuildHasher_hash_one(const StdBuildHasherOps *ops, const void *self, const void *x,
									   void (*hash) (const void *x, StdHasher state));

extern StdSipHasher13 std_SipHasher13_new_with_keys(uint64 key0, uint64 key1);
extern StdRandomState std_RandomState_new(void);

#ifdef DOLTGRES_TEST_HOST
extern void test_std_hash_tests_random_state_hash_one_is_rusts(void);
#endif

#endif							/* DOLTGRES_STD_HASH_H */
