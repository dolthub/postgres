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
 * A tree's summaries (encoding's summary.rs): what a node of a GiST index's
 * tree keeps of the keys below it, the union of each column's keys by its
 * operator class's rule.
 */
#ifndef DOLTGRES_ENCODING_SUMMARY_H
#define DOLTGRES_ENCODING_SUMMARY_H

#include "doltgres/chunks/error.h"
#include "doltgres/std/str.h"
#include "doltgres/std/vec.h"

#define ENCODING_SUMMARY_GIST 1
#define ENCODING_SUMMARY_GIST_COUNTED 2
#define ENCODING_SUMMARY_GIST_COMPARED 3
#define ENCODING_SUMMARY_GIST_COMPARED_COUNTED 4
#define ENCODING_SUMMARY_GIST_NULL 0
#define ENCODING_SUMMARY_GIST_BOX 1
#define ENCODING_SUMMARY_GIST_POINT 2
#define ENCODING_SUMMARY_GIST_INET 3
#define ENCODING_SUMMARY_GIST_RANGE 4
#define ENCODING_SUMMARY_GIST_TSVECTOR 5
#define ENCODING_SUMMARY_GIST_TSQUERY 6
#define ENCODING_SUMMARY_GIST_RANGE_DATUM 7
#define ENCODING_SUMMARY_TSVECTOR_ARRKEY 0x01
#define ENCODING_SUMMARY_TSVECTOR_SIGNKEY 0x02
#define ENCODING_SUMMARY_TSVECTOR_ALLISTRUE 0x04
#define ENCODING_SUMMARY_RANGE_EMPTY 0x01
#define ENCODING_SUMMARY_RANGE_LB_INC 0x02
#define ENCODING_SUMMARY_RANGE_UB_INC 0x04
#define ENCODING_SUMMARY_RANGE_LB_INF 0x08
#define ENCODING_SUMMARY_RANGE_UB_INF 0x10
#define ENCODING_SUMMARY_RANGE_CONTAIN_EMPTY 0x80
#define ENCODING_SUMMARY_PGSQL_AF_INET 2
#define ENCODING_SUMMARY_PGSQL_AF_INET6 (ENCODING_SUMMARY_PGSQL_AF_INET + 1)

/* encoding_summary_gist_keys_t is a GiST key: its tag and its bytes, (u8, &[u8]). */
typedef struct encoding_summary_gist_keys_t
{
	uint8		f0;
	StdBytes	f1;
} encoding_summary_gist_keys_t;

/* EncodingSummaryBound is a range's bound: its key and its field, (&[u8], &[u8]). */
typedef struct EncodingSummaryBound
{
	StdBytes	f0;
	StdBytes	f1;
} EncodingSummaryBound;

/* EncodingSummaryRangeKey is a range's GiST key: its flags and its bounds, none where infinite or empty. */
typedef struct EncodingSummaryRangeKey
{
	uint8		flags;
	bool		has_lower;
	EncodingSummaryBound lower;
	bool		has_upper;
	EncodingSummaryBound upper;
} EncodingSummaryRangeKey;

/* EncodingSummarySummarizer is a summarizer: the union of a level's items, fn(u8, &[&[u8]]). */
typedef bool (*EncodingSummarySummarizer) (uint8 level, const StdBytes *items,
										   size_t items_len, StdVec *result,
										   ChunksErrorError *error);

extern bool encoding_summary_summarizer(uint8 kind, EncodingSummarySummarizer *result,
										ChunksErrorError *error);
extern bool encoding_summary_engine_made(uint8 kind);
extern bool encoding_summary_gist_keys(const uint8 *value, size_t value_len,
									   StdVec *result_0, StdBytes *result_1,
									   ChunksErrorError *error);
extern bool encoding_summary_gist_rows(const StdBytes *items, size_t items_len,
									   StdVec *result, ChunksErrorError *error);
extern bool encoding_summary_column_union(StdVec *out,
										  const encoding_summary_gist_keys_t *keys,
										  size_t keys_len,
										  ChunksErrorError *error);
extern bool encoding_summary_uncounted(const StdBytes *items, size_t items_len,
									   StdVec *result, ChunksErrorError *error);
extern size_t encoding_summary_bitncommon(const uint8 *l, size_t l_len,
										  const uint8 *r, size_t r_len, size_t n);
extern bool encoding_summary_RangeKey_read(const uint8 *bytes, size_t bytes_len,
										   EncodingSummaryRangeKey *result_0,
										   size_t *result_1,
										   ChunksErrorError *error);

#ifdef DOLTGRES_TEST_ENCODING
extern void test_encoding_summary_tests_each_columns_union_bytes_stay_as_they_are_stored(void);
extern void test_encoding_summary_tests_range_unions_are_stored_as_they_are_and_whatever_the_order_of_their_items(void);
extern void test_encoding_summary_tests_text_search_unions_bytes_stay_as_they_are_stored(void);
#endif

#endif							/* DOLTGRES_ENCODING_SUMMARY_H */
