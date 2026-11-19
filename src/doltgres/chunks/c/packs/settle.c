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
 * The startup process's settle of a repository's packs (chunks'
 * packs/settle.rs, design item 6): before redo, with no record of the
 * repository made and no other process in it, it mends what a crash or
 * damage left in the repository's directories and list files.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/file.h"
#include "doltgres/chunks/index.h"
#include "doltgres/chunks/pack.h"
#include "doltgres/chunks/packs.h"
#include "doltgres/chunks/packs/dropped.h"
#include "doltgres/chunks/packs/lists.h"
#include "doltgres/chunks/packs/merge.h"
#include "doltgres/chunks/packs/open.h"
#include "doltgres/chunks/packs/settle.h"
#include "doltgres/chunks/packs/view.h"
#include "doltgres/chunks/trace.h"
#include "doltgres/host/lib.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/num.h"
#include "doltgres/std/path.h"
#include "doltgres/std/slice.h"
#include "doltgres/std/sort.h"
#include "doltgres/std/str.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"

#ifdef DOLTGRES_TEST_CHUNKS
#include "doltgres/chunks/packs/fixtures.h"
#include "doltgres/chunks/packs/retire.h"
#include "doltgres/simc/simc.h"
#include "doltgres/std/sync.h"
#include "doltgres/std/test.h"
#include "doltgres/std/test_fs.h"
#endif

#define RUST_FILE "chunks" STD_SEP "src" STD_SEP "packs" STD_SEP "settle.rs"

/* The sort of a pack's (Address, Locator) entries that rebuilt runs: core's shared sort parts at the tuple, which its stable sort calls. */
#define STD_SORT_SCOPE static
#define STD_SORT_DECLARE
#define STD_SORT_DEFINE
#define STD_SORT_SHARED_ONLY
#define STD_SORT_WITHOUT_UNSTABLE
#define STD_SORT_TS TupleAddressLocator
#define STD_SORT_T chunks_TupleAddressLocator_t
#define STD_SORT_FREEZE 1
#include "doltgres/std/sort_template.h"

/* The stable sort_by_key of a pack's (Address, Locator) entries by their address and offset, which rebuilt runs. */
#define STD_STABLE_SORT_SCOPE static
#define STD_STABLE_SORT_DECLARE
#define STD_STABLE_SORT_DEFINE
#define STD_STABLE_SORT_TS TupleAddressLocator
#define STD_STABLE_SORT_T chunks_TupleAddressLocator_t
#define STD_STABLE_SORT_FREEZE 1
#define STD_STABLE_SORT_WITHOUT_SORT
#define STD_STABLE_SORT_WITH_BY_KEY
#define STD_STABLE_SORT_KEY_KS TupleAddressU64
#define STD_STABLE_SORT_KEY_K chunks_TupleAddressU64_t
#define STD_STABLE_SORT_KEY_LT(a, b) chunks_pack_TupleAddressU64_lt((a), (b))
#include "doltgres/std/stable_sort_template.h"

/* chunks_packs_settle_Settled_Default_default is Settled's derived Default: nothing changed. */
void
chunks_packs_settle_Settled_Default_default(ChunksPacksSettleSettled *result)
{
	std_vec_new(&result->removed, sizeof(StdPathBuf), _Alignof(StdPathBuf));
	std_vec_new(&result->moved, sizeof(uint32), _Alignof(uint32));
	std_vec_new(&result->fallbacks, sizeof(chunks_TuplePathBufFallback_t), _Alignof(chunks_TuplePathBufFallback_t));
	std_vec_new(&result->damaged, sizeof(chunks_TuplePathBufDamage_t), _Alignof(chunks_TuplePathBufDamage_t));
	std_vec_new(&result->rebuilt, sizeof(chunks_TuplePathBufLost_t), _Alignof(chunks_TuplePathBufLost_t));
	std_vec_new(&result->unlisted, sizeof(uint64), _Alignof(uint64));
	result->dropped = false;
}

/* chunks_packs_settle_Paths_Debug_fmt is a Vec<PathBuf>'s Debug. */
static bool
chunks_packs_settle_Paths_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *paths = value;
	StdDebugList l = std_fmt_debug_list(f);

	for (size_t k = 0, k_len = paths->len; k < k_len; k++)
		std_DebugList_entry(&l, &STD_VEC_AT(paths, StdPathBuf, k), std_PathBuf_Debug_fmt);
	return std_DebugList_finish(&l);
}

/* chunks_packs_settle_U32s_Debug_fmt is a Vec<u32>'s Debug. */
static bool
chunks_packs_settle_U32s_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *numbers = value;
	StdDebugList l = std_fmt_debug_list(f);

	for (size_t k = 0, k_len = numbers->len; k < k_len; k++)
		std_DebugList_entry(&l, &STD_VEC_AT(numbers, uint32, k), std_u32_Debug_fmt);
	return std_DebugList_finish(&l);
}

/* chunks_packs_settle_U64s_Debug_fmt is a Vec<u64>'s Debug. */
static bool
chunks_packs_settle_U64s_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *numbers = value;
	StdDebugList l = std_fmt_debug_list(f);

	for (size_t k = 0, k_len = numbers->len; k < k_len; k++)
		std_DebugList_entry(&l, &STD_VEC_AT(numbers, uint64, k), std_u64_Debug_fmt);
	return std_DebugList_finish(&l);
}

/* chunks_packs_settle_Fallback_pair_Debug_fmt is a (PathBuf, Fallback)'s Debug. */
static bool
chunks_packs_settle_Fallback_pair_Debug_fmt(const void *value, StdFormatter *f)
{
	const chunks_TuplePathBufFallback_t *pair = value;
	StdTupleRef tuple = {2, {{&pair->f0, std_PathBuf_Debug_fmt}, {&pair->f1, chunks_file_Fallback_Debug_fmt}}};

	return std_tuple_Debug_fmt(&tuple, f);
}

/* chunks_packs_settle_Damage_pair_Debug_fmt is a (PathBuf, Damage)'s Debug. */
static bool
chunks_packs_settle_Damage_pair_Debug_fmt(const void *value, StdFormatter *f)
{
	const chunks_TuplePathBufDamage_t *pair = value;
	StdTupleRef tuple = {2, {{&pair->f0, std_PathBuf_Debug_fmt}, {&pair->f1, chunks_file_Damage_Debug_fmt}}};

	return std_tuple_Debug_fmt(&tuple, f);
}

/* chunks_packs_settle_Lost_pair_Debug_fmt is a (PathBuf, Lost)'s Debug. */
static bool
chunks_packs_settle_Lost_pair_Debug_fmt(const void *value, StdFormatter *f)
{
	const chunks_TuplePathBufLost_t *pair = value;
	StdTupleRef tuple = {2, {{&pair->f0, std_PathBuf_Debug_fmt}, {&pair->f1, chunks_pack_Lost_Debug_fmt}}};

	return std_tuple_Debug_fmt(&tuple, f);
}

/* chunks_packs_settle_Pairs_Debug_fmt is a Vec of pairs' Debug, by the size and Debug of a pair. */
static bool
chunks_packs_settle_Pairs_Debug_fmt(const StdVec *pairs, size_t size, StdFmtFn fmt, StdFormatter *f)
{
	StdDebugList l = std_fmt_debug_list(f);

	for (size_t k = 0, k_len = pairs->len; k < k_len; k++)
		std_DebugList_entry(&l, STD_VEC_AT_SIZE(pairs, k, size), fmt);
	return std_DebugList_finish(&l);
}

/* chunks_packs_settle_Fallbacks_Debug_fmt is a Vec<(PathBuf, Fallback)>'s Debug. */
static bool
chunks_packs_settle_Fallbacks_Debug_fmt(const void *value, StdFormatter *f)
{
	return chunks_packs_settle_Pairs_Debug_fmt(value, sizeof(chunks_TuplePathBufFallback_t),
											   chunks_packs_settle_Fallback_pair_Debug_fmt, f);
}

/* chunks_packs_settle_Damages_Debug_fmt is a Vec<(PathBuf, Damage)>'s Debug. */
static bool
chunks_packs_settle_Damages_Debug_fmt(const void *value, StdFormatter *f)
{
	return chunks_packs_settle_Pairs_Debug_fmt(value, sizeof(chunks_TuplePathBufDamage_t),
											   chunks_packs_settle_Damage_pair_Debug_fmt, f);
}

/* chunks_packs_settle_Losses_Debug_fmt is a Vec<(PathBuf, Lost)>'s Debug. */
static bool
chunks_packs_settle_Losses_Debug_fmt(const void *value, StdFormatter *f)
{
	return chunks_packs_settle_Pairs_Debug_fmt(value, sizeof(chunks_TuplePathBufLost_t),
											   chunks_packs_settle_Lost_pair_Debug_fmt, f);
}

/* chunks_packs_settle_Settled_Debug_fmt is Settled's derived Debug. */
bool
chunks_packs_settle_Settled_Debug_fmt(const void *value, StdFormatter *f)
{
	const ChunksPacksSettleSettled *self = value;
	StdDebugStruct s = std_fmt_debug_struct(f, "Settled", 7);

	std_DebugStruct_field(&s, "removed", 7, &self->removed, chunks_packs_settle_Paths_Debug_fmt);
	std_DebugStruct_field(&s, "moved", 5, &self->moved, chunks_packs_settle_U32s_Debug_fmt);
	std_DebugStruct_field(&s, "fallbacks", 9, &self->fallbacks, chunks_packs_settle_Fallbacks_Debug_fmt);
	std_DebugStruct_field(&s, "damaged", 7, &self->damaged, chunks_packs_settle_Damages_Debug_fmt);
	std_DebugStruct_field(&s, "rebuilt", 7, &self->rebuilt, chunks_packs_settle_Losses_Debug_fmt);
	std_DebugStruct_field(&s, "unlisted", 8, &self->unlisted, chunks_packs_settle_U64s_Debug_fmt);
	std_DebugStruct_field(&s, "dropped", 7, &self->dropped, std_bool_Debug_fmt);
	return std_DebugStruct_finish(&s);
}

/* chunks_packs_settle_path_eq reports whether two paths are equal, as Path's PartialEq compares them. */
static bool
chunks_packs_settle_path_eq(const StdPathBuf *a, const StdPathBuf *b)
{
	return std_path_PartialEq_eq(a->bytes.ptr, a->bytes.len, b->bytes.ptr, b->bytes.len);
}

/* chunks_packs_settle_Settled_PartialEq_eq is Settled's derived PartialEq: each field in order. */
bool
chunks_packs_settle_Settled_PartialEq_eq(const ChunksPacksSettleSettled *self, const ChunksPacksSettleSettled *other)
{
	if (self->removed.len != other->removed.len)
		return false;
	for (size_t k = 0, k_len = self->removed.len; k < k_len; k++)
		if (!chunks_packs_settle_path_eq(&STD_VEC_AT(&self->removed, StdPathBuf, k),
										 &STD_VEC_AT(&other->removed, StdPathBuf, k)))
			return false;
	if (self->moved.len != other->moved.len ||
		(self->moved.len != 0 && memcmp(self->moved.ptr, other->moved.ptr, self->moved.len * sizeof(uint32)) != 0))
		return false;
	if (self->fallbacks.len != other->fallbacks.len)
		return false;
	for (size_t k = 0, k_len = self->fallbacks.len; k < k_len; k++)
	{
		const chunks_TuplePathBufFallback_t *a = &STD_VEC_AT(&self->fallbacks, chunks_TuplePathBufFallback_t, k);
		const chunks_TuplePathBufFallback_t *b = &STD_VEC_AT(&other->fallbacks, chunks_TuplePathBufFallback_t, k);

		if (!chunks_packs_settle_path_eq(&a->f0, &b->f0) || !chunks_file_Fallback_PartialEq_eq(&a->f1, &b->f1))
			return false;
	}
	if (self->damaged.len != other->damaged.len)
		return false;
	for (size_t k = 0, k_len = self->damaged.len; k < k_len; k++)
	{
		const chunks_TuplePathBufDamage_t *a = &STD_VEC_AT(&self->damaged, chunks_TuplePathBufDamage_t, k);
		const chunks_TuplePathBufDamage_t *b = &STD_VEC_AT(&other->damaged, chunks_TuplePathBufDamage_t, k);

		if (!chunks_packs_settle_path_eq(&a->f0, &b->f0) || !chunks_file_Damage_PartialEq_eq(&a->f1, &b->f1))
			return false;
	}
	if (self->rebuilt.len != other->rebuilt.len)
		return false;
	for (size_t k = 0, k_len = self->rebuilt.len; k < k_len; k++)
	{
		const chunks_TuplePathBufLost_t *a = &STD_VEC_AT(&self->rebuilt, chunks_TuplePathBufLost_t, k);
		const chunks_TuplePathBufLost_t *b = &STD_VEC_AT(&other->rebuilt, chunks_TuplePathBufLost_t, k);

		if (!chunks_packs_settle_path_eq(&a->f0, &b->f0) || !chunks_pack_Lost_PartialEq_eq(&a->f1, &b->f1))
			return false;
	}
	if (self->unlisted.len != other->unlisted.len ||
		(self->unlisted.len != 0 &&
		 memcmp(self->unlisted.ptr, other->unlisted.ptr, self->unlisted.len * sizeof(uint64)) != 0))
		return false;
	return self->dropped == other->dropped;
}

/* chunks_packs_settle_Settled_drop is Settled's drop glue: its paths, damages, and losses, and its vectors. */
void
chunks_packs_settle_Settled_drop(ChunksPacksSettleSettled *self)
{
	std_vec_drop_Checked(&self->removed, sizeof(StdPathBuf), _Alignof(StdPathBuf), chunks_pack_PathBuf_drop);
	std_vec_free_Checked(&self->moved, sizeof(uint32), _Alignof(uint32));
	for (size_t k = 0, k_len = self->fallbacks.len; k < k_len; k++)
		std_vec_free_Checked(&STD_VEC_AT(&self->fallbacks, chunks_TuplePathBufFallback_t, k).f0.bytes, 1, 1);
	std_vec_free_Checked(&self->fallbacks, sizeof(chunks_TuplePathBufFallback_t),
						 _Alignof(chunks_TuplePathBufFallback_t));
	for (size_t k = 0, k_len = self->damaged.len; k < k_len; k++)
	{
		chunks_TuplePathBufDamage_t *pair = &STD_VEC_AT(&self->damaged, chunks_TuplePathBufDamage_t, k);

		std_vec_free_Checked(&pair->f0.bytes, 1, 1);
		chunks_file_Damage_drop(&pair->f1);
	}
	std_vec_free_Checked(&self->damaged, sizeof(chunks_TuplePathBufDamage_t), _Alignof(chunks_TuplePathBufDamage_t));
	for (size_t k = 0, k_len = self->rebuilt.len; k < k_len; k++)
	{
		chunks_TuplePathBufLost_t *pair = &STD_VEC_AT(&self->rebuilt, chunks_TuplePathBufLost_t, k);

		std_vec_free_Checked(&pair->f0.bytes, 1, 1);
		chunks_pack_Lost_drop(&pair->f1);
	}
	std_vec_free_Checked(&self->rebuilt, sizeof(chunks_TuplePathBufLost_t), _Alignof(chunks_TuplePathBufLost_t));
	std_vec_free_Checked(&self->unlisted, sizeof(uint64), _Alignof(uint64));
}

/* chunks_packs_settle_path_buf returns a path's own copy. */
static StdPathBuf
chunks_packs_settle_path_buf(const char *path, size_t path_len)
{
	StdPathBuf	buf;

	std_path_to_path_buf(path, path_len, &buf);
	return buf;
}

/* chunks_packs_settle_got_frame_0_t is got's state across its pad: what the file holds, and the path a damaged file goes aside to. */
typedef struct chunks_packs_settle_got_frame_0_t
{
	const ChunksPacksViewListOps *ops;
	HostIoIo   *io;
	const char *path;
	size_t		path_len;
	ChunksPacksSettleSettled *settled;
	void	   *list;
	ChunksPacksViewRead *read;
	bool	   *whole;
	bool	   *has_result;
	ChunksErrorError *error;
	bool		ok;
	ChunksFileAppended appended;
	bool		appended_live;
	StdPathBuf	aside;
	bool		aside_live;
} chunks_packs_settle_got_frame_0_t;

/* chunks_packs_settle_got_pad_0 is got's pad: the path aside, then what the file holds. */
static void
chunks_packs_settle_got_pad_0(void *frame_)
{
	chunks_packs_settle_got_frame_0_t *frame = frame_;

	if (frame->aside_live)
	{
		frame->aside_live = false;
		std_vec_free_Checked(&frame->aside.bytes, 1, 1);
	}
	if (frame->appended_live)
	{
		frame->appended_live = false;
		chunks_file_Appended_drop(&frame->appended);
	}
}

/*
 * chunks_packs_settle_got_body_0 is got's body: the file's records applied
 * to the list, the fallback reported, and a damaged newest kept beside the
 * file and reported.
 */
static void
chunks_packs_settle_got_body_0(void *frame_)
{
	chunks_packs_settle_got_frame_0_t *frame = frame_;
	ChunksFileAppended *appended = &frame->appended;
	bool		has;
	uint64		past;
	bool		whole;

	frame->ok = false;
	*frame->has_result = false;
	if (!chunks_file_read_appended(HOST_IO_IO_READ_DOORS(frame->io), frame->path, frame->path_len, &has, appended,
								   frame->error))
		return;
	if (!has)
	{
		frame->ok = true;
		return;
	}
	frame->appended_live = true;
	if (appended->records.len == 0 || appended->kept)
	{
		StdString	message;
		StdStr		shown = {frame->path, frame->path_len};
		StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(" is no list file")};
		StdFmtArg	args[] = {{&shown, std_path_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_fmt_format_Checked(&message, &arguments);
		*frame->error = chunks_error_Error_corrupt(&message);
		goto done;
	}
	past = (uint64) appended->records.len - 1;
	for (size_t k = 0, k_len = appended->records.len; k < k_len; k++)
	{
		const StdVec *record = &STD_VEC_AT(&appended->records, StdVec, k);

		if (!frame->ops->apply(frame->list, record->ptr, record->len, frame->error))
			goto done;
	}
	whole = !appended->has_fallback && !appended->has_damage;
	if (appended->has_fallback)
	{
		chunks_TuplePathBufFallback_t fallback = {chunks_packs_settle_path_buf(frame->path, frame->path_len),
		appended->fallback};

		std_vec_push_Checked(&frame->settled->fallbacks, &fallback, sizeof(fallback),
							 _Alignof(chunks_TuplePathBufFallback_t));
	}
	if (appended->has_damage)
	{
		chunks_TuplePathBufDamage_t damaged;

		if (!chunks_file_damaged_name(HOST_IO_IO_IO_DOORS(frame->io), frame->path, frame->path_len, &frame->aside,
									  frame->error))
			goto done;
		frame->aside_live = true;
		if (!chunks_file_rename(HOST_IO_IO_IO_DOORS(frame->io), frame->path, frame->path_len, frame->aside.bytes.ptr,
								frame->aside.bytes.len, frame->error))
			goto done;
		damaged.f0 = chunks_packs_settle_path_buf(frame->path, frame->path_len);
		damaged.f1 = appended->damage;
		appended->has_damage = false;
		std_vec_push_Checked(&frame->settled->damaged, &damaged, sizeof(damaged),
							 _Alignof(chunks_TuplePathBufDamage_t));
	}
	frame->read->at = appended->has_end && whole ? appended->end.end : 0;
	frame->read->past = past;
	*frame->whole = appended->has_end && whole;
	*frame->has_result = true;
	frame->ok = true;
done:
	chunks_packs_settle_got_pad_0(frame);
}

/*
 * chunks_packs_settle_got reads a list file as a settle takes it: whole, or
 * as its previous generation, or up to damage, which it reports, keeping a
 * damaged newest beside the file. It answers nothing for a file neither
 * generation of which is there. The list, which holds its Default at the
 * call, holds what the file's records give.
 */
static bool
chunks_packs_settle_got(const ChunksPacksViewListOps *ops, HostIoIo *io, const char *path, size_t path_len,
						ChunksPacksSettleSettled *settled, void *list, ChunksPacksViewRead *read, bool *whole,
						bool *has_result, ChunksErrorError *error)
{
	chunks_packs_settle_got_frame_0_t frame = {.ops = ops, .io = io, .path = path, .path_len = path_len,
	.settled = settled, .list = list, .read = read, .whole = whole, .has_result = has_result, .error = error};

	std_landing(&frame, chunks_packs_settle_got_body_0, chunks_packs_settle_got_pad_0);
	return frame.ok;
}

/* chunks_packs_settle_rebuilt_frame_0_t is rebuilt's state across its pad: the pack's reader, the entries, and the loss. */
typedef struct chunks_packs_settle_rebuilt_frame_0_t
{
	HostIoIo   *io;
	const char *pack;
	size_t		pack_len;
	uint64		number;
	const char *run;
	size_t		run_len;
	ChunksPackLost *result;
	ChunksErrorError *error;
	bool		ok;
	ChunksPackPackReader reader;
	bool		reader_live;
	StdVec		entries;
	bool		entries_live;
	ChunksPackLost lost;
	bool		lost_live;
} chunks_packs_settle_rebuilt_frame_0_t;

/* chunks_packs_settle_rebuilt_pad_0 is rebuilt's pad: the loss, the entries, then the reader. */
static void
chunks_packs_settle_rebuilt_pad_0(void *frame_)
{
	chunks_packs_settle_rebuilt_frame_0_t *frame = frame_;

	if (frame->lost_live)
	{
		frame->lost_live = false;
		chunks_pack_Lost_drop(&frame->lost);
	}
	if (frame->entries_live)
	{
		frame->entries_live = false;
		std_vec_free_Checked(&frame->entries, sizeof(chunks_TupleAddressLocator_t),
							 _Alignof(chunks_TupleAddressLocator_t));
	}
	if (frame->reader_live)
	{
		frame->reader_live = false;
		chunks_pack_PackReader_drop(&frame->reader);
	}
}

/* chunks_packs_settle_rebuilt_closure_0 is rebuilt's walk: each verified chunk's address and place, pushed to the entries. */
static bool
chunks_packs_settle_rebuilt_closure_0(void *captures, ChunksFileInterruptible io, ChunksPackVerified *chunk,
									  ChunksErrorError *error)
{
	chunks_TupleAddressLocator_t entry = {chunks_pack_Verified_address(chunk), chunks_pack_Verified_locator(chunk)};

	(void) io;
	(void) error;
	std_vec_push_Checked(captures, &entry, sizeof(entry), _Alignof(chunks_TupleAddressLocator_t));
	return true;
}

/* chunks_packs_settle_rebuilt_closure_1 is rebuilt's sort key: an entry's address and its locator's offset. */
static chunks_TupleAddressU64_t
chunks_packs_settle_rebuilt_closure_1(void *captures, const void *elem)
{
	const chunks_TupleAddressLocator_t *entry = elem;
	chunks_TupleAddressU64_t key = {entry->f0, chunks_trailer_Locator_offset(&entry->f1)};

	(void) captures;
	return key;
}

/*
 * chunks_packs_settle_rebuilt_body_0 is rebuilt's body: the pack's verified
 * chunks and lost records sorted into a run written at the run's path.
 */
static void
chunks_packs_settle_rebuilt_body_0(void *frame_)
{
	chunks_packs_settle_rebuilt_frame_0_t *frame = frame_;
	ChunksFileInterruptible io = {&CHUNKS_FILE_IO_INTERRUPTIBLE_OPS, frame->io};
	ChunksIndexRunLocator run;
	bool		verified;

	frame->ok = false;
	std_vec_new(&frame->entries, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
	frame->entries_live = true;
	if (!chunks_pack_PackReader_open(HOST_IO_IO_READ_DOORS(frame->io), frame->pack, frame->pack_len, &frame->reader,
									 frame->error))
		goto done;
	frame->reader_live = true;
	verified = chunks_pack_PackReader_verified(&frame->reader, io, frame->number, chunks_packs_settle_rebuilt_closure_0,
											   &frame->entries, NULL, &frame->lost, frame->error);
	frame->reader_live = false;
	chunks_pack_PackReader_drop(&frame->reader);
	if (!verified)
		goto done;
	frame->lost_live = true;
	for (size_t k = 0, k_len = frame->lost.records.len; k < k_len; k++)
		std_vec_push_Checked(&frame->entries, &STD_VEC_AT(&frame->lost.records, chunks_TupleAddressLocator_t, k),
							 sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
	std_slice_sort_by_key_TupleAddressLocatorTupleAddressU64(frame->entries.ptr, frame->entries.len,
															 chunks_packs_settle_rebuilt_closure_1, NULL, NULL);
	if (!chunks_index_written_run(HOST_IO_IO_IO_DOORS(frame->io), frame->run, frame->run_len, frame->entries.ptr,
								  frame->entries.len, &run, frame->error))
		goto done;
	chunks_index_RunLocator_drop(&run);
	*frame->result = frame->lost;
	frame->lost_live = false;
	frame->ok = true;
done:
	chunks_packs_settle_rebuilt_pad_0(frame);
}

/*
 * chunks_packs_settle_rebuilt writes a listed pack's address-index run again
 * from the chunks its address table names, and returns what the pack lost:
 * a chunk whose record doesn't hash to its address is indexed under the
 * address the table gives, which reads refuse.
 */
bool
chunks_packs_settle_rebuilt(HostIoIo *io, const char *pack, size_t pack_len, uint64 number, const char *run,
							size_t run_len, ChunksPackLost *result, ChunksErrorError *error)
{
	chunks_packs_settle_rebuilt_frame_0_t frame = {.io = io, .pack = pack, .pack_len = pack_len, .number = number,
	.run = run, .run_len = run_len, .result = result, .error = error};

	std_landing(&frame, chunks_packs_settle_rebuilt_body_0, chunks_packs_settle_rebuilt_pad_0);
	return frame.ok;
}

/* chunks_TupleU64PathBuf_t is a listed pack's number and its file's path. */
typedef struct chunks_TupleU64PathBuf_t
{
	uint64		f0;
	StdPathBuf	f1;
} chunks_TupleU64PathBuf_t;

/* chunks_TupleU64String_t is a listed pack's number and its run's file name. */
typedef struct chunks_TupleU64String_t
{
	uint64		f0;
	StdString	f1;
} chunks_TupleU64String_t;

/*
 * chunks_packs_settle_Packs_settle_frame_0_t is settle's state across its
 * pad: what it changed, the repository's directory, the pack table and the
 * list files it holds, and the names, paths, and sets of the step it is at.
 */
typedef struct chunks_packs_settle_Packs_settle_frame_0_t
{
	HostIoIo   *io;
	uint32		repository;
	uint32		own;
	ChunksPacksSettleSettled *result;
	ChunksErrorError *error;
	bool		ok;
	ChunksPacksSettleSettled settled;
	bool		settled_live;
	StdPathBuf	dir;
	bool		dir_live;
	/* path is the pack table's file, then the forwarding file. */
	StdPathBuf	path;
	bool		path_live;
	ChunksPacksViewReadTable table;
	bool		table_live;
	/* listed is a Vec<(u64, PathBuf)> of the listed packs' files. */
	StdVec		listed;
	bool		listed_live;
	/* dirs is a Vec<PathBuf> of the repository's directories. */
	StdVec		dirs;
	bool		dirs_live;
	/* names is a Vec<String> of a directory's files' names. */
	StdVec		names;
	bool		names_live;
	/* file is a path a step joins. */
	StdPathBuf	file;
	bool		file_live;
	/* spaces is a BTreeSet<u32> of the tablespaces to settle, in order. */
	StdVec		spaces;
	bool		spaces_live;

	/*
	 * moved is a Vec<u32> of the tablespaces whose runs file's end the table
	 * takes.
	 */
	StdVec		moved;
	bool		moved_live;
	StdPathBuf	space_dir;
	bool		space_dir_live;

	/*
	 * packs is a Vec<(u64, String)> of a tablespace's listed packs' runs'
	 * file names.
	 */
	StdVec		packs;
	bool		packs_live;
	StdPathBuf	runs_path;
	bool		runs_path_live;
	/* runs is a tablespace's runs file as read, then the forwarding file. */
	ChunksPacksViewReadRuns runs;
	bool		runs_live;

	/*
	 * taken is a Vec<String> of the runs file's lines from taken_at on, which
	 * the settle has yet to keep or drop.
	 */
	StdVec		taken;
	size_t		taken_at;
	bool		taken_live;
	/* lines is a Vec<String> of the lines the runs file keeps. */
	StdVec		lines;
	bool		lines_live;
	/* named is a BTreeSet<u64> of the packs the kept lines cover. */
	StdVec		named;
	bool		named_live;
	StdPathBuf	pack;
	bool		pack_live;
	ChunksPackLost lost;
	bool		lost_live;
	StdVec		record;
	bool		record_live;
} chunks_packs_settle_Packs_settle_frame_0_t;

/* chunks_packs_settle_free_path frees a path the frame holds, while it holds it. */
static void
chunks_packs_settle_free_path(StdPathBuf *path, bool *live)
{
	if (*live)
	{
		*live = false;
		std_vec_free_Checked(&path->bytes, 1, 1);
	}
}

/* chunks_packs_settle_free_names frees a Vec<String> the frame holds, while it holds it. */
static void
chunks_packs_settle_free_names(StdVec *names, bool *live)
{
	if (*live)
	{
		*live = false;
		std_vec_drop_Checked(names, sizeof(StdString), _Alignof(StdString), std_String_drop);
	}
}

/* chunks_packs_settle_space_pad frees what the step over one tablespace holds. */
static void
chunks_packs_settle_space_pad(chunks_packs_settle_Packs_settle_frame_0_t *frame)
{
	if (frame->record_live)
	{
		frame->record_live = false;
		std_vec_free_Checked(&frame->record, 1, 1);
	}
	if (frame->lost_live)
	{
		frame->lost_live = false;
		chunks_pack_Lost_drop(&frame->lost);
	}
	chunks_packs_settle_free_path(&frame->pack, &frame->pack_live);
	if (frame->named_live)
	{
		frame->named_live = false;
		std_vec_free_Checked(&frame->named, sizeof(uint64), _Alignof(uint64));
	}
	chunks_packs_settle_free_names(&frame->lines, &frame->lines_live);
	if (frame->taken_live)
	{
		frame->taken_live = false;
		for (size_t k = frame->taken_at, k_len = frame->taken.len; k < k_len; k++)
			std_vec_free_Checked(&STD_VEC_AT(&frame->taken, StdString, k), 1, 1);
		std_vec_free_Checked(&frame->taken, sizeof(StdString), _Alignof(StdString));
	}
	if (frame->runs_live)
	{
		frame->runs_live = false;
		chunks_packs_Runs_drop(&frame->runs.list);
	}
	chunks_packs_settle_free_path(&frame->runs_path, &frame->runs_path_live);
	if (frame->packs_live)
	{
		frame->packs_live = false;
		for (size_t k = 0, k_len = frame->packs.len; k < k_len; k++)
			std_vec_free_Checked(&STD_VEC_AT(&frame->packs, chunks_TupleU64String_t, k).f1, 1, 1);
		std_vec_free_Checked(&frame->packs, sizeof(chunks_TupleU64String_t), _Alignof(chunks_TupleU64String_t));
	}
	chunks_packs_settle_free_names(&frame->names, &frame->names_live);
	chunks_packs_settle_free_path(&frame->file, &frame->file_live);
	chunks_packs_settle_free_path(&frame->space_dir, &frame->space_dir_live);
}

/* chunks_packs_settle_Packs_settle_pad_0 is settle's pad: the step's state, then what the settle holds, the latest first. */
static void
chunks_packs_settle_Packs_settle_pad_0(void *frame_)
{
	chunks_packs_settle_Packs_settle_frame_0_t *frame = frame_;

	chunks_packs_settle_space_pad(frame);
	if (frame->moved_live)
	{
		frame->moved_live = false;
		std_vec_free_Checked(&frame->moved, sizeof(uint32), _Alignof(uint32));
	}
	if (frame->spaces_live)
	{
		frame->spaces_live = false;
		std_vec_free_Checked(&frame->spaces, sizeof(uint32), _Alignof(uint32));
	}
	if (frame->dirs_live)
	{
		frame->dirs_live = false;
		std_vec_drop_Checked(&frame->dirs, sizeof(StdPathBuf), _Alignof(StdPathBuf), chunks_pack_PathBuf_drop);
	}
	if (frame->listed_live)
	{
		frame->listed_live = false;
		for (size_t k = 0, k_len = frame->listed.len; k < k_len; k++)
			std_vec_free_Checked(&STD_VEC_AT(&frame->listed, chunks_TupleU64PathBuf_t, k).f1.bytes, 1, 1);
		std_vec_free_Checked(&frame->listed, sizeof(chunks_TupleU64PathBuf_t), _Alignof(chunks_TupleU64PathBuf_t));
	}
	if (frame->table_live)
	{
		frame->table_live = false;
		chunks_packs_Table_drop(&frame->table.list);
	}
	chunks_packs_settle_free_path(&frame->path, &frame->path_live);
	chunks_packs_settle_free_path(&frame->dir, &frame->dir_live);
	if (frame->settled_live)
	{
		frame->settled_live = false;
		chunks_packs_settle_Settled_drop(&frame->settled);
	}
}

/* chunks_packs_settle_space_dir makes a path the repository's directory in a tablespace, its own for the default number. */
static void
chunks_packs_settle_space_dir(const chunks_packs_settle_Packs_settle_frame_0_t *frame, uint32 space, StdPathBuf *result)
{
	if (space == CHUNKS_PACKS_DEFAULT_SPACE)
		std_PathBuf_Clone_clone(&frame->dir, result);
	else
		*result = host_tablespace_dir(frame->repository, space);
}

/* chunks_packs_settle_joined makes the frame's file a directory's path with a name joined to it. */
static void
chunks_packs_settle_joined(chunks_packs_settle_Packs_settle_frame_0_t *frame, const StdPathBuf *dir, const char *name,
						   size_t name_len)
{
	chunks_packs_settle_free_path(&frame->file, &frame->file_live);
	std_path_join(dir->bytes.ptr, dir->bytes.len, name, name_len, &frame->file);
	frame->file_live = true;
}

/* chunks_packs_settle_removed records the frame's file as removed, which then holds none. */
static void
chunks_packs_settle_removed(chunks_packs_settle_Packs_settle_frame_0_t *frame)
{
	std_vec_push_Checked(&frame->settled.removed, &frame->file, sizeof(frame->file), _Alignof(StdPathBuf));
	frame->file_live = false;
}

/* chunks_packs_settle_ends_with reports whether a name ends with a text. */
static bool
chunks_packs_settle_ends_with(const StdString *name, const char *text)
{
	size_t		len = strlen(text);

	return name->len >= len && memcmp((const char *) name->ptr + name->len - len, text, len) == 0;
}

/* chunks_packs_settle_names_into lists a directory's files' names into the frame's names. */
static bool
chunks_packs_settle_names_into(chunks_packs_settle_Packs_settle_frame_0_t *frame, const StdPathBuf *dir)
{
	chunks_packs_settle_free_names(&frame->names, &frame->names_live);
	if (!chunks_file_file_names(HOST_IO_IO_READ_DOORS(frame->io), dir->bytes.ptr, dir->bytes.len, &frame->names,
								frame->error))
		return false;
	frame->names_live = true;
	return true;
}

/*
 * chunks_packs_settle_unlisted leaves out of a pack table that didn't read
 * whole each pack whose file is gone from a directory that is there.
 */
static bool
chunks_packs_settle_unlisted(chunks_packs_settle_Packs_settle_frame_0_t *frame)
{
	const StdVec *packs = &frame->table.list.packs;

	std_vec_new(&frame->listed, sizeof(chunks_TupleU64PathBuf_t), _Alignof(chunks_TupleU64PathBuf_t));
	frame->listed_live = true;
	for (size_t k = 0, k_len = packs->len; k < k_len; k++)
	{
		const chunks_TupleU64Listed_t *pack = &STD_VEC_AT(packs, chunks_TupleU64Listed_t, k);
		StdPathBuf	space_dir;
		StdString	name = chunks_pack_PackId_file_name(&pack->f1.id, "pack", 4);
		chunks_TupleU64PathBuf_t listed = {pack->f0};

		chunks_packs_settle_space_dir(frame, pack->f1.space, &space_dir);
		std_path_join(space_dir.bytes.ptr, space_dir.bytes.len, name.ptr, name.len, &listed.f1);
		std_vec_free_Checked(&name, 1, 1);
		std_vec_free_Checked(&space_dir.bytes, 1, 1);
		std_vec_push_Checked(&frame->listed, &listed, sizeof(listed), _Alignof(chunks_TupleU64PathBuf_t));
	}
	for (size_t k = 0, k_len = frame->listed.len; k < k_len; k++)
	{
		const chunks_TupleU64PathBuf_t *listed = &STD_VEC_AT(&frame->listed, chunks_TupleU64PathBuf_t, k);
		StdStr		parent;
		bool		there;
		bool		exists;

		if (!std_path_parent(listed->f1.bytes.ptr, listed->f1.bytes.len, &parent))
		{
			parent.ptr = frame->dir.bytes.ptr;
			parent.len = frame->dir.bytes.len;
		}
		if (!chunks_file_dir_there(HOST_IO_IO_READ_DOORS(frame->io), parent.ptr, parent.len, &there, frame->error))
			return false;
		if (!there)
			continue;
		if (!chunks_file_exists(HOST_IO_IO_READ_DOORS(frame->io), listed->f1.bytes.ptr, listed->f1.bytes.len, &exists,
								frame->error))
			return false;
		if (!exists)
		{
			(void) chunks_packs_table_packs_remove(&frame->table.list.packs, listed->f0);
			std_vec_push_Checked(&frame->settled.unlisted, &listed->f0, sizeof(uint64), _Alignof(uint64));
		}
	}
	frame->listed_live = false;
	for (size_t k = 0, k_len = frame->listed.len; k < k_len; k++)
		std_vec_free_Checked(&STD_VEC_AT(&frame->listed, chunks_TupleU64PathBuf_t, k).f1.bytes, 1, 1);
	std_vec_free_Checked(&frame->listed, sizeof(chunks_TupleU64PathBuf_t), _Alignof(chunks_TupleU64PathBuf_t));
	return true;
}

/* chunks_packs_settle_unfinished removes each unfinished pack's file in the repository's directories, which it reports. */
static bool
chunks_packs_settle_unfinished(chunks_packs_settle_Packs_settle_frame_0_t *frame)
{
	if (!chunks_packs_open_directories(HOST_IO_IO_READ_DOORS(frame->io), frame->repository, frame->own, &frame->dirs,
									   frame->error))
		return false;
	frame->dirs_live = true;
	for (size_t d = 0, d_len = frame->dirs.len; d < d_len; d++)
	{
		const StdPathBuf *dir = &STD_VEC_AT(&frame->dirs, StdPathBuf, d);

		if (!chunks_packs_settle_names_into(frame, dir))
			return false;
		for (size_t k = 0, k_len = frame->names.len; k < k_len; k++)
		{
			const StdString *name = &STD_VEC_AT(&frame->names, StdString, k);

			if (!chunks_packs_settle_ends_with(name, CHUNKS_PACK_TEMP))
				continue;
			chunks_packs_settle_joined(frame, dir, name->ptr, name->len);
			if (!chunks_file_remove_file(HOST_IO_IO_IO_DOORS(frame->io), frame->file.bytes.ptr, frame->file.bytes.len,
										 frame->error))
				return false;
			chunks_packs_settle_removed(frame);
		}
	}
	chunks_packs_settle_free_names(&frame->names, &frame->names_live);
	frame->dirs_live = false;
	std_vec_drop_Checked(&frame->dirs, sizeof(StdPathBuf), _Alignof(StdPathBuf), chunks_pack_PathBuf_drop);
	return true;
}

/* chunks_packs_settle_set_insert inserts a number into a set of numbers, a vector in order with each once. */
static void
chunks_packs_settle_set_insert(StdVec *set, size_t size, uint64 number, uint32 line, uint32 col)
{
	size_t		low = 0;
	size_t		high = set->len;

	while (low < high)
	{
		size_t		middle = low + (high - low) / 2;
		uint64		at = size == sizeof(uint32) ? STD_VEC_AT(set, uint32, middle) : STD_VEC_AT(set, uint64, middle);

		if (at == number)
			return;
		if (at < number)
			low = middle + 1;
		else
			high = middle;
	}
	if (size == sizeof(uint32))
	{
		uint32		narrow = (uint32) number;

		std_vec_insert(set, low, &narrow, size, _Alignof(uint32), RUST_FILE, line, col);
	}
	else
		std_vec_insert(set, low, &number, size, _Alignof(uint64), RUST_FILE, line, col);
}

/* chunks_packs_settle_set_contains reports whether a set of u64 numbers holds one. */
static bool
chunks_packs_settle_set_contains(const StdVec *set, uint64 number)
{
	size_t		at;

	return std_slice_binary_search(set->ptr, set->len, sizeof(uint64), &number, std_u64_Ord_cmp, &at);
}

/* chunks_packs_settle_runs_line returns a rebuilt run's line of the runs file: its file name and its pack's number, marked when the pack lost blocks. */
static void
chunks_packs_settle_runs_line(const StdString *run, uint64 number, bool lossy, StdString *result)
{
	StdStr		shown = {run->ptr, run->len};
	StdStr		mark = {CHUNKS_PACKS_LOSSY, strlen(CHUNKS_PACKS_LOSSY)};
	StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(":"), STD_FMT_ARG(1), STD_FMT_LIT(":"), STD_FMT_ARG(2)};
	StdFmtArg	args[] = {{&shown, std_str_Display_fmt}, {&number, std_u64_Display_fmt}, {&mark, std_str_Display_fmt}};
	StdFmtArguments arguments = {pieces, lossy ? 5 : 3, args};

	std_fmt_format_Checked(result, &arguments);
}

/* chunks_packs_settle_kept reports whether a run's file name is one a kept line names. */
static bool
chunks_packs_settle_kept(const StdVec *lines, const StdString *name)
{
	for (size_t k = 0, k_len = lines->len; k < k_len; k++)
	{
		const StdString *line = &STD_VEC_AT(lines, StdString, k);
		StdStr		run = chunks_packs_run_name(line->ptr, line->len);

		if (run.len == name->len && memcmp(run.ptr, name->ptr, run.len) == 0)
			return true;
	}
	return false;
}

/*
 * chunks_packs_settle_space settles the repository's directory in one
 * tablespace, when it is there: the runs file's lines whose run's file is
 * gone, or which are marked, dropped; each listed pack no line covers given
 * its run again; each run's file no line names removed; the runs file
 * written whole if it changed; and its end recorded for the table.
 */
static bool
chunks_packs_settle_space(chunks_packs_settle_Packs_settle_frame_0_t *frame, uint32 space)
{
	HostIoReadDoors read = HOST_IO_IO_READ_DOORS(frame->io);
	HostIoIoDoors io = HOST_IO_IO_IO_DOORS(frame->io);
	const StdVec *listed = &frame->table.list.packs;
	ChunksPacksTail ends;
	ChunksPacksTail *current;
	bool		there;
	bool		has;
	bool		whole_runs;

	chunks_packs_settle_space_dir(frame, space, &frame->space_dir);
	frame->space_dir_live = true;
	if (!chunks_file_dir_there(read, frame->space_dir.bytes.ptr, frame->space_dir.bytes.len, &there, frame->error))
		return false;
	if (!there)
		goto next;
	std_vec_new(&frame->packs, sizeof(chunks_TupleU64String_t), _Alignof(chunks_TupleU64String_t));
	frame->packs_live = true;
	for (size_t k = 0, k_len = listed->len; k < k_len; k++)
	{
		const chunks_TupleU64Listed_t *pack = &STD_VEC_AT(listed, chunks_TupleU64Listed_t, k);
		chunks_TupleU64String_t entry = {pack->f0};

		if (pack->f1.space != space)
			continue;
		entry.f1 = chunks_pack_PackId_file_name(&pack->f1.id, "idx", 3);
		std_vec_push_Checked(&frame->packs, &entry, sizeof(entry), _Alignof(chunks_TupleU64String_t));
	}
	std_path_join(frame->space_dir.bytes.ptr, frame->space_dir.bytes.len, CHUNKS_PACKS_RUNS,
				  strlen(CHUNKS_PACKS_RUNS), &frame->runs_path);
	frame->runs_path_live = true;
	chunks_packs_Runs_Default_default(&frame->runs.list);
	frame->runs.read.at = 0;
	frame->runs.read.past = 0;
	frame->runs_live = true;
	if (!chunks_packs_settle_got(&CHUNKS_PACKS_VIEW_RUNS_LIST_OPS, frame->io, frame->runs_path.bytes.ptr,
								 frame->runs_path.bytes.len, &frame->settled, &frame->runs.list, &frame->runs.read,
								 &whole_runs, &has, frame->error))
		return false;
	if (!has && frame->packs.len == 0 && chunks_packs_table_runs_get(&frame->table.list.runs, space) == NULL)
		goto next;
	if (!has)
		whole_runs = false;
	frame->taken = frame->runs.list.lines;
	frame->taken_at = 0;
	frame->taken_live = true;
	std_vec_new(&frame->runs.list.lines, sizeof(StdString), _Alignof(StdString));
	std_vec_new(&frame->lines, sizeof(StdString), _Alignof(StdString));
	frame->lines_live = true;
	for (size_t k_len = frame->taken.len; frame->taken_at < k_len; frame->taken_at++)
	{
		StdString  *line = &STD_VEC_AT(&frame->taken, StdString, frame->taken_at);
		bool		kept = false;

		if (!chunks_packs_lossy(line->ptr, line->len))
		{
			StdStr		run = chunks_packs_run_name(line->ptr, line->len);

			chunks_packs_settle_joined(frame, &frame->space_dir, run.ptr, run.len);
			if (!chunks_file_exists(read, frame->file.bytes.ptr, frame->file.bytes.len, &kept, frame->error))
				return false;
		}
		if (kept)
			std_vec_push_Checked(&frame->lines, line, sizeof(*line), _Alignof(StdString));
		else
		{
			std_vec_free_Checked(line, 1, 1);
			whole_runs = false;
		}
	}
	frame->taken_live = false;
	std_vec_free_Checked(&frame->taken, sizeof(StdString), _Alignof(StdString));
	std_vec_new(&frame->named, sizeof(uint64), _Alignof(uint64));
	frame->named_live = true;
	for (size_t k = 0, k_len = frame->lines.len; k < k_len; k++)
	{
		const StdString *line = &STD_VEC_AT(&frame->lines, StdString, k);
		StdVec		covered;

		chunks_packs_merge_covered(line->ptr, line->len, &covered);
		for (size_t c = 0, c_len = covered.len; c < c_len; c++)
			chunks_packs_settle_set_insert(&frame->named, sizeof(uint64), STD_VEC_AT(&covered, uint64, c), 192, 84);
		std_vec_free_Checked(&covered, sizeof(uint64), _Alignof(uint64));
	}
	for (size_t k = 0, k_len = frame->packs.len; k < k_len; k++)
	{
		const chunks_TupleU64String_t *pack = &STD_VEC_AT(&frame->packs, chunks_TupleU64String_t, k);
		chunks_TuplePathBufLost_t rebuilt;
		StdString	line;

		if (chunks_packs_settle_set_contains(&frame->named, pack->f0))
			continue;
		chunks_packs_settle_joined(frame, &frame->space_dir, pack->f1.ptr, pack->f1.len);
		std_path_with_extension(frame->file.bytes.ptr, frame->file.bytes.len, "pack", 4, &frame->pack);
		frame->pack_live = true;
		if (!chunks_packs_settle_rebuilt(frame->io, frame->pack.bytes.ptr, frame->pack.bytes.len, pack->f0,
										 frame->file.bytes.ptr, frame->file.bytes.len, &frame->lost, frame->error))
			return false;
		frame->lost_live = true;
		chunks_packs_settle_runs_line(&pack->f1, pack->f0, frame->lost.blocks.len != 0, &line);
		std_vec_push_Checked(&frame->lines, &line, sizeof(line), _Alignof(StdString));
		rebuilt.f0 = frame->pack;
		rebuilt.f1 = frame->lost;
		frame->pack_live = false;
		frame->lost_live = false;
		std_vec_push_Checked(&frame->settled.rebuilt, &rebuilt, sizeof(rebuilt), _Alignof(chunks_TuplePathBufLost_t));
		whole_runs = false;
	}
	std_vec_free_Checked(&frame->runs.list.lines, sizeof(StdString), _Alignof(StdString));
	frame->runs.list.lines = frame->lines;
	frame->lines_live = false;
	if (!chunks_packs_settle_names_into(frame, &frame->space_dir))
		return false;
	for (size_t k = 0, k_len = frame->names.len; k < k_len; k++)
	{
		const StdString *name = &STD_VEC_AT(&frame->names, StdString, k);

		if (!chunks_packs_settle_ends_with(name, ".idx") || chunks_packs_settle_kept(&frame->runs.list.lines, name))
			continue;
		chunks_packs_settle_joined(frame, &frame->space_dir, name->ptr, name->len);
		if (!chunks_file_remove_file(io, frame->file.bytes.ptr, frame->file.bytes.len, frame->error))
			return false;
		chunks_packs_settle_removed(frame);
	}
	if (!whole_runs)
	{
		bool		placing = false;

		if (!chunks_packs_lists_written(&CHUNKS_PACKS_VIEW_RUNS_LIST_OPS, io, frame->runs_path.bytes.ptr,
										frame->runs_path.bytes.len, &frame->runs.list, &frame->runs.read, &placing,
										frame->error))
			return false;
	}
	ends.end = frame->runs.read.at;
	ends.generation = frame->runs.list.generation;
	current = chunks_packs_table_runs_get(&frame->table.list.runs, space);
	if ((current == NULL || !chunks_packs_Tail_PartialEq_eq(current, &ends)) && (!whole_runs || current != NULL))
	{
		chunks_packs_table_runs_insert(&frame->table.list.runs, space, ends);
		std_vec_push_Checked(&frame->moved, &space, sizeof(space), _Alignof(uint32));
	}
next:
	chunks_packs_settle_space_pad(frame);
	return true;
}

/* chunks_packs_settle_packs_settled is PacksSettled's event, of how much the settle changed. */
static void
chunks_packs_settle_packs_settled(void *captures, ChunksTraceEvent *result)
{
	const ChunksPacksSettleSettled *settled = captures;

	result->tag = CHUNKS_TRACE_EVENT_PACKS_SETTLED;
	result->u.packs_settled.removed = settled->removed.len;
	std_vec_clone(&result->u.packs_settled.moved, &settled->moved, sizeof(uint32), _Alignof(uint32));
	result->u.packs_settled.mended = settled->fallbacks.len + settled->damaged.len;
	result->u.packs_settled.rebuilt = settled->rebuilt.len;
	std_vec_clone(&result->u.packs_settled.unlisted, &settled->unlisted, sizeof(uint64), _Alignof(uint64));
}

/*
 * chunks_packs_settle_forwards settles the forwarding file: written whole if
 * it didn't read so, and each forwards' file no line of it names removed,
 * the names its lines held before the write, which keeps them.
 */
static bool
chunks_packs_settle_forwards(chunks_packs_settle_Packs_settle_frame_0_t *frame)
{
	bool		whole;
	bool		has;

	chunks_packs_settle_free_path(&frame->path, &frame->path_live);
	std_path_join(frame->dir.bytes.ptr, frame->dir.bytes.len, CHUNKS_PACKS_FORWARDING,
				  strlen(CHUNKS_PACKS_FORWARDING), &frame->path);
	frame->path_live = true;
	chunks_packs_Runs_Default_default(&frame->runs.list);
	frame->runs.read.at = 0;
	frame->runs.read.past = 0;
	frame->runs_live = true;
	if (!chunks_packs_settle_got(&CHUNKS_PACKS_VIEW_RUNS_LIST_OPS, frame->io, frame->path.bytes.ptr,
								 frame->path.bytes.len, &frame->settled, &frame->runs.list, &frame->runs.read, &whole,
								 &has, frame->error))
		return false;
	if (!has)
	{
		*frame->error = chunks_packs_open_gone(frame->path.bytes.ptr, frame->path.bytes.len);
		return false;
	}
	if (!whole)
	{
		bool		placing = false;

		if (!chunks_packs_lists_written(&CHUNKS_PACKS_VIEW_RUNS_LIST_OPS, HOST_IO_IO_IO_DOORS(frame->io),
										frame->path.bytes.ptr, frame->path.bytes.len, &frame->runs.list,
										&frame->runs.read, &placing, frame->error))
			return false;
	}
	if (!chunks_packs_settle_names_into(frame, &frame->dir))
		return false;
	for (size_t k = 0, k_len = frame->names.len; k < k_len; k++)
	{
		const StdString *name = &STD_VEC_AT(&frame->names, StdString, k);

		if (!chunks_packs_settle_ends_with(name, ".fwd") || chunks_packs_settle_kept(&frame->runs.list.lines, name))
			continue;
		chunks_packs_settle_joined(frame, &frame->dir, name->ptr, name->len);
		if (!chunks_file_remove_file(HOST_IO_IO_IO_DOORS(frame->io), frame->file.bytes.ptr, frame->file.bytes.len,
									 frame->error))
			return false;
		chunks_packs_settle_removed(frame);
	}
	chunks_packs_settle_space_pad(frame);
	return true;
}

/* chunks_packs_settle_appended appends to the pack table's file the record of each tablespace whose runs file's end it takes. */
static bool
chunks_packs_settle_appended(chunks_packs_settle_Packs_settle_frame_0_t *frame)
{
	for (size_t k = 0, k_len = frame->moved.len; k < k_len; k++)
	{
		uint32		space = STD_VEC_AT(&frame->moved, uint32, k);
		const ChunksPacksTail *tail = chunks_packs_table_runs_get(&frame->table.list.runs, space);
		ChunksFileTornTail torn;
		ChunksFileTornTail grown;

		if (tail == NULL)
		{
			static const char missing[] = "no entry found for key";

			std_option_expect_failed(missing, sizeof(missing) - 1, STD_BTREE_DIR "map.rs", 2653, 23);
		}
		chunks_packs_Table_moved(space, *tail, &frame->record);
		frame->record_live = true;
		torn = chunks_packs_lists_end(&CHUNKS_PACKS_VIEW_TABLE_LIST_OPS, &frame->table.read, &frame->table.list,
									  frame->path.bytes.ptr, frame->path.bytes.len);
		if (!chunks_file_append(HOST_IO_IO_IO_DOORS(frame->io), &torn, frame->record.ptr, frame->record.len, &grown,
								frame->error))
			return false;
		frame->table.read.at = chunks_file_TornTail_end(&grown);
		chunks_file_TornTail_drop(&grown);
		frame->record_live = false;
		std_vec_free_Checked(&frame->record, 1, 1);
		std_vec_push_Checked(&frame->settled.moved, &space, sizeof(space), _Alignof(uint32));
	}
	return true;
}

/*
 * chunks_packs_settle_Packs_settle_body_0 is settle's body: refused with the
 * record made, a dropped repository's directories removed, and otherwise the
 * pack table read, every tablespace's directory settled, the table written
 * whole or appended to, and the forwarding file settled.
 */
static void
chunks_packs_settle_Packs_settle_body_0(void *frame_)
{
	chunks_packs_settle_Packs_settle_frame_0_t *frame = frame_;
	HostSharedHeld held;
	uint64		location;
	bool		has;
	bool		marked;
	bool		whole;
	bool		removed;

	frame->ok = false;
	if (!chunks_packs_open_found(HOST_SHARED_IO_WITHIN(frame->io), frame->repository, &has, &held, &location,
								 frame->error))
		return;
	if (has)
	{
		StdFmtPiece pieces[] = {STD_FMT_LIT("repository "), STD_FMT_ARG(0),
		STD_FMT_LIT("'s packs are settled with their record made")};
		StdFmtArg	args[] = {{&frame->repository, std_u32_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		host_shared_Held_drop(&held);
		frame->error->tag = CHUNKS_ERROR_ERROR_INVARIANT;
		std_fmt_format_Checked(&frame->error->u.invariant, &arguments);
		return;
	}
	chunks_packs_settle_Settled_Default_default(&frame->settled);
	frame->settled_live = true;
	frame->dir = host_tablespace_dir(frame->repository, frame->own);
	frame->dir_live = true;
	if (!chunks_packs_dropped_marked(HOST_IO_IO_READ_DOORS(frame->io), frame->dir.bytes.ptr, frame->dir.bytes.len,
									 &marked, frame->error))
		goto done;
	if (marked)
	{
		if (!chunks_packs_dropped_remove_directories(frame->io, frame->repository, frame->own, &removed,
													 frame->error))
			goto done;
		frame->settled.dropped = true;
		goto settled;
	}
	std_path_join(frame->dir.bytes.ptr, frame->dir.bytes.len, CHUNKS_PACKS_TABLE, strlen(CHUNKS_PACKS_TABLE),
				  &frame->path);
	frame->path_live = true;
	chunks_packs_Table_Default_default(&frame->table.list);
	frame->table.read.at = 0;
	frame->table.read.past = 0;
	frame->table_live = true;
	if (!chunks_packs_settle_got(&CHUNKS_PACKS_VIEW_TABLE_LIST_OPS, frame->io, frame->path.bytes.ptr,
								 frame->path.bytes.len, &frame->settled, &frame->table.list, &frame->table.read,
								 &whole, &has, frame->error))
		goto done;
	if (!has)
	{
		*frame->error = chunks_packs_open_gone(frame->path.bytes.ptr, frame->path.bytes.len);
		goto done;
	}
	if (!whole && !chunks_packs_settle_unlisted(frame))
		goto done;
	if (!chunks_packs_settle_unfinished(frame))
		goto done;
	std_vec_new(&frame->spaces, sizeof(uint32), _Alignof(uint32));
	frame->spaces_live = true;
	for (size_t k = 0, k_len = frame->table.list.packs.len; k < k_len; k++)
		chunks_packs_settle_set_insert(&frame->spaces, sizeof(uint32),
									   STD_VEC_AT(&frame->table.list.packs, chunks_TupleU64Listed_t, k).f1.space, 164, 106);
	for (size_t k = 0, k_len = frame->table.list.runs.len; k < k_len; k++)
		chunks_packs_settle_set_insert(&frame->spaces, sizeof(uint32),
									   STD_VEC_AT(&frame->table.list.runs, chunks_TupleU32Tail_t, k).f0, 164, 106);
	chunks_packs_settle_set_insert(&frame->spaces, sizeof(uint32), CHUNKS_PACKS_DEFAULT_SPACE, 164, 106);
	std_vec_new(&frame->moved, sizeof(uint32), _Alignof(uint32));
	frame->moved_live = true;
	for (size_t k = 0, k_len = frame->spaces.len; k < k_len; k++)
		if (!chunks_packs_settle_space(frame, STD_VEC_AT(&frame->spaces, uint32, k)))
			goto done;
	if (!whole)
	{
		bool		placing = false;

		if (!chunks_packs_lists_written(&CHUNKS_PACKS_VIEW_TABLE_LIST_OPS, HOST_IO_IO_IO_DOORS(frame->io),
										frame->path.bytes.ptr, frame->path.bytes.len, &frame->table.list,
										&frame->table.read, &placing, frame->error))
			goto done;
	}
	else if (!chunks_packs_settle_appended(frame))
		goto done;
	if (!chunks_packs_settle_forwards(frame))
		goto done;
	chunks_trace_emit(frame->repository, chunks_packs_settle_packs_settled, &frame->settled, NULL);
settled:
	*frame->result = frame->settled;
	frame->settled_live = false;
	frame->ok = true;
done:
	chunks_packs_settle_Packs_settle_pad_0(frame);
}

/*
 * chunks_packs_settle_Packs_settle mends what a crash or damage left in the
 * directories and list files of the repository with an id, whose own
 * directory is in the tablespace with an id, and returns what it changed. A
 * list file that reads only as its previous generation, or only up to
 * damage, it writes whole again from what read; a pack table read so leaves
 * out the packs whose files are gone. In the repository's directory in every
 * tablespace that is there it removes each unfinished pack's file, drops
 * from the runs file each run whose file is gone, writes again from its pack
 * the run of each listed pack no run covers, and removes each run's file the
 * runs file doesn't name. A run written from a pack whose address table
 * didn't read whole is marked in its line, and a marked run counts as
 * covering nothing. A runs file whose whole write the pack table's file
 * doesn't name it takes as it is, appending the table's record of its end.
 * It refuses a repository whose record is published, and one whose pack
 * table's file or forwarding file is gone with its previous generation.
 */
bool
chunks_packs_settle_Packs_settle(HostIoIo *io, uint32 repository, uint32 own, ChunksPacksSettleSettled *result,
								 ChunksErrorError *error)
{
	chunks_packs_settle_Packs_settle_frame_0_t frame = {.io = io, .repository = repository, .own = own,
	.result = result, .error = error};

	std_landing(&frame, chunks_packs_settle_Packs_settle_body_0, chunks_packs_settle_Packs_settle_pad_0);
	return frame.ok;
}

#ifdef DOLTGRES_TEST_CHUNKS

/* chunks_packs_settle_tests_restarted frees a repository's record, as a restart makes shared memory anew, taking its packs. */
static void
chunks_packs_settle_tests_restarted(HostIoIo *io, ChunksPacksBackendPacks *packs)
{
	uint64		location = chunks_packs_backend_Packs_location(packs);

	chunks_packs_backend_Packs_drop(packs);
	chunks_packs_tests_freed(RUST_FILE, io, location, 267, 75, 84);
}

/* chunks_packs_settle_tests_opened opens the packs of the repository with an id. */
static bool
chunks_packs_settle_tests_opened(HostIoIo *io, uint32 id, ChunksPacksBackendPacks *result, ChunksErrorError *error)
{
	if (id == 0)
		STD_OPTION_UNWRAP_FAILED(272, 45);
	return chunks_packs_open_Packs_open(io, id, HOST_PG_DEFAULT, result, error);
}

/* chunks_packs_settle_tests_opened_unwrapped opens the packs of the repository with an id, which unwraps at a line and column. */
static void
chunks_packs_settle_tests_opened_unwrapped(HostIoIo *io, uint32 id, ChunksPacksBackendPacks *result, uint32 line,
										   uint32 col)
{
	ChunksErrorError error;

	if (!chunks_packs_settle_tests_opened(io, id, result, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, line, col);
}

/* chunks_packs_settle_tests_Names_Debug_fmt is a Vec<String>'s Debug. */
static bool
chunks_packs_settle_tests_Names_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *names = value;
	StdDebugList l = std_fmt_debug_list(f);

	for (size_t k = 0, k_len = names->len; k < k_len; k++)
		std_DebugList_entry(&l, &STD_VEC_AT(names, StdString, k), std_String_Debug_fmt);
	return std_DebugList_finish(&l);
}

/* chunks_packs_settle_tests_string_lt reports whether one string sorts before another, as str's Ord does. */
static bool
chunks_packs_settle_tests_string_lt(const StdString *a, const StdString *b)
{
	int			order = memcmp(a->ptr, b->ptr, Min(a->len, b->len));

	return order < 0 || (order == 0 && a->len < b->len);
}

/* chunks_packs_settle_tests_names returns the names of a directory's files that end with some text, in order. */
static StdVec
chunks_packs_settle_tests_names(const char *dir, size_t dir_len, const char *ending)
{
	StdVec		files;
	StdVec		names;
	StdIoError	io_error;
	bool		entry;

	if (!std_fs_read_dir_names(dir, dir_len, &files, &entry, &io_error))
	{
		if (entry)
			STD_RESULT_EXPECT_FAILED(347, 61, CHUNKS_PACKS_FIXTURES_UNWRAP, &io_error, std_io_Error_Debug_fmt);
		STD_RESULT_EXPECT_FAILED(347, 36, CHUNKS_PACKS_FIXTURES_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	}
	std_vec_new(&names, sizeof(StdString), _Alignof(StdString));
	for (size_t k = 0, k_len = files.len; k < k_len; k++)
	{
		StdString  *name = &STD_VEC_AT(&files, StdString, k);
		size_t		at = names.len;

		if (name->len < strlen(ending) ||
			memcmp((const char *) name->ptr + name->len - strlen(ending), ending, strlen(ending)) != 0)
		{
			std_vec_free_Checked(name, 1, 1);
			continue;
		}
		while (at > 0 && chunks_packs_settle_tests_string_lt(name, &STD_VEC_AT(&names, StdString, at - 1)))
			at--;
		std_vec_insert(&names, at, name, sizeof(StdString), _Alignof(StdString), RUST_FILE, 349, 9);
	}
	std_vec_free_Checked(&files, sizeof(StdString), _Alignof(StdString));
	return names;
}

/* chunks_packs_settle_tests_settled settles the repository with an id in the default tablespace, which unwraps at a line and column. */
static ChunksPacksSettleSettled
chunks_packs_settle_tests_settled(HostIoIo *io, uint32 id, uint32 line, uint32 col)
{
	ChunksPacksSettleSettled settled;
	ChunksErrorError error;

	if (!chunks_packs_settle_Packs_settle(io, id, HOST_PG_DEFAULT, &settled, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, line, col);
	return settled;
}

/* chunks_packs_settle_tests_settled_eq asserts at a line that a settle's changes, which it frees, are those given, which it frees too. */
static void
chunks_packs_settle_tests_settled_eq(ChunksPacksSettleSettled *got, ChunksPacksSettleSettled *want, uint32 line,
									 uint32 col)
{
	if (!chunks_packs_settle_Settled_PartialEq_eq(got, want))
		std_assert_failed(RUST_FILE, line, col, "==", got, chunks_packs_settle_Settled_Debug_fmt, want,
						  chunks_packs_settle_Settled_Debug_fmt, NULL);
	chunks_packs_settle_Settled_drop(got);
	chunks_packs_settle_Settled_drop(want);
}

/* chunks_packs_settle_tests_unchanged asserts at a line that a settle of the repository with an id changes nothing. */
static void
chunks_packs_settle_tests_unchanged(HostIoIo *io, uint32 id, uint32 line, uint32 unwrap_col)
{
	ChunksPacksSettleSettled got = chunks_packs_settle_tests_settled(io, id, line, unwrap_col);
	ChunksPacksSettleSettled want;

	chunks_packs_settle_Settled_Default_default(&want);
	chunks_packs_settle_tests_settled_eq(&got, &want, line, 9);
}

/* chunks_packs_settle_tests_written writes a file's bytes, which unwraps at a line and column. */
static void
chunks_packs_settle_tests_written(const StdPathBuf *path, const uint8 *bytes, size_t bytes_len, uint32 line,
								  uint32 col)
{
	StdIoError	io_error;

	if (!std_fs_write(path->bytes.ptr, path->bytes.len, bytes, bytes_len, &io_error))
		STD_RESULT_EXPECT_FAILED(line, col, CHUNKS_PACKS_FIXTURES_UNWRAP, &io_error, std_io_Error_Debug_fmt);
}

/* chunks_packs_settle_tests_read reads a file's bytes, which unwraps at a line and column. */
static StdVec
chunks_packs_settle_tests_read(const StdPathBuf *path, uint32 line, uint32 col)
{
	StdVec		bytes;
	StdIoError	io_error;

	if (!std_fs_read(path->bytes.ptr, path->bytes.len, &bytes, &io_error))
		STD_RESULT_EXPECT_FAILED(line, col, CHUNKS_PACKS_FIXTURES_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	return bytes;
}

/* chunks_packs_settle_tests_removed_one returns the changes of a settle that removed one file and changed nothing else. */
static ChunksPacksSettleSettled
chunks_packs_settle_tests_removed_one(const StdPathBuf *file)
{
	ChunksPacksSettleSettled want;
	StdPathBuf	removed;

	chunks_packs_settle_Settled_Default_default(&want);
	std_PathBuf_Clone_clone(file, &removed);
	std_vec_push_Checked(&want.removed, &removed, sizeof(removed), _Alignof(StdPathBuf));
	return want;
}

/*
 * test_chunks_packs_settle_tests_a_settle_removes_the_unfinished_packs_a_crash_left:
 * a settle removes the file of a pack a crash left unfinished and reports
 * it, leaves a pack no table lists as it is, changes nothing in a repository
 * a crash left nothing in, and is refused once the repository's record is
 * made.
 */
void
test_chunks_packs_settle_tests_a_settle_removes_the_unfinished_packs_a_crash_left(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedReader reading = chunks_packs_tests_reading(RUST_FILE, io, 282, 46);
	ChunksPacksBackendPacks packs;
	ChunksPacksBackendPacks after;
	chunks_TupleAddressLocator_t one;
	ChunksPacksSettleSettled settled;
	ChunksPacksSettleSettled want;
	ChunksErrorError error;
	StdPathBuf	dir = host_tablespace_dir(91, HOST_PG_DEFAULT);
	StdPathBuf	left;
	StdPathBuf	unlisted;
	StdVec		bytes;
	bool		refused;
	static const char kept[] = "a pack no table lists";

	chunks_packs_fixtures_created(io, 91, &packs);
	one = chunks_packs_fixtures_added(io, &packs, (const uint8 *) "one", 3);
	refused = !chunks_packs_settle_Packs_settle(io, 91, HOST_PG_DEFAULT, &settled, &error);
	if (refused)
	{
		refused = error.tag == CHUNKS_ERROR_ERROR_INVARIANT;
		chunks_error_Error_drop(&error);
	}
	else
		chunks_packs_settle_Settled_drop(&settled);
	STD_ASSERT(286, 9, refused, "matches!(Packs::settle(io, id, host::PG_DEFAULT), Err(Error::Invariant(_)))");
	chunks_packs_settle_tests_restarted(io, &packs);
	chunks_packs_settle_tests_unchanged(io, 91, 288, 60);

	std_path_join(dir.bytes.ptr, dir.bytes.len, "0123456789abcdef0123456789abcdef.pack.tmp",
				  strlen("0123456789abcdef0123456789abcdef.pack.tmp"), &left);
	chunks_packs_settle_tests_written(&left, (const uint8 *) "unfinished", strlen("unfinished"), 291, 46);
	std_path_with_file_name(left.bytes.ptr, left.bytes.len, "fedcba9876543210fedcba9876543210.pack",
							strlen("fedcba9876543210fedcba9876543210.pack"), &unlisted);
	chunks_packs_settle_tests_written(&unlisted, (const uint8 *) kept, strlen(kept), 293, 61);
	settled = chunks_packs_settle_tests_settled(io, 91, 294, 63);
	want = chunks_packs_settle_tests_removed_one(&left);
	chunks_packs_settle_tests_settled_eq(&settled, &want, 295, 9);
	STD_ASSERT(296, 9, !std_path_Path_exists(left.bytes.ptr, left.bytes.len), "!left.exists()");
	bytes = chunks_packs_settle_tests_read(&unlisted, 297, 45);
	if (bytes.len != strlen(kept) || memcmp(bytes.ptr, kept, bytes.len) != 0)
	{
		StdBytes	got = {bytes.ptr, bytes.len};
		StdBytes	wanted = {(const uint8 *) kept, strlen(kept)};

		std_assert_failed(RUST_FILE, 297, 9, "==", &got, std_bytes_Debug_fmt, &wanted, std_bytes_Debug_fmt, NULL);
	}
	std_vec_free_Checked(&bytes, 1, 1);
	chunks_packs_settle_tests_opened_unwrapped(io, 91, &after, 298, 35);
	chunks_packs_tests_found(RUST_FILE, &after, io, &reading, &one.f0, &one.f1, 298, 9, 69);
	chunks_packs_backend_Packs_drop(&after);
	std_vec_free_Checked(&unlisted.bytes, 1, 1);
	std_vec_free_Checked(&left.bytes, 1, 1);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	host_shared_Reader_drop(&reading);
	simc_TestIo_drop(&test_io);
}

/* chunks_packs_settle_tests_moved_default returns the changes of a settle that recorded only the default tablespace's runs file's end. */
static ChunksPacksSettleSettled
chunks_packs_settle_tests_moved_default(void)
{
	ChunksPacksSettleSettled want;
	uint32		space = CHUNKS_PACKS_DEFAULT_SPACE;

	chunks_packs_settle_Settled_Default_default(&want);
	std_vec_push_Checked(&want.moved, &space, sizeof(space), _Alignof(uint32));
	return want;
}

/*
 * test_chunks_packs_settle_tests_a_settle_records_a_runs_files_whole_write_the_table_does_not_name:
 * a crash between a runs file's whole write and the pack table's record of
 * its new end leaves a repository no backend reads: its view raises on the
 * runs file. A settle records the runs file's end in the table, after which
 * the repository's chunks are found where they were.
 */
void
test_chunks_packs_settle_tests_a_settle_records_a_runs_files_whole_write_the_table_does_not_name(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedReader reading = chunks_packs_tests_reading(RUST_FILE, io, 308, 46);
	ChunksPacksBackendPacks packs;
	ChunksPacksBackendPacks unsettled;
	ChunksPacksBackendPacks after;
	chunks_TupleAddressLocator_t placed[2];
	StdPathBuf	dir = host_tablespace_dir(92, HOST_PG_DEFAULT);
	StdPathBuf	path;
	ChunksPacksViewReadRuns runs;
	StdVec		whole;
	ChunksFileTornTail torn;
	ChunksPacksSettleSettled settled;
	ChunksPacksSettleSettled want;
	ChunksErrorError error;
	ChunksTrailerLocator found;
	bool		placing = false;
	bool		has;
	bool		failed;

	chunks_packs_fixtures_created(io, 92, &packs);
	placed[0] = chunks_packs_fixtures_added(io, &packs, (const uint8 *) "one", 3);
	placed[1] = chunks_packs_fixtures_added(io, &packs, (const uint8 *) "two", 3);
	chunks_packs_settle_tests_restarted(io, &packs);
	std_path_join(dir.bytes.ptr, dir.bytes.len, CHUNKS_PACKS_RUNS, strlen(CHUNKS_PACKS_RUNS), &path);
	chunks_packs_Runs_Default_default(&runs.list);
	if (!chunks_packs_open_read_whole(&CHUNKS_PACKS_VIEW_RUNS_LIST_OPS, HOST_IO_IO_READ_DOORS(io), path.bytes.ptr,
									  path.bytes.len, &runs.list, &runs.read, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 314, 58);
	runs.list.generation ^= 1;
	chunks_packs_Runs_whole(&runs.list, &whole);
	if (!chunks_file_write_appended(HOST_IO_IO_IO_DOORS(io), path.bytes.ptr, path.bytes.len, whole.ptr, whole.len,
									&placing, &torn, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 316, 67);
	chunks_file_TornTail_drop(&torn);
	std_vec_free_Checked(&whole, 1, 1);

	chunks_packs_settle_tests_opened_unwrapped(io, 92, &unsettled, 318, 40);
	failed = !chunks_packs_backend_Packs_find(&unsettled, io, &reading, &placed[0].f0, &has, &found, &error);
	if (failed)
		chunks_error_Error_drop(&error);
	STD_ASSERT_MSG(319, 9, failed, "a view read a runs file its table doesn't name");
	chunks_packs_settle_tests_restarted(io, &unsettled);
	settled = chunks_packs_settle_tests_settled(io, 92, 321, 89);
	want = chunks_packs_settle_tests_moved_default();
	chunks_packs_settle_tests_settled_eq(&settled, &want, 322, 9);
	chunks_packs_settle_tests_opened_unwrapped(io, 92, &after, 323, 36);
	for (size_t k = 0; k < lengthof(placed); k++)
		chunks_packs_tests_found(RUST_FILE, &after, io, &reading, &placed[k].f0, &placed[k].f1, 325, 13, 59);
	chunks_packs_settle_tests_restarted(io, &after);
	chunks_packs_settle_tests_unchanged(io, 92, 328, 86);
	chunks_packs_Runs_drop(&runs.list);
	std_vec_free_Checked(&path.bytes, 1, 1);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	host_shared_Reader_drop(&reading);
	simc_TestIo_drop(&test_io);
}

/* chunks_packs_settle_tests_shown panics at a line and column with a settle's changes as the message. */
static pg_noreturn void
chunks_packs_settle_tests_shown(const ChunksPacksSettleSettled *settled, uint32 line, uint32 col)
{
	StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
	StdFmtArg	args[] = {{settled, chunks_packs_settle_Settled_Debug_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	std_panic_fmt(RUST_FILE, line, col, &arguments);
}

/* chunks_packs_settle_tests_renamed renames a file, which unwraps at a line and column 43, as each rename the tests make does. */
static void
chunks_packs_settle_tests_renamed(const StdPathBuf *from, const StdPathBuf *to, uint32 line)
{
	StdIoError	io_error;

	if (!std_fs_rename(from->bytes.ptr, from->bytes.len, to->bytes.ptr, to->bytes.len, &io_error))
		STD_RESULT_EXPECT_FAILED(line, 43, CHUNKS_PACKS_FIXTURES_UNWRAP, &io_error, std_io_Error_Debug_fmt);
}

/* chunks_packs_settle_tests_unit_Debug_fmt is ()'s Debug. */
static bool
chunks_packs_settle_tests_unit_Debug_fmt(const void *value, StdFormatter *f)
{
	(void) value;
	return std_fmt_write_str(f, "()", 2);
}

/* chunks_packs_settle_tests_refused_as asserts at a line that an answer failed as a corrupt error with a wording, with the answer as the message. */
static void
chunks_packs_settle_tests_refused_as(bool ok, const void *value, StdFmtFn fmt, const ChunksErrorError *error,
									 const StdString *wording, uint32 line)
{
	if (ok || error->tag != CHUNKS_ERROR_ERROR_CORRUPT || error->u.corrupt.len != wording->len ||
		memcmp(error->u.corrupt.ptr, wording->ptr, wording->len) != 0)
	{
		ChunksPacksTestsResult answer = {ok, ok ? STD_ITEM(value, fmt) : STD_ITEM(error, chunks_error_Error_Debug_fmt)};
		StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&answer, chunks_packs_tests_Result_Debug_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_panic_fmt(RUST_FILE, line, 13, &arguments);
	}
}

/*
 * test_chunks_packs_settle_tests_a_settle_keeps_a_damaged_list_files_records_before_the_damage:
 * a list file damaged past its first record keeps its records before the
 * damage: a settle keeps the damaged file beside it, writes the file whole
 * again from those records, and reports where the damage is with the whole
 * records after it. A pack table or a forwarding file gone with its previous
 * generation is refused, naming the file, and a later open of the repository
 * raises the same refusal.
 */
void
test_chunks_packs_settle_tests_a_settle_keeps_a_damaged_list_files_records_before_the_damage(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedReader reading = chunks_packs_tests_reading(RUST_FILE, io, 388, 46);
	ChunksPacksBackendPacks packs;
	ChunksPacksBackendPacks after;
	chunks_TupleAddressLocator_t placed[3];
	StdPathBuf	dir = host_tablespace_dir(94, HOST_PG_DEFAULT);
	StdPathBuf	path;
	StdFsMetadata metadata;
	StdIoError	io_error;
	StdVec		bytes;
	size_t		one;
	size_t		add;
	size_t		middle;
	ChunksPacksSettleSettled settled;
	ChunksErrorError error;
	StdStr		parent;
	StdVec		names;
	static const uint64 first[] = {1};
	static const char *const files[] = {CHUNKS_PACKS_FORWARDING, CHUNKS_PACKS_TABLE};
	bool		failed;

	chunks_packs_fixtures_created(io, 94, &packs);
	std_path_join(dir.bytes.ptr, dir.bytes.len, CHUNKS_PACKS_TABLE, strlen(CHUNKS_PACKS_TABLE), &path);
	placed[0] = chunks_packs_fixtures_added(io, &packs, (const uint8 *) "one", 3);
	if (!std_fs_metadata(path.bytes.ptr, path.bytes.len, &metadata, &io_error))
		STD_RESULT_EXPECT_FAILED(392, 44, CHUNKS_PACKS_FIXTURES_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	one = (size_t) std_fs_Metadata_len(&metadata);
	std_fs_Metadata_drop(&metadata);
	placed[1] = chunks_packs_fixtures_added(io, &packs, (const uint8 *) "two", 3);
	placed[2] = chunks_packs_fixtures_added(io, &packs, (const uint8 *) "six", 3);
	chunks_packs_settle_tests_restarted(io, &packs);
	bytes = chunks_packs_settle_tests_read(&path, 395, 46);
	add = STD_SUB(usize, bytes.len, one, 396, 20) / 2;
	middle = STD_SUB(usize, STD_SUB(usize, bytes.len, add, 397, 22), add / 2, 397, 22);
	std_index_check(middle, bytes.len, RUST_FILE, 398, 9);
	((uint8 *) bytes.ptr)[middle] ^= 0xff;
	chunks_packs_settle_tests_written(&path, bytes.ptr, bytes.len, 399, 39);
	std_vec_free_Checked(&bytes, 1, 1);
	failed = !chunks_packs_settle_tests_opened(io, 94, &after, &error);
	if (failed)
		chunks_error_Error_drop(&error);
	else
		chunks_packs_backend_Packs_drop(&after);
	STD_ASSERT(400, 9, failed, "opened(io, 94).is_err()");

	settled = chunks_packs_settle_tests_settled(io, 94, 402, 89);
	if (settled.damaged.len != 1)
		chunks_packs_settle_tests_shown(&settled, 403, 69);
	{
		const chunks_TuplePathBufDamage_t *damaged = &STD_VEC_AT(&settled.damaged, chunks_TuplePathBufDamage_t, 0);

		if (!std_path_PartialEq_eq(damaged->f0.bytes.ptr, damaged->f0.bytes.len, path.bytes.ptr, path.bytes.len) ||
			damaged->f1.offset == 0 || damaged->f1.after.len != 1)
			chunks_packs_settle_tests_shown(&settled, 404, 9);
	}
	chunks_packs_settle_Settled_drop(&settled);
	if (!std_path_parent(path.bytes.ptr, path.bytes.len, &parent))
		STD_OPTION_UNWRAP_FAILED(405, 40);
	names = chunks_packs_settle_tests_names(parent.ptr, parent.len, ".damaged");
	if (names.len != 1 || STD_VEC_AT(&names, StdString, 0).len != strlen("table.damaged") ||
		memcmp(STD_VEC_AT(&names, StdString, 0).ptr, "table.damaged", strlen("table.damaged")) != 0)
	{
		StdVec		want;
		StdString	name;

		std_vec_new(&want, sizeof(StdString), _Alignof(StdString));
		std_string_from_Checked(&name, "table.damaged", strlen("table.damaged"));
		std_vec_push_Checked(&want, &name, sizeof(name), _Alignof(StdString));
		std_assert_failed(RUST_FILE, 405, 9, "==", &names, chunks_packs_settle_tests_Names_Debug_fmt, &want,
						  chunks_packs_settle_tests_Names_Debug_fmt, NULL);
	}
	std_vec_drop_Checked(&names, sizeof(StdString), _Alignof(StdString), std_String_drop);
	chunks_packs_settle_tests_opened_unwrapped(io, 94, &after, 406, 36);
	chunks_packs_tests_numbers_eq(RUST_FILE, &after, io, &reading, first, lengthof(first), NULL, 407, 9, 48);
	chunks_packs_tests_found(RUST_FILE, &after, io, &reading, &placed[0].f0, &placed[0].f1, 408, 9, 59);
	chunks_packs_settle_tests_restarted(io, &after);

	for (size_t k = 0; k < lengthof(files); k++)
	{
		StdPathBuf	file;
		StdPathBuf	kept;
		StdString	wording;
		StdStr		shown;
		ChunksPacksSettleSettled gone;
		ChunksPacksBackendPacks later;
		bool		ok;
		uint8		unit = 0;
		StdFmtPiece pieces[] = {STD_FMT_LIT("required file \""), STD_FMT_ARG(0), STD_FMT_LIT("\" does not exist")};
		StdFmtArg	args[] = {{&shown, std_path_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_path_with_file_name(path.bytes.ptr, path.bytes.len, files[k], strlen(files[k]), &file);
		std_path_with_file_name(path.bytes.ptr, path.bytes.len, "kept", 4, &kept);
		chunks_packs_settle_tests_renamed(&file, &kept, 413);
		ok = chunks_packs_settle_Packs_settle(io, 94, HOST_PG_DEFAULT, &gone, &error);
		shown.ptr = file.bytes.ptr;
		shown.len = file.bytes.len;
		std_fmt_format_Checked(&wording, &arguments);
		chunks_packs_settle_tests_refused_as(ok, &gone, chunks_packs_settle_Settled_Debug_fmt, &error, &wording, 416);
		chunks_error_Error_drop(&error);
		ok = chunks_packs_settle_tests_opened(io, 94, &later, &error);
		if (ok)
			chunks_packs_backend_Packs_drop(&later);
		chunks_packs_settle_tests_refused_as(ok, &unit, chunks_packs_settle_tests_unit_Debug_fmt, &error, &wording,
											 418);
		chunks_error_Error_drop(&error);
		chunks_packs_settle_tests_renamed(&kept, &file, 419);
		std_vec_free_Checked(&wording, 1, 1);
		std_vec_free_Checked(&kept.bytes, 1, 1);
		std_vec_free_Checked(&file.bytes, 1, 1);
	}
	std_vec_free_Checked(&path.bytes, 1, 1);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	host_shared_Reader_drop(&reading);
	simc_TestIo_drop(&test_io);
}

/* chunks_packs_settle_tests_marked returns how many lines of a runs file mark their run as written with loss. */
static size_t
chunks_packs_settle_tests_marked(HostIoIo *io, const StdPathBuf *runs)
{
	ChunksPacksViewReadRuns read;
	ChunksErrorError error;
	size_t		marked = 0;

	chunks_packs_Runs_Default_default(&read.list);
	if (!chunks_packs_open_read_whole(&CHUNKS_PACKS_VIEW_RUNS_LIST_OPS, HOST_IO_IO_READ_DOORS(io), runs->bytes.ptr,
									  runs->bytes.len, &read.list, &read.read, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 480, 55);
	for (size_t k = 0, k_len = read.list.lines.len; k < k_len; k++)
	{
		const StdString *line = &STD_VEC_AT(&read.list.lines, StdString, k);

		marked += chunks_packs_lossy(line->ptr, line->len) ? 1 : 0;
	}
	chunks_packs_Runs_drop(&read.list);
	return marked;
}

/* chunks_packs_settle_tests_marked_eq asserts at a line and column that a runs file marks as many lines as given. */
static void
chunks_packs_settle_tests_marked_eq(HostIoIo *io, const StdPathBuf *runs, size_t want, uint32 line, uint32 col)
{
	size_t		marked = chunks_packs_settle_tests_marked(io, runs);

	if (marked != want)
		std_assert_failed(RUST_FILE, line, col, "==", &marked, std_usize_Debug_fmt, &want, std_usize_Debug_fmt, NULL);
}

/*
 * chunks_packs_settle_tests_finds asserts at a line and column that a
 * repository's packs find the second address where given, and the first
 * there or nowhere, whose finds unwrap at another line and its columns.
 */
static void
chunks_packs_settle_tests_finds(HostIoIo *io, const HostSharedReader *reading, const ChunksPacksBackendPacks *packs,
								const chunks_TupleAddressLocator_t *one, const chunks_TupleAddressLocator_t *two,
								bool one_found, uint32 line, uint32 col, uint32 unwrap_line,
								const uint32 *unwrap_cols)
{
	const chunks_TupleAddressLocator_t *placed[] = {one, two};
	ChunksTrailerLocator found[2];
	bool		has[2];
	bool		same = true;
	StdOptionRef got[2];
	StdOptionRef want[2];
	StdTupleRef got_pair;
	StdTupleRef want_pair;

	for (size_t k = 0; k < 2; k++)
	{
		ChunksErrorError error;

		if (!chunks_packs_backend_Packs_find(packs, io, reading, &placed[k]->f0, &has[k], &found[k], &error))
			chunks_packs_tests_unwrapped(RUST_FILE, &error, unwrap_line, unwrap_cols[k]);
		got[k] = (StdOptionRef)
		{
			has[k], STD_ITEM(&found[k], chunks_trailer_Locator_Debug_fmt)
		};
		want[k] = (StdOptionRef)
		{
			k == 1 || one_found, STD_ITEM(&placed[k]->f1, chunks_trailer_Locator_Debug_fmt)
		};
		same = same && has[k] == want[k].has &&
			(!has[k] || chunks_trailer_Locator_PartialEq_eq(&found[k], &placed[k]->f1));
	}
	if (same)
		return;
	got_pair = STD_TUPLE(2, STD_ITEM(&got[0], std_option_Debug_fmt), STD_ITEM(&got[1], std_option_Debug_fmt));
	want_pair = STD_TUPLE(2, STD_ITEM(&want[0], std_option_Debug_fmt), STD_ITEM(&want[1], std_option_Debug_fmt));
	std_assert_failed(RUST_FILE, line, col, "==", &got_pair, std_tuple_Debug_fmt, &want_pair, std_tuple_Debug_fmt,
					  NULL);
}

/* chunks_packs_settle_tests_losses_eq reports whether two Vec<(PathBuf, Lost)> are equal. */
static bool
chunks_packs_settle_tests_losses_eq(const StdVec *a, const StdVec *b)
{
	if (a->len != b->len)
		return false;
	for (size_t k = 0, k_len = a->len; k < k_len; k++)
	{
		const chunks_TuplePathBufLost_t *x = &STD_VEC_AT(a, chunks_TuplePathBufLost_t, k);
		const chunks_TuplePathBufLost_t *y = &STD_VEC_AT(b, chunks_TuplePathBufLost_t, k);

		if (!std_path_PartialEq_eq(x->f0.bytes.ptr, x->f0.bytes.len, y->f0.bytes.ptr, y->f0.bytes.len) ||
			!chunks_pack_Lost_PartialEq_eq(&x->f1, &y->f1))
			return false;
	}
	return true;
}

/*
 * test_chunks_packs_settle_tests_a_settle_marks_a_run_rebuilt_with_loss_and_rebuilds_it_until_its_pack_is_whole:
 * a run written again from a pack whose address table has a block that
 * fails its checksum is marked in its line: another process's view reads it,
 * without the chunks that block named; each later settle writes it again and
 * reports the same loss while the pack is damaged; and once the pack's bytes
 * are back a settle writes the whole run, unmarked, and the chunk the lost
 * block held is found.
 */
void
test_chunks_packs_settle_tests_a_settle_marks_a_run_rebuilt_with_loss_and_rebuilds_it_until_its_pack_is_whole(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedReader reading = chunks_packs_tests_reading(RUST_FILE, io, 466, 46);
	ChunksPacksBackendPacks packs;
	ChunksPacksBackendPacks after;
	chunks_TupleAddressLocator_t one;
	chunks_TupleAddressLocator_t two;
	StdArcInner *arc;
	StdPathBuf	pack;
	StdPathBuf	runs;
	StdPathBuf	idx;
	StdVec		whole;
	StdVec		damaged;
	StdVec		losses[2];
	StdIoError	io_error;
	ChunksErrorError error;
	ChunksPacksSettleSettled settled;
	size_t		footer;
	size_t		table;
	bool		has;
	static const uint32 first_cols[] = {49, 90};
	static const uint32 last_cols[] = {45, 86};

	chunks_packs_fixtures_created(io, 96, &packs);
	one = chunks_packs_fixtures_added(io, &packs, (const uint8 *) "one", 3);
	two = chunks_packs_fixtures_added(io, &packs, (const uint8 *) "two", 3);
	if (!chunks_packs_backend_Packs_pack_reader(&packs, io, &reading, 1, &has, &arc, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 469, 55);
	if (!has)
		STD_OPTION_UNWRAP_FAILED(469, 64);
	{
		const ChunksPackPackReader *reader = std_arc_Deref_deref(arc, _Alignof(ChunksPackPackReader));
		size_t		path_len;
		const char *path = chunks_pack_PackReader_path(reader, &path_len);

		std_path_to_path_buf(path, path_len, &pack);
	}
	chunks_pack_ArcPackReader_drop(arc);
	chunks_packs_settle_tests_restarted(io, &packs);
	whole = chunks_packs_settle_tests_read(&pack, 471, 51);
	std_path_with_file_name(pack.bytes.ptr, pack.bytes.len, CHUNKS_PACKS_RUNS, strlen(CHUNKS_PACKS_RUNS), &runs);
	footer = STD_SUB(usize, whole.len, 56, 472, 22);
	std_range_check(STD_ADD(usize, footer, 24, 473, 46), STD_ADD(usize, footer, 32, 473, 59), whole.len, RUST_FILE,
					473, 40);
	table = (size_t) std_u64_from_le_bytes((const uint8 *) whole.ptr + footer + 24);
	std_vec_clone(&damaged, &whole, 1, 1);
	std_index_check(STD_ADD(usize, table, 9, 475, 17), damaged.len, RUST_FILE, 475, 9);
	((uint8 *) damaged.ptr)[table + 9] ^= 0xff;
	chunks_packs_settle_tests_written(&pack, damaged.ptr, damaged.len, 476, 41);
	std_vec_free_Checked(&damaged, 1, 1);
	std_path_with_extension(pack.bytes.ptr, pack.bytes.len, "idx", 3, &idx);
	if (!std_fs_remove_file(idx.bytes.ptr, idx.bytes.len, &io_error))
		STD_RESULT_EXPECT_FAILED(477, 58, CHUNKS_PACKS_FIXTURES_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	std_vec_free_Checked(&idx.bytes, 1, 1);

	for (size_t k = 0; k < 2; k++)
	{
		const chunks_TuplePathBufLost_t *rebuilt;

		settled = chunks_packs_settle_tests_settled(io, 96, 486, 67);
		if (settled.rebuilt.len != 1)
			chunks_packs_settle_tests_shown(&settled, 487, 71);
		rebuilt = &STD_VEC_AT(&settled.rebuilt, chunks_TuplePathBufLost_t, 0);
		if (!std_path_PartialEq_eq(rebuilt->f0.bytes.ptr, rebuilt->f0.bytes.len, pack.bytes.ptr, pack.bytes.len) ||
			rebuilt->f1.blocks.len != 1)
			chunks_packs_settle_tests_shown(&settled, 488, 13);
		losses[k] = settled.rebuilt;
		std_vec_new(&settled.rebuilt, sizeof(chunks_TuplePathBufLost_t), _Alignof(chunks_TuplePathBufLost_t));
		chunks_packs_settle_Settled_drop(&settled);
		chunks_packs_settle_tests_marked_eq(io, &runs, 1, 490, 13);
		chunks_packs_settle_tests_opened_unwrapped(io, 96, &after, 491, 40);
		chunks_packs_settle_tests_finds(io, &reading, &after, &one, &two, false, 492, 13, 493, first_cols);
		chunks_packs_settle_tests_restarted(io, &after);
	}
	if (!chunks_packs_settle_tests_losses_eq(&losses[0], &losses[1]))
		std_assert_failed(RUST_FILE, 498, 9, "==", &losses[0], chunks_packs_settle_Losses_Debug_fmt, &losses[1],
						  chunks_packs_settle_Losses_Debug_fmt, NULL);

	chunks_packs_settle_tests_written(&pack, whole.ptr, whole.len, 500, 39);
	settled = chunks_packs_settle_tests_settled(io, 96, 501, 63);
	{
		StdVec		want;
		chunks_TuplePathBufLost_t entry;

		std_PathBuf_Clone_clone(&pack, &entry.f0);
		entry.f1 = chunks_pack_Lost_Default_default();
		std_vec_new(&want, sizeof(entry), _Alignof(chunks_TuplePathBufLost_t));
		std_vec_push_Checked(&want, &entry, sizeof(entry), _Alignof(chunks_TuplePathBufLost_t));
		if (!chunks_packs_settle_tests_losses_eq(&settled.rebuilt, &want))
			std_assert_failed(RUST_FILE, 502, 9, "==", &settled.rebuilt, chunks_packs_settle_Losses_Debug_fmt, &want,
							  chunks_packs_settle_Losses_Debug_fmt, NULL);
		std_vec_free_Checked(&entry.f0.bytes, 1, 1);
		chunks_pack_Lost_drop(&STD_VEC_AT(&want, chunks_TuplePathBufLost_t, 0).f1);
		std_vec_free_Checked(&want, sizeof(entry), _Alignof(chunks_TuplePathBufLost_t));
	}
	chunks_packs_settle_Settled_drop(&settled);
	chunks_packs_settle_tests_marked_eq(io, &runs, 0, 503, 9);
	chunks_packs_settle_tests_opened_unwrapped(io, 96, &after, 504, 36);
	chunks_packs_settle_tests_finds(io, &reading, &after, &one, &two, true, 505, 9, 506, last_cols);
	chunks_packs_settle_tests_restarted(io, &after);
	chunks_packs_settle_tests_unchanged(io, 96, 510, 60);
	for (size_t k = 0; k < 2; k++)
	{
		for (size_t l = 0, l_len = losses[k].len; l < l_len; l++)
		{
			chunks_TuplePathBufLost_t *loss = &STD_VEC_AT(&losses[k], chunks_TuplePathBufLost_t, l);

			std_vec_free_Checked(&loss->f0.bytes, 1, 1);
			chunks_pack_Lost_drop(&loss->f1);
		}
		std_vec_free_Checked(&losses[k], sizeof(chunks_TuplePathBufLost_t), _Alignof(chunks_TuplePathBufLost_t));
	}
	std_vec_free_Checked(&whole, 1, 1);
	std_vec_free_Checked(&runs.bytes, 1, 1);
	std_vec_free_Checked(&pack.bytes, 1, 1);
	host_shared_Reader_drop(&reading);
	simc_TestIo_drop(&test_io);
}

/*
 * test_chunks_packs_settle_tests_a_settle_removes_an_unfinished_pack_alone_in_its_tablespace:
 * a settle removes an unfinished pack that is the only file of its
 * tablespace, which no list names.
 */
void
test_chunks_packs_settle_tests_a_settle_removes_an_unfinished_pack_alone_in_its_tablespace(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	ChunksPacksBackendPacks packs;
	StdPathBuf	dir = host_tablespace_dir(97, 16400);
	StdPathBuf	left;
	StdString	name;
	StdIoError	io_error;
	ChunksPacksSettleSettled settled;
	ChunksPacksSettleSettled want;

	chunks_packs_fixtures_created(io, 97, &packs);
	chunks_packs_settle_tests_restarted(io, &packs);
	if (!std_fs_create_dir_all(dir.bytes.ptr, dir.bytes.len, &io_error))
		STD_RESULT_EXPECT_FAILED(521, 39, CHUNKS_PACKS_FIXTURES_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	std_string_from_Checked(&name, "0123456789abcdef0123456789abcdef.7.1", strlen("0123456789abcdef0123456789abcdef.7.1"));
	std_string_push_str_Checked(&name, CHUNKS_PACK_TEMP, strlen(CHUNKS_PACK_TEMP));
	std_path_join(dir.bytes.ptr, dir.bytes.len, name.ptr, name.len, &left);
	chunks_packs_settle_tests_written(&left, (const uint8 *) "unfinished", strlen("unfinished"), 523, 46);
	settled = chunks_packs_settle_tests_settled(io, 97, 524, 89);
	want = chunks_packs_settle_tests_removed_one(&left);
	chunks_packs_settle_tests_settled_eq(&settled, &want, 525, 9);
	STD_ASSERT(526, 9, !std_path_Path_exists(left.bytes.ptr, left.bytes.len), "!left.exists()");
	std_vec_free_Checked(&left.bytes, 1, 1);
	std_vec_free_Checked(&name, 1, 1);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	simc_TestIo_drop(&test_io);
}

/* chunks_packs_settle_tests_listing makes a repository with an id that lists three packs, a forward, and list files written whole, frees its record, and returns its chunks' places and its directory. */
static StdPathBuf
chunks_packs_settle_tests_listing(HostIoIo *io, uint32 id, chunks_TupleAddressLocator_t *placed)
{
	HostSharedReader reading = chunks_packs_tests_reading(RUST_FILE, io, 334, 46);
	ChunksPacksBackendPacks packs;
	chunks_packs_backend_Packs_forward_moves_t moves[1];
	ChunksPacksRetireForwarded forwarded;
	ChunksErrorError error;
	static const char *const bodies[] = {"one", "two", "six"};

	chunks_packs_fixtures_created(io, id, &packs);
	for (size_t k = 0; k < lengthof(bodies); k++)
		placed[k] = chunks_packs_fixtures_added(io, &packs, (const uint8 *) bodies[k], 3);
	moves[0].f0 = placed[0].f1;
	moves[0].f1 = placed[1].f1;
	if (!chunks_packs_backend_Packs_forward(&packs, io, &reading, moves, lengthof(moves), &forwarded, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 338, 73);
	chunks_packs_retire_Forwarded_drop(&forwarded);
	if (!chunks_packs_backend_Packs_compact_lists(&packs, io, &reading, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 339, 43);
	chunks_packs_settle_tests_restarted(io, &packs);
	host_shared_Reader_drop(&reading);
	return host_tablespace_dir(id, HOST_PG_DEFAULT);
}

/* chunks_packs_settle_tests_removed_file removes a file in a directory, which unwraps at a line and column. */
static void
chunks_packs_settle_tests_removed_file(const StdPathBuf *dir, const char *name, size_t name_len, uint32 line,
									   uint32 col)
{
	StdPathBuf	path;
	StdIoError	io_error;

	std_path_join(dir->bytes.ptr, dir->bytes.len, name, name_len, &path);
	if (!std_fs_remove_file(path.bytes.ptr, path.bytes.len, &io_error))
		STD_RESULT_EXPECT_FAILED(line, col, CHUNKS_PACKS_FIXTURES_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	std_vec_free_Checked(&path.bytes, 1, 1);
}

/* chunks_packs_settle_tests_Names_drop drops a Vec<String>. */
static void
chunks_packs_settle_tests_Names_drop(StdVec *names)
{
	std_vec_drop_Checked(names, sizeof(StdString), _Alignof(StdString), std_String_drop);
}

/* chunks_packs_settle_tests_found_eq asserts at a line that a find, which unwraps at a column, answers the place given, or none when it is NULL, with a message, or none when it is NULL. */
static void
chunks_packs_settle_tests_found_eq(const ChunksPacksBackendPacks *packs, HostIoIo *io,
								   const HostSharedReader *reading, const ChunksAddressAddress *address,
								   const ChunksTrailerLocator *want, const StdFmtArguments *message, uint32 line,
								   uint32 assert_col, uint32 unwrap_col)
{
	ChunksTrailerLocator found;
	ChunksErrorError error;
	bool		has;

	if (!chunks_packs_backend_Packs_find(packs, io, reading, address, &has, &found, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, line, unwrap_col);
	if (has != (want != NULL) || (has && !chunks_trailer_Locator_PartialEq_eq(&found, want)))
	{
		StdOptionRef got = {has, STD_ITEM(&found, chunks_trailer_Locator_Debug_fmt)};
		StdOptionRef wanted = {want != NULL, STD_ITEM(want, chunks_trailer_Locator_Debug_fmt)};

		std_assert_failed(RUST_FILE, line, assert_col, "==", &got, std_option_Debug_fmt, &wanted,
						  std_option_Debug_fmt, message);
	}
}

/*
 * test_chunks_packs_settle_tests_a_settle_writes_a_pack_table_that_fell_back_whole_again:
 * a pack table or a forwarding file whose newest generation is gone is read
 * as its previous one, which a backend's open refuses: a settle writes each
 * whole again from that, leaving out a pack the table lists whose file is
 * gone, and reports them.
 */
void
test_chunks_packs_settle_tests_a_settle_writes_a_pack_table_that_fell_back_whole_again(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedReader reading = chunks_packs_tests_reading(RUST_FILE, io, 360, 46);
	chunks_TupleAddressLocator_t placed[3];
	StdPathBuf	dir = chunks_packs_settle_tests_listing(io, 93, placed);
	ChunksPacksBackendPacks after;
	ChunksPacksSettleSettled settled;
	ChunksErrorError error;
	StdVec		packs;
	StdString	first;
	StdVec		listed;
	chunks_TuplePathBufFallback_t fallbacks[2] = {{.f1 = {CHUNKS_FILE_FALLBACK_MISSING}},
	{.f1 = {CHUNKS_FILE_FALLBACK_MISSING}}};
	StdVec		want;
	bool		refused;
	bool		same;
	size_t		len;
	size_t		one = 1;
	size_t		two = 2;

	chunks_packs_settle_tests_removed_file(&dir, CHUNKS_PACKS_TABLE, strlen(CHUNKS_PACKS_TABLE), 362, 47);
	chunks_packs_settle_tests_removed_file(&dir, CHUNKS_PACKS_FORWARDING, strlen(CHUNKS_PACKS_FORWARDING), 363, 52);
	refused = !chunks_packs_settle_tests_opened(io, 93, &after, &error);
	if (refused)
	{
		refused = error.tag == CHUNKS_ERROR_ERROR_CORRUPT;
		chunks_error_Error_drop(&error);
	}
	else
		chunks_packs_backend_Packs_drop(&after);
	STD_ASSERT_MSG(364, 9, refused, "a backend opened a table that fell back");
	packs = chunks_packs_settle_tests_names(dir.bytes.ptr, dir.bytes.len, ".pack");
	std_vec_remove(&packs, 0, &first, sizeof(StdString), _Alignof(StdString), RUST_FILE, 365, 60);
	chunks_packs_settle_tests_removed_file(&dir, first.ptr, first.len, 365, 72);
	std_String_drop(&first);
	chunks_packs_settle_tests_Names_drop(&packs);
	settled = chunks_packs_settle_tests_settled(io, 93, 366, 89);
	std_path_join(dir.bytes.ptr, dir.bytes.len, CHUNKS_PACKS_TABLE, strlen(CHUNKS_PACKS_TABLE), &fallbacks[0].f0);
	std_path_join(dir.bytes.ptr, dir.bytes.len, CHUNKS_PACKS_FORWARDING, strlen(CHUNKS_PACKS_FORWARDING),
				  &fallbacks[1].f0);
	same = settled.fallbacks.len == lengthof(fallbacks);
	for (size_t k = 0; same && k < lengthof(fallbacks); k++)
	{
		const chunks_TuplePathBufFallback_t *got = &STD_VEC_AT(&settled.fallbacks, chunks_TuplePathBufFallback_t, k);

		same = chunks_packs_settle_path_eq(&got->f0, &fallbacks[k].f0) &&
			chunks_file_Fallback_PartialEq_eq(&got->f1, &fallbacks[k].f1);
	}
	want.ptr = fallbacks;
	want.len = lengthof(fallbacks);
	want.cap = lengthof(fallbacks);
	STD_ASSERT_EQ(367, 9, same, &settled.fallbacks, chunks_packs_settle_Fallbacks_Debug_fmt, &want,
				  chunks_packs_settle_Fallbacks_Debug_fmt);
	for (size_t k = 0; k < lengthof(fallbacks); k++)
		std_vec_free_Checked(&fallbacks[k].f0.bytes, 1, 1);
	len = settled.unlisted.len;
	STD_ASSERT_EQ(368, 9, len == one, &len, std_usize_Debug_fmt, &one, std_usize_Debug_fmt);
	chunks_packs_settle_tests_opened_unwrapped(io, 93, &after, 369, 36);
	if (!chunks_packs_backend_Packs_numbers(&after, io, &reading, &listed, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 370, 50);
	len = listed.len;
	STD_ASSERT_EQ(371, 9, len == two, &len, std_usize_Debug_fmt, &two, std_usize_Debug_fmt);
	for (size_t k = 0; k < lengthof(placed); k++)
	{
		uint64		number = k + 1;
		bool		contains = false;
		StdFmtPiece pieces[] = {STD_FMT_LIT("pack "), STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&number, std_u64_Display_fmt}};
		StdFmtArguments message = {pieces, lengthof(pieces), args};

		for (size_t n = 0; n < listed.len; n++)
			contains = contains || STD_VEC_AT(&listed, uint64, n) == number;
		chunks_packs_settle_tests_found_eq(&after, io, &reading, &placed[k].f0, contains ? &placed[k].f1 : NULL,
										   &message, 374, 13, 60);
	}
	std_vec_free_Checked(&listed, sizeof(uint64), _Alignof(uint64));
	chunks_packs_settle_tests_restarted(io, &after);
	chunks_packs_settle_tests_unchanged(io, 93, 377, 86);
	chunks_packs_settle_Settled_drop(&settled);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	host_shared_Reader_drop(&reading);
	simc_TestIo_drop(&test_io);
}

/*
 * test_chunks_packs_settle_tests_a_settle_rebuilds_the_runs_a_listed_pack_lacks_and_removes_the_strays:
 * a settle writes again from its pack the run of a listed pack no run
 * covers, whether the runs file names a run whose file is gone or is gone
 * itself, and removes a run's file no runs file names.
 */
void
test_chunks_packs_settle_tests_a_settle_rebuilds_the_runs_a_listed_pack_lacks_and_removes_the_strays(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedReader reading = chunks_packs_tests_reading(RUST_FILE, io, 429, 46);
	chunks_TupleAddressLocator_t placed[3];
	StdPathBuf	dir = chunks_packs_settle_tests_listing(io, 95, placed);
	StdVec		runs = chunks_packs_settle_tests_names(dir.bytes.ptr, dir.bytes.len, ".idx");
	const StdString *run;
	StdPathBuf	stray;
	StdPathBuf	path;
	StdPathBuf	pack;
	StdVec		names;
	StdVec		strays;
	ChunksPacksSettleSettled settled;
	ChunksPacksSettleSettled again;
	ChunksPacksBackendPacks after;
	ChunksPackLost none = chunks_pack_Lost_Default_default();
	const chunks_TuplePathBufLost_t *rebuilt;
	uint32		spaces[] = {CHUNKS_PACKS_DEFAULT_SPACE};
	StdVec		want_spaces = {spaces, 1, 1};
	size_t		len;
	bool		same;
	const char *stray_name = "0123456789abcdef0123456789abcdef.idx";

	std_index_check(0, runs.len, RUST_FILE, 432, 40);
	run = &STD_VEC_AT(&runs, StdString, 0);
	chunks_packs_settle_tests_removed_file(&dir, run->ptr, run->len, 432, 50);
	std_path_join(dir.bytes.ptr, dir.bytes.len, stray_name, strlen(stray_name), &stray);
	chunks_packs_settle_tests_written(&stray, (const uint8 *) "stray", 5, 434, 42);
	settled = chunks_packs_settle_tests_settled(io, 95, 435, 89);
	std_vec_new(&strays, sizeof(StdPathBuf), _Alignof(StdPathBuf));
	std_vec_push_Checked(&strays, &stray, sizeof(StdPathBuf), _Alignof(StdPathBuf));
	same = settled.removed.len == 1 &&
		chunks_packs_settle_path_eq(&STD_VEC_AT(&settled.removed, StdPathBuf, 0), &stray);
	STD_ASSERT_EQ(436, 9, same, &settled.removed, chunks_packs_settle_Paths_Debug_fmt, &strays,
				  chunks_packs_settle_Paths_Debug_fmt);
	if (settled.rebuilt.len != 1)
	{
		StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&settled, chunks_packs_settle_Settled_Debug_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_panic_fmt(RUST_FILE, 437, 64, &arguments);
	}
	rebuilt = &STD_VEC_AT(&settled.rebuilt, chunks_TuplePathBufLost_t, 0);
	std_index_check(0, runs.len, RUST_FILE, 438, 36);
	std_path_join(dir.bytes.ptr, dir.bytes.len, run->ptr, run->len, &path);
	std_path_with_extension(path.bytes.ptr, path.bytes.len, "pack", 4, &pack);
	STD_ASSERT(438, 9, chunks_packs_settle_path_eq(&rebuilt->f0, &pack) && chunks_pack_Lost_PartialEq_eq(&rebuilt->f1, &none),
			   "pack == &dir.join(&runs[0]).with_extension(\"pack\") && lost == &Lost::default()");
	std_vec_free_Checked(&pack.bytes, 1, 1);
	std_vec_free_Checked(&path.bytes, 1, 1);
	names = chunks_packs_settle_tests_names(dir.bytes.ptr, dir.bytes.len, ".idx");
	same = settled.moved.len == 1 && STD_VEC_AT(&settled.moved, uint32, 0) == CHUNKS_PACKS_DEFAULT_SPACE &&
		names.len == runs.len;
	for (size_t k = 0; same && k < names.len; k++)
	{
		const StdString *a = &STD_VEC_AT(&names, StdString, k);
		const StdString *b = &STD_VEC_AT(&runs, StdString, k);

		same = a->len == b->len && memcmp(a->ptr, b->ptr, a->len) == 0;
	}
	if (!same)
	{
		StdTupleRef left = STD_TUPLE(2, STD_ITEM(&settled.moved, chunks_packs_settle_U32s_Debug_fmt),
									 STD_ITEM(&names, chunks_packs_settle_tests_Names_Debug_fmt));
		StdTupleRef right = STD_TUPLE(2, STD_ITEM(&want_spaces, chunks_packs_settle_U32s_Debug_fmt),
									  STD_ITEM(&runs, chunks_packs_settle_tests_Names_Debug_fmt));

		std_assert_failed(RUST_FILE, 439, 9, "==", &left, std_tuple_Debug_fmt, &right, std_tuple_Debug_fmt, NULL);
	}
	chunks_packs_settle_tests_Names_drop(&names);
	chunks_packs_settle_tests_opened_unwrapped(io, 95, &after, 440, 36);
	for (size_t k = 0; k < lengthof(placed); k++)
		chunks_packs_settle_tests_found_eq(&after, io, &reading, &placed[k].f0, &placed[k].f1, NULL, 442, 13, 58);
	chunks_packs_settle_tests_restarted(io, &after);

	names = chunks_packs_settle_tests_names(dir.bytes.ptr, dir.bytes.len, "runs");
	for (size_t k = 0; k < names.len; k++)
	{
		const StdString *name = &STD_VEC_AT(&names, StdString, k);

		chunks_packs_settle_tests_removed_file(&dir, name->ptr, name->len, 447, 50);
	}
	chunks_packs_settle_tests_Names_drop(&names);
	std_index_check(1, runs.len, RUST_FILE, 449, 40);
	run = &STD_VEC_AT(&runs, StdString, 1);
	chunks_packs_settle_tests_removed_file(&dir, run->ptr, run->len, 449, 50);
	again = chunks_packs_settle_tests_settled(io, 95, 450, 89);
	len = again.rebuilt.len;
	if (len != 1 || again.moved.len != 1 || STD_VEC_AT(&again.moved, uint32, 0) != CHUNKS_PACKS_DEFAULT_SPACE)
	{
		size_t		one = 1;
		StdTupleRef left = STD_TUPLE(2, STD_ITEM(&len, std_usize_Debug_fmt),
									 STD_ITEM(&again.moved, chunks_packs_settle_U32s_Debug_fmt));
		StdTupleRef right = STD_TUPLE(2, STD_ITEM(&one, std_usize_Debug_fmt),
									  STD_ITEM(&want_spaces, chunks_packs_settle_U32s_Debug_fmt));

		std_assert_failed(RUST_FILE, 451, 9, "==", &left, std_tuple_Debug_fmt, &right, std_tuple_Debug_fmt, NULL);
	}
	chunks_packs_settle_tests_opened_unwrapped(io, 95, &after, 452, 36);
	for (size_t k = 0; k < lengthof(placed); k++)
		chunks_packs_settle_tests_found_eq(&after, io, &reading, &placed[k].f0, &placed[k].f1, NULL, 454, 13, 58);
	chunks_packs_backend_Packs_drop(&after);
	chunks_packs_settle_Settled_drop(&again);
	chunks_pack_Lost_drop(&none);
	chunks_packs_settle_Settled_drop(&settled);
	std_vec_drop_Checked(&strays, sizeof(StdPathBuf), _Alignof(StdPathBuf), chunks_pack_PathBuf_drop);
	chunks_packs_settle_tests_Names_drop(&runs);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	host_shared_Reader_drop(&reading);
	simc_TestIo_drop(&test_io);
}

/*
 * test_chunks_packs_settle_tests_a_settle_removes_a_forwards_file_the_forwarding_file_doesnt_name:
 * a settle removes a forwards' file the forwarding file doesn't name, as a
 * forward that failed between its file's write and its line leaves one, and
 * keeps the file a line names.
 */
void
test_chunks_packs_settle_tests_a_settle_removes_a_forwards_file_the_forwarding_file_doesnt_name(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedReader reading = chunks_packs_tests_reading(RUST_FILE, io, 535, 46);
	ChunksPacksBackendPacks packs;
	chunks_TupleAddressLocator_t first;
	chunks_TupleAddressLocator_t copy;
	chunks_packs_backend_Packs_forward_moves_t moves[1];
	ChunksPacksRetireForwarded forwarded;
	ChunksErrorError error;
	StdPathBuf	dir = host_tablespace_dir(98, HOST_PG_DEFAULT);
	StdPathBuf	stray;
	ChunksPacksSettleSettled settled;
	ChunksPacksSettleSettled want;
	StdVec		names;
	size_t		len;
	size_t		one = 1;
	const char *stray_name = "0123456789abcdef0123456789abcdef.fwd";
	const char *body = "a forwards' file no line names";

	chunks_packs_fixtures_created(io, 98, &packs);
	first = chunks_packs_fixtures_added(io, &packs, (const uint8 *) "one", 3);
	copy = chunks_packs_fixtures_added(io, &packs, (const uint8 *) "one", 3);
	moves[0].f0 = first.f1;
	moves[0].f1 = copy.f1;
	if (!chunks_packs_backend_Packs_forward(&packs, io, &reading, moves, lengthof(moves), &forwarded, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 538, 66);
	chunks_packs_retire_Forwarded_drop(&forwarded);
	std_path_join(dir.bytes.ptr, dir.bytes.len, stray_name, strlen(stray_name), &stray);
	chunks_packs_settle_tests_written(&stray, (const uint8 *) body, strlen(body), 541, 67);
	chunks_packs_settle_tests_restarted(io, &packs);
	settled = chunks_packs_settle_tests_settled(io, 98, 543, 89);
	want = chunks_packs_settle_tests_removed_one(&stray);
	chunks_packs_settle_tests_settled_eq(&settled, &want, 544, 9);
	STD_ASSERT(545, 9, !std_path_Path_exists(stray.bytes.ptr, stray.bytes.len), "!stray.exists()");
	names = chunks_packs_settle_tests_names(dir.bytes.ptr, dir.bytes.len, ".fwd");
	len = names.len;
	chunks_packs_settle_tests_Names_drop(&names);
	STD_ASSERT_EQ(546, 9, len == one, &len, std_usize_Debug_fmt, &one, std_usize_Debug_fmt);
	std_vec_free_Checked(&stray.bytes, 1, 1);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	host_shared_Reader_drop(&reading);
	simc_TestIo_drop(&test_io);
}

#endif							/* DOLTGRES_TEST_CHUNKS */
