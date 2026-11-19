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
 * Packs, the immutable files every chunk is stored in: a header, the chunks'
 * records, a table of the zstd dictionaries its leaves are compressed with,
 * its address table, and a footer.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "blake3.h"
#include "doltgres/chunks/header.h"
#include "doltgres/chunks/pack.h"
#include "doltgres/chunks/trace.h"
#include "doltgres/chunks/varint.h"
#include "doltgres/host/lib.h"
#include "doltgres/host/local.h"
#include "doltgres/host/lock.h"
#include "doltgres/std/atomic.h"
#include "doltgres/std/cmp.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/hash.h"
#include "doltgres/std/int128.h"
#include "doltgres/std/num.h"
#include "doltgres/std/ptr.h"
#include "doltgres/std/slice.h"
#include "doltgres/std/sort.h"
#include "doltgres/std/str.h"
#include "doltgres/std/time.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"

#define XXH_STATIC_LINKING_ONLY
#include "xxhash.h"

#ifdef DOLTGRES_TEST_CHUNKS
#include "doltgres/simc/simc.h"
#include "doltgres/std/io.h"
#include "doltgres/std/test.h"
#include "doltgres/std/test_fs.h"
#endif

/* CHUNKS_PACK_RAW_LAYOUT is a ChunksPackRaw's opaque storage read as Raw's layout, which only pack.c knows. */
#define CHUNKS_PACK_RAW_LAYOUT(raw) ((chunks_pack_RawLayout *) (raw))

#define RUST_FILE "chunks" STD_SEP "src" STD_SEP "pack.rs"

/* CHUNKS_PACK_MAGIC starts and ends every pack. */
static const uint8 CHUNKS_PACK_MAGIC[8] = {0x9f, 0x3a, 0xd1, 0x5c, 0x07, 0xe2, 0x6b, 0x49};

/*
 * CHUNKS_PACK_FOOTER is the size of a pack's footer: the dictionary table's
 * offset and length, the chunk count, the header's length, the address
 * table's offset, length, and block count, the checksum of the header and
 * footer, and the magic.
 */
#define CHUNKS_PACK_FOOTER ((size_t) 56)

/* CHUNKS_PACK_HEADER is the length of a header: the magic, the pack id, and the compression descriptor. */
#define CHUNKS_PACK_HEADER ((size_t) 25)

/* CHUNKS_PACK_ZSTD is the pack header's compression descriptor for zstd. */
#define CHUNKS_PACK_ZSTD ((uint8) 1)

/* CHUNKS_PACK_RAW is the flag of a chunk record stored uncompressed. */
#define CHUNKS_PACK_RAW ((uint8) 0)

/* CHUNKS_PACK_COMPRESSED is the flag of a chunk record compressed with zstd. */
#define CHUNKS_PACK_COMPRESSED ((uint8) 1)

/* CHUNKS_PACK_WITH_DICTIONARY is the flag of a chunk record compressed with zstd and one of the pack's dictionaries, whose slot leads its data. */
#define CHUNKS_PACK_WITH_DICTIONARY ((uint8) 2)

/* CHUNKS_PACK_LEVEL is the zstd level chunks are compressed at. */
#define CHUNKS_PACK_LEVEL (-1)

/* CHUNKS_PACK_MAX_STORED is the largest stored length a locator can hold. */
#define CHUNKS_PACK_MAX_STORED (((size_t) 1 << 24) - 1)

/* CHUNKS_PACK_MAX_OFFSET is the largest offset a locator can hold. */
#define CHUNKS_PACK_MAX_OFFSET ((UINT64CONST(1) << 40) - 1)

/* CHUNKS_PACK_FLUSH is how much a pack writer buffers before writing. */
#define CHUNKS_PACK_FLUSH ((size_t) 1 << 20)

/*
 * CHUNKS_PACK_TRAIN_LEAVES is how many of its first leaves a pack that
 * trains keeps as the samples it trains its dictionary on once it holds
 * CHUNKS_PACK_TRAIN_AFTER bytes.
 */
#define CHUNKS_PACK_TRAIN_LEAVES ((size_t) 64)

/* CHUNKS_PACK_TRAIN_AFTER is how many bytes a pack holds before it trains its dictionary. */
#define CHUNKS_PACK_TRAIN_AFTER (UINT64CONST(16) << 20)

/* chunks_pack_PackId_Debug_fmt is PackId's derived Debug: its bytes as a list inside "PackId(...)". */
bool
chunks_pack_PackId_Debug_fmt(const void *value, StdFormatter *f)
{
	const ChunksPackPackId *self = value;
	StdBytes	bytes = {self->f0, 16};
	StdDebugTuple builder = std_fmt_debug_tuple(f, "PackId", 6);

	std_DebugTuple_field(&builder, &bytes, std_bytes_Debug_fmt);
	return std_DebugTuple_finish(&builder);
}

/* chunks_pack_PackId_PartialEq_eq is PackId's derived PartialEq, its bytes in order. */
bool
chunks_pack_PackId_PartialEq_eq(const ChunksPackPackId *self, const ChunksPackPackId *other)
{
	return memcmp(self->f0, other->f0, 16) == 0;
}

/* chunks_pack_PackId_Ord_cmp is PackId's derived Ord, its bytes in order. */
StdOrdering
chunks_pack_PackId_Ord_cmp(const ChunksPackPackId *self, const ChunksPackPackId *other)
{
	return std_bytes_Ord_cmp(self->f0, 16, other->f0, 16);
}

/* chunks_pack_PackId_new returns an id that no other pack has, from the time, the process, and a counter. */
ChunksPackPackId
chunks_pack_PackId_new(void)
{
	blake3_hasher hasher;
	StdSystemTime now;
	StdDuration nanos;
	StdDuration before;
	uint8		nanos_bytes[16];
	uint8		process_bytes[4];
	uint8		unique_bytes[8];
	uint8		hash[32];
	ChunksPackPackId id;

	blake3_hasher_init(&hasher);
	now = host_now();
	if (!std_SystemTime_duration_since(&now, STD_UNIX_EPOCH, &nanos, &before))
		nanos = std_Duration_Default_default();
	std_u128_to_le_bytes(std_Duration_as_nanos(&nanos), nanos_bytes);
	blake3_hasher_update(&hasher, nanos_bytes, 16);
	std_u32_to_le_bytes(host_process_id(), process_bytes);
	blake3_hasher_update(&hasher, process_bytes, 4);
	std_u64_to_le_bytes(host_unique(), unique_bytes);
	blake3_hasher_update(&hasher, unique_bytes, 8);
	blake3_hasher_finalize(&hasher, hash, 32);
	std_range_to_check(16, 32, RUST_FILE, 107, 44);
	if (16 != 16)
		std_result_unwrap_failed("called `Result::unwrap()` on an `Err` value",
								 sizeof("called `Result::unwrap()` on an `Err` value") - 1, STD_DANGLING,
								 std_TryFromSliceError_Debug_fmt, RUST_FILE, 107, 62);
	memcpy(id.f0, hash, 16);
	return id;
}

/* chunks_pack_PackId_file_name_closure_0 is file_name's digits: a byte as two hex digits. */
static StdString
chunks_pack_PackId_file_name_closure_0(const uint8 *b)
{
	StdString	digits;
	StdFmtPiece pieces[] = {{NULL, 0, 0, {.sign_aware_zero_pad = true, .fill = ' ', .align = STD_FMT_ALIGN_UNKNOWN,
	.has_width = true, .width = 2}}};
	StdFmtArg	args[] = {{b, std_u8_LowerHex_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	std_fmt_format_Checked(&digits, &arguments);
	return digits;
}

/*
 * chunks_pack_PackId_file_name_frame_0_t is file_name's state across its
 * pad: its arguments, its result, and the hex and digits it holds.
 */
typedef struct chunks_pack_PackId_file_name_frame_0_t
{
	const ChunksPackPackId *self;
	const char *extension;
	size_t		extension_len;
	StdString	result;
	StdString	hex;
	bool		hex_live;
	StdString	digits;
	bool		digits_live;
} chunks_pack_PackId_file_name_frame_0_t;

/* chunks_pack_PackId_file_name_body_0 is file_name's body. */
static void
chunks_pack_PackId_file_name_body_0(void *frame_)
{
	chunks_pack_PackId_file_name_frame_0_t *frame = frame_;
	StdStr		extension = {frame->extension, frame->extension_len};
	StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT("."), STD_FMT_ARG(1)};
	StdFmtArg	args[] = {{&frame->hex, std_String_Display_fmt}, {&extension, std_str_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};
	const uint8 *rest = STD_PTR_AT(frame->self->f0, 1, 1);

	if (!(16 > 0))
	{
		frame->hex = (StdString) STD_VEC_NEW;
		frame->hex_live = true;
	}
	else
	{
		frame->hex = chunks_pack_PackId_file_name_closure_0(frame->self->f0);
		frame->hex_live = true;
		for (size_t i = 0, i_len = 15; i < i_len; i++)
		{
			frame->digits = chunks_pack_PackId_file_name_closure_0(STD_PTR_AT(rest, i, 1));
			frame->digits_live = true;
			std_string_push_str_Checked(&frame->hex, frame->digits.ptr, frame->digits.len);
			frame->digits_live = false;
			std_vec_free_Checked(&frame->digits, 1, 1);
		}
	}
	std_fmt_format_Checked(&frame->result, &arguments);
	frame->hex_live = false;
	std_vec_free_Checked(&frame->hex, 1, 1);
}

/* chunks_pack_PackId_file_name_pad_0 is file_name's pad: the digits and the hex, while it holds them. */
static void
chunks_pack_PackId_file_name_pad_0(void *frame_)
{
	chunks_pack_PackId_file_name_frame_0_t *frame = frame_;

	if (frame->digits_live)
		std_vec_free_Checked(&frame->digits, 1, 1);
	if (frame->hex_live)
		std_vec_free_Checked(&frame->hex, 1, 1);
}

/* chunks_pack_PackId_file_name returns the pack's file name, with the extension. */
StdString
chunks_pack_PackId_file_name(const ChunksPackPackId *self, const char *extension, size_t extension_len)
{
	chunks_pack_PackId_file_name_frame_0_t frame = {.self = self, .extension = extension,
	.extension_len = extension_len};

	std_landing(&frame, chunks_pack_PackId_file_name_body_0, chunks_pack_PackId_file_name_pad_0);
	return frame.result;
}

/* chunks_pack_PackId_of_name returns the id a file name of a pack's begins with, or false for a name that begins with none. */
bool
chunks_pack_PackId_of_name(const char *name, size_t name_len, ChunksPackPackId *result)
{
	ChunksPackPackId id = {0};
	StdChunks	pairs;

	if (!(32 <= name_len))
		return false;
	std_slice_chunks_check(2, RUST_FILE, 136, 53);
	pairs = std_slice_chunks(name, 32, 2);
	for (size_t i = 0, i_len = 16; i < i_len; i++)
	{
		const void *digits;
		size_t		digits_len;
		StdUtf8Error utf8_error;
		StdParseIntError parse_error;

		(void) std_slice_Chunks_next(&pairs, 1, 1, &digits, &digits_len);
		if (!std_str_from_utf8(digits, digits_len, &utf8_error))
			return false;
		if (!std_u8_from_str_radix(digits, digits_len, 16, STD_PTR_AT(id.f0, i, 1), &parse_error))
			return false;
	}
	*result = id;
	return true;
}

/* chunks_pack_PackId_Hash_hash is PackId's derived Hash: its 16 bytes as a slice, their length first. */
void
chunks_pack_PackId_Hash_hash(const void *self_, StdHasher state)
{
	const ChunksPackPackId *self = self_;

	std_Hasher_write_length_prefix(state, sizeof(self->f0));
	state.ops->write(state.self, self->f0, sizeof(self->f0));
}

/* chunks_pack_PackId_PartialOrd_partial_cmp is PackId's derived PartialOrd, its Ord's order, which orders every pair. */
bool
chunks_pack_PackId_PartialOrd_partial_cmp(const ChunksPackPackId *self, const ChunksPackPackId *other,
										  StdOrdering *result)
{
	*result = chunks_pack_PackId_Ord_cmp(self, other);
	return true;
}

/* chunks_pack_PackId_Default_default is PackId's Default, a new id. */
ChunksPackPackId
chunks_pack_PackId_Default_default(void)
{
	return chunks_pack_PackId_new();
}

/*
 * chunks_pack_temp_of returns the numbers an unfinished pack's file name
 * carries after the pack's id, the process's that writes it and the pack's
 * own, or nothing for a name of another form.
 */
bool
chunks_pack_temp_of(const char *name, size_t name_len, uint32 *result_0, uint64 *result_1)
{
	size_t		temp_len = strlen(CHUNKS_PACK_TEMP);
	size_t		stem_len;
	size_t		dots[2];
	size_t		count = 0;
	StdParseIntError error;

	if (name_len < temp_len || memcmp(name + std_wrapping_sub_usize(name_len, temp_len), CHUNKS_PACK_TEMP,
									  temp_len) != 0)
		return false;
	stem_len = std_wrapping_sub_usize(name_len, temp_len);
	for (size_t k = 0, k_len = stem_len; k < k_len; k++)
		if (name[k] == '.')
		{
			if (count == 2)
				return false;
			dots[count] = k;
			count = std_wrapping_add_usize(count, 1);
		}
	if (count != 2)
		return false;
	return std_str_parse_u32(name + std_wrapping_add_usize(dots[0], 1),
							 std_wrapping_sub_usize(std_wrapping_sub_usize(dots[1], dots[0]), 1), result_0, &error) &&
		std_str_parse_u64(name + std_wrapping_add_usize(dots[1], 1),
						  std_wrapping_sub_usize(std_wrapping_sub_usize(stem_len, dots[1]), 1), result_1, &error);
}

/*
 * chunks_pack_train_dictionary returns a zstd dictionary trained on the
 * samples, or nothing when they are too few to train on.
 */
static bool
chunks_pack_train_dictionary(const StdBytes *samples, size_t samples_len, StdVec *result)
{
	size_t		total = 0;

	for (size_t k = 0, k_len = samples_len; k < k_len; k++)
		total = STD_ADD(usize, total, samples[k].len, 156, 24);
	if (samples_len < 64 || total < (64 << 10))
		return false;
	return host_train(samples, samples_len, std_max_usize(4 << 10, std_min_usize(STD_DIV_FOLDED(usize, total, 16), 64 << 10)), result);
}

/* chunks_TuplePathBufArcAtomicBool_t is Listed, (PathBuf, Arc<AtomicBool>): an unfinished pack's file with the flag its writer's drop sets. */
typedef struct chunks_TuplePathBufArcAtomicBool_t
{
	StdPathBuf	f0;
	StdArcInner *f1;
} chunks_TuplePathBufArcAtomicBool_t;

/* chunks_pack_ArcAtomicBool_drop drops an Arc<AtomicBool>. */
static void
chunks_pack_ArcAtomicBool_drop(StdArcInner *arc)
{
	std_arc_drop(arc, sizeof(StdAtomicBool), _Alignof(StdAtomicBool), NULL);
}

/* chunks_pack_Listed_drop is Listed's drop glue: its path's and its flag's. */
static void
chunks_pack_Listed_drop(void *value)
{
	chunks_TuplePathBufArcAtomicBool_t *self = value;

	std_vec_free_Checked(&self->f0.bytes, 1, 1);
	chunks_pack_ArcAtomicBool_drop(self->f1);
}

/* chunks_pack_VecListed_drop is Vec<Listed>'s drop glue: each entry's, then its buffer. */
static void
chunks_pack_VecListed_drop(void *value)
{
	StdVec	   *self = value;

	std_vec_truncate(self, 0, sizeof(chunks_TuplePathBufArcAtomicBool_t), chunks_pack_Listed_drop);
	std_vec_free_Checked(self, sizeof(chunks_TuplePathBufArcAtomicBool_t),
						 _Alignof(chunks_TuplePathBufArcAtomicBool_t));
}

/* chunks_pack_VecListed_Default_default is Vec<Listed>'s Default: no entries. */
static void
chunks_pack_VecListed_Default_default(StdVec *result)
{
	std_vec_new(result, sizeof(chunks_TuplePathBufArcAtomicBool_t), _Alignof(chunks_TuplePathBufArcAtomicBool_t));
}

/* taken at Vec<Listed> and IoMutex<Vec<Listed>>, which pack.c alone uses. */
#define HOST_LOCK_SCOPE static
#define HOST_LOCK_DECLARE
#define HOST_LOCK_DEFINE
#define HOST_LOCK_TAKEN_TS VecListed
#define HOST_LOCK_TAKEN_T StdVec
#define HOST_LOCK_TAKEN_DEFAULT(place) chunks_pack_VecListed_Default_default(place)
#define HOST_LOCK_TAKEN_DROP(place) chunks_pack_VecListed_drop(place)
#define HOST_LOCK_TAKEN_TYPE CHUNKS_PACK_UNFINISHED_LOCK_TYPE
#define HOST_LOCK_IO_MUTEX
#define HOST_LOCK_TS VecListed
#define HOST_LOCK_T StdVec
#define HOST_LOCK_DEFAULT(place) chunks_pack_VecListed_Default_default(place)
#include "doltgres/host/lock_template.h"

/* chunks_pack_IoMutexVecListed_drop is IoMutex<Vec<Listed>>'s drop glue: its value's. */
static void
chunks_pack_IoMutexVecListed_drop(void *value)
{
	HostLockIoMutexVecListed *self = value;

	chunks_pack_VecListed_drop(&self->value);
}

/* CHUNKS_PACK_UNFINISHED_TYPE is IoMutex<Vec<Listed>>'s size, Default, and drop. */
static const StdDefaultType CHUNKS_PACK_UNFINISHED_TYPE = {sizeof(HostLockIoMutexVecListed),
	_Alignof(HostLockIoMutexVecListed), host_lock_IoMutexVecListed_Default_default,
chunks_pack_IoMutexVecListed_drop};

/*
 * CHUNKS_PACK_UNFINISHED is UNFINISHED: the file of each pack this backend's
 * writers haven't finished, with whether its writer was dropped; its
 * BackendLocal header and the process's value, which follows it.
 */
static struct
{
	HostLocalBackendLocal local;
	HostLockIoMutexVecListed process;
}			CHUNKS_PACK_UNFINISHED = {HOST_LOCAL_BACKEND_LOCAL_NEW};

/*
 * chunks_pack_Listed_dropped reports whether an entry's writer was dropped,
 * the flag its drop sets.
 */
static bool
chunks_pack_Listed_dropped(const chunks_TuplePathBufArcAtomicBool_t *entry)
{
	return std_AtomicBool_load_relaxed(std_arc_Deref_deref(entry->f1, _Alignof(StdAtomicBool)));
}

/*
 * chunks_pack_remove_dropped_frame_0_t is remove_dropped's state across its
 * pad: the backend's list and its guard, and the paths it takes off the list,
 * each while live.
 */
typedef struct chunks_pack_remove_dropped_frame_0_t
{
	HostIoIo   *io;
	HostLocalLocal listed;
	bool		listed_live;
	HostLockLockedRwLockWriteGuard guard;
	bool		guard_live;
	/* left is a Vec<PathBuf>. */
	StdVec		left;
	bool		left_live;
	/* path is a dropped writer's path on its way into left. */
	StdPathBuf	path;
	bool		path_live;
} chunks_pack_remove_dropped_frame_0_t;

/* chunks_pack_PathBuf_drop is PathBuf's drop glue: its bytes'. */
void
chunks_pack_PathBuf_drop(void *value)
{
	StdPathBuf *self = value;

	std_vec_free_Checked(&self->bytes, 1, 1);
}

/* chunks_pack_remove_dropped_pad_0 is remove_dropped's pad: the guard, the paths taken, and the list, while live. */
static void
chunks_pack_remove_dropped_pad_0(void *frame_)
{
	chunks_pack_remove_dropped_frame_0_t *frame = frame_;

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
	if (frame->left_live)
	{
		frame->left_live = false;
		std_vec_truncate(&frame->left, 0, sizeof(StdPathBuf), chunks_pack_PathBuf_drop);
		std_vec_free_Checked(&frame->left, sizeof(StdPathBuf), _Alignof(StdPathBuf));
	}
	if (frame->listed_live)
	{
		frame->listed_live = false;
		host_local_Local_drop(&frame->listed, &CHUNKS_PACK_UNFINISHED_TYPE);
	}
}

/*
 * chunks_pack_remove_dropped_body_0 is remove_dropped's body: the dropped
 * writers' paths copied and their entries taken off the list under its lock,
 * then each file removed with the lock released.
 */
static void
chunks_pack_remove_dropped_body_0(void *frame_)
{
	chunks_pack_remove_dropped_frame_0_t *frame = frame_;
	StdVec	   *unfinished;
	size_t		kept = 0;

	frame->listed = host_local_BackendLocal_get(&CHUNKS_PACK_UNFINISHED.local, &CHUNKS_PACK_UNFINISHED_TYPE);
	frame->listed_live = true;
	frame->guard = host_lock_IoMutex_lock((HostLockIoMutex *) host_local_Local_Deref_deref(&frame->listed,
																						   &CHUNKS_PACK_UNFINISHED_TYPE),
										  &CHUNKS_PACK_UNFINISHED_LOCK_TYPE, &frame->io->held);
	frame->guard_live = true;
	unfinished = host_lock_LockedRwLockWriteGuard_DerefMut_deref_mut(&frame->guard);
	std_vec_new(&frame->left, sizeof(StdPathBuf), _Alignof(StdPathBuf));
	frame->left_live = true;
	for (size_t k = 0, k_len = unfinished->len; k < k_len; k++)
	{
		const chunks_TuplePathBufArcAtomicBool_t *entry = &STD_VEC_AT(unfinished, chunks_TuplePathBufArcAtomicBool_t,
																	  k);

		if (chunks_pack_Listed_dropped(entry))
		{
			std_PathBuf_Clone_clone(&entry->f0, &frame->path);
			frame->path_live = true;
			std_vec_push_Checked(&frame->left, &frame->path, sizeof(frame->path), _Alignof(StdPathBuf));
			frame->path_live = false;
		}
	}
	for (size_t k = 0, k_len = unfinished->len; k < k_len; k++)
	{
		chunks_TuplePathBufArcAtomicBool_t *entry = &STD_VEC_AT(unfinished, chunks_TuplePathBufArcAtomicBool_t, k);

		if (chunks_pack_Listed_dropped(entry))
			chunks_pack_Listed_drop(entry);
		else
		{
			STD_VEC_AT(unfinished, chunks_TuplePathBufArcAtomicBool_t, kept) = *entry;
			kept = std_wrapping_add_usize(kept, 1);
		}
	}
	unfinished->len = kept;
	frame->guard_live = false;
	host_lock_LockedRwLockWriteGuard_drop(&frame->guard);
	for (size_t k = 0, k_len = frame->left.len; k < k_len; k++)
	{
		const StdPathBuf *path = &STD_VEC_AT(&frame->left, StdPathBuf, k);

		host_remove_leftover(HOST_IO_IO_IO_DOORS(frame->io), (const char *) path->bytes.ptr, path->bytes.len);
	}
	chunks_pack_remove_dropped_pad_0(frame);
}

/*
 * chunks_pack_remove_dropped removes the files of the packs this backend's
 * writers dropped unfinished, and takes them off its list.
 */
static void
chunks_pack_remove_dropped(HostIoIo *io)
{
	chunks_pack_remove_dropped_frame_0_t frame = {.io = io};

	std_landing(&frame, chunks_pack_remove_dropped_body_0, chunks_pack_remove_dropped_pad_0);
}

/* chunks_pack_Dropped_Exit_run is Dropped's Exit: at the process's exit, the files its writers dropped unfinished go. */
static bool
chunks_pack_Dropped_Exit_run(HostIoIo *io, HostError *error)
{
	chunks_pack_remove_dropped(io);
	return true;
}

/* CHUNKS_PACK_DROPPED_TYPE_ID is Dropped's identity, by which its exit work is registered once. */
static const StdTypeId CHUNKS_PACK_DROPPED_TYPE_ID = {"chunks::pack::Dropped", 0, 1, NULL};

/* CHUNKS_PACK_DROPPED_EXIT_OPS is Dropped's Exit. */
static const HostExitOps CHUNKS_PACK_DROPPED_EXIT_OPS = {chunks_pack_Dropped_Exit_run};

/*
 * chunks_pack_Unfinished_settled_frame_0_t is Unfinished::settled's state
 * across its pad: the backend's list and its guard, while live.
 */
typedef struct chunks_pack_Unfinished_settled_frame_0_t
{
	const ChunksPackUnfinished *self;
	HostIoTakesShort io;
	HostLocalLocal listed;
	bool		listed_live;
	HostLockLockedRwLockWriteGuard guard;
	bool		guard_live;
} chunks_pack_Unfinished_settled_frame_0_t;

/* chunks_pack_Unfinished_settled_pad_0 is Unfinished::settled's pad: the guard, then the list, while live. */
static void
chunks_pack_Unfinished_settled_pad_0(void *frame_)
{
	chunks_pack_Unfinished_settled_frame_0_t *frame = frame_;

	if (frame->guard_live)
	{
		frame->guard_live = false;
		host_lock_LockedRwLockWriteGuard_drop(&frame->guard);
	}
	if (frame->listed_live)
	{
		frame->listed_live = false;
		host_local_Local_drop(&frame->listed, &CHUNKS_PACK_UNFINISHED_TYPE);
	}
}

/* chunks_pack_Unfinished_settled_body_0 is Unfinished::settled's body: the file's entry taken off under the list's lock. */
static void
chunks_pack_Unfinished_settled_body_0(void *frame_)
{
	chunks_pack_Unfinished_settled_frame_0_t *frame = frame_;
	StdVec	   *unfinished;
	size_t		kept = 0;

	frame->listed = host_local_BackendLocal_get(&CHUNKS_PACK_UNFINISHED.local, &CHUNKS_PACK_UNFINISHED_TYPE);
	frame->listed_live = true;
	frame->guard = host_lock_IoMutex_lock((HostLockIoMutex *) host_local_Local_Deref_deref(&frame->listed,
																						   &CHUNKS_PACK_UNFINISHED_TYPE),
										  &CHUNKS_PACK_UNFINISHED_LOCK_TYPE,
										  frame->io.ops->io_doors.read_doors.deref_mut.deref_mut(frame->io.self));
	frame->guard_live = true;
	unfinished = host_lock_LockedRwLockWriteGuard_DerefMut_deref_mut(&frame->guard);
	for (size_t k = 0, k_len = unfinished->len; k < k_len; k++)
	{
		chunks_TuplePathBufArcAtomicBool_t *entry = &STD_VEC_AT(unfinished, chunks_TuplePathBufArcAtomicBool_t, k);

		if (entry->f1 == frame->self->dropped)
			chunks_pack_Listed_drop(entry);
		else
		{
			STD_VEC_AT(unfinished, chunks_TuplePathBufArcAtomicBool_t, kept) = *entry;
			kept = std_wrapping_add_usize(kept, 1);
		}
	}
	unfinished->len = kept;
	chunks_pack_Unfinished_settled_pad_0(frame);
}

/* chunks_pack_Unfinished_settled takes the file out of the list once it is renamed into its pack or removed. */
static void
chunks_pack_Unfinished_settled(const ChunksPackUnfinished *self, HostIoTakesShort io)
{
	chunks_pack_Unfinished_settled_frame_0_t frame = {.self = self, .io = io};

	std_landing(&frame, chunks_pack_Unfinished_settled_body_0, chunks_pack_Unfinished_settled_pad_0);
}

/* chunks_pack_Unfinished_drop sets the entry's flag, which the backend's next writer or its exit acts on, then frees the path and the flag. */
static void
chunks_pack_Unfinished_drop(ChunksPackUnfinished *self)
{
	std_AtomicBool_store_relaxed(std_arc_Deref_deref(self->dropped, _Alignof(StdAtomicBool)), true);
	std_vec_free_Checked(&self->path.bytes, 1, 1);
	chunks_pack_ArcAtomicBool_drop(self->dropped);
}

/* chunks_pack_InDir_Deref_deref returns the pack's file. */
const void *
chunks_pack_InDir_Deref_deref(const void *self)
{
	const ChunksPackInDir *in_dir = self;

	return &in_dir->file;
}

/* chunks_pack_InDir_PackFile_write writes bytes of the pack at an offset of its file under its temporary name. */
bool
chunks_pack_InDir_PackFile_write(const void *self, HostIoTakesShort io, uint64 offset, const uint8 *bytes,
								 size_t bytes_len, ChunksErrorError *error)
{
	const ChunksPackInDir *in_dir = self;

	return chunks_file_write_at(HOST_IO_TAKES_SHORT_IO_DOORS(io), &in_dir->file,
								(const char *) in_dir->temp.path.bytes.ptr, in_dir->temp.path.bytes.len, offset, bytes,
								bytes_len, error);
}

/* chunks_pack_InDir_PackFile_name names the pack's file in an error, by its temporary path. */
StdString
chunks_pack_InDir_PackFile_name(const void *self)
{
	const ChunksPackInDir *in_dir = self;
	StdStr		shown = {(const char *) in_dir->temp.path.bytes.ptr, in_dir->temp.path.bytes.len};
	StdFmtPiece pieces[] = {STD_FMT_LIT("pack file \""), STD_FMT_ARG(0), STD_FMT_LIT("\"")};
	StdFmtArg	args[] = {{&shown, std_path_Display_fmt}};
	StdFmtArguments arguments = {pieces, 3, args};
	StdString	name;

	std_fmt_format_Checked(&name, &arguments);
	return name;
}

/* chunks_pack_ScratchFile_Deref_deref returns the scratch file's File. */
static const void *
chunks_pack_ScratchFile_Deref_deref(const void *self)
{
	return host_ScratchFile_Deref_deref(self);
}

/* chunks_pack_ScratchFile_PackFile_write writes bytes of the pack at an offset of the scratch file. */
bool
chunks_pack_ScratchFile_PackFile_write(const void *self, HostIoTakesShort io, uint64 offset, const uint8 *bytes,
									   size_t bytes_len, ChunksErrorError *error)
{
	HostError	host;

	if (!host_write_at(HOST_IO_TAKES_SHORT_IO_DOORS(io), host_ScratchFile_Deref_deref(self), offset, bytes, bytes_len,
					   &host))
	{
		*error = chunks_error_Error_FromError_from(&host);
		return false;
	}
	return true;
}

/* chunks_pack_ScratchFile_PackFile_name names a scratch pack in an error. */
StdString
chunks_pack_ScratchFile_PackFile_name(const void *self)
{
	StdFmtPiece pieces[] = {STD_FMT_LIT("a scratch pack")};
	StdFmtArguments arguments = {pieces, 1, NULL};
	StdString	name;

	std_fmt_format_Checked(&name, &arguments);
	return name;
}

/* CHUNKS_PACK_IN_DIR_PACK_FILE_OPS is InDir's PackFile, its Deref first. */
const ChunksPackPackFileOps CHUNKS_PACK_IN_DIR_PACK_FILE_OPS = {{chunks_pack_InDir_Deref_deref},
chunks_pack_InDir_PackFile_write, chunks_pack_InDir_PackFile_name};

/* CHUNKS_PACK_SCRATCH_FILE_PACK_FILE_OPS is ScratchFile's PackFile, its Deref first. */
const ChunksPackPackFileOps CHUNKS_PACK_SCRATCH_FILE_PACK_FILE_OPS = {{chunks_pack_ScratchFile_Deref_deref},
chunks_pack_ScratchFile_PackFile_write, chunks_pack_ScratchFile_PackFile_name};

/* chunks_pack_InDir_drop is InDir's drop glue: its file, its directory, its temporary file's entry, and its number's hold. */
void
chunks_pack_InDir_drop(ChunksPackInDir *self)
{
	host_File_drop(&self->file);
	std_vec_free_Checked(&self->dir.bytes, 1, 1);
	chunks_pack_Unfinished_drop(&self->temp);
	if (self->has_owner)
	{
		self->has_owner = false;
		chunks_packs_writers_Writing_drop(&self->owner);
	}
}

/*
 * chunks_pack_RawLayout is Raw's layout inside a ChunksPackRaw's opaque
 * storage: its flag, its data past the slot of the dictionary it was
 * compressed with, and that slot, if any.
 */
typedef struct chunks_pack_RawLayout
{
	uint8		flag;
	StdVec		data;
	bool		has_slot;
	size_t		slot;
} chunks_pack_RawLayout;

StaticAssertDecl(sizeof(chunks_pack_RawLayout) <= sizeof(ChunksPackRaw) &&
				 _Alignof(chunks_pack_RawLayout) <= _Alignof(ChunksPackRaw),
				 "Raw's layout fits ChunksPackRaw's opaque storage");

/* chunks_pack_Raw_drop_in_place is Raw's drop glue: its data's. */
static void
chunks_pack_Raw_drop_in_place(chunks_pack_RawLayout *self)
{
	std_vec_free_Checked(&self->data, 1, 1);
}

/* chunks_pack_Raw_drop is Raw's drop glue, of its opaque storage: its data's. */
void
chunks_pack_Raw_drop(ChunksPackRaw *self)
{
	chunks_pack_Raw_drop_in_place(CHUNKS_PACK_RAW_LAYOUT(self));
}

/* chunks_pack_header_addresses counts the addresses a stored chunk's header names, for Stored::split. */
bool
chunks_pack_header_addresses(void *captures, const uint8 *hashed, size_t hashed_len, size_t *result,
							 ChunksErrorError *error)
{
	ChunksHeaderHeader header;
	size_t		read;

	if (!chunks_header_Header_read(hashed, hashed_len, &header, &read, error))
		return false;
	*result = chunks_header_Addresses_len(&header.addresses);
	chunks_header_Header_drop(&header);
	return true;
}

/* chunks_pack_raw_of splits a chunk record into its flag, the slot of its dictionary, and the rest of its data. */
static bool
chunks_pack_raw_of(const uint8 *record, size_t record_len, chunks_pack_RawLayout *result,
				   ChunksErrorError *error)
{
	const uint8 *data;
	size_t		data_len;
	size_t		at = 0;
	uint64		slot;

	if (record_len == 0)
	{
		*error = chunks_error_Error_corrupt_RefStr("empty chunk record", strlen("empty chunk record"));
		return false;
	}
	data = STD_PTR_AT(record, 1, 1);
	data_len = std_wrapping_sub_usize(record_len, 1);
	result->flag = record[0];
	if (result->flag != CHUNKS_PACK_WITH_DICTIONARY)
	{
		std_slice_to_vec_Checked(&result->data, data, data_len, 1, 1);
		result->has_slot = false;
		return true;
	}
	if (!chunks_varint_read_varint(data, data_len, &at, &slot, error))
		return false;
	std_range_from_check(at, data_len, RUST_FILE, 640, 33);
	std_slice_to_vec_Checked(&result->data, STD_PTR_AT(data, at, 1), std_wrapping_sub_usize(data_len, at), 1, 1);
	result->has_slot = true;
	result->slot = (size_t) slot;
	return true;
}

/*
 * chunks_pack_decompress_frame_0_t is decompress's state across its pad: its
 * arguments, the dictionary function while it is held, and the record split
 * while live.
 */
typedef struct chunks_pack_decompress_frame_0_t
{
	HostIoReadDoors io;
	const uint8 *record;
	size_t		record_len;
	bool		(*dictionary) (void *captures, HostIoReadDoors io, size_t slot,
							   const HostDecompressDictionary **result, ChunksErrorError *error);
	void	   *dictionary_captures;
	void		(*dictionary_drop) (void *captures);
	bool		dictionary_live;
	StdVec	   *result;
	ChunksErrorError *error;
	bool		ok;
	chunks_pack_RawLayout raw;
	bool		raw_live;
} chunks_pack_decompress_frame_0_t;

/* chunks_pack_decompress_pad_0 is decompress's pad: the record split and the dictionary function, while live. */
static void
chunks_pack_decompress_pad_0(void *frame_)
{
	chunks_pack_decompress_frame_0_t *frame = frame_;

	if (frame->raw_live)
	{
		frame->raw_live = false;
		chunks_pack_Raw_drop_in_place(&frame->raw);
	}
	if (frame->dictionary_live)
	{
		frame->dictionary_live = false;
		if (frame->dictionary_drop != NULL)
			frame->dictionary_drop(frame->dictionary_captures);
	}
}

/* chunks_pack_decompress_body_0 is decompress's body: the record's data as it is, or decompressed. */
static void
chunks_pack_decompress_body_0(void *frame_)
{
	chunks_pack_decompress_frame_0_t *frame = frame_;
	uint64		size;
	const HostDecompressDictionary *prepared = NULL;
	bool		decompressed = false;
	HostError	host;

	frame->ok = false;
	if (!chunks_pack_raw_of(frame->record, frame->record_len, &frame->raw, frame->error))
	{
		chunks_pack_decompress_pad_0(frame);
		return;
	}
	frame->raw_live = true;
	if (frame->raw.flag == CHUNKS_PACK_RAW)
	{
		*frame->result = frame->raw.data;
		frame->raw_live = false;
		frame->ok = true;
		chunks_pack_decompress_pad_0(frame);
		return;
	}
	if (!host_frame_size(frame->raw.data.ptr, frame->raw.data.len, &size) ||
		size > (uint64) CHUNKS_PACK_MAX_STORED * 8)
	{
		*frame->error = chunks_error_Error_corrupt_RefStr("chunk record has no valid size",
														  strlen("chunk record has no valid size"));
		chunks_pack_decompress_pad_0(frame);
		return;
	}
	if (frame->raw.has_slot)
	{
		if (!frame->dictionary(frame->dictionary_captures, frame->io, frame->raw.slot, &prepared, frame->error))
		{
			chunks_pack_decompress_pad_0(frame);
			return;
		}
		if (prepared == NULL)
		{
			*frame->error = chunks_error_Error_corrupt_RefStr("chunk record names a dictionary its pack lacks",
															  strlen("chunk record names a dictionary its pack lacks"));
			chunks_pack_decompress_pad_0(frame);
			return;
		}
	}
	if ((frame->raw.flag == CHUNKS_PACK_COMPRESSED && prepared == NULL) ||
		(frame->raw.flag == CHUNKS_PACK_WITH_DICTIONARY && prepared != NULL))
	{
		if (!host_decompress(frame->raw.data.ptr, frame->raw.data.len, (size_t) size, prepared, &decompressed,
							 frame->result, &host))
		{
			*frame->error = chunks_error_Error_FromError_from(&host);
			chunks_pack_decompress_pad_0(frame);
			return;
		}
	}
	if (!decompressed)
	{
		*frame->error = chunks_error_Error_corrupt_RefStr("chunk record fails to decompress",
														  strlen("chunk record fails to decompress"));
		chunks_pack_decompress_pad_0(frame);
		return;
	}
	frame->ok = true;
	chunks_pack_decompress_pad_0(frame);
}

/*
 * chunks_pack_decompress returns a chunk record's stored bytes, decompressed
 * with the dictionary of its slot that the function gives, which it takes by
 * value.
 */
static bool
chunks_pack_decompress(HostIoReadDoors io, const uint8 *record, size_t record_len,
					   bool (*dictionary) (void *captures, HostIoReadDoors io, size_t slot,
										   const HostDecompressDictionary **result, ChunksErrorError *error),
					   void *dictionary_captures, void (*dictionary_drop) (void *captures), StdVec *result,
					   ChunksErrorError *error)
{
	chunks_pack_decompress_frame_0_t frame = {.io = io, .record = record, .record_len = record_len,
		.dictionary = dictionary, .dictionary_captures = dictionary_captures, .dictionary_drop = dictionary_drop,
	.dictionary_live = true, .result = result, .error = error};

	std_landing(&frame, chunks_pack_decompress_body_0, chunks_pack_decompress_pad_0);
	return frame.ok;
}

/* chunks_pack_stored_of_frame_0_t is stored_of's state across its pad: the stored bytes, while live. */
typedef struct chunks_pack_stored_of_frame_0_t
{
	const ChunksAddressAddress *address;
	ChunksChunkChunk *result;
	ChunksErrorError *error;
	bool		ok;
	StdVec		stored;
	bool		stored_live;
} chunks_pack_stored_of_frame_0_t;

/* chunks_pack_stored_of_pad_0 is stored_of's pad: the stored bytes, while live. */
static void
chunks_pack_stored_of_pad_0(void *frame_)
{
	chunks_pack_stored_of_frame_0_t *frame = frame_;

	if (frame->stored_live)
	{
		frame->stored_live = false;
		std_vec_free_Checked(&frame->stored, 1, 1);
	}
}

/* chunks_pack_stored_of_body_0 is stored_of's body: the stored bytes split, and checked against the address. */
static void
chunks_pack_stored_of_body_0(void *frame_)
{
	chunks_pack_stored_of_frame_0_t *frame = frame_;
	ChunksTrailerStored split;

	frame->ok = false;
	if (!chunks_trailer_Stored_split(frame->stored.ptr, frame->stored.len, chunks_pack_header_addresses, NULL, NULL,
									 &split, frame->error))
	{
		chunks_pack_stored_of_pad_0(frame);
		return;
	}
	frame->stored_live = false;
	if (!chunks_chunk_Chunk_checked(frame->address, &frame->stored, split.hashed.len, frame->result))
	{
		StdFmtPiece pieces[] = {STD_FMT_LIT("chunk "), STD_FMT_ARG(0), STD_FMT_LIT(" does not match its address")};
		StdFmtArg	args[] = {{frame->address, chunks_address_Address_Display_fmt}};
		StdFmtArguments arguments = {pieces, 3, args};
		StdString	message;

		std_fmt_format_Checked(&message, &arguments);
		*frame->error = chunks_error_Error_corrupt(&message);
		return;
	}
	frame->ok = true;
}

/*
 * chunks_pack_stored_of returns a chunk record's stored bytes, decompressed
 * with the dictionary of its slot that the function gives, after checking
 * that its hashed bytes have the address.
 */
static bool
chunks_pack_stored_of(HostIoReadDoors io, const uint8 *record, size_t record_len,
					  bool (*dictionary) (void *captures, HostIoReadDoors io, size_t slot,
										  const HostDecompressDictionary **result, ChunksErrorError *error),
					  void *dictionary_captures, void (*dictionary_drop) (void *captures),
					  const ChunksAddressAddress *address, ChunksChunkChunk *result, ChunksErrorError *error)
{
	chunks_pack_stored_of_frame_0_t frame = {.address = address, .result = result, .error = error};

	if (!chunks_pack_decompress(io, record, record_len, dictionary, dictionary_captures, dictionary_drop,
								&frame.stored, error))
		return false;
	frame.stored_live = true;
	std_landing(&frame, chunks_pack_stored_of_body_0, chunks_pack_stored_of_pad_0);
	return frame.ok;
}

/* chunks_pack_address_of returns the address of a chunk's stored bytes, the hash of their hashed part. */
bool
chunks_pack_address_of(const uint8 *stored, size_t stored_len, ChunksAddressAddress *result, ChunksErrorError *error)
{
	ChunksTrailerStored split;

	if (!chunks_trailer_Stored_split(stored, stored_len, chunks_pack_header_addresses, NULL, NULL, &split, error))
		return false;
	*result = chunks_address_Address_of(split.hashed.ptr, split.hashed.len);
	return true;
}

/* chunks_pack_checksum returns the checksum of a pack's header and its footer up to the checksum. */
static uint64
chunks_pack_checksum(const uint8 *header, size_t header_len, const uint8 *footer, size_t footer_len)
{
	XXH3_state_t hasher;

	std_range_to_check(40, footer_len, RUST_FILE, 646, 20);
	XXH3_64bits_reset(&hasher);
	XXH3_64bits_update(&hasher, header, header_len);
	XXH3_64bits_update(&hasher, footer, 40);
	return XXH3_64bits_digest(&hasher);
}

/* chunks_TupleVecU8DecompressDictionary_t is a pack writer's dictionary, (Vec<u8>, DecompressDictionary): its bytes and its prepared form. */
typedef struct chunks_TupleVecU8DecompressDictionary_t
{
	StdVec		f0;
	HostDecompressDictionary f1;
} chunks_TupleVecU8DecompressDictionary_t;

/* chunks_pack_SharedDictionaryValue is a SharedDictionary's value, OnceLock<Option<Vec<u8>>>: its state, then its value. */
typedef struct chunks_pack_SharedDictionaryValue
{
	StdOnceLock lock;
	chunks_OptionVecU8_t value;
} chunks_pack_SharedDictionaryValue;

/* chunks_pack_SharedDictionaryValue_drop is OnceLock<Option<Vec<u8>>>'s drop glue: its dictionary, once set. */
static void
chunks_pack_SharedDictionaryValue_drop(void *value)
{
	chunks_pack_SharedDictionaryValue *self = value;

	if (std_once_lock_get(&self->lock) && self->value.has)
		std_vec_free_Checked(&self->value.value, 1, 1);
}

/* chunks_pack_SharedDictionary_Default_default is SharedDictionary's Default: an Arc of a OnceLock not yet set. */
ChunksPackSharedDictionary
chunks_pack_SharedDictionary_Default_default(void)
{
	chunks_pack_SharedDictionaryValue value = {.lock = STD_ONCE_LOCK_NEW};

	return std_arc_new(&value, sizeof(value), _Alignof(chunks_pack_SharedDictionaryValue));
}

/* chunks_pack_SharedDictionary_drop drops a SharedDictionary, the Arc and its value with its last reference. */
void
chunks_pack_SharedDictionary_drop(ChunksPackSharedDictionary shared)
{
	std_arc_drop(shared, sizeof(chunks_pack_SharedDictionaryValue), _Alignof(chunks_pack_SharedDictionaryValue),
				 chunks_pack_SharedDictionaryValue_drop);
}

/* chunks_pack_SharedDictionary_get returns a SharedDictionary's dictionary once trained, or NULL before. */
const chunks_OptionVecU8_t *
chunks_pack_SharedDictionary_get(ChunksPackSharedDictionary shared)
{
	const chunks_pack_SharedDictionaryValue *value = std_arc_Deref_deref(shared,
																		 _Alignof(chunks_pack_SharedDictionaryValue));

	return std_once_lock_get(&value->lock) ? &value->value : NULL;
}

/* chunks_pack_SharedDictionary_trained reports whether a SharedDictionary is trained, its OnceLock set. */
static bool
chunks_pack_SharedDictionary_trained(ChunksPackSharedDictionary shared)
{
	const chunks_pack_SharedDictionaryValue *value = std_arc_Deref_deref(shared,
																		 _Alignof(chunks_pack_SharedDictionaryValue));

	return std_once_lock_get(&value->lock);
}

/*
 * chunks_pack_SharedDictionary_get_or_init returns a SharedDictionary's
 * dictionary, trained by the function on its first init.
 */
static const chunks_OptionVecU8_t *
chunks_pack_SharedDictionary_get_or_init(ChunksPackSharedDictionary shared, void (*f) (void *captures, void *result),
										 void *f_captures)
{
	chunks_pack_SharedDictionaryValue *value = std_arc_Deref_deref(shared,
																   _Alignof(chunks_pack_SharedDictionaryValue));

	std_once_lock_get_or_init(&value->lock, &value->value, f, f_captures);
	return &value->value;
}

/* chunks_pack_VecU8_drop is Vec<u8>'s drop glue. */
static void
chunks_pack_VecU8_drop(void *value)
{
	std_vec_free_Checked(value, 1, 1);
}

/* chunks_pack_VecVecU8_drop is Vec<Vec<u8>>'s drop glue: each vector's, then its buffer. */
void
chunks_pack_VecVecU8_drop(StdVec *self)
{
	std_vec_truncate(self, 0, sizeof(StdVec), chunks_pack_VecU8_drop);
	std_vec_free_Checked(self, sizeof(StdVec), _Alignof(StdVec));
}

/* chunks_pack_Dictionary_drop is a pack writer's dictionary's drop glue: its bytes, then its prepared form. */
static void
chunks_pack_Dictionary_drop(void *value)
{
	chunks_TupleVecU8DecompressDictionary_t *self = value;

	std_vec_free_Checked(&self->f0, 1, 1);
	host_DecompressDictionary_drop(&self->f1);
}

/* chunks_pack_Dictionaries_drop is a pack writer's dictionaries' drop glue: each one's, then the buffer. */
static void
chunks_pack_Dictionaries_drop(StdVec *self)
{
	std_vec_truncate(self, 0, sizeof(chunks_TupleVecU8DecompressDictionary_t), chunks_pack_Dictionary_drop);
	std_vec_free_Checked(self, sizeof(chunks_TupleVecU8DecompressDictionary_t),
						 _Alignof(chunks_TupleVecU8DecompressDictionary_t));
}

/* chunks_pack_ScratchFile_drop is ScratchFile's drop, for the template's F. */
static void
chunks_pack_ScratchFile_drop(HostScratchFile *self)
{
	host_ScratchFile_drop(self);
}

/* chunks_pack_failed_frame_0_t is failed's state across its pad: the error, which the pad drops should its text's format unwind. */
typedef struct chunks_pack_failed_frame_0_t
{
	StdString  *result;
	ChunksErrorError *error;
	bool		error_live;
} chunks_pack_failed_frame_0_t;

/* chunks_pack_failed_pad_0 is failed's pad: the error, while its caller can't drop it. */
static void
chunks_pack_failed_pad_0(void *frame_)
{
	chunks_pack_failed_frame_0_t *frame = frame_;

	if (frame->error_live)
	{
		frame->error_live = false;
		chunks_error_Error_drop(frame->error);
	}
}

/* chunks_pack_failed_body_0 is failed's body: the error's text. */
static void
chunks_pack_failed_body_0(void *frame_)
{
	chunks_pack_failed_frame_0_t *frame = frame_;
	StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
	StdFmtArg	args[] = {{frame->error, chunks_error_Error_Display_fmt}};
	StdFmtArguments arguments = {pieces, 1, args};

	std_fmt_format_Checked(frame->result, &arguments);
	frame->error_live = false;
}

/*
 * chunks_pack_failed returns a failed write's error's text, which a writer
 * keeps so nothing goes on after it, dropping the error should the format
 * unwind, as the Rust's unwind drops its `err`.
 */
static void
chunks_pack_failed(StdString *result, ChunksErrorError *error)
{
	chunks_pack_failed_frame_0_t frame = {.result = result, .error = error, .error_live = true};

	std_landing(&frame, chunks_pack_failed_body_0, chunks_pack_failed_pad_0);
}

/* PackWriter<InDir>, which pack.h declares. */
#define CHUNKS_PACK_SCOPE extern
#define CHUNKS_PACK_DEFINE
#define CHUNKS_PACK_FS InDir
#define CHUNKS_PACK_F ChunksPackInDir
#define CHUNKS_PACK_F_OPS (&CHUNKS_PACK_IN_DIR_PACK_FILE_OPS)
#define CHUNKS_PACK_F_DROP(place) chunks_pack_InDir_drop(place)
#include "doltgres/chunks/pack_template.h"

/* PackWriter<ScratchFile>, which pack.h declares. */
#define CHUNKS_PACK_SCOPE extern
#define CHUNKS_PACK_DEFINE
#define CHUNKS_PACK_FS ScratchFile
#define CHUNKS_PACK_F HostScratchFile
#define CHUNKS_PACK_F_OPS (&CHUNKS_PACK_SCRATCH_FILE_PACK_FILE_OPS)
#define CHUNKS_PACK_F_DROP(place) chunks_pack_ScratchFile_drop(place)
#include "doltgres/chunks/pack_template.h"

/*
 * chunks_pack_PackWriterInDir_create_frame_0_t is create's state across its
 * pad: the file's name and the unfinished file, while live.
 */
typedef struct chunks_pack_PackWriterInDir_create_frame_0_t
{
	HostIoIo   *io;
	const char *dir;
	size_t		dir_len;
	uint64		number;
	ChunksPackPackWriterInDir *result;
	ChunksErrorError *error;
	bool		ok;
	StdString	extension;
	bool		extension_live;
	StdString	name;
	bool		name_live;
	ChunksPackUnfinished temp;
	bool		temp_live;
	HostLocalLocal listed;
	bool		listed_live;
	HostLockLockedRwLockWriteGuard guard;
	bool		guard_live;
	ChunksPackInDir in_dir;
	bool		file_live;
	bool		in_dir_live;
} chunks_pack_PackWriterInDir_create_frame_0_t;

/* chunks_pack_PackWriterInDir_create_pad_0 is create's pad: what it holds, while live. */
static void
chunks_pack_PackWriterInDir_create_pad_0(void *frame_)
{
	chunks_pack_PackWriterInDir_create_frame_0_t *frame = frame_;

	if (frame->in_dir_live)
	{
		frame->in_dir_live = false;
		chunks_pack_InDir_drop(&frame->in_dir);
	}
	if (frame->file_live)
	{
		frame->file_live = false;
		host_File_drop(&frame->in_dir.file);
	}
	if (frame->guard_live)
	{
		frame->guard_live = false;
		host_lock_LockedRwLockWriteGuard_drop(&frame->guard);
	}
	if (frame->listed_live)
	{
		frame->listed_live = false;
		host_local_Local_drop(&frame->listed, &CHUNKS_PACK_UNFINISHED_TYPE);
	}
	if (frame->temp_live)
	{
		frame->temp_live = false;
		chunks_pack_Unfinished_drop(&frame->temp);
	}
	if (frame->name_live)
	{
		frame->name_live = false;
		std_vec_free_Checked(&frame->name, 1, 1);
	}
	if (frame->extension_live)
	{
		frame->extension_live = false;
		std_vec_free_Checked(&frame->extension, 1, 1);
	}
}

/*
 * chunks_pack_PackWriterInDir_create_body_0 is create's body: the exit work
 * registered, the dropped writers' files removed, and the file made and
 * listed.
 */
static void
chunks_pack_PackWriterInDir_create_body_0(void *frame_)
{
	chunks_pack_PackWriterInDir_create_frame_0_t *frame = frame_;
	ChunksPackPackId id = chunks_pack_PackId_new();
	uint32		process;
	chunks_TuplePathBufArcAtomicBool_t entry;
	StdVec	   *unfinished;
	HostError	host;
	StdAtomicBool dropped;

	frame->ok = false;
	if (!host_at_exit(&CHUNKS_PACK_DROPPED_TYPE_ID, &CHUNKS_PACK_DROPPED_EXIT_OPS, &host))
	{
		*frame->error = chunks_error_Error_FromError_from(&host);
		return;
	}
	chunks_pack_remove_dropped(frame->io);
	process = host_shared_proc_number();
	{
		StdStr		temp = {CHUNKS_PACK_TEMP, strlen(CHUNKS_PACK_TEMP)};
		StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT("."), STD_FMT_ARG(1), STD_FMT_ARG(2)};
		StdFmtArg	args[] = {{&process, std_u32_Display_fmt}, {&frame->number, std_u64_Display_fmt},
		{&temp, std_str_Display_fmt}};
		StdFmtArguments arguments = {pieces, 4, args};

		std_fmt_format_Checked(&frame->extension, &arguments);
	}
	frame->extension_live = true;
	frame->name = chunks_pack_PackId_file_name(&id, (const char *) frame->extension.ptr, frame->extension.len);
	frame->name_live = true;
	frame->extension_live = false;
	std_vec_free_Checked(&frame->extension, 1, 1);
	std_path_join(frame->dir, frame->dir_len, (const char *) frame->name.ptr, frame->name.len, &frame->temp.path);
	frame->name_live = false;
	std_vec_free_Checked(&frame->name, 1, 1);
	std_AtomicBool_new(&dropped, false);
	frame->temp.dropped = std_arc_new(&dropped, sizeof(dropped), _Alignof(StdAtomicBool));
	frame->temp_live = true;
	frame->listed = host_local_BackendLocal_get(&CHUNKS_PACK_UNFINISHED.local, &CHUNKS_PACK_UNFINISHED_TYPE);
	frame->listed_live = true;
	frame->guard = host_lock_IoMutex_lock((HostLockIoMutex *) host_local_Local_Deref_deref(&frame->listed,
																						   &CHUNKS_PACK_UNFINISHED_TYPE),
										  &CHUNKS_PACK_UNFINISHED_LOCK_TYPE, &frame->io->held);
	frame->guard_live = true;
	unfinished = host_lock_LockedRwLockWriteGuard_DerefMut_deref_mut(&frame->guard);
	std_PathBuf_Clone_clone(&frame->temp.path, &entry.f0);
	entry.f1 = std_arc_clone(frame->temp.dropped);
	std_vec_push_Checked(unfinished, &entry, sizeof(entry), _Alignof(chunks_TuplePathBufArcAtomicBool_t));
	frame->guard_live = false;
	host_lock_LockedRwLockWriteGuard_drop(&frame->guard);
	frame->listed_live = false;
	host_local_Local_drop(&frame->listed, &CHUNKS_PACK_UNFINISHED_TYPE);
	if (!chunks_file_create(HOST_IO_IO_IO_DOORS(frame->io), (const char *) frame->temp.path.bytes.ptr,
							frame->temp.path.bytes.len, &frame->in_dir.file, frame->error))
	{
		chunks_pack_PackWriterInDir_create_pad_0(frame);
		return;
	}
	frame->file_live = true;
	std_path_to_path_buf(frame->dir, frame->dir_len, &frame->in_dir.dir);
	frame->in_dir.temp = frame->temp;
	frame->in_dir.has_owner = false;
	frame->in_dir_live = true;
	frame->file_live = false;
	frame->temp_live = false;
	chunks_pack_PackWriterInDir_over(&frame->in_dir, frame->number, id, frame->result);
	frame->in_dir_live = false;
	frame->ok = true;
}

/*
 * chunks_pack_PackWriterInDir_create starts a pack in the directory,
 * numbered in its repository's pack table, which trains no dictionary unless
 * train or share asks for one. Its file's name carries, after the pack's id,
 * the number of the process that writes it and the pack's number.
 */
bool
chunks_pack_PackWriterInDir_create(HostIoIo *io, const char *dir, size_t dir_len, uint64 number,
								   ChunksPackPackWriterInDir *result, ChunksErrorError *error)
{
	chunks_pack_PackWriterInDir_create_frame_0_t frame = {.io = io, .dir = dir, .dir_len = dir_len,
	.number = number, .result = result, .error = error};

	std_landing(&frame, chunks_pack_PackWriterInDir_create_body_0, chunks_pack_PackWriterInDir_create_pad_0);
	return frame.ok;
}

/* chunks_pack_PackWriterInDir_owned gives the pack the hold of its number among its process's open writers. */
void
chunks_pack_PackWriterInDir_owned(ChunksPackPackWriterInDir *self, ChunksPacksWritersWriting *owner)
{
	if (self->file.has_owner)
		chunks_packs_writers_Writing_drop(&self->file.owner);
	self->file.owner = *owner;
	self->file.has_owner = true;
}

/* chunks_pack_PackWriterInDir_abandon_closure_0 is abandon's event: the pack abandoned, by its number. */
static void
chunks_pack_PackWriterInDir_abandon_closure_0(void *captures, ChunksTraceEvent *result)
{
	result->tag = CHUNKS_TRACE_EVENT_PACK_ABANDONED;
	result->u.pack_abandoned.pack = *(const uint64 *) captures;
}

/* chunks_pack_PackWriterInDir_abandon_frame_0_t is abandon's state across its pad: the writer it takes. */
typedef struct chunks_pack_PackWriterInDir_abandon_frame_0_t
{
	ChunksPackPackWriterInDir *self;
	HostIoTakesShort io;
	ChunksErrorError *error;
	bool		ok;

	/*
	 * parts_live is whether the writer's parts but its file and its number's
	 * hold are still to drop.
	 */
	bool		parts_live;
} chunks_pack_PackWriterInDir_abandon_frame_0_t;

/*
 * chunks_pack_PackWriterInDir_abandon_pad_0 is abandon's pad: the writer's
 * parts but its file, which the body closes first, its number's hold last.
 */
static void
chunks_pack_PackWriterInDir_abandon_pad_0(void *frame_)
{
	chunks_pack_PackWriterInDir_abandon_frame_0_t *frame = frame_;
	ChunksPackPackWriterInDir *self = frame->self;
	ChunksPackInDir *in_dir = &self->file;

	if (in_dir->has_owner)
	{
		in_dir->has_owner = false;
		chunks_packs_writers_Writing_drop(&in_dir->owner);
	}
	if (!frame->parts_live)
		return;
	frame->parts_live = false;
	std_vec_free_Checked(&in_dir->dir.bytes, 1, 1);
	chunks_pack_Unfinished_drop(&in_dir->temp);
	std_vec_free_Checked(&self->header, 1, 1);
	std_vec_free_Checked(&self->buffer, 1, 1);
	std_vec_free_Checked(&self->entries, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
	if (self->has_samples)
	{
		self->has_samples = false;
		chunks_pack_VecVecU8_drop(&self->samples);
	}
	chunks_pack_Dictionaries_drop(&self->dictionaries);
	if (self->has_own)
	{
		self->has_own = false;
		host_CompressDictionary_drop(&self->own.f1);
	}
	if (self->has_shared)
	{
		self->has_shared = false;
		chunks_pack_SharedDictionary_drop(self->shared);
	}
	if (self->has_failed)
	{
		self->has_failed = false;
		std_vec_free_Checked(&self->failed, 1, 1);
	}
}

/* chunks_pack_PackWriterInDir_abandon_body_0 is abandon's body: the file closed and removed, and taken off the list. */
static void
chunks_pack_PackWriterInDir_abandon_body_0(void *frame_)
{
	chunks_pack_PackWriterInDir_abandon_frame_0_t *frame = frame_;
	ChunksPackPackWriterInDir *self = frame->self;
	ChunksPackInDir *in_dir = &self->file;

	host_File_drop(&in_dir->file);
	frame->ok = chunks_file_remove_file(HOST_IO_TAKES_SHORT_IO_DOORS(frame->io),
										(const char *) in_dir->temp.path.bytes.ptr, in_dir->temp.path.bytes.len,
										frame->error);
	if (frame->ok)
	{
		chunks_pack_Unfinished_settled(&in_dir->temp, frame->io);
		chunks_trace_emit(self->repo, chunks_pack_PackWriterInDir_abandon_closure_0, &self->number, NULL);
	}
	chunks_pack_PackWriterInDir_abandon_pad_0(frame);
}

/*
 * chunks_pack_PackWriterInDir_abandon removes the pack before it is
 * finished, and only then lets its number go, so no collection takes the
 * file for one no writer has open while this removes it. It takes the
 * writer.
 */
bool
chunks_pack_PackWriterInDir_abandon(ChunksPackPackWriterInDir *self, HostIoTakesShort io, ChunksErrorError *error)
{
	chunks_pack_PackWriterInDir_abandon_frame_0_t frame = {.self = self, .io = io, .error = error, .parts_live = true};

	std_landing(&frame, chunks_pack_PackWriterInDir_abandon_body_0, chunks_pack_PackWriterInDir_abandon_pad_0);
	return frame.ok;
}

/* chunks_pack_TupleAddressU64_lt is (Address, u64)'s PartialOrd::lt: by address, then by offset. */
bool
chunks_pack_TupleAddressU64_lt(const chunks_TupleAddressU64_t *a, const chunks_TupleAddressU64_t *b)
{
	if (chunks_address_Address_PartialOrd_lt(&a->f0, &b->f0))
		return true;
	return chunks_address_Address_PartialEq_eq(&a->f0, &b->f0) && a->f1 < b->f1;
}

/* The sort of a pack's (Address, Locator) entries: core's shared sort parts at the tuple, which its stable sort calls. */
#define STD_SORT_SCOPE static
#define STD_SORT_DECLARE
#define STD_SORT_DEFINE
#define STD_SORT_SHARED_ONLY
#define STD_SORT_WITHOUT_UNSTABLE
#define STD_SORT_TS PackTupleAddressLocator
#define STD_SORT_T chunks_TupleAddressLocator_t
#define STD_SORT_FREEZE 1
#include "doltgres/std/sort_template.h"

/* The stable sort_by_key of a pack's (Address, Locator) entries by their address and offset, and its sort_by. */
#define STD_STABLE_SORT_SCOPE static
#define STD_STABLE_SORT_DECLARE
#define STD_STABLE_SORT_DEFINE
#define STD_STABLE_SORT_TS PackTupleAddressLocator
#define STD_STABLE_SORT_T chunks_TupleAddressLocator_t
#define STD_STABLE_SORT_FREEZE 1
#define STD_STABLE_SORT_WITHOUT_SORT
#define STD_STABLE_SORT_WITH_BY
#define STD_STABLE_SORT_WITH_BY_KEY
#define STD_STABLE_SORT_KEY_KS TupleAddressU64
#define STD_STABLE_SORT_KEY_K chunks_TupleAddressU64_t
#define STD_STABLE_SORT_KEY_LT(a, b) chunks_pack_TupleAddressU64_lt((a), (b))
#include "doltgres/std/stable_sort_template.h"

/* chunks_pack_PackWriterInDir_finish_closure_0 is finish's sort key: an entry's address and its locator's offset. */
static chunks_TupleAddressU64_t
chunks_pack_PackWriterInDir_finish_closure_0(void *captures, const void *elem)
{
	const chunks_TupleAddressLocator_t *entry = elem;
	chunks_TupleAddressU64_t key = {entry->f0, chunks_trailer_Locator_offset(&entry->f1)};

	return key;
}

/* chunks_pack_PackWriterInDir_finish_closure_1 is finish's event: the pack synced, by its number. */
static void
chunks_pack_PackWriterInDir_finish_closure_1(void *captures, ChunksTraceEvent *result)
{
	result->tag = CHUNKS_TRACE_EVENT_PACK_SYNCED;
	result->u.pack_synced.pack = *(const uint64 *) captures;
}

/*
 * chunks_pack_PackWriterInDir_finish_frame_0_t is finish's state across its
 * pad: the writer it takes, and what it builds, each while live.
 */
typedef struct chunks_pack_PackWriterInDir_finish_frame_0_t
{
	ChunksPackPackWriterInDir *self;
	HostIoTakesShort io;
	ChunksPackPack *result;
	ChunksErrorError *error;
	bool		ok;
	bool		self_live;
	StdVec		table;
	bool		table_live;
	StdVec		entries;
	bool		entries_live;
	StdVec		addresses;
	bool		addresses_live;
	StdVec		footer;
	bool		footer_live;
	StdString	name;
	bool		name_live;
	StdPathBuf	path;
	bool		path_live;
	StdPathBuf	index;
	bool		index_live;
	ChunksIndexRunLocator run;
	bool		run_live;
} chunks_pack_PackWriterInDir_finish_frame_0_t;

/* chunks_pack_PackWriterInDir_finish_pad_0 is finish's pad: what it holds, then the writer, while live. */
static void
chunks_pack_PackWriterInDir_finish_pad_0(void *frame_)
{
	chunks_pack_PackWriterInDir_finish_frame_0_t *frame = frame_;

	if (frame->run_live)
	{
		frame->run_live = false;
		chunks_index_RunLocator_drop(&frame->run);
	}
	if (frame->index_live)
	{
		frame->index_live = false;
		std_vec_free_Checked(&frame->index.bytes, 1, 1);
	}
	if (frame->table_live)
	{
		frame->table_live = false;
		std_vec_free_Checked(&frame->table, 1, 1);
	}
	if (frame->entries_live)
	{
		frame->entries_live = false;
		std_vec_free_Checked(&frame->entries, sizeof(chunks_TupleAddressLocator_t),
							 _Alignof(chunks_TupleAddressLocator_t));
	}
	if (frame->addresses_live)
	{
		frame->addresses_live = false;
		std_vec_free_Checked(&frame->addresses, 1, 1);
	}
	if (frame->footer_live)
	{
		frame->footer_live = false;
		std_vec_free_Checked(&frame->footer, 1, 1);
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
	if (frame->self_live)
	{
		frame->self_live = false;
		chunks_pack_PackWriterInDir_drop(frame->self);
	}
}

/* chunks_pack_put_le appends an integer's little-endian bytes of a width. */
static void
chunks_pack_put_le(StdVec *out, uint64 value, size_t width)
{
	uint8		bytes[8];

	std_u64_to_le_bytes(value, bytes);
	std_vec_extend_from_slice_Checked(out, bytes, width, 1, 1);
}

/*
 * chunks_pack_PackWriterInDir_finish_body_0 is finish's body: the dictionary
 * table, the address table, and the footer written, the pack synced and
 * named, and its run written.
 */
static void
chunks_pack_PackWriterInDir_finish_body_0(void *frame_)
{
	chunks_pack_PackWriterInDir_finish_frame_0_t *frame = frame_;
	ChunksPackPackWriterInDir *self = frame->self;
	HostIoIoDoors doors = HOST_IO_TAKES_SHORT_IO_DOORS(frame->io);
	uint64		table_at;
	uint64		count;
	uint64		addresses_at;

	frame->ok = false;
	if (!chunks_pack_PackWriterInDir_usable(self, frame->error))
	{
		chunks_pack_PackWriterInDir_finish_pad_0(frame);
		return;
	}
	std_vec_new(&frame->table, 1, 1);
	frame->table_live = true;
	if (self->dictionaries.len != 0)
	{
		chunks_pack_put_le(&frame->table, (uint64) (uint16) self->dictionaries.len, 2);
		for (size_t k = 0, k_len = self->dictionaries.len; k < k_len; k++)
		{
			const StdVec *dictionary = &STD_VEC_AT(&self->dictionaries, chunks_TupleVecU8DecompressDictionary_t, k).f0;

			chunks_pack_put_le(&frame->table, (uint64) (uint32) dictionary->len, 4);
			chunks_pack_put_le(&frame->table, XXH3_64bits(dictionary->ptr, dictionary->len), 8);
		}
		for (size_t k = 0, k_len = self->dictionaries.len; k < k_len; k++)
		{
			const StdVec *dictionary = &STD_VEC_AT(&self->dictionaries, chunks_TupleVecU8DecompressDictionary_t, k).f0;

			std_vec_extend_from_slice_Checked(&frame->table, dictionary->ptr, dictionary->len, 1, 1);
		}
	}
	table_at = frame->table.len == 0 ? 0 : chunks_pack_PackWriterInDir_size(self);
	std_vec_extend_from_slice_Checked(&self->buffer, frame->table.ptr, frame->table.len, 1, 1);
	count = (uint64) self->entries.len;
	frame->entries = self->entries;
	std_vec_new(&self->entries, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
	frame->entries_live = true;
	std_slice_sort_by_key_PackTupleAddressLocatorTupleAddressU64(frame->entries.ptr, frame->entries.len,
																 chunks_pack_PackWriterInDir_finish_closure_0, NULL,
																 NULL);
	if (!chunks_index_RunLocator_encode(frame->entries.ptr, frame->entries.len, &frame->addresses, frame->error))
	{
		chunks_pack_PackWriterInDir_finish_pad_0(frame);
		return;
	}
	frame->addresses_live = true;
	addresses_at = chunks_pack_PackWriterInDir_size(self);
	std_vec_extend_from_slice_Checked(&self->buffer, frame->addresses.ptr, frame->addresses.len, 1, 1);
	std_vec_with_capacity_Checked(&frame->footer, CHUNKS_PACK_FOOTER, 1, 1);
	frame->footer_live = true;
	chunks_pack_put_le(&frame->footer, table_at, 8);
	chunks_pack_put_le(&frame->footer, (uint64) (uint32) frame->table.len, 4);
	chunks_pack_put_le(&frame->footer, count, 8);
	chunks_pack_put_le(&frame->footer, (uint64) (uint32) self->header.len, 4);
	chunks_pack_put_le(&frame->footer, addresses_at, 8);
	chunks_pack_put_le(&frame->footer, (uint64) (uint32) frame->addresses.len, 4);
	chunks_pack_put_le(&frame->footer,
					   (uint64) (uint32) std_div_ceil_usize(frame->entries.len, chunks_index_RunLocator_per_block(),
															RUST_FILE, 371, 53), 4);
	chunks_pack_put_le(&frame->footer, chunks_pack_checksum(self->header.ptr, self->header.len, frame->footer.ptr,
															frame->footer.len), 8);
	std_vec_extend_from_slice_Checked(&frame->footer, CHUNKS_PACK_MAGIC, sizeof(CHUNKS_PACK_MAGIC), 1, 1);
	std_vec_extend_from_slice_Checked(&self->buffer, frame->footer.ptr, frame->footer.len, 1, 1);
	if (!chunks_pack_PackWriterInDir_flush(self, frame->io, frame->error) ||
		!chunks_file_sync(doors, &self->file.file, frame->error))
	{
		chunks_pack_PackWriterInDir_finish_pad_0(frame);
		return;
	}
	frame->name = chunks_pack_PackId_file_name(&self->id, "pack", 4);
	frame->name_live = true;
	std_path_join((const char *) self->file.dir.bytes.ptr, self->file.dir.bytes.len, (const char *) frame->name.ptr,
				  frame->name.len, &frame->path);
	frame->path_live = true;
	frame->name_live = false;
	std_vec_free_Checked(&frame->name, 1, 1);
	if (!chunks_file_rename(doors, (const char *) self->file.temp.path.bytes.ptr, self->file.temp.path.bytes.len,
							(const char *) frame->path.bytes.ptr, frame->path.bytes.len, frame->error))
	{
		chunks_pack_PackWriterInDir_finish_pad_0(frame);
		return;
	}
	chunks_pack_Unfinished_settled(&self->file.temp, frame->io);
	frame->name = chunks_pack_PackId_file_name(&self->id, "idx", 3);
	frame->name_live = true;
	std_path_join((const char *) self->file.dir.bytes.ptr, self->file.dir.bytes.len, (const char *) frame->name.ptr,
				  frame->name.len, &frame->index);
	frame->index_live = true;
	frame->name_live = false;
	std_vec_free_Checked(&frame->name, 1, 1);
	if (!chunks_index_written_run(doors, (const char *) frame->index.bytes.ptr, frame->index.bytes.len,
								  frame->entries.ptr, frame->entries.len, &frame->run, frame->error))
	{
		chunks_pack_PackWriterInDir_finish_pad_0(frame);
		return;
	}
	frame->run_live = true;
	frame->index_live = false;
	std_vec_free_Checked(&frame->index.bytes, 1, 1);
	if (!chunks_file_sync_dir(doors, (const char *) self->file.dir.bytes.ptr, self->file.dir.bytes.len,
							  frame->error))
	{
		chunks_pack_PackWriterInDir_finish_pad_0(frame);
		return;
	}
	chunks_trace_emit(self->repo, chunks_pack_PackWriterInDir_finish_closure_1, &self->number, NULL);
	frame->result->id = self->id;
	frame->result->number = self->number;
	frame->result->path = frame->path;
	frame->path_live = false;
	frame->result->run = frame->run;
	frame->run_live = false;
	frame->result->has_owner = self->file.has_owner;
	frame->result->owner = self->file.owner;
	self->file.has_owner = false;
	frame->ok = true;
	chunks_pack_PackWriterInDir_finish_pad_0(frame);
}

/*
 * chunks_pack_PackWriterInDir_finish writes the dictionary table and the
 * footer, syncs the pack, gives it its name, and writes its run of the
 * address index; the pack is durable when it returns. It takes the writer.
 */
bool
chunks_pack_PackWriterInDir_finish(ChunksPackPackWriterInDir *self, HostIoTakesShort io, ChunksPackPack *result,
								   ChunksErrorError *error)
{
	chunks_pack_PackWriterInDir_finish_frame_0_t frame = {.self = self, .io = io, .result = result, .error = error,
	.self_live = true};

	std_landing(&frame, chunks_pack_PackWriterInDir_finish_body_0, chunks_pack_PackWriterInDir_finish_pad_0);
	return frame.ok;
}

/*
 * chunks_pack_PackWriterScratchFile_scratch starts a pack no repository
 * lists, numbered 0 as no repository numbers a pack, in a scratch file of the
 * entry's transaction, which goes when the writer drops.
 */
/* chunks_pack_PackWriterScratchFile_scratch_frame_0_t is scratch's state across its pad: the scratch file, until the writer takes it. */
typedef struct chunks_pack_PackWriterScratchFile_scratch_frame_0_t
{
	HostScratchFile file;
	bool		file_live;
	ChunksPackPackWriterScratchFile *result;
} chunks_pack_PackWriterScratchFile_scratch_frame_0_t;

/* chunks_pack_PackWriterScratchFile_scratch_pad_0 is scratch's pad: the scratch file, while it owns it. */
static void
chunks_pack_PackWriterScratchFile_scratch_pad_0(void *frame_)
{
	chunks_pack_PackWriterScratchFile_scratch_frame_0_t *frame = frame_;

	if (frame->file_live)
	{
		frame->file_live = false;
		host_ScratchFile_drop(&frame->file);
	}
}

/* chunks_pack_PackWriterScratchFile_scratch_body_0 is scratch's body: the writer over the file, numbered 0. */
static void
chunks_pack_PackWriterScratchFile_scratch_body_0(void *frame_)
{
	chunks_pack_PackWriterScratchFile_scratch_frame_0_t *frame = frame_;

	chunks_pack_PackWriterScratchFile_over(&frame->file, 0, chunks_pack_PackId_new(), frame->result);
	frame->file_live = false;
}

/*
 * chunks_pack_PackWriterScratchFile_scratch starts a pack no repository
 * lists, numbered 0 as no repository numbers a pack, in a scratch file of the
 * entry's transaction, which goes when the writer drops.
 */
bool
chunks_pack_PackWriterScratchFile_scratch(HostIoIo *io, ChunksPackPackWriterScratchFile *result,
										  ChunksErrorError *error)
{
	chunks_pack_PackWriterScratchFile_scratch_frame_0_t frame = {.result = result};
	HostError	host;

	if (!host_open_scratch(io, &frame.file, &host))
	{
		*error = chunks_error_Error_FromError_from(&host);
		return false;
	}
	frame.file_live = true;
	std_landing(&frame, chunks_pack_PackWriterScratchFile_scratch_body_0, chunks_pack_PackWriterScratchFile_scratch_pad_0);
	return true;
}

/* chunks_pack_Pack_drop is Pack's drop glue: its path, its run, and its number's hold. */
void
chunks_pack_Pack_drop(ChunksPackPack *self)
{
	std_vec_free_Checked(&self->path.bytes, 1, 1);
	chunks_index_RunLocator_drop(&self->run);
	if (self->has_owner)
	{
		self->has_owner = false;
		chunks_packs_writers_Writing_drop(&self->owner);
	}
}

/*
 * chunks_pack_Slot is Slot: a dictionary a pack keeps, where its bytes lie in
 * the file, their checksum, and their prepared form once a read needed it,
 * a OnceLock<DecompressDictionary>'s state and value.
 */
typedef struct chunks_pack_Slot
{
	uint64		at;
	uint32		len;
	uint64		sum;
	StdOnceLock lock;
	HostDecompressDictionary prepared;
} chunks_pack_Slot;

/* chunks_pack_Slot_drop is Slot's drop glue: its prepared dictionary, once set. */
static void
chunks_pack_Slot_drop(void *value)
{
	chunks_pack_Slot *self = value;

	if (std_once_lock_get(&self->lock))
		host_DecompressDictionary_drop(&self->prepared);
}

/* chunks_pack_Slots_drop is Vec<Slot>'s drop glue: each slot's, then the buffer. */
static void
chunks_pack_Slots_drop(StdVec *self)
{
	std_vec_truncate(self, 0, sizeof(chunks_pack_Slot), chunks_pack_Slot_drop);
	std_vec_free_Checked(self, sizeof(chunks_pack_Slot), _Alignof(chunks_pack_Slot));
}

/*
 * chunks_pack_dictionary_table returns the dictionaries a pack's table, read
 * from an offset of its file, names, or nothing when the table's count or
 * lengths pass its end or leave bytes over, or it names more than
 * MAX_DICTIONARIES.
 */
static bool
chunks_pack_dictionary_table(const uint8 *table, size_t table_len, uint64 at, StdVec *result)
{
	size_t		count;
	size_t		lens_len;
	uint64		offset;
	uint64		end;

	if (table_len < 2)
		return false;
	count = (size_t) std_u16_from_le_bytes(table);
	if (count > CHUNKS_PACK_MAX_DICTIONARIES)
		return false;
	lens_len = std_wrapping_add_usize(2, std_wrapping_mul_usize(12, count));
	if (lens_len > table_len || !std_checked_add_u64(at, (uint64) lens_len, &offset))
		return false;
	std_vec_with_capacity_Checked(result, count, sizeof(chunks_pack_Slot), _Alignof(chunks_pack_Slot));
	for (size_t k = 0, k_len = count; k < k_len; k++)
	{
		const uint8 *entry = STD_PTR_AT(table, std_wrapping_add_usize(2, std_wrapping_mul_usize(12, k)), 1);
		chunks_pack_Slot slot = {.at = offset, .len = std_u32_from_le_bytes(entry),
		.sum = std_u64_from_le_bytes(STD_PTR_AT(entry, 4, 1)), .lock = STD_ONCE_LOCK_NEW};

		std_vec_push_Checked(result, &slot, sizeof(slot), _Alignof(chunks_pack_Slot));
		if (!std_checked_add_u64(offset, (uint64) slot.len, &offset))
		{
			chunks_pack_Slots_drop(result);
			return false;
		}
	}
	if (!std_checked_add_u64(at, (uint64) table_len, &end) || offset != end)
	{
		chunks_pack_Slots_drop(result);
		return false;
	}
	return true;
}

/* chunks_pack_PackReader_path returns the pack's file. */
const char *
chunks_pack_PackReader_path(const ChunksPackPackReader *self, size_t *result_len)
{
	*result_len = self->path.bytes.len;
	return (const char *) self->path.bytes.ptr;
}

/* chunks_pack_PackReader_dead reads the generation of the pack's deletion vector that the pack table's record of the pack names. */
bool
chunks_pack_PackReader_dead(const ChunksPackPackReader *self, HostIoReadDoors io, uint64 generation,
							ChunksDeadDead *result, ChunksErrorError *error)
{
	return chunks_dead_Dead_read(io, (const char *) self->path.bytes.ptr, self->path.bytes.len, generation, result,
								 error);
}

/* chunks_pack_corrupt_pack returns the error of a pack whose header or footer is corrupt, naming its path. */
static ChunksErrorError
chunks_pack_corrupt_pack(const char *path, size_t path_len)
{
	StdStr		shown = {path, path_len};
	StdFmtPiece pieces[] = {STD_FMT_LIT("pack "), STD_FMT_ARG(0), STD_FMT_LIT(" is corrupt")};
	StdFmtArg	args[] = {{&shown, std_path_Display_fmt}};
	StdFmtArguments arguments = {pieces, 3, args};
	StdString	message;

	std_fmt_format_Checked(&message, &arguments);
	return chunks_error_Error_corrupt(&message);
}

/* chunks_pack_PackReader_open_frame_0_t is open's state across its pad: the file and what it reads, while live. */
typedef struct chunks_pack_PackReader_open_frame_0_t
{
	HostIoReadDoors io;
	const char *path;
	size_t		path_len;
	ChunksPackPackReader *result;
	ChunksErrorError *error;
	bool		ok;
	HostFile	file;
	bool		file_live;
	StdVec		footer;
	bool		footer_live;
	StdVec		header;
	bool		header_live;
	StdVec		table;
	bool		table_live;
	StdVec		dictionaries;
	bool		dictionaries_live;
} chunks_pack_PackReader_open_frame_0_t;

/* chunks_pack_PackReader_open_pad_0 is open's pad: what it holds, while live. */
static void
chunks_pack_PackReader_open_pad_0(void *frame_)
{
	chunks_pack_PackReader_open_frame_0_t *frame = frame_;

	if (frame->dictionaries_live)
	{
		frame->dictionaries_live = false;
		chunks_pack_Slots_drop(&frame->dictionaries);
	}
	if (frame->table_live)
	{
		frame->table_live = false;
		std_vec_free_Checked(&frame->table, 1, 1);
	}
	if (frame->header_live)
	{
		frame->header_live = false;
		std_vec_free_Checked(&frame->header, 1, 1);
	}
	if (frame->footer_live)
	{
		frame->footer_live = false;
		std_vec_free_Checked(&frame->footer, 1, 1);
	}
	if (frame->file_live)
	{
		frame->file_live = false;
		host_File_drop(&frame->file);
	}
}

/* chunks_pack_PackReader_open_body_0 is open's body: the header and footer read and checked, and the tables found. */
static void
chunks_pack_PackReader_open_body_0(void *frame_)
{
	chunks_pack_PackReader_open_frame_0_t *frame = frame_;
	uint64		size;
	uint64		body;
	size_t		header_len;
	uint64		table_at;
	size_t		table_len;
	uint64		count;
	uint64		addresses_at;
	uint32		addresses_len;
	uint32		blocks;
	uint64		records;
	HostError	host;

	frame->ok = false;
	if (!host_open_to_read(frame->io, frame->path, frame->path_len, &frame->file, &host))
	{
		*frame->error = chunks_error_Error_FromError_from(&host);
		return;
	}
	frame->file_live = true;
	if (!host_size(frame->io, &frame->file, &size, &host))
	{
		*frame->error = chunks_error_Error_FromError_from(&host);
		chunks_pack_PackReader_open_pad_0(frame);
		return;
	}
	if (size < (uint64) (CHUNKS_PACK_FOOTER + sizeof(CHUNKS_PACK_MAGIC) + 16))
		goto corrupt;
	body = std_wrapping_sub_u64(size, CHUNKS_PACK_FOOTER);
	if (!chunks_file_read_at(frame->io, &frame->file, body, CHUNKS_PACK_FOOTER, &frame->footer, frame->error))
	{
		chunks_pack_PackReader_open_pad_0(frame);
		return;
	}
	frame->footer_live = true;
	if (memcmp(STD_PTR_AT(frame->footer.ptr, 48, 1), CHUNKS_PACK_MAGIC, sizeof(CHUNKS_PACK_MAGIC)) != 0)
		goto corrupt;
	header_len = (size_t) std_u32_from_le_bytes(STD_PTR_AT(frame->footer.ptr, 20, 1));
	if (header_len != CHUNKS_PACK_HEADER || (uint64) header_len > size)
		goto corrupt;
	if (!chunks_file_read_at(frame->io, &frame->file, 0, header_len, &frame->header, frame->error))
	{
		chunks_pack_PackReader_open_pad_0(frame);
		return;
	}
	frame->header_live = true;
	if (memcmp(frame->header.ptr, CHUNKS_PACK_MAGIC, sizeof(CHUNKS_PACK_MAGIC)) != 0 ||
		chunks_pack_checksum(frame->header.ptr, frame->header.len, frame->footer.ptr, frame->footer.len) !=
		std_u64_from_le_bytes(STD_PTR_AT(frame->footer.ptr, 40, 1)))
		goto corrupt;
	if (((const uint8 *) frame->header.ptr)[24] != CHUNKS_PACK_ZSTD)
		goto corrupt;
	table_at = std_u64_from_le_bytes(frame->footer.ptr);
	table_len = (size_t) std_u32_from_le_bytes(STD_PTR_AT(frame->footer.ptr, 8, 1));
	if (table_len == 0)
		std_vec_new(&frame->dictionaries, sizeof(chunks_pack_Slot), _Alignof(chunks_pack_Slot));
	else
	{
		if (table_at < (uint64) CHUNKS_PACK_HEADER || std_saturating_add_u64(table_at, (uint64) table_len) > body)
			goto corrupt;
		if (!chunks_file_read_at(frame->io, &frame->file, table_at, table_len, &frame->table, frame->error))
		{
			chunks_pack_PackReader_open_pad_0(frame);
			return;
		}
		frame->table_live = true;
		if (!chunks_pack_dictionary_table(frame->table.ptr, frame->table.len, table_at, &frame->dictionaries))
			goto corrupt;
		frame->table_live = false;
		std_vec_free_Checked(&frame->table, 1, 1);
	}
	frame->dictionaries_live = true;
	count = std_u64_from_le_bytes(STD_PTR_AT(frame->footer.ptr, 12, 1));
	addresses_at = std_u64_from_le_bytes(STD_PTR_AT(frame->footer.ptr, 24, 1));
	addresses_len = std_u32_from_le_bytes(STD_PTR_AT(frame->footer.ptr, 32, 1));
	blocks = std_u32_from_le_bytes(STD_PTR_AT(frame->footer.ptr, 36, 1));
	if (addresses_at < (uint64) CHUNKS_PACK_HEADER ||
		std_saturating_add_u64(addresses_at, (uint64) addresses_len) != body)
		goto corrupt;
	if (!std_checked_sub_u64(body, STD_ADD(u64, (uint64) STD_ADD(usize, header_len, table_len, 757, 23),
										   (uint64) addresses_len, 757, 23), &records))
		goto corrupt;
	memcpy(frame->result->id.f0, STD_PTR_AT(frame->header.ptr, 8, 1), sizeof(frame->result->id.f0));
	std_path_to_path_buf(frame->path, frame->path_len, &frame->result->path);
	frame->result->file = frame->file;
	frame->file_live = false;
	frame->result->dictionaries = frame->dictionaries;
	frame->dictionaries_live = false;
	frame->result->count = count;
	frame->result->records = records;
	frame->result->addresses.f0 = addresses_at;
	frame->result->addresses.f1 = addresses_len;
	frame->result->addresses.f2 = blocks;
	frame->ok = true;
	chunks_pack_PackReader_open_pad_0(frame);
	return;
corrupt:
	*frame->error = chunks_pack_corrupt_pack(frame->path, frame->path_len);
	chunks_pack_PackReader_open_pad_0(frame);
}

/* chunks_pack_PackReader_open opens a pack, checking its header and footer. */
bool
chunks_pack_PackReader_open(HostIoReadDoors io, const char *path, size_t path_len, ChunksPackPackReader *result,
							ChunksErrorError *error)
{
	chunks_pack_PackReader_open_frame_0_t frame = {.io = io, .path = path, .path_len = path_len, .result = result,
	.error = error};

	std_landing(&frame, chunks_pack_PackReader_open_body_0, chunks_pack_PackReader_open_pad_0);
	return frame.ok;
}

/* chunks_pack_PackReader_len returns the number of chunk records. */
uint64
chunks_pack_PackReader_len(const ChunksPackPackReader *self)
{
	return self->count;
}

/* chunks_pack_PackReader_is_empty reports whether the pack holds no chunks. */
bool
chunks_pack_PackReader_is_empty(const ChunksPackPackReader *self)
{
	return self->count == 0;
}

/* chunks_pack_PackReader_records_len returns the bytes of the pack's chunk records. */
uint64
chunks_pack_PackReader_records_len(const ChunksPackPackReader *self)
{
	return self->records;
}

/* chunks_pack_PackReader_has_dictionaries reports whether the pack holds any dictionary. */
bool
chunks_pack_PackReader_has_dictionaries(const ChunksPackPackReader *self)
{
	return self->dictionaries.len != 0;
}

/* chunks_pack_PrepareOnce is prepared's init of a slot's OnceLock: the dictionary it prepared, and whether the lock took it. */
typedef struct chunks_pack_PrepareOnce
{
	HostDecompressDictionary *prepared;
	bool		taken;
} chunks_pack_PrepareOnce;

/* chunks_pack_PackReader_prepared_closure_0 is prepared's init: the dictionary it prepared, moved into the slot. */
static void
chunks_pack_PackReader_prepared_closure_0(void *captures, void *result)
{
	chunks_pack_PrepareOnce *once = captures;

	*(HostDecompressDictionary *) result = *once->prepared;
	once->taken = true;
}

/* chunks_pack_damaged_frame_0_t is damaged's state across its pad: the message, until the error takes it. */
typedef struct chunks_pack_damaged_frame_0_t
{
	const StdPathBuf *path;
	StdFmtArguments *arguments;
	ChunksErrorError *result;
	bool		message_live;
} chunks_pack_damaged_frame_0_t;

/* chunks_pack_damaged_pad_0 is damaged's pad: the message, while live. */
static void
chunks_pack_damaged_pad_0(void *frame_)
{
	chunks_pack_damaged_frame_0_t *frame = frame_;

	if (frame->message_live)
	{
		frame->message_live = false;
		std_vec_free_Checked(&frame->result->u.damaged.message, 1, 1);
	}
}

/* chunks_pack_damaged_body_0 is damaged's body: the message formatted, then the path cloned. */
static void
chunks_pack_damaged_body_0(void *frame_)
{
	chunks_pack_damaged_frame_0_t *frame = frame_;

	std_fmt_format_Checked(&frame->result->u.damaged.message, frame->arguments);
	frame->message_live = true;
	std_PathBuf_Clone_clone(frame->path, &frame->result->u.damaged.path);
	frame->message_live = false;
}

/* chunks_pack_damaged returns the error of damage at an offset of a pack, its path and the message the arguments format. */
static ChunksErrorError
chunks_pack_damaged(const StdPathBuf *path, uint64 offset, StdFmtArguments *arguments)
{
	ChunksErrorError error = {.tag = CHUNKS_ERROR_ERROR_DAMAGED};
	chunks_pack_damaged_frame_0_t frame = {.path = path, .arguments = arguments, .result = &error};

	std_landing(&frame, chunks_pack_damaged_body_0, chunks_pack_damaged_pad_0);
	error.u.damaged.offset = offset;
	return error;
}

/* chunks_pack_PackReader_prepared_frame_0_t is prepared's state across its pad: the dictionary's bytes and prepared form, while live. */
typedef struct chunks_pack_PackReader_prepared_frame_0_t
{
	const ChunksPackPackReader *self;
	HostIoReadDoors io;
	size_t		slot;
	const HostDecompressDictionary **result;
	ChunksErrorError *error;
	bool		ok;
	StdVec		bytes;
	bool		bytes_live;
	HostDecompressDictionary prepared;
	bool		prepared_live;
} chunks_pack_PackReader_prepared_frame_0_t;

/* chunks_pack_PackReader_prepared_pad_0 is prepared's pad: what it holds, while live. */
static void
chunks_pack_PackReader_prepared_pad_0(void *frame_)
{
	chunks_pack_PackReader_prepared_frame_0_t *frame = frame_;

	if (frame->prepared_live)
	{
		frame->prepared_live = false;
		host_DecompressDictionary_drop(&frame->prepared);
	}
	if (frame->bytes_live)
	{
		frame->bytes_live = false;
		std_vec_free_Checked(&frame->bytes, 1, 1);
	}
}

/* chunks_pack_PackReader_prepared_body_0 is prepared's body: the slot's dictionary read, checked, and prepared once. */
static void
chunks_pack_PackReader_prepared_body_0(void *frame_)
{
	chunks_pack_PackReader_prepared_frame_0_t *frame = frame_;
	chunks_pack_Slot *entry;
	chunks_pack_PrepareOnce once;
	HostError	host;

	frame->ok = false;
	if (frame->slot >= frame->self->dictionaries.len)
	{
		*frame->result = NULL;
		frame->ok = true;
		return;
	}
	entry = &STD_VEC_AT(&frame->self->dictionaries, chunks_pack_Slot, frame->slot);
	if (std_once_lock_get(&entry->lock))
	{
		*frame->result = &entry->prepared;
		frame->ok = true;
		return;
	}
	if (!chunks_file_read_at(frame->io, &frame->self->file, entry->at, (size_t) entry->len, &frame->bytes,
							 frame->error))
		return;
	frame->bytes_live = true;
	if (XXH3_64bits(frame->bytes.ptr, frame->bytes.len) != entry->sum)
	{
		StdStr		shown = {(const char *) frame->self->path.bytes.ptr, frame->self->path.bytes.len};
		StdFmtPiece pieces[] = {STD_FMT_LIT("dictionary at offset "), STD_FMT_ARG(0), STD_FMT_LIT(" of pack \""),
		STD_FMT_ARG(1), STD_FMT_LIT("\" fails its checksum")};
		StdFmtArg	args[] = {{&entry->at, std_u64_Display_fmt}, {&shown, std_path_Display_fmt}};
		StdFmtArguments arguments = {pieces, 5, args};

		*frame->error = chunks_pack_damaged(&frame->self->path, entry->at, &arguments);
		chunks_pack_PackReader_prepared_pad_0(frame);
		return;
	}
	if (!host_prepare_decompressing(frame->bytes.ptr, frame->bytes.len, &frame->prepared, &host))
	{
		*frame->error = chunks_error_Error_FromError_from(&host);
		chunks_pack_PackReader_prepared_pad_0(frame);
		return;
	}
	frame->prepared_live = true;
	once.prepared = &frame->prepared;
	once.taken = false;
	std_once_lock_get_or_init(&entry->lock, &entry->prepared, chunks_pack_PackReader_prepared_closure_0, &once);
	if (once.taken)
		frame->prepared_live = false;
	*frame->result = &entry->prepared;
	frame->ok = true;
	chunks_pack_PackReader_prepared_pad_0(frame);
}

/*
 * chunks_pack_PackReader_prepared returns the pack's dictionary in a slot
 * prepared for decompressing, reading it the first time, or none for a slot
 * the pack lacks.
 */
static bool
chunks_pack_PackReader_prepared(const ChunksPackPackReader *self, HostIoReadDoors io, size_t slot,
								const HostDecompressDictionary **result, ChunksErrorError *error)
{
	chunks_pack_PackReader_prepared_frame_0_t frame = {.self = self, .io = io, .slot = slot, .result = result,
	.error = error};

	std_landing(&frame, chunks_pack_PackReader_prepared_body_0, chunks_pack_PackReader_prepared_pad_0);
	return frame.ok;
}

/* chunks_pack_PackReader_prepared_closure_1 is a read's dictionary of a slot: the pack's, prepared once. */
static bool
chunks_pack_PackReader_prepared_closure_1(void *captures, HostIoReadDoors io, size_t slot,
										  const HostDecompressDictionary **result, ChunksErrorError *error)
{
	return chunks_pack_PackReader_prepared(captures, io, slot, result, error);
}

/* chunks_pack_PackReader_read_frame_0_t is read's state across its pad: the record, while live. */
typedef struct chunks_pack_PackReader_read_frame_0_t
{
	const ChunksPackPackReader *self;
	HostIoReadDoors io;
	ChunksTrailerLocator locator;
	const ChunksAddressAddress *address;
	ChunksChunkChunk *result;
	ChunksErrorError *error;
	bool		ok;
	StdVec		record;
	bool		record_live;
	ChunksErrorError failed;
	bool		failed_live;
} chunks_pack_PackReader_read_frame_0_t;

/* chunks_pack_PackReader_read_pad_0 is read's pad: the record and the corruption it turns into damage, while live. */
static void
chunks_pack_PackReader_read_pad_0(void *frame_)
{
	chunks_pack_PackReader_read_frame_0_t *frame = frame_;

	if (frame->failed_live)
	{
		frame->failed_live = false;
		chunks_error_Error_drop(&frame->failed);
	}
	if (frame->record_live)
	{
		frame->record_live = false;
		std_vec_free_Checked(&frame->record, 1, 1);
	}
}

/*
 * chunks_pack_PackReader_read_body_0 is read's body: the record read and its
 * stored bytes checked, a corrupt record's error made damage at its offset.
 */
static void
chunks_pack_PackReader_read_body_0(void *frame_)
{
	chunks_pack_PackReader_read_frame_0_t *frame = frame_;
	uint64		offset = chunks_trailer_Locator_offset(&frame->locator);

	frame->ok = false;
	if (!chunks_file_read_at(frame->io, &frame->self->file, offset,
							 (size_t) chunks_trailer_Locator_length(&frame->locator), &frame->record, frame->error))
		return;
	frame->record_live = true;
	if (chunks_pack_stored_of(frame->io, frame->record.ptr, frame->record.len,
							  chunks_pack_PackReader_prepared_closure_1, (void *) frame->self, NULL, frame->address,
							  frame->result, &frame->failed))
		frame->ok = true;
	else if (frame->failed.tag == CHUNKS_ERROR_ERROR_CORRUPT)
	{
		StdStr		shown = {(const char *) frame->self->path.bytes.ptr, frame->self->path.bytes.len};
		StdStr		why;
		StdFmtPiece pieces[] = {STD_FMT_LIT("chunk at offset "), STD_FMT_ARG(0), STD_FMT_LIT(" of pack \""),
		STD_FMT_ARG(1), STD_FMT_LIT("\" is damaged: "), STD_FMT_ARG(2)};
		StdFmtArg	args[] = {{&offset, std_u64_Display_fmt}, {&shown, std_path_Display_fmt},
		{&why, std_str_Display_fmt}};
		StdFmtArguments arguments = {pieces, 6, args};

		frame->failed_live = true;
		why.ptr = (const char *) frame->failed.u.corrupt.ptr;
		why.len = frame->failed.u.corrupt.len;
		*frame->error = chunks_pack_damaged(&frame->self->path, offset, &arguments);
	}
	else
		*frame->error = frame->failed;
	chunks_pack_PackReader_read_pad_0(frame);
}

/*
 * chunks_pack_PackReader_read returns the stored bytes of the chunk at the
 * locator, after checking that its hashed bytes have the address, refusing a
 * record that doesn't decompress or hash to it as damage at its offset in
 * the pack.
 */
bool
chunks_pack_PackReader_read(const ChunksPackPackReader *self, HostIoReadDoors io, ChunksTrailerLocator locator,
							const ChunksAddressAddress *address, ChunksChunkChunk *result, ChunksErrorError *error)
{
	chunks_pack_PackReader_read_frame_0_t frame = {.self = self, .io = io, .locator = locator, .address = address,
	.result = result, .error = error};

	std_landing(&frame, chunks_pack_PackReader_read_body_0, chunks_pack_PackReader_read_pad_0);
	return frame.ok;
}

/* chunks_pack_PackReader_raw_frame_0_t is raw's state across its pad: the record and the chunk it checks, while live. */
typedef struct chunks_pack_PackReader_raw_frame_0_t
{
	const ChunksPackPackReader *self;
	HostIoIo   *io;
	ChunksTrailerLocator locator;
	const ChunksAddressAddress *address;
	ChunksPackRaw *result;
	ChunksErrorError *error;
	bool		ok;
	StdVec		record;
	bool		record_live;
	ChunksChunkChunk chunk;
	bool		chunk_live;
	chunks_pack_RawLayout raw;
} chunks_pack_PackReader_raw_frame_0_t;

/* chunks_pack_PackReader_raw_pad_0 is raw's pad: what it holds, while live. */
static void
chunks_pack_PackReader_raw_pad_0(void *frame_)
{
	chunks_pack_PackReader_raw_frame_0_t *frame = frame_;

	if (frame->chunk_live)
	{
		frame->chunk_live = false;
		chunks_chunk_Chunk_drop(&frame->chunk);
	}
	if (frame->record_live)
	{
		frame->record_live = false;
		std_vec_free_Checked(&frame->record, 1, 1);
	}
}

/* chunks_pack_PackReader_raw_body_0 is raw's body: the record read, its stored bytes checked, and split. */
static void
chunks_pack_PackReader_raw_body_0(void *frame_)
{
	chunks_pack_PackReader_raw_frame_0_t *frame = frame_;

	frame->ok = false;
	if (!chunks_file_read_at(HOST_IO_IO_READ_DOORS(frame->io), &frame->self->file,
							 chunks_trailer_Locator_offset(&frame->locator),
							 (size_t) chunks_trailer_Locator_length(&frame->locator), &frame->record, frame->error))
		return;
	frame->record_live = true;
	if (!chunks_pack_stored_of(HOST_IO_IO_READ_DOORS(frame->io), frame->record.ptr, frame->record.len,
							   chunks_pack_PackReader_prepared_closure_1, (void *) frame->self, NULL, frame->address,
							   &frame->chunk, frame->error))
	{
		chunks_pack_PackReader_raw_pad_0(frame);
		return;
	}
	frame->chunk_live = true;
	if (chunks_pack_raw_of(frame->record.ptr, frame->record.len, &frame->raw, frame->error))
	{
		*CHUNKS_PACK_RAW_LAYOUT(frame->result) = frame->raw;
		frame->ok = true;
	}
	chunks_pack_PackReader_raw_pad_0(frame);
}

/*
 * chunks_pack_PackReader_raw returns the record at the locator as the pack
 * stores it, for a copy that keeps its compressed bytes, after checking that
 * its stored bytes have the address.
 */
bool
chunks_pack_PackReader_raw(const ChunksPackPackReader *self, HostIoIo *io, ChunksTrailerLocator locator,
						   const ChunksAddressAddress *address, ChunksPackRaw *result, ChunksErrorError *error)
{
	chunks_pack_PackReader_raw_frame_0_t frame = {.self = self, .io = io, .locator = locator, .address = address,
	.result = result, .error = error};

	std_landing(&frame, chunks_pack_PackReader_raw_body_0, chunks_pack_PackReader_raw_pad_0);
	return frame.ok;
}

/* chunks_pack_PackReader_dictionary returns the bytes of the pack's dictionary in a slot, for a copy that carries it. */
bool
chunks_pack_PackReader_dictionary(const ChunksPackPackReader *self, HostIoIo *io, size_t slot, StdVec *result,
								  ChunksErrorError *error)
{
	const chunks_pack_Slot *entry;

	if (slot >= self->dictionaries.len)
	{
		*error = chunks_error_Error_corrupt_RefStr("a pack lacks a dictionary slot",
												   strlen("a pack lacks a dictionary slot"));
		return false;
	}
	entry = &STD_VEC_AT(&self->dictionaries, chunks_pack_Slot, slot);
	return chunks_file_read_at(HOST_IO_IO_READ_DOORS(io), &self->file, entry->at, (size_t) entry->len, result, error);
}

/* chunks_pack_PackReader_dictionaries checks every dictionary the pack keeps against its checksum, returning the damage of the first that fails. */
bool
chunks_pack_PackReader_dictionaries(const ChunksPackPackReader *self, HostIoIo *io, ChunksErrorError *error)
{
	for (size_t k = 0, k_len = self->dictionaries.len; k < k_len; k++)
	{
		const HostDecompressDictionary *prepared;

		if (!chunks_pack_PackReader_prepared(self, HOST_IO_IO_READ_DOORS(io), k, &prepared, error))
			return false;
	}
	return true;
}

/*
 * chunks_pack_PackReader_table returns the pack's address table, as a run
 * named by the pack's file, whose blocks each refuse their damage as they
 * are read.
 */
bool
chunks_pack_PackReader_table(const ChunksPackPackReader *self, HostIoReadDoors io, ChunksIndexRunLocator *result,
							 ChunksErrorError *error)
{
	StdVec		bytes;

	if (!chunks_file_read_at(io, &self->file, self->addresses.f0, (size_t) self->addresses.f1, &bytes, error) ||
		!chunks_index_RunLocator_decode(io, (const char *) self->path.bytes.ptr, self->path.bytes.len,
										self->addresses.f0, &bytes, result, error))
		return false;
	if (chunks_index_RunLocator_blocks(result) != (size_t) self->addresses.f2)
	{
		StdStr		shown = {(const char *) self->path.bytes.ptr, self->path.bytes.len};
		StdFmtPiece pieces[] = {STD_FMT_LIT("pack "), STD_FMT_ARG(0),
		STD_FMT_LIT(" has an address table of the wrong size")};
		StdFmtArg	args[] = {{&shown, std_path_Display_fmt}};
		StdFmtArguments arguments = {pieces, 3, args};
		StdString	message;

		chunks_index_RunLocator_drop(result);
		std_fmt_format_Checked(&message, &arguments);
		*error = chunks_error_Error_corrupt(&message);
		return false;
	}
	return true;
}

/* chunks_pack_PackReader_advise tells the operating system that the chunk at the locator will be read soon. */
void
chunks_pack_PackReader_advise(const ChunksPackPackReader *self, HostIoIo *io, ChunksTrailerLocator locator)
{
	chunks_file_advise(HOST_IO_IO_READ_DOORS(io), &self->file, chunks_trailer_Locator_offset(&locator),
					   (uint64) chunks_trailer_Locator_length(&locator));
}

/* chunks_pack_PackReader_drop_glue is PackReader's drop glue as a function value, which an Arc drops it through. */
static void
chunks_pack_PackReader_drop_glue(void *value)
{
	chunks_pack_PackReader_drop(value);
}

/* chunks_pack_ArcPackReader_new is Arc::<PackReader>::new: the reader moved into a new Arc. */
StdArcInner *
chunks_pack_ArcPackReader_new(ChunksPackPackReader *reader)
{
	return std_arc_new_dropping(reader, sizeof(*reader), _Alignof(ChunksPackPackReader),
								chunks_pack_PackReader_drop_glue);
}

/* chunks_pack_ArcPackReader_drop is Arc<PackReader>'s drop glue: the reader's, when the last reference goes. */
void
chunks_pack_ArcPackReader_drop(StdArcInner *self)
{
	std_arc_drop(self, sizeof(ChunksPackPackReader), _Alignof(ChunksPackPackReader), chunks_pack_PackReader_drop_glue);
}

/* chunks_pack_PackReader_drop is PackReader's drop glue: its file, its path, and its dictionaries. */
void
chunks_pack_PackReader_drop(ChunksPackPackReader *self)
{
	host_File_drop(&self->file);
	std_vec_free_Checked(&self->path.bytes, 1, 1);
	chunks_pack_Slots_drop(&self->dictionaries);
}

/* chunks_pack_Lost_drop is Lost's drop glue: its blocks and records. */
void
chunks_pack_Lost_drop(ChunksPackLost *self)
{
	std_vec_free_Checked(&self->blocks, sizeof(chunks_TupleAddressOptionAddressU64_t),
						 _Alignof(chunks_TupleAddressOptionAddressU64_t));
	std_vec_free_Checked(&self->records, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
}

/* chunks_pack_Lost_Default_default is Lost's Default: nothing lost. */
ChunksPackLost
chunks_pack_Lost_Default_default(void)
{
	ChunksPackLost lost;

	std_vec_new(&lost.blocks, sizeof(chunks_TupleAddressOptionAddressU64_t),
				_Alignof(chunks_TupleAddressOptionAddressU64_t));
	std_vec_new(&lost.records, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
	return lost;
}

/* chunks_pack_Lost_block_Debug_fmt is a lost block's Debug, (Address, Option<Address>, u64). */
static bool
chunks_pack_Lost_block_Debug_fmt(const void *value, StdFormatter *f)
{
	const chunks_TupleAddressOptionAddressU64_t *self = value;
	StdOptionRef next = {self->has_f1, {&self->f1, chunks_address_Address_Debug_fmt}};
	StdTupleRef tuple = {3, {{&self->f0, chunks_address_Address_Debug_fmt}, {&next, std_option_Debug_fmt},
	{&self->f2, std_u64_Debug_fmt}}};

	return std_tuple_Debug_fmt(&tuple, f);
}

/* chunks_pack_Lost_record_Debug_fmt is a lost record's Debug, (Address, Locator). */
static bool
chunks_pack_Lost_record_Debug_fmt(const void *value, StdFormatter *f)
{
	const chunks_TupleAddressLocator_t *self = value;
	StdTupleRef tuple = {2, {{&self->f0, chunks_address_Address_Debug_fmt}, {&self->f1, chunks_trailer_Locator_Debug_fmt}}};

	return std_tuple_Debug_fmt(&tuple, f);
}

/* chunks_pack_Lost_blocks_Debug_fmt is the lost blocks' Debug, a Vec's: a list of them. */
static bool
chunks_pack_Lost_blocks_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *self = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t k = 0, k_len = self->len; k < k_len; k++)
		std_DebugList_entry(&list, &STD_VEC_AT(self, chunks_TupleAddressOptionAddressU64_t, k),
							chunks_pack_Lost_block_Debug_fmt);
	return std_DebugList_finish(&list);
}

/* chunks_pack_Lost_records_Debug_fmt is the lost records' Debug, a Vec's: a list of them. */
static bool
chunks_pack_Lost_records_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *self = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t k = 0, k_len = self->len; k < k_len; k++)
		std_DebugList_entry(&list, &STD_VEC_AT(self, chunks_TupleAddressLocator_t, k),
							chunks_pack_Lost_record_Debug_fmt);
	return std_DebugList_finish(&list);
}

/* chunks_pack_Lost_Debug_fmt is Lost's derived Debug. */
bool
chunks_pack_Lost_Debug_fmt(const void *value, StdFormatter *f)
{
	const ChunksPackLost *self = value;
	StdDebugStruct s = std_fmt_debug_struct(f, "Lost", strlen("Lost"));

	std_DebugStruct_field(&s, "blocks", strlen("blocks"), &self->blocks, chunks_pack_Lost_blocks_Debug_fmt);
	std_DebugStruct_field(&s, "records", strlen("records"), &self->records, chunks_pack_Lost_records_Debug_fmt);
	return std_DebugStruct_finish(&s);
}

/* chunks_pack_Lost_PartialEq_eq is Lost's derived PartialEq: the same blocks and records, in order. */
bool
chunks_pack_Lost_PartialEq_eq(const ChunksPackLost *self, const ChunksPackLost *other)
{
	if (self->blocks.len != other->blocks.len || self->records.len != other->records.len)
		return false;
	for (size_t k = 0, k_len = self->blocks.len; k < k_len; k++)
	{
		const chunks_TupleAddressOptionAddressU64_t *a = &STD_VEC_AT(&self->blocks,
																	 chunks_TupleAddressOptionAddressU64_t, k);
		const chunks_TupleAddressOptionAddressU64_t *b = &STD_VEC_AT(&other->blocks,
																	 chunks_TupleAddressOptionAddressU64_t, k);

		if (!chunks_address_Address_PartialEq_eq(&a->f0, &b->f0) || a->has_f1 != b->has_f1 ||
			(a->has_f1 && !chunks_address_Address_PartialEq_eq(&a->f1, &b->f1)) || a->f2 != b->f2)
			return false;
	}
	for (size_t k = 0, k_len = self->records.len; k < k_len; k++)
	{
		const chunks_TupleAddressLocator_t *a = &STD_VEC_AT(&self->records, chunks_TupleAddressLocator_t, k);
		const chunks_TupleAddressLocator_t *b = &STD_VEC_AT(&other->records, chunks_TupleAddressLocator_t, k);

		if (!chunks_address_Address_PartialEq_eq(&a->f0, &b->f0) ||
			!chunks_trailer_Locator_PartialEq_eq(&a->f1, &b->f1))
			return false;
	}
	return true;
}

/* chunks_pack_Lost_message_frame_0_t is message's state across its pad: the message, while it builds it. */
typedef struct chunks_pack_Lost_message_frame_0_t
{
	const ChunksPackLost *self;
	const char *pack;
	size_t		pack_len;
	StdString  *result;
	bool		some;
	StdString	message;
	bool		message_live;
} chunks_pack_Lost_message_frame_0_t;

/* chunks_pack_Lost_message_pad_0 is message's pad: the message, while it builds it. */
static void
chunks_pack_Lost_message_pad_0(void *frame_)
{
	chunks_pack_Lost_message_frame_0_t *frame = frame_;

	if (frame->message_live)
	{
		frame->message_live = false;
		std_vec_free_Checked(&frame->message, 1, 1);
	}
}

/* chunks_pack_Lost_message_body_0 is message's body: the pack named, then what it lost, joined by commas. */
static void
chunks_pack_Lost_message_body_0(void *frame_)
{
	chunks_pack_Lost_message_frame_0_t *frame = frame_;
	const ChunksPackLost *self = frame->self;
	StdStr		shown = {frame->pack, frame->pack_len};
	bool		first = true;

	frame->some = self->blocks.len != 0 || self->records.len != 0;
	if (!frame->some)
		return;
	std_vec_new(&frame->message, 1, 1);
	frame->message_live = true;
	{
		StdFmtPiece pieces[] = {STD_FMT_LIT("pack \""), STD_FMT_ARG(0), STD_FMT_LIT("\" lost ")};
		StdFmtArg	args[] = {{&shown, std_path_Display_fmt}};
		StdFmtArguments arguments = {pieces, 3, args};

		(void) std_fmt_write(&STD_STRING_WRITE_OPS, &frame->message, &arguments);
	}
	for (size_t k = 0, k_len = self->blocks.len; k < k_len; k++)
	{
		const chunks_TupleAddressOptionAddressU64_t *block = &STD_VEC_AT(&self->blocks,
																		 chunks_TupleAddressOptionAddressU64_t, k);
		StdFmtPiece pieces[] = {STD_FMT_LIT(", "), STD_FMT_LIT("the address table block of addresses from "),
		STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&block->f0, chunks_address_Address_Display_fmt}};
		StdFmtArguments arguments = {first ? pieces + 1 : pieces, first ? 2 : 3, args};

		(void) std_fmt_write(&STD_STRING_WRITE_OPS, &frame->message, &arguments);
		first = false;
	}
	for (size_t k = 0, k_len = self->records.len; k < k_len; k++)
	{
		const chunks_TupleAddressLocator_t *record = &STD_VEC_AT(&self->records, chunks_TupleAddressLocator_t, k);
		uint64		offset = chunks_trailer_Locator_offset(&record->f1);
		StdFmtPiece pieces[] = {STD_FMT_LIT(", "), STD_FMT_LIT("chunk "), STD_FMT_ARG(0), STD_FMT_LIT(" at offset "),
		STD_FMT_ARG(1)};
		StdFmtArg	args[] = {{&record->f0, chunks_address_Address_Display_fmt}, {&offset, std_u64_Display_fmt}};
		StdFmtArguments arguments = {first ? pieces + 1 : pieces, first ? 4 : 5, args};

		(void) std_fmt_write(&STD_STRING_WRITE_OPS, &frame->message, &arguments);
		first = false;
	}
	*frame->result = frame->message;
	frame->message_live = false;
}

/*
 * chunks_pack_Lost_message returns what the walk of the pack's chunks lost,
 * naming the pack, each address table block that fails by where its entries'
 * addresses begin, and each chunk whose record doesn't hash to its address by
 * address and offset, or nothing when it lost nothing.
 */
bool
chunks_pack_Lost_message(const ChunksPackLost *self, const char *pack, size_t pack_len, StdString *result)
{
	chunks_pack_Lost_message_frame_0_t frame = {.self = self, .pack = pack, .pack_len = pack_len, .result = result};

	std_landing(&frame, chunks_pack_Lost_message_body_0, chunks_pack_Lost_message_pad_0);
	return frame.some;
}

/* chunks_pack_Lost_offset returns the offset of the first chunk the walk lost, or 0 when it lost only address table blocks. */
uint64
chunks_pack_Lost_offset(const ChunksPackLost *self)
{
	if (self->records.len == 0)
		return 0;
	return chunks_trailer_Locator_offset(&STD_VEC_AT(&self->records, chunks_TupleAddressLocator_t, 0).f1);
}

/* chunks_pack_Verified_address returns the chunk's address. */
ChunksAddressAddress
chunks_pack_Verified_address(const ChunksPackVerified *self)
{
	return self->address;
}

/* chunks_pack_Verified_locator returns where the chunk's record lies, under the pack's number in the table. */
ChunksTrailerLocator
chunks_pack_Verified_locator(const ChunksPackVerified *self)
{
	return self->locator;
}

/* chunks_pack_Verified_slot returns the slot of the dictionary the record was compressed with, if any. */
bool
chunks_pack_Verified_slot(const ChunksPackVerified *self, size_t *result)
{
	*result = self->slot;
	return self->has_slot;
}

/* chunks_pack_Verified_stored returns the chunk's stored bytes. */
const uint8 *
chunks_pack_Verified_stored(const ChunksPackVerified *self, size_t *result_len)
{
	*result_len = self->stored.len;
	return self->stored.ptr;
}

/* chunks_pack_Verified_split reads the chunk's trailer, as Stored::split reads it. */
bool
chunks_pack_Verified_split(const ChunksPackVerified *self,
						   bool (*addresses) (void *captures, const uint8 *hashed, size_t hashed_len, size_t *result,
											  ChunksErrorError *error),
						   void *addresses_captures, void (*addresses_drop) (void *), ChunksTrailerStored *result,
						   ChunksErrorError *error)
{
	return chunks_trailer_Stored_split(self->stored.ptr, self->stored.len, addresses, addresses_captures,
									   addresses_drop, result, error);
}

/* chunks_pack_PackReader_verified_closure_0 is verified's sort of the table's entries: by their locators' offsets. */
static StdOrdering
chunks_pack_PackReader_verified_closure_0(void *captures, const void *a, const void *b)
{
	uint64		at_a = chunks_trailer_Locator_offset(&((const chunks_TupleAddressLocator_t *) a)->f1);
	uint64		at_b = chunks_trailer_Locator_offset(&((const chunks_TupleAddressLocator_t *) b)->f1);

	return at_a < at_b ? STD_ORDERING_LESS : at_a == at_b ? STD_ORDERING_EQUAL : STD_ORDERING_GREATER;
}

/* chunks_pack_PackReader_verified_frame_0_t is verified's state across its pad: what it holds, each while live. */
typedef struct chunks_pack_PackReader_verified_frame_0_t
{
	const ChunksPackPackReader *self;
	ChunksFileInterruptible io;
	uint64		number;
	bool		(*f) (void *captures, ChunksFileInterruptible io, ChunksPackVerified *verified,
					  ChunksErrorError *error);
	void	   *f_captures;
	void		(*f_drop) (void *captures);
	bool		f_live;
	ChunksPackLost *result;
	ChunksErrorError *error;
	bool		ok;
	ChunksIndexRunLocator table;
	bool		table_live;
	ChunksPackLost lost;
	bool		lost_live;
	StdVec		entries;
	bool		entries_live;
	StdVec		block;
	bool		block_live;
	ChunksErrorError failed;
	bool		failed_live;
	StdVec		record;
	bool		record_live;
	ChunksPackVerified verified;
	bool		verified_live;
} chunks_pack_PackReader_verified_frame_0_t;

/* chunks_pack_PackReader_verified_pad_0 is verified's pad: what it holds, while live. */
static void
chunks_pack_PackReader_verified_pad_0(void *frame_)
{
	chunks_pack_PackReader_verified_frame_0_t *frame = frame_;

	if (frame->verified_live)
	{
		frame->verified_live = false;
		std_vec_free_Checked(&frame->verified.stored, 1, 1);
	}
	if (frame->record_live)
	{
		frame->record_live = false;
		std_vec_free_Checked(&frame->record, 1, 1);
	}
	if (frame->failed_live)
	{
		frame->failed_live = false;
		chunks_error_Error_drop(&frame->failed);
	}
	if (frame->block_live)
	{
		frame->block_live = false;
		std_vec_free_Checked(&frame->block, sizeof(chunks_TupleAddressLocator_t),
							 _Alignof(chunks_TupleAddressLocator_t));
	}
	if (frame->entries_live)
	{
		frame->entries_live = false;
		std_vec_free_Checked(&frame->entries, sizeof(chunks_TupleAddressLocator_t),
							 _Alignof(chunks_TupleAddressLocator_t));
	}
	if (frame->lost_live)
	{
		frame->lost_live = false;
		chunks_pack_Lost_drop(&frame->lost);
	}
	if (frame->table_live)
	{
		frame->table_live = false;
		chunks_index_RunLocator_drop(&frame->table);
	}
	if (frame->f_live)
	{
		frame->f_live = false;
		if (frame->f_drop != NULL)
			frame->f_drop(frame->f_captures);
	}
}

/*
 * chunks_pack_PackReader_verified_body_0 is verified's body: the table's
 * blocks read, their entries walked in pack order, and each record that
 * hashes to its address handed to the function.
 */
static void
chunks_pack_PackReader_verified_body_0(void *frame_)
{
	chunks_pack_PackReader_verified_frame_0_t *frame = frame_;
	const ChunksPackPackReader *self = frame->self;
	HostIoReadDoors doors = {&frame->io.ops->read_doors, frame->io.self};
	size_t		blocks;
	uint64		end;
	HostError	host;

	frame->ok = false;
	if (!chunks_pack_PackReader_table(self, doors, &frame->table, frame->error))
	{
		chunks_pack_PackReader_verified_pad_0(frame);
		return;
	}
	frame->table_live = true;
	frame->lost = chunks_pack_Lost_Default_default();
	frame->lost_live = true;
	std_vec_with_capacity_Checked(&frame->entries, (size_t) chunks_index_RunLocator_len(&frame->table),
								  sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
	frame->entries_live = true;
	blocks = chunks_index_RunLocator_blocks(&frame->table);
	for (size_t i = 0, i_end = blocks; i < i_end; i = std_wrapping_add_usize(i, 1))
	{
		if (chunks_index_RunLocator_block(&frame->table, doors, i, &frame->block, &frame->failed))
		{
			frame->block_live = true;
			std_vec_extend_from_slice_Checked(&frame->entries, frame->block.ptr, frame->block.len,
											  sizeof(chunks_TupleAddressLocator_t),
											  _Alignof(chunks_TupleAddressLocator_t));
			frame->block_live = false;
			std_vec_free_Checked(&frame->block, sizeof(chunks_TupleAddressLocator_t),
								 _Alignof(chunks_TupleAddressLocator_t));
		}
		else if (frame->failed.tag == CHUNKS_ERROR_ERROR_DAMAGED)
		{
			chunks_TupleAddressOptionAddressU64_t lost = {.f0 = chunks_index_RunLocator_fence(&frame->table, i),
			.has_f1 = std_wrapping_add_usize(i, 1) < blocks, .f2 = frame->failed.u.damaged.offset};

			frame->failed_live = true;
			if (lost.has_f1)
				lost.f1 = chunks_index_RunLocator_fence(&frame->table, std_wrapping_add_usize(i, 1));
			std_vec_push_Checked(&frame->lost.blocks, &lost, sizeof(lost),
								 _Alignof(chunks_TupleAddressOptionAddressU64_t));
			frame->failed_live = false;
			chunks_error_Error_drop(&frame->failed);
		}
		else
		{
			*frame->error = frame->failed;
			chunks_pack_PackReader_verified_pad_0(frame);
			return;
		}
	}
	std_slice_sort_by_PackTupleAddressLocator(frame->entries.ptr, frame->entries.len,
											  chunks_pack_PackReader_verified_closure_0, NULL, NULL);
	end = STD_ADD(u64, (uint64) CHUNKS_PACK_HEADER, self->records, 885, 19);
	for (size_t k = 0, k_len = frame->entries.len; k < k_len; k++)
	{
		const chunks_TupleAddressLocator_t *entry = &STD_VEC_AT(&frame->entries, chunks_TupleAddressLocator_t, k);
		ChunksAddressAddress address = entry->f0;
		ChunksTrailerLocator locator;
		uint64		offset;
		uint32		length;
		chunks_TupleAddressLocator_t lost;
		ChunksAddressAddress hashed;
		bool		read;

		if (!frame->io.ops->check_interrupt(frame->io.self, &host))
		{
			*frame->error = chunks_error_Error_FromError_from(&host);
			chunks_pack_PackReader_verified_pad_0(frame);
			return;
		}
		locator = chunks_trailer_Locator_at(frame->number, chunks_trailer_Locator_offset(&entry->f1),
											chunks_trailer_Locator_length(&entry->f1));
		offset = chunks_trailer_Locator_offset(&locator);
		length = chunks_trailer_Locator_length(&locator);
		lost.f0 = address;
		lost.f1 = locator;
		if (offset < (uint64) CHUNKS_PACK_HEADER || std_saturating_add_u64(offset, (uint64) length) > end)
		{
			std_vec_push_Checked(&frame->lost.records, &lost, sizeof(lost), _Alignof(chunks_TupleAddressLocator_t));
			continue;
		}
		if (!chunks_file_read_at(doors, &self->file, offset, (size_t) length, &frame->record, frame->error))
		{
			chunks_pack_PackReader_verified_pad_0(frame);
			return;
		}
		frame->record_live = true;
		frame->verified.has_slot = false;
		if (frame->record.len != 0 && ((const uint8 *) frame->record.ptr)[0] == CHUNKS_PACK_WITH_DICTIONARY)
		{
			size_t		at = 0;
			uint64		slot;
			ChunksVarintMalformed malformed;

			if (chunks_varint_decode(STD_PTR_AT(frame->record.ptr, 1, 1), std_wrapping_sub_usize(frame->record.len, 1),
									 &at, &slot, &malformed))
			{
				frame->verified.has_slot = true;
				frame->verified.slot = (size_t) slot;
			}
		}
		read = chunks_pack_decompress(doors, frame->record.ptr, frame->record.len,
									  chunks_pack_PackReader_prepared_closure_1, (void *) self, NULL,
									  &frame->verified.stored, &frame->failed);
		if (read)
		{
			frame->verified_live = true;
			read = chunks_pack_address_of(frame->verified.stored.ptr, frame->verified.stored.len, &hashed,
										  &frame->failed);
		}
		if (read && chunks_address_Address_PartialEq_eq(&hashed, &address))
		{
			frame->verified.address = address;
			frame->verified.locator = locator;
			if (!frame->f(frame->f_captures, frame->io, &frame->verified, frame->error))
			{
				chunks_pack_PackReader_verified_pad_0(frame);
				return;
			}
		}
		else if (read)
			std_vec_push_Checked(&frame->lost.records, &lost, sizeof(lost), _Alignof(chunks_TupleAddressLocator_t));
		else
		{
			ChunksErrorOwn own;

			frame->failed_live = true;
			if (!chunks_error_Error_own(&frame->failed, &own, frame->error))
			{
				frame->failed_live = false;
				chunks_pack_PackReader_verified_pad_0(frame);
				return;
			}
			frame->failed_live = false;
			chunks_error_Own_drop(&own);
			std_vec_push_Checked(&frame->lost.records, &lost, sizeof(lost), _Alignof(chunks_TupleAddressLocator_t));
		}
		if (frame->verified_live)
		{
			frame->verified_live = false;
			std_vec_free_Checked(&frame->verified.stored, 1, 1);
		}
		frame->record_live = false;
		std_vec_free_Checked(&frame->record, 1, 1);
	}
	*frame->result = frame->lost;
	frame->lost_live = false;
	frame->ok = true;
	chunks_pack_PackReader_verified_pad_0(frame);
}

/*
 * chunks_pack_PackReader_verified calls the function with each chunk the
 * pack's address table names whose record hashes to its address, under its
 * number in the pack table, in pack order, and returns what it lost: the
 * address table's blocks that fail their checksums and the chunks whose
 * records don't read whole or don't hash to their addresses. It takes the
 * function.
 */
bool
chunks_pack_PackReader_verified(const ChunksPackPackReader *self, ChunksFileInterruptible io, uint64 number,
								bool (*f) (void *captures, ChunksFileInterruptible io, ChunksPackVerified *verified,
										   ChunksErrorError *error),
								void *f_captures, void (*f_drop) (void *), ChunksPackLost *result,
								ChunksErrorError *error)
{
	chunks_pack_PackReader_verified_frame_0_t frame = {.self = self, .io = io, .number = number, .f = f,
		.f_captures = f_captures, .f_drop = f_drop, .f_live = true, .result = result,
	.error = error};

	std_landing(&frame, chunks_pack_PackReader_verified_body_0, chunks_pack_PackReader_verified_pad_0);
	return frame.ok;
}

#ifdef DOLTGRES_TEST_CHUNKS

/* CHUNKS_PACK_TESTS_IO_INTERRUPTIBLE coerces an Io to the Interruptible it implements. */
#define CHUNKS_PACK_TESTS_IO_INTERRUPTIBLE(io) ((ChunksFileInterruptible) {&CHUNKS_FILE_IO_INTERRUPTIBLE_OPS, (io)})

/* CHUNKS_PACK_TESTS_UNWRAP is Result::unwrap's panic message. */
#define CHUNKS_PACK_TESTS_UNWRAP "called `Result::unwrap()` on an `Err` value"

static void chunks_pack_tests_read_all(const ChunksPackPackReader *reader, HostIoIo *io, const StdVec *written,
									   ChunksErrorError *error, uint32 unwrap_line, uint32 unwrap_col, uint32 line,
									   uint32 col);

/* chunks_pack_tests_noise is the tests' xorshift: the next byte of a state, which it moves. */
static uint8
chunks_pack_tests_noise(uint64 *state)
{
	*state ^= STD_SHL_FOLDED(u64, *state, 13);
	*state ^= STD_SHR_FOLDED(u64, *state, 7);
	*state ^= STD_SHL_FOLDED(u64, *state, 17);
	return (uint8) *state;
}

/* chunks_TupleAddressVecU8_t is the tuple (Address, Vec<u8>): a chunk's address and its stored bytes. */
typedef struct chunks_TupleAddressVecU8_t
{
	ChunksAddressAddress f0;
	StdVec		f1;
} chunks_TupleAddressVecU8_t;

/* chunks_TupleAddressVecU8Locator_t is the tuple (Address, Vec<u8>, Locator): a chunk written, and where. */
typedef struct chunks_TupleAddressVecU8Locator_t
{
	ChunksAddressAddress f0;
	StdVec		f1;
	ChunksTrailerLocator f2;
} chunks_TupleAddressVecU8Locator_t;

/* chunks_pack_tests_Written_drop is a written chunk's drop glue: its stored bytes'. */
static void
chunks_pack_tests_Written_drop(void *value)
{
	chunks_TupleAddressVecU8Locator_t *self = value;

	std_vec_free_Checked(&self->f1, 1, 1);
}

/* chunks_pack_tests_VecWritten_drop is Vec<(Address, Vec<u8>, Locator)>'s drop glue. */
static void
chunks_pack_tests_VecWritten_drop(StdVec *self)
{
	std_vec_truncate(self, 0, sizeof(chunks_TupleAddressVecU8Locator_t), chunks_pack_tests_Written_drop);
	std_vec_free_Checked(self, sizeof(chunks_TupleAddressVecU8Locator_t),
						 _Alignof(chunks_TupleAddressVecU8Locator_t));
}

/* chunks_pack_tests_leaf_frame_0_t is leaf's state across its pad: the body and the hashed bytes, while live. */
typedef struct chunks_pack_tests_leaf_frame_0_t
{
	const ChunksTrailerStamps *stamps;
	chunks_TupleAddressVecU8_t *result;
	StdVec		body;
	bool		body_live;
	StdVec		hashed;
	bool		hashed_live;
} chunks_pack_tests_leaf_frame_0_t;

/* chunks_pack_tests_leaf_pad_0 is leaf's pad: what it holds, while live. */
static void
chunks_pack_tests_leaf_pad_0(void *frame_)
{
	chunks_pack_tests_leaf_frame_0_t *frame = frame_;

	if (frame->hashed_live)
	{
		frame->hashed_live = false;
		std_vec_free_Checked(&frame->hashed, 1, 1);
	}
	if (frame->body_live)
	{
		frame->body_live = false;
		std_vec_free_Checked(&frame->body, 1, 1);
	}
}

/* chunks_pack_tests_leaf_body_0 is leaf's body: the body under a leaf's header, stored with the stamps. */
static void
chunks_pack_tests_leaf_body_0(void *frame_)
{
	chunks_pack_tests_leaf_frame_0_t *frame = frame_;
	ChunksHeaderHeader header = {.kind = CHUNKS_HEADER_TREE_LEAF, .flags = 0,
	.addresses = {.tag = CHUNKS_HEADER_ADDRESSES_OFFSETS}};

	std_vec_new(&header.addresses.u.offsets, sizeof(uint32), _Alignof(uint32));
	chunks_header_Header_write(&header, frame->body.ptr, frame->body.len, &frame->hashed);
	frame->hashed_live = true;
	frame->result->f0 = chunks_address_Address_of(frame->hashed.ptr, frame->hashed.len);
	chunks_trailer_store(frame->hashed.ptr, frame->hashed.len, NULL, 0, frame->stamps, &frame->result->f1);
	chunks_pack_tests_leaf_pad_0(frame);
}

/*
 * chunks_pack_tests_leaf returns a leaf's address and stored bytes, of a
 * body it takes, the leaf's header's and the trailer's of its stamps, as
 * chunk and pad make them.
 */
static void
chunks_pack_tests_leaf(StdVec *body, const ChunksTrailerStamps *stamps, chunks_TupleAddressVecU8_t *result)
{
	chunks_pack_tests_leaf_frame_0_t frame = {.stamps = stamps, .result = result, .body = *body, .body_live = true};

	std_landing(&frame, chunks_pack_tests_leaf_body_0, chunks_pack_tests_leaf_pad_0);
}

/* chunks_pack_tests_chunk_frame_0_t is chunk's state across its pad: the stamps, while live. */
typedef struct chunks_pack_tests_chunk_frame_0_t
{
	uint32		i;
	chunks_TupleAddressVecU8_t *result;
	ChunksTrailerStamps stamps;
	bool		stamps_live;
} chunks_pack_tests_chunk_frame_0_t;

/* chunks_pack_tests_chunk_pad_0 is chunk's pad: the stamps, while live. */
static void
chunks_pack_tests_chunk_pad_0(void *frame_)
{
	chunks_pack_tests_chunk_frame_0_t *frame = frame_;

	if (frame->stamps_live)
	{
		frame->stamps_live = false;
		chunks_trailer_Stamps_drop(&frame->stamps);
	}
}

/* chunks_pack_tests_chunk_body_0 is chunk's body: a leaf of a body of the number's letters, stamped by it. */
static void
chunks_pack_tests_chunk_body_0(void *frame_)
{
	chunks_pack_tests_chunk_frame_0_t *frame = frame_;
	uint32		i = frame->i;
	size_t		len = (size_t) STD_ADD(u32, 1100, STD_REM_FOLDED(u32, i, 300), 988, 33);
	StdVec		single;
	StdVec		body;
	ChunksTrailerStamp stamp = {.txn = (uint64) i, .tuple = 1};

	std_vec_with_capacity_Checked(&single, 1, sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
	std_vec_push_Checked(&single, &stamp, sizeof(stamp), _Alignof(ChunksTrailerStamp));
	frame->stamps = chunks_trailer_Stamps_FromVecStamp_from(&single);
	frame->stamps_live = true;
	std_vec_with_capacity_Checked(&body, len, 1, 1);
	for (uint32 j = 0, j_end = (uint32) len; j < j_end; j = std_forward_unchecked_u32_Checked(j, 1))
	{
		uint8		letter = (uint8) STD_ADD(u32, STD_MUL(u32, j, 7, 988, 58), i, 988, 57);

		std_vec_u8_push_Checked(&body, STD_ADD(u8, STD_REM_FOLDED(u8, letter, 13), (uint8) 'a', 988, 57));
	}
	chunks_pack_tests_leaf(&body, &frame->stamps, frame->result);
	chunks_pack_tests_chunk_pad_0(frame);
}

/* chunks_pack_tests_chunk returns a numbered leaf's address and stored bytes, its body the number's letters. */
static void
chunks_pack_tests_chunk(uint32 i, chunks_TupleAddressVecU8_t *result)
{
	chunks_pack_tests_chunk_frame_0_t frame = {.i = i, .result = result};

	std_landing(&frame, chunks_pack_tests_chunk_body_0, chunks_pack_tests_chunk_pad_0);
}

/* chunks_pack_tests_pad_frame_0_t is pad's state across its pad: the chunks written, and the one in hand, while live. */
typedef struct chunks_pack_tests_pad_frame_0_t
{
	HostIoIo   *io;
	ChunksPackPackWriterInDir *writer;
	StdVec	   *result;
	StdVec		written;
	bool		written_live;
	chunks_TupleAddressVecU8_t chunk;
	bool		chunk_live;
	StdVec		copy;
	bool		copy_live;
	ChunksErrorError error;
} chunks_pack_tests_pad_frame_0_t;

/* chunks_pack_tests_pad_pad_0 is pad's pad: what it holds, while live. */
static void
chunks_pack_tests_pad_pad_0(void *frame_)
{
	chunks_pack_tests_pad_frame_0_t *frame = frame_;

	if (frame->copy_live)
	{
		frame->copy_live = false;
		std_vec_free_Checked(&frame->copy, 1, 1);
	}
	if (frame->chunk_live)
	{
		frame->chunk_live = false;
		std_vec_free_Checked(&frame->chunk.f1, 1, 1);
	}
	if (frame->written_live)
	{
		frame->written_live = false;
		chunks_pack_tests_VecWritten_drop(&frame->written);
	}
}

/*
 * chunks_pack_tests_pad_body_0 is pad's body: incompressible chunks that are
 * not leaves, a MiB each, written until the pack holds TRAIN_AFTER bytes.
 */
static void
chunks_pack_tests_pad_body_0(void *frame_)
{
	chunks_pack_tests_pad_frame_0_t *frame = frame_;
	uint64		state = UINT64CONST(0x9e3779b97f4a7c15);
	ChunksTrailerStamps stamps = chunks_trailer_Stamps_Default_default();

	std_vec_new(&frame->written, sizeof(chunks_TupleAddressVecU8Locator_t),
				_Alignof(chunks_TupleAddressVecU8Locator_t));
	frame->written_live = true;
	while (chunks_pack_PackWriterInDir_size(frame->writer) < CHUNKS_PACK_TRAIN_AFTER)
	{
		StdVec		body;
		chunks_TupleAddressVecU8Locator_t entry;

		std_vec_with_capacity_Checked(&body, (size_t) 1 << 20, 1, 1);
		for (size_t k = 0, k_len = (size_t) 1 << 20; k < k_len; k++)
			std_vec_u8_push_Checked(&body, chunks_pack_tests_noise(&state));
		chunks_pack_tests_leaf(&body, &stamps, &frame->chunk);
		frame->chunk_live = true;
		std_slice_to_vec_Checked(&frame->copy, frame->chunk.f1.ptr, frame->chunk.f1.len, 1, 1);
		frame->copy_live = true;
		if (!chunks_pack_PackWriterInDir_write(frame->writer, frame->io, frame->chunk.f0, frame->chunk.f1.ptr,
											   frame->chunk.f1.len, false, &entry.f2, &frame->error))
			STD_RESULT_EXPECT_FAILED(1010, 94, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
									 chunks_error_Error_Debug_fmt);
		entry.f0 = frame->chunk.f0;
		entry.f1 = frame->copy;
		std_vec_push_Checked(&frame->written, &entry, sizeof(entry), _Alignof(chunks_TupleAddressVecU8Locator_t));
		frame->copy_live = false;
		frame->chunk_live = false;
		std_vec_free_Checked(&frame->chunk.f1, 1, 1);
	}
	*frame->result = frame->written;
	frame->written_live = false;
}

/*
 * chunks_pack_tests_pad writes incompressible chunks that are not leaves
 * until the pack holds TRAIN_AFTER bytes, so that a pack that trains trains
 * on its next leaves, and returns them as written.
 */
static void
chunks_pack_tests_pad(HostIoIo *io, ChunksPackPackWriterInDir *writer, StdVec *result)
{
	chunks_pack_tests_pad_frame_0_t frame = {.io = io, .writer = writer, .result = result};

	std_landing(&frame, chunks_pack_tests_pad_body_0, chunks_pack_tests_pad_pad_0);
}

/*
 * The state of a_write_past_what_a_locator_holds_is_refused across its pad:
 * its capability, directory, writer, noise, and an unwrapped error.
 */
typedef struct test_chunks_pack_tests_a_write_past_what_a_locator_holds_is_refused_frame_0_t
{
	SimcTestIo	test_io;
	bool		test_io_live;
	SimcTempDir dir;
	bool		dir_live;
	ChunksPackPackWriterInDir writer;
	bool		writer_live;
	StdVec		long_;
	bool		long_live;
	ChunksErrorError error;
	bool		error_live;
} test_chunks_pack_tests_a_write_past_what_a_locator_holds_is_refused_frame_0_t;

/* The test's pad. */
static void
test_chunks_pack_tests_a_write_past_what_a_locator_holds_is_refused_pad_0(void *frame_)
{
	test_chunks_pack_tests_a_write_past_what_a_locator_holds_is_refused_frame_0_t *frame = frame_;

	if (frame->error_live)
		chunks_error_Error_drop(&frame->error);
	if (frame->long_live)
		std_vec_free_Checked(&frame->long_, 1, 1);
	if (frame->writer_live)
		chunks_pack_PackWriterInDir_drop(&frame->writer);
	if (frame->dir_live)
		simc_TempDir_drop(&frame->dir);
	if (frame->test_io_live)
		simc_TestIo_drop(&frame->test_io);
}

/* The test's body. */
static void
test_chunks_pack_tests_a_write_past_what_a_locator_holds_is_refused_body_0(void *frame_)
{
	test_chunks_pack_tests_a_write_past_what_a_locator_holds_is_refused_frame_0_t *frame = frame_;
	HostIoIo   *io;
	StdIoError	io_error;
	const char *dir_path;
	size_t		dir_path_len;
	uint64		state = UINT64CONST(0x9e3779b97f4a7c15);
	ChunksAddressAddress address = chunks_address_Address_of((const uint8 *) "long", 4);
	ChunksTrailerLocator locator;
	size_t		length;
	uint64		offset;
	uint64		max_offset = CHUNKS_PACK_MAX_OFFSET;
	size_t		max_stored = CHUNKS_PACK_MAX_STORED;

	frame->test_io = simc_io();
	frame->test_io_live = true;
	io = &frame->test_io.io;
	if (!simc_tempdir(&frame->dir, &io_error))
		STD_RESULT_EXPECT_FAILED(1021, 34, CHUNKS_PACK_TESTS_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	frame->dir_live = true;
	dir_path = simc_TempDir_path(&frame->dir, &dir_path_len);
	if (!chunks_pack_PackWriterInDir_create(io, dir_path, dir_path_len, 7, &frame->writer, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1022, 64, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->writer_live = true;
	std_vec_with_capacity_Checked(&frame->long_, CHUNKS_PACK_MAX_STORED, 1, 1);
	frame->long_live = true;
	for (size_t k = 0, k_len = CHUNKS_PACK_MAX_STORED; k < k_len; k++)
		std_vec_u8_push_Checked(&frame->long_, chunks_pack_tests_noise(&state));
	if (chunks_pack_PackWriterInDir_write(&frame->writer, io, address, frame->long_.ptr, frame->long_.len, false,
										  &locator, &frame->error))
		std_panic(RUST_FILE, 1031, 9,
				  "assertion failed: matches!(writer.write(io, address, &long, false), Err(Error::Corrupt(_)))",
				  strlen("assertion failed: matches!(writer.write(io, address, &long, false), Err(Error::Corrupt(_)))"));
	frame->error_live = true;
	if (frame->error.tag != CHUNKS_ERROR_ERROR_CORRUPT)
		std_panic(RUST_FILE, 1031, 9,
				  "assertion failed: matches!(writer.write(io, address, &long, false), Err(Error::Corrupt(_)))",
				  strlen("assertion failed: matches!(writer.write(io, address, &long, false), Err(Error::Corrupt(_)))"));
	frame->error_live = false;
	chunks_error_Error_drop(&frame->error);
	std_range_from_check(1, frame->long_.len, RUST_FILE, 1032, 51);
	if (!chunks_pack_PackWriterInDir_write(&frame->writer, io, address, STD_PTR_AT(frame->long_.ptr, 1, 1),
										   std_wrapping_sub_usize(frame->long_.len, 1), false, &locator,
										   &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1032, 65, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	length = (size_t) chunks_trailer_Locator_length(&locator);
	STD_ASSERT_EQ(1032, 9, length == max_stored, &length, std_usize_Debug_fmt, &max_stored,
				  std_usize_Debug_fmt);
	frame->writer.flushed = STD_SUB(u64, CHUNKS_PACK_MAX_OFFSET, (uint64) frame->writer.buffer.len, 1033, 26);
	if (!chunks_pack_PackWriterInDir_write(&frame->writer, io, address, (const uint8 *) "last", 4, false, &locator,
										   &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1034, 62, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	offset = chunks_trailer_Locator_offset(&locator);
	STD_ASSERT_EQ(1034, 9, offset == max_offset, &offset, std_u64_Debug_fmt, &max_offset,
				  std_u64_Debug_fmt);
	if (chunks_pack_PackWriterInDir_write(&frame->writer, io, address, (const uint8 *) "past", 4, false, &locator,
										  &frame->error))
		std_panic(RUST_FILE, 1035, 9,
				  "assertion failed: matches!(writer.write(io, address, b\"past\", false), Err(Error::Invariant(_)))",
				  strlen("assertion failed: matches!(writer.write(io, address, b\"past\", false), Err(Error::Invariant(_)))"));
	frame->error_live = true;
	if (frame->error.tag != CHUNKS_ERROR_ERROR_INVARIANT)
		std_panic(RUST_FILE, 1035, 9,
				  "assertion failed: matches!(writer.write(io, address, b\"past\", false), Err(Error::Invariant(_)))",
				  strlen("assertion failed: matches!(writer.write(io, address, b\"past\", false), Err(Error::Invariant(_)))"));
	frame->error_live = false;
	chunks_error_Error_drop(&frame->error);
	frame->writer_live = false;
	if (!chunks_pack_PackWriterInDir_abandon(&frame->writer, HOST_IO_IO_TAKES_SHORT(io), &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1036, 28, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->long_live = false;
	std_vec_free_Checked(&frame->long_, 1, 1);
	frame->dir_live = false;
	simc_TempDir_drop(&frame->dir);
	frame->test_io_live = false;
	simc_TestIo_drop(&frame->test_io);
}

/*
 * test_chunks_pack_tests_a_write_past_what_a_locator_holds_is_refused: a
 * locator holds no length past 2^24 - 1 and no offset past 2^40 - 1, so a
 * write of a longer chunk, and a write to a pack that large, are refused,
 * and the largest of each is taken.
 */
void
test_chunks_pack_tests_a_write_past_what_a_locator_holds_is_refused(void)
{
	test_chunks_pack_tests_a_write_past_what_a_locator_holds_is_refused_frame_0_t frame = {0};

	std_landing(&frame, test_chunks_pack_tests_a_write_past_what_a_locator_holds_is_refused_body_0,
				test_chunks_pack_tests_a_write_past_what_a_locator_holds_is_refused_pad_0);
}


/* chunks_pack_tests_keep_all is a run lookup's keep that keeps every value, the test's `|_| true`. */
static bool
chunks_pack_tests_keep_all(void *captures, const ChunksTrailerLocator *value)
{
	return true;
}

/*
 * test_chunks_pack_tests_writes_and_reads_chunks_closure_0_t is the
 * verified walk's captures: the chunks written, the tiny chunk's locator,
 * address, and stored bytes, and the count seen.
 */
typedef struct test_chunks_pack_tests_writes_and_reads_chunks_closure_0_t
{
	const StdVec *written;
	ChunksTrailerLocator tiny;
	ChunksAddressAddress address;
	const StdVec *stored;
	size_t		seen;
} test_chunks_pack_tests_writes_and_reads_chunks_closure_0_t;

/* The verified walk's step: each chunk is the next written, in order, then the tiny one. */
static bool
test_chunks_pack_tests_writes_and_reads_chunks_closure_0(void *captures, ChunksFileInterruptible io,
														 ChunksPackVerified *chunk, ChunksErrorError *error)
{
	test_chunks_pack_tests_writes_and_reads_chunks_closure_0_t *walk = captures;
	const chunks_TupleAddressVecU8Locator_t *w = walk->seen < walk->written->len ?
		&STD_VEC_AT(walk->written, chunks_TupleAddressVecU8Locator_t, walk->seen) : NULL;
	ChunksTrailerLocator locator = chunks_pack_Verified_locator(chunk);
	ChunksTrailerLocator want_locator = w != NULL ? w->f2 : walk->tiny;
	ChunksAddressAddress address = chunks_pack_Verified_address(chunk);
	ChunksAddressAddress want_address = w != NULL ? w->f0 : walk->address;
	StdBytes	stored;
	StdBytes	want_stored = w != NULL ? (StdBytes) {w->f1.ptr, w->f1.len} :
	(StdBytes) {walk->stored->ptr, walk->stored->len};

	STD_ASSERT_EQ(1068, 17, chunks_trailer_Locator_PartialEq_eq(&locator, &want_locator), &locator,
				  chunks_trailer_Locator_Debug_fmt, &want_locator, chunks_trailer_Locator_Debug_fmt);
	STD_ASSERT_EQ(1069, 17, chunks_address_Address_PartialEq_eq(&address, &want_address), &address,
				  chunks_address_Address_Debug_fmt, &want_address, chunks_address_Address_Debug_fmt);
	stored.ptr = chunks_pack_Verified_stored(chunk, &stored.len);
	STD_ASSERT_EQ(1070, 17, stored.len == want_stored.len && memcmp(stored.ptr, want_stored.ptr, stored.len) == 0,
				  &stored, std_bytes_Debug_fmt, &want_stored, std_bytes_Debug_fmt);
	walk->seen = STD_ADD(usize, walk->seen, 1, 1071, 17);
	return true;
}

/*
 * The state of writes_and_reads_chunks across its pad: its capability,
 * directory, writer, chunks, pack, reader, a chunk read back, the file's
 * bytes, what the walk lost, and an unwrapped error.
 */
typedef struct test_chunks_pack_tests_writes_and_reads_chunks_frame_0_t
{
	SimcTestIo	test_io;
	bool		test_io_live;
	SimcTempDir dir;
	bool		dir_live;
	ChunksPackPackWriterInDir writer;
	bool		writer_live;
	StdVec		written;
	bool		written_live;
	chunks_TupleAddressVecU8_t chunk;
	bool		chunk_live;
	StdVec		stored;
	bool		stored_live;
	ChunksPackPack pack;
	bool		pack_live;
	ChunksPackPackReader reader;
	bool		reader_live;
	ChunksChunkChunk read;
	bool		read_live;
	StdVec		bytes;
	bool		bytes_live;
	ChunksPackLost lost;
	bool		lost_live;
	ChunksErrorError error;
	bool		error_live;
} test_chunks_pack_tests_writes_and_reads_chunks_frame_0_t;

/* The test's pad. */
static void
test_chunks_pack_tests_writes_and_reads_chunks_pad_0(void *frame_)
{
	test_chunks_pack_tests_writes_and_reads_chunks_frame_0_t *frame = frame_;

	if (frame->error_live)
		chunks_error_Error_drop(&frame->error);
	if (frame->lost_live)
		chunks_pack_Lost_drop(&frame->lost);
	if (frame->bytes_live)
		std_vec_free_Checked(&frame->bytes, 1, 1);
	if (frame->read_live)
		chunks_chunk_Chunk_drop(&frame->read);
	if (frame->reader_live)
		chunks_pack_PackReader_drop(&frame->reader);
	if (frame->pack_live)
		chunks_pack_Pack_drop(&frame->pack);
	if (frame->stored_live)
		std_vec_free_Checked(&frame->stored, 1, 1);
	if (frame->chunk_live)
		std_vec_free_Checked(&frame->chunk.f1, 1, 1);
	if (frame->written_live)
		chunks_pack_tests_VecWritten_drop(&frame->written);
	if (frame->writer_live)
		chunks_pack_PackWriterInDir_drop(&frame->writer);
	if (frame->dir_live)
		simc_TempDir_drop(&frame->dir);
	if (frame->test_io_live)
		simc_TestIo_drop(&frame->test_io);
}

/* The test's body. */
static void
test_chunks_pack_tests_writes_and_reads_chunks_body_0(void *frame_)
{
	test_chunks_pack_tests_writes_and_reads_chunks_frame_0_t *frame = frame_;
	HostIoIo   *io;
	StdIoError	io_error;
	const char *dir_path;
	size_t		dir_path_len;
	size_t		dictionaries;
	size_t		one = 1;
	ChunksAddressAddress address = chunks_address_Address_of((const uint8 *) "tiny", 4);
	ChunksAddressAddress tinx = chunks_address_Address_of((const uint8 *) "tinx", 4);
	ChunksTrailerStamps stamps = chunks_trailer_Stamps_Default_default();
	ChunksTrailerLocator tiny;
	uint64		len;
	uint64		want_len;
	test_chunks_pack_tests_writes_and_reads_chunks_closure_0_t walk;
	ChunksPackLost none;
	size_t		want_seen;
	bool		wrote;

	frame->test_io = simc_io();
	frame->test_io_live = true;
	io = &frame->test_io.io;
	if (!simc_tempdir(&frame->dir, &io_error))
		STD_RESULT_EXPECT_FAILED(1043, 34, CHUNKS_PACK_TESTS_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	frame->dir_live = true;
	dir_path = simc_TempDir_path(&frame->dir, &dir_path_len);
	if (!chunks_pack_PackWriterInDir_create(io, dir_path, dir_path_len, 7, &frame->writer, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1044, 64, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->writer_live = true;
	chunks_pack_PackWriterInDir_train(&frame->writer);
	chunks_pack_tests_pad(io, &frame->writer, &frame->written);
	frame->written_live = true;
	for (uint32 i = 0, i_end = 3000; i < i_end; i = std_forward_unchecked_u32_Checked(i, 1))
	{
		chunks_TupleAddressVecU8Locator_t entry;

		chunks_pack_tests_chunk(i, &frame->chunk);
		frame->chunk_live = true;
		if (!chunks_pack_PackWriterInDir_write(&frame->writer, io, frame->chunk.f0, frame->chunk.f1.ptr,
											   frame->chunk.f1.len, STD_REM_FOLDED(u32, i, 2) == 0, &entry.f2,
											   &frame->error))
		{
			frame->error_live = true;
			STD_RESULT_EXPECT_FAILED(1049, 74, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
									 chunks_error_Error_Debug_fmt);
		}
		entry.f0 = frame->chunk.f0;
		entry.f1 = frame->chunk.f1;
		std_vec_push_Checked(&frame->written, &entry, sizeof(entry), _Alignof(chunks_TupleAddressVecU8Locator_t));
		frame->chunk_live = false;
	}
	dictionaries = frame->writer.dictionaries.len;
	STD_ASSERT_EQ(1052, 9, dictionaries == one, &dictionaries, std_usize_Debug_fmt, &one,
				  std_usize_Debug_fmt);
	chunks_trailer_store((const uint8 *) "tiny", 4, NULL, 0, &stamps, &frame->stored);
	frame->stored_live = true;
	if (!chunks_pack_PackWriterInDir_write(&frame->writer, io, address, frame->stored.ptr, frame->stored.len, false,
										   &tiny, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1054, 62, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->writer_live = false;
	if (!chunks_pack_PackWriterInDir_finish(&frame->writer, HOST_IO_IO_TAKES_SHORT(io), &frame->pack, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1055, 38, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->pack_live = true;
	if (!chunks_pack_PackReader_open(HOST_IO_IO_READ_DOORS(io), (const char *) frame->pack.path.bytes.ptr,
									 frame->pack.path.bytes.len, &frame->reader, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1056, 55, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->reader_live = true;
	len = chunks_pack_PackReader_len(&frame->reader);
	want_len = STD_ADD(u64, (uint64) frame->written.len, 1, 1057, 34);
	STD_ASSERT_EQ(1057, 9, len == want_len, &len, std_u64_Debug_fmt, &want_len, std_u64_Debug_fmt);
	STD_ASSERT_EQ(1058, 9, chunks_pack_PackId_PartialEq_eq(&frame->reader.id, &frame->pack.id),
				  &frame->reader.id, chunks_pack_PackId_Debug_fmt, &frame->pack.id, chunks_pack_PackId_Debug_fmt);
	for (size_t k = 0, k_len = frame->written.len; k < k_len; k++)
	{
		const chunks_TupleAddressVecU8Locator_t *w = &STD_VEC_AT(&frame->written, chunks_TupleAddressVecU8Locator_t,
																 k);
		uint64		pack = w->f2.pack;
		uint64		seven = 7;
		StdBytes	read_bytes;
		StdBytes	want = {w->f1.ptr, w->f1.len};
		bool		has;
		ChunksTrailerLocator found;
		StdVec		bytes;

		STD_ASSERT_EQ(1060, 13, pack == seven, &pack, std_u64_Debug_fmt, &seven, std_u64_Debug_fmt);
		if (!chunks_pack_PackReader_read(&frame->reader, HOST_IO_IO_READ_DOORS(io), w->f2, &w->f0, &frame->read,
										 &frame->error))
		{
			frame->error_live = true;
			STD_RESULT_EXPECT_FAILED(1061, 60, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
									 chunks_error_Error_Debug_fmt);
		}
		chunks_chunk_Chunk_into_bytes(&frame->read, &bytes);
		frame->bytes = bytes;
		frame->bytes_live = true;
		read_bytes.ptr = frame->bytes.ptr;
		read_bytes.len = frame->bytes.len;
		STD_ASSERT_EQ(1061, 13, read_bytes.len == want.len && memcmp(read_bytes.ptr, want.ptr, want.len) == 0,
					  &read_bytes, std_bytes_Debug_fmt, &want, std_bytes_Debug_fmt);
		frame->bytes_live = false;
		std_vec_free_Checked(&frame->bytes, 1, 1);
		if (!chunks_index_RunLocator_get(&frame->pack.run, HOST_IO_IO_READ_DOORS(io), &w->f0, chunks_pack_tests_keep_all,
										 NULL, NULL, &has, &found, &frame->error))
		{
			frame->error_live = true;
			STD_RESULT_EXPECT_FAILED(1062, 60, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
									 chunks_error_Error_Debug_fmt);
		}
		{
			StdOptionRef got = {has, STD_ITEM(&found, chunks_trailer_Locator_Debug_fmt)};
			StdOptionRef want_found = {true, STD_ITEM(&w->f2, chunks_trailer_Locator_Debug_fmt)};

			STD_ASSERT_EQ(1062, 13, has && chunks_trailer_Locator_PartialEq_eq(&found, &w->f2), &got,
						  std_option_Debug_fmt, &want_found, std_option_Debug_fmt);
		}
	}
	if (chunks_pack_PackReader_read(&frame->reader, HOST_IO_IO_READ_DOORS(io), tiny, &tinx, &frame->read,
									&frame->error))
	{
		frame->read_live = true;
		std_panic(RUST_FILE, 1064, 9, "assertion failed: reader.read(io, tiny, &Address::of(b\"tinx\")).is_err()",
				  strlen("assertion failed: reader.read(io, tiny, &Address::of(b\"tinx\")).is_err()"));
	}
	chunks_error_Error_drop(&frame->error);
	walk.written = &frame->written;
	walk.tiny = tiny;
	walk.address = address;
	walk.stored = &frame->stored;
	walk.seen = 0;
	if (!chunks_pack_PackReader_verified(&frame->reader, CHUNKS_PACK_TESTS_IO_INTERRUPTIBLE(io), 7,
										 test_chunks_pack_tests_writes_and_reads_chunks_closure_0, &walk, NULL,
										 &frame->lost, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1074, 14, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->lost_live = true;
	none = chunks_pack_Lost_Default_default();
	STD_ASSERT_EQ(1075, 9, chunks_pack_Lost_PartialEq_eq(&frame->lost, &none), &frame->lost,
				  chunks_pack_Lost_Debug_fmt, &none, chunks_pack_Lost_Debug_fmt);
	chunks_pack_Lost_drop(&none);
	want_seen = STD_ADD(usize, frame->written.len, 1, 1076, 26);
	STD_ASSERT_EQ(1076, 9, walk.seen == want_seen, &walk.seen, std_usize_Debug_fmt, &want_seen,
				  std_usize_Debug_fmt);
	if (!std_fs_read((const char *) frame->pack.path.bytes.ptr, frame->pack.path.bytes.len, &frame->bytes, &io_error))
		STD_RESULT_EXPECT_FAILED(1077, 51, CHUNKS_PACK_TESTS_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	frame->bytes_live = true;
	std_index_check(9, frame->bytes.len, RUST_FILE, 1078, 9);
	((uint8 *) frame->bytes.ptr)[9] ^= 1;
	wrote = std_fs_write((const char *) frame->pack.path.bytes.ptr, frame->pack.path.bytes.len, frame->bytes.ptr,
						 frame->bytes.len, &io_error);
	if (!wrote)
		STD_RESULT_EXPECT_FAILED(1079, 44, CHUNKS_PACK_TESTS_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	frame->reader_live = false;
	chunks_pack_PackReader_drop(&frame->reader);
	if (chunks_pack_PackReader_open(HOST_IO_IO_READ_DOORS(io), (const char *) frame->pack.path.bytes.ptr,
									frame->pack.path.bytes.len, &frame->reader, &frame->error))
	{
		frame->reader_live = true;
		std_panic(RUST_FILE, 1080, 9, "assertion failed: PackReader::open(io, &pack.path).is_err()",
				  strlen("assertion failed: PackReader::open(io, &pack.path).is_err()"));
	}
	chunks_error_Error_drop(&frame->error);
	frame->bytes_live = false;
	std_vec_free_Checked(&frame->bytes, 1, 1);
	frame->lost_live = false;
	chunks_pack_Lost_drop(&frame->lost);
	frame->pack_live = false;
	chunks_pack_Pack_drop(&frame->pack);
	frame->stored_live = false;
	std_vec_free_Checked(&frame->stored, 1, 1);
	frame->written_live = false;
	chunks_pack_tests_VecWritten_drop(&frame->written);
	frame->dir_live = false;
	simc_TempDir_drop(&frame->dir);
	frame->test_io_live = false;
	simc_TestIo_drop(&frame->test_io);
}

/* test_chunks_pack_tests_writes_and_reads_chunks: a pack's chunks read back, by locator and through its run and its walk. */
void
test_chunks_pack_tests_writes_and_reads_chunks(void)
{
	test_chunks_pack_tests_writes_and_reads_chunks_frame_0_t frame = {0};

	std_landing(&frame, test_chunks_pack_tests_writes_and_reads_chunks_body_0,
				test_chunks_pack_tests_writes_and_reads_chunks_pad_0);
}

/* BTreeSet<u64>, which dead.c defines, for a deletion vector's offsets the tests write. */
#define STD_BTREE_SCOPE extern
#define STD_BTREE_DECLARE
#define STD_BTREE_KS U64
#define STD_BTREE_K uint64
#define STD_BTREE_VS Set
#define STD_BTREE_V StdBTreeSetValZST
#define STD_BTREE_V_ZERO_SIZED
#define STD_BTREE_ITERATE
#define STD_BTREE_SET
#define STD_BTREE_DROP
#define STD_BTREE_BULK
#define STD_BTREE_KEY_NE(a, b) (*(a) != *(b))
#define STD_BTREE_LAYOUT U64_SET
#define STD_BTREE_CMP(a, b) std_u64_Ord_cmp((a), (b))
#define STD_BTREE_KEY_DROP(k) ((void) (k))
#define STD_BTREE_VAL_DROP(v) ((void) (v))
#define STD_BTREE_VAL_DEFAULT(v) ((void) (v))
#define STD_BTREE_TABLES
#define STD_BTREE_TABLE_ARGS
#include "doltgres/std/btree_template.h"

/*
 * The state of a_packs_reader_reads_the_packs_deletion_vector across its
 * pad: its capability, directory, writer, chunk, pack, offsets, reader,
 * vector, paths, and an unwrapped error.
 */
typedef struct test_chunks_pack_tests_a_packs_reader_reads_the_packs_deletion_vector_frame_0_t
{
	SimcTestIo	test_io;
	bool		test_io_live;
	SimcTempDir dir;
	bool		dir_live;
	ChunksPackPackWriterInDir writer;
	bool		writer_live;
	chunks_TupleAddressVecU8_t chunk;
	bool		chunk_live;
	ChunksPackPack pack;
	bool		pack_live;
	StdBTreeMap offsets;
	bool		offsets_live;
	ChunksPackPackReader reader;
	bool		reader_live;
	ChunksDeadDead vector;
	bool		vector_live;
	StdPathBuf	path;
	bool		path_live;
	StdString	name;
	bool		name_live;
	StdPathBuf	joined;
	bool		joined_live;
	ChunksErrorError error;
	bool		error_live;
} test_chunks_pack_tests_a_packs_reader_reads_the_packs_deletion_vector_frame_0_t;

/* The test's pad. */
static void
test_chunks_pack_tests_a_packs_reader_reads_the_packs_deletion_vector_pad_0(void *frame_)
{
	test_chunks_pack_tests_a_packs_reader_reads_the_packs_deletion_vector_frame_0_t *frame = frame_;

	if (frame->error_live)
		chunks_error_Error_drop(&frame->error);
	if (frame->joined_live)
		std_vec_free_Checked(&frame->joined.bytes, 1, 1);
	if (frame->name_live)
		std_vec_free_Checked(&frame->name, 1, 1);
	if (frame->path_live)
		std_vec_free_Checked(&frame->path.bytes, 1, 1);
	if (frame->vector_live)
		chunks_dead_Dead_drop(&frame->vector);
	if (frame->reader_live)
		chunks_pack_PackReader_drop(&frame->reader);
	if (frame->offsets_live)
		std_btree_BTreeMapU64Set_drop(&frame->offsets);
	if (frame->pack_live)
		chunks_pack_Pack_drop(&frame->pack);
	if (frame->chunk_live)
		std_vec_free_Checked(&frame->chunk.f1, 1, 1);
	if (frame->writer_live)
		chunks_pack_PackWriterInDir_drop(&frame->writer);
	if (frame->dir_live)
		simc_TempDir_drop(&frame->dir);
	if (frame->test_io_live)
		simc_TestIo_drop(&frame->test_io);
}

/* test_chunks_pack_tests_a_packs_reader_reads_the_packs_deletion_vector_closure_0 is the test's write: a numbered chunk's offset. */
static uint64
test_chunks_pack_tests_a_packs_reader_reads_the_packs_deletion_vector_closure_0(test_chunks_pack_tests_a_packs_reader_reads_the_packs_deletion_vector_frame_0_t *frame,
																				uint32 i)
{
	ChunksTrailerLocator locator;

	chunks_pack_tests_chunk(i, &frame->chunk);
	frame->chunk_live = true;
	if (!chunks_pack_PackWriterInDir_write(&frame->writer, &frame->test_io.io, frame->chunk.f0, frame->chunk.f1.ptr,
										   frame->chunk.f1.len, false, &locator, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1093, 55, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->chunk_live = false;
	std_vec_free_Checked(&frame->chunk.f1, 1, 1);
	return chunks_trailer_Locator_offset(&locator);
}

/* The test's body. */
static void
test_chunks_pack_tests_a_packs_reader_reads_the_packs_deletion_vector_body_0(void *frame_)
{
	test_chunks_pack_tests_a_packs_reader_reads_the_packs_deletion_vector_frame_0_t *frame = frame_;
	HostIoIo   *io;
	StdIoError	io_error;
	const char *dir_path;
	size_t		dir_path_len;
	uint64		dead;
	uint64		live;
	uint64		arr[1];
	ChunksDeadDead missing;

	frame->test_io = simc_io();
	frame->test_io_live = true;
	io = &frame->test_io.io;
	if (!simc_tempdir(&frame->dir, &io_error))
		STD_RESULT_EXPECT_FAILED(1089, 34, CHUNKS_PACK_TESTS_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	frame->dir_live = true;
	dir_path = simc_TempDir_path(&frame->dir, &dir_path_len);
	if (!chunks_pack_PackWriterInDir_create(io, dir_path, dir_path_len, 7, &frame->writer, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1090, 64, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->writer_live = true;
	dead = test_chunks_pack_tests_a_packs_reader_reads_the_packs_deletion_vector_closure_0(frame, 1);
	live = test_chunks_pack_tests_a_packs_reader_reads_the_packs_deletion_vector_closure_0(frame, 2);
	frame->writer_live = false;
	if (!chunks_pack_PackWriterInDir_finish(&frame->writer, HOST_IO_IO_TAKES_SHORT(io), &frame->pack, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1096, 38, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->pack_live = true;
	arr[0] = dead;
	frame->offsets = std_btree_BTreeSetU64_FromArray_from(arr, 1);
	frame->offsets_live = true;
	if (!chunks_dead_Dead_write(HOST_IO_IO_IO_DOORS(io), (const char *) frame->pack.path.bytes.ptr,
								frame->pack.path.bytes.len, 2, &frame->offsets, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1098, 61, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	if (!chunks_pack_PackReader_open(HOST_IO_IO_READ_DOORS(io), (const char *) frame->pack.path.bytes.ptr,
									 frame->pack.path.bytes.len, &frame->reader, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1099, 55, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->reader_live = true;
	if (!chunks_pack_PackReader_dead(&frame->reader, HOST_IO_IO_READ_DOORS(io), 2, &frame->vector, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1100, 46, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->vector_live = true;
	STD_ASSERT(1101, 9, chunks_dead_Dead_holds(&frame->vector, dead) && !chunks_dead_Dead_holds(&frame->vector, live),
			   "vector.holds(dead) && !vector.holds(live)");
	frame->path = chunks_dead_Dead_path((const char *) frame->pack.path.bytes.ptr, frame->pack.path.bytes.len, 2);
	frame->path_live = true;
	frame->name = chunks_pack_PackId_file_name(&frame->pack.id, "2.dead", 6);
	frame->name_live = true;
	std_path_join(dir_path, dir_path_len, (const char *) frame->name.ptr, frame->name.len, &frame->joined);
	frame->joined_live = true;
	STD_ASSERT_EQ(1102, 9, std_path_PartialEq_eq((const char *) frame->path.bytes.ptr, frame->path.bytes.len,
												 (const char *) frame->joined.bytes.ptr,
												 frame->joined.bytes.len),
				  &frame->path, std_PathBuf_Debug_fmt, &frame->joined, std_PathBuf_Debug_fmt);
	if (chunks_pack_PackReader_dead(&frame->reader, HOST_IO_IO_READ_DOORS(io), 3, &missing, &frame->error))
	{
		chunks_dead_Dead_drop(&missing);
		std_panic(RUST_FILE, 1103, 9,
				  "assertion failed: matches!(reader.dead(io, third), Err(Error::Damaged { .. }))",
				  strlen("assertion failed: matches!(reader.dead(io, third), Err(Error::Damaged { .. }))"));
	}
	frame->error_live = true;
	if (frame->error.tag != CHUNKS_ERROR_ERROR_DAMAGED)
		std_panic(RUST_FILE, 1103, 9,
				  "assertion failed: matches!(reader.dead(io, third), Err(Error::Damaged { .. }))",
				  strlen("assertion failed: matches!(reader.dead(io, third), Err(Error::Damaged { .. }))"));
	test_chunks_pack_tests_a_packs_reader_reads_the_packs_deletion_vector_pad_0(frame);
}

/*
 * test_chunks_pack_tests_a_packs_reader_reads_the_packs_deletion_vector: a
 * pack's reader reads the generation of the pack's deletion vector it is
 * asked for, from beside the pack, and fails for a generation no collection
 * wrote.
 */
void
test_chunks_pack_tests_a_packs_reader_reads_the_packs_deletion_vector(void)
{
	test_chunks_pack_tests_a_packs_reader_reads_the_packs_deletion_vector_frame_0_t frame = {0};

	std_landing(&frame, test_chunks_pack_tests_a_packs_reader_reads_the_packs_deletion_vector_body_0,
				test_chunks_pack_tests_a_packs_reader_reads_the_packs_deletion_vector_pad_0);
}

/*
 * The state of refuses_a_footer_with_a_short_header across its pad: its
 * capability, directory, writer, chunk, pack, the file's bytes, and an
 * unwrapped error.
 */
typedef struct test_chunks_pack_tests_refuses_a_footer_with_a_short_header_frame_0_t
{
	SimcTestIo	test_io;
	bool		test_io_live;
	SimcTempDir dir;
	bool		dir_live;
	ChunksPackPackWriterInDir writer;
	bool		writer_live;
	chunks_TupleAddressVecU8_t chunk;
	bool		chunk_live;
	ChunksPackPack pack;
	bool		pack_live;
	StdVec		bytes;
	bool		bytes_live;
	ChunksErrorError error;
	bool		error_live;
} test_chunks_pack_tests_refuses_a_footer_with_a_short_header_frame_0_t;

/* The test's pad. */
static void
test_chunks_pack_tests_refuses_a_footer_with_a_short_header_pad_0(void *frame_)
{
	test_chunks_pack_tests_refuses_a_footer_with_a_short_header_frame_0_t *frame = frame_;

	if (frame->error_live)
		chunks_error_Error_drop(&frame->error);
	if (frame->bytes_live)
		std_vec_free_Checked(&frame->bytes, 1, 1);
	if (frame->pack_live)
		chunks_pack_Pack_drop(&frame->pack);
	if (frame->chunk_live)
		std_vec_free_Checked(&frame->chunk.f1, 1, 1);
	if (frame->writer_live)
		chunks_pack_PackWriterInDir_drop(&frame->writer);
	if (frame->dir_live)
		simc_TempDir_drop(&frame->dir);
	if (frame->test_io_live)
		simc_TestIo_drop(&frame->test_io);
}

/* The test's body. */
static void
test_chunks_pack_tests_refuses_a_footer_with_a_short_header_body_0(void *frame_)
{
	test_chunks_pack_tests_refuses_a_footer_with_a_short_header_frame_0_t *frame = frame_;
	HostIoIo   *io;
	StdIoError	io_error;
	const char *dir_path;
	size_t		dir_path_len;
	ChunksTrailerLocator locator;
	size_t		at;
	static const uint32 lens[] = {0, 8, 24};

	frame->test_io = simc_io();
	frame->test_io_live = true;
	io = &frame->test_io.io;
	if (!simc_tempdir(&frame->dir, &io_error))
		STD_RESULT_EXPECT_FAILED(1147, 34, CHUNKS_PACK_TESTS_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	frame->dir_live = true;
	dir_path = simc_TempDir_path(&frame->dir, &dir_path_len);
	if (!chunks_pack_PackWriterInDir_create(io, dir_path, dir_path_len, 7, &frame->writer, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1148, 64, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->writer_live = true;
	chunks_pack_tests_chunk(1, &frame->chunk);
	frame->chunk_live = true;
	if (!chunks_pack_PackWriterInDir_write(&frame->writer, io, frame->chunk.f0, frame->chunk.f1.ptr,
										   frame->chunk.f1.len, false, &locator, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1150, 51, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->writer_live = false;
	if (!chunks_pack_PackWriterInDir_finish(&frame->writer, HOST_IO_IO_TAKES_SHORT(io), &frame->pack, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1151, 38, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->pack_live = true;
	if (!std_fs_read((const char *) frame->pack.path.bytes.ptr, frame->pack.path.bytes.len, &frame->bytes, &io_error))
		STD_RESULT_EXPECT_FAILED(1152, 51, CHUNKS_PACK_TESTS_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	frame->bytes_live = true;
	at = STD_ADD(usize, STD_SUB(usize, frame->bytes.len, CHUNKS_PACK_FOOTER, 1153, 18), 20,
				 1153, 18);
	for (size_t k = 0, k_len = lengthof(lens); k < k_len; k++)
	{
		uint8		le[4];
		size_t		end = STD_ADD(usize, at, 4, 1155, 23);
		ChunksPackPackReader reader;

		std_u32_to_le_bytes(lens[k], le);
		std_range_check(at, end, frame->bytes.len, RUST_FILE, 1155, 18);
		std_slice_copy_from_slice_Checked(STD_PTR_AT(frame->bytes.ptr, at, 1), 4, le, 4, 1, 1, RUST_FILE,
										  1155, 31);
		if (!std_fs_write((const char *) frame->pack.path.bytes.ptr, frame->pack.path.bytes.len, frame->bytes.ptr,
						  frame->bytes.len, &io_error))
			STD_RESULT_EXPECT_FAILED(1156, 48, CHUNKS_PACK_TESTS_UNWRAP, &io_error, std_io_Error_Debug_fmt);
		if (chunks_pack_PackReader_open(HOST_IO_IO_READ_DOORS(io), (const char *) frame->pack.path.bytes.ptr,
										frame->pack.path.bytes.len, &reader, &frame->error))
		{
			chunks_pack_PackReader_drop(&reader);
			std_panic(RUST_FILE, 1157, 13,
					  "assertion failed: matches!(PackReader::open(io, &pack.path), Err(Error::Corrupt(_)))",
					  strlen("assertion failed: matches!(PackReader::open(io, &pack.path), Err(Error::Corrupt(_)))"));
		}
		frame->error_live = true;
		if (frame->error.tag != CHUNKS_ERROR_ERROR_CORRUPT)
			std_panic(RUST_FILE, 1157, 13,
					  "assertion failed: matches!(PackReader::open(io, &pack.path), Err(Error::Corrupt(_)))",
					  strlen("assertion failed: matches!(PackReader::open(io, &pack.path), Err(Error::Corrupt(_)))"));
		frame->error_live = false;
		chunks_error_Error_drop(&frame->error);
	}
	test_chunks_pack_tests_refuses_a_footer_with_a_short_header_pad_0(frame);
}

/* test_chunks_pack_tests_refuses_a_footer_with_a_short_header: a footer naming a header of the wrong length is corrupt. */
void
test_chunks_pack_tests_refuses_a_footer_with_a_short_header(void)
{
	test_chunks_pack_tests_refuses_a_footer_with_a_short_header_frame_0_t frame = {0};

	std_landing(&frame, test_chunks_pack_tests_refuses_a_footer_with_a_short_header_body_0,
				test_chunks_pack_tests_refuses_a_footer_with_a_short_header_pad_0);
}

/* The test's none: a dictionary function that has no dictionary for any slot. */
static bool
test_chunks_pack_tests_a_record_or_a_table_naming_a_dictionary_the_pack_lacks_is_corrupt_closure_0(void *captures,
																								   HostIoReadDoors io,
																								   size_t slot,
																								   const HostDecompressDictionary **result,
																								   ChunksErrorError *error)
{
	*result = NULL;
	return true;
}

/* The test's table: a dictionary table of a count, each length with a zero checksum, and bytes of zeros after. */
static void
test_chunks_pack_tests_a_record_or_a_table_naming_a_dictionary_the_pack_lacks_is_corrupt_closure_1(uint16 count,
																								   const uint32 *lens,
																								   size_t lens_len,
																								   size_t bytes,
																								   StdVec *result)
{
	uint8		le[4];
	static const uint8 zeros[8] = {0};

	std_vec_new(result, 1, 1);
	std_u16_to_le_bytes(count, le);
	std_vec_extend_from_slice_Checked(result, le, 2, 1, 1);
	for (size_t k = 0, k_len = lens_len; k < k_len; k++)
	{
		std_u32_to_le_bytes(lens[k], le);
		std_vec_extend_from_slice_Checked(result, le, 4, 1, 1);
		std_vec_extend_from_slice_Checked(result, zeros, sizeof(zeros), 1, 1);
	}
	for (size_t k = 0, k_len = bytes; k < k_len; k++)
		std_vec_u8_push_Checked(result, 0);
}

/* The state of the test across its pad: its capability, the table it reads, the slots it finds, and the error. */
typedef struct test_chunks_pack_tests_a_record_or_a_table_naming_a_dictionary_the_pack_lacks_is_corrupt_frame_0_t
{
	SimcTestIo	test_io;
	bool		test_io_live;
	StdVec		table;
	bool		table_live;
	StdVec		slots;
	bool		slots_live;
	StdVec		read;
	bool		read_live;
	ChunksErrorError error;
	bool		error_live;
} test_chunks_pack_tests_a_record_or_a_table_naming_a_dictionary_the_pack_lacks_is_corrupt_frame_0_t;

/* The test's pad. */
static void
test_chunks_pack_tests_a_record_or_a_table_naming_a_dictionary_the_pack_lacks_is_corrupt_pad_0(void *frame_)
{
	test_chunks_pack_tests_a_record_or_a_table_naming_a_dictionary_the_pack_lacks_is_corrupt_frame_0_t *frame = frame_;

	if (frame->error_live)
		chunks_error_Error_drop(&frame->error);
	if (frame->read_live)
		std_vec_free_Checked(&frame->read, 1, 1);
	if (frame->slots_live)
		chunks_pack_Slots_drop(&frame->slots);
	if (frame->table_live)
		std_vec_free_Checked(&frame->table, 1, 1);
	if (frame->test_io_live)
		simc_TestIo_drop(&frame->test_io);
}

/* chunks_pack_tests_table_none asserts that a dictionary table of a count, lengths, and bytes after names no slots. */
static void
chunks_pack_tests_table_none(test_chunks_pack_tests_a_record_or_a_table_naming_a_dictionary_the_pack_lacks_is_corrupt_frame_0_t *frame,
							 uint32 line, uint32 col, uint16 count, const uint32 *lens, size_t lens_len, size_t bytes,
							 const char *source)
{
	test_chunks_pack_tests_a_record_or_a_table_naming_a_dictionary_the_pack_lacks_is_corrupt_closure_1(count, lens, lens_len,
																									   bytes,
																									   &frame->table);
	frame->table_live = true;
	if (chunks_pack_dictionary_table(frame->table.ptr, frame->table.len, 100, &frame->slots))
	{
		StdFmtPiece pieces[] = {STD_FMT_LIT("assertion failed: "), STD_FMT_ARG(0)};
		StdStr		shown = {source, strlen(source)};
		StdFmtArg	args[] = {{&shown, std_str_Display_fmt}};
		StdFmtArguments arguments = {pieces, 2, args};

		frame->slots_live = true;
		std_panic_fmt(RUST_FILE, line, col, &arguments);
	}
	frame->table_live = false;
	std_vec_free_Checked(&frame->table, 1, 1);
}

/* The test's body. */
static void
test_chunks_pack_tests_a_record_or_a_table_naming_a_dictionary_the_pack_lacks_is_corrupt_body_0(void *frame_)
{
	test_chunks_pack_tests_a_record_or_a_table_naming_a_dictionary_the_pack_lacks_is_corrupt_frame_0_t *frame = frame_;
	static const uint8 record[] = {CHUNKS_PACK_WITH_DICTIONARY, 5, 0x28, 0xb5};
	static const uint32 three_four[] = {3, 4};
	static const uint32 max_four[] = {PG_UINT32_MAX, 4};
	static const uint32 zeros[CHUNKS_PACK_MAX_DICTIONARIES + 1] = {0};
	static const uint8 one[] = {1};
	bool		found;

	frame->test_io = simc_io();
	frame->test_io_live = true;
	if (chunks_pack_decompress(HOST_IO_IO_READ_DOORS(&frame->test_io.io), record, sizeof(record),
							   test_chunks_pack_tests_a_record_or_a_table_naming_a_dictionary_the_pack_lacks_is_corrupt_closure_0,
							   NULL, NULL, &frame->read, &frame->error))
	{
		frame->read_live = true;
		std_panic(RUST_FILE, 1228, 9, "assertion failed: matches!(read, Err(Error::Corrupt(_)))",
				  strlen("assertion failed: matches!(read, Err(Error::Corrupt(_)))"));
	}
	frame->error_live = true;
	if (frame->error.tag != CHUNKS_ERROR_ERROR_CORRUPT)
		std_panic(RUST_FILE, 1228, 9, "assertion failed: matches!(read, Err(Error::Corrupt(_)))",
				  strlen("assertion failed: matches!(read, Err(Error::Corrupt(_)))"));
	frame->error_live = false;
	chunks_error_Error_drop(&frame->error);
	test_chunks_pack_tests_a_record_or_a_table_naming_a_dictionary_the_pack_lacks_is_corrupt_closure_1(2, three_four, 2, 7,
																									   &frame->table);
	frame->table_live = true;
	found = chunks_pack_dictionary_table(frame->table.ptr, frame->table.len, 100, &frame->slots);
	if (found)
		frame->slots_live = true;
	STD_ASSERT(1238, 9, found && STD_VEC_AT(&frame->slots, chunks_pack_Slot, 1).at == 100 + 26 + 3,
			   "dictionary_table(&table(2, &[3, 4], 7), 100).is_some_and(|slots| slots[1].at == 100 + 26 + 3)");
	frame->slots_live = false;
	chunks_pack_Slots_drop(&frame->slots);
	frame->table_live = false;
	std_vec_free_Checked(&frame->table, 1, 1);
	chunks_pack_tests_table_none(frame, 1239, 9, 2, three_four, 2, 6,
								 "dictionary_table(&table(2, &[3, 4], 6), 100).is_none()");
	chunks_pack_tests_table_none(frame, 1240, 9, 2, three_four, 2, 8,
								 "dictionary_table(&table(2, &[3, 4], 8), 100).is_none()");
	chunks_pack_tests_table_none(frame, 1241, 9, 3, three_four, 2, 7,
								 "dictionary_table(&table(3, &[3, 4], 7), 100).is_none()");
	chunks_pack_tests_table_none(frame, 1242, 9, 2, max_four, 2, 7,
								 "dictionary_table(&table(2, &[u32::MAX, 4], 7), 100).is_none()");
	chunks_pack_tests_table_none(frame, 1243, 9, CHUNKS_PACK_MAX_DICTIONARIES + 1, zeros,
								 CHUNKS_PACK_MAX_DICTIONARIES + 1, 0,
								 "dictionary_table(&table(MAX_DICTIONARIES as u16 + 1, &[0; MAX_DICTIONARIES + 1], 0), 100).is_none()");
	if (chunks_pack_dictionary_table(one, sizeof(one), 100, &frame->slots))
	{
		frame->slots_live = true;
		std_panic(RUST_FILE, 1244, 9, "assertion failed: dictionary_table(&[1], 100).is_none()",
				  strlen("assertion failed: dictionary_table(&[1], 100).is_none()"));
	}
	frame->test_io_live = false;
	simc_TestIo_drop(&frame->test_io);
}

/*
 * test_chunks_pack_tests_a_record_or_a_table_naming_a_dictionary_the_pack_lacks_is_corrupt:
 * a record naming a dictionary slot the pack lacks is corrupt, as is a
 * dictionary table whose count or lengths pass its end, leave bytes over, or
 * name more than MAX_DICTIONARIES.
 */
void
test_chunks_pack_tests_a_record_or_a_table_naming_a_dictionary_the_pack_lacks_is_corrupt(void)
{
	test_chunks_pack_tests_a_record_or_a_table_naming_a_dictionary_the_pack_lacks_is_corrupt_frame_0_t frame = {0};

	std_landing(&frame, test_chunks_pack_tests_a_record_or_a_table_naming_a_dictionary_the_pack_lacks_is_corrupt_body_0,
				test_chunks_pack_tests_a_record_or_a_table_naming_a_dictionary_the_pack_lacks_is_corrupt_pad_0);
}

/*
 * The state of a_writer_dropped_unfinished_leaves_its_file_for_the_backends_next_writer_to_remove
 * across its pad: its capability, directory, writers, their paths, and an
 * unwrapped error.
 */
typedef struct test_chunks_pack_tests_a_writer_dropped_unfinished_leaves_its_file_for_the_backends_next_writer_to_remove_frame_0_t
{
	SimcTestIo	test_io;
	bool		test_io_live;
	SimcTempDir dir;
	bool		dir_live;
	ChunksPackPackWriterInDir writer;
	bool		writer_live;
	StdPathBuf	left;
	bool		left_live;
	StdPathBuf	unfinished;
	bool		unfinished_live;
	ChunksErrorError error;
	bool		error_live;
} test_chunks_pack_tests_a_writer_dropped_unfinished_leaves_its_file_for_the_backends_next_writer_to_remove_frame_0_t;

/* The test's pad. */
static void
test_chunks_pack_tests_a_writer_dropped_unfinished_leaves_its_file_for_the_backends_next_writer_to_remove_pad_0(void *frame_)
{
	test_chunks_pack_tests_a_writer_dropped_unfinished_leaves_its_file_for_the_backends_next_writer_to_remove_frame_0_t *frame = frame_;

	if (frame->error_live)
		chunks_error_Error_drop(&frame->error);
	if (frame->unfinished_live)
		std_vec_free_Checked(&frame->unfinished.bytes, 1, 1);
	if (frame->left_live)
		std_vec_free_Checked(&frame->left.bytes, 1, 1);
	if (frame->writer_live)
		chunks_pack_PackWriterInDir_drop(&frame->writer);
	if (frame->dir_live)
		simc_TempDir_drop(&frame->dir);
	if (frame->test_io_live)
		simc_TestIo_drop(&frame->test_io);
}

/* The test's body. */
static void
test_chunks_pack_tests_a_writer_dropped_unfinished_leaves_its_file_for_the_backends_next_writer_to_remove_body_0(void *frame_)
{
	test_chunks_pack_tests_a_writer_dropped_unfinished_leaves_its_file_for_the_backends_next_writer_to_remove_frame_0_t *frame = frame_;
	HostIoIo   *io;
	StdIoError	io_error;
	const char *dir_path;
	size_t		dir_path_len;

	frame->test_io = simc_io();
	frame->test_io_live = true;
	io = &frame->test_io.io;
	if (!simc_tempdir(&frame->dir, &io_error))
		STD_RESULT_EXPECT_FAILED(1458, 34, CHUNKS_PACK_TESTS_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	frame->dir_live = true;
	dir_path = simc_TempDir_path(&frame->dir, &dir_path_len);
	if (!chunks_pack_PackWriterInDir_create(io, dir_path, dir_path_len, 1, &frame->writer, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1459, 60, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->writer_live = true;
	std_PathBuf_Clone_clone(&frame->writer.file.temp.path, &frame->left);
	frame->left_live = true;
	frame->writer_live = false;
	chunks_pack_PackWriterInDir_drop(&frame->writer);
	STD_ASSERT(1462, 9, std_path_Path_exists((const char *) frame->left.bytes.ptr, frame->left.bytes.len),
			   "left.exists()");
	if (!chunks_pack_PackWriterInDir_create(io, dir_path, dir_path_len, 2, &frame->writer, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1463, 58, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->writer_live = true;
	STD_ASSERT(1464, 9, !std_path_Path_exists((const char *) frame->left.bytes.ptr, frame->left.bytes.len),
			   "!left.exists()");
	std_PathBuf_Clone_clone(&frame->writer.file.temp.path, &frame->unfinished);
	frame->unfinished_live = true;
	frame->writer_live = false;
	if (!chunks_pack_PackWriterInDir_abandon(&frame->writer, HOST_IO_IO_TAKES_SHORT(io), &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1466, 26, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	STD_ASSERT(1467, 9,
			   !std_path_Path_exists((const char *) frame->unfinished.bytes.ptr, frame->unfinished.bytes.len),
			   "!unfinished.exists()");
	test_chunks_pack_tests_a_writer_dropped_unfinished_leaves_its_file_for_the_backends_next_writer_to_remove_pad_0(frame);
}

/*
 * test_chunks_pack_tests_a_writer_dropped_unfinished_leaves_its_file_for_the_backends_next_writer_to_remove:
 * a writer dropped unfinished leaves its file, which the backend's next
 * writer removes as it starts.
 */
void
test_chunks_pack_tests_a_writer_dropped_unfinished_leaves_its_file_for_the_backends_next_writer_to_remove(void)
{
	test_chunks_pack_tests_a_writer_dropped_unfinished_leaves_its_file_for_the_backends_next_writer_to_remove_frame_0_t frame = {0};

	std_landing(&frame,
				test_chunks_pack_tests_a_writer_dropped_unfinished_leaves_its_file_for_the_backends_next_writer_to_remove_body_0,
				test_chunks_pack_tests_a_writer_dropped_unfinished_leaves_its_file_for_the_backends_next_writer_to_remove_pad_0);
}

/*
 * The state of an_abandon_that_fails_leaves_its_file_for_the_backends_next_writer_to_remove
 * across its pad: its capability, directory, simulation, writers, the
 * path left, and an unwrapped error.
 */
typedef struct test_chunks_pack_tests_an_abandon_that_fails_leaves_its_file_for_the_backends_next_writer_to_remove_frame_0_t
{
	SimcTestIo	test_io;
	bool		test_io_live;
	SimcTempDir dir;
	bool		dir_live;
	SimcSim		sim;
	bool		sim_live;
	ChunksPackPackWriterInDir writer;
	bool		writer_live;
	StdPathBuf	left;
	bool		left_live;
	ChunksErrorError error;
	bool		error_live;
} test_chunks_pack_tests_an_abandon_that_fails_leaves_its_file_for_the_backends_next_writer_to_remove_frame_0_t;

/* The test's pad. */
static void
test_chunks_pack_tests_an_abandon_that_fails_leaves_its_file_for_the_backends_next_writer_to_remove_pad_0(void *frame_)
{
	test_chunks_pack_tests_an_abandon_that_fails_leaves_its_file_for_the_backends_next_writer_to_remove_frame_0_t *frame = frame_;

	if (frame->error_live)
		chunks_error_Error_drop(&frame->error);
	if (frame->left_live)
		std_vec_free_Checked(&frame->left.bytes, 1, 1);
	if (frame->writer_live)
		chunks_pack_PackWriterInDir_drop(&frame->writer);
	if (frame->sim_live)
		simc_Sim_drop(&frame->sim);
	if (frame->dir_live)
		simc_TempDir_drop(&frame->dir);
	if (frame->test_io_live)
		simc_TestIo_drop(&frame->test_io);
}

/* The test's body. */
static void
test_chunks_pack_tests_an_abandon_that_fails_leaves_its_file_for_the_backends_next_writer_to_remove_body_0(void *frame_)
{
	test_chunks_pack_tests_an_abandon_that_fails_leaves_its_file_for_the_backends_next_writer_to_remove_frame_0_t *frame = frame_;
	HostIoIo   *io;
	StdIoError	io_error;
	const char *dir_path;
	size_t		dir_path_len;

	frame->test_io = simc_io();
	frame->test_io_live = true;
	io = &frame->test_io.io;
	if (!simc_tempdir(&frame->dir, &io_error))
		STD_RESULT_EXPECT_FAILED(1475, 34, CHUNKS_PACK_TESTS_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	frame->dir_live = true;
	dir_path = simc_TempDir_path(&frame->dir, &dir_path_len);
	frame->sim = simc_Sim_start(dir_path, dir_path_len, 13);
	frame->sim_live = true;
	if (!chunks_pack_PackWriterInDir_create(io, dir_path, dir_path_len, 1, &frame->writer, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1477, 60, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->writer_live = true;
	std_PathBuf_Clone_clone(&frame->writer.file.temp.path, &frame->left);
	frame->left_live = true;
	simc_Sim_full_at(&frame->sim, STD_ADD(u64, simc_Sim_calls(&frame->sim), 1, 1479, 21));
	frame->writer_live = false;
	if (chunks_pack_PackWriterInDir_abandon(&frame->writer, HOST_IO_IO_TAKES_SHORT(io), &frame->error))
		std_panic(RUST_FILE, 1480, 9, "assertion failed: writer.abandon(io).is_err()",
				  strlen("assertion failed: writer.abandon(io).is_err()"));
	chunks_error_Error_drop(&frame->error);
	STD_ASSERT(1481, 9, std_path_Path_exists((const char *) frame->left.bytes.ptr, frame->left.bytes.len),
			   "left.exists()");
	if (!chunks_pack_PackWriterInDir_create(io, dir_path, dir_path_len, 2, &frame->writer, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1482, 58, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->writer_live = true;
	STD_ASSERT(1483, 9, !std_path_Path_exists((const char *) frame->left.bytes.ptr, frame->left.bytes.len),
			   "!left.exists()");
	frame->writer_live = false;
	if (!chunks_pack_PackWriterInDir_abandon(&frame->writer, HOST_IO_IO_TAKES_SHORT(io), &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1484, 26, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	simc_Sim_stop(&frame->sim);
	test_chunks_pack_tests_an_abandon_that_fails_leaves_its_file_for_the_backends_next_writer_to_remove_pad_0(frame);
}

/*
 * test_chunks_pack_tests_an_abandon_that_fails_leaves_its_file_for_the_backends_next_writer_to_remove:
 * a writer whose abandon fails to remove its file leaves it listed, and the
 * backend's next writer removes it.
 */
void
test_chunks_pack_tests_an_abandon_that_fails_leaves_its_file_for_the_backends_next_writer_to_remove(void)
{
	test_chunks_pack_tests_an_abandon_that_fails_leaves_its_file_for_the_backends_next_writer_to_remove_frame_0_t frame = {0};

	std_landing(&frame,
				test_chunks_pack_tests_an_abandon_that_fails_leaves_its_file_for_the_backends_next_writer_to_remove_body_0,
				test_chunks_pack_tests_an_abandon_that_fails_leaves_its_file_for_the_backends_next_writer_to_remove_pad_0);
}

/* The state of an_unfinished_packs_name_carries_a_wide_number across its pad: the extension and the name, while live. */
typedef struct test_chunks_pack_tests_an_unfinished_packs_name_carries_a_wide_number_frame_0_t
{
	StdString	extension;
	bool		extension_live;
	StdString	name;
	bool		name_live;
} test_chunks_pack_tests_an_unfinished_packs_name_carries_a_wide_number_frame_0_t;

/* The test's pad. */
static void
test_chunks_pack_tests_an_unfinished_packs_name_carries_a_wide_number_pad_0(void *frame_)
{
	test_chunks_pack_tests_an_unfinished_packs_name_carries_a_wide_number_frame_0_t *frame = frame_;

	if (frame->name_live)
		std_vec_free_Checked(&frame->name, 1, 1);
	if (frame->extension_live)
		std_vec_free_Checked(&frame->extension, 1, 1);
}

/* The test's body. */
static void
test_chunks_pack_tests_an_unfinished_packs_name_carries_a_wide_number_body_0(void *frame_)
{
	test_chunks_pack_tests_an_unfinished_packs_name_carries_a_wide_number_frame_0_t *frame = frame_;
	ChunksPackPackId id = chunks_pack_PackId_new();
	StdStr		temp = {CHUNKS_PACK_TEMP, strlen(CHUNKS_PACK_TEMP)};
	StdFmtPiece pieces[] = {STD_FMT_LIT("3.4294967301"), STD_FMT_ARG(0)};
	StdFmtArg	args[] = {{&temp, std_str_Display_fmt}};
	StdFmtArguments arguments = {pieces, 2, args};
	uint32		process;
	uint64		number;
	bool		some;

	std_fmt_format_Checked(&frame->extension, &arguments);
	frame->extension_live = true;
	frame->name = chunks_pack_PackId_file_name(&id, (const char *) frame->extension.ptr, frame->extension.len);
	frame->name_live = true;
	some = chunks_pack_temp_of((const char *) frame->name.ptr, frame->name.len, &process, &number);
	{
		uint32		want_process = 3;
		uint64		want_number = UINT64CONST(4294967301);
		StdTupleRef got_tuple = STD_TUPLE(2, STD_ITEM(&process, std_u32_Debug_fmt), STD_ITEM(&number, std_u64_Debug_fmt));
		StdTupleRef want_tuple = STD_TUPLE(2, STD_ITEM(&want_process, std_u32_Debug_fmt),
										   STD_ITEM(&want_number, std_u64_Debug_fmt));
		StdOptionRef got_option = {some, STD_ITEM(&got_tuple, std_tuple_Debug_fmt)};
		StdOptionRef want_option = {true, STD_ITEM(&want_tuple, std_tuple_Debug_fmt)};

		STD_ASSERT_EQ(1492, 9, some && process == want_process && number == want_number, &got_option,
					  std_option_Debug_fmt, &want_option, std_option_Debug_fmt);
	}
	test_chunks_pack_tests_an_unfinished_packs_name_carries_a_wide_number_pad_0(frame);
}

/*
 * test_chunks_pack_tests_an_unfinished_packs_name_carries_a_wide_number: an
 * unfinished pack's file name gives back its process's number and a pack's
 * number above 32 bits.
 */
void
test_chunks_pack_tests_an_unfinished_packs_name_carries_a_wide_number(void)
{
	test_chunks_pack_tests_an_unfinished_packs_name_carries_a_wide_number_frame_0_t frame = {0};

	std_landing(&frame, test_chunks_pack_tests_an_unfinished_packs_name_carries_a_wide_number_body_0,
				test_chunks_pack_tests_an_unfinished_packs_name_carries_a_wide_number_pad_0);
}

/*
 * The state of a_copy_takes_no_record_whose_stored_bytes_lack_its_address
 * across its pad: its capability, directory, writer, chunk, pack, the file's
 * bytes, reader, and an unwrapped error.
 */
typedef struct test_chunks_pack_tests_a_copy_takes_no_record_whose_stored_bytes_lack_its_address_frame_0_t
{
	SimcTestIo	test_io;
	bool		test_io_live;
	SimcTempDir dir;
	bool		dir_live;
	ChunksPackPackWriterInDir writer;
	bool		writer_live;
	chunks_TupleAddressVecU8_t chunk;
	bool		chunk_live;
	ChunksPackPack pack;
	bool		pack_live;
	StdVec		bytes;
	bool		bytes_live;
	ChunksPackPackReader reader;
	bool		reader_live;
	ChunksErrorError error;
	bool		error_live;
} test_chunks_pack_tests_a_copy_takes_no_record_whose_stored_bytes_lack_its_address_frame_0_t;

/* The test's pad. */
static void
test_chunks_pack_tests_a_copy_takes_no_record_whose_stored_bytes_lack_its_address_pad_0(void *frame_)
{
	test_chunks_pack_tests_a_copy_takes_no_record_whose_stored_bytes_lack_its_address_frame_0_t *frame = frame_;

	if (frame->error_live)
		chunks_error_Error_drop(&frame->error);
	if (frame->reader_live)
		chunks_pack_PackReader_drop(&frame->reader);
	if (frame->bytes_live)
		std_vec_free_Checked(&frame->bytes, 1, 1);
	if (frame->pack_live)
		chunks_pack_Pack_drop(&frame->pack);
	if (frame->chunk_live)
		std_vec_free_Checked(&frame->chunk.f1, 1, 1);
	if (frame->writer_live)
		chunks_pack_PackWriterInDir_drop(&frame->writer);
	if (frame->dir_live)
		simc_TempDir_drop(&frame->dir);
	if (frame->test_io_live)
		simc_TestIo_drop(&frame->test_io);
}

/* The test's body. */
static void
test_chunks_pack_tests_a_copy_takes_no_record_whose_stored_bytes_lack_its_address_body_0(void *frame_)
{
	test_chunks_pack_tests_a_copy_takes_no_record_whose_stored_bytes_lack_its_address_frame_0_t *frame = frame_;
	HostIoIo   *io;
	StdIoError	io_error;
	const char *dir_path;
	size_t		dir_path_len;
	ChunksTrailerLocator locator;
	size_t		at;
	ChunksPackRaw raw;

	frame->test_io = simc_io();
	frame->test_io_live = true;
	io = &frame->test_io.io;
	if (!simc_tempdir(&frame->dir, &io_error))
		STD_RESULT_EXPECT_FAILED(1393, 34, CHUNKS_PACK_TESTS_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	frame->dir_live = true;
	dir_path = simc_TempDir_path(&frame->dir, &dir_path_len);
	if (!chunks_pack_PackWriterInDir_create(io, dir_path, dir_path_len, 4, &frame->writer, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1394, 64, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->writer_live = true;
	chunks_pack_tests_chunk(1, &frame->chunk);
	frame->chunk_live = true;
	if (!chunks_pack_PackWriterInDir_write(&frame->writer, io, frame->chunk.f0, frame->chunk.f1.ptr,
										   frame->chunk.f1.len, false, &locator, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1396, 65, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->writer_live = false;
	if (!chunks_pack_PackWriterInDir_finish(&frame->writer, HOST_IO_IO_TAKES_SHORT(io), &frame->pack, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1397, 38, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->pack_live = true;
	if (!std_fs_read((const char *) frame->pack.path.bytes.ptr, frame->pack.path.bytes.len, &frame->bytes, &io_error))
		STD_RESULT_EXPECT_FAILED(1398, 51, CHUNKS_PACK_TESTS_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	frame->bytes_live = true;
	at = STD_ADD(usize, (size_t) chunks_trailer_Locator_offset(&locator), 20, 1399, 15);
	std_index_check(at, frame->bytes.len, RUST_FILE, 1399, 9);
	((uint8 *) frame->bytes.ptr)[at] ^= 1;
	if (!std_fs_write((const char *) frame->pack.path.bytes.ptr, frame->pack.path.bytes.len, frame->bytes.ptr,
					  frame->bytes.len, &io_error))
		STD_RESULT_EXPECT_FAILED(1400, 44, CHUNKS_PACK_TESTS_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	if (!chunks_pack_PackReader_open(HOST_IO_IO_READ_DOORS(io), (const char *) frame->pack.path.bytes.ptr,
									 frame->pack.path.bytes.len, &frame->reader, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1401, 55, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->reader_live = true;
	if (chunks_pack_PackReader_raw(&frame->reader, io, locator, &frame->chunk.f0, &raw, &frame->error))
	{
		chunks_pack_Raw_drop(&raw);
		std_panic(RUST_FILE, 1402, 9,
				  "assertion failed: matches!(reader.raw(io, locator, &address), Err(Error::Corrupt(_)))",
				  strlen("assertion failed: matches!(reader.raw(io, locator, &address), Err(Error::Corrupt(_)))"));
	}
	frame->error_live = true;
	if (frame->error.tag != CHUNKS_ERROR_ERROR_CORRUPT)
		std_panic(RUST_FILE, 1402, 9,
				  "assertion failed: matches!(reader.raw(io, locator, &address), Err(Error::Corrupt(_)))",
				  strlen("assertion failed: matches!(reader.raw(io, locator, &address), Err(Error::Corrupt(_)))"));
	test_chunks_pack_tests_a_copy_takes_no_record_whose_stored_bytes_lack_its_address_pad_0(frame);
}

/*
 * test_chunks_pack_tests_a_copy_takes_no_record_whose_stored_bytes_lack_its_address:
 * a record whose stored bytes no longer hash to its address is corrupt to a
 * copy's raw read.
 */
void
test_chunks_pack_tests_a_copy_takes_no_record_whose_stored_bytes_lack_its_address(void)
{
	test_chunks_pack_tests_a_copy_takes_no_record_whose_stored_bytes_lack_its_address_frame_0_t frame = {0};

	std_landing(&frame, test_chunks_pack_tests_a_copy_takes_no_record_whose_stored_bytes_lack_its_address_body_0,
				test_chunks_pack_tests_a_copy_takes_no_record_whose_stored_bytes_lack_its_address_pad_0);
}

/* The cancel test's walk: it counts the chunks walked. */
static bool
test_chunks_pack_tests_a_walk_of_a_packs_chunks_stops_within_a_chunk_once_a_cancel_is_pending_closure_0(void *captures,
																										ChunksFileInterruptible io,
																										ChunksPackVerified *verified,
																										ChunksErrorError *error)
{
	size_t	   *walked = captures;

	*walked = STD_ADD(usize, *walked, 1, 1420, 13);
	return true;
}

/*
 * The state of a_walk_of_a_packs_chunks_stops_within_a_chunk_once_a_cancel_is_pending
 * across its pad: its capability, directory, writer, chunk, pack, reader,
 * what the walk lost, and an error.
 */
typedef struct test_chunks_pack_tests_a_walk_of_a_packs_chunks_stops_within_a_chunk_once_a_cancel_is_pending_frame_0_t
{
	SimcTestIo	test_io;
	bool		test_io_live;
	SimcTempDir dir;
	bool		dir_live;
	ChunksPackPackWriterInDir writer;
	bool		writer_live;
	chunks_TupleAddressVecU8_t chunk;
	bool		chunk_live;
	ChunksPackPack pack;
	bool		pack_live;
	ChunksPackPackReader reader;
	bool		reader_live;
	ChunksPackLost lost;
	bool		lost_live;
	ChunksErrorError error;
	bool		error_live;
} test_chunks_pack_tests_a_walk_of_a_packs_chunks_stops_within_a_chunk_once_a_cancel_is_pending_frame_0_t;

/* The test's pad. */
static void
test_chunks_pack_tests_a_walk_of_a_packs_chunks_stops_within_a_chunk_once_a_cancel_is_pending_pad_0(void *frame_)
{
	test_chunks_pack_tests_a_walk_of_a_packs_chunks_stops_within_a_chunk_once_a_cancel_is_pending_frame_0_t *frame = frame_;

	if (frame->error_live)
		chunks_error_Error_drop(&frame->error);
	if (frame->lost_live)
		chunks_pack_Lost_drop(&frame->lost);
	if (frame->reader_live)
		chunks_pack_PackReader_drop(&frame->reader);
	if (frame->pack_live)
		chunks_pack_Pack_drop(&frame->pack);
	if (frame->chunk_live)
		std_vec_free_Checked(&frame->chunk.f1, 1, 1);
	if (frame->writer_live)
		chunks_pack_PackWriterInDir_drop(&frame->writer);
	if (frame->dir_live)
		simc_TempDir_drop(&frame->dir);
	if (frame->test_io_live)
		simc_TestIo_drop(&frame->test_io);
}

/* The test's body. */
static void
test_chunks_pack_tests_a_walk_of_a_packs_chunks_stops_within_a_chunk_once_a_cancel_is_pending_body_0(void *frame_)
{
	test_chunks_pack_tests_a_walk_of_a_packs_chunks_stops_within_a_chunk_once_a_cancel_is_pending_frame_0_t *frame = frame_;
	HostIoIo   *io;
	StdIoError	io_error;
	const char *dir_path;
	size_t		dir_path_len;
	size_t		walked = 0;
	size_t		three = 3;
	bool		read;

	frame->test_io = simc_io();
	frame->test_io_live = true;
	io = &frame->test_io.io;
	if (!simc_tempdir(&frame->dir, &io_error))
		STD_RESULT_EXPECT_FAILED(1409, 34, CHUNKS_PACK_TESTS_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	frame->dir_live = true;
	dir_path = simc_TempDir_path(&frame->dir, &dir_path_len);
	if (!chunks_pack_PackWriterInDir_create(io, dir_path, dir_path_len, 4, &frame->writer, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1410, 64, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->writer_live = true;
	for (uint32 n = 1, n_end = 10; n <= n_end; n = std_forward_unchecked_u32_Checked(n, 1))
	{
		ChunksTrailerLocator locator;

		chunks_pack_tests_chunk(n, &frame->chunk);
		frame->chunk_live = true;
		if (!chunks_pack_PackWriterInDir_write(&frame->writer, io, frame->chunk.f0, frame->chunk.f1.ptr,
											   frame->chunk.f1.len, false, &locator, &frame->error))
		{
			frame->error_live = true;
			STD_RESULT_EXPECT_FAILED(1413, 55, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
									 chunks_error_Error_Debug_fmt);
		}
		frame->chunk_live = false;
		std_vec_free_Checked(&frame->chunk.f1, 1, 1);
	}
	frame->writer_live = false;
	if (!chunks_pack_PackWriterInDir_finish(&frame->writer, HOST_IO_IO_TAKES_SHORT(io), &frame->pack, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1415, 38, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->pack_live = true;
	if (!chunks_pack_PackReader_open(HOST_IO_IO_READ_DOORS(io), (const char *) frame->pack.path.bytes.ptr,
									 frame->pack.path.bytes.len, &frame->reader, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1416, 55, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->reader_live = true;
	simc_interrupt_at(3);
	read = chunks_pack_PackReader_verified(&frame->reader, CHUNKS_PACK_TESTS_IO_INTERRUPTIBLE(io), 4,
										   test_chunks_pack_tests_a_walk_of_a_packs_chunks_stops_within_a_chunk_once_a_cancel_is_pending_closure_0,
										   &walked, NULL, &frame->lost, &frame->error);
	if (read)
		frame->lost_live = true;
	else
		frame->error_live = true;
	STD_ASSERT_MSG(1423, 9,
				   !read && frame->error.tag == CHUNKS_ERROR_ERROR_HOST &&
				   memcmp(frame->error.u.host.sqlstate, "57014", 5) == 0,
				   "a walk of a pack went on past a cancel");
	STD_ASSERT_EQ(1427, 9, walked == three, &walked, std_usize_Debug_fmt, &three, std_usize_Debug_fmt);
	test_chunks_pack_tests_a_walk_of_a_packs_chunks_stops_within_a_chunk_once_a_cancel_is_pending_pad_0(frame);
}

/*
 * test_chunks_pack_tests_a_walk_of_a_packs_chunks_stops_within_a_chunk_once_a_cancel_is_pending:
 * a walk of a pack's chunks checks for an interrupt before each chunk, and
 * stops with Postgres' cancel once one is pending.
 */
void
test_chunks_pack_tests_a_walk_of_a_packs_chunks_stops_within_a_chunk_once_a_cancel_is_pending(void)
{
	test_chunks_pack_tests_a_walk_of_a_packs_chunks_stops_within_a_chunk_once_a_cancel_is_pending_frame_0_t frame = {0};

	std_landing(&frame,
				test_chunks_pack_tests_a_walk_of_a_packs_chunks_stops_within_a_chunk_once_a_cancel_is_pending_body_0,
				test_chunks_pack_tests_a_walk_of_a_packs_chunks_stops_within_a_chunk_once_a_cancel_is_pending_pad_0);
}

/* chunks_pack_tests_stamped_frame_0_t is the keeps-each-write test's stamped's state across its pad: the stamps, while live. */
typedef struct chunks_pack_tests_stamped_frame_0_t
{
	const StdVec *hashed;
	StdVec	   *result;
	ChunksTrailerStamps stamps;
	bool		stamps_live;
} chunks_pack_tests_stamped_frame_0_t;

/* chunks_pack_tests_stamped_pad_0 is stamped's pad: the stamps, while live. */
static void
chunks_pack_tests_stamped_pad_0(void *frame_)
{
	chunks_pack_tests_stamped_frame_0_t *frame = frame_;

	if (frame->stamps_live)
	{
		frame->stamps_live = false;
		chunks_trailer_Stamps_drop(&frame->stamps);
	}
}

/* chunks_pack_tests_stamped_body_0 is stamped's body: the hashed bytes stored with the stamps. */
static void
chunks_pack_tests_stamped_body_0(void *frame_)
{
	chunks_pack_tests_stamped_frame_0_t *frame = frame_;

	chunks_trailer_store(frame->hashed->ptr, frame->hashed->len, NULL, 0, &frame->stamps, frame->result);
	chunks_pack_tests_stamped_pad_0(frame);
}

/* The keeps-each-write test's stamped: the hashed bytes stored with one stamp of a transaction. */
static void
test_chunks_pack_tests_a_pack_keeps_each_write_of_an_address_whose_trailer_differs_and_walks_them_all_closure_0(const StdVec *hashed,
																												uint64 txn,
																												StdVec *result)
{
	uint8		stamp[CHUNKS_TRAILER_STAMP_LEN] = {0};
	ChunksTrailerStamp read;
	StdVec		single;
	chunks_pack_tests_stamped_frame_0_t frame = {.hashed = hashed, .result = result};

	std_u64_to_le_bytes(txn, stamp);
	read = chunks_trailer_Stamp_read(stamp, sizeof(stamp));
	std_vec_with_capacity_Checked(&single, 1, sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
	std_vec_push_Checked(&single, &read, sizeof(read), _Alignof(ChunksTrailerStamp));
	frame.stamps = chunks_trailer_Stamps_FromVecStamp_from(&single);
	frame.stamps_live = true;
	std_landing(&frame, chunks_pack_tests_stamped_body_0, chunks_pack_tests_stamped_pad_0);
}

/* The keeps-each-write test's walk: each chunk's locator and stored bytes, pushed in the walk's order. */
static bool
test_chunks_pack_tests_a_pack_keeps_each_write_of_an_address_whose_trailer_differs_and_walks_them_all_closure_1(void *captures,
																												ChunksFileInterruptible io,
																												ChunksPackVerified *chunk,
																												ChunksErrorError *error)
{
	StdVec	   *walked = captures;
	chunks_TupleAddressVecU8Locator_t entry = {.f2 = chunks_pack_Verified_locator(chunk)};
	const uint8 *stored;
	size_t		stored_len;

	stored = chunks_pack_Verified_stored(chunk, &stored_len);
	std_slice_to_vec_Checked(&entry.f1, stored, stored_len, 1, 1);
	std_vec_push_Checked(walked, &entry, sizeof(entry), _Alignof(chunks_TupleAddressVecU8Locator_t));
	return true;
}

/*
 * The state of a_pack_keeps_each_write_of_an_address_whose_trailer_differs_and_walks_them_all
 * across its pad: its capability, directory, writer, hashed bytes, the two
 * stored copies, pack, reader, a chunk's bytes read back, what the walk
 * walked and lost, and an unwrapped error.
 */
typedef struct test_chunks_pack_tests_a_pack_keeps_each_write_of_an_address_whose_trailer_differs_and_walks_them_all_frame_0_t
{
	SimcTestIo	test_io;
	bool		test_io_live;
	SimcTempDir dir;
	bool		dir_live;
	ChunksPackPackWriterInDir writer;
	bool		writer_live;
	StdVec		hashed;
	bool		hashed_live;
	StdVec		first;
	bool		first_live;
	StdVec		second;
	bool		second_live;
	ChunksPackPack pack;
	bool		pack_live;
	ChunksPackPackReader reader;
	bool		reader_live;
	StdVec		bytes;
	bool		bytes_live;
	StdVec		walked;
	bool		walked_live;
	ChunksPackLost lost;
	bool		lost_live;
	ChunksErrorError error;
	bool		error_live;
} test_chunks_pack_tests_a_pack_keeps_each_write_of_an_address_whose_trailer_differs_and_walks_them_all_frame_0_t;

/* The test's pad. */
static void
test_chunks_pack_tests_a_pack_keeps_each_write_of_an_address_whose_trailer_differs_and_walks_them_all_pad_0(void *frame_)
{
	test_chunks_pack_tests_a_pack_keeps_each_write_of_an_address_whose_trailer_differs_and_walks_them_all_frame_0_t *frame = frame_;

	if (frame->error_live)
		chunks_error_Error_drop(&frame->error);
	if (frame->lost_live)
		chunks_pack_Lost_drop(&frame->lost);
	if (frame->walked_live)
		chunks_pack_tests_VecWritten_drop(&frame->walked);
	if (frame->bytes_live)
		std_vec_free_Checked(&frame->bytes, 1, 1);
	if (frame->reader_live)
		chunks_pack_PackReader_drop(&frame->reader);
	if (frame->pack_live)
		chunks_pack_Pack_drop(&frame->pack);
	if (frame->second_live)
		std_vec_free_Checked(&frame->second, 1, 1);
	if (frame->first_live)
		std_vec_free_Checked(&frame->first, 1, 1);
	if (frame->hashed_live)
		std_vec_free_Checked(&frame->hashed, 1, 1);
	if (frame->writer_live)
		chunks_pack_PackWriterInDir_drop(&frame->writer);
	if (frame->dir_live)
		simc_TempDir_drop(&frame->dir);
	if (frame->test_io_live)
		simc_TestIo_drop(&frame->test_io);
}

/* chunks_pack_tests_TupleLocatorVecU8_Debug_fmt is a walked entry's Debug, (Locator, Vec<u8>). */
static bool
chunks_pack_tests_TupleLocatorVecU8_Debug_fmt(const void *value,
											  StdFormatter *f)
{
	const chunks_TupleAddressVecU8Locator_t *self = value;
	StdBytes	bytes = {self->f1.ptr, self->f1.len};
	StdTupleRef tuple = STD_TUPLE(2, STD_ITEM(&self->f2, chunks_trailer_Locator_Debug_fmt),
								  STD_ITEM(&bytes, std_bytes_Debug_fmt));

	return std_tuple_Debug_fmt(&tuple, f);
}

/* chunks_pack_tests_VecTupleLocatorVecU8_Debug_fmt is the walked entries' Debug, a list of them. */
static bool
chunks_pack_tests_VecTupleLocatorVecU8_Debug_fmt(const void *value,
												 StdFormatter *f)
{
	const StdVec *self = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t k = 0, k_len = self->len; k < k_len; k++)
		std_DebugList_entry(&list, &STD_VEC_AT(self, chunks_TupleAddressVecU8Locator_t, k),
							chunks_pack_tests_TupleLocatorVecU8_Debug_fmt);
	return std_DebugList_finish(&list);
}

/* The test's body. */
static void
test_chunks_pack_tests_a_pack_keeps_each_write_of_an_address_whose_trailer_differs_and_walks_them_all_body_0(void *frame_)
{
	test_chunks_pack_tests_a_pack_keeps_each_write_of_an_address_whose_trailer_differs_and_walks_them_all_frame_0_t *frame = frame_;
	HostIoIo   *io;
	StdIoError	io_error;
	const char *dir_path;
	size_t		dir_path_len;
	static const uint8 lead[] = {1, 0, 0, 0};
	static const char body[] = "one address, two trailers";
	StdBytes	parts[2] = {{lead, sizeof(lead)}, {(const uint8 *) body, sizeof(body) - 1}};
	ChunksAddressAddress address;
	ChunksTrailerLocator at_first;
	ChunksTrailerLocator at_second;
	uint64		len;
	uint64		two = 2;
	ChunksPackLost none;
	bool		has;
	ChunksTrailerLocator found;
	bool		same;

	frame->test_io = simc_io();
	frame->test_io_live = true;
	io = &frame->test_io.io;
	if (!simc_tempdir(&frame->dir, &io_error))
		STD_RESULT_EXPECT_FAILED(1359, 34, CHUNKS_PACK_TESTS_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	frame->dir_live = true;
	dir_path = simc_TempDir_path(&frame->dir, &dir_path_len);
	if (!chunks_pack_PackWriterInDir_create(io, dir_path, dir_path_len, 3, &frame->writer, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1360, 64, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->writer_live = true;
	std_slice_concat(parts, 2, sizeof(StdBytes), 1, 1, &frame->hashed);
	frame->hashed_live = true;
	address = chunks_address_Address_of(frame->hashed.ptr, frame->hashed.len);
	test_chunks_pack_tests_a_pack_keeps_each_write_of_an_address_whose_trailer_differs_and_walks_them_all_closure_0(&frame->hashed,
																													7,
																													&frame->first);
	frame->first_live = true;
	test_chunks_pack_tests_a_pack_keeps_each_write_of_an_address_whose_trailer_differs_and_walks_them_all_closure_0(&frame->hashed,
																													8,
																													&frame->second);
	frame->second_live = true;
	if (!chunks_pack_PackWriterInDir_write(&frame->writer, io, address, frame->first.ptr, frame->first.len, true,
										   &at_first, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1369, 64, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	if (!chunks_pack_PackWriterInDir_write(&frame->writer, io, address, frame->second.ptr, frame->second.len, true,
										   &at_second, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1370, 66, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	if (chunks_trailer_Locator_PartialEq_eq(&at_first, &at_second))
		std_assert_failed(RUST_FILE, 1371, 9, "!=", &at_first, chunks_trailer_Locator_Debug_fmt, &at_second,
						  chunks_trailer_Locator_Debug_fmt, NULL);
	frame->writer_live = false;
	if (!chunks_pack_PackWriterInDir_finish(&frame->writer, HOST_IO_IO_TAKES_SHORT(io), &frame->pack, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1372, 38, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->pack_live = true;
	if (!chunks_pack_PackReader_open(HOST_IO_IO_READ_DOORS(io), (const char *) frame->pack.path.bytes.ptr,
									 frame->pack.path.bytes.len, &frame->reader, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1373, 55, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->reader_live = true;
	len = chunks_pack_PackReader_len(&frame->reader);
	STD_ASSERT_EQ(1374, 9, len == two, &len, std_u64_Debug_fmt, &two, std_u64_Debug_fmt);
	{
		chunks_TupleAddressVecU8Locator_t one = {address, frame->first, at_first};
		StdVec		ones = {&one, 1, 1};

		chunks_pack_tests_read_all(&frame->reader, io, &ones, &frame->error, 1375, 56, 1375, 9);
	}
	{
		chunks_TupleAddressVecU8Locator_t one = {address, frame->second, at_second};
		StdVec		ones = {&one, 1, 1};

		chunks_pack_tests_read_all(&frame->reader, io, &ones, &frame->error, 1376, 57, 1376, 9);
	}
	std_vec_new(&frame->walked, sizeof(chunks_TupleAddressVecU8Locator_t), _Alignof(chunks_TupleAddressVecU8Locator_t));
	frame->walked_live = true;
	if (!chunks_pack_PackReader_verified(&frame->reader, CHUNKS_PACK_TESTS_IO_INTERRUPTIBLE(io), 3,
										 test_chunks_pack_tests_a_pack_keeps_each_write_of_an_address_whose_trailer_differs_and_walks_them_all_closure_1,
										 &frame->walked, NULL, &frame->lost, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1383, 14, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->lost_live = true;
	none = chunks_pack_Lost_Default_default();
	STD_ASSERT_EQ(1384, 9, chunks_pack_Lost_PartialEq_eq(&frame->lost, &none), &frame->lost,
				  chunks_pack_Lost_Debug_fmt, &none, chunks_pack_Lost_Debug_fmt);
	chunks_pack_Lost_drop(&none);
	{
		chunks_TupleAddressVecU8Locator_t want[2] = {{.f1 = frame->first, .f2 = at_first},
		{.f1 = frame->second, .f2 = at_second}};
		StdVec		wanted = {want, 2, 2};

		same = frame->walked.len == 2;
		for (size_t k = 0, k_len = 2; same && k < k_len; k++)
		{
			const chunks_TupleAddressVecU8Locator_t *got = &STD_VEC_AT(&frame->walked, chunks_TupleAddressVecU8Locator_t,
																	   k);

			same = chunks_trailer_Locator_PartialEq_eq(&got->f2, &want[k].f2) && got->f1.len == want[k].f1.len &&
				memcmp(got->f1.ptr, want[k].f1.ptr, got->f1.len) == 0;
		}
		STD_ASSERT_EQ(1385, 9, same, &frame->walked,
					  chunks_pack_tests_VecTupleLocatorVecU8_Debug_fmt,
					  &wanted,
					  chunks_pack_tests_VecTupleLocatorVecU8_Debug_fmt);
	}
	if (!chunks_index_RunLocator_get(&frame->pack.run, HOST_IO_IO_READ_DOORS(io), &address, chunks_pack_tests_keep_all,
									 NULL, NULL, &has, &found, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1386, 57, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	{
		StdOptionRef got = {has, STD_ITEM(&found, chunks_trailer_Locator_Debug_fmt)};
		StdOptionRef want = {true, STD_ITEM(&at_first, chunks_trailer_Locator_Debug_fmt)};

		STD_ASSERT_EQ(1386, 9, has && chunks_trailer_Locator_PartialEq_eq(&found, &at_first), &got,
					  std_option_Debug_fmt, &want, std_option_Debug_fmt);
	}
	test_chunks_pack_tests_a_pack_keeps_each_write_of_an_address_whose_trailer_differs_and_walks_them_all_pad_0(frame);
}

/*
 * test_chunks_pack_tests_a_pack_keeps_each_write_of_an_address_whose_trailer_differs_and_walks_them_all:
 * two writes of one address whose trailers differ are both kept, read back,
 * and walked, and the run finds the first.
 */
void
test_chunks_pack_tests_a_pack_keeps_each_write_of_an_address_whose_trailer_differs_and_walks_them_all(void)
{
	test_chunks_pack_tests_a_pack_keeps_each_write_of_an_address_whose_trailer_differs_and_walks_them_all_frame_0_t frame = {0};

	std_landing(&frame,
				test_chunks_pack_tests_a_pack_keeps_each_write_of_an_address_whose_trailer_differs_and_walks_them_all_body_0,
				test_chunks_pack_tests_a_pack_keeps_each_write_of_an_address_whose_trailer_differs_and_walks_them_all_pad_0);
}

/*
 * chunks_pack_tests_leaves writes leaves numbered from a start by a step, a
 * count of them, appending each with its locator to the chunks written; an
 * unwrap of a write fails at its line and column.
 */
static void
chunks_pack_tests_leaves(HostIoIo *io, ChunksPackPackWriterInDir *writer, uint32 start, uint32 step, uint32 count,
						 StdVec *written, ChunksErrorError *error, uint32 line, uint32 col)
{
	for (uint32 i = 0, i_end = count; i < i_end; i = std_forward_unchecked_u32_Checked(i, 1))
	{
		chunks_TupleAddressVecU8_t chunk;
		chunks_TupleAddressVecU8Locator_t entry;

		chunks_pack_tests_chunk(STD_ADD(u32, STD_MUL(u32, i, step, line, col), start, line, col), &chunk);
		if (!chunks_pack_PackWriterInDir_write(writer, io, chunk.f0, chunk.f1.ptr, chunk.f1.len, true, &entry.f2, error))
		{
			std_vec_free_Checked(&chunk.f1, 1, 1);
			STD_RESULT_EXPECT_FAILED(line, col, CHUNKS_PACK_TESTS_UNWRAP, error, chunks_error_Error_Debug_fmt);
		}
		entry.f0 = chunk.f0;
		entry.f1 = chunk.f1;
		std_vec_push_Checked(written, &entry, sizeof(entry), _Alignof(chunks_TupleAddressVecU8Locator_t));
	}
}

/* chunks_pack_tests_read_all asserts that a pack's reader reads back every chunk written, at its lines and columns. */
static void
chunks_pack_tests_read_all(const ChunksPackPackReader *reader, HostIoIo *io, const StdVec *written,
						   ChunksErrorError *error, uint32 unwrap_line, uint32 unwrap_col, uint32 line, uint32 col)
{
	for (size_t k = 0, k_len = written->len; k < k_len; k++)
	{
		const chunks_TupleAddressVecU8Locator_t *w = &STD_VEC_AT(written, chunks_TupleAddressVecU8Locator_t, k);
		ChunksChunkChunk read;
		StdVec		bytes;
		StdBytes	got;
		StdBytes	want = {w->f1.ptr, w->f1.len};
		bool		same;

		if (!chunks_pack_PackReader_read(reader, HOST_IO_IO_READ_DOORS(io), w->f2, &w->f0, &read, error))
			STD_RESULT_EXPECT_FAILED(unwrap_line, unwrap_col, CHUNKS_PACK_TESTS_UNWRAP, error,
									 chunks_error_Error_Debug_fmt);
		chunks_chunk_Chunk_into_bytes(&read, &bytes);
		got.ptr = bytes.ptr;
		got.len = bytes.len;
		same = got.len == want.len && memcmp(got.ptr, want.ptr, want.len) == 0;
		if (!same)
			STD_ASSERT_EQ(line, col, false, &got, std_bytes_Debug_fmt, &want, std_bytes_Debug_fmt);
		std_vec_free_Checked(&bytes, 1, 1);
	}
}

/* The state of a dictionary test across its pad: its capability, directory, writer, chunks, pack, reader, shared dictionary, a dictionary's bytes, and an error. */
typedef struct chunks_pack_tests_dictionary_frame_0_t
{
	SimcTestIo	test_io;
	bool		test_io_live;
	SimcTempDir dir;
	bool		dir_live;
	ChunksPackPackWriterInDir writer;
	bool		writer_live;
	StdVec		written;
	bool		written_live;
	StdVec		written_2;
	bool		written_2_live;
	ChunksPackPack pack;
	bool		pack_live;
	ChunksPackPack pack_2;
	bool		pack_2_live;
	ChunksPackPackReader reader;
	bool		reader_live;
	ChunksPackSharedDictionary shared;
	bool		shared_live;
	StdVec		own;
	bool		own_live;
	StdVec		own_2;
	bool		own_2_live;
	StdVec		own_3;
	bool		own_3_live;
	ChunksErrorError error;
	bool		error_live;
} chunks_pack_tests_dictionary_frame_0_t;

/* chunks_pack_tests_dictionary_pad_0 is a dictionary test's pad: what it holds, while live. */
static void
chunks_pack_tests_dictionary_pad_0(void *frame_)
{
	chunks_pack_tests_dictionary_frame_0_t *frame = frame_;

	if (frame->error_live)
		chunks_error_Error_drop(&frame->error);
	if (frame->own_3_live)
		std_vec_free_Checked(&frame->own_3, 1, 1);
	if (frame->own_2_live)
		std_vec_free_Checked(&frame->own_2, 1, 1);
	if (frame->own_live)
		std_vec_free_Checked(&frame->own, 1, 1);
	if (frame->shared_live)
		chunks_pack_SharedDictionary_drop(frame->shared);
	if (frame->reader_live)
		chunks_pack_PackReader_drop(&frame->reader);
	if (frame->pack_2_live)
		chunks_pack_Pack_drop(&frame->pack_2);
	if (frame->pack_live)
		chunks_pack_Pack_drop(&frame->pack);
	if (frame->written_2_live)
		chunks_pack_tests_VecWritten_drop(&frame->written_2);
	if (frame->written_live)
		chunks_pack_tests_VecWritten_drop(&frame->written);
	if (frame->writer_live)
		chunks_pack_PackWriterInDir_drop(&frame->writer);
	if (frame->dir_live)
		simc_TempDir_drop(&frame->dir);
	if (frame->test_io_live)
		simc_TestIo_drop(&frame->test_io);
}

/* chunks_pack_tests_dictionary_start begins a dictionary test: its capability, its directory, and a writer of a number. */
static HostIoIo *
chunks_pack_tests_dictionary_start(chunks_pack_tests_dictionary_frame_0_t *frame, uint32 dir_line, uint32 dir_col)
{
	StdIoError	io_error;

	frame->test_io = simc_io();
	frame->test_io_live = true;
	if (!simc_tempdir(&frame->dir, &io_error))
		STD_RESULT_EXPECT_FAILED(dir_line, dir_col, CHUNKS_PACK_TESTS_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	frame->dir_live = true;
	return &frame->test_io.io;
}

/* chunks_pack_tests_dictionary_create makes the frame's writer, of a number, in its directory. */
static void
chunks_pack_tests_dictionary_create(chunks_pack_tests_dictionary_frame_0_t *frame, uint64 number, uint32 line,
									uint32 col)
{
	size_t		dir_path_len;
	const char *dir_path = simc_TempDir_path(&frame->dir, &dir_path_len);

	if (!chunks_pack_PackWriterInDir_create(&frame->test_io.io, dir_path, dir_path_len, number, &frame->writer,
											&frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(line, col, CHUNKS_PACK_TESTS_UNWRAP, &frame->error, chunks_error_Error_Debug_fmt);
	}
	frame->writer_live = true;
}

/* chunks_pack_tests_dictionary_finish finishes the frame's writer into a pack, and opens a reader of it. */
static void
chunks_pack_tests_dictionary_finish(chunks_pack_tests_dictionary_frame_0_t *frame, ChunksPackPack *pack,
									bool *pack_live, uint32 finish_line, uint32 finish_col, uint32 open_line,
									uint32 open_col)
{
	HostIoIo   *io = &frame->test_io.io;

	frame->writer_live = false;
	if (!chunks_pack_PackWriterInDir_finish(&frame->writer, HOST_IO_IO_TAKES_SHORT(io), pack, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(finish_line, finish_col, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	*pack_live = true;
	if (frame->reader_live)
	{
		frame->reader_live = false;
		chunks_pack_PackReader_drop(&frame->reader);
	}
	if (open_line == 0)
		return;
	if (!chunks_pack_PackReader_open(HOST_IO_IO_READ_DOORS(io), (const char *) pack->path.bytes.ptr,
									 pack->path.bytes.len, &frame->reader, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(open_line, open_col, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->reader_live = true;
}

/* chunks_pack_tests_assert_dictionaries asserts that a writer holds a count of dictionaries, at a line and column. */
static void
chunks_pack_tests_assert_dictionaries(const ChunksPackPackWriterInDir *writer, size_t want, uint32 line, uint32 col)
{
	size_t		got = writer->dictionaries.len;

	STD_ASSERT_EQ(line, col, got == want, &got, std_usize_Debug_fmt, &want, std_usize_Debug_fmt);
}

/* The sharing test's write: a pack sharing the dictionary or training its own, its 400 leaves, and its own dictionary's bytes. */
static void
test_chunks_pack_tests_packs_that_share_a_dictionary_take_the_one_the_first_to_collect_its_samples_trained_closure_0(chunks_pack_tests_dictionary_frame_0_t *frame,
																													 uint64 number,
																													 uint32 seed,
																													 bool share,
																													 StdVec *written,
																													 bool *written_live,
																													 ChunksPackPack *pack,
																													 bool *pack_live,
																													 StdVec *own,
																													 bool *own_live)
{
	HostIoIo   *io = &frame->test_io.io;
	StdVec		padded;
	const StdVec *dictionary;

	chunks_pack_tests_dictionary_create(frame, number, 1254, 73);
	if (share)
	{
		ChunksPackSharedDictionary shared = std_arc_clone(frame->shared);

		chunks_pack_PackWriterInDir_share(&frame->writer, &shared);
	}
	else
		chunks_pack_PackWriterInDir_train(&frame->writer);
	chunks_pack_tests_pad(io, &frame->writer, &padded);
	chunks_pack_tests_VecWritten_drop(&padded);
	std_vec_new(written, sizeof(chunks_TupleAddressVecU8Locator_t), _Alignof(chunks_TupleAddressVecU8Locator_t));
	*written_live = true;
	chunks_pack_tests_leaves(io, &frame->writer, STD_MUL(u32, seed, 1000, 1262, 55), 7, 400, written,
							 &frame->error, 1263, 97);
	std_index_check(0, frame->writer.dictionaries.len, RUST_FILE, 1265, 30);
	dictionary = &STD_VEC_AT(&frame->writer.dictionaries, chunks_TupleVecU8DecompressDictionary_t, 0).f0;
	std_slice_to_vec_Checked(own, dictionary->ptr, dictionary->len, 1, 1);
	*own_live = true;
	chunks_pack_tests_dictionary_finish(frame, pack, pack_live, 1266, 32, 0, 0);
}

/* The test's body. */
static void
test_chunks_pack_tests_packs_that_share_a_dictionary_take_the_one_the_first_to_collect_its_samples_trained_body_0(void *frame_)
{
	chunks_pack_tests_dictionary_frame_0_t *frame = frame_;
	HostIoIo   *io = chunks_pack_tests_dictionary_start(frame, 1251, 34);
	ChunksPackPack alone;
	bool		alone_live = false;
	StdVec		alone_written;
	bool		alone_written_live = false;
	StdBytes	first_own;
	StdBytes	second_own;
	StdBytes	alone_own;

	frame->shared = chunks_pack_SharedDictionary_Default_default();
	frame->shared_live = true;
	test_chunks_pack_tests_packs_that_share_a_dictionary_take_the_one_the_first_to_collect_its_samples_trained_closure_0(frame,
																														 1,
																														 0,
																														 true,
																														 &frame->written,
																														 &frame->written_live,
																														 &frame->pack,
																														 &frame->pack_live,
																														 &frame->own,
																														 &frame->own_live);
	test_chunks_pack_tests_packs_that_share_a_dictionary_take_the_one_the_first_to_collect_its_samples_trained_closure_0(frame,
																														 2,
																														 1,
																														 true,
																														 &frame->written_2,
																														 &frame->written_2_live,
																														 &frame->pack_2,
																														 &frame->pack_2_live,
																														 &frame->own_2,
																														 &frame->own_2_live);
	test_chunks_pack_tests_packs_that_share_a_dictionary_take_the_one_the_first_to_collect_its_samples_trained_closure_0(frame,
																														 3,
																														 1,
																														 false,
																														 &alone_written,
																														 &alone_written_live,
																														 &alone,
																														 &alone_live,
																														 &frame->own_3,
																														 &frame->own_3_live);
	chunks_pack_tests_VecWritten_drop(&alone_written);
	chunks_pack_Pack_drop(&alone);
	first_own.ptr = frame->own.ptr;
	first_own.len = frame->own.len;
	second_own.ptr = frame->own_2.ptr;
	second_own.len = frame->own_2.len;
	alone_own.ptr = frame->own_3.ptr;
	alone_own.len = frame->own_3.len;
	STD_ASSERT_EQ(1271, 9, second_own.len == first_own.len &&
				  memcmp(second_own.ptr, first_own.ptr, first_own.len) == 0, &second_own, std_bytes_Debug_fmt,
				  &first_own, std_bytes_Debug_fmt);
	if (alone_own.len == first_own.len && memcmp(alone_own.ptr, first_own.ptr, first_own.len) == 0)
		std_assert_failed(RUST_FILE, 1272, 9, "!=", &alone_own, std_bytes_Debug_fmt, &first_own,
						  std_bytes_Debug_fmt, NULL);
	if (!chunks_pack_PackReader_open(HOST_IO_IO_READ_DOORS(io), (const char *) frame->pack.path.bytes.ptr,
									 frame->pack.path.bytes.len, &frame->reader, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1274, 59, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->reader_live = true;
	chunks_pack_tests_read_all(&frame->reader, io, &frame->written, &frame->error, 1276, 64, 1276, 17);
	frame->reader_live = false;
	chunks_pack_PackReader_drop(&frame->reader);
	if (!chunks_pack_PackReader_open(HOST_IO_IO_READ_DOORS(io), (const char *) frame->pack_2.path.bytes.ptr,
									 frame->pack_2.path.bytes.len, &frame->reader, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1274, 59, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->reader_live = true;
	chunks_pack_tests_read_all(&frame->reader, io, &frame->written_2, &frame->error, 1276, 64,
							   1276, 17);
	chunks_pack_tests_dictionary_pad_0(frame);
}

/*
 * test_chunks_pack_tests_packs_that_share_a_dictionary_take_the_one_the_first_to_collect_its_samples_trained:
 * packs that share a dictionary all take the one the first of them to
 * collect its samples trained, which a pack training alone doesn't.
 */
void
test_chunks_pack_tests_packs_that_share_a_dictionary_take_the_one_the_first_to_collect_its_samples_trained(void)
{
	chunks_pack_tests_dictionary_frame_0_t frame = {0};

	std_landing(&frame,
				test_chunks_pack_tests_packs_that_share_a_dictionary_take_the_one_the_first_to_collect_its_samples_trained_body_0,
				chunks_pack_tests_dictionary_pad_0);
}

/* The test's body. */
static void
test_chunks_pack_tests_a_pack_that_trains_none_compresses_its_leaves_without_a_dictionary_body_0(void *frame_)
{
	chunks_pack_tests_dictionary_frame_0_t *frame = frame_;
	HostIoIo   *io = chunks_pack_tests_dictionary_start(frame, 1285, 34);

	chunks_pack_tests_dictionary_create(frame, 7, 1286, 64);
	std_vec_new(&frame->written, sizeof(chunks_TupleAddressVecU8Locator_t), _Alignof(chunks_TupleAddressVecU8Locator_t));
	frame->written_live = true;
	chunks_pack_tests_leaves(io, &frame->writer, 0, 1, 400, &frame->written, &frame->error, 1290, 84);
	STD_ASSERT(1293, 9, frame->writer.dictionaries.len == 0, "writer.dictionaries.is_empty()");
	chunks_pack_tests_dictionary_finish(frame, &frame->pack, &frame->pack_live, 1294, 38, 1295, 55);
	for (size_t k = 0, k_len = frame->written.len; k < k_len; k++)
	{
		const chunks_TupleAddressVecU8Locator_t *w = &STD_VEC_AT(&frame->written, chunks_TupleAddressVecU8Locator_t,
																 k);

		STD_ASSERT(1297, 13, w->f1.len > (size_t) chunks_trailer_Locator_length(&w->f2),
				   "stored.len() > locator.length() as usize");
	}
	chunks_pack_tests_read_all(&frame->reader, io, &frame->written, &frame->error, 1298, 60, 1298, 13);
	chunks_pack_tests_dictionary_pad_0(frame);
}

/* test_chunks_pack_tests_a_pack_that_trains_none_compresses_its_leaves_without_a_dictionary: a pack that trains no dictionary compresses its leaves alone. */
void
test_chunks_pack_tests_a_pack_that_trains_none_compresses_its_leaves_without_a_dictionary(void)
{
	chunks_pack_tests_dictionary_frame_0_t frame = {0};

	std_landing(&frame, test_chunks_pack_tests_a_pack_that_trains_none_compresses_its_leaves_without_a_dictionary_body_0,
				chunks_pack_tests_dictionary_pad_0);
}

/* The test's body. */
static void
test_chunks_pack_tests_a_pack_that_trains_takes_no_dictionary_until_it_holds_its_threshold_of_bytes_body_0(void *frame_)
{
	chunks_pack_tests_dictionary_frame_0_t *frame = frame_;
	HostIoIo   *io = chunks_pack_tests_dictionary_start(frame, 1306, 34);

	chunks_pack_tests_dictionary_create(frame, 7, 1307, 64);
	chunks_pack_PackWriterInDir_train(&frame->writer);
	std_vec_new(&frame->written, sizeof(chunks_TupleAddressVecU8Locator_t), _Alignof(chunks_TupleAddressVecU8Locator_t));
	frame->written_live = true;
	chunks_pack_tests_leaves(io, &frame->writer, 0, 1, 400, &frame->written, &frame->error, 1312, 84);
	STD_ASSERT(1315, 9, frame->writer.dictionaries.len == 0, "writer.dictionaries.is_empty()");
	chunks_pack_tests_pad(io, &frame->writer, &frame->written_2);
	frame->written_2_live = true;
	std_vec_extend_from_slice_Checked(&frame->written, frame->written_2.ptr, frame->written_2.len,
									  sizeof(chunks_TupleAddressVecU8Locator_t),
									  _Alignof(chunks_TupleAddressVecU8Locator_t));
	frame->written_2_live = false;
	std_vec_free_Checked(&frame->written_2, sizeof(chunks_TupleAddressVecU8Locator_t),
						 _Alignof(chunks_TupleAddressVecU8Locator_t));
	chunks_pack_tests_leaves(io, &frame->writer, 400, 1, 1, &frame->written, &frame->error, 1318, 89);
	chunks_pack_tests_assert_dictionaries(&frame->writer, 1, 1319, 9);
	chunks_pack_tests_dictionary_finish(frame, &frame->pack, &frame->pack_live, 1320, 38, 1321, 55);
	chunks_pack_tests_read_all(&frame->reader, io, &frame->written, &frame->error, 1323, 60, 1323, 13);
	chunks_pack_tests_dictionary_pad_0(frame);
}

/*
 * test_chunks_pack_tests_a_pack_that_trains_takes_no_dictionary_until_it_holds_its_threshold_of_bytes:
 * a pack that trains takes no dictionary until it holds TRAIN_AFTER bytes,
 * and then its next leaves train it.
 */
void
test_chunks_pack_tests_a_pack_that_trains_takes_no_dictionary_until_it_holds_its_threshold_of_bytes(void)
{
	chunks_pack_tests_dictionary_frame_0_t frame = {0};

	std_landing(&frame,
				test_chunks_pack_tests_a_pack_that_trains_takes_no_dictionary_until_it_holds_its_threshold_of_bytes_body_0,
				chunks_pack_tests_dictionary_pad_0);
}

/* The test's body. */
static void
test_chunks_pack_tests_a_pack_sharing_a_trained_dictionary_takes_it_once_it_collects_its_samples_body_0(void *frame_)
{
	chunks_pack_tests_dictionary_frame_0_t *frame = frame_;
	HostIoIo   *io = chunks_pack_tests_dictionary_start(frame, 1331, 34);
	StdBytes	slices[CHUNKS_PACK_TRAIN_LEAVES];
	bool		trained;
	chunks_OptionVecU8_t set;
	chunks_pack_SharedDictionaryValue *value;
	ChunksPackSharedDictionary shared;

	std_vec_new(&frame->written_2, sizeof(chunks_TupleAddressVecU8Locator_t),
				_Alignof(chunks_TupleAddressVecU8Locator_t));
	frame->written_2_live = true;
	for (uint32 i = 0, i_end = (uint32) CHUNKS_PACK_TRAIN_LEAVES; i < i_end; i = std_forward_unchecked_u32_Checked(i, 1))
	{
		chunks_TupleAddressVecU8_t chunk;
		chunks_TupleAddressVecU8Locator_t entry = {0};

		chunks_pack_tests_chunk(STD_ADD(u32, STD_MUL(u32, i, 7, 1332, 76), 5000, 1332, 76), &chunk);
		entry.f1 = chunk.f1;
		std_vec_push_Checked(&frame->written_2, &entry, sizeof(entry), _Alignof(chunks_TupleAddressVecU8Locator_t));
		slices[i].ptr = chunk.f1.ptr;
		slices[i].len = chunk.f1.len;
	}
	trained = chunks_pack_train_dictionary(slices, CHUNKS_PACK_TRAIN_LEAVES, &frame->own);
	frame->own_live = trained;
	STD_ASSERT(1334, 9, trained, "dictionary.is_some()");
	frame->shared = chunks_pack_SharedDictionary_Default_default();
	frame->shared_live = true;
	set.has = true;
	std_slice_to_vec_Checked(&set.value, frame->own.ptr, frame->own.len, 1, 1);
	value = std_arc_Deref_deref(frame->shared, _Alignof(chunks_pack_SharedDictionaryValue));
	if (!std_once_lock_set(&value->lock, &value->value, &set, sizeof(set)))
	{
		StdBytes	held = {set.value.ptr, set.value.len};
		StdOptionRef refused = {true, STD_ITEM(&held, std_bytes_Debug_fmt)};

		STD_RESULT_EXPECT_FAILED(1336, 40, CHUNKS_PACK_TESTS_UNWRAP, &refused, std_option_Debug_fmt);
	}
	chunks_pack_tests_dictionary_create(frame, 7, 1337, 64);
	shared = frame->shared;
	frame->shared_live = false;
	chunks_pack_PackWriterInDir_share(&frame->writer, &shared);
	std_vec_new(&frame->written, sizeof(chunks_TupleAddressVecU8Locator_t), _Alignof(chunks_TupleAddressVecU8Locator_t));
	frame->written_live = true;
	chunks_pack_tests_leaves(io, &frame->writer, 0, 1, (uint32) CHUNKS_PACK_TRAIN_LEAVES + 1, &frame->written,
							 &frame->error, 1342, 84);
	{
		StdBytes	want = {frame->own.ptr, frame->own.len};
		const StdVec *got = frame->writer.dictionaries.len == 1 ?
			&STD_VEC_AT(&frame->writer.dictionaries, chunks_TupleVecU8DecompressDictionary_t, 0).f0 : NULL;
		StdBytes	got_bytes = got != NULL ? (StdBytes) {got->ptr, got->len} : (StdBytes) {NULL, 0};

		STD_ASSERT_EQ(1345, 9, got != NULL && got->len == want.len && memcmp(got->ptr, want.ptr, want.len) == 0,
					  &got_bytes, std_bytes_Debug_fmt, &want, std_bytes_Debug_fmt);
	}
	chunks_pack_tests_dictionary_finish(frame, &frame->pack, &frame->pack_live, 1349, 38, 1350, 55);
	chunks_pack_tests_read_all(&frame->reader, io, &frame->written, &frame->error, 1352, 60, 1352, 13);
	chunks_pack_tests_dictionary_pad_0(frame);
}

/*
 * test_chunks_pack_tests_a_pack_sharing_a_trained_dictionary_takes_it_once_it_collects_its_samples:
 * a pack sharing a dictionary that is trained already takes it as its own
 * once it collects its samples.
 */
void
test_chunks_pack_tests_a_pack_sharing_a_trained_dictionary_takes_it_once_it_collects_its_samples(void)
{
	chunks_pack_tests_dictionary_frame_0_t frame = {0};

	std_landing(&frame,
				test_chunks_pack_tests_a_pack_sharing_a_trained_dictionary_takes_it_once_it_collects_its_samples_body_0,
				chunks_pack_tests_dictionary_pad_0);
}

/*
 * The state of a_scratch_pack_reads_back_its_chunks_and_goes_when_its_writer_drops
 * across its pad: its capability, writer, the chunks written, a hashed
 * body, and an unwrapped error.
 */
typedef struct test_chunks_pack_tests_a_scratch_pack_reads_back_its_chunks_and_goes_when_its_writer_drops_frame_0_t
{
	SimcTestIo	test_io;
	bool		test_io_live;
	ChunksPackPackWriterScratchFile writer;
	bool		writer_live;
	StdVec		written;
	bool		written_live;
	StdVec		bytes;
	bool		bytes_live;
	ChunksErrorError error;
	bool		error_live;
} test_chunks_pack_tests_a_scratch_pack_reads_back_its_chunks_and_goes_when_its_writer_drops_frame_0_t;

/* The test's pad. */
static void
test_chunks_pack_tests_a_scratch_pack_reads_back_its_chunks_and_goes_when_its_writer_drops_pad_0(void *frame_)
{
	test_chunks_pack_tests_a_scratch_pack_reads_back_its_chunks_and_goes_when_its_writer_drops_frame_0_t *frame = frame_;

	if (frame->error_live)
		chunks_error_Error_drop(&frame->error);
	if (frame->bytes_live)
		std_vec_free_Checked(&frame->bytes, 1, 1);
	if (frame->written_live)
		chunks_pack_tests_VecWritten_drop(&frame->written);
	if (frame->writer_live)
		chunks_pack_PackWriterScratchFile_drop(&frame->writer);
	if (frame->test_io_live)
		simc_TestIo_drop(&frame->test_io);
}

/* The test's body. */
static void
test_chunks_pack_tests_a_scratch_pack_reads_back_its_chunks_and_goes_when_its_writer_drops_body_0(void *frame_)
{
	test_chunks_pack_tests_a_scratch_pack_reads_back_its_chunks_and_goes_when_its_writer_drops_frame_0_t *frame = frame_;
	HostIoIo   *io;
	size_t		before;
	uint64		state = UINT64CONST(0x9e3779b97f4a7c15);
	static const size_t lens[] = {100, CHUNKS_PACK_FLUSH, 100};
	ChunksTrailerStamps stamps = chunks_trailer_Stamps_Default_default();

	frame->test_io = simc_io();
	frame->test_io_live = true;
	io = &frame->test_io.io;
	before = simc_scratch_files();
	if (!chunks_pack_PackWriterScratchFile_scratch(io, &frame->writer, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1113, 50, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->writer_live = true;
	{
		uint64		number = chunks_pack_PackWriterScratchFile_number(&frame->writer);
		size_t		files = simc_scratch_files();
		uint64		zero = 0;
		size_t		more = STD_ADD(usize, before, 1, 1114, 65);
		StdTupleRef got = STD_TUPLE(2, STD_ITEM(&number, std_u64_Debug_fmt), STD_ITEM(&files, std_usize_Debug_fmt));
		StdTupleRef want = STD_TUPLE(2, STD_ITEM(&zero, std_u64_Debug_fmt), STD_ITEM(&more, std_usize_Debug_fmt));

		STD_ASSERT_EQ(1114, 9, number == zero && files == more, &got, std_tuple_Debug_fmt, &want,
					  std_tuple_Debug_fmt);
	}
	std_vec_new(&frame->written, sizeof(chunks_TupleAddressVecU8Locator_t), _Alignof(chunks_TupleAddressVecU8Locator_t));
	frame->written_live = true;
	for (size_t k = 0, k_len = lengthof(lens); k < k_len; k++)
	{
		StdVec		body;
		chunks_TupleAddressVecU8_t chunk;
		chunks_TupleAddressVecU8Locator_t entry;

		std_vec_with_capacity_Checked(&body, lens[k], 1, 1);
		for (size_t j = 0, j_len = lens[k]; j < j_len; j++)
			std_vec_u8_push_Checked(&body, chunks_pack_tests_noise(&state));
		chunks_pack_tests_leaf(&body, &stamps, &chunk);
		if (!chunks_pack_PackWriterScratchFile_write(&frame->writer, io, chunk.f0, chunk.f1.ptr, chunk.f1.len, false,
													 &entry.f2, &frame->error))
		{
			std_vec_free_Checked(&chunk.f1, 1, 1);
			frame->error_live = true;
			STD_RESULT_EXPECT_FAILED(1131, 68, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
									 chunks_error_Error_Debug_fmt);
		}
		entry.f0 = chunk.f0;
		entry.f1 = chunk.f1;
		std_vec_push_Checked(&frame->written, &entry, sizeof(entry), _Alignof(chunks_TupleAddressVecU8Locator_t));
	}
	STD_ASSERT_MSG(1135, 9, frame->writer.flushed > 0, "the pack never reached its file");
	for (size_t k = 0, k_len = frame->written.len; k < k_len; k++)
	{
		const chunks_TupleAddressVecU8Locator_t *w = &STD_VEC_AT(&frame->written, chunks_TupleAddressVecU8Locator_t,
																 k);
		ChunksChunkChunk read;
		StdBytes	got;
		StdBytes	want = {w->f1.ptr, w->f1.len};

		if (!chunks_pack_PackWriterScratchFile_read(&frame->writer, io, w->f2, &w->f0, &read, &frame->error))
		{
			frame->error_live = true;
			STD_RESULT_EXPECT_FAILED(1137, 55, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
									 chunks_error_Error_Debug_fmt);
		}
		chunks_chunk_Chunk_into_bytes(&read, &frame->bytes);
		frame->bytes_live = true;
		got.ptr = frame->bytes.ptr;
		got.len = frame->bytes.len;
		STD_ASSERT_EQ(1137, 13, got.len == want.len && memcmp(got.ptr, want.ptr, want.len) == 0, &got,
					  std_bytes_Debug_fmt, &want, std_bytes_Debug_fmt);
		frame->bytes_live = false;
		std_vec_free_Checked(&frame->bytes, 1, 1);
	}
	frame->writer_live = false;
	chunks_pack_PackWriterScratchFile_drop(&frame->writer);
	{
		size_t		files = simc_scratch_files();

		STD_ASSERT_EQ(1140, 9, files == before, &files, std_usize_Debug_fmt, &before, std_usize_Debug_fmt);
	}
	test_chunks_pack_tests_a_scratch_pack_reads_back_its_chunks_and_goes_when_its_writer_drops_pad_0(frame);
}

/*
 * test_chunks_pack_tests_a_scratch_pack_reads_back_its_chunks_and_goes_when_its_writer_drops:
 * a scratch pack reads back the chunks written to it, from its buffer and
 * from its file once it grew past the buffer, and its file is gone when its
 * writer drops.
 */
void
test_chunks_pack_tests_a_scratch_pack_reads_back_its_chunks_and_goes_when_its_writer_drops(void)
{
	test_chunks_pack_tests_a_scratch_pack_reads_back_its_chunks_and_goes_when_its_writer_drops_frame_0_t frame = {0};

	std_landing(&frame, test_chunks_pack_tests_a_scratch_pack_reads_back_its_chunks_and_goes_when_its_writer_drops_body_0,
				test_chunks_pack_tests_a_scratch_pack_reads_back_its_chunks_and_goes_when_its_writer_drops_pad_0);
}

/* The OOM test's out_of_memory: a read that failed with Postgres' out of memory, SQLSTATE 53200. */
static bool
test_chunks_pack_tests_a_decompression_out_of_memory_fails_a_read_and_a_walk_with_postgres_error_and_loses_no_record_out_of_memory(bool ok,
																																   const ChunksErrorError *error)
{
	return !ok && error->tag == CHUNKS_ERROR_ERROR_HOST && memcmp(error->u.host.sqlstate, "53200", 5) == 0;
}

/* The OOM test's walk: it takes every chunk. */
static bool
test_chunks_pack_tests_a_decompression_out_of_memory_fails_a_read_and_a_walk_with_postgres_error_and_loses_no_record_closure_0(void *captures,
																															   ChunksFileInterruptible io,
																															   ChunksPackVerified *verified,
																															   ChunksErrorError *error)
{
	return true;
}

/* The OOM test's result: a read's, Ok of its bytes or Err of its error, for its Debug. */
typedef struct test_chunks_pack_tests_a_decompression_out_of_memory_result_t
{
	bool		ok;
	const void *value;
	StdFmtFn	fmt;
} test_chunks_pack_tests_a_decompression_out_of_memory_result_t;

/* The OOM test's result's Debug: Ok(..) or Err(..). */
static bool
test_chunks_pack_tests_a_decompression_out_of_memory_result_Debug_fmt(const void *value, StdFormatter *f)
{
	const test_chunks_pack_tests_a_decompression_out_of_memory_result_t *self = value;
	StdDebugTuple t = std_fmt_debug_tuple(f, self->ok ? "Ok" : "Err", self->ok ? 2 : 3);

	std_DebugTuple_field(&t, self->value, self->fmt);
	return std_DebugTuple_finish(&t);
}

/*
 * The state of a_decompression_out_of_memory_fails_a_read_and_a_walk_with_postgres_error_and_loses_no_record
 * across its pad: its capability, directory, writer, chunk, pack, reader,
 * a chunk's bytes, what a walk lost, the message, and an error.
 */
typedef struct test_chunks_pack_tests_a_decompression_out_of_memory_fails_a_read_and_a_walk_with_postgres_error_and_loses_no_record_frame_0_t
{
	SimcTestIo	test_io;
	bool		test_io_live;
	SimcTempDir dir;
	bool		dir_live;
	ChunksPackPackWriterInDir writer;
	bool		writer_live;
	chunks_TupleAddressVecU8_t chunk;
	bool		chunk_live;
	ChunksPackPack pack;
	bool		pack_live;
	ChunksPackPackReader reader;
	bool		reader_live;
	StdVec		bytes;
	bool		bytes_live;
	ChunksPackLost lost;
	bool		lost_live;
	StdString	message;
	bool		message_live;
	ChunksErrorError error;
	bool		error_live;
} test_chunks_pack_tests_a_decompression_out_of_memory_fails_a_read_and_a_walk_with_postgres_error_and_loses_no_record_frame_0_t;

/* The test's pad. */
static void
test_chunks_pack_tests_a_decompression_out_of_memory_fails_a_read_and_a_walk_with_postgres_error_and_loses_no_record_pad_0(void *frame_)
{
	test_chunks_pack_tests_a_decompression_out_of_memory_fails_a_read_and_a_walk_with_postgres_error_and_loses_no_record_frame_0_t *frame = frame_;

	if (frame->error_live)
		chunks_error_Error_drop(&frame->error);
	if (frame->message_live)
		std_vec_free_Checked(&frame->message, 1, 1);
	if (frame->lost_live)
		chunks_pack_Lost_drop(&frame->lost);
	if (frame->bytes_live)
		std_vec_free_Checked(&frame->bytes, 1, 1);
	if (frame->reader_live)
		chunks_pack_PackReader_drop(&frame->reader);
	if (frame->pack_live)
		chunks_pack_Pack_drop(&frame->pack);
	if (frame->chunk_live)
		std_vec_free_Checked(&frame->chunk.f1, 1, 1);
	if (frame->writer_live)
		chunks_pack_PackWriterInDir_drop(&frame->writer);
	if (frame->dir_live)
		simc_TempDir_drop(&frame->dir);
	if (frame->test_io_live)
		simc_TestIo_drop(&frame->test_io);
}

/* The test's body. */
static void
test_chunks_pack_tests_a_decompression_out_of_memory_fails_a_read_and_a_walk_with_postgres_error_and_loses_no_record_body_0(void *frame_)
{
	test_chunks_pack_tests_a_decompression_out_of_memory_fails_a_read_and_a_walk_with_postgres_error_and_loses_no_record_frame_0_t *frame = frame_;
	HostIoIo   *io;
	StdIoError	io_error;
	const char *dir_path;
	size_t		dir_path_len;
	ChunksTrailerLocator locator;
	ChunksChunkChunk read;
	bool		ok;
	ChunksFileInterruptible interruptible;

	frame->test_io = simc_io();
	frame->test_io_live = true;
	io = &frame->test_io.io;
	interruptible = CHUNKS_PACK_TESTS_IO_INTERRUPTIBLE(io);
	if (!simc_tempdir(&frame->dir, &io_error))
		STD_RESULT_EXPECT_FAILED(1434, 34, CHUNKS_PACK_TESTS_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	frame->dir_live = true;
	dir_path = simc_TempDir_path(&frame->dir, &dir_path_len);
	if (!chunks_pack_PackWriterInDir_create(io, dir_path, dir_path_len, 4, &frame->writer, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1435, 64, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->writer_live = true;
	chunks_pack_tests_chunk(1, &frame->chunk);
	frame->chunk_live = true;
	if (!chunks_pack_PackWriterInDir_write(&frame->writer, io, frame->chunk.f0, frame->chunk.f1.ptr,
										   frame->chunk.f1.len, false, &locator, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1437, 65, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->writer_live = false;
	if (!chunks_pack_PackWriterInDir_finish(&frame->writer, HOST_IO_IO_TAKES_SHORT(io), &frame->pack, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1438, 38, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->pack_live = true;
	if (!chunks_pack_PackReader_open(HOST_IO_IO_READ_DOORS(io), (const char *) frame->pack.path.bytes.ptr,
									 frame->pack.path.bytes.len, &frame->reader, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1439, 55, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->reader_live = true;
	simc_out_of_memory_at_next_decompress();
	ok = chunks_pack_PackReader_read(&frame->reader, HOST_IO_IO_READ_DOORS(io), locator, &frame->chunk.f0, &read,
									 &frame->error);
	if (ok)
	{
		chunks_chunk_Chunk_into_bytes(&read, &frame->bytes);
		frame->bytes_live = true;
	}
	else
		frame->error_live = true;
	if (!test_chunks_pack_tests_a_decompression_out_of_memory_fails_a_read_and_a_walk_with_postgres_error_and_loses_no_record_out_of_memory(ok, &frame->error))
	{
		StdBytes	bytes = {frame->bytes.ptr, frame->bytes.len};
		test_chunks_pack_tests_a_decompression_out_of_memory_result_t result = {ok,
			ok ? (const void *) &bytes : (const void *) &frame->error,
		ok ? std_bytes_Debug_fmt : chunks_error_Error_Debug_fmt};
		StdFmtPiece pieces[] = {STD_FMT_LIT("a read out of memory: "), STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&result, test_chunks_pack_tests_a_decompression_out_of_memory_result_Debug_fmt}};
		StdFmtArguments arguments = {pieces, 2, args};

		std_panic_fmt(RUST_FILE, 1445, 9, &arguments);
	}
	frame->error_live = false;
	chunks_error_Error_drop(&frame->error);
	simc_out_of_memory_at_next_decompress();
	ok = chunks_pack_PackReader_verified(&frame->reader, interruptible, 4,
										 test_chunks_pack_tests_a_decompression_out_of_memory_fails_a_read_and_a_walk_with_postgres_error_and_loses_no_record_closure_0,
										 NULL, NULL, &frame->lost, &frame->error);
	if (ok)
		frame->lost_live = true;
	else
		frame->error_live = true;
	if (!test_chunks_pack_tests_a_decompression_out_of_memory_fails_a_read_and_a_walk_with_postgres_error_and_loses_no_record_out_of_memory(ok, &frame->error))
	{
		test_chunks_pack_tests_a_decompression_out_of_memory_result_t result = {ok,
			ok ? (const void *) &frame->lost : (const void *) &frame->error,
		ok ? chunks_pack_Lost_Debug_fmt : chunks_error_Error_Debug_fmt};
		StdFmtPiece pieces[] = {STD_FMT_LIT("a walk out of memory: "), STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&result, test_chunks_pack_tests_a_decompression_out_of_memory_result_Debug_fmt}};
		StdFmtArguments arguments = {pieces, 2, args};

		std_panic_fmt(RUST_FILE, 1448, 9, &arguments);
	}
	frame->error_live = false;
	chunks_error_Error_drop(&frame->error);
	if (!chunks_pack_PackReader_read(&frame->reader, HOST_IO_IO_READ_DOORS(io), locator, &frame->chunk.f0, &read,
									 &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1449, 55, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	chunks_chunk_Chunk_into_bytes(&read, &frame->bytes);
	frame->bytes_live = true;
	{
		StdBytes	got = {frame->bytes.ptr, frame->bytes.len};
		StdBytes	want = {frame->chunk.f1.ptr, frame->chunk.f1.len};

		STD_ASSERT_EQ(1449, 9, got.len == want.len && memcmp(got.ptr, want.ptr, want.len) == 0, &got,
					  std_bytes_Debug_fmt, &want, std_bytes_Debug_fmt);
	}
	if (!chunks_pack_PackReader_verified(&frame->reader, interruptible, 4,
										 test_chunks_pack_tests_a_decompression_out_of_memory_fails_a_read_and_a_walk_with_postgres_error_and_loses_no_record_closure_0,
										 NULL, NULL, &frame->lost, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1450, 55, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->lost_live = true;
	if (chunks_pack_Lost_message(&frame->lost, (const char *) frame->pack.path.bytes.ptr, frame->pack.path.bytes.len,
								 &frame->message))
	{
		frame->message_live = true;
		std_panic(RUST_FILE, 1450, 9,
				  "assertion failed: reader.verified(io, 4, |_, _| Ok(())).unwrap().message(&pack.path).is_none()",
				  strlen("assertion failed: reader.verified(io, 4, |_, _| Ok(())).unwrap().message(&pack.path).is_none()"));
	}
	test_chunks_pack_tests_a_decompression_out_of_memory_fails_a_read_and_a_walk_with_postgres_error_and_loses_no_record_pad_0(frame);
}

/*
 * test_chunks_pack_tests_a_decompression_out_of_memory_fails_a_read_and_a_walk_with_postgres_error_and_loses_no_record:
 * a decompression out of memory fails a read and a walk with Postgres' out
 * of memory, and loses no record, which both read afterwards.
 */
void
test_chunks_pack_tests_a_decompression_out_of_memory_fails_a_read_and_a_walk_with_postgres_error_and_loses_no_record(void)
{
	test_chunks_pack_tests_a_decompression_out_of_memory_fails_a_read_and_a_walk_with_postgres_error_and_loses_no_record_frame_0_t frame = {0};

	std_landing(&frame,
				test_chunks_pack_tests_a_decompression_out_of_memory_fails_a_read_and_a_walk_with_postgres_error_and_loses_no_record_body_0,
				test_chunks_pack_tests_a_decompression_out_of_memory_fails_a_read_and_a_walk_with_postgres_error_and_loses_no_record_pad_0);
}

/*
 * chunks_pack_tests_trained_pack writes a pack padded to TRAIN_AFTER bytes
 * and then 400 leaves whose bodies vary with a seed, which trains its own
 * dictionary, returning the pack and the leaves as written.
 */
static void
chunks_pack_tests_trained_pack(chunks_pack_tests_dictionary_frame_0_t *frame, uint64 number, uint32 seed,
							   ChunksPackPack *pack, bool *pack_live, StdVec *written, bool *written_live)
{
	HostIoIo   *io = &frame->test_io.io;
	StdVec		padded;

	chunks_pack_tests_dictionary_create(frame, number, 1164, 62);
	chunks_pack_PackWriterInDir_train(&frame->writer);
	chunks_pack_tests_pad(io, &frame->writer, &padded);
	chunks_pack_tests_VecWritten_drop(&padded);
	std_vec_new(written, sizeof(chunks_TupleAddressVecU8Locator_t), _Alignof(chunks_TupleAddressVecU8Locator_t));
	*written_live = true;
	chunks_pack_tests_leaves(io, &frame->writer, STD_MUL(u32, seed, 1000, 1169, 51), 7, 400, written,
							 &frame->error, 1170, 68);
	chunks_pack_tests_assert_dictionaries(&frame->writer, 1, 1173, 9);
	chunks_pack_tests_dictionary_finish(frame, pack, pack_live, 1174, 28, 0, 0);
}

/* chunks_pack_tests_copy copies a record read raw from a reader into the frame's writer, with its dictionary, if any. */
static bool
chunks_pack_tests_copy(chunks_pack_tests_dictionary_frame_0_t *frame, const ChunksPackPackReader *reader,
					   ChunksAddressAddress address, ChunksTrailerLocator locator, bool slot_needed,
					   ChunksTrailerLocator *copy, uint32 raw_line, uint32 raw_col, uint32 dictionary_line,
					   uint32 dictionary_col, uint32 slot_line, uint32 slot_col, uint32 copy_line, uint32 copy_col)
{
	HostIoIo   *io = &frame->test_io.io;
	ChunksPackRaw raw;
	StdVec		dictionary;
	bool		has_dictionary = false;
	bool		has;

	if (!chunks_pack_PackReader_raw(reader, io, locator, &address, &raw, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(raw_line, raw_col, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	if (slot_needed && !CHUNKS_PACK_RAW_LAYOUT(&raw)->has_slot)
		STD_OPTION_UNWRAP_FAILED(slot_line, slot_col);
	if (CHUNKS_PACK_RAW_LAYOUT(&raw)->has_slot)
	{
		if (!chunks_pack_PackReader_dictionary(reader, io, CHUNKS_PACK_RAW_LAYOUT(&raw)->slot, &dictionary,
											   &frame->error))
		{
			frame->error_live = true;
			STD_RESULT_EXPECT_FAILED(dictionary_line, dictionary_col, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
									 chunks_error_Error_Debug_fmt);
		}
		has_dictionary = true;
	}
	if (!chunks_pack_PackWriterInDir_copy(&frame->writer, io, address, &raw, has_dictionary ? dictionary.ptr : NULL,
										  has_dictionary ? dictionary.len : 0, &has, copy, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(copy_line, copy_col, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	if (has_dictionary)
		std_vec_free_Checked(&dictionary, 1, 1);
	chunks_pack_Raw_drop(&raw);
	return has;
}

/* The test's body. */
static void
test_chunks_pack_tests_copies_keep_their_compressed_bytes_with_the_dictionaries_they_name_one_slot_each_body_0(void *frame_)
{
	chunks_pack_tests_dictionary_frame_0_t *frame = frame_;
	HostIoIo   *io = chunks_pack_tests_dictionary_start(frame, 1181, 34);
	ChunksPackPack sources[3];
	bool		sources_live[3] = {0};
	StdVec		written[3];
	bool		written_live[3] = {0};
	static const size_t order[] = {0, 1, 2, 0};

	for (uint32 seed = 0, seed_end = 3; seed < seed_end; seed = std_forward_unchecked_u32_Checked(seed, 1))
		chunks_pack_tests_trained_pack(frame, STD_ADD(u64, (uint64) seed, 1, 1182, 78), seed,
									   &sources[seed], &sources_live[seed], &written[seed], &written_live[seed]);
	chunks_pack_tests_dictionary_create(frame, 9, 1183, 64);
	std_vec_new(&frame->written, sizeof(chunks_TupleAddressVecU8Locator_t), _Alignof(chunks_TupleAddressVecU8Locator_t));
	frame->written_live = true;
	for (size_t k = 0, k_len = lengthof(order); k < k_len; k++)
	{
		const ChunksPackPack *pack = &sources[order[k]];
		const StdVec *source = &written[order[k]];

		if (frame->reader_live)
		{
			frame->reader_live = false;
			chunks_pack_PackReader_drop(&frame->reader);
		}
		if (!chunks_pack_PackReader_open(HOST_IO_IO_READ_DOORS(io), (const char *) pack->path.bytes.ptr,
										 pack->path.bytes.len, &frame->reader, &frame->error))
		{
			frame->error_live = true;
			STD_RESULT_EXPECT_FAILED(1186, 59, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
									 chunks_error_Error_Debug_fmt);
		}
		frame->reader_live = true;
		for (size_t j = 0, j_len = std_saturating_sub_usize(source->len, 200); j < j_len; j++)
		{
			const chunks_TupleAddressVecU8Locator_t *w = &STD_VEC_AT(source, chunks_TupleAddressVecU8Locator_t,
																	 std_wrapping_add_usize(200, j));
			chunks_TupleAddressVecU8Locator_t entry;
			uint32		got;
			uint32		want = chunks_trailer_Locator_length(&w->f2);

			if (!chunks_pack_tests_copy(frame, &frame->reader, w->f0, w->f2, false, &entry.f2, 1188, 61,
										1189, 82, 0, 0, 1190, 83))
				STD_OPTION_UNWRAP_FAILED(1190, 92);
			got = chunks_trailer_Locator_length(&entry.f2);
			STD_ASSERT_EQ(1191, 17, got == want, &got, std_u32_Debug_fmt, &want, std_u32_Debug_fmt);
			entry.f0 = w->f0;
			std_slice_to_vec_Checked(&entry.f1, w->f1.ptr, w->f1.len, 1, 1);
			std_vec_push_Checked(&frame->written, &entry, sizeof(entry), _Alignof(chunks_TupleAddressVecU8Locator_t));
		}
	}
	chunks_pack_tests_assert_dictionaries(&frame->writer, 3, 1195, 9);
	chunks_pack_tests_dictionary_finish(frame, &frame->pack, &frame->pack_live, 1196, 38, 1197, 55);
	chunks_pack_tests_read_all(&frame->reader, io, &frame->written, &frame->error, 1199, 60, 1199, 13);
	frame->reader_live = false;
	chunks_pack_PackReader_drop(&frame->reader);
	if (!chunks_pack_PackReader_open(HOST_IO_IO_READ_DOORS(io), (const char *) frame->pack.path.bytes.ptr,
									 frame->pack.path.bytes.len, &frame->reader, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1202, 57, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->reader_live = true;
	std_index_check(0, frame->written.len, RUST_FILE, 1203, 39);
	{
		StdVec		first = {frame->written.ptr, 1, 1};

		chunks_pack_tests_read_all(&frame->reader, io, &first, &frame->error, 1203, 66, 1203, 9);
	}
	for (size_t k = 0, k_len = 3; k < k_len; k++)
	{
		if (written_live[k])
			chunks_pack_tests_VecWritten_drop(&written[k]);
		if (sources_live[k])
			chunks_pack_Pack_drop(&sources[k]);
	}
	chunks_pack_tests_dictionary_pad_0(frame);
}

/*
 * test_chunks_pack_tests_copies_keep_their_compressed_bytes_with_the_dictionaries_they_name_one_slot_each:
 * copies keep their records' compressed bytes and carry the dictionaries
 * they were compressed with, one slot each however many records name it.
 */
void
test_chunks_pack_tests_copies_keep_their_compressed_bytes_with_the_dictionaries_they_name_one_slot_each(void)
{
	chunks_pack_tests_dictionary_frame_0_t frame = {0};

	std_landing(&frame,
				test_chunks_pack_tests_copies_keep_their_compressed_bytes_with_the_dictionaries_they_name_one_slot_each_body_0,
				chunks_pack_tests_dictionary_pad_0);
}

/* The test's body. */
static void
test_chunks_pack_tests_a_pack_carries_at_most_its_bound_of_dictionaries_keeping_a_slot_for_its_own_body_0(void *frame_)
{
	chunks_pack_tests_dictionary_frame_0_t *frame = frame_;
	HostIoIo   *io = chunks_pack_tests_dictionary_start(frame, 1210, 34);
	ChunksPackPackWriterInDir target;

	chunks_pack_tests_dictionary_create(frame, 20, 1211, 65);
	target = frame->writer;
	frame->writer_live = false;
	for (uint32 seed = 0, seed_end = (uint32) CHUNKS_PACK_MAX_DICTIONARIES; seed < seed_end;
		 seed = std_forward_unchecked_u32_Checked(seed, 1))
	{
		const chunks_TupleAddressVecU8Locator_t *w;
		ChunksTrailerLocator copy;
		bool		copied;
		bool		want = (size_t) seed < CHUNKS_PACK_MAX_DICTIONARIES - 1;

		if (frame->pack_live)
		{
			frame->pack_live = false;
			chunks_pack_Pack_drop(&frame->pack);
		}
		if (frame->written_live)
		{
			frame->written_live = false;
			chunks_pack_tests_VecWritten_drop(&frame->written);
		}
		chunks_pack_tests_trained_pack(frame, STD_ADD(u64, (uint64) seed, 1, 1213, 64), seed,
									   &frame->pack, &frame->pack_live, &frame->written, &frame->written_live);
		if (frame->reader_live)
		{
			frame->reader_live = false;
			chunks_pack_PackReader_drop(&frame->reader);
		}
		if (!chunks_pack_PackReader_open(HOST_IO_IO_READ_DOORS(io), (const char *) frame->pack.path.bytes.ptr,
										 frame->pack.path.bytes.len, &frame->reader, &frame->error))
		{
			frame->error_live = true;
			STD_RESULT_EXPECT_FAILED(1214, 59, CHUNKS_PACK_TESTS_UNWRAP, &frame->error,
									 chunks_error_Error_Debug_fmt);
		}
		frame->reader_live = true;
		std_index_check(300, frame->written.len, RUST_FILE, 1215, 41);
		w = &STD_VEC_AT(&frame->written, chunks_TupleAddressVecU8Locator_t, 300);
		frame->writer = target;
		copied = chunks_pack_tests_copy(frame, &frame->reader, w->f0, w->f2, true, &copy, 1216, 57,
										1217, 71, 1217, 61, 1218, 76);
		target = frame->writer;
		STD_ASSERT_EQ(1219, 13, copied == want, &copied, std_bool_Debug_fmt, &want, std_bool_Debug_fmt);
	}
	frame->writer = target;
	frame->writer_live = true;
	chunks_pack_tests_dictionary_pad_0(frame);
}

/*
 * test_chunks_pack_tests_a_pack_carries_at_most_its_bound_of_dictionaries_keeping_a_slot_for_its_own:
 * a pack carries at most MAX_DICTIONARIES - 1 other packs' dictionaries,
 * keeping a slot for its own, and refuses a copy that would take another.
 */
void
test_chunks_pack_tests_a_pack_carries_at_most_its_bound_of_dictionaries_keeping_a_slot_for_its_own(void)
{
	chunks_pack_tests_dictionary_frame_0_t frame = {0};

	std_landing(&frame,
				test_chunks_pack_tests_a_pack_carries_at_most_its_bound_of_dictionaries_keeping_a_slot_for_its_own_body_0,
				chunks_pack_tests_dictionary_pad_0);
}
#endif							/* DOLTGRES_TEST_CHUNKS */
