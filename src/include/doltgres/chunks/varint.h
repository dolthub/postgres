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
 * Variable-length unsigned integers: seven bits per byte, low bits first,
 * with the high bit set on every byte but the last (chunks' varint.rs).
 */
#ifndef DOLTGRES_CHUNKS_VARINT_H
#define DOLTGRES_CHUNKS_VARINT_H

#include "doltgres/chunks/error.h"
#include "doltgres/std/vec.h"

/* ChunksVarintMalformed is why bytes hold no varint where one is read. */
typedef enum ChunksVarintMalformed
{
	CHUNKS_VARINT_MALFORMED_PAST_END,
	CHUNKS_VARINT_MALFORMED_TOO_LARGE,
	CHUNKS_VARINT_MALFORMED_NOT_SHORTEST,
} ChunksVarintMalformed;

extern void chunks_varint_put_varint(StdVec *out, uint64 value);
extern size_t chunks_varint_varint_len(uint64 value);
extern bool chunks_varint_decode(const uint8 *bytes, size_t bytes_len,
								 size_t *at, uint64 *result,
								 ChunksVarintMalformed *error);
extern bool chunks_varint_read_varint(const uint8 *bytes, size_t bytes_len,
									  size_t *at, uint64 *result,
									  ChunksErrorError *error);
extern ChunksErrorError chunks_varint_Malformed_error(ChunksVarintMalformed self);

extern void test_chunks_varint_tests_round_trips(void);
extern void test_chunks_varint_tests_rejects_long_forms(void);

#endif							/* DOLTGRES_CHUNKS_VARINT_H */
