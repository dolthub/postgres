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
 * The C port of chunks' packs/reads.rs: reads through a backend's view of a
 * repository's packs, where the address index says a chunk is, with the
 * copies a full collection found dead answered only when no other is
 * listed, and a chunk's stored bytes by its locator, through the forwards
 * when a collection retired its pack.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/pack.h"
#include "doltgres/chunks/packs.h"
#include "doltgres/chunks/packs/view.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/sync.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"

#ifdef DOLTGRES_TEST_CHUNKS
#include "doltgres/chunks/dead.h"
#include "doltgres/chunks/index.h"
#include "doltgres/chunks/packs.h"
#include "doltgres/chunks/packs/fixtures.h"
#include "doltgres/chunks/packs/lists.h"
#include "doltgres/simc/simc.h"
#include "doltgres/std/test.h"
#endif

#define RUST_FILE "chunks" STD_SEP "src" STD_SEP "packs" STD_SEP "reads.rs"

/* ChunksPacksReadsNamedTag is Named's variants: what an address alone names among the records of it. */
typedef enum ChunksPacksReadsNamedTag
{
	CHUNKS_PACKS_READS_NAMED_MISSING,
	CHUNKS_PACKS_READS_NAMED_ONE,
	CHUNKS_PACKS_READS_NAMED_SEVERAL,
} ChunksPacksReadsNamedTag;

/*
 * ChunksPacksReadsNamed is Named: none, one record, or records that hold
 * other stamps with none without, which no one record stands for.
 */
typedef struct ChunksPacksReadsNamed
{
	ChunksPacksReadsNamedTag tag;
	ChunksTrailerLocator one;
	/* several is a Vec<Locator>. */
	StdVec		several;
} ChunksPacksReadsNamed;

/* chunks_TupleLocatorStamps_t is a record's place with its stamps. */
typedef struct chunks_TupleLocatorStamps_t
{
	ChunksTrailerLocator f0;
	ChunksTrailerStamps f1;
} chunks_TupleLocatorStamps_t;

/* chunks_packs_reads_stamped_drop is a (Locator, Stamps)'s drop glue. */
static void
chunks_packs_reads_stamped_drop(void *value)
{
	chunks_trailer_Stamps_drop(&((chunks_TupleLocatorStamps_t *) value)->f1);
}

/*
 * chunks_packs_reads_named_by_address returns what an address alone names
 * among its records with their stamps: the first when every record holds the
 * same stamps, else one without stamps.
 */
static void
chunks_packs_reads_named_by_address(const StdVec *stamps, ChunksPacksReadsNamed *result)
{
	const chunks_TupleLocatorStamps_t *first;
	ChunksTrailerStamps bare;
	bool		same = true;

	if (stamps->len == 0)
	{
		result->tag = CHUNKS_PACKS_READS_NAMED_MISSING;
		return;
	}
	first = &STD_VEC_AT(stamps, chunks_TupleLocatorStamps_t, 0);
	for (size_t k = 0, k_len = stamps->len; k < k_len && same; k++)
		same = chunks_trailer_Stamps_PartialEq_eq(&STD_VEC_AT(stamps, chunks_TupleLocatorStamps_t, k).f1, &first->f1);
	if (same)
	{
		result->tag = CHUNKS_PACKS_READS_NAMED_ONE;
		result->one = first->f0;
		return;
	}
	bare = chunks_trailer_Stamps_Default_default();
	for (size_t k = 0, k_len = stamps->len; k < k_len; k++)
	{
		const chunks_TupleLocatorStamps_t *stamped = &STD_VEC_AT(stamps, chunks_TupleLocatorStamps_t, k);

		if (chunks_trailer_Stamps_PartialEq_eq(&stamped->f1, &bare))
		{
			chunks_trailer_Stamps_drop(&bare);
			result->tag = CHUNKS_PACKS_READS_NAMED_ONE;
			result->one = stamped->f0;
			return;
		}
	}
	chunks_trailer_Stamps_drop(&bare);
	result->tag = CHUNKS_PACKS_READS_NAMED_SEVERAL;
	std_vec_with_capacity_Checked(&result->several, stamps->len, sizeof(ChunksTrailerLocator),
								  _Alignof(ChunksTrailerLocator));
	for (size_t k = 0, k_len = stamps->len; k < k_len; k++)
		std_vec_push_Checked(&result->several, &STD_VEC_AT(stamps, chunks_TupleLocatorStamps_t, k).f0,
							 sizeof(ChunksTrailerLocator), _Alignof(ChunksTrailerLocator));
}

/* chunks_packs_reads_ambiguous returns the refusal of a read by an address alone whose records hold other stamps. */
static ChunksErrorError
chunks_packs_reads_ambiguous(const ChunksAddressAddress *address, const StdVec *records)
{
	StdString	listed;
	StdString	message;
	StdFmtPiece pieces[] = {STD_FMT_LIT("chunk "), STD_FMT_ARG(0), STD_FMT_LIT(" has records with other stamps, at "),
	STD_FMT_ARG(1), STD_FMT_LIT(", so its address alone names no one record")};
	StdFmtArg	args[] = {{address, chunks_address_Address_Display_fmt}, {&listed, std_String_Display_fmt}};
	StdFmtArguments arguments = {pieces, 5, args};

	std_vec_new(&listed, 1, 1);
	for (size_t k = 0, k_len = records->len; k < k_len; k++)
	{
		const ChunksTrailerLocator *at = &STD_VEC_AT(records, ChunksTrailerLocator, k);
		uint64		offset = chunks_trailer_Locator_offset(at);
		StdString	one;
		StdFmtPiece one_pieces[] = {STD_FMT_LIT("offset "), STD_FMT_ARG(0), STD_FMT_LIT(" of pack "), STD_FMT_ARG(1)};
		StdFmtArg	one_args[] = {{&offset, std_u64_Display_fmt}, {&at->pack, std_u64_Display_fmt}};
		StdFmtArguments one_arguments = {one_pieces, 4, one_args};

		if (k != 0)
			std_string_push_str_Checked(&listed, ", ", 2);
		std_fmt_format_Checked(&one, &one_arguments);
		std_string_push_str_Checked(&listed, one.ptr, one.len);
		std_String_drop(&one);
	}
	std_fmt_format_Checked(&message, &arguments);
	std_String_drop(&listed);
	return chunks_error_Error_corrupt(&message);
}

/* chunks_packs_reads_stamps_of returns the stamps of a chunk's stored bytes. */
static bool
chunks_packs_reads_stamps_of(const ChunksChunkChunk *chunk, ChunksTrailerStamps *result, ChunksErrorError *error)
{
	ChunksTrailerStored stored;

	return chunks_chunk_Chunk_split(chunk, chunks_pack_header_addresses, NULL, NULL, &stored, error) &&
		chunks_trailer_Stored_stamps(&stored, result, error);
}

/* chunks_packs_reads_dead_find reports whether the view's vectors hold a pack's, and where it goes. */
static bool
chunks_packs_reads_dead_find(const StdVec *dead, uint64 number, size_t *at)
{
	*at = 0;
	while (*at < dead->len && STD_VEC_AT(dead, chunks_TupleU64Dead_t, *at).f0 < number)
		(*at)++;
	return *at < dead->len && STD_VEC_AT(dead, chunks_TupleU64Dead_t, *at).f0 == number;
}

/*
 * chunks_packs_reads_View_is_dead reports whether a full collection found
 * the chunk at a locator dead: whether the deletion vector the table names
 * for its pack, which the view reads on first need, holds its offset.
 */
bool
chunks_packs_reads_View_is_dead(ChunksPacksViewView *self, HostIoReadDoors io, ChunksTrailerLocator locator,
								bool *result, ChunksErrorError *error)
{
	const ChunksPacksListed *listed = chunks_packs_table_packs_get(&self->table.list.packs, locator.pack);
	StdArcInner *const *reader = chunks_packs_view_View_pack(self, locator.pack);
	size_t		at;
	chunks_TupleU64Dead_t *dead;

	if (listed == NULL || !listed->has_dead || reader == NULL)
	{
		*result = false;
		return true;
	}
	if (!chunks_packs_reads_dead_find(&self->dead, locator.pack, &at) ||
		STD_VEC_AT(&self->dead, chunks_TupleU64Dead_t, at).generation != listed->dead)
	{
		chunks_TupleU64Dead_t read = {.f0 = locator.pack, .generation = listed->dead};

		if (!chunks_pack_PackReader_dead(std_arc_Deref_deref(*reader, _Alignof(ChunksPackPackReader)), io,
										 listed->dead, &read.dead, error))
			return false;
		if (chunks_packs_reads_dead_find(&self->dead, locator.pack, &at))
		{
			chunks_dead_Dead_drop(&STD_VEC_AT(&self->dead, chunks_TupleU64Dead_t, at).dead);
			STD_VEC_AT(&self->dead, chunks_TupleU64Dead_t, at) = read;
		}
		else
			std_vec_insert(&self->dead, at, &read, sizeof(read), _Alignof(chunks_TupleU64Dead_t), RUST_FILE, 67, 23);
	}
	dead = &STD_VEC_AT(&self->dead, chunks_TupleU64Dead_t, at);
	*result = chunks_dead_Dead_holds(&dead->dead, chunks_trailer_Locator_offset(&locator));
	return true;
}

/*
 * chunks_packs_reads_sorted_frame_0_t is a read's state across its pad over
 * a tablespace's records: the records listed, those dead and live, the
 * stamps read, the chunk read, and the answer named.
 */
typedef struct chunks_packs_reads_sorted_frame_0_t
{
	ChunksPacksViewView *self;
	HostIoReadDoors io;
	const ChunksAddressAddress *address;
	uint32		space;
	ChunksPacksReadsNamed *named;
	ChunksErrorError *error;
	bool		ok;
	StdVec		listed;
	StdVec		dead;
	StdVec		live;
	StdVec		stamps;
	ChunksChunkChunk chunk;
	bool		chunk_live;
} chunks_packs_reads_sorted_frame_0_t;

/* chunks_packs_reads_sorted_pad_0 is the pad of a read over a tablespace's records: what it holds. */
static void
chunks_packs_reads_sorted_pad_0(void *frame_)
{
	chunks_packs_reads_sorted_frame_0_t *frame = frame_;

	if (frame->chunk_live)
	{
		frame->chunk_live = false;
		chunks_chunk_Chunk_drop(&frame->chunk);
	}
	std_vec_drop_Checked(&frame->stamps, sizeof(chunks_TupleLocatorStamps_t), _Alignof(chunks_TupleLocatorStamps_t),
						 chunks_packs_reads_stamped_drop);
	std_vec_new(&frame->stamps, sizeof(chunks_TupleLocatorStamps_t), _Alignof(chunks_TupleLocatorStamps_t));
	std_vec_free_Checked(&frame->live, sizeof(ChunksTrailerLocator), _Alignof(ChunksTrailerLocator));
	std_vec_new(&frame->live, sizeof(ChunksTrailerLocator), _Alignof(ChunksTrailerLocator));
	std_vec_free_Checked(&frame->dead, sizeof(ChunksTrailerLocator), _Alignof(ChunksTrailerLocator));
	std_vec_new(&frame->dead, sizeof(ChunksTrailerLocator), _Alignof(ChunksTrailerLocator));
	std_vec_free_Checked(&frame->listed, sizeof(ChunksTrailerLocator), _Alignof(ChunksTrailerLocator));
	std_vec_new(&frame->listed, sizeof(ChunksTrailerLocator), _Alignof(ChunksTrailerLocator));
}

/* chunks_packs_reads_frame_new makes a frame of a read over a space's records, its lists empty. */
static void
chunks_packs_reads_frame_new(chunks_packs_reads_sorted_frame_0_t *frame)
{
	std_vec_new(&frame->listed, sizeof(ChunksTrailerLocator), _Alignof(ChunksTrailerLocator));
	std_vec_new(&frame->dead, sizeof(ChunksTrailerLocator), _Alignof(ChunksTrailerLocator));
	std_vec_new(&frame->live, sizeof(ChunksTrailerLocator), _Alignof(ChunksTrailerLocator));
	std_vec_new(&frame->stamps, sizeof(chunks_TupleLocatorStamps_t), _Alignof(chunks_TupleLocatorStamps_t));
}

/*
 * chunks_packs_reads_sorted_in adds to the frame's lists the records a
 * tablespace's listed packs hold of the chunk with the address, those a full
 * collection found dead apart from the live ones.
 */
static bool
chunks_packs_reads_sorted_in(chunks_packs_reads_sorted_frame_0_t *frame, uint32 space)
{
	const ChunksIndexAddressIndexLocator *index = chunks_packs_view_View_index(frame->self, space);

	if (index == NULL)
		return true;
	std_vec_free_Checked(&frame->listed, sizeof(ChunksTrailerLocator), _Alignof(ChunksTrailerLocator));
	std_vec_new(&frame->listed, sizeof(ChunksTrailerLocator), _Alignof(ChunksTrailerLocator));
	if (!chunks_index_AddressIndexLocator_all(index, frame->io, frame->address, &frame->listed, frame->error))
		return false;
	for (size_t k = 0, k_len = frame->listed.len; k < k_len; k++)
	{
		ChunksTrailerLocator at = STD_VEC_AT(&frame->listed, ChunksTrailerLocator, k);
		bool		dead;

		if (chunks_packs_view_View_pack(frame->self, at.pack) == NULL)
			continue;
		if (!chunks_packs_reads_View_is_dead(frame->self, frame->io, at, &dead, frame->error))
			return false;
		std_vec_push_Checked(dead ? &frame->dead : &frame->live, &at, sizeof(at), _Alignof(ChunksTrailerLocator));
	}
	return true;
}

/*
 * chunks_packs_reads_named_in_body_0 is named_in's body: what the address
 * alone names among a tablespace's records of it, its live ones before those
 * a full collection found dead.
 */
static void
chunks_packs_reads_named_in_body_0(void *frame_)
{
	chunks_packs_reads_sorted_frame_0_t *frame = frame_;
	const StdVec *chosen;

	frame->ok = false;
	if (!chunks_packs_reads_sorted_in(frame, frame->space))
		goto done;
	chosen = frame->live.len == 0 ? &frame->dead : &frame->live;
	if (chosen->len <= 1)
	{
		frame->named->tag = chosen->len == 0 ? CHUNKS_PACKS_READS_NAMED_MISSING : CHUNKS_PACKS_READS_NAMED_ONE;
		if (chosen->len == 1)
			frame->named->one = STD_VEC_AT(chosen, ChunksTrailerLocator, 0);
		frame->ok = true;
		goto done;
	}
	std_vec_free_Checked(&frame->stamps, sizeof(chunks_TupleLocatorStamps_t), _Alignof(chunks_TupleLocatorStamps_t));
	std_vec_with_capacity_Checked(&frame->stamps, chosen->len, sizeof(chunks_TupleLocatorStamps_t),
								  _Alignof(chunks_TupleLocatorStamps_t));
	for (size_t k = 0, k_len = chosen->len; k < k_len; k++)
	{
		chunks_TupleLocatorStamps_t stamped = {.f0 = STD_VEC_AT(chosen, ChunksTrailerLocator, k)};
		StdArcInner *const *reader = chunks_packs_view_View_pack(frame->self, stamped.f0.pack);
		bool		read;

		if (reader == NULL)
			continue;
		if (!chunks_pack_PackReader_read(std_arc_Deref_deref(*reader, _Alignof(ChunksPackPackReader)), frame->io,
										 stamped.f0, frame->address, &frame->chunk, frame->error))
			goto done;
		frame->chunk_live = true;
		read = chunks_packs_reads_stamps_of(&frame->chunk, &stamped.f1, frame->error);
		frame->chunk_live = false;
		chunks_chunk_Chunk_drop(&frame->chunk);
		if (!read)
			goto done;
		std_vec_push_Checked(&frame->stamps, &stamped, sizeof(stamped), _Alignof(chunks_TupleLocatorStamps_t));
	}
	chunks_packs_reads_named_by_address(&frame->stamps, frame->named);
	frame->ok = true;
done:
	chunks_packs_reads_sorted_pad_0(frame);
}

/*
 * chunks_packs_reads_View_named_in returns what the address alone names
 * among the records a tablespace's listed packs hold of it.
 */
static bool
chunks_packs_reads_View_named_in(ChunksPacksViewView *self, HostIoReadDoors io, const ChunksAddressAddress *address,
								 uint32 space, ChunksPacksReadsNamed *result, ChunksErrorError *error)
{
	chunks_packs_reads_sorted_frame_0_t frame = {.self = self, .io = io, .address = address, .space = space,
	.named = result, .error = error};

	chunks_packs_reads_frame_new(&frame);
	std_landing(&frame, chunks_packs_reads_named_in_body_0, chunks_packs_reads_sorted_pad_0);
	return frame.ok;
}

/* chunks_packs_reads_records_body_0 is records' body: every tablespace's records, the live ones or else the dead. */
static void
chunks_packs_reads_records_body_0(void *frame_)
{
	chunks_packs_reads_sorted_frame_0_t *frame = frame_;
	StdVec		spaces;
	StdVec	   *chosen;

	frame->ok = false;
	std_vec_with_capacity_Checked(&spaces, frame->self->indexes.len, sizeof(uint32), _Alignof(uint32));
	for (size_t k = 0, k_len = frame->self->indexes.len; k < k_len; k++)
		std_vec_push_Checked(&spaces, &STD_VEC_AT(&frame->self->indexes, chunks_TupleU32AddressIndex_t, k).f0,
							 sizeof(uint32), _Alignof(uint32));
	for (size_t k = 0, k_len = spaces.len; k < k_len; k++)
		if (!chunks_packs_reads_sorted_in(frame, STD_VEC_AT(&spaces, uint32, k)))
		{
			std_vec_free_Checked(&spaces, sizeof(uint32), _Alignof(uint32));
			goto done;
		}
	std_vec_free_Checked(&spaces, sizeof(uint32), _Alignof(uint32));
	chosen = frame->live.len == 0 ? &frame->dead : &frame->live;
	*(StdVec *) frame->named = *chosen;
	std_vec_new(chosen, sizeof(ChunksTrailerLocator), _Alignof(ChunksTrailerLocator));
	frame->ok = true;
done:
	chunks_packs_reads_sorted_pad_0(frame);
}

/*
 * chunks_packs_reads_View_records returns every record the listed packs hold
 * of the chunk with the address, from every tablespace's address index: the
 * live ones, or those a full collection found dead when none is live.
 */
bool
chunks_packs_reads_View_records(ChunksPacksViewView *self, HostIoReadDoors io, const ChunksAddressAddress *address,
								StdVec *result, ChunksErrorError *error)
{
	chunks_packs_reads_sorted_frame_0_t frame = {.self = self, .io = io, .address = address,
	.named = (ChunksPacksReadsNamed *) result, .error = error};

	chunks_packs_reads_frame_new(&frame);
	std_landing(&frame, chunks_packs_reads_records_body_0, chunks_packs_reads_sorted_pad_0);
	return frame.ok;
}

/* chunks_packs_reads_answered answers a find from what an address names, refusing records with other stamps. */
static bool
chunks_packs_reads_answered(ChunksPacksReadsNamed *named, const ChunksAddressAddress *address, bool *has_result,
							ChunksTrailerLocator *result, ChunksErrorError *error)
{
	switch (named->tag)
	{
		case CHUNKS_PACKS_READS_NAMED_MISSING:
			*has_result = false;
			return true;
		case CHUNKS_PACKS_READS_NAMED_ONE:
			*has_result = true;
			*result = named->one;
			return true;
		case CHUNKS_PACKS_READS_NAMED_SEVERAL:
			break;
	}
	*error = chunks_packs_reads_ambiguous(address, &named->several);
	std_vec_free_Checked(&named->several, sizeof(ChunksTrailerLocator), _Alignof(ChunksTrailerLocator));
	return false;
}

/*
 * chunks_packs_reads_View_find returns where the chunk with the address is,
 * from each tablespace's address index in order, answering a copy a full
 * collection found dead only when no tablespace holds another.
 */
bool
chunks_packs_reads_View_find(ChunksPacksViewView *self, HostIoReadDoors io, const ChunksAddressAddress *address,
							 bool *has_result, ChunksTrailerLocator *result, ChunksErrorError *error)
{
	bool		has_dead = false;
	ChunksTrailerLocator dead_at;

	for (size_t k = 0; k < self->indexes.len; k++)
	{
		uint32		space = STD_VEC_AT(&self->indexes, chunks_TupleU32AddressIndex_t, k).f0;
		ChunksPacksReadsNamed named;
		bool		dead;

		if (!chunks_packs_reads_View_named_in(self, io, address, space, &named, error))
			return false;
		if (named.tag == CHUNKS_PACKS_READS_NAMED_MISSING)
			continue;
		if (named.tag == CHUNKS_PACKS_READS_NAMED_SEVERAL)
			return chunks_packs_reads_answered(&named, address, has_result, result, error);
		if (!chunks_packs_reads_View_is_dead(self, io, named.one, &dead, error))
			return false;
		if (!dead)
		{
			*has_result = true;
			*result = named.one;
			return true;
		}
		if (!has_dead)
		{
			has_dead = true;
			dead_at = named.one;
		}
	}
	*has_result = has_dead;
	if (has_dead)
		*result = dead_at;
	return true;
}

/*
 * chunks_packs_reads_View_find_in returns where a tablespace's packs hold
 * the chunk with the address, answering a copy a full collection found dead
 * only when the tablespace's packs hold no other.
 */
bool
chunks_packs_reads_View_find_in(ChunksPacksViewView *self, HostIoReadDoors io, const ChunksAddressAddress *address,
								uint32 space, bool *has_result, ChunksTrailerLocator *result, ChunksErrorError *error)
{
	ChunksPacksReadsNamed named;

	return chunks_packs_reads_View_named_in(self, io, address, space, &named, error) &&
		chunks_packs_reads_answered(&named, address, has_result, result, error);
}

/* chunks_packs_reads_missing returns Error::Missing of an address, with no read named. */
static ChunksErrorError
chunks_packs_reads_missing(const ChunksAddressAddress *address)
{
	ChunksErrorError error = {.tag = CHUNKS_ERROR_ERROR_MISSING};

	error.u.missing.f0 = *address;
	error.u.missing.has_f1 = false;
	return error;
}

/* chunks_packs_reads_listed_at returns the tablespace and id the view lists a pack's number with, if it does. */
static bool
chunks_packs_reads_listed_at(const ChunksPacksViewView *self, uint64 number, ChunksPacksTableEntry *result)
{
	const ChunksPacksListed *listed = chunks_packs_table_packs_get(&self->table.list.packs, number);

	for (size_t k = self->deferred.len; k > 0; k--)
		if (STD_VEC_AT(&self->deferred, ChunksPacksTableEntry, k - 1).f0 == number)
		{
			*result = STD_VEC_AT(&self->deferred, ChunksPacksTableEntry, k - 1);
			return true;
		}
	if (listed == NULL)
		return false;
	result->f0 = number;
	result->f1 = listed->space;
	result->f2 = listed->id;
	return true;
}

/*
 * chunks_packs_reads_View_read returns the stored bytes of the chunk with
 * the address: from the locator when its pack is listed, through the
 * forwards when a collection retired its pack, and through the address index
 * when the locator doesn't say where the chunk is or the record there isn't
 * the chunk's.
 */
bool
chunks_packs_reads_View_read(ChunksPacksViewView *self, HostIoReadDoors io, const ChunksAddressAddress *address,
							 ChunksTrailerLocator locator, ChunksChunkChunk *result, ChunksErrorError *error)
{
	bool		has_found;
	ChunksTrailerLocator found;
	StdArcInner *const *reader;

	if (chunks_trailer_Locator_is_known(&locator))
	{
		ChunksTrailerLocator led = locator;
		ChunksPacksTableEntry listed;

		if (chunks_packs_view_View_pack(self, locator.pack) == NULL &&
			!chunks_packs_followed(io, &self->forwards, &self->table.list, locator, &led, error))
			return false;
		reader = chunks_packs_view_View_pack(self, led.pack);
		if (reader != NULL)
		{
			ChunksErrorError failed;
			ChunksErrorOwn own;

			if (chunks_pack_PackReader_read(std_arc_Deref_deref(*reader, _Alignof(ChunksPackPackReader)), io, led,
											address, result, &failed))
				return true;
			if (!chunks_error_Error_own(&failed, &own, error))
				return false;
			chunks_error_Own_drop(&own);
		}
		else if (chunks_packs_reads_listed_at(self, led.pack, &listed))
		{
			*error = chunks_packs_view_View_unread(self, listed.f1, &listed.f2);
			return false;
		}
		else if (locator.pack <= self->table.list.added)
		{
			*error = chunks_packs_reads_missing(address);
			return false;
		}
	}
	if (!chunks_packs_reads_View_find(self, io, address, &has_found, &found, error))
		return false;
	reader = has_found ? chunks_packs_view_View_pack(self, found.pack) : NULL;
	if (reader == NULL)
	{
		*error = chunks_packs_reads_missing(address);
		return false;
	}
	return chunks_pack_PackReader_read(std_arc_Deref_deref(*reader, _Alignof(ChunksPackPackReader)), io, found,
									   address, result, error);
}

#ifdef DOLTGRES_TEST_CHUNKS

/*
 * chunks_packs_reads_tests_found asserts at a line that a view finds an
 * address at a place, or nowhere when `at` is NULL, in a tablespace or, when
 * `space` is negative, in any, with the assertion's and unwrap's columns and
 * a literal message or none.
 */
static void
chunks_packs_reads_tests_found(ChunksPacksViewView *view, HostIoIo *io, const ChunksAddressAddress *address,
							   int64 space, const ChunksTrailerLocator *at, const char *message, uint32 line,
							   uint32 assert_col, uint32 unwrap_col)
{
	ChunksTrailerLocator found;
	ChunksErrorError error;
	bool		has;
	bool		ok = space < 0 ?
		chunks_packs_reads_View_find(view, HOST_IO_IO_READ_DOORS(io), address, &has, &found, &error) :
		chunks_packs_reads_View_find_in(view, HOST_IO_IO_READ_DOORS(io), address, (uint32) space, &has, &found,
										&error);

	if (!ok)
		chunks_packs_tests_unwrapped(RUST_FILE, &error, line, unwrap_col);
	if (has != (at != NULL) || (has && !chunks_trailer_Locator_PartialEq_eq(&found, at)))
	{
		StdOptionRef got = {has, STD_ITEM(&found, chunks_trailer_Locator_Debug_fmt)};
		StdOptionRef want = {at != NULL, STD_ITEM(at, chunks_trailer_Locator_Debug_fmt)};
		StdFmtPiece pieces[] = {{message, message == NULL ? 0 : strlen(message)}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), NULL};

		std_assert_failed(RUST_FILE, line, assert_col, "==", &got, std_option_Debug_fmt, &want, std_option_Debug_fmt,
						  message == NULL ? NULL : &arguments);
	}
}

/* ChunksPacksReadsTestsAdds is three packs added at once, and the line and column each add unwraps at. */
typedef struct ChunksPacksReadsTestsAdds
{
	uint32		line;
	uint32		col;
	ChunksPacksTableEntry entries[3];
} ChunksPacksReadsTestsAdds;

/* chunks_packs_reads_tests_added is a change's body that adds its three packs at once. */
static void
chunks_packs_reads_tests_added(ChunksPacksListsChange *change, void *captures)
{
	const ChunksPacksReadsTestsAdds *adds = captures;
	ChunksErrorError error;

	for (size_t k = 0; k < lengthof(adds->entries); k++)
		if (!chunks_packs_lists_Change_add(change, &adds->entries[k], false, &error))
			chunks_packs_tests_unwrapped(RUST_FILE, &error, adds->line, adds->col);
}

/* ChunksPacksReadsTestsVector is a change's naming of a pack's deletion vector, and the line its unwrap is at. */
typedef struct ChunksPacksReadsTestsVector
{
	uint64		number;
	uint64		generation;
	uint32		line;
} ChunksPacksReadsTestsVector;

/* chunks_packs_reads_tests_named is a change's body that names a pack's deletion vector. */
static void
chunks_packs_reads_tests_named(ChunksPacksListsChange *change, void *captures)
{
	const ChunksPacksReadsTestsVector *vector = captures;
	ChunksErrorError error;

	if (!chunks_packs_lists_Change_name_vector(change, vector->number, vector->generation, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, vector->line, 83);
}

/* chunks_packs_reads_tests_vector names a pack's deletion vector of a generation in a change of a view. */
static void
chunks_packs_reads_tests_vector(HostIoIo *io, HostSharedRecord record, ChunksPacksViewView *view, uint64 number,
								uint64 generation, uint32 line)
{
	ChunksPacksReadsTestsVector vector = {number, generation, line};

	chunks_packs_lists_tests_changed(io, record, view, chunks_packs_reads_tests_named, &vector);
}

/* chunks_packs_reads_tests_dead writes a pack's deletion vector of a generation with one offset dead, which unwraps at a line. */
static void
chunks_packs_reads_tests_dead(HostIoIo *io, const StdPathBuf *dir, const ChunksPackPackId *id, uint64 generation,
							  uint64 offset, uint32 line)
{
	StdString	name = chunks_pack_PackId_file_name(id, "pack", 4);
	StdPathBuf	path;
	StdBTreeMap offsets = std_btree_BTreeSetU64_FromArray_from(&offset, 1);
	ChunksErrorError error;

	std_path_join(dir->bytes.ptr, dir->bytes.len, name.ptr, name.len, &path);
	std_vec_free_Checked(&name, 1, 1);
	if (!chunks_dead_Dead_write(HOST_IO_IO_IO_DOORS(io), path.bytes.ptr, path.bytes.len, generation, &offsets, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, line, 82);
	std_btree_BTreeMapU64Set_drop(&offsets);
	std_vec_free_Checked(&path.bytes, 1, 1);
}

/*
 * test_chunks_packs_reads_tests_a_find_answers_a_live_copy_over_a_dead_one: a
 * find answers a live copy of a chunk over one a full collection found dead,
 * a dead copy only when no other is listed, and fails when a deletion vector
 * the table names can't be read, never taking it for an empty one.
 */
void
test_chunks_packs_reads_tests_a_find_answers_a_live_copy_over_a_dead_one(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	StdPathBuf	dir;
	HostSharedOwned owned;
	HostSharedRecord record;
	ChunksPacksFixturesPacked older;
	ChunksPacksFixturesPacked newer;
	ChunksPacksFixturesPacked other;
	ChunksPacksViewView mine;
	ChunksPacksViewView theirs;
	ChunksPacksViewView *views[2] = {&mine, &theirs};
	ChunksPacksReadsTestsAdds adds;
	ChunksAddressAddress absent = chunks_address_Address_of((const uint8 *) "absent", 6);
	uint64		generation = 4;

	chunks_packs_fixtures_home(&dir);
	owned = chunks_packs_lists_tests_repository(io, &dir);
	record = host_shared_Owned_record(&owned);
	older = chunks_packs_fixtures_pack_of(io, &dir, 1, (const uint8 *) "copied", 6);
	newer = chunks_packs_fixtures_pack_of(io, &dir, 2, (const uint8 *) "copied", 6);
	other = chunks_packs_fixtures_pack(io, &dir, 3);
	chunks_packs_fixtures_view(&mine);
	chunks_packs_fixtures_view(&theirs);
	adds.line = 211;
	adds.col = 89;
	adds.entries[0] = older.entry;
	adds.entries[1] = newer.entry;
	adds.entries[2] = other.entry;
	chunks_packs_lists_tests_changed(io, record, &mine, chunks_packs_reads_tests_added, &adds);
	chunks_packs_reads_tests_found(&mine, io, &older.address, -1, &newer.at, NULL, 213, 9, 43);
	chunks_packs_reads_tests_found(&mine, io, &other.address, 0, &other.at, NULL, 214, 9, 48);
	chunks_packs_reads_tests_found(&mine, io, &absent, -1, NULL, NULL, 215, 9, 59);

	chunks_packs_reads_tests_dead(io, &dir, &newer.entry.f2, generation, chunks_trailer_Locator_offset(&newer.at), 219);
	chunks_packs_reads_tests_vector(io, record, &mine, 2, generation, 220);
	for (size_t k = 0; k < lengthof(views); k++)
	{
		chunks_packs_tests_refreshed(RUST_FILE, views[k], io, record, 222, 38);
		chunks_packs_reads_tests_found(views[k], io, &older.address, -1, &older.at, NULL, 223, 13, 47);
		chunks_packs_reads_tests_found(views[k], io, &other.address, -1, &other.at, NULL, 224, 13, 46);
	}
	chunks_packs_reads_tests_dead(io, &dir, &older.entry.f2, generation, chunks_trailer_Locator_offset(&older.at), 226);
	chunks_packs_reads_tests_vector(io, record, &mine, 1, generation, 227);
	chunks_packs_reads_tests_found(&mine, io, &older.address, -1, &newer.at, "a chunk with only dead copies was not found",
								   228, 9, 43);

	chunks_packs_reads_tests_vector(io, record, &mine, 3, generation, 230);
	chunks_packs_tests_refreshed(RUST_FILE, &theirs, io, record, 231, 36);
	for (size_t k = 0; k < lengthof(views); k++)
	{
		ChunksTrailerLocator found;
		ChunksErrorError failed;
		bool		has;
		static const char tail[] = "is missing";
		bool		missing;

		if (chunks_packs_reads_View_find(views[k], HOST_IO_IO_READ_DOORS(io), &other.address, &has, &found, &failed))
		{
			StdOptionRef ok = {has, STD_ITEM(&found, chunks_trailer_Locator_Debug_fmt)};

			STD_RESULT_EXPECT_FAILED(233, 48, "called `Result::unwrap_err()` on an `Ok` value", &ok,
									 std_option_Debug_fmt);
		}
		missing = failed.tag == CHUNKS_ERROR_ERROR_DAMAGED &&
			failed.u.damaged.message.len >= sizeof(tail) - 1 &&
			memcmp((const char *) failed.u.damaged.message.ptr + failed.u.damaged.message.len - (sizeof(tail) - 1), tail,
				   sizeof(tail) - 1) == 0;
		if (!missing)
		{
			StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
			StdFmtArg	args[] = {{&failed, chunks_error_Error_Debug_fmt}};
			StdFmtArguments arguments = {pieces, lengthof(pieces), args};

			std_panic_fmt(RUST_FILE, 234, 13, &arguments);
		}
		chunks_error_Error_drop(&failed);
	}
	chunks_packs_view_View_drop(&theirs);
	chunks_packs_view_View_drop(&mine);
	host_shared_Owned_drop(&owned);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	simc_TestIo_drop(&test_io);
}

/* chunks_packs_reads_tests_read returns the stored bytes a view reads of an address from a locator, which unwraps at a line and column. */
static StdVec
chunks_packs_reads_tests_read(ChunksPacksViewView *view, HostIoIo *io, const ChunksAddressAddress *address,
							  ChunksTrailerLocator locator, uint32 line, uint32 col)
{
	ChunksChunkChunk chunk;
	ChunksErrorError error;
	StdVec		bytes;

	if (!chunks_packs_reads_View_read(view, HOST_IO_IO_READ_DOORS(io), address, locator, &chunk, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, line, col);
	chunks_chunk_Chunk_into_bytes(&chunk, &bytes);
	return bytes;
}

/* chunks_packs_reads_tests_read_eq asserts at a line and columns that a view reads of an address from a locator the bytes given, or other bytes when `same` is false. */
static void
chunks_packs_reads_tests_read_eq(ChunksPacksViewView *view, HostIoIo *io, const ChunksAddressAddress *address,
								 ChunksTrailerLocator locator, const StdVec *stored, bool same, uint32 line,
								 uint32 assert_col, uint32 unwrap_col)
{
	StdVec		bytes = chunks_packs_reads_tests_read(view, io, address, locator, line, unwrap_col);
	bool		equal = bytes.len == stored->len && memcmp(bytes.ptr, stored->ptr, bytes.len) == 0;

	if (equal != same)
		std_assert_failed(RUST_FILE, line, assert_col, same ? "==" : "!=", &bytes, chunks_chunk_VecU8_Debug_fmt,
						  stored, chunks_chunk_VecU8_Debug_fmt, NULL);
	std_vec_free_Checked(&bytes, 1, 1);
}

/* ChunksPacksReadsTestsRetirement is a_read_follows_its_locator_the_forwards_or_the_index's retirement's captures: the pack it retires. */
typedef struct ChunksPacksReadsTestsRetirement
{
	const ChunksPacksTableEntry *retired;
} ChunksPacksReadsTestsRetirement;

/* chunks_packs_reads_tests_retirement is the retirement's change: the forwards' run, the pack retired, and its run removed. */
static void
chunks_packs_reads_tests_retirement(ChunksPacksListsChange *change, void *captures)
{
	const ChunksPacksReadsTestsRetirement *retirement = captures;
	static const uint64 numbers[] = {1};
	StdString	name = chunks_pack_PackId_file_name(&retirement->retired->f2, "idx", 3);
	StdVec		record;
	ChunksErrorError error;

	chunks_packs_Runs_added("a.fwd:1", 7, &record);
	if (!chunks_packs_lists_Change_change_forwarding(change, record.ptr, record.len, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 262, 63);
	std_vec_free_Checked(&record, 1, 1);
	if (!chunks_packs_lists_Change_retire(change, numbers, lengthof(numbers), &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 263, 33);
	chunks_packs_Runs_removed(&name, 1, &record);
	std_vec_free_Checked(&name, 1, 1);
	if (!chunks_packs_lists_Change_change_runs(change, 0, record.ptr, record.len, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 264, 82);
	std_vec_free_Checked(&record, 1, 1);
}

/* chunks_packs_reads_tests_missing reports whether a read of an address from a locator fails with the address missing, where none is named when `named` is set. */
static bool
chunks_packs_reads_tests_missing(ChunksPacksViewView *view, HostIoIo *io, const ChunksAddressAddress *address,
								 ChunksTrailerLocator locator, bool named)
{
	ChunksChunkChunk chunk;
	ChunksErrorError error;
	bool		missing;

	if (chunks_packs_reads_View_read(view, HOST_IO_IO_READ_DOORS(io), address, locator, &chunk, &error))
	{
		chunks_chunk_Chunk_drop(&chunk);
		return false;
	}
	missing = error.tag == CHUNKS_ERROR_ERROR_MISSING &&
		(!named || (!error.u.missing.has_f1 && chunks_address_Address_PartialEq_eq(&error.u.missing.f0, address)));
	chunks_error_Error_drop(&error);
	return missing;
}

/*
 * test_chunks_packs_reads_tests_a_read_follows_its_locator_the_forwards_or_the_index:
 * a read gives a chunk's bytes from its locator while the locator's pack is
 * listed, through the forwards once a collection retired that pack, and
 * through the address index when the locator doesn't say where the chunk is
 * or names another chunk's record; a locator into a retired pack that no
 * forward leads from, and an address no pack holds, find the chunk missing.
 */
void
test_chunks_packs_reads_tests_a_read_follows_its_locator_the_forwards_or_the_index(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	StdPathBuf	dir;
	StdPathBuf	path;
	HostSharedOwned owned;
	HostSharedRecord record;
	ChunksPacksFixturesPacked retired;
	ChunksPacksFixturesPacked kept;
	ChunksPacksFixturesPacked other;
	ChunksPacksViewView mine;
	ChunksPacksViewView theirs;
	ChunksPacksViewView *views[2] = {&mine, &theirs};
	ChunksPacksReadsTestsAdds adds;
	ChunksPacksReadsTestsRetirement retirement;
	chunks_TupleLocatorLocator_t moved;
	StdVec		entries;
	StdVec		stored;
	ChunksIndexRunLocator run;
	ChunksErrorError error;
	ChunksAddressAddress absent = chunks_address_Address_of((const uint8 *) "absent", 6);

	chunks_packs_fixtures_home(&dir);
	owned = chunks_packs_lists_tests_repository(io, &dir);
	record = host_shared_Owned_record(&owned);
	retired = chunks_packs_fixtures_pack_of(io, &dir, 1, (const uint8 *) "moved", 5);
	kept = chunks_packs_fixtures_pack_of(io, &dir, 2, (const uint8 *) "moved", 5);
	other = chunks_packs_fixtures_pack(io, &dir, 3);
	chunks_packs_fixtures_view(&mine);
	chunks_packs_fixtures_view(&theirs);
	adds.line = 253;
	adds.col = 90;
	adds.entries[0] = retired.entry;
	adds.entries[1] = kept.entry;
	adds.entries[2] = other.entry;
	chunks_packs_lists_tests_changed(io, record, &mine, chunks_packs_reads_tests_added, &adds);
	stored = chunks_packs_reads_tests_read(&mine, io, &retired.address, retired.at, 255, 50);
	chunks_packs_reads_tests_read_eq(&mine, io, &retired.address, chunks_trailer_Locator_Default_default(), &stored,
									 true, 256, 9, 62);
	chunks_packs_reads_tests_read_eq(&mine, io, &retired.address, other.at, &stored, true, 257, 9, 52);
	chunks_packs_reads_tests_read_eq(&mine, io, &other.address, other.at, &stored, false, 258, 9, 52);

	moved.f0 = retired.at;
	moved.f1 = kept.at;
	std_path_join(dir.bytes.ptr, dir.bytes.len, "a.fwd", 5, &path);
	chunks_packs_forward_entries(&moved, 1, &entries);
	if (!chunks_index_RunLocator_write(HOST_IO_IO_IO_DOORS(io), path.bytes.ptr, path.bytes.len, entries.ptr,
									   entries.len, &run, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 260, 82);
	chunks_index_RunLocator_drop(&run);
	std_vec_free_Checked(&entries, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
	std_vec_free_Checked(&path.bytes, 1, 1);
	retirement.retired = &retired.entry;
	chunks_packs_lists_tests_changed(io, record, &mine, chunks_packs_reads_tests_retirement, &retirement);
	chunks_packs_tests_refreshed(RUST_FILE, &theirs, io, record, 266, 36);
	for (size_t k = 0; k < lengthof(views); k++)
	{
		ChunksTrailerLocator unled = chunks_trailer_Locator_at(retired.at.pack,
															   chunks_trailer_Locator_offset(&retired.at) + 1,
															   chunks_trailer_Locator_length(&retired.at));

		chunks_packs_reads_tests_read_eq(views[k], io, &retired.address, retired.at, &stored, true, 268, 13, 52);
		STD_ASSERT(270, 13, chunks_packs_reads_tests_missing(views[k], io, &retired.address, unled, true),
				   "matches!(view.read(io, &moved, unled), Err(Error::Missing(address, None)) if address == moved)");
		STD_ASSERT(272, 13, chunks_packs_reads_tests_missing(views[k], io, &absent,
															 chunks_trailer_Locator_Default_default(), false),
				   "matches!(view.read(io, &absent, Locator::default()), Err(Error::Missing(..)))");
	}
	std_vec_free_Checked(&stored, 1, 1);
	chunks_packs_view_View_drop(&theirs);
	chunks_packs_view_View_drop(&mine);
	host_shared_Owned_drop(&owned);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	simc_TestIo_drop(&test_io);
}

#endif							/* DOLTGRES_TEST_CHUNKS */
