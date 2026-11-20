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
 * The C form of Rust's cmp::Ordering (the port text's section 4), a
 * fieldless enum with Rust's discriminants.
 */
#ifndef DOLTGRES_STD_CMP_H
#define DOLTGRES_STD_CMP_H

/* StdOrdering is cmp::Ordering. */
typedef enum StdOrdering
{
	STD_ORDERING_LESS = -1,
	STD_ORDERING_EQUAL = 0,
	STD_ORDERING_GREATER = 1,
} StdOrdering;

/* std_u32_PartialOrd_lt is u32's PartialOrd::lt, on pointers to the values. */
static inline bool
std_u32_PartialOrd_lt(const uint32 *self, const uint32 *other)
{
	return *self < *other;
}

/* std_i32_PartialOrd_lt is i32's PartialOrd::lt, on pointers to the values. */
static inline bool
std_i32_PartialOrd_lt(const int32 *self, const int32 *other)
{
	return *self < *other;
}

/* std_u64_PartialOrd_lt is u64's PartialOrd::lt, on pointers to the values. */
static inline bool
std_u64_PartialOrd_lt(const uint64 *self, const uint64 *other)
{
	return *self < *other;
}

/* std_i32_Ord_cmp is i32's Ord, on pointers to the values. */
static inline StdOrdering
std_i32_Ord_cmp(const void *a, const void *b)
{
	int32		x = *(const int32 *) a;
	int32		y = *(const int32 *) b;

	return x < y ? STD_ORDERING_LESS : (x > y ? STD_ORDERING_GREATER : STD_ORDERING_EQUAL);
}

/* std_u64_Ord_cmp is u64's Ord, on pointers to the values, as a map's code takes it. */
static inline StdOrdering
std_u64_Ord_cmp(const void *a, const void *b)
{
	uint64		x = *(const uint64 *) a;
	uint64		y = *(const uint64 *) b;

	return x < y ? STD_ORDERING_LESS : (x > y ? STD_ORDERING_GREATER : STD_ORDERING_EQUAL);
}

/*
 * std_bytes_Ord_cmp is [u8]'s Ord, core's SliceOrd for u8: the bytes the two
 * share compared, then their lengths.
 */
static inline StdOrdering
std_bytes_Ord_cmp(const uint8 *left, size_t left_len, const uint8 *right, size_t right_len)
{
	ssize_t		diff = (ssize_t) left_len - (ssize_t) right_len;
	size_t		len = left_len < right_len ? left_len : right_len;
	ssize_t		order = len == 0 ? 0 : memcmp(left, right, len);

	if (order == 0)
		order = diff;
	return order < 0 ? STD_ORDERING_LESS : (order > 0 ? STD_ORDERING_GREATER : STD_ORDERING_EQUAL);
}

#endif							/* DOLTGRES_STD_CMP_H */
