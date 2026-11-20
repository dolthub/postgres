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
 * The header every chunk's hashed bytes start with: its kind, flags, and a
 * table of where the addresses it refers to are, so that garbage collection,
 * pushes, and integrity checks can follow any chunk's references without
 * knowing its kind (chunks' header.rs).
 */
#ifndef DOLTGRES_CHUNKS_HEADER_H
#define DOLTGRES_CHUNKS_HEADER_H

#include "doltgres/chunks/address.h"
#include "doltgres/chunks/error.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/vec.h"

/*
 * ChunksHeaderKind says what a chunk holds. The values below 64 belong to
 * chunks and the tree format. Records take the rest, and the layers that
 * write them assign their meanings.
 */
typedef uint8 ChunksHeaderKind;

/* CHUNKS_HEADER_TREE_LEAF is the kind of a tree's leaf node. */
#define CHUNKS_HEADER_TREE_LEAF 1

/* CHUNKS_HEADER_TREE_INTERNAL is the kind of a tree's internal node. */
#define CHUNKS_HEADER_TREE_INTERNAL 2

/* CHUNKS_HEADER_BLOB_LEAF is the kind of a blob tree's leaf, which holds raw bytes. */
#define CHUNKS_HEADER_BLOB_LEAF 3

/* CHUNKS_HEADER_FIRST_RECORD is the first kind that a record can have. */
#define CHUNKS_HEADER_FIRST_RECORD 64

/* ChunksHeaderAddressesTag is an Addresses' variant. */
typedef enum ChunksHeaderAddressesTag
{
	CHUNKS_HEADER_ADDRESSES_OFFSETS,
	CHUNKS_HEADER_ADDRESSES_STRIDE,
} ChunksHeaderAddressesTag;

/* ChunksHeaderAddresses says where a chunk's addresses are in its body, the hashed bytes after the header. */
typedef struct ChunksHeaderAddresses
{
	ChunksHeaderAddressesTag tag;
	union
	{
		/* offsets is an offset per address, a Vec<u32>. */
		StdVec		offsets;

		/*
		 * stride is count addresses, the first at first and each after it
		 * stride bytes further.
		 */
		struct
		{
			uint32		first;
			uint32		stride;
			uint32		count;
		}			stride;
	}			u;
} ChunksHeaderAddresses;

/* ChunksHeaderHeader is a chunk's header. */
typedef struct ChunksHeaderHeader
{
	ChunksHeaderKind kind;
	/* flags are bits whose meaning depends on the kind. */
	uint8		flags;
	ChunksHeaderAddresses addresses;
} ChunksHeaderHeader;

extern bool chunks_header_VecU32_Debug_fmt(const void *value, StdFormatter *f);
extern bool chunks_header_Addresses_Debug_fmt(const void *value, StdFormatter *f);
extern bool chunks_header_Addresses_PartialEq_eq(const ChunksHeaderAddresses *self,
												 const ChunksHeaderAddresses *other);
extern void chunks_header_Addresses_drop(ChunksHeaderAddresses *self);
extern size_t chunks_header_Addresses_len(const ChunksHeaderAddresses *self);
extern bool chunks_header_Addresses_is_empty(const ChunksHeaderAddresses *self);
extern size_t chunks_header_Addresses_offset(const ChunksHeaderAddresses *self, size_t i);
extern bool chunks_header_Header_Debug_fmt(const void *value, StdFormatter *f);
extern bool chunks_header_Header_PartialEq_eq(const ChunksHeaderHeader *self, const ChunksHeaderHeader *other);
extern void chunks_header_Header_drop(ChunksHeaderHeader *self);
extern void chunks_header_Header_write(const ChunksHeaderHeader *self, const uint8 *body, size_t body_len,
									   StdVec *result);
extern bool chunks_header_Header_read(const uint8 *bytes, size_t bytes_len, ChunksHeaderHeader *result_0,
									  size_t *result_1, ChunksErrorError *error);
extern bool chunks_header_addresses(const uint8 *bytes, size_t bytes_len, StdVec *result,
									ChunksErrorError *error);

extern void test_chunks_header_tests_finds_addresses_of_either_form(void);
extern void test_chunks_header_tests_refuses_more_addresses_than_the_body_holds(void);

#endif							/* DOLTGRES_CHUNKS_HEADER_H */
