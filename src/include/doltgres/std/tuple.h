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
 * The tuples of std and primitive types alone (the port text's section 4):
 * one struct per Rust tuple type, named by its parts.
 */
#ifndef DOLTGRES_STD_TUPLE_H
#define DOLTGRES_STD_TUPLE_H

/* std_TupleUsizeUsize_t is a (usize, usize). */
typedef struct std_TupleUsizeUsize_t
{
	size_t		f0;
	size_t		f1;
} std_TupleUsizeUsize_t;

/* std_TupleU64U32U32_t is a (u64, u32, u32). */
typedef struct std_TupleU64U32U32_t
{
	uint64		f0;
	uint32		f1;
	uint32		f2;
} std_TupleU64U32U32_t;

/* std_TupleU32U32_t is a (u32, u32). */
typedef struct std_TupleU32U32_t
{
	uint32		f0;
	uint32		f1;
} std_TupleU32U32_t;

/* std_TupleU64U32_t is a (u64, u32). */
typedef struct std_TupleU64U32_t
{
	uint64		f0;
	uint32		f1;
} std_TupleU64U32_t;

/* std_TupleU64U64_t is a (u64, u64). */
typedef struct std_TupleU64U64_t
{
	uint64		f0;
	uint64		f1;
} std_TupleU64U64_t;

/* std_TupleU32U64_t is a (u32, u64). */
typedef struct std_TupleU32U64_t
{
	uint32		f0;
	uint64		f1;
} std_TupleU32U64_t;

/* std_TupleU32U16U16U64_t is a (u32, u16, u16, u64). */
typedef struct std_TupleU32U16U16U64_t
{
	uint32		f0;
	uint16		f1;
	uint16		f2;
	uint64		f3;
} std_TupleU32U16U16U64_t;

/* std_TupleU32RefSliceU8_t is a (u32, &[u8]): the number and the slice's bytes and length. */
typedef struct std_TupleU32RefSliceU8_t
{
	uint32		f0;
	const uint8 *f1;
	size_t		f1_len;
} std_TupleU32RefSliceU8_t;

/* std_TupleUsizeU32Usize_t is a (usize, u32, usize). */
typedef struct std_TupleUsizeU32Usize_t
{
	size_t		f0;
	uint32		f1;
	size_t		f2;
} std_TupleUsizeU32Usize_t;

#endif							/* DOLTGRES_STD_TUPLE_H */
