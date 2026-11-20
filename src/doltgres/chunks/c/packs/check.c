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
 * The check of a repository's packs over a stopped server's files, changing
 * nothing: its list files read as a backend reads them, each listed pack's
 * records against their addresses, each run's entries against the packs, and
 * the files a crash leaves, with a view of the packs built from what it
 * read, through which the check of the repository's trees reads every chunk
 * as a backend's read does.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/index.h"
#include "doltgres/chunks/pack.h"
#include "doltgres/chunks/packs.h"
#include "doltgres/chunks/packs/check.h"
#include "doltgres/chunks/packs/dropped.h"
#include "doltgres/chunks/packs/merge.h"
#include "doltgres/chunks/packs/open.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/path.h"
#include "doltgres/std/slice.h"
#include "doltgres/std/str.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"

#ifdef DOLTGRES_TEST_CHUNKS
#include "doltgres/chunks/packs/backend.h"
#include "doltgres/chunks/packs/fixtures.h"
#include "doltgres/simc/simc.h"
#include "doltgres/std/io.h"
#include "doltgres/std/num.h"
#include "doltgres/std/test.h"
#include "doltgres/std/test_fs.h"
#endif

#define RUST_FILE "chunks" STD_SEP "src" STD_SEP "packs" STD_SEP "check.rs"

/* chunks_packs_check_chunk_t is a record a pack holds that hashes to its address: its offset and its address. */
typedef struct chunks_packs_check_chunk_t
{
	uint64		offset;
	ChunksAddressAddress address;
} chunks_packs_check_chunk_t;

/* chunks_packs_check_held_t is a listed pack's records that hash to their addresses, by offset, under its number. */
typedef struct chunks_packs_check_held_t
{
	uint64		number;

	/*
	 * chunks is a BTreeMap<u64, Address>: chunks_packs_check_chunk_t in
	 * offset order.
	 */
	StdVec		chunks;
} chunks_packs_check_held_t;

/* chunks_packs_check_held_drop drops one pack's records of what packs read. */
static void
chunks_packs_check_held_drop(void *held)
{
	std_vec_free_Checked(&((chunks_packs_check_held_t *) held)->chunks, sizeof(chunks_packs_check_chunk_t),
						 _Alignof(chunks_packs_check_chunk_t));
}

/* chunks_packs_check_Held_drop drops what packs read, a BTreeMap<u64, BTreeMap<u64, Address>>. */
static void
chunks_packs_check_Held_drop(StdVec *held)
{
	std_vec_drop_Checked(held, sizeof(chunks_packs_check_held_t), _Alignof(chunks_packs_check_held_t),
						 chunks_packs_check_held_drop);
}

/* chunks_packs_check_Lines_drop drops a Vec<String>. */
static void
chunks_packs_check_Lines_drop(StdVec *lines)
{
	std_vec_drop_Checked(lines, sizeof(StdString), _Alignof(StdString), std_String_drop);
}

/* chunks_packs_check_push appends a formatted line to one of what a check reports. */
static void
chunks_packs_check_push(StdVec *lines, const StdFmtArguments *arguments)
{
	StdString	line;

	std_fmt_format_Checked(&line, arguments);
	std_vec_push_Checked(lines, &line, sizeof(line), _Alignof(StdString));
}

/* chunks_packs_check_path_said appends a line of a path between two texts. */
static void
chunks_packs_check_path_said(StdVec *lines, const char *before, const StdPathBuf *path, const char *after)
{
	StdStr		first = {before, strlen(before)};
	StdStr		shown = {path->bytes.ptr, path->bytes.len};
	StdStr		last = {after, strlen(after)};
	StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_ARG(1), STD_FMT_ARG(2)};
	StdFmtArg	args[] = {{&first, std_str_Display_fmt}, {&shown, std_path_Display_fmt}, {&last, std_str_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	chunks_packs_check_push(lines, &arguments);
}

/* chunks_packs_check_failed appends a finding of a path between two texts, then the error, which it takes. */
static void
chunks_packs_check_failed(StdVec *found, const char *before, const StdPathBuf *path, const char *after,
						  ChunksErrorError *error)
{
	StdString	finding;
	StdStr		first = {before, strlen(before)};
	StdStr		shown = {path->bytes.ptr, path->bytes.len};
	StdStr		last = {after, strlen(after)};
	StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_ARG(1), STD_FMT_ARG(2), STD_FMT_ARG(3)};
	StdFmtArg	args[] = {{&first, std_str_Display_fmt}, {&shown, std_path_Display_fmt}, {&last, std_str_Display_fmt},
	{&finding, std_String_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	chunks_error_Error_into_finding(error, &finding);
	chunks_packs_check_push(found, &arguments);
	std_String_drop(&finding);
}

/* chunks_packs_check_Checked_fallen_back reports a list file read as its previous generation, or only up to damage. */
static void
chunks_packs_check_Checked_fallen_back(ChunksPacksCheckChecked *self, const StdPathBuf *path,
									   const ChunksFileFallback *why)
{
	switch (why->tag)
	{
		case CHUNKS_FILE_FALLBACK_MISSING:
			chunks_packs_check_path_said(&self->settled, "\"", path,
										 "\" is missing, as a crash during its write leaves it; its previous "
										 "generation was read, which the next start writes whole again");
			return;
		case CHUNKS_FILE_FALLBACK_DAMAGED:
			chunks_packs_check_path_said(&self->found, "\"", path, "\" is damaged; its previous generation was read");
			return;
		case CHUNKS_FILE_FALLBACK_UNCLEAR:
			chunks_packs_check_path_said(&self->found, "\"", path,
										 "\" ends in damage that could not be told from a record a crash cut short; "
										 "the records before it were read");
			return;
		case CHUNKS_FILE_FALLBACK_UNREADABLE:
			chunks_packs_check_path_said(&self->found, "\"", path, "\" could not be read in either generation");
			return;
		case CHUNKS_FILE_FALLBACK_KEPT:
			{
				StdStr		shown = {path->bytes.ptr, path->bytes.len};
				StdFmtPiece pieces[] = {STD_FMT_LIT("\""), STD_FMT_ARG(0), STD_FMT_LIT("\" is damaged at offset "),
					STD_FMT_ARG(1),
				STD_FMT_LIT("; the records before it were read, and only the WAL restores those after it")};
				StdFmtArg	args[] = {{&shown, std_path_Display_fmt}, {&why->u.kept, std_u64_Display_fmt}};
				StdFmtArguments arguments = {pieces, lengthof(pieces), args};

				chunks_packs_check_push(&self->found, &arguments);
				return;
			}
	}
	pg_unreachable();
}

/* chunks_packs_check_list_frame_0_t is list's state across its pad: the file's records while it applies them. */
typedef struct chunks_packs_check_list_frame_0_t
{
	ChunksPacksCheckChecked *self;
	HostIoReadDoors io;
	const ChunksPacksViewListOps *ops;
	const StdPathBuf *path;
	ChunksPacksViewRead *read;
	void	   *list;
	bool		ok;
	ChunksFileAppended appended;
	bool		appended_live;
} chunks_packs_check_list_frame_0_t;

/* chunks_packs_check_list_pad_0 is list's pad: the file's records. */
static void
chunks_packs_check_list_pad_0(void *frame_)
{
	chunks_packs_check_list_frame_0_t *frame = frame_;

	if (frame->appended_live)
	{
		frame->appended_live = false;
		chunks_file_Appended_drop(&frame->appended);
	}
}

/*
 * chunks_packs_check_list_body_0 is list's body: the previous generation's
 * damage, the file's records read as read_appended reads them, and the list
 * they apply to.
 */
static void
chunks_packs_check_list_body_0(void *frame_)
{
	chunks_packs_check_list_frame_0_t *frame = frame_;
	ChunksPacksCheckChecked *self = frame->self;
	const StdPathBuf *path = frame->path;
	ChunksErrorError error;
	bool		damaged;
	bool		has;

	frame->ok = false;
	if (!chunks_file_previous_damaged(frame->io, path->bytes.ptr, path->bytes.len, &damaged, &error))
		chunks_packs_check_failed(&self->found, "could not read the previous generation of \"", path, "\": ", &error);
	else if (damaged)
		chunks_packs_check_path_said(&self->found, "the previous generation of \"", path,
									 "\" is damaged, so a read falling back to it finds nothing");
	if (!chunks_file_read_appended(frame->io, path->bytes.ptr, path->bytes.len, &has, &frame->appended, &error))
	{
		chunks_packs_check_failed(&self->found, "could not read \"", path, "\": ", &error);
		goto done;
	}
	if (!has)
	{
		StdString	finding;
		ChunksErrorError gone = chunks_packs_open_gone(path->bytes.ptr, path->bytes.len);

		chunks_error_Error_into_finding(&gone, &finding);
		std_vec_push_Checked(&self->found, &finding, sizeof(finding), _Alignof(StdString));
		goto done;
	}
	frame->appended_live = true;
	if (frame->appended.kept)
	{
		chunks_packs_check_path_said(&self->found, "\"", path, "\" is not a file of appended records");
		goto done;
	}
	if (frame->appended.has_fallback)
		chunks_packs_check_Checked_fallen_back(self, path, &frame->appended.fallback);
	if (frame->appended.has_damage)
	{
		size_t		after = frame->appended.damage.after.len;
		StdString	lost;
		StdStr		shown = {path->bytes.ptr, path->bytes.len};
		StdFmtPiece pieces[] = {STD_FMT_LIT("\""), STD_FMT_ARG(0), STD_FMT_LIT("\" is damaged at offset "),
		STD_FMT_ARG(1), STD_FMT_LIT(", so "), STD_FMT_ARG(2)};
		StdFmtArg	args[] = {{&shown, std_path_Display_fmt}, {&frame->appended.damage.offset, std_u64_Display_fmt},
		{&lost, std_String_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		if (after == 1)
		{
			static const char one[] = "the 1 whole record after it is lost until the WAL restores it";

			std_string_from_Checked(&lost, one, sizeof(one) - 1);
		}
		else
		{
			StdFmtPiece lost_pieces[] = {STD_FMT_LIT("the "), STD_FMT_ARG(0),
			STD_FMT_LIT(" whole records after it are lost until the WAL restores them")};
			StdFmtArg	lost_args[] = {{&after, std_usize_Display_fmt}};
			StdFmtArguments lost_arguments = {lost_pieces, lengthof(lost_pieces), lost_args};

			std_fmt_format_Checked(&lost, &lost_arguments);
		}
		chunks_packs_check_push(&self->found, &arguments);
		std_String_drop(&lost);
	}
	frame->ops->reset(frame->list);
	for (size_t k = 0, k_len = frame->appended.records.len; k < k_len; k++)
	{
		const StdVec *record = &STD_VEC_AT(&frame->appended.records, StdVec, k);

		if (!frame->ops->apply(frame->list, record->ptr, record->len, &error))
		{
			chunks_packs_check_failed(&self->found, "\"", path, "\" holds a record no writer makes: ", &error);
			goto done;
		}
	}
	frame->read->at = frame->appended.has_end ? frame->appended.end.end : 0;
	frame->read->past = frame->appended.records.len == 0 ? 0 : (uint64) frame->appended.records.len - 1;
	frame->ok = true;
done:
	if (!frame->ok)
		frame->ops->reset(frame->list);
	chunks_packs_check_list_pad_0(frame);
}

/*
 * chunks_packs_check_Checked_list reads a list file as read_appended reads
 * it into a list and its read, reporting a previous generation that is
 * damaged, a newest read as its previous generation or only up to damage,
 * and a file gone with its previous generation, with the settle's own refusal
 * of it, and reports whether it could read its records, leaving the list its
 * Default when it couldn't.
 */
static bool
chunks_packs_check_Checked_list(ChunksPacksCheckChecked *self, HostIoReadDoors io, const ChunksPacksViewListOps *ops,
								const StdPathBuf *path, ChunksPacksViewRead *read, void *list)
{
	chunks_packs_check_list_frame_0_t frame = {.self = self, .io = io, .ops = ops, .path = path, .read = read,
	.list = list};

	std_landing(&frame, chunks_packs_check_list_body_0, chunks_packs_check_list_pad_0);
	return frame.ok;
}

/* chunks_packs_check_listed_t is a pack the pack table lists: its number, its tablespace, and its id. */
typedef struct chunks_packs_check_listed_t
{
	uint64		number;
	uint32		space;
	ChunksPackPackId id;
} chunks_packs_check_listed_t;

/* chunks_packs_check_packs_frame_0_t is packs' state across its pad: the listed packs and the one it reads. */
typedef struct chunks_packs_check_packs_frame_0_t
{
	ChunksPacksCheckChecked *self;
	ChunksFileInterruptible io;
	StdVec	   *result;
	StdVec		listed;
	bool		listed_live;
	StdPathBuf	path;
	bool		path_live;
	ChunksPackPackReader reader;
	bool		reader_live;
	StdVec		chunks;
	bool		chunks_live;
	ChunksPackLost lost;
	bool		lost_live;
} chunks_packs_check_packs_frame_0_t;

/* chunks_packs_check_packs_pad_0 is packs' pad: the pack it reads, then the listed packs. */
static void
chunks_packs_check_packs_pad_0(void *frame_)
{
	chunks_packs_check_packs_frame_0_t *frame = frame_;

	if (frame->lost_live)
	{
		frame->lost_live = false;
		chunks_pack_Lost_drop(&frame->lost);
	}
	if (frame->chunks_live)
	{
		frame->chunks_live = false;
		std_vec_free_Checked(&frame->chunks, sizeof(chunks_packs_check_chunk_t), _Alignof(chunks_packs_check_chunk_t));
	}
	if (frame->reader_live)
	{
		frame->reader_live = false;
		chunks_pack_PackReader_drop(&frame->reader);
	}
	if (frame->path_live)
	{
		frame->path_live = false;
		std_vec_free_Checked(&frame->path.bytes, 1, 1);
	}
	if (frame->listed_live)
	{
		frame->listed_live = false;
		std_vec_free_Checked(&frame->listed, sizeof(chunks_packs_check_listed_t), _Alignof(chunks_packs_check_listed_t));
	}
}

/* chunks_packs_check_chunk_at finds a record by its offset among a pack's, or where it would go. */
static bool
chunks_packs_check_chunk_at(const StdVec *chunks, uint64 offset, size_t *at)
{
	size_t		low = 0;
	size_t		high = chunks->len;

	while (low < high)
	{
		size_t		mid = low + (high - low) / 2;
		uint64		here = STD_VEC_AT(chunks, chunks_packs_check_chunk_t, mid).offset;

		if (here == offset)
		{
			*at = mid;
			return true;
		}
		if (here < offset)
			low = mid + 1;
		else
			high = mid;
	}
	*at = low;
	return false;
}

/* chunks_packs_check_packs_closure_0 is packs' walk: each verified record's address, by its offset. */
static bool
chunks_packs_check_packs_closure_0(void *captures, ChunksFileInterruptible io, ChunksPackVerified *chunk,
								   ChunksErrorError *error)
{
	StdVec	   *chunks = captures;
	ChunksTrailerLocator locator = chunks_pack_Verified_locator(chunk);
	chunks_packs_check_chunk_t entry = {chunks_trailer_Locator_offset(&locator), chunks_pack_Verified_address(chunk)};
	size_t		at;

	(void) io;
	(void) error;
	if (chunks_packs_check_chunk_at(chunks, entry.offset, &at))
		STD_VEC_AT(chunks, chunks_packs_check_chunk_t, at) = entry;
	else
		std_vec_insert(chunks, at, &entry, sizeof(entry), _Alignof(chunks_packs_check_chunk_t), RUST_FILE, 0, 0);
	return true;
}

/* chunks_packs_check_listed_remove takes a pack out of the pack table the view read, by its number. */
static void
chunks_packs_check_listed_remove(StdVec *packs, uint64 number)
{
	for (size_t k = 0, k_len = packs->len; k < k_len; k++)
		if (STD_VEC_AT(packs, chunks_TupleU64Listed_t, k).f0 == number)
		{
			chunks_TupleU64Listed_t removed;

			std_vec_remove(packs, k, &removed, sizeof(removed), _Alignof(chunks_TupleU64Listed_t), RUST_FILE, 0, 0);
			return;
		}
}

/*
 * chunks_packs_check_packs_body_0 is packs' body: each listed pack outside a
 * missing tablespace read and verified, its records kept by offset or its
 * failure found and the pack left out of the view.
 */
static void
chunks_packs_check_packs_body_0(void *frame_)
{
	chunks_packs_check_packs_frame_0_t *frame = frame_;
	ChunksPacksCheckChecked *self = frame->self;
	HostIoReadDoors read = {&frame->io.ops->read_doors, frame->io.self};
	const StdVec *packs = &self->view.table.list.packs;

	std_vec_new(frame->result, sizeof(chunks_packs_check_held_t), _Alignof(chunks_packs_check_held_t));
	std_vec_with_capacity_Checked(&frame->listed, packs->len, sizeof(chunks_packs_check_listed_t),
								  _Alignof(chunks_packs_check_listed_t));
	frame->listed_live = true;
	for (size_t k = 0, k_len = packs->len; k < k_len; k++)
	{
		const chunks_TupleU64Listed_t *pack = &STD_VEC_AT(packs, chunks_TupleU64Listed_t, k);
		chunks_packs_check_listed_t entry = {pack->f0, pack->f1.space, pack->f1.id};

		std_vec_push_Checked(&frame->listed, &entry, sizeof(entry), _Alignof(chunks_packs_check_listed_t));
	}
	for (size_t k = 0, k_len = frame->listed.len; k < k_len; k++)
	{
		const chunks_packs_check_listed_t *pack = &STD_VEC_AT(&frame->listed, chunks_packs_check_listed_t, k);
		StdPathBuf	dir;
		StdString	name;
		ChunksErrorError error;
		bool		ok;

		if (chunks_packs_view_missing_contains(&self->view.missing, pack->space))
			continue;
		name = chunks_pack_PackId_file_name(&pack->id, "pack", 4);
		chunks_packs_view_View_space_dir(&self->view, pack->space, &dir);
		std_path_join(dir.bytes.ptr, dir.bytes.len, name.ptr, name.len, &frame->path);
		frame->path_live = true;
		std_vec_free_Checked(&dir.bytes, 1, 1);
		std_String_drop(&name);
		ok = chunks_pack_PackReader_open(read, frame->path.bytes.ptr, frame->path.bytes.len, &frame->reader, &error);
		if (ok)
		{
			frame->reader_live = true;
			std_vec_new(&frame->chunks, sizeof(chunks_packs_check_chunk_t), _Alignof(chunks_packs_check_chunk_t));
			frame->chunks_live = true;
			ok = chunks_pack_PackReader_verified(&frame->reader, frame->io, pack->number,
												 chunks_packs_check_packs_closure_0, &frame->chunks, NULL,
												 &frame->lost, &error);
			frame->reader_live = false;
			chunks_pack_PackReader_drop(&frame->reader);
		}
		if (ok)
		{
			StdString	message;
			chunks_packs_check_held_t held = {pack->number, frame->chunks};

			frame->lost_live = true;
			if (chunks_pack_Lost_message(&frame->lost, frame->path.bytes.ptr, frame->path.bytes.len, &message))
				std_vec_push_Checked(&self->found, &message, sizeof(message), _Alignof(StdString));
			frame->lost_live = false;
			chunks_pack_Lost_drop(&frame->lost);
			frame->chunks_live = false;
			std_vec_push_Checked(frame->result, &held, sizeof(held), _Alignof(chunks_packs_check_held_t));
		}
		else
		{
			StdString	finding;
			StdStr		shown = {frame->path.bytes.ptr, frame->path.bytes.len};
			StdFmtPiece pieces[] = {STD_FMT_LIT("pack "), STD_FMT_ARG(0), STD_FMT_LIT(" \""), STD_FMT_ARG(1),
			STD_FMT_LIT("\", which the pack table lists, can't be read: "), STD_FMT_ARG(2)};
			StdFmtArg	args[] = {{&pack->number, std_u64_Display_fmt}, {&shown, std_path_Display_fmt},
			{&finding, std_String_Display_fmt}};
			StdFmtArguments arguments = {pieces, lengthof(pieces), args};

			if (frame->chunks_live)
			{
				frame->chunks_live = false;
				std_vec_free_Checked(&frame->chunks, sizeof(chunks_packs_check_chunk_t),
									 _Alignof(chunks_packs_check_chunk_t));
			}
			chunks_error_Error_into_finding(&error, &finding);
			chunks_packs_check_push(&self->found, &arguments);
			std_String_drop(&finding);
			chunks_packs_check_listed_remove(&self->view.table.list.packs, pack->number);
		}
		frame->path_live = false;
		std_vec_free_Checked(&frame->path.bytes, 1, 1);
	}
	chunks_packs_check_packs_pad_0(frame);
}

/*
 * chunks_packs_check_Checked_packs reads each listed pack's records against
 * their addresses, under its number, and returns the address of each record
 * that hashes to its address, by pack and offset. A pack that can't be read
 * is a finding, and the view leaves it out, so its chunks are found missing
 * by what reads them.
 */
static void
chunks_packs_check_Checked_packs(ChunksPacksCheckChecked *self, ChunksFileInterruptible io, StdVec *result)
{
	chunks_packs_check_packs_frame_0_t frame = {.self = self, .io = io, .result = result};

	std_landing(&frame, chunks_packs_check_packs_body_0, chunks_packs_check_packs_pad_0);
}

/* chunks_packs_check_u64_insert inserts a number into a set of them in order, a BTreeSet<u64>'s insert. */
static void
chunks_packs_check_u64_insert(StdVec *set, uint64 value)
{
	size_t		at;

	if (!std_slice_binary_search(set->ptr, set->len, sizeof(uint64), &value, std_u64_Ord_cmp, &at))
		std_vec_insert(set, at, &value, sizeof(value), _Alignof(uint64), RUST_FILE, 0, 0);
}

/* chunks_packs_check_place_cmp is (u64, u64)'s Ord, a pack's number then a record's offset. */
static StdOrdering
chunks_packs_check_place_cmp(const void *a, const void *b)
{
	const std_TupleU64U64_t *x = a;
	const std_TupleU64U64_t *y = b;

	if (x->f0 != y->f0)
		return x->f0 < y->f0 ? STD_ORDERING_LESS : STD_ORDERING_GREATER;
	if (x->f1 != y->f1)
		return x->f1 < y->f1 ? STD_ORDERING_LESS : STD_ORDERING_GREATER;
	return STD_ORDERING_EQUAL;
}

/* chunks_packs_check_held_get returns a pack's records of what packs read, or NULL for a pack it doesn't hold. */
static const StdVec *
chunks_packs_check_held_get(const StdVec *held, uint64 number)
{
	for (size_t k = 0, k_len = held->len; k < k_len; k++)
		if (STD_VEC_AT(held, chunks_packs_check_held_t, k).number == number)
			return &STD_VEC_AT(held, chunks_packs_check_held_t, k).chunks;
	return NULL;
}

/* chunks_packs_check_runs_frame_0_t is runs' state across its pad: the places reached, the packs indexed, and the run it reads. */
typedef struct chunks_packs_check_runs_frame_0_t
{
	ChunksPacksCheckChecked *self;
	HostIoReadDoors io;
	const StdVec *held;
	/* reached is a BTreeSet<(u64, u64)>: std_TupleU64U64_t in order. */
	StdVec		reached;
	bool		reached_live;
	/* indexed is a BTreeSet<u64> in order. */
	StdVec		indexed;
	bool		indexed_live;
	StdPathBuf	dir;
	bool		dir_live;
	StdPathBuf	path;
	bool		path_live;
	/* entries is a Vec<(Address, Locator)> of a run's entries. */
	StdVec		entries;
	bool		entries_live;
} chunks_packs_check_runs_frame_0_t;

/* chunks_packs_check_runs_pad_0 is runs' pad: the run it reads, then the places and packs. */
static void
chunks_packs_check_runs_pad_0(void *frame_)
{
	chunks_packs_check_runs_frame_0_t *frame = frame_;

	if (frame->entries_live)
	{
		frame->entries_live = false;
		std_vec_free_Checked(&frame->entries, sizeof(chunks_TupleAddressLocator_t),
							 _Alignof(chunks_TupleAddressLocator_t));
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
	if (frame->indexed_live)
	{
		frame->indexed_live = false;
		std_vec_free_Checked(&frame->indexed, sizeof(uint64), _Alignof(uint64));
	}
	if (frame->reached_live)
	{
		frame->reached_live = false;
		std_vec_free_Checked(&frame->reached, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
	}
}

/* chunks_packs_check_stale_noted notes the entries of a run that name packs the pack table doesn't list. */
static void
chunks_packs_check_stale_noted(ChunksPacksCheckChecked *self, const StdPathBuf *path, uint64 stale)
{
	StdStr		shown = {path->bytes.ptr, path->bytes.len};

	if (stale == 0)
		return;
	if (stale == 1)
	{
		chunks_packs_check_path_said(&self->notes, "1 entry of run \"", path,
									 "\" names a pack the pack table doesn't list, which the run's next merge drops");
		return;
	}
	{
		StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(" entries of run \""), STD_FMT_ARG(1),
		STD_FMT_LIT("\" name packs the pack table doesn't list, which the run's next merge drops")};
		StdFmtArg	args[] = {{&stale, std_u64_Display_fmt}, {&shown, std_path_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		chunks_packs_check_push(&self->notes, &arguments);
	}
}

/* chunks_packs_check_run_checked checks one run's entries against the records the packs hold. */
static void
chunks_packs_check_run_checked(chunks_packs_check_runs_frame_0_t *frame, const StdString *line)
{
	ChunksPacksCheckChecked *self = frame->self;
	StdVec		covered;
	StdStr		name = chunks_packs_run_name(line->ptr, line->len);
	ChunksIndexRunLocator run;
	ChunksErrorError error;
	uint64		stale = 0;
	bool		ok;

	chunks_packs_merge_covered(line->ptr, line->len, &covered);
	for (size_t k = 0, k_len = covered.len; k < k_len; k++)
		chunks_packs_check_u64_insert(&frame->indexed, STD_VEC_AT(&covered, uint64, k));
	std_vec_free_Checked(&covered, sizeof(uint64), _Alignof(uint64));
	std_path_join(frame->dir.bytes.ptr, frame->dir.bytes.len, name.ptr, name.len, &frame->path);
	frame->path_live = true;
	ok = chunks_index_RunLocator_open(frame->io, frame->path.bytes.ptr, frame->path.bytes.len, &run, &error);
	if (ok)
	{
		ok = chunks_index_RunLocator_entries(&run, frame->io, &frame->entries, &error);
		chunks_index_RunLocator_drop(&run);
	}
	if (!ok)
	{
		chunks_packs_check_failed(&self->found, "run \"", &frame->path, "\" can't be read: ", &error);
		goto done;
	}
	frame->entries_live = true;
	for (size_t k = 0, k_len = frame->entries.len; k < k_len; k++)
	{
		const chunks_TupleAddressLocator_t *entry = &STD_VEC_AT(&frame->entries, chunks_TupleAddressLocator_t, k);
		const StdVec *chunks = chunks_packs_check_held_get(frame->held, entry->f1.pack);
		uint64		offset = chunks_trailer_Locator_offset(&entry->f1);
		size_t		at;

		if (chunks == NULL)
		{
			stale++;
			continue;
		}
		if (chunks_packs_check_chunk_at(chunks, offset, &at) &&
			chunks_address_Address_PartialEq_eq(&STD_VEC_AT(chunks, chunks_packs_check_chunk_t, at).address,
												&entry->f0))
		{
			std_TupleU64U64_t place = {entry->f1.pack, offset};

			if (!std_slice_binary_search(frame->reached.ptr, frame->reached.len, sizeof(place), &place,
										 chunks_packs_check_place_cmp, &at))
				std_vec_insert(&frame->reached, at, &place, sizeof(place), _Alignof(std_TupleU64U64_t), RUST_FILE,
							   0, 0);
			continue;
		}
		{
			StdStr		shown = {frame->path.bytes.ptr, frame->path.bytes.len};
			StdFmtPiece pieces[] = {STD_FMT_LIT("chunk "), STD_FMT_ARG(0), STD_FMT_LIT(" of run \""), STD_FMT_ARG(1),
				STD_FMT_LIT("\" lies at offset "), STD_FMT_ARG(2), STD_FMT_LIT(" of pack "), STD_FMT_ARG(3),
				STD_FMT_LIT(", where the pack holds no record of it, so a read of the chunk by its address reads "
							"bytes that aren't its own")};
			StdFmtArg	args[] = {{&entry->f0, chunks_address_Address_Display_fmt}, {&shown, std_path_Display_fmt},
			{&offset, std_u64_Display_fmt}, {&entry->f1.pack, std_u64_Display_fmt}};
			StdFmtArguments arguments = {pieces, lengthof(pieces), args};

			chunks_packs_check_push(&self->found, &arguments);
		}
	}
	frame->entries_live = false;
	std_vec_free_Checked(&frame->entries, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
	chunks_packs_check_stale_noted(self, &frame->path, stale);
done:
	frame->path_live = false;
	std_vec_free_Checked(&frame->path.bytes, 1, 1);
}

/* chunks_packs_check_missed_said reports a held pack's records no run reached. */
static void
chunks_packs_check_missed_said(chunks_packs_check_runs_frame_0_t *frame, const chunks_packs_check_held_t *pack)
{
	ChunksPacksCheckChecked *self = frame->self;
	const ChunksAddressAddress *first = NULL;
	size_t		count = 0;
	size_t		at;

	for (size_t k = 0, k_len = pack->chunks.len; k < k_len; k++)
	{
		const chunks_packs_check_chunk_t *chunk = &STD_VEC_AT(&pack->chunks, chunks_packs_check_chunk_t, k);
		std_TupleU64U64_t place = {pack->number, chunk->offset};

		if (std_slice_binary_search(frame->reached.ptr, frame->reached.len, sizeof(place), &place,
									chunks_packs_check_place_cmp, &at))
			continue;
		if (first == NULL)
			first = &chunk->address;
		count++;
	}
	if (first == NULL)
		return;
	if (!std_slice_binary_search(frame->indexed.ptr, frame->indexed.len, sizeof(uint64), &pack->number,
								 std_u64_Ord_cmp, &at))
	{
		StdFmtPiece pieces[] = {STD_FMT_LIT("pack "), STD_FMT_ARG(0),
		STD_FMT_LIT(" is in no run, whose run the next start writes again from the pack")};
		StdFmtArg	args[] = {{&pack->number, std_u64_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		chunks_packs_check_push(&self->settled, &arguments);
	}
	else if (count == 1)
	{
		StdFmtPiece pieces[] = {STD_FMT_LIT("pack "), STD_FMT_ARG(0), STD_FMT_LIT(" holds 1 chunk no run names, "),
		STD_FMT_ARG(1), STD_FMT_LIT(", so a read of it by its address misses it")};
		StdFmtArg	args[] = {{&pack->number, std_u64_Display_fmt}, {first, chunks_address_Address_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		chunks_packs_check_push(&self->found, &arguments);
	}
	else
	{
		StdFmtPiece pieces[] = {STD_FMT_LIT("pack "), STD_FMT_ARG(0), STD_FMT_LIT(" holds "), STD_FMT_ARG(1),
			STD_FMT_LIT(" chunks no run names, the first "), STD_FMT_ARG(2),
		STD_FMT_LIT(", so a read of them by their addresses misses them")};
		StdFmtArg	args[] = {{&pack->number, std_u64_Display_fmt}, {&count, std_usize_Display_fmt},
		{first, chunks_address_Address_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		chunks_packs_check_push(&self->found, &arguments);
	}
}

/* chunks_packs_check_runs_body_0 is runs' body: each tablespace's runs read against the packs, then each held pack's records no run reached. */
static void
chunks_packs_check_runs_body_0(void *frame_)
{
	chunks_packs_check_runs_frame_0_t *frame = frame_;
	ChunksPacksCheckChecked *self = frame->self;

	std_vec_new(&frame->reached, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
	frame->reached_live = true;
	std_vec_new(&frame->indexed, sizeof(uint64), _Alignof(uint64));
	frame->indexed_live = true;
	for (size_t k = 0, k_len = self->view.runs.len; k < k_len; k++)
	{
		const chunks_TupleU32ReadRuns_t *runs = &STD_VEC_AT(&self->view.runs, chunks_TupleU32ReadRuns_t, k);

		chunks_packs_view_View_space_dir(&self->view, runs->f0, &frame->dir);
		frame->dir_live = true;
		for (size_t n = 0, n_len = runs->f1.list.lines.len; n < n_len; n++)
			chunks_packs_check_run_checked(frame, &STD_VEC_AT(&runs->f1.list.lines, StdString, n));
		frame->dir_live = false;
		std_vec_free_Checked(&frame->dir.bytes, 1, 1);
	}
	for (size_t k = 0, k_len = frame->held->len; k < k_len; k++)
		chunks_packs_check_missed_said(frame, &STD_VEC_AT(frame->held, chunks_packs_check_held_t, k));
	chunks_packs_check_runs_pad_0(frame);
}

/*
 * chunks_packs_check_Checked_runs checks each run's entries against the
 * records the packs hold, and that a lookup by address through the runs
 * finds every record a listed pack holds.
 */
static void
chunks_packs_check_Checked_runs(ChunksPacksCheckChecked *self, HostIoReadDoors io, const StdVec *held)
{
	chunks_packs_check_runs_frame_0_t frame = {.self = self, .io = io, .held = held};

	std_landing(&frame, chunks_packs_check_runs_body_0, chunks_packs_check_runs_pad_0);
}

/* chunks_packs_check_named reports whether a set of names, a Vec<String>, holds a name. */
static bool
chunks_packs_check_named(const StdVec *names, const char *name, size_t name_len)
{
	for (size_t k = 0, k_len = names->len; k < k_len; k++)
	{
		const StdString *known = &STD_VEC_AT(names, StdString, k);

		if (known->len == name_len && memcmp(known->ptr, name, name_len) == 0)
			return true;
	}
	return false;
}

/* chunks_packs_check_run_names appends the names of the runs a list's lines name. */
static void
chunks_packs_check_run_names(StdVec *names, const StdVec *lines)
{
	for (size_t k = 0, k_len = lines->len; k < k_len; k++)
	{
		const StdString *line = &STD_VEC_AT(lines, StdString, k);
		StdStr		name = chunks_packs_run_name(line->ptr, line->len);
		StdString	owned;

		std_string_from_Checked(&owned, name.ptr, name.len);
		std_vec_push_Checked(names, &owned, sizeof(owned), _Alignof(StdString));
	}
}

/* chunks_packs_check_ends_with reports whether a name ends with a text. */
static bool
chunks_packs_check_ends_with(const StdString *name, const char *ending)
{
	size_t		len = strlen(ending);

	return name->len >= len && memcmp((const char *) name->ptr + name->len - len, ending, len) == 0;
}

/* chunks_packs_check_leftovers_frame_0_t is leftovers' state across its pad: the tablespaces and the names of one. */
typedef struct chunks_packs_check_leftovers_frame_0_t
{
	ChunksPacksCheckChecked *self;
	HostIoReadDoors io;
	/* spaces is a Vec<u32>. */
	StdVec		spaces;
	bool		spaces_live;
	StdPathBuf	dir;
	bool		dir_live;
	/* listed is a BTreeSet<String> of the listed packs' file names there. */
	StdVec		listed;
	bool		listed_live;
	/* named is a BTreeSet<String> of the runs' file names a list names there. */
	StdVec		named;
	bool		named_live;
	/* names is a Vec<String> of the directory's files. */
	StdVec		names;
	bool		names_live;
	StdPathBuf	path;
	bool		path_live;
} chunks_packs_check_leftovers_frame_0_t;

/* chunks_packs_check_leftovers_pad_0 is leftovers' pad: a tablespace's names, then the tablespaces. */
static void
chunks_packs_check_leftovers_pad_0(void *frame_)
{
	chunks_packs_check_leftovers_frame_0_t *frame = frame_;

	if (frame->path_live)
	{
		frame->path_live = false;
		std_vec_free_Checked(&frame->path.bytes, 1, 1);
	}
	if (frame->names_live)
	{
		frame->names_live = false;
		chunks_packs_check_Lines_drop(&frame->names);
	}
	if (frame->named_live)
	{
		frame->named_live = false;
		chunks_packs_check_Lines_drop(&frame->named);
	}
	if (frame->listed_live)
	{
		frame->listed_live = false;
		chunks_packs_check_Lines_drop(&frame->listed);
	}
	if (frame->dir_live)
	{
		frame->dir_live = false;
		std_vec_free_Checked(&frame->dir.bytes, 1, 1);
	}
	if (frame->spaces_live)
	{
		frame->spaces_live = false;
		std_vec_free_Checked(&frame->spaces, sizeof(uint32), _Alignof(uint32));
	}
}

/* chunks_packs_check_space_left reports the files a crash leaves in one tablespace's directory of packs. */
static void
chunks_packs_check_space_left(chunks_packs_check_leftovers_frame_0_t *frame, uint32 space)
{
	ChunksPacksCheckChecked *self = frame->self;
	const ChunksPacksViewReadRuns *runs = chunks_packs_view_runs_get(&self->view.runs, space);
	StdVec		listed;
	ChunksErrorError error;

	chunks_packs_view_View_space_dir(&self->view, space, &frame->dir);
	frame->dir_live = true;
	std_vec_new(&frame->listed, sizeof(StdString), _Alignof(StdString));
	frame->listed_live = true;
	chunks_packs_view_View_listed(&self->view, &listed);
	for (size_t k = 0, k_len = listed.len; k < k_len; k++)
	{
		const ChunksPacksTableEntry *entry = &STD_VEC_AT(&listed, ChunksPacksTableEntry, k);
		StdString	name;

		if (entry->f1 != space)
			continue;
		name = chunks_pack_PackId_file_name(&entry->f2, "pack", 4);
		std_vec_push_Checked(&frame->listed, &name, sizeof(name), _Alignof(StdString));
	}
	std_vec_free_Checked(&listed, sizeof(ChunksPacksTableEntry), _Alignof(ChunksPacksTableEntry));
	std_vec_new(&frame->named, sizeof(StdString), _Alignof(StdString));
	frame->named_live = true;
	if (runs != NULL)
		chunks_packs_check_run_names(&frame->named, &runs->list.lines);
	if (space == CHUNKS_PACKS_DEFAULT_SPACE)
		chunks_packs_check_run_names(&frame->named, &self->view.forwarding.list.lines);
	if (!chunks_file_file_names(frame->io, frame->dir.bytes.ptr, frame->dir.bytes.len, &frame->names, &error))
		chunks_packs_check_failed(&self->found, "could not read \"", &frame->dir, "\": ", &error);
	else
	{
		frame->names_live = true;
		for (size_t k = 0, k_len = frame->names.len; k < k_len; k++)
		{
			const StdString *name = &STD_VEC_AT(&frame->names, StdString, k);

			std_path_join(frame->dir.bytes.ptr, frame->dir.bytes.len, name->ptr, name->len, &frame->path);
			frame->path_live = true;
			if (chunks_packs_check_ends_with(name, ".pack.tmp"))
				chunks_packs_check_path_said(&self->settled, "\"", &frame->path,
											 "\" is a pack a writer left unfinished, which the next start removes");
			else if (chunks_packs_check_ends_with(name, ".pack") &&
					 !chunks_packs_check_named(&frame->listed, name->ptr, name->len))
				chunks_packs_check_path_said(&self->notes, "pack \"", &frame->path, "\" is in no pack table");
			else if (chunks_packs_check_ends_with(name, ".idx") &&
					 !chunks_packs_check_named(&frame->named, name->ptr, name->len))
				chunks_packs_check_path_said(&self->settled, "run \"", &frame->path,
											 "\" is named by no runs file, which the next start removes");
			frame->path_live = false;
			std_vec_free_Checked(&frame->path.bytes, 1, 1);
		}
		frame->names_live = false;
		chunks_packs_check_Lines_drop(&frame->names);
	}
	frame->named_live = false;
	chunks_packs_check_Lines_drop(&frame->named);
	frame->listed_live = false;
	chunks_packs_check_Lines_drop(&frame->listed);
	frame->dir_live = false;
	std_vec_free_Checked(&frame->dir.bytes, 1, 1);
}

/* chunks_packs_check_leftovers_body_0 is leftovers' body: the default tablespace and each the pack table names, unless missing. */
static void
chunks_packs_check_leftovers_body_0(void *frame_)
{
	chunks_packs_check_leftovers_frame_0_t *frame = frame_;
	ChunksPacksCheckChecked *self = frame->self;
	const StdVec *tails = &self->view.table.list.runs;
	bool		has_default = false;
	uint32		space = CHUNKS_PACKS_DEFAULT_SPACE;

	std_vec_with_capacity_Checked(&frame->spaces, tails->len + 1, sizeof(uint32), _Alignof(uint32));
	frame->spaces_live = true;
	for (size_t k = 0, k_len = tails->len; k < k_len; k++)
	{
		uint32		at = STD_VEC_AT(tails, chunks_TupleU32Tail_t, k).f0;

		has_default = has_default || at == CHUNKS_PACKS_DEFAULT_SPACE;
		std_vec_push_Checked(&frame->spaces, &at, sizeof(at), _Alignof(uint32));
	}
	if (!has_default)
		std_vec_insert(&frame->spaces, 0, &space, sizeof(space), _Alignof(uint32), RUST_FILE, 302, 20);
	for (size_t k = 0, k_len = frame->spaces.len; k < k_len; k++)
	{
		space = STD_VEC_AT(&frame->spaces, uint32, k);
		if (!chunks_packs_view_missing_contains(&self->view.missing, space))
			chunks_packs_check_space_left(frame, space);
	}
	chunks_packs_check_leftovers_pad_0(frame);
}

/*
 * chunks_packs_check_Checked_leftovers reports the files a crash leaves in
 * the repository's directories of packs: a pack a writer left unfinished and
 * a run no runs file or forwarding file names, which the next start removes,
 * and a pack no pack table lists, which it leaves as it is.
 */
static void
chunks_packs_check_Checked_leftovers(ChunksPacksCheckChecked *self, HostIoReadDoors io)
{
	chunks_packs_check_leftovers_frame_0_t frame = {.self = self, .io = io};

	std_landing(&frame, chunks_packs_check_leftovers_body_0, chunks_packs_check_leftovers_pad_0);
}

/*
 * chunks_packs_check_Checked_read reads a chunk by its address and its
 * locator as a backend's read does, through the forwards when a collection
 * retired the locator's pack.
 */
bool
chunks_packs_check_Checked_read(ChunksPacksCheckChecked *self, HostIoReadDoors io, const ChunksAddressAddress *address,
								ChunksTrailerLocator locator, ChunksChunkChunk *result, ChunksErrorError *error)
{
	return chunks_packs_reads_View_read(&self->view, io, address, locator, result, error);
}

/* chunks_packs_check_Checked_drop drops what a check found and the view it built. */
void
chunks_packs_check_Checked_drop(ChunksPacksCheckChecked *self)
{
	chunks_packs_check_Lines_drop(&self->found);
	chunks_packs_check_Lines_drop(&self->settled);
	chunks_packs_check_Lines_drop(&self->notes);
	chunks_packs_view_View_drop(&self->view);
}

/* chunks_packs_check_missing_insert inserts a tablespace into the view's missing ones in order, a BTreeSet<u32>'s insert. */
static void
chunks_packs_check_missing_insert(StdVec *missing, uint32 space)
{
	size_t		at = 0;

	while (at < missing->len && STD_VEC_AT(missing, uint32, at) < space)
		at++;
	if (at < missing->len && STD_VEC_AT(missing, uint32, at) == space)
		return;
	std_vec_insert(missing, at, &space, sizeof(space), _Alignof(uint32), RUST_FILE, 0, 0);
}

/* chunks_packs_check_check_frame_0_t is check's state across its pad: the result, its directory, a path, a runs file, and what packs read. */
typedef struct chunks_packs_check_check_frame_0_t
{
	ChunksFileInterruptible io;
	uint32		repository;
	uint32		own;
	ChunksPacksCheckChecked *result;
	bool		result_live;
	StdPathBuf	dir;
	bool		dir_live;
	StdPathBuf	space_dir;
	bool		space_dir_live;
	StdPathBuf	path;
	bool		path_live;
	ChunksPacksViewReadRuns runs;
	bool		runs_live;

	/*
	 * held is a BTreeMap<u64, BTreeMap<u64, Address>>:
	 * chunks_packs_check_held_t in number order.
	 */
	StdVec		held;
	bool		held_live;
} chunks_packs_check_check_frame_0_t;

/* chunks_packs_check_check_pad_0 is check's pad: what packs read, the runs file and paths, the directory, then the result. */
static void
chunks_packs_check_check_pad_0(void *frame_)
{
	chunks_packs_check_check_frame_0_t *frame = frame_;

	if (frame->held_live)
	{
		frame->held_live = false;
		chunks_packs_check_Held_drop(&frame->held);
	}
	if (frame->runs_live)
	{
		frame->runs_live = false;
		chunks_packs_Runs_drop(&frame->runs.list);
	}
	if (frame->path_live)
	{
		frame->path_live = false;
		std_vec_free_Checked(&frame->path.bytes, 1, 1);
	}
	if (frame->space_dir_live)
	{
		frame->space_dir_live = false;
		std_vec_free_Checked(&frame->space_dir.bytes, 1, 1);
	}
	if (frame->dir_live)
	{
		frame->dir_live = false;
		std_vec_free_Checked(&frame->dir.bytes, 1, 1);
	}
	if (frame->result_live)
	{
		frame->result_live = false;
		chunks_packs_check_Checked_drop(frame->result);
	}
}

/* chunks_packs_check_joined sets the frame's path to a file in a directory. */
static void
chunks_packs_check_joined(chunks_packs_check_check_frame_0_t *frame, const StdPathBuf *dir, const char *file)
{
	std_path_join(dir->bytes.ptr, dir->bytes.len, file, strlen(file), &frame->path);
	frame->path_live = true;
}

/* chunks_packs_check_unjoined frees the frame's path. */
static void
chunks_packs_check_unjoined(chunks_packs_check_check_frame_0_t *frame)
{
	frame->path_live = false;
	std_vec_free_Checked(&frame->path.bytes, 1, 1);
}

/* chunks_packs_check_space_read reads one tablespace's runs file into the view, or notes the tablespace missing. */
static void
chunks_packs_check_space_read(chunks_packs_check_check_frame_0_t *frame, HostIoReadDoors read, uint32 space)
{
	ChunksPacksCheckChecked *checked = frame->result;
	ChunksErrorError error;
	bool		there;

	chunks_packs_view_View_space_dir(&checked->view, space, &frame->space_dir);
	frame->space_dir_live = true;
	if (!chunks_file_dir_there(read, frame->space_dir.bytes.ptr, frame->space_dir.bytes.len, &there, &error))
		chunks_packs_check_failed(&checked->found, "could not read \"", &frame->space_dir, "\": ", &error);
	else if (there)
	{
		chunks_packs_check_joined(frame, &frame->space_dir, CHUNKS_PACKS_RUNS);
		frame->runs.read.at = 0;
		frame->runs.read.past = 0;
		chunks_packs_Runs_Default_default(&frame->runs.list);
		frame->runs_live = true;
		if (chunks_packs_check_Checked_list(checked, read, &CHUNKS_PACKS_VIEW_RUNS_LIST_OPS, &frame->path,
											&frame->runs.read, &frame->runs.list))
		{
			ChunksPacksViewReadRuns *entry = chunks_packs_view_runs_entry(&checked->view.runs, space);

			chunks_packs_Runs_drop(&entry->list);
			*entry = frame->runs;
		}
		else
			chunks_packs_Runs_drop(&frame->runs.list);
		frame->runs_live = false;
		chunks_packs_check_unjoined(frame);
	}
	else
	{
		StdStr		shown = {frame->space_dir.bytes.ptr, frame->space_dir.bytes.len};
		StdFmtPiece pieces[] = {STD_FMT_LIT("tablespace "), STD_FMT_ARG(0), STD_FMT_LIT("'s directory \""),
		STD_FMT_ARG(1), STD_FMT_LIT("\" is missing, so its packs are not checked")};
		StdFmtArg	args[] = {{&space, std_u32_Display_fmt}, {&shown, std_path_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		chunks_packs_check_push(&checked->notes, &arguments);
		chunks_packs_check_missing_insert(&checked->view.missing, space);
	}
	frame->space_dir_live = false;
	std_vec_free_Checked(&frame->space_dir.bytes, 1, 1);
}

/*
 * chunks_packs_check_check_body_0 is check's body: the drop's mark, the pack
 * table, each tablespace's runs file, the forwarding file, the packs, the
 * view's open, the runs, and the files a crash leaves.
 */
static void
chunks_packs_check_check_body_0(void *frame_)
{
	chunks_packs_check_check_frame_0_t *frame = frame_;
	ChunksPacksCheckChecked *checked = frame->result;
	HostIoReadDoors read = {&frame->io.ops->read_doors, frame->io.self};
	ChunksPacksTableEntry none[1];
	ChunksErrorError error;
	bool		marked;

	std_vec_new(&checked->found, sizeof(StdString), _Alignof(StdString));
	std_vec_new(&checked->settled, sizeof(StdString), _Alignof(StdString));
	std_vec_new(&checked->notes, sizeof(StdString), _Alignof(StdString));
	checked->dropped = false;
	chunks_packs_view_View_new(frame->repository, frame->own, &checked->view);
	frame->result_live = true;
	std_PathBuf_Clone_clone(&checked->view.dir, &frame->dir);
	frame->dir_live = true;
	if (!chunks_packs_dropped_marked(read, frame->dir.bytes.ptr, frame->dir.bytes.len, &marked, &error))
		chunks_packs_check_failed(&checked->found, "could not read \"", &frame->dir, "\": ", &error);
	else if (marked)
	{
		checked->dropped = true;
		goto done;
	}
	chunks_packs_check_joined(frame, &frame->dir, CHUNKS_PACKS_TABLE);
	if (!chunks_packs_check_Checked_list(checked, read, &CHUNKS_PACKS_VIEW_TABLE_LIST_OPS, &frame->path,
										 &checked->view.table.read, &checked->view.table.list))
		goto done;
	chunks_packs_check_unjoined(frame);
	for (size_t k = 0, k_len = checked->view.table.list.runs.len; k < k_len; k++)
		chunks_packs_check_space_read(frame, read, STD_VEC_AT(&checked->view.table.list.runs, chunks_TupleU32Tail_t,
															  k).f0);
	chunks_packs_check_joined(frame, &frame->dir, CHUNKS_PACKS_FORWARDING);
	(void) chunks_packs_check_Checked_list(checked, read, &CHUNKS_PACKS_VIEW_RUNS_LIST_OPS, &frame->path,
										   &checked->view.forwarding.read, &checked->view.forwarding.list);
	chunks_packs_check_unjoined(frame);
	std_vec_new(&frame->held, sizeof(chunks_packs_check_held_t), _Alignof(chunks_packs_check_held_t));
	frame->held_live = true;
	chunks_packs_check_Checked_packs(checked, frame->io, &frame->held);
	if (!chunks_packs_view_View_open(&checked->view, read, none, 0, &error))
	{
		chunks_packs_check_failed(&checked->found, "the packs of \"", &frame->dir,
								  "\" can't be opened as a read opens them: ", &error);
		goto done;
	}
	chunks_packs_check_Checked_runs(checked, read, &frame->held);
	chunks_packs_check_Checked_leftovers(checked, read);
done:
	frame->result_live = false;
	chunks_packs_check_check_pad_0(frame);
}

/* chunks_packs_check_check checks the packs of the repository with an id, whose own directory is in the tablespace with an id. */
void
chunks_packs_check_check(ChunksFileInterruptible io, uint32 repository, uint32 own, ChunksPacksCheckChecked *result)
{
	chunks_packs_check_check_frame_0_t frame = {.io = io, .repository = repository, .own = own, .result = result};

	std_landing(&frame, chunks_packs_check_check_body_0, chunks_packs_check_check_pad_0);
}

#ifdef DOLTGRES_TEST_CHUNKS

/* chunks_packs_check_tests_made makes a repository with an id holding two chunks, and frees its record, as a restart leaves it; it returns the repository's directory. */
static StdPathBuf
chunks_packs_check_tests_made(HostIoIo *io, uint32 id, chunks_TupleAddressLocator_t *placed)
{
	ChunksPacksBackendPacks packs;
	uint64		location;

	chunks_packs_fixtures_created(io, id, &packs);
	placed[0] = chunks_packs_fixtures_added(io, &packs, (const uint8 *) "one", 3);
	placed[1] = chunks_packs_fixtures_added(io, &packs, (const uint8 *) "two", 3);
	location = chunks_packs_backend_Packs_location(&packs);
	chunks_packs_backend_Packs_drop(&packs);
	chunks_packs_tests_freed(RUST_FILE, io, location, 363, 75, 84);
	return host_tablespace_dir(id, HOST_PG_DEFAULT);
}

/* chunks_packs_check_tests_checking checks the repository with an id. */
static void
chunks_packs_check_tests_checking(HostIoIo *io, uint32 id, ChunksPacksCheckChecked *result)
{
	ChunksFileInterruptible interruptible = {&CHUNKS_FILE_IO_INTERRUPTIBLE_OPS, io};

	if (id == 0)
		STD_OPTION_UNWRAP_FAILED(369, 39);
	chunks_packs_check_check(interruptible, id, HOST_PG_DEFAULT, result);
}

/* chunks_packs_check_tests_string_lt reports whether one string sorts before another, as str's Ord does. */
static bool
chunks_packs_check_tests_string_lt(const StdString *a, const StdString *b)
{
	int			order = memcmp(a->ptr, b->ptr, Min(a->len, b->len));

	return order < 0 || (order == 0 && a->len < b->len);
}

/* chunks_packs_check_tests_files returns the paths in a directory with a suffix, in order. */
static StdVec
chunks_packs_check_tests_files(const StdPathBuf *dir, const char *suffix)
{
	StdVec		names;
	StdVec		sorted;
	StdVec		found;
	StdIoError	io_error;
	bool		entry;

	if (!std_fs_read_dir_names(dir->bytes.ptr, dir->bytes.len, &names, &entry, &io_error))
	{
		if (entry)
			STD_RESULT_EXPECT_FAILED(376, 32, CHUNKS_PACKS_FIXTURES_UNWRAP, &io_error, std_io_Error_Debug_fmt);
		STD_RESULT_EXPECT_FAILED(375, 14, CHUNKS_PACKS_FIXTURES_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	}
	std_vec_new(&sorted, sizeof(StdString), _Alignof(StdString));
	for (size_t k = 0, k_len = names.len; k < k_len; k++)
	{
		StdString  *name = &STD_VEC_AT(&names, StdString, k);
		size_t		at = sorted.len;

		if (!chunks_packs_check_ends_with(name, suffix))
		{
			std_String_drop(name);
			continue;
		}
		while (at > 0 && chunks_packs_check_tests_string_lt(name, &STD_VEC_AT(&sorted, StdString, at - 1)))
			at--;
		std_vec_insert(&sorted, at, name, sizeof(StdString), _Alignof(StdString), RUST_FILE, 0, 0);
	}
	std_vec_free_Checked(&names, sizeof(StdString), _Alignof(StdString));
	std_vec_new(&found, sizeof(StdPathBuf), _Alignof(StdPathBuf));
	for (size_t k = 0, k_len = sorted.len; k < k_len; k++)
	{
		const StdString *name = &STD_VEC_AT(&sorted, StdString, k);
		StdPathBuf	path;

		std_path_join(dir->bytes.ptr, dir->bytes.len, name->ptr, name->len, &path);
		std_vec_push_Checked(&found, &path, sizeof(path), _Alignof(StdPathBuf));
	}
	chunks_packs_check_Lines_drop(&sorted);
	return found;
}

/* chunks_packs_check_tests_Paths_drop drops a Vec<PathBuf>. */
static void
chunks_packs_check_tests_Paths_drop(StdVec *paths)
{
	std_vec_drop_Checked(paths, sizeof(StdPathBuf), _Alignof(StdPathBuf), chunks_pack_PathBuf_drop);
}

/* chunks_packs_check_tests_first returns a copy of the first of the paths with a suffix in a directory, which panics at a line for none. */
static StdPathBuf
chunks_packs_check_tests_first(const StdPathBuf *dir, const char *suffix, uint32 line)
{
	StdVec		found = chunks_packs_check_tests_files(dir, suffix);
	StdPathBuf	first;

	std_index_check(0, found.len, RUST_FILE, line, 20);
	std_PathBuf_Clone_clone(&STD_VEC_AT(&found, StdPathBuf, 0), &first);
	chunks_packs_check_tests_Paths_drop(&found);
	return first;
}

/* chunks_packs_check_tests_Lines_Debug_fmt is a Vec<String>'s Debug. */
static bool
chunks_packs_check_tests_Lines_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *lines = value;
	StdDebugList l = std_fmt_debug_list(f);

	for (size_t k = 0, k_len = lines->len; k < k_len; k++)
		std_DebugList_entry(&l, &STD_VEC_AT(lines, StdString, k), std_String_Debug_fmt);
	return std_DebugList_finish(&l);
}

/* chunks_packs_check_tests_lines_same reports whether two lists of lines are the same, in order. */
static bool
chunks_packs_check_tests_lines_same(const StdVec *got, const StdVec *want)
{
	if (got->len != want->len)
		return false;
	for (size_t k = 0, k_len = got->len; k < k_len; k++)
	{
		const StdString *a = &STD_VEC_AT(got, StdString, k);
		const StdString *b = &STD_VEC_AT(want, StdString, k);

		if (a->len != b->len || memcmp(a->ptr, b->ptr, a->len) != 0)
			return false;
	}
	return true;
}

/* chunks_packs_check_tests_lines_eq asserts at a line that lines are those given, which it drops. */
static void
chunks_packs_check_tests_lines_eq(const StdVec *got, StdVec *want, uint32 line)
{
	if (!chunks_packs_check_tests_lines_same(got, want))
		std_assert_failed(RUST_FILE, line, 9, "==", got, chunks_packs_check_tests_Lines_Debug_fmt, want,
						  chunks_packs_check_tests_Lines_Debug_fmt, NULL);
	chunks_packs_check_Lines_drop(want);
}

/* chunks_packs_check_tests_one returns a list of one line, formatted. */
static StdVec
chunks_packs_check_tests_one(const StdFmtArguments *arguments)
{
	StdVec		lines;

	std_vec_new(&lines, sizeof(StdString), _Alignof(StdString));
	chunks_packs_check_push(&lines, arguments);
	return lines;
}

/* chunks_packs_check_tests_none returns an empty list of lines. */
static StdVec
chunks_packs_check_tests_none(void)
{
	StdVec		lines;

	std_vec_new(&lines, sizeof(StdString), _Alignof(StdString));
	return lines;
}

/* chunks_packs_check_tests_read_file reads a file's bytes, which unwraps at a line and column. */
static StdVec
chunks_packs_check_tests_read_file(const StdPathBuf *path, uint32 line, uint32 col)
{
	StdVec		bytes;
	StdIoError	io_error;

	if (!std_fs_read(path->bytes.ptr, path->bytes.len, &bytes, &io_error))
		STD_RESULT_EXPECT_FAILED(line, col, CHUNKS_PACKS_FIXTURES_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	return bytes;
}

/* chunks_packs_check_tests_write_file writes a file's bytes, which unwraps at a line and column. */
static void
chunks_packs_check_tests_write_file(const StdPathBuf *path, const uint8 *bytes, size_t bytes_len, uint32 line,
									uint32 col)
{
	StdIoError	io_error;

	if (!std_fs_write(path->bytes.ptr, path->bytes.len, bytes, bytes_len, &io_error))
		STD_RESULT_EXPECT_FAILED(line, col, CHUNKS_PACKS_FIXTURES_UNWRAP, &io_error, std_io_Error_Debug_fmt);
}

/* chunks_packs_check_tests_removed_file removes a file, which unwraps at a line and column. */
static void
chunks_packs_check_tests_removed_file(const StdPathBuf *path, uint32 line, uint32 col)
{
	StdIoError	io_error;

	if (!std_fs_remove_file(path->bytes.ptr, path->bytes.len, &io_error))
		STD_RESULT_EXPECT_FAILED(line, col, CHUNKS_PACKS_FIXTURES_UNWRAP, &io_error, std_io_Error_Debug_fmt);
}

/* chunks_packs_check_tests_written_appended writes a list file whole from a record, which unwraps at a line and column. */
static void
chunks_packs_check_tests_written_appended(HostIoIo *io, const StdPathBuf *path, const StdVec *whole, uint32 line,
										  uint32 col)
{
	ChunksFileTornTail torn;
	ChunksErrorError error;
	bool		placing = false;

	if (!chunks_file_write_appended(HOST_IO_IO_IO_DOORS(io), path->bytes.ptr, path->bytes.len, whole->ptr, whole->len,
									&placing, &torn, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, line, col);
	chunks_file_TornTail_drop(&torn);
}

/*
 * test_chunks_packs_check_tests_a_clean_repository_finds_nothing_and_reads_its_chunks:
 * a repository a clean stop leaves finds nothing, and its chunks read
 * through the check's view.
 */
void
test_chunks_packs_check_tests_a_clean_repository_finds_nothing_and_reads_its_chunks(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	chunks_TupleAddressLocator_t placed[2];
	StdPathBuf	dir = chunks_packs_check_tests_made(io, 101, placed);
	ChunksPacksCheckChecked checked;
	StdVec		none = chunks_packs_check_tests_none();

	chunks_packs_check_tests_checking(io, 101, &checked);
	if (!chunks_packs_check_tests_lines_same(&checked.found, &none) ||
		!chunks_packs_check_tests_lines_same(&checked.settled, &none) ||
		!chunks_packs_check_tests_lines_same(&checked.notes, &none))
	{
		StdTupleRef got = STD_TUPLE(3, STD_ITEM(&checked.found, chunks_packs_check_tests_Lines_Debug_fmt),
									STD_ITEM(&checked.settled, chunks_packs_check_tests_Lines_Debug_fmt),
									STD_ITEM(&checked.notes, chunks_packs_check_tests_Lines_Debug_fmt));
		StdTupleRef want = STD_TUPLE(3, STD_ITEM(&none, chunks_packs_check_tests_Lines_Debug_fmt),
									 STD_ITEM(&none, chunks_packs_check_tests_Lines_Debug_fmt),
									 STD_ITEM(&none, chunks_packs_check_tests_Lines_Debug_fmt));

		std_assert_failed(RUST_FILE, 390, 9, "==", &got, std_tuple_Debug_fmt, &want, std_tuple_Debug_fmt, NULL);
	}
	for (size_t k = 0; k < lengthof(placed); k++)
	{
		ChunksChunkChunk chunk;
		ChunksAddressAddress read;
		ChunksErrorError error;

		if (!chunks_packs_check_Checked_read(&checked, HOST_IO_IO_READ_DOORS(io), &placed[k].f0, placed[k].f1, &chunk,
											 &error))
			chunks_packs_tests_unwrapped(RUST_FILE, &error, 392, 60);
		read = chunks_chunk_Chunk_address(&chunk);
		chunks_chunk_Chunk_drop(&chunk);
		STD_ASSERT_EQ(392, 13, chunks_address_Address_PartialEq_eq(&read, &placed[k].f0), &read,
					  chunks_address_Address_Debug_fmt, &placed[k].f0, chunks_address_Address_Debug_fmt);
	}
	chunks_packs_check_Checked_drop(&checked);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	simc_TestIo_drop(&test_io);
}

/* chunks_packs_check_tests_held_closure is a_damaged_pack_record_is_found_with_the_run_entry_naming_it's walk: each verified chunk's address. */
static bool
chunks_packs_check_tests_held_closure(void *captures, ChunksFileInterruptible io, ChunksPackVerified *chunk,
									  ChunksErrorError *error)
{
	ChunksAddressAddress address = chunks_pack_Verified_address(chunk);

	(void) io;
	(void) error;
	std_vec_push_Checked(captures, &address, sizeof(address), _Alignof(ChunksAddressAddress));
	return true;
}

/*
 * test_chunks_packs_check_tests_a_damaged_pack_record_is_found_with_the_run_entry_naming_it:
 * a pack record that doesn't hash to its address is found, naming the pack
 * and the chunk, and so is the run entry that names it, which a read by the
 * chunk's address would follow to bytes that aren't its own.
 */
void
test_chunks_packs_check_tests_a_damaged_pack_record_is_found_with_the_run_entry_naming_it(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	chunks_TupleAddressLocator_t placed[2];
	StdPathBuf	dir = chunks_packs_check_tests_made(io, 102, placed);
	StdPathBuf	pack = chunks_packs_check_tests_first(&dir, ".pack", 403);
	ChunksPackPackReader reader;
	ChunksFileInterruptible interruptible = {&CHUNKS_FILE_IO_INTERRUPTIBLE_OPS, io};
	StdVec		held;
	ChunksPackLost lost;
	ChunksErrorError error;
	const chunks_TupleAddressLocator_t *chosen = NULL;
	StdVec		bytes;
	size_t		middle;
	ChunksPacksCheckChecked checked;
	StdVec		runs;
	const StdPathBuf *run = NULL;
	StdStr		pack_stem;
	StdVec		found;

	if (!chunks_pack_PackReader_open(HOST_IO_IO_READ_DOORS(io), pack.bytes.ptr, pack.bytes.len, &reader, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 404, 50);
	std_vec_new(&held, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
	if (chunks_pack_PackReader_verified(&reader, interruptible, 0, chunks_packs_check_tests_held_closure, &held, NULL,
										&lost, &error))
		chunks_pack_Lost_drop(&lost);
	else
		chunks_error_Error_drop(&error);
	chunks_pack_PackReader_drop(&reader);
	for (size_t k = 0; k < lengthof(placed) && chosen == NULL; k++)
		for (size_t n = 0; n < held.len; n++)
			if (chunks_address_Address_PartialEq_eq(&STD_VEC_AT(&held, ChunksAddressAddress, n), &placed[k].f0))
				chosen = &placed[k];
	if (chosen == NULL)
		STD_OPTION_UNWRAP_FAILED(410, 101);
	bytes = chunks_packs_check_tests_read_file(&pack, 411, 46);
	middle = STD_ADD(usize, (size_t) chunks_trailer_Locator_offset(&chosen->f1),
					 (size_t) (chunks_trailer_Locator_length(&chosen->f1) / 2), 412, 22);
	std_index_check(middle, bytes.len, RUST_FILE, 413, 9);
	((uint8 *) bytes.ptr)[middle] ^= 0xff;
	chunks_packs_check_tests_write_file(&pack, bytes.ptr, bytes.len, 414, 38);
	std_vec_free_Checked(&bytes, 1, 1);
	chunks_packs_check_tests_checking(io, 102, &checked);
	runs = chunks_packs_check_tests_files(&dir, ".idx");
	(void) std_path_file_stem(pack.bytes.ptr, pack.bytes.len, &pack_stem);
	for (size_t k = 0; k < runs.len && run == NULL; k++)
	{
		const StdPathBuf *candidate = &STD_VEC_AT(&runs, StdPathBuf, k);
		StdStr		stem;

		if (std_path_file_stem(candidate->bytes.ptr, candidate->bytes.len, &stem) && stem.len == pack_stem.len &&
			memcmp(stem.ptr, pack_stem.ptr, stem.len) == 0)
			run = candidate;
	}
	if (run == NULL)
		STD_OPTION_UNWRAP_FAILED(416, 99);
	{
		uint64		offset = chunks_trailer_Locator_offset(&chosen->f1);
		StdStr		pack_shown = {pack.bytes.ptr, pack.bytes.len};
		StdStr		run_shown = {run->bytes.ptr, run->bytes.len};
		StdFmtPiece lost_pieces[] = {STD_FMT_LIT("pack \""), STD_FMT_ARG(0), STD_FMT_LIT("\" lost chunk "),
		STD_FMT_ARG(1), STD_FMT_LIT(" at offset "), STD_FMT_ARG(2)};
		StdFmtArg	lost_args[] = {{&pack_shown, std_path_Display_fmt}, {&chosen->f0, chunks_address_Address_Display_fmt},
		{&offset, std_u64_Display_fmt}};
		StdFmtArguments lost_arguments = {lost_pieces, lengthof(lost_pieces), lost_args};
		StdFmtPiece run_pieces[] = {STD_FMT_LIT("chunk "), STD_FMT_ARG(0), STD_FMT_LIT(" of run \""), STD_FMT_ARG(1),
			STD_FMT_LIT("\" lies at offset "), STD_FMT_ARG(2), STD_FMT_LIT(" of pack "), STD_FMT_ARG(3),
			STD_FMT_LIT(", where the pack holds no record of it, so a read of the chunk by its address reads "
						"bytes that aren't its own")};
		StdFmtArg	run_args[] = {{&chosen->f0, chunks_address_Address_Display_fmt}, {&run_shown, std_path_Display_fmt},
		{&offset, std_u64_Display_fmt}, {&chosen->f1.pack, std_u64_Display_fmt}};
		StdFmtArguments run_arguments = {run_pieces, lengthof(run_pieces), run_args};

		found = chunks_packs_check_tests_one(&lost_arguments);
		chunks_packs_check_push(&found, &run_arguments);
	}
	chunks_packs_check_tests_lines_eq(&checked.found, &found, 427);
	chunks_packs_check_tests_Paths_drop(&runs);
	chunks_packs_check_Checked_drop(&checked);
	std_vec_free_Checked(&held, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
	std_vec_free_Checked(&pack.bytes, 1, 1);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	simc_TestIo_drop(&test_io);
}

/*
 * test_chunks_packs_check_tests_a_listed_pack_whose_file_is_gone_is_found:
 * a listed pack whose file is gone is found, and the rest of the repository
 * is still checked.
 */
void
test_chunks_packs_check_tests_a_listed_pack_whose_file_is_gone_is_found(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	chunks_TupleAddressLocator_t placed[2];
	StdPathBuf	dir = chunks_packs_check_tests_made(io, 103, placed);
	StdPathBuf	pack = chunks_packs_check_tests_first(&dir, ".pack", 436);
	ChunksPacksCheckChecked checked;
	bool		matched = false;

	chunks_packs_check_tests_removed_file(&pack, 437, 37);
	chunks_packs_check_tests_checking(io, 103, &checked);
	if (checked.found.len == 1)
	{
		const StdString *line = &STD_VEC_AT(&checked.found, StdString, 0);
		StdString	shown;
		StdStr		path = {pack.bytes.ptr, pack.bytes.len};
		StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&path, std_path_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_fmt_format_Checked(&shown, &arguments);
		matched = line->len >= 5 && memcmp(line->ptr, "pack ", 5) == 0;
		if (matched)
		{
			matched = false;
			for (size_t k = 0; k + shown.len <= line->len && !matched; k++)
				matched = memcmp((const char *) line->ptr + k, shown.ptr, shown.len) == 0;
		}
		std_String_drop(&shown);
	}
	if (!matched)
	{
		StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&checked.found, chunks_packs_check_tests_Lines_Debug_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_panic_fmt(RUST_FILE, 439, 9, &arguments);
	}
	chunks_packs_check_Checked_drop(&checked);
	std_vec_free_Checked(&pack.bytes, 1, 1);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	simc_TestIo_drop(&test_io);
}

/*
 * test_chunks_packs_check_tests_a_pack_table_gone_with_its_previous_generation_is_found_as_the_settle_refuses_it:
 * a pack table gone with its previous generation is found with the settle's
 * own refusal of it.
 */
void
test_chunks_packs_check_tests_a_pack_table_gone_with_its_previous_generation_is_found_as_the_settle_refuses_it(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	chunks_TupleAddressLocator_t placed[2];
	StdPathBuf	dir = chunks_packs_check_tests_made(io, 104, placed);
	StdPathBuf	table;
	StdPathBuf	previous;
	StdIoError	io_error;
	ChunksPacksCheckChecked checked;
	StdStr		shown;
	StdFmtPiece pieces[] = {STD_FMT_LIT("required file \""), STD_FMT_ARG(0), STD_FMT_LIT("\" does not exist")};
	StdFmtArg	args[] = {{&shown, std_path_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};
	StdVec		found;

	std_path_join(dir.bytes.ptr, dir.bytes.len, CHUNKS_PACKS_TABLE, strlen(CHUNKS_PACKS_TABLE), &table);
	chunks_packs_check_tests_removed_file(&table, 453, 38);
	previous = chunks_file_previous(table.bytes.ptr, table.bytes.len);
	(void) std_fs_remove_file(previous.bytes.ptr, previous.bytes.len, &io_error);
	chunks_packs_check_tests_checking(io, 104, &checked);
	shown.ptr = table.bytes.ptr;
	shown.len = table.bytes.len;
	found = chunks_packs_check_tests_one(&arguments);
	chunks_packs_check_tests_lines_eq(&checked.found, &found, 456);
	chunks_packs_check_Checked_drop(&checked);
	std_vec_free_Checked(&previous.bytes, 1, 1);
	std_vec_free_Checked(&table.bytes, 1, 1);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	simc_TestIo_drop(&test_io);
}

/* chunks_packs_check_tests_path_line returns a line of a path between two texts. */
static StdVec
chunks_packs_check_tests_path_line(const char *before, const StdPathBuf *path, const char *after)
{
	StdVec		lines = chunks_packs_check_tests_none();

	chunks_packs_check_path_said(&lines, before, path, after);
	return lines;
}

/*
 * test_chunks_packs_check_tests_a_pack_table_read_as_its_previous_generation_is_settled_and_a_damaged_previous_one_found:
 * a pack table read as its previous generation, its newest missing as a
 * crash during its write leaves it, is what the next start settles, and a
 * damaged previous generation is found.
 */
void
test_chunks_packs_check_tests_a_pack_table_read_as_its_previous_generation_is_settled_and_a_damaged_previous_one_found(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	chunks_TupleAddressLocator_t placed[2];
	StdPathBuf	dir = chunks_packs_check_tests_made(io, 105, placed);
	StdPathBuf	table;
	StdPathBuf	previous;
	StdIoError	io_error;
	ChunksPacksCheckChecked checked;
	StdVec		none = chunks_packs_check_tests_none();
	StdVec		missing;
	StdVec		damaged;
	StdVec		bytes;
	size_t		last;

	std_path_join(dir.bytes.ptr, dir.bytes.len, CHUNKS_PACKS_TABLE, strlen(CHUNKS_PACKS_TABLE), &table);
	previous = chunks_file_previous(table.bytes.ptr, table.bytes.len);
	if (!std_fs_rename(table.bytes.ptr, table.bytes.len, previous.bytes.ptr, previous.bytes.len, &io_error))
		STD_RESULT_EXPECT_FAILED(467, 51, CHUNKS_PACKS_FIXTURES_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	chunks_packs_check_tests_checking(io, 105, &checked);
	missing = chunks_packs_check_tests_path_line("\"", &table,
												 "\" is missing, as a crash during its write leaves it; its previous "
												 "generation was read, which the next start writes whole again");
	if (!chunks_packs_check_tests_lines_same(&checked.found, &none) ||
		!chunks_packs_check_tests_lines_same(&checked.settled, &missing))
	{
		StdTupleRef got = STD_TUPLE(2, STD_ITEM(&checked.found, chunks_packs_check_tests_Lines_Debug_fmt),
									STD_ITEM(&checked.settled, chunks_packs_check_tests_Lines_Debug_fmt));
		StdTupleRef want = STD_TUPLE(2, STD_ITEM(&none, chunks_packs_check_tests_Lines_Debug_fmt),
									 STD_ITEM(&missing, chunks_packs_check_tests_Lines_Debug_fmt));

		std_assert_failed(RUST_FILE, 474, 9, "==", &got, std_tuple_Debug_fmt, &want, std_tuple_Debug_fmt, NULL);
	}
	chunks_packs_check_Lines_drop(&missing);
	chunks_packs_check_Checked_drop(&checked);

	bytes = chunks_packs_check_tests_read_file(&previous, 476, 49);
	chunks_packs_check_tests_write_file(&table, bytes.ptr, bytes.len, 476, 49);
	std_vec_free_Checked(&bytes, 1, 1);
	bytes = chunks_packs_check_tests_read_file(&previous, 477, 57);
	last = STD_SUB(usize, bytes.len, 1, 478, 20);
	std_index_check(last / 2, bytes.len, RUST_FILE, 479, 9);
	((uint8 *) bytes.ptr)[last / 2] ^= 0xff;
	chunks_packs_check_tests_write_file(&previous, bytes.ptr, bytes.len, 480, 49);
	std_vec_free_Checked(&bytes, 1, 1);
	chunks_packs_check_tests_checking(io, 105, &checked);
	damaged = chunks_packs_check_tests_path_line("the previous generation of \"", &table,
												 "\" is damaged, so a read falling back to it finds nothing");
	chunks_packs_check_tests_lines_eq(&checked.found, &damaged, 486);
	chunks_packs_check_Checked_drop(&checked);
	std_vec_free_Checked(&previous.bytes, 1, 1);
	std_vec_free_Checked(&table.bytes, 1, 1);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	simc_TestIo_drop(&test_io);
}

/* chunks_packs_check_tests_set returns a BTreeSet<String> of lines: each line once, borrowed, in order. */
static StdVec
chunks_packs_check_tests_set(const StdVec *lines)
{
	StdVec		set;

	std_vec_new(&set, sizeof(const StdString *), _Alignof(const StdString *));
	for (size_t k = 0, k_len = lines->len; k < k_len; k++)
	{
		const StdString *line = &STD_VEC_AT(lines, StdString, k);
		size_t		at = 0;
		bool		held = false;

		while (at < set.len)
		{
			const StdString *known = STD_VEC_AT(&set, const StdString *, at);

			if (known->len == line->len && memcmp(known->ptr, line->ptr, line->len) == 0)
			{
				held = true;
				break;
			}
			if (chunks_packs_check_tests_string_lt(line, known))
				break;
			at++;
		}
		if (!held)
			std_vec_insert(&set, at, &line, sizeof(line), _Alignof(const StdString *), RUST_FILE, 0, 0);
	}
	return set;
}

/* chunks_packs_check_tests_Set_Debug_fmt is a BTreeSet<String>'s Debug, of the lines a set borrows. */
static bool
chunks_packs_check_tests_Set_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *set = value;
	StdDebugList s = std_fmt_debug_set(f);

	for (size_t k = 0, k_len = set->len; k < k_len; k++)
		std_DebugSet_entry(&s, STD_VEC_AT(set, const StdString *, k), std_String_Debug_fmt);
	return std_DebugSet_finish(&s);
}

/*
 * test_chunks_packs_check_tests_the_files_a_crash_leaves_are_settled_or_noted:
 * a pack a writer left unfinished and a run no runs file names are what the
 * next start removes, and a pack no pack table lists is a note.
 */
void
test_chunks_packs_check_tests_the_files_a_crash_leaves_are_settled_or_noted(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	chunks_TupleAddressLocator_t placed[2];
	StdPathBuf	dir = chunks_packs_check_tests_made(io, 106, placed);
	StdPathBuf	left[3];
	static const char *const names[] = {"0123456789abcdef0123456789abcdef.pack.tmp", "00.idx", "ff.pack"};
	ChunksPacksCheckChecked checked;
	StdVec		settled;
	StdVec		notes;
	StdVec		nothing;
	StdVec		got_set;
	StdVec		want_set;
	bool		same;

	for (size_t k = 0; k < lengthof(names); k++)
		std_path_join(dir.bytes.ptr, dir.bytes.len, names[k], strlen(names[k]), &left[k]);
	for (size_t k = 0; k < lengthof(left); k++)
		chunks_packs_check_tests_write_file(&left[k], (const uint8 *) "left", 4, 499, 43);
	chunks_packs_check_tests_checking(io, 106, &checked);
	settled = chunks_packs_check_tests_path_line("\"", &left[0],
												 "\" is a pack a writer left unfinished, which the next start removes");
	chunks_packs_check_path_said(&settled, "run \"", &left[1],
								 "\" is named by no runs file, which the next start removes");
	nothing = chunks_packs_check_tests_none();
	chunks_packs_check_tests_lines_eq(&checked.found, &nothing, 506);
	got_set = chunks_packs_check_tests_set(&checked.settled);
	want_set = chunks_packs_check_tests_set(&settled);
	same = got_set.len == want_set.len;
	for (size_t k = 0; same && k < want_set.len; k++)
	{
		const StdString *got = STD_VEC_AT(&got_set, const StdString *, k);
		const StdString *want = STD_VEC_AT(&want_set, const StdString *, k);

		same = got->len == want->len && memcmp(got->ptr, want->ptr, got->len) == 0;
	}
	STD_ASSERT_EQ(507, 9, same, &got_set, chunks_packs_check_tests_Set_Debug_fmt, &want_set,
				  chunks_packs_check_tests_Set_Debug_fmt);
	std_vec_free_Checked(&want_set, sizeof(const StdString *), _Alignof(const StdString *));
	std_vec_free_Checked(&got_set, sizeof(const StdString *), _Alignof(const StdString *));
	chunks_packs_check_Lines_drop(&settled);
	notes = chunks_packs_check_tests_path_line("pack \"", &left[2], "\" is in no pack table");
	chunks_packs_check_tests_lines_eq(&checked.notes, &notes, 508);
	chunks_packs_check_Checked_drop(&checked);
	for (size_t k = 0; k < lengthof(left); k++)
		std_vec_free_Checked(&left[k].bytes, 1, 1);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	simc_TestIo_drop(&test_io);
}

/*
 * test_chunks_packs_check_tests_a_pack_in_no_run_is_settled: a pack no run
 * covers is what the next start settles by writing its run again, and its
 * run's file, which the runs file no longer names, is what the next start
 * removes.
 */
void
test_chunks_packs_check_tests_a_pack_in_no_run_is_settled(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	chunks_TupleAddressLocator_t placed[2];
	StdPathBuf	dir = chunks_packs_check_tests_made(io, 107, placed);
	StdPathBuf	runs_path;
	ChunksPacksRuns runs;
	StdVec		whole;
	ChunksPacksCheckChecked checked;
	StdVec		nothing;
	uint64		packs[2] = {placed[0].f1.pack, placed[1].f1.pack};

	std_path_join(dir.bytes.ptr, dir.bytes.len, CHUNKS_PACKS_RUNS, strlen(CHUNKS_PACKS_RUNS), &runs_path);
	chunks_packs_Runs_Default_default(&runs);
	CHUNKS_PACKS_VIEW_RUNS_LIST_OPS.whole(&runs, &whole);
	chunks_packs_check_tests_written_appended(io, &runs_path, &whole, 518, 96);
	std_vec_free_Checked(&whole, 1, 1);
	chunks_packs_Runs_drop(&runs);
	chunks_packs_check_tests_checking(io, 107, &checked);
	nothing = chunks_packs_check_tests_none();
	chunks_packs_check_tests_lines_eq(&checked.found, &nothing, 520);
	if (packs[1] < packs[0])
	{
		uint64		swapped = packs[0];

		packs[0] = packs[1];
		packs[1] = swapped;
	}
	for (size_t k = 0; k < lengthof(packs); k++)
	{
		StdString	line;
		bool		held = false;
		StdFmtPiece pieces[] = {STD_FMT_LIT("pack "), STD_FMT_ARG(0),
		STD_FMT_LIT(" is in no run, whose run the next start writes again from the pack")};
		StdFmtArg	args[] = {{&packs[k], std_u64_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_fmt_format_Checked(&line, &arguments);
		for (size_t n = 0; n < checked.settled.len && !held; n++)
		{
			const StdString *got = &STD_VEC_AT(&checked.settled, StdString, n);

			held = got->len == line.len && memcmp(got->ptr, line.ptr, got->len) == 0;
		}
		std_String_drop(&line);
		if (!held)
		{
			StdFmtPiece shown_pieces[] = {STD_FMT_ARG(0)};
			StdFmtArg	shown_args[] = {{&checked.settled, chunks_packs_check_tests_Lines_Debug_fmt}};
			StdFmtArguments shown = {shown_pieces, lengthof(shown_pieces), shown_args};

			std_panic_fmt(RUST_FILE, 525, 13, &shown);
		}
	}
	chunks_packs_check_Checked_drop(&checked);
	std_vec_free_Checked(&runs_path.bytes, 1, 1);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	simc_TestIo_drop(&test_io);
}

/*
 * test_chunks_packs_check_tests_chunks_no_run_names_are_found_and_stale_entries_noted:
 * a run that names none of its pack's chunks leaves them found missing by a
 * read by address, and a run entry naming a pack no table lists is a note.
 */
void
test_chunks_packs_check_tests_chunks_no_run_names_are_found_and_stale_entries_noted(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	chunks_TupleAddressLocator_t placed[2];
	StdPathBuf	dir = chunks_packs_check_tests_made(io, 108, placed);
	ChunksAddressAddress address = placed[0].f0;
	ChunksTrailerLocator locator = placed[0].f1;
	StdVec		runs = chunks_packs_check_tests_files(&dir, ".idx");
	const StdPathBuf *run = NULL;
	ChunksIndexRunLocator written;
	chunks_TupleAddressLocator_t entry[1];
	ChunksErrorError error;
	ChunksPacksCheckChecked checked;
	StdVec		found;
	StdVec		stale;

	for (size_t k = 0; k < runs.len && run == NULL; k++)
	{
		const StdPathBuf *candidate = &STD_VEC_AT(&runs, StdPathBuf, k);
		ChunksIndexRunLocator opened;
		StdVec		entries;

		if (!chunks_index_RunLocator_open(HOST_IO_IO_READ_DOORS(io), candidate->bytes.ptr, candidate->bytes.len,
										  &opened, &error))
			chunks_packs_tests_unwrapped(RUST_FILE, &error, 538, 43);
		if (!chunks_index_RunLocator_entries(&opened, HOST_IO_IO_READ_DOORS(io), &entries, &error))
			chunks_packs_tests_unwrapped(RUST_FILE, &error, 538, 64);
		chunks_index_RunLocator_drop(&opened);
		for (size_t n = 0; n < entries.len && run == NULL; n++)
			if (chunks_address_Address_PartialEq_eq(&STD_VEC_AT(&entries, chunks_TupleAddressLocator_t, n).f0, &address))
				run = candidate;
		std_vec_free_Checked(&entries, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
	}
	if (run == NULL)
		STD_OPTION_UNWRAP_FAILED(540, 23);
	chunks_packs_check_tests_removed_file(run, 541, 36);
	entry[0].f0 = address;
	entry[0].f1 = chunks_trailer_Locator_at(99, chunks_trailer_Locator_offset(&locator),
											chunks_trailer_Locator_length(&locator));
	if (!chunks_index_RunLocator_write(HOST_IO_IO_IO_DOORS(io), run->bytes.ptr, run->bytes.len, entry, lengthof(entry),
									   &written, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 542, 102);
	chunks_index_RunLocator_drop(&written);
	chunks_packs_check_tests_checking(io, 108, &checked);
	{
		StdFmtPiece pieces[] = {STD_FMT_LIT("pack "), STD_FMT_ARG(0), STD_FMT_LIT(" holds 1 chunk no run names, "),
		STD_FMT_ARG(1), STD_FMT_LIT(", so a read of it by its address misses it")};
		StdFmtArg	args[] = {{&locator.pack, std_u64_Display_fmt}, {&address, chunks_address_Address_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		found = chunks_packs_check_tests_one(&arguments);
	}
	chunks_packs_check_tests_lines_eq(&checked.found, &found, 548);
	stale = chunks_packs_check_tests_path_line("1 entry of run \"", run,
											   "\" names a pack the pack table doesn't list, which the run's next "
											   "merge drops");
	chunks_packs_check_tests_lines_eq(&checked.notes, &stale, 553);
	chunks_packs_check_Checked_drop(&checked);
	chunks_packs_check_tests_Paths_drop(&runs);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	simc_TestIo_drop(&test_io);
}

/*
 * test_chunks_packs_check_tests_a_tablespace_whose_directory_is_missing_is_noted:
 * a tablespace the pack table names runs in whose directory is missing is a
 * note, and its packs aren't checked.
 */
void
test_chunks_packs_check_tests_a_tablespace_whose_directory_is_missing_is_noted(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	chunks_TupleAddressLocator_t placed[2];
	StdPathBuf	dir = chunks_packs_check_tests_made(io, 109, placed);
	StdPathBuf	path;
	ChunksPacksViewReadTable table;
	ChunksErrorError error;
	chunks_TupleU32Tail_t tail = {.f0 = 5};
	size_t		at = 0;
	StdVec		whole;
	ChunksPacksCheckChecked checked;
	StdPathBuf	missing = host_tablespace_dir(109, 5);
	StdVec		none = chunks_packs_check_tests_none();
	StdVec		note;

	std_path_join(dir.bytes.ptr, dir.bytes.len, CHUNKS_PACKS_TABLE, strlen(CHUNKS_PACKS_TABLE), &path);
	chunks_packs_Table_Default_default(&table.list);
	if (!chunks_packs_open_read_whole(&CHUNKS_PACKS_VIEW_TABLE_LIST_OPS, HOST_IO_IO_READ_DOORS(io), path.bytes.ptr,
									  path.bytes.len, &table.list, &table.read, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 563, 80);
	while (at < table.list.runs.len && STD_VEC_AT(&table.list.runs, chunks_TupleU32Tail_t, at).f0 < 5)
		at++;
	if (at < table.list.runs.len && STD_VEC_AT(&table.list.runs, chunks_TupleU32Tail_t, at).f0 == 5)
		STD_VEC_AT(&table.list.runs, chunks_TupleU32Tail_t, at) = tail;
	else
		std_vec_insert(&table.list.runs, at, &tail, sizeof(tail), _Alignof(chunks_TupleU32Tail_t), RUST_FILE, 0, 0);
	CHUNKS_PACKS_VIEW_TABLE_LIST_OPS.whole(&table.list, &whole);
	chunks_packs_check_tests_written_appended(io, &path, &whole, 565, 81);
	std_vec_free_Checked(&whole, 1, 1);
	chunks_packs_check_tests_checking(io, 109, &checked);
	{
		uint32		space = 5;
		StdStr		shown = {missing.bytes.ptr, missing.bytes.len};
		StdFmtPiece pieces[] = {STD_FMT_LIT("tablespace "), STD_FMT_ARG(0), STD_FMT_LIT("'s directory \""),
		STD_FMT_ARG(1), STD_FMT_LIT("\" is missing, so its packs are not checked")};
		StdFmtArg	args[] = {{&space, std_u32_Display_fmt}, {&shown, std_path_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		note = chunks_packs_check_tests_one(&arguments);
	}
	if (!chunks_packs_check_tests_lines_same(&checked.found, &none) ||
		!chunks_packs_check_tests_lines_same(&checked.notes, &note))
	{
		StdTupleRef got = STD_TUPLE(2, STD_ITEM(&checked.found, chunks_packs_check_tests_Lines_Debug_fmt),
									STD_ITEM(&checked.notes, chunks_packs_check_tests_Lines_Debug_fmt));
		StdTupleRef want = STD_TUPLE(2, STD_ITEM(&none, chunks_packs_check_tests_Lines_Debug_fmt),
									 STD_ITEM(&note, chunks_packs_check_tests_Lines_Debug_fmt));

		std_assert_failed(RUST_FILE, 570, 9, "==", &got, std_tuple_Debug_fmt, &want, std_tuple_Debug_fmt, NULL);
	}
	chunks_packs_check_Lines_drop(&note);
	chunks_packs_check_Checked_drop(&checked);
	std_vec_free_Checked(&missing.bytes, 1, 1);
	chunks_packs_Table_drop(&table.list);
	std_vec_free_Checked(&path.bytes, 1, 1);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	simc_TestIo_drop(&test_io);
}

#endif							/* DOLTGRES_TEST_CHUNKS */
