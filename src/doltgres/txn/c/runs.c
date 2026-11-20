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

#ifdef DOLTGRES_TEST_TXN
#include "doltgres/simc/simc.h"
#include "doltgres/std/test.h"
#include "doltgres/std/test_fs.h"
#include "doltgres/txn/free_space.h"
#include "doltgres/txn/memtable/testing.h"

/* TXN_RUNS_TESTS_NAMES is NAMES, a test's run of a storage's rows. */
static const TxnRunsNames TXN_RUNS_TESTS_NAMES = {.kind = TXN_RUNS_KIND_ROWS, .persistence = TXN_STATE_PERSISTENCE_PERMANENT,
.tablespace = 1663, .database = 9400, .relation = 16384, .redo = UINT64CONST(0x100000a28)};

/* TXN_RUNS_TESTS_CUT is CUT, the counter a test's free space run is cut at. */
#define TXN_RUNS_TESTS_CUT ((UINT64CONST(5) << 16) | 1)

/* txn_runs_tests_put_record returns a version's record: a put of a value with heap bytes by a writer, with an LSN. */
static StdVec
txn_runs_tests_put_record(uint64 xid, const uint8 *value, size_t value_len, bool has_lsn, uint64 lsn,
						  uint32 heap_bytes)
{
	TxnMemtableOp op = txn_memtable_testing_put(value, value_len);
	TxnMemtableVersionWriter writer = {.xid = xid};
	TxnMemtableVersionShown shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND};
	StdVec		out;

	op.u.put.heap_bytes = heap_bytes;
	std_vec_new(&out, 1, 1);
	txn_memtable_version_encode(&writer, &op, TXN_MEMTABLE_LOCK_MODE_UPDATE, shown, has_lsn, lsn, &out);
	txn_memtable_Op_drop(&op);
	return out;
}

/* txn_runs_tests_record returns a version's record: a put of a row of 32 heap bytes by a writer, with an LSN. */
static StdVec
txn_runs_tests_record(uint64 xid, const uint8 *value, size_t value_len, bool has_lsn, uint64 lsn)
{
	return txn_runs_tests_put_record(xid, value, value_len, has_lsn, lsn, 32);
}

/* txn_runs_tests_entry_record returns a map's version's record: a put of a value by a writer, with an LSN, holding no heap bytes. */
static StdVec
txn_runs_tests_entry_record(uint64 xid, const uint8 *value, size_t value_len, bool has_lsn, uint64 lsn)
{
	return txn_runs_tests_put_record(xid, value, value_len, has_lsn, lsn, 0);
}

/* txn_runs_tests_sum returns the xxh3 a run's file's trailer holds, which its manifest entry names. */
static uint64
txn_runs_tests_sum(const StdVec *bytes)
{
	return std_u64_from_le_bytes((const uint8 *) bytes->ptr + bytes->len - 16);
}

/* txn_runs_tests_keyed_t is a key of a test's run's file: its bytes and its versions' records, newest first. */
typedef struct txn_runs_tests_keyed_t
{
	StdBytes	key;
	/* versions is a slice of Vec<u8>. */
	const StdVec *versions;
	size_t		versions_len;
} txn_runs_tests_keyed_t;

/* txn_runs_tests_tail returns a test run's header's tail: a free space run's cut at CUT, and an entries run's of table 16385 and index 16390. */
static TxnRunsTail
txn_runs_tests_tail(const TxnRunsNames *names)
{
	switch (names->kind)
	{
		case TXN_RUNS_KIND_FREE:
			return (TxnRunsTail)
			{
				.tag = TXN_RUNS_TAIL_COUNTER, .counter = TXN_RUNS_TESTS_CUT
			};
		case TXN_RUNS_KIND_ENTRIES:
		case TXN_RUNS_KIND_SPILLED_ENTRIES:
			return (TxnRunsTail)
			{
				.tag = TXN_RUNS_TAIL_INDEX, .table = 16385, .index = 16390
			};
		default:
			return (TxnRunsTail)
			{
				.tag = TXN_RUNS_TAIL_NONE
			};
	}
}

/* txn_runs_tests_file returns a run's file of the names given, holding each key's versions. */
static StdVec
txn_runs_tests_file(const TxnRunsNames *names, const txn_runs_tests_keyed_t *keys, size_t keys_len)
{
	TxnRunsEncoder encoder;
	StdVec		out;
	StdVec		part;

	std_vec_new(&out, 1, 1);
	txn_runs_Encoder_begin(names, txn_runs_tests_tail(names), &encoder, &out);
	for (size_t k = 0; k < keys_len; k++)
	{
		StdVec		versions;

		std_vec_with_capacity_Checked(&versions, keys[k].versions_len, sizeof(StdBytes), _Alignof(StdBytes));
		for (size_t v = 0; v < keys[k].versions_len; v++)
		{
			StdBytes	version = {keys[k].versions[v].ptr, keys[k].versions[v].len};

			std_vec_push_Checked(&versions, &version, sizeof(StdBytes), _Alignof(StdBytes));
		}
		std_vec_new(&part, 1, 1);
		txn_runs_Encoder_key(&encoder, keys[k].key.ptr, keys[k].key.len, versions.ptr, versions.len, &part);
		std_vec_extend_from_slice_Checked(&out, part.ptr, part.len, 1, 1);
		std_vec_free_Checked(&part, 1, 1);
		std_vec_free_Checked(&versions, sizeof(StdBytes), _Alignof(StdBytes));
	}
	std_vec_new(&part, 1, 1);
	txn_runs_Encoder_end(&encoder, NULL, 0, &part);
	std_vec_extend_from_slice_Checked(&out, part.ptr, part.len, 1, 1);
	std_vec_free_Checked(&part, 1, 1);
	txn_runs_Encoder_drop(&encoder);
	return out;
}

/* txn_runs_tests_header returns the bytes of a run's header, as the encoder frames them, with its tail. */
static StdVec
txn_runs_tests_header(const TxnRunsNames *names)
{
	TxnRunsEncoder encoder;
	StdVec		framed;
	StdVec		header;

	std_vec_new(&framed, 1, 1);
	txn_runs_Encoder_begin(names, txn_runs_tests_tail(names), &encoder, &framed);
	std_slice_to_vec_Checked(&header, (const uint8 *) framed.ptr + sizeof(TXN_RUNS_MAGIC) + 1,
							 framed.len - 8 - (sizeof(TXN_RUNS_MAGIC) + 1), 1, 1);
	std_vec_free_Checked(&framed, 1, 1);
	txn_runs_Encoder_drop(&encoder);
	return header;
}

/* txn_runs_tests_keyed returns the bytes of a key's record: the key, then its versions' records. */
static StdVec
txn_runs_tests_keyed(const uint8 *key, size_t key_len, const StdVec *versions, size_t versions_len)
{
	StdVec		record;

	std_vec_new(&record, 1, 1);
	chunks_varint_put_varint(&record, (uint64) key_len);
	std_vec_extend_from_slice_Checked(&record, key, key_len, 1, 1);
	chunks_varint_put_varint(&record, (uint64) versions_len);
	for (size_t k = 0; k < versions_len; k++)
	{
		chunks_varint_put_varint(&record, (uint64) versions[k].len);
		std_vec_extend_from_slice_Checked(&record, versions[k].ptr, versions[k].len, 1, 1);
	}
	return record;
}

/*
 * txn_runs_tests_raw returns a run's file built by hand, for the files the
 * encoder never writes: its header's bytes and each key's record framed, then
 * a trailer of a count of versions and the frames' xxh3 with bits of it
 * flipped.
 */
static StdVec
txn_runs_tests_raw(const uint8 *header, size_t header_len, const StdVec *records, size_t records_len, uint64 count,
				   uint64 flipped)
{
	StdVec		out;
	uint8		trailer[16];

	std_slice_to_vec_Checked(&out, TXN_RUNS_MAGIC, sizeof(TXN_RUNS_MAGIC), 1, 1);
	txn_runs_framed(&out, header, header_len);
	for (size_t k = 0; k < records_len; k++)
		txn_runs_framed(&out, records[k].ptr, records[k].len);
	std_u64_to_le_bytes(count, trailer);
	std_u64_to_le_bytes(XXH3_64bits((const uint8 *) out.ptr + sizeof(TXN_RUNS_MAGIC),
									out.len - sizeof(TXN_RUNS_MAGIC)) ^ flipped, trailer + 8);
	txn_runs_framed(&out, trailer, sizeof(trailer));
	return out;
}

/* txn_runs_tests_chain_t is a key of a tree with its versions' records, newest first. */
typedef struct txn_runs_tests_chain_t
{
	/* key is a Vec<u8>. */
	StdVec		key;
	/* records is a Vec<Vec<u8>>. */
	StdVec		records;
} txn_runs_tests_chain_t;

/* txn_runs_tests_chain_Debug_fmt is a key and its records' Debug, the tuple (key, records). */
static bool
txn_runs_tests_chain_Debug_fmt(const void *value, StdFormatter *f)
{
	const txn_runs_tests_chain_t *chain = value;
	StdBytes	key = {chain->key.ptr, chain->key.len};
	StdTupleRef tuple = STD_TUPLE(2, STD_ITEM(&key, std_bytes_Debug_fmt),
								  STD_ITEM(&chain->records, txn_memtable_table_tests_keys_Debug_fmt));

	return std_tuple_Debug_fmt(&tuple, f);
}

/* txn_runs_tests_chains_Debug_fmt is a Vec<(Vec<u8>, Vec<Vec<u8>>)>'s Debug. */
static bool
txn_runs_tests_chains_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *chains = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t k = 0; k < chains->len; k++)
		std_DebugList_entry(&list, &STD_VEC_AT(chains, txn_runs_tests_chain_t, k), txn_runs_tests_chain_Debug_fmt);
	return std_DebugList_finish(&list);
}

/* txn_runs_tests_chains_eq reports whether two Vec<(Vec<u8>, Vec<Vec<u8>>)>s are equal. */
static bool
txn_runs_tests_chains_eq(const StdVec *a, const StdVec *b)
{
	if (a->len != b->len)
		return false;
	for (size_t k = 0; k < a->len; k++)
	{
		const txn_runs_tests_chain_t *x = &STD_VEC_AT(a, txn_runs_tests_chain_t, k);
		const txn_runs_tests_chain_t *y = &STD_VEC_AT(b, txn_runs_tests_chain_t, k);

		if (x->key.len != y->key.len || memcmp(x->key.ptr, y->key.ptr, x->key.len) != 0 ||
			!txn_memtable_table_tests_keys_eq(&x->records, &y->records))
			return false;
	}
	return true;
}

/* txn_runs_tests_chains_free frees a Vec<(Vec<u8>, Vec<Vec<u8>>)> and its keys and records. */
static void
txn_runs_tests_chains_free(StdVec *chains)
{
	for (size_t k = 0; k < chains->len; k++)
	{
		txn_runs_tests_chain_t *chain = &STD_VEC_AT(chains, txn_runs_tests_chain_t, k);

		std_vec_free_Checked(&chain->key, 1, 1);
		txn_memtable_table_tests_keys_free(&chain->records);
	}
	std_vec_free_Checked(chains, sizeof(txn_runs_tests_chain_t), _Alignof(txn_runs_tests_chain_t));
}

/* txn_runs_tests_chain_push pushes a key and copies of its records onto a Vec<(Vec<u8>, Vec<Vec<u8>>)>. */
static void
txn_runs_tests_chain_push(StdVec *chains, const uint8 *key, size_t key_len, const StdVec *records, size_t records_len)
{
	txn_runs_tests_chain_t chain;

	std_slice_to_vec_Checked(&chain.key, key, key_len, 1, 1);
	std_vec_with_capacity_Checked(&chain.records, records_len, sizeof(StdVec), _Alignof(StdVec));
	for (size_t k = 0; k < records_len; k++)
	{
		StdVec		record;

		std_vec_clone(&record, &records[k], 1, 1);
		std_vec_push_Checked(&chain.records, &record, sizeof(StdVec), _Alignof(StdVec));
	}
	std_vec_push_Checked(chains, &chain, sizeof(chain), _Alignof(txn_runs_tests_chain_t));
}

/* txn_runs_tests_records_free frees an array of a test's records. */
static void
txn_runs_tests_records_free(StdVec *records, size_t len)
{
	for (size_t k = 0; k < len; k++)
		std_vec_free_Checked(&records[k], 1, 1);
}

/*
 * The run's format: a file reads back each key and its versions as written,
 * newest first, with a key that goes on in a second record read as one key.
 */
void
test_txn_runs_tests_a_runs_file_reads_back_as_written(void)
{
	StdVec		one = txn_runs_tests_record(7, (const uint8 *) "one", 3, true, 9);
	StdVec		two = txn_runs_tests_record(8, (const uint8 *) "two", 3, false, 0);
	StdVec		three = txn_runs_tests_record(9, (const uint8 *) "three", 5, true, 10);
	StdVec		first[2] = {one, two};
	StdVec		again[1] = {three};
	StdVec		other[1] = {two};
	const txn_runs_tests_keyed_t keys[3] = {{{(const uint8 *) "a", 1}, first, 2}, {{(const uint8 *) "a", 1}, again, 1},
	{{(const uint8 *) "b", 1}, other, 1}};
	StdVec		a[3] = {one, two, three};
	StdVec		bytes = txn_runs_tests_file(&TXN_RUNS_TESTS_NAMES, keys, lengthof(keys));
	TxnRunsDecoded decoded;
	HostError	error;
	StdVec		read;
	StdVec		wanted;

	if (!txn_runs_decode(bytes.ptr, bytes.len, &TXN_RUNS_TESTS_NAMES, txn_runs_tests_sum(&bytes), &decoded, &error))
		STD_RESULT_EXPECT_FAILED(1849, 14, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	std_vec_with_capacity_Checked(&read, decoded.keys.len, sizeof(txn_runs_tests_chain_t),
								  _Alignof(txn_runs_tests_chain_t));
	for (size_t k = 0; k < decoded.keys.len; k++)
	{
		const TxnRunsKey *key = &STD_VEC_AT(&decoded.keys, TxnRunsKey, k);
		txn_runs_tests_chain_t chain;

		std_slice_to_vec_Checked(&chain.key, key->key.ptr, key->key.len, 1, 1);
		std_vec_with_capacity_Checked(&chain.records, key->versions.len, sizeof(StdVec), _Alignof(StdVec));
		for (size_t v = 0; v < key->versions.len; v++)
		{
			StdBytes	version = txn_memtable_version_Record_bytes(&STD_VEC_AT(&key->versions,
																				TxnMemtableVersionRecord, v));
			StdVec		record;

			std_slice_to_vec_Checked(&record, version.ptr, version.len, 1, 1);
			std_vec_push_Checked(&chain.records, &record, sizeof(StdVec), _Alignof(StdVec));
		}
		std_vec_push_Checked(&read, &chain, sizeof(chain), _Alignof(txn_runs_tests_chain_t));
	}
	txn_runs_Decoded_drop(&decoded);
	std_vec_new(&wanted, sizeof(txn_runs_tests_chain_t), _Alignof(txn_runs_tests_chain_t));
	txn_runs_tests_chain_push(&wanted, (const uint8 *) "a", 1, a, lengthof(a));
	txn_runs_tests_chain_push(&wanted, (const uint8 *) "b", 1, other, lengthof(other));
	STD_ASSERT_EQ(1854, 9, txn_runs_tests_chains_eq(&read, &wanted), &read, txn_runs_tests_chains_Debug_fmt, &wanted,
				  txn_runs_tests_chains_Debug_fmt);
	txn_runs_tests_chains_free(&wanted);
	txn_runs_tests_chains_free(&read);
	std_vec_free_Checked(&bytes, 1, 1);
	txn_runs_tests_records_free(a, lengthof(a));
}

/* txn_runs_tests_flipped returns a copy of a file with a bit of one of its bytes flipped, as the refusals case's flipped closure does. */
static StdVec
txn_runs_tests_flipped(const StdVec *good, size_t at)
{
	StdVec		flipped;

	std_vec_clone(&flipped, good, 1, 1);
	((uint8 *) flipped.ptr)[at] ^= 1;
	return flipped;
}

/* txn_runs_tests_changed returns a copy of a header with one of its bytes changed, as the refusals case's changed closure does. */
static StdVec
txn_runs_tests_changed(const StdVec *head, size_t at, uint8 to)
{
	StdVec		changed;

	std_vec_clone(&changed, head, 1, 1);
	((uint8 *) changed.ptr)[at] = to;
	return changed;
}

/* txn_runs_tests_blocks returns a free space run's file of one block's key with an entry's version, as the refusals case's blocks closure does. */
static StdVec
txn_runs_tests_blocks(const TxnRunsNames *free, const StdVec *entry, uint32 block)
{
	uint8		key[4];
	txn_runs_tests_keyed_t keyed = {{key, sizeof(key)}, entry, 1};

	std_u32_to_be_bytes(block, key);
	return txn_runs_tests_file(free, &keyed, 1);
}

/* txn_runs_tests_one_key returns a run's file of the names given holding one key with one version. */
static StdVec
txn_runs_tests_one_key(const TxnRunsNames *names, const uint8 *key, size_t key_len, const StdVec *version)
{
	txn_runs_tests_keyed_t keyed = {{key, key_len}, version, 1};

	return txn_runs_tests_file(names, &keyed, 1);
}

/* txn_runs_tests_keys_len returns the count of keys a run's file of the names given decodes to with its own xxh3, as the refusals case's reads unwrap it at their places. */
static size_t
txn_runs_tests_keys_len(uint32 line, uint32 col, const StdVec *bytes, const TxnRunsNames *names)
{
	TxnRunsDecoded decoded;
	HostError	error;
	size_t		len;

	if (!txn_runs_decode(bytes->ptr, bytes->len, names, txn_runs_tests_sum(bytes), &decoded, &error))
		STD_RESULT_EXPECT_FAILED(line, col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	len = decoded.keys.len;
	txn_runs_Decoded_drop(&decoded);
	return len;
}

/* txn_runs_tests_Tail_Debug_fmt is Tail's derived Debug. */
static bool
txn_runs_tests_Tail_Debug_fmt(const void *value, StdFormatter *f)
{
	const TxnRunsTail *tail = value;

	switch (tail->tag)
	{
		case TXN_RUNS_TAIL_COUNTER:
			{
				StdDebugTuple t = std_fmt_debug_tuple(f, "Counter", 7);

				std_DebugTuple_field(&t, &tail->counter, std_u64_Debug_fmt);
				return std_DebugTuple_finish(&t);
			}
		case TXN_RUNS_TAIL_INDEX:
			{
				StdDebugStruct s = std_fmt_debug_struct(f, "Index", 5);

				std_DebugStruct_field(&s, "table", 5, &tail->table, std_u32_Debug_fmt);
				std_DebugStruct_field(&s, "index", 5, &tail->index, std_u32_Debug_fmt);
				return std_DebugStruct_finish(&s);
			}
		default:
			return std_fmt_write_str(f, "None", 4);
	}
}

/*
 * The decoder's refusals, each XX001: a flipped byte in a key's frame, in the
 * magic, and in the trailer's own xxh3, another run's names, a key out of
 * order, bytes past the trailer, a file with no trailer, a free space map's
 * version a transaction wrote with an LSN, a version's record with an unknown
 * flag and one with VALUES and no ids, a header of a kind or a persistence
 * none has or cut short, a byte past a key's versions, a trailer whose count
 * or xxh3 isn't the file's, a free space run with no counter, a rows' run
 * with one, a free space run's key past its counter's block, a value map's
 * version redo pushed, and a rows run's version that empties a free space
 * entry (PRevPF 7's V2), a rows run's version that puts a row with no heap
 * bytes, and a free space run's version with them (bugs row 80); and a
 * manifest entry's xxh3 that isn't the trailer's. A free space run whose last
 * key is its counter's block reads, and answers its counter, a value map's
 * run whose version carries an LSN, a kept value's count add's, reads, and so
 * does a free space run whose version carries one written by no transaction,
 * a free's.
 */
void
test_txn_runs_tests_a_runs_decoder_refuses_what_isnt_its_run(void)
{
	static const StdFmtPiece numbered[] = {STD_FMT_ARG(0)};
	static const uint8 zero[2] = {0, 0};
	const TxnRunsNames *names = &TXN_RUNS_TESTS_NAMES;
	TxnRunsNames redone = TXN_RUNS_TESTS_NAMES;
	TxnRunsNames values = TXN_RUNS_TESTS_NAMES;
	TxnRunsNames free_run = TXN_RUNS_TESTS_NAMES;
	TxnMemtableVersionWriter none = TXN_MEMTABLE_VERSION_NO_TRANSACTION;
	TxnMemtableVersionWriter seven = {.xid = 7};
	TxnMemtableVersionShown shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND};
	TxnMemtableOp emptying = {.tag = TXN_MEMTABLE_OP_EMPTIED, .u.emptied = {3}};
	StdVec		one = txn_runs_tests_record(7, (const uint8 *) "one", 3, true, 9);
	StdVec		good = txn_runs_tests_one_key(names, (const uint8 *) "a", 1, &one);
	StdVec		head = txn_runs_tests_header(names);
	StdVec		key = txn_runs_tests_keyed((const uint8 *) "a", 1, &one, 1);
	StdVec		built = txn_runs_tests_raw(head.ptr, head.len, &key, 1, 1, 0);
	StdBytes	built_bytes = {built.ptr, built.len};
	StdBytes	good_bytes = {good.ptr, good.len};
	StdVec		past;
	StdVec		unknown;
	StdVec		replayed;
	StdVec		empty;
	StdVec		longer;
	StdVec		entry;
	StdVec		counted;
	StdVec		emptied;
	StdVec		row;
	StdVec		given;
	StdVec		free_head;
	StdVec		free_key;
	StdVec		cases[24];
	const TxnRunsNames *cases_names[24];
	uint8		block_one[4];
	uint8		block_five[4];
	uint8		cut[8];
	TxnRunsDecoded decoded;
	HostError	error;
	size_t		len;
	size_t		count_one = 1;
	bool		refused;

	redone.redo = 1;
	values.kind = TXN_RUNS_KIND_VALUES;
	free_run.kind = TXN_RUNS_KIND_FREE;
	STD_ASSERT_EQ(1873, 9, built.len == good.len && memcmp(built.ptr, good.ptr, good.len) == 0, &built_bytes,
				  std_bytes_Debug_fmt, &good_bytes, std_bytes_Debug_fmt);
	std_vec_clone(&past, &good, 1, 1);
	std_vec_extend_from_slice_Checked(&past, zero, 1, 1, 1);
	std_vec_clone(&unknown, &one, 1, 1);
	((uint8 *) unknown.ptr)[28] |= 32;
	replayed = txn_runs_tests_entry_record(7, (const uint8 *) "one", 3, true, 9);
	((uint8 *) replayed.ptr)[28] |= 8;
	std_slice_to_vec_Checked(&empty, one.ptr, 29, 1, 1);
	((uint8 *) empty.ptr)[28] |= 4;
	std_vec_extend_from_slice_Checked(&empty, zero, 2, 1, 1);
	std_vec_extend_from_slice_Checked(&empty, (const uint8 *) one.ptr + 29, one.len - 29, 1, 1);
	std_vec_clone(&longer, &key, 1, 1);
	std_vec_extend_from_slice_Checked(&longer, zero, 1, 1, 1);
	entry = txn_runs_tests_entry_record(7, (const uint8 *) "entry", 5, false, 0);
	std_vec_clone(&counted, &head, 1, 1);
	std_u64_to_le_bytes(TXN_RUNS_TESTS_CUT, cut);
	std_vec_extend_from_slice_Checked(&counted, cut, sizeof(cut), 1, 1);
	std_vec_new(&emptied, 1, 1);
	txn_memtable_version_encode(&seven, &emptying, TXN_MEMTABLE_LOCK_MODE_UPDATE, shown, false, 0, &emptied);
	std_u32_to_be_bytes(1, block_one);
	cases[0] = txn_runs_tests_flipped(&good, sizeof(TXN_RUNS_MAGIC) + 1 + TXN_RUNS_HEADER + 8 + 3);
	cases[1] = txn_runs_tests_flipped(&good, 0);
	cases[2] = txn_runs_tests_flipped(&good, good.len - 1);
	std_vec_clone(&cases[3], &good, 1, 1);
	{
		StdVec		b[1] = {one};
		StdVec		a[1] = {one};
		const txn_runs_tests_keyed_t order[2] = {{{(const uint8 *) "b", 1}, b, 1}, {{(const uint8 *) "a", 1}, a, 1}};

		cases[4] = txn_runs_tests_file(names, order, lengthof(order));
	}
	cases[5] = past;
	std_slice_to_vec_Checked(&cases[6], good.ptr, good.len - (1 + TXN_RUNS_TRAILER + 8), 1, 1);
	cases[7] = txn_runs_tests_one_key(&free_run, block_one, sizeof(block_one), &one);
	cases[8] = txn_runs_tests_one_key(names, (const uint8 *) "a", 1, &unknown);
	cases[9] = txn_runs_tests_one_key(names, (const uint8 *) "a", 1, &empty);
	{
		StdVec		changed = txn_runs_tests_changed(&head, 0, 4);

		cases[10] = txn_runs_tests_raw(changed.ptr, changed.len, &key, 1, 1, 0);
		std_vec_free_Checked(&changed, 1, 1);
		changed = txn_runs_tests_changed(&head, 1, 2);
		cases[11] = txn_runs_tests_raw(changed.ptr, changed.len, &key, 1, 1, 0);
		std_vec_free_Checked(&changed, 1, 1);
	}
	cases[12] = txn_runs_tests_raw(head.ptr, TXN_RUNS_HEADER - 1, &key, 1, 1, 0);
	cases[13] = txn_runs_tests_raw(head.ptr, head.len, &longer, 1, 1, 0);
	cases[14] = txn_runs_tests_raw(head.ptr, head.len, &key, 1, 2, 0);
	cases[15] = txn_runs_tests_raw(head.ptr, head.len, &key, 1, 0, 0);
	cases[16] = txn_runs_tests_raw(head.ptr, head.len, &key, 1, 1, 1);
	free_head = txn_runs_tests_header(&free_run);
	free_key = txn_runs_tests_keyed(block_one, sizeof(block_one), &entry, 1);
	cases[17] = txn_runs_tests_raw(free_head.ptr, TXN_RUNS_HEADER, &free_key, 1, 1, 0);
	cases[18] = txn_runs_tests_raw(counted.ptr, counted.len, &key, 1, 1, 0);
	cases[19] = txn_runs_tests_blocks(&free_run, &entry, 6);
	cases[20] = txn_runs_tests_one_key(&values, block_one, sizeof(block_one), &replayed);
	cases[21] = txn_runs_tests_one_key(names, (const uint8 *) "a", 1, &emptied);
	row = txn_runs_tests_entry_record(7, (const uint8 *) "row", 3, false, 0);
	cases[22] = txn_runs_tests_one_key(names, (const uint8 *) "a", 1, &row);
	std_vec_free_Checked(&row, 1, 1);
	row = txn_runs_tests_record(none.xid, (const uint8 *) "entry", 5, false, 0);
	cases[23] = txn_runs_tests_one_key(&free_run, block_one, sizeof(block_one), &row);
	std_vec_free_Checked(&row, 1, 1);
	for (size_t k = 0; k < lengthof(cases_names); k++)
		cases_names[k] = names;
	cases_names[3] = &redone;
	cases_names[7] = &free_run;
	cases_names[17] = &free_run;
	cases_names[19] = &free_run;
	cases_names[20] = &values;
	cases_names[23] = &free_run;
	for (size_t at = 0; at < lengthof(cases); at++)
	{
		StdBytes	sqlstate;
		StdBytes	wanted = {(const uint8 *) "XX001", 5};

		if (txn_runs_decode(cases[at].ptr, cases[at].len, cases_names[at], txn_runs_tests_sum(&cases[at]), &decoded,
							&error))
		{
			txn_runs_Decoded_drop(&decoded);
			STD_RESULT_EXPECT_FAILED(1939, 73, "called `Result::unwrap_err()` on an `Ok` value", NULL,
									 std_unit_Debug_fmt);
		}
		sqlstate = (StdBytes)
		{
			host_Error_sqlstate(&error), 5
		};
		if (sqlstate.len != wanted.len || memcmp(sqlstate.ptr, wanted.ptr, 5) != 0)
		{
			StdFmtArg	args[1] = {{&at, std_usize_Display_fmt}};
			StdFmtArguments message = {numbered, lengthof(numbered), args};

			std_assert_failed(RUST_FILE, 1940, 13, "==", &sqlstate, std_bytes_Debug_fmt, &wanted, std_bytes_Debug_fmt,
							  &message);
		}
		host_Error_drop(&error);
	}
	len = txn_runs_tests_keys_len(1942, 54, &good, names);
	STD_ASSERT_EQ(1942, 9, len == 1, &len, std_usize_Debug_fmt, &count_one, std_usize_Debug_fmt);
	refused = !txn_runs_decode(good.ptr, good.len, names, txn_runs_tests_sum(&good) ^ 1, &decoded, &error);
	if (refused)
		host_Error_drop(&error);
	else
		txn_runs_Decoded_drop(&decoded);
	STD_ASSERT(1943, 9, refused, "decode(&good, &NAMES, sum(&good) ^ 1).is_err()");
	{
		StdVec		cut_file = txn_runs_tests_blocks(&free_run, &entry, 5);
		TxnRunsTail counter = {.tag = TXN_RUNS_TAIL_COUNTER, .counter = TXN_RUNS_TESTS_CUT};
		StdTupleRef left;
		StdTupleRef right;

		if (!txn_runs_decode(cut_file.ptr, cut_file.len, &free_run, txn_runs_tests_sum(&cut_file), &decoded, &error))
			STD_RESULT_EXPECT_FAILED(1945, 51, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		left = STD_TUPLE(2, STD_ITEM(&decoded.tail, txn_runs_tests_Tail_Debug_fmt),
						 STD_ITEM(&decoded.keys.len, std_usize_Debug_fmt));
		right = STD_TUPLE(2, STD_ITEM(&counter, txn_runs_tests_Tail_Debug_fmt),
						  STD_ITEM(&count_one, std_usize_Debug_fmt));
		STD_ASSERT_EQ(1946, 9, decoded.tail.tag == TXN_RUNS_TAIL_COUNTER && decoded.tail.counter == TXN_RUNS_TESTS_CUT &&
					  decoded.keys.len == 1, &left, std_tuple_Debug_fmt, &right, std_tuple_Debug_fmt);
		txn_runs_Decoded_drop(&decoded);
		std_vec_free_Checked(&cut_file, 1, 1);
	}
	row = txn_runs_tests_entry_record(7, (const uint8 *) "one", 3, true, 9);
	given = txn_runs_tests_one_key(&values, block_one, sizeof(block_one), &row);
	std_vec_free_Checked(&row, 1, 1);
	len = txn_runs_tests_keys_len(1948, 57, &given, &values);
	STD_ASSERT_EQ(1948, 9, len == 1, &len, std_usize_Debug_fmt, &count_one, std_usize_Debug_fmt);
	std_vec_free_Checked(&given, 1, 1);
	std_u32_to_be_bytes(5, block_five);
	row = txn_runs_tests_entry_record(none.xid, (const uint8 *) "entry", 5, true, 9);
	given = txn_runs_tests_one_key(&free_run, block_five, sizeof(block_five), &row);
	std_vec_free_Checked(&row, 1, 1);
	len = txn_runs_tests_keys_len(1950, 55, &given, &free_run);
	STD_ASSERT_EQ(1950, 9, len == 1, &len, std_usize_Debug_fmt, &count_one, std_usize_Debug_fmt);
	std_vec_free_Checked(&given, 1, 1);
	txn_runs_tests_records_free(cases, lengthof(cases));
	txn_runs_tests_records_free(&free_key, 1);
	txn_runs_tests_records_free(&free_head, 1);
	txn_runs_tests_records_free(&emptied, 1);
	txn_runs_tests_records_free(&counted, 1);
	txn_runs_tests_records_free(&entry, 1);
	txn_runs_tests_records_free(&longer, 1);
	txn_runs_tests_records_free(&empty, 1);
	txn_runs_tests_records_free(&replayed, 1);
	txn_runs_tests_records_free(&unknown, 1);
	txn_runs_tests_records_free(&built, 1);
	txn_runs_tests_records_free(&key, 1);
	txn_runs_tests_records_free(&head, 1);
	txn_runs_tests_records_free(&good, 1);
	txn_runs_tests_records_free(&one, 1);
}

/* txn_runs_tests_key_count_t is a run's key and its count of versions, as the trees case reads them. */
typedef struct txn_runs_tests_key_count_t
{
	StdBytes	key;
	size_t		count;
} txn_runs_tests_key_count_t;

/* txn_runs_tests_key_count_Debug_fmt is a key and its count's Debug, the tuple (key, count). */
static bool
txn_runs_tests_key_count_Debug_fmt(const void *value, StdFormatter *f)
{
	const txn_runs_tests_key_count_t *read = value;
	StdTupleRef tuple = STD_TUPLE(2, STD_ITEM(&read->key, std_bytes_Debug_fmt),
								  STD_ITEM(&read->count, std_usize_Debug_fmt));

	return std_tuple_Debug_fmt(&tuple, f);
}

/* txn_runs_tests_key_counts_Debug_fmt is a Vec<(&[u8], usize)>'s Debug. */
static bool
txn_runs_tests_key_counts_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *v = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t k = 0; k < v->len; k++)
		std_DebugList_entry(&list, &STD_VEC_AT(v, txn_runs_tests_key_count_t, k), txn_runs_tests_key_count_Debug_fmt);
	return std_DebugList_finish(&list);
}

/* txn_runs_tests_redo_files_is asserts at its place that a run's redo point and count of files are a pair, with a message when one is given. */
static void
txn_runs_tests_redo_files_is(uint32 line, uint32 col, const RepoRuns *runs, HostLsn redo, const StdFmtArguments *message)
{
	size_t		one = 1;
	StdTupleRef left = STD_TUPLE(2, STD_ITEM(&runs->redo, std_u64_Debug_fmt),
								 STD_ITEM(&runs->files.len, std_usize_Debug_fmt));
	StdTupleRef right = STD_TUPLE(2, STD_ITEM(&redo, std_u64_Debug_fmt), STD_ITEM(&one, std_usize_Debug_fmt));

	if (runs->redo != redo || runs->files.len != 1)
		std_assert_failed(RUST_FILE, line, col, "==", &left, std_tuple_Debug_fmt, &right, std_tuple_Debug_fmt,
						  message);
}

/* txn_runs_tests_repository makes a test's repository of a database and a reader of the shared state, at the places a case unwraps them. */
static void
txn_runs_tests_repository(HostIoIo *io, uint32 database, uint32 create_line, uint32 create_col, uint32 read_line,
						  uint32 read_col, RepoRepo *repo, HostSharedReader *reading)
{
	ChunksErrorError failed;
	HostError	error;

	if (!repo_Repo_create(io, database, HOST_PG_DEFAULT, repo, &failed))
		STD_RESULT_EXPECT_FAILED(create_line, create_col, "called `Result::unwrap()` on an `Err` value", &failed,
								 chunks_error_Error_Debug_fmt);
	if (!host_shared_read(io, reading, &error))
		STD_RESULT_EXPECT_FAILED(read_line, read_col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
}

/* txn_runs_tests_stated returns the state of a test's storage of rows, made in its repository, at the place a case unwraps it. */
static HostSharedHeld
txn_runs_tests_stated(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading, const HostStorage *storage,
					  uint32 line, uint32 col)
{
	HostSharedHeld held;
	HostError	error;

	if (!txn_state_state(io, repo, reading, storage, TXN_STATE_PERSISTENCE_PERMANENT, false, NULL, &held, &error))
		STD_RESULT_EXPECT_FAILED(line, col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	return held;
}

/* txn_runs_tests_pushed pushes a put of "v" by xid 7 with LSN 9 to a key of a state's memtable, at the place a case unwraps it. */
static void
txn_runs_tests_pushed(HostIoIo *io, HostSharedRecord state, const char *key, uint32 line, uint32 col)
{
	TxnMemtableOp op = txn_memtable_testing_put((const uint8 *) "v", 1);
	TxnMemtableVersionChange change = {.writer = {.xid = 7}, .op = &op, .lock = TXN_MEMTABLE_LOCK_MODE_UPDATE,
	.shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND}, .has_lsn = true, .lsn = 9};
	TxnMemtableVersionHandle handle;
	HostError	error;

	if (!txn_memtable_table_push(io, state, (const uint8 *) key, strlen(key), &change, &handle, &error))
		STD_RESULT_EXPECT_FAILED(line, col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	txn_memtable_Op_drop(&op);
}

/*
 * The writer's case: a checkpoint's runs of a repository hold a file for
 * each tree with a version, its rows here, whose length is the manifest
 * entry's and whose keys read back in byte order, each with its versions; a
 * tree with none writes no file.
 */
void
test_txn_runs_tests_a_checkpoints_runs_hold_each_trees_versions(void)
{
	static const char *const pushes[] = {"b", "a", "b"};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostStorage storage = {.tablespace = HOST_PG_DEFAULT, .database = 9410, .relation = 16384};
	HostLsn		redo = UINT64CONST(0x100000a28);
	TxnRunsNames names = TXN_RUNS_TESTS_NAMES;
	txn_runs_tests_key_count_t wanted[2] = {{{(const uint8 *) "a", 1}, 1}, {{(const uint8 *) "b", 1}, 2}};
	StdVec		wanted_vec = {wanted, 2, 2};
	RepoRepo	repo;
	HostSharedReader reading;
	HostSharedHeld held;
	RepoRuns	runs;
	StdPathBuf	dir;
	StdPathBuf	path;
	StdVec		bytes;
	StdVec		read;
	TxnRunsDecoded decoded;
	ChunksErrorError failed;
	HostError	error;
	bool		eq;

	txn_runs_tests_repository(io, 9410, 1962, 65, 1963, 46, &repo, &reading);
	held = txn_runs_tests_stated(io, &repo, &reading, &storage, 1965, 94);
	for (size_t k = 0; k < lengthof(pushes); k++)
		txn_runs_tests_pushed(io, host_shared_Held_record(&held), pushes[k], 1981, 58);
	if (!txn_runs_write(io, 9410, HOST_PG_DEFAULT, redo, 0, NULL, 0, &runs, &error))
		STD_RESULT_EXPECT_FAILED(1984, 68, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	txn_runs_tests_redo_files_is(1985, 9, &runs, redo, NULL);
	txn_runs_directory(9410, HOST_PG_DEFAULT, redo, &dir);
	std_path_join(dir.bytes.ptr, dir.bytes.len, "rows-1663-16384", 15, &path);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	if (!chunks_file_read_file(HOST_IO_IO_READ_DOORS(io), path.bytes.ptr, path.bytes.len, &bytes, &failed))
		STD_RESULT_EXPECT_FAILED(1987, 56, "called `Result::unwrap()` on an `Err` value", &failed,
								 chunks_error_Error_Debug_fmt);
	{
		uint64		len = bytes.len;
		uint64		file_len = STD_VEC_AT(&runs.files, RepoRunFile, 0).len;

		STD_ASSERT_EQ(1988, 9, len == file_len, &len, std_u64_Debug_fmt, &file_len, std_u64_Debug_fmt);
	}
	names.database = 9410;
	names.redo = redo;
	if (!txn_runs_decode(bytes.ptr, bytes.len, &names, STD_VEC_AT(&runs.files, RepoRunFile, 0).hash, &decoded,
						 &error))
		STD_RESULT_EXPECT_FAILED(1991, 14, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	std_vec_with_capacity_Checked(&read, decoded.keys.len, sizeof(txn_runs_tests_key_count_t),
								  _Alignof(txn_runs_tests_key_count_t));
	for (size_t k = 0; k < decoded.keys.len; k++)
	{
		const TxnRunsKey *key = &STD_VEC_AT(&decoded.keys, TxnRunsKey, k);
		txn_runs_tests_key_count_t entry = {key->key, key->versions.len};

		std_vec_push_Checked(&read, &entry, sizeof(entry), _Alignof(txn_runs_tests_key_count_t));
	}
	eq = read.len == 2;
	for (size_t k = 0; eq && k < 2; k++)
	{
		const txn_runs_tests_key_count_t *got = &STD_VEC_AT(&read, txn_runs_tests_key_count_t, k);

		eq = got->count == wanted[k].count && got->key.len == wanted[k].key.len &&
			memcmp(got->key.ptr, wanted[k].key.ptr, got->key.len) == 0;
	}
	STD_ASSERT_EQ(1996, 9, eq, &read, txn_runs_tests_key_counts_Debug_fmt, &wanted_vec,
				  txn_runs_tests_key_counts_Debug_fmt);
	std_vec_free_Checked(&read, sizeof(txn_runs_tests_key_count_t), _Alignof(txn_runs_tests_key_count_t));
	txn_runs_Decoded_drop(&decoded);
	std_vec_free_Checked(&bytes, 1, 1);
	std_vec_free_Checked(&path.bytes, 1, 1);
	repo_Runs_drop(&runs);
	host_shared_Held_drop(&held);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	simc_TestIo_drop(&test_io);
}

/* txn_runs_tests_based is the saving case's gathering: table 16385's base record, folded at 5, as the one edit. */
static bool
txn_runs_tests_based(void *captures, HostIoIo *io, const RepoRead *read, StdVec *result, ChunksErrorError *error)
{
	RepoGathered edit = {.tag = REPO_GATHERED_BASE};

	edit.u.base.storage = (RepoStorage)
	{
		.tablespace = HOST_PG_DEFAULT, .relation = 16385
	};
	std_slice_to_vec_Checked(&edit.u.base.record.bytes, (const uint8 *) "base", 4, 1, 1);
	std_vec_new(&edit.u.base.record.addresses, sizeof(chunks_TupleU32Locator_t), _Alignof(chunks_TupleU32Locator_t));
	edit.u.base.folded = 5;
	std_vec_with_capacity_Checked(result, 1, sizeof(RepoGathered), _Alignof(RepoGathered));
	std_vec_push_Checked(result, &edit, sizeof(edit), _Alignof(RepoGathered));
	return true;
}

/* txn_runs_tests_saved is the saving case's marks, which mark nothing. */
static bool
txn_runs_tests_saved(void *captures, HostIoIo *io, ChunksErrorError *error)
{
	return true;
}

/* txn_runs_tests_strings_Debug_fmt is a Vec<String>'s Debug. */
static bool
txn_runs_tests_strings_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *v = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t k = 0; k < v->len; k++)
		std_DebugList_entry(&list, &STD_VEC_AT(v, StdString, k), std_String_Debug_fmt);
	return std_DebugList_finish(&list);
}

/* txn_runs_tests_bools_Debug_fmt is a Vec<bool>'s Debug. */
static bool
txn_runs_tests_bools_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *v = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t k = 0; k < v->len; k++)
		std_DebugList_entry(&list, &STD_VEC_AT(v, bool, k), std_bool_Debug_fmt);
	return std_DebugList_finish(&list);
}

/* txn_runs_tests_Manifest_drop_glue is an Arc<Manifest>'s value's drop glue. */
static void
txn_runs_tests_Manifest_drop_glue(void *manifest)
{
	repo_Manifest_drop(manifest);
}

/*
 * Caller (1)'s case: each checkpoint writes the repository's runs at its
 * redo point and saves the repository with them, so the manifest names that
 * checkpoint's runs, and it removes every run directory neither generation of
 * the manifest names, keeping the previous generation's (K = 2).
 */
void
test_txn_runs_tests_a_checkpoint_saves_its_runs_and_removes_the_unnamed(void)
{
	static const StdFmtPiece no_runs[] = {STD_FMT_LIT("the packs wrote no runs file: "), STD_FMT_ARG(0)};
	static const StdFmtPiece numbered[] = {STD_FMT_ARG(0)};
	static const HostLsn points[] = {UINT64CONST(0x100000a28), UINT64CONST(0x100001a28), UINT64CONST(0x100002a28)};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostStorage storage = {.tablespace = HOST_PG_DEFAULT, .database = 9411, .relation = 16384};
	bool		kept[3];
	bool		wanted[3] = {false, true, true};
	StdVec		kept_vec = {kept, 3, 3};
	StdVec		wanted_vec = {wanted, 3, 3};
	RepoRepo	repo;
	HostSharedReader reading;
	HostSharedHeld held;
	StdPathBuf	dir;
	StdVec		listed;
	ChunksErrorError failed;
	HostError	error;
	bool		runs_named = false;

	txn_runs_tests_repository(io, 9411, 2008, 59, 2009, 46, &repo, &reading);
	held = txn_runs_tests_stated(io, &repo, &reading, &storage, 2011, 94);
	txn_runs_tests_pushed(io, host_shared_Held_record(&held), "k", 2026, 55);
	if (!repo_Repo_save(&repo, io, &reading, false, NULL, txn_runs_tests_based, NULL, txn_runs_tests_saved, NULL,
						&failed))
		STD_RESULT_EXPECT_FAILED(2032, 58, "called `Result::unwrap()` on an `Err` value", &failed,
								 chunks_error_Error_Debug_fmt);
	if (!repo_Repo_close(&repo, io, &reading, &failed))
		STD_RESULT_EXPECT_FAILED(2033, 34, "called `Result::unwrap()` on an `Err` value", &failed,
								 chunks_error_Error_Debug_fmt);
	dir = host_tablespace_dir(9411, HOST_PG_DEFAULT);
	if (!host_list_dir(HOST_IO_IO_READ_DOORS(io), dir.bytes.ptr, dir.bytes.len, &listed, &error))
		STD_RESULT_EXPECT_FAILED(2034, 87, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	for (size_t k = 0; !runs_named && k < listed.len; k++)
	{
		const StdString *name = &STD_VEC_AT(&listed, StdString, k);

		runs_named = name->len == 4 && memcmp(name->ptr, "runs", 4) == 0;
	}
	if (!runs_named)
	{
		StdFmtArg	args[1] = {{&listed, txn_runs_tests_strings_Debug_fmt}};
		StdFmtArguments message = {no_runs, lengthof(no_runs), args};

		std_panic_fmt(RUST_FILE, 2035, 9, &message);
	}
	for (size_t at = 0; at < lengthof(points); at++)
	{
		StdFmtArg	args[1] = {{&at, std_usize_Display_fmt}};
		StdFmtArguments message = {numbered, lengthof(numbered), args};
		HostLsn		redo = points[at];
		RepoRepo	opened;
		StdArcInner *manifest;
		const RepoManifest *named;

		simc_checkpointed(redo - 1);
		if (!txn_runs_checkpoint(io, redo, &error))
			STD_RESULT_EXPECT_FAILED(2039, 34, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		if (!repo_Repo_open(io, 9411, HOST_PG_DEFAULT, &opened, &failed))
			STD_RESULT_EXPECT_FAILED(2040, 62, "called `Result::unwrap()` on an `Err` value", &failed,
									 chunks_error_Error_Debug_fmt);
		if (!repo_Repo_manifest(&opened, io, &manifest, &failed))
			STD_RESULT_EXPECT_FAILED(2040, 84, "called `Result::unwrap()` on an `Err` value", &failed,
									 chunks_error_Error_Debug_fmt);
		named = std_arc_Deref_deref(manifest, _Alignof(RepoManifest));
		if (!named->has_runs)
			STD_OPTION_UNWRAP_FAILED(2040, 106);
		txn_runs_tests_redo_files_is(2041, 13, &named->runs, redo, &message);
		std_arc_drop(manifest, sizeof(RepoManifest), _Alignof(RepoManifest), txn_runs_tests_Manifest_drop_glue);
		repo_Repo_drop(&opened);
	}
	for (size_t at = 0; at < lengthof(points); at++)
	{
		StdPathBuf	run_dir;

		txn_runs_directory(9411, HOST_PG_DEFAULT, points[at], &run_dir);
		if (!chunks_file_dir_there(HOST_IO_IO_READ_DOORS(io), run_dir.bytes.ptr, run_dir.bytes.len, &kept[at],
								   &failed))
			STD_RESULT_EXPECT_FAILED(2044, 73, "called `Result::unwrap()` on an `Err` value", &failed,
									 chunks_error_Error_Debug_fmt);
		std_vec_free_Checked(&run_dir.bytes, 1, 1);
	}
	STD_ASSERT_EQ(2045, 9, kept[0] == wanted[0] && kept[1] == wanted[1] && kept[2] == wanted[2], &kept_vec,
				  txn_runs_tests_bools_Debug_fmt, &wanted_vec, txn_runs_tests_bools_Debug_fmt);
	std_vec_drop_Checked(&listed, sizeof(StdString), _Alignof(StdString), std_String_drop);
	host_shared_Held_drop(&held);
	host_shared_Reader_drop(&reading);
	simc_TestIo_drop(&test_io);
}

/* txn_runs_tests_chains_each is chains' each closure: it pushes a key with its versions' records. */
static bool
txn_runs_tests_chains_each(void *captures, HostIoIo *io, const uint8 *key, size_t key_len, const StdVec *values,
						   size_t values_len, HostError *error)
{
	txn_runs_tests_chain_push(captures, key, key_len, values, values_len);
	return true;
}

/* txn_runs_tests_chains returns every key of a tree with its versions' records, newest first, as a walk hands them out. */
static StdVec
txn_runs_tests_chains(const TxnMemtableTableTreeOps *tree, HostIoIo *io, HostSharedRecord owner)
{
	StdVec		chains;
	HostError	error;

	std_vec_new(&chains, sizeof(txn_runs_tests_chain_t), _Alignof(txn_runs_tests_chain_t));
	if (!txn_memtable_table_walked(tree, io, owner, txn_runs_tests_chains_each, &chains, NULL, &error))
		STD_RESULT_EXPECT_FAILED(1831, 45, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	return chains;
}

/* The map's takes that keep nothing, which map.c defines and the loader case takes. */
#define TXN_MEMTABLE_MAP_RS Unit
#define TXN_MEMTABLE_MAP_RETURNS bool
#define TXN_MEMTABLE_MAP_RESULT_PARAMS , HostError *error
#define TXN_MEMTABLE_MAP_TS Unit
#define TXN_MEMTABLE_MAP_KEPT_OUT
#define TXN_MEMTABLE_MAP_KEPT_IN
#define TXN_MEMTABLE_MAP_DECLARE
#include "doltgres/txn/memtable/map_template.h"

/* txn_runs_tests_put_step is the loader case's take's step: the take's value put, whatever the key holds. */
static TxnMemtableMapStep
txn_runs_tests_put_step(void *captures, bool has_current, const uint8 *current, size_t current_len, StdVec *out)
{
	const StdBytes *value = captures;

	std_vec_extend_from_slice_Checked(out, value->ptr, value->len, 1, 1);
	return TXN_MEMTABLE_MAP_STEP_PUT;
}

/* txn_runs_tests_nothing is the loader case's take's then, which does nothing. */
static void
txn_runs_tests_nothing(void *captures)
{
}

/* txn_runs_tests_loaded_captures_t is the loader case's first prepare's captures: the free space entry and the row's change. */
typedef struct txn_runs_tests_loaded_captures_t
{
	const StdVec *free;
	const TxnMemtableVersionChange *change;
} txn_runs_tests_loaded_captures_t;

/* txn_runs_tests_rowed finishes a prepare's row: its tuple id and the change at its key, as the cases' prepares end. */
static bool
txn_runs_tests_rowed(HostIoIo *io, TxnMemtableTablePreparedState *prepared, HostRow tid,
					 const TxnMemtableVersionChange *change, HostError *error)
{
	TxnStatusRoomed roomed = {0};
	TxnMemtableVersionHandle handle;

	return txn_memtable_tuples_finish(HOST_IO_IO_TAKES_SHORT(io), prepared, tid, error) &&
		txn_memtable_table_finish(HOST_IO_IO_TAKES_SHORT(io), prepared, change, false, roomed, &handle, error);
}

/* txn_runs_tests_first_prepared is the loader case's first prepare: the free space map's and the value map's entries, then the row. */
static bool
txn_runs_tests_first_prepared(void *captures, HostIoIo *io, TxnMemtableTablePreparedState *prepared, HostError *error)
{
	const txn_runs_tests_loaded_captures_t *c = captures;
	StdBytes	values[2] = {{c->free->ptr, c->free->len}, {(const uint8 *) "value", 5}};
	HostRow		tid = {5};

	for (size_t take = 0; take < lengthof(values); take++)
	{
		TxnMemtableMapTaken taken;

		if (!txn_memtable_map_finish_Unit(HOST_IO_IO_TAKES_SHORT(io), prepared, take, c->change->writer, false, 0,
										  false, NULL, 0, txn_runs_tests_put_step, &values[take], NULL,
										  txn_runs_tests_nothing, NULL, NULL, &taken, error))
			return false;
	}
	return txn_runs_tests_rowed(io, prepared, tid, c->change, error);
}

/* txn_runs_tests_second_prepared is the loader case's second prepare: the second storage's row. */
static bool
txn_runs_tests_second_prepared(void *captures, HostIoIo *io, TxnMemtableTablePreparedState *prepared,
							   HostError *error)
{
	HostRow		tid = {5};

	return txn_runs_tests_rowed(io, prepared, tid, captures, error);
}

/* txn_runs_tests_chunk_captures_t is the loader case's chunk's prepare's captures: the chunk and its LSN. */
typedef struct txn_runs_tests_chunk_captures_t
{
	StdVec	   *chunk;
	bool		has_lsn;
	uint64		lsn;
} txn_runs_tests_chunk_captures_t;

/* txn_runs_tests_chunk_prepared is the loader case's chunk's prepare: the chunk by no transaction, with its LSN. */
static bool
txn_runs_tests_chunk_prepared(void *captures, HostIoIo *io, TxnMemtableTablePreparedBranchDeltas *prepared,
							  HostError *error)
{
	const txn_runs_tests_chunk_captures_t *c = captures;
	TxnMemtableVersionWriter none = TXN_MEMTABLE_VERSION_NO_TRANSACTION;
	TxnMemtableVersionHandle handle;

	return txn_memtable_branch_finish(io, prepared, none, c->chunk, c->has_lsn, c->lsn, &handle, error);
}

/* txn_runs_tests_trees_t is the loader case's five trees' chains: the first storage's rows and values, the second's rows, the repository's chunks, and the first's free space. */
typedef struct txn_runs_tests_trees_t
{
	StdVec		rows;
	StdVec		values;
	StdVec		second;
	StdVec		chunks;
	StdVec		free;
} txn_runs_tests_trees_t;

/* txn_runs_tests_trees reads the loader case's five trees' chains. */
static txn_runs_tests_trees_t
txn_runs_tests_trees(HostIoIo *io, HostSharedRecord first, HostSharedRecord second, HostSharedRecord deltas)
{
	txn_runs_tests_trees_t trees;

	trees.rows = txn_runs_tests_chains(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, io, first);
	trees.values = txn_runs_tests_chains(&TXN_MEMTABLE_MAP_VALUES_TREE_OPS, io, first);
	trees.second = txn_runs_tests_chains(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, io, second);
	trees.chunks = txn_runs_tests_chains(&TXN_MEMTABLE_BRANCH_CHUNKS_TREE_OPS, io, deltas);
	trees.free = txn_runs_tests_chains(&TXN_MEMTABLE_MAP_FREE_TREE_OPS, io, first);
	return trees;
}

/* txn_runs_tests_trees_free frees the loader case's five trees' chains. */
static void
txn_runs_tests_trees_free(txn_runs_tests_trees_t *trees)
{
	txn_runs_tests_chains_free(&trees->rows);
	txn_runs_tests_chains_free(&trees->values);
	txn_runs_tests_chains_free(&trees->second);
	txn_runs_tests_chains_free(&trees->chunks);
	txn_runs_tests_chains_free(&trees->free);
}

/* txn_runs_tests_helds returns the states of the loader case's two storages, at the place a case unwraps them. */
static void
txn_runs_tests_helds(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading, const HostStorage *storages,
					 uint32 line, uint32 col, HostSharedHeld *helds)
{
	for (size_t k = 0; k < 2; k++)
	{
		HostError	error;

		if (!txn_state_state(io, repo, reading, &storages[k], TXN_STATE_PERSISTENCE_PERMANENT, false, NULL, &helds[k],
							 &error))
			STD_RESULT_EXPECT_FAILED(line, col, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
	}
}

/* txn_runs_tests_deltas returns the loader case's repository's working state's delta, at the place a case unwraps it. */
static HostSharedHeld
txn_runs_tests_deltas(HostIoIo *io, uint32 line, uint32 col)
{
	HostSharedHeld deltas;
	HostError	error;

	if (!txn_memtable_branch_deltas(io, 9412, HOST_PG_DEFAULT, &deltas, &error))
		STD_RESULT_EXPECT_FAILED(line, col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	return deltas;
}

/* txn_runs_tests_entry_option_t is an Option<Option<Vec<u8>>>, as map::newest answers it. */
typedef struct txn_runs_tests_entry_option_t
{
	bool		some;
	TxnMemtableMapEntry entry;
} txn_runs_tests_entry_option_t;

/* txn_runs_tests_entry_Debug_fmt is an Option<Vec<u8>>'s Debug, a map entry's. */
static bool
txn_runs_tests_entry_Debug_fmt(const void *value, StdFormatter *f)
{
	const TxnMemtableMapEntry *entry = value;
	StdBytes	bytes = {entry->value.ptr, entry->value.len};
	StdOptionRef option = {entry->has_value, STD_ITEM(&bytes, std_bytes_Debug_fmt)};

	return std_option_Debug_fmt(&option, f);
}

/* txn_runs_tests_entry_option_Debug_fmt is an Option<Option<Vec<u8>>>'s Debug. */
static bool
txn_runs_tests_entry_option_Debug_fmt(const void *value, StdFormatter *f)
{
	const txn_runs_tests_entry_option_t *read = value;
	StdOptionRef option = {read->some, STD_ITEM(&read->entry, txn_runs_tests_entry_Debug_fmt)};

	return std_option_Debug_fmt(&option, f);
}

/* txn_runs_tests_u64s_Debug_fmt is a [u64]'s Debug. */
static bool
txn_runs_tests_u64s_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *v = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t k = 0; k < v->len; k++)
		std_DebugList_entry(&list, &STD_VEC_AT(v, uint64, k), std_u64_Debug_fmt);
	return std_DebugList_finish(&list);
}

/*
 * The loader's case: two storages' deltas and their repository's working
 * state's chunks, written to a checkpoint's runs and then gone from the
 * delta, as a restart leaves it, load back record for record: a key's two
 * versions in their order with their writers, locks, and LSNs, a value map's
 * entry, and a chunk's unlogged and logged copies. The free space map's entry
 * of the block the counter starts on, below the cut, loads after the
 * counter's move, so its versions keep their writers above the move's, and
 * reads as written; each row's tuple id names its key, and each storage's
 * counter is what its runs gave it: the one its free space run was cut at for
 * one, and the next past its row's tuple id for the other, which has no free
 * space map.
 */
void
test_txn_runs_tests_a_checkpoints_runs_load_back_into_the_delta(void)
{
	static const uint16 blocks[] = {2, 3};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostStorage storages[2] = {{.tablespace = HOST_PG_DEFAULT, .database = 9412, .relation = 16384},
	{.tablespace = HOST_PG_DEFAULT, .database = 9412, .relation = 16385}};
	TxnFreeSpaceFree made = txn_free_space_Free_new(100, blocks, lengthof(blocks), 291);
	uint8		address[TXN_MEMTABLE_BRANCH_ADDRESS];
	TxnInsertPage page = {.bytes = 8160, .most = 291};
	RepoRepo	repo;
	HostSharedReader reading;
	HostSharedHeld helds[2];
	HostSharedHeld deltas;
	txn_runs_tests_trees_t before;
	txn_runs_tests_trees_t after;
	StdVec		free;
	HostError	error;

	memset(address, 6, sizeof(address));
	txn_free_space_Free_encode(&made, &free);
	txn_runs_tests_repository(io, 9412, 2061, 59, 2062, 46, &repo, &reading);
	{
		TxnMemtableOp op = txn_memtable_testing_put((const uint8 *) "v", 1);
		TxnMemtableVersionChange change = {.writer = {.xid = 7}, .op = &op, .lock = TXN_MEMTABLE_LOCK_MODE_UPDATE,
		.shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND}, .has_lsn = true, .lsn = 0x0a28};
		TxnMemtableOp deleted = {.tag = TXN_MEMTABLE_OP_DELETE};
		TxnMemtableVersionChange locked = {.writer = {.xid = 8, .has_top = true, .top = 6, .command = 2},
		.op = &deleted, .lock = TXN_MEMTABLE_LOCK_MODE_NO_KEY_UPDATE,
		.shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND}, .has_lsn = true, .lsn = 0x0b00};
		txn_runs_tests_loaded_captures_t c = {.free = &free, .change = &change};
		TxnMemtableVersionHandle handle;
		size_t		need;
		txn_TupleRefSliceU8Usize_t rows[1];
		TxnMemtableMapTake takes[2];
		HostSharedRecord first;
		HostSharedRecord second;
		const struct
		{
			bool		has_lsn;
			uint64		lsn;
		}			chunks[2] = {{false, 0}, {true, 0x0c00}};

		op.u.put.tuple.f0 = 5;
		txn_runs_tests_helds(io, &repo, &reading, storages, 2069, 106, helds);
		first = host_shared_Held_record(&helds[0]);
		second = host_shared_Held_record(&helds[1]);
		need = txn_memtable_version_encoded(&change.writer, change.op, change.shown, change.has_lsn, change.lsn);
		rows[0] = (txn_TupleRefSliceU8Usize_t)
		{
			{
				(const uint8 *) "k", 1
			}, need
		};
		takes[0] = (TxnMemtableMapTake)
		{
			.state = first, .map = TXN_MEMTABLE_MAP_MAP_FREE, .key = 0, .most = free.len
		};
		takes[1] = (TxnMemtableMapTake)
		{
			.state = first, .map = TXN_MEMTABLE_MAP_MAP_VALUES, .key = 16400, .most = 5
		};
		if (!txn_memtable_table_prepare_Unit(io, first, rows, 1, takes, 2, true, txn_runs_tests_first_prepared, &c, NULL,
											 &error))
			STD_RESULT_EXPECT_FAILED(2096, 14, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		if (!txn_memtable_table_push(io, first, (const uint8 *) "k", 1, &locked, &handle, &error))
			STD_RESULT_EXPECT_FAILED(2104, 51, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		txn_state_moved(first, 1, (4 << 16) | 1);
		if (!txn_memtable_table_prepare_Unit(io, second, rows, 1, NULL, 0, true, txn_runs_tests_second_prepared, &change,
											 NULL, &error))
			STD_RESULT_EXPECT_FAILED(2110, 14, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		deltas = txn_runs_tests_deltas(io, 2111, 69);
		for (size_t k = 0; k < lengthof(chunks); k++)
		{
			StdVec		chunk;
			txn_runs_tests_chunk_captures_t cc = {.chunk = &chunk, .has_lsn = chunks[k].has_lsn,
			.lsn = chunks[k].lsn};

			std_slice_to_vec_Checked(&chunk, (const uint8 *) "chunk", 5, 1, 1);
			if (!txn_memtable_branch_prepare(io, host_shared_Held_record(&deltas), address, sizeof(address),
											 txn_memtable_version_largest(chunk.len), txn_runs_tests_chunk_prepared, &cc,
											 NULL, &error))
				STD_RESULT_EXPECT_FAILED(2118, 18, "called `Result::unwrap()` on an `Err` value", &error,
										 host_Error_Debug_fmt);
			std_vec_free_Checked(&chunk, 1, 1);
		}
		before = txn_runs_tests_trees(io, first, second, host_shared_Held_record(&deltas));
		host_shared_Held_drop(&deltas);
		host_shared_Held_drop(&helds[1]);
		host_shared_Held_drop(&helds[0]);
		txn_memtable_Op_drop(&op);
	}
	{
		size_t		first_versions;
		size_t		chunk_versions;
		size_t		two = 2;
		StdTupleRef left = STD_TUPLE(2, STD_ITEM(&first_versions, std_usize_Debug_fmt),
									 STD_ITEM(&chunk_versions, std_usize_Debug_fmt));
		StdTupleRef right = STD_TUPLE(2, STD_ITEM(&two, std_usize_Debug_fmt), STD_ITEM(&two, std_usize_Debug_fmt));

		std_index_check(0, before.rows.len, RUST_FILE, 2128, 21);
		first_versions = STD_VEC_AT(&before.rows, txn_runs_tests_chain_t, 0).records.len;
		std_index_check(0, before.chunks.len, RUST_FILE, 2128, 42);
		chunk_versions = STD_VEC_AT(&before.chunks, txn_runs_tests_chain_t, 0).records.len;
		STD_ASSERT_EQ(2128, 9, first_versions == 2 && chunk_versions == 2, &left, std_tuple_Debug_fmt, &right,
					  std_tuple_Debug_fmt);
	}
	simc_checkpointed(UINT64CONST(0x100000a27));
	if (!txn_runs_checkpoint(io, UINT64CONST(0x100000a28), &error))
		STD_RESULT_EXPECT_FAILED(2130, 39, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	for (size_t k = 0; k < lengthof(storages); k++)
		if (!txn_state_unlinked(io, &storages[k], &error))
			STD_RESULT_EXPECT_FAILED(2132, 42, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
	if (!txn_memtable_branch_dropped(io, 9412, &error))
		STD_RESULT_EXPECT_FAILED(2134, 33, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!txn_runs_loaded(io, &page, &error))
		STD_RESULT_EXPECT_FAILED(2135, 54, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	txn_runs_tests_helds(io, &repo, &reading, storages, 2137, 110, helds);
	deltas = txn_runs_tests_deltas(io, 2139, 65);
	after = txn_runs_tests_trees(io, host_shared_Held_record(&helds[0]), host_shared_Held_record(&helds[1]),
								 host_shared_Held_record(&deltas));
	{
		StdTupleRef left = STD_TUPLE(4, STD_ITEM(&after.rows, txn_runs_tests_chains_Debug_fmt),
									 STD_ITEM(&after.values, txn_runs_tests_chains_Debug_fmt),
									 STD_ITEM(&after.second, txn_runs_tests_chains_Debug_fmt),
									 STD_ITEM(&after.chunks, txn_runs_tests_chains_Debug_fmt));
		StdTupleRef right = STD_TUPLE(4, STD_ITEM(&before.rows, txn_runs_tests_chains_Debug_fmt),
									  STD_ITEM(&before.values, txn_runs_tests_chains_Debug_fmt),
									  STD_ITEM(&before.second, txn_runs_tests_chains_Debug_fmt),
									  STD_ITEM(&before.chunks, txn_runs_tests_chains_Debug_fmt));

		STD_ASSERT_EQ(2147, 9, txn_runs_tests_chains_eq(&after.rows, &before.rows) &&
					  txn_runs_tests_chains_eq(&after.values, &before.values) &&
					  txn_runs_tests_chains_eq(&after.second, &before.second) &&
					  txn_runs_tests_chains_eq(&after.chunks, &before.chunks), &left, std_tuple_Debug_fmt, &right,
					  std_tuple_Debug_fmt);
	}
	{
		const txn_runs_tests_chain_t *free_after;
		const txn_runs_tests_chain_t *free_before;
		StdBytes	after_key;
		StdBytes	before_key;
		size_t		one = 1;
		StdTupleRef left = STD_TUPLE(2, STD_ITEM(&after.free.len, std_usize_Debug_fmt),
									 STD_ITEM(&after_key, std_bytes_Debug_fmt));
		StdTupleRef right = STD_TUPLE(2, STD_ITEM(&one, std_usize_Debug_fmt),
									  STD_ITEM(&before_key, std_bytes_Debug_fmt));
		StdVec		prefix;

		std_index_check(0, after.free.len, RUST_FILE, 2148, 43);
		free_after = &STD_VEC_AT(&after.free, txn_runs_tests_chain_t, 0);
		std_index_check(0, before.free.len, RUST_FILE, 2148, 56);
		free_before = &STD_VEC_AT(&before.free, txn_runs_tests_chain_t, 0);
		after_key = (StdBytes)
		{
			free_after->key.ptr, free_after->key.len
		};
		before_key = (StdBytes)
		{
			free_before->key.ptr, free_before->key.len
		};
		prefix = (StdVec)
		{
			free_after->records.ptr, Min(free_after->records.len, free_before->records.len),
				Min(free_after->records.len, free_before->records.len)
		};
		STD_ASSERT_EQ(2149, 9, after.free.len == 1 && after_key.len == before_key.len &&
					  memcmp(after_key.ptr, before_key.ptr, after_key.len) == 0, &left, std_tuple_Debug_fmt, &right,
					  std_tuple_Debug_fmt);
		STD_ASSERT_MSG(2150, 9, free_after->records.len >= free_before->records.len &&
					   txn_memtable_table_tests_keys_eq(&prefix, &free_before->records),
					   "the loader's move went over the run's entry");
	}
	{
		txn_runs_tests_entry_option_t newest;
		txn_runs_tests_entry_option_t wanted = {.some = true, .entry = {.has_value = true, .value = free}};

		if (!txn_memtable_map_newest(io, host_shared_Held_record(&helds[0]), TXN_MEMTABLE_MAP_MAP_FREE, 0, &newest.some,
									 &newest.entry, &error))
			STD_RESULT_EXPECT_FAILED(2151, 57, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		STD_ASSERT_EQ(2151, 9, newest.some && newest.entry.has_value && newest.entry.value.len == free.len &&
					  memcmp(newest.entry.value.ptr, free.ptr, free.len) == 0, &newest,
					  txn_runs_tests_entry_option_Debug_fmt, &wanted, txn_runs_tests_entry_option_Debug_fmt);
		if (newest.some && newest.entry.has_value)
			std_vec_free_Checked(&newest.entry.value, 1, 1);
	}
	for (size_t k = 0; k < lengthof(helds); k++)
	{
		HostRow		tid = {5};
		StdVec		key;
		StdBytes	bytes;
		StdBytes	k_bytes = {(const uint8 *) "k", 1};
		StdOptionRef got;
		StdOptionRef wanted = {true, STD_ITEM(&k_bytes, std_bytes_Debug_fmt)};
		bool		some;

		if (!txn_memtable_tuples_get(io, host_shared_Held_record(&helds[k]), tid, &some, &key, &error))
			STD_RESULT_EXPECT_FAILED(2153, 56, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		bytes = (StdBytes)
		{
			some ? key.ptr : NULL, some ? key.len : 0
		};
		got = (StdOptionRef)
		{
			some, STD_ITEM(&bytes, std_bytes_Debug_fmt)
		};
		STD_ASSERT_EQ(2153, 13, some && key.len == 1 && memcmp(key.ptr, "k", 1) == 0, &got, std_option_Debug_fmt,
					  &wanted, std_option_Debug_fmt);
		if (some)
			std_vec_free_Checked(&key, 1, 1);
	}
	{
		uint64		counters[2] = {txn_state_counter(host_shared_Held_record(&helds[0])),
		txn_state_counter(host_shared_Held_record(&helds[1]))};
		uint64		wanted[2] = {(4 << 16) | 1, 6};
		StdVec		counters_vec = {counters, 2, 2};
		StdVec		wanted_vec = {wanted, 2, 2};

		STD_ASSERT_EQ(2155, 9, counters[0] == wanted[0] && counters[1] == wanted[1], &counters_vec,
					  txn_runs_tests_u64s_Debug_fmt, &wanted_vec, txn_runs_tests_u64s_Debug_fmt);
	}
	txn_runs_tests_trees_free(&after);
	txn_runs_tests_trees_free(&before);
	host_shared_Held_drop(&deltas);
	host_shared_Held_drop(&helds[1]);
	host_shared_Held_drop(&helds[0]);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	std_vec_free_Checked(&free, 1, 1);
	simc_TestIo_drop(&test_io);
}

/* txn_runs_tests_warning_Debug_fmt is a taken warning's Debug, the tuple (SQLSTATE, message). */
static bool
txn_runs_tests_warning_Debug_fmt(const void *value, StdFormatter *f)
{
	const simc_TupleSqlstateString_t *warning = value;
	StdBytes	code = {warning->f0, sizeof(warning->f0)};
	StdTupleRef tuple = STD_TUPLE(2, STD_ITEM(&code, std_bytes_Debug_fmt),
								  STD_ITEM(&warning->f1, std_String_Debug_fmt));

	return std_tuple_Debug_fmt(&tuple, f);
}

/* txn_runs_tests_warnings_Debug_fmt is a Vec<([u8; 5], String)>'s Debug. */
static bool
txn_runs_tests_warnings_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *v = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t k = 0; k < v->len; k++)
		std_DebugList_entry(&list, &STD_VEC_AT(v, simc_TupleSqlstateString_t, k), txn_runs_tests_warning_Debug_fmt);
	return std_DebugList_finish(&list);
}

/* txn_runs_tests_warnings_free frees taken warnings and their messages. */
static void
txn_runs_tests_warnings_free(StdVec *warnings)
{
	for (size_t k = 0; k < warnings->len; k++)
		std_vec_free_Checked(&STD_VEC_AT(warnings, simc_TupleSqlstateString_t, k).f1, 1, 1);
	std_vec_free_Checked(warnings, sizeof(simc_TupleSqlstateString_t), _Alignof(simc_TupleSqlstateString_t));
}

/* txn_runs_tests_contains reports whether a message holds a text. */
static bool
txn_runs_tests_contains(const StdString *message, const char *text)
{
	size_t		len = strlen(text);

	for (size_t at = 0; at + len <= message->len; at++)
		if (memcmp((const uint8 *) message->ptr + at, text, len) == 0)
			return true;
	return false;
}

/*
 * txn_runs_tests_skipped_at runs a start's load and reports whether it
 * skipped a database: one warning of data corruption naming it, its marker,
 * and no state of it filled. The marker is then taken off, as the next
 * start's fresh shared memory leaves it.
 */
static bool
txn_runs_tests_skipped_at(HostIoIo *io, uint32 database)
{
	static const StdFmtPiece listed[] = {STD_FMT_ARG(0)};
	TxnInsertPage page = {.bytes = 8160, .most = 291};
	StdVec		warnings = simc_take_warnings();
	HostError	error;
	char		named[32];
	bool		warned;
	bool		marked;

	txn_runs_tests_warnings_free(&warnings);
	if (!txn_runs_loaded(io, &page, &error))
		STD_RESULT_EXPECT_FAILED(2163, 54, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	warnings = simc_take_warnings();
	snprintf(named, sizeof(named), "database %u:", database);
	warned = warnings.len == 1 &&
		memcmp(STD_VEC_AT(&warnings, simc_TupleSqlstateString_t, 0).f0, "XX001", 5) == 0 &&
		txn_runs_tests_contains(&STD_VEC_AT(&warnings, simc_TupleSqlstateString_t, 0).f1, named);
	if (!txn_state_marked(io, database, &marked, &error))
		STD_RESULT_EXPECT_FAILED(2167, 50, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (warned != marked)
	{
		StdFmtArg	args[1] = {{&warnings, txn_runs_tests_warnings_Debug_fmt}};
		StdFmtArguments message = {listed, lengthof(listed), args};
		StdTupleRef left = STD_TUPLE(2, STD_ITEM(&warned, std_bool_Debug_fmt), STD_ITEM(&marked, std_bool_Debug_fmt));
		StdTupleRef right = STD_TUPLE(2, STD_ITEM(&marked, std_bool_Debug_fmt),
									  STD_ITEM(&marked, std_bool_Debug_fmt));

		std_assert_failed(RUST_FILE, 2168, 9, "==", &left, std_tuple_Debug_fmt, &right, std_tuple_Debug_fmt,
						  &message);
	}
	if (marked)
	{
		HostStorage marker = txn_state_marker(database);
		StdVec		filled;
		bool		none = true;

		if (!txn_state_states(io, database, &filled, &error))
			STD_RESULT_EXPECT_FAILED(2170, 54, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		for (size_t k = 0; none && k < filled.len; k++)
		{
			HostStorage storage;
			TxnStatePersistence persistence;

			none = !txn_state_described(host_shared_Held_record(&STD_VEC_AT(&filled, txn_TupleU64HeldState_t, k).f1),
										&storage, &persistence);
		}
		STD_ASSERT(2171, 13, none, "filled.iter().all(|(_, held)| state::described(held.record()).is_none())");
		txn_state_states_free(&filled);
		if (!txn_state_unlinked(io, &marker, &error))
			STD_RESULT_EXPECT_FAILED(2173, 59, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
	}
	txn_runs_tests_warnings_free(&warnings);
	return marked;
}

/* txn_runs_tests_cut_captures_t is the cut case's prepare's captures: the free space entry, the row's tuple id, and its change. */
typedef struct txn_runs_tests_cut_captures_t
{
	StdBytes	free;
	HostRow		tuple;
	const TxnMemtableVersionChange *change;
} txn_runs_tests_cut_captures_t;

/* txn_runs_tests_cut_prepared is the cut case's prepare: the free space map's entry, then the row. */
static bool
txn_runs_tests_cut_prepared(void *captures, HostIoIo *io, TxnMemtableTablePreparedState *prepared, HostError *error)
{
	txn_runs_tests_cut_captures_t *c = captures;
	TxnMemtableMapTaken taken;

	return txn_memtable_map_finish_Unit(HOST_IO_IO_TAKES_SHORT(io), prepared, 0, c->change->writer, false, 0, false,
										NULL, 0, txn_runs_tests_put_step, &c->free, NULL, txn_runs_tests_nothing, NULL,
										NULL, &taken, error) &&
		txn_runs_tests_rowed(io, prepared, c->tuple, c->change, error);
}

/*
 * The cut's bound on the rows: runs whose rows hold a tuple id at or past the
 * counter their free space run was cut at, which no checkpoint writes since
 * a swept row's take is before the cut, are data corruption, and the start
 * skips their repository: a row at the cut, (0,1) for a storage that took
 * none, and one past it.
 */
void
test_txn_runs_tests_a_start_refuses_runs_whose_row_is_at_or_past_their_cut(void)
{
	static const StdFmtPiece numbered[] = {STD_FMT_ARG(0)};
	static const struct
	{
		uint32		database;
		uint64		tuple;
	}			cases[2] = {{9419, 1}, {9422, 5}};

	for (size_t k = 0; k < lengthof(cases); k++)
	{
		SimcTestIo	test_io = simc_io();
		HostIoIo   *io = &test_io.io;
		uint32		database = cases[k].database;
		HostRow		tuple = {cases[k].tuple};
		HostStorage storage = {.tablespace = HOST_PG_DEFAULT, .database = database, .relation = 16384};
		RepoRepo	repo;
		HostSharedReader reading;
		HostError	error;

		txn_runs_tests_repository(io, database, 2187, 95, 2188, 50, &repo, &reading);
		{
			HostSharedHeld held = txn_runs_tests_stated(io, &repo, &reading, &storage, 2191, 102);
			TxnMemtableOp op = txn_memtable_testing_put((const uint8 *) "v", 1);
			TxnMemtableVersionChange change = {.writer = {.xid = 7}, .op = &op,
			.lock = TXN_MEMTABLE_LOCK_MODE_UPDATE, .shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND},
			.has_lsn = true, .lsn = 9};
			TxnFreeSpaceFree made = txn_free_space_Free_new(100, NULL, 0, 291);
			txn_runs_tests_cut_captures_t c = {.tuple = tuple, .change = &change};
			txn_TupleRefSliceU8Usize_t rows[1];
			TxnMemtableMapTake take;
			StdVec		free;
			uint64		counter;
			uint64		one = 1;

			op.u.put.tuple = tuple;
			txn_free_space_Free_encode(&made, &free);
			c.free = (StdBytes)
			{
				free.ptr, free.len
			};
			rows[0] = (txn_TupleRefSliceU8Usize_t)
			{
				{
					(const uint8 *) "k", 1
				},
					txn_memtable_version_encoded(&change.writer, change.op, change.shown, change.has_lsn, change.lsn)
			};
			take = (TxnMemtableMapTake)
			{
				.state = host_shared_Held_record(&held), .map = TXN_MEMTABLE_MAP_MAP_FREE,
					.key = 0, .most = free.len
			};
			if (!txn_memtable_table_prepare_Unit(io, host_shared_Held_record(&held), rows, 1, &take, 1, true,
												 txn_runs_tests_cut_prepared, &c, NULL, &error))
				STD_RESULT_EXPECT_FAILED(2213, 18, "called `Result::unwrap()` on an `Err` value", &error,
										 host_Error_Debug_fmt);
			counter = txn_state_counter(host_shared_Held_record(&held));
			STD_ASSERT_EQ(2214, 17, counter == one, &counter, std_u64_Debug_fmt, &one, std_u64_Debug_fmt);
			std_vec_free_Checked(&free, 1, 1);
			txn_memtable_Op_drop(&op);
			host_shared_Held_drop(&held);
		}
		simc_checkpointed(UINT64CONST(0x100000a27));
		if (!txn_runs_checkpoint(io, UINT64CONST(0x100000a28), &error))
			STD_RESULT_EXPECT_FAILED(2217, 43, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		if (!txn_state_unlinked(io, &storage, &error))
			STD_RESULT_EXPECT_FAILED(2218, 43, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		if (!txn_runs_tests_skipped_at(io, database))
		{
			StdFmtArg	args[1] = {{&tuple, host_Row_Debug_fmt}};
			StdFmtArguments message = {numbered, lengthof(numbered), args};

			std_panic_fmt(RUST_FILE, 2219, 13, &message);
		}
		host_shared_Reader_drop(&reading);
		repo_Repo_drop(&repo);
		simc_TestIo_drop(&test_io);
	}
}

/* txn_runs_tests_gathered_none is a save's gathering of nothing, as the start cases save their runs alone. */
static bool
txn_runs_tests_gathered_none(void *captures, HostIoIo *io, const RepoRead *read, StdVec *result,
							 ChunksErrorError *error)
{
	std_vec_new(result, sizeof(RepoGathered), _Alignof(RepoGathered));
	return true;
}

/*
 * D1, the counter's form: a free space run whose counter has offset 0, an
 * offset past the page's most tuples plus 1, or a block past four bytes is
 * data corruption, and the start skips its repository; one at the page's
 * most tuples plus 1 moves the counter to it.
 */
void
test_txn_runs_tests_a_start_refuses_a_free_space_run_whose_counter_isnt_a_tuple_ids(void)
{
	static const StdFmtPiece numbered[] = {STD_FMT_ARG(0)};
	static const uint64 counters[] = {UINT64CONST(5) << 16, (UINT64CONST(5) << 16) | 293,
	((UINT64CONST(1) << 32) << 16) | 1, (UINT64CONST(5) << 16) | 292};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostStorage storage = {.tablespace = HOST_PG_DEFAULT, .database = 9421, .relation = 16384};
	TxnFreeSpaceFree made = txn_free_space_Free_new(100, NULL, 0, 291);
	TxnMemtableVersionWriter none = TXN_MEMTABLE_VERSION_NO_TRANSACTION;
	RepoRepo	repo;
	HostSharedReader reading;
	HostSharedHeld held;
	StdVec		entry;
	StdVec		record;
	HostError	error;
	uint64		counter;
	uint64		wanted = (UINT64CONST(5) << 16) | 292;

	txn_runs_tests_repository(io, 9421, 2232, 59, 2233, 46, &repo, &reading);
	txn_free_space_Free_encode(&made, &entry);
	for (size_t at = 0; at < lengthof(counters); at++)
	{
		StdFmtArg	args[1] = {{&at, std_usize_Display_fmt}};
		StdFmtArguments message = {numbered, lengthof(numbered), args};
		HostLsn		redo = UINT64CONST(0x100000a28) + at;
		TxnRunsNames names = TXN_RUNS_TESTS_NAMES;
		TxnRunsTail tail = {.tag = TXN_RUNS_TAIL_COUNTER, .counter = counters[at]};
		uint8		key[4] = {0};
		StdBytes	records[1];
		StdPathBuf	dir;
		StdStr		parent;
		TxnRunsWriting *writing;
		RepoRuns	runs = {.redo = redo};
		RepoRunFile file;
		ChunksErrorError failed;
		bool		some;
		bool		skipped;
		bool		refused = counters[at] != wanted;

		txn_runs_directory(9421, HOST_PG_DEFAULT, redo, &dir);
		if (!std_path_parent(dir.bytes.ptr, dir.bytes.len, &parent))
			STD_OPTION_UNWRAP_FAILED(2239, 39);
		if (!chunks_file_create_dir(HOST_IO_IO_IO_DOORS(io), parent.ptr, parent.len, &failed))
			STD_RESULT_EXPECT_FAILED(2240, 52, "called `Result::unwrap()` on an `Err` value", &failed,
									 chunks_error_Error_Debug_fmt);
		if (!chunks_file_create_dir(HOST_IO_IO_IO_DOORS(io), dir.bytes.ptr, dir.bytes.len, &failed))
			STD_RESULT_EXPECT_FAILED(2240, 52, "called `Result::unwrap()` on an `Err` value", &failed,
									 chunks_error_Error_Debug_fmt);
		names.kind = TXN_RUNS_KIND_FREE;
		names.database = 9421;
		names.redo = redo;
		writing = txn_runs_Writing_begin(dir.bytes.ptr, dir.bytes.len, &names, tail, false, 0);
		record = txn_runs_tests_entry_record(none.xid, entry.ptr, entry.len, false, 0);
		records[0] = (StdBytes)
		{
			record.ptr, record.len
		};
		if (!txn_runs_Writing_key(writing, io, key, sizeof(key), records, 1, &error))
			STD_RESULT_EXPECT_FAILED(2244, 90, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		std_vec_free_Checked(&record, 1, 1);
		if (!txn_runs_Writing_end(writing, io, NULL, 0, &some, &file, &error))
			STD_RESULT_EXPECT_FAILED(2245, 46, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		std_vec_new(&runs.files, sizeof(RepoRunFile), _Alignof(RepoRunFile));
		if (some)
			std_vec_push_Checked(&runs.files, &file, sizeof(file), _Alignof(RepoRunFile));
		if (!repo_Repo_save(&repo, io, &reading, true, &runs, txn_runs_tests_gathered_none, NULL, txn_runs_tests_saved,
							NULL, &failed))
			STD_RESULT_EXPECT_FAILED(2246, 100, "called `Result::unwrap()` on an `Err` value", &failed,
									 chunks_error_Error_Debug_fmt);
		skipped = txn_runs_tests_skipped_at(io, 9421);
		if (skipped != refused)
			std_assert_failed(RUST_FILE, 2247, 13, "==", &skipped, std_bool_Debug_fmt, &refused, std_bool_Debug_fmt,
							  &message);
		txn_runs_Writing_drop(writing);
		std_vec_free_Checked(&dir.bytes, 1, 1);
	}
	held = txn_runs_tests_stated(io, &repo, &reading, &storage, 2249, 94);
	counter = txn_state_counter(host_shared_Held_record(&held));
	STD_ASSERT_EQ(2250, 9, counter == wanted, &counter, std_u64_Debug_fmt, &wanted, std_u64_Debug_fmt);
	host_shared_Held_drop(&held);
	std_vec_free_Checked(&entry, 1, 1);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	simc_TestIo_drop(&test_io);
}

/*
 * The entry's length: a run whose file is whole and whose manifest entry
 * names another length is data corruption, and the start skips its
 * repository; it loads once the entry names its own again.
 */
void
test_txn_runs_tests_a_start_refuses_a_run_whose_length_isnt_its_entrys(void)
{
	static const uint64 longers[] = {1, 0};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostStorage storage = {.tablespace = HOST_PG_DEFAULT, .database = 9420, .relation = 16384};
	RepoRepo	repo;
	HostSharedReader reading;
	StdArcInner *manifest;
	const RepoManifest *read;
	RepoRuns	named;
	ChunksErrorError failed;
	HostError	error;

	txn_runs_tests_repository(io, 9420, 2261, 59, 2262, 46, &repo, &reading);
	{
		HostSharedHeld held = txn_runs_tests_stated(io, &repo, &reading, &storage, 2279, 98);

		txn_runs_tests_pushed(io, host_shared_Held_record(&held), "k", 2280, 59);
		host_shared_Held_drop(&held);
	}
	simc_checkpointed(UINT64CONST(0x100000a27));
	if (!txn_runs_checkpoint(io, UINT64CONST(0x100000a28), &error))
		STD_RESULT_EXPECT_FAILED(2283, 39, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!txn_state_unlinked(io, &storage, &error))
		STD_RESULT_EXPECT_FAILED(2284, 39, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!repo_Repo_manifest(&repo, io, &manifest, &failed))
		STD_RESULT_EXPECT_FAILED(2285, 39, "called `Result::unwrap()` on an `Err` value", &failed,
								 chunks_error_Error_Debug_fmt);
	read = std_arc_Deref_deref(manifest, _Alignof(RepoManifest));
	if (!read->has_runs)
		STD_OPTION_UNWRAP_FAILED(2285, 61);
	repo_Runs_Clone_clone(&read->runs, &named);
	std_arc_drop(manifest, sizeof(RepoManifest), _Alignof(RepoManifest), txn_runs_tests_Manifest_drop_glue);
	for (size_t k = 0; k < lengthof(longers); k++)
	{
		RepoRuns	runs;
		bool		skipped;
		bool		refused = longers[k] == 1;

		repo_Runs_Clone_clone(&named, &runs);
		std_index_check(0, runs.files.len, RUST_FILE, 2288, 13);
		STD_VEC_AT(&runs.files, RepoRunFile, 0).len += longers[k];
		if (!repo_Repo_save(&repo, io, &reading, true, &runs, txn_runs_tests_gathered_none, NULL, txn_runs_tests_saved,
							NULL, &failed))
			STD_RESULT_EXPECT_FAILED(2289, 84, "called `Result::unwrap()` on an `Err` value", &failed,
									 chunks_error_Error_Debug_fmt);
		skipped = txn_runs_tests_skipped_at(io, 9420);
		STD_ASSERT_EQ(2290, 13, skipped == refused, &skipped, std_bool_Debug_fmt, &refused, std_bool_Debug_fmt);
	}
	repo_Runs_drop(&named);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	simc_TestIo_drop(&test_io);
}

/* txn_runs_tests_there reports whether a repository's runs at a redo point have their directory. */
static bool
txn_runs_tests_there(HostIoIo *io, uint32 repository, HostLsn redo)
{
	StdPathBuf	path;
	ChunksErrorError failed;
	bool		there;

	txn_runs_directory(repository, HOST_PG_DEFAULT, redo, &path);
	if (!chunks_file_dir_there(HOST_IO_IO_READ_DOORS(io), path.bytes.ptr, path.bytes.len, &there, &failed))
		STD_RESULT_EXPECT_FAILED(1837, 85, "called `Result::unwrap()` on an `Err` value", &failed,
								 chunks_error_Error_Debug_fmt);
	std_vec_free_Checked(&path.bytes, 1, 1);
	return there;
}

/* txn_runs_tests_change returns a put's change by xid 7 at an LSN, as the unnamed runs case's change closure makes it. */
static TxnMemtableVersionChange
txn_runs_tests_change(const TxnMemtableOp *op, uint64 lsn)
{
	TxnMemtableVersionChange change = {.writer = {.xid = 7}, .op = op, .lock = TXN_MEMTABLE_LOCK_MODE_UPDATE,
	.shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND}, .has_lsn = true, .lsn = lsn};

	return change;
}

/* txn_runs_tests_theres_is asserts at its line that the unnamed runs case's two run directories stand as given. */
static void
txn_runs_tests_theres_is(uint32 line, HostIoIo *io, const HostLsn redos[2], const bool wanted[2])
{
	bool		theres[2];
	StdVec		theres_vec = {theres, 2, 2};
	StdVec		wanted_vec = {(void *) wanted, 2, 2};

	for (size_t k = 0; k < 2; k++)
		theres[k] = txn_runs_tests_there(io, 9414, redos[k]);
	STD_ASSERT_EQ(line, 9, theres[0] == wanted[0] && theres[1] == wanted[1], &theres_vec,
				  txn_runs_tests_bools_Debug_fmt, &wanted_vec, txn_runs_tests_bools_Debug_fmt);
}

/*
 * The crash between a checkpoint's runs and its manifest: a start removes the
 * directory of the runs no manifest names and loads the runs the manifest
 * does name, the checkpoint's before, so a row pushed after that checkpoint
 * isn't in the delta before redo brings it.
 */
void
test_txn_runs_tests_a_start_removes_the_runs_no_manifest_names_and_loads_the_named(void)
{
	static const HostLsn redos[2] = {UINT64CONST(0x100000a28), UINT64CONST(0x100001a28)};
	static const bool both[2] = {true, true};
	static const bool named_only[2] = {true, false};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostStorage storage = {.tablespace = HOST_PG_DEFAULT, .database = 9414, .relation = 16384};
	TxnMemtableOp op = txn_memtable_testing_put((const uint8 *) "v", 1);
	TxnInsertPage page = {.bytes = 8160, .most = 291};
	const struct
	{
		const char *key;
		uint64		lsn;
	}			keys[2] = {{"k", 9}, {"l", 10}};
	bool		holds[2];
	StdVec		holds_vec = {holds, 2, 2};
	StdVec		named_vec = {(void *) named_only, 2, 2};
	RepoRepo	repo;
	HostSharedReader reading;
	HostSharedHeld held;
	RepoRuns	runs;
	HostError	error;

	txn_runs_tests_repository(io, 9414, 2303, 59, 2304, 46, &repo, &reading);
	{
		TxnMemtableVersionChange change;
		TxnMemtableVersionHandle handle;

		held = txn_runs_tests_stated(io, &repo, &reading, &storage, 2322, 98);
		change = txn_runs_tests_change(&op, 9);
		if (!txn_memtable_table_push(io, host_shared_Held_record(&held), (const uint8 *) "k", 1, &change, &handle,
									 &error))
			STD_RESULT_EXPECT_FAILED(2323, 62, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		simc_checkpointed(redos[0] - 1);
		if (!txn_runs_checkpoint(io, redos[0], &error))
			STD_RESULT_EXPECT_FAILED(2325, 35, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		change = txn_runs_tests_change(&op, 10);
		if (!txn_memtable_table_push(io, host_shared_Held_record(&held), (const uint8 *) "l", 1, &change, &handle,
									 &error))
			STD_RESULT_EXPECT_FAILED(2326, 63, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		if (!txn_runs_write(io, 9414, HOST_PG_DEFAULT, redos[1], 0, NULL, 0, &runs, &error))
			STD_RESULT_EXPECT_FAILED(2327, 64, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		repo_Runs_drop(&runs);
		host_shared_Held_drop(&held);
	}
	txn_runs_tests_theres_is(2329, io, redos, both);
	if (!txn_state_unlinked(io, &storage, &error))
		STD_RESULT_EXPECT_FAILED(2330, 39, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!txn_runs_loaded(io, &page, &error))
		STD_RESULT_EXPECT_FAILED(2331, 54, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	held = txn_runs_tests_stated(io, &repo, &reading, &storage, 2332, 94);
	for (size_t k = 0; k < lengthof(keys); k++)
		if (!txn_memtable_table_holds(io, host_shared_Held_record(&held), (const uint8 *) keys[k].key, 1, keys[k].lsn,
									  &holds[k], &error))
			STD_RESULT_EXPECT_FAILED(2333, 104, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
	STD_ASSERT_EQ(2334, 9, holds[0] && !holds[1], &holds_vec, txn_runs_tests_bools_Debug_fmt, &named_vec,
				  txn_runs_tests_bools_Debug_fmt);
	txn_runs_tests_theres_is(2335, io, redos, named_only);
	host_shared_Held_drop(&held);
	txn_memtable_Op_drop(&op);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	simc_TestIo_drop(&test_io);
}

/*
 * The runs are durable before their manifest names them: after a power loss
 * right after a checkpoint, which takes back every write and every directory
 * entry no sync made durable, the runs the manifest names load whole.
 */
void
test_txn_runs_tests_a_checkpoints_runs_survive_a_power_loss(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostStorage storage = {.tablespace = HOST_PG_DEFAULT, .database = 9415, .relation = 16384};
	TxnInsertPage page = {.bytes = 8160, .most = 291};
	RepoRepo	repo;
	HostSharedReader reading;
	HostSharedHeld held;
	StdPathBuf	root;
	SimcSim		power;
	StdIoError	io_error;
	HostError	error;
	bool		holds;

	txn_runs_tests_repository(io, 9415, 2346, 59, 2347, 46, &repo, &reading);
	held = txn_runs_tests_stated(io, &repo, &reading, &storage, 2364, 98);
	txn_runs_tests_pushed(io, host_shared_Held_record(&held), "k", 2365, 59);
	host_shared_Held_drop(&held);
	root = simc_data_dir();
	power = simc_Sim_start(root.bytes.ptr, root.bytes.len, 31);
	std_vec_free_Checked(&root.bytes, 1, 1);
	simc_checkpointed(UINT64CONST(0x100000a27));
	if (!txn_runs_checkpoint(io, UINT64CONST(0x100000a28), &error))
		STD_RESULT_EXPECT_FAILED(2369, 39, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!simc_Sim_recover(&power, false, &io_error))
		STD_RESULT_EXPECT_FAILED(2370, 30, "called `Result::unwrap()` on an `Err` value", &io_error,
								 std_io_Error_Debug_fmt);
	simc_Sim_stop(&power);
	if (!txn_state_unlinked(io, &storage, &error))
		STD_RESULT_EXPECT_FAILED(2372, 39, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!txn_runs_loaded(io, &page, &error))
		STD_RESULT_EXPECT_FAILED(2373, 54, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	held = txn_runs_tests_stated(io, &repo, &reading, &storage, 2374, 94);
	if (!txn_memtable_table_holds(io, host_shared_Held_record(&held), (const uint8 *) "k", 1, 9, &holds, &error))
		STD_RESULT_EXPECT_FAILED(2375, 58, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(2375, 9, holds, "table::holds(io, held.record(), b\"k\", 9).unwrap()");
	host_shared_Held_drop(&held);
	simc_Sim_drop(&power);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	simc_TestIo_drop(&test_io);
}

/* txn_runs_tests_kept_t is a kept file of a repository's checkpoints: its path and its bytes. */
typedef struct txn_runs_tests_kept_t
{
	StdPathBuf	path;
	/* bytes is a Vec<u8>. */
	StdVec		bytes;
} txn_runs_tests_kept_t;

/* txn_runs_tests_kept_Debug_fmt is a kept file's Debug, the tuple (path, bytes). */
static bool
txn_runs_tests_kept_Debug_fmt(const void *value, StdFormatter *f)
{
	const txn_runs_tests_kept_t *kept = value;
	StdBytes	bytes = {kept->bytes.ptr, kept->bytes.len};
	StdTupleRef tuple = STD_TUPLE(2, STD_ITEM(&kept->path, std_PathBuf_Debug_fmt),
								  STD_ITEM(&bytes, std_bytes_Debug_fmt));

	return std_tuple_Debug_fmt(&tuple, f);
}

/* txn_runs_tests_kepts_Debug_fmt is a Vec<(PathBuf, Vec<u8>)>'s Debug. */
static bool
txn_runs_tests_kepts_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *v = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t k = 0; k < v->len; k++)
		std_DebugList_entry(&list, &STD_VEC_AT(v, txn_runs_tests_kept_t, k), txn_runs_tests_kept_Debug_fmt);
	return std_DebugList_finish(&list);
}

/* txn_runs_tests_kepts_eq reports whether two Vec<(PathBuf, Vec<u8>)>s are equal. */
static bool
txn_runs_tests_kepts_eq(const StdVec *a, const StdVec *b)
{
	if (a->len != b->len)
		return false;
	for (size_t k = 0; k < a->len; k++)
	{
		const txn_runs_tests_kept_t *x = &STD_VEC_AT(a, txn_runs_tests_kept_t, k);
		const txn_runs_tests_kept_t *y = &STD_VEC_AT(b, txn_runs_tests_kept_t, k);

		if (!std_path_PartialEq_eq(x->path.bytes.ptr, x->path.bytes.len, y->path.bytes.ptr, y->path.bytes.len) ||
			x->bytes.len != y->bytes.len || memcmp(x->bytes.ptr, y->bytes.ptr, x->bytes.len) != 0)
			return false;
	}
	return true;
}

/* txn_runs_tests_kepts_free frees kept files and their paths and bytes. */
static void
txn_runs_tests_kepts_free(StdVec *kepts)
{
	for (size_t k = 0; k < kepts->len; k++)
	{
		txn_runs_tests_kept_t *kept = &STD_VEC_AT(kepts, txn_runs_tests_kept_t, k);

		std_vec_free_Checked(&kept->path.bytes, 1, 1);
		std_vec_free_Checked(&kept->bytes, 1, 1);
	}
	std_vec_free_Checked(kepts, sizeof(txn_runs_tests_kept_t), _Alignof(txn_runs_tests_kept_t));
}

/* txn_runs_tests_path_cmp orders two paths by their components, as a Path's Ord does: a separator sorts below every other byte. */
static int
txn_runs_tests_path_cmp(const void *a, const void *b)
{
	const StdPathBuf *x = a;
	const StdPathBuf *y = b;
	size_t		len = Min(x->bytes.len, y->bytes.len);

	for (size_t k = 0; k < len; k++)
	{
		uint8		p = ((const uint8 *) x->bytes.ptr)[k];
		uint8		q = ((const uint8 *) y->bytes.ptr)[k];

		p = p == '/' || p == '\\' ? 0 : p;
		q = q == '/' || q == '\\' ? 0 : q;
		if (p != q)
			return p < q ? -1 : 1;
	}
	return x->bytes.len < y->bytes.len ? -1 : x->bytes.len > y->bytes.len ? 1 : 0;
}

/* txn_runs_tests_listed returns the names in a directory, at the places a case unwraps its read and its Option. */
static StdVec
txn_runs_tests_listed(HostIoIo *io, const StdPathBuf *dir, uint32 line, uint32 col, uint32 option_col)
{
	HostError	error;
	StdVec		names;
	bool		has;

	if (!host_list_dir_if_there(HOST_IO_IO_READ_DOORS(io), dir->bytes.ptr, dir->bytes.len, &has, &names, &error))
		STD_RESULT_EXPECT_FAILED(line, col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!has)
		STD_OPTION_UNWRAP_FAILED(line, option_col);
	return names;
}

/* txn_runs_tests_kept returns what a repository keeps of its checkpoints: both generations of its manifest and every file of its runs, each with its bytes. */
static StdVec
txn_runs_tests_kept(HostIoIo *io, uint32 repository)
{
	StdPathBuf	dir = host_tablespace_dir(repository, HOST_PG_DEFAULT);
	StdPathBuf	manifest;
	StdPathBuf	delta;
	StdPathBuf	previous;
	StdVec		paths;
	StdVec		runs;
	StdVec		kept;

	std_path_join(dir.bytes.ptr, dir.bytes.len, CHUNKS_PACKS_MANIFEST, strlen(CHUNKS_PACKS_MANIFEST), &manifest);
	previous = chunks_file_previous(manifest.bytes.ptr, manifest.bytes.len);
	std_vec_with_capacity_Checked(&paths, 2, sizeof(StdPathBuf), _Alignof(StdPathBuf));
	std_vec_push_Checked(&paths, &previous, sizeof(StdPathBuf), _Alignof(StdPathBuf));
	std_vec_push_Checked(&paths, &manifest, sizeof(StdPathBuf), _Alignof(StdPathBuf));
	std_path_join(dir.bytes.ptr, dir.bytes.len, CHUNKS_PACKS_DELTA, strlen(CHUNKS_PACKS_DELTA), &delta);
	runs = txn_runs_tests_listed(io, &delta, 2384, 66, 75);
	for (size_t r = 0; r < runs.len; r++)
	{
		const StdString *run = &STD_VEC_AT(&runs, StdString, r);
		StdPathBuf	run_dir;
		StdVec		files;

		std_path_join(delta.bytes.ptr, delta.bytes.len, run->ptr, run->len, &run_dir);
		files = txn_runs_tests_listed(io, &run_dir, 2385, 82, 91);
		for (size_t k = 0; k < files.len; k++)
		{
			const StdString *file = &STD_VEC_AT(&files, StdString, k);
			StdPathBuf	path;

			std_path_join(run_dir.bytes.ptr, run_dir.bytes.len, file->ptr, file->len, &path);
			std_vec_push_Checked(&paths, &path, sizeof(StdPathBuf), _Alignof(StdPathBuf));
		}
		std_vec_drop_Checked(&files, sizeof(StdString), _Alignof(StdString), std_String_drop);
		std_vec_free_Checked(&run_dir.bytes, 1, 1);
	}
	std_vec_drop_Checked(&runs, sizeof(StdString), _Alignof(StdString), std_String_drop);
	qsort(paths.ptr, paths.len, sizeof(StdPathBuf), txn_runs_tests_path_cmp);
	std_vec_with_capacity_Checked(&kept, paths.len, sizeof(txn_runs_tests_kept_t), _Alignof(txn_runs_tests_kept_t));
	for (size_t k = 0; k < paths.len; k++)
	{
		txn_runs_tests_kept_t entry = {.path = STD_VEC_AT(&paths, StdPathBuf, k)};
		ChunksErrorError failed;

		if (!chunks_file_read_file(HOST_IO_IO_READ_DOORS(io), entry.path.bytes.ptr, entry.path.bytes.len, &entry.bytes,
								   &failed))
			STD_RESULT_EXPECT_FAILED(2389, 88, "called `Result::unwrap()` on an `Err` value", &failed,
									 chunks_error_Error_Debug_fmt);
		std_vec_push_Checked(&kept, &entry, sizeof(entry), _Alignof(txn_runs_tests_kept_t));
	}
	std_vec_free_Checked(&paths, sizeof(StdPathBuf), _Alignof(StdPathBuf));
	std_vec_free_Checked(&delta.bytes, 1, 1);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	return kept;
}

/* txn_runs_tests_sqlstate_is asserts at its place that a refusal's SQLSTATE is XX001, and drops the refusal. */
static void
txn_runs_tests_sqlstate_is(uint32 line, uint32 col, HostError *refused)
{
	StdBytes	sqlstate = {host_Error_sqlstate(refused), 5};
	StdBytes	wanted = {(const uint8 *) "XX001", 5};

	STD_ASSERT_EQ(line, col, memcmp(sqlstate.ptr, wanted.ptr, 5) == 0, &sqlstate, std_bytes_Debug_fmt, &wanted,
				  std_bytes_Debug_fmt);
	host_Error_drop(refused);
}

/* txn_runs_tests_reopened opens a test's repository of a database again, as the manifest case's reopened closure does. */
static RepoRepo
txn_runs_tests_reopened(HostIoIo *io, uint32 database)
{
	RepoRepo	repo;
	ChunksErrorError failed;

	if (!repo_Repo_open(io, database, HOST_PG_DEFAULT, &repo, &failed))
		STD_RESULT_EXPECT_FAILED(2410, 82, "called `Result::unwrap()` on an `Err` value", &failed,
								 chunks_error_Error_Debug_fmt);
	return repo;
}

/* txn_runs_tests_holds_k asserts at its line that a held state holds key k at LSN 9. */
static void
txn_runs_tests_holds_k(uint32 line, HostIoIo *io, const HostSharedHeld *held)
{
	HostError	error;
	bool		holds;

	if (!txn_memtable_table_holds(io, host_shared_Held_record(held), (const uint8 *) "k", 1, 9, &holds, &error))
		STD_RESULT_EXPECT_FAILED(line, 58, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(line, 9, holds, "table::holds(io, held.record(), b\"k\", 9).unwrap()");
}

/*
 * The loader's rule for a repository it can't read (CommPF 6): a start that
 * finds one repository's newest manifest damaged warns of it once as data
 * corruption, naming the database, loads the other repository's runs, and
 * leaves the damaged one's as they are, which a recovery's cleanup passes
 * over. With its manifest made whole again under the running server, its
 * repository opens, yet a state's make of it is still refused, so redo raises
 * at a record of it, and a checkpoint leaves both generations of its
 * manifest and its runs' files byte for byte. Once its marker is gone, as a
 * restart leaves the state pool, the next start loads its runs.
 */
void
test_txn_runs_tests_a_start_warns_of_a_manifest_it_cant_read_and_loads_the_others_runs(void)
{
	static const StdFmtPiece listed[] = {STD_FMT_ARG(0)};
	static const HostLsn redos[2] = {UINT64CONST(0x100000a28), UINT64CONST(0x100001a28)};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	TxnInsertPage page = {.bytes = 8160, .most = 291};
	HostStorage storages[2];
	HostStorage marker = txn_state_marker(9416);
	HostSharedReader reading;
	HostSharedHeld held;
	RepoRepo	other;
	RepoRepo	repaired;
	StdPathBuf	dir;
	StdPathBuf	manifest;
	StdVec		whole;
	StdVec		bytes;
	StdVec		warnings;
	StdVec		before;
	StdVec		after;
	StdIoError	io_error;
	ChunksErrorError failed;
	HostError	error;
	size_t		four = 4;
	bool		warned;

	if (!host_shared_read(io, &reading, &error))
		STD_RESULT_EXPECT_FAILED(2404, 46, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	for (size_t k = 0; k < 2; k++)
	{
		uint32		database = k == 0 ? 9416 : 9417;
		RepoRepo	made;

		if (!repo_Repo_create(io, database, HOST_PG_DEFAULT, &made, &failed))
			STD_RESULT_EXPECT_FAILED(2406, 89, "called `Result::unwrap()` on an `Err` value", &failed,
									 chunks_error_Error_Debug_fmt);
		repo_Repo_drop(&made);
		storages[k] = (HostStorage)
		{
			.tablespace = HOST_PG_DEFAULT, .database = database, .relation = 16384
		};
	}
	for (size_t k = 0; k < 2; k++)
	{
		RepoRepo	repo = txn_runs_tests_reopened(io, storages[k].database);

		held = txn_runs_tests_stated(io, &repo, &reading, &storages[k], 2428, 97);
		txn_runs_tests_pushed(io, host_shared_Held_record(&held), "k", 2429, 59);
		host_shared_Held_drop(&held);
		repo_Repo_drop(&repo);
	}
	for (size_t k = 0; k < lengthof(redos); k++)
	{
		simc_checkpointed(redos[k] - 1);
		if (!txn_runs_checkpoint(io, redos[k], &error))
			STD_RESULT_EXPECT_FAILED(2433, 34, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
	}
	for (size_t k = 0; k < 2; k++)
		if (!txn_state_unlinked(io, &storages[k], &error))
			STD_RESULT_EXPECT_FAILED(2436, 42, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
	dir = host_tablespace_dir(9416, HOST_PG_DEFAULT);
	std_path_join(dir.bytes.ptr, dir.bytes.len, CHUNKS_PACKS_MANIFEST, strlen(CHUNKS_PACKS_MANIFEST), &manifest);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	if (!std_fs_read(manifest.bytes.ptr, manifest.bytes.len, &whole, &io_error))
		STD_RESULT_EXPECT_FAILED(2439, 46, "called `Result::unwrap()` on an `Err` value", &io_error,
								 std_io_Error_Debug_fmt);
	std_vec_clone(&bytes, &whole, 1, 1);
	std_index_check(0, bytes.len, RUST_FILE, 2441, 9);
	((uint8 *) bytes.ptr)[0] ^= 0xff;
	if (!std_fs_write(manifest.bytes.ptr, manifest.bytes.len, bytes.ptr, bytes.len, &io_error))
		STD_RESULT_EXPECT_FAILED(2442, 42, "called `Result::unwrap()` on an `Err` value", &io_error,
								 std_io_Error_Debug_fmt);
	std_vec_free_Checked(&bytes, 1, 1);
	warnings = simc_take_warnings();
	txn_runs_tests_warnings_free(&warnings);
	if (!txn_runs_loaded(io, &page, &error))
		STD_RESULT_EXPECT_FAILED(2444, 54, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	warnings = simc_take_warnings();
	warned = warnings.len == 1 &&
		memcmp(STD_VEC_AT(&warnings, simc_TupleSqlstateString_t, 0).f0, "XX001", 5) == 0 &&
		txn_runs_tests_contains(&STD_VEC_AT(&warnings, simc_TupleSqlstateString_t, 0).f1, "database 9416");
	if (!warned)
	{
		StdFmtArg	args[1] = {{&warnings, txn_runs_tests_warnings_Debug_fmt}};
		StdFmtArguments message = {listed, lengthof(listed), args};

		std_panic_fmt(RUST_FILE, 2446, 9, &message);
	}
	txn_runs_tests_warnings_free(&warnings);
	other = txn_runs_tests_reopened(io, 9417);
	held = txn_runs_tests_stated(io, &other, &reading, &storages[1], 2451, 99);
	txn_runs_tests_holds_k(2452, io, &held);
	{
		RepoRepo	refused;

		if (repo_Repo_open(io, 9416, HOST_PG_DEFAULT, &refused, &failed))
		{
			repo_Repo_drop(&refused);
			STD_RESULT_EXPECT_FAILED(2454, 51, "called `Result::unwrap_err()` on an `Ok` value", NULL,
									 std_unit_Debug_fmt);
		}
		error = txn_base_storage(&failed);
		txn_runs_tests_sqlstate_is(2454, 9, &error);
	}
	if (!txn_runs_cleaned(io, &error))
		STD_RESULT_EXPECT_FAILED(2455, 21, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!std_fs_write(manifest.bytes.ptr, manifest.bytes.len, whole.ptr, whole.len, &io_error))
		STD_RESULT_EXPECT_FAILED(2456, 42, "called `Result::unwrap()` on an `Err` value", &io_error,
								 std_io_Error_Debug_fmt);
	std_vec_free_Checked(&whole, 1, 1);
	before = txn_runs_tests_kept(io, 9416);
	STD_ASSERT_EQ(2458, 9, before.len == four, &before.len, std_usize_Debug_fmt, &four, std_usize_Debug_fmt);
	repaired = txn_runs_tests_reopened(io, 9416);
	for (size_t k = 0; k < 2; k++)
	{
		HostSharedHeld refused;

		if (txn_state_state(io, &repaired, &reading, &storages[0], TXN_STATE_PERSISTENCE_PERMANENT, false, NULL,
							&refused, &error))
		{
			host_shared_Held_drop(&refused);
			STD_RESULT_EXPECT_FAILED(2462, 32, "called `Result::unwrap_err()` on an `Ok` value", NULL,
									 std_unit_Debug_fmt);
		}
		txn_runs_tests_sqlstate_is(2462, 13, &error);
	}
	if (txn_state_usable(io, 9416, &error))
		STD_RESULT_EXPECT_FAILED(2464, 44, "called `Result::unwrap_err()` on an `Ok` value", NULL,
								 std_unit_Debug_fmt);
	txn_runs_tests_sqlstate_is(2464, 9, &error);
	simc_checkpointed(UINT64CONST(0x100002a27));
	if (!txn_runs_checkpoint(io, UINT64CONST(0x100002a28), &error))
		STD_RESULT_EXPECT_FAILED(2466, 39, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(2467, 9, txn_runs_tests_there(io, 9417, UINT64CONST(0x100002a28)),
			   "there(io, 9417, 0x1_0000_2a28)");
	after = txn_runs_tests_kept(io, 9416);
	STD_ASSERT_EQ(2468, 9, txn_runs_tests_kepts_eq(&after, &before), &after, txn_runs_tests_kepts_Debug_fmt, &before,
				  txn_runs_tests_kepts_Debug_fmt);
	txn_runs_tests_kepts_free(&after);
	if (!txn_state_unlinked(io, &marker, &error))
		STD_RESULT_EXPECT_FAILED(2469, 51, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!txn_state_unlinked(io, &storages[1], &error))
		STD_RESULT_EXPECT_FAILED(2470, 43, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!txn_runs_loaded(io, &page, &error))
		STD_RESULT_EXPECT_FAILED(2471, 54, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	warnings = simc_take_warnings();
	{
		StdVec		none = {NULL, 0, 0};

		STD_ASSERT_EQ(2472, 9, warnings.len == 0, &warnings, txn_runs_tests_warnings_Debug_fmt, &none,
					  txn_runs_tests_warnings_Debug_fmt);
	}
	txn_runs_tests_warnings_free(&warnings);
	host_shared_Held_drop(&held);
	held = txn_runs_tests_stated(io, &repaired, &reading, &storages[0], 2473, 102);
	txn_runs_tests_holds_k(2474, io, &held);
	host_shared_Held_drop(&held);
	txn_runs_tests_kepts_free(&before);
	repo_Repo_drop(&repaired);
	repo_Repo_drop(&other);
	std_vec_free_Checked(&manifest.bytes, 1, 1);
	host_shared_Reader_drop(&reading);
	simc_TestIo_drop(&test_io);
}

/*
 * The loader's rule for any other failure (CommPF 6): a start whose open of a
 * repository fails for a reason that isn't data corruption, here a directory
 * in its manifest's place, which holds a file so that every file system
 * gives it a length to read, fails with that error and no warning, and
 * leaves every run file as it was, so the next start, with the manifest
 * back, loads them.
 */
void
test_txn_runs_tests_a_start_fails_on_a_failure_that_isnt_corruption_and_keeps_its_runs(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostStorage storage = {.tablespace = HOST_PG_DEFAULT, .database = 9418, .relation = 16384};
	TxnInsertPage page = {.bytes = 8160, .most = 291};
	RepoRepo	repo;
	HostSharedReader reading;
	HostSharedHeld held;
	StdPathBuf	dir;
	StdPathBuf	manifest;
	StdPathBuf	inside;
	StdVec		bytes;
	StdVec		before;
	StdVec		after;
	StdVec		warnings;
	StdVec		none = {NULL, 0, 0};
	StdIoError	io_error;
	HostError	error;

	txn_runs_tests_repository(io, 9418, 2488, 59, 2489, 46, &repo, &reading);
	held = txn_runs_tests_stated(io, &repo, &reading, &storage, 2506, 98);
	txn_runs_tests_pushed(io, host_shared_Held_record(&held), "k", 2507, 59);
	host_shared_Held_drop(&held);
	simc_checkpointed(UINT64CONST(0x100000a27));
	if (!txn_runs_checkpoint(io, UINT64CONST(0x100000a28), &error))
		STD_RESULT_EXPECT_FAILED(2510, 39, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!txn_state_unlinked(io, &storage, &error))
		STD_RESULT_EXPECT_FAILED(2511, 39, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	before = txn_runs_tests_kept(io, 9418);
	dir = host_tablespace_dir(9418, HOST_PG_DEFAULT);
	std_path_join(dir.bytes.ptr, dir.bytes.len, CHUNKS_PACKS_MANIFEST, strlen(CHUNKS_PACKS_MANIFEST), &manifest);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	if (!std_fs_read(manifest.bytes.ptr, manifest.bytes.len, &bytes, &io_error))
		STD_RESULT_EXPECT_FAILED(2514, 46, "called `Result::unwrap()` on an `Err` value", &io_error,
								 std_io_Error_Debug_fmt);
	if (!std_fs_remove_file(manifest.bytes.ptr, manifest.bytes.len, &io_error))
		STD_RESULT_EXPECT_FAILED(2515, 41, "called `Result::unwrap()` on an `Err` value", &io_error,
								 std_io_Error_Debug_fmt);
	if (!std_fs_create_dir(manifest.bytes.ptr, manifest.bytes.len, &io_error))
		STD_RESULT_EXPECT_FAILED(2516, 40, "called `Result::unwrap()` on an `Err` value", &io_error,
								 std_io_Error_Debug_fmt);
	std_path_join(manifest.bytes.ptr, manifest.bytes.len, "held", 4, &inside);
	if (!std_fs_write(inside.bytes.ptr, inside.bytes.len, (const uint8 *) "", 0, &io_error))
		STD_RESULT_EXPECT_FAILED(2517, 52, "called `Result::unwrap()` on an `Err` value", &io_error,
								 std_io_Error_Debug_fmt);
	std_vec_free_Checked(&inside.bytes, 1, 1);
	warnings = simc_take_warnings();
	txn_runs_tests_warnings_free(&warnings);
	if (txn_runs_loaded(io, &page, &error))
		STD_RESULT_EXPECT_FAILED(2519, 67, "called `Result::unwrap_err()` on an `Ok` value", NULL,
								 std_unit_Debug_fmt);
	{
		StdBytes	sqlstate = {host_Error_sqlstate(&error), 5};
		StdBytes	corrupt = {(const uint8 *) "XX001", 5};

		if (memcmp(sqlstate.ptr, corrupt.ptr, 5) == 0)
			std_assert_failed(RUST_FILE, 2520, 9, "!=", &sqlstate, std_bytes_Debug_fmt, &corrupt, std_bytes_Debug_fmt,
							  NULL);
		host_Error_drop(&error);
	}
	warnings = simc_take_warnings();
	STD_ASSERT_EQ(2521, 9, warnings.len == 0, &warnings, txn_runs_tests_warnings_Debug_fmt, &none,
				  txn_runs_tests_warnings_Debug_fmt);
	txn_runs_tests_warnings_free(&warnings);
	if (!std_fs_remove_dir_all(manifest.bytes.ptr, manifest.bytes.len, &io_error))
		STD_RESULT_EXPECT_FAILED(2522, 44, "called `Result::unwrap()` on an `Err` value", &io_error,
								 std_io_Error_Debug_fmt);
	if (!std_fs_write(manifest.bytes.ptr, manifest.bytes.len, bytes.ptr, bytes.len, &io_error))
		STD_RESULT_EXPECT_FAILED(2523, 42, "called `Result::unwrap()` on an `Err` value", &io_error,
								 std_io_Error_Debug_fmt);
	std_vec_free_Checked(&bytes, 1, 1);
	after = txn_runs_tests_kept(io, 9418);
	STD_ASSERT_EQ(2524, 9, txn_runs_tests_kepts_eq(&after, &before), &after, txn_runs_tests_kepts_Debug_fmt, &before,
				  txn_runs_tests_kepts_Debug_fmt);
	txn_runs_tests_kepts_free(&after);
	if (!txn_runs_loaded(io, &page, &error))
		STD_RESULT_EXPECT_FAILED(2525, 54, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	held = txn_runs_tests_stated(io, &repo, &reading, &storage, 2526, 94);
	txn_runs_tests_holds_k(2527, io, &held);
	host_shared_Held_drop(&held);
	txn_runs_tests_kepts_free(&before);
	std_vec_free_Checked(&manifest.bytes, 1, 1);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	simc_TestIo_drop(&test_io);
}

/* txn_runs_tests_holds_both_t is the unlogged case's two storages: each storage and its persistence. */
typedef struct txn_runs_tests_holds_both_t
{
	HostStorage storage;
	TxnStatePersistence persistence;
} txn_runs_tests_holds_both_t;

/* txn_runs_tests_holds_both asserts at its line whether each of the unlogged case's storages holds key k at LSN 9, as its held closure reads them. */
static void
txn_runs_tests_holds_both(uint32 line, HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading,
						  const txn_runs_tests_holds_both_t *storages, const bool wanted[2])
{
	bool		holds[2];
	StdVec		holds_vec = {holds, 2, 2};
	StdVec		wanted_vec = {(void *) wanted, 2, 2};

	for (size_t k = 0; k < 2; k++)
	{
		HostSharedHeld held;
		HostError	error;

		if (!txn_state_state(io, repo, reading, &storages[k].storage, storages[k].persistence, false, NULL, &held,
							 &error))
			STD_RESULT_EXPECT_FAILED(2570, 90, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		if (!txn_memtable_table_holds(io, host_shared_Held_record(&held), (const uint8 *) "k", 1, 9, &holds[k],
									  &error))
			STD_RESULT_EXPECT_FAILED(2571, 58, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		host_shared_Held_drop(&held);
	}
	STD_ASSERT_EQ(line, 9, holds[0] == wanted[0] && holds[1] == wanted[1], &holds_vec, txn_runs_tests_bools_Debug_fmt,
				  &wanted_vec, txn_runs_tests_bools_Debug_fmt);
}

/*
 * The unlogged case at the unit level: an unlogged storage's rows come back
 * with its runs, as a clean restart keeps them, and a recovery's cleanup
 * drops them, leaving a permanent storage's as loaded.
 */
void
test_txn_runs_tests_a_recoverys_cleanup_drops_an_unlogged_storages_loaded_rows(void)
{
	static const bool both[2] = {true, true};
	static const bool permanent[2] = {false, true};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	TxnInsertPage page = {.bytes = 8160, .most = 291};
	txn_runs_tests_holds_both_t storages[2] = {
		{{.tablespace = HOST_PG_DEFAULT, .database = 9413, .relation = 16384 + TXN_STATE_PERSISTENCE_UNLOGGED},
		TXN_STATE_PERSISTENCE_UNLOGGED},
		{{.tablespace = HOST_PG_DEFAULT, .database = 9413, .relation = 16384 + TXN_STATE_PERSISTENCE_PERMANENT},
	TXN_STATE_PERSISTENCE_PERMANENT}};
	RepoRepo	repo;
	HostSharedReader reading;
	HostError	error;

	txn_runs_tests_repository(io, 9413, 2538, 59, 2539, 46, &repo, &reading);
	for (size_t k = 0; k < lengthof(storages); k++)
	{
		HostSharedHeld held;

		if (!txn_state_state(io, &repo, &reading, &storages[k].storage, storages[k].persistence, false, NULL, &held,
							 &error))
			STD_RESULT_EXPECT_FAILED(2559, 87, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		txn_runs_tests_pushed(io, host_shared_Held_record(&held), "k", 2560, 59);
		host_shared_Held_drop(&held);
	}
	simc_checkpointed(UINT64CONST(0x100000a27));
	if (!txn_runs_checkpoint(io, UINT64CONST(0x100000a28), &error))
		STD_RESULT_EXPECT_FAILED(2563, 39, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	for (size_t k = 0; k < lengthof(storages); k++)
		if (!txn_state_unlinked(io, &storages[k].storage, &error))
			STD_RESULT_EXPECT_FAILED(2565, 42, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
	if (!txn_runs_loaded(io, &page, &error))
		STD_RESULT_EXPECT_FAILED(2567, 54, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	txn_runs_tests_holds_both(2574, io, &repo, &reading, storages, both);
	if (!txn_runs_cleaned(io, &error))
		STD_RESULT_EXPECT_FAILED(2575, 21, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	txn_runs_tests_holds_both(2576, io, &repo, &reading, storages, permanent);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	simc_TestIo_drop(&test_io);
}

/* txn_runs_tests_row_captures_t is a prepare's captures that finish one row: its tuple id and its change. */
typedef struct txn_runs_tests_row_captures_t
{
	HostRow		tuple;
	const TxnMemtableVersionChange *change;
} txn_runs_tests_row_captures_t;

/* txn_runs_tests_row_prepared is the counters case's prepare: the storage's row at its tuple id. */
static bool
txn_runs_tests_row_prepared(void *captures, HostIoIo *io, TxnMemtableTablePreparedState *prepared, HostError *error)
{
	const txn_runs_tests_row_captures_t *c = captures;

	return txn_runs_tests_rowed(io, prepared, c->tuple, c->change, error);
}

/*
 * The loader's end of each storage: two storages with rows and no free space
 * run, one before the other in the manifest's order, each move their counter
 * past their own rows, the first as the second's runs begin and the second
 * as the runs end.
 */
void
test_txn_runs_tests_a_start_moves_each_storage_with_no_free_space_run_past_its_rows(void)
{
	static const uint64 tuples[2] = {7, (1 << 16) | 3};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	TxnInsertPage page = {.bytes = 8160, .most = 291};
	HostStorage storages[2] = {{.tablespace = HOST_PG_DEFAULT, .database = 9423, .relation = 16384},
	{.tablespace = HOST_PG_DEFAULT, .database = 9423, .relation = 16385}};
	uint64		counters[2];
	uint64		wanted[2] = {8, (1 << 16) | 4};
	StdVec		counters_vec = {counters, 2, 2};
	StdVec		wanted_vec = {wanted, 2, 2};
	RepoRepo	repo;
	HostSharedReader reading;
	HostError	error;

	txn_runs_tests_repository(io, 9423, 2587, 87, 2588, 46, &repo, &reading);
	for (size_t k = 0; k < lengthof(storages); k++)
	{
		HostSharedHeld held = txn_runs_tests_stated(io, &repo, &reading, &storages[k], 2593, 97);
		TxnMemtableOp op = txn_memtable_testing_put((const uint8 *) "v", 1);
		TxnMemtableVersionChange change = {.writer = {.xid = 7}, .op = &op, .lock = TXN_MEMTABLE_LOCK_MODE_UPDATE,
		.shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND}, .has_lsn = true, .lsn = 9};
		txn_runs_tests_row_captures_t c = {.tuple = {tuples[k]}, .change = &change};
		txn_TupleRefSliceU8Usize_t rows[1];

		op.u.put.tuple = c.tuple;
		rows[0] = (txn_TupleRefSliceU8Usize_t)
		{
			{
				(const uint8 *) "k", 1
			},
				txn_memtable_version_encoded(&change.writer, change.op, change.shown, change.has_lsn, change.lsn)
		};
		if (!txn_memtable_table_prepare_Unit(io, host_shared_Held_record(&held), rows, 1, NULL, 0, true,
											 txn_runs_tests_row_prepared, &c, NULL, &error))
			STD_RESULT_EXPECT_FAILED(2607, 14, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		txn_memtable_Op_drop(&op);
		host_shared_Held_drop(&held);
	}
	simc_checkpointed(UINT64CONST(0x100000a27));
	if (!txn_runs_checkpoint(io, UINT64CONST(0x100000a28), &error))
		STD_RESULT_EXPECT_FAILED(2610, 39, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	for (size_t k = 0; k < lengthof(storages); k++)
		if (!txn_state_unlinked(io, &storages[k], &error))
			STD_RESULT_EXPECT_FAILED(2612, 42, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
	if (!txn_runs_loaded(io, &page, &error))
		STD_RESULT_EXPECT_FAILED(2614, 54, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	for (size_t k = 0; k < lengthof(storages); k++)
	{
		HostSharedHeld held = txn_runs_tests_stated(io, &repo, &reading, &storages[k], 2617, 97);

		counters[k] = txn_state_counter(host_shared_Held_record(&held));
		host_shared_Held_drop(&held);
	}
	STD_ASSERT_EQ(2620, 9, counters[0] == wanted[0] && counters[1] == wanted[1], &counters_vec,
				  txn_runs_tests_u64s_Debug_fmt, &wanted_vec, txn_runs_tests_u64s_Debug_fmt);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	simc_TestIo_drop(&test_io);
}

/* txn_runs_tests_columns_key is the unlogged reset case's key: one int4 column under btree's int4 class. */
static TxnBaseKey
txn_runs_tests_columns_key(void)
{
	TxnBaseKeyColumn column = {.attnum = 1, .class = 1978, .typid = 23};
	TxnBaseKey	key = {.tag = TXN_BASE_KEY_COLUMNS};

	std_vec_with_capacity_Checked(&key.u.columns, 1, sizeof(TxnBaseKeyColumn), _Alignof(TxnBaseKeyColumn));
	std_vec_push_Checked(&key.u.columns, &column, sizeof(column), _Alignof(TxnBaseKeyColumn));
	return key;
}

/* txn_runs_tests_saved_base reads the base record a storage's repository saved, decoded, answering whether it has one. */
static bool
txn_runs_tests_saved_base(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading,
						  const HostStorage *storage, TxnBaseBase *result)
{
	RepoStorage at = {.tablespace = storage->tablespace, .relation = storage->relation};
	RepoBaseRecord read;
	ChunksErrorError failed;
	HostError	error;
	bool		has;

	if (!repo_Repo_base(repo, io, reading, &at, &has, &read, &failed))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &failed,
								 chunks_error_Error_Debug_fmt);
	if (!has)
		return false;
	if (!txn_base_Base_decode(read.bytes.ptr, read.bytes.len, read.addresses.ptr, read.addresses.len, result,
							  &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	repo_BaseRecord_drop(&read);
	return true;
}

/* txn_runs_tests_made_dirs makes a directory and each of its parents up to a count of them, the farthest first. */
static void
txn_runs_tests_made_dirs(HostIoIo *io, const StdPathBuf *dir, size_t count)
{
	StdStr		dirs[4];
	ChunksErrorError failed;

	dirs[0].ptr = dir->bytes.ptr;
	dirs[0].len = dir->bytes.len;
	for (size_t k = 1; k < count; k++)
		if (!std_path_parent(dirs[k - 1].ptr, dirs[k - 1].len, &dirs[k]))
			STD_ASSERT(__LINE__, 0, false, "a directory of the case has its parents");
	for (size_t k = count; k > 0; k--)
		if (!chunks_file_create_dir(HOST_IO_IO_IO_DOORS(io), dirs[k - 1].ptr, dirs[k - 1].len, &failed))
			STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &failed,
									 chunks_error_Error_Debug_fmt);
}

/* txn_runs_tests_made_file makes an empty file of a name in a directory and closes it. */
static void
txn_runs_tests_made_file(HostIoIo *io, const StdPathBuf *dir, const char *name)
{
	StdPathBuf	path;
	HostFile	file;
	ChunksErrorError failed;

	std_path_join(dir->bytes.ptr, dir->bytes.len, name, strlen(name), &path);
	if (!chunks_file_create(HOST_IO_IO_IO_DOORS(io), path.bytes.ptr, path.bytes.len, &file, &failed))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &failed,
								 chunks_error_Error_Debug_fmt);
	host_File_drop(&file);
	std_vec_free_Checked(&path.bytes, 1, 1);
}

/*
 * The unlogged reset: each storage of an unlogged relation, found by its init
 * fork in pg_default's directory or another tablespace's, whose checkpoint
 * saved a base record with its key, its table, and a counter past its rows,
 * gets a state anew at the cleanup that holds an empty base record keeping
 * the key and the table, its counter at the first tuple id, unsaved, so the
 * next checkpoint writes the empty record over the saved one; the cleanup
 * run again does the same, a permanent relation's storage, whose main fork's
 * file sits beside them, keeps its saved base, and an unlogged relation's
 * storage with no saved base gets no state.
 */
void
test_txn_runs_tests_a_recoverys_cleanup_resets_an_unlogged_storages_saved_base(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostStorage unlogged[2] = {{.tablespace = HOST_PG_DEFAULT, .database = 9424, .relation = 16384},
	{.tablespace = 16400, .database = 9424, .relation = 16387}};
	HostStorage permanent = {.tablespace = HOST_PG_DEFAULT, .database = 9424, .relation = 16385};
	HostStorage all[3] = {unlogged[0], unlogged[1], permanent};
	TxnStatePersistence persistences[3] = {TXN_STATE_PERSISTENCE_UNLOGGED, TXN_STATE_PERSISTENCE_UNLOGGED,
	TXN_STATE_PERSISTENCE_PERMANENT};
	TxnBaseKey	key = txn_runs_tests_columns_key();
	TxnMemtableOp op = txn_memtable_testing_put((const uint8 *) "v", 1);
	TxnMemtableVersionChange change = {.writer = {.xid = 7}, .op = &op, .lock = TXN_MEMTABLE_LOCK_MODE_UPDATE,
	.shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND}, .has_lsn = true, .lsn = 9};
	RepoRepo	repo;
	HostSharedReader reading;
	StdPathBuf	root;
	StdPathBuf	other;
	StdPathBuf	base_dir;
	StdPathBuf	other_dir;
	StdStr		parent;
	TxnBaseBase saved;
	HostError	error;

	op.u.put.tuple.f0 = 3;
	txn_runs_tests_repository(io, 9424, __LINE__, 0, __LINE__, 0, &repo, &reading);
	for (size_t k = 0; k < lengthof(all); k++)
	{
		TxnStateMade made = {.table = all[k].relation + 16};
		HostSharedHeld held;
		TxnMemtableVersionHandle handle;

		txn_base_Key_Clone_clone(&key, &made.key);
		if (!txn_state_state(io, &repo, &reading, &all[k], persistences[k], true, &made, &held, &error))
			STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		if (!txn_memtable_table_push(io, host_shared_Held_record(&held), (const uint8 *) "k", 1, &change, &handle,
									 &error))
			STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		txn_state_moved(host_shared_Held_record(&held), 1, 4);
		host_shared_Held_drop(&held);
	}
	simc_checkpointed(UINT64CONST(0x100000a27));
	if (!txn_runs_checkpoint(io, UINT64CONST(0x100000a28), &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	for (size_t k = 0; k < lengthof(unlogged); k++)
	{
		STD_ASSERT(__LINE__, 0, txn_runs_tests_saved_base(io, &repo, &reading, &unlogged[k], &saved),
				   "the checkpoint saved an unlogged storage's base");
		STD_ASSERT_EQ_U64(__LINE__, 0, saved.counter, 4);
		txn_base_Base_drop(&saved);
	}
	root = host_versioned_dir(HOST_PG_DEFAULT);
	other = host_versioned_dir(16400);
	STD_ASSERT(__LINE__, 0, std_path_parent(root.bytes.ptr, root.bytes.len, &parent), "the versioned dir has a parent");
	std_path_join(parent.ptr, parent.len, "base", 4, &base_dir);
	std_PathBuf_push(&base_dir, "9424", 4);
	STD_ASSERT(__LINE__, 0, std_path_parent(other.bytes.ptr, other.bytes.len, &parent),
			   "the other versioned dir has a parent");
	std_path_join(parent.ptr, parent.len, "9424", 4, &other_dir);
	txn_runs_tests_made_dirs(io, &base_dir, 2);
	txn_runs_tests_made_dirs(io, &other_dir, 4);
	txn_runs_tests_made_file(io, &base_dir, "16384_init");
	txn_runs_tests_made_file(io, &base_dir, "16385");
	txn_runs_tests_made_file(io, &base_dir, "16386_init");
	txn_runs_tests_made_file(io, &other_dir, "16387_init");
	for (size_t k = 0; k < lengthof(all); k++)
		if (!txn_state_unlinked(io, &all[k], &error))
			STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
	for (int pass = 0; pass < 2; pass++)
	{
		StdVec		found;

		if (!txn_runs_cleaned(io, &error))
			STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		if (!txn_state_unlogged(io, &found, &error))
			STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		STD_ASSERT(__LINE__, 0, found.len == 2 &&
				   host_Storage_PartialEq_eq(&STD_VEC_AT(&found, HostStorage, 0), &unlogged[0]) &&
				   host_Storage_PartialEq_eq(&STD_VEC_AT(&found, HostStorage, 1), &unlogged[1]),
				   "the cleanup leaves the unlogged storages' states, in order");
		std_vec_free_Checked(&found, sizeof(HostStorage), _Alignof(HostStorage));
		for (size_t k = 0; k < lengthof(unlogged); k++)
		{
			HostSharedHeld held;
			TxnBaseKey	empty_key;
			TxnBaseBase empty;
			TxnBaseBase based;
			bool		some;

			if (!txn_state_state(io, &repo, &reading, &unlogged[k], TXN_STATE_PERSISTENCE_UNLOGGED, false, NULL, &held,
								 &error))
				STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
										 host_Error_Debug_fmt);
			if (!txn_state_base_record(io, &held, &some, &based, &error))
				STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
										 host_Error_Debug_fmt);
			txn_base_Key_Clone_clone(&key, &empty_key);
			empty = txn_base_Base_empty(&empty_key, unlogged[k].relation + 16, false, 0);
			STD_ASSERT_EQ(__LINE__, 0, some && txn_base_Base_PartialEq_eq(&based, &empty), &based,
						  txn_base_Base_Debug_fmt, &empty, txn_base_Base_Debug_fmt);
			STD_ASSERT_EQ_U64(__LINE__, 0, txn_state_counter(host_shared_Held_record(&held)), 1);
			if (some)
				txn_base_Base_drop(&based);
			txn_base_Base_drop(&empty);
			host_shared_Held_drop(&held);
		}
	}
	simc_checkpointed(UINT64CONST(0x100001a27));
	if (!txn_runs_checkpoint(io, UINT64CONST(0x100001a28), &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	for (size_t k = 0; k < lengthof(unlogged); k++)
	{
		TxnBaseKey	empty_key;
		TxnBaseBase empty;

		txn_base_Key_Clone_clone(&key, &empty_key);
		empty = txn_base_Base_empty(&empty_key, unlogged[k].relation + 16, false, 0);
		STD_ASSERT(__LINE__, 0, txn_runs_tests_saved_base(io, &repo, &reading, &unlogged[k], &saved),
				   "the second checkpoint saved an unlogged storage's base");
		STD_ASSERT_EQ(__LINE__, 0, txn_base_Base_PartialEq_eq(&saved, &empty), &saved, txn_base_Base_Debug_fmt,
					  &empty, txn_base_Base_Debug_fmt);
		txn_base_Base_drop(&saved);
		txn_base_Base_drop(&empty);
	}
	STD_ASSERT(__LINE__, 0, txn_runs_tests_saved_base(io, &repo, &reading, &permanent, &saved),
			   "the permanent storage keeps its saved base");
	STD_ASSERT_EQ_U64(__LINE__, 0, saved.counter, 4);
	txn_base_Base_drop(&saved);
	std_vec_free_Checked(&other_dir.bytes, 1, 1);
	std_vec_free_Checked(&base_dir.bytes, 1, 1);
	std_vec_free_Checked(&other.bytes, 1, 1);
	std_vec_free_Checked(&root.bytes, 1, 1);
	txn_memtable_Op_drop(&op);
	txn_base_Key_drop(&key);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	simc_TestIo_drop(&test_io);
}

/*
 * The loader's rule for a run it can't read (CommPF 7's O1): a start that
 * finds one repository's run file damaged warns of it once, frees the states
 * and the working state's delta it had already loaded for that repository,
 * and marks its database, so a make for either of its storages is refused
 * with the restart in its message, while the other repository's rows load.
 */
void
test_txn_runs_tests_a_start_skips_a_repository_whose_run_is_damaged_and_loads_the_others(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostStorage storages[3] = {{.tablespace = HOST_PG_DEFAULT, .database = 9425, .relation = 16384},
	{.tablespace = HOST_PG_DEFAULT, .database = 9425, .relation = 16385},
	{.tablespace = HOST_PG_DEFAULT, .database = 9426, .relation = 16384}};
	TxnInsertPage page = {.bytes = 8160, .most = 291};
	TxnRunsNames names = TXN_RUNS_TESTS_NAMES;
	uint8		address[TXN_MEMTABLE_BRANCH_ADDRESS];
	RepoRepo	repos[2];
	HostSharedReader reading;
	HostSharedHeld deltas;
	HostSharedHeld other;
	StdVec		chunk;
	txn_runs_tests_chunk_captures_t cc = {.chunk = &chunk, .has_lsn = true, .lsn = 0x0c00};
	StdPathBuf	path;
	StdVec		bytes;
	StdIoError	io_error;
	StdVec		warnings;
	StdVec		filled;
	HostError	error;
	char		name[128];
	bool		warned;
	bool		marked;
	bool		none = true;
	bool		some;
	bool		holds;
	ChunksErrorError failed;

	memset(address, 6, sizeof(address));
	if (!host_shared_read(io, &reading, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	for (size_t k = 0; k < lengthof(repos); k++)
		if (!repo_Repo_create(io, 9425 + (uint32) k, HOST_PG_DEFAULT, &repos[k], &failed))
			STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &failed,
									 chunks_error_Error_Debug_fmt);
	for (size_t k = 0; k < lengthof(storages); k++)
	{
		HostSharedHeld held = txn_runs_tests_stated(io, &repos[storages[k].database - 9425], &reading, &storages[k],
													__LINE__, 0);

		txn_runs_tests_pushed(io, host_shared_Held_record(&held), "k", __LINE__, 0);
		host_shared_Held_drop(&held);
	}
	if (!txn_memtable_branch_deltas(io, 9425, HOST_PG_DEFAULT, &deltas, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	std_slice_to_vec_Checked(&chunk, (const uint8 *) "chunk", 5, 1, 1);
	if (!txn_memtable_branch_prepare(io, host_shared_Held_record(&deltas), address, sizeof(address),
									 txn_memtable_version_largest(chunk.len), txn_runs_tests_chunk_prepared, &cc, NULL,
									 &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	std_vec_free_Checked(&chunk, 1, 1);
	host_shared_Held_drop(&deltas);
	simc_checkpointed(UINT64CONST(0x100000a27));
	if (!txn_runs_checkpoint(io, UINT64CONST(0x100000a28), &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	for (size_t k = 0; k < lengthof(storages); k++)
		if (!txn_state_unlinked(io, &storages[k], &error))
			STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
	if (!txn_memtable_branch_dropped(io, 9425, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	names.database = 9425;
	names.relation = 16385;
	txn_runs_directory(9425, HOST_PG_DEFAULT, UINT64CONST(0x100000a28), &path);
	std_PathBuf_push(&path, name, txn_runs_file_name(&names, name, sizeof(name)));
	if (!std_fs_read(path.bytes.ptr, path.bytes.len, &bytes, &io_error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &io_error,
								 std_io_Error_Debug_fmt);
	((uint8 *) bytes.ptr)[bytes.len - 1] ^= 0xff;
	if (!std_fs_write(path.bytes.ptr, path.bytes.len, bytes.ptr, bytes.len, &io_error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &io_error,
								 std_io_Error_Debug_fmt);
	std_vec_free_Checked(&bytes, 1, 1);
	std_vec_free_Checked(&path.bytes, 1, 1);
	warnings = simc_take_warnings();
	txn_runs_tests_warnings_free(&warnings);
	if (!txn_runs_loaded(io, &page, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	warnings = simc_take_warnings();
	warned = warnings.len == 1 &&
		memcmp(STD_VEC_AT(&warnings, simc_TupleSqlstateString_t, 0).f0, "XX001", 5) == 0 &&
		txn_runs_tests_contains(&STD_VEC_AT(&warnings, simc_TupleSqlstateString_t, 0).f1, "database 9425:");
	STD_ASSERT(__LINE__, 0, warned, "the start warns once of the damaged run, naming its database");
	txn_runs_tests_warnings_free(&warnings);
	if (!txn_state_marked(io, 9425, &marked, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(__LINE__, 0, marked, "state::marked(io, 9425).unwrap()");
	if (!txn_state_states(io, 9425, &filled, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	for (size_t k = 0; none && k < filled.len; k++)
	{
		HostStorage storage;
		TxnStatePersistence persistence;

		none = !txn_state_described(host_shared_Held_record(&STD_VEC_AT(&filled, txn_TupleU64HeldState_t, k).f1),
									&storage, &persistence);
	}
	STD_ASSERT(__LINE__, 0, none, "filled.iter().all(|(_, held)| state::described(held.record()).is_none())");
	txn_state_states_free(&filled);
	if (!txn_memtable_branch_find(HOST_SHARED_IO_WITHIN(io), 9425, &some, &deltas, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (some)
		host_shared_Held_drop(&deltas);
	STD_ASSERT(__LINE__, 0, !some, "the skipped repository's working state has no delta");
	other = txn_runs_tests_stated(io, &repos[1], &reading, &storages[2], __LINE__, 0);
	if (!txn_memtable_table_holds(io, host_shared_Held_record(&other), (const uint8 *) "k", 1, 9, &holds, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(__LINE__, 0, holds, "the other repository's row loads");
	host_shared_Held_drop(&other);
	for (size_t k = 0; k < 2; k++)
	{
		HostSharedHeld refused;
		StdStr		message;
		bool		restarts = false;

		STD_ASSERT(__LINE__, 0,
				   !txn_state_state(io, &repos[0], &reading, &storages[k], TXN_STATE_PERSISTENCE_PERMANENT, false, NULL,
									&refused, &error),
				   "a make for a storage of the skipped repository is refused");
		message = host_Error_message(&error);
		for (size_t at = 0; !restarts && at + 8 <= message.len; at++)
			restarts = memcmp(message.ptr + at, "restarts", 8) == 0;
		STD_ASSERT(__LINE__, 0, restarts, "the refusal names the restart");
		txn_runs_tests_sqlstate_is(__LINE__, 0, &error);
	}
	for (size_t k = 0; k < lengthof(repos); k++)
		repo_Repo_drop(&repos[k]);
	host_shared_Reader_drop(&reading);
	simc_TestIo_drop(&test_io);
}

/* txn_runs_tests_status_word returns a status run's version of an outcome word and its saved chain entries, 2. */
static StdVec
txn_runs_tests_status_word(uint64 word)
{
	uint8		bytes[16];
	StdVec		version;

	std_u64_to_le_bytes(word, bytes);
	std_u64_to_le_bytes(2, bytes + 8);
	std_slice_to_vec_Checked(&version, bytes, sizeof(bytes), 1, 1);
	return version;
}

/*
 * txn_runs_tests_status_file returns a status run's file of at most two
 * top-level xids, each key the xid's eight bytes big-endian and its versions
 * those given.
 */
static StdVec
txn_runs_tests_status_file(const TxnRunsNames *names, const uint64 *xids, StdVec *const *versions,
						   const size_t *versions_lens, size_t len)
{
	uint8		keys[2][8];
	txn_runs_tests_keyed_t keyed[2];

	for (size_t k = 0; k < len; k++)
	{
		std_u64_to_be_bytes(xids[k], keys[k]);
		keyed[k].key.ptr = keys[k];
		keyed[k].key.len = 8;
		keyed[k].versions = versions[k];
		keyed[k].versions_len = versions_lens[k];
	}
	return txn_runs_tests_file(names, keyed, len);
}

/*
 * Bugs row 71's status run: a file reads back each top-level xid with its
 * outcome, a commit record's end or pending in no process, and the decoder
 * refuses as XX001 an xid no entry can have, keys out of order or repeated, a
 * word that names a process, two words for one xid, and a word of another
 * length; decode refuses a status run as a run of versions.
 */
void
test_txn_runs_tests_a_status_run_reads_back_its_outcomes_and_refuses_what_isnt_one(void)
{
	TxnRunsNames status = TXN_RUNS_TESTS_NAMES;
	StdVec		committed = txn_runs_tests_status_word(0x0c00);
	StdVec		unowned = txn_runs_tests_status_word(0);
	StdVec		process = txn_runs_tests_status_word(UINT64CONST(1) << 62 | 3);
	StdVec		two[2];
	StdVec		short_word;
	uint8		word[8];
	StdVec	   *goods[2] = {&committed, &unowned};
	size_t		ones[2] = {1, 1};
	uint64		good_xids[2] = {5, 9};
	StdVec		good;
	StdVec		decoded;
	TxnRunsDecoded runs;
	HostError	error;
	struct
	{
		uint64		xids[2];
		StdVec	   *versions[2];
		size_t		versions_lens[2];
		size_t		len;
	}			cases[7] = {
		{{2}, {&committed}, {1}, 1},
		{{PG_UINT64_MAX}, {&committed}, {1}, 1},
		{{9, 5}, {&unowned, &committed}, {1, 1}, 2},
		{{5, 5}, {&committed, &unowned}, {1, 1}, 2},
		{{5}, {&process}, {1}, 1},
		{{5}, {two}, {2}, 1},
		{{5}, {&short_word}, {1}, 1},
	};

	status.kind = TXN_RUNS_KIND_STATUS;
	status.tablespace = 0;
	status.relation = 0;
	std_u64_to_le_bytes(0x0c00, word);
	std_slice_to_vec_Checked(&two[0], word, 8, 1, 1);
	std_u64_to_le_bytes(0, word);
	std_slice_to_vec_Checked(&two[1], word, 8, 1, 1);
	std_u32_to_le_bytes(0x0c00, word);
	std_slice_to_vec_Checked(&short_word, word, 4, 1, 1);
	good = txn_runs_tests_status_file(&status, good_xids, goods, ones, 2);
	if (!txn_runs_decode_status(good.ptr, good.len, &status, txn_runs_tests_sum(&good), &decoded, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	{
		const txn_TupleU64OutcomeU64_t *read = decoded.ptr;
		TxnStatusOutcome at_commit = {.tag = TXN_STATUS_OUTCOME_COMMITTED, .u.committed = 0x0c00};
		TxnStatusOutcome none = {.tag = TXN_STATUS_OUTCOME_UNOWNED};

		STD_ASSERT(__LINE__, 0, decoded.len == 2 && read[0].f0 == 5 &&
				   txn_status_Outcome_PartialEq_eq(&read[0].f1, &at_commit) && read[0].f2 == 2 &&
				   read[1].f0 == 9 && txn_status_Outcome_PartialEq_eq(&read[1].f1, &none) && read[1].f2 == 2,
				   "a status run reads back [(5, Committed(0x0c00), 2), (9, Unowned, 2)]");
	}
	std_vec_free_Checked(&decoded, sizeof(txn_TupleU64OutcomeU64_t), _Alignof(txn_TupleU64OutcomeU64_t));
	for (size_t at = 0; at < lengthof(cases); at++)
	{
		StdVec		bytes = txn_runs_tests_status_file(&status, cases[at].xids, cases[at].versions,
													   cases[at].versions_lens, cases[at].len);

		STD_ASSERT(__LINE__, 0,
				   !txn_runs_decode_status(bytes.ptr, bytes.len, &status, txn_runs_tests_sum(&bytes), &decoded,
										   &error),
				   "the status run decoder refuses a case");
		txn_runs_tests_sqlstate_is(__LINE__, 0, &error);
		std_vec_free_Checked(&bytes, 1, 1);
	}
	STD_ASSERT(__LINE__, 0, !txn_runs_decode(good.ptr, good.len, &status, txn_runs_tests_sum(&good), &runs, &error),
			   "decode refuses a status run as a run of versions");
	txn_runs_tests_sqlstate_is(__LINE__, 0, &error);
	std_vec_free_Checked(&good, 1, 1);
	std_vec_free_Checked(&short_word, 1, 1);
	std_vec_free_Checked(&two[1], 1, 1);
	std_vec_free_Checked(&two[0], 1, 1);
	std_vec_free_Checked(&process, 1, 1);
	std_vec_free_Checked(&unowned, 1, 1);
	std_vec_free_Checked(&committed, 1, 1);
}

/*
 * Bugs row 71: a checkpoint's status run holds the status table's entry of
 * each top-level transaction its runs' versions name, so after a restart's
 * table is gone the loader gives each loaded writer its outcome before redo:
 * a committed transaction its commit record's end, through its
 * subtransaction's version too, a running one pending in no process, which
 * its own commit record decides, and an aborted one, which has no entry, the
 * commit log's answer.
 */
void
test_txn_runs_tests_a_checkpoints_status_run_gives_the_loaded_writers_their_outcomes(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostStorage storage = {.tablespace = HOST_PG_DEFAULT, .database = 9427, .relation = 16384};
	TxnInsertPage page = {.bytes = 8160, .most = 291};
	TxnStatusOutcome at_commit = {.tag = TXN_STATUS_OUTCOME_COMMITTED, .u.committed = 0x0c00};
	TxnStatusOutcome pending = {.tag = TXN_STATUS_OUTCOME_PENDING_IN, .u.pending_in = 3};
	TxnStatusOutcome unowned = {.tag = TXN_STATUS_OUTCOME_UNOWNED};
	RepoRepo	repo;
	HostSharedReader reading;
	HostXid		committed;
	HostXid		running;
	HostXid		aborted;
	HostXid		sub;
	HostXid		removed[2];
	TxnStatusRoomed roomed;
	TxnStatusEntry entry;
	HostStatus	outcome;
	HostError	error;
	bool		took;
	bool		some;

	txn_runs_tests_repository(io, 9427, __LINE__, 0, __LINE__, 0, &repo, &reading);
	committed = simc_begin();
	running = simc_begin();
	aborted = simc_begin();
	sub = simc_begin_under(committed);
	if (!txn_status_take(io, committed, at_commit, &roomed, &took, &error) ||
		!txn_status_take(io, running, pending, &roomed, &took, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	{
		HostSharedHeld held = txn_runs_tests_stated(io, &repo, &reading, &storage, __LINE__, 0);
		TxnMemtableVersionWriter writers[4] = {{.xid = committed.f0}, {.xid = sub.f0, .has_top = true,
		.top = committed.f0, .command = 1}, {.xid = running.f0}, {.xid = aborted.f0}};
		const char *keys[4] = {"a", "b", "c", "d"};

		for (size_t k = 0; k < lengthof(writers); k++)
		{
			TxnMemtableOp op = txn_memtable_testing_put((const uint8 *) "v", 1);
			TxnMemtableVersionChange change = {.writer = writers[k], .op = &op, .lock = TXN_MEMTABLE_LOCK_MODE_UPDATE,
			.shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND}};
			TxnMemtableVersionHandle handle;

			op.u.put.tuple.f0 = 0;
			if (!txn_memtable_table_push(io, host_shared_Held_record(&held), (const uint8 *) keys[k], 1, &change,
										 &handle, &error))
				STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
										 host_Error_Debug_fmt);
			txn_memtable_Op_drop(&op);
		}
		host_shared_Held_drop(&held);
	}
	simc_commit(committed);
	simc_abort(aborted);
	simc_checkpointed(UINT64CONST(0x100000a27));
	if (!txn_runs_checkpoint(io, UINT64CONST(0x100000a28), &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!txn_state_unlinked(io, &storage, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	removed[0] = committed;
	removed[1] = running;
	for (size_t k = 0; k < lengthof(removed); k++)
	{
		bool		was;

		if (!txn_status_remove(io, removed[k], &was, &error))
			STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		STD_ASSERT(__LINE__, 0, was, "status::remove(io, xid).unwrap()");
	}
	if (!txn_runs_loaded(io, &page, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!txn_status_lookup(io, committed, &some, &entry, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT_EQ(__LINE__, 0, some && txn_status_Outcome_PartialEq_eq(&entry.outcome, &at_commit), &entry.outcome,
				  txn_status_Outcome_Debug_fmt, &at_commit, txn_status_Outcome_Debug_fmt);
	if (!txn_status_lookup(io, running, &some, &entry, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT_EQ(__LINE__, 0, some && txn_status_Outcome_PartialEq_eq(&entry.outcome, &unowned), &entry.outcome,
				  txn_status_Outcome_Debug_fmt, &unowned, txn_status_Outcome_Debug_fmt);
	if (!txn_status_lookup(io, aborted, &some, &entry, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(__LINE__, 0, !some, "the aborted transaction has no entry");
	if (!host_status(HOST_IO_IO_IO_DOORS(io), aborted, &outcome, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(__LINE__, 0, outcome == HOST_STATUS_ABORTED, "the commit log calls the aborted transaction aborted");
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	simc_TestIo_drop(&test_io);
}

/* txn_runs_tests_entry_version returns the record of an entries run's version of an op, by xid 7 with LSN 9. */
static StdVec
txn_runs_tests_entry_version(const TxnMemtableOp *op)
{
	TxnMemtableVersionWriter writer = {.xid = 7};
	TxnMemtableVersionShown shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND};
	StdVec		out;

	std_vec_new(&out, 1, 1);
	txn_memtable_version_encode(&writer, op, TXN_MEMTABLE_LOCK_MODE_UPDATE, shown, true, 9, &out);
	return out;
}

/* txn_runs_tests_entry_put returns an entries run's put or add of an entry, naming a tuple id with heap bytes. */
static TxnMemtableOp
txn_runs_tests_entry_put(TxnMemtableOpTag tag, uint64 tuple, uint32 heap_bytes)
{
	TxnMemtableOp op = txn_memtable_testing_put((const uint8 *) "entry", 5);

	op.tag = tag;
	op.u.put.tuple.f0 = tuple;
	op.u.put.heap_bytes = heap_bytes;
	return op;
}

/* txn_runs_tests_entries_file returns an entries run's file of key a holding versions. */
static StdVec
txn_runs_tests_entries_file(const TxnRunsNames *names, const StdVec *versions, size_t versions_len)
{
	txn_runs_tests_keyed_t keyed = {.key = {(const uint8 *) "a", 1}, .versions = versions,
	.versions_len = versions_len};

	return txn_runs_tests_file(names, &keyed, 1);
}

/*
 * PLAN's run header row, kind 8: an index's storage's entries run reads back
 * its puts naming no tuple id, its deletes, and its counted entries' added
 * and removed copies naming the copy's, and the decoder refuses as XX001 a
 * version with heap bytes, a put naming a tuple id, and any other op, here a
 * move, a row's delete, and a lock.
 */
void
test_txn_runs_tests_an_entries_run_reads_its_entries_and_refuses_heap_bytes_tuple_ids_and_other_ops(void)
{
	TxnRunsNames entries = TXN_RUNS_TESTS_NAMES;
	uint64		copy = UINT64CONST(1) << 16 | 2;
	TxnMemtableOp reads[4];
	TxnMemtableOp refuses[6];
	StdVec		read[4];
	StdVec		good;
	TxnRunsDecoded decoded;
	HostError	error;

	entries.kind = TXN_RUNS_KIND_ENTRIES;
	entries.relation = 16390;
	reads[0] = txn_runs_tests_entry_put(TXN_MEMTABLE_OP_PUT, 0, 0);
	reads[1] = (TxnMemtableOp)
	{
		.tag = TXN_MEMTABLE_OP_DELETE
	};
	reads[2] = txn_runs_tests_entry_put(TXN_MEMTABLE_OP_ADD, copy, 0);
	reads[3] = (TxnMemtableOp)
	{
		.tag = TXN_MEMTABLE_OP_REMOVE, .u.remove.f0.f0 = copy
	};
	for (size_t k = 0; k < lengthof(reads); k++)
		read[k] = txn_runs_tests_entry_version(&reads[k]);
	good = txn_runs_tests_entries_file(&entries, read, lengthof(read));
	if (!txn_runs_decode(good.ptr, good.len, &entries, txn_runs_tests_sum(&good), &decoded, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(__LINE__, 0, decoded.keys.len == 1 && STD_VEC_AT(&decoded.keys, TxnRunsKey, 0).versions.len == 4,
			   "decoded.keys.len() == 1 && decoded.keys[0].versions.len() == 4");
	txn_runs_Decoded_drop(&decoded);
	std_vec_free_Checked(&good, 1, 1);
	refuses[0] = txn_runs_tests_entry_put(TXN_MEMTABLE_OP_PUT, 0, 32);
	refuses[1] = txn_runs_tests_entry_put(TXN_MEMTABLE_OP_ADD, copy, 32);
	refuses[2] = txn_runs_tests_entry_put(TXN_MEMTABLE_OP_PUT, copy, 0);
	refuses[3] = (TxnMemtableOp)
	{
		.tag = TXN_MEMTABLE_OP_MOVE, .u.move.tag = TXN_MEMTABLE_SUCCESSOR_KEY
	};
	std_slice_to_vec_Checked(&refuses[3].u.move.u.key, (const uint8 *) "b", 1, 1, 1);
	refuses[4] = (TxnMemtableOp)
	{
		.tag = TXN_MEMTABLE_OP_DELETE_ROW, .u.delete_row.f0.f0 = copy
	};
	refuses[5] = (TxnMemtableOp)
	{
		.tag = TXN_MEMTABLE_OP_LOCK, .u.lock = TXN_MEMTABLE_LOCK_MODE_SHARE
	};
	for (size_t at = 0; at < lengthof(refuses); at++)
	{
		StdVec		one = txn_runs_tests_entry_version(&refuses[at]);
		StdVec		bytes = txn_runs_tests_entries_file(&entries, &one, 1);

		STD_ASSERT(__LINE__, 0,
				   !txn_runs_decode(bytes.ptr, bytes.len, &entries, txn_runs_tests_sum(&bytes), &decoded, &error),
				   "the entries run decoder refuses a case");
		txn_runs_tests_sqlstate_is(__LINE__, 0, &error);
		std_vec_free_Checked(&bytes, 1, 1);
		std_vec_free_Checked(&one, 1, 1);
		txn_memtable_Op_drop(&refuses[at]);
	}
	for (size_t k = 0; k < lengthof(reads); k++)
	{
		std_vec_free_Checked(&read[k], 1, 1);
		txn_memtable_Op_drop(&reads[k]);
	}
}

/*
 * Bugs row 90: a status run of more entries than the status table holds below
 * its load factor loads whole: the table grows during the load, which runs in
 * recovery, while no loaded version is counted yet, and the growth drops no
 * entry as ended there, so after it every committed transaction's entry has
 * its outcome and its loaded version's count.
 */
void
test_txn_runs_tests_a_large_status_run_keeps_every_entry_through_its_load(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostStorage storage = {.tablespace = HOST_PG_DEFAULT, .database = 9462, .relation = 16384};
	TxnInsertPage page = {.bytes = 8160, .most = 291};
	RepoRepo	repo;
	HostSharedReader reading;
	HostXid		xids[5000];
	HostError	error;

	txn_runs_tests_repository(io, 9462, __LINE__, 0, __LINE__, 0, &repo, &reading);
	{
		HostSharedHeld held = txn_runs_tests_stated(io, &repo, &reading, &storage, __LINE__, 0);

		for (uint64 n = 0; n < lengthof(xids); n++)
		{
			HostXid		xid = simc_begin();
			TxnStatusOutcome outcome = {.tag = TXN_STATUS_OUTCOME_COMMITTED, .u.committed = 0x0c00 + n};
			TxnMemtableOp op = txn_memtable_testing_put((const uint8 *) "v", 1);
			TxnMemtableVersionChange change = {.writer = {.xid = xid.f0}, .op = &op, .lock = TXN_MEMTABLE_LOCK_MODE_UPDATE,
			.shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND}};
			TxnMemtableVersionHandle handle;
			TxnStatusRoomed roomed;
			uint8		key[8];
			uint64		count;
			bool		took;
			bool		has_count;

			op.u.put.tuple.f0 = 0;
			std_u64_to_be_bytes(n, key);
			if (!txn_status_take(io, xid, outcome, &roomed, &took, &error) ||
				!txn_memtable_table_push(io, host_shared_Held_record(&held), key, sizeof(key), &change, &handle,
										 &error) ||
				!txn_status_add(io, xid, 1, &has_count, &count, &error))
				STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
										 host_Error_Debug_fmt);
			txn_memtable_Op_drop(&op);
			simc_commit(xid);
			xids[n] = xid;
		}
		host_shared_Held_drop(&held);
	}
	simc_checkpointed(UINT64CONST(0x100000a27));
	if (!txn_runs_checkpoint(io, UINT64CONST(0x100000a28), &error) || !txn_state_unlinked(io, &storage, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	for (size_t n = 0; n < lengthof(xids); n++)
	{
		bool		removed;

		if (!txn_status_remove(io, xids[n], &removed, &error))
			STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		STD_ASSERT(__LINE__, 0, removed, "status::remove(io, xid).unwrap()");
	}
	simc_unanswered(true);
	if (!txn_runs_loaded(io, &page, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	simc_unanswered(false);
	for (size_t n = 0; n < lengthof(xids); n++)
	{
		TxnStatusEntry wanted = {.outcome = {.tag = TXN_STATUS_OUTCOME_COMMITTED, .u.committed = 0x0c00 + n},
		.count = 1};
		TxnStatusEntry entry;
		bool		some;

		if (!txn_status_lookup(io, xids[n], &some, &entry, &error))
			STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		STD_ASSERT_EQ(__LINE__, 0, some && txn_status_Entry_PartialEq_eq(&entry, &wanted), &entry,
					  txn_status_Entry_Debug_fmt, &wanted, txn_status_Entry_Debug_fmt);
	}
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	simc_TestIo_drop(&test_io);
}

/* txn_runs_tests_ender returns a free space run's record with a row's ender added, as a dropped row's take is written (D2). */
static StdVec
txn_runs_tests_ender(StdVec record, uint32 xid)
{
	uint8		bytes[4];

	((uint8 *) record.ptr)[28] |= 128;
	std_u32_to_le_bytes(xid, bytes);
	std_vec_extend_from_slice_Checked(&record, bytes, sizeof(bytes), 1, 1);
	return record;
}

/*
 * D2's run form: a row's ender rides only on a free space run's take of a
 * tuple id by a transaction, at or past the first normal xid, and a decoder
 * refuses it on a rows or a values run's version, on a take no transaction
 * wrote, on a version that names no tuple id, and below the first normal xid.
 */
void
test_txn_runs_tests_a_runs_decoder_takes_a_rows_ender_only_on_a_transactions_take(void)
{
	TxnRunsNames free = TXN_RUNS_TESTS_NAMES;
	TxnRunsNames values = TXN_RUNS_TESTS_NAMES;
	TxnRunsNames cases_names[5];
	uint8		block[4];
	TxnMemtableOp op = txn_runs_tests_entry_put(TXN_MEMTABLE_OP_PUT, 0, 0);
	TxnMemtableVersionWriter writer = {.xid = 7};
	TxnMemtableVersionShown shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND};
	StdVec		untaken;
	StdVec		versions[5];
	StdVec		taken_version;
	StdVec		taken;
	TxnRunsDecoded decoded;
	HostError	error;
	uint32		ender;

	free.kind = TXN_RUNS_KIND_FREE;
	values.kind = TXN_RUNS_KIND_VALUES;
	std_u32_to_be_bytes(1, block);
	std_vec_new(&untaken, 1, 1);
	txn_memtable_version_encode(&writer, &op, TXN_MEMTABLE_LOCK_MODE_UPDATE, shown, false, 0, &untaken);
	txn_memtable_Op_drop(&op);
	versions[0] = txn_runs_tests_ender(txn_runs_tests_record(7, (const uint8 *) "row", 3, false, 0), 9);
	cases_names[0] = TXN_RUNS_TESTS_NAMES;
	versions[1] = txn_runs_tests_ender(txn_runs_tests_entry_record(7, (const uint8 *) "value", 5, false, 0), 9);
	cases_names[1] = values;
	versions[2] = txn_runs_tests_ender(txn_runs_tests_entry_record(TXN_MEMTABLE_VERSION_NO_TRANSACTION.xid,
																   (const uint8 *) "entry", 5, false, 0), 9);
	cases_names[2] = free;
	versions[3] = txn_runs_tests_ender(untaken, 9);
	cases_names[3] = free;
	versions[4] = txn_runs_tests_ender(txn_runs_tests_entry_record(7, (const uint8 *) "entry", 5, false, 0), 2);
	cases_names[4] = free;
	for (size_t at = 0; at < lengthof(versions); at++)
	{
		txn_runs_tests_keyed_t keyed = {.key = {cases_names[at].kind == TXN_RUNS_KIND_ROWS ?
		(const uint8 *) "a" : block, cases_names[at].kind == TXN_RUNS_KIND_ROWS ? 1 : sizeof(block)},
		.versions = &versions[at], .versions_len = 1};
		StdVec		bytes = txn_runs_tests_file(&cases_names[at], &keyed, 1);

		STD_ASSERT(__LINE__, 0,
				   !txn_runs_decode(bytes.ptr, bytes.len, &cases_names[at], txn_runs_tests_sum(&bytes), &decoded,
									&error),
				   "the decoder refuses an ender off a transaction's take");
		txn_runs_tests_sqlstate_is(__LINE__, 0, &error);
		std_vec_free_Checked(&bytes, 1, 1);
		std_vec_free_Checked(&versions[at], 1, 1);
	}
	taken_version = txn_runs_tests_ender(txn_runs_tests_entry_record(7, (const uint8 *) "entry", 5, false, 0), 9);
	{
		txn_runs_tests_keyed_t keyed = {.key = {block, sizeof(block)}, .versions = &taken_version, .versions_len = 1};

		taken = txn_runs_tests_file(&free, &keyed, 1);
	}
	if (!txn_runs_decode(taken.ptr, taken.len, &free, txn_runs_tests_sum(&taken), &decoded, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(__LINE__, 0,
			   txn_memtable_version_Record_ender(&STD_VEC_AT(&STD_VEC_AT(&decoded.keys, TxnRunsKey, 0).versions,
															 TxnMemtableVersionRecord, 0), &ender) && ender == 9,
			   "read.keys[0].versions[0].ender() == Some(9)");
	txn_runs_Decoded_drop(&decoded);
	std_vec_free_Checked(&taken, 1, 1);
	std_vec_free_Checked(&taken_version, 1, 1);
}

/* txn_runs_tests_row_one is the ender case's take's tuple: the row's tuple id, 1. */
static HostRow
txn_runs_tests_row_one(void *captures)
{
	HostRow		tuple = {1};

	return tuple;
}

/* txn_runs_tests_ender_prepared is the ender case's prepare: the free space entry's take by xid 7 naming tuple id 1. */
static bool
txn_runs_tests_ender_prepared(void *captures, HostIoIo *io, TxnMemtableTablePreparedState *prepared, HostError *error)
{
	TxnMemtableVersionWriter writer = {.xid = 7};
	HostRow		none = {0};
	TxnMemtableMapTaken taken;

	return txn_memtable_map_took_Unit(HOST_IO_IO_TAKES_SHORT(io), prepared, 0, writer, false, 0, false, none, 0, false,
									  NULL, 0, txn_runs_tests_put_step, captures, NULL, txn_runs_tests_row_one, NULL,
									  NULL, txn_runs_tests_nothing, NULL, NULL, &taken, error);
}

/*
 * D2's checkpoint case: a take whose row a fold's drop took out keeps its
 * row's ender through a checkpoint's free space run and the start's load,
 * which puts it back in the take's hint word, so VACUUM reads it after a
 * restart.
 */
void
test_txn_runs_tests_a_takes_ender_loads_back_from_its_free_space_run(void)
{
	static const uint16 blocks[] = {2, 3};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostStorage storage = {.tablespace = HOST_PG_DEFAULT, .database = 9460, .relation = 16384};
	TxnFreeSpaceFree made = txn_free_space_Free_new(100, blocks, lengthof(blocks), 291);
	TxnInsertPage page = {.bytes = 8160, .most = 291};
	RepoRepo	repo;
	HostSharedReader reading;
	StdVec		free;
	StdBytes	value;
	StdVec		before;
	StdVec		after;
	StdVec		takers;
	HostError	error;
	uint8		nine[4];
	bool		found = false;

	txn_runs_tests_repository(io, 9460, __LINE__, 0, __LINE__, 0, &repo, &reading);
	txn_free_space_Free_encode(&made, &free);
	value.ptr = free.ptr;
	value.len = free.len;
	{
		HostSharedHeld held = txn_runs_tests_stated(io, &repo, &reading, &storage, __LINE__, 0);
		TxnMemtableMapTake take = {.state = host_shared_Held_record(&held), .map = TXN_MEMTABLE_MAP_MAP_FREE,
		.most = free.len};
		HostRow		one = {1};

		if (!txn_memtable_map_prepare_Unit(io, &take, 1, txn_runs_tests_ender_prepared, &value, NULL, &error) ||
			!txn_memtable_map_ended(io, host_shared_Held_record(&held), one, 9, &error))
			STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		txn_state_moved(host_shared_Held_record(&held), 1, (UINT64CONST(4) << 16) | 1);
		before = txn_runs_tests_chains(&TXN_MEMTABLE_MAP_FREE_TREE_OPS, io, host_shared_Held_record(&held));
		host_shared_Held_drop(&held);
	}
	std_u32_to_le_bytes(9, nine);
	{
		const StdVec *first = &STD_VEC_AT(&STD_VEC_AT(&before, txn_runs_tests_chain_t, 0).records, StdVec, 0);

		STD_ASSERT(__LINE__, 0, first->len >= 4 && memcmp((const uint8 *) first->ptr + first->len - 4, nine, 4) == 0,
				   "before[0].1[0].ends_with(&9u32.to_le_bytes())");
	}
	simc_checkpointed(UINT64CONST(0x100000a27));
	if (!txn_runs_checkpoint(io, UINT64CONST(0x100000a28), &error) || !txn_state_unlinked(io, &storage, &error) ||
		!txn_runs_loaded(io, &page, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	{
		HostSharedHeld held = txn_runs_tests_stated(io, &repo, &reading, &storage, __LINE__, 0);
		const StdVec *was = &STD_VEC_AT(&before, txn_runs_tests_chain_t, 0).records;
		const StdVec *now;
		bool		starts;

		after = txn_runs_tests_chains(&TXN_MEMTABLE_MAP_FREE_TREE_OPS, io, host_shared_Held_record(&held));
		now = &STD_VEC_AT(&after, txn_runs_tests_chain_t, 0).records;
		starts = now->len >= was->len;
		for (size_t k = 0; starts && k < was->len; k++)
		{
			const StdVec *a = &STD_VEC_AT(now, StdVec, k);
			const StdVec *b = &STD_VEC_AT(was, StdVec, k);

			starts = a->len == b->len && memcmp(a->ptr, b->ptr, a->len) == 0;
		}
		STD_ASSERT(__LINE__, 0, starts, "the loader's move went over the run's entry");
		if (!txn_memtable_map_takers(io, host_shared_Held_record(&held), TXN_MEMTABLE_MAP_MAP_FREE, 0, &takers,
									 &error))
			STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		for (size_t k = 0; !found && k < takers.len; k++)
		{
			const txn_TupleWriterRowEntryU64TupleVecU32Bool_t *taker =
				&STD_VEC_AT(&takers, txn_TupleWriterRowEntryU64TupleVecU32Bool_t, k);

			if (taker->f1.f0 == 1)
			{
				found = true;
				STD_ASSERT_EQ_U64(__LINE__, 0, taker->f3, 9);
			}
		}
		STD_ASSERT(__LINE__, 0, found, "a taker of tuple id 1 is read");
		txn_memtable_map_takers_drop(&takers);
		host_shared_Held_drop(&held);
	}
	txn_runs_tests_chains_free(&after);
	txn_runs_tests_chains_free(&before);
	std_vec_free_Checked(&free, 1, 1);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	simc_TestIo_drop(&test_io);
}

/*
 * Bugs row 161: an index storage's entries run names its table and index in
 * its header's tail, so a start that loads it makes the index's state as
 * redo's make does, and the next checkpoint writes its entries as an entries
 * run again, which the start after loads, where a state with no base wrote
 * them as a rows run its load refused.
 */
void
test_txn_runs_tests_an_index_storages_loaded_entries_stay_an_entries_run_through_a_second_checkpoint(void)
{
	static const HostLsn redos[2] = {UINT64CONST(0x100000a28), UINT64CONST(0x100000b28)};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostStorage storage = {.tablespace = HOST_PG_DEFAULT, .database = 9490, .relation = 16390};
	TxnStateMade made = {.key = {.tag = TXN_BASE_KEY_NONE}, .table = 16385, .has_index = true, .index = 16390};
	RepoRepo	repo;
	HostSharedReader reading;
	StdVec		before;
	HostError	error;

	txn_runs_tests_repository(io, 9490, __LINE__, 0, __LINE__, 0, &repo, &reading);
	{
		HostSharedHeld held;
		TxnMemtableOp op = txn_runs_tests_entry_put(TXN_MEMTABLE_OP_PUT, 0, 0);
		TxnMemtableVersionChange change = {.writer = {.xid = 7}, .op = &op, .lock = TXN_MEMTABLE_LOCK_MODE_UPDATE,
		.shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND}, .has_lsn = true, .lsn = 0x0a28};
		TxnMemtableVersionHandle handle;

		if (!txn_state_state(io, &repo, &reading, &storage, TXN_STATE_PERSISTENCE_PERMANENT, true, &made, &held,
							 &error) ||
			!txn_memtable_table_push(io, host_shared_Held_record(&held), (const uint8 *) "k", 1, &change, &handle,
									 &error))
			STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		before = txn_runs_tests_chains(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, io, host_shared_Held_record(&held));
		txn_memtable_Op_drop(&op);
		host_shared_Held_drop(&held);
	}
	for (size_t k = 0; k < lengthof(redos); k++)
	{
		HostSharedHeld held;
		StdPathBuf	path;
		StdVec		read;
		StdVec		after;
		TxnBaseBase base;
		ChunksErrorError failed;
		bool		some;

		simc_checkpointed(redos[k] - 1);
		if (!txn_runs_checkpoint(io, redos[k], &error))
			STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		txn_runs_directory(9490, HOST_PG_DEFAULT, redos[k], &path);
		std_PathBuf_push(&path, "entries-1663-16390", 18);
		if (!chunks_file_read_file(HOST_IO_IO_READ_DOORS(io), path.bytes.ptr, path.bytes.len, &read, &failed))
			STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &failed,
									 chunks_error_Error_Debug_fmt);
		std_vec_free_Checked(&read, 1, 1);
		std_vec_free_Checked(&path.bytes, 1, 1);
		if (!txn_state_unlinked(io, &storage, &error))
			STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		STD_ASSERT(__LINE__, 0, !txn_runs_tests_skipped_at(io, 9490), "the start loads the entries run");
		held = txn_runs_tests_stated(io, &repo, &reading, &storage, __LINE__, 0);
		if (!txn_state_base_record(io, &held, &some, &base, &error))
			STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		STD_ASSERT(__LINE__, 0, some, "state::base_record(io, &held).unwrap().unwrap()");
		after = txn_runs_tests_chains(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, io, host_shared_Held_record(&held));
		STD_ASSERT(__LINE__, 0, base.table == 16385 && base.has_index && base.index == 16390 &&
				   txn_runs_tests_chains_eq(&after, &before),
				   "(base.table, base.index, chains) == (16385, Some(16390), before)");
		txn_runs_tests_chains_free(&after);
		txn_base_Base_drop(&base);
		host_shared_Held_drop(&held);
	}
	txn_runs_tests_chains_free(&before);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	simc_TestIo_drop(&test_io);
}

/*
 * Bugs row 161's header form: an entries run's header holds its table's and
 * index's object ids past its redo point and reads them back, and the decoder
 * refuses as XX001 an entries run's header without them and a rows run's with
 * them.
 */
void
test_txn_runs_tests_an_entries_runs_header_holds_its_table_and_index(void)
{
	TxnRunsNames entries = TXN_RUNS_TESTS_NAMES;
	TxnRunsNames rows_names = TXN_RUNS_TESTS_NAMES;
	TxnMemtableOp delete = {.tag = TXN_MEMTABLE_OP_DELETE};
	TxnMemtableVersionWriter writer = {.xid = 7};
	TxnMemtableVersionShown shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND};
	StdVec		one;
	StdVec		row;
	StdVec		good;
	StdVec		rows;
	StdVec		entries_header;
	StdVec		rows_header;
	StdVec		keys[2];
	StdVec		cases[2];
	TxnRunsNames cases_names[2];
	TxnRunsDecoded decoded;
	HostError	error;

	entries.kind = TXN_RUNS_KIND_ENTRIES;
	entries.relation = 16390;
	std_vec_new(&one, 1, 1);
	txn_memtable_version_encode(&writer, &delete, TXN_MEMTABLE_LOCK_MODE_UPDATE, shown, true, 9, &one);
	{
		txn_runs_tests_keyed_t keyed = {.key = {(const uint8 *) "a", 1}, .versions = &one, .versions_len = 1};

		good = txn_runs_tests_file(&entries, &keyed, 1);
	}
	if (!txn_runs_decode(good.ptr, good.len, &entries, txn_runs_tests_sum(&good), &decoded, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(__LINE__, 0, decoded.tail.tag == TXN_RUNS_TAIL_INDEX && decoded.tail.table == 16385 &&
			   decoded.tail.index == 16390, "decode(&good).tail == Tail::Index { table: 16385, index: 16390 }");
	txn_runs_Decoded_drop(&decoded);
	row = txn_runs_tests_record(7, (const uint8 *) "one", 3, true, 9);
	{
		txn_runs_tests_keyed_t keyed = {.key = {(const uint8 *) "a", 1}, .versions = &row, .versions_len = 1};

		rows = txn_runs_tests_file(&rows_names, &keyed, 1);
	}
	if (!txn_runs_decode(rows.ptr, rows.len, &rows_names, txn_runs_tests_sum(&rows), &decoded, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(__LINE__, 0, decoded.tail.tag == TXN_RUNS_TAIL_NONE, "decode(&rows).tail == Tail::None");
	txn_runs_Decoded_drop(&decoded);
	entries_header = txn_runs_tests_header(&entries);
	rows_header = txn_runs_tests_header(&rows_names);
	keys[0] = txn_runs_tests_keyed((const uint8 *) "a", 1, &one, 1);
	keys[1] = txn_runs_tests_keyed((const uint8 *) "a", 1, &row, 1);
	cases[0] = txn_runs_tests_raw(entries_header.ptr, TXN_RUNS_HEADER, &keys[0], 1, 1, 0);
	cases_names[0] = entries;
	std_vec_extend_from_slice_Checked(&rows_header, (const uint8 *) entries_header.ptr + TXN_RUNS_HEADER,
									  entries_header.len - TXN_RUNS_HEADER, 1, 1);
	cases[1] = txn_runs_tests_raw(rows_header.ptr, rows_header.len, &keys[1], 1, 1, 0);
	cases_names[1] = rows_names;
	for (size_t at = 0; at < lengthof(cases); at++)
	{
		STD_ASSERT(__LINE__, 0,
				   !txn_runs_decode(cases[at].ptr, cases[at].len, &cases_names[at], txn_runs_tests_sum(&cases[at]),
									&decoded, &error),
				   "the decoder refuses a header whose tail isn't its kind's");
		txn_runs_tests_sqlstate_is(__LINE__, 0, &error);
		std_vec_free_Checked(&cases[at], 1, 1);
		std_vec_free_Checked(&keys[at], 1, 1);
	}
	std_vec_free_Checked(&rows_header, 1, 1);
	std_vec_free_Checked(&entries_header, 1, 1);
	std_vec_free_Checked(&rows, 1, 1);
	std_vec_free_Checked(&row, 1, 1);
	std_vec_free_Checked(&good, 1, 1);
	std_vec_free_Checked(&one, 1, 1);
}

/* TXN_RUNS_TESTS_SPILLED is SPILLED, a test's spill of a storage's rows by the transaction of xid 742. */
static const TxnRunsNames TXN_RUNS_TESTS_SPILLED = {.kind = TXN_RUNS_KIND_SPILLED_ROWS,
	.persistence = TXN_STATE_PERSISTENCE_PERMANENT, .tablespace = 1663, .database = 9400, .relation = 16384,
.redo = 742};

/* txn_runs_tests_sum_spill returns the xxh3 a spill's file's trailer holds. */
static uint64
txn_runs_tests_sum_spill(const StdVec *bytes)
{
	return std_u64_from_le_bytes((const uint8 *) bytes->ptr + bytes->len - 32);
}

/*
 * Bugs row 74's spill: a spill's file reads back as written, its index naming
 * the first key frame and one at or past each 64 KiB step after it, each
 * entry's key that frame's.
 */
void
test_txn_runs_tests_a_spills_file_reads_back_with_its_index(void)
{
	uint8		keys[200][4];
	StdVec		versions[200][2];
	txn_runs_tests_keyed_t keyed[200];
	StdVec		bytes;
	StdBytes	body;
	StdBytes	index_frame;
	StdVec		entries;
	TxnRunsDecoded decoded;
	HostError	error;
	size_t		index_at;
	size_t		filter_at;
	size_t		next;
	bool		all = true;
	uint8		zero[4];

	for (uint32 key = 0; key < lengthof(keys); key++)
	{
		uint8		value[1000];

		memset(value, (uint8) key, sizeof(value));
		std_u32_to_be_bytes(key, keys[key]);
		versions[key][0] = txn_runs_tests_record(742, value, sizeof(value), true, 9);
		versions[key][1] = txn_runs_tests_record(742, (const uint8 *) "v", 1, false, 0);
		keyed[key].key.ptr = keys[key];
		keyed[key].key.len = 4;
		keyed[key].versions = versions[key];
		keyed[key].versions_len = 2;
	}
	bytes = txn_runs_tests_file(&TXN_RUNS_TESTS_SPILLED, keyed, lengthof(keyed));
	if (!txn_runs_decode(bytes.ptr, bytes.len, &TXN_RUNS_TESTS_SPILLED, txn_runs_tests_sum_spill(&bytes), &decoded,
						 &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT_EQ_U64(__LINE__, 0, decoded.keys.len, 200);
	for (size_t k = 0; all && k < decoded.keys.len; k++)
	{
		const TxnRunsKey *read = &STD_VEC_AT(&decoded.keys, TxnRunsKey, k);

		all = read->key.len == 4 && memcmp(read->key.ptr, keys[k], 4) == 0 && read->versions.len == 2;
	}
	STD_ASSERT(__LINE__, 0, all, "each key reads back with its two versions");
	txn_runs_Decoded_drop(&decoded);
	std_vec_new(&entries, sizeof(txn_TupleRefSliceU8U64_t), _Alignof(txn_TupleRefSliceU8U64_t));
	body.ptr = (const uint8 *) bytes.ptr + sizeof(TXN_RUNS_MAGIC);
	body.len = bytes.len - sizeof(TXN_RUNS_MAGIC);
	if (!txn_runs_spill_tail(body, &index_at, &filter_at, &error) ||
		!txn_runs_frame(body.ptr, body.len, index_at, &index_frame, &next, &error) ||
		!txn_runs_index(index_frame.ptr, index_frame.len, &entries, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(__LINE__, 0, entries.len >= 3, "the index holds at least three entries");
	std_u32_to_be_bytes(0, zero);
	{
		const txn_TupleRefSliceU8U64_t *first = &STD_VEC_AT(&entries, txn_TupleRefSliceU8U64_t, 0);

		STD_ASSERT(__LINE__, 0, first->f0.len == 4 && memcmp(first->f0.ptr, zero, 4) == 0,
				   "entries[0].0 == 0u32.to_be_bytes()");
	}
	for (size_t k = 1; k < entries.len; k++)
		STD_ASSERT(__LINE__, 0,
				   STD_VEC_AT(&entries, txn_TupleRefSliceU8U64_t, k).f1 -
				   STD_VEC_AT(&entries, txn_TupleRefSliceU8U64_t, k - 1).f1 >= TXN_RUNS_STRIDE,
				   "each entry is a stride or more past the one before");
	for (size_t k = 0; k < entries.len; k++)
	{
		const txn_TupleRefSliceU8U64_t *entry = &STD_VEC_AT(&entries, txn_TupleRefSliceU8U64_t, k);
		StdBytes	record;
		StdBytes	key;
		size_t		cursor = 0;
		size_t		len;

		if (!txn_runs_frame(bytes.ptr, bytes.len, (size_t) entry->f1, &record, &next, &error) ||
			!txn_runs_varint(record, &cursor, &len, &error) || !txn_runs_part(record, &cursor, len, &key, &error))
			STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		STD_ASSERT(__LINE__, 0, key.len == entry->f0.len && memcmp(key.ptr, entry->f0.ptr, key.len) == 0,
				   "each entry's key is its frame's");
	}
	std_vec_free_Checked(&entries, sizeof(txn_TupleRefSliceU8U64_t), _Alignof(txn_TupleRefSliceU8U64_t));
	std_vec_free_Checked(&bytes, 1, 1);
	for (size_t key = 0; key < lengthof(keys); key++)
	{
		std_vec_free_Checked(&versions[key][0], 1, 1);
		std_vec_free_Checked(&versions[key][1], 1, 1);
	}
}

/* txn_runs_tests_index_entry_t is spill_raw's index entry: the key frame's number and the key the entry names. */
typedef struct txn_runs_tests_index_entry_t
{
	size_t		frame;
	const char *key;
} txn_runs_tests_index_entry_t;

/*
 * txn_runs_tests_spill_raw returns a spill's file built by hand: its
 * header's bytes, each key frame's record, an index of the key frames'
 * numbers, each entry naming that frame's place with a key given, and its
 * count of versions.
 */
static StdVec
txn_runs_tests_spill_raw(const StdVec *records, size_t records_len, const txn_runs_tests_index_entry_t *index,
						 size_t index_len, uint64 count)
{
	StdVec		out;
	StdVec		header = txn_runs_tests_header(&TXN_RUNS_TESTS_SPILLED);
	StdVec		entries;
	uint64		places[2];
	uint64		at;
	uint8		trailer[32];

	std_slice_to_vec_Checked(&out, TXN_RUNS_MAGIC, sizeof(TXN_RUNS_MAGIC), 1, 1);
	txn_runs_framed(&out, header.ptr, header.len);
	std_vec_free_Checked(&header, 1, 1);
	for (size_t k = 0; k < records_len; k++)
	{
		places[k] = out.len;
		txn_runs_framed(&out, records[k].ptr, records[k].len);
	}
	at = out.len;
	std_vec_new(&entries, 1, 1);
	chunks_varint_put_varint(&entries, (uint64) index_len);
	for (size_t k = 0; k < index_len; k++)
	{
		chunks_varint_put_varint(&entries, (uint64) strlen(index[k].key));
		std_vec_extend_from_slice_Checked(&entries, (const uint8 *) index[k].key, strlen(index[k].key), 1, 1);
		chunks_varint_put_varint(&entries, places[index[k].frame]);
	}
	txn_runs_framed(&out, entries.ptr, entries.len);
	std_vec_free_Checked(&entries, 1, 1);
	std_u64_to_le_bytes(count, trailer);
	std_u64_to_le_bytes(XXH3_64bits((const uint8 *) out.ptr + sizeof(TXN_RUNS_MAGIC),
									out.len - sizeof(TXN_RUNS_MAGIC)), trailer + 8);
	std_u64_to_le_bytes(at, trailer + 16);
	std_u64_to_le_bytes(out.len, trailer + 24);
	txn_runs_framed(&out, trailer, sizeof(trailer));
	return out;
}

/*
 * Bugs row 74's spill: the decoder refuses as data corruption a spill holding
 * another transaction's version, an index entry that names another key or a
 * place that isn't a key frame's start, entries out of order, and an index
 * with no entry for a spill with a key, and reads one built by hand that is
 * right.
 */
void
test_txn_runs_tests_a_spills_decoder_refuses_another_writer_and_a_wrong_index(void)
{
	static const txn_runs_tests_index_entry_t both[2] = {{0, "a"}, {1, "b"}};
	static const txn_runs_tests_index_entry_t first_only[1] = {{0, "a"}};
	static const txn_runs_tests_index_entry_t wrong_key[1] = {{0, "b"}};
	static const txn_runs_tests_index_entry_t reversed[2] = {{1, "b"}, {0, "a"}};
	StdVec		versions[3];
	StdVec		frames[3];
	StdVec		good;
	StdVec		refused[4];
	TxnRunsDecoded decoded;
	HostError	error;

	versions[0] = txn_runs_tests_record(742, (const uint8 *) "1", 1, false, 0);
	versions[1] = txn_runs_tests_record(742, (const uint8 *) "2", 1, false, 0);
	versions[2] = txn_runs_tests_record(743, (const uint8 *) "2", 1, false, 0);
	frames[0] = txn_runs_tests_keyed((const uint8 *) "a", 1, &versions[0], 1);
	frames[1] = txn_runs_tests_keyed((const uint8 *) "b", 1, &versions[1], 1);
	frames[2] = txn_runs_tests_keyed((const uint8 *) "b", 1, &versions[2], 1);
	good = txn_runs_tests_spill_raw(frames, 2, both, lengthof(both), 2);
	if (!txn_runs_decode(good.ptr, good.len, &TXN_RUNS_TESTS_SPILLED, txn_runs_tests_sum_spill(&good), &decoded,
						 &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT_EQ_U64(__LINE__, 0, decoded.keys.len, 2);
	txn_runs_Decoded_drop(&decoded);
	{
		StdVec		others[2] = {frames[0], frames[2]};

		refused[0] = txn_runs_tests_spill_raw(others, 2, first_only, lengthof(first_only), 2);
	}
	refused[1] = txn_runs_tests_spill_raw(frames, 2, wrong_key, lengthof(wrong_key), 2);
	refused[2] = txn_runs_tests_spill_raw(frames, 2, reversed, lengthof(reversed), 2);
	refused[3] = txn_runs_tests_spill_raw(frames, 2, NULL, 0, 2);
	for (size_t at = 0; at < lengthof(refused); at++)
	{
		STD_ASSERT(__LINE__, 0,
				   !txn_runs_decode(refused[at].ptr, refused[at].len, &TXN_RUNS_TESTS_SPILLED,
									txn_runs_tests_sum_spill(&refused[at]), &decoded, &error),
				   "the spill decoder refuses a case");
		txn_runs_tests_sqlstate_is(__LINE__, 0, &error);
		std_vec_free_Checked(&refused[at], 1, 1);
	}
	std_vec_free_Checked(&good, 1, 1);
	for (size_t k = 0; k < lengthof(frames); k++)
	{
		std_vec_free_Checked(&frames[k], 1, 1);
		std_vec_free_Checked(&versions[k], 1, 1);
	}
}

/* txn_runs_tests_spill_files counts a repository's spill files in pg_default, leaving out its spills' manifests. */
static size_t
txn_runs_tests_spill_files(HostIoIo *io, uint32 repository)
{
	StdPathBuf	dir;
	StdVec		names;
	HostError	error;
	bool		some;
	size_t		files = 0;

	txn_runs_spill_directory(repository, HOST_PG_DEFAULT, &dir);
	if (!host_list_dir_if_there(HOST_IO_IO_READ_DOORS(io), dir.bytes.ptr, dir.bytes.len, &some, &names, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	if (!some)
		return 0;
	for (size_t k = 0; k < names.len; k++)
	{
		const StdString *name = &STD_VEC_AT(&names, StdString, k);

		files += name->len < strlen(TXN_SPILL_MANIFEST) ||
			memcmp(name->ptr, TXN_SPILL_MANIFEST, strlen(TXN_SPILL_MANIFEST)) != 0;
	}
	txn_runs_names_drop(&names);
	return files;
}

/*
 * Bugs row 74's checkpoint: a checkpoint names a running transaction's spill
 * files in the manifest beside its runs, with the transaction's status entry,
 * and a start links them back, their rows and tuple ids read again; once the
 * transaction aborts, its files leave the chain, and the second checkpoint
 * after, which neither generation of the manifest names them in, keeps them
 * for a reader begun before (bugs row 124's M3), and the next checkpoint
 * after that reader ended removes them.
 */
void
test_txn_runs_tests_a_checkpoint_names_a_running_spill_and_a_start_links_it_back(void)
{
	static const HostLsn later[3][2] = {{UINT64CONST(0x100000b27), UINT64CONST(0x100000b28)},
	{UINT64CONST(0x100000c27), UINT64CONST(0x100000c28)}, {UINT64CONST(0x100000c37), UINT64CONST(0x100000c38)}};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostStorage storage = {.tablespace = HOST_PG_DEFAULT, .database = 9748, .relation = 16384};
	TxnInsertPage page = {.bytes = 8160, .most = 291};
	TxnStatusOutcome unowned = {.tag = TXN_STATUS_OUTCOME_UNOWNED};
	RepoRepo	repo;
	HostSharedReader reading;
	HostSharedHeld held;
	HostXid		mine;
	StdArcInner *manifest;
	TxnStatusRoomed roomed;
	TxnStatusEntry entry;
	ChunksErrorError failed;
	HostError	error;
	bool		took;
	bool		some;
	bool		spilled;

	txn_runs_tests_repository(io, 9748, __LINE__, 0, __LINE__, 0, &repo, &reading);
	mine = simc_begin();
	held = txn_runs_tests_stated(io, &repo, &reading, &storage, __LINE__, 0);
	for (uint64 number = 1; number <= 3; number++)
	{
		TxnMemtableOp op = txn_memtable_testing_put((const uint8 *) "v", 1);
		TxnMemtableVersionChange change = {.writer = {.xid = mine.f0}, .op = &op,
		.lock = TXN_MEMTABLE_LOCK_MODE_UPDATE, .shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND},
		.has_lsn = true, .lsn = 0x0a00 + number};
		txn_runs_tests_row_captures_t c = {.tuple = {(UINT64CONST(1) << 16) | number}, .change = &change};
		txn_TupleRefSliceU8Usize_t rows[1];
		uint8		key[8];

		op.u.put.tuple = c.tuple;
		std_u64_to_be_bytes(number, key);
		rows[0].f0.ptr = key;
		rows[0].f0.len = sizeof(key);
		rows[0].f1 = txn_memtable_version_encoded(&change.writer, change.op, change.shown, change.has_lsn, change.lsn);
		if (!txn_memtable_table_prepare_Unit(io, host_shared_Held_record(&held), rows, 1, NULL, 0, true,
											 txn_runs_tests_row_prepared, &c, NULL, &error))
			STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		txn_memtable_Op_drop(&op);
	}
	if (!txn_spill_spilled(io, &storage, TXN_WRITTEN_OF_ROWS, mine.f0, &spilled, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(__LINE__, 0, spilled, "crate::spill::spilled(io, &storage, Of::Rows, mine).unwrap()");
	host_shared_Held_drop(&held);
	if (!txn_status_take(io, mine, unowned, &roomed, &took, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	simc_checkpointed(UINT64CONST(0x100000a27));
	if (!txn_runs_checkpoint(io, UINT64CONST(0x100000a28), &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!repo_Repo_manifest(&repo, io, &manifest, &failed))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &failed,
								 chunks_error_Error_Debug_fmt);
	{
		const RepoManifest *named = std_arc_Deref_deref(manifest, _Alignof(RepoManifest));
		uint8		kinds[2];
		size_t		found = 0;
		bool		mine_only = true;

		for (size_t k = 0; named->has_runs && k < named->runs.files.len; k++)
		{
			const RepoRunFile *file = &STD_VEC_AT(&named->runs.files, RepoRunFile, k);

			if (!file->has_spilled)
				continue;
			mine_only = mine_only && file->spilled.xid == mine.f0;
			if (found < lengthof(kinds))
				kinds[found] = file->kind;
			found++;
		}
		STD_ASSERT(__LINE__, 0, found == 2 && mine_only && kinds[0] == TXN_RUNS_KIND_SPILLED_ROWS &&
				   kinds[1] == TXN_RUNS_KIND_SPILLED_TUPLES,
				   "spilled == [(SpilledRows, mine), (SpilledTuples, mine)]");
	}
	std_arc_drop(manifest, sizeof(RepoManifest), _Alignof(RepoManifest), txn_runs_tests_Manifest_drop_glue);
	if (!txn_state_unlinked(io, &storage, &error) || !txn_runs_loaded(io, &page, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	held = txn_runs_tests_stated(io, &repo, &reading, &storage, __LINE__, 0);
	{
		HostDeltaHold hold;

		if (!host_delta_hold(io, &hold, &error))
			STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		for (uint64 number = 1; number <= 3; number++)
		{
			StdVec		versions;
			uint8		key[8];

			std_u64_to_be_bytes(number, key);
			if (!txn_memtable_table_versions(io, &hold, host_shared_Held_record(&held), key, sizeof(key), &versions,
											 &error))
				STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
										 host_Error_Debug_fmt);
			STD_ASSERT_EQ_U64(__LINE__, 0, versions.len, 1);
			txn_memtable_version_versions_drop(&versions, 0);
		}
		host_delta_Hold_drop(&hold);
	}
	for (uint64 number = 1; number <= 3; number++)
	{
		HostRow		tid = {(UINT64CONST(1) << 16) | number};
		StdVec		key;
		uint8		wanted[8];

		std_u64_to_be_bytes(number, wanted);
		if (!txn_memtable_tuples_get(io, host_shared_Held_record(&held), tid, &some, &key, &error))
			STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		STD_ASSERT(__LINE__, 0, some && key.len == 8 && memcmp(key.ptr, wanted, 8) == 0,
				   "tuples::get(io, record, tid).unwrap() == Some(number.to_be_bytes())");
		if (some)
			std_vec_free_Checked(&key, 1, 1);
	}
	if (!txn_status_lookup(io, mine, &some, &entry, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(__LINE__, 0, some && entry.count == 3, "status::lookup(io, mine).unwrap().map(|e| e.count) == Some(3)");
	STD_ASSERT_EQ_U64(__LINE__, 0, txn_runs_tests_spill_files(io, 9748), 2);
	simc_abort(mine);
	if (!txn_spill_aborted(io, mine.f0, &storage, 1, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	for (size_t k = 0; k < lengthof(later); k++)
	{
		simc_checkpointed(later[k][0]);
		if (!txn_runs_checkpoint(io, later[k][1], &error))
			STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		STD_ASSERT_EQ_U64(__LINE__, 0, txn_runs_tests_spill_files(io, 9748), 2);
	}
	host_shared_Held_drop(&held);
	host_shared_Reader_drop(&reading);
	simc_checkpointed(UINT64CONST(0x100000d27));
	if (!txn_runs_checkpoint(io, UINT64CONST(0x100000d28), &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT_EQ_U64(__LINE__, 0, txn_runs_tests_spill_files(io, 9748), 0);
	repo_Repo_drop(&repo);
	simc_TestIo_drop(&test_io);
}

#endif							/* DOLTGRES_TEST_TXN */
