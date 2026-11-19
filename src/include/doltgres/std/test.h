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
 * The C forms of a test's assertions (the port text's sections 6 and 13):
 * assert! panics with "assertion failed: " and the condition's source, and
 * assert_eq! and assert_ne! with core's message and both values' Debug, each
 * at the Rust's line of the file's RUST_FILE.
 */
#ifndef DOLTGRES_STD_TEST_H
#define DOLTGRES_STD_TEST_H

#include "doltgres/std/fmt.h"
#include "doltgres/std/unwind.h"

/* STD_ASSERT is assert!(cond) at a line and column, with the condition's source as rustc stringifies it. */
#define STD_ASSERT(line, col, cond, source) \
	do { \
		if (!(cond)) \
			std_panic(RUST_FILE, (line), (col), "assertion failed: " source, \
					  sizeof("assertion failed: " source) - 1); \
	} while (0)

/* STD_ASSERT_MSG is assert!(cond, "literal") at a line and column, its panic the literal message. */
#define STD_ASSERT_MSG(line, col, cond, literal) \
	do { \
		if (!(cond)) \
			std_panic(RUST_FILE, (line), (col), (literal), sizeof(literal) - 1); \
	} while (0)

/*
 * STD_ASSERT_EQ is assert_eq! at a line and column: eq is the values' comparison, and
 * left and right point at the values with their Debug.
 */
#define STD_ASSERT_EQ(line, col, eq, left, left_fmt, right, right_fmt) \
	do { \
		if (!(eq)) \
			std_assert_failed(RUST_FILE, (line), (col), "==", (left), (left_fmt), (right), (right_fmt), NULL); \
	} while (0)

/*
 * STD_ASSERT_EQ_MSG is assert_eq! with a literal message at a line and
 * column, the message core's assert_failed gets as Some(Arguments).
 */
#define STD_ASSERT_EQ_MSG(line, col, eq, left, left_fmt, right, right_fmt, literal) \
	do { \
		if (!(eq)) \
		{ \
			static const StdFmtPiece message_pieces_[] = {STD_FMT_LIT(literal)}; \
			StdFmtArguments message_ = {message_pieces_, 1, NULL}; \
			std_assert_failed(RUST_FILE, (line), (col), "==", (left), (left_fmt), (right), (right_fmt), &message_); \
		} \
	} while (0)

/* STD_ASSERT_EQ_U64 is assert_eq! of two u64s at a line and column. */
#define STD_ASSERT_EQ_U64(line, col, a, b) \
	do { \
		uint64		left_ = (a); \
		uint64		right_ = (b); \
		STD_ASSERT_EQ((line), (col), left_ == right_, &left_, std_u64_Debug_fmt, &right_, std_u64_Debug_fmt); \
	} while (0)

/* STD_ITEM is a value and its Debug, an item of a tuple or an Option to format. */
#define STD_ITEM(value, fmt) ((StdDebugRef) {(value), (fmt)})

/* STD_TUPLE is a tuple of n items to format, as a tuple's Debug sees it. */
#define STD_TUPLE(n, ...) ((StdTupleRef) {(n), {__VA_ARGS__}})

/*
 * StdTestsAligned64 is a value aligned to 64 bytes, past MAXIMUM_ALIGNOF, as
 * a cache-line-aligned record is, which the over-aligned tests of std's
 * forms hold.
 */
typedef struct pg_attribute_aligned (64)
StdTestsAligned64
{
	uint8		bytes[64];
} StdTestsAligned64;

extern void std_io__print(const StdFmtArguments *args);
extern void std_io__eprint(const StdFmtArguments *args);

#endif							/* DOLTGRES_STD_TEST_H */
