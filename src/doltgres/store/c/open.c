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
 * The startup process's open of every repository before the redo decision:
 * each repository's packs settled, what each settle changed reported once,
 * and each manifest whose two generations differ written once more when its
 * newest root reads, so that a start leaves no repository holding WAL back
 * for a previous manifest.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/packs/backend.h"
#include "doltgres/chunks/packs/generation.h"
#include "doltgres/chunks/packs/open.h"
#include "doltgres/chunks/packs/settle.h"
#include "doltgres/chunks/packs/shared.h"
#include "doltgres/host/lib.h"
#include "doltgres/host/shared.h"
#include "doltgres/repo/lib.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/path.h"
#include "doltgres/std/str.h"
#include "doltgres/std/tuple.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"
#include "doltgres/store/databases.h"
#include "doltgres/store/lib.h"
#include "doltgres/store/open.h"
#include "doltgres/trees/published.h"

#ifdef DOLTGRES_TEST_STORE
#include "doltgres/chunks/file.h"
#include "doltgres/simc/simc.h"
#include "doltgres/std/io.h"
#include "doltgres/std/num.h"
#include "doltgres/std/path.h"
#include "doltgres/std/slice.h"
#include "doltgres/std/test.h"
#include "doltgres/std/test_fs.h"
#include "doltgres/store/fixtures.h"
#endif

#define RUST_FILE "store" STD_SEP "src" STD_SEP "open.rs"

/* store_open_counted words a count with its noun, singular for one, as Postgres' errmsg_plural does. */
static StdString
store_open_counted(size_t count, const char *one, const char *many)
{
	StdString	result;
	StdStr		noun = {count == 1 ? one : many, strlen(count == 1 ? one : many)};
	StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(" "), STD_FMT_ARG(1)};
	StdFmtArg	args[] = {{&count, std_usize_Display_fmt}, {&noun, std_str_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	std_fmt_format_Checked(&result, &arguments);
	return result;
}

/* store_open_text makes a report's line as Postgres takes it in place, with no NUL in it and one after it. */
static void
store_open_text(StdString *line)
{
	size_t		kept = 0;

	for (size_t k = 0, k_len = line->len; k < k_len; k++)
		if (((const char *) line->ptr)[k] != '\0')
			((char *) line->ptr)[kept++] = ((const char *) line->ptr)[k];
	line->len = kept;
	std_string_push_str_Checked(line, "", 1);
}

/* store_open_said makes a report's line of a formatting. */
static StdString
store_open_said(const StdFmtArguments *arguments)
{
	StdString	line;

	std_fmt_format_Checked(&line, arguments);
	store_open_text(&line);
	return line;
}

/* store_open_Lines_drop drops a Vec<String>. */
static void
store_open_Lines_drop(StdVec *lines)
{
	std_vec_drop_Checked(lines, sizeof(StdString), _Alignof(StdString), std_String_drop);
}

/* store_open_report_frame_0_t is report's state across its pad: the lines it warns of. */
typedef struct store_open_report_frame_0_t
{
	HostIoIo   *io;
	const StdString *context;
	const ChunksPacksSettleSettled *settled;
	HostError  *error;
	bool		ok;

	/*
	 * warned is a Vec<String> of the lines a WARNING reports, one for each
	 * file.
	 */
	StdVec		warned;
	bool		warned_live;
	StdString	line;
	bool		line_live;
} store_open_report_frame_0_t;

/* store_open_report_pad_0 is report's pad: a line, then the lines to warn of. */
static void
store_open_report_pad_0(void *frame_)
{
	store_open_report_frame_0_t *frame = frame_;

	if (frame->line_live)
	{
		frame->line_live = false;
		std_String_drop(&frame->line);
	}
	if (frame->warned_live)
	{
		frame->warned_live = false;
		store_open_Lines_drop(&frame->warned);
	}
}

/* store_open_logged reports the frame's line at LOG. */
static bool
store_open_logged(store_open_report_frame_0_t *frame)
{
	bool		ok;

	frame->line_live = true;
	ok = host_log(HOST_IO_IO_IO_DOORS(frame->io), frame->line.ptr, frame->error);
	frame->line_live = false;
	std_String_drop(&frame->line);
	return ok;
}

/* store_open_warn_line appends a line to warn of. */
static void
store_open_warn_line(store_open_report_frame_0_t *frame, const StdFmtArguments *arguments)
{
	StdString	line;

	std_fmt_format_Checked(&line, arguments);
	std_vec_push_Checked(&frame->warned, &line, sizeof(line), _Alignof(StdString));
}

/* store_open_report_body_0 is report's body: a crash's leftovers and the runs files' ends at LOG, then each fallback, damage, loss, and missing pack at WARNING. */
static void
store_open_report_body_0(void *frame_)
{
	store_open_report_frame_0_t *frame = frame_;
	const ChunksPacksSettleSettled *settled = frame->settled;
	StdStr		name = {frame->context->ptr, frame->context->len - 1};

	frame->ok = false;
	if (settled->removed.len != 0)
	{
		const StdPathBuf *first = &STD_VEC_AT(&settled->removed, StdPathBuf, 0);
		StdString	files = store_open_counted(settled->removed.len, "file", "files");
		StdStr		shown = {first->bytes.ptr, first->bytes.len};
		StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(": removed "), STD_FMT_ARG(1),
		STD_FMT_LIT(" a crash left behind, the first \""), STD_FMT_ARG(2), STD_FMT_LIT("\"")};
		StdFmtArg	args[] = {{&name, std_str_Display_fmt}, {&files, std_String_Display_fmt},
		{&shown, std_path_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		frame->line = store_open_said(&arguments);
		std_String_drop(&files);
		if (!store_open_logged(frame))
			goto done;
	}
	if (settled->moved.len != 0)
	{
		StdString	spaces = store_open_counted(settled->moved.len, "tablespace", "tablespaces");
		StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(": recorded the runs file's end of "), STD_FMT_ARG(1),
		STD_FMT_LIT(", the first "), STD_FMT_ARG(2)};
		StdFmtArg	args[] = {{&name, std_str_Display_fmt}, {&spaces, std_String_Display_fmt},
		{&STD_VEC_AT(&settled->moved, uint32, 0), std_u32_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		frame->line = store_open_said(&arguments);
		std_String_drop(&spaces);
		if (!store_open_logged(frame))
			goto done;
	}
	std_vec_new(&frame->warned, sizeof(StdString), _Alignof(StdString));
	frame->warned_live = true;
	for (size_t k = 0, k_len = settled->fallbacks.len; k < k_len; k++)
	{
		const chunks_TuplePathBufFallback_t *fallback = &STD_VEC_AT(&settled->fallbacks, chunks_TuplePathBufFallback_t,
																	k);
		StdStr		shown = {fallback->f0.bytes.ptr, fallback->f0.bytes.len};
		StdFmtPiece pieces[] = {STD_FMT_LIT("list file \""), STD_FMT_ARG(0),
			STD_FMT_LIT("\" read as its previous generation ("), STD_FMT_ARG(1),
		STD_FMT_LIT(") and written whole again")};
		StdFmtArg	args[] = {{&shown, std_path_Display_fmt}, {&fallback->f1, chunks_file_Fallback_Debug_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		store_open_warn_line(frame, &arguments);
	}
	for (size_t k = 0, k_len = settled->damaged.len; k < k_len; k++)
	{
		const chunks_TuplePathBufDamage_t *damaged = &STD_VEC_AT(&settled->damaged, chunks_TuplePathBufDamage_t, k);
		size_t		after = damaged->f1.after.len;
		StdStr		shown = {damaged->f0.bytes.ptr, damaged->f0.bytes.len};
		StdString	lost;
		StdFmtPiece pieces[] = {STD_FMT_LIT("list file \""), STD_FMT_ARG(0), STD_FMT_LIT("\" was damaged at offset "),
		STD_FMT_ARG(1), STD_FMT_LIT(", so "), STD_FMT_ARG(2)};
		StdFmtArg	args[] = {{&shown, std_path_Display_fmt}, {&damaged->f1.offset, std_u64_Display_fmt},
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
		store_open_warn_line(frame, &arguments);
		std_String_drop(&lost);
	}
	for (size_t k = 0, k_len = settled->rebuilt.len; k < k_len; k++)
	{
		const chunks_TuplePathBufLost_t *rebuilt = &STD_VEC_AT(&settled->rebuilt, chunks_TuplePathBufLost_t, k);
		StdString	blocks;
		StdString	records;
		StdStr		shown = {rebuilt->f0.bytes.ptr, rebuilt->f0.bytes.len};
		StdFmtPiece pieces[] = {STD_FMT_LIT("pack \""), STD_FMT_ARG(0),
			STD_FMT_LIT("\" was indexed again from its records, losing "), STD_FMT_ARG(1),
		STD_FMT_LIT(" of its table and "), STD_FMT_ARG(2)};
		StdFmtArg	args[] = {{&shown, std_path_Display_fmt}, {&blocks, std_String_Display_fmt},
		{&records, std_String_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		if (rebuilt->f1.records.len == 0 && rebuilt->f1.blocks.len == 0)
			continue;
		blocks = store_open_counted(rebuilt->f1.blocks.len, "block", "blocks");
		records = store_open_counted(rebuilt->f1.records.len, "record", "records");
		store_open_warn_line(frame, &arguments);
		std_String_drop(&records);
		std_String_drop(&blocks);
	}
	for (size_t k = 0, k_len = settled->unlisted.len; k < k_len; k++)
	{
		StdFmtPiece pieces[] = {STD_FMT_LIT("pack "), STD_FMT_ARG(0),
		STD_FMT_LIT(", which the pack table listed, has no file, so it was left out")};
		StdFmtArg	args[] = {{&STD_VEC_AT(&settled->unlisted, uint64, k), std_u64_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		store_open_warn_line(frame, &arguments);
	}
	for (size_t k = 0, k_len = frame->warned.len; k < k_len; k++)
	{
		StdString  *message = &STD_VEC_AT(&frame->warned, StdString, k);
		StdString	taken = *message;
		HostError	failure;

		std_string_from_Checked(message, "", 0);
		failure = host_Error_fork((const uint8 *) "XX001", &taken);
		if (!host_warn(HOST_IO_IO_IO_DOORS(frame->io), &failure, frame->context->ptr, frame->error))
			goto done;
	}
	frame->ok = true;
done:
	store_open_report_pad_0(frame);
}

/*
 * store_open_report reports what a repository's settle changed: the files a
 * crash left, and the runs files' ends it recorded, at LOG, one line for each
 * kind with its count and its first path; a list file read as its previous
 * generation, one damaged, a pack whose run it wrote again with loss, and a
 * listed pack whose file is gone at WARNING with Postgres' data-corrupted
 * SQLSTATE, one for each file. The context is a line of open's own, NUL
 * after it.
 */
static bool
store_open_report(HostIoIo *io, const StdString *context, const ChunksPacksSettleSettled *settled, HostError *error)
{
	store_open_report_frame_0_t frame = {.io = io, .context = context, .settled = settled, .error = error};

	std_landing(&frame, store_open_report_body_0, store_open_report_pad_0);
	return frame.ok;
}

/* store_open_roots_read reads the roots of a manifest's base tree and identity tree, failing at the first that can't be read. */
static bool
store_open_roots_read(HostIoIo *io, const ChunksPacksBackendPacks *packs, const HostSharedReader *reading,
					  const RepoManifest *manifest, ChunksErrorError *error)
{
	const TreesPublishedPublished *roots[2] = {manifest->has_bases ? &manifest->bases : NULL,
	manifest->has_identity ? &manifest->identity : NULL};

	for (size_t k = 0; k < lengthof(roots); k++)
	{
		TreesStoreNamed named;
		ChunksAddressAddress address;
		ChunksTrailerLocator locator;
		ChunksChunkChunk chunk;

		if (roots[k] == NULL)
			continue;
		named = TREES_PUBLISHED_PUBLISHED_NAMED(unconstify(TreesPublishedPublished *, roots[k]));
		named.ops->parts(named.self, &address, &locator);
		if (!chunks_packs_backend_Packs_read(packs, io, reading, &address, locator, &chunk, error))
			return false;
		chunks_chunk_Chunk_drop(&chunk);
	}
	return true;
}

/* store_open_evened_frame_0_t is evened's state across its pad: the newest manifest's bytes, the manifest, the previous one's bytes, and a line. */
typedef struct store_open_evened_frame_0_t
{
	HostIoIo   *io;
	const ChunksPacksBackendPacks *packs;
	const HostSharedReader *reading;
	HostLsn		redo;
	const StdString *context;
	HostError  *error;
	bool		ok;
	StdVec		bytes;
	bool		bytes_live;
	RepoManifest manifest;
	bool		manifest_live;
	StdVec		previous;
	bool		previous_live;
	StdString	line;
	bool		line_live;
} store_open_evened_frame_0_t;

/* store_open_evened_pad_0 is evened's pad: a line, the previous bytes, the manifest, then the newest bytes. */
static void
store_open_evened_pad_0(void *frame_)
{
	store_open_evened_frame_0_t *frame = frame_;

	if (frame->line_live)
	{
		frame->line_live = false;
		std_String_drop(&frame->line);
	}
	if (frame->previous_live)
	{
		frame->previous_live = false;
		std_vec_free_Checked(&frame->previous, 1, 1);
	}
	if (frame->manifest_live)
	{
		frame->manifest_live = false;
		repo_Manifest_drop(&frame->manifest);
	}
	if (frame->bytes_live)
	{
		frame->bytes_live = false;
		std_vec_free_Checked(&frame->bytes, 1, 1);
	}
}

/* store_open_evened_body_0 is evened's body: the newest manifest written once more, or both generations kept and the WAL their fallback replays read through. */
static void
store_open_evened_body_0(void *frame_)
{
	store_open_evened_frame_0_t *frame = frame_;
	ChunksErrorError failed;
	ChunksFileFallback fallback;
	HostError	failure;
	bool		has;
	bool		has_fallback;

	frame->ok = false;
	if (!chunks_packs_backend_Packs_read_kept(frame->packs, frame->io, CHUNKS_PACKS_BACKEND_KEPT_MANIFEST, &has,
											  &frame->bytes, &has_fallback, &fallback, &failed))
		goto raised;
	if (!has)
	{
		frame->ok = true;
		goto done;
	}
	frame->bytes_live = true;
	if (!repo_Manifest_decode(frame->bytes.ptr, frame->bytes.len, trees_published_RepoId_of(frame->packs),
							  &frame->manifest, &failed))
		goto raised;
	frame->manifest_live = true;
	if (has_fallback)
	{
		static const char message[] =
			"its newest manifest is not whole, so both generations are kept for the fallback of redo";
		StdString	said;

		std_string_from_Checked(&said, message, sizeof(message) - 1);
		failure = host_Error_fork((const uint8 *) "XX001", &said);
		frame->ok = host_warn(HOST_IO_IO_IO_DOORS(frame->io), &failure, frame->context->ptr, frame->error) &&
			store_read_kept_wal(frame->io, frame->manifest.redo, frame->redo, frame->error);
		goto done;
	}
	if (!chunks_packs_backend_Packs_read_previous(frame->packs, frame->io, CHUNKS_PACKS_BACKEND_KEPT_MANIFEST, &has,
												  &frame->previous, &failed))
		goto raised;
	if (has)
	{
		bool		same = frame->previous.len == frame->bytes.len &&
			memcmp(frame->previous.ptr, frame->bytes.ptr, frame->bytes.len) == 0;

		frame->previous_live = true;
		if (same)
		{
			frame->ok = true;
			goto done;
		}
	}
	if (store_open_roots_read(frame->io, frame->packs, frame->reading, &frame->manifest, &failed))
	{
		if (!chunks_packs_backend_Packs_write_kept(frame->packs, frame->io, CHUNKS_PACKS_BACKEND_KEPT_MANIFEST,
												   frame->bytes.ptr, frame->bytes.len, &failed))
			goto raised;
		frame->ok = true;
		goto done;
	}
	{
		StdStr		name = {frame->context->ptr, frame->context->len - 1};
		StdFmtPiece pieces[] = {STD_FMT_ARG(0),
			STD_FMT_LIT(": its newest manifest's base tree's or identity tree's root can't be read, so both "
						"generations are kept for the fallback of redo")};
		StdFmtArg	args[] = {{&name, std_str_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		failure = chunks_packs_shared_raised(&failed);
		frame->line = store_open_said(&arguments);
		frame->line_live = true;
		frame->ok = host_warn(HOST_IO_IO_IO_DOORS(frame->io), &failure, frame->line.ptr, frame->error) &&
			store_read_kept_wal(frame->io, frame->manifest.redo, frame->redo, frame->error);
		goto done;
	}
raised:
	*frame->error = chunks_packs_shared_raised(&failed);
done:
	store_open_evened_pad_0(frame);
}

/* store_open_evened is even's work over a repository's open packs. */
static bool
store_open_evened(HostIoIo *io, const ChunksPacksBackendPacks *packs, const HostSharedReader *reading, HostLsn redo,
				  const StdString *context, HostError *error)
{
	store_open_evened_frame_0_t frame = {.io = io, .packs = packs, .reading = reading, .redo = redo,
	.context = context, .error = error};

	std_landing(&frame, store_open_evened_body_0, store_open_evened_pad_0);
	return frame.ok;
}

/* store_open_even_frame_0_t is even's state across its pad: the repository's packs and the reader. */
typedef struct store_open_even_frame_0_t
{
	HostIoIo   *io;
	uint32		repository;
	uint32		own;
	HostLsn		redo;
	const StdString *context;
	HostError  *error;
	bool		ok;
	ChunksPacksBackendPacks packs;
	bool		packs_live;
	HostSharedReader reading;
	bool		reading_live;
} store_open_even_frame_0_t;

/* store_open_even_pad_0 is even's pad: the reader, then the packs. */
static void
store_open_even_pad_0(void *frame_)
{
	store_open_even_frame_0_t *frame = frame_;

	if (frame->reading_live)
	{
		frame->reading_live = false;
		host_shared_Reader_drop(&frame->reading);
	}
	if (frame->packs_live)
	{
		frame->packs_live = false;
		chunks_packs_backend_Packs_drop(&frame->packs);
	}
}

/* store_open_warned warns of an error, taking it, in a context. */
static bool
store_open_warned(HostIoIo *io, HostError *failure, const StdString *context, HostError *error)
{
	return host_warn(HOST_IO_IO_IO_DOORS(io), failure, context->ptr, error);
}

/* store_open_even_body_0 is even's body: the packs opened, evened, and closed, each failure warned of. */
static void
store_open_even_body_0(void *frame_)
{
	store_open_even_frame_0_t *frame = frame_;
	ChunksErrorError failed;
	HostError	failure;

	frame->ok = false;
	if (!chunks_packs_open_Packs_open(frame->io, frame->repository, frame->own, &frame->packs, &failed))
	{
		failure = chunks_packs_shared_raised(&failed);
		frame->ok = store_open_warned(frame->io, &failure, frame->context, frame->error);
		goto done;
	}
	frame->packs_live = true;
	if (!host_shared_read(frame->io, &frame->reading, frame->error))
		goto done;
	frame->reading_live = true;
	if (!store_open_evened(frame->io, &frame->packs, &frame->reading, frame->redo, frame->context, &failure) &&
		!store_open_warned(frame->io, &failure, frame->context, frame->error))
		goto done;
	frame->packs_live = false;
	if (chunks_packs_backend_Packs_close(&frame->packs, frame->io, &frame->reading, &failed))
		frame->ok = true;
	else
	{
		failure = chunks_packs_shared_raised(&failed);
		frame->ok = store_open_warned(frame->io, &failure, frame->context, frame->error);
	}
done:
	store_open_even_pad_0(frame);
}

/*
 * store_open_even writes a settled repository's newest manifest once more
 * where its two generations differ and its newest root reads, and otherwise
 * leaves both generations for txn's fallback and reads through the WAL it
 * replays. A repository whose packs or manifest it can't read is warned of
 * and left as it is, as a failed settle is.
 */
static bool
store_open_even(HostIoIo *io, uint32 repository, uint32 own, HostLsn redo, const StdString *context,
				HostError *error)
{
	store_open_even_frame_0_t frame = {.io = io, .repository = repository, .own = own, .redo = redo,
	.context = context, .error = error};

	std_landing(&frame, store_open_even_body_0, store_open_even_pad_0);
	return frame.ok;
}

/* store_open_startup_open_frame_0_t is startup_open's state across its pad: the repositories, a context, and a settle's changes. */
typedef struct store_open_startup_open_frame_0_t
{
	HostIoIo   *io;
	HostLsn		redo;
	HostLsn		label;
	HostError  *error;
	bool		ok;
	/* found is a Vec<(NonZeroU32, u32)> of the repositories. */
	StdVec		found;
	bool		found_live;
	StdString	context;
	bool		context_live;
	ChunksPacksSettleSettled settled;
	bool		settled_live;
	StdString	line;
	bool		line_live;
} store_open_startup_open_frame_0_t;

/* store_open_startup_open_pad_0 is startup_open's pad: a line, a settle's changes, a context, then the repositories. */
static void
store_open_startup_open_pad_0(void *frame_)
{
	store_open_startup_open_frame_0_t *frame = frame_;

	if (frame->line_live)
	{
		frame->line_live = false;
		std_String_drop(&frame->line);
	}
	if (frame->settled_live)
	{
		frame->settled_live = false;
		chunks_packs_settle_Settled_drop(&frame->settled);
	}
	if (frame->context_live)
	{
		frame->context_live = false;
		std_String_drop(&frame->context);
	}
	if (frame->found_live)
	{
		frame->found_live = false;
		std_vec_free_Checked(&frame->found, sizeof(std_TupleU32U32_t), _Alignof(std_TupleU32U32_t));
	}
}

/*
 * store_open_settled takes back a backup's kept generation of one
 * repository's files at a start from its label, removes the generations
 * backups kept, and settles its packs and reports or evens it, or warns of a
 * failure of any of them.
 */
static bool
store_open_settled(store_open_startup_open_frame_0_t *frame, uint32 repository, uint32 own)
{
	ChunksErrorError failed;
	HostError	failure;
	bool		ok;

	if ((frame->label != 0 &&
		 !chunks_packs_generation_Packs_restore_generation(frame->io, repository, own, frame->label, &failed)) ||
		!chunks_packs_generation_Packs_drop_generations(HOST_IO_IO_IO_DOORS(frame->io), repository, own, &failed) ||
		!chunks_packs_settle_Packs_settle(frame->io, repository, own, &frame->settled, &failed))
	{
		failure = chunks_packs_shared_raised(&failed);
		return store_open_warned(frame->io, &failure, &frame->context, frame->error);
	}
	frame->settled_live = true;
	if (frame->settled.dropped)
	{
		StdFmtPiece pieces[] = {STD_FMT_LIT("removed the directories of dropped database "), STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&repository, std_u32_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		frame->line = store_open_said(&arguments);
		frame->line_live = true;
		ok = host_log(HOST_IO_IO_IO_DOORS(frame->io), frame->line.ptr, frame->error);
		frame->line_live = false;
		std_String_drop(&frame->line);
	}
	else
		ok = store_open_report(frame->io, &frame->context, &frame->settled, frame->error) &&
			store_open_even(frame->io, repository, own, frame->redo, &frame->context, frame->error);
	frame->settled_live = false;
	chunks_packs_settle_Settled_drop(&frame->settled);
	return ok;
}

/* store_open_startup_open_body_0 is startup_open's body: the marked moves finished, each repository settled, then template1's made. */
static void
store_open_startup_open_body_0(void *frame_)
{
	store_open_startup_open_frame_0_t *frame = frame_;
	ChunksErrorError failed;

	frame->ok = false;
	if (!store_databases_marked_moves(frame->io, frame->error))
		goto done;
	if (!chunks_packs_open_repositories(HOST_IO_IO_READ_DOORS(frame->io), &frame->found, &failed))
	{
		*frame->error = chunks_packs_shared_raised(&failed);
		goto done;
	}
	frame->found_live = true;
	for (size_t k = 0, k_len = frame->found.len; k < k_len; k++)
	{
		const std_TupleU32U32_t *found = &STD_VEC_AT(&frame->found, std_TupleU32U32_t, k);
		StdFmtPiece pieces[] = {STD_FMT_LIT("database "), STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&found->f0, std_u32_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};
		bool		ok;

		frame->context = store_open_said(&arguments);
		frame->context_live = true;
		ok = store_open_settled(frame, found->f0, found->f1);
		frame->context_live = false;
		std_String_drop(&frame->context);
		if (!ok)
			goto done;
	}
	frame->ok = store_databases_first(frame->io, frame->error);
done:
	store_open_startup_open_pad_0(frame);
}

/*
 * store_open_startup_open settles every repository's packs before the redo
 * decision, at the control file's redo point, and reports what each settle
 * changed once. A repository whose settle fails is warned of and left
 * unopened. A manifest whose two generations differ is written once more
 * when its newest root reads; otherwise both generations are kept and the
 * WAL their fallback replays is read through. At a start from a base
 * backup's label, given the label's checkpoint, each repository first takes
 * back the generation that checkpoint kept (bugs row 178), and every start
 * removes the generations backups kept. A bootstrap process does nothing.
 * Last, it makes template1's repository when bootstrap's marker says to,
 * after the settles.
 */
bool
store_open_startup_open(HostIoIo *io, HostLsn redo, HostLsn label, HostError *error)
{
	store_open_startup_open_frame_0_t frame = {.io = io, .redo = redo, .label = label, .error = error};

	if (!host_shared_ready())
		return true;
	std_landing(&frame, store_open_startup_open_body_0, store_open_startup_open_pad_0);
	return frame.ok;
}

#ifdef DOLTGRES_TEST_STORE

/* STORE_OPEN_TESTS_UNWRAP is Result::unwrap's panic text. */
#define STORE_OPEN_TESTS_UNWRAP "called `Result::unwrap()` on an `Err` value"

/* STORE_OPEN_TESTS_UNWRAP_ERR is Result::unwrap_err's panic text. */
#define STORE_OPEN_TESTS_UNWRAP_ERR "called `Result::unwrap_err()` on an `Ok` value"

/* store_open_tests_chunks_ok asserts a call of chunks' success, which unwraps at a line and column. */
static void
store_open_tests_chunks_ok(bool ok, const ChunksErrorError *error, uint32 line, uint32 col)
{
	if (!ok)
		STD_RESULT_EXPECT_FAILED(line, col, STORE_OPEN_TESTS_UNWRAP, error, chunks_error_Error_Debug_fmt);
}

/* store_open_tests_io_ok asserts a file call's success, which unwraps at a line and column. */
static void
store_open_tests_io_ok(bool ok, const StdIoError *error, uint32 line, uint32 col)
{
	if (!ok)
		STD_RESULT_EXPECT_FAILED(line, col, STORE_OPEN_TESTS_UNWRAP, error, std_io_Error_Debug_fmt);
}

/* store_open_tests_newest returns a repository's newest manifest, whose read and option unwrap at a line and columns. */
static StdVec
store_open_tests_newest(HostIoIo *io, uint32 number, uint32 line, uint32 col, uint32 some_col)
{
	StdPathBuf	path = store_fixtures_manifest(number);
	ChunksErrorError failed;
	ChunksFileFallback fallback;
	StdVec		newest;
	bool		has;
	bool		has_fallback;

	store_open_tests_chunks_ok(chunks_file_read_kept(HOST_IO_IO_READ_DOORS(io), path.bytes.ptr, path.bytes.len, &has,
													 &newest, &has_fallback, &fallback, &failed), &failed, line, col);
	if (!has)
		std_option_unwrap_failed(RUST_FILE, line, some_col);
	std_vec_free_Checked(&path.bytes, 1, 1);
	return newest;
}

/* store_open_tests_even reports whether a repository's manifest's two generations hold the same bytes. */
static bool
store_open_tests_even(HostIoIo *io, uint32 number)
{
	StdVec		newest = store_open_tests_newest(io, number, 188, 69, 78);
	StdPathBuf	path = store_fixtures_manifest(number);
	ChunksErrorError failed;
	StdVec		previous;
	bool		has;
	bool		even;

	store_open_tests_chunks_ok(chunks_file_read_previous(HOST_IO_IO_READ_DOORS(io), path.bytes.ptr, path.bytes.len,
														 &has, &previous, &failed), &failed, 189, 60);
	even = has && previous.len == newest.len && memcmp(previous.ptr, newest.ptr, newest.len) == 0;
	if (has)
		std_vec_free_Checked(&previous, 1, 1);
	std_vec_free_Checked(&path.bytes, 1, 1);
	std_vec_free_Checked(&newest, 1, 1);
	return even;
}

/* store_open_tests_started runs the startup process's open at redo point 50, with no label, which unwraps at a line and column 33. */
static void
store_open_tests_started(HostIoIo *io, uint32 line)
{
	HostError	error;

	if (!store_open_startup_open(io, 50, 0, &error))
		STD_RESULT_EXPECT_FAILED(line, 33, STORE_OPEN_TESTS_UNWRAP, &error, host_Error_Debug_fmt);
}

/* store_open_tests_read reads a file's bytes, which unwraps at a line and column. */
static StdVec
store_open_tests_read(const StdPathBuf *path, uint32 line, uint32 col)
{
	StdIoError	io_error;
	StdVec		bytes;

	store_open_tests_io_ok(std_fs_read(path->bytes.ptr, path->bytes.len, &bytes, &io_error), &io_error, line, col);
	return bytes;
}

/* store_open_tests_bytes_eq asserts at a line that a file's bytes, which it frees, are those given, with a literal message when one is given. */
static void
store_open_tests_bytes_eq(StdVec *got, const uint8 *want, size_t want_len, uint32 line, const char *message)
{
	if (got->len != want_len || memcmp(got->ptr, want, want_len) != 0)
	{
		StdBytes	left = {got->ptr, got->len};
		StdBytes	right = {want, want_len};
		StdFmtPiece piece = {message, message == NULL ? 0 : strlen(message), 0, {0}};
		StdFmtArguments arguments = {&piece, 1, NULL};

		std_assert_failed(RUST_FILE, line, 9, "==", &left, std_bytes_Debug_fmt, &right, std_bytes_Debug_fmt,
						  message == NULL ? NULL : &arguments);
	}
	std_vec_free_Checked(got, 1, 1);
}

/* store_open_tests_formatted returns a line that holds a path between two texts, as format! with the path's display makes it. */
static StdString
store_open_tests_formatted(const char *before, const StdPathBuf *path, const char *after)
{
	StdString	line;
	StdStr		shown = {path->bytes.ptr, path->bytes.len};
	StdFmtPiece pieces[] = {{before, strlen(before), 0, {0}}, STD_FMT_ARG(0), {after, strlen(after), 0, {0}}};
	StdFmtArg	args[] = {{&shown, std_path_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	std_fmt_format_Checked(&line, &arguments);
	return line;
}

/* store_open_tests_warnings_eq asserts at a line that the simulation's reports at WARNING are of the data-corrupted SQLSTATE with the messages given, and drops both. */
static void
store_open_tests_warnings_eq(StdString *said, size_t said_len, uint32 line)
{
	StdVec		warnings = simc_take_warnings();
	StdVec		want;
	bool		same = warnings.len == said_len;

	std_vec_with_capacity_Checked(&want, said_len, sizeof(simc_TupleSqlstateString_t),
								  _Alignof(simc_TupleSqlstateString_t));
	for (size_t k = 0; k < said_len; k++)
	{
		simc_TupleSqlstateString_t warning = {{'X', 'X', '0', '0', '1'}, said[k]};

		std_vec_push_Checked(&want, &warning, sizeof(warning), _Alignof(simc_TupleSqlstateString_t));
	}
	for (size_t k = 0; same && k < said_len; k++)
	{
		const simc_TupleSqlstateString_t *got = &STD_VEC_AT(&warnings, simc_TupleSqlstateString_t, k);

		same = memcmp(got->f0, "XX001", 5) == 0 && got->f1.len == said[k].len &&
			memcmp(got->f1.ptr, said[k].ptr, said[k].len) == 0;
	}
	if (!same)
		std_assert_failed(RUST_FILE, line, 9, "==", &warnings, store_tests_Warnings_Debug_fmt, &want,
						  store_tests_Warnings_Debug_fmt, NULL);
	store_tests_Warnings_drop(&want);
	store_tests_Warnings_drop(&warnings);
}

/* store_open_tests_taken drops the simulation's reports at WARNING and at LOG. */
static void
store_open_tests_taken(void)
{
	StdVec		warnings = simc_take_warnings();
	StdVec		logs = simc_take_logs();

	store_tests_Warnings_drop(&warnings);
	store_tests_Logs_drop(&logs);
}

/* store_open_tests_opens reports whether a repository opens in pg_default, dropping what the open returns. */
static bool
store_open_tests_opens(HostIoIo *io, uint32 number)
{
	RepoRepo	repo;
	ChunksErrorError failed;

	if (!repo_Repo_open(io, store_fixtures_id(number), HOST_PG_DEFAULT, &repo, &failed))
	{
		chunks_error_Error_drop(&failed);
		return false;
	}
	repo_Repo_drop(&repo);
	return true;
}

/*
 * test_store_open_tests_a_start_evens_each_repository_whose_manifests_differ:
 * a start writes once more the manifest of each repository whose generations
 * differ and whose newest root reads, reporting nothing, and leaves a
 * directory that holds no pack table's file, which names no repository, as
 * it is.
 */
void
test_store_open_tests_a_start_evens_each_repository_whose_manifests_differ(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	StdPathBuf	stray;
	StdPathBuf	note;
	StdIoError	io_error;
	StdVec		warnings;
	StdVec		logs;
	StdVec		bytes;
	static const char kept[] = "not a repository";

	store_fixtures_made(io, 911);
	store_fixtures_made(io, 912);
	stray = host_tablespace_dir(913, HOST_PG_DEFAULT);
	store_open_tests_io_ok(std_fs_create_dir_all(stray.bytes.ptr, stray.bytes.len, &io_error), &io_error, 201, 41);
	std_path_join(stray.bytes.ptr, stray.bytes.len, "note", 4, &note);
	store_open_tests_io_ok(std_fs_write(note.bytes.ptr, note.bytes.len, (const uint8 *) kept, strlen(kept), &io_error),
						   &io_error, 202, 65);
	store_open_tests_taken();
	STD_ASSERT(204, 9, !store_open_tests_even(io, 911) && !store_open_tests_even(io, 912),
			   "!even(io, 911) && !even(io, 912)");
	store_open_tests_started(io, 205);
	STD_ASSERT(206, 9, store_open_tests_even(io, 911) && store_open_tests_even(io, 912),
			   "even(io, 911) && even(io, 912)");
	warnings = simc_take_warnings();
	logs = simc_take_logs();
	if (warnings.len != 0 || logs.len != 0)
	{
		StdVec		none = {0};
		StdTupleRef left = {2, {STD_ITEM(&warnings, store_tests_Warnings_Debug_fmt),
		STD_ITEM(&logs, store_tests_Logs_Debug_fmt)}};
		StdTupleRef right = {2, {STD_ITEM(&none, store_tests_Warnings_Debug_fmt),
		STD_ITEM(&none, store_tests_Logs_Debug_fmt)}};

		std_assert_failed(RUST_FILE, 207, 9, "==", &left, std_tuple_Debug_fmt, &right, std_tuple_Debug_fmt, NULL);
	}
	store_tests_Logs_drop(&logs);
	store_tests_Warnings_drop(&warnings);
	bytes = store_open_tests_read(&note, 208, 54);
	store_open_tests_bytes_eq(&bytes, (const uint8 *) kept, strlen(kept), 208, NULL);
	std_vec_free_Checked(&note.bytes, 1, 1);
	std_vec_free_Checked(&stray.bytes, 1, 1);
	simc_TestIo_drop(&test_io);
}

/*
 * test_store_open_tests_a_start_warns_of_a_repository_it_cant_settle_and_opens_the_others:
 * a repository whose settle is refused, its pack table's file gone with its
 * previous generation, is warned of with Postgres' data-corrupted SQLSTATE
 * naming the file, and a later open of it raises the same error; the start
 * goes on: the other repository is settled and evened, and opens after.
 */
void
test_store_open_tests_a_start_warns_of_a_repository_it_cant_settle_and_opens_the_others(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	StdPathBuf	dir;
	StdPathBuf	table;
	StdPathBuf	previous;
	StdIoError	io_error;
	StdVec		warnings;
	StdString	refusal;
	StdString	said;
	StdFmtPiece pieces[] = {STD_FMT_LIT("database 914: "), STD_FMT_ARG(0)};
	StdFmtArg	args[1];
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};
	RepoRepo	repo;
	ChunksErrorError failed;
	HostError	later;

	store_fixtures_made(io, 914);
	store_fixtures_made(io, 915);
	dir = host_tablespace_dir(914, HOST_PG_DEFAULT);
	std_path_join(dir.bytes.ptr, dir.bytes.len, CHUNKS_PACKS_TABLE, strlen(CHUNKS_PACKS_TABLE), &table);
	store_open_tests_io_ok(std_fs_remove_file(table.bytes.ptr, table.bytes.len, &io_error), &io_error, 221, 38);
	previous = chunks_file_previous(table.bytes.ptr, table.bytes.len);
	(void) std_fs_remove_file(previous.bytes.ptr, previous.bytes.len, &io_error);
	std_vec_free_Checked(&previous.bytes, 1, 1);
	warnings = simc_take_warnings();
	store_tests_Warnings_drop(&warnings);
	store_open_tests_started(io, 224);
	refusal = store_open_tests_formatted("required file \"", &table, "\" does not exist");
	args[0].value = &refusal;
	args[0].fmt = std_String_Display_fmt;
	std_fmt_format_Checked(&said, &arguments);
	store_open_tests_warnings_eq(&said, 1, 226);
	if (repo_Repo_open(io, store_fixtures_id(914), HOST_PG_DEFAULT, &repo, &failed))
	{
		repo_Repo_drop(&repo);
		STD_RESULT_EXPECT_FAILED(227, 88, STORE_OPEN_TESTS_UNWRAP_ERR, STD_DANGLING, std_unit_Debug_fmt);
	}
	later = chunks_packs_shared_raised(&failed);
	store_tests_raised_eq(RUST_FILE, &later, "XX001", refusal.ptr, refusal.len, 228, 9);
	STD_ASSERT(229, 9, store_open_tests_even(io, 915), "even(io, 915)");
	STD_ASSERT(230, 9, store_open_tests_opens(io, 915), "Repo::open(io, id(915), host::PG_DEFAULT).is_ok()");
	std_String_drop(&refusal);
	std_vec_free_Checked(&table.bytes, 1, 1);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	simc_TestIo_drop(&test_io);
}

/*
 * test_store_open_tests_a_start_keeps_both_generations_of_a_manifest_whose_newest_is_not_whole:
 * a repository whose newest manifest is not whole keeps both generations for
 * redo's fallback, is warned of, and has the WAL that fallback replays read
 * through, from the redo point of the generation it falls back to.
 */
void
test_store_open_tests_a_start_keeps_both_generations_of_a_manifest_whose_newest_is_not_whole(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	StdPathBuf	path;
	StdIoError	io_error;
	StdVec		bytes;
	StdVec		warnings;
	StdVec		after;
	HostLsn		from;
	HostLsn		to;

	store_fixtures_made(io, 916);
	path = store_fixtures_manifest(916);
	bytes = store_open_tests_read(&path, 241, 46);
	std_index_check(0, bytes.len, RUST_FILE, 242, 9);
	STD_VEC_AT(&bytes, uint8, 0) ^= 0xff;
	store_open_tests_io_ok(std_fs_write(path.bytes.ptr, path.bytes.len, bytes.ptr, bytes.len, &io_error), &io_error,
						   243, 39);
	warnings = simc_take_warnings();
	(void) simc_kept_wal_read(&from, &to);
	store_tests_Warnings_drop(&warnings);
	store_open_tests_started(io, 245);
	warnings = simc_take_warnings();
	store_tests_warned_once(RUST_FILE, &warnings, "database 916", 247);
	after = store_open_tests_read(&path, 251, 41);
	store_open_tests_bytes_eq(&after, bytes.ptr, bytes.len, 251, "the newest manifest was written over");
	store_tests_kept_eq(RUST_FILE, true, 10, 50, 252, NULL);
	std_vec_free_Checked(&bytes, 1, 1);
	std_vec_free_Checked(&path.bytes, 1, 1);
	simc_TestIo_drop(&test_io);
}

/* store_open_tests_ends_pack reports whether a directory entry's name has the extension "pack", as Path::extension finds it. */
static bool
store_open_tests_ends_pack(const StdString *name)
{
	static const char extension[] = ".pack";
	size_t		len = strlen(extension);

	return name->len > len && memcmp((const char *) name->ptr + name->len - len, extension, len) == 0;
}

/* store_open_tests_line_t is the lines and columns of a damage of a root's pack, which the two tests that damage one give. */
typedef struct store_open_tests_line_t
{
	uint32		read_dir;
	uint32		read;
	uint32		at;
	uint32		write;
} store_open_tests_line_t;

/* store_open_tests_damaged damages the byte at the middle of a root's place in each pack of a repository's directory. */
static void
store_open_tests_damaged(const StdPathBuf *dir, ChunksTrailerLocator locator, const store_open_tests_line_t *lines)
{
	StdVec		names;
	StdIoError	io_error;
	bool		entry;

	if (!std_fs_read_dir_names(dir->bytes.ptr, dir->bytes.len, &names, &entry, &io_error))
	{
		if (entry)
			STD_RESULT_EXPECT_FAILED(lines->read_dir + 1, 30, STORE_OPEN_TESTS_UNWRAP, &io_error,
									 std_io_Error_Debug_fmt);
		STD_RESULT_EXPECT_FAILED(lines->read_dir, 46, STORE_OPEN_TESTS_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	}
	for (size_t k = 0, k_len = names.len; k < k_len; k++)
	{
		const StdString *name = &STD_VEC_AT(&names, StdString, k);
		StdPathBuf	path;
		StdVec		bytes;
		size_t		at;

		if (!store_open_tests_ends_pack(name))
			continue;
		std_path_join(dir->bytes.ptr, dir->bytes.len, name->ptr, name->len, &path);
		bytes = store_open_tests_read(&path, lines->read, 54);
		at = STD_ADD(usize, std_usize_try_from_u64(chunks_trailer_Locator_offset(&locator)),
					 std_usize_try_from_u64(chunks_trailer_Locator_length(&locator) / 2), lines->at, 26);
		std_index_check(at, bytes.len, RUST_FILE, lines->at + 1, 17);
		STD_VEC_AT(&bytes, uint8, at) ^= 0xff;
		store_open_tests_io_ok(std_fs_write(path.bytes.ptr, path.bytes.len, bytes.ptr, bytes.len, &io_error),
							   &io_error, lines->write, 46);
		std_vec_free_Checked(&bytes, 1, 1);
		std_vec_free_Checked(&path.bytes, 1, 1);
	}
	store_tests_Logs_drop(&names);
}

/*
 * store_open_tests_root_damaged is a start over a repository whose newest
 * manifest's base tree's root, or identity tree's, can't be read, a byte of
 * the root's place in its pack damaged: it keeps both generations, warns,
 * and reads the WAL from the newest's redo point. First is the line of the
 * test's made, and kept is its assertion's text that the manifest kept both
 * generations.
 */
static void
store_open_tests_root_damaged(uint32 number, bool identity, uint32 first, const char *kept)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	StdVec		newest;
	RepoManifest manifest;
	ChunksErrorError failed;
	ChunksTrailerLocator locator;
	StdPathBuf	dir;
	StdPathBuf	path;
	StdVec		before;
	StdVec		warnings;
	StdVec		after;
	HostLsn		from;
	HostLsn		to;
	bool		even;
	bool		same;
	char		needle[32];
	store_open_tests_line_t lines = {first + 5, first + 8, first + 9, first + 11};

	store_fixtures_made(io, number);
	newest = store_open_tests_newest(io, number, first + 1, 66, 75);
	store_open_tests_chunks_ok(repo_Manifest_decode(newest.ptr, newest.len, trees_published_RepoId_unique(), &manifest,
													&failed), &failed, first + 3, 57);
	if (!(identity ? manifest.has_identity : manifest.has_bases))
		std_option_unwrap_failed(RUST_FILE, first + 3, identity ? 102 : 99);
	locator = identity ? manifest.identity.locator : manifest.bases.locator;
	repo_Manifest_drop(&manifest);
	dir = host_tablespace_dir(number, HOST_PG_DEFAULT);
	store_open_tests_damaged(&dir, locator, &lines);
	path = store_fixtures_manifest(number);
	before = store_open_tests_read(&path, first + 14, 51);
	warnings = simc_take_warnings();
	(void) simc_kept_wal_read(&from, &to);
	store_tests_Warnings_drop(&warnings);
	store_open_tests_started(io, first + 16);
	warnings = simc_take_warnings();
	snprintf(needle, sizeof(needle), "database %u", number);
	store_tests_warned_once(RUST_FILE, &warnings, needle, first + 18);
	even = store_open_tests_even(io, number);
	same = false;
	if (!even)
	{
		after = store_open_tests_read(&path, first + 22, 64);
		same = after.len == before.len && memcmp(after.ptr, before.ptr, before.len) == 0;
		std_vec_free_Checked(&after, 1, 1);
	}
	if (even || !same)
		std_panic(RUST_FILE, first + 22, 9, kept, strlen(kept));
	store_tests_kept_eq(RUST_FILE, true, 20, 50, first + 23, NULL);
	std_vec_free_Checked(&before, 1, 1);
	std_vec_free_Checked(&path.bytes, 1, 1);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	std_vec_free_Checked(&newest, 1, 1);
	simc_TestIo_drop(&test_io);
}

/*
 * test_store_open_tests_a_start_keeps_both_generations_of_a_manifest_whose_newest_root_cant_be_read:
 * a repository whose newest manifest's base tree's root can't be read, a
 * byte of the root's place in its pack damaged, keeps both generations for
 * redo's fallback, is warned of, and has the WAL that fallback replays read
 * through, from the newest's redo point.
 */
void
test_store_open_tests_a_start_keeps_both_generations_of_a_manifest_whose_newest_root_cant_be_read(void)
{
	store_open_tests_root_damaged(917, false, 262,
								  "assertion failed: !even(io, 917) && std::fs::read(manifest(917)).unwrap() == before");
}

/*
 * test_store_open_tests_a_start_reports_what_each_settle_changed_and_removes_a_dropped_repositorys_directories:
 * a start reports what each settle changed once: a pack a crash left
 * unfinished removed at LOG with its path, a pack table read as its previous
 * generation at WARNING with Postgres' data-corrupted SQLSTATE, and a dropped
 * repository's directories removed at LOG.
 */
void
test_store_open_tests_a_start_reports_what_each_settle_changed_and_removes_a_dropped_repositorys_directories(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	StdPathBuf	dir;
	StdPathBuf	left;
	StdPathBuf	table;
	StdPathBuf	previous;
	StdPathBuf	dropped;
	StdPathBuf	mark;
	StdIoError	io_error;
	StdString	said[2];
	StdString	fallback;
	StdVec		logs;
	bool		same;
	static const char unfinished[] = "unfinished";
	static const char removed_dirs[] = "removed the directories of dropped database 919";

	store_fixtures_made(io, 918);
	store_fixtures_made(io, 919);
	dir = host_tablespace_dir(918, HOST_PG_DEFAULT);
	std_path_join(dir.bytes.ptr, dir.bytes.len, "0123456789abcdef0123456789abcdef.pack.tmp", 41, &left);
	store_open_tests_io_ok(std_fs_write(left.bytes.ptr, left.bytes.len, (const uint8 *) unfinished, strlen(unfinished),
										&io_error), &io_error, 299, 46);
	std_path_join(dir.bytes.ptr, dir.bytes.len, CHUNKS_PACKS_TABLE, strlen(CHUNKS_PACKS_TABLE), &table);
	previous = chunks_file_previous(table.bytes.ptr, table.bytes.len);
	store_open_tests_io_ok(std_fs_rename(table.bytes.ptr, table.bytes.len, previous.bytes.ptr, previous.bytes.len,
										 &io_error), &io_error, 301, 65);
	std_vec_free_Checked(&previous.bytes, 1, 1);
	dropped = host_tablespace_dir(919, HOST_PG_DEFAULT);
	std_path_join(dropped.bytes.ptr, dropped.bytes.len, "dropped", 7, &mark);
	store_open_tests_io_ok(std_fs_write(mark.bytes.ptr, mark.bytes.len, (const uint8 *) "", 0, &io_error), &io_error,
						   303, 54);
	std_vec_free_Checked(&mark.bytes, 1, 1);
	store_open_tests_taken();
	store_open_tests_started(io, 305);
	said[0] = store_open_tests_formatted("database 918: removed 1 file a crash left behind, the first \"", &left, "\"");
	std_string_from_Checked(&said[1], removed_dirs, strlen(removed_dirs));
	logs = simc_take_logs();
	same = logs.len == lengthof(said);
	for (size_t k = 0; same && k < lengthof(said); k++)
		same = STD_VEC_AT(&logs, StdString, k).len == said[k].len &&
			memcmp(STD_VEC_AT(&logs, StdString, k).ptr, said[k].ptr, said[k].len) == 0;
	if (!same)
	{
		StdVec		want = {said, lengthof(said), lengthof(said)};

		std_assert_failed(RUST_FILE, 307, 9, "==", &logs, store_tests_Logs_Debug_fmt, &want, store_tests_Logs_Debug_fmt,
						  NULL);
	}
	store_tests_Logs_drop(&logs);
	std_String_drop(&said[1]);
	std_String_drop(&said[0]);
	fallback = store_open_tests_formatted("database 918: list file \"", &table,
										  "\" read as its previous generation (Missing) and written whole again");
	store_open_tests_warnings_eq(&fallback, 1, 312);
	STD_ASSERT(313, 9, !std_path_Path_exists(left.bytes.ptr, left.bytes.len) &&
			   !std_path_Path_exists(dropped.bytes.ptr, dropped.bytes.len) &&
			   std_path_Path_exists(table.bytes.ptr, table.bytes.len),
			   "!left.exists() && !dropped.exists() && table.exists()");
	STD_ASSERT(314, 9, store_open_tests_opens(io, 918), "Repo::open(io, id(918), host::PG_DEFAULT).is_ok()");
	std_vec_free_Checked(&dropped.bytes, 1, 1);
	std_vec_free_Checked(&table.bytes, 1, 1);
	std_vec_free_Checked(&left.bytes, 1, 1);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	simc_TestIo_drop(&test_io);
}

/*
 * test_store_open_tests_a_damaged_list_files_warning_words_its_lost_records_by_their_count:
 * a list file damaged with one whole record after the damage is warned of in
 * the singular, and one with more in the plural, each as one message.
 */
void
test_store_open_tests_a_damaged_list_files_warning_words_its_lost_records_by_their_count(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	StdPathBuf	dir = host_tablespace_dir(920, HOST_PG_DEFAULT);
	StdPathBuf	path;
	StdString	context;
	StdString	damaged;
	StdString	said[2];
	StdVec		warnings;
	HostError	error;
	static const size_t afters[] = {1, 2};
	static const char *const endings[] = {" 1 whole record after it is lost until the WAL restores it",
	" 2 whole records after it are lost until the WAL restores them"};

	std_path_join(dir.bytes.ptr, dir.bytes.len, CHUNKS_PACKS_TABLE, strlen(CHUNKS_PACKS_TABLE), &path);
	warnings = simc_take_warnings();
	store_tests_Warnings_drop(&warnings);
	std_string_from_Checked(&context, "database 920", sizeof("database 920"));
	for (size_t k = 0; k < lengthof(afters); k++)
	{
		ChunksPacksSettleSettled settled;
		chunks_TuplePathBufDamage_t entry = {.f1 = {.offset = 40}};

		std_vec_with_capacity_Checked(&entry.f1.after, afters[k], sizeof(StdVec), _Alignof(StdVec));
		for (size_t r = 0; r < afters[k]; r++)
		{
			StdVec		record;

			std_slice_to_vec_Checked(&record, (const uint8 *) "record", 6, 1, 1);
			std_vec_push_Checked(&entry.f1.after, &record, sizeof(record), _Alignof(StdVec));
		}
		std_vec_clone(&entry.f0.bytes, &path.bytes, 1, 1);
		chunks_packs_settle_Settled_Default_default(&settled);
		std_vec_push_Checked(&settled.damaged, &entry, sizeof(entry), _Alignof(chunks_TuplePathBufDamage_t));
		if (!store_open_report(io, &context, &settled, &error))
			STD_RESULT_EXPECT_FAILED(328, 50, STORE_OPEN_TESTS_UNWRAP, &error, host_Error_Debug_fmt);
		chunks_packs_settle_Settled_drop(&settled);
	}
	damaged = store_open_tests_formatted("database 920: list file \"", &path, "\" was damaged at offset 40, so the");
	for (size_t k = 0; k < lengthof(said); k++)
	{
		std_string_from_Checked(&said[k], damaged.ptr, damaged.len);
		std_string_push_str_Checked(&said[k], endings[k], strlen(endings[k]));
	}
	store_open_tests_warnings_eq(said, lengthof(said), 335);
	std_String_drop(&damaged);
	std_String_drop(&context);
	std_vec_free_Checked(&path.bytes, 1, 1);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	simc_TestIo_drop(&test_io);
}

/*
 * test_store_open_tests_a_start_keeps_both_generations_of_a_manifest_whose_newest_identity_root_cant_be_read:
 * design item 10's identity tree: a repository whose newest manifest's
 * identity tree's root can't be read keeps both generations for redo's
 * fallback, is warned of, and has the WAL that fallback replays read
 * through, as one whose base tree's root can't be read does.
 */
void
test_store_open_tests_a_start_keeps_both_generations_of_a_manifest_whose_newest_identity_root_cant_be_read(void)
{
	store_open_tests_root_damaged(918, true, 345,
								  "assertion failed: !even(io, 918) && std::fs::read(manifest(918)).unwrap() == before");
}

#endif							/* DOLTGRES_TEST_STORE */
