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
 * The C port of coretests' num/mod.rs helpers the flt2dec tests use (Rust
 * 1.99.1): ldexp at f16, f32, and f64, with the casts between f64 and an f16
 * held as its bits that `as` makes.
 */
#include "postgres.h"

#include <math.h>

#include "doltgres/std/num.h"
#include "doltgres/std/test_flt2dec.h"

/*
 * std_f64_as_f16 is `v as f16`: v rounded to the nearest f16, ties to even,
 * as its bits.
 */
uint16
std_f64_as_f16(float8 v)
{
	uint64		bits = std_f64_to_bits(v);
	uint16		sign = (uint16) ((bits >> 48) & 0x8000);
	int32		exp = (int32) ((bits >> 52) & 0x7ff);
	uint64		mant = bits & ((UINT64CONST(1) << 52) - 1);
	int32		e = exp - 1023 + 15;
	uint64		m;
	uint32		shift;
	uint64		q;
	uint64		rem;
	uint64		half;

	if (exp == 0x7ff)
		return (uint16) (sign | 0x7c00 | (mant != 0 ? 0x200 | (uint16) ((mant >> 42) & 0x3ff) : 0));
	if (e >= 0x1f)
		return (uint16) (sign | 0x7c00);
	if (e <= 0)
	{
		if (exp == 0)
			return sign;
		m = mant | (UINT64CONST(1) << 52);
		shift = (uint32) (1051 - exp);
		if (shift >= 64)
			return sign;
		q = m >> shift;
		rem = m & ((UINT64CONST(1) << shift) - 1);
		half = UINT64CONST(1) << (shift - 1);
		if (rem > half || (rem == half && (q & 1) != 0))
			q++;
		return (uint16) (sign | q);
	}
	q = ((uint64) e << 10) | (mant >> 42);
	rem = mant & ((UINT64CONST(1) << 42) - 1);
	half = UINT64CONST(1) << 41;
	if (rem > half || (rem == half && (q & 1) != 0))
		q++;
	return (uint16) (sign | q);
}

/* std_f16_as_f64 is `v as f64` of an f16 held as its bits, which is exact. */
float8
std_f16_as_f64(uint16 v)
{
	float8		sign = (v & 0x8000) != 0 ? -1.0 : 1.0;
	int32		exp = (v >> 10) & 0x1f;
	int32		mant = v & 0x3ff;

	if (exp == 0x1f)
		return mant == 0 ? sign * INFINITY : NAN;
	if (exp == 0)
		return sign * ldexp((float8) mant, -24);
	return sign * ldexp((float8) (mant | 0x400), exp - 25);
}

/* coretests_num_ldexp_f16 is num::ldexp_f16: a * 2^b, through f64. */
uint16
coretests_num_ldexp_f16(uint16 a, int32 b)
{
	return std_f64_as_f16(coretests_num_ldexp_f64(std_f16_as_f64(a), b));
}

/* coretests_num_ldexp_f32 is num::ldexp_f32: a * 2^b, through f64. */
float4
coretests_num_ldexp_f32(float4 a, int32 b)
{
	return (float4) coretests_num_ldexp_f64((float8) a, b);
}

/* coretests_num_ldexp_f64 is num::ldexp_f64: a * 2^b, by the C library's ldexp. */
float8
coretests_num_ldexp_f64(float8 a, int32 b)
{
	return ldexp(a, b);
}
