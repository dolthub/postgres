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
 * The collector's collection of a repository's packs: its roots captured, a
 * marking through every copy they reach, the live chunks of mostly dead
 * packs copied and forwarded, and the packs with nothing live retired.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/chunk.h"
#include "doltgres/chunks/dead.h"
#include "doltgres/chunks/file.h"
#include "doltgres/chunks/header.h"
#include "doltgres/chunks/pack.h"
#include "doltgres/chunks/packs.h"
#include "doltgres/chunks/packs/backend.h"
#include "doltgres/chunks/packs/marking.h"
#include "doltgres/chunks/packs/open.h"
#include "doltgres/chunks/packs/removal.h"
#include "doltgres/chunks/packs/retire.h"
#include "doltgres/chunks/packs/roots.h"
#include "doltgres/chunks/packs/shared.h"
#include "doltgres/chunks/trailer.h"
#include "doltgres/host/lib.h"
#include "doltgres/host/shared.h"
#include "doltgres/repo/lib.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/int128.h"
#include "doltgres/std/num.h"
#include "doltgres/std/slice.h"
#include "doltgres/std/str.h"
#include "doltgres/std/sync.h"
#include "doltgres/std/time.h"
#include "doltgres/std/tuple.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"
#include "doltgres/store/collect.h"
#include "doltgres/store/lib.h"
#include "doltgres/store/databases.h"
#include "doltgres/txn/roots.h"
#include "doltgres/txn/state.h"

#ifdef DOLTGRES_TEST_STORE
#include "doltgres/chunks/address.h"
#include "doltgres/chunks/packs/check.h"
#include "doltgres/simc/simc.h"
#include "doltgres/std/test.h"
#include "doltgres/std/test_fs.h"
#include "doltgres/store/fixtures.h"
#endif

#define RUST_FILE "store" STD_SEP "src" STD_SEP "collect.rs"

/* STORE_COLLECT_BATCH is how many copies a marking reaches between its reads of whether a base backup waits. */
#define STORE_COLLECT_BATCH 4096

/* STORE_COLLECT_CARRY_RATIO is how many times its own length the leaf records a copy keeps compressed with a source pack's dictionary must take for the new pack to carry it. */
#define STORE_COLLECT_CARRY_RATIO 32

/* StoreCollectPacer is Pacer: a collection's reads and copies held to a number of bytes a second, none when it is 0. */
typedef struct StoreCollectPacer
{
	uint64		limit;
	bool		has_started;
	StdInstant	started;
	uint64		bytes;
} StoreCollectPacer;

/*
 * StoreCollectMarks is Marks: a collection's marks, the copies it reached by
 * pack and offset; in each pack below the marking's bound, the live copies'
 * offsets and bytes; the forwards it followed; the chunks named by address
 * alone that no pack holds and the delta doesn't hold either; whether it
 * passed over a chunk in a pack still being written; whether it stopped; and
 * the pace of its reads. A young marking reaches the copies in its old packs
 * as leaves.
 */
typedef struct StoreCollectMarks
{
	/* seen is a BTreeSet<(u64, u64)>, a Vec of std_TupleU64U64_t in order. */
	StdVec		seen;
	/* old is a BTreeSet<u64>, a Vec<u64> in order. */
	StdVec		old;

	/*
	 * live is a BTreeMap<u64, BTreeSet<u64>>, a Vec of store_collect_live_t
	 * in pack order.
	 */
	StdVec		live;

	/*
	 * bytes is a BTreeMap<u64, u64>, a Vec of std_TupleU64U64_t in pack
	 * order.
	 */
	StdVec		bytes;

	/*
	 * followed is a BTreeSet<(u64, u64)>, a Vec of std_TupleU64U64_t in
	 * order.
	 */
	StdVec		followed;
	/* unresolved is a Vec<Named> of chunks_TupleAddressLocator_t. */
	StdVec		unresolved;
	bool		passed;

	/*
	 * stopped is whether it stopped for a base backup or a drop or move of
	 * its database that waits, its marks then partial.
	 */
	bool		stopped;
	StoreCollectPacer pacer;

	/*
	 * database is the database whose lock its collection holds, whose waiters
	 * stop it.
	 */
	bool		has_database;
	uint32		database;
} StoreCollectMarks;

/* store_collect_live_t is a pack's entry in a marking's live copies: its number and its live copies' offsets, a BTreeSet<u64>. */
typedef struct store_collect_live_t
{
	uint64		pack;
	StdVec		offsets;
} store_collect_live_t;

/* store_collect_place_cmp is (u64, u64)'s Ord, a pack's number then a record's offset. */
static StdOrdering
store_collect_place_cmp(const void *a, const void *b)
{
	const std_TupleU64U64_t *x = a;
	const std_TupleU64U64_t *y = b;

	if (x->f0 != y->f0)
		return x->f0 < y->f0 ? STD_ORDERING_LESS : STD_ORDERING_GREATER;
	if (x->f1 != y->f1)
		return x->f1 < y->f1 ? STD_ORDERING_LESS : STD_ORDERING_GREATER;
	return STD_ORDERING_EQUAL;
}

/* store_collect_places_insert inserts a place into a BTreeSet<(u64, u64)> and reports whether it was new. */
static bool
store_collect_places_insert(StdVec *set, uint64 pack, uint64 offset)
{
	std_TupleU64U64_t place = {pack, offset};
	size_t		at;

	if (std_slice_binary_search(set->ptr, set->len, sizeof(place), &place, store_collect_place_cmp, &at))
		return false;
	std_vec_insert(set, at, &place, sizeof(place), _Alignof(std_TupleU64U64_t), RUST_FILE, 0, 0);
	return true;
}

/* store_collect_places_contains reports whether a BTreeSet<(u64, u64)> holds a place. */
static bool
store_collect_places_contains(const StdVec *set, uint64 pack, uint64 offset)
{
	std_TupleU64U64_t place = {pack, offset};
	size_t		at;

	return std_slice_binary_search(set->ptr, set->len, sizeof(place), &place, store_collect_place_cmp, &at);
}

/* store_collect_numbers_insert inserts a number into a BTreeSet<u64> and reports whether it was new. */
static bool
store_collect_numbers_insert(StdVec *set, uint64 number)
{
	size_t		at;

	if (std_slice_binary_search(set->ptr, set->len, sizeof(number), &number, std_u64_Ord_cmp, &at))
		return false;
	std_vec_insert(set, at, &number, sizeof(number), _Alignof(uint64), RUST_FILE, 0, 0);
	return true;
}

/* store_collect_numbers_contains reports whether a BTreeSet<u64> holds a number. */
static bool
store_collect_numbers_contains(const StdVec *set, uint64 number)
{
	size_t		at;

	return std_slice_binary_search(set->ptr, set->len, sizeof(number), &number, std_u64_Ord_cmp, &at);
}

/* store_collect_numbers_clone returns a copy of a BTreeSet<u64>, which a marking takes of the old packs as its own. */
static StdVec
store_collect_numbers_clone(const StdVec *set)
{
	StdVec		copy;

	std_slice_to_vec_Checked(&copy, set->ptr, set->len, sizeof(uint64), _Alignof(uint64));
	return copy;
}

/* store_collect_live_entry returns a pack's set of live offsets in a marking's live copies, a new empty one if none. */
static StdVec *
store_collect_live_entry(StdVec *live, uint64 pack)
{
	size_t		at = 0;
	store_collect_live_t entry = {.pack = pack};

	while (at < live->len && STD_VEC_AT(live, store_collect_live_t, at).pack < pack)
		at++;
	if (at == live->len || STD_VEC_AT(live, store_collect_live_t, at).pack != pack)
	{
		std_vec_new(&entry.offsets, sizeof(uint64), _Alignof(uint64));
		std_vec_insert(live, at, &entry, sizeof(entry), _Alignof(store_collect_live_t), RUST_FILE, 0, 0);
	}
	return &STD_VEC_AT(live, store_collect_live_t, at).offsets;
}

/* store_collect_live_get returns a pack's set of live offsets, or NULL for a pack with none. */
static const StdVec *
store_collect_live_get(const StdVec *live, uint64 pack)
{
	for (size_t k = 0, k_len = live->len; k < k_len; k++)
		if (STD_VEC_AT(live, store_collect_live_t, k).pack == pack)
			return &STD_VEC_AT(live, store_collect_live_t, k).offsets;
	return NULL;
}

/* store_collect_bytes_add adds bytes to a pack's count in a BTreeMap<u64, u64>. */
static void
store_collect_bytes_add(StdVec *bytes, uint64 pack, uint64 added)
{
	size_t		at = 0;
	std_TupleU64U64_t entry = {pack, 0};

	while (at < bytes->len && STD_VEC_AT(bytes, std_TupleU64U64_t, at).f0 < pack)
		at++;
	if (at == bytes->len || STD_VEC_AT(bytes, std_TupleU64U64_t, at).f0 != pack)
		std_vec_insert(bytes, at, &entry, sizeof(entry), _Alignof(std_TupleU64U64_t), RUST_FILE, 0, 0);
	STD_VEC_AT(bytes, std_TupleU64U64_t, at).f1 = STD_ADD(u64, STD_VEC_AT(bytes, std_TupleU64U64_t, at).f1, added,
														  139, 21);
}

/* store_collect_bytes_get returns a pack's count of live bytes, zero for a pack with none. */
static uint64
store_collect_bytes_get(const StdVec *bytes, uint64 pack)
{
	for (size_t k = 0, k_len = bytes->len; k < k_len; k++)
		if (STD_VEC_AT(bytes, std_TupleU64U64_t, k).f0 == pack)
			return STD_VEC_AT(bytes, std_TupleU64U64_t, k).f1;
	return 0;
}

/* store_collect_Marks_new returns a marking's empty marks with old packs, a pace, and the database whose lock it holds, when it has one. */
static StoreCollectMarks
store_collect_Marks_new(StdVec old, uint64 limit, bool has_database, uint32 database)
{
	StoreCollectMarks marks = {.old = old, .pacer = {.limit = limit}, .has_database = has_database,
	.database = database};

	std_vec_new(&marks.seen, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
	std_vec_new(&marks.live, sizeof(store_collect_live_t), _Alignof(store_collect_live_t));
	std_vec_new(&marks.bytes, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
	std_vec_new(&marks.followed, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
	std_vec_new(&marks.unresolved, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
	return marks;
}

/* store_collect_Marks_drop drops a marking's marks. */
static void
store_collect_Marks_drop(StoreCollectMarks *self)
{
	std_vec_free_Checked(&self->seen, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
	std_vec_free_Checked(&self->old, sizeof(uint64), _Alignof(uint64));
	for (size_t k = 0, k_len = self->live.len; k < k_len; k++)
		std_vec_free_Checked(&STD_VEC_AT(&self->live, store_collect_live_t, k).offsets, sizeof(uint64),
							 _Alignof(uint64));
	std_vec_free_Checked(&self->live, sizeof(store_collect_live_t), _Alignof(store_collect_live_t));
	std_vec_free_Checked(&self->bytes, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
	std_vec_free_Checked(&self->followed, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
	std_vec_free_Checked(&self->unresolved, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
}

/* store_collect_header_addresses counts the addresses a stored chunk's header names, for a chunk's split. */
static bool
store_collect_header_addresses(void *captures, const uint8 *hashed, size_t hashed_len, size_t *result,
							   ChunksErrorError *error)
{
	ChunksHeaderHeader header;
	size_t		read;

	(void) captures;
	if (!chunks_header_Header_read(hashed, hashed_len, &header, &read, error))
		return false;
	*result = chunks_header_Addresses_len(&header.addresses);
	chunks_header_Header_drop(&header);
	return true;
}

/* store_collect_named_by returns the chunks a stored chunk names, a Vec<Named>, each with the locator its trailer gives. */
static bool
store_collect_named_by(const ChunksChunkChunk *chunk, StdVec *result, HostError *error)
{
	ChunksTrailerStored split;
	ChunksErrorError failed;
	StdVec		addresses;

	if (!chunks_chunk_Chunk_split(chunk, store_collect_header_addresses, NULL, NULL, &split, &failed) ||
		!chunks_header_addresses(split.hashed.ptr, split.hashed.len, &addresses, &failed))
	{
		*error = chunks_packs_shared_raised(&failed);
		return false;
	}
	std_vec_with_capacity_Checked(result, addresses.len, sizeof(chunks_TupleAddressLocator_t),
								  _Alignof(chunks_TupleAddressLocator_t));
	for (size_t at = 0, at_len = addresses.len; at < at_len; at++)
	{
		chunks_TupleAddressLocator_t named = {STD_VEC_AT(&addresses, ChunksAddressAddress, at),
		chunks_trailer_Stored_locator(&split, at)};

		std_vec_push_Checked(result, &named, sizeof(named), _Alignof(chunks_TupleAddressLocator_t));
	}
	std_vec_free_Checked(&addresses, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
	return true;
}

/*
 * store_collect_nanos is u128::from(bytes) * 1_000_000_000 / u128::from(limit)
 * as u64::try_from(..).unwrap_or(u64::MAX): the exact quotient of the 128-bit
 * product by the limit, which is not 0, or u64::MAX when it doesn't fit.
 */
static uint64
store_collect_nanos(uint64 bytes, uint64 limit)
{
	StdU128		product = std_mul_u64_wide(bytes, UINT64CONST(1000000000));
	uint64		quotient = 0;

	if (product.hi >= limit)
		return PG_UINT64_MAX;
	for (int bit = 63; bit >= 0; bit--)
	{
		bool		carry = (product.hi >> 63) != 0;

		product.hi = (product.hi << 1) | (product.lo >> 63);
		product.lo <<= 1;
		if (carry || product.hi >= limit)
		{
			product.hi -= limit;
			quotient |= UINT64CONST(1) << bit;
		}
	}
	return quotient;
}

/*
 * store_collect_Pacer_pace counts bytes read or copied and naps until they
 * fit the limit, first checking for a cancel or a stop, as Doltgrust's pace
 * checks its stop before anything else, so a marking or a copy stops within
 * a chunk (bugs row 503).
 */
static bool
store_collect_Pacer_pace(StoreCollectPacer *self, HostIoIo *io, uint64 bytes, HostError *error)
{
	StdDuration due;
	StdDuration wait;
	StdInstant	now;

	if (!host_io_Io_check(io, error))
		return false;
	if (self->limit == 0)
		return true;
	if (!self->has_started)
	{
		self->started = host_instant();
		self->has_started = true;
	}
	self->bytes = STD_ADD(u64, self->bytes, bytes, 83, 9);
	due = std_Duration_from_nanos(store_collect_nanos(self->bytes, self->limit));
	now = host_instant();
	if (!std_Duration_checked_sub(due, std_Instant_saturating_duration_since(&now, self->started), &wait))
		return true;
	return host_nap(HOST_IO_IO_IO_DOORS(io), wait, error);
}

/* store_collect_Marks_waited reports whether a session waits for the lock the collection holds on its database. */
static bool
store_collect_Marks_waited(const StoreCollectMarks *self, HostIoIo *io, bool *result, HostError *error)
{
	*result = false;
	return !self->has_database || host_database_lock_waited(io, self->database, result, error);
}

/*
 * store_collect_Marks_copy_of returns the copy of a chunk a locator names:
 * that copy while its pack is listed, its forward when a collection retired
 * its pack, recorded as followed, and otherwise, and for a chunk named by
 * address alone, none.
 */
static bool
store_collect_Marks_copy_of(StoreCollectMarks *self, HostIoIo *io, const ChunksPacksBackendPacks *packs,
							const HostSharedReader *reading, ChunksTrailerLocator named, bool *has_result,
							ChunksTrailerLocator *result, HostError *error)
{
	ChunksErrorError failed;
	StdArcInner *reader;
	bool		has;

	*has_result = false;
	if (!chunks_trailer_Locator_is_known(&named))
		return true;
	if (!chunks_packs_backend_Packs_pack_reader(packs, io, reading, named.pack, &has, &reader, &failed))
		goto raised;
	if (has)
	{
		chunks_pack_ArcPackReader_drop(reader);
		*has_result = true;
		*result = named;
		return true;
	}
	if (!chunks_packs_backend_Packs_forwarded(packs, io, reading, named, has_result, result, &failed))
		goto raised;
	if (*has_result)
		(void) store_collect_places_insert(&self->followed, named.pack, chunks_trailer_Locator_offset(&named));
	return true;
raised:
	*error = chunks_packs_shared_raised(&failed);
	return false;
}

/* store_collect_mark_frame_0_t is mark's state across its pad: the chunks to reach, the packs touched, a copy's records, a chunk read, and its names. */
typedef struct store_collect_mark_frame_0_t
{
	StoreCollectMarks *self;
	HostIoIo   *io;
	const ChunksPacksBackendPacks *packs;
	const HostSharedReader *reading;
	uint64		bound;
	const StdVec *outside;
	StdVec	   *pending;
	StdVec	   *result;
	HostError  *error;
	bool		ok;
	/* records is a Vec<Locator> of a chunk's records named by address alone. */
	StdVec		records;
	bool		records_live;
	ChunksChunkChunk chunk;
	bool		chunk_live;
	/* names is a Vec<Named> of what a chunk names. */
	StdVec		names;
	bool		names_live;
} store_collect_mark_frame_0_t;

/* store_collect_mark_pad_0 is mark's pad: a chunk's names, the chunk, its records, the packs touched on a failure, then the chunks to reach. */
static void
store_collect_mark_pad_0(void *frame_)
{
	store_collect_mark_frame_0_t *frame = frame_;

	if (frame->names_live)
	{
		frame->names_live = false;
		std_vec_free_Checked(&frame->names, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
	}
	if (frame->chunk_live)
	{
		frame->chunk_live = false;
		chunks_chunk_Chunk_drop(&frame->chunk);
	}
	if (frame->records_live)
	{
		frame->records_live = false;
		std_vec_free_Checked(&frame->records, sizeof(ChunksTrailerLocator), _Alignof(ChunksTrailerLocator));
	}
	if (!frame->ok)
		std_vec_free_Checked(frame->result, sizeof(uint64), _Alignof(uint64));
	std_vec_free_Checked(frame->pending, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
}

/* store_collect_address_cmp is Address's Ord, for a search of a BTreeSet<Address>. */
static StdOrdering
store_collect_address_cmp(const void *a, const void *b)
{
	return chunks_address_Address_Ord_cmp(a, b);
}

/*
 * store_collect_mark_locator reaches one copy of a chunk: it marks the copy
 * live below the bound, reads it, paces the read, and adds what it names to
 * the chunks to reach, reporting through stopped when a waiter stops the
 * marking.
 */
static bool
store_collect_mark_locator(store_collect_mark_frame_0_t *frame, const ChunksAddressAddress *address,
						   ChunksTrailerLocator locator, bool *stopped)
{
	StoreCollectMarks *self = frame->self;
	uint64		offset = chunks_trailer_Locator_offset(&locator);
	ChunksErrorError failed;
	bool		waited;

	*stopped = false;
	if (!store_collect_places_insert(&self->seen, locator.pack, offset) ||
		store_collect_numbers_contains(&self->old, locator.pack))
		return true;
	if (locator.pack < frame->bound && store_collect_numbers_insert(store_collect_live_entry(&self->live, locator.pack),
																	offset))
	{
		store_collect_bytes_add(&self->bytes, locator.pack, chunks_trailer_Locator_length(&locator));
		(void) store_collect_numbers_insert(frame->result, locator.pack);
	}
	if (!chunks_packs_backend_Packs_read(frame->packs, frame->io, frame->reading, address, locator, &frame->chunk,
										 &failed))
	{
		*frame->error = chunks_packs_shared_raised(&failed);
		return false;
	}
	frame->chunk_live = true;
	if (self->pacer.limit != 0)
	{
		if (!store_collect_Marks_waited(self, frame->io, &waited, frame->error))
			return false;
		if (waited)
		{
			self->stopped = true;
			*stopped = true;
			return true;
		}
	}
	if (!store_collect_Pacer_pace(&self->pacer, frame->io, chunks_trailer_Locator_length(&locator), frame->error) ||
		!store_collect_named_by(&frame->chunk, &frame->names, frame->error))
		return false;
	frame->names_live = true;
	frame->chunk_live = false;
	chunks_chunk_Chunk_drop(&frame->chunk);
	if (!std_vec_try_reserve(frame->pending, frame->names.len, sizeof(chunks_TupleAddressLocator_t),
							 _Alignof(chunks_TupleAddressLocator_t)))
	{
		*frame->error = host_Error_out_of_memory();
		return false;
	}
	for (size_t k = 0, k_len = frame->names.len; k < k_len; k++)
		std_vec_push_Checked(frame->pending, &STD_VEC_AT(&frame->names, chunks_TupleAddressLocator_t, k),
							 sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
	frame->names_live = false;
	std_vec_free_Checked(&frame->names, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
	return true;
}

/* store_collect_mark_batch reports through stopped whether a base backup or a waiter for the database stops the marking at a batch's start. */
static bool
store_collect_mark_batch(store_collect_mark_frame_0_t *frame, bool *stopped)
{
	*stopped = false;
	if (frame->self->seen.len % STORE_COLLECT_BATCH != 0)
		return true;
	if (!host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-collector-batch", frame->error))
		return false;
	if (host_backups_running())
		*stopped = true;
	else if (!store_collect_Marks_waited(frame->self, frame->io, stopped, frame->error))
		return false;
	if (*stopped)
		frame->self->stopped = true;
	return true;
}

/* store_collect_mark_body_0 is mark's body: each chunk to reach, its copy or its records, and what each names. */
static void
store_collect_mark_body_0(void *frame_)
{
	store_collect_mark_frame_0_t *frame = frame_;
	StoreCollectMarks *self = frame->self;
	ChunksErrorError failed;

	frame->ok = false;
	std_vec_new(frame->result, sizeof(uint64), _Alignof(uint64));
	while (frame->pending->len != 0)
	{
		chunks_TupleAddressLocator_t named = STD_VEC_AT(frame->pending, chunks_TupleAddressLocator_t,
														frame->pending->len - 1);
		ChunksTrailerLocator copy;
		bool		has_copy;
		bool		stopped = false;
		size_t		at;

		frame->pending->len--;
		if (!store_collect_mark_batch(frame, &stopped))
			goto done;
		if (stopped)
			break;
		if (!store_collect_Marks_copy_of(self, frame->io, frame->packs, frame->reading, named.f1, &has_copy, &copy,
										 frame->error))
			goto done;
		if (has_copy)
			std_vec_new(&frame->records, sizeof(ChunksTrailerLocator), _Alignof(ChunksTrailerLocator));
		else if (!chunks_packs_backend_Packs_records(frame->packs, frame->io, frame->reading, &named.f0,
													 &frame->records, &failed))
		{
			*frame->error = chunks_packs_shared_raised(&failed);
			goto done;
		}
		frame->records_live = true;
		if (!has_copy && frame->records.len == 0)
		{
			if (chunks_trailer_Locator_is_known(&named.f1) &&
				chunks_packs_backend_Packs_writing(frame->packs, frame->io, named.f1.pack))
				self->passed = true;
			else if (!std_slice_binary_search(frame->outside->ptr, frame->outside->len, sizeof(ChunksAddressAddress),
											  &named.f0, store_collect_address_cmp, &at))
			{
				if (!std_vec_try_reserve(&self->unresolved, 1, sizeof(chunks_TupleAddressLocator_t),
										 _Alignof(chunks_TupleAddressLocator_t)))
				{
					*frame->error = host_Error_out_of_memory();
					goto done;
				}
				std_vec_push_Checked(&self->unresolved, &named, sizeof(named), _Alignof(chunks_TupleAddressLocator_t));
			}
		}
		else if (has_copy)
		{
			if (!store_collect_mark_locator(frame, &named.f0, copy, &stopped))
				goto done;
		}
		else
		{
			for (size_t k = 0, k_len = frame->records.len; k < k_len && !stopped; k++)
				if (!store_collect_mark_locator(frame, &named.f0, STD_VEC_AT(&frame->records, ChunksTrailerLocator, k),
												&stopped))
					goto done;
		}
		frame->records_live = false;
		std_vec_free_Checked(&frame->records, sizeof(ChunksTrailerLocator), _Alignof(ChunksTrailerLocator));
		if (stopped)
			break;
	}
	frame->ok = true;
done:
	store_collect_mark_pad_0(frame);
}

/*
 * store_collect_Marks_mark follows the references from chunks, a Vec<Named>
 * it takes, through every copy the packs hold, marking live those in packs
 * numbered below a bound, the marking's, and returns the packs it marked a
 * chunk in for the first time, a BTreeSet<u64>. A chunk named by its address
 * alone reaches every record of it. A chunk the delta of chunks holds
 * (outside, a BTreeSet<Address>) is in no pack.
 */
static bool
store_collect_Marks_mark(StoreCollectMarks *self, HostIoIo *io, const ChunksPacksBackendPacks *packs,
						 const HostSharedReader *reading, uint64 bound, const StdVec *outside, StdVec *pending,
						 StdVec *result, HostError *error)
{
	store_collect_mark_frame_0_t frame = {.self = self, .io = io, .packs = packs, .reading = reading, .bound = bound,
	.outside = outside, .pending = pending, .result = result, .error = error};

	std_landing(&frame, store_collect_mark_body_0, store_collect_mark_pad_0);
	return frame.ok;
}

/* StoreCollectKept is Kept: a record a copy keeps, read once before any is written, its source pack's place among the sources, its locator and address, the slot of its dictionary, whether it is a tree's leaf, and the known locators its trailer gives. */
typedef struct StoreCollectKept
{
	size_t		source;
	ChunksTrailerLocator locator;
	ChunksAddressAddress address;
	bool		has_slot;
	size_t		slot;
	bool		leaf;
	/* named is a Vec<Locator>. */
	StdVec		named;
} StoreCollectKept;

/* store_collect_source_t is a copy's source pack: its reader, an Arc<PackReader>, and its number. */
typedef struct store_collect_source_t
{
	StdArcInner *pack;
	uint64		number;
} store_collect_source_t;

/* store_collect_covered_t is an entry of a copy's covered bytes: a source pack's place and a dictionary's slot, and the leaf bytes kept with it. */
typedef struct store_collect_covered_t
{
	size_t		source;
	size_t		slot;
	uint64		bytes;
} store_collect_covered_t;

/* store_collect_dictionary_t is a dictionary's bytes with its Covering: the leaf bytes it would cover, and the source pack and slot of each copy of it, a Vec<(usize, usize)>. */
typedef struct store_collect_dictionary_t
{
	StdVec		dictionary;
	uint64		bytes;
	StdVec		slots;
} store_collect_dictionary_t;

/* store_collect_carried_t is a carried dictionary's entry: a source pack's place and slot, and the chosen dictionary carried for it. */
typedef struct store_collect_carried_t
{
	size_t		source;
	size_t		slot;
	const StdVec *dictionary;
} store_collect_carried_t;

/* store_collect_copied_t is a copied record's entry: its old pack and offset, and its copy's locator. */
typedef struct store_collect_copied_t
{
	std_TupleU64U64_t from;
	ChunksTrailerLocator copy;
} store_collect_copied_t;

/* store_collect_keep_t is copy_packs' keep, a &mut dyn FnMut(u64, Locator) -> bool: its call and its captures. */
typedef struct store_collect_keep_t
{
	bool		(*call) (void *captures, uint64 number, ChunksTrailerLocator locator);
	void	   *captures;
} store_collect_keep_t;

/* store_collect_Kept_drop drops the records a copy keeps, a Vec<Kept>. */
static void
store_collect_Kept_drop(StdVec *kept)
{
	for (size_t k = 0, k_len = kept->len; k < k_len; k++)
		std_vec_free_Checked(&STD_VEC_AT(kept, StoreCollectKept, k).named, sizeof(ChunksTrailerLocator),
							 _Alignof(ChunksTrailerLocator));
	std_vec_free_Checked(kept, sizeof(StoreCollectKept), _Alignof(StoreCollectKept));
}

/* store_collect_Dictionaries_drop drops a copy's dictionaries with their coverings. */
static void
store_collect_Dictionaries_drop(StdVec *dictionaries)
{
	for (size_t k = 0, k_len = dictionaries->len; k < k_len; k++)
	{
		store_collect_dictionary_t *entry = &STD_VEC_AT(dictionaries, store_collect_dictionary_t, k);

		std_vec_free_Checked(&entry->dictionary, 1, 1);
		std_vec_free_Checked(&entry->slots, sizeof(std_TupleUsizeUsize_t), _Alignof(std_TupleUsizeUsize_t));
	}
	std_vec_free_Checked(dictionaries, sizeof(store_collect_dictionary_t), _Alignof(store_collect_dictionary_t));
}

/* store_collect_kept_frame_t is a source pack's verify's captures in copy_packs: the keep, the source, and the records kept. */
typedef struct store_collect_kept_frame_t
{
	const store_collect_keep_t *keep;
	size_t		source;
	uint64		number;
	StdVec	   *kept;
} store_collect_kept_frame_t;

/* store_collect_kept_closure is copy_packs' verify closure: a record its keep keeps, read with its trailer's known locators. */
static bool
store_collect_kept_closure(void *captures, ChunksFileInterruptible io, ChunksPackVerified *chunk,
						   ChunksErrorError *error)
{
	store_collect_kept_frame_t *frame = captures;
	ChunksTrailerLocator locator = chunks_pack_Verified_locator(chunk);
	StoreCollectKept kept = {.source = frame->source, .locator = locator};
	ChunksTrailerStored split;
	ChunksHeaderHeader header;
	StdVec		addresses;
	size_t		count;
	size_t		read;

	(void) io;
	if (!frame->keep->call(frame->keep->captures, frame->number, locator))
		return true;
	if (!chunks_pack_Verified_split(chunk, store_collect_header_addresses, NULL, NULL, &split, error) ||
		!chunks_header_addresses(split.hashed.ptr, split.hashed.len, &addresses, error))
		return false;
	count = addresses.len;
	std_vec_free_Checked(&addresses, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
	std_vec_new(&kept.named, sizeof(ChunksTrailerLocator), _Alignof(ChunksTrailerLocator));
	for (size_t at = 0; at < count; at++)
	{
		ChunksTrailerLocator named = chunks_trailer_Stored_locator(&split, at);

		if (chunks_trailer_Locator_is_known(&named))
			std_vec_push_Checked(&kept.named, &named, sizeof(named), _Alignof(ChunksTrailerLocator));
	}
	if (!chunks_header_Header_read(split.hashed.ptr, split.hashed.len, &header, &read, error))
	{
		std_vec_free_Checked(&kept.named, sizeof(ChunksTrailerLocator), _Alignof(ChunksTrailerLocator));
		return false;
	}
	kept.leaf = header.kind == CHUNKS_HEADER_TREE_LEAF;
	chunks_header_Header_drop(&header);
	kept.address = chunks_pack_Verified_address(chunk);
	kept.has_slot = chunks_pack_Verified_slot(chunk, &kept.slot);
	std_vec_push_Checked(frame->kept, &kept, sizeof(kept), _Alignof(StoreCollectKept));
	return true;
}

/* store_collect_copy_packs_frame_0_t is copy_packs' state across its pad: what it read and chose, and a record's copy in progress. */
typedef struct store_collect_copy_packs_frame_0_t
{
	HostIoIo   *io;
	ChunksPackPackWriterInDir *writer;
	const ChunksPacksBackendPacks *packs;
	const HostSharedReader *reading;
	const store_collect_source_t *sources;
	size_t		sources_len;
	const store_collect_keep_t *keep;
	StoreCollectPacer *pacer;
	StdVec	   *result;
	HostError  *error;
	bool		ok;
	/* kept is a Vec<Kept>. */
	StdVec		kept;
	bool		kept_live;
	ChunksPackLost lost;
	bool		lost_live;
	StdString	message;
	bool		message_live;
	/* copying is a BTreeSet<(u64, u64)> of the records being copied. */
	StdVec		copying;
	bool		copying_live;

	/*
	 * moves_children is a Vec<bool>, whether each kept record names a copy
	 * moving.
	 */
	StdVec		moves_children;
	bool		moves_children_live;

	/*
	 * covered is a BTreeMap<(usize, usize), u64> of store_collect_covered_t
	 * in order.
	 */
	StdVec		covered;
	bool		covered_live;
	StdVec		dictionary;
	bool		dictionary_live;

	/*
	 * dictionaries is by_bytes, then chosen: store_collect_dictionary_t in
	 * the dictionaries' order, then by bytes.
	 */
	StdVec		dictionaries;
	bool		dictionaries_live;

	/*
	 * carried is a BTreeMap<(usize, usize), Arc<Vec<u8>>> of
	 * store_collect_carried_t in order, borrowing the chosen.
	 */
	StdVec		carried;
	bool		carried_live;

	/*
	 * copied is a BTreeMap<(u64, u64), Locator> of store_collect_copied_t in
	 * order.
	 */
	StdVec		copied;
	bool		copied_live;
	ChunksPackRaw raw;
	bool		raw_live;
	ChunksChunkChunk chunk;
	bool		chunk_live;
	/* locators is a Vec<Locator> of a record's trailer. */
	StdVec		locators;
	bool		locators_live;
	ChunksTrailerStamps stamps;
	bool		stamps_live;
	StdVec		stored;
	bool		stored_live;
} store_collect_copy_packs_frame_0_t;

/* store_collect_copy_packs_pad_0 is copy_packs' pad: a record's copy in progress, then what it chose and read, and its moves on a failure. */
static void
store_collect_copy_packs_pad_0(void *frame_)
{
	store_collect_copy_packs_frame_0_t *frame = frame_;

	if (frame->stored_live)
	{
		frame->stored_live = false;
		std_vec_free_Checked(&frame->stored, 1, 1);
	}
	if (frame->stamps_live)
	{
		frame->stamps_live = false;
		chunks_trailer_Stamps_drop(&frame->stamps);
	}
	if (frame->locators_live)
	{
		frame->locators_live = false;
		std_vec_free_Checked(&frame->locators, sizeof(ChunksTrailerLocator), _Alignof(ChunksTrailerLocator));
	}
	if (frame->chunk_live)
	{
		frame->chunk_live = false;
		chunks_chunk_Chunk_drop(&frame->chunk);
	}
	if (frame->raw_live)
	{
		frame->raw_live = false;
		chunks_pack_Raw_drop(&frame->raw);
	}
	if (frame->copied_live)
	{
		frame->copied_live = false;
		std_vec_free_Checked(&frame->copied, sizeof(store_collect_copied_t), _Alignof(store_collect_copied_t));
	}
	if (frame->carried_live)
	{
		frame->carried_live = false;
		std_vec_free_Checked(&frame->carried, sizeof(store_collect_carried_t), _Alignof(store_collect_carried_t));
	}
	if (frame->dictionaries_live)
	{
		frame->dictionaries_live = false;
		store_collect_Dictionaries_drop(&frame->dictionaries);
	}
	if (frame->dictionary_live)
	{
		frame->dictionary_live = false;
		std_vec_free_Checked(&frame->dictionary, 1, 1);
	}
	if (frame->covered_live)
	{
		frame->covered_live = false;
		std_vec_free_Checked(&frame->covered, sizeof(store_collect_covered_t), _Alignof(store_collect_covered_t));
	}
	if (frame->moves_children_live)
	{
		frame->moves_children_live = false;
		std_vec_free_Checked(&frame->moves_children, sizeof(bool), _Alignof(bool));
	}
	if (frame->copying_live)
	{
		frame->copying_live = false;
		std_vec_free_Checked(&frame->copying, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
	}
	if (frame->message_live)
	{
		frame->message_live = false;
		std_String_drop(&frame->message);
	}
	if (frame->lost_live)
	{
		frame->lost_live = false;
		chunks_pack_Lost_drop(&frame->lost);
	}
	if (frame->kept_live)
	{
		frame->kept_live = false;
		store_collect_Kept_drop(&frame->kept);
	}
	if (!frame->ok)
		std_vec_free_Checked(frame->result, sizeof(chunks_packs_backend_Packs_forward_moves_t),
							 _Alignof(chunks_packs_backend_Packs_forward_moves_t));
}

/* store_collect_reader returns a source pack's reader. */
static const ChunksPackPackReader *
store_collect_reader(const store_collect_source_t *source)
{
	return std_arc_Deref_deref(source->pack, _Alignof(ChunksPackPackReader));
}

/* store_collect_copy_read reads the records each source pack keeps, failing with the data-corrupted SQLSTATE on a record that doesn't read whole. */
static bool
store_collect_copy_read(store_collect_copy_packs_frame_0_t *frame)
{
	ChunksFileInterruptible interruptible = {&CHUNKS_FILE_IO_INTERRUPTIBLE_OPS, frame->io};
	ChunksErrorError failed;

	std_vec_new(&frame->kept, sizeof(StoreCollectKept), _Alignof(StoreCollectKept));
	frame->kept_live = true;
	for (size_t source = 0; source < frame->sources_len; source++)
	{
		const ChunksPackPackReader *pack = store_collect_reader(&frame->sources[source]);
		store_collect_kept_frame_t captures = {frame->keep, source, frame->sources[source].number, &frame->kept};
		const char *path;
		size_t		path_len;
		bool		has_message;

		if (!chunks_pack_PackReader_verified(pack, interruptible, frame->sources[source].number,
											 store_collect_kept_closure, &captures, NULL, &frame->lost, &failed))
		{
			*frame->error = chunks_packs_shared_raised(&failed);
			return false;
		}
		frame->lost_live = true;
		path = chunks_pack_PackReader_path(pack, &path_len);
		has_message = chunks_pack_Lost_message(&frame->lost, path, path_len, &frame->message);
		frame->lost_live = false;
		chunks_pack_Lost_drop(&frame->lost);
		if (has_message)
		{
			*frame->error = host_Error_fork((const uint8 *) "XX001", &frame->message);
			return false;
		}
	}
	return true;
}

/* store_collect_copy_moving finds whether each record kept names a copy being moved or a place a collection forwarded. */
static bool
store_collect_copy_moving(store_collect_copy_packs_frame_0_t *frame)
{
	ChunksErrorError failed;

	std_vec_new(&frame->copying, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
	frame->copying_live = true;
	for (size_t k = 0, k_len = frame->kept.len; k < k_len; k++)
	{
		const StoreCollectKept *kept = &STD_VEC_AT(&frame->kept, StoreCollectKept, k);

		(void) store_collect_places_insert(&frame->copying, kept->locator.pack,
										   chunks_trailer_Locator_offset(&kept->locator));
	}
	std_vec_with_capacity_Checked(&frame->moves_children, frame->kept.len, sizeof(bool), _Alignof(bool));
	frame->moves_children_live = true;
	for (size_t k = 0, k_len = frame->kept.len; k < k_len; k++)
	{
		const StoreCollectKept *record = &STD_VEC_AT(&frame->kept, StoreCollectKept, k);
		bool		moving = false;

		for (size_t n = 0, n_len = record->named.len; n < n_len; n++)
		{
			ChunksTrailerLocator named = STD_VEC_AT(&record->named, ChunksTrailerLocator, n);
			ChunksTrailerLocator copy;
			bool		has;

			if (store_collect_places_contains(&frame->copying, named.pack, chunks_trailer_Locator_offset(&named)))
				has = true;
			else if (!chunks_packs_backend_Packs_forwarded(frame->packs, frame->io, frame->reading, named, &has,
														   &copy, &failed))
			{
				*frame->error = chunks_packs_shared_raised(&failed);
				return false;
			}
			moving = moving || has;
		}
		std_vec_push_Checked(&frame->moves_children, &moving, sizeof(moving), _Alignof(bool));
	}
	return true;
}

/* store_collect_covered_add adds a kept leaf's bytes to its source pack's slot's count in covered. */
static void
store_collect_covered_add(StdVec *covered, size_t source, size_t slot, uint64 bytes)
{
	size_t		at = 0;
	store_collect_covered_t entry = {source, slot, 0};

	while (at < covered->len && (STD_VEC_AT(covered, store_collect_covered_t, at).source < source ||
								 (STD_VEC_AT(covered, store_collect_covered_t, at).source == source &&
								  STD_VEC_AT(covered, store_collect_covered_t, at).slot < slot)))
		at++;
	if (at == covered->len || STD_VEC_AT(covered, store_collect_covered_t, at).source != source ||
		STD_VEC_AT(covered, store_collect_covered_t, at).slot != slot)
		std_vec_insert(covered, at, &entry, sizeof(entry), _Alignof(store_collect_covered_t), RUST_FILE, 0, 0);
	STD_VEC_AT(covered, store_collect_covered_t, at).bytes =
		STD_ADD(u64, STD_VEC_AT(covered, store_collect_covered_t, at).bytes, bytes, 258, 13);
}

/* store_collect_bytes_cmp is Vec<u8>'s Ord: the bytes compared in order, then the lengths. */
static int
store_collect_bytes_cmp(const StdVec *a, const StdVec *b)
{
	int			c = memcmp(a->ptr, b->ptr, Min(a->len, b->len));

	if (c != 0)
		return c;
	return a->len < b->len ? -1 : a->len > b->len ? 1 : 0;
}

/* store_collect_copy_choose chooses the dictionaries the new pack carries: those whose kept leaves take CARRY_RATIO times their length, the MAX_DICTIONARIES - 1 that cover the most. */
static bool
store_collect_copy_choose(store_collect_copy_packs_frame_0_t *frame)
{
	ChunksErrorError failed;
	size_t		kept = 0;

	std_vec_new(&frame->covered, sizeof(store_collect_covered_t), _Alignof(store_collect_covered_t));
	frame->covered_live = true;
	for (size_t k = 0, k_len = frame->kept.len; k < k_len; k++)
	{
		const StoreCollectKept *record = &STD_VEC_AT(&frame->kept, StoreCollectKept, k);

		if (record->leaf && !STD_VEC_AT(&frame->moves_children, bool, k) && record->has_slot)
			store_collect_covered_add(&frame->covered, record->source, record->slot,
									  chunks_trailer_Locator_length(&record->locator));
	}
	std_vec_new(&frame->dictionaries, sizeof(store_collect_dictionary_t), _Alignof(store_collect_dictionary_t));
	frame->dictionaries_live = true;
	for (size_t k = 0, k_len = frame->covered.len; k < k_len; k++)
	{
		const store_collect_covered_t *covered = &STD_VEC_AT(&frame->covered, store_collect_covered_t, k);
		std_TupleUsizeUsize_t slot = {covered->source, covered->slot};
		store_collect_dictionary_t *entry;
		size_t		at = 0;
		int			c = 1;

		if (!chunks_pack_PackReader_dictionary(store_collect_reader(&frame->sources[covered->source]), frame->io,
											   covered->slot, &frame->dictionary, &failed))
		{
			*frame->error = chunks_packs_shared_raised(&failed);
			return false;
		}
		frame->dictionary_live = true;
		while (at < frame->dictionaries.len &&
			   (c = store_collect_bytes_cmp(&STD_VEC_AT(&frame->dictionaries, store_collect_dictionary_t, at).dictionary,
											&frame->dictionary)) < 0)
			at++;
		if (at == frame->dictionaries.len || c != 0)
		{
			store_collect_dictionary_t made = {.dictionary = frame->dictionary};

			std_vec_new(&made.slots, sizeof(std_TupleUsizeUsize_t), _Alignof(std_TupleUsizeUsize_t));
			std_vec_insert(&frame->dictionaries, at, &made, sizeof(made), _Alignof(store_collect_dictionary_t),
						   RUST_FILE, 0, 0);
			frame->dictionary_live = false;
		}
		else
		{
			frame->dictionary_live = false;
			std_vec_free_Checked(&frame->dictionary, 1, 1);
		}
		entry = &STD_VEC_AT(&frame->dictionaries, store_collect_dictionary_t, at);
		entry->bytes = STD_ADD(u64, entry->bytes, covered->bytes, 265, 9);
		std_vec_push_Checked(&entry->slots, &slot, sizeof(slot), _Alignof(std_TupleUsizeUsize_t));
	}
	for (size_t k = 0, k_len = frame->dictionaries.len; k < k_len; k++)
	{
		store_collect_dictionary_t *entry = &STD_VEC_AT(&frame->dictionaries, store_collect_dictionary_t, k);

		if (entry->bytes >= STD_MUL(u64, STORE_COLLECT_CARRY_RATIO, (uint64) entry->dictionary.len, 270, 54))
			STD_VEC_AT(&frame->dictionaries, store_collect_dictionary_t, kept++) = *entry;
		else
		{
			std_vec_free_Checked(&entry->dictionary, 1, 1);
			std_vec_free_Checked(&entry->slots, sizeof(std_TupleUsizeUsize_t), _Alignof(std_TupleUsizeUsize_t));
		}
	}
	frame->dictionaries.len = kept;
	for (size_t k = 1; k < frame->dictionaries.len; k++)
	{
		store_collect_dictionary_t entry = STD_VEC_AT(&frame->dictionaries, store_collect_dictionary_t, k);
		size_t		at = k;

		while (at > 0 && STD_VEC_AT(&frame->dictionaries, store_collect_dictionary_t, at - 1).bytes < entry.bytes)
		{
			STD_VEC_AT(&frame->dictionaries, store_collect_dictionary_t, at) =
				STD_VEC_AT(&frame->dictionaries, store_collect_dictionary_t, at - 1);
			at--;
		}
		STD_VEC_AT(&frame->dictionaries, store_collect_dictionary_t, at) = entry;
	}
	while (frame->dictionaries.len > CHUNKS_PACK_MAX_DICTIONARIES - 1)
	{
		store_collect_dictionary_t *entry = &STD_VEC_AT(&frame->dictionaries, store_collect_dictionary_t,
														frame->dictionaries.len - 1);

		std_vec_free_Checked(&entry->dictionary, 1, 1);
		std_vec_free_Checked(&entry->slots, sizeof(std_TupleUsizeUsize_t), _Alignof(std_TupleUsizeUsize_t));
		frame->dictionaries.len--;
	}
	std_vec_new(&frame->carried, sizeof(store_collect_carried_t), _Alignof(store_collect_carried_t));
	frame->carried_live = true;
	for (size_t k = 0, k_len = frame->dictionaries.len; k < k_len; k++)
	{
		const store_collect_dictionary_t *entry = &STD_VEC_AT(&frame->dictionaries, store_collect_dictionary_t, k);

		for (size_t s = 0, s_len = entry->slots.len; s < s_len; s++)
		{
			std_TupleUsizeUsize_t slot = STD_VEC_AT(&entry->slots, std_TupleUsizeUsize_t, s);
			store_collect_carried_t carried = {slot.f0, slot.f1, &entry->dictionary};
			size_t		at = 0;

			while (at < frame->carried.len &&
				   (STD_VEC_AT(&frame->carried, store_collect_carried_t, at).source < slot.f0 ||
					(STD_VEC_AT(&frame->carried, store_collect_carried_t, at).source == slot.f0 &&
					 STD_VEC_AT(&frame->carried, store_collect_carried_t, at).slot < slot.f1)))
				at++;
			if (at < frame->carried.len && STD_VEC_AT(&frame->carried, store_collect_carried_t, at).source == slot.f0 &&
				STD_VEC_AT(&frame->carried, store_collect_carried_t, at).slot == slot.f1)
				STD_VEC_AT(&frame->carried, store_collect_carried_t, at) = carried;
			else
				std_vec_insert(&frame->carried, at, &carried, sizeof(carried), _Alignof(store_collect_carried_t),
							   RUST_FILE, 0, 0);
		}
	}
	return true;
}

/* store_collect_carried_get returns the dictionary carried for a source pack's slot, or NULL for none. */
static const StdVec *
store_collect_carried_get(const StdVec *carried, size_t source, size_t slot)
{
	for (size_t k = 0, k_len = carried->len; k < k_len; k++)
		if (STD_VEC_AT(carried, store_collect_carried_t, k).source == source &&
			STD_VEC_AT(carried, store_collect_carried_t, k).slot == slot)
			return STD_VEC_AT(carried, store_collect_carried_t, k).dictionary;
	return NULL;
}

/* store_collect_copied_get returns the copy of a record copied, reporting whether it was copied. */
static bool
store_collect_copied_get(const StdVec *copied, uint64 pack, uint64 offset, ChunksTrailerLocator *result)
{
	std_TupleU64U64_t from = {pack, offset};
	size_t		at;

	if (!std_slice_binary_search(copied->ptr, copied->len, sizeof(store_collect_copied_t), &from,
								 store_collect_place_cmp, &at))
		return false;
	*result = STD_VEC_AT(copied, store_collect_copied_t, at).copy;
	return true;
}

/* store_collect_copied_insert records a record's copy, in place of an earlier copy of it. */
static void
store_collect_copied_insert(StdVec *copied, uint64 pack, uint64 offset, ChunksTrailerLocator copy)
{
	store_collect_copied_t entry = {{pack, offset}, copy};
	size_t		at;

	if (std_slice_binary_search(copied->ptr, copied->len, sizeof(entry), &entry.from, store_collect_place_cmp, &at))
		STD_VEC_AT(copied, store_collect_copied_t, at) = entry;
	else
		std_vec_insert(copied, at, &entry, sizeof(entry), _Alignof(store_collect_copied_t), RUST_FILE, 0, 0);
}

/* store_collect_copy_rewritten writes a record again, its trailer's copies and forwards replaced, compressed again. */
static bool
store_collect_copy_rewritten(store_collect_copy_packs_frame_0_t *frame, const StoreCollectKept *record,
							 ChunksTrailerLocator *result)
{
	const ChunksPackPackReader *pack = store_collect_reader(&frame->sources[record->source]);
	ChunksTrailerStored split;
	ChunksErrorError failed;
	StdVec		addresses;
	size_t		count;
	bool		repaired = false;
	StdBytes	bytes;

	if (!chunks_pack_PackReader_read(pack, HOST_IO_IO_READ_DOORS(frame->io), record->locator, &record->address,
									 &frame->chunk, &failed))
		goto raised;
	frame->chunk_live = true;
	if (!chunks_chunk_Chunk_split(&frame->chunk, store_collect_header_addresses, NULL, NULL, &split, &failed) ||
		!chunks_header_addresses(split.hashed.ptr, split.hashed.len, &addresses, &failed))
		goto raised;
	count = addresses.len;
	std_vec_free_Checked(&addresses, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
	std_vec_with_capacity_Checked(&frame->locators, count, sizeof(ChunksTrailerLocator), _Alignof(ChunksTrailerLocator));
	frame->locators_live = true;
	for (size_t at = 0; at < count; at++)
	{
		ChunksTrailerLocator named = chunks_trailer_Stored_locator(&split, at);

		std_vec_push_Checked(&frame->locators, &named, sizeof(named), _Alignof(ChunksTrailerLocator));
	}
	for (size_t at = 0; at < count; at++)
	{
		ChunksTrailerLocator *named = &STD_VEC_AT(&frame->locators, ChunksTrailerLocator, at);
		ChunksTrailerLocator copy;
		bool		has;

		if (!chunks_trailer_Locator_is_known(named))
			continue;
		has = store_collect_copied_get(&frame->copied, named->pack, chunks_trailer_Locator_offset(named), &copy);
		if (!has && !chunks_packs_backend_Packs_forwarded(frame->packs, frame->io, frame->reading, *named, &has,
														  &copy, &failed))
			goto raised;
		if (has)
		{
			*named = copy;
			repaired = true;
		}
	}
	if (repaired)
	{
		if (!chunks_trailer_Stored_stamps(&split, &frame->stamps, &failed))
			goto raised;
		frame->stamps_live = true;
		chunks_trailer_store(split.hashed.ptr, split.hashed.len, frame->locators.ptr, frame->locators.len,
							 &frame->stamps, &frame->stored);
		frame->stamps_live = false;
		chunks_trailer_Stamps_drop(&frame->stamps);
	}
	else
	{
		bytes = chunks_chunk_Chunk_bytes(&frame->chunk);
		std_slice_to_vec_Checked(&frame->stored, bytes.ptr, bytes.len, 1, 1);
	}
	frame->stored_live = true;
	if (!chunks_pack_PackWriterInDir_write(frame->writer, frame->io, record->address, frame->stored.ptr,
										   frame->stored.len, record->leaf, result, &failed))
		goto raised;
	frame->stored_live = false;
	std_vec_free_Checked(&frame->stored, 1, 1);
	frame->locators_live = false;
	std_vec_free_Checked(&frame->locators, sizeof(ChunksTrailerLocator), _Alignof(ChunksTrailerLocator));
	frame->chunk_live = false;
	chunks_chunk_Chunk_drop(&frame->chunk);
	return true;
raised:
	*frame->error = chunks_packs_shared_raised(&failed);
	return false;
}

/* store_collect_copy_packs_body_0 is copy_packs' body: the records kept read, the dictionaries chosen, then each record copied. */
static void
store_collect_copy_packs_body_0(void *frame_)
{
	store_collect_copy_packs_frame_0_t *frame = frame_;
	ChunksErrorError failed;

	frame->ok = false;
	std_vec_new(frame->result, sizeof(chunks_packs_backend_Packs_forward_moves_t),
				_Alignof(chunks_packs_backend_Packs_forward_moves_t));
	if (!store_collect_copy_read(frame) || !store_collect_copy_moving(frame) || !store_collect_copy_choose(frame))
		goto done;
	std_vec_new(&frame->copied, sizeof(store_collect_copied_t), _Alignof(store_collect_copied_t));
	frame->copied_live = true;
	for (size_t k = 0, k_len = frame->kept.len; k < k_len; k++)
	{
		const StoreCollectKept *record = &STD_VEC_AT(&frame->kept, StoreCollectKept, k);
		const ChunksPackPackReader *pack = store_collect_reader(&frame->sources[record->source]);
		const StdVec *dictionary = record->has_slot ?
			store_collect_carried_get(&frame->carried, record->source, record->slot) : NULL;
		chunks_packs_backend_Packs_forward_moves_t move = {.f0 = record->locator};
		bool		has_copy = false;

		if (!STD_VEC_AT(&frame->moves_children, bool, k) && (!record->has_slot || dictionary != NULL))
		{
			if (!chunks_pack_PackReader_raw(pack, frame->io, record->locator, &record->address, &frame->raw, &failed))
			{
				*frame->error = chunks_packs_shared_raised(&failed);
				goto done;
			}
			frame->raw_live = true;
			if (!chunks_pack_PackWriterInDir_copy(frame->writer, frame->io, record->address, &frame->raw,
												  dictionary == NULL ? NULL : dictionary->ptr,
												  dictionary == NULL ? 0 : dictionary->len, &has_copy, &move.f1,
												  &failed))
			{
				*frame->error = chunks_packs_shared_raised(&failed);
				goto done;
			}
			frame->raw_live = false;
			chunks_pack_Raw_drop(&frame->raw);
		}
		if (!has_copy && !store_collect_copy_rewritten(frame, record, &move.f1))
			goto done;
		store_collect_copied_insert(&frame->copied, record->locator.pack, chunks_trailer_Locator_offset(&record->locator),
									move.f1);
		if (!store_collect_Pacer_pace(frame->pacer, frame->io, chunks_trailer_Locator_length(&record->locator),
									  frame->error))
			goto done;
		if (!std_vec_try_reserve(frame->result, 1, sizeof(move), _Alignof(chunks_packs_backend_Packs_forward_moves_t)))
		{
			*frame->error = host_Error_out_of_memory();
			goto done;
		}
		std_vec_push_Checked(frame->result, &move, sizeof(move), _Alignof(chunks_packs_backend_Packs_forward_moves_t));
	}
	frame->ok = true;
done:
	store_collect_copy_packs_pad_0(frame);
}

/*
 * store_collect_copy_packs writes the records of packs that keep keeps into a
 * pack being written, in pack order, each under its address, and returns
 * each copied record's old locator with its copy's, a Moves. A record whose
 * trailer names a copy being moved or a place a collection forwarded is
 * written again with those locators replaced by the copies, compressed
 * again. Every other record keeps its compressed bytes, carrying the
 * dictionary it names when the leaf records kept that way with that
 * dictionary take CARRY_RATIO times its length and it is among the
 * MAX_DICTIONARIES - 1 that cover the most; the rest are compressed again
 * with the new pack's own. A source pack's record that doesn't read whole is
 * the collection's error. The pacer paces its copies.
 */
static bool
store_collect_copy_packs(HostIoIo *io, ChunksPackPackWriterInDir *writer, const ChunksPacksBackendPacks *packs,
						 const HostSharedReader *reading, const store_collect_source_t *sources, size_t sources_len,
						 const store_collect_keep_t *keep, StoreCollectPacer *pacer, StdVec *result, HostError *error)
{
	store_collect_copy_packs_frame_0_t frame = {.io = io, .writer = writer, .packs = packs, .reading = reading,
	.sources = sources, .sources_len = sources_len, .keep = keep, .pacer = pacer, .result = result, .error = error};

	std_landing(&frame, store_collect_copy_packs_body_0, store_collect_copy_packs_pad_0);
	return frame.ok;
}

/* store_collect_trees_extend appends the manifests' trees to the chunks a marking starts from, each as its address and locator, and frees the trees, a Vec<Tree>. */
static void
store_collect_trees_extend(StdVec *pending, StdVec *trees)
{
	std_vec_reserve_Checked(pending, trees->len, sizeof(chunks_TupleAddressLocator_t),
							_Alignof(chunks_TupleAddressLocator_t));
	for (size_t k = 0, k_len = trees->len; k < k_len; k++)
	{
		const TreesStoreTree *tree = &STD_VEC_AT(trees, TreesStoreTree, k);
		chunks_TupleAddressLocator_t named = {tree->address, tree->locator};

		std_vec_push_Checked(pending, &named, sizeof(named), _Alignof(chunks_TupleAddressLocator_t));
	}
	std_vec_free_Checked(trees, sizeof(TreesStoreTree), _Alignof(TreesStoreTree));
}

/* store_collect_pending returns the chunks a marking starts from, a Vec<Named>: the registered roots, then the states' and deltas', then the manifests'. */
static bool
store_collect_pending(HostIoIo *io, const RepoRepo *repo, TxnRootsCaptured *captured, StdVec *result,
					  HostError *error)
{
	ChunksErrorError failed;
	StdVec		manifests;

	if (!chunks_packs_roots_Packs_registered(&repo->packs, io, result, &failed))
		goto raised;
	if (!repo_Repo_roots(repo, io, &manifests, &failed))
	{
		std_vec_free_Checked(result, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
		goto raised;
	}
	std_vec_extend_from_slice_Checked(result, captured->roots.ptr, captured->roots.len, sizeof(chunks_TupleAddressLocator_t),
									  _Alignof(chunks_TupleAddressLocator_t));
	std_vec_free_Checked(&captured->roots, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
	std_vec_new(&captured->roots, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
	store_collect_trees_extend(result, &manifests);
	return true;
raised:
	*error = chunks_packs_shared_raised(&failed);
	return false;
}

/* store_collect_verified_frame_0_t is verified's state across its pad: the retirement, the numbers listed again, a capture, its marks, and what it wants listed again. */
typedef struct store_collect_verified_frame_0_t
{
	HostIoIo   *io;
	const RepoRepo *repo;
	const HostSharedReader *reading;
	uint32		database;
	const StdVec *old;
	ChunksPacksRetireRetired *retired;
	bool		retired_live;
	StdVec	   *result;
	HostError  *error;
	bool		ok;
	TxnRootsCaptured captured;
	bool		captured_live;
	StoreCollectMarks marks;
	bool		marks_live;
	/* wanted is a BTreeSet<u64> of the retired packs to list again. */
	StdVec		wanted;
	bool		wanted_live;
	StdString	message;
	bool		message_live;

	/*
	 * holding is whether it holds the process's interrupts, which its pad
	 * gives back.
	 */
	bool		holding;
} store_collect_verified_frame_0_t;

/* store_collect_verified_pad_0 is verified's pad: the interrupts it holds, a heal's message, the packs wanted, the marks, the capture, the numbers on a failure, then the retirement. */
static void
store_collect_verified_pad_0(void *frame_)
{
	store_collect_verified_frame_0_t *frame = frame_;

	if (frame->holding)
	{
		frame->holding = false;
		host_io_Io_resume(frame->io);
	}
	if (frame->message_live)
	{
		frame->message_live = false;
		std_String_drop(&frame->message);
	}
	if (frame->wanted_live)
	{
		frame->wanted_live = false;
		std_vec_free_Checked(&frame->wanted, sizeof(uint64), _Alignof(uint64));
	}
	if (frame->marks_live)
	{
		frame->marks_live = false;
		store_collect_Marks_drop(&frame->marks);
	}
	if (frame->captured_live)
	{
		frame->captured_live = false;
		txn_roots_Captured_drop(&frame->captured);
	}
	if (!frame->ok)
		std_vec_free_Checked(frame->result, sizeof(uint64), _Alignof(uint64));
	if (frame->retired_live)
	{
		frame->retired_live = false;
		chunks_packs_retire_Retired_drop(frame->retired);
	}
}

/* store_collect_capture_redo stores a capture's redo point, zero for none, into the store's record; the collector is its only writer. */
static bool
store_collect_capture_redo(HostLsn redo, HostError *error)
{
	HostSharedRecord record;
	HostSharedOp load = {.tag = HOST_SHARED_OP_LOAD};
	HostSharedOp exchange = {.tag = HOST_SHARED_OP_EXCHANGE};

	if (!host_shared_single(&STORE_STORE_STATE_SINGLE_OPS, &record, error))
		return false;
	exchange.u.exchange.f0 = host_shared_Record_atomic(record, STORE_CAPTURE_REDO_INDEX, load);
	exchange.u.exchange.f1 = redo;
	(void) host_shared_Record_atomic(record, STORE_CAPTURE_REDO_INDEX, exchange);
	return true;
}

/* store_collect_captured_frame_0_t is captured's state across its pad: its arguments, and whether it keeps the WAL. */
typedef struct store_collect_captured_frame_0_t
{
	HostIoIo   *io;
	uint32		database;
	TxnRootsCaptured *result;
	HostError  *error;
	bool		ok;
	bool		holding;
} store_collect_captured_frame_0_t;

/* store_collect_captured_body_0 is captured's body. */
static void
store_collect_captured_body_0(void *frame_)
{
	store_collect_captured_frame_0_t *frame = frame_;
	HostIoIoDoors doors = HOST_IO_IO_IO_DOORS(frame->io);
	HostLsn		redo;
	HostLsn		again;

	if (!host_last_redo(doors, &redo, frame->error) || !store_collect_capture_redo(redo, frame->error))
		return;
	frame->holding = true;
	if (!host_last_redo(doors, &again, frame->error))
		return;
	frame->ok = txn_roots_roots(frame->io, frame->database, Max(redo, again), frame->result, frame->error);
}

/* store_collect_captured_pad_0 is captured's pad: it stops keeping the WAL. */
static void
store_collect_captured_pad_0(void *frame_)
{
	store_collect_captured_frame_0_t *frame = frame_;
	HostError	failed;

	if (frame->holding && !store_collect_capture_redo(0, &failed))
		host_Error_drop(&failed);
	frame->holding = false;
}

/*
 * store_collect_captured captures a repository's roots for a collection, the
 * WAL's from the last checkpoint's redo point among them, as Doltgrust's
 * capture_roots does, keeping that WAL while it reads, as its collection's
 * hold of the WAL does: it stores the redo point for KeepLogSeg, reads it
 * again, and reads the WAL from the newer, since a checkpoint that moved it
 * before the store recycled only what recovery no longer reads (bugs row
 * 406).
 */
static bool
store_collect_captured(HostIoIo *io, uint32 database, TxnRootsCaptured *result, HostError *error)
{
	store_collect_captured_frame_0_t frame = {.io = io, .database = database, .result = result, .error = error};

	std_landing(&frame, store_collect_captured_body_0, store_collect_captured_pad_0);
	store_collect_captured_pad_0(&frame);
	return frame.ok;
}

/* store_collect_verified_marked marks from a fresh capture of the roots and finds the retired packs to list again, none when it reaches every chunk. */
static bool
store_collect_verified_marked(store_collect_verified_frame_0_t *frame)
{
	const ChunksPacksBackendPacks *packs = &frame->repo->packs;
	const uint64 *remaining;
	size_t		remaining_len;
	StdVec		pending;
	StdVec		touched;

	if (!store_collect_captured(frame->io, frame->database, &frame->captured, frame->error))
		return false;
	frame->captured_live = true;
	if (!store_collect_pending(frame->io, frame->repo, &frame->captured, &pending, frame->error))
		return false;
	frame->marks = store_collect_Marks_new(store_collect_numbers_clone(frame->old), 0, false, 0);
	frame->marks_live = true;
	if (!store_collect_Marks_mark(&frame->marks, frame->io, packs, frame->reading, 0, &frame->captured.outside,
								  &pending, &touched, frame->error))
		return false;
	std_vec_free_Checked(&touched, sizeof(uint64), _Alignof(uint64));
	remaining = chunks_packs_retire_Retired_numbers(frame->retired, &remaining_len);
	std_vec_new(&frame->wanted, sizeof(uint64), _Alignof(uint64));
	frame->wanted_live = true;
	for (size_t k = 0, k_len = frame->marks.unresolved.len; k < k_len; k++)
	{
		ChunksTrailerLocator named = STD_VEC_AT(&frame->marks.unresolved, chunks_TupleAddressLocator_t, k).f1;

		for (size_t r = 0; r < remaining_len; r++)
			if (!chunks_trailer_Locator_is_known(&named) || remaining[r] == named.pack)
				(void) store_collect_numbers_insert(&frame->wanted, remaining[r]);
	}
	frame->marks_live = false;
	store_collect_Marks_drop(&frame->marks);
	frame->captured_live = false;
	txn_roots_Captured_drop(&frame->captured);
	return true;
}

/*
 * store_collect_verified_restored, for a verify that failed before its
 * queue, lists again every pack the retirement still holds, as Doltgrust's
 * settle unretires them, so that a failed check never deletes a pack (bugs
 * row 503); the vectors no listed pack names then are the next marking's
 * pass's to queue (H14). Interrupts wait meanwhile, since a pending
 * termination would fail each restore's own checks. The verify's error
 * stays the answer unless a restore fails, whose error then replaces it.
 */
static void
store_collect_verified_restored(store_collect_verified_frame_0_t *frame)
{
	ChunksErrorError failed;
	uint64		fresh;
	bool		ok = true;

	host_io_Io_hold(frame->io);
	frame->holding = true;
	while (ok && frame->retired->listed.len != 0)
		ok = chunks_packs_retire_Retired_restore(frame->retired, frame->io, frame->reading,
												 STD_VEC_AT(&frame->retired->listed, ChunksPacksTableEntry, 0).f0,
												 &fresh, &failed);
	frame->holding = false;
	host_io_Io_resume(frame->io);
	if (!ok)
	{
		host_Error_drop(frame->error);
		*frame->error = chunks_packs_shared_raised(&failed);
	}
}

/* store_collect_verified_body_0 is verified's body: marking again until it reaches every chunk, each retired pack it wants listed again, then the rest queued. */
static void
store_collect_verified_body_0(void *frame_)
{
	store_collect_verified_frame_0_t *frame = frame_;
	ChunksErrorError failed;

	frame->ok = false;
	std_vec_new(frame->result, sizeof(uint64), _Alignof(uint64));
	if (!host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-collector-retired", frame->error))
		goto restored;
	for (;;)
	{
		if (!store_collect_verified_marked(frame))
			goto restored;
		if (frame->wanted.len == 0)
			break;
		for (size_t k = 0, k_len = frame->wanted.len; k < k_len; k++)
		{
			uint64		number = STD_VEC_AT(&frame->wanted, uint64, k);
			uint64		fresh;
			StdFmtPiece pieces[] = {STD_FMT_LIT("collection of repository "), STD_FMT_ARG(0),
			STD_FMT_LIT(" listed retired pack "), STD_FMT_ARG(1), STD_FMT_LIT(" again as "), STD_FMT_ARG(2)};
			StdFmtArg	args[] = {{&frame->database, std_u32_Display_fmt}, {&number, std_u64_Display_fmt},
			{&fresh, std_u64_Display_fmt}};
			StdFmtArguments arguments = {pieces, lengthof(pieces), args};
			bool		logged;

			if (!chunks_packs_retire_Retired_restore(frame->retired, frame->io, frame->reading, number, &fresh, &failed))
			{
				*frame->error = chunks_packs_shared_raised(&failed);
				goto restored;
			}
			if (!std_vec_try_reserve(frame->result, 1, sizeof(uint64), _Alignof(uint64)))
			{
				*frame->error = host_Error_out_of_memory();
				goto restored;
			}
			std_vec_push_Checked(frame->result, &fresh, sizeof(fresh), _Alignof(uint64));
			std_fmt_format_Checked(&frame->message, &arguments);
			frame->message_live = true;
			std_string_push_str_Checked(&frame->message, "", 1);
			logged = host_log(HOST_IO_IO_IO_DOORS(frame->io), frame->message.ptr, frame->error);
			frame->message_live = false;
			std_String_drop(&frame->message);
			if (!logged)
				goto restored;
		}
		frame->wanted_live = false;
		std_vec_free_Checked(&frame->wanted, sizeof(uint64), _Alignof(uint64));
	}
	frame->retired_live = false;
	if (!chunks_packs_retire_Retired_queue(frame->retired, frame->io, frame->reading, &failed))
	{
		*frame->error = chunks_packs_shared_raised(&failed);
		goto done;
	}
	frame->ok = true;
	goto done;

restored:
	store_collect_verified_restored(frame);
done:
	store_collect_verified_pad_0(frame);
}

/*
 * store_collect_verified runs a collection's verify after its retirement,
 * which it takes: a marking from a fresh capture of the roots that lists
 * again each retired pack holding a chunk it reaches no other copy of, one a
 * named locator places in that pack, and every retired pack for a chunk
 * named by address alone that nothing holds, reporting each such heal at LOG
 * and marking again until it reaches every chunk, and then queues the files
 * of the rest. A young collection's verify reaches its old packs' copies as
 * leaves. It returns the numbers it listed packs again under.
 */
static bool
store_collect_verified(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading, uint32 database,
					   const StdVec *old, ChunksPacksRetireRetired *retired, StdVec *result, HostError *error)
{
	store_collect_verified_frame_0_t frame = {.io = io, .repo = repo, .reading = reading, .database = database,
	.old = old, .retired = retired, .retired_live = true, .result = result, .error = error};

	std_landing(&frame, store_collect_verified_body_0, store_collect_verified_pad_0);
	return frame.ok;
}

/* store_collect_over reports whether pack bytes pass live bytes by more than a percent of them, as u128s. */
static bool
store_collect_over(uint64 total, uint64 live, uint64 percent)
{
	StdU128		left = std_mul_u64_wide(total, 100);
	StdU128		share = std_mul_u64_wide(Max(live, 1), percent);
	StdU128		whole = std_mul_u64_wide(Max(live, 1), 100);
	StdU128		right;

	right.lo = share.lo + whole.lo;
	right.hi = share.hi + whole.hi + (right.lo < share.lo ? 1 : 0);
	STD_CHECKED(right.hi < share.hi || (right.hi == share.hi && right.lo < share.lo), RUST_FILE, 376, 31,
				"attempt to multiply with overflow");
	return std_u128_lt(right, left);
}

#ifdef DOLTGRES_TEST_STORE

/*
 * STORE_COLLECT_HIDDEN is a root a test hides from the next first capture of
 * a database's collection, standing for a root a capture missed, which the
 * verify must reach.
 */
static struct
{
	bool		has;
	uint32		of;
	ChunksAddressAddress address;
}			STORE_COLLECT_HIDDEN;

#endif

/* store_collect_space_t is a tablespace's entry in a collection's copies: its id and the numbers of the packs copied out into it, a Vec<u64>. */
typedef struct store_collect_space_t
{
	uint32		space;
	StdVec		numbers;
} store_collect_space_t;

/* store_collect_partly_t is a pack a collection keeps with dead copies: its number and their offsets, a BTreeSet<u64>. */
typedef struct store_collect_partly_t
{
	uint64		number;
	StdBTreeMap offsets;
} store_collect_partly_t;

/* store_collect_Spaces_drop drops a collection's copies by tablespace. */
static void
store_collect_Spaces_drop(StdVec *copied)
{
	for (size_t k = 0, k_len = copied->len; k < k_len; k++)
		std_vec_free_Checked(&STD_VEC_AT(copied, store_collect_space_t, k).numbers, sizeof(uint64), _Alignof(uint64));
	std_vec_free_Checked(copied, sizeof(store_collect_space_t), _Alignof(store_collect_space_t));
}

/* store_collect_Partly_drop drops the packs a collection keeps with dead copies. */
static void
store_collect_Partly_drop(StdVec *partly)
{
	for (size_t k = 0, k_len = partly->len; k < k_len; k++)
		std_btree_BTreeMapU64Set_drop(&STD_VEC_AT(partly, store_collect_partly_t, k).offsets);
	std_vec_free_Checked(partly, sizeof(store_collect_partly_t), _Alignof(store_collect_partly_t));
}

/* store_collect_Sources_drop drops a copy's source packs, the readers' Arcs. */
static void
store_collect_Sources_drop(StdVec *sources)
{
	for (size_t k = 0, k_len = sources->len; k < k_len; k++)
		chunks_pack_ArcPackReader_drop(STD_VEC_AT(sources, store_collect_source_t, k).pack);
	std_vec_free_Checked(sources, sizeof(store_collect_source_t), _Alignof(store_collect_source_t));
}

/* store_collect_copied_push appends a pack's number to its tablespace's copies. */
static void
store_collect_copied_push(StdVec *copied, uint32 space, uint64 number)
{
	size_t		at = 0;
	store_collect_space_t entry = {.space = space};

	while (at < copied->len && STD_VEC_AT(copied, store_collect_space_t, at).space < space)
		at++;
	if (at == copied->len || STD_VEC_AT(copied, store_collect_space_t, at).space != space)
	{
		std_vec_new(&entry.numbers, sizeof(uint64), _Alignof(uint64));
		std_vec_insert(copied, at, &entry, sizeof(entry), _Alignof(store_collect_space_t), RUST_FILE, 0, 0);
	}
	std_vec_push_Checked(&STD_VEC_AT(copied, store_collect_space_t, at).numbers, &number, sizeof(number),
						 _Alignof(uint64));
}

/* store_collect_keep is a copy's keep: whether a record's copy is live in the marks. */
static bool
store_collect_keep(void *captures, uint64 number, ChunksTrailerLocator locator)
{
	const StdVec *live = store_collect_live_get(captures, number);

	return live != NULL && store_collect_numbers_contains(live, chunks_trailer_Locator_offset(&locator));
}

/* store_collect_slice_contains reports whether a slice of numbers holds one. */
static bool
store_collect_slice_contains(const StdVec *numbers, uint64 number)
{
	for (size_t k = 0, k_len = numbers->len; k < k_len; k++)
		if (STD_VEC_AT(numbers, uint64, k) == number)
			return true;
	return false;
}

/* store_collect_offsets_t is partly_dead's verify's captures: a pack's live offsets and the dead ones found. */
typedef struct store_collect_offsets_t
{
	const StdVec *live;
	StdVec	   *offsets;
} store_collect_offsets_t;

/* store_collect_dead_offset is partly_dead's verify closure: a record's offset when its copy isn't live. */
static bool
store_collect_dead_offset(void *captures, ChunksFileInterruptible io, ChunksPackVerified *chunk,
						  ChunksErrorError *error)
{
	store_collect_offsets_t *offsets = captures;
	ChunksTrailerLocator locator = chunks_pack_Verified_locator(chunk);
	uint64		offset = chunks_trailer_Locator_offset(&locator);

	(void) io;
	(void) error;
	if (!store_collect_numbers_contains(offsets->live, offset))
		(void) store_collect_numbers_insert(offsets->offsets, offset);
	return true;
}

/*
 * store_collect_partly_dead returns the dead offsets of each pack below the
 * marking's bound that a collection keeps and that holds a copy its marking
 * didn't reach, a Vec<(u64, BTreeSet<u64>)>, which its deletion vector names.
 */
static bool
store_collect_partly_dead(HostIoIo *io, const ChunksPacksBackendPacks *packs, const HostSharedReader *reading,
						  const StoreCollectMarks *marks, const StdVec *chosen, uint64 bound, StdVec *result,
						  HostError *error)
{
	ChunksFileInterruptible interruptible = {&CHUNKS_FILE_IO_INTERRUPTIBLE_OPS, io};
	ChunksErrorError failed;

	std_vec_new(result, sizeof(store_collect_partly_t), _Alignof(store_collect_partly_t));
	for (size_t k = 0, k_len = marks->live.len; k < k_len; k++)
	{
		const store_collect_live_t *live = &STD_VEC_AT(&marks->live, store_collect_live_t, k);
		store_collect_partly_t entry = {.number = live->pack};
		store_collect_offsets_t captures;
		const ChunksPackPackReader *pack;
		StdArcInner *reader;
		ChunksPackLost lost;
		StdVec		offsets;
		bool		has;
		bool		ok;

		if (live->pack >= bound)
			break;
		if (store_collect_slice_contains(chosen, live->pack))
			continue;
		if (!chunks_packs_backend_Packs_pack_reader(packs, io, reading, live->pack, &has, &reader, &failed))
			goto raised;
		if (!has)
			continue;
		pack = std_arc_Deref_deref(reader, _Alignof(ChunksPackPackReader));
		if ((uint64) live->offsets.len == chunks_pack_PackReader_len(pack))
		{
			chunks_pack_ArcPackReader_drop(reader);
			continue;
		}
		std_vec_new(&offsets, sizeof(uint64), _Alignof(uint64));
		captures.live = &live->offsets;
		captures.offsets = &offsets;
		ok = chunks_pack_PackReader_verified(pack, interruptible, live->pack, store_collect_dead_offset, &captures, NULL,
											 &lost, &failed);
		if (ok)
			chunks_pack_Lost_drop(&lost);
		chunks_pack_ArcPackReader_drop(reader);
		if (!ok)
		{
			std_vec_free_Checked(&offsets, sizeof(uint64), _Alignof(uint64));
			goto raised;
		}
		entry.offsets = std_btree_BTreeSetU64_FromArray_from(offsets.ptr, offsets.len);
		std_vec_free_Checked(&offsets, sizeof(uint64), _Alignof(uint64));
		std_vec_push_Checked(result, &entry, sizeof(entry), _Alignof(store_collect_partly_t));
	}
	return true;
raised:
	store_collect_Partly_drop(result);
	*error = chunks_packs_shared_raised(&failed);
	return false;
}

/* store_collect_retiring_t is a retirement's keep's captures: the collection's io and packs, the copies the marking reached, the packs chosen, the packs kept with dead copies, and their vectors' generation. */
typedef struct store_collect_retiring_t
{
	HostIoIo   *io;
	const ChunksPacksBackendPacks *packs;
	const StdVec *seen;
	const StdVec *chosen;
	const StdVec *partly;
	uint64		generation;
} store_collect_retiring_t;

static bool store_collect_retiring(void *captures, ChunksPacksRetireMarking *marking, StdVec *entries,
								   StdVec *result, ChunksErrorError *error) STD_REQUIRES(CHUNKS_PACKS_BARRIER_HOLDING);

/*
 * store_collect_retiring is a retirement's keep: every pack chosen when a
 * writer was answered with a copy the marking didn't reach, and otherwise
 * none, each pack kept with dead copies given its deletion vector, with a
 * check for a cancel or a stop before each, since each is a durable change
 * of its own and a Postgres process acts on a signal only at such a check
 * (bugs row 499); Doltgrust checks its stop once before the retirement, as
 * its keep writes no vector. The vectors' generation passes every one on
 * disk, read here under the barrier, since a marking another collection took
 * over still retires (H14).
 */
static bool
store_collect_retiring(void *captures, ChunksPacksRetireMarking *marking, StdVec *entries, StdVec *result,
					   ChunksErrorError *error)
{
	const store_collect_retiring_t *retiring = captures;
	uint64		highest = 0;
	uint64		generation;
	bool		unseen = false;

	for (size_t k = 0, k_len = entries->len; k < k_len && !unseen; k++)
	{
		ChunksTrailerLocator locator = STD_VEC_AT(entries, chunks_TupleAddressLocator_t, k).f1;

		unseen = !store_collect_places_contains(retiring->seen, locator.pack, chunks_trailer_Locator_offset(&locator));
	}
	std_vec_free_Checked(entries, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
	if (unseen)
	{
		for (size_t k = 0, k_len = retiring->chosen->len; k < k_len; k++)
			(void) store_collect_numbers_insert(result, STD_VEC_AT(retiring->chosen, uint64, k));
		return true;
	}
	if (retiring->partly->len != 0 &&
		!chunks_packs_marking_Packs_highest_vector(retiring->packs, retiring->io, &highest, error))
		return false;
	generation = Max(retiring->generation, STD_ADD(u64, highest, 1, 546, 39));
	for (size_t k = 0, k_len = retiring->partly->len; k < k_len; k++)
	{
		const store_collect_partly_t *partly = &STD_VEC_AT(retiring->partly, store_collect_partly_t, k);
		HostError	stopped;
		bool		set;

		if (!host_io_Io_check(retiring->io, &stopped))
		{
			*error = chunks_error_Error_FromError_from(&stopped);
			return false;
		}
		if (!chunks_packs_retire_Marking_set_dead(marking, partly->number, generation, &partly->offsets, &set,
												  error))
			return false;
	}
	return true;
}

/* store_collect_marked_frame_0_t is marked's state across its pad: the capture, the marks, the packs chosen and copied, a copy's writer and sources, the retirement, and what it found. */
typedef struct store_collect_marked_frame_0_t
{
	HostIoIo   *io;
	const RepoRepo *repo;
	const HostSharedReader *reading;
	uint32		database;
	const StdVec *old;
	uint64		newest;
	uint64		limit;
	uint64		percent;
	uint32		emptied;

	/*
	 * ended is whether the retirement ended the marking, which a failure
	 * after it then doesn't end again.
	 */
	bool	   *ended;
	bool	   *has_result;
	StoreCollectCollected *result;
	HostError  *error;
	bool		ok;
	uint64		bound;
	TxnRootsCaptured captured;
	bool		captured_live;
	StoreCollectMarks marks;
	bool		marks_live;
	/* spaces is a BTreeMap<u64, u32> of std_TupleU64U32_t in number order. */
	StdVec		spaces;
	bool		spaces_live;
	/* dead is a Vec<u64> of the packs with nothing live. */
	StdVec		dead;
	bool		dead_live;

	/*
	 * copied is a BTreeMap<u32, Vec<u64>> of store_collect_space_t in
	 * tablespace order.
	 */
	StdVec		copied;
	bool		copied_live;
	ChunksPacksRetireForwarded forwarded;
	bool		forwarded_live;
	/* written is a Vec<u64> of the copies' packs. */
	StdVec		written;
	bool		written_live;
	uint64		moved;
	ChunksPackPackWriterInDir writer;
	bool		writer_live;
	/* sources is a Vec of store_collect_source_t. */
	StdVec		sources;
	bool		sources_live;
	/* moves is a Moves, a Vec of chunks_packs_backend_Packs_forward_moves_t. */
	StdVec		moves;
	bool		moves_live;
	ChunksPackPack pack;
	bool		pack_live;
	/* chosen is a Vec<u64> of the packs to retire. */
	StdVec		chosen;
	bool		chosen_live;
	/* partly is a Vec of store_collect_partly_t. */
	StdVec		partly;
	bool		partly_live;
	ChunksPacksRetireRetired retirement;
	bool		retirement_live;

	/*
	 * restored is a Vec<u64> of the numbers the verify listed packs again
	 * under.
	 */
	StdVec		restored;
	bool		restored_live;
} store_collect_marked_frame_0_t;

/* store_collect_marked_pad_0 is marked's pad: what it found, the retirement, the packs chosen, a copy's moves, pack, sources, and writer, the copies, then the marks and the capture. */
static void
store_collect_marked_pad_0(void *frame_)
{
	store_collect_marked_frame_0_t *frame = frame_;

	if (frame->restored_live)
	{
		frame->restored_live = false;
		std_vec_free_Checked(&frame->restored, sizeof(uint64), _Alignof(uint64));
	}
	if (frame->retirement_live)
	{
		frame->retirement_live = false;
		chunks_packs_retire_Retired_drop(&frame->retirement);
	}
	if (frame->partly_live)
	{
		frame->partly_live = false;
		store_collect_Partly_drop(&frame->partly);
	}
	if (frame->chosen_live)
	{
		frame->chosen_live = false;
		std_vec_free_Checked(&frame->chosen, sizeof(uint64), _Alignof(uint64));
	}
	if (frame->pack_live)
	{
		frame->pack_live = false;
		chunks_pack_Pack_drop(&frame->pack);
	}
	if (frame->moves_live)
	{
		frame->moves_live = false;
		std_vec_free_Checked(&frame->moves, sizeof(chunks_packs_backend_Packs_forward_moves_t),
							 _Alignof(chunks_packs_backend_Packs_forward_moves_t));
	}
	if (frame->sources_live)
	{
		frame->sources_live = false;
		store_collect_Sources_drop(&frame->sources);
	}
	if (frame->writer_live)
	{
		frame->writer_live = false;
		chunks_pack_PackWriterInDir_drop(&frame->writer);
	}
	if (frame->written_live)
	{
		frame->written_live = false;
		std_vec_free_Checked(&frame->written, sizeof(uint64), _Alignof(uint64));
	}
	if (frame->forwarded_live)
	{
		frame->forwarded_live = false;
		chunks_packs_retire_Forwarded_drop(&frame->forwarded);
	}
	if (frame->copied_live)
	{
		frame->copied_live = false;
		store_collect_Spaces_drop(&frame->copied);
	}
	if (frame->dead_live)
	{
		frame->dead_live = false;
		std_vec_free_Checked(&frame->dead, sizeof(uint64), _Alignof(uint64));
	}
	if (frame->spaces_live)
	{
		frame->spaces_live = false;
		std_vec_free_Checked(&frame->spaces, sizeof(std_TupleU64U32_t), _Alignof(std_TupleU64U32_t));
	}
	if (frame->marks_live)
	{
		frame->marks_live = false;
		store_collect_Marks_drop(&frame->marks);
	}
	if (frame->captured_live)
	{
		frame->captured_live = false;
		txn_roots_Captured_drop(&frame->captured);
	}
}

/* store_collect_raised returns chunks' error as a host error, for a call that fails with one. */
static bool
store_collect_raised(store_collect_marked_frame_0_t *frame, ChunksErrorError *failed)
{
	*frame->error = chunks_packs_shared_raised(failed);
	return false;
}

/* store_collect_marked_marks captures the roots and marks through every copy they reach and every chunk a writer was answered with meanwhile, reporting through stopped when it stopped. */
static bool
store_collect_marked_marks(store_collect_marked_frame_0_t *frame, bool *stopped)
{
	const ChunksPacksBackendPacks *packs = &frame->repo->packs;
	ChunksErrorError failed;
	StdVec		registered;
	StdVec		manifests;
	StdVec		touched;
	StdVec		hits;
	uint64		next = STD_ADD(u64, frame->newest, 1, 459, 18);
	uint64		kept_from;

	*stopped = false;
	if (!chunks_packs_backend_Packs_kept_from(packs, &kept_from, &failed))
		return store_collect_raised(frame, &failed);
	frame->bound = Min(next, kept_from);
	if (!chunks_packs_roots_Packs_registered(packs, frame->io, &registered, &failed))
		return store_collect_raised(frame, &failed);
	if (!host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-collector-registered", frame->error) ||
		!store_collect_captured(frame->io, frame->database, &frame->captured, frame->error))
	{
		std_vec_free_Checked(&registered, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
		return false;
	}
	frame->captured_live = true;
	if (!host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-collector-states", frame->error))
	{
		std_vec_free_Checked(&registered, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
		return false;
	}
	if (!repo_Repo_roots(frame->repo, frame->io, &manifests, &failed))
	{
		std_vec_free_Checked(&registered, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
		return store_collect_raised(frame, &failed);
	}
	std_vec_extend_from_slice_Checked(&registered, frame->captured.roots.ptr, frame->captured.roots.len,
									  sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
	store_collect_trees_extend(&registered, &manifests);
#ifdef DOLTGRES_TEST_STORE
	if (STORE_COLLECT_HIDDEN.has && STORE_COLLECT_HIDDEN.of == frame->database)
	{
		size_t		kept = 0;

		for (size_t k = 0, k_len = registered.len; k < k_len; k++)
			if (chunks_address_Address_Ord_cmp(&STD_VEC_AT(&registered, chunks_TupleAddressLocator_t, k).f0,
											   &STORE_COLLECT_HIDDEN.address) != STD_ORDERING_EQUAL)
				STD_VEC_AT(&registered, chunks_TupleAddressLocator_t, kept++) =
					STD_VEC_AT(&registered, chunks_TupleAddressLocator_t, k);
		registered.len = kept;
		STORE_COLLECT_HIDDEN.has = false;
	}
#endif
	frame->marks = store_collect_Marks_new(store_collect_numbers_clone(frame->old), frame->limit, true,
										   frame->database);
	frame->marks_live = true;
	if (!store_collect_Marks_mark(&frame->marks, frame->io, packs, frame->reading, frame->bound,
								  &frame->captured.outside, &registered, &touched, frame->error))
		return false;
	std_vec_free_Checked(&touched, sizeof(uint64), _Alignof(uint64));
	for (;;)
	{
		size_t		seen = frame->marks.seen.len;

		if (!chunks_packs_marking_Packs_hits(packs, frame->io, &hits, &failed))
			return store_collect_raised(frame, &failed);
		if (!store_collect_Marks_mark(&frame->marks, frame->io, packs, frame->reading, frame->bound,
									  &frame->captured.outside, &hits, &touched, frame->error))
			return false;
		std_vec_free_Checked(&touched, sizeof(uint64), _Alignof(uint64));
		if (frame->marks.stopped)
		{
			*stopped = true;
			return true;
		}
		if (frame->marks.seen.len == seen)
			break;
	}
	*stopped = frame->marks.stopped || host_backups_running();
	return true;
}

/* store_collect_marked_choose chooses the packs below the marking's bound with nothing live and those mostly dead, when it reached every chunk it was named. */
static bool
store_collect_marked_choose(store_collect_marked_frame_0_t *frame)
{
	const ChunksPacksBackendPacks *packs = &frame->repo->packs;
	bool		resolved = frame->marks.unresolved.len == 0 && !frame->marks.passed;
	ChunksErrorError failed;

	if (!chunks_packs_backend_Packs_spaces(packs, frame->io, frame->reading, &frame->spaces, &failed))
		return store_collect_raised(frame, &failed);
	frame->spaces_live = true;
	std_vec_new(&frame->dead, sizeof(uint64), _Alignof(uint64));
	frame->dead_live = true;
	std_vec_new(&frame->copied, sizeof(store_collect_space_t), _Alignof(store_collect_space_t));
	frame->copied_live = true;
	for (size_t k = 0, k_len = frame->spaces.len; k < k_len; k++)
	{
		std_TupleU64U32_t entry = STD_VEC_AT(&frame->spaces, std_TupleU64U32_t, k);
		StdArcInner *reader;
		uint64		live;
		bool		has;

		if (entry.f0 >= frame->bound)
			break;
		if (!resolved || store_collect_numbers_contains(frame->old, entry.f0))
			continue;
		if (!chunks_packs_backend_Packs_pack_reader(packs, frame->io, frame->reading, entry.f0, &has, &reader, &failed))
			return store_collect_raised(frame, &failed);
		if (!has)
			continue;
		live = store_collect_bytes_get(&frame->marks.bytes, entry.f0);
		if (live == 0)
			std_vec_push_Checked(&frame->dead, &entry.f0, sizeof(uint64), _Alignof(uint64));
		else if (entry.f1 == frame->emptied && frame->emptied != CHUNKS_PACKS_DEFAULT_SPACE)
			store_collect_copied_push(&frame->copied, CHUNKS_PACKS_DEFAULT_SPACE, entry.f0);
		else if (store_collect_over(chunks_pack_PackReader_records_len(std_arc_Deref_deref(reader,
																						   _Alignof(ChunksPackPackReader))),
									live, frame->percent))
			store_collect_copied_push(&frame->copied, entry.f1, entry.f0);
		chunks_pack_ArcPackReader_drop(reader);
	}
	frame->spaces_live = false;
	std_vec_free_Checked(&frame->spaces, sizeof(std_TupleU64U32_t), _Alignof(std_TupleU64U32_t));
	return true;
}

/* store_collect_marked_copy copies the live chunks of a tablespace's mostly dead packs into a new pack there with their forwards, reporting through skipped when it wrote nothing. */
static bool
store_collect_marked_copy(store_collect_marked_frame_0_t *frame, const store_collect_space_t *space)
{
	const ChunksPacksBackendPacks *packs = &frame->repo->packs;
	store_collect_keep_t keep = {store_collect_keep, &frame->marks.live};
	ChunksPacksRetireForwarded next;
	ChunksPacksRetireForwarded joined;
	ChunksErrorError failed;
	HostError	copy_error;
	uint64		moved = 0;

	if (!chunks_packs_backend_Packs_writer(packs, frame->io, frame->reading, space->space, &frame->writer, &failed))
		return store_collect_raised(frame, &failed);
	frame->writer_live = true;
	chunks_pack_PackWriterInDir_train(&frame->writer);
	std_vec_with_capacity_Checked(&frame->sources, space->numbers.len, sizeof(store_collect_source_t),
								  _Alignof(store_collect_source_t));
	frame->sources_live = true;
	for (size_t k = 0, k_len = space->numbers.len; k < k_len; k++)
	{
		store_collect_source_t source = {.number = STD_VEC_AT(&space->numbers, uint64, k)};
		bool		has;

		if (!chunks_packs_backend_Packs_pack_reader(packs, frame->io, frame->reading, source.number, &has,
													&source.pack, &failed))
			return store_collect_raised(frame, &failed);
		if (has)
			std_vec_push_Checked(&frame->sources, &source, sizeof(source), _Alignof(store_collect_source_t));
	}
	if (!store_collect_copy_packs(frame->io, &frame->writer, packs, frame->reading, frame->sources.ptr,
								  frame->sources.len, &keep, &frame->marks.pacer, &frame->moves, &copy_error))
	{
		uint8		code[5];
		StdString	message;
		StdFmtPiece pieces[] = {STD_FMT_LIT("could not copy the live chunks of database "), STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&frame->database, std_u32_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		memcpy(code, host_Error_sqlstate(&copy_error), 5);
		if (!host_warn(HOST_IO_IO_IO_DOORS(frame->io), &copy_error, "versioned collection's copy", frame->error))
			return false;
		frame->writer_live = false;
		if (!chunks_pack_PackWriterInDir_abandon(&frame->writer, HOST_IO_IO_TAKES_SHORT(frame->io), &failed))
			return store_collect_raised(frame, &failed);
		std_fmt_format_Checked(&message, &arguments);
		*frame->error = host_Error_fork(code, &message);
		return false;
	}
	frame->moves_live = true;
	if (chunks_pack_PackWriterInDir_is_empty(&frame->writer))
	{
		frame->writer_live = false;
		if (!chunks_pack_PackWriterInDir_abandon(&frame->writer, HOST_IO_IO_TAKES_SHORT(frame->io), &failed))
			return store_collect_raised(frame, &failed);
		goto copied;
	}
	frame->writer_live = false;
	if (!chunks_pack_PackWriterInDir_finish(&frame->writer, HOST_IO_IO_TAKES_SHORT(frame->io), &frame->pack, &failed))
		return store_collect_raised(frame, &failed);
	frame->pack_live = true;
	if (!std_vec_try_reserve(&frame->written, 1, sizeof(uint64), _Alignof(uint64)))
	{
		*frame->error = host_Error_out_of_memory();
		return false;
	}
	std_vec_push_Checked(&frame->written, &frame->pack.number, sizeof(uint64), _Alignof(uint64));
	for (size_t k = 0, k_len = frame->moves.len; k < k_len; k++)
		moved = std_add_u64(moved, chunks_trailer_Locator_length(&STD_VEC_AT(&frame->moves,
																			 chunks_packs_backend_Packs_forward_moves_t,
																			 k).f0),
							STD_ACCUM_FILE, 206, 1);
	frame->moved = STD_ADD(u64, frame->moved, moved, 537, 9);
	frame->pack_live = false;
	if (!chunks_packs_backend_Packs_add(packs, frame->io, frame->reading, &frame->pack, &failed) ||
		!chunks_packs_backend_Packs_forward(packs, frame->io, frame->reading, frame->moves.ptr, frame->moves.len, &next,
											&failed))
		return store_collect_raised(frame, &failed);
	frame->forwarded_live = false;
	if (!chunks_packs_retire_Forwarded_join(&frame->forwarded, &next, &joined, &failed))
		return store_collect_raised(frame, &failed);
	frame->forwarded = joined;
	frame->forwarded_live = true;
copied:
	frame->moves_live = false;
	std_vec_free_Checked(&frame->moves, sizeof(chunks_packs_backend_Packs_forward_moves_t),
						 _Alignof(chunks_packs_backend_Packs_forward_moves_t));
	frame->sources_live = false;
	store_collect_Sources_drop(&frame->sources);
	return true;
}

/* store_collect_marked_retire retires the packs chosen, keeping every one when a writer was answered with a copy the marking didn't reach, and verifies what it retired. */
static bool
store_collect_marked_retire(store_collect_marked_frame_0_t *frame)
{
	const ChunksPacksBackendPacks *packs = &frame->repo->packs;
	store_collect_retiring_t retiring;
	ChunksPacksRemovalReservation reservation;
	ChunksErrorError failed;
	uint64		vector;
	uint64		generation;
	size_t		wanted;

	std_vec_new(&frame->chosen, sizeof(uint64), _Alignof(uint64));
	frame->chosen_live = true;
	std_vec_extend_from_slice_Checked(&frame->chosen, frame->dead.ptr, frame->dead.len, sizeof(uint64),
									  _Alignof(uint64));
	for (size_t k = 0, k_len = frame->copied.len; k < k_len; k++)
	{
		const StdVec *numbers = &STD_VEC_AT(&frame->copied, store_collect_space_t, k).numbers;

		std_vec_extend_from_slice_Checked(&frame->chosen, numbers->ptr, numbers->len, sizeof(uint64), _Alignof(uint64));
	}
	generation = STD_ADD(u64, frame->newest, 1, 546, 23);
	if (!chunks_packs_backend_Packs_newest_vector(packs, frame->io, frame->reading, &vector, &failed))
		return store_collect_raised(frame, &failed);
	generation = Max(generation, STD_ADD(u64, vector, 1, 546, 39));
	if (generation == 0)
	{
		StdString	message;

		std_string_from_Checked(&message, "a marking's bound is 0", strlen("a marking's bound is 0"));
		*frame->error = host_Error_fork((const uint8 *) "XX000", &message);
		return false;
	}
	if (frame->old->len == 0)
	{
		if (!store_collect_partly_dead(frame->io, packs, frame->reading, &frame->marks, &frame->chosen, frame->bound,
									   &frame->partly, frame->error))
			return false;
	}
	else
		std_vec_new(&frame->partly, sizeof(store_collect_partly_t), _Alignof(store_collect_partly_t));
	frame->partly_live = true;
	wanted = STD_ADD(usize, STD_MUL(usize, frame->chosen.len, 3, 553, 43), frame->partly.len, 553, 43);
	if (!chunks_packs_removal_reserve(frame->io, wanted, &reservation, &failed))
		return store_collect_raised(frame, &failed);
	retiring.io = frame->io;
	retiring.packs = packs;
	retiring.seen = &frame->marks.seen;
	retiring.chosen = &frame->chosen;
	retiring.partly = &frame->partly;
	retiring.generation = generation;
	frame->forwarded_live = false;
	if (!chunks_packs_retire_Packs_retire(packs, frame->io, frame->chosen.ptr, frame->chosen.len, &frame->forwarded,
										  &reservation, store_collect_retiring, &retiring, NULL, &frame->retirement,
										  &failed))
		return store_collect_raised(frame, &failed);
	frame->retirement_live = true;
	*frame->ended = true;
	return true;
}

/* store_collect_listed_kept reports whether a listed pack is among a collection's old packs after it: below its marking's bound, a copy's pack, or one its verify listed again. */
static bool
store_collect_listed_kept(const store_collect_marked_frame_0_t *frame, uint64 number)
{
	return number < frame->bound || store_collect_slice_contains(&frame->written, number) ||
		store_collect_slice_contains(&frame->restored, number);
}

/* store_collect_marked_body_0 is marked's body: the marking, the packs chosen, the copies, the retirement and its verify, and what it did. */
static void
store_collect_marked_body_0(void *frame_)
{
	store_collect_marked_frame_0_t *frame = frame_;
	const ChunksPacksBackendPacks *packs = &frame->repo->packs;
	ChunksErrorError failed;
	StdVec		listed;
	size_t		taken;
	size_t		copied = 0;
	uint64		live = 0;
	bool		stopped;

	frame->ok = false;
	*frame->has_result = false;
	if (!store_collect_marked_marks(frame, &stopped))
		goto done;
	if (stopped)
	{
		frame->ok = true;
		goto done;
	}
	if (!store_collect_marked_choose(frame))
		goto done;
	if (!chunks_packs_backend_Packs_forward(packs, frame->io, frame->reading, NULL, 0, &frame->forwarded, &failed))
	{
		(void) store_collect_raised(frame, &failed);
		goto done;
	}
	frame->forwarded_live = true;
	std_vec_new(&frame->written, sizeof(uint64), _Alignof(uint64));
	frame->written_live = true;
	for (size_t k = 0, k_len = frame->copied.len; k < k_len; k++)
		if (!store_collect_marked_copy(frame, &STD_VEC_AT(&frame->copied, store_collect_space_t, k)))
			goto done;
	if (host_backups_running())
	{
		frame->ok = true;
		goto done;
	}
	if (!store_collect_marked_retire(frame))
		goto done;
	taken = 0;
	(void) chunks_packs_retire_Retired_numbers(&frame->retirement, &taken);
	frame->retirement_live = false;
	if (!store_collect_verified(frame->io, frame->repo, frame->reading, frame->database, frame->old, &frame->retirement,
								&frame->restored, frame->error))
		goto done;
	frame->restored_live = true;
	if (!chunks_packs_backend_Packs_spaces(packs, frame->io, frame->reading, &listed, &failed))
	{
		(void) store_collect_raised(frame, &failed);
		goto done;
	}
	for (size_t k = 0, k_len = frame->copied.len; k < k_len; k++)
		copied = std_add_usize(copied, STD_VEC_AT(&frame->copied, store_collect_space_t, k).numbers.len, STD_ACCUM_FILE,
							   206, 1);
	for (size_t k = 0, k_len = frame->marks.bytes.len; k < k_len; k++)
		live = std_add_u64(live, STD_VEC_AT(&frame->marks.bytes, std_TupleU64U64_t, k).f1, STD_ACCUM_FILE, 206, 1);
	frame->result->retired = STD_SUB(usize, taken, frame->restored.len, 572, 18);
	frame->result->copied = copied;
	frame->result->live = live;
	frame->result->moved = frame->moved;
	std_vec_new(&frame->result->old, sizeof(uint64), _Alignof(uint64));
	for (size_t k = 0, k_len = listed.len; k < k_len; k++)
	{
		uint64		number = STD_VEC_AT(&listed, std_TupleU64U32_t, k).f0;

		if (store_collect_listed_kept(frame, number))
			std_vec_push_Checked(&frame->result->old, &number, sizeof(number), _Alignof(uint64));
	}
	std_vec_free_Checked(&listed, sizeof(std_TupleU64U32_t), _Alignof(std_TupleU64U32_t));
	*frame->has_result = true;
	frame->ok = true;
done:
	store_collect_marked_pad_0(frame);
}

/*
 * store_collect_marked is a collection's work once its marking has begun,
 * with its old packs, the newest pack number the marking covers, and the
 * collection's pace and its pack's target percent. It answers nothing when it
 * stopped for a base backup before its retirement, its marking left to end.
 */
static bool
store_collect_marked(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading, uint32 database,
					 const StdVec *old, uint64 newest, uint64 limit, uint64 percent, uint32 emptied, bool *ended,
					 bool *has_result, StoreCollectCollected *result, HostError *error)
{
	store_collect_marked_frame_0_t frame = {.io = io, .repo = repo, .reading = reading, .database = database,
		.old = old, .newest = newest, .limit = limit, .percent = percent, .emptied = emptied, .ended = ended,
	.has_result = has_result, .result = result, .error = error};

	std_landing(&frame, store_collect_marked_body_0, store_collect_marked_pad_0);
	return frame.ok;
}

/*
 * store_collect_marking begins a collection's marking and runs the
 * collection, ending the marking when the collection stops for a base backup
 * or fails; a failure is reported at WARNING before the end and raised after
 * it with its SQLSTATE, as the end may fail too.
 */
static bool
store_collect_marking(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading, uint32 database,
					  const StdVec *old, uint64 limit, uint64 percent, uint32 emptied, bool *has_result,
					  StoreCollectCollected *result, HostError *error)
{
	const ChunksPacksBackendPacks *packs = &repo->packs;
	ChunksErrorError failed;
	HostError	collect_error;
	uint64		newest;
	bool		ended = false;
	uint8		code[5];
	StdString	message;
	StdFmtPiece pieces[] = {STD_FMT_LIT("could not collect the repository of database "), STD_FMT_ARG(0)};
	StdFmtArg	args[] = {{&database, std_u32_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	if (!chunks_packs_marking_Packs_begin_marking(packs, io, reading, &newest, &failed))
		goto raised;
	if (store_collect_marked(io, repo, reading, database, old, newest, limit, percent, emptied, &ended, has_result,
							 result, &collect_error))
	{
		if (*has_result)
			return true;
		if (!chunks_packs_marking_Packs_end_marking(packs, io, &failed))
			goto raised;
		return true;
	}
	memcpy(code, host_Error_sqlstate(&collect_error), 5);
	if (!host_warn(HOST_IO_IO_IO_DOORS(io), &collect_error, "versioned collection", error))
		return false;
	if (!ended && !chunks_packs_marking_Packs_end_marking(packs, io, &failed))
		goto raised;
	std_fmt_format_Checked(&message, &arguments);
	*error = host_Error_fork(code, &message);
	return false;
raised:
	*error = chunks_packs_shared_raised(&failed);
	return false;
}

/* store_collect_collected_frame_0_t is collected's state across its pad: its arguments, and whether its collection began. */
typedef struct store_collect_collected_frame_0_t
{
	HostIoIo   *io;
	const RepoRepo *repo;
	const HostSharedReader *reading;
	uint32		database;
	uint64		limit;
	uint64		percent;
	uint32		emptied;
	const StdVec *old;
	bool	   *has_result;
	StoreCollectCollected *result;
	HostError  *error;
	bool		ok;
	bool		began;
} store_collect_collected_frame_0_t;

/* store_collect_collected_body_0 is collected's body: the collection, once a base backup counted in doesn't keep it from beginning. */
static void
store_collect_collected_body_0(void *frame_)
{
	store_collect_collected_frame_0_t *frame = frame_;

	if (!txn_state_usable(frame->io, frame->database, frame->error))
		return;
	if (!host_collection_begin())
	{
		frame->ok = true;
		return;
	}
	frame->began = true;
	frame->ok = store_collect_marking(frame->io, frame->repo, frame->reading, frame->database, frame->old,
									  frame->limit, frame->percent, frame->emptied, frame->has_result, frame->result,
									  frame->error);
}

/*
 * store_collect_collected_pad_0 is collected's pad: it ends the collection
 * once begun, an unwind's too, so that no base backup waits on it forever
 * (bugs row 499).
 */
static void
store_collect_collected_pad_0(void *frame_)
{
	store_collect_collected_frame_0_t *frame = frame_;

	if (frame->began)
	{
		frame->began = false;
		host_collection_end();
	}
}

/*
 * store_collect_collected runs a collection of a database's repository, full
 * when it is given no old packs and otherwise young, taking the copies in the
 * old packs as leaves and choosing only among the others. A repository the
 * start's loader left unusable isn't collected, and it runs nothing while a
 * base backup is counted in. Its marking's reads and its copies run at limit
 * bytes a second, or unpaced at 0, and it copies out the live chunks of a
 * pack that passes them by percent. It answers nothing when a base backup
 * kept it from finishing. A tablespace it empties, unless that is
 * CHUNKS_PACKS_DEFAULT_SPACE, has the live chunks of each of its packs copied
 * into the repository's own directory whatever their share, so that the
 * packs retire, as DROP TABLESPACE needs (bugs row 99).
 */
bool
store_collect_collected(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading, uint32 database,
						uint64 limit, uint64 percent, uint32 emptied, const StdVec *old, bool *has_result,
						StoreCollectCollected *result, HostError *error)
{
	store_collect_collected_frame_0_t frame = {.io = io, .repo = repo, .reading = reading, .database = database,
		.limit = limit, .percent = percent, .emptied = emptied, .old = old, .has_result = has_result,
	.result = result, .error = error};

	*has_result = false;
	std_landing(&frame, store_collect_collected_body_0, store_collect_collected_pad_0);
	store_collect_collected_pad_0(&frame);
	return frame.ok;
}

/* store_collect_Collected_drop drops what a collection did, its old packs. */
void
store_collect_Collected_drop(StoreCollectCollected *self)
{
	std_vec_free_Checked(&self->old, sizeof(uint64), _Alignof(uint64));
}

/* store_collect_Held_drop drops what the worker keeps of a repository, its old packs. */
static void
store_collect_Held_drop(StoreCollectHeld *self)
{
	std_vec_free_Checked(&self->old, sizeof(uint64), _Alignof(uint64));
}

/* store_collect_Schedule_Default_default makes an empty schedule. */
void
store_collect_Schedule_Default_default(StoreCollectSchedule *result)
{
	std_vec_new(&result->held, sizeof(store_collect_held_t), _Alignof(store_collect_held_t));
	std_vec_new(&result->reported, sizeof(uint32), _Alignof(uint32));
}

/* store_collect_Schedule_drop drops a schedule. */
void
store_collect_Schedule_drop(StoreCollectSchedule *self)
{
	for (size_t k = 0, k_len = self->held.len; k < k_len; k++)
		store_collect_Held_drop(&STD_VEC_AT(&self->held, store_collect_held_t, k).held);
	std_vec_free_Checked(&self->held, sizeof(store_collect_held_t), _Alignof(store_collect_held_t));
	std_vec_free_Checked(&self->reported, sizeof(uint32), _Alignof(uint32));
}

/* store_collect_held_entry returns what the schedule keeps of a repository, a new empty one if none. */
static StoreCollectHeld *
store_collect_held_entry(StoreCollectSchedule *schedule, uint32 id)
{
	size_t		at = 0;
	store_collect_held_t entry = {.id = id};

	while (at < schedule->held.len && STD_VEC_AT(&schedule->held, store_collect_held_t, at).id < id)
		at++;
	if (at == schedule->held.len || STD_VEC_AT(&schedule->held, store_collect_held_t, at).id != id)
	{
		std_vec_new(&entry.held.old, sizeof(uint64), _Alignof(uint64));
		std_vec_insert(&schedule->held, at, &entry, sizeof(entry), _Alignof(store_collect_held_t), RUST_FILE, 0, 0);
	}
	return &STD_VEC_AT(&schedule->held, store_collect_held_t, at).held;
}

/* store_collect_u32_cmp is u32's Ord, for a search of a BTreeSet<u32>. */
static StdOrdering
store_collect_u32_cmp(const void *a, const void *b)
{
	uint32		x = *(const uint32 *) a;
	uint32		y = *(const uint32 *) b;

	return x < y ? STD_ORDERING_LESS : x > y ? STD_ORDERING_GREATER : STD_ORDERING_EQUAL;
}

/* store_collect_reported_insert records a directory reported, by its id, and reports whether it was new. */
static bool
store_collect_reported_insert(StdVec *reported, uint32 id)
{
	size_t		at;

	if (std_slice_binary_search(reported->ptr, reported->len, sizeof(id), &id, store_collect_u32_cmp, &at))
		return false;
	std_vec_insert(reported, at, &id, sizeof(id), _Alignof(uint32), RUST_FILE, 0, 0);
	return true;
}

/* store_collect_find returns the row of pg_database with an id, the collectable rows' alone when asked, or NULL for none. */
static const StoreCollectDatabase *
store_collect_find(const StoreCollectDatabase *databases, size_t databases_len, uint32 id, bool collectable)
{
	for (size_t k = 0; k < databases_len; k++)
		if (databases[k].id == id && (!collectable || databases[k].collectable))
			return &databases[k];
	return NULL;
}

/* store_collect_Held_found records what a full or young collection of the repository found, taking its old packs. */
static void
store_collect_Held_found(StoreCollectHeld *self, StoreCollectCollected *done, bool full)
{
	if (full)
	{
		self->has_live = true;
		self->live = done->live;
		self->has_last = true;
		self->last = host_instant();
		self->young_live = 0;
		self->moved = 0;
	}
	else
		self->young_live = STD_ADD(u64, self->young_live, done->live, 730, 22);
	self->moved = STD_ADD(u64, self->moved, done->moved, 732, 9);
	std_vec_free_Checked(&self->old, sizeof(uint64), _Alignof(uint64));
	self->old = done->old;
}

/* store_collect_opened opens a database's repository for a check, finishing a committed move whose finish never ran. */
static bool
store_collect_opened(HostIoIo *io, uint32 repository, const StoreCollectDatabase *database, RepoRepo *result,
					 HostError *error)
{
	ChunksErrorError failed;
	uint32		own;

	if (!store_databases_committed(io, repository, database->tablespace, &own, error))
		return false;
	if (!repo_Repo_open(io, repository, own, result, &failed))
	{
		*error = chunks_packs_shared_raised(&failed);
		return false;
	}
	return true;
}

/*
 * store_collect_due returns whether a held repository's collection is due,
 * full or young, with its pace, by the bytes of its listed packs' records and
 * whether the disk is low. A young one with no old packs is full.
 */
static bool
store_collect_due(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading, const StoreCollectHeld *held,
				  bool low, const StoreCollectSettings *settings, bool *has_result, bool *full, uint64 *limit,
				  HostError *error)
{
	const ChunksPacksBackendPacks *packs = &repo->packs;
	ChunksErrorError failed;
	StdVec		numbers;
	uint64		total = 0;
	uint64		young = 0;
	size_t		young_packs = 0;
	uint64		percent = settings->target_percent;
	StdU128		moved;
	StdU128		share;
	bool		past;
	bool		aged = false;

	*has_result = false;
	if (!chunks_packs_backend_Packs_spaces(packs, io, reading, &numbers, &failed))
		goto raised;
	if (!held->has_live)
	{
		if (low || numbers.len >= settings->first_packs)
		{
			*has_result = true;
			*full = true;
			*limit = settings->io_limit;
		}
		std_vec_free_Checked(&numbers, sizeof(std_TupleU64U32_t), _Alignof(std_TupleU64U32_t));
		return true;
	}
	for (size_t k = 0, k_len = numbers.len; k < k_len; k++)
	{
		uint64		number = STD_VEC_AT(&numbers, std_TupleU64U32_t, k).f0;
		StdArcInner *reader;
		uint64		records;
		bool		has;

		if (!chunks_packs_backend_Packs_pack_reader(packs, io, reading, number, &has, &reader, &failed))
		{
			std_vec_free_Checked(&numbers, sizeof(std_TupleU64U32_t), _Alignof(std_TupleU64U32_t));
			goto raised;
		}
		if (!has)
			continue;
		records = chunks_pack_PackReader_records_len(std_arc_Deref_deref(reader, _Alignof(ChunksPackPackReader)));
		total = STD_ADD(u64, total, records, 799, 13);
		if (!store_collect_numbers_contains(&held->old, number))
		{
			young = STD_ADD(u64, young, records, 801, 41);
			young_packs = STD_ADD(usize, young_packs, 1, 801, 69);
		}
		chunks_pack_ArcPackReader_drop(reader);
	}
	std_vec_free_Checked(&numbers, sizeof(std_TupleU64U32_t), _Alignof(std_TupleU64U32_t));
	past = store_collect_over(total, held->live, percent);
	*limit = past ? 0 : settings->io_limit;
	moved = std_mul_u64_wide(held->moved, 100);
	share = std_mul_u64_wide(Max(held->live, 1), percent);
	if (held->has_last)
	{
		StdInstant	now = host_instant();
		StdDuration since = std_Instant_saturating_duration_since(&now, held->last);

		aged = std_Duration_cmp(&since, &settings->full_interval) >= 0;
	}
	if (past || !std_u128_lt(moved, share) || aged ||
		(low && total > STD_ADD(u64, held->live, held->young_live, 810, 50)))
	{
		*has_result = true;
		*full = true;
		return true;
	}
	if (young >= settings->young_size || young_packs >= settings->young_packs)
	{
		*has_result = true;
		*full = held->old.len == 0;
	}
	return true;
raised:
	*error = chunks_packs_shared_raised(&failed);
	return false;
}

/*
 * store_collect_checked is a repository's check under its database's lock: it
 * opens the repository, runs its collection when one is due, recording what
 * it found, and closes it. Its caller gives the lock back however it ends.
 */
static bool
store_collect_checked(HostIoIo *io, uint32 repository, const StoreCollectDatabase *database,
					  const HostSharedReader *reading, StoreCollectHeld *held, bool low,
					  const StoreCollectSettings *settings, HostError *error)
{
	ChunksErrorError failed;
	RepoRepo	repo;
	StoreCollectCollected done;
	StdVec		old;
	uint64		limit;
	bool		has;
	bool		full;
	bool		has_done;

	if (!store_collect_opened(io, repository, database, &repo, error))
		return false;
	if (!store_collect_due(io, &repo, reading, held, low, settings, &has, &full, &limit, error))
		goto dropped;
	if (has)
	{
		if (full)
			std_vec_new(&old, sizeof(uint64), _Alignof(uint64));
		else
		{
			old = held->old;
			std_vec_new(&held->old, sizeof(uint64), _Alignof(uint64));
		}
		if (!store_collect_collected(io, &repo, reading, database->id, limit, settings->target_percent,
									 CHUNKS_PACKS_DEFAULT_SPACE, &old, &has_done, &done, error))
		{
			std_vec_free_Checked(&old, sizeof(uint64), _Alignof(uint64));
			goto dropped;
		}
		if (has_done)
		{
			store_collect_Held_found(held, &done, full);
			std_vec_free_Checked(&old, sizeof(uint64), _Alignof(uint64));
		}
		else
		{
			std_vec_free_Checked(&held->old, sizeof(uint64), _Alignof(uint64));
			held->old = old;
		}
	}
	if (!repo_Repo_close(&repo, io, reading, &failed))
	{
		*error = chunks_packs_shared_raised(&failed);
		return false;
	}
	return true;
dropped:
	repo_Repo_drop(&repo);
	return false;
}

/* store_collect_collect_due_frame_0_t is collect_due's state across its pad: the reader and the repositories. */
typedef struct store_collect_collect_due_frame_0_t
{
	HostIoIo   *io;
	StoreCollectSchedule *schedule;
	const StoreCollectDatabase *databases;
	size_t		databases_len;
	const StoreCollectSettings *settings;
	HostError  *error;
	bool		ok;
	HostSharedReader reading;
	bool		reading_live;
	/* repositories is a Vec<(NonZeroU32, u32)> of std_TupleU32U32_t. */
	StdVec		repositories;
	bool		repositories_live;
	StdString	message;
	bool		message_live;
} store_collect_collect_due_frame_0_t;

/* store_collect_collect_due_pad_0 is collect_due's pad: a message, the repositories, then the reader. */
static void
store_collect_collect_due_pad_0(void *frame_)
{
	store_collect_collect_due_frame_0_t *frame = frame_;

	if (frame->message_live)
	{
		frame->message_live = false;
		std_String_drop(&frame->message);
	}
	if (frame->repositories_live)
	{
		frame->repositories_live = false;
		std_vec_free_Checked(&frame->repositories, sizeof(std_TupleU32U32_t), _Alignof(std_TupleU32U32_t));
	}
	if (frame->reading_live)
	{
		frame->reading_live = false;
		host_shared_Reader_drop(&frame->reading);
	}
}

/* store_collect_unnamed reports a repository directory no row of pg_database names at LOG, once, unless a drop marked it. */
static bool
store_collect_unnamed(store_collect_collect_due_frame_0_t *frame, uint32 id, uint32 space)
{
	ChunksErrorError failed;
	StdPathBuf	dir;
	StdStr		shown;
	bool		dropping;
	bool		logged;

	if (!chunks_packs_open_dropping(HOST_IO_IO_READ_DOORS(frame->io), id, space, &dropping, &failed))
	{
		*frame->error = chunks_packs_shared_raised(&failed);
		return false;
	}
	if (dropping || !store_collect_reported_insert(&frame->schedule->reported, id))
		return true;
	dir = host_tablespace_dir(id, space);
	shown.ptr = dir.bytes.ptr;
	shown.len = dir.bytes.len;
	{
		StdFmtPiece pieces[] = {STD_FMT_LIT("repository directory \""), STD_FMT_ARG(0),
		STD_FMT_LIT("\" belongs to no database")};
		StdFmtArg	args[] = {{&shown, std_path_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_fmt_format_Checked(&frame->message, &arguments);
	}
	frame->message_live = true;
	std_vec_free_Checked(&dir.bytes, 1, 1);
	std_string_push_str_Checked(&frame->message, "", 1);
	logged = host_log(HOST_IO_IO_IO_DOORS(frame->io), frame->message.ptr, frame->error);
	frame->message_live = false;
	std_String_drop(&frame->message);
	return logged;
}

/* store_collect_collect_due_body_0 is collect_due's body: each repository's check, the schedule's forgotten repositories, then the files due removed. */
static void
store_collect_collect_due_body_0(void *frame_)
{
	store_collect_collect_due_frame_0_t *frame = frame_;
	HostIoIo   *io = frame->io;
	StdPathBuf	versioned;
	ChunksErrorError failed;
	uint64		free_bytes;
	uint64		total;
	bool		low;
	size_t		kept = 0;
	bool		measured;

	frame->ok = false;
	if (!store_collect_capture_redo(0, frame->error))
		goto done;
	if (!host_shared_read(io, &frame->reading, frame->error))
		goto done;
	frame->reading_live = true;
	versioned = host_versioned_dir(HOST_PG_DEFAULT);
	measured = host_disk_space(HOST_IO_IO_IO_DOORS(io), versioned.bytes.ptr, versioned.bytes.len, &free_bytes, &total,
							   frame->error);
	std_vec_free_Checked(&versioned.bytes, 1, 1);
	if (!measured)
		goto done;
	low = free_bytes < (frame->settings->has_min_free_disk ? frame->settings->min_free_disk :
						Min(UINT64CONST(10) << 30, total / 10));
	if (!chunks_packs_open_repositories(HOST_IO_IO_READ_DOORS(io), &frame->repositories, &failed))
	{
		*frame->error = chunks_packs_shared_raised(&failed);
		goto done;
	}
	frame->repositories_live = true;
	for (size_t k = 0, k_len = frame->repositories.len; k < k_len; k++)
	{
		std_TupleU32U32_t entry = STD_VEC_AT(&frame->repositories, std_TupleU32U32_t, k);
		const StoreCollectDatabase *database = store_collect_find(frame->databases, frame->databases_len, entry.f0,
																  false);
		bool		passed;
		bool		checked;

		if (database == NULL)
		{
			if (!store_collect_unnamed(frame, entry.f0, entry.f1))
				goto done;
			continue;
		}
		if (!database->collectable)
			continue;
		if (!txn_state_marked(io, entry.f0, &passed, frame->error))
			goto done;
		if (passed)
			continue;
		if (!store_databases_unfinished(HOST_IO_IO_READ_DOORS(io), entry.f0, database->tablespace, &passed,
										frame->error))
			goto done;
		if (passed)
			continue;
		if (!host_database_locked(io, entry.f0, &passed, frame->error))
			goto done;
		if (!passed)
			continue;
		checked = store_collect_checked(io, entry.f0, database, &frame->reading,
										store_collect_held_entry(frame->schedule, entry.f0), low, frame->settings,
										frame->error);
		host_database_unlocked(entry.f0);
		if (!checked)
			goto done;
	}
	for (size_t k = 0, k_len = frame->schedule->held.len; k < k_len; k++)
	{
		store_collect_held_t *entry = &STD_VEC_AT(&frame->schedule->held, store_collect_held_t, k);

		if (store_collect_find(frame->databases, frame->databases_len, entry->id, false) != NULL)
			STD_VEC_AT(&frame->schedule->held, store_collect_held_t, kept++) = *entry;
		else
			store_collect_Held_drop(&entry->held);
	}
	frame->schedule->held.len = kept;
	if (!host_pause(HOST_SHARED_IO_PAUSES(io), "doltgres-collector-removing", frame->error))
		goto done;
	if (!chunks_packs_removal_remove_due(io, &failed))
	{
		*frame->error = chunks_packs_shared_raised(&failed);
		goto done;
	}
	frame->ok = true;
done:
	store_collect_collect_due_pad_0(frame);
}

/*
 * store_collect_collect_due runs the collection of each repository whose
 * collection is due, the collector worker's check, and then removes the
 * files whose readers are gone. A repository the start's loader left
 * unusable is passed over, a directory no row of pg_database names is
 * reported once at LOG and left, and what it kept of a repository whose
 * database is gone is forgotten. Each repository is checked under its
 * database's lock, taken without waiting and passed over when a session
 * holds it, and opened only for its check; one that stands only as a
 * committed move's copy is passed over. It first drops the redo point of a
 * capture a terminated collector left, so its WAL is kept no longer (bugs
 * row 406).
 */
bool
store_collect_collect_due(HostIoIo *io, StoreCollectSchedule *schedule, const StoreCollectDatabase *databases,
						  size_t databases_len, const StoreCollectSettings *settings, HostError *error)
{
	store_collect_collect_due_frame_0_t frame = {.io = io, .schedule = schedule, .databases = databases,
	.databases_len = databases_len, .settings = settings, .error = error};

	std_landing(&frame, store_collect_collect_due_body_0, store_collect_collect_due_pad_0);
	return frame.ok;
}

/*
 * store_collect_unlisted takes an emptied tablespace out of a repository's
 * lists once the repository lists none of its packs (bugs row 99).
 */
static bool
store_collect_unlisted(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading, uint32 emptied,
					   HostError *error)
{
	ChunksErrorError failed;
	StdVec		spaces;
	bool		listed = false;

	if (!chunks_packs_backend_Packs_spaces(&repo->packs, io, reading, &spaces, &failed))
	{
		*error = chunks_packs_shared_raised(&failed);
		return false;
	}
	for (size_t k = 0, k_len = spaces.len; k < k_len; k++)
		listed |= STD_VEC_AT(&spaces, std_TupleU64U32_t, k).f1 == emptied;
	std_vec_free_Checked(&spaces, sizeof(std_TupleU64U32_t), _Alignof(std_TupleU64U32_t));
	if (listed)
		return true;
	if (!chunks_packs_backend_Packs_remove_space(&repo->packs, HOST_IO_IO_TAKES_SHORT(io), reading, emptied, &failed))
	{
		*error = chunks_packs_shared_raised(&failed);
		return false;
	}
	return true;
}

/*
 * store_collect_emptied removes the files due removal, and then a
 * repository's directory in an emptied tablespace once no pack file is left
 * in it, every one either copied out and retired with its readers ended or
 * never listed; a pack file still there, a pack being written included,
 * leaves the directory for a later ask. A writer that opens in the
 * tablespace after the directory's listing finds it gone and its pack's
 * write fails, which leaves its fold's delta for a retry that makes the
 * directory again, and its relation's own file there makes Postgres refuse
 * the DROP.
 */
static bool
store_collect_emptied(HostIoIo *io, uint32 database, uint32 emptied, HostError *error)
{
	ChunksErrorError failed;
	StdPathBuf	dir;
	StdVec		names;
	bool		has;
	bool		packed = false;
	bool		removed;
	bool		ok = true;

	if (!chunks_packs_removal_remove_due(io, &failed))
	{
		*error = chunks_packs_shared_raised(&failed);
		return false;
	}
	dir = host_tablespace_dir(database, emptied);
	if (!host_list_dir_if_there(HOST_IO_IO_READ_DOORS(io), (const char *) dir.bytes.ptr, dir.bytes.len, &has, &names,
								error))
		ok = false;
	else if (has)
	{
		for (size_t k = 0, k_len = names.len; k < k_len; k++)
		{
			const StdString *name = &STD_VEC_AT(&names, StdString, k);

			packed |= (name->len >= 5 && memcmp((const char *) name->ptr + name->len - 5, ".pack", 5) == 0) ||
				(name->len >= 9 && memcmp((const char *) name->ptr + name->len - 9, ".pack.tmp", 9) == 0);
		}
		std_vec_drop_Checked(&names, sizeof(StdString), _Alignof(StdString), std_String_drop);
		if (!packed)
			ok = host_remove_tree(HOST_IO_IO_IO_DOORS(io), (const char *) dir.bytes.ptr, dir.bytes.len, &removed,
								  error);
	}
	std_vec_free_Checked(&dir.bytes, 1, 1);
	return ok;
}

/* store_collect_asked is an ask's full collection under its database's lock, emptying the tablespace it names, the repository opened for it and closed after; its caller gives the lock back however it ends. */
static bool
store_collect_asked(HostIoIo *io, uint32 repository, const StoreCollectDatabase *database, uint32 emptied,
					const StoreCollectSettings *settings, bool *has_result, StoreCollectCollected *result,
					HostError *error)
{
	HostSharedReader reading;
	ChunksErrorError failed;
	RepoRepo	repo;
	StdVec		none;
	bool		ok;

	if (!host_shared_read(io, &reading, error))
		return false;
	if (!store_collect_opened(io, repository, database, &repo, error))
	{
		host_shared_Reader_drop(&reading);
		return false;
	}
	std_vec_new(&none, sizeof(uint64), _Alignof(uint64));
	ok = store_collect_collected(io, &repo, &reading, database->id, 0, settings->target_percent, emptied, &none,
								 has_result, result, error);
	if (ok && *has_result && emptied != CHUNKS_PACKS_DEFAULT_SPACE &&
		!store_collect_unlisted(io, &repo, &reading, emptied, error))
	{
		store_collect_Collected_drop(result);
		ok = false;
	}
	if (!ok)
		repo_Repo_drop(&repo);
	else if (!repo_Repo_close(&repo, io, &reading, &failed))
	{
		if (*has_result)
			store_collect_Collected_drop(result);
		*error = chunks_packs_shared_raised(&failed);
		ok = false;
	}
	host_shared_Reader_drop(&reading);
	if (ok && *has_result && emptied != CHUNKS_PACKS_DEFAULT_SPACE && !store_collect_emptied(io, repository, emptied, error))
	{
		store_collect_Collected_drop(result);
		ok = false;
	}
	return ok;
}

/*
 * store_collect_collect_asked runs the full collection a backend asked of the
 * collector worker in a database's repository, emptying the tablespace the
 * ask names (collected), recorded in the schedule as
 * its own, under the database's lock as collect_due takes it, and returns its
 * counts, a [u64; 3]: the packs it retired, those it copied out, and its live
 * bytes, all 0 when the database may not be collected, a session holds its
 * lock, or a base backup or a drop or move of the database stopped it.
 */
bool
store_collect_collect_asked(HostIoIo *io, StoreCollectSchedule *schedule, const StoreCollectDatabase *databases,
							size_t databases_len, uint32 id, uint32 emptied, const StoreCollectSettings *settings,
							uint64 *result, HostError *error)
{
	const StoreCollectDatabase *database = store_collect_find(databases, databases_len, id, true);
	StoreCollectCollected done;
	bool		locked;
	bool		has;
	bool		ok;

	result[0] = result[1] = result[2] = 0;
	if (database == NULL)
		return true;
	if (id == 0)
	{
		StdString	message;

		std_string_from_Checked(&message, "database 0", strlen("database 0"));
		*error = host_Error_fork((const uint8 *) "XX000", &message);
		return false;
	}
	if (!host_database_locked(io, id, &locked, error))
		return false;
	if (!locked)
		return true;
	ok = store_collect_asked(io, id, database, emptied, settings, &has, &done, error);
	host_database_unlocked(id);
	if (!ok)
		return false;
	if (!has)
		return true;
	result[0] = (uint64) done.retired;
	result[1] = (uint64) done.copied;
	result[2] = done.live;
	store_collect_Held_found(store_collect_held_entry(schedule, id), &done, true);
	return true;
}

#ifdef DOLTGRES_TEST_STORE

/* STORE_COLLECT_TESTS_UNWRAP is Result::unwrap's panic text. */
#define STORE_COLLECT_TESTS_UNWRAP "called `Result::unwrap()` on an `Err` value"

/* store_collect_Old_Debug_fmt is a BTreeSet<u64>'s Debug. */
static bool
store_collect_Old_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *old = value;
	StdDebugList s = std_fmt_debug_set(f);

	for (size_t k = 0, k_len = old->len; k < k_len; k++)
		std_DebugSet_entry(&s, &STD_VEC_AT(old, uint64, k), std_u64_Debug_fmt);
	return std_DebugSet_finish(&s);
}

/* store_collect_Collected_Debug_fmt is Collected's derived Debug. */
static bool
store_collect_Collected_Debug_fmt(const void *value, StdFormatter *f)
{
	const StoreCollectCollected *collected = value;
	StdDebugStruct b = std_fmt_debug_struct(f, "Collected", 9);

	std_DebugStruct_field(&b, "retired", 7, &collected->retired, std_usize_Debug_fmt);
	std_DebugStruct_field(&b, "copied", 6, &collected->copied, std_usize_Debug_fmt);
	std_DebugStruct_field(&b, "live", 4, &collected->live, std_u64_Debug_fmt);
	std_DebugStruct_field(&b, "moved", 5, &collected->moved, std_u64_Debug_fmt);
	std_DebugStruct_field(&b, "old", 3, &collected->old, store_collect_Old_Debug_fmt);
	return std_DebugStruct_finish(&b);
}

/* store_collect_Collected_PartialEq_eq is Collected's derived PartialEq. */
static bool
store_collect_Collected_PartialEq_eq(const StoreCollectCollected *self, const StoreCollectCollected *other)
{
	return self->retired == other->retired && self->copied == other->copied && self->live == other->live &&
		self->moved == other->moved && self->old.len == other->old.len &&
		memcmp(self->old.ptr, other->old.ptr, self->old.len * sizeof(uint64)) == 0;
}

/* store_collect_tests_option_eq is Option<Collected>'s PartialEq: both none, or both some and equal. */
static bool
store_collect_tests_option_eq(bool has, const StoreCollectCollected *a, bool other_has, const StoreCollectCollected *b)
{
	if (has != other_has)
		return false;
	return !has || store_collect_Collected_PartialEq_eq(a, b);
}

/* store_collect_tests_ok asserts a call's success, which unwraps at a line and column. */
static void
store_collect_tests_ok(bool ok, const HostError *error, uint32 line, uint32 col)
{
	if (!ok)
		STD_RESULT_EXPECT_FAILED(line, col, STORE_COLLECT_TESTS_UNWRAP, error, host_Error_Debug_fmt);
}

/* store_collect_tests_chunks_ok asserts a call of chunks' success, which unwraps at a line and column. */
static void
store_collect_tests_chunks_ok(bool ok, const ChunksErrorError *error, uint32 line, uint32 col)
{
	if (!ok)
		STD_RESULT_EXPECT_FAILED(line, col, STORE_COLLECT_TESTS_UNWRAP, error, chunks_error_Error_Debug_fmt);
}

/* store_collect_tests_settings returns the collector's settings at their defaults. */
static StoreCollectSettings
store_collect_tests_settings(void)
{
	StoreCollectSettings settings = {.target_percent = 50, .first_packs = 64,
		.full_interval = std_Duration_from_secs(24 * 60 * 60), .young_size = UINT64CONST(256) << 20,
	.young_packs = 16};

	return settings;
}

/* store_collect_tests_chunk returns a leaf chunk with a body: its address and its stored bytes. */
static ChunksAddressAddress
store_collect_tests_chunk(const uint8 *body, size_t body_len, StdVec *stored)
{
	ChunksHeaderHeader header = {.kind = CHUNKS_HEADER_TREE_LEAF, .addresses = {.tag = CHUNKS_HEADER_ADDRESSES_OFFSETS}};
	ChunksTrailerStamps stamps = chunks_trailer_Stamps_Default_default();
	ChunksAddressAddress address;
	StdVec		hashed;

	std_vec_new(&header.addresses.u.offsets, sizeof(uint32), _Alignof(uint32));
	chunks_header_Header_write(&header, body, body_len, &hashed);
	address = chunks_address_Address_of(hashed.ptr, hashed.len);
	chunks_trailer_store(hashed.ptr, hashed.len, NULL, 0, &stamps, stored);
	chunks_trailer_Stamps_drop(&stamps);
	std_vec_free_Checked(&hashed, 1, 1);
	return address;
}

/* store_collect_tests_written_t is a pack written by a test: its number, its chunk's address, and the chunk's locator. */
typedef struct store_collect_tests_written_t
{
	uint64		number;
	ChunksAddressAddress address;
	ChunksTrailerLocator at;
} store_collect_tests_written_t;

/* store_collect_tests_writer returns a writer of a repository's packs in pg_default, which unwraps at a line and column. */
static ChunksPackPackWriterInDir
store_collect_tests_writer(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading, uint32 line,
						   uint32 col)
{
	ChunksPackPackWriterInDir writer;
	ChunksErrorError failed;

	store_collect_tests_chunks_ok(chunks_packs_backend_Packs_writer(&repo->packs, io, reading,
																	CHUNKS_PACKS_DEFAULT_SPACE, &writer, &failed),
								  &failed, line, col);
	return writer;
}

/* store_collect_tests_added finishes a writer's pack and lists it, returning its number, which unwraps at lines and columns. */
static uint64
store_collect_tests_added(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading,
						  ChunksPackPackWriterInDir *writer, uint32 finish_line, uint32 finish_col, uint32 add_line,
						  uint32 add_col)
{
	ChunksErrorError failed;
	ChunksPackPack pack;
	uint64		number;

	store_collect_tests_chunks_ok(chunks_pack_PackWriterInDir_finish(writer, HOST_IO_IO_TAKES_SHORT(io), &pack, &failed),
								  &failed, finish_line, finish_col);
	number = pack.number;
	store_collect_tests_chunks_ok(chunks_packs_backend_Packs_add(&repo->packs, io, reading, &pack, &failed), &failed,
								  add_line, add_col);
	return number;
}

/* store_collect_tests_written writes a pack of one chunk with a body into a repository and lists it. */
static store_collect_tests_written_t
store_collect_tests_written(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading, const char *body)
{
	store_collect_tests_written_t written;
	ChunksPackPackWriterInDir writer = store_collect_tests_writer(io, repo, reading, 852, 72);
	ChunksErrorError failed;
	StdVec		stored;

	written.address = store_collect_tests_chunk((const uint8 *) body, strlen(body), &stored);
	store_collect_tests_chunks_ok(chunks_pack_PackWriterInDir_write(&writer, io, written.address, stored.ptr, stored.len,
																	true, &written.at, &failed), &failed, 854, 59);
	std_vec_free_Checked(&stored, 1, 1);
	written.number = store_collect_tests_added(io, repo, reading, &writer, 855, 38, 857, 43);
	return written;
}

/* store_collect_tests_named_t is a save's captures: the chunk its base record names. */
typedef struct store_collect_tests_named_t
{
	ChunksAddressAddress address;
	ChunksTrailerLocator at;
} store_collect_tests_named_t;

/* store_collect_tests_bases_t is a save's captures: the chunks its base records name. */
typedef struct store_collect_tests_bases_t
{
	const store_collect_tests_named_t *named;
	size_t		len;
} store_collect_tests_bases_t;

/* store_collect_tests_base is a save's gathering: a base for each chunk given, storage k + 1's record naming chunk k. */
static bool
store_collect_tests_base(void *captures, HostIoIo *io, const RepoRead *read, StdVec *result, ChunksErrorError *error)
{
	const store_collect_tests_bases_t *bases = captures;

	(void) io;
	(void) read;
	(void) error;
	std_vec_with_capacity_Checked(result, bases->len, sizeof(RepoGathered), _Alignof(RepoGathered));
	for (size_t k = 0; k < bases->len; k++)
	{
		RepoGathered gathered = {.tag = REPO_GATHERED_BASE};
		chunks_TupleU32Locator_t address = {0, bases->named[k].at};

		gathered.u.base.storage.tablespace = HOST_PG_DEFAULT;
		gathered.u.base.storage.relation = (uint32) k + 1;
		std_slice_to_vec_Checked(&gathered.u.base.record.bytes, (const uint8 *) &bases->named[k].address,
								 sizeof(ChunksAddressAddress), 1, 1);
		std_vec_with_capacity_Checked(&gathered.u.base.record.addresses, 1, sizeof(address),
									  _Alignof(chunks_TupleU32Locator_t));
		std_vec_push_Checked(&gathered.u.base.record.addresses, &address, sizeof(address),
							 _Alignof(chunks_TupleU32Locator_t));
		gathered.u.base.folded = 5;
		std_vec_push_Checked(result, &gathered, sizeof(gathered), _Alignof(RepoGathered));
	}
	return true;
}

/* store_collect_tests_after is a save's after-write, which does nothing. */
static bool
store_collect_tests_after(void *captures, HostIoIo *io, ChunksErrorError *error)
{
	(void) captures;
	(void) io;
	(void) error;
	return true;
}

/* store_collect_tests_saved_each saves a manifest whose base records name the chunks given, one each, which unwraps at a line and column. */
static void
store_collect_tests_saved_each(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading,
							   const store_collect_tests_named_t *named, size_t named_len, uint32 line, uint32 col)
{
	store_collect_tests_bases_t bases = {named, named_len};
	ChunksErrorError failed;

	store_collect_tests_chunks_ok(repo_Repo_save(repo, io, reading, false, NULL, store_collect_tests_base, &bases,
												 store_collect_tests_after, NULL, &failed), &failed, line, col);
}

/* store_collect_tests_saved saves a manifest whose base record names one chunk, which unwraps at a line and column. */
static void
store_collect_tests_saved(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading,
						  ChunksAddressAddress address, ChunksTrailerLocator at, uint32 line, uint32 col)
{
	store_collect_tests_named_t named = {address, at};

	store_collect_tests_saved_each(io, repo, reading, &named, 1, line, col);
}

/* store_collect_tests_created creates a repository in pg_default and returns it with a reader, which unwrap at a line. */
static RepoRepo
store_collect_tests_created(HostIoIo *io, uint32 number, uint32 line, HostSharedReader *reading)
{
	RepoRepo	repo;
	ChunksErrorError failed;
	HostError	error;

	store_collect_tests_chunks_ok(repo_Repo_create(io, number, HOST_PG_DEFAULT, &repo, &failed), &failed, line, 96);
	store_collect_tests_ok(host_shared_read(io, reading, &error), &error, line + 1, 46);
	return repo;
}

/* store_collect_tests_collected runs a full or young collection that must finish, whose result and option unwrap at a line and columns. */
static StoreCollectCollected
store_collect_tests_collected(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading, uint32 database,
							  uint64 limit, const StdVec *old, uint32 line, uint32 col, uint32 some_col)
{
	StoreCollectCollected done;
	StdVec		none;
	HostError	error;
	bool		has;

	std_vec_new(&none, sizeof(uint64), _Alignof(uint64));
	store_collect_tests_ok(store_collect_collected(io, repo, reading, database, limit, 50, CHUNKS_PACKS_DEFAULT_SPACE,
												   old == NULL ? &none : old, &has, &done, &error), &error, line, col);
	if (!has)
		std_option_unwrap_failed(RUST_FILE, line, some_col);
	return done;
}

/* store_collect_tests_numbers returns the numbers of a repository's listed packs, which unwraps at a line and column. */
static StdVec
store_collect_tests_numbers(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading, uint32 line,
							uint32 col)
{
	ChunksErrorError failed;
	StdVec		numbers;

	store_collect_tests_chunks_ok(chunks_packs_backend_Packs_numbers(&repo->packs, io, reading, &numbers, &failed),
								  &failed, line, col);
	return numbers;
}

/* store_collect_tests_reads_eq asserts at a line and column that a chunk read by a locator has its address, whose read unwraps at a column. */
static void
store_collect_tests_reads_eq(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading,
							 const ChunksAddressAddress *address, ChunksTrailerLocator at, uint32 line, uint32 col,
							 uint32 unwrap_col)
{
	ChunksChunkChunk chunk;
	ChunksErrorError failed;
	ChunksAddressAddress read;

	store_collect_tests_chunks_ok(chunks_packs_backend_Packs_read(&repo->packs, io, reading, address, at, &chunk,
																  &failed), &failed, line, unwrap_col);
	read = chunks_chunk_Chunk_address(&chunk);
	chunks_chunk_Chunk_drop(&chunk);
	STD_ASSERT_EQ(line, col, chunks_address_Address_Ord_cmp(&read, address) == STD_ORDERING_EQUAL, &read,
				  chunks_address_Address_Debug_fmt, address, chunks_address_Address_Debug_fmt);
}

/* store_collect_tests_holds reports whether numbers hold one. */
static bool
store_collect_tests_holds(const StdVec *numbers, uint64 number)
{
	for (size_t k = 0, k_len = numbers->len; k < k_len; k++)
		if (STD_VEC_AT(numbers, uint64, k) == number)
			return true;
	return false;
}

/* store_collect_tests_found returns where a repository's packs hold a chunk, whose find and option unwrap at a line and columns. */
static ChunksTrailerLocator
store_collect_tests_found(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading,
						  const ChunksAddressAddress *address, uint32 line, uint32 col, uint32 some_col)
{
	ChunksErrorError failed;
	ChunksTrailerLocator found;
	bool		has;

	store_collect_tests_chunks_ok(chunks_packs_backend_Packs_find(&repo->packs, io, reading, address, &has, &found,
																  &failed), &failed, line, col);
	if (!has)
		std_option_unwrap_failed(RUST_FILE, line, some_col);
	return found;
}

/* store_collect_tests_retired_eq asserts at a line that a collection retired the packs given. */
static void
store_collect_tests_retired_eq(const StoreCollectCollected *collected, size_t retired, uint32 line)
{
	STD_ASSERT_EQ(line, 9, collected->retired == retired, &collected->retired, std_usize_Debug_fmt, &retired,
				  std_usize_Debug_fmt);
}

/*
 * test_store_collect_tests_a_collection_retires_a_pack_nothing_names_and_keeps_a_named_one:
 * the collector text's first case: a full collection retires a pack whose
 * chunk nothing names and keeps the pack of a chunk the manifest's base
 * record names, which still reads.
 */
void
test_store_collect_tests_a_collection_retires_a_pack_nothing_names_and_keeps_a_named_one(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedReader reading;
	RepoRepo	repo = store_collect_tests_created(io, 830, 868, &reading);
	store_collect_tests_written_t kept = store_collect_tests_written(io, &repo, &reading, "named");
	store_collect_tests_written_t dropped = store_collect_tests_written(io, &repo, &reading, "dropped");
	StoreCollectCollected collected;
	StdVec		numbers;

	store_collect_tests_saved(io, &repo, &reading, kept.address, kept.at, 877, 57);
	collected = store_collect_tests_collected(io, &repo, &reading, 830, 0, NULL, 878, 90, 99);
	store_collect_tests_retired_eq(&collected, 1, 879);
	numbers = store_collect_tests_numbers(io, &repo, &reading, 880, 56);
	STD_ASSERT(881, 9, store_collect_tests_holds(&numbers, kept.number) &&
			   !store_collect_tests_holds(&numbers, dropped.number), "numbers.contains(&kept) && !numbers.contains(&dropped)");
	store_collect_tests_reads_eq(io, &repo, &reading, &kept.address, kept.at, 882, 9, 62);
	std_vec_free_Checked(&numbers, sizeof(uint64), _Alignof(uint64));
	store_collect_Collected_drop(&collected);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	simc_TestIo_drop(&test_io);
}

/*
 * test_store_collect_tests_the_verify_lists_again_a_retired_pack_a_missed_root_reaches:
 * a root the first capture misses leaves its pack unreached and retired, and
 * the verify's fresh capture reaches its chunk, lists the pack again under a
 * fresh number, and the chunk still reads, the collection counting it as no
 * retirement.
 */
void
test_store_collect_tests_the_verify_lists_again_a_retired_pack_a_missed_root_reaches(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedReader reading;
	RepoRepo	repo = store_collect_tests_created(io, 831, 893, &reading);
	store_collect_tests_written_t missed = store_collect_tests_written(io, &repo, &reading, "missed");
	StoreCollectCollected collected;
	ChunksErrorError failed;
	ChunksTrailerLocator found;
	StdVec		roots;
	StdVec		numbers;

	store_collect_tests_saved(io, &repo, &reading, missed.address, missed.at, 901, 57);
	store_collect_tests_chunks_ok(repo_Repo_roots(&repo, io, &roots, &failed), &failed, 902, 35);
	std_index_check(0, roots.len, RUST_FILE, 902, 20);
	STORE_COLLECT_HIDDEN.has = true;
	STORE_COLLECT_HIDDEN.of = 831;
	STORE_COLLECT_HIDDEN.address = STD_VEC_AT(&roots, TreesStoreTree, 0).address;
	std_vec_free_Checked(&roots, sizeof(TreesStoreTree), _Alignof(TreesStoreTree));
	collected = store_collect_tests_collected(io, &repo, &reading, 831, 0, NULL, 904, 90, 99);
	store_collect_tests_retired_eq(&collected, 0, 905);
	numbers = store_collect_tests_numbers(io, &repo, &reading, 906, 56);
	STD_ASSERT(907, 9, !store_collect_tests_holds(&numbers, missed.number), "!numbers.contains(&missed)");
	found = store_collect_tests_found(io, &repo, &reading, &missed.address, 908, 59, 68);
	STD_ASSERT(909, 9, store_collect_tests_holds(&numbers, found.pack), "numbers.contains(&found.pack)");
	store_collect_tests_reads_eq(io, &repo, &reading, &missed.address, found, 910, 9, 65);
	std_vec_free_Checked(&numbers, sizeof(uint64), _Alignof(uint64));
	store_collect_Collected_drop(&collected);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	simc_TestIo_drop(&test_io);
}

/*
 * test_store_collect_tests_a_collection_runs_nothing_while_a_base_backup_is_counted_in:
 * while a base backup is counted in a collection runs nothing and retires
 * nothing, and once it is counted out the same collection retires the pack
 * nothing names; a backup's start meets no running collection, so it doesn't
 * wait.
 */
void
test_store_collect_tests_a_collection_runs_nothing_while_a_base_backup_is_counted_in(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedReader reading;
	RepoRepo	repo = store_collect_tests_created(io, 832, 921, &reading);
	store_collect_tests_written_t dropped = store_collect_tests_written(io, &repo, &reading, "dropped");
	StoreCollectCollected collected;
	HostError	error;
	StdVec		none;
	StdVec		numbers;
	bool		has;

	store_collect_tests_ok(host_backup_begin(io, &error), &error, 924, 32);
	STD_ASSERT(925, 9, host_backups_running(), "host::backups_running()");
	std_vec_new(&none, sizeof(uint64), _Alignof(uint64));
	store_collect_tests_ok(store_collect_collected(io, &repo, &reading, 832, 0, 50, CHUNKS_PACKS_DEFAULT_SPACE, &none,
												   &has, &collected, &error), &error, 926, 85);
	if (!store_collect_tests_option_eq(has, &collected, false, NULL))
	{
		StdOptionRef left = {has, STD_ITEM(&collected, store_collect_Collected_Debug_fmt)};
		StdOptionRef right = {false, STD_ITEM(&collected, store_collect_Collected_Debug_fmt)};

		std_assert_failed(RUST_FILE, 926, 9, "==", &left, std_option_Debug_fmt, &right, std_option_Debug_fmt, NULL);
	}
	if (has)
		store_collect_Collected_drop(&collected);
	numbers = store_collect_tests_numbers(io, &repo, &reading, 927, 50);
	STD_ASSERT(927, 9, store_collect_tests_holds(&numbers, dropped.number),
			   "repo.packs.numbers(io, &reading).unwrap().contains(&dropped)");
	std_vec_free_Checked(&numbers, sizeof(uint64), _Alignof(uint64));
	host_backup_end();
	collected = store_collect_tests_collected(io, &repo, &reading, 832, 0, NULL, 929, 85, 94);
	store_collect_tests_retired_eq(&collected, 1, 929);
	store_collect_Collected_drop(&collected);
	numbers = store_collect_tests_numbers(io, &repo, &reading, 930, 51);
	STD_ASSERT(930, 9, !store_collect_tests_holds(&numbers, dropped.number),
			   "!repo.packs.numbers(io, &reading).unwrap().contains(&dropped)");
	std_vec_free_Checked(&numbers, sizeof(uint64), _Alignof(uint64));
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	simc_TestIo_drop(&test_io);
}

/*
 * test_store_collect_tests_a_paced_collection_naps_for_the_bytes_it_reads:
 * a collection paced at a byte a second naps, in all, about a second for
 * each byte its marking reads, and one unpaced naps not at all.
 */
void
test_store_collect_tests_a_paced_collection_naps_for_the_bytes_it_reads(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedReader reading;
	RepoRepo	repo = store_collect_tests_created(io, 835, 940, &reading);
	store_collect_tests_written_t paced = store_collect_tests_written(io, &repo, &reading, "paced");
	StoreCollectCollected collected;
	StdDuration zero = std_Duration_from_secs(0);
	StdDuration napped;
	StdDuration least;
	StdDuration most;

	store_collect_tests_saved(io, &repo, &reading, paced.address, paced.at, 948, 57);
	(void) simc_napped();
	collected = store_collect_tests_collected(io, &repo, &reading, 835, 0, NULL, 950, 74, 83);
	store_collect_Collected_drop(&collected);
	napped = simc_napped();
	STD_ASSERT_EQ(951, 9, std_Duration_cmp(&napped, &zero) == 0, &napped, std_Duration_Debug_fmt, &zero,
				  std_Duration_Debug_fmt);
	collected = store_collect_tests_collected(io, &repo, &reading, 835, 1, NULL, 952, 74, 83);
	store_collect_Collected_drop(&collected);
	napped = simc_napped();
	least = std_Duration_from_secs(chunks_trailer_Locator_length(&paced.at));
	most = std_Duration_from_secs(UINT64CONST(1) << 20);
	STD_ASSERT(954, 9, std_Duration_cmp(&napped, &least) > 0 && std_Duration_cmp(&napped, &most) < 0,
			   "napped > Duration::from_secs(u64::from(at.length())) && napped < Duration::from_secs(1 << 20)");
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	simc_TestIo_drop(&test_io);
}

/* store_collect_tests_held returns how many packs repository 833 lists, its open, count, and close unwrapping at lines 976 to 978. */
static size_t
store_collect_tests_held(HostIoIo *io, const HostSharedReader *reading)
{
	RepoRepo	repo;
	ChunksErrorError failed;
	StdVec		numbers;
	size_t		count;

	store_collect_tests_chunks_ok(repo_Repo_open(io, 833, HOST_PG_DEFAULT, &repo, &failed), &failed, 976, 98);
	numbers = store_collect_tests_numbers(io, &repo, reading, 977, 58);
	count = numbers.len;
	std_vec_free_Checked(&numbers, sizeof(uint64), _Alignof(uint64));
	store_collect_tests_chunks_ok(repo_Repo_close(&repo, io, reading, &failed), &failed, 978, 38);
	return count;
}

/* store_collect_tests_held_eq asserts at a line that repository 833 lists the packs given. */
static void
store_collect_tests_held_eq(HostIoIo *io, const HostSharedReader *reading, size_t want, uint32 line)
{
	size_t		got = store_collect_tests_held(io, reading);

	STD_ASSERT_EQ(line, 9, got == want, &got, std_usize_Debug_fmt, &want, std_usize_Debug_fmt);
}

/* store_collect_tests_due runs the worker's check with the default settings, which unwraps at a line and column. */
static void
store_collect_tests_due(HostIoIo *io, StoreCollectSchedule *schedule, const StoreCollectDatabase *databases,
						size_t databases_len, uint32 line, uint32 col)
{
	StoreCollectSettings settings = store_collect_tests_settings();
	HostError	error;

	store_collect_tests_ok(store_collect_collect_due(io, schedule, databases, databases_len, &settings, &error), &error,
						   line, col);
}

/*
 * test_store_collect_tests_the_check_collects_on_a_low_disk_and_reports_a_directory_no_database_names:
 * the worker's check leaves a repository that holds fewer than
 * collector_first_packs packs while the disk has room, collects it once the
 * disk is low, reports once at LOG a directory no database names, and
 * forgets what it kept of a database that is gone.
 */
void
test_store_collect_tests_the_check_collects_on_a_low_disk_and_reports_a_directory_no_database_names(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedReader reading;
	ChunksErrorError failed;
	HostError	error;
	static const uint32 ids[] = {833, 834};
	StoreCollectDatabase databases[] = {{833, HOST_PG_DEFAULT, true}};
	StoreCollectSchedule schedule;
	StdPathBuf	dir;
	StdString	unnamed;
	StdVec		logs;
	size_t		count = 0;
	size_t		once = 1;

	store_collect_tests_ok(host_shared_read(io, &reading, &error), &error, 965, 46);
	for (size_t k = 0; k < lengthof(ids); k++)
	{
		RepoRepo	repo;

		store_collect_tests_chunks_ok(repo_Repo_create(io, ids[k], HOST_PG_DEFAULT, &repo, &failed), &failed, 967, 99);
		(void) store_collect_tests_written(io, &repo, &reading, "dropped");
		store_collect_tests_chunks_ok(repo_Repo_close(&repo, io, &reading, &failed), &failed, 969, 38);
	}
	store_collect_Schedule_Default_default(&schedule);
	logs = simc_take_logs();
	store_tests_Logs_drop(&logs);
	store_collect_tests_due(io, &schedule, databases, lengthof(databases), 974, 65);
	store_collect_tests_held_eq(io, &reading, 1, 981);
	simc_set_disk_space(1, 100);
	store_collect_tests_due(io, &schedule, databases, lengthof(databases), 983, 65);
	simc_set_disk_space(UINT64CONST(1) << 40, UINT64CONST(2) << 40);
	store_collect_tests_held_eq(io, &reading, 0, 985);
	dir = host_tablespace_dir(834, HOST_PG_DEFAULT);
	{
		StdStr		shown = {dir.bytes.ptr, dir.bytes.len};
		StdFmtPiece pieces[] = {STD_FMT_LIT("\""), STD_FMT_ARG(0), STD_FMT_LIT("\" belongs to no database")};
		StdFmtArg	args[] = {{&shown, std_path_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_fmt_format_Checked(&unnamed, &arguments);
	}
	logs = simc_take_logs();
	for (size_t k = 0, k_len = logs.len; k < k_len; k++)
		if (std_str_contains_str_Checked(STD_VEC_AT(&logs, StdString, k).ptr, STD_VEC_AT(&logs, StdString, k).len,
										 unnamed.ptr, unnamed.len))
			count++;
	STD_ASSERT_EQ(987, 9, count == once, &count, std_usize_Debug_fmt, &once, std_usize_Debug_fmt);
	store_tests_Logs_drop(&logs);
	store_collect_tests_due(io, &schedule, NULL, 0, 988, 58);
	STD_ASSERT(989, 9, schedule.held.len == 0, "schedule.held.is_empty()");
	std_String_drop(&unnamed);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	store_collect_Schedule_drop(&schedule);
	host_shared_Reader_drop(&reading);
	simc_TestIo_drop(&test_io);
}

/*
 * test_store_collect_tests_a_second_collection_with_no_pack_written_since_runs:
 * a second collection with no pack written since the first, as a daily or a
 * low-disk one is, gives the deletion vector of a pack it keeps partly dead a
 * generation of its own rather than the first's.
 */
void
test_store_collect_tests_a_second_collection_with_no_pack_written_since_runs(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedReader reading;
	RepoRepo	repo = store_collect_tests_created(io, 836, 999, &reading);
	store_collect_tests_written_t again = store_collect_tests_written(io, &repo, &reading, "again");
	StoreCollectCollected collected;
	ChunksErrorError failed;
	uint64		newest;
	uint64		after;

	store_collect_tests_saved(io, &repo, &reading, again.address, again.at, 1007, 57);
	collected = store_collect_tests_collected(io, &repo, &reading, 836, 0, NULL, 1008, 74, 83);
	store_collect_Collected_drop(&collected);
	store_collect_tests_chunks_ok(chunks_packs_backend_Packs_newest_vector(&repo.packs, io, &reading, &newest, &failed),
								  &failed, 1009, 61);
	collected = store_collect_tests_collected(io, &repo, &reading, 836, 0, NULL, 1010, 74, 83);
	store_collect_Collected_drop(&collected);
	store_collect_tests_chunks_ok(chunks_packs_backend_Packs_newest_vector(&repo.packs, io, &reading, &after, &failed),
								  &failed, 1011, 56);
	STD_ASSERT(1011, 9, after >= newest, "repo.packs.newest_vector(io, &reading).unwrap() >= newest");
	store_collect_tests_reads_eq(io, &repo, &reading, &again.address, again.at, 1012, 9, 62);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	simc_TestIo_drop(&test_io);
}

/* store_collect_tests_noise returns bytes that compress to about their own length, for a chunk whose pack is mostly dead. */
static StdVec
store_collect_tests_noise(size_t len)
{
	uint64		state = UINT64CONST(0x2545f4914f6cdd1d);
	StdVec		noise;

	std_vec_with_capacity_Checked(&noise, len, 1, 1);
	for (size_t k = 0; k < len; k++)
	{
		uint8		byte;

		state ^= state << 13;
		state ^= state >> 7;
		state ^= state << 17;
		byte = (uint8) state;
		std_vec_push_Checked(&noise, &byte, 1, 1);
	}
	return noise;
}

/*
 * test_store_collect_tests_a_young_collection_retires_a_new_dead_pack_and_leaves_the_old_ones:
 * a full collection copies the live chunk of a mostly dead pack into a pack
 * it writes, which joins the old packs; a young collection after it retires
 * a dead pack written since and leaves the old ones, the copy's pack still
 * listed and its chunk reading, by the copy's locator and through the
 * forward from the one the base record holds; and once nothing names the
 * copy, a young collection still leaves its old pack and the next full one
 * retires it.
 */
void
test_store_collect_tests_a_young_collection_retires_a_new_dead_pack_and_leaves_the_old_ones(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedReader reading;
	RepoRepo	repo = store_collect_tests_created(io, 837, 1048, &reading);
	ChunksPackPackWriterInDir writer = store_collect_tests_writer(io, &repo, &reading, 1050, 73);
	ChunksErrorError failed;
	ChunksAddressAddress named;
	ChunksAddressAddress waste;
	ChunksTrailerLocator at;
	ChunksTrailerLocator wasted_at;
	ChunksTrailerLocator copy;
	StoreCollectCollected full;
	StoreCollectCollected young;
	StoreCollectCollected again;
	StoreCollectCollected last;
	store_collect_tests_written_t dead;
	store_collect_tests_written_t other;
	StdVec		stored;
	StdVec		noise;
	StdVec		numbers;
	uint64		mixed_number;
	size_t		one = 1;

	named = store_collect_tests_chunk((const uint8 *) "kept", 4, &stored);
	store_collect_tests_chunks_ok(chunks_pack_PackWriterInDir_write(&writer, io, named, stored.ptr, stored.len, true, &at,
																	&failed), &failed, 1052, 57);
	std_vec_free_Checked(&stored, 1, 1);
	noise = store_collect_tests_noise(8192);
	waste = store_collect_tests_chunk(noise.ptr, noise.len, &stored);
	std_vec_free_Checked(&noise, 1, 1);
	store_collect_tests_chunks_ok(chunks_pack_PackWriterInDir_write(&writer, io, waste, stored.ptr, stored.len, true,
																	&wasted_at, &failed), &failed, 1054, 48);
	std_vec_free_Checked(&stored, 1, 1);
	mixed_number = store_collect_tests_added(io, &repo, &reading, &writer, 1055, 39, 1057, 45);
	store_collect_tests_saved(io, &repo, &reading, named, at, 1035, 56);
	full = store_collect_tests_collected(io, &repo, &reading, 837, 0, NULL, 1059, 85, 94);
	STD_ASSERT_EQ(1060, 9, full.copied == one, &full.copied, std_usize_Debug_fmt, &one, std_usize_Debug_fmt);
	copy = store_collect_tests_found(io, &repo, &reading, &named, 1061, 58, 67);
	STD_ASSERT(1062, 9, copy.pack != mixed_number && store_collect_numbers_contains(&full.old, copy.pack) &&
			   !store_collect_numbers_contains(&full.old, mixed_number),
			   "copy.pack != mixed_number && full.old.contains(&copy.pack) && !full.old.contains(&mixed_number)");
	dead = store_collect_tests_written(io, &repo, &reading, "young and dead");
	young = store_collect_tests_collected(io, &repo, &reading, 837, 0, &full.old, 1064, 79, 88);
	numbers = store_collect_tests_numbers(io, &repo, &reading, 1065, 56);
	STD_ASSERT(1066, 9, !store_collect_tests_holds(&numbers, dead.number) &&
			   store_collect_tests_holds(&numbers, copy.pack) && store_collect_numbers_contains(&young.old, copy.pack),
			   "!numbers.contains(&dead) && numbers.contains(&copy.pack) && young.old.contains(&copy.pack)");
	std_vec_free_Checked(&numbers, sizeof(uint64), _Alignof(uint64));
	store_collect_tests_reads_eq(io, &repo, &reading, &named, copy, 1067, 9, 64);
	store_collect_tests_reads_eq(io, &repo, &reading, &named, at, 1068, 9, 62);
	other = store_collect_tests_written(io, &repo, &reading, "other");
	store_collect_tests_saved(io, &repo, &reading, other.address, other.at, 1035, 56);
	store_collect_tests_saved(io, &repo, &reading, other.address, other.at, 1035, 56);
	again = store_collect_tests_collected(io, &repo, &reading, 837, 0, &young.old, 1072, 80, 89);
	numbers = store_collect_tests_numbers(io, &repo, &reading, 1073, 50);
	STD_ASSERT(1073, 9, store_collect_tests_holds(&numbers, copy.pack) &&
			   store_collect_numbers_contains(&again.old, copy.pack),
			   "repo.packs.numbers(io, &reading).unwrap().contains(&copy.pack) && young.old.contains(&copy.pack)");
	std_vec_free_Checked(&numbers, sizeof(uint64), _Alignof(uint64));
	last = store_collect_tests_collected(io, &repo, &reading, 837, 0, NULL, 1074, 74, 83);
	store_collect_Collected_drop(&last);
	numbers = store_collect_tests_numbers(io, &repo, &reading, 1075, 51);
	STD_ASSERT(1075, 9, !store_collect_tests_holds(&numbers, copy.pack),
			   "!repo.packs.numbers(io, &reading).unwrap().contains(&copy.pack)");
	std_vec_free_Checked(&numbers, sizeof(uint64), _Alignof(uint64));
	store_collect_Collected_drop(&again);
	store_collect_Collected_drop(&young);
	store_collect_Collected_drop(&full);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	simc_TestIo_drop(&test_io);
}

/* store_collect_tests_stamped writes a pack of one record of a chunk under a transaction's stamp and lists it, returning its number and the record's locator. */
static store_collect_tests_written_t
store_collect_tests_stamped(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading,
							const StdVec *hashed, ChunksAddressAddress address, uint64 txn)
{
	store_collect_tests_written_t written = {.address = address};
	ChunksTrailerStamp stamp = {txn, 1};
	ChunksPackPackWriterInDir writer = store_collect_tests_writer(io, repo, reading, 1092, 77);
	ChunksTrailerStamps stamps;
	ChunksErrorError failed;
	StdVec		single;
	StdVec		stored;

	std_vec_with_capacity_Checked(&single, 1, sizeof(stamp), _Alignof(ChunksTrailerStamp));
	std_vec_push_Checked(&single, &stamp, sizeof(stamp), _Alignof(ChunksTrailerStamp));
	stamps = chunks_trailer_Stamps_FromVecStamp_from(&single);
	chunks_trailer_store(hashed->ptr, hashed->len, NULL, 0, &stamps, &stored);
	store_collect_tests_chunks_ok(chunks_pack_PackWriterInDir_write(&writer, io, address, stored.ptr, stored.len, true,
																	&written.at, &failed), &failed, 1093, 102);
	std_vec_free_Checked(&stored, 1, 1);
	written.number = store_collect_tests_added(io, repo, reading, &writer, 1094, 42, 1096, 48);
	chunks_trailer_Stamps_drop(&stamps);
	return written;
}

/*
 * test_store_collect_tests_a_chunk_named_by_address_alone_marks_every_record_of_it:
 * bugs row 119: two packs holding records of one address under other
 * stamps, as the nodes two tables of the same rows share, and a root naming
 * that address alone; a full collection marks both records, retiring neither
 * pack, and each still reads.
 */
void
test_store_collect_tests_a_chunk_named_by_address_alone_marks_every_record_of_it(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedReader reading;
	RepoRepo	repo = store_collect_tests_created(io, 838, 1086, &reading);
	ChunksHeaderHeader header = {.kind = CHUNKS_HEADER_TREE_LEAF, .addresses = {.tag = CHUNKS_HEADER_ADDRESSES_OFFSETS}};
	ChunksAddressAddress address;
	store_collect_tests_written_t records[2];
	StoreCollectCollected collected;
	StdVec		hashed;
	StdVec		numbers;

	std_vec_new(&header.addresses.u.offsets, sizeof(uint32), _Alignof(uint32));
	chunks_header_Header_write(&header, (const uint8 *) "same", 4, &hashed);
	address = chunks_address_Address_of(hashed.ptr, hashed.len);
	records[0] = store_collect_tests_stamped(io, &repo, &reading, &hashed, address, 7);
	records[1] = store_collect_tests_stamped(io, &repo, &reading, &hashed, address, 8);
	store_collect_tests_saved(io, &repo, &reading, address, chunks_trailer_Locator_Default_default(), 1035, 56);
	collected = store_collect_tests_collected(io, &repo, &reading, 838, 0, NULL, 1101, 90, 99);
	store_collect_tests_retired_eq(&collected, 0, 1102);
	numbers = store_collect_tests_numbers(io, &repo, &reading, 1103, 56);
	for (size_t k = 0; k < lengthof(records); k++)
	{
		if (!store_collect_tests_holds(&numbers, records[k].number))
		{
			StdFmtPiece pieces[] = {STD_FMT_LIT("pack "), STD_FMT_ARG(0), STD_FMT_LIT(" was retired")};
			StdFmtArg	args[] = {{&records[k].number, std_u64_Display_fmt}};
			StdFmtArguments arguments = {pieces, lengthof(pieces), args};

			std_panic_fmt(RUST_FILE, 1105, 13, &arguments);
		}
		store_collect_tests_reads_eq(io, &repo, &reading, &address, records[k].at, 1106, 13, 68);
	}
	std_vec_free_Checked(&numbers, sizeof(uint64), _Alignof(uint64));
	store_collect_Collected_drop(&collected);
	std_vec_free_Checked(&hashed, 1, 1);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	simc_TestIo_drop(&test_io);
}


/* STORE_COLLECT_TESTS_PARTLY is how many packs a test's full collection keeps partly dead. */
#define STORE_COLLECT_TESTS_PARTLY 3

/* store_collect_tests_partly writes a pack of a live chunk, noise led by a byte, beside a dead chunk into a repository, lists it, and returns its number. */
static uint64
store_collect_tests_partly(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading, uint8 lead,
						   store_collect_tests_named_t *live)
{
	ChunksPackPackWriterInDir writer = store_collect_tests_writer(io, repo, reading, 1110, 73);
	ChunksErrorError failed;
	ChunksAddressAddress dead;
	ChunksTrailerLocator dead_at;
	uint8		body[] = {'d', lead};
	StdVec		noise = store_collect_tests_noise(8192);
	StdVec		stored;

	STD_VEC_AT(&noise, uint8, 0) = lead;
	live->address = store_collect_tests_chunk(noise.ptr, noise.len, &stored);
	std_vec_free_Checked(&noise, 1, 1);
	store_collect_tests_chunks_ok(chunks_pack_PackWriterInDir_write(&writer, io, live->address, stored.ptr, stored.len,
																	true, &live->at, &failed), &failed, 1111, 57);
	std_vec_free_Checked(&stored, 1, 1);
	dead = store_collect_tests_chunk(body, sizeof(body), &stored);
	store_collect_tests_chunks_ok(chunks_pack_PackWriterInDir_write(&writer, io, dead, stored.ptr, stored.len, true,
																	&dead_at, &failed), &failed, 1112, 57);
	std_vec_free_Checked(&stored, 1, 1);
	return store_collect_tests_added(io, repo, reading, &writer, 1113, 38, 1113, 43);
}

/* store_collect_tests_vectored_t is a read's captures: a test's partly dead packs, and their deletion vectors' generations, 0 for none. */
typedef struct store_collect_tests_vectored_t
{
	const uint64 *numbers;
	uint64	   *generations;
} store_collect_tests_vectored_t;

/* store_collect_tests_vectored is a read of the pack table: the generation of each partly dead pack's deletion vector. */
static bool
store_collect_tests_vectored(void *captures, ChunksPacksViewView *view, HostIoIo *io, ChunksErrorError *error)
{
	const store_collect_tests_vectored_t *vectored = captures;
	const ChunksPacksTable *table = chunks_packs_view_View_table(view);

	(void) io;
	(void) error;
	for (size_t k = 0; k < STORE_COLLECT_TESTS_PARTLY; k++)
	{
		vectored->generations[k] = 0;
		for (size_t j = 0, j_len = table->packs.len; j < j_len; j++)
		{
			const chunks_TupleU64Listed_t *entry = &STD_VEC_AT(&table->packs, chunks_TupleU64Listed_t, j);

			if (entry->f0 == vectored->numbers[k] && entry->f1.has_dead)
				vectored->generations[k] = entry->f1.dead;
		}
	}
	return true;
}

/* store_collect_tests_generations returns the generations of a test's partly dead packs' deletion vectors, 0 for none, which unwraps at a line and column. */
static void
store_collect_tests_generations(HostIoIo *io, const RepoRepo *repo, const uint64 *numbers, uint64 *generations,
								uint32 line, uint32 col)
{
	store_collect_tests_vectored_t vectored = {numbers, generations};
	ChunksErrorError failed;

	store_collect_tests_chunks_ok(chunks_packs_backend_Packs_viewed(&repo->packs, io, store_collect_tests_vectored,
																	&vectored, &failed), &failed, line, col);
}

/* store_collect_tests_covered returns how many of a test's partly dead packs have a deletion vector newer than before, asserting at a line that they lead the packs. */
static size_t
store_collect_tests_covered(HostIoIo *io, const RepoRepo *repo, const uint64 *numbers, const uint64 *before,
							uint32 line)
{
	uint64		generations[STORE_COLLECT_TESTS_PARTLY];
	size_t		covered = 0;

	store_collect_tests_generations(io, repo, numbers, generations, line, 13);
	while (covered < STORE_COLLECT_TESTS_PARTLY && generations[covered] > before[covered])
		covered++;
	for (size_t k = covered; k < STORE_COLLECT_TESTS_PARTLY; k++)
		STD_ASSERT(line, 9, generations[k] == before[k],
				   "a pack has a new deletion vector while one before it has none");
	return covered;
}

/* store_collect_tests_files_t is a read's captures: the directory of a repository's packs, and the files its lists name there, a Vec<PathBuf>. */
typedef struct store_collect_tests_files_t
{
	StdPathBuf	dir;
	StdVec		named;
} store_collect_tests_files_t;

/* store_collect_tests_listed_files is a read of the lists: each listed pack's file and the vector the table names for it, and each run the runs file lists. */
static bool
store_collect_tests_listed_files(void *captures, ChunksPacksViewView *view, HostIoIo *io, ChunksErrorError *error)
{
	store_collect_tests_files_t *files = captures;
	const ChunksPacksTable *table = chunks_packs_view_View_table(view);
	const ChunksPacksViewReadRuns *runs = chunks_packs_view_runs_get(&view->runs, CHUNKS_PACKS_DEFAULT_SPACE);

	(void) io;
	(void) error;
	std_slice_to_vec_Checked(&files->dir.bytes, view->dir.bytes.ptr, view->dir.bytes.len, 1, 1);
	for (size_t k = 0, k_len = table->packs.len; k < k_len; k++)
	{
		const chunks_TupleU64Listed_t *entry = &STD_VEC_AT(&table->packs, chunks_TupleU64Listed_t, k);
		StdArcInner *const *pack = chunks_packs_view_View_pack(view, entry->f0);
		const char *path;
		size_t		path_len;
		StdPathBuf	file;

		STD_ASSERT(1134, 9, pack != NULL, "a pack the table lists has no reader");
		path = chunks_pack_PackReader_path(std_arc_Deref_deref(*pack, _Alignof(ChunksPackPackReader)), &path_len);
		std_path_to_path_buf(path, path_len, &file);
		std_vec_push_Checked(&files->named, &file, sizeof(file), _Alignof(StdPathBuf));
		if (!entry->f1.has_dead)
			continue;
		file = chunks_dead_Dead_path(path, path_len, entry->f1.dead);
		std_vec_push_Checked(&files->named, &file, sizeof(file), _Alignof(StdPathBuf));
	}
	for (size_t k = 0, k_len = runs == NULL ? 0 : runs->list.lines.len; k < k_len; k++)
	{
		const StdString *line = &STD_VEC_AT(&runs->list.lines, StdString, k);
		StdStr		name = chunks_packs_run_name(line->ptr, line->len);
		StdPathBuf	file;

		std_path_join((const char *) files->dir.bytes.ptr, files->dir.bytes.len, name.ptr, name.len, &file);
		std_vec_push_Checked(&files->named, &file, sizeof(file), _Alignof(StdPathBuf));
	}
	return true;
}

/* store_collect_tests_paths_drop drops a Vec<PathBuf>. */
static void
store_collect_tests_paths_drop(StdVec *paths)
{
	for (size_t k = 0, k_len = paths->len; k < k_len; k++)
		std_vec_free_Checked(&STD_VEC_AT(paths, StdPathBuf, k).bytes, 1, 1);
	std_vec_free_Checked(paths, sizeof(StdPathBuf), _Alignof(StdPathBuf));
}

/* store_collect_tests_paths_hold reports whether a Vec<PathBuf> holds a path. */
static bool
store_collect_tests_paths_hold(const StdVec *paths, const StdPathBuf *path)
{
	for (size_t k = 0, k_len = paths->len; k < k_len; k++)
	{
		const StdVec *bytes = &STD_VEC_AT(paths, StdPathBuf, k).bytes;

		if (bytes->len == path->bytes.len && memcmp(bytes->ptr, path->bytes.ptr, bytes->len) == 0)
			return true;
	}
	return false;
}

/* store_collect_tests_ends_with reports whether a name's bytes end with some text. */
static bool
store_collect_tests_ends_with(const uint8 *name, size_t name_len, const char *ending)
{
	size_t		len = strlen(ending);

	return name_len >= len && memcmp(name + name_len - len, ending, len) == 0;
}

/*
 * store_collect_tests_unnamed returns the pack, run, and vector files in the
 * directory of a repository's packs that neither its lists name nor its
 * removal queue holds, a Vec<PathBuf>, asserting at a line that the queue
 * holds no file the lists name, which the reads unwrap at too.
 */
static StdVec
store_collect_tests_unnamed(HostIoIo *io, const RepoRepo *repo, uint32 id, uint32 line)
{
	store_collect_tests_files_t files;
	ChunksErrorError failed;
	HostError	error;
	StdVec		queued;
	StdVec		names;
	StdVec		unnamed;

	std_vec_new(&files.named, sizeof(StdPathBuf), _Alignof(StdPathBuf));
	store_collect_tests_chunks_ok(chunks_packs_backend_Packs_viewed(&repo->packs, io, store_collect_tests_listed_files,
																	&files, &failed), &failed, line, 13);
	store_collect_tests_chunks_ok(chunks_packs_removal_queued(io, id, &queued, &failed), &failed, line, 13);
	for (size_t k = 0, k_len = queued.len; k < k_len; k++)
	{
		StdString	name = chunks_packs_removal_file_name(&STD_VEC_AT(&queued, ChunksPacksSingleRemoval, k));
		StdPathBuf	file;

		std_path_join((const char *) files.dir.bytes.ptr, files.dir.bytes.len, (const char *) name.ptr, name.len,
					  &file);
		std_vec_free_Checked(&name, 1, 1);
		STD_ASSERT(line, 9, !store_collect_tests_paths_hold(&files.named, &file),
				   "the removal queue holds a file the lists still name");
		std_vec_push_Checked(&files.named, &file, sizeof(file), _Alignof(StdPathBuf));
	}
	std_vec_free_Checked(&queued, sizeof(ChunksPacksSingleRemoval), _Alignof(ChunksPacksSingleRemoval));
	store_collect_tests_ok(host_list_dir(HOST_IO_IO_READ_DOORS(io), (const char *) files.dir.bytes.ptr,
										 files.dir.bytes.len, &names, &error), &error, line, 13);
	std_vec_new(&unnamed, sizeof(StdPathBuf), _Alignof(StdPathBuf));
	for (size_t k = 0, k_len = names.len; k < k_len; k++)
	{
		const StdString *name = &STD_VEC_AT(&names, StdString, k);
		StdPathBuf	file;

		if (!store_collect_tests_ends_with(name->ptr, name->len, ".pack") &&
			!store_collect_tests_ends_with(name->ptr, name->len, ".idx") &&
			!store_collect_tests_ends_with(name->ptr, name->len, ".dead"))
			continue;
		std_path_join((const char *) files.dir.bytes.ptr, files.dir.bytes.len, (const char *) name->ptr, name->len,
					  &file);
		if (store_collect_tests_paths_hold(&files.named, &file))
			std_vec_free_Checked(&file.bytes, 1, 1);
		else
			std_vec_push_Checked(&unnamed, &file, sizeof(file), _Alignof(StdPathBuf));
	}
	std_vec_drop_Checked(&names, sizeof(StdString), _Alignof(StdString), std_String_drop);
	store_collect_tests_paths_drop(&files.named);
	std_vec_free_Checked(&files.dir.bytes, 1, 1);
	return unnamed;
}

/*
 * store_collect_tests_retirements reports whether every file a failed
 * collection left neither listed nor queued is a vector or the retired
 * pack's own file or run, with that pack no longer listed: what a failed
 * Retired_queue leaves, bugs row 500, which a pack's restore or the stray
 * pass doesn't take.
 */
static bool
store_collect_tests_retirements(const StdVec *unnamed, const StdVec *listed, const store_collect_tests_written_t *dead,
								const StdPathBuf *dead_path)
{
	size_t		stem = dead_path->bytes.len - strlen(".pack");

	if (store_collect_tests_holds(listed, dead->number))
		return false;
	for (size_t k = 0, k_len = unnamed->len; k < k_len; k++)
	{
		const StdVec *bytes = &STD_VEC_AT(unnamed, StdPathBuf, k).bytes;
		bool		of_dead = bytes->len > stem && memcmp(bytes->ptr, dead_path->bytes.ptr, stem) == 0;

		if (!store_collect_tests_ends_with(bytes->ptr, bytes->len, ".dead") &&
			!(of_dead && (store_collect_tests_ends_with(bytes->ptr, bytes->len, ".pack") ||
						  store_collect_tests_ends_with(bytes->ptr, bytes->len, ".idx"))))
			return false;
	}
	return true;
}

/* store_collect_tests_collect_t is a caught collection's captures: its arguments, and what it answered. */
typedef struct store_collect_tests_collect_t
{
	HostIoIo   *io;
	const RepoRepo *repo;
	const HostSharedReader *reading;
	uint32		id;
	const StdVec *none;
	bool		finished;
	bool		has;
	StoreCollectCollected collected;
	HostError	error;
} store_collect_tests_collect_t;

/* store_collect_tests_collect_body runs a full collection of a repository's packs, for a catch of its unwind. */
static void
store_collect_tests_collect_body(void *captures)
{
	store_collect_tests_collect_t *c = captures;

	c->finished = store_collect_collected(c->io, c->repo, c->reading, c->id, 0, 50, CHUNKS_PACKS_DEFAULT_SPACE, c->none,
										  &c->has, &c->collected, &c->error);
}

/*
 * store_collect_tests_swept runs a full collection of a test's packs, the
 * kept ones and a pack of a dead chunk alone, in a fresh repository once
 * for each n, after a first full collection when `again` is set, until an n
 * past its last lets it finish. It fails each at its n-th check for a
 * cancel or, with `allocation`, at its n-th allocation, an allocation's
 * returned 53200 or its unwind alike, and checks what each failure leaves.
 * It returns whether some cancel fell between two kept packs' new vectors,
 * or some allocation failure between the keep and the install, and sets
 * restored when some failure left the retired pack listed again under
 * another number, a failed verify's restore.
 */
static bool
store_collect_tests_swept(uint32 *id, bool allocation, bool again, bool *restored)
{
	bool		between = false;

	*restored = false;

	for (uint32 n = 0;; n++)
	{
		SimcTestIo	test_io = simc_io();
		HostIoIo   *io = &test_io.io;
		ChunksFileInterruptible interruptible = {&CHUNKS_FILE_IO_INTERRUPTIBLE_OPS, io};
		HostSharedReader reading;
		RepoRepo	repo = store_collect_tests_created(io, ++*id, 1115, &reading);
		store_collect_tests_named_t named[STORE_COLLECT_TESTS_PARTLY];
		uint64		numbers[STORE_COLLECT_TESTS_PARTLY];
		uint64		before[STORE_COLLECT_TESTS_PARTLY] = {0};
		store_collect_tests_written_t dead = {0};
		StoreCollectCollected collected;
		ChunksPacksCheckChecked checked;
		store_collect_tests_collect_t collect;
		ChunksErrorError failed;
		StdArcInner *reader;
		StdPathBuf	dead_path;
		StdCaught	caught;
		StdVec		listed;
		StdVec		unnamed;
		StdVec		none;
		uint64		collecting;
		uint64		held;
		size_t		covered;
		bool		finished;
		bool		leaked;
		bool		unwound;
		bool		has;

		STD_ASSERT(1116, 9, n < 65536, "a full collection fails at fewer than 65536 points");
		for (size_t k = 0; k < STORE_COLLECT_TESTS_PARTLY; k++)
			numbers[k] = store_collect_tests_partly(io, &repo, &reading, (uint8) k, &named[k]);
		store_collect_tests_saved_each(io, &repo, &reading, named, STORE_COLLECT_TESTS_PARTLY, 1118, 9);
		std_vec_new(&none, sizeof(uint64), _Alignof(uint64));
		if (again)
		{
			collected = store_collect_tests_collected(io, &repo, &reading, *id, 0, &none, 1119, 74, 83);
			store_collect_Collected_drop(&collected);
			store_collect_tests_generations(io, &repo, numbers, before, 1119, 13);
		}
		dead = store_collect_tests_written(io, &repo, &reading, "retired");
		store_collect_tests_chunks_ok(chunks_packs_backend_Packs_pack_reader(&repo.packs, io, &reading, dead.number,
																			 &has, &reader, &failed), &failed, 1120,
									  13);
		STD_ASSERT(1120, 9, has, "a test's dead pack has no reader");
		{
			size_t		path_len;
			const char *path = chunks_pack_PackReader_path(std_arc_Deref_deref(reader, _Alignof(ChunksPackPackReader)),
														   &path_len);

			std_path_to_path_buf(path, path_len, &dead_path);
		}
		chunks_pack_ArcPackReader_drop(reader);
		collecting = simc_collecting();
		held = simc_interrupts_held();
		if (allocation)
			simc_fail_allocation(n + 1);
		else
			simc_interrupt_at(n);
		collect.io = io;
		collect.repo = &repo;
		collect.reading = &reading;
		collect.id = *id;
		collect.none = &none;
		collect.finished = false;
		unwound = !std_catch_unwind(store_collect_tests_collect_body, &collect, &caught);
		simc_fail_allocation(0);
		simc_interrupt_at(PG_UINT64_MAX);
		if (unwound)
			std_Caught_drop_Checked(&caught);
		finished = collect.finished;
		has = collect.has;
		collected = collect.collected;
		if (finished)
		{
			if (has)
				store_collect_Collected_drop(&collected);
			STD_ASSERT(1121, 9,
					   store_collect_tests_covered(io, &repo, numbers, before, 1121) == STORE_COLLECT_TESTS_PARTLY,
					   "a full collection gave a pack it keeps partly dead no new deletion vector");
			unnamed = store_collect_tests_unnamed(io, &repo, *id, 1122);
			STD_ASSERT(1122, 9, unnamed.len == 0, "a full collection left a file neither listed nor queued");
			store_collect_tests_paths_drop(&unnamed);
			std_vec_free_Checked(&dead_path.bytes, 1, 1);
			std_vec_free_Checked(&none, sizeof(uint64), _Alignof(uint64));
			host_shared_Reader_drop(&reading);
			repo_Repo_drop(&repo);
			simc_TestIo_drop(&test_io);
			return between;
		}
		STD_ASSERT(1123, 9, allocation || !unwound, "a cancelled collection unwound");
		if (!unwound)
		{
			STD_ASSERT(1123, 9, memcmp(host_Error_sqlstate(&collect.error), allocation ? "53200" : "57014", 5) == 0,
					   "a full collection failed with other than its fault's error");
			host_Error_drop(&collect.error);
		}
		STD_ASSERT(1123, 9, simc_collecting() == collecting, "a failed collection left its collection begun");
		STD_ASSERT(1123, 9, simc_interrupts_held() == held, "a failed collection left the interrupts held");
		listed = store_collect_tests_numbers(io, &repo, &reading, 1124, 56);
		covered = store_collect_tests_covered(io, &repo, numbers, before, 1124);
		for (size_t k = 0; k < STORE_COLLECT_TESTS_PARTLY; k++)
		{
			STD_ASSERT(1125, 13, store_collect_tests_holds(&listed, numbers[k]),
					   "a failed collection retired a pack it keeps");
			store_collect_tests_reads_eq(io, &repo, &reading, &named[k].address, named[k].at, 1126, 13, 64);
		}
		if (store_collect_tests_holds(&listed, dead.number))
			store_collect_tests_reads_eq(io, &repo, &reading, &dead.address, dead.at, 1126, 13, 64);
		else
		{
			ChunksTrailerLocator found;

			store_collect_tests_chunks_ok(chunks_packs_backend_Packs_find(&repo.packs, io, &reading, &dead.address, &has,
																		  &found, &failed), &failed, 1126, 13);
			*restored = *restored || (has && store_collect_tests_holds(&listed, found.pack));
		}
		unnamed = store_collect_tests_unnamed(io, &repo, *id, 1127);
		leaked = allocation && store_collect_tests_retirements(&unnamed, &listed, &dead, &dead_path);
		for (size_t k = 0, k_len = unnamed.len; k < k_len && !leaked; k++)
		{
			const StdVec *bytes = &STD_VEC_AT(&unnamed, StdPathBuf, k).bytes;

			STD_ASSERT(1127, 13, store_collect_tests_ends_with(bytes->ptr, bytes->len, ".dead"),
					   "a failed collection left a pack or run file neither listed nor queued, but for bugs row 500's");
		}
		store_collect_tests_paths_drop(&unnamed);
		between = between || (allocation ? covered == STORE_COLLECT_TESTS_PARTLY &&
							  store_collect_tests_holds(&listed, dead.number) :
							  covered > 0 && covered < STORE_COLLECT_TESTS_PARTLY);
		std_vec_free_Checked(&listed, sizeof(uint64), _Alignof(uint64));
		store_collect_tests_generations(io, &repo, numbers, before, 1128, 13);
		collected = store_collect_tests_collected(io, &repo, &reading, *id, 0, &none, 1129, 74, 83);
		store_collect_Collected_drop(&collected);
		STD_ASSERT(1129, 9,
				   store_collect_tests_covered(io, &repo, numbers, before, 1129) == STORE_COLLECT_TESTS_PARTLY,
				   "a full collection after a failed one gave a pack it keeps partly dead no new deletion vector");
		for (size_t k = 0; k < STORE_COLLECT_TESTS_PARTLY; k++)
			store_collect_tests_reads_eq(io, &repo, &reading, &named[k].address, named[k].at, 1130, 13, 64);
		chunks_packs_check_check(interruptible, *id, HOST_PG_DEFAULT, &checked);
		STD_ASSERT(1131, 9, checked.found.len == 0,
				   "the packs' check after a failed collection and another found something");
		chunks_packs_check_Checked_drop(&checked);
		unnamed = store_collect_tests_unnamed(io, &repo, *id, 1132);

		/*
		 * TODO: bugs row 500: a failed Retired_queue leaves the retirement's
		 * pack and run files neither listed nor queued; this exception goes
		 * when it lands.
		 */
		STD_ASSERT(1132, 9, unnamed.len == 0 || leaked,
				   "a full collection after a failed one left a file neither listed nor queued, but for bugs row 500's");
		store_collect_tests_paths_drop(&unnamed);
		std_vec_free_Checked(&dead_path.bytes, 1, 1);
		std_vec_free_Checked(&none, sizeof(uint64), _Alignof(uint64));
		host_shared_Reader_drop(&reading);
		repo_Repo_drop(&repo);
		simc_TestIo_drop(&test_io);
	}
}

/* STORE_COLLECT_TESTS_QUEUED is the generation of a vector a test's removal queue holds and no file has, past any a collection would give. */
#define STORE_COLLECT_TESTS_QUEUED 1000

/* STORE_COLLECT_TESTS_STRAYS is how many generations of never-named vectors a test leaves beside a pack, past any a collection would give. */
#define STORE_COLLECT_TESTS_STRAYS 64

/*
 * store_collect_tests_crashed writes beside a repository's pack what a crash
 * in or after a vector's write and before its naming leaves: a vector of
 * each generation from 1 that the table never named, and a write's
 * temporary of the next, which unwraps at a line.
 */
static void
store_collect_tests_crashed(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading, uint64 number,
							uint32 line)
{
	static const uint8 torn[] = {0x64, 0x65};
	uint64		offset = 0;
	StdBTreeMap none = std_btree_BTreeSetU64_FromArray_from(&offset, 1);
	ChunksErrorError failed;
	StdArcInner *reader;
	StdPathBuf	temp;
	StdIoError	io_error;
	const char *path;
	size_t		path_len;
	bool		has;

	store_collect_tests_chunks_ok(chunks_packs_backend_Packs_pack_reader(&repo->packs, io, reading, number, &has,
																		 &reader, &failed), &failed, line, 13);
	STD_ASSERT(line, 9, has, "a test's pack has no reader");
	path = chunks_pack_PackReader_path(std_arc_Deref_deref(reader, _Alignof(ChunksPackPackReader)), &path_len);
	for (uint64 generation = 1; generation <= STORE_COLLECT_TESTS_STRAYS; generation++)
		store_collect_tests_chunks_ok(chunks_dead_Dead_write(HOST_IO_IO_IO_DOORS(io), path, path_len, generation, &none,
															 &failed), &failed, line, 13);
	temp = chunks_dead_Dead_path(path, path_len, STORE_COLLECT_TESTS_STRAYS + 1);
	temp.bytes.len -= strlen("dead");
	std_vec_extend_from_slice_Checked(&temp.bytes, "tmp", strlen("tmp"), 1, 1);
	if (!std_fs_write(temp.bytes.ptr, temp.bytes.len, torn, sizeof(torn), &io_error))
		STD_RESULT_EXPECT_FAILED(line, 9, "called `Result::unwrap()` on an `Err` value", &io_error,
								 std_io_Error_Debug_fmt);
	std_vec_free_Checked(&temp.bytes, 1, 1);
	chunks_pack_ArcPackReader_drop(reader);
	std_btree_BTreeMapU64Set_drop(&none);
}

/*
 * test_store_collect_tests_a_cancel_at_any_check_of_a_full_collection_leaves_its_packs_whole:
 * bugs rows 499 and 503: three packs a full collection keeps partly dead,
 * each a live chunk beside a dead one, and a pack of a dead chunk alone,
 * which it retires; a cancel at each of the collection's checks in turn, in
 * a repository's first full collection and in its second, which replaces
 * each kept pack's deletion vector, and an allocation failure at each of a
 * first collection's allocations, stop it with Postgres' error, its
 * collection ended and the interrupts not held, every kept pack still listed
 * and its chunk reading, the dead pack's chunk reading while it is listed
 * (a failed verify restores it), the kept packs given a new vector leading
 * the three, and every pack and run file beside them named by the lists or
 * queued for removal, a vector named by neither the only stray, but for what
 * a failed Retired_queue leaves (bugs row 500); some cancel falls between
 * two kept packs' vectors, and some allocation failure between the keep and
 * the install; and after each, a collection finishes, every kept pack has a
 * new vector, the packs' check finds nothing, and every pack, run, and
 * vector file is named or queued, none of them both, but for row 500's.
 * Beside what a crash between a vector's write and its naming leaves (bugs
 * row 499, H14), never-named vectors of the generations a collection would
 * give and a write's temporary, a full collection queues each, removes the
 * temporary, and gives its own vectors a generation past them; and beside
 * a vector the removal queue holds whose file is already gone, as a
 * duplicate entry's second is once the first removed it, a full collection
 * gives its vectors a generation past the queued one, so no entry removes a
 * vector written again under its name.
 */
void
test_store_collect_tests_a_cancel_at_any_check_of_a_full_collection_leaves_its_packs_whole(void)
{
	uint32		id = 40000;
	bool		restored;

	STD_ASSERT(1133, 9, store_collect_tests_swept(&id, false, false, &restored),
			   "no cancel fell between two packs' deletion vectors");
	STD_ASSERT(1133, 9, restored, "no cancel fell in a verify that restored the retired pack");
	STD_ASSERT(1133, 9, store_collect_tests_swept(&id, false, true, &restored),
			   "no cancel fell between two packs' deletion vectors");
	STD_ASSERT(1133, 9, store_collect_tests_swept(&id, true, false, &restored),
			   "no allocation failure fell between the keep and the install");
	{
		SimcTestIo	test_io = simc_io();
		HostIoIo   *io = &test_io.io;
		ChunksFileInterruptible interruptible = {&CHUNKS_FILE_IO_INTERRUPTIBLE_OPS, io};
		HostSharedReader reading;
		RepoRepo	repo = store_collect_tests_created(io, ++id, 1134, &reading);
		store_collect_tests_named_t named[STORE_COLLECT_TESTS_PARTLY];
		uint64		numbers[STORE_COLLECT_TESTS_PARTLY];
		uint64		before[STORE_COLLECT_TESTS_PARTLY] = {0};
		StoreCollectCollected collected;
		ChunksPacksCheckChecked checked;
		StdVec		unnamed;

		for (size_t k = 0; k < STORE_COLLECT_TESTS_PARTLY; k++)
			numbers[k] = store_collect_tests_partly(io, &repo, &reading, (uint8) k, &named[k]);
		store_collect_tests_saved_each(io, &repo, &reading, named, STORE_COLLECT_TESTS_PARTLY, 1135, 9);
		store_collect_tests_crashed(io, &repo, &reading, numbers[0], 1136);
		collected = store_collect_tests_collected(io, &repo, &reading, id, 0, NULL, 1137, 74, 83);
		store_collect_Collected_drop(&collected);
		STD_ASSERT(1138, 9,
				   store_collect_tests_covered(io, &repo, numbers, before, 1138) == STORE_COLLECT_TESTS_PARTLY,
				   "a full collection beside a crash's vectors gave a pack it keeps partly dead no vector");
		for (size_t k = 0; k < STORE_COLLECT_TESTS_PARTLY; k++)
			store_collect_tests_reads_eq(io, &repo, &reading, &named[k].address, named[k].at, 1139, 13, 64);
		chunks_packs_check_check(interruptible, id, HOST_PG_DEFAULT, &checked);
		STD_ASSERT(1140, 9, checked.found.len == 0, "the packs' check beside a crash's vectors found something");
		chunks_packs_check_Checked_drop(&checked);
		unnamed = store_collect_tests_unnamed(io, &repo, id, 1141);
		STD_ASSERT(1141, 9, unnamed.len == 0,
				   "a full collection left a crash's vector neither listed nor queued");
		store_collect_tests_paths_drop(&unnamed);
		host_shared_Reader_drop(&reading);
		repo_Repo_drop(&repo);
		simc_TestIo_drop(&test_io);
	}
	{
		SimcTestIo	test_io = simc_io();
		HostIoIo   *io = &test_io.io;
		HostSharedReader reading;
		RepoRepo	repo = store_collect_tests_created(io, ++id, 1142, &reading);
		store_collect_tests_named_t named[STORE_COLLECT_TESTS_PARTLY];
		uint64		numbers[STORE_COLLECT_TESTS_PARTLY];
		uint64		generations[STORE_COLLECT_TESTS_PARTLY];
		ChunksPacksRemovalReservation reservation;
		ChunksPacksRemovalFile file;
		StoreCollectCollected collected;
		ChunksErrorError failed;
		StdArcInner *reader;
		bool		has;

		for (size_t k = 0; k < STORE_COLLECT_TESTS_PARTLY; k++)
			numbers[k] = store_collect_tests_partly(io, &repo, &reading, (uint8) k, &named[k]);
		store_collect_tests_saved_each(io, &repo, &reading, named, STORE_COLLECT_TESTS_PARTLY, 1143, 9);
		store_collect_tests_chunks_ok(chunks_packs_backend_Packs_pack_reader(&repo.packs, io, &reading, numbers[0],
																			 &has, &reader, &failed), &failed, 1144,
									  13);
		STD_ASSERT(1144, 9, has, "a test's pack has no reader");
		file.f0 = repo.packs.own;
		file.f1.tag = CHUNKS_PACKS_SINGLE_QUEUED_VECTOR;
		file.f1.u.vector = STORE_COLLECT_TESTS_QUEUED;
		file.f2 = ((const ChunksPackPackReader *) std_arc_Deref_deref(reader, _Alignof(ChunksPackPackReader)))->id;
		chunks_pack_ArcPackReader_drop(reader);
		store_collect_tests_chunks_ok(chunks_packs_removal_reserve(io, 1, &reservation, &failed), &failed, 1145, 13);
		store_collect_tests_chunks_ok(chunks_packs_removal_queue(io, &reservation, id, &file, 1, &failed), &failed,
									  1145, 13);
		collected = store_collect_tests_collected(io, &repo, &reading, id, 0, NULL, 1146, 74, 83);
		store_collect_Collected_drop(&collected);
		store_collect_tests_generations(io, &repo, numbers, generations, 1147, 13);
		for (size_t k = 0; k < STORE_COLLECT_TESTS_PARTLY; k++)
			STD_ASSERT(1147, 13, generations[k] > STORE_COLLECT_TESTS_QUEUED,
					   "a full collection gave a vector a generation the removal queue holds a name of");
		host_shared_Reader_drop(&reading);
		repo_Repo_drop(&repo);
		simc_TestIo_drop(&test_io);
	}
}

#endif							/* DOLTGRES_TEST_STORE */
