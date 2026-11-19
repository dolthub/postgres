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
 * The words of datums of types passed by value: each reader and writer is
 * the C of the macro of postgres.h its comment names, over the 64 bits a
 * Datum is on every platform. A word never leaves its process; what the fork
 * compares, hashes, or stores of a value is what a reader gives, never the
 * word.
 */
#ifndef FRONTEND
#include "postgres.h"

#include "doltgres/pg/word.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/num.h"
#include "doltgres/std/test.h"

#define RUST_FILE "pg" STD_SEP "src" STD_SEP "word.rs"

/* pg_word_to_bool reads a word as DatumGetBool does. */
bool
pg_word_to_bool(uint64 word)
{
	return word != 0;
}

/*
 * pg_word_to_char reads a word's byte as DatumGetChar does, the only form of
 * a char value the fork's own code compares, hashes, or stores, since the
 * word's other bits follow the platform's char.
 */
uint8
pg_word_to_char(uint64 word)
{
	return (uint8) word;
}

/* pg_word_to_int16 reads a word as DatumGetInt16 does. */
int16
pg_word_to_int16(uint64 word)
{
	return (int16) word;
}

/* pg_word_to_int32 reads a word as DatumGetInt32 does. */
int32
pg_word_to_int32(uint64 word)
{
	return (int32) word;
}

/* pg_word_to_int64 reads a word as DatumGetInt64 does. */
int64
pg_word_to_int64(uint64 word)
{
	return (int64) word;
}

/* pg_word_to_oid reads a word as DatumGetObjectId does. */
uint32
pg_word_to_oid(uint64 word)
{
	return (uint32) word;
}

/* pg_word_to_float4 reads a word as DatumGetFloat4 does, the float whose bits its low 32 bits are. */
float4
pg_word_to_float4(uint64 word)
{
	return std_f32_from_bits((uint32) word);
}

/* pg_word_to_float8 reads a word as DatumGetFloat8 does, the float whose bits it is. */
float8
pg_word_to_float8(uint64 word)
{
	return std_f64_from_bits(word);
}

/* pg_word_from_bool returns the word BoolGetDatum makes. */
uint64
pg_word_from_bool(bool value)
{
	return (uint64) value;
}

/*
 * pg_word_from_char returns the word CharGetDatum makes of a byte on this
 * platform, whose char is signed or not as C's is, so that Postgres, which
 * compares whole words, finds it equal to its own. Nothing stored holds the
 * word.
 */
uint64
pg_word_from_char(uint8 value)
{
	return (uint64) (char) value;
}

/* pg_word_from_int16 returns the word Int16GetDatum makes. */
uint64
pg_word_from_int16(int16 value)
{
	return (uint64) value;
}

/* pg_word_from_int32 returns the word Int32GetDatum makes. */
uint64
pg_word_from_int32(int32 value)
{
	return (uint64) value;
}

/* pg_word_from_int64 returns the word Int64GetDatum makes. */
uint64
pg_word_from_int64(int64 value)
{
	return (uint64) value;
}

/* pg_word_from_oid returns the word ObjectIdGetDatum makes. */
uint64
pg_word_from_oid(uint32 value)
{
	return (uint64) value;
}

#ifdef DOLTGRES_TEST_PG

/*
 * test_pg_word_tests_words_read_back_as_they_were_written: each writer's word
 * reads back as its value, a negative integer's word has every high bit set
 * as a C cast to Datum gives it, and a reader takes only its type's low bits.
 */
void
test_pg_word_tests_words_read_back_as_they_were_written(void)
{
	{
		uint64		tmp_1 = pg_word_from_bool(true);
		bool		tmp_2 = pg_word_to_bool(tmp_1);

		if (tmp_2)
		{
			uint64		tmp_3 = pg_word_from_bool(false);

			tmp_2 = !pg_word_to_bool(tmp_3);
		}
		STD_ASSERT(99, 9, tmp_2, "to_bool(from_bool(true)) && !to_bool(from_bool(false))");
	}
	{
		uint64		tmp_1 = pg_word_from_char(0xE9);
		uint8		left = pg_word_to_char(tmp_1);
		uint8		right = 0xE9;

		STD_ASSERT_EQ(100, 9, left == right, &left, std_u8_Debug_fmt, &right, std_u8_Debug_fmt);
	}
	{
		uint64		tmp_1 = pg_word_from_int16(-2);
		int16		left = pg_word_to_int16(tmp_1);
		int16		right = -2;

		STD_ASSERT_EQ(101, 9, left == right, &left, std_i16_Debug_fmt, &right, std_i16_Debug_fmt);
	}
	{
		uint64		left = pg_word_from_int16(-2);
		uint64		right = (uint64) (PG_UINT64_MAX - 1);

		STD_ASSERT_EQ(102, 9, left == right, &left, std_u64_Debug_fmt, &right, std_u64_Debug_fmt);
	}
	{
		uint64		tmp_1 = pg_word_from_int32(PG_INT32_MIN);
		int32		left = pg_word_to_int32(tmp_1);
		int32		right = PG_INT32_MIN;

		STD_ASSERT_EQ(103, 9, left == right, &left, std_i32_Debug_fmt, &right, std_i32_Debug_fmt);
	}
	{
		uint64		left = pg_word_from_int32(-1);
		uint64		right = PG_UINT64_MAX;

		STD_ASSERT_EQ(104, 9, left == right, &left, std_u64_Debug_fmt, &right, std_u64_Debug_fmt);
	}
	{
		uint64		tmp_1 = pg_word_from_int64(PG_INT64_MIN);
		int64		left = pg_word_to_int64(tmp_1);
		int64		right = PG_INT64_MIN;

		STD_ASSERT_EQ(105, 9, left == right, &left, std_i64_Debug_fmt, &right, std_i64_Debug_fmt);
	}
	{
		uint64		left = pg_word_from_oid(PG_UINT32_MAX);
		uint64		right = UINT64CONST(0xFFFFFFFF);

		STD_ASSERT_EQ(106, 9, left == right, &left, std_u64_Debug_fmt, &right, std_u64_Debug_fmt);
	}
	{
		uint64		tmp_1 = pg_word_from_oid(16384);
		uint32		left = pg_word_to_oid(tmp_1);
		uint32		right = 16384;

		STD_ASSERT_EQ(107, 9, left == right, &left, std_u32_Debug_fmt, &right, std_u32_Debug_fmt);
	}
	{
		int32		left = pg_word_to_int32(UINT64CONST(0x100000005));
		int32		right = 5;

		STD_ASSERT_EQ(108, 9, left == right, &left, std_i32_Debug_fmt, &right, std_i32_Debug_fmt);
	}
	{
		uint32		tmp_1 = std_f32_to_bits(1.5f);
		uint64		tmp_2 = UINT64CONST(0xFFFFFFFF00000000) | (uint64) tmp_1;
		float4		left = pg_word_to_float4(tmp_2);
		float4		right = 1.5f;

		STD_ASSERT_EQ(109, 9, left == right, &left, std_f32_Debug_fmt, &right, std_f32_Debug_fmt);
	}
	{
		uint64		tmp_1 = std_f64_to_bits(-0.25);
		float8		left = pg_word_to_float8(tmp_1);
		float8		right = -0.25;

		STD_ASSERT_EQ(110, 9, left == right, &left, std_f64_Debug_fmt, &right, std_f64_Debug_fmt);
	}
}

#endif							/* DOLTGRES_TEST_PG */

#endif							/* FRONTEND */
