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
 * A storage's spills, and a repository's chunk spills (txn's spill.rs): each
 * storage's manifest file in its repository's spills' directory, listing its
 * live spill files, whose count and generation its state holds and which
 * each backend keeps a copy of, each file's Bloom filter in the file's own
 * pages, read through the node cache, and each process's slot naming the one
 * spill it hasn't listed yet.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/file.h"
#include "doltgres/host/cache.h"
#include "doltgres/host/delta.h"
#include "doltgres/host/local.h"
#include "doltgres/host/lock.h"
#include "doltgres/host/shared.h"
#include "doltgres/repo/lib.h"
#include "doltgres/std/cmp.h"
#include "doltgres/std/num.h"
#include "doltgres/std/path.h"
#include "doltgres/std/slice.h"
#include "doltgres/std/sort.h"
#include "doltgres/std/str.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"
#include "doltgres/txn/base.h"
#include "doltgres/txn/memtable/branch.h"
#include "doltgres/txn/memtable/map.h"
#include "doltgres/txn/memtable/table.h"
#include "doltgres/txn/memtable/tuples.h"
#include "doltgres/txn/memtable/version.h"
#include "doltgres/txn/record.h"
#include "doltgres/txn/runs.h"
#include "doltgres/txn/spill.h"
#include "doltgres/txn/state.h"
#include "doltgres/txn/status.h"
#include "doltgres/txn/written.h"

#define XXH_STATIC_LINKING_ONLY
#include "xxhash.h"

#define RUST_FILE "txn" STD_SEP "src" STD_SEP "spill.rs"

/* txn_spill_internal returns an internal error of the spills. */
static HostError
txn_spill_internal(const char *text)
{
	StdString	message;

	std_string_from_Checked(&message, text, strlen(text));
	return host_Error_fork((const uint8 *) "XX000", &message);
}

/* txn_spill_corrupt returns the error of a storage's spills' manifest that doesn't read. */
static HostError
txn_spill_corrupt(void)
{
	StdString	message;
	const char *text = "a storage's spills' manifest is corrupt";

	std_string_from_Checked(&message, text, strlen(text));
	return host_Error_fork((const uint8 *) "XX001", &message);
}

/*
 * TxnSpillHomeOps is the Home trait: a record whose spills a manifest lists,
 * its manifest's writers' long lock, its own lock, its atomics of the
 * manifest's count, its pending spills, and its generation, and the tree of
 * each Of whose spills it holds, which copied, removed, and chain walk.
 */
struct TxnSpillHomeOps
{
	size_t		writing;
	size_t		short_;
	size_t		listed;
	size_t		pended;
	size_t		changed;
	uint32		(*own) (HostSharedRecord record);
	bool		(*described) (HostSharedRecord record, HostStorage *storage, TxnStatePersistence *persistence);
	bool		(*kind) (HostIoIo *io, const HostSharedHeld *held, TxnWrittenOf of, TxnRunsKind *kind,
						 TxnRunsTail *tail, HostError *error);
	bool		(*tree) (TxnWrittenOf of, const TxnMemtableTableTreeOps **result, HostError *error);
	bool		(*untupled) (HostIoIo *io, HostSharedRecord record, const uint64 *tids, size_t tids_len,
							 HostError *error);

	/*
	 * through returns the point the record's newest base is folded at, which
	 * a map's spill at or below it has passed: the fold that installed the
	 * base applied it or put its versions back.
	 */
	bool		(*through) (HostIoIo *io, HostSharedRecord record, uint64 *result, HostError *error);

	/*
	 * ended_number takes the number of a spill file of an ended transaction
	 * past past, as state's numbered does for a storage's state; a working
	 * state's chunk spills are only their running transactions'.
	 */
	uint32		(*ended_number) (HostSharedRecord record, uint32 past);
};

/* TXN_SPILL_IDENTITY is the error of a spill of a database's identity tree, which never spills. */
#define TXN_SPILL_IDENTITY "a database's identity tree never spills"

/* txn_spill_identity returns the error of a spill of a database's identity tree. */
static HostError
txn_spill_identity(void)
{
	StdString	message;

	std_string_from_Checked(&message, TXN_SPILL_IDENTITY, strlen(TXN_SPILL_IDENTITY));
	return host_Error_fork((const uint8 *) "XX000", &message);
}

/* txn_spill_State_Home_described is State's described: the state's storage and persistence, or nothing for a state not filled. */
static bool
txn_spill_State_Home_described(HostSharedRecord record, HostStorage *storage, TxnStatePersistence *persistence)
{
	return txn_state_described(record, storage, persistence);
}

/* txn_spill_State_Home_kind is State's kind: an index's storage's rows tree spills as entries, naming its table and index as its run does. */
static bool
txn_spill_State_Home_kind(HostIoIo *io, const HostSharedHeld *held, TxnWrittenOf of, TxnRunsKind *kind,
						  TxnRunsTail *tail, HostError *error)
{
	bool		some;
	TxnBaseBase base;
	bool		indexed;
	uint32		table = 0;
	uint32		index = 0;

	if (!txn_state_base_record(io, held, &some, &base, error))
		return false;
	indexed = some && base.has_index;
	if (indexed)
	{
		table = base.table;
		index = base.index;
	}
	if (some)
		txn_base_Base_drop(&base);
	tail->tag = TXN_RUNS_TAIL_NONE;
	switch (of)
	{
		case TXN_WRITTEN_OF_ROWS:
			*kind = TXN_RUNS_KIND_SPILLED_ROWS;
			if (indexed)
			{
				*kind = TXN_RUNS_KIND_SPILLED_ENTRIES;
				tail->tag = TXN_RUNS_TAIL_INDEX;
				tail->table = table;
				tail->index = index;
			}
			return true;
		case TXN_WRITTEN_OF_VALUES:
			*kind = TXN_RUNS_KIND_SPILLED_VALUES;
			return true;
		case TXN_WRITTEN_OF_FREE:
			*kind = TXN_RUNS_KIND_SPILLED_FREE;
			return true;
		case TXN_WRITTEN_OF_IDENTITY:
			*error = txn_spill_identity();
			return false;
		case TXN_WRITTEN_OF_CHUNKS:
			break;
	}
	*error = txn_spill_internal("a storage's state has no chunk tree");
	return false;
}

/* txn_spill_State_tree is the tree of a state's that copied, removed, and chain walk for an Of: its rows, its value map, or its free space map. */
static bool
txn_spill_State_tree(TxnWrittenOf of, const TxnMemtableTableTreeOps **result, HostError *error)
{
	switch (of)
	{
		case TXN_WRITTEN_OF_ROWS:
			*result = &TXN_MEMTABLE_TABLE_ROWS_TREE_OPS;
			return true;
		case TXN_WRITTEN_OF_VALUES:
			*result = &TXN_MEMTABLE_MAP_VALUES_TREE_OPS;
			return true;
		case TXN_WRITTEN_OF_FREE:
			*result = &TXN_MEMTABLE_MAP_FREE_TREE_OPS;
			return true;
		case TXN_WRITTEN_OF_IDENTITY:
			*error = txn_spill_identity();
			return false;
		case TXN_WRITTEN_OF_CHUNKS:
			break;
	}
	*error = txn_spill_internal("a storage's state has no chunk tree");
	return false;
}

/* txn_spill_State_Home_untupled is State's untupled: it drops a spill's rows' tuple ids from the state's tuple id table. */
static bool
txn_spill_State_Home_untupled(HostIoIo *io, HostSharedRecord record, const uint64 *tids, size_t tids_len, HostError *error)
{
	return txn_memtable_tuples_dropped(io, record, tids, tids_len, error);
}

/* txn_spill_State_Home_through is State's through: its newest base's read point. */
static bool
txn_spill_State_Home_through(HostIoIo *io, HostSharedRecord record, uint64 *result, HostError *error)
{
	return txn_state_read_point(io, record, result, error);
}

const TxnSpillHomeOps TXN_SPILL_STATE_HOME_OPS = {
	.writing = TXN_STATE_SPILLS_WRITING_INDEX,
	.short_ = TXN_STATE_STATE_LOCK_INDEX,
	.listed = TXN_STATE_SPILLS_INDEX,
	.pended = TXN_STATE_SPILLS_PENDING_INDEX,
	.changed = TXN_STATE_SPILLS_CHANGED_INDEX,
	.own = txn_state_own,
	.described = txn_spill_State_Home_described,
	.kind = txn_spill_State_Home_kind,
	.tree = txn_spill_State_tree,
	.untupled = txn_spill_State_Home_untupled,
	.through = txn_spill_State_Home_through,
	.ended_number = txn_state_numbered,
};

/* txn_spill_BranchDeltas_Home_described is BranchDeltas' described: a working state's chunk spills by its repository's chunk storage, logged. */
static bool
txn_spill_BranchDeltas_Home_described(HostSharedRecord record, HostStorage *storage, TxnStatePersistence *persistence)
{
	uint32		repository;

	if (!txn_memtable_branch_repository(record, &repository))
		return false;
	*storage = txn_spill_chunks_storage(repository);
	*persistence = TXN_STATE_PERSISTENCE_PERMANENT;
	return true;
}

/* txn_spill_BranchDeltas_tree is the tree of a working state's delta that spills: its chunk tree alone. */
static bool
txn_spill_BranchDeltas_tree(TxnWrittenOf of, const TxnMemtableTableTreeOps **result, HostError *error)
{
	if (of == TXN_WRITTEN_OF_CHUNKS)
	{
		*result = &TXN_MEMTABLE_BRANCH_CHUNKS_TREE_OPS;
		return true;
	}
	*error = txn_spill_internal("a working state's delta spills only its chunk tree");
	return false;
}

/* txn_spill_BranchDeltas_Home_kind is BranchDeltas' kind: a chunk spill, whose header holds no tail. */
static bool
txn_spill_BranchDeltas_Home_kind(HostIoIo *io, const HostSharedHeld *held, TxnWrittenOf of, TxnRunsKind *kind,
								 TxnRunsTail *tail, HostError *error)
{
	const TxnMemtableTableTreeOps *tree;

	if (!txn_spill_BranchDeltas_tree(of, &tree, error))
		return false;
	*kind = TXN_RUNS_KIND_SPILLED_CHUNKS;
	tail->tag = TXN_RUNS_TAIL_NONE;
	return true;
}

/* txn_spill_BranchDeltas_Home_untupled is BranchDeltas' untupled: a working state's delta has no tuple id table. */
static bool
txn_spill_BranchDeltas_Home_untupled(HostIoIo *io, HostSharedRecord record, const uint64 *tids, size_t tids_len,
									 HostError *error)
{
	return true;
}

/* txn_spill_BranchDeltas_Home_through is BranchDeltas' through: 0, since a working state's chunk spills hold no map's versions. */
static bool
txn_spill_BranchDeltas_Home_through(HostIoIo *io, HostSharedRecord record, uint64 *result, HostError *error)
{
	*result = 0;
	return true;
}

/* txn_spill_BranchDeltas_Home_ended_number is BranchDeltas' ended_number: past itself. */
static uint32
txn_spill_BranchDeltas_Home_ended_number(HostSharedRecord record, uint32 past)
{
	return past;
}

const TxnSpillHomeOps TXN_SPILL_BRANCH_DELTAS_HOME_OPS = {
	.writing = TXN_MEMTABLE_BRANCH_CHUNK_SPILLS_WRITING_INDEX,
	.short_ = TXN_MEMTABLE_BRANCH_DELTAS_LOCK_INDEX,
	.listed = TXN_MEMTABLE_BRANCH_CHUNK_SPILLS_INDEX,
	.pended = TXN_MEMTABLE_BRANCH_CHUNK_SPILLS_PENDING_INDEX,
	.changed = TXN_MEMTABLE_BRANCH_CHUNK_SPILLS_CHANGED_INDEX,
	.own = txn_memtable_branch_own,
	.described = txn_spill_BranchDeltas_Home_described,
	.kind = txn_spill_BranchDeltas_Home_kind,
	.tree = txn_spill_BranchDeltas_tree,
	.untupled = txn_spill_BranchDeltas_Home_untupled,
	.through = txn_spill_BranchDeltas_Home_through,
	.ended_number = txn_spill_BranchDeltas_Home_ended_number,
};

/* txn_spill_Home_copied hands every key of one of a home's trees that has a version to a walk's step, with no killed version. */
static bool
txn_spill_Home_copied(const TxnSpillHomeOps *home, HostIoIo *io, HostSharedRecord record, TxnWrittenOf of,
					  bool (*each) (void *captures, HostIoIo *io, const uint8 *key, size_t key_len, const StdVec *records,
									size_t records_len, const txn_TupleHandleU64Bool_t *handles, size_t handles_len,
									HostError *error),
					  void *captures, HostError *error)
{
	const TxnMemtableTableTreeOps *tree;

	return home->tree(of, &tree, error) && txn_memtable_table_copied(tree, io, record, false, each, captures, error);
}

/* txn_spill_Home_removed takes the versions a spill took out of each key's chain in one of a home's trees. */
static bool
txn_spill_Home_removed(const TxnSpillHomeOps *home, HostIoIo *io, HostSharedRecord record, TxnWrittenOf of,
					   const TxnMemtableTableRemoving *keys, size_t keys_len, HostError *error)
{
	const TxnMemtableTableTreeOps *tree;
	StdVec		taken;

	if (!home->tree(of, &tree, error) || !txn_memtable_table_removed_all(tree, io, record, keys, keys_len, &taken,
																		 error))
		return false;
	for (size_t k = 0; k < taken.len; k++)
		std_vec_free_Checked(&STD_VEC_AT(&taken, StdVec, k), 1, 1);
	std_vec_free_Checked(&taken, sizeof(StdVec), _Alignof(StdVec));
	return true;
}

/* txn_spill_Home_chain lends a key's versions in one of a home's trees, newest first, for a caller's hold. */
static bool
txn_spill_Home_chain(const TxnSpillHomeOps *home, HostIoIo *io, const HostDeltaHold *hold, HostSharedRecord record,
					 TxnWrittenOf of, const uint8 *key, size_t key_len, StdVec *result, HostError *error)
{
	const TxnMemtableTableTreeOps *tree;

	return home->tree(of, &tree, error) && txn_memtable_table_chain(tree, io, hold, record, key, key_len, result,
																	error);
}

/* txn_spill_chunks_storage returns the storage a repository's chunk spills are named by: its database with no tablespace and no relation. */
HostStorage
txn_spill_chunks_storage(uint32 repository)
{
	HostStorage storage = {.database = repository};

	return storage;
}

/* txn_spill_chunks_repository returns the repository whose chunk spills a storage names, or nothing for a storage's own. */
bool
txn_spill_chunks_repository(const HostStorage *storage, uint32 *result)
{
	if (storage->tablespace != 0 || storage->relation != 0 || storage->database == 0)
		return false;
	*result = storage->database;
	return true;
}

/* TxnSpillFoundTag is Found's variants. */
typedef enum TxnSpillFoundTag
{
	TXN_SPILL_FOUND_STATE,
	TXN_SPILL_FOUND_CHUNKS,
} TxnSpillFoundTag;

/* TxnSpillFound is Found: the home of a storage's spills, its state, or its repository's working state's delta. */
typedef struct TxnSpillFound
{
	TxnSpillFoundTag tag;
	HostSharedHeld held;
} TxnSpillFound;

/* txn_spill_Found_home returns the Home of a found home's record. */
static const TxnSpillHomeOps *
txn_spill_Found_home(const TxnSpillFound *found)
{
	return found->tag == TXN_SPILL_FOUND_STATE ? &TXN_SPILL_STATE_HOME_OPS : &TXN_SPILL_BRANCH_DELTAS_HOME_OPS;
}

/* txn_spill_found returns the home of a storage's spills, or nothing when it is gone, its spills with it. */
static bool
txn_spill_found(HostIoIo *io, const HostStorage *storage, bool *some, TxnSpillFound *result, HostError *error)
{
	uint32		repository;

	if (txn_spill_chunks_repository(storage, &repository))
	{
		result->tag = TXN_SPILL_FOUND_CHUNKS;
		return txn_memtable_branch_find(HOST_SHARED_IO_WITHIN(io), repository, some, &result->held, error);
	}
	result->tag = TXN_SPILL_FOUND_STATE;
	return txn_state_found(io, storage, some, &result->held, error);
}

/* txn_spill_at_found_frame_0_t is at_found's state across its pad: the home found, while live. */
typedef struct txn_spill_at_found_frame_0_t
{
	HostIoIo   *io;
	const HostStorage *storage;
	bool		(*step) (void *captures, const TxnSpillHomeOps *home, const HostSharedHeld *held, HostError *error);
	void	   *captures;
	bool	   *some;
	HostError  *error;
	bool		ok;
	TxnSpillFound found;
	bool		found_live;
} txn_spill_at_found_frame_0_t;

/* txn_spill_at_found_pad_0 is at_found's pad: the home found, while live. */
static void
txn_spill_at_found_pad_0(void *frame_)
{
	txn_spill_at_found_frame_0_t *frame = frame_;

	if (frame->found_live)
	{
		frame->found_live = false;
		host_shared_Held_drop(&frame->found.held);
	}
}

/* txn_spill_at_found_body_0 is at_found's body. */
static void
txn_spill_at_found_body_0(void *frame_)
{
	txn_spill_at_found_frame_0_t *frame = frame_;

	if (!txn_spill_found(frame->io, frame->storage, frame->some, &frame->found, frame->error))
		return;
	if (!*frame->some)
	{
		frame->ok = true;
		return;
	}
	frame->found_live = true;
	frame->ok = frame->step(frame->captures, txn_spill_Found_home(&frame->found), &frame->found.held, frame->error);
	txn_spill_at_found_pad_0(frame);
}

/* txn_spill_at_found runs a step on the home of a storage's spills, held while it runs, answering in some whether it found one. */
static bool
txn_spill_at_found(HostIoIo *io, const HostStorage *storage,
				   bool (*step) (void *captures, const TxnSpillHomeOps *home, const HostSharedHeld *held,
								 HostError *error),
				   void *captures, bool *some, HostError *error)
{
	txn_spill_at_found_frame_0_t frame = {.io = io, .storage = storage, .step = step, .captures = captures,
	.some = some, .error = error};

	std_landing(&frame, txn_spill_at_found_body_0, txn_spill_at_found_pad_0);
	return frame.ok;
}

/* TXN_SPILL_BITS_PER_KEY is BITS_PER_KEY, the filter's bits for each key it holds. */
#define TXN_SPILL_BITS_PER_KEY 10

/* TXN_SPILL_HASHES is HASHES, the count of bits a key sets. */
#define TXN_SPILL_HASHES 7

/* TXN_SPILL_PAGE_BITS is PAGE_BITS, the bits of a full page of a spill file's filter. */
#define TXN_SPILL_PAGE_BITS ((uint64) TXN_RUNS_FILTER_WORDS * 64)

/* txn_spill_positions writes the bits of a filter of a count of bits that a key sets: the low half of its 128-bit xxh3 stepped by its high half, made odd. */
static void
txn_spill_positions(const uint8 *key, size_t key_len, uint64 bits, uint64 result[TXN_SPILL_HASHES])
{
	XXH128_hash_t hash = XXH3_128bits(key, key_len);
	uint64		first = hash.low64;
	uint64		step = hash.high64 | 1;

	for (uint64 at = 0; at < TXN_SPILL_HASHES; at++)
		result[at] = STD_REM(u64, std_wrapping_add_u64(first, std_wrapping_mul_u64(at, step)), bits,
							 312, 31);
}

/* txn_spill_Filter_bits returns the count of the filter's bits. */
static uint64
txn_spill_Filter_bits(const TxnSpillFilter *self)
{
	return STD_MUL(u64, (uint64) self->words.len, 64, 298, 9);
}

/* txn_spill_Filter_add sets a key's bits. */
static void
txn_spill_Filter_add(TxnSpillFilter *self, const uint8 *key, size_t key_len)
{
	uint64		bits[TXN_SPILL_HASHES];

	txn_spill_positions(key, key_len, txn_spill_Filter_bits(self), bits);
	for (size_t k = 0; k < TXN_SPILL_HASHES; k++)
	{
		size_t		word = (size_t) (bits[k] / 64);

		std_index_check(word, self->words.len, RUST_FILE, 292, 13);
		STD_VEC_AT(&self->words, uint64, word) |= (uint64) 1 << (bits[k] % 64);
	}
}

/* txn_spill_Filter_with returns an empty filter of ten bits for each of a count of keys, at least one word. */
static bool
txn_spill_Filter_with(uint64 count, TxnSpillFilter *result, HostError *error)
{
	uint64		bits = STD_MUL(u64, Max(count, 1), TXN_SPILL_BITS_PER_KEY, 282, 22);
	size_t		words = (size_t) (bits / 64 + (bits % 64 != 0));

	std_vec_new(&result->words, sizeof(uint64), _Alignof(uint64));
	if (!std_vec_try_reserve_exact(&result->words, words, sizeof(uint64), _Alignof(uint64)))
	{
		*error = host_Error_out_of_memory();
		return false;
	}
	memset(result->words.ptr, 0, words * sizeof(uint64));
	result->words.len = words;
	return true;
}

/* txn_spill_Filter_of builds the filter of a spill's distinct keys: ten bits for each, at least one word. */
bool
txn_spill_Filter_of(size_t count, const StdBytes *keys, size_t keys_len, TxnSpillFilter *result, HostError *error)
{
	if (!txn_spill_Filter_with((uint64) count, result, error))
		return false;
	for (size_t k = 0; k < keys_len; k++)
		txn_spill_Filter_add(result, keys[k].ptr, keys[k].len);
	return true;
}

/* txn_spill_Filter_hits reports whether a key may be among the filter's keys. */
bool
txn_spill_Filter_hits(const TxnSpillFilter *self, const uint8 *key, size_t key_len)
{
	uint64		bits[TXN_SPILL_HASHES];

	txn_spill_positions(key, key_len, txn_spill_Filter_bits(self), bits);
	for (size_t k = 0; k < TXN_SPILL_HASHES; k++)
	{
		size_t		word = (size_t) (bits[k] / 64);

		std_index_check(word, self->words.len, RUST_FILE, 303, 47);
		if ((STD_VEC_AT(&self->words, uint64, word) & (uint64) 1 << (bits[k] % 64)) == 0)
			return false;
	}
	return true;
}

/* txn_spill_File_numbered returns the word that holds a file's kind and number. */
static uint64
txn_spill_File_numbered(const TxnSpillFile *self)
{
	return (uint64) self->kind << 32 | self->number;
}

/* txn_spill_File_PartialEq_eq is File's PartialEq: every field equal. */
static bool
txn_spill_File_PartialEq_eq(const TxnSpillFile *self, const TxnSpillFile *other)
{
	return self->xid == other->xid && self->kind == other->kind && self->number == other->number &&
		self->len == other->len && self->hash == other->hash && self->bits == other->bits &&
		self->passed == other->passed && self->from == other->from;
}

/* txn_spill_File_line appends a file's line in its storage's manifest to a buffer: its xid, kind, number, length, xxh3, bits, abort's end, and residual's point. */
static void
txn_spill_File_line(const TxnSpillFile *self, StdString *lines)
{
	char		line[192];
	int			len = snprintf(line, sizeof(line), UINT64_FORMAT " %u %u " UINT64_FORMAT " " UINT64_FORMAT " "
							   UINT64_FORMAT " " UINT64_FORMAT " " UINT64_FORMAT "\n", self->xid, (unsigned) self->kind,
							   self->number, self->len, self->hash, self->bits, self->passed, self->from);

	std_vec_extend_from_slice_Checked(lines, line, (size_t) len, 1, 1);
}

/* txn_spill_File_parsed returns the file a manifest's line names, or nothing for a line that names none. */
static bool
txn_spill_File_parsed(const char *line, size_t len, TxnSpillFile *result)
{
	uint64		words[8];
	size_t		at = 0;
	StdParseIntError failed;

	for (size_t count = 0; count < lengthof(words); count++)
	{
		size_t		end = at;

		if (at > len)
			return false;
		while (end < len && line[end] != ' ')
			end++;
		if (!std_str_parse_u64(line + at, end - at, &words[count], &failed))
			return false;
		at = end + 1;
	}
	if (at != len + 1 || words[1] > PG_UINT8_MAX || words[2] > PG_UINT32_MAX)
		return false;
	if (!txn_runs_Kind_of((uint8) words[1], &result->kind) || !txn_runs_Kind_spilled(result->kind))
		return false;
	result->xid = words[0];
	result->number = (uint32) words[2];
	result->len = words[3];
	result->hash = words[4];
	result->bits = words[5];
	result->passed = words[6];
	result->from = words[7];
	return result->bits > 0 && result->bits % 64 == 0;
}

/* txn_spill_replaced_by reports whether a merge a slot names replaces a file: one of its transaction and tree, numbered from its lowest source below its own. */
static bool
txn_spill_replaced_by(const txn_TupleU64KindU32OptionU32_t *slotted, size_t slotted_len, const TxnSpillFile *file)
{
	for (size_t k = 0; k < slotted_len; k++)
	{
		const txn_TupleU64KindU32OptionU32_t *merge = &slotted[k];

		if (merge->has_f3 && file->xid == merge->f0 && file->number >= merge->f3 && file->number < merge->f2 &&
			(file->kind == merge->f1 ||
			 (file->kind == TXN_RUNS_KIND_SPILLED_TUPLES && merge->f1 == TXN_RUNS_KIND_SPILLED_ROWS)))
			return true;
	}
	return false;
}

/* txn_spill_manifest returns the path of a storage's spills' manifest file. */
static void
txn_spill_manifest(const TxnSpillHomeOps *home, HostSharedRecord record, const HostStorage *storage, StdPathBuf *result)
{
	char		name[64];
	int			len;
	uint32		repository;
	StdPathBuf	dir;

	if (txn_spill_chunks_repository(storage, &repository))
		len = snprintf(name, sizeof(name), "%schunks", TXN_SPILL_MANIFEST);
	else
		len = snprintf(name, sizeof(name), "%s%u-%u", TXN_SPILL_MANIFEST, storage->tablespace, storage->relation);
	txn_runs_spill_directory(storage->database, home->own(record), &dir);
	std_path_join(dir.bytes.ptr, dir.bytes.len, name, (size_t) len, result);
	std_vec_free_Checked(&dir.bytes, 1, 1);
}

/* txn_spill_read_frame_0_t is read's state across its pad: the manifest file opened, its bytes, and the files, while live. */
typedef struct txn_spill_read_frame_0_t
{
	HostIoReadDoors io;
	HostSharedPauses pauses;
	const StdPathBuf *path;
	StdVec	   *result;
	HostError  *error;
	bool		ok;
	HostFile	opened;
	bool		opened_live;
	StdVec		bytes;
	bool		bytes_live;
	bool		result_live;
} txn_spill_read_frame_0_t;

/* txn_spill_read_pad_0 is read's pad: the files, the bytes, and the file, while live. */
static void
txn_spill_read_pad_0(void *frame_)
{
	txn_spill_read_frame_0_t *frame = frame_;

	if (frame->result_live)
	{
		frame->result_live = false;
		std_vec_free_Checked(frame->result, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	}
	if (frame->bytes_live)
	{
		frame->bytes_live = false;
		std_vec_free_Checked(&frame->bytes, 1, 1);
	}
	if (frame->opened_live)
	{
		frame->opened_live = false;
		host_File_drop(&frame->opened);
	}
}

/* txn_spill_read_lines parses the manifest's bytes into the frame's files, refusing a line that names no spill's file. */
static bool
txn_spill_read_lines(txn_spill_read_frame_0_t *frame)
{
	const char *text = (const char *) frame->bytes.ptr;
	size_t		len = frame->bytes.len;
	size_t		lines = 0;
	StdUtf8Error invalid;

	if (!std_str_from_utf8(frame->bytes.ptr, len, &invalid))
	{
		*frame->error = txn_spill_corrupt();
		return false;
	}
	if (len == 0 || text[len - 1] != '\n')
		return true;
	len--;
	for (size_t k = 0; k <= len; k++)
		if (k == len || text[k] == '\n')
			lines++;
	if (!std_vec_try_reserve(frame->result, lines, sizeof(TxnSpillFile), _Alignof(TxnSpillFile)))
	{
		*frame->error = host_Error_out_of_memory();
		return false;
	}
	for (size_t at = 0, k = 0; k <= len; k++)
		if (k == len || text[k] == '\n')
		{
			TxnSpillFile file;

			if (!txn_spill_File_parsed(text +at, k - at, &file))
			{
				*frame->error = txn_spill_corrupt();
				return false;
			}
			std_vec_push_Checked(frame->result, &file, sizeof(file), _Alignof(TxnSpillFile));
			at = k + 1;
		}
	return true;
}

/* txn_spill_read_body_0 is read's body. */
static void
txn_spill_read_body_0(void *frame_)
{
	txn_spill_read_frame_0_t *frame = frame_;
	bool		some;
	uint64		size;
	ChunksErrorError failed;

	std_vec_new(frame->result, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	frame->result_live = true;
	if (!host_open_if_there(frame->io, frame->path->bytes.ptr, frame->path->bytes.len, &some, &frame->opened,
							frame->error))
	{
		txn_spill_read_pad_0(frame);
		return;
	}
	if (!some)
	{
		frame->result_live = false;
		frame->ok = true;
		return;
	}
	frame->opened_live = true;
	if (!host_pause(frame->pauses, "doltgres-spill-manifest-opened", frame->error) ||
		!host_size(frame->io, &frame->opened, &size, frame->error))
	{
		txn_spill_read_pad_0(frame);
		return;
	}
	if (size > SIZE_MAX)
	{
		*frame->error = txn_spill_corrupt();
		txn_spill_read_pad_0(frame);
		return;
	}
	if (!chunks_file_read_at(frame->io, &frame->opened, 0, (size_t) size, &frame->bytes, &failed))
	{
		*frame->error = txn_base_storage(&failed);
		txn_spill_read_pad_0(frame);
		return;
	}
	frame->bytes_live = true;
	if (!txn_spill_read_lines(frame))
	{
		txn_spill_read_pad_0(frame);
		return;
	}
	frame->result_live = false;
	frame->ok = true;
	txn_spill_read_pad_0(frame);
}

/*
 * txn_spill_read returns the files a storage's manifest file lists, the
 * oldest listed first, or none when it has no file, refusing as data
 * corruption a line that names no spill's file. A reader holds the file open
 * for this one read.
 */
static bool
txn_spill_read(HostIoReadDoors io, HostSharedPauses pauses, const StdPathBuf *path, StdVec *result, HostError *error)
{
	txn_spill_read_frame_0_t frame = {.io = io, .pauses = pauses, .path = path, .result = result, .error = error};

	std_landing(&frame, txn_spill_read_body_0, txn_spill_read_pad_0);
	return frame.ok;
}

/* txn_spill_rewrote_frame_0_t is rewrote's state across its pad: the manifest's lines, their write aside, and the record's lock, while live. */
typedef struct txn_spill_rewrote_frame_0_t
{
	const TxnSpillHomeOps *home;
	HostSharedLongIo *io;
	HostSharedRecord record;
	const StdPathBuf *path;
	const StdVec *files;
	HostError  *error;
	bool		ok;
	StdString	lines;
	bool		lines_live;
	StdPathBuf	aside;
	bool		aside_live;
	HostFile	written;
	bool		written_live;
	HostSharedGuard exclusive;
	bool		exclusive_live;
} txn_spill_rewrote_frame_0_t;

/* txn_spill_rewrote_pad_0 is rewrote's pad: what it holds, while live. */
static void
txn_spill_rewrote_pad_0(void *frame_)
{
	txn_spill_rewrote_frame_0_t *frame = frame_;

	if (frame->exclusive_live)
	{
		frame->exclusive_live = false;
		host_shared_Guard_drop(&frame->exclusive);
	}
	if (frame->written_live)
	{
		frame->written_live = false;
		host_File_drop(&frame->written);
	}
	if (frame->aside_live)
	{
		frame->aside_live = false;
		std_vec_free_Checked(&frame->aside.bytes, 1, 1);
	}
	if (frame->lines_live)
	{
		frame->lines_live = false;
		std_vec_free_Checked(&frame->lines, 1, 1);
	}
}

/* txn_spill_rewrote_written writes the frame's files' lines whole beside the manifest and renames them over it. */
static bool
txn_spill_rewrote_written(txn_spill_rewrote_frame_0_t *frame)
{
	HostIoIoDoors io = HOST_SHARED_LONG_IO_IO_DOORS(frame->io);
	ChunksErrorError failed;
	bool		some;

	std_vec_new(&frame->lines, 1, 1);
	frame->lines_live = true;
	for (size_t k = 0; k < frame->files->len; k++)
		txn_spill_File_line(&STD_VEC_AT(frame->files, TxnSpillFile, k), &frame->lines);
	std_path_with_extension(frame->path->bytes.ptr, frame->path->bytes.len, "new", 3, &frame->aside);
	frame->aside_live = true;
	if (!host_create_if_absent(io, frame->aside.bytes.ptr, frame->aside.bytes.len, &some, &frame->written,
							   frame->error))
		return false;
	if (!some)
	{
		if (!host_open(io, frame->aside.bytes.ptr, frame->aside.bytes.len, HOST_OPEN_READ_WRITE, &frame->written,
					   frame->error))
			return false;
		frame->written_live = true;
		if (!host_truncate(io, &frame->written, 0, frame->error))
			return false;
	}
	frame->written_live = true;
	if (!chunks_file_write_at(io, &frame->written, frame->aside.bytes.ptr, frame->aside.bytes.len, 0,
							  frame->lines.ptr, frame->lines.len, &failed))
	{
		*frame->error = txn_base_storage(&failed);
		return false;
	}
	frame->written_live = false;
	host_File_drop(&frame->written);
	if (!chunks_file_rename(io, frame->aside.bytes.ptr, frame->aside.bytes.len, frame->path->bytes.ptr,
							frame->path->bytes.len, &failed))
	{
		*frame->error = txn_base_storage(&failed);
		return false;
	}
	return true;
}

/* txn_spill_rewrote_body_0 is rewrote's body. */
static void
txn_spill_rewrote_body_0(void *frame_)
{
	txn_spill_rewrote_frame_0_t *frame = frame_;
	uint64		generation;
	HostSharedOp load = {.tag = HOST_SHARED_OP_LOAD};
	HostSharedOp exchange = {.tag = HOST_SHARED_OP_EXCHANGE};

	if (frame->files->len == 0)
	{
		ChunksErrorError failed;
		bool		removed;

		if (!chunks_file_remove_if_there(HOST_SHARED_LONG_IO_IO_DOORS(frame->io), frame->path->bytes.ptr,
										 frame->path->bytes.len, &removed, &failed))
		{
			*frame->error = txn_base_storage(&failed);
			txn_spill_rewrote_pad_0(frame);
			return;
		}
	}
	else if (!txn_spill_rewrote_written(frame))
	{
		txn_spill_rewrote_pad_0(frame);
		return;
	}
	if (!txn_state_spills_generation(&generation, frame->error) ||
		!host_shared_LongIo_lock(frame->io, frame->home->short_, &frame->exclusive, frame->error))
	{
		txn_spill_rewrote_pad_0(frame);
		return;
	}
	frame->exclusive_live = true;
	exchange.u.exchange.f0 = host_shared_Record_atomic(frame->record, frame->home->listed, load);
	exchange.u.exchange.f1 = (uint64) frame->files->len;
	host_shared_Record_atomic(frame->record, frame->home->listed, exchange);
	exchange.u.exchange.f0 = host_shared_Record_atomic(frame->record, frame->home->changed, load);
	exchange.u.exchange.f1 = generation;
	host_shared_Record_atomic(frame->record, frame->home->changed, exchange);
	frame->ok = true;
	txn_spill_rewrote_pad_0(frame);
}

/*
 * txn_spill_rewrote writes a storage's manifest file under its writers' lock,
 * its files' lines through a file set aside and renamed over it, unsynced, or
 * removes it when it lists none, and then, under the record's lock held
 * exclusively for the in-memory steps alone, moves the count and the
 * generation its record holds.
 */
static bool
txn_spill_rewrote(const TxnSpillHomeOps *home, HostSharedLongIo *io, HostSharedRecord record, const StdPathBuf *path,
				  const StdVec *files, HostError *error)
{
	txn_spill_rewrote_frame_0_t frame = {.home = home, .io = io, .record = record, .path = path, .files = files,
	.error = error};

	std_landing(&frame, txn_spill_rewrote_body_0, txn_spill_rewrote_pad_0);
	return frame.ok;
}

/* txn_spill_changed_frame_0_t is changed's state across its pad: the manifest's path, its writers' lock, and its files, while live. */
typedef struct txn_spill_changed_frame_0_t
{
	const TxnSpillHomeOps *home;
	HostIoIo   *io;
	HostSharedRecord record;
	const HostStorage *storage;
	bool		(*edit) (void *captures, HostSharedLongIo *io, StdVec *files, HostError *error);
	void	   *captures;
	HostError  *error;
	bool		ok;
	StdPathBuf	path;
	bool		path_live;
	HostSharedLongGuard writing;
	bool		writing_live;
	HostSharedLongIo long_io;
	/* files is a Vec<File>. */
	StdVec		files;
	bool		files_live;
} txn_spill_changed_frame_0_t;

/* txn_spill_changed_pad_0 is changed's pad: what it holds, while live. */
static void
txn_spill_changed_pad_0(void *frame_)
{
	txn_spill_changed_frame_0_t *frame = frame_;

	if (frame->files_live)
	{
		frame->files_live = false;
		std_vec_free_Checked(&frame->files, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	}
	if (frame->writing_live)
	{
		frame->writing_live = false;
		host_shared_LongGuard_drop(&frame->writing);
	}
	if (frame->path_live)
	{
		frame->path_live = false;
		std_vec_free_Checked(&frame->path.bytes, 1, 1);
	}
}

/* txn_spill_changed_body_0 is changed's body. */
static void
txn_spill_changed_body_0(void *frame_)
{
	txn_spill_changed_frame_0_t *frame = frame_;
	HostIoIoDoors io;

	txn_spill_manifest(frame->home, frame->record, frame->storage, &frame->path);
	frame->path_live = true;
	if (!host_shared_Record_lock_long(frame->record, HOST_IO_IO_TAKES_SHORT(frame->io), frame->home->writing,
									  &frame->writing, frame->error))
	{
		txn_spill_changed_pad_0(frame);
		return;
	}
	frame->writing_live = true;
	frame->long_io = host_shared_LongGuard_io(&frame->writing);
	io = HOST_SHARED_LONG_IO_IO_DOORS(&frame->long_io);
	if (!txn_spill_read(HOST_IO_IO_DOORS_READ_DOORS(io), HOST_SHARED_LONG_IO_PAUSES(&frame->long_io), &frame->path,
						&frame->files, frame->error))
	{
		txn_spill_changed_pad_0(frame);
		return;
	}
	frame->files_live = true;
	if (!host_pause(HOST_SHARED_LONG_IO_PAUSES(&frame->long_io), "doltgres-spill-manifest-read", frame->error) ||
		!frame->edit(frame->captures, &frame->long_io, &frame->files, frame->error) ||
		!txn_spill_rewrote(frame->home, &frame->long_io, frame->record, &frame->path, &frame->files, frame->error))
	{
		txn_spill_changed_pad_0(frame);
		return;
	}
	frame->ok = true;
	txn_spill_changed_pad_0(frame);
}

/*
 * txn_spill_changed changes a storage's spills' manifest under its writers'
 * lock: it reads the manifest file, edits its list with an edit that may
 * write files under the lock and fail, which leaves the manifest as it was,
 * and rewrites the manifest with the edited list (rewrote). What the edit
 * answers it leaves in its captures.
 */
static bool
txn_spill_changed(const TxnSpillHomeOps *home, HostIoIo *io, HostSharedRecord record, const HostStorage *storage,
				  bool (*edit) (void *captures, HostSharedLongIo *io, StdVec *files, HostError *error),
				  void *captures, HostError *error)
{
	txn_spill_changed_frame_0_t frame = {.home = home, .io = io, .record = record, .storage = storage, .edit = edit,
	.captures = captures, .error = error};

	std_landing(&frame, txn_spill_changed_body_0, txn_spill_changed_pad_0);
	return frame.ok;
}

/* txn_spill_linked_captures_t is linked's edit's captures: the files it lists and the merge they replace files for, if any. */
typedef struct txn_spill_linked_captures_t
{
	const TxnSpillFile *linking;
	size_t		linking_len;
	bool		has_replacing;
	txn_TupleU64KindU32OptionU32_t replacing;
} txn_spill_linked_captures_t;

/*
 * txn_spill_linked_closure_0 is linked's edit: the files the merge replaces
 * out, then each linked file once, at the place of the newest file the
 * merge replaces, or last when it replaces none.
 */
static bool
txn_spill_linked_closure_0(void *captures, HostSharedLongIo *io, StdVec *files, HostError *error)
{
	txn_spill_linked_captures_t *c = captures;
	size_t		kept = 0;
	size_t		at = files->len;

	for (size_t k = 0; k < files->len; k++)
	{
		const TxnSpillFile *file = &STD_VEC_AT(files, TxnSpillFile, k);

		if (!c->has_replacing || !txn_spill_replaced_by(&c->replacing, 1, file))
			STD_VEC_AT(files, TxnSpillFile, kept++) = *file;
		else
			at = kept;
	}
	files->len = kept;
	for (size_t j = 0; j < c->linking_len; j++)
	{
		bool		listed = false;

		for (size_t k = 0; k < files->len && !listed; k++)
			listed = txn_spill_File_PartialEq_eq(&STD_VEC_AT(files, TxnSpillFile, k), &c->linking[j]);
		if (!listed)
		{
			std_vec_insert(files, at, &c->linking[j], sizeof(TxnSpillFile), _Alignof(TxnSpillFile), RUST_FILE, 523,
						   23);
			at++;
		}
	}
	return true;
}

/*
 * txn_spill_linked lists a spill's files in its storage's manifest, the
 * newest, a row spill's tuple-id file after its row file, each once; a
 * merge's files take the place of the files it replaces, in the same change,
 * at the newest one's place, so its versions stay below every later file's
 * of their keys (bugs row 228).
 */
bool
txn_spill_linked(const TxnSpillHomeOps *home, HostIoIo *io, HostSharedRecord record, const HostStorage *storage,
				 const TxnSpillFile *linking, size_t linking_len, bool has_replacing,
				 txn_TupleU64KindU32OptionU32_t replacing, HostError *error)
{
	txn_spill_linked_captures_t captures = {linking, linking_len, has_replacing, replacing};

	return txn_spill_changed(home, io, record, storage, txn_spill_linked_closure_0, &captures, error);
}

/* txn_spill_removed_captures_t is removed's edit's captures: its test and the files it took. */
typedef struct txn_spill_removed_captures_t
{
	bool		(*picked) (void *captures, const TxnSpillFile *file);
	void	   *picked_captures;
	StdVec	   *gone;
} txn_spill_removed_captures_t;

/* txn_spill_removed_closure_0 is removed's edit: the files its test picks out, kept in order, and the rest left. */
static bool
txn_spill_removed_closure_0(void *captures, HostSharedLongIo *io, StdVec *files, HostError *error)
{
	txn_spill_removed_captures_t *c = captures;
	size_t		kept = 0;

	for (size_t k = 0; k < files->len; k++)
	{
		TxnSpillFile file = STD_VEC_AT(files, TxnSpillFile, k);

		if (c->picked(c->picked_captures, &file))
			std_vec_push_Checked(c->gone, &file, sizeof(file), _Alignof(TxnSpillFile));
		else
			STD_VEC_AT(files, TxnSpillFile, kept++) = file;
	}
	files->len = kept;
	return true;
}

/* txn_spill_removed_frame_0_t is removed's state across its pad: the files it takes, while live. */
typedef struct txn_spill_removed_frame_0_t
{
	const TxnSpillHomeOps *home;
	HostIoIo   *io;
	HostSharedRecord record;
	const HostStorage *storage;
	txn_spill_removed_captures_t captures;
	HostError  *error;
	bool		ok;
	bool		result_live;
} txn_spill_removed_frame_0_t;

/* txn_spill_removed_pad_0 is removed's pad: the files it took, while live. */
static void
txn_spill_removed_pad_0(void *frame_)
{
	txn_spill_removed_frame_0_t *frame = frame_;

	if (frame->result_live)
	{
		frame->result_live = false;
		std_vec_free_Checked(frame->captures.gone, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	}
}

/* txn_spill_removed_body_0 is removed's body. */
static void
txn_spill_removed_body_0(void *frame_)
{
	txn_spill_removed_frame_0_t *frame = frame_;

	std_vec_new(frame->captures.gone, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	frame->result_live = true;
	if (!txn_spill_changed(frame->home, frame->io, frame->record, frame->storage, txn_spill_removed_closure_0,
						   &frame->captures, frame->error))
	{
		txn_spill_removed_pad_0(frame);
		return;
	}
	frame->result_live = false;
	frame->ok = true;
}

/* txn_spill_removed takes the files of a storage's spills that a test picks out of its manifest, answering them. */
bool
txn_spill_removed(const TxnSpillHomeOps *home, HostIoIo *io, HostSharedRecord record, const HostStorage *storage,
				  bool (*picked) (void *captures, const TxnSpillFile *file), void *picked_captures, StdVec *result,
				  HostError *error)
{
	txn_spill_removed_frame_0_t frame = {.home = home, .io = io, .record = record, .storage = storage,
	.captures = {picked, picked_captures, result}, .error = error};

	std_landing(&frame, txn_spill_removed_body_0, txn_spill_removed_pad_0);
	return frame.ok;
}

/* TXN_SPILL_KEPT_LISTS is KEPT_LISTS, the most storages' manifests a backend keeps its copies of. */
#define TXN_SPILL_KEPT_LISTS 16

/* txn_TupleStorageU64VecFile_t is KeptList, (Storage, u64, Vec<File>): a backend's copy of a storage's manifest, the generation it read it at, and its files, newest first. */
typedef struct txn_TupleStorageU64VecFile_t
{
	HostStorage f0;
	uint64		f1;
	StdVec		f2;
} txn_TupleStorageU64VecFile_t;

/* txn_spill_TupleStorageU64VecFile_drop is KeptList's drop glue: its files'. */
static void
txn_spill_TupleStorageU64VecFile_drop(void *elem)
{
	txn_TupleStorageU64VecFile_t *self = elem;

	std_vec_free_Checked(&self->f2, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
}

/* txn_spill_VecKeptList_drop is Vec<KeptList>'s drop glue. */
static void
txn_spill_VecKeptList_drop(void *value)
{
	std_vec_drop_Checked(value, sizeof(txn_TupleStorageU64VecFile_t), _Alignof(txn_TupleStorageU64VecFile_t),
						 txn_spill_TupleStorageU64VecFile_drop);
}

/* taken at Vec<KeptList> and IoMutex<Vec<KeptList>>, which spill.c alone uses. */
#define HOST_LOCK_SCOPE static
#define HOST_LOCK_DECLARE
#define HOST_LOCK_DEFINE
#define HOST_LOCK_TAKEN_TS VecKeptList
#define HOST_LOCK_TAKEN_T StdVec
#define HOST_LOCK_TAKEN_DEFAULT(place) \
	std_vec_new((place), sizeof(txn_TupleStorageU64VecFile_t), _Alignof(txn_TupleStorageU64VecFile_t))
#define HOST_LOCK_TAKEN_DROP(place) txn_spill_VecKeptList_drop(place)
#define HOST_LOCK_TAKEN_TYPE TXN_SPILL_LISTS_LOCK_TYPE
#define HOST_LOCK_IO_MUTEX
#define HOST_LOCK_TS VecKeptList
#define HOST_LOCK_T StdVec
#define HOST_LOCK_DEFAULT(place) \
	std_vec_new((place), sizeof(txn_TupleStorageU64VecFile_t), _Alignof(txn_TupleStorageU64VecFile_t))
#include "doltgres/host/lock_template.h"

/* txn_spill_IoMutexVecKeptList_drop is IoMutex<Vec<KeptList>>' drop glue: its value's. */
static void
txn_spill_IoMutexVecKeptList_drop(void *value)
{
	HostLockIoMutexVecKeptList *self = value;

	txn_spill_VecKeptList_drop(&self->value);
}

/* TXN_SPILL_LISTS_TYPE is IoMutex<Vec<KeptList>>' size, Default, and drop. */
static const StdDefaultType TXN_SPILL_LISTS_TYPE = {sizeof(HostLockIoMutexVecKeptList),
	_Alignof(HostLockIoMutexVecKeptList), host_lock_IoMutexVecKeptList_Default_default,
txn_spill_IoMutexVecKeptList_drop};

/*
 * TXN_SPILL_LISTS is LISTS: the backend's copies of the storages' manifests
 * it read last, the latest first, each with the generation its state held
 * when it began the read; its BackendLocal header and the process's value,
 * which follows it.
 */
static struct
{
	HostLocalBackendLocal local;
	HostLockIoMutexVecKeptList process;
}			TXN_SPILL_LISTS = {HOST_LOCAL_BACKEND_LOCAL_NEW};

/* txn_spill_listed_frame_0_t is listed's state across its pad: the backend's copies and their guard, the files read, and the one kept, while live. */
typedef struct txn_spill_listed_frame_0_t
{
	const TxnSpillHomeOps *home;
	HostIoIo   *io;
	HostSharedRecord record;
	const HostStorage *storage;
	txn_TupleVecFileU64_t *result;
	HostError  *error;
	bool		ok;
	HostLocalLocal lists;
	bool		lists_live;
	HostLockLockedRwLockWriteGuard guard;
	bool		guard_live;
	StdPathBuf	path;
	bool		path_live;
	bool		result_live;
	txn_TupleStorageU64VecFile_t kept;
	bool		kept_live;
} txn_spill_listed_frame_0_t;

/* txn_spill_listed_pad_0 is listed's pad: what it holds, while live. */
static void
txn_spill_listed_pad_0(void *frame_)
{
	txn_spill_listed_frame_0_t *frame = frame_;

	if (frame->kept_live)
	{
		frame->kept_live = false;
		txn_spill_TupleStorageU64VecFile_drop(&frame->kept);
	}
	if (frame->result_live)
	{
		frame->result_live = false;
		std_vec_free_Checked(&frame->result->f0, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	}
	if (frame->path_live)
	{
		frame->path_live = false;
		std_vec_free_Checked(&frame->path.bytes, 1, 1);
	}
	if (frame->guard_live)
	{
		frame->guard_live = false;
		host_lock_LockedRwLockWriteGuard_drop(&frame->guard);
	}
	if (frame->lists_live)
	{
		frame->lists_live = false;
		host_local_Local_drop(&frame->lists, &TXN_SPILL_LISTS_TYPE);
	}
}

/* txn_spill_listed_lock locks the backend's copies, answering them. */
static StdVec *
txn_spill_listed_lock(txn_spill_listed_frame_0_t *frame)
{
	frame->guard = host_lock_IoMutex_lock((HostLockIoMutex *) host_local_Local_Deref_deref(&frame->lists,
																						   &TXN_SPILL_LISTS_TYPE),
										  &TXN_SPILL_LISTS_LOCK_TYPE, &frame->io->held);
	frame->guard_live = true;
	return host_lock_LockedRwLockWriteGuard_DerefMut_deref_mut(&frame->guard);
}

/* txn_spill_listed_unlock drops the guard of the backend's copies. */
static void
txn_spill_listed_unlock(txn_spill_listed_frame_0_t *frame)
{
	frame->guard_live = false;
	host_lock_LockedRwLockWriteGuard_drop(&frame->guard);
}

/* txn_spill_listed_kept copies the files of the backend's copy read at the generation into the result, moving the copy first, or answers false for none. */
static bool
txn_spill_listed_kept(txn_spill_listed_frame_0_t *frame, uint64 generation)
{
	StdVec	   *lists = txn_spill_listed_lock(frame);
	size_t		at;

	for (at = 0; at < lists->len; at++)
	{
		const txn_TupleStorageU64VecFile_t *list = &STD_VEC_AT(lists, txn_TupleStorageU64VecFile_t, at);

		if (host_Storage_PartialEq_eq(&list->f0, frame->storage) && list->f1 == generation)
			break;
	}
	if (at == lists->len)
	{
		txn_spill_listed_unlock(frame);
		return false;
	}
	std_vec_remove(lists, at, &frame->kept, sizeof(frame->kept), _Alignof(txn_TupleStorageU64VecFile_t),
				   RUST_FILE, 491, 34);
	frame->kept_live = true;
	std_vec_clone(&frame->result->f0, &frame->kept.f2, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	frame->result_live = true;
	std_vec_insert(lists, 0, &frame->kept, sizeof(frame->kept), _Alignof(txn_TupleStorageU64VecFile_t), RUST_FILE,
				   493, 23);
	frame->kept_live = false;
	txn_spill_listed_unlock(frame);
	return true;
}

/* txn_spill_listed_read reads the storage's manifest file into the result, newest first, and keeps a copy of it first among the backend's. */
static bool
txn_spill_listed_read(txn_spill_listed_frame_0_t *frame, uint64 generation)
{
	StdVec	   *lists;
	StdVec	   *files = &frame->result->f0;
	size_t		kept = 0;

	txn_spill_manifest(frame->home, frame->record, frame->storage, &frame->path);
	frame->path_live = true;
	if (!txn_spill_read(HOST_IO_IO_READ_DOORS(frame->io), HOST_SHARED_IO_PAUSES(frame->io), &frame->path, files,
						frame->error))
		return false;
	frame->result_live = true;
	for (size_t k = 0; k < files->len / 2; k++)
	{
		TxnSpillFile swapped = STD_VEC_AT(files, TxnSpillFile, k);

		STD_VEC_AT(files, TxnSpillFile, k) = STD_VEC_AT(files, TxnSpillFile, files->len - 1 - k);
		STD_VEC_AT(files, TxnSpillFile, files->len - 1 - k) = swapped;
	}
	lists = txn_spill_listed_lock(frame);
	for (size_t k = 0; k < lists->len; k++)
	{
		txn_TupleStorageU64VecFile_t *list = &STD_VEC_AT(lists, txn_TupleStorageU64VecFile_t, k);

		if (host_Storage_PartialEq_eq(&list->f0, frame->storage))
			txn_spill_TupleStorageU64VecFile_drop(list);
		else
			STD_VEC_AT(lists, txn_TupleStorageU64VecFile_t, kept++) = *list;
	}
	lists->len = kept;
	if (!std_vec_try_reserve(lists, 1, sizeof(frame->kept), _Alignof(txn_TupleStorageU64VecFile_t)))
	{
		*frame->error = host_Error_out_of_memory();
		return false;
	}
	frame->kept.f0 = *frame->storage;
	frame->kept.f1 = generation;
	std_vec_clone(&frame->kept.f2, files, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	frame->kept_live = true;
	std_vec_insert(lists, 0, &frame->kept, sizeof(frame->kept), _Alignof(txn_TupleStorageU64VecFile_t), RUST_FILE,
				   507, 19);
	frame->kept_live = false;
	std_vec_truncate(lists, TXN_SPILL_KEPT_LISTS, sizeof(txn_TupleStorageU64VecFile_t),
					 txn_spill_TupleStorageU64VecFile_drop);
	txn_spill_listed_unlock(frame);
	return true;
}

/* txn_spill_listed_body_0 is listed's body. */
static void
txn_spill_listed_body_0(void *frame_)
{
	txn_spill_listed_frame_0_t *frame = frame_;
	HostSharedOp load = {.tag = HOST_SHARED_OP_LOAD};
	uint64		generation = host_shared_Record_atomic(frame->record, frame->home->changed, load);

	frame->result->f1 = generation;
	if (host_shared_Record_atomic(frame->record, frame->home->listed, load) == 0)
	{
		std_vec_new(&frame->result->f0, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
		frame->ok = true;
		return;
	}
	frame->lists = host_local_BackendLocal_get(&TXN_SPILL_LISTS.local, &TXN_SPILL_LISTS_TYPE);
	frame->lists_live = true;
	if (!txn_spill_listed_kept(frame, generation) && !txn_spill_listed_read(frame, generation))
	{
		txn_spill_listed_pad_0(frame);
		return;
	}
	frame->result_live = false;
	frame->ok = true;
	txn_spill_listed_pad_0(frame);
}

/*
 * txn_spill_listed returns a storage's spill files, newest first, a part's
 * tuple-id file before its row file, with the generation it loaded before it
 * read them: from the backend's copy while that generation is the copy's,
 * and from the manifest file otherwise. It copies the list out of the
 * backend's copy, which the next read keeps.
 */
bool
txn_spill_listed(const TxnSpillHomeOps *home, HostIoIo *io, HostSharedRecord record, const HostStorage *storage,
				 txn_TupleVecFileU64_t *result, HostError *error)
{
	txn_spill_listed_frame_0_t frame = {.home = home, .io = io, .record = record, .storage = storage,
	.result = result, .error = error};

	std_landing(&frame, txn_spill_listed_body_0, txn_spill_listed_pad_0);
	return frame.ok;
}

/* txn_spill_filter_key returns the node cache's key of a page of a listed spill's file's filter: the file's path, length, and xxh3, and the page's number. */
static HostCacheKey
txn_spill_filter_key(const StdPathBuf *path, const TxnSpillFile *file, uint64 page)
{
	uint8		address[HOST_CACHE_ADDRESS] = {0};
	uint8		locator[HOST_CACHE_LOCATOR] = {0};
	XXH128_hash_t hash = XXH3_128bits(path->bytes.ptr, path->bytes.len);

	std_u64_to_le_bytes(hash.low64, address);
	std_u64_to_le_bytes(hash.high64, address + 8);
	std_u64_to_le_bytes(file->len, address + 16);
	std_u64_to_le_bytes(file->hash, address + 24);
	std_u64_to_le_bytes(page, locator);
	return host_cache_key(0, address, locator);
}

/* txn_spill_hits_frame_0_t is hits' state across its pad: a page's pin, or the file and the page it reads and installs, while live. */
typedef struct txn_spill_hits_frame_0_t
{
	HostIoIo   *io;
	const StdPathBuf *path;
	const TxnSpillFile *file;
	const uint8 *key;
	size_t		key_len;
	bool	   *result;
	HostError  *error;
	bool		ok;
	HostCachePinned pinned;
	bool		pinned_live;
	HostFile	opened;
	bool		opened_live;
	StdVec		bytes;
	bool		bytes_live;
	HostCacheRead read;
	bool		read_live;
} txn_spill_hits_frame_0_t;

/* txn_spill_hits_pad_0 is hits' pad: what it holds, while live. */
static void
txn_spill_hits_pad_0(void *frame_)
{
	txn_spill_hits_frame_0_t *frame = frame_;

	if (frame->read_live)
	{
		frame->read_live = false;
		host_cache_Read_drop(&frame->read);
	}
	if (frame->bytes_live)
	{
		frame->bytes_live = false;
		std_vec_free_Checked(&frame->bytes, 1, 1);
	}
	if (frame->opened_live)
	{
		frame->opened_live = false;
		host_File_drop(&frame->opened);
	}
	if (frame->pinned_live)
	{
		frame->pinned_live = false;
		host_cache_Pinned_drop(&frame->pinned);
	}
}

/* txn_spill_hits_fill is install's fill: the page's bytes read from the file. */
static void
txn_spill_hits_fill(void *captures, uint8 *out, size_t len)
{
	const StdVec *bytes = captures;

	memcpy(out, bytes->ptr, len);
}

/* txn_spill_word reads the filter word at a byte offset of a page, or answers false for a page too short. */
static bool
txn_spill_word(StdBytes page, size_t at, uint64 *result)
{
	if (at > page.len || page.len - at < 8)
		return false;
	*result = std_u64_from_le_bytes(page.ptr + at);
	return true;
}

/* txn_spill_hits_page loads the filter word a bit falls in, from the node cache or from the file. */
static bool
txn_spill_hits_page(txn_spill_hits_frame_0_t *frame, uint64 page, size_t at, bool *loaded, uint64 *word)
{
	HostCacheKey key = txn_spill_filter_key(frame->path, frame->file, page);
	ChunksErrorError failed;
	bool		some;

	if (!host_cache_get(frame->io, HOST_CACHE_PLACEMENT_SERVER, &key, &some, &frame->pinned, frame->error))
		return false;
	if (some)
	{
		frame->pinned_live = true;
		*loaded = txn_spill_word(host_cache_Pinned_Deref_deref(&frame->pinned), at, word);
		frame->pinned_live = false;
		host_cache_Pinned_drop(&frame->pinned);
		return true;
	}
	if (!chunks_file_open_rw(HOST_IO_IO_IO_DOORS(frame->io), frame->path->bytes.ptr, frame->path->bytes.len, false,
							 &frame->opened, &failed))
	{
		*frame->error = txn_base_storage(&failed);
		return false;
	}
	frame->opened_live = true;
	if (!txn_runs_spill_filter(frame->io, &frame->opened, frame->file->len, frame->file->bits, page, &frame->bytes,
							   frame->error))
		return false;
	frame->bytes_live = true;
	if (!host_cache_install(frame->io, HOST_CACHE_PLACEMENT_SERVER, &key, frame->bytes.len,
							txn_spill_hits_fill, &frame->bytes, NULL, &frame->read, frame->error))
		return false;
	frame->read_live = true;
	*loaded = txn_spill_word(host_cache_Read_Deref_deref(&frame->read), at, word);
	txn_spill_hits_pad_0(frame);
	return true;
}

/* txn_spill_hits_body_0 is hits' body. */
static void
txn_spill_hits_body_0(void *frame_)
{
	txn_spill_hits_frame_0_t *frame = frame_;
	uint64		bits[TXN_SPILL_HASHES];

	txn_spill_positions(frame->key, frame->key_len, frame->file->bits, bits);
	*frame->result = true;
	for (size_t k = 0; k < TXN_SPILL_HASHES; k++)
	{
		uint64		page = bits[k] / TXN_SPILL_PAGE_BITS;
		size_t		at = (size_t) (bits[k] % TXN_SPILL_PAGE_BITS / 64) * 8;
		bool		loaded;
		uint64		word;

		if (!txn_spill_hits_page(frame, page, at, &loaded, &word))
		{
			txn_spill_hits_pad_0(frame);
			return;
		}
		if (!loaded)
		{
			*frame->error = txn_spill_internal("a spill's filter page is shorter than its bits");
			return;
		}
		if ((word & (uint64) 1 << (bits[k] % 64)) == 0)
		{
			*frame->result = false;
			break;
		}
	}
	frame->ok = true;
}

/*
 * txn_spill_hits reports whether a key may be in a listed spill's file of a
 * path: it reads each page of the file's filter the key's bits fall in
 * through the node cache, and from the file when the cache holds no copy.
 */
static bool
txn_spill_hits(HostIoIo *io, const StdPathBuf *path, const TxnSpillFile *file, const uint8 *key, size_t key_len,
			   bool *result, HostError *error)
{
	txn_spill_hits_frame_0_t frame = {.io = io, .path = path, .file = file, .key = key, .key_len = key_len,
	.result = result, .error = error};

	std_landing(&frame, txn_spill_hits_body_0, txn_spill_hits_pad_0);
	return frame.ok;
}

/* TXN_SPILL_PENDING_PROC_SLOTS_OPS is Pending's ProcSlots: each process's slot naming its one spill no manifest lists yet. */
const HostSharedProcSlotsOps TXN_SPILL_PENDING_PROC_SLOTS_OPS = {.id = UINT64CONST(0x74786e000015), .words = 8};

/* The slot's words: its storage's database and relation, tablespace, xid, numbered word, sources, unspilled bytes, turn, and top. */
#define TXN_SPILL_PENDING_STORAGE 0
#define TXN_SPILL_PENDING_TABLESPACE 1
#define TXN_SPILL_PENDING_XID 2
#define TXN_SPILL_PENDING_NUMBERED 3
#define TXN_SPILL_PENDING_SOURCES 4
#define TXN_SPILL_PENDING_UNSPILLED 5
#define TXN_SPILL_PENDING_TURN 6
#define TXN_SPILL_PENDING_TOP 7

/* txn_spill_mine returns the backend's own slot of Pending. */
static bool
txn_spill_mine(HostIoIo *io, HostSharedMine *result, HostError *error)
{
	return host_shared_mine(HOST_IO_IO_TAKES_SHORT(io), &TXN_SPILL_PENDING_PROC_SLOTS_OPS, result, error);
}

/* txn_spill_wrote publishes a count of bytes more of the versions the backend's transaction, of a top-level xid, wrote and hasn't spilled. */
bool
txn_spill_wrote(HostIoIo *io, uint64 top, uint64 bytes, HostError *error)
{
	HostSharedMine mine;

	if (!host_shared_ready())
		return true;
	if (!txn_spill_mine(io, &mine, error))
		return false;
	host_shared_Mine_store(&mine, TXN_SPILL_PENDING_TOP, top);
	for (;;)
	{
		uint64		loaded = host_shared_Mine_load(&mine, TXN_SPILL_PENDING_UNSPILLED);

		if (host_shared_Mine_exchange(&mine, TXN_SPILL_PENDING_UNSPILLED, loaded,
									  STD_ADD(u64, loaded, bytes, 629, 54)) == loaded)
			return true;
	}
}

/* txn_spill_unwrote takes a count of bytes a spill took out of the memtable from the backend's published count, or, given none, zeroes it. */
bool
txn_spill_unwrote(HostIoIo *io, bool has_bytes, uint64 bytes, HostError *error)
{
	HostSharedMine mine;

	if (!host_shared_ready())
		return true;
	if (!txn_spill_mine(io, &mine, error))
		return false;
	for (;;)
	{
		uint64		loaded = host_shared_Mine_load(&mine, TXN_SPILL_PENDING_UNSPILLED);
		uint64		left = has_bytes ? std_saturating_sub_u64(loaded, bytes) : 0;

		if (host_shared_Mine_exchange(&mine, TXN_SPILL_PENDING_UNSPILLED, loaded, left) == loaded)
			return true;
	}
}

/* TxnSpillTurnSlotTag is TurnSlot's variants: the backend's own slot, or another process's. */
typedef enum TxnSpillTurnSlotTag
{
	TXN_SPILL_TURN_SLOT_MINE,
	TXN_SPILL_TURN_SLOT_THEIRS,
} TxnSpillTurnSlotTag;

/* TxnSpillTurn is Turn: a process's turn at spilling a transaction's versions, held in the slot of the process whose transaction it is, and one past the id of the process that took it. */
typedef struct TxnSpillTurn
{
	TxnSpillTurnSlotTag tag;
	HostSharedMine mine;
	HostSharedTheirs theirs;
	uint64		id;
} TxnSpillTurn;

/* txn_spill_Turn_own takes the backend's turn at spilling its own transaction's versions, or answers nothing while a helper spills them. */
static bool
txn_spill_Turn_own(HostIoIo *io, bool *some, TxnSpillTurn *result, HostError *error)
{
	if (!txn_spill_mine(io, &result->mine, error))
		return false;
	result->tag = TXN_SPILL_TURN_SLOT_MINE;
	result->id = STD_ADD(u64, host_shared_Mine_id(&result->mine), 1, 671, 18);
	*some = host_shared_Mine_exchange(&result->mine, TXN_SPILL_PENDING_TURN, 0, result->id) == 0;
	return true;
}

/* txn_spill_Turn_of takes a helper's turn at spilling another process's transaction's versions, in that process's slot, or answers nothing while another process spills them. */
static bool
txn_spill_Turn_of(HostIoIo *io, const HostSharedTheirs *theirs, bool *some, TxnSpillTurn *result, HostError *error)
{
	HostSharedMine mine;

	if (!txn_spill_mine(io, &mine, error))
		return false;
	result->tag = TXN_SPILL_TURN_SLOT_THEIRS;
	result->theirs = *theirs;
	result->id = STD_ADD(u64, host_shared_Mine_id(&mine), 1, 678, 18);
	*some = host_shared_Theirs_exchange(&result->theirs, TXN_SPILL_PENDING_TURN, 0, result->id) == 0;
	return true;
}

/* txn_spill_Turn_given gives the turn back, and for a helper's, zeroes the holder's count of unspilled bytes it loaded. */
static void
txn_spill_Turn_given(const TxnSpillTurn *self, uint64 unspilled)
{
	if (self->tag == TXN_SPILL_TURN_SLOT_MINE)
	{
		host_shared_Mine_exchange(&self->mine, TXN_SPILL_PENDING_TURN, self->id, 0);
		return;
	}
	host_shared_Theirs_exchange(&self->theirs, TXN_SPILL_PENDING_TURN, self->id, 0);
	host_shared_Theirs_exchange(&self->theirs, TXN_SPILL_PENDING_UNSPILLED, unspilled, 0);
}

/* TXN_SPILL_TUPLED is TUPLED, the bit of the slot's kind-and-number word that says the pending spill has a tuple-id file of its number too. */
#define TXN_SPILL_TUPLED ((uint64) 1 << 40)

/*
 * txn_spill_pend stores the process's slot naming a spill of a storage
 * before its versions leave the memtable, after adding to the record's count
 * of pending spills, with whether the spill has a tuple-id file, and, for a
 * merge, the lowest number of the files it replaces.
 */
static bool
txn_spill_pend(const TxnSpillHomeOps *home, HostIoIo *io, HostSharedRecord state, const HostStorage *storage,
			   const TxnSpillFile *file, bool tupled, bool has_sources, uint32 sources, HostError *error)
{
	HostSharedMine mine;
	HostSharedOp add = {.tag = HOST_SHARED_OP_ADD, .u.add = 1};

	if (!txn_spill_mine(io, &mine, error))
		return false;
	if (host_shared_Mine_load(&mine, TXN_SPILL_PENDING_NUMBERED) != 0)
	{
		*error = txn_spill_internal("a process pended a spill while another was pending");
		return false;
	}
	host_shared_Record_atomic(state, home->pended, add);
	host_shared_Mine_store(&mine, TXN_SPILL_PENDING_STORAGE, (uint64) storage->database << 32 | storage->relation);
	host_shared_Mine_store(&mine, TXN_SPILL_PENDING_TABLESPACE, storage->tablespace);
	host_shared_Mine_store(&mine, TXN_SPILL_PENDING_XID, file->xid);
	host_shared_Mine_store(&mine, TXN_SPILL_PENDING_SOURCES, has_sources ? (uint64) sources + 1 : 0);
	host_shared_Mine_store(&mine, TXN_SPILL_PENDING_NUMBERED,
						   STD_ADD(u64, txn_spill_File_numbered(file) | (tupled ? TXN_SPILL_TUPLED : 0), 1,
								   720, 35));
	return true;
}

/* TxnSpillPended is Pended: a spill a process's slot names, its storage, xid, kind, number, whether it has a tuple-id file, and, for a merge, the lowest number of the files it replaces. */
typedef struct TxnSpillPended
{
	HostStorage storage;
	uint64		xid;
	TxnRunsKind kind;
	uint32		number;
	bool		tupled;
	bool		has_sources;
	uint32		sources;
} TxnSpillPended;

/* txn_spill_pending returns the spill the process's slot names, or nothing for none. */
static bool
txn_spill_pending(HostIoIo *io, bool *some, TxnSpillPended *result, HostError *error)
{
	HostSharedMine mine;
	uint64		numbered;
	uint64		named;
	uint64		sources;

	if (!txn_spill_mine(io, &mine, error))
		return false;
	numbered = host_shared_Mine_load(&mine, TXN_SPILL_PENDING_NUMBERED);
	*some = numbered != 0;
	if (!*some)
		return true;
	numbered--;
	named = host_shared_Mine_load(&mine, TXN_SPILL_PENDING_STORAGE);
	result->storage.tablespace = (uint32) host_shared_Mine_load(&mine, TXN_SPILL_PENDING_TABLESPACE);
	result->storage.database = (uint32) (named >> 32);
	result->storage.relation = (uint32) named;
	if (!txn_runs_Kind_of((uint8) (numbered >> 32), &result->kind))
	{
		*error = txn_spill_internal("a pending spill names no kind");
		return false;
	}
	sources = host_shared_Mine_load(&mine, TXN_SPILL_PENDING_SOURCES);
	result->has_sources = sources != 0;
	result->sources = (uint32) (sources - result->has_sources);
	result->xid = host_shared_Mine_load(&mine, TXN_SPILL_PENDING_XID);
	result->number = (uint32) numbered;
	result->tupled = (numbered & TXN_SPILL_TUPLED) != 0;
	return true;
}

/* txn_spill_settled clears the process's slot once its spill is listed or its transaction ended, then subtracts from the record's count of pending spills. */
static bool
txn_spill_settled(const TxnSpillHomeOps *home, HostIoIo *io, const HostSharedRecord *state, HostError *error)
{
	HostSharedMine mine;
	HostSharedOp sub = {.tag = HOST_SHARED_OP_ADD, .u.add = PG_UINT64_MAX};

	if (!txn_spill_mine(io, &mine, error))
		return false;
	host_shared_Mine_store(&mine, TXN_SPILL_PENDING_NUMBERED, 0);
	host_shared_Mine_store(&mine, TXN_SPILL_PENDING_SOURCES, 0);
	if (state != NULL)
		host_shared_Record_atomic(*state, home->pended, sub);
	return true;
}

/* txn_spill_pended_frame_0_t is pended's state across its pad: the processes and the spills found, while live. */
typedef struct txn_spill_pended_frame_0_t
{
	const TxnSpillHomeOps *home;
	HostSharedRecord state;
	const HostStorage *storage;
	StdVec	   *result;
	HostError  *error;
	bool		ok;
	bool		result_live;
	/* processes is a Vec<u32>. */
	StdVec		processes;
	bool		processes_live;
} txn_spill_pended_frame_0_t;

/* txn_spill_pended_pad_0 is pended's pad: the processes, then the spills found, while live. */
static void
txn_spill_pended_pad_0(void *frame_)
{
	txn_spill_pended_frame_0_t *frame = frame_;

	if (frame->processes_live)
	{
		frame->processes_live = false;
		std_vec_free_Checked(&frame->processes, sizeof(uint32), _Alignof(uint32));
	}
	if (frame->result_live)
	{
		frame->result_live = false;
		std_vec_free_Checked(frame->result, sizeof(txn_TupleU64KindU32OptionU32_t),
							 _Alignof(txn_TupleU64KindU32OptionU32_t));
	}
}

/* txn_spill_pended_process adds the spills one process's slot names of the frame's storage. */
static bool
txn_spill_pended_process(txn_spill_pended_frame_0_t *frame, uint32 process)
{
	const HostSharedProcSlotsOps *slots = &TXN_SPILL_PENDING_PROC_SLOTS_OPS;
	uint64		named = (uint64) frame->storage->database << 32 | frame->storage->relation;
	uint64		numbered;
	uint64		word;
	uint64		tablespace;
	uint64		sources;
	txn_TupleU64KindU32OptionU32_t found;

	if (!host_shared_load(slots, TXN_SPILL_PENDING_NUMBERED, process, &numbered, frame->error))
		return false;
	if (numbered == 0)
		return true;
	numbered--;
	if (!host_shared_load(slots, TXN_SPILL_PENDING_STORAGE, process, &word, frame->error))
		return false;
	if (word != named)
		return true;
	if (!host_shared_load(slots, TXN_SPILL_PENDING_TABLESPACE, process, &tablespace, frame->error))
		return false;
	if (tablespace != frame->storage->tablespace || !txn_runs_Kind_of((uint8) (numbered >> 32), &found.f1))
		return true;
	if (!host_shared_load(slots, TXN_SPILL_PENDING_XID, process, &found.f0, frame->error) ||
		!host_shared_load(slots, TXN_SPILL_PENDING_SOURCES, process, &sources, frame->error))
		return false;
	found.f2 = (uint32) numbered;
	found.has_f3 = sources != 0;
	found.f3 = (uint32) (sources - found.has_f3);
	if (!std_vec_try_reserve(frame->result, 2, sizeof(found), _Alignof(txn_TupleU64KindU32OptionU32_t)))
	{
		*frame->error = host_Error_out_of_memory();
		return false;
	}
	std_vec_push_Checked(frame->result, &found, sizeof(found), _Alignof(txn_TupleU64KindU32OptionU32_t));
	if ((numbered & TXN_SPILL_TUPLED) != 0)
	{
		found.f1 = TXN_RUNS_KIND_SPILLED_TUPLES;
		std_vec_push_Checked(frame->result, &found, sizeof(found), _Alignof(txn_TupleU64KindU32OptionU32_t));
	}
	return true;
}

/* txn_spill_pended_body_0 is pended's body. */
static void
txn_spill_pended_body_0(void *frame_)
{
	txn_spill_pended_frame_0_t *frame = frame_;
	HostSharedOp load = {.tag = HOST_SHARED_OP_LOAD};

	std_vec_new(frame->result, sizeof(txn_TupleU64KindU32OptionU32_t), _Alignof(txn_TupleU64KindU32OptionU32_t));
	if (host_shared_Record_atomic(frame->state, frame->home->pended, load) == 0)
	{
		frame->ok = true;
		return;
	}
	frame->result_live = true;
	if (!host_shared_processes(&TXN_SPILL_PENDING_PROC_SLOTS_OPS, &frame->processes, frame->error))
	{
		txn_spill_pended_pad_0(frame);
		return;
	}
	frame->processes_live = true;
	for (size_t k = 0; k < frame->processes.len; k++)
		if (!txn_spill_pended_process(frame, STD_VEC_AT(&frame->processes, uint32, k)))
		{
			txn_spill_pended_pad_0(frame);
			return;
		}
	frame->result_live = false;
	frame->ok = true;
	txn_spill_pended_pad_0(frame);
}

/*
 * txn_spill_pended returns the spills of a storage that processes' slots
 * name, a spill's tuple-id file after its row file, read only while the
 * record's count of pending spills isn't zero.
 */
static bool
txn_spill_pended(const TxnSpillHomeOps *home, HostSharedRecord state, const HostStorage *storage, StdVec *result,
				 HostError *error)
{
	txn_spill_pended_frame_0_t frame = {.home = home, .state = state, .storage = storage, .result = result,
	.error = error};

	std_landing(&frame, txn_spill_pended_body_0, txn_spill_pended_pad_0);
	return frame.ok;
}

/* TXN_SPILL_SHARE is SHARE, the part of the delta's budget a transaction's versions in one storage's tree reach before they spill. */
#define TXN_SPILL_SHARE 8

/* TXN_SPILL_LOW is LOW, the bytes a transaction's versions in one storage's tree reach before they spill once the delta is full. */
#define TXN_SPILL_LOW ((uint64) 256 << 10)

/* txn_spill_over returns the threshold of a writer over the budget, or refused by it: an eighth of the budget, at most LOW. */
static uint64
txn_spill_over(void)
{
	return Min(host_delta_budget() / TXN_SPILL_SHARE, TXN_SPILL_LOW);
}

/* txn_spill_threshold returns the bytes a transaction's versions in one storage's tree reach before they spill. */
uint64
txn_spill_threshold(void)
{
	if (host_delta_used() >= host_delta_budget())
		return txn_spill_over();
	return host_delta_budget() / TXN_SPILL_SHARE;
}

/* txn_spill_tree reports whether a spill file's kind is one a reader asks for, a storage's entries among its rows. */
static bool
txn_spill_tree(TxnRunsKind file, TxnRunsKind asked)
{
	return file == asked || (file == TXN_RUNS_KIND_SPILLED_ENTRIES && asked == TXN_RUNS_KIND_SPILLED_ROWS);
}

/* txn_spill_names returns what a spill's header names: its kind, its storage and the storage's persistence, and its transaction's xid. */
static bool
txn_spill_names(const TxnSpillHomeOps *home, HostSharedRecord record, const HostStorage *storage, TxnRunsKind kind,
				uint64 xid, TxnRunsNames *result, HostError *error)
{
	HostStorage described;

	if (!home->described(record, &described, &result->persistence))
	{
		*error = txn_spill_internal("a spill's state isn't filled");
		return false;
	}
	result->kind = kind;
	result->tablespace = storage->tablespace;
	result->database = storage->database;
	result->relation = storage->relation;
	result->redo = xid;
	return true;
}

/* txn_spill_VecReplayed_drop is Vec<Replayed>'s drop glue. */
static void
txn_spill_VecReplayed_drop(void *value)
{
	std_vec_free_Checked(value, sizeof(txn_TupleStorageOfU64U64_t), _Alignof(txn_TupleStorageOfU64U64_t));
}

/* taken at Vec<Replayed> and IoMutex<Vec<Replayed>>, which spill.c alone uses. */
#define HOST_LOCK_SCOPE static
#define HOST_LOCK_DECLARE
#define HOST_LOCK_DEFINE
#define HOST_LOCK_TAKEN_TS VecReplayed
#define HOST_LOCK_TAKEN_T StdVec
#define HOST_LOCK_TAKEN_DEFAULT(place) \
	std_vec_new((place), sizeof(txn_TupleStorageOfU64U64_t), _Alignof(txn_TupleStorageOfU64U64_t))
#define HOST_LOCK_TAKEN_DROP(place) txn_spill_VecReplayed_drop(place)
#define HOST_LOCK_TAKEN_TYPE TXN_SPILL_REPLAYED_LOCK_TYPE
#define HOST_LOCK_IO_MUTEX
#define HOST_LOCK_TS VecReplayed
#define HOST_LOCK_T StdVec
#define HOST_LOCK_DEFAULT(place) \
	std_vec_new((place), sizeof(txn_TupleStorageOfU64U64_t), _Alignof(txn_TupleStorageOfU64U64_t))
#include "doltgres/host/lock_template.h"

/* txn_spill_IoMutexVecReplayed_drop is IoMutex<Vec<Replayed>>' drop glue: its value's. */
static void
txn_spill_IoMutexVecReplayed_drop(void *value)
{
	HostLockIoMutexVecReplayed *self = value;

	txn_spill_VecReplayed_drop(&self->value);
}

/* TXN_SPILL_REPLAYED_TYPE is IoMutex<Vec<Replayed>>' size, Default, and drop. */
static const StdDefaultType TXN_SPILL_REPLAYED_TYPE = {sizeof(HostLockIoMutexVecReplayed),
	_Alignof(HostLockIoMutexVecReplayed), host_lock_IoMutexVecReplayed_Default_default,
txn_spill_IoMutexVecReplayed_drop};

/*
 * TXN_SPILL_REPLAYED is REPLAYED: the startup process's bytes of the versions
 * each replayed top-level transaction wrote to each storage's tree since
 * that tree's last spill, by storage, tree, and xid; its BackendLocal header
 * and the process's value, which follows it.
 */
static struct
{
	HostLocalBackendLocal local;
	HostLockIoMutexVecReplayed process;
}			TXN_SPILL_REPLAYED = {HOST_LOCAL_BACKEND_LOCAL_NEW};

/* txn_spill_replayed_frame_0_t is the state of a step under REPLAYED's lock across its pad: the local and its guard, while live. */
typedef struct txn_spill_replayed_frame_0_t
{
	HostIoIo   *io;
	void		(*step) (void *captures, StdVec *replayed);
	void	   *captures;
	HostLocalLocal local;
	bool		local_live;
	HostLockLockedRwLockWriteGuard guard;
	bool		guard_live;
} txn_spill_replayed_frame_0_t;

/* txn_spill_replayed_pad_0 is the pad of a step under REPLAYED's lock: the guard, then the local, while live. */
static void
txn_spill_replayed_pad_0(void *frame_)
{
	txn_spill_replayed_frame_0_t *frame = frame_;

	if (frame->guard_live)
	{
		frame->guard_live = false;
		host_lock_LockedRwLockWriteGuard_drop(&frame->guard);
	}
	if (frame->local_live)
	{
		frame->local_live = false;
		host_local_Local_drop(&frame->local, &TXN_SPILL_REPLAYED_TYPE);
	}
}

/* txn_spill_replayed_body_0 runs a step on REPLAYED under its lock. */
static void
txn_spill_replayed_body_0(void *frame_)
{
	txn_spill_replayed_frame_0_t *frame = frame_;

	frame->local = host_local_BackendLocal_get(&TXN_SPILL_REPLAYED.local, &TXN_SPILL_REPLAYED_TYPE);
	frame->local_live = true;
	frame->guard = host_lock_IoMutex_lock((HostLockIoMutex *) host_local_Local_Deref_deref(&frame->local,
																						   &TXN_SPILL_REPLAYED_TYPE),
										  &TXN_SPILL_REPLAYED_LOCK_TYPE, &frame->io->held);
	frame->guard_live = true;
	frame->step(frame->captures, host_lock_LockedRwLockWriteGuard_DerefMut_deref_mut(&frame->guard));
	txn_spill_replayed_pad_0(frame);
}

/* txn_spill_replayed_locked runs a step on the backend's REPLAYED under its lock. */
static void
txn_spill_replayed_locked(HostIoIo *io, void (*step) (void *captures, StdVec *replayed), void *captures)
{
	txn_spill_replayed_frame_0_t frame = {.io = io, .step = step, .captures = captures};

	std_landing(&frame, txn_spill_replayed_body_0, txn_spill_replayed_pad_0);
}

/* taken at Vec<u64> and IoMutex<Vec<u64>>, which spill.c alone uses. */
#define HOST_LOCK_SCOPE static
#define HOST_LOCK_DECLARE
#define HOST_LOCK_DEFINE
#define HOST_LOCK_TAKEN_TS VecU64
#define HOST_LOCK_TAKEN_T StdVec
#define HOST_LOCK_TAKEN_DEFAULT(place) std_vec_new((place), sizeof(uint64), _Alignof(uint64))
#define HOST_LOCK_TAKEN_DROP(place) std_vec_free_Checked((place), sizeof(uint64), _Alignof(uint64))
#define HOST_LOCK_TAKEN_TYPE TXN_SPILL_SPILLERS_LOCK_TYPE
#define HOST_LOCK_IO_MUTEX
#define HOST_LOCK_TS VecU64
#define HOST_LOCK_T StdVec
#define HOST_LOCK_DEFAULT(place) std_vec_new((place), sizeof(uint64), _Alignof(uint64))
#include "doltgres/host/lock_template.h"

/* txn_spill_IoMutexVecU64_drop is IoMutex<Vec<u64>>' drop glue: its value's. */
static void
txn_spill_IoMutexVecU64_drop(void *value)
{
	HostLockIoMutexVecU64 *self = value;

	std_vec_free_Checked(&self->value, sizeof(uint64), _Alignof(uint64));
}

/* TXN_SPILL_SPILLERS_TYPE is IoMutex<Vec<u64>>' size, Default, and drop. */
static const StdDefaultType TXN_SPILL_SPILLERS_TYPE = {sizeof(HostLockIoMutexVecU64),
	_Alignof(HostLockIoMutexVecU64), host_lock_IoMutexVecU64_Default_default,
txn_spill_IoMutexVecU64_drop};

/*
 * TXN_SPILL_FREE_SPILLERS is FREE_SPILLERS: the startup process's xids whose
 * free space map spills it listed, by its load of a checkpoint's runs or a
 * replayed spill, so that a replayed abort sweeps the states for its
 * transaction's spills only when it has some; its BackendLocal header and
 * the process's value, which follows it.
 */
static struct
{
	HostLocalBackendLocal local;
	HostLockIoMutexVecU64 process;
}			TXN_SPILL_FREE_SPILLERS = {HOST_LOCAL_BACKEND_LOCAL_NEW};

/* txn_spill_spillers_frame_0_t is the state of a step under FREE_SPILLERS' lock across its pad: the local and its guard, while live. */
typedef struct txn_spill_spillers_frame_0_t
{
	HostIoIo   *io;
	void		(*step) (void *captures, StdVec *spillers);
	void	   *captures;
	HostLocalLocal local;
	bool		local_live;
	HostLockLockedRwLockWriteGuard guard;
	bool		guard_live;
} txn_spill_spillers_frame_0_t;

/* txn_spill_spillers_pad_0 is the pad of a step under FREE_SPILLERS' lock: the guard, then the local, while live. */
static void
txn_spill_spillers_pad_0(void *frame_)
{
	txn_spill_spillers_frame_0_t *frame = frame_;

	if (frame->guard_live)
	{
		frame->guard_live = false;
		host_lock_LockedRwLockWriteGuard_drop(&frame->guard);
	}
	if (frame->local_live)
	{
		frame->local_live = false;
		host_local_Local_drop(&frame->local, &TXN_SPILL_SPILLERS_TYPE);
	}
}

/* txn_spill_spillers_body_0 runs a step on FREE_SPILLERS under its lock. */
static void
txn_spill_spillers_body_0(void *frame_)
{
	txn_spill_spillers_frame_0_t *frame = frame_;

	frame->local = host_local_BackendLocal_get(&TXN_SPILL_FREE_SPILLERS.local, &TXN_SPILL_SPILLERS_TYPE);
	frame->local_live = true;
	frame->guard = host_lock_IoMutex_lock((HostLockIoMutex *) host_local_Local_Deref_deref(&frame->local,
																						   &TXN_SPILL_SPILLERS_TYPE),
										  &TXN_SPILL_SPILLERS_LOCK_TYPE, &frame->io->held);
	frame->guard_live = true;
	frame->step(frame->captures, host_lock_LockedRwLockWriteGuard_DerefMut_deref_mut(&frame->guard));
	txn_spill_spillers_pad_0(frame);
}

/* txn_spill_spillers_locked runs a step on the process's FREE_SPILLERS under its lock. */
static void
txn_spill_spillers_locked(HostIoIo *io, void (*step) (void *captures, StdVec *spillers), void *captures)
{
	txn_spill_spillers_frame_0_t frame = {.io = io, .step = step, .captures = captures};

	std_landing(&frame, txn_spill_spillers_body_0, txn_spill_spillers_pad_0);
}

/* txn_spill_spillers_t is the steps' captures on FREE_SPILLERS: the xid, and whether the step found or recorded it. */
typedef struct txn_spill_spillers_t
{
	uint64		xid;
	bool		done;
} txn_spill_spillers_t;

/* txn_spill_spilled_free_closure_0 is spilled_free's step: the xid recorded once, done unless the list's growth failed. */
static void
txn_spill_spilled_free_closure_0(void *captures, StdVec *spillers)
{
	txn_spill_spillers_t *c = captures;

	for (size_t k = 0; k < spillers->len; k++)
		if (STD_VEC_AT(spillers, uint64, k) == c->xid)
		{
			c->done = true;
			return;
		}
	c->done = std_vec_try_reserve(spillers, 1, sizeof(uint64), _Alignof(uint64));
	if (c->done)
		std_vec_push_Checked(spillers, &c->xid, sizeof(uint64), _Alignof(uint64));
}

/* txn_spill_spilled_free records, in the startup process, an xid whose free space map spill it listed. */
bool
txn_spill_spilled_free(HostIoIo *io, uint64 xid, HostError *error)
{
	txn_spill_spillers_t captures = {xid};

	txn_spill_spillers_locked(io, txn_spill_spilled_free_closure_0, &captures);
	if (!captures.done)
		*error = host_Error_out_of_memory();
	return captures.done;
}

/* txn_spill_redo_aborted_closure_0 is redo_aborted's step on FREE_SPILLERS: the xid taken out, done when it was there. */
static void
txn_spill_redo_aborted_closure_0(void *captures, StdVec *spillers)
{
	txn_spill_spillers_t *c = captures;

	for (size_t k = 0; k < spillers->len; k++)
		if (STD_VEC_AT(spillers, uint64, k) == c->xid)
		{
			uint64		taken;

			std_vec_swap_remove(spillers, k, &taken, sizeof(uint64), _Alignof(uint64));
			c->done = true;
			return;
		}
}

/* txn_spill_marked_t is the edit's captures that marks a transaction's free space map spills: the xid and the end. */
typedef struct txn_spill_marked_t
{
	uint64		xid;
	uint64		end;
} txn_spill_marked_t;

/* txn_spill_marked_closure_0 marks each of a transaction's free space map spills with an abort record's end. */
static bool
txn_spill_marked_closure_0(void *captures, HostSharedLongIo *io, StdVec *files, HostError *error)
{
	const txn_spill_marked_t *c = captures;

	for (size_t k = 0; k < files->len; k++)
	{
		TxnSpillFile *file = &STD_VEC_AT(files, TxnSpillFile, k);

		if (file->kind == TXN_RUNS_KIND_SPILLED_FREE && file->xid == c->xid)
			file->passed = c->end;
	}
	return true;
}

/* txn_spill_redo_aborted_frame_0_t is redo_aborted's state across its pad: the swept states, a state opened, and its files, while live. */
typedef struct txn_spill_redo_aborted_frame_0_t
{
	HostIoIo   *io;
	txn_spill_marked_t marked;
	HostError  *error;
	bool		ok;
	/* swept is a Vec<(u64, u64)> of the swept states' locations. */
	StdVec		swept;
	bool		swept_live;
	HostSharedHeld held;
	bool		held_live;
	txn_TupleVecFileU64_t listed;
	bool		listed_live;
} txn_spill_redo_aborted_frame_0_t;

/* txn_spill_redo_aborted_pad_0 is redo_aborted's pad: the files, the state, and the swept list, while live. */
static void
txn_spill_redo_aborted_pad_0(void *frame_)
{
	txn_spill_redo_aborted_frame_0_t *frame = frame_;

	if (frame->listed_live)
	{
		frame->listed_live = false;
		std_vec_free_Checked(&frame->listed.f0, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	}
	if (frame->held_live)
	{
		frame->held_live = false;
		host_shared_Held_drop(&frame->held);
	}
	if (frame->swept_live)
	{
		frame->swept_live = false;
		std_vec_free_Checked(&frame->swept, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
	}
}

/* txn_spill_redo_aborted_body_0 is redo_aborted's body, for a transaction spilled_free recorded. */
static void
txn_spill_redo_aborted_body_0(void *frame_)
{
	txn_spill_redo_aborted_frame_0_t *frame = frame_;

	if (!host_shared_sweep(HOST_SHARED_IO_WITHIN(frame->io), &TXN_STATE_STATE_POOL_OPS, TXN_STATE_SPILLS_INDEX,
						   &frame->swept, frame->error))
		return;
	frame->swept_live = true;
	for (size_t k = 0; k < frame->swept.len; k++)
	{
		bool		some;
		bool		spilled = false;
		HostStorage storage;
		TxnStatePersistence persistence;
		HostSharedRecord record;

		if (!host_shared_open(HOST_SHARED_IO_WITHIN(frame->io), &TXN_STATE_STATE_POOL_OPS,
							  STD_VEC_AT(&frame->swept, std_TupleU64U64_t, k).f0, &some, &frame->held, frame->error))
		{
			txn_spill_redo_aborted_pad_0(frame);
			return;
		}
		if (!some)
			continue;
		frame->held_live = true;
		record = host_shared_Held_record(&frame->held);
		if (txn_state_described(record, &storage, &persistence))
		{
			if (!txn_spill_listed(&TXN_SPILL_STATE_HOME_OPS, frame->io, record, &storage, &frame->listed,
								  frame->error))
			{
				txn_spill_redo_aborted_pad_0(frame);
				return;
			}
			frame->listed_live = true;
			for (size_t j = 0; j < frame->listed.f0.len && !spilled; j++)
			{
				const TxnSpillFile *file = &STD_VEC_AT(&frame->listed.f0, TxnSpillFile, j);

				spilled = file->kind == TXN_RUNS_KIND_SPILLED_FREE && file->xid == frame->marked.xid;
			}
			frame->listed_live = false;
			std_vec_free_Checked(&frame->listed.f0, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
			if (spilled && !txn_spill_changed(&TXN_SPILL_STATE_HOME_OPS, frame->io, record, &storage,
											  txn_spill_marked_closure_0, &frame->marked, frame->error))
			{
				txn_spill_redo_aborted_pad_0(frame);
				return;
			}
		}
		frame->held_live = false;
		host_shared_Held_drop(&frame->held);
	}
	frame->ok = true;
	txn_spill_redo_aborted_pad_0(frame);
}

/*
 * txn_spill_redo_aborted marks a replayed aborted transaction's free space
 * map spills with its abort record's end, as its abort marked them on its
 * primary, so a standby's folds pass them where its primary's do; it sweeps
 * the states that list spills only for a transaction spilled_free recorded.
 */
bool
txn_spill_redo_aborted(HostIoIo *io, uint64 xid, HostLsn end, HostError *error)
{
	txn_spill_spillers_t known = {xid};
	txn_spill_redo_aborted_frame_0_t frame = {.io = io, .marked = {xid, end}, .error = error};

	txn_spill_spillers_locked(io, txn_spill_redo_aborted_closure_0, &known);
	if (!known.done)
		return true;
	std_landing(&frame, txn_spill_redo_aborted_body_0, txn_spill_redo_aborted_pad_0);
	return frame.ok;
}

/* txn_spill_relieved_own_closure_0 is relieved_own's step on REPLAYED: the tree a replayed transaction wrote the most to, the last of equals as max_by_key takes it. */
static void
txn_spill_relieved_own_closure_0(void *captures, StdVec *replayed)
{
	txn_TupleStorageOfU64U64_t *largest = captures;

	for (size_t k = 0; k < replayed->len; k++)
		if (k == 0 || STD_VEC_AT(replayed, txn_TupleStorageOfU64U64_t, k).f2 >= largest->f2)
			*largest = STD_VEC_AT(replayed, txn_TupleStorageOfU64U64_t, k);
}

/*
 * txn_spill_relieved_own spills, as relieved does, the trees of the
 * backend's own transaction, or for the startup process a replayed
 * transaction, alone, the most first, until one spills a key. A process
 * whose spill's part is pending spills nothing.
 */
bool
txn_spill_relieved_own(HostIoIo *io, bool *result, HostError *error)
{
	bool		some;
	TxnSpillPended pended;

	*result = false;
	if (!txn_spill_pending(io, &some, &pended, error))
		return false;
	if (some)
		return true;
	for (;;)
	{
		txn_TupleStorageOfU64U64_t largest = {0};
		bool		spilled;

		if (!txn_written_largest(io, &some, &largest, error))
			return false;
		if (!some)
			txn_spill_replayed_locked(io, txn_spill_relieved_own_closure_0, &largest);
		if (largest.f2 == 0)
			return true;
		if (!txn_spill_spilled(io, &largest.f0, largest.f1, largest.f3, &spilled, error))
			return false;
		if (spilled)
		{
			*result = true;
			return true;
		}
	}
}

/* txn_spill_replayed_captures_t is replayed's step's captures: the tree, its xid, the bytes, and the count it answers. */
typedef struct txn_spill_replayed_captures_t
{
	const HostStorage *storage;
	TxnWrittenOf of;
	uint64		xid;
	uint64		bytes;
	uint64	   *result;
	bool		full;
} txn_spill_replayed_captures_t;

/* txn_spill_replayed_closure_0 is replayed's step on REPLAYED: the tree's count grown, or a count begun. */
static void
txn_spill_replayed_closure_0(void *captures, StdVec *replayed)
{
	txn_spill_replayed_captures_t *c = captures;
	txn_TupleStorageOfU64U64_t added = {*c->storage, c->of, c->bytes, c->xid};

	for (size_t k = 0; k < replayed->len; k++)
	{
		txn_TupleStorageOfU64U64_t *count = &STD_VEC_AT(replayed, txn_TupleStorageOfU64U64_t, k);

		if (host_Storage_PartialEq_eq(&count->f0, c->storage) && count->f1 == c->of && count->f3 == c->xid)
		{
			count->f2 = STD_ADD(u64, count->f2, c->bytes, 879, 13);
			*c->result = count->f2;
			return;
		}
	}
	if (!std_vec_try_reserve(replayed, 1, sizeof(added), _Alignof(txn_TupleStorageOfU64U64_t)))
	{
		c->full = true;
		return;
	}
	std_vec_push_Checked(replayed, &added, sizeof(added), _Alignof(txn_TupleStorageOfU64U64_t));
	*c->result = c->bytes;
}

/* txn_spill_replayed adds the bytes of a version redo pushes for a top-level transaction to a storage's tree's count and returns the count. */
bool
txn_spill_replayed(HostIoIo *io, const HostStorage *storage, TxnWrittenOf of, uint64 xid, size_t bytes,
				   uint64 *result, HostError *error)
{
	txn_spill_replayed_captures_t captures = {storage, of, xid, (uint64) bytes, result};

	txn_spill_replayed_locked(io, txn_spill_replayed_closure_0, &captures);
	if (captures.full)
	{
		*error = host_Error_out_of_memory();
		return false;
	}
	return true;
}

/* txn_spill_unreplayed_captures_t is unreplayed's step's captures: the tree and its xid. */
typedef struct txn_spill_unreplayed_captures_t
{
	const HostStorage *storage;
	TxnWrittenOf of;
	uint64		xid;
} txn_spill_unreplayed_captures_t;

/* txn_spill_unreplayed_closure_0 is unreplayed's step on REPLAYED: the tree's count taken out. */
static void
txn_spill_unreplayed_closure_0(void *captures, StdVec *replayed)
{
	txn_spill_unreplayed_captures_t *c = captures;
	size_t		kept = 0;

	for (size_t k = 0; k < replayed->len; k++)
	{
		txn_TupleStorageOfU64U64_t count = STD_VEC_AT(replayed, txn_TupleStorageOfU64U64_t, k);

		if (!(host_Storage_PartialEq_eq(&count.f0, c->storage) && count.f1 == c->of && count.f3 == c->xid))
			STD_VEC_AT(replayed, txn_TupleStorageOfU64U64_t, kept++) = count;
	}
	replayed->len = kept;
}

/* txn_spill_unreplayed zeroes a replayed transaction's count of a storage's tree once a spill of it ran. */
static void
txn_spill_unreplayed(HostIoIo *io, const HostStorage *storage, TxnWrittenOf of, uint64 xid)
{
	txn_spill_unreplayed_captures_t captures = {storage, of, xid};

	txn_spill_replayed_locked(io, txn_spill_unreplayed_closure_0, &captures);
}

static bool txn_spill_spilled_from(const TxnSpillHomeOps *home, HostIoIo *io, const HostSharedHeld *held,
								   const HostStorage *storage, TxnWrittenOf of, uint64 xid, bool oldest,
								   bool *result, HostError *error);

/* txn_spill_spilled_turn_captures_t is spilled_turn's step's captures: the tree, the xid, and the answer. */
typedef struct txn_spill_spilled_turn_captures_t
{
	HostIoIo   *io;
	const HostStorage *storage;
	TxnWrittenOf of;
	uint64		xid;
	bool	   *result;
} txn_spill_spilled_turn_captures_t;

/* txn_spill_spilled_turn_none ends a spill of a tree that spills nothing: its count taken down, as an identity tree's or a gone home's is. */
static bool
txn_spill_spilled_turn_none(const txn_spill_spilled_turn_captures_t *c, HostError *error)
{
	if (!txn_written_spilled(c->io, c->storage, c->of, error))
		return false;
	txn_spill_unreplayed(c->io, c->storage, c->of, c->xid);
	return true;
}

/* txn_spill_spilled_turn_closure_0 is spilled_turn's step: the spill from the tree's home. */
static bool
txn_spill_spilled_turn_closure_0(void *captures, const TxnSpillHomeOps *home, const HostSharedHeld *held,
								 HostError *error)
{
	txn_spill_spilled_turn_captures_t *c = captures;

	if (c->of == TXN_WRITTEN_OF_IDENTITY)
		return txn_spill_spilled_turn_none(c, error);
	return txn_spill_spilled_from(home, c->io, held, c->storage, c->of, c->xid, false, c->result, error);
}

/* txn_spill_spilled_turn spills a storage's tree's versions of a transaction from the tree's home, by a process whose turn it is; a database's identity tree never spills. */
static bool
txn_spill_spilled_turn(HostIoIo *io, const HostStorage *storage, TxnWrittenOf of, uint64 xid, bool *result,
					   HostError *error)
{
	txn_spill_spilled_turn_captures_t captures = {io, storage, of, xid, result};
	bool		some = false;

	*result = false;
	if (!txn_spill_settle_pending(io, error))
		return false;
	if (!txn_spill_at_found(io, storage, txn_spill_spilled_turn_closure_0, &captures, &some, error))
		return false;
	return some || txn_spill_spilled_turn_none(&captures, error);
}

/* txn_spill_spilled spills a transaction's own versions in one storage's tree, under the transaction's turn, and reports whether it spilled a key. */
bool
txn_spill_spilled(HostIoIo *io, const HostStorage *storage, TxnWrittenOf of, uint64 xid, bool *result,
				  HostError *error)
{
	HostSharedMine mine;
	TxnSpillTurn turn;
	bool		some;
	bool		ok;

	if (!txn_spill_mine(io, &mine, error))
		return false;
	if (host_shared_Mine_load(&mine, TXN_SPILL_PENDING_TOP) != xid)
		return txn_spill_spilled_turn(io, storage, of, xid, result, error);
	if (!txn_spill_Turn_own(io, &some, &turn, error))
		return false;
	if (!some)
	{
		*result = false;
		return txn_written_spilled(io, storage, of, error);
	}
	ok = txn_spill_spilled_turn(io, storage, of, xid, result, error);
	txn_spill_Turn_given(&turn, 0);
	return ok;
}

/* txn_TupleVecU8VecHandle_t is a spill's taken key, (Vec<u8>, Vec<Handle>): the key and the handles of the versions it took. */
typedef struct txn_TupleVecU8VecHandle_t
{
	StdVec		f0;
	StdVec		f1;
} txn_TupleVecU8VecHandle_t;

/* txn_spill_taken_drop drops a Vec<(Vec<u8>, Vec<Handle>)>. */
static void
txn_spill_taken_drop(StdVec *taken)
{
	for (size_t k = 0; k < taken->len; k++)
	{
		txn_TupleVecU8VecHandle_t *key = &STD_VEC_AT(taken, txn_TupleVecU8VecHandle_t, k);

		std_vec_free_Checked(&key->f0, 1, 1);
		std_vec_free_Checked(&key->f1, sizeof(TxnMemtableVersionHandle), _Alignof(TxnMemtableVersionHandle));
	}
	std_vec_free_Checked(taken, sizeof(txn_TupleVecU8VecHandle_t), _Alignof(txn_TupleVecU8VecHandle_t));
}

/* txn_spill_tuples_drop drops a Vec<(u64, Vec<u8>)> of a spill's tuple ids and their rows' keys. */
static void
txn_spill_tuples_drop(StdVec *tuples)
{
	for (size_t k = 0; k < tuples->len; k++)
		std_vec_free_Checked(&STD_VEC_AT(tuples, txn_TupleU64VecU8_t, k).f1, 1, 1);
	std_vec_free_Checked(tuples, sizeof(txn_TupleU64VecU8_t), _Alignof(txn_TupleU64VecU8_t));
}

/* txn_spill_removed_keys_frame_0_t is removed_keys' state across its pad: the keys it hands the removal, while live. */
typedef struct txn_spill_removed_keys_frame_0_t
{
	const TxnSpillHomeOps *home;
	HostIoIo   *io;
	HostSharedRecord record;
	TxnWrittenOf of;
	const StdVec *taken;
	HostError  *error;
	bool		ok;
	/* keys is a Vec<(&[u8], &[Handle])>. */
	StdVec		keys;
	bool		keys_live;
} txn_spill_removed_keys_frame_0_t;

/* txn_spill_removed_keys_pad_0 is removed_keys' pad: the keys, while live. */
static void
txn_spill_removed_keys_pad_0(void *frame_)
{
	txn_spill_removed_keys_frame_0_t *frame = frame_;

	if (frame->keys_live)
	{
		frame->keys_live = false;
		std_vec_free_Checked(&frame->keys, sizeof(TxnMemtableTableRemoving), _Alignof(TxnMemtableTableRemoving));
	}
}

/* txn_spill_removed_keys_body_0 is removed_keys' body. */
static void
txn_spill_removed_keys_body_0(void *frame_)
{
	txn_spill_removed_keys_frame_0_t *frame = frame_;

	std_vec_with_capacity_Checked(&frame->keys, frame->taken->len, sizeof(TxnMemtableTableRemoving),
								  _Alignof(TxnMemtableTableRemoving));
	frame->keys_live = true;
	for (size_t k = 0; k < frame->taken->len; k++)
	{
		const txn_TupleVecU8VecHandle_t *key = &STD_VEC_AT(frame->taken, txn_TupleVecU8VecHandle_t, k);
		TxnMemtableTableRemoving removing = {{key->f0.ptr, key->f0.len}, key->f1.ptr, key->f1.len};

		std_vec_push_Checked(&frame->keys, &removing, sizeof(removing), _Alignof(TxnMemtableTableRemoving));
	}
	frame->ok = txn_spill_Home_removed(frame->home, frame->io, frame->record, frame->of, frame->keys.ptr,
									   frame->keys.len, frame->error);
	txn_spill_removed_keys_pad_0(frame);
}

/* txn_spill_removed_keys takes the versions a spill took out of each key's chain in one of a home's trees. */
static bool
txn_spill_removed_keys(const TxnSpillHomeOps *home, HostIoIo *io, HostSharedRecord record, TxnWrittenOf of,
					   const StdVec *taken, HostError *error)
{
	txn_spill_removed_keys_frame_0_t frame = {.home = home, .io = io, .record = record, .of = of, .taken = taken,
	.error = error};

	std_landing(&frame, txn_spill_removed_keys_body_0, txn_spill_removed_keys_pad_0);
	return frame.ok;
}

/* txn_spill_op_tid returns the tuple id a row version's op holds, the row a put, an add, an added row, or an update names. */
static bool
txn_spill_op_tid(TxnMemtableVersionOpRef op, uint64 *result)
{
	switch (op.tag)
	{
		case TXN_MEMTABLE_VERSION_OP_REF_PUT:
			*result = op.u.put.tuple.f0;
			return true;
		case TXN_MEMTABLE_VERSION_OP_REF_ADD:
			*result = op.u.add.tuple.f0;
			return true;
		case TXN_MEMTABLE_VERSION_OP_REF_ADD_ROW:
			*result = op.u.add_row.tuple.f0;
			return true;
		case TXN_MEMTABLE_VERSION_OP_REF_UPDATE_ROW:
			*result = op.u.update_row.f1.tuple.f0;
			return true;
		default:
			return false;
	}
}

/* txn_spill_tid returns the tuple id a row version's record holds, as op_tid gives it. */
static bool
txn_spill_tid(const uint8 *record, size_t record_len, uint64 *result)
{
	TxnMemtableVersionRecord stored = txn_memtable_version_Record_of(record, record_len);

	return txn_spill_op_tid(txn_memtable_version_Record_op(&stored), result);
}

/* txn_spill_numbered returns the number of a transaction's next spill file of a storage's kind: one past its newest listed. */
static bool
txn_spill_numbered(const TxnSpillHomeOps *home, HostIoIo *io, HostSharedRecord state, const HostStorage *storage,
				   uint64 xid, TxnRunsKind kind, uint32 *result, HostError *error)
{
	txn_TupleVecFileU64_t listed;

	if (!txn_spill_listed(home, io, state, storage, &listed, error))
		return false;
	*result = 0;
	for (size_t k = 0; k < listed.f0.len; k++)
	{
		const TxnSpillFile *file = &STD_VEC_AT(&listed.f0, TxnSpillFile, k);

		if (file->xid == xid && file->kind == kind)
			*result = Max(*result, STD_ADD(u32, file->number, 1, 1393, 23));
	}
	std_vec_free_Checked(&listed.f0, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	return true;
}

/* txn_spill_from_frame_0_t is spilled_from's state across its pad: the spill's directory, its writing, the keys and tuple ids it took, its filter, and its tuple-id file's tids, while live. */
typedef struct txn_spill_from_frame_0_t
{
	const TxnSpillHomeOps *home;
	HostIoIo   *io;
	const HostSharedHeld *held;
	const HostStorage *storage;
	TxnWrittenOf of;
	uint64		xid;

	/*
	 * oldest is the budget's committed arm's spill, of keys whose oldest
	 * versions are the transaction's alone.
	 */
	bool		oldest;
	bool	   *result;
	HostError  *error;
	bool		ok;
	TxnRunsKind kind;
	StdPathBuf	dir;
	bool		dir_live;
	TxnRunsWriting *writing;
	bool		writing_live;
	/* taken is a Vec<(Vec<u8>, Vec<Handle>)>. */
	StdVec		taken;
	bool		taken_live;
	/* tuples is a Vec<(u64, Vec<u8>)>. */
	StdVec		tuples;
	bool		tuples_live;
	/* records is a Vec<&[u8]> of a key's records. */
	StdVec		records;
	bool		records_live;
	TxnSpillFilter filter;
	bool		filter_live;
	/* tids is a Vec<u64> of the tuple-id file's tids. */
	StdVec		tids;
	bool		tids_live;
} txn_spill_from_frame_0_t;

/* txn_spill_from_pad_0 is spilled_from's pad: what it holds, while live. */
static void
txn_spill_from_pad_0(void *frame_)
{
	txn_spill_from_frame_0_t *frame = frame_;

	if (frame->tids_live)
	{
		frame->tids_live = false;
		std_vec_free_Checked(&frame->tids, sizeof(uint64), _Alignof(uint64));
	}
	if (frame->filter_live)
	{
		frame->filter_live = false;
		std_vec_free_Checked(&frame->filter.words, sizeof(uint64), _Alignof(uint64));
	}
	if (frame->records_live)
	{
		frame->records_live = false;
		std_vec_free_Checked(&frame->records, sizeof(StdBytes), _Alignof(StdBytes));
	}
	if (frame->tuples_live)
	{
		frame->tuples_live = false;
		txn_spill_tuples_drop(&frame->tuples);
	}
	if (frame->taken_live)
	{
		frame->taken_live = false;
		txn_spill_taken_drop(&frame->taken);
	}
	if (frame->writing_live)
	{
		frame->writing_live = false;
		txn_runs_Writing_drop(frame->writing);
	}
	if (frame->dir_live)
	{
		frame->dir_live = false;
		std_vec_free_Checked(&frame->dir.bytes, 1, 1);
	}
}

/*
 * txn_spill_from_closure_0 is spilled_from's walk's step: of a key whose
 * chain holds only the transaction's versions, or, for the committed arm,
 * whose oldest versions are the transaction's alone, those versions written,
 * their tuple ids and their handles taken, so every spilled version stays
 * older than the versions the chain keeps.
 */
static bool
txn_spill_from_closure_0(void *captures, HostIoIo *io, const uint8 *key, size_t key_len, const StdVec *records,
						 size_t records_len, const txn_TupleHandleU64Bool_t *handles, size_t handles_len,
						 HostError *error)
{
	txn_spill_from_frame_0_t *frame = captures;
	txn_TupleVecU8VecHandle_t took;
	size_t		below = 0;
	size_t		kept = 0;

	for (size_t k = handles_len; k > 0 && below == 0; k--)
		if (handles[k - 1].f1 != frame->xid)
			below = k;
	if (below == handles_len || (below > 0 && !frame->oldest))
		return true;
	for (size_t k = 0; k < below; k++)
	{
		if (handles[k].f1 == frame->xid)
			return true;
		kept += !handles[k].f2;
	}
	records += kept;
	records_len -= kept;
	handles += below;
	handles_len -= below;
	if (records_len > 0)
	{
		frame->records.len = 0;
		for (size_t k = 0; k < records_len; k++)
		{
			StdBytes	record = {records[k].ptr, records[k].len};

			std_vec_push_Checked(&frame->records, &record, sizeof(record), _Alignof(StdBytes));
		}
		if (!txn_runs_Writing_key(frame->writing, HOST_IO_IO_IO_DOORS(io), key, key_len, frame->records.ptr, frame->records.len, error))
			return false;
	}
	for (size_t k = 0; frame->kind == TXN_RUNS_KIND_SPILLED_ROWS && k < records_len; k++)
	{
		txn_TupleU64VecU8_t tuple;

		if (!txn_spill_tid(records[k].ptr, records[k].len, &tuple.f0))
			continue;
		if (!std_vec_try_reserve(&frame->tuples, 1, sizeof(tuple), _Alignof(txn_TupleU64VecU8_t)))
		{
			*error = host_Error_out_of_memory();
			return false;
		}
		std_slice_to_vec_Checked(&tuple.f1, key, key_len, 1, 1);
		std_vec_push_Checked(&frame->tuples, &tuple, sizeof(tuple), _Alignof(txn_TupleU64VecU8_t));
	}
	if (!std_vec_try_reserve(&frame->taken, 1, sizeof(took), _Alignof(txn_TupleVecU8VecHandle_t)))
	{
		*error = host_Error_out_of_memory();
		return false;
	}
	std_vec_with_capacity_Checked(&took.f1, handles_len, sizeof(TxnMemtableVersionHandle),
								  _Alignof(TxnMemtableVersionHandle));
	for (size_t k = 0; k < handles_len; k++)
		std_vec_push_Checked(&took.f1, &handles[k].f0, sizeof(handles[k].f0), _Alignof(TxnMemtableVersionHandle));
	std_slice_to_vec_Checked(&took.f0, key, key_len, 1, 1);
	std_vec_push_Checked(&frame->taken, &took, sizeof(took), _Alignof(txn_TupleVecU8VecHandle_t));
	return true;
}

static bool txn_spill_tupled(const TxnSpillHomeOps *home, HostIoIoDoors io, HostSharedRecord record,
							 const HostStorage *storage, const StdPathBuf *dir, uint64 xid, uint32 number,
							 StdVec *tuples, TxnSpillFile *result_0, StdVec *result_1, HostError *error);
static bool txn_spill_merged(const TxnSpillHomeOps *home, HostIoIo *io, HostSharedRecord record,
							 const HostStorage *storage, uint64 xid, TxnRunsKind kind, TxnRunsTail tail,
							 HostError *error);

/* txn_spill_from_ended ends a spill that took its tree's count down, as every way out of spilled_from does but the committed arm's, which leaves the count to its transaction's spills. */
static bool
txn_spill_from_ended(txn_spill_from_frame_0_t *frame)
{
	if (frame->oldest)
		return true;
	if (!txn_written_spilled(frame->io, frame->storage, frame->of, frame->error))
		return false;
	txn_spill_unreplayed(frame->io, frame->storage, frame->of, frame->xid);
	return true;
}

/*
 * txn_spill_from_listed pends, unchains, and lists a spill's files, then
 * settles its slot and merges the transaction's files, but the committed
 * arm's; the startup process records a free space map's spiller.
 */
static bool
txn_spill_from_listed(txn_spill_from_frame_0_t *frame, HostSharedRecord record, const TxnSpillFile *listing,
					  size_t listing_len, TxnRunsTail tail)
{
	txn_TupleU64KindU32OptionU32_t none = {0};

	if (!txn_spill_pend(frame->home, frame->io, record, frame->storage, &listing[0], listing_len > 1, false, 0,
						frame->error) ||
		(frame->kind == TXN_RUNS_KIND_SPILLED_FREE && host_wal_stand(HOST_IO_IO_IO_DOORS(frame->io)).recovering &&
		 !txn_spill_spilled_free(frame->io, frame->xid, frame->error)) ||
		!host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-spill-pended", frame->error) ||
		!txn_spill_removed_keys(frame->home, frame->io, record, frame->of, &frame->taken, frame->error) ||
		(frame->tids_live &&
		 !frame->home->untupled(frame->io, record, frame->tids.ptr, frame->tids.len, frame->error)) ||
		!host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-spill-linking", frame->error) ||
		!txn_spill_linked(frame->home, frame->io, record, frame->storage, listing, listing_len, false, none,
						  frame->error) ||
		!host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-spill-linked", frame->error) ||
		!txn_spill_settled(frame->home, frame->io, &record, frame->error))
		return false;
	return frame->oldest ||
		(txn_spill_merged(frame->home, frame->io, record, frame->storage, frame->xid, frame->kind, tail,
						  frame->error) &&
		 txn_spill_from_ended(frame));
}

/* txn_spill_from_body_0 is spilled_from's body. */
static void
txn_spill_from_body_0(void *frame_)
{
	txn_spill_from_frame_0_t *frame = frame_;
	HostSharedRecord record = host_shared_Held_record(frame->held);
	TxnRunsTail tail;
	TxnRunsNames names;
	StdStr		parent;
	ChunksErrorError failed;
	uint32		number;
	bool		some;
	RepoRunFile run;
	TxnSpillFile listing[2] = {0};

	*frame->result = false;
	if (!frame->home->kind(frame->io, frame->held, frame->of, &frame->kind, &tail, frame->error) ||
		!txn_spill_names(frame->home, record, frame->storage, frame->kind, frame->xid, &names, frame->error))
		return;
	txn_runs_spill_directory(frame->storage->database, frame->home->own(record), &frame->dir);
	frame->dir_live = true;
	if (!std_path_parent(frame->dir.bytes.ptr, frame->dir.bytes.len, &parent))
	{
		parent.ptr = frame->dir.bytes.ptr;
		parent.len = frame->dir.bytes.len;
	}
	if (!chunks_file_create_dir(HOST_IO_IO_IO_DOORS(frame->io), parent.ptr, parent.len, &failed) ||
		!chunks_file_create_dir(HOST_IO_IO_IO_DOORS(frame->io), frame->dir.bytes.ptr, frame->dir.bytes.len, &failed))
	{
		*frame->error = txn_base_storage(&failed);
		txn_spill_from_pad_0(frame);
		return;
	}
	if (!txn_spill_numbered(frame->home, frame->io, record, frame->storage, frame->xid, frame->kind, &number,
							frame->error))
	{
		txn_spill_from_pad_0(frame);
		return;
	}
	if (frame->oldest)
		number = frame->home->ended_number(record, number);
	frame->writing = txn_runs_Writing_spill(frame->dir.bytes.ptr, frame->dir.bytes.len, &names, tail, number);
	frame->writing_live = true;
	std_vec_new(&frame->taken, sizeof(txn_TupleVecU8VecHandle_t), _Alignof(txn_TupleVecU8VecHandle_t));
	frame->taken_live = true;
	std_vec_new(&frame->tuples, sizeof(txn_TupleU64VecU8_t), _Alignof(txn_TupleU64VecU8_t));
	frame->tuples_live = true;
	std_vec_new(&frame->records, sizeof(StdBytes), _Alignof(StdBytes));
	frame->records_live = true;
	if (!txn_spill_Home_copied(frame->home, frame->io, record, frame->of, txn_spill_from_closure_0, frame,
							   frame->error) ||
		!txn_spill_Filter_with(frame->taken.len, &frame->filter, frame->error))
	{
		txn_spill_from_pad_0(frame);
		return;
	}
	frame->filter_live = true;
	for (size_t k = 0; k < frame->taken.len; k++)
	{
		const StdVec *key = &STD_VEC_AT(&frame->taken, txn_TupleVecU8VecHandle_t, k).f0;

		txn_spill_Filter_add(&frame->filter, key->ptr, key->len);
	}
	if (!txn_runs_Writing_end(frame->writing, HOST_IO_IO_IO_DOORS(frame->io), frame->filter.words.ptr, frame->filter.words.len, &some,
							  &run, frame->error))
	{
		txn_spill_from_pad_0(frame);
		return;
	}
	if (!some)
	{
		frame->ok = txn_spill_removed_keys(frame->home, frame->io, record, frame->of, &frame->taken, frame->error) &&
			txn_spill_from_ended(frame);
		txn_spill_from_pad_0(frame);
		return;
	}
	listing[0].xid = frame->xid;
	listing[0].kind = frame->kind;
	listing[0].number = number;
	listing[0].len = run.len;
	listing[0].hash = run.hash;
	listing[0].bits = txn_spill_Filter_bits(&frame->filter);
	if (frame->tuples.len > 0)
	{
		frame->tuples_live = false;
		if (!txn_spill_tupled(frame->home, HOST_IO_IO_IO_DOORS(frame->io), record, frame->storage, &frame->dir, frame->xid, number,
							  &frame->tuples, &listing[1], &frame->tids, frame->error))
		{
			txn_spill_from_pad_0(frame);
			return;
		}
		frame->tids_live = true;
	}
	if (!txn_spill_from_listed(frame, record, listing, frame->tids_live ? 2 : 1, tail))
	{
		txn_spill_from_pad_0(frame);
		return;
	}
	*frame->result = true;
	frame->ok = true;
	txn_spill_from_pad_0(frame);
}

/*
 * txn_spill_spilled_from spills a tree's versions of a transaction from its
 * home: it walks the tree and writes, in key order, each key whose chain
 * holds only the transaction's versions, or, for the budget's committed arm
 * (oldest), the oldest versions in the memtable that are the transaction's
 * alone, to a new spill file with its filter, and a row spill's tuple ids to
 * a tuple-id file of its number; it names the files in the process's slot,
 * takes the versions out of the memtable, lists the files in the storage's
 * manifest, clears the slot, and merges the transaction's files. It zeroes
 * the tree's count, and reports whether it spilled a key. The committed arm
 * takes its file's number through the home's ended_number, since a fold's
 * residual numbers the same transaction's files, and neither merges nor
 * zeroes a count.
 */
static bool
txn_spill_spilled_from(const TxnSpillHomeOps *home, HostIoIo *io, const HostSharedHeld *held,
					   const HostStorage *storage, TxnWrittenOf of, uint64 xid, bool oldest, bool *result,
					   HostError *error)
{
	txn_spill_from_frame_0_t frame = {.home = home, .io = io, .held = held, .storage = storage, .of = of, .xid = xid,
	.oldest = oldest, .result = result, .error = error};

	std_landing(&frame, txn_spill_from_body_0, txn_spill_from_pad_0);
	return frame.ok;
}

/* txn_TupleU64U32U64_t is helped's holder, (u64, u32, u64): its unspilled bytes, its process, and its top-level xid. */
typedef struct txn_TupleU64U32U64_t
{
	uint64		f0;
	uint32		f1;
	uint64		f2;
} txn_TupleU64U32U64_t;

/* txn_TupleStorageOf_t is swept's tree, (Storage, Of): a storage and one of its trees. */
typedef struct txn_TupleStorageOf_t
{
	HostStorage f0;
	TxnWrittenOf f1;
} txn_TupleStorageOf_t;

/* txn_spill_TupleU64VecU8_lt is tupled's sort key's PartialOrd::lt: by tuple id. */
static bool
txn_spill_TupleU64VecU8_lt(void *captures, const void *a, const void *b)
{
	return ((const txn_TupleU64VecU8_t *) a)->f0 < ((const txn_TupleU64VecU8_t *) b)->f0;
}

/* The sort of a spill's tuple ids with their rows' keys: sort_unstable_by_key at (u64, Vec<u8>), by tid. */
#define STD_SORT_SCOPE static
#define STD_SORT_DECLARE
#define STD_SORT_DEFINE
#define STD_SORT_TS TupleU64VecU8
#define STD_SORT_T txn_TupleU64VecU8_t
#define STD_SORT_FREEZE 1
#define STD_SORT_COPY 0
#define STD_SORT_BRANCHLESS 1
#define STD_SORT_LT txn_spill_TupleU64VecU8_lt
#include "doltgres/std/sort_template.h"

/* txn_spill_tupled_frame_0_t is tupled's state across its pad: the tuples it takes, the tids, their keys, the filter, and the writing, while live. */
typedef struct txn_spill_tupled_frame_0_t
{
	const TxnSpillHomeOps *home;
	HostIoIoDoors io;
	HostSharedRecord record;
	const HostStorage *storage;
	const StdPathBuf *dir;
	uint64		xid;
	uint32		number;
	StdVec		tuples;
	TxnSpillFile *result_0;
	StdVec	   *result_1;
	HostError  *error;
	bool		ok;
	bool		tuples_live;
	bool		result_1_live;
	/* keys is a Vec<[u8; 8]>. */
	StdVec		keys;
	bool		keys_live;
	TxnSpillFilter filter;
	bool		filter_live;
	TxnRunsWriting *writing;
	bool		writing_live;
} txn_spill_tupled_frame_0_t;

/* txn_spill_tupled_pad_0 is tupled's pad: what it holds, while live. */
static void
txn_spill_tupled_pad_0(void *frame_)
{
	txn_spill_tupled_frame_0_t *frame = frame_;

	if (frame->writing_live)
	{
		frame->writing_live = false;
		txn_runs_Writing_drop(frame->writing);
	}
	if (frame->filter_live)
	{
		frame->filter_live = false;
		std_vec_free_Checked(&frame->filter.words, sizeof(uint64), _Alignof(uint64));
	}
	if (frame->keys_live)
	{
		frame->keys_live = false;
		std_vec_free_Checked(&frame->keys, 8, 1);
	}
	if (frame->result_1_live)
	{
		frame->result_1_live = false;
		std_vec_free_Checked(frame->result_1, sizeof(uint64), _Alignof(uint64));
	}
	if (frame->tuples_live)
	{
		frame->tuples_live = false;
		txn_spill_tuples_drop(&frame->tuples);
	}
}

/* txn_spill_tupled_body_0 is tupled's body. */
static void
txn_spill_tupled_body_0(void *frame_)
{
	txn_spill_tupled_frame_0_t *frame = frame_;
	size_t		kept = 0;
	TxnRunsNames names;
	TxnRunsTail none = {.tag = TXN_RUNS_TAIL_NONE};
	bool		some;
	RepoRunFile run;

	std_slice_sort_unstable_TupleU64VecU8(frame->tuples.ptr, frame->tuples.len);
	for (size_t k = 0; k < frame->tuples.len; k++)
	{
		txn_TupleU64VecU8_t *tuple = &STD_VEC_AT(&frame->tuples, txn_TupleU64VecU8_t, k);

		if (kept > 0 && STD_VEC_AT(&frame->tuples, txn_TupleU64VecU8_t, kept - 1).f0 == tuple->f0)
			std_vec_free_Checked(&tuple->f1, 1, 1);
		else
			STD_VEC_AT(&frame->tuples, txn_TupleU64VecU8_t, kept++) = *tuple;
	}
	frame->tuples.len = kept;
	std_vec_with_capacity_Checked(frame->result_1, kept, sizeof(uint64), _Alignof(uint64));
	frame->result_1_live = true;
	std_vec_with_capacity_Checked(&frame->keys, kept, 8, 1);
	frame->keys_live = true;
	for (size_t k = 0; k < kept; k++)
	{
		uint64		tid = STD_VEC_AT(&frame->tuples, txn_TupleU64VecU8_t, k).f0;
		uint8		key[8];

		std_vec_push_Checked(frame->result_1, &tid, sizeof(tid), _Alignof(uint64));
		std_u64_to_be_bytes(tid, key);
		std_vec_push_Checked(&frame->keys, key, 8, 1);
	}
	if (!txn_spill_Filter_with(kept, &frame->filter, frame->error))
	{
		txn_spill_tupled_pad_0(frame);
		return;
	}
	frame->filter_live = true;
	for (size_t k = 0; k < kept; k++)
		txn_spill_Filter_add(&frame->filter, (const uint8 *) frame->keys.ptr + k * 8, 8);
	if (!txn_spill_names(frame->home, frame->record, frame->storage, TXN_RUNS_KIND_SPILLED_TUPLES, frame->xid, &names,
						 frame->error))
	{
		txn_spill_tupled_pad_0(frame);
		return;
	}
	frame->writing = txn_runs_Writing_spill(frame->dir->bytes.ptr, frame->dir->bytes.len, &names, none, frame->number);
	frame->writing_live = true;
	for (size_t k = 0; k < kept; k++)
	{
		const StdVec *row = &STD_VEC_AT(&frame->tuples, txn_TupleU64VecU8_t, k).f1;
		StdBytes	record = {row->ptr, row->len};

		if (!txn_runs_Writing_key(frame->writing, frame->io, (const uint8 *) frame->keys.ptr + k * 8, 8, &record, 1,
								  frame->error))
		{
			txn_spill_tupled_pad_0(frame);
			return;
		}
	}
	if (!txn_runs_Writing_end(frame->writing, frame->io, frame->filter.words.ptr, frame->filter.words.len, &some, &run,
							  frame->error))
	{
		txn_spill_tupled_pad_0(frame);
		return;
	}
	if (!some)
	{
		*frame->error = txn_spill_internal("a spill's tuple ids wrote no file");
		txn_spill_tupled_pad_0(frame);
		return;
	}
	*frame->result_0 = (TxnSpillFile)
	{
		.xid = frame->xid, .kind = TXN_RUNS_KIND_SPILLED_TUPLES, .number = frame->number, .len = run.len,
			.hash = run.hash, .bits = txn_spill_Filter_bits(&frame->filter)
	};
	frame->result_1_live = false;
	frame->ok = true;
	txn_spill_tupled_pad_0(frame);
}

/*
 * txn_spill_tupled writes the tuple ids of the row versions a spill took,
 * each with its row's key, in tid order, to the spill's file of spilled
 * tuple ids, of the spill's number, with its filter, synced, and returns it
 * with its tids. It takes the tuples.
 */
static bool
txn_spill_tupled(const TxnSpillHomeOps *home, HostIoIoDoors io, HostSharedRecord record, const HostStorage *storage,
				 const StdPathBuf *dir, uint64 xid, uint32 number, StdVec *tuples, TxnSpillFile *result_0,
				 StdVec *result_1, HostError *error)
{
	txn_spill_tupled_frame_0_t frame = {.home = home, .io = io, .record = record, .storage = storage, .dir = dir,
		.xid = xid, .number = number, .tuples = *tuples, .result_0 = result_0, .result_1 = result_1, .error = error,
	.tuples_live = true};

	std_landing(&frame, txn_spill_tupled_body_0, txn_spill_tupled_pad_0);
	return frame.ok;
}

/* txn_spill_holders_lt orders helped's holders the most bytes first, as a reversed tuple comparison does. */
static bool
txn_spill_holders_lt(const txn_TupleU64U32U64_t *a, const txn_TupleU64U32U64_t *b)
{
	if (a->f0 != b->f0)
		return a->f0 > b->f0;
	if (a->f1 != b->f1)
		return a->f1 > b->f1;
	return a->f2 > b->f2;
}

/* txn_spill_helped_frame_0_t is helped's state across its pad: the processes and the holders, while live. */
typedef struct txn_spill_helped_frame_0_t
{
	HostIoIo   *io;
	bool	   *result;
	HostError  *error;
	bool		ok;
	/* processes is a Vec<u32>. */
	StdVec		processes;
	bool		processes_live;

	/*
	 * holders is a Vec<(u64, u32, u64)> of each holder's bytes, process, and
	 * top-level xid.
	 */
	StdVec		holders;
	bool		holders_live;
} txn_spill_helped_frame_0_t;

/* txn_spill_helped_pad_0 is helped's pad: the holders and the processes, while live. */
static void
txn_spill_helped_pad_0(void *frame_)
{
	txn_spill_helped_frame_0_t *frame = frame_;

	if (frame->holders_live)
	{
		frame->holders_live = false;
		std_vec_free_Checked(&frame->holders, sizeof(txn_TupleU64U32U64_t), _Alignof(txn_TupleU64U32U64_t));
	}
	if (frame->processes_live)
	{
		frame->processes_live = false;
		std_vec_free_Checked(&frame->processes, sizeof(uint32), _Alignof(uint32));
	}
}

static bool txn_spill_swept(HostIoIo *io, uint64 xid, bool *result, HostError *error);

/* txn_spill_helped_holders gathers the processes whose transactions hold unspilled bytes, but the backend's own, most first. */
static bool
txn_spill_helped_holders(txn_spill_helped_frame_0_t *frame)
{
	HostSharedMine mine;
	uint64		own;

	if (!txn_spill_mine(frame->io, &mine, frame->error))
		return false;
	own = host_shared_Mine_load(&mine, TXN_SPILL_PENDING_TOP);
	if (!host_shared_processes(&TXN_SPILL_PENDING_PROC_SLOTS_OPS, &frame->processes, frame->error))
		return false;
	frame->processes_live = true;
	std_vec_new(&frame->holders, sizeof(txn_TupleU64U32U64_t), _Alignof(txn_TupleU64U32U64_t));
	frame->holders_live = true;
	for (size_t k = 0; k < frame->processes.len; k++)
	{
		txn_TupleU64U32U64_t holder = {.f1 = STD_VEC_AT(&frame->processes, uint32, k)};

		if (!host_shared_load(&TXN_SPILL_PENDING_PROC_SLOTS_OPS, TXN_SPILL_PENDING_UNSPILLED, holder.f1, &holder.f0,
							  frame->error) ||
			!host_shared_load(&TXN_SPILL_PENDING_PROC_SLOTS_OPS, TXN_SPILL_PENDING_TOP, holder.f1, &holder.f2,
							  frame->error))
			return false;
		if (holder.f0 == 0 || holder.f2 == own)
			continue;
		if (!std_vec_try_reserve(&frame->holders, 1, sizeof(holder), _Alignof(txn_TupleU64U32U64_t)))
		{
			*frame->error = host_Error_out_of_memory();
			return false;
		}
		std_vec_push_Checked(&frame->holders, &holder, sizeof(holder), _Alignof(txn_TupleU64U32U64_t));
	}
	for (size_t k = 1; k < frame->holders.len; k++)
		for (size_t j = k; j > 0 && txn_spill_holders_lt(&STD_VEC_AT(&frame->holders, txn_TupleU64U32U64_t, j),
														 &STD_VEC_AT(&frame->holders, txn_TupleU64U32U64_t, j - 1));
			 j--)
		{
			txn_TupleU64U32U64_t swapped = STD_VEC_AT(&frame->holders, txn_TupleU64U32U64_t, j);

			STD_VEC_AT(&frame->holders, txn_TupleU64U32U64_t, j) =
				STD_VEC_AT(&frame->holders, txn_TupleU64U32U64_t, j - 1);
			STD_VEC_AT(&frame->holders, txn_TupleU64U32U64_t, j - 1) = swapped;
		}
	return true;
}

/* txn_spill_helped_body_0 is helped's body. */
static void
txn_spill_helped_body_0(void *frame_)
{
	txn_spill_helped_frame_0_t *frame = frame_;

	*frame->result = false;
	if (!txn_spill_helped_holders(frame))
	{
		txn_spill_helped_pad_0(frame);
		return;
	}
	for (size_t k = 0; k < frame->holders.len; k++)
	{
		const txn_TupleU64U32U64_t *holder = &STD_VEC_AT(&frame->holders, txn_TupleU64U32U64_t, k);
		HostSharedTheirs theirs;
		TxnSpillTurn turn;
		bool		some;
		bool		swept;

		if (!host_shared_theirs(&TXN_SPILL_PENDING_PROC_SLOTS_OPS, holder->f1, &theirs, frame->error) ||
			!txn_spill_Turn_of(frame->io, &theirs, &some, &turn, frame->error))
		{
			txn_spill_helped_pad_0(frame);
			return;
		}
		if (!some)
			continue;
		swept = txn_spill_swept(frame->io, holder->f2, frame->result, frame->error);
		txn_spill_Turn_given(&turn, holder->f0);
		if (!swept || *frame->result)
		{
			frame->ok = swept;
			txn_spill_helped_pad_0(frame);
			return;
		}
	}
	frame->ok = true;
	txn_spill_helped_pad_0(frame);
}

/*
 * txn_spill_helped spills, for a writer refused at the budget with no
 * versions of its own to spill, the unspilled versions of the transaction
 * that holds the most: it takes that transaction's turn, passing over one
 * another process spills, spills its versions from every storage's
 * memtable, and reports whether it spilled a key.
 */
static bool
txn_spill_helped(HostIoIo *io, bool *result, HostError *error)
{
	txn_spill_helped_frame_0_t frame = {.io = io, .result = result, .error = error};

	std_landing(&frame, txn_spill_helped_body_0, txn_spill_helped_pad_0);
	return frame.ok;
}

/* TxnSpillCommittedState is a state the committed arm may spill: its delta's bytes, the state held, and its storage. */
typedef struct TxnSpillCommittedState
{
	uint64		bytes;
	HostSharedHeld held;
	HostStorage storage;
} TxnSpillCommittedState;

/* TxnSpillOldest is a writer of a chain's oldest memtable version that committed: its commit's end, its top-level xid, and the tree. */
typedef struct TxnSpillOldest
{
	HostLsn		end;
	uint64		xid;
	TxnWrittenOf of;
} TxnSpillOldest;

/* txn_spill_committed_frame_0_t is committed's state across its pad: the swept locations, a state opened, and the states held, while live. */
typedef struct txn_spill_committed_frame_0_t
{
	HostIoIo   *io;
	bool	   *result;
	HostError  *error;
	bool		ok;
	/* swept is a Vec<(u64, u64)> of a sweep's locations and words. */
	StdVec		swept;
	bool		swept_live;
	HostSharedHeld held;
	bool		held_live;
	/* states is a Vec<CommittedState>. */
	StdVec		states;
	bool		states_live;
	/* oldest is a Vec<Oldest> of one state's writers. */
	StdVec		oldest;
	bool		oldest_live;

	/*
	 * turned is the state whose turn the process holds, as id, while
	 * turn_live.
	 */
	HostSharedRecord turned;
	uint64		id;
	bool		turn_live;
} txn_spill_committed_frame_0_t;

/* txn_spill_committed_pad_0 is committed's pad: the turn taken, the writers, the states held, the state opened, and the swept list, while live. */
static void
txn_spill_committed_pad_0(void *frame_)
{
	txn_spill_committed_frame_0_t *frame = frame_;

	if (frame->turn_live)
	{
		frame->turn_live = false;
		txn_state_committed_given(frame->turned, frame->id);
	}
	if (frame->oldest_live)
	{
		frame->oldest_live = false;
		std_vec_free_Checked(&frame->oldest, sizeof(TxnSpillOldest), _Alignof(TxnSpillOldest));
	}
	if (frame->states_live)
	{
		frame->states_live = false;
		for (size_t k = 0; k < frame->states.len; k++)
			host_shared_Held_drop(&STD_VEC_AT(&frame->states, TxnSpillCommittedState, k).held);
		std_vec_free_Checked(&frame->states, sizeof(TxnSpillCommittedState), _Alignof(TxnSpillCommittedState));
	}
	if (frame->held_live)
	{
		frame->held_live = false;
		host_shared_Held_drop(&frame->held);
	}
	if (frame->swept_live)
	{
		frame->swept_live = false;
		std_vec_free_Checked(&frame->swept, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
	}
}

/* txn_spill_committed_bytes_cmp orders committed's states by their delta's bytes, the most first. */
static int
txn_spill_committed_bytes_cmp(const void *a, const void *b)
{
	uint64		x = ((const TxnSpillCommittedState *) a)->bytes;
	uint64		y = ((const TxnSpillCommittedState *) b)->bytes;

	return x > y ? -1 : x < y;
}

/* txn_spill_oldest_cmp orders a state's oldest writers by their commits' ends, then their xids. */
static int
txn_spill_oldest_cmp(const void *a, const void *b)
{
	const TxnSpillOldest *x = a;
	const TxnSpillOldest *y = b;

	if (x->end != y->end)
		return x->end < y->end ? -1 : 1;
	return x->xid < y->xid ? -1 : x->xid > y->xid;
}

/* txn_spill_committed_oldest_t is committed_from's walk's captures: the frame and the tree walked. */
typedef struct txn_spill_committed_oldest_t
{
	txn_spill_committed_frame_0_t *frame;
	TxnWrittenOf of;
} txn_spill_committed_oldest_t;

/* txn_spill_committed_closure_0 is committed_from's walk's step: the writer of a chain's oldest memtable version, once, when it committed. */
static bool
txn_spill_committed_closure_0(void *captures, HostIoIo *io, const uint8 *key, size_t key_len, const StdVec *records,
							  size_t records_len, const txn_TupleHandleU64Bool_t *handles, size_t handles_len,
							  HostError *error)
{
	txn_spill_committed_oldest_t *c = captures;
	StdVec	   *oldest = &c->frame->oldest;
	TxnSpillOldest writer = {.of = c->of};
	HostXid		xid;
	TxnStatusCommit commit;

	if (handles_len == 0)
		return true;
	writer.xid = handles[handles_len - 1].f1;
	for (size_t k = 0; k < oldest->len; k++)
	{
		const TxnSpillOldest *at = &STD_VEC_AT(oldest, TxnSpillOldest, k);

		if (at->xid == writer.xid && at->of == writer.of)
			return true;
	}
	xid.f0 = writer.xid;
	if (!txn_status_committed(io, xid, &commit, error))
		return false;
	if (commit.tag != TXN_STATUS_COMMIT_AT)
		return true;
	writer.end = commit.u.at;
	if (!std_vec_try_reserve(oldest, 1, sizeof(writer), _Alignof(TxnSpillOldest)))
	{
		*error = host_Error_out_of_memory();
		return false;
	}
	std_vec_push_Checked(oldest, &writer, sizeof(writer), _Alignof(TxnSpillOldest));
	return true;
}

/*
 * txn_spill_committed_from spills a storage's committed versions as
 * committed does, under the process's turn at it: the writers of its rows'
 * and value map's chains' oldest memtable versions that committed, the
 * oldest commit first.
 */
static bool
txn_spill_committed_from(txn_spill_committed_frame_0_t *frame, const TxnSpillCommittedState *state)
{
	static const TxnWrittenOf ofs[] = {TXN_WRITTEN_OF_ROWS, TXN_WRITTEN_OF_VALUES};

	std_vec_new(&frame->oldest, sizeof(TxnSpillOldest), _Alignof(TxnSpillOldest));
	frame->oldest_live = true;
	for (size_t k = 0; k < lengthof(ofs); k++)
	{
		txn_spill_committed_oldest_t captures = {frame, ofs[k]};

		if (!txn_spill_Home_copied(&TXN_SPILL_STATE_HOME_OPS, frame->io, host_shared_Held_record(&state->held),
								   ofs[k], txn_spill_committed_closure_0, &captures, frame->error))
			return false;
	}
	if (frame->oldest.len > 1)
		qsort(frame->oldest.ptr, frame->oldest.len, sizeof(TxnSpillOldest), txn_spill_oldest_cmp);
	for (size_t k = 0; k < frame->oldest.len && !*frame->result; k++)
	{
		const TxnSpillOldest *writer = &STD_VEC_AT(&frame->oldest, TxnSpillOldest, k);

		if (!txn_spill_spilled_from(&TXN_SPILL_STATE_HOME_OPS, frame->io, &state->held, &state->storage, writer->of,
									writer->xid, true, frame->result, frame->error))
			return false;
	}
	frame->oldest_live = false;
	std_vec_free_Checked(&frame->oldest, sizeof(TxnSpillOldest), _Alignof(TxnSpillOldest));
	return true;
}

/* txn_spill_committed_body_0 is committed's body. */
static void
txn_spill_committed_body_0(void *frame_)
{
	txn_spill_committed_frame_0_t *frame = frame_;
	HostSharedMine mine;

	*frame->result = false;
	if (!txn_spill_mine(frame->io, &mine, frame->error) ||
		!host_shared_sweep(HOST_SHARED_IO_WITHIN(frame->io), &TXN_STATE_STATE_POOL_OPS,
						   TXN_STATE_MEMTABLE_WORD_INDEX, &frame->swept, frame->error))
		return;
	frame->id = host_shared_Mine_id(&mine);
	frame->swept_live = true;
	std_vec_new(&frame->states, sizeof(TxnSpillCommittedState), _Alignof(TxnSpillCommittedState));
	frame->states_live = true;
	for (size_t k = 0; k < frame->swept.len; k++)
	{
		bool		some;
		TxnSpillCommittedState state;
		TxnStatePersistence persistence;

		if (!host_shared_open(HOST_SHARED_IO_WITHIN(frame->io), &TXN_STATE_STATE_POOL_OPS,
							  STD_VEC_AT(&frame->swept, std_TupleU64U64_t, k).f0, &some, &frame->held, frame->error))
		{
			txn_spill_committed_pad_0(frame);
			return;
		}
		if (!some)
			continue;
		frame->held_live = true;
		if (!txn_state_described(host_shared_Held_record(&frame->held), &state.storage, &persistence))
		{
			frame->held_live = false;
			host_shared_Held_drop(&frame->held);
			continue;
		}
		if (!std_vec_try_reserve(&frame->states, 1, sizeof(state), _Alignof(TxnSpillCommittedState)))
		{
			*frame->error = host_Error_out_of_memory();
			txn_spill_committed_pad_0(frame);
			return;
		}
		state.bytes = txn_state_delta_bytes(host_shared_Held_record(&frame->held));
		state.held = frame->held;
		frame->held_live = false;
		std_vec_push_Checked(&frame->states, &state, sizeof(state), _Alignof(TxnSpillCommittedState));
	}
	if (frame->states.len > 1)
		qsort(frame->states.ptr, frame->states.len, sizeof(TxnSpillCommittedState), txn_spill_committed_bytes_cmp);
	for (size_t k = 0; k < frame->states.len && !*frame->result; k++)
	{
		const TxnSpillCommittedState *state = &STD_VEC_AT(&frame->states, TxnSpillCommittedState, k);
		HostSharedRecord record = host_shared_Held_record(&state->held);
		bool		spilled;

		if (!txn_state_committed_taken(record, frame->id))
			continue;
		frame->turned = record;
		frame->turn_live = true;
		spilled = txn_spill_committed_from(frame, state);
		frame->turn_live = false;
		txn_state_committed_given(record, frame->id);
		if (!spilled)
		{
			txn_spill_committed_pad_0(frame);
			return;
		}
	}
	frame->ok = true;
	txn_spill_committed_pad_0(frame);
}

/*
 * txn_spill_committed spills, for a writer the full delta refused when no
 * transaction had versions of its own left to spill, the committed versions
 * a fold leaves in a storage's memtable, its point held below their commits
 * by a snapshot (the budget's committed arm): of the storages, the most
 * delta bytes first, each whose turn at it no other process holds, its
 * committed transactions' versions of its rows and its value map, the oldest
 * commit first, of every key whose oldest versions in the memtable are that
 * transaction's alone, the spilled versions below them older still
 * (spilled_from), until one spills a key; it answers whether one did.
 */
static bool
txn_spill_committed(HostIoIo *io, bool *result, HostError *error)
{
	txn_spill_committed_frame_0_t frame = {.io = io, .result = result, .error = error};

	std_landing(&frame, txn_spill_committed_body_0, txn_spill_committed_pad_0);
	return frame.ok;
}

/*
 * txn_spill_relieved spills, for a writer that found the delta full, its own
 * trees, or else another transaction's, and reports whether one spilled a
 * key; once no transaction has versions of its own left to spill, it spills
 * committed versions a fold leaves (committed).
 */
bool
txn_spill_relieved(HostIoIo *io, bool *result, HostError *error)
{
	if (!txn_spill_relieved_own(io, result, error))
		return false;
	if (!*result && !txn_spill_helped(io, result, error))
		return false;
	return *result || txn_spill_committed(io, result, error);
}

/* txn_spill_swept_frame_0_t is swept's state across its pad: the swept locations, a home opened, and the trees, while live. */
typedef struct txn_spill_swept_frame_0_t
{
	HostIoIo   *io;
	uint64		xid;
	bool	   *result;
	HostError  *error;
	bool		ok;
	/* swept is a Vec<(u64, u64)> of a sweep's locations and words. */
	StdVec		swept;
	bool		swept_live;
	HostSharedHeld held;
	bool		held_live;
	/* trees is a Vec<(Storage, Of)>. */
	StdVec		trees;
	bool		trees_live;
} txn_spill_swept_frame_0_t;

/* txn_spill_swept_pad_0 is swept's pad: what it holds, while live. */
static void
txn_spill_swept_pad_0(void *frame_)
{
	txn_spill_swept_frame_0_t *frame = frame_;

	if (frame->trees_live)
	{
		frame->trees_live = false;
		std_vec_free_Checked(&frame->trees, sizeof(txn_TupleStorageOf_t), _Alignof(txn_TupleStorageOf_t));
	}
	if (frame->held_live)
	{
		frame->held_live = false;
		host_shared_Held_drop(&frame->held);
	}
	if (frame->swept_live)
	{
		frame->swept_live = false;
		std_vec_free_Checked(&frame->swept, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
	}
}

/* txn_spill_swept_pool adds the trees of each record of a pool a sweep of its memtable word finds, filled. */
static bool
txn_spill_swept_pool(txn_spill_swept_frame_0_t *frame, const HostSharedPoolOps *pool, size_t word,
					 const TxnSpillHomeOps *home, const TxnWrittenOf *ofs, size_t ofs_len)
{
	if (!host_shared_sweep(HOST_SHARED_IO_WITHIN(frame->io), pool, word, &frame->swept, frame->error))
		return false;
	frame->swept_live = true;
	for (size_t k = 0; k < frame->swept.len; k++)
	{
		bool		some;
		txn_TupleStorageOf_t tree;
		TxnStatePersistence persistence;

		if (!host_shared_open(HOST_SHARED_IO_WITHIN(frame->io), pool, STD_VEC_AT(&frame->swept, std_TupleU64U64_t,
																				 k).f0, &some, &frame->held,
							  frame->error))
			return false;
		if (!some)
			continue;
		frame->held_live = true;
		if (home->described(host_shared_Held_record(&frame->held), &tree.f0, &persistence))
		{
			if (!std_vec_try_reserve(&frame->trees, ofs_len, sizeof(tree), _Alignof(txn_TupleStorageOf_t)))
			{
				*frame->error = host_Error_out_of_memory();
				return false;
			}
			for (size_t j = 0; j < ofs_len; j++)
			{
				tree.f1 = ofs[j];
				std_vec_push_Checked(&frame->trees, &tree, sizeof(tree), _Alignof(txn_TupleStorageOf_t));
			}
		}
		frame->held_live = false;
		host_shared_Held_drop(&frame->held);
	}
	frame->swept_live = false;
	std_vec_free_Checked(&frame->swept, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
	return true;
}

/* txn_spill_swept_body_0 is swept's body. */
static void
txn_spill_swept_body_0(void *frame_)
{
	txn_spill_swept_frame_0_t *frame = frame_;
	static const TxnWrittenOf state_ofs[] = {TXN_WRITTEN_OF_ROWS, TXN_WRITTEN_OF_VALUES, TXN_WRITTEN_OF_FREE};
	static const TxnWrittenOf chunk_ofs[] = {TXN_WRITTEN_OF_CHUNKS};

	*frame->result = false;
	std_vec_new(&frame->trees, sizeof(txn_TupleStorageOf_t), _Alignof(txn_TupleStorageOf_t));
	frame->trees_live = true;
	if (!txn_spill_swept_pool(frame, &TXN_STATE_STATE_POOL_OPS, TXN_STATE_MEMTABLE_WORD_INDEX,
							  &TXN_SPILL_STATE_HOME_OPS, state_ofs, lengthof(state_ofs)) ||
		!txn_spill_swept_pool(frame, &TXN_MEMTABLE_BRANCH_BRANCH_DELTAS_POOL_OPS,
							  TXN_MEMTABLE_BRANCH_CHUNK_WORD_INDEX, &TXN_SPILL_BRANCH_DELTAS_HOME_OPS, chunk_ofs, 1))
	{
		txn_spill_swept_pad_0(frame);
		return;
	}
	for (size_t k = 0; k < frame->trees.len; k++)
	{
		const txn_TupleStorageOf_t *tree = &STD_VEC_AT(&frame->trees, txn_TupleStorageOf_t, k);
		bool		spilled;

		if (!txn_spill_spilled_turn(frame->io, &tree->f0, tree->f1, frame->xid, &spilled, frame->error))
		{
			txn_spill_swept_pad_0(frame);
			return;
		}
		*frame->result |= spilled;
	}
	frame->ok = true;
	txn_spill_swept_pad_0(frame);
}

/*
 * txn_spill_swept spills a transaction's versions from every storage's
 * memtable, its three trees, and every repository's chunk tree, for a helper
 * whose turn it is, and reports whether it spilled a key.
 */
static bool
txn_spill_swept(HostIoIo *io, uint64 xid, bool *result, HostError *error)
{
	txn_spill_swept_frame_0_t frame = {.io = io, .xid = xid, .result = result, .error = error};

	std_landing(&frame, txn_spill_swept_body_0, txn_spill_swept_pad_0);
	return frame.ok;
}

/* txn_spill_aborted_closure_0 is aborted's test: a file of the aborted transaction. */
static bool
txn_spill_aborted_closure_0(void *captures, const TxnSpillFile *file)
{
	return file->xid == *(const uint64 *) captures;
}

/* txn_spill_aborted_pending_t is aborted's step's captures on the home of its pending spill: the backend's I/O and the spill's kind. */
typedef struct txn_spill_aborted_pending_t
{
	HostIoIo   *io;
	TxnRunsKind kind;
} txn_spill_aborted_pending_t;

/*
 * txn_spill_aborted_closure_1 is aborted's step on the home of its pending
 * spill: a free space map's spill of a storage listed from the slot, and any
 * other the slot cleared.
 */
static bool
txn_spill_aborted_closure_1(void *captures, const TxnSpillHomeOps *home, const HostSharedHeld *held, HostError *error)
{
	const txn_spill_aborted_pending_t *c = captures;
	HostSharedRecord record = host_shared_Held_record(held);

	if (home == &TXN_SPILL_STATE_HOME_OPS && c->kind == TXN_RUNS_KIND_SPILLED_FREE)
		return txn_spill_settle_pending(c->io, error);
	return txn_spill_settled(home, c->io, &record, error);
}

/* txn_spill_aborted_kept_closure_0 is aborted's edit of a storage's manifest: the transaction's files out but its free space map's, which it marks with the abort's end. */
static bool
txn_spill_aborted_kept_closure_0(void *captures, HostSharedLongIo *io, StdVec *files, HostError *error)
{
	const txn_spill_marked_t *c = captures;
	size_t		kept = 0;

	for (size_t k = 0; k < files->len; k++)
	{
		TxnSpillFile file = STD_VEC_AT(files, TxnSpillFile, k);

		if (file.xid == c->xid && file.kind != TXN_RUNS_KIND_SPILLED_FREE)
			continue;
		if (file.xid == c->xid)
			file.passed = c->end;
		STD_VEC_AT(files, TxnSpillFile, kept++) = file;
	}
	files->len = kept;
	return true;
}

/* txn_spill_aborted_captures_t is aborted's step's captures on a storage it spilled: the backend's I/O, the storage, and the xid. */
typedef struct txn_spill_aborted_captures_t
{
	HostIoIo   *io;
	const HostStorage *storage;
	txn_spill_marked_t marked;
} txn_spill_aborted_captures_t;

/*
 * txn_spill_aborted_closure_2 is aborted's step on a storage it spilled: the
 * transaction's files taken out of its manifest, but a storage's free space
 * map's spills, which stay, marked with the abort's end.
 */
static bool
txn_spill_aborted_closure_2(void *captures, const TxnSpillHomeOps *home, const HostSharedHeld *held, HostError *error)
{
	txn_spill_aborted_captures_t *c = captures;
	StdVec		gone;

	if (home == &TXN_SPILL_STATE_HOME_OPS)
		return txn_spill_changed(home, c->io, host_shared_Held_record(held), c->storage,
								 txn_spill_aborted_kept_closure_0, &c->marked, error);
	if (!txn_spill_removed(home, c->io, host_shared_Held_record(held), c->storage, txn_spill_aborted_closure_0,
						   &c->marked.xid, &gone, error))
		return false;
	std_vec_free_Checked(&gone, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	return true;
}

/*
 * txn_spill_aborted takes an aborted transaction's spills out of the
 * manifests of the storages it spilled, and clears the process's slot when
 * it names one of its spills; a storage whose state is gone took its spills
 * with it. A free space map's spill stays, listed from the slot when the
 * slot names it, since an aborted take keeps its place until a fold takes
 * the file, marked with the abort's end, the point a fold passes it at.
 */
bool
txn_spill_aborted(HostIoIo *io, uint64 xid, const HostStorage *storages, size_t storages_len, HostLsn end,
				  HostError *error)
{
	bool		some;
	TxnSpillPended pended;

	if (!txn_spill_pending(io, &some, &pended, error))
		return false;
	if (some && pended.xid == xid)
	{
		txn_spill_aborted_pending_t pending = {io, pended.kind};

		if (!txn_spill_at_found(io, &pended.storage, txn_spill_aborted_closure_1, &pending, &some, error))
			return false;
		if (!some && !txn_spill_settled(&TXN_SPILL_STATE_HOME_OPS, io, NULL, error))
			return false;
	}
	for (size_t k = 0; k < storages_len; k++)
	{
		txn_spill_aborted_captures_t captures = {io, &storages[k], {xid, end}};

		if (!txn_spill_at_found(io, &storages[k], txn_spill_aborted_closure_2, &captures, &some, error))
			return false;
	}
	return true;
}

/* txn_spill_counted returns how many of a spill's decoded row versions count at their transaction's entry: every one but a lock's. */
uint64
txn_spill_counted(const TxnRunsDecoded *decoded)
{
	uint64		count = 0;

	for (size_t k = 0; k < decoded->keys.len; k++)
	{
		const TxnRunsKey *key = &STD_VEC_AT(&decoded->keys, TxnRunsKey, k);

		for (size_t v = 0; v < key->versions.len; v++)
		{
			TxnMemtableVersionOpRef op = txn_memtable_version_Record_op(&STD_VEC_AT(&key->versions,
																					TxnMemtableVersionRecord, v));

			if (op.tag != TXN_MEMTABLE_VERSION_OP_REF_LOCK && op.tag != TXN_MEMTABLE_VERSION_OP_REF_LOCK_ROW)
				count++;
		}
	}
	return count;
}

/* TxnSpillSource is Source: a spill file a merge reads, its index's entries, where its index frame starts, the entry of the chunk it holds, and that chunk's keys from the one at head. */
typedef struct TxnSpillSource
{
	HostFile	file;
	/* entries is an Index, a Vec<(Vec<u8>, u64)>. */
	StdVec		entries;
	uint64		index_at;
	size_t		entry;

	/*
	 * keys is a Chunk, a Vec<(Vec<u8>, Vec<Vec<u8>>)>, whose keys before head
	 * were taken.
	 */
	StdVec		keys;
	size_t		head;
} TxnSpillSource;

/* txn_spill_spilled_drop drops a chunk's keys from an index on, and its buffer. */
static void
txn_spill_spilled_drop(StdVec *keys, size_t from)
{
	for (size_t k = from; k < keys->len; k++)
	{
		txn_TupleVecU8VecVecU8_t *key = &STD_VEC_AT(keys, txn_TupleVecU8VecVecU8_t, k);

		std_vec_free_Checked(&key->f0, 1, 1);
		for (size_t r = 0; r < key->f1.len; r++)
			std_vec_free_Checked(&STD_VEC_AT(&key->f1, StdVec, r), 1, 1);
		std_vec_free_Checked(&key->f1, sizeof(StdVec), _Alignof(StdVec));
	}
	std_vec_free_Checked(keys, sizeof(txn_TupleVecU8VecVecU8_t), _Alignof(txn_TupleVecU8VecVecU8_t));
}

/* txn_spill_Source_drop is Source's drop glue: its file, its entries, and its keys left. */
static void
txn_spill_Source_drop(TxnSpillSource *self)
{
	txn_spill_spilled_drop(&self->keys, self->head);
	txn_runs_Index_drop(&self->entries);
	host_File_drop(&self->file);
}

/* txn_spill_Source_front returns the key the source is at, reading its next chunk when it read the last's, or nothing past its last key. */
static bool
txn_spill_Source_front(TxnSpillSource *self, HostIoIo *io, const TxnRunsNames *names, bool *some, StdBytes *result,
					   HostError *error)
{
	while (self->head == self->keys.len && self->entry < self->entries.len)
	{
		uint64		start = STD_VEC_AT(&self->entries, txn_TupleVecU8U64_t, self->entry).f1;
		uint64		end = self->index_at;
		size_t		next = STD_ADD(usize, self->entry, 1, 1272, 40);

		if (next < self->entries.len)
			end = STD_VEC_AT(&self->entries, txn_TupleVecU8U64_t, next).f1;
		txn_spill_spilled_drop(&self->keys, self->head);
		self->head = 0;
		std_vec_new(&self->keys, sizeof(txn_TupleVecU8VecVecU8_t), _Alignof(txn_TupleVecU8VecVecU8_t));
		if (!txn_runs_spill_chunk(io, &self->file, names, start, end, &self->keys, error))
			return false;
		self->entry = next;
	}
	*some = self->head < self->keys.len;
	if (*some)
	{
		const StdVec *key = &STD_VEC_AT(&self->keys, txn_TupleVecU8VecVecU8_t, self->head).f0;

		result->ptr = key->ptr;
		result->len = key->len;
	}
	return true;
}

static bool txn_spill_ends_hold(const StdVec *ends, uint64 lsn);

/*
 * TxnSpillLeft is what a join leaves out of a carry its primary unlisted:
 * the ends of the carry's records, ascending; the count of the versions it
 * dropped; and the tuple ids of the rows it dropped, which a rows join adds
 * and a tuple-id join of the same files leaves out, ascending by then.
 */
typedef struct TxnSpillLeft
{
	const StdVec *ends;

	/*
	 * carried is the carried rows' tuple ids, ascending, whose free space
	 * takes, which carry no LSN, a free space run leaves out, or NULL.
	 */
	const StdVec *carried;
	uint64		dropped;
	/* tids is a Vec<u64>. */
	StdVec		tids;
} TxnSpillLeft;

/* txn_spill_joined_frame_0_t is joined's state across its pad: the sources, the filter, the writing, the least key, and its versions, while live. */
typedef struct txn_spill_joined_frame_0_t
{
	HostIoIo   *io;
	const StdPathBuf *dir;
	const TxnRunsNames *names;
	TxnRunsTail tail;
	const TxnSpillFile *files;
	size_t		files_len;
	uint32		number;

	/*
	 * left is what the join leaves out of an unlisted carry, or NULL for
	 * none.
	 */
	TxnSpillLeft *left;
	TxnSpillFile *result;
	HostError  *error;
	bool		ok;
	StdPathBuf	path;
	bool		path_live;
	StdString	name;
	bool		name_live;
	HostFile	opened;
	bool		opened_live;
	/* entries is a source's Index, before it joins the sources. */
	StdVec		entries;
	bool		entries_live;
	/* sources is a Vec<Source>. */
	StdVec		sources;
	bool		sources_live;
	TxnSpillFilter filter;
	bool		filter_live;
	TxnRunsWriting *writing;
	bool		writing_live;
	StdVec		least;
	bool		least_live;
	/* versions is a Vec<Vec<u8>>. */
	StdVec		versions;
	bool		versions_live;
	/* records is a Vec<&[u8]>. */
	StdVec		records;
	bool		records_live;
} txn_spill_joined_frame_0_t;

/* txn_spill_vec_u8_drop is Vec<u8>'s drop glue. */
static void
txn_spill_vec_u8_drop(void *elem)
{
	std_vec_free_Checked(elem, 1, 1);
}

/* txn_spill_versions_drop drops a Vec<Vec<u8>>. */
static void
txn_spill_versions_drop(StdVec *versions)
{
	for (size_t k = 0; k < versions->len; k++)
		std_vec_free_Checked(&STD_VEC_AT(versions, StdVec, k), 1, 1);
	std_vec_free_Checked(versions, sizeof(StdVec), _Alignof(StdVec));
}

/* txn_spill_joined_pad_0 is joined's pad: what it holds, while live. */
static void
txn_spill_joined_pad_0(void *frame_)
{
	txn_spill_joined_frame_0_t *frame = frame_;

	if (frame->records_live)
	{
		frame->records_live = false;
		std_vec_free_Checked(&frame->records, sizeof(StdBytes), _Alignof(StdBytes));
	}
	if (frame->versions_live)
	{
		frame->versions_live = false;
		txn_spill_versions_drop(&frame->versions);
	}
	if (frame->least_live)
	{
		frame->least_live = false;
		std_vec_free_Checked(&frame->least, 1, 1);
	}
	if (frame->writing_live)
	{
		frame->writing_live = false;
		txn_runs_Writing_drop(frame->writing);
	}
	if (frame->filter_live)
	{
		frame->filter_live = false;
		std_vec_free_Checked(&frame->filter.words, sizeof(uint64), _Alignof(uint64));
	}
	if (frame->sources_live)
	{
		frame->sources_live = false;
		for (size_t k = 0; k < frame->sources.len; k++)
			txn_spill_Source_drop(&STD_VEC_AT(&frame->sources, TxnSpillSource, k));
		std_vec_free_Checked(&frame->sources, sizeof(TxnSpillSource), _Alignof(TxnSpillSource));
	}
	if (frame->entries_live)
	{
		frame->entries_live = false;
		txn_runs_Index_drop(&frame->entries);
	}
	if (frame->opened_live)
	{
		frame->opened_live = false;
		host_File_drop(&frame->opened);
	}
	if (frame->name_live)
	{
		frame->name_live = false;
		std_vec_free_Checked(&frame->name, 1, 1);
	}
	if (frame->path_live)
	{
		frame->path_live = false;
		std_vec_free_Checked(&frame->path.bytes, 1, 1);
	}
}

/* txn_spill_joined_opened opens each file the merge joins, with its index, counting their keys for the filter. */
static bool
txn_spill_joined_opened(txn_spill_joined_frame_0_t *frame, uint64 *count)
{
	ChunksErrorError failed;

	std_vec_new(&frame->sources, sizeof(TxnSpillSource), _Alignof(TxnSpillSource));
	frame->sources_live = true;
	for (size_t k = 0; k < frame->files_len; k++)
	{
		const TxnSpillFile *file = &frame->files[k];
		TxnSpillSource source = {0};
		uint64		keys;

		txn_runs_spill_name(frame->names, file->number, &frame->name);
		frame->name_live = true;
		std_path_join(frame->dir->bytes.ptr, frame->dir->bytes.len, (const char *) frame->name.ptr, frame->name.len,
					  &frame->path);
		frame->path_live = true;
		frame->name_live = false;
		std_vec_free_Checked(&frame->name, 1, 1);
		if (!chunks_file_open_rw(HOST_IO_IO_IO_DOORS(frame->io), frame->path.bytes.ptr, frame->path.bytes.len, false,
								 &frame->opened, &failed))
		{
			*frame->error = txn_base_storage(&failed);
			return false;
		}
		frame->opened_live = true;
		frame->path_live = false;
		std_vec_free_Checked(&frame->path.bytes, 1, 1);
		if (!txn_runs_spill_index(frame->io, &frame->opened, file->len, &source.index_at, &frame->entries,
								  frame->error))
			return false;
		frame->entries_live = true;
		if (!txn_runs_spill_tail_count(frame->io, &frame->opened, file->len, &keys, frame->error))
			return false;
		*count = STD_ADD(u64, *count, keys, 1349, 9);
		source.file = frame->opened;
		source.entries = frame->entries;
		std_vec_new(&source.keys, sizeof(txn_TupleVecU8VecVecU8_t), _Alignof(txn_TupleVecU8VecVecU8_t));
		std_vec_push_Checked(&frame->sources, &source, sizeof(source), _Alignof(TxnSpillSource));
		frame->entries_live = false;
		frame->opened_live = false;
	}
	return true;
}

/* txn_spill_joined_key writes the least key the sources are at with its versions, the newer files' first, a tuple id's the newest file's alone. */
static bool
txn_spill_joined_key(txn_spill_joined_frame_0_t *frame)
{
	std_vec_new(&frame->versions, sizeof(StdVec), _Alignof(StdVec));
	frame->versions_live = true;
	for (size_t k = 0; k < frame->sources.len; k++)
	{
		TxnSpillSource *source = &STD_VEC_AT(&frame->sources, TxnSpillSource, k);
		bool		some;
		StdBytes	front;
		txn_TupleVecU8VecVecU8_t *taken;

		if (!txn_spill_Source_front(source, frame->io, frame->names, &some, &front, frame->error))
			return false;
		if (!some || front.len != frame->least.len || memcmp(front.ptr, frame->least.ptr, front.len) != 0)
			continue;
		taken = &STD_VEC_AT(&source->keys, txn_TupleVecU8VecVecU8_t, source->head);
		source->head++;
		std_vec_free_Checked(&taken->f0, 1, 1);
		std_vec_extend_from_slice_Checked(&frame->versions, taken->f1.ptr, taken->f1.len, sizeof(StdVec),
										  _Alignof(StdVec));
		std_vec_free_Checked(&taken->f1, sizeof(StdVec), _Alignof(StdVec));
	}
	if (frame->names->kind == TXN_RUNS_KIND_SPILLED_TUPLES)
		std_vec_truncate(&frame->versions, 1, sizeof(StdVec), txn_spill_vec_u8_drop);
	if (frame->left != NULL && frame->names->kind == TXN_RUNS_KIND_SPILLED_TUPLES && frame->least.len == 8 &&
		txn_spill_ends_hold(&frame->left->tids, std_u64_from_be_bytes(frame->least.ptr)))
		std_vec_truncate(&frame->versions, 0, sizeof(StdVec), txn_spill_vec_u8_drop);
	frame->records.len = 0;
	for (size_t k = 0; k < frame->versions.len; k++)
	{
		const StdVec *version = &STD_VEC_AT(&frame->versions, StdVec, k);
		StdBytes	record = {version->ptr, version->len};

		if (frame->left != NULL && frame->names->kind != TXN_RUNS_KIND_SPILLED_TUPLES)
		{
			TxnMemtableVersionRecord stored = txn_memtable_version_Record_of(version->ptr, version->len);
			uint64		lsn;
			uint64		tid;

			if (txn_memtable_version_Record_lsn(&stored, &lsn) && txn_spill_ends_hold(frame->left->ends, lsn))
			{
				frame->left->dropped++;
				if (txn_spill_tid(version->ptr, version->len, &tid))
					std_vec_push_Checked(&frame->left->tids, &tid, sizeof(uint64), _Alignof(uint64));
				continue;
			}
			tid = txn_memtable_map_taken(txn_memtable_version_Record_op(&stored)).f0;
			if (frame->left->carried != NULL && frame->names->kind == TXN_RUNS_KIND_SPILLED_FREE && tid != 0 &&
				txn_spill_ends_hold(frame->left->carried, tid))
			{
				frame->left->dropped++;
				continue;
			}
		}
		std_vec_push_Checked(&frame->records, &record, sizeof(record), _Alignof(StdBytes));
	}
	if (frame->records.len > 0)
	{
		if (!txn_runs_Writing_key(frame->writing, HOST_IO_IO_IO_DOORS(frame->io), frame->least.ptr, frame->least.len,
								  frame->records.ptr, frame->records.len, frame->error))
			return false;
		txn_spill_Filter_add(&frame->filter, frame->least.ptr, frame->least.len);
	}
	frame->versions_live = false;
	txn_spill_versions_drop(&frame->versions);
	return true;
}

/* txn_spill_joined_least sets the frame's least key to the least key the sources are at, answering false past every source's last. */
static bool
txn_spill_joined_least(txn_spill_joined_frame_0_t *frame, bool *some)
{
	*some = false;
	for (size_t k = 0; k < frame->sources.len; k++)
	{
		bool		has;
		StdBytes	front;

		if (!txn_spill_Source_front(&STD_VEC_AT(&frame->sources, TxnSpillSource, k), frame->io, frame->names, &has,
									&front, frame->error))
			return false;
		if (has && (!*some || std_bytes_Ord_cmp(front.ptr, front.len, frame->least.ptr, frame->least.len) ==
					STD_ORDERING_LESS))
		{
			frame->least.len = 0;
			std_vec_extend_from_slice_Checked(&frame->least, front.ptr, front.len, 1, 1);
			*some = true;
		}
	}
	return true;
}

/* txn_spill_joined_body_0 is joined's body. */
static void
txn_spill_joined_body_0(void *frame_)
{
	txn_spill_joined_frame_0_t *frame = frame_;
	uint64		count = 0;
	bool		some;
	RepoRunFile run;

	if (!txn_spill_joined_opened(frame, &count) || !txn_spill_Filter_with(count, &frame->filter, frame->error))
	{
		txn_spill_joined_pad_0(frame);
		return;
	}
	frame->filter_live = true;
	frame->writing = txn_runs_Writing_spill(frame->dir->bytes.ptr, frame->dir->bytes.len, frame->names, frame->tail,
											frame->number);
	frame->writing_live = true;
	std_vec_new(&frame->least, 1, 1);
	frame->least_live = true;
	std_vec_new(&frame->records, sizeof(StdBytes), _Alignof(StdBytes));
	frame->records_live = true;
	for (;;)
	{
		if (!txn_spill_joined_least(frame, &some))
		{
			txn_spill_joined_pad_0(frame);
			return;
		}
		if (!some)
			break;
		if (!txn_spill_joined_key(frame))
		{
			txn_spill_joined_pad_0(frame);
			return;
		}
	}
	if (!txn_runs_Writing_end(frame->writing, HOST_IO_IO_IO_DOORS(frame->io), frame->filter.words.ptr, frame->filter.words.len, &some, &run,
							  frame->error))
	{
		txn_spill_joined_pad_0(frame);
		return;
	}
	if (!some && frame->left != NULL)
	{
		*frame->result = (TxnSpillFile)
		{
			0
		};
		frame->ok = true;
		txn_spill_joined_pad_0(frame);
		return;
	}
	if (!some)
	{
		*frame->error = txn_spill_internal("a merge of spills wrote no key");
		txn_spill_joined_pad_0(frame);
		return;
	}
	*frame->result = (TxnSpillFile)
	{
		.xid = frame->files_len > 0 ? frame->files[0].xid : 0, .kind = frame->names->kind, .number = frame->number,
			.len = run.len, .hash = run.hash, .bits = txn_spill_Filter_bits(&frame->filter)
	};
	frame->ok = true;
	txn_spill_joined_pad_0(frame);
}

/*
 * txn_spill_joined streams spill files of one kind into one new file of a
 * number with their header's tail, in key order, a key's versions the newer
 * file's first, a tuple id's key the newest file's alone, with a filter of
 * its keys, synced, and returns it; given what to leave out of an unlisted
 * carry, it leaves out each version whose LSN its ends hold, adding the
 * dropped rows' tuple ids, or for tuple ids each one it holds, and a key left
 * with none, and answers a file of no length when it leaves out every one.
 */
static bool
txn_spill_joined(HostIoIo *io, const StdPathBuf *dir, const TxnRunsNames *names, TxnRunsTail tail,
				 const TxnSpillFile *files, size_t files_len, uint32 number, TxnSpillLeft *left,
				 TxnSpillFile *result, HostError *error)
{
	txn_spill_joined_frame_0_t frame = {.io = io, .dir = dir, .names = names, .tail = tail, .files = files,
	.files_len = files_len, .number = number, .left = left, .result = result, .error = error};

	std_landing(&frame, txn_spill_joined_body_0, txn_spill_joined_pad_0);
	return frame.ok;
}

/* txn_spill_merged_frame_0_t is merged's state across its pad: the spills' directory, the files listed, and the ones a merge takes, while live. */
typedef struct txn_spill_merged_frame_0_t
{
	const TxnSpillHomeOps *home;
	HostIoIo   *io;
	HostSharedRecord record;
	const HostStorage *storage;
	uint64		xid;
	TxnRunsKind kind;
	TxnRunsTail tail;
	HostError  *error;
	bool		ok;
	StdPathBuf	dir;
	bool		dir_live;
	txn_TupleVecFileU64_t listed;
	bool		listed_live;

	/*
	 * own is a Vec<File> of the transaction's files of the kind, newest
	 * first.
	 */
	StdVec		own;
	bool		own_live;
	/* named is a Vec<File> of the tuple-id files of the numbers merged. */
	StdVec		named;
	bool		named_live;
} txn_spill_merged_frame_0_t;

/* txn_spill_merged_pad_0 is merged's pad: what it holds, while live. */
static void
txn_spill_merged_pad_0(void *frame_)
{
	txn_spill_merged_frame_0_t *frame = frame_;

	if (frame->named_live)
	{
		frame->named_live = false;
		std_vec_free_Checked(&frame->named, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	}
	if (frame->own_live)
	{
		frame->own_live = false;
		std_vec_free_Checked(&frame->own, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	}
	if (frame->listed_live)
	{
		frame->listed_live = false;
		std_vec_free_Checked(&frame->listed.f0, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	}
	if (frame->dir_live)
	{
		frame->dir_live = false;
		std_vec_free_Checked(&frame->dir.bytes, 1, 1);
	}
}

/* txn_spill_merged_mine gathers the listed files of the transaction of a kind, in the listing's order, newest first. */
static void
txn_spill_merged_mine(const txn_spill_merged_frame_0_t *frame, TxnRunsKind kind, StdVec *result)
{
	std_vec_new(result, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	for (size_t k = 0; k < frame->listed.f0.len; k++)
	{
		const TxnSpillFile *file = &STD_VEC_AT(&frame->listed.f0, TxnSpillFile, k);

		if (file->xid == frame->xid && file->kind == kind)
			std_vec_push_Checked(result, file, sizeof(*file), _Alignof(TxnSpillFile));
	}
}

/* txn_spill_merged_step merges the transaction's newest file with the earlier files no larger than those merged so far, answering false in done when there is none to take. */
static bool
txn_spill_merged_step(txn_spill_merged_frame_0_t *frame, const TxnRunsNames *names, const TxnRunsNames *tuple_names,
					  bool *done)
{
	uint64		size;
	size_t		taken = 1;
	uint32		number = 0;
	uint32		lowest;
	TxnSpillFile listing[2] = {0};
	size_t		listing_len = 1;
	TxnRunsTail none = {.tag = TXN_RUNS_TAIL_NONE};
	txn_TupleU64KindU32OptionU32_t replacing;

	*done = true;
	if (!txn_spill_listed(frame->home, frame->io, frame->record, frame->storage, &frame->listed, frame->error))
		return false;
	frame->listed_live = true;
	txn_spill_merged_mine(frame, frame->kind, &frame->own);
	frame->own_live = true;
	if (frame->own.len == 0)
		return true;
	size = STD_VEC_AT(&frame->own, TxnSpillFile, 0).len;
	while (taken < frame->own.len)
	{
		uint64		older = STD_VEC_AT(&frame->own, TxnSpillFile, taken).len;
		bool		take = older <= size;

		size = STD_ADD(u64, size, older, 1307, 17);
		if (!take)
			break;
		taken++;
	}
	if (taken < 2)
		return true;
	*done = false;
	for (size_t k = 0; k < frame->own.len; k++)
		number = Max(number, STD_ADD(u32, STD_VEC_AT(&frame->own, TxnSpillFile, k).number, 1,
									 1315, 44));
	lowest = number;
	for (size_t k = 0; k < taken; k++)
		lowest = Min(lowest, STD_VEC_AT(&frame->own, TxnSpillFile, k).number);
	if (!txn_spill_joined(frame->io, &frame->dir, names, frame->tail, frame->own.ptr, taken, number, NULL,
						  &listing[0], frame->error))
		return false;
	txn_spill_merged_mine(frame, TXN_RUNS_KIND_SPILLED_TUPLES, &frame->named);
	frame->named_live = true;
	{
		size_t		kept = 0;

		for (size_t j = 0; j < frame->named.len; j++)
		{
			const TxnSpillFile *tuples = &STD_VEC_AT(&frame->named, TxnSpillFile, j);
			bool		merged = false;

			for (size_t k = 0; frame->kind == TXN_RUNS_KIND_SPILLED_ROWS && k < taken && !merged; k++)
				merged = STD_VEC_AT(&frame->own, TxnSpillFile, k).number == tuples->number;
			if (merged)
				STD_VEC_AT(&frame->named, TxnSpillFile, kept++) = *tuples;
		}
		frame->named.len = kept;
	}
	if (frame->named.len > 0)
	{
		if (!txn_spill_joined(frame->io, &frame->dir, tuple_names, none, frame->named.ptr, frame->named.len, number,
							  NULL, &listing[1], frame->error))
			return false;
		listing_len = 2;
	}
	replacing.f0 = frame->xid;
	replacing.f1 = frame->kind;
	replacing.f2 = number;
	replacing.has_f3 = true;
	replacing.f3 = lowest;
	if (!txn_spill_pend(frame->home, frame->io, frame->record, frame->storage, &listing[0], listing_len > 1, true,
						lowest, frame->error) ||
		!host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-spill-merge-pended", frame->error) ||
		!txn_spill_linked(frame->home, frame->io, frame->record, frame->storage, listing, listing_len, true,
						  replacing, frame->error) ||
		!host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-spill-merge-unchained", frame->error) ||
		!txn_spill_settled(frame->home, frame->io, &frame->record, frame->error))
		return false;
	return true;
}

/* txn_spill_merged_body_0 is merged's body. */
static void
txn_spill_merged_body_0(void *frame_)
{
	txn_spill_merged_frame_0_t *frame = frame_;
	TxnRunsNames names;
	TxnRunsNames tuple_names;
	bool		done = false;

	if (!txn_spill_names(frame->home, frame->record, frame->storage, frame->kind, frame->xid, &names, frame->error) ||
		!txn_spill_names(frame->home, frame->record, frame->storage, TXN_RUNS_KIND_SPILLED_TUPLES, frame->xid,
						 &tuple_names, frame->error))
		return;
	txn_runs_spill_directory(frame->storage->database, frame->home->own(frame->record), &frame->dir);
	frame->dir_live = true;
	while (!done)
	{
		if (!txn_spill_merged_step(frame, &names, &tuple_names, &done))
		{
			txn_spill_merged_pad_0(frame);
			return;
		}
		if (frame->named_live)
		{
			frame->named_live = false;
			std_vec_free_Checked(&frame->named, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
		}
		frame->own_live = false;
		std_vec_free_Checked(&frame->own, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
		frame->listed_live = false;
		std_vec_free_Checked(&frame->listed.f0, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	}
	frame->ok = true;
	txn_spill_merged_pad_0(frame);
}

/*
 * txn_spill_merged merges a transaction's newest spill file of a storage's
 * kind with its earlier files newest first while each is no larger than the
 * files merged so far, as an LSM's tiers merge: it joins them into one new
 * file, and for a merge of rows the tuple-id files of the same numbers into
 * one of the merged file's number, pends the merged file in the process's
 * slot with the lowest number it replaces, lists it in place of the files it
 * joined, and clears the slot, until the newest file has no earlier file to
 * take.
 */
static bool
txn_spill_merged(const TxnSpillHomeOps *home, HostIoIo *io, HostSharedRecord record, const HostStorage *storage,
				 uint64 xid, TxnRunsKind kind, TxnRunsTail tail, HostError *error)
{
	txn_spill_merged_frame_0_t frame = {.home = home, .io = io, .record = record, .storage = storage, .xid = xid,
	.kind = kind, .tail = tail, .error = error};

	std_landing(&frame, txn_spill_merged_body_0, txn_spill_merged_pad_0);
	return frame.ok;
}

/* txn_TupleFileU64_t is folded's count, (File, u64): a listed spill file and how many of its versions count at its transaction's entry. */
typedef struct txn_TupleFileU64_t
{
	TxnSpillFile f0;
	uint64		f1;
} txn_TupleFileU64_t;

/* txn_spill_has_xid reports whether a list of xids holds one. */
static bool
txn_spill_has_xid(const StdVec *xids, uint64 xid)
{
	for (size_t k = 0; k < xids->len; k++)
		if (STD_VEC_AT(xids, uint64, k) == xid)
			return true;
	return false;
}

/* txn_spill_xids_closure_0 is a removal's test: a file of one of the xids its captures list. */
static bool
txn_spill_xids_closure_0(void *captures, const TxnSpillFile *file)
{
	return txn_spill_has_xid(captures, file->xid);
}

/* txn_spill_entered reports whether a spill file's kind counts its versions at its transaction's entry: rows, entries, and value and free space map versions. */
static bool
txn_spill_entered(TxnRunsKind kind)
{
	return kind == TXN_RUNS_KIND_SPILLED_ROWS || kind == TXN_RUNS_KIND_SPILLED_ENTRIES ||
		kind == TXN_RUNS_KIND_SPILLED_VALUES || kind == TXN_RUNS_KIND_SPILLED_FREE;
}

/* txn_spill_file_counted_frame_0_t is file_counted's state across its pad: the file's path, its bytes, and their decoding, while live. */
typedef struct txn_spill_file_counted_frame_0_t
{
	HostIoIo   *io;
	HostSharedRecord state;
	const HostStorage *storage;
	const TxnSpillFile *file;
	uint64	   *result;
	HostError  *error;
	bool		ok;
	StdPathBuf	path;
	bool		path_live;
	StdString	name;
	bool		name_live;
	StdVec		bytes;
	bool		bytes_live;
	TxnRunsDecoded decoded;
	bool		decoded_live;
} txn_spill_file_counted_frame_0_t;

/* txn_spill_file_counted_pad_0 is file_counted's pad: what it holds, while live. */
static void
txn_spill_file_counted_pad_0(void *frame_)
{
	txn_spill_file_counted_frame_0_t *frame = frame_;

	if (frame->decoded_live)
	{
		frame->decoded_live = false;
		txn_runs_Decoded_drop(&frame->decoded);
	}
	if (frame->bytes_live)
	{
		frame->bytes_live = false;
		std_vec_free_Checked(&frame->bytes, 1, 1);
	}
	if (frame->name_live)
	{
		frame->name_live = false;
		std_vec_free_Checked(&frame->name, 1, 1);
	}
	if (frame->path_live)
	{
		frame->path_live = false;
		std_vec_free_Checked(&frame->path.bytes, 1, 1);
	}
}

/* txn_spill_file_counted_body_0 is file_counted's body. */
static void
txn_spill_file_counted_body_0(void *frame_)
{
	txn_spill_file_counted_frame_0_t *frame = frame_;
	TxnRunsNames names;
	ChunksErrorError failed;
	StdPathBuf	joined;

	if (!txn_spill_names(&TXN_SPILL_STATE_HOME_OPS, frame->state, frame->storage, frame->file->kind,
						 frame->file->xid, &names, frame->error))
		return;
	txn_runs_spill_directory(frame->storage->database, txn_state_own(frame->state), &frame->path);
	frame->path_live = true;
	txn_runs_spill_name(&names, frame->file->number, &frame->name);
	frame->name_live = true;
	std_path_join(frame->path.bytes.ptr, frame->path.bytes.len, (const char *) frame->name.ptr, frame->name.len,
				  &joined);
	std_vec_free_Checked(&frame->path.bytes, 1, 1);
	frame->path = joined;
	if (!chunks_file_read_file(HOST_IO_IO_READ_DOORS(frame->io), frame->path.bytes.ptr, frame->path.bytes.len,
							   &frame->bytes, &failed))
	{
		*frame->error = txn_base_storage(&failed);
		txn_spill_file_counted_pad_0(frame);
		return;
	}
	frame->bytes_live = true;
	if (!txn_runs_decode(frame->bytes.ptr, frame->bytes.len, &names, frame->file->hash, &frame->decoded,
						 frame->error))
	{
		txn_spill_file_counted_pad_0(frame);
		return;
	}
	frame->decoded_live = true;
	*frame->result = txn_spill_counted(&frame->decoded);
	frame->ok = true;
	txn_spill_file_counted_pad_0(frame);
}

/* txn_spill_file_counted reads a storage's listed spill file back and counts its versions as they count at its transaction's entry. */
static bool
txn_spill_file_counted(HostIoIo *io, HostSharedRecord state, const HostStorage *storage, const TxnSpillFile *file,
					   uint64 *result, HostError *error)
{
	txn_spill_file_counted_frame_0_t frame = {.io = io, .state = state, .storage = storage, .file = file,
	.result = result, .error = error};

	std_landing(&frame, txn_spill_file_counted_body_0, txn_spill_file_counted_pad_0);
	return frame.ok;
}

/*
 * txn_spill_uncounted takes back at a spill file's transaction's entry what
 * its rows, entries, and value and free space map versions added, once its
 * removal from its manifest took it out, read before any checkpoint after
 * that removal unlinks it.
 */
static bool
txn_spill_uncounted(HostIoIo *io, HostSharedRecord state, const HostStorage *storage, const TxnSpillFile *file,
					HostError *error)
{
	uint64		count;
	HostXid		xid = {file->xid};
	bool		has;
	uint64		added;

	if (!txn_spill_entered(file->kind))
		return true;
	return txn_spill_file_counted(io, state, storage, file, &count, error) &&
		txn_status_add(io, xid, std_wrapping_sub_u64(0, count), &has, &added, error);
}

/* txn_spill_folded_frame_0_t is folded's state across its pad: the files listed, the xids applied, the counts, and the files removed, while live. */
typedef struct txn_spill_folded_frame_0_t
{
	HostIoIo   *io;
	HostSharedRecord state;
	HostLsn		point;
	HostError  *error;
	bool		ok;
	HostStorage storage;
	txn_TupleVecFileU64_t listed;
	bool		listed_live;
	/* applied is a Vec<u64>. */
	StdVec		applied;
	bool		applied_live;
	/* counts is a Vec<(File, u64)>. */
	StdVec		counts;
	bool		counts_live;
	/* gone is a Vec<File>. */
	StdVec		gone;
	bool		gone_live;
} txn_spill_folded_frame_0_t;

/* txn_spill_folded_pad_0 is folded's pad: what it holds, while live. */
static void
txn_spill_folded_pad_0(void *frame_)
{
	txn_spill_folded_frame_0_t *frame = frame_;

	if (frame->gone_live)
	{
		frame->gone_live = false;
		std_vec_free_Checked(&frame->gone, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	}
	if (frame->counts_live)
	{
		frame->counts_live = false;
		std_vec_free_Checked(&frame->counts, sizeof(txn_TupleFileU64_t), _Alignof(txn_TupleFileU64_t));
	}
	if (frame->applied_live)
	{
		frame->applied_live = false;
		std_vec_free_Checked(&frame->applied, sizeof(uint64), _Alignof(uint64));
	}
	if (frame->listed_live)
	{
		frame->listed_live = false;
		std_vec_free_Checked(&frame->listed.f0, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	}
}

/* txn_spill_folded_frozen reports whether a spill file is a frozen insert's, of a writer below the first normal xid, that a fold at a point passes. */
static bool
txn_spill_folded_frozen(const TxnSpillFile *file, HostLsn point)
{
	return file->xid < TXN_STATUS_FIRST_NORMAL && file->passed <= point;
}

/* txn_spill_folded_freed reports whether the listed files hold a free space map's spill of an xid. */
static bool
txn_spill_folded_freed(const StdVec *listed, uint64 xid)
{
	for (size_t k = 0; k < listed->len; k++)
	{
		const TxnSpillFile *file = &STD_VEC_AT(listed, TxnSpillFile, k);

		if (file->xid == xid && file->kind == TXN_RUNS_KIND_SPILLED_FREE)
			return true;
	}
	return false;
}

/*
 * txn_spill_folded_gather gathers the xids whose versions the fold applies,
 * by their commits or, for an xid with a free space map's spill listed, its
 * abort, deciding every xid before it counts a file, so a transaction a crash
 * ended, whose other spills stay listed until a checkpoint names them, is
 * counted whatever its files' order; then it gathers the counts of their
 * files, but the residuals the fold at the point listed.
 */
static bool
txn_spill_folded_gather(txn_spill_folded_frame_0_t *frame)
{
	for (size_t k = 0; k < frame->listed.f0.len; k++)
	{
		const TxnSpillFile *file = &STD_VEC_AT(&frame->listed.f0, TxnSpillFile, k);
		HostXid		xid = {file->xid};
		TxnStatusCommit commit;
		bool		applies;

		if (file->xid < TXN_STATUS_FIRST_NORMAL || txn_spill_has_xid(&frame->applied, file->xid))
			continue;
		if (!txn_status_committed(frame->io, xid, &commit, frame->error))
			return false;
		if (commit.tag == TXN_STATUS_COMMIT_AT)
			applies = commit.u.at <= frame->point;
		else
		{
			HostStatus	status = HOST_STATUS_IN_PROGRESS;

			if (txn_spill_folded_freed(&frame->listed.f0, file->xid) &&
				!host_status(HOST_IO_IO_IO_DOORS(frame->io), xid, &status, frame->error))
				return false;
			applies = status == HOST_STATUS_ABORTED;
		}
		if (!applies)
			continue;
		if (!std_vec_try_reserve(&frame->applied, 1, sizeof(uint64), _Alignof(uint64)))
		{
			*frame->error = host_Error_out_of_memory();
			return false;
		}
		std_vec_push_Checked(&frame->applied, &file->xid, sizeof(uint64), _Alignof(uint64));
	}
	for (size_t k = 0; k < frame->listed.f0.len; k++)
	{
		const TxnSpillFile *file = &STD_VEC_AT(&frame->listed.f0, TxnSpillFile, k);
		txn_TupleFileU64_t count = {*file};

		if (file->xid < TXN_STATUS_FIRST_NORMAL || !txn_spill_has_xid(&frame->applied, file->xid) ||
			!txn_spill_entered(file->kind) || file->from == frame->point)
			continue;
		if (!std_vec_try_reserve(&frame->counts, 1, sizeof(count), _Alignof(txn_TupleFileU64_t)))
		{
			*frame->error = host_Error_out_of_memory();
			return false;
		}
		if (!txn_spill_file_counted(frame->io, frame->state, &frame->storage, file, &count.f1, frame->error))
			return false;
		std_vec_push_Checked(&frame->counts, &count, sizeof(count), _Alignof(txn_TupleFileU64_t));
	}
	return true;
}

/* txn_spill_folded_closure_0 is folded's removal's test: a frozen file the point passes, or an applied transaction's file that is no residual of the fold. */
static bool
txn_spill_folded_closure_0(void *captures, const TxnSpillFile *file)
{
	const txn_spill_folded_frame_0_t *frame = captures;

	return txn_spill_folded_frozen(file, frame->point) ||
		(txn_spill_has_xid(&frame->applied, file->xid) && file->from != frame->point);
}

/* txn_spill_folded_body_0 is folded's body. */
static void
txn_spill_folded_body_0(void *frame_)
{
	txn_spill_folded_frame_0_t *frame = frame_;
	TxnStatePersistence persistence;
	bool		frozen = false;

	if (!txn_state_described(frame->state, &frame->storage, &persistence))
	{
		frame->ok = true;
		return;
	}
	if (!txn_spill_listed(&TXN_SPILL_STATE_HOME_OPS, frame->io, frame->state, &frame->storage, &frame->listed,
						  frame->error))
		return;
	frame->listed_live = true;
	std_vec_new(&frame->applied, sizeof(uint64), _Alignof(uint64));
	frame->applied_live = true;
	std_vec_new(&frame->counts, sizeof(txn_TupleFileU64_t), _Alignof(txn_TupleFileU64_t));
	frame->counts_live = true;
	if (!txn_spill_folded_gather(frame))
	{
		txn_spill_folded_pad_0(frame);
		return;
	}
	for (size_t k = 0; k < frame->listed.f0.len && !frozen; k++)
		frozen = txn_spill_folded_frozen(&STD_VEC_AT(&frame->listed.f0, TxnSpillFile, k), frame->point);
	if (frame->applied.len == 0 && !frozen)
	{
		frame->ok = true;
		txn_spill_folded_pad_0(frame);
		return;
	}
	if (!host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-spill-folded-counted", frame->error) ||
		!txn_spill_removed(&TXN_SPILL_STATE_HOME_OPS, frame->io, frame->state, &frame->storage,
						   txn_spill_folded_closure_0, frame, &frame->gone, frame->error))
	{
		txn_spill_folded_pad_0(frame);
		return;
	}
	frame->gone_live = true;
	for (size_t k = 0; k < frame->counts.len; k++)
	{
		const txn_TupleFileU64_t *count = &STD_VEC_AT(&frame->counts, txn_TupleFileU64_t, k);
		bool		gone = false;

		for (size_t j = 0; j < frame->gone.len && !gone; j++)
			gone = txn_spill_File_PartialEq_eq(&STD_VEC_AT(&frame->gone, TxnSpillFile, j), &count->f0);
		if (gone)
		{
			HostXid		xid = {count->f0.xid};
			bool		has;
			uint64		added;

			if (!txn_status_add(frame->io, xid, std_wrapping_sub_u64(0, count->f1), &has, &added, frame->error))
			{
				txn_spill_folded_pad_0(frame);
				return;
			}
		}
	}
	frame->ok = true;
	txn_spill_folded_pad_0(frame);
}

/*
 * txn_spill_folded takes out of a storage's manifest the spills of every
 * transaction whose commit a fold at a point applies, and takes back at
 * each transaction's entry what its spilled rows, entries, and value and
 * free space map versions added, reading each file's count before it leaves
 * the manifest; only the counts of the files its own removal took out go
 * back. An aborted transaction's free space map spill, which its abort
 * left, goes too, the fold having applied its takes as it applies an aborted
 * take in the memtable, and so does a file of a writer below the first
 * normal xid, a frozen insert's, which counts nothing, once the point
 * reaches its passing point. A residual the fold at the point listed stays,
 * holding the versions it kept, and takes the place of the rest of its
 * transaction's files of its kind (bugs row 228).
 */
bool
txn_spill_folded(HostIoIo *io, HostSharedRecord state, HostLsn point, HostError *error)
{
	txn_spill_folded_frame_0_t frame = {.io = io, .state = state, .point = point, .error = error};

	std_landing(&frame, txn_spill_folded_body_0, txn_spill_folded_pad_0);
	return frame.ok;
}

/* txn_TupleRefSliceU8RefSliceU8U_t is a residual's kept version, (&[u8], &[u8], usize): its key, its record's bytes, and its place among its group's. */
typedef struct txn_TupleRefSliceU8RefSliceU8U_t
{
	StdBytes	f0;
	StdBytes	f1;
	size_t		f2;
} txn_TupleRefSliceU8RefSliceU8U_t;

/* TxnSpillGroup is Group: a residual's transaction and kind, its spill's names, and the kept versions it holds. */
typedef struct TxnSpillGroup
{
	uint64		xid;
	TxnRunsKind kind;
	TxnRunsNames names;
	/* versions is a Vec<(&[u8], &[u8], usize)>. */
	StdVec		versions;
} TxnSpillGroup;

/* txn_spill_kept_cmp orders a group's kept versions by their keys, then their places, as a stable sort by key does. */
static int
txn_spill_kept_cmp(const void *a, const void *b)
{
	const txn_TupleRefSliceU8RefSliceU8U_t *x = a;
	const txn_TupleRefSliceU8RefSliceU8U_t *y = b;
	int			order = (int) std_bytes_Ord_cmp(x->f0.ptr, x->f0.len, y->f0.ptr, y->f0.len);

	if (order != 0)
		return order;
	return x->f2 < y->f2 ? -1 : x->f2 > y->f2;
}

/* txn_spill_group_written_frame_0_t is group_written's state across its pad: the writing, the distinct keys, the filter, and a key's records, while live. */
typedef struct txn_spill_group_written_frame_0_t
{
	HostIoIoDoors io;
	const StdPathBuf *dir;
	const TxnSpillGroup *group;
	uint32		number;
	HostLsn		point;
	TxnSpillFile *result;
	HostError  *error;
	bool		ok;
	TxnRunsWriting *writing;
	bool		writing_live;
	/* keys is a Vec<&[u8]> of the distinct keys. */
	StdVec		keys;
	bool		keys_live;
	TxnSpillFilter filter;
	bool		filter_live;
	/* records is a Vec<&[u8]> of a key's records. */
	StdVec		records;
	bool		records_live;
} txn_spill_group_written_frame_0_t;

/* txn_spill_group_written_pad_0 is group_written's pad: what it holds, while live. */
static void
txn_spill_group_written_pad_0(void *frame_)
{
	txn_spill_group_written_frame_0_t *frame = frame_;

	if (frame->records_live)
	{
		frame->records_live = false;
		std_vec_free_Checked(&frame->records, sizeof(StdBytes), _Alignof(StdBytes));
	}
	if (frame->filter_live)
	{
		frame->filter_live = false;
		std_vec_free_Checked(&frame->filter.words, sizeof(uint64), _Alignof(uint64));
	}
	if (frame->keys_live)
	{
		frame->keys_live = false;
		std_vec_free_Checked(&frame->keys, sizeof(StdBytes), _Alignof(StdBytes));
	}
	if (frame->writing_live)
	{
		frame->writing_live = false;
		txn_runs_Writing_drop(frame->writing);
	}
}

/* txn_spill_group_written_body_0 is group_written's body. */
static void
txn_spill_group_written_body_0(void *frame_)
{
	txn_spill_group_written_frame_0_t *frame = frame_;
	const StdVec *versions = &frame->group->versions;
	TxnRunsTail none = {.tag = TXN_RUNS_TAIL_NONE};
	bool		some;
	RepoRunFile run;

	std_vec_new(&frame->keys, sizeof(StdBytes), _Alignof(StdBytes));
	frame->keys_live = true;
	std_vec_new(&frame->records, sizeof(StdBytes), _Alignof(StdBytes));
	frame->records_live = true;
	if (!std_vec_try_reserve_exact(&frame->keys, versions->len, sizeof(StdBytes), _Alignof(StdBytes)) ||
		!std_vec_try_reserve_exact(&frame->records, versions->len, sizeof(StdBytes), _Alignof(StdBytes)))
	{
		*frame->error = host_Error_out_of_memory();
		txn_spill_group_written_pad_0(frame);
		return;
	}
	for (size_t k = 0; k < versions->len; k++)
	{
		StdBytes	key = STD_VEC_AT(versions, txn_TupleRefSliceU8RefSliceU8U_t, k).f0;
		const StdBytes *last = frame->keys.len == 0 ? NULL :
			&STD_VEC_AT(&frame->keys, StdBytes, frame->keys.len - 1);

		if (last == NULL || last->len != key.len || memcmp(last->ptr, key.ptr, key.len) != 0)
			std_vec_push_Checked(&frame->keys, &key, sizeof(key), _Alignof(StdBytes));
	}
	if (!txn_spill_Filter_of(frame->keys.len, frame->keys.ptr, frame->keys.len, &frame->filter, frame->error))
	{
		txn_spill_group_written_pad_0(frame);
		return;
	}
	frame->filter_live = true;
	frame->writing = txn_runs_Writing_spill(frame->dir->bytes.ptr, frame->dir->bytes.len, &frame->group->names, none,
											frame->number);
	frame->writing_live = true;
	for (size_t k = 0, at = 0; k < frame->keys.len; k++)
	{
		const StdBytes *key = &STD_VEC_AT(&frame->keys, StdBytes, k);

		frame->records.len = 0;
		for (; at < versions->len; at++)
		{
			const txn_TupleRefSliceU8RefSliceU8U_t *version = &STD_VEC_AT(versions, txn_TupleRefSliceU8RefSliceU8U_t,
																		  at);

			if (version->f0.len != key->len || memcmp(version->f0.ptr, key->ptr, key->len) != 0)
				break;
			std_vec_push_Checked(&frame->records, &version->f1, sizeof(StdBytes), _Alignof(StdBytes));
		}
		if (!txn_runs_Writing_key(frame->writing, frame->io, key->ptr, key->len, frame->records.ptr,
								  frame->records.len, frame->error))
		{
			txn_spill_group_written_pad_0(frame);
			return;
		}
	}
	if (!txn_runs_Writing_end(frame->writing, frame->io, frame->filter.words.ptr, frame->filter.words.len, &some,
							  &run, frame->error))
	{
		txn_spill_group_written_pad_0(frame);
		return;
	}
	if (!some)
	{
		*frame->error = txn_spill_internal("a residual wrote no key");
		txn_spill_group_written_pad_0(frame);
		return;
	}
	*frame->result = (TxnSpillFile)
	{
		.xid = frame->group->xid, .kind = frame->group->kind, .number = frame->number, .len = run.len,
			.hash = run.hash, .bits = txn_spill_Filter_bits(&frame->filter), .from = frame->point
	};
	frame->ok = true;
	txn_spill_group_written_pad_0(frame);
}

/* txn_spill_group_written writes a residual's file of a group's kept versions, sorted by key, each key's newest first, with its filter, synced. */
static bool
txn_spill_group_written(HostIoIoDoors io, const StdPathBuf *dir, const TxnSpillGroup *group, uint32 number,
						HostLsn point, TxnSpillFile *result, HostError *error)
{
	txn_spill_group_written_frame_0_t frame = {.io = io, .dir = dir, .group = group, .number = number,
	.point = point, .result = result, .error = error};

	std_landing(&frame, txn_spill_group_written_body_0, txn_spill_group_written_pad_0);
	return frame.ok;
}

/* txn_spill_residual_captures_t is residual's edit's captures: the state, the groups, the slots' spills, the directory, the point, and the files it made. */
typedef struct txn_spill_residual_captures_t
{
	HostSharedRecord state;
	const StdVec *groups;
	const StdVec *slotted;
	const StdPathBuf *dir;
	HostLsn		point;
	StdVec	   *made;
} txn_spill_residual_captures_t;

/*
 * txn_spill_residual_closure_0 is residual's edit: each group's file written,
 * numbered past its transaction's newest file of the kind listed or pended in
 * a slot through the state's numbered, and listed after that newest listed
 * file.
 */
static bool
txn_spill_residual_closure_0(void *captures, HostSharedLongIo *io, StdVec *files, HostError *error)
{
	const txn_spill_residual_captures_t *c = captures;

	if (!std_vec_try_reserve_exact(c->made, c->groups->len, sizeof(TxnSpillFile), _Alignof(TxnSpillFile)))
	{
		*error = host_Error_out_of_memory();
		return false;
	}
	for (size_t g = 0; g < c->groups->len; g++)
	{
		const TxnSpillGroup *group = &STD_VEC_AT(c->groups, TxnSpillGroup, g);
		uint32		past = 0;
		size_t		at = files->len;
		TxnSpillFile file;

		for (size_t k = 0; k < files->len; k++)
		{
			const TxnSpillFile *listed = &STD_VEC_AT(files, TxnSpillFile, k);

			if (listed->xid == group->xid && listed->kind == group->kind)
			{
				past = Max(past, listed->number + 1);
				at = k + 1;
			}
		}
		for (size_t k = 0; k < c->slotted->len; k++)
		{
			const txn_TupleU64KindU32OptionU32_t *slot = &STD_VEC_AT(c->slotted, txn_TupleU64KindU32OptionU32_t, k);

			if (slot->f0 == group->xid && slot->f1 == group->kind)
				past = Max(past, slot->f2 + 1);
		}
		if (!txn_spill_group_written(HOST_SHARED_LONG_IO_IO_DOORS(io), c->dir, group,
									 txn_state_numbered(c->state, past), c->point, &file, error))
			return false;
		std_vec_insert(files, at, &file, sizeof(file), _Alignof(TxnSpillFile), RUST_FILE, 1418, 13);
		std_vec_push_Checked(c->made, &file, sizeof(file), _Alignof(TxnSpillFile));
	}
	return true;
}

/* txn_spill_residual_frame_0_t is residual's state across its pad: the groups, the slots' spills, and the directory, while live. */
typedef struct txn_spill_residual_frame_0_t
{
	HostIoIo   *io;
	HostSharedRecord state;
	HostLsn		point;
	const TxnSpillKept *kept;
	size_t		kept_len;
	StdVec	   *result;
	HostError  *error;
	bool		ok;
	HostStorage storage;
	/* groups is a Vec<Group>. */
	StdVec		groups;
	bool		groups_live;
	/* slotted is a Vec<Slotted>. */
	StdVec		slotted;
	bool		slotted_live;
	StdPathBuf	dir;
	bool		dir_live;

	/*
	 * counted is how many groups have their counts added at their entries
	 * with their files not yet listed, which the pad gives back.
	 */
	size_t		counted;
} txn_spill_residual_frame_0_t;

static void txn_spill_residual_given_back(txn_spill_residual_frame_0_t *frame, size_t groups);

/* txn_spill_residual_pad_0 is residual's pad: the counts of groups not listed, then what it holds, while live. */
static void
txn_spill_residual_pad_0(void *frame_)
{
	txn_spill_residual_frame_0_t *frame = frame_;

	if (frame->counted > 0)
	{
		txn_spill_residual_given_back(frame, frame->counted);
		frame->counted = 0;
	}
	if (frame->dir_live)
	{
		frame->dir_live = false;
		std_vec_free_Checked(&frame->dir.bytes, 1, 1);
	}
	if (frame->slotted_live)
	{
		frame->slotted_live = false;
		std_vec_free_Checked(&frame->slotted, sizeof(txn_TupleU64KindU32OptionU32_t),
							 _Alignof(txn_TupleU64KindU32OptionU32_t));
	}
	if (frame->groups_live)
	{
		frame->groups_live = false;
		for (size_t k = 0; k < frame->groups.len; k++)
			std_vec_free_Checked(&STD_VEC_AT(&frame->groups, TxnSpillGroup, k).versions,
								 sizeof(txn_TupleRefSliceU8RefSliceU8U_t),
								 _Alignof(txn_TupleRefSliceU8RefSliceU8U_t));
		std_vec_free_Checked(&frame->groups, sizeof(TxnSpillGroup), _Alignof(TxnSpillGroup));
	}
}

/* txn_spill_residual_grouped groups the kept versions by their writers' top-level xids and their kinds, each group with its spill's names. */
static bool
txn_spill_residual_grouped(txn_spill_residual_frame_0_t *frame)
{
	std_vec_new(&frame->groups, sizeof(TxnSpillGroup), _Alignof(TxnSpillGroup));
	frame->groups_live = true;
	for (size_t k = 0; k < frame->kept_len; k++)
	{
		const TxnSpillKept *kept = &frame->kept[k];
		TxnMemtableVersionRecord record;
		TxnMemtableVersionWriter writer;
		uint64		xid;
		TxnSpillGroup *group = NULL;
		txn_TupleRefSliceU8RefSliceU8U_t version = {{kept->key.ptr, kept->key.len}, {kept->bytes.ptr, kept->bytes.len}};

		if (!txn_memtable_version_stored(kept->bytes.ptr, kept->bytes.len, &record, frame->error))
			return false;
		writer = txn_memtable_version_Record_writer(&record);
		xid = writer.has_top ? writer.top : writer.xid;
		for (size_t g = 0; g < frame->groups.len && group == NULL; g++)
		{
			TxnSpillGroup *at = &STD_VEC_AT(&frame->groups, TxnSpillGroup, g);

			if (at->xid == xid && at->kind == kept->kind)
				group = at;
		}
		if (group == NULL)
		{
			TxnSpillGroup made = {.xid = xid, .kind = kept->kind};

			if (!std_vec_try_reserve(&frame->groups, 1, sizeof(made), _Alignof(TxnSpillGroup)))
			{
				*frame->error = host_Error_out_of_memory();
				return false;
			}
			if (!txn_spill_names(&TXN_SPILL_STATE_HOME_OPS, frame->state, &frame->storage, kept->kind, xid,
								 &made.names, frame->error))
				return false;
			std_vec_new(&made.versions, sizeof(version), _Alignof(txn_TupleRefSliceU8RefSliceU8U_t));
			std_vec_push_Checked(&frame->groups, &made, sizeof(made), _Alignof(TxnSpillGroup));
			group = &STD_VEC_AT(&frame->groups, TxnSpillGroup, frame->groups.len - 1);
		}
		version.f2 = group->versions.len;
		if (!std_vec_try_reserve(&group->versions, 1, sizeof(version), _Alignof(txn_TupleRefSliceU8RefSliceU8U_t)))
		{
			*frame->error = host_Error_out_of_memory();
			return false;
		}
		std_vec_push_Checked(&group->versions, &version, sizeof(version), _Alignof(txn_TupleRefSliceU8RefSliceU8U_t));
	}
	for (size_t g = 0; g < frame->groups.len; g++)
	{
		StdVec	   *versions = &STD_VEC_AT(&frame->groups, TxnSpillGroup, g).versions;

		qsort(versions->ptr, versions->len, sizeof(txn_TupleRefSliceU8RefSliceU8U_t), txn_spill_kept_cmp);
	}
	return true;
}

/*
 * txn_spill_residual_given_back takes the counts of the first groups back
 * from their transactions' entries after a failed listing or a panic; a give-back that
 * fails leaves an overcount, which only keeps an entry longer, where an
 * undercount could free it under its listed versions.
 */
static void
txn_spill_residual_given_back(txn_spill_residual_frame_0_t *frame, size_t groups)
{
	for (size_t k = 0; k < groups; k++)
	{
		const TxnSpillGroup *group = &STD_VEC_AT(&frame->groups, TxnSpillGroup, k);
		HostXid		xid = {group->xid};
		HostError	error;
		bool		has;
		uint64		added;

		if (!txn_status_add(frame->io, xid, std_wrapping_sub_u64(0, (uint64) group->versions.len), &has, &added,
							&error))
			host_Error_drop(&error);
	}
}

/* txn_spill_residual_body_0 is residual's body. */
static void
txn_spill_residual_body_0(void *frame_)
{
	txn_spill_residual_frame_0_t *frame = frame_;
	TxnStatePersistence persistence;
	txn_spill_residual_captures_t captures;

	std_vec_new(frame->result, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	if (frame->kept_len == 0 || !txn_state_described(frame->state, &frame->storage, &persistence))
	{
		frame->ok = true;
		return;
	}
	if (!txn_spill_residual_grouped(frame))
	{
		txn_spill_residual_pad_0(frame);
		return;
	}
	txn_runs_spill_directory(frame->storage.database, txn_state_own(frame->state), &frame->dir);
	frame->dir_live = true;
	if (!txn_spill_pended(&TXN_SPILL_STATE_HOME_OPS, frame->state, &frame->storage, &frame->slotted, frame->error))
	{
		txn_spill_residual_pad_0(frame);
		return;
	}
	frame->slotted_live = true;
	for (size_t k = 0; k < frame->groups.len; k++)
	{
		const TxnSpillGroup *group = &STD_VEC_AT(&frame->groups, TxnSpillGroup, k);
		HostXid		xid = {group->xid};
		bool		has;
		uint64		added;

		if (!txn_status_add(frame->io, xid, (uint64) group->versions.len, &has, &added, frame->error))
		{
			txn_spill_residual_pad_0(frame);
			return;
		}
		frame->counted = k + 1;
	}
	captures = (txn_spill_residual_captures_t)
	{
		frame->state, &frame->groups, &frame->slotted, &frame->dir, frame->point, frame->result
	};
	if (!txn_spill_changed(&TXN_SPILL_STATE_HOME_OPS, frame->io, frame->state, &frame->storage,
						   txn_spill_residual_closure_0, &captures, frame->error))
	{
		txn_spill_residual_pad_0(frame);
		return;
	}
	frame->counted = 0;
	frame->ok = true;
	txn_spill_residual_pad_0(frame);
}

/*
 * txn_spill_residual lists, before a fold at a point installs, a residual of
 * each transaction's spills of a map kind whose versions the fold keeps
 * (bugs row 228), all in one hold of the manifest's writers' lock, so a
 * concurrent fold lists its own after: a file holding the kept versions
 * alone, each key's newest first, numbered past the transaction's newest
 * file of the kind listed or pended in a slot through the state's numbered,
 * as the committed arm numbers its files, written and synced, listed at the
 * place of that newest listed file, its versions counted at the
 * transaction's entry before the hold, since the status lock can't be taken
 * inside it, and given back if the listing fails, so a listed residual is
 * always counted. Readers read it in place of every other file of its
 * transaction and kind once the base's fold point reaches the fold's
 * (files), so they see the kept versions in its sources until the install
 * and in it after, and never hold its versions in the delta's budget. A fold
 * at the same point keeps the same versions of the same sources, so two such
 * residuals hold the same versions. It answers the files it listed, which a
 * fold whose install is refused takes back out (unlisted).
 */
bool
txn_spill_residual(HostIoIo *io, HostSharedRecord state, HostLsn point, const TxnSpillKept *kept, size_t kept_len,
				   StdVec *result, HostError *error)
{
	txn_spill_residual_frame_0_t frame = {.io = io, .state = state, .point = point, .kept = kept,
	.kept_len = kept_len, .result = result, .error = error};

	std_landing(&frame, txn_spill_residual_body_0, txn_spill_residual_pad_0);
	if (!frame.ok)
		std_vec_free_Checked(result, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	return frame.ok;
}

/* txn_spill_unlisted_closure_0 is unlisted's removal's test: one of the fold's residuals. */
static bool
txn_spill_unlisted_closure_0(void *captures, const TxnSpillFile *file)
{
	const StdVec *residuals = captures;

	for (size_t k = 0; k < residuals->len; k++)
		if (txn_spill_File_PartialEq_eq(&STD_VEC_AT(residuals, TxnSpillFile, k), file))
			return true;
	return false;
}

/* txn_spill_unlisted_frame_t is unlisted's state across its pad: the files it took out, while live. */
typedef struct txn_spill_unlisted_frame_t
{
	HostIoIo   *io;
	HostSharedRecord state;
	const StdVec *residuals;
	StdVec		gone;
	bool		gone_live;
	HostError  *error;
	bool		ok;
} txn_spill_unlisted_frame_t;

/* txn_spill_unlisted_body takes the residuals out and takes back each one's count. */
static void
txn_spill_unlisted_body(void *frame_)
{
	txn_spill_unlisted_frame_t *frame = frame_;
	HostStorage storage;
	TxnStatePersistence persistence;

	if (frame->residuals->len == 0 || !txn_state_described(frame->state, &storage, &persistence))
	{
		frame->ok = true;
		return;
	}
	if (!txn_spill_removed(&TXN_SPILL_STATE_HOME_OPS, frame->io, frame->state, &storage, txn_spill_unlisted_closure_0,
						   (void *) frame->residuals, &frame->gone, frame->error))
		return;
	frame->gone_live = true;
	for (size_t k = 0; k < frame->gone.len; k++)
		if (!txn_spill_uncounted(frame->io, frame->state, &storage, &STD_VEC_AT(&frame->gone, TxnSpillFile, k),
								 frame->error))
			return;
	frame->ok = true;
}

/* txn_spill_unlisted_pad frees the files unlisted took out, while live. */
static void
txn_spill_unlisted_pad(void *frame_)
{
	txn_spill_unlisted_frame_t *frame = frame_;

	if (frame->gone_live)
		std_vec_free_Checked(&frame->gone, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	frame->gone_live = false;
}

/*
 * txn_spill_unlisted takes a fold's residuals back out of its storage's
 * manifest when its install is refused, with their counts; a fold whose
 * install raised leaves them for the next fold's drop or the start, which
 * no reader reads meanwhile.
 */
bool
txn_spill_unlisted(HostIoIo *io, HostSharedRecord state, const StdVec *residuals, HostError *error)
{
	txn_spill_unlisted_frame_t frame = {.io = io, .state = state, .residuals = residuals, .error = error};

	std_landing(&frame, txn_spill_unlisted_body, txn_spill_unlisted_pad);
	txn_spill_unlisted_pad(&frame);
	return frame.ok;
}

/* txn_spill_Chain_drop frees a carry's chain: its key and its versions' records. */
void
txn_spill_Chain_drop(TxnSpillChain *self)
{
	std_vec_free_Checked(&self->key, 1, 1);
	for (size_t k = 0; k < self->versions.len; k++)
		std_vec_free_Checked(&STD_VEC_AT(&self->versions, txn_TupleU64VecU8_t, k).f1, 1, 1);
	std_vec_free_Checked(&self->versions, sizeof(txn_TupleU64VecU8_t), _Alignof(txn_TupleU64VecU8_t));
}

/* TxnSpillLayerFile is one file of a carry's layer: its writer's top-level xid and tree, with its keys, each with its records newest first. */
typedef struct TxnSpillLayerFile
{
	uint64		xid;
	TxnWrittenOf of;
	/* keys is a Vec<(Vec<u8>, Vec<Vec<u8>>)>. */
	StdVec		keys;
} TxnSpillLayerFile;

/* txn_spill_KeyRecords_drop frees a key with its records. */
static void
txn_spill_KeyRecords_drop(txn_TupleVecU8VecVecU8_t *self)
{
	std_vec_free_Checked(&self->f0, 1, 1);
	for (size_t k = 0; k < self->f1.len; k++)
		std_vec_free_Checked(&STD_VEC_AT(&self->f1, StdVec, k), 1, 1);
	std_vec_free_Checked(&self->f1, sizeof(StdVec), _Alignof(StdVec));
}

/* txn_spill_layers_drop frees a carry's layers, a Vec<Vec<LayerFile>>. */
static void
txn_spill_layers_drop(StdVec *layers)
{
	for (size_t d = 0; d < layers->len; d++)
	{
		StdVec	   *layer = &STD_VEC_AT(layers, StdVec, d);

		for (size_t f = 0; f < layer->len; f++)
		{
			TxnSpillLayerFile *file = &STD_VEC_AT(layer, TxnSpillLayerFile, f);

			for (size_t k = 0; k < file->keys.len; k++)
				txn_spill_KeyRecords_drop(&STD_VEC_AT(&file->keys, txn_TupleVecU8VecVecU8_t, k));
			std_vec_free_Checked(&file->keys, sizeof(txn_TupleVecU8VecVecU8_t), _Alignof(txn_TupleVecU8VecVecU8_t));
		}
		std_vec_free_Checked(layer, sizeof(TxnSpillLayerFile), _Alignof(TxnSpillLayerFile));
	}
	std_vec_free_Checked(layers, sizeof(StdVec), _Alignof(StdVec));
}

/* txn_spill_key_records_cmp orders a layer file's keys by their bytes. */
static int
txn_spill_key_records_cmp(const void *a, const void *b)
{
	const txn_TupleVecU8VecVecU8_t *x = a;
	const txn_TupleVecU8VecVecU8_t *y = b;

	return (int) std_bytes_Ord_cmp(x->f0.ptr, x->f0.len, y->f0.ptr, y->f0.len);
}

/*
 * txn_spill_layered splits a carry's chains, which it takes, into layers of
 * spill files (P2): each chain's versions, oldest first, into runs of one
 * writer's, its first run in the first layer, its second in the second, and
 * so on, so a layer's files hold a key of a tree once and listing the layers
 * oldest first keeps every chain's order, as readers read a key's versions
 * across files in their listing's order. A chain's key is copied into each
 * layer it reaches, since each file owns its keys.
 */
static bool
txn_spill_layered(StdVec *chains, StdVec *layers, HostError *error)
{
	bool		ok = true;

	std_vec_new(layers, sizeof(StdVec), _Alignof(StdVec));
	for (size_t c = 0; c < chains->len && ok; c++)
	{
		TxnSpillChain *chain = &STD_VEC_AT(chains, TxnSpillChain, c);
		size_t		depth = 0;

		for (size_t start = 0; start < chain->versions.len && ok; depth++)
		{
			uint64		xid = STD_VEC_AT(&chain->versions, txn_TupleU64VecU8_t, start).f0;
			size_t		end = start;
			txn_TupleVecU8VecVecU8_t keyed;
			StdVec	   *layer;
			TxnSpillLayerFile *file = NULL;

			while (end < chain->versions.len && STD_VEC_AT(&chain->versions, txn_TupleU64VecU8_t, end).f0 == xid)
				end++;
			if (layers->len == depth)
			{
				StdVec		fresh;

				ok = std_vec_try_reserve(layers, 1, sizeof(StdVec), _Alignof(StdVec));
				if (!ok)
					break;
				std_vec_new(&fresh, sizeof(TxnSpillLayerFile), _Alignof(TxnSpillLayerFile));
				std_vec_push_Checked(layers, &fresh, sizeof(fresh), _Alignof(StdVec));
			}
			layer = &STD_VEC_AT(layers, StdVec, depth);
			for (size_t f = 0; f < layer->len && file == NULL; f++)
				if (STD_VEC_AT(layer, TxnSpillLayerFile, f).xid == xid &&
					STD_VEC_AT(layer, TxnSpillLayerFile, f).of == chain->of)
					file = &STD_VEC_AT(layer, TxnSpillLayerFile, f);
			if (file == NULL)
			{
				TxnSpillLayerFile made = {.xid = xid, .of = chain->of};

				ok = std_vec_try_reserve(layer, 1, sizeof(made), _Alignof(TxnSpillLayerFile));
				if (!ok)
					break;
				std_vec_new(&made.keys, sizeof(txn_TupleVecU8VecVecU8_t), _Alignof(txn_TupleVecU8VecVecU8_t));
				std_vec_push_Checked(layer, &made, sizeof(made), _Alignof(TxnSpillLayerFile));
				file = &STD_VEC_AT(layer, TxnSpillLayerFile, layer->len - 1);
			}
			ok = std_vec_try_reserve(&file->keys, 1, sizeof(keyed), _Alignof(txn_TupleVecU8VecVecU8_t));
			if (!ok)
				break;
			std_vec_clone(&keyed.f0, &chain->key, 1, 1);
			std_vec_with_capacity_Checked(&keyed.f1, end - start, sizeof(StdVec), _Alignof(StdVec));
			for (size_t v = end; v > start; v--)
			{
				txn_TupleU64VecU8_t *version = &STD_VEC_AT(&chain->versions, txn_TupleU64VecU8_t, v - 1);

				std_vec_push_Checked(&keyed.f1, &version->f1, sizeof(StdVec), _Alignof(StdVec));
				std_vec_new(&version->f1, 1, 1);
			}
			std_vec_push_Checked(&file->keys, &keyed, sizeof(keyed), _Alignof(txn_TupleVecU8VecVecU8_t));
			start = end;
		}
	}
	for (size_t c = 0; c < chains->len; c++)
		txn_spill_Chain_drop(&STD_VEC_AT(chains, TxnSpillChain, c));
	std_vec_free_Checked(chains, sizeof(TxnSpillChain), _Alignof(TxnSpillChain));
	if (!ok)
	{
		*error = host_Error_out_of_memory();
		txn_spill_layers_drop(layers);
		return false;
	}
	for (size_t d = 0; d < layers->len; d++)
	{
		StdVec	   *layer = &STD_VEC_AT(layers, StdVec, d);

		for (size_t f = 0; f < layer->len; f++)
		{
			StdVec	   *keys = &STD_VEC_AT(layer, TxnSpillLayerFile, f).keys;

			qsort(keys->ptr, keys->len, sizeof(txn_TupleVecU8VecVecU8_t), txn_spill_key_records_cmp);
		}
	}
	return true;
}

/* TxnSpillSlot is a carried writer's status entry's slot, resolved before the carry's hold: its top-level xid, and its slot when it has an entry. */
typedef struct TxnSpillSlot
{
	uint64		xid;
	bool		has;
	TxnStatusRoomed roomed;
} TxnSpillSlot;

/* txn_spill_carried_frame_0_t is carried's state across its pad: the spills' directory, the manifest's path, its writers' lock, the slots, the layers, the files, a file's writing, filter, keys, and tuples, and the counts, while live. */
typedef struct txn_spill_carried_frame_0_t
{
	HostIoIo   *io;
	const HostSharedHeld *held;
	bool		(*logged) (void *captures, HostSharedLongIo *io, StdVec *chains, HostError *error);
	void	   *captures;
	HostError  *error;
	bool		ok;
	HostStorage storage;
	TxnRunsKind kinds[3];
	TxnRunsTail tails[3];
	StdPathBuf	dir;
	bool		dir_live;
	StdPathBuf	path;
	bool		path_live;
	HostSharedLongGuard writing;
	bool		writing_live;
	HostSharedLongIo long_io;
	StdVec		slotted;
	bool		slotted_live;
	StdVec		chains;
	StdVec		layers;
	bool		layers_live;
	StdVec		files;
	bool		files_live;
	TxnRunsWriting *writer;
	bool		writer_live;
	TxnSpillFilter filter;
	bool		filter_live;

	/*
	 * keys is a Vec<&[u8]> of a file's keys, and records a Vec<&[u8]> of a
	 * key's records.
	 */
	StdVec		keys;
	bool		keys_live;
	StdVec		records;
	bool		records_live;
	/* tuples is a Vec<(u64, Vec<u8>)>. */
	StdVec		tuples;
	bool		tuples_live;

	/*
	 * counts is a Vec<(u64, u64)> of each file's writer and its versions'
	 * count.
	 */
	StdVec		counts;
	bool		counts_live;

	/*
	 * writers are the top-level xids the carry may spill under, and slots
	 * their entries' slots.
	 */
	const uint64 *writers;
	size_t		writers_len;
	StdVec		slots;
	bool		slots_live;
	/* made_from is where the carry's files start in the files listed. */
	size_t		made_from;

	/*
	 * ends are the ends of the carry's records, ascending, and made the files
	 * it listed.
	 */
	StdVec		ends;
	bool		ends_live;
	StdVec		made;
	bool		made_live;
	StdVec		body;
	bool		body_live;
	HostDeltaHold hold;
	bool		hold_live;
	HostDelayCheckpoint window;
	bool		window_live;
} txn_spill_carried_frame_0_t;

/* txn_spill_carried_file_drop frees what one file of carried's holds. */
static void
txn_spill_carried_file_drop(txn_spill_carried_frame_0_t *frame)
{
	if (frame->tuples_live)
	{
		frame->tuples_live = false;
		for (size_t k = 0; k < frame->tuples.len; k++)
			std_vec_free_Checked(&STD_VEC_AT(&frame->tuples, txn_TupleU64VecU8_t, k).f1, 1, 1);
		std_vec_free_Checked(&frame->tuples, sizeof(txn_TupleU64VecU8_t), _Alignof(txn_TupleU64VecU8_t));
	}
	if (frame->records_live)
	{
		frame->records_live = false;
		std_vec_free_Checked(&frame->records, sizeof(StdBytes), _Alignof(StdBytes));
	}
	if (frame->keys_live)
	{
		frame->keys_live = false;
		std_vec_free_Checked(&frame->keys, sizeof(StdBytes), _Alignof(StdBytes));
	}
	if (frame->filter_live)
	{
		frame->filter_live = false;
		std_vec_free_Checked(&frame->filter.words, sizeof(uint64), _Alignof(uint64));
	}
	if (frame->writer_live)
	{
		frame->writer_live = false;
		txn_runs_Writing_drop(frame->writer);
	}
}

/* txn_spill_carried_pad_0 is carried's pad: what it holds, while live. */
static void
txn_spill_carried_pad_0(void *frame_)
{
	txn_spill_carried_frame_0_t *frame = frame_;

	if (frame->window_live)
	{
		frame->window_live = false;
		host_DelayCheckpoint_drop(&frame->window);
	}
	if (frame->hold_live)
	{
		frame->hold_live = false;
		host_delta_Hold_drop(&frame->hold);
	}
	if (frame->body_live)
	{
		frame->body_live = false;
		std_vec_free_Checked(&frame->body, 1, 1);
	}
	if (frame->made_live)
	{
		frame->made_live = false;
		std_vec_free_Checked(&frame->made, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	}
	if (frame->ends_live)
	{
		frame->ends_live = false;
		std_vec_free_Checked(&frame->ends, sizeof(uint64), _Alignof(uint64));
	}
	if (frame->slots_live)
	{
		frame->slots_live = false;
		std_vec_free_Checked(&frame->slots, sizeof(TxnSpillSlot), _Alignof(TxnSpillSlot));
	}
	txn_spill_carried_file_drop(frame);
	if (frame->counts_live)
	{
		frame->counts_live = false;
		std_vec_free_Checked(&frame->counts, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
	}
	if (frame->files_live)
	{
		frame->files_live = false;
		std_vec_free_Checked(&frame->files, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	}
	if (frame->layers_live)
	{
		frame->layers_live = false;
		txn_spill_layers_drop(&frame->layers);
	}
	if (frame->slotted_live)
	{
		frame->slotted_live = false;
		std_vec_free_Checked(&frame->slotted, sizeof(txn_TupleU64KindU32OptionU32_t),
							 _Alignof(txn_TupleU64KindU32OptionU32_t));
	}
	if (frame->writing_live)
	{
		frame->writing_live = false;
		host_shared_LongGuard_drop(&frame->writing);
	}
	if (frame->path_live)
	{
		frame->path_live = false;
		std_vec_free_Checked(&frame->path.bytes, 1, 1);
	}
	if (frame->dir_live)
	{
		frame->dir_live = false;
		std_vec_free_Checked(&frame->dir.bytes, 1, 1);
	}
}

/* txn_spill_carried_push adds an element to one of carried's vectors, refusing it when the vector can't grow. */
static bool
txn_spill_carried_push(txn_spill_carried_frame_0_t *frame, StdVec *vector, const void *elem, size_t size,
					   size_t align)
{
	if (!std_vec_try_reserve(vector, 1, size, align))
	{
		*frame->error = host_Error_out_of_memory();
		return false;
	}
	std_vec_push_Checked(vector, elem, size, align);
	return true;
}

/*
 * txn_spill_carried_file writes one file of a layer: its keys and records
 * under its writer's xid, numbered through the state's numbered past the
 * writer's files of the kind listed or pended, with a row file's spilled
 * tuple ids, and lists it, counting its versions but its locks' and taking a
 * frozen writer's passing point from its records' ends.
 */
static bool
txn_spill_carried_file(txn_spill_carried_frame_0_t *frame, const TxnSpillLayerFile *layered)
{
	HostSharedRecord record = host_shared_Held_record(frame->held);
	HostIoIoDoors doors = HOST_SHARED_LONG_IO_IO_DOORS(&frame->long_io);
	size_t		at = layered->of == TXN_WRITTEN_OF_ROWS ? 0 : layered->of == TXN_WRITTEN_OF_FREE ? 1 : 2;
	TxnRunsKind kind = frame->kinds[at];
	TxnRunsNames names;
	uint32		past = 0;
	uint64		count = 0;
	uint64		passed = 0;
	bool		some;
	RepoRunFile run;
	TxnSpillFile file;
	uint32		number;
	std_TupleU64U64_t counted;

	if (layered->of != TXN_WRITTEN_OF_ROWS && layered->of != TXN_WRITTEN_OF_FREE &&
		layered->of != TXN_WRITTEN_OF_VALUES)
	{
		*frame->error = txn_spill_internal("a carry spills only a storage's rows and maps");
		return false;
	}
	if (!txn_spill_names(&TXN_SPILL_STATE_HOME_OPS, record, &frame->storage, kind, layered->xid, &names,
						 frame->error))
		return false;
	for (size_t k = 0; k < frame->files.len; k++)
	{
		const TxnSpillFile *listed = &STD_VEC_AT(&frame->files, TxnSpillFile, k);

		if (listed->xid == layered->xid && listed->kind == kind)
			past = Max(past, listed->number + 1);
	}
	for (size_t k = 0; k < frame->slotted.len; k++)
	{
		const txn_TupleU64KindU32OptionU32_t *slot = &STD_VEC_AT(&frame->slotted, txn_TupleU64KindU32OptionU32_t, k);

		if (slot->f0 == layered->xid && slot->f1 == kind)
			past = Max(past, slot->f2 + 1);
	}
	number = txn_state_numbered(record, past);
	std_vec_with_capacity_Checked(&frame->keys, layered->keys.len, sizeof(StdBytes), _Alignof(StdBytes));
	frame->keys_live = true;
	for (size_t k = 0; k < layered->keys.len; k++)
	{
		const StdVec *key = &STD_VEC_AT(&layered->keys, txn_TupleVecU8VecVecU8_t, k).f0;
		StdBytes	bytes = {key->ptr, key->len};

		std_vec_push_Checked(&frame->keys, &bytes, sizeof(bytes), _Alignof(StdBytes));
	}
	if (!txn_spill_Filter_of(frame->keys.len, frame->keys.ptr, frame->keys.len, &frame->filter, frame->error))
		return false;
	frame->filter_live = true;
	frame->writer = txn_runs_Writing_spill(frame->dir.bytes.ptr, frame->dir.bytes.len, &names, frame->tails[at],
										   number);
	frame->writer_live = true;
	std_vec_new(&frame->records, sizeof(StdBytes), _Alignof(StdBytes));
	frame->records_live = true;
	std_vec_new(&frame->tuples, sizeof(txn_TupleU64VecU8_t), _Alignof(txn_TupleU64VecU8_t));
	frame->tuples_live = true;
	for (size_t k = 0; k < layered->keys.len; k++)
	{
		const txn_TupleVecU8VecVecU8_t *keyed = &STD_VEC_AT(&layered->keys, txn_TupleVecU8VecVecU8_t, k);

		frame->records.len = 0;
		if (!std_vec_try_reserve(&frame->records, keyed->f1.len, sizeof(StdBytes), _Alignof(StdBytes)))
		{
			*frame->error = host_Error_out_of_memory();
			return false;
		}
		for (size_t r = 0; r < keyed->f1.len; r++)
		{
			const StdVec *bytes = &STD_VEC_AT(&keyed->f1, StdVec, r);
			StdBytes	lent = {bytes->ptr, bytes->len};
			TxnMemtableVersionRecord version;
			TxnMemtableVersionOpRef op;
			txn_TupleU64VecU8_t tuple;
			uint64		lsn;

			std_vec_push_Checked(&frame->records, &lent, sizeof(lent), _Alignof(StdBytes));
			if (!txn_memtable_version_stored(bytes->ptr, bytes->len, &version, frame->error))
				return false;
			if (layered->xid < TXN_STATUS_FIRST_NORMAL)
				passed = Max(passed, txn_memtable_version_Record_lsn(&version, &lsn) ? lsn : 1);
			op = txn_memtable_version_Record_op(&version);
			if (op.tag != TXN_MEMTABLE_VERSION_OP_REF_LOCK && op.tag != TXN_MEMTABLE_VERSION_OP_REF_LOCK_ROW)
				count++;
			if (kind == TXN_RUNS_KIND_SPILLED_ROWS && txn_spill_tid(bytes->ptr, bytes->len, &tuple.f0))
			{
				std_vec_clone(&tuple.f1, &keyed->f0, 1, 1);
				if (!txn_spill_carried_push(frame, &frame->tuples, &tuple, sizeof(tuple),
											_Alignof(txn_TupleU64VecU8_t)))
				{
					std_vec_free_Checked(&tuple.f1, 1, 1);
					return false;
				}
			}
		}
		if (!txn_runs_Writing_key(frame->writer, doors, keyed->f0.ptr, keyed->f0.len, frame->records.ptr,
								  frame->records.len, frame->error))
			return false;
	}
	if (!txn_runs_Writing_end(frame->writer, doors, frame->filter.words.ptr, frame->filter.words.len, &some, &run,
							  frame->error))
		return false;
	if (!some)
	{
		*frame->error = txn_spill_internal("a carry's file wrote no key");
		return false;
	}
	file = (TxnSpillFile)
	{
		.xid = layered->xid, .kind = kind, .number = number, .len = run.len, .hash = run.hash,
			.bits = txn_spill_Filter_bits(&frame->filter), .passed = passed
	};
	if (!txn_spill_carried_push(frame, &frame->files, &file, sizeof(file), _Alignof(TxnSpillFile)))
		return false;
	if (frame->tuples.len > 0)
	{
		TxnSpillFile tupled;
		StdVec		tids;

		frame->tuples_live = false;
		if (!txn_spill_tupled(&TXN_SPILL_STATE_HOME_OPS, doors, record, &frame->storage, &frame->dir, layered->xid,
							  number, &frame->tuples, &tupled, &tids, frame->error))
			return false;
		std_vec_free_Checked(&tids, sizeof(uint64), _Alignof(uint64));
		tupled.passed = passed;
		if (!txn_spill_carried_push(frame, &frame->files, &tupled, sizeof(tupled), _Alignof(TxnSpillFile)))
			return false;
	}
	counted = (std_TupleU64U64_t)
	{
		layered->xid, count
	};
	if (!txn_spill_carried_push(frame, &frame->counts, &counted, sizeof(counted), _Alignof(std_TupleU64U64_t)))
		return false;
	txn_spill_carried_file_drop(frame);
	return true;
}

/* txn_spill_carried_slots resolves each carried writer's status entry's slot before the carry's hold, as an entry with a count never moves or leaves. */
static bool
txn_spill_carried_slots(txn_spill_carried_frame_0_t *frame)
{
	std_vec_new(&frame->slots, sizeof(TxnSpillSlot), _Alignof(TxnSpillSlot));
	frame->slots_live = true;
	if (!std_vec_try_reserve_exact(&frame->slots, frame->writers_len, sizeof(TxnSpillSlot), _Alignof(TxnSpillSlot)))
	{
		*frame->error = host_Error_out_of_memory();
		return false;
	}
	for (size_t k = 0; k < frame->writers_len; k++)
	{
		TxnMemtableVersionWriter writer = {.xid = frame->writers[k]};
		TxnSpillSlot slot = {.xid = frame->writers[k]};

		if (!txn_status_kept_roomed(frame->io, &writer, &slot.has, &slot.roomed, frame->error))
			return false;
		std_vec_push_Checked(&frame->slots, &slot, sizeof(slot), _Alignof(TxnSpillSlot));
	}
	return true;
}

/* txn_spill_u64_cmp orders u64s ascending. */
static int
txn_spill_u64_cmp(const void *a, const void *b)
{
	uint64		x = *(const uint64 *) a;
	uint64		y = *(const uint64 *) b;

	return x < y ? -1 : x > y;
}

/* txn_spill_carried_ends gathers the ends of the carry's records from its chains' versions, ascending and each once. */
static bool
txn_spill_carried_ends(txn_spill_carried_frame_0_t *frame)
{
	size_t		kept = 0;

	std_vec_new(&frame->ends, sizeof(uint64), _Alignof(uint64));
	frame->ends_live = true;
	for (size_t k = 0; k < frame->chains.len; k++)
	{
		const TxnSpillChain *chain = &STD_VEC_AT(&frame->chains, TxnSpillChain, k);

		for (size_t v = 0; v < chain->versions.len; v++)
		{
			const txn_TupleU64VecU8_t *version = &STD_VEC_AT(&chain->versions, txn_TupleU64VecU8_t, v);
			TxnMemtableVersionRecord stored = txn_memtable_version_Record_of(version->f1.ptr, version->f1.len);
			uint64		lsn;

			if (!txn_memtable_version_Record_lsn(&stored, &lsn))
				continue;
			if (!std_vec_try_reserve(&frame->ends, 1, sizeof(uint64), _Alignof(uint64)))
			{
				*frame->error = host_Error_out_of_memory();
				return false;
			}
			std_vec_push_Checked(&frame->ends, &lsn, sizeof(uint64), _Alignof(uint64));
		}
	}
	if (frame->ends.len > 1)
		qsort(frame->ends.ptr, frame->ends.len, sizeof(uint64), txn_spill_u64_cmp);
	for (size_t k = 0; k < frame->ends.len; k++)
		if (kept == 0 || STD_VEC_AT(&frame->ends, uint64, kept - 1) != STD_VEC_AT(&frame->ends, uint64, k))
			STD_VEC_AT(&frame->ends, uint64, kept++) = STD_VEC_AT(&frame->ends, uint64, k);
	frame->ends.len = kept;
	return true;
}

/* txn_spill_unlist_pauses are the pauses an unlist's window runs, loaded before the carry's first count's add. */
static const char *const txn_spill_unlist_pauses[] = {"doltgres-carry-unlisting", "doltgres-spill-manifest-opened",
"doltgres-spill-manifest-read"};

/*
 * txn_spill_carried_made keeps the files the carry listed and, for a logged
 * storage, its unlist's record, both made before an unlist's window so the
 * window allocates neither.
 */
static bool
txn_spill_carried_made(txn_spill_carried_frame_0_t *frame)
{
	size_t		made = frame->files.len - frame->made_from;

	std_vec_new(&frame->made, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	frame->made_live = true;
	if (!std_vec_try_reserve_exact(&frame->made, made, sizeof(TxnSpillFile), _Alignof(TxnSpillFile)))
	{
		*frame->error = host_Error_out_of_memory();
		return false;
	}
	std_vec_extend_from_slice_Checked(&frame->made, &STD_VEC_AT(&frame->files, TxnSpillFile, frame->made_from), made,
									  sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	if (frame->ends.len > 0)
	{
		TxnRecordCarryUnlisted unlisted = {.storage = frame->storage, .ends = frame->ends};

		txn_record_CarryUnlisted_encode(&unlisted, &frame->body);
		frame->body_live = true;
	}
	return true;
}

/*
 * txn_spill_carried_counted adds an amount to a listed carried file's
 * writer's count through the slot resolved before the hold; a writer below
 * the first normal xid, or with no entry, takes none.
 */
static bool
txn_spill_carried_counted(txn_spill_carried_frame_0_t *frame, const std_TupleU64U64_t *counted, uint64 amount)
{
	const TxnSpillSlot *slot = NULL;

	if (counted->f1 == 0 || counted->f0 < TXN_STATUS_FIRST_NORMAL)
		return true;
	for (size_t k = 0; k < frame->slots.len && slot == NULL; k++)
		if (STD_VEC_AT(&frame->slots, TxnSpillSlot, k).xid == counted->f0)
			slot = &STD_VEC_AT(&frame->slots, TxnSpillSlot, k);

	/*
	 * The rewrite's AccessExclusiveLock, taken before its caller read the
	 * writers, keeps the old storage from gaining a writer the carry spills
	 * under.
	 */
	Assert(slot != NULL);
	if (slot == NULL || !slot->has)
		return true;
	return txn_status_counted(HOST_IO_IO_TAKES_SHORT(frame->io), &frame->hold, slot->roomed, amount, frame->error);
}

/* txn_spill_carried_unlisted_closure_0 is the unlist's removal's test: one of the files the carry listed. */
static bool
txn_spill_carried_unlisted_closure_0(void *captures, const TxnSpillFile *file)
{
	const StdVec *made = captures;

	for (size_t k = 0; k < made->len; k++)
		if (txn_spill_File_PartialEq_eq(&STD_VEC_AT(made, TxnSpillFile, k), file))
			return true;
	return false;
}

/*
 * txn_spill_carried_unlisted takes the carry's files back out of the
 * manifest after the add of the count of file at failed did, in one
 * checkpoint window, Postgres' critical section, so none of its steps can
 * leave part of it done: it logs the unlist and flushes its record before
 * the manifest loses a file, as RelationTruncate flushes before a truncate,
 * takes the files out, and gives back the counts of the files before failed.
 * A step's failure leaves the window open, which stops the server, as an
 * error in a critical section does; its restart replays the carry's records
 * and the unlist.
 */
static void
txn_spill_carried_unlisted(txn_spill_carried_frame_0_t *frame, size_t failed)
{
	HostSharedRecord record = host_shared_Held_record(frame->held);
	StdVec		gone;
	uint64		lsn;

	if (!host_delay_checkpoint(HOST_IO_IO_IO_DOORS(frame->io), txn_spill_unlist_pauses,
							   lengthof(txn_spill_unlist_pauses), &frame->window, frame->error))
		return;
	frame->window_live = true;
	if (frame->body_live &&
		(!host_wal_insert(HOST_IO_IO_IO_DOORS(frame->io), HOST_WAL_KIND_CARRY_UNLISTED, frame->body.ptr, frame->body.len,
						  &lsn, frame->error) ||
		 !host_wal_flush(HOST_IO_IO_IO_DOORS(frame->io), lsn, frame->error)))
		return;
	if (!host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-carry-unlisting", frame->error) ||
		!txn_spill_removed(&TXN_SPILL_STATE_HOME_OPS, frame->io, record, &frame->storage,
						   txn_spill_carried_unlisted_closure_0, &frame->made, &gone, frame->error))
		return;
	std_vec_free_Checked(&gone, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	for (size_t k = 0; k < failed; k++)
	{
		const std_TupleU64U64_t *counted = &STD_VEC_AT(&frame->counts, std_TupleU64U64_t, k);

		if (!txn_spill_carried_counted(frame, counted, std_wrapping_sub_u64(0, counted->f1)))
			return;
	}
	frame->window_live = false;
	host_DelayCheckpoint_finish(&frame->window);
}

/* txn_spill_carried_body_0 is carried's body. */
static void
txn_spill_carried_body_0(void *frame_)
{
	txn_spill_carried_frame_0_t *frame = frame_;
	HostSharedRecord record = host_shared_Held_record(frame->held);
	static const TxnWrittenOf ofs[3] = {TXN_WRITTEN_OF_ROWS, TXN_WRITTEN_OF_FREE, TXN_WRITTEN_OF_VALUES};
	TxnStatePersistence persistence;
	StdStr		parent;
	ChunksErrorError failed;

	if (!txn_state_described(record, &frame->storage, &persistence))
	{
		frame->ok = true;
		return;
	}
	for (size_t k = 0; k < lengthof(ofs); k++)
		if (!txn_spill_State_Home_kind(frame->io, frame->held, ofs[k], &frame->kinds[k], &frame->tails[k],
									   frame->error))
			return;
	txn_runs_spill_directory(frame->storage.database, txn_state_own(record), &frame->dir);
	frame->dir_live = true;
	if (!std_path_parent(frame->dir.bytes.ptr, frame->dir.bytes.len, &parent))
	{
		parent.ptr = frame->dir.bytes.ptr;
		parent.len = frame->dir.bytes.len;
	}
	if (!chunks_file_create_dir(HOST_IO_IO_IO_DOORS(frame->io), parent.ptr, parent.len, &failed) ||
		!chunks_file_create_dir(HOST_IO_IO_IO_DOORS(frame->io), frame->dir.bytes.ptr, frame->dir.bytes.len, &failed))
	{
		*frame->error = txn_base_storage(&failed);
		txn_spill_carried_pad_0(frame);
		return;
	}
	txn_spill_manifest(&TXN_SPILL_STATE_HOME_OPS, record, &frame->storage, &frame->path);
	frame->path_live = true;
	if (!txn_spill_pended(&TXN_SPILL_STATE_HOME_OPS, record, &frame->storage, &frame->slotted, frame->error))
	{
		txn_spill_carried_pad_0(frame);
		return;
	}
	frame->slotted_live = true;
	if (!txn_spill_carried_slots(frame))
	{
		txn_spill_carried_pad_0(frame);
		return;
	}
	if (!host_shared_Record_lock_long(record, HOST_IO_IO_TAKES_SHORT(frame->io), TXN_STATE_SPILLS_WRITING_INDEX,
									  &frame->writing, frame->error))
	{
		txn_spill_carried_pad_0(frame);
		return;
	}
	frame->writing_live = true;
	frame->long_io = host_shared_LongGuard_io(&frame->writing);
	std_vec_new(&frame->chains, sizeof(TxnSpillChain), _Alignof(TxnSpillChain));
	if (!frame->logged(frame->captures, &frame->long_io, &frame->chains, frame->error))
	{
		for (size_t k = 0; k < frame->chains.len; k++)
			txn_spill_Chain_drop(&STD_VEC_AT(&frame->chains, TxnSpillChain, k));
		std_vec_free_Checked(&frame->chains, sizeof(TxnSpillChain), _Alignof(TxnSpillChain));
		txn_spill_carried_pad_0(frame);
		return;
	}
	if (!txn_spill_carried_ends(frame) || !txn_spill_layered(&frame->chains, &frame->layers, frame->error))
	{
		txn_spill_carried_pad_0(frame);
		return;
	}
	frame->layers_live = true;
	if (!host_pause(HOST_SHARED_LONG_IO_PAUSES(&frame->long_io), "doltgres-carry-logged", frame->error) ||
		!txn_spill_read(HOST_IO_IO_DOORS_READ_DOORS(HOST_SHARED_LONG_IO_IO_DOORS(&frame->long_io)),
						HOST_SHARED_LONG_IO_PAUSES(&frame->long_io), &frame->path, &frame->files, frame->error))
	{
		txn_spill_carried_pad_0(frame);
		return;
	}
	frame->files_live = true;
	frame->made_from = frame->files.len;
	std_vec_new(&frame->counts, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
	frame->counts_live = true;
	for (size_t d = 0; d < frame->layers.len; d++)
	{
		const StdVec *layer = &STD_VEC_AT(&frame->layers, StdVec, d);

		for (size_t f = 0; f < layer->len; f++)
			if (!txn_spill_carried_file(frame, &STD_VEC_AT(layer, TxnSpillLayerFile, f)))
			{
				txn_spill_carried_pad_0(frame);
				return;
			}
	}
	if (!txn_spill_carried_made(frame) ||
		!host_load_pauses(HOST_IO_IO_IO_DOORS(frame->io), txn_spill_unlist_pauses, lengthof(txn_spill_unlist_pauses),
						  frame->error) ||
		!host_delta_hold(frame->io, &frame->hold, frame->error))
	{
		txn_spill_carried_pad_0(frame);
		return;
	}
	frame->hold_live = true;
	if (!host_pause(HOST_SHARED_LONG_IO_PAUSES(&frame->long_io), "doltgres-carry-written", frame->error) ||
		!txn_spill_rewrote(&TXN_SPILL_STATE_HOME_OPS, &frame->long_io, record, &frame->path, &frame->files,
						   frame->error))
	{
		txn_spill_carried_pad_0(frame);
		return;
	}
	frame->writing_live = false;
	host_shared_LongGuard_drop(&frame->writing);
	for (size_t k = 0; k < frame->counts.len; k++)
	{
		const std_TupleU64U64_t *counted = &STD_VEC_AT(&frame->counts, std_TupleU64U64_t, k);

		if (!host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-carry-counting", frame->error) ||
			!txn_spill_carried_counted(frame, counted, counted->f1))
		{
			txn_spill_carried_unlisted(frame, k);
			txn_spill_carried_pad_0(frame);
			return;
		}
	}
	frame->ok = true;
	txn_spill_carried_pad_0(frame);
}

/*
 * txn_spill_carried writes a carry's versions of other writers into a
 * rewrite's or a move's new storage as spill files under each writer's
 * top-level xid (P2), disk to disk, so the carry never waits at the delta's
 * budget on versions it can't spill: under one hold of the storage's
 * manifest's writers' lock, taken before any record, logged logs their
 * records and answers each tree's chains with every version's record, its
 * LSN the record's end; their layers' files (layered) are written and
 * synced, numbered through the state's numbered as a residual's are, a row
 * file with its file of spilled tuple ids, and listed oldest layer first,
 * newest last; after the hold each file's versions are counted at its
 * writer's entry as their pushes would be, a lock's left out. A checkpoint
 * whose redo point falls after the records names the files, since its naming
 * takes the same lock (named); one before them replays the records, which
 * redo's holds check applies once. A failed write raises its error, leaving
 * the files unlisted for the sweep.
 */
bool
txn_spill_carried(HostIoIo *io, const HostSharedHeld *held, const uint64 *writers, size_t writers_len,
				  bool (*logged) (void *captures, HostSharedLongIo *io, StdVec *chains, HostError *error),
				  void *captures, HostError *error)
{
	txn_spill_carried_frame_0_t frame = {.io = io, .held = held, .logged = logged, .captures = captures,
	.error = error, .writers = writers, .writers_len = writers_len};

	std_landing(&frame, txn_spill_carried_body_0, txn_spill_carried_pad_0);
	return frame.ok;
}

/*
 * txn_spill_unlisted_redo_captures_t is an unlist's redo's walk's captures:
 * the state, the tree walked, the carry's ends, ascending, and for the rows
 * the tuple ids of the rows taken out, or for the free space map those rows'
 * tuple ids, ascending, whose takes go too.
 */
typedef struct txn_spill_unlisted_redo_captures_t
{
	HostSharedRecord record;
	const TxnMemtableTableTreeOps *tree;
	const StdVec *ends;
	StdVec	   *tids;
	const StdVec *carried;
} txn_spill_unlisted_redo_captures_t;

/* txn_spill_unlisted_redo_frame_t is the walk's state at one key across its pad: the hold, the key's versions, and the handles taken out, each while live. */
typedef struct txn_spill_unlisted_redo_frame_t
{
	txn_spill_unlisted_redo_captures_t *walk;
	HostIoIo   *io;
	const uint8 *key;
	size_t		key_len;
	HostDeltaHold hold;
	bool		hold_live;
	StdVec		versions;
	bool		versions_live;
	StdVec		handles;
	bool		handles_live;
	StdVec		taken;
	bool		taken_live;
	HostError  *error;
	bool		ok;
} txn_spill_unlisted_redo_frame_t;

/* txn_spill_ends_hold reports whether a carry's ends, ascending, hold an LSN. */
static bool
txn_spill_ends_hold(const StdVec *ends, uint64 lsn)
{
	size_t		low = 0;
	size_t		high = ends->len;

	while (low < high)
	{
		size_t		mid = low + (high - low) / 2;
		uint64		at = STD_VEC_AT(ends, uint64, mid);

		if (at == lsn)
			return true;
		if (at < lsn)
			low = mid + 1;
		else
			high = mid;
	}
	return false;
}

/*
 * txn_spill_unlisted_redo_body takes out the key's versions whose LSN is one
 * of the carry's ends, and in the free space map the takes of the carried
 * rows too, with their counts.
 */
static void
txn_spill_unlisted_redo_body(void *frame_)
{
	txn_spill_unlisted_redo_frame_t *frame = frame_;
	txn_spill_unlisted_redo_captures_t *walk = frame->walk;

	if (!host_delta_hold(frame->io, &frame->hold, frame->error))
		return;
	frame->hold_live = true;
	if (!txn_memtable_table_chain(walk->tree, frame->io, &frame->hold, walk->record, frame->key, frame->key_len,
								  &frame->versions, frame->error))
		return;
	frame->versions_live = true;
	std_vec_new(&frame->handles, sizeof(TxnMemtableVersionHandle), _Alignof(TxnMemtableVersionHandle));
	frame->handles_live = true;
	for (size_t i = 0; i < frame->versions.len; i++)
	{
		TxnMemtableVersionVersion *version = &STD_VEC_AT(&frame->versions, TxnMemtableVersionVersion, i);
		TxnMemtableVersionRecord record = txn_memtable_version_Version_record(version);
		uint64		lsn;

		uint64		tid = txn_memtable_map_taken(txn_memtable_version_Record_op(&record)).f0;

		if (!(txn_memtable_version_Record_lsn(&record, &lsn) && txn_spill_ends_hold(walk->ends, lsn)) &&
			!(walk->carried != NULL && tid != 0 && txn_spill_ends_hold(walk->carried, tid)))
			continue;
		if (walk->tids != NULL && txn_spill_op_tid(txn_memtable_version_Record_op(&record), &tid))
		{
			if (!std_vec_try_reserve(walk->tids, 1, sizeof(uint64), _Alignof(uint64)))
			{
				*frame->error = host_Error_out_of_memory();
				return;
			}
			std_vec_push_Checked(walk->tids, &tid, sizeof(uint64), _Alignof(uint64));
		}
		if (!std_vec_try_reserve(&frame->handles, 1, sizeof(TxnMemtableVersionHandle),
								 _Alignof(TxnMemtableVersionHandle)))
		{
			*frame->error = host_Error_out_of_memory();
			return;
		}
		std_vec_push_Checked(&frame->handles, &version->handle, sizeof(TxnMemtableVersionHandle),
							 _Alignof(TxnMemtableVersionHandle));
	}
	if (frame->handles.len > 0)
	{
		if (!txn_memtable_table_removed(walk->tree, frame->io, walk->record, frame->key, frame->key_len,
										frame->handles.ptr, frame->handles.len, &frame->taken, frame->error))
			return;
		frame->taken_live = true;
	}
	frame->ok = true;
}

/* txn_spill_unlisted_redo_pad frees what the walk holds live at its key. */
static void
txn_spill_unlisted_redo_pad(void *frame_)
{
	txn_spill_unlisted_redo_frame_t *frame = frame_;

	if (frame->taken_live)
		std_vec_free_Checked(&frame->taken, sizeof(bool), _Alignof(bool));
	if (frame->handles_live)
		std_vec_free_Checked(&frame->handles, sizeof(TxnMemtableVersionHandle), _Alignof(TxnMemtableVersionHandle));
	if (frame->versions_live)
		txn_memtable_version_versions_drop(&frame->versions, 0);
	if (frame->hold_live)
		host_delta_Hold_drop(&frame->hold);
	frame->taken_live = frame->handles_live = frame->versions_live = frame->hold_live = false;
}

/* txn_spill_unlisted_redo_closure_0 is the unlist's redo's walk's closure, of one key of a tree. */
static bool
txn_spill_unlisted_redo_closure_0(void *captures, HostIoIo *io, const uint8 *key, size_t key_len,
								  const StdVec *values, size_t values_len, HostError *error)
{
	txn_spill_unlisted_redo_frame_t frame = {.walk = captures, .io = io, .key = key, .key_len = key_len,
	.error = error};

	std_landing(&frame, txn_spill_unlisted_redo_body, txn_spill_unlisted_redo_pad);
	txn_spill_unlisted_redo_pad(&frame);
	return frame.ok;
}

/*
 * txn_spill_unlisted_redone takes out of one of a storage's memtables each
 * version whose LSN is one of an unlisted carry's ends, adding the rows'
 * tuple ids to tids when given, and with the carried rows' tuple ids given,
 * the free space takes of those rows too, which carry no LSN, each with its
 * count, as its primary took the carry's files back out (bugs row 291's
 * review R2, G1, and G2).
 */
static bool
txn_spill_unlisted_redone(HostIoIo *io, const HostSharedHeld *held, const TxnMemtableTableTreeOps *tree,
						  const TxnRecordCarryUnlisted *unlisted, StdVec *tids, const StdVec *carried,
						  HostError *error)
{
	txn_spill_unlisted_redo_captures_t walk = {host_shared_Held_record(held), tree, &unlisted->ends, tids, carried};

	return txn_memtable_table_walked_killed(tree, io, walk.record, txn_spill_unlisted_redo_closure_0, &walk, NULL,
											error);
}

/*
 * txn_spill_unlisted_run_frame_t is an unlist's redo's state at one writer's
 * run of a kind across its pad: the files listed, the run's files and its
 * rows' tuple-id files, and what the joins leave out, each while live.
 */
typedef struct txn_spill_unlisted_run_frame_t
{
	HostIoIo   *io;
	HostSharedRecord record;
	const HostStorage *storage;
	const StdPathBuf *dir;
	const StdVec *ends;
	const StdVec *carried;
	uint64		xid;
	TxnRunsKind kind;
	TxnRunsTail tail;
	HostError  *error;
	bool		ok;
	txn_TupleVecFileU64_t listed;
	bool		listed_live;
	StdVec		own;
	bool		own_live;
	StdVec		named;
	bool		named_live;
	TxnSpillLeft left;
	bool		left_live;
	StdVec		gone;
	bool		gone_live;
} txn_spill_unlisted_run_frame_t;

/* txn_spill_unlisted_run_pad frees what the run holds live. */
static void
txn_spill_unlisted_run_pad(void *frame_)
{
	txn_spill_unlisted_run_frame_t *frame = frame_;

	if (frame->gone_live)
		std_vec_free_Checked(&frame->gone, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	if (frame->left_live)
		std_vec_free_Checked(&frame->left.tids, sizeof(uint64), _Alignof(uint64));
	if (frame->named_live)
		std_vec_free_Checked(&frame->named, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	if (frame->own_live)
		std_vec_free_Checked(&frame->own, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	if (frame->listed_live)
		std_vec_free_Checked(&frame->listed.f0, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	frame->gone_live = frame->left_live = frame->named_live = frame->own_live = frame->listed_live = false;
}

/* txn_spill_unlisted_run_closure_0 is the run's removal's test: one of the files the run's listing replaces. */
static bool
txn_spill_unlisted_run_closure_0(void *captures, const TxnSpillFile *file)
{
	return txn_spill_replaced_by(captures, 1, file);
}

/* txn_spill_tid_cmp orders tuple ids ascending. */
static int
txn_spill_tid_cmp(const void *a, const void *b)
{
	uint64		x = *(const uint64 *) a;
	uint64		y = *(const uint64 *) b;

	return x < y ? -1 : x > y;
}

/*
 * txn_spill_unlisted_run_body joins the writer's whole run of the kind, and
 * for rows its tuple-id files of the same numbers, without the carry's
 * versions and their rows' tuple ids; when it left out any, it lists the
 * join in place of the run, as a merge lists its file, or takes the run out
 * whole when nothing is left, and gives the dropped versions' count back at
 * the writer's entry.
 */
static void
txn_spill_unlisted_run_body(void *frame_)
{
	txn_spill_unlisted_run_frame_t *frame = frame_;
	TxnRunsNames names;
	TxnRunsNames tuple_names;
	TxnRunsTail none = {.tag = TXN_RUNS_TAIL_NONE};
	TxnSpillFile listing[2] = {0};
	size_t		listing_len = 1;
	uint32		number = 0;
	uint32		lowest = PG_UINT32_MAX;
	txn_TupleU64KindU32OptionU32_t replacing;
	HostXid		xid = {frame->xid};
	bool		has;
	uint64		added;

	if (!txn_spill_listed(&TXN_SPILL_STATE_HOME_OPS, frame->io, frame->record, frame->storage, &frame->listed,
						  frame->error))
		return;
	frame->listed_live = true;
	std_vec_new(&frame->own, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	frame->own_live = true;
	std_vec_new(&frame->named, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	frame->named_live = true;
	for (size_t k = 0; k < frame->listed.f0.len; k++)
	{
		const TxnSpillFile *file = &STD_VEC_AT(&frame->listed.f0, TxnSpillFile, k);
		StdVec	   *into = file->kind == frame->kind ? &frame->own :
			file->kind == TXN_RUNS_KIND_SPILLED_TUPLES && frame->kind == TXN_RUNS_KIND_SPILLED_ROWS ? &frame->named :
			NULL;

		if (file->xid != frame->xid || into == NULL)
			continue;
		if (!std_vec_try_reserve(into, 1, sizeof(TxnSpillFile), _Alignof(TxnSpillFile)))
		{
			*frame->error = host_Error_out_of_memory();
			return;
		}
		std_vec_push_Checked(into, file, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	}
	if (frame->own.len == 0)
	{
		frame->ok = true;
		return;
	}
	for (size_t k = 0; k < frame->own.len; k++)
	{
		number = Max(number, STD_ADD(u32, STD_VEC_AT(&frame->own, TxnSpillFile, k).number, 1, 0, 0));
		lowest = Min(lowest, STD_VEC_AT(&frame->own, TxnSpillFile, k).number);
	}
	frame->left = (TxnSpillLeft)
	{
		.ends = frame->ends, .carried = frame->carried
	};
	std_vec_new(&frame->left.tids, sizeof(uint64), _Alignof(uint64));
	frame->left_live = true;
	if (!txn_spill_names(&TXN_SPILL_STATE_HOME_OPS, frame->record, frame->storage, frame->kind, frame->xid, &names,
						 frame->error) ||
		!txn_spill_joined(frame->io, frame->dir, &names, frame->tail, frame->own.ptr, frame->own.len, number,
						  &frame->left, &listing[0], frame->error))
		return;
	if (frame->left.dropped == 0)
	{
		frame->ok = true;
		return;
	}
	if (frame->left.tids.len > 1)
		qsort(frame->left.tids.ptr, frame->left.tids.len, sizeof(uint64), txn_spill_tid_cmp);
	if (frame->named.len > 0)
	{
		if (!txn_spill_names(&TXN_SPILL_STATE_HOME_OPS, frame->record, frame->storage, TXN_RUNS_KIND_SPILLED_TUPLES,
							 frame->xid, &tuple_names, frame->error) ||
			!txn_spill_joined(frame->io, frame->dir, &tuple_names, none, frame->named.ptr, frame->named.len, number,
							  &frame->left, &listing[1], frame->error))
			return;
		listing_len = listing[1].len > 0 ? 2 : 1;
	}
	replacing.f0 = frame->xid;
	replacing.f1 = frame->kind;
	replacing.f2 = number;
	replacing.has_f3 = true;
	replacing.f3 = lowest;
	if (listing[0].len == 0)
	{
		if (!txn_spill_removed(&TXN_SPILL_STATE_HOME_OPS, frame->io, frame->record, frame->storage,
							   txn_spill_unlisted_run_closure_0, &replacing, &frame->gone, frame->error))
			return;
		frame->gone_live = true;
	}
	else if (!txn_spill_pend(&TXN_SPILL_STATE_HOME_OPS, frame->io, frame->record, frame->storage, &listing[0],
							 listing_len > 1, true, lowest, frame->error) ||
			 !txn_spill_linked(&TXN_SPILL_STATE_HOME_OPS, frame->io, frame->record, frame->storage, listing,
							   listing_len, true, replacing, frame->error) ||
			 !txn_spill_settled(&TXN_SPILL_STATE_HOME_OPS, frame->io, &frame->record, frame->error))
		return;
	if (frame->xid >= TXN_STATUS_FIRST_NORMAL &&
		!txn_status_add(frame->io, xid, std_wrapping_sub_u64(0, frame->left.dropped), &has, &added, frame->error))
		return;
	frame->ok = true;
}

/* txn_spill_unlisted_run does one writer's run of a kind of an unlist's redo, as its body does. */
static bool
txn_spill_unlisted_run(HostIoIo *io, HostSharedRecord record, const HostStorage *storage, const StdPathBuf *dir,
					   const StdVec *ends, const StdVec *carried, StdVec *tids, uint64 xid, TxnRunsKind kind,
					   TxnRunsTail tail, HostError *error)
{
	txn_spill_unlisted_run_frame_t frame = {.io = io, .record = record, .storage = storage, .dir = dir, .ends = ends,
	.carried = carried, .xid = xid, .kind = kind, .tail = tail, .error = error};

	std_landing(&frame, txn_spill_unlisted_run_body, txn_spill_unlisted_run_pad);
	if (frame.ok && frame.left_live && tids != NULL)
		std_vec_extend_from_slice_Checked(tids, frame.left.tids.ptr, frame.left.tids.len, sizeof(uint64),
										  _Alignof(uint64));
	txn_spill_unlisted_run_pad(&frame);
	return frame.ok;
}

/*
 * txn_spill_unlisted_of redoes an unlist's spill files of one tree: each
 * writer's run of its kind joined without the carry's versions, or for the
 * free space map without the carried rows' takes, adding a rows run's
 * dropped tuple ids to tids when given.
 */
static bool
txn_spill_unlisted_of(HostIoIo *io, const HostSharedHeld *held, const HostStorage *storage, const StdPathBuf *dir,
					  const TxnRecordCarryUnlisted *unlisted, TxnWrittenOf of, const StdVec *carried, StdVec *tids,
					  HostError *error)
{
	HostSharedRecord record = host_shared_Held_record(held);
	TxnRunsKind kind;
	TxnRunsTail tail;
	txn_TupleVecFileU64_t listed;
	StdVec		xids;
	bool		ok;

	if (!txn_spill_State_Home_kind(io, held, of, &kind, &tail, error) ||
		!txn_spill_listed(&TXN_SPILL_STATE_HOME_OPS, io, record, storage, &listed, error))
		return false;
	std_vec_new(&xids, sizeof(uint64), _Alignof(uint64));
	for (size_t k = 0; k < listed.f0.len; k++)
	{
		const TxnSpillFile *file = &STD_VEC_AT(&listed.f0, TxnSpillFile, k);

		if (file->kind == kind && !txn_spill_has_xid(&xids, file->xid))
			std_vec_push_Checked(&xids, &file->xid, sizeof(uint64), _Alignof(uint64));
	}
	std_vec_free_Checked(&listed.f0, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	ok = true;
	for (size_t k = 0; k < xids.len && ok; k++)
		ok = txn_spill_unlisted_run(io, record, storage, dir, &unlisted->ends, carried, tids,
									STD_VEC_AT(&xids, uint64, k), kind, tail, error);
	std_vec_free_Checked(&xids, sizeof(uint64), _Alignof(uint64));
	return ok;
}

/*
 * txn_spill_redo_unlisted replays a carry's unlist (bugs row 291's review
 * R2): it takes out of the storage's delta each version whose LSN is one of
 * the record's ends, with its count, from the memtables and, for each
 * writer's run of spill files of the rows, the free space map, or the value
 * map holding one, by a join of the run without them listed in its place,
 * and the free space takes of the rows it takes out, which carry no LSN, so
 * a standby or a restart keeps none of a carry its primary took back.
 */
bool
txn_spill_redo_unlisted(HostIoIo *io, const HostSharedHeld *held, const TxnRecordCarryUnlisted *unlisted,
						HostError *error)
{
	HostSharedRecord record = host_shared_Held_record(held);
	HostStorage storage;
	TxnStatePersistence persistence;
	StdPathBuf	dir;
	bool		described = txn_state_described(record, &storage, &persistence);
	bool		ok;
	StdVec		tids;
	size_t		kept = 0;

	std_vec_new(&tids, sizeof(uint64), _Alignof(uint64));
	if (described)
		txn_runs_spill_directory(storage.database, txn_state_own(record), &dir);
	ok = txn_spill_unlisted_redone(io, held, &TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, unlisted, &tids, NULL, error) &&
		(!described || txn_spill_unlisted_of(io, held, &storage, &dir, unlisted, TXN_WRITTEN_OF_ROWS, NULL, &tids,
											 error));
	if (ok && tids.len > 1)
		qsort(tids.ptr, tids.len, sizeof(uint64), txn_spill_tid_cmp);
	for (size_t k = 0; ok && k < tids.len; k++)
		if (kept == 0 || STD_VEC_AT(&tids, uint64, kept - 1) != STD_VEC_AT(&tids, uint64, k))
			STD_VEC_AT(&tids, uint64, kept++) = STD_VEC_AT(&tids, uint64, k);
	tids.len = kept;
	ok = ok && txn_spill_unlisted_redone(io, held, &TXN_MEMTABLE_MAP_FREE_TREE_OPS, unlisted, NULL, &tids, error) &&
		txn_spill_unlisted_redone(io, held, &TXN_MEMTABLE_MAP_VALUES_TREE_OPS, unlisted, NULL, NULL, error) &&
		(!described ||
		 (txn_spill_unlisted_of(io, held, &storage, &dir, unlisted, TXN_WRITTEN_OF_FREE, &tids, NULL, error) &&
		  txn_spill_unlisted_of(io, held, &storage, &dir, unlisted, TXN_WRITTEN_OF_VALUES, NULL, NULL, error)));
	std_vec_free_Checked(&tids, sizeof(uint64), _Alignof(uint64));
	if (described)
		std_vec_free_Checked(&dir.bytes, 1, 1);
	return ok;
}

/* txn_spill_unfolded_frame_0_t is unfolded_since's state across its pad: the newest base, the hold, and the next key found, while live. */
typedef struct txn_spill_unfolded_frame_0_t
{
	HostIoIo   *io;
	const HostSharedHeld *held;
	HostLsn		end;
	bool	   *result;
	HostError  *error;
	bool		ok;
	TxnBaseBase base;
	bool		base_live;
	HostDeltaHold hold;
	bool		hold_live;
	txn_TupleVecU8VecVersion_t next;
	bool		next_live;
	txn_TupleVecFileU64_t listed;
	bool		listed_live;
} txn_spill_unfolded_frame_0_t;

/* txn_spill_unfolded_pad_0 is unfolded_since's pad: what it holds, while live. */
static void
txn_spill_unfolded_pad_0(void *frame_)
{
	txn_spill_unfolded_frame_0_t *frame = frame_;

	if (frame->listed_live)
	{
		frame->listed_live = false;
		std_vec_free_Checked(&frame->listed.f0, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	}
	if (frame->next_live)
	{
		frame->next_live = false;
		std_vec_free_Checked(&frame->next.f0, 1, 1);
		txn_memtable_version_versions_drop(&frame->next.f1, 0);
	}
	if (frame->hold_live)
	{
		frame->hold_live = false;
		host_delta_Hold_drop(&frame->hold);
	}
	if (frame->base_live)
	{
		frame->base_live = false;
		txn_base_Base_drop(&frame->base);
	}
}

/* txn_spill_unfolded_body_0 is unfolded_since's body. */
static void
txn_spill_unfolded_body_0(void *frame_)
{
	txn_spill_unfolded_frame_0_t *frame = frame_;
	HostSharedRecord record = host_shared_Held_record(frame->held);
	HostSharedOp load = {.tag = HOST_SHARED_OP_LOAD};
	bool		some;
	uint64		word;

	*frame->result = false;
	if (!txn_state_newest(frame->io, frame->held, &some, &frame->base, frame->error))
		return;
	frame->base_live = some;
	if (some && frame->base.read >= frame->end)
	{
		frame->ok = true;
		txn_spill_unfolded_pad_0(frame);
		return;
	}
	if (host_shared_Record_atomic(record, TXN_STATE_SPILLS_PENDING_INDEX, load) != 0 ||
		txn_memtable_table_memtable(&TXN_MEMTABLE_MAP_VALUES_TREE_OPS, record, &word))
	{
		*frame->result = true;
		frame->ok = true;
		txn_spill_unfolded_pad_0(frame);
		return;
	}
	if (host_shared_Record_atomic(record, TXN_STATE_SPILLS_INDEX, load) != 0)
	{
		HostStorage storage;
		TxnStatePersistence persistence;

		if (txn_state_described(record, &storage, &persistence))
		{
			if (!txn_spill_listed(&TXN_SPILL_STATE_HOME_OPS, frame->io, record, &storage, &frame->listed,
								  frame->error))
			{
				txn_spill_unfolded_pad_0(frame);
				return;
			}
			frame->listed_live = true;
			for (size_t k = 0; k < frame->listed.f0.len && !*frame->result; k++)
				*frame->result = STD_VEC_AT(&frame->listed.f0, TxnSpillFile, k).kind != TXN_RUNS_KIND_SPILLED_FREE;
		}
		if (*frame->result)
		{
			frame->ok = true;
			txn_spill_unfolded_pad_0(frame);
			return;
		}
	}
	if (!host_delta_hold(frame->io, &frame->hold, frame->error))
	{
		txn_spill_unfolded_pad_0(frame);
		return;
	}
	frame->hold_live = true;
	if (!txn_memtable_table_next(frame->io, &frame->hold, record, false, NULL, 0, TXN_MEMTABLE_TABLE_DIRECTION_FORWARD,
								 &some, &frame->next, frame->error))
	{
		txn_spill_unfolded_pad_0(frame);
		return;
	}
	frame->next_live = some;
	*frame->result = some;
	frame->ok = true;
	txn_spill_unfolded_pad_0(frame);
}

/*
 * txn_spill_unfolded_since reports whether a storage state's newest base is
 * read below a commit's end while the state holds a row, entry, or value map
 * version that no fold applied, in its memtables or its spills, which may
 * name a chunk of that commit's transaction; its free space map's versions
 * name none.
 */
static bool
txn_spill_unfolded_since(HostIoIo *io, const HostSharedHeld *held, HostLsn end, bool *result, HostError *error)
{
	txn_spill_unfolded_frame_0_t frame = {.io = io, .held = held, .end = end, .result = result, .error = error};

	std_landing(&frame, txn_spill_unfolded_body_0, txn_spill_unfolded_pad_0);
	return frame.ok;
}

/* txn_spill_chunks_ended_frame_0_t is chunks_ended's state across its pad: the files listed, the xids seen and ended, the database's states, and the files removed, while live. */
typedef struct txn_spill_chunks_ended_frame_0_t
{
	HostIoIo   *io;
	HostSharedRecord deltas;
	HostError  *error;
	bool		ok;
	HostStorage storage;
	txn_TupleVecFileU64_t listed;
	bool		listed_live;
	/* seen and ended are Vec<u64>s of xids. */
	StdVec		seen;
	bool		seen_live;
	StdVec		ended;
	bool		ended_live;
	/* states is a Vec<(u64, Held<State>)>. */
	StdVec		states;
	bool		states_live;
	StdVec		gone;
	bool		gone_live;
} txn_spill_chunks_ended_frame_0_t;

/* txn_spill_states_drop drops a Vec<(u64, Held<State>)>. */
static void
txn_spill_states_drop(StdVec *states)
{
	for (size_t k = 0; k < states->len; k++)
		host_shared_Held_drop(&STD_VEC_AT(states, txn_TupleU64HeldState_t, k).f1);
	std_vec_free_Checked(states, sizeof(txn_TupleU64HeldState_t), _Alignof(txn_TupleU64HeldState_t));
}

/* txn_spill_chunks_ended_pad_0 is chunks_ended's pad: what it holds, while live. */
static void
txn_spill_chunks_ended_pad_0(void *frame_)
{
	txn_spill_chunks_ended_frame_0_t *frame = frame_;

	if (frame->gone_live)
	{
		frame->gone_live = false;
		std_vec_free_Checked(&frame->gone, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	}
	if (frame->states_live)
	{
		frame->states_live = false;
		txn_spill_states_drop(&frame->states);
	}
	if (frame->ended_live)
	{
		frame->ended_live = false;
		std_vec_free_Checked(&frame->ended, sizeof(uint64), _Alignof(uint64));
	}
	if (frame->seen_live)
	{
		frame->seen_live = false;
		std_vec_free_Checked(&frame->seen, sizeof(uint64), _Alignof(uint64));
	}
	if (frame->listed_live)
	{
		frame->listed_live = false;
		std_vec_free_Checked(&frame->listed.f0, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	}
}

/* txn_spill_chunks_ended_gone reports whether a committed transaction's chunk spills may go: its status entry gone, or no state of the repository unfolded past its commit. */
static bool
txn_spill_chunks_ended_gone(txn_spill_chunks_ended_frame_0_t *frame, HostXid xid, bool *gone)
{
	TxnStatusCommit commit;

	if (!txn_status_committed(frame->io, xid, &commit, frame->error))
		return false;
	*gone = commit.tag == TXN_STATUS_COMMIT_MISSING;
	if (commit.tag != TXN_STATUS_COMMIT_AT)
		return true;
	if (!txn_state_states(frame->io, frame->storage.database, &frame->states, frame->error))
		return false;
	frame->states_live = true;
	*gone = true;
	for (size_t k = 0; k < frame->states.len && *gone; k++)
	{
		bool		unfolded;

		if (!txn_spill_unfolded_since(frame->io, &STD_VEC_AT(&frame->states, txn_TupleU64HeldState_t, k).f1,
									  commit.u.at, &unfolded, frame->error))
			return false;
		*gone = !unfolded;
	}
	frame->states_live = false;
	txn_spill_states_drop(&frame->states);
	return true;
}

/* txn_spill_chunks_ended_body_0 is chunks_ended's body. */
static void
txn_spill_chunks_ended_body_0(void *frame_)
{
	txn_spill_chunks_ended_frame_0_t *frame = frame_;
	TxnStatePersistence persistence;

	if (!txn_spill_BranchDeltas_Home_described(frame->deltas, &frame->storage, &persistence))
	{
		frame->ok = true;
		return;
	}
	if (!txn_spill_listed(&TXN_SPILL_BRANCH_DELTAS_HOME_OPS, frame->io, frame->deltas, &frame->storage,
						  &frame->listed, frame->error))
		return;
	frame->listed_live = true;
	std_vec_new(&frame->seen, sizeof(uint64), _Alignof(uint64));
	frame->seen_live = true;
	std_vec_new(&frame->ended, sizeof(uint64), _Alignof(uint64));
	frame->ended_live = true;
	for (size_t k = 0; k < frame->listed.f0.len; k++)
	{
		HostXid		xid = {STD_VEC_AT(&frame->listed.f0, TxnSpillFile, k).xid};
		HostStatus	status;
		bool		gone = false;

		if (txn_spill_has_xid(&frame->seen, xid.f0))
			continue;
		if (!std_vec_try_reserve(&frame->seen, 1, sizeof(uint64), _Alignof(uint64)))
		{
			*frame->error = host_Error_out_of_memory();
			txn_spill_chunks_ended_pad_0(frame);
			return;
		}
		std_vec_push_Checked(&frame->seen, &xid.f0, sizeof(uint64), _Alignof(uint64));
		if (!host_status(HOST_IO_IO_IO_DOORS(frame->io), xid, &status, frame->error) ||
			(status == HOST_STATUS_COMMITTED && !txn_spill_chunks_ended_gone(frame, xid, &gone)))
		{
			txn_spill_chunks_ended_pad_0(frame);
			return;
		}
		if (status != HOST_STATUS_COMMITTED || !gone)
			continue;
		if (!std_vec_try_reserve(&frame->ended, 1, sizeof(uint64), _Alignof(uint64)))
		{
			*frame->error = host_Error_out_of_memory();
			txn_spill_chunks_ended_pad_0(frame);
			return;
		}
		std_vec_push_Checked(&frame->ended, &xid.f0, sizeof(uint64), _Alignof(uint64));
	}
	if (frame->ended.len > 0)
	{
		if (!txn_spill_removed(&TXN_SPILL_BRANCH_DELTAS_HOME_OPS, frame->io, frame->deltas, &frame->storage,
							   txn_spill_xids_closure_0, &frame->ended, &frame->gone, frame->error))
		{
			txn_spill_chunks_ended_pad_0(frame);
			return;
		}
		frame->gone_live = true;
	}
	frame->ok = true;
	txn_spill_chunks_ended_pad_0(frame);
}

/*
 * txn_spill_chunks_ended takes out of a working state's chunk spills'
 * manifest the files of every committed transaction whose status entry is
 * gone, or past whose commit no storage state of the repository holds an
 * unfolded version that may name one of its chunks; a later checkpoint
 * unlinks the files it took out.
 */
bool
txn_spill_chunks_ended(HostIoIo *io, HostSharedRecord deltas, HostError *error)
{
	txn_spill_chunks_ended_frame_0_t frame = {.io = io, .deltas = deltas, .error = error};

	std_landing(&frame, txn_spill_chunks_ended_body_0, txn_spill_chunks_ended_pad_0);
	return frame.ok;
}

/* TxnSpillRead is Read: a backend's read of a spill file, its path, length, and names, its index frame's place and entries, and the chunk it read last, by its entry. */
typedef struct TxnSpillRead
{
	StdPathBuf	path;
	uint64		len;
	TxnRunsNames names;
	uint64		index_at;
	/* entries is an Index, a Vec<(Vec<u8>, u64)>. */
	StdVec		entries;
	bool		has_chunk;
	size_t		chunk_at;
	/* chunk is a Chunk, a Vec<(Vec<u8>, Vec<Vec<u8>>)>. */
	StdVec		chunk;
} TxnSpillRead;

/* txn_spill_Read_drop is Read's drop glue: its path, entries, and chunk. */
static void
txn_spill_Read_drop(void *elem)
{
	TxnSpillRead *self = elem;

	if (self->has_chunk)
		txn_spill_spilled_drop(&self->chunk, 0);
	txn_runs_Index_drop(&self->entries);
	std_vec_free_Checked(&self->path.bytes, 1, 1);
}

/* TXN_SPILL_KEPT is KEPT, the most spill files a backend keeps its reads of. */
#define TXN_SPILL_KEPT 8

/* txn_spill_VecRead_drop is Vec<Read>'s drop glue. */
static void
txn_spill_VecRead_drop(void *value)
{
	std_vec_drop_Checked(value, sizeof(TxnSpillRead), _Alignof(TxnSpillRead), txn_spill_Read_drop);
}

/* taken at Vec<Read> and IoMutex<Vec<Read>>, which spill.c alone uses. */
#define HOST_LOCK_SCOPE static
#define HOST_LOCK_DECLARE
#define HOST_LOCK_DEFINE
#define HOST_LOCK_TAKEN_TS VecRead
#define HOST_LOCK_TAKEN_T StdVec
#define HOST_LOCK_TAKEN_DEFAULT(place) std_vec_new((place), sizeof(TxnSpillRead), _Alignof(TxnSpillRead))
#define HOST_LOCK_TAKEN_DROP(place) txn_spill_VecRead_drop(place)
#define HOST_LOCK_TAKEN_TYPE TXN_SPILL_READS_LOCK_TYPE
#define HOST_LOCK_IO_MUTEX
#define HOST_LOCK_TS VecRead
#define HOST_LOCK_T StdVec
#define HOST_LOCK_DEFAULT(place) std_vec_new((place), sizeof(TxnSpillRead), _Alignof(TxnSpillRead))
#include "doltgres/host/lock_template.h"

/* txn_spill_IoMutexVecRead_drop is IoMutex<Vec<Read>>' drop glue: its value's. */
static void
txn_spill_IoMutexVecRead_drop(void *value)
{
	HostLockIoMutexVecRead *self = value;

	txn_spill_VecRead_drop(&self->value);
}

/* TXN_SPILL_READS_TYPE is IoMutex<Vec<Read>>' size, Default, and drop. */
static const StdDefaultType TXN_SPILL_READS_TYPE = {sizeof(HostLockIoMutexVecRead), _Alignof(HostLockIoMutexVecRead),
host_lock_IoMutexVecRead_Default_default, txn_spill_IoMutexVecRead_drop};

/*
 * TXN_SPILL_READS is READS: the backend's reads of the spill files it read
 * last, the latest first, so a scan's steps read each chunk once; its
 * BackendLocal header and the process's value, which follows it.
 */
static struct
{
	HostLocalBackendLocal local;
	HostLockIoMutexVecRead process;
}			TXN_SPILL_READS = {HOST_LOCAL_BACKEND_LOCAL_NEW};

/* txn_spill_chunk_frame_0_t is Read's chunk's state across its pad: the chunk it replaces, the file, and the chunk read, while live. */
typedef struct txn_spill_chunk_frame_0_t
{
	TxnSpillRead *self;
	HostIoIo   *io;
	size_t		entry;
	HostError  *error;
	bool		ok;
	StdVec		old;
	bool		old_live;
	HostFile	file;
	bool		file_live;
	StdVec		read;
	bool		read_live;
} txn_spill_chunk_frame_0_t;

/* txn_spill_chunk_pad_0 is Read's chunk's pad: what it holds, while live. */
static void
txn_spill_chunk_pad_0(void *frame_)
{
	txn_spill_chunk_frame_0_t *frame = frame_;

	if (frame->read_live)
	{
		frame->read_live = false;
		txn_spill_spilled_drop(&frame->read, 0);
	}
	if (frame->file_live)
	{
		frame->file_live = false;
		host_File_drop(&frame->file);
	}
	if (frame->old_live)
	{
		frame->old_live = false;
		txn_spill_spilled_drop(&frame->old, 0);
	}
}

/* txn_spill_chunk_body_0 is Read's chunk's body. */
static void
txn_spill_chunk_body_0(void *frame_)
{
	txn_spill_chunk_frame_0_t *frame = frame_;
	TxnSpillRead *self = frame->self;
	ChunksErrorError failed;
	uint64		start;
	uint64		end = self->index_at;
	size_t		next;

	if (self->has_chunk && self->chunk_at == frame->entry)
	{
		frame->ok = true;
		return;
	}
	frame->old = self->chunk;
	frame->old_live = self->has_chunk;
	self->has_chunk = false;
	if (!chunks_file_open_rw(HOST_IO_IO_IO_DOORS(frame->io), self->path.bytes.ptr, self->path.bytes.len, false,
							 &frame->file, &failed))
	{
		*frame->error = txn_base_storage(&failed);
		txn_spill_chunk_pad_0(frame);
		return;
	}
	frame->file_live = true;
	std_index_check(frame->entry, self->entries.len, RUST_FILE, 1679, 29);
	start = STD_VEC_AT(&self->entries, txn_TupleVecU8U64_t, frame->entry).f1;
	next = STD_ADD(usize, frame->entry, 1, 1680, 44);
	if (next < self->entries.len)
		end = STD_VEC_AT(&self->entries, txn_TupleVecU8U64_t, next).f1;
	if (!txn_runs_spill_chunk(frame->io, &frame->file, &self->names, start, end, &frame->read, frame->error))
	{
		txn_spill_chunk_pad_0(frame);
		return;
	}
	frame->read_live = true;
	frame->file_live = false;
	host_File_drop(&frame->file);
	if (frame->old_live)
	{
		frame->old_live = false;
		txn_spill_spilled_drop(&frame->old, 0);
	}
	self->chunk = frame->read;
	self->chunk_at = frame->entry;
	self->has_chunk = true;
	frame->read_live = false;
	frame->ok = true;
}

/* txn_spill_Read_chunk reads the keys of the chunk an index entry starts into the read, once. */
static bool
txn_spill_Read_chunk(TxnSpillRead *self, HostIoIo *io, size_t entry, HostError *error)
{
	txn_spill_chunk_frame_0_t frame = {.self = self, .io = io, .entry = entry, .error = error};

	std_landing(&frame, txn_spill_chunk_body_0, txn_spill_chunk_pad_0);
	return frame.ok;
}

/* txn_spill_Read_entries_before returns how many of the read's index entries start at or below a key, or below it when strictly. */
static size_t
txn_spill_Read_entries_before(const TxnSpillRead *self, const uint8 *key, size_t key_len, bool strictly)
{
	size_t		low = 0;
	size_t		high = self->entries.len;

	while (low < high)
	{
		size_t		middle = low + (high - low) / 2;
		const StdVec *first = &STD_VEC_AT(&self->entries, txn_TupleVecU8U64_t, middle).f0;
		StdOrdering order = std_bytes_Ord_cmp(first->ptr, first->len, key, key_len);

		if (order == STD_ORDERING_LESS || (!strictly && order == STD_ORDERING_EQUAL))
			low = middle + 1;
		else
			high = middle;
	}
	return low;
}

/* txn_spill_vec_u8_clone is Vec<u8>'s Clone, into a place. */
static void
txn_spill_vec_u8_clone(const void *elem, void *result)
{
	const StdVec *record = elem;

	std_slice_to_vec_Checked(result, record->ptr, record->len, 1, 1);
}

/* txn_spill_versions_clone clones a Vec<Vec<u8>> of records. */
static void
txn_spill_versions_clone(const StdVec *records, StdVec *result)
{
	std_slice_to_vec_clone(result, records->ptr, records->len, sizeof(StdVec), _Alignof(StdVec),
						   txn_spill_vec_u8_clone, txn_spill_vec_u8_drop);
}

/* txn_spill_clone_frame_0_t is Spilled's clone's state across its pad: the key cloned, while live. */
typedef struct txn_spill_clone_frame_0_t
{
	const txn_TupleVecU8VecVecU8_t *self;
	txn_TupleVecU8VecVecU8_t *result;
	bool		key_live;
} txn_spill_clone_frame_0_t;

/* txn_spill_clone_pad_0 is Spilled's clone's pad: the key cloned, while live. */
static void
txn_spill_clone_pad_0(void *frame_)
{
	txn_spill_clone_frame_0_t *frame = frame_;

	if (frame->key_live)
	{
		frame->key_live = false;
		std_vec_free_Checked(&frame->result->f0, 1, 1);
	}
}

/* txn_spill_clone_body_0 is Spilled's clone's body: the key, then its records. */
static void
txn_spill_clone_body_0(void *frame_)
{
	txn_spill_clone_frame_0_t *frame = frame_;

	std_slice_to_vec_Checked(&frame->result->f0, frame->self->f0.ptr, frame->self->f0.len, 1, 1);
	frame->key_live = true;
	txn_spill_versions_clone(&frame->self->f1, &frame->result->f1);
	frame->key_live = false;
}

/* txn_spill_Spilled_clone is Spilled's Clone: a key of a spill's file with its records copied, which a scan's step keeps. */
static void
txn_spill_Spilled_clone(const txn_TupleVecU8VecVecU8_t *self, txn_TupleVecU8VecVecU8_t *result)
{
	txn_spill_clone_frame_0_t frame = {.self = self, .result = result};

	std_landing(&frame, txn_spill_clone_body_0, txn_spill_clone_pad_0);
}

/* txn_spill_Read_looked_up returns a key's versions' records, newest first, or none for a key the file doesn't hold. */
static bool
txn_spill_Read_looked_up(TxnSpillRead *self, HostIoIo *io, const uint8 *key, size_t key_len, StdVec *result,
						 HostError *error)
{
	size_t		entry = txn_spill_Read_entries_before(self, key, key_len, false);
	size_t		low = 0;
	size_t		high;

	std_vec_new(result, sizeof(StdVec), _Alignof(StdVec));
	if (entry == 0)
		return true;
	if (!txn_spill_Read_chunk(self, io, entry - 1, error))
		return false;
	high = self->chunk.len;
	while (low < high)
	{
		size_t		middle = low + (high - low) / 2;
		const txn_TupleVecU8VecVecU8_t *found = &STD_VEC_AT(&self->chunk, txn_TupleVecU8VecVecU8_t, middle);
		StdOrdering order = std_bytes_Ord_cmp(found->f0.ptr, found->f0.len, key, key_len);

		if (order == STD_ORDERING_EQUAL)
		{
			txn_spill_versions_clone(&found->f1, result);
			return true;
		}
		if (order == STD_ORDERING_LESS)
			low = middle + 1;
		else
			high = middle;
	}
	return true;
}

/* txn_spill_Read_next returns the file's first key past a place the way a scan goes, with its versions' records, or nothing past its last. */
static bool
txn_spill_Read_next(TxnSpillRead *self, HostIoIo *io, bool has_after, const uint8 *after, size_t after_len,
					TxnMemtableTableDirection way, bool *some, txn_TupleVecU8VecVecU8_t *result, HostError *error)
{
	size_t		entries = self->entries.len;
	size_t		entry;

	*some = false;
	if (!has_after)
		entry = way == TXN_MEMTABLE_TABLE_DIRECTION_FORWARD ? 0 : std_saturating_sub_usize(entries, 1);
	else if (way == TXN_MEMTABLE_TABLE_DIRECTION_FORWARD)
		entry = std_saturating_sub_usize(txn_spill_Read_entries_before(self, after, after_len, false), 1);
	else
	{
		entry = txn_spill_Read_entries_before(self, after, after_len, true);
		if (entry == 0)
			return true;
		entry--;
	}
	while (entry < entries)
	{
		const txn_TupleVecU8VecVecU8_t *found = NULL;

		if (!txn_spill_Read_chunk(self, io, entry, error))
			return false;
		for (size_t k = 0; k < self->chunk.len && found == NULL; k++)
		{
			size_t		at = way == TXN_MEMTABLE_TABLE_DIRECTION_FORWARD ? k : self->chunk.len - 1 - k;
			const txn_TupleVecU8VecVecU8_t *key = &STD_VEC_AT(&self->chunk, txn_TupleVecU8VecVecU8_t, at);
			StdOrdering order = has_after ? std_bytes_Ord_cmp(key->f0.ptr, key->f0.len, after, after_len) :
				STD_ORDERING_EQUAL;

			if (!has_after || (way == TXN_MEMTABLE_TABLE_DIRECTION_FORWARD ? order == STD_ORDERING_GREATER :
							   order == STD_ORDERING_LESS))
				found = key;
		}
		if (found != NULL)
		{
			txn_spill_Spilled_clone(found, result);
			*some = true;
			return true;
		}
		if (way == TXN_MEMTABLE_TABLE_DIRECTION_FORWARD)
			entry = STD_ADD(usize, entry, 1, 1728, 39);
		else if (entry == 0)
			return true;
		else
			entry--;
	}
	return true;
}

/* txn_spill_reading_frame_0_t is reading's state across its pad: the backend's reads and their guard, the read it runs, and the file it opens, while live. */
typedef struct txn_spill_reading_frame_0_t
{
	HostIoIo   *io;
	const StdPathBuf *path;
	const TxnRunsNames *names;
	bool		has_len;
	uint64		len;
	bool		(*with) (void *captures, HostIoIo *io, TxnSpillRead *read, HostError *error);
	void	   *captures;
	HostError  *error;
	bool		ok;
	HostLocalLocal reads;
	bool		reads_live;
	HostLockLockedRwLockWriteGuard guard;
	bool		guard_live;
	TxnSpillRead read;
	bool		read_live;
	HostFile	file;
	bool		file_live;
} txn_spill_reading_frame_0_t;

/* txn_spill_reading_pad_0 is reading's pad: what it holds, while live. */
static void
txn_spill_reading_pad_0(void *frame_)
{
	txn_spill_reading_frame_0_t *frame = frame_;

	if (frame->file_live)
	{
		frame->file_live = false;
		host_File_drop(&frame->file);
	}
	if (frame->read_live)
	{
		frame->read_live = false;
		txn_spill_Read_drop(&frame->read);
	}
	if (frame->guard_live)
	{
		frame->guard_live = false;
		host_lock_LockedRwLockWriteGuard_drop(&frame->guard);
	}
	if (frame->reads_live)
	{
		frame->reads_live = false;
		host_local_Local_drop(&frame->reads, &TXN_SPILL_READS_TYPE);
	}
}

/* txn_spill_reading_lock locks the backend's reads, answering them. */
static StdVec *
txn_spill_reading_lock(txn_spill_reading_frame_0_t *frame)
{
	frame->guard = host_lock_IoMutex_lock((HostLockIoMutex *) host_local_Local_Deref_deref(&frame->reads,
																						   &TXN_SPILL_READS_TYPE),
										  &TXN_SPILL_READS_LOCK_TYPE, &frame->io->held);
	frame->guard_live = true;
	return host_lock_LockedRwLockWriteGuard_DerefMut_deref_mut(&frame->guard);
}

/* txn_spill_reading_opened opens the file the backend keeps no read of and reads its index into the frame's read. */
static bool
txn_spill_reading_opened(txn_spill_reading_frame_0_t *frame)
{
	ChunksErrorError failed;
	TxnSpillRead *read = &frame->read;

	if (!chunks_file_open_rw(HOST_IO_IO_IO_DOORS(frame->io), frame->path->bytes.ptr, frame->path->bytes.len, false,
							 &frame->file, &failed))
	{
		*frame->error = txn_base_storage(&failed);
		return false;
	}
	frame->file_live = true;
	std_PathBuf_Clone_clone(frame->path, &read->path);
	read->names = *frame->names;
	read->has_chunk = false;
	read->len = frame->len;
	std_vec_new(&read->entries, sizeof(txn_TupleVecU8U64_t), _Alignof(txn_TupleVecU8U64_t));
	frame->read_live = true;
	if ((!frame->has_len && !host_size(HOST_IO_IO_READ_DOORS(frame->io), &frame->file, &read->len, frame->error)) ||
		!txn_runs_spill_index(frame->io, &frame->file, read->len, &read->index_at, &read->entries, frame->error))
		return false;
	frame->file_live = false;
	host_File_drop(&frame->file);
	return true;
}

/* txn_spill_reading_body_0 is reading's body. */
static void
txn_spill_reading_body_0(void *frame_)
{
	txn_spill_reading_frame_0_t *frame = frame_;
	StdVec	   *reads;
	size_t		at;

	frame->reads = host_local_BackendLocal_get(&TXN_SPILL_READS.local, &TXN_SPILL_READS_TYPE);
	frame->reads_live = true;
	reads = txn_spill_reading_lock(frame);
	for (at = 0; at < reads->len; at++)
	{
		const TxnSpillRead *kept = &STD_VEC_AT(reads, TxnSpillRead, at);

		if (std_path_PartialEq_eq(kept->path.bytes.ptr, kept->path.bytes.len, frame->path->bytes.ptr,
								  frame->path->bytes.len) && (!frame->has_len || kept->len == frame->len))
			break;
	}
	if (at < reads->len)
	{
		std_vec_remove(reads, at, &frame->read, sizeof(frame->read), _Alignof(TxnSpillRead), RUST_FILE,
					   1651, 29);
		frame->read_live = true;
	}
	frame->guard_live = false;
	host_lock_LockedRwLockWriteGuard_drop(&frame->guard);
	if (!frame->read_live && !txn_spill_reading_opened(frame))
	{
		txn_spill_reading_pad_0(frame);
		return;
	}
	frame->ok = frame->with(frame->captures, frame->io, &frame->read, frame->error);
	reads = txn_spill_reading_lock(frame);
	std_vec_insert(reads, 0, &frame->read, sizeof(frame->read), _Alignof(TxnSpillRead), RUST_FILE,
				   1667, 11);
	frame->read_live = false;
	std_vec_truncate(reads, TXN_SPILL_KEPT, sizeof(TxnSpillRead), txn_spill_Read_drop);
	txn_spill_reading_pad_0(frame);
}

/*
 * txn_spill_reading runs a read of a spill's file of the length its
 * manifest lists, or of its own for a pending file's, keeping the read first
 * among the backend's and reading the index of a file it doesn't keep.
 */
static bool
txn_spill_reading(HostIoIo *io, const StdPathBuf *path, const TxnRunsNames *names, bool has_len, uint64 len,
				  bool (*with) (void *captures, HostIoIo *io, TxnSpillRead *read, HostError *error), void *captures,
				  HostError *error)
{
	txn_spill_reading_frame_0_t frame = {.io = io, .path = path, .names = names, .has_len = has_len, .len = len,
	.with = with, .captures = captures, .error = error};

	std_landing(&frame, txn_spill_reading_body_0, txn_spill_reading_pad_0);
	return frame.ok;
}

/* txn_spill_consistent reads a storage's slots and then lists its manifest, starting over while a change of the manifest moved its generation between the two, setting its caller's live flags of the two. */
static bool
txn_spill_consistent(const TxnSpillHomeOps *home, HostIoIo *io, HostSharedRecord state, const HostStorage *storage,
					 StdVec *slotted, bool *slotted_live, txn_TupleVecFileU64_t *listed, bool *listed_live,
					 HostError *error)
{
	HostSharedOp load = {.tag = HOST_SHARED_OP_LOAD};

	for (;;)
	{
		uint64		before = host_shared_Record_atomic(state, home->changed, load);

		if (!txn_spill_pended(home, state, storage, slotted, error))
			return false;
		*slotted_live = true;
		if (!host_pause(HOST_SHARED_IO_PAUSES(io), "doltgres-spill-slots-read", error) ||
			!txn_spill_listed(home, io, state, storage, listed, error))
			return false;
		*listed_live = true;
		if (before == listed->f1)
			return true;
		*listed_live = false;
		std_vec_free_Checked(&listed->f0, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
		*slotted_live = false;
		std_vec_free_Checked(slotted, sizeof(txn_TupleU64KindU32OptionU32_t), _Alignof(txn_TupleU64KindU32OptionU32_t));
	}
}

/* TxnSpillReading is Reading: a spill file a reader reads, its xid, its kind, its number, and its length when its manifest lists it. */
typedef struct TxnSpillReading
{
	uint64		xid;
	TxnRunsKind kind;
	uint32		number;
	bool		has_len;
	uint64		len;
} TxnSpillReading;

/* txn_spill_files_frame_0_t is files' state across its pad: the slots, the listed files, the spills' directory, a file's path, and the readings, while live. */
typedef struct txn_spill_files_frame_0_t
{
	const TxnSpillHomeOps *home;
	HostIoIo   *io;
	HostSharedRecord state;
	const HostStorage *storage;
	TxnRunsKind kind;
	bool		has_key;
	const uint8 *key;
	size_t		key_len;
	StdVec	   *result;
	HostError  *error;
	bool		ok;
	bool		result_live;
	StdVec		slotted;
	bool		slotted_live;
	txn_TupleVecFileU64_t listed;
	bool		listed_live;
	StdPathBuf	dir;
	bool		dir_live;
	StdString	name;
	bool		name_live;
	StdPathBuf	path;
	bool		path_live;
	/* through is the point the newest base passed a map's spills at, or 0. */
	HostLsn		through;
	/* residuals is a Vec<FilesResidual> of the residuals read at that point. */
	StdVec		residuals;
	bool		residuals_live;
	/* passing is a Vec<(u64, u64)> of each transaction's passing point. */
	StdVec		passing;
	bool		passing_live;
} txn_spill_files_frame_0_t;

/* TxnSpillFilesResidual is a transaction's residual of a kind that files reads, with its fold's point and the greatest number. */
typedef struct TxnSpillFilesResidual
{
	uint64		xid;
	TxnRunsKind kind;
	HostLsn		from;
	uint32		number;
} TxnSpillFilesResidual;

/* txn_spill_files_pad_0 is files' pad: what it holds, while live. */
static void
txn_spill_files_pad_0(void *frame_)
{
	txn_spill_files_frame_0_t *frame = frame_;

	if (frame->passing_live)
	{
		frame->passing_live = false;
		std_vec_free_Checked(&frame->passing, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
	}
	if (frame->residuals_live)
	{
		frame->residuals_live = false;
		std_vec_free_Checked(&frame->residuals, sizeof(TxnSpillFilesResidual), _Alignof(TxnSpillFilesResidual));
	}
	if (frame->path_live)
	{
		frame->path_live = false;
		std_vec_free_Checked(&frame->path.bytes, 1, 1);
	}
	if (frame->name_live)
	{
		frame->name_live = false;
		std_vec_free_Checked(&frame->name, 1, 1);
	}
	if (frame->dir_live)
	{
		frame->dir_live = false;
		std_vec_free_Checked(&frame->dir.bytes, 1, 1);
	}
	if (frame->listed_live)
	{
		frame->listed_live = false;
		std_vec_free_Checked(&frame->listed.f0, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	}
	if (frame->slotted_live)
	{
		frame->slotted_live = false;
		std_vec_free_Checked(&frame->slotted, sizeof(txn_TupleU64KindU32OptionU32_t),
							 _Alignof(txn_TupleU64KindU32OptionU32_t));
	}
	if (frame->result_live)
	{
		frame->result_live = false;
		std_vec_free_Checked(frame->result, sizeof(TxnSpillReading), _Alignof(TxnSpillReading));
	}
}

/* txn_spill_files_push adds a reading to the frame's result. */
static bool
txn_spill_files_push(txn_spill_files_frame_0_t *frame, const TxnSpillReading *reading)
{
	if (!std_vec_try_reserve(frame->result, 1, sizeof(*reading), _Alignof(TxnSpillReading)))
	{
		*frame->error = host_Error_out_of_memory();
		return false;
	}
	std_vec_push_Checked(frame->result, reading, sizeof(*reading), _Alignof(TxnSpillReading));
	return true;
}

/* txn_spill_files_hit reports whether the frame's key hits a listed file's filter. */
static bool
txn_spill_files_hit(txn_spill_files_frame_0_t *frame, const TxnSpillFile *file, bool *hit)
{
	TxnRunsNames names;

	if (!txn_spill_names(frame->home, frame->state, frame->storage, file->kind, file->xid, &names, frame->error))
		return false;
	txn_runs_spill_name(&names, file->number, &frame->name);
	frame->name_live = true;
	std_path_join(frame->dir.bytes.ptr, frame->dir.bytes.len, (const char *) frame->name.ptr, frame->name.len,
				  &frame->path);
	frame->path_live = true;
	frame->name_live = false;
	std_vec_free_Checked(&frame->name, 1, 1);
	if (!txn_spill_hits(frame->io, &frame->path, file, frame->key, frame->key_len, hit, frame->error))
		return false;
	frame->path_live = false;
	std_vec_free_Checked(&frame->path.bytes, 1, 1);
	return true;
}

/* txn_spill_files_residuals gathers the residuals of the listed files read at the newest base's point: of each transaction and kind, the greatest number. */
static bool
txn_spill_files_residuals(txn_spill_files_frame_0_t *frame)
{
	std_vec_new(&frame->residuals, sizeof(TxnSpillFilesResidual), _Alignof(TxnSpillFilesResidual));
	frame->residuals_live = true;
	for (size_t k = 0; k < frame->listed.f0.len; k++)
	{
		const TxnSpillFile *file = &STD_VEC_AT(&frame->listed.f0, TxnSpillFile, k);
		TxnSpillFilesResidual residual = {file->xid, file->kind, file->from, file->number};
		bool		found = false;

		if (file->from == 0 || file->from != frame->through)
			continue;
		for (size_t j = 0; j < frame->residuals.len && !found; j++)
		{
			TxnSpillFilesResidual *at = &STD_VEC_AT(&frame->residuals, TxnSpillFilesResidual, j);

			found = at->xid == file->xid && at->kind == file->kind;
			if (found && (file->from > at->from || (file->from == at->from && file->number > at->number)))
				*at = residual;
		}
		if (found)
			continue;
		if (!std_vec_try_reserve(&frame->residuals, 1, sizeof(residual), _Alignof(TxnSpillFilesResidual)))
		{
			*frame->error = host_Error_out_of_memory();
			return false;
		}
		std_vec_push_Checked(&frame->residuals, &residual, sizeof(residual), _Alignof(TxnSpillFilesResidual));
	}
	return true;
}

/*
 * txn_spill_files_passed reports whether a listed file is left out of the
 * read: of a residual's transaction and kind every file but that residual,
 * any other residual, and a map's spill the newest base passed, a committed
 * transaction's whose commit ends at or below the base's point or one
 * marked with a passing point there.
 */
static bool
txn_spill_files_passed(txn_spill_files_frame_0_t *frame, const TxnSpillFile *file, bool *result)
{
	uint64		point = 0;
	bool		cached = false;

	*result = false;
	for (size_t j = 0; j < frame->residuals.len; j++)
	{
		const TxnSpillFilesResidual *residual = &STD_VEC_AT(&frame->residuals, TxnSpillFilesResidual, j);

		if (residual->xid == file->xid && residual->kind == file->kind)
		{
			*result = file->from != residual->from || file->number != residual->number;
			return true;
		}
	}
	if (file->from != 0)
	{
		*result = true;
		return true;
	}
	if (frame->through == 0)
		return true;
	for (size_t j = 0; j < frame->passing.len && !cached; j++)
	{
		const std_TupleU64U64_t *passing = &STD_VEC_AT(&frame->passing, std_TupleU64U64_t, j);

		cached = passing->f0 == file->xid;
		if (cached)
			point = passing->f1;
	}
	if (!cached)
	{
		std_TupleU64U64_t passing = {file->xid, file->passed};

		if (file->passed == 0)
		{
			HostXid		xid = {file->xid};
			TxnStatusCommit commit;

			if (!txn_status_committed(frame->io, xid, &commit, frame->error))
				return false;
			if (commit.tag == TXN_STATUS_COMMIT_AT)
				passing.f1 = commit.u.at;
		}
		if (!std_vec_try_reserve(&frame->passing, 1, sizeof(passing), _Alignof(std_TupleU64U64_t)))
		{
			*frame->error = host_Error_out_of_memory();
			return false;
		}
		std_vec_push_Checked(&frame->passing, &passing, sizeof(passing), _Alignof(std_TupleU64U64_t));
		point = passing.f1;
	}
	*result = point != 0 && point <= frame->through;
	return true;
}

/* txn_spill_files_body_0 is files' body. */
static void
txn_spill_files_body_0(void *frame_)
{
	txn_spill_files_frame_0_t *frame = frame_;

	if (!txn_spill_consistent(frame->home, frame->io, frame->state, frame->storage, &frame->slotted,
							  &frame->slotted_live, &frame->listed, &frame->listed_live, frame->error))
	{
		txn_spill_files_pad_0(frame);
		return;
	}
	txn_runs_spill_directory(frame->storage->database, frame->home->own(frame->state), &frame->dir);
	frame->dir_live = true;
	std_vec_new(frame->result, sizeof(TxnSpillReading), _Alignof(TxnSpillReading));
	frame->result_live = true;
	std_vec_new(&frame->passing, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
	frame->passing_live = true;
	if (((frame->kind == TXN_RUNS_KIND_SPILLED_FREE || frame->kind == TXN_RUNS_KIND_SPILLED_VALUES) &&
		 !frame->home->through(frame->io, frame->state, &frame->through, frame->error)) ||
		!txn_spill_files_residuals(frame))
	{
		txn_spill_files_pad_0(frame);
		return;
	}
	for (size_t k = 0; k < frame->slotted.len; k++)
	{
		const txn_TupleU64KindU32OptionU32_t *slot = &STD_VEC_AT(&frame->slotted, txn_TupleU64KindU32OptionU32_t, k);
		TxnSpillReading reading = {slot->f0, slot->f1, slot->f2};

		if (txn_spill_tree(slot->f1, frame->kind) && !txn_spill_files_push(frame, &reading))
		{
			txn_spill_files_pad_0(frame);
			return;
		}
	}
	for (size_t k = 0; k < frame->listed.f0.len; k++)
	{
		const TxnSpillFile *file = &STD_VEC_AT(&frame->listed.f0, TxnSpillFile, k);
		TxnSpillReading reading = {file->xid, file->kind, file->number, true, file->len};
		bool		seen = false;
		bool		hit = true;
		bool		passed;

		for (size_t j = 0; j < frame->result->len && !seen; j++)
		{
			const TxnSpillReading *found = &STD_VEC_AT(frame->result, TxnSpillReading, j);

			seen = found->xid == file->xid && found->number == file->number;
		}
		if (!txn_spill_tree(file->kind, frame->kind) || seen ||
			txn_spill_replaced_by(frame->slotted.ptr, frame->slotted.len, file))
			continue;
		if (!txn_spill_files_passed(frame, file, &passed))
		{
			txn_spill_files_pad_0(frame);
			return;
		}
		if (passed)
			continue;
		if ((frame->has_key && !txn_spill_files_hit(frame, file, &hit)) ||
			(hit && !txn_spill_files_push(frame, &reading)))
		{
			txn_spill_files_pad_0(frame);
			return;
		}
	}
	frame->result_live = false;
	frame->ok = true;
	txn_spill_files_pad_0(frame);
}

/*
 * txn_spill_files returns a storage's spill files of a kind's tree: those
 * processes' slots name first, then the listed newest-listed first, each
 * once, and, for a reader that looks for a key, only the listed whose
 * filters it hits. A map's listed spill the newest base has passed (the
 * home's through) is left out: a committed transaction's whose commit ends
 * at or below the base's fold point, or an aborted one's marked with an end
 * there, which the fold that installed the base applied or left in a
 * residual, so a reader sees its versions once. A residual is read only
 * while that point is the residual's own, and of a transaction's residuals
 * of a kind with that point only the one with the greatest number, which
 * supersedes the rest and every other file of its transaction and kind
 * (bugs row 228); a residual whose fold's install failed is never read,
 * since no base has its point.
 */
static bool
txn_spill_files(const TxnSpillHomeOps *home, HostIoIo *io, HostSharedRecord state, const HostStorage *storage,
				TxnRunsKind kind, bool has_key, const uint8 *key, size_t key_len, StdVec *result, HostError *error)
{
	txn_spill_files_frame_0_t frame = {.home = home, .io = io, .state = state, .storage = storage, .kind = kind,
	.has_key = has_key, .key = key, .key_len = key_len, .result = result, .error = error};

	std_landing(&frame, txn_spill_files_body_0, txn_spill_files_pad_0);
	return frame.ok;
}

/* txn_spill_looked_up_captures_t is a lookup's captures: the key and the records it answers. */
typedef struct txn_spill_looked_up_captures_t
{
	const uint8 *key;
	size_t		key_len;
	StdVec	   *records;
	bool	   *records_live;
} txn_spill_looked_up_captures_t;

/* txn_spill_looked_up_closure_0 is a lookup's read: the key's records in the file. */
static bool
txn_spill_looked_up_closure_0(void *captures, HostIoIo *io, TxnSpillRead *read, HostError *error)
{
	txn_spill_looked_up_captures_t *c = captures;

	if (!txn_spill_Read_looked_up(read, io, c->key, c->key_len, c->records, error))
		return false;
	*c->records_live = true;
	return true;
}

/* txn_spill_versions_frame_0_t is versions' state across its pad: the readings, the spills' directory, a file's path and records, and the versions found, while live. */
typedef struct txn_spill_versions_frame_0_t
{
	const TxnSpillHomeOps *home;
	HostIoIo   *io;
	HostSharedRecord state;
	TxnRunsKind kind;
	const uint8 *key;
	size_t		key_len;
	StdVec	   *result;
	HostError  *error;
	bool		ok;
	bool		result_live;
	HostStorage storage;
	StdVec		files;
	bool		files_live;
	StdPathBuf	dir;
	bool		dir_live;
	StdString	name;
	bool		name_live;
	StdPathBuf	path;
	bool		path_live;
	/* records is a Vec<Vec<u8>>, of which moved are taken. */
	StdVec		records;
	bool		records_live;
	size_t		moved;
} txn_spill_versions_frame_0_t;

/* txn_spill_versions_pad_0 is versions' pad: what it holds, while live. */
static void
txn_spill_versions_pad_0(void *frame_)
{
	txn_spill_versions_frame_0_t *frame = frame_;

	if (frame->records_live)
	{
		frame->records_live = false;
		for (size_t k = frame->moved; k < frame->records.len; k++)
			std_vec_free_Checked(&STD_VEC_AT(&frame->records, StdVec, k), 1, 1);
		std_vec_free_Checked(&frame->records, sizeof(StdVec), _Alignof(StdVec));
	}
	if (frame->path_live)
	{
		frame->path_live = false;
		std_vec_free_Checked(&frame->path.bytes, 1, 1);
	}
	if (frame->name_live)
	{
		frame->name_live = false;
		std_vec_free_Checked(&frame->name, 1, 1);
	}
	if (frame->dir_live)
	{
		frame->dir_live = false;
		std_vec_free_Checked(&frame->dir.bytes, 1, 1);
	}
	if (frame->files_live)
	{
		frame->files_live = false;
		std_vec_free_Checked(&frame->files, sizeof(TxnSpillReading), _Alignof(TxnSpillReading));
	}
	if (frame->result_live)
	{
		frame->result_live = false;
		txn_memtable_version_versions_drop(frame->result, 0);
	}
}

/* txn_spill_versions_file reads one file's versions of the key into the frame's result, each by its handle. */
static bool
txn_spill_versions_file(txn_spill_versions_frame_0_t *frame, const TxnSpillReading *reading)
{
	TxnRunsNames names;
	txn_spill_looked_up_captures_t captures = {frame->key, frame->key_len, &frame->records, &frame->records_live};

	if (!txn_spill_names(frame->home, frame->state, &frame->storage, reading->kind, reading->xid, &names,
						 frame->error))
		return false;
	txn_runs_spill_name(&names, reading->number, &frame->name);
	frame->name_live = true;
	std_path_join(frame->dir.bytes.ptr, frame->dir.bytes.len, (const char *) frame->name.ptr, frame->name.len,
				  &frame->path);
	frame->path_live = true;
	frame->name_live = false;
	std_vec_free_Checked(&frame->name, 1, 1);
	frame->moved = 0;
	if (!txn_spill_reading(frame->io, &frame->path, &names, reading->has_len, reading->len,
						   txn_spill_looked_up_closure_0, &captures, frame->error))
		return false;
	if (!std_vec_try_reserve(frame->result, frame->records.len, sizeof(TxnMemtableVersionVersion),
							 _Alignof(TxnMemtableVersionVersion)))
	{
		*frame->error = host_Error_out_of_memory();
		return false;
	}
	while (frame->moved < frame->records.len)
	{
		uint32		place = (uint32) frame->moved;
		TxnMemtableVersionVersion version;

		version = txn_memtable_version_Version_spilled(txn_memtable_version_Handle_spilled(reading->xid,
																						   reading->number, place),
													   &STD_VEC_AT(&frame->records, StdVec, frame->moved));
		frame->moved++;
		std_vec_push_Checked(frame->result, &version, sizeof(version), _Alignof(TxnMemtableVersionVersion));
	}
	frame->records_live = false;
	std_vec_free_Checked(&frame->records, sizeof(StdVec), _Alignof(StdVec));
	frame->path_live = false;
	std_vec_free_Checked(&frame->path.bytes, 1, 1);
	return true;
}

/* txn_spill_versions_body_0 is versions' body. */
static void
txn_spill_versions_body_0(void *frame_)
{
	txn_spill_versions_frame_0_t *frame = frame_;
	HostSharedOp load = {.tag = HOST_SHARED_OP_LOAD};
	TxnStatePersistence persistence;

	std_vec_new(frame->result, sizeof(TxnMemtableVersionVersion), _Alignof(TxnMemtableVersionVersion));
	if ((host_shared_Record_atomic(frame->state, frame->home->pended, load) == 0 &&
		 host_shared_Record_atomic(frame->state, frame->home->listed, load) == 0) ||
		!frame->home->described(frame->state, &frame->storage, &persistence))
	{
		frame->ok = true;
		return;
	}
	frame->result_live = true;
	txn_runs_spill_directory(frame->storage.database, frame->home->own(frame->state), &frame->dir);
	frame->dir_live = true;
	if (!txn_spill_files(frame->home, frame->io, frame->state, &frame->storage, frame->kind, true, frame->key,
						 frame->key_len, &frame->files, frame->error))
	{
		txn_spill_versions_pad_0(frame);
		return;
	}
	frame->files_live = true;
	if (!host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-spill-listed", frame->error))
	{
		txn_spill_versions_pad_0(frame);
		return;
	}
	for (size_t k = 0; k < frame->files.len; k++)
		if (!txn_spill_versions_file(frame, &STD_VEC_AT(&frame->files, TxnSpillReading, k)))
		{
			txn_spill_versions_pad_0(frame);
			return;
		}
	frame->result_live = false;
	frame->ok = true;
	txn_spill_versions_pad_0(frame);
}

/*
 * txn_spill_versions returns a key's versions in a storage's spills of a
 * kind, newest first: those of the file a process's slot names, then those
 * of the listed files whose filters the key hits, newest-listed first, each
 * file once, read under a hold.
 */
bool
txn_spill_versions(const TxnSpillHomeOps *home, HostIoIo *io, const HostDeltaHold *hold, HostSharedRecord state,
				   TxnRunsKind kind, const uint8 *key, size_t key_len, StdVec *result, HostError *error)
{
	txn_spill_versions_frame_0_t frame = {.home = home, .io = io, .state = state, .kind = kind, .key = key,
	.key_len = key_len, .result = result, .error = error};

	std_landing(&frame, txn_spill_versions_body_0, txn_spill_versions_pad_0);
	return frame.ok;
}

/* txn_spill_next_captures_t is next's read's captures: the place, the way, and the key it answers. */
typedef struct txn_spill_next_captures_t
{
	bool		has_after;
	const uint8 *after;
	size_t		after_len;
	TxnMemtableTableDirection way;
	bool	   *some;
	txn_TupleVecU8VecVecU8_t *result;
	bool	   *result_live;
} txn_spill_next_captures_t;

/* txn_spill_next_closure_0 is next's read: the file's first key past the place. */
static bool
txn_spill_next_closure_0(void *captures, HostIoIo *io, TxnSpillRead *read, HostError *error)
{
	txn_spill_next_captures_t *c = captures;

	if (!txn_spill_Read_next(read, io, c->has_after, c->after, c->after_len, c->way, c->some, c->result, error))
		return false;
	*c->result_live = *c->some;
	return true;
}

/* txn_spill_next_frame_0_t is next's state across its pad: the readings, the spills' directory, a file's path and key, and the key found, while live. */
typedef struct txn_spill_next_frame_0_t
{
	const TxnSpillHomeOps *home;
	HostIoIo   *io;
	HostSharedRecord state;
	TxnRunsKind kind;
	bool		has_after;
	const uint8 *after;
	size_t		after_len;
	TxnMemtableTableDirection way;
	bool	   *some;
	txn_TupleVecU8VecVersion_t *result;
	HostError  *error;
	bool		ok;
	bool		result_live;
	HostStorage storage;
	StdVec		files;
	bool		files_live;
	StdPathBuf	dir;
	bool		dir_live;
	StdString	name;
	bool		name_live;
	StdPathBuf	path;
	bool		path_live;
	txn_TupleVecU8VecVecU8_t spilled;
	bool		spilled_live;
	size_t		moved;
} txn_spill_next_frame_0_t;

/* txn_spill_next_spilled_drop drops a file's key next read, its records not moved, while live. */
static void
txn_spill_next_spilled_drop(txn_spill_next_frame_0_t *frame)
{
	if (frame->spilled_live)
	{
		frame->spilled_live = false;
		std_vec_free_Checked(&frame->spilled.f0, 1, 1);
		for (size_t k = frame->moved; k < frame->spilled.f1.len; k++)
			std_vec_free_Checked(&STD_VEC_AT(&frame->spilled.f1, StdVec, k), 1, 1);
		std_vec_free_Checked(&frame->spilled.f1, sizeof(StdVec), _Alignof(StdVec));
	}
}

/* txn_spill_next_pad_0 is next's pad: what it holds, while live. */
static void
txn_spill_next_pad_0(void *frame_)
{
	txn_spill_next_frame_0_t *frame = frame_;

	txn_spill_next_spilled_drop(frame);
	if (frame->path_live)
	{
		frame->path_live = false;
		std_vec_free_Checked(&frame->path.bytes, 1, 1);
	}
	if (frame->name_live)
	{
		frame->name_live = false;
		std_vec_free_Checked(&frame->name, 1, 1);
	}
	if (frame->dir_live)
	{
		frame->dir_live = false;
		std_vec_free_Checked(&frame->dir.bytes, 1, 1);
	}
	if (frame->files_live)
	{
		frame->files_live = false;
		std_vec_free_Checked(&frame->files, sizeof(TxnSpillReading), _Alignof(TxnSpillReading));
	}
	if (frame->result_live)
	{
		frame->result_live = false;
		std_vec_free_Checked(&frame->result->f0, 1, 1);
		txn_memtable_version_versions_drop(&frame->result->f1, 0);
	}
}

/* txn_spill_next_file reads one file's first key past the place and keeps it when it is nearer than the one found, or the same key. */
static bool
txn_spill_next_file(txn_spill_next_frame_0_t *frame, const TxnSpillReading *reading)
{
	TxnRunsNames names;
	bool		some;
	txn_spill_next_captures_t captures = {frame->has_after, frame->after, frame->after_len, frame->way, &some,
	&frame->spilled, &frame->spilled_live};
	bool		nearer = !frame->result_live;
	bool		same = false;

	if (!txn_spill_names(frame->home, frame->state, &frame->storage, reading->kind, reading->xid, &names,
						 frame->error))
		return false;
	txn_runs_spill_name(&names, reading->number, &frame->name);
	frame->name_live = true;
	std_path_join(frame->dir.bytes.ptr, frame->dir.bytes.len, (const char *) frame->name.ptr, frame->name.len,
				  &frame->path);
	frame->path_live = true;
	frame->name_live = false;
	std_vec_free_Checked(&frame->name, 1, 1);
	frame->moved = 0;
	if (!txn_spill_reading(frame->io, &frame->path, &names, reading->has_len, reading->len, txn_spill_next_closure_0,
						   &captures, frame->error))
		return false;
	frame->path_live = false;
	std_vec_free_Checked(&frame->path.bytes, 1, 1);
	if (!some)
		return true;
	if (frame->result_live)
	{
		StdOrdering order = std_bytes_Ord_cmp(frame->spilled.f0.ptr, frame->spilled.f0.len, frame->result->f0.ptr,
											  frame->result->f0.len);

		nearer = frame->way == TXN_MEMTABLE_TABLE_DIRECTION_FORWARD ? order == STD_ORDERING_LESS :
			order == STD_ORDERING_GREATER;
		same = order == STD_ORDERING_EQUAL;
	}
	if (nearer)
	{
		if (frame->result_live)
		{
			frame->result_live = false;
			std_vec_free_Checked(&frame->result->f0, 1, 1);
			txn_memtable_version_versions_drop(&frame->result->f1, 0);
		}
		frame->result->f0 = frame->spilled.f0;
		std_vec_new(&frame->spilled.f0, 1, 1);
		std_vec_new(&frame->result->f1, sizeof(TxnMemtableVersionVersion), _Alignof(TxnMemtableVersionVersion));
		frame->result_live = true;
	}
	if (nearer || same)
	{
		if (!std_vec_try_reserve(&frame->result->f1, frame->spilled.f1.len, sizeof(TxnMemtableVersionVersion),
								 _Alignof(TxnMemtableVersionVersion)))
		{
			*frame->error = host_Error_out_of_memory();
			return false;
		}
		while (frame->moved < frame->spilled.f1.len)
		{
			uint32		place = (uint32) frame->moved;
			TxnMemtableVersionVersion version;

			version = txn_memtable_version_Version_spilled(txn_memtable_version_Handle_spilled(reading->xid,
																							   reading->number,
																							   place),
														   &STD_VEC_AT(&frame->spilled.f1, StdVec, frame->moved));
			frame->moved++;
			std_vec_push_Checked(&frame->result->f1, &version, sizeof(version), _Alignof(TxnMemtableVersionVersion));
		}
	}
	txn_spill_next_spilled_drop(frame);
	return true;
}

/* txn_spill_next_body_0 is next's body. */
static void
txn_spill_next_body_0(void *frame_)
{
	txn_spill_next_frame_0_t *frame = frame_;
	HostSharedOp load = {.tag = HOST_SHARED_OP_LOAD};
	TxnStatePersistence persistence;

	*frame->some = false;
	if ((host_shared_Record_atomic(frame->state, frame->home->pended, load) == 0 &&
		 host_shared_Record_atomic(frame->state, frame->home->listed, load) == 0) ||
		!frame->home->described(frame->state, &frame->storage, &persistence))
	{
		frame->ok = true;
		return;
	}
	txn_runs_spill_directory(frame->storage.database, frame->home->own(frame->state), &frame->dir);
	frame->dir_live = true;
	if (!txn_spill_files(frame->home, frame->io, frame->state, &frame->storage, frame->kind, false, NULL, 0,
						 &frame->files, frame->error))
	{
		txn_spill_next_pad_0(frame);
		return;
	}
	frame->files_live = true;
	for (size_t k = 0; k < frame->files.len; k++)
		if (!txn_spill_next_file(frame, &STD_VEC_AT(&frame->files, TxnSpillReading, k)))
		{
			txn_spill_next_pad_0(frame);
			return;
		}
	*frame->some = frame->result_live;
	frame->result_live = false;
	frame->ok = true;
	txn_spill_next_pad_0(frame);
}

/*
 * txn_spill_next returns the first key of a storage's spills of a kind past
 * a place, the way a scan goes, with its versions, newest first, the newer
 * files' first, or nothing past the last, read under a hold; with no place
 * it starts at the first key or the last.
 */
bool
txn_spill_next(const TxnSpillHomeOps *home, HostIoIo *io, const HostDeltaHold *hold, HostSharedRecord state,
			   TxnRunsKind kind, bool has_after, const uint8 *after, size_t after_len, TxnMemtableTableDirection way,
			   bool *some, txn_TupleVecU8VecVersion_t *result, HostError *error)
{
	txn_spill_next_frame_0_t frame = {.home = home, .io = io, .state = state, .kind = kind, .has_after = has_after,
	.after = after, .after_len = after_len, .way = way, .some = some, .result = result, .error = error};

	std_landing(&frame, txn_spill_next_body_0, txn_spill_next_pad_0);
	return frame.ok;
}

/* txn_spill_held_frame_0_t is the state of a read of a key's spilled versions under a hold across its pad: the hold and the versions, while live. */
typedef struct txn_spill_held_frame_0_t
{
	const TxnSpillHomeOps *home;
	HostIoIo   *io;
	HostSharedRecord state;
	TxnRunsKind kind;
	const uint8 *key;
	size_t		key_len;
	bool		(*with) (void *captures, const StdVec *versions, HostError *error);
	void	   *captures;
	HostError  *error;
	bool		ok;
	HostDeltaHold hold;
	bool		hold_live;
	StdVec		versions;
	bool		versions_live;
} txn_spill_held_frame_0_t;

/* txn_spill_held_pad_0 is the pad of a read under a hold: the versions, then the hold, while live. */
static void
txn_spill_held_pad_0(void *frame_)
{
	txn_spill_held_frame_0_t *frame = frame_;

	if (frame->versions_live)
	{
		frame->versions_live = false;
		txn_memtable_version_versions_drop(&frame->versions, 0);
	}
	if (frame->hold_live)
	{
		frame->hold_live = false;
		host_delta_Hold_drop(&frame->hold);
	}
}

/* txn_spill_held_body_0 reads the key's spilled versions under a hold and runs the step on them. */
static void
txn_spill_held_body_0(void *frame_)
{
	txn_spill_held_frame_0_t *frame = frame_;

	if (!host_delta_hold(frame->io, &frame->hold, frame->error))
		return;
	frame->hold_live = true;
	if (!txn_spill_versions(frame->home, frame->io, &frame->hold, frame->state, frame->kind, frame->key,
							frame->key_len, &frame->versions, frame->error))
	{
		txn_spill_held_pad_0(frame);
		return;
	}
	frame->versions_live = true;
	frame->ok = frame->with(frame->captures, &frame->versions, frame->error);
	txn_spill_held_pad_0(frame);
}

/* txn_spill_held runs a step on a key's versions in a storage's spills of a kind, read under a hold it takes. */
static bool
txn_spill_held(const TxnSpillHomeOps *home, HostIoIo *io, HostSharedRecord state, TxnRunsKind kind, const uint8 *key,
			   size_t key_len, bool (*with) (void *captures, const StdVec *versions, HostError *error),
			   void *captures, HostError *error)
{
	txn_spill_held_frame_0_t frame = {.home = home, .io = io, .state = state, .kind = kind, .key = key,
	.key_len = key_len, .with = with, .captures = captures, .error = error};

	std_landing(&frame, txn_spill_held_body_0, txn_spill_held_pad_0);
	return frame.ok;
}

/* txn_spill_twinned_captures_t is twinned's and holds' step's captures: the record's bytes or the LSN, and the answer. */
typedef struct txn_spill_twinned_captures_t
{
	const uint8 *bytes;
	size_t		bytes_len;
	uint64		lsn;
	bool	   *result;
} txn_spill_twinned_captures_t;

/* txn_spill_twinned_closure_0 is twinned's step: whether a version's record is the bytes. */
static bool
txn_spill_twinned_closure_0(void *captures, const StdVec *versions, HostError *error)
{
	txn_spill_twinned_captures_t *c = captures;

	*c->result = false;
	for (size_t k = 0; k < versions->len && !*c->result; k++)
	{
		TxnMemtableVersionRecord record = txn_memtable_version_Version_record(&STD_VEC_AT(versions,
																						  TxnMemtableVersionVersion,
																						  k));
		StdBytes	bytes = txn_memtable_version_Record_bytes(&record);

		*c->result = bytes.len == c->bytes_len && memcmp(bytes.ptr, c->bytes, bytes.len) == 0;
	}
	return true;
}

/* txn_spill_twinned reports whether a key's spills of a kind hold a version of a record's bytes. */
bool
txn_spill_twinned(HostIoIo *io, HostSharedRecord state, TxnRunsKind kind, const uint8 *key, size_t key_len,
				  const uint8 *bytes, size_t bytes_len, bool *result, HostError *error)
{
	txn_spill_twinned_captures_t captures = {bytes, bytes_len, 0, result};

	return txn_spill_held(&TXN_SPILL_STATE_HOME_OPS, io, state, kind, key, key_len, txn_spill_twinned_closure_0,
						  &captures, error);
}

/* txn_spill_holds_closure_0 is holds' step: whether a version's record carries the LSN. */
static bool
txn_spill_holds_closure_0(void *captures, const StdVec *versions, HostError *error)
{
	txn_spill_twinned_captures_t *c = captures;

	*c->result = false;
	for (size_t k = 0; k < versions->len && !*c->result; k++)
	{
		TxnMemtableVersionRecord record = txn_memtable_version_Version_record(&STD_VEC_AT(versions,
																						  TxnMemtableVersionVersion,
																						  k));
		uint64		lsn;

		*c->result = txn_memtable_version_Record_lsn(&record, &lsn) && lsn == c->lsn;
	}
	return true;
}

/* txn_spill_holds reports whether a key's spills of a kind hold a version carrying the end of a WAL record. */
bool
txn_spill_holds(HostIoIo *io, HostSharedRecord state, TxnRunsKind kind, const uint8 *key, size_t key_len, uint64 lsn,
				bool *result, HostError *error)
{
	txn_spill_twinned_captures_t captures = {NULL, 0, lsn, result};

	return txn_spill_held(&TXN_SPILL_STATE_HOME_OPS, io, state, kind, key, key_len, txn_spill_holds_closure_0,
						  &captures, error);
}

/* txn_spill_newest_value_captures_t is newest_value's step's captures: the entry it answers. */
typedef struct txn_spill_newest_value_captures_t
{
	bool	   *some;
	TxnMemtableMapEntry *result;
} txn_spill_newest_value_captures_t;

/* txn_spill_newest_value_closure_0 is newest_value's step: the newest version's put or removal. */
static bool
txn_spill_newest_value_closure_0(void *captures, const StdVec *versions, HostError *error)
{
	txn_spill_newest_value_captures_t *c = captures;
	TxnMemtableVersionRecord record;
	TxnMemtableVersionOpRef op;

	*c->some = versions->len > 0;
	if (!*c->some)
		return true;
	record = txn_memtable_version_Version_record(&STD_VEC_AT(versions, TxnMemtableVersionVersion, 0));
	op = txn_memtable_version_Record_op(&record);
	if (op.tag == TXN_MEMTABLE_VERSION_OP_REF_PUT)
	{
		c->result->has_value = true;
		std_slice_to_vec_Checked(&c->result->value, op.u.put.value.ptr, op.u.put.value.len, 1, 1);
		return true;
	}
	if (op.tag == TXN_MEMTABLE_VERSION_OP_REF_DELETE)
	{
		c->result->has_value = false;
		return true;
	}
	*c->some = false;
	*error = txn_spill_internal("a spilled map's version neither puts nor deletes");
	return false;
}

/* txn_spill_newest_value returns a value id's newest entry in a storage's spilled value maps: nothing for an id they hold no version of, and none for a removal. */
bool
txn_spill_newest_value(HostIoIo *io, HostSharedRecord state, uint32 id, bool *some, TxnMemtableMapEntry *result,
					   HostError *error)
{
	uint8		key[4];
	txn_spill_newest_value_captures_t captures = {some, result};

	std_u32_to_be_bytes(id, key);
	return txn_spill_held(&TXN_SPILL_STATE_HOME_OPS, io, state, TXN_RUNS_KIND_SPILLED_VALUES, key, sizeof(key),
						  txn_spill_newest_value_closure_0, &captures, error);
}

/* txn_spill_generation returns the generation of a storage's spills' manifest, which every change of it moves. */
uint64
txn_spill_generation(const TxnSpillHomeOps *home, HostSharedRecord state)
{
	HostSharedOp load = {.tag = HOST_SHARED_OP_LOAD};

	return host_shared_Record_atomic(state, home->changed, load);
}

/* txn_spill_chunk_versions returns a key's versions in a working state's chunk spills, newest first, as versions does. */
bool
txn_spill_chunk_versions(HostIoIo *io, const HostDeltaHold *hold, HostSharedRecord deltas, const uint8 *key,
						 size_t key_len, StdVec *result, HostError *error)
{
	return txn_spill_versions(&TXN_SPILL_BRANCH_DELTAS_HOME_OPS, io, hold, deltas, TXN_RUNS_KIND_SPILLED_CHUNKS, key,
							  key_len, result, error);
}

/* txn_spill_chunk_closure_0 is chunk's step: the newest spilled version's chunk. */
static bool
txn_spill_chunk_closure_0(void *captures, const StdVec *versions, HostError *error)
{
	txn_spill_newest_value_captures_t *c = captures;
	TxnMemtableVersionRecord record;
	TxnMemtableVersionOpRef op;

	*c->some = versions->len > 0;
	if (!*c->some)
		return true;
	record = txn_memtable_version_Version_record(&STD_VEC_AT(versions, TxnMemtableVersionVersion, 0));
	op = txn_memtable_version_Record_op(&record);
	if (op.tag != TXN_MEMTABLE_VERSION_OP_REF_PUT)
	{
		*c->some = false;
		*error = txn_spill_internal("a spilled chunk's version puts no chunk");
		return false;
	}
	std_slice_to_vec_Checked(&c->result->value, op.u.put.value.ptr, op.u.put.value.len, 1, 1);
	return true;
}

/*
 * txn_spill_chunk returns the bytes of the chunk under a key in a working
 * state's delta, else in its chunk spills, the newest version's, or nothing
 * for a key neither holds.
 */
bool
txn_spill_chunk(HostIoIo *io, HostSharedRecord deltas, const uint8 *key, size_t key_len, bool *some, StdVec *result,
				HostError *error)
{
	TxnMemtableMapEntry entry;
	txn_spill_newest_value_captures_t captures = {some, &entry};

	if (!txn_memtable_branch_get(io, deltas, key, key_len, some, result, error))
		return false;
	if (*some)
		return true;
	if (!txn_spill_held(&TXN_SPILL_BRANCH_DELTAS_HOME_OPS, io, deltas, TXN_RUNS_KIND_SPILLED_CHUNKS, key, key_len,
						txn_spill_chunk_closure_0, &captures, error))
		return false;
	if (*some)
		*result = entry.value;
	return true;
}

/* txn_spill_tuple_key_frame_0_t is tuple_key's state across its pad: the readings, the spills' directory, a file's path, and the values read, while live. */
typedef struct txn_spill_tuple_key_frame_0_t
{
	HostIoIo   *io;
	HostSharedRecord state;
	HostRow		tid;
	bool	   *some;
	StdVec	   *result;
	HostError  *error;
	bool		ok;
	HostStorage storage;
	StdVec		files;
	bool		files_live;
	StdPathBuf	dir;
	bool		dir_live;
	StdString	name;
	bool		name_live;
	StdPathBuf	path;
	bool		path_live;
	/* values is a Vec<Vec<u8>>. */
	StdVec		values;
	bool		values_live;
} txn_spill_tuple_key_frame_0_t;

/* txn_spill_tuple_key_pad_0 is tuple_key's pad: what it holds, while live. */
static void
txn_spill_tuple_key_pad_0(void *frame_)
{
	txn_spill_tuple_key_frame_0_t *frame = frame_;

	if (frame->values_live)
	{
		frame->values_live = false;
		txn_spill_versions_drop(&frame->values);
	}
	if (frame->path_live)
	{
		frame->path_live = false;
		std_vec_free_Checked(&frame->path.bytes, 1, 1);
	}
	if (frame->name_live)
	{
		frame->name_live = false;
		std_vec_free_Checked(&frame->name, 1, 1);
	}
	if (frame->dir_live)
	{
		frame->dir_live = false;
		std_vec_free_Checked(&frame->dir.bytes, 1, 1);
	}
	if (frame->files_live)
	{
		frame->files_live = false;
		std_vec_free_Checked(&frame->files, sizeof(TxnSpillReading), _Alignof(TxnSpillReading));
	}
}

/* txn_spill_tuple_key_body_0 is tuple_key's body. */
static void
txn_spill_tuple_key_body_0(void *frame_)
{
	txn_spill_tuple_key_frame_0_t *frame = frame_;
	HostSharedOp load = {.tag = HOST_SHARED_OP_LOAD};
	TxnStatePersistence persistence;
	uint8		key[8];

	*frame->some = false;
	if ((host_shared_Record_atomic(frame->state, TXN_STATE_SPILLS_PENDING_INDEX, load) == 0 &&
		 host_shared_Record_atomic(frame->state, TXN_STATE_SPILLS_INDEX, load) == 0) ||
		!txn_state_described(frame->state, &frame->storage, &persistence))
	{
		frame->ok = true;
		return;
	}
	txn_runs_spill_directory(frame->storage.database, txn_state_own(frame->state), &frame->dir);
	frame->dir_live = true;
	std_u64_to_be_bytes(frame->tid.f0, key);
	if (!txn_spill_files(&TXN_SPILL_STATE_HOME_OPS, frame->io, frame->state, &frame->storage,
						 TXN_RUNS_KIND_SPILLED_TUPLES, true, key, sizeof(key), &frame->files, frame->error))
	{
		txn_spill_tuple_key_pad_0(frame);
		return;
	}
	frame->files_live = true;
	for (size_t k = 0; k < frame->files.len; k++)
	{
		const TxnSpillReading *reading = &STD_VEC_AT(&frame->files, TxnSpillReading, k);
		TxnRunsNames names;
		txn_spill_looked_up_captures_t captures = {key, sizeof(key), &frame->values, &frame->values_live};

		if (!txn_spill_names(&TXN_SPILL_STATE_HOME_OPS, frame->state, &frame->storage, reading->kind, reading->xid,
							 &names, frame->error))
		{
			txn_spill_tuple_key_pad_0(frame);
			return;
		}
		txn_runs_spill_name(&names, reading->number, &frame->name);
		frame->name_live = true;
		std_path_join(frame->dir.bytes.ptr, frame->dir.bytes.len, (const char *) frame->name.ptr, frame->name.len,
					  &frame->path);
		frame->path_live = true;
		frame->name_live = false;
		std_vec_free_Checked(&frame->name, 1, 1);
		if (!txn_spill_reading(frame->io, &frame->path, &names, reading->has_len, reading->len,
							   txn_spill_looked_up_closure_0, &captures, frame->error))
		{
			txn_spill_tuple_key_pad_0(frame);
			return;
		}
		frame->values_live = true;
		frame->path_live = false;
		std_vec_free_Checked(&frame->path.bytes, 1, 1);
		if (frame->values.len > 0)
		{
			*frame->result = STD_VEC_AT(&frame->values, StdVec, 0);
			for (size_t v = 1; v < frame->values.len; v++)
				std_vec_free_Checked(&STD_VEC_AT(&frame->values, StdVec, v), 1, 1);
			frame->values_live = false;
			std_vec_free_Checked(&frame->values, sizeof(StdVec), _Alignof(StdVec));
			*frame->some = true;
			break;
		}
		frame->values_live = false;
		txn_spill_versions_drop(&frame->values);
	}
	frame->ok = true;
	txn_spill_tuple_key_pad_0(frame);
}

/*
 * txn_spill_tuple_key returns the key of a tid's row in a storage's spilled
 * tuple ids, the pending file's first, or nothing for a tid they don't hold,
 * read under a hold.
 */
bool
txn_spill_tuple_key(HostIoIo *io, const HostDeltaHold *hold, HostSharedRecord state, HostRow tid, bool *some,
					StdVec *result, HostError *error)
{
	txn_spill_tuple_key_frame_0_t frame = {.io = io, .state = state, .tid = tid, .some = some, .result = result,
	.error = error};

	std_landing(&frame, txn_spill_tuple_key_body_0, txn_spill_tuple_key_pad_0);
	return frame.ok;
}

/*
 * TxnSpillNaming is Naming: a spill file a checkpoint names, its xid, kind,
 * and number, its length and xxh3 when its manifest lists them, the point a
 * fold passes it at, or 0 for none, and its residual's point, or 0 for none.
 */
typedef struct TxnSpillNaming
{
	uint64		xid;
	TxnRunsKind kind;
	uint32		number;
	bool		has_read;
	uint64		len;
	uint64		hash;
	uint64		passed;
	uint64		from;
} TxnSpillNaming;

/* txn_spill_named_frame_0_t is named's state across its pad: the manifest's path and its writers' lock, the slots, the listed files, the namings, the aborted and crashed xids, the spills' directory, a file's path and file, and the named run files, while live. */
typedef struct txn_spill_named_frame_0_t
{
	const TxnSpillHomeOps *home;
	HostIoIo   *io;
	HostSharedRecord state;
	const HostStorage *storage;
	HostLsn		redo;
	StdVec	   *result;
	HostError  *error;
	bool		ok;
	bool		result_live;
	StdPathBuf	manifest;
	bool		manifest_live;
	HostSharedLongGuard writing;
	bool		writing_live;
	HostSharedLongIo long_io;
	StdVec		slotted;
	bool		slotted_live;
	/* listed is a Vec<File>, in the manifest's order. */
	StdVec		listed;
	bool		listed_live;
	/* files is a Vec<Naming>. */
	StdVec		files;
	bool		files_live;
	/* aborted is a Vec<u64> of xids. */
	StdVec		aborted;
	bool		aborted_live;

	/*
	 * crashed is a Vec<u64> of the xids of free space map spills a crash
	 * ended.
	 */
	StdVec		crashed;
	bool		crashed_live;
	StdPathBuf	dir;
	bool		dir_live;
	StdString	name;
	bool		name_live;
	StdPathBuf	path;
	bool		path_live;
	HostFile	file;
	bool		file_live;
} txn_spill_named_frame_0_t;

/* txn_spill_named_pad_0 is named's pad: what it holds, while live. */
static void
txn_spill_named_pad_0(void *frame_)
{
	txn_spill_named_frame_0_t *frame = frame_;

	if (frame->file_live)
	{
		frame->file_live = false;
		host_File_drop(&frame->file);
	}
	if (frame->path_live)
	{
		frame->path_live = false;
		std_vec_free_Checked(&frame->path.bytes, 1, 1);
	}
	if (frame->name_live)
	{
		frame->name_live = false;
		std_vec_free_Checked(&frame->name, 1, 1);
	}
	if (frame->dir_live)
	{
		frame->dir_live = false;
		std_vec_free_Checked(&frame->dir.bytes, 1, 1);
	}
	if (frame->crashed_live)
	{
		frame->crashed_live = false;
		std_vec_free_Checked(&frame->crashed, sizeof(uint64), _Alignof(uint64));
	}
	if (frame->aborted_live)
	{
		frame->aborted_live = false;
		std_vec_free_Checked(&frame->aborted, sizeof(uint64), _Alignof(uint64));
	}
	if (frame->files_live)
	{
		frame->files_live = false;
		std_vec_free_Checked(&frame->files, sizeof(TxnSpillNaming), _Alignof(TxnSpillNaming));
	}
	if (frame->listed_live)
	{
		frame->listed_live = false;
		std_vec_free_Checked(&frame->listed, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	}
	if (frame->slotted_live)
	{
		frame->slotted_live = false;
		std_vec_free_Checked(&frame->slotted, sizeof(txn_TupleU64KindU32OptionU32_t),
							 _Alignof(txn_TupleU64KindU32OptionU32_t));
	}
	if (frame->writing_live)
	{
		frame->writing_live = false;
		host_shared_LongGuard_drop(&frame->writing);
	}
	if (frame->manifest_live)
	{
		frame->manifest_live = false;
		std_vec_free_Checked(&frame->manifest.bytes, 1, 1);
	}
	if (frame->result_live)
	{
		frame->result_live = false;
		std_vec_free_Checked(frame->result, sizeof(RepoRunFile), _Alignof(RepoRunFile));
	}
}

/* txn_spill_named_push adds an element to a vector, refusing it when the vector can't grow. */
static bool
txn_spill_named_push(txn_spill_named_frame_0_t *frame, StdVec *vector, const void *elem, size_t size, size_t align)
{
	if (!std_vec_try_reserve(vector, 1, size, align))
	{
		*frame->error = host_Error_out_of_memory();
		return false;
	}
	std_vec_push_Checked(vector, elem, size, align);
	return true;
}

/*
 * txn_spill_named_listed gathers, under the manifest's writers' lock, the
 * namings of the listed files in the manifest's order, oldest-linked first,
 * passing over those a pending merge replaces and gathering the aborted: an
 * aborted transaction's files leave the naming but its free space map's,
 * and a free space map's spill of a transaction a crash ended, which no
 * abort record marked, is marked with the checkpoint's redo point.
 */
static bool
txn_spill_named_listed(txn_spill_named_frame_0_t *frame)
{
	for (size_t k = 0; k < frame->listed.len; k++)
	{
		const TxnSpillFile *file = &STD_VEC_AT(&frame->listed, TxnSpillFile, k);
		bool		map_free = file->kind == TXN_RUNS_KIND_SPILLED_FREE;
		TxnSpillNaming naming = {file->xid, file->kind, file->number, true, file->len, file->hash, file->passed,
		file->from};

		if (txn_spill_replaced_by(frame->slotted.ptr, frame->slotted.len, file))
			continue;
		if (!map_free || file->passed == 0)
		{
			HostXid		xid = {file->xid};
			HostStatus	status;

			if (!host_status(HOST_SHARED_LONG_IO_IO_DOORS(&frame->long_io), xid, &status, frame->error))
				return false;
			if (status == HOST_STATUS_ABORTED &&
				!txn_spill_named_push(frame, map_free ? &frame->crashed : &frame->aborted, &file->xid, sizeof(uint64),
									  _Alignof(uint64)))
				return false;
			if (status == HOST_STATUS_ABORTED && !map_free)
				continue;
		}
		if (map_free && txn_spill_has_xid(&frame->crashed, file->xid))
			naming.passed = frame->redo;
		if (!txn_spill_named_push(frame, &frame->files, &naming, sizeof(naming), _Alignof(TxnSpillNaming)))
			return false;
	}
	return true;
}

/* txn_spill_named_read reads a pending file's length and its trailer's xxh3 from the file. */
static bool
txn_spill_named_read(txn_spill_named_frame_0_t *frame, const TxnSpillNaming *naming, uint64 *len, uint64 *hash)
{
	TxnRunsNames names;
	ChunksErrorError failed;

	if (!txn_spill_names(frame->home, frame->state, frame->storage, naming->kind, naming->xid, &names, frame->error))
		return false;
	txn_runs_spill_name(&names, naming->number, &frame->name);
	frame->name_live = true;
	std_path_join(frame->dir.bytes.ptr, frame->dir.bytes.len, (const char *) frame->name.ptr, frame->name.len,
				  &frame->path);
	frame->path_live = true;
	frame->name_live = false;
	std_vec_free_Checked(&frame->name, 1, 1);
	if (!chunks_file_open_rw(HOST_IO_IO_IO_DOORS(frame->io), frame->path.bytes.ptr, frame->path.bytes.len, false,
							 &frame->file, &failed))
	{
		*frame->error = txn_base_storage(&failed);
		return false;
	}
	frame->file_live = true;
	if (!host_size(HOST_IO_IO_READ_DOORS(frame->io), &frame->file, len, frame->error) ||
		!txn_runs_spill_tail_hash(frame->io, &frame->file, *len, hash, frame->error))
		return false;
	frame->file_live = false;
	host_File_drop(&frame->file);
	frame->path_live = false;
	std_vec_free_Checked(&frame->path.bytes, 1, 1);
	return true;
}

/* txn_spill_named_body_0 is named's body. */
static void
txn_spill_named_body_0(void *frame_)
{
	txn_spill_named_frame_0_t *frame = frame_;

	std_vec_new(&frame->files, sizeof(TxnSpillNaming), _Alignof(TxnSpillNaming));
	frame->files_live = true;
	std_vec_new(&frame->aborted, sizeof(uint64), _Alignof(uint64));
	frame->aborted_live = true;
	std_vec_new(&frame->crashed, sizeof(uint64), _Alignof(uint64));
	frame->crashed_live = true;
	txn_spill_manifest(frame->home, frame->state, frame->storage, &frame->manifest);
	frame->manifest_live = true;
	if (!host_shared_Record_lock_long(frame->state, HOST_IO_IO_TAKES_SHORT(frame->io), frame->home->writing,
									  &frame->writing, frame->error))
	{
		txn_spill_named_pad_0(frame);
		return;
	}
	frame->writing_live = true;
	frame->long_io = host_shared_LongGuard_io(&frame->writing);
	if (!txn_spill_pended(frame->home, frame->state, frame->storage, &frame->slotted, frame->error))
	{
		txn_spill_named_pad_0(frame);
		return;
	}
	frame->slotted_live = true;
	if (!txn_spill_read(HOST_IO_IO_DOORS_READ_DOORS(HOST_SHARED_LONG_IO_IO_DOORS(&frame->long_io)),
						HOST_SHARED_LONG_IO_PAUSES(&frame->long_io), &frame->manifest, &frame->listed, frame->error))
	{
		txn_spill_named_pad_0(frame);
		return;
	}
	frame->listed_live = true;
	if (!txn_spill_named_listed(frame))
	{
		txn_spill_named_pad_0(frame);
		return;
	}
	if (frame->aborted.len > 0 || frame->crashed.len > 0)
	{
		size_t		kept = 0;

		for (size_t k = 0; k < frame->listed.len; k++)
		{
			TxnSpillFile file = STD_VEC_AT(&frame->listed, TxnSpillFile, k);
			bool		map_free = file.kind == TXN_RUNS_KIND_SPILLED_FREE;

			if (!map_free && txn_spill_has_xid(&frame->aborted, file.xid))
				continue;
			if (map_free && file.passed == 0 && txn_spill_has_xid(&frame->crashed, file.xid))
				file.passed = frame->redo;
			STD_VEC_AT(&frame->listed, TxnSpillFile, kept++) = file;
		}
		frame->listed.len = kept;
		if (!txn_spill_rewrote(frame->home, &frame->long_io, frame->state, &frame->manifest, &frame->listed,
							   frame->error))
		{
			txn_spill_named_pad_0(frame);
			return;
		}
	}
	frame->writing_live = false;
	host_shared_LongGuard_drop(&frame->writing);
	for (size_t k = 0; k < frame->slotted.len; k++)
	{
		const txn_TupleU64KindU32OptionU32_t *slot = &STD_VEC_AT(&frame->slotted, txn_TupleU64KindU32OptionU32_t, k);
		TxnSpillNaming naming = {slot->f0, slot->f1, slot->f2};
		bool		named = false;

		for (size_t j = 0; j < frame->files.len && !named; j++)
		{
			const TxnSpillNaming *found = &STD_VEC_AT(&frame->files, TxnSpillNaming, j);

			named = found->xid == slot->f0 && found->kind == slot->f1 && found->number == slot->f2;
		}
		if (!named && !txn_spill_named_push(frame, &frame->files, &naming, sizeof(naming), _Alignof(TxnSpillNaming)))
		{
			txn_spill_named_pad_0(frame);
			return;
		}
	}
	txn_runs_spill_directory(frame->storage->database, frame->home->own(frame->state), &frame->dir);
	frame->dir_live = true;
	std_vec_new(frame->result, sizeof(RepoRunFile), _Alignof(RepoRunFile));
	frame->result_live = true;
	if (!std_vec_try_reserve(frame->result, frame->files.len, sizeof(RepoRunFile), _Alignof(RepoRunFile)))
	{
		*frame->error = host_Error_out_of_memory();
		txn_spill_named_pad_0(frame);
		return;
	}
	for (size_t k = 0; k < frame->files.len; k++)
	{
		const TxnSpillNaming *naming = &STD_VEC_AT(&frame->files, TxnSpillNaming, k);
		RepoRunFile run = {.kind = (uint8) naming->kind, .tablespace = frame->storage->tablespace,
			.relation = frame->storage->relation, .len = naming->len, .hash = naming->hash, .has_spilled = true,
			.spilled = {naming->xid, naming->number, naming->passed != 0, naming->passed, naming->from != 0,
		naming->from}};

		if (!naming->has_read && !txn_spill_named_read(frame, naming, &run.len, &run.hash))
		{
			txn_spill_named_pad_0(frame);
			return;
		}
		std_vec_push_Checked(frame->result, &run, sizeof(run), _Alignof(RepoRunFile));
	}
	frame->result_live = false;
	frame->ok = true;
	txn_spill_named_pad_0(frame);
}

/*
 * txn_spill_named returns the manifest's entries of a storage's spill files
 * a checkpoint names, each synced at its write: the listed oldest-linked
 * first, then those processes' slots name. It reads the slots and then the
 * manifest under the manifest's writers' lock, so a merge pending its file
 * names that file and not the sources it replaces, and a carry that took the
 * lock before it logged its versions' records lists their files before this
 * naming reads. A listed spill of a transaction the commit log calls
 * aborted, which only redo leaves, leaves the manifest unnamed, for the
 * sweep, but a free space map's, which a fold takes; a free space map's
 * spill of a transaction a crash ended, which wrote no abort record to mark
 * it, is marked with the checkpoint's redo point, redo, the point a fold
 * passes it at.
 */
bool
txn_spill_named(const TxnSpillHomeOps *home, HostIoIo *io, HostSharedRecord state, const HostStorage *storage,
				HostLsn redo, StdVec *result, HostError *error)
{
	txn_spill_named_frame_0_t frame = {.home = home, .io = io, .state = state, .storage = storage, .redo = redo,
	.result = result, .error = error};

	std_landing(&frame, txn_spill_named_body_0, txn_spill_named_pad_0);
	return frame.ok;
}

/* txn_spill_settled_from_frame_0_t is settled_from's state across its pad: the spill's path, bytes, and decoding, its twins in the memtable, the hold and a key's chain, the tuple-id file's bytes and rows, and its tids, while live. */
typedef struct txn_spill_settled_from_frame_0_t
{
	const TxnSpillHomeOps *home;
	HostIoIo   *io;
	HostSharedRecord record;
	const TxnSpillPended *pended;
	HostError  *error;
	bool		ok;
	StdPathBuf	path;
	bool		path_live;
	StdString	name;
	bool		name_live;
	StdVec		bytes;
	bool		bytes_live;
	TxnRunsDecoded decoded;
	bool		decoded_live;
	/* twins is a Vec<(Vec<u8>, Vec<Handle>)>. */
	StdVec		twins;
	bool		twins_live;
	HostDeltaHold hold;
	bool		hold_live;
	/* chain is a Vec<Version>. */
	StdVec		chain;
	bool		chain_live;
	/* handles is a Vec<Handle> of a key's twins. */
	StdVec		handles;
	bool		handles_live;
	StdVec		tuple_bytes;
	bool		tuple_bytes_live;
	/* rows is decode_tuples' Vec<(u64, &[u8])>. */
	StdVec		rows;
	bool		rows_live;
	/* tids is a Vec<u64>. */
	StdVec		tids;
	bool		tids_live;
} txn_spill_settled_from_frame_0_t;

/* txn_spill_settled_from_pad_0 is settled_from's pad: what it holds, while live. */
static void
txn_spill_settled_from_pad_0(void *frame_)
{
	txn_spill_settled_from_frame_0_t *frame = frame_;

	if (frame->tids_live)
	{
		frame->tids_live = false;
		std_vec_free_Checked(&frame->tids, sizeof(uint64), _Alignof(uint64));
	}
	if (frame->rows_live)
	{
		frame->rows_live = false;
		std_vec_free_Checked(&frame->rows, sizeof(txn_TupleU64RefSliceU8_t), _Alignof(txn_TupleU64RefSliceU8_t));
	}
	if (frame->tuple_bytes_live)
	{
		frame->tuple_bytes_live = false;
		std_vec_free_Checked(&frame->tuple_bytes, 1, 1);
	}
	if (frame->handles_live)
	{
		frame->handles_live = false;
		std_vec_free_Checked(&frame->handles, sizeof(TxnMemtableVersionHandle), _Alignof(TxnMemtableVersionHandle));
	}
	if (frame->chain_live)
	{
		frame->chain_live = false;
		txn_memtable_version_versions_drop(&frame->chain, 0);
	}
	if (frame->hold_live)
	{
		frame->hold_live = false;
		host_delta_Hold_drop(&frame->hold);
	}
	if (frame->twins_live)
	{
		frame->twins_live = false;
		txn_spill_taken_drop(&frame->twins);
	}
	if (frame->decoded_live)
	{
		frame->decoded_live = false;
		txn_runs_Decoded_drop(&frame->decoded);
	}
	if (frame->bytes_live)
	{
		frame->bytes_live = false;
		std_vec_free_Checked(&frame->bytes, 1, 1);
	}
	if (frame->name_live)
	{
		frame->name_live = false;
		std_vec_free_Checked(&frame->name, 1, 1);
	}
	if (frame->path_live)
	{
		frame->path_live = false;
		std_vec_free_Checked(&frame->path.bytes, 1, 1);
	}
}

/* txn_spill_settled_from_read reads a spill's file of the pending names whole into a buffer, with its xxh3, its filter's bits, and its length. */
static bool
txn_spill_settled_from_read(txn_spill_settled_from_frame_0_t *frame, const TxnRunsNames *names, StdVec *bytes,
							bool *live, bool bits, TxnSpillFile *result)
{
	ChunksErrorError failed;

	txn_runs_spill_name(names, frame->pended->number, &frame->name);
	frame->name_live = true;
	if (frame->path_live)
		std_PathBuf_set_file_name(&frame->path, (const char *) frame->name.ptr, frame->name.len);
	else
	{
		txn_runs_spill_directory(frame->pended->storage.database, frame->home->own(frame->record), &frame->path);
		frame->path_live = true;
		{
			StdPathBuf	joined;

			std_path_join(frame->path.bytes.ptr, frame->path.bytes.len, (const char *) frame->name.ptr,
						  frame->name.len, &joined);
			std_vec_free_Checked(&frame->path.bytes, 1, 1);
			frame->path = joined;
		}
	}
	frame->name_live = false;
	std_vec_free_Checked(&frame->name, 1, 1);
	if (!chunks_file_read_file(HOST_IO_IO_READ_DOORS(frame->io), frame->path.bytes.ptr, frame->path.bytes.len, bytes,
							   &failed))
	{
		*frame->error = txn_base_storage(&failed);
		return false;
	}
	*live = true;
	*result = (TxnSpillFile)
	{
		.xid = frame->pended->xid, .kind = names->kind, .number = frame->pended->number, .len = (uint64) bytes->len
	};
	return txn_runs_spill_hash(bytes->ptr, bytes->len, &result->hash, frame->error) &&
		(!bits || txn_runs_spill_bits(bytes->ptr, bytes->len, &result->bits, frame->error));
}

/* txn_spill_settled_from_twins gathers each version of a key the file holds whose record the memtable's chain holds too, by its bytes, and takes them out. */
static bool
txn_spill_settled_from_twins(txn_spill_settled_from_frame_0_t *frame, TxnWrittenOf of)
{
	std_vec_new(&frame->twins, sizeof(txn_TupleVecU8VecHandle_t), _Alignof(txn_TupleVecU8VecHandle_t));
	frame->twins_live = true;
	if (!host_delta_hold(frame->io, &frame->hold, frame->error))
		return false;
	frame->hold_live = true;
	for (size_t k = 0; k < frame->decoded.keys.len; k++)
	{
		const TxnRunsKey *key = &STD_VEC_AT(&frame->decoded.keys, TxnRunsKey, k);
		txn_TupleVecU8VecHandle_t twin;

		if (!txn_spill_Home_chain(frame->home, frame->io, &frame->hold, frame->record, of, key->key.ptr, key->key.len,
								  &frame->chain, frame->error))
			return false;
		frame->chain_live = true;
		std_vec_new(&frame->handles, sizeof(TxnMemtableVersionHandle), _Alignof(TxnMemtableVersionHandle));
		frame->handles_live = true;
		for (size_t c = 0; c < frame->chain.len; c++)
		{
			const TxnMemtableVersionVersion *version = &STD_VEC_AT(&frame->chain, TxnMemtableVersionVersion, c);
			TxnMemtableVersionRecord record = txn_memtable_version_Version_record(version);
			StdBytes	bytes = txn_memtable_version_Record_bytes(&record);
			bool		twinned = false;

			for (size_t v = 0; v < key->versions.len && !twinned; v++)
			{
				StdBytes	kept = txn_memtable_version_Record_bytes(&STD_VEC_AT(&key->versions,
																				 TxnMemtableVersionRecord, v));

				twinned = kept.len == bytes.len && memcmp(kept.ptr, bytes.ptr, bytes.len) == 0;
			}
			if (twinned)
				std_vec_push_Checked(&frame->handles, &version->handle, sizeof(version->handle),
									 _Alignof(TxnMemtableVersionHandle));
		}
		frame->chain_live = false;
		txn_memtable_version_versions_drop(&frame->chain, 0);
		if (frame->handles.len == 0)
		{
			frame->handles_live = false;
			std_vec_free_Checked(&frame->handles, sizeof(TxnMemtableVersionHandle),
								 _Alignof(TxnMemtableVersionHandle));
			continue;
		}
		if (!std_vec_try_reserve(&frame->twins, 1, sizeof(twin), _Alignof(txn_TupleVecU8VecHandle_t)))
		{
			*frame->error = host_Error_out_of_memory();
			return false;
		}
		std_slice_to_vec_Checked(&twin.f0, key->key.ptr, key->key.len, 1, 1);
		twin.f1 = frame->handles;
		frame->handles_live = false;
		std_vec_push_Checked(&frame->twins, &twin, sizeof(twin), _Alignof(txn_TupleVecU8VecHandle_t));
	}
	frame->hold_live = false;
	host_delta_Hold_drop(&frame->hold);
	return txn_spill_removed_keys(frame->home, frame->io, frame->record, of, &frame->twins, frame->error);
}

/* txn_spill_settled_from_body_0 is settled_from's body. */
static void
txn_spill_settled_from_body_0(void *frame_)
{
	txn_spill_settled_from_frame_0_t *frame = frame_;
	const TxnSpillPended *pended = frame->pended;
	TxnRunsNames names;
	TxnSpillFile listing[2] = {0};
	size_t		listing_len = 1;
	TxnWrittenOf of;
	txn_TupleU64KindU32OptionU32_t replacing = {pended->xid, pended->kind, pended->number, true, pended->sources};

	if (!txn_spill_names(frame->home, frame->record, &pended->storage, pended->kind, pended->xid, &names,
						 frame->error) ||
		!txn_spill_settled_from_read(frame, &names, &frame->bytes, &frame->bytes_live, true, &listing[0]) ||
		!txn_runs_decode(frame->bytes.ptr, frame->bytes.len, &names, listing[0].hash, &frame->decoded, frame->error))
	{
		txn_spill_settled_from_pad_0(frame);
		return;
	}
	frame->decoded_live = true;
	switch (pended->kind)
	{
		case TXN_RUNS_KIND_SPILLED_ROWS:
		case TXN_RUNS_KIND_SPILLED_ENTRIES:
			of = TXN_WRITTEN_OF_ROWS;
			break;
		case TXN_RUNS_KIND_SPILLED_VALUES:
			of = TXN_WRITTEN_OF_VALUES;
			break;
		case TXN_RUNS_KIND_SPILLED_FREE:
			of = TXN_WRITTEN_OF_FREE;
			break;
		case TXN_RUNS_KIND_SPILLED_CHUNKS:
			of = TXN_WRITTEN_OF_CHUNKS;
			break;
		default:
			*frame->error = txn_spill_internal("a process's slot names a spill of no tree");
			txn_spill_settled_from_pad_0(frame);
			return;
	}
	if (!pended->has_sources && !txn_spill_settled_from_twins(frame, of))
	{
		txn_spill_settled_from_pad_0(frame);
		return;
	}
	if (pended->tupled)
	{
		TxnRunsNames tuple_names;

		if (!txn_spill_names(frame->home, frame->record, &pended->storage, TXN_RUNS_KIND_SPILLED_TUPLES, pended->xid,
							 &tuple_names, frame->error) ||
			!txn_spill_settled_from_read(frame, &tuple_names, &frame->tuple_bytes, &frame->tuple_bytes_live, false,
										 &listing[1]))
		{
			txn_spill_settled_from_pad_0(frame);
			return;
		}
		if (!pended->has_sources)
		{
			if (!txn_runs_decode_tuples(frame->tuple_bytes.ptr, frame->tuple_bytes.len, &tuple_names, listing[1].hash,
										&frame->rows, frame->error))
			{
				txn_spill_settled_from_pad_0(frame);
				return;
			}
			frame->rows_live = true;
			std_vec_with_capacity_Checked(&frame->tids, frame->rows.len, sizeof(uint64), _Alignof(uint64));
			frame->tids_live = true;
			for (size_t k = 0; k < frame->rows.len; k++)
				std_vec_push_Checked(&frame->tids, &STD_VEC_AT(&frame->rows, txn_TupleU64RefSliceU8_t, k).f0,
									 sizeof(uint64), _Alignof(uint64));
			if (!frame->home->untupled(frame->io, frame->record, frame->tids.ptr, frame->tids.len, frame->error))
			{
				txn_spill_settled_from_pad_0(frame);
				return;
			}
		}
		if (!txn_runs_spill_bits(frame->tuple_bytes.ptr, frame->tuple_bytes.len, &listing[1].bits, frame->error))
		{
			txn_spill_settled_from_pad_0(frame);
			return;
		}
		listing_len = 2;
	}
	frame->ok = txn_spill_linked(frame->home, frame->io, frame->record, &pended->storage, listing, listing_len,
								 pended->has_sources, replacing, frame->error) &&
		txn_spill_settled(frame->home, frame->io, &frame->record, frame->error);
	txn_spill_settled_from_pad_0(frame);
}

/* txn_spill_settled_from lists a pending spill in its home's manifest as settle_pending does. */
static bool
txn_spill_settled_from(const TxnSpillHomeOps *home, HostIoIo *io, HostSharedRecord record,
					   const TxnSpillPended *pended, HostError *error)
{
	txn_spill_settled_from_frame_0_t frame = {.home = home, .io = io, .record = record, .pended = pended,
	.error = error};

	std_landing(&frame, txn_spill_settled_from_body_0, txn_spill_settled_from_pad_0);
	return frame.ok;
}

/* txn_spill_settle_pending_captures_t is settle_pending's step's captures: the backend's I/O and its pending spill. */
typedef struct txn_spill_settle_pending_captures_t
{
	HostIoIo   *io;
	const TxnSpillPended *pended;
} txn_spill_settle_pending_captures_t;

/* txn_spill_settle_pending_closure_0 is settle_pending's step: the pending spill listed in its home's manifest. */
static bool
txn_spill_settle_pending_closure_0(void *captures, const TxnSpillHomeOps *home, const HostSharedHeld *held,
								   HostError *error)
{
	txn_spill_settle_pending_captures_t *c = captures;

	return txn_spill_settled_from(home, c->io, host_shared_Held_record(held), c->pended, error);
}

/*
 * txn_spill_settle_pending lists the spill the process's slot names, read
 * back from its file, and clears the slot; a slot whose storage has no state
 * any more is cleared alone. It first takes out of the memtable each version
 * of a key the file holds whose record is one the file holds, by its bytes,
 * and drops the tids of the spill's tuple-id file, then lists the spill's
 * tuple-id file after its row file, and a merge's files in place of those it
 * replaces. A bootstrap process has no slot.
 */
bool
txn_spill_settle_pending(HostIoIo *io, HostError *error)
{
	bool		some;
	TxnSpillPended pended;
	txn_spill_settle_pending_captures_t captures = {io, &pended};

	if (!host_shared_ready())
		return true;
	if (!txn_spill_pending(io, &some, &pended, error))
		return false;
	if (!some)
		return true;
	if (!txn_spill_at_found(io, &pended.storage, txn_spill_settle_pending_closure_0, &captures, &some, error))
		return false;
	return some || txn_spill_settled(&TXN_SPILL_STATE_HOME_OPS, io, NULL, error);
}

#ifdef DOLTGRES_TEST_TXN
#include "doltgres/simc/simc.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/test.h"
#include "doltgres/txn/memtable/testing.h"

/* TXN_SPILL_TESTS_KIND_NAMES are Kind's variants' names, at their numbers, for its derived Debug. */
static const char *const TXN_SPILL_TESTS_KIND_NAMES[] = {"Rows", "Free", "Values", "Chunks", "Status", "SpilledRows",
"SpilledValues", "SpilledTuples", "Entries", "SpilledEntries", "Identity", "SpilledFree", "SpilledChunks"};

/* txn_spill_tests_Kind_Debug_fmt is Kind's derived Debug, its variant's name, for the tests' asserts. */
static bool
txn_spill_tests_Kind_Debug_fmt(const void *value, StdFormatter *f)
{
	const char *name = TXN_SPILL_TESTS_KIND_NAMES[*(const TxnRunsKind *) value];

	return std_fmt_write_str(f, name, strlen(name));
}

/* txn_spill_tests_File_Debug_fmt is File's derived Debug, File { xid, kind, number, len, hash, bits }, for the tests' asserts. */
static bool
txn_spill_tests_File_Debug_fmt(const void *value, StdFormatter *f)
{
	const TxnSpillFile *file = value;
	StdDebugStruct fields = std_fmt_debug_struct(f, "File", 4);

	std_DebugStruct_field(&fields, "xid", 3, &file->xid, std_u64_Debug_fmt);
	std_DebugStruct_field(&fields, "kind", 4, &file->kind, txn_spill_tests_Kind_Debug_fmt);
	std_DebugStruct_field(&fields, "number", 6, &file->number, std_u32_Debug_fmt);
	std_DebugStruct_field(&fields, "len", 3, &file->len, std_u64_Debug_fmt);
	std_DebugStruct_field(&fields, "hash", 4, &file->hash, std_u64_Debug_fmt);
	std_DebugStruct_field(&fields, "bits", 4, &file->bits, std_u64_Debug_fmt);
	return std_DebugStruct_finish(&fields);
}

/* txn_spill_tests_files_Debug_fmt is a Vec<File>'s Debug. */
static bool
txn_spill_tests_files_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *files = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t k = 0; k < files->len; k++)
		std_DebugList_entry(&list, &STD_VEC_AT(files, TxnSpillFile, k), txn_spill_tests_File_Debug_fmt);
	return std_DebugList_finish(&list);
}

/* txn_spill_tests_files_are asserts at its place that a Vec<File> is a list of files, and frees the Vec. */
static void
txn_spill_tests_files_are(uint32 line, uint32 col, StdVec *files, const TxnSpillFile *wanted, size_t wanted_len)
{
	StdVec		wanted_vec = {(void *) wanted, wanted_len, wanted_len};
	bool		eq = files->len == wanted_len;

	for (size_t k = 0; eq && k < wanted_len; k++)
		eq = txn_spill_File_PartialEq_eq(&STD_VEC_AT(files, TxnSpillFile, k), &wanted[k]);
	STD_ASSERT_EQ(line, col, eq, files, txn_spill_tests_files_Debug_fmt, &wanted_vec, txn_spill_tests_files_Debug_fmt);
	std_vec_free_Checked(files, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
}

/* txn_spill_tests_Slotted_Debug_fmt is Slotted's Debug, the tuple (xid, kind, number, sources). */
static bool
txn_spill_tests_Slotted_Debug_fmt(const void *value, StdFormatter *f)
{
	const txn_TupleU64KindU32OptionU32_t *slotted = value;
	StdOptionRef sources = {slotted->has_f3, STD_ITEM(&slotted->f3, std_u32_Debug_fmt)};
	StdTupleRef tuple = STD_TUPLE(4, STD_ITEM(&slotted->f0, std_u64_Debug_fmt),
								  STD_ITEM(&slotted->f1, txn_spill_tests_Kind_Debug_fmt),
								  STD_ITEM(&slotted->f2, std_u32_Debug_fmt),
								  STD_ITEM(&sources, std_option_Debug_fmt));

	return std_tuple_Debug_fmt(&tuple, f);
}

/* txn_spill_tests_slotteds_Debug_fmt is a Vec<Slotted>'s Debug. */
static bool
txn_spill_tests_slotteds_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *slotteds = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t k = 0; k < slotteds->len; k++)
		std_DebugList_entry(&list, &STD_VEC_AT(slotteds, txn_TupleU64KindU32OptionU32_t, k),
							txn_spill_tests_Slotted_Debug_fmt);
	return std_DebugList_finish(&list);
}

/* txn_spill_tests_Pended_Debug_fmt is Pended's Debug, the tuple (storage, xid, kind, number, tupled, sources). */
static bool
txn_spill_tests_Pended_Debug_fmt(const void *value, StdFormatter *f)
{
	const TxnSpillPended *pended = value;
	StdOptionRef sources = {pended->has_sources, STD_ITEM(&pended->sources, std_u32_Debug_fmt)};
	StdTupleRef tuple = STD_TUPLE(6, STD_ITEM(&pended->storage, host_Storage_Debug_fmt),
								  STD_ITEM(&pended->xid, std_u64_Debug_fmt),
								  STD_ITEM(&pended->kind, txn_spill_tests_Kind_Debug_fmt),
								  STD_ITEM(&pended->number, std_u32_Debug_fmt),
								  STD_ITEM(&pended->tupled, std_bool_Debug_fmt),
								  STD_ITEM(&sources, std_option_Debug_fmt));

	return std_tuple_Debug_fmt(&tuple, f);
}

/* txn_spill_tests_file returns a test's spill file of rows by a transaction, of a number, which no file on disk holds. */
static TxnSpillFile
txn_spill_tests_file(uint64 xid, uint32 number)
{
	return (TxnSpillFile)
	{
		.xid = xid, .kind = TXN_RUNS_KIND_SPILLED_ROWS, .number = number, .len = 100 + (uint64) number,
			.hash = xid ^ 0xfeed, .bits = 64
	};
}

/* txn_spill_tests_files_of returns the files a state's storage's manifest lists, newest first. */
static StdVec
txn_spill_tests_files_of(HostIoIo *io, HostSharedRecord state)
{
	HostStorage storage;
	TxnStatePersistence persistence;
	txn_TupleVecFileU64_t listed;
	HostError	error;

	if (!txn_state_described(state, &storage, &persistence))
		STD_OPTION_EXPECT_FAILED(1854, 52, "the state is filled");
	if (!txn_spill_listed(&TXN_SPILL_STATE_HOME_OPS, io, state, &storage, &listed, &error))
		STD_RESULT_EXPECT_FAILED(1855, 37, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	return listed.f0;
}

/*
 * txn_spill_tests_written writes a test's spill file of rows of a storage's
 * state by a transaction, of a number, holding its keys with no versions, and
 * its filter, and returns it with its path.
 */
static TxnSpillFile
txn_spill_tests_written(HostIoIo *io, HostSharedRecord state, uint64 xid, uint32 number, const StdBytes *keys,
						size_t keys_len, StdPathBuf *path)
{
	TxnRunsTail tail = {.tag = TXN_RUNS_TAIL_NONE};
	HostStorage storage;
	TxnStatePersistence persistence;
	StdPathBuf	dir;
	StdStr		made[2];
	ChunksErrorError failed;
	TxnRunsNames names;
	TxnRunsWriting *writing;
	TxnSpillFilter filter;
	HostError	error;
	RepoRunFile run;
	StdString	name;
	TxnSpillFile file;
	bool		some;

	if (!txn_state_described(state, &storage, &persistence))
		STD_OPTION_EXPECT_FAILED(1866, 52, "the state is filled");
	txn_runs_spill_directory(storage.database, txn_state_own(state), &dir);
	if (!std_path_parent(dir.bytes.ptr, dir.bytes.len, &made[0]))
		STD_OPTION_UNWRAP_FAILED(1868, 35);
	made[1] = (StdStr)
	{
		dir.bytes.ptr, dir.bytes.len
	};
	for (size_t k = 0; k < lengthof(made); k++)
		if (!chunks_file_create_dir(HOST_IO_IO_IO_DOORS(io), made[k].ptr, made[k].len, &failed))
			STD_RESULT_EXPECT_FAILED(1869, 48, "called `Result::unwrap()` on an `Err` value", &failed,
									 chunks_error_Error_Debug_fmt);
	if (!txn_spill_names(&TXN_SPILL_STATE_HOME_OPS, state, &storage, TXN_RUNS_KIND_SPILLED_ROWS, xid, &names, &error))
		STD_RESULT_EXPECT_FAILED(1871, 68, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	writing = txn_runs_Writing_spill(dir.bytes.ptr, dir.bytes.len, &names, tail, number);
	for (size_t k = 0; k < keys_len; k++)
		if (!txn_runs_Writing_key(writing, HOST_IO_IO_IO_DOORS(io), keys[k].ptr, keys[k].len, NULL, 0, &error))
			STD_RESULT_EXPECT_FAILED(1874, 39, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
	if (!txn_spill_Filter_of(keys_len, keys, keys_len, &filter, &error))
		STD_RESULT_EXPECT_FAILED(1876, 67, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!txn_runs_Writing_end(writing, HOST_IO_IO_IO_DOORS(io), filter.words.ptr, filter.words.len, &some, &run, &error))
		STD_RESULT_EXPECT_FAILED(1877, 50, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!some)
		STD_OPTION_UNWRAP_FAILED(1877, 59);
	txn_runs_Writing_drop(writing);
	file = (TxnSpillFile)
	{
		.xid = xid, .kind = TXN_RUNS_KIND_SPILLED_ROWS, .number = number, .len = run.len,
			.hash = run.hash, .bits = txn_spill_Filter_bits(&filter)
	};
	txn_runs_spill_name(&names, number, &name);
	std_path_join(dir.bytes.ptr, dir.bytes.len, name.ptr, name.len, path);
	std_String_drop(&name);
	std_vec_free_Checked(&filter.words, sizeof(uint64), _Alignof(uint64));
	std_vec_free_Checked(&dir.bytes, 1, 1);
	return file;
}

/* txn_spill_tests_of_742 is the list, hit, and removal case's first removal's pick: transaction 742's files. */
static bool
txn_spill_tests_of_742(void *captures, const TxnSpillFile *file)
{
	return file->xid == 742;
}

/* txn_spill_tests_every is the list, hit, and removal case's second removal's pick: every file. */
static bool
txn_spill_tests_every(void *captures, const TxnSpillFile *file)
{
	return true;
}

/*
 * Bugs row 143's manifest: listed spills list newest first with what names
 * their files, each file's filter, read from its own pages, hits its own keys
 * and misses most others, and a removal takes exactly the files it picks out
 * of the manifest, the delta's used bytes unmoved by any of it.
 */
void
test_txn_spill_tests_a_storages_spills_list_hit_and_remove(void)
{
	static const StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(" misses")};
	static const StdBytes first[] = {{(const uint8 *) "a", 1}, {(const uint8 *) "b", 1}};
	static const StdBytes second[] = {{(const uint8 *) "c", 1}};
	static const StdBytes third[] = {{(const uint8 *) "d", 1}};
	static const StdBytes *const keys[] = {first, second, third};
	static const size_t keys_lens[] = {lengthof(first), lengthof(second), lengthof(third)};
	static const uint64 xids[] = {742, 742, 800};
	static const uint32 numbers[] = {0, 1, 0};
	txn_TupleU64KindU32OptionU32_t no_replacing = {0};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9741);
	HostSharedRecord state = host_shared_Held_record(&held);
	HostStorage storage = txn_memtable_testing_storage(9741);
	uint64		used = host_delta_used();
	uint64		now;
	TxnSpillFile spills[3];
	StdPathBuf	paths[3];
	TxnSpillFile wanted[3];
	StdVec		files;
	StdVec		gone;
	StdPathBuf	manifest;
	HostError	error;
	HostFile	opened;
	bool		there;
	bool		hit;

	for (size_t k = 0; k < lengthof(spills); k++)
		spills[k] = txn_spill_tests_written(io, state, xids[k], numbers[k], keys[k], keys_lens[k], &paths[k]);
	for (size_t k = 0; k < lengthof(spills); k++)
		if (!txn_spill_linked(&TXN_SPILL_STATE_HOME_OPS, io, state, &storage, &spills[k], 1, false, no_replacing,
							  &error))
			STD_RESULT_EXPECT_FAILED(1899, 63, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
	now = host_delta_used();
	STD_ASSERT_EQ_MSG(1901, 9, now == used, &now, std_u64_Debug_fmt, &used, std_u64_Debug_fmt,
					  "the manifest takes none of the delta");
	files = txn_spill_tests_files_of(io, state);
	wanted[0] = spills[2];
	wanted[1] = spills[1];
	wanted[2] = spills[0];
	txn_spill_tests_files_are(1902, 9, &files, wanted, 3);
	for (size_t k = 0; k < lengthof(spills); k++)
	{
		size_t		misses = 0;

		hit = true;
		for (size_t j = 0; hit && j < keys_lens[k]; j++)
			if (!txn_spill_hits(io, &paths[k], &spills[k], keys[k][j].ptr, keys[k][j].len, &hit, &error))
				STD_RESULT_EXPECT_FAILED(1904, 71, "called `Result::unwrap()` on an `Err` value", &error,
										 host_Error_Debug_fmt);
		STD_ASSERT(1904, 13, hit, "keys.iter().all(|key| hits(io, (path, file), key).unwrap())");
		for (uint32 other = 0; other < 100; other++)
		{
			uint8		key[4];

			std_u32_to_be_bytes(other, key);
			if (!txn_spill_hits(io, &paths[k], &spills[k], key, sizeof(key), &hit, &error))
				STD_RESULT_EXPECT_FAILED(1905, 99, "called `Result::unwrap()` on an `Err` value", &error,
										 host_Error_Debug_fmt);
			if (!hit)
				misses++;
		}
		if (misses < 90)
		{
			StdFmtArg	args[1] = {{&misses, std_usize_Display_fmt}};
			StdFmtArguments message = {pieces, lengthof(pieces), args};

			std_panic_fmt(RUST_FILE, 1906, 13, &message);
		}
	}
	if (!txn_spill_removed(&TXN_SPILL_STATE_HOME_OPS, io, state, &storage, txn_spill_tests_of_742, NULL, &gone,
						   &error))
		STD_RESULT_EXPECT_FAILED(1908, 79, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	wanted[0] = spills[0];
	wanted[1] = spills[1];
	txn_spill_tests_files_are(1909, 9, &gone, wanted, 2);
	files = txn_spill_tests_files_of(io, state);
	txn_spill_tests_files_are(1910, 9, &files, &spills[2], 1);
	if (!txn_spill_removed(&TXN_SPILL_STATE_HOME_OPS, io, state, &storage, txn_spill_tests_every, NULL, &gone,
						   &error))
		STD_RESULT_EXPECT_FAILED(1911, 54, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	std_vec_free_Checked(&gone, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	files = txn_spill_tests_files_of(io, state);
	STD_ASSERT(1912, 9, files.len == 0, "files_of(io, state).is_empty()");
	std_vec_free_Checked(&files, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	txn_spill_manifest(&TXN_SPILL_STATE_HOME_OPS, state, &storage, &manifest);
	if (!host_open_if_there(HOST_IO_IO_READ_DOORS(io), manifest.bytes.ptr, manifest.bytes.len, &there, &opened,
							&error))
		STD_RESULT_EXPECT_FAILED(1913, 75, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (there)
		host_File_drop(&opened);
	std_vec_free_Checked(&manifest.bytes, 1, 1);
	STD_ASSERT(1913, 9, !there, "host::open_if_there(io, &manifest(state, &storage(9741))).unwrap().is_none()");
	for (size_t k = 0; k < lengthof(paths); k++)
		std_vec_free_Checked(&paths[k].bytes, 1, 1);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/* txn_spill_tests_pended_are asserts at its line that a storage's pended spills are only a wanted one, or are none with an assert!'s message. */
static void
txn_spill_tests_pended_are(uint32 line, uint32 unwrap_col, HostSharedRecord state, const HostStorage *storage,
						   const txn_TupleU64KindU32OptionU32_t *wanted, const char *empty)
{
	StdVec		wanted_vec = {(void *) wanted, wanted != NULL, wanted != NULL};
	StdVec		pended;
	HostError	error;
	bool		eq;

	if (!txn_spill_pended(&TXN_SPILL_STATE_HOME_OPS, state, storage, &pended, &error))
		STD_RESULT_EXPECT_FAILED(line, unwrap_col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	eq = pended.len == wanted_vec.len;
	if (eq && wanted != NULL)
	{
		const txn_TupleU64KindU32OptionU32_t *slotted = pended.ptr;

		eq = slotted->f0 == wanted->f0 && slotted->f1 == wanted->f1 && slotted->f2 == wanted->f2 &&
			slotted->has_f3 == wanted->has_f3 && (!slotted->has_f3 || slotted->f3 == wanted->f3);
	}
	if (!eq && wanted == NULL)
		std_panic(RUST_FILE, line, 9, empty, strlen(empty));
	STD_ASSERT_EQ(line, 9, eq, &pended, txn_spill_tests_slotteds_Debug_fmt, &wanted_vec,
				  txn_spill_tests_slotteds_Debug_fmt);
	std_vec_free_Checked(&pended, sizeof(txn_TupleU64KindU32OptionU32_t), _Alignof(txn_TupleU64KindU32OptionU32_t));
}

/* txn_spill_tests_pending_is asserts at its place that the process's slot names a pended spill, or none. */
static void
txn_spill_tests_pending_is(uint32 line, HostIoIo *io, const TxnSpillPended *wanted)
{
	TxnSpillPended pended;
	StdOptionRef left;
	StdOptionRef right = {wanted != NULL, STD_ITEM(wanted, txn_spill_tests_Pended_Debug_fmt)};
	HostError	error;
	bool		some;
	bool		eq;

	if (!txn_spill_pending(io, &some, &pended, &error))
		STD_RESULT_EXPECT_FAILED(line, 32, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	left = (StdOptionRef)
	{
		some, STD_ITEM(&pended, txn_spill_tests_Pended_Debug_fmt)
	};
	eq = some == (wanted != NULL);
	if (eq && some)
		eq = host_Storage_PartialEq_eq(&pended.storage, &wanted->storage) && pended.xid == wanted->xid &&
			pended.kind == wanted->kind && pended.number == wanted->number && pended.tupled == wanted->tupled &&
			pended.has_sources == wanted->has_sources && (!pended.has_sources || pended.sources == wanted->sources);
	STD_ASSERT_EQ(line, 9, eq, &left, std_option_Debug_fmt, &right, std_option_Debug_fmt);
}

/*
 * Bugs row 74's pending slot: a process's pending spill is found by a reader
 * of its storage, not of another, while it is pending, and a second pend
 * before it settles is refused.
 */
void
test_txn_spill_tests_a_pending_spill_is_found_by_its_storages_readers_until_it_settles(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9742);
	HostSharedRecord state = host_shared_Held_record(&held);
	HostStorage storage = txn_memtable_testing_storage(9742);
	HostStorage other = storage;
	txn_TupleU64KindU32OptionU32_t slotted = {.f0 = 742, .f1 = TXN_RUNS_KIND_SPILLED_ROWS, .f2 = 3};
	TxnSpillPended pended = {.storage = storage, .xid = 742, .kind = TXN_RUNS_KIND_SPILLED_ROWS, .number = 3};
	TxnSpillFile file;
	HostError	error;
	bool		refused;

	other.relation = 16385;
	txn_spill_tests_pended_are(1925, 41, state, &storage, NULL, "assertion failed: pended(state, &storage).unwrap().is_empty()");
	file = txn_spill_tests_file(742, 3);
	if (!txn_spill_pend(&TXN_SPILL_STATE_HOME_OPS, io, state, &storage, &file, false, false, 0, &error))
		STD_RESULT_EXPECT_FAILED(1926, 65, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	txn_spill_tests_pended_are(1927, 44, state, &storage, &slotted, NULL);
	txn_spill_tests_pended_are(1928, 39, state, &other, NULL,
							   "assertion failed: pended(state, &other).unwrap().is_empty()");
	txn_spill_tests_pending_is(1929, io, &pended);
	file = txn_spill_tests_file(742, 4);
	refused = !txn_spill_pend(&TXN_SPILL_STATE_HOME_OPS, io, state, &storage, &file, false, false, 0, &error);
	if (refused)
		host_Error_drop(&error);
	STD_ASSERT(1930, 9, refused, "pend(io, state, &storage, (&file(742, 4), false), None).is_err()");
	if (!txn_spill_settled(&TXN_SPILL_STATE_HOME_OPS, io, &state, &error))
		STD_RESULT_EXPECT_FAILED(1931, 34, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	txn_spill_tests_pended_are(1932, 41, state, &storage, NULL, "assertion failed: pended(state, &storage).unwrap().is_empty()");
	txn_spill_tests_pending_is(1933, io, NULL);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/* txn_spill_tests_numbered returns the four-byte big-endian keys of a count of numbers from zero, and their slices. */
static void
txn_spill_tests_numbered(uint32 count, StdVec *bytes, StdVec *keys)
{
	std_vec_from_elem_zeroed(bytes, count, 4, 1);
	std_vec_from_elem_zeroed(keys, count, sizeof(StdBytes), _Alignof(StdBytes));
	for (uint32 key = 0; key < count; key++)
	{
		uint8	   *at = (uint8 *) bytes->ptr + 4 * (size_t) key;

		std_u32_to_be_bytes(key, at);
		STD_VEC_AT(keys, StdBytes, key) = (StdBytes)
		{
			at, 4
		};
	}
}

/*
 * Bugs row 74's filter: every key a filter was built of hits it, and of a
 * thousand other keys about one in a hundred does, at most three in a
 * hundred.
 */
void
test_txn_spill_tests_a_filter_hits_its_keys_and_few_others(void)
{
	static const StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(" false hits")};
	StdVec		bytes;
	StdVec		keys;
	TxnSpillFilter filter;
	HostError	error;
	uint64		bits;
	uint64		wanted = 10048;
	size_t		false_hits = 0;
	bool		all = true;

	txn_spill_tests_numbered(1000, &bytes, &keys);
	if (!txn_spill_Filter_of(keys.len, keys.ptr, keys.len, &filter, &error))
		STD_RESULT_EXPECT_FAILED(1941, 78, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	bits = txn_spill_Filter_bits(&filter);
	STD_ASSERT_EQ(1942, 9, bits == wanted, &bits, std_u64_Debug_fmt, &wanted, std_u64_Debug_fmt);
	for (size_t k = 0; all && k < keys.len; k++)
		all = txn_spill_Filter_hits(&filter, STD_VEC_AT(&keys, StdBytes, k).ptr, 4);
	STD_ASSERT(1943, 9, all, "keys.iter().all(|key| filter.hits(key))");
	for (uint32 key = 1000; key < 2000; key++)
	{
		uint8		be[4];

		std_u32_to_be_bytes(key, be);
		if (txn_spill_Filter_hits(&filter, be, sizeof(be)))
			false_hits++;
	}
	if (false_hits > 30)
	{
		StdFmtArg	args[1] = {{&false_hits, std_usize_Display_fmt}};
		StdFmtArguments message = {pieces, lengthof(pieces), args};

		std_panic_fmt(RUST_FILE, 1945, 9, &message);
	}
	std_vec_free_Checked(&filter.words, sizeof(uint64), _Alignof(uint64));
	std_vec_free_Checked(&keys, sizeof(StdBytes), _Alignof(StdBytes));
	std_vec_free_Checked(&bytes, 4, 1);
}

/*
 * Bugs row 143's filter pages: a file's filter past a page's bits is written
 * in several pages, each of the most words but the last, which the file's
 * trailer places, and a key whose bits fall in each page hits.
 */
void
test_txn_spill_tests_a_large_filter_takes_several_pages(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9517);
	HostSharedRecord state = host_shared_Held_record(&held);
	StdVec		bytes;
	StdVec		listed;
	StdPathBuf	path;
	TxnSpillFile file;
	HostFile	opened;
	ChunksErrorError failed;
	HostError	error;
	StdVec		page;
	size_t		pages[3];
	size_t		wanted[3];
	StdVec		pages_vec = {pages, 3, 3};
	StdVec		wanted_vec = {wanted, 3, 3};
	uint64		count;
	uint64		three = 3;
	bool		refused;
	bool		hit = true;

	txn_spill_tests_numbered(14000, &bytes, &listed);
	file = txn_spill_tests_written(io, state, 744, 0, listed.ptr, listed.len, &path);
	count = std_div_ceil_u64(file.bits, TXN_SPILL_PAGE_BITS, RUST_FILE, 1959, 30);
	STD_ASSERT_EQ(1959, 9, count == three, &count, std_u64_Debug_fmt, &three, std_u64_Debug_fmt);
	if (!chunks_file_open_rw(HOST_IO_IO_IO_DOORS(io), path.bytes.ptr, path.bytes.len, false, &opened, &failed))
		STD_RESULT_EXPECT_FAILED(1960, 62, "called `Result::unwrap()` on an `Err` value", &failed,
								 chunks_error_Error_Debug_fmt);
	for (uint64 at = 0; at < 3; at++)
	{
		if (!txn_runs_spill_filter(io, &opened, file.len, file.bits, at, &page, &error))
			STD_RESULT_EXPECT_FAILED(1962, 92, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		pages[at] = page.len;
		std_vec_free_Checked(&page, 1, 1);
	}
	wanted[0] = TXN_RUNS_FILTER_WORDS * 8;
	wanted[1] = TXN_RUNS_FILTER_WORDS * 8;
	wanted[2] = (size_t) (file.bits - 2 * TXN_SPILL_PAGE_BITS) / 8;
	STD_ASSERT_EQ(1963, 9, pages[0] == wanted[0] && pages[1] == wanted[1] && pages[2] == wanted[2], &pages_vec,
				  txn_memtable_table_tests_usizes_Debug_fmt, &wanted_vec, txn_memtable_table_tests_usizes_Debug_fmt);
	refused = !txn_runs_spill_filter(io, &opened, file.len, file.bits, 3, &page, &error);
	if (refused)
		host_Error_drop(&error);
	else
		std_vec_free_Checked(&page, 1, 1);
	STD_ASSERT(1964, 9, refused, "runs::spill_filter(io, &opened, (file.len, file.bits), 3).is_err()");
	for (size_t k = 0; hit && k < listed.len; k++)
		if (!txn_spill_hits(io, &path, &file, STD_VEC_AT(&listed, StdBytes, k).ptr, 4, &hit, &error))
			STD_RESULT_EXPECT_FAILED(1965, 71, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
	STD_ASSERT(1965, 9, hit, "listed.iter().all(|key| hits(io, (&path, &file), key).unwrap())");
	host_File_drop(&opened);
	std_vec_free_Checked(&path.bytes, 1, 1);
	std_vec_free_Checked(&listed, sizeof(StdBytes), _Alignof(StdBytes));
	std_vec_free_Checked(&bytes, 4, 1);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/* txn_spill_tests_read_t is a spill case's read key: its key and its versions' values, newest first. */
typedef struct txn_spill_tests_read_t
{
	StdBytes	key;
	/* values is a Vec<Vec<u8>>. */
	StdVec		values;
} txn_spill_tests_read_t;

/* txn_spill_tests_read_Debug_fmt is a read key's Debug, the tuple (key, values). */
static bool
txn_spill_tests_read_Debug_fmt(const void *value, StdFormatter *f)
{
	const txn_spill_tests_read_t *read = value;
	StdTupleRef tuple = STD_TUPLE(2, STD_ITEM(&read->key, std_bytes_Debug_fmt),
								  STD_ITEM(&read->values, txn_memtable_table_tests_keys_Debug_fmt));

	return std_tuple_Debug_fmt(&tuple, f);
}

/* txn_spill_tests_reads_Debug_fmt is a Vec of read keys' Debug. */
static bool
txn_spill_tests_reads_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *reads = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t k = 0; k < reads->len; k++)
		std_DebugList_entry(&list, &STD_VEC_AT(reads, txn_spill_tests_read_t, k), txn_spill_tests_read_Debug_fmt);
	return std_DebugList_finish(&list);
}

/* txn_spill_tests_reads_eq reports whether two Vecs of read keys are equal. */
static bool
txn_spill_tests_reads_eq(const StdVec *a, const StdVec *b)
{
	if (a->len != b->len)
		return false;
	for (size_t k = 0; k < a->len; k++)
	{
		const txn_spill_tests_read_t *x = &STD_VEC_AT(a, txn_spill_tests_read_t, k);
		const txn_spill_tests_read_t *y = &STD_VEC_AT(b, txn_spill_tests_read_t, k);

		if (x->key.len != y->key.len || memcmp(x->key.ptr, y->key.ptr, x->key.len) != 0 ||
			!txn_memtable_table_tests_keys_eq(&x->values, &y->values))
			return false;
	}
	return true;
}

/* txn_spill_tests_reads_free frees a Vec of read keys and their values. */
static void
txn_spill_tests_reads_free(StdVec *reads)
{
	for (size_t k = 0; k < reads->len; k++)
		txn_memtable_table_tests_keys_free(&STD_VEC_AT(reads, txn_spill_tests_read_t, k).values);
	std_vec_free_Checked(reads, sizeof(txn_spill_tests_read_t), _Alignof(txn_spill_tests_read_t));
}

/* txn_spill_tests_value returns a version's value, as a spill case's match on its owned op reads it: a put's value, or empty for any other op. */
static StdVec
txn_spill_tests_value(const TxnMemtableVersionRecord *record)
{
	TxnMemtableVersionOpRef op = txn_memtable_version_Record_op(record);
	TxnMemtableOp owned = txn_memtable_version_OpRef_owned(&op);
	StdVec		value;

	if (owned.tag != TXN_MEMTABLE_OP_PUT)
	{
		txn_memtable_Op_drop(&owned);
		std_vec_new(&value, 1, 1);
		return value;
	}
	value = owned.u.put.value;
	std_vec_new(&owned.u.put.value, 1, 1);
	txn_memtable_Op_drop(&owned);
	return value;
}

/* txn_spill_tests_versioned_t is a spill case's chain entry: a version's writer's xid and its value. */
typedef struct txn_spill_tests_versioned_t
{
	uint64		xid;
	/* value is a Vec<u8>. */
	StdVec		value;
} txn_spill_tests_versioned_t;

/* txn_spill_tests_versioned_Debug_fmt is a chain entry's Debug, the tuple (xid, value). */
static bool
txn_spill_tests_versioned_Debug_fmt(const void *value, StdFormatter *f)
{
	const txn_spill_tests_versioned_t *versioned = value;
	StdBytes	bytes = {versioned->value.ptr, versioned->value.len};
	StdTupleRef tuple = STD_TUPLE(2, STD_ITEM(&versioned->xid, std_u64_Debug_fmt),
								  STD_ITEM(&bytes, std_bytes_Debug_fmt));

	return std_tuple_Debug_fmt(&tuple, f);
}

/* txn_spill_tests_chain_Debug_fmt is a Vec of chain entries' Debug. */
static bool
txn_spill_tests_chain_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *chain = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t k = 0; k < chain->len; k++)
		std_DebugList_entry(&list, &STD_VEC_AT(chain, txn_spill_tests_versioned_t, k),
							txn_spill_tests_versioned_Debug_fmt);
	return std_DebugList_finish(&list);
}

/* txn_spill_tests_chain_is asserts at its line that a key's chain is a list of xids and values, and frees the chain. */
static void
txn_spill_tests_chain_is(uint32 line, StdVec *chain, const uint64 *xids, const char *const *values, size_t len)
{
	txn_spill_tests_versioned_t wanted[2];
	StdVec		wanted_vec = {wanted, len, len};
	bool		eq = chain->len == len;

	for (size_t k = 0; k < len; k++)
	{
		wanted[k].xid = xids[k];
		std_slice_to_vec_Checked(&wanted[k].value, (const uint8 *) values[k], strlen(values[k]), 1, 1);
	}
	for (size_t k = 0; eq && k < len; k++)
	{
		const txn_spill_tests_versioned_t *entry = &STD_VEC_AT(chain, txn_spill_tests_versioned_t, k);

		eq = entry->xid == wanted[k].xid && entry->value.len == wanted[k].value.len &&
			memcmp(entry->value.ptr, wanted[k].value.ptr, entry->value.len) == 0;
	}
	STD_ASSERT_EQ(line, 9, eq, chain, txn_spill_tests_chain_Debug_fmt, &wanted_vec, txn_spill_tests_chain_Debug_fmt);
	for (size_t k = 0; k < len; k++)
		std_vec_free_Checked(&wanted[k].value, 1, 1);
	for (size_t k = 0; k < chain->len; k++)
		std_vec_free_Checked(&STD_VEC_AT(chain, txn_spill_tests_versioned_t, k).value, 1, 1);
	std_vec_free_Checked(chain, sizeof(txn_spill_tests_versioned_t), _Alignof(txn_spill_tests_versioned_t));
}

/* txn_spill_tests_chain returns a key's versions in a state's memtable and spills, each its writer's xid and its value, as the spill case's chain closure does. */
static StdVec
txn_spill_tests_chain(HostIoIo *io, const HostDeltaHold *hold, HostSharedRecord state, const char *key)
{
	StdVec		versions;
	StdVec		chain;
	HostError	error;

	if (!txn_memtable_table_versions(io, hold, state, (const uint8 *) key, strlen(key), &versions, &error))
		STD_RESULT_EXPECT_FAILED(2035, 67, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	std_vec_with_capacity_Checked(&chain, versions.len, sizeof(txn_spill_tests_versioned_t),
								  _Alignof(txn_spill_tests_versioned_t));
	for (size_t k = 0; k < versions.len; k++)
	{
		TxnMemtableVersionVersion *version = &STD_VEC_AT(&versions, TxnMemtableVersionVersion, k);
		TxnMemtableVersionRecord record = txn_memtable_version_Version_record(version);
		txn_spill_tests_versioned_t entry = {.xid = txn_memtable_version_Record_writer(&record).xid,
		.value = txn_spill_tests_value(&record)};

		std_vec_push_Checked(&chain, &entry, sizeof(entry), _Alignof(txn_spill_tests_versioned_t));
		txn_memtable_version_Version_drop(version);
	}
	std_vec_free_Checked(&versions, sizeof(TxnMemtableVersionVersion), _Alignof(TxnMemtableVersionVersion));
	return chain;
}

/*
 * Bugs row 74's spill: a spill takes out of the memtable exactly the keys
 * whose chain holds only its transaction's versions, writes them, each key's
 * versions newest first, to a synced file its entry names, and leaves a key
 * another writer's version is in, whatever the order of the two.
 */
void
test_txn_spill_tests_a_spill_takes_only_the_keys_its_transaction_alone_holds(void)
{
	static const char *const left_keys[] = {"c", "d"};
	static const char *const a_values[] = {"1"};
	static const char *const b_values[] = {"3", "2"};
	static const char *const d_values[] = {"6", "5"};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9743);
	HostSharedRecord state = host_shared_Held_record(&held);
	HostStorage storage = txn_memtable_testing_storage(9743);
	uint64		mine = simc_begin().f0;
	uint64		other = simc_begin().f0;
	const struct
	{
		uint64		xid;
		const char *key;
		const char *value;
	}			pushes[] = {{mine, "a", "1"}, {mine, "b", "2"}, {mine, "b", "3"}, {other, "c", "4"}, {other, "d", "5"},
	{mine, "d", "6"}};
	uint64		b_xids[2] = {mine, mine};
	uint64		d_xids[2] = {mine, other};
	uint32		zero = 0;
	uint64		sized[2];
	txn_spill_tests_read_t entries[2] = {{.key = {(const uint8 *) "a", 1}}, {.key = {(const uint8 *) "b", 1}}};
	StdTupleRef left_tuple;
	StdTupleRef right_tuple;
	StdVec		left;
	StdVec		wanted;
	StdVec		listed;
	StdVec		bytes;
	StdVec		read;
	StdVec		chain;
	StdPathBuf	dir;
	StdPathBuf	path;
	StdString	name;
	TxnRunsNames names;
	TxnRunsDecoded decoded;
	TxnSpillFile file;
	HostDeltaHold hold;
	ChunksErrorError failed;
	HostError	error;
	size_t		kept = 0;
	size_t		one = 1;
	uint64		hash;
	bool		spilled;
	bool		hit;

	for (size_t k = 0; k < lengthof(pushes); k++)
	{
		TxnMemtableOp op = txn_memtable_testing_put((const uint8 *) pushes[k].value, 1);
		TxnMemtableVersionChange change = {.writer = txn_memtable_testing_writer(pushes[k].xid), .op = &op,
		.lock = TXN_MEMTABLE_LOCK_MODE_UPDATE, .shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND}};
		TxnMemtableVersionHandle handle;

		if (!txn_memtable_table_push(io, state, (const uint8 *) pushes[k].key, 1, &change, &handle, &error))
			STD_RESULT_EXPECT_FAILED(1995, 50, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		txn_memtable_Op_drop(&op);
	}
	if (!txn_spill_spilled(io, &storage, TXN_WRITTEN_OF_ROWS, mine, &spilled, &error))
		STD_RESULT_EXPECT_FAILED(1997, 61, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(1997, 9, spilled, "spilled(io, &storage(9743), Of::Rows, mine).unwrap()");
	std_vec_new(&left, sizeof(StdVec), _Alignof(StdVec));
	if (!txn_memtable_table_walked(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, io, state, txn_memtable_table_tests_collecting,
								   &left, NULL, &error))
		STD_RESULT_EXPECT_FAILED(2003, 10, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	txn_memtable_table_tests_keys_of(left_keys, lengthof(left_keys), &wanted);
	STD_ASSERT_EQ(2004, 9, txn_memtable_table_tests_keys_eq(&left, &wanted), &left,
				  txn_memtable_table_tests_keys_Debug_fmt, &wanted, txn_memtable_table_tests_keys_Debug_fmt);
	txn_memtable_table_tests_keys_free(&wanted);
	txn_memtable_table_tests_keys_free(&left);
	listed = txn_spill_tests_files_of(io, state);
	for (size_t k = 0; k < listed.len; k++)
		if (STD_VEC_AT(&listed, TxnSpillFile, k).kind == TXN_RUNS_KIND_SPILLED_ROWS)
			STD_VEC_AT(&listed, TxnSpillFile, kept++) = STD_VEC_AT(&listed, TxnSpillFile, k);
	listed.len = kept;
	STD_ASSERT_EQ(2007, 9, listed.len == 1, &listed.len, std_usize_Debug_fmt, &one, std_usize_Debug_fmt);
	file = STD_VEC_AT(&listed, TxnSpillFile, 0);
	std_vec_free_Checked(&listed, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	left_tuple = STD_TUPLE(2, STD_ITEM(&file.xid, std_u64_Debug_fmt), STD_ITEM(&file.number, std_u32_Debug_fmt));
	right_tuple = STD_TUPLE(2, STD_ITEM(&mine, std_u64_Debug_fmt), STD_ITEM(&zero, std_u32_Debug_fmt));
	STD_ASSERT_EQ(2009, 9, file.xid == mine && file.number == 0, &left_tuple, std_tuple_Debug_fmt, &right_tuple,
				  std_tuple_Debug_fmt);
	if (!txn_spill_names(&TXN_SPILL_STATE_HOME_OPS, state, &storage, TXN_RUNS_KIND_SPILLED_ROWS, mine, &names, &error))
		STD_RESULT_EXPECT_FAILED(2010, 75, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	txn_runs_spill_directory(9743, txn_state_own(state), &dir);
	txn_runs_spill_name(&names, 0, &name);
	std_path_join(dir.bytes.ptr, dir.bytes.len, name.ptr, name.len, &path);
	std_String_drop(&name);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	if (!txn_spill_hits(io, &path, &file, (const uint8 *) "a", 1, &hit, &error))
		STD_RESULT_EXPECT_FAILED(2012, 48, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (hit && !txn_spill_hits(io, &path, &file, (const uint8 *) "b", 1, &hit, &error))
		STD_RESULT_EXPECT_FAILED(2012, 91, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(2012, 9, hit,
			   "hits(io, (&path, &file), b\"a\").unwrap() && hits(io, (&path, &file), b\"b\").unwrap()");
	if (!chunks_file_read_file(HOST_IO_IO_READ_DOORS(io), path.bytes.ptr, path.bytes.len, &bytes, &failed))
		STD_RESULT_EXPECT_FAILED(2013, 56, "called `Result::unwrap()` on an `Err` value", &failed,
								 chunks_error_Error_Debug_fmt);
	if (!txn_runs_spill_hash(bytes.ptr, bytes.len, &hash, &error))
		STD_RESULT_EXPECT_FAILED(2014, 66, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	sized[0] = bytes.len;
	sized[1] = hash;
	left_tuple = STD_TUPLE(2, STD_ITEM(&sized[0], std_u64_Debug_fmt), STD_ITEM(&sized[1], std_u64_Debug_fmt));
	right_tuple = STD_TUPLE(2, STD_ITEM(&file.len, std_u64_Debug_fmt), STD_ITEM(&file.hash, std_u64_Debug_fmt));
	STD_ASSERT_EQ(2014, 9, sized[0] == file.len && sized[1] == file.hash, &left_tuple, std_tuple_Debug_fmt,
				  &right_tuple, std_tuple_Debug_fmt);
	if (!txn_runs_decode(bytes.ptr, bytes.len, &names, file.hash, &decoded, &error))
		STD_RESULT_EXPECT_FAILED(2015, 63, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	std_vec_with_capacity_Checked(&read, decoded.keys.len, sizeof(txn_spill_tests_read_t),
								  _Alignof(txn_spill_tests_read_t));
	for (size_t k = 0; k < decoded.keys.len; k++)
	{
		const TxnRunsKey *key = &STD_VEC_AT(&decoded.keys, TxnRunsKey, k);
		txn_spill_tests_read_t entry = {.key = key->key};

		std_vec_with_capacity_Checked(&entry.values, key->versions.len, sizeof(StdVec), _Alignof(StdVec));
		for (size_t j = 0; j < key->versions.len; j++)
		{
			StdVec		value = txn_spill_tests_value(&STD_VEC_AT(&key->versions, TxnMemtableVersionRecord, j));

			std_vec_push_Checked(&entry.values, &value, sizeof(StdVec), _Alignof(StdVec));
		}
		std_vec_push_Checked(&read, &entry, sizeof(entry), _Alignof(txn_spill_tests_read_t));
	}
	txn_memtable_table_tests_keys_of(a_values, lengthof(a_values), &entries[0].values);
	txn_memtable_table_tests_keys_of(b_values, lengthof(b_values), &entries[1].values);
	std_slice_to_vec_Checked(&wanted, entries, lengthof(entries), sizeof(txn_spill_tests_read_t),
							 _Alignof(txn_spill_tests_read_t));
	STD_ASSERT_EQ(2032, 9, txn_spill_tests_reads_eq(&read, &wanted), &read, txn_spill_tests_reads_Debug_fmt, &wanted,
				  txn_spill_tests_reads_Debug_fmt);
	txn_spill_tests_reads_free(&wanted);
	txn_spill_tests_reads_free(&read);
	txn_runs_Decoded_drop(&decoded);
	std_vec_free_Checked(&bytes, 1, 1);
	if (!host_delta_hold(io, &hold, &error))
		STD_RESULT_EXPECT_FAILED(2033, 36, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	chain = txn_spill_tests_chain(io, &hold, state, "b");
	txn_spill_tests_chain_is(2047, &chain, b_xids, b_values, 2);
	chain = txn_spill_tests_chain(io, &hold, state, "d");
	txn_spill_tests_chain_is(2048, &chain, d_xids, d_values, 2);
	chain = txn_spill_tests_chain(io, &hold, state, "e");
	STD_ASSERT(2049, 9, chain.len == 0, "chain(io, b\"e\").is_empty()");
	std_vec_free_Checked(&chain, sizeof(txn_spill_tests_versioned_t), _Alignof(txn_spill_tests_versioned_t));
	host_delta_Hold_drop(&hold);
	std_vec_free_Checked(&path.bytes, 1, 1);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/* txn_spill_tests_Direction_Debug_fmt is Direction's derived Debug, its variant's name, for the tests' asserts. */
static bool
txn_spill_tests_Direction_Debug_fmt(const void *value, StdFormatter *f)
{
	if (*(const TxnMemtableTableDirection *) value == TXN_MEMTABLE_TABLE_DIRECTION_FORWARD)
		return std_fmt_write_str(f, "Forward", 7);
	return std_fmt_write_str(f, "Backward", 8);
}

/* txn_spill_tests_versions_free drops a Vec<Version>'s versions and frees it. */
static void
txn_spill_tests_versions_free(StdVec *versions)
{
	for (size_t k = 0; k < versions->len; k++)
		txn_memtable_version_Version_drop(&STD_VEC_AT(versions, TxnMemtableVersionVersion, k));
	std_vec_free_Checked(versions, sizeof(TxnMemtableVersionVersion), _Alignof(TxnMemtableVersionVersion));
}

/* txn_spill_tests_one_version asserts at its places that a number's key has one version in a state's memtable and spills, the message the number when one is asked for. */
static void
txn_spill_tests_one_version(uint32 line, uint32 col, uint32 unwrap_col, HostIoIo *io, const HostDeltaHold *hold,
							HostSharedRecord state, uint64 number, bool numbered)
{
	static const StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
	StdFmtArg	args[1] = {{&number, std_u64_Display_fmt}};
	StdFmtArguments message = {pieces, lengthof(pieces), args};
	StdVec		versions;
	HostError	error;
	size_t		one = 1;
	uint8		key[8];

	std_u64_to_be_bytes(number, key);
	if (!txn_memtable_table_versions(io, hold, state, key, sizeof(key), &versions, &error))
		STD_RESULT_EXPECT_FAILED(line, unwrap_col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (versions.len != 1)
		std_assert_failed(RUST_FILE, line, col, "==", &versions.len, std_usize_Debug_fmt, &one, std_usize_Debug_fmt,
						  numbered ? &message : NULL);
	txn_spill_tests_versions_free(&versions);
}

/* txn_spill_tests_doubled returns the scan case's key of a number: its double, as big-endian bytes, as its key closure does. */
static void
txn_spill_tests_doubled(uint32 number, StdVec *result)
{
	uint8		be[4];

	std_u32_to_be_bytes(number * 2, be);
	std_slice_to_vec_Checked(result, be, sizeof(be), 1, 1);
}

/*
 * Bugs row 74's scan: a scan's steps over a storage's rows merge its
 * memtable's keys and its spilled keys in key order, both ways, across a
 * spill file's chunks, and a key's lookup finds its spilled version in any
 * chunk.
 */
void
test_txn_spill_tests_a_scan_merges_spilled_keys_across_chunks_in_order(void)
{
	static const StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
	static const TxnMemtableTableDirection ways[] = {TXN_MEMTABLE_TABLE_DIRECTION_FORWARD,
	TXN_MEMTABLE_TABLE_DIRECTION_BACKWARD};
	static const uint32 looked[] = {0, 77, 150, 299};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9744);
	HostSharedRecord state = host_shared_Held_record(&held);
	HostStorage storage = txn_memtable_testing_storage(9744);
	uint64		mine = simc_begin().f0;
	uint64		other = simc_begin().f0;
	uint8		odd[4];
	uint8		three[4];
	uint8		value[1000];
	StdVec		expected;
	StdVec		odd_key;
	StdVec		versions;
	TxnMemtableOp op;
	TxnMemtableVersionChange change = {.writer = txn_memtable_testing_writer(mine), .op = &op,
	.lock = TXN_MEMTABLE_LOCK_MODE_UPDATE, .shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND}};
	TxnMemtableVersionHandle handle;
	HostDeltaHold hold;
	HostError	error;
	bool		sorted_odd = false;
	size_t		one = 1;
	bool		spilled;

	for (uint32 number = 0; number < 300; number++)
	{
		StdVec		key;

		memset(value, (uint8) number, sizeof(value));
		op = txn_memtable_testing_put(value, sizeof(value));
		txn_spill_tests_doubled(number, &key);
		if (!txn_memtable_table_push(io, state, key.ptr, key.len, &change, &handle, &error))
			STD_RESULT_EXPECT_FAILED(2071, 59, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		std_vec_free_Checked(&key, 1, 1);
		txn_memtable_Op_drop(&op);
	}
	std_u32_to_be_bytes(301, odd);
	op = txn_memtable_testing_put((const uint8 *) "o", 1);
	change.writer = txn_memtable_testing_writer(other);
	if (!txn_memtable_table_push(io, state, odd, sizeof(odd), &change, &handle, &error))
		STD_RESULT_EXPECT_FAILED(2076, 47, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	txn_memtable_Op_drop(&op);
	if (!txn_spill_spilled(io, &storage, TXN_WRITTEN_OF_ROWS, mine, &spilled, &error))
		STD_RESULT_EXPECT_FAILED(2077, 61, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(2077, 9, spilled, "spilled(io, &storage(9744), Of::Rows, mine).unwrap()");
	std_vec_with_capacity_Checked(&expected, 301, sizeof(StdVec), _Alignof(StdVec));
	for (uint32 number = 0; number < 300; number++)
	{
		StdVec		key;

		txn_spill_tests_doubled(number, &key);
		if (!sorted_odd && memcmp(odd, key.ptr, sizeof(odd)) < 0)
		{
			std_slice_to_vec_Checked(&odd_key, odd, sizeof(odd), 1, 1);
			std_vec_push_Checked(&expected, &odd_key, sizeof(StdVec), _Alignof(StdVec));
			sorted_odd = true;
		}
		std_vec_push_Checked(&expected, &key, sizeof(StdVec), _Alignof(StdVec));
	}
	if (!host_delta_hold(io, &hold, &error))
		STD_RESULT_EXPECT_FAILED(2081, 36, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	for (size_t w = 0; w < lengthof(ways); w++)
	{
		StdVec		keys;
		StdVec		after = {0};
		bool		has_after = false;

		std_vec_new(&keys, sizeof(StdVec), _Alignof(StdVec));
		for (;;)
		{
			txn_TupleVecU8VecVersion_t found;
			StdVec		copy;
			bool		some;

			if (!txn_memtable_table_next(io, &hold, state, has_after, after.ptr, after.len, ways[w], &some, &found,
										 &error))
				STD_RESULT_EXPECT_FAILED(2085, 102, "called `Result::unwrap()` on an `Err` value", &error,
										 host_Error_Debug_fmt);
			if (!some)
				break;
			STD_ASSERT_EQ(2086, 17, found.f1.len == 1, &found.f1.len, std_usize_Debug_fmt, &one,
						  std_usize_Debug_fmt);
			txn_spill_tests_versions_free(&found.f1);
			std_vec_clone(&copy, &found.f0, 1, 1);
			std_vec_push_Checked(&keys, &copy, sizeof(StdVec), _Alignof(StdVec));
			if (has_after)
				std_vec_free_Checked(&after, 1, 1);
			after = found.f0;
			has_after = true;
		}
		if (has_after)
			std_vec_free_Checked(&after, 1, 1);
		if (ways[w] == TXN_MEMTABLE_TABLE_DIRECTION_BACKWARD)
			for (size_t k = 0; k < keys.len / 2; k++)
			{
				StdVec		swap = STD_VEC_AT(&keys, StdVec, k);

				STD_VEC_AT(&keys, StdVec, k) = STD_VEC_AT(&keys, StdVec, keys.len - 1 - k);
				STD_VEC_AT(&keys, StdVec, keys.len - 1 - k) = swap;
			}
		if (!txn_memtable_table_tests_keys_eq(&keys, &expected))
		{
			StdFmtArg	args[1] = {{&ways[w], txn_spill_tests_Direction_Debug_fmt}};
			StdFmtArguments message = {pieces, lengthof(pieces), args};

			std_assert_failed(RUST_FILE, 2093, 13, "==", &keys, txn_memtable_table_tests_keys_Debug_fmt, &expected,
							  txn_memtable_table_tests_keys_Debug_fmt, &message);
		}
		txn_memtable_table_tests_keys_free(&keys);
	}
	txn_memtable_table_tests_keys_free(&expected);
	for (size_t k = 0; k < lengthof(looked); k++)
	{
		StdVec		key;
		TxnMemtableVersionRecord record;
		uint64		xid;

		txn_spill_tests_doubled(looked[k], &key);
		if (!txn_memtable_table_versions(io, &hold, state, key.ptr, key.len, &versions, &error))
			STD_RESULT_EXPECT_FAILED(2096, 76, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		std_vec_free_Checked(&key, 1, 1);
		STD_ASSERT_EQ(2097, 13, versions.len == 1, &versions.len, std_usize_Debug_fmt, &one, std_usize_Debug_fmt);
		record = txn_memtable_version_Version_record(&STD_VEC_AT(&versions, TxnMemtableVersionVersion, 0));
		xid = txn_memtable_version_Record_writer(&record).xid;
		STD_ASSERT_EQ(2098, 13, xid == mine, &xid, std_u64_Debug_fmt, &mine, std_u64_Debug_fmt);
		txn_spill_tests_versions_free(&versions);
	}
	std_u32_to_be_bytes(3, three);
	if (!txn_memtable_table_versions(io, &hold, state, three, sizeof(three), &versions, &error))
		STD_RESULT_EXPECT_FAILED(2100, 72, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(2100, 9, versions.len == 0, "table::versions(io, &hold, state, &3u32.to_be_bytes()).unwrap().is_empty()");
	txn_spill_tests_versions_free(&versions);
	host_delta_Hold_drop(&hold);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

#include "doltgres/txn/values.h"

/* txn_spill_tests_newest_is asserts at its line that a value id's newest entry, in a state's value map or in its spilled value maps, is a wanted one or nothing. */
static void
txn_spill_tests_newest_is(uint32 line, uint32 unwrap_col, HostIoIo *io, HostSharedRecord state, bool spilled,
						  uint32 id, bool has_expected, const TxnMemtableMapEntry *expected)
{
	TxnMemtableMapEntry entry;
	HostError	error;
	bool		some;
	bool		ok;
	StdOptionRef left;
	StdOptionRef right = {has_expected, STD_ITEM(expected, txn_memtable_map_tests_seen_Debug_fmt)};

	if (spilled)
		ok = txn_spill_newest_value(io, state, id, &some, &entry, &error);
	else
		ok = txn_memtable_map_newest(io, state, TXN_MEMTABLE_MAP_MAP_VALUES, id, &some, &entry, &error);
	if (!ok)
		STD_RESULT_EXPECT_FAILED(line, unwrap_col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	left = (StdOptionRef)
	{
		some, STD_ITEM(&entry, txn_memtable_map_tests_seen_Debug_fmt)
	};
	STD_ASSERT_EQ(line, 9, some == has_expected && (!some || txn_memtable_map_tests_seen_eq(&entry, expected)),
				  &left, std_option_Debug_fmt, &right, std_option_Debug_fmt);
	if (some)
		txn_memtable_map_tests_seen_drop(&entry);
}

/* txn_spill_tests_mark_Debug_fmt is a value map's mark's Debug, the tuple (u64, u64). */
static bool
txn_spill_tests_mark_Debug_fmt(const void *value, StdFormatter *f)
{
	const std_TupleU64U64_t *mark = value;
	StdTupleRef tuple = STD_TUPLE(2, STD_ITEM(&mark->f0, std_u64_Debug_fmt), STD_ITEM(&mark->f1, std_u64_Debug_fmt));

	return std_tuple_Debug_fmt(&tuple, f);
}

/*
 * Bugs row 74's value map: a spill of a storage's value map takes out the
 * entries only its transaction wrote, and a reader finds a spilled entry's
 * newest below the memtable, a removal included, and moves the state's mark.
 */
void
test_txn_spill_tests_a_spilled_value_maps_entries_are_read_below_the_memtable(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9745);
	HostSharedRecord state = host_shared_Held_record(&held);
	HostStorage storage = txn_memtable_testing_storage(9745);
	uint64		mine = simc_begin().f0;
	uint64		other = simc_begin().f0;
	const struct
	{
		uint64		xid;
		uint32		id;
		const char *value;
	}			pushes[] = {{mine, 7, "e"}, {mine, 8, "f"}, {mine, 8, NULL}, {other, 9, "g"}};
	TxnMemtableMapEntry e = {.has_value = true};
	TxnMemtableMapEntry removal = {.has_value = false};
	TxnMemtableMapEntry g = {.has_value = true};
	std_TupleU64U64_t before;
	std_TupleU64U64_t after;
	HostError	error;
	bool		spilled;

	for (size_t k = 0; k < lengthof(pushes); k++)
	{
		TxnMemtableOp op = {.tag = TXN_MEMTABLE_OP_DELETE};
		TxnMemtableVersionChange change = {.writer = txn_memtable_testing_writer(pushes[k].xid), .op = &op,
		.lock = TXN_MEMTABLE_LOCK_MODE_UPDATE, .shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND}};
		TxnMemtableVersionHandle handle;
		uint8		key[4];

		if (pushes[k].value != NULL)
			op = txn_memtable_testing_put((const uint8 *) pushes[k].value, 1);
		std_u32_to_be_bytes(pushes[k].id, key);
		if (!txn_memtable_table_alone_State(io, state, true, TXN_MEMTABLE_MAP_MAP_VALUES, key, sizeof(key), &change,
											&handle, &error))
			STD_RESULT_EXPECT_FAILED(2122, 84, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		txn_memtable_Op_drop(&op);
	}
	before = txn_values_mark(state);
	if (!txn_spill_spilled(io, &storage, TXN_WRITTEN_OF_VALUES, mine, &spilled, &error))
		STD_RESULT_EXPECT_FAILED(2125, 63, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(2125, 9, spilled, "spilled(io, &storage(9745), Of::Values, mine).unwrap()");
	after = txn_values_mark(state);
	if (after.f0 == before.f0 && after.f1 == before.f1)
		std_assert_failed(RUST_FILE, 2126, 9, "!=", &after, txn_spill_tests_mark_Debug_fmt, &before,
						  txn_spill_tests_mark_Debug_fmt, NULL);
	std_slice_to_vec_Checked(&e.value, (const uint8 *) "e", 1, 1, 1);
	std_slice_to_vec_Checked(&g.value, (const uint8 *) "g", 1, 1, 1);
	txn_spill_tests_newest_is(2127, 59, io, state, false, 7, false, NULL);
	txn_spill_tests_newest_is(2128, 47, io, state, true, 7, true, &e);
	txn_spill_tests_newest_is(2129, 47, io, state, true, 8, true, &removal);
	txn_spill_tests_newest_is(2130, 47, io, state, true, 9, false, NULL);
	txn_spill_tests_newest_is(2131, 59, io, state, false, 9, true, &g);
	std_vec_free_Checked(&g.value, 1, 1);
	std_vec_free_Checked(&e.value, 1, 1);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

#define TXN_MEMTABLE_TABLE_RS Unit
#define TXN_MEMTABLE_TABLE_RETURNS bool
#define TXN_MEMTABLE_TABLE_RESULT_PARAMS , HostError *error
#define TXN_MEMTABLE_TABLE_DECLARE
#include "doltgres/txn/memtable/table_template.h"

/* txn_spill_tests_kinds_Debug_fmt is a Vec<Kind>'s Debug. */
static bool
txn_spill_tests_kinds_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *kinds = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t k = 0; k < kinds->len; k++)
		std_DebugList_entry(&list, &STD_VEC_AT(kinds, TxnRunsKind, k), txn_spill_tests_Kind_Debug_fmt);
	return std_DebugList_finish(&list);
}

/* txn_spill_tests_kinds_are asserts at its place that listed files are of a list of kinds, newest first. */
static void
txn_spill_tests_kinds_are(uint32 line, const StdVec *files, const TxnRunsKind *wanted, size_t wanted_len)
{
	StdVec		kinds;
	StdVec		wanted_vec = {(void *) wanted, wanted_len, wanted_len};
	bool		eq = files->len == wanted_len;

	std_vec_with_capacity_Checked(&kinds, files->len, sizeof(TxnRunsKind), _Alignof(TxnRunsKind));
	for (size_t k = 0; k < files->len; k++)
		std_vec_push_Checked(&kinds, &STD_VEC_AT(files, TxnSpillFile, k).kind, sizeof(TxnRunsKind),
							 _Alignof(TxnRunsKind));
	for (size_t k = 0; eq && k < wanted_len; k++)
		eq = STD_VEC_AT(&kinds, TxnRunsKind, k) == wanted[k];
	STD_ASSERT_EQ(line, 9, eq, &kinds, txn_spill_tests_kinds_Debug_fmt, &wanted_vec, txn_spill_tests_kinds_Debug_fmt);
	std_vec_free_Checked(&kinds, sizeof(TxnRunsKind), _Alignof(TxnRunsKind));
}

/* txn_spill_tests_tuple_is asserts at its line that a tid's row's key in a state's tuple id table, or in its spilled tuple ids, is a wanted key or nothing. */
static void
txn_spill_tests_tuple_is(uint32 line, uint32 col, uint32 unwrap_col, HostIoIo *io, HostSharedRecord state, uint64 tid,
						 const TxnMemtableMapEntry *expected)
{
	HostRow		row = {tid};
	TxnMemtableMapEntry found;
	HostError	error;

	if (!txn_memtable_tuples_get(io, state, row, &found.has_value, &found.value, &error))
		STD_RESULT_EXPECT_FAILED(line, unwrap_col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT_EQ(line, col, txn_memtable_map_tests_seen_eq(&found, expected), &found,
				  txn_memtable_map_tests_seen_Debug_fmt, expected, txn_memtable_map_tests_seen_Debug_fmt);
	txn_memtable_map_tests_seen_drop(&found);
}

/*
 * Bugs row 74's tuple ids: a spill of a storage's rows takes its rows' tuple
 * ids out of the tuple id table into a file of spilled tuple ids, and each
 * tid still finds its row's key, the other writer's from the table.
 */
void
test_txn_spill_tests_a_spills_tuple_ids_still_find_their_rows_keys(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9746);
	HostSharedRecord state = host_shared_Held_record(&held);
	HostStorage storage = txn_memtable_testing_storage(9746);
	uint64		mine = simc_begin().f0;
	uint64		other = simc_begin().f0;
	const struct
	{
		uint64		xid;
		uint64		number;
	}			writes[] = {{mine, 1}, {mine, 2}, {mine, 3}, {other, 4}};
	static const TxnRunsKind wanted[] = {TXN_RUNS_KIND_SPILLED_TUPLES, TXN_RUNS_KIND_SPILLED_ROWS};
	TxnMemtableMapEntry none = {.has_value = false};
	StdVec		files;
	HostError	error;
	bool		spilled;

	for (size_t k = 0; k < lengthof(writes); k++)
	{
		uint64		tid = UINT64CONST(1) << 16 | writes[k].number;
		TxnMemtableOp op = txn_memtable_testing_put((const uint8 *) "v", 1);
		TxnMemtableVersionChange change = {.writer = txn_memtable_testing_writer(writes[k].xid), .op = &op,
		.lock = TXN_MEMTABLE_LOCK_MODE_UPDATE, .shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND}};
		txn_memtable_tuples_tests_stored_t captures = {.tid = tid, .change = &change};
		txn_TupleRefSliceU8Usize_t rows[1];
		uint8		key[8];

		op.u.put.tuple.f0 = tid;
		std_u64_to_be_bytes(writes[k].number, key);
		rows[0].f0 = (StdBytes)
		{
			key, sizeof(key)
		};
		rows[0].f1 = txn_memtable_version_encoded(&change.writer, change.op, change.shown, false, 0);
		if (!txn_memtable_table_prepare_Unit(io, state, rows, lengthof(rows), NULL, 0, true,
											 txn_memtable_tuples_tests_stored_inside, &captures, NULL, &error))
			STD_RESULT_EXPECT_FAILED(2161, 14, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		txn_memtable_Op_drop(&op);
	}
	if (!txn_spill_spilled(io, &storage, TXN_WRITTEN_OF_ROWS, mine, &spilled, &error))
		STD_RESULT_EXPECT_FAILED(2163, 61, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(2163, 9, spilled, "spilled(io, &storage(9746), Of::Rows, mine).unwrap()");
	files = txn_spill_tests_files_of(io, state);
	txn_spill_tests_kinds_are(2165, &files, wanted, lengthof(wanted));
	std_vec_free_Checked(&files, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	for (uint64 number = 1; number <= 4; number++)
	{
		TxnMemtableMapEntry key = {.has_value = true};
		uint8		be[8];

		std_u64_to_be_bytes(number, be);
		std_slice_to_vec_Checked(&key.value, be, sizeof(be), 1, 1);
		txn_spill_tests_tuple_is(2167, 13, 65, io, state, UINT64CONST(1) << 16 | number, &key);
		std_vec_free_Checked(&key.value, 1, 1);
	}
	txn_spill_tests_tuple_is(2169, 9, 56, io, state, UINT64CONST(1) << 16 | 9, &none);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

#include "doltgres/txn/fold.h"
#include "doltgres/txn/insert.h"

/* TXN_SPILL_TESTS_WRITERS are the three writers' names and databases. */
static const struct
{
	const char *name;
	uint32		database;
}			TXN_SPILL_TESTS_WRITERS[] = {{"w1", 9484}, {"w2", 9485}, {"w3", 9486}};

/* txn_spill_tests_tree returns the three writers' case's storage of a tree of a database, as its trees closure numbers them. */
static HostStorage
txn_spill_tests_tree(uint32 database, uint32 tree)
{
	HostStorage storage = txn_memtable_testing_storage(database);

	storage.relation = 16384 + tree;
	return storage;
}

/* txn_spill_tests_based_t is the three writers' case's base of a tree: its repository, a shared read, and its state. */
typedef struct txn_spill_tests_based_t
{
	RepoRepo	repo;
	HostSharedReader reading;
	HostSharedHeld held;
} txn_spill_tests_based_t;

/* txn_spill_tests_based opens a tree's repository and a shared read, and makes the tree's state, keyless and permanent, as the three writers' case's based closure does. */
static bool
txn_spill_tests_based(HostIoIo *io, const HostStorage *tree, txn_spill_tests_based_t *result, HostError *error)
{
	TxnStateMade made = {.key = {.tag = TXN_BASE_KEY_NONE}, .table = tree->relation};
	ChunksErrorError failed;

	if (!repo_Repo_open(io, tree->database, HOST_PG_DEFAULT, &result->repo, &failed))
	{
		*error = txn_base_storage(&failed);
		return false;
	}
	if (!host_shared_read(io, &result->reading, error))
	{
		repo_Repo_drop(&result->repo);
		return false;
	}
	if (!txn_state_state(io, &result->repo, &result->reading, tree, TXN_STATE_PERSISTENCE_PERMANENT, true, &made,
						 &result->held, error))
	{
		host_shared_Reader_drop(&result->reading);
		repo_Repo_drop(&result->repo);
		return false;
	}
	return true;
}

/* txn_spill_tests_based_drop drops a tree's base: its state, its read, then its repository. */
static void
txn_spill_tests_based_drop(txn_spill_tests_based_t *based)
{
	host_shared_Held_drop(&based->held);
	host_shared_Reader_drop(&based->reading);
	repo_Repo_drop(&based->repo);
}

/* txn_spill_tests_three_t is the three writers' case's shared state: its trees and rows each, whether it folds, its counts of writers finished and ended, the databases committed, and whether a writer failed. */
typedef struct txn_spill_tests_three_t
{
	uint32		trees_each;
	uint64		rows;
	bool		folding;
	size_t		finished;
	size_t		ended;
	/* committed is a Vec<u32>. */
	StdVec		committed;
	bool		failed;
} txn_spill_tests_three_t;

/* txn_spill_tests_writing_t is a writer's captures in the three writers' case: the shared state and its database. */
typedef struct txn_spill_tests_writing_t
{
	txn_spill_tests_three_t *three;
	uint32		database;
} txn_spill_tests_writing_t;

/* txn_spill_tests_states_free drops a writer's trees' states and frees them. */
static void
txn_spill_tests_states_free(StdVec *states)
{
	for (size_t k = 0; k < states->len; k++)
		host_shared_Held_drop(&STD_VEC_AT(states, HostSharedHeld, k));
	std_vec_free_Checked(states, sizeof(HostSharedHeld), _Alignof(HostSharedHeld));
}

/* txn_spill_tests_pushed writes each row of a writer's trees, a key at a time across its trees, each counted in the transaction's written trees first. */
static bool
txn_spill_tests_pushed(const txn_spill_tests_writing_t *c, HostIoIo *io, const StdVec *states, HostXid xid,
					   const TxnMemtableOp *op, HostError *error)
{
	TxnMemtableVersionChange change = {.writer = txn_memtable_testing_writer(xid.f0), .op = op,
	.lock = TXN_MEMTABLE_LOCK_MODE_UPDATE, .shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND}};
	size_t		need = txn_memtable_version_encoded(&change.writer, change.op, change.shown, change.has_lsn,
													change.lsn);

	for (uint64 number = 0; number < c->three->rows; number++)
	{
		uint8		key[8];

		std_u64_to_be_bytes(number, key);
		for (size_t k = 0; k < states->len; k++)
		{
			HostStorage tree = txn_spill_tests_tree(c->database, (uint32) k);
			TxnMemtableVersionHandle handle;
			uint64		touched;

			if (!txn_written_touched(io, xid.f0, &tree, TXN_WRITTEN_OF_ROWS, key, sizeof(key), need, &touched,
									 error) ||
				!txn_memtable_table_push(io, host_shared_Held_record(&STD_VEC_AT(states, HostSharedHeld, k)), key,
										 sizeof(key), &change, &handle, error))
				return false;
		}
	}
	return true;
}

/* txn_spill_tests_wrote is a writer's body in the three writers' case, its wrote closure: it writes its trees' rows, waits for the others or commits, and ends its writes. */
static bool
txn_spill_tests_wrote(const txn_spill_tests_writing_t *c, HostIoIo *io, HostError *error)
{
	txn_spill_tests_three_t *three = c->three;
	uint8		value[1000];
	StdVec		states;
	HostXid		xid;
	TxnMemtableOp op;
	bool		ok;

	std_vec_new(&states, sizeof(HostSharedHeld), _Alignof(HostSharedHeld));
	for (uint32 tree = 0; tree < three->trees_each; tree++)
	{
		HostStorage storage = txn_spill_tests_tree(c->database, tree);
		HostSharedHeld held;

		if (!txn_memtable_testing_held(io, &storage, &held, error))
		{
			txn_spill_tests_states_free(&states);
			return false;
		}
		std_vec_push_Checked(&states, &held, sizeof(held), _Alignof(HostSharedHeld));
	}
	xid = simc_begin();
	memset(value, 7, sizeof(value));
	op = txn_memtable_testing_put(value, sizeof(value));
	ok = txn_spill_tests_pushed(c, io, &states, xid, &op, error);
	txn_memtable_Op_drop(&op);
	txn_spill_tests_states_free(&states);
	if (!ok)
		return false;
	three->finished++;
	while (!three->folding && three->finished < lengthof(TXN_SPILL_TESTS_WRITERS))
	{
		if ((size_t) simc_delta_waiting() + three->finished >= lengthof(TXN_SPILL_TESTS_WRITERS))
		{
			*error = txn_spill_internal("every writer still writing sleeps at the budget");
			return false;
		}
		if (!host_pause(HOST_SHARED_IO_PAUSES(io), "doltgres-spill-case-written", error))
			return false;
	}
	if (three->folding)
	{
		TxnStatusOutcome committed = {.tag = TXN_STATUS_OUTCOME_COMMITTED};
		TxnStatusRoomed roomed;
		bool		took;

		if (!host_wal_insert(HOST_IO_IO_IO_DOORS(io), HOST_WAL_KIND_ROW_CHANGE, NULL, 0, &committed.u.committed,
							 error) ||
			!txn_status_take(io, xid, committed, &roomed, &took, error))
			return false;
		simc_commit(xid);
		std_vec_push_Checked(&three->committed, &c->database, sizeof(uint32), _Alignof(uint32));
	}
	if (!txn_written_ended(io, error))
		return false;
	three->ended++;
	return true;
}

/* txn_spill_tests_writer is a writer's actor in the three writers' case: its body, a failure marked in the shared state. */
static bool
txn_spill_tests_writer(void *captures, HostIoIo *io, HostError *error)
{
	const txn_spill_tests_writing_t *c = captures;
	bool		wrote = txn_spill_tests_wrote(c, io, error);

	c->three->failed |= !wrote;
	return wrote;
}

/* txn_spill_tests_folded folds each tree of each database committed, as the three writers' case's folder does when a writer waits. */
static bool
txn_spill_tests_folded(const txn_spill_tests_three_t *three, HostIoIo *io, const StdVec *all, HostError *error)
{
	const TreesKeyOrder order = {.tag = TREES_KEY_ORDER_BYTES};
	const TxnInsertPage page = {.bytes = 8168, .most = 291};

	for (size_t k = 0; k < all->len; k++)
		for (uint32 tree = 0; tree < three->trees_each; tree++)
		{
			HostStorage storage = txn_spill_tests_tree(STD_VEC_AT(all, uint32, k), tree);
			txn_spill_tests_based_t based;
			StdVec		warned;
			HostLsn		point;
			bool		has_point;
			bool		folded;
			bool		ok;

			if (!txn_spill_tests_based(io, &storage, &based, error))
				return false;
			std_vec_new(&warned, sizeof(StdString), _Alignof(StdString));
			ok = txn_fold_point(io, &has_point, &point, error) &&
				txn_fold_fold_now(io, &based.repo, &based.reading, &based.held, &order, false, has_point, point,
								  PG_UINT64_MAX, &warned, &page, NULL, &folded, error);
			std_vec_drop_Checked(&warned, sizeof(StdString), _Alignof(StdString), std_String_drop);
			txn_spill_tests_based_drop(&based);
			if (!ok)
				return false;
		}
	return true;
}

/* txn_spill_tests_folder is the three writers' case's folder: while a writer is unended, it folds the committed databases' trees whenever a writer waits at the budget and a database committed since, or every five thousand idle steps. */
static bool
txn_spill_tests_folder(void *captures, HostIoIo *io, HostError *error)
{
	txn_spill_tests_three_t *three = captures;
	size_t		folded = 0;
	int32		idle = 0;

	while (three->ended < lengthof(TXN_SPILL_TESTS_WRITERS))
	{
		StdVec		all;
		bool		fresh;
		bool		ok = true;

		if (three->failed)
		{
			*error = txn_spill_internal("a writer failed, so the fold has nothing to wait for");
			return false;
		}
		std_vec_clone(&all, &three->committed, sizeof(uint32), _Alignof(uint32));
		fresh = all.len > folded;
		idle = !fresh && simc_delta_waiting() > 0 ? idle + 1 : 0;
		if (idle > 100000)
		{
			std_vec_free_Checked(&all, sizeof(uint32), _Alignof(uint32));
			*error = txn_spill_internal("every writer still writing sleeps with nothing committed to fold");
			return false;
		}
		if (simc_delta_waiting() > 0 && (fresh || idle % 5000 == 4999))
		{
			folded = all.len;
			ok = txn_spill_tests_folded(three, io, &all, error);
		}
		std_vec_free_Checked(&all, sizeof(uint32), _Alignof(uint32));
		if (!ok || !host_pause(HOST_SHARED_IO_PAUSES(io), "doltgres-spill-case-folding", error))
			return false;
	}
	return true;
}

/*
 * txn_spill_tests_three_writers runs bugs row 124's three writers, each of a
 * count of trees of a count of rows: holding their commits until all three
 * have written, or, folding, each committing once written while a fold runs,
 * its commit at an empty record's end, since the simulation writes no commit
 * record, so the fold's point passes it.
 */
static void
txn_spill_tests_three_writers(uint32 trees_each, uint64 rows, bool folding)
{
	static const StdFmtPiece unfinished[] = {STD_FMT_LIT("the writers didn't finish, "), STD_FMT_ARG(0),
		STD_FMT_LIT(" row and "), STD_FMT_ARG(1), STD_FMT_LIT(" tuple-id entries in "), STD_FMT_ARG(2),
	STD_FMT_LIT(" bytes: "), STD_FMT_ARG(3)};
	static const StdFmtPiece outcome[] = {STD_FMT_ARG(0)};
	txn_spill_tests_three_t three = {.trees_each = trees_each, .rows = rows, .folding = folding};
	txn_spill_tests_writing_t writing[lengthof(TXN_SPILL_TESTS_WRITERS)];
	SimcEnding	returned = {.tag = SIMC_ENDING_RETURNED};
	SimcTestIo	test_io;
	HostIoIo   *io;
	SimcRun		run;
	SimcReport	report;
	HostDeltaHold hold;
	HostError	error;
	bool		all;

	simc_size_delta(2 << 20);
	test_io = simc_io();
	io = &test_io.io;
	for (size_t w = 0; w < lengthof(TXN_SPILL_TESTS_WRITERS); w++)
	{
		RepoRepo	repo;
		ChunksErrorError failed;

		if (!repo_Repo_create(io, TXN_SPILL_TESTS_WRITERS[w].database, HOST_PG_DEFAULT, &repo, &failed))
			STD_RESULT_EXPECT_FAILED(2215, 84, "called `Result::unwrap()` on an `Err` value", &failed,
									 chunks_error_Error_Debug_fmt);
		repo_Repo_drop(&repo);
		for (uint32 tree = 0; tree < trees_each; tree++)
		{
			HostStorage storage = txn_spill_tests_tree(TXN_SPILL_TESTS_WRITERS[w].database, tree);
			txn_spill_tests_based_t based;

			if (!txn_spill_tests_based(io, &storage, &based, &error))
				STD_RESULT_EXPECT_FAILED(2217, 39, "called `Result::unwrap()` on an `Err` value", &error,
										 host_Error_Debug_fmt);
			txn_spill_tests_based_drop(&based);
		}
	}
	std_vec_new(&three.committed, sizeof(uint32), _Alignof(uint32));
	run = simc_Run_new(1);
	for (size_t w = 0; w < lengthof(TXN_SPILL_TESTS_WRITERS); w++)
	{
		SimcActor	writer = {txn_spill_tests_writer, &writing[w]};

		writing[w] = (txn_spill_tests_writing_t)
		{
			&three, TXN_SPILL_TESTS_WRITERS[w].database
		};
		simc_Run_actor(&run, TXN_SPILL_TESTS_WRITERS[w].name, strlen(TXN_SPILL_TESTS_WRITERS[w].name), writer);
	}
	if (folding)
	{
		SimcActor	folder = {txn_spill_tests_folder, &three};

		simc_Run_actor(&run, "folder", 6, folder);
	}
	report = simc_Run_finish(run);
	if (report.outcome.tag != SIMC_OUTCOME_ENDED)
	{
		size_t		entries = 0;
		size_t		tuples = 0;
		uint64		used;

		for (size_t w = 0; w < lengthof(TXN_SPILL_TESTS_WRITERS); w++)
			for (uint32 tree = 0; tree < trees_each; tree++)
			{
				HostStorage storage = txn_spill_tests_tree(TXN_SPILL_TESTS_WRITERS[w].database, tree);
				HostSharedHeld state;
				StdVec		files;

				if (!txn_memtable_testing_held(io, &storage, &state, &error))
					STD_RESULT_EXPECT_FAILED(2317, 49, "called `Result::unwrap()` on an `Err` value", &error,
											 host_Error_Debug_fmt);
				files = txn_spill_tests_files_of(io, host_shared_Held_record(&state));
				for (size_t k = 0; k < files.len; k++)
				{
					if (STD_VEC_AT(&files, TxnSpillFile, k).kind != TXN_RUNS_KIND_SPILLED_TUPLES)
						entries++;
					if (STD_VEC_AT(&files, TxnSpillFile, k).kind == TXN_RUNS_KIND_SPILLED_TUPLES)
						tuples++;
				}
				std_vec_free_Checked(&files, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
				host_shared_Held_drop(&state);
			}
		used = host_delta_used();
		{
			StdFmtArg	args[4] = {{&entries, std_usize_Display_fmt}, {&tuples, std_usize_Display_fmt},
			{&used, std_u64_Display_fmt}, {&report.outcome, simc_Outcome_Debug_fmt}};
			StdFmtArguments message = {unfinished, lengthof(unfinished), args};

			std_panic_fmt(RUST_FILE, 2323, 13, &message);
		}
	}
	all = true;
	for (size_t k = 0; all && k < report.outcome.u.ended.len; k++)
		all = simc_Ending_PartialEq_eq(&STD_VEC_AT(&report.outcome.u.ended, simc_TupleStringEnding_t, k).f1,
									   &returned);
	if (!all)
	{
		StdFmtArg	args[1] = {{&report.outcome, simc_Outcome_Debug_fmt}};
		StdFmtArguments message = {outcome, lengthof(outcome), args};

		std_panic_fmt(RUST_FILE, 2329, 9, &message);
	}
	if (!folding)
	{
		if (!host_delta_hold(io, &hold, &error))
			STD_RESULT_EXPECT_FAILED(2333, 36, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		for (size_t w = 0; w < lengthof(TXN_SPILL_TESTS_WRITERS); w++)
			for (uint32 tree = 0; tree < trees_each; tree++)
			{
				HostStorage storage = txn_spill_tests_tree(TXN_SPILL_TESTS_WRITERS[w].database, tree);
				HostSharedHeld state;
				uint64		numbers[2] = {0, rows - 1};

				if (!txn_memtable_testing_held(io, &storage, &state, &error))
					STD_RESULT_EXPECT_FAILED(2336, 45, "called `Result::unwrap()` on an `Err` value", &error,
											 host_Error_Debug_fmt);
				for (size_t k = 0; k < lengthof(numbers); k++)
					txn_spill_tests_one_version(2338, 21, 98, io, &hold, host_shared_Held_record(&state), numbers[k],
												false);
				host_shared_Held_drop(&state);
			}
		host_delta_Hold_drop(&hold);
	}
	std_vec_free_Checked(&three.committed, sizeof(uint32), _Alignof(uint32));
	simc_Report_drop(&report);
	simc_TestIo_drop(&test_io);
}

/*
 * Bugs row 124's three writers: three transactions each write many small
 * trees a row at a time, interleaved at every yield point by the run's seed,
 * so their trees' small pages share the delta's blocks, and each writes past
 * a third of the budget. A writer refused at it spills its trees one after
 * another, and each part's removal frees pages whose blocks the others' pages
 * keep, so no block frees; each part's entry takes the slot its own removal
 * claimed. No writer commits until all three have written, so every version
 * is a waiter's to spill, and every writer finishes with its rows read back.
 */
void
test_txn_spill_tests_three_writers_of_small_trees_past_a_third_of_the_budget_each_spill_and_finish(void)
{
	txn_spill_tests_three_writers(16, 40, false);
}

/*
 * Bugs row 124's committed load: the three writers of twice the trees of half
 * the rows each commit once written, so the budget the last one waits at is
 * held by committed versions no waiter can spill; a fold of the committed
 * storages, which an actor runs while a writer waits, as the server's fold
 * trigger does, again while the writer still waits, since a fold refused by
 * an older snapshot installs later, frees them, and every writer finishes.
 */
void
test_txn_spill_tests_three_writers_waiting_on_committed_versions_finish_once_a_fold_installs(void)
{
	txn_spill_tests_three_writers(32, 20, true);
}

/* txn_spill_tests_returned asserts at its lines that a run's actors all ended, panicking with its lead-in and the outcome when the run didn't end, and that each returned with no error. */
static void
txn_spill_tests_returned(const SimcReport *report, uint32 line, uint32 col, const StdFmtPiece lead[2],
						 uint32 all_line)
{
	static const StdFmtPiece outcome[] = {STD_FMT_ARG(0)};
	SimcEnding	returned = {.tag = SIMC_ENDING_RETURNED};
	StdFmtArg	args[1] = {{&report->outcome, simc_Outcome_Debug_fmt}};
	bool		all = true;

	if (report->outcome.tag != SIMC_OUTCOME_ENDED)
	{
		StdFmtArguments message = {lead, 2, args};

		std_panic_fmt(RUST_FILE, line, col, &message);
	}
	for (size_t k = 0; all && k < report->outcome.u.ended.len; k++)
		all = simc_Ending_PartialEq_eq(&STD_VEC_AT(&report->outcome.u.ended, simc_TupleStringEnding_t, k).f1,
									   &returned);
	if (!all)
	{
		StdFmtArguments message = {outcome, lengthof(outcome), args};

		std_panic_fmt(RUST_FILE, all_line, 9, &message);
	}
}

/* txn_spill_tests_fill takes the delta's pages of six kilobytes until it refuses one, as the spill cases' fillers do. */
static bool
txn_spill_tests_fill(HostIoIo *io, StdVec *pages, HostError *error)
{
	HostDeltaPageSize size = {.bytes = 6 << 10, .words = 256};

	for (;;)
	{
		StdVec		taken;
		HostDeltaFull full;
		bool		ok;

		if (!host_delta_alloc(io, &size, 1, &ok, &taken, &full, error))
			return false;
		if (!ok)
			return true;
		std_vec_extend_from_slice_Checked(pages, taken.ptr, taken.len, sizeof(HostDeltaOwned),
										  _Alignof(HostDeltaOwned));
		std_vec_free_Checked(&taken, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned));
	}
}

/* txn_spill_tests_one_range_writer is the one-range case's writer: it writes its few rows, spills them at the filled budget, and ends its writes. */
static bool
txn_spill_tests_one_range_writer(void *captures, HostIoIo *io, HostError *error)
{
	HostStorage storage = txn_memtable_testing_storage(9488);
	uint8		value[1000];
	HostSharedHeld held;
	TxnMemtableOp op;
	uint64		xid;
	bool		ok = true;
	bool		spilled;

	if (!txn_memtable_testing_stated(io, 9488, &held, error))
		return false;
	xid = simc_begin().f0;
	memset(value, 7, sizeof(value));
	op = txn_memtable_testing_put(value, sizeof(value));
	for (uint64 number = 0; ok && number < 5; number++)
	{
		TxnMemtableVersionChange change = {.writer = txn_memtable_testing_writer(xid), .op = &op,
		.lock = TXN_MEMTABLE_LOCK_MODE_UPDATE, .shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND}};
		size_t		need = txn_memtable_version_encoded(&change.writer, change.op, change.shown, change.has_lsn,
														change.lsn);
		TxnMemtableVersionHandle handle;
		uint64		touched;
		uint8		key[8];

		std_u64_to_be_bytes(number, key);
		ok = txn_written_touched(io, xid, &storage, TXN_WRITTEN_OF_ROWS, key, sizeof(key), need, &touched, error) &&
			txn_memtable_table_push(io, host_shared_Held_record(&held), key, sizeof(key), &change, &handle, error);
	}
	ok = ok && host_pause(HOST_SHARED_IO_PAUSES(io), "doltgres-spill-case-written", error) &&
		txn_spill_spilled(io, &storage, TXN_WRITTEN_OF_ROWS, xid, &spilled, error);
	if (ok)
		STD_ASSERT(2368, 13, spilled, "spilled(io, &storage(9488), Of::Rows, xid)?");
	ok = ok && host_pause(HOST_SHARED_IO_PAUSES(io), "doltgres-spill-case-spilled", error) &&
		txn_written_ended(io, error);
	txn_memtable_Op_drop(&op);
	host_shared_Held_drop(&held);
	return ok;
}

/* txn_spill_tests_filler is the one-range case's filler: it fills the delta, waits at a pause, and frees its pages. */
static bool
txn_spill_tests_filler(void *captures, HostIoIo *io, HostError *error)
{
	StdVec		pages;
	bool		ok;

	std_vec_new(&pages, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned));
	ok = txn_spill_tests_fill(io, &pages, error) &&
		host_pause(HOST_SHARED_IO_PAUSES(io), "doltgres-spill-case-filled", error);
	std_vec_drop_Checked(&pages, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned), host_delta_Owned_drop);
	return ok;
}

/*
 * Bugs row 124's one-range tree: a tree of a few rows, its one range the
 * memtable's floor, spills at a delta another process filled; its manifest
 * takes none of the delta (bugs row 143), so the spill lists it at once.
 */
void
test_txn_spill_tests_a_one_range_tree_spills_at_a_full_budget(void)
{
	static const StdFmtPiece lead[] = {STD_FMT_LIT("the spill didn't link: "), STD_FMT_ARG(0)};
	SimcStep	steps[2] = {{"filler", 6, PG_UINT64_MAX}, {"writer", 6, PG_UINT64_MAX}};
	SimcActor	writer = {txn_spill_tests_one_range_writer};
	SimcActor	filler = {txn_spill_tests_filler};
	SimcTestIo	test_io;
	HostIoIo   *io;
	HostSharedHeld held;
	SimcRun		run;
	SimcReport	report;
	HostDeltaHold hold;
	HostError	error;

	simc_size_delta(2 << 20);
	test_io = simc_io();
	io = &test_io.io;
	held = txn_memtable_testing_created(io, 9488);
	host_shared_Held_drop(&held);
	run = simc_Run_new(1);
	simc_Run_actor(&run, "writer", 6, writer);
	simc_Run_actor(&run, "filler", 6, filler);
	simc_Run_until(&run, "writer", 6, "doltgres-spill-case-written");
	simc_Run_until(&run, "filler", 6, "doltgres-spill-case-filled");
	simc_Run_until(&run, "writer", 6, "doltgres-spill-case-spilled");
	simc_Run_script(&run, steps, lengthof(steps));
	report = simc_Run_finish(run);
	txn_spill_tests_returned(&report, 2387, 13, lead, 2389);
	if (!txn_memtable_testing_stated(io, 9488, &held, &error))
		STD_RESULT_EXPECT_FAILED(2390, 37, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!host_delta_hold(io, &hold, &error))
		STD_RESULT_EXPECT_FAILED(2391, 36, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	for (uint64 number = 0; number < 5; number++)
		txn_spill_tests_one_version(2393, 13, 89, io, &hold, host_shared_Held_record(&held), number, false);
	host_delta_Hold_drop(&hold);
	host_shared_Held_drop(&held);
	simc_Report_drop(&report);
	simc_TestIo_drop(&test_io);
}

/* txn_spill_tests_tupled_rows writes a transaction's rows of a range of numbers to a state's memtable, each counted in its written trees with no size and each a put whose tuple id is its number plus one. */
static void
txn_spill_tests_tupled_rows(uint32 touch_line, uint32 touch_col, uint32 push_line, uint32 push_col, HostIoIo *io,
							HostSharedRecord state, const HostStorage *storage, uint64 xid, uint64 from, uint64 to)
{
	HostError	error;

	for (uint64 number = from; number < to; number++)
	{
		TxnMemtableOp op = txn_memtable_testing_put((const uint8 *) "v", 1);
		TxnMemtableVersionChange change = {.writer = txn_memtable_testing_writer(xid), .op = &op,
		.lock = TXN_MEMTABLE_LOCK_MODE_UPDATE, .shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND}};
		TxnMemtableVersionHandle handle;
		uint64		touched;
		uint8		key[8];

		std_u64_to_be_bytes(number, key);
		if (!txn_written_touched(io, xid, storage, TXN_WRITTEN_OF_ROWS, key, sizeof(key), 0, &touched, &error))
			STD_RESULT_EXPECT_FAILED(touch_line, touch_col, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		op.u.put.tuple.f0 = number + 1;
		if (!txn_memtable_table_push(io, state, key, sizeof(key), &change, &handle, &error))
			STD_RESULT_EXPECT_FAILED(push_line, push_col, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		txn_memtable_Op_drop(&op);
	}
}

/* txn_spill_tests_kind_number_t is a spill file's kind and number, as the merge case lists them. */
typedef struct txn_spill_tests_kind_number_t
{
	TxnRunsKind kind;
	uint32		number;
} txn_spill_tests_kind_number_t;

/* txn_spill_tests_kind_numbers_Debug_fmt is a Vec<(Kind, u32)>'s Debug. */
static bool
txn_spill_tests_kind_numbers_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *kinds = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t k = 0; k < kinds->len; k++)
	{
		const txn_spill_tests_kind_number_t *kind = &STD_VEC_AT(kinds, txn_spill_tests_kind_number_t, k);
		StdTupleRef tuple = STD_TUPLE(2, STD_ITEM(&kind->kind, txn_spill_tests_Kind_Debug_fmt),
									  STD_ITEM(&kind->number, std_u32_Debug_fmt));

		std_DebugList_entry(&list, &tuple, std_tuple_Debug_fmt);
	}
	return std_DebugList_finish(&list);
}

/* txn_spill_tests_tuple_key_is asserts at its line that a number's row's tuple id, its number plus one, finds the number's key, the message the number. */
static void
txn_spill_tests_tuple_key_is(uint32 line, HostIoIo *io, const HostDeltaHold *hold, HostSharedRecord state,
							 uint64 number)
{
	static const StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
	HostRow		tid = {number + 1};
	TxnMemtableMapEntry found;
	TxnMemtableMapEntry key = {.has_value = true};
	HostError	error;
	uint8		be[8];

	if (!txn_spill_tuple_key(io, hold, state, tid, &found.has_value, &found.value, &error))
		STD_RESULT_EXPECT_FAILED(line, 69, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	std_u64_to_be_bytes(number, be);
	key.value = (StdVec)
	{
		be, sizeof(be), sizeof(be)
	};
	if (!txn_memtable_map_tests_seen_eq(&found, &key))
	{
		StdFmtArg	args[1] = {{&number, std_u64_Display_fmt}};
		StdFmtArguments message = {pieces, lengthof(pieces), args};

		std_assert_failed(RUST_FILE, line, 13, "==", &found, txn_memtable_map_tests_seen_Debug_fmt, &key,
						  txn_memtable_map_tests_seen_Debug_fmt, &message);
	}
	txn_memtable_map_tests_seen_drop(&found);
}

/*
 * Bugs row 124's merge of tuple ids: a transaction's two spills of one tree,
 * the second as large as the first, merge into one row file, and the tuple-id
 * files their entries name merge into one of the same number, which the
 * merged entry names; every row's tuple id still finds its key.
 */
void
test_txn_spill_tests_a_merge_of_row_spills_merges_the_tuple_ids_their_entries_name(void)
{
	static const uint64 numbers[] = {0, 99, 100, 199};
	txn_spill_tests_kind_number_t wanted[] = {{TXN_RUNS_KIND_SPILLED_TUPLES, 2}, {TXN_RUNS_KIND_SPILLED_ROWS, 2}};
	StdVec		wanted_vec = {wanted, lengthof(wanted), lengthof(wanted)};
	SimcTestIo	test_io;
	HostIoIo   *io;
	HostSharedHeld held;
	HostSharedRecord state;
	HostStorage storage = txn_memtable_testing_storage(9489);
	StdVec		files;
	StdVec		kinds;
	HostDeltaHold hold;
	HostError	error;
	uint64		xid;
	bool		spilled;
	bool		eq;

	simc_size_delta(64 << 20);
	test_io = simc_io();
	io = &test_io.io;
	held = txn_memtable_testing_created(io, 9489);
	state = host_shared_Held_record(&held);
	xid = simc_begin().f0;
	for (uint64 half = 0; half < 2; half++)
	{
		txn_spill_tests_tupled_rows(2412, 78, 2418, 55, io, state, &storage, xid, half * 100, half * 100 + 100);
		if (!txn_spill_spilled(io, &storage, TXN_WRITTEN_OF_ROWS, xid, &spilled, &error))
			STD_RESULT_EXPECT_FAILED(2420, 64, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		STD_ASSERT(2420, 13, spilled, "spilled(io, &storage(9489), Of::Rows, xid).unwrap()");
	}
	files = txn_spill_tests_files_of(io, state);
	std_vec_with_capacity_Checked(&kinds, files.len, sizeof(txn_spill_tests_kind_number_t),
								  _Alignof(txn_spill_tests_kind_number_t));
	for (size_t k = 0; k < files.len; k++)
	{
		const TxnSpillFile *file = &STD_VEC_AT(&files, TxnSpillFile, k);
		txn_spill_tests_kind_number_t kind = {file->kind, file->number};

		std_vec_push_Checked(&kinds, &kind, sizeof(kind), _Alignof(txn_spill_tests_kind_number_t));
	}
	if (!host_delta_hold(io, &hold, &error))
		STD_RESULT_EXPECT_FAILED(2424, 36, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	eq = kinds.len == lengthof(wanted);
	for (size_t k = 0; eq && k < lengthof(wanted); k++)
		eq = STD_VEC_AT(&kinds, txn_spill_tests_kind_number_t, k).kind == wanted[k].kind &&
			STD_VEC_AT(&kinds, txn_spill_tests_kind_number_t, k).number == wanted[k].number;
	STD_ASSERT_EQ(2425, 9, eq, &kinds, txn_spill_tests_kind_numbers_Debug_fmt, &wanted_vec,
				  txn_spill_tests_kind_numbers_Debug_fmt);
	for (size_t k = 0; k < lengthof(numbers); k++)
		txn_spill_tests_tuple_key_is(2428, io, &hold, state, numbers[k]);
	host_delta_Hold_drop(&hold);
	if (!txn_written_ended(io, &error))
		STD_RESULT_EXPECT_FAILED(2431, 28, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	std_vec_free_Checked(&kinds, sizeof(txn_spill_tests_kind_number_t), _Alignof(txn_spill_tests_kind_number_t));
	std_vec_free_Checked(&files, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/*
 * Bugs row 143's merge at a full budget: a transaction's two small spills of
 * one tree at a delta another process filled merge into one file, listed in
 * place of the two with no page of the delta, and every key reads back
 * through it.
 */
void
test_txn_spill_tests_a_merge_at_a_full_budget_lists_its_file_in_place_of_its_sources(void)
{
	static const uint64 numbers[] = {0, 19};
	static const TxnRunsKind wanted[] = {TXN_RUNS_KIND_SPILLED_TUPLES, TXN_RUNS_KIND_SPILLED_ROWS};
	SimcTestIo	test_io;
	HostIoIo   *io;
	HostSharedHeld held;
	HostSharedRecord state;
	HostStorage storage = txn_memtable_testing_storage(9490);
	StdVec		filler;
	HostDeltaHold hold;
	StdVec		files;
	HostError	error;
	uint64		xid;
	bool		spilled;

	simc_size_delta(2 << 20);
	test_io = simc_io();
	io = &test_io.io;
	held = txn_memtable_testing_created(io, 9490);
	state = host_shared_Held_record(&held);
	xid = simc_begin().f0;
	std_vec_new(&filler, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned));
	for (uint64 half = 0; half < 2; half++)
	{
		txn_spill_tests_tupled_rows(2450, 78, 2456, 55, io, state, &storage, xid, half * 10, half * 10 + 10);
		if ((filler.len > 0 || half * 10 > 0) && !txn_spill_tests_fill(io, &filler, &error))
			STD_RESULT_EXPECT_FAILED(2459, 96, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		if (!txn_spill_spilled(io, &storage, TXN_WRITTEN_OF_ROWS, xid, &spilled, &error))
			STD_RESULT_EXPECT_FAILED(2463, 64, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		STD_ASSERT(2463, 13, spilled, "spilled(io, &storage(9490), Of::Rows, xid).unwrap()");
	}
	files = txn_spill_tests_files_of(io, state);
	txn_spill_tests_kinds_are(2466, &files, wanted, lengthof(wanted));
	std_vec_free_Checked(&files, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	if (!host_delta_hold(io, &hold, &error))
		STD_RESULT_EXPECT_FAILED(2467, 36, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	for (size_t k = 0; k < lengthof(numbers); k++)
	{
		txn_spill_tests_one_version(2470, 13, 64, io, &hold, state, numbers[k], true);
		txn_spill_tests_tuple_key_is(2471, io, &hold, state, numbers[k]);
	}
	host_delta_Hold_drop(&hold);
	std_vec_drop_Checked(&filler, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned), host_delta_Owned_drop);
	if (!txn_written_ended(io, &error))
		STD_RESULT_EXPECT_FAILED(2474, 28, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/* txn_spill_tests_merger is the merge window case's merger: its transaction's second spill, which merges with its first. */
static bool
txn_spill_tests_merger(void *captures, HostIoIo *io, HostError *error)
{
	HostStorage storage = txn_memtable_testing_storage(9491);
	bool		spilled;

	return txn_spill_spilled(io, &storage, TXN_WRITTEN_OF_ROWS, *(const uint64 *) captures, &spilled, error);
}

/* txn_spill_tests_found_t is the merge window case's reader's finding: whether it read, and its count of the first key's versions. */
typedef struct txn_spill_tests_found_t
{
	bool		some;
	size_t		count;
} txn_spill_tests_found_t;

/* txn_spill_tests_found_Debug_fmt is the reader's finding's Debug, an Option<usize>. */
static bool
txn_spill_tests_found_Debug_fmt(const void *value, StdFormatter *f)
{
	const txn_spill_tests_found_t *found = value;
	StdOptionRef option = {found->some, STD_ITEM(&found->count, std_usize_Debug_fmt)};

	return std_option_Debug_fmt(&option, f);
}

/* txn_spill_tests_window_reader is the merge window case's reader: it counts the first key's versions. */
static bool
txn_spill_tests_window_reader(void *captures, HostIoIo *io, HostError *error)
{
	txn_spill_tests_found_t *found = captures;
	HostSharedHeld held;
	HostDeltaHold hold;
	StdVec		versions;
	uint8		key[8];
	bool		ok;

	if (!txn_memtable_testing_stated(io, 9491, &held, error))
		return false;
	if (!host_delta_hold(io, &hold, error))
	{
		host_shared_Held_drop(&held);
		return false;
	}
	std_u64_to_be_bytes(0, key);
	ok = txn_memtable_table_versions(io, &hold, host_shared_Held_record(&held), key, sizeof(key), &versions, error);
	if (ok)
	{
		*found = (txn_spill_tests_found_t)
		{
			true, versions.len
		};
		txn_spill_tests_versions_free(&versions);
	}
	host_delta_Hold_drop(&hold);
	host_shared_Held_drop(&held);
	return ok;
}

/*
 * Bugs row 124's merge window: a reader that read the slots before a merge
 * pended its file, and lists the chain after the merge unchained its sources
 * but before it linked the merged file, sees the chain changed, starts over,
 * and reads the key's version once, through the pended merged file.
 */
void
test_txn_spill_tests_a_reader_across_a_merges_unchain_reads_each_version_once(void)
{
	static const StdFmtPiece lead[] = {STD_FMT_LIT("the run didn't end: "), STD_FMT_ARG(0)};
	SimcStep	steps[2] = {{"reader", 6, PG_UINT64_MAX}, {"merger", 6, PG_UINT64_MAX}};
	txn_spill_tests_found_t found = {0};
	txn_spill_tests_found_t once = {true, 1};
	SimcTestIo	test_io;
	HostIoIo   *io;
	HostSharedHeld held;
	HostStorage storage = txn_memtable_testing_storage(9491);
	SimcActor	merger = {txn_spill_tests_merger};
	SimcActor	reader = {txn_spill_tests_window_reader, &found};
	SimcRun		run;
	SimcReport	report;
	HostError	error;
	uint64		xid;
	bool		spilled;

	simc_size_delta(64 << 20);
	test_io = simc_io();
	io = &test_io.io;
	held = txn_memtable_testing_created(io, 9491);
	xid = simc_begin().f0;
	for (uint64 half = 0; half < 2; half++)
	{
		for (uint64 number = half * 10; number < half * 10 + 10; number++)
		{
			TxnMemtableOp op = txn_memtable_testing_put((const uint8 *) "v", 1);
			TxnMemtableVersionChange change = {.writer = txn_memtable_testing_writer(xid), .op = &op,
			.lock = TXN_MEMTABLE_LOCK_MODE_UPDATE, .shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND}};
			TxnMemtableVersionHandle handle;
			uint64		touched;
			uint8		key[8];

			std_u64_to_be_bytes(number, key);
			if (!txn_written_touched(io, xid, &storage, TXN_WRITTEN_OF_ROWS, key, sizeof(key), 0, &touched, &error))
				STD_RESULT_EXPECT_FAILED(2491, 78, "called `Result::unwrap()` on an `Err` value", &error,
										 host_Error_Debug_fmt);
			if (!txn_memtable_table_push(io, host_shared_Held_record(&held), key, sizeof(key), &change, &handle,
										 &error))
				STD_RESULT_EXPECT_FAILED(2499, 63, "called `Result::unwrap()` on an `Err` value", &error,
										 host_Error_Debug_fmt);
			txn_memtable_Op_drop(&op);
		}
		if (half == 0)
		{
			if (!txn_spill_spilled(io, &storage, TXN_WRITTEN_OF_ROWS, xid, &spilled, &error))
				STD_RESULT_EXPECT_FAILED(2502, 68, "called `Result::unwrap()` on an `Err` value", &error,
										 host_Error_Debug_fmt);
			STD_ASSERT(2502, 17, spilled, "spilled(io, &storage(9491), Of::Rows, xid).unwrap()");
		}
	}
	host_shared_Held_drop(&held);
	merger.captures = &xid;
	run = simc_Run_new(1);
	simc_Run_actor(&run, "merger", 6, merger);
	simc_Run_actor(&run, "reader", 6, reader);
	simc_Run_until(&run, "merger", 6, "doltgres-spill-linked");
	simc_Run_until(&run, "reader", 6, "doltgres-spill-slots-read");
	simc_Run_until(&run, "merger", 6, "doltgres-spill-merge-unchained");
	simc_Run_script(&run, steps, lengthof(steps));
	report = simc_Run_finish(run);
	txn_spill_tests_returned(&report, 2521, 67, lead, 2522);
	STD_ASSERT_EQ(2523, 9, found.some && found.count == 1, &found, txn_spill_tests_found_Debug_fmt, &once,
				  txn_spill_tests_found_Debug_fmt);
	if (!txn_written_ended(io, &error))
		STD_RESULT_EXPECT_FAILED(2524, 28, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	simc_Report_drop(&report);
	simc_TestIo_drop(&test_io);
}

/* txn_spill_tests_paused_wrote writes a transaction's rows of a count to each of a database's nine trees, a tree at a time, as the paused holder case's wrote closure does. */
static bool
txn_spill_tests_paused_wrote(HostIoIo *io, uint32 database, uint64 rows, HostError *error)
{
	uint64		xid = simc_begin().f0;
	uint8		value[1000];
	TxnMemtableOp op;
	bool		ok = true;

	memset(value, 7, sizeof(value));
	op = txn_memtable_testing_put(value, sizeof(value));
	for (uint32 tree = 0; ok && tree < 9; tree++)
	{
		HostStorage storage = txn_spill_tests_tree(database, tree);
		HostSharedHeld state;

		if (!txn_memtable_testing_held(io, &storage, &state, error))
		{
			ok = false;
			break;
		}
		for (uint64 number = 0; ok && number < rows; number++)
		{
			TxnMemtableVersionChange change = {.writer = txn_memtable_testing_writer(xid), .op = &op,
			.lock = TXN_MEMTABLE_LOCK_MODE_UPDATE, .shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND}};
			size_t		need = txn_memtable_version_encoded(&change.writer, change.op, change.shown,
															change.has_lsn, change.lsn);
			TxnMemtableVersionHandle handle;
			uint64		touched;
			uint8		key[8];

			std_u64_to_be_bytes(number, key);
			ok = txn_written_touched(io, xid, &storage, TXN_WRITTEN_OF_ROWS, key, sizeof(key), need, &touched,
									 error) &&
				txn_memtable_table_push(io, host_shared_Held_record(&state), key, sizeof(key), &change, &handle,
										error);
		}
		host_shared_Held_drop(&state);
	}
	txn_memtable_Op_drop(&op);
	return ok;
}

/* txn_spill_tests_holder is the paused holder case's holder: it writes its trees, waits at a pause, and ends its writes. */
static bool
txn_spill_tests_holder(void *captures, HostIoIo *io, HostError *error)
{
	return txn_spill_tests_paused_wrote(io, 9492, 190, error) &&
		host_pause(HOST_SHARED_IO_PAUSES(io), "doltgres-spill-case-held", error) &&
		txn_written_ended(io, error);
}

/* txn_spill_tests_relieving_writer is the paused holder case's writer: its relief spills the paused transaction's versions. */
static bool
txn_spill_tests_relieving_writer(void *captures, HostIoIo *io, HostError *error)
{
	bool		relieved;

	if (!txn_spill_relieved(io, &relieved, error))
		return false;
	STD_ASSERT_MSG(2570, 13, relieved, "the paused transaction's versions spill");
	return true;
}

/*
 * Bugs row 144's paused holder: a transaction that wrote trees each under the
 * spill threshold waits at a pause before its commit, not at the budget, so
 * it spills nothing itself; a process with nothing of its own to spill
 * relieves the budget by spilling the paused transaction's versions from
 * every tree, and the paused one's rows read back after its commit.
 */
void
test_txn_spill_tests_a_process_with_nothing_of_its_own_spills_a_paused_transactions_versions(void)
{
	static const StdFmtPiece lead[] = {STD_FMT_LIT("the writer didn't finish: "), STD_FMT_ARG(0)};
	SimcStep	steps[2] = {{"writer", 6, PG_UINT64_MAX}, {"holder", 6, PG_UINT64_MAX}};
	SimcActor	holder = {txn_spill_tests_holder};
	SimcActor	writer = {txn_spill_tests_relieving_writer};
	SimcTestIo	test_io;
	HostIoIo   *io;
	HostSharedHeld held;
	SimcRun		run;
	SimcReport	report;
	HostDeltaHold hold;
	HostError	error;

	simc_size_delta(2 << 20);
	test_io = simc_io();
	io = &test_io.io;
	held = txn_memtable_testing_created(io, 9492);
	host_shared_Held_drop(&held);
	run = simc_Run_new(1);
	simc_Run_actor(&run, "holder", 6, holder);
	simc_Run_actor(&run, "writer", 6, writer);
	simc_Run_until(&run, "holder", 6, "doltgres-spill-case-held");
	simc_Run_script(&run, steps, lengthof(steps));
	report = simc_Run_finish(run);
	txn_spill_tests_returned(&report, 2577, 13, lead, 2579);
	if (!host_delta_hold(io, &hold, &error))
		STD_RESULT_EXPECT_FAILED(2580, 36, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	for (uint32 tree = 0; tree < 9; tree++)
	{
		HostStorage storage = txn_spill_tests_tree(9492, tree);
		HostSharedHeld state;
		StdVec		files;

		if (!txn_memtable_testing_held(io, &storage, &state, &error))
			STD_RESULT_EXPECT_FAILED(2582, 41, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		files = txn_spill_tests_files_of(io, host_shared_Held_record(&state));
		STD_ASSERT_MSG(2583, 13, files.len > 0, "every tree spilled");
		std_vec_free_Checked(&files, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
		txn_spill_tests_one_version(2585, 17, 94, io, &hold, host_shared_Held_record(&state), 0, false);
		txn_spill_tests_one_version(2585, 17, 94, io, &hold, host_shared_Held_record(&state), 189, false);
		host_shared_Held_drop(&state);
	}
	host_delta_Hold_drop(&hold);
	simc_Report_drop(&report);
	simc_TestIo_drop(&test_io);
}

/* txn_spill_tests_owner is the turn case's owner: it writes its rows, and its own spill at the pause spills nothing on the helper's turn. */
static bool
txn_spill_tests_owner(void *captures, HostIoIo *io, HostError *error)
{
	HostStorage storage = txn_memtable_testing_storage(9494);
	HostSharedHeld state;
	uint64		xid;
	bool		ok = true;
	bool		spilled;

	if (!txn_memtable_testing_stated(io, 9494, &state, error))
		return false;
	xid = simc_begin().f0;
	for (uint64 number = 0; ok && number < 50; number++)
	{
		TxnMemtableOp op = txn_memtable_testing_put((const uint8 *) "v", 1);
		TxnMemtableVersionChange change = {.writer = txn_memtable_testing_writer(xid), .op = &op,
		.lock = TXN_MEMTABLE_LOCK_MODE_UPDATE, .shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND}};
		size_t		need = txn_memtable_version_encoded(&change.writer, change.op, change.shown, change.has_lsn,
														change.lsn);
		TxnMemtableVersionHandle handle;
		uint64		touched;
		uint8		key[8];

		std_u64_to_be_bytes(number, key);
		ok = txn_written_touched(io, xid, &storage, TXN_WRITTEN_OF_ROWS, key, sizeof(key), need, &touched, error) &&
			txn_memtable_table_push(io, host_shared_Held_record(&state), key, sizeof(key), &change, &handle, error);
		txn_memtable_Op_drop(&op);
	}
	ok = ok && host_pause(HOST_SHARED_IO_PAUSES(io), "doltgres-spill-case-written", error) &&
		txn_spill_spilled(io, &storage, TXN_WRITTEN_OF_ROWS, xid, &spilled, error);
	if (ok)
		STD_ASSERT_MSG(2615, 13, !spilled, "the owner spills nothing on the helper's turn");
	ok = ok && txn_written_ended(io, error);
	host_shared_Held_drop(&state);
	return ok;
}

/* txn_spill_tests_helper is the turn case's helper: its relief spills the owner's transaction's versions. */
static bool
txn_spill_tests_helper(void *captures, HostIoIo *io, HostError *error)
{
	bool		relieved;

	if (!txn_spill_relieved(io, &relieved, error))
		return false;
	STD_ASSERT(2619, 13, relieved, "relieved(io)?");
	return true;
}

/*
 * Bugs row 144's turn: while a helper spills a transaction's versions, held
 * inside its spill, the transaction's owner asks to spill its own tree and
 * spills nothing, the turn being the helper's; once the helper is done, the
 * transaction's files have distinct numbers and every key reads once.
 */
void
test_txn_spill_tests_an_owner_spills_nothing_while_a_helper_holds_its_transactions_turn(void)
{
	static const StdFmtPiece lead[] = {STD_FMT_LIT("the run didn't end: "), STD_FMT_ARG(0)};
	SimcStep	steps[2] = {{"owner", 5, PG_UINT64_MAX}, {"helper", 6, PG_UINT64_MAX}};
	SimcActor	owner = {txn_spill_tests_owner};
	SimcActor	helper = {txn_spill_tests_helper};
	SimcTestIo	test_io;
	HostIoIo   *io;
	HostSharedHeld held;
	SimcRun		run;
	SimcReport	report;
	HostDeltaHold hold;
	HostError	error;
	StdVec		files;
	size_t		count;
	size_t		distinct = 0;

	simc_size_delta(64 << 20);
	test_io = simc_io();
	io = &test_io.io;
	held = txn_memtable_testing_created(io, 9494);
	host_shared_Held_drop(&held);
	run = simc_Run_new(1);
	simc_Run_actor(&run, "owner", 5, owner);
	simc_Run_actor(&run, "helper", 6, helper);
	simc_Run_until(&run, "owner", 5, "doltgres-spill-case-written");
	simc_Run_until(&run, "helper", 6, "doltgres-spill-pended");
	simc_Run_script(&run, steps, lengthof(steps));
	report = simc_Run_finish(run);
	txn_spill_tests_returned(&report, 2626, 67, lead, 2627);
	if (!txn_memtable_testing_stated(io, 9494, &held, &error))
		STD_RESULT_EXPECT_FAILED(2628, 38, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	files = txn_spill_tests_files_of(io, host_shared_Held_record(&held));
	if (!host_delta_hold(io, &hold, &error))
		STD_RESULT_EXPECT_FAILED(2631, 36, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	count = files.len;
	for (size_t k = 0; k < files.len; k++)
	{
		const TxnSpillFile *file = &STD_VEC_AT(&files, TxnSpillFile, k);
		bool		seen = false;

		for (size_t j = 0; !seen && j < k; j++)
			seen = STD_VEC_AT(&files, TxnSpillFile, j).kind == file->kind &&
				STD_VEC_AT(&files, TxnSpillFile, j).number == file->number;
		if (!seen)
			distinct++;
	}
	std_vec_free_Checked(&files, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	STD_ASSERT_EQ_MSG(2635, 9, distinct == count, &distinct, std_usize_Debug_fmt, &count, std_usize_Debug_fmt,
					  "no two files share a number");
	txn_spill_tests_one_version(2637, 13, 90, io, &hold, host_shared_Held_record(&held), 0, false);
	txn_spill_tests_one_version(2637, 13, 90, io, &hold, host_shared_Held_record(&held), 49, false);
	host_delta_Hold_drop(&hold);
	host_shared_Held_drop(&held);
	simc_Report_drop(&report);
	simc_TestIo_drop(&test_io);
}

/*
 * Bugs row 143's one file: a spill of more keys than one page of a filter
 * holds writes one row file and one tuple-id file, each filter in several
 * pages of its file, and every key reads back through the spill, its row's
 * tuple id finding it through the tuple-id filter.
 */
void
test_txn_spill_tests_a_spill_of_many_keys_writes_one_file_whose_filter_takes_pages(void)
{
	static const uint64 numbers[] = {0, 3000, 6000, 6999};
	static const TxnRunsKind wanted[] = {TXN_RUNS_KIND_SPILLED_TUPLES, TXN_RUNS_KIND_SPILLED_ROWS};
	SimcTestIo	test_io;
	HostIoIo   *io;
	HostSharedHeld held;
	HostSharedRecord state;
	HostStorage storage = txn_memtable_testing_storage(9483);
	StdVec		files;
	HostDeltaHold hold;
	HostError	error;
	uint64		xid;
	bool		spilled;
	bool		all = true;

	simc_size_delta(64 << 20);
	test_io = simc_io();
	io = &test_io.io;
	held = txn_memtable_testing_created(io, 9483);
	state = host_shared_Held_record(&held);
	xid = simc_begin().f0;
	txn_spill_tests_tupled_rows(2656, 74, 2662, 51, io, state, &storage, xid, 0, 7000);
	if (!txn_spill_spilled(io, &storage, TXN_WRITTEN_OF_ROWS, xid, &spilled, &error))
		STD_RESULT_EXPECT_FAILED(2664, 60, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(2664, 9, spilled, "spilled(io, &storage(9483), Of::Rows, xid).unwrap()");
	files = txn_spill_tests_files_of(io, state);
	txn_spill_tests_kinds_are(2667, &files, wanted, lengthof(wanted));
	for (size_t k = 0; all && k < files.len; k++)
		all = STD_VEC_AT(&files, TxnSpillFile, k).bits > TXN_SPILL_PAGE_BITS;
	STD_ASSERT(2668, 9, all, "files.iter().all(|file| file.bits > PAGE_BITS)");
	if (!host_delta_hold(io, &hold, &error))
		STD_RESULT_EXPECT_FAILED(2669, 36, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	for (size_t k = 0; k < lengthof(numbers); k++)
	{
		txn_spill_tests_one_version(2672, 13, 64, io, &hold, state, numbers[k], true);
		txn_spill_tests_tuple_key_is(2673, io, &hold, state, numbers[k]);
	}
	host_delta_Hold_drop(&hold);
	if (!txn_written_ended(io, &error))
		STD_RESULT_EXPECT_FAILED(2676, 28, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	std_vec_free_Checked(&files, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/*
 * Bugs row 74's abort: an aborted transaction's spills leave its storages'
 * chains with its take-back, so no reader reads its spilled versions, and
 * another transaction's spill stays.
 */
void
test_txn_spill_tests_an_abort_takes_its_spills_out_of_the_chain(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9747);
	HostSharedRecord state = host_shared_Held_record(&held);
	HostStorage storage = txn_memtable_testing_storage(9747);
	uint64		mine = simc_begin().f0;
	uint64		other = simc_begin().f0;
	const struct
	{
		uint64		xid;
		const char *key;
	}			pushes[] = {{other, "o"}, {mine, "m"}};
	HostXid		aborted = {mine};
	StdVec		files;
	StdVec		versions;
	HostDeltaHold hold;
	HostError	error;
	size_t		one = 1;
	bool		spilled;
	bool		all = true;

	for (size_t k = 0; k < lengthof(pushes); k++)
	{
		TxnMemtableOp op = txn_memtable_testing_put((const uint8 *) "v", 1);
		TxnMemtableVersionChange change = {.writer = txn_memtable_testing_writer(pushes[k].xid), .op = &op,
		.lock = TXN_MEMTABLE_LOCK_MODE_UPDATE, .shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND}};
		TxnMemtableVersionHandle handle;
		uint64		touched;

		if (!txn_written_touched(io, mine, &storage, TXN_WRITTEN_OF_ROWS, (const uint8 *) pushes[k].key, 1, 0,
								 &touched, &error))
			STD_RESULT_EXPECT_FAILED(2690, 74, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		if (!txn_memtable_table_push(io, state, (const uint8 *) pushes[k].key, 1, &change, &handle, &error))
			STD_RESULT_EXPECT_FAILED(2698, 50, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		txn_memtable_Op_drop(&op);
	}
	if (!txn_spill_spilled(io, &storage, TXN_WRITTEN_OF_ROWS, other, &spilled, &error))
		STD_RESULT_EXPECT_FAILED(2700, 62, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(2700, 9, spilled, "spilled(io, &storage(9747), Of::Rows, other).unwrap()");
	if (!txn_spill_spilled(io, &storage, TXN_WRITTEN_OF_ROWS, mine, &spilled, &error))
		STD_RESULT_EXPECT_FAILED(2701, 61, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(2701, 9, spilled, "spilled(io, &storage(9747), Of::Rows, mine).unwrap()");
	simc_abort(aborted);
	if (!txn_written_aborted(io, 0, &error))
		STD_RESULT_EXPECT_FAILED(2703, 30, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	files = txn_spill_tests_files_of(io, state);
	if (!host_delta_hold(io, &hold, &error))
		STD_RESULT_EXPECT_FAILED(2705, 36, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	for (size_t k = 0; all && k < files.len; k++)
		all = STD_VEC_AT(&files, TxnSpillFile, k).xid == other;
	STD_ASSERT(2706, 9, all && files.len > 0, "left.iter().all(|&xid| xid == other) && !left.is_empty()");
	std_vec_free_Checked(&files, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	if (!txn_memtable_table_versions(io, &hold, state, (const uint8 *) "m", 1, &versions, &error))
		STD_RESULT_EXPECT_FAILED(2707, 57, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(2707, 9, versions.len == 0, "table::versions(io, &hold, state, b\"m\").unwrap().is_empty()");
	txn_spill_tests_versions_free(&versions);
	if (!txn_memtable_table_versions(io, &hold, state, (const uint8 *) "o", 1, &versions, &error))
		STD_RESULT_EXPECT_FAILED(2708, 60, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT_EQ(2708, 9, versions.len == 1, &versions.len, std_usize_Debug_fmt, &one, std_usize_Debug_fmt);
	txn_spill_tests_versions_free(&versions);
	host_delta_Hold_drop(&hold);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/* txn_spill_tests_u64s_Debug_fmt is a Vec<u64>'s Debug. */
static bool
txn_spill_tests_u64s_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *v = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t k = 0; k < v->len; k++)
		std_DebugList_entry(&list, &STD_VEC_AT(v, uint64, k), std_u64_Debug_fmt);
	return std_DebugList_finish(&list);
}

/* txn_spill_tests_rows_xids returns the xids of a state's storage's files of spilled rows, newest first, as the redo case's spilled closure does. */
static StdVec
txn_spill_tests_rows_xids(HostIoIo *io, HostSharedRecord state)
{
	StdVec		files = txn_spill_tests_files_of(io, state);
	StdVec		xids;

	std_vec_new(&xids, sizeof(uint64), _Alignof(uint64));
	for (size_t k = 0; k < files.len; k++)
	{
		const TxnSpillFile *file = &STD_VEC_AT(&files, TxnSpillFile, k);

		if (file->kind == TXN_RUNS_KIND_SPILLED_ROWS)
			std_vec_push_Checked(&xids, &file->xid, sizeof(uint64), _Alignof(uint64));
	}
	std_vec_free_Checked(&files, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	return xids;
}

/* txn_spill_tests_xids_are asserts at its line that a Vec<u64> of xids is a list of them, and frees it. */
static void
txn_spill_tests_xids_are(uint32 line, StdVec *xids, const uint64 *wanted, size_t len)
{
	StdVec		wanted_vec = {(void *) wanted, len, len};

	STD_ASSERT_EQ(line, 9, xids->len == len && memcmp(xids->ptr, wanted, len * sizeof(uint64)) == 0, xids,
				  txn_spill_tests_u64s_Debug_fmt, &wanted_vec, txn_spill_tests_u64s_Debug_fmt);
	std_vec_free_Checked(xids, sizeof(uint64), _Alignof(uint64));
}

/*
 * Bugs row 74's redo: the startup process, which has no transaction of its
 * own, spills the replayed transactions' trees when it finds the delta full,
 * the largest first and whatever their size, as a backend spills its own
 * (bugs row 124), until none is left.
 */
void
test_txn_spill_tests_redo_over_budget_spills_the_largest_replayed_transaction(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9749);
	HostSharedRecord state = host_shared_Held_record(&held);
	HostStorage storage = txn_memtable_testing_storage(9749);
	uint64		small = simc_begin().f0;
	uint64		large = simc_begin().f0;
	const struct
	{
		uint64		xid;
		const char *key;
	}			pushes[] = {{small, "s"}, {large, "l"}};
	const uint64 first[] = {large};
	const uint64 both[] = {small, large};
	StdVec		xids;
	HostError	error;
	uint64		count;
	bool		relieved;

	for (size_t k = 0; k < lengthof(pushes); k++)
	{
		TxnMemtableOp op = txn_memtable_testing_put((const uint8 *) "v", 1);
		TxnMemtableVersionChange change = {.writer = txn_memtable_testing_writer(pushes[k].xid), .op = &op,
			.lock = TXN_MEMTABLE_LOCK_MODE_UPDATE, .shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND},
		.has_lsn = true, .lsn = 9};
		TxnMemtableVersionHandle handle;

		if (!txn_memtable_table_push(io, state, (const uint8 *) pushes[k].key, 1, &change, &handle, &error))
			STD_RESULT_EXPECT_FAILED(2730, 50, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		txn_memtable_Op_drop(&op);
	}
	if (!txn_spill_replayed(io, &storage, TXN_WRITTEN_OF_ROWS, small, 10, &count, &error))
		STD_RESULT_EXPECT_FAILED(2732, 59, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!txn_spill_replayed(io, &storage, TXN_WRITTEN_OF_ROWS, large, (size_t) TXN_SPILL_LOW, &count, &error))
		STD_RESULT_EXPECT_FAILED(2733, 69, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!txn_spill_relieved(io, &relieved, &error))
		STD_RESULT_EXPECT_FAILED(2738, 30, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(2738, 9, relieved, "relieved(io).unwrap()");
	xids = txn_spill_tests_rows_xids(io, state);
	txn_spill_tests_xids_are(2739, &xids, first, lengthof(first));
	if (!txn_spill_relieved(io, &relieved, &error))
		STD_RESULT_EXPECT_FAILED(2740, 30, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(2740, 9, relieved, "relieved(io).unwrap()");
	xids = txn_spill_tests_rows_xids(io, state);
	txn_spill_tests_xids_are(2741, &xids, both, lengthof(both));
	if (!txn_spill_relieved(io, &relieved, &error))
		STD_RESULT_EXPECT_FAILED(2742, 31, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(2742, 9, !relieved, "!relieved(io).unwrap()");
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/* txn_spill_tests_u32s_Debug_fmt is a Vec<u32>'s Debug. */
static bool
txn_spill_tests_u32s_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *v = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t k = 0; k < v->len; k++)
		std_DebugList_entry(&list, &STD_VEC_AT(v, uint32, k), std_u32_Debug_fmt);
	return std_DebugList_finish(&list);
}

/* txn_spill_tests_put_by pushes a transaction's put of a value to a key of a state's memtable, as the tiers case's push closure does. */
static void
txn_spill_tests_put_by(HostIoIo *io, HostSharedRecord state, uint64 xid, const char *key, const char *value)
{
	TxnMemtableOp op = txn_memtable_testing_put((const uint8 *) value, strlen(value));
	TxnMemtableVersionChange change = {.writer = txn_memtable_testing_writer(xid), .op = &op,
	.lock = TXN_MEMTABLE_LOCK_MODE_UPDATE, .shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND}};
	TxnMemtableVersionHandle handle;
	HostError	error;

	if (!txn_memtable_table_push(io, state, (const uint8 *) key, strlen(key), &change, &handle, &error))
		STD_RESULT_EXPECT_FAILED(2764, 50, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	txn_memtable_Op_drop(&op);
}

/* txn_spill_tests_values_are asserts at its line that a key's versions' values in a state's memtable and spills are a list, newest first, as the tiers case's values closure reads them. */
static void
txn_spill_tests_values_are(uint32 line, HostIoIo *io, const HostDeltaHold *hold, HostSharedRecord state,
						   const char *key, const char *const *wanted, size_t len)
{
	StdVec		versions;
	StdVec		values;
	StdVec		expected;
	HostError	error;

	if (!txn_memtable_table_versions(io, hold, state, (const uint8 *) key, strlen(key), &versions, &error))
		STD_RESULT_EXPECT_FAILED(2778, 18, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	std_vec_with_capacity_Checked(&values, versions.len, sizeof(StdVec), _Alignof(StdVec));
	for (size_t k = 0; k < versions.len; k++)
	{
		TxnMemtableVersionRecord record =
			txn_memtable_version_Version_record(&STD_VEC_AT(&versions, TxnMemtableVersionVersion, k));
		StdVec		value = txn_spill_tests_value(&record);

		std_vec_push_Checked(&values, &value, sizeof(StdVec), _Alignof(StdVec));
	}
	txn_spill_tests_versions_free(&versions);
	txn_memtable_table_tests_keys_of(wanted, len, &expected);
	STD_ASSERT_EQ(line, 9, txn_memtable_table_tests_keys_eq(&values, &expected), &values,
				  txn_memtable_table_tests_keys_Debug_fmt, &expected, txn_memtable_table_tests_keys_Debug_fmt);
	txn_memtable_table_tests_keys_free(&expected);
	txn_memtable_table_tests_keys_free(&values);
}

/*
 * Bugs row 74's tiers: a transaction's second spill of a storage's rows, no
 * larger than its first, merges with it into one file, a key written again
 * after the first spill holding its newer version first, and the merged
 * files leave the chain.
 */
void
test_txn_spill_tests_a_second_spill_merges_with_the_first_keeping_its_versions_order(void)
{
	static const char *const a[] = {"1"};
	static const char *const b[] = {"3", "2"};
	static const char *const c[] = {"4"};
	static const uint32 merged[] = {2};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9516);
	HostSharedRecord state = host_shared_Held_record(&held);
	HostStorage storage = txn_memtable_testing_storage(9516);
	uint64		mine = simc_begin().f0;
	StdVec		files;
	StdVec		rows;
	StdVec		merged_vec = {(void *) merged, lengthof(merged), lengthof(merged)};
	HostDeltaHold hold;
	HostError	error;
	bool		spilled;

	txn_spill_tests_put_by(io, state, mine, "a", "1");
	txn_spill_tests_put_by(io, state, mine, "b", "2");
	if (!txn_spill_spilled(io, &storage, TXN_WRITTEN_OF_ROWS, mine, &spilled, &error))
		STD_RESULT_EXPECT_FAILED(2768, 61, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(2768, 9, spilled, "spilled(io, &storage(9516), Of::Rows, mine).unwrap()");
	txn_spill_tests_put_by(io, state, mine, "b", "3");
	txn_spill_tests_put_by(io, state, mine, "c", "4");
	if (!txn_spill_spilled(io, &storage, TXN_WRITTEN_OF_ROWS, mine, &spilled, &error))
		STD_RESULT_EXPECT_FAILED(2771, 61, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(2771, 9, spilled, "spilled(io, &storage(9516), Of::Rows, mine).unwrap()");
	files = txn_spill_tests_files_of(io, state);
	std_vec_new(&rows, sizeof(uint32), _Alignof(uint32));
	for (size_t k = 0; k < files.len; k++)
	{
		const TxnSpillFile *file = &STD_VEC_AT(&files, TxnSpillFile, k);

		if (file->kind == TXN_RUNS_KIND_SPILLED_ROWS)
			std_vec_push_Checked(&rows, &file->number, sizeof(uint32), _Alignof(uint32));
	}
	std_vec_free_Checked(&files, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	if (!host_delta_hold(io, &hold, &error))
		STD_RESULT_EXPECT_FAILED(2774, 36, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT_EQ(2775, 9, rows.len == 1 && STD_VEC_AT(&rows, uint32, 0) == 2, &rows, txn_spill_tests_u32s_Debug_fmt,
				  &merged_vec, txn_spill_tests_u32s_Debug_fmt);
	std_vec_free_Checked(&rows, sizeof(uint32), _Alignof(uint32));
	txn_spill_tests_values_are(2786, io, &hold, state, "a", a, lengthof(a));
	txn_spill_tests_values_are(2787, io, &hold, state, "b", b, lengthof(b));
	txn_spill_tests_values_are(2788, io, &hold, state, "c", c, lengthof(c));
	host_delta_Hold_drop(&hold);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/*
 * Bugs row 107's threshold (STORAGE_DESIGN §III.3): a spill's threshold is an
 * eighth of the budget, and at the least delta_memory that is below LOW, so a
 * recovery's replayed transactions there spill before the delta's fixed
 * records leave no room for any of them to reach LOW.
 */
void
test_txn_spill_tests_a_small_budgets_threshold_is_an_eighth_of_it(void)
{
	uint64		threshold;
	uint64		eighth = ((uint64) 1 << 20) / TXN_SPILL_SHARE;

	simc_size_delta(1 << 20);
	threshold = txn_spill_threshold();
	STD_ASSERT_EQ(2798, 9, threshold == eighth, &threshold, std_u64_Debug_fmt, &eighth, std_u64_Debug_fmt);
	STD_ASSERT(2799, 9, txn_spill_threshold() < TXN_SPILL_LOW, "threshold() < LOW");
}

/* txn_spill_tests_pushed_rows pushes a transaction's version of each of a range of numbered keys to a storage's rows, counted toward its spill as a writer's are. */
static void
txn_spill_tests_pushed_rows(HostIoIo *io, HostSharedRecord state, const HostStorage *at, uint64 xid, uint64 start,
							uint64 end)
{
	for (uint64 number = start; number < end; number++)
	{
		TxnMemtableOp op = txn_memtable_testing_put((const uint8 *) "v", 1);
		TxnMemtableVersionChange change = {.writer = txn_memtable_testing_writer(xid), .op = &op,
		.lock = TXN_MEMTABLE_LOCK_MODE_UPDATE, .shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND}};
		TxnMemtableVersionHandle handle;
		HostError	error;
		uint64		touched;
		uint8		key[8];

		std_u64_to_be_bytes(number, key);
		if (!txn_written_touched(io, xid, at, TXN_WRITTEN_OF_ROWS, key, sizeof(key), 0, &touched, &error))
			STD_RESULT_EXPECT_FAILED(2814, 62, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		if (!txn_memtable_table_push(io, state, key, sizeof(key), &change, &handle, &error))
			STD_RESULT_EXPECT_FAILED(2815, 51, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		txn_memtable_Op_drop(&op);
	}
}

/* txn_spill_tests_relation returns the storage of a relation of a test's database, numbered past the first. */
static HostStorage
txn_spill_tests_relation(uint32 database, uint32 number)
{
	HostStorage storage = txn_memtable_testing_storage(database);

	storage.relation = 16384 + number;
	return storage;
}

/*
 * Bugs row 143's many storages: one transaction spills more storages than a
 * 256 KB delta holds in 1 KB slots, its spills' bookkeeping taking none of
 * the delta, so the delta's used bytes after each later spill are no more
 * than after its first, and every storage's rows read back through its
 * spill.
 */
void
test_txn_spill_tests_a_transaction_spilling_more_storages_than_the_budget_holds_pages_completes(void)
{
	static const StdFmtPiece past[] = {STD_FMT_LIT("after "), STD_FMT_ARG(0), STD_FMT_LIT(" the delta uses "),
	STD_FMT_ARG(1), STD_FMT_LIT(" past "), STD_FMT_ARG(2)};
	static const uint32 read[] = {0, 299};
	SimcTestIo	test_io;
	HostIoIo   *io;
	HostSharedHeld held;
	HostError	error;
	uint64		xid;
	uint64		first = 0;
	bool		has_first = false;

	simc_size_delta(256 << 10);
	test_io = simc_io();
	io = &test_io.io;
	held = txn_memtable_testing_created(io, 9510);
	host_shared_Held_drop(&held);
	xid = simc_begin().f0;
	for (uint32 number = 0; number < 300; number++)
	{
		HostStorage at = txn_spill_tests_relation(9510, number);
		HostSharedHeld state;
		bool		spilled;
		uint64		used;

		if (!txn_memtable_testing_held(io, &at, &state, &error))
			STD_RESULT_EXPECT_FAILED(2837, 59, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		txn_spill_tests_pushed_rows(io, host_shared_Held_record(&state), &at, xid, 0, 4);
		if (!txn_spill_spilled(io, &at, TXN_WRITTEN_OF_ROWS, xid, &spilled, &error))
			STD_RESULT_EXPECT_FAILED(2839, 73, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		STD_ASSERT(2839, 13, spilled, "spilled(io, &relation(9510, number), Of::Rows, xid).unwrap()");
		if (!has_first)
		{
			first = host_delta_used();
			has_first = true;
		}
		used = first;
		if (!(host_delta_used() <= used))
		{
			uint64		now = host_delta_used();
			StdFmtArg	args[3] = {{&number, std_u32_Display_fmt}, {&now, std_u64_Display_fmt},
			{&used, std_u64_Display_fmt}};
			StdFmtArguments message = {past, lengthof(past), args};

			std_panic_fmt(RUST_FILE, 2841, 13, &message);
		}
		host_shared_Held_drop(&state);
	}
	for (size_t k = 0; k < lengthof(read); k++)
	{
		HostStorage at = txn_spill_tests_relation(9510, read[k]);
		HostSharedHeld state;
		HostDeltaHold hold;

		if (!txn_memtable_testing_held(io, &at, &state, &error))
			STD_RESULT_EXPECT_FAILED(2844, 59, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		if (!host_delta_hold(io, &hold, &error))
			STD_RESULT_EXPECT_FAILED(2845, 40, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		txn_spill_tests_one_version(2846, 13, 88, io, &hold, host_shared_Held_record(&state), 3, false);
		host_delta_Hold_drop(&hold);
		host_shared_Held_drop(&state);
	}
	if (!txn_written_ended(io, &error))
		STD_RESULT_EXPECT_FAILED(2848, 28, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	simc_TestIo_drop(&test_io);
}

/*
 * Bugs row 143's two at once: two transactions spill the same storages
 * crosswise, one first in a storage and the other first in the next, more
 * storages than a 256 KB delta holds in 1 KB slots, both completing, and
 * once both abort, every manifest lists nothing and the delta's used bytes
 * are no more than after the first storage's spills.
 */
void
test_txn_spill_tests_two_transactions_spilling_the_same_storages_crosswise_complete(void)
{
	static const StdFmtPiece past[] = {STD_FMT_LIT("the delta uses "), STD_FMT_ARG(0), STD_FMT_LIT(" past "),
	STD_FMT_ARG(1)};
	SimcTestIo	test_io;
	HostIoIo   *io;
	HostSharedHeld held;
	HostStorage storages[300];
	HostError	error;
	uint64		one;
	uint64		two;
	uint64		first = 0;
	bool		has_first = false;

	simc_size_delta(256 << 10);
	test_io = simc_io();
	io = &test_io.io;
	held = txn_memtable_testing_created(io, 9511);
	host_shared_Held_drop(&held);
	one = simc_begin().f0;
	two = simc_begin().f0;
	for (uint32 number = 0; number < lengthof(storages); number++)
		storages[number] = txn_spill_tests_relation(9511, number);
	for (size_t number = 0; number < lengthof(storages); number++)
	{
		const HostStorage *at = &storages[number];
		uint64		order[2] = {number % 2 == 0 ? one : two, number % 2 == 0 ? two : one};
		HostSharedHeld state;

		if (!txn_memtable_testing_held(io, at, &state, &error))
			STD_RESULT_EXPECT_FAILED(2866, 38, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		txn_spill_tests_pushed_rows(io, host_shared_Held_record(&state), at, one, 0, 2);
		txn_spill_tests_pushed_rows(io, host_shared_Held_record(&state), at, two, 2, 4);
		for (size_t k = 0; k < lengthof(order); k++)
		{
			bool		spilled;

			if (!txn_spill_spilled(io, at, TXN_WRITTEN_OF_ROWS, order[k], &spilled, &error))
				STD_RESULT_EXPECT_FAILED(2870, 56, "called `Result::unwrap()` on an `Err` value", &error,
										 host_Error_Debug_fmt);
			STD_ASSERT(2870, 17, spilled, "spilled(io, at, Of::Rows, xid).unwrap()");
		}
		if (!has_first)
		{
			first = host_delta_used();
			has_first = true;
		}
		host_shared_Held_drop(&state);
	}
	if (!txn_spill_aborted(io, one, storages, lengthof(storages), 0, &error))
		STD_RESULT_EXPECT_FAILED(2875, 41, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!txn_spill_aborted(io, two, storages, lengthof(storages), 0, &error))
		STD_RESULT_EXPECT_FAILED(2875, 41, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	for (size_t k = 0; k < 2; k++)
	{
		const HostStorage *at = &storages[k == 0 ? 0 : 299];
		HostSharedHeld state;
		StdVec		files;

		if (!txn_memtable_testing_held(io, at, &state, &error))
			STD_RESULT_EXPECT_FAILED(2878, 38, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		files = txn_spill_tests_files_of(io, host_shared_Held_record(&state));
		STD_ASSERT(2879, 13, files.len == 0, "files_of(io, state.record()).is_empty()");
		std_vec_free_Checked(&files, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
		host_shared_Held_drop(&state);
	}
	if (!(host_delta_used() <= first))
	{
		uint64		now = host_delta_used();
		StdFmtArg	args[2] = {{&now, std_u64_Display_fmt}, {&first, std_u64_Display_fmt}};
		StdFmtArguments message = {past, lengthof(past), args};

		std_panic_fmt(RUST_FILE, 2882, 9, &message);
	}
	if (!txn_written_ended(io, &error))
		STD_RESULT_EXPECT_FAILED(2883, 28, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	simc_TestIo_drop(&test_io);
}

/*
 * Bugs row 143's many spillers: twenty transactions, more than any fixed
 * count of a storage's spillers, each spill one storage at a delta another
 * process filled, every one completing with no page of the delta, and every
 * transaction's rows read back.
 */
void
test_txn_spill_tests_more_transactions_than_any_fixed_count_spill_one_storage_at_a_full_budget(void)
{
	static const uint64 numbers[] = {0, 99, 199};
	SimcTestIo	test_io;
	HostIoIo   *io;
	HostSharedHeld held;
	HostSharedRecord state;
	HostStorage storage = txn_memtable_testing_storage(9512);
	uint64		xids[20];
	StdVec		filler;
	StdVec		files;
	HostDeltaHold hold;
	HostError	error;
	size_t		rows = 0;
	size_t		count = lengthof(xids);

	simc_size_delta(2 << 20);
	test_io = simc_io();
	io = &test_io.io;
	held = txn_memtable_testing_created(io, 9512);
	state = host_shared_Held_record(&held);
	for (size_t k = 0; k < lengthof(xids); k++)
		xids[k] = simc_begin().f0;
	for (uint64 at = 0; at < lengthof(xids); at++)
		txn_spill_tests_pushed_rows(io, state, &storage, xids[at], at * 10, at * 10 + 10);
	std_vec_new(&filler, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned));
	if (!txn_spill_tests_fill(io, &filler, &error))
		STD_RESULT_EXPECT_FAILED(2902, 88, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	for (size_t k = 0; k < lengthof(xids); k++)
	{
		bool		spilled;

		if (!txn_spill_spilled(io, &storage, TXN_WRITTEN_OF_ROWS, xids[k], &spilled, &error))
			STD_RESULT_EXPECT_FAILED(2906, 64, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		STD_ASSERT(2906, 13, spilled, "spilled(io, &storage(9512), Of::Rows, xid).unwrap()");
	}
	files = txn_spill_tests_files_of(io, state);
	for (size_t k = 0; k < files.len; k++)
		if (STD_VEC_AT(&files, TxnSpillFile, k).kind == TXN_RUNS_KIND_SPILLED_ROWS)
			rows++;
	std_vec_free_Checked(&files, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	STD_ASSERT_EQ(2909, 9, rows == count, &rows, std_usize_Debug_fmt, &count, std_usize_Debug_fmt);
	if (!host_delta_hold(io, &hold, &error))
		STD_RESULT_EXPECT_FAILED(2910, 36, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	for (size_t k = 0; k < lengthof(numbers); k++)
		txn_spill_tests_one_version(2912, 13, 81, io, &hold, state, numbers[k], true);
	host_delta_Hold_drop(&hold);
	std_vec_drop_Checked(&filler, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned), host_delta_Owned_drop);
	if (!txn_written_ended(io, &error))
		STD_RESULT_EXPECT_FAILED(2915, 28, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/* txn_spill_tests_rewriter is the writers' lock case's merger: its transaction's two spills of halves of its rows, the second merging with the first. */
static bool
txn_spill_tests_rewriter(void *captures, HostIoIo *io, HostError *error)
{
	static const uint64 halves[2][2] = {{0, 10}, {10, 20}};
	HostStorage storage = txn_memtable_testing_storage(9513);
	uint64		merging = *(const uint64 *) captures;
	HostSharedHeld state;

	if (!txn_memtable_testing_stated(io, 9513, &state, error))
		return false;
	for (size_t k = 0; k < lengthof(halves); k++)
	{
		bool		spilled;

		txn_spill_tests_pushed_rows(io, host_shared_Held_record(&state), &storage, merging, halves[k][0],
									halves[k][1]);
		if (!txn_spill_spilled(io, &storage, TXN_WRITTEN_OF_ROWS, merging, &spilled, error))
		{
			host_shared_Held_drop(&state);
			return false;
		}
		STD_ASSERT(2934, 17, spilled, "spilled(io, &storage(9513), Of::Rows, merging)?");
	}
	host_shared_Held_drop(&state);
	return true;
}

/* txn_spill_tests_lister is the writers' lock case's other transaction: it spills its own rows, its listing waiting on the merge's lock. */
static bool
txn_spill_tests_lister(void *captures, HostIoIo *io, HostError *error)
{
	HostStorage storage = txn_memtable_testing_storage(9513);
	uint64		other = *(const uint64 *) captures;
	HostSharedHeld state;
	bool		spilled;
	bool		ok;

	if (!txn_memtable_testing_stated(io, 9513, &state, error))
		return false;
	txn_spill_tests_pushed_rows(io, host_shared_Held_record(&state), &storage, other, 100, 110);
	ok = txn_spill_spilled(io, &storage, TXN_WRITTEN_OF_ROWS, other, &spilled, error);
	if (ok)
		STD_ASSERT(2941, 13, spilled, "spilled(io, &storage(9513), Of::Rows, other)?");
	host_shared_Held_drop(&state);
	return ok;
}

/*
 * Bugs row 143's writers' lock: a merge holds its storage's manifest's
 * writers' lock after it read the manifest, and another transaction's listing
 * of its own spill of the same storage waits its turn, so both the merged
 * file and the other's file are listed after, nothing lost, and every key
 * reads back once.
 */
void
test_txn_spill_tests_a_merges_rewrite_and_another_transactions_listing_lose_nothing(void)
{
	static const StdFmtPiece lead[] = {STD_FMT_LIT("the run didn't end: "), STD_FMT_ARG(0)};
	static const uint64 numbers[] = {0, 19, 100, 109};
	SimcStep	steps[1] = {{"other", 5, PG_UINT64_MAX}};
	SimcTestIo	test_io;
	HostIoIo   *io;
	HostSharedHeld held;
	SimcRun		run;
	SimcReport	report;
	HostDeltaHold hold;
	HostError	error;
	StdVec		xids;
	uint64		merging;
	uint64		other;
	uint64		listed[2];
	SimcActor	merger = {txn_spill_tests_rewriter, &merging};
	SimcActor	lister = {txn_spill_tests_lister, &other};

	simc_size_delta(64 << 20);
	test_io = simc_io();
	io = &test_io.io;
	held = txn_memtable_testing_created(io, 9513);
	host_shared_Held_drop(&held);
	merging = simc_begin().f0;
	other = simc_begin().f0;
	run = simc_Run_new(1);
	simc_Run_actor(&run, "merger", 6, merger);
	simc_Run_actor(&run, "other", 5, lister);
	simc_Run_until(&run, "merger", 6, "doltgres-spill-merge-pended");
	simc_Run_until(&run, "merger", 6, "doltgres-spill-manifest-read");
	simc_Run_script(&run, steps, lengthof(steps));
	report = simc_Run_finish(run);
	txn_spill_tests_returned(&report, 2948, 67, lead, 2949);
	if (!txn_memtable_testing_stated(io, 9513, &held, &error))
		STD_RESULT_EXPECT_FAILED(2950, 38, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	xids = txn_spill_tests_rows_xids(io, host_shared_Held_record(&held));
	listed[0] = other;
	listed[1] = merging;
	txn_spill_tests_xids_are(2956, &xids, listed, lengthof(listed));
	if (!host_delta_hold(io, &hold, &error))
		STD_RESULT_EXPECT_FAILED(2957, 36, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	for (size_t k = 0; k < lengthof(numbers); k++)
		txn_spill_tests_one_version(2959, 13, 90, io, &hold, host_shared_Held_record(&held), numbers[k], true);
	host_delta_Hold_drop(&hold);
	host_shared_Held_drop(&held);
	simc_Report_drop(&report);
	simc_TestIo_drop(&test_io);
}

/* txn_spill_tests_paused_reader is the whole lists case's reader: it reads the key's versions, paused between its open of the manifest and its read. */
static bool
txn_spill_tests_paused_reader(void *captures, HostIoIo *io, HostError *error)
{
	HostSharedHeld state;
	HostDeltaHold hold;
	StdVec		versions;
	size_t		one = 1;
	uint8		key[8];
	bool		ok;

	if (!txn_memtable_testing_stated(io, 9514, &state, error))
		return false;
	if (!host_delta_hold(io, &hold, error))
	{
		host_shared_Held_drop(&state);
		return false;
	}
	std_u64_to_be_bytes(100, key);
	ok = txn_memtable_table_versions(io, &hold, host_shared_Held_record(&state), key, sizeof(key), &versions, error);
	if (ok)
	{
		STD_ASSERT_EQ_MSG(2984, 13, versions.len == 1, &versions.len, std_usize_Debug_fmt, &one, std_usize_Debug_fmt,
						  "the paused read finds the version once");
		txn_spill_tests_versions_free(&versions);
	}
	host_delta_Hold_drop(&hold);
	host_shared_Held_drop(&state);
	return ok;
}

/* txn_spill_tests_spiller is the whole lists case's spiller: its spill of the key's version renames a new list over the manifest. */
static bool
txn_spill_tests_spiller(void *captures, HostIoIo *io, HostError *error)
{
	HostStorage storage = txn_memtable_testing_storage(9514);
	bool		spilled;

	if (!txn_spill_spilled(io, &storage, TXN_WRITTEN_OF_ROWS, *(const uint64 *) captures, &spilled, error))
		return false;
	STD_ASSERT(2988, 13, spilled, "spilled(io, &storage(9514), Of::Rows, second)?");
	return true;
}

/*
 * Bugs row 143's whole lists: a reader that read a key's chain in the
 * memtable pauses between its open of the storage's manifest file and its
 * read, while a spill of the key's version renames a new list over the
 * manifest; it finds the key's version once, through the chain it read, and
 * a later read finds it once through the new list.
 */
void
test_txn_spill_tests_a_reader_paused_in_a_manifest_read_finds_each_version_once(void)
{
	static const StdFmtPiece lead[] = {STD_FMT_LIT("the run didn't end: "), STD_FMT_ARG(0)};
	SimcStep	steps[1] = {{"spiller", 7, PG_UINT64_MAX}};
	SimcActor	reader = {txn_spill_tests_paused_reader};
	HostStorage storage = txn_memtable_testing_storage(9514);
	SimcTestIo	test_io;
	HostIoIo   *io;
	HostSharedHeld held;
	SimcRun		run;
	SimcReport	report;
	HostDeltaHold hold;
	HostError	error;
	StdVec		files;
	uint64		first;
	uint64		second;
	size_t		rows = 0;
	size_t		two = 2;
	bool		spilled;
	SimcActor	spiller = {txn_spill_tests_spiller, &second};

	simc_size_delta(64 << 20);
	test_io = simc_io();
	io = &test_io.io;
	held = txn_memtable_testing_created(io, 9514);
	first = simc_begin().f0;
	second = simc_begin().f0;
	txn_spill_tests_pushed_rows(io, host_shared_Held_record(&held), &storage, first, 0, 10);
	if (!txn_spill_spilled(io, &storage, TXN_WRITTEN_OF_ROWS, first, &spilled, &error))
		STD_RESULT_EXPECT_FAILED(2976, 62, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(2976, 9, spilled, "spilled(io, &storage(9514), Of::Rows, first).unwrap()");
	txn_spill_tests_pushed_rows(io, host_shared_Held_record(&held), &storage, second, 100, 101);
	host_shared_Held_drop(&held);
	run = simc_Run_new(1);
	simc_Run_actor(&run, "reader", 6, reader);
	simc_Run_actor(&run, "spiller", 7, spiller);
	simc_Run_until(&run, "reader", 6, "doltgres-spill-manifest-opened");
	simc_Run_script(&run, steps, lengthof(steps));
	report = simc_Run_finish(run);
	txn_spill_tests_returned(&report, 2994, 67, lead, 2995);
	if (!txn_memtable_testing_stated(io, 9514, &held, &error))
		STD_RESULT_EXPECT_FAILED(2996, 38, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	files = txn_spill_tests_files_of(io, host_shared_Held_record(&held));
	for (size_t k = 0; k < files.len; k++)
		if (STD_VEC_AT(&files, TxnSpillFile, k).kind == TXN_RUNS_KIND_SPILLED_ROWS)
			rows++;
	std_vec_free_Checked(&files, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	STD_ASSERT_EQ(2998, 9, rows == two, &rows, std_usize_Debug_fmt, &two, std_usize_Debug_fmt);
	if (!host_delta_hold(io, &hold, &error))
		STD_RESULT_EXPECT_FAILED(2999, 36, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	txn_spill_tests_one_version(3000, 9, 86, io, &hold, host_shared_Held_record(&held), 100, false);
	if (!txn_written_ended(io, &error))
		STD_RESULT_EXPECT_FAILED(3001, 28, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	host_delta_Hold_drop(&hold);
	host_shared_Held_drop(&held);
	simc_Report_drop(&report);
	simc_TestIo_drop(&test_io);
}

/* txn_spill_tests_ones fills an installed node's bytes with ones, as the evicted filter case's install closure does. */
static void
txn_spill_tests_ones(void *captures, uint8 *out, size_t len)
{
	memset(out, 1, len);
}

/*
 * Bugs row 143's evicted filter: a lookup in a spill's file reads its
 * filter's page into the node cache; once other nodes' installs take the
 * cache's one frame, the page is gone from the cache, and the next lookup
 * reads it from the file again and finds its key.
 */
void
test_txn_spill_tests_a_lookup_reads_an_evicted_filter_page_again(void)
{
	static const size_t counts[HOST_CACHE_CLASSES_LEN] = {1, 0, 0, 0, 0};
	const StdBytes keys[1] = {{(const uint8 *) "k", 1}};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9515);
	StdPathBuf	path;
	TxnSpillFile file = txn_spill_tests_written(io, host_shared_Held_record(&held), 745, 0, keys, lengthof(keys),
												&path);
	HostCacheKey page;
	HostError	error;
	bool		hit;
	bool		contains;

	simc_size_shared(counts);
	if (!txn_spill_hits(io, &path, &file, (const uint8 *) "k", 1, &hit, &error))
		STD_RESULT_EXPECT_FAILED(3014, 48, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(3014, 9, hit, "hits(io, (&path, &file), b\"k\").unwrap()");
	page = txn_spill_filter_key(&path, &file, 0);
	if (!host_cache_contains(io, HOST_CACHE_PLACEMENT_SERVER, &page, &contains, &error))
		STD_RESULT_EXPECT_FAILED(3016, 63, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(3016, 9, contains, "cache::contains(io, Placement::Server, &page).unwrap()");
	for (uint8 other = 0; other < 4; other++)
	{
		uint8		address[HOST_CACHE_ADDRESS];
		uint8		locator[HOST_CACHE_LOCATOR] = {0};
		HostCacheKey key;
		HostCacheRead read;

		memset(address, other, sizeof(address));
		key = host_cache_key(1, address, locator);
		if (!host_cache_install(io, HOST_CACHE_PLACEMENT_SERVER, &key, 8, txn_spill_tests_ones, NULL, NULL, &read,
								&error))
			STD_RESULT_EXPECT_FAILED(3019, 86, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		host_cache_Read_drop(&read);
	}
	if (!host_cache_contains(io, HOST_CACHE_PLACEMENT_SERVER, &page, &contains, &error))
		STD_RESULT_EXPECT_FAILED(3021, 64, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT_MSG(3021, 9, !contains, "the other node took the page's frame");
	if (!txn_spill_hits(io, &path, &file, (const uint8 *) "k", 1, &hit, &error))
		STD_RESULT_EXPECT_FAILED(3022, 48, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(3022, 9, hit, "hits(io, (&path, &file), b\"k\").unwrap()");
	std_vec_free_Checked(&path.bytes, 1, 1);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/*
 * The free space map's spill: a spill of a storage's free space map takes
 * out the blocks only its transaction wrote, and every reader finds a
 * spilled block's entries below the memtable: its newest, a removal
 * included, its takers, a seek past it, and a version by its WAL record's
 * end.
 */
void
test_txn_spill_tests_a_spilled_free_space_maps_entries_are_read_below_the_memtable(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9762);
	HostSharedRecord state = host_shared_Held_record(&held);
	HostStorage storage = txn_memtable_testing_storage(9762);
	uint64		mine = simc_begin().f0;
	uint64		other = simc_begin().f0;
	const struct
	{
		uint64		xid;
		uint32		block;
		const char *value;
		bool		has_lsn;
		uint64		lsn;
	}			pushes[] = {{mine, 7, "e", true, 4242}, {mine, 8, "f"}, {mine, 8, NULL}, {other, 9, "g"},
	{mine, 10, "h"}};
	const struct
	{
		uint32		block;
		bool		some;
		const char *value;
	}			newest[] = {{7, true, "e"}, {8, true, NULL}, {9, true, "g"}, {10, true, "h"}, {11, false, NULL}};
	const struct
	{
		uint32		from;
		bool		some;
		uint32		block;
		const char *value;
	}			sought[] = {{0, true, 7, "e"}, {8, true, 8, NULL}, {9, true, 9, "g"}, {10, true, 10, "h"},
	{11, false, 0, NULL}};
	HostDeltaHold hold;
	StdVec		versions;
	StdVec		takers;
	HostError	error;
	bool		spilled;
	bool		holds;

	for (size_t k = 0; k < lengthof(pushes); k++)
	{
		TxnMemtableOp op = {.tag = TXN_MEMTABLE_OP_DELETE};
		TxnMemtableVersionChange change = {.writer = txn_memtable_testing_writer(pushes[k].xid), .op = &op,
			.lock = TXN_MEMTABLE_LOCK_MODE_UPDATE, .shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND},
		.has_lsn = pushes[k].has_lsn, .lsn = pushes[k].lsn};
		TxnMemtableVersionHandle handle;
		uint8		key[4];

		if (pushes[k].value != NULL)
			op = txn_memtable_testing_put((const uint8 *) pushes[k].value, 1);
		std_u32_to_be_bytes(pushes[k].block, key);
		if (!txn_memtable_table_alone_State(io, state, true, TXN_MEMTABLE_MAP_MAP_FREE, key, sizeof(key), &change,
											&handle, &error))
			STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		txn_memtable_Op_drop(&op);
	}
	if (!txn_spill_spilled(io, &storage, TXN_WRITTEN_OF_FREE, mine, &spilled, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(__LINE__, 9, spilled, "spilled(io, &storage(9762), Of::Free, mine).unwrap()");
	if (!host_delta_hold(io, &hold, &error) ||
		!txn_memtable_map_versions(&TXN_MEMTABLE_MAP_FREE_TREE_OPS, io, &hold, state, 7, &versions, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(__LINE__, 9, versions.len == 0, "map::versions::<map::Free>(io, &hold, state, 7).unwrap().is_empty()");
	txn_spill_tests_versions_free(&versions);
	host_delta_Hold_drop(&hold);
	for (size_t k = 0; k < lengthof(newest); k++)
	{
		TxnMemtableMapEntry entry;
		TxnMemtableMapEntry wanted = {.has_value = newest[k].value != NULL};
		bool		some;
		StdOptionRef left;
		StdOptionRef right = {newest[k].some, STD_ITEM(&wanted, txn_memtable_map_tests_seen_Debug_fmt)};

		if (wanted.has_value)
			std_slice_to_vec_Checked(&wanted.value, (const uint8 *) newest[k].value, 1, 1, 1);
		if (!txn_memtable_map_newest(io, state, TXN_MEMTABLE_MAP_MAP_FREE, newest[k].block, &some, &entry, &error))
			STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		left = (StdOptionRef)
		{
			some, STD_ITEM(&entry, txn_memtable_map_tests_seen_Debug_fmt)
		};
		STD_ASSERT_EQ(__LINE__, 9, some == newest[k].some && (!some || txn_memtable_map_tests_seen_eq(&entry, &wanted)),
					  &left, std_option_Debug_fmt, &right, std_option_Debug_fmt);
		if (some)
			txn_memtable_map_tests_seen_drop(&entry);
		txn_memtable_map_tests_seen_drop(&wanted);
	}
	if (!txn_memtable_map_takers(io, state, TXN_MEMTABLE_MAP_MAP_FREE, 8, &takers, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT_EQ_U64(__LINE__, 9, takers.len, 2);
	for (size_t k = 0; k < takers.len; k++)
	{
		const txn_TupleWriterRowEntryU64TupleVecU32Bool_t *taker =
			&STD_VEC_AT(&takers, txn_TupleWriterRowEntryU64TupleVecU32Bool_t, k);
		TxnMemtableMapEntry wanted = {.has_value = k == 0};

		if (wanted.has_value)
			std_slice_to_vec_Checked(&wanted.value, (const uint8 *) "f", 1, 1, 1);
		STD_ASSERT_EQ_U64(__LINE__, 9, taker->f0.xid, mine);
		STD_ASSERT_EQ(__LINE__, 9, txn_memtable_map_tests_seen_eq(&taker->f2, &wanted), &taker->f2,
					  txn_memtable_map_tests_seen_Debug_fmt, &wanted, txn_memtable_map_tests_seen_Debug_fmt);
		txn_memtable_map_tests_seen_drop(&wanted);
	}
	txn_memtable_map_takers_drop(&takers);
	for (size_t k = 0; k < lengthof(sought); k++)
	{
		txn_TupleU32Entry_t found;
		TxnMemtableMapEntry wanted = {.has_value = sought[k].value != NULL};
		bool		some;

		if (wanted.has_value)
			std_slice_to_vec_Checked(&wanted.value, (const uint8 *) sought[k].value, 1, 1, 1);
		if (!txn_memtable_map_seek(io, state, TXN_MEMTABLE_MAP_MAP_FREE, sought[k].from, &some, &found, &error))
			STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		STD_ASSERT(__LINE__, 9, some == sought[k].some, "map::seek(io, state, Map::Free, from).unwrap().is_some()");
		if (some)
		{
			STD_ASSERT_EQ_U64(__LINE__, 9, found.f0, sought[k].block);
			STD_ASSERT_EQ(__LINE__, 9, txn_memtable_map_tests_seen_eq(&found.f1, &wanted), &found.f1,
						  txn_memtable_map_tests_seen_Debug_fmt, &wanted, txn_memtable_map_tests_seen_Debug_fmt);
			txn_memtable_map_tests_seen_drop(&found.f1);
		}
		txn_memtable_map_tests_seen_drop(&wanted);
	}
	if (!txn_memtable_map_holds(io, state, TXN_MEMTABLE_MAP_MAP_FREE, 7, 4242, &holds, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(__LINE__, 9, holds, "map::holds(io, state, Map::Free, 7, 4242).unwrap()");
	if (!txn_memtable_map_holds(io, state, TXN_MEMTABLE_MAP_MAP_FREE, 7, 4243, &holds, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(__LINE__, 9, !holds, "!map::holds(io, state, Map::Free, 7, 4243).unwrap()");
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/*
 * An aborted transaction's free space map spill, whose takes are never
 * taken back: its abort and every checkpoint's naming keep it listed, it
 * holds no repository's chunk spills back, and the fold's drop takes it out
 * once a fold applied its takes; another transaction's helper at the budget
 * spills a storage's free space map too.
 */
void
test_txn_spill_tests_an_aborted_free_space_map_spill_stays_listed_until_a_fold(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9763);
	HostSharedRecord state = host_shared_Held_record(&held);
	HostStorage storage = txn_memtable_testing_storage(9763);
	HostXid		aborting = simc_begin();
	HostXid		other = simc_begin();
	const struct
	{
		uint64		xid;
		uint32		block;
	}			pushes[] = {{aborting.f0, 7}, {other.f0, 9}};
	TxnMemtableMapEntry e = {.has_value = true};
	TxnMemtableMapEntry entry;
	txn_TupleVecFileU64_t listed;
	StdVec		names;
	HostError	error;
	bool		spilled;
	bool		unfolded;
	bool		swept;
	bool		some;

	for (size_t k = 0; k < lengthof(pushes); k++)
	{
		TxnMemtableOp op = txn_memtable_testing_put((const uint8 *) "e", 1);
		TxnMemtableVersionChange change = {.writer = txn_memtable_testing_writer(pushes[k].xid), .op = &op,
		.lock = TXN_MEMTABLE_LOCK_MODE_UPDATE, .shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND}};
		TxnMemtableVersionHandle handle;
		uint8		key[4];

		op.u.put.heap_bytes = 0;
		std_u32_to_be_bytes(pushes[k].block, key);
		if (!txn_memtable_table_alone_State(io, state, true, TXN_MEMTABLE_MAP_MAP_FREE, key, sizeof(key), &change,
											&handle, &error))
			STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		txn_memtable_Op_drop(&op);
	}
	if (!txn_spill_spilled(io, &storage, TXN_WRITTEN_OF_FREE, aborting.f0, &spilled, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(__LINE__, 9, spilled, "spilled(io, &storage(9763), Of::Free, aborting.0).unwrap()");
	simc_abort(aborting);
	if (!txn_spill_aborted(io, aborting.f0, &storage, 1, 150, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!txn_spill_listed(&TXN_SPILL_STATE_HOME_OPS, io, state, &storage, &listed, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT_MSG(__LINE__, 9, listed.f0.len == 1 &&
				   STD_VEC_AT(&listed.f0, TxnSpillFile, 0).kind == TXN_RUNS_KIND_SPILLED_FREE,
				   "the abort keeps the spill");
	std_vec_free_Checked(&listed.f0, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	if (!txn_spill_named(&TXN_SPILL_STATE_HOME_OPS, io, state, &storage, 1, &names, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(__LINE__, 9, names.len == 1 && STD_VEC_AT(&names, RepoRunFile, 0).kind == TXN_RUNS_KIND_SPILLED_FREE,
			   "names.iter().map(|file| file.kind).collect::<Vec<_>>() == [Kind::SpilledFree as u8]");
	std_vec_free_Checked(&names, sizeof(RepoRunFile), _Alignof(RepoRunFile));
	if (!txn_spill_listed(&TXN_SPILL_STATE_HOME_OPS, io, state, &storage, &listed, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT_MSG(__LINE__, 9, listed.f0.len == 1 &&
				   STD_VEC_AT(&listed.f0, TxnSpillFile, 0).kind == TXN_RUNS_KIND_SPILLED_FREE,
				   "a checkpoint's naming keeps the spill");
	std_vec_free_Checked(&listed.f0, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	if (!txn_spill_unfolded_since(io, &held, 1, &unfolded, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT_MSG(__LINE__, 9, !unfolded, "a free space map spill holds no chunks back");
	if (!txn_spill_swept(io, other.f0, &swept, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT_MSG(__LINE__, 9, swept, "a helper spills another transaction's free space map");
	if (!txn_memtable_map_newest(io, state, TXN_MEMTABLE_MAP_MAP_FREE, 9, &some, &entry, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	std_slice_to_vec_Checked(&e.value, (const uint8 *) "e", 1, 1, 1);
	STD_ASSERT(__LINE__, 9, some && txn_memtable_map_tests_seen_eq(&entry, &e),
			   "map::newest(io, state, Map::Free, 9).unwrap() == Some(Some(b\"e\".to_vec()))");
	txn_memtable_map_tests_seen_drop(&e);
	if (some)
		txn_memtable_map_tests_seen_drop(&entry);
	if (!txn_spill_folded(io, state, 1, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!txn_spill_listed(&TXN_SPILL_STATE_HOME_OPS, io, state, &storage, &listed, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT_MSG(__LINE__, 9, listed.f0.len == 1 && STD_VEC_AT(&listed.f0, TxnSpillFile, 0).xid == other.f0,
				   "the fold's drop takes the aborted spill and keeps a running one's");
	std_vec_free_Checked(&listed.f0, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/*
 * The budget's committed arm: a key a running transaction and a committed
 * one spilled, a third, committed, writing it again in the memtable, is
 * relieved by spilling the third's versions, the chain's oldest in the
 * memtable, which read newest above the spilled ones; a key whose oldest
 * memtable version a running transaction wrote keeps the committed version
 * above it, and the arm then has nothing left to spill.
 */
void
test_txn_spill_tests_the_committed_arm_spills_a_keys_oldest_committed_versions_above_its_spills(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9771);
	HostSharedRecord state = host_shared_Held_record(&held);
	HostStorage storage = txn_memtable_testing_storage(9771);
	HostXid		running = simc_begin();
	HostXid		spilled_committed = simc_begin();
	HostXid		above = simc_begin();
	HostXid		holder = {0};
	HostXid		after = {0};
	struct
	{
		HostXid		xid;
		const char *key;
		const char *value;
		bool		spills;
	}			pushes[] = {{running, "k", "1", true}, {spilled_committed, "k", "2", true}, {above, "k", "3"},
	{{0}, "m", "4"}, {{0}, "m", "5"}};
	struct
	{
		HostXid		xid;
		HostLsn		end;
	}			commits[3];
	uint64		before[3];
	HostDeltaHold hold;
	StdVec		chain;
	StdVec		versions;
	HostError	error;
	bool		spilled;
	bool		relieved;

	for (size_t k = 0; k < lengthof(pushes); k++)
	{
		TxnMemtableOp op = txn_memtable_testing_put((const uint8 *) pushes[k].value, 1);
		TxnMemtableVersionChange change = {.op = &op, .lock = TXN_MEMTABLE_LOCK_MODE_UPDATE,
		.shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND}};
		TxnMemtableVersionHandle handle;

		if (k == 3)
		{
			holder = simc_begin();
			after = simc_begin();
			pushes[3].xid = holder;
			pushes[4].xid = after;
		}
		change.writer = txn_memtable_testing_writer(pushes[k].xid.f0);
		if (!txn_memtable_table_push(io, state, (const uint8 *) pushes[k].key, 1, &change, &handle, &error))
			STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		txn_memtable_Op_drop(&op);
		if (!pushes[k].spills)
			continue;
		if (!txn_spill_spilled(io, &storage, TXN_WRITTEN_OF_ROWS, pushes[k].xid.f0, &spilled, &error))
			STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		STD_ASSERT(__LINE__, 9, spilled, "spilled(io, &storage(9771), Of::Rows, xid.0).unwrap()");
	}
	commits[0].xid = spilled_committed;
	commits[0].end = 100;
	commits[1].xid = above;
	commits[1].end = 200;
	commits[2].xid = after;
	commits[2].end = 300;
	for (size_t k = 0; k < lengthof(commits); k++)
	{
		TxnStatusOutcome committed = {.tag = TXN_STATUS_OUTCOME_COMMITTED, .u.committed = commits[k].end};
		TxnStatusRoomed roomed;
		TxnStatusEntry entry;
		bool		took;
		bool		has;

		if (!txn_status_take(io, commits[k].xid, committed, &roomed, &took, &error))
			STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		simc_commit(commits[k].xid);
		if (!txn_status_lookup(io, commits[k].xid, &has, &entry, &error))
			STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		STD_ASSERT(__LINE__, 9, has, "status::lookup(io, xid).unwrap().is_some()");
		before[k] = entry.count;
	}
	if (!txn_spill_relieved(io, &relieved, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT_MSG(__LINE__, 9, relieved, "the committed arm spills the third writer's versions");
	if (!host_delta_hold(io, &hold, &error) ||
		!txn_memtable_table_chain(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, io, &hold, state, (const uint8 *) "k", 1, &chain,
								  &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT_MSG(__LINE__, 9, chain.len == 0, "k left the memtable");
	txn_spill_tests_versions_free(&chain);
	if (!txn_spill_versions(&TXN_SPILL_STATE_HOME_OPS, io, &hold, state, TXN_RUNS_KIND_SPILLED_ROWS,
							(const uint8 *) "k", 1, &versions, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	{
		const uint64 wanted[] = {above.f0, spilled_committed.f0, running.f0};
		bool		eq = versions.len == lengthof(wanted);

		for (size_t k = 0; eq && k < versions.len; k++)
		{
			TxnMemtableVersionRecord record =
				txn_memtable_version_Version_record(&STD_VEC_AT(&versions, TxnMemtableVersionVersion, k));

			eq = txn_memtable_version_Record_writer(&record).xid == wanted[k];
		}
		STD_ASSERT_MSG(__LINE__, 9, eq, "newest first");
	}
	txn_spill_tests_versions_free(&versions);
	if (!txn_memtable_table_chain(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, io, &hold, state, (const uint8 *) "m", 1, &chain,
								  &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT_EQ_U64(__LINE__, 9, chain.len, 2);
	txn_spill_tests_versions_free(&chain);
	host_delta_Hold_drop(&hold);
	for (size_t k = 0; k < lengthof(commits); k++)
	{
		TxnStatusEntry entry;
		bool		has;

		if (!txn_status_lookup(io, commits[k].xid, &has, &entry, &error))
			STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		STD_ASSERT_MSG(__LINE__, 9, has && entry.count == before[k], "the spilled versions keep their counts");
	}
	if (!txn_spill_relieved(io, &relieved, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT_MSG(__LINE__, 9, !relieved, "nothing committed is left at a chain's bottom");
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/*
 * The committed arm's spill after a restart: the versions it spilled, named
 * by a checkpoint, read back after a restart in the order they read before
 * it, newest first above the older transactions' spilled versions, each
 * counted once at its writer's entry.
 */
void
test_txn_spill_tests_the_committed_arms_spill_reads_back_in_order_after_a_restart(void)
{
	const TxnInsertPage page = {.bytes = 8160, .most = 291};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9772);
	HostStorage storage = txn_memtable_testing_storage(9772);
	HostXid		running = simc_begin();
	HostXid		spilled_committed = simc_begin();
	HostXid		above = simc_begin();
	const HostXid pushes[] = {running, spilled_committed, above};
	const char *const values[] = {"1", "2", "3"};
	const HostXid committed[] = {spilled_committed, above};
	const HostLsn ends[] = {100, 200};
	const uint64 wanted[] = {above.f0, spilled_committed.f0, running.f0};
	HostError	error;
	bool		relieved;

	for (size_t k = 0; k < lengthof(pushes); k++)
	{
		TxnMemtableOp op = txn_memtable_testing_put((const uint8 *) values[k], 1);
		TxnMemtableVersionChange change = {.writer = txn_memtable_testing_writer(pushes[k].f0), .op = &op,
		.lock = TXN_MEMTABLE_LOCK_MODE_UPDATE, .shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND}};
		TxnMemtableVersionHandle handle;
		bool		spilled;

		if (!txn_memtable_table_push(io, host_shared_Held_record(&held), (const uint8 *) "k", 1, &change, &handle,
									 &error))
			STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		txn_memtable_Op_drop(&op);
		if (k == 2)
			continue;
		if (!txn_spill_spilled(io, &storage, TXN_WRITTEN_OF_ROWS, pushes[k].f0, &spilled, &error))
			STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		STD_ASSERT(__LINE__, 9, spilled, "spilled(io, &storage(9772), Of::Rows, xid.0).unwrap()");
	}
	for (size_t k = 0; k < lengthof(committed); k++)
	{
		TxnStatusOutcome outcome = {.tag = TXN_STATUS_OUTCOME_COMMITTED, .u.committed = ends[k]};
		TxnStatusRoomed roomed;
		bool		took;

		if (!txn_status_take(io, committed[k], outcome, &roomed, &took, &error))
			STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		simc_commit(committed[k]);
	}
	if (!txn_spill_relieved(io, &relieved, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT_MSG(__LINE__, 9, relieved, "the committed arm spills the third writer's version");
	for (int restarted = 0; restarted < 2; restarted++)
	{
		HostDeltaHold hold;
		StdVec		chain;
		StdVec		versions;
		bool		eq;

		if (restarted)
		{
			host_shared_Held_drop(&held);
			simc_checkpointed(UINT64CONST(0x100000a27));
			if (!txn_runs_checkpoint(io, UINT64CONST(0x100000a28), &error) ||
				!txn_state_unlinked(io, &storage, &error))
				STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
										 host_Error_Debug_fmt);
			for (size_t k = 0; k < lengthof(committed); k++)
			{
				bool		removed;

				if (!txn_status_remove(io, committed[k], &removed, &error))
					STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
											 host_Error_Debug_fmt);
				STD_ASSERT(__LINE__, 9, removed, "status::remove(io, xid).unwrap()");
			}
			if (!txn_runs_loaded(io, &page, &error) || !txn_memtable_testing_stated(io, 9772, &held, &error))
				STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
										 host_Error_Debug_fmt);
		}
		if (!host_delta_hold(io, &hold, &error) ||
			!txn_memtable_table_chain(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, io, &hold, host_shared_Held_record(&held),
									  (const uint8 *) "k", 1, &chain, &error))
			STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		STD_ASSERT(__LINE__, 9, chain.len == 0, "table::chain::<Rows>(io, &hold, state, b\"k\").unwrap().is_empty()");
		txn_spill_tests_versions_free(&chain);
		if (!txn_spill_versions(&TXN_SPILL_STATE_HOME_OPS, io, &hold, host_shared_Held_record(&held),
								TXN_RUNS_KIND_SPILLED_ROWS, (const uint8 *) "k", 1, &versions, &error))
			STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		eq = versions.len == lengthof(wanted);
		for (size_t k = 0; eq && k < versions.len; k++)
		{
			TxnMemtableVersionRecord record =
				txn_memtable_version_Version_record(&STD_VEC_AT(&versions, TxnMemtableVersionVersion, k));

			eq = txn_memtable_version_Record_writer(&record).xid == wanted[k];
		}
		if (restarted)
			STD_ASSERT_MSG(__LINE__, 9, eq, "the same order after the start");
		else
			STD_ASSERT_MSG(__LINE__, 9, eq, "newest first");
		txn_spill_tests_versions_free(&versions);
		host_delta_Hold_drop(&hold);
	}
	for (size_t k = 0; k < lengthof(committed); k++)
	{
		TxnStatusEntry entry;
		bool		has;

		if (!txn_status_lookup(io, committed[k], &has, &entry, &error))
			STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		STD_ASSERT(__LINE__, 9, has && entry.count == 1, "counts == [Some(1), Some(1)]");
	}
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/*
 * The crash mark, run file field 8: a running transaction's free space map
 * spill that a checkpoint named, ended by a crash with no abort record, is
 * marked by the next checkpoint's naming with that checkpoint's redo point,
 * the point a fold passes it at, which a later start loads back with the
 * file.
 */
void
test_txn_spill_tests_a_free_space_spill_a_crash_ended_is_marked_with_the_next_checkpoints_redo_point(void)
{
	const TxnInsertPage page = {.bytes = 8160, .most = 291};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9773);
	HostStorage storage = txn_memtable_testing_storage(9773);
	HostXid		xid = simc_begin();
	TxnFreeSpaceFree space = txn_free_space_Free_new(8000, NULL, 0, 291);
	StdVec		value;
	TxnMemtableOp op;
	TxnMemtableVersionChange change = {.writer = txn_memtable_testing_writer(xid.f0), .op = &op,
	.lock = TXN_MEMTABLE_LOCK_MODE_UPDATE, .shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND}};
	TxnMemtableVersionHandle handle;
	uint8		key[4];
	HostError	error;
	bool		spilled;

	std_vec_new(&value, 1, 1);
	txn_free_space_Free_encode_into(&space, &value);
	op = txn_memtable_testing_put(value.ptr, value.len);
	op.u.put.tuple.f0 = 7 << 16 | 1;
	op.u.put.heap_bytes = 0;
	std_vec_free_Checked(&value, 1, 1);
	std_u32_to_be_bytes(7, key);
	if (!txn_memtable_table_alone_State(io, host_shared_Held_record(&held), true, TXN_MEMTABLE_MAP_MAP_FREE, key,
										sizeof(key), &change, &handle, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	txn_memtable_Op_drop(&op);
	if (!txn_spill_spilled(io, &storage, TXN_WRITTEN_OF_FREE, xid.f0, &spilled, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(__LINE__, 9, spilled, "spilled(io, &storage(9773), Of::Free, xid).unwrap()");
	host_shared_Held_drop(&held);
	simc_checkpointed(UINT64CONST(0x100000a27));
	if (!txn_runs_checkpoint(io, UINT64CONST(0x100000a28), &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	for (int round = 0; round < 3; round++)
	{
		static const HostLsn marks[] = {0, UINT64CONST(0x100000b28), UINT64CONST(0x100000b28)};
		static const char *const messages[] = {"named unmarked while its transaction ran",
		"the crash's end marked at the next naming", "the mark loads back"};
		txn_TupleVecFileU64_t listed;
		size_t		found = 0;
		bool		marked = true;

		if (round == 1)
		{
			simc_abort(xid);
			simc_checkpointed(UINT64CONST(0x100000b27));
			if (!txn_runs_checkpoint(io, UINT64CONST(0x100000b28), &error))
				STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
										 host_Error_Debug_fmt);
		}
		else if (!txn_state_unlinked(io, &storage, &error) || !txn_runs_loaded(io, &page, &error))
			STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		if (!txn_memtable_testing_stated(io, 9773, &held, &error) ||
			!txn_spill_listed(&TXN_SPILL_STATE_HOME_OPS, io, host_shared_Held_record(&held), &storage, &listed,
							  &error))
			STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		for (size_t k = 0; k < listed.f0.len; k++)
		{
			const TxnSpillFile *file = &STD_VEC_AT(&listed.f0, TxnSpillFile, k);

			if (file->xid != xid.f0)
				continue;
			found++;
			marked &= file->passed == marks[round];
		}
		if (found != 1 || !marked)
			std_panic(RUST_FILE, __LINE__, 9, messages[round], strlen(messages[round]));
		std_vec_free_Checked(&listed.f0, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
		host_shared_Held_drop(&held);
	}
	simc_TestIo_drop(&test_io);
}

/*
 * A prepared transaction's spills: its rows' and its free space map's, its
 * entry unowned once it prepared, are kept by a fold's drop at any point, as
 * a commit it hasn't made can't apply, named by a checkpoint, and loaded
 * back and read by a start, never taken as a committed or an aborted
 * transaction's.
 */
void
test_txn_spill_tests_a_prepared_transactions_spills_are_never_taken_out(void)
{
	const TxnInsertPage page = {.bytes = 8160, .most = 291};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9774);
	HostStorage storage = txn_memtable_testing_storage(9774);
	HostXid		xid = simc_begin();
	TxnFreeSpaceFree space = txn_free_space_Free_new(8000, NULL, 0, 291);
	TxnMemtableMapEntry entry = {.has_value = true};
	TxnMemtableOp row = txn_memtable_testing_put((const uint8 *) "r", 1);
	TxnMemtableOp take;
	TxnMemtableVersionChange change = {.writer = txn_memtable_testing_writer(xid.f0), .op = &row,
	.lock = TXN_MEMTABLE_LOCK_MODE_UPDATE, .shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND}};
	TxnStatusOutcome unowned = {.tag = TXN_STATUS_OUTCOME_UNOWNED};
	const TxnWrittenOf ofs[] = {TXN_WRITTEN_OF_ROWS, TXN_WRITTEN_OF_FREE};
	TxnStatusRoomed roomed;
	TxnMemtableVersionHandle handle;
	uint8		key[4];
	HostError	error;
	bool		took;

	std_vec_new(&entry.value, 1, 1);
	txn_free_space_Free_encode_into(&space, &entry.value);
	take = txn_memtable_testing_put(entry.value.ptr, entry.value.len);
	take.u.put.tuple.f0 = 7 << 16 | 1;
	take.u.put.heap_bytes = 0;
	if (!txn_memtable_table_push(io, host_shared_Held_record(&held), (const uint8 *) "k", 1, &change, &handle, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	change.op = &take;
	std_u32_to_be_bytes(7, key);
	if (!txn_memtable_table_alone_State(io, host_shared_Held_record(&held), true, TXN_MEMTABLE_MAP_MAP_FREE, key,
										sizeof(key), &change, &handle, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	txn_memtable_Op_drop(&take);
	txn_memtable_Op_drop(&row);
	for (size_t k = 0; k < lengthof(ofs); k++)
	{
		bool		spilled;

		if (!txn_spill_spilled(io, &storage, ofs[k], xid.f0, &spilled, &error))
			STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		STD_ASSERT(__LINE__, 9, spilled, "spilled(io, &storage(9774), of, xid).unwrap()");
	}
	if (!txn_status_take(io, xid, unowned, &roomed, &took, &error) ||
		!txn_spill_folded(io, host_shared_Held_record(&held), PG_UINT64_MAX >> 2, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	for (int restarted = 0; restarted < 2; restarted++)
	{
		txn_TupleVecFileU64_t listed;
		TxnMemtableMapEntry newest;
		HostDeltaHold hold;
		StdVec		versions;
		bool		some;
		bool		kept;

		if (restarted)
		{
			host_shared_Held_drop(&held);
			simc_checkpointed(UINT64CONST(0x100000a27));
			if (!txn_runs_checkpoint(io, UINT64CONST(0x100000a28), &error) ||
				!txn_state_unlinked(io, &storage, &error) || !txn_runs_loaded(io, &page, &error) ||
				!txn_memtable_testing_stated(io, 9774, &held, &error))
				STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
										 host_Error_Debug_fmt);
		}
		if (!txn_spill_listed(&TXN_SPILL_STATE_HOME_OPS, io, host_shared_Held_record(&held), &storage, &listed,
							  &error) ||
			!host_delta_hold(io, &hold, &error) ||
			!txn_spill_versions(&TXN_SPILL_STATE_HOME_OPS, io, &hold, host_shared_Held_record(&held),
								TXN_RUNS_KIND_SPILLED_ROWS, (const uint8 *) "k", 1, &versions, &error))
			STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		host_delta_Hold_drop(&hold);
		if (!txn_memtable_map_newest(io, host_shared_Held_record(&held), TXN_MEMTABLE_MAP_MAP_FREE, 7, &some, &newest,
									 &error))
			STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		kept = listed.f0.len == 3 && versions.len == 1 && some && txn_memtable_map_tests_seen_eq(&newest, &entry);
		if (restarted)
			STD_ASSERT_MSG(__LINE__, 9, kept, "named and loaded back");
		else
			STD_ASSERT_MSG(__LINE__, 9, kept, "kept by the drop");
		if (some)
			txn_memtable_map_tests_seen_drop(&newest);
		txn_spill_tests_versions_free(&versions);
		std_vec_free_Checked(&listed.f0, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	}
	txn_memtable_map_tests_seen_drop(&entry);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/*
 * A free space map spill's merge joins its own files alone: a transaction
 * whose rows spilled once, with their file of tuple ids, and whose free
 * space map spilled twice, merges its two free space map files into one and
 * leaves the rows' tuple ids as they were, listing no file of tuple ids no
 * row file has, so every row's tuple id still finds its key.
 */
void
test_txn_spill_tests_a_free_space_spills_merge_leaves_the_rows_tuple_ids_alone(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9776);
	HostSharedRecord state = host_shared_Held_record(&held);
	HostStorage storage = txn_memtable_testing_storage(9776);
	uint64		xid = simc_begin().f0;
	TxnFreeSpaceFree space = txn_free_space_Free_new(8000, NULL, 0, 291);
	static const uint32 blocks[] = {7, 8};
	txn_TupleVecFileU64_t listed;
	StdVec		value;
	StdVec		rows;
	StdVec		tuples;
	HostDeltaHold hold;
	HostError	error;
	bool		spilled;
	size_t		frees = 0;

	for (uint64 n = 1; n <= 3; n++)
	{
		TxnMemtableOp row = txn_memtable_testing_put((const uint8 *) "row", 3);
		TxnMemtableVersionChange change = {.writer = txn_memtable_testing_writer(xid), .op = &row,
		.lock = TXN_MEMTABLE_LOCK_MODE_UPDATE, .shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND}};
		TxnMemtableVersionHandle handle;
		uint8		key[8];

		row.u.put.tuple.f0 = 1 << 16 | n;
		std_u64_to_be_bytes(n, key);
		if (!txn_memtable_table_push(io, state, key, sizeof(key), &change, &handle, &error))
			STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		txn_memtable_Op_drop(&row);
	}
	if (!txn_spill_spilled(io, &storage, TXN_WRITTEN_OF_ROWS, xid, &spilled, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(__LINE__, 9, spilled, "spilled(io, &storage(9776), Of::Rows, xid).unwrap()");
	std_vec_new(&value, 1, 1);
	txn_free_space_Free_encode_into(&space, &value);
	for (size_t k = 0; k < lengthof(blocks); k++)
	{
		TxnMemtableOp take = txn_memtable_testing_put(value.ptr, value.len);
		TxnMemtableVersionChange change = {.writer = txn_memtable_testing_writer(xid), .op = &take,
		.lock = TXN_MEMTABLE_LOCK_MODE_UPDATE, .shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND}};
		TxnMemtableVersionHandle handle;
		uint8		key[4];

		take.u.put.tuple.f0 = 7 << 16 | 1;
		take.u.put.heap_bytes = 0;
		std_u32_to_be_bytes(blocks[k], key);
		if (!txn_memtable_table_alone_State(io, state, true, TXN_MEMTABLE_MAP_MAP_FREE, key, sizeof(key), &change,
											&handle, &error) ||
			!txn_spill_spilled(io, &storage, TXN_WRITTEN_OF_FREE, xid, &spilled, &error))
			STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		STD_ASSERT(__LINE__, 9, spilled, "spilled(io, &storage(9776), Of::Free, xid).unwrap()");
		txn_memtable_Op_drop(&take);
	}
	std_vec_free_Checked(&value, 1, 1);
	if (!txn_spill_listed(&TXN_SPILL_STATE_HOME_OPS, io, state, &storage, &listed, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	std_vec_new(&rows, sizeof(uint32), _Alignof(uint32));
	std_vec_new(&tuples, sizeof(uint32), _Alignof(uint32));
	for (size_t k = 0; k < listed.f0.len; k++)
	{
		const TxnSpillFile *file = &STD_VEC_AT(&listed.f0, TxnSpillFile, k);

		if (file->kind == TXN_RUNS_KIND_SPILLED_FREE)
			frees++;
		else if (file->kind == TXN_RUNS_KIND_SPILLED_ROWS)
			std_vec_push_Checked(&rows, &file->number, sizeof(uint32), _Alignof(uint32));
		else if (file->kind == TXN_RUNS_KIND_SPILLED_TUPLES)
			std_vec_push_Checked(&tuples, &file->number, sizeof(uint32), _Alignof(uint32));
	}
	STD_ASSERT_MSG(__LINE__, 9, frees == 1, "the free space map's files merged");
	STD_ASSERT_MSG(__LINE__, 9, rows.len == tuples.len &&
				   (rows.len == 0 || memcmp(rows.ptr, tuples.ptr, rows.len * sizeof(uint32)) == 0),
				   "no tuple ids without their row file");
	std_vec_free_Checked(&rows, sizeof(uint32), _Alignof(uint32));
	std_vec_free_Checked(&tuples, sizeof(uint32), _Alignof(uint32));
	std_vec_free_Checked(&listed.f0, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	if (!host_delta_hold(io, &hold, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	for (uint64 n = 1; n <= 3; n++)
	{
		HostRow		tid = {1 << 16 | n};
		uint8		key[8];
		StdVec		found;
		bool		some;

		std_u64_to_be_bytes(n, key);
		if (!txn_spill_tuple_key(io, &hold, state, tid, &some, &found, &error))
			STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		STD_ASSERT(__LINE__, 9, some && found.len == sizeof(key) && memcmp(found.ptr, key, sizeof(key)) == 0,
				   "tuple_key(io, &hold, state, Row(1 << 16 | n)).unwrap() == Some(n.to_be_bytes().to_vec())");
		if (some)
			std_vec_free_Checked(&found, 1, 1);
	}
	host_delta_Hold_drop(&hold);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}


/* txn_spill_tests_residual_t is a failed residual's captures: the io, the state, the kept version, the files listed, and whether it listed them. */
typedef struct txn_spill_tests_residual_t
{
	HostIoIo   *io;
	HostSharedRecord state;
	const TxnSpillKept *kept;
	StdVec		files;
	bool		listed;
	bool		must;
} txn_spill_tests_residual_t;

/* txn_spill_tests_residual_body lists the kept version's residual at a point, an error answering it unlisted. */
static void
txn_spill_tests_residual_body(void *captures)
{
	txn_spill_tests_residual_t *c = captures;
	HostError	error;

	c->listed = txn_spill_residual(c->io, c->state, 7, c->kept, 1, &c->files, &error);
	if (!c->listed && c->must)
		STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!c->listed)
		host_Error_drop(&error);
}

/* txn_spill_tests_entry_count returns an xid's status entry's count. */
static uint64
txn_spill_tests_entry_count(HostIoIo *io, HostXid xid)
{
	TxnStatusEntry entry;
	HostError	error;
	bool		has;

	if (!txn_status_lookup(io, xid, &has, &entry, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(__LINE__, 9, has, "the writer has an entry");
	return entry.count;
}

/*
 * The residual's count order (bugs row 291's review R1): a residual adds its
 * versions' count at their writer's entry before the manifest's hold, and
 * its pad gives it back unless the listing finished, so a residual that
 * fails or panics at any step lists no file and leaves the entry's count
 * where it was, and one that finishes lists its file with the count added.
 */
void
test_txn_spill_tests_a_residual_whose_listing_fails_lists_nothing_and_gives_its_count_back(void)
{
	bool		failed = false;

	for (uint64 nth = 0; nth < 200; nth++)
	{
		SimcTestIo	test_io = simc_io();
		HostIoIo   *io = &test_io.io;
		HostSharedHeld held = txn_memtable_testing_created(io, 9760);
		HostSharedRecord state = host_shared_Held_record(&held);
		HostXid		writer = simc_begin();
		TxnMemtableOp op = txn_memtable_testing_put((const uint8 *) "v", 1);
		TxnMemtableVersionWriter written = txn_memtable_testing_writer(writer.f0);
		TxnMemtableVersionShown shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND};
		TxnSpillKept kept = {.kind = TXN_RUNS_KIND_ROWS};
		txn_spill_tests_residual_t residual = {.io = io, .state = state, .kept = &kept, .must = nth == 0};
		TxnStatusRoomed roomed;
		HostError	error;
		StdCaught	caught;
		StdVec		listed;
		uint64		before;

		if (!txn_status_roomed(io, writer, &roomed, &error))
			STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		std_slice_to_vec_Checked(&kept.key, "a", 1, 1, 1);
		std_vec_new(&kept.bytes, 1, 1);
		txn_memtable_version_encode(&written, &op, TXN_MEMTABLE_LOCK_MODE_UPDATE, shown, false, 0, &kept.bytes);
		before = txn_spill_tests_entry_count(io, writer);
		simc_fail_allocation(nth);
		if (!std_catch_unwind(txn_spill_tests_residual_body, &residual, &caught))
			std_Caught_drop_Checked(&caught);
		simc_fail_allocation(0);
		listed = txn_spill_tests_files_of(io, state);
		if (!residual.listed)
		{
			failed = true;
			STD_ASSERT_EQ_U64(__LINE__, 9, listed.len, 0);
			STD_ASSERT_EQ_U64(__LINE__, 9, txn_spill_tests_entry_count(io, writer), before);
		}
		else
		{
			STD_ASSERT_EQ_U64(__LINE__, 9, listed.len, 1);
			STD_ASSERT_EQ_U64(__LINE__, 9, txn_spill_tests_entry_count(io, writer), before + 1);
			std_vec_free_Checked(&residual.files, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
		}
		std_vec_free_Checked(&listed, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
		std_vec_free_Checked(&kept.key, 1, 1);
		std_vec_free_Checked(&kept.bytes, 1, 1);
		txn_memtable_Op_drop(&op);
		host_shared_Held_drop(&held);
		simc_TestIo_drop(&test_io);
		if (nth > 0 && residual.listed)
			break;
	}
	STD_ASSERT(__LINE__, 9, failed, "a failed allocation failed the residual");
}


/* txn_spill_tests_crashed_spill spills a crashed transaction's rows or free space map versions, one version, a key or a block. */
static void
txn_spill_tests_crashed_spill(HostIoIo *io, HostSharedRecord state, const HostStorage *storage, HostXid xid,
							  TxnWrittenOf of)
{
	TxnMemtableOp op = txn_memtable_testing_put((const uint8 *) "e", 1);
	TxnMemtableVersionChange change = {.writer = txn_memtable_testing_writer(xid.f0), .op = &op,
	.lock = TXN_MEMTABLE_LOCK_MODE_UPDATE, .shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND}};
	TxnMemtableVersionHandle handle;
	HostError	error;
	bool		pushed;
	bool		spilled;
	uint8		key[4];

	if (of == TXN_WRITTEN_OF_FREE)
	{
		op.u.put.heap_bytes = 0;
		std_u32_to_be_bytes(7, key);
		pushed = txn_memtable_table_alone_State(io, state, true, TXN_MEMTABLE_MAP_MAP_FREE, key, sizeof(key), &change,
												&handle, &error);
	}
	else
		pushed = txn_memtable_table_push(io, state, (const uint8 *) "a", 1, &change, &handle, &error);
	if (!pushed || !txn_spill_spilled(io, storage, of, xid.f0, &spilled, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(__LINE__, 9, spilled, "the version spilled");
	txn_memtable_Op_drop(&op);
}

/*
 * txn_spill_tests_crashed_count spills a transaction's rows and its free
 * space map versions, in an order, ends it by a crash, with no abort record,
 * drops what a fold passes, and returns its entry's count after the drop.
 */
static uint64
txn_spill_tests_crashed_count(uint32 database, bool rows_first)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, database);
	HostSharedRecord state = host_shared_Held_record(&held);
	HostStorage storage = txn_memtable_testing_storage(database);
	HostXid		crashed = simc_begin();
	TxnStatusRoomed roomed;
	StdVec		listed;
	HostError	error;
	uint64		count;

	if (!txn_status_roomed(io, crashed, &roomed, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	txn_spill_tests_crashed_spill(io, state, &storage, crashed, rows_first ? TXN_WRITTEN_OF_ROWS : TXN_WRITTEN_OF_FREE);
	txn_spill_tests_crashed_spill(io, state, &storage, crashed, rows_first ? TXN_WRITTEN_OF_FREE : TXN_WRITTEN_OF_ROWS);
	simc_abort(crashed);
	if (!txn_spill_folded(io, state, 1, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	listed = txn_spill_tests_files_of(io, state);
	STD_ASSERT_EQ_U64(__LINE__, 9, listed.len, 0);
	std_vec_free_Checked(&listed, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	count = txn_spill_tests_entry_count(io, crashed);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
	return count;
}

/*
 * The crashed transaction's counts (bugs row 291's review Q1): a drop decides
 * every transaction whose files it reads before it counts a file, so a
 * transaction a crash ended, whose rows spill stays listed beside its free
 * space map spill until a checkpoint names them, has both files' counts
 * taken back whichever is listed first.
 */
void
test_txn_spill_tests_a_drop_counts_a_crashed_transactions_files_in_either_order(void)
{
	uint64		rows_first = txn_spill_tests_crashed_count(9764, true);
	uint64		free_first = txn_spill_tests_crashed_count(9765, false);

	STD_ASSERT_EQ_U64(__LINE__, 9, rows_first, free_first);
}


/* txn_spill_tests_carry_t is a carry case's captures: the io, the state, its writers, and whether the carry finished. */
typedef struct txn_spill_tests_carry_t
{
	HostIoIo   *io;
	const HostSharedHeld *held;
	uint64		writers[3];
	bool		carried;
	HostError	error;
} txn_spill_tests_carry_t;

/* txn_spill_tests_carry_logged is the carry case's logged: one row version of each writer, each its own key, as a chain of the rows. */
static bool
txn_spill_tests_carry_logged(void *captures, HostSharedLongIo *io, StdVec *chains, HostError *error)
{
	const txn_spill_tests_carry_t *c = captures;
	TxnMemtableVersionShown shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND};

	for (size_t k = 0; k < lengthof(c->writers); k++)
	{
		TxnMemtableOp op = txn_memtable_testing_put((const uint8 *) "v", 1);
		TxnMemtableVersionWriter writer = txn_memtable_testing_writer(c->writers[k]);
		TxnSpillChain chain = {.of = TXN_WRITTEN_OF_ROWS};
		txn_TupleU64VecU8_t version = {.f0 = c->writers[k]};
		uint8		key = (uint8) ('a' + k);

		op.u.put.tuple.f0 = (uint64) (k + 1);
		std_slice_to_vec_Checked(&chain.key, &key, 1, 1, 1);
		std_vec_new(&version.f1, 1, 1);
		txn_memtable_version_encode(&writer, &op, TXN_MEMTABLE_LOCK_MODE_UPDATE, shown, false, 0, &version.f1);
		std_vec_new(&chain.versions, sizeof(txn_TupleU64VecU8_t), _Alignof(txn_TupleU64VecU8_t));
		std_vec_push_Checked(&chain.versions, &version, sizeof(version), _Alignof(txn_TupleU64VecU8_t));
		std_vec_push_Checked(chains, &chain, sizeof(chain), _Alignof(TxnSpillChain));
		txn_memtable_Op_drop(&op);
	}
	return true;
}

/* txn_spill_tests_carry_body carries the writers' versions, an error answering it unfinished. */
static void
txn_spill_tests_carry_body(void *captures)
{
	txn_spill_tests_carry_t *c = captures;

	c->carried = txn_spill_carried(c->io, c->held, c->writers, lengthof(c->writers), txn_spill_tests_carry_logged, c,
								   &c->error);
}

/*
 * txn_spill_tests_carry_refused runs a carry of three writers' versions with
 * one of its steps refused after a count of them, a page's open or an
 * allocation, and checks it: a carry that failed lists no file and leaves
 * every writer's count where it was, and one that finished lists its files
 * with each writer's count one more; it answers whether the carry failed.
 */
static bool
txn_spill_tests_carry_refused(bool opens, uint64 after)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9766);
	HostSharedRecord state = host_shared_Held_record(&held);
	txn_spill_tests_carry_t carry = {.io = io, .held = &held};
	uint64		before[3];
	StdCaught	caught;
	StdVec		listed;
	HostError	error;
	bool		panicked;

	for (size_t k = 0; k < lengthof(carry.writers); k++)
	{
		HostXid		writer = simc_begin();
		TxnStatusRoomed roomed;

		carry.writers[k] = writer.f0;
		if (!txn_status_roomed(io, writer, &roomed, &error))
			STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		before[k] = txn_spill_tests_entry_count(io, writer);
	}
	if (opens)
		simc_refuse_any_open(after);
	else
		simc_fail_allocation(after);
	panicked = !std_catch_unwind(txn_spill_tests_carry_body, &carry, &caught);
	if (panicked)
		std_Caught_drop_Checked(&caught);
	simc_refuse_nothing();
	simc_fail_allocation(0);
	listed = txn_spill_tests_files_of(io, state);
	if (!carry.carried && !panicked)
		host_Error_drop(&carry.error);
	STD_ASSERT(__LINE__, 9, carry.carried ? listed.len > 0 : listed.len == 0,
			   "a carry lists its files when it finishes and none when it fails");
	for (size_t k = 0; k < lengthof(carry.writers); k++)
	{
		HostXid		writer = {carry.writers[k]};

		STD_ASSERT_EQ_U64(__LINE__, 9, txn_spill_tests_entry_count(io, writer), before[k] + (carry.carried ? 1 : 0));
	}
	std_vec_free_Checked(&listed, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
	return !carry.carried;
}

/*
 * The carry's count order (bugs row 291's review R2): a carry resolves its
 * writers' slots before its hold and adds each listed file's count after it,
 * and a failed add takes the carry's files back out with the counts already
 * added in one checkpoint window, so a carry that fails at any page's open
 * or allocation, the adds' included, lists no file and leaves every
 * writer's count where it was, and one that finishes counts each writer's
 * versions.
 */
void
test_txn_spill_tests_a_carry_that_fails_at_any_step_lists_nothing_and_leaves_every_count(void)
{
	bool		failed = false;

	for (uint64 after = 0; after < 400; after++)
	{
		bool		refused = txn_spill_tests_carry_refused(true, after);

		failed |= refused;
		if (!refused && after > 0)
			break;
	}
	for (uint64 nth = 1; nth < 400; nth++)
		if (!txn_spill_tests_carry_refused(false, nth))
			break;
	STD_ASSERT(__LINE__, 9, failed, "a refused open failed the carry");
}

#endif							/* DOLTGRES_TEST_TXN */
