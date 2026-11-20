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
 * zstd's compression, decompression, and dictionary training over the
 * libzstd Postgres links (pg's zstd.rs, design item 11).
 */
#ifndef DOLTGRES_PG_ZSTD_H
#define DOLTGRES_PG_ZSTD_H

#include <zstd.h>

#include "doltgres/std/any.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/str.h"
#include "doltgres/std/vec.h"

/* PgZstdCompressDictionary is a zstd dictionary prepared once for compressing at a level. */
typedef struct PgZstdCompressDictionary
{
	ZSTD_CDict *f0;
} PgZstdCompressDictionary;

/* PgZstdDecompressDictionary is a zstd dictionary prepared once for decompressing. */
typedef struct PgZstdDecompressDictionary
{
	ZSTD_DDict *f0;
} PgZstdDecompressDictionary;

/* PgZstdFailed is why a frame failed to decompress. */
typedef enum PgZstdFailed
{
	/*
	 * PG_ZSTD_FAILED_DAMAGED is a frame whose bytes are damaged, or are not
	 * the size it was asked for.
	 */
	PG_ZSTD_FAILED_DAMAGED,

	/*
	 * PG_ZSTD_FAILED_OUT_OF_MEMORY is libzstd's failure to allocate its
	 * decompression context.
	 */
	PG_ZSTD_FAILED_OUT_OF_MEMORY,
} PgZstdFailed;

/* PG_ZSTD_COMPRESS_DICTIONARY_TYPE_ID and PG_ZSTD_DECOMPRESS_DICTIONARY_TYPE_ID are the dictionaries' identities, which a host's dictionary holds as an Any. */
extern const StdTypeId PG_ZSTD_COMPRESS_DICTIONARY_TYPE_ID;
extern const StdTypeId PG_ZSTD_DECOMPRESS_DICTIONARY_TYPE_ID;

extern bool pg_zstd_CompressDictionary_new(const uint8 *bytes, size_t bytes_len, int32 level,
										   PgZstdCompressDictionary *result);
extern bool pg_zstd_DecompressDictionary_new(const uint8 *bytes, size_t bytes_len,
											 PgZstdDecompressDictionary *result);
extern void pg_zstd_CompressDictionary_drop(void *self_);
extern void pg_zstd_DecompressDictionary_drop(void *self_);
extern bool pg_zstd_compress(const uint8 *bytes, size_t bytes_len, int32 level,
							 const PgZstdCompressDictionary *dictionary, StdVec *result);
extern bool pg_zstd_frame_size(const uint8 *frame, size_t frame_len, uint64 *result);
extern bool pg_zstd_Failed_Debug_fmt(const void *value, StdFormatter *f);
extern bool pg_zstd_decompress(const uint8 *frame, size_t frame_len, size_t size,
							   const PgZstdDecompressDictionary *dictionary, StdVec *result, PgZstdFailed *error);
extern bool pg_zstd_train(const StdBytes *samples, size_t samples_len, size_t capacity, StdVec *result);

#ifdef DOLTGRES_TEST_PG
extern void test_pg_zstd_tests_compression_round_trips(void);
extern void test_pg_zstd_tests_damaged_frames_record_no_size(void);
extern void test_pg_zstd_tests_damaged_frames_fail_as_damaged(void);
extern void test_pg_zstd_tests_a_dictionary_round_trips_what_it_compresses(void);
extern void test_pg_zstd_tests_too_few_samples_train_nothing(void);
#endif

#endif							/* DOLTGRES_PG_ZSTD_H */
