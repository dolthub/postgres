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
 * The C form of a float's Debug (core/src/fmt/float.rs at Rust 1.99.1):
 * fmt/float.rs's generics at f64, and f64's Debug and Display.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include <math.h>

#include "doltgres/std/flt2dec.h"
#include "doltgres/std/fmt.h"

/*
 * std_fmt_float_F32_GeneralFormat_already_rounded_value_should_use_exponential
 * is f32's GeneralFormat: whether a value takes the exponential form, by its
 * magnitude.
 */
static bool
std_fmt_float_F32_GeneralFormat_already_rounded_value_should_use_exponential(const float4 *self)
{
	float4		max_abs = 1e+16f;
	float4		abs = fabsf(*self);

	return (abs != 0.0f && abs < 1e-4f) || abs >= max_abs;
}

/*
 * std_fmt_float_F64_GeneralFormat_already_rounded_value_should_use_exponential
 * is f64's GeneralFormat: whether a value takes the exponential form, by its
 * magnitude.
 */
static bool
std_fmt_float_F64_GeneralFormat_already_rounded_value_should_use_exponential(const float8 *self)
{
	float8		max_abs = 1e+16;
	float8		abs = fabs(*self);

	return (abs != 0.0 && abs < 1e-4) || abs >= max_abs;
}

/* fmt/float.rs's generics at f32, for f32's Debug. */
#define STD_FLOAT_SCOPE static
#define STD_FLOAT_DECLARE
#define STD_FLOAT_DEFINE
#define STD_FLOAT_TS F32
#define STD_FLOAT_T float4
#define STD_FLOAT_GENERAL_FORMAT std_fmt_float_F32_GeneralFormat_already_rounded_value_should_use_exponential
#include "doltgres/std/float_template.h"

/* fmt/float.rs's generics at f64, with its Display. */
#define STD_FLOAT_SCOPE static
#define STD_FLOAT_DECLARE
#define STD_FLOAT_DEFINE
#define STD_FLOAT_DISPLAY
#define STD_FLOAT_TS F64
#define STD_FLOAT_T float8
#define STD_FLOAT_GENERAL_FORMAT std_fmt_float_F64_GeneralFormat_already_rounded_value_should_use_exponential
#include "doltgres/std/float_template.h"

/* std_f32_Debug_fmt is f32's Debug: float_to_general_debug. */
bool
std_f32_Debug_fmt(const void *value, StdFormatter *f)
{
	return std_fmt_float_float_to_general_debugF32(f, (const float4 *) value);
}

/* std_f64_Debug_fmt is f64's Debug: float_to_general_debug. */
bool
std_f64_Debug_fmt(const void *value, StdFormatter *f)
{
	return std_fmt_float_float_to_general_debugF64(f, (const float8 *) value);
}

/* std_f64_Display_fmt is f64's Display: float_to_decimal_display. */
bool
std_f64_Display_fmt(const void *value, StdFormatter *f)
{
	return std_fmt_float_float_to_decimal_displayF64(f, (const float8 *) value);
}
