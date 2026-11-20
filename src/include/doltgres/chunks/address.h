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
 * Chunk addresses: the BLAKE3 hash of a chunk's hashed bytes, written as 52
 * base32 characters (chunks' address.rs).
 */
#ifndef DOLTGRES_CHUNKS_ADDRESS_H
#define DOLTGRES_CHUNKS_ADDRESS_H

#include "doltgres/std/foldhash.h"
#include "doltgres/std/cmp.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/hash.h"
#include "doltgres/std/str.h"

/* CHUNKS_ADDRESS_TEXT_LEN is the length of an address written out. */
#define CHUNKS_ADDRESS_TEXT_LEN 52

/* CHUNKS_ADDRESS_ADDRESS_LEN is an address's length in bytes. */
#define CHUNKS_ADDRESS_ADDRESS_LEN 32

/* ChunksAddressAddress names a chunk by the BLAKE3 hash of its hashed bytes. */
typedef struct ChunksAddressAddress
{
	uint8		f0[32];
} ChunksAddressAddress;

/* chunks_TupleAddressAddress_t is the tuple (Address, Address). */
typedef struct chunks_TupleAddressAddress_t
{
	ChunksAddressAddress f0;
	ChunksAddressAddress f1;
} chunks_TupleAddressAddress_t;

/* chunks_TupleAddressOptionAddressU64_t is the tuple (Address, Option<Address>, u64). */
typedef struct chunks_TupleAddressOptionAddressU64_t
{
	ChunksAddressAddress f0;
	bool		has_f1;
	ChunksAddressAddress f1;
	uint64		f2;
} chunks_TupleAddressOptionAddressU64_t;

/* chunks_TupleAddressU64_t is the tuple (Address, u64): an address and a number beside it. */
typedef struct chunks_TupleAddressU64_t
{
	ChunksAddressAddress f0;
	uint64		f1;
} chunks_TupleAddressU64_t;

/* ChunksAddressAddressState is AddressState, foldhash's RandomState: the hasher of maps keyed by address. */
typedef FoldhashFastRandomState ChunksAddressAddressState;

extern ChunksAddressAddress chunks_address_Address_of(const uint8 *bytes,
													  size_t bytes_len);
extern bool chunks_address_Address_before(const ChunksAddressAddress *self,
										  const ChunksAddressAddress *other);
extern bool chunks_address_Address_same(const ChunksAddressAddress *self,
										const ChunksAddressAddress *other);
extern void chunks_address_Address_Hash_hash(const void *self_, StdHasher state);
extern bool chunks_address_Address_is_empty(const ChunksAddressAddress *self);
extern bool chunks_address_Address_parse(const char *text, size_t text_len,
										 ChunksAddressAddress *result);
extern bool chunks_address_Address_prefix_range(const char *prefix,
												size_t prefix_len,
												chunks_TupleAddressAddress_t *result);
extern bool chunks_address_Address_has_prefix(const ChunksAddressAddress *self,
											  const char *prefix,
											  size_t prefix_len);
extern bool chunks_address_Address_Display_fmt(const void *self,
											   StdFormatter *f);
extern bool chunks_address_Address_Debug_fmt(const void *self,
											 StdFormatter *f);
extern bool chunks_address_Address_PartialEq_eq(const void *self_, const void *other_);
extern bool chunks_address_Address_PartialEq_ne(const void *self, const void *other);
extern bool chunks_address_Address_PartialOrd_partial_cmp(const void *self_, const void *other_,
														  StdOrdering *result);
extern bool chunks_address_Address_PartialOrd_lt(const void *self, const void *other);
extern bool chunks_address_Address_PartialOrd_le(const void *self, const void *other);
extern bool chunks_address_Address_PartialOrd_ge(const void *self, const void *other);
extern StdOrdering chunks_address_Address_Ord_cmp(const void *self_, const void *other_);

extern void test_chunks_address_tests_text_round_trips(void);
extern void test_chunks_address_tests_hashes_with_blake3(void);

#endif							/* DOLTGRES_CHUNKS_ADDRESS_H */
