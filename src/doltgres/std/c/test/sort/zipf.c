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
 * The C port of core's sort tests' zipf.rs (library/alloctests/tests/sort,
 * Rust 1.99.1): a Zipf distribution over 1 to n by rejection-inversion
 * sampling, which the random_zipf pattern draws from.
 */
#include "postgres.h"

#include "doltgres/std/num.h"
#include "doltgres/std/test_flt2dec.h"
#include "doltgres/std/test_libm.h"
#include "doltgres/std/test_sort.h"

static float8 alloctests_sort_zipf_ZipfDistribution_h_integral(float8 x, float8 exponent);
static float8 alloctests_sort_zipf_ZipfDistribution_h(float8 x, float8 exponent);
static float8 alloctests_sort_zipf_ZipfDistribution_h_integral_inv(float8 x, float8 exponent);
static float8 alloctests_sort_zipf_helper1(float8 x);
static float8 alloctests_sort_zipf_helper2(float8 x);

/*
 * alloctests_sort_zipf_ZipfDistribution_new is ZipfDistribution::new: the
 * distribution over 1 to num_elements with the exponent, or Err(()) for no
 * elements or an exponent that isn't positive.
 */
bool
alloctests_sort_zipf_ZipfDistribution_new(size_t num_elements, float8 exponent,
										  AlloctestsSortZipfDistribution *result)
{
	if (num_elements == 0)
		return false;
	if (exponent <= 0.0)
		return false;
	result->num_elements = (float8) num_elements;
	result->exponent = exponent;
	result->h_integral_x1 = alloctests_sort_zipf_ZipfDistribution_h_integral(1.5, exponent) - 1.0;
	result->h_integral_num_elements =
		alloctests_sort_zipf_ZipfDistribution_h_integral((float8) num_elements + 0.5, exponent);
	result->s = 2.0 - alloctests_sort_zipf_ZipfDistribution_h_integral_inv(
																		   alloctests_sort_zipf_ZipfDistribution_h_integral(2.5, exponent) -
																		   alloctests_sort_zipf_ZipfDistribution_h(2.0, exponent), exponent);
	return true;
}

/*
 * alloctests_sort_zipf_ZipfDistribution_next is ZipfDistribution::next, its
 * Distribution's sample: a uniform draw over the integral of h, inverted,
 * kept when it falls under h at its nearest integer.
 */
size_t
alloctests_sort_zipf_ZipfDistribution_next(const AlloctestsSortZipfDistribution *self, uint32 *rng)
{
	float8		hnum = self->h_integral_num_elements;

	for (;;)
	{
		float8		u = hnum + rand_StandardUniform_sample_f64(rng) * (self->h_integral_x1 - hnum);
		float8		x = alloctests_sort_zipf_ZipfDistribution_h_integral_inv(u, self->exponent);
		float8		k64 = std_f64_min(std_f64_max(x, 1.0), self->num_elements);
		size_t		k = std_max_usize(1, std_as_f64_usize(k64 + 0.5));

		if (k64 - x <= self->s ||
			u >= alloctests_sort_zipf_ZipfDistribution_h_integral(k64 + 0.5, self->exponent) -
			alloctests_sort_zipf_ZipfDistribution_h(k64, self->exponent))
			return k;
	}
}

/* alloctests_sort_zipf_ZipfDistribution_h_integral is H(x), the integral of h: helper2((1 - e) ln x) ln x. */
static float8
alloctests_sort_zipf_ZipfDistribution_h_integral(float8 x, float8 exponent)
{
	float8		log_x = std_f64_ln(x);

	return alloctests_sort_zipf_helper2((1.0 - exponent) * log_x) * log_x;
}

/* alloctests_sort_zipf_ZipfDistribution_h is h(x) = exp(-e ln x). */
static float8
alloctests_sort_zipf_ZipfDistribution_h(float8 x, float8 exponent)
{
	return std_f64_exp(-exponent * std_f64_ln(x));
}

/* alloctests_sort_zipf_ZipfDistribution_h_integral_inv is H's inverse: exp(helper1(t) x), with t = x (1 - e) no less than -1. */
static float8
alloctests_sort_zipf_ZipfDistribution_h_integral_inv(float8 x, float8 exponent)
{
	float8		t = x * (1.0 - exponent);

	if (t < -1.0)
		t = -1.0;
	return std_f64_exp(alloctests_sort_zipf_helper1(t) * x);
}

/* alloctests_sort_zipf_helper1 is ln(1 + x) / x, by its series near zero. */
static float8
alloctests_sort_zipf_helper1(float8 x)
{
	if (std_f64_abs(x) > 1e-8)
		return std_f64_ln_1p(x) / x;
	else
		return 1.0 - x * (0.5 - x * (1.0 / 3.0 - 0.25 * x));
}

/* alloctests_sort_zipf_helper2 is (exp(x) - 1) / x, by its series near zero. */
static float8
alloctests_sort_zipf_helper2(float8 x)
{
	if (std_f64_abs(x) > 1e-8)
		return std_f64_exp_m1(x) / x;
	else
		return 1.0 + x * 0.5 * (1.0 + x * 1.0 / 3.0 * (1.0 + 0.25 * x));
}
