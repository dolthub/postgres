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
 * The C port of coretests' flt2dec tests at Rust 1.99.1 (the port text's
 * section 13): the helpers num/mod.rs and num/flt2dec/mod.rs share with the
 * strategies' and random.rs's tests, the f16 and f32 instances of flt2dec's
 * generics they format with, and each test.
 */
#ifndef DOLTGRES_STD_TEST_FLT2DEC_H
#define DOLTGRES_STD_TEST_FLT2DEC_H

#include "doltgres/std/flt2dec.h"

/* CORETESTS_F16_MAX is f16::MAX's bits. */
#define CORETESTS_F16_MAX ((uint16) 0x7bff)

/* CORETESTS_F16_MIN_POSITIVE is f16::MIN_POSITIVE's bits. */
#define CORETESTS_F16_MIN_POSITIVE ((uint16) 0x0400)

/* core's flt2dec generics at f32, which the tests format with. */
#define STD_FLT2DEC_SCOPE extern
#define STD_FLT2DEC_DECLARE
#define STD_FLT2DEC_TS F32
#define STD_FLT2DEC_T float4
#include "doltgres/std/flt2dec_template.h"

/* core's flt2dec generics at f16, held as its bits, which the tests format with. */
#define STD_FLT2DEC_SCOPE extern
#define STD_FLT2DEC_DECLARE
#define STD_FLT2DEC_TS F16
#define STD_FLT2DEC_T uint16
#include "doltgres/std/flt2dec_template.h"

/*
 * CoretestsFormatShortestOptFn is format_shortest_opt taken as random.rs's
 * FnMut: true with the digits' count in result and the exponent in result_1,
 * false for None.
 */
typedef bool (*CoretestsFormatShortestOptFn) (void *captures, const StdDecoded *d, uint8 *buf, size_t buf_len,
											  size_t *result, int16 *result_1);

extern uint16 std_f64_as_f16(float8 v);
extern float8 std_f16_as_f64(uint16 v);
extern uint16 coretests_num_ldexp_f16(uint16 a, int32 b);
extern float4 coretests_num_ldexp_f32(float4 a, int32 b);
extern float8 coretests_num_ldexp_f64(float8 a, int32 b);
extern void coretests_test_rng(uint32 *result, const char *file, uint32 line, uint32 col);
extern uint32 coretests_XorShiftRng_next_u32(uint32 *self);
extern uint64 coretests_XorShiftRng_next_u64(uint32 *self);
extern void coretests_XorShiftRng_from_seed(const uint8 *seed, uint32 *result);
extern void coretests_XorShiftRng_seed_from_u64(uint64 state, uint32 *result);

/* CoretestsUniformInt is rand's UniformInt at an integer type: its low, its range, and its rejection threshold, as bits. */
typedef struct CoretestsUniformInt
{
	uint64		low;
	uint64		range;
	uint64		thresh;
} CoretestsUniformInt;

extern uint32 rand_StandardUniform_sample_u32(uint32 *rng);
extern int32 rand_StandardUniform_sample_i32(uint32 *rng);
extern float8 rand_StandardUniform_sample_f64(uint32 *rng);
extern CoretestsUniformInt coretests_rand_UniformInt_new(uint64 low, uint64 high, bool is_signed, uint32 bits,
														 uint32 sample_bits, const char *file, uint32 line,
														 uint32 col);
extern CoretestsUniformInt coretests_rand_UniformInt_new_inclusive(uint64 low, uint64 high, bool is_signed,
																   uint32 bits, uint32 sample_bits, const char *file,
																   uint32 line, uint32 col);
extern uint64 coretests_rand_UniformInt_sample(const CoretestsUniformInt *self, uint32 *rng, uint32 bits,
											   uint32 sample_bits);

extern bool std_flt2dec_decoder_Decoded_Debug_fmt(const void *value, struct StdFormatter *f);
extern StdDecoded coretests_num_flt2dec_decode_finiteF16(uint16 v);
extern StdDecoded coretests_num_flt2dec_decode_finiteF32(float4 v);
extern StdDecoded coretests_num_flt2dec_decode_finiteF64(float8 v);
extern void coretests_num_flt2dec_f16_shortest_sanity_test(StdFlt2decFormatShortestFn f, void *f_captures,
														   void (*f_drop) (void *));
extern void coretests_num_flt2dec_f16_exact_sanity_test(StdFlt2decFormatExactFn f, void *f_captures,
														void (*f_drop) (void *));
extern void coretests_num_flt2dec_f32_shortest_sanity_test(StdFlt2decFormatShortestFn f, void *f_captures,
														   void (*f_drop) (void *));
extern void coretests_num_flt2dec_f32_exact_sanity_test(StdFlt2decFormatExactFn f, void *f_captures,
														void (*f_drop) (void *));
extern void coretests_num_flt2dec_f64_shortest_sanity_test(StdFlt2decFormatShortestFn f, void *f_captures,
														   void (*f_drop) (void *));
extern void coretests_num_flt2dec_f64_exact_sanity_test(StdFlt2decFormatExactFn f, void *f_captures,
														void (*f_drop) (void *));
extern void coretests_num_flt2dec_more_shortest_sanity_test(StdFlt2decFormatShortestFn f, void *f_captures,
															void (*f_drop) (void *));
extern void coretests_num_flt2dec_to_shortest_str_test(StdFlt2decFormatShortestFn f_, void *f__captures,
													   void (*f__drop) (void *));
extern void coretests_num_flt2dec_to_shortest_exp_str_test(StdFlt2decFormatShortestFn f_, void *f__captures,
														   void (*f__drop) (void *));
extern void coretests_num_flt2dec_to_exact_exp_str_test(StdFlt2decFormatExactFn f_, void *f__captures,
														void (*f__drop) (void *));
extern void coretests_num_flt2dec_to_exact_fixed_str_test(StdFlt2decFormatExactFn f_, void *f__captures,
														  void (*f__drop) (void *));

extern void test_coretests_num_flt2dec_estimator_test_estimate_scaling_factor(void);
extern void test_coretests_num_flt2dec_random_exact_f16_random_equivalence_test(void);
extern void test_coretests_num_flt2dec_random_exact_f32_random_equivalence_test(void);
extern void test_coretests_num_flt2dec_random_exact_f64_random_equivalence_test(void);
extern void test_coretests_num_flt2dec_random_shortest_f16_exhaustive_equivalence_test(void);
extern void test_coretests_num_flt2dec_random_shortest_random_equivalence_test(void);
extern void test_coretests_num_flt2dec_strategy_dragon_exact_sanity_test(void);
extern void test_coretests_num_flt2dec_strategy_dragon_shortest_sanity_test(void);
extern void test_coretests_num_flt2dec_strategy_dragon_test_mul_pow10(void);
extern void test_coretests_num_flt2dec_strategy_dragon_test_to_exact_exp_str(void);
extern void test_coretests_num_flt2dec_strategy_dragon_test_to_exact_fixed_str(void);
extern void test_coretests_num_flt2dec_strategy_dragon_test_to_shortest_exp_str(void);
extern void test_coretests_num_flt2dec_strategy_dragon_test_to_shortest_str(void);
extern void test_coretests_num_flt2dec_strategy_grisu_exact_sanity_test(void);
extern void test_coretests_num_flt2dec_strategy_grisu_shortest_sanity_test(void);
extern void test_coretests_num_flt2dec_strategy_grisu_test_cached_power(void);
extern void test_coretests_num_flt2dec_strategy_grisu_test_max_pow10_no_more_than(void);
extern void test_coretests_num_flt2dec_strategy_grisu_test_to_exact_exp_str(void);
extern void test_coretests_num_flt2dec_strategy_grisu_test_to_exact_fixed_str(void);
extern void test_coretests_num_flt2dec_strategy_grisu_test_to_shortest_exp_str(void);
extern void test_coretests_num_flt2dec_strategy_grisu_test_to_shortest_str(void);

#endif							/* DOLTGRES_STD_TEST_FLT2DEC_H */
