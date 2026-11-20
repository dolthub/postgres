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
 * The template of core's flt2dec generics that hold the float by value (the
 * port text's section 2): FloatExt::integer_decode, decoder::decode, and
 * flt2dec's to_shortest_str, to_shortest_exp_str, to_exact_exp_str, and
 * to_exact_fixed_str at a float type T. An instance defines its parameters
 * and includes this file; every parameter is undefined at the end.
 *
 * STD_FLT2DEC_SCOPE is static for an instance one file alone uses, and
 * extern for one defined in one file and declared in others.
 * STD_FLT2DEC_DECLARE generates the instance's declarations, and
 * STD_FLT2DEC_DEFINE its definitions, which its declarations precede.
 *
 * STD_FLT2DEC_TS spells T, STD_FLT2DEC_T is T (an f16 held as its bits),
 * STD_FLT2DEC_TO_BITS(v) is T::to_bits as a uint64, and STD_FLT2DEC_BITS,
 * STD_FLT2DEC_SIG_BITS, and STD_FLT2DEC_EXP_BIAS are T's width, its stored
 * mantissa's bits, and its exponent's bias.
 */

/* STD_FLT2DEC_CAT pastes two tokens after expanding them. */
#define STD_FLT2DEC_CAT_(a, b) a##b
#define STD_FLT2DEC_CAT(a, b) STD_FLT2DEC_CAT_(a, b)

/* STD_FLT2DEC_DEF is STD_FLT2DEC_SCOPE's storage class for a definition: static, or none for extern. */
#define STD_FLT2DEC_DEF_static static
#define STD_FLT2DEC_DEF_extern
#define STD_FLT2DEC_DEF STD_FLT2DEC_CAT(STD_FLT2DEC_DEF_, STD_FLT2DEC_SCOPE)

/* STD_FLT2DEC_NAME is a generic's name at the instance's T. */
#define STD_FLT2DEC_NAME(name) STD_FLT2DEC_CAT(name, STD_FLT2DEC_TS)

#ifdef STD_FLT2DEC_DECLARE

STD_FLT2DEC_SCOPE uint64 STD_FLT2DEC_NAME(std_num_FloatExt_integer_decode) (STD_FLT2DEC_T self, int16 *result_1,
																			int8 *result_2);
STD_FLT2DEC_SCOPE bool STD_FLT2DEC_NAME(std_flt2dec_decoder_decode) (STD_FLT2DEC_T v, StdFullDecoded *result_1);
STD_FLT2DEC_SCOPE StdNumfmtFormatted STD_FLT2DEC_NAME(std_flt2dec_to_shortest_str) (StdFlt2decFormatShortestFn format_shortest,
																					void *format_shortest_captures,
																					void (*format_shortest_drop) (void *),
																					STD_FLT2DEC_T v, StdFlt2decSign sign,
																					size_t frac_digits, uint8 *buf,
																					size_t buf_len, StdNumfmtPart *parts,
																					size_t parts_len);
STD_FLT2DEC_SCOPE StdNumfmtFormatted STD_FLT2DEC_NAME(std_flt2dec_to_shortest_exp_str) (StdFlt2decFormatShortestFn format_shortest,
																						void *format_shortest_captures,
																						void (*format_shortest_drop) (void *),
																						STD_FLT2DEC_T v, StdFlt2decSign sign,
																						int16 dec_bounds_0, int16 dec_bounds_1,
																						bool upper, uint8 *buf, size_t buf_len,
																						StdNumfmtPart *parts, size_t parts_len);
STD_FLT2DEC_SCOPE StdNumfmtFormatted STD_FLT2DEC_NAME(std_flt2dec_to_exact_exp_str) (StdFlt2decFormatExactFn format_exact,
																					 void *format_exact_captures,
																					 void (*format_exact_drop) (void *),
																					 STD_FLT2DEC_T v, StdFlt2decSign sign,
																					 size_t frac_digits, bool upper,
																					 uint8 *buf, size_t buf_len,
																					 StdNumfmtPart *parts, size_t parts_len);
STD_FLT2DEC_SCOPE StdNumfmtFormatted STD_FLT2DEC_NAME(std_flt2dec_to_exact_fixed_str) (StdFlt2decFormatExactFn format_exact,
																					   void *format_exact_captures,
																					   void (*format_exact_drop) (void *),
																					   STD_FLT2DEC_T v, StdFlt2decSign sign,
																					   size_t frac_digits, uint8 *buf,
																					   size_t buf_len, StdNumfmtPart *parts,
																					   size_t parts_len);

#endif							/* STD_FLT2DEC_DECLARE */

#ifdef STD_FLT2DEC_DEFINE

/* STD_FLT2DEC_MOD_FILE is core's num/imp/flt2dec/mod.rs. */
#define STD_FLT2DEC_MOD_FILE STD_RUST_LIBRARY STD_SEP "core" STD_SEP "src" STD_SEP "num" STD_SEP "imp" STD_SEP "flt2dec" STD_SEP "mod.rs"

/* STD_FLT2DEC_TEMPLATE_PANIC is a panic of core's in mod.rs with a literal message. */
#define STD_FLT2DEC_TEMPLATE_PANIC(line, col, text) \
	std_panic(STD_FLT2DEC_MOD_FILE, (line), (col), (text), sizeof(text) - 1)

/* STD_FLT2DEC_PART_COPY is Part::Copy of a literal. */
#define STD_FLT2DEC_PART_COPY(literal) \
	((StdNumfmtPart) {.kind = STD_NUMFMT_PART_COPY, .u.copy = {(const uint8 *) (literal), sizeof(literal) - 1}})

/* STD_FLT2DEC_PART_ZERO is Part::Zero. */
#define STD_FLT2DEC_PART_ZERO(n) ((StdNumfmtPart) {.kind = STD_NUMFMT_PART_ZERO, .u.zero = (n)})

/* STD_FLT2DEC_FORMATTED is a Formatted of a sign and the first n parts. */
#define STD_FLT2DEC_FORMATTED(sign, parts, n) ((StdNumfmtFormatted) {(sign), (parts), (n)})

/*
 * std_num_FloatExt_integer_decode is FloatExt::integer_decode: the
 * mantissa, with its exponent in result_1 and its sign, 1 or -1, in
 * result_2.
 */
STD_FLT2DEC_DEF uint64
STD_FLT2DEC_NAME(std_num_FloatExt_integer_decode) (STD_FLT2DEC_T self, int16 *result_1, int8 *result_2)
{
	uint64		bits = STD_FLT2DEC_TO_BITS(self);
	uint64		sig_mask = (UINT64CONST(1) << STD_FLT2DEC_SIG_BITS) - 1;
	uint64		exp_mask = ((UINT64CONST(1) << (STD_FLT2DEC_BITS - 1)) - 1) & ~sig_mask;
	int8		sign = (bits >> (STD_FLT2DEC_BITS - 1)) == 0 ? 1 : -1;
	int16		exponent = (int16) ((bits & exp_mask) >> STD_FLT2DEC_SIG_BITS);
	uint64		mantissa;

	if (exponent == 0)
		mantissa = (bits & sig_mask) << 1;
	else
		mantissa = (bits & sig_mask) | (UINT64CONST(1) << STD_FLT2DEC_SIG_BITS);
	exponent = (int16) (exponent - (int16) (STD_FLT2DEC_EXP_BIAS + STD_FLT2DEC_SIG_BITS));
	*result_1 = exponent;
	*result_2 = sign;
	return mantissa;
}

/*
 * std_flt2dec_decoder_decode is decoder::decode: whether the value is
 * negative, with its decoded form in result_1, classified as T::classify
 * classifies its bits.
 */
STD_FLT2DEC_DEF bool
STD_FLT2DEC_NAME(std_flt2dec_decoder_decode) (STD_FLT2DEC_T v, StdFullDecoded *result_1)
{
	int16		exp;
	int8		sign;
	uint64		mant = STD_FLT2DEC_NAME(std_num_FloatExt_integer_decode) (v, &exp, &sign);
	bool		even = (mant & 1) == 0;
	uint64		bits = STD_FLT2DEC_TO_BITS(v);
	uint64		sig_mask = (UINT64CONST(1) << STD_FLT2DEC_SIG_BITS) - 1;
	uint64		exp_mask = ((UINT64CONST(1) << (STD_FLT2DEC_BITS - 1)) - 1) & ~sig_mask;

	memset(result_1, 0, sizeof(*result_1));
	if ((bits & exp_mask) == exp_mask)
		result_1->kind = (bits & sig_mask) == 0 ? STD_FULL_DECODED_INFINITE : STD_FULL_DECODED_NAN;
	else if ((bits & exp_mask) == 0 && (bits & sig_mask) == 0)
		result_1->kind = STD_FULL_DECODED_ZERO;
	else if ((bits & exp_mask) == 0)
	{
		result_1->kind = STD_FULL_DECODED_FINITE;
		result_1->finite.mant = mant;
		result_1->finite.minus = 1;
		result_1->finite.plus = 1;
		result_1->finite.exp = exp;
		result_1->finite.inclusive = even;
	}
	else
	{
		uint64		minnorm_0 = UINT64CONST(1) << STD_FLT2DEC_SIG_BITS;

		result_1->kind = STD_FULL_DECODED_FINITE;
		if (mant == minnorm_0)
		{
			result_1->finite.mant = mant << 2;
			result_1->finite.minus = 1;
			result_1->finite.plus = 2;
			result_1->finite.exp = (int16) (exp - 2);
			result_1->finite.inclusive = even;
		}
		else
		{
			result_1->finite.mant = mant << 1;
			result_1->finite.minus = 1;
			result_1->finite.plus = 1;
			result_1->finite.exp = (int16) (exp - 1);
			result_1->finite.inclusive = even;
		}
	}
	return sign < 0;
}

/*
 * std_flt2dec_to_shortest_str is flt2dec::to_shortest_str: the parts of v's
 * shortest digits in decimal with at least frac_digits fractional digits.
 */
STD_FLT2DEC_DEF StdNumfmtFormatted
STD_FLT2DEC_NAME(std_flt2dec_to_shortest_str) (StdFlt2decFormatShortestFn format_shortest,
											   void *format_shortest_captures,
											   void (*format_shortest_drop) (void *), STD_FLT2DEC_T v,
											   StdFlt2decSign sign, size_t frac_digits, uint8 *buf,
											   size_t buf_len, StdNumfmtPart *parts, size_t parts_len)
{
	volatile bool format_shortest_live = true;
	StdNumfmtFormatted result = {0};
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		StdFullDecoded full_decoded;
		bool		negative;
		StdBytes	sign_2;

		if (!(parts_len >= 4))
			STD_FLT2DEC_TEMPLATE_PANIC(369, 5, "assertion failed: parts.len() >= 4");
		if (!(buf_len >= STD_FLT2DEC_MAX_SIG_DIGITS))
			STD_FLT2DEC_TEMPLATE_PANIC(370, 5, "assertion failed: buf.len() >= MAX_SIG_DIGITS");
		negative = STD_FLT2DEC_NAME(std_flt2dec_decoder_decode) (v, &full_decoded);
		sign_2 = std_flt2dec_determine_sign(sign, &full_decoded, negative);
		switch (full_decoded.kind)
		{
			case STD_FULL_DECODED_NAN:
				std_index_check(0, parts_len, STD_FLT2DEC_MOD_FILE, 376, 13);
				parts[0] = STD_FLT2DEC_PART_COPY("NaN");
				std_range_to_check(1, parts_len, STD_FLT2DEC_MOD_FILE, 378, 52);
				result = STD_FLT2DEC_FORMATTED(sign_2, parts, 1);
				break;
			case STD_FULL_DECODED_INFINITE:
				std_index_check(0, parts_len, STD_FLT2DEC_MOD_FILE, 381, 13);
				parts[0] = STD_FLT2DEC_PART_COPY("inf");
				std_range_to_check(1, parts_len, STD_FLT2DEC_MOD_FILE, 383, 52);
				result = STD_FLT2DEC_FORMATTED(sign_2, parts, 1);
				break;
			case STD_FULL_DECODED_ZERO:
				if (frac_digits > 0)
				{
					std_index_check(0, parts_len, STD_FLT2DEC_MOD_FILE, 388, 17);
					parts[0] = STD_FLT2DEC_PART_COPY("0.");
					std_index_check(1, parts_len, STD_FLT2DEC_MOD_FILE, 389, 17);
					parts[1] = STD_FLT2DEC_PART_ZERO(frac_digits);
					std_range_to_check(2, parts_len, STD_FLT2DEC_MOD_FILE, 393, 42);
					result = STD_FLT2DEC_FORMATTED(sign_2, parts, 2);
				}
				else
				{
					std_index_check(0, parts_len, STD_FLT2DEC_MOD_FILE, 396, 17);
					parts[0] = STD_FLT2DEC_PART_COPY("0");
					std_range_to_check(1, parts_len, STD_FLT2DEC_MOD_FILE, 400, 42);
					result = STD_FLT2DEC_FORMATTED(sign_2, parts, 1);
				}
				break;
			case STD_FULL_DECODED_FINITE:
				{
					int16		exp;
					size_t		len = format_shortest(format_shortest_captures, &full_decoded.finite, buf, buf_len,
													  &exp);

					result.sign = sign_2;
					result.parts = parts;
					result.parts_len = std_flt2dec_digits_to_dec_str(buf, len, exp, frac_digits, parts, parts_len);
				}
				break;
		}
		format_shortest_live = false;
		if (format_shortest_drop != NULL)
			format_shortest_drop(format_shortest_captures);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (format_shortest_live && format_shortest_drop != NULL)
			format_shortest_drop(format_shortest_captures);
		std_unwind_resume(&unwind);
	}
	return result;
}

/*
 * std_flt2dec_to_shortest_exp_str is flt2dec::to_shortest_exp_str: the
 * parts of v's shortest digits in decimal when its exponent is in
 * dec_bounds, and in exponential form otherwise.
 */
STD_FLT2DEC_DEF StdNumfmtFormatted
STD_FLT2DEC_NAME(std_flt2dec_to_shortest_exp_str) (StdFlt2decFormatShortestFn format_shortest,
												   void *format_shortest_captures,
												   void (*format_shortest_drop) (void *), STD_FLT2DEC_T v,
												   StdFlt2decSign sign, int16 dec_bounds_0, int16 dec_bounds_1,
												   bool upper, uint8 *buf, size_t buf_len, StdNumfmtPart *parts,
												   size_t parts_len)
{
	volatile bool format_shortest_live = true;
	StdNumfmtFormatted result = {0};
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		StdFullDecoded full_decoded;
		bool		negative;
		StdBytes	sign_2;

		if (!(parts_len >= 6))
			STD_FLT2DEC_TEMPLATE_PANIC(444, 5, "assertion failed: parts.len() >= 6");
		if (!(buf_len >= STD_FLT2DEC_MAX_SIG_DIGITS))
			STD_FLT2DEC_TEMPLATE_PANIC(445, 5, "assertion failed: buf.len() >= MAX_SIG_DIGITS");
		if (!(dec_bounds_0 <= dec_bounds_1))
			STD_FLT2DEC_TEMPLATE_PANIC(446, 5, "assertion failed: dec_bounds.0 <= dec_bounds.1");
		negative = STD_FLT2DEC_NAME(std_flt2dec_decoder_decode) (v, &full_decoded);
		sign_2 = std_flt2dec_determine_sign(sign, &full_decoded, negative);
		switch (full_decoded.kind)
		{
			case STD_FULL_DECODED_NAN:
				std_index_check(0, parts_len, STD_FLT2DEC_MOD_FILE, 452, 13);
				parts[0] = STD_FLT2DEC_PART_COPY("NaN");
				std_range_to_check(1, parts_len, STD_FLT2DEC_MOD_FILE, 454, 52);
				result = STD_FLT2DEC_FORMATTED(sign_2, parts, 1);
				break;
			case STD_FULL_DECODED_INFINITE:
				std_index_check(0, parts_len, STD_FLT2DEC_MOD_FILE, 457, 13);
				parts[0] = STD_FLT2DEC_PART_COPY("inf");
				std_range_to_check(1, parts_len, STD_FLT2DEC_MOD_FILE, 459, 52);
				result = STD_FLT2DEC_FORMATTED(sign_2, parts, 1);
				break;
			case STD_FULL_DECODED_ZERO:
				{
					StdNumfmtPart part;

					if (dec_bounds_0 <= 0 && 0 < dec_bounds_1)
						part = STD_FLT2DEC_PART_COPY("0");
					else
						part = upper ? STD_FLT2DEC_PART_COPY("0E0") : STD_FLT2DEC_PART_COPY("0e0");
					std_index_check(0, parts_len, STD_FLT2DEC_MOD_FILE, 462, 13);
					parts[0] = part;
					std_range_to_check(1, parts_len, STD_FLT2DEC_MOD_FILE, 468, 52);
					result = STD_FLT2DEC_FORMATTED(sign_2, parts, 1);
				}
				break;
			case STD_FULL_DECODED_FINITE:
				{
					int16		exp;
					size_t		len = format_shortest(format_shortest_captures, &full_decoded.finite, buf, buf_len,
													  &exp);
					int32		vis_exp = (int32) exp - 1;
					size_t		n;

					if ((int32) dec_bounds_0 <= vis_exp && vis_exp < (int32) dec_bounds_1)
						n = std_flt2dec_digits_to_dec_str(buf, len, exp, 0, parts, parts_len);
					else
						n = std_flt2dec_digits_to_exp_str(buf, len, exp, 0, upper, parts, parts_len);
					result = STD_FLT2DEC_FORMATTED(sign_2, parts, n);
				}
				break;
		}
		format_shortest_live = false;
		if (format_shortest_drop != NULL)
			format_shortest_drop(format_shortest_captures);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (format_shortest_live && format_shortest_drop != NULL)
			format_shortest_drop(format_shortest_captures);
		std_unwind_resume(&unwind);
	}
	return result;
}

/*
 * std_flt2dec_to_exact_exp_str is flt2dec::to_exact_exp_str: the parts of
 * v's exact digits in exponential form with exactly frac_digits fractional
 * digits.
 */
STD_FLT2DEC_DEF StdNumfmtFormatted
STD_FLT2DEC_NAME(std_flt2dec_to_exact_exp_str) (StdFlt2decFormatExactFn format_exact, void *format_exact_captures,
												void (*format_exact_drop) (void *), STD_FLT2DEC_T v,
												StdFlt2decSign sign, size_t frac_digits, bool upper, uint8 *buf,
												size_t buf_len, StdNumfmtPart *parts, size_t parts_len)
{
	volatile bool format_exact_live = true;
	StdNumfmtFormatted result = {0};
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		StdFullDecoded full_decoded;
		bool		negative;
		StdBytes	sign_2;

		if (!(parts_len >= 6))
			STD_FLT2DEC_TEMPLATE_PANIC(538, 5, "assertion failed: parts.len() >= 6");
		negative = STD_FLT2DEC_NAME(std_flt2dec_decoder_decode) (v, &full_decoded);
		sign_2 = std_flt2dec_determine_sign(sign, &full_decoded, negative);
		switch (full_decoded.kind)
		{
			case STD_FULL_DECODED_NAN:
				std_index_check(0, parts_len, STD_FLT2DEC_MOD_FILE, 544, 13);
				parts[0] = STD_FLT2DEC_PART_COPY("NaN");
				std_range_to_check(1, parts_len, STD_FLT2DEC_MOD_FILE, 546, 52);
				result = STD_FLT2DEC_FORMATTED(sign_2, parts, 1);
				break;
			case STD_FULL_DECODED_INFINITE:
				std_index_check(0, parts_len, STD_FLT2DEC_MOD_FILE, 549, 13);
				parts[0] = STD_FLT2DEC_PART_COPY("inf");
				std_range_to_check(1, parts_len, STD_FLT2DEC_MOD_FILE, 551, 52);
				result = STD_FLT2DEC_FORMATTED(sign_2, parts, 1);
				break;
			case STD_FULL_DECODED_ZERO:
				if (frac_digits > 0)
				{
					std_index_check(0, parts_len, STD_FLT2DEC_MOD_FILE, 556, 17);
					parts[0] = STD_FLT2DEC_PART_COPY("0.");
					std_index_check(1, parts_len, STD_FLT2DEC_MOD_FILE, 557, 17);
					parts[1] = STD_FLT2DEC_PART_ZERO(frac_digits);
					std_index_check(2, parts_len, STD_FLT2DEC_MOD_FILE, 558, 17);
					parts[2] = upper ? STD_FLT2DEC_PART_COPY("E0") : STD_FLT2DEC_PART_COPY("e0");
					std_range_to_check(3, parts_len, STD_FLT2DEC_MOD_FILE, 562, 42);
					result = STD_FLT2DEC_FORMATTED(sign_2, parts, 3);
				}
				else
				{
					std_index_check(0, parts_len, STD_FLT2DEC_MOD_FILE, 565, 17);
					parts[0] = upper ? STD_FLT2DEC_PART_COPY("0E0") : STD_FLT2DEC_PART_COPY("0e0");
					std_range_to_check(1, parts_len, STD_FLT2DEC_MOD_FILE, 569, 42);
					result = STD_FLT2DEC_FORMATTED(sign_2, parts, 1);
				}
				break;
			case STD_FULL_DECODED_FINITE:
				{
					size_t		maxlen = std_flt2dec_estimate_max_buf_len(full_decoded.finite.exp);
					size_t		sig_digits = frac_digits < maxlen ? frac_digits + 1 : maxlen;
					int16		exp;
					size_t		len;

					if (!(buf_len >= sig_digits))
						STD_FLT2DEC_TEMPLATE_PANIC(579, 13, "assertion failed: buf.len() >= sig_digits");
					std_range_to_check(sig_digits, buf_len, STD_FLT2DEC_MOD_FILE, 581, 60);
					len = format_exact(format_exact_captures, &full_decoded.finite, buf, sig_digits, PG_INT16_MIN,
									   &exp);
					result.sign = sign_2;
					result.parts = parts;
					result.parts_len = std_flt2dec_digits_to_exp_str(buf, len, exp, frac_digits, upper, parts,
																	 parts_len);
				}
				break;
		}
		format_exact_live = false;
		if (format_exact_drop != NULL)
			format_exact_drop(format_exact_captures);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (format_exact_live && format_exact_drop != NULL)
			format_exact_drop(format_exact_captures);
		std_unwind_resume(&unwind);
	}
	return result;
}

/*
 * std_flt2dec_to_exact_fixed_str is flt2dec::to_exact_fixed_str: the parts
 * of v's exact digits in decimal with exactly frac_digits fractional
 * digits.
 */
STD_FLT2DEC_DEF StdNumfmtFormatted
STD_FLT2DEC_NAME(std_flt2dec_to_exact_fixed_str) (StdFlt2decFormatExactFn format_exact, void *format_exact_captures,
												  void (*format_exact_drop) (void *), STD_FLT2DEC_T v,
												  StdFlt2decSign sign, size_t frac_digits, uint8 *buf,
												  size_t buf_len, StdNumfmtPart *parts, size_t parts_len)
{
	volatile bool format_exact_live = true;
	StdNumfmtFormatted result = {0};
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		StdFullDecoded full_decoded;
		bool		negative;
		StdBytes	sign_2;

		if (!(parts_len >= 4))
			STD_FLT2DEC_TEMPLATE_PANIC(615, 5, "assertion failed: parts.len() >= 4");
		negative = STD_FLT2DEC_NAME(std_flt2dec_decoder_decode) (v, &full_decoded);
		sign_2 = std_flt2dec_determine_sign(sign, &full_decoded, negative);
		switch (full_decoded.kind)
		{
			case STD_FULL_DECODED_NAN:
				std_index_check(0, parts_len, STD_FLT2DEC_MOD_FILE, 621, 13);
				parts[0] = STD_FLT2DEC_PART_COPY("NaN");
				std_range_to_check(1, parts_len, STD_FLT2DEC_MOD_FILE, 623, 52);
				result = STD_FLT2DEC_FORMATTED(sign_2, parts, 1);
				break;
			case STD_FULL_DECODED_INFINITE:
				std_index_check(0, parts_len, STD_FLT2DEC_MOD_FILE, 626, 13);
				parts[0] = STD_FLT2DEC_PART_COPY("inf");
				std_range_to_check(1, parts_len, STD_FLT2DEC_MOD_FILE, 628, 52);
				result = STD_FLT2DEC_FORMATTED(sign_2, parts, 1);
				break;
			case STD_FULL_DECODED_ZERO:
				if (frac_digits > 0)
				{
					std_index_check(0, parts_len, STD_FLT2DEC_MOD_FILE, 633, 17);
					parts[0] = STD_FLT2DEC_PART_COPY("0.");
					std_index_check(1, parts_len, STD_FLT2DEC_MOD_FILE, 634, 17);
					parts[1] = STD_FLT2DEC_PART_ZERO(frac_digits);
					std_range_to_check(2, parts_len, STD_FLT2DEC_MOD_FILE, 638, 42);
					result = STD_FLT2DEC_FORMATTED(sign_2, parts, 2);
				}
				else
				{
					std_index_check(0, parts_len, STD_FLT2DEC_MOD_FILE, 641, 17);
					parts[0] = STD_FLT2DEC_PART_COPY("0");
					std_range_to_check(1, parts_len, STD_FLT2DEC_MOD_FILE, 645, 42);
					result = STD_FLT2DEC_FORMATTED(sign_2, parts, 1);
				}
				break;
			case STD_FULL_DECODED_FINITE:
				{
					size_t		maxlen = std_flt2dec_estimate_max_buf_len(full_decoded.finite.exp);
					int16		limit;
					int16		exp;
					size_t		len;

					if (!(buf_len >= maxlen))
						STD_FLT2DEC_TEMPLATE_PANIC(651, 13, "assertion failed: buf.len() >= maxlen");
					limit = frac_digits < 0x8000 ? (int16) -(int16) frac_digits : PG_INT16_MIN;
					std_range_to_check(maxlen, buf_len, STD_FLT2DEC_MOD_FILE, 657, 60);
					len = format_exact(format_exact_captures, &full_decoded.finite, buf, maxlen, limit, &exp);
					if (exp <= limit)
					{
						if (frac_digits > 0)
						{
							std_index_check(0, parts_len, STD_FLT2DEC_MOD_FILE, 665, 21);
							parts[0] = STD_FLT2DEC_PART_COPY("0.");
							std_index_check(1, parts_len, STD_FLT2DEC_MOD_FILE, 666, 21);
							parts[1] = STD_FLT2DEC_PART_ZERO(frac_digits);
							std_range_to_check(2, parts_len, STD_FLT2DEC_MOD_FILE, 670, 46);
							result = STD_FLT2DEC_FORMATTED(sign_2, parts, 2);
						}
						else
						{
							std_index_check(0, parts_len, STD_FLT2DEC_MOD_FILE, 673, 21);
							parts[0] = STD_FLT2DEC_PART_COPY("0");
							std_range_to_check(1, parts_len, STD_FLT2DEC_MOD_FILE, 677, 46);
							result = STD_FLT2DEC_FORMATTED(sign_2, parts, 1);
						}
					}
					else
					{
						result.sign = sign_2;
						result.parts = parts;
						result.parts_len = std_flt2dec_digits_to_dec_str(buf, len, exp, frac_digits, parts,
																		 parts_len);
					}
				}
				break;
		}
		format_exact_live = false;
		if (format_exact_drop != NULL)
			format_exact_drop(format_exact_captures);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (format_exact_live && format_exact_drop != NULL)
			format_exact_drop(format_exact_captures);
		std_unwind_resume(&unwind);
	}
	return result;
}

#undef STD_FLT2DEC_MOD_FILE
#undef STD_FLT2DEC_TEMPLATE_PANIC
#undef STD_FLT2DEC_PART_COPY
#undef STD_FLT2DEC_PART_ZERO
#undef STD_FLT2DEC_FORMATTED

#endif							/* STD_FLT2DEC_DEFINE */

#undef STD_FLT2DEC_CAT_
#undef STD_FLT2DEC_CAT
#undef STD_FLT2DEC_DEF_static
#undef STD_FLT2DEC_DEF_extern
#undef STD_FLT2DEC_DEF
#undef STD_FLT2DEC_NAME
#undef STD_FLT2DEC_SCOPE
#undef STD_FLT2DEC_DECLARE
#undef STD_FLT2DEC_DEFINE
#undef STD_FLT2DEC_TS
#undef STD_FLT2DEC_T
#undef STD_FLT2DEC_TO_BITS
#undef STD_FLT2DEC_BITS
#undef STD_FLT2DEC_SIG_BITS
#undef STD_FLT2DEC_EXP_BIAS
