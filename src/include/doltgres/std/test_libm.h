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
 * The test-only forms of f64's methods that call the platform's libm, as
 * Rust's do, so that each agrees with Rust bit for bit on its platform; std's
 * test module alone uses them (the port text's section 3).
 */
#ifndef DOLTGRES_STD_TEST_LIBM_H
#define DOLTGRES_STD_TEST_LIBM_H

#include <math.h>

/* std_f64_ln is f64::ln, the natural logarithm, by the platform's libm as Rust's is. */
static inline float8
std_f64_ln(float8 x)
{
	return log(x);
}

/* std_f64_exp is f64::exp, e to the power x, by the platform's libm as Rust's is. */
static inline float8
std_f64_exp(float8 x)
{
	return exp(x);
}

/* std_f64_ln_1p is f64::ln_1p, ln(1 + x) taken exactly near zero, by the platform's libm as Rust's is. */
static inline float8
std_f64_ln_1p(float8 x)
{
	return log1p(x);
}

/* std_f64_exp_m1 is f64::exp_m1, e^x - 1 taken exactly near zero, by the platform's libm as Rust's is. */
static inline float8
std_f64_exp_m1(float8 x)
{
	return expm1(x);
}

/* std_f64_log2 is f64::log2, the base 2 logarithm, by the platform's libm as Rust's is. */
static inline float8
std_f64_log2(float8 x)
{
	return log2(x);
}

/* std_f64_log is f64::log, the logarithm in a base, ln(x) / ln(base) as Rust's is. */
static inline float8
std_f64_log(float8 x, float8 base)
{
	return std_f64_ln(x) / std_f64_ln(base);
}

#endif							/* DOLTGRES_STD_TEST_LIBM_H */
