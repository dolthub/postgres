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
 * The C port of txn's runs.rs: the checkpoint's runs, a written tree's whole
 * delta at a checkpoint's redo point, its keys in byte order and each key's
 * versions newest first, in a file of framed records that the manifest's
 * field 10 names, and its decoder, which refuses a damaged or foreign file as
 * data corruption (XX001); and the spills' files, which add an index frame
 * and a filter.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/file.h"
#include "doltgres/chunks/packs.h"
#include "doltgres/chunks/packs/open.h"
#include "doltgres/chunks/varint.h"
#include "doltgres/host/cache.h"
#include "doltgres/host/delta.h"
#include "doltgres/host/delta/page.h"
#include "doltgres/host/local.h"
#include "doltgres/host/lock.h"
#include "doltgres/host/shared.h"
#include "doltgres/repo/lib.h"
#include "doltgres/std/alloc.h"
#include "doltgres/std/cmp.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/num.h"
#include "doltgres/std/path.h"
#include "doltgres/std/slice.h"
#include "doltgres/std/sort.h"
#include "doltgres/std/str.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"
#include "doltgres/trees/store.h"
#include "doltgres/txn/base.h"
#include "doltgres/txn/fold.h"
#include "doltgres/txn/identity.h"
#include "doltgres/txn/insert.h"
#include "doltgres/txn/memtable/branch.h"
#include "doltgres/txn/memtable/map.h"
#include "doltgres/txn/memtable/table.h"
#include "doltgres/txn/memtable/tuples.h"
#include "doltgres/txn/memtable/version.h"
#include "doltgres/txn/outcome.h"
#include "doltgres/txn/runs.h"
#include "doltgres/txn/spill.h"
#include "doltgres/txn/state.h"
#include "doltgres/txn/status.h"

#define XXH_STATIC_LINKING_ONLY
#include "xxhash.h"

/* The prepare whose closure returns nothing, which the loader's rows take. */
#define TXN_MEMTABLE_TABLE_RS Unit
#define TXN_MEMTABLE_TABLE_RETURNS bool
#define TXN_MEMTABLE_TABLE_RESULT_PARAMS , HostError *error
#define TXN_MEMTABLE_TABLE_DECLARE
#include "doltgres/txn/memtable/table_template.h"

#define RUST_FILE "txn" STD_SEP "src" STD_SEP "runs.rs"

/* TXN_RUNS_MAGIC is MAGIC, which starts a run's file. */
static const uint8 TXN_RUNS_MAGIC[8] = {0x72, 0x75, 0x6e, 0x73, 0x9e, 0x31, 0xc4, 0x07};

/* TXN_RUNS_HEADER is HEADER, a run's header record: its kind, its persistence, tablespace, database, relation, and redo point. */
#define TXN_RUNS_HEADER (1 + 1 + 4 + 4 + 4 + 8)

/* TXN_RUNS_TAIL is TAIL, what a free space run's and an entries run's headers hold past the others'. */
#define TXN_RUNS_TAIL 8

/* TXN_RUNS_TRAILER is TRAILER, a run's trailer record: its count of versions again, and the xxh3 of every frame before. */
#define TXN_RUNS_TRAILER (8 + 8)

/* TXN_RUNS_INDEXED is INDEXED, what a spill's trailer holds past a run's: its index frame's place and its filter's. */
#define TXN_RUNS_INDEXED (8 + 8)

/* TXN_RUNS_TRAILED is TRAILED, a spill's trailer frame: its length, its record, and its xxh3. */
#define TXN_RUNS_TRAILED (1 + TXN_RUNS_TRAILER + TXN_RUNS_INDEXED + 8)

/* TXN_RUNS_STRIDE is STRIDE, the most bytes of key frames between two of a spill's index entries. */
#define TXN_RUNS_STRIDE (UINT64CONST(64) << 10)

/* txn_runs_corrupt returns the error of a run's file that is damaged or isn't the one its manifest names. */
static HostError
txn_runs_corrupt(void)
{
	StdString	message;
	const char *text = "a checkpoint's run of the delta is corrupt";

	std_string_from_Checked(&message, text, strlen(text));
	return host_Error_fork((const uint8 *) "XX001", &message);
}

/* txn_runs_Kind_of returns the kind a file's number names, answering false for a number no kind has. */
bool
txn_runs_Kind_of(uint8 number, TxnRunsKind *result)
{
	static const TxnRunsKind kinds[] = {
		TXN_RUNS_KIND_ROWS, TXN_RUNS_KIND_FREE, TXN_RUNS_KIND_VALUES, TXN_RUNS_KIND_CHUNKS, TXN_RUNS_KIND_STATUS,
		TXN_RUNS_KIND_SPILLED_ROWS, TXN_RUNS_KIND_SPILLED_VALUES, TXN_RUNS_KIND_SPILLED_TUPLES, TXN_RUNS_KIND_ENTRIES,
		TXN_RUNS_KIND_SPILLED_ENTRIES, TXN_RUNS_KIND_IDENTITY, TXN_RUNS_KIND_SPILLED_CHUNKS,
	};

	for (size_t k = 0; k < lengthof(kinds); k++)
		if ((uint8) kinds[k] == number)
		{
			*result = kinds[k];
			return true;
		}
	return false;
}

/* txn_runs_Kind_spilled reports whether a kind is a transaction's spill, whose header holds its xid and whose file holds an index frame. */
bool
txn_runs_Kind_spilled(TxnRunsKind self)
{
	return self == TXN_RUNS_KIND_SPILLED_ROWS || self == TXN_RUNS_KIND_SPILLED_VALUES ||
		self == TXN_RUNS_KIND_SPILLED_TUPLES || self == TXN_RUNS_KIND_SPILLED_ENTRIES ||
		self == TXN_RUNS_KIND_SPILLED_CHUNKS;
}

/* txn_runs_Names_PartialEq_eq is Names' derived PartialEq. */
bool
txn_runs_Names_PartialEq_eq(const TxnRunsNames *self, const TxnRunsNames *other)
{
	return self->kind == other->kind && self->persistence == other->persistence &&
		self->tablespace == other->tablespace && self->database == other->database &&
		self->relation == other->relation && self->redo == other->redo;
}

/* txn_runs_framed_open starts a frame of a length in out: its length's varint, answering where the frame starts. */
static size_t
txn_runs_framed_open(StdVec *out, size_t len)
{
	size_t		start = out->len;

	chunks_varint_put_varint(out, (uint64) len);
	return start;
}

/* txn_runs_framed_close ends a frame started at a place of out: the xxh3 of its length and bytes. */
static void
txn_runs_framed_close(StdVec *out, size_t start)
{
	uint8		sum[8];

	std_u64_to_le_bytes(XXH3_64bits((const uint8 *) out->ptr + start, out->len - start), sum);
	std_vec_extend_from_slice_Checked(out, sum, 8, 1, 1);
}

/* txn_runs_framed appends a frame of bytes: their length, the bytes, and an xxh3 of both. */
static void
txn_runs_framed(StdVec *out, const uint8 *bytes, size_t len)
{
	size_t		start = txn_runs_framed_open(out, len);

	std_vec_extend_from_slice_Checked(out, bytes, len, 1, 1);
	txn_runs_framed_close(out, start);
}

/*
 * TxnRunsEncoder is Encoder: it writes a run's file in order, handing out
 * each part's bytes to append, and keeps the xxh3 of every frame so far, the
 * count of versions, where the next bytes go, and, for a spill, the index's
 * entries so far and the key written last.
 */
typedef struct TxnRunsEncoder
{
	XXH3_state_t hasher;
	uint64		count;
	uint64		at;
	bool		has_index;
	/* index is a Vec<(Vec<u8>, u64)>. */
	StdVec		index;
	/* previous is a Vec<u8>. */
	StdVec		previous;
} TxnRunsEncoder;

/* txn_runs_Index_drop drops a Vec<(Vec<u8>, u64)> of a spill's index: each key, then the buffer. */
void
txn_runs_Index_drop(StdVec *index)
{
	for (size_t k = 0; k < index->len; k++)
		std_vec_free_Checked(&STD_VEC_AT(index, txn_TupleVecU8U64_t, k).f0, 1, 1);
	std_vec_free_Checked(index, sizeof(txn_TupleVecU8U64_t), _Alignof(txn_TupleVecU8U64_t));
}

/* txn_runs_Encoder_drop drops an encoder's index and its key written last. */
static void
txn_runs_Encoder_drop(TxnRunsEncoder *self)
{
	txn_runs_Index_drop(&self->index);
	std_vec_free_Checked(&self->previous, 1, 1);
}

/*
 * txn_runs_Encoder_begin starts a file of a run that names what it holds,
 * with its header's tail, into an encoder, writing the bytes of its magic
 * and its header into an empty Vec.
 */
static void
txn_runs_Encoder_begin(const TxnRunsNames *names, TxnRunsTail tail, TxnRunsEncoder *self, StdVec *out)
{
	uint8		header[TXN_RUNS_HEADER + TXN_RUNS_TAIL];
	size_t		len = TXN_RUNS_HEADER;

	header[0] = (uint8) names->kind;
	header[1] = (uint8) names->persistence;
	std_u32_to_le_bytes(names->tablespace, header + 2);
	std_u32_to_le_bytes(names->database, header + 6);
	std_u32_to_le_bytes(names->relation, header + 10);
	std_u64_to_le_bytes(names->redo, header + 14);
	if (tail.tag == TXN_RUNS_TAIL_COUNTER)
	{
		std_u64_to_le_bytes(tail.counter, header + len);
		len += 8;
	}
	else if (tail.tag == TXN_RUNS_TAIL_INDEX)
	{
		std_u32_to_le_bytes(tail.table, header + len);
		std_u32_to_le_bytes(tail.index, header + len + 4);
		len += 8;
	}
	std_vec_extend_from_slice_Checked(out, TXN_RUNS_MAGIC, sizeof(TXN_RUNS_MAGIC), 1, 1);
	txn_runs_framed(out, header, len);
	XXH3_64bits_reset(&self->hasher);
	XXH3_64bits_update(&self->hasher, (const uint8 *) out->ptr + sizeof(TXN_RUNS_MAGIC),
					   out->len - sizeof(TXN_RUNS_MAGIC));
	self->count = 0;
	self->at = (uint64) out->len;
	self->has_index = txn_runs_Kind_spilled(names->kind);
	std_vec_new(&self->index, sizeof(txn_TupleVecU8U64_t), _Alignof(txn_TupleVecU8U64_t));
	std_vec_new(&self->previous, 1, 1);
}

/* txn_runs_put_part appends a part of a record: its length's varint and its bytes. */
static void
txn_runs_put_part(StdVec *out, const uint8 *bytes, size_t len)
{
	chunks_varint_put_varint(out, (uint64) len);
	std_vec_extend_from_slice_Checked(out, bytes, len, 1, 1);
}

/*
 * txn_runs_Encoder_key writes into an empty Vec the bytes of one key's
 * record: the key, then its versions' records, newest first, framed, and
 * counts it in the index of a spill.
 */
static void
txn_runs_Encoder_key(TxnRunsEncoder *self, const uint8 *key, size_t key_len, const StdBytes *versions,
					 size_t versions_len, StdVec *out)
{
	size_t		len = chunks_varint_varint_len((uint64) key_len) + key_len +
		chunks_varint_varint_len((uint64) versions_len);
	size_t		start;

	for (size_t k = 0; k < versions_len; k++)
		len += chunks_varint_varint_len((uint64) versions[k].len) + versions[k].len;
	start = txn_runs_framed_open(out, len);
	txn_runs_put_part(out, key, key_len);
	chunks_varint_put_varint(out, (uint64) versions_len);
	self->count = STD_ADD(u64, self->count, (uint64) versions_len, 209, 9);
	for (size_t k = 0; k < versions_len; k++)
		txn_runs_put_part(out, versions[k].ptr, versions[k].len);
	txn_runs_framed_close(out, start);
	XXH3_64bits_update(&self->hasher, out->ptr, out->len);
	if (self->has_index)
	{
		bool		due = self->index.len == 0 ||
			STD_SUB(u64, self->at, STD_VEC_AT(&self->index, txn_TupleVecU8U64_t, self->index.len - 1).f1,
					218, 59) >= TXN_RUNS_STRIDE;

		if (due && (self->index.len == 0 || self->previous.len != key_len ||
					memcmp(self->previous.ptr, key, key_len) != 0))
		{
			txn_TupleVecU8U64_t entry = {.f1 = self->at};

			std_vec_new(&entry.f0, 1, 1);
			std_vec_push_Checked(&self->index, &entry, sizeof(entry), _Alignof(txn_TupleVecU8U64_t));
			std_vec_extend_from_slice_Checked(&STD_VEC_AT(&self->index, txn_TupleVecU8U64_t, self->index.len - 1).f0,
											  key, key_len, 1, 1);
		}
		self->previous.len = 0;
		std_vec_extend_from_slice_Checked(&self->previous, key, key_len, 1, 1);
	}
	self->at = STD_ADD(u64, self->at, (uint64) out->len, 225, 9);
}

/*
 * txn_runs_Encoder_end writes into an empty Vec the bytes of a spill's index
 * frame, of its filter's pages, a frame each of at most FILTER_WORDS words of
 * the filter a spill passes, and of the trailer: the count of versions, the
 * xxh3 of every frame before it, which it answers, and a spill's index
 * frame's place and its filter's.
 */
static uint64
txn_runs_Encoder_end(TxnRunsEncoder *self, const uint64 *filter, size_t filter_len, StdVec *out)
{
	uint8		trailer[TXN_RUNS_TRAILER + TXN_RUNS_INDEXED];
	size_t		trailer_len = TXN_RUNS_TRAILER;
	uint64		sum;

	if (self->has_index)
	{
		size_t		len = chunks_varint_varint_len((uint64) self->index.len);
		size_t		start;
		uint64		filter_at;

		for (size_t k = 0; k < self->index.len; k++)
		{
			const txn_TupleVecU8U64_t *entry = &STD_VEC_AT(&self->index, txn_TupleVecU8U64_t, k);

			len += chunks_varint_varint_len((uint64) entry->f0.len) + entry->f0.len +
				chunks_varint_varint_len(entry->f1);
		}
		start = txn_runs_framed_open(out, len);
		chunks_varint_put_varint(out, (uint64) self->index.len);
		for (size_t k = 0; k < self->index.len; k++)
		{
			const txn_TupleVecU8U64_t *entry = &STD_VEC_AT(&self->index, txn_TupleVecU8U64_t, k);

			txn_runs_put_part(out, entry->f0.ptr, entry->f0.len);
			chunks_varint_put_varint(out, entry->f1);
		}
		txn_runs_framed_close(out, start);
		filter_at = STD_ADD(u64, self->at, (uint64) out->len, 243, 29);
		for (size_t from = 0; from < filter_len; from += TXN_RUNS_FILTER_WORDS)
		{
			size_t		words = Min(filter_len - from, TXN_RUNS_FILTER_WORDS);
			size_t		page = txn_runs_framed_open(out, words * 8);

			for (size_t k = 0; k < words; k++)
			{
				uint8		word[8];

				std_u64_to_le_bytes(filter[from + k], word);
				std_vec_extend_from_slice_Checked(out, word, 8, 1, 1);
			}
			txn_runs_framed_close(out, page);
		}
		XXH3_64bits_update(&self->hasher, out->ptr, out->len);
		std_u64_to_le_bytes(self->at, trailer + TXN_RUNS_TRAILER);
		std_u64_to_le_bytes(filter_at, trailer + TXN_RUNS_TRAILER + 8);
		trailer_len += TXN_RUNS_INDEXED;
	}
	sum = XXH3_64bits_digest(&self->hasher);
	std_u64_to_le_bytes(self->count, trailer);
	std_u64_to_le_bytes(sum, trailer + 8);
	txn_runs_framed(out, trailer, trailer_len);
	return sum;
}

/* txn_runs_storage returns a file's error as the storage's error. */
static HostError
txn_runs_storage(ChunksErrorError *error)
{
	return txn_base_storage(error);
}

/*
 * txn_runs_frame reads the frame at a place of a file's bytes, checking its
 * xxh3, and answers its bytes and where the next starts.
 */
static bool
txn_runs_frame(const uint8 *bytes, size_t bytes_len, size_t at, StdBytes *result, size_t *next, HostError *error)
{
	size_t		place = at;
	uint64		read;
	ChunksErrorError failed;
	size_t		len;
	size_t		end;

	if (!chunks_varint_read_varint(bytes, bytes_len, &place, &read, &failed))
	{
		*error = txn_runs_storage(&failed);
		return false;
	}
	if (read > SIZE_MAX)
	{
		*error = txn_runs_corrupt();
		return false;
	}
	len = (size_t) read;
	if (len > SIZE_MAX - place || STD_ADD(usize, place + len, 8, 272, 51) > bytes_len)
	{
		*error = txn_runs_corrupt();
		return false;
	}
	end = place + len;
	if (XXH3_64bits(bytes + at, end - at) != std_u64_from_le_bytes(bytes + end))
	{
		*error = txn_runs_corrupt();
		return false;
	}
	result->ptr = bytes + place;
	result->len = len;
	*next = STD_ADD(usize, end, 8, 275, 41);
	return true;
}

/* txn_runs_varint reads a varint at a place of a record, refusing one cut short. */
static bool
txn_runs_varint(StdBytes record, size_t *at, size_t *result, HostError *error)
{
	uint64		read;
	ChunksErrorError failed;

	if (!chunks_varint_read_varint(record.ptr, record.len, at, &read, &failed))
	{
		*error = txn_runs_storage(&failed);
		return false;
	}
	if (read > SIZE_MAX)
	{
		*error = txn_runs_corrupt();
		return false;
	}
	*result = (size_t) read;
	return true;
}

/* txn_runs_part reads a part of a record of a length at a place, refusing one cut short. */
static bool
txn_runs_part(StdBytes record, size_t *at, size_t len, StdBytes *result, HostError *error)
{
	if (len > SIZE_MAX - *at || *at + len > record.len)
	{
		*error = txn_runs_corrupt();
		return false;
	}
	result->ptr = record.ptr + *at;
	result->len = len;
	*at = STD_ADD(usize, *at, len, 288, 5);
	return true;
}

/*
 * TxnRunsOpened is Opened: a run's file past its header, its body after the
 * magic, where its first record's frame begins, its storage's persistence,
 * and its header's tail.
 */
typedef struct TxnRunsOpened
{
	StdBytes	body;
	size_t		at;
	TxnStatePersistence persistence;
	TxnRunsTail tail;
} TxnRunsOpened;

/* txn_runs_body returns a run's file's body past its magic, answering false for a file that doesn't start with it. */
static bool
txn_runs_body(const uint8 *bytes, size_t bytes_len, StdBytes *result)
{
	if (bytes_len < sizeof(TXN_RUNS_MAGIC) || memcmp(bytes, TXN_RUNS_MAGIC, sizeof(TXN_RUNS_MAGIC)) != 0)
		return false;
	result->ptr = bytes + sizeof(TXN_RUNS_MAGIC);
	result->len = bytes_len - sizeof(TXN_RUNS_MAGIC);
	return true;
}

/*
 * txn_runs_opened reads a run's magic and header, refusing as data
 * corruption another magic, a header frame whose xxh3 fails, a header of
 * another length than its kind's, a free space run's or an entries run's
 * with its tail and any other's without, of a kind or a persistence none
 * has, or that names another run than names, whatever its persistence and
 * its tail, which it answers.
 */
static bool
txn_runs_opened(const uint8 *bytes, size_t bytes_len, const TxnRunsNames *names, TxnRunsOpened *result,
				HostError *error)
{
	StdBytes	header;
	const uint8 *tail;
	size_t		tail_len;
	TxnRunsNames named;

	if (!txn_runs_body(bytes, bytes_len, &result->body))
	{
		*error = txn_runs_corrupt();
		return false;
	}
	if (!txn_runs_frame(result->body.ptr, result->body.len, 0, &header, &result->at, error))
		return false;
	if (header.len < TXN_RUNS_HEADER)
	{
		*error = txn_runs_corrupt();
		return false;
	}
	tail = header.ptr + TXN_RUNS_HEADER;
	tail_len = header.len - TXN_RUNS_HEADER;
	memset(&result->tail, 0, sizeof(result->tail));
	if (names->kind == TXN_RUNS_KIND_FREE && tail_len == TXN_RUNS_TAIL)
	{
		result->tail.tag = TXN_RUNS_TAIL_COUNTER;
		result->tail.counter = std_u64_from_le_bytes(tail);
	}
	else if ((names->kind == TXN_RUNS_KIND_ENTRIES || names->kind == TXN_RUNS_KIND_SPILLED_ENTRIES) &&
			 tail_len == TXN_RUNS_TAIL)
	{
		result->tail.tag = TXN_RUNS_TAIL_INDEX;
		result->tail.table = std_u32_from_le_bytes(tail);
		result->tail.index = std_u32_from_le_bytes(tail + 4);
	}
	else if (names->kind != TXN_RUNS_KIND_FREE && names->kind != TXN_RUNS_KIND_ENTRIES &&
			 names->kind != TXN_RUNS_KIND_SPILLED_ENTRIES && tail_len != TXN_RUNS_TAIL && tail_len == 0)
		result->tail.tag = TXN_RUNS_TAIL_NONE;
	else
	{
		*error = txn_runs_corrupt();
		return false;
	}
	if (header.ptr[1] == 0)
		result->persistence = TXN_STATE_PERSISTENCE_PERMANENT;
	else if (header.ptr[1] == 1)
		result->persistence = TXN_STATE_PERSISTENCE_UNLOGGED;
	else
	{
		*error = txn_runs_corrupt();
		return false;
	}
	if (!txn_runs_Kind_of(header.ptr[0], &named.kind))
	{
		*error = txn_runs_corrupt();
		return false;
	}
	named.persistence = result->persistence;
	named.tablespace = std_u32_from_le_bytes(header.ptr + 2);
	named.database = std_u32_from_le_bytes(header.ptr + 6);
	named.relation = std_u32_from_le_bytes(header.ptr + 10);
	named.redo = std_u64_from_le_bytes(header.ptr + 14);
	{
		TxnRunsNames asked = *names;

		asked.persistence = result->persistence;
		if (!txn_runs_Names_PartialEq_eq(&named, &asked))
		{
			*error = txn_runs_corrupt();
			return false;
		}
	}
	return true;
}

/*
 * txn_runs_trailed checks a run's trailer, refusing as data corruption one
 * whose count isn't the records' read or whose xxh3 isn't the frames'
 * before it or hash, the manifest entry's.
 */
static bool
txn_runs_trailed(StdBytes record, XXH3_state_t * hasher, uint64 read, uint64 hash, HostError *error)
{
	uint64		sum;

	if (record.len != TXN_RUNS_TRAILER)
	{
		*error = txn_runs_corrupt();
		return false;
	}
	sum = std_u64_from_le_bytes(record.ptr + 8);
	if (std_u64_from_le_bytes(record.ptr) != read || XXH3_64bits_digest(hasher) != sum || hash != sum)
	{
		*error = txn_runs_corrupt();
		return false;
	}
	return true;
}

/* txn_runs_top returns the top-level xid of a version's writer: its top, else its own xid. */
static uint64
txn_runs_top(const TxnMemtableVersionRecord *version)
{
	TxnMemtableVersionWriter writer = txn_memtable_version_Record_writer(version);

	return writer.has_top ? writer.top : writer.xid;
}

/* txn_runs_put_of returns the put of an op that puts a row, a put, an add, an added row, or an update, answering false for any other. */
static bool
txn_runs_put_of(const TxnMemtableVersionOpRef *op, TxnMemtableVersionPutRef *result)
{
	switch (op->tag)
	{
		case TXN_MEMTABLE_VERSION_OP_REF_PUT:
			*result = op->u.put;
			return true;
		case TXN_MEMTABLE_VERSION_OP_REF_ADD:
			*result = op->u.add;
			return true;
		case TXN_MEMTABLE_VERSION_OP_REF_ADD_ROW:
			*result = op->u.add_row;
			return true;
		case TXN_MEMTABLE_VERSION_OP_REF_UPDATE_ROW:
			*result = op->u.update_row.f1;
			return true;
		default:
			return false;
	}
}

/* txn_runs_refused reports whether a run of a kind refuses a version decode read. */
static bool
txn_runs_refused(const TxnRunsNames *names, const TxnMemtableVersionRecord *version)
{
	TxnMemtableVersionOpRef op = txn_memtable_version_Record_op(version);
	TxnMemtableVersionShown shown = txn_memtable_version_Record_shown(version);
	TxnMemtableVersionPutRef put;
	uint32		ender;
	bool		emptied = op.tag == TXN_MEMTABLE_VERSION_OP_REF_EMPTIED;
	bool		named = op.tag == TXN_MEMTABLE_VERSION_OP_REF_NAMED;
	bool		ended = txn_memtable_version_Record_ender(version, &ender);
	bool		has_heaped = txn_runs_put_of(&op, &put);
	bool		heaped_true = has_heaped && put.heap_bytes != 0;
	bool		heaped_false = has_heaped && put.heap_bytes == 0;
	bool		command = shown.tag == TXN_MEMTABLE_VERSION_SHOWN_COMMAND;

	switch (names->kind)
	{
		case TXN_RUNS_KIND_ROWS:
			return emptied || named || ended || heaped_false;
		case TXN_RUNS_KIND_FREE:
			{
				TxnMemtableVersionWriter writer = txn_memtable_version_Record_writer(version);
				TxnMemtableVersionWriter none = TXN_MEMTABLE_VERSION_NO_TRANSACTION;

				return !command || heaped_true ||
					(ended && (ender < (uint32) TXN_STATUS_FIRST_NORMAL ||
							   txn_memtable_version_Writer_PartialEq_eq(&writer, &none) ||
							   txn_memtable_map_taken(op).f0 == 0));
			}
		case TXN_RUNS_KIND_VALUES:
			return emptied || named || ended || !command || heaped_true;
		case TXN_RUNS_KIND_CHUNKS:
			return emptied || ended || !command || heaped_true;
		case TXN_RUNS_KIND_STATUS:
		case TXN_RUNS_KIND_SPILLED_TUPLES:
			return true;
		case TXN_RUNS_KIND_ENTRIES:
		case TXN_RUNS_KIND_SPILLED_ENTRIES:
			{
				bool		entry;

				if (op.tag == TXN_MEMTABLE_VERSION_OP_REF_PUT)
					entry = op.u.put.heap_bytes != 0 || op.u.put.tuple.f0 != 0;
				else if (op.tag == TXN_MEMTABLE_VERSION_OP_REF_ADD)
					entry = op.u.add.heap_bytes != 0;
				else
					entry = !(op.tag == TXN_MEMTABLE_VERSION_OP_REF_DELETE ||
							  (op.tag == TXN_MEMTABLE_VERSION_OP_REF_REMOVE && !op.u.remove.has_f1));
				return entry || (names->kind == TXN_RUNS_KIND_SPILLED_ENTRIES && txn_runs_top(version) != names->redo);
			}
		case TXN_RUNS_KIND_SPILLED_ROWS:
			return emptied || heaped_false || txn_runs_top(version) != names->redo;
		case TXN_RUNS_KIND_SPILLED_VALUES:
		case TXN_RUNS_KIND_SPILLED_CHUNKS:
			return emptied || !command || heaped_true || txn_runs_top(version) != names->redo;
		case TXN_RUNS_KIND_IDENTITY:
			if (op.tag == TXN_MEMTABLE_VERSION_OP_REF_PUT)
				return op.u.put.heap_bytes != 0 || op.u.put.tuple.f0 != 0;
			return op.tag != TXN_MEMTABLE_VERSION_OP_REF_DELETE;
	}
	return true;
}

/* txn_runs_Start_t is an element of a spill's key starts: the place of the key frame that starts a key, and the key. */
typedef struct txn_runs_Start_t
{
	size_t		f0;
	StdBytes	f1;
} txn_runs_Start_t;

/* txn_runs_Decoded_drop drops a run's keys and their versions. */
void
txn_runs_Decoded_drop(TxnRunsDecoded *self)
{
	for (size_t k = 0; k < self->keys.len; k++)
		std_vec_free_Checked(&STD_VEC_AT(&self->keys, TxnRunsKey, k).versions, sizeof(TxnMemtableVersionRecord),
							 _Alignof(TxnMemtableVersionRecord));
	std_vec_free_Checked(&self->keys, sizeof(TxnRunsKey), _Alignof(TxnRunsKey));
}

/* txn_runs_bytes_lt reports whether one byte string orders before another. */
static bool
txn_runs_bytes_lt(StdBytes a, StdBytes b)
{
	return std_bytes_Ord_cmp(a.ptr, a.len, b.ptr, b.len) == STD_ORDERING_LESS;
}

/* txn_runs_bytes_eq reports whether two byte strings are equal. */
static bool
txn_runs_bytes_eq(StdBytes a, StdBytes b)
{
	return a.len == b.len && memcmp(a.ptr, b.ptr, a.len) == 0;
}

static bool txn_runs_spill_tail(StdBytes body, size_t *index_at, size_t *filter_at, HostError *error);
static bool txn_runs_filtered(StdBytes body, size_t at, XXH3_state_t * hasher, uint64 *result, HostError *error);
static bool txn_runs_indexed(StdBytes record, const StdVec *starts, StdVec *entries, HostError *error);

/* txn_runs_decode_frame_0_t is decode's state across its pad: its keys, the spill's key starts, and a key's versions, while live. */
typedef struct txn_runs_decode_frame_0_t
{
	const uint8 *bytes;
	size_t		bytes_len;
	const TxnRunsNames *names;
	uint64		hash;
	TxnRunsDecoded *result;
	HostError  *error;
	bool		ok;
	TxnRunsDecoded decoded;
	bool		decoded_live;
	/* starts is a Vec<(usize, &[u8])>. */
	StdVec		starts;
	bool		starts_live;
	/* versions is a Vec<Record>. */
	StdVec		versions;
	bool		versions_live;

	/*
	 * entries is a Vec<(&[u8], u64)> of the index frame's entries, which
	 * indexed reads.
	 */
	StdVec		entries;
	bool		entries_live;
	XXH3_state_t hasher;
} txn_runs_decode_frame_0_t;

/* txn_runs_decode_pad_0 is decode's pad: the index's entries, a key's versions, the key starts, and the keys, while live. */
static void
txn_runs_decode_pad_0(void *frame_)
{
	txn_runs_decode_frame_0_t *frame = frame_;

	if (frame->entries_live)
	{
		frame->entries_live = false;
		std_vec_free_Checked(&frame->entries, sizeof(txn_TupleRefSliceU8U64_t), _Alignof(txn_TupleRefSliceU8U64_t));
	}
	if (frame->versions_live)
	{
		frame->versions_live = false;
		std_vec_free_Checked(&frame->versions, sizeof(TxnMemtableVersionRecord), _Alignof(TxnMemtableVersionRecord));
	}
	if (frame->starts_live)
	{
		frame->starts_live = false;
		std_vec_free_Checked(&frame->starts, sizeof(txn_runs_Start_t), _Alignof(txn_runs_Start_t));
	}
	if (frame->decoded_live)
	{
		frame->decoded_live = false;
		txn_runs_Decoded_drop(&frame->decoded);
	}
}

/* txn_runs_decode_versions reads a record's versions past its key into the frame's versions, refusing what the run's kind refuses. */
static bool
txn_runs_decode_versions(txn_runs_decode_frame_0_t *frame, StdBytes record, size_t *place)
{
	size_t		count;

	if (!txn_runs_varint(record, place, &count, frame->error))
		return false;
	for (size_t k = 0; k < count; k++)
	{
		size_t		len;
		StdBytes	part;
		TxnMemtableVersionRecord version;

		if (!txn_runs_varint(record, place, &len, frame->error) ||
			!txn_runs_part(record, place, len, &part, frame->error) ||
			!txn_memtable_version_stored(part.ptr, part.len, &version, frame->error))
			return false;
		if (txn_runs_refused(frame->names, &version))
		{
			*frame->error = txn_runs_corrupt();
			return false;
		}
		if (!std_vec_try_reserve(&frame->versions, 1, sizeof(version), _Alignof(TxnMemtableVersionRecord)))
		{
			*frame->error = host_Error_out_of_memory();
			return false;
		}
		std_vec_push_Checked(&frame->versions, &version, sizeof(version), _Alignof(TxnMemtableVersionRecord));
	}
	return true;
}

/* txn_runs_decode_end reads a spill's index frame, its filter, and its trailer once decode reaches the index's place. */
static bool
txn_runs_decode_end(txn_runs_decode_frame_0_t *frame, StdBytes body, size_t index_at, size_t filter_at,
					uint64 read)
{
	StdBytes	index;
	StdBytes	trailer;
	size_t		next;
	size_t		end;
	uint64		bits;

	if (!txn_runs_frame(body.ptr, body.len, index_at, &index, &next, frame->error))
		return false;
	XXH3_64bits_update(&frame->hasher, body.ptr + index_at, next - index_at);
	if (!txn_runs_indexed(index, &frame->starts, &frame->entries, frame->error))
		return false;
	if (next != filter_at)
	{
		*frame->error = txn_runs_corrupt();
		return false;
	}
	if (!txn_runs_filtered(body, filter_at, &frame->hasher, &bits, frame->error) ||
		!txn_runs_frame(body.ptr, body.len, STD_SUB(usize, body.len, TXN_RUNS_TRAILED, 402, 44),
						&trailer, &end, frame->error))
		return false;
	if (trailer.len < TXN_RUNS_TRAILER)
	{
		*frame->error = txn_runs_corrupt();
		return false;
	}
	trailer.len = TXN_RUNS_TRAILER;
	return txn_runs_trailed(trailer, &frame->hasher, read, frame->hash, frame->error);
}

/* txn_runs_decode_body_0 is decode's body. */
static void
txn_runs_decode_body_0(void *frame_)
{
	txn_runs_decode_frame_0_t *frame = frame_;
	TxnRunsOpened opened;
	bool		has_above = false;
	uint8		above[4];
	bool		spilled = txn_runs_Kind_spilled(frame->names->kind);
	size_t		index_at = 0;
	size_t		filter_at = 0;
	uint64		read = 0;
	size_t		at;

	if (!txn_runs_opened(frame->bytes, frame->bytes_len, frame->names, &opened, frame->error))
		return;
	if (opened.tail.tag == TXN_RUNS_TAIL_COUNTER)
	{
		has_above = true;
		std_u32_to_be_bytes((uint32) (opened.tail.counter >> 16), above);
	}
	if (spilled && !txn_runs_spill_tail(opened.body, &index_at, &filter_at, frame->error))
		return;
	XXH3_64bits_reset(&frame->hasher);
	XXH3_64bits_update(&frame->hasher, opened.body.ptr, opened.at);
	frame->decoded.persistence = opened.persistence;
	frame->decoded.tail = opened.tail;
	std_vec_new(&frame->decoded.keys, sizeof(TxnRunsKey), _Alignof(TxnRunsKey));
	frame->decoded_live = true;
	std_vec_new(&frame->starts, sizeof(txn_runs_Start_t), _Alignof(txn_runs_Start_t));
	frame->starts_live = true;
	std_vec_new(&frame->entries, sizeof(txn_TupleRefSliceU8U64_t), _Alignof(txn_TupleRefSliceU8U64_t));
	frame->entries_live = true;
	at = opened.at;
	for (;;)
	{
		StdVec	   *keys = &frame->decoded.keys;
		TxnRunsKey *last = keys->len > 0 ? &STD_VEC_AT(keys, TxnRunsKey, keys->len - 1) : NULL;
		StdBytes	record;
		StdBytes	key;
		size_t		next;
		size_t		place = 0;
		size_t		len;

		if (spilled && index_at == at)
		{
			if (!txn_runs_decode_end(frame, opened.body, index_at, filter_at, read))
				break;
			*frame->result = frame->decoded;
			frame->decoded_live = false;
			frame->ok = true;
			break;
		}
		if (!txn_runs_frame(opened.body.ptr, opened.body.len, at, &record, &next, frame->error))
			break;
		if (next == opened.body.len && !spilled)
		{
			if (!txn_runs_trailed(record, &frame->hasher, read, frame->hash, frame->error))
				break;
			*frame->result = frame->decoded;
			frame->decoded_live = false;
			frame->ok = true;
			break;
		}
		XXH3_64bits_update(&frame->hasher, opened.body.ptr + at, next - at);
		if (!txn_runs_varint(record, &place, &len, frame->error) ||
			!txn_runs_part(record, &place, len, &key, frame->error))
			break;
		if (spilled && (last == NULL || !txn_runs_bytes_eq(last->key, key)))
		{
			txn_runs_Start_t start = {STD_ADD(usize, at, sizeof(TXN_RUNS_MAGIC), 417, 26), key};

			if (!std_vec_try_reserve(&frame->starts, 1, sizeof(start), _Alignof(txn_runs_Start_t)))
			{
				*frame->error = host_Error_out_of_memory();
				break;
			}
			std_vec_push_Checked(&frame->starts, &start, sizeof(start), _Alignof(txn_runs_Start_t));
		}
		if ((last != NULL && txn_runs_bytes_lt(key, last->key)) ||
			(has_above && std_bytes_Ord_cmp(key.ptr, key.len, above, 4) == STD_ORDERING_GREATER))
		{
			*frame->error = txn_runs_corrupt();
			break;
		}
		std_vec_new(&frame->versions, sizeof(TxnMemtableVersionRecord), _Alignof(TxnMemtableVersionRecord));
		frame->versions_live = true;
		if (!txn_runs_decode_versions(frame, record, &place))
			break;
		if (place != record.len)
		{
			*frame->error = txn_runs_corrupt();
			break;
		}
		read = STD_ADD(u64, read, (uint64) frame->versions.len, 477, 9);
		if (last != NULL && txn_runs_bytes_eq(last->key, key))
		{
			std_vec_extend_from_slice_Checked(&last->versions, frame->versions.ptr, frame->versions.len,
											  sizeof(TxnMemtableVersionRecord), _Alignof(TxnMemtableVersionRecord));
			frame->versions_live = false;
			std_vec_free_Checked(&frame->versions, sizeof(TxnMemtableVersionRecord),
								 _Alignof(TxnMemtableVersionRecord));
		}
		else
		{
			TxnRunsKey	added = {key, frame->versions};

			if (!std_vec_try_reserve(keys, 1, sizeof(added), _Alignof(TxnRunsKey)))
			{
				*frame->error = host_Error_out_of_memory();
				break;
			}
			std_vec_push_Checked(keys, &added, sizeof(added), _Alignof(TxnRunsKey));
			frame->versions_live = false;
		}
		at = next;
	}
	txn_runs_decode_pad_0(frame);
}

/*
 * txn_runs_decode reads a run of versions whole, refusing as data corruption
 * what opened refuses, a key out of byte order, a free space run's key past
 * its counter's block, a version's record version's stored refuses, a
 * version its run's kind refuses, bytes past a key's versions or past the
 * trailer, and what trailed refuses. A key may go on in the next record,
 * which repeats it, when its versions pass one record. Its keys and versions
 * borrow the bytes.
 */
bool
txn_runs_decode(const uint8 *bytes, size_t bytes_len, const TxnRunsNames *names, uint64 hash, TxnRunsDecoded *result,
				HostError *error)
{
	txn_runs_decode_frame_0_t frame = {.bytes = bytes, .bytes_len = bytes_len, .names = names, .hash = hash,
	.result = result, .error = error};

	std_landing(&frame, txn_runs_decode_body_0, txn_runs_decode_pad_0);
	return frame.ok;
}

/* txn_runs_place reads a place a spill's trailer holds, past the magic, refusing one inside it. */
static bool
txn_runs_place(StdBytes trailer, size_t from, size_t *result)
{
	uint64		place = std_u64_from_le_bytes(trailer.ptr + from);

	if (place > SIZE_MAX || place < sizeof(TXN_RUNS_MAGIC))
		return false;
	*result = (size_t) place - sizeof(TXN_RUNS_MAGIC);
	return true;
}

/*
 * txn_runs_spill_tail reads a spill's trailer, the last frame of its body,
 * refusing as data corruption one of another length than a spill's or
 * whose index frame's and filter's places aren't in order inside the body,
 * and answers where its index frame and its filter start in the body.
 */
static bool
txn_runs_spill_tail(StdBytes body, size_t *index_at, size_t *filter_at, HostError *error)
{
	size_t		at;
	StdBytes	trailer;
	size_t		end;

	if (body.len < TXN_RUNS_TRAILED)
	{
		*error = txn_runs_corrupt();
		return false;
	}
	at = body.len - TXN_RUNS_TRAILED;
	if (!txn_runs_frame(body.ptr, body.len, at, &trailer, &end, error))
		return false;
	if (end != body.len || trailer.len != TXN_RUNS_TRAILER + TXN_RUNS_INDEXED ||
		!txn_runs_place(trailer, TXN_RUNS_TRAILER, index_at) || !txn_runs_place(trailer, TXN_RUNS_TRAILER + 8, filter_at) ||
		!(*index_at < *filter_at && *filter_at <= at))
	{
		*error = txn_runs_corrupt();
		return false;
	}
	return true;
}

/*
 * txn_runs_filtered reads a spill's filter from its first page's place in its
 * body to its trailer, adding each page's frame to the file's xxh3, and
 * answers its count of bits, refusing as data corruption a page of no whole
 * words, past FILTER_WORDS, or short of them before the last.
 */
static bool
txn_runs_filtered(StdBytes body, size_t at, XXH3_state_t * hasher, uint64 *result, HostError *error)
{
	size_t		end = STD_SUB(usize, body.len, TXN_RUNS_TRAILED, 519, 40);
	size_t		words = 0;
	bool		short_page = false;

	while (at < end)
	{
		StdBytes	page;
		size_t		next;

		if (!txn_runs_frame(body.ptr, body.len, at, &page, &next, error))
			return false;
		if (short_page || page.len == 0 || page.len % 8 != 0 || page.len > TXN_RUNS_FILTER_WORDS * 8)
		{
			*error = txn_runs_corrupt();
			return false;
		}
		short_page = page.len < TXN_RUNS_FILTER_WORDS * 8;
		XXH3_64bits_update(hasher, body.ptr + at, next - at);
		words = STD_ADD(usize, words, page.len / 8, 527, 9);
		at = next;
	}
	if (at != end)
	{
		*error = txn_runs_corrupt();
		return false;
	}
	*result = STD_MUL(u64, (uint64) words, 64, 531, 20);
	return true;
}

/*
 * txn_runs_spill_bits returns the count of the bits of a spill's filter,
 * read from its file's bytes, refusing as data corruption what spill_tail
 * and filtered refuse.
 */
bool
txn_runs_spill_bits(const uint8 *bytes, size_t bytes_len, uint64 *result, HostError *error)
{
	StdBytes	body;
	size_t		index_at;
	size_t		filter_at;
	XXH3_state_t hasher;

	if (!txn_runs_body(bytes, bytes_len, &body))
	{
		*error = txn_runs_corrupt();
		return false;
	}
	XXH3_64bits_reset(&hasher);
	return txn_runs_spill_tail(body, &index_at, &filter_at, error) &&
		txn_runs_filtered(body, filter_at, &hasher, result, error);
}

/* txn_runs_filter_frame returns the length of the frame of a page of a spill's filter of a count of words. */
static size_t
txn_runs_filter_frame(size_t words)
{
	return STD_ADD(usize,
				   STD_ADD(usize, chunks_varint_varint_len(STD_MUL(u64, (uint64) words, 8, 546, 16)),
						   STD_MUL(usize, words, 8, 546, 36), 546, 5),
				   8, 546, 5);
}

/* txn_runs_File_read reads a stretch of a file into an empty Vec, mapping a file's error to the storage's. */
static bool
txn_runs_File_read(HostIoIo *io, const HostFile *file, uint64 at, size_t len, StdVec *result, HostError *error)
{
	ChunksErrorError failed;

	if (!chunks_file_read_at(HOST_IO_IO_READ_DOORS(io), file, at, len, result, &failed))
	{
		*error = txn_runs_storage(&failed);
		return false;
	}
	return true;
}

/*
 * txn_runs_spill_filter returns a page of the filter of a count of bits of a
 * spill's file of a length, its words' bytes, each word little-endian, read
 * from the place the bits give, since the filter's pages end where the
 * trailer starts, each full but the last, refusing as data corruption a page
 * past the filter's or whose frame doesn't read whole.
 */
bool
txn_runs_spill_filter(HostIoIo *io, const HostFile *file, uint64 len, uint64 bits, uint64 page, StdVec *result,
					  HostError *error)
{
	size_t		words = (size_t) (bits / 64);
	size_t		full = words / TXN_RUNS_FILTER_WORDS;
	size_t		last = words % TXN_RUNS_FILTER_WORDS;
	size_t		filter = STD_ADD(usize, STD_MUL(usize, full, txn_runs_filter_frame(TXN_RUNS_FILTER_WORDS),
												560, 18),
								 last == 0 ? 0 : txn_runs_filter_frame(last),
								 560, 18);
	uint64		trailed = (uint64) STD_ADD(usize, TXN_RUNS_TRAILED, filter, 561, 38);
	uint64		filter_at;
	size_t		count;
	size_t		at_page;
	uint64		at;
	StdVec		bytes;
	StdBytes	read;
	size_t		end;
	bool		ok;

	if (len < trailed || page > SIZE_MAX)
	{
		*error = txn_runs_corrupt();
		return false;
	}
	filter_at = len - trailed;
	at_page = (size_t) page;
	count = at_page < full ? TXN_RUNS_FILTER_WORDS : last;
	if (at_page > full || count == 0)
	{
		*error = txn_runs_corrupt();
		return false;
	}
	at = STD_ADD(u64, filter_at,
				 (uint64) STD_MUL(usize, at_page, txn_runs_filter_frame(TXN_RUNS_FILTER_WORDS),
								  567, 27),
				 567, 14);
	if (!txn_runs_File_read(io, file, at, txn_runs_filter_frame(count), &bytes, error))
		return false;
	ok = txn_runs_frame(bytes.ptr, bytes.len, 0, &read, &end, error);
	if (ok && !(read.len == STD_MUL(usize, count, 8, 570, 25) && end == bytes.len))
	{
		*error = txn_runs_corrupt();
		ok = false;
	}
	if (!ok)
	{
		std_vec_free_Checked(&bytes, 1, 1);
		return false;
	}
	memmove(bytes.ptr, read.ptr, read.len);
	bytes.len = read.len;
	*result = bytes;
	return true;
}

/* txn_runs_trailer_word returns a word of a spill's trailer at an offset, refusing a trailer too short to hold it. */
static bool
txn_runs_trailer_word(StdBytes trailer, size_t from, uint64 *result, HostError *error)
{
	if (trailer.len < from + 8)
	{
		*error = txn_runs_corrupt();
		return false;
	}
	*result = std_u64_from_le_bytes(trailer.ptr + from);
	return true;
}

/* txn_runs_spill_hash returns the xxh3 a spill's trailer holds, refusing as data corruption a file whose last frame isn't a spill's trailer. */
bool
txn_runs_spill_hash(const uint8 *bytes, size_t bytes_len, uint64 *result, HostError *error)
{
	StdBytes	body;
	StdBytes	trailer;
	size_t		end;

	if (!txn_runs_body(bytes, bytes_len, &body) || body.len < TXN_RUNS_TRAILED)
	{
		*error = txn_runs_corrupt();
		return false;
	}
	return txn_runs_frame(body.ptr, body.len, body.len - TXN_RUNS_TRAILED, &trailer, &end, error) &&
		txn_runs_trailer_word(trailer, 8, result, error);
}

/* txn_runs_spill_trailer reads a spill's trailer from the end of its file of a length, answering one of its words at an offset. */
static bool
txn_runs_spill_trailer(HostIoIo *io, const HostFile *file, uint64 len, size_t from, uint64 *result, HostError *error)
{
	StdVec		bytes;
	StdBytes	trailer;
	size_t		end;
	bool		ok;

	if (len < TXN_RUNS_TRAILED)
	{
		*error = txn_runs_corrupt();
		return false;
	}
	if (!txn_runs_File_read(io, file, len - TXN_RUNS_TRAILED, TXN_RUNS_TRAILED, &bytes, error))
		return false;
	ok = txn_runs_frame(bytes.ptr, bytes.len, 0, &trailer, &end, error) &&
		txn_runs_trailer_word(trailer, from, result, error);
	std_vec_free_Checked(&bytes, 1, 1);
	return ok;
}

/* txn_runs_spill_tail_hash returns the xxh3 a spill's trailer holds, read from the end of its file of a length. */
bool
txn_runs_spill_tail_hash(HostIoIo *io, const HostFile *file, uint64 len, uint64 *result, HostError *error)
{
	return txn_runs_spill_trailer(io, file, len, 8, result, error);
}

/* txn_runs_spill_tail_count returns the count of versions a spill's trailer holds, read from the end of its file of a length. */
bool
txn_runs_spill_tail_count(HostIoIo *io, const HostFile *file, uint64 len, uint64 *result, HostError *error)
{
	return txn_runs_spill_trailer(io, file, len, 0, result, error);
}

/*
 * txn_runs_index reads a spill's index frame into an empty Vec of its
 * entries, each a key and the place in the file of the key frame that starts
 * it, refusing as data corruption a record cut short or with bytes past its
 * entries.
 */
bool
txn_runs_index(const uint8 *record_ptr, size_t record_len, StdVec *result, HostError *error)
{
	StdBytes	record = {record_ptr, record_len};
	size_t		place = 0;
	size_t		count;

	if (!txn_runs_varint(record, &place, &count, error))
		return false;
	if (!std_vec_try_reserve(result, Min(count, record.len), sizeof(txn_TupleRefSliceU8U64_t),
							 _Alignof(txn_TupleRefSliceU8U64_t)))
	{
		*error = host_Error_out_of_memory();
		return false;
	}
	for (size_t k = 0; k < count; k++)
	{
		txn_TupleRefSliceU8U64_t entry;
		size_t		len;
		ChunksErrorError failed;

		if (!txn_runs_varint(record, &place, &len, error) || !txn_runs_part(record, &place, len, &entry.f0, error))
			return false;
		if (!chunks_varint_read_varint(record.ptr, record.len, &place, &entry.f1, &failed))
		{
			*error = txn_runs_storage(&failed);
			return false;
		}
		std_vec_push_Checked(result, &entry, sizeof(entry), _Alignof(txn_TupleRefSliceU8U64_t));
	}
	if (place != record.len)
	{
		*error = txn_runs_corrupt();
		return false;
	}
	return true;
}

/*
 * txn_runs_indexed checks a spill's index frame against the places and keys
 * of the key frames that start a key, refusing as data corruption an entry
 * that names another place or another key, entries out of order, and an
 * empty index of a spill with a key; it reads the entries into a scratch Vec
 * its caller's frame holds, which it empties.
 */
static bool
txn_runs_indexed(StdBytes record, const StdVec *starts, StdVec *entries, HostError *error)
{
	if (!txn_runs_index(record.ptr, record.len, entries, error))
		return false;
	if ((entries->len == 0) != (starts->len == 0))
	{
		*error = txn_runs_corrupt();
		return false;
	}
	for (size_t k = 1; k < entries->len; k++)
		if (STD_VEC_AT(entries, txn_TupleRefSliceU8U64_t, k - 1).f1 >= STD_VEC_AT(entries, txn_TupleRefSliceU8U64_t, k).f1)
		{
			*error = txn_runs_corrupt();
			return false;
		}
	for (size_t k = 0; k < entries->len; k++)
	{
		const txn_TupleRefSliceU8U64_t *entry = &STD_VEC_AT(entries, txn_TupleRefSliceU8U64_t, k);
		size_t		low = 0;
		size_t		high = starts->len;
		bool		found = false;

		if (entry->f1 > SIZE_MAX)
		{
			*error = txn_runs_corrupt();
			return false;
		}
		while (low < high)
		{
			size_t		mid = low + (high - low) / 2;
			const txn_runs_Start_t *start = &STD_VEC_AT(starts, txn_runs_Start_t, mid);

			if (start->f0 == (size_t) entry->f1)
			{
				found = txn_runs_bytes_eq(start->f1, entry->f0);
				break;
			}
			if (start->f0 < (size_t) entry->f1)
				low = mid + 1;
			else
				high = mid;
		}
		if (!found)
		{
			*error = txn_runs_corrupt();
			return false;
		}
	}
	entries->len = 0;
	return true;
}

/* txn_runs_Chunk_drop drops a Chunk, a Vec<(Vec<u8>, Vec<Vec<u8>>)>: each key and its records, then the buffer. */
void
txn_runs_Chunk_drop(StdVec *chunk)
{
	for (size_t k = 0; k < chunk->len; k++)
	{
		txn_TupleVecU8VecVecU8_t *spilled = &STD_VEC_AT(chunk, txn_TupleVecU8VecVecU8_t, k);

		std_vec_free_Checked(&spilled->f0, 1, 1);
		for (size_t j = 0; j < spilled->f1.len; j++)
			std_vec_free_Checked(&STD_VEC_AT(&spilled->f1, StdVec, j), 1, 1);
		std_vec_free_Checked(&spilled->f1, sizeof(StdVec), _Alignof(StdVec));
	}
	std_vec_free_Checked(chunk, sizeof(txn_TupleVecU8VecVecU8_t), _Alignof(txn_TupleVecU8VecVecU8_t));
}

/* txn_runs_read_frame_0_t is the state of spill_index's and spill_chunk's reads across their pad: the bytes read and what they build. */
typedef struct txn_runs_read_frame_0_t
{
	HostIoIo   *io;
	const HostFile *file;
	uint64		len;
	const TxnRunsNames *names;
	uint64		start;
	uint64		end;
	uint64	   *index_at;
	StdVec	   *result;
	HostError  *error;
	bool		ok;
	StdVec		trailer;
	bool		trailer_live;
	StdVec		bytes;
	bool		bytes_live;
	/* entries is a Vec<(&[u8], u64)> of the index frame's entries. */
	StdVec		entries;
	bool		entries_live;
	/* versions is a Vec<Vec<u8>> of a key's records. */
	StdVec		versions;
	bool		versions_live;
	bool		result_live;
} txn_runs_read_frame_0_t;

/* txn_runs_read_pad_0 is the reads' pad: a key's records, the entries, the bytes, the trailer, and what they built, while live. */
static void
txn_runs_read_pad_0(void *frame_)
{
	txn_runs_read_frame_0_t *frame = frame_;

	if (frame->versions_live)
	{
		frame->versions_live = false;
		for (size_t k = 0; k < frame->versions.len; k++)
			std_vec_free_Checked(&STD_VEC_AT(&frame->versions, StdVec, k), 1, 1);
		std_vec_free_Checked(&frame->versions, sizeof(StdVec), _Alignof(StdVec));
	}
	if (frame->entries_live)
	{
		frame->entries_live = false;
		std_vec_free_Checked(&frame->entries, sizeof(txn_TupleRefSliceU8U64_t), _Alignof(txn_TupleRefSliceU8U64_t));
	}
	if (frame->bytes_live)
	{
		frame->bytes_live = false;
		std_vec_free_Checked(&frame->bytes, 1, 1);
	}
	if (frame->trailer_live)
	{
		frame->trailer_live = false;
		std_vec_free_Checked(&frame->trailer, 1, 1);
	}
	if (frame->result_live)
	{
		frame->result_live = false;
		if (frame->names == NULL)
			txn_runs_Index_drop(frame->result);
		else
			txn_runs_Chunk_drop(frame->result);
	}
}

/* txn_runs_spill_index_body_0 is spill_index's body. */
static void
txn_runs_spill_index_body_0(void *frame_)
{
	txn_runs_read_frame_0_t *frame = frame_;
	StdBytes	trailer;
	StdBytes	record;
	size_t		end;
	uint64		filter_at;

	if (frame->len < TXN_RUNS_TRAILED || frame->len - TXN_RUNS_TRAILED < sizeof(TXN_RUNS_MAGIC))
	{
		*frame->error = txn_runs_corrupt();
		return;
	}
	if (!txn_runs_File_read(frame->io, frame->file, frame->len - TXN_RUNS_TRAILED, TXN_RUNS_TRAILED, &frame->trailer,
							frame->error))
		return;
	frame->trailer_live = true;
	if (!txn_runs_frame(frame->trailer.ptr, frame->trailer.len, 0, &trailer, &end, frame->error) ||
		!txn_runs_trailer_word(trailer, TXN_RUNS_TRAILER, frame->index_at, frame->error) ||
		!txn_runs_trailer_word(trailer, TXN_RUNS_TRAILER + 8, &filter_at, frame->error))
	{
		txn_runs_read_pad_0(frame);
		return;
	}
	if (filter_at < *frame->index_at || filter_at - *frame->index_at > SIZE_MAX)
	{
		*frame->error = txn_runs_corrupt();
		txn_runs_read_pad_0(frame);
		return;
	}
	if (!txn_runs_File_read(frame->io, frame->file, *frame->index_at, (size_t) (filter_at - *frame->index_at),
							&frame->bytes, frame->error))
	{
		txn_runs_read_pad_0(frame);
		return;
	}
	frame->bytes_live = true;
	std_vec_new(&frame->entries, sizeof(txn_TupleRefSliceU8U64_t), _Alignof(txn_TupleRefSliceU8U64_t));
	frame->entries_live = true;
	if (!txn_runs_frame(frame->bytes.ptr, frame->bytes.len, 0, &record, &end, frame->error) ||
		!txn_runs_index(record.ptr, record.len, &frame->entries, frame->error))
	{
		txn_runs_read_pad_0(frame);
		return;
	}
	std_vec_new(frame->result, sizeof(txn_TupleVecU8U64_t), _Alignof(txn_TupleVecU8U64_t));
	frame->result_live = true;
	for (size_t k = 0; k < frame->entries.len; k++)
	{
		const txn_TupleRefSliceU8U64_t *entry = &STD_VEC_AT(&frame->entries, txn_TupleRefSliceU8U64_t, k);
		txn_TupleVecU8U64_t owned = {.f1 = entry->f1};

		if (!std_vec_try_reserve(frame->result, 1, sizeof(owned), _Alignof(txn_TupleVecU8U64_t)))
		{
			*frame->error = host_Error_out_of_memory();
			txn_runs_read_pad_0(frame);
			return;
		}
		std_vec_new(&owned.f0, 1, 1);
		std_vec_extend_from_slice_Checked(&owned.f0, entry->f0.ptr, entry->f0.len, 1, 1);
		std_vec_push_Checked(frame->result, &owned, sizeof(owned), _Alignof(txn_TupleVecU8U64_t));
	}
	for (size_t k = 1; k < frame->result->len; k++)
	{
		const txn_TupleVecU8U64_t *a = &STD_VEC_AT(frame->result, txn_TupleVecU8U64_t, k - 1);
		const txn_TupleVecU8U64_t *b = &STD_VEC_AT(frame->result, txn_TupleVecU8U64_t, k);

		if (std_bytes_Ord_cmp(a->f0.ptr, a->f0.len, b->f0.ptr, b->f0.len) != STD_ORDERING_LESS || a->f1 >= b->f1)
		{
			*frame->error = txn_runs_corrupt();
			txn_runs_read_pad_0(frame);
			return;
		}
	}
	frame->result_live = false;
	frame->ok = true;
	txn_runs_read_pad_0(frame);
}

/*
 * txn_runs_spill_index reads a spill's index from its file of a length:
 * where its index frame starts, and each entry's key and the place of the
 * key frame it names, refusing as data corruption a file whose trailer or
 * index doesn't read.
 */
bool
txn_runs_spill_index(HostIoIo *io, const HostFile *file, uint64 len, uint64 *index_at, StdVec *result,
					 HostError *error)
{
	txn_runs_read_frame_0_t frame = {.io = io, .file = file, .len = len, .index_at = index_at, .result = result,
	.error = error};

	std_landing(&frame, txn_runs_spill_index_body_0, txn_runs_read_pad_0);
	return frame.ok;
}

/* txn_runs_chunk_refused reports whether a spill's record a chunk read is refused: of another writer, or of a map or chunk not of its own command. */
static bool
txn_runs_chunk_refused(const TxnRunsNames *names, StdBytes value, bool *refused, HostError *error)
{
	TxnMemtableVersionRecord version;

	if (names->kind == TXN_RUNS_KIND_SPILLED_TUPLES)
	{
		*refused = false;
		return true;
	}
	if (!txn_memtable_version_stored(value.ptr, value.len, &version, error))
		return false;
	*refused = txn_runs_top(&version) != names->redo;
	if (names->kind == TXN_RUNS_KIND_SPILLED_VALUES || names->kind == TXN_RUNS_KIND_SPILLED_CHUNKS)
		*refused = txn_memtable_version_Record_shown(&version).tag != TXN_MEMTABLE_VERSION_SHOWN_COMMAND || *refused;
	return true;
}

/* txn_runs_spill_chunk_record reads one key frame's record into the frame's chunk, joining a key's frames. */
static bool
txn_runs_spill_chunk_record(txn_runs_read_frame_0_t *frame, StdBytes record)
{
	StdVec	   *keys = frame->result;
	size_t		cursor = 0;
	size_t		len;
	size_t		count;
	StdBytes	key;
	txn_TupleVecU8VecVecU8_t *last;

	if (!txn_runs_varint(record, &cursor, &len, frame->error) ||
		!txn_runs_part(record, &cursor, len, &key, frame->error) ||
		!txn_runs_varint(record, &cursor, &count, frame->error))
		return false;
	if (frame->names->kind == TXN_RUNS_KIND_SPILLED_TUPLES && (count != 1 || key.len != 8))
	{
		*frame->error = txn_runs_corrupt();
		return false;
	}
	std_vec_new(&frame->versions, sizeof(StdVec), _Alignof(StdVec));
	frame->versions_live = true;
	for (size_t k = 0; k < count; k++)
	{
		StdBytes	value;
		bool		refused;
		StdVec		copy;

		if (!txn_runs_varint(record, &cursor, &len, frame->error) ||
			!txn_runs_part(record, &cursor, len, &value, frame->error) ||
			!txn_runs_chunk_refused(frame->names, value, &refused, frame->error))
			return false;
		if (refused)
		{
			*frame->error = txn_runs_corrupt();
			return false;
		}
		if (!std_vec_try_reserve(&frame->versions, 1, sizeof(StdVec), _Alignof(StdVec)))
		{
			*frame->error = host_Error_out_of_memory();
			return false;
		}
		std_vec_new(&copy, 1, 1);
		std_vec_extend_from_slice_Checked(&copy, value.ptr, value.len, 1, 1);
		std_vec_push_Checked(&frame->versions, &copy, sizeof(copy), _Alignof(StdVec));
	}
	if (cursor != record.len)
	{
		*frame->error = txn_runs_corrupt();
		return false;
	}
	last = keys->len > 0 ? &STD_VEC_AT(keys, txn_TupleVecU8VecVecU8_t, keys->len - 1) : NULL;
	if (last != NULL && last->f0.len == key.len && memcmp(last->f0.ptr, key.ptr, key.len) == 0)
	{
		std_vec_extend_from_slice_Checked(&last->f1, frame->versions.ptr, frame->versions.len, sizeof(StdVec),
										  _Alignof(StdVec));
		frame->versions_live = false;
		std_vec_free_Checked(&frame->versions, sizeof(StdVec), _Alignof(StdVec));
		return true;
	}
	if (last != NULL && std_bytes_Ord_cmp(last->f0.ptr, last->f0.len, key.ptr, key.len) == STD_ORDERING_GREATER)
	{
		*frame->error = txn_runs_corrupt();
		return false;
	}
	if (!std_vec_try_reserve(keys, 1, sizeof(txn_TupleVecU8VecVecU8_t), _Alignof(txn_TupleVecU8VecVecU8_t)))
	{
		*frame->error = host_Error_out_of_memory();
		return false;
	}
	{
		txn_TupleVecU8VecVecU8_t spilled = {.f1 = frame->versions};

		std_vec_new(&spilled.f0, 1, 1);
		std_vec_extend_from_slice_Checked(&spilled.f0, key.ptr, key.len, 1, 1);
		frame->versions_live = false;
		std_vec_push_Checked(keys, &spilled, sizeof(spilled), _Alignof(txn_TupleVecU8VecVecU8_t));
	}
	return true;
}

/* txn_runs_spill_chunk_body_0 is spill_chunk's body. */
static void
txn_runs_spill_chunk_body_0(void *frame_)
{
	txn_runs_read_frame_0_t *frame = frame_;
	size_t		place = 0;

	if (frame->end < frame->start || frame->end - frame->start > SIZE_MAX)
	{
		*frame->error = txn_runs_corrupt();
		return;
	}
	if (!txn_runs_File_read(frame->io, frame->file, frame->start, (size_t) (frame->end - frame->start),
							&frame->bytes, frame->error))
		return;
	frame->bytes_live = true;
	std_vec_new(frame->result, sizeof(txn_TupleVecU8VecVecU8_t), _Alignof(txn_TupleVecU8VecVecU8_t));
	frame->result_live = true;
	while (place < frame->bytes.len)
	{
		StdBytes	record;
		size_t		next;

		if (!txn_runs_frame(frame->bytes.ptr, frame->bytes.len, place, &record, &next, frame->error) ||
			!txn_runs_spill_chunk_record(frame, record))
		{
			txn_runs_read_pad_0(frame);
			return;
		}
		place = next;
	}
	frame->result_live = false;
	frame->ok = true;
	txn_runs_read_pad_0(frame);
}

/*
 * txn_runs_spill_chunk reads the keys of a spill's file between two places,
 * each frame's xxh3 and each version checked as decode checks them, a key's
 * frames joined, refusing as data corruption keys out of order.
 */
bool
txn_runs_spill_chunk(HostIoIo *io, const HostFile *file, const TxnRunsNames *names, uint64 start, uint64 end,
					 StdVec *result, HostError *error)
{
	txn_runs_read_frame_0_t frame = {.io = io, .file = file, .names = names, .start = start, .end = end,
	.result = result, .error = error};

	std_landing(&frame, txn_runs_spill_chunk_body_0, txn_runs_read_pad_0);
	return frame.ok;
}

/* txn_runs_decoded_frame_0_t is the state of decode_tuples and decode_status across their pad: the entries, the key starts, and the index's entries. */
typedef struct txn_runs_decoded_frame_0_t
{
	const uint8 *bytes;
	size_t		bytes_len;
	const TxnRunsNames *names;
	uint64		hash;
	StdVec	   *result;
	size_t		result_size;
	size_t		result_align;
	HostError  *error;
	bool		ok;
	bool		result_live;
	StdVec		starts;
	bool		starts_live;
	StdVec		entries;
	bool		entries_live;
	XXH3_state_t hasher;
} txn_runs_decoded_frame_0_t;

/* txn_runs_decoded_pad_0 is the pad of decode_tuples and decode_status: the index's entries, the key starts, and the entries read, while live. */
static void
txn_runs_decoded_pad_0(void *frame_)
{
	txn_runs_decoded_frame_0_t *frame = frame_;

	if (frame->entries_live)
	{
		frame->entries_live = false;
		std_vec_free_Checked(&frame->entries, sizeof(txn_TupleRefSliceU8U64_t), _Alignof(txn_TupleRefSliceU8U64_t));
	}
	if (frame->starts_live)
	{
		frame->starts_live = false;
		std_vec_free_Checked(&frame->starts, sizeof(txn_runs_Start_t), _Alignof(txn_runs_Start_t));
	}
	if (frame->result_live)
	{
		frame->result_live = false;
		std_vec_free_Checked(frame->result, frame->result_size, frame->result_align);
	}
}

/* txn_runs_decode_tuples_record reads one tuple id's record into the frame's entries and its start. */
static bool
txn_runs_decode_tuples_record(txn_runs_decoded_frame_0_t *frame, StdBytes record, size_t at)
{
	size_t		place = 0;
	size_t		len;
	size_t		count;
	StdBytes	tid;
	txn_TupleU64RefSliceU8_t entry;
	txn_runs_Start_t start;

	if (!txn_runs_varint(record, &place, &len, frame->error) || !txn_runs_part(record, &place, len, &tid, frame->error))
		return false;
	if (tid.len != 8)
	{
		*frame->error = txn_runs_corrupt();
		return false;
	}
	entry.f0 = std_u64_from_be_bytes(tid.ptr);
	if ((frame->result->len > 0 &&
		 STD_VEC_AT(frame->result, txn_TupleU64RefSliceU8_t, frame->result->len - 1).f0 >= entry.f0))
	{
		*frame->error = txn_runs_corrupt();
		return false;
	}
	if (!txn_runs_varint(record, &place, &count, frame->error))
		return false;
	if (count != 1)
	{
		*frame->error = txn_runs_corrupt();
		return false;
	}
	if (!txn_runs_varint(record, &place, &len, frame->error) ||
		!txn_runs_part(record, &place, len, &entry.f1, frame->error))
		return false;
	if (place != record.len)
	{
		*frame->error = txn_runs_corrupt();
		return false;
	}
	if (!std_vec_try_reserve(frame->result, 1, sizeof(entry), _Alignof(txn_TupleU64RefSliceU8_t)) ||
		!std_vec_try_reserve(&frame->starts, 1, sizeof(start), _Alignof(txn_runs_Start_t)))
	{
		*frame->error = host_Error_out_of_memory();
		return false;
	}
	start.f0 = STD_ADD(usize, at, sizeof(TXN_RUNS_MAGIC), 763, 22);
	start.f1 = tid;
	std_vec_push_Checked(frame->result, &entry, sizeof(entry), _Alignof(txn_TupleU64RefSliceU8_t));
	std_vec_push_Checked(&frame->starts, &start, sizeof(start), _Alignof(txn_runs_Start_t));
	return true;
}

/* txn_runs_decode_tuples_body_0 is decode_tuples' body. */
static void
txn_runs_decode_tuples_body_0(void *frame_)
{
	txn_runs_decoded_frame_0_t *frame = frame_;
	TxnRunsOpened opened;
	size_t		index_at;
	size_t		filter_at;
	size_t		at;
	StdBytes	index;
	StdBytes	trailer;
	size_t		next;
	uint64		bits;

	if (!txn_runs_opened(frame->bytes, frame->bytes_len, frame->names, &opened, frame->error) ||
		!txn_runs_spill_tail(opened.body, &index_at, &filter_at, frame->error))
		return;
	XXH3_64bits_reset(&frame->hasher);
	XXH3_64bits_update(&frame->hasher, opened.body.ptr, opened.at);
	std_vec_new(frame->result, sizeof(txn_TupleU64RefSliceU8_t), _Alignof(txn_TupleU64RefSliceU8_t));
	frame->result_live = true;
	std_vec_new(&frame->starts, sizeof(txn_runs_Start_t), _Alignof(txn_runs_Start_t));
	frame->starts_live = true;
	std_vec_new(&frame->entries, sizeof(txn_TupleRefSliceU8U64_t), _Alignof(txn_TupleRefSliceU8U64_t));
	frame->entries_live = true;
	at = opened.at;
	while (at < index_at)
	{
		StdBytes	record;

		if (!txn_runs_frame(opened.body.ptr, opened.body.len, at, &record, &next, frame->error))
		{
			txn_runs_decoded_pad_0(frame);
			return;
		}
		XXH3_64bits_update(&frame->hasher, opened.body.ptr + at, next - at);
		if (!txn_runs_decode_tuples_record(frame, record, at))
		{
			txn_runs_decoded_pad_0(frame);
			return;
		}
		at = next;
	}
	if (at != index_at)
	{
		*frame->error = txn_runs_corrupt();
		txn_runs_decoded_pad_0(frame);
		return;
	}
	if (!txn_runs_frame(opened.body.ptr, opened.body.len, at, &index, &next, frame->error))
	{
		txn_runs_decoded_pad_0(frame);
		return;
	}
	XXH3_64bits_update(&frame->hasher, opened.body.ptr + at, next - at);
	if (!txn_runs_indexed(index, &frame->starts, &frame->entries, frame->error))
	{
		txn_runs_decoded_pad_0(frame);
		return;
	}
	if (next != filter_at)
	{
		*frame->error = txn_runs_corrupt();
		txn_runs_decoded_pad_0(frame);
		return;
	}
	if (!txn_runs_filtered(opened.body, filter_at, &frame->hasher, &bits, frame->error) ||
		!txn_runs_frame(opened.body.ptr, opened.body.len,
						STD_SUB(usize, opened.body.len, TXN_RUNS_TRAILED, 776, 36), &trailer,
						&next, frame->error))
	{
		txn_runs_decoded_pad_0(frame);
		return;
	}
	if (trailer.len < TXN_RUNS_TRAILER)
	{
		*frame->error = txn_runs_corrupt();
		txn_runs_decoded_pad_0(frame);
		return;
	}
	trailer.len = TXN_RUNS_TRAILER;
	if (!txn_runs_trailed(trailer, &frame->hasher, (uint64) frame->result->len, frame->hash, frame->error))
	{
		txn_runs_decoded_pad_0(frame);
		return;
	}
	frame->result_live = false;
	frame->ok = true;
	txn_runs_decoded_pad_0(frame);
}

/*
 * txn_runs_decode_tuples reads a spill of tuple ids whole into its entries,
 * each a tuple id and its row's key, refusing as data corruption what opened
 * and spill_tail refuse, a key that isn't a tuple id's eight bytes or isn't
 * past the last entry's, a count of values other than one, bytes past the
 * value, an index indexed refuses, and what trailed refuses.
 */
bool
txn_runs_decode_tuples(const uint8 *bytes, size_t bytes_len, const TxnRunsNames *names, uint64 hash, StdVec *result,
					   HostError *error)
{
	txn_runs_decoded_frame_0_t frame = {.bytes = bytes, .bytes_len = bytes_len, .names = names, .hash = hash,
		.result = result, .result_size = sizeof(txn_TupleU64RefSliceU8_t),
	.result_align = _Alignof(txn_TupleU64RefSliceU8_t), .error = error};

	std_landing(&frame, txn_runs_decode_tuples_body_0, txn_runs_decoded_pad_0);
	return frame.ok;
}

/*
 * txn_runs_entry reads a status run's record, refusing as data corruption: a
 * key that isn't an xid an entry can have, eight bytes big-endian, or isn't
 * past the last entry's, a count of words other than one, a word of another
 * length than sixteen bytes, an outcome that names a process, and bytes past
 * the word.
 */
static bool
txn_runs_entry(StdBytes record, const txn_TupleU64OutcomeU64_t *last, txn_TupleU64OutcomeU64_t *result,
			   HostError *error)
{
	size_t		at = 0;
	size_t		len;
	size_t		count;
	size_t		size;
	StdBytes	part;
	uint64		key;
	HostXid		xid;

	if (!txn_runs_varint(record, &at, &len, error) || !txn_runs_part(record, &at, len, &part, error))
		return false;
	if (part.len != 8)
	{
		*error = txn_runs_corrupt();
		return false;
	}
	result->f0 = std_u64_from_be_bytes(part.ptr);
	xid.f0 = result->f0;
	if (!txn_status_key(xid, &key) || (last != NULL && last->f0 >= result->f0))
	{
		*error = txn_runs_corrupt();
		return false;
	}
	if (!txn_runs_varint(record, &at, &count, error))
		return false;
	if (count != 1)
	{
		*error = txn_runs_corrupt();
		return false;
	}
	if (!txn_runs_varint(record, &at, &size, error))
		return false;
	if (size != 16)
	{
		*error = txn_runs_corrupt();
		return false;
	}
	if (!txn_runs_part(record, &at, 8, &part, error))
		return false;
	result->f1 = txn_status_Outcome_of(std_u64_from_le_bytes(part.ptr));
	if (!txn_runs_part(record, &at, 8, &part, error))
		return false;
	result->f2 = std_u64_from_le_bytes(part.ptr);
	if (result->f1.tag == TXN_STATUS_OUTCOME_PENDING_IN || at != record.len)
	{
		*error = txn_runs_corrupt();
		return false;
	}
	return true;
}

/* txn_runs_decode_status_body_0 is decode_status' body. */
static void
txn_runs_decode_status_body_0(void *frame_)
{
	txn_runs_decoded_frame_0_t *frame = frame_;
	TxnRunsOpened opened;
	size_t		at;

	if (!txn_runs_opened(frame->bytes, frame->bytes_len, frame->names, &opened, frame->error))
		return;
	XXH3_64bits_reset(&frame->hasher);
	XXH3_64bits_update(&frame->hasher, opened.body.ptr, opened.at);
	std_vec_new(frame->result, sizeof(txn_TupleU64OutcomeU64_t), _Alignof(txn_TupleU64OutcomeU64_t));
	frame->result_live = true;
	at = opened.at;
	for (;;)
	{
		StdBytes	record;
		size_t		next;
		txn_TupleU64OutcomeU64_t entry;
		StdVec	   *entries = frame->result;

		if (!txn_runs_frame(opened.body.ptr, opened.body.len, at, &record, &next, frame->error))
			break;
		if (next == opened.body.len)
		{
			if (txn_runs_trailed(record, &frame->hasher, (uint64) entries->len, frame->hash, frame->error))
			{
				frame->result_live = false;
				frame->ok = true;
			}
			break;
		}
		XXH3_64bits_update(&frame->hasher, opened.body.ptr + at, next - at);
		if (!std_vec_try_reserve(entries, 1, sizeof(entry), _Alignof(txn_TupleU64OutcomeU64_t)))
		{
			*frame->error = host_Error_out_of_memory();
			break;
		}
		if (!txn_runs_entry(record,
							entries->len > 0 ? &STD_VEC_AT(entries, txn_TupleU64OutcomeU64_t, entries->len - 1) : NULL,
							&entry, frame->error))
			break;
		std_vec_push_Checked(entries, &entry, sizeof(entry), _Alignof(txn_TupleU64OutcomeU64_t));
		at = next;
	}
	txn_runs_decoded_pad_0(frame);
}

/*
 * txn_runs_decode_status reads a status run whole into its entries, each a
 * top-level xid with its outcome and its count of saved chain entries,
 * refusing as data corruption what opened refuses, a record entry refuses,
 * and what trailed refuses.
 */
bool
txn_runs_decode_status(const uint8 *bytes, size_t bytes_len, const TxnRunsNames *names, uint64 hash, StdVec *result,
					   HostError *error)
{
	txn_runs_decoded_frame_0_t frame = {.bytes = bytes, .bytes_len = bytes_len, .names = names, .hash = hash,
		.result = result, .result_size = sizeof(txn_TupleU64OutcomeU64_t),
	.result_align = _Alignof(txn_TupleU64OutcomeU64_t), .error = error};

	std_landing(&frame, txn_runs_decode_status_body_0, txn_runs_decoded_pad_0);
	return frame.ok;
}

/* txn_runs_tops adds the top-level xid of each version's writer among a key's records to a list, each that an entry of the status table can have. */
static bool
txn_runs_tops(const StdBytes *records, size_t records_len, StdVec *tops, HostError *error)
{
	for (size_t k = 0; k < records_len; k++)
	{
		TxnMemtableVersionRecord record = txn_memtable_version_Record_of(records[k].ptr, records[k].len);
		HostXid		xid = {txn_runs_top(&record)};
		uint64		top = xid.f0;
		uint64		key;

		if (!txn_status_key(xid, &key) || (tops->len > 0 && STD_VEC_AT(tops, uint64, tops->len - 1) == top))
			continue;
		if (!std_vec_try_reserve(tops, 1, sizeof(uint64), _Alignof(uint64)))
		{
			*error = host_Error_out_of_memory();
			return false;
		}
		std_vec_push_Checked(tops, &top, sizeof(top), _Alignof(uint64));
	}
	return true;
}

/* txn_runs_tree_name returns a run's tree's name in its file's name, answering whether the name is the file's whole name. */
static const char *
txn_runs_tree_name(TxnRunsKind kind, bool *whole)
{
	*whole = false;
	switch (kind)
	{
		case TXN_RUNS_KIND_ROWS:
		case TXN_RUNS_KIND_SPILLED_ROWS:
			return "rows";
		case TXN_RUNS_KIND_FREE:
			return "free";
		case TXN_RUNS_KIND_VALUES:
		case TXN_RUNS_KIND_SPILLED_VALUES:
			return "values";
		case TXN_RUNS_KIND_SPILLED_TUPLES:
			return "tuples";
		case TXN_RUNS_KIND_ENTRIES:
		case TXN_RUNS_KIND_SPILLED_ENTRIES:
			return "entries";
		case TXN_RUNS_KIND_CHUNKS:
		case TXN_RUNS_KIND_SPILLED_CHUNKS:
			*whole = true;
			return "chunks";
		case TXN_RUNS_KIND_IDENTITY:
			*whole = true;
			return "identity";
		case TXN_RUNS_KIND_STATUS:
			*whole = true;
			return "status";
	}
	pg_unreachable();
}

/* txn_runs_file_name writes the name of a run's file in its checkpoint's directory into a buffer, its tree's and its storage's, answering its length. */
static size_t
txn_runs_file_name(const TxnRunsNames *names, char *buffer, size_t size)
{
	bool		whole;
	const char *tree = txn_runs_tree_name(names->kind, &whole);

	if (whole)
		return (size_t) snprintf(buffer, size, "%s", tree);
	return (size_t) snprintf(buffer, size, "%s-%u-%u", tree, names->tablespace, names->relation);
}

/* txn_runs_joined returns a path joined with a name, freeing the path it extends. */
static void
txn_runs_joined(StdPathBuf *path, const char *name, size_t len)
{
	StdPathBuf	joined;

	std_path_join(path->bytes.ptr, path->bytes.len, name, len, &joined);
	std_vec_free_Checked(&path->bytes, 1, 1);
	*path = joined;
}

/* txn_runs_directory returns the directory of a repository's runs at a checkpoint's redo point. */
void
txn_runs_directory(uint32 repository, uint32 own, HostLsn redo, StdPathBuf *result)
{
	char		point[17];

	snprintf(point, sizeof(point), "%016" PRIx64, (uint64) redo);
	*result = host_tablespace_dir(repository, own);
	txn_runs_joined(result, CHUNKS_PACKS_DELTA, strlen(CHUNKS_PACKS_DELTA));
	txn_runs_joined(result, point, 16);
}

/* TXN_RUNS_SPILL is SPILL, the directory of a repository's spills in its runs' directory. */
#define TXN_RUNS_SPILL "spill"

/* txn_runs_spill_directory returns the directory of a repository's spills. */
void
txn_runs_spill_directory(uint32 repository, uint32 own, StdPathBuf *result)
{
	*result = host_tablespace_dir(repository, own);
	txn_runs_joined(result, CHUNKS_PACKS_DELTA, strlen(CHUNKS_PACKS_DELTA));
	txn_runs_joined(result, TXN_RUNS_SPILL, strlen(TXN_RUNS_SPILL));
}

/*
 * txn_runs_spill_name returns the name of a spill's file in its repository's
 * spills' directory: its tree's and its storage's, then its transaction's
 * xid and its number among that transaction's files of the storage and
 * tree.
 */
void
txn_runs_spill_name(const TxnRunsNames *names, uint32 number, StdString *result)
{
	char		name[128];
	size_t		len = txn_runs_file_name(names, name, sizeof(name));

	len += (size_t) snprintf(name + len, sizeof(name) - len, "-" UINT64_FORMAT "-%u", (uint64) names->redo, number);
	std_string_from_Checked(result, name, len);
}

/*
 * TxnRunsWriting is Writing: a run's file while it is written, what its
 * header names, its encoder, the bytes of its magic and header, which wait
 * for the first key, a spill's number, the file's path, and the file with
 * how much of it is written, once a key made it, with the bytes of the
 * record it writes next.
 */
struct TxnRunsWriting
{
	TxnRunsNames names;
	TxnRunsEncoder encoder;
	StdVec		head;
	bool		has_number;
	uint32		number;
	StdPathBuf	path;
	bool		has_file;
	HostFile	file;
	uint64		at;
	StdVec		out;
};

/* txn_runs_Writing_drop drops a run's file's writing: its file, its encoder, its bytes, and its path, then the writing. */
void
txn_runs_Writing_drop(TxnRunsWriting *self)
{
	if (self->has_file)
	{
		self->has_file = false;
		host_File_drop(&self->file);
	}
	txn_runs_Encoder_drop(&self->encoder);
	std_vec_free_Checked(&self->head, 1, 1);
	std_vec_free_Checked(&self->path.bytes, 1, 1);
	std_vec_free_Checked(&self->out, 1, 1);
	std_free(self, sizeof(TxnRunsWriting), _Alignof(TxnRunsWriting));
}

/* txn_runs_Writing_new_frame_0_t is the state of a writing's making across its pad: the writing made so far. */
typedef struct txn_runs_Writing_new_frame_0_t
{
	const char *dir;
	size_t		dir_len;
	const TxnRunsNames *names;
	TxnRunsTail tail;
	bool		has_number;
	uint32		number;
	TxnRunsWriting *writing;
	bool		writing_live;
} txn_runs_Writing_new_frame_0_t;

/* txn_runs_Writing_new_pad_0 is the pad of a writing's making: the writing made so far, while live. */
static void
txn_runs_Writing_new_pad_0(void *frame_)
{
	txn_runs_Writing_new_frame_0_t *frame = frame_;

	if (frame->writing_live)
	{
		frame->writing_live = false;
		txn_runs_Writing_drop(frame->writing);
	}
}

/* txn_runs_Writing_new_body_0 is the body of a writing's making. */
static void
txn_runs_Writing_new_body_0(void *frame_)
{
	txn_runs_Writing_new_frame_0_t *frame = frame_;
	TxnRunsWriting *writing = std_alloc(sizeof(TxnRunsWriting), _Alignof(TxnRunsWriting));
	char		name[128];
	size_t		len;

	writing->names = *frame->names;
	writing->has_number = frame->has_number;
	writing->number = frame->number;
	writing->has_file = false;
	writing->at = 0;
	std_vec_new(&writing->encoder.index, sizeof(txn_TupleVecU8U64_t), _Alignof(txn_TupleVecU8U64_t));
	std_vec_new(&writing->encoder.previous, 1, 1);
	std_vec_new(&writing->head, 1, 1);
	std_PathBuf_new(&writing->path);
	std_vec_new(&writing->out, 1, 1);
	frame->writing = writing;
	frame->writing_live = true;
	txn_runs_Encoder_begin(frame->names, frame->tail, &writing->encoder, &writing->head);
	len = txn_runs_file_name(frame->names, name, sizeof(name));
	if (frame->has_number)
		len += (size_t) snprintf(name + len, sizeof(name) - len, "-" UINT64_FORMAT "-%u",
								 (uint64) frame->names->redo, frame->number);
	std_vec_free_Checked(&writing->path.bytes, 1, 1);
	std_path_join(frame->dir, frame->dir_len, name, len, &writing->path);
	frame->writing_live = false;
}

/* txn_runs_Writing_begin starts a run's file in a directory, with its header's tail and a spill's number, writing nothing before its first key. */
static TxnRunsWriting *
txn_runs_Writing_begin(const char *dir, size_t dir_len, const TxnRunsNames *names, TxnRunsTail tail, bool has_number,
					   uint32 number)
{
	txn_runs_Writing_new_frame_0_t frame = {.dir = dir, .dir_len = dir_len, .names = names, .tail = tail,
	.has_number = has_number, .number = number};

	std_landing(&frame, txn_runs_Writing_new_body_0, txn_runs_Writing_new_pad_0);
	return frame.writing;
}

/* txn_runs_Writing_spill starts a spill's file of a number in its repository's spills' directory, with its header's tail, writing nothing before its first key. */
TxnRunsWriting *
txn_runs_Writing_spill(const char *dir, size_t dir_len, const TxnRunsNames *names, TxnRunsTail tail, uint32 number)
{
	return txn_runs_Writing_begin(dir, dir_len, names, tail, true, number);
}

/* txn_runs_Writing_path lends the file's path. */
StdBytes
txn_runs_Writing_path(const TxnRunsWriting *self)
{
	StdBytes	path = {self->path.bytes.ptr, self->path.bytes.len};

	return path;
}

/* txn_runs_Writing_write writes the record the writing holds next at its place in its file, mapping a file's error to the storage's. */
static bool
txn_runs_Writing_write(TxnRunsWriting *self, HostIoIo *io, uint64 at, HostError *error)
{
	ChunksErrorError failed;

	if (!chunks_file_write_at(HOST_IO_IO_IO_DOORS(io), &self->file, self->path.bytes.ptr, self->path.bytes.len, at,
							  self->out.ptr, self->out.len, &failed))
	{
		*error = txn_runs_storage(&failed);
		return false;
	}
	return true;
}

/* txn_runs_Writing_key appends a key's record to the file, with no lock held, making the file with its header at the first. */
bool
txn_runs_Writing_key(TxnRunsWriting *self, HostIoIo *io, const uint8 *key, size_t key_len, const StdBytes *records,
					 size_t records_len, HostError *error)
{
	ChunksErrorError failed;

	if (!self->has_file)
	{
		if (!chunks_file_create(HOST_IO_IO_IO_DOORS(io), self->path.bytes.ptr, self->path.bytes.len, &self->file,
								&failed))
		{
			*error = txn_runs_storage(&failed);
			return false;
		}
		self->has_file = true;
		if (!chunks_file_write_at(HOST_IO_IO_IO_DOORS(io), &self->file, self->path.bytes.ptr, self->path.bytes.len, 0,
								  self->head.ptr, self->head.len, &failed))
		{
			self->has_file = false;
			host_File_drop(&self->file);
			*error = txn_runs_storage(&failed);
			return false;
		}
		self->at = (uint64) self->head.len;
	}
	self->out.len = 0;
	txn_runs_Encoder_key(&self->encoder, key, key_len, records, records_len, &self->out);
	if (!txn_runs_Writing_write(self, io, self->at, error))
		return false;
	self->at = STD_ADD(u64, self->at, (uint64) self->out.len, 949, 9);
	return true;
}

/*
 * txn_runs_Writing_end writes the trailer, after a spill's index and its
 * filter's words, syncs the file, and closes it, answering the manifest's
 * entry for it, or nothing for a run no key was written to, which has no
 * file. Its owner drops the writing after.
 */
bool
txn_runs_Writing_end(TxnRunsWriting *self, HostIoIo *io, const uint64 *filter, size_t filter_len, bool *some,
					 RepoRunFile *result, HostError *error)
{
	ChunksErrorError failed;
	uint64		hash;

	*some = false;
	if (!self->has_file)
		return true;
	self->out.len = 0;
	hash = txn_runs_Encoder_end(&self->encoder, filter, filter_len, &self->out);
	if (!txn_runs_Writing_write(self, io, self->at, error))
		return false;
	if (!chunks_file_sync(HOST_IO_IO_IO_DOORS(io), &self->file, &failed))
	{
		*error = txn_runs_storage(&failed);
		return false;
	}
	result->kind = (uint8) self->names.kind;
	result->tablespace = self->names.tablespace;
	result->relation = self->names.relation;
	result->len = STD_ADD(u64, self->at, (uint64) self->out.len, 961, 41);
	result->hash = hash;
	result->has_spilled = self->has_number;
	result->spilled.xid = self->names.redo;
	result->spilled.number = self->number;
	self->has_file = false;
	host_File_drop(&self->file);
	*some = true;
	return true;
}

/* txn_runs_u64_lt is the fn item u64's PartialOrd::lt's FnMut shim, which sort_unstable passes. */
static bool
txn_runs_u64_lt(void *captures, const void *a, const void *b)
{
	(void) captures;
	return *(const uint64 *) a < *(const uint64 *) b;
}

/* The status run's sort of its xids: sort_unstable at u64. */
#define STD_SORT_SCOPE static
#define STD_SORT_DECLARE
#define STD_SORT_DEFINE
#define STD_SORT_TS U64
#define STD_SORT_T uint64
#define STD_SORT_FREEZE 1
#define STD_SORT_COPY 1
#define STD_SORT_BRANCHLESS 1
#define STD_SORT_LT txn_runs_u64_lt
#include "doltgres/std/sort_template.h"

/* txn_runs_u64_eq is u64's PartialEq, which the status run's dedup takes. */
static bool
txn_runs_u64_eq(const void *self, const void *other)
{
	return *(const uint64 *) self == *(const uint64 *) other;
}

/* txn_runs_statuses_frame_0_t is statuses' state across its pad: the xids it takes and its writing, while live. */
typedef struct txn_runs_statuses_frame_0_t
{
	HostIoIo   *io;
	StdBytes	dir;
	const TxnRunsNames *names;
	StdVec		xids;
	bool		xids_live;
	const uint64 *chained;
	size_t		chained_len;
	bool	   *some;
	RepoRunFile *result;
	HostError  *error;
	bool		ok;
	TxnRunsWriting *writing;
	bool		writing_live;
} txn_runs_statuses_frame_0_t;

/* txn_runs_statuses_pad_0 is statuses' pad: its writing and its xids, while live. */
static void
txn_runs_statuses_pad_0(void *frame_)
{
	txn_runs_statuses_frame_0_t *frame = frame_;

	if (frame->writing_live)
	{
		frame->writing_live = false;
		txn_runs_Writing_drop(frame->writing);
	}
	if (frame->xids_live)
	{
		frame->xids_live = false;
		std_vec_free_Checked(&frame->xids, sizeof(uint64), _Alignof(uint64));
	}
}

/* txn_runs_statuses_body_0 is statuses' body. */
static void
txn_runs_statuses_body_0(void *frame_)
{
	txn_runs_statuses_frame_0_t *frame = frame_;
	TxnRunsTail none = {.tag = TXN_RUNS_TAIL_NONE};

	std_slice_sort_unstable_U64(frame->xids.ptr, frame->xids.len);
	std_vec_dedup(&frame->xids, sizeof(uint64), _Alignof(uint64), txn_runs_u64_eq, NULL);
	frame->writing = txn_runs_Writing_begin((const char *) frame->dir.ptr, frame->dir.len, frame->names, none, false, 0);
	frame->writing_live = true;
	for (size_t k = 0; k < frame->xids.len; k++)
	{
		uint64		xid = STD_VEC_AT(&frame->xids, uint64, k);
		TxnStatusCommit commit;
		uint64		word;
		uint64		count = 0;
		uint8		key[8];
		uint8		value[16];
		StdBytes	record = {value, sizeof(value)};

		HostXid		asked = {xid};

		if (!txn_status_committed(frame->io, asked, &commit, frame->error))
		{
			txn_runs_statuses_pad_0(frame);
			return;
		}
		if (commit.tag == TXN_STATUS_COMMIT_MISSING)
			continue;
		word = commit.tag == TXN_STATUS_COMMIT_AT ? commit.u.at : 0;
		for (size_t j = 0; j < frame->chained_len; j++)
			if (frame->chained[j] == xid)
				count = std_wrapping_add_u64(count, 1);
		std_u64_to_le_bytes(word, value);
		std_u64_to_le_bytes(count, value + 8);
		std_u64_to_be_bytes(xid, key);
		if (!txn_runs_Writing_key(frame->writing, frame->io, key, sizeof(key), &record, 1, frame->error))
		{
			txn_runs_statuses_pad_0(frame);
			return;
		}
	}
	frame->ok = txn_runs_Writing_end(frame->writing, frame->io, NULL, 0, frame->some, frame->result, frame->error);
	txn_runs_statuses_pad_0(frame);
}

/*
 * txn_runs_statuses writes a repository's status run into a checkpoint's
 * directory: for each top-level xid its runs' versions name, in order and
 * once, that the status table holds an entry of, its outcome word, its
 * commit record's end or zero for one not yet committed, beside the number
 * of the states' saved chain entries naming the xid. It takes the xids and
 * answers the manifest's entry for it, or nothing for a run of no entry.
 */
static bool
txn_runs_statuses(HostIoIo *io, StdBytes dir, const TxnRunsNames *names, StdVec xids, const uint64 *chained,
				  size_t chained_len, bool *some, RepoRunFile *result, HostError *error)
{
	txn_runs_statuses_frame_0_t frame = {.io = io, .dir = dir, .names = names, .xids = xids, .xids_live = true,
	.chained = chained, .chained_len = chained_len, .some = some, .result = result, .error = error};

	std_landing(&frame, txn_runs_statuses_body_0, txn_runs_statuses_pad_0);
	return frame.ok;
}

/* txn_runs_written_frame_0_t is the state of written, unfolded, and cut across their pad: the writing, a key's records, and the cut's entry, while live. */
typedef struct txn_runs_written_frame_0_t
{
	HostIoIo   *io;
	StdBytes	dir;
	TxnRunsNames names;
	TxnRunsTail tail;
	HostSharedRecord owner;
	StdVec	   *xids;
	HostLsn		point;
	bool	   *some;
	RepoRunFile *result;
	HostError  *error;
	bool		ok;
	TxnRunsWriting *writing;
	bool		writing_live;
	/* records is a Vec<&[u8]> of a key's records. */
	StdVec		records;
	bool		records_live;
	txn_TupleU64VecVecU8_t entry;
	bool		entry_live;
	uint8		block[4];
} txn_runs_written_frame_0_t;

/* txn_runs_written_pad_0 is the pad of written, unfolded, and cut: the cut's entry, the records, and the writing, while live. */
static void
txn_runs_written_pad_0(void *frame_)
{
	txn_runs_written_frame_0_t *frame = frame_;

	if (frame->entry_live)
	{
		frame->entry_live = false;
		for (size_t k = 0; k < frame->entry.f1.len; k++)
			std_vec_free_Checked(&STD_VEC_AT(&frame->entry.f1, StdVec, k), 1, 1);
		std_vec_free_Checked(&frame->entry.f1, sizeof(StdVec), _Alignof(StdVec));
	}
	if (frame->records_live)
	{
		frame->records_live = false;
		std_vec_free_Checked(&frame->records, sizeof(StdBytes), _Alignof(StdBytes));
	}
	if (frame->writing_live)
	{
		frame->writing_live = false;
		txn_runs_Writing_drop(frame->writing);
	}
}

/* txn_runs_written_begin starts the frame's writing and its records. */
static void
txn_runs_written_begin(txn_runs_written_frame_0_t *frame)
{
	std_vec_new(&frame->records, sizeof(StdBytes), _Alignof(StdBytes));
	frame->records_live = true;
	frame->writing = txn_runs_Writing_begin((const char *) frame->dir.ptr, frame->dir.len, &frame->names, frame->tail,
											false, 0);
	frame->writing_live = true;
}

/* txn_runs_written_key writes a key with the records the frame gathered, their writers' top-level xids added to the frame's. */
static bool
txn_runs_written_key(txn_runs_written_frame_0_t *frame, HostIoIo *io, const uint8 *key, size_t key_len,
					 HostError *error)
{
	return txn_runs_tops(frame->records.ptr, frame->records.len, frame->xids, error) &&
		txn_runs_Writing_key(frame->writing, io, key, key_len, frame->records.ptr, frame->records.len, error);
}

/* txn_runs_written_gather gathers a key's records into the frame's records, borrowing them. */
static void
txn_runs_written_gather(txn_runs_written_frame_0_t *frame, const StdVec *values, size_t values_len)
{
	frame->records.len = 0;
	for (size_t k = 0; k < values_len; k++)
	{
		StdBytes	record = {values[k].ptr, values[k].len};

		std_vec_push_Checked(&frame->records, &record, sizeof(record), _Alignof(StdBytes));
	}
}

/* txn_runs_written_each is written's step of its walk: a key with all its records. */
static bool
txn_runs_written_each(void *captures, HostIoIo *io, const uint8 *key, size_t key_len, const StdVec *values,
					  size_t values_len, HostError *error)
{
	txn_runs_written_frame_0_t *frame = captures;

	txn_runs_written_gather(frame, values, values_len);
	return txn_runs_written_key(frame, io, key, key_len, error);
}

/* txn_runs_written_end ends the frame's writing, answering the manifest's entry, and drops it. */
static void
txn_runs_written_end(txn_runs_written_frame_0_t *frame)
{
	frame->ok = txn_runs_Writing_end(frame->writing, frame->io, NULL, 0, frame->some, frame->result, frame->error);
	txn_runs_written_pad_0(frame);
}

/* txn_runs_written_body_0 is written's body. */
static void
txn_runs_written_body_0(void *frame_)
{
	txn_runs_written_frame_0_t *frame = frame_;
	const TxnMemtableTableTreeOps *tree = &TXN_MEMTABLE_TABLE_ROWS_TREE_OPS;

	if (frame->names.kind == TXN_RUNS_KIND_VALUES)
		tree = &TXN_MEMTABLE_MAP_VALUES_TREE_OPS;
	else if (frame->names.kind == TXN_RUNS_KIND_CHUNKS)
		tree = &TXN_MEMTABLE_BRANCH_CHUNKS_TREE_OPS;
	txn_runs_written_begin(frame);
	if (!txn_memtable_table_walked(tree, frame->io, frame->owner, txn_runs_written_each, frame, NULL, frame->error))
	{
		txn_runs_written_pad_0(frame);
		return;
	}
	txn_runs_written_end(frame);
}

/*
 * txn_runs_written writes a tree's run, with its header's tail, into a
 * checkpoint's directory as a walk of the tree hands its keys out, the tree
 * the names' kind gives, rows or entries, values, or chunks, answering the
 * manifest's entry for it, or nothing for a tree with no version.
 */
static bool
txn_runs_written(HostIoIo *io, StdBytes dir, TxnRunsNames names, TxnRunsTail tail, HostSharedRecord owner,
				 StdVec *xids, bool *some, RepoRunFile *result, HostError *error)
{
	txn_runs_written_frame_0_t frame = {.io = io, .dir = dir, .names = names, .tail = tail, .owner = owner,
	.xids = xids, .some = some, .result = result, .error = error};

	std_landing(&frame, txn_runs_written_body_0, txn_runs_written_pad_0);
	return frame.ok;
}

/* txn_runs_unfolded_each is unfolded's step of its walk: a key's records the save's fold point doesn't apply. */
static bool
txn_runs_unfolded_each(void *captures, HostIoIo *io, const uint8 *key, size_t key_len, const StdVec *values,
					   size_t values_len, HostError *error)
{
	txn_runs_written_frame_0_t *frame = captures;

	frame->records.len = 0;
	for (size_t k = 0; k < values_len; k++)
	{
		TxnMemtableVersionRecord stored = txn_memtable_version_Record_of(values[k].ptr, values[k].len);
		TxnMemtableVersionWriter writer = txn_memtable_version_Record_writer(&stored);
		uint64		lsn;
		bool		has_lsn = txn_memtable_version_Record_lsn(&stored, &lsn);
		HostStatus	status;
		TxnFoldApplied applied;

		if (!txn_outcome_stamped(io, writer.xid, &status, error) ||
			!txn_fold_applied_by(io, writer, has_lsn, lsn, status, frame->point, &applied, error))
			return false;
		if (applied.tag != TXN_FOLD_APPLIED_YES)
		{
			StdBytes	record = {values[k].ptr, values[k].len};

			std_vec_push_Checked(&frame->records, &record, sizeof(record), _Alignof(StdBytes));
		}
	}
	if (frame->records.len == 0)
		return true;
	return txn_runs_written_key(frame, io, key, key_len, error);
}

/* txn_runs_unfolded_body_0 is unfolded's body. */
static void
txn_runs_unfolded_body_0(void *frame_)
{
	txn_runs_written_frame_0_t *frame = frame_;

	txn_runs_written_begin(frame);
	if (!txn_memtable_table_walked(&TXN_MEMTABLE_BRANCH_IDENTITY_TREE_OPS, frame->io, frame->owner,
								   txn_runs_unfolded_each, frame, NULL, frame->error))
	{
		txn_runs_written_pad_0(frame);
		return;
	}
	txn_runs_written_end(frame);
}

/*
 * txn_runs_unfolded writes a database's identity run as written writes a
 * tree's, leaving out each version the checkpoint's save applies at its fold
 * point, since the manifest that save writes holds it.
 */
static bool
txn_runs_unfolded(HostIoIo *io, StdBytes dir, TxnRunsNames names, HostSharedRecord deltas, HostLsn point,
				  StdVec *xids, bool *some, RepoRunFile *result, HostError *error)
{
	txn_runs_written_frame_0_t frame = {.io = io, .dir = dir, .names = names, .owner = deltas, .xids = xids,
	.point = point, .some = some, .result = result, .error = error};

	std_landing(&frame, txn_runs_unfolded_body_0, txn_runs_written_pad_0);
	return frame.ok;
}

/* txn_runs_cut_each is cut's step of its walk: the entries of the blocks below the counter's. */
static bool
txn_runs_cut_each(void *captures, HostIoIo *io, const uint8 *key, size_t key_len, const StdVec *values,
				  size_t values_len, HostError *error)
{
	txn_runs_written_frame_0_t *frame = captures;

	if (std_bytes_Ord_cmp(key, key_len, frame->block, sizeof(frame->block)) != STD_ORDERING_LESS)
		return true;
	txn_runs_written_gather(frame, values, values_len);
	return txn_runs_written_key(frame, io, key, key_len, error);
}

/* txn_runs_cut_body_0 is cut's body. */
static void
txn_runs_cut_body_0(void *frame_)
{
	txn_runs_written_frame_0_t *frame = frame_;
	bool		some;

	*frame->some = false;
	if (!txn_memtable_map_cut(frame->io, frame->owner, &some, &frame->entry, frame->error))
		return;
	if (!some)
	{
		frame->ok = true;
		return;
	}
	frame->entry_live = true;
	if (!host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-runs-free-cut", frame->error))
	{
		txn_runs_written_pad_0(frame);
		return;
	}
	std_u32_to_be_bytes((uint32) (frame->entry.f0 >> 16), frame->block);
	frame->tail.tag = TXN_RUNS_TAIL_COUNTER;
	frame->tail.counter = frame->entry.f0;
	txn_runs_written_begin(frame);
	if (!txn_memtable_table_walked(&TXN_MEMTABLE_MAP_FREE_TREE_OPS, frame->io, frame->owner, txn_runs_cut_each, frame,
								   NULL, frame->error))
	{
		txn_runs_written_pad_0(frame);
		return;
	}
	if (frame->entry.f1.len > 0)
	{
		txn_runs_written_gather(frame, frame->entry.f1.ptr, frame->entry.f1.len);
		if (!txn_runs_written_key(frame, frame->io, frame->block, sizeof(frame->block), frame->error))
		{
			txn_runs_written_pad_0(frame);
			return;
		}
	}
	txn_runs_written_end(frame);
}

/*
 * txn_runs_cut writes a storage's free space run, cut at the tuple id
 * counter: the counter and its block's entry as map's cut reads them
 * together, the entries of the blocks below it as a walk hands them out
 * after, and no block above, with the counter in the run's header.
 */
static bool
txn_runs_cut(HostIoIo *io, StdBytes dir, TxnRunsNames names, HostSharedRecord state, StdVec *xids, bool *some,
			 RepoRunFile *result, HostError *error)
{
	txn_runs_written_frame_0_t frame = {.io = io, .dir = dir, .names = names, .owner = state, .xids = xids,
	.some = some, .result = result, .error = error};

	std_landing(&frame, txn_runs_cut_body_0, txn_runs_written_pad_0);
	return frame.ok;
}

/* txn_runs_write_frame_0_t is write's state across its pad: its directories, its files and xids, the states, a held state or delta, and a home's named spills, while live. */
typedef struct txn_runs_write_frame_0_t
{
	HostIoIo   *io;
	uint32		repository;
	uint32		own;
	HostLsn		redo;
	HostLsn		point;
	const uint64 *chained;
	size_t		chained_len;
	RepoRuns   *result;
	HostError  *error;
	bool		ok;
	StdPathBuf	dir;
	bool		dir_live;
	/* files is a Vec<RunFile>. */
	StdVec		files;
	bool		files_live;
	/* xids is a Vec<u64>. */
	StdVec		xids;
	bool		xids_live;
	/* states is a Vec<(u64, Held<State>)>, of which moved are taken. */
	StdVec		states;
	bool		states_live;
	size_t		moved;
	HostSharedHeld held;
	bool		held_live;
	/* named is a Vec<RunFile> of a home's spills. */
	StdVec		named;
	bool		named_live;
} txn_runs_write_frame_0_t;

/* txn_runs_write_pad_0 is write's pad: the named spills, the held state, the states, the xids, the files, and the directory, while live. */
static void
txn_runs_write_pad_0(void *frame_)
{
	txn_runs_write_frame_0_t *frame = frame_;

	if (frame->named_live)
	{
		frame->named_live = false;
		std_vec_free_Checked(&frame->named, sizeof(RepoRunFile), _Alignof(RepoRunFile));
	}
	if (frame->held_live)
	{
		frame->held_live = false;
		host_shared_Held_drop(&frame->held);
	}
	if (frame->states_live)
	{
		frame->states_live = false;
		for (size_t k = frame->moved; k < frame->states.len; k++)
			host_shared_Held_drop(&STD_VEC_AT(&frame->states, txn_TupleU64HeldState_t, k).f1);
		std_vec_free_Checked(&frame->states, sizeof(txn_TupleU64HeldState_t), _Alignof(txn_TupleU64HeldState_t));
	}
	if (frame->xids_live)
	{
		frame->xids_live = false;
		std_vec_free_Checked(&frame->xids, sizeof(uint64), _Alignof(uint64));
	}
	if (frame->files_live)
	{
		frame->files_live = false;
		std_vec_free_Checked(&frame->files, sizeof(RepoRunFile), _Alignof(RepoRunFile));
	}
	if (frame->dir_live)
	{
		frame->dir_live = false;
		std_vec_free_Checked(&frame->dir.bytes, 1, 1);
	}
}

/* txn_runs_write_dir returns the frame's directory as bytes. */
static StdBytes
txn_runs_write_dir(const txn_runs_write_frame_0_t *frame)
{
	StdBytes	dir = {frame->dir.bytes.ptr, frame->dir.bytes.len};

	return dir;
}

/* txn_runs_write_file adds a run's manifest entry to the frame's files, when its writing answered one. */
static void
txn_runs_write_file(txn_runs_write_frame_0_t *frame, bool some, const RepoRunFile *file)
{
	if (some)
		std_vec_push_Checked(&frame->files, file, sizeof(*file), _Alignof(RepoRunFile));
}

/* txn_runs_write_named adds the spill files a home's manifest and its processes' slots name, a checkpoint's, to the frame's files, and their xids. */
static bool
txn_runs_write_named(txn_runs_write_frame_0_t *frame, const TxnSpillHomeOps *home, HostSharedRecord record,
					 const HostStorage *storage)
{
	if (!txn_spill_named(home, frame->io, record, storage, &frame->named, frame->error))
		return false;
	frame->named_live = true;
	for (size_t k = 0; k < frame->named.len; k++)
	{
		const RepoRunFile *file = &STD_VEC_AT(&frame->named, RepoRunFile, k);

		if (file->has_spilled)
		{
			if (!std_vec_try_reserve(&frame->xids, 1, sizeof(uint64), _Alignof(uint64)))
			{
				*frame->error = host_Error_out_of_memory();
				return false;
			}
			std_vec_push_Checked(&frame->xids, &file->spilled.xid, sizeof(uint64), _Alignof(uint64));
		}
		if (!std_vec_try_reserve(&frame->files, 1, sizeof(RepoRunFile), _Alignof(RepoRunFile)))
		{
			*frame->error = host_Error_out_of_memory();
			return false;
		}
		std_vec_push_Checked(&frame->files, file, sizeof(*file), _Alignof(RepoRunFile));
	}
	frame->named_live = false;
	std_vec_free_Checked(&frame->named, sizeof(RepoRunFile), _Alignof(RepoRunFile));
	return true;
}

/* txn_runs_write_state writes the runs of one state: its rows' or entries', its free space's, its value map's, and its named spills. */
static bool
txn_runs_write_state(txn_runs_write_frame_0_t *frame)
{
	HostSharedRecord record = host_shared_Held_record(&frame->held);
	HostStorage storage;
	TxnStatePersistence persistence;
	TxnBaseBase base;
	bool		some;
	bool		indexed = false;
	TxnRunsNames names;
	TxnRunsTail tail = {.tag = TXN_RUNS_TAIL_NONE};
	TxnRunsTail none = {.tag = TXN_RUNS_TAIL_NONE};
	RepoRunFile file;

	if (!txn_state_described(record, &storage, &persistence))
		return true;
	if (!txn_state_base_record(frame->io, &frame->held, &some, &base, frame->error))
		return false;
	if (some)
	{
		if (base.has_index)
		{
			indexed = true;
			tail.tag = TXN_RUNS_TAIL_INDEX;
			tail.table = base.table;
			tail.index = base.index;
		}
		txn_base_Base_drop(&base);
	}
	names.kind = indexed ? TXN_RUNS_KIND_ENTRIES : TXN_RUNS_KIND_ROWS;
	names.persistence = persistence;
	names.tablespace = storage.tablespace;
	names.database = storage.database;
	names.relation = storage.relation;
	names.redo = frame->redo;
	if (!txn_runs_written(frame->io, txn_runs_write_dir(frame), names, tail, record, &frame->xids, &some, &file,
						  frame->error))
		return false;
	txn_runs_write_file(frame, some, &file);
	if (!host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-runs-rows-swept", frame->error))
		return false;
	names.kind = TXN_RUNS_KIND_FREE;
	if (!txn_runs_cut(frame->io, txn_runs_write_dir(frame), names, record, &frame->xids, &some, &file, frame->error))
		return false;
	txn_runs_write_file(frame, some, &file);
	names.kind = TXN_RUNS_KIND_VALUES;
	if (!txn_runs_written(frame->io, txn_runs_write_dir(frame), names, none, record, &frame->xids, &some, &file,
						  frame->error))
		return false;
	txn_runs_write_file(frame, some, &file);
	return txn_runs_write_named(frame, &TXN_SPILL_STATE_HOME_OPS, record, &storage);
}

/* txn_runs_write_deltas writes the runs of a repository's working state's delta: its chunks', its named chunk spills, and its identity's. */
static bool
txn_runs_write_deltas(txn_runs_write_frame_0_t *frame)
{
	HostSharedRecord record = host_shared_Held_record(&frame->held);
	TxnRunsNames names = {.kind = TXN_RUNS_KIND_CHUNKS, .persistence = TXN_STATE_PERSISTENCE_PERMANENT,
	.database = frame->repository, .redo = frame->redo};
	TxnRunsTail none = {.tag = TXN_RUNS_TAIL_NONE};
	HostStorage chunks = txn_spill_chunks_storage(frame->repository);
	bool		some;
	RepoRunFile file;

	if (!txn_runs_written(frame->io, txn_runs_write_dir(frame), names, none, record, &frame->xids, &some, &file,
						  frame->error))
		return false;
	txn_runs_write_file(frame, some, &file);
	if (!txn_spill_chunks_ended(frame->io, record, frame->error) ||
		!txn_runs_write_named(frame, &TXN_SPILL_BRANCH_DELTAS_HOME_OPS, record, &chunks))
		return false;
	names.kind = TXN_RUNS_KIND_IDENTITY;
	if (!txn_runs_unfolded(frame->io, txn_runs_write_dir(frame), names, record, frame->point, &frame->xids, &some,
						   &file, frame->error))
		return false;
	txn_runs_write_file(frame, some, &file);
	return true;
}

/* txn_runs_create_dir makes a directory if it isn't there, mapping a file's error to the storage's. */
static bool
txn_runs_create_dir(HostIoIo *io, const char *dir, size_t dir_len, HostError *error)
{
	ChunksErrorError failed;

	if (!chunks_file_create_dir(HOST_IO_IO_IO_DOORS(io), dir, dir_len, &failed))
	{
		*error = txn_runs_storage(&failed);
		return false;
	}
	return true;
}

/* txn_runs_write_body_0 is write's body. */
static void
txn_runs_write_body_0(void *frame_)
{
	txn_runs_write_frame_0_t *frame = frame_;
	TxnRunsNames names = {.kind = TXN_RUNS_KIND_STATUS, .persistence = TXN_STATE_PERSISTENCE_PERMANENT,
	.database = frame->repository, .redo = frame->redo};
	StdStr		parent;
	bool		some;
	RepoRunFile file;
	StdVec		xids;

	txn_runs_directory(frame->repository, frame->own, frame->redo, &frame->dir);
	frame->dir_live = true;
	if (!std_path_parent(frame->dir.bytes.ptr, frame->dir.bytes.len, &parent))
	{
		parent.ptr = frame->dir.bytes.ptr;
		parent.len = frame->dir.bytes.len;
	}
	if (!txn_runs_create_dir(frame->io, parent.ptr, parent.len, frame->error) ||
		!txn_runs_create_dir(frame->io, frame->dir.bytes.ptr, frame->dir.bytes.len, frame->error))
	{
		txn_runs_write_pad_0(frame);
		return;
	}
	std_vec_new(&frame->files, sizeof(RepoRunFile), _Alignof(RepoRunFile));
	frame->files_live = true;
	std_vec_new(&frame->xids, sizeof(uint64), _Alignof(uint64));
	frame->xids_live = true;
	if (!txn_state_states(frame->io, frame->repository, &frame->states, frame->error))
	{
		txn_runs_write_pad_0(frame);
		return;
	}
	frame->states_live = true;
	while (frame->moved < frame->states.len)
	{
		frame->held = STD_VEC_AT(&frame->states, txn_TupleU64HeldState_t, frame->moved).f1;
		frame->held_live = true;
		frame->moved = std_wrapping_add_usize(frame->moved, 1);
		if (!txn_runs_write_state(frame))
		{
			txn_runs_write_pad_0(frame);
			return;
		}
		frame->held_live = false;
		host_shared_Held_drop(&frame->held);
	}
	frame->states_live = false;
	std_vec_free_Checked(&frame->states, sizeof(txn_TupleU64HeldState_t), _Alignof(txn_TupleU64HeldState_t));
	if (!txn_memtable_branch_find(HOST_SHARED_IO_WITHIN(frame->io), frame->repository, &some, &frame->held,
								  frame->error))
	{
		txn_runs_write_pad_0(frame);
		return;
	}
	if (some)
	{
		frame->held_live = true;
		if (!txn_runs_write_deltas(frame))
		{
			txn_runs_write_pad_0(frame);
			return;
		}
		frame->held_live = false;
		host_shared_Held_drop(&frame->held);
	}
	if (!std_vec_try_reserve(&frame->xids, frame->chained_len, sizeof(uint64), _Alignof(uint64)))
	{
		*frame->error = host_Error_out_of_memory();
		txn_runs_write_pad_0(frame);
		return;
	}
	std_vec_extend_from_slice_Checked(&frame->xids, frame->chained, frame->chained_len, sizeof(uint64),
									  _Alignof(uint64));
	xids = frame->xids;
	frame->xids_live = false;
	if (!txn_runs_statuses(frame->io, txn_runs_write_dir(frame), &names, xids, frame->chained, frame->chained_len,
						   &some, &file, frame->error))
	{
		txn_runs_write_pad_0(frame);
		return;
	}
	txn_runs_write_file(frame, some, &file);
	frame->result->redo = frame->redo;
	frame->result->files = frame->files;
	frame->files_live = false;
	frame->ok = true;
	txn_runs_write_pad_0(frame);
}

/*
 * txn_runs_write writes a repository's runs at a checkpoint's redo point:
 * for each filled state of its database, a run of each of its trees that
 * holds a version, with the storage's persistence, one of its working
 * state's chunks, and its status run of the outcomes of the transactions
 * those versions name, in a directory of their own, each file synced and
 * its entry durable from its creation; it answers the manifest's field that
 * names them. The identity run leaves out what the save's fold point
 * applies. It sweeps a state's rows before it cuts its free space run.
 */
static bool
txn_runs_write(HostIoIo *io, uint32 repository, uint32 own, HostLsn redo, HostLsn point, const uint64 *chained,
			   size_t chained_len, RepoRuns *result, HostError *error)
{
	txn_runs_write_frame_0_t frame = {.io = io, .repository = repository, .own = own, .redo = redo, .point = point,
	.chained = chained, .chained_len = chained_len, .result = result, .error = error};

	std_landing(&frame, txn_runs_write_body_0, txn_runs_write_pad_0);
	return frame.ok;
}

/* TxnRunsRemoval is Removal: when removed runs, after a checkpoint's manifest is written, or at a start, before redo. */
typedef enum TxnRunsRemoval
{
	TXN_RUNS_REMOVAL_CHECKPOINT,
	TXN_RUNS_REMOVAL_START,
} TxnRunsRemoval;

/* txn_runs_host returns a host error as a chunks error, as chunks::Error::Host wraps it. */
static ChunksErrorError
txn_runs_host(HostError error)
{
	ChunksErrorError wrapped = {.tag = CHUNKS_ERROR_ERROR_HOST};

	wrapped.u.host = error;
	return wrapped;
}

/* txn_runs_repo_frame_0_t is the state of cleaned and checkpoint across their pad: a repository open, its reader, and what each holds of it, while live. */
typedef struct txn_runs_repo_frame_0_t
{
	HostIoIo   *io;
	HostLsn		redo;
	HostError  *error;
	bool		ok;
	/* repositories is a Vec<(u32, u32)>. */
	StdVec		repositories;
	bool		repositories_live;
	RepoRepo	repo;
	bool		repo_live;
	HostSharedReader reading;
	bool		reading_live;
	/* storages is a Vec<Storage>. */
	StdVec		storages;
	bool		storages_live;
	RepoBaseRecord read;
	bool		read_live;
	/* states is a Vec<(u64, Held<State>)>, of which moved are taken. */
	StdVec		states;
	bool		states_live;
	size_t		moved;
	HostSharedHeld held;
	bool		held_live;
	/* chained is a Vec<u64>. */
	StdVec		chained;
	bool		chained_live;
	StdVec		settled;
	bool		settled_live;
	RepoRuns	runs;
	TxnStateSaving saving;
	bool		saving_live;
	/* edits is a Vec<Gathered> a gathering makes. */
	StdVec		edits;
	bool		edits_live;
	/* pairs is a Vec<Pair>, of which moved_pairs are taken. */
	StdVec		pairs;
	bool		pairs_live;
	size_t		moved_pairs;
	uint32		database;
	HostLsn		point;
} txn_runs_repo_frame_0_t;

/* txn_runs_pairs_drop drops a Vec<Pair>'s pairs from an index on, and its buffer. */
static void
txn_runs_pairs_drop(StdVec *pairs, size_t from)
{
	for (size_t k = from; k < pairs->len; k++)
	{
		TxnIdentityPair *pair = &STD_VEC_AT(pairs, TxnIdentityPair, k);

		std_vec_free_Checked(&pair->f0, 1, 1);
		if (pair->has_f1)
			std_vec_free_Checked(&pair->f1, 1, 1);
	}
	std_vec_free_Checked(pairs, sizeof(TxnIdentityPair), _Alignof(TxnIdentityPair));
}

/* txn_runs_repo_step_drop drops what a repository's step holds, while live. */
static void
txn_runs_repo_step_drop(txn_runs_repo_frame_0_t *frame)
{
	if (frame->pairs_live)
	{
		frame->pairs_live = false;
		txn_runs_pairs_drop(&frame->pairs, frame->moved_pairs);
	}
	if (frame->edits_live)
	{
		frame->edits_live = false;
		for (size_t k = 0; k < frame->edits.len; k++)
			repo_Gathered_drop(&STD_VEC_AT(&frame->edits, RepoGathered, k));
		std_vec_free_Checked(&frame->edits, sizeof(RepoGathered), _Alignof(RepoGathered));
	}
	if (frame->saving_live)
	{
		frame->saving_live = false;
		std_vec_free_Checked(&frame->saving.locations, sizeof(uint64), _Alignof(uint64));
	}
	if (frame->settled_live)
	{
		frame->settled_live = false;
		std_vec_free_Checked(&frame->settled, sizeof(uint64), _Alignof(uint64));
	}
	if (frame->chained_live)
	{
		frame->chained_live = false;
		std_vec_free_Checked(&frame->chained, sizeof(uint64), _Alignof(uint64));
	}
	if (frame->held_live)
	{
		frame->held_live = false;
		host_shared_Held_drop(&frame->held);
	}
	if (frame->states_live)
	{
		frame->states_live = false;
		for (size_t k = frame->moved; k < frame->states.len; k++)
			host_shared_Held_drop(&STD_VEC_AT(&frame->states, txn_TupleU64HeldState_t, k).f1);
		std_vec_free_Checked(&frame->states, sizeof(txn_TupleU64HeldState_t), _Alignof(txn_TupleU64HeldState_t));
	}
	if (frame->read_live)
	{
		frame->read_live = false;
		repo_BaseRecord_drop(&frame->read);
	}
	if (frame->storages_live)
	{
		frame->storages_live = false;
		std_vec_free_Checked(&frame->storages, sizeof(HostStorage), _Alignof(HostStorage));
	}
	if (frame->reading_live)
	{
		frame->reading_live = false;
		host_shared_Reader_drop(&frame->reading);
	}
	if (frame->repo_live)
	{
		frame->repo_live = false;
		repo_Repo_drop(&frame->repo);
	}
}

/* txn_runs_repo_pad_0 is the pad of cleaned and checkpoint: a repository's step's holdings, then the repositories, while live. */
static void
txn_runs_repo_pad_0(void *frame_)
{
	txn_runs_repo_frame_0_t *frame = frame_;

	txn_runs_repo_step_drop(frame);
	if (frame->repositories_live)
	{
		frame->repositories_live = false;
		std_vec_free_Checked(&frame->repositories, sizeof(std_TupleU32U32_t), _Alignof(std_TupleU32U32_t));
	}
}

/* txn_runs_repo_open opens a repository and takes a reader, mapping a file's error to the storage's. */
static bool
txn_runs_repo_open(txn_runs_repo_frame_0_t *frame, uint32 repository, uint32 own)
{
	ChunksErrorError failed;

	if (!repo_Repo_open(frame->io, repository, own, &frame->repo, &failed))
	{
		*frame->error = txn_base_storage(&failed);
		return false;
	}
	frame->repo_live = true;
	if (!host_shared_read(frame->io, &frame->reading, frame->error))
		return false;
	frame->reading_live = true;
	return true;
}

/* txn_runs_repo_close closes the frame's repository with its reader, mapping a file's error to the storage's. */
static bool
txn_runs_repo_close(txn_runs_repo_frame_0_t *frame)
{
	ChunksErrorError failed;
	bool		closed;

	frame->repo_live = false;
	closed = repo_Repo_close(&frame->repo, frame->io, &frame->reading, &failed);
	if (!closed)
		*frame->error = txn_base_storage(&failed);
	txn_runs_repo_step_drop(frame);
	return closed;
}

/* txn_runs_init_forked_name adds the storage an init fork's name names, a relation number then INIT, to the found storages. */
static bool
txn_runs_init_forked_name(StdVec *found, uint32 tablespace, uint32 database, const StdString *name, HostError *error)
{
	static const char init[] = "_init";
	size_t		len = sizeof(init) - 1;
	uint32		relation;
	StdParseIntError failed;
	HostStorage storage;

	if (name->len < len || memcmp((const char *) name->ptr + name->len - len, init, len) != 0 ||
		!std_str_parse_u32(name->ptr, name->len - len, &relation, &failed))
		return true;
	if (!std_vec_try_reserve(found, 1, sizeof(storage), _Alignof(HostStorage)))
	{
		*error = host_Error_out_of_memory();
		return false;
	}
	storage.tablespace = tablespace;
	storage.database = database;
	storage.relation = relation;
	std_vec_push_Checked(found, &storage, sizeof(storage), _Alignof(HostStorage));
	return true;
}

/* txn_runs_init_frame_0_t is init_forked's state across its pad: the tablespaces, a directory, and its names, while live. */
typedef struct txn_runs_init_frame_0_t
{
	HostIoIo   *io;
	uint32		database;
	StdVec	   *result;
	HostError  *error;
	bool		ok;
	bool		result_live;
	/* tablespaces is a Vec<u32>. */
	StdVec		tablespaces;
	bool		tablespaces_live;
	StdPathBuf	dir;
	bool		dir_live;
	/* names is a Vec<String>. */
	StdVec		names;
	bool		names_live;
} txn_runs_init_frame_0_t;

/* txn_runs_names_drop drops a Vec<String> of a directory's names. */
static void
txn_runs_names_drop(StdVec *names)
{
	for (size_t k = 0; k < names->len; k++)
		std_vec_free_Checked(&STD_VEC_AT(names, StdString, k), 1, 1);
	std_vec_free_Checked(names, sizeof(StdString), _Alignof(StdString));
}

/* txn_runs_init_pad_0 is init_forked's pad: a directory's names, the directory, the tablespaces, and the storages found, while live. */
static void
txn_runs_init_pad_0(void *frame_)
{
	txn_runs_init_frame_0_t *frame = frame_;

	if (frame->names_live)
	{
		frame->names_live = false;
		txn_runs_names_drop(&frame->names);
	}
	if (frame->dir_live)
	{
		frame->dir_live = false;
		std_vec_free_Checked(&frame->dir.bytes, 1, 1);
	}
	if (frame->tablespaces_live)
	{
		frame->tablespaces_live = false;
		std_vec_free_Checked(&frame->tablespaces, sizeof(uint32), _Alignof(uint32));
	}
	if (frame->result_live)
	{
		frame->result_live = false;
		std_vec_free_Checked(frame->result, sizeof(HostStorage), _Alignof(HostStorage));
	}
}

/* txn_runs_init_tablespace adds the storages of the database's init forks in one tablespace's directory of its relation files. */
static bool
txn_runs_init_tablespace(txn_runs_init_frame_0_t *frame, uint32 tablespace)
{
	char		database[16];
	int			len;
	bool		some;

	frame->dir = host_versioned_dir(tablespace);
	frame->dir_live = true;
	if (!std_PathBuf_pop(&frame->dir))
		frame->dir.bytes.len = 0;
	if (tablespace == HOST_PG_DEFAULT)
		std_PathBuf_push(&frame->dir, "base", 4);
	len = snprintf(database, sizeof(database), "%u", frame->database);
	std_PathBuf_push(&frame->dir, database, (size_t) len);
	if (!host_list_dir_if_there(HOST_IO_IO_READ_DOORS(frame->io), frame->dir.bytes.ptr, frame->dir.bytes.len, &some,
								&frame->names, frame->error))
		return false;
	if (some)
	{
		frame->names_live = true;
		for (size_t k = 0; k < frame->names.len; k++)
			if (!txn_runs_init_forked_name(frame->result, tablespace, frame->database,
										   &STD_VEC_AT(&frame->names, StdString, k), frame->error))
				return false;
		frame->names_live = false;
		txn_runs_names_drop(&frame->names);
	}
	frame->dir_live = false;
	std_vec_free_Checked(&frame->dir.bytes, 1, 1);
	return true;
}

/* txn_runs_init_body_0 is init_forked's body. */
static void
txn_runs_init_body_0(void *frame_)
{
	txn_runs_init_frame_0_t *frame = frame_;

	std_vec_new(frame->result, sizeof(HostStorage), _Alignof(HostStorage));
	frame->result_live = true;
	if (!txn_runs_init_tablespace(frame, HOST_PG_DEFAULT) ||
		!host_tablespaces(HOST_IO_IO_READ_DOORS(frame->io), &frame->tablespaces, frame->error))
	{
		txn_runs_init_pad_0(frame);
		return;
	}
	frame->tablespaces_live = true;
	for (size_t k = 0; k < frame->tablespaces.len; k++)
		if (!txn_runs_init_tablespace(frame, STD_VEC_AT(&frame->tablespaces, uint32, k)))
		{
			txn_runs_init_pad_0(frame);
			return;
		}
	frame->result_live = false;
	frame->ok = true;
	txn_runs_init_pad_0(frame);
}

/*
 * txn_runs_init_forked returns the storages of a database's unlogged
 * relations, read from the names of their init forks in each tablespace's
 * directory of the database's relation files, base for pg_default.
 */
static bool
txn_runs_init_forked(HostIoIo *io, uint32 database, StdVec *result, HostError *error)
{
	txn_runs_init_frame_0_t frame = {.io = io, .database = database, .result = result, .error = error};

	std_landing(&frame, txn_runs_init_body_0, txn_runs_init_pad_0);
	return frame.ok;
}

/* txn_runs_cleaned_repository resets each unlogged storage of one repository whose manifest holds a base record for it. */
static bool
txn_runs_cleaned_repository(txn_runs_repo_frame_0_t *frame, uint32 repository, uint32 own)
{
	bool		marked;

	if (!txn_state_marked(frame->io, repository, &marked, frame->error))
		return false;
	if (marked)
		return true;
	if (!txn_runs_repo_open(frame, repository, own) ||
		!txn_runs_init_forked(frame->io, repository, &frame->storages, frame->error))
		return false;
	frame->storages_live = true;
	for (size_t k = 0; k < frame->storages.len; k++)
	{
		const HostStorage *storage = &STD_VEC_AT(&frame->storages, HostStorage, k);
		RepoStorage at = {storage->tablespace, storage->relation};
		ChunksErrorError failed;
		bool		some;
		TxnBaseBase base;

		if (!repo_Repo_base(&frame->repo, frame->io, &frame->reading, &at, &some, &frame->read, &failed))
		{
			*frame->error = txn_base_storage(&failed);
			return false;
		}
		if (!some)
			continue;
		frame->read_live = true;
		if (!txn_base_Base_decode(frame->read.bytes.ptr, frame->read.bytes.len, (const chunks_TupleU32Locator_t *) frame->read.addresses.ptr,
								  frame->read.addresses.len, &base, frame->error) ||
			!txn_state_reset(frame->io, &frame->repo, storage, &base, frame->error))
			return false;
		frame->read_live = false;
		repo_BaseRecord_drop(&frame->read);
	}
	return txn_runs_repo_close(frame);
}

/* txn_runs_cleaned_body_0 is cleaned's body. */
static void
txn_runs_cleaned_body_0(void *frame_)
{
	txn_runs_repo_frame_0_t *frame = frame_;
	ChunksErrorError failed;

	if (!host_shared_ready())
	{
		frame->ok = true;
		return;
	}
	if (!txn_state_unlogged(frame->io, &frame->storages, frame->error))
		return;
	frame->storages_live = true;
	for (size_t k = 0; k < frame->storages.len; k++)
		if (!txn_state_unlinked(frame->io, &STD_VEC_AT(&frame->storages, HostStorage, k), frame->error))
		{
			txn_runs_repo_pad_0(frame);
			return;
		}
	frame->storages_live = false;
	std_vec_free_Checked(&frame->storages, sizeof(HostStorage), _Alignof(HostStorage));
	if (!chunks_packs_open_repositories(HOST_IO_IO_READ_DOORS(frame->io), &frame->repositories, &failed))
	{
		*frame->error = txn_base_storage(&failed);
		return;
	}
	frame->repositories_live = true;
	for (size_t k = 0; k < frame->repositories.len; k++)
	{
		const std_TupleU32U32_t *repository = &STD_VEC_AT(&frame->repositories, std_TupleU32U32_t, k);

		if (!txn_runs_cleaned_repository(frame, repository->f0, repository->f1))
		{
			txn_runs_repo_pad_0(frame);
			return;
		}
	}
	frame->ok = true;
	txn_runs_repo_pad_0(frame);
}

/*
 * txn_runs_cleaned empties every unlogged storage in a recovery's start: each
 * unlogged storage's state is unlinked, its memtables and tuple id table
 * retired with it, and each storage of an unlogged relation, found by its
 * init fork, whose repository's manifest holds a base record for it gets a
 * state made anew that holds an empty one. A repository the loader skipped
 * is left to its marker. A bootstrap process does nothing.
 */
bool
txn_runs_cleaned(HostIoIo *io, HostError *error)
{
	txn_runs_repo_frame_0_t frame = {.io = io, .error = error};

	std_landing(&frame, txn_runs_cleaned_body_0, txn_runs_repo_pad_0);
	return frame.ok;
}

/* txn_runs_gathering is checkpoint's gathering for its repository's save: the states' edits, kept for the marks, and the identity tree's folded pairs. */
static bool
txn_runs_gathering(void *captures, HostIoIo *io, const RepoRead *read, StdVec *result, ChunksErrorError *error)
{
	txn_runs_repo_frame_0_t *frame = captures;
	HostError	failed;

	if (frame->saving_live)
	{
		frame->saving_live = false;
		std_vec_free_Checked(&frame->saving.locations, sizeof(uint64), _Alignof(uint64));
	}
	if (!txn_state_gather(io, frame->database, read, &frame->edits, &frame->saving, &failed))
	{
		*error = txn_runs_host(failed);
		return false;
	}
	frame->edits_live = true;
	frame->saving_live = true;
	if (!txn_identity_folded(io, frame->database, frame->point, &frame->pairs, &failed))
	{
		*error = txn_runs_host(failed);
		return false;
	}
	frame->pairs_live = true;
	frame->moved_pairs = 0;
	if (!std_vec_try_reserve(&frame->edits, frame->pairs.len, sizeof(RepoGathered), _Alignof(RepoGathered)))
	{
		*error = txn_runs_host(host_Error_out_of_memory());
		return false;
	}
	while (frame->moved_pairs < frame->pairs.len)
	{
		TxnIdentityPair *pair = &STD_VEC_AT(&frame->pairs, TxnIdentityPair, frame->moved_pairs);
		RepoGathered edit = {.tag = REPO_GATHERED_PAIR};

		edit.u.pair.key = pair->f0;
		edit.u.pair.has_value = pair->has_f1;
		edit.u.pair.value = pair->f1;
		frame->moved_pairs = std_wrapping_add_usize(frame->moved_pairs, 1);
		std_vec_push_Checked(&frame->edits, &edit, sizeof(edit), _Alignof(RepoGathered));
	}
	frame->pairs_live = false;
	std_vec_free_Checked(&frame->pairs, sizeof(TxnIdentityPair), _Alignof(TxnIdentityPair));
	*result = frame->edits;
	frame->edits_live = false;
	return true;
}

/* txn_runs_marks is checkpoint's marks for its repository's save: the states its gathering stored, marked saved. */
static bool
txn_runs_marks(void *captures, HostIoIo *io, ChunksErrorError *error)
{
	txn_runs_repo_frame_0_t *frame = captures;
	HostError	failed;
	bool		saved;

	if (!frame->saving_live)
		return true;
	frame->saving_live = false;
	saved = txn_state_Saving_saved(&frame->saving, io, &failed);
	if (!saved)
		*error = txn_runs_host(failed);
	return saved;
}

static bool txn_runs_removed(HostIoIo *io, const RepoRepo *repo, uint32 repository, uint32 own,
							 TxnRunsRemoval at, HostError *error);

/* txn_runs_checkpoint_repository writes one repository's runs at the redo point and saves its bases with them. */
static bool
txn_runs_checkpoint_repository(txn_runs_repo_frame_0_t *frame, uint32 database, uint32 own)
{
	HostLsn		position;
	ChunksErrorError failed;

	frame->database = database;
	if (database == 0)
		return true;
	if (!txn_runs_repo_open(frame, database, own) ||
		!txn_state_states(frame->io, database, &frame->states, frame->error))
		return false;
	frame->states_live = true;
	frame->moved = 0;
	std_vec_new(&frame->chained, sizeof(uint64), _Alignof(uint64));
	frame->chained_live = true;
	while (frame->moved < frame->states.len)
	{
		frame->held = STD_VEC_AT(&frame->states, txn_TupleU64HeldState_t, frame->moved).f1;
		frame->held_live = true;
		frame->moved = std_wrapping_add_usize(frame->moved, 1);
		if (!txn_fold_settled(frame->io, &frame->repo, &frame->reading, &frame->held, &frame->settled, frame->error))
			return false;
		frame->settled_live = true;
		std_vec_extend_from_slice_Checked(&frame->chained, frame->settled.ptr, frame->settled.len, sizeof(uint64),
										  _Alignof(uint64));
		frame->settled_live = false;
		std_vec_free_Checked(&frame->settled, sizeof(uint64), _Alignof(uint64));
		frame->held_live = false;
		host_shared_Held_drop(&frame->held);
	}
	frame->states_live = false;
	std_vec_free_Checked(&frame->states, sizeof(txn_TupleU64HeldState_t), _Alignof(txn_TupleU64HeldState_t));
	if (!host_fold_point(HOST_IO_IO_IO_DOORS(frame->io), &position, frame->error) ||
		!txn_status_lowered(frame->io, position, &frame->point, frame->error) ||
		!txn_runs_write(frame->io, database, own, frame->redo, frame->point, frame->chained.ptr, frame->chained.len,
						&frame->runs, frame->error))
		return false;
	if (!repo_Repo_save(&frame->repo, frame->io, &frame->reading, true, &frame->runs, txn_runs_gathering, frame,
						txn_runs_marks, frame, &failed))
	{
		*frame->error = txn_base_storage(&failed);
		return false;
	}
	return txn_identity_dropped(frame->io, database, frame->point, frame->error) &&
		txn_runs_removed(frame->io, &frame->repo, database, own, TXN_RUNS_REMOVAL_CHECKPOINT, frame->error) &&
		txn_runs_repo_close(frame);
}

static bool txn_runs_saved(HostIoIo *io, StdVec *result, HostError *error);

/* txn_runs_checkpoint_body_0 is checkpoint's body. */
static void
txn_runs_checkpoint_body_0(void *frame_)
{
	txn_runs_repo_frame_0_t *frame = frame_;

	if (!host_shared_ready())
	{
		frame->ok = true;
		return;
	}
	if (!txn_runs_saved(frame->io, &frame->repositories, frame->error))
		return;
	frame->repositories_live = true;
	for (size_t k = 0; k < frame->repositories.len; k++)
	{
		const std_TupleU32U32_t *saved = &STD_VEC_AT(&frame->repositories, std_TupleU32U32_t, k);

		if (!txn_runs_checkpoint_repository(frame, saved->f0, saved->f1))
		{
			txn_runs_repo_pad_0(frame);
			return;
		}
	}
	frame->ok = true;
	txn_runs_repo_pad_0(frame);
}

/*
 * txn_runs_checkpoint is the save a checkpoint runs at its redo point: for
 * every repository with a filled state, it writes the repository's runs at
 * that point, then saves its unsaved bases with them, writing the manifest
 * whether or not a base moved, and then removes every run directory neither
 * generation of the manifest names. A bootstrap process does nothing.
 */
bool
txn_runs_checkpoint(HostIoIo *io, HostLsn redo, HostError *error)
{
	txn_runs_repo_frame_0_t frame = {.io = io, .redo = redo, .error = error};

	std_landing(&frame, txn_runs_checkpoint_body_0, txn_runs_repo_pad_0);
	return frame.ok;
}

/* txn_runs_saved_frame_0_t is saved's state across its pad: the repositories found, the filled ones, and the repositories on disk, while live. */
typedef struct txn_runs_saved_frame_0_t
{
	HostIoIo   *io;
	StdVec	   *result;
	HostError  *error;
	bool		ok;
	bool		result_live;
	/* filled is a Vec<u32>. */
	StdVec		filled;
	bool		filled_live;
	/* repositories is a Vec<(u32, u32)>. */
	StdVec		repositories;
	bool		repositories_live;
} txn_runs_saved_frame_0_t;

/* txn_runs_saved_pad_0 is saved's pad: the repositories on disk, the filled ones, and those found, while live. */
static void
txn_runs_saved_pad_0(void *frame_)
{
	txn_runs_saved_frame_0_t *frame = frame_;

	if (frame->repositories_live)
	{
		frame->repositories_live = false;
		std_vec_free_Checked(&frame->repositories, sizeof(std_TupleU32U32_t), _Alignof(std_TupleU32U32_t));
	}
	if (frame->filled_live)
	{
		frame->filled_live = false;
		std_vec_free_Checked(&frame->filled, sizeof(uint32), _Alignof(uint32));
	}
	if (frame->result_live)
	{
		frame->result_live = false;
		std_vec_free_Checked(frame->result, sizeof(std_TupleU32U32_t), _Alignof(std_TupleU32U32_t));
	}
}

/* txn_runs_saved_has reports whether a list of (u32, u32) repositories holds one. */
static bool
txn_runs_saved_has(const StdVec *found, uint32 repository)
{
	for (size_t k = 0; k < found->len; k++)
		if (STD_VEC_AT(found, std_TupleU32U32_t, k).f0 == repository)
			return true;
	return false;
}

/* txn_runs_saved_body_0 is saved's body. */
static void
txn_runs_saved_body_0(void *frame_)
{
	txn_runs_saved_frame_0_t *frame = frame_;
	ChunksErrorError failed;
	size_t		kept = 0;

	if (!txn_state_repositories(frame->io, frame->result, frame->error))
		return;
	frame->result_live = true;
	if (!txn_memtable_branch_filled(frame->io, &frame->filled, frame->error))
	{
		txn_runs_saved_pad_0(frame);
		return;
	}
	frame->filled_live = true;
	for (size_t k = 0; k < frame->filled.len; k++)
		if (!txn_runs_saved_has(frame->result, STD_VEC_AT(&frame->filled, uint32, k)))
			STD_VEC_AT(&frame->filled, uint32, kept++) = STD_VEC_AT(&frame->filled, uint32, k);
	frame->filled.len = kept;
	if (kept > 0)
	{
		if (!chunks_packs_open_repositories(HOST_IO_IO_READ_DOORS(frame->io), &frame->repositories, &failed))
		{
			*frame->error = txn_base_storage(&failed);
			txn_runs_saved_pad_0(frame);
			return;
		}
		frame->repositories_live = true;
		for (size_t k = 0; k < frame->repositories.len; k++)
		{
			const std_TupleU32U32_t *repository = &STD_VEC_AT(&frame->repositories, std_TupleU32U32_t, k);
			bool		filled = false;

			for (size_t j = 0; j < frame->filled.len && !filled; j++)
				filled = STD_VEC_AT(&frame->filled, uint32, j) == repository->f0;
			if (!filled)
				continue;
			if (!std_vec_try_reserve(frame->result, 1, sizeof(*repository), _Alignof(std_TupleU32U32_t)))
			{
				*frame->error = host_Error_out_of_memory();
				txn_runs_saved_pad_0(frame);
				return;
			}
			std_vec_push_Checked(frame->result, repository, sizeof(*repository), _Alignof(std_TupleU32U32_t));
		}
	}
	frame->result_live = false;
	frame->ok = true;
	txn_runs_saved_pad_0(frame);
}

/*
 * txn_runs_saved returns each repository a checkpoint saves, with its
 * tablespace: each of a storage's state, and each whose working state's
 * delta names a memtable, whose tablespace its directory gives.
 */
static bool
txn_runs_saved(HostIoIo *io, StdVec *result, HostError *error)
{
	txn_runs_saved_frame_0_t frame = {.io = io, .result = result, .error = error};

	std_landing(&frame, txn_runs_saved_body_0, txn_runs_saved_pad_0);
	return frame.ok;
}

/* txn_TuplePathBufU64_t is DEFERRED's (PathBuf, u64): a spill file and the epoch a checkpoint found it at. */
typedef struct txn_TuplePathBufU64_t
{
	StdPathBuf	f0;
	uint64		f1;
} txn_TuplePathBufU64_t;

/* txn_runs_TuplePathBufU64_drop is (PathBuf, u64)'s drop glue: its path's. */
static void
txn_runs_TuplePathBufU64_drop(void *elem)
{
	txn_TuplePathBufU64_t *self = elem;

	std_vec_free_Checked(&self->f0.bytes, 1, 1);
}

/* txn_runs_VecTuplePathBufU64_drop is Vec<(PathBuf, u64)>'s drop glue. */
static void
txn_runs_VecTuplePathBufU64_drop(void *value)
{
	std_vec_drop_Checked(value, sizeof(txn_TuplePathBufU64_t), _Alignof(txn_TuplePathBufU64_t),
						 txn_runs_TuplePathBufU64_drop);
}

/* taken at Vec<(PathBuf, u64)> and IoMutex<Vec<(PathBuf, u64)>>, which runs.c alone uses. */
#define HOST_LOCK_SCOPE static
#define HOST_LOCK_DECLARE
#define HOST_LOCK_DEFINE
#define HOST_LOCK_TAKEN_TS VecTuplePathBufU64
#define HOST_LOCK_TAKEN_T StdVec
#define HOST_LOCK_TAKEN_DEFAULT(place) \
	std_vec_new((place), sizeof(txn_TuplePathBufU64_t), _Alignof(txn_TuplePathBufU64_t))
#define HOST_LOCK_TAKEN_DROP(place) txn_runs_VecTuplePathBufU64_drop(place)
#define HOST_LOCK_TAKEN_TYPE TXN_RUNS_DEFERRED_LOCK_TYPE
#define HOST_LOCK_IO_MUTEX
#define HOST_LOCK_TS VecTuplePathBufU64
#define HOST_LOCK_T StdVec
#define HOST_LOCK_DEFAULT(place) \
	std_vec_new((place), sizeof(txn_TuplePathBufU64_t), _Alignof(txn_TuplePathBufU64_t))
#include "doltgres/host/lock_template.h"

/* txn_runs_IoMutexVecTuplePathBufU64_drop is IoMutex<Vec<(PathBuf, u64)>>' drop glue: its value's. */
static void
txn_runs_IoMutexVecTuplePathBufU64_drop(void *value)
{
	HostLockIoMutexVecTuplePathBufU64 *self = value;

	txn_runs_VecTuplePathBufU64_drop(&self->value);
}

/* TXN_RUNS_DEFERRED_TYPE is IoMutex<Vec<(PathBuf, u64)>>' size, Default, and drop. */
static const StdDefaultType TXN_RUNS_DEFERRED_TYPE = {sizeof(HostLockIoMutexVecTuplePathBufU64),
	_Alignof(HostLockIoMutexVecTuplePathBufU64), host_lock_IoMutexVecTuplePathBufU64_Default_default,
txn_runs_IoMutexVecTuplePathBufU64_drop};

/*
 * TXN_RUNS_DEFERRED is DEFERRED: the checkpointer's spill files that no
 * manifest names and whose transactions ended, each with the epoch a
 * checkpoint found it at, which a later checkpoint removes once every reader
 * that may have listed it ended; its BackendLocal header and the process's
 * value, which follows it.
 */
static struct
{
	HostLocalBackendLocal local;
	HostLockIoMutexVecTuplePathBufU64 process;
}			TXN_RUNS_DEFERRED = {HOST_LOCAL_BACKEND_LOCAL_NEW};

/* txn_runs_removed_frame_0_t is removed's state across its pad: the manifests, the directories and their names, the kept spills, and the deferred files' guard, while live. */
typedef struct txn_runs_removed_frame_0_t
{
	HostIoIo   *io;
	const RepoRepo *repo;
	uint32		repository;
	uint32		own;
	TxnRunsRemoval at;
	HostError  *error;
	bool		ok;
	StdArcInner *newest;
	bool		newest_live;
	RepoManifest previous;
	bool		previous_live;
	StdPathBuf	dir;
	bool		dir_live;
	/* names is a Vec<String> of a directory's names. */
	StdVec		names;
	bool		names_live;
	StdPathBuf	path;
	bool		path_live;
	/* kept is a Vec<String> of the spill files the manifests name. */
	StdVec		kept;
	bool		kept_live;
	HostLocalLocal deferred;
	bool		deferred_live;
	HostLockLockedRwLockWriteGuard guard;
	bool		guard_live;
} txn_runs_removed_frame_0_t;

/* txn_runs_removed_pad_0 is removed's pad: what it holds, while live. */
static void
txn_runs_removed_pad_0(void *frame_)
{
	txn_runs_removed_frame_0_t *frame = frame_;

	if (frame->guard_live)
	{
		frame->guard_live = false;
		host_lock_LockedRwLockWriteGuard_drop(&frame->guard);
	}
	if (frame->deferred_live)
	{
		frame->deferred_live = false;
		host_local_Local_drop(&frame->deferred, &TXN_RUNS_DEFERRED_TYPE);
	}
	if (frame->kept_live)
	{
		frame->kept_live = false;
		txn_runs_names_drop(&frame->kept);
	}
	if (frame->path_live)
	{
		frame->path_live = false;
		std_vec_free_Checked(&frame->path.bytes, 1, 1);
	}
	if (frame->names_live)
	{
		frame->names_live = false;
		txn_runs_names_drop(&frame->names);
	}
	if (frame->dir_live)
	{
		frame->dir_live = false;
		std_vec_free_Checked(&frame->dir.bytes, 1, 1);
	}
	if (frame->previous_live)
	{
		frame->previous_live = false;
		repo_Manifest_drop(&frame->previous);
	}
	if (frame->newest_live)
	{
		frame->newest_live = false;
		repo_ArcManifest_drop(frame->newest);
	}
}

/* txn_runs_removed_named returns the runs the manifests name, the newest's then the previous', answering how many. */
static size_t
txn_runs_removed_named(const txn_runs_removed_frame_0_t *frame, const RepoRuns *named[2])
{
	const RepoManifest *newest = repo_Repo_newest(frame->newest);
	size_t		len = 0;

	if (newest->has_runs)
		named[len++] = &newest->runs;
	if (frame->previous_live && frame->previous.has_runs)
		named[len++] = &frame->previous.runs;
	return len;
}

/* txn_runs_string_eq reports whether a String holds a name's bytes. */
static bool
txn_runs_string_eq(const StdString *string, const char *name, size_t len)
{
	return string->len == len && memcmp(string->ptr, name, len) == 0;
}

/* txn_runs_removed_runs removes every run directory in the repository's runs' directory that neither manifest names. */
static bool
txn_runs_removed_runs(txn_runs_removed_frame_0_t *frame, const RepoRuns *const *named, size_t named_len)
{
	bool		some;

	frame->dir = host_tablespace_dir(frame->repository, frame->own);
	frame->dir_live = true;
	txn_runs_joined(&frame->dir, CHUNKS_PACKS_DELTA, strlen(CHUNKS_PACKS_DELTA));
	if (!host_list_dir_if_there(HOST_IO_IO_READ_DOORS(frame->io), frame->dir.bytes.ptr, frame->dir.bytes.len, &some,
								&frame->names, frame->error))
		return false;
	frame->names_live = some;
	for (size_t k = 0; some && k < frame->names.len; k++)
	{
		const StdString *name = &STD_VEC_AT(&frame->names, StdString, k);
		bool		kept = txn_runs_string_eq(name, TXN_RUNS_SPILL, strlen(TXN_RUNS_SPILL));
		bool		removed;

		for (size_t j = 0; j < named_len && !kept; j++)
		{
			char		point[17];

			snprintf(point, sizeof(point), "%016" PRIx64, (uint64) named[j]->redo);
			kept = txn_runs_string_eq(name, point, 16);
		}
		if (kept)
			continue;
		std_path_join(frame->dir.bytes.ptr, frame->dir.bytes.len, (const char *) name->ptr, name->len, &frame->path);
		frame->path_live = true;
		if (!host_remove_tree(HOST_IO_IO_IO_DOORS(frame->io), frame->path.bytes.ptr, frame->path.bytes.len, &removed,
							  frame->error))
			return false;
		frame->path_live = false;
		std_vec_free_Checked(&frame->path.bytes, 1, 1);
	}
	if (frame->names_live)
	{
		frame->names_live = false;
		txn_runs_names_drop(&frame->names);
	}
	frame->dir_live = false;
	std_vec_free_Checked(&frame->dir.bytes, 1, 1);
	return true;
}

/* txn_runs_removed_kept gathers the names of the spill files the manifests name into the frame's kept. */
static bool
txn_runs_removed_kept(txn_runs_removed_frame_0_t *frame, const RepoRuns *const *named, size_t named_len)
{
	std_vec_new(&frame->kept, sizeof(StdString), _Alignof(StdString));
	frame->kept_live = true;
	for (size_t j = 0; j < named_len; j++)
		for (size_t k = 0; k < named[j]->files.len; k++)
		{
			const RepoRunFile *file = &STD_VEC_AT(&named[j]->files, RepoRunFile, k);
			TxnRunsNames names = {.persistence = TXN_STATE_PERSISTENCE_PERMANENT, .tablespace = file->tablespace,
			.database = frame->repository, .relation = file->relation};
			StdString	name;

			if (!file->has_spilled || !txn_runs_Kind_of(file->kind, &names.kind))
				continue;
			names.redo = file->spilled.xid;
			if (!std_vec_try_reserve(&frame->kept, 1, sizeof(StdString), _Alignof(StdString)))
			{
				*frame->error = host_Error_out_of_memory();
				return false;
			}
			txn_runs_spill_name(&names, file->spilled.number, &name);
			std_vec_push_Checked(&frame->kept, &name, sizeof(name), _Alignof(StdString));
		}
	return true;
}

/* txn_runs_removed_xid returns the xid a spill file's name holds, chunks' third word of three or a tree's fourth of five. */
static bool
txn_runs_removed_xid(const StdString *name, uint64 *xid)
{
	const char *bytes = (const char *) name->ptr;
	size_t		starts[6];
	size_t		words = 1;
	StdParseIntError failed;

	starts[0] = 0;
	for (size_t k = 0; k < name->len; k++)
		if (bytes[k] == '-')
		{
			if (words == 5)
				return false;
			starts[words++] = std_wrapping_add_usize(k, 1);
		}
	starts[words] = std_wrapping_add_usize(name->len, 1);
	if (words == 3 && starts[1] - 1 == strlen("chunks") && memcmp(bytes, "chunks", strlen("chunks")) == 0)
		return std_str_parse_u64(bytes + starts[1], starts[2] - starts[1] - 1, xid, &failed);
	if (words == 5)
		return std_str_parse_u64(bytes + starts[3], starts[4] - starts[3] - 1, xid, &failed);
	return false;
}

/* txn_runs_removed_deferred reports whether a checkpoint removes a spill file of an ended transaction: one it deferred whose epoch every reader passed, deferring one it didn't. */
static bool
txn_runs_removed_deferred(txn_runs_removed_frame_0_t *frame, uint64 passed, uint64 oldest, bool *removable)
{
	StdVec	   *deferred;
	size_t		at;

	frame->deferred = host_local_BackendLocal_get(&TXN_RUNS_DEFERRED.local, &TXN_RUNS_DEFERRED_TYPE);
	frame->deferred_live = true;
	frame->guard = host_lock_IoMutex_lock((HostLockIoMutex *) host_local_Local_Deref_deref(&frame->deferred,
																						   &TXN_RUNS_DEFERRED_TYPE),
										  &TXN_RUNS_DEFERRED_LOCK_TYPE, &frame->io->held);
	frame->guard_live = true;
	deferred = host_lock_LockedRwLockWriteGuard_DerefMut_deref_mut(&frame->guard);
	for (at = 0; at < deferred->len; at++)
	{
		const StdPathBuf *listed = &STD_VEC_AT(deferred, txn_TuplePathBufU64_t, at).f0;

		if (std_path_PartialEq_eq(listed->bytes.ptr, listed->bytes.len, frame->path.bytes.ptr, frame->path.bytes.len))
			break;
	}
	*removable = false;
	if (at < deferred->len)
	{
		if (host_delta_page_passed(STD_VEC_AT(deferred, txn_TuplePathBufU64_t, at).f1, oldest))
		{
			txn_TuplePathBufU64_t removed;

			std_vec_swap_remove(deferred, at, &removed, sizeof(removed), _Alignof(txn_TuplePathBufU64_t));
			std_vec_free_Checked(&removed.f0.bytes, 1, 1);
			*removable = true;
		}
	}
	else
	{
		txn_TuplePathBufU64_t listed = {.f1 = passed};

		if (!std_vec_try_reserve(deferred, 1, sizeof(listed), _Alignof(txn_TuplePathBufU64_t)))
		{
			*frame->error = host_Error_out_of_memory();
			return false;
		}
		std_PathBuf_Clone_clone(&frame->path, &listed.f0);
		std_vec_push_Checked(deferred, &listed, sizeof(listed), _Alignof(txn_TuplePathBufU64_t));
	}
	frame->guard_live = false;
	host_lock_LockedRwLockWriteGuard_drop(&frame->guard);
	frame->deferred_live = false;
	host_local_Local_drop(&frame->deferred, &TXN_RUNS_DEFERRED_TYPE);
	return true;
}

/* txn_runs_removed_spills removes every spill file in the repository's spills' directory that the removal's time lets go. */
static bool
txn_runs_removed_spills(txn_runs_removed_frame_0_t *frame)
{
	bool		some;
	bool		advanced = false;
	uint64		passed = 0;
	uint64		oldest = 0;

	txn_runs_spill_directory(frame->repository, frame->own, &frame->dir);
	frame->dir_live = true;
	if (!host_list_dir_if_there(HOST_IO_IO_READ_DOORS(frame->io), frame->dir.bytes.ptr, frame->dir.bytes.len, &some,
								&frame->names, frame->error))
		return false;
	frame->names_live = some;
	for (size_t k = 0; some && k < frame->names.len; k++)
	{
		const StdString *name = &STD_VEC_AT(&frame->names, StdString, k);
		bool		listing = name->len >= strlen(TXN_SPILL_MANIFEST) &&
			memcmp(name->ptr, TXN_SPILL_MANIFEST, strlen(TXN_SPILL_MANIFEST)) == 0 &&
			frame->at == TXN_RUNS_REMOVAL_CHECKPOINT;
		bool		kept = false;
		bool		running = false;
		bool		removable = true;
		uint64		xid;

		for (size_t j = 0; j < frame->kept.len && !kept; j++)
		{
			const StdString *spilled = &STD_VEC_AT(&frame->kept, StdString, j);

			kept = txn_runs_string_eq(name, (const char *) spilled->ptr, spilled->len);
		}
		if (kept || listing)
			continue;
		if (frame->at == TXN_RUNS_REMOVAL_CHECKPOINT && txn_runs_removed_xid(name, &xid))
		{
			HostXid		asked = {xid};

			if (!host_in_progress(HOST_IO_IO_IO_DOORS(frame->io), asked, &running, frame->error))
				return false;
		}
		std_path_join(frame->dir.bytes.ptr, frame->dir.bytes.len, (const char *) name->ptr, name->len, &frame->path);
		frame->path_live = true;
		if (frame->at == TXN_RUNS_REMOVAL_CHECKPOINT)
		{
			if (running)
				removable = false;
			else
			{
				if (!advanced)
				{
					host_delta_advanced(&passed, &oldest);
					advanced = true;
				}
				if (!txn_runs_removed_deferred(frame, passed, oldest, &removable))
					return false;
			}
		}
		if (removable)
		{
			ChunksErrorError failed;
			bool		removed;

			if (!chunks_file_remove_if_there(HOST_IO_IO_IO_DOORS(frame->io), frame->path.bytes.ptr,
											 frame->path.bytes.len, &removed, &failed))
			{
				*frame->error = txn_base_storage(&failed);
				return false;
			}
		}
		frame->path_live = false;
		std_vec_free_Checked(&frame->path.bytes, 1, 1);
	}
	return true;
}

/* txn_runs_removed_body_0 is removed's body. */
static void
txn_runs_removed_body_0(void *frame_)
{
	txn_runs_removed_frame_0_t *frame = frame_;
	ChunksErrorError failed;
	const RepoRuns *named[2];
	size_t		named_len;
	bool		has;

	if (!repo_Repo_manifest(frame->repo, frame->io, &frame->newest, &failed))
	{
		*frame->error = txn_base_storage(&failed);
		return;
	}
	frame->newest_live = true;
	if (!repo_Repo_previous(frame->repo, frame->io, &has, &frame->previous, &failed))
	{
		*frame->error = txn_base_storage(&failed);
		txn_runs_removed_pad_0(frame);
		return;
	}
	frame->previous_live = has;
	named_len = txn_runs_removed_named(frame, named);
	frame->ok = txn_runs_removed_runs(frame, named, named_len) && txn_runs_removed_kept(frame, named, named_len) &&
		txn_runs_removed_spills(frame);
	txn_runs_removed_pad_0(frame);
}

/*
 * txn_runs_removed removes every directory of a repository's runs that
 * neither generation of its manifest names, as a checkpoint does after its
 * manifest is written and a startup does before it loads, and every spill
 * file neither generation names, but, after a checkpoint, one whose
 * transaction runs or is prepared, and the storages' spills' manifests,
 * which only a start removes.
 */
static bool
txn_runs_removed(HostIoIo *io, const RepoRepo *repo, uint32 repository, uint32 own, TxnRunsRemoval at,
				 HostError *error)
{
	txn_runs_removed_frame_0_t frame = {.io = io, .repo = repo, .repository = repository, .own = own, .at = at,
	.error = error};

	std_landing(&frame, txn_runs_removed_body_0, txn_runs_removed_pad_0);
	return frame.ok;
}

/* TxnRunsLoading is Loading: the storage whose runs the loader is pushing, its state, its free space run's cut, the tuple id past every row version it pushed, and whether it linked a spill. */
typedef struct TxnRunsLoading
{
	HostStorage storage;
	HostSharedHeld held;
	bool		has_cut;
	uint64		cut;
	uint64		past;
	bool		spills;
} TxnRunsLoading;

/* txn_runs_ended ends a storage's load: a storage with no free space run moves its tuple id counter past every row version it pushed. */
static bool
txn_runs_ended(HostIoIo *io, TreesStoreNodeStore store, const TxnInsertPage *page, const TxnRunsLoading *loading,
			   HostError *error)
{
	if (loading == NULL || loading->has_cut)
		return true;
	return txn_insert_loaded(io, store, &loading->held, page, loading->past, error);
}

/* txn_runs_counted checks that a free space run's counter is a tuple id's form, refusing any other as data corruption. */
static bool
txn_runs_counted(uint64 counter, const TxnInsertPage *page, uint64 *result, HostError *error)
{
	uint64		offset = counter & 0xffff;

	if (offset < 1 || offset > (uint64) page->most + 1 || counter >> 16 > PG_UINT32_MAX)
	{
		*error = txn_runs_corrupt();
		return false;
	}
	*result = counter;
	return true;
}

/* txn_runs_tid returns the tuple id a row version holds, the row a put, an add, an added row, or an update names. */
static bool
txn_runs_tid(const TxnMemtableVersionOpRef *op, HostRow *result)
{
	switch (op->tag)
	{
		case TXN_MEMTABLE_VERSION_OP_REF_PUT:
			*result = op->u.put.tuple;
			return true;
		case TXN_MEMTABLE_VERSION_OP_REF_ADD:
			*result = op->u.add.tuple;
			return true;
		case TXN_MEMTABLE_VERSION_OP_REF_ADD_ROW:
			*result = op->u.add_row.tuple;
			return true;
		case TXN_MEMTABLE_VERSION_OP_REF_UPDATE_ROW:
			*result = op->u.update_row.f1.tuple;
			return true;
		default:
			return false;
	}
}

/* txn_runs_map_counted counts a map's or a database-level tree's version read back from a run at its writer's entry. */
static bool
txn_runs_map_counted(HostIoIo *io, const TxnMemtableVersionRecord *version, HostError *error)
{
	TxnMemtableVersionWriter writer = txn_memtable_version_Record_writer(version);
	TxnMemtableVersionWriter none = TXN_MEMTABLE_VERSION_NO_TRANSACTION;
	HostXid		top = {writer.has_top ? writer.top : writer.xid};
	bool		has;
	uint64		added;

	if (txn_memtable_version_Writer_PartialEq_eq(&writer, &none))
		return true;
	return txn_status_add(io, top, 1, &has, &added, error);
}

/* txn_runs_pushed_back_frame_0_t is pushed_back's state across its pad: the version's op, owned, while live. */
typedef struct txn_runs_pushed_back_frame_0_t
{
	const TxnMemtableTableTreeOps *tree;
	HostIoIo   *io;
	HostSharedRecord owner;
	StdBytes	key;
	const TxnMemtableVersionRecord *version;
	HostError  *error;
	bool		ok;
	TxnMemtableOp op;
	bool		op_live;
} txn_runs_pushed_back_frame_0_t;

/* txn_runs_pushed_back_pad_0 is pushed_back's pad: the op, while live. */
static void
txn_runs_pushed_back_pad_0(void *frame_)
{
	txn_runs_pushed_back_frame_0_t *frame = frame_;

	if (frame->op_live)
	{
		frame->op_live = false;
		txn_memtable_Op_drop(&frame->op);
	}
}

/* txn_runs_pushed_back_body_0 is pushed_back's body. */
static void
txn_runs_pushed_back_body_0(void *frame_)
{
	txn_runs_pushed_back_frame_0_t *frame = frame_;
	TxnMemtableVersionOpRef op = txn_memtable_version_Record_op(frame->version);
	TxnMemtableVersionChange change = {.writer = txn_memtable_version_Record_writer(frame->version),
		.op = &frame->op, .lock = txn_memtable_version_Record_lock(frame->version),
	.shown = txn_memtable_version_Record_shown(frame->version)};
	TxnMemtableVersionHandle handle;

	frame->op = txn_memtable_version_OpRef_owned(&op);
	frame->op_live = true;
	change.has_lsn = txn_memtable_version_Record_lsn(frame->version, &change.lsn);
	frame->ok = txn_memtable_table_alone(frame->tree, frame->io, frame->owner, frame->key.ptr, frame->key.len, &change,
										 &handle, frame->error);
	txn_runs_pushed_back_pad_0(frame);
}

/*
 * txn_runs_pushed_back pushes a version read back from a run into one of an
 * owner's trees with no window, its writer, op, lock, and LSN as the run
 * holds them.
 */
static bool
txn_runs_pushed_back(const TxnMemtableTableTreeOps *tree, HostIoIo *io, HostSharedRecord owner, StdBytes key,
					 const TxnMemtableVersionRecord *version, HostError *error)
{
	txn_runs_pushed_back_frame_0_t frame = {.tree = tree, .io = io, .owner = owner, .key = key, .version = version,
	.error = error};

	std_landing(&frame, txn_runs_pushed_back_body_0, txn_runs_pushed_back_pad_0);
	return frame.ok;
}

/* txn_runs_same_spill reports whether two files a manifest names are of the same storage, transaction, and number. */
static bool
txn_runs_same_spill(const RepoRunFile *file, const RepoRunFile *other)
{
	return file->tablespace == other->tablespace && file->relation == other->relation &&
		file->has_spilled == other->has_spilled &&
		(!file->has_spilled ||
		 (file->spilled.xid == other->spilled.xid && file->spilled.number == other->spilled.number));
}

/* txn_runs_tuple_ids returns the tuple-id file a manifest names for a row spill file, or none for a row file that has none. */
static const RepoRunFile *
txn_runs_tuple_ids(const StdVec *files, const RepoRunFile *rows)
{
	for (size_t k = 0; k < files->len; k++)
	{
		const RepoRunFile *file = &STD_VEC_AT(files, RepoRunFile, k);

		if (file->kind == TXN_RUNS_KIND_SPILLED_TUPLES && txn_runs_same_spill(file, rows))
			return file;
	}
	return NULL;
}

/* txn_runs_tupled_rows returns the row spill file a manifest names for a tuple-id file, or none for one no row file names. */
static const RepoRunFile *
txn_runs_tupled_rows(const StdVec *files, const RepoRunFile *tuples)
{
	for (size_t k = 0; k < files->len; k++)
	{
		const RepoRunFile *file = &STD_VEC_AT(files, RepoRunFile, k);

		if (file->kind == TXN_RUNS_KIND_SPILLED_ROWS && txn_runs_same_spill(file, tuples))
			return file;
	}
	return NULL;
}

/* txn_TupleBoolU32U32BoolBoolU8_t is pushed_runs' sort key of a run file: status files first, then by storage, free space first, spills last, and by kind. */
typedef struct txn_TupleBoolU32U32BoolBoolU8_t
{
	bool		f0;
	uint32		f1;
	uint32		f2;
	bool		f3;
	bool		f4;
	uint8		f5;
} txn_TupleBoolU32U32BoolBoolU8_t;

/* txn_runs_TupleBoolU32U32BoolBoolU8_lt is the sort key's PartialOrd::lt, field by field. */
static bool
txn_runs_TupleBoolU32U32BoolBoolU8_lt(const txn_TupleBoolU32U32BoolBoolU8_t *a,
									  const txn_TupleBoolU32U32BoolBoolU8_t *b)
{
	if (a->f0 != b->f0)
		return a->f0 < b->f0;
	if (a->f1 != b->f1)
		return a->f1 < b->f1;
	if (a->f2 != b->f2)
		return a->f2 < b->f2;
	if (a->f3 != b->f3)
		return a->f3 < b->f3;
	if (a->f4 != b->f4)
		return a->f4 < b->f4;
	return a->f5 < b->f5;
}

/* The sort of a manifest's run files: core's shared sort parts at RunFile, which its stable sort calls. */
#define STD_SORT_SCOPE static
#define STD_SORT_DECLARE
#define STD_SORT_DEFINE
#define STD_SORT_SHARED_ONLY
#define STD_SORT_WITHOUT_UNSTABLE
#define STD_SORT_TS RunFile
#define STD_SORT_T RepoRunFile
#define STD_SORT_FREEZE 1
#include "doltgres/std/sort_template.h"

/* The stable sort_by_key of a manifest's run files by the order the loader pushes them in. */
#define STD_STABLE_SORT_SCOPE static
#define STD_STABLE_SORT_DECLARE
#define STD_STABLE_SORT_DEFINE
#define STD_STABLE_SORT_TS RunFile
#define STD_STABLE_SORT_T RepoRunFile
#define STD_STABLE_SORT_FREEZE 1
#define STD_STABLE_SORT_WITHOUT_SORT
#define STD_STABLE_SORT_WITH_BY_KEY
#define STD_STABLE_SORT_KEY_KS TupleBoolU32U32BoolBoolU8
#define STD_STABLE_SORT_KEY_K txn_TupleBoolU32U32BoolBoolU8_t
#define STD_STABLE_SORT_KEY_LT(a, b) txn_runs_TupleBoolU32U32BoolBoolU8_lt((a), (b))
#include "doltgres/std/stable_sort_template.h"

/* txn_runs_pushed_runs_closure_0 is pushed_runs' sort key of a run file. */
static txn_TupleBoolU32U32BoolBoolU8_t
txn_runs_pushed_runs_closure_0(void *captures, const void *elem)
{
	const RepoRunFile *file = elem;
	TxnRunsKind kind;
	bool		spilled = txn_runs_Kind_of(file->kind, &kind) && txn_runs_Kind_spilled(kind);
	txn_TupleBoolU32U32BoolBoolU8_t key = {file->kind != TXN_RUNS_KIND_STATUS, file->tablespace, file->relation,
	file->kind != TXN_RUNS_KIND_FREE, !spilled, file->kind};

	return key;
}

/* txn_runs_pushed_runs_frame_0_t is pushed_runs' state across its pad: the files' paths and bytes, their decodings, the storage loading, and a working state's delta, while live. */
typedef struct txn_runs_pushed_runs_frame_0_t
{
	HostIoIo   *io;
	const TxnInsertPage *page;
	const RepoRepo *repo;
	const HostSharedReader *reading;
	uint32		repository;
	uint32		own;
	RepoRuns   *runs;
	HostError  *error;
	bool		ok;
	TreesStoreReader store;
	StdPathBuf	dir;
	bool		dir_live;
	StdPathBuf	path;
	bool		path_live;
	StdString	name;
	bool		name_live;
	StdVec		bytes;
	bool		bytes_live;
	StdVec		tuple_bytes;
	bool		tuple_bytes_live;

	/*
	 * rows is a Vec<(u64, &[u8])> of the tuple-id file's rows, or a Vec<(u64,
	 * Outcome, u64)> of a status run's entries.
	 */
	StdVec		rows;
	bool		rows_live;
	size_t		rows_size;
	size_t		rows_align;
	TxnRunsDecoded decoded;
	bool		decoded_live;
	HostSharedHeld deltas;
	bool		deltas_live;
	TxnRunsLoading loading;
	bool		loading_live;
	TxnMemtableOp op;
	bool		op_live;
} txn_runs_pushed_runs_frame_0_t;

/* txn_runs_pushed_runs_step_drop drops what a file's step holds, while live. */
static void
txn_runs_pushed_runs_step_drop(txn_runs_pushed_runs_frame_0_t *frame)
{
	if (frame->op_live)
	{
		frame->op_live = false;
		txn_memtable_Op_drop(&frame->op);
	}
	if (frame->deltas_live)
	{
		frame->deltas_live = false;
		host_shared_Held_drop(&frame->deltas);
	}
	if (frame->decoded_live)
	{
		frame->decoded_live = false;
		txn_runs_Decoded_drop(&frame->decoded);
	}
	if (frame->rows_live)
	{
		frame->rows_live = false;
		std_vec_free_Checked(&frame->rows, frame->rows_size, frame->rows_align);
	}
	if (frame->tuple_bytes_live)
	{
		frame->tuple_bytes_live = false;
		std_vec_free_Checked(&frame->tuple_bytes, 1, 1);
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

/* txn_runs_pushed_runs_pad_0 is pushed_runs' pad: a file's step's holdings, the storage loading, and the directory, while live. */
static void
txn_runs_pushed_runs_pad_0(void *frame_)
{
	txn_runs_pushed_runs_frame_0_t *frame = frame_;

	txn_runs_pushed_runs_step_drop(frame);
	if (frame->loading_live)
	{
		frame->loading_live = false;
		host_shared_Held_drop(&frame->loading.held);
	}
	if (frame->dir_live)
	{
		frame->dir_live = false;
		std_vec_free_Checked(&frame->dir.bytes, 1, 1);
	}
}

/* txn_runs_read_whole reads a run's file whole into a buffer, refusing one whose length isn't its entry's as data corruption. */
static bool
txn_runs_read_whole(HostIoIo *io, const StdPathBuf *path, uint64 len, StdVec *result, bool *live, HostError *error)
{
	ChunksErrorError failed;

	if (!chunks_file_read_file(HOST_IO_IO_READ_DOORS(io), path->bytes.ptr, path->bytes.len, result, &failed))
	{
		*error = txn_base_storage(&failed);
		return false;
	}
	*live = true;
	if ((uint64) result->len != len)
	{
		*error = txn_runs_corrupt();
		return false;
	}
	return true;
}

/* txn_runs_spilled_path sets the frame's path to a spill's file in the repository's spills' directory, dropping the one before. */
static void
txn_runs_spilled_path(txn_runs_pushed_runs_frame_0_t *frame, const TxnRunsNames *names, uint32 number)
{
	if (frame->path_live)
	{
		frame->path_live = false;
		std_vec_free_Checked(&frame->path.bytes, 1, 1);
	}
	txn_runs_spill_name(names, number, &frame->name);
	frame->name_live = true;
	txn_runs_spill_directory(frame->repository, frame->own, &frame->path);
	frame->path_live = true;
	txn_runs_joined(&frame->path, (const char *) frame->name.ptr, frame->name.len);
	frame->name_live = false;
	std_vec_free_Checked(&frame->name, 1, 1);
}

/* txn_runs_pushed_status takes a status run's entries into the commit status table. */
static bool
txn_runs_pushed_status(txn_runs_pushed_runs_frame_0_t *frame, const TxnRunsNames *names, const RepoRunFile *file)
{
	if (!txn_runs_decode_status(frame->bytes.ptr, frame->bytes.len, names, file->hash, &frame->rows, frame->error))
		return false;
	frame->rows_size = sizeof(txn_TupleU64OutcomeU64_t);
	frame->rows_align = _Alignof(txn_TupleU64OutcomeU64_t);
	frame->rows_live = true;
	for (size_t k = 0; k < frame->rows.len; k++)
	{
		const txn_TupleU64OutcomeU64_t *entry = &STD_VEC_AT(&frame->rows, txn_TupleU64OutcomeU64_t, k);
		HostXid		xid = {entry->f0};
		TxnStatusRoomed roomed;
		bool		taken;
		bool		has;
		uint64		added;

		if (!txn_status_take(frame->io, xid, entry->f1, &roomed, &taken, frame->error) ||
			(entry->f2 != 0 && !txn_status_add(frame->io, xid, entry->f2, &has, &added, frame->error)))
			return false;
	}
	return true;
}

/* txn_runs_pushed_branch pushes a working state's chunks or identity run, or links its chunk spill. */
static bool
txn_runs_pushed_branch(txn_runs_pushed_runs_frame_0_t *frame, TxnRunsKind kind, const RepoRunFile *file)
{
	HostSharedRecord record;

	if (!txn_memtable_branch_deltas(frame->io, frame->repository, frame->own, &frame->deltas, frame->error))
		return false;
	frame->deltas_live = true;
	record = host_shared_Held_record(&frame->deltas);
	if (kind == TXN_RUNS_KIND_SPILLED_CHUNKS)
	{
		TxnSpillFile listing = {.xid = file->spilled.xid, .kind = kind, .number = file->spilled.number,
		.len = file->len, .hash = file->hash};
		HostStorage storage = txn_spill_chunks_storage(frame->repository);
		txn_TupleU64KindU32OptionU32_t none = {0};

		return txn_runs_spill_bits(frame->bytes.ptr, frame->bytes.len, &listing.bits, frame->error) &&
			txn_spill_linked(&TXN_SPILL_BRANCH_DELTAS_HOME_OPS, frame->io, record, &storage, &listing, 1, false, none,
							 frame->error);
	}
	for (size_t k = 0; k < frame->decoded.keys.len; k++)
	{
		const TxnRunsKey *key = &STD_VEC_AT(&frame->decoded.keys, TxnRunsKey, k);

		for (size_t v = key->versions.len; v > 0; v--)
		{
			const TxnMemtableVersionRecord *version = &STD_VEC_AT(&key->versions, TxnMemtableVersionRecord, v - 1);

			if (!txn_runs_pushed_back(kind == TXN_RUNS_KIND_CHUNKS ? &TXN_MEMTABLE_BRANCH_CHUNKS_TREE_OPS :
									  &TXN_MEMTABLE_BRANCH_IDENTITY_TREE_OPS, frame->io, record, key->key, version,
									  frame->error) ||
				(kind == TXN_RUNS_KIND_IDENTITY && !txn_runs_map_counted(frame->io, version, frame->error)))
				return false;
		}
	}
	return true;
}

/* txn_runs_pushed_loading makes the frame's storage loading the file's storage's, ending the one before. */
static bool
txn_runs_pushed_loading(txn_runs_pushed_runs_frame_0_t *frame, const HostStorage *storage)
{
	TxnStateMade made = {.key = {.tag = TXN_BASE_KEY_NONE}};
	bool		has_made = frame->decoded.tail.tag == TXN_RUNS_TAIL_INDEX;

	if (frame->loading_live)
	{
		if (host_Storage_PartialEq_eq(&frame->loading.storage, storage))
			return true;
		if (!txn_runs_ended(frame->io, TREES_STORE_READER_NODE_STORE(&frame->store), frame->page, &frame->loading,
							frame->error))
			return false;
		frame->loading_live = false;
		host_shared_Held_drop(&frame->loading.held);
	}
	if (has_made)
	{
		made.table = frame->decoded.tail.table;
		made.has_index = true;
		made.index = frame->decoded.tail.index;
	}
	if (!txn_state_state(frame->io, frame->repo, frame->reading, storage, frame->decoded.persistence, has_made, &made,
						 &frame->loading.held, frame->error))
		return false;
	txn_state_totals_unknown(host_shared_Held_record(&frame->loading.held));
	frame->loading.storage = *storage;
	frame->loading.has_cut = false;
	frame->loading.past = 0;
	frame->loading.spills = false;
	frame->loading_live = true;
	return true;
}

/* txn_runs_pushed_spill links a storage's spill file, with its tuple-id file, and counts its versions at its transaction's entry. */
static bool
txn_runs_pushed_spill(txn_runs_pushed_runs_frame_0_t *frame, TxnRunsKind kind, const TxnRunsNames *names,
					  const RepoRunFile *file)
{
	TxnSpillFile listing[2] = {{.xid = file->spilled.xid, .kind = kind, .number = file->spilled.number,
	.len = file->len, .hash = file->hash}};
	size_t		listing_len = 1;
	const RepoRunFile *tuples = txn_runs_tuple_ids(&frame->runs->files, file);
	txn_TupleU64KindU32OptionU32_t none = {0};

	for (size_t k = 0; kind == TXN_RUNS_KIND_SPILLED_ROWS && k < frame->decoded.keys.len; k++)
	{
		const TxnRunsKey *key = &STD_VEC_AT(&frame->decoded.keys, TxnRunsKey, k);

		for (size_t v = 0; v < key->versions.len; v++)
		{
			TxnMemtableVersionOpRef op = txn_memtable_version_Record_op(&STD_VEC_AT(&key->versions,
																					TxnMemtableVersionRecord, v));
			HostRow		tuple;

			if (txn_runs_tid(&op, &tuple))
				frame->loading.past = Max(frame->loading.past, STD_ADD(u64, tuple.f0, 1, 1591, 53));
		}
	}
	if (!txn_runs_spill_bits(frame->bytes.ptr, frame->bytes.len, &listing[0].bits, frame->error))
		return false;
	if (tuples != NULL)
	{
		TxnRunsNames tupled = *names;

		tupled.kind = TXN_RUNS_KIND_SPILLED_TUPLES;
		txn_runs_spilled_path(frame, &tupled, file->spilled.number);
		if (!txn_runs_read_whole(frame->io, &frame->path, tuples->len, &frame->tuple_bytes, &frame->tuple_bytes_live,
								 frame->error) ||
			!txn_runs_decode_tuples(frame->tuple_bytes.ptr, frame->tuple_bytes.len, &tupled, tuples->hash,
									&frame->rows, frame->error))
			return false;
		frame->rows_live = false;
		std_vec_free_Checked(&frame->rows, sizeof(txn_TupleU64RefSliceU8_t), _Alignof(txn_TupleU64RefSliceU8_t));
		listing[1].xid = file->spilled.xid;
		listing[1].kind = TXN_RUNS_KIND_SPILLED_TUPLES;
		listing[1].number = file->spilled.number;
		listing[1].len = tuples->len;
		listing[1].hash = tuples->hash;
		if (!txn_runs_spill_bits(frame->tuple_bytes.ptr, frame->tuple_bytes.len, &listing[1].bits, frame->error))
			return false;
		listing_len = 2;
	}
	if (!txn_spill_linked(&TXN_SPILL_STATE_HOME_OPS, frame->io, host_shared_Held_record(&frame->loading.held),
						  &frame->loading.storage, listing, listing_len, false, none, frame->error))
		return false;
	if (kind == TXN_RUNS_KIND_SPILLED_ROWS || kind == TXN_RUNS_KIND_SPILLED_ENTRIES ||
		kind == TXN_RUNS_KIND_SPILLED_VALUES)
	{
		HostXid		xid = {file->spilled.xid};
		bool		has;
		uint64		added;

		if (!txn_status_add(frame->io, xid, txn_spill_counted(&frame->decoded), &has, &added, frame->error))
			return false;
	}
	frame->loading.spills = true;
	return true;
}

/* txn_runs_pushed_row_closure_captures_t is the captures of a loaded row's prepare's closure: its tuple id and its change. */
typedef struct txn_runs_pushed_row_closure_captures_t
{
	bool		has_tuple;
	HostRow		tuple;
	const TxnMemtableVersionChange *change;
	bool		has_roomed;
	TxnStatusRoomed roomed;
} txn_runs_pushed_row_closure_captures_t;

/* txn_runs_pushed_row_closure_0 is a loaded row's prepare's closure: its tuple id's slot, then its version. */
static bool
txn_runs_pushed_row_closure_0(void *captures, HostIoIo *io, TxnMemtableTablePreparedState *prepared,
							  HostError *error)
{
	txn_runs_pushed_row_closure_captures_t *c = captures;
	TxnMemtableVersionHandle handle;

	if (c->has_tuple && !txn_memtable_tuples_finish(HOST_IO_IO_TAKES_SHORT(io), prepared, c->tuple, error))
		return false;
	return txn_memtable_table_finish(HOST_IO_IO_TAKES_SHORT(io), prepared, c->change, c->has_roomed, c->roomed,
									 &handle, error);
}

/* txn_runs_pushed_row pushes a row's or an index entry's version read back from a run through a prepare, with its tuple id's slot. */
static bool
txn_runs_pushed_row(txn_runs_pushed_runs_frame_0_t *frame, TxnRunsKind kind, StdBytes key,
					const TxnMemtableVersionRecord *version)
{
	TxnMemtableVersionOpRef op = txn_memtable_version_Record_op(version);
	txn_runs_pushed_row_closure_captures_t captures = {0};
	TxnMemtableVersionChange change = {.writer = txn_memtable_version_Record_writer(version), .op = &frame->op,
	.lock = txn_memtable_version_Record_lock(version), .shown = txn_memtable_version_Record_shown(version)};
	txn_TupleRefSliceU8Usize_t row = {key, txn_memtable_version_Record_bytes(version).len};
	HostXid		top = {change.writer.has_top ? change.writer.top : change.writer.xid};
	bool		counts;

	captures.has_tuple = kind == TXN_RUNS_KIND_ROWS && txn_runs_tid(&op, &captures.tuple);
	if (captures.has_tuple)
	{
		if (frame->loading.has_cut && captures.tuple.f0 >= frame->loading.cut)
		{
			*frame->error = txn_runs_corrupt();
			return false;
		}
		frame->loading.past = Max(frame->loading.past, STD_ADD(u64, captures.tuple.f0, 1, 1642, 61));
	}
	frame->op = txn_memtable_version_OpRef_owned(&op);
	frame->op_live = true;
	change.has_lsn = txn_memtable_version_Record_lsn(version, &change.lsn);
	captures.change = &change;
	counts = frame->op.tag != TXN_MEMTABLE_OP_LOCK && frame->op.tag != TXN_MEMTABLE_OP_LOCK_ROW;
	if (counts)
	{
		TxnStatusEntry entry;

		if (!txn_status_lookup(frame->io, top, &counts, &entry, frame->error))
			return false;
	}
	if (counts &&
		!txn_status_redo_roomed(frame->io, &change.writer, &captures.has_roomed, &captures.roomed, frame->error))
		return false;
	if (!txn_memtable_table_prepare_Unit(frame->io, host_shared_Held_record(&frame->loading.held), &row, 1, NULL, 0,
										 captures.has_tuple, txn_runs_pushed_row_closure_0, &captures, NULL,
										 frame->error))
		return false;
	frame->op_live = false;
	txn_memtable_Op_drop(&frame->op);
	return true;
}

/* txn_runs_pushed_versions pushes a storage's run's keys' versions, oldest first, leaving out each a linked spill holds again. */
static bool
txn_runs_pushed_versions(txn_runs_pushed_runs_frame_0_t *frame, TxnRunsKind kind)
{
	HostSharedRecord record = host_shared_Held_record(&frame->loading.held);
	bool		has_twin = frame->loading.spills;
	TxnRunsKind twin = TXN_RUNS_KIND_SPILLED_VALUES;

	if (kind == TXN_RUNS_KIND_ROWS)
		twin = TXN_RUNS_KIND_SPILLED_ROWS;
	else if (kind == TXN_RUNS_KIND_ENTRIES)
		twin = TXN_RUNS_KIND_SPILLED_ENTRIES;
	else if (kind != TXN_RUNS_KIND_VALUES)
		has_twin = false;
	for (size_t k = 0; k < frame->decoded.keys.len; k++)
	{
		const TxnRunsKey *key = &STD_VEC_AT(&frame->decoded.keys, TxnRunsKey, k);

		for (size_t v = key->versions.len; v > 0; v--)
		{
			const TxnMemtableVersionRecord *version = &STD_VEC_AT(&key->versions, TxnMemtableVersionRecord, v - 1);
			StdBytes	bytes = txn_memtable_version_Record_bytes(version);
			bool		twinned = false;
			uint32		ender;

			if (has_twin &&
				!txn_spill_twinned(frame->io, record, twin, key->key.ptr, key->key.len, bytes.ptr, bytes.len, &twinned,
								   frame->error))
				return false;
			if (twinned)
				continue;
			if (kind == TXN_RUNS_KIND_ROWS || kind == TXN_RUNS_KIND_ENTRIES)
			{
				if (!txn_runs_pushed_row(frame, kind, key->key, version))
					return false;
			}
			else if (kind == TXN_RUNS_KIND_FREE)
			{
				if (!txn_runs_pushed_back(&TXN_MEMTABLE_MAP_FREE_TREE_OPS, frame->io, record, key->key, version,
										  frame->error) ||
					!txn_runs_map_counted(frame->io, version, frame->error))
					return false;
				if (txn_memtable_version_Record_ender(version, &ender) &&
					!txn_memtable_map_ended(frame->io, record,
											txn_memtable_map_taken(txn_memtable_version_Record_op(version)), ender,
											frame->error))
					return false;
			}
			else if (!txn_runs_pushed_back(&TXN_MEMTABLE_MAP_VALUES_TREE_OPS, frame->io, record, key->key, version,
										   frame->error) ||
					 !txn_runs_map_counted(frame->io, version, frame->error))
				return false;
		}
	}
	return true;
}

/* txn_runs_pushed_file pushes one file a manifest names into the delta. */
static bool
txn_runs_pushed_file(txn_runs_pushed_runs_frame_0_t *frame, const RepoRunFile *file)
{
	TxnRunsKind kind;
	TxnRunsNames names = {.persistence = TXN_STATE_PERSISTENCE_PERMANENT, .tablespace = file->tablespace,
	.database = frame->repository, .relation = file->relation, .redo = frame->runs->redo};
	HostStorage storage = {file->tablespace, frame->repository, file->relation};

	if (!txn_runs_Kind_of(file->kind, &kind))
	{
		*frame->error = txn_runs_corrupt();
		return false;
	}
	names.kind = kind;
	if (file->has_spilled)
		names.redo = file->spilled.xid;
	if (txn_runs_Kind_spilled(kind) != file->has_spilled)
	{
		*frame->error = txn_runs_corrupt();
		return false;
	}
	if (file->has_spilled)
		txn_runs_spilled_path(frame, &names, file->spilled.number);
	else
	{
		char		name[64];
		size_t		len = txn_runs_file_name(&names, name, sizeof(name));

		std_path_join(frame->dir.bytes.ptr, frame->dir.bytes.len, name, len, &frame->path);
		frame->path_live = true;
	}
	if (!txn_runs_read_whole(frame->io, &frame->path, file->len, &frame->bytes, &frame->bytes_live, frame->error))
		return false;
	if (kind == TXN_RUNS_KIND_STATUS)
		return txn_runs_pushed_status(frame, &names, file);
	if (kind == TXN_RUNS_KIND_SPILLED_TUPLES)
	{
		if (txn_runs_tupled_rows(&frame->runs->files, file) != NULL)
			return true;
		*frame->error = txn_runs_corrupt();
		return false;
	}
	if (!txn_runs_decode(frame->bytes.ptr, frame->bytes.len, &names, file->hash, &frame->decoded, frame->error))
		return false;
	frame->decoded_live = true;
	if (kind == TXN_RUNS_KIND_SPILLED_CHUNKS || kind == TXN_RUNS_KIND_CHUNKS || kind == TXN_RUNS_KIND_IDENTITY)
		return txn_runs_pushed_branch(frame, kind, file);
	if (!txn_runs_pushed_loading(frame, &storage))
		return false;
	if (file->has_spilled)
		return txn_runs_pushed_spill(frame, kind, &names, file);
	if (frame->decoded.tail.tag == TXN_RUNS_TAIL_COUNTER)
	{
		uint64		counter;

		if (!txn_runs_counted(frame->decoded.tail.counter, frame->page, &counter, frame->error) ||
			!txn_insert_loaded(frame->io, TREES_STORE_READER_NODE_STORE(&frame->store), &frame->loading.held,
							   frame->page, counter, frame->error))
			return false;
		frame->loading.has_cut = true;
		frame->loading.cut = frame->decoded.tail.counter;
	}
	return txn_runs_pushed_versions(frame, kind);
}

/* txn_runs_pushed_runs_body_0 is pushed_runs' body. */
static void
txn_runs_pushed_runs_body_0(void *frame_)
{
	txn_runs_pushed_runs_frame_0_t *frame = frame_;

	txn_runs_directory(frame->repository, frame->own, frame->runs->redo, &frame->dir);
	frame->dir_live = true;
	frame->store = repo_Repo_reader(frame->repo, HOST_CACHE_PLACEMENT_SERVER, frame->reading);
	std_slice_sort_by_key_RunFileTupleBoolU32U32BoolBoolU8(frame->runs->files.ptr, frame->runs->files.len,
														   txn_runs_pushed_runs_closure_0, NULL, NULL);
	for (size_t k = 0; k < frame->runs->files.len; k++)
	{
		if (!txn_runs_pushed_file(frame, &STD_VEC_AT(&frame->runs->files, RepoRunFile, k)))
		{
			txn_runs_pushed_runs_pad_0(frame);
			return;
		}
		txn_runs_pushed_runs_step_drop(frame);
	}
	frame->ok = txn_runs_ended(frame->io, TREES_STORE_READER_NODE_STORE(&frame->store), frame->page,
							   frame->loading_live ? &frame->loading : NULL, frame->error);
	txn_runs_pushed_runs_pad_0(frame);
}

/* txn_runs_pushed_runs pushes the runs a repository's newest manifest names into the delta, as loaded says. */
static bool
txn_runs_pushed_runs(HostIoIo *io, const TxnInsertPage *page, const RepoRepo *repo, const HostSharedReader *reading,
					 uint32 repository, uint32 own, RepoRuns *runs, HostError *error)
{
	txn_runs_pushed_runs_frame_0_t frame = {.io = io, .page = page, .repo = repo, .reading = reading,
	.repository = repository, .own = own, .runs = runs, .error = error};

	std_landing(&frame, txn_runs_pushed_runs_body_0, txn_runs_pushed_runs_pad_0);
	return frame.ok;
}

/* txn_runs_dropped_frame_0_t is dropped's state across its pad: the database's states and their storages, while live. */
typedef struct txn_runs_dropped_frame_0_t
{
	HostIoIo   *io;
	uint32		repository;
	HostError  *error;
	bool		ok;
	/* states is a Vec<(u64, Held<State>)>. */
	StdVec		states;
	bool		states_live;
	/* storages is a Vec<Storage>. */
	StdVec		storages;
	bool		storages_live;
} txn_runs_dropped_frame_0_t;

/* txn_runs_dropped_pad_0 is dropped's pad: the storages and the states, while live. */
static void
txn_runs_dropped_pad_0(void *frame_)
{
	txn_runs_dropped_frame_0_t *frame = frame_;

	if (frame->storages_live)
	{
		frame->storages_live = false;
		std_vec_free_Checked(&frame->storages, sizeof(HostStorage), _Alignof(HostStorage));
	}
	if (frame->states_live)
	{
		frame->states_live = false;
		for (size_t k = 0; k < frame->states.len; k++)
			host_shared_Held_drop(&STD_VEC_AT(&frame->states, txn_TupleU64HeldState_t, k).f1);
		std_vec_free_Checked(&frame->states, sizeof(txn_TupleU64HeldState_t), _Alignof(txn_TupleU64HeldState_t));
	}
}

/* txn_runs_dropped_body_0 is dropped's body. */
static void
txn_runs_dropped_body_0(void *frame_)
{
	txn_runs_dropped_frame_0_t *frame = frame_;

	std_vec_new(&frame->storages, sizeof(HostStorage), _Alignof(HostStorage));
	frame->storages_live = true;
	if (!txn_state_states(frame->io, frame->repository, &frame->states, frame->error))
	{
		txn_runs_dropped_pad_0(frame);
		return;
	}
	frame->states_live = true;
	for (size_t k = 0; k < frame->states.len; k++)
	{
		HostStorage storage;
		TxnStatePersistence persistence;

		if (txn_state_described(host_shared_Held_record(&STD_VEC_AT(&frame->states, txn_TupleU64HeldState_t, k).f1),
								&storage, &persistence))
			std_vec_push_Checked(&frame->storages, &storage, sizeof(storage), _Alignof(HostStorage));
	}
	frame->states_live = false;
	for (size_t k = 0; k < frame->states.len; k++)
		host_shared_Held_drop(&STD_VEC_AT(&frame->states, txn_TupleU64HeldState_t, k).f1);
	std_vec_free_Checked(&frame->states, sizeof(txn_TupleU64HeldState_t), _Alignof(txn_TupleU64HeldState_t));
	for (size_t k = 0; k < frame->storages.len; k++)
		if (!txn_state_unlinked(frame->io, &STD_VEC_AT(&frame->storages, HostStorage, k), frame->error))
		{
			txn_runs_dropped_pad_0(frame);
			return;
		}
	frame->ok = txn_memtable_branch_dropped(frame->io, frame->repository, frame->error);
	txn_runs_dropped_pad_0(frame);
}

/*
 * txn_runs_dropped frees every state of a database and its working state's
 * delta, as a skipped repository's are, and as DROP DATABASE and its redo do
 * before the repository is dropped.
 */
bool
txn_runs_dropped(HostIoIo *io, uint32 repository, HostError *error)
{
	txn_runs_dropped_frame_0_t frame = {.io = io, .repository = repository, .error = error};

	std_landing(&frame, txn_runs_dropped_body_0, txn_runs_dropped_pad_0);
	return frame.ok;
}

/* txn_runs_skipped warns once of a repository whose newest manifest or a run file is corrupt, frees what its runs made, and marks its database. */
static bool
txn_runs_skipped(HostIoIo *io, uint32 repository, HostError *raised, HostError *error)
{
	char		context[96];

	snprintf(context, sizeof(context), "database %u: its checkpoint's runs aren't loaded", repository);
	return host_warn(HOST_IO_IO_IO_DOORS(io), raised, context, error) && txn_runs_dropped(io, repository, error) &&
		txn_state_skipped(io, repository, error);
}

/* txn_runs_loaded_frame_0_t is loaded's state across its pad: the repositories, one open with its reader, and its runs, while live. */
typedef struct txn_runs_loaded_frame_0_t
{
	HostIoIo   *io;
	const TxnInsertPage *page;
	HostError  *error;
	bool		ok;
	/* repositories is a Vec<(u32, u32)>. */
	StdVec		repositories;
	bool		repositories_live;
	RepoRepo	repo;
	bool		repo_live;
	StdArcInner *manifest;
	bool		manifest_live;
	RepoRuns	runs;
	bool		runs_live;
	HostSharedReader reading;
	bool		reading_live;
} txn_runs_loaded_frame_0_t;

/* txn_runs_loaded_step_drop drops what a repository's step holds, while live. */
static void
txn_runs_loaded_step_drop(txn_runs_loaded_frame_0_t *frame)
{
	if (frame->reading_live)
	{
		frame->reading_live = false;
		host_shared_Reader_drop(&frame->reading);
	}
	if (frame->runs_live)
	{
		frame->runs_live = false;
		repo_Runs_drop(&frame->runs);
	}
	if (frame->manifest_live)
	{
		frame->manifest_live = false;
		repo_ArcManifest_drop(frame->manifest);
	}
	if (frame->repo_live)
	{
		frame->repo_live = false;
		repo_Repo_drop(&frame->repo);
	}
}

/* txn_runs_loaded_pad_0 is loaded's pad: a repository's step's holdings, then the repositories, while live. */
static void
txn_runs_loaded_pad_0(void *frame_)
{
	txn_runs_loaded_frame_0_t *frame = frame_;

	txn_runs_loaded_step_drop(frame);
	if (frame->repositories_live)
	{
		frame->repositories_live = false;
		std_vec_free_Checked(&frame->repositories, sizeof(std_TupleU32U32_t), _Alignof(std_TupleU32U32_t));
	}
}

/* txn_runs_named opens a repository at a start into the frame and reads the runs its newest manifest names. */
static bool
txn_runs_named(txn_runs_loaded_frame_0_t *frame, uint32 repository, uint32 own, ChunksErrorError *error)
{
	const RepoManifest *newest;

	if (!repo_Repo_open(frame->io, repository, own, &frame->repo, error))
		return false;
	frame->repo_live = true;
	if (!repo_Repo_manifest(&frame->repo, frame->io, &frame->manifest, error))
	{
		frame->repo_live = false;
		repo_Repo_drop(&frame->repo);
		return false;
	}
	frame->manifest_live = true;
	newest = repo_Repo_newest(frame->manifest);
	if (newest->has_runs)
	{
		repo_Runs_Clone_clone(&newest->runs, &frame->runs);
		frame->runs_live = true;
	}
	frame->manifest_live = false;
	repo_ArcManifest_drop(frame->manifest);
	return true;
}

/* txn_runs_raised answers whether a failure is a corrupt repository's, which the loader skips. */
static bool
txn_runs_raised(const HostError *error)
{
	return memcmp(host_Error_sqlstate(error), "XX001", 5) == 0;
}

/* txn_runs_loaded_repository loads one repository's runs, skipping a corrupt one. */
static bool
txn_runs_loaded_repository(txn_runs_loaded_frame_0_t *frame, uint32 repository, uint32 own)
{
	ChunksErrorError failed;
	HostError	raised;

	if (!txn_runs_named(frame, repository, own, &failed))
	{
		raised = txn_base_storage(&failed);
		if (!txn_runs_raised(&raised))
		{
			*frame->error = raised;
			return false;
		}
		return txn_runs_skipped(frame->io, repository, &raised, frame->error);
	}
	if (!txn_runs_removed(frame->io, &frame->repo, repository, own, TXN_RUNS_REMOVAL_START, frame->error) ||
		!host_shared_read(frame->io, &frame->reading, frame->error))
		return false;
	frame->reading_live = true;
	if (frame->runs_live &&
		!txn_runs_pushed_runs(frame->io, frame->page, &frame->repo, &frame->reading, repository, own, &frame->runs,
							  &raised))
	{
		if (!txn_runs_raised(&raised))
		{
			*frame->error = raised;
			return false;
		}
		if (!txn_runs_skipped(frame->io, repository, &raised, frame->error))
			return false;
	}
	frame->repo_live = false;
	if (!repo_Repo_close(&frame->repo, frame->io, &frame->reading, &failed))
	{
		*frame->error = txn_base_storage(&failed);
		return false;
	}
	txn_runs_loaded_step_drop(frame);
	return true;
}

/* txn_runs_loaded_body_0 is loaded's body. */
static void
txn_runs_loaded_body_0(void *frame_)
{
	txn_runs_loaded_frame_0_t *frame = frame_;
	ChunksErrorError failed;

	if (!host_shared_ready())
	{
		frame->ok = true;
		return;
	}
	if (!chunks_packs_open_repositories(HOST_IO_IO_READ_DOORS(frame->io), &frame->repositories, &failed))
	{
		*frame->error = txn_base_storage(&failed);
		return;
	}
	frame->repositories_live = true;
	for (size_t k = 0; k < frame->repositories.len; k++)
	{
		const std_TupleU32U32_t *repository = &STD_VEC_AT(&frame->repositories, std_TupleU32U32_t, k);

		if (!txn_runs_loaded_repository(frame, repository->f0, repository->f1))
		{
			txn_runs_loaded_pad_0(frame);
			return;
		}
	}
	frame->ok = true;
	txn_runs_loaded_pad_0(frame);
}

/*
 * txn_runs_loaded loads every repository's runs into the delta at a start,
 * before redo's first record: for each repository it removes the run
 * directories neither generation of its manifest names, then reads each file
 * its newest manifest names, refusing one whose length or xxh3 isn't its
 * entry's as data corruption, and pushes each key's versions oldest first. A
 * repository whose packs, newest manifest, or a run file are corrupt is
 * warned of once and left as it is, its database marked. A bootstrap process
 * does nothing.
 */
bool
txn_runs_loaded(HostIoIo *io, const TxnInsertPage *page, HostError *error)
{
	txn_runs_loaded_frame_0_t frame = {.io = io, .page = page, .error = error};

	std_landing(&frame, txn_runs_loaded_body_0, txn_runs_loaded_pad_0);
	return frame.ok;
}
