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
 * Chunk tokens: a chunk's stored bytes whose hashed part has the chunk's
 * address, which only hashing in verified code makes (chunks' chunk.rs).
 */
#ifndef DOLTGRES_CHUNKS_CHUNK_H
#define DOLTGRES_CHUNKS_CHUNK_H

#include "doltgres/chunks/address.h"
#include "doltgres/chunks/error.h"
#include "doltgres/chunks/trailer.h"
#include "doltgres/std/vec.h"

/*
 * ChunksChunkChunk is Chunk: a chunk's stored bytes whose hashed part has the
 * chunk's address, which only a read that checked it or a write that hashed
 * it makes, so that no decoder takes bytes a damaged record holds. Its
 * storage is opaque, laid out by chunk.c alone, as the Rust's fields are
 * private to its module, so no other C names its parts, and t/117 refuses a
 * file outside chunk.c that initializes one or copies bytes into one.
 */
typedef struct ChunksChunkChunk
{
	uint64		opaque[8];
} ChunksChunkChunk;

extern bool chunks_chunk_VecU8_Debug_fmt(const void *value, StdFormatter *f);
extern bool chunks_chunk_Chunk_Debug_fmt(const void *value, StdFormatter *f);
extern bool chunks_chunk_Chunk_PartialEq_eq(const void *self_, const void *other_);
extern bool chunks_chunk_Chunk_checked(const ChunksAddressAddress *address, StdVec *stored, size_t hashed,
									   ChunksChunkChunk *result);
extern void chunks_chunk_Chunk_written(StdVec *stored, size_t hashed, ChunksChunkChunk *result);
extern ChunksAddressAddress chunks_chunk_Chunk_address(const ChunksChunkChunk *self);
extern StdBytes chunks_chunk_Chunk_bytes(const ChunksChunkChunk *self);
extern void chunks_chunk_Chunk_into_bytes(ChunksChunkChunk *self, StdVec *result);
extern void chunks_chunk_Chunk_Clone_clone(const ChunksChunkChunk *self, ChunksChunkChunk *result);
extern void chunks_chunk_Chunk_stored(const uint8 *hashed, size_t hashed_len, const ChunksTrailerLocator *locators,
									  size_t locators_len, const ChunksTrailerStamps *stamps,
									  ChunksChunkChunk *result);
extern bool chunks_chunk_Chunk_split(const ChunksChunkChunk *self,
									 bool (*addresses) (void *captures, const uint8 *hashed, size_t hashed_len,
														size_t *result, ChunksErrorError *error),
									 void *addresses_captures, void (*addresses_drop) (void *),
									 ChunksTrailerStored *result, ChunksErrorError *error);
extern void chunks_chunk_Chunk_drop(void *self_);

#endif							/* DOLTGRES_CHUNKS_CHUNK_H */
