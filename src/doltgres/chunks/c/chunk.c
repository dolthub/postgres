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
 * address, which only hashing in verified code makes.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/chunk.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/unwind.h"

/* CHUNKS_CHUNK_LAYOUT is a ChunksChunkChunk's opaque storage read as Chunk's layout, which only chunk.c knows. */
#define CHUNKS_CHUNK_LAYOUT(chunk) ((chunks_chunk_ChunkLayout *) (chunk))

/*
 * chunks_chunk_ChunkLayout is Chunk's layout inside a ChunksChunkChunk's
 * opaque storage: its address, its stored bytes, and how many of them are
 * the hashed part.
 */
typedef struct chunks_chunk_ChunkLayout
{
	ChunksAddressAddress address;
	StdVec		stored;
	size_t		hashed;
} chunks_chunk_ChunkLayout;

StaticAssertDecl(sizeof(chunks_chunk_ChunkLayout) <= sizeof(ChunksChunkChunk) &&
				 _Alignof(chunks_chunk_ChunkLayout) <= _Alignof(ChunksChunkChunk),
				 "Chunk's layout fits ChunksChunkChunk's opaque storage");

#define RUST_FILE "chunks" STD_SEP "src" STD_SEP "chunk.rs"

/* chunks_chunk_VecU8_Debug_fmt is a Vec<u8>'s Debug: its slice's. */
bool
chunks_chunk_VecU8_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *v = value;
	StdBytes	bytes = {v->ptr, v->len};

	return std_bytes_Debug_fmt(&bytes, f);
}

/* chunks_chunk_Chunk_Debug_fmt is Chunk's derived Debug. */
bool
chunks_chunk_Chunk_Debug_fmt(const void *value, StdFormatter *f)
{
	const ChunksChunkChunk *self = value;
	StdDebugStruct s = std_fmt_debug_struct(f, "Chunk", 5);

	std_DebugStruct_field(&s, "address", 7, &CHUNKS_CHUNK_LAYOUT(self)->address, chunks_address_Address_Debug_fmt);
	std_DebugStruct_field(&s, "stored", 6, &CHUNKS_CHUNK_LAYOUT(self)->stored, chunks_chunk_VecU8_Debug_fmt);
	std_DebugStruct_field(&s, "hashed", 6, &CHUNKS_CHUNK_LAYOUT(self)->hashed, std_usize_Debug_fmt);
	return std_DebugStruct_finish(&s);
}

/* chunks_chunk_Chunk_PartialEq_eq is Chunk's derived PartialEq: each field's, in order. */
bool
chunks_chunk_Chunk_PartialEq_eq(const void *self_, const void *other_)
{
	const ChunksChunkChunk *self = self_;
	const ChunksChunkChunk *other = other_;

	return chunks_address_Address_PartialEq_eq(&CHUNKS_CHUNK_LAYOUT(self)->address, &CHUNKS_CHUNK_LAYOUT(other)->address) &&
		CHUNKS_CHUNK_LAYOUT(self)->stored.len == CHUNKS_CHUNK_LAYOUT(other)->stored.len &&
		memcmp(CHUNKS_CHUNK_LAYOUT(self)->stored.ptr, CHUNKS_CHUNK_LAYOUT(other)->stored.ptr, CHUNKS_CHUNK_LAYOUT(self)->stored.len) == 0 &&
		CHUNKS_CHUNK_LAYOUT(self)->hashed == CHUNKS_CHUNK_LAYOUT(other)->hashed;
}

/*
 * chunks_chunk_Chunk_checked_frame_0_t is checked's state across its pad: its
 * arguments, its result, and the stored bytes it holds.
 */
typedef struct chunks_chunk_Chunk_checked_frame_0_t
{
	const ChunksAddressAddress *address;
	StdVec		stored;
	bool		stored_live;
	size_t		hashed;
	ChunksChunkChunk *result;
	bool		found;
} chunks_chunk_Chunk_checked_frame_0_t;

/* chunks_chunk_Chunk_checked_body_0 is checked's body. */
static void
chunks_chunk_Chunk_checked_body_0(void *frame_)
{
	chunks_chunk_Chunk_checked_frame_0_t *frame = frame_;
	ChunksAddressAddress found;

	if (frame->hashed > frame->stored.len)
	{
		frame->stored_live = false;
		std_vec_free_Checked(&frame->stored, 1, 1);
		frame->found = false;
		return;
	}
	std_range_to_check(frame->hashed, frame->stored.len, RUST_FILE, 80, 51);
	found = chunks_address_Address_of(frame->stored.ptr, frame->hashed);
	if (chunks_address_Address_same(&found, frame->address))
	{
		CHUNKS_CHUNK_LAYOUT(frame->result)->address = found;
		frame->stored_live = false;
		CHUNKS_CHUNK_LAYOUT(frame->result)->stored = frame->stored;
		CHUNKS_CHUNK_LAYOUT(frame->result)->hashed = frame->hashed;
		frame->found = true;
	}
	else
	{
		frame->stored_live = false;
		std_vec_free_Checked(&frame->stored, 1, 1);
		frame->found = false;
	}
}

/* chunks_chunk_Chunk_checked_pad_0 is checked's pad: the stored bytes, while it holds them. */
static void
chunks_chunk_Chunk_checked_pad_0(void *frame_)
{
	chunks_chunk_Chunk_checked_frame_0_t *frame = frame_;

	if (frame->stored_live)
		std_vec_free_Checked(&frame->stored, 1, 1);
}

/*
 * chunks_chunk_Chunk_checked returns a chunk's stored bytes, taking them,
 * under an address when their first `hashed` bytes hash to it, or none.
 */
bool
chunks_chunk_Chunk_checked(const ChunksAddressAddress *address, StdVec *stored, size_t hashed,
						   ChunksChunkChunk *result)
{
	chunks_chunk_Chunk_checked_frame_0_t frame = {.address = address, .stored = *stored, .stored_live = true,
	.hashed = hashed, .result = result};

	std_landing(&frame, chunks_chunk_Chunk_checked_body_0, chunks_chunk_Chunk_checked_pad_0);
	return frame.found;
}

/*
 * chunks_chunk_Chunk_written_frame_0_t is written's state across its pad: its
 * arguments, its result, and the stored bytes it holds.
 */
typedef struct chunks_chunk_Chunk_written_frame_0_t
{
	StdVec		stored;
	bool		stored_live;
	size_t		hashed;
	ChunksChunkChunk *result;
} chunks_chunk_Chunk_written_frame_0_t;

/* chunks_chunk_Chunk_written_body_0 is written's body. */
static void
chunks_chunk_Chunk_written_body_0(void *frame_)
{
	chunks_chunk_Chunk_written_frame_0_t *frame = frame_;
	size_t		hashed = frame->hashed <= frame->stored.len ? frame->hashed : frame->stored.len;
	ChunksAddressAddress address;

	std_range_to_check(hashed, frame->stored.len, RUST_FILE, 99, 53);
	address = chunks_address_Address_of(frame->stored.ptr, hashed);
	CHUNKS_CHUNK_LAYOUT(frame->result)->address = address;
	frame->stored_live = false;
	CHUNKS_CHUNK_LAYOUT(frame->result)->stored = frame->stored;
	CHUNKS_CHUNK_LAYOUT(frame->result)->hashed = hashed;
}

/* chunks_chunk_Chunk_written_pad_0 is written's pad: the stored bytes, while it holds them. */
static void
chunks_chunk_Chunk_written_pad_0(void *frame_)
{
	chunks_chunk_Chunk_written_frame_0_t *frame = frame_;

	if (frame->stored_live)
		std_vec_free_Checked(&frame->stored, 1, 1);
}

/*
 * chunks_chunk_Chunk_written returns the stored bytes of a chunk being
 * written, taking them, whose first `hashed` bytes are its hashed part, under
 * their address.
 */
void
chunks_chunk_Chunk_written(StdVec *stored, size_t hashed, ChunksChunkChunk *result)
{
	chunks_chunk_Chunk_written_frame_0_t frame = {.stored = *stored, .stored_live = true, .hashed = hashed,
	.result = result};

	std_landing(&frame, chunks_chunk_Chunk_written_body_0, chunks_chunk_Chunk_written_pad_0);
}

/* chunks_chunk_Chunk_address returns the chunk's address. */
ChunksAddressAddress
chunks_chunk_Chunk_address(const ChunksChunkChunk *self)
{
	return CHUNKS_CHUNK_LAYOUT(self)->address;
}

/* chunks_chunk_Chunk_bytes returns the chunk's stored bytes. */
StdBytes
chunks_chunk_Chunk_bytes(const ChunksChunkChunk *self)
{
	StdBytes	bytes = {CHUNKS_CHUNK_LAYOUT(self)->stored.ptr, CHUNKS_CHUNK_LAYOUT(self)->stored.len};

	return bytes;
}

/* chunks_chunk_Chunk_into_bytes returns the chunk's stored bytes, taking them. */
void
chunks_chunk_Chunk_into_bytes(ChunksChunkChunk *self, StdVec *result)
{
	*result = CHUNKS_CHUNK_LAYOUT(self)->stored;
}

/* chunks_chunk_Chunk_Clone_clone is Chunk's Clone: the stored bytes cloned, the address and length copied. */
void
chunks_chunk_Chunk_Clone_clone(const ChunksChunkChunk *self, ChunksChunkChunk *result)
{
	StdVec		stored;

	std_vec_clone(&stored, &CHUNKS_CHUNK_LAYOUT(self)->stored, 1, 1);
	CHUNKS_CHUNK_LAYOUT(result)->address = CHUNKS_CHUNK_LAYOUT(self)->address;
	CHUNKS_CHUNK_LAYOUT(result)->stored = stored;
	CHUNKS_CHUNK_LAYOUT(result)->hashed = CHUNKS_CHUNK_LAYOUT(self)->hashed;
}

/*
 * chunks_chunk_Chunk_stored returns the stored bytes of a chunk being
 * written, its hashed bytes with a trailer of its locators and stamps, under
 * the address of its hashed bytes.
 */
void
chunks_chunk_Chunk_stored(const uint8 *hashed, size_t hashed_len, const ChunksTrailerLocator *locators,
						  size_t locators_len, const ChunksTrailerStamps *stamps, ChunksChunkChunk *result)
{
	StdVec		stored;

	chunks_trailer_store(hashed, hashed_len, locators, locators_len, stamps, &stored);
	chunks_chunk_Chunk_written(&stored, hashed_len, result);
}

/* chunks_chunk_Chunk_split reads the chunk's trailer, as Stored::split reads it. */
bool
chunks_chunk_Chunk_split(const ChunksChunkChunk *self,
						 bool (*addresses) (void *captures, const uint8 *hashed, size_t hashed_len, size_t *result,
											ChunksErrorError *error),
						 void *addresses_captures, void (*addresses_drop) (void *), ChunksTrailerStored *result,
						 ChunksErrorError *error)
{
	return chunks_trailer_Stored_split(CHUNKS_CHUNK_LAYOUT(self)->stored.ptr, CHUNKS_CHUNK_LAYOUT(self)->stored.len, addresses, addresses_captures,
									   addresses_drop, result, error);
}

/* chunks_chunk_Chunk_drop is Chunk's drop glue: its stored bytes. */
void
chunks_chunk_Chunk_drop(void *self_)
{
	ChunksChunkChunk *self = self_;

	std_vec_free_Checked(&CHUNKS_CHUNK_LAYOUT(self)->stored, 1, 1);
}
