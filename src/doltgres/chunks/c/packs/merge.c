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
 * The merge of a tablespace's address-index runs (chunks' packs/merge.rs,
 * design item 6), a long operation under the pack record's merge flag: the
 * merged run is written with no lock held, and takes the place of the runs it
 * merged in one change under the lists lock, only if those runs are still
 * listed together.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/file.h"
#include "doltgres/chunks/index.h"
#include "doltgres/chunks/packs.h"
#include "doltgres/chunks/packs/lists.h"
#include "doltgres/chunks/packs/merge.h"
#include "doltgres/chunks/packs/shared.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/num.h"
#include "doltgres/std/path.h"
#include "doltgres/std/slice.h"
#include "doltgres/std/str.h"
#include "doltgres/std/sync.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"

#ifdef DOLTGRES_TEST_CHUNKS
#include "doltgres/chunks/packs/fixtures.h"
#include "doltgres/simc/simc.h"
#include "doltgres/std/test.h"
#endif

#define RUST_FILE "chunks" STD_SEP "src" STD_SEP "packs" STD_SEP "merge.rs"

/*
 * chunks_packs_merge_covered returns the numbers of the packs a runs file's
 * line says its run's entries name: those of its second field, separated by
 * commas, that parse as numbers.
 */
void
chunks_packs_merge_covered(const char *line, size_t line_len, StdVec *result)
{
	const char *start = memchr(line, ':', line_len);
	const char *end;

	std_vec_new(result, sizeof(uint64), _Alignof(uint64));
	if (start == NULL)
		return;
	start++;
	end = memchr(start, ':', line_len - (size_t) (start - line));
	if (end == NULL)
		end = line + line_len;
	while (true)
	{
		const char *comma = memchr(start, ',', (size_t) (end - start));
		const char *stop = comma == NULL ? end : comma;
		uint64		number;
		StdParseIntError error;

		if (std_str_parse_u64(start, (size_t) (stop - start), &number, &error))
			std_vec_push_Checked(result, &number, sizeof(number), _Alignof(uint64));
		if (comma == NULL)
			return;
		start = comma + 1;
	}
}

/*
 * chunks_packs_merge_due returns the first tablespace whose address index is
 * due a merge, as the view last refreshed lists it, with the places of the
 * runs to merge among its runs.
 */
bool
chunks_packs_merge_due(const ChunksPacksViewView *view, uint32 *space, size_t *from, size_t *to)
{
	for (size_t k = 0, k_len = view->indexes.len; k < k_len; k++)
	{
		const chunks_TupleU32AddressIndex_t *index = &STD_VEC_AT(&view->indexes, chunks_TupleU32AddressIndex_t, k);

		if (chunks_index_AddressIndexLocator_to_merge(&index->f1, from))
		{
			(void) chunks_index_AddressIndexLocator_runs(&index->f1, to);
			*space = index->f0;
			return true;
		}
	}
	return false;
}

/* chunks_packs_merge_has reports whether a sorted vector of numbers holds one. */
static bool
chunks_packs_merge_has(const StdVec *numbers, uint64 number)
{
	size_t		at;

	return std_slice_binary_search(numbers->ptr, numbers->len, sizeof(uint64), &number, std_u64_Ord_cmp, &at);
}

/*
 * chunks_packs_merge_merge_runs_frame_0_t is merge_runs' state across its
 * pad: the tablespace's directory, the runs' paths and file names, the
 * merged entries and the numbers they name, the merged run's name and path,
 * the change, and the line and record of its runs.
 */
typedef struct chunks_packs_merge_merge_runs_frame_0_t
{
	ChunksPacksViewView *view;
	HostSharedRecord record;
	HostSharedFlagIo *io;
	bool	   *has_result;
	uint32	   *space;
	StdVec	   *result;
	ChunksErrorError *error;
	bool		ok;
	StdPathBuf	dir;
	bool		dir_live;
	StdVec		merged;
	bool		merged_live;
	StdVec		entries;
	bool		entries_live;
	StdVec		named;
	bool		named_live;
	StdString	name;
	bool		name_live;
	StdPathBuf	path;
	bool		path_live;
	StdVec		names;
	bool		names_live;
	ChunksPacksListsChange change;
	bool		change_live;
	StdString	line;
	bool		line_live;
	StdVec		record_bytes;
	bool		record_bytes_live;
} chunks_packs_merge_merge_runs_frame_0_t;

/* chunks_packs_merge_merge_runs_pad_0 is merge_runs' pad: what it holds, the latest first. */
static void
chunks_packs_merge_merge_runs_pad_0(void *frame_)
{
	chunks_packs_merge_merge_runs_frame_0_t *frame = frame_;

	if (frame->record_bytes_live)
	{
		frame->record_bytes_live = false;
		std_vec_free_Checked(&frame->record_bytes, 1, 1);
	}
	if (frame->line_live)
	{
		frame->line_live = false;
		std_vec_free_Checked(&frame->line, 1, 1);
	}
	if (frame->change_live)
	{
		frame->change_live = false;
		chunks_packs_lists_Change_drop(&frame->change);
	}
	if (frame->names_live)
	{
		frame->names_live = false;
		std_vec_drop_Checked(&frame->names, sizeof(StdString), _Alignof(StdString), std_String_drop);
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
	if (frame->named_live)
	{
		frame->named_live = false;
		std_vec_free_Checked(&frame->named, sizeof(uint64), _Alignof(uint64));
	}
	if (frame->entries_live)
	{
		frame->entries_live = false;
		std_vec_free_Checked(&frame->entries, sizeof(chunks_TupleAddressLocator_t),
							 _Alignof(chunks_TupleAddressLocator_t));
	}
	if (frame->merged_live)
	{
		frame->merged_live = false;
		std_vec_drop_Checked(&frame->merged, sizeof(StdPathBuf), _Alignof(StdPathBuf), chunks_pack_PathBuf_drop);
	}
	if (frame->dir_live)
	{
		frame->dir_live = false;
		std_vec_free_Checked(&frame->dir.bytes, 1, 1);
	}
}

/* chunks_packs_merge_merged_lists reports whether the runs merged hold the run a runs file's line names in the directory. */
static bool
chunks_packs_merge_merged_lists(const chunks_packs_merge_merge_runs_frame_0_t *frame, const StdString *line)
{
	StdStr		name = chunks_packs_run_name(line->ptr, line->len);
	StdPathBuf	joined;
	bool		found = false;

	std_path_join(frame->dir.bytes.ptr, frame->dir.bytes.len, name.ptr, name.len, &joined);
	for (size_t k = 0, k_len = frame->merged.len; k < k_len && !found; k++)
	{
		const StdPathBuf *merged = &STD_VEC_AT(&frame->merged, StdPathBuf, k);

		found = std_path_PartialEq_eq(merged->bytes.ptr, merged->bytes.len, joined.bytes.ptr, joined.bytes.len);
	}
	std_vec_free_Checked(&joined.bytes, 1, 1);
	return found;
}

/*
 * chunks_packs_merge_lost reports whether a listed pack that the merged
 * runs' lines say they cover has no entry among the merged entries, and
 * which.
 */
static bool
chunks_packs_merge_lost(const chunks_packs_merge_merge_runs_frame_0_t *frame, const ChunksPacksRuns *runs,
						uint64 *result)
{
	for (size_t k = 0, k_len = runs->lines.len; k < k_len; k++)
	{
		const StdString *line = &STD_VEC_AT(&runs->lines, StdString, k);
		StdVec		covered;
		bool		lost = false;

		if (!chunks_packs_merge_merged_lists(frame, line))
			continue;
		chunks_packs_merge_covered(line->ptr, line->len, &covered);
		for (size_t c = 0, c_len = covered.len; c < c_len && !lost; c++)
		{
			uint64		number = STD_VEC_AT(&covered, uint64, c);

			lost = chunks_packs_view_View_pack(frame->view, number) != NULL &&
				!chunks_packs_merge_has(&frame->named, number);
			*result = number;
		}
		std_vec_free_Checked(&covered, sizeof(uint64), _Alignof(uint64));
		if (lost)
			return true;
	}
	return false;
}

/* chunks_packs_merge_line returns the merged run's line of the runs file: its name, then the numbers its entries name. */
static void
chunks_packs_merge_line(const StdString *name, const StdVec *named, StdString *result)
{
	std_string_from_Checked(result, name->ptr, name->len);
	std_string_push_str_Checked(result, ":", 1);
	for (size_t k = 0, k_len = named->len; k < k_len; k++)
	{
		StdString	number;
		StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&STD_VEC_AT(named, uint64, k), std_u64_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		if (k > 0)
			std_string_push_str_Checked(result, ",", 1);
		std_fmt_format_Checked(&number, &arguments);
		std_string_push_str_Checked(result, number.ptr, number.len);
		std_vec_free_Checked(&number, 1, 1);
	}
}

/*
 * chunks_packs_merge_merge_runs_body_0 is merge_runs' body: the due runs'
 * entries merged into a run written with no lock held, which takes their
 * place in one change under the lists lock while they are listed together.
 */
static void
chunks_packs_merge_merge_runs_body_0(void *frame_)
{
	chunks_packs_merge_merge_runs_frame_0_t *frame = frame_;
	ChunksPacksViewView *view = frame->view;
	HostIoIoDoors io = HOST_SHARED_FLAG_IO_IO_DOORS(frame->io);
	uint32		space;
	size_t		from;
	size_t		to;
	const ChunksIndexAddressIndexLocator *index;
	const ChunksPacksViewReadRuns *runs;
	StdArcInner *const *all;
	size_t		all_len;
	size_t		kept = 0;
	uint64		number;
	ChunksIndexRunLocator run;
	HostSharedLongGuard lists;
	HostError	host;

	frame->ok = false;
	*frame->has_result = false;
	if (!chunks_packs_merge_due(view, &space, &from, &to))
		goto none;
	index = chunks_packs_view_View_index(view, space);
	runs = chunks_packs_view_runs_get(&view->runs, space);
	if (index == NULL || runs == NULL)
		goto none;
	chunks_packs_view_View_space_dir(view, space, &frame->dir);
	frame->dir_live = true;
	all = chunks_index_AddressIndexLocator_runs(index, &all_len);
	std_range_check(from, to, all_len, RUST_FILE, 58, 32);
	std_vec_new(&frame->merged, sizeof(StdPathBuf), _Alignof(StdPathBuf));
	frame->merged_live = true;
	for (size_t k = from; k < to; k++)
	{
		const ChunksIndexRunLocator *merged = std_arc_Deref_deref(all[k], _Alignof(ChunksIndexRunLocator));
		size_t		path_len;
		const char *path = chunks_index_RunLocator_path(merged, &path_len);
		StdPathBuf	buf;

		std_path_to_path_buf(path, path_len, &buf);
		std_vec_push_Checked(&frame->merged, &buf, sizeof(buf), _Alignof(StdPathBuf));
	}
	for (size_t k = 0, k_len = runs->list.lines.len; k < k_len; k++)
	{
		const StdString *line = &STD_VEC_AT(&runs->list.lines, StdString, k);

		if (chunks_packs_lossy(line->ptr, line->len) && chunks_packs_merge_merged_lists(frame, line))
			goto none;
	}
	if (!chunks_index_AddressIndexLocator_merged(index, HOST_IO_IO_DOORS_READ_DOORS(io), from, to, &frame->entries,
												 frame->error))
		goto done;
	frame->entries_live = true;
	for (size_t k = 0, k_len = frame->entries.len; k < k_len; k++)
	{
		chunks_TupleAddressLocator_t entry = STD_VEC_AT(&frame->entries, chunks_TupleAddressLocator_t, k);

		if (chunks_packs_view_View_pack(view, entry.f1.pack) != NULL)
			STD_VEC_AT(&frame->entries, chunks_TupleAddressLocator_t, kept++) = entry;
	}
	frame->entries.len = kept;
	std_vec_new(&frame->named, sizeof(uint64), _Alignof(uint64));
	frame->named_live = true;
	for (size_t k = 0, k_len = frame->entries.len; k < k_len; k++)
	{
		uint64		pack = STD_VEC_AT(&frame->entries, chunks_TupleAddressLocator_t, k).f1.pack;
		size_t		at;

		if (!std_slice_binary_search(frame->named.ptr, frame->named.len, sizeof(uint64), &pack, std_u64_Ord_cmp,
									 &at))
			std_vec_insert(&frame->named, at, &pack, sizeof(pack), _Alignof(uint64), RUST_FILE, 62, 72);
	}
	if (chunks_packs_merge_lost(frame, &runs->list, &number))
	{
		StdString	message;
		StdFmtPiece pieces[] = {STD_FMT_LIT("a merge of address-index runs would leave pack number "), STD_FMT_ARG(0),
		STD_FMT_LIT(", which its runs cover, in no run")};
		StdFmtArg	args[] = {{&number, std_u64_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_fmt_format_Checked(&message, &arguments);
		*frame->error = chunks_error_Error_corrupt(&message);
		goto done;
	}
	{
		ChunksPackPackId id = chunks_pack_PackId_new();

		frame->name = chunks_pack_PackId_file_name(&id, "idx", 3);
		frame->name_live = true;
	}
	std_path_join(frame->dir.bytes.ptr, frame->dir.bytes.len, frame->name.ptr, frame->name.len, &frame->path);
	frame->path_live = true;
	if (!chunks_index_written_run(io, frame->path.bytes.ptr, frame->path.bytes.len, frame->entries.ptr,
								  frame->entries.len, &run, frame->error))
		goto done;
	chunks_index_RunLocator_drop(&run);

	std_vec_new(&frame->names, sizeof(StdString), _Alignof(StdString));
	frame->names_live = true;
	for (size_t k = 0, k_len = frame->merged.len; k < k_len; k++)
	{
		const StdPathBuf *merged = &STD_VEC_AT(&frame->merged, StdPathBuf, k);
		StdStr		file_name;
		StdCowStr	lossy;
		StdString	owned;

		if (!std_path_file_name(merged->bytes.ptr, merged->bytes.len, &file_name))
			continue;
		std_path_to_string_lossy(file_name.ptr, file_name.len, &lossy);
		std_CowStr_into_owned(&lossy, &owned);
		std_vec_push_Checked(&frame->names, &owned, sizeof(owned), _Alignof(StdString));
	}
	if (!host_shared_FlagIo_lock_long(frame->io, CHUNKS_PACKS_SHARED_LISTS_LOCK_INDEX, &lists, &host))
	{
		*frame->error = chunks_error_Error_FromError_from(&host);
		goto done;
	}
	if (!chunks_packs_lists_Change_begin(view, frame->record, &lists, &frame->change, frame->error))
		goto done;
	frame->change_live = true;
	if (!chunks_packs_lists_Change_flush(&frame->change, frame->error))
		goto done;
	if (!chunks_packs_lists_Change_lists_together(&frame->change, space, frame->names.ptr, frame->names.len))
	{
		frame->change_live = false;
		chunks_packs_lists_Change_drop(&frame->change);
		if (!chunks_file_remove_file(io, frame->path.bytes.ptr, frame->path.bytes.len, frame->error))
			goto done;
		goto none;
	}
	chunks_packs_merge_line(&frame->name, &frame->named, &frame->line);
	frame->line_live = true;
	chunks_packs_Runs_replaced(frame->names.ptr, frame->names.len, frame->line.ptr, frame->line.len,
							   &frame->record_bytes);
	frame->record_bytes_live = true;
	if (!chunks_packs_lists_Change_change_runs(&frame->change, space, frame->record_bytes.ptr, frame->record_bytes.len,
											   frame->error))
		goto done;
	frame->change_live = false;
	if (!chunks_packs_lists_Change_install(&frame->change, frame->error))
		goto done;
	*frame->has_result = true;
	*frame->space = space;
	*frame->result = frame->names;
	frame->names_live = false;
none:
	frame->ok = true;
done:
	chunks_packs_merge_merge_runs_pad_0(frame);
}

/*
 * chunks_packs_merge_merge_runs merges the newest runs of the first
 * tablespace whose address index is due a merge, as the view last refreshed
 * lists them, under the merge flag its caller holds. It returns the
 * tablespace and the file names of the runs the merged run replaced, which
 * go through the removal queue, or nothing when no merge is due or the runs
 * are no longer listed together once every deferred add's run is, as a
 * retirement meanwhile leaves them, in which case the merged run's file is
 * removed, or when one of the runs is marked as written from a pack whose
 * address table didn't read whole, which a merge would leave unmarked. It
 * fails rather than replace runs when a listed pack they cover has no entry
 * in them, as a lost entry would leave it.
 */
bool
chunks_packs_merge_merge_runs(ChunksPacksViewView *view, HostSharedRecord record, HostSharedFlagIo *io,
							  bool *has_result, uint32 *space, StdVec *names, ChunksErrorError *error)
{
	chunks_packs_merge_merge_runs_frame_0_t frame = {.view = view, .record = record, .io = io,
	.has_result = has_result, .space = space, .result = names, .error = error};

	std_landing(&frame, chunks_packs_merge_merge_runs_body_0, chunks_packs_merge_merge_runs_pad_0);
	return frame.ok;
}

#ifdef DOLTGRES_TEST_CHUNKS

/* ChunksPacksMergeTestsMerged is a merge's answer: whether it replaced runs, and the tablespace and their file names. */
typedef struct ChunksPacksMergeTestsMerged
{
	bool		has;
	uint32		space;
	StdVec		names;
} ChunksPacksMergeTestsMerged;

/* chunks_packs_merge_tests_Names_Debug_fmt is a Vec<String>'s Debug. */
static bool
chunks_packs_merge_tests_Names_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *names = value;
	StdDebugList l = std_fmt_debug_list(f);

	for (size_t k = 0, k_len = names->len; k < k_len; k++)
		std_DebugList_entry(&l, &STD_VEC_AT(names, StdString, k), std_String_Debug_fmt);
	return std_DebugList_finish(&l);
}

/* chunks_packs_merge_tests_Merged_Debug_fmt is a merge's answer's Debug, the Rust's Option<(u32, Vec<String>)>. */
static bool
chunks_packs_merge_tests_Merged_Debug_fmt(const void *value, StdFormatter *f)
{
	const ChunksPacksMergeTestsMerged *merged = value;
	StdTupleRef pair = STD_TUPLE(2, STD_ITEM(&merged->space, std_u32_Debug_fmt),
								 STD_ITEM(&merged->names, chunks_packs_merge_tests_Names_Debug_fmt));
	StdOptionRef some = {merged->has, STD_ITEM(&pair, std_tuple_Debug_fmt)};

	return std_option_Debug_fmt(&some, f);
}

/* chunks_packs_merge_tests_Merged_drop frees a merge's answer. */
static void
chunks_packs_merge_tests_Merged_drop(ChunksPacksMergeTestsMerged *merged)
{
	if (merged->has)
		std_vec_drop_Checked(&merged->names, sizeof(StdString), _Alignof(StdString), std_String_drop);
}

/* chunks_packs_merge_tests_merge_stale runs a merge of a view's runs as the view lists them, without bringing it up to date first. */
static bool
chunks_packs_merge_tests_merge_stale(HostIoIo *io, HostSharedRecord record, ChunksPacksViewView *view,
									 ChunksPacksMergeTestsMerged *result, ChunksErrorError *error)
{
	HostSharedFlagGuard merging;
	HostSharedFlagIo flag_io;
	HostError	host;
	bool		ok;

	result->has = false;
	if (!host_shared_Record_flag(record, HOST_IO_IO_TAKES_SHORT(io), CHUNKS_PACKS_SHARED_MERGING_INDEX, &merging,
								 &host))
	{
		*error = chunks_error_Error_FromError_from(&host);
		return false;
	}
	flag_io = host_shared_FlagGuard_io(&merging);
	ok = chunks_packs_merge_merge_runs(view, record, &flag_io, &result->has, &result->space, &result->names, error);
	host_shared_FlagGuard_drop(&merging);
	return ok;
}

/* chunks_packs_merge_tests_merged runs a merge of a view's runs under the record's merge flag. */
static bool
chunks_packs_merge_tests_merged(HostIoIo *io, HostSharedRecord record, ChunksPacksViewView *view,
								ChunksPacksMergeTestsMerged *result, ChunksErrorError *error)
{
	result->has = false;
	if (!chunks_packs_view_View_refresh(view, io, record, error))
		return false;
	return chunks_packs_merge_tests_merge_stale(io, record, view, result, error);
}

/* chunks_packs_merge_tests_none asserts at a line that a merge, brought up to date first or not, replaced nothing, which unwraps at a column. */
static void
chunks_packs_merge_tests_none(HostIoIo *io, HostSharedRecord record, ChunksPacksViewView *view, bool stale,
							  uint32 line, uint32 col)
{
	ChunksPacksMergeTestsMerged merged;
	ChunksPacksMergeTestsMerged none = {false};
	ChunksErrorError error;
	bool		ok = stale ? chunks_packs_merge_tests_merge_stale(io, record, view, &merged, &error) :
		chunks_packs_merge_tests_merged(io, record, view, &merged, &error);

	if (!ok)
		chunks_packs_tests_unwrapped(RUST_FILE, &error, line, col);
	if (merged.has)
		std_assert_failed(RUST_FILE, line, 9, "==", &merged, chunks_packs_merge_tests_Merged_Debug_fmt, &none,
						  chunks_packs_merge_tests_Merged_Debug_fmt, NULL);
}

/* chunks_packs_merge_tests_runs_eq asserts at a line that a view's index of the default tablespace holds as many runs as given. */
static void
chunks_packs_merge_tests_runs_eq(const ChunksPacksViewView *view, size_t want, uint32 line, uint32 assert_col,
								 uint32 unwrap_col)
{
	size_t		len;

	(void) chunks_packs_tests_runs(RUST_FILE, view, 0, line, unwrap_col, &len);
	if (len != want)
		std_assert_failed(RUST_FILE, line, assert_col, "==", &len, std_usize_Debug_fmt, &want, std_usize_Debug_fmt,
						  NULL);
}

/* chunks_packs_merge_tests_found asserts at a line that a view finds an address at a place. */
static void
chunks_packs_merge_tests_found(ChunksPacksViewView *view, HostIoIo *io, const ChunksAddressAddress *address,
							   const ChunksTrailerLocator *at, uint32 line, uint32 assert_col, uint32 unwrap_col)
{
	ChunksTrailerLocator found;
	ChunksErrorError error;
	bool		has;

	if (!chunks_packs_reads_View_find(view, HOST_IO_IO_READ_DOORS(io), address, &has, &found, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, line, unwrap_col);
	if (!has || !chunks_trailer_Locator_PartialEq_eq(&found, at))
	{
		StdOptionRef got = {has, STD_ITEM(&found, chunks_trailer_Locator_Debug_fmt)};
		StdOptionRef want = {true, STD_ITEM(at, chunks_trailer_Locator_Debug_fmt)};

		std_assert_failed(RUST_FILE, line, assert_col, "==", &got, std_option_Debug_fmt, &want, std_option_Debug_fmt,
						  NULL);
	}
}

/* chunks_packs_merge_tests_added is a change's body that adds a pack, deferred or not, which unwraps at a line and column. */
static void
chunks_packs_merge_tests_added(ChunksPacksListsChange *change, const ChunksPacksTableEntry *entry, bool defer,
							   uint32 line, uint32 col)
{
	ChunksErrorError error;

	if (!chunks_packs_lists_Change_add(change, entry, defer, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, line, col);
}

/* chunks_packs_merge_tests_add_first is a_merge_puts_one_run_in_the_place_of_the_runs_it_merged's first change: the first pack's add. */
static void
chunks_packs_merge_tests_add_first(ChunksPacksListsChange *change, void *captures)
{
	chunks_packs_merge_tests_added(change, captures, false, 125, 74);
}

/* chunks_packs_merge_tests_add_second is a_merge_puts_one_run_in_the_place_of_the_runs_it_merged's second change: the second pack's deferred add. */
static void
chunks_packs_merge_tests_add_second(ChunksPacksListsChange *change, void *captures)
{
	chunks_packs_merge_tests_added(change, captures, true, 127, 74);
}

/* chunks_packs_merge_tests_idx returns a pack's run's file name. */
static StdString
chunks_packs_merge_tests_idx(const ChunksPacksFixturesPacked *packed)
{
	return chunks_pack_PackId_file_name(&packed->entry.f2, "idx", 3);
}

/*
 * test_chunks_packs_merge_tests_a_merge_puts_one_run_in_the_place_of_the_runs_it_merged:
 * a merge puts one run in the place of the runs it merged, which every view
 * then reads each chunk through, and returns the files it replaced; with one
 * run left none is due.
 */
void
test_chunks_packs_merge_tests_a_merge_puts_one_run_in_the_place_of_the_runs_it_merged(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	StdPathBuf	dir;
	HostSharedOwned owned;
	HostSharedRecord record;
	ChunksPacksFixturesPacked first;
	ChunksPacksFixturesPacked second;
	ChunksPacksViewView views[2];
	ChunksPacksMergeTestsMerged replaced;
	ChunksPacksMergeTestsMerged want = {true, 0};
	ChunksErrorError error;
	StdString	name;
	bool		same;

	chunks_packs_fixtures_home(&dir);
	owned = chunks_packs_lists_tests_repository(io, &dir);
	record = host_shared_Owned_record(&owned);
	first = chunks_packs_fixtures_pack(io, &dir, 1);
	second = chunks_packs_fixtures_pack(io, &dir, 2);
	chunks_packs_fixtures_view(&views[0]);
	chunks_packs_fixtures_view(&views[1]);
	chunks_packs_lists_tests_changed(io, record, &views[0], chunks_packs_merge_tests_add_first, &first.entry);
	chunks_packs_merge_tests_none(io, record, &views[0], false, 126, 50);
	chunks_packs_lists_tests_changed(io, record, &views[0], chunks_packs_merge_tests_add_second, &second.entry);
	chunks_packs_tests_refreshed(RUST_FILE, &views[1], io, record, 128, 35);
	if (!chunks_packs_merge_tests_merged(io, record, &views[0], &replaced, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 130, 54);
	if (!replaced.has)
		STD_OPTION_UNWRAP_FAILED(130, 63);
	std_vec_new(&want.names, sizeof(StdString), _Alignof(StdString));
	name = chunks_packs_merge_tests_idx(&first);
	std_vec_push_Checked(&want.names, &name, sizeof(name), _Alignof(StdString));
	name = chunks_packs_merge_tests_idx(&second);
	std_vec_push_Checked(&want.names, &name, sizeof(name), _Alignof(StdString));
	same = replaced.space == 0 && replaced.names.len == want.names.len;
	for (size_t k = 0, k_len = want.names.len; same && k < k_len; k++)
	{
		const StdString *a = &STD_VEC_AT(&replaced.names, StdString, k);
		const StdString *b = &STD_VEC_AT(&want.names, StdString, k);

		same = a->len == b->len && memcmp(a->ptr, b->ptr, a->len) == 0;
	}
	if (!same)
		std_assert_failed(RUST_FILE, 131, 9, "==", &replaced, chunks_packs_merge_tests_Merged_Debug_fmt, &want,
						  chunks_packs_merge_tests_Merged_Debug_fmt, NULL);
	chunks_packs_merge_tests_Merged_drop(&want);
	chunks_packs_merge_tests_Merged_drop(&replaced);
	for (size_t k = 0; k < lengthof(views); k++)
	{
		StdVec		listed;
		size_t		two = 2;

		chunks_packs_tests_refreshed(RUST_FILE, &views[k], io, record, 133, 38);
		chunks_packs_merge_tests_runs_eq(&views[k], 1, 134, 13, 38);
		chunks_packs_merge_tests_found(&views[k], io, &first.address, &first.at, 135, 13, 44);
		chunks_packs_merge_tests_found(&views[k], io, &second.address, &second.at, 136, 13, 44);
		chunks_packs_view_View_listed(&views[k], &listed);
		if (listed.len != two)
			std_assert_failed(RUST_FILE, 137, 13, "==", &listed.len, std_usize_Debug_fmt, &two, std_usize_Debug_fmt,
							  NULL);
		std_vec_free_Checked(&listed, sizeof(ChunksPacksTableEntry), _Alignof(ChunksPacksTableEntry));
	}
	chunks_packs_merge_tests_none(io, record, &views[0], false, 139, 50);
	chunks_packs_view_View_drop(&views[1]);
	chunks_packs_view_View_drop(&views[0]);
	host_shared_Owned_drop(&owned);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	simc_TestIo_drop(&test_io);
}

/* ChunksPacksMergeTestsPacks is the packs a change's body takes. */
typedef struct ChunksPacksMergeTestsPacks
{
	const ChunksPacksFixturesPacked *first;
	const ChunksPacksFixturesPacked *second;
	const ChunksPacksFixturesPacked *third;
} ChunksPacksMergeTestsPacks;

/* chunks_packs_merge_tests_add_both is a_merge_whose_runs_are_no_longer_listed_together_replaces_nothing's first change: two packs' adds. */
static void
chunks_packs_merge_tests_add_both(ChunksPacksListsChange *change, void *captures)
{
	const ChunksPacksMergeTestsPacks *packs = captures;

	chunks_packs_merge_tests_added(change, &packs->first->entry, false, 154, 38);
	chunks_packs_merge_tests_added(change, &packs->second->entry, false, 155, 39);
}

/* chunks_packs_merge_tests_take_apart is a change's body that retires the first pack and removes its run. */
static void
chunks_packs_merge_tests_take_apart(ChunksPacksListsChange *change, void *captures)
{
	const ChunksPacksMergeTestsPacks *packs = captures;
	static const uint64 one[] = {1};
	StdString	name = chunks_packs_merge_tests_idx(packs->first);
	StdVec		record;
	ChunksErrorError error;

	if (!chunks_packs_lists_Change_retire(change, one, lengthof(one), &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 159, 33);
	chunks_packs_Runs_removed(&name, 1, &record);
	if (!chunks_packs_lists_Change_change_runs(change, 0, record.ptr, record.len, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 160, 80);
	std_vec_free_Checked(&record, 1, 1);
	std_vec_free_Checked(&name, 1, 1);
}

/* chunks_packs_merge_tests_names returns the names of a directory's files, which unwraps at a line and column. */
static StdVec
chunks_packs_merge_tests_names(HostIoIo *io, const StdPathBuf *dir, uint32 line, uint32 col)
{
	StdVec		names;
	ChunksErrorError error;

	if (!chunks_file_file_names(HOST_IO_IO_READ_DOORS(io), dir->bytes.ptr, dir->bytes.len, &names, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, line, col);
	return names;
}

/*
 * test_chunks_packs_merge_tests_a_merge_whose_runs_are_no_longer_listed_together_replaces_nothing:
 * a merge whose runs another backend's change took apart meanwhile replaces
 * nothing and removes the run it wrote.
 */
void
test_chunks_packs_merge_tests_a_merge_whose_runs_are_no_longer_listed_together_replaces_nothing(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	StdPathBuf	dir;
	HostSharedOwned owned;
	HostSharedRecord record;
	ChunksPacksFixturesPacked first;
	ChunksPacksFixturesPacked second;
	ChunksPacksMergeTestsPacks packs = {&first, &second};
	ChunksPacksViewView mine;
	ChunksPacksViewView other;
	StdVec		before;
	StdVec		after;
	bool		same;
	StdFmtPiece pieces[] = {STD_FMT_LIT("the merged run's file was left behind")};
	StdFmtArguments left_behind = {pieces, lengthof(pieces), NULL};

	chunks_packs_fixtures_home(&dir);
	owned = chunks_packs_lists_tests_repository(io, &dir);
	record = host_shared_Owned_record(&owned);
	first = chunks_packs_fixtures_pack(io, &dir, 1);
	second = chunks_packs_fixtures_pack(io, &dir, 2);
	chunks_packs_fixtures_view(&mine);
	chunks_packs_fixtures_view(&other);
	chunks_packs_lists_tests_changed(io, record, &other, chunks_packs_merge_tests_add_both, &packs);
	chunks_packs_tests_refreshed(RUST_FILE, &mine, io, record, 157, 34);
	chunks_packs_lists_tests_changed(io, record, &other, chunks_packs_merge_tests_take_apart, &packs);
	before = chunks_packs_merge_tests_names(io, &dir, 162, 55);
	chunks_packs_merge_tests_none(io, record, &mine, true, 163, 55);
	after = chunks_packs_merge_tests_names(io, &dir, 164, 53);
	same = after.len == before.len;
	for (size_t k = 0, k_len = after.len; same && k < k_len; k++)
	{
		const StdString *a = &STD_VEC_AT(&after, StdString, k);
		const StdString *b = &STD_VEC_AT(&before, StdString, k);

		same = a->len == b->len && memcmp(a->ptr, b->ptr, a->len) == 0;
	}
	if (!same)
		std_assert_failed(RUST_FILE, 164, 9, "==", &after, chunks_packs_merge_tests_Names_Debug_fmt, &before,
						  chunks_packs_merge_tests_Names_Debug_fmt, &left_behind);
	std_vec_drop_Checked(&after, sizeof(StdString), _Alignof(StdString), std_String_drop);
	chunks_packs_tests_refreshed(RUST_FILE, &mine, io, record, 165, 34);
	chunks_packs_merge_tests_runs_eq(&mine, 1, 166, 9, 34);
	chunks_packs_merge_tests_found(&mine, io, &second.address, &second.at, 167, 9, 40);
	std_vec_drop_Checked(&before, sizeof(StdString), _Alignof(StdString), std_String_drop);
	chunks_packs_view_View_drop(&other);
	chunks_packs_view_View_drop(&mine);
	host_shared_Owned_drop(&owned);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	simc_TestIo_drop(&test_io);
}

/* chunks_packs_merge_tests_short is a change's body that adds three packs, and replaces the first two's runs with one that names both but holds only the first's entry. */
static void
chunks_packs_merge_tests_short(ChunksPacksListsChange *change, void *captures)
{
	const ChunksPacksMergeTestsPacks *packs = captures;
	StdString	names[2];
	StdVec		record;
	ChunksErrorError error;

	chunks_packs_merge_tests_added(change, &packs->first->entry, false, 183, 38);
	chunks_packs_merge_tests_added(change, &packs->second->entry, false, 184, 39);
	names[0] = chunks_packs_merge_tests_idx(packs->first);
	names[1] = chunks_packs_merge_tests_idx(packs->second);
	chunks_packs_Runs_replaced(names, lengthof(names), "short.idx:1,2", strlen("short.idx:1,2"), &record);
	if (!chunks_packs_lists_Change_change_runs(change, 0, record.ptr, record.len, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 186, 77);
	std_vec_free_Checked(&record, 1, 1);
	std_vec_free_Checked(&names[1], 1, 1);
	std_vec_free_Checked(&names[0], 1, 1);
	chunks_packs_merge_tests_added(change, &packs->third->entry, false, 187, 38);
}

/* chunks_packs_merge_tests_contains reports whether a message holds a text. */
static bool
chunks_packs_merge_tests_contains(const StdString *message, const char *text)
{
	for (size_t k = 0; k + strlen(text) <= message->len; k++)
		if (memcmp((const char *) message->ptr + k, text, strlen(text)) == 0)
			return true;
	return false;
}

/*
 * test_chunks_packs_merge_tests_a_merge_that_would_leave_a_covered_pack_in_no_run_fails:
 * a merge fails, replacing nothing, when a listed pack that the runs' lines
 * say they cover has no entry in them.
 */
void
test_chunks_packs_merge_tests_a_merge_that_would_leave_a_covered_pack_in_no_run_fails(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	StdPathBuf	dir;
	StdPathBuf	short_path;
	HostSharedOwned owned;
	HostSharedRecord record;
	ChunksPacksFixturesPacked first;
	ChunksPacksFixturesPacked second;
	ChunksPacksFixturesPacked third;
	ChunksPacksMergeTestsPacks packs = {&first, &second, &third};
	ChunksPacksViewView mine;
	chunks_TupleAddressLocator_t entry;
	ChunksIndexRunLocator run;
	ChunksPacksMergeTestsMerged merged;
	ChunksErrorError failed;
	bool		matched;

	chunks_packs_fixtures_home(&dir);
	owned = chunks_packs_lists_tests_repository(io, &dir);
	record = host_shared_Owned_record(&owned);
	first = chunks_packs_fixtures_pack(io, &dir, 1);
	second = chunks_packs_fixtures_pack(io, &dir, 2);
	third = chunks_packs_fixtures_pack(io, &dir, 3);
	chunks_packs_fixtures_view(&mine);
	std_path_join(dir.bytes.ptr, dir.bytes.len, "short.idx", strlen("short.idx"), &short_path);
	entry.f0 = first.address;
	entry.f1 = first.at;
	if (!chunks_index_RunLocator_write(HOST_IO_IO_IO_DOORS(io), short_path.bytes.ptr, short_path.bytes.len, &entry, 1,
									   &run, &failed))
		chunks_packs_tests_unwrapped(RUST_FILE, &failed, 181, 71);
	chunks_index_RunLocator_drop(&run);
	std_vec_free_Checked(&short_path.bytes, 1, 1);
	chunks_packs_lists_tests_changed(io, record, &mine, chunks_packs_merge_tests_short, &packs);
	if (chunks_packs_merge_tests_merged(io, record, &mine, &merged, &failed))
		STD_RESULT_EXPECT_FAILED(189, 52, "called `Result::unwrap_err()` on an `Ok` value", &merged,
								 chunks_packs_merge_tests_Merged_Debug_fmt);
	matched = failed.tag == CHUNKS_ERROR_ERROR_CORRUPT &&
		chunks_packs_merge_tests_contains(&failed.u.corrupt, "pack number 2");
	if (!matched)
	{
		StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&failed, chunks_error_Error_Debug_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_panic_fmt(RUST_FILE, 190, 9, &arguments);
	}
	chunks_error_Error_drop(&failed);
	chunks_packs_tests_refreshed(RUST_FILE, &mine, io, record, 191, 34);
	chunks_packs_merge_tests_runs_eq(&mine, 2, 192, 9, 34);
	chunks_packs_view_View_drop(&mine);
	host_shared_Owned_drop(&owned);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	simc_TestIo_drop(&test_io);
}

/* chunks_packs_merge_tests_lossy is a change's body that adds two packs and marks the first's run as written with loss. */
static void
chunks_packs_merge_tests_lossy(ChunksPacksListsChange *change, void *captures)
{
	const ChunksPacksMergeTestsPacks *packs = captures;
	StdString	name;
	StdString	line;
	StdVec		record;
	ChunksErrorError error;

	chunks_packs_merge_tests_added(change, &packs->first->entry, false, 207, 38);
	chunks_packs_merge_tests_added(change, &packs->second->entry, false, 208, 39);
	name = chunks_packs_merge_tests_idx(packs->first);
	std_string_from_Checked(&line, name.ptr, name.len);
	std_string_push_str_Checked(&line, ":1:lossy", strlen(":1:lossy"));
	chunks_packs_Runs_replaced(&name, 1, line.ptr, line.len, &record);
	if (!chunks_packs_lists_Change_change_runs(change, 0, record.ptr, record.len, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 210, 109);
	std_vec_free_Checked(&record, 1, 1);
	std_vec_free_Checked(&line, 1, 1);
	std_vec_free_Checked(&name, 1, 1);
}

/*
 * test_chunks_packs_merge_tests_a_merge_takes_no_run_marked_as_written_with_loss:
 * a merge takes no run marked as written from a pack whose address table
 * didn't read whole, since the merged run would carry no mark for the next
 * settle to write it again by.
 */
void
test_chunks_packs_merge_tests_a_merge_takes_no_run_marked_as_written_with_loss(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	StdPathBuf	dir;
	HostSharedOwned owned;
	HostSharedRecord record;
	ChunksPacksFixturesPacked first;
	ChunksPacksFixturesPacked second;
	ChunksPacksMergeTestsPacks packs = {&first, &second};
	ChunksPacksViewView mine;

	chunks_packs_fixtures_home(&dir);
	owned = chunks_packs_lists_tests_repository(io, &dir);
	record = host_shared_Owned_record(&owned);
	first = chunks_packs_fixtures_pack(io, &dir, 1);
	second = chunks_packs_fixtures_pack(io, &dir, 2);
	chunks_packs_fixtures_view(&mine);
	chunks_packs_lists_tests_changed(io, record, &mine, chunks_packs_merge_tests_lossy, &packs);
	chunks_packs_merge_tests_none(io, record, &mine, false, 212, 50);
	chunks_packs_tests_refreshed(RUST_FILE, &mine, io, record, 213, 34);
	chunks_packs_merge_tests_runs_eq(&mine, 2, 214, 9, 34);
	chunks_packs_view_View_drop(&mine);
	host_shared_Owned_drop(&owned);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	simc_TestIo_drop(&test_io);
}

/* test_chunks_packs_merge_tests_a_runs_line_carries_wide_numbers: a runs file's line gives back its packs' numbers above 32 bits. */
void
test_chunks_packs_merge_tests_a_runs_line_carries_wide_numbers(void)
{
	static const char line[] = "a.idx:4294967301,7";
	StdVec		covered;
	StdVec		want;
	static const uint64 numbers[] = {UINT64CONST(4294967301), 7};

	chunks_packs_merge_covered(line, strlen(line), &covered);
	if (covered.len != lengthof(numbers) || memcmp(covered.ptr, numbers, sizeof(numbers)) != 0)
	{
		std_slice_to_vec_Checked(&want, numbers, lengthof(numbers), sizeof(uint64), _Alignof(uint64));
		std_assert_failed(RUST_FILE, 220, 9, "==", &covered, chunks_packs_tests_Numbers_Debug_fmt, &want,
						  chunks_packs_tests_Numbers_Debug_fmt, NULL);
	}
	std_vec_free_Checked(&covered, sizeof(uint64), _Alignof(uint64));
}

#endif							/* DOLTGRES_TEST_CHUNKS */
