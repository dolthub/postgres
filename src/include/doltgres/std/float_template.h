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
 * The template of core's fmt/float.rs generics that hold the float by value
 * (the port text's section 2): the decimal and exponential formatting that
 * float_to_general_debug, a float's Debug, chooses between, at a float type T
 * whose flt2dec instance (flt2dec_template.h) has the same spelling. An
 * instance defines its parameters and includes this file; every parameter is
 * undefined at the end.
 *
 * STD_FLOAT_SCOPE is static for an instance one file alone uses, and extern
 * for one defined in one file and declared in others. STD_FLOAT_DECLARE
 * generates the instance's declarations, and STD_FLOAT_DEFINE its
 * definitions, which its declarations precede. STD_FLOAT_TS spells T,
 * STD_FLOAT_T is T (an f16 held as its bits), and
 * STD_FLOAT_GENERAL_FORMAT is T's GeneralFormat test, a function of a
 * const T * that the instance's file defines. STD_FLOAT_DISPLAY generates
 * float_to_decimal_display too, for a T whose Display the crates use.
 */

/* STD_FLOAT_CAT pastes two tokens after expanding them. */
#define STD_FLOAT_CAT_(a, b) a##b
#define STD_FLOAT_CAT(a, b) STD_FLOAT_CAT_(a, b)

/* STD_FLOAT_DEF is STD_FLOAT_SCOPE's storage class for a definition: static, or none for extern. */
#define STD_FLOAT_DEF_static static
#define STD_FLOAT_DEF_extern
#define STD_FLOAT_DEF STD_FLOAT_CAT(STD_FLOAT_DEF_, STD_FLOAT_SCOPE)

/* STD_FLOAT_NAME is a generic's name at the instance's T. */
#define STD_FLOAT_NAME(name) STD_FLOAT_CAT(name, STD_FLOAT_TS)

#ifdef STD_FLOAT_DECLARE

STD_FLOAT_SCOPE bool STD_FLOAT_NAME(std_fmt_float_float_to_general_debug) (StdFormatter *fmt, const STD_FLOAT_T * num);
#ifdef STD_FLOAT_DISPLAY
STD_FLOAT_SCOPE bool STD_FLOAT_NAME(std_fmt_float_float_to_decimal_display) (StdFormatter *fmt, const STD_FLOAT_T * num);
#endif

#endif							/* STD_FLOAT_DECLARE */

#ifdef STD_FLOAT_DEFINE

/* std_fmt_float_float_to_decimal_common_exact is float_to_decimal_common_exact: num in decimal at a precision. */
static bool
STD_FLOAT_NAME(std_fmt_float_float_to_decimal_common_exact) (StdFormatter *fmt, const STD_FLOAT_T * num,
															 StdFlt2decSign sign, uint16 precision)
{
	uint8		buf[1024];
	StdNumfmtPart parts[4];
	StdNumfmtFormatted formatted;

	formatted = STD_FLOAT_NAME(std_flt2dec_to_exact_fixed_str) (std_flt2dec_strategy_grisu_format_exact_FnMut_call_mut,
																NULL, NULL, *num, sign, (size_t) precision, buf,
																lengthof(buf), parts, lengthof(parts));
	return std_fmt_pad_formatted_parts(fmt, &formatted);
}

/*
 * std_fmt_float_float_to_decimal_common_shortest is
 * float_to_decimal_common_shortest: num's shortest digits in decimal with at
 * least precision fractional digits.
 */
static bool
STD_FLOAT_NAME(std_fmt_float_float_to_decimal_common_shortest) (StdFormatter *fmt, const STD_FLOAT_T * num,
																StdFlt2decSign sign, uint16 precision)
{
	uint8		buf[STD_FLT2DEC_MAX_SIG_DIGITS];
	StdNumfmtPart parts[4];
	StdNumfmtFormatted formatted;

	formatted = STD_FLOAT_NAME(std_flt2dec_to_shortest_str) (std_flt2dec_strategy_grisu_format_shortest_FnMut_call_mut,
															 NULL, NULL, *num, sign, (size_t) precision, buf,
															 lengthof(buf), parts, lengthof(parts));
	return std_fmt_pad_formatted_parts(fmt, &formatted);
}

/*
 * std_fmt_float_float_to_exponential_common_shortest is
 * float_to_exponential_common_shortest: num's shortest digits in exponential
 * form, `E` for the exponent when upper.
 */
static bool
STD_FLOAT_NAME(std_fmt_float_float_to_exponential_common_shortest) (StdFormatter *fmt, const STD_FLOAT_T * num,
																	StdFlt2decSign sign, bool upper)
{
	uint8		buf[STD_FLT2DEC_MAX_SIG_DIGITS];
	StdNumfmtPart parts[6];
	StdNumfmtFormatted formatted;

	formatted = STD_FLOAT_NAME(std_flt2dec_to_shortest_exp_str) (std_flt2dec_strategy_grisu_format_shortest_FnMut_call_mut,
																 NULL, NULL, *num, sign, 0, 0, upper, buf,
																 lengthof(buf), parts, lengthof(parts));
	return std_fmt_pad_formatted_parts(fmt, &formatted);
}

/*
 * std_fmt_float_float_to_general_debug is float_to_general_debug: a float's
 * Debug, in decimal at a precision when the formatter has one, and otherwise
 * by its shortest digits, in exponential form when its magnitude asks for it.
 */
STD_FLOAT_DEF bool
STD_FLOAT_NAME(std_fmt_float_float_to_general_debug) (StdFormatter *fmt, const STD_FLOAT_T * num)
{
	bool		force_sign = fmt->options.sign_plus;
	StdFlt2decSign sign = force_sign ? STD_FLT2DEC_SIGN_MINUS_PLUS : STD_FLT2DEC_SIGN_MINUS;

	if (fmt->options.has_precision)
		return STD_FLOAT_NAME(std_fmt_float_float_to_decimal_common_exact) (fmt, num, sign, fmt->options.precision);
	else
	{
		if (STD_FLOAT_GENERAL_FORMAT(num))
		{
			bool		upper = false;

			return STD_FLOAT_NAME(std_fmt_float_float_to_exponential_common_shortest) (fmt, num, sign, upper);
		}
		else
		{
			uint16		min_precision = 1;

			return STD_FLOAT_NAME(std_fmt_float_float_to_decimal_common_shortest) (fmt, num, sign, min_precision);
		}
	}
}

#ifdef STD_FLOAT_DISPLAY

/*
 * std_fmt_float_float_to_decimal_display is float_to_decimal_display: a
 * float's Display, in decimal at a precision when the formatter has one, and
 * otherwise by its shortest digits.
 */
STD_FLOAT_DEF bool
STD_FLOAT_NAME(std_fmt_float_float_to_decimal_display) (StdFormatter *fmt, const STD_FLOAT_T * num)
{
	bool		force_sign = fmt->options.sign_plus;
	StdFlt2decSign sign = force_sign ? STD_FLT2DEC_SIGN_MINUS_PLUS : STD_FLT2DEC_SIGN_MINUS;

	if (fmt->options.has_precision)
		return STD_FLOAT_NAME(std_fmt_float_float_to_decimal_common_exact) (fmt, num, sign, fmt->options.precision);
	else
	{
		uint16		min_precision = 0;

		return STD_FLOAT_NAME(std_fmt_float_float_to_decimal_common_shortest) (fmt, num, sign, min_precision);
	}
}

#endif							/* STD_FLOAT_DISPLAY */

#endif							/* STD_FLOAT_DEFINE */

#undef STD_FLOAT_CAT_
#undef STD_FLOAT_CAT
#undef STD_FLOAT_DEF_static
#undef STD_FLOAT_DEF_extern
#undef STD_FLOAT_DEF
#undef STD_FLOAT_NAME
#undef STD_FLOAT_SCOPE
#undef STD_FLOAT_DECLARE
#undef STD_FLOAT_DEFINE
#undef STD_FLOAT_TS
#undef STD_FLOAT_T
#undef STD_FLOAT_GENERAL_FORMAT
#undef STD_FLOAT_DISPLAY
