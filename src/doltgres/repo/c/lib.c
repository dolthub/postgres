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
 * The C port of repo's lib.rs: a database's repository, its packs and the
 * manifest that names the tree of its storages' bases as its last save left
 * it.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/packs.h"
#include "doltgres/chunks/packs/open.h"
#include "doltgres/chunks/record.h"
#include "doltgres/chunks/varint.h"
#include "doltgres/repo/lib.h"
#include "doltgres/std/alloc.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/num.h"
#include "doltgres/std/ptr.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"
#ifdef DOLTGRES_TEST_REPO
#include "doltgres/simc/simc.h"
#include "doltgres/std/test.h"
#include "doltgres/std/test_fs.h"
#endif
#include "doltgres/host/shared.h"
#include "doltgres/std/cmp.h"
#include "doltgres/std/sort.h"
#include "doltgres/trees/build.h"
#include "doltgres/trees/cursor.h"
#include "doltgres/trees/chunker.h"
#include "doltgres/trees/lib.h"
#include "doltgres/trees/mutate.h"
#include "doltgres/trees/node.h"
#include "doltgres/trees/published.h"

#define RUST_FILE "repo" STD_SEP "src" STD_SEP "lib.rs"

/* REPO_MANIFEST_MAGIC starts a repository's manifest file. */
static const uint8 REPO_MANIFEST_MAGIC[8] = {0xc4, 0x19, 0x6e, 0xf3, 0x58, 0x0a, 0x97, 0x2d};

/* REPO_BASES is the number of the manifest's field that names its base tree. */
#define REPO_BASES ((uint32) 2)

/* REPO_FOLDED is the number of the manifest's field that holds the highest fold point a save of the repository wrote. */
#define REPO_FOLDED ((uint32) 8)

/* REPO_REDO is the number of the manifest's field that holds the redo point of the last checkpoint completed when the manifest was written. */
#define REPO_REDO ((uint32) 9)

/* REPO_RUNS is the number of the manifest's field that names the runs of the delta its checkpoint wrote. */
#define REPO_RUNS ((uint32) 10)

/* REPO_IDENTITY is the number of the manifest's field that names the database's identity tree as its saves folded it. */
#define REPO_IDENTITY ((uint32) 11)

/* REPO_ADDRESS is the length of an entry of the manifest's address table: an offset and a locator. */
#define REPO_ADDRESS ((size_t) (4 + CHUNKS_TRAILER_LOCATOR_LEN))

/* REPO_CORRUPT is the message of a manifest that isn't one. */
#define REPO_CORRUPT "repository manifest is corrupt"

/* repo_corrupt returns the error of a manifest that isn't one. */
static bool
repo_corrupt(ChunksErrorError *error)
{
	*error = chunks_error_Error_corrupt_RefStr(REPO_CORRUPT, strlen(REPO_CORRUPT));
	return false;
}

/* repo_known reports whether a record holds no field but those numbered in a list. */
static bool
repo_known(const ChunksRecordRecord *record, const uint32 *numbers, size_t numbers_len)
{
	for (size_t k = 0, k_len = record->fields.len; k < k_len; k++)
	{
		uint32		number = STD_VEC_AT(&record->fields, ChunksRecordField, k).number;
		bool		listed = false;

		for (size_t n = 0, n_len = numbers_len; n < n_len; n++)
			listed |= numbers[n] == number;
		if (!listed)
			return false;
	}
	return true;
}

/* repo_narrow returns a field of a record of a manifest's runs as the u32 it holds, refusing one past four bytes. */
static bool
repo_narrow(const ChunksRecordRecord *item, uint32 number, uint32 *result, ChunksErrorError *error)
{
	uint64		value;

	if (!chunks_record_Record_uint(item, number, &value, error))
		return false;
	if (value > PG_UINT32_MAX)
		return repo_corrupt(error);
	*result = (uint32) value;
	return true;
}

/* repo_runs_frame_0_t is runs's state across its pad: the items it reads and the files it read, while live. */
typedef struct repo_runs_frame_0_t
{
	const ChunksRecordRecord *record;
	RepoRuns   *result;
	ChunksErrorError *error;
	bool		ok;
	StdVec		items;
	bool		items_live;
	StdVec		files;
	bool		files_live;
} repo_runs_frame_0_t;

/* repo_runs_pad_0 is runs's pad: the files read, then the items, while live. */
static void
repo_runs_pad_0(void *frame_)
{
	repo_runs_frame_0_t *frame = frame_;

	if (frame->files_live)
	{
		frame->files_live = false;
		std_vec_free_Checked(&frame->files, sizeof(RepoRunFile), _Alignof(RepoRunFile));
	}
	if (frame->items_live)
	{
		frame->items_live = false;
		for (size_t k = 0, k_len = frame->items.len; k < k_len; k++)
			chunks_record_Record_drop(&STD_VEC_AT(&frame->items, ChunksRecordRecord, k));
		std_vec_free_Checked(&frame->items, sizeof(ChunksRecordRecord), _Alignof(ChunksRecordRecord));
	}
}

/* repo_runs_file reads one file of a manifest's runs from its item, refusing what runs refuses. */
static bool
repo_runs_file(const ChunksRecordRecord *item, RepoRunFile *file, ChunksErrorError *error)
{
	static const uint32 numbers[] = {1, 2, 3, 4, 5, 6, 7};
	uint64		kind;
	bool		has_xid;
	bool		has_number;

	if (!repo_known(item, numbers, lengthof(numbers)))
		return repo_corrupt(error);
	if (!chunks_record_Record_uint(item, 1, &kind, error))
		return false;
	if (kind > PG_UINT8_MAX)
		return repo_corrupt(error);
	file->kind = (uint8) kind;
	has_xid = chunks_record_Record_field(item, 6) != NULL;
	has_number = chunks_record_Record_field(item, 7) != NULL;
	if (has_xid != has_number)
		return repo_corrupt(error);
	file->has_spilled = has_xid;
	if (has_xid && (!chunks_record_Record_uint(item, 6, &file->spilled.xid, error) ||
					!repo_narrow(item, 7, &file->spilled.number, error)))
		return false;
	return repo_narrow(item, 2, &file->tablespace, error) && repo_narrow(item, 3, &file->relation, error) &&
		chunks_record_Record_uint(item, 4, &file->len, error) && chunks_record_Record_uint(item, 5, &file->hash, error);
}

/* repo_runs_body_0 is runs's body. */
static void
repo_runs_body_0(void *frame_)
{
	static const uint32 numbers[] = {1, 2};
	repo_runs_frame_0_t *frame = frame_;
	const ChunksRecordField *listed;

	if (!repo_known(frame->record, numbers, lengthof(numbers)))
	{
		repo_corrupt(frame->error);
		return;
	}
	listed = chunks_record_Record_field(frame->record, 2);
	if (listed == NULL)
		std_vec_new(&frame->items, sizeof(ChunksRecordRecord), _Alignof(ChunksRecordRecord));
	else if (!chunks_record_Record_list(frame->record, listed, &frame->items, frame->error))
		return;
	frame->items_live = true;
	std_vec_new(&frame->files, sizeof(RepoRunFile), _Alignof(RepoRunFile));
	frame->files_live = true;
	if (!std_vec_try_reserve(&frame->files, frame->items.len, sizeof(RepoRunFile), _Alignof(RepoRunFile)))
	{
		*frame->error = chunks_error_Error_corrupt_RefStr("a manifest's runs are past memory",
														  strlen("a manifest's runs are past memory"));
		repo_runs_pad_0(frame);
		return;
	}
	for (size_t k = 0, k_len = frame->items.len; k < k_len; k++)
	{
		RepoRunFile file;

		if (!repo_runs_file(&STD_VEC_AT(&frame->items, ChunksRecordRecord, k), &file, frame->error))
		{
			repo_runs_pad_0(frame);
			return;
		}
		std_vec_push_Checked(&frame->files, &file, sizeof(file), _Alignof(RepoRunFile));
	}
	if (!chunks_record_Record_uint(frame->record, 1, &frame->result->redo, frame->error))
	{
		repo_runs_pad_0(frame);
		return;
	}
	frame->result->files = frame->files;
	frame->files_live = false;
	frame->ok = true;
	repo_runs_pad_0(frame);
}

/*
 * repo_runs reads the runs a manifest's field names, refusing a field it
 * doesn't know, a number past its field's width, and a spill's xid without
 * its number or its number without its xid.
 */
static bool
repo_runs(const ChunksRecordRecord *record, RepoRuns *result, ChunksErrorError *error)
{
	repo_runs_frame_0_t frame = {.record = record, .result = result, .error = error};

	std_landing(&frame, repo_runs_body_0, repo_runs_pad_0);
	return frame.ok;
}

/* repo_Manifest_encode_frame_0_t is encode's state across its pad: its writers, the items, and the parts and bytes it makes, while live. */
typedef struct repo_Manifest_encode_frame_0_t
{
	const RepoManifest *self;
	StdVec	   *result;
	ChunksRecordRecordWriter w;
	bool		w_live;
	ChunksRecordRecordWriter inner;
	bool		inner_live;
	StdVec		files;
	bool		files_live;
	ChunksRecordRecordWriter item;
	bool		item_live;
	StdVec		bytes;
	StdVec		addresses;
	bool		parts_live;
	StdVec		out;
	bool		out_live;
} repo_Manifest_encode_frame_0_t;

/* repo_Manifest_encode_pad_0 is encode's pad: what it holds, while live. */
static void
repo_Manifest_encode_pad_0(void *frame_)
{
	repo_Manifest_encode_frame_0_t *frame = frame_;

	if (frame->out_live)
	{
		frame->out_live = false;
		std_vec_free_Checked(&frame->out, 1, 1);
	}
	if (frame->parts_live)
	{
		frame->parts_live = false;
		std_vec_free_Checked(&frame->bytes, 1, 1);
		std_vec_free_Checked(&frame->addresses, sizeof(chunks_TupleU32Locator_t), _Alignof(chunks_TupleU32Locator_t));
	}
	if (frame->item_live)
	{
		frame->item_live = false;
		chunks_record_RecordWriter_drop(&frame->item);
	}
	if (frame->files_live)
	{
		frame->files_live = false;
		for (size_t k = 0, k_len = frame->files.len; k < k_len; k++)
			chunks_record_RecordWriter_drop(&STD_VEC_AT(&frame->files, ChunksRecordRecordWriter, k));
		std_vec_free_Checked(&frame->files, sizeof(ChunksRecordRecordWriter), _Alignof(ChunksRecordRecordWriter));
	}
	if (frame->inner_live)
	{
		frame->inner_live = false;
		chunks_record_RecordWriter_drop(&frame->inner);
	}
	if (frame->w_live)
	{
		frame->w_live = false;
		chunks_record_RecordWriter_drop(&frame->w);
	}
}

/* repo_Manifest_encode_addresses writes a name's field of the record: its root's address and locator. */
static void
repo_Manifest_encode_addresses(ChunksRecordRecordWriter *w, uint32 number, const TreesPublishedPublished *name)
{
	chunks_TupleAddressLocator_t parts;

	trees_published_Published_parts(name, &parts.f0, &parts.f1);
	chunks_record_RecordWriter_addresses(w, number, &parts, 1);
}

/* repo_Manifest_encode_body_0 is encode's body. */
static void
repo_Manifest_encode_body_0(void *frame_)
{
	repo_Manifest_encode_frame_0_t *frame = frame_;
	const RepoManifest *self = frame->self;

	frame->w = chunks_record_RecordWriter_new();
	frame->w_live = true;
	if (self->has_bases)
		repo_Manifest_encode_addresses(&frame->w, REPO_BASES, &self->bases);
	chunks_record_RecordWriter_uint(chunks_record_RecordWriter_uint(&frame->w, REPO_FOLDED, self->folded), REPO_REDO,
									self->redo);
	if (self->has_runs)
	{
		std_vec_with_capacity_Checked(&frame->files, self->runs.files.len, sizeof(ChunksRecordRecordWriter),
									  _Alignof(ChunksRecordRecordWriter));
		frame->files_live = true;
		for (size_t k = 0, k_len = self->runs.files.len; k < k_len; k++)
		{
			const RepoRunFile *file = &STD_VEC_AT(&self->runs.files, RepoRunFile, k);

			frame->item = chunks_record_RecordWriter_new();
			frame->item_live = true;
			chunks_record_RecordWriter_uint(&frame->item, 1, file->kind);
			chunks_record_RecordWriter_uint(&frame->item, 2, file->tablespace);
			chunks_record_RecordWriter_uint(&frame->item, 3, file->relation);
			chunks_record_RecordWriter_uint(&frame->item, 4, file->len);
			chunks_record_RecordWriter_uint(&frame->item, 5, file->hash);
			if (file->has_spilled)
			{
				chunks_record_RecordWriter_uint(&frame->item, 6, file->spilled.xid);
				chunks_record_RecordWriter_uint(&frame->item, 7, file->spilled.number);
			}
			std_vec_push_Checked(&frame->files, &frame->item, sizeof(frame->item), _Alignof(ChunksRecordRecordWriter));
			frame->item_live = false;
		}
		frame->inner = chunks_record_RecordWriter_new();
		frame->inner_live = true;
		frame->files_live = false;
		chunks_record_RecordWriter_list(chunks_record_RecordWriter_uint(&frame->inner, 1, self->runs.redo), 2,
										&frame->files);
		frame->inner_live = false;
		chunks_record_RecordWriter_record(&frame->w, REPO_RUNS, &frame->inner);
	}
	if (self->has_identity)
		repo_Manifest_encode_addresses(&frame->w, REPO_IDENTITY, &self->identity);
	frame->w_live = false;
	chunks_record_RecordWriter_finish_value(&frame->w, &frame->bytes, &frame->addresses);
	frame->parts_live = true;
	std_slice_to_vec_Checked(&frame->out, REPO_MANIFEST_MAGIC, sizeof(REPO_MANIFEST_MAGIC), 1, 1);
	frame->out_live = true;
	chunks_varint_put_varint(&frame->out, (uint64) frame->bytes.len);
	std_vec_extend_from_slice_Checked(&frame->out, frame->bytes.ptr, frame->bytes.len, 1, 1);
	chunks_varint_put_varint(&frame->out, (uint64) frame->addresses.len);
	for (size_t k = 0, k_len = frame->addresses.len; k < k_len; k++)
	{
		const chunks_TupleU32Locator_t *address = &STD_VEC_AT(&frame->addresses, chunks_TupleU32Locator_t, k);
		uint8		offset[4];

		std_u32_to_le_bytes(address->f0, offset);
		std_vec_extend_from_slice_Checked(&frame->out, offset, sizeof(offset), 1, 1);
		chunks_trailer_Locator_write(&address->f1, &frame->out);
	}
	*frame->result = frame->out;
	frame->out_live = false;
	repo_Manifest_encode_pad_0(frame);
}

/* repo_Manifest_encode returns the manifest file's bytes: the magic, the record, and its address table with locators. */
void
repo_Manifest_encode(const RepoManifest *self, StdVec *result)
{
	repo_Manifest_encode_frame_0_t frame = {.self = self, .result = result};

	std_landing(&frame, repo_Manifest_encode_body_0, repo_Manifest_encode_pad_0);
}

/* repo_Manifest_decode_frame_0_t is decode's state across its pad: the address table, the fields, the record, and the manifest's parts, while live. */
typedef struct repo_Manifest_decode_frame_0_t
{
	const uint8 *bytes;
	size_t		bytes_len;
	TreesPublishedRepoId repo;
	RepoManifest *result;
	ChunksErrorError *error;
	bool		ok;
	StdVec		addresses;
	bool		addresses_live;
	StdVec		fields;
	bool		fields_live;
	ChunksRecordRecord record;
	bool		record_live;
	ChunksRecordRecord nested;
	bool		nested_live;
	RepoManifest manifest;
	bool		runs_live;
} repo_Manifest_decode_frame_0_t;

/* repo_Manifest_decode_pad_0 is decode's pad: what it holds, while live. */
static void
repo_Manifest_decode_pad_0(void *frame_)
{
	repo_Manifest_decode_frame_0_t *frame = frame_;

	if (frame->runs_live)
	{
		frame->runs_live = false;
		repo_Runs_drop(&frame->manifest.runs);
	}
	if (frame->nested_live)
	{
		frame->nested_live = false;
		chunks_record_Record_drop(&frame->nested);
	}
	if (frame->record_live)
	{
		frame->record_live = false;
		chunks_record_Record_drop(&frame->record);
	}
	if (frame->fields_live)
	{
		frame->fields_live = false;
		std_vec_free_Checked(&frame->fields, sizeof(ChunksRecordField), _Alignof(ChunksRecordField));
	}
	if (frame->addresses_live)
	{
		frame->addresses_live = false;
		std_vec_free_Checked(&frame->addresses, sizeof(chunks_TupleU32Locator_t), _Alignof(chunks_TupleU32Locator_t));
	}
}

/* repo_Manifest_decode_name reads a name's field of the record, bound to the repository, when it holds one. */
static bool
repo_Manifest_decode_name(repo_Manifest_decode_frame_0_t *frame, uint32 number, bool *has,
						  TreesPublishedPublished *result)
{
	ChunksAddressAddress address;
	ChunksTrailerLocator locator;

	if (!chunks_record_Record_address(&frame->record, number, has, &address, &locator, frame->error))
		return false;
	if (*has)
		*result = trees_published_Published_decoded(frame->repo, address, locator);
	return true;
}

/* repo_Manifest_decode_body_0 is decode's body. */
static void
repo_Manifest_decode_body_0(void *frame_)
{
	static const uint32 numbers[] = {REPO_BASES, REPO_FOLDED, REPO_REDO, REPO_RUNS, REPO_IDENTITY};
	repo_Manifest_decode_frame_0_t *frame = frame_;
	RepoManifest *manifest = &frame->manifest;
	const uint8 *rest;
	size_t		rest_len;
	size_t		at = 0;
	uint64		len;
	uint64		count;
	size_t		end;
	size_t		table;
	const uint8 *body;
	const ChunksRecordField *runs;

	if (frame->bytes_len < sizeof(REPO_MANIFEST_MAGIC) ||
		memcmp(frame->bytes, REPO_MANIFEST_MAGIC, sizeof(REPO_MANIFEST_MAGIC)) != 0)
	{
		repo_corrupt(frame->error);
		return;
	}
	rest = STD_PTR_AT(frame->bytes, sizeof(REPO_MANIFEST_MAGIC), 1);
	rest_len = std_wrapping_sub_usize(frame->bytes_len, sizeof(REPO_MANIFEST_MAGIC));
	if (!chunks_varint_read_varint(rest, rest_len, &at, &len, frame->error))
		return;
	if (!std_checked_add_usize(at, (size_t) len, &end) || end > rest_len)
	{
		repo_corrupt(frame->error);
		return;
	}
	body = STD_PTR_AT(rest, at, 1);
	at = end;
	if (!chunks_varint_read_varint(rest, rest_len, &at, &count, frame->error))
		return;
	if (!std_checked_mul_usize((size_t) count, REPO_ADDRESS, &table) || std_wrapping_sub_usize(rest_len, at) != table)
	{
		repo_corrupt(frame->error);
		return;
	}
	std_vec_with_capacity_Checked(&frame->addresses, (size_t) count, sizeof(chunks_TupleU32Locator_t),
								  _Alignof(chunks_TupleU32Locator_t));
	frame->addresses_live = true;
	for (size_t k = 0, k_len = (size_t) count; k < k_len; k++)
	{
		const uint8 *entry = STD_PTR_AT(rest, std_wrapping_add_usize(at, std_wrapping_mul_usize(k, REPO_ADDRESS)), 1);
		chunks_TupleU32Locator_t address;

		address.f0 = std_u32_from_le_bytes(entry);
		address.f1 = chunks_trailer_Locator_read(STD_PTR_AT(entry, 4, 1), CHUNKS_TRAILER_LOCATOR_LEN);
		std_vec_push_Checked(&frame->addresses, &address, sizeof(address), _Alignof(chunks_TupleU32Locator_t));
	}
	if (!chunks_record_fields(body, (size_t) len, &frame->fields, frame->error))
	{
		repo_Manifest_decode_pad_0(frame);
		return;
	}
	frame->fields_live = true;
	for (size_t k = 0, k_len = frame->fields.len; k < k_len; k++)
	{
		uint32		number = STD_VEC_AT(&frame->fields, ChunksRecordField, k).number;
		bool		listed = false;

		for (size_t n = 0, n_len = lengthof(numbers); n < n_len; n++)
			listed |= numbers[n] == number;
		if (!listed)
		{
			repo_corrupt(frame->error);
			repo_Manifest_decode_pad_0(frame);
			return;
		}
	}
	frame->fields_live = false;
	std_vec_free_Checked(&frame->fields, sizeof(ChunksRecordField), _Alignof(ChunksRecordField));
	if (!chunks_record_Record_value(body, (size_t) len, frame->addresses.ptr, frame->addresses.len, &frame->record,
									frame->error))
	{
		repo_Manifest_decode_pad_0(frame);
		return;
	}
	frame->record_live = true;
	if (!repo_Manifest_decode_name(frame, REPO_BASES, &manifest->has_bases, &manifest->bases))
	{
		repo_Manifest_decode_pad_0(frame);
		return;
	}
	runs = chunks_record_Record_field(&frame->record, REPO_RUNS);
	manifest->has_runs = runs != NULL;
	if (runs != NULL)
	{
		if (!chunks_record_Record_nested(&frame->record, runs, &frame->nested, frame->error))
		{
			repo_Manifest_decode_pad_0(frame);
			return;
		}
		frame->nested_live = true;
		if (!repo_runs(&frame->nested, &manifest->runs, frame->error))
		{
			repo_Manifest_decode_pad_0(frame);
			return;
		}
		frame->runs_live = true;
		frame->nested_live = false;
		chunks_record_Record_drop(&frame->nested);
	}
	if (!repo_Manifest_decode_name(frame, REPO_IDENTITY, &manifest->has_identity, &manifest->identity) ||
		!chunks_record_Record_uint(&frame->record, REPO_FOLDED, &manifest->folded, frame->error) ||
		!chunks_record_Record_uint(&frame->record, REPO_REDO, &manifest->redo, frame->error))
	{
		repo_Manifest_decode_pad_0(frame);
		return;
	}
	*frame->result = *manifest;
	frame->runs_live = false;
	frame->ok = true;
	repo_Manifest_decode_pad_0(frame);
}

/*
 * repo_Manifest_decode reads a manifest file's bytes, its base tree's and
 * its identity tree's names bound to the repository, refusing a field it
 * doesn't know.
 */
bool
repo_Manifest_decode(const uint8 *bytes, size_t bytes_len, TreesPublishedRepoId repo, RepoManifest *result,
					 ChunksErrorError *error)
{
	repo_Manifest_decode_frame_0_t frame = {.bytes = bytes, .bytes_len = bytes_len, .repo = repo, .result = result,
	.error = error};

	std_landing(&frame, repo_Manifest_decode_body_0, repo_Manifest_decode_pad_0);
	return frame.ok;
}

/* repo_Manifest_drop is Manifest's drop glue: its names' registrations and its runs. */
void
repo_Manifest_drop(RepoManifest *self)
{
	if (self->has_bases)
		trees_published_Published_drop(&self->bases);
	if (self->has_runs)
		repo_Runs_drop(&self->runs);
	if (self->has_identity)
		trees_published_Published_drop(&self->identity);
}

/* repo_Spilled_Debug_fmt is Spilled's derived Debug. */
bool
repo_Spilled_Debug_fmt(const void *value, StdFormatter *f)
{
	const RepoSpilled *self = value;
	StdDebugStruct s = std_fmt_debug_struct(f, "Spilled", strlen("Spilled"));

	std_DebugStruct_field(&s, "xid", strlen("xid"), &self->xid, std_u64_Debug_fmt);
	std_DebugStruct_field(&s, "number", strlen("number"), &self->number, std_u32_Debug_fmt);
	return std_DebugStruct_finish(&s);
}

/* repo_Spilled_PartialEq_eq is Spilled's derived PartialEq. */
bool
repo_Spilled_PartialEq_eq(const RepoSpilled *self, const RepoSpilled *other)
{
	return self->xid == other->xid && self->number == other->number;
}

/* repo_RunFile_Debug_fmt is RunFile's derived Debug. */
bool
repo_RunFile_Debug_fmt(const void *value, StdFormatter *f)
{
	const RepoRunFile *self = value;
	StdOptionRef spilled = {self->has_spilled, {&self->spilled, repo_Spilled_Debug_fmt}};
	StdDebugStruct s = std_fmt_debug_struct(f, "RunFile", strlen("RunFile"));

	std_DebugStruct_field(&s, "kind", strlen("kind"), &self->kind, std_u8_Debug_fmt);
	std_DebugStruct_field(&s, "tablespace", strlen("tablespace"), &self->tablespace, std_u32_Debug_fmt);
	std_DebugStruct_field(&s, "relation", strlen("relation"), &self->relation, std_u32_Debug_fmt);
	std_DebugStruct_field(&s, "len", strlen("len"), &self->len, std_u64_Debug_fmt);
	std_DebugStruct_field(&s, "hash", strlen("hash"), &self->hash, std_u64_Debug_fmt);
	std_DebugStruct_field(&s, "spilled", strlen("spilled"), &spilled, std_option_Debug_fmt);
	return std_DebugStruct_finish(&s);
}

/* repo_RunFile_PartialEq_eq is RunFile's derived PartialEq. */
bool
repo_RunFile_PartialEq_eq(const RepoRunFile *self, const RepoRunFile *other)
{
	return self->kind == other->kind && self->tablespace == other->tablespace && self->relation == other->relation &&
		self->len == other->len && self->hash == other->hash && self->has_spilled == other->has_spilled &&
		(!self->has_spilled || repo_Spilled_PartialEq_eq(&self->spilled, &other->spilled));
}

/* repo_Runs_files_Debug_fmt is the Debug of a Vec<RunFile>, a list's. */
static bool
repo_Runs_files_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *files = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t k = 0, k_len = files->len; k < k_len; k++)
		std_DebugList_entry(&list, &STD_VEC_AT(files, RepoRunFile, k), repo_RunFile_Debug_fmt);
	return std_DebugList_finish(&list);
}

/* repo_Runs_Debug_fmt is Runs's derived Debug. */
bool
repo_Runs_Debug_fmt(const void *value, StdFormatter *f)
{
	const RepoRuns *self = value;
	StdDebugStruct s = std_fmt_debug_struct(f, "Runs", strlen("Runs"));

	std_DebugStruct_field(&s, "redo", strlen("redo"), &self->redo, std_u64_Debug_fmt);
	std_DebugStruct_field(&s, "files", strlen("files"), &self->files, repo_Runs_files_Debug_fmt);
	return std_DebugStruct_finish(&s);
}

/* repo_Runs_PartialEq_eq is Runs's derived PartialEq. */
bool
repo_Runs_PartialEq_eq(const RepoRuns *self, const RepoRuns *other)
{
	if (self->redo != other->redo || self->files.len != other->files.len)
		return false;
	for (size_t k = 0, k_len = self->files.len; k < k_len; k++)
		if (!repo_RunFile_PartialEq_eq(&STD_VEC_AT(&self->files, RepoRunFile, k),
									   &STD_VEC_AT(&other->files, RepoRunFile, k)))
			return false;
	return true;
}

/* repo_Runs_Clone_clone is Runs's derived Clone: its files copied, since each caller keeps its own. */
void
repo_Runs_Clone_clone(const RepoRuns *self, RepoRuns *result)
{
	result->redo = self->redo;
	std_slice_to_vec_Checked(&result->files, self->files.ptr, self->files.len, sizeof(RepoRunFile),
							 _Alignof(RepoRunFile));
}

/* repo_Runs_drop is Runs's drop glue: its files. */
void
repo_Runs_drop(RepoRuns *self)
{
	std_vec_free_Checked(&self->files, sizeof(RepoRunFile), _Alignof(RepoRunFile));
}

/* repo_Manifest_drop_value is the drop of the Manifest an Arc holds. */
static void
repo_Manifest_drop_value(void *manifest)
{
	repo_Manifest_drop(manifest);
}

/* repo_ArcManifest_drop is Arc<Manifest>'s drop: its share, and the manifest with the last. */
void
repo_ArcManifest_drop(StdArcInner *self)
{
	std_arc_drop(self, sizeof(RepoManifest), _Alignof(RepoManifest), repo_Manifest_drop_value);
}

/* repo_Copy_drop is the drop of a Repo's copy: the manifest's share it holds, when it holds one. */
static void
repo_Copy_drop(repo_OptionTupleU64ArcManifest_t *self)
{
	if (self->has)
	{
		self->has = false;
		repo_ArcManifest_drop(self->f1);
	}
}

/* taken at RepoCopy and IoMutex<RepoCopy>, a Repo's copy of its manifest. */
#define HOST_LOCK_SCOPE extern
#define HOST_LOCK_DEFINE
#define HOST_LOCK_TAKEN_TS RepoCopy
#define HOST_LOCK_TAKEN_T repo_OptionTupleU64ArcManifest_t
#define HOST_LOCK_TAKEN_DEFAULT(place) ((place)->has = false)
#define HOST_LOCK_TAKEN_DROP(place) repo_Copy_drop(place)
#define HOST_LOCK_TAKEN_TYPE REPO_COPY_LOCK_TYPE
#define HOST_LOCK_IO_MUTEX
#define HOST_LOCK_TS RepoCopy
#define HOST_LOCK_T repo_OptionTupleU64ArcManifest_t
#define HOST_LOCK_DEFAULT(place) ((place)->has = false)
#include "doltgres/host/lock_template.h"

/* REPO_REPO_COPY_AT is the site of over's IoMutex::default(), the copy's. */
static const StdLocation REPO_REPO_COPY_AT = {RUST_FILE, 271, 84};

/* repo_Repo_over returns a handle over a repository's opened packs, which it takes, its manifest not yet read. */
static void
repo_Repo_over(ChunksPacksBackendPacks *packs, uint32 repository, uint32 own, RepoRepo *result)
{
	result->packs = *packs;
	result->dir = host_tablespace_dir(repository, own);
	result->own = own;
	host_lock_IoMutexRepoCopy_Default_default(&result->copy, &REPO_REPO_COPY_AT);
}

/* repo_Repo_drop is Repo's drop glue: its packs, its directory, and its copy of the manifest. */
void
repo_Repo_drop(RepoRepo *self)
{
	chunks_packs_backend_Packs_drop(&self->packs);
	std_vec_free_Checked(&self->dir.bytes, 1, 1);
	repo_Copy_drop(&self->copy.value);
}

/* repo_Repo_create_frame_0_t is create's state across its pad: the packs, the repository, and the manifest's bytes, while live. */
typedef struct repo_Repo_create_frame_0_t
{
	HostIoIo   *io;
	uint32		repository;
	uint32		own;
	RepoRepo   *result;
	ChunksErrorError *error;
	bool		ok;
	ChunksPacksBackendPacks packs;
	bool		packs_live;
	bool		repo_live;
	StdVec		bytes;
	bool		bytes_live;
} repo_Repo_create_frame_0_t;

/* repo_Repo_create_pad_0 is create's pad: the manifest's bytes, the repository, and the packs, while live. */
static void
repo_Repo_create_pad_0(void *frame_)
{
	repo_Repo_create_frame_0_t *frame = frame_;

	if (frame->bytes_live)
	{
		frame->bytes_live = false;
		std_vec_free_Checked(&frame->bytes, 1, 1);
	}
	if (frame->repo_live)
	{
		frame->repo_live = false;
		repo_Repo_drop(frame->result);
	}
	if (frame->packs_live)
	{
		frame->packs_live = false;
		chunks_packs_backend_Packs_drop(&frame->packs);
	}
}

/* repo_Repo_create_body_0 is create's body. */
static void
repo_Repo_create_body_0(void *frame_)
{
	repo_Repo_create_frame_0_t *frame = frame_;
	RepoManifest empty = {0};
	HostError	host_error;

	if (!chunks_packs_open_Packs_create(frame->io, frame->repository, frame->own, &frame->packs, frame->error))
		return;
	frame->packs_live = true;
	repo_Repo_over(&frame->packs, frame->repository, frame->own, frame->result);
	frame->packs_live = false;
	frame->repo_live = true;
	if (!host_last_redo(HOST_IO_IO_IO_DOORS(frame->io), &empty.redo, &host_error))
	{
		*frame->error = chunks_error_Error_FromError_from(&host_error);
		repo_Repo_create_pad_0(frame);
		return;
	}
	repo_Manifest_encode(&empty, &frame->bytes);
	frame->bytes_live = true;
	if (!chunks_packs_backend_Packs_write_kept(&frame->result->packs, frame->io, CHUNKS_PACKS_BACKEND_KEPT_MANIFEST,
											   frame->bytes.ptr, frame->bytes.len, frame->error))
	{
		repo_Repo_create_pad_0(frame);
		return;
	}
	frame->repo_live = false;
	frame->ok = true;
	repo_Repo_create_pad_0(frame);
}

/*
 * repo_Repo_create makes the repository of a database, by its id and the
 * tablespace of its own directory: its packs' list files, and last an empty
 * manifest, so that a repository with a manifest has all three.
 */
bool
repo_Repo_create(HostIoIo *io, uint32 repository, uint32 own, RepoRepo *result, ChunksErrorError *error)
{
	repo_Repo_create_frame_0_t frame = {.io = io, .repository = repository, .own = own, .result = result,
	.error = error};

	std_landing(&frame, repo_Repo_create_body_0, repo_Repo_create_pad_0);
	return frame.ok;
}

/* repo_Repo_open_frame_0_t is open's state across its pad: the packs and the repository, while live. */
typedef struct repo_Repo_open_frame_0_t
{
	HostIoIo   *io;
	uint32		repository;
	uint32		own;
	RepoRepo   *result;
	ChunksErrorError *error;
	bool		ok;
	ChunksPacksBackendPacks packs;
	bool		packs_live;
	bool		repo_live;
} repo_Repo_open_frame_0_t;

/* repo_Repo_open_pad_0 is open's pad: the repository and the packs, while live. */
static void
repo_Repo_open_pad_0(void *frame_)
{
	repo_Repo_open_frame_0_t *frame = frame_;

	if (frame->repo_live)
	{
		frame->repo_live = false;
		repo_Repo_drop(frame->result);
	}
	if (frame->packs_live)
	{
		frame->packs_live = false;
		chunks_packs_backend_Packs_drop(&frame->packs);
	}
}

/* repo_Repo_open_body_0 is open's body. */
static void
repo_Repo_open_body_0(void *frame_)
{
	repo_Repo_open_frame_0_t *frame = frame_;
	StdArcInner *manifest;

	if (!chunks_packs_open_Packs_open(frame->io, frame->repository, frame->own, &frame->packs, frame->error))
		return;
	frame->packs_live = true;
	repo_Repo_over(&frame->packs, frame->repository, frame->own, frame->result);
	frame->packs_live = false;
	frame->repo_live = true;
	if (!repo_Repo_manifest(frame->result, frame->io, &manifest, frame->error))
	{
		repo_Repo_open_pad_0(frame);
		return;
	}
	repo_ArcManifest_drop(manifest);
	frame->repo_live = false;
	frame->ok = true;
}

/* repo_Repo_open opens the repository of a database, by its id and the tablespace of its own directory, reading its manifest. */
bool
repo_Repo_open(HostIoIo *io, uint32 repository, uint32 own, RepoRepo *result, ChunksErrorError *error)
{
	repo_Repo_open_frame_0_t frame = {.io = io, .repository = repository, .own = own, .result = result,
	.error = error};

	std_landing(&frame, repo_Repo_open_body_0, repo_Repo_open_pad_0);
	return frame.ok;
}

/* repo_Repo_close closes the process's hold of the repository, which it takes, appending the adds its packs defer. */
bool
repo_Repo_close(RepoRepo *self, HostIoIo *io, const HostSharedReader *reading, ChunksErrorError *error)
{
	bool		closed = chunks_packs_backend_Packs_close(&self->packs, io, reading, error);

	std_vec_free_Checked(&self->dir.bytes, 1, 1);
	repo_Copy_drop(&self->copy.value);
	return closed;
}

/* repo_Repo_id returns the repository's id, which the names it decodes are bound to. */
TreesPublishedRepoId
repo_Repo_id(const RepoRepo *self)
{
	return trees_published_RepoId_of(&self->packs);
}

/* repo_Repo_own returns the tablespace of the repository's own directory, which an open of it names. */
uint32
repo_Repo_own(const RepoRepo *self)
{
	return self->own;
}

/* repo_Repo_dir returns the repository's own directory. */
const char *
repo_Repo_dir(const RepoRepo *self, size_t *result_len)
{
	*result_len = self->dir.bytes.len;
	return self->dir.bytes.ptr;
}

/* repo_Repo_reader returns a reader of the repository's chunks through a node cache, under a reader's guard. */
TreesStoreReader
repo_Repo_reader(const RepoRepo *self, HostCachePlacement placement, const HostSharedReader *reading)
{
	TreesStoreReader reader = {&self->packs, placement, reading};

	return reader;
}

/*
 * repo_Repo_read reads the newest manifest from its file.  A manifest that
 * is missing or readable only as its previous generation is the startup
 * process's to settle, so it is refused.
 */
static bool
repo_Repo_read(const RepoRepo *self, HostIoIo *io, RepoManifest *result, ChunksErrorError *error)
{
	bool		has;
	StdVec		bytes;
	bool		has_fallback;
	ChunksFileFallback fallback;
	bool		decoded;

	if (!chunks_packs_backend_Packs_read_kept(&self->packs, io, CHUNKS_PACKS_BACKEND_KEPT_MANIFEST, &has, &bytes,
											  &has_fallback, &fallback, error))
		return false;
	if (!has || has_fallback)
	{
		StdPathBuf	path;
		StdStr		shown;
		StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(" is not whole, which the startup process settles")};
		StdFmtArg	args[] = {{&shown, std_path_Display_fmt}};
		StdFmtArguments arguments = {pieces, 2, args};
		StdString	message;

		if (has)
			std_vec_free_Checked(&bytes, 1, 1);
		std_path_join(self->dir.bytes.ptr, self->dir.bytes.len, CHUNKS_PACKS_MANIFEST, strlen(CHUNKS_PACKS_MANIFEST),
					  &path);
		shown.ptr = path.bytes.ptr;
		shown.len = path.bytes.len;
		std_fmt_format_Checked(&message, &arguments);
		std_vec_free_Checked(&path.bytes, 1, 1);
		*error = chunks_error_Error_corrupt(&message);
		return false;
	}
	decoded = repo_Manifest_decode(bytes.ptr, bytes.len, repo_Repo_id(self), result, error);
	std_vec_free_Checked(&bytes, 1, 1);
	return decoded;
}

/* repo_Repo_current_frame_0_t is current's state across its pad: the guard of the copy and the manifest read, while live. */
typedef struct repo_Repo_current_frame_0_t
{
	const RepoRepo *self;
	HostIoIo   *io;
	uint64	   *result_0;
	StdArcInner **result_1;
	ChunksErrorError *error;
	bool		ok;
	HostLockLockedRwLockWriteGuard guard;
	bool		guard_live;
	RepoManifest manifest;
	bool		manifest_live;
	StdArcInner *arc;
	bool		arc_live;
} repo_Repo_current_frame_0_t;

/* repo_Repo_current_pad_0 is current's pad: the guard, the manifest read, and its Arc, while live. */
static void
repo_Repo_current_pad_0(void *frame_)
{
	repo_Repo_current_frame_0_t *frame = frame_;

	if (frame->guard_live)
	{
		frame->guard_live = false;
		host_lock_LockedRwLockWriteGuard_drop(&frame->guard);
	}
	if (frame->manifest_live)
	{
		frame->manifest_live = false;
		repo_Manifest_drop(&frame->manifest);
	}
	if (frame->arc_live)
	{
		frame->arc_live = false;
		repo_ArcManifest_drop(frame->arc);
	}
}

/* repo_Repo_current_copy takes the guard of the repository's copy of its manifest and returns the copy. */
static repo_OptionTupleU64ArcManifest_t *
repo_Repo_current_copy(repo_Repo_current_frame_0_t *frame)
{
	frame->guard = host_lock_IoMutex_lock((HostLockIoMutex *) &frame->self->copy.header, &REPO_COPY_LOCK_TYPE,
										  &frame->io->held);
	frame->guard_live = true;
	return host_lock_LockedRwLockWriteGuard_DerefMut_deref_mut(&frame->guard);
}

/* repo_Repo_current_body_0 is current's body. */
static void
repo_Repo_current_body_0(void *frame_)
{
	repo_Repo_current_frame_0_t *frame = frame_;
	uint64		rotations = chunks_packs_backend_Packs_rotations(&frame->self->packs);
	repo_OptionTupleU64ArcManifest_t *copy = repo_Repo_current_copy(frame);

	if (copy->has && copy->f0 == rotations)
	{
		*frame->result_0 = rotations;
		*frame->result_1 = std_arc_clone(copy->f1);
		frame->ok = true;
		repo_Repo_current_pad_0(frame);
		return;
	}
	frame->guard_live = false;
	host_lock_LockedRwLockWriteGuard_drop(&frame->guard);
	if (!repo_Repo_read(frame->self, frame->io, &frame->manifest, frame->error))
		return;
	frame->manifest_live = true;
	frame->arc = std_arc_new(&frame->manifest, sizeof(RepoManifest), _Alignof(RepoManifest));
	frame->manifest_live = false;
	frame->arc_live = true;
	copy = repo_Repo_current_copy(frame);
	repo_Copy_drop(copy);
	copy->f0 = rotations;
	copy->f1 = std_arc_clone(frame->arc);
	copy->has = true;
	*frame->result_0 = rotations;
	*frame->result_1 = frame->arc;
	frame->arc_live = false;
	frame->ok = true;
	repo_Repo_current_pad_0(frame);
}

/*
 * repo_Repo_current returns the newest manifest with the count of rotations
 * it was read at, read again when a file of the repository was written whole
 * since the process last read it.  It clones the copy's Arc, since the copy
 * and its reader share the manifest, and reads a stale one with no lock
 * held.
 */
static bool
repo_Repo_current(const RepoRepo *self, HostIoIo *io, uint64 *result_0, StdArcInner **result_1,
				  ChunksErrorError *error)
{
	repo_Repo_current_frame_0_t frame = {.self = self, .io = io, .result_0 = result_0, .result_1 = result_1,
	.error = error};

	std_landing(&frame, repo_Repo_current_body_0, repo_Repo_current_pad_0);
	return frame.ok;
}

/*
 * repo_Repo_manifest returns the repository's newest manifest, an
 * Arc<Manifest>, read again when a file of the repository was written whole
 * since the process last read it.
 */
bool
repo_Repo_manifest(const RepoRepo *self, HostIoIo *io, StdArcInner **result, ChunksErrorError *error)
{
	uint64		rotations;

	return repo_Repo_current(self, io, &rotations, result, error);
}

/* repo_Repo_newest returns the manifest an Arc<Manifest> holds. */
const RepoManifest *
repo_Repo_newest(StdArcInner *manifest)
{
	return std_arc_Deref_deref(manifest, _Alignof(RepoManifest));
}

/*
 * repo_Repo_previous returns the previous generation of the repository's
 * manifest, when it has one that reads, whose base tree stays a root of
 * collections while it can be fallen back to.
 */
bool
repo_Repo_previous(const RepoRepo *self, HostIoIo *io, bool *has_result, RepoManifest *result,
				   ChunksErrorError *error)
{
	StdVec		bytes;
	bool		decoded;

	if (!chunks_packs_backend_Packs_read_previous(&self->packs, io, CHUNKS_PACKS_BACKEND_KEPT_MANIFEST, has_result,
												  &bytes, error))
		return false;
	if (!*has_result)
		return true;
	decoded = repo_Manifest_decode(bytes.ptr, bytes.len, repo_Repo_id(self), result, error);
	std_vec_free_Checked(&bytes, 1, 1);
	return decoded;
}

/* repo_roots_of appends the base and identity trees a manifest names to a collection's roots. */
static void
repo_roots_of(const RepoManifest *manifest, StdVec *roots)
{
	TreesStoreTree tree;

	if (manifest->has_bases)
	{
		tree = trees_published_Published_base(&manifest->bases);
		std_vec_push_Checked(roots, &tree, sizeof(tree), _Alignof(TreesStoreTree));
	}
	if (manifest->has_identity)
	{
		tree = trees_published_Published_base(&manifest->identity);
		std_vec_push_Checked(roots, &tree, sizeof(tree), _Alignof(TreesStoreTree));
	}
}

/* repo_Repo_roots_frame_0_t is roots's state across its pad: the roots, the newest manifest, and the previous one, while live. */
typedef struct repo_Repo_roots_frame_0_t
{
	const RepoRepo *self;
	HostIoIo   *io;
	StdVec	   *result;
	ChunksErrorError *error;
	bool		ok;
	StdVec		roots;
	bool		roots_live;
	StdArcInner *newest;
	bool		newest_live;
	RepoManifest previous;
	bool		previous_live;
} repo_Repo_roots_frame_0_t;

/* repo_Repo_roots_pad_0 is roots's pad: what it holds, while live. */
static void
repo_Repo_roots_pad_0(void *frame_)
{
	repo_Repo_roots_frame_0_t *frame = frame_;

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
	if (frame->roots_live)
	{
		frame->roots_live = false;
		std_vec_free_Checked(&frame->roots, sizeof(TreesStoreTree), _Alignof(TreesStoreTree));
	}
}

/* repo_Repo_roots_body_0 is roots's body. */
static void
repo_Repo_roots_body_0(void *frame_)
{
	repo_Repo_roots_frame_0_t *frame = frame_;
	bool		has;

	std_vec_with_capacity_Checked(&frame->roots, 4, sizeof(TreesStoreTree), _Alignof(TreesStoreTree));
	frame->roots_live = true;
	if (!repo_Repo_manifest(frame->self, frame->io, &frame->newest, frame->error))
	{
		repo_Repo_roots_pad_0(frame);
		return;
	}
	frame->newest_live = true;
	repo_roots_of(repo_Repo_newest(frame->newest), &frame->roots);
	if (!repo_Repo_previous(frame->self, frame->io, &has, &frame->previous, frame->error))
	{
		repo_Repo_roots_pad_0(frame);
		return;
	}
	frame->previous_live = has;
	if (has)
		repo_roots_of(&frame->previous, &frame->roots);
	*frame->result = frame->roots;
	frame->roots_live = false;
	frame->ok = true;
	repo_Repo_roots_pad_0(frame);
}

/*
 * repo_Repo_roots returns the trees a collection marks from in the
 * repository's manifests: the base and identity trees of the newest
 * manifest and of its previous generation, which a fallback reads.
 */
bool
repo_Repo_roots(const RepoRepo *self, HostIoIo *io, StdVec *result, ChunksErrorError *error)
{
	repo_Repo_roots_frame_0_t frame = {.self = self, .io = io, .result = result, .error = error};

	std_landing(&frame, repo_Repo_roots_body_0, repo_Repo_roots_pad_0);
	return frame.ok;
}

/* The pin scope returning Result<Option<BaseRecord>>, base's. */
#define TREES_PIN_SCOPE static
#define TREES_PIN_DECLARE
#define TREES_PIN_DEFINE
#define TREES_PIN_RS ResultOptionBaseRecord
#define TREES_PIN_RETURNS bool
#define TREES_PIN_RESULT_PARAMS , bool *has_result, RepoBaseRecord *result, ChunksErrorError *error
#define TREES_PIN_RESULT_FIELDS bool *has_result; RepoBaseRecord *result; ChunksErrorError *error;
#define TREES_PIN_RESULT_INIT .has_result = has_result, .result = result, .error = error
#define TREES_PIN_RESULT_ARGS , frame->has_result, frame->result, frame->error
#include "doltgres/trees/pin_template.h"

/* The pin scope returning Result<Option<Vec<u8>>>, held's. */
#define TREES_PIN_SCOPE static
#define TREES_PIN_DECLARE
#define TREES_PIN_DEFINE
#define TREES_PIN_RS ResultOptionVecU8
#define TREES_PIN_RETURNS bool
#define TREES_PIN_RESULT_PARAMS , bool *has_result, StdVec *result, ChunksErrorError *error
#define TREES_PIN_RESULT_FIELDS bool *has_result; StdVec *result; ChunksErrorError *error;
#define TREES_PIN_RESULT_INIT .has_result = has_result, .result = result, .error = error
#define TREES_PIN_RESULT_ARGS , frame->has_result, frame->result, frame->error
#include "doltgres/trees/pin_template.h"

/*
 * repo_Storage_key writes the storage's key in the base tree, its tablespace
 * and relation number big-endian, so that the tree orders its entries by
 * their bytes.
 */
static void
repo_Storage_key(const RepoStorage *self, uint8 *result)
{
	std_u32_to_be_bytes(self->tablespace, result);
	std_u32_to_be_bytes(self->relation, STD_PTR_AT(result, 4, 1));
}

/* repo_BaseRecord_drop is BaseRecord's drop glue: its bytes and its addresses. */
void
repo_BaseRecord_drop(RepoBaseRecord *self)
{
	std_vec_free_Checked(&self->bytes, 1, 1);
	std_vec_free_Checked(&self->addresses, sizeof(chunks_TupleU32Locator_t), _Alignof(chunks_TupleU32Locator_t));
}

/* repo_seek_t is what a seek of one key in a tree takes: the tree and the key. */
typedef struct repo_seek_t
{
	TreesStoreTree tree;
	const uint8 *key;
	size_t		key_len;
} repo_seek_t;

/* repo_seek_frame_0_t is a seek's state across its pad: its cursor, while live, and the bytes it copies. */
typedef struct repo_seek_frame_0_t
{
	const repo_seek_t *seek;
	HostIoIo   *io;
	TreesPinPin *pin;
	bool	   *has_result;
	StdVec	   *bytes;
	StdVec	   *addresses;
	ChunksErrorError *error;
	bool		ok;
	TreesCursorCursor cursor;
	bool		cursor_live;
	bool		bytes_live;
} repo_seek_frame_0_t;

/* repo_seek_pad_0 is a seek's pad: the bytes it copied, then the cursor, while live. */
static void
repo_seek_pad_0(void *frame_)
{
	repo_seek_frame_0_t *frame = frame_;

	if (frame->bytes_live)
	{
		frame->bytes_live = false;
		std_vec_free_Checked(frame->bytes, 1, 1);
	}
	if (frame->cursor_live)
	{
		frame->cursor_live = false;
		trees_cursor_Cursor_drop(&frame->cursor);
	}
}

/*
 * repo_seek_body_0 is a seek's body: the value of the key, when the tree
 * holds it, and the value's addresses when they are wanted.
 */
static void
repo_seek_body_0(void *frame_)
{
	repo_seek_frame_0_t *frame = frame_;
	TreesKeyOrder order = {.tag = TREES_KEY_ORDER_BYTES};
	TreesStoreTree tree = frame->seek->tree;

	if (!trees_cursor_Cursor_seek(frame->io, frame->pin, TREES_STORE_TREE_NAMED(&tree), frame->seek->key,
								  frame->seek->key_len, &order, &frame->cursor, frame->error))
		return;
	frame->cursor_live = true;
	*frame->has_result = false;
	if (trees_cursor_Cursor_is_valid(&frame->cursor))
	{
		StdBytes	key = trees_cursor_Cursor_key(&frame->cursor);

		if (key.len == frame->seek->key_len && memcmp(key.ptr, frame->seek->key, key.len) == 0)
		{
			StdBytes	value = trees_cursor_Cursor_value(&frame->cursor);

			std_slice_to_vec_Checked(frame->bytes, value.ptr, value.len, 1, 1);
			frame->bytes_live = true;
			if (frame->addresses != NULL)
			{
				TreesNodeNode node = trees_cursor_Cursor_node(&frame->cursor);

				trees_node_Node_value_addresses(&node, trees_cursor_Cursor_index(&frame->cursor),
												frame->addresses);
			}
			frame->bytes_live = false;
			*frame->has_result = true;
		}
	}
	frame->ok = true;
	repo_seek_pad_0(frame);
}

/* repo_seek finds a key in a tree under a pin, giving its value and, when wanted, the value's addresses. */
static bool
repo_seek(const repo_seek_t *seek, HostIoIo *io, TreesPinPin *pin, bool *has_result, StdVec *bytes,
		  StdVec *addresses, ChunksErrorError *error)
{
	repo_seek_frame_0_t frame = {.seek = seek, .io = io, .pin = pin, .has_result = has_result, .bytes = bytes,
	.addresses = addresses, .error = error};

	std_landing(&frame, repo_seek_body_0, repo_seek_pad_0);
	return frame.ok;
}

/* repo_held_closure_0 is held's |io, pin|: the key's value, when the tree holds it. */
static bool
repo_held_closure_0(void *captures, HostIoIo *io, TreesPinPin *pin, bool *has_result, StdVec *result,
					ChunksErrorError *error)
{
	return repo_seek(captures, io, pin, has_result, result, NULL, error);
}

/* repo_held returns the value a key holds in a tree, or none for a key it doesn't hold. */
static bool
repo_held(HostIoIo *io, const TreesStoreReader *reader, TreesStoreTree tree, const uint8 *key, size_t key_len,
		  bool *has_result, StdVec *result, ChunksErrorError *error)
{
	repo_seek_t seek = {tree, key, key_len};

	return trees_pin_pin_ResultOptionVecU8(io, TREES_STORE_READER_NODE_STORE((TreesStoreReader *) reader),
										   repo_held_closure_0, &seek, NULL, has_result, result, error);
}

/* repo_Repo_base_closure_0 is base's |io, pin|: the storage's entry, its record and the chunks the record names. */
static bool
repo_Repo_base_closure_0(void *captures, HostIoIo *io, TreesPinPin *pin, bool *has_result, RepoBaseRecord *result,
						 ChunksErrorError *error)
{
	return repo_seek(captures, io, pin, has_result, &result->bytes, &result->addresses, error);
}

/* repo_Repo_bases returns the newest manifest's tree of one kind, its base tree or its identity tree, when it names one. */
static bool
repo_Repo_bases(const RepoRepo *self, HostIoIo *io, bool identity, bool *has_result, TreesStoreTree *result,
				ChunksErrorError *error)
{
	StdArcInner *manifest;
	const RepoManifest *newest;

	if (!repo_Repo_manifest(self, io, &manifest, error))
		return false;
	newest = repo_Repo_newest(manifest);
	*has_result = identity ? newest->has_identity : newest->has_bases;
	if (*has_result)
		*result = trees_published_Published_base(identity ? &newest->identity : &newest->bases);
	repo_ArcManifest_drop(manifest);
	return true;
}

/* repo_Repo_base returns a storage's entry in the newest manifest's base tree, or none for a storage it doesn't hold. */
bool
repo_Repo_base(const RepoRepo *self, HostIoIo *io, const HostSharedReader *reading, const RepoStorage *storage,
			   bool *has_result, RepoBaseRecord *result, ChunksErrorError *error)
{
	TreesStoreTree tree;
	uint8		key[8];
	TreesStoreReader reader;
	repo_seek_t seek;

	if (!repo_Repo_bases(self, io, false, has_result, &tree, error))
		return false;
	if (!*has_result)
		return true;
	repo_Storage_key(storage, key);
	reader = repo_Repo_reader(self, HOST_CACHE_PLACEMENT_SERVER, reading);
	seek.tree = tree;
	seek.key = key;
	seek.key_len = sizeof(key);
	return trees_pin_pin_ResultOptionBaseRecord(io, TREES_STORE_READER_NODE_STORE(&reader), repo_Repo_base_closure_0,
												&seek, NULL, has_result, result, error);
}

/* repo_Repo_identity returns the value a key holds in the newest manifest's identity tree, or none for a key it doesn't hold. */
bool
repo_Repo_identity(const RepoRepo *self, HostIoIo *io, const HostSharedReader *reading, const uint8 *key,
				   size_t key_len, bool *has_result, StdVec *result, ChunksErrorError *error)
{
	TreesStoreTree tree;
	TreesStoreReader reader;

	if (!repo_Repo_bases(self, io, true, has_result, &tree, error))
		return false;
	if (!*has_result)
		return true;
	reader = repo_Repo_reader(self, HOST_CACHE_PLACEMENT_SERVER, reading);
	return repo_held(io, &reader, tree, key, key_len, has_result, result, error);
}

/* repo_Gathered_drop is Gathered's drop glue: a base's record, or a pair's key and value. */
void
repo_Gathered_drop(RepoGathered *self)
{
	switch (self->tag)
	{
		case REPO_GATHERED_BASE:
			repo_BaseRecord_drop(&self->u.base.record);
			break;
		case REPO_GATHERED_UNLINKED:
			break;
		case REPO_GATHERED_PAIR:
			std_vec_free_Checked(&self->u.pair.key, 1, 1);
			if (self->u.pair.has_value)
				std_vec_free_Checked(&self->u.pair.value, 1, 1);
			break;
	}
}

/* repo_VecGathered_drop is Vec<Gathered>'s drop glue from a place on, the gathered edits not yet taken. */
static void
repo_VecGathered_drop(StdVec *self, size_t from)
{
	for (size_t k = 0, k_len = std_wrapping_sub_usize(self->len, from); k < k_len; k++)
		repo_Gathered_drop(&STD_VEC_AT(self, RepoGathered, std_wrapping_add_usize(from, k)));
	std_vec_free_Checked(self, sizeof(RepoGathered), _Alignof(RepoGathered));
}

/* repo_VecEdit_drop is Vec<Edit>'s drop glue. */
static void
repo_VecEdit_drop(StdVec *self)
{
	for (size_t k = 0, k_len = self->len; k < k_len; k++)
		trees_mutate_Edit_drop(&STD_VEC_AT(self, TreesMutateEdit, k));
	std_vec_free_Checked(self, sizeof(TreesMutateEdit), _Alignof(TreesMutateEdit));
}

/* The stable sort of Edits by their keys, save's. */
#define STD_SORT_SCOPE static
#define STD_SORT_DECLARE
#define STD_SORT_DEFINE
#define STD_SORT_SHARED_ONLY
#define STD_SORT_WITHOUT_UNSTABLE
#define STD_SORT_TS RepoEdit
#define STD_SORT_T TreesMutateEdit
#define STD_SORT_FREEZE 0
#include "doltgres/std/sort_template.h"

#define STD_STABLE_SORT_SCOPE static
#define STD_STABLE_SORT_DECLARE
#define STD_STABLE_SORT_DEFINE
#define STD_STABLE_SORT_TS RepoEdit
#define STD_STABLE_SORT_T TreesMutateEdit
#define STD_STABLE_SORT_FREEZE 0
#define STD_STABLE_SORT_WITHOUT_SORT
#define STD_STABLE_SORT_WITH_BY
#include "doltgres/std/stable_sort_template.h"

/* repo_Edit_key_cmp is save's |a, b| a.key().cmp(b.key()). */
static StdOrdering
repo_Edit_key_cmp(void *captures, const void *a, const void *b)
{
	StdBytes	left = trees_mutate_Edit_key(a);
	StdBytes	right = trees_mutate_Edit_key(b);

	return std_bytes_Ord_cmp(left.ptr, left.len, right.ptr, right.len);
}

/* repo_Repo_save_frame_0_t is save's state across its pad: what one pass of its loop holds, each while live. */
typedef struct repo_Repo_save_frame_0_t
{
	const RepoRepo *self;
	HostIoIo   *io;
	const HostSharedReader *reading;
	bool		(*gather) (void *captures, HostIoIo *io, const RepoRead *read, StdVec *result,
						   ChunksErrorError *error);
	void	   *gather_captures;
	bool		(*saved) (void *captures, HostIoIo *io, ChunksErrorError *error);
	void	   *saved_captures;
	ChunksErrorError *error;
	bool		ok;
	bool		checkpoint;
	/* The runs given, or none, until a pass takes them. */
	bool		has_runs;
	RepoRuns	runs;
	StdArcInner *manifest;
	bool		manifest_live;
	/* The runs the pass writes: those given, or those it read. */
	bool		has_written;
	RepoRuns	written;
	StdVec		gathered;
	size_t		gathered_at;
	bool		gathered_live;
	StdVec		edits;
	bool		edits_live;
	StdVec		pairs;
	bool		pairs_live;
	StdVec		held;
	bool		held_live;
	TreesShape	shape;
	bool		shape_live;
	TreesStoreWriter loose;
	bool		loose_live;
	TreesStoreBrandedWriter writer;
	bool		writer_live;
	StdVec		published;
	bool		published_live;
	StdVec		finished;
	bool		finished_live;
	RepoManifest written_manifest;
	bool		written_manifest_live;
	StdVec		bytes;
	bool		bytes_live;

	/*
	 * The pass's read: its rotations, and the bases, folded point, and names
	 * the read manifest gives.
	 */
	RepoRead	read;
	bool		has_tree;
	TreesStoreTree tree;
	bool		has_identity;
	TreesStoreTree identity;
	HostLsn		folded;
	bool		has_read_bases;
	chunks_TupleAddressLocator_t read_bases;
	bool		has_read_identity;
	chunks_TupleAddressLocator_t read_identity;

	/*
	 * Whether the pass built new trees, and the places of the roots it wrote,
	 * when it wrote them.
	 */
	bool		built;
	bool		has_bases_root;
	size_t		bases_root;
	bool		has_identity_root;
	size_t		identity_root;
	bool		landed;
} repo_Repo_save_frame_0_t;

/* repo_Repo_save_pass_pad_0 drops what one pass holds, each while live, keeping the runs given. */
static void
repo_Repo_save_pass_pad_0(repo_Repo_save_frame_0_t *frame)
{
	if (frame->bytes_live)
	{
		frame->bytes_live = false;
		std_vec_free_Checked(&frame->bytes, 1, 1);
	}
	if (frame->written_manifest_live)
	{
		frame->written_manifest_live = false;
		repo_Manifest_drop(&frame->written_manifest);
	}
	if (frame->finished_live)
	{
		frame->finished_live = false;
		std_vec_free_Checked(&frame->finished, sizeof(ChunksPackPackId), _Alignof(ChunksPackPackId));
	}
	if (frame->published_live)
	{
		frame->published_live = false;
		for (size_t k = 0, k_len = frame->published.len; k < k_len; k++)
			trees_published_Published_drop(&STD_VEC_AT(&frame->published, TreesPublishedPublished, k));
		std_vec_free_Checked(&frame->published, sizeof(TreesPublishedPublished), _Alignof(TreesPublishedPublished));
	}
	if (frame->writer_live)
	{
		frame->writer_live = false;
		trees_store_BrandedWriter_drop(&frame->writer);
	}
	if (frame->loose_live)
	{
		frame->loose_live = false;
		trees_store_Writer_drop(&frame->loose);
	}
	if (frame->shape_live)
	{
		frame->shape_live = false;
		trees_Shape_drop(&frame->shape);
	}
	if (frame->held_live)
	{
		frame->held_live = false;
		std_vec_free_Checked(&frame->held, 1, 1);
	}
	if (frame->pairs_live)
	{
		frame->pairs_live = false;
		repo_VecEdit_drop(&frame->pairs);
	}
	if (frame->edits_live)
	{
		frame->edits_live = false;
		repo_VecEdit_drop(&frame->edits);
	}
	if (frame->gathered_live)
	{
		frame->gathered_live = false;
		repo_VecGathered_drop(&frame->gathered, frame->gathered_at);
	}
	if (frame->has_written)
	{
		frame->has_written = false;
		repo_Runs_drop(&frame->written);
	}
	if (frame->manifest_live)
	{
		frame->manifest_live = false;
		repo_ArcManifest_drop(frame->manifest);
	}
}

/* repo_Repo_save_pad_0 is save's pad: what its pass holds, then the runs given, while live. */
static void
repo_Repo_save_pad_0(void *frame_)
{
	repo_Repo_save_frame_0_t *frame = frame_;

	repo_Repo_save_pass_pad_0(frame);
	if (frame->has_runs)
	{
		frame->has_runs = false;
		repo_Runs_drop(&frame->runs);
	}
}

/* repo_Edit_put returns the Edit that puts an entry, its key and value taken. */
static void
repo_Edit_put(StdVec *key, StdVec *value, StdVec *addresses, TreesMutateEdit *result)
{
	result->tag = TREES_MUTATE_EDIT_PUT;
	trees_build_Entry_new_VecU8VecU8(key, value, &result->u.put);
	if (addresses != NULL)
	{
		std_vec_free_Checked(&result->u.put.addresses, sizeof(chunks_TupleU32Locator_t),
							 _Alignof(chunks_TupleU32Locator_t));
		result->u.put.addresses = *addresses;
	}
}

/* repo_storage_key_vec returns a storage's key in the base tree as a Vec<u8>. */
static void
repo_storage_key_vec(const RepoStorage *storage, StdVec *result)
{
	uint8		key[8];

	repo_Storage_key(storage, key);
	std_slice_to_vec_Checked(result, key, sizeof(key), 1, 1);
}

/*
 * repo_Repo_save_edits turns what the pass gathered into the base tree's
 * edits and the identity tree's, a pair's only where it changes what the
 * tree as read holds, raising the pass's fold point to each base's.
 */
static bool
repo_Repo_save_edits(repo_Repo_save_frame_0_t *frame)
{
	TreesStoreReader reader = repo_Repo_reader(frame->self, HOST_CACHE_PLACEMENT_SERVER, frame->reading);

	for (; frame->gathered_at < frame->gathered.len; frame->gathered_at = std_wrapping_add_usize(frame->gathered_at, 1))
	{
		RepoGathered *edit = &STD_VEC_AT(&frame->gathered, RepoGathered, frame->gathered_at);
		TreesMutateEdit made;
		StdVec		key;

		std_vec_reserve_Checked(&frame->edits, 1, sizeof(TreesMutateEdit), _Alignof(TreesMutateEdit));
		std_vec_reserve_Checked(&frame->pairs, 1, sizeof(TreesMutateEdit), _Alignof(TreesMutateEdit));

		switch (edit->tag)
		{
			case REPO_GATHERED_BASE:
				frame->folded = std_max_u64(frame->folded, edit->u.base.folded);
				repo_storage_key_vec(&edit->u.base.storage, &key);
				edit->tag = REPO_GATHERED_UNLINKED;
				repo_Edit_put(&key, &edit->u.base.record.bytes, &edit->u.base.record.addresses, &made);
				std_vec_push_Checked(&frame->edits, &made, sizeof(made), _Alignof(TreesMutateEdit));
				break;
			case REPO_GATHERED_UNLINKED:
				made.tag = TREES_MUTATE_EDIT_DELETE;
				repo_storage_key_vec(&edit->u.unlinked, &made.u.delete);
				std_vec_push_Checked(&frame->edits, &made, sizeof(made), _Alignof(TreesMutateEdit));
				break;
			case REPO_GATHERED_PAIR:
				{
					bool		has_held = false;

					if (frame->has_identity)
					{
						if (!repo_held(frame->io, &reader, frame->identity, edit->u.pair.key.ptr, edit->u.pair.key.len,
									   &has_held, &frame->held, frame->error))
							return false;
						frame->held_live = has_held;
					}
					if (edit->u.pair.has_value &&
						!(has_held && frame->held.len == edit->u.pair.value.len &&
						  memcmp(frame->held.ptr, edit->u.pair.value.ptr, frame->held.len) == 0))
					{
						edit->tag = REPO_GATHERED_UNLINKED;
						repo_Edit_put(&edit->u.pair.key, &edit->u.pair.value, NULL, &made);
						std_vec_push_Checked(&frame->pairs, &made, sizeof(made), _Alignof(TreesMutateEdit));
					}
					else if (!edit->u.pair.has_value && has_held)
					{
						edit->tag = REPO_GATHERED_UNLINKED;
						made.tag = TREES_MUTATE_EDIT_DELETE;
						made.u.delete = edit->u.pair.key;
						std_vec_push_Checked(&frame->pairs, &made, sizeof(made), _Alignof(TreesMutateEdit));
					}
					if (frame->held_live)
					{
						frame->held_live = false;
						std_vec_free_Checked(&frame->held, 1, 1);
					}
					break;
				}
		}
		repo_Gathered_drop(edit);
	}
	frame->gathered_live = false;
	std_vec_free_Checked(&frame->gathered, sizeof(RepoGathered), _Alignof(RepoGathered));
	std_slice_sort_by_RepoEdit(frame->edits.ptr, frame->edits.len, repo_Edit_key_cmp, NULL, NULL);
	std_slice_sort_by_RepoEdit(frame->pairs.ptr, frame->pairs.len, repo_Edit_key_cmp, NULL, NULL);
	return true;
}

/*
 * repo_Repo_save_mutated is save's mutated: no root for no edits, else the
 * tree the edits make of the tree as read, or of an empty one, written by
 * the pass's writer, which takes the edits.
 */
static bool
repo_Repo_save_mutated(repo_Repo_save_frame_0_t *frame, bool has_tree, TreesStoreTree tree, StdVec *edits,
					   bool *edits_live, bool *has_root, size_t *root)
{
	*has_root = false;
	if (edits->len == 0)
		return true;
	if (!has_tree)
	{
		size_t		empty;

		if (!trees_store_BrandedWriter_empty(&frame->writer, frame->io, &empty, frame->error))
			return false;
		{
			TreesStoreTreeRef root = trees_store_BrandedWriter_root(&frame->writer, empty);

			tree = trees_store_Tree_at(root.address, root.locator);
		}
	}
	*edits_live = false;
	if (!trees_store_BrandedWriter_mutate(&frame->writer, frame->io, TREES_STORE_TREE_NAMED(&tree),
										  TREES_CHUNKER_DEFAULT_TARGET, edits, &frame->shape, root, frame->error))
		return false;
	*has_root = true;
	return true;
}

/* repo_decoded returns a name as read, bound to the repository, when the read gave one. */
static void
repo_decoded(TreesPublishedRepoId repo, bool has, const chunks_TupleAddressLocator_t *read, bool *has_result,
			 TreesPublishedPublished *result)
{
	*has_result = has;
	if (has)
		*result = trees_published_Published_decoded(repo, read->f0, read->f1);
}

/*
 * repo_Repo_save_run_0 is save's run under the root slots: it holds and
 * publishes the roots the pass wrote, writes the manifest that names them
 * if no kept file of the repository was written since the pass's read, and
 * takes the runs back out of it.
 */
static bool
repo_Repo_save_run_inner(repo_Repo_save_frame_0_t *frame, HostIoRegistering *registering,
						 ChunksPacksRootsRootSlots *slots, void *result, ChunksErrorError *error)
{
	HostIoTakesShort io = {&HOST_IO_REGISTERING_TAKES_SHORT_OPS, registering};
	HostIoIoDoors doors = {&HOST_IO_REGISTERING_TAKES_SHORT_OPS.io_doors, registering};
	TreesPublishedRepoId repo = repo_Repo_id(frame->self);
	RepoManifest *manifest = &frame->written_manifest;
	HostError	host_error;

	if (frame->built)
	{
		ChunksPacksRootsFold slot;
		bool		held = true;

		chunks_packs_roots_RootSlots_fold(slots, &slot);
		if (frame->has_bases_root)
			held = trees_store_BrandedWriter_hold(&frame->writer, io, slot.s0, frame->bases_root, error);
		else
			chunks_packs_roots_RootSlot_unused(slot.s0);
		if (frame->has_identity_root && held)
			held = trees_store_BrandedWriter_hold(&frame->writer, io, slot.s1, frame->identity_root, error);
		else
			chunks_packs_roots_RootSlot_unused(slot.s1);
		chunks_packs_roots_RootSlot_unused(slot.s2);
		chunks_packs_roots_RootSlot_unused(slot.s3);
		if (!held)
			return false;
		frame->writer_live = false;
		if (!trees_store_BrandedWriter_publish(&frame->writer, io, &frame->published, &frame->finished, error))
			return false;
		frame->published_live = true;
		frame->finished_live = true;
		manifest->has_bases = frame->has_bases_root;
		if (frame->has_bases_root)
			manifest->bases = STD_VEC_AT(&frame->published, TreesPublishedPublished, 0);
		else
			repo_decoded(repo, frame->has_read_bases, &frame->read_bases, &manifest->has_bases, &manifest->bases);
		manifest->has_identity = frame->has_identity_root;
		if (frame->has_identity_root)
			manifest->identity = STD_VEC_AT(&frame->published, TreesPublishedPublished,
											frame->has_bases_root ? 1 : 0);
		else
			repo_decoded(repo, frame->has_read_identity, &frame->read_identity, &manifest->has_identity,
						 &manifest->identity);
		frame->published.len = 0;
	}
	else
	{
		repo_decoded(repo, frame->has_read_bases, &frame->read_bases, &manifest->has_bases, &manifest->bases);
		repo_decoded(repo, frame->has_read_identity, &frame->read_identity, &manifest->has_identity,
					 &manifest->identity);
	}
	manifest->has_runs = false;
	frame->written_manifest_live = true;
	if (!host_last_redo(doors, &manifest->redo, &host_error))
	{
		*error = chunks_error_Error_FromError_from(&host_error);
		return false;
	}
	manifest->folded = frame->folded;
	manifest->has_runs = frame->has_written;
	if (frame->has_written)
		manifest->runs = frame->written;
	frame->has_written = false;
	if (!host_pause(HOST_SHARED_REGISTERING_PAUSES(registering), "doltgres-repo-save-written", &host_error))
	{
		*error = chunks_error_Error_FromError_from(&host_error);
		return false;
	}
	repo_Manifest_encode(manifest, &frame->bytes);
	frame->bytes_live = true;
	if (!chunks_packs_backend_Packs_write_kept_if(&frame->self->packs, io, CHUNKS_PACKS_BACKEND_KEPT_MANIFEST,
												  frame->bytes.ptr, frame->bytes.len, frame->read.rotations,
												  result, error))
		return false;
	frame->has_written = manifest->has_runs;
	if (manifest->has_runs)
		frame->written = manifest->runs;
	manifest->has_runs = false;
	return true;
}

/* repo_Repo_save_run_frame_0_t is run's state across its pad: its arguments and the save's frame, whose pins it drops. */
typedef struct repo_Repo_save_run_frame_0_t
{
	repo_Repo_save_frame_0_t *save;
	HostIoRegistering *registering;
	ChunksPacksRootsRootSlots *slots;
	void	   *result;
	ChunksErrorError *error;
	bool		ok;
} repo_Repo_save_run_frame_0_t;

/*
 * repo_Repo_save_run_pad_0 drops what holds a root's Pin, the writer and its
 * held roots, the published names, and the manifest that names them, so
 * that no Pin outlives the registration, as the closure's locals don't.
 */
static void
repo_Repo_save_run_pad_0(void *frame_)
{
	repo_Repo_save_frame_0_t *frame = ((repo_Repo_save_run_frame_0_t *) frame_)->save;

	if (frame->written_manifest_live)
	{
		frame->written_manifest_live = false;
		repo_Manifest_drop(&frame->written_manifest);
	}
	if (frame->published_live)
	{
		frame->published_live = false;
		for (size_t k = 0, k_len = frame->published.len; k < k_len; k++)
			trees_published_Published_drop(&STD_VEC_AT(&frame->published, TreesPublishedPublished, k));
		std_vec_free_Checked(&frame->published, sizeof(TreesPublishedPublished), _Alignof(TreesPublishedPublished));
	}
	if (frame->writer_live)
	{
		frame->writer_live = false;
		trees_store_BrandedWriter_drop(&frame->writer);
	}
}

/* repo_Repo_save_run_body_0 is run's body, which drops the pins it made before it returns. */
static void
repo_Repo_save_run_body_0(void *frame_)
{
	repo_Repo_save_run_frame_0_t *frame = frame_;

	frame->ok = repo_Repo_save_run_inner(frame->save, frame->registering, frame->slots, frame->result, frame->error);
	repo_Repo_save_run_pad_0(frame);
}

/*
 * repo_Repo_save_run_0 is save's run under the root slots: its writes, with
 * every name holding a Pin dropped while the slots it borrows live.
 */
static bool
repo_Repo_save_run_0(void *captures, HostIoRegistering *registering, ChunksPacksRootsRootSlots *slots, void *result,
					 ChunksErrorError *error)
{
	repo_Repo_save_run_frame_0_t frame = {.save = captures, .registering = registering, .slots = slots,
	.result = result, .error = error};

	std_landing(&frame, repo_Repo_save_run_body_0, repo_Repo_save_run_pad_0);
	return frame.ok;
}

/* repo_Repo_save_body_0 is save's body: its loop of passes, each from its read of the newest manifest. */
static void
repo_Repo_save_body_0(void *frame_)
{
	repo_Repo_save_frame_0_t *frame = frame_;
	const RepoRepo *self = frame->self;

	for (;;)
	{
		const RepoManifest *read;

		if (!repo_Repo_current(self, frame->io, &frame->read.rotations, &frame->manifest, frame->error))
		{
			repo_Repo_save_pad_0(frame);
			return;
		}
		frame->manifest_live = true;
		read = repo_Repo_newest(frame->manifest);
		frame->has_tree = read->has_bases;
		if (read->has_bases)
		{
			frame->tree = trees_published_Published_base(&read->bases);
			trees_published_Published_parts(&read->bases, &frame->read_bases.f0, &frame->read_bases.f1);
		}
		frame->has_read_bases = read->has_bases;
		frame->folded = read->folded;
		frame->has_identity = read->has_identity;
		if (read->has_identity)
		{
			frame->identity = trees_published_Published_base(&read->identity);
			trees_published_Published_parts(&read->identity, &frame->read_identity.f0, &frame->read_identity.f1);
		}
		frame->has_read_identity = read->has_identity;
		if (frame->has_runs)
		{
			frame->written = frame->runs;
			frame->has_runs = false;
			frame->has_written = true;
		}
		else if (read->has_runs)
		{
			repo_Runs_Clone_clone(&read->runs, &frame->written);
			frame->has_written = true;
		}
		if (!frame->gather(frame->gather_captures, frame->io, &frame->read, &frame->gathered, frame->error))
		{
			repo_Repo_save_pad_0(frame);
			return;
		}
		frame->gathered_at = 0;
		frame->gathered_live = true;
		if (frame->gathered.len == 0 && !frame->checkpoint)
		{
			frame->ok = true;
			repo_Repo_save_pad_0(frame);
			return;
		}
		std_vec_new(&frame->edits, sizeof(TreesMutateEdit), _Alignof(TreesMutateEdit));
		frame->edits_live = true;
		std_vec_new(&frame->pairs, sizeof(TreesMutateEdit), _Alignof(TreesMutateEdit));
		frame->pairs_live = true;
		if (!repo_Repo_save_edits(frame))
		{
			repo_Repo_save_pad_0(frame);
			return;
		}
		frame->built = frame->edits.len != 0 || frame->pairs.len != 0;
		if (frame->built)
		{
			frame->shape = trees_Shape_Default_default();
			frame->shape_live = true;
			if (!trees_store_Writer_new(frame->io, repo_Repo_reader(self, HOST_CACHE_PLACEMENT_SERVER, frame->reading),
										0, &frame->loose, frame->error))
			{
				repo_Repo_save_pad_0(frame);
				return;
			}
			frame->loose_live = false;
			trees_store_Writer_brand(&frame->loose, &frame->writer);
			frame->writer_live = true;
			if (!repo_Repo_save_mutated(frame, frame->has_tree, frame->tree, &frame->edits, &frame->edits_live,
										&frame->has_bases_root, &frame->bases_root) ||
				!repo_Repo_save_mutated(frame, frame->has_identity, frame->identity, &frame->pairs,
										&frame->pairs_live, &frame->has_identity_root, &frame->identity_root))
			{
				repo_Repo_save_pad_0(frame);
				return;
			}
		}
		if (!chunks_packs_roots_RootSlots_register(frame->io, repo_Repo_save_run_0, frame, NULL, &frame->landed,
												   frame->error))
		{
			repo_Repo_save_pad_0(frame);
			return;
		}
		if (frame->landed)
		{
			repo_Repo_save_pad_0(frame);
			frame->ok = frame->saved(frame->saved_captures, frame->io, frame->error);
			return;
		}
		if (frame->checkpoint && frame->has_written)
		{
			frame->runs = frame->written;
			frame->has_runs = true;
			frame->has_written = false;
		}
		repo_Repo_save_pass_pad_0(frame);
	}
}

/*
 * repo_Repo_save writes the repository's newest bases into a new manifest:
 * it reads the newest manifest, calls gather with that read, edits the base
 * tree and the identity tree with what it gathered in a pack of its own,
 * registers the new roots for as long as it takes to write the manifest that
 * names them, which it writes only if no kept file of the repository was
 * written since the read, and then calls saved.  A refused write starts
 * over from the read; nothing gathered writes nothing, unless runs names a
 * checkpoint's runs, which are written with the bases as read.  It takes
 * the runs given.
 */
bool
repo_Repo_save(const RepoRepo *self, HostIoIo *io, const HostSharedReader *reading, bool has_runs, RepoRuns *runs,
			   bool (*gather) (void *captures, HostIoIo *io, const RepoRead *read, StdVec *result,
							   ChunksErrorError *error),
			   void *gather_captures,
			   bool (*saved) (void *captures, HostIoIo *io, ChunksErrorError *error), void *saved_captures,
			   ChunksErrorError *error)
{
	repo_Repo_save_frame_0_t frame = {.self = self, .io = io, .reading = reading, .gather = gather,
		.gather_captures = gather_captures, .saved = saved, .saved_captures = saved_captures, .error = error,
	.checkpoint = has_runs, .has_runs = has_runs};

	if (has_runs)
		frame.runs = *runs;
	std_landing(&frame, repo_Repo_save_body_0, repo_Repo_save_pad_0);
	return frame.ok;
}

/* The pin scope returning Result<Vec<Gathered>>, rekeyed's. */
#define TREES_PIN_SCOPE static
#define TREES_PIN_DECLARE
#define TREES_PIN_DEFINE
#define TREES_PIN_RS ResultVecGathered
#define TREES_PIN_RETURNS bool
#define TREES_PIN_RESULT_PARAMS , StdVec *result, ChunksErrorError *error
#define TREES_PIN_RESULT_FIELDS StdVec *result; ChunksErrorError *error;
#define TREES_PIN_RESULT_INIT .result = result, .error = error
#define TREES_PIN_RESULT_ARGS , frame->result, frame->error
#include "doltgres/trees/pin_template.h"

/* repo_rekeyed_t is what rekeyed's gather takes: the repository, its reader's guard, and the tablespaces it moves between. */
typedef struct repo_rekeyed_t
{
	const RepoRepo *self;
	const HostSharedReader *reading;
	uint32		from;
	uint32		to;
	TreesStoreTree tree;
} repo_rekeyed_t;

/* repo_rekeyed_scope_frame_0_t is rekeyed's scope's state across its pad: the gathered edits and the cursor, while live. */
typedef struct repo_rekeyed_scope_frame_0_t
{
	const repo_rekeyed_t *rekeyed;
	HostIoIo   *io;
	TreesPinPin *pin;
	StdVec	   *result;
	ChunksErrorError *error;
	bool		ok;
	StdVec		gathered;
	bool		gathered_live;
	TreesCursorCursor cursor;
	bool		cursor_live;
	RepoGathered edit;
	bool		edit_live;
} repo_rekeyed_scope_frame_0_t;

/* repo_rekeyed_scope_pad_0 is the scope's pad: the edit being made, the cursor, and the gathered edits, while live. */
static void
repo_rekeyed_scope_pad_0(void *frame_)
{
	repo_rekeyed_scope_frame_0_t *frame = frame_;

	if (frame->edit_live)
	{
		frame->edit_live = false;
		repo_Gathered_drop(&frame->edit);
	}
	if (frame->cursor_live)
	{
		frame->cursor_live = false;
		trees_cursor_Cursor_drop(&frame->cursor);
	}
	if (frame->gathered_live)
	{
		frame->gathered_live = false;
		repo_VecGathered_drop(&frame->gathered, 0);
	}
}

/*
 * repo_rekeyed_scope_body_0 is the scope's body: each entry under the old
 * tablespace put under its new key and taken out under its old.
 */
static void
repo_rekeyed_scope_body_0(void *frame_)
{
	repo_rekeyed_scope_frame_0_t *frame = frame_;
	const repo_rekeyed_t *rekeyed = frame->rekeyed;
	TreesKeyOrder order = {.tag = TREES_KEY_ORDER_BYTES};
	TreesStoreTree tree = rekeyed->tree;
	uint8		prefix[4];

	std_u32_to_be_bytes(rekeyed->from, prefix);
	std_vec_new(&frame->gathered, sizeof(RepoGathered), _Alignof(RepoGathered));
	frame->gathered_live = true;
	if (!trees_cursor_Cursor_seek(frame->io, frame->pin, TREES_STORE_TREE_NAMED(&tree), prefix, sizeof(prefix), &order,
								  &frame->cursor, frame->error))
	{
		repo_rekeyed_scope_pad_0(frame);
		return;
	}
	frame->cursor_live = true;
	while (trees_cursor_Cursor_is_valid(&frame->cursor))
	{
		StdBytes	key = trees_cursor_Cursor_key(&frame->cursor);
		StdBytes	value;
		TreesNodeNode node;
		uint32		relation;

		if (key.len < sizeof(prefix) || memcmp(key.ptr, prefix, sizeof(prefix)) != 0)
			break;
		if (key.len < 8)
		{
			*frame->error = chunks_error_Error_corrupt_RefStr("a base tree's key isn't a storage's",
															  strlen("a base tree's key isn't a storage's"));
			repo_rekeyed_scope_pad_0(frame);
			return;
		}
		relation = std_u32_from_be_bytes(STD_PTR_AT(key.ptr, 4, 1));
		value = trees_cursor_Cursor_value(&frame->cursor);
		node = trees_cursor_Cursor_node(&frame->cursor);
		frame->edit.tag = REPO_GATHERED_BASE;
		std_vec_new(&frame->edit.u.base.record.bytes, 1, 1);
		std_vec_new(&frame->edit.u.base.record.addresses, sizeof(chunks_TupleU32Locator_t),
					_Alignof(chunks_TupleU32Locator_t));
		frame->edit_live = true;
		std_slice_to_vec_Checked(&frame->edit.u.base.record.bytes, value.ptr, value.len, 1, 1);
		trees_node_Node_value_addresses(&node, trees_cursor_Cursor_index(&frame->cursor),
										&frame->edit.u.base.record.addresses);
		frame->edit.u.base.storage.tablespace = rekeyed->to;
		frame->edit.u.base.storage.relation = relation;
		frame->edit.u.base.folded = 0;
		std_vec_push_Checked(&frame->gathered, &frame->edit, sizeof(frame->edit), _Alignof(RepoGathered));
		frame->edit_live = false;
		frame->edit.tag = REPO_GATHERED_UNLINKED;
		frame->edit.u.unlinked.tablespace = rekeyed->from;
		frame->edit.u.unlinked.relation = relation;
		std_vec_push_Checked(&frame->gathered, &frame->edit, sizeof(frame->edit), _Alignof(RepoGathered));
		if (!trees_cursor_Cursor_next(&frame->cursor, frame->io, frame->pin, frame->error))
		{
			repo_rekeyed_scope_pad_0(frame);
			return;
		}
	}
	*frame->result = frame->gathered;
	frame->gathered_live = false;
	frame->ok = true;
	repo_rekeyed_scope_pad_0(frame);
}

/* repo_rekeyed_scope_0 is rekeyed's |io, pin| over the base tree. */
static bool
repo_rekeyed_scope_0(void *captures, HostIoIo *io, TreesPinPin *pin, StdVec *result, ChunksErrorError *error)
{
	repo_rekeyed_scope_frame_0_t frame = {.rekeyed = captures, .io = io, .pin = pin, .result = result,
	.error = error};

	std_landing(&frame, repo_rekeyed_scope_body_0, repo_rekeyed_scope_pad_0);
	return frame.ok;
}

/* repo_rekeyed_gather_0 is rekeyed's gather: nothing for a manifest with no base tree, else every storage under the old tablespace moved. */
static bool
repo_rekeyed_gather_0(void *captures, HostIoIo *io, const RepoRead *read, StdVec *result, ChunksErrorError *error)
{
	repo_rekeyed_t *rekeyed = captures;
	TreesStoreReader reader;
	bool		has;

	if (!repo_Repo_bases(rekeyed->self, io, false, &has, &rekeyed->tree, error))
		return false;
	if (!has)
	{
		std_vec_new(result, sizeof(RepoGathered), _Alignof(RepoGathered));
		return true;
	}
	reader = repo_Repo_reader(rekeyed->self, HOST_CACHE_PLACEMENT_SERVER, rekeyed->reading);
	return trees_pin_pin_ResultVecGathered(io, TREES_STORE_READER_NODE_STORE(&reader), repo_rekeyed_scope_0, rekeyed,
										   NULL, result, error);
}

/* repo_rekeyed_saved_0 is rekeyed's |_| Ok(()). */
static bool
repo_rekeyed_saved_0(void *captures, HostIoIo *io, ChunksErrorError *error)
{
	return true;
}

/*
 * repo_Repo_rekeyed moves every storage the newest manifest's base tree
 * holds under one tablespace to another, in one new manifest, each entry put
 * under its new key and taken out under its old; a manifest that holds none
 * under the old one writes nothing, so a finish run again rekeys nothing
 * twice.
 */
bool
repo_Repo_rekeyed(const RepoRepo *self, HostIoIo *io, const HostSharedReader *reading, uint32 from, uint32 to,
				  ChunksErrorError *error)
{
	repo_rekeyed_t rekeyed = {self, reading, from, to};

	return repo_Repo_save(self, io, reading, false, NULL, repo_rekeyed_gather_0, &rekeyed, repo_rekeyed_saved_0, NULL,
						  error);
}

#ifdef DOLTGRES_TEST_REPO

/* repo_tests_frame_0_t is a codec test's state across its pad: the bytes it encodes and the manifest it decodes, while live. */
typedef struct repo_tests_frame_0_t
{
	StdVec		bytes;
	bool		bytes_live;
	RepoManifest read;
	bool		read_live;
	ChunksErrorError error;
	bool		error_live;
} repo_tests_frame_0_t;

/* repo_tests_pad_0 is a codec test's pad: what it holds, while live. */
static void
repo_tests_pad_0(void *frame_)
{
	repo_tests_frame_0_t *frame = frame_;

	if (frame->error_live)
	{
		frame->error_live = false;
		chunks_error_Error_drop(&frame->error);
	}
	if (frame->read_live)
	{
		frame->read_live = false;
		repo_Manifest_drop(&frame->read);
	}
	if (frame->bytes_live)
	{
		frame->bytes_live = false;
		std_vec_free_Checked(&frame->bytes, 1, 1);
	}
}

/* repo_tests_round_trip encodes a manifest and decodes its bytes, unwrapping at a site of the test. */
static void
repo_tests_round_trip(repo_tests_frame_0_t *frame, const RepoManifest *written, TreesPublishedRepoId repo,
					  uint32 line, uint32 col)
{
	repo_Manifest_encode(written, &frame->bytes);
	frame->bytes_live = true;
	if (!repo_Manifest_decode(frame->bytes.ptr, frame->bytes.len, repo, &frame->read, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(line, col, "called `Result::unwrap()` on an `Err` value", &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->read_live = true;
	frame->bytes_live = false;
	std_vec_free_Checked(&frame->bytes, 1, 1);
}

/* repo_tests_pair_Debug_fmt is the Debug of an (Address, Locator). */
static bool
repo_tests_pair_Debug_fmt(const void *value, StdFormatter *f)
{
	const chunks_TupleAddressLocator_t *pair = value;
	StdTupleRef tuple = {2, {{&pair->f0, chunks_address_Address_Debug_fmt}, {&pair->f1, chunks_trailer_Locator_Debug_fmt}}};

	return std_tuple_Debug_fmt(&tuple, f);
}

/* repo_tests_u64s_Debug_fmt is the Debug of a (u64, u64). */
static bool
repo_tests_u64s_Debug_fmt(const void *value, StdFormatter *f)
{
	const std_TupleU64U64_t *pair = value;
	StdTupleRef tuple = {2, {{&pair->f0, std_u64_Debug_fmt}, {&pair->f1, std_u64_Debug_fmt}}};

	return std_tuple_Debug_fmt(&tuple, f);
}

/* repo_tests_a_manifest_reads_back_what_it_was_written_with_body_0 is the test's body. */
static void
repo_tests_a_manifest_reads_back_what_it_was_written_with_body_0(void *frame_)
{
	repo_tests_frame_0_t *frame = frame_;
	TreesPublishedRepoId repo = trees_published_RepoId_unique();
	chunks_TupleAddressLocator_t name;
	RepoManifest written = {0};
	chunks_TupleAddressLocator_t got;
	StdOptionRef got_ref;
	StdOptionRef want_ref = {true, {&name, repo_tests_pair_Debug_fmt}};
	std_TupleU64U64_t points;
	std_TupleU64U64_t want_points = {5, 4};

	memset(name.f0.f0, 7, sizeof(name.f0.f0));
	name.f1 = chunks_trailer_Locator_at(3, 99, 41);
	written.has_bases = true;
	written.bases = trees_published_Published_decoded(repo, name.f0, name.f1);
	written.folded = 5;
	written.redo = 4;
	repo_tests_round_trip(frame, &written, repo, 591, 62);
	if (frame->read.has_bases)
		trees_published_Published_parts(&frame->read.bases, &got.f0, &got.f1);
	got_ref.has = frame->read.has_bases;
	got_ref.some.value = &got;
	got_ref.some.fmt = repo_tests_pair_Debug_fmt;
	STD_ASSERT_EQ(592, 9,
				  frame->read.has_bases && chunks_address_Address_PartialEq_eq(&got.f0, &name.f0) &&
				  chunks_trailer_Locator_PartialEq_eq(&got.f1, &name.f1),
				  &got_ref, std_option_Debug_fmt, &want_ref, std_option_Debug_fmt);
	points.f0 = frame->read.folded;
	points.f1 = frame->read.redo;
	STD_ASSERT_EQ(593, 9, points.f0 == 5 && points.f1 == 4, &points, repo_tests_u64s_Debug_fmt,
				  &want_points, repo_tests_u64s_Debug_fmt);
	frame->read_live = false;
	repo_Manifest_drop(&frame->read);
	memset(&written, 0, sizeof(written));
	written.redo = 9;
	repo_tests_round_trip(frame, &written, repo, 596, 18);
	STD_ASSERT(597, 9, !frame->read.has_bases, "empty.bases.is_none()");
	points.f0 = frame->read.folded;
	points.f1 = frame->read.redo;
	want_points.f0 = 0;
	want_points.f1 = 9;
	STD_ASSERT_EQ(598, 9, points.f0 == 0 && points.f1 == 9, &points, repo_tests_u64s_Debug_fmt,
				  &want_points, repo_tests_u64s_Debug_fmt);
	repo_tests_pad_0(frame);
}

/* test_repo_tests_a_manifest_reads_back_what_it_was_written_with: a manifest's bytes give back its base tree's name and its two positions, with and without a base tree. */
void
test_repo_tests_a_manifest_reads_back_what_it_was_written_with(void)
{
	repo_tests_frame_0_t frame = {0};

	std_landing(&frame, repo_tests_a_manifest_reads_back_what_it_was_written_with_body_0, repo_tests_pad_0);
}

/* repo_tests_refused reports whether a manifest's decode refuses bytes as corrupt. */
static bool
repo_tests_refused(const uint8 *bytes, size_t bytes_len, TreesPublishedRepoId repo)
{
	RepoManifest read;
	ChunksErrorError error;
	bool		corrupt;

	if (repo_Manifest_decode(bytes, bytes_len, repo, &read, &error))
	{
		repo_Manifest_drop(&read);
		return false;
	}
	corrupt = error.tag == CHUNKS_ERROR_ERROR_CORRUPT;
	chunks_error_Error_drop(&error);
	return corrupt;
}

/* repo_tests_a_manifest_that_is_not_one_is_refused_body_0 is the test's body. */
static void
repo_tests_a_manifest_that_is_not_one_is_refused_body_0(void *frame_)
{
	repo_tests_frame_0_t *frame = frame_;
	TreesPublishedRepoId repo = trees_published_RepoId_unique();
	RepoManifest written = {0};
	ChunksAddressAddress address;
	uint8	   *whole;
	size_t		whole_len;
	ChunksRecordRecordWriter w;
	StdVec		body;
	StdVec		unknown;

	memset(address.f0, 7, sizeof(address.f0));
	written.has_bases = true;
	written.bases = trees_published_Published_decoded(repo, address, chunks_trailer_Locator_at(3, 99, 41));
	written.folded = 5;
	written.redo = 4;
	repo_Manifest_encode(&written, &frame->bytes);
	frame->bytes_live = true;
	whole = frame->bytes.ptr;
	whole_len = frame->bytes.len;
	STD_ASSERT(609, 9, repo_tests_refused(whole, 0, repo), "refused(&[])");
	memset(whole, 0, 8);
	STD_ASSERT(610, 9, repo_tests_refused(whole, whole_len, repo),
			   "refused(&[&[0u8; 8][..], &whole[8..]].concat())");
	memcpy(whole, REPO_MANIFEST_MAGIC, sizeof(REPO_MANIFEST_MAGIC));
	STD_ASSERT(611, 9, repo_tests_refused(whole, STD_SUB(usize, whole_len, 1, 611, 34), repo),
			   "refused(&whole[..whole.len() - 1])");
	std_vec_u8_push_Checked(&frame->bytes, 0);
	STD_ASSERT(612, 9, repo_tests_refused(frame->bytes.ptr, frame->bytes.len, repo),
			   "refused(&[&whole[..], &[0u8][..]].concat())");
	frame->bytes_live = false;
	std_vec_free_Checked(&frame->bytes, 1, 1);
	w = chunks_record_RecordWriter_new();
	chunks_record_RecordWriter_uint(chunks_record_RecordWriter_uint(chunks_record_RecordWriter_uint(&w, 4, 1),
																	REPO_FOLDED, 5), REPO_REDO, 4);
	body = chunks_record_RecordWriter_finish(&w);
	std_slice_to_vec_Checked(&unknown, REPO_MANIFEST_MAGIC, sizeof(REPO_MANIFEST_MAGIC), 1, 1);
	chunks_varint_put_varint(&unknown, (uint64) body.len);
	std_vec_extend_from_slice_Checked(&unknown, body.ptr, body.len, 1, 1);
	std_vec_free_Checked(&body, 1, 1);
	chunks_varint_put_varint(&unknown, 0);
	frame->bytes = unknown;
	frame->bytes_live = true;
	STD_ASSERT(620, 9, repo_tests_refused(frame->bytes.ptr, frame->bytes.len, repo), "refused(&unknown)");
	repo_tests_pad_0(frame);
}

/* test_repo_tests_a_manifest_that_is_not_one_is_refused: a manifest with another magic, cut short, with bytes after its address table, or with a field it doesn't know is refused as corrupt. */
void
test_repo_tests_a_manifest_that_is_not_one_is_refused(void)
{
	repo_tests_frame_0_t frame = {0};

	std_landing(&frame, repo_tests_a_manifest_that_is_not_one_is_refused_body_0, repo_tests_pad_0);
}

/* repo_tests_base_t is what a test's gather gives: one storage's base in pg_default by its relation, with a field and a fold point. */
typedef struct repo_tests_base_t
{
	uint32		relation;
	uint64		field;
	HostLsn		folded;
} repo_tests_base_t;

/* repo_tests_base returns a save's edit of a storage in pg_default by its relation number: a record of one field, and a fold point. */
static void
repo_tests_base(const repo_tests_base_t *base, RepoGathered *result)
{
	ChunksRecordRecordWriter w = chunks_record_RecordWriter_new();

	chunks_record_RecordWriter_uint(&w, 1, base->field);
	result->tag = REPO_GATHERED_BASE;
	chunks_record_RecordWriter_finish_value(&w, &result->u.base.record.bytes, &result->u.base.record.addresses);
	result->u.base.storage.tablespace = HOST_PG_DEFAULT;
	result->u.base.storage.relation = base->relation;
	result->u.base.folded = base->folded;
}

/* repo_tests_bases_gather_0 is a test's |_, _| Ok(vec![base(...)]): one base edit. */
static bool
repo_tests_bases_gather_0(void *captures, HostIoIo *io, const RepoRead *read, StdVec *result, ChunksErrorError *error)
{
	RepoGathered edit;

	std_vec_with_capacity_Checked(result, 1, sizeof(RepoGathered), _Alignof(RepoGathered));
	repo_tests_base(captures, &edit);
	std_vec_push_Checked(result, &edit, sizeof(edit), _Alignof(RepoGathered));
	return true;
}

/* repo_tests_marks_0 is saved's marks: it records that the save called it. */
static bool
repo_tests_marks_0(void *captures, HostIoIo *io, ChunksErrorError *error)
{
	*(bool *) captures = true;
	return true;
}

/* repo_tests_repo_frame_0_t is a repository test's state across its pad: what it holds, each while live. */
typedef struct repo_tests_repo_frame_0_t
{
	SimcTestIo	test_io;
	bool		test_io_live;
	RepoRepo	repo;
	bool		repo_live;
	RepoRepo	again;
	bool		again_live;
	HostSharedReader reading;
	bool		reading_live;
	StdArcInner *manifest;
	bool		manifest_live;
	RepoManifest previous;
	bool		previous_live;
	TreesNodeFlat flat;
	bool		flat_live;
	ChunksErrorError error;
	bool		error_live;
} repo_tests_repo_frame_0_t;

/* repo_tests_repo_pad_0 is a repository test's pad: what it holds, while live. */
static void
repo_tests_repo_pad_0(void *frame_)
{
	repo_tests_repo_frame_0_t *frame = frame_;

	if (frame->error_live)
	{
		frame->error_live = false;
		chunks_error_Error_drop(&frame->error);
	}
	if (frame->flat_live)
	{
		frame->flat_live = false;
		trees_node_Flat_drop(&frame->flat);
	}
	if (frame->previous_live)
	{
		frame->previous_live = false;
		repo_Manifest_drop(&frame->previous);
	}
	if (frame->manifest_live)
	{
		frame->manifest_live = false;
		repo_ArcManifest_drop(frame->manifest);
	}
	if (frame->reading_live)
	{
		frame->reading_live = false;
		host_shared_Reader_drop(&frame->reading);
	}
	if (frame->again_live)
	{
		frame->again_live = false;
		repo_Repo_drop(&frame->again);
	}
	if (frame->repo_live)
	{
		frame->repo_live = false;
		repo_Repo_drop(&frame->repo);
	}
	if (frame->test_io_live)
	{
		frame->test_io_live = false;
		simc_TestIo_drop(&frame->test_io);
	}
}

/* repo_tests_unwrap fails a repository test at a site of its Rust when a step returned an error. */
static void
repo_tests_unwrap(repo_tests_repo_frame_0_t *frame, bool ok, uint32 line, uint32 col)
{
	if (!ok)
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(line, col, "called `Result::unwrap()` on an `Err` value", &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
}

/* repo_tests_manifest reads a repository's newest manifest into the frame, unwrapping at a site. */
static const RepoManifest *
repo_tests_manifest(repo_tests_repo_frame_0_t *frame, const RepoRepo *repo, uint32 line, uint32 col)
{
	if (frame->manifest_live)
	{
		frame->manifest_live = false;
		repo_ArcManifest_drop(frame->manifest);
	}
	repo_tests_unwrap(frame, repo_Repo_manifest(repo, &frame->test_io.io, &frame->manifest, &frame->error), line, col);
	frame->manifest_live = true;
	return repo_Repo_newest(frame->manifest);
}

/*
 * repo_tests_saved saves the repository with one storage's base after a
 * checkpoint completed at a redo point, checks that the save called its
 * saved, and returns the name of the base tree the manifest names.
 */
static chunks_TupleAddressLocator_t
repo_tests_saved(repo_tests_repo_frame_0_t *frame, const RepoRepo *repo, uint32 relation, HostLsn folded,
				 HostLsn redo)
{
	repo_tests_base_t base = {relation, 1, folded};
	HostSharedReader reading;
	HostError	host_error;
	bool		called = false;
	bool		ok;
	const RepoManifest *manifest;
	chunks_TupleAddressLocator_t name;

	simc_checkpointed(redo);
	if (!host_shared_read(&frame->test_io.io, &reading, &host_error))
	{
		frame->error = chunks_error_Error_FromError_from(&host_error);
		repo_tests_unwrap(frame, false, 568, 46);
	}
	ok = repo_Repo_save(repo, &frame->test_io.io, &reading, false, NULL, repo_tests_bases_gather_0, &base,
						repo_tests_marks_0, &called, &frame->error);
	host_shared_Reader_drop(&reading);
	repo_tests_unwrap(frame, ok, 574, 90);
	STD_ASSERT_MSG(575, 9, called, "a save whose write landed called no `saved`");
	manifest = repo_tests_manifest(frame, repo, 576, 14);
	if (!manifest->has_bases)
		STD_OPTION_UNWRAP_FAILED(576, 68);
	trees_published_Published_parts(&manifest->bases, &name.f0, &name.f1);
	return name;
}

/* repo_tests_name_is asserts that a manifest names a base tree, the one given. */
static void
repo_tests_name_is(const RepoManifest *manifest, const chunks_TupleAddressLocator_t *name, uint32 line, uint32 col)
{
	chunks_TupleAddressLocator_t got = {0};
	StdOptionRef got_ref = {manifest->has_bases, {&got, repo_tests_pair_Debug_fmt}};
	StdOptionRef want_ref = {true, {name, repo_tests_pair_Debug_fmt}};

	if (manifest->has_bases)
		trees_published_Published_parts(&manifest->bases, &got.f0, &got.f1);
	STD_ASSERT_EQ(line, col,
				  manifest->has_bases && chunks_address_Address_PartialEq_eq(&got.f0, &name->f0) &&
				  chunks_trailer_Locator_PartialEq_eq(&got.f1, &name->f1),
				  &got_ref, std_option_Debug_fmt, &want_ref, std_option_Debug_fmt);
}

/* repo_tests_a_repository_keeps_its_manifest_and_chunks_body_0 is the test's body. */
static void
repo_tests_a_repository_keeps_its_manifest_and_chunks_body_0(void *frame_)
{
	repo_tests_repo_frame_0_t *frame = frame_;
	HostIoIo   *io;
	StdPathBuf	want;
	StdStr		got_dir;
	StdStr		want_dir;
	const RepoManifest *manifest;
	chunks_TupleAddressLocator_t name;
	TreesPublishedRepoId repository;
	std_TupleU64U64_t points;
	std_TupleU64U64_t want_points = {30, 20};
	HostError	host_error;
	TreesStoreReader reader;
	bool		has;
	bool		accepted;

	frame->test_io = simc_io();
	frame->test_io_live = true;
	io = &frame->test_io.io;
	simc_checkpointed(12);
	repo_tests_unwrap(frame, repo_Repo_create(io, 9101, HOST_PG_DEFAULT, &frame->repo, &frame->error), 631, 65);
	frame->repo_live = true;
	want = host_tablespace_dir(9101, HOST_PG_DEFAULT);
	got_dir.ptr = repo_Repo_dir(&frame->repo, &got_dir.len);
	want_dir.ptr = (const char *) want.bytes.ptr;
	want_dir.len = want.bytes.len;
	if (got_dir.len != want_dir.len || memcmp(got_dir.ptr, want_dir.ptr, got_dir.len) != 0)
		std_assert_failed(RUST_FILE, 632, 9, "==", &got_dir, std_path_Debug_fmt, &want_dir,
						  std_path_Debug_fmt, NULL);
	std_vec_free_Checked(&want.bytes, 1, 1);
	manifest = repo_tests_manifest(frame, &frame->repo, 633, 38);
	STD_ASSERT(634, 9, !manifest->has_bases && manifest->folded == 0 && manifest->redo == 12,
			   "made.bases.is_none() && (made.folded, made.redo) == (0, 12)");
	repo_tests_unwrap(frame, repo_Repo_previous(&frame->repo, io, &has, &frame->previous, &frame->error),
					  635, 35);
	frame->previous_live = has;
	STD_ASSERT(635, 9, !has, "repo.previous(io).unwrap().is_none()");
	name = repo_tests_saved(frame, &frame->repo, 1, 30, 20);
	repository = repo_Repo_id(&frame->repo);
	manifest = repo_tests_manifest(frame, &frame->repo, 638, 42);
	repo_tests_name_is(manifest, &name, 639, 9);
	points.f0 = manifest->folded;
	points.f1 = manifest->redo;
	STD_ASSERT_EQ(640, 9, points.f0 == 30 && points.f1 == 20, &points, repo_tests_u64s_Debug_fmt,
				  &want_points, repo_tests_u64s_Debug_fmt);
	accepted = false;
	if (manifest->has_bases)
	{
		accepted = trees_published_RepoId_accept(&repository, &manifest->bases, "test", 4, &frame->error);
		if (!accepted)
			chunks_error_Error_drop(&frame->error);
	}
	STD_ASSERT(641, 9, accepted,
			   "manifest.bases.as_ref().is_some_and(|bases| repository.accept(bases, \"test\").is_ok())");
	if (!host_shared_read(io, &frame->reading, &host_error))
	{
		frame->error = chunks_error_Error_FromError_from(&host_error);
		repo_tests_unwrap(frame, false, 642, 46);
	}
	frame->reading_live = true;
	reader = repo_Repo_reader(&frame->repo, HOST_CACHE_PLACEMENT_SERVER, &frame->reading);
	accepted = trees_store_Reader_read(&reader, io, &name.f0, name.f1, &frame->flat, &frame->error);
	if (accepted)
	{
		frame->flat_live = false;
		trees_node_Flat_drop(&frame->flat);
	}
	else
		chunks_error_Error_drop(&frame->error);
	STD_ASSERT(643, 9, accepted, "repo.reader(Placement::Server, &reading).read(io, &name.0, name.1).is_ok()");
	repo_tests_unwrap(frame, repo_Repo_previous(&frame->repo, io, &has, &frame->previous, &frame->error),
					  644, 40);
	if (!has)
		STD_OPTION_UNWRAP_FAILED(644, 49);
	frame->previous_live = true;
	STD_ASSERT(645, 9, !frame->previous.has_bases, "before.bases.is_none()");
	frame->manifest_live = false;
	repo_ArcManifest_drop(frame->manifest);
	frame->repo_live = false;
	repo_tests_unwrap(frame, repo_Repo_close(&frame->repo, io, &frame->reading, &frame->error), 646, 34);
	repo_tests_unwrap(frame, repo_Repo_open(io, 9101, HOST_PG_DEFAULT, &frame->again, &frame->error), 647, 64);
	frame->again_live = true;
	manifest = repo_tests_manifest(frame, &frame->again, 648, 39);
	repo_tests_name_is(manifest, &name, 648, 9);
	repo_tests_repo_pad_0(frame);
}

/*
 * test_repo_tests_a_repository_keeps_its_manifest_and_chunks: a repository is
 * made with an empty manifest; a saved manifest names a root that was held
 * and published, whose tree reads through the repository's reader; the
 * manifest it replaced is the previous one; and the repository opened again
 * reads the same.
 */
void
test_repo_tests_a_repository_keeps_its_manifest_and_chunks(void)
{
	repo_tests_repo_frame_0_t frame = {0};

	std_landing(&frame, repo_tests_a_repository_keeps_its_manifest_and_chunks_body_0, repo_tests_repo_pad_0);
}

/* repo_tests_a_handle_reads_the_manifest_another_saved_body_0 is the test's body. */
static void
repo_tests_a_handle_reads_the_manifest_another_saved_body_0(void *frame_)
{
	repo_tests_repo_frame_0_t *frame = frame_;
	HostIoIo   *io;
	const RepoManifest *manifest;
	chunks_TupleAddressLocator_t name;
	chunks_TupleAddressLocator_t next;
	bool		has;

	frame->test_io = simc_io();
	frame->test_io_live = true;
	io = &frame->test_io.io;
	repo_tests_unwrap(frame, repo_Repo_create(io, 9102, HOST_PG_DEFAULT, &frame->repo, &frame->error), 656, 65);
	frame->repo_live = true;
	repo_tests_unwrap(frame, repo_Repo_open(io, 9102, HOST_PG_DEFAULT, &frame->again, &frame->error), 657, 65);
	frame->again_live = true;
	manifest = repo_tests_manifest(frame, &frame->again, 658, 37);
	STD_ASSERT(658, 9, !manifest->has_bases, "theirs.manifest(io).unwrap().bases.is_none()");
	name = repo_tests_saved(frame, &frame->repo, 1, 7, 6);
	repo_tests_name_is(repo_tests_manifest(frame, &frame->again, 660, 40), &name, 660, 9);
	next = repo_tests_saved(frame, &frame->again, 2, 9, 8);
	repo_tests_name_is(repo_tests_manifest(frame, &frame->repo, 662, 38), &next, 662, 9);
	repo_tests_unwrap(frame, repo_Repo_previous(&frame->repo, io, &has, &frame->previous, &frame->error),
					  663, 38);
	if (!has)
		STD_OPTION_UNWRAP_FAILED(663, 47);
	frame->previous_live = true;
	repo_tests_name_is(&frame->previous, &name, 663, 9);
	repo_tests_repo_pad_0(frame);
}

/* test_repo_tests_a_handle_reads_the_manifest_another_saved: a process's copy of the manifest is read again once another handle of the repository saved one. */
void
test_repo_tests_a_handle_reads_the_manifest_another_saved(void)
{
	repo_tests_repo_frame_0_t frame = {0};

	std_landing(&frame, repo_tests_a_handle_reads_the_manifest_another_saved_body_0, repo_tests_repo_pad_0);
}

/* repo_tests_unsettled reports whether an open of a repository is refused as a manifest that is not whole. */
static bool
repo_tests_unsettled(repo_tests_repo_frame_0_t *frame, uint32 repository)
{
	RepoRepo	opened;
	ChunksErrorError error;
	bool		unsettled;

	if (repo_Repo_open(&frame->test_io.io, repository, HOST_PG_DEFAULT, &opened, &error))
	{
		repo_Repo_drop(&opened);
		return false;
	}
	unsettled = error.tag == CHUNKS_ERROR_ERROR_CORRUPT &&
		std_str_contains_str_Checked(error.u.corrupt.ptr, error.u.corrupt.len, "manifest is not whole",
									 strlen("manifest is not whole"));
	chunks_error_Error_drop(&error);
	return unsettled;
}

/* repo_tests_remove removes a file of a repository the test reaches by path, unwrapping at a site. */
static void
repo_tests_remove(const StdPathBuf *path, uint32 line, uint32 col)
{
	StdIoError	io_error;

	if (!std_fs_remove_file(path->bytes.ptr, path->bytes.len, &io_error))
		STD_RESULT_EXPECT_FAILED(line, col, "called `Result::unwrap()` on an `Err` value", &io_error,
								 std_io_Error_Debug_fmt);
}

/* repo_tests_a_manifest_that_is_not_whole_is_refused_body_0 is the test's body. */
static void
repo_tests_a_manifest_that_is_not_whole_is_refused_body_0(void *frame_)
{
	repo_tests_repo_frame_0_t *frame = frame_;
	HostIoIo   *io;
	const char *dir;
	size_t		dir_len;
	StdPathBuf	path;
	StdPathBuf	previous;

	frame->test_io = simc_io();
	frame->test_io_live = true;
	io = &frame->test_io.io;
	repo_tests_unwrap(frame, repo_Repo_create(io, 9103, HOST_PG_DEFAULT, &frame->repo, &frame->error), 672, 65);
	frame->repo_live = true;
	repo_tests_saved(frame, &frame->repo, 1, 3, 2);
	dir = repo_Repo_dir(&frame->repo, &dir_len);
	std_path_join(dir, dir_len, CHUNKS_PACKS_MANIFEST, strlen(CHUNKS_PACKS_MANIFEST), &path);
	repo_tests_remove(&path, 675, 37);
	STD_ASSERT(680, 9, repo_tests_unsettled(frame, 9103), "unsettled(Repo::open(io, id(9103), host::PG_DEFAULT))");
	repo_tests_saved(frame, &frame->repo, 2, 5, 4);
	{
		StdArcInner *manifest;
		bool		ok = repo_Repo_manifest(&frame->repo, io, &manifest, &frame->error);

		if (ok)
			repo_ArcManifest_drop(manifest);
		else
			chunks_error_Error_drop(&frame->error);
		STD_ASSERT(682, 9, ok, "repo.manifest(io).is_ok()");
	}
	repo_tests_remove(&path, 683, 37);
	previous = chunks_file_previous(path.bytes.ptr, path.bytes.len);
	repo_tests_remove(&previous, 684, 47);
	std_vec_free_Checked(&previous.bytes, 1, 1);
	std_vec_free_Checked(&path.bytes, 1, 1);
	STD_ASSERT(685, 9, repo_tests_unsettled(frame, 9103), "unsettled(Repo::open(io, id(9103), host::PG_DEFAULT))");
	repo_tests_repo_pad_0(frame);
}

/*
 * test_repo_tests_a_manifest_that_is_not_whole_is_refused: a manifest
 * readable only as its previous generation, or gone with it, is refused by
 * an open and by a handle that reads it again, naming the file.
 */
void
test_repo_tests_a_manifest_that_is_not_whole_is_refused(void)
{
	repo_tests_repo_frame_0_t frame = {0};

	std_landing(&frame, repo_tests_a_manifest_that_is_not_whole_is_refused_body_0, repo_tests_repo_pad_0);
}

/*
 * repo_tests_runs returns a checkpoint's runs at a redo point with three
 * files: a storage's rows, a working state's chunks, and a transaction's
 * spill of the storage's rows.
 */
static void
repo_tests_runs(HostLsn redo, RepoRuns *result)
{
	RepoRunFile files[3] = {
		{.kind = 0, .tablespace = HOST_PG_DEFAULT, .relation = 16384, .len = 4096, .hash = 0xfeed},
		{.kind = 3, .tablespace = 0, .relation = 0, .len = 128, .hash = PG_UINT64_MAX},
		{.kind = 5, .tablespace = HOST_PG_DEFAULT, .relation = 16384, .len = 512, .hash = 7, .has_spilled = true,
		.spilled = {((uint64) 1 << 40) | 742, 3}},
	};

	result->redo = redo;
	std_slice_to_vec_Checked(&result->files, files, lengthof(files), sizeof(RepoRunFile), _Alignof(RepoRunFile));
}

/* repo_tests_option_runs_Debug_fmt is the Debug of an Option<Runs>, a manifest's runs. */
static bool
repo_tests_option_runs_Debug_fmt(const void *value, StdFormatter *f)
{
	const RepoManifest *manifest = value;
	StdOptionRef shown = {manifest->has_runs, {&manifest->runs, repo_Runs_Debug_fmt}};

	return std_option_Debug_fmt(&shown, f);
}

/* repo_tests_encoded returns a manifest's bytes whose record holds field 9 and a runs field of an inner record, which it takes. */
static void
repo_tests_encoded(ChunksRecordRecordWriter *inner, StdVec *result)
{
	ChunksRecordRecordWriter w = chunks_record_RecordWriter_new();
	StdVec		bytes;
	StdVec		addresses;

	chunks_record_RecordWriter_record(chunks_record_RecordWriter_uint(&w, REPO_REDO, 9), REPO_RUNS, inner);
	chunks_record_RecordWriter_finish_value(&w, &bytes, &addresses);
	std_vec_free_Checked(&addresses, sizeof(chunks_TupleU32Locator_t), _Alignof(chunks_TupleU32Locator_t));
	std_slice_to_vec_Checked(result, REPO_MANIFEST_MAGIC, sizeof(REPO_MANIFEST_MAGIC), 1, 1);
	chunks_varint_put_varint(result, (uint64) bytes.len);
	std_vec_extend_from_slice_Checked(result, bytes.ptr, bytes.len, 1, 1);
	std_vec_free_Checked(&bytes, 1, 1);
	chunks_varint_put_varint(result, 0);
}

/* repo_tests_listing returns the runs field's inner record of redo 7 listing one file with one field. */
static ChunksRecordRecordWriter
repo_tests_listing(uint32 number, uint64 value)
{
	ChunksRecordRecordWriter item = chunks_record_RecordWriter_new();
	ChunksRecordRecordWriter inner = chunks_record_RecordWriter_new();
	StdVec		items;

	chunks_record_RecordWriter_uint(&item, number, value);
	std_vec_with_capacity_Checked(&items, 1, sizeof(item), _Alignof(ChunksRecordRecordWriter));
	std_vec_push_Checked(&items, &item, sizeof(item), _Alignof(ChunksRecordRecordWriter));
	chunks_record_RecordWriter_list(chunks_record_RecordWriter_uint(&inner, 1, 7), 2, &items);
	return inner;
}

/* repo_tests_decodes reports whether a manifest's bytes decode, dropping what the decode gives. */
static bool
repo_tests_decodes(repo_tests_frame_0_t *frame)
{
	RepoManifest read;
	ChunksErrorError error;
	bool		decoded = repo_Manifest_decode(frame->bytes.ptr, frame->bytes.len, trees_published_RepoId_unique(),
											   &read, &error);

	if (decoded)
		repo_Manifest_drop(&read);
	else
		chunks_error_Error_drop(&error);
	frame->bytes_live = false;
	std_vec_free_Checked(&frame->bytes, 1, 1);
	return decoded;
}

/* repo_tests_runs_body_0 is the runs codec test's body. */
static void
repo_tests_runs_body_0(void *frame_)
{
	repo_tests_frame_0_t *frame = frame_;
	TreesPublishedRepoId repo = trees_published_RepoId_unique();
	ChunksRecordRecordWriter inner;
	static const struct
	{
		uint32		number;
		uint64		value;
	}			listings[] = {{1, 256}, {2, (uint64) 1 << 32}, {3, (uint64) 1 << 32}, {6, 1}, {7, 1}, {8, 1}};

	for (size_t k = 0, k_len = 3; k < k_len; k++)
	{
		RepoManifest written = {.redo = 9};

		if (k == 0)
		{
			repo_tests_runs(UINT64CONST(0x100000a28), &written.runs);
			written.has_runs = true;
		}
		else if (k == 1)
		{
			written.runs.redo = 7;
			std_vec_new(&written.runs.files, sizeof(RepoRunFile), _Alignof(RepoRunFile));
			written.has_runs = true;
		}
		repo_tests_round_trip(frame, &written, repo, 886, 67);
		STD_ASSERT_EQ(886, 13,
					  frame->read.has_runs == written.has_runs &&
					  (!written.has_runs || repo_Runs_PartialEq_eq(&frame->read.runs, &written.runs)),
					  &frame->read, repo_tests_option_runs_Debug_fmt, &written, repo_tests_option_runs_Debug_fmt);
		frame->read_live = false;
		repo_Manifest_drop(&frame->read);
		if (written.has_runs)
			repo_Runs_drop(&written.runs);
	}
	for (size_t k = 0, k_len = 1 + lengthof(listings); k < k_len; k++)
	{
		size_t		at = k;
		StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&at, std_usize_Display_fmt}};
		StdFmtArguments arguments = {pieces, 1, args};

		if (k == 0)
		{
			inner = chunks_record_RecordWriter_new();
			chunks_record_RecordWriter_uint(chunks_record_RecordWriter_uint(&inner, 1, 7), 3, 1);
		}
		else
			inner = repo_tests_listing(listings[std_wrapping_sub_usize(k, 1)].number, listings[std_wrapping_sub_usize(k, 1)].value);
		repo_tests_encoded(&inner, &frame->bytes);
		frame->bytes_live = true;
		if (repo_tests_decodes(frame))
			std_panic_fmt(RUST_FILE, 917, 13, &arguments);
	}
	inner = repo_tests_listing(1, 255);
	repo_tests_encoded(&inner, &frame->bytes);
	frame->bytes_live = true;
	STD_ASSERT(919, 9, repo_tests_decodes(frame), "Manifest::decode(&encoded(listing(1, 255)), repo).is_ok()");
	repo_tests_pad_0(frame);
}

/*
 * test_repo_tests_a_manifests_runs_read_back_and_unknown_fields_are_refused:
 * field 10's codec, a manifest's runs reading back as written, and runs with
 * a field they don't know, or a file with a field, a number, or a spill's
 * half past what it holds, refused as corrupt.
 */
void
test_repo_tests_a_manifests_runs_read_back_and_unknown_fields_are_refused(void)
{
	repo_tests_frame_0_t frame = {0};

	std_landing(&frame, repo_tests_runs_body_0, repo_tests_pad_0);
}

/* repo_tests_edit_t is one edit a test's gather gives: a storage's base, or the storage in pg_default taken out. */
typedef struct repo_tests_edit_t
{
	bool		unlinked;
	repo_tests_base_t base;
} repo_tests_edit_t;

/* repo_tests_edit_gather_0 is a test's |_, _| Ok(vec![edit()]). */
static bool
repo_tests_edit_gather_0(void *captures, HostIoIo *io, const RepoRead *read, StdVec *result, ChunksErrorError *error)
{
	const repo_tests_edit_t *edit = captures;
	RepoGathered made;

	if (!edit->unlinked)
		return repo_tests_bases_gather_0((void *) &edit->base, io, read, result, error);
	std_vec_with_capacity_Checked(result, 1, sizeof(RepoGathered), _Alignof(RepoGathered));
	made.tag = REPO_GATHERED_UNLINKED;
	made.u.unlinked.tablespace = HOST_PG_DEFAULT;
	made.u.unlinked.relation = edit->base.relation;
	std_vec_push_Checked(result, &made, sizeof(made), _Alignof(RepoGathered));
	return true;
}

/* repo_tests_ok_saved_0 is a test's |_| Ok(()). */
static bool
repo_tests_ok_saved_0(void *captures, HostIoIo *io, ChunksErrorError *error)
{
	return true;
}

/* repo_tests_fields saves one edit after a checkpoint at a redo point and asserts the manifest's fold point and redo point. */
static void
repo_tests_fields(repo_tests_repo_frame_0_t *frame, const repo_tests_edit_t *edit, HostLsn redo, uint64 folded_is,
				  uint64 redo_is, uint32 line, uint32 col)
{
	const RepoManifest *manifest;
	std_TupleU64U64_t got;
	std_TupleU64U64_t want = {folded_is, redo_is};

	simc_checkpointed(redo);
	repo_tests_unwrap(frame, repo_Repo_save(&frame->repo, &frame->test_io.io, &frame->reading, false, NULL,
											repo_tests_edit_gather_0, (void *) edit, repo_tests_ok_saved_0, NULL,
											&frame->error),
					  752, 80);
	manifest = repo_tests_manifest(frame, &frame->repo, 753, 46);
	got.f0 = manifest->folded;
	got.f1 = manifest->redo;
	STD_ASSERT_EQ(line, col, got.f0 == want.f0 && got.f1 == want.f1, &got, repo_tests_u64s_Debug_fmt, &want,
				  repo_tests_u64s_Debug_fmt);
}

/* repo_tests_fold_point_body_0 is the fold point test's body. */
static void
repo_tests_fold_point_body_0(void *frame_)
{
	repo_tests_repo_frame_0_t *frame = frame_;
	HostIoIo   *io;
	HostError	host_error;
	repo_tests_edit_t first = {false, {1, 1, 10}};
	repo_tests_edit_t second = {false, {2, 1, 4}};
	repo_tests_edit_t storage = {true, {1, 0, 0}};
	repo_tests_edit_t last = {false, {2, 2, 12}};

	frame->test_io = simc_io();
	frame->test_io_live = true;
	io = &frame->test_io.io;
	repo_tests_unwrap(frame, repo_Repo_create(io, 9105, HOST_PG_DEFAULT, &frame->repo, &frame->error), 748, 65);
	frame->repo_live = true;
	if (!host_shared_read(io, &frame->reading, &host_error))
	{
		frame->error = chunks_error_Error_FromError_from(&host_error);
		repo_tests_unwrap(frame, false, 749, 46);
	}
	frame->reading_live = true;
	repo_tests_fields(frame, &first, 3, 10, 3, 756, 9);
	repo_tests_fields(frame, &second, 7, 10, 7, 757, 9);
	repo_tests_fields(frame, &storage, 8, 10, 8, 759, 9);
	repo_tests_fields(frame, &last, 9, 12, 9, 760, 9);
	repo_tests_repo_pad_0(frame);
}

/*
 * test_repo_tests_a_saves_fold_point_is_the_highest_written_and_its_redo_point_the_last_checkpoints:
 * a save writes field 8 as the higher of the field it read and its edits'
 * fold points, and field 9 as the redo point of the last checkpoint
 * completed when it writes, never the one the manifest it read held.
 */
void
test_repo_tests_a_saves_fold_point_is_the_highest_written_and_its_redo_point_the_last_checkpoints(void)
{
	repo_tests_repo_frame_0_t frame = {0};

	std_landing(&frame, repo_tests_fold_point_body_0, repo_tests_repo_pad_0);
}

/* repo_tests_nothing_gather_0 is a test's |_, _| Ok(Vec::new()). */
static bool
repo_tests_nothing_gather_0(void *captures, HostIoIo *io, const RepoRead *read, StdVec *result,
							ChunksErrorError *error)
{
	std_vec_new(result, sizeof(RepoGathered), _Alignof(RepoGathered));
	return true;
}

/* repo_tests_runs_is asserts that a manifest's runs are the ones given. */
static void
repo_tests_runs_is(const RepoManifest *manifest, const RepoRuns *runs, uint32 line, uint32 col)
{
	RepoManifest want = {.has_runs = true, .runs = *runs};

	STD_ASSERT_EQ(line, col, manifest->has_runs && repo_Runs_PartialEq_eq(&manifest->runs, runs), manifest,
				  repo_tests_option_runs_Debug_fmt, &want, repo_tests_option_runs_Debug_fmt);
}

/* repo_tests_checkpoints_runs_body_0 is the checkpoint's runs test's body. */
static void
repo_tests_checkpoints_runs_body_0(void *frame_)
{
	repo_tests_repo_frame_0_t *frame = frame_;
	HostIoIo   *io;
	HostError	host_error;
	chunks_TupleAddressLocator_t name;
	chunks_TupleAddressLocator_t moved;
	RepoRuns	written;
	RepoRuns	given;
	const RepoManifest *manifest;

	frame->test_io = simc_io();
	frame->test_io_live = true;
	io = &frame->test_io.io;
	simc_checkpointed(12);
	repo_tests_unwrap(frame, repo_Repo_create(io, 9105, HOST_PG_DEFAULT, &frame->repo, &frame->error), 930, 65);
	frame->repo_live = true;
	name = repo_tests_saved(frame, &frame->repo, 1, 30, 20);
	repo_tests_runs(UINT64CONST(0x100000a28), &written);
	if (!host_shared_read(io, &frame->reading, &host_error))
	{
		frame->error = chunks_error_Error_FromError_from(&host_error);
		repo_tests_unwrap(frame, false, 934, 50);
	}
	frame->reading_live = true;
	repo_Runs_Clone_clone(&written, &given);
	repo_tests_unwrap(frame, repo_Repo_save(&frame->repo, io, &frame->reading, true, &given, repo_tests_nothing_gather_0,
											NULL, repo_tests_ok_saved_0, NULL, &frame->error),
					  935, 95);
	frame->reading_live = false;
	host_shared_Reader_drop(&frame->reading);
	manifest = repo_tests_manifest(frame, &frame->repo, 937, 42);
	repo_tests_name_is(manifest, &name, 938, 9);
	repo_tests_runs_is(manifest, &written, 938, 9);
	moved = repo_tests_saved(frame, &frame->repo, 2, 40, 21);
	if (chunks_address_Address_PartialEq_eq(&moved.f0, &name.f0) &&
		chunks_trailer_Locator_PartialEq_eq(&moved.f1, &name.f1))
		std_assert_failed(RUST_FILE, 940, 9, "!=", &moved, repo_tests_pair_Debug_fmt, &name,
						  repo_tests_pair_Debug_fmt, NULL);
	repo_tests_unwrap(frame, repo_Repo_open(io, 9105, HOST_PG_DEFAULT, &frame->again, &frame->error), 941, 67);
	frame->again_live = true;
	manifest = repo_tests_manifest(frame, &frame->again, 941, 76);
	repo_tests_runs_is(manifest, &written, 942, 9);
	repo_Runs_drop(&written);
	repo_tests_repo_pad_0(frame);
}

/*
 * test_repo_tests_a_checkpoints_runs_are_written_and_kept_by_the_next_save: a
 * checkpoint's save with nothing gathered writes its runs with the bases as
 * read, and a save between two checkpoints, given no runs, writes the runs'
 * names as it read them beside its new base, so a restart's read finds them.
 */
void
test_repo_tests_a_checkpoints_runs_are_written_and_kept_by_the_next_save(void)
{
	repo_tests_repo_frame_0_t frame = {0};

	std_landing(&frame, repo_tests_checkpoints_runs_body_0, repo_tests_repo_pad_0);
}

/* repo_tests_gather_t is what a test's gather of several edits gives: its edits, and whether the first names a chunk. */
typedef struct repo_tests_gather_t
{
	size_t		len;
	repo_tests_edit_t edits[2];

	/*
	 * Whether the first edit is naming's: a base whose record names `chunk`
	 * in field 1.
	 */
	bool		naming;
	chunks_TupleAddressLocator_t chunk;
} repo_tests_gather_t;

/* repo_tests_gather_0 is a test's gather of its edits, in order. */
static bool
repo_tests_gather_0(void *captures, HostIoIo *io, const RepoRead *read, StdVec *result, ChunksErrorError *error)
{
	const repo_tests_gather_t *gather = captures;

	std_vec_with_capacity_Checked(result, gather->len, sizeof(RepoGathered), _Alignof(RepoGathered));
	for (size_t k = 0, k_len = gather->len; k < k_len; k++)
	{
		const repo_tests_edit_t *edit = &gather->edits[k];
		RepoGathered made;

		if (edit->unlinked)
		{
			made.tag = REPO_GATHERED_UNLINKED;
			made.u.unlinked.tablespace = HOST_PG_DEFAULT;
			made.u.unlinked.relation = edit->base.relation;
		}
		else if (k == 0 && gather->naming)
		{
			ChunksRecordRecordWriter w = chunks_record_RecordWriter_new();

			chunks_record_RecordWriter_addresses(&w, 1, &gather->chunk, 1);
			made.tag = REPO_GATHERED_BASE;
			chunks_record_RecordWriter_finish_value(&w, &made.u.base.record.bytes, &made.u.base.record.addresses);
			made.u.base.storage.tablespace = HOST_PG_DEFAULT;
			made.u.base.storage.relation = edit->base.relation;
			made.u.base.folded = edit->base.folded;
		}
		else
			repo_tests_base(&edit->base, &made);
		std_vec_push_Checked(result, &made, sizeof(made), _Alignof(RepoGathered));
	}
	return true;
}

/* repo_tests_save saves the test's repository with a gather's edits and a no-op saved, unwrapping at a site. */
static void
repo_tests_save(repo_tests_repo_frame_0_t *frame, const repo_tests_gather_t *gather, uint32 line, uint32 col)
{
	repo_tests_unwrap(frame, repo_Repo_save(&frame->repo, &frame->test_io.io, &frame->reading, false, NULL,
											repo_tests_gather_0, (void *) gather, repo_tests_ok_saved_0, NULL,
											&frame->error),
					  line, col);
}

/* repo_tests_base_of reads a storage's entry in pg_default from the newest base tree, unwrapping at a site. */
static bool
repo_tests_base_of(repo_tests_repo_frame_0_t *frame, uint32 relation, RepoBaseRecord *result, uint32 line,
				   uint32 col)
{
	RepoStorage storage = {HOST_PG_DEFAULT, relation};
	bool		has;

	repo_tests_unwrap(frame, repo_Repo_base(&frame->repo, &frame->test_io.io, &frame->reading, &storage, &has, result,
											&frame->error),
					  line, col);
	return has;
}

/* repo_tests_field returns the field of a record base made, dropping the record. */
static uint64
repo_tests_field(repo_tests_repo_frame_0_t *frame, RepoBaseRecord *record)
{
	ChunksRecordRecord read;
	uint64		field;

	repo_tests_unwrap(frame, chunks_record_Record_value(record->bytes.ptr, record->bytes.len, record->addresses.ptr,
														record->addresses.len, &read, &frame->error),
					  699, 57);
	repo_tests_unwrap(frame, chunks_record_Record_uint(&read, 1, &field, &frame->error), 699, 74);
	chunks_record_Record_drop(&read);
	repo_BaseRecord_drop(record);
	return field;
}

/* repo_tests_saved_base_body_0 is the saved base test's body. */
static void
repo_tests_saved_base_body_0(void *frame_)
{
	repo_tests_repo_frame_0_t *frame = frame_;
	HostIoIo   *io;
	HostError	host_error;
	RepoBaseRecord record;
	ChunksRecordRecord read;
	chunks_TupleAddressLocator_t chunk;
	chunks_TupleAddressLocator_t named;
	bool		has;
	StdOptionRef got_ref;
	StdOptionRef want_ref;
	uint64		fields[2];
	uint64		want_fields[2] = {3, 4};
	uint64		rotations;
	bool		called = false;
	repo_tests_gather_t gather = {0};

	frame->test_io = simc_io();
	frame->test_io_live = true;
	io = &frame->test_io.io;
	repo_tests_unwrap(frame, repo_Repo_create(io, 9104, HOST_PG_DEFAULT, &frame->repo, &frame->error), 709, 65);
	frame->repo_live = true;
	if (!host_shared_read(io, &frame->reading, &host_error))
	{
		frame->error = chunks_error_Error_FromError_from(&host_error);
		repo_tests_unwrap(frame, false, 710, 46);
	}
	frame->reading_live = true;
	has = repo_tests_base_of(frame, 1, &record, 712, 54);
	if (has)
		repo_BaseRecord_drop(&record);
	STD_ASSERT(712, 9, !has, "repo.base(io, &reading, &storage(1)).unwrap().is_none()");
	chunk = repo_tests_saved(frame, &frame->repo, 1, 4, 3);
	gather.len = 1;
	gather.edits[0].base.relation = 2;
	gather.edits[0].base.folded = 5;
	gather.naming = true;
	gather.chunk = chunk;
	repo_tests_save(frame, &gather, 714, 89);
	if (!repo_tests_base_of(frame, 2, &record, 715, 57))
		STD_OPTION_UNWRAP_FAILED(715, 66);
	repo_tests_unwrap(frame, chunks_record_Record_value(record.bytes.ptr, record.bytes.len, record.addresses.ptr,
														record.addresses.len, &read, &frame->error),
					  716, 65);
	repo_tests_unwrap(frame, chunks_record_Record_address(&read, 1, &has, &named.f0, &named.f1, &frame->error),
					  716, 85);
	chunks_record_Record_drop(&read);
	repo_BaseRecord_drop(&record);
	got_ref.has = has;
	got_ref.some.value = &named;
	got_ref.some.fmt = repo_tests_pair_Debug_fmt;
	want_ref.has = true;
	want_ref.some.value = &chunk;
	want_ref.some.fmt = repo_tests_pair_Debug_fmt;
	STD_ASSERT_EQ(717, 9,
				  has && chunks_address_Address_PartialEq_eq(&named.f0, &chunk.f0) &&
				  chunks_trailer_Locator_PartialEq_eq(&named.f1, &chunk.f1),
				  &got_ref, std_option_Debug_fmt, &want_ref, std_option_Debug_fmt);
	if (!repo_tests_base_of(frame, 1, &record, 718, 64))
		STD_OPTION_UNWRAP_FAILED(718, 73);
	fields[0] = repo_tests_field(frame, &record);
	want_fields[0] = 1;
	STD_ASSERT_EQ(718, 9, fields[0] == 1, &fields[0], std_u64_Debug_fmt, &want_fields[0], std_u64_Debug_fmt);
	memset(&gather, 0, sizeof(gather));
	gather.len = 1;
	gather.edits[0].unlinked = true;
	gather.edits[0].base.relation = 1;
	repo_tests_save(frame, &gather, 720, 61);
	has = repo_tests_base_of(frame, 1, &record, 721, 54);
	if (has)
		repo_BaseRecord_drop(&record);
	STD_ASSERT(721, 9, !has, "repo.base(io, &reading, &storage(1)).unwrap().is_none()");
	has = repo_tests_base_of(frame, 2, &record, 722, 54);
	if (has)
		repo_BaseRecord_drop(&record);
	STD_ASSERT(722, 9, has, "repo.base(io, &reading, &storage(2)).unwrap().is_some()");
	memset(&gather, 0, sizeof(gather));
	gather.len = 2;
	gather.edits[0].base.relation = 4;
	gather.edits[0].base.field = 4;
	gather.edits[0].base.folded = 6;
	gather.edits[1].base.relation = 3;
	gather.edits[1].base.field = 3;
	gather.edits[1].base.folded = 6;
	repo_tests_save(frame, &gather, 723, 98);
	for (size_t k = 0, k_len = 2; k < k_len; k++)
	{
		if (!repo_tests_base_of(frame, (uint32) std_wrapping_add_usize(3, k), &record, 724, 95))
			STD_OPTION_UNWRAP_FAILED(724, 104);
		fields[k] = repo_tests_field(frame, &record);
	}
	want_fields[0] = 3;
	{
		StdTupleRef got_fields = {2, {{&fields[0], std_u64_Debug_fmt}, {&fields[1], std_u64_Debug_fmt}}};
		StdTupleRef wanted = {2, {{&want_fields[0], std_u64_Debug_fmt}, {&want_fields[1], std_u64_Debug_fmt}}};

		STD_ASSERT_EQ(725, 9, fields[0] == 3 && fields[1] == 4, &got_fields, std_tuple_Debug_fmt, &wanted,
					  std_tuple_Debug_fmt);
	}
	rotations = chunks_packs_backend_Packs_rotations(&frame->repo.packs);
	repo_tests_unwrap(frame, repo_Repo_save(&frame->repo, io, &frame->reading, false, NULL, repo_tests_nothing_gather_0,
											NULL, repo_tests_marks_0, &called, &frame->error),
					  737, 10);
	STD_ASSERT(738, 9, chunks_packs_backend_Packs_rotations(&frame->repo.packs) == rotations && !called,
			   "repo.packs.rotations() == rotations && !called");
	repo_tests_repo_pad_0(frame);
}

/*
 * test_repo_tests_a_saved_base_reads_back_with_its_chunks_and_a_storage_taken_out_is_gone:
 * a saved base reads back with the chunks its record names; a storage the
 * tree doesn't hold reads as none; a storage taken out reads as none after,
 * leaving the others; edits gathered out of key order are all applied; and
 * a save with nothing gathered writes nothing.
 */
void
test_repo_tests_a_saved_base_reads_back_with_its_chunks_and_a_storage_taken_out_is_gone(void)
{
	repo_tests_repo_frame_0_t frame = {0};

	std_landing(&frame, repo_tests_saved_base_body_0, repo_tests_repo_pad_0);
}

/* repo_tests_failed returns a repository error as host's XX000 error with its text, the actors' failed. */
static HostError
repo_tests_failed(ChunksErrorError *error)
{
	StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
	StdFmtArg	args[] = {{error, chunks_error_Error_Display_fmt}};
	StdFmtArguments arguments = {pieces, 1, args};
	StdString	message;

	std_fmt_format_Checked(&message, &arguments);
	chunks_error_Error_drop(error);
	return host_Error_fork((const uint8 *) "XX000", &message);
}

/* repo_tests_saving_t is a saving actor's captures: its repository, its field for each call, and the count of its gathers. */
typedef struct repo_tests_saving_t
{
	uint32		repository;
	uint64		(*field) (uint64 calls);
	StdAtomicU64 *gathered;
} repo_tests_saving_t;

/* repo_tests_saving_gather_0 is the actor's gather: one base whose field its count of calls gives. */
static bool
repo_tests_saving_gather_0(void *captures, HostIoIo *io, const RepoRead *read, StdVec *result,
						   ChunksErrorError *error)
{
	const repo_tests_saving_t *saving = captures;
	uint64		calls = std_AtomicU64_fetch_add_relaxed(saving->gathered, 1);
	repo_tests_base_t base = {1, saving->field(calls), 1};

	return repo_tests_bases_gather_0(&base, io, read, result, error);
}

/* repo_tests_saving_0 is a saving actor's body: it opens the repository and saves one base. */
static bool
repo_tests_saving_0(void *captures, HostIoIo *io, HostError *error)
{
	const repo_tests_saving_t *saving = captures;
	RepoRepo	repo;
	HostSharedReader reading;
	ChunksErrorError failure;
	bool		saved;

	if (!repo_Repo_open(io, saving->repository, HOST_PG_DEFAULT, &repo, &failure))
	{
		*error = repo_tests_failed(&failure);
		return false;
	}
	if (!host_shared_read(io, &reading, error))
	{
		repo_Repo_drop(&repo);
		return false;
	}
	saved = repo_Repo_save(&repo, io, &reading, false, NULL, repo_tests_saving_gather_0, (void *) saving,
						   repo_tests_ok_saved_0, NULL, &failure);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	if (!saved)
		*error = repo_tests_failed(&failure);
	return saved;
}

/* repo_tests_older_field is the older saver's field: 1 at its first gather, then 3. */
static uint64
repo_tests_older_field(uint64 calls)
{
	return calls == 0 ? 1 : 3;
}

/* repo_tests_newer_field is the newer saver's field: always 2. */
static uint64
repo_tests_newer_field(uint64 calls)
{
	return 2;
}

/* repo_tests_actor returns a saving actor over a copy of its captures, which the run takes. */
static SimcActor
repo_tests_actor(const repo_tests_saving_t *saving)
{
	repo_tests_saving_t *captures = std_alloc(sizeof(*captures), _Alignof(repo_tests_saving_t));
	SimcActor	actor = {repo_tests_saving_0, captures, NULL, sizeof(*captures), _Alignof(repo_tests_saving_t)};

	*captures = *saving;
	return actor;
}

/* repo_tests_all_returned reports whether a run ended with every actor returning Ok. */
static bool
repo_tests_all_returned(const SimcReport *report)
{
	if (report->outcome.tag != SIMC_OUTCOME_ENDED)
		return false;
	for (size_t k = 0, k_len = report->outcome.u.ended.len; k < k_len; k++)
	{
		const SimcEnding *ending = &STD_VEC_AT(&report->outcome.u.ended, simc_TupleStringEnding_t, k).f1;

		if (ending->tag != SIMC_ENDING_RETURNED || ending->is_err)
			return false;
	}
	return true;
}

/* repo_tests_refused_save_body_0 is the refused save test's body. */
static void
repo_tests_refused_save_body_0(void *frame_)
{
	repo_tests_repo_frame_0_t *frame = frame_;
	HostIoIo   *io;
	HostError	host_error;
	StdAtomicU64 gathered = STD_ATOMIC_U64_INIT(0);
	StdAtomicU64 others = STD_ATOMIC_U64_INIT(0);
	repo_tests_saving_t older = {9106, repo_tests_older_field, &gathered};
	repo_tests_saving_t newer = {9106, repo_tests_newer_field, &others};
	SimcStep	steps[] = {{"newer", 5, PG_UINT64_MAX}, {"older", 5, PG_UINT64_MAX}};
	SimcRun		run;
	SimcReport	report;
	RepoBaseRecord record;
	StdVec		registered;
	StdVec		numbers;
	const RepoManifest *manifest;
	uint64		newest;
	uint64		previous;
	size_t		unnamed = 0;
	uint64		count;
	uint64		want;

	frame->test_io = simc_io();
	frame->test_io_live = true;
	io = &frame->test_io.io;
	repo_tests_unwrap(frame, repo_Repo_create(io, 9106, HOST_PG_DEFAULT, &frame->repo, &frame->error), 770, 59);
	frame->repo_live = false;
	repo_Repo_drop(&frame->repo);
	run = simc_Run_new(1);
	simc_Run_actor(&run, "older", 5, repo_tests_actor(&older));
	simc_Run_actor(&run, "newer", 5, repo_tests_actor(&newer));
	simc_Run_until(&run, "older", 5, "doltgres-repo-save-written");
	simc_Run_script(&run, steps, lengthof(steps));
	report = simc_Run_finish(run);
	if (!repo_tests_all_returned(&report))
	{
		StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&report.outcome, simc_Outcome_Debug_fmt}};
		StdFmtArguments arguments = {pieces, 1, args};

		std_panic_fmt(RUST_FILE, 790, 9, &arguments);
	}
	simc_Report_drop(&report);
	repo_tests_unwrap(frame, repo_Repo_open(io, 9106, HOST_PG_DEFAULT, &frame->repo, &frame->error), 797, 63);
	frame->repo_live = true;
	if (!host_shared_read(io, &frame->reading, &host_error))
	{
		frame->error = chunks_error_Error_FromError_from(&host_error);
		repo_tests_unwrap(frame, false, 798, 46);
	}
	frame->reading_live = true;
	if (!repo_tests_base_of(frame, 1, &record, 800, 61))
		STD_OPTION_UNWRAP_FAILED(800, 70);
	count = repo_tests_field(frame, &record);
	want = 3;
	STD_ASSERT_EQ(800, 9, count == 3, &count, std_u64_Debug_fmt, &want, std_u64_Debug_fmt);
	count = std_AtomicU64_load_relaxed(&gathered);
	want = 2;
	STD_ASSERT_EQ(801, 9, count == 2, &count, std_u64_Debug_fmt, &want, std_u64_Debug_fmt);
	repo_tests_unwrap(frame, chunks_packs_roots_Packs_registered(&frame->repo.packs, io, &registered, &frame->error),
					  802, 43);
	count = registered.len;
	std_vec_free_Checked(&registered, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
	STD_ASSERT(802, 9, count == 0, "repo.packs.registered(io).unwrap().is_empty()");
	manifest = repo_tests_manifest(frame, &frame->repo, 803, 40);
	if (!manifest->has_bases)
		STD_OPTION_UNWRAP_FAILED(803, 82);
	newest = manifest->bases.locator.pack;
	{
		bool		has;

		repo_tests_unwrap(frame, repo_Repo_previous(&frame->repo, io, &has, &frame->previous, &frame->error),
						  804, 42);
		if (!has)
			STD_OPTION_UNWRAP_FAILED(804, 51);
		frame->previous_live = true;
		if (!frame->previous.has_bases)
			STD_OPTION_UNWRAP_FAILED(804, 93);
		previous = frame->previous.bases.locator.pack;
	}
	repo_tests_unwrap(frame, chunks_packs_backend_Packs_numbers(&frame->repo.packs, io, &frame->reading, &numbers,
																&frame->error),
					  806, 46);
	for (size_t k = 0, k_len = numbers.len; k < k_len; k++)
	{
		uint64		n = STD_VEC_AT(&numbers, uint64, k);

		if (n != newest && n != previous)
			unnamed = std_wrapping_add_usize(unnamed, 1);
	}
	std_vec_free_Checked(&numbers, sizeof(uint64), _Alignof(uint64));
	{
		size_t		want_len = 1;

		STD_ASSERT_EQ(807, 9, unnamed == 1, &unnamed, std_usize_Debug_fmt, &want_len,
					  std_usize_Debug_fmt);
	}
	repo_tests_repo_pad_0(frame);
}

/*
 * test_repo_tests_a_save_refused_by_another_starts_over_and_its_first_pack_is_named_by_no_manifest:
 * of two saves of one repository, the one whose read is older is refused
 * when the other's write lands between its read and its write: it starts
 * over from the newer manifest, gathers again, and the tree holds the newer
 * record; its first pack is named by neither generation of the manifest, and
 * no root stays registered.
 */
void
test_repo_tests_a_save_refused_by_another_starts_over_and_its_first_pack_is_named_by_no_manifest(void)
{
	repo_tests_repo_frame_0_t frame = {0};

	std_landing(&frame, repo_tests_refused_save_body_0, repo_tests_repo_pad_0);
}

/* repo_tests_pair_t is one pair a test's gather gives: a key of one byte and its value of one byte, or none. */
typedef struct repo_tests_pair_t
{
	uint8		key;
	bool		has_value;
	uint8		value;
} repo_tests_pair_t;

/* repo_tests_pairs_t is a test's gather of pairs. */
typedef struct repo_tests_pairs_t
{
	size_t		len;
	repo_tests_pair_t pairs[2];
} repo_tests_pairs_t;

/* repo_tests_pairs_gather_0 is a test's gather of its pairs, in order. */
static bool
repo_tests_pairs_gather_0(void *captures, HostIoIo *io, const RepoRead *read, StdVec *result,
						  ChunksErrorError *error)
{
	const repo_tests_pairs_t *pairs = captures;

	std_vec_with_capacity_Checked(result, pairs->len, sizeof(RepoGathered), _Alignof(RepoGathered));
	for (size_t k = 0, k_len = pairs->len; k < k_len; k++)
	{
		RepoGathered made = {.tag = REPO_GATHERED_PAIR};

		std_slice_to_vec_Checked(&made.u.pair.key, &pairs->pairs[k].key, 1, 1, 1);
		made.u.pair.has_value = pairs->pairs[k].has_value;
		if (made.u.pair.has_value)
			std_slice_to_vec_Checked(&made.u.pair.value, &pairs->pairs[k].value, 1, 1, 1);
		std_vec_push_Checked(result, &made, sizeof(made), _Alignof(RepoGathered));
	}
	return true;
}

/* repo_tests_pairs_save saves the test's pairs and returns the identity tree the manifest names, if any. */
static bool
repo_tests_pairs_save(repo_tests_repo_frame_0_t *frame, const repo_tests_pairs_t *pairs,
					  chunks_TupleAddressLocator_t *result)
{
	const RepoManifest *manifest;

	repo_tests_unwrap(frame, repo_Repo_save(&frame->repo, &frame->test_io.io, &frame->reading, false, NULL,
											repo_tests_pairs_gather_0, (void *) pairs, repo_tests_ok_saved_0, NULL,
											&frame->error),
					  1005, 63);
	manifest = repo_tests_manifest(frame, &frame->repo, 1006, 31);
	if (manifest->has_identity)
		trees_published_Published_parts(&manifest->identity, &result->f0, &result->f1);
	return manifest->has_identity;
}

/* repo_tests_identity_is asserts the value a key of one byte holds in the identity tree, or none. */
static void
repo_tests_identity_is(repo_tests_repo_frame_0_t *frame, uint8 key, bool has_want, uint8 want, uint32 unwrap_line,
					   uint32 unwrap_col, uint32 line, uint32 col)
{
	StdVec		value;
	bool		has;
	StdBytes	got_bytes;
	StdBytes	want_bytes = {&want, 1};
	StdOptionRef got_ref;
	StdOptionRef want_ref = {has_want, {&want_bytes, std_bytes_Debug_fmt}};
	bool		same;

	repo_tests_unwrap(frame, repo_Repo_identity(&frame->repo, &frame->test_io.io, &frame->reading, &key, 1, &has,
												&value, &frame->error),
					  unwrap_line, unwrap_col);
	got_ref.has = has;
	got_bytes.ptr = has ? value.ptr : NULL;
	got_bytes.len = has ? value.len : 0;
	got_ref.some.value = &got_bytes;
	got_ref.some.fmt = std_bytes_Debug_fmt;
	same = has == has_want && (!has || (value.len == 1 && ((uint8 *) value.ptr)[0] == want));
	if (!same)
		std_assert_failed(RUST_FILE, line, col, "==", &got_ref, std_option_Debug_fmt, &want_ref, std_option_Debug_fmt,
						  NULL);
	if (has)
		std_vec_free_Checked(&value, 1, 1);
}

/* repo_tests_pairs_body_0 is the identity tree test's body. */
static void
repo_tests_pairs_body_0(void *frame_)
{
	repo_tests_repo_frame_0_t *frame = frame_;
	HostIoIo   *io;
	HostError	host_error;
	repo_tests_pairs_t pairs = {2, {{'a', true, '1'}, {'b', true, '2'}}};
	chunks_TupleAddressLocator_t first;
	chunks_TupleAddressLocator_t again;
	chunks_TupleAddressLocator_t second;
	bool		has;
	StdOptionRef got_ref;
	StdOptionRef want_ref;
	StdVec		bytes;
	RepoManifest read;
	const RepoManifest *written;

	frame->test_io = simc_io();
	frame->test_io_live = true;
	io = &frame->test_io.io;
	simc_checkpointed(12);
	repo_tests_unwrap(frame, repo_Repo_create(io, 9109, HOST_PG_DEFAULT, &frame->repo, &frame->error), 999, 65);
	frame->repo_live = true;
	if (!host_shared_read(io, &frame->reading, &host_error))
	{
		frame->error = chunks_error_Error_FromError_from(&host_error);
		repo_tests_unwrap(frame, false, 1000, 46);
	}
	frame->reading_live = true;
	if (!repo_tests_pairs_save(frame, &pairs, &first))
		STD_OPTION_UNWRAP_FAILED(1008, 76);
	repo_tests_identity_is(frame, 'a', true, '1', 1009, 54, 1009, 9);
	repo_tests_identity_is(frame, 'c', false, 0, 1010, 54, 1010, 9);
	pairs.pairs[1].key = 'c';
	pairs.pairs[1].has_value = false;
	has = repo_tests_pairs_save(frame, &pairs, &again);
	got_ref.has = has;
	got_ref.some.value = &again;
	got_ref.some.fmt = repo_tests_pair_Debug_fmt;
	want_ref.has = true;
	want_ref.some.value = &first;
	want_ref.some.fmt = repo_tests_pair_Debug_fmt;
	STD_ASSERT_EQ(1011, 9,
				  has && chunks_address_Address_PartialEq_eq(&again.f0, &first.f0) &&
				  chunks_trailer_Locator_PartialEq_eq(&again.f1, &first.f1),
				  &got_ref, std_option_Debug_fmt, &want_ref, std_option_Debug_fmt);
	pairs.len = 1;
	pairs.pairs[0].has_value = false;
	if (!repo_tests_pairs_save(frame, &pairs, &second))
		STD_OPTION_UNWRAP_FAILED(1012, 51);
	if (chunks_address_Address_PartialEq_eq(&second.f0, &first.f0) &&
		chunks_trailer_Locator_PartialEq_eq(&second.f1, &first.f1))
		std_assert_failed(RUST_FILE, 1013, 9, "!=", &second, repo_tests_pair_Debug_fmt, &first,
						  repo_tests_pair_Debug_fmt, NULL);
	repo_tests_identity_is(frame, 'a', false, 0, 1014, 54, 1014, 9);
	repo_tests_identity_is(frame, 'b', true, '2', 1015, 54, 1015, 9);
	written = repo_tests_manifest(frame, &frame->repo, 1016, 41);
	repo_Manifest_encode(written, &bytes);
	if (!repo_Manifest_decode(bytes.ptr, bytes.len, repo_Repo_id(&frame->repo), &read, &frame->error))
	{
		std_vec_free_Checked(&bytes, 1, 1);
		repo_tests_unwrap(frame, false, 1017, 67);
	}
	std_vec_free_Checked(&bytes, 1, 1);
	has = read.has_identity;
	if (has)
		trees_published_Published_parts(&read.identity, &again.f0, &again.f1);
	repo_Manifest_drop(&read);
	got_ref.has = has;
	want_ref.some.value = &second;
	STD_ASSERT_EQ(1018, 9,
				  has && chunks_address_Address_PartialEq_eq(&again.f0, &second.f0) &&
				  chunks_trailer_Locator_PartialEq_eq(&again.f1, &second.f1),
				  &got_ref, std_option_Debug_fmt, &want_ref, std_option_Debug_fmt);
	repo_tests_repo_pad_0(frame);
}

/*
 * test_repo_tests_a_saves_pairs_fold_into_the_identity_tree: a save's pairs
 * fold into the identity tree, which the manifest names and a read finds by
 * key; a save whose pairs change nothing the tree holds names the same tree;
 * a removal takes a key out; and a manifest's identity tree reads back as
 * written.
 */
void
test_repo_tests_a_saves_pairs_fold_into_the_identity_tree(void)
{
	repo_tests_repo_frame_0_t frame = {0};

	std_landing(&frame, repo_tests_pairs_body_0, repo_tests_repo_pad_0);
}

/* repo_tests_runs_saving_t is a runs saving actor's captures: its repository, the relation it gathers if any, and the runs it is given if any. */
typedef struct repo_tests_runs_saving_t
{
	uint32		repository;
	bool		has_relation;
	repo_tests_base_t base;
	bool		has_given;
	RepoRuns	given;
} repo_tests_runs_saving_t;

/* repo_tests_runs_saving_gather_0 is the actor's gather: its relation's base, or nothing. */
static bool
repo_tests_runs_saving_gather_0(void *captures, HostIoIo *io, const RepoRead *read, StdVec *result,
								ChunksErrorError *error)
{
	const repo_tests_runs_saving_t *saving = captures;

	if (!saving->has_relation)
		return repo_tests_nothing_gather_0(NULL, io, read, result, error);
	return repo_tests_bases_gather_0((void *) &saving->base, io, read, result, error);
}

/* repo_tests_runs_saving_drop is the actor's captures' drop glue: the runs it was given and didn't save. */
static void
repo_tests_runs_saving_drop(void *captures)
{
	repo_tests_runs_saving_t *saving = captures;

	if (saving->has_given)
	{
		saving->has_given = false;
		repo_Runs_drop(&saving->given);
	}
}

/* repo_tests_runs_saving_0 is the actor's body: it opens the repository and saves with the runs it was given. */
static bool
repo_tests_runs_saving_0(void *captures, HostIoIo *io, HostError *error)
{
	repo_tests_runs_saving_t *saving = captures;
	RepoRepo	repo;
	HostSharedReader reading;
	ChunksErrorError failure;
	bool		given = saving->has_given;
	bool		saved;

	if (!repo_Repo_open(io, saving->repository, HOST_PG_DEFAULT, &repo, &failure))
	{
		*error = repo_tests_failed(&failure);
		return false;
	}
	if (!host_shared_read(io, &reading, error))
	{
		repo_Repo_drop(&repo);
		return false;
	}
	saving->has_given = false;
	saved = repo_Repo_save(&repo, io, &reading, given, &saving->given, repo_tests_runs_saving_gather_0, saving,
						   repo_tests_ok_saved_0, NULL, &failure);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	if (!saved)
		*error = repo_tests_failed(&failure);
	return saved;
}

/* repo_tests_runs_actor returns a runs saving actor over a copy of its captures, which the run takes with its runs. */
static SimcActor
repo_tests_runs_actor(uint32 repository, bool has_relation, uint32 relation, bool has_given)
{
	repo_tests_runs_saving_t *captures = std_alloc(sizeof(*captures), _Alignof(repo_tests_runs_saving_t));
	SimcActor	actor = {repo_tests_runs_saving_0, captures, repo_tests_runs_saving_drop, sizeof(*captures),
	_Alignof(repo_tests_runs_saving_t)};

	captures->repository = repository;
	captures->has_relation = has_relation;
	captures->base.relation = relation;
	captures->base.field = 1;
	captures->base.folded = 1;
	captures->has_given = has_given;
	if (has_given)
		repo_tests_runs(30, &captures->given);
	return actor;
}

/* repo_tests_own_runs_body_0 is the refused save's runs test's body. */
static void
repo_tests_own_runs_body_0(void *frame_)
{
	repo_tests_repo_frame_0_t *frame = frame_;
	HostIoIo   *io;
	HostError	host_error;
	SimcStep	steps[] = {{"newer", 5, PG_UINT64_MAX}, {"older", 5, PG_UINT64_MAX}};

	frame->test_io = simc_io();
	frame->test_io_live = true;
	io = &frame->test_io.io;
	for (size_t k = 0, k_len = 2; k < k_len; k++)
	{
		uint32		repository = k == 0 ? 9107 : 9108;
		bool		older_given = k == 0;
		RepoRuns	runs;
		SimcRun		run;
		SimcReport	report;
		RepoBaseRecord record;
		const RepoManifest *manifest;
		StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(": "), STD_FMT_ARG(1)};
		StdFmtArg	args[] = {{&repository, std_u32_Display_fmt}, {NULL, simc_Outcome_Debug_fmt}};
		StdFmtArguments arguments = {pieces, 3, args};
		StdFmtPiece named[] = {STD_FMT_ARG(0)};
		StdFmtArguments repository_only = {named, 1, args};
		bool		same;

		repo_tests_unwrap(frame, repo_Repo_create(io, repository, HOST_PG_DEFAULT, &frame->repo, &frame->error),
						  963, 75);
		frame->repo_live = true;
		if (!host_shared_read(io, &frame->reading, &host_error))
		{
			frame->error = chunks_error_Error_FromError_from(&host_error);
			repo_tests_unwrap(frame, false, 965, 54);
		}
		frame->reading_live = true;
		repo_tests_runs(20, &runs);
		repo_tests_unwrap(frame, repo_Repo_save(&frame->repo, io, &frame->reading, true, &runs,
												repo_tests_nothing_gather_0, NULL, repo_tests_ok_saved_0, NULL,
												&frame->error),
						  966, 92);
		frame->reading_live = false;
		host_shared_Reader_drop(&frame->reading);
		frame->repo_live = false;
		repo_Repo_drop(&frame->repo);
		run = simc_Run_new(1);
		simc_Run_actor(&run, "older", 5, repo_tests_runs_actor(repository, !older_given, 1, older_given));
		simc_Run_actor(&run, "newer", 5, repo_tests_runs_actor(repository, older_given, 2, !older_given));
		simc_Run_until(&run, "older", 5, "doltgres-repo-save-written");
		simc_Run_script(&run, steps, lengthof(steps));
		report = simc_Run_finish(run);
		if (!repo_tests_all_returned(&report))
		{
			args[1].value = &report.outcome;
			std_panic_fmt(RUST_FILE, 975, 13, &arguments);
		}
		simc_Report_drop(&report);
		repo_tests_unwrap(frame, repo_Repo_open(io, repository, HOST_PG_DEFAULT, &frame->repo, &frame->error),
						  982, 73);
		frame->repo_live = true;
		manifest = repo_tests_manifest(frame, &frame->repo, 983, 42);
		repo_tests_runs(30, &runs);
		same = manifest->has_runs && repo_Runs_PartialEq_eq(&manifest->runs, &runs);
		if (!same)
		{
			RepoManifest want = {.has_runs = true, .runs = runs};

			std_assert_failed(RUST_FILE, 983, 13, "==", manifest, repo_tests_option_runs_Debug_fmt, &want,
							  repo_tests_option_runs_Debug_fmt, &repository_only);
		}
		repo_Runs_drop(&runs);
		if (!host_shared_read(io, &frame->reading, &host_error))
		{
			frame->error = chunks_error_Error_FromError_from(&host_error);
			repo_tests_unwrap(frame, false, 984, 50);
		}
		frame->reading_live = true;
		same = repo_tests_base_of(frame, repository == 9107 ? 2 : 1, &record, 987, 55);
		if (same)
			repo_BaseRecord_drop(&record);
		if (!same)
			std_panic_fmt(RUST_FILE, 987, 13, &repository_only);
		frame->reading_live = false;
		host_shared_Reader_drop(&frame->reading);
		frame->manifest_live = false;
		repo_ArcManifest_drop(frame->manifest);
		frame->repo_live = false;
		repo_Repo_drop(&frame->repo);
	}
	repo_tests_repo_pad_0(frame);
}

/*
 * test_repo_tests_a_refused_save_starts_over_with_its_own_runs_or_the_ones_it_reads_again:
 * a save refused by another's write starts over with the runs it was given
 * and none it read: a checkpoint's save refused by a save between two
 * checkpoints writes its own runs beside that save's base, and a save given
 * no runs refused by a checkpoint's save writes the runs that checkpoint
 * named, not the ones it read before.
 */
void
test_repo_tests_a_refused_save_starts_over_with_its_own_runs_or_the_ones_it_reads_again(void)
{
	repo_tests_repo_frame_0_t frame = {0};

	std_landing(&frame, repo_tests_own_runs_body_0, repo_tests_repo_pad_0);
}

/* repo_tests_crash_t is a crash test actor's captures: its repository and the step the crash was at. */
typedef struct repo_tests_crash_t
{
	uint32		repository;
	uint64		step;
} repo_tests_crash_t;

/* repo_tests_crash_actor returns an actor over a copy of a crash test's captures, which the run takes. */
static SimcActor
repo_tests_crash_actor(bool (*body) (void *captures, HostIoIo *io, HostError *error), uint32 repository,
					   uint64 step)
{
	repo_tests_crash_t *captures = std_alloc(sizeof(*captures), _Alignof(repo_tests_crash_t));
	SimcActor	actor = {body, captures, NULL, sizeof(*captures), _Alignof(repo_tests_crash_t)};

	captures->repository = repository;
	captures->step = step;
	return actor;
}

/* repo_tests_saver_0 is the crash test's saver: it opens the repository and saves one base. */
static bool
repo_tests_saver_0(void *captures, HostIoIo *io, HostError *error)
{
	const repo_tests_crash_t *crash = captures;
	repo_tests_base_t base = {1, 1, 5};
	RepoRepo	repo;
	HostSharedReader reading;
	ChunksErrorError failure;
	bool		saved;

	if (!repo_Repo_open(io, crash->repository, HOST_PG_DEFAULT, &repo, &failure))
	{
		*error = repo_tests_failed(&failure);
		return false;
	}
	if (!host_shared_read(io, &reading, error))
	{
		repo_Repo_drop(&repo);
		return false;
	}
	saved = repo_Repo_save(&repo, io, &reading, false, NULL, repo_tests_bases_gather_0, &base, repo_tests_ok_saved_0,
						   NULL, &failure);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	if (!saved)
		*error = repo_tests_failed(&failure);
	return saved;
}

/* repo_tests_reader_0 is the crash test's reader: the saved base must read back after the crash. */
static bool
repo_tests_reader_0(void *captures, HostIoIo *io, HostError *error)
{
	const repo_tests_crash_t *crash = captures;
	RepoStorage storage = {HOST_PG_DEFAULT, 1};
	RepoRepo	repo;
	HostSharedReader reading;
	ChunksErrorError failure;
	RepoBaseRecord record;
	bool		has;
	bool		kept = false;

	if (!repo_Repo_open(io, crash->repository, HOST_PG_DEFAULT, &repo, &failure))
	{
		*error = repo_tests_failed(&failure);
		return false;
	}
	if (!host_shared_read(io, &reading, error))
	{
		repo_Repo_drop(&repo);
		return false;
	}
	if (!repo_Repo_base(&repo, io, &reading, &storage, &has, &record, &failure))
	{
		host_shared_Reader_drop(&reading);
		repo_Repo_drop(&repo);
		*error = repo_tests_failed(&failure);
		return false;
	}
	if (has)
	{
		ChunksRecordRecord read;
		uint64		field;

		kept = chunks_record_Record_value(record.bytes.ptr, record.bytes.len, record.addresses.ptr,
										  record.addresses.len, &read, &failure) &&
			chunks_record_Record_uint(&read, 1, &field, &failure) && field == 1;
		repo_BaseRecord_drop(&record);
	}
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	if (!kept)
	{
		StdFmtPiece pieces[] = {STD_FMT_LIT("the save's base is gone after a crash at "), STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&crash->step, std_u64_Display_fmt}};
		StdFmtArguments arguments = {pieces, 2, args};

		failure.tag = CHUNKS_ERROR_ERROR_INVARIANT;
		std_fmt_format_Checked(&failure.u.invariant, &arguments);
		*error = repo_tests_failed(&failure);
	}
	return kept;
}

/* repo_tests_crash_body_0 is the crash test's body. */
static void
repo_tests_crash_body_0(void *frame_)
{
	repo_tests_repo_frame_0_t *frame = frame_;
	HostIoIo   *io;
	uint64		crashes = 0;
	uint64		saved = 0;

	frame->test_io = simc_io();
	frame->test_io_live = true;
	io = &frame->test_io.io;
	for (size_t k = 0, k_len = 199; k < k_len; k++)
	{
		uint64		step = std_wrapping_add_u64((uint64) k, 1);
		uint32		repository = (uint32) std_wrapping_add_u64(9200, step);
		StdPathBuf	root = simc_data_dir();
		SimcSim		sim;
		SimcRun		run;
		SimcReport	report;
		bool		crashed;
		StdPathBuf	dir;
		StdPathBuf	path;
		bool		has;
		StdVec		bytes;
		bool		has_fallback;
		ChunksFileFallback fallback;
		RepoManifest manifest;

		repo_tests_unwrap(frame, repo_Repo_create(io, repository, HOST_PG_DEFAULT, &frame->repo, &frame->error),
						  821, 65);
		repo_Repo_drop(&frame->repo);
		sim = simc_Sim_start(root.bytes.ptr, root.bytes.len, 31);
		std_vec_free_Checked(&root.bytes, 1, 1);
		simc_Sim_crash_at(&sim, std_wrapping_add_u64(simc_Sim_calls(&sim), step));
		run = simc_Run_new(1);
		simc_Run_actor(&run, "saver", 5, repo_tests_crash_actor(repo_tests_saver_0, repository, step));
		report = simc_Run_finish(run);
		crashed = report.outcome.tag == SIMC_OUTCOME_CRASHED;
		simc_Report_drop(&report);
		if (crashed)
		{
			StdIoError	io_error;

			if (!simc_Sim_recover(&sim, true, &io_error))
				STD_RESULT_EXPECT_FAILED(832, 35, "called `Result::unwrap()` on an `Err` value", &io_error,
										 std_io_Error_Debug_fmt);
			crashes = std_wrapping_add_u64(crashes, 1);
		}
		simc_Sim_stop(&sim);
		simc_Sim_drop(&sim);
		dir = host_tablespace_dir(repository, HOST_PG_DEFAULT);
		std_path_join(dir.bytes.ptr, dir.bytes.len, CHUNKS_PACKS_MANIFEST, strlen(CHUNKS_PACKS_MANIFEST), &path);
		std_vec_free_Checked(&dir.bytes, 1, 1);
		repo_tests_unwrap(frame, chunks_file_read_kept(HOST_IO_IO_READ_DOORS(io), path.bytes.ptr, path.bytes.len, &has,
													   &bytes, &has_fallback, &fallback, &frame->error),
						  837, 65);
		std_vec_free_Checked(&path.bytes, 1, 1);
		if (!has)
			std_option_expect_failed("a manifest after the crash", strlen("a manifest after the crash"), RUST_FILE,
									 837, 74);
		if (!repo_Manifest_decode(bytes.ptr, bytes.len, trees_published_RepoId_unique(), &manifest, &frame->error))
		{
			std_vec_free_Checked(&bytes, 1, 1);
			repo_tests_unwrap(frame, false, 838, 71);
		}
		std_vec_free_Checked(&bytes, 1, 1);
		if (manifest.has_bases)
		{
			saved = std_wrapping_add_u64(saved, 1);
			run = simc_Run_new(1);
			simc_Run_actor(&run, "reader", 6, repo_tests_crash_actor(repo_tests_reader_0, repository, step));
			report = simc_Run_finish(run);
			if (!repo_tests_all_returned(&report))
			{
				StdFmtPiece pieces[] = {STD_FMT_LIT("after a crash at step "), STD_FMT_ARG(0), STD_FMT_LIT(": "),
				STD_FMT_ARG(1)};
				StdFmtArg	args[] = {{&step, std_u64_Display_fmt}, {&report.outcome, simc_Outcome_Debug_fmt}};
				StdFmtArguments arguments = {pieces, 4, args};

				std_panic_fmt(RUST_FILE, 852, 17, &arguments);
			}
			simc_Report_drop(&report);
		}
		repo_Manifest_drop(&manifest);
		if (!crashed)
			break;
	}
	if (!(crashes > 0 && saved > 0))
	{
		StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(" crashes, "), STD_FMT_ARG(1), STD_FMT_LIT(" saves kept")};
		StdFmtArg	args[] = {{&crashes, std_u64_Display_fmt}, {&saved, std_u64_Display_fmt}};
		StdFmtArguments arguments = {pieces, 4, args};

		std_panic_fmt(RUST_FILE, 863, 9, &arguments);
	}
	repo_tests_repo_pad_0(frame);
}

/*
 * test_repo_tests_a_save_cut_short_by_a_crash_at_any_step_leaves_one_whole_manifest:
 * a crash at any step of a save leaves a manifest that reads as one whole
 * generation, the one before the save or the one it wrote; a manifest that
 * names the save's tree, read back after the crash, gives the saved base.
 */
void
test_repo_tests_a_save_cut_short_by_a_crash_at_any_step_leaves_one_whole_manifest(void)
{
	repo_tests_repo_frame_0_t frame = {0};

	std_landing(&frame, repo_tests_crash_body_0, repo_tests_repo_pad_0);
}

#endif							/* DOLTGRES_TEST_REPO */
