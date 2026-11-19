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
		case TXN_WRITTEN_OF_IDENTITY:
			*error = txn_spill_identity();
			return false;
		case TXN_WRITTEN_OF_CHUNKS:
			break;
	}
	*error = txn_spill_internal("a storage's state has no chunk tree");
	return false;
}

/* txn_spill_State_tree is the tree of a state's that copied, removed, and chain walk for an Of: its rows or its value map. */
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
};

/* txn_spill_Home_copied hands every key of one of a home's trees that has a version to a walk's step, with no killed version. */
static bool
txn_spill_Home_copied(const TxnSpillHomeOps *home, HostIoIo *io, HostSharedRecord record, TxnWrittenOf of,
					  bool (*each) (void *captures, HostIoIo *io, const uint8 *key, size_t key_len, const StdVec *records,
									size_t records_len, const txn_TupleHandleU64_t *handles, size_t handles_len,
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
		self->len == other->len && self->hash == other->hash && self->bits == other->bits;
}

/* txn_spill_File_line appends a file's line in its storage's manifest to a buffer: its xid, kind, number, length, xxh3, and bits. */
static void
txn_spill_File_line(const TxnSpillFile *self, StdString *lines)
{
	char		line[128];
	int			len = snprintf(line, sizeof(line), UINT64_FORMAT " %u %u " UINT64_FORMAT " " UINT64_FORMAT " "
							   UINT64_FORMAT "\n", self->xid, (unsigned) self->kind, self->number, self->len,
							   self->hash, self->bits);

	std_vec_extend_from_slice_Checked(lines, line, (size_t) len, 1, 1);
}

/* txn_spill_File_parsed returns the file a manifest's line names, or nothing for a line that names none. */
static bool
txn_spill_File_parsed(const char *line, size_t len, TxnSpillFile *result)
{
	uint64		words[6];
	size_t		at = 0;
	StdParseIntError failed;

	for (size_t count = 0; count < 6; count++)
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

/* txn_spill_changed_frame_0_t is changed's state across its pad: the manifest's path, its writers' lock, its files, and their write aside, while live. */
typedef struct txn_spill_changed_frame_0_t
{
	const TxnSpillHomeOps *home;
	HostIoIo   *io;
	HostSharedRecord record;
	const HostStorage *storage;
	void		(*edit) (void *captures, StdVec *files);
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
	StdString	lines;
	bool		lines_live;
	StdPathBuf	aside;
	bool		aside_live;
	HostFile	written;
	bool		written_live;
	HostSharedGuard exclusive;
	bool		exclusive_live;
} txn_spill_changed_frame_0_t;

/* txn_spill_changed_pad_0 is changed's pad: what it holds, while live. */
static void
txn_spill_changed_pad_0(void *frame_)
{
	txn_spill_changed_frame_0_t *frame = frame_;

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

/* txn_spill_changed_written writes the frame's files' lines whole beside the manifest and renames them over it. */
static bool
txn_spill_changed_written(txn_spill_changed_frame_0_t *frame)
{
	HostIoIoDoors io = HOST_SHARED_LONG_IO_IO_DOORS(&frame->long_io);
	ChunksErrorError failed;
	bool		some;

	std_vec_new(&frame->lines, 1, 1);
	frame->lines_live = true;
	for (size_t k = 0; k < frame->files.len; k++)
		txn_spill_File_line(&STD_VEC_AT(&frame->files, TxnSpillFile, k), &frame->lines);
	std_path_with_extension(frame->path.bytes.ptr, frame->path.bytes.len, "new", 3, &frame->aside);
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
	if (!chunks_file_rename(io, frame->aside.bytes.ptr, frame->aside.bytes.len, frame->path.bytes.ptr,
							frame->path.bytes.len, &failed))
	{
		*frame->error = txn_base_storage(&failed);
		return false;
	}
	return true;
}

/* txn_spill_changed_body_0 is changed's body. */
static void
txn_spill_changed_body_0(void *frame_)
{
	txn_spill_changed_frame_0_t *frame = frame_;
	HostIoIoDoors io;
	uint64		generation;
	HostSharedOp load = {.tag = HOST_SHARED_OP_LOAD};
	HostSharedOp exchange = {.tag = HOST_SHARED_OP_EXCHANGE};

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
	if (!host_pause(HOST_SHARED_LONG_IO_PAUSES(&frame->long_io), "doltgres-spill-manifest-read", frame->error))
	{
		txn_spill_changed_pad_0(frame);
		return;
	}
	frame->edit(frame->captures, &frame->files);
	if (frame->files.len == 0)
	{
		ChunksErrorError failed;
		bool		removed;

		if (!chunks_file_remove_if_there(io, frame->path.bytes.ptr, frame->path.bytes.len, &removed, &failed))
		{
			*frame->error = txn_base_storage(&failed);
			txn_spill_changed_pad_0(frame);
			return;
		}
	}
	else if (!txn_spill_changed_written(frame))
	{
		txn_spill_changed_pad_0(frame);
		return;
	}
	if (!txn_state_spills_generation(&generation, frame->error) ||
		!host_shared_LongIo_lock(&frame->long_io, frame->home->short_, &frame->exclusive, frame->error))
	{
		txn_spill_changed_pad_0(frame);
		return;
	}
	frame->exclusive_live = true;
	exchange.u.exchange.f0 = host_shared_Record_atomic(frame->record, frame->home->listed, load);
	exchange.u.exchange.f1 = (uint64) frame->files.len;
	host_shared_Record_atomic(frame->record, frame->home->listed, exchange);
	exchange.u.exchange.f0 = host_shared_Record_atomic(frame->record, frame->home->changed, load);
	exchange.u.exchange.f1 = generation;
	host_shared_Record_atomic(frame->record, frame->home->changed, exchange);
	frame->ok = true;
	txn_spill_changed_pad_0(frame);
}

/*
 * txn_spill_changed changes a storage's spills' manifest under its writers'
 * lock: it reads the manifest file, edits its list, writes the list whole
 * beside it and renames it over the manifest, unsynced, or removes the file
 * once the list is empty; then, under the record's lock held exclusively for
 * the in-memory steps alone, it stores the list's count and a new
 * generation. What the edit answers it leaves in its captures.
 */
static bool
txn_spill_changed(const TxnSpillHomeOps *home, HostIoIo *io, HostSharedRecord record, const HostStorage *storage,
				  void (*edit) (void *captures, StdVec *files), void *captures, HostError *error)
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

/* txn_spill_linked_closure_0 is linked's edit: the files the merge replaces out, then each linked file once. */
static void
txn_spill_linked_closure_0(void *captures, StdVec *files)
{
	txn_spill_linked_captures_t *c = captures;
	size_t		kept = 0;

	for (size_t k = 0; k < files->len; k++)
	{
		const TxnSpillFile *file = &STD_VEC_AT(files, TxnSpillFile, k);

		if (!c->has_replacing || !txn_spill_replaced_by(&c->replacing, 1, file))
			STD_VEC_AT(files, TxnSpillFile, kept++) = *file;
	}
	files->len = kept;
	for (size_t j = 0; j < c->linking_len; j++)
	{
		bool		listed = false;

		for (size_t k = 0; k < files->len && !listed; k++)
			listed = txn_spill_File_PartialEq_eq(&STD_VEC_AT(files, TxnSpillFile, k), &c->linking[j]);
		if (!listed)
			std_vec_push_Checked(files, &c->linking[j], sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	}
}

/*
 * txn_spill_linked lists a spill's files in its storage's manifest, the
 * newest, a row spill's tuple-id file after its row file, each once; a
 * merge's files take the place of the files it replaces, in the same change.
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
static void
txn_spill_removed_closure_0(void *captures, StdVec *files)
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
static bool
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
								   const HostStorage *storage, TxnWrittenOf of, uint64 xid, bool *result,
								   HostError *error);

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
	return txn_spill_spilled_from(home, c->io, held, c->storage, c->of, c->xid, c->result, error);
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

/* txn_spill_tid returns the tuple id a row version's record holds, the row a put, an add, an added row, or an update names. */
static bool
txn_spill_tid(const uint8 *record, size_t record_len, uint64 *result)
{
	TxnMemtableVersionRecord stored = txn_memtable_version_Record_of(record, record_len);
	TxnMemtableVersionOpRef op = txn_memtable_version_Record_op(&stored);

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

/* txn_spill_from_closure_0 is spilled_from's walk's step: a key whose chain holds only the transaction's versions written, its tuple ids and its handles taken. */
static bool
txn_spill_from_closure_0(void *captures, HostIoIo *io, const uint8 *key, size_t key_len, const StdVec *records,
						 size_t records_len, const txn_TupleHandleU64_t *handles, size_t handles_len,
						 HostError *error)
{
	txn_spill_from_frame_0_t *frame = captures;
	txn_TupleVecU8VecHandle_t took;

	for (size_t k = 0; k < handles_len; k++)
		if (handles[k].f1 != frame->xid)
			return true;
	if (records_len > 0)
	{
		frame->records.len = 0;
		for (size_t k = 0; k < records_len; k++)
		{
			StdBytes	record = {records[k].ptr, records[k].len};

			std_vec_push_Checked(&frame->records, &record, sizeof(record), _Alignof(StdBytes));
		}
		if (!txn_runs_Writing_key(frame->writing, io, key, key_len, frame->records.ptr, frame->records.len, error))
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

static bool txn_spill_tupled(const TxnSpillHomeOps *home, HostIoIo *io, HostSharedRecord record,
							 const HostStorage *storage, const StdPathBuf *dir, uint64 xid, uint32 number,
							 StdVec *tuples, TxnSpillFile *result_0, StdVec *result_1, HostError *error);
static bool txn_spill_merged(const TxnSpillHomeOps *home, HostIoIo *io, HostSharedRecord record,
							 const HostStorage *storage, uint64 xid, TxnRunsKind kind, TxnRunsTail tail,
							 HostError *error);

/* txn_spill_from_ended ends a spill that took its tree's count down, as every way out of spilled_from does. */
static bool
txn_spill_from_ended(txn_spill_from_frame_0_t *frame)
{
	if (!txn_written_spilled(frame->io, frame->storage, frame->of, frame->error))
		return false;
	txn_spill_unreplayed(frame->io, frame->storage, frame->of, frame->xid);
	return true;
}

/* txn_spill_from_listed pends, unchains, and lists a spill's files, then settles its slot and merges the transaction's files. */
static bool
txn_spill_from_listed(txn_spill_from_frame_0_t *frame, HostSharedRecord record, const TxnSpillFile *listing,
					  size_t listing_len, TxnRunsTail tail)
{
	txn_TupleU64KindU32OptionU32_t none = {0};

	if (!txn_spill_pend(frame->home, frame->io, record, frame->storage, &listing[0], listing_len > 1, false, 0,
						frame->error) ||
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
	return txn_spill_merged(frame->home, frame->io, record, frame->storage, frame->xid, frame->kind, tail,
							frame->error) &&
		txn_spill_from_ended(frame);
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
	TxnSpillFile listing[2];

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
	if (!txn_runs_Writing_end(frame->writing, frame->io, frame->filter.words.ptr, frame->filter.words.len, &some,
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
		if (!txn_spill_tupled(frame->home, frame->io, record, frame->storage, &frame->dir, frame->xid, number,
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
 * holds only the transaction's versions to a new spill file with its filter,
 * and a row spill's tuple ids to a tuple-id file of its number; it names the
 * files in the process's slot, takes the versions out of the memtable, lists
 * the files in the storage's manifest, clears the slot, and merges the
 * transaction's files. It zeroes the tree's count, and reports whether it
 * spilled a key.
 */
static bool
txn_spill_spilled_from(const TxnSpillHomeOps *home, HostIoIo *io, const HostSharedHeld *held,
					   const HostStorage *storage, TxnWrittenOf of, uint64 xid, bool *result, HostError *error)
{
	txn_spill_from_frame_0_t frame = {.home = home, .io = io, .held = held, .storage = storage, .of = of, .xid = xid,
	.result = result, .error = error};

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
	HostIoIo   *io;
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
	frame->result_0->xid = frame->xid;
	frame->result_0->kind = TXN_RUNS_KIND_SPILLED_TUPLES;
	frame->result_0->number = frame->number;
	frame->result_0->len = run.len;
	frame->result_0->hash = run.hash;
	frame->result_0->bits = txn_spill_Filter_bits(&frame->filter);
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
txn_spill_tupled(const TxnSpillHomeOps *home, HostIoIo *io, HostSharedRecord record, const HostStorage *storage,
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

/* txn_spill_relieved spills, for a writer that found the delta full, its own trees, or else another transaction's, and reports whether one spilled a key. */
bool
txn_spill_relieved(HostIoIo *io, bool *result, HostError *error)
{
	if (!txn_spill_relieved_own(io, result, error))
		return false;
	return *result || txn_spill_helped(io, result, error);
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
	static const TxnWrittenOf state_ofs[] = {TXN_WRITTEN_OF_ROWS, TXN_WRITTEN_OF_VALUES};
	static const TxnWrittenOf chunk_ofs[] = {TXN_WRITTEN_OF_CHUNKS};

	*frame->result = false;
	std_vec_new(&frame->trees, sizeof(txn_TupleStorageOf_t), _Alignof(txn_TupleStorageOf_t));
	frame->trees_live = true;
	if (!txn_spill_swept_pool(frame, &TXN_STATE_STATE_POOL_OPS, TXN_STATE_MEMTABLE_WORD_INDEX,
							  &TXN_SPILL_STATE_HOME_OPS, state_ofs, 2) ||
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
 * memtable, both trees, and every repository's chunk tree, for a helper
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

/* txn_spill_aborted_closure_1 is aborted's step on the home of its pending spill: the slot cleared. */
static bool
txn_spill_aborted_closure_1(void *captures, const TxnSpillHomeOps *home, const HostSharedHeld *held, HostError *error)
{
	HostSharedRecord record = host_shared_Held_record(held);

	return txn_spill_settled(home, captures, &record, error);
}

/* txn_spill_aborted_captures_t is aborted's step's captures on a storage it spilled: the backend's I/O, the storage, and the xid. */
typedef struct txn_spill_aborted_captures_t
{
	HostIoIo   *io;
	const HostStorage *storage;
	uint64		xid;
} txn_spill_aborted_captures_t;

/* txn_spill_aborted_closure_2 is aborted's step on a storage it spilled: the transaction's files taken out of its manifest. */
static bool
txn_spill_aborted_closure_2(void *captures, const TxnSpillHomeOps *home, const HostSharedHeld *held, HostError *error)
{
	txn_spill_aborted_captures_t *c = captures;
	StdVec		gone;

	if (!txn_spill_removed(home, c->io, host_shared_Held_record(held), c->storage, txn_spill_aborted_closure_0,
						   &c->xid, &gone, error))
		return false;
	std_vec_free_Checked(&gone, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	return true;
}

/*
 * txn_spill_aborted takes an aborted transaction's spills out of the
 * manifests of the storages it spilled, and clears the process's slot when
 * it names one of its spills; a storage whose state is gone took its spills
 * with it.
 */
bool
txn_spill_aborted(HostIoIo *io, uint64 xid, const HostStorage *storages, size_t storages_len, HostError *error)
{
	bool		some;
	TxnSpillPended pended;

	if (!txn_spill_pending(io, &some, &pended, error))
		return false;
	if (some && pended.xid == xid)
	{
		if (!txn_spill_at_found(io, &pended.storage, txn_spill_aborted_closure_1, io, &some, error))
			return false;
		if (!some && !txn_spill_settled(&TXN_SPILL_STATE_HOME_OPS, io, NULL, error))
			return false;
	}
	for (size_t k = 0; k < storages_len; k++)
	{
		txn_spill_aborted_captures_t captures = {io, &storages[k], xid};

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
	frame->records.len = 0;
	for (size_t k = 0; k < frame->versions.len; k++)
	{
		const StdVec *version = &STD_VEC_AT(&frame->versions, StdVec, k);
		StdBytes	record = {version->ptr, version->len};

		std_vec_push_Checked(&frame->records, &record, sizeof(record), _Alignof(StdBytes));
	}
	if (!txn_runs_Writing_key(frame->writing, frame->io, frame->least.ptr, frame->least.len, frame->records.ptr,
							  frame->records.len, frame->error))
		return false;
	txn_spill_Filter_add(&frame->filter, frame->least.ptr, frame->least.len);
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
	if (!txn_runs_Writing_end(frame->writing, frame->io, frame->filter.words.ptr, frame->filter.words.len, &some, &run,
							  frame->error))
	{
		txn_spill_joined_pad_0(frame);
		return;
	}
	if (!some)
	{
		*frame->error = txn_spill_internal("a merge of spills wrote no key");
		txn_spill_joined_pad_0(frame);
		return;
	}
	frame->result->xid = frame->files_len > 0 ? frame->files[0].xid : 0;
	frame->result->kind = frame->names->kind;
	frame->result->number = frame->number;
	frame->result->len = run.len;
	frame->result->hash = run.hash;
	frame->result->bits = txn_spill_Filter_bits(&frame->filter);
	frame->ok = true;
	txn_spill_joined_pad_0(frame);
}

/*
 * txn_spill_joined streams spill files of one kind into one new file of a
 * number with their header's tail, in key order, a key's versions the newer
 * file's first, a tuple id's key the newest file's alone, with a filter of
 * its keys, synced, and returns it.
 */
static bool
txn_spill_joined(HostIoIo *io, const StdPathBuf *dir, const TxnRunsNames *names, TxnRunsTail tail,
				 const TxnSpillFile *files, size_t files_len, uint32 number, TxnSpillFile *result, HostError *error)
{
	txn_spill_joined_frame_0_t frame = {.io = io, .dir = dir, .names = names, .tail = tail, .files = files,
	.files_len = files_len, .number = number, .result = result, .error = error};

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
	TxnSpillFile listing[2];
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
	if (!txn_spill_joined(frame->io, &frame->dir, names, frame->tail, frame->own.ptr, taken, number, &listing[0],
						  frame->error))
		return false;
	txn_spill_merged_mine(frame, TXN_RUNS_KIND_SPILLED_TUPLES, &frame->named);
	frame->named_live = true;
	{
		size_t		kept = 0;

		for (size_t j = 0; j < frame->named.len; j++)
		{
			const TxnSpillFile *tuples = &STD_VEC_AT(&frame->named, TxnSpillFile, j);
			bool		merged = false;

			for (size_t k = 0; k < taken && !merged; k++)
				merged = STD_VEC_AT(&frame->own, TxnSpillFile, k).number == tuples->number;
			if (merged)
				STD_VEC_AT(&frame->named, TxnSpillFile, kept++) = *tuples;
		}
		frame->named.len = kept;
	}
	if (frame->named.len > 0)
	{
		if (!txn_spill_joined(frame->io, &frame->dir, tuple_names, none, frame->named.ptr, frame->named.len, number,
							  &listing[1], frame->error))
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
 * file, and the tuple-id files of the same numbers into one of the merged
 * file's number, pends the merged file in the process's slot with the lowest
 * number it replaces, lists it in place of the files it joined, and clears
 * the slot, until the newest file has no earlier file to take.
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

/* txn_spill_folded_frame_0_t is folded's state across its pad: the files listed, the xids applied, the counts, a file's path and bytes and decoding, and the files removed, while live. */
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
	StdPathBuf	path;
	bool		path_live;
	StdString	name;
	bool		name_live;
	StdVec		bytes;
	bool		bytes_live;
	TxnRunsDecoded decoded;
	bool		decoded_live;
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

/* txn_spill_folded_counted reads a listed spill file back and counts its versions at its transaction's entry, into the frame's counts. */
static bool
txn_spill_folded_counted(txn_spill_folded_frame_0_t *frame, const TxnSpillFile *file)
{
	TxnRunsNames names;
	ChunksErrorError failed;
	txn_TupleFileU64_t count = {*file};

	if (!txn_spill_names(&TXN_SPILL_STATE_HOME_OPS, frame->state, &frame->storage, file->kind, file->xid, &names,
						 frame->error))
		return false;
	txn_runs_spill_directory(frame->storage.database, txn_state_own(frame->state), &frame->path);
	frame->path_live = true;
	txn_runs_spill_name(&names, file->number, &frame->name);
	frame->name_live = true;
	{
		StdPathBuf	joined;

		std_path_join(frame->path.bytes.ptr, frame->path.bytes.len, (const char *) frame->name.ptr, frame->name.len,
					  &joined);
		std_vec_free_Checked(&frame->path.bytes, 1, 1);
		frame->path = joined;
	}
	frame->name_live = false;
	std_vec_free_Checked(&frame->name, 1, 1);
	if (!chunks_file_read_file(HOST_IO_IO_READ_DOORS(frame->io), frame->path.bytes.ptr, frame->path.bytes.len,
							   &frame->bytes, &failed))
	{
		*frame->error = txn_base_storage(&failed);
		return false;
	}
	frame->bytes_live = true;
	if (!std_vec_try_reserve(&frame->counts, 1, sizeof(count), _Alignof(txn_TupleFileU64_t)))
	{
		*frame->error = host_Error_out_of_memory();
		return false;
	}
	if (!txn_runs_decode(frame->bytes.ptr, frame->bytes.len, &names, file->hash, &frame->decoded, frame->error))
		return false;
	frame->decoded_live = true;
	count.f1 = txn_spill_counted(&frame->decoded);
	frame->decoded_live = false;
	txn_runs_Decoded_drop(&frame->decoded);
	frame->bytes_live = false;
	std_vec_free_Checked(&frame->bytes, 1, 1);
	frame->path_live = false;
	std_vec_free_Checked(&frame->path.bytes, 1, 1);
	std_vec_push_Checked(&frame->counts, &count, sizeof(count), _Alignof(txn_TupleFileU64_t));
	return true;
}

/* txn_spill_folded_gather gathers the xids whose commits the fold applies and the counts of their files. */
static bool
txn_spill_folded_gather(txn_spill_folded_frame_0_t *frame)
{
	for (size_t k = 0; k < frame->listed.f0.len; k++)
	{
		const TxnSpillFile *file = &STD_VEC_AT(&frame->listed.f0, TxnSpillFile, k);
		bool		known = txn_spill_has_xid(&frame->applied, file->xid);

		if (!known)
		{
			HostXid		xid = {file->xid};
			TxnStatusCommit commit;

			if (!txn_status_committed(frame->io, xid, &commit, frame->error))
				return false;
			if (commit.tag != TXN_STATUS_COMMIT_AT || commit.u.at > frame->point)
				continue;
			if (!std_vec_try_reserve(&frame->applied, 1, sizeof(uint64), _Alignof(uint64)))
			{
				*frame->error = host_Error_out_of_memory();
				return false;
			}
			std_vec_push_Checked(&frame->applied, &file->xid, sizeof(uint64), _Alignof(uint64));
		}
		if ((file->kind == TXN_RUNS_KIND_SPILLED_ROWS || file->kind == TXN_RUNS_KIND_SPILLED_ENTRIES ||
			 file->kind == TXN_RUNS_KIND_SPILLED_VALUES) && !txn_spill_folded_counted(frame, file))
			return false;
	}
	return true;
}

/* txn_spill_folded_body_0 is folded's body. */
static void
txn_spill_folded_body_0(void *frame_)
{
	txn_spill_folded_frame_0_t *frame = frame_;
	TxnStatePersistence persistence;

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
	if (frame->applied.len == 0)
	{
		frame->ok = true;
		txn_spill_folded_pad_0(frame);
		return;
	}
	if (!host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-spill-folded-counted", frame->error) ||
		!txn_spill_removed(&TXN_SPILL_STATE_HOME_OPS, frame->io, frame->state, &frame->storage,
						   txn_spill_xids_closure_0, &frame->applied, &frame->gone, frame->error))
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
 * each transaction's entry what its spilled rows, entries, and value map
 * versions added, reading each file's count before it leaves the manifest;
 * only the counts of the files its own removal took out go back.
 */
bool
txn_spill_folded(HostIoIo *io, HostSharedRecord state, HostLsn point, HostError *error)
{
	txn_spill_folded_frame_0_t frame = {.io = io, .state = state, .point = point, .error = error};

	std_landing(&frame, txn_spill_folded_body_0, txn_spill_folded_pad_0);
	return frame.ok;
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
} txn_spill_unfolded_frame_0_t;

/* txn_spill_unfolded_pad_0 is unfolded_since's pad: what it holds, while live. */
static void
txn_spill_unfolded_pad_0(void *frame_)
{
	txn_spill_unfolded_frame_0_t *frame = frame_;

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
	if (some && frame->base.folded >= frame->end)
	{
		frame->ok = true;
		txn_spill_unfolded_pad_0(frame);
		return;
	}
	if (host_shared_Record_atomic(record, TXN_STATE_SPILLS_INDEX, load) != 0 ||
		host_shared_Record_atomic(record, TXN_STATE_SPILLS_PENDING_INDEX, load) != 0 ||
		txn_memtable_table_memtable(&TXN_MEMTABLE_MAP_VALUES_TREE_OPS, record, &word))
	{
		*frame->result = true;
		frame->ok = true;
		txn_spill_unfolded_pad_0(frame);
		return;
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
 * folded below a commit's end while the state holds a row, entry, or value
 * map version that no fold applied, in its memtables or its spills.
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
} txn_spill_files_frame_0_t;

/* txn_spill_files_pad_0 is files' pad: what it holds, while live. */
static void
txn_spill_files_pad_0(void *frame_)
{
	txn_spill_files_frame_0_t *frame = frame_;

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

		for (size_t j = 0; j < frame->result->len && !seen; j++)
		{
			const TxnSpillReading *found = &STD_VEC_AT(frame->result, TxnSpillReading, j);

			seen = found->xid == file->xid && found->number == file->number;
		}
		if (!txn_spill_tree(file->kind, frame->kind) || seen ||
			txn_spill_replaced_by(frame->slotted.ptr, frame->slotted.len, file))
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
 * filters it hits.
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

/* TxnSpillNaming is Naming: a spill file a checkpoint names, its xid, kind, and number, and its length and xxh3 when its manifest lists them. */
typedef struct TxnSpillNaming
{
	uint64		xid;
	TxnRunsKind kind;
	uint32		number;
	bool		has_read;
	uint64		len;
	uint64		hash;
} TxnSpillNaming;

/* txn_spill_named_frame_0_t is named's state across its pad: the slots, the listed files, the namings, the aborted xids, the spills' directory, a file's path and file, and the named run files, while live. */
typedef struct txn_spill_named_frame_0_t
{
	const TxnSpillHomeOps *home;
	HostIoIo   *io;
	HostSharedRecord state;
	const HostStorage *storage;
	StdVec	   *result;
	HostError  *error;
	bool		ok;
	bool		result_live;
	StdVec		slotted;
	bool		slotted_live;
	txn_TupleVecFileU64_t listed;
	bool		listed_live;
	/* files is a Vec<Naming>. */
	StdVec		files;
	bool		files_live;
	/* aborted is a Vec<u64> of xids. */
	StdVec		aborted;
	bool		aborted_live;
	StdVec		gone;
	bool		gone_live;
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
	if (frame->gone_live)
	{
		frame->gone_live = false;
		std_vec_free_Checked(&frame->gone, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
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

/* txn_spill_named_listed gathers the namings of the listed files, oldest-linked first, passing over those a pending merge replaces and gathering the aborted. */
static bool
txn_spill_named_listed(txn_spill_named_frame_0_t *frame)
{
	for (size_t k = frame->listed.f0.len; k > 0; k--)
	{
		const TxnSpillFile *file = &STD_VEC_AT(&frame->listed.f0, TxnSpillFile, k - 1);
		HostXid		xid = {file->xid};
		HostStatus	status;
		TxnSpillNaming naming = {file->xid, file->kind, file->number, true, file->len, file->hash};

		if (txn_spill_replaced_by(frame->slotted.ptr, frame->slotted.len, file))
			continue;
		if (!host_status(HOST_IO_IO_IO_DOORS(frame->io), xid, &status, frame->error))
			return false;
		if (status == HOST_STATUS_ABORTED)
		{
			if (!txn_spill_named_push(frame, &frame->aborted, &file->xid, sizeof(uint64), _Alignof(uint64)))
				return false;
			continue;
		}
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
	if (!txn_spill_consistent(frame->home, frame->io, frame->state, frame->storage, &frame->slotted,
							  &frame->slotted_live, &frame->listed, &frame->listed_live, frame->error))
	{
		txn_spill_named_pad_0(frame);
		return;
	}
	if (!txn_spill_named_listed(frame))
	{
		txn_spill_named_pad_0(frame);
		return;
	}
	if (frame->aborted.len > 0)
	{
		if (!txn_spill_removed(frame->home, frame->io, frame->state, frame->storage, txn_spill_xids_closure_0,
							   &frame->aborted, &frame->gone, frame->error))
		{
			txn_spill_named_pad_0(frame);
			return;
		}
		frame->gone_live = true;
	}
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
		.spilled = {naming->xid, naming->number}};

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
 * first, then those processes' slots name. It reads the slots and the
 * manifest as a reader does, so a merge pending its file names that file and
 * not the sources it replaces. A listed spill of a transaction the commit
 * log calls aborted leaves the manifest unnamed.
 */
bool
txn_spill_named(const TxnSpillHomeOps *home, HostIoIo *io, HostSharedRecord state, const HostStorage *storage,
				StdVec *result, HostError *error)
{
	txn_spill_named_frame_0_t frame = {.home = home, .io = io, .state = state, .storage = storage, .result = result,
	.error = error};

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
	result->xid = frame->pended->xid;
	result->kind = names->kind;
	result->number = frame->pended->number;
	result->len = (uint64) bytes->len;
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
	TxnSpillFile listing[2];
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
